#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <mutex>
#include "detector.h"

enum VisionMode {
    AI_VISION = 0,
    COLOR_VISION = 1
};

enum MouseInputMethod {
    MOUSE_SENDINPUT = 0,     // Standard SendInput (works almost everywhere)
    MOUSE_NTUSERINJECT = 1,  // NtUserInjectMouseInput via win32u.dll (relative-only)
    MOUSE_MAKCU = 2,         // External MAKCU serial device (relative-only, hardware-level)
    MOUSE_VIGEM = 3          // Virtual Xbox 360 pad (ViGEmBus) right-stick output, for games
                             // that block mouse input once a controller is the active device.
};

// Configuration struct shared between overlay and loop
struct AppConfig {
    std::wstring selectedModelPath;
    std::vector<std::wstring> availableModels;
    
    int visionMode = AI_VISION;
    int colorTargetR = 255;
    int colorTargetG = 0;
    int colorTargetB = 4;
    int colorTolerance = 5; // Start with a strict default tolerance
    int colorMinArea = 15;
    
    bool useTensorRT = false;       // Use TensorRT EP (faster) instead of plain CUDA EP
    bool trtFp16 = true;            // FP16 ("medium numbers") when TensorRT is on; big free win

    int fovSize = 120;              // 320 or 640
    int targetFps = 170;            // 1 to 400
    int maxDetections = 5;
    float confThreshold = 0.5f;
    bool autoAim = true;
    float aimHeightRatio = 0.5f;    // Vertical aim point within the box: 0 = top edge,
                                    // 0.5 = center mass, ~0.15 = head on full-body boxes
    bool showBoundingBoxes = true;
    bool showFov = true;
    int fovShape = 0;               // 0 = square, 1 = circle (visual indicator only)
    bool showVisuals = true;
    bool streamProof = true;        // Hide overlay from screen capture (WDA_EXCLUDEFROMCAPTURE).
                                    // Default ON: also hides the HUD from our own desktop
                                    // duplication, so overlay drawings can't feed back into
                                    // color/AI vision as false targets.
    bool showMenu = true;           // ImGui config panel visibility (Insert toggles)
    bool showAimVisualizer = false;
    bool showSmoothingTrail = true;
    bool showTargetVector = true;
    bool showRawAimPoint = true;
    bool showSmoothedAimPoint = true;
    // Target tracking (frame-to-frame track IDs; see tracker.h)
    bool trackerEnabled = true;
    float trackerIou = 0.30f;        // Min IoU to match a detection to an existing track
    int trackerMaxMissed = 8;        // Frames a lost track coasts before being dropped
    float trackerSwitchRatio = 0.65f; // Switch targets only if the rival is < this fraction
                                      // of the locked target's crosshair distance

    int hotkeyKey = VK_XBUTTON1;    // Mouse 4 by default
    bool isAimingActive = false;

    // Color picker communication flags
    bool requestStillFrame = false;
    bool colorPickerActive = false;

    // Humanized Smoothing settings (copied from Blazestrike)
    bool aimbot_humanized = false;
    bool aimbot_relative = true;     // Relative mouse movement (Raw input) vs absolute
    int mouseInputMethod = MOUSE_SENDINPUT; // How relative moves are injected (see MouseInputMethod)
    float vigemStickScale = 150.0f;  // Stick units per pixel of dx/dy for MOUSE_VIGEM
    bool vigemAlwaysAim = false;     // MOUSE_VIGEM only: skip the hotkey/trigger check and
                                      // aim whenever a target is detected.
    float aimbot_sensitivity = 1.0f; // Scale factor for relative movement
    float aimbot_smooth = 2.0f;
    float aimbot_jitter = 0.0f;
    float aimbot_curve_strength = 0.0f;
    float aimbot_ease_in = 0.15f;
    float aimbot_ease_out = 0.35f;

