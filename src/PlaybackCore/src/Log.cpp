#include "AnvilPlayer/Playback/Log.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace anvil::playback {
namespace {

int Severity(const LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return 0;
    case LogLevel::Info:
        return 1;
    case LogLevel::Warning:
        return 2;
    case LogLevel::Error:
        return 3;
    }
    return 1;
}

std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring RedactUrlCredentials(std::wstring value) {
    std::size_t searchFrom = 0;
    while (searchFrom < value.size()) {
        const auto scheme = value.find(L"://", searchFrom);
        if (scheme == std::wstring::npos) {
            break;
        }

        const auto authorityBegin = scheme + 3;
        const auto authorityEnd = value.find_first_of(L"/?# \t\r\n\"'", authorityBegin);
        const auto end = authorityEnd == std::wstring::npos ? value.size() : authorityEnd;
        const auto at = value.rfind(L'@', end);
        if (at != std::wstring::npos && at >= authorityBegin && at < end) {
            value.replace(authorityBegin, at - authorityBegin + 1, L"[credentials]@");
            searchFrom = authorityBegin + std::wstring_view(L"[credentials]@").size();
        } else {
            searchFrom = end;
        }
    }

    constexpr std::array<std::wstring_view, 5> sensitiveParameters{
        L"api_key=", L"api-key=", L"access_token=", L"token=", L"password="};
    for (const auto parameter : sensitiveParameters) {
        searchFrom = 0;
        while (searchFrom < value.size()) {
            const auto lowercase = Lowercase(value);
            const auto key = lowercase.find(parameter, searchFrom);
            if (key == std::wstring::npos) {
                break;
            }
            if (key > 0 && value[key - 1] != L'?' && value[key - 1] != L'&' &&
                !std::iswspace(value[key - 1])) {
                searchFrom = key + parameter.size();
                continue;
            }
            const auto secretBegin = key + parameter.size();
            const auto secretEnd = value.find_first_of(L"& \t\r\n\"'", secretBegin);
            const auto end = secretEnd == std::wstring::npos ? value.size() : secretEnd;
            value.replace(secretBegin, end - secretBegin, L"[redacted]");
            searchFrom = secretBegin + std::wstring_view(L"[redacted]").size();
        }
    }
    return value;
}

std::string WideToUtf8(const std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::wstring TimestampText(const std::chrono::system_clock::time_point timestamp) {
    const auto time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm localTime{};
    localtime_s(&localTime, &time);

    const auto sinceEpoch = timestamp.time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch).count() % 1000;

    std::wostringstream stream;
    stream << std::put_time(&localTime, L"%Y-%m-%d %H:%M:%S")
           << L'.' << std::setw(3) << std::setfill(L'0') << milliseconds;
    return stream.str();
}

std::wstring FormatEntry(const LogEntry& entry) {
    std::wostringstream stream;
    stream << TimestampText(entry.timestamp)
           << L" [" << ToDisplayString(entry.level) << L"] "
           << entry.category << L": " << entry.message;
    return stream.str();
}

}  // namespace

struct InMemoryLogSink::AsyncWriter {
    enum class WorkKind {
        Entry,
        FilePathChanged,
    };

    struct WorkItem {
        std::uint64_t sequence = 0;
        WorkKind kind = WorkKind::Entry;
        LogEntry entry;
        std::filesystem::path filePath;
        bool debuggerOutputEnabled = false;
    };

    struct State {
        std::mutex mutex;
        std::condition_variable workAvailable;
        std::condition_variable progressChanged;
        std::deque<WorkItem> pending;
        std::uint64_t lastSubmitted = 0;
        std::uint64_t completedThrough = 0;
        bool stopping = false;
        bool workerCompleted = false;
    };

    struct FileTarget {
        void Select(std::filesystem::path newPath) {
            if (path == newPath) {
                return;
            }
            Close();
            path = std::move(newPath);
            retryAfter = {};
        }

        bool EnsureOpen() {
            if (path.empty()) {
                return false;
            }
            if (file.is_open()) {
                return true;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now < retryAfter) {
                return false;
            }

            std::error_code ignored;
            const auto parent = path.parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, ignored);
            }

