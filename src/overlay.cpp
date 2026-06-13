#include "overlay.h"
#include "makcu.h"
#include "vigem_input.h"
#include <filesystem>
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <iostream>
#include <cstdio>
#include <cfloat>
#include <cmath>

#pragma comment(lib, "dcomp.lib")

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam))
        return true;

    switch (uMsg) {
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_KEYMENU) // Disable ALT application menu
                return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

Overlay::Overlay() {}

Overlay::~Overlay() {
    Shutdown();
}

bool Overlay::Init(HINSTANCE hInstance) {
    // 1. Get Screen Resolution
    m_screenWidth = GetSystemMetrics(SM_CXSCREEN);
    m_screenHeight = GetSystemMetrics(SM_CYSCREEN);

    // 2. Register Window Class
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_CLASSDC, WindowProc, 0L, 0L, hInstance, NULL, NULL, NULL, NULL, L"OverlayWindowClass", NULL };
    RegisterClassExW(&wc);

    // 3. Create Window with transparent click-through styles.
    // WS_EX_NOREDIRECTIONBITMAP: no GDI redirection surface is allocated — all visuals come
    // from the DirectComposition swap chain, so DWM skips the legacy layered-window copy.
    // WS_EX_LAYERED stays only because WS_EX_TRANSPARENT (mouse click-through) requires it;
    // with NOREDIRECTIONBITMAP it costs nothing (no SetLayeredWindowAttributes needed).
    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        L"OverlayWindowClass",
        L"Overlay Assist",
        WS_POPUP,
        0, 0, m_screenWidth, m_screenHeight,
        NULL, NULL, hInstance, NULL
    );

    if (!m_hwnd) {
        std::cerr << "Failed to create overlay window\n";
        return false;
    }

    // 4. Initialize D3D11 Device and SwapChain
    if (!CreateDeviceD3D()) {
        CleanupDeviceD3D();
        return false;
    }

    // Show window without focusing it
    ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(m_hwnd);

    // 5. Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Apply Premium Dark/Translucent Style
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowBorderSize = 0.0f;
    
    // Sleek HSL dark styling
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                   = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.09f, 0.09f, 0.12f, 0.85f); // Translucent Dark
    colors[ImGuiCol_Border]                 = ImVec4(0.18f, 0.18f, 0.24f, 0.50f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.14f, 0.14f, 0.18f, 0.70f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.20f, 0.20f, 0.28f, 0.90f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.25f, 0.25f, 0.35f, 1.00f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.09f, 0.09f, 0.12f, 0.90f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.12f, 0.12f, 0.18f, 1.00f);
    colors[ImGuiCol_CheckMark]              = ImVec4(0.38f, 0.45f, 0.90f, 1.00f);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.38f, 0.45f, 0.90f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.50f, 0.58f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]                 = ImVec4(0.20f, 0.22f, 0.35f, 0.75f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.28f, 0.30f, 0.50f, 0.90f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.38f, 0.45f, 0.90f, 1.00f);
    colors[ImGuiCol_Header]                 = ImVec4(0.20f, 0.20f, 0.30f, 0.70f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.28f, 0.30f, 0.50f, 0.80f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.38f, 0.45f, 0.90f, 1.00f);

    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_pd3dDevice.Get(), m_pd3dDeviceContext.Get());

    return true;
}

void Overlay::Shutdown() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    UnregisterClassW(L"OverlayWindowClass", GetModuleHandle(NULL));
}

void Overlay::ApplyStreamProof(bool enabled) {
    if (!m_hwnd) return;
    // WDA_EXCLUDEFROMCAPTURE removes the window from every capture path (OBS/Discord
    // window+display capture, PrintScreen, and our own DXGI desktop duplication — so the
    // HUD can never feed back into color/AI vision). Fails on pre-2004 Win10; ignore there.
    SetWindowDisplayAffinity(m_hwnd, enabled ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
}

bool Overlay::ProcessMessages() {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (msg.message == WM_QUIT)
            return false;
    }
    
    UpdateClickThroughState();
    return true;
}

void Overlay::UpdateClickThroughState() {
    ImGuiIO& io = ImGui::GetIO();
    LONG_PTR exStyle = GetWindowLongPtr(m_hwnd, GWL_EXSTYLE);
    
    // Dynamic click-through:
    // If the config menu is open, ImGui wants the mouse (e.g. user is dragging/clicking a
    // panel), or the color picker is active, we remove WS_EX_TRANSPARENT. Otherwise, we add
    // it back so clicks fall through to the background. The menu must force this (not just
    // WantCaptureMouse): in a game the cursor is pinned to screen center, never hovers a
    // panel, so WantCaptureMouse alone would never flip and the menu would be unclickable.
    if (m_menuOpen || io.WantCaptureMouse || m_stillFrameCaptured) {
        if (exStyle & WS_EX_TRANSPARENT) {
            SetWindowLongPtr(m_hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_TRANSPARENT);
        }
    } else {
        if (!(exStyle & WS_EX_TRANSPARENT)) {
            SetWindowLongPtr(m_hwnd, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT);
        }
    }

    // Focus stealing: WM_CHAR/WM_KEYDOWN (and a visible cursor in fullscreen games) only
    // come with being the *foreground* window, but the overlay is WS_EX_NOACTIVATE so it
    // normally never gets focus. We take focus in two cases:
    //  - ImGui wants text input (typing into a field must reach us), and
    //  - the config menu is open: a game in the foreground keeps the cursor captured,
    //    clipped and hidden, so the menu can't be used until we become foreground and
    //    release the clip. Closing the menu hands focus straight back to the game.
    bool wantFocus = io.WantTextInput || m_menuOpen;
    static bool s_hadFocus = false;
    if (wantFocus) {
        if (!s_hadFocus) {
            m_prevForeground = GetForegroundWindow();
            exStyle = GetWindowLongPtr(m_hwnd, GWL_EXSTYLE);
            SetWindowLongPtr(m_hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_NOACTIVATE);
            SetForegroundWindow(m_hwnd);
            SetFocus(m_hwnd);
            ClipCursor(nullptr); // Undo any cursor confinement the game had active
            s_hadFocus = true;
        }
    } else if (s_hadFocus) {
        exStyle = GetWindowLongPtr(m_hwnd, GWL_EXSTYLE);
        SetWindowLongPtr(m_hwnd, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);
        if (m_prevForeground && IsWindow(m_prevForeground) && m_prevForeground != m_hwnd) {
            SetForegroundWindow(m_prevForeground); // Give the game its focus back
        }
        m_prevForeground = nullptr;
        s_hadFocus = false;
    }
}

