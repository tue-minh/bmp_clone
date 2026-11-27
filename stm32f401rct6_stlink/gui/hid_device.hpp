#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct HidDeviceInfo {
    std::wstring path;
    std::wstring product;
    std::wstring manufacturer;
    std::wstring serial;
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint16_t inReportLen = 65;
    uint16_t outReportLen = 65;
};

class WinHidDevice {
public:
    WinHidDevice() = default;
    ~WinHidDevice();

    WinHidDevice(const WinHidDevice&) = delete;
    WinHidDevice& operator=(const WinHidDevice&) = delete;

    bool open(const HidDeviceInfo& info, std::string& errOut);
    void close();
    bool isOpen() const { return handle_ != INVALID_HANDLE_VALUE; }

    std::vector<uint8_t> exchange(const std::vector<uint8_t>& payload, uint32_t timeoutMs = 2000);

    static std::vector<HidDeviceInfo> enumerateDapDevices();
    static GUID getHidGuid();

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE evW_ = nullptr;
    HANDLE evR_ = nullptr;
    uint16_t inLen_ = 65;
    uint16_t outLen_ = 65;
};
