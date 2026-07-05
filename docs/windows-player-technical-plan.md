# Anvil Player Windows Technical Plan

## 1. Product Goal

Anvil Player is a native Windows media player focused on strong decoding capability, high-quality HDR playback, and enthusiast-grade Dolby/DTS compatibility.

The first product target is not a certified Dolby Vision player. The practical target is:

- Play common local media files reliably.
- Use GPU decoding whenever possible.
- Output SDR, HDR10, HDR10+, and HLG correctly.
- Play Dolby Vision content with graceful fallback to HDR10 or SDR.
- Provide an experimental Dolby Vision passthrough path on compatible Windows systems.
- Support Dolby Atmos and DTS:X through HDMI bitstream passthrough.
- Keep the architecture open for future licensed Dolby/DTS SDK integration.

## 2. Non-Goals For The First Version

The first version should avoid over-promising features that require vendor licensing, driver-specific behavior, or private platform integration.

Out of scope for the first version:

- Guaranteed Dolby Vision certification.
- Guaranteed Dolby Vision passthrough on all Windows devices.
- Native object-audio rendering for Dolby Atmos or DTS:X without system/vendor support.
- Custom video codec implementation.
- Custom audio codec implementation.

The player should expose Dolby Vision passthrough as an experimental compatibility feature and always provide a safe fallback path.

## 3. Recommended Technology Stack

### UI

- WinUI 3 desktop application.
- Native Windows windowing and input.
- C++/WinRT for Windows API integration.

### Core Runtime

- C++20.
- Dedicated playback core separated from UI.
- Audio clock as the primary playback clock.
- Structured logging for decoder selection, HDR mode, audio passthrough, and fallback reasons.

### Demuxing

- FFmpeg for container parsing and stream extraction.
- Supported containers should include MKV, MP4, MOV, M2TS, TS, WebM, and AVI where practical.

### Video Decoding

- FFmpeg software decoder as universal fallback.
- FFmpeg D3D11VA decoder as the primary open decoding backend.
- Media Foundation decoder as an alternate backend for system codecs and experimental Dolby Vision work.

### Rendering

- D3D11 as the initial rendering backend.
- Optional later D3D12 backend after the player is stable.
- libplacebo for color management, HDR tone mapping, and Dolby Vision fallback processing.

### Audio

- WASAPI shared mode for normal playback.
- WASAPI exclusive mode for bitstream passthrough.
- HDMI passthrough for AC-3, E-AC-3, TrueHD, DTS, DTS-HD MA, Dolby Atmos, and DTS:X where the output device supports it.

### Subtitles

- libass for advanced subtitle rendering.
- Support external subtitle files and embedded subtitle tracks.

## 4. High-Level Architecture

```text
AnvilPlayer
├─ App
│  ├─ WinUI 3 shell
│  ├─ playback window
│  ├─ transport controls
│  ├─ media information panel
│  └─ settings
│
├─ PlaybackCore
│  ├─ PlayerController
│  ├─ Demuxer
│  ├─ PlaybackClock
│  ├─ DecoderManager
│  ├─ VideoPipeline
│  ├─ AudioPipeline
│  ├─ SubtitlePipeline
│  └─ MediaSessionState
│
├─ Decoders
│  ├─ FFmpegSoftwareDecoder
│  ├─ FFmpegD3D11Decoder
│  ├─ MediaFoundationDecoder
│  └─ DolbyVisionExperimentalDecoder
│
├─ Renderers
│  ├─ D3D11VideoRenderer
│  ├─ HDRPresenter
│  └─ ToneMapper
│
├─ Audio
│  ├─ WasapiSharedOutput
│  ├─ WasapiExclusiveOutput
│  └─ BitstreamPassthrough
│
└─ Capabilities
   ├─ GpuCapabilityDetector
   ├─ DisplayCapabilityDetector
   ├─ AudioDeviceCapabilityDetector
   ├─ CodecCapabilityDetector
   └─ DolbyVisionCapabilityDetector
```

## 5. Playback Path Selection