void Overlay::BeginFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // Manually update mouse position because WS_EX_TRANSPARENT / WS_EX_NOACTIVATE
    // prevents the Win32 backend from receiving mouse messages or updating mouse position.
    ImGuiIO& io = ImGui::GetIO();
    POINT cursorPos;
    if (GetCursorPos(&cursorPos)) {
        ScreenToClient(m_hwnd, &cursorPos);
        io.MousePos = ImVec2(static_cast<float>(cursorPos.x), static_cast<float>(cursorPos.y));
    }

    ImGui::NewFrame();
}

void Overlay::EndFrame() {
    ImGui::Render();
    
    // Clear swap chain render target with fully transparent black
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_pd3dDeviceContext->OMSetRenderTargets(1, m_mainRenderTargetView.GetAddressOf(), NULL);
    m_pd3dDeviceContext->ClearRenderTargetView(m_mainRenderTargetView.Get(), clearColor);

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void Overlay::Present() {
    // Immediate present (no VSync). VSync here pushes a vblank-wait into the shared GPU
    // command queue, which stalls the processing thread's CUDA sync behind it. The main
    // loop caps the overlay frame rate instead (see ProcessingThread / GUI loop).
    m_pSwapChain->Present(0, 0);
}

static void DrawGlowCircle(ImDrawList* dl, ImVec2 center, float radius, ImVec4 color, float intensity, int layers = 5) {
    for (int i = layers; i >= 1; i--) {
        float t = (float)i / layers;
        float r = radius + t * radius * 1.6f;
        float a = intensity * (1.0f - t) * (1.0f - t);
        dl->AddCircleFilled(center, r, ImGui::ColorConvertFloat4ToU32({color.x, color.y, color.z, a}), 28);
    }
}

static void DrawGlowRect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImVec4 color, float rounding, float intensity, int layers = 4, float spread = 9.0f) {
    for (int i = layers; i >= 1; i--) {
        float t = (float)i / layers;
        float e = spread * t;
        float a = intensity * (1.0f - t) * (1.0f - t);
        dl->AddRectFilled({mn.x - e, mn.y - e}, {mx.x + e, mx.y + e},
                          ImGui::ColorConvertFloat4ToU32({color.x, color.y, color.z, a}), rounding + e);
    }
}

