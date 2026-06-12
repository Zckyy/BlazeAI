#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstdio>

// MAKCU serial mouse backend. The MAKCU is an external USB device (CH343 serial)
// that injects mouse movement at the hardware level, fully bypassing the Windows
// input stack (SendInput hooks, NtUserInjectMouseInput, etc.).
//
// Ported from SunOner/sunone_aimbot_2 (mouse/Makcu + modules/makcu). We talk the
// MAKCU serial protocol directly rather than pulling in the whole upstream module:
//   - device boots at 115200 baud
//   - sending DE AD 05 00 A5 <baud_u32_LE> switches it to a high-speed baud
//   - movement/clicks are plain ASCII commands: "km.move(dx,dy)\r\n", "km.left(1)" ...
//
// All serial writes are serialized by m_writeMutex because the UI thread (Test
// button / connect) and the processing thread (aim-assist) both drive moves.
class MakcuInput {
public:
    static constexpr unsigned int INITIAL_BAUD = 115200;
    static constexpr unsigned int HIGH_SPEED_BAUD = 4000000;

    ~MakcuInput() { Disconnect(); }

    // Enumerate currently-present COM ports (e.g. {"COM3","COM4"}) via QueryDosDevice.
    static std::vector<std::string> EnumPorts() {
        std::vector<std::string> ports;
        char targetPath[512];
        for (int i = 1; i <= 256; ++i) {
            char name[16];
            snprintf(name, sizeof(name), "COM%d", i);
            // QueryDosDevice succeeds only for COM names that actually map to a device.
            if (QueryDosDeviceA(name, targetPath, sizeof(targetPath)) != 0) {
                ports.push_back(name);
            }
        }
        return ports;
    }

    // Open the given port and validate that a MAKCU is actually responding. Returns
    // true and flips IsConnected() only when the device answers km.version().
    bool Connect(const std::string& port) {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        CloseHandleLocked();

        // CreateFile wants the \\.\COMx form for ports >= COM10 (harmless for low ones).
        const std::string path = "\\\\.\\" + port;
        m_handle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) {
            m_handle = nullptr;
            return false;
        }

        ConfigureBaud(INITIAL_BAUD);

        // Ask the device to jump to high speed, then follow it up on our side. If the
        // driver rejects the custom rate we silently stay usable at 115200.
        unsigned char sw[9] = { 0xDE, 0xAD, 0x05, 0x00, 0xA5, 0, 0, 0, 0 };
        sw[5] = static_cast<unsigned char>(HIGH_SPEED_BAUD & 0xFF);
        sw[6] = static_cast<unsigned char>((HIGH_SPEED_BAUD >> 8) & 0xFF);
        sw[7] = static_cast<unsigned char>((HIGH_SPEED_BAUD >> 16) & 0xFF);
        sw[8] = static_cast<unsigned char>((HIGH_SPEED_BAUD >> 24) & 0xFF);
        DWORD written = 0;
        WriteFile(m_handle, sw, sizeof(sw), &written, nullptr);
        Sleep(20);
        if (!ConfigureBaud(HIGH_SPEED_BAUD)) {
            ConfigureBaud(INITIAL_BAUD); // fall back if the high rate wasn't accepted
        }
        PurgeComm(m_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

        // Validate: a real MAKCU replies to km.version(). Anything that answers we accept.
        WriteLocked("km.version()\r\n");
        m_connected = ReadAny(200);
        if (!m_connected) {
            CloseHandleLocked();
        } else {
            m_port = port;
        }
        return m_connected;
    }

    void Disconnect() {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        CloseHandleLocked();
        m_port.clear();
    }

    bool IsConnected() const { return m_connected; }
    std::string CurrentPort() const { return m_port; }

    // dx/dy are relative pixel deltas, matching the SendInput/NtInput backends.
    void MoveRelative(int dx, int dy) {
        if (!m_connected) return;
        char cmd[48];
        int n = snprintf(cmd, sizeof(cmd), "km.move(%d,%d)\r\n", dx, dy);
        std::lock_guard<std::mutex> lock(m_writeMutex);
        WriteLocked(cmd, n);
    }

    // Left mouse button press/release.
    void Click(bool down) {
        if (!m_connected) return;
        std::lock_guard<std::mutex> lock(m_writeMutex);
        WriteLocked(down ? "km.left(1)\r\n" : "km.left(0)\r\n");
    }

private:
    // Apply a baud rate + 8N1 + non-blocking timeouts to the open handle.
    bool ConfigureBaud(unsigned int baud) {
        if (!m_handle) return false;
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(m_handle, &dcb)) return false;
        dcb.BaudRate = baud;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        if (!SetCommState(m_handle, &dcb)) return false;

        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = MAXDWORD; // return immediately with whatever is buffered
        SetCommTimeouts(m_handle, &to);
        return true;
    }

    void WriteLocked(const char* data, int len = -1) {
        if (!m_handle) return;
        DWORD written = 0;
        WriteFile(m_handle, data, len < 0 ? static_cast<DWORD>(strlen(data)) : len, &written, nullptr);
    }

    // Poll for any inbound byte for up to timeoutMs; true if the device said anything.
    bool ReadAny(int timeoutMs) {
        if (!m_handle) return false;
        const DWORD start = GetTickCount();
        char buf[64];
        while (GetTickCount() - start < static_cast<DWORD>(timeoutMs)) {
            DWORD read = 0;
            if (ReadFile(m_handle, buf, sizeof(buf), &read, nullptr) && read > 0) {
                return true;
            }
            Sleep(5);
        }
        return false;
    }

    void CloseHandleLocked() {
        m_connected = false;
        if (m_handle) {
            CloseHandle(m_handle);
            m_handle = nullptr;
        }
    }

    HANDLE m_handle = nullptr;
    bool m_connected = false;
    std::string m_port;
    std::mutex m_writeMutex;
};

inline MakcuInput g_makcu;
