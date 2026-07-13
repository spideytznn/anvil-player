# Native FFmpeg + D3D11 Handoff Plan (Archived)

> Superseded by `native-d3d12-frame-graph.md`. This file is retained only as a
> historical implementation record and does not describe the current runtime.

This is a temporary handoff plan for replacing the current playback compatibility layer with an in-process FFmpeg video decode path and D3D11 renderer.

## Continuation Status (2026-07-05)

The first native-video milestone has now been implemented and smoke-tested. Audio-clock video scheduling with a bounded frame queue is implemented and smoke-tested. D3D11VA hardware decode and zero-copy D3D11 texture handoff are also implemented and smoke-tested.

Completed:

- FFmpeg shared/dev files are present under `third_party/ffmpeg`.
  - Build source: BtbN `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip`.
  - Details and SHA256 are recorded in `third_party/ffmpeg/README.md`.
- `src/AnvilPlayer.App/AnvilPlayer.App.vcxproj` links `avformat.lib`, `avcodec.lib`, `avutil.lib`, `swscale.lib`, and `swresample.lib`, and copies FFmpeg DLLs to `x64/<Config>/`.
- Default playback backend is `NativeFfmpegD3D11`.
  - `--native-playback`: native FFmpeg video + D3D11 viewport, default.
  - `--external-playback`: embedded `ffplay` compatibility fallback.
  - `--internal-playback`: legacy raw BGRA pipe development path.
- `MainWindow` owns a D3D11 child viewport host, `FfmpegVideoDecoder`, and `D3D11VideoRenderer`.
- `PlaybackCore::MediaProbe` now uses FFmpeg `AVFormatContext` stream discovery instead of the `ffprobe` executable.
  - The app log now reports `probe: tool=avformat ...`.
  - Invalid/unsupported files still fall back to extension-based defaults.
- Native video path now does:

```text
FFmpeg demux/decode in process -> bounded BGRA frame queue -> audio-clock scheduler -> dynamic D3D11 texture -> child-HWND swap chain
```

- Native audio path now does:

```text
FFmpeg audio decode in process -> swresample -> WASAPI shared-mode PCM default endpoint -> playback clock
```

- The hidden `ffplay -nodisp` audio bridge has been removed from the native and raw-frame development backends.
- Embedded `ffplay` startup has popup mitigation (`SW_HIDE` before reparent/show) and is no longer the default video path.
- Decoder/rendering hardening done in this continuation:
  - renderer initialization must succeed before the video host is marked ready;
  - D3D render-target creation logs the actual failing HRESULT;
  - swscale uses `sws_getCachedContext` so frame size/pixel-format changes recreate conversion state;
  - native decode errors are written to the app log sink.
- Audio-clock scheduling work done in this continuation:
  - WASAPI output exposes a thread-safe playback clock based on submitted output frames minus endpoint padding;
  - native playback starts WASAPI audio before video so video scheduling can use audio as the master clock from startup;
  - `FfmpegVideoDecoder` now keeps a bounded 6-frame native video queue instead of only a latest-frame slot;
  - the scheduler publishes due frames against the WASAPI clock, drops late frames when catching up, and falls back to a wall clock for video-only playback or audio startup failure;
  - debug logs now report `clock: master=audio ... drift_ms=...` and `video: scheduler=audio_clock queue_depth=... rendered=... dropped_late=...`.