void Overlay::DrawConfigPanel(AppConfig& config) {
    ImGui::SetNextWindowSize(ImVec2(380, 560), ImGuiCond_FirstUseEver);
    
    // Renders the modern control panel with a custom title bar and scrolling
    ImGui::Begin("Assistive Pointing Hub", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImVec4 accent = ImVec4(0.38f, 0.45f, 0.90f, 1.0f); // Sleek neon violet-blue
    ImU32 accent_u32 = ImGui::ColorConvertFloat4ToU32(accent);
    float header_h = 44.0f;

    // Glowing diamond logo glyph
    ImVec2 logo_c = { p.x + 17, p.y + header_h * 0.5f };
    DrawGlowCircle(dl, logo_c, 9.0f, accent, 0.45f);
    ImVec2 dpts[4] = { {logo_c.x, logo_c.y - 9}, {logo_c.x + 9, logo_c.y},
                       {logo_c.x, logo_c.y + 9}, {logo_c.x - 9, logo_c.y} };
    dl->AddConvexPolyFilled(dpts, 4, accent_u32);
    ImVec2 ipts[4] = { {logo_c.x, logo_c.y - 4}, {logo_c.x + 4, logo_c.y},
                       {logo_c.x, logo_c.y + 4}, {logo_c.x - 4, logo_c.y} };
    dl->AddConvexPolyFilled(ipts, 4, ImGui::ColorConvertFloat4ToU32({0.09f, 0.09f, 0.12f, 0.9f}));

    ImGui::SetCursorScreenPos({ p.x + 38, p.y + 6.0f });
    ImGui::TextColored(accent, "BlazeAI");
    ImGui::SetCursorScreenPos({ p.x + 38, p.y + 22.0f });
    ImGui::TextColored({0.45f, 0.50f, 0.50f, 1.0f}, "Assistive Pointing Suite");

    // Status pill, glowing, right-aligned
    bool active = config.autoAim;
    ImVec4 status_col = active ? ImVec4{0.35f, 0.95f, 0.55f, 1.0f} : ImVec4{0.95f, 0.40f, 0.40f, 1.0f};
    ImU32 status_u32 = ImGui::ColorConvertFloat4ToU32(status_col);
    const char* status_txt = active ? "ACTIVE" : "DISABLED";
    ImVec2 ts = ImGui::CalcTextSize(status_txt);
    float pill_w = ts.x + 32.0f, pill_h = 24.0f;
    ImVec2 pill_min = { p.x + w - pill_w, p.y + (header_h - pill_h) * 0.5f };
    ImVec2 pill_max = { pill_min.x + pill_w, pill_min.y + pill_h };
    DrawGlowRect(dl, pill_min, pill_max, status_col, pill_h * 0.5f, 0.30f, 3, 7.0f);
    dl->AddRectFilled(pill_min, pill_max,
                      ImGui::ColorConvertFloat4ToU32({status_col.x, status_col.y, status_col.z, 0.14f}), pill_h * 0.5f);
    dl->AddRect(pill_min, pill_max,
                ImGui::ColorConvertFloat4ToU32({status_col.x, status_col.y, status_col.z, 0.55f}), pill_h * 0.5f, 0, 1.2f);
    ImVec2 dot_c = { pill_min.x + 15, (pill_min.y + pill_max.y) * 0.5f };
    dl->AddCircleFilled(dot_c, 3.0f, status_u32, 12);
    dl->AddText({ dot_c.x + 9, (pill_min.y + pill_max.y) * 0.5f - ts.y * 0.5f }, status_u32, status_txt);

    // Gradient separator
    ImGui::SetCursorScreenPos({ p.x, p.y + header_h + 6.0f });
    ImVec2 sp = ImGui::GetCursorScreenPos();
    ImU32 fade = ImGui::ColorConvertFloat4ToU32({accent.x, accent.y, accent.z, 0.0f});
    ImU32 solid = ImGui::ColorConvertFloat4ToU32({accent.x, accent.y, accent.z, 0.5f});
    dl->AddRectFilledMultiColor(sp, {sp.x + w * 0.5f, sp.y + 1.4f}, fade, solid, solid, fade);
    dl->AddRectFilledMultiColor({sp.x + w * 0.5f, sp.y}, {sp.x + w, sp.y + 1.4f}, solid, fade, fade, solid);

    ImGui::SetCursorScreenPos({ p.x, sp.y + 10.0f });

    // Begin a scrolling child region for settings so the window remains cleanly bounded
    ImGui::BeginChild("##settings_scroll", ImGui::GetContentRegionAvail(), false, ImGuiWindowFlags_NoScrollbar);

    // Master Mode Toggle
    ImGui::TextColored(accent, "Master Mode:");
    ImGui::SameLine();
    const char* modes[] = { "AI Vision", "Color Vision" };
    int currentMode = config.visionMode;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Master Mode:").x - 15.0f);
    if (ImGui::Combo("##master_mode_combo", &currentMode, modes, IM_ARRAYSIZE(modes))) {
        config.visionMode = currentMode;
    }
    ImGui::Dummy(ImVec2(0.0f, 5.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 5.0f));

    if (config.visionMode == COLOR_VISION) {
        if (ImGui::CollapsingHeader("Color Vision Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent();
            
            // Visual Color Picker
            float targetColor[3] = { config.colorTargetR / 255.0f, config.colorTargetG / 255.0f, config.colorTargetB / 255.0f };
            if (ImGui::ColorEdit3("Target Color", targetColor)) {
                config.colorTargetR = static_cast<int>(targetColor[0] * 255.0f + 0.5f);
                config.colorTargetG = static_cast<int>(targetColor[1] * 255.0f + 0.5f);
                config.colorTargetB = static_cast<int>(targetColor[2] * 255.0f + 0.5f);
            }

            ImGui::Dummy(ImVec2(0.0f, 3.0f));
            if (ImGui::Button("Pick Color from Screen", ImVec2(-FLT_MIN, 28.0f))) {
                config.requestStillFrame = true;
                config.colorPickerActive = true;
                m_stillFrameCaptured = false;
            }
            ImGui::Dummy(ImVec2(0.0f, 3.0f));
            
            ImGui::SliderInt("Color Tolerance", &config.colorTolerance, 0, 100);
            ImGui::SliderInt("Min Target Area", &config.colorMinArea, 5, 500);
            ImGui::Unindent();
            ImGui::Dummy(ImVec2(0.0f, 5.0f));
        }
    } else {
        // 1. Model Configuration
        if (ImGui::CollapsingHeader("ONNX Model Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent();
            // Model Dropdown Selection
            std::string currentModel = "Select Model...";
            if (!config.selectedModelPath.empty()) {
                std::filesystem::path modelPath(config.selectedModelPath);
                currentModel = modelPath.filename().string();
            }

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 85.0f);
            if (ImGui::BeginCombo("Active Model", currentModel.c_str())) {
                for (const auto& path : config.availableModels) {
                    std::filesystem::path fsPath(path);
                    std::string modelName = fsPath.filename().string();
                    bool isSelected = (config.selectedModelPath == path);
                    if (ImGui::Selectable(modelName.c_str(), isSelected)) {
                        config.selectedModelPath = path;
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::SameLine();
            if (ImGui::Button("Refresh", ImVec2(75.0f, 0.0f))) {
                config.availableModels = Detector::ScanModelsDir();
                if (!config.availableModels.empty()) {
                    bool stillExists = false;
                    for (const auto& path : config.availableModels) {
                        if (path == config.selectedModelPath) {
                            stillExists = true;
                            break;
                        }
                    }
                    if (!stillExists) {
                        config.selectedModelPath = config.availableModels[0];
                    }
                } else {
                    config.selectedModelPath = L"";
                }
            }

            // Backend acceleration. Toggling either of these triggers a model reload on the
            // processing thread; the first TensorRT build can take several minutes (cached after).
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::Checkbox("Use TensorRT (faster, first load is slow)", &config.useTensorRT);
            if (config.useTensorRT) {
                ImGui::Indent();
                ImGui::Checkbox("FP16 (recommended)", &config.trtFp16);
                ImGui::TextDisabled("First load builds a GPU engine (minutes), then cached.");
                ImGui::Unindent();
            }

            // While a model loads, the processing thread is blocked in LoadModel. ORT exposes
            // no build-progress callback, so this is an indeterminate (animated) bar plus an
            // elapsed timer — enough feedback for the multi-minute TensorRT engine build.
            static double buildStart = 0.0;
            static bool wasBuilding = false;
            if (config.engineBuilding) {
                double now = ImGui::GetTime();
                if (!wasBuilding) { buildStart = now; wasBuilding = true; }
                int elapsed = (int)(now - buildStart);
                char timeLabel[32];
                snprintf(timeLabel, sizeof(timeLabel), "%02d:%02d elapsed", elapsed / 60, elapsed % 60);
                ImGui::Dummy(ImVec2(0.0f, 2.0f));
                ImGui::ProgressBar(-1.0f, ImVec2(-1.0f, 0.0f), timeLabel); // <0 = indeterminate
                ImGui::TextWrapped("%s", config.useTensorRT
                    ? "Building TensorRT engine for this model + GPU. First run only; do not close."
                    : "Loading model...");
            } else {
                wasBuilding = false;
            }
            ImGui::Unindent();
            ImGui::Dummy(ImVec2(0.0f, 5.0f));
        }
    }

    // 2. FOV & Capture Configuration
    if (ImGui::CollapsingHeader("Capture & FOV Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        ImGui::SliderInt("FOV Size (Pixels)", &config.fovSize, 100, 800);
        ImGui::Checkbox("Show FOV Indicator", &config.showFov);
        if (config.showFov) {
            const char* fovShapes[] = { "Square", "Circle" };
            ImGui::Combo("FOV Indicator Shape", &config.fovShape, fovShapes, IM_ARRAYSIZE(fovShapes));
        }
        ImGui::SliderInt("Capture FPS Limit", &config.targetFps, 1, 400);
        if (ImGui::Checkbox("Stream-Proof Overlay", &config.streamProof)) {
            ApplyStreamProof(config.streamProof);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Hides the overlay from recordings, screenshots and screen shares.\nRequires Windows 10 2004 or newer.");
        }
        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0.0f, 5.0f));
    }

    if (ImGui::CollapsingHeader("Aim Debug Visualizer")) {
        ImGui::Indent();
        ImGui::Checkbox("Show Aim Visualizer", &config.showAimVisualizer);
        if (config.showAimVisualizer) {
            ImGui::Indent();
            ImGui::Checkbox("Show Smoothing Trail", &config.showSmoothingTrail);
            ImGui::Checkbox("Show Target Vector", &config.showTargetVector);
            ImGui::Checkbox("Show Raw Aim Point", &config.showRawAimPoint);
            ImGui::Checkbox("Show Smoothed Aim Point", &config.showSmoothedAimPoint);
            ImGui::TextDisabled("Hold the aim hotkey to display live telemetry.");
            ImGui::Unindent();
        }
        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0.0f, 5.0f));
    }

    // 3. Targeting Options
    if (ImGui::CollapsingHeader("Targeting & Filters", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        ImGui::Checkbox("Enable Assistive Auto-Aim", &config.autoAim);
        ImGui::Checkbox("Show Bounding Boxes", &config.showBoundingBoxes);
        ImGui::SliderFloat("Confidence Threshold", &config.confThreshold, 0.1f, 1.0f, "%.2f");
        ImGui::SliderInt("Max Detections", &config.maxDetections, 1, 20);
        ImGui::SliderFloat("Aim Height", &config.aimHeightRatio, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Where on the target box to aim vertically.\n0.50 = center mass, ~0.15 = head (full-body boxes), 0.30 = upper chest.\nNormalized to box height, so it works at any target distance.\nCtrl+Click the slider to type an exact value.");
        }

        // Aim height presets. The active one (within slider display precision) is highlighted.
        static const struct { const char* name; float value; } aimPresets[] = {
            { "Head", 0.12f }, { "Neck", 0.20f }, { "Chest", 0.30f }, { "Center", 0.50f },
        };
        for (int i = 0; i < IM_ARRAYSIZE(aimPresets); ++i) {
            if (i > 0) ImGui::SameLine();
            bool active = std::fabs(config.aimHeightRatio - aimPresets[i].value) < 0.005f;
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.38f, 0.45f, 1.00f, 0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.52f, 1.00f, 0.95f));
            }
            if (ImGui::SmallButton(aimPresets[i].name)) {
                config.aimHeightRatio = aimPresets[i].value;
            }
            if (active) ImGui::PopStyleColor(2);
        }

        // Dynamic Hotkey Selection
        struct KeyBinding {
            const char* name;
            int vkCode;
        };
        static const KeyBinding bindings[] = {
            {"Left Click", VK_LBUTTON},
            {"Right Click", VK_RBUTTON},
            {"Middle Click", VK_MBUTTON},
            {"Alt", VK_MENU},
            {"Shift", VK_SHIFT},
            {"Caps Lock", VK_CAPITAL},
            {"Mouse 4 (Back)", VK_XBUTTON1},
            {"Mouse 5 (Forward)", VK_XBUTTON2}
        };

        std::string currentHotkey = "Select Key...";
        for (const auto& binding : bindings) {
            if (config.hotkeyKey == binding.vkCode) {
                currentHotkey = binding.name;
                break;
            }
        }

        if (ImGui::BeginCombo("Aim Assist Hotkey", currentHotkey.c_str())) {
            for (const auto& binding : bindings) {
                bool isSelected = (config.hotkeyKey == binding.vkCode);
                if (ImGui::Selectable(binding.name, isSelected)) {
                    config.hotkeyKey = binding.vkCode;
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0.0f, 5.0f));

        ImGui::Separator();
        ImGui::Checkbox("Enable Trigger Bot", &config.triggerBotEnabled);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("While the aim hotkey is held and the locked target's aim point is\nwithin the radius below, automatically fires a left click.\nUses the same mouse injection method selected under Mouse & Smoothing Tuning.");
        }
        if (config.triggerBotEnabled) {
            ImGui::Indent();
            ImGui::SliderFloat("Trigger Radius (px)", &config.triggerBotRadius, 1.0f, 50.0f, "%.1f");
            ImGui::SliderInt("Click Hold (ms)", &config.triggerBotHoldMs, 10, 200);
            ImGui::SliderInt("Click Cooldown (ms)", &config.triggerBotCooldownMs, 0, 1000);
            ImGui::Unindent();
        }
        ImGui::Dummy(ImVec2(0.0f, 5.0f));
    }

    if (ImGui::CollapsingHeader("Target Tracking", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        ImGui::Checkbox("Enable Target Tracking", &config.trackerEnabled);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Gives detections stable IDs across frames so the aim sticks to one\ntarget instead of snapping to whichever is nearest each frame.");
        }
        if (config.trackerEnabled) {
            ImGui::Indent();
            ImGui::SliderFloat("Match Strictness (IoU)", &config.trackerIou, 0.05f, 0.7f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How much a box must overlap its track to count as the same target.\nLower = tracks survive fast movement; higher = fewer identity mix-ups.");
            }
            ImGui::SliderInt("Lost-Target Patience (frames)", &config.trackerMaxMissed, 0, 30);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Frames a vanished target keeps coasting before its track is dropped.\nBridges single-frame detection dropouts.");
            }
            ImGui::SliderFloat("Switch Stickiness", &config.trackerSwitchRatio, 0.1f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("A rival target must be closer than this fraction of the locked target's\ndistance to steal the lock. 1.0 = always take the nearest (no stickiness).");
            }
            ImGui::Unindent();
        }
        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0.0f, 5.0f));
    }

    // 4. Mouse Input & Smoothing Tuning
    if (ImGui::CollapsingHeader("Mouse & Smoothing Tuning", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        ImGui::Checkbox("Relative Mouse Input (for 3D/FPS)", &config.aimbot_relative);
        if (config.aimbot_relative) {
            ImGui::SliderFloat("Relative Sensitivity Scale", &config.aimbot_sensitivity, 0.1f, 5.0f, "%.2f");

            const char* methodNames[] = { "SendInput", "NtUserInjectMouseInput (win32u)", "MAKCU (Serial)", "Virtual Controller (ViGEm)" };
            int method = config.mouseInputMethod;
            if (ImGui::Combo("Mouse Injection Method", &method, methodNames, IM_ARRAYSIZE(methodNames))) {
                config.mouseInputMethod = method;
            }

            // MAKCU device configuration: only shown when the serial backend is selected.
            if (config.mouseInputMethod == MOUSE_MAKCU) {
                ImGui::Separator();
                ImGui::TextUnformatted("MAKCU Connection");
                ImGui::Indent();

                // COM-port list is cached and only rescanned on Refresh (QueryDosDevice
                // sweeps 256 names, too heavy to run every frame).
                static std::vector<std::string> ports = MakcuInput::EnumPorts();
                static int portIdx = -1;
                if (portIdx < 0) {
                    // Default the selection to whatever is currently connected, if anything.
                    for (size_t i = 0; i < ports.size(); ++i) {
                        if (ports[i] == g_makcu.CurrentPort()) { portIdx = static_cast<int>(i); break; }
                    }
                }

                std::string preview = (portIdx >= 0 && portIdx < (int)ports.size()) ? ports[portIdx] : "Select COM port";
                if (ImGui::BeginCombo("COM Port", preview.c_str())) {
                    for (int i = 0; i < (int)ports.size(); ++i) {
                        bool selected = (i == portIdx);
                        if (ImGui::Selectable(ports[i].c_str(), selected)) {
                            portIdx = i;
                            // Auto-connect on selection — no explicit connect button.
                            g_makcu.Connect(ports[i]);
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                ImGui::SameLine();
                if (ImGui::Button("Refresh")) {
                    ports = MakcuInput::EnumPorts();
                    portIdx = -1; // re-resolve against current connection next frame
                }

                if (g_makcu.IsConnected()) {
                    ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f),
                                       "Connected on %s", g_makcu.CurrentPort().c_str());
                    if (ImGui::Button("Test Movement")) {
                        g_makcu.MoveRelative(50, 0); // nudge 50px right as a visible probe
                    }
                } else {
                    ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "Not connected");
                }

                ImGui::Unindent();
            }

            // ViGEm virtual-controller backend configuration.
            if (config.mouseInputMethod == MOUSE_VIGEM) {
                ImGui::Separator();
                ImGui::TextUnformatted("Virtual Controller (ViGEmBus)");
                ImGui::Indent();

                ImGui::SliderFloat("Stick Scale (units/px)", &config.vigemStickScale, 10.0f, 1000.0f, "%.0f");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("How hard the virtual right stick is pushed per pixel of aim error.\nTune in-game; too high overshoots, too low feels sluggish.");
                }

                ImGui::Checkbox("Always Aim (no trigger needed)", &config.vigemAlwaysAim);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Aim assist activates as soon as a target is detected,\nwithout needing to hold the left trigger or hotkey.");
                }

                if (g_vigem.Available()) {
                    ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "Virtual X360 pad active");
                    if (g_vigem.HasRealPad()) {
                        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "Real pad found - passing through inputs");
                    } else {
                        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f),
                                           "No real pad detected - virtual pad will only output aim assist.\nConnect your controller before enabling this mode.");
                    }
                    if (ImGui::Button("Test Stick Movement")) {
                        g_vigem.MoveRelative(50, 0, config.vigemStickScale);
                    }
                } else {
                    ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f),
                                       "ViGEmBus not found - install the driver from\nhttps://github.com/ViGEm/ViGEmBus/releases");
                }

                ImGui::TextWrapped("Hide your physical controller's HID with HidHide so the game "
                                    "binds to this virtual pad instead.");

                ImGui::Unindent();
            }
        }

        ImGui::Separator();
        
        ImGui::Checkbox("Enable Humanized Smoothing", &config.aimbot_humanized);
        ImGui::SliderFloat("Smoothness", &config.aimbot_smooth, 1.0f, 50.0f, "%.1f");
        
        if (config.aimbot_humanized) {
            ImGui::SliderFloat("Curve Strength", &config.aimbot_curve_strength, 0.0f, 5.0f, "%.2f");
            ImGui::SliderFloat("Jitter", &config.aimbot_jitter, 0.0f, 5.0f, "%.2f");
            ImGui::SliderFloat("Ease In", &config.aimbot_ease_in, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Ease Out", &config.aimbot_ease_out, 0.0f, 1.0f, "%.2f");
        }
        ImGui::Unindent();
        ImGui::Dummy(ImVec2(0.0f, 5.0f));
    }

    // 5. Diagnostics
    if (ImGui::CollapsingHeader("Telemetry & Diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        ImGui::Text("Overlay Framework FPS: %.1f", ImGui::GetIO().Framerate);
        // Telemetry values are raw per-frame samples and naturally jitter. The text overlays
        // use an EMA so the number is legible; the plots show the raw history so the jitter is
        // visible. All of this is display-only main-thread state — never touches g_config.
        auto smooth = [](float& acc, float sample) -> float {
            constexpr float alpha = 0.1f; // low-pass; lower = steadier, slower to react
            acc = (acc <= 0.0f) ? sample : acc + alpha * (sample - acc);
            return acc;
        };
        static float fpsAvg = 0.0f, capAvg = 0.0f, preAvg = 0.0f, infAvg = 0.0f;
        smooth(fpsAvg, config.actualFps);
        smooth(capAvg, config.captureTimeMs);
        smooth(preAvg, config.preprocessTimeMs);
        smooth(infAvg, config.inferenceTimeMs);

        // Rolling raw-sample history feeding the sparkline plots (push-on-render ring buffers).
        constexpr int kHist = 150;
        struct Ring {
            float v[kHist] = {};
            int off = 0;
            void push(float x) { v[off] = x; off = (off + 1) % kHist; }
        };
        static Ring fpsHist, capHist, preHist, infHist;
        fpsHist.push(config.actualFps);
        capHist.push(config.captureTimeMs);
        preHist.push(config.preprocessTimeMs);
        infHist.push(config.inferenceTimeMs);

        const char* backend = config.useTensorRT
            ? (config.trtFp16 ? "TensorRT (FP16)" : "TensorRT (FP32)") : "CUDA";
        ImGui::Text("Backend: %s", backend);

        // Full-width sparklines, y-axis anchored at 0 with auto max; current EMA value overlaid.
        char ovl[48];
        const ImVec2 plotSize(-1.0f, 45.0f);
        snprintf(ovl, sizeof(ovl), "FPS: %.1f", fpsAvg);
        ImGui::PlotLines("##fps", fpsHist.v, kHist, fpsHist.off, ovl, 0.0f, FLT_MAX, plotSize);
        snprintf(ovl, sizeof(ovl), "Inference: %.2f ms", infAvg);
        ImGui::PlotLines("##inf", infHist.v, kHist, infHist.off, ovl, 0.0f, FLT_MAX, plotSize);
        snprintf(ovl, sizeof(ovl), "Preprocess (CUDA): %.2f ms", preAvg);
        ImGui::PlotLines("##pre", preHist.v, kHist, preHist.off, ovl, 0.0f, FLT_MAX, plotSize);
        // Preprocess breakdown: shows whether the cost is driver map/unmap (sync stall) or kernel.
        ImGui::Indent();
        ImGui::TextDisabled("map %.2f  |  kernel(cpu) %.2f  |  unmap %.2f ms",
                            config.preprocessMapMs, config.preprocessKernelMs, config.preprocessUnmapMs);
        ImGui::TextDisabled("kernel(gpu) %.3f ms  <- true compute; cpu-gpu gap = queue/contention wait",
                            config.preprocessKernelGpuMs);
        ImGui::Unindent();
        snprintf(ovl, sizeof(ovl), "Capture: %.2f ms", capAvg);
        ImGui::PlotLines("##cap", capHist.v, kHist, capHist.off, ovl, 0.0f, FLT_MAX, plotSize);
        ImGui::Unindent();
    }

    ImGui::EndChild();
    ImGui::End();
}

