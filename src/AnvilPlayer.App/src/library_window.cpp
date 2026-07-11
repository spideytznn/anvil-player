#include "AnvilPlayer/App/library_window.h"

#include "AnvilPlayer/App/app_arguments.h"
#include "AnvilPlayer/App/app_messages.h"
#include "AnvilPlayer/App/display_refresh_rate.h"
#include "AnvilPlayer/App/library_commands.h"
#include "AnvilPlayer/App/rect_util.h"
#include "AnvilPlayer/App/single_instance.h"
#include "AnvilPlayer/App/string_util.h"
#include "AnvilPlayer/App/window_chrome.h"
#include "AnvilPlayer/App/web_ui_json.h"
#include "AnvilPlayer/App/webui_root.h"
#include "AnvilPlayer/Playback/MediaProbe.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <list>
#include <mutex>
#include <new>
#include <set>
#include <sstream>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace anvil::app {

constexpr std::size_t kMaxLibraryOperations = 16;
constexpr std::size_t kMaxLibraryJobs = 16;
constexpr std::size_t kMaxLibraryResults = kMaxLibraryOperations * 2;
constexpr std::size_t kMaxLibraryRetiredPayloads = 32;
constexpr wchar_t kVideoSettingsRegistryPath[] = L"Software\\AnvilPlayer\\Video";

bool LoadVideoPassthroughSetting(const wchar_t* name, const bool fallback) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     kVideoSettingsRegistryPath,
                     name,
                     RRF_RT_REG_DWORD,
                     nullptr,
                     &value,
                     &size) != ERROR_SUCCESS) {
        return fallback;
    }
    return value != 0;
}

void SaveVideoPassthroughSetting(const wchar_t* name, const bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        kVideoSettingsRegistryPath,
                        0,
                        nullptr,
                        REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE,
                        nullptr,
                        &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    const DWORD value = enabled ? 1u : 0u;
    RegSetValueExW(key,
                   name,
                   0,
                   REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value),
                   sizeof(value));
    RegCloseKey(key);
}

enum class LibraryAsyncResultKind {
    Json,
    SmbConnection,
};

struct LibraryAsyncOperation {
    std::wstring key;
    std::uint64_t generation = 0;
    std::uint64_t lifetimeGeneration = 0;
    std::stop_source stopSource;
    std::stop_token shutdownToken;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);

    bool StopRequested() const noexcept {
        return stopSource.stop_requested() || shutdownToken.stop_requested();
    }
};

struct LibraryAsyncResult {
    LibraryAsyncResultKind kind = LibraryAsyncResultKind::Json;
    std::shared_ptr<LibraryAsyncOperation> operation;
    bool finalResult = true;
    std::wstring json;
    std::optional<std::wstring> connectionError;
};

struct LibraryAsyncJob {
    std::shared_ptr<LibraryAsyncOperation> operation;
    std::function<void()> run;
};

struct LibraryPendingResultIds {
    std::uintptr_t progress = 0;
    std::uintptr_t final = 0;
};

// Superseded payloads are moved here in O(1) and destroyed by a library
// worker. Keeping this ring strictly bounded prevents rapid Web messages from
// turning cancellation itself into an unbounded queue when all I/O workers
// are blocked in the OS.
struct LibraryRetiredPayload {
    std::optional<LibraryAsyncJob> job;
    std::array<std::optional<LibraryAsyncResult>, 2> results;
    std::shared_ptr<LibraryAsyncOperation> operation;
};

struct DeferredSmbPlayback {
    std::uint64_t generation = 0;
    std::wstring normalizedConnectionPath;
    std::optional<std::filesystem::path> playbackPath;
    double startPositionRatio = 0.0;
    int audioTrackIndex = -2;
    int subtitleTrackIndex = -2;
};

struct LibraryWindowAsyncState {
    LibraryWindowAsyncState();

    // Workers retain this state, never LibraryWindow. Closing the HWND only
    // invalidates the target and generations; it never waits for network I/O.
    std::mutex mutex;
    std::condition_variable workAvailable;
    bool acceptingResults = false;
    bool shuttingDown = false;
    HWND targetWindow = nullptr;
    std::uintptr_t messageCookie = 0;
    std::size_t workerCount = 0;
    std::uint64_t lifetimeGeneration = 1;
    std::uint64_t nextGeneration = 1;
    std::uintptr_t nextResultId = 1;
    std::stop_source shutdownStopSource;
    std::list<LibraryAsyncJob> jobs;
    std::unordered_map<std::wstring, std::list<LibraryAsyncJob>::iterator> queuedJobsByKey;
    std::unordered_map<std::wstring, std::shared_ptr<LibraryAsyncOperation>> operations;
    std::unordered_map<std::uintptr_t, LibraryAsyncResult> results;
    std::unordered_map<std::wstring, LibraryPendingResultIds> pendingResultsByKey;
    std::vector<LibraryRetiredPayload> retired;
    std::optional<DeferredSmbPlayback> deferredSmbPlayback;
};

LibraryWindowAsyncState::LibraryWindowAsyncState() {
    queuedJobsByKey.reserve(kMaxLibraryJobs);
    operations.reserve(kMaxLibraryOperations);
    results.reserve(kMaxLibraryResults);
    pendingResultsByKey.reserve(kMaxLibraryOperations);
    retired.reserve(kMaxLibraryRetiredPayloads);
}