The player should choose the safest high-quality path automatically.

```text
Open media file
↓
Probe container, streams, codecs, HDR metadata, Dolby Vision metadata, audio formats
↓
Is the video Dolby Vision?
├─ No
│  ├─ Try FFmpeg D3D11VA hardware decode
│  ├─ Use HDR10/HLG/HDR10+ output if available
│  └─ Fall back to tone-mapped SDR if needed
│
└─ Yes
   ├─ If experimental DV passthrough is enabled and supported:
   │  ├─ Try Media Foundation / Dolby Vision system decoder path
   │  ├─ Share D3D11 device with decoder
   │  └─ Present decoded GPU texture
   │
   ├─ If experimental path fails:
   │  ├─ Use FFmpeg/libplacebo path
   │  ├─ Convert Dolby Vision to HDR10/PQ when possible
   │  └─ Tone-map to SDR when HDR output is unavailable
   │
   └─ Log the selected path and fallback reason
```

Audio path:

```text
Probe selected audio track
↓
Is passthrough enabled?
├─ No
│  └─ Decode to PCM and play through WASAPI shared/exclusive
│
└─ Yes
   ├─ Check output endpoint format support
   ├─ Try WASAPI exclusive bitstream
   ├─ Let AVR/soundbar decode Atmos or DTS:X
   └─ Fall back to PCM if passthrough fails
```

## 6. Decoder Backend Responsibilities

### FFmpegSoftwareDecoder

Purpose:

- Universal fallback.
- Debug reference path.
- Compatibility for formats not supported by GPU decode.

Responsibilities:

- Decode compressed video to CPU frames.
- Convert to renderer-compatible texture upload format.
- Report performance warnings for high-resolution content.

### FFmpegD3D11Decoder

Purpose:

- Primary GPU decode path.

Responsibilities:

- Use FFmpeg hardware device context with D3D11VA.
- Decode H.264, HEVC, AV1, VP9, and other supported formats on GPU.
- Return D3D11 textures or hardware frames to the renderer.
- Fall back cleanly when GPU decode is unsupported or unstable.

### MediaFoundationDecoder

Purpose:

- Access Windows system codec stack.
- Prepare the path needed for experimental Dolby Vision output.

Responsibilities:

- Create Media Foundation transforms for supported codecs.
- Share the app D3D11 device through `IMFDXGIDeviceManager`.
- Output GPU textures where supported.
- Report exact MFT selection and failure reasons.

### DolbyVisionExperimentalDecoder

Purpose:

- Try native/system Dolby Vision playback on compatible machines.

Responsibilities:

- Detect Dolby Vision display capability.
- Detect whether a system Dolby Vision decoder path appears available.
- Accept compressed HEVC samples from FFmpeg demuxing.
- Try to feed samples into the Media Foundation / Dolby Vision decoder path.
- Present decoded output through the shared D3D11 device.
- Abort quickly and fall back if negotiation, decode, or present fails.

This backend must be optional, disabled by default in early builds, and clearly marked experimental in settings.

## 7. Dolby Vision Strategy

Dolby Vision support should be split into two independent capabilities.

### 7.1 Dolby Vision Fallback Playback

This is the baseline feature and should be reliable.

Expected behavior:

- Detect Dolby Vision metadata and profile.
- Support playback even when native DV output is unavailable.
- Convert or map Dolby Vision content to HDR10/PQ when possible.
- Tone-map to SDR when HDR output is unavailable.
- Prefer correct, watchable output over trying to force native DV.

Implementation direction:

- FFmpeg demux and decode.
- libplacebo for Dolby Vision metadata processing, reshaping, color management, and tone mapping where available.
- D3D11 renderer for presentation.

### 7.2 Experimental Dolby Vision Passthrough

This is an advanced compatibility mode.

Required checks:

- Windows version supports the needed HDR/Dolby APIs.
- Display is in HDR mode.
- Display reports Dolby Vision support in HDR mode where available.
- GPU driver and display chain expose compatible behavior.
- Dolby Vision Extension or equivalent system decoder component appears installed.
- Media Foundation decoder negotiation succeeds.