void Overlay::DrawDetections(const std::vector<Detection>& detections, const AppConfig& config) {
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();

    // 1. Draw central FOV indicator. Shape affects visualization only; capture remains square.
    float centerX = m_screenWidth / 2.0f;
    float centerY = m_screenHeight / 2.0f;
    float halfFov = config.fovSize / 2.0f;

    ImVec4 fovColor = config.isAimingActive ? ImVec4(0.9f, 0.2f, 0.2f, 0.6f) : ImVec4(0.5f, 0.5f, 0.5f, 0.4f);
    if (config.showFov) {
        ImU32 fovU32 = ImGui::ColorConvertFloat4ToU32(fovColor);
        if (config.fovShape == 1) {
            drawList->AddCircle(ImVec2(centerX, centerY), halfFov, fovU32, 64, 1.5f);
        } else {
            drawList->AddRect(
                ImVec2(centerX - halfFov, centerY - halfFov),
                ImVec2(centerX + halfFov, centerY + halfFov),
                fovU32,
                3.0f,
                0,
                1.5f
            );
        }
    }

    // Aim Debug Visualizer. History is display-only state kept on the UI thread.
    static std::vector<ImVec2> smoothingTrail;
    if (!config.showAimVisualizer || !config.aimDebugTargetActive) {
        smoothingTrail.clear();
    } else {
        const ImVec2 crosshair(centerX, centerY);
        const ImVec2 rawPoint(config.aimDebugTargetX, config.aimDebugTargetY);
        const ImVec2 smoothedPoint(centerX + config.aimDebugMoveX, centerY + config.aimDebugMoveY);

        float severity = (config.aimDebugSmooth - 1.0f) / 14.0f;
        severity = std::max(0.0f, std::min(1.0f, severity));
        ImVec4 smoothColor = severity < 0.5f
            ? ImVec4(0.25f + severity * 1.5f, 0.95f, 0.25f, 1.0f)
            : ImVec4(1.0f, 0.95f - (severity - 0.5f) * 1.5f, 0.20f, 1.0f);
        ImU32 smoothU32 = ImGui::ColorConvertFloat4ToU32(smoothColor);

        if (config.showTargetVector) {
            drawList->AddLine(crosshair, rawPoint, IM_COL32(100, 190, 255, 190), 1.5f);
            float length = std::max(0.001f, config.aimDebugDistance);
            ImVec2 direction((rawPoint.x - crosshair.x) / length, (rawPoint.y - crosshair.y) / length);
            ImVec2 perpendicular(-direction.y, direction.x);
            ImVec2 arrowBase(rawPoint.x - direction.x * 11.0f, rawPoint.y - direction.y * 11.0f);
            drawList->AddTriangleFilled(
                rawPoint,
                ImVec2(arrowBase.x + perpendicular.x * 5.0f, arrowBase.y + perpendicular.y * 5.0f),
                ImVec2(arrowBase.x - perpendicular.x * 5.0f, arrowBase.y - perpendicular.y * 5.0f),
                IM_COL32(100, 190, 255, 220));

            char distanceText[48];
            snprintf(distanceText, sizeof(distanceText), "%.1f px", config.aimDebugDistance);
            ImVec2 midpoint((crosshair.x + rawPoint.x) * 0.5f + 6.0f,
                            (crosshair.y + rawPoint.y) * 0.5f + 6.0f);
            drawList->AddText(midpoint, IM_COL32(180, 220, 255, 230), distanceText);
        }

        if (config.showSmoothingTrail) {
            if (smoothingTrail.empty()) smoothingTrail.push_back(crosshair);
            ImVec2 last = smoothingTrail.back();
            float change = std::sqrt((last.x - smoothedPoint.x) * (last.x - smoothedPoint.x) +
                                     (last.y - smoothedPoint.y) * (last.y - smoothedPoint.y));
            if (change >= 0.5f) smoothingTrail.push_back(smoothedPoint);
            constexpr size_t kMaxTrailPoints = 30;
            if (smoothingTrail.size() > kMaxTrailPoints) smoothingTrail.erase(smoothingTrail.begin());

            for (size_t i = 1; i < smoothingTrail.size(); ++i) {
                float alpha = static_cast<float>(i) / static_cast<float>(smoothingTrail.size());
                ImVec4 faded = smoothColor;
                faded.w = alpha * 0.75f;
                drawList->AddLine(smoothingTrail[i - 1], smoothingTrail[i],
                                  ImGui::ColorConvertFloat4ToU32(faded), 2.0f);
            }
            drawList->AddLine(crosshair, smoothedPoint,
                              ImGui::ColorConvertFloat4ToU32({smoothColor.x, smoothColor.y, smoothColor.z, 0.35f}),
                              1.0f);
        } else {
            smoothingTrail.clear();
        }

        if (config.showRawAimPoint) {
            drawList->AddCircle(rawPoint, 6.0f, IM_COL32(90, 190, 255, 255), 16, 2.0f);
            drawList->AddCircleFilled(rawPoint, 2.0f, IM_COL32(220, 245, 255, 255), 12);
        }
        if (config.showSmoothedAimPoint) {
            DrawGlowCircle(drawList, smoothedPoint, 5.0f, smoothColor, 0.55f, 4);
            drawList->AddCircleFilled(smoothedPoint, 4.0f, smoothU32, 16);
        }

        char stats[256];
        snprintf(stats, sizeof(stats),
                 "AIM DEBUG\nSmooth: %.2f\nDelta: %.1f, %.1f\nDistance: %.1f px\nMove: %.1f, %.1f",
                 config.aimDebugSmooth, config.aimDebugDeltaX, config.aimDebugDeltaY,
                 config.aimDebugDistance, config.aimDebugMoveX, config.aimDebugMoveY);
        ImVec2 statsPos(centerX + halfFov + 16.0f, centerY - halfFov);
        ImVec2 statsSize = ImGui::CalcTextSize(stats);
        drawList->AddRectFilled(ImVec2(statsPos.x - 8.0f, statsPos.y - 7.0f),
                                ImVec2(statsPos.x + statsSize.x + 8.0f, statsPos.y + statsSize.y + 7.0f),
                                IM_COL32(15, 16, 22, 205), 5.0f);
        drawList->AddRect(ImVec2(statsPos.x - 8.0f, statsPos.y - 7.0f),
                          ImVec2(statsPos.x + statsSize.x + 8.0f, statsPos.y + statsSize.y + 7.0f),
                          smoothU32, 5.0f, 0, 1.0f);
        drawList->AddText(statsPos, IM_COL32(235, 238, 245, 255), stats);
    }

    // 2. Draw detections bounding boxes and labels if enabled
    if (!config.showBoundingBoxes) return;

    int count = 0;
    for (const auto& det : detections) {
        if (count >= config.maxDetections) break;

        ImVec2 pMin(static_cast<float>(det.box.x), static_cast<float>(det.box.y));
        ImVec2 pMax(static_cast<float>(det.box.x + det.box.width), static_cast<float>(det.box.y + det.box.height));

        // Bounding box (Premium Neon violet-blue theme)
        ImVec4 boxColor = ImVec4(0.38f, 0.45f, 1.00f, 0.9f);
        drawList->AddRect(pMin, pMax, ImGui::ColorConvertFloat4ToU32(boxColor), 4.0f, 0, 2.0f);

        // Render confidence text and label
        char labelText[128];
        if (det.trackId != -1) {
            sprintf_s(labelText, "#%d %s %.2f", det.trackId, det.label.c_str(), det.confidence);
        } else {
            sprintf_s(labelText, "%s %.2f", det.label.c_str(), det.confidence);
        }
        
        ImVec2 textSize = ImGui::CalcTextSize(labelText);
        ImVec2 labelRectMin(pMin.x, pMin.y - textSize.y - 4.0f);
        ImVec2 labelRectMax(pMin.x + textSize.x + 8.0f, pMin.y);

        // Fill background of label text
        drawList->AddRectFilled(labelRectMin, labelRectMax, ImGui::ColorConvertFloat4ToU32(boxColor), 3.0f, ImDrawFlags_RoundCornersTop);
        // Draw Text inside label box
        drawList->AddText(ImVec2(pMin.x + 4.0f, pMin.y - textSize.y - 2.0f), IM_COL32(255, 255, 255, 255), labelText);

        // Highlight the aim point (horizontal center, vertical at the configured ratio)
        ImVec2 center(pMin.x + det.box.width / 2.0f, pMin.y + det.box.height * config.aimHeightRatio);
        drawList->AddCircleFilled(center, 3.0f, IM_COL32(255, 60, 60, 255));

        count++;
    }
}

