#include "serial_port.hpp"

#include <initguid.h>
#include <setupapi.h>
#include <devguid.h>
#include <algorithm>
#include <vector>

std::vector<ComPortInfo> SerialPort::enumeratePorts() {
    std::vector<ComPortInfo> ports;

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(
        &GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (hDevInfo != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA devData{};
        devData.cbSize = sizeof(SP_DEVINFO_DATA);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devData); ++i) {
            wchar_t friendly[256] = {0};
            if (SetupDiGetDeviceRegistryPropertyW(
                    hDevInfo, &devData, SPDRP_FRIENDLYNAME, nullptr,
                    reinterpret_cast<PBYTE>(friendly), sizeof(friendly), nullptr)) {
                std::wstring fstr = friendly;
                size_t p1 = fstr.rfind(L"(COM");
                size_t p2 = fstr.rfind(L')');
                if (p1 != std::wstring::npos && p2 != std::wstring::npos && p2 > p1 + 1) {
                    ComPortInfo info;
                    info.portName = fstr.substr(p1 + 1, p2 - p1 - 1);
                    info.friendlyName = fstr;
                    ports.push_back(std::move(info));
                }
            }
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }

    // Prioritize USB Serial Device
    std::sort(ports.begin(), ports.end(), [](const ComPortInfo& a, const ComPortInfo& b) {
        bool aUsb = (a.friendlyName.find(L"USB Serial") != std::wstring::npos);
        bool bUsb = (b.friendlyName.find(L"USB Serial") != std::wstring::npos);
        if (aUsb != bUsb) return aUsb > bUsb;
        return a.portName < b.portName;
    });

    return ports;
}

SerialPort::~SerialPort() {
    close();
}

bool SerialPort::open(const std::wstring& portName, uint32_t baudRate, std::string& errOut,
                      std::function<void(const std::string&)> onRx,
                      std::function<void()> onClosed) {
    close();

    std::wstring devPath = L"\\\\.\\" + portName;
    handle_ = CreateFileW(
        devPath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        errOut = "Cannot open COM port (Win32 error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    SetupComm(handle_, 4096, 4096);

    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (GetCommState(handle_, &dcb)) {
        dcb.BaudRate = baudRate;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fErrorChar = FALSE;
        dcb.fNull = FALSE;
        dcb.fAbortOnError = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        SetCommState(handle_, &dcb);
    }

    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = 50;
    to.WriteTotalTimeoutMultiplier = 10;
    to.WriteTotalTimeoutConstant = 1000;
    SetCommTimeouts(handle_, &to);

    PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);

    running_.store(true);
    rxThread_ = std::thread([this, onRx, onClosed]() {
        HANDLE evRead = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        char buf[512];
        while (running_.load()) {
            ResetEvent(evRead);
            OVERLAPPED ov{};
            ov.hEvent = evRead;
            DWORD nRead = 0;

            BOOL ok = ReadFile(handle_, buf, sizeof(buf), &nRead, &ov);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    DWORD waitRes = WaitForSingleObject(evRead, 100);
                    if (!running_.load()) {
                        CancelIoEx(handle_, &ov);
                        break;
                    }
                    if (waitRes == WAIT_OBJECT_0) {
                        if (!GetOverlappedResult(handle_, &ov, &nRead, FALSE)) {
                            DWORD ovErr = GetLastError();
                            if (ovErr != ERROR_OPERATION_ABORTED) break;
                        }
                    } else if (waitRes == WAIT_TIMEOUT) {
                        CancelIoEx(handle_, &ov);
                        DWORD dummy = 0;
                        GetOverlappedResult(handle_, &ov, &dummy, TRUE);
                        continue;
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }
            if (nRead > 0 && onRx) {
                onRx(std::string(buf, buf + nRead));
            }
        }
        CloseHandle(evRead);
        bool wasRunning = running_.exchange(false);
        if (wasRunning && onClosed) {
            onClosed();
        }
    });
    return true;
}

void SerialPort::close() {
    running_.store(false);
    if (handle_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(handle_, nullptr);
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    if (rxThread_.joinable()) {
        if (rxThread_.get_id() == std::this_thread::get_id()) {
            rxThread_.detach();
        } else {
            rxThread_.join();
        }
    }
}

bool SerialPort::writeText(const std::string& text) {
    if (!isOpen() || handle_ == INVALID_HANDLE_VALUE || text.empty()) return false;

    HANDLE evWrite = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED ov{};
    ov.hEvent = evWrite;
    DWORD written = 0;

    BOOL ok = WriteFile(handle_, text.data(), static_cast<DWORD>(text.size()), &written, &ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            if (WaitForSingleObject(evWrite, 1500) == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(handle_, &ov, &written, FALSE);
            } else {
                CancelIoEx(handle_, &ov);
                ok = FALSE;
            }
        }
    }
    CloseHandle(evWrite);
    return ok != FALSE;
}
