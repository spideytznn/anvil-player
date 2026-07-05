#pragma once

#include <chrono>
#include <string>

struct tagRECT;
using HWND = struct HWND__*;

namespace anvil::app {

// Converts a wide string to UTF-8. Returns an empty string on failure or empty input.
std::string WideToUtf8(const std::wstring& value);

// Converts a UTF-8 (or, on failure, ANSI) string to wide. Returns an empty string on failure.
std::wstring Utf8ToWide(const char* text);

// Formats an HRESULT as a lowercase hex wstring, e.g. 0x80004005 -> "80004005".
std::wstring HexHr(long hr);

// Resolves an FFmpeg error code into a human-readable wstring via av_strerror.
std::wstring FfmpegErrorString(int error);

// Shell-quotes a wide argument for CreateProcessW-style command lines.
std::wstring QuoteArgument(std::wstring value);

// Formats a duration as HH:MM:SS.mmm for ffmpeg/ffplay -ss arguments.
std::wstring FormatFfmpegSeekTime(std::chrono::milliseconds position);

}  // namespace anvil::app
