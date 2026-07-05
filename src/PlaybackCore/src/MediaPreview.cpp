#include "AnvilPlayer/Playback/MediaPreview.h"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>

namespace anvil::playback {
namespace {

std::wstring QuoteForCommand(std::wstring value) {
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

std::filesystem::path PreviewPathFor(const std::filesystem::path& mediaPath) {
    const auto hash = std::hash<std::wstring>{}(mediaPath.wstring());
    auto directory = std::filesystem::temp_directory_path() / L"anvil-player";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory / (L"preview-" + std::to_wstring(static_cast<unsigned long long>(hash)) + L".bmp");
}

bool HasUsableFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) &&
           std::filesystem::is_regular_file(path, error) &&
           std::filesystem::file_size(path, error) > 0;
}

}  // namespace

MediaPreviewResult MediaPreview::ExtractFirstFrame(const std::filesystem::path& mediaPath) {
    if (_wsystem(L"ffmpeg -version >NUL 2>NUL") != 0) {
        return MediaPreviewResult{false, {}, L"ffmpeg unavailable"};
    }

    const auto outputPath = PreviewPathFor(mediaPath);
    const std::wstring command =
        L"ffmpeg -hide_banner -loglevel error -y "
        L"-ss 00:00:00.250 -i " +
        QuoteForCommand(mediaPath.wstring()) +
        L" -an -frames:v 1 -vf scale=960:-2 -f image2 " +
        QuoteForCommand(outputPath.wstring()) +
        L" >NUL 2>NUL";

    const int exitCode = _wsystem(command.c_str());
    if (exitCode != 0 || !HasUsableFile(outputPath)) {
        std::error_code ignored;
        std::filesystem::remove(outputPath, ignored);
        return MediaPreviewResult{false, {}, L"ffmpeg preview extraction failed"};
    }

    return MediaPreviewResult{true, outputPath, L""};
}

}  // namespace anvil::playback
