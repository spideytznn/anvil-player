#include "AnvilPlayer/App/libass_subtitle_renderer.h"

#include "AnvilPlayer/App/string_util.h"

#include <ass/ass.h>
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>

namespace anvil::app {

using anvil::playback::LogLevel;

namespace {

constexpr std::chrono::milliseconds kStreamingEventPruneDelay{30000};
constexpr std::chrono::milliseconds kFallbackAssPacketDuration{4000};
constexpr std::size_t kAssCoalesceMinImages = 8;
constexpr uint64_t kAssCoalesceMaxPixels = 2500000;
constexpr uint64_t kAssCoalesceMaxExpansionFactor = 4;
constexpr uint64_t kSlowAssRenderLogThresholdUs = 8000;

std::filesystem::path ExeDirectory() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) {
        return {};
    }
    std::filesystem::path path(buffer);
    path.remove_filename();
    return path;
}

bool PathExists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

std::filesystem::path CurrentDirectory() {
    std::error_code error;
    return std::filesystem::current_path(error);
}

std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    return error ? path : absolute;
}

std::wstring LastWin32ErrorString() {
    const DWORD error = GetLastError();
    if (error == 0) {
        return L"0";
    }
    return std::to_wstring(error);
}

uint8_t PremultiplyAssChannel(const uint8_t value, const uint8_t alpha) {
    return static_cast<uint8_t>((static_cast<unsigned int>(value) * alpha + 127) / 255);
}

uint8_t BlendPremultipliedChannel(const uint8_t source, const uint8_t destination, const uint8_t inverseAlpha) {
    return static_cast<uint8_t>(source + (static_cast<unsigned int>(destination) * inverseAlpha + 127) / 255);
}

void BlendPremultipliedPixel(uint8_t* destination,
                             const uint8_t blue,
                             const uint8_t green,
                             const uint8_t red,
                             const uint8_t alpha) {
    if (!destination || alpha == 0) {
        return;
    }

    const uint8_t inverseAlpha = static_cast<uint8_t>(255 - alpha);
    destination[0] = BlendPremultipliedChannel(blue, destination[0], inverseAlpha);
    destination[1] = BlendPremultipliedChannel(green, destination[1], inverseAlpha);
    destination[2] = BlendPremultipliedChannel(red, destination[2], inverseAlpha);
    destination[3] = BlendPremultipliedChannel(alpha, destination[3], inverseAlpha);
}

}  // namespace

struct LibassSubtitleRenderer::Impl {
    struct Api {
        int (*library_version)() = nullptr;
        ASS_Library* (*library_init)() = nullptr;
        void (*library_done)(ASS_Library*) = nullptr;
        void (*set_message_cb)(ASS_Library*, void (*)(int, const char*, va_list, void*), void*) = nullptr;
        void (*set_extract_fonts)(ASS_Library*, int) = nullptr;
        void (*add_font)(ASS_Library*, const char*, const char*, int) = nullptr;
        ASS_Renderer* (*renderer_init)(ASS_Library*) = nullptr;
        void (*renderer_done)(ASS_Renderer*) = nullptr;
        void (*set_frame_size)(ASS_Renderer*, int, int) = nullptr;
        void (*set_storage_size)(ASS_Renderer*, int, int) = nullptr;
        void (*set_pixel_aspect)(ASS_Renderer*, double) = nullptr;
        void (*set_fonts)(ASS_Renderer*, const char*, const char*, int, const char*, int) = nullptr;
        void (*set_hinting)(ASS_Renderer*, ASS_Hinting) = nullptr;
        void (*set_shaper)(ASS_Renderer*, ASS_ShapingLevel) = nullptr;
        void (*set_cache_limits)(ASS_Renderer*, int, int) = nullptr;
        ASS_Track* (*new_track)(ASS_Library*) = nullptr;
        void (*free_track)(ASS_Track*) = nullptr;
        void (*process_codec_private)(ASS_Track*, const char*, int) = nullptr;
        void (*process_data)(ASS_Track*, const char*, int) = nullptr;
        ASS_Track* (*read_memory)(ASS_Library*, char*, size_t, const char*) = nullptr;
        void (*process_chunk)(ASS_Track*, const char*, int, long long, long long) = nullptr;
        void (*set_check_readorder)(ASS_Track*, int) = nullptr;
        void (*configure_prune)(ASS_Track*, long long) = nullptr;
        void (*flush_events)(ASS_Track*) = nullptr;
        ASS_Image* (*render_frame)(ASS_Renderer*, ASS_Track*, long long, int*) = nullptr;
    };

