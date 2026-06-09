# TensorRT runtime DLLs (optional backend)

These DLLs back the **"Use TensorRT"** toggle in the app. `CMakeLists.txt` copies every
`*.dll` here into `build/Release/` on each build, so the toggle survives a clean build.

They are **not committed to git** — `nvinfer_10.dll` (~358 MB) and the builder resources
(~230–250 MB each) exceed GitHub's 100 MB per-file limit, and `*.dll` is gitignored anyway.
So on a **fresh clone this folder is empty** and the app falls back to the CUDA backend.

## Re-fetching them

The version must match what ONNX Runtime links against. ORT 1.26's
`onnxruntime_providers_tensorrt.dll` imports **`nvinfer_10.dll` / `nvonnxparser_10.dll`**, so
use **TensorRT 10.x** (NOT 11.x). Pull the libs wheel from NVIDIA's pip index and extract:

```powershell
pip download tensorrt-cu13-libs==10.16.1.11 --index-url https://pypi.nvidia.com --no-deps --dest $env:TEMP\trt
Expand-Archive (Get-ChildItem "$env:TEMP\trt\*.whl") "$env:TEMP\trt\x" -Force
# copy only what we need into this folder:
Copy-Item "$env:TEMP\trt\x\tensorrt_libs\nvinfer_10.dll"        $PSScriptRoot
Copy-Item "$env:TEMP\trt\x\tensorrt_libs\nvinfer_plugin_10.dll" $PSScriptRoot
Copy-Item "$env:TEMP\trt\x\tensorrt_libs\nvonnxparser_10.dll"   $PSScriptRoot
# GPU-arch builder resource: sm120 = Blackwell (RTX 50xx). Pick the one(s) for your GPU,
# plus the ptx fallback. Other-arch resources are ~1.5 GB total and not needed.
Copy-Item "$env:TEMP\trt\x\tensorrt_libs\nvinfer_builder_resource_sm120_10.dll" $PSScriptRoot
Copy-Item "$env:TEMP\trt\x\tensorrt_libs\nvinfer_builder_resource_ptx_10.dll"   $PSScriptRoot
```

`nvinfer_builder_resource_smXX_10.dll` must match your GPU's compute capability (sm_120 =
Blackwell, sm_89 = Ada/RTX 40xx, sm_86 = Ampere/RTX 30xx, etc.). The first TensorRT model
load builds an engine (minutes) cached under `build/Release/trt_engine_cache/`.
