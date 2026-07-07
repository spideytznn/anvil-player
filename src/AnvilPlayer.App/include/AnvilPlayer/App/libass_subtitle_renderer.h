#pragma once

#include "AnvilPlayer/App/ffmpeg_video_decoder.h"
#include "AnvilPlayer/App/log_sink_ptr.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace anvil::app {

class LibassSubtitleRenderer {
public:
    explicit LibassSubtitleRenderer(LogSinkPtr logSink = nullptr);
    ~LibassSubtitleRenderer();

    LibassSubtitleRenderer(const LibassSubtitleRenderer&) = delete;
    LibassSubtitleRenderer& operator=(const LibassSubtitleRenderer&) = delete;

    bool Initialize(const std::filesystem::path& searchRoot = {});
    bool IsAvailable() const;

    void ResetTrack();
    void FlushEvents();
    void ResetRenderCache();

    bool AddFont(const std::string& name, const uint8_t* data, int size);
    bool ConfigureTrackFromCodecPrivate(const uint8_t* data, int size);
    bool ConfigureTrackFromMemory(const uint8_t* data, std::size_t size);
    bool ProcessPacket(const uint8_t* data,
                       int size,
                       std::chrono::milliseconds pts,
                       std::chrono::milliseconds duration);

    std::vector<NativeSubtitleBitmap> Render(std::chrono::milliseconds pts,
                                             int frameWidth,
                                             int frameHeight,
                                             uint64_t& serial);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anvil::app