namespace {

constexpr wchar_t kWebUiLibraryUrl[] = L"http://appassets.anvilplayer.local/index.html#/library";
// Timer id reused by Application for the Emby report relay retry.
constexpr UINT_PTR kApplicationTimer = 9001;
constexpr UINT_PTR kSmbConnectionFallbackTimer = 9002;
constexpr UINT kSmbConnectionFallbackMs = 5000;
constexpr std::size_t kLibraryWorkerCount = 4;
constexpr wchar_t kLibraryQueueBusy[] = L"Library background queue is busy; please retry.";
std::atomic_uintptr_t gNextLibraryMessageCookie{1};

std::size_t EffectiveLibraryOperationLimit(const LibraryWindowAsyncState& state) noexcept {
    return std::min(kMaxLibraryOperations, state.workerCount * 4);
}

std::size_t EffectiveLibraryJobLimit(const LibraryWindowAsyncState& state) noexcept {
    return std::min(kMaxLibraryJobs, state.workerCount * 4);
}

std::size_t EffectiveLibraryResultLimit(const LibraryWindowAsyncState& state) noexcept {
    return std::min(kMaxLibraryResults, EffectiveLibraryOperationLimit(state) * 2);
}

std::size_t EffectiveLibraryRetiredLimit(const LibraryWindowAsyncState& state) noexcept {
    return std::min(kMaxLibraryRetiredPayloads, state.workerCount * 8);
}

int ScaleForDpi(int value, UINT dpi) {
    return MulDiv(value, dpi, 96);
}

std::uintptr_t AllocateLibraryMessageCookie() noexcept {
    std::uintptr_t cookie = 0;
    while (cookie == 0) {
        cookie = gNextLibraryMessageCookie.fetch_add(1, std::memory_order_relaxed);
    }
    return cookie;
}

void LibraryWorkerLoop(const std::shared_ptr<LibraryWindowAsyncState>& state) {
    for (;;) {
        LibraryAsyncJob job;
        LibraryRetiredPayload retiredPayload;
        bool retiring = false;
        {
            std::unique_lock lock(state->mutex);
            state->workAvailable.wait(lock, [&state]() {
                return state->shuttingDown || !state->retired.empty() || !state->jobs.empty();
            });
            if (state->shuttingDown) {
                return;
            }
            if (!state->retired.empty()) {
                retiredPayload = std::move(state->retired.back());
                state->retired.pop_back();
                retiring = true;
            } else {
                const auto queuedJob = state->jobs.begin();
                if (queuedJob->operation) {
                    const auto indexed = state->queuedJobsByKey.find(queuedJob->operation->key);
                    if (indexed != state->queuedJobsByKey.end() && indexed->second == queuedJob) {
                        state->queuedJobsByKey.erase(indexed);
                    }
                }
                job = std::move(*queuedJob);
                state->jobs.erase(queuedJob);
            }
        }

        // Destruction of superseded JSON and captured request data happens on
        // this worker after the state mutex is released.
        if (retiring) {
            continue;
        }
        if (!job.operation || job.operation->StopRequested()) {
            continue;
        }
        try {
            job.run();
        } catch (...) {
            OutputDebugStringW(L"[library] unhandled exception in async I/O worker\n");
        }
    }
}

void StartLibraryWorkers(const std::shared_ptr<LibraryWindowAsyncState>& state) noexcept {
    for (std::size_t index = 0; index < kLibraryWorkerCount; ++index) {
        try {
            std::thread worker([state]() {
                LibraryWorkerLoop(state);
            });
            worker.detach();
            std::lock_guard lock(state->mutex);
            ++state->workerCount;
        } catch (...) {
            OutputDebugStringW(L"[library] failed to start an async I/O worker\n");
        }
    }
}

void ActivateLibraryAsyncState(const std::shared_ptr<LibraryWindowAsyncState>& state, const HWND hwnd) {
    if (!state) {
        return;
    }
    std::lock_guard lock(state->mutex);
    if (state->shuttingDown) {
        return;
    }
    state->targetWindow = hwnd;
    state->acceptingResults = hwnd != nullptr;
}

enum class LibraryAdmissionStatus {
    Accepted,
    Unavailable,
    Overloaded,
};

struct LibraryOperationStart {
    LibraryAdmissionStatus status = LibraryAdmissionStatus::Unavailable;
    std::shared_ptr<LibraryAsyncOperation> operation;
};

LibraryOperationStart BeginLibraryOperation(
    const std::shared_ptr<LibraryWindowAsyncState>& state,
    std::wstring key) {
    if (!state) {
        return {};
    }

    std::shared_ptr<LibraryAsyncOperation> operation;
    try {
        operation = std::make_shared<LibraryAsyncOperation>();
        operation->key = key;
    } catch (...) {
        return {LibraryAdmissionStatus::Overloaded, {}};
    }

    bool retiredExisting = false;
    {
        std::lock_guard lock(state->mutex);
        if (state->shuttingDown || !state->acceptingResults || !state->targetWindow) {
            return {};
        }
        if (state->workerCount == 0) {
            return {LibraryAdmissionStatus::Overloaded, {}};
        }

        const auto current = state->operations.find(key);
        if (current != state->operations.end()) {
            if (state->retired.size() >= EffectiveLibraryRetiredLimit(*state)) {
                return {LibraryAdmissionStatus::Overloaded, {}};
            }

            LibraryRetiredPayload retired;
            current->second->stopSource.request_stop();

            if (const auto queued = state->queuedJobsByKey.find(key);
                queued != state->queuedJobsByKey.end()) {
                retired.job.emplace(std::move(*queued->second));
                state->jobs.erase(queued->second);
                state->queuedJobsByKey.erase(queued);
            }

            if (const auto pending = state->pendingResultsByKey.find(key);
                pending != state->pendingResultsByKey.end()) {
                const std::array ids{pending->second.progress, pending->second.final};
                for (std::size_t index = 0; index < ids.size(); ++index) {
                    if (ids[index] == 0) {
                        continue;
                    }
                    if (const auto result = state->results.find(ids[index]);
                        result != state->results.end()) {
                        retired.results[index].emplace(std::move(result->second));
                        state->results.erase(result);
                    }
                }
                pending->second = {};
            }

            retired.operation = std::move(current->second);
            state->operations.erase(current);
            state->retired.push_back(std::move(retired));
            retiredExisting = true;
        } else if (state->operations.size() >= EffectiveLibraryOperationLimit(*state) ||
                   state->pendingResultsByKey.size() >= EffectiveLibraryOperationLimit(*state)) {
            return {LibraryAdmissionStatus::Overloaded, {}};
        }

        operation->generation = state->nextGeneration++;
        if (operation->generation == 0) {
            operation->generation = state->nextGeneration++;
        }
        operation->lifetimeGeneration = state->lifetimeGeneration;
        operation->shutdownToken = state->shutdownStopSource.get_token();
        state->pendingResultsByKey.try_emplace(operation->key);
        state->operations.emplace(operation->key, operation);
    }

    if (retiredExisting) {
        state->workAvailable.notify_one();
    }
    return {LibraryAdmissionStatus::Accepted, std::move(operation)};
}

void CancelLibraryOperation(const std::shared_ptr<LibraryWindowAsyncState>& state,
                            const std::wstring& key) noexcept {
    if (!state || key.empty()) return;
    LibraryRetiredPayload retired;
    bool hasRetired = false;
    {
        std::lock_guard lock(state->mutex);
        const auto current = state->operations.find(key);
        if (current == state->operations.end()) return;
        current->second->stopSource.request_stop();
        if (const auto queued = state->queuedJobsByKey.find(key);
            queued != state->queuedJobsByKey.end()) {
            retired.job.emplace(std::move(*queued->second));
            state->jobs.erase(queued->second);
            state->queuedJobsByKey.erase(queued);
        }
        if (const auto pending = state->pendingResultsByKey.find(key);
            pending != state->pendingResultsByKey.end()) {
            const std::array ids{pending->second.progress, pending->second.final};
            for (std::size_t index = 0; index < ids.size(); ++index) {
                if (ids[index] == 0) continue;
                if (const auto result = state->results.find(ids[index]); result != state->results.end()) {
                    retired.results[index].emplace(std::move(result->second));
                    state->results.erase(result);
                }
            }
            state->pendingResultsByKey.erase(pending);
        }
        retired.operation = std::move(current->second);
        state->operations.erase(current);
        if (state->retired.size() < EffectiveLibraryRetiredLimit(*state)) {
            state->retired.push_back(std::move(retired));
            hasRetired = true;
        }
    }
    if (hasRetired) state->workAvailable.notify_one();
}

LibraryAdmissionStatus QueueLibraryJob(
    const std::shared_ptr<LibraryWindowAsyncState>& state,
    const std::shared_ptr<LibraryAsyncOperation>& operation,
    std::function<void()> job,
    const bool highPriority = false) {
    if (!state || !operation || !job) {
        return LibraryAdmissionStatus::Unavailable;
    }

    {
        std::lock_guard lock(state->mutex);
        const auto current = state->operations.find(operation->key);
        if (state->shuttingDown || current == state->operations.end() ||
            current->second != operation || operation->StopRequested() ||
            operation->lifetimeGeneration != state->lifetimeGeneration) {
            return LibraryAdmissionStatus::Unavailable;
        }
        if (state->workerCount == 0 ||
            state->jobs.size() >= EffectiveLibraryJobLimit(*state) ||
            state->queuedJobsByKey.contains(operation->key)) {
            operation->stopSource.request_stop();
            state->operations.erase(current);
            state->pendingResultsByKey.erase(operation->key);
            return LibraryAdmissionStatus::Overloaded;
        }
        LibraryAsyncJob queued{operation, std::move(job)};
        std::list<LibraryAsyncJob>::iterator queuedJob;
        if (highPriority) {
            queuedJob = state->jobs.insert(state->jobs.begin(), std::move(queued));
        } else {
            queuedJob = state->jobs.insert(state->jobs.end(), std::move(queued));
        }
        state->queuedJobsByKey.emplace(operation->key, queuedJob);
    }
    state->workAvailable.notify_one();
    return LibraryAdmissionStatus::Accepted;
}

void PublishLibraryResult(const std::shared_ptr<LibraryWindowAsyncState>& state,
                          const std::shared_ptr<LibraryAsyncOperation>& operation,
                          LibraryAsyncResult result) {
    if (!state || !operation || operation->StopRequested()) {
        return;
    }

    LibraryRetiredPayload discarded;
    bool overflow = false;
    {
        std::lock_guard lock(state->mutex);
        const auto current = state->operations.find(operation->key);
        if (state->shuttingDown || !state->acceptingResults || !state->targetWindow ||
            current == state->operations.end() || current->second != operation ||
            operation->StopRequested() ||
            operation->lifetimeGeneration != state->lifetimeGeneration) {
            return;
        }

        const auto pending = state->pendingResultsByKey.find(operation->key);
        if (pending == state->pendingResultsByKey.end()) {
            return;
        }
        std::uintptr_t& slot = result.finalResult ? pending->second.final : pending->second.progress;
        if (slot != 0) {
            if (const auto previous = state->results.find(slot); previous != state->results.end()) {
                discarded.results[0].emplace(std::move(previous->second));
                state->results.erase(previous);
            }
            slot = 0;
        }
        if (state->results.size() >= EffectiveLibraryResultLimit(*state)) {
            overflow = true;
        } else {
            std::uintptr_t resultId = 0;
            do {
                resultId = state->nextResultId++;
            } while (resultId == 0 || state->results.contains(resultId));
            result.operation = operation;
            state->results.emplace(resultId, std::move(result));
            slot = resultId;
            if (!PostMessageW(
                    state->targetWindow,
                    kLocalFolderScanResultMessage,
                    static_cast<WPARAM>(state->messageCookie),
                    static_cast<LPARAM>(resultId))) {
                const auto failed = state->results.find(resultId);
                if (failed != state->results.end()) {
                    discarded.results[1].emplace(std::move(failed->second));
                    state->results.erase(failed);
                }
                slot = 0;
            }
        }
    }
    if (overflow) {
        OutputDebugStringW(L"[library] async result capacity exhausted; result rejected\n");
    }
}

std::optional<LibraryAsyncResult> TakeLibraryResult(
    const std::shared_ptr<LibraryWindowAsyncState>& state,
    const std::uintptr_t resultId) {
    if (!state || resultId == 0) {
        return std::nullopt;
    }

    std::lock_guard lock(state->mutex);
    const auto found = state->results.find(resultId);
    if (found == state->results.end()) {
        return std::nullopt;
    }

    LibraryAsyncResult result = std::move(found->second);
    state->results.erase(found);
    if (result.operation) {
        if (const auto pending = state->pendingResultsByKey.find(result.operation->key);
            pending != state->pendingResultsByKey.end()) {
            std::uintptr_t& slot = result.finalResult ? pending->second.final : pending->second.progress;
            if (slot == resultId) {
                slot = 0;
            }
        }
    }
    const auto removeEmptyPendingIndex = [&state, &result]() {
        if (!result.operation) {
            return;
        }
        const auto pending = state->pendingResultsByKey.find(result.operation->key);
        if (pending != state->pendingResultsByKey.end() &&
            pending->second.progress == 0 && pending->second.final == 0) {
            state->pendingResultsByKey.erase(pending);
        }
    };
    if (!result.operation || result.operation->StopRequested() ||
        result.operation->lifetimeGeneration != state->lifetimeGeneration) {
        removeEmptyPendingIndex();
        return std::nullopt;
    }
    const auto current = state->operations.find(result.operation->key);
    if (current == state->operations.end() || current->second != result.operation) {
        removeEmptyPendingIndex();
        return std::nullopt;
    }
    if (result.finalResult) {
        state->operations.erase(current);
        removeEmptyPendingIndex();
    }
    return result;
}

void StopLibraryAsyncState(const std::shared_ptr<LibraryWindowAsyncState>& state) noexcept {
    if (!state) {
        return;
    }
    {
        std::lock_guard lock(state->mutex);
        state->acceptingResults = false;
        state->targetWindow = nullptr;
        state->shuttingDown = true;
        state->shutdownStopSource.request_stop();
        ++state->lifetimeGeneration;
        if (state->lifetimeGeneration == 0) {
            ++state->lifetimeGeneration;
        }
    }
    state->workAvailable.notify_all();
}

void RetireLibraryAsyncState(std::shared_ptr<LibraryWindowAsyncState> state) noexcept {
    if (!state) {
        return;
    }

    // The holder is released on a background thread. If thread creation fails
    // it is intentionally retained: leaking one closed-window state is safer
    // than destroying bounded but potentially large scan JSON on the UI stack.
    auto* holder = new (std::nothrow) std::shared_ptr<LibraryWindowAsyncState>(std::move(state));
    if (!holder) {
        OutputDebugStringW(L"[library] failed to allocate async-state retirement holder\n");
        return;
    }
    try {
        std::thread([holder]() {
            delete holder;
        }).detach();
    } catch (...) {
        OutputDebugStringW(L"[library] failed to start async-state retirement thread\n");
    }
}

std::wstring MessageJson(const wchar_t* type, const std::wstring& message) {
    return L"{\"type\":\"" + std::wstring(type) + L"\",\"message\":\"" +
           JsonEscape(message) + L"\"}";
}

std::wstring MediaDetailsProbedJson(const std::wstring& requestId,
                                    const std::filesystem::path& path,
                                    const anvil::playback::MediaDescriptor& descriptor) {
    std::wostringstream json;
    json << L"{\"type\":\"mediaDetailsProbed\",\"requestId\":\"" << JsonEscape(requestId)
         << L"\",\"path\":\"" << JsonEscape(path.wstring()) << L"\",";
    std::wstring videoSpec;
    if (descriptor.hasVideo) {
        videoSpec = descriptor.videoCodec;
        if (descriptor.videoWidth > 0 && descriptor.videoHeight > 0) {
            videoSpec += (videoSpec.empty() ? L"" : L" · ") +
                         std::to_wstring(descriptor.videoWidth) + L"x" + std::to_wstring(descriptor.videoHeight);
        }
        if (descriptor.videoFrameRate > 0.0) {
            std::wostringstream fps;
            fps << std::fixed << std::setprecision(3) << descriptor.videoFrameRate << L" fps";
            videoSpec += (videoSpec.empty() ? L"" : L" · ") + fps.str();
        }
        if (!descriptor.hdrFormat.empty() && descriptor.hdrFormat != L"SDR / unknown" &&
            descriptor.hdrFormat != L"Probe pending") {
            videoSpec += (videoSpec.empty() ? L"" : L" · ") + descriptor.hdrFormat;
        }
    }
    json << L"\"videoSpec\":\"" << JsonEscape(videoSpec)
         << L"\",\"audioSpec\":\"" << JsonEscape(descriptor.audioCodec)
         << L"\",\"streamSpecs\":[";
    bool first = true;
    for (const auto& stream : descriptor.streams) {
        const std::wstring kind = LowerCopy(stream.kind);
        const wchar_t* type = kind == L"video" ? L"video" : kind == L"audio" ? L"audio" : kind == L"subtitle" ? L"subtitle" : nullptr;
        if (!type) continue;
        if (!first) json << L",";
        first = false;
        json << L"{\"id\":\"probe-" << stream.index << L"\",\"type\":\"" << type
             << L"\",\"title\":\"" << JsonEscape(stream.codec.empty() ? stream.kind : stream.codec)
             << L"\",\"subtitle\":\"" << JsonEscape(stream.details)
             << L"\",\"details\":["
             << L"{\"label\":\"流索引\",\"value\":\"" << stream.index << L"\"},"
             << L"{\"label\":\"编码\",\"value\":\"" << JsonEscape(stream.codec) << L"\"}";
        if (!stream.language.empty()) {
            json << L",{\"label\":\"语言\",\"value\":\"" << JsonEscape(stream.language) << L"\"}";
        }
        if (!stream.details.empty()) {
            json << L",{\"label\":\"详细\",\"value\":\"" << JsonEscape(stream.details) << L"\"}";
        }
        json << L"]}";
    }
    json << L"]}";
    return json.str();
}

std::wstring LocalScanOperationKey(const std::filesystem::path& folder) {
    return L"local-scan:" + LowerCopy(folder.lexically_normal().wstring());
}

std::wstring WebDavScanOperationKey(const std::wstring& url) {
    return L"webdav-scan:" + LowerCopy(NormalizeWebDavUrlText(url));
}

std::wstring SmbAccessKey(const std::filesystem::path& path) {
    const auto shareRoot = SmbShareRootFromPath(path);
    return LowerCopy(shareRoot.empty() ? NormalizeSmbPathText(path.wstring()) : shareRoot);
}

std::vector<LocalFolderScanItem> ScanLocalFolderCancellable(
    const std::filesystem::path& root,
    bool& truncated,
    const LibraryAsyncOperation& operation) {
    truncated = false;
    std::vector<LocalFolderScanItem> entries;

    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    while (!operation.StopRequested() && !error && iterator != end) {
        std::error_code entryError;
        if (iterator.depth() >= kMaxLocalFolderScanDepth && iterator->is_directory(entryError)) {
            iterator.disable_recursion_pending();
        }

        entryError.clear();
        if (iterator->is_regular_file(entryError)) {
            const auto entryPath = NormalizeListPath(iterator->path());
            if (IsMediaFilePath(entryPath)) {
                std::error_code sizeError;
                const auto fileSize = std::filesystem::file_size(entryPath, sizeError);
                entries.push_back(LocalFolderScanItem{
                    entryPath,
                    L"",
                    LastWriteTimeUnixMilliseconds(entryPath),
                    sizeError ? 0 : fileSize,
                });
                if (entries.size() >= kMaxLocalFolderScanItems) {
                    truncated = true;
                    break;
                }
            }
        }
        if (!operation.StopRequested()) {
            iterator.increment(error);
        }
    }

    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        if (left.modifiedAtMs != right.modifiedAtMs) {
            return left.modifiedAtMs > right.modifiedAtMs;
        }
        return LowerCopy(left.path.filename().wstring()) < LowerCopy(right.path.filename().wstring());
    });
    return entries;
}

