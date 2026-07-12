# Frame interpolation model

`rife_v4.25_lite.onnx` is the internally padded v2 ONNX export of the RIFE
4.25 Lite model. It accepts one tensor shaped `[1, 7, height, width]`: RGB for
frame A, RGB for frame B, and a timestep plane. Anvil Player uses `t = 0.5`.

SHA-256: `610B5DE57CDCFBCCE9914C23E60A1CD357779A6F9582A1BCFCB035F8EB38509B`

- Model family: https://github.com/hzwer/Practical-RIFE
- ONNX conversion distribution: https://github.com/AmusementClub/vs-mlrt/releases/tag/external-models
- License: MIT; see `RIFE-LICENSE.txt`.

Keep this model filename stable because the native runtime resolves it relative
to the application executable at `assets/models/rife_v4.25_lite.onnx`.