            ignored.clear();
            const bool exists = std::filesystem::exists(path, ignored);
            bool writeBom = !exists;
            if (exists && !ignored) {
                const auto size = std::filesystem::file_size(path, ignored);
                writeBom = !ignored && size == 0;
            }

            file.clear();
            file.open(path, std::ios::app | std::ios::binary);
            if (!file) {
                file.close();
                file.clear();
                retryAfter = now + retryDelay;
                return false;
            }

            if (writeBom) {
                constexpr unsigned char bom[] = {0xEF, 0xBB, 0xBF};
                file.write(reinterpret_cast<const char*>(bom), sizeof(bom));
            }
            return true;
        }

        void Write(const std::string& line) {
            if (!EnsureOpen()) {
                return;
            }
            file.write(line.data(), static_cast<std::streamsize>(line.size()));
            file.put('\n');
            if (!file) {
                Close();
                retryAfter = std::chrono::steady_clock::now() + retryDelay;
            }
        }

        void Flush() {
            if (!file.is_open()) {
                return;
            }
            file.flush();
            if (!file) {
                Close();
                retryAfter = std::chrono::steady_clock::now() + retryDelay;
            }
        }

        void Close() {
            if (file.is_open()) {
                file.flush();
                file.close();
            }
            file.clear();
        }

        static constexpr std::chrono::seconds retryDelay{1};
        std::filesystem::path path;
        std::ofstream file;
        std::chrono::steady_clock::time_point retryAfter{};
    };

    AsyncWriter()
        : state_(std::make_shared<State>()) {
    }

    ~AsyncWriter() {
        Shutdown();
    }

    void Enqueue(LogEntry entry,
                 std::filesystem::path filePath,
                 const bool debuggerOutputEnabled) {
        WorkItem item;
        item.kind = WorkKind::Entry;
        item.entry = std::move(entry);
        item.filePath = std::move(filePath);
        item.debuggerOutputEnabled = debuggerOutputEnabled;
        Enqueue(std::move(item));
    }

    void FilePathChanged(std::filesystem::path filePath) {
        WorkItem item;
        item.kind = WorkKind::FilePathChanged;
        item.filePath = std::move(filePath);
        Enqueue(std::move(item));
    }

    bool Flush(const std::chrono::milliseconds timeout) {
        std::uint64_t target = 0;
        {
            std::scoped_lock lock(state_->mutex);
            target = state_->lastSubmitted;
            if (state_->completedThrough >= target) {
                return true;
            }
        }

        EnsureWorkerStarted();
        std::unique_lock lock(state_->mutex);
        const auto boundedTimeout = std::max(timeout, std::chrono::milliseconds{0});
        state_->progressChanged.wait_for(lock, boundedTimeout, [&] {
            return state_->completedThrough >= target || state_->workerCompleted;
        });
        return state_->completedThrough >= target;
    }