std::vector<LocalFolderScanItem> ScanWebDavFolderCancellable(
    const std::wstring& rootUrl,
    const std::wstring& username,
    const std::wstring& password,
    bool& truncated,
    std::wstring& errorMessage,
    const LibraryAsyncOperation& operation,
    const WebDavRequestOptions& requestOptions) {
    truncated = false;
    std::vector<LocalFolderScanItem> items;
    std::deque<std::pair<std::wstring, int>> queue;
    std::set<std::wstring> seenDirectories;
    std::set<std::wstring> scheduledDirectories;
    std::set<std::wstring> seenFiles;
    constexpr std::size_t kMaxWebDavScanDirectories = 2048;
    const std::wstring normalizedRoot = NormalizeWebDavUrlText(rootUrl);
    queue.push_back({normalizedRoot, 0});
    scheduledDirectories.insert(LowerCopy(normalizedRoot));

    while (!operation.StopRequested() && !queue.empty() && items.size() < kMaxLocalFolderScanItems) {
        const auto [currentUrl, depth] = queue.front();
        queue.pop_front();
        const auto directoryKey = LowerCopy(NormalizeWebDavUrlText(currentUrl));
        if (!seenDirectories.insert(directoryKey).second) {
            continue;
        }

        std::wstring propFindError;
        const auto entries = WebDavPropFind(
            currentUrl,
            username,
            password,
            propFindError,
            requestOptions);
        if (operation.StopRequested()) {
            return items;
        }
        if (!propFindError.empty()) {
            errorMessage = std::move(propFindError);
            return items;
        }

        for (const auto& entry : entries) {
            if (operation.StopRequested()) {
                return items;
            }
            if (entry.isDirectory) {
                if (depth < kMaxLocalFolderScanDepth) {
                    const std::wstring directoryUrl = NormalizeWebDavUrlText(entry.href);
                    const std::wstring scheduledKey = LowerCopy(directoryUrl);
                    if (scheduledDirectories.contains(scheduledKey)) {
                        continue;
                    }
                    if (scheduledDirectories.size() >= kMaxWebDavScanDirectories) {
                        truncated = true;
                        continue;
                    }
                    scheduledDirectories.insert(scheduledKey);
                    queue.push_back({directoryUrl, depth + 1});
                }
                continue;
            }

            if (!IsMediaFilePath(std::filesystem::path(L"media" + WebDavExtension(entry.href)))) {
                continue;
            }
            const auto fileKey = LowerCopy(entry.href);
            if (!seenFiles.insert(fileKey).second) {
                continue;
            }
            items.push_back(LocalFolderScanItem{
                std::filesystem::path(entry.href),
                CredentialedWebDavUrl(entry.href, username, password),
                entry.modifiedAtMs,
                entry.sizeBytes,
            });
            if (items.size() >= kMaxLocalFolderScanItems) {
                truncated = true;
                break;
            }
        }
    }

    std::sort(items.begin(), items.end(), [](const auto& left, const auto& right) {
        return LowerCopy(left.path.filename().wstring()) < LowerCopy(right.path.filename().wstring());
    });
    return items;
}

}  // namespace

