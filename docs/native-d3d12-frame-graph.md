# Native D3D12 Frame Graph

This document is the authoritative description of the native video path. The
older D3D11 handoff and phase-one documents are historical records only.

## Runtime invariants

- The application creates one `ID3D12Device` and gives it to FFmpeg's D3D12VA
  hardware context.
- Decode, preprocess compute, ML compute, graphics, and copy work use queues
  from that device. Preprocess and ML have separate compute queues so a long
  inference cannot delay an original decoded frame that is ready to present.
- Normal playback never creates a shared NT handle, cross-API fence, staging
  video texture, CPU pixel buffer, or dynamic video upload. The packaged
  libplacebo DV bridge is the sole D3D11On12 exception and
  still shares the original D3D12 device, queue, resources, and fences.
- Every resource handoff carries a producing fence/value. Queue waits are GPU
  waits; the CPU scheduler never waits for inference.
- Seek increments the frame-graph epoch. Old work completes naturally and is
  recycled after its fence, but cannot be presented.

## Decode-to-present path

```text
FFmpeg demux -> D3D12VA NV12/P010 surface
             -> D3D12 compute crop/chroma/YUV/range/transfer/DV reconstruction
             -> application-owned FP16 NCHW tensor buffer
             -> Windows ML or DirectML DML1 executor
             -> application-owned FP16 RGB output buffer
             -> final D3D12 graphics composite (scale/tone map/subtitle/UI)
             -> scRGB/HDR or SDR swap chain
```

The decoder publishes a bounded look-ahead callback as soon as a decoded GPU
surface is ready. The renderer owns pair formation, preprocessing, inference,
cadence, composition, and presentation; interpolation is not a decoder
feature. The presentation queue independently schedules original frames.

Base NV12/P010 composition, subtitle/UI composition, and presentation become
ready before any interpolation model is loaded. When interpolation is enabled,
the renderer starts the lightweight tensor preprocessor and model executor on
a below-normal-priority background thread while original frames continue to
play. The executor is atomically published at a frame boundary and generated
frames join the existing cadence without restarting decode, audio, or the swap
chain. A session that leaves interpolation disabled never loads the model.

The preprocessor accepts arbitrary `t`. Display refresh and source frame rate
select a 1x-5x cadence. Deadline telemetry reduces the multiplier when the GPU
cannot finish generated frames in time and cautiously raises it after sustained
headroom. Generated frames are either shown between their two source frames or
dropped before the right endpoint; they are never presented out of order.
Admission is bounded to two outstanding model jobs. When the executor is full,
the renderer retains only the newest source endpoint and does not submit more
preprocess work, protecting the original-frame path from queue starvation.

## Model executors

Both executors bind application-owned D3D12 buffers directly:

- `onnx_runtime_directml_dml1_d3d12` is the deterministic low-latency
  baseline. ONNX Runtime parses/optimizes the ONNX graph, while its DML1 API
  executes on the application's `IDMLDevice` and compute queue.
- `windows_ml_native_d3d12` uses `ILearningModelDeviceFactoryNative` and
  `ITensorStaticsNative` to create a Windows ML session and FP16 tensors from
  the same queue/resources. It is the fallback when DML1 graph deployment is
  unavailable.

Completion errors are recorded per fence value. A failed job cannot poison a
later successful frame, and failed jobs still signal completion so resource
recycling cannot deadlock.

## Dolby Vision and HDR

The internal interpolation domain is canonical BT.2020 PQ FP16. The shared
D3D12 reconstruction shader performs RPU polynomial/MMR reshaping, Profile 7
BL+EL residual composition, LINEAR_DZ NLQ reconstruction, FEL resampling, and
display-trim selection before interpolation. The final composite applies the
selected HDR/scRGB/SDR mapping; generated frames do not fabricate RPU data.

The large DV graphics and tensor-reconstruction shaders are demand-loaded only
after a frame carrying valid RPU/EL metadata arrives. Until the graphics PSO is
published, the base layer remains visible; until the DV tensor PSO is
published, only generated DV frames are bypassed. SDR and HDR10 playback never
pay the DV shader compilation cost.

Profile 7 EL/FEL composition is active when FFmpeg exposes the enhancement
layer as a decodable video stream. Containers that keep BL and EL multiplexed
inside one HEVC elementary stream still depend on FFmpeg exposing a separate
enhancement surface; the compositor cannot reconstruct pixels it was never
given. This input limitation is reported as `no_el_only_stream`, while the BL
RPU path remains active.

### libplacebo D3D12 interop

The packaged libplacebo API 371 renderer is selected by default. Set
`ANVIL_DOVI_LIBPLACEBO_D3D12=0` to disable it for driver diagnostics.
libplacebo has no native D3D12 backend, so the adapter creates a
D3D11On12 device on the application's existing D3D12 graphics queue. It wraps
the D3D12VA NV12/P010 base layer, an optional Profile 7 enhancement layer, and
the `R16G16B16A16_FLOAT` swap-chain buffer without copying pixels or creating
shared handles. The resource state contract is `COMMON -> COMMON` for decode
surfaces and `PRESENT -> PRESENT` for the back buffer.

Profiles 5 and 8 use libplacebo's BL+RPU reshape. Profile 7 uses BL+EL/FEL+NLQ
when FFmpeg provides a separate enhancement surface and the RPU contains a
non-trivial LINEAR_DZ mapping; otherwise it safely falls back to BL+RPU. The
target remains scRGB, so the existing D3D12 subtitle/UI overlay and swap-chain
presentation stages are unchanged.

Frame interpolation keeps using the native D3D12 reconstruction path so source
and generated frames stay in the same canonical BT.2020 PQ domain. Any bridge
load or per-frame rendering failure falls back to the native D3D12 DV shader.
Use `ANVIL_LIBPLACEBO_DLL` to test another API-compatible DLL explicitly.

## Overlay lifetime

Subtitle and UI bitmaps are the only CPU-authored pixels in the native path.
Each swap-chain buffer owns persistent overlay textures and persistently mapped
upload buffers. Pixels are uploaded only when the bitmap serial/identity or
dimensions change; unchanged subtitles and UI are sampled without resource
creation or re-upload. They are drawn in the same graphics command list as the
final video composition.
