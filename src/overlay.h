#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <mutex>
#include "detector.h"

enum VisionMode {
    AI_VISION = 0,
    COLOR_VISION = 1
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
    
    int fovSize = 120;              // 320 or 640
    int targetFps = 170;            // 1 to 400
    int maxDetections = 5;
    float confThreshold = 0.5f;
    bool autoAim = true;
    bool showBoundingBoxes = true;
    bool showVisuals = true;
    bool showMenu = true;           // ImGui config panel visibility (Insert toggles)
    int hotkeyKey = VK_XBUTTON1;    // Mouse 4 by default
    bool isAimingActive = false;

    // Color picker communication flags
    bool requestStillFrame = false;
    bool colorPickerActive = false;

    // Humanized Smoothing settings (copied from Blazestrike)
    bool aimbot_humanized = false;
    bool aimbot_relative = true;     // Relative mouse movement (Raw input) vs absolute
    float aimbot_sensitivity = 1.0f; // Scale factor for relative movement
    float aimbot_smooth = 2.0f;
    float aimbot_jitter = 0.0f;
    float aimbot_curve_strength = 0.0f;
    float aimbot_ease_in = 0.15f;
    float aimbot_ease_out = 0.35f;

    float captureTimeMs = 0.0f;
    float preprocessTimeMs = 0.0f;
    float inferenceTimeMs = 0.0f;
    float actualFps = 0.0f;
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
    int m_screenWidth = 0;
    int m_screenHeight = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> m_pd3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_pd3dDeviceContext;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_pSwapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_mainRenderTargetView;

    bool CreateDeviceD3D();
    void CleanupDeviceD3D();
    void CreateRenderTarget();
    void CleanupRenderTarget();
    
    void UpdateClickThroughState();
};
