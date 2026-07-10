# Repository Guidelines

## Project Structure & Module Organization

- `src/PlaybackCore/` contains reusable C++20 playback state, probing, settings, logging, and capability logic. Public headers live under `include/AnvilPlayer/Playback/`; implementations live in `src/`.
- `src/AnvilPlayer.App/` is the native Windows application, including D3D11 rendering, FFmpeg decoding, WASAPI audio, subtitles, and WebView2 integration. Native headers mirror the `AnvilPlayer/App` namespace.
- `src/AnvilPlayer.App/webui/` contains the React/TypeScript UI. Put static assets in `public/` and UI modules in `src/`.
- `src/PlaybackCore.Tests/` contains the single native test executable. Supporting design notes are in `docs/`, packaging scripts in `scripts/` and `packaging/`, and vendored dependencies in `third_party/`.

## Build, Test, and Development Commands

Run commands from the repository root in PowerShell.

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" AnvilPlayer.sln /m:1 /p:Configuration=Debug /p:Platform=x64
.\x64\Debug\PlaybackCore.Tests.exe
.\x64\Debug\AnvilPlayer.App.exe --play "D:\media\sample.mp4"
cd src\AnvilPlayer.App\webui; npm ci; npm run build
```

The first command builds the complete x64 solution with MSVC v143. The second runs native tests; the third launches a media file. `npm run build` type-checks and bundles the Web UI. Use `npm run dev` for Vite-only UI development. Visual Studio 2022 Build Tools and a Windows 10/11 SDK are required.

## Coding Style & Naming Conventions

Follow existing formatting: four-space indentation in C++, two spaces in TypeScript/CSS, and no semicolons in TypeScript. Use `PascalCase` for C++ types/functions and React components, `camelCase` for TypeScript functions/variables, and `snake_case` for native app filenames. Keep public declarations in matching `include/` paths. C++ builds as C++20 with `/W4`, `/permissive-`, Unicode, and conformance mode; resolve new warnings even though warnings are not errors.

## Testing Guidelines

Tests use standard `assert` in `src/PlaybackCore.Tests/src/main.cpp`. Name cases `TestFeatureBehavior`, keep fixtures deterministic, and clean up temporary files. Add focused coverage for playback state, math helpers, and regressions; run the Debug test executable before submitting. No numeric coverage threshold is configured.

## Commit & Pull Request Guidelines

Recent commits use concise, scoped subjects such as `修复: ...`, `C4 重构(c++): ...`, and `W6 重构(webui): ...`. Follow that pattern: identify the change type and affected subsystem, using an imperative, single-line summary. Pull requests should explain behavior and risk, list validation commands, link relevant issues or design notes, and include screenshots for UI changes. Do not commit generated `x64/`, `dist/`, `node_modules/`, or Web UI build output.
