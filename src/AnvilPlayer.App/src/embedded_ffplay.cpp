#include "AnvilPlayer/App/embedded_ffplay.h"

#include "AnvilPlayer/App/string_util.h"
#include "AnvilPlayer/App/ui_draw.h"

#include <algorithm>
#include <cwctype>
#include <system_error>

namespace anvil::app {

namespace {

std::filesystem::path FindBundledFfplayFrom(std::filesystem::path start) {
    std::error_code error;
    for (int depth = 0; depth < 6 && !start.empty(); ++depth) {
        const auto candidate = start / L"third_party" / L"ffmpeg" / L"bin" / L"ffplay.exe";
        if (std::filesystem::exists(candidate, error)) {
            return candidate;
        }

        const auto parent = start.parent_path();
        if (parent == start) {
            break;
        }
        start = parent;
    }
    return {};
}

// Executable discovery can touch slow or disconnected filesystems. It is only
// called by EmbeddedFfplayPlayer::ControlLoop, never by a window thread.
std::filesystem::path FfplayExecutablePath() {
    std::error_code error;

    wchar_t modulePath[MAX_PATH]{};
    const DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (moduleLength > 0 && moduleLength < std::size(modulePath)) {
        const auto executableDir = std::filesystem::path(modulePath).parent_path();
        const auto besideExecutable = executableDir / L"ffplay.exe";
        if (std::filesystem::exists(besideExecutable, error)) {
            return besideExecutable;
        }
        if (const auto bundled = FindBundledFfplayFrom(executableDir); !bundled.empty()) {
            return bundled;
        }
    }

    if (const auto bundled = FindBundledFfplayFrom(std::filesystem::current_path(error)); !bundled.empty()) {
        return bundled;
    }

    return L"ffplay.exe";
}

bool IsNetworkMediaPath(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value.rfind(L"http://", 0) == 0 ||
           value.rfind(L"https://", 0) == 0;
}

std::wstring DolbyVisionLibplaceboFilter() {
    return L"libplacebo="
           L"apply_dolbyvision=1:"
           L"tonemapping=auto:"
           L"gamut_mode=perceptual:"
           L"peak_detect=1:"
           L"colorspace=bt709:"
           L"color_primaries=bt709:"
           L"color_trc=bt709:"
           L"range=pc";
}

void CloseProcessHandles(PROCESS_INFORMATION& process) {
    if (process.hThread) {
        CloseHandle(process.hThread);
        process.hThread = nullptr;
    }
    if (process.hProcess) {
        CloseHandle(process.hProcess);
        process.hProcess = nullptr;
    }
    process.dwProcessId = 0;
    process.dwThreadId = 0;
}

}  // namespace

BOOL CALLBACK FindProcessWindowProc(HWND hwnd, LPARAM lParam) {
    auto* search = reinterpret_cast<WindowSearch*>(lParam);
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != search->processId || !IsWindowVisible(hwnd)) {
        return TRUE;
    }

    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    if (!search->title || search->title->empty() || *search->title == title) {
        search->window = hwnd;
        return FALSE;
    }
    return TRUE;
}

EmbeddedFfplayPlayer::~EmbeddedFfplayPlayer() {
    // MainWindow destruction is retired to a background reaper. Blocking here
    // is intentional: no control worker may outlive the fields it accesses.
    Stop();
    {
        std::scoped_lock lock(controlMutex_);
        exitRequested_ = true;
        desiredRunning_ = false;
        pendingStart_.reset();
        ++desiredGeneration_;
    }
    controlCv_.notify_all();
    if (controlThread_.joinable()) {
        controlThread_.join();
    }
}

bool EmbeddedFfplayPlayer::EnsureControlWorkerLocked() {
    if (workerStarted_) {
        return true;
    }

    workerStarted_ = true;
    try {
        controlThread_ = std::thread([this]() {
            ControlLoop();
        });
    } catch (const std::system_error&) {
        workerStarted_ = false;
        return false;
    }
    return true;
}