private:
    static constexpr std::size_t maxPendingEntries = 2048;
    static constexpr std::size_t maxBatchEntries = 128;
    static constexpr std::chrono::milliseconds coalesceDelay{4};
    static constexpr std::chrono::milliseconds shutdownDrainTimeout{200};
    static constexpr std::chrono::milliseconds cancellationGraceTimeout{50};

    void Enqueue(WorkItem item) {
        {
            std::scoped_lock lock(state_->mutex);
            if (state_->stopping) {
                return;
            }

            item.sequence = ++state_->lastSubmitted;
            if (state_->pending.size() >= maxPendingEntries) {
                // Keep producer latency and memory fixed; recent diagnostics are more useful
                // than stale entries when the storage target is slower than the producers.
                state_->pending.pop_front();
            }
            state_->pending.push_back(std::move(item));
        }

        EnsureWorkerStarted();
        state_->workAvailable.notify_one();
    }

    bool EnsureWorkerStarted() {
        std::scoped_lock threadLock(threadMutex_);
        if (worker_.joinable()) {
            return true;
        }
        {
            std::scoped_lock stateLock(state_->mutex);
            if (state_->stopping) {
                return false;
            }
        }

        try {
            worker_ = std::thread(&AsyncWriter::WorkerLoop, state_);
        } catch (const std::system_error&) {
            return false;
        }
        return true;
    }

    static void WorkerLoop(const std::shared_ptr<State> state) {
        {
            FileTarget fileTarget;
            std::vector<WorkItem> batch;
            batch.reserve(maxBatchEntries);

            while (true) {
                batch.clear();
                {
                    std::unique_lock lock(state->mutex);
                    state->workAvailable.wait(lock, [&] {
                        return state->stopping || !state->pending.empty();
                    });
                    if (state->pending.empty() && state->stopping) {
                        break;
                    }

                    if (!state->stopping && state->pending.size() < maxBatchEntries) {
                        state->workAvailable.wait_for(lock, coalesceDelay, [&] {
                            return state->stopping || state->pending.size() >= maxBatchEntries;
                        });
                    }

                    const auto count = std::min(maxBatchEntries, state->pending.size());
                    for (std::size_t index = 0; index < count; ++index) {
                        batch.push_back(std::move(state->pending.front()));
                        state->pending.pop_front();
                    }
                }

                for (const auto& item : batch) {
                    fileTarget.Select(item.filePath);
                    if (item.kind == WorkKind::FilePathChanged) {
                        continue;
                    }

                    const auto formatted = FormatEntry(item.entry);
                    if (item.debuggerOutputEnabled) {
                        const auto debuggerLine = formatted + L"\n";
                        OutputDebugStringW(debuggerLine.c_str());
                    }
                    if (!item.filePath.empty()) {
                        fileTarget.Write(WideToUtf8(formatted));
                    }
                }
                fileTarget.Flush();

                {
                    std::scoped_lock lock(state->mutex);
                    for (const auto& item : batch) {
                        // FIFO processing means every earlier sequence was either written or
                        // evicted from the bounded queue.
                        state->completedThrough = std::max(state->completedThrough, item.sequence);
                    }
                }
                state->progressChanged.notify_all();
            }

            fileTarget.Close();
        }

        {
            std::scoped_lock lock(state->mutex);
            state->workerCompleted = true;
        }
        state->progressChanged.notify_all();
    }

    void Shutdown() noexcept {
        bool workRemains = false;
        {
            std::scoped_lock lock(state_->mutex);
            workRemains = state_->completedThrough < state_->lastSubmitted;
        }
        if (workRemains) {
            EnsureWorkerStarted();
        }

        bool hasWorker = false;
        {
            std::scoped_lock lock(threadMutex_);
            hasWorker = worker_.joinable();
        }
        {
            std::scoped_lock lock(state_->mutex);
            state_->stopping = true;
            if (!hasWorker) {
                state_->pending.clear();
                state_->completedThrough = state_->lastSubmitted;
                state_->workerCompleted = true;
            }
        }
        state_->workAvailable.notify_all();
        state_->progressChanged.notify_all();

        if (!hasWorker) {
            return;
        }

        bool completed = false;
        {
            std::unique_lock lock(state_->mutex);
            completed = state_->progressChanged.wait_for(lock, shutdownDrainTimeout, [&] {
                return state_->workerCompleted;
            });
        }

        if (!completed) {
            {
                std::scoped_lock lock(state_->mutex);
                state_->pending.clear();
                state_->completedThrough = state_->lastSubmitted;
            }
            {
                std::scoped_lock lock(threadMutex_);
                if (worker_.joinable()) {
                    CancelSynchronousIo(worker_.native_handle());
                }
            }
            std::unique_lock lock(state_->mutex);
            completed = state_->progressChanged.wait_for(lock, cancellationGraceTimeout, [&] {
                return state_->workerCompleted;
            });
        }

        std::scoped_lock lock(threadMutex_);
        if (!worker_.joinable()) {
            return;
        }
        if (completed) {
            worker_.join();
        } else {
            // WorkerLoop owns only shared state, so a blocked filesystem call can safely
            // finish after the sink has gone away without holding up the caller.
            worker_.detach();
        }
    }

    std::shared_ptr<State> state_;
    std::mutex threadMutex_;
    std::thread worker_;
};

InMemoryLogSink::InMemoryLogSink()
    : InMemoryLogSink(DefaultLogLevel()) {
}

InMemoryLogSink::InMemoryLogSink(const LogLevel minimumLevel)
    : minimumLevel_(minimumLevel), asyncWriter_(std::make_unique<AsyncWriter>()) {
}

InMemoryLogSink::~InMemoryLogSink() = default;

