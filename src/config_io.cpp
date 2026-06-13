#include "config_io.h"

#include <windows.h>
#include <fstream>
#include <sstream>
#include <unordered_map>

// --- Narrow/wide conversion (config.ini is stored as UTF-8) -----------------

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), len, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

std::wstring GetConfigPath() {
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    size_t slash = path.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
    return dir + L"\\config.ini";
}

// --- Save -------------------------------------------------------------------

void SaveConfig(const AppConfig& config) {
    std::ofstream f(GetConfigPath());
    if (!f) return;

    f << "# BlazeAI configuration. Edited automatically when UI settings change.\n";

    f << "selectedModelPath=" << WideToUtf8(config.selectedModelPath) << "\n";

    f << "visionMode=" << config.visionMode << "\n";
    f << "colorTargetR=" << config.colorTargetR << "\n";
    f << "colorTargetG=" << config.colorTargetG << "\n";
    f << "colorTargetB=" << config.colorTargetB << "\n";
    f << "colorTolerance=" << config.colorTolerance << "\n";
    f << "colorMinArea=" << config.colorMinArea << "\n";

    f << "useTensorRT=" << (config.useTensorRT ? 1 : 0) << "\n";
    f << "trtFp16=" << (config.trtFp16 ? 1 : 0) << "\n";

    f << "fovSize=" << config.fovSize << "\n";
    f << "targetFps=" << config.targetFps << "\n";
    f << "maxDetections=" << config.maxDetections << "\n";
    f << "confThreshold=" << config.confThreshold << "\n";

    f << "autoAim=" << (config.autoAim ? 1 : 0) << "\n";
    f << "aimHeightRatio=" << config.aimHeightRatio << "\n";
    f << "showBoundingBoxes=" << (config.showBoundingBoxes ? 1 : 0) << "\n";
    f << "showFov=" << (config.showFov ? 1 : 0) << "\n";
    f << "fovShape=" << config.fovShape << "\n";
    f << "showVisuals=" << (config.showVisuals ? 1 : 0) << "\n";
    f << "streamProof=" << (config.streamProof ? 1 : 0) << "\n";
    f << "showMenu=" << (config.showMenu ? 1 : 0) << "\n";
    f << "showAimVisualizer=" << (config.showAimVisualizer ? 1 : 0) << "\n";
    f << "showSmoothingTrail=" << (config.showSmoothingTrail ? 1 : 0) << "\n";
    f << "showTargetVector=" << (config.showTargetVector ? 1 : 0) << "\n";
    f << "showRawAimPoint=" << (config.showRawAimPoint ? 1 : 0) << "\n";
    f << "showSmoothedAimPoint=" << (config.showSmoothedAimPoint ? 1 : 0) << "\n";
    f << "hotkeyKey=" << config.hotkeyKey << "\n";

    f << "trackerEnabled=" << (config.trackerEnabled ? 1 : 0) << "\n";
    f << "trackerIou=" << config.trackerIou << "\n";
    f << "trackerMaxMissed=" << config.trackerMaxMissed << "\n";
    f << "trackerSwitchRatio=" << config.trackerSwitchRatio << "\n";

    f << "aimbot_humanized=" << (config.aimbot_humanized ? 1 : 0) << "\n";
    f << "aimbot_relative=" << (config.aimbot_relative ? 1 : 0) << "\n";
    f << "mouseInputMethod=" << config.mouseInputMethod << "\n";
    f << "vigemStickScale=" << config.vigemStickScale << "\n";
    f << "aimbot_sensitivity=" << config.aimbot_sensitivity << "\n";
    f << "aimbot_smooth=" << config.aimbot_smooth << "\n";
    f << "aimbot_jitter=" << config.aimbot_jitter << "\n";
    f << "aimbot_curve_strength=" << config.aimbot_curve_strength << "\n";
    f << "aimbot_ease_in=" << config.aimbot_ease_in << "\n";
    f << "aimbot_ease_out=" << config.aimbot_ease_out << "\n";
}

// --- Load -------------------------------------------------------------------