    explicit Impl(LogSinkPtr sink) : logSink(std::move(sink)) {}

    ~Impl() {
        Release();
    }

    bool Initialize(const std::filesystem::path& searchRoot) {
        if (IsAvailable()) {
            return true;
        }

        if (!LoadLibraryModule(searchRoot) || !LoadApi()) {
            Release();
            return false;
        }

        library = api.library_init();
        if (!library) {
            Log(LogLevel::Warning, L"subtitle", L"libass_init_failed");
            Release();
            return false;
        }
        api.set_message_cb(library, &Impl::AssMessageCallback, this);
        api.set_extract_fonts(library, 1);

        renderer = api.renderer_init(library);
        if (!renderer) {
            Log(LogLevel::Warning, L"subtitle", L"libass_renderer_init_failed");
            Release();
            return false;
        }

        const std::string defaultFont = WideToUtf8(DefaultFontPath().wstring());
        api.set_fonts(renderer,
                      defaultFont.empty() ? nullptr : defaultFont.c_str(),
                      "Arial",
                      ASS_FONTPROVIDER_DIRECTWRITE,
                      nullptr,
                      0);
        api.set_hinting(renderer, ASS_HINTING_NONE);
        api.set_shaper(renderer, ASS_SHAPING_COMPLEX);
        api.set_cache_limits(renderer, 12000, 128);

        const int version = api.library_version ? api.library_version() : 0;
        Log(LogLevel::Info,
            L"subtitle",
            L"libass active version=0x" + HexVersion(version) + L" dll=" + loadedPath.wstring());
        return true;
    }

    bool IsAvailable() const {
        return dll && library && renderer;
    }

    void ResetTrack() {
        if (track && api.free_track) {
            api.free_track(track);
        }
        track = nullptr;
        ResetRenderCache();
    }

    void FlushEvents() {
        if (track && api.flush_events) {
            api.flush_events(track);
        }
        ResetRenderCache();
    }

    void ResetRenderCache() {
        cachedBitmaps.clear();
        cacheValid = false;
        lastFrameWidth = 0;
        lastFrameHeight = 0;
        lastRenderedPts = std::chrono::milliseconds{0};
    }

    bool AddFont(const std::string& name, const uint8_t* data, const int size) {
        if (!IsAvailable() || !data || size <= 0) {
            return false;
        }
        const std::string fallbackName = name.empty() ? "attachment-font" : name;
        api.add_font(library,
                     fallbackName.c_str(),
                     reinterpret_cast<const char*>(data),
                     size);
        return true;
    }

    bool ConfigureTrackFromCodecPrivate(const uint8_t* data, const int size) {
        if (!IsAvailable()) {
            return false;
        }
        ResetTrack();
        track = api.new_track(library);
        if (!track) {
            return false;
        }
        if (data && size > 0) {
            api.process_codec_private(track, reinterpret_cast<const char*>(data), size);
        }
        if (api.set_check_readorder) {
            api.set_check_readorder(track, 1);
        }
        if (api.configure_prune) {
            api.configure_prune(track, kStreamingEventPruneDelay.count());
        }
        ResetRenderCache();
        return true;
    }