LibraryWindow::LibraryWindow()
    : asyncIoState_(std::make_shared<LibraryWindowAsyncState>()) {
    asyncIoState_->messageCookie = AllocateLibraryMessageCookie();
    StartLibraryWorkers(asyncIoState_);
}

LibraryWindow::~LibraryWindow() {
    CancelAsyncIo();
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

void LibraryWindow::SetPlaybackRequest(PlaybackRequest callback) {
    playbackRequest_ = std::move(callback);
}

void LibraryWindow::SetUiLanguageChangedRequest(std::function<void()> callback) {
    uiLanguageChangedRequest_ = std::move(callback);
}

void LibraryWindow::SetRefreshRatePreferencesChangedRequest(std::function<void()> callback) {
    refreshRatePreferencesChangedRequest_ = std::move(callback);
}

void LibraryWindow::SetVideoPassthroughPreferencesChangedRequest(std::function<void()> callback) {
    videoPassthroughPreferencesChangedRequest_ = std::move(callback);
}

void LibraryWindow::SetAllowInsecureCertificatesRequest(std::function<void(bool)> callback) {
    allowInsecureCertificatesRequest_ = std::move(callback);
}

void LibraryWindow::SetFocusPlayerRequest(std::function<void()> callback) {
    focusPlayerRequest_ = std::move(callback);
}

void LibraryWindow::SetEmbyPlaybackReportRelay(std::function<void(const std::wstring&)> callback) {
    embyPlaybackReportRelay_ = std::move(callback);
}

void LibraryWindow::SetQuitHandler(QuitHandler handler) {
    quitHandler_ = std::move(handler);
}

void LibraryWindow::SetTimerHandler(std::function<void()> callback) {
    timerHandler_ = std::move(callback);
}

void LibraryWindow::SetDebugLogHandler(std::function<void(const std::wstring&)> callback) {
    debugLogHandler_ = std::move(callback);
}

bool LibraryWindow::Create(HINSTANCE instance) {
    instance_ = instance;
    dpi_ = GetDpiForSystem();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance_;
    wc.lpfnWndProc = &LibraryWindow::WindowProc;
    wc.lpszClassName = kLibraryWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    // Forge-style borderless chrome: keep the native resize frame, shadow and
    // taskbar behavior, but let the Web UI own the title bar and controls.
    constexpr DWORD windowStyle = WS_POPUP |
                                  WS_THICKFRAME |
                                  WS_SYSMENU |
                                  WS_MINIMIZEBOX |
                                  WS_MAXIMIZEBOX |
                                  WS_CLIPCHILDREN;
    constexpr DWORD windowExStyle = WS_EX_APPWINDOW;
    const auto adjustedWindowRect = [this, windowStyle, windowExStyle](const int clientWidth, const int clientHeight) {
        RECT rect = MakeRect(0, 0, clientWidth, clientHeight);
        if (!AdjustWindowRectExForDpi(&rect, windowStyle, FALSE, windowExStyle, dpi_)) {
            AdjustWindowRectEx(&rect, windowStyle, FALSE, windowExStyle);
        }
        return rect;
    };

    const int targetClientWidth = ScaleForDpi(1280, dpi_);
    const int targetClientHeight = ScaleForDpi(800, dpi_);
    RECT initialRect = adjustedWindowRect(targetClientWidth, targetClientHeight);
    RECT workArea{};
    int initialX = CW_USEDEFAULT;
    int initialY = CW_USEDEFAULT;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0) &&
        RectWidth(workArea) > 0 &&
        RectHeight(workArea) > 0) {
        initialX = workArea.left + std::max(0, RectWidth(workArea) - RectWidth(initialRect)) / 2;
        initialY = workArea.top + std::max(0, RectHeight(workArea) - RectHeight(initialRect)) / 2;
    }

    hwnd_ = CreateWindowExW(
        windowExStyle,
        kLibraryWindowClassName,
        L"Anvil Player",
        windowStyle,
        initialX,
        initialY,
        RectWidth(initialRect),
        RectHeight(initialRect),
        nullptr,
        nullptr,
        instance_,
        this);

    if (!hwnd_) {
        return false;
    }

    // Do not rely solely on CreateWindowEx style normalization: explicitly
    // remove any caption bits and recalculate the non-client frame. This keeps
    // DWM's resize border/shadow without allowing a system title bar.
    const LONG_PTR createdStyle = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, createdStyle & ~static_cast<LONG_PTR>(WS_CAPTION));
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                 SWP_NOZORDER | SWP_NOACTIVATE);
    ApplyWindowChrome();

    TryCreateWebUi();
    return true;
}

void LibraryWindow::Show(const int commandShow) const {
    ShowWindow(hwnd_, commandShow);
    UpdateWindow(hwnd_);
}

bool LibraryWindow::DeliverLocalPlaybackProgress(const std::wstring& progressJson) const {
    if (!webUiActive_ || !webUiHost_ || !webUiHost_->Ready()) return false;
    webUiHost_->PostJson(progressJson);
    return true;
}

std::filesystem::path LibraryWindow::WebUiRoot() const {
    wchar_t modulePath[MAX_PATH]{};
    constexpr DWORD modulePathCount = static_cast<DWORD>(sizeof(modulePath) / sizeof(modulePath[0]));
    const DWORD length = GetModuleFileNameW(instance_, modulePath, modulePathCount);
    if (length > 0 && length < modulePathCount) {
        return std::filesystem::path(modulePath).parent_path() / L"webui";
    }
    return {};
}

bool LibraryWindow::TryCreateWebUi() {
    if (webUiActive_ || !hwnd_) {
        return false;
    }

    auto host = std::make_unique<WebUiHost>();
    const auto root = WebUiRoot();
    // Reuse the legacy base WebView2 folder (empty profile name) so the
    // library preserves existing user data (Emby connections, TMDB settings,
    // view state) saved before the window split. The player window uses a
    // separate "Player" subfolder.
    const bool started = host->Create(hwnd_, root, kWebUiLibraryUrl, L"",
                                      [this](const std::wstring& message) {
                                          HandleWebUiMessage(message);
                                      });
    if (!started) {
        return false;
    }

    webUiHost_ = std::move(host);
    webUiActive_ = true;
    return true;
}

void LibraryWindow::PostScanResult(const std::wstring& json) {
    if (webUiActive_ && webUiHost_ && webUiHost_->Ready()) {
        webUiHost_->PostJson(json);
    }
}

