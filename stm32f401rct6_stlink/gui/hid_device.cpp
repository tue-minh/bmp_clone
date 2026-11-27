#include "hid_device.hpp"

#include <setupapi.h>
#include <hidsdi.h>
#include <algorithm>
#include <stdexcept>

static std::wstring wideLower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(towlower(ch));
    return s;
}

GUID WinHidDevice::getHidGuid() {
    GUID g{};
    HidD_GetHidGuid(&g);
    return g;
}

std::vector<HidDeviceInfo> WinHidDevice::enumerateDapDevices() {
    std::vector<HidDeviceInfo> result;
    GUID hidGuid = getHidGuid();

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) {
        return result;
    }

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &hidGuid, idx, &ifData); ++idx) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, nullptr, 0, &reqSize, nullptr);
        if (reqSize == 0) continue;

        std::vector<uint8_t> detailBuf(reqSize, 0);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, reqSize, nullptr, nullptr)) {
            continue;
        }

        HANDLE h = CreateFileW(
            detail->DevicePath,
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs{};
        attrs.Size = sizeof(HIDD_ATTRIBUTES);
        if (HidD_GetAttributes(h, &attrs)) {
            wchar_t prodBuf[128] = {0};
            wchar_t mfgBuf[128] = {0};
            wchar_t serBuf[128] = {0};
            HidD_GetProductString(h, prodBuf, sizeof(prodBuf));
            HidD_GetManufacturerString(h, mfgBuf, sizeof(mfgBuf));
            HidD_GetSerialNumberString(h, serBuf, sizeof(serBuf));

            std::wstring prod = prodBuf;
            std::wstring prodLow = wideLower(prod);
            bool isTue = (attrs.VendorID == 0x0483 && (attrs.ProductID == 0x5752 || attrs.ProductID == 0x5750));
            bool isDap = (prodLow.find(L"cmsis-dap") != std::wstring::npos ||
                          prodLow.find(L"tue") != std::wstring::npos);

            if (isTue || isDap) {
                uint16_t inLen = 65;
                uint16_t outLen = 65;
                PHIDP_PREPARSED_DATA ppd = nullptr;
                if (HidD_GetPreparsedData(h, &ppd)) {
                    HIDP_CAPS caps{};
                    if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                        inLen = caps.InputReportByteLength;
                        outLen = caps.OutputReportByteLength;
                    }
                    HidD_FreePreparsedData(ppd);
                }
                HidDeviceInfo info;
                info.path = detail->DevicePath;
                info.product = prod.empty() ? L"CMSIS-DAP HID" : prod;
                info.manufacturer = mfgBuf;
                info.serial = serBuf;
                info.vid = attrs.VendorID;
                info.pid = attrs.ProductID;
                info.inReportLen = inLen > 0 ? inLen : 65;
                info.outReportLen = outLen > 0 ? outLen : 65;
                result.push_back(std::move(info));
            }
        }
        CloseHandle(h);
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);

    // Prioritize TUE CMSIS-DAP (0x0483:0x5752)
    std::sort(result.begin(), result.end(), [](const HidDeviceInfo& a, const HidDeviceInfo& b) {
        bool aTue = (a.vid == 0x0483 && a.pid == 0x5752);
        bool bTue = (b.vid == 0x0483 && b.pid == 0x5752);
        return aTue > bTue;
    });

    return result;
}

WinHidDevice::~WinHidDevice() {
    close();
}

bool WinHidDevice::open(const HidDeviceInfo& info, std::string& errOut) {
    close();
    handle_ = CreateFileW(
        info.path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        errOut = "Cannot open HID device (Win32 error " + std::to_string(e) + ")";
        return false;
    }
    inLen_ = info.inReportLen;
    outLen_ = info.outReportLen;
    evW_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    evR_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!evW_ || !evR_) {
        close();
        errOut = "Cannot create Win32 event handles";
        return false;
    }
    HidD_SetNumInputBuffers(handle_, 32);
    return true;
}

void WinHidDevice::close() {
    if (evW_) {
        CloseHandle(evW_);
        evW_ = nullptr;
    }
    if (evR_) {
        CloseHandle(evR_);
        evR_ = nullptr;
    }
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
}

std::vector<uint8_t> WinHidDevice::exchange(const std::vector<uint8_t>& payload, uint32_t timeoutMs) {
    if (!isOpen()) {
        throw std::runtime_error("HID device is not open");
    }

    std::vector<uint8_t> outBuf(outLen_, 0);
    size_t copyLen = (std::min)(payload.size(), static_cast<size_t>(outLen_ - 1));
    outBuf[0] = 0x00; // Report ID 0
    for (size_t i = 0; i < copyLen; ++i) {
        outBuf[1 + i] = payload[i];
    }

    ResetEvent(evW_);
    OVERLAPPED ovW{};
    ovW.hEvent = evW_;

    DWORD written = 0;
    BOOL okW = WriteFile(handle_, outBuf.data(), outLen_, &written, &ovW);
    if (!okW) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            throw std::runtime_error("HID WriteFile failed (err=" + std::to_string(err) + ")");
        }
        if (WaitForSingleObject(evW_, timeoutMs) != WAIT_OBJECT_0) {
            CancelIo(handle_);
            throw std::runtime_error("HID WriteFile timeout");
        }
        GetOverlappedResult(handle_, &ovW, &written, FALSE);
    }

    std::vector<uint8_t> inBuf(inLen_, 0);
    ResetEvent(evR_);
    OVERLAPPED ovR{};
    ovR.hEvent = evR_;

    DWORD readBytes = 0;
    BOOL okR = ReadFile(handle_, inBuf.data(), inLen_, &readBytes, &ovR);
    if (!okR) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            throw std::runtime_error("HID ReadFile failed (err=" + std::to_string(err) + ")");
        }
        if (WaitForSingleObject(evR_, timeoutMs) != WAIT_OBJECT_0) {
            CancelIo(handle_);
            throw std::runtime_error("HID ReadFile timeout");
        }
        GetOverlappedResult(handle_, &ovR, &readBytes, FALSE);
    }

    if (readBytes > 1) {
        return std::vector<uint8_t>(inBuf.begin() + 1, inBuf.begin() + readBytes);
    }
    return {};
}