bool InMemoryLogSink::Flush(const std::chrono::milliseconds timeout) {
    return asyncWriter_->Flush(timeout);
}

void InMemoryLogSink::Write(LogLevel level, std::wstring category, std::wstring message) {
    LogEntry entry;
    std::filesystem::path filePath;
    bool debuggerOutputEnabled = false;
    {
        std::scoped_lock lock(mutex_);
        if (!ShouldLog(level, minimumLevel_)) {
            return;
        }

        entry = LogEntry{
            std::chrono::system_clock::now(),
            level,
            RedactUrlCredentials(std::move(category)),
            RedactUrlCredentials(std::move(message)),
        };
        entries_.push_back(entry);

        constexpr std::size_t maxBufferedEntries = 500;
        if (entries_.size() > maxBufferedEntries) {
            entries_.pop_front();
        }
        filePath = filePath_;
        debuggerOutputEnabled = debuggerOutputEnabled_;
    }

    if (!filePath.empty() || debuggerOutputEnabled) {
        asyncWriter_->Enqueue(std::move(entry), std::move(filePath), debuggerOutputEnabled);
    }
}

std::vector<LogEntry> InMemoryLogSink::Entries() const {
    std::scoped_lock lock(mutex_);
    return {entries_.begin(), entries_.end()};
}

std::vector<LogEntry> InMemoryLogSink::LatestEntries(const std::size_t count) const {
    std::scoped_lock lock(mutex_);
    const std::size_t start = entries_.size() > count ? entries_.size() - count : 0;
    return {entries_.begin() + static_cast<std::ptrdiff_t>(start), entries_.end()};
}

std::wstring InMemoryLogSink::FormatLatest(const std::size_t count) const {
    std::scoped_lock lock(mutex_);
    std::wostringstream stream;
    const auto start = entries_.size() > count ? entries_.size() - count : 0;
    for (std::size_t index = start; index < entries_.size(); ++index) {
        const auto& entry = entries_[index];
        stream << TimestampText(entry.timestamp)
               << L" [" << ToDisplayString(entry.level) << L"] "
               << entry.category << L": " << entry.message;
        if (index + 1 < entries_.size()) {
            stream << L'\n';
        }
    }
    return stream.str();
}

void InMemoryLogSink::Clear() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
}

void InMemoryLogSink::SetMinimumLevel(const LogLevel level) {
    std::scoped_lock lock(mutex_);
    minimumLevel_ = level;
}

LogLevel InMemoryLogSink::MinimumLevel() const {
    std::scoped_lock lock(mutex_);
    return minimumLevel_;
}

void InMemoryLogSink::SetFilePath(std::filesystem::path path) {
    {
        std::scoped_lock lock(mutex_);
        filePath_ = path;
    }
    asyncWriter_->FilePathChanged(std::move(path));
}

std::filesystem::path InMemoryLogSink::FilePath() const {
    std::scoped_lock lock(mutex_);
    return filePath_;
}

void InMemoryLogSink::EnableDebuggerOutput(const bool enabled) {
    std::scoped_lock lock(mutex_);
    debuggerOutputEnabled_ = enabled;
}

bool ShouldLog(const LogLevel level, const LogLevel minimumLevel) {
    return Severity(level) >= Severity(minimumLevel);
}

LogLevel DefaultLogLevel() {
#if defined(_DEBUG)
    return LogLevel::Debug;
#else
    return LogLevel::Info;
#endif
}

LogLevel LogLevelFromString(const std::wstring& value, const LogLevel fallback) {
    const auto normalized = Lowercase(value);
    if (normalized == L"debug" || normalized == L"trace" || normalized == L"verbose") {
        return LogLevel::Debug;
    }
    if (normalized == L"info" || normalized == L"information") {
        return LogLevel::Info;
    }
    if (normalized == L"warn" || normalized == L"warning") {
        return LogLevel::Warning;
    }
    if (normalized == L"error" || normalized == L"err") {
        return LogLevel::Error;
    }
    return fallback;
}

std::wstring ToDisplayString(const LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return L"debug";
    case LogLevel::Info:
        return L"info";
    case LogLevel::Warning:
        return L"warn";
    case LogLevel::Error:
        return L"error";
    }
    return L"unknown";
}

}  // namespace anvil::playback