void LibraryWindow::StartLocalFolderScan(std::filesystem::path folder,
                                         std::wstring username,
                                         std::wstring password,
                                         const bool postPickedResult) {
    folder = NormalizeListPath(folder);
    const std::wstring overloadJson = postPickedResult
        ? MessageJson(L"localFolderPickFailed", kLibraryQueueBusy)
        : LocalFolderScanFailedJson(folder, kLibraryQueueBusy);
    auto start = BeginLibraryOperation(asyncIoState_, LocalScanOperationKey(folder));
    if (!start.operation) {
        if (start.status == LibraryAdmissionStatus::Overloaded) {
            PostScanResult(overloadJson);
        }
        return;
    }

    const auto state = asyncIoState_;
    const auto operation = std::move(start.operation);
    const SmbCredentials credentials{std::move(username), std::move(password)};
    const auto queueStatus = QueueLibraryJob(
        state,
        operation,
        [state, operation, folder = std::move(folder), credentials, postPickedResult]() {
            bool pickedResultPublished = false;
            try {
                if (const auto connectError = ConnectSmbPathForAccess(folder, credentials)) {
                    LibraryAsyncResult result;
                    result.json = postPickedResult
                        ? MessageJson(L"localFolderPickFailed", *connectError)
                        : LocalFolderScanFailedJson(folder, *connectError);
                    PublishLibraryResult(state, operation, std::move(result));
                    return;
                }
                if (operation->StopRequested()) {
                    return;
                }

                std::error_code error;
                const auto folderStatus = std::filesystem::status(folder, error);
                if (error || !std::filesystem::exists(folderStatus) ||
                    !std::filesystem::is_directory(folderStatus)) {
                    constexpr wchar_t kNotReadableFolder[] = L"所选路径不是可读取的文件夹";
                    LibraryAsyncResult result;
                    result.json = postPickedResult
                        ? MessageJson(L"localFolderPickFailed", kNotReadableFolder)
                        : LocalFolderScanFailedJson(folder, kNotReadableFolder);
                    PublishLibraryResult(state, operation, std::move(result));
                    return;
                }
                if (operation->StopRequested()) {
                    return;
                }

                if (postPickedResult) {
                    LibraryAsyncResult picked;
                    picked.finalResult = false;
                    picked.json = LocalFolderPickedJson(folder, {}, false, true);
                    PublishLibraryResult(state, operation, std::move(picked));
                    pickedResultPublished = true;
                }

                bool truncated = false;
                const auto items = ScanLocalFolderCancellable(folder, truncated, *operation);
                if (operation->StopRequested()) {
                    return;
                }
                LibraryAsyncResult completed;
                completed.json = LocalFolderScanCompletedJson(folder, items, truncated);
                PublishLibraryResult(state, operation, std::move(completed));
            } catch (const std::exception& error) {
                const std::wstring details = L"扫描本地文件夹失败：" + Utf8ToWide(error.what());
                LibraryAsyncResult failed;
                failed.json = postPickedResult && !pickedResultPublished
                    ? MessageJson(L"localFolderPickFailed", details)
                    : LocalFolderScanFailedJson(folder, details);
                PublishLibraryResult(state, operation, std::move(failed));
            } catch (...) {
                constexpr wchar_t kFailure[] = L"扫描本地文件夹失败";
                LibraryAsyncResult failed;
                failed.json = postPickedResult && !pickedResultPublished
                    ? MessageJson(L"localFolderPickFailed", kFailure)
                    : LocalFolderScanFailedJson(folder, kFailure);
                PublishLibraryResult(state, operation, std::move(failed));
            }
        });
    if (queueStatus == LibraryAdmissionStatus::Overloaded) {
        PostScanResult(overloadJson);
    }
}

void LibraryWindow::StartSmbDirectoryList(std::wstring requestId,
                                          std::wstring host,
                                          std::wstring path,
                                          std::wstring username,
                                          std::wstring password) {
    const auto requestedPath = std::filesystem::path(path.empty() ? host : path);
    const auto overloadJson = SmbDirectoryFailedJson(requestId, requestedPath, kLibraryQueueBusy);
    auto start = BeginLibraryOperation(asyncIoState_, L"smb-directory-list");
    if (!start.operation) {
        if (start.status == LibraryAdmissionStatus::Overloaded) {
            PostScanResult(overloadJson);
        }
        return;
    }

    const auto state = asyncIoState_;
    const auto operation = std::move(start.operation);
    const SmbCredentials credentials{std::move(username), std::move(password)};
    const auto queueStatus = QueueLibraryJob(
        state,
        operation,
        [state,
         operation,
         requestId = std::move(requestId),
         host = std::move(host),
         path = std::move(path),
         credentials]() {
            std::filesystem::path currentPath;
            try {
                std::wstring browseText = path.empty() ? host : path;
                browseText = NormalizeSmbPathText(std::move(browseText));
                const auto parts = SmbUncParts(browseText);
                if (parts.empty()) {
                    currentPath = std::filesystem::path(NormalizeSmbPathText(host));
                    LibraryAsyncResult failed;
                    failed.json = SmbDirectoryFailedJson(
                        requestId,
                        currentPath,
                        L"请填写有效的 SMB 主机或 IP。");
                    PublishLibraryResult(state, operation, std::move(failed));
                    return;
                }

                std::wstring errorMessage;
                std::vector<SmbDirectoryEntry> directories;
                if (parts.size() <= 1) {
                    const std::wstring serverRoot = SmbServerRootFromHost(browseText);
                    currentPath = std::filesystem::path(serverRoot);
                    directories = ListSmbShares(serverRoot, credentials, errorMessage);
                } else {
                    currentPath = std::filesystem::path(browseText);
                    directories = ListSmbChildDirectories(currentPath, credentials, errorMessage);
                }
                if (operation->StopRequested()) {
                    return;
                }

                LibraryAsyncResult result;
                result.json = errorMessage.empty()
                    ? SmbDirectoryListedJson(requestId, currentPath, directories)
                    : SmbDirectoryFailedJson(requestId, currentPath, errorMessage);
                PublishLibraryResult(state, operation, std::move(result));
            } catch (const std::exception& error) {
                LibraryAsyncResult failed;
                failed.json = SmbDirectoryFailedJson(
                    requestId,
                    currentPath,
                    L"读取 SMB 文件夹失败：" + Utf8ToWide(error.what()));
                PublishLibraryResult(state, operation, std::move(failed));
            }
        },
        true);
    if (queueStatus == LibraryAdmissionStatus::Overloaded) {
        PostScanResult(overloadJson);
    }
}

void LibraryWindow::StartSmbConnection(std::filesystem::path path,
                                       std::wstring username,
                                       std::wstring password) {
    if (hwnd_) {
        KillTimer(hwnd_, kSmbConnectionFallbackTimer);
    }
    path = std::filesystem::path(StartsWithInsensitive(path.wstring(), L"smb://")
        ? NormalizeSmbPathText(path.wstring())
        : path.wstring());
    auto start = BeginLibraryOperation(asyncIoState_, L"smb-connect");
    if (!start.operation) {
        if (start.status == LibraryAdmissionStatus::Overloaded) {
            PostScanResult(MessageJson(L"debugLog", kLibraryQueueBusy));
        }
        return;
    }

    const auto state = asyncIoState_;
    const auto operation = std::move(start.operation);
    {
        std::lock_guard lock(state->mutex);
        state->deferredSmbPlayback = DeferredSmbPlayback{
            operation->generation,
            SmbAccessKey(path),
            std::nullopt,
            0.0,
        };
    }

    const SmbCredentials credentials{std::move(username), std::move(password)};
    const auto queueStatus = QueueLibraryJob(
            state,
            operation,
            [state, operation, path = std::move(path), credentials]() {
                LibraryAsyncResult result;
                result.kind = LibraryAsyncResultKind::SmbConnection;
                try {
                    result.connectionError = ConnectSmbPathForAccess(path, credentials);
                } catch (const std::exception& error) {
                    result.connectionError = L"SMB 连接失败：" + Utf8ToWide(error.what());
                } catch (...) {
                    result.connectionError = L"SMB 连接失败";
                }
                PublishLibraryResult(state, operation, std::move(result));
            },
            true);
    if (queueStatus != LibraryAdmissionStatus::Accepted) {
        operation->stopSource.request_stop();
        std::lock_guard lock(state->mutex);
        if (state->deferredSmbPlayback &&
            state->deferredSmbPlayback->generation == operation->generation) {
            state->deferredSmbPlayback.reset();
        }
    }
    if (queueStatus == LibraryAdmissionStatus::Overloaded) {
        PostScanResult(MessageJson(L"debugLog", kLibraryQueueBusy));
    }
}

void LibraryWindow::StartWebDavDirectoryList(std::wstring requestId,
                                             std::wstring url,
                                             std::wstring username,
                                             std::wstring password) {
    const auto overloadJson = WebDavDirectoryFailedJson(requestId, url, kLibraryQueueBusy);
    auto start = BeginLibraryOperation(asyncIoState_, L"webdav-directory-list");
    if (!start.operation) {
        if (start.status == LibraryAdmissionStatus::Overloaded) {
            PostScanResult(overloadJson);
        }
        return;
    }

    const auto state = asyncIoState_;
    const auto operation = std::move(start.operation);
    const auto queueStatus = QueueLibraryJob(
        state,
        operation,
        [state,
         operation,
         requestId = std::move(requestId),
         url = std::move(url),
         username = std::move(username),
         password = std::move(password)]() {
            const auto normalizedUrl = NormalizeWebDavUrlText(url);
            try {
                WebDavRequestOptions requestOptions;
                requestOptions.deadline = operation->deadline;
                requestOptions.cancellationToken = operation->stopSource.get_token();
                requestOptions.shutdownToken = operation->shutdownToken;
                std::wstring errorMessage;
                const auto directories = ListWebDavDirectories(
                    normalizedUrl,
                    username,
                    password,
                    errorMessage,
                    requestOptions);
                if (operation->StopRequested()) {
                    return;
                }
                LibraryAsyncResult result;
                result.json = errorMessage.empty()
                    ? WebDavDirectoryListedJson(requestId, normalizedUrl, directories)
                    : WebDavDirectoryFailedJson(requestId, normalizedUrl, errorMessage);
                PublishLibraryResult(state, operation, std::move(result));
            } catch (const std::exception& error) {
                LibraryAsyncResult failed;
                failed.json = WebDavDirectoryFailedJson(
                    requestId,
                    normalizedUrl,
                    L"读取 WebDAV 文件夹失败：" + Utf8ToWide(error.what()));
                PublishLibraryResult(state, operation, std::move(failed));
            }
        },
        true);
    if (queueStatus == LibraryAdmissionStatus::Overloaded) {
        PostScanResult(overloadJson);
    }
}

