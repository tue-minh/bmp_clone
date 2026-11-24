#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class OpenOcdRunner {
public:
    OpenOcdRunner() = default;
    ~OpenOcdRunner();

    static int runOnce(const std::wstring& ocdExe,
                       const std::wstring& targetCfg,
                       uint32_t speedKhz,
                       const std::vector<std::wstring>& extraCmds,
                       std::function<void(const std::string&)> onLog);

    bool startGdbServer(const std::wstring& ocdExe,
                        const std::wstring& targetCfg,
                        uint32_t speedKhz,
                        std::function<void(const std::string&)> onLog,
                        std::function<void()> onStopped);
    void stopGdbServer();
    bool isGdbRunning() const { return gdbRunning_.load(); }

private:
    HANDLE hGdbProc_ = nullptr;
    HANDLE hGdbPipeRead_ = nullptr;
    std::atomic<bool> gdbRunning_{false};
    std::thread gdbThread_;
};
