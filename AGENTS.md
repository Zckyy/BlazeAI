# AGENTS.md

Guidance for AI agents (and humans) working in this repository.

## What this is

BlazeAI is a Windows-only, real-time **screen analysis + assistive pointing** tool written
in C++17 with CUDA. It captures the primary monitor, runs object detection (YOLOv5/v8 via
ONNX Runtime CUDA EP) or color-blob detection on a center field-of-view (FOV) region, draws
a transparent click-through HUD over the screen, and can nudge the mouse toward the nearest
detected target while a hotkey is held.

The CMake target/executable is named `ScreenAnalysisTool` (the product/brand is "BlazeAI").

## Build & run

The toolchain (CMake, CUDA, MSVC, ONNX Runtime, OpenCV via vcpkg) is expected to be present.

```powershell
# Configure + build Release (preferred — handles ORT paths automatically)
./build.ps1

# Or, against an already-configured build dir:
cmake --build build --config Release
```

- Output: `build/Release/ScreenAnalysisTool.exe`. ORT DLLs are auto-copied next to it.
- `build.ps1` looks for `onnxruntime-win-x64-gpu-1.26.0` in the project root **or its parent**.
- Models: drop `.onnx` files into `build/Release/models/` (created on first run if missing).
- Always do a Release build to verify changes; there is no test suite. A clean build is the
  bar for "done" here.

## Architecture & data flow

```
DXGI Desktop Duplication  ->  D3D11 desktop texture (full screen, BGRA)
        │
        ├─ AI Vision:   CUDA-D3D11 interop maps the texture -> PreprocessKernel crops the
        │               FOV, bilinear-resizes to the model input, normalizes, writes CHW
        │               planar float buffer (on-GPU) -> ONNX Runtime CUDA EP inference
        │               -> CPU postprocess (YOLO decode + NMS) -> Detections
        │
        └─ Color Vision: CopySubresourceRegion of the FOV -> CPU cv::Mat -> cv::inRange +
                         findContours -> Detections

Detections (absolute screen coords) -> ApplyAimAssist (mouse) + overlay HUD draw
```

Two threads share one D3D11 device/context:
- **Main thread** (`main`): Win32 message pump + ImGui HUD rendering + Present (VSync).
- **Processing thread** (`ProcessingThread`): capture + preprocess + inference + aiming.

## File map (`src/`)

| File | Responsibility |
|------|----------------|
| `main.cpp` | Entry point, the two threads, and the per-frame pipeline. Helpers: `CaptureStillFrame`, `RunColorVision`, `RunAiVision`, `ApplyAimAssist`. `AppConfig` is the shared state (defined in `overlay.h`). |
| `capture.{h,cpp}` | `DXGICapture`: desktop duplication, full-frame copy, FOV-crop-to-`cv::Mat`, full-frame-to-`cv::Mat`. Handles duplication reset on device loss / `WAIT_TIMEOUT`. |
| `cuda_process.{h,cu}` | `CUDAProcessor`: registers the D3D texture with CUDA and runs `PreprocessKernel`. Caches the `cudaTextureObject_t` across frames. |
| `detector.{h,cpp}` | `Detector`: ONNX Runtime session (CUDA EP), model scanning, YOLOv5/v8 auto-detected decode, NMS, COCO class names. Input tensor is fed **directly from the GPU buffer** (zero-copy). |
| `overlay.{h,cpp}` | `Overlay`: transparent topmost D3D11 window, ImGui setup, config panel, detection/FOV drawing, the color-picker magnifier. Also defines `AppConfig` and `Detection` consumers. |

## Key invariants & gotchas (read before changing)

- **Coordinate spaces.** Detections are stored in **absolute screen pixels**. The FOV is a
  square of `fovSize` centered on the screen; crop origin is `((W-fovSize)/2, (H-fovSize)/2)`.
  Model output is in network pixels and is mapped network -> FOV -> screen in `Detector::Detect`.
  If you touch any scaling, keep all three stages consistent.
- **YOLO format auto-detect.** In `Detector::Detect`, a 3-D output with `dim1 < dim2` is
  treated as YOLOv8 `[1, 4+classes, anchors]`; otherwise YOLOv5 `[1, anchors, 5+classes]`.
- **Pixel layout is BGRA.** DXGI gives `DXGI_FORMAT_B8G8R8A8_UNORM`. CUDA reads `float4` as
  (B,G,R,A); OpenCV `cv::Vec4b` is (B,G,R,A). Swap to RGB only where noted.
- **Shared D3D11 context is not thread-safe.** Every use of the device context from either
  thread must hold `overlay->GetD3DMutex()`. `Present()` is intentionally called *outside*
  the lock. Don't add unlocked context calls.
- **CUDA texture-object cache.** `CUDAProcessor` reuses its `cudaTextureObject_t` while the
  mapped `cudaArray` is unchanged; it is destroyed in `UnregisterTexture`. The registered
  D3D texture (`desktopTexture`) is created once and reused, which is what makes this valid.
- **`g_config` is shared without locking.** It is plain scalars written by the UI thread and
  read by the processing thread (and vice-versa for telemetry). This is a deliberate,
  pre-existing lossy/racy design — fine for scalars on x86, but do **not** add non-trivial
  (non-atomic, multi-word) state to it and assume safe cross-thread access.
- **Detections handoff** goes through `g_detectionsMutex` (`g_detections`). The processing
  thread sorts by distance-to-crosshair (squared distance) before publishing, so consumers
  can assume "nearest first".
- **`ApplyAimAssist`** keeps its smoothing/curve/jitter state in a `static AimState` across
  frames; relative vs absolute mouse movement and the humanized path are all config-driven.

## Conventions

- Match the surrounding style: 4-space indent, `m_` members, `g_` globals, early-return
  guards, comments that explain *why* (coordinate mapping, threading) not *what*.
- Prefer extracting cohesive helpers over growing the per-frame loop.
- After any change, run a Release build and confirm it is clean before reporting done.
