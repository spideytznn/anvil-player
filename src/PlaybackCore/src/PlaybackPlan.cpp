#include "AnvilPlayer/Playback/PlaybackPlan.h"

#include <algorithm>

namespace anvil::playback {
namespace {

bool Contains(const std::wstring& value, const std::wstring& needle) {
    return value.find(needle) != std::wstring::npos;
}

bool IsD3D12VaCandidate(const std::wstring& codec) {
    return Contains(codec, L"H.264") ||
           Contains(codec, L"HEVC") ||
           Contains(codec, L"AV1") ||
           Contains(codec, L"VP9");
}

bool IsBitstreamCandidate(const std::wstring& codec) {
    return codec == L"AC-3" ||
           codec == L"E-AC-3" ||
           codec == L"TrueHD" ||
           codec == L"DTS";
}

bool IsBitstreamEnabled(const std::wstring& codec, const AudioSettings& settings) {
    if (codec == L"AC-3") return settings.ac3Passthrough;
    if (codec == L"E-AC-3") return settings.eac3Passthrough;
    if (codec == L"TrueHD") return settings.trueHdPassthrough;
    if (codec == L"DTS") return settings.dtsPassthrough || settings.dtsHdPassthrough;
    return false;
}

std::wstring ChooseVideoDecoder(const MediaDescriptor& media, const VideoSettings& settings, std::wstring& reason) {
    if (!media.hasVideo) {
        reason = L"no_video_stream";
        return L"none";
    }
    if (settings.hardwareDecode == HardwareDecodeMode::Off) {
        reason = L"hardware_decode_disabled";
        return L"ffmpeg_software";
    }
    if (settings.hardwareDecode == HardwareDecodeMode::D3D12VA && !IsD3D12VaCandidate(media.videoCodec)) {
        reason = L"requested_d3d12va_but_codec_not_candidate";
        return L"ffmpeg_software";
    }
    if (IsD3D12VaCandidate(media.videoCodec)) {
        reason = L"hardware_decode_candidate";
        return L"ffmpeg_d3d12va";
    }
    reason = L"software_decode_compatibility_path";
    return L"ffmpeg_software";
}

std::wstring ChooseVideoMode(const MediaDescriptor& media, const VideoSettings& settings, const CapabilityReport& capabilities) {
    if (!media.hasVideo) {
        return L"none";
    }
    if (media.dolbyVisionDetected) {
        if (settings.dolbyVision == DolbyVisionMode::Off) {
            // DV reshaping disabled by user: treat as plain HDR10 (works for
            // Profile 7 BL; Profile 5/8 will be purple but that is expected
            // when explicitly opting out).
            return L"dolby_vision_disabled_tone_map";
        }
        if (settings.dolbyVisionSystemPipelineExperimental &&
            capabilities.codecs.dolbyVisionExtensionDetected) {
            return L"dolby_vision_system_extensions";
        }
        // Software-extracted RPU reshaping on the GPU, output as HDR10 PQ
        // (HDR display) or tone-mapped SDR.
        return L"dolby_vision_software_reshape";
    }
    if (media.videoColor.IsHdr() || Contains(media.hdrFormat, L"HDR") || Contains(media.hdrFormat, L"HLG") || Contains(media.hdrFormat, L"PQ")) {
        if (settings.hdrOutput == HdrOutputMode::ForceSdr || !capabilities.display.hdrEnabled) {
            return L"tone_mapped_sdr";
        }
        return media.videoColor.transfer == VideoTransferCharacteristic::Hlg ? L"hlg_output" : L"hdr10_output";
    }
    return L"sdr_output";
}

std::wstring ChooseAudioOutput(const MediaDescriptor& media, const AudioSettings& settings, std::wstring& reason) {
    if (!media.hasAudio) {
        reason = L"no_audio_stream";
        return L"none";
    }
    const bool wantsPassthrough = settings.passthroughPreferred ||
                                  settings.outputMode == AudioOutputMode::Passthrough;

    if (!wantsPassthrough) {
        reason = L"default_pcm_path";
        return settings.wasapiMode == WasapiMode::Exclusive ? L"wasapi_exclusive_pcm" : L"wasapi_shared_pcm";
    }

    if (!IsBitstreamCandidate(media.audioCodec)) {
        reason = L"codec_not_bitstream_candidate_fallback_pcm";
        return L"wasapi_shared_pcm";
    }

    if (!IsBitstreamEnabled(media.audioCodec, settings)) {
        reason = L"codec_passthrough_disabled_fallback_pcm";
        return L"wasapi_shared_pcm";
    }

    reason = L"endpoint_capability_probe_pending";
    return L"wasapi_exclusive_bitstream";
}

}  // namespace

PlaybackPlan PlaybackPlanner::Build(const MediaDescriptor& media,
                                    const PlayerSettings& settings,
                                    const CapabilityReport& capabilities) {
    PlaybackPlan plan;
    plan.videoDecoder = ChooseVideoDecoder(media, settings.video, plan.videoReason);
    plan.videoMode = ChooseVideoMode(media, settings.video, capabilities);
    plan.audioOutput = ChooseAudioOutput(media, settings.audio, plan.audioReason);
    return plan;
}

}  // namespace anvil::playback