    bool ConfigureTrackFromMemory(const uint8_t* data, const std::size_t size) {
        if (!IsAvailable() || !data || size == 0 || size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        ResetTrack();
        if (api.read_memory) {
            std::vector<char> copy(reinterpret_cast<const char*>(data),
                                   reinterpret_cast<const char*>(data) + size);
            track = api.read_memory(library, copy.data(), copy.size(), nullptr);
        }
        if (!track) {
            track = api.new_track(library);
            if (track) {
                api.process_data(track, reinterpret_cast<const char*>(data), static_cast<int>(size));
            }
        }
        ResetRenderCache();
        return track != nullptr;
    }

    bool ProcessPacket(const uint8_t* data,
                       const int size,
                       const std::chrono::milliseconds pts,
                       std::chrono::milliseconds duration) {
        if (!IsAvailable() || !track || !data || size <= 0) {
            return false;
        }
        if (duration <= std::chrono::milliseconds{0}) {
            duration = kFallbackAssPacketDuration;
        }
        api.process_chunk(track,
                          reinterpret_cast<const char*>(data),
                          size,
                          pts.count(),
                          duration.count());
        return true;
    }

    std::vector<NativeSubtitleBitmap> Render(const std::chrono::milliseconds pts,
                                             const int frameWidth,
                                             const int frameHeight,
                                             uint64_t& serial) {
        if (!IsAvailable() || !track || frameWidth <= 0 || frameHeight <= 0) {
            return {};
        }

        if (frameWidth != configuredFrameWidth || frameHeight != configuredFrameHeight) {
            api.set_frame_size(renderer, frameWidth, frameHeight);
            api.set_storage_size(renderer, frameWidth, frameHeight);
            api.set_pixel_aspect(renderer, 1.0);
            configuredFrameWidth = frameWidth;
            configuredFrameHeight = frameHeight;
            ResetRenderCache();
        }

        if (cacheValid &&
            pts == lastRenderedPts &&
            frameWidth == lastFrameWidth &&
            frameHeight == lastFrameHeight) {
            return cachedBitmaps;
        }

        const auto renderStart = std::chrono::steady_clock::now();
        auto maybeLogRenderCost = [&](const std::size_t sourceImages,
                                      const std::size_t outputImages,
                                      const bool coalesced,
                                      const bool reusedCache) {
            if (slowRenderLogCount >= 12) {
                return;
            }
            const auto elapsedUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - renderStart).count());
            if (elapsedUs < kSlowAssRenderLogThresholdUs) {
                return;
            }
            ++slowRenderLogCount;
            Log(LogLevel::Debug,
                L"subtitle",
                L"libass_render_slow pts_ms=" + std::to_wstring(pts.count()) +
                    L" us=" + std::to_wstring(elapsedUs) +
                    L" source_rects=" + std::to_wstring(sourceImages) +
                    L" output_rects=" + std::to_wstring(outputImages) +
                    L" coalesced=" + std::wstring(coalesced ? L"true" : L"false") +
                    L" cache=" + std::wstring(reusedCache ? L"hit_after_render" : L"miss"));
        };

        int detectChange = 0;
        ASS_Image* image = api.render_frame(renderer, track, pts.count(), &detectChange);
        const bool monotonic = !cacheValid || pts >= lastRenderedPts;
        if (detectChange == 0 &&
            cacheValid &&
            monotonic &&
            frameWidth == lastFrameWidth &&
            frameHeight == lastFrameHeight) {
            lastRenderedPts = pts;
            maybeLogRenderCost(cachedBitmaps.size(), cachedBitmaps.size(), false, true);
            return cachedBitmaps;
        }

        std::vector<ASS_Image*> images;
        int unionLeft = 0;
        int unionTop = 0;
        int unionRight = 0;
        int unionBottom = 0;
        uint64_t sourcePixels = 0;
        for (ASS_Image* current = image; current; current = current->next) {
            if (!current->bitmap || current->w <= 0 || current->h <= 0 || current->stride <= 0) {
                continue;
            }
            if (images.empty()) {
                unionLeft = current->dst_x;
                unionTop = current->dst_y;
                unionRight = current->dst_x + current->w;
                unionBottom = current->dst_y + current->h;
            } else {
                unionLeft = std::min(unionLeft, current->dst_x);
                unionTop = std::min(unionTop, current->dst_y);
                unionRight = std::max(unionRight, current->dst_x + current->w);
                unionBottom = std::max(unionBottom, current->dst_y + current->h);
            }
            sourcePixels += static_cast<uint64_t>(current->w) * static_cast<uint64_t>(current->h);
            images.push_back(current);
        }

        std::vector<NativeSubtitleBitmap> result;
        const int unionWidth = std::max(0, unionRight - unionLeft);
        const int unionHeight = std::max(0, unionBottom - unionTop);
        const uint64_t unionPixels = static_cast<uint64_t>(unionWidth) * static_cast<uint64_t>(unionHeight);
        const bool coalesceImages =
            images.size() >= kAssCoalesceMinImages &&
            unionWidth > 0 &&
            unionHeight > 0 &&
            unionPixels <= kAssCoalesceMaxPixels &&
            unionPixels <= std::max<uint64_t>(sourcePixels, 1) * kAssCoalesceMaxExpansionFactor;

