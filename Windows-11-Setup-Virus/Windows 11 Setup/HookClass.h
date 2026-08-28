#ifndef DISABLE_WIN_KEY_H
#define DISABLE_WIN_KEY_H

#include <windows.h>
#include <iostream>


namespace WinKeyBlocker {

    static HHOOK hHook = nullptr;

    LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode >= 0) {
            KBDLLHOOKSTRUCT* pKeyStruct = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

            if (pKeyStruct->vkCode == VK_LWIN || pKeyStruct->vkCode == VK_RWIN) {
                return 1;
            }
        }
        return CallNextHookEx(hHook, nCode, wParam, lParam);
    }
    inline void Enable() {
        if (hHook != nullptr) {
            return;
        }
        hHook = SetWindowsHookEx(
            WH_KEYBOARD_LL,
            LowLevelKeyboardProc,
            GetModuleHandle(NULL),
            0
        );

        if (hHook == nullptr) {
            std::cerr << "[Error] Failed to install keyboard hook. Error code: " << GetLastError() << std::endl;
        }
        else {
            std::cout << "[Success] Win key blocker installed. Press Ctrl+C to exit (if in console)." << std::endl;
        }
    }

    inline void Disable() {
        if (hHook != nullptr) {
            UnhookWindowsHookEx(hHook);
            hHook = nullptr;
            std::cout << "[Info] Win key blocker removed." << std::endl;
        }
    }

    inline bool IsActive() {
        return hHook != nullptr;
    }

} 
#endif
