#include "openocd_runner.hpp"

#include <sstream>

static std::wstring quoteArg(const std::wstring& s) {
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'\"') out += L"\\\"";
        else out += c;
    }
    out += L"\"";
    return out;
}

static std::wstring buildCmdLine(const std::wstring& ocdExe,
                                 const std::wstring& targetCfg,
                                 uint32_t speedKhz,
                                 const std::vector<std::wstring>& extraCmds) {
    std::wstring cmd = quoteArg(ocdExe);
    cmd += L" -f interface/cmsis-dap.cfg";
    cmd += L" -f " + quoteArg(targetCfg);
    cmd += L" -c " + quoteArg(L"adapter speed " + std::to_wstring(speedKhz));
    for (const auto& c : extraCmds) {
        cmd += L" -c " + quoteArg(c);
    }
    return cmd;
}

OpenOcdRunner::~OpenOcdRunner() {
    stopGdbServer();
}

int OpenOcdRunner::runOnce(const std::wstring& ocdExe,
                           const std::wstring& targetCfg,
                           uint32_t speedKhz,
                           const std::vector<std::wstring>& extraCmds,
                           std::function<void(const std::string&)> onLog) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        if (onLog) onLog("[OCD] Khong tao duoc pipe");
        return -1;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring cmdLine = buildCmdLine(ocdExe, targetCfg, speedKhz, extraCmds);

    BOOL ok = CreateProcessW(
        nullptr,
        &cmdLine[0],
        nullptr, nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);

    CloseHandle(hWrite);
    if (!ok) {
        CloseHandle(hRead);
        if (onLog) onLog("[OCD] Khong chay duoc openocd.exe (kiem tra duong dan trong Tools -> OpenOCD)");
        return -1;
    }

    std::string lineBuf;
    char buf[512];
    DWORD nRead = 0;
    while (ReadFile(hRead, buf, sizeof(buf), &nRead, nullptr) && nRead > 0) {
        lineBuf.append(buf, buf + nRead);
        size_t pos = 0;
        while ((pos = lineBuf.find('\n')) != std::string::npos) {
            std::string line = lineBuf.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (onLog && !line.empty()) onLog("[OCD] " + line);
            lineBuf.erase(0, pos + 1);
        }
    }
    if (!lineBuf.empty() && onLog) {
        onLog("[OCD] " + lineBuf);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    return static_cast<int>(exitCode);
}

bool OpenOcdRunner::startGdbServer(const std::wstring& ocdExe,
                                   const std::wstring& targetCfg,
                                   uint32_t speedKhz,
                                   std::function<void(const std::string&)> onLog,
                                   std::function<void()> onStopped) {
    stopGdbServer();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;

    PROCESS_INFORMATION pi{};
    std::wstring cmdLine = buildCmdLine(ocdExe, targetCfg, speedKhz, {});
    BOOL ok = CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);
    if (!ok) {
        CloseHandle(hRead);
        if (onLog) onLog("[GDB] Khong khoi dong duoc openocd.exe");
        return false;
    }

    CloseHandle(pi.hThread);
    hGdbProc_ = pi.hProcess;
    hGdbPipeRead_ = hRead;
    gdbRunning_.store(true);

    gdbThread_ = std::thread([this, onLog, onStopped]() {
        std::string lineBuf;
        char buf[512];
        DWORD nRead = 0;
        while (gdbRunning_.load() && ReadFile(hGdbPipeRead_, buf, sizeof(buf), &nRead, nullptr) && nRead > 0) {
            lineBuf.append(buf, buf + nRead);
            size_t pos = 0;
            while ((pos = lineBuf.find('\n')) != std::string::npos) {
                std::string line = lineBuf.substr(0, pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (onLog && !line.empty()) onLog("[GDB] " + line);
                lineBuf.erase(0, pos + 1);
            }
        }
        gdbRunning_.store(false);
        if (onStopped) onStopped();
    });

    return true;
}

void OpenOcdRunner::stopGdbServer() {
    gdbRunning_.store(false);
    if (hGdbProc_) {
        TerminateProcess(hGdbProc_, 0);
        WaitForSingleObject(hGdbProc_, 2000);
        CloseHandle(hGdbProc_);
        hGdbProc_ = nullptr;
    }
    if (hGdbPipeRead_) {
        CloseHandle(hGdbPipeRead_);
        hGdbPipeRead_ = nullptr;
    }
    if (gdbThread_.joinable()) {
        if (gdbThread_.get_id() == std::this_thread::get_id()) {
            gdbThread_.detach();
        } else {
            gdbThread_.join();
        }
    }
}