Expected behavior:

- If all checks pass, try the system decoder path.
- If any step fails, fall back to the normal Dolby Vision fallback path.
- Never leave the player in a black-screen state.
- Show the selected playback mode in the media information panel.

Known risks:

- Windows does not provide a simple public desktop-player API for guaranteed Dolby Vision HDMI passthrough.
- Behavior may vary by GPU vendor, driver, display EDID, HDMI chain, Windows build, and installed extensions.
- Vendor licensing may be required for commercial claims or certified output.

## 8. HDR Strategy

The renderer must handle these cases:

- SDR input to SDR display.
- HDR10 input to HDR display.
- HDR10 input to SDR display.
- HLG input to HDR or SDR display.
- HDR10+ input with metadata-aware fallback where possible.
- Dolby Vision input to HDR10 or SDR fallback.

Required renderer features:

- Correct color primaries handling.
- Correct transfer function handling.
- PQ and HLG awareness.
- Metadata propagation where supported.
- Tone mapping controls.
- User-selectable tone-mapping mode.
- Debug overlay for color pipeline state.

Minimum useful settings:

- HDR output: Auto / Force HDR / Force SDR.
- Tone mapping: Auto / Preserve highlights / Balanced / Bright room.
- Peak brightness override.
- Gamut mapping mode.

## 9. Audio Strategy

Dolby Atmos and DTS:X should be supported first through bitstream passthrough.

### Decode-To-PCM Mode

Used when:

- The output device does not support bitstream.
- The user selects normal PCM playback.
- The format is unsupported for passthrough.

Implementation:

- FFmpeg decodes audio to PCM.
- Audio engine resamples if required.
- WASAPI shared or exclusive outputs PCM.

### Passthrough Mode

Used when:

- User enables passthrough.
- Output device is HDMI or another endpoint that supports encoded formats.
- Format is AC-3, E-AC-3, TrueHD, DTS, DTS-HD, or compatible encoded bitstream.

Expected result:

- AVR/soundbar/TV receives encoded audio.
- AVR/soundbar performs Dolby Atmos or DTS:X decoding.

Fallback:

- If exclusive mode fails or endpoint rejects the format, decode to PCM.
- Log exact reason.

## 10. Capability Detection

The player should include a capability report because advanced playback depends heavily on the machine.

Display capability:

- HDR support.
- Current HDR mode.
- Color space.
- Reported peak brightness where available.
- Dolby Vision support signal where available.

GPU capability:

- Adapter name.
- Driver version where available.
- D3D feature level.
- Supported hardware decode profiles.
- Supported texture formats.

Audio capability:

- Endpoint name.
- Shared/exclusive availability.
- Supported encoded formats where queryable.
- Passthrough test result.

Codec/system capability:

- FFmpeg version and enabled libraries.
- Available Media Foundation transforms.
- Presence of Dolby Vision related system components where detectable.

## 11. Settings Model

Initial settings:

```text
Video
├─ Hardware decode: Auto / D3D11VA / Off
├─ Renderer: D3D11
├─ HDR output: Auto / Force HDR / Force SDR
├─ Tone mapping: Auto / Balanced / Preserve highlights / Bright room
└─ Dolby Vision mode: Fallback only / Experimental passthrough / Off

Audio
├─ Output device: Auto / selected endpoint
├─ Output mode: Auto / PCM / Passthrough
├─ WASAPI mode: Shared / Exclusive
├─ AC-3 passthrough: On / Off
├─ E-AC-3 passthrough: On / Off
├─ TrueHD passthrough: On / Off
├─ DTS passthrough: On / Off
└─ DTS-HD passthrough: On / Off

Subtitles
├─ Preferred language
├─ Font scale
├─ Subtitle delay
└─ External subtitle auto-load

Diagnostics
├─ Show playback stats
├─ Save debug log
└─ Export capability report
```

## 12. Development Phases

### Phase 1: Player Skeleton

