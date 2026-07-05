# Anvil Player

Anvil Player is a native Windows media player prototype focused on reliable local playback, HDR-aware rendering paths, and future Dolby/DTS compatibility work.

This repository currently contains the Phase 1 application skeleton:

- `PlaybackCore`: C++20 playback state, settings, capability report, and structured log boundaries.
- `AnvilPlayer.App`: native Windows desktop shell with a dark theme, dark-orange accents, and rounded player surfaces.
- `PlaybackCore.Tests`: lightweight tests for the core state machine.

The current shell opens local media files, probes real stream metadata with FFmpeg `AVFormatContext`, reads D3D11/DXGI device capabilities, and uses in-process FFmpeg decode with D3D11 video rendering, embedded/external text and bitmap subtitle overlays, and WASAPI shared-mode PCM audio for default playback. The embedded `ffplay` compatibility backend and older raw-frame video bridge remain available as fallback/development paths.

## Build

Requirements:

- Visual Studio 2022 Build Tools with MSVC v143.
- Windows 10/11 SDK.

Build Debug x64:

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" AnvilPlayer.sln /m:1 /p:Configuration=Debug /p:Platform=x64
```

Run tests:

```powershell
.\x64\Debug\PlaybackCore.Tests.exe
```

Run the app:

```powershell
.\x64\Debug\AnvilPlayer.App.exe
```

Open a file from the command line:

```powershell
.\x64\Debug\AnvilPlayer.App.exe "D:\media\sample.mp4"
```

Open and autoplay a file:

```powershell
.\x64\Debug\AnvilPlayer.App.exe --play "D:\media\sample.mp4"
```

Override the runtime log level:

```powershell
.\x64\Debug\AnvilPlayer.App.exe --log-level debug
.\x64\Release\AnvilPlayer.App.exe --log-level info
```

Debug builds default to `debug` logging for development. Release builds default to `info` logging for normal use. Logs are written to `%TEMP%\anvil-player\anvil-player.log` and mirrored in the in-app Log inspector.
When `debug` logging is enabled, native playback also emits periodic `clock`, `video`, and `renderer` aggregates for queue depth, zero-copy counts, render/Present timing, SRV cache behavior, and subtitle overlay rebuilds.

Default playback uses the native FFmpeg/D3D11 video path:

```powershell
.\x64\Release\AnvilPlayer.App.exe --native-playback --play "D:\media\sample.mp4"
```

Use the embedded FFplay compatibility backend only as a fallback:

```powershell
.\x64\Release\AnvilPlayer.App.exe --external-playback --play "D:\media\sample.mp4"
```

Use the raw-frame development bridge only when comparing against the old in-player renderer:

```powershell
.\x64\Release\AnvilPlayer.App.exe --internal-playback --play "D:\media\sample.mp4"
```

## Current UI Direction

- Dark window and content surfaces.
- Dark-orange accent color for primary actions, progress, and emphasis.
- Rounded title bar, viewport, transport bar, buttons, panels, badges, and progress handle.
- Icon-first controls with hover tooltips.
- Inspector tabs for media, device, and playback log views.
- Default playback uses in-process FFmpeg software or D3D11VA hardware video decode rendered through a D3D11 child viewport.
- Embedded text/bitmap subtitles and same-folder external subtitles (`.srt`, `.ass`, `.ssa`, `.vtt`) are decoded through FFmpeg and composited over the native D3D11 video host.
- Subtitle selection is available from the transport subtitle menu, with `S` retained as a quick cycle shortcut.
- Audio uses in-process FFmpeg decode, `swresample`, and WASAPI shared-mode PCM output.
- The embedded `ffplay` child-window backend is retained behind `--external-playback` as a compatibility fallback.
- The development-only `--internal-playback` path uses an external `ffmpeg` raw-frame bridge; decoded frame arrival drives viewport redraws while the playback timer only updates transport state.
- The development-only raw-frame path also uses the native WASAPI audio path.
- Media information and settings panels share the same core settings model planned for later playback work.

## Next Engineering Step

The next milestone is to keep hardening the native playback stack around the D3D11 video path:

1. Validate HDR10/HLG output and SDR tone mapping with real display samples.
2. Broaden subtitle regression coverage for bitmap formats, language switching, and ASS/SSA fallback behavior.
3. Expand WASAPI device selection, format fallback, and endpoint diagnostics.
