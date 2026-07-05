#include "AnvilPlayer/Playback/Log.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

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

InMemoryLogSink::InMemoryLogSink()
    : InMemoryLogSink(DefaultLogLevel()) {
}

InMemoryLogSink::InMemoryLogSink(const LogLevel minimumLevel)
    : minimumLevel_(minimumLevel) {
}

void InMemoryLogSink::Write(LogLevel level, std::wstring category, std::wstring message) {
    std::scoped_lock lock(mutex_);
    if (!ShouldLog(level, minimumLevel_)) {
        return;
    }

    const LogEntry entry{
        std::chrono::system_clock::now(),
        level,
        std::move(category),
        std::move(message),
    };
    entries_.push_back(entry);

    constexpr std::size_t maxBufferedEntries = 500;
    if (entries_.size() > maxBufferedEntries) {
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(entries_.size() - maxBufferedEntries));
    }

    WriteFileLocked(entry);
    if (debuggerOutputEnabled_) {
        const auto line = FormatEntry(entry) + L"\n";
        OutputDebugStringW(line.c_str());
    }
}

std::vector<LogEntry> InMemoryLogSink::Entries() const {
    std::scoped_lock lock(mutex_);
    return entries_;
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
    std::scoped_lock lock(mutex_);
    filePath_ = std::move(path);
}

std::filesystem::path InMemoryLogSink::FilePath() const {
    std::scoped_lock lock(mutex_);
    return filePath_;
}

void InMemoryLogSink::EnableDebuggerOutput(const bool enabled) {
    std::scoped_lock lock(mutex_);
    debuggerOutputEnabled_ = enabled;
}

void InMemoryLogSink::WriteFileLocked(const LogEntry& entry) {
    if (filePath_.empty()) {
        return;
    }

    std::error_code ignored;
    std::filesystem::create_directories(filePath_.parent_path(), ignored);
    const bool writeBom = !std::filesystem::exists(filePath_, ignored) ||
                          std::filesystem::file_size(filePath_, ignored) == 0;

    std::ofstream file(filePath_, std::ios::app | std::ios::binary);
    if (!file) {
        return;
    }
    if (writeBom) {
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        file.write(reinterpret_cast<const char*>(bom), sizeof(bom));
    }
    file << WideToUtf8(FormatEntry(entry)) << "\n";
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
