#pragma once

#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace anvil::playback {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
};

struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level = LogLevel::Info;
    std::wstring category;
    std::wstring message;
};

bool ShouldLog(LogLevel level, LogLevel minimumLevel);
LogLevel DefaultLogLevel();
LogLevel LogLevelFromString(const std::wstring& value, LogLevel fallback);
std::wstring ToDisplayString(LogLevel level);

class InMemoryLogSink {
public:
    InMemoryLogSink();
    explicit InMemoryLogSink(LogLevel minimumLevel);
    ~InMemoryLogSink();

    void Write(LogLevel level, std::wstring category, std::wstring message);
    // Waits for output accepted before this call to be processed; returns false on timeout.
    bool Flush(std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});
    std::vector<LogEntry> Entries() const;
    std::vector<LogEntry> LatestEntries(std::size_t count) const;
    std::wstring FormatLatest(std::size_t count) const;
    void Clear();
    void SetMinimumLevel(LogLevel level);
    LogLevel MinimumLevel() const;
    void SetFilePath(std::filesystem::path path);
    std::filesystem::path FilePath() const;
    void EnableDebuggerOutput(bool enabled);

private:
    struct AsyncWriter;

    mutable std::mutex mutex_;
    LogLevel minimumLevel_ = LogLevel::Info;
    std::filesystem::path filePath_;
    bool debuggerOutputEnabled_ = false;
    std::deque<LogEntry> entries_;
    std::unique_ptr<AsyncWriter> asyncWriter_;
};

}  // namespace anvil::playback