bool EmbeddedFfplayPlayer::Start(HWND parent,
                                 RECT bounds,
                                 const std::filesystem::path& mediaPath,
                                 const std::chrono::milliseconds startPosition,
                                 const double volume,
                                 const bool useLibplaceboDolbyVision) {
    if (!parent || mediaPath.empty()) {
        return false;
    }

    {
        std::scoped_lock lock(controlMutex_);
        if (!EnsureControlWorkerLocked()) {
            running_ = false;
            return false;
        }

        const std::uint64_t generation = ++desiredGeneration_;
        pendingStart_ = StartRequest{
            generation,
            parent,
            bounds,
            mediaPath,
            startPosition,
            volume,
            useLibplaceboDolbyVision,
        };
        pendingBounds_ = bounds;
        ++boundsGeneration_;
        desiredRunning_ = true;
        running_ = true;
        attached_ = false;
    }
    controlCv_.notify_all();
    return true;
}

std::uint64_t EmbeddedFfplayPlayer::RequestStopLocked() {
    const std::uint64_t generation = ++desiredGeneration_;
    desiredRunning_ = false;
    pendingStart_.reset();
    if (!workerStarted_) {
        settledGeneration_ = generation;
        running_ = false;
        attached_ = false;
    }
    return generation;
}

void EmbeddedFfplayPlayer::RequestStop() {
    {
        std::scoped_lock lock(controlMutex_);
        RequestStopLocked();
    }
    // This is deliberately only a state publication and wakeup. Process
    // termination, waits, handle closure, and HWND calls are worker-owned.
    controlCv_.notify_all();
}

void EmbeddedFfplayPlayer::Stop() {
    std::unique_lock lock(controlMutex_);
    const std::uint64_t stopGeneration = RequestStopLocked();
    controlCv_.notify_all();
    if (!workerStarted_) {
        return;
    }
    controlCv_.wait(lock, [this, stopGeneration]() {
        return settledGeneration_ >= stopGeneration;
    });
}

bool EmbeddedFfplayPlayer::IsRunning() const {
    return running_.load();
}

bool EmbeddedFfplayPlayer::HasAttachedWindow() const {
    return attached_.load();
}

void EmbeddedFfplayPlayer::SetBounds(const RECT bounds) {
    {
        std::scoped_lock lock(controlMutex_);
        pendingBounds_ = bounds;
        ++boundsGeneration_;
    }
    controlCv_.notify_all();
}

std::wstring EmbeddedFfplayPlayer::LastCommandLine() const {
    std::scoped_lock lock(commandLineMutex_);
    return lastCommandLine_;
}