Goal:

- Open local media and play SDR video.

Tasks:

- Create WinUI 3 desktop app.
- Create C++ playback core library.
- Add FFmpeg demuxer.
- Add basic audio clock.
- Add D3D11 rendering surface.
- Implement play, pause, seek, stop, volume, and fullscreen.

Acceptance:

- MP4 and MKV files open.
- H.264 SDR video plays with audio.
- Seek works without crashing.
- UI remains responsive during playback.

### Phase 2: GPU Decode

Goal:

- Use GPU decoding as the normal path.

Tasks:

- Add FFmpeg D3D11VA backend.
- Share decoded frames with D3D11 renderer.
- Add hardware decode fallback logic.
- Add playback statistics overlay.
- Add GPU decode state to media information panel.

Acceptance:

- 4K HEVC file uses hardware decode on supported GPU.
- CPU usage is significantly lower than software decode.
- Unsupported files fall back without user intervention.

### Phase 3: HDR And Dolby Vision Fallback

Goal:

- Make HDR and Dolby Vision content watchable without native Dolby output.

Tasks:

- Detect HDR10, HLG, HDR10+, and Dolby Vision metadata.
- Integrate libplacebo.
- Add HDR output path.
- Add HDR-to-SDR tone mapping.
- Add Dolby Vision to HDR10/SDR fallback path.
- Add visible media info for HDR/DV mode.

Acceptance:

- HDR10 content displays correctly on HDR displays.
- HDR content tone-maps correctly on SDR displays.
- Dolby Vision content plays through fallback path.
- User can see whether playback is native HDR, tone-mapped, or DV fallback.

### Phase 4: Audio Passthrough

Goal:

- Support Atmos and DTS:X through HDMI bitstream passthrough.

Tasks:

- Add WASAPI shared PCM output.
- Add WASAPI exclusive output.
- Add encoded bitstream passthrough.
- Add endpoint capability checks.
- Add fallback to PCM.
- Add user settings for passthrough formats.

Acceptance:

- AC-3 and E-AC-3 passthrough works on compatible output.
- TrueHD Atmos passthrough works on compatible AVR/soundbar.
- DTS-HD/DTS:X passthrough works on compatible AVR/soundbar.
- Unsupported endpoints automatically fall back to PCM.

### Phase 5: Experimental Dolby Vision Passthrough

Goal:

- Attempt native/system Dolby Vision output on compatible machines.

Tasks:

- Add Dolby Vision capability detector.
- Query display HDR and Dolby Vision support where available.
- Enumerate Media Foundation transforms.
- Build compressed-sample path from FFmpeg demuxer to Media Foundation decoder.
- Share D3D11 device with Media Foundation through `IMFDXGIDeviceManager`.
- Present decoder output texture.
- Add fast fallback to libplacebo path.
- Add verbose logging for every negotiation step.

Acceptance:

- On unsupported systems, the player clearly reports why DV passthrough is unavailable.
- On compatible systems, the player can attempt the system Dolby Vision decode path.
- Failure does not cause crash, freeze, black screen, or lost audio.
- Fallback path remains usable.

### Phase 6: Productization

Goal:

- Turn the player from a technical prototype into a usable app.

Tasks:

- Add file association.
- Add recent files.
- Add playlist.
- Add media information panel.
- Add settings UI.
- Add capability report export.
- Add crash logging.
- Add installer.
- Add sample-based regression tests.

Acceptance:

- App can be installed and launched normally.
- Users can configure video, HDR, audio, subtitles, and diagnostics.
- Logs are good enough to debug machine-specific playback issues.
- A repeatable sample library can verify major playback paths.

## 13. Test Media Matrix

The project should keep a private test matrix. Do not commit copyrighted media files.

Required categories:

