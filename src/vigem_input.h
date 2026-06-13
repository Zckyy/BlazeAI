#pragma once

#include <windows.h>
#include <xinput.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cmath>

#if BLAZE_HAVE_VIGEM
#include <ViGEm/Client.h>
#endif

// Virtual Xbox 360 controller backend (ViGEmBus). Lets aim-assist drive a
// virtual gamepad's right stick instead of mouse deltas, for games that
// ignore SendInput/raw-mouse once a controller is the "active" input device.
//
// Pairs with HidHide (separate tool, configured outside this app): HidHide
// must whitelist this process (so BlazeAI can still read the real pad via
// XInput) while hiding the real pad's HID interface from everything else
// (the game), so the game only ever sees this virtual X360 pad.
//
// A background thread continuously reads the real pad and forwards its full
// state (buttons, triggers, both sticks) to the virtual pad, with the
// aim-assist's right-stick correction summed on top of the real right stick.
// Without this passthrough the virtual pad would only ever output the
// aim-assist delta and your physical inputs would go nowhere.
#if BLAZE_HAVE_VIGEM
class VigemInput {
public:
    ~VigemInput() { Disconnect(); }

    // Allocates the ViGEm client, connects to the bus driver, plugs in a
    // virtual X360 pad, locates the real pad via XInput, and starts the
    // passthrough thread. Safe to call repeatedly; only does work once.
    bool Initialize() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized) return m_connected;
        m_initialized = true;

        // Find the real pad's XInput slot BEFORE the virtual pad exists, so it's
        // unambiguous which slot is "real" vs the one ViGEm is about to create.
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE state{};
            if (XInputGetState(i, &state) == ERROR_SUCCESS) {
                m_realIndex = static_cast<int>(i);
                break;
            }
        }

        m_client = vigem_alloc();
        if (!m_client) return false;

        if (!VIGEM_SUCCESS(vigem_connect(m_client))) {
            vigem_free(m_client);
            m_client = nullptr;
            return false;
        }

        m_target = vigem_target_x360_alloc();
        if (!m_target) {
            vigem_disconnect(m_client);
            vigem_free(m_client);
            m_client = nullptr;
            return false;
        }

        if (!VIGEM_SUCCESS(vigem_target_add(m_client, m_target))) {
            vigem_target_free(m_target);
            vigem_disconnect(m_client);
            vigem_free(m_client);
            m_client = nullptr;
            m_target = nullptr;
            return false;
        }

        m_connected = true;
        m_running = true;
        m_thread = std::thread(&VigemInput::PassthroughLoop, this);
        return true;
    }

    bool Available() {
        return Initialize();
    }

    bool IsConnected() const { return m_connected; }

    // True if a real XInput pad was found at Initialize time (the one being
    // passed through). False means only the aim-assist delta will reach the pad.
    bool HasRealPad() const { return m_realIndex >= 0; }

    // Left trigger state of the real pad, as last read by the passthrough thread.
    // Used as the aim-assist hotkey when the physical pad is hidden from
    // GetAsyncKeyState by HidHide.
    bool LeftTriggerHeld(BYTE threshold = 30) const {
        return m_realLeftTrigger.load(std::memory_order_relaxed) >= threshold;
    }

    // dx/dy are relative pixel deltas, matching the SendInput/NtUserInjectMouseInput/MAKCU
    // backends. Scaled into right-stick units (px -> stick units per pixel) and stored as
    // an offset that the passthrough thread adds on top of the real right stick. Expires
    // automatically if not refreshed (see kAimTimeoutMs), so releasing the aim hotkey
    // doesn't leave the stick pinned.
    void MoveRelative(int dx, int dy, float stickScale) const {
        const long rx = std::lround(static_cast<float>(dx) * stickScale);
        const long ry = std::lround(static_cast<float>(-dy) * stickScale); // screen Y down -> stick Y up

        m_aimRX.store(static_cast<SHORT>(std::clamp<long>(rx, -32768, 32767)), std::memory_order_relaxed);
        m_aimRY.store(static_cast<SHORT>(std::clamp<long>(ry, -32768, 32767)), std::memory_order_relaxed);
        m_aimTimestamp.store(GetTickCount64(), std::memory_order_relaxed);
    }

    // Trigger-bot fire: pulls the right trigger fully (most shooters map RT to fire/ADS-fire).
    // ORed with the real right trigger by the passthrough thread.
    void Click(bool down) const {
        m_aimTrigger.store(down, std::memory_order_relaxed);
    }

    void Disconnect() {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_target) {
            if (m_client) vigem_target_remove(m_client, m_target);
            vigem_target_free(m_target);
            m_target = nullptr;
        }
        if (m_client) {
            vigem_disconnect(m_client);
            vigem_free(m_client);
            m_client = nullptr;
        }
        m_connected = false;
        m_initialized = false;
        m_realIndex = -1;
    }

