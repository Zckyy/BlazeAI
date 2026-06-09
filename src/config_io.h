#pragma once

#include <string>
#include "overlay.h"

// Persistence for AppConfig to a `config.ini` placed next to the executable.
//
// Only the user-tunable settings are persisted; runtime/telemetry fields
// (timings, FPS, available-model list, transient picker flags) are skipped so
// the file stays a clean record of what the UI controls.

// Absolute path to the config file (same directory as the running .exe).
std::wstring GetConfigPath();

// Read config.ini into `config`. Missing file or keys leave defaults intact.
void LoadConfig(AppConfig& config);

// Write the persisted settings of `config` to config.ini (overwrites).
void SaveConfig(const AppConfig& config);

// True when every persisted setting matches between `a` and `b`. Used by the
// UI loop to detect when an ImGui change should trigger a save.
bool SettingsEqual(const AppConfig& a, const AppConfig& b);
