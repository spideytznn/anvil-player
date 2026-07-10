#include "AnvilPlayer/App/external_video_decoder.h"

#include "AnvilPlayer/App/string_util.h"

#include <algorithm>
#include <system_error>

namespace anvil::app {

namespace {

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

void CloseHandleIfValid(HANDLE& handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    handle = nullptr;
}

}  // namespace

ExternalVideoDecoder::~ExternalVideoDecoder() {
    // The owning MainWindow is destroyed on its background reaper. Waiting for
    // the control worker here is safe and prevents access after destruction.
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

bool ExternalVideoDecoder::EnsureControlWorkerLocked() {
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

bool ExternalVideoDecoder::Start(const std::filesystem::path& mediaPath,
                                 const std::chrono::milliseconds startPosition,
                                 HWND notificationWindow,
                                 const UINT notificationMessage,
                                 const std::uint64_t notificationCookie) {
    if (mediaPath.empty()) {
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
            mediaPath,
            startPosition,
            notificationWindow,
            notificationMessage,
            notificationCookie,
        };
        desiredRunning_ = true;
        running_ = true;
        frameMessagePending_ = false;
    }
    controlCv_.notify_all();
    return true;
}

std::uint64_t ExternalVideoDecoder::RequestStopLocked() {
    const std::uint64_t generation = ++desiredGeneration_;
    desiredRunning_ = false;
    pendingStart_.reset();
    frameMessagePending_ = false;
    if (!workerStarted_) {
        settledGeneration_ = generation;
        running_ = false;
    }
    return generation;
}

void ExternalVideoDecoder::RequestStop() {
    {
        std::scoped_lock lock(controlMutex_);
        RequestStopLocked();
    }
    // The control worker owns pipe cancellation and child termination. The
    // caller only publishes the new generation and wakes that worker.
    controlCv_.notify_all();
}

void ExternalVideoDecoder::Stop() {
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

bool ExternalVideoDecoder::LatestFrame(VideoFrame& frame) const {
    std::scoped_lock lock(frameMutex_);
    if (!frame_.HasPixels()) {
        return false;
    }
    frame = frame_;
    return true;
}

void ExternalVideoDecoder::ClearFrame() {
    std::scoped_lock lock(frameMutex_);
    frame_.pixels.reset();
    frame_.serial = 0;
}

void ExternalVideoDecoder::AcknowledgeFrameNotification() {
    frameMessagePending_ = false;
}

std::wstring ExternalVideoDecoder::LastCommandLine() const {
    std::scoped_lock lock(commandLineMutex_);
    return lastCommandLine_;
}

void ExternalVideoDecoder::NotifyFrameReady(const StartRequest& request) {
    bool requestStillCurrent = false;
    {
        std::scoped_lock lock(controlMutex_);
        requestStillCurrent = !exitRequested_ && desiredRunning_ &&
                              desiredGeneration_ == request.generation;
    }
    if (!requestStillCurrent || !request.notificationWindow || request.notificationMessage == 0 ||
        frameMessagePending_.exchange(true)) {
        return;
    }
    if (!PostMessageW(request.notificationWindow,
                      request.notificationMessage,
                      static_cast<WPARAM>(request.notificationCookie),
                      0)) {
        frameMessagePending_ = false;
    }
}

void ExternalVideoDecoder::ControlLoop() {
    PROCESS_INFORMATION process{};
    HANDLE stdoutRead = nullptr;
    std::optional<StartRequest> activeRequest;
    std::thread readerThread;
    std::atomic_bool readerDone = true;

    auto retireActiveProcess = [&](const bool terminate) {
        if (process.hProcess) {
            if (terminate && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process.hProcess, 0);
            }
        }
        // Wake a synchronous ReadFile before waiting for its issuing thread.
        // Terminating the child closes its writers; these explicit cancellation
        // calls also cover inherited writers or a pipe implementation change.
        if (readerThread.joinable()) {
            CancelSynchronousIo(readerThread.native_handle());
            if (stdoutRead) {
                CancelIoEx(stdoutRead, nullptr);
            }
            readerThread.join();
        }
        if (process.hProcess && terminate) {
            WaitForSingleObject(process.hProcess, 2000);
        }
        CloseHandleIfValid(stdoutRead);
        CloseProcessHandles(process);
        activeRequest.reset();
        readerDone = true;
        frameMessagePending_ = false;
    };

    auto failCurrentRequest = [&](const std::uint64_t generation) {
        std::scoped_lock lock(controlMutex_);
        if (desiredGeneration_ == generation) {
            desiredRunning_ = false;
            running_ = false;
            settledGeneration_ = std::max(settledGeneration_, generation);
            controlCv_.notify_all();
        }
    };

    for (;;) {
        bool exitRequested = false;
        bool desiredRunning = false;
        std::uint64_t desiredGeneration = 0;
        {
            std::scoped_lock lock(controlMutex_);
            exitRequested = exitRequested_;
            desiredRunning = desiredRunning_;
            desiredGeneration = desiredGeneration_;
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
                    settledGeneration_ = std::max(settledGeneration_, desiredGeneration_);
                    controlCv_.notify_all();
                    break;
                }
                if (!desiredRunning_) {
                    running_ = false;
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

            SECURITY_ATTRIBUTES securityAttributes{};
            securityAttributes.nLength = sizeof(securityAttributes);
            securityAttributes.bInheritHandle = TRUE;

            HANDLE createdRead = nullptr;
            HANDLE stdoutWrite = nullptr;
            if (!CreatePipe(&createdRead, &stdoutWrite, &securityAttributes, 0)) {
                failCurrentRequest(request->generation);
                continue;
            }
            SetHandleInformation(createdRead, HANDLE_FLAG_INHERIT, 0);

            HANDLE nulWrite = CreateFileW(L"NUL",
                                           GENERIC_WRITE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           &securityAttributes,
                                           OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL,
                                           nullptr);
            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            startupInfo.dwFlags = STARTF_USESTDHANDLES;
            startupInfo.hStdOutput = stdoutWrite;
            startupInfo.hStdError = nulWrite != INVALID_HANDLE_VALUE ? nulWrite : stdoutWrite;
            startupInfo.hStdInput = nullptr;

            const std::wstring filter =
                L"scale=960:540:force_original_aspect_ratio=decrease,"
                L"pad=960:540:(ow-iw)/2:(oh-ih)/2";
            std::wstring commandLine =
                L"ffmpeg -hide_banner -loglevel error -nostdin -re -ss " +
                FormatFfmpegSeekTime(request->startPosition) +
                L" -i " + QuoteArgument(request->mediaPath.wstring()) +
                L" -an -vf " + QuoteArgument(filter) +
                L" -pix_fmt bgra -f rawvideo pipe:1";
            {
                std::scoped_lock lock(commandLineMutex_);
                lastCommandLine_ = commandLine;
            }

            PROCESS_INFORMATION createdProcess{};
            const BOOL created = CreateProcessW(nullptr,
                                                commandLine.data(),
                                                nullptr,
                                                nullptr,
                                                TRUE,
                                                CREATE_NO_WINDOW,
                                                nullptr,
                                                nullptr,
                                                &startupInfo,
                                                &createdProcess);
            CloseHandleIfValid(stdoutWrite);
            CloseHandleIfValid(nulWrite);

            bool requestStillCurrent = false;
            {
                std::scoped_lock lock(controlMutex_);
                requestStillCurrent = !exitRequested_ && desiredRunning_ &&
                                      desiredGeneration_ == request->generation;
            }
            if (!created || !requestStillCurrent) {
                CloseHandleIfValid(createdRead);
                if (created) {
                    if (WaitForSingleObject(createdProcess.hProcess, 0) == WAIT_TIMEOUT) {
                        TerminateProcess(createdProcess.hProcess, 0);
                    }
                    WaitForSingleObject(createdProcess.hProcess, 2000);
                    CloseProcessHandles(createdProcess);
                } else {
                    failCurrentRequest(request->generation);
                }
                continue;
            }

            {
                std::shared_ptr<const std::vector<unsigned char>> retiredPixels;
                std::scoped_lock lock(frameMutex_);
                retiredPixels = std::move(frame_.pixels);
                frame_.serial = 0;
            }
            process = createdProcess;
            stdoutRead = createdRead;
            activeRequest = std::move(request);
            readerDone = false;
            try {
                readerThread = std::thread([this, request = *activeRequest, createdRead, &readerDone]() mutable {
                    ReaderLoop(std::move(request), createdRead, readerDone);
                });
            } catch (const std::exception&) {
                const std::uint64_t generation = activeRequest->generation;
                readerDone = true;
                retireActiveProcess(true);
                failCurrentRequest(generation);
                continue;
            }
            running_ = true;
            continue;
        }

        const bool processExited = WaitForSingleObject(process.hProcess, 0) != WAIT_TIMEOUT;
        if (processExited || readerDone.load()) {
            const std::uint64_t generation = activeRequest->generation;
            retireActiveProcess(!processExited);
            failCurrentRequest(generation);
            continue;
        }

        std::unique_lock lock(controlMutex_);
        controlCv_.wait_for(lock,
                            std::chrono::milliseconds{25},
                            [this, generation = activeRequest->generation, &readerDone]() {
                                return exitRequested_ || !desiredRunning_ || desiredGeneration_ != generation ||
                                       readerDone.load();
                            });
    }

    retireActiveProcess(true);
}

void ExternalVideoDecoder::ReaderLoop(StartRequest request,
                                      HANDLE stdoutRead,
                                      std::atomic_bool& readerDone) {
    constexpr std::size_t frameSize =
        static_cast<std::size_t>(VideoFrame::Width) *
        static_cast<std::size_t>(VideoFrame::Height) *
        static_cast<std::size_t>(VideoFrame::BytesPerPixel);

    try {
        std::vector<unsigned char> buffer(frameSize);
        std::uint64_t serial = 0;
        while (true) {
            bool requestStillCurrent = false;
            {
                std::scoped_lock lock(controlMutex_);
                requestStillCurrent = !exitRequested_ && desiredRunning_ &&
                                      desiredGeneration_ == request.generation;
            }
            if (!requestStillCurrent) {
                break;
            }

            std::size_t filled = 0;
            while (filled < frameSize) {
                const DWORD chunk = static_cast<DWORD>(
                    std::min<std::size_t>(frameSize - filled, 64u * 1024u));
                DWORD read = 0;
                if (!ReadFile(stdoutRead, buffer.data() + filled, chunk, &read, nullptr) || read == 0) {
                    readerDone = true;
                    controlCv_.notify_all();
                    return;
                }
                filled += read;

                {
                    std::scoped_lock lock(controlMutex_);
                    requestStillCurrent = !exitRequested_ && desiredRunning_ &&
                                          desiredGeneration_ == request.generation;
                }
                if (!requestStillCurrent) {
                    readerDone = true;
                    controlCv_.notify_all();
                    return;
                }
            }

            auto pixels = std::make_shared<std::vector<unsigned char>>(std::move(buffer));
            {
                std::scoped_lock lock(frameMutex_);
                frame_.pixels = std::move(pixels);
                frame_.serial = ++serial;
            }
            NotifyFrameReady(request);
            buffer = std::vector<unsigned char>(frameSize);
        }
    } catch (const std::bad_alloc&) {
        // The control worker observes readerDone and exposes the launch/runtime
        // failure through IsRunning(), which the existing watchdog already uses.
    }
    readerDone = true;
    controlCv_.notify_all();
}

}  // namespace anvil::app
