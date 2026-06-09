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

// Mouse aiming helper
void MoveMouseTo(int x, int y) {
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    // Map pixels to SendInput absolute space [0, 65535]
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    input.mi.dx = static_cast<LONG>((x * 65536) / screenWidth);
    input.mi.dy = static_cast<LONG>((y * 65536) / screenHeight);
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}

// Relative mouse movement helper (ideal for 3D/FPS games)
void MoveMouseRelative(int dx, int dy) {
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(dx);
    input.mi.dy = static_cast<LONG>(dy);
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
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

    // Track frame rate metrics
    auto lastFpsTime = std::chrono::high_resolution_clock::now();
    int frameCount = 0;

    while (g_running) {
        auto loopStart = std::chrono::high_resolution_clock::now();

        // Dynamically load selected model if changed
        std::wstring modelToLoad = g_config.selectedModelPath;
        if (!modelToLoad.empty() && modelToLoad != loadedModelPath) {
            if (detector.LoadModel(modelToLoad)) {
                loadedModelPath = modelToLoad;
            }
        }

        // Check for still frame capture request
        if (g_config.requestStillFrame) {
            std::lock_guard<std::mutex> lock(overlay->GetD3DMutex());
            // Capture a fresh frame
            capture.CaptureFrame(desktopTexture);
            if (desktopTexture) {
                // Copy to Mat
                capture.CaptureFullToMat(desktopTexture, overlay->m_stillFrameMat);

                // Create ID3D11ShaderResourceView
                D3D11_TEXTURE2D_DESC desc;
                desktopTexture->GetDesc(&desc);
                
                D3D11_TEXTURE2D_DESC copyDesc = desc;
                copyDesc.Usage = D3D11_USAGE_DEFAULT;
                copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                copyDesc.CPUAccessFlags = 0;
                copyDesc.MiscFlags = 0;

                Microsoft::WRL::ComPtr<ID3D11Texture2D> stillTexture;
                HRESULT hr = overlay->GetDevice()->CreateTexture2D(&copyDesc, nullptr, &stillTexture);
                if (SUCCEEDED(hr)) {
                    overlay->GetDeviceContext()->CopyResource(stillTexture.Get(), desktopTexture.Get());
                    overlay->GetDeviceContext()->Flush();

                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Format = desc.Format;
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MostDetailedMip = 0;
                    srvDesc.Texture2D.MipLevels = 1;

                    overlay->m_stillFrameSRV.Reset();
                    hr = overlay->GetDevice()->CreateShaderResourceView(stillTexture.Get(), &srvDesc, &overlay->m_stillFrameSRV);
                    if (SUCCEEDED(hr)) {
                        overlay->m_stillFrameCaptured = true;
                    }
                }
            }
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
            bool processed = false;

            if (g_config.visionMode == COLOR_VISION) {
                // Color Vision Mode
                t0 = std::chrono::high_resolution_clock::now();
                cv::Mat fovMat;
                bool captured = false;
                {
                    std::lock_guard<std::mutex> lock(overlay->GetD3DMutex());
                    captured = capture.CaptureFovToMat(desktopTexture, g_config.fovSize, fovMat);
                }
                auto t1 = std::chrono::high_resolution_clock::now();
                g_config.preprocessTimeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();

                if (captured && !fovMat.empty()) {
                    t0 = std::chrono::high_resolution_clock::now();
                    
                    int r = g_config.colorTargetR;
                    int g = g_config.colorTargetG;
                    int b = g_config.colorTargetB;
                    int tol = g_config.colorTolerance;

                    // Match Target Color with Tolerance in BGRA format
                    cv::Mat mask;
                    cv::Scalar lower(std::max(0, b - tol), std::max(0, g - tol), std::max(0, r - tol), 0);
                    cv::Scalar upper(std::min(255, b + tol), std::min(255, g + tol), std::min(255, r + tol), 255);
                    cv::inRange(fovMat, lower, upper, mask);

                    std::vector<std::vector<cv::Point>> contours;
                    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

                    int startX = (capture.GetWidth() - g_config.fovSize) / 2;
                    int startY = (capture.GetHeight() - g_config.fovSize) / 2;

                    for (const auto& contour : contours) {
                        double area = cv::contourArea(contour);
                        if (area < g_config.colorMinArea) continue;

                        cv::Rect localBox = cv::boundingRect(contour);
                        Detection det;
                        det.box = cv::Rect(
                            startX + localBox.x,
                            startY + localBox.y,
                            localBox.width,
                            localBox.height
                        );
                        det.confidence = 1.0f;
                        det.classId = 0;
                        det.label = "Color Target";
                        localDetections.push_back(det);
                    }

                    t1 = std::chrono::high_resolution_clock::now();
                    g_config.inferenceTimeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
                    processed = true;
                } else {
                    g_config.inferenceTimeMs = 0.0f;
                }
            } else {
                // AI Vision Mode (ONNX / YOLO)
                int inputW = detector.IsLoaded() ? detector.GetInputWidth() : 640;
                int inputH = detector.IsLoaded() ? detector.GetInputHeight() : 640;
                bool preprocessed = false;
                
                {
                    std::lock_guard<std::mutex> lock(overlay->GetD3DMutex());
                    // Register texture with CUDA if first time or resized
                    if (!textureRegistered) {
                        cudaProc.RegisterTexture(desktopTexture.Get());
                        textureRegistered = true;
                    }

                    // GPU Preprocessing (cropping, scaling to detector input size, normalizing, planar copy)
                    t0 = std::chrono::high_resolution_clock::now();
                    preprocessed = cudaProc.ProcessFrame(
                        capture.GetWidth(), capture.GetHeight(),
                        g_config.fovSize,
                        inputW, inputH,
                        d_inputBuffer
                    );
                    auto t1 = std::chrono::high_resolution_clock::now();
                    g_config.preprocessTimeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
                }

                if (preprocessed && detector.IsLoaded()) {
                    t0 = std::chrono::high_resolution_clock::now();
                    detector.Detect(
                        d_inputBuffer,
                        capture.GetWidth(), capture.GetHeight(),
                        g_config.fovSize,
                        g_config.confThreshold,
                        localDetections
                    );
                    auto t1 = std::chrono::high_resolution_clock::now();
                    g_config.inferenceTimeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
                    processed = true;
                }
            }

            // Common Mouse Control and Aiming logic
            if (processed) {
                // Sort detections by distance to screen center (crosshair) so the closest target is prioritized
                float centerX = capture.GetWidth() / 2.0f;
                float centerY = capture.GetHeight() / 2.0f;
                std::sort(localDetections.begin(), localDetections.end(), [centerX, centerY](const Detection& a, const Detection& b) {
                    float distA = std::sqrt(std::pow((a.box.x + a.box.width / 2.0f) - centerX, 2) + std::pow((a.box.y + a.box.height / 2.0f) - centerY, 2));
                    float distB = std::sqrt(std::pow((b.box.x + b.box.width / 2.0f) - centerX, 2) + std::pow((b.box.y + b.box.height / 2.0f) - centerY, 2));
                    return distA < distB;
                });

                // Mouse Control (Assistive Pointing)
                bool aiming = false;
                static bool g_aimbot_was_aiming = false;
                static float g_initial_aim_dist = 0.f;
                static float g_curve_direction = 1.f;
                static float aim_error_x = 0.f;
                static float aim_error_y = 0.f;
                static int lastTargetX = -1;
                static int lastTargetY = -1;

                if (g_config.autoAim && !localDetections.empty()) {
                    if (GetAsyncKeyState(g_config.hotkeyKey) & 0x8000) {
                        aiming = true;
                        
                        // Select target closest to screen center
                        float closestDist = 999999.0f;
                        Detection bestTarget;
                        bool foundTarget = false;

                        int count = 0;
                        for (const auto& det : localDetections) {
                            if (count >= g_config.maxDetections) break;
                            
                            float targetX = det.box.x + det.box.width / 2.0f;
                            float targetY = det.box.y + det.box.height / 2.0f;
                            
                            // Check if target is inside FOV boundary
                            float halfFov = g_config.fovSize / 2.0f;
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
                            count++;
                        }

                        if (foundTarget) {
                            float targetCenterX = bestTarget.box.x + bestTarget.box.width / 2.0f;
                            float targetCenterY = bestTarget.box.y + bestTarget.box.height / 2.0f;

                            float deltaX = targetCenterX - centerX;
                            float deltaY = targetCenterY - centerY;
                            float dist = std::sqrt(deltaX * deltaX + deltaY * deltaY);

                            // Detect target switch or new aim sequence
                            if (std::abs(targetCenterX - lastTargetX) > 10 || std::abs(targetCenterY - lastTargetY) > 10 || !g_aimbot_was_aiming) {
                                g_initial_aim_dist = dist;
                                g_curve_direction = (((float)rand() / RAND_MAX) > 0.5f) ? 1.0f : -1.0f;
                                aim_error_x = aim_error_y = 0.0f;
                            }
                            lastTargetX = static_cast<int>(targetCenterX);
                            lastTargetY = static_cast<int>(targetCenterY);
                            g_aimbot_was_aiming = true;

                            if (dist > 0.1f) {
                                if (g_config.aimbot_humanized) {
                                    // Curved path
                                    if (g_config.aimbot_curve_strength > 0.0f && g_initial_aim_dist > 5.0f) {
                                        float t = dist / g_initial_aim_dist;
                                        if (t > 1.0f) t = 1.0f;
                                        
                                        float curveScale = 4.0f * t * (1.0f - t);
                                        float curveOffsetVal = curveScale * g_config.aimbot_curve_strength * (g_initial_aim_dist * 0.05f);
                                        
                                        if (curveOffsetVal > g_config.aimbot_curve_strength * 8.0f) 
                                            curveOffsetVal = g_config.aimbot_curve_strength * 8.0f;
                                            
                                        float perpX = -deltaY;
                                        float perpY = deltaX;
                                        float perpLength = std::sqrt(perpX * perpX + perpY * perpY);
                                        if (perpLength > 0.1f) {
                                            perpX /= perpLength;
                                            perpY /= perpLength;
                                            
                                            deltaX += perpX * g_curve_direction * curveOffsetVal;
                                            deltaY += perpY * g_curve_direction * curveOffsetVal;
                                            dist = std::sqrt(deltaX * deltaX + deltaY * deltaY);
                                        }
                                    }

                                    // Smoothing with ease-in / ease-out
                                    float currentSmooth = g_config.aimbot_smooth;
                                    float fov_in_pixels = g_config.fovSize / 2.0f;
                                    if (fov_in_pixels < 1.0f) fov_in_pixels = 1.0f;
                                    float normDist = dist / fov_in_pixels;
                                    if (normDist > 1.0f) normDist = 1.0f;
                                    
                                    float easeOutTerm = (1.0f - normDist) * g_config.aimbot_ease_out * 12.0f;
                                    float easeInTerm = normDist * g_config.aimbot_ease_in * 6.0f;
                                    
                                    currentSmooth = g_config.aimbot_smooth * (1.0f + easeOutTerm + easeInTerm);
                                    if (currentSmooth < 1.0f) currentSmooth = 1.0f;

                                    float move_x = deltaX / currentSmooth;
                                    float move_y = deltaY / currentSmooth;

                                    aim_error_x += move_x;
                                    aim_error_y += move_y;

                                    // Micro-jitter
                                    if (g_config.aimbot_jitter > 0.0f && dist > 1.0f) {
                                        float jitterX = (((float)rand() / RAND_MAX) * 2.0f - 1.0f) * g_config.aimbot_jitter;
                                        float jitterY = (((float)rand() / RAND_MAX) * 2.0f - 1.0f) * g_config.aimbot_jitter;
                                        aim_error_x += jitterX;
                                        aim_error_y += jitterY;
                                    }

                                    int dx = static_cast<int>(aim_error_x);
                                    int dy = static_cast<int>(aim_error_y);

                                    // Overshoot prevention
                                    if (std::abs(dx) > (int)(std::fabs(deltaX) + 1.0f)) dx = (int)deltaX;
                                    if (std::abs(dy) > (int)(std::fabs(deltaY) + 1.0f)) dy = (int)deltaY;

                                    aim_error_x -= static_cast<float>(dx);
                                    aim_error_y -= static_cast<float>(dy);

                                    if (dx != 0 || dy != 0) {
                                        if (g_config.aimbot_relative) {
                                            int rel_dx = static_cast<int>(dx * g_config.aimbot_sensitivity);
                                            int rel_dy = static_cast<int>(dy * g_config.aimbot_sensitivity);
                                            if (rel_dx == 0 && dx != 0) rel_dx = (dx > 0) ? 1 : -1;
                                            if (rel_dy == 0 && dy != 0) rel_dy = (dy > 0) ? 1 : -1;
                                            MoveMouseRelative(rel_dx, rel_dy);
                                        } else {
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
                                        if (rel_dx != 0 || rel_dy != 0) {
                                            MoveMouseRelative(rel_dx, rel_dy);
                                        }
                                    } else {
                                        MoveMouseTo(static_cast<int>(centerX + move_x), static_cast<int>(centerY + move_y));
                                    }
                                }
                            }
                        }
                    } else {
                        g_aimbot_was_aiming = false;
                    }
                } else {
                    g_aimbot_was_aiming = false;
                }
                g_config.isAimingActive = aiming;

                // Safely update global detections for overlay drawing
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

    // 2. Scan for ONNX models
    g_config.availableModels = Detector::ScanModelsDir();
    if (!g_config.availableModels.empty()) {
        g_config.selectedModelPath = g_config.availableModels[0];
    } else {
        std::cout << "Warning: No .onnx models found in ./models. Please copy models into the folder.\n";
    }

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
