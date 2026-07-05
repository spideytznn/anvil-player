# Phase 1 Status

## Implemented

- Visual Studio solution with x64 Debug/Release configurations.
- `PlaybackCore` static library with:
  - playback state machine,
  - transport commands,
  - seek and volume clamping,
  - FFmpeg `AVFormatContext` media probing with extension fallback,
  - ffmpeg-backed first-frame preview extraction,
  - initial video/render/audio path planning,
  - default video/audio/subtitle/diagnostic settings,
  - D3D11/DXGI GPU, display, and hardware decode profile probing,
  - in-memory structured playback log.
- Native Windows desktop shell with:
  - dark window chrome,
  - dark-orange accent styling,
  - rounded surfaces and controls,
  - icon controls with hover tooltips,
  - open-file dialog and drag/drop,
  - play/pause/stop/seek/volume/fullscreen UI actions,
  - media/device/log inspector tabs,
  - first-frame preview rendering in the viewport,
  - default in-process FFmpeg software video decode to BGRA frames,
  - bounded native video frame queue scheduled against the WASAPI playback clock,
  - FFmpeg D3D11VA hardware decode for supported video codecs with software fallback,
  - zero-copy D3D11 texture handoff from FFmpeg D3D11VA frames to the renderer for NV12/P010/P016 surfaces,
  - reusable software BGRA frame buffers to reduce per-frame allocation/copy pressure,
  - cached D3D11VA Y/UV shader-resource views for repeated zero-copy hardware frames,
  - debug renderer performance aggregates for render/Present time, BGRA upload, SRV cache behavior, slow frames, and subtitle overlay rebuilds,
  - D3D11 child-window rendering for native video playback,
  - embedded/external text and bitmap subtitle decode/render with D3D11 overlay composition,
  - in-process FFmpeg audio decode through `swresample` into WASAPI shared-mode PCM,
  - progress-bar click/drag seeking that previews while dragging and commits one native playback restart on release,
  - native pause/resume continuity with the last rendered frame held visible across keyboard and mouse-click pause paths until replacement video arrives,
  - paused native-video seek refresh through a one-shot FFmpeg/D3D11 frame decode before remaining paused,
  - long MKV/FLAC seek restart stability through container-clock FFmpeg seeks for WASAPI audio,
  - fallback embedded `ffplay` compatibility backend behind `--external-playback`,
  - legacy external FFmpeg raw-video bridge behind `--internal-playback`,
  - command-line media path opening,
  - `--play` command-line autoplay,
  - settings panel,
  - transport subtitle menu with Auto/stream/Off selection.
- `PlaybackCore.Tests` covering core open, transport, seek, volume, and error logging behavior.

## Intentional Stub Areas

- Media probing uses FFmpeg `AVFormatContext` stream discovery and falls back to extension-based defaults for invalid or unsupported files.
- Preview extraction uses the `ffmpeg` executable when available. Production video playback now uses in-process FFmpeg software decode and D3D11 rendering.
- Native in-process video decoding currently uses FFmpeg software decode or FFmpeg D3D11VA hardware decode for supported codecs, a bounded frame queue, audio-clock scheduling, zero-copy D3D11 texture presentation for supported hardware surfaces, and BGRA texture upload as fallback. The software path reuses BGRA frame buffers, and the hardware zero-copy renderer caches D3D11VA Y/UV shader-resource views by decoder texture/slice/format to reduce repeated per-frame work. HDR10/HLG color metadata now flows from probe/decoder into the D3D11 renderer for native HDR output or SDR tone mapping. Embedded text subtitles, bitmap subtitle rects, same-folder external `.srt`/`.ass`/`.ssa`/`.vtt` subtitles, keyboard track cycling, and the transport subtitle menu are decoded/composited over the D3D11 video host; broader real-world subtitle regression coverage is still pending. The previous synchronous libass file-filter renderer is intentionally removed until it can return as an explicit setting or async path.
- The development-only `--internal-playback` path still uses an external `ffmpeg` process that streams BGRA frames to the app, with viewport redraws driven by decoded frame arrival instead of a fixed video paint timer.
- Native WASAPI audio output currently uses the default render endpoint in shared mode. Device selection, richer endpoint diagnostics, exclusive mode, and passthrough are still pending.
- Capability detection has D3D11/DXGI coverage. WASAPI endpoint probing, Media Foundation transform enumeration, and native FFmpeg version/library reporting are still pending.
- The desktop shell is currently a low-dependency native Win32 shell so it can build with the available Build Tools. The core boundary is designed so a WinUI 3 shell can replace or sit beside it later without changing playback logic.

## Next Checkpoint

Continue native playback integration after the HDR/color diagnostics baseline:

```text
[probe] color primaries=bt2020 transfer=pq matrix=bt2020nc range=limited
[renderer] input=d3d11_texture output=hdr10 color_space=rgb_full_pq_bt2020 tone_mapping=Balanced
[video] scheduler=audio_clock queue_depth=3 dropped_late=1
[video] decoder=ffmpeg_d3d11va zero_copy_frames=133 cpu_transfer_frames=0
[renderer] frames=48 avg_ms=0.18 present_avg_ms=0.08 srv_cache_hits=48 slow_frames=0
[transport] pause position=0:01
[app] paused native frame refresh completed
[subtitle] selected stream=2 codec=subrip
[subtitle] external file=external-smoke.srt cues=2
[d3d11] subtitle_overlay active=true lines=1
[subtitle] bitmap_overlay active=true rects=1
[d3d11] subtitle_overlay active=true lines=0 bitmap_rects=1
```