// Direct3D11 helper functions.
// Flip-model swap chain hosted on a DirectComposition visual: DWM composites the buffer
// directly with premultiplied alpha (often on a hardware overlay plane) instead of the
// legacy blt-model copy through a layered-window redirection surface. ImGui renders onto a
// (0,0,0,0)-cleared target with standard SrcAlpha blending, which yields premultiplied
// values for the compositor.
bool Overlay::CreateDeviceD3D() {
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // required for DComp interop
    // createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG; // Uncomment for active debugging

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT hr = D3D11CreateDevice(
        NULL,
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        createDeviceFlags,
        featureLevelArray,
        2,
        D3D11_SDK_VERSION,
        &m_pd3dDevice,
        &featureLevel,
        &m_pd3dDeviceContext
    );

    if (FAILED(hr)) {
        std::cerr << "D3D11CreateDevice failed. HR = " << std::hex << hr << "\n";
        return false;
    }

    // Composition swap chains have no OutputWindow and must use a flip swap effect.
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = m_screenWidth;
    sd.Height = m_screenHeight;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory;
    hr = m_pd3dDevice.As(&dxgiDevice);
    if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    if (SUCCEEDED(hr)) hr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
    if (SUCCEEDED(hr)) hr = dxgiFactory->CreateSwapChainForComposition(m_pd3dDevice.Get(), &sd, NULL, &m_pSwapChain);
    if (FAILED(hr)) {
        std::cerr << "CreateSwapChainForComposition failed. HR = " << std::hex << hr << "\n";
        return false;
    }

    // Wire the swap chain to the window: device -> target(hwnd) -> visual(swap chain).
    hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&m_dcompDevice));
    if (SUCCEEDED(hr)) hr = m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, &m_dcompTarget);
    if (SUCCEEDED(hr)) hr = m_dcompDevice->CreateVisual(&m_dcompVisual);
    if (SUCCEEDED(hr)) hr = m_dcompVisual->SetContent(m_pSwapChain.Get());
    if (SUCCEEDED(hr)) hr = m_dcompTarget->SetRoot(m_dcompVisual.Get());
    if (SUCCEEDED(hr)) hr = m_dcompDevice->Commit();
    if (FAILED(hr)) {
        std::cerr << "DirectComposition setup failed. HR = " << std::hex << hr << "\n";
        return false;
    }

    CreateRenderTarget();
    return true;
}

