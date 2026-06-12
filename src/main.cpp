#include <windows.h>
#include <thread>
#include <mutex>
#include <chrono>
#include <iostream>
#include <filesystem>
#include <atomic>

#include "overlay.h"
#include "capture.h"
#include "cuda_process.h"
#include "detector.h"
#include "config_io.h"
#include "input.h"
#include "makcu.h"

// Threading and Synchronization
std::atomic<bool> g_running{true};
std::mutex g_detectionsMutex;
std::vector<Detection> g_detections;
AppConfig g_config;

// High-precision sleep utility
void PreciseSleep(double milliseconds) {
    static HANDLE timer = CreateWaitableTimer(NULL, TRUE, NULL);
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(-milliseconds * 10000.0); // 100ns units (negative for relative)
    SetWaitableTimer(timer, &li, 0, NULL, NULL, 0);
    WaitForSingleObject(timer, INFINITE);
}

// Mouse aiming helper. Maps pixels to SendInput absolute space [0, 65535].
void MoveMouseTo(int x, int y) {
    // Screen metrics are constant for the session; cache to avoid a syscall per move.
    static const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    static const int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>((x * 65536) / screenWidth);
    input.mi.dy = static_cast<LONG>((y * 65536) / screenHeight);
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}

// Relative mouse movement helper (ideal for 3D/FPS games)
void MoveMouseRelative(int dx, int dy) {
    if (g_config.mouseInputMethod == MOUSE_NTUSERINJECT && g_ntInput.Available()) {
        g_ntInput.MoveRelative(dx, dy);
        return;
    }

    if (g_config.mouseInputMethod == MOUSE_MAKCU && g_makcu.IsConnected()) {
        g_makcu.MoveRelative(dx, dy);
        return;
    }

    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(dx);
    input.mi.dy = static_cast<LONG>(dy);
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}

// Persistent state for the humanized aim-assist controller, carried across frames.
struct AimState {
    bool  wasAiming      = false;
    float initialAimDist = 0.0f;
    float curveDirection = 1.0f;
    float errorX         = 0.0f; // Sub-pixel movement accumulators
    float errorY         = 0.0f;
    int   lastTargetX    = -1;
    int   lastTargetY    = -1;
};

// Captures a fresh full-screen still frame into the overlay for the color picker tool.
// Builds both a CPU cv::Mat (for pixel sampling) and a D3D SRV (for on-screen display).
static void CaptureStillFrame(Overlay* overlay, DXGICapture& capture,
                              Microsoft::WRL::ComPtr<ID3D11Texture2D>& desktopTexture) {
    std::lock_guard<std::mutex> lock(overlay->GetD3DMutex());

    capture.CaptureFrame(desktopTexture);
    if (!desktopTexture) return;

    capture.CaptureFullToMat(desktopTexture, overlay->m_stillFrameMat);

    D3D11_TEXTURE2D_DESC desc;
    desktopTexture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC copyDesc = desc;
    copyDesc.Usage = D3D11_USAGE_DEFAULT;
    copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copyDesc.CPUAccessFlags = 0;
    copyDesc.MiscFlags = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> stillTexture;
    if (FAILED(overlay->GetDevice()->CreateTexture2D(&copyDesc, nullptr, &stillTexture))) return;

    overlay->GetDeviceContext()->CopyResource(stillTexture.Get(), desktopTexture.Get());
    overlay->GetDeviceContext()->Flush();

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    overlay->m_stillFrameSRV.Reset();
    if (SUCCEEDED(overlay->GetDevice()->CreateShaderResourceView(stillTexture.Get(), &srvDesc, &overlay->m_stillFrameSRV))) {
        overlay->m_stillFrameCaptured = true;
    }
}

