# FFmpeg shared build (in-tree third-party)

This directory is an in-tree copy of a prebuilt FFmpeg **shared** build used by
`AnvilPlayer.App` for native in-process demux/decode and BGRA frame delivery to
the D3D11 renderer.

It is **not** checked into git (see top-level `.gitignore`). Re-obtain it with
the steps in "Re-obtain" below.

## Current build

- Source: BtbN `FFmpeg-Builds` release `latest`
  - <https://github.com/BtbN/FFmpeg-Builds/releases>
- Asset: `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip`
- Direct URL:
  `https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip`
- FFmpeg version (from `bin/ffmpeg.exe -version`):
  `ffmpeg version n8.1.2-21-gce3c09c101-20260703`
- License: LGPL v2.1+ (see `LICENSE.txt`); no GPL-only codecs.
- SHA256 of the original zip:
  `cc3fd6feffd4936a4fdac0b59186e4da41aba21287b26c118f76355062bfb6e2`

## Layout (consumed by the build)

```
third_party/ffmpeg/bin/       avcodec-62.dll, avformat-62.dll, avutil-60.dll,
                              swscale-9.dll, swresample-6.dll, avfilter-11.dll,
                              avdevice-62.dll, ffmpeg.exe, ffplay.exe, ffprobe.exe
third_party/ffmpeg/include/   libavcodec/, libavformat/, libavutil/, libswscale/,
                              libswresample/, libavfilter/, libavdevice/
third_party/ffmpeg/lib/       avcodec.lib, avformat.lib, avutil.lib, swscale.lib,
                              swresample.lib, avfilter.lib, avdevice.lib
```

The app links against `avformat.lib;avcodec.lib;avutil.lib;swscale.lib` and
copies `bin/*.dll` to the output directory via a post-build step
(see `src/AnvilPlayer.App/AnvilPlayer.App.vcxproj`).

Major DLL sonames for this build: `avcodec-62`, `avformat-62`, `avutil-60`,
`swscale-9`, `swresample-6`. If you swap in a different FFmpeg major version,
the DLL names in the post-build `xcopy *.dll` glob still match, but check that
the headers' `LIBAV*_VERSION_MAJOR` values line up with what the app expects.

## Re-obtain

The local shell on this machine has broken IPv6 DNS (resolves `github.com` to a
bogus `fc00::78` and times out). The router runs an IPv4 transparent proxy, so
force IPv4 when fetching from a shell:

```bash
cd third_party
curl -4 -L -o ffmpeg-shared.zip \
  https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip
unzip -q ffmpeg-shared.zip
rm -rf ffmpeg && mv ffmpeg-n8.1-latest-win64-lgpl-shared-8.1 ffmpeg
rm ffmpeg-shared.zip
```

Verify the SHA256 matches the value above before relying on the build.
