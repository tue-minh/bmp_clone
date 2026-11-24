#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

struct ComPortInfo {
    std::wstring portName;     // e.g. L"COM4"
    std::wstring friendlyName; // e.g. L"USB Serial Device (COM4)"
};

class SerialPort {
public:
    SerialPort() = default;
    ~SerialPort();

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    bool open(const std::wstring& portName, uint32_t baudRate, std::string& errOut,
              std::function<void(const std::string&)> onRx,
              std::function<void()> onClosed);
    void close();
    bool isOpen() const { return running_.load(); }
    bool writeText(const std::string& text);

    static std::vector<ComPortInfo> enumeratePorts();

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::atomic<bool> running_{false};
    std::thread rxThread_;
};
