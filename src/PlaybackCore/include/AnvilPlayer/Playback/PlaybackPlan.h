#pragma once

#include "AnvilPlayer/Playback/CapabilityReport.h"
#include "AnvilPlayer/Playback/Settings.h"
#include "AnvilPlayer/Playback/Types.h"

#include <string>

namespace anvil::playback {

struct PlaybackPlan {
    std::wstring videoDecoder;
    std::wstring videoMode;
    std::wstring videoReason;
    std::wstring audioOutput;
    std::wstring audioReason;
};

class PlaybackPlanner {
public:
    static PlaybackPlan Build(const MediaDescriptor& media,
                              const PlayerSettings& settings,
                              const CapabilityReport& capabilities);
};

}  // namespace anvil::playback
