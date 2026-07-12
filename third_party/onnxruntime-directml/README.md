# ONNX Runtime DirectML

This directory contains the native headers, import library, and x64 runtime
DLLs from `Microsoft.ML.OnnxRuntime.DirectML` NuGet package version 1.24.4.

- Upstream: https://github.com/microsoft/onnxruntime
- Package: https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime.DirectML/1.24.4
- License: MIT; see `LICENSE.txt` and `ThirdPartyNotices.txt`.

The checked-in native files keep normal local and CI builds deterministic.
When updating the package, update the version here and validate the bundled
RIFE model with the new DirectML execution provider before committing it.
