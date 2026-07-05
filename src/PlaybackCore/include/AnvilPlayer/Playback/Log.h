#pragma once

#include <chrono>
#include <filesystem>
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

    void Write(LogLevel level, std::wstring category, std::wstring message);
    std::vector<LogEntry> Entries() const;
    std::wstring FormatLatest(std::size_t count) const;
    void Clear();
    void SetMinimumLevel(LogLevel level);
    LogLevel MinimumLevel() const;
    void SetFilePath(std::filesystem::path path);
    std::filesystem::path FilePath() const;
    void EnableDebuggerOutput(bool enabled);

private:
    void WriteFileLocked(const LogEntry& entry);

    mutable std::mutex mutex_;
    LogLevel minimumLevel_ = LogLevel::Info;
    std::filesystem::path filePath_;
    bool debuggerOutputEnabled_ = false;
    std::vector<LogEntry> entries_;
};

}  // namespace anvil::playback
