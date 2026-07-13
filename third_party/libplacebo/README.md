# libplacebo for the optional D3D12 Dolby Vision path

The tracked headers and license come from libplacebo 7.371.0 at commit
`a7a18af88ff0a17c04840dcb3246047bb6b46df3`. The runtime DLL is generated
locally and is intentionally ignored by Git.

libplacebo does not expose a native D3D12 GPU backend. Anvil Player creates a
D3D11On12 device over its existing D3D12 device and graphics queue, then gives
that device to libplacebo's D3D11 backend. FFmpeg D3D12VA surfaces and the FP16
swap-chain buffer are wrapped in place; the video path does not use CPU copies
or shared NT handles.

## Build

Install MSYS2 UCRT64, then run this command from the repository root:

```powershell
.\scripts\build-libplacebo.ps1 -Msys2Root C:\msys64
```

Use `-SkipPackageInstall` after the required UCRT64 packages are installed.
The script builds shaderc, SPIRV-Cross, the C++ runtime, and winpthreads into
`bin\libplacebo-371.dll`. Keeping those dependencies inside the DLL avoids a
module-name collision with the MinGW runtime already bundled for libass.

## Enable

The bridge is experimental and opt-in:

```powershell
$env:ANVIL_DOVI_LIBPLACEBO_D3D12 = '1'
.\x64\Debug\AnvilPlayer.App.exe --play 'D:\media\sample.mkv'
```

Set `ANVIL_LIBPLACEBO_DLL` to an absolute API-371 DLL path to override the
copy next to `AnvilPlayer.App.exe`. If loading or rendering fails, the player
keeps the native D3D12 Dolby Vision shader path active as a fallback.