        if (coalesceImages) {
            const int stride = unionWidth * 4;
            auto pixels = std::make_shared<std::vector<uint8_t>>(
                static_cast<std::size_t>(stride) * static_cast<std::size_t>(unionHeight),
                uint8_t{0});

            for (ASS_Image* current : images) {
                const uint8_t red = static_cast<uint8_t>((current->color >> 24) & 0xff);
                const uint8_t green = static_cast<uint8_t>((current->color >> 16) & 0xff);
                const uint8_t blue = static_cast<uint8_t>((current->color >> 8) & 0xff);
                const uint8_t colorAlpha = static_cast<uint8_t>(255 - (current->color & 0xff));
                if (colorAlpha == 0) {
                    continue;
                }

                const int destX = current->dst_x - unionLeft;
                const int destY = current->dst_y - unionTop;
                for (int y = 0; y < current->h; ++y) {
                    const uint8_t* sourceRow = current->bitmap + static_cast<std::size_t>(current->stride) * y;
                    uint8_t* destinationRow = pixels->data() +
                        static_cast<std::size_t>(destY + y) * static_cast<std::size_t>(stride) +
                        static_cast<std::size_t>(destX) * 4;
                    for (int x = 0; x < current->w; ++x) {
                        const uint8_t mask = sourceRow[x];
                        if (mask == 0) {
                            continue;
                        }
                        const uint8_t alpha = static_cast<uint8_t>(
                            (static_cast<unsigned int>(mask) * colorAlpha + 127) / 255);
                        BlendPremultipliedPixel(destinationRow + static_cast<std::size_t>(x) * 4,
                                                PremultiplyAssChannel(blue, alpha),
                                                PremultiplyAssChannel(green, alpha),
                                                PremultiplyAssChannel(red, alpha),
                                                alpha);
                    }
                }
            }

            NativeSubtitleBitmap bitmap;
            bitmap.x = unionLeft;
            bitmap.y = unionTop;
            bitmap.width = unionWidth;
            bitmap.height = unionHeight;
            bitmap.canvasWidth = frameWidth;
            bitmap.canvasHeight = frameHeight;
            bitmap.stride = stride;
            bitmap.serial = ++serial;
            bitmap.bgra = std::move(pixels);
            result.push_back(std::move(bitmap));

            if (!coalescedRenderLogged) {
                coalescedRenderLogged = true;
                Log(LogLevel::Debug,
                    L"subtitle",
                    L"libass_coalesce active source_rects=" + std::to_wstring(images.size()) +
                        L" output_rects=1 union=" + std::to_wstring(unionWidth) + L"x" +
                        std::to_wstring(unionHeight) +
                        L" source_pixels=" + std::to_wstring(sourcePixels));
            }
        } else {
            for (ASS_Image* current : images) {
                const int stride = current->w * 4;
                auto pixels = std::make_shared<std::vector<uint8_t>>(
                    static_cast<std::size_t>(stride) * static_cast<std::size_t>(current->h));

                const uint8_t red = static_cast<uint8_t>((current->color >> 24) & 0xff);
                const uint8_t green = static_cast<uint8_t>((current->color >> 16) & 0xff);
                const uint8_t blue = static_cast<uint8_t>((current->color >> 8) & 0xff);
                const uint8_t colorAlpha = static_cast<uint8_t>(255 - (current->color & 0xff));

                for (int y = 0; y < current->h; ++y) {
                    const uint8_t* sourceRow = current->bitmap + static_cast<std::size_t>(current->stride) * y;
                    uint8_t* destinationRow = pixels->data() + static_cast<std::size_t>(stride) * y;
                    for (int x = 0; x < current->w; ++x) {
                        const uint8_t mask = sourceRow[x];
                        if (mask == 0 || colorAlpha == 0) {
                            continue;
                        }
                        const uint8_t alpha = static_cast<uint8_t>(
                            (static_cast<unsigned int>(mask) * colorAlpha + 127) / 255);
                        uint8_t* destination = destinationRow + static_cast<std::size_t>(x) * 4;
                        destination[0] = PremultiplyAssChannel(blue, alpha);
                        destination[1] = PremultiplyAssChannel(green, alpha);
                        destination[2] = PremultiplyAssChannel(red, alpha);
                        destination[3] = alpha;
                    }
                }

                NativeSubtitleBitmap bitmap;
                bitmap.x = current->dst_x;
                bitmap.y = current->dst_y;
                bitmap.width = current->w;
                bitmap.height = current->h;
                bitmap.canvasWidth = frameWidth;
                bitmap.canvasHeight = frameHeight;
                bitmap.stride = stride;
                bitmap.serial = ++serial;
                bitmap.bgra = std::move(pixels);
                result.push_back(std::move(bitmap));
            }
        }