- D3D11VA hardware decode work done in this continuation:
  - native video startup passes the `PlaybackCore` planned decoder into `FfmpegVideoDecoder`;
  - H.264, HEVC, VP9, and AV1 candidates try FFmpeg `AV_HWDEVICE_TYPE_D3D11VA` through `AVCodecContext::hw_device_ctx`;
  - codec/device/open failures fall back to software decode and log the fallback reason;
  - FFmpeg D3D11VA now uses the renderer's D3D11 device when available and allocates decode surfaces with `D3D11_BIND_SHADER_RESOURCE`;
  - hardware frames are retained as referenced `AVFrame`s while queued so FFmpeg cannot reuse the surface before presentation;
  - the renderer can sample NV12/P010/P016 D3D11 decoder textures directly with Y and UV shader-resource views and a metadata-driven YUV/HDR pixel shader;
  - HDR10/HLG color metadata now flows through probe, decoder frames, logs, the Inspector, swapchain color-space selection, HDR10 metadata, and SDR tone mapping fallback;
  - if zero-copy texture handoff is unavailable, the decoder can still fall back to `av_hwframe_transfer_data` and the existing BGRA conversion/upload path;
  - debug logs now report `decoder=ffmpeg_d3d11va`, `hardware_frames=...`, `zero_copy_frames=...`, `cpu_transfer_frames=...`, or `decoder=ffmpeg_software ... fallback_reason=...`.
- Text/bitmap subtitle composition work done in this continuation:
  - native playback opens the first preferred/default decodable subtitle stream with FFmpeg;
  - same-folder external `.srt`, `.ass`, `.ssa`, and `.vtt` subtitles auto-load when `settings.subtitles.externalSubtitleAutoLoad` is enabled;
  - external subtitles are matched by media basename, with language suffixes such as `movie.en.srt` scored against the preferred subtitle language;
  - external subtitles take priority over embedded subtitle streams to avoid duplicate overlays;
  - SRT/plain text and basic ASS event text are normalized into active cue ranges;
  - ASS/SSA subtitles currently use the FFmpeg decoded subtitle text/bitmap fallback path; the earlier synchronous libass-backed file-filter renderer has been removed until it can return as an explicit setting or async renderer;
  - each scheduled native video frame carries the active subtitle text for its PTS;
  - the D3D11 renderer builds a cached transparent BGRA subtitle overlay with GDI+ text and composites it with a premultiplied-alpha pass over the native video host;
  - bitmap subtitle rects are converted from FFmpeg palette data into premultiplied BGRA overlays, scaled into the letterboxed video viewport, and composited through the same D3D11 overlay pass;
  - the transport subtitle menu selects Auto, embedded subtitle streams, or Off, restarting native playback or refreshing the paused native frame as needed;
  - pressing `S` cycles through the same subtitle selections as a shortcut;
  - broader real-world subtitle regression coverage remains pending.
- Playback performance hardening done in this continuation:
  - software decode now writes `sws_scale` BGRA output directly into a small reusable frame-buffer pool instead of allocating and copying a new vector for every video frame;
  - the D3D11VA zero-copy renderer caches Y/UV shader-resource views by decoder texture, array slice, and DXGI format instead of recreating SRVs every rendered frame;
  - renderer clear/restart releases cached hardware SRVs so old decoder surfaces are not held across playback sessions;
  - `--debug-log` now emits 2-second renderer performance aggregates for CPU-side render time, Present time, BGRA upload time, D3D11VA SRV cache hits/misses, slow frames, and subtitle overlay rebuilds.
- Playback operation stability work done in this continuation:
  - progress bar interaction now has a real drag state with mouse capture;
  - dragging only updates transport preview and commits one seek on mouse release instead of restarting playback on mouse down or every move;
  - pause now captures the latest native frame before stopping decode/audio, keeps the D3D renderer alive, and repaints the cached native frame after mouse/layout-triggered host redraws;
  - resume keeps the held frame visible until replacement video frames arrive from the restarted native pipeline;
  - paused native-video seeks run a one-shot FFmpeg/D3D11 frame decode and render that frame before remaining paused;
  - the D3D11 renderer enables `ID3D10Multithread` protection on its shared device/context to reduce UI-thread render and decoder-thread D3D contention;
  - native video seek drops preroll frames before the requested position instead of sending them through renderer scheduling, reducing seek burst work;
  - WASAPI audio restart now seeks the container clock (`AV_TIME_BASE` with stream index `-1`) instead of the audio stream time base, avoiding long MKV/FLAC restart stalls after progress-bar seeks.