void LibraryWindow::StartMediaDetailsProbe(std::wstring requestId, std::filesystem::path path) {
    if (requestId.empty() || path.empty()) return;
    auto start = BeginLibraryOperation(asyncIoState_, L"media-probe:" + requestId);
    if (!start.operation) {
        PostScanResult(L"{\"type\":\"mediaDetailsProbeFailed\",\"requestId\":\"" +
                       JsonEscape(requestId) + L"\",\"message\":\"媒体信息探测队列繁忙\"}");
        return;
    }
    const auto state = asyncIoState_;
    const auto operation = std::move(start.operation);
    const auto queueStatus = QueueLibraryJob(
        state,
        operation,
        [state, operation, requestId = std::move(requestId), path = std::move(path)]() {
            LibraryAsyncResult result;
            try {
                anvil::playback::MediaProbeOptions options;
                options.stopToken = operation->stopSource.get_token();
                options.timeout = std::chrono::seconds(20);
                const auto probe = anvil::playback::MediaProbe::Probe(path, options);
                if (operation->StopRequested()) return;
                if (probe.failed || probe.cancelled || probe.timedOut || probe.fallbackUsed ||
                    probe.descriptor.streams.empty()) {
                    result.json = L"{\"type\":\"mediaDetailsProbeFailed\",\"requestId\":\"" +
                                  JsonEscape(requestId) + L"\",\"message\":\"" +
                                  JsonEscape(probe.diagnostic.empty() ? L"未探测到媒体流信息" : probe.diagnostic) + L"\"}";
                } else {
                    result.json = MediaDetailsProbedJson(requestId, path, probe.descriptor);
                }
            } catch (const std::exception& error) {
                result.json = L"{\"type\":\"mediaDetailsProbeFailed\",\"requestId\":\"" +
                              JsonEscape(requestId) + L"\",\"message\":\"" +
                              JsonEscape(Utf8ToWide(error.what())) + L"\"}";
            }
            PublishLibraryResult(state, operation, std::move(result));
        },
        true);
    if (queueStatus == LibraryAdmissionStatus::Overloaded) {
        PostScanResult(L"{\"type\":\"mediaDetailsProbeFailed\",\"requestId\":\"" +
                       JsonEscape(requestId) + L"\",\"message\":\"媒体信息探测队列繁忙\"}");
    }
}

void LibraryWindow::StartBilibiliTrailerSearch(std::wstring requestId, std::wstring keyword) {
    if (requestId.empty() || keyword.empty()) return;
    auto start = BeginLibraryOperation(asyncIoState_, L"bilibili-trailer:" + requestId);
    if (!start.operation) {
        PostScanResult(L"{\"type\":\"bilibiliTrailerSearchFailed\",\"requestId\":\"" +
                       JsonEscape(requestId) + L"\",\"message\":\"B 站搜索队列繁忙\"}");
        return;
    }
    const auto state = asyncIoState_;
    const auto operation = std::move(start.operation);
    const auto queueStatus = QueueLibraryJob(
        state,
        operation,
        [state, operation, requestId = std::move(requestId), keyword = std::move(keyword)]() {
            LibraryAsyncResult result;
            try {
                WebDavRequestOptions options;
                options.deadline = operation->deadline;
                options.cancellationToken = operation->stopSource.get_token();
                options.shutdownToken = operation->shutdownToken;
                options.maxResponseBytes = 4 * 1024 * 1024;
                std::wstring errorMessage;
                const std::string response = SearchBilibiliVideos(keyword, errorMessage, options);
                if (operation->StopRequested()) return;
                if (!errorMessage.empty() || response.empty()) {
                    result.json = L"{\"type\":\"bilibiliTrailerSearchFailed\",\"requestId\":\"" +
                                  JsonEscape(requestId) + L"\",\"message\":\"" +
                                  JsonEscape(errorMessage.empty() ? L"B 站没有返回搜索结果" : errorMessage) + L"\"}";
                } else {
                    result.json = L"{\"type\":\"bilibiliTrailerSearchCompleted\",\"requestId\":\"" +
                                  JsonEscape(requestId) + L"\",\"response\":" + Utf8ToWide(response.c_str()) + L"}";
                }
            } catch (const std::exception& error) {
                result.json = L"{\"type\":\"bilibiliTrailerSearchFailed\",\"requestId\":\"" +
                              JsonEscape(requestId) + L"\",\"message\":\"" +
                              JsonEscape(Utf8ToWide(error.what())) + L"\"}";
            }
            PublishLibraryResult(state, operation, std::move(result));
        },
        true);
    if (queueStatus == LibraryAdmissionStatus::Overloaded) {
        PostScanResult(L"{\"type\":\"bilibiliTrailerSearchFailed\",\"requestId\":\"" +
                       JsonEscape(requestId) + L"\",\"message\":\"B 站搜索队列繁忙\"}");
    }
}

void LibraryWindow::StartWebDavScan(std::wstring url,
                                    std::wstring username,
                                    std::wstring password) {
    const auto normalizedUrl = NormalizeWebDavUrlText(url);
    const auto folder = std::filesystem::path(normalizedUrl);
    const auto overloadJson = LocalFolderScanFailedJson(folder, kLibraryQueueBusy);
    auto start = BeginLibraryOperation(asyncIoState_, WebDavScanOperationKey(normalizedUrl));
    if (!start.operation) {
        if (start.status == LibraryAdmissionStatus::Overloaded) {
            PostScanResult(overloadJson);
        }
        return;
    }

    const auto state = asyncIoState_;
    const auto operation = std::move(start.operation);
    const auto queueStatus = QueueLibraryJob(
        state,
        operation,
        [state,
         operation,
         normalizedUrl,
         username = std::move(username),
         password = std::move(password)]() {
            const auto folder = std::filesystem::path(normalizedUrl);
            try {
                WebDavRequestOptions requestOptions;
                requestOptions.deadline = operation->deadline;
                requestOptions.cancellationToken = operation->stopSource.get_token();
                requestOptions.shutdownToken = operation->shutdownToken;
                bool truncated = false;
                std::wstring errorMessage;
                const auto items = ScanWebDavFolderCancellable(
                    normalizedUrl,
                    username,
                    password,
                    truncated,
                    errorMessage,
                    *operation,
                    requestOptions);
                if (operation->StopRequested()) {
                    return;
                }
                LibraryAsyncResult result;
                result.json = errorMessage.empty()
                    ? LocalFolderScanCompletedJson(folder, items, truncated)
                    : LocalFolderScanFailedJson(folder, errorMessage);
                PublishLibraryResult(state, operation, std::move(result));
            } catch (const std::exception& error) {
                LibraryAsyncResult failed;
                failed.json = LocalFolderScanFailedJson(
                    folder,
                    L"扫描 WebDAV 文件夹失败：" + Utf8ToWide(error.what()));
                PublishLibraryResult(state, operation, std::move(failed));
            }
        });
    if (queueStatus == LibraryAdmissionStatus::Overloaded) {
        PostScanResult(overloadJson);
    }
}

void LibraryWindow::HandleAsyncIoResult(const std::uintptr_t resultId) {
    auto result = TakeLibraryResult(asyncIoState_, resultId);
    if (!result) {
        return;
    }
    if (result->kind == LibraryAsyncResultKind::Json) {
        PostScanResult(result->json);
        return;
    }

    if (result->connectionError) {
        PostScanResult(MessageJson(L"debugLog", L"smb connect failed: " + *result->connectionError));
    }

    bool completesDeferredPlayback = false;
    {
        std::lock_guard lock(asyncIoState_->mutex);
        completesDeferredPlayback = asyncIoState_->deferredSmbPlayback &&
            result->operation &&
            asyncIoState_->deferredSmbPlayback->generation == result->operation->generation;
    }
    if (completesDeferredPlayback) {
        CompleteDeferredSmbPlayback();
    }
}

void LibraryWindow::CancelAsyncIo() noexcept {
    if (hwnd_) {
        KillTimer(hwnd_, kSmbConnectionFallbackTimer);
    }
    auto retiredState = std::exchange(asyncIoState_, {});
    StopLibraryAsyncState(retiredState);
    RetireLibraryAsyncState(std::move(retiredState));
}

bool LibraryWindow::DeferPlaybackForPendingSmbConnection(
    const std::filesystem::path& path,
    const double startPositionRatio,
    const int audioTrackIndex,
    const int subtitleTrackIndex) {
    if (!asyncIoState_) {
        return false;
    }

    bool deferred = false;
    {
        std::lock_guard lock(asyncIoState_->mutex);
        const std::wstring normalizedPath = SmbAccessKey(path);
        if (asyncIoState_->deferredSmbPlayback &&
            asyncIoState_->deferredSmbPlayback->normalizedConnectionPath == normalizedPath) {
            asyncIoState_->deferredSmbPlayback->playbackPath = path;
            asyncIoState_->deferredSmbPlayback->startPositionRatio = startPositionRatio;
            asyncIoState_->deferredSmbPlayback->audioTrackIndex = audioTrackIndex;
            asyncIoState_->deferredSmbPlayback->subtitleTrackIndex = subtitleTrackIndex;
            deferred = true;
        }
    }
    if (deferred && hwnd_) {
        SetTimer(hwnd_, kSmbConnectionFallbackTimer, kSmbConnectionFallbackMs, nullptr);
    }
    return deferred;
}