        cachedBitmaps = result;
        cacheValid = true;
        lastFrameWidth = frameWidth;
        lastFrameHeight = frameHeight;
        lastRenderedPts = pts;
        maybeLogRenderCost(images.size(), cachedBitmaps.size(), coalesceImages, false);
        return result;
    }

    void Release() {
        ResetTrack();
        if (renderer && api.renderer_done) {
            api.renderer_done(renderer);
        }
        renderer = nullptr;
        if (library && api.library_done) {
            api.library_done(library);
        }
        library = nullptr;
        if (dll) {
            FreeLibrary(dll);
        }
        dll = nullptr;
        api = {};
        loadedPath.clear();
    }

    bool LoadLibraryModule(const std::filesystem::path& searchRoot) {
        std::vector<std::filesystem::path> candidates;
        const auto exeDir = ExeDirectory();
        if (!exeDir.empty()) {
            candidates.push_back(exeDir / L"libass-9.dll");
        }
        const auto currentDir = CurrentDirectory();
        if (!currentDir.empty()) {
            candidates.push_back(currentDir / L"libass-9.dll");
            candidates.push_back(currentDir / L"third_party" / L"libass" / L"bin" / L"libass-9.dll");
        }
        if (!searchRoot.empty()) {
            candidates.push_back(searchRoot / L"libass-9.dll");
            candidates.push_back(searchRoot / L"third_party" / L"libass" / L"bin" / L"libass-9.dll");
        }

        for (const auto& candidate : candidates) {
            const auto absolute = AbsolutePath(candidate);
            if (!PathExists(absolute)) {
                continue;
            }
            dll = LoadLibraryExW(absolute.c_str(),
                                 nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!dll) {
                dll = LoadLibraryW(absolute.c_str());
            }
            if (dll) {
                loadedPath = absolute;
                return true;
            }
        }

        dll = LoadLibraryW(L"libass-9.dll");
        if (dll) {
            loadedPath = L"libass-9.dll";
            return true;
        }

        Log(LogLevel::Warning, L"subtitle", L"libass_load_failed error=" + LastWin32ErrorString());
        return false;
    }

    template <typename T>
    bool LoadProc(T& target, const char* name) {
        target = reinterpret_cast<T>(GetProcAddress(dll, name));
        if (!target) {
            Log(LogLevel::Warning, L"subtitle", L"libass_proc_missing name=" + Utf8ToWide(name));
            return false;
        }
        return true;
    }

    bool LoadApi() {
        bool ok = true;
        ok = LoadProc(api.library_version, "ass_library_version") && ok;
        ok = LoadProc(api.library_init, "ass_library_init") && ok;
        ok = LoadProc(api.library_done, "ass_library_done") && ok;
        ok = LoadProc(api.set_message_cb, "ass_set_message_cb") && ok;
        ok = LoadProc(api.set_extract_fonts, "ass_set_extract_fonts") && ok;
        ok = LoadProc(api.add_font, "ass_add_font") && ok;
        ok = LoadProc(api.renderer_init, "ass_renderer_init") && ok;
        ok = LoadProc(api.renderer_done, "ass_renderer_done") && ok;
        ok = LoadProc(api.set_frame_size, "ass_set_frame_size") && ok;
        ok = LoadProc(api.set_storage_size, "ass_set_storage_size") && ok;
        ok = LoadProc(api.set_pixel_aspect, "ass_set_pixel_aspect") && ok;
        ok = LoadProc(api.set_fonts, "ass_set_fonts") && ok;
        ok = LoadProc(api.set_hinting, "ass_set_hinting") && ok;
        ok = LoadProc(api.set_shaper, "ass_set_shaper") && ok;
        ok = LoadProc(api.set_cache_limits, "ass_set_cache_limits") && ok;
        ok = LoadProc(api.new_track, "ass_new_track") && ok;
        ok = LoadProc(api.free_track, "ass_free_track") && ok;
        ok = LoadProc(api.process_codec_private, "ass_process_codec_private") && ok;
        ok = LoadProc(api.process_data, "ass_process_data") && ok;
        ok = LoadProc(api.read_memory, "ass_read_memory") && ok;
        ok = LoadProc(api.process_chunk, "ass_process_chunk") && ok;
        ok = LoadProc(api.configure_prune, "ass_configure_prune") && ok;
        ok = LoadProc(api.flush_events, "ass_flush_events") && ok;
        ok = LoadProc(api.render_frame, "ass_render_frame") && ok;
        api.set_check_readorder = reinterpret_cast<decltype(api.set_check_readorder)>(
            GetProcAddress(dll, "ass_set_check_readorder"));
        return ok;
    }

    std::filesystem::path DefaultFontPath() const {
        wchar_t windowsDir[MAX_PATH]{};
        const UINT length = GetWindowsDirectoryW(windowsDir, static_cast<UINT>(std::size(windowsDir)));
        if (length > 0 && length < std::size(windowsDir)) {
            const auto arial = std::filesystem::path(windowsDir) / L"Fonts" / L"arial.ttf";
            if (PathExists(arial)) {
                return arial;
            }
        }
        return {};
    }

    static std::wstring HexVersion(const int version) {
        wchar_t buffer[32]{};
        swprintf_s(buffer, L"%08x", static_cast<unsigned int>(version));
        return buffer;
    }

    static void AssMessageCallback(int level, const char* format, va_list args, void* data) {
        if (level > 2 || !data || !format) {
            return;
        }
        auto* self = static_cast<Impl*>(data);
        char buffer[1024]{};
        va_list copy;
        va_copy(copy, args);
        std::vsnprintf(buffer, sizeof(buffer), format, copy);
        va_end(copy);
        self->Log(level <= 1 ? LogLevel::Error : LogLevel::Warning,
                  L"libass",
                  Utf8ToWide(buffer));
    }

    void Log(const LogLevel level, const std::wstring& category, const std::wstring& message) const {
        if (logSink) {
            logSink->Write(level, category, message);
        }
        OutputDebugStringW((L"[" + category + L"] " + message + L"\n").c_str());
    }

    LogSinkPtr logSink;
    HMODULE dll = nullptr;
    std::filesystem::path loadedPath;
    Api api;
    ASS_Library* library = nullptr;
    ASS_Renderer* renderer = nullptr;
    ASS_Track* track = nullptr;
    int configuredFrameWidth = 0;
    int configuredFrameHeight = 0;
    bool cacheValid = false;
    int lastFrameWidth = 0;
    int lastFrameHeight = 0;
    std::chrono::milliseconds lastRenderedPts{0};
    std::vector<NativeSubtitleBitmap> cachedBitmaps;
    bool coalescedRenderLogged = false;
    int slowRenderLogCount = 0;
};

