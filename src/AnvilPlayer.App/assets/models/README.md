# Frame interpolation model

`rife_v4.25.onnx` is the internally padded v2 ONNX export of the full RIFE
4.25 model. It accepts one tensor shaped `[1, 7, height, width]`: RGB for frame
A, RGB for frame B, and a timestep plane. Anvil Player uses `t = 0.5`.

The native DirectML runtime loads `rife_v4.25_fp16.onnx`, whose inputs,
weights, and output are converted to FP16. Four coordinate-generation casts
remain FP16 so DirectML shape inference and the application-owned FP16 buffer
contract agree.

- FP32 SHA-256: `65C57A5E4ABB17AD67FAF35054291AC53AFFAB0506D509D2E66698F6ECD75584`
- FP16 SHA-256: `E9D960B74B3A27A12C72591E73C7F0996EA0B2ABD70A4402D448A8C110AF4819`

- Model family: https://github.com/hzwer/Practical-RIFE
- ONNX conversion distribution: https://github.com/AmusementClub/vs-mlrt/releases/tag/external-models
- License: MIT; see `RIFE-LICENSE.txt`.

Keep this model filename stable because the native runtime resolves it relative
to the application executable at `assets/models/rife_v4.25_fp16.onnx`.