void LoadConfig(AppConfig& config) {
    std::ifstream f(GetConfigPath());
    if (!f) return; // No file yet: keep defaults.

    std::unordered_map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Trim trailing CR (file may have CRLF endings).
        if (!val.empty() && val.back() == '\r') val.pop_back();
        kv[key] = val;
    }

    auto getInt = [&](const char* key, int& dst) {
        auto it = kv.find(key);
        if (it != kv.end()) { try { dst = std::stoi(it->second); } catch (...) {} }
    };
    auto getFloat = [&](const char* key, float& dst) {
        auto it = kv.find(key);
        if (it != kv.end()) { try { dst = std::stof(it->second); } catch (...) {} }
    };
    auto getBool = [&](const char* key, bool& dst) {
        auto it = kv.find(key);
        if (it != kv.end()) dst = (it->second == "1" || it->second == "true");
    };

    {
        auto it = kv.find("selectedModelPath");
        if (it != kv.end()) config.selectedModelPath = Utf8ToWide(it->second);
    }

    getInt("visionMode", config.visionMode);
    getInt("colorTargetR", config.colorTargetR);
    getInt("colorTargetG", config.colorTargetG);
    getInt("colorTargetB", config.colorTargetB);
    getInt("colorTolerance", config.colorTolerance);
    getInt("colorMinArea", config.colorMinArea);

    getBool("useTensorRT", config.useTensorRT);
    getBool("trtFp16", config.trtFp16);

    getInt("fovSize", config.fovSize);
    getInt("targetFps", config.targetFps);
    getInt("maxDetections", config.maxDetections);
    getFloat("confThreshold", config.confThreshold);

    getBool("autoAim", config.autoAim);
    getFloat("aimHeightRatio", config.aimHeightRatio);
    getBool("showBoundingBoxes", config.showBoundingBoxes);
    getBool("showFov", config.showFov);
    getInt("fovShape", config.fovShape);
    getBool("showVisuals", config.showVisuals);
    getBool("streamProof", config.streamProof);
    getBool("showMenu", config.showMenu);
    getBool("showAimVisualizer", config.showAimVisualizer);
    getBool("showSmoothingTrail", config.showSmoothingTrail);
    getBool("showTargetVector", config.showTargetVector);
    getBool("showRawAimPoint", config.showRawAimPoint);
    getBool("showSmoothedAimPoint", config.showSmoothedAimPoint);
    getInt("hotkeyKey", config.hotkeyKey);

    getBool("trackerEnabled", config.trackerEnabled);
    getFloat("trackerIou", config.trackerIou);
    getInt("trackerMaxMissed", config.trackerMaxMissed);
    getFloat("trackerSwitchRatio", config.trackerSwitchRatio);

    getBool("aimbot_humanized", config.aimbot_humanized);
    getBool("aimbot_relative", config.aimbot_relative);
    getInt("mouseInputMethod", config.mouseInputMethod);
    getFloat("vigemStickScale", config.vigemStickScale);
    getFloat("aimbot_sensitivity", config.aimbot_sensitivity);
    getFloat("aimbot_smooth", config.aimbot_smooth);
    getFloat("aimbot_jitter", config.aimbot_jitter);
    getFloat("aimbot_curve_strength", config.aimbot_curve_strength);
    getFloat("aimbot_ease_in", config.aimbot_ease_in);
    getFloat("aimbot_ease_out", config.aimbot_ease_out);
}

// --- Change detection -------------------------------------------------------

bool SettingsEqual(const AppConfig& a, const AppConfig& b) {
    return a.selectedModelPath == b.selectedModelPath
        && a.visionMode == b.visionMode
        && a.colorTargetR == b.colorTargetR
        && a.colorTargetG == b.colorTargetG
        && a.colorTargetB == b.colorTargetB
        && a.colorTolerance == b.colorTolerance
        && a.colorMinArea == b.colorMinArea
        && a.useTensorRT == b.useTensorRT
        && a.trtFp16 == b.trtFp16
        && a.fovSize == b.fovSize
        && a.targetFps == b.targetFps
        && a.maxDetections == b.maxDetections
        && a.confThreshold == b.confThreshold
        && a.autoAim == b.autoAim
        && a.aimHeightRatio == b.aimHeightRatio
        && a.showBoundingBoxes == b.showBoundingBoxes
        && a.showFov == b.showFov
        && a.fovShape == b.fovShape
        && a.showVisuals == b.showVisuals
        && a.streamProof == b.streamProof
        && a.showMenu == b.showMenu
        && a.showAimVisualizer == b.showAimVisualizer
        && a.showSmoothingTrail == b.showSmoothingTrail
        && a.showTargetVector == b.showTargetVector
        && a.showRawAimPoint == b.showRawAimPoint
        && a.showSmoothedAimPoint == b.showSmoothedAimPoint
        && a.hotkeyKey == b.hotkeyKey
        && a.trackerEnabled == b.trackerEnabled
        && a.trackerIou == b.trackerIou
        && a.trackerMaxMissed == b.trackerMaxMissed
        && a.trackerSwitchRatio == b.trackerSwitchRatio
        && a.aimbot_humanized == b.aimbot_humanized
        && a.aimbot_relative == b.aimbot_relative
        && a.mouseInputMethod == b.mouseInputMethod
        && a.vigemStickScale == b.vigemStickScale
        && a.aimbot_sensitivity == b.aimbot_sensitivity
        && a.aimbot_smooth == b.aimbot_smooth
        && a.aimbot_jitter == b.aimbot_jitter
        && a.aimbot_curve_strength == b.aimbot_curve_strength
        && a.aimbot_ease_in == b.aimbot_ease_in
        && a.aimbot_ease_out == b.aimbot_ease_out;
}