void EmbeddedFfplayPlayer::ApplyBounds(HWND child, const RECT bounds) {
    if (!child || !IsWindow(child)) {
        return;
    }

    const int width = RectWidth(bounds);
    const int height = RectHeight(bounds);
    if (width <= 0 || height <= 0) {
        MoveWindow(child, 0, 0, 1, 1, TRUE);
        ShowWindow(child, SW_HIDE);
        return;
    }

    SetWindowPos(child,
                 HWND_TOP,
                 bounds.left,
                 bounds.top,
                 width,
                 height,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ShowWindow(child, SW_SHOWNOACTIVATE);
}

void EmbeddedFfplayPlayer::AttachWindow(HWND parent, HWND child, const RECT bounds) {
    if (!parent || !child || !IsWindow(parent) || !IsWindow(child)) {
        return;
    }

    // All interactions with the foreign ffplay HWND happen on ControlLoop.
    ShowWindow(child, SW_HIDE);
    SetParent(child, parent);
    LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
    style &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    style |= WS_CHILD | WS_CLIPSIBLINGS;
    SetWindowLongPtrW(child, GWL_STYLE, style);

    LONG_PTR exStyle = GetWindowLongPtrW(child, GWL_EXSTYLE);
    exStyle &= ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
    SetWindowLongPtrW(child, GWL_EXSTYLE, exStyle);
    ApplyBounds(child, bounds);
}

void EmbeddedFfplayPlayer::ControlLoop() {
    PROCESS_INFORMATION process{};
    std::optional<StartRequest> activeRequest;
    HWND childWindow = nullptr;
    std::wstring windowTitle;
    std::uint64_t appliedBoundsGeneration = 0;
    int attachAttempts = 0;

    auto retireActiveProcess = [&](const bool terminate) {
        if (childWindow && IsWindow(childWindow)) {
            ShowWindow(childWindow, SW_HIDE);
        }
        childWindow = nullptr;
        attached_ = false;

        if (process.hProcess) {
            if (terminate && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process.hProcess, 0);
            }
            if (terminate) {
                WaitForSingleObject(process.hProcess, 2000);
            }
        }
        CloseProcessHandles(process);
        activeRequest.reset();
    };

    for (;;) {
        bool exitRequested = false;
        bool desiredRunning = false;
        std::uint64_t desiredGeneration = 0;
        RECT latestBounds{};
        std::uint64_t boundsGeneration = 0;
        {
            std::scoped_lock lock(controlMutex_);
            exitRequested = exitRequested_;
            desiredRunning = desiredRunning_;
            desiredGeneration = desiredGeneration_;
            latestBounds = pendingBounds_;
            boundsGeneration = boundsGeneration_;
        }

        if (activeRequest &&
            (exitRequested || !desiredRunning || activeRequest->generation != desiredGeneration)) {
            retireActiveProcess(true);
            continue;
        }

        if (!activeRequest) {
            std::optional<StartRequest> request;
            {
                std::unique_lock lock(controlMutex_);
                if (exitRequested_) {
                    running_ = false;
                    attached_ = false;
                    settledGeneration_ = std::max(settledGeneration_, desiredGeneration_);
                    controlCv_.notify_all();
                    break;
                }

                if (!desiredRunning_) {
                    running_ = false;
                    attached_ = false;
                    settledGeneration_ = std::max(settledGeneration_, desiredGeneration_);
                    controlCv_.notify_all();
                    controlCv_.wait(lock, [this]() {
                        return exitRequested_ || desiredRunning_;
                    });
                    continue;
                }

                if (!pendingStart_) {
                    controlCv_.wait(lock, [this]() {
                        return exitRequested_ || !desiredRunning_ || pendingStart_.has_value();
                    });
                    continue;
                }
                request = std::move(pendingStart_);
                pendingStart_.reset();
                if (request->generation > 0) {
                    settledGeneration_ = std::max(settledGeneration_, request->generation - 1);
                    controlCv_.notify_all();
                }
            }

            windowTitle = L"Anvil Player Playback " +
                          std::to_wstring(GetCurrentProcessId()) +
                          L"-" + std::to_wstring(request->generation);
            const int volumePercent = std::clamp(static_cast<int>(request->volume * 100.0 + 0.5), 0, 100);
            const int playbackWidth = std::max(1, RectWidth(request->bounds));
            const int playbackHeight = std::max(1, RectHeight(request->bounds));
            std::wstring commandLine =
                QuoteArgument(FfplayExecutablePath().wstring()) +
                L" -hide_banner -loglevel warning -autoexit "
                L"-hwaccel d3d11va -framedrop -noborder "
                L"-window_title " + QuoteArgument(windowTitle) +
                L" -x " + std::to_wstring(playbackWidth) +
                L" -y " + std::to_wstring(playbackHeight) +
                L" -ss " + FormatFfmpegSeekTime(request->startPosition) +
                L" -volume " + std::to_wstring(volumePercent);
            if (IsNetworkMediaPath(request->mediaPath)) {
                commandLine +=
                    L" -rw_timeout 15000000"
                    L" -seekable 1"
                    L" -http_seekable 1"
                    L" -reconnect_on_network_error 1"
                    L" -reconnect_streamed 1"
                    L" -reconnect_delay_max 2"
                    L" -user_agent " +
                    QuoteArgument(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/120 Safari/537.36");
            }
            if (request->useLibplaceboDolbyVision) {
                commandLine += L" -vf " + QuoteArgument(DolbyVisionLibplaceboFilter());
            }
            commandLine += L" " + QuoteArgument(request->mediaPath.wstring());
            {
                std::scoped_lock lock(commandLineMutex_);
                lastCommandLine_ = commandLine;
            }

            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            startupInfo.dwFlags = STARTF_USESHOWWINDOW;
            startupInfo.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION createdProcess{};
            const BOOL created = CreateProcessW(nullptr,
                                                commandLine.data(),
                                                nullptr,
                                                nullptr,
                                                FALSE,
                                                0,
                                                nullptr,
                                                nullptr,
                                                &startupInfo,
                                                &createdProcess);

            bool requestStillCurrent = false;
            {
                std::scoped_lock lock(controlMutex_);
                requestStillCurrent = !exitRequested_ && desiredRunning_ &&
                                      desiredGeneration_ == request->generation;
            }
            if (!created || !requestStillCurrent) {
                if (created) {
                    if (WaitForSingleObject(createdProcess.hProcess, 0) == WAIT_TIMEOUT) {
                        TerminateProcess(createdProcess.hProcess, 0);
                    }
                    WaitForSingleObject(createdProcess.hProcess, 2000);
                    CloseProcessHandles(createdProcess);
                }
                if (!created) {
                    std::scoped_lock lock(controlMutex_);
                    if (desiredGeneration_ == request->generation) {
                        desiredRunning_ = false;
                        running_ = false;
                        settledGeneration_ = std::max(settledGeneration_, request->generation);
                        controlCv_.notify_all();
                    }
                }
                continue;
            }

            process = createdProcess;
            activeRequest = std::move(request);
            childWindow = nullptr;
            attached_ = false;
            running_ = true;
            attachAttempts = 0;
            appliedBoundsGeneration = 0;
            continue;
        }

        if (WaitForSingleObject(process.hProcess, 0) != WAIT_TIMEOUT) {
            const std::uint64_t completedGeneration = activeRequest->generation;
            retireActiveProcess(false);
            std::scoped_lock lock(controlMutex_);
            if (desiredGeneration_ == completedGeneration) {
                desiredRunning_ = false;
                running_ = false;
                settledGeneration_ = std::max(settledGeneration_, completedGeneration);
                controlCv_.notify_all();
            }
            continue;
        }

        if (!childWindow && attachAttempts < 80) {
            WindowSearch search{process.dwProcessId, &windowTitle, nullptr};
            EnumWindows(FindProcessWindowProc, reinterpret_cast<LPARAM>(&search));
            ++attachAttempts;
            if (search.window) {
                AttachWindow(activeRequest->parent, search.window, latestBounds);
                if (IsWindow(search.window)) {
                    childWindow = search.window;
                    attached_ = true;
                    appliedBoundsGeneration = boundsGeneration;
                }
            }
        } else if (childWindow && boundsGeneration != appliedBoundsGeneration) {
            ApplyBounds(childWindow, latestBounds);
            appliedBoundsGeneration = boundsGeneration;
        }
        if (!childWindow) {
            // Bounds are applied when the foreign window is eventually found;
            // remember the version here so the worker does not spin meanwhile.
            appliedBoundsGeneration = boundsGeneration;
        }

        std::unique_lock lock(controlMutex_);
        controlCv_.wait_for(lock, std::chrono::milliseconds{25}, [this, generation = activeRequest->generation,
                                                                  appliedBoundsGeneration]() {
            return exitRequested_ || !desiredRunning_ || desiredGeneration_ != generation ||
                   boundsGeneration_ != appliedBoundsGeneration;
        });
    }

    retireActiveProcess(true);
}

}  // namespace anvil::app
