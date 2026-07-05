#include "AnvilPlayer/App/string_util.h"

#include <windows.h>

extern "C" {
#include <libavutil/error.h>
}

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace anvil::app {

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1,
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1,
                                            out.data(), needed, nullptr, nullptr);
    if (written <= 0) return {};
    out.resize(static_cast<std::size_t>(written - 1));
    return out;
}

std::wstring Utf8ToWide(const char* text) {
    if (!text || *text == '\0') return {};
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (needed <= 0) {
        codePage = CP_ACP;
        flags = 0;
        needed = MultiByteToWideChar(codePage, flags, text, -1, nullptr, 0);
    }
    if (needed <= 1) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(codePage, flags, text, -1, out.data(), needed);
    if (written <= 0) return {};
    out.resize(static_cast<std::size_t>(written - 1));
    return out;
}

std::wstring HexHr(const long hr) {
    std::wostringstream stream;
    stream << std::hex << static_cast<unsigned long>(hr);
    return stream.str();
}

std::wstring FfmpegErrorString(const int error) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(error, buffer, sizeof(buffer)) < 0) {
        std::wostringstream stream;
        stream << L"ffmpeg error " << error;
        return stream.str();
    }
    return Utf8ToWide(buffer);
}

std::wstring QuoteArgument(std::wstring value) {
    std::wstring quoted = L"\"";
    for (const wchar_t ch : value) {
        if (ch == L'"') {
            quoted += L"\\\"";
        } else {
            quoted += ch;
        }
    }
    quoted += L"\"";
    return quoted;
}

std::wstring FormatFfmpegSeekTime(const std::chrono::milliseconds position) {
    const auto totalMilliseconds = std::max<long long>(0, position.count());
    const auto hours = totalMilliseconds / 3'600'000;
    const auto minutes = (totalMilliseconds % 3'600'000) / 60'000;
    const auto seconds = (totalMilliseconds % 60'000) / 1000;
    const auto milliseconds = totalMilliseconds % 1000;

    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%02lld:%02lld:%02lld.%03lld", hours, minutes, seconds, milliseconds);
    return buffer;
}

}  // namespace anvil::app