void Overlay::CleanupDeviceD3D() {
    CleanupRenderTarget();
    // Release the composition chain before the swap chain it references.
    m_dcompVisual.Reset();
    m_dcompTarget.Reset();
    m_dcompDevice.Reset();
    m_pSwapChain.Reset();
    m_pd3dDeviceContext.Reset();
    m_pd3dDevice.Reset();
}

void Overlay::CreateRenderTarget() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> pBackBuffer;
    m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    m_pd3dDevice->CreateRenderTargetView(pBackBuffer.Get(), NULL, &m_mainRenderTargetView);
}

void Overlay::CleanupRenderTarget() {
    m_mainRenderTargetView.Reset();
}

void Overlay::DrawColorPickerOverlay(AppConfig& config) {
    if (!config.colorPickerActive) return;

    if (!m_stillFrameCaptured) {
        // Draw a neat centered status overlay while capturing
        ImGui::SetNextWindowPos(ImVec2((m_screenWidth - 300) * 0.5f, (m_screenHeight - 80) * 0.5f));
        ImGui::SetNextWindowSize(ImVec2(300, 80));
        ImGui::Begin("##CapturingPicker", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("Taking still frame...");
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.38f, 0.45f, 0.90f, 1.0f), "Please wait...");
        ImGui::End();
        return;
    }

    if (!m_stillFrameSRV) {
        config.colorPickerActive = false;
        m_stillFrameCaptured = false;
        return;
    }

    // Escape cancels the operation
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        config.colorPickerActive = false;
        m_stillFrameCaptured = false;
        m_stillFrameSRV.Reset();
        m_stillFrameMat.release();
        return;
    }

    // Fullscreen borderless window to display the still image and capture clicks
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(m_screenWidth), static_cast<float>(m_screenHeight)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##ColorPickerFullscreen", nullptr, 
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | 
                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings | 
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Draw the still image
    ImGui::Image(reinterpret_cast<void*>(m_stillFrameSRV.Get()), ImVec2(static_cast<float>(m_screenWidth), static_cast<float>(m_screenHeight)));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mousePos = ImGui::GetMousePos();
    int mx = static_cast<int>(mousePos.x);
    int my = static_cast<int>(mousePos.y);

    // Clamp coordinates to screen bounds for pixel querying
    if (mx >= 0 && mx < m_screenWidth && my >= 0 && my < m_screenHeight && !m_stillFrameMat.empty()) {
        cv::Vec4b centerPixel = m_stillFrameMat.at<cv::Vec4b>(my, mx);
        unsigned char cb = centerPixel[0];
        unsigned char cg = centerPixel[1];
        unsigned char cr = centerPixel[2];

        // Draw Magnifier
        float boxW = 160.0f;
        float boxH = 210.0f;
        float magX = mousePos.x + 35.0f;
        float magY = mousePos.y - 230.0f;

        // Prevent magnifier from drawing offscreen
        if (magX + boxW > m_screenWidth) magX = mousePos.x - boxW - 35.0f;
        if (magY < 0.0f) magY = mousePos.y + 35.0f;

        ImVec2 magMin(magX, magY);
        ImVec2 magMax(magX + boxW, magY + boxH);

        // Premium background and border for magnifier
        DrawGlowRect(dl, magMin, magMax, ImVec4(0.38f, 0.45f, 0.90f, 1.0f), 8.0f, 0.35f, 4, 6.0f);
        dl->AddRectFilled(magMin, magMax, ImGui::ColorConvertFloat4ToU32({0.09f, 0.09f, 0.12f, 0.95f}), 8.0f);
        dl->AddRect(magMin, magMax, ImGui::ColorConvertFloat4ToU32({0.38f, 0.45f, 0.90f, 0.8f}), 8.0f, 0, 1.5f);

        // Magnified grid size
        const int gridSize = 9; // 9x9 pixels
        float cellW = 140.0f / gridSize;
        float cellH = 140.0f / gridSize;
        ImVec2 gridStart(magMin.x + 10.0f, magMin.y + 10.0f);

        // Draw pixel grid
        for (int dy = -gridSize/2; dy <= gridSize/2; ++dy) {
            for (int dx = -gridSize/2; dx <= gridSize/2; ++dx) {
                int px = mx + dx;
                int py = my + dy;
                
                // Clamp pixel coordinates
                if (px < 0) px = 0;
                if (py < 0) py = 0;
                if (px >= m_screenWidth) px = m_screenWidth - 1;
                if (py >= m_screenHeight) py = m_screenHeight - 1;

                cv::Vec4b pix = m_stillFrameMat.at<cv::Vec4b>(py, px);
                ImU32 cellColor = IM_COL32(pix[2], pix[1], pix[0], 255);

                ImVec2 cellMin(gridStart.x + (dx + gridSize/2) * cellW, gridStart.y + (dy + gridSize/2) * cellH);
                ImVec2 cellMax(cellMin.x + cellW, cellMin.y + cellH);

                dl->AddRectFilled(cellMin, cellMax, cellColor);
                
                // Fine separator line between magnified cells
                dl->AddRect(cellMin, cellMax, IM_COL32(45, 45, 60, 45), 0.0f, 0, 0.5f);
            }
        }

        // Highlight targeted center pixel in grid
        ImVec2 centerCellMin(gridStart.x + (gridSize/2) * cellW, gridStart.y + (gridSize/2) * cellH);
        ImVec2 centerCellMax(centerCellMin.x + cellW, centerCellMin.y + cellH);
        dl->AddRect(centerCellMin, centerCellMax, IM_COL32(255, 255, 255, 255), 0.0f, 0, 1.5f);
        dl->AddRect({centerCellMin.x - 1, centerCellMin.y - 1}, {centerCellMax.x + 1, centerCellMax.y + 1}, IM_COL32(0, 0, 0, 255), 0.0f, 0, 0.75f);

        // Render RGB / HEX / instruction text below grid
        char rgbText[64];
        sprintf_s(rgbText, "RGB: %d, %d, %d", cr, cg, cb);
        char hexText[64];
        sprintf_s(hexText, "HEX: #%02X%02X%02X", cr, cg, cb);

        dl->AddText({magMin.x + 12.0f, magMin.y + 155.0f}, IM_COL32(240, 240, 250, 255), rgbText);
        dl->AddText({magMin.x + 12.0f, magMin.y + 172.0f}, ImGui::ColorConvertFloat4ToU32({0.38f, 0.45f, 0.90f, 1.0f}), hexText);
        dl->AddText({magMin.x + 12.0f, magMin.y + 190.0f}, IM_COL32(140, 145, 160, 255), "LClick to Pick. ESC to Exit");

        // Set target color on left click
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            config.colorTargetR = cr;
            config.colorTargetG = cg;
            config.colorTargetB = cb;

            config.colorPickerActive = false;
            m_stillFrameCaptured = false;
            m_stillFrameSRV.Reset();
            m_stillFrameMat.release();
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
}
