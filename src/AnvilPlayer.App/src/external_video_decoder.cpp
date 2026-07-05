#include "AnvilPlayer/App/external_video_decoder.h"

#include "AnvilPlayer/App/string_util.h"

#include <algorithm>

namespace anvil::app {

ExternalVideoDecoder::~ExternalVideoDecoder() {
    Stop();
}

bool ExternalVideoDecoder::Start(const std::filesystem::path& mediaPath,
                                 const std::chrono::milliseconds startPosition,
                                 HWND notificationWindow,
                                 const UINT notificationMessage) {
    Stop();
    if (mediaPath.empty()) {
        return false;
    }

    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &securityAttributes, 0)) {
        return false;
    }
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

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

    PROCESS_INFORMATION processInfo{};
    const std::wstring filter =
        L"scale=960:540:force_original_aspect_ratio=decrease,"
        L"pad=960:540:(ow-iw)/2:(oh-ih)/2";
    std::wstring commandLine =
        L"ffmpeg -hide_banner -loglevel error -nostdin -re -ss " +
        FormatFfmpegSeekTime(startPosition) +
        L" -i " + QuoteArgument(mediaPath.wstring()) +
        L" -an -vf " + QuoteArgument(filter) +
        L" -pix_fmt bgra -f rawvideo pipe:1";
    lastCommandLine_ = commandLine;

    const BOOL created = CreateProcessW(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);

    CloseHandle(stdoutWrite);
    if (nulWrite && nulWrite != INVALID_HANDLE_VALUE) {
        CloseHandle(nulWrite);
    }

    if (!created) {
        CloseHandle(stdoutRead);
        return false;
    }

    {
        std::scoped_lock lock(mutex_);
        frame_.pixels.reset();
        frame_.serial = 0;
    }

    stdoutRead_ = stdoutRead;
    process_ = processInfo;
    notificationWindow_.store(notificationWindow);
    notificationMessage_.store(notificationMessage);
    frameMessagePending_.store(false);
    stopping_ = false;
    running_ = true;
    readerThread_ = std::thread([this]() {
        ReaderLoop();
    });
    return true;
}

void ExternalVideoDecoder::Stop() {
    stopping_ = true;
    notificationWindow_.store(nullptr);
    notificationMessage_.store(0);
    frameMessagePending_.store(false);
    if (process_.hProcess) {
        TerminateProcess(process_.hProcess, 0);
        WaitForSingleObject(process_.hProcess, 2000);
    }
    if (readerThread_.joinable()) {
        readerThread_.join();
    }
    if (stdoutRead_) {
        CloseHandle(stdoutRead_);
        stdoutRead_ = nullptr;
    }
    if (process_.hThread) {
        CloseHandle(process_.hThread);
        process_.hThread = nullptr;
    }
    if (process_.hProcess) {
        CloseHandle(process_.hProcess);
        process_.hProcess = nullptr;
    }
    running_ = false;
}

bool ExternalVideoDecoder::LatestFrame(VideoFrame& frame) const {
    std::scoped_lock lock(mutex_);
    if (!frame_.HasPixels()) {
        return false;
    }
    frame = frame_;
    return true;
}

void ExternalVideoDecoder::ClearFrame() {
    std::scoped_lock lock(mutex_);
    frame_.pixels.reset();
    frame_.serial = 0;
}

void ExternalVideoDecoder::AcknowledgeFrameNotification() {
    frameMessagePending_.store(false);
}

void ExternalVideoDecoder::NotifyFrameReady() {
    const HWND window = notificationWindow_.load();
    const UINT message = notificationMessage_.load();
    if (!window || message == 0 || frameMessagePending_.exchange(true)) {
        return;
    }
    PostMessageW(window, message, 0, 0);
}

void ExternalVideoDecoder::ReaderLoop() {
    constexpr std::size_t frameSize =
        static_cast<std::size_t>(VideoFrame::Width) *
        static_cast<std::size_t>(VideoFrame::Height) *
        static_cast<std::size_t>(VideoFrame::BytesPerPixel);
    std::vector<unsigned char> buffer(frameSize);
    std::uint64_t serial = 0;

    while (!stopping_) {
        std::size_t filled = 0;
        while (filled < frameSize && !stopping_) {
            DWORD read = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(frameSize - filled, 64 * 1024));
            if (!ReadFile(stdoutRead_, buffer.data() + filled, chunk, &read, nullptr) || read == 0) {
                running_ = false;
                return;
            }
            filled += read;
        }
        if (filled != frameSize) {
            break;
        }

        auto pixels = std::make_shared<std::vector<unsigned char>>(std::move(buffer));
        {
            std::scoped_lock lock(mutex_);
            frame_.pixels = std::move(pixels);
            frame_.serial = ++serial;
        }
        NotifyFrameReady();
        buffer = std::vector<unsigned char>(frameSize);
    }
    running_ = false;
}

}  // namespace anvil::app