void LibraryWindow::CompleteDeferredSmbPlayback() {
    std::optional<std::filesystem::path> path;
    double startPositionRatio = 0.0;
    int audioTrackIndex = -2;
    int subtitleTrackIndex = -2;
    {
        std::lock_guard lock(asyncIoState_->mutex);
        if (!asyncIoState_->deferredSmbPlayback) {
            return;
        }
        path = std::move(asyncIoState_->deferredSmbPlayback->playbackPath);
        startPositionRatio = asyncIoState_->deferredSmbPlayback->startPositionRatio;
        audioTrackIndex = asyncIoState_->deferredSmbPlayback->audioTrackIndex;
        subtitleTrackIndex = asyncIoState_->deferredSmbPlayback->subtitleTrackIndex;
        asyncIoState_->deferredSmbPlayback.reset();
    }
    if (hwnd_) {
        KillTimer(hwnd_, kSmbConnectionFallbackTimer);
    }
    if (path && playbackRequest_) {
        playbackRequest_(*path, startPositionRatio, audioTrackIndex, subtitleTrackIndex);
    }
}

void LibraryWindow::OpenLocalFolderDialog() {
    const auto postMessage = [this](const std::wstring& json) {
        PostScanResult(json);
    };

    BROWSEINFOW dialog{};
    dialog.hwndOwner = hwnd_;
    dialog.lpszTitle = L"选择本地媒体文件夹";
    dialog.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&dialog);
    if (!pidl) {
        postMessage(L"{\"type\":\"localFolderPickCancelled\"}");
        return;
    }

    wchar_t folderPath[MAX_PATH]{};
    const BOOL hasPath = SHGetPathFromIDListW(pidl, folderPath);
    CoTaskMemFree(pidl);

    if (!hasPath || folderPath[0] == L'\0') {
        postMessage(L"{\"type\":\"localFolderPickFailed\",\"message\":\"无法读取所选文件夹路径\"}");
        return;
    }

    const auto folder = NormalizeListPath(folderPath);
    StartLocalFolderScan(folder, L"", L"", true);
}

void LibraryWindow::OpenMediaFileDialog() {
    std::vector<wchar_t> filePath(32768);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = filePath.data();
    dialog.nMaxFile = static_cast<DWORD>(filePath.size());
    dialog.lpstrFilter = L"Media Files\0*.mp4;*.mkv;*.mov;*.m2ts;*.ts;*.webm;*.avi\0All Files\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&dialog) && playbackRequest_) {
        playbackRequest_(std::filesystem::path(filePath.data()), 0.0, -2, -2);
    }
}

void LibraryWindow::ApplyWindowChrome() const {
    ApplyFramelessWindowChrome(hwnd_, RGB(1, 3, 6), RGB(239, 241, 245));
    UpdateFramelessWindowRegion(hwnd_, dpi_);
}

