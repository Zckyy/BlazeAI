#pragma once

#include <windows.h>
#include <cstdint>

// Alternative mouse-movement backend using the undocumented win32u.dll
// NtUserInjectMouseInput, which bypasses some SendInput hooks/detections.
// Mirrors the approach used in the Blazestrike project.
class NtInput {
public:
    bool Initialize() {
        if (m_initialized) return m_inject_mouse != nullptr;
        m_initialized = true;

        const HMODULE win32u = GetModuleHandleA("win32u.dll");
        if (!win32u) return false;

        m_inject_mouse = reinterpret_cast<fn_inject_mouse>(GetProcAddress(win32u, "NtUserInjectMouseInput"));
        return m_inject_mouse != nullptr;
    }

    bool Available() {
        return Initialize();
    }

    // dx/dy are relative pixel deltas (MOUSEEVENTF_MOVE, no ABSOLUTE flag).
    void MoveRelative(int dx, int dy) const {
        if (!m_inject_mouse) return;

        mouse_info_t info{};
        info.pt.x = dx;
        info.pt.y = dy;
        info.flags = MOUSEEVENTF_MOVE;
        m_inject_mouse(&info, 1);
    }

private:
    struct mouse_info_t {
        POINT pt;
        unsigned long data;
        unsigned long flags;
        unsigned long time;
        std::uintptr_t extra_info;
    };

    using fn_inject_mouse = BOOL(NTAPI*)(mouse_info_t*, int);

    fn_inject_mouse m_inject_mouse{};
    bool m_initialized{false};
};

inline NtInput g_ntInput;