Verified:

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" AnvilPlayer.sln /m:1 /p:Configuration=Debug /p:Platform=x64
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" AnvilPlayer.sln /m /p:Configuration=Release /p:Platform=x64
.\x64\Debug\PlaybackCore.Tests.exe
.\x64\Release\PlaybackCore.Tests.exe
```

Release smoke test used a generated 10-second MP4 from `third_party/ffmpeg/bin/ffmpeg.exe` with the default `--play` path. During playback, window enumeration showed `Anvil Player` only; the new `ffplay` process had no visible window title and was gone after closing the app. The app log confirmed `playback backend=native_ffmpeg_d3d11` and `native ffmpeg/d3d11 playback start video=true audio=true`.

Later Release smoke after WASAPI integration confirmed no new `ffplay` process is started and the app log reports `wasapi audio=wasapi shared pcm 48000 Hz 2 ch float32`.

Release smoke after audio-clock scheduling used a generated 6-second MP4 from `third_party/ffmpeg/bin/ffmpeg.exe` with `mpeg4` video and AAC audio because this LGPL FFmpeg package does not include `libx264`. The default `--play --debug-log` path started no new `ffplay` process. The app log confirmed:

```text
clock: master=audio position=0:02 drift_ms=5
video: scheduler=audio_clock queue_depth=6 rendered=64 dropped_late=0 dropped_queue_full=0
```

Release smoke after D3D11VA integration used a generated 6-second H.264/AAC MP4 from `third_party/ffmpeg/bin/ffmpeg.exe` using the available `libopenh264` encoder. The default `--play --debug-log` path started no new `ffplay` process. The app log confirmed:

```text
decoder: planned=ffmpeg_d3d11va reason=hardware_decode_candidate
decoder: d3d11va candidate pix_fmt=d3d11
decoder: selected=ffmpeg_d3d11va transfer=cpu_bgra
decoder: d3d11va format selected=d3d11
video: decoder=ffmpeg_d3d11va scheduler=audio_clock queue_depth=6 rendered=65 hardware_frames=72 dropped_late=1 dropped_queue_full=0
```

A software-path smoke with MPEG-4 Part 2 confirmed `decoder=ffmpeg_software` and `hardware_frames=0`.

Release smoke after zero-copy handoff reused the H.264/AAC MP4. The app log confirmed renderer-device sharing and direct texture presentation:

```text
decoder: d3d11va candidate pix_fmt=d3d11 device=renderer_shared
decoder: selected=ffmpeg_d3d11va handoff=zero_copy_or_cpu_transfer
video: decoder=ffmpeg_d3d11va scheduler=audio_clock queue_depth=6 rendered=65 hardware_frames=72 zero_copy_frames=72 cpu_transfer_frames=0 dropped_late=0 dropped_queue_full=0
```

Release smoke after the progress-drag fix used window messages to drag the progress bar from roughly 20% to 75%. The app stayed responsive, closed normally, and logged a single seek on release:

```text
d3d11: multithread protection enabled
transport: seek position=0:04
video: decoder=ffmpeg_d3d11va scheduler=audio_clock queue_depth=1 rendered=4 hardware_frames=6 zero_copy_frames=6 cpu_transfer_frames=0 dropped_late=1 dropped_queue_full=0
```

Release smoke after the long MKV seek fix used an 83:52 H.264/FLAC 1080p MKV. A single drag seek from roughly 15% to 70% stayed responsive, closed normally, and restarted native playback immediately:

```text
transport: seek position=58:49
wasapi: seek target=58:49
app: native ffmpeg/d3d11 playback restart video=true audio=true
video: decoder=ffmpeg_d3d11va scheduler=audio_clock queue_depth=1 rendered=4 hardware_frames=6 zero_copy_frames=6 cpu_transfer_frames=0 dropped_late=1 dropped_queue_full=0
```

A follow-up multi-seek smoke on the same file exercised progress clicks and drags at roughly 25%, 82%, 42%, and 93%. The window remained responsive after every operation, all four seeks restarted native playback, and zero-copy hardware frames continued with `cpu_transfer_frames=0`.

A Debug subtitle smoke generated a 4-second MKV with MPEG-4/AAC and an embedded SRT stream. The default `--play --debug-log` native path selected the SRT stream and generated subtitle overlays:

```text
Subtitle: stream=2 codec=SRT details=Subtitle stream
subtitle: selected stream=2 codec=subrip language=-
d3d11: subtitle_overlay active=true lines=1 surface=1186x353
```

A Debug external-subtitle smoke generated a 4-second MP4 with no embedded subtitles plus a same-folder `external-smoke.srt`. The default `--play --debug-log` native path auto-loaded the external file and generated subtitle overlays:

```text
subtitle: selected stream=0 codec=subrip language=-
subtitle: external file=external-smoke.srt cues=2
d3d11: subtitle_overlay active=true lines=1 surface=1186x353
```

Historical debug libass smokes generated a styled external ASS sample and an embedded ASS MKV. That synchronous FFmpeg `libavfilter` renderer has since been removed; these log lines document the retired path, not current expected output:

```text
subtitle: libass renderer=ass external file=styled-ass.ass size=640x360
d3d11: subtitle_overlay active=true lines=0 bitmap_rects=1 surface=1794x1112
subtitle: libass renderer=subtitles embedded si=0 size=640x360
d3d11: subtitle_overlay active=true lines=0 bitmap_rects=1 surface=1794x1112
```

The matching Release smoke covered the same retired libass bitmap overlay path.

Debug and Release performance smokes reused a 6-second H.264/AAC MP4. D3D11VA zero-copy remained active after SRV caching:

```text
decoder: selected=ffmpeg_d3d11va handoff=zero_copy_or_cpu_transfer
video: decoder=ffmpeg_d3d11va scheduler=audio_clock queue_depth=6 rendered=125 hardware_frames=133 zero_copy_frames=133 cpu_transfer_frames=0 dropped_late=1 dropped_queue_full=0
transport: completed
```

Release renderer-diagnostics smoke on the same H.264/AAC sample confirmed the debug-only aggregate render log and SRV cache behavior:

```text
renderer: frames=50 hw_frames=50 bgra_frames=0 avg_ms=0.23 max_ms=0.60 present_avg_ms=0.10 present_max_ms=0.44 slow_frames=0 hw_prepare_avg_ms=0.00 srv_cache_hits=42 srv_cache_misses=8
renderer: frames=48 hw_frames=48 bgra_frames=0 avg_ms=0.18 max_ms=0.32 present_avg_ms=0.08 present_max_ms=0.30 slow_frames=0 hw_prepare_avg_ms=0.00 srv_cache_hits=48 srv_cache_misses=0
```

Debug and Release pause/resume/paused-seek smokes used a generated 30-second H.264/AAC MP4. Window probing confirmed the native video host stayed visible through pause, paused seek refresh, immediate resume, and continued playback. The app log confirmed:

```text
transport: pause position=0:01
transport: seek_relative position=0:11
app: paused native frame refresh start position=0:11
video: decoder=ffmpeg_d3d11va scheduler=wall_clock queue_depth=0 rendered=1 hardware_frames=1 zero_copy_frames=1 cpu_transfer_frames=0 dropped_late=0 dropped_queue_full=0
app: paused native frame refresh completed
renderer: frames=50 hw_frames=50 bgra_frames=0 avg_ms=0.29 max_ms=0.49 present_avg_ms=0.14 present_max_ms=0.37 slow_frames=0
```

Release click-pause smoke used a generated H.264/AAC MP4 and sent mouse messages through the native video child window. The log confirmed `transport: pause position=0:02`; screen sampling of the video host stayed non-black before and after pause (`BrightnessBefore=30.39`, `BrightnessAfterPause=30.69`), and the host remained visible.

Next recommended work:

1. Broaden subtitle regression coverage for bitmap formats, language switching, and ASS/SSA fallback behavior.
2. Add visual HDR sample validation on HDR and SDR displays.
3. Expand WASAPI device selection, format fallback, and endpoint diagnostics.

## Original Handoff State

- Workspace: `D:\localproject\anvil-player`
- App entry/UI: `src/AnvilPlayer.App/src/main.cpp`
- Core media model/probing: `src/PlaybackCore`
- Current default playback was recently switched to an embedded `ffplay` compatibility path:
  - `EmbeddedFfplayPlayer` starts around `src/AnvilPlayer.App/src/main.cpp`.
  - `AppArguments::externalPlayback = true` makes that path default.
  - `--internal-playback` still uses the older `ffmpeg` raw BGRA pipe plus hidden `ffplay -nodisp` audio.
- This improved responsiveness, but a black `ffplay` window can appear briefly or visibly when play/seek starts. That window is from `ffplay`/SDL before it is embedded or while it is restarting.
- `PlayerController::OpenMedia(path, false)` is used by the app to avoid synchronous preview extraction and speed up startup.

## Original Immediate Goal

Make default playback use:

```text
FFmpeg demux/decode in process -> decoded video frame queue -> D3D11 texture upload -> D3D11 swap-chain renderer in the viewport
```

Keep audio temporarily on the existing hidden `ffplay -nodisp` bridge until WASAPI is implemented. Do not use visible/embedded `ffplay` as the default video path after this work.

## Original Dependency Status

The machine currently has only FFmpeg executables from `Gyan.FFmpeg`, located under:

```text
C:\Users\spideytznn\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe
```

That install does not provide MSVC headers/import libs for linking.

Attempted dependency installs/downloads:

- `Invoke-WebRequest` and `curl.exe` to Gyan/GitHub failed with TLS/Schannel handshake errors.
- `winget install --id Gyan.FFmpeg.Shared -e --scope user --accept-source-agreements --accept-package-agreements` found the package but failed during download:

```text
InternetOpenUrl() failed.
0x80072efd
```

The next agent should first obtain a shared/dev FFmpeg package by one of these methods:

1. Use a browser/manual download if the local shell TLS path is broken:
   - Preferred: `Gyan.FFmpeg.Shared` 8.1.x shared build.
   - Alternative: BtbN `win64-lgpl-shared` or `win64-gpl-shared` build.
2. Extract to:

```text
third_party/ffmpeg
```

Expected layout:

```text
third_party/ffmpeg/bin/avcodec-*.dll
third_party/ffmpeg/bin/avformat-*.dll
third_party/ffmpeg/bin/avutil-*.dll
third_party/ffmpeg/bin/swscale-*.dll
third_party/ffmpeg/include/libavcodec/avcodec.h
third_party/ffmpeg/include/libavformat/avformat.h
third_party/ffmpeg/include/libavutil/...
third_party/ffmpeg/include/libswscale/swscale.h
third_party/ffmpeg/lib/avcodec.lib
third_party/ffmpeg/lib/avformat.lib
third_party/ffmpeg/lib/avutil.lib
third_party/ffmpeg/lib/swscale.lib
```

3. Copy required DLLs from `third_party/ffmpeg/bin` to `x64/<Config>/` in the app post-build step.

## Original Project Wiring

Update `src/AnvilPlayer.App/AnvilPlayer.App.vcxproj` first, because the native renderer is app/UI-specific:

- Add include dir:

```xml
$(SolutionDir)third_party\ffmpeg\include
```

- Add lib dir:

```xml
$(SolutionDir)third_party\ffmpeg\lib
```

- Add dependencies:

```xml
avformat.lib;avcodec.lib;avutil.lib;swscale.lib
```

- Keep existing D3D libs:

```xml
d3d11.lib;dxgi.lib;dxguid.lib
```

- Add a post-build copy for FFmpeg DLLs:

```bat
xcopy "$(SolutionDir)third_party\ffmpeg\bin\*.dll" "$(OutDir)" /Y /I /D
```

Optional but recommended: create `third_party/ffmpeg/README.md` with exact package URL/version/hash after installing.

## Original Implementation Plan

### 1. Remove visible `ffplay` from default video playback

In `src/AnvilPlayer.App/src/main.cpp`:

- Introduce a new playback backend enum instead of `bool externalPlayback_`:

```cpp
enum class PlaybackBackend {
    NativeFfmpegD3D11,
    EmbeddedFfplay,
    RawFrameBridge,
};
```

- Default should be `NativeFfmpegD3D11`.
- Keep command line flags:
  - `--native-playback`: native FFmpeg/D3D11, default.
  - `--external-playback`: fallback embedded ffplay.
  - `--internal-playback`: old raw-frame bridge development path.
- Until native path is functional, hide the `ffplay` popup better:
  - In `EmbeddedFfplayPlayer::Start`, set `STARTUPINFO::dwFlags |= STARTF_USESHOWWINDOW`.
  - Set `startupInfo.wShowWindow = SW_HIDE`.
  - Keep `-noborder`.
  - As soon as `EnumWindows` finds the ffplay window, call `ShowWindow(child, SW_HIDE)` before `SetParent`, then show after `MoveWindow`.
  - This is a fallback mitigation only; the default should not rely on it.

### 2. Add a D3D11 viewport host

Create a child window that exactly covers `PlaybackSurfaceBounds()`:

- Register a simple child class, e.g. `AnvilVideoHostWindow`.
- Store `HWND videoHost_` in `MainWindow`.
- Create it after main window creation or lazily before playback starts.
- Move it in `UpdateLayout()` with `MoveWindow(videoHost_, bounds.left, bounds.top, width, height, TRUE)`.
- Hide it when no native video is playing or when the inspector/settings overlay occupies the video area.
- Use `WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN`.

Rationale: D3D11 swap chains are cleaner against a child HWND than trying to mix GDI and D3D directly on the main window.

### 3. Add `D3D11VideoRenderer`

Add a class in `src/AnvilPlayer.App/src/main.cpp` first, or split into new app files if desired:

Responsibilities:

- Create D3D11 device/context.
- Create `IDXGISwapChain1` for `videoHost_`.
- Create render target view from back buffer.
- Maintain a dynamic `ID3D11Texture2D` for BGRA frames.
- On each frame:
  - `Map(D3D11_MAP_WRITE_DISCARD)`
  - copy BGRA rows into texture
  - unmap
  - render full-screen textured quad
  - `Present(1, 0)` initially; consider `Present(0, DXGI_PRESENT_DO_NOT_WAIT)` later.
- On resize:
  - release RTV
  - `ResizeBuffers`
  - recreate RTV

First implementation can use BGRA software frames. That is acceptable for the milestone. Hardware D3D11VA decode can come later.

Minimum D3D pieces:

- `D3D11CreateDevice`
- `CreateDXGIFactory2`
- `CreateSwapChainForHwnd`
- vertex shader + pixel shader for textured quad
- sampler state
- blend off, viewport set to child size

### 4. Add `FfmpegVideoDecoder`

Add an app-side class first:

```cpp
class FfmpegVideoDecoder {
public:
    bool Start(std::filesystem::path path,
               std::chrono::milliseconds startPosition,
               HWND notifyWindow,
               UINT notifyMessage);
    void Stop();
    bool LatestFrame(NativeVideoFrame& frame);
};
```

Frame shape:

```cpp
struct NativeVideoFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::shared_ptr<const std::vector<uint8_t>> bgra;
    std::chrono::milliseconds pts{0};
    uint64_t serial = 0;
};
```

Decoder thread flow:

```text
avformat_open_input
avformat_find_stream_info
av_find_best_stream(video)
avcodec_alloc_context3
avcodec_parameters_to_context
avcodec_find_decoder
avcodec_open2
seek if startPosition > 0 using av_seek_frame
read packets
send packets to decoder
receive frames
convert to BGRA using sws_scale
publish latest frame
PostMessage(hwnd, WM_APP_NATIVE_FRAME_READY, ...)
```

Important details:

- Use `av_packet_unref` and `av_frame_free` correctly.
- Compute PTS from `frame->best_effort_timestamp` and stream `time_base`.
- For a first working version, preserve source size up to a reasonable cap if needed. Do not force 960x540 unless required for performance.
- Drop old frames by storing only latest frame for now. Later add a bounded queue and audio-clock scheduling.
- On seek, stop/restart decoder from the new position, similar to the current process bridge.

### 5. Wire native playback into `MainWindow`

Add members:

```cpp
D3D11VideoRenderer d3dRenderer_;
FfmpegVideoDecoder nativeVideoDecoder_;
HWND videoHost_ = nullptr;
```

Add message:

```cpp
constexpr UINT kNativeVideoFrameReadyMessage = WM_APP + 2;
```

In message handler:

- On `kNativeVideoFrameReadyMessage`:
  - get latest native frame
  - call `d3dRenderer_.Render(frame)`
  - invalidate/refresh transport at low frequency only
  - do not call heavy GDI full-window paint per frame

In `StartRuntime()`:

- If backend is `NativeFfmpegD3D11`:
  - ensure video host exists
  - initialize renderer with video host
  - start `nativeVideoDecoder_`
  - start `audioPlayer_` for temporary audio only
  - do not start `EmbeddedFfplayPlayer`
  - do not start `ExternalVideoDecoder`

In `StopRuntime()`:

- Stop native decoder.
- Stop temp audio bridge.
- Hide video host or clear renderer.

In seek/restart:

- Restart native decoder with `snapshot.position`.
- Restart temp audio bridge with same position.

### 6. Keep UI responsive

Do not use `UpdateWindow()` inside per-frame message handlers.

Avoid repainting the full main window for every video frame. Native video frames should render directly to the D3D child HWND. The main GDI UI should repaint only transport/buttons/log panels as needed.

### 7. Tests and Smoke Verification

Build:

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" AnvilPlayer.sln /m /p:Configuration=Debug /p:Platform=x64
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" AnvilPlayer.sln /m /p:Configuration=Release /p:Platform=x64
```