- SDR H.264 1080p MP4.
- SDR H.265 4K MKV.
- HDR10 HEVC 4K MKV.
- HDR10+ HEVC sample.
- HLG sample.
- Dolby Vision Profile 5 sample.
- Dolby Vision Profile 7 sample.
- Dolby Vision Profile 8 sample.
- AC-3 5.1 sample.
- E-AC-3 Atmos sample.
- TrueHD Atmos sample.
- DTS core sample.
- DTS-HD MA sample.
- DTS:X sample.
- ASS subtitle sample.
- PGS subtitle sample.
- Multi-audio-track MKV.
- High-bitrate 4K remux sample.

For each sample, record:

- File name.
- Container.
- Video codec.
- HDR format.
- Dolby Vision profile if present.
- Audio format.
- Expected decode path.
- Expected display mode.
- Expected audio mode.
- Known issues.

## 14. Logging Requirements

Playback logs should include:

- File open time.
- Container and stream summary.
- Selected video decoder.
- Hardware decode state.
- Selected renderer.
- HDR mode.
- Dolby Vision mode.
- Tone mapping mode.
- Selected audio output.
- Passthrough state.
- Subtitle renderer state.
- Dropped frame count.
- Decoder queue state.
- Fallback reasons.

Example:

```text
[video] stream=0 codec=hevc resolution=3840x2160 hdr=dolby_vision profile=8
[capability] display_hdr=true display_dolby_vision=false
[decoder] dv_experimental skipped reason=display_dolby_vision_not_available
[decoder] selected=ffmpeg_d3d11va
[renderer] hdr_output=hdr10 tone_mapping=dolby_vision_to_hdr10
[audio] selected_track=1 codec=truehd atmos=true output=wasapi_exclusive passthrough=true
```

## 15. Risk Register

### Dolby Vision passthrough instability

Risk:

- Native Dolby Vision output may work only on specific combinations of Windows version, GPU, driver, display, HDMI chain, and installed system components.

Mitigation:

- Keep fallback playback reliable.
- Gate passthrough behind capability checks.
- Mark it experimental.
- Log every failure point.

### Licensing and branding

Risk:

- Dolby Vision, Dolby Atmos, and DTS:X branding and certified feature claims may require vendor licensing.

Mitigation:

- Avoid certification claims until licensing is resolved.
- Phrase early feature text as compatibility/fallback/passthrough support.
- Leave room for future licensed SDK integration.

### Hardware decode format gaps

Risk:

- Some GPUs cannot decode specific codecs, bit depths, chroma formats, or profiles.

Mitigation:

- Detect hardware support.
- Keep software decode fallback.
- Show clear diagnostics.

### HDR color errors

Risk:

- Incorrect transfer function, color space, or metadata handling can make images washed out, too dark, clipped, or oversaturated.

Mitigation:

- Centralize color pipeline.
- Use libplacebo for tone mapping and color management.
- Build sample-based visual tests.

### Audio passthrough device variance

Risk:

- WASAPI exclusive encoded output can vary across HDMI devices, drivers, AVRs, and soundbars.

Mitigation:

- Provide manual settings.
- Test endpoint support.
- Fall back to PCM.
- Log endpoint negotiation.

## 16. Immediate Next Steps

Recommended first implementation milestone:

1. Create the WinUI 3 desktop project.
2. Add a separate C++ playback core library.
3. Wire FFmpeg demuxing.
4. Render SDR video frames through D3D11.
5. Add basic WASAPI PCM audio.
6. Add the first playback log format.

After this milestone, implement FFmpeg D3D11VA hardware decoding before starting HDR and Dolby Vision work.

## 17. External References

- Dolby licensing: https://professional.dolby.com/licensing/
- Microsoft Advanced Color / HDR: https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range
- Microsoft Media Foundation D3D11 video decoding: https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation
- Microsoft Media Foundation Source Reader: https://learn.microsoft.com/en-us/windows/win32/medfound/processing-media-data-with-the-source-reader
- Microsoft Spatial Sound: https://learn.microsoft.com/en-us/windows/win32/coreaudio/spatial-sound
- libplacebo: https://github.com/haasn/libplacebo
- FFmpeg hardware acceleration: https://trac.ffmpeg.org/wiki/HWAccelIntro
