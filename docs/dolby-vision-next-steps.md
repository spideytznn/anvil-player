# Dolby Vision / HDR Color Next Steps

Date: 2026-07-06

## Current conclusion

The current Dolby Vision Profile 5 software fallback is close on the first half of the pipeline:

```text
FFmpeg software decode
-> preserve raw 10-bit YUV/IPT samples
-> extract per-frame RPU metadata
-> GPU reshape
-> BT.2020 PQ output
```

Offline checks against libplacebo showed the DV reshape -> BT.2020/PQ math is already fairly close. The color that still looks wrong is most likely in the display mapping stage:

```text
BT.2020 PQ / HDR nits
-> SDR or monitor output mapping
```

PotPlayer's useful switch is probably its `SMPTE ST 2084/2086 HDR Correction` / `HDR2084 to SDR` path: PQ EOTF correction, HDR peak handling, gamut conversion, then SDR output. Our old per-channel ACES path was too crude and shifted hue/saturation.

Vivid Player appears to use FFmpeg for normal media work, but likely delegates Dolby Vision to a Dolby/system decoder path when available. That suggests our long-term design should not rely only on a hand-written DV shader.

## Strategy

Use two independent paths:

1. Native/system Dolby path for machines that support it.
2. High-quality software fallback for everything else.

The fallback should be correct and watchable, but native Dolby output is the path most likely to match commercial players and trigger Dolby Vision displays.

## Path A: Native Dolby / system decoder

Goal: try to reproduce the Vivid-style approach.

Tasks:

1. Detect Dolby Vision system support.
   - Check Windows HDR state.
   - Check display HDR capability and current color space.
   - Check whether Dolby Vision Extension / system Dolby component is installed.
   - Log clear reasons when unavailable.

2. Prototype Media Foundation playback/decode path.
   - Feed HEVC stream with Dolby Vision metadata intact.
   - Test whether MF exposes decoded frames, protected output, or direct presentation only.
   - Confirm whether DV Profile 5 can light up a DV-capable display.

3. Add an experimental setting.
   - `Dolby Vision: Auto / Native if available / Software fallback / Off`
   - Default should stay safe: `Auto`.

4. Runtime logs to add.
   - `dv_native_probe display_hdr=... dolby_extension=... mf_decoder=...`
   - `dv_native_selected=true/false reason=...`
   - `dv_output=native_dolby | hdr10_fallback | sdr_tonemap`

## Path B: Software fallback

Goal: match libplacebo/PotPlayer-style color as closely as practical.

Tasks:

1. Keep the current FFmpeg software decode + raw YUV path.
   - This is required for Profile 5 because normal YUV->RGB conversion destroys the IPT signal.

2. Tighten ST 2084 correction.
   - Use PQ EOTF to convert encoded values to nits.
   - Use source peak from DV metadata when available (`source_max_pq`) or HDR10 metadata fallback.
   - Avoid per-channel tone mapping; map luma and preserve hue.

3. Improve gamut mapping.
   - Current `BT.2020 -> BT.709` matrix can produce negative or out-of-gamut values.
   - Add a saturation-preserving compression step instead of simple clipping.

4. Build a deterministic frame comparison harness.
   - Generate libplacebo reference frames with bundled FFmpeg.
   - Render our shader-equivalent math offline.
   - Compare mean/max error and save side-by-side PNGs.
   - Keep generated comparison images outside git unless they are final baselines.

5. Consider using libplacebo directly later.
   - Best quality route: integrate libplacebo as the color/tone mapping engine.
   - Short-term route: keep HLSL fallback and port only the needed math.

## Immediate next session checklist

1. Play the same DV sample in:
   - PotPlayer with 2084 correction on.
   - Anvil Debug build.
   - FFmpeg/libplacebo reference output.

2. Capture one matching frame from each.

3. Decide which target to match:
   - PotPlayer SDR look.
   - libplacebo BT.2446A SDR look.
   - HDR10 PQ output on HDR display.

4. Add a renderer debug label for the active color path:
   - `dv_reshape=true`
   - `st2084_correction=true`
   - `tone_map=luma_reinhard | bt2446a | hdr_passthrough`

5. If SDR still looks wrong, tune only the post-reshape mapping first. Do not rewrite the RPU reshape path unless the PQ reference comparison fails.

## Cleanup notes

The old `docs/dv-baseline/*.png` screenshots are disposable debug artifacts unless they are named as final reference baselines. Prefer keeping only:

- `docs/dv-baseline/BASELINE.md`
- raw/reference YUV files if they are still used by comparison scripts
- one or two final reference PNGs, if needed

Generated temporary references should live under `%TEMP%\anvil-dv-references` or another ignored folder.

## Known dirty worktree items

At the time this note was written:

- `src/AnvilPlayer.App/src/d3d11_video_renderer.cpp` has active DV/HDR mapping changes.
- `src/AnvilPlayer.App/src/ffmpeg_video_decoder.cpp` already had active DV raw-YUV/SIMD packing changes.
- Several `docs/dv-baseline/*.png` files are deleted in the working tree and appear to be obsolete debug screenshots.