Tests:

```powershell
.\x64\Debug\PlaybackCore.Tests.exe
.\x64\Release\PlaybackCore.Tests.exe
```

Smoke test:

```powershell
$sample = Join-Path $env:TEMP 'anvil-player-smoke.mp4'
ffmpeg -hide_banner -loglevel error -y `
  -f lavfi -i testsrc2=size=1280x720:rate=30:duration=10 `
  -f lavfi -i sine=frequency=440:duration=10 `
  -c:v libx264 -pix_fmt yuv420p -c:a aac $sample

.\x64\Release\AnvilPlayer.App.exe --play $sample
```

Acceptance criteria:

- No separate black ffplay window appears in default playback.
- Video appears inside the Anvil viewport.
- Play/pause/seek buttons remain clickable while video is playing.
- Seeking restarts native video near the requested position.
- Closing the app terminates decoder/audio worker processes/threads.
- Release build can play the smoke file for 10 seconds without freezing the main window.

## Suggested First Milestone

Do not attempt hardware decode, subtitles, audio sync perfection, or HDR in the first pass.

Milestone 1 should be:

```text
Software FFmpeg video decode -> BGRA frames -> D3D11 texture upload -> visible playback in viewport
```

Audio may remain temporary via hidden `ffplay -nodisp`. After this milestone, replace the audio bridge with WASAPI and use audio clock for proper video scheduling.

## Known Risks

- FFmpeg shared dependency is currently blocked by local HTTPS download failures. Manual browser download may be required.
- BtbN and Gyan package names/versions may differ in DLL names, but import libraries should be stable (`avcodec.lib`, etc.).
- Mixing child D3D windows and GDI UI requires correct z-order. Keep the video host behind overlays and avoid painting opaque GDI over it unless intentionally hiding it.
- First software decode implementation may still be CPU-heavy for 4K/HDR. That is acceptable for the milestone; D3D11VA decode is a later optimization.
