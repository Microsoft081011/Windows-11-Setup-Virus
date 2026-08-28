#include "HookClass.h"
#include <iostream>
#include <thread>
#include <chrono>

int HookClassMain() {
    WinKeyBlocker::Enable();

    if (!WinKeyBlocker::IsActive()) {
        return 1;
    }
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
