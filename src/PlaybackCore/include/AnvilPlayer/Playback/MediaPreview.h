#pragma once

#include <filesystem>
#include <string>

namespace anvil::playback {

struct MediaPreviewResult {
    bool generated = false;
    std::filesystem::path imagePath;
    std::wstring diagnostic;
};

class MediaPreview {
public:
    static MediaPreviewResult ExtractFirstFrame(const std::filesystem::path& mediaPath);
};

}  // namespace anvil::playback