    // Trigger bot: while the aim hotkey is held and the locked target's aim point is
    // within `triggerBotRadius` pixels of the crosshair, automatically fires a left click.
    bool triggerBotEnabled = false;
    float triggerBotRadius = 5.0f;   // Max crosshair-to-target distance (px) to fire
    int triggerBotHoldMs = 50;       // How long the click is held down
    int triggerBotCooldownMs = 100;  // Minimum gap between clicks

    float captureTimeMs = 0.0f;
    float preprocessTimeMs = 0.0f;
    float inferenceTimeMs = 0.0f;
    // Preprocess sub-stage breakdown (ms): driver map, kernel+sync, driver unmap.
    float preprocessMapMs = 0.0f;
    float preprocessKernelMs = 0.0f;     // CPU wall-time: launch + sync (includes GPU queue wait)
    float preprocessUnmapMs = 0.0f;
    float preprocessKernelGpuMs = 0.0f;  // True GPU execution time of the preprocess kernel
    float actualFps = 0.0f;

    // Scalar-only processing-thread telemetry consumed by the Aim Debug Visualizer.
    bool aimDebugTargetActive = false;
    float aimDebugTargetX = 0.0f;
    float aimDebugTargetY = 0.0f;
    float aimDebugDeltaX = 0.0f;
    float aimDebugDeltaY = 0.0f;
    float aimDebugDistance = 0.0f;
    float aimDebugSmooth = 1.0f;
    float aimDebugMoveX = 0.0f;
    float aimDebugMoveY = 0.0f;

    // Set by the processing thread while a model is loading (blocks for minutes when the
    // TensorRT engine is being built); the UI shows an indeterminate progress bar meanwhile.
    bool engineBuilding = false;
};

class Overlay {
public:
    Overlay();
    ~Overlay();

    bool Init(HINSTANCE hInstance);
    void Shutdown();

    // Pump Win32 messages
    bool ProcessMessages();

    void BeginFrame();
    void EndFrame();
    void Present();

    // UI Renders
    void DrawConfigPanel(AppConfig& config);
    void DrawDetections(const std::vector<Detection>& detections, const AppConfig& config);
    void DrawColorPickerOverlay(AppConfig& config);

    // Hide/show the overlay window in screen capture (recordings, screenshots,
    // and this app's own desktop duplication). Requires Win10 2004+; no-op below.
    void ApplyStreamProof(bool enabled);

    // Tell the overlay whether the config menu is open. While open, the overlay takes
    // keyboard focus and mouse input away from the game so the cursor appears and the
    // menu is usable; on close, focus is handed back to the previous window.
    void SetMenuOpen(bool open) { m_menuOpen = open; }

    ID3D11Device* GetDevice() { return m_pd3dDevice.Get(); }
    ID3D11DeviceContext* GetDeviceContext() { return m_pd3dDeviceContext.Get(); }
    HWND GetHwnd() const { return m_hwnd; }
    std::mutex& GetD3DMutex() { return m_d3dMutex; }

    int GetWidth() const { return m_screenWidth; }
    int GetHeight() const { return m_screenHeight; }

    // Color picker still frame storage
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_stillFrameSRV;
    cv::Mat m_stillFrameMat;
    bool m_stillFrameCaptured = false;

private:
    std::mutex m_d3dMutex;
    HWND m_hwnd = nullptr;
    bool m_menuOpen = false;
    HWND m_prevForeground = nullptr; // Window to restore focus to when the menu closes
    int m_screenWidth = 0;
    int m_screenHeight = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> m_pd3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_pd3dDeviceContext;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_pSwapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_mainRenderTargetView;

    // DirectComposition chain that hosts the flip-model swap chain. DWM composites the
    // swap chain buffer directly (premultiplied alpha), skipping the legacy layered-window
    // redirection-surface copy, so the overlay no longer degrades the game's frame pacing.
    Microsoft::WRL::ComPtr<IDCompositionDevice> m_dcompDevice;
    Microsoft::WRL::ComPtr<IDCompositionTarget> m_dcompTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual> m_dcompVisual;

    bool CreateDeviceD3D();
    void CleanupDeviceD3D();
    void CreateRenderTarget();
    void CleanupRenderTarget();
    
    void UpdateClickThroughState();
};