private:
    // How long an aim-assist stick correction stays applied after the last
    // MoveRelative call before it's treated as expired (stick relaxes to 0).
    static constexpr ULONGLONG kAimTimeoutMs = 100;

    void PassthroughLoop() {
        while (m_running) {
            XUSB_REPORT report{};

            if (m_realIndex >= 0) {
                XINPUT_STATE state{};
                if (XInputGetState(static_cast<DWORD>(m_realIndex), &state) == ERROR_SUCCESS) {
                    const XINPUT_GAMEPAD& pad = state.Gamepad;
                    report.wButtons = pad.wButtons;
                    report.bLeftTrigger = pad.bLeftTrigger;
                    report.bRightTrigger = pad.bRightTrigger;
                    report.sThumbLX = pad.sThumbLX;
                    report.sThumbLY = pad.sThumbLY;
                    report.sThumbRX = pad.sThumbRX;
                    report.sThumbRY = pad.sThumbRY;
                    m_realLeftTrigger.store(pad.bLeftTrigger, std::memory_order_relaxed);
                }
            }

            // Sum the aim-assist correction onto the real right stick, if still fresh.
            if (GetTickCount64() - m_aimTimestamp.load(std::memory_order_relaxed) <= kAimTimeoutMs) {
                const long rx = static_cast<long>(report.sThumbRX) + m_aimRX.load(std::memory_order_relaxed);
                const long ry = static_cast<long>(report.sThumbRY) + m_aimRY.load(std::memory_order_relaxed);
                report.sThumbRX = static_cast<SHORT>(std::clamp<long>(rx, -32768, 32767));
                report.sThumbRY = static_cast<SHORT>(std::clamp<long>(ry, -32768, 32767));

                if (m_aimTrigger.load(std::memory_order_relaxed)) {
                    report.bRightTrigger = 0xFF;
                }
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_client && m_target) {
                    vigem_target_x360_update(m_client, m_target, report);
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(4)); // ~250 Hz
        }
    }

    PVIGEM_CLIENT m_client = nullptr;
    PVIGEM_TARGET m_target = nullptr;
    bool m_initialized = false;
    bool m_connected = false;
    int m_realIndex = -1; // XInput user index of the physical pad, -1 if none found
    mutable std::mutex m_mutex;

    std::thread m_thread;
    std::atomic<bool> m_running{false};

    mutable std::atomic<SHORT> m_aimRX{0};
    mutable std::atomic<SHORT> m_aimRY{0};
    mutable std::atomic<bool> m_aimTrigger{false};
    mutable std::atomic<ULONGLONG> m_aimTimestamp{0};
    mutable std::atomic<BYTE> m_realLeftTrigger{0};
};
#else
// Stub used when third_party/vigem (the ViGEmClient SDK) isn't present. Keeps
// MOUSE_VIGEM selectable in the UI without making the SDK a hard build requirement.
class VigemInput {
public:
    bool Initialize() { return false; }
    bool Available() { return false; }
    bool IsConnected() const { return false; }
    bool HasRealPad() const { return false; }
    bool LeftTriggerHeld(BYTE = 30) const { return false; }
    void MoveRelative(int, int, float) const {}
    void Click(bool) const {}
    void Disconnect() {}
};
#endif

inline VigemInput g_vigem;