// Color Vision mode: extracts the center FOV to CPU and finds blobs matching the target color.
// Fills `out` with detections in absolute screen coordinates. Returns true if processing ran.
static bool RunColorVision(DXGICapture& capture, Overlay* overlay,
                           Microsoft::WRL::ComPtr<ID3D11Texture2D>& desktopTexture,
                           std::vector<Detection>& out) {
    auto t0 = std::chrono::high_resolution_clock::now();
    cv::Mat fovMat;
    bool captured = false;
    {
        std::lock_guard<std::mutex> lock(overlay->GetD3DMutex());
        captured = capture.CaptureFovToMat(desktopTexture, g_config.fovSize, fovMat);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    g_config.preprocessTimeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();

    if (!captured || fovMat.empty()) {
        g_config.inferenceTimeMs = 0.0f;
        return false;
    }

    t0 = std::chrono::high_resolution_clock::now();

    const int r = g_config.colorTargetR;
    const int g = g_config.colorTargetG;
    const int b = g_config.colorTargetB;
    const int tol = g_config.colorTolerance;

    // Match target color with tolerance in BGRA format
    cv::Mat mask;
    cv::Scalar lower(std::max(0, b - tol), std::max(0, g - tol), std::max(0, r - tol), 0);
    cv::Scalar upper(std::min(255, b + tol), std::min(255, g + tol), std::min(255, r + tol), 255);
    cv::inRange(fovMat, lower, upper, mask);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    const int startX = (capture.GetWidth() - g_config.fovSize) / 2;
    const int startY = (capture.GetHeight() - g_config.fovSize) / 2;

    for (const auto& contour : contours) {
        if (cv::contourArea(contour) < g_config.colorMinArea) continue;

        cv::Rect localBox = cv::boundingRect(contour);
        Detection det;
        det.box = cv::Rect(startX + localBox.x, startY + localBox.y, localBox.width, localBox.height);
        det.confidence = 1.0f;
        det.classId = 0;
        det.label = "Color Target";
        out.push_back(det);
    }

    t1 = std::chrono::high_resolution_clock::now();
    g_config.inferenceTimeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
    return true;
}

// AI Vision mode: GPU-preprocesses the FOV (crop/resize/normalize) and runs ONNX YOLO inference.
// Fills `out` with detections in absolute screen coordinates. Returns true if inference ran.
static bool RunAiVision(DXGICapture& capture, CUDAProcessor& cudaProc, Detector& detector,
                        Overlay* overlay, Microsoft::WRL::ComPtr<ID3D11Texture2D>& desktopTexture,
                        float* d_inputBuffer, bool& textureRegistered,
                        std::vector<Detection>& out) {
    const int inputW = detector.IsLoaded() ? detector.GetInputWidth() : 640;
    const int inputH = detector.IsLoaded() ? detector.GetInputHeight() : 640;

    bool preprocessed = false;
    {
        std::lock_guard<std::mutex> lock(overlay->GetD3DMutex());
        // Register texture with CUDA on first use
        if (!textureRegistered) {
            cudaProc.RegisterTexture(desktopTexture.Get());
            textureRegistered = true;
        }

        // GPU preprocessing (crop, scale to detector input, normalize, planar CHW copy)
        auto t0 = std::chrono::high_resolution_clock::now();
        preprocessed = cudaProc.ProcessFrame(
            capture.GetWidth(), capture.GetHeight(),
            g_config.fovSize, inputW, inputH, d_inputBuffer);
        auto t1 = std::chrono::high_resolution_clock::now();
        g_config.preprocessTimeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
    }

    if (!preprocessed || !detector.IsLoaded()) return false;

    auto t0 = std::chrono::high_resolution_clock::now();
    detector.Detect(d_inputBuffer, capture.GetWidth(), capture.GetHeight(),
                    g_config.fovSize, g_config.confThreshold, out);
    auto t1 = std::chrono::high_resolution_clock::now();
    g_config.inferenceTimeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
    return true;
}

// Applies assistive aiming toward the detection nearest the crosshair within the FOV.
// `detections` should be sorted by proximity to (centerX, centerY). Returns true while
// the hotkey is held and aiming is active.
static bool ApplyAimAssist(const std::vector<Detection>& detections,
                           float centerX, float centerY, AimState& s) {
    g_config.aimDebugTargetActive = false;

    if (!g_config.autoAim || detections.empty() || !(GetAsyncKeyState(g_config.hotkeyKey) & 0x8000)) {
        s.wasAiming = false;
        return false;
    }

    // Select target closest to screen center within the FOV boundary
    float closestDist = 999999.0f;
    Detection bestTarget;
    bool foundTarget = false;
    const float halfFov = g_config.fovSize / 2.0f;

    int count = 0;
    for (const auto& det : detections) {
        if (count >= g_config.maxDetections) break;
        ++count;

        float targetX = det.box.x + det.box.width / 2.0f;
        float targetY = det.box.y + det.box.height / 2.0f;

        if (targetX >= (centerX - halfFov) && targetX <= (centerX + halfFov) &&
            targetY >= (centerY - halfFov) && targetY <= (centerY + halfFov)) {
            float dist = std::sqrt((targetX - centerX) * (targetX - centerX) +
                                   (targetY - centerY) * (targetY - centerY));
            if (dist < closestDist) {
                closestDist = dist;
                bestTarget = det;
                foundTarget = true;
            }
        }
    }

    if (!foundTarget) return true; // Hotkey held but no in-FOV target

    float targetCenterX = bestTarget.box.x + bestTarget.box.width / 2.0f;
    float targetCenterY = bestTarget.box.y + bestTarget.box.height / 2.0f;

    float deltaX = targetCenterX - centerX;
    float deltaY = targetCenterY - centerY;
    float dist = std::sqrt(deltaX * deltaX + deltaY * deltaY);

    g_config.aimDebugTargetActive = true;
    g_config.aimDebugTargetX = targetCenterX;
    g_config.aimDebugTargetY = targetCenterY;
    g_config.aimDebugDeltaX = deltaX;
    g_config.aimDebugDeltaY = deltaY;
    g_config.aimDebugDistance = dist;
    g_config.aimDebugSmooth = std::max(1.0f, g_config.aimbot_smooth);
    g_config.aimDebugMoveX = 0.0f;
    g_config.aimDebugMoveY = 0.0f;

    // Detect target switch or new aim sequence
    if (std::abs(targetCenterX - s.lastTargetX) > 10 || std::abs(targetCenterY - s.lastTargetY) > 10 || !s.wasAiming) {
        s.initialAimDist = dist;
        s.curveDirection = (((float)rand() / RAND_MAX) > 0.5f) ? 1.0f : -1.0f;
        s.errorX = s.errorY = 0.0f;
    }
    s.lastTargetX = static_cast<int>(targetCenterX);
    s.lastTargetY = static_cast<int>(targetCenterY);
    s.wasAiming = true;

    if (dist <= 0.1f) return true;

    if (g_config.aimbot_humanized) {
        // Curved path
        if (g_config.aimbot_curve_strength > 0.0f && s.initialAimDist > 5.0f) {
            float t = dist / s.initialAimDist;
            if (t > 1.0f) t = 1.0f;

            float curveScale = 4.0f * t * (1.0f - t);
            float curveOffsetVal = curveScale * g_config.aimbot_curve_strength * (s.initialAimDist * 0.05f);

            if (curveOffsetVal > g_config.aimbot_curve_strength * 8.0f)
                curveOffsetVal = g_config.aimbot_curve_strength * 8.0f;

            float perpX = -deltaY;
            float perpY = deltaX;
            float perpLength = std::sqrt(perpX * perpX + perpY * perpY);
            if (perpLength > 0.1f) {
                perpX /= perpLength;
                perpY /= perpLength;

                deltaX += perpX * s.curveDirection * curveOffsetVal;
                deltaY += perpY * s.curveDirection * curveOffsetVal;
                dist = std::sqrt(deltaX * deltaX + deltaY * deltaY);
            }
        }

        // Smoothing with ease-in / ease-out
        float fov_in_pixels = g_config.fovSize / 2.0f;
        if (fov_in_pixels < 1.0f) fov_in_pixels = 1.0f;
        float normDist = dist / fov_in_pixels;
        if (normDist > 1.0f) normDist = 1.0f;

        float easeOutTerm = (1.0f - normDist) * g_config.aimbot_ease_out * 12.0f;
        float easeInTerm = normDist * g_config.aimbot_ease_in * 6.0f;

        float currentSmooth = g_config.aimbot_smooth * (1.0f + easeOutTerm + easeInTerm);
        if (currentSmooth < 1.0f) currentSmooth = 1.0f;
        g_config.aimDebugSmooth = currentSmooth;

        s.errorX += deltaX / currentSmooth;
        s.errorY += deltaY / currentSmooth;

        // Micro-jitter
        if (g_config.aimbot_jitter > 0.0f && dist > 1.0f) {
            s.errorX += (((float)rand() / RAND_MAX) * 2.0f - 1.0f) * g_config.aimbot_jitter;
            s.errorY += (((float)rand() / RAND_MAX) * 2.0f - 1.0f) * g_config.aimbot_jitter;
        }

        int dx = static_cast<int>(s.errorX);
        int dy = static_cast<int>(s.errorY);

        // Overshoot prevention
        if (std::abs(dx) > (int)(std::fabs(deltaX) + 1.0f)) dx = (int)deltaX;
        if (std::abs(dy) > (int)(std::fabs(deltaY) + 1.0f)) dy = (int)deltaY;

        s.errorX -= static_cast<float>(dx);
        s.errorY -= static_cast<float>(dy);

        if (dx != 0 || dy != 0) {
            if (g_config.aimbot_relative) {
                int rel_dx = static_cast<int>(dx * g_config.aimbot_sensitivity);
                int rel_dy = static_cast<int>(dy * g_config.aimbot_sensitivity);
                if (rel_dx == 0 && dx != 0) rel_dx = (dx > 0) ? 1 : -1;
                if (rel_dy == 0 && dy != 0) rel_dy = (dy > 0) ? 1 : -1;
                g_config.aimDebugMoveX = static_cast<float>(rel_dx);
                g_config.aimDebugMoveY = static_cast<float>(rel_dy);
                MoveMouseRelative(rel_dx, rel_dy);
            } else {
                g_config.aimDebugMoveX = static_cast<float>(dx);
                g_config.aimDebugMoveY = static_cast<float>(dy);
                MoveMouseTo(static_cast<int>(centerX + dx), static_cast<int>(centerY + dy));
            }
        }
    } else {
        // Basic linear smoothing
        float move_x = deltaX;
        float move_y = deltaY;
        if (g_config.aimbot_smooth > 1.0f) {
            move_x /= g_config.aimbot_smooth;
            move_y /= g_config.aimbot_smooth;
        }

        if (g_config.aimbot_relative) {
            int rel_dx = static_cast<int>(move_x * g_config.aimbot_sensitivity);
            int rel_dy = static_cast<int>(move_y * g_config.aimbot_sensitivity);
            if (rel_dx == 0 && move_x != 0.0f) rel_dx = (move_x > 0.0f) ? 1 : -1;
            if (rel_dy == 0 && move_y != 0.0f) rel_dy = (move_y > 0.0f) ? 1 : -1;
            g_config.aimDebugMoveX = static_cast<float>(rel_dx);
            g_config.aimDebugMoveY = static_cast<float>(rel_dy);
            if (rel_dx != 0 || rel_dy != 0) {
                MoveMouseRelative(rel_dx, rel_dy);
            }
        } else {
            g_config.aimDebugMoveX = move_x;
            g_config.aimDebugMoveY = move_y;
            MoveMouseTo(static_cast<int>(centerX + move_x), static_cast<int>(centerY + move_y));
        }
    }

    return true;
}

// Thread function for real-time capture and inference
void ProcessingThread(Overlay* overlay) {
    // 1. Initialize CUDA interop & ORT Detector
    CUDAProcessor cudaProc;
    Detector detector;

    // Allocate GPU buffer for YOLO input [1, 3, 640, 640]
    float* d_inputBuffer = nullptr;
    const size_t maxBufferSize = 1 * 3 * 640 * 640 * sizeof(float);
    cudaMalloc(&d_inputBuffer, maxBufferSize);

    // Initialize DXGI Capture
    DXGICapture capture(overlay->GetDevice(), overlay->GetDeviceContext());
    if (!capture.Init()) {
        std::cerr << "Failed to initialize DXGI Capture in processing thread\n";
        cudaFree(d_inputBuffer);
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTexture;
    bool textureRegistered = false;
    std::wstring loadedModelPath = L"";
    bool loadedTensorRT = false;
    bool loadedTrtFp16 = true;

    // Track frame rate metrics
    auto lastFpsTime = std::chrono::high_resolution_clock::now();
    int frameCount = 0;

    while (g_running) {
        auto loopStart = std::chrono::high_resolution_clock::now();

        // (Re)load the model when the selection or the TensorRT backend settings change.
        // Switching backend rebuilds the session, so the TRT engine build cost is paid here.
        std::wstring modelToLoad = g_config.selectedModelPath;
        bool backendChanged = (g_config.useTensorRT != loadedTensorRT) ||
                              (g_config.trtFp16 != loadedTrtFp16);
        if (!modelToLoad.empty() && (modelToLoad != loadedModelPath || backendChanged)) {
            // Flag the (potentially multi-minute) load so the UI can show a progress bar.
            // LoadModel blocks this thread; the UI keeps rendering on the main thread.
            g_config.engineBuilding = true;
            bool ok = detector.LoadModel(modelToLoad, g_config.useTensorRT, g_config.trtFp16);
            g_config.engineBuilding = false;
            if (ok) {
                loadedModelPath = modelToLoad;
                loadedTensorRT = g_config.useTensorRT;
                loadedTrtFp16 = g_config.trtFp16;
            }
        }

        // Check for still frame capture request (color picker tool)
        if (g_config.requestStillFrame) {
            CaptureStillFrame(overlay, capture, desktopTexture);
            g_config.requestStillFrame = false;
        }

        // Frame Capture
        auto t0 = std::chrono::high_resolution_clock::now();
        bool newFrame = false;
        {
            std::lock_guard<std::mutex> lock(overlay->GetD3DMutex());
            newFrame = capture.CaptureFrame(desktopTexture);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        g_config.captureTimeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();

        if (newFrame && desktopTexture) {
            std::vector<Detection> localDetections;

            bool processed = (g_config.visionMode == COLOR_VISION)
                ? RunColorVision(capture, overlay, desktopTexture, localDetections)
                : RunAiVision(capture, cudaProc, detector, overlay, desktopTexture,
                              d_inputBuffer, textureRegistered, localDetections);

            // Common mouse control and aiming logic
            if (processed) {
                const float centerX = capture.GetWidth() / 2.0f;
                const float centerY = capture.GetHeight() / 2.0f;

                // Sort detections by squared distance to crosshair so the closest target is first.
                // (Squared distance preserves ordering and avoids per-comparison sqrt/pow.)
                std::sort(localDetections.begin(), localDetections.end(),
                          [centerX, centerY](const Detection& a, const Detection& b) {
                    float ax = (a.box.x + a.box.width / 2.0f) - centerX;
                    float ay = (a.box.y + a.box.height / 2.0f) - centerY;
                    float bx = (b.box.x + b.box.width / 2.0f) - centerX;
                    float by = (b.box.y + b.box.height / 2.0f) - centerY;
                    return (ax * ax + ay * ay) < (bx * bx + by * by);
                });

                static AimState aimState;
                g_config.isAimingActive = ApplyAimAssist(localDetections, centerX, centerY, aimState);

                // Safely publish detections for overlay drawing
                {
                    std::lock_guard<std::mutex> lock(g_detectionsMutex);
                    g_detections = std::move(localDetections);
                }
            }
        }

        // Frame rate calculation
        frameCount++;
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFpsTime).count();
        if (duration >= 1000) {
            g_config.actualFps = static_cast<float>(frameCount * 1000) / duration;
            frameCount = 0;
            lastFpsTime = now;
        }

        // Control high-frequency loop to match configured FPS target
        auto loopEnd = std::chrono::high_resolution_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(loopEnd - loopStart).count();
        double targetPeriodMs = 1000.0 / g_config.targetFps;
        if (elapsedMs < targetPeriodMs) {
            PreciseSleep(targetPeriodMs - elapsedMs);
        }
    }

    // Cleanup GPU resources
    {
        std::lock_guard<std::mutex> lock(overlay->GetD3DMutex());
        cudaProc.UnregisterTexture();
    }
    if (d_inputBuffer) {
        cudaFree(d_inputBuffer);
    }
}

int main() {
    HINSTANCE hInstance = GetModuleHandle(NULL);
    // Enable console logging for easier setup diagnostics
    AllocConsole();
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONOUT$", "w", stderr);
    
    std::cout << "Starting Assistive Pointing Tool...\n";

    // 1. Initialize GUI Overlay
    Overlay overlay;
    if (!overlay.Init(hInstance)) {
        std::cerr << "Failed to initialize overlay window\n";
        return -1;
    }

    // 2. Load persisted settings from config.ini (next to the exe), then scan models.
    LoadConfig(g_config);
    overlay.ApplyStreamProof(g_config.streamProof);

    g_config.availableModels = Detector::ScanModelsDir();
    if (!g_config.availableModels.empty()) {
        // Honor the saved model if it still exists; otherwise fall back to the first.
        bool savedStillExists = false;
        for (const auto& path : g_config.availableModels) {
            if (path == g_config.selectedModelPath) { savedStillExists = true; break; }
        }
        if (!savedStillExists) {
            g_config.selectedModelPath = g_config.availableModels[0];
        }
    } else {
        std::cout << "Warning: No .onnx models found in ./models. Please copy models into the folder.\n";
    }

    // Snapshot of persisted settings; the UI loop saves whenever this diverges.
    AppConfig lastSavedConfig = g_config;

    // 3. Launch background capture and processing thread
    std::thread procThread(ProcessingThread, &overlay);

    // 4. GUI Window Loop
    static bool deletePressedLast = false;
    static bool insertPressedLast = false;
    while (overlay.ProcessMessages()) {
        // Toggle visuals toggle hotkey (Delete Key)
        bool deletePressed = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
        if (deletePressed && !deletePressedLast) {
            g_config.showVisuals = !g_config.showVisuals;
        }
        deletePressedLast = deletePressed;

        // Toggle the ImGui config menu (Insert Key)
        bool insertPressed = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (insertPressed && !insertPressedLast) {
            g_config.showMenu = !g_config.showMenu;
        }
        insertPressedLast = insertPressed;

        {
            std::lock_guard<std::mutex> lock(overlay.GetD3DMutex());
            overlay.BeginFrame();

            if (g_config.showVisuals) {
                // Render configuration panel (toggled by Insert)
                if (g_config.showMenu) {
                    overlay.DrawConfigPanel(g_config);
                }

                // Fetch and draw detection boxes safely
                {
                    std::lock_guard<std::mutex> lockDets(g_detectionsMutex);
                    overlay.DrawDetections(g_detections, g_config);
                }

                // Render color picker overlay if active
                if (g_config.colorPickerActive) {
                    overlay.DrawColorPickerOverlay(g_config);
                }
            }

            overlay.EndFrame();
        }

        // Present outside the mutex lock to synchronize with the monitor's native refresh rate (VSync)
        // without blocking the capture/processing thread.
        overlay.Present();

        // Persist settings whenever the UI (or a toggle hotkey) changed them.
        if (!SettingsEqual(g_config, lastSavedConfig)) {
            SaveConfig(g_config);
            lastSavedConfig = g_config;
        }
    }

    // 5. Terminate and cleanup
    g_running = false;
    if (procThread.joinable()) {
        procThread.join();
    }

    overlay.Shutdown();
    FreeConsole();

    return 0;
}