void LibraryWindow::HandleWebUiMessage(const std::wstring_view message) {
    if (MessageContains(message, L"\"type\":\"requestState\"")) {
        // The library route does not consume PlayerState; acknowledge with an
        // empty state so subscribeNativeState resolves without blocking.
        PostScanResult(L"{\"type\":\"state\",\"state\":{}}");
        return;
    }

    if (!MessageContains(message, L"\"type\":\"command\"")) {
        return;
    }

    if (MessageContains(message, L"\"command\":\"setAllowInsecureCertificates\"")) {
        const bool enabled = MessageContains(message, L"\"enabled\":true");
        if (webUiHost_) {
            webUiHost_->SetAllowInsecureCertificates(enabled);
        }
        if (allowInsecureCertificatesRequest_) {
            allowInsecureCertificatesRequest_(enabled);
        }
    } else if (MessageContains(message, L"\"command\":\"setLibraryWebViewMuted\"")) {
        if (webUiHost_) {
            webUiHost_->SetMuted(MessageContains(message, L"\"muted\":true"));
        }
    } else if (MessageContains(message, L"\"command\":\"open\"")) {
        OpenMediaFileDialog();
    } else if (MessageContains(message, L"\"command\":\"pickLocalFolder\"")) {
        OpenLocalFolderDialog();
    } else if (MessageContains(message, L"\"command\":\"scanLocalFolder\"")) {
        if (const auto path = ReadJsonString(message, L"path")) {
            std::wstring pathText = *path;
            if (StartsWithInsensitive(pathText, L"smb://")) {
                pathText = NormalizeSmbPathText(pathText);
            }
            StartLocalFolderScan(
                std::filesystem::path(pathText),
                ReadJsonString(message, L"username").value_or(L""),
                ReadJsonString(message, L"password").value_or(L""),
                false);
        }
    } else if (MessageContains(message, L"\"command\":\"listSmbDirectory\"")) {
        StartSmbDirectoryList(
            ReadJsonString(message, L"requestId").value_or(L""),
            ReadJsonString(message, L"host").value_or(L""),
            ReadJsonString(message, L"path").value_or(L""),
            ReadJsonString(message, L"username").value_or(L""),
            ReadJsonString(message, L"password").value_or(L""));
    } else if (MessageContains(message, L"\"command\":\"connectSmbShare\"")) {
        if (const auto path = ReadJsonString(message, L"path")) {
            StartSmbConnection(
                std::filesystem::path(*path),
                ReadJsonString(message, L"username").value_or(L""),
                ReadJsonString(message, L"password").value_or(L""));
        }
    } else if (MessageContains(message, L"\"command\":\"listWebDavDirectory\"")) {
        StartWebDavDirectoryList(
            ReadJsonString(message, L"requestId").value_or(L""),
            ReadJsonString(message, L"url").value_or(L""),
            ReadJsonString(message, L"username").value_or(L""),
            ReadJsonString(message, L"password").value_or(L""));
    } else if (MessageContains(message, L"\"command\":\"scanWebDavFolder\"")) {
        StartWebDavScan(
            ReadJsonString(message, L"url").value_or(L""),
            ReadJsonString(message, L"username").value_or(L""),
            ReadJsonString(message, L"password").value_or(L""));
    } else if (MessageContains(message, L"\"command\":\"cancelLibraryScan\"")) {
        const auto path = ReadJsonString(message, L"path").value_or(L"");
        const bool webDav = MessageContains(message, L"\"webDav\":true");
        CancelLibraryOperation(asyncIoState_, webDav
            ? WebDavScanOperationKey(path)
            : LocalScanOperationKey(std::filesystem::path(path)));
    } else if (MessageContains(message, L"\"command\":\"cancelLibraryProbe\"")) {
        const auto requestId = ReadJsonString(message, L"requestId").value_or(L"");
        CancelLibraryOperation(asyncIoState_, L"media-probe:" + requestId);
    } else if (MessageContains(message, L"\"command\":\"debugLog\"")) {
        if (const auto text = ReadJsonString(message, L"message")) {
            OutputDebugStringW((L"[library] " + *text + L"\n").c_str());
            if (debugLogHandler_) {
                debugLogHandler_(*text);
            }
        }
    } else if (MessageContains(message, L"\"command\":\"searchBilibiliTrailers\"")) {
        StartBilibiliTrailerSearch(
            ReadJsonString(message, L"requestId").value_or(L""),
            ReadJsonString(message, L"keyword").value_or(L""));
    } else if (MessageContains(message, L"\"command\":\"probeMediaDetails\"")) {
        StartMediaDetailsProbe(
            ReadJsonString(message, L"requestId").value_or(L""),
            std::filesystem::path(ReadJsonString(message, L"path").value_or(L"")));
    } else if (MessageContains(message, L"\"command\":\"requestPlayback\"")) {
        if (const auto path = ReadJsonString(message, L"path")) {
            const double startRatio = ReadJsonNumber(message, L"startPositionRatio").value_or(0.0);
            const int audioTrackIndex = static_cast<int>(ReadJsonNumber(message, L"audioTrackIndex").value_or(-2.0));
            const int subtitleTrackIndex = static_cast<int>(ReadJsonNumber(message, L"subtitleTrackIndex").value_or(-2.0));
            const std::filesystem::path playbackPath(*path);
            if (!DeferPlaybackForPendingSmbConnection(playbackPath, startRatio, audioTrackIndex, subtitleTrackIndex) && playbackRequest_) {
                playbackRequest_(playbackPath, startRatio, audioTrackIndex, subtitleTrackIndex);
            }
        }
    } else if (MessageContains(message, L"\"command\":\"openExternalUrl\"")) {
        if (const auto url = ReadJsonString(message, L"url")) {
            if (StartsWithInsensitive(*url, L"https://") || StartsWithInsensitive(*url, L"http://")) {
                ShellExecuteW(nullptr, L"open", url->c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
    } else if (MessageContains(message, L"\"command\":\"setGlobalRefreshRateSync\"")) {
        DisplayRefreshRateController::SaveGlobalEnabled(MessageContains(message, L"\"enabled\":true"));
        if (refreshRatePreferencesChangedRequest_) refreshRatePreferencesChangedRequest_();
    } else if (MessageContains(message, L"\"command\":\"setGlobalRefreshRateMaximumMultiple\"")) {
        DisplayRefreshRateController::SaveMaximumMultipleEnabled(MessageContains(message, L"\"enabled\":true"));
        if (refreshRatePreferencesChangedRequest_) refreshRatePreferencesChangedRequest_();
    } else if (MessageContains(message, L"\"command\":\"requestGlobalVideoPassthroughSettings\"")) {
        const bool displayMetadata = LoadVideoPassthroughSetting(L"DisplayMetadataPassthrough", true);
        const bool autoDisplayFormat = LoadVideoPassthroughSetting(L"AutoDisplayFormat", false);
        const bool windowsHdrEnabled = anvil::playback::CapabilityDetector::IsHdrEnabledNow();
        const bool dolbySystemPipeline =
            windowsHdrEnabled &&
            LoadVideoPassthroughSetting(L"DolbyVisionSystemPipelineExperimental", false);
        PostScanResult(L"{\"type\":\"globalVideoPassthroughSettings\",\"autoDisplayFormat\":" +
                       std::wstring(autoDisplayFormat ? L"true" : L"false") +
                       L",\"displayMetadataPassthrough\":" +
                       std::wstring((autoDisplayFormat || displayMetadata) ? L"true" : L"false") +
                       L",\"dolbyVisionSystemPipelineExperimental\":" +
                       std::wstring((autoDisplayFormat || dolbySystemPipeline) ? L"true" : L"false") +
                       L",\"windowsHdrEnabled\":" +
                       std::wstring(windowsHdrEnabled ? L"true" : L"false") +
                       L"}");
    } else if (MessageContains(message, L"\"command\":\"setGlobalAutoDisplayFormat\"")) {
        SaveVideoPassthroughSetting(L"AutoDisplayFormat",
                                    MessageContains(message, L"\"enabled\":true"));
        if (videoPassthroughPreferencesChangedRequest_) videoPassthroughPreferencesChangedRequest_();
    } else if (MessageContains(message, L"\"command\":\"setGlobalDisplayMetadataPassthrough\"")) {
        if (LoadVideoPassthroughSetting(L"AutoDisplayFormat", false)) {
            return;
        }
        if (!anvil::playback::CapabilityDetector::IsHdrEnabledNow()) {
            return;
        }
        SaveVideoPassthroughSetting(L"DisplayMetadataPassthrough",
                                    MessageContains(message, L"\"enabled\":true"));
        if (videoPassthroughPreferencesChangedRequest_) videoPassthroughPreferencesChangedRequest_();
    } else if (MessageContains(message, L"\"command\":\"setGlobalDolbyVisionSystemPipelineExperimental\"")) {
        if (LoadVideoPassthroughSetting(L"AutoDisplayFormat", false)) {
            return;
        }
        const bool enabled = MessageContains(message, L"\"enabled\":true");
        if (enabled && !anvil::playback::CapabilityDetector::IsHdrEnabledNow()) {
            return;
        }
        SaveVideoPassthroughSetting(L"DolbyVisionSystemPipelineExperimental", enabled);
        if (videoPassthroughPreferencesChangedRequest_) videoPassthroughPreferencesChangedRequest_();
    } else if (MessageContains(message, L"\"command\":\"setUiLanguage\"")) {
        DisplayRefreshRateController::SaveUiLanguage(ReadJsonString(message, L"language").value_or(L"zh"));
        if (uiLanguageChangedRequest_) uiLanguageChangedRequest_();
    } else if (MessageContains(message, L"\"command\":\"focusPlayer\"")) {
        if (focusPlayerRequest_) {
            focusPlayerRequest_();
        }
    } else if (MessageContains(message, L"\"command\":\"requestWindowChrome\"")) {
        PostScanResult(L"{\"type\":\"windowChrome\",\"customTitleBar\":true}");
    } else if (MessageContains(message, L"\"command\":\"beginWindowDrag\"")) {
        if (!IsZoomed(hwnd_)) {
            ReleaseCapture();
            SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
    } else if (MessageContains(message, L"\"command\":\"minimizeWindow\"")) {
        ShowWindow(hwnd_, SW_MINIMIZE);
    } else if (MessageContains(message, L"\"command\":\"toggleMaximizeWindow\"")) {
        ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
    } else if (MessageContains(message, L"\"command\":\"closeWindow\"")) {
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    } else if (MessageContains(message, L"\"command\":\"deliverEmbyPlaybackReport\"")) {
        // Relay the full report object to the player window via Application so
        // the player's WebView (separate storage) can inject it.
        if (const auto reportJson = ReadJsonObject(message, L"report")) {
            if (embyPlaybackReportRelay_) {
                embyPlaybackReportRelay_(*reportJson);
            }
        }
    } else if (MessageContains(message, L"\"command\":\"setWebUiRoute\"")) {
        // Legacy single-window route notification; a standalone library window
        // is always on the library route, so this is a no-op.
    }
}

LRESULT CALLBACK LibraryWindow::WindowProc(HWND hwnd, const UINT message, const WPARAM wParam, const LPARAM lParam) {
    LibraryWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<LibraryWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    } else {
        window = reinterpret_cast<LibraryWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window) {
        if (message == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            window->hwnd_ = nullptr;
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        return window->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT LibraryWindow::HandleMessage(const UINT message, const WPARAM wParam, const LPARAM lParam) {
    switch (message) {
    case WM_NCCALCSIZE:
        if (wParam != FALSE && IsZoomed(hwnd_)) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
                params->rgrc[0] = monitor.rcWork;
            }
        }
        return 0;
    case WM_NCHITTEST:
        if (!IsZoomed(hwnd_)) {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT windowRect{};
            GetWindowRect(hwnd_, &windowRect);
            const int edge = ScaleForDpi(6, dpi_);
            const bool left = point.x < windowRect.left + edge;
            const bool right = point.x >= windowRect.right - edge;
            const bool top = point.y < windowRect.top + edge;
            const bool bottom = point.y >= windowRect.bottom - edge;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        return HTCLIENT;
    case WM_CREATE:
        dpi_ = GetDpiForWindow(hwnd_);
        ActivateLibraryAsyncState(asyncIoState_, hwnd_);
        ApplyWindowChrome();
        return 0;
    case WM_COPYDATA: {
        // Single-instance command-line forwarding from a second launch. Parse
        // the forwarded command line the same way the entry point does and, if
        // it carries a media path, hand it to the player via the playback
        // request callback. WM_COPYDATA is the only message Windows marshals
        // across the process boundary.
        auto* copyData = reinterpret_cast<COPYDATASTRUCT*>(lParam);
        if (copyData && copyData->dwData == kForwardCommandLineMagic &&
            copyData->lpData != nullptr && copyData->cbData > 0 &&
            (copyData->cbData % sizeof(wchar_t)) == 0) {
            const std::size_t charCount = copyData->cbData / sizeof(wchar_t);
            std::wstring forwarded(static_cast<const wchar_t*>(copyData->lpData), charCount);
            const auto args = ParseCommandLine(forwarded);
            if (!args.mediaPath.empty() && playbackRequest_) {
                playbackRequest_(args.mediaPath, 0.0, -2, -2);
            }
        }
        return TRUE;
    }
    case WM_SIZE:
        UpdateFramelessWindowRegion(hwnd_, dpi_);
        if (webUiHost_) {
            RECT client{};
            GetClientRect(hwnd_, &client);
            webUiHost_->Resize(client);
        }
        return 0;
    case kLocalFolderScanResultMessage: {
        if (wParam != 0) {
            // A stale message can target a recycled HWND. Only the cookie for
            // this lifetime may consume an id from this state's result table.
            if (asyncIoState_ && wParam == static_cast<WPARAM>(asyncIoState_->messageCookie)) {
                HandleAsyncIoResult(static_cast<std::uintptr_t>(lParam));
            }
            return 0;
        }
        std::unique_ptr<std::wstring> json(reinterpret_cast<std::wstring*>(lParam));
        if (json) {
            PostScanResult(*json);
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = ScaleForDpi(640, dpi_);
        info->ptMinTrackSize.y = ScaleForDpi(420, dpi_);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kApplicationTimer) {
            KillTimer(hwnd_, kApplicationTimer);
            if (timerHandler_) {
                timerHandler_();
            }
        } else if (wParam == kSmbConnectionFallbackTimer) {
            KillTimer(hwnd_, kSmbConnectionFallbackTimer);
            CompleteDeferredSmbPlayback();
        }
        return 0;
    case WM_DESTROY:
        CancelAsyncIo();
        if (webUiHost_) {
            webUiHost_->Shutdown();
        }
        hwnd_ = nullptr;
        if (quitHandler_) {
            quitHandler_();
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace anvil::app