LibassSubtitleRenderer::LibassSubtitleRenderer(LogSinkPtr logSink)
    : impl_(std::make_unique<Impl>(std::move(logSink))) {}

LibassSubtitleRenderer::~LibassSubtitleRenderer() = default;

bool LibassSubtitleRenderer::Initialize(const std::filesystem::path& searchRoot) {
    return impl_->Initialize(searchRoot);
}

bool LibassSubtitleRenderer::IsAvailable() const {
    return impl_ && impl_->IsAvailable();
}

void LibassSubtitleRenderer::ResetTrack() {
    impl_->ResetTrack();
}

void LibassSubtitleRenderer::FlushEvents() {
    impl_->FlushEvents();
}

void LibassSubtitleRenderer::ResetRenderCache() {
    impl_->ResetRenderCache();
}

bool LibassSubtitleRenderer::AddFont(const std::string& name, const uint8_t* data, const int size) {
    return impl_->AddFont(name, data, size);
}

bool LibassSubtitleRenderer::ConfigureTrackFromCodecPrivate(const uint8_t* data, const int size) {
    return impl_->ConfigureTrackFromCodecPrivate(data, size);
}

bool LibassSubtitleRenderer::ConfigureTrackFromMemory(const uint8_t* data, const std::size_t size) {
    return impl_->ConfigureTrackFromMemory(data, size);
}

bool LibassSubtitleRenderer::ProcessPacket(const uint8_t* data,
                                           const int size,
                                           const std::chrono::milliseconds pts,
                                           const std::chrono::milliseconds duration) {
    return impl_->ProcessPacket(data, size, pts, duration);
}

std::vector<NativeSubtitleBitmap> LibassSubtitleRenderer::Render(const std::chrono::milliseconds pts,
                                                                 const int frameWidth,
                                                                 const int frameHeight,
                                                                 uint64_t& serial) {
    return impl_->Render(pts, frameWidth, frameHeight, serial);
}

}  // namespace anvil::app
