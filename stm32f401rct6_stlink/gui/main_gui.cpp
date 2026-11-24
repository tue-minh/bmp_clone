#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dbt.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "resource.h"
#include "hid_device.hpp"
#include "firmware_loader.hpp"
#include "cmsis_dap.hpp"
#include "serial_port.hpp"
#include "openocd_runner.hpp"

// ---------------------------------------------------------------------------
// Global State & Handles
// ---------------------------------------------------------------------------
static HINSTANCE g_hInst = nullptr;
static HWND g_hMainWnd = nullptr;
static HWND g_hDebugWnd = nullptr;
static HWND g_hHexWnd = nullptr;
static HWND g_hUartWnd = nullptr;
static HWND g_hOcdDlg = nullptr;
static HDEVNOTIFY g_hDevNotifyHid = nullptr;

static HFONT g_fontUi = nullptr;
static HFONT g_fontUiBold = nullptr;
static HFONT g_fontMono = nullptr;

// Main window controls (Fixed layout)
static HWND g_lblUsbStatus = nullptr;
static HWND g_chkUseOcd = nullptr;
static HWND g_cmbOcdTarget = nullptr;
static HWND g_cmbSwdSpeed = nullptr;
static HWND g_chkUnderReset = nullptr;

static HWND g_btnConnect = nullptr;
static HWND g_btnDisconnect = nullptr;
static HWND g_btnFlash = nullptr;
static HWND g_btnVerify = nullptr;
static HWND g_btnHwReset = nullptr;
static HWND g_btnSwdReset = nullptr;
static HWND g_btnErase = nullptr;
static HWND g_btnOpenDebug = nullptr;

static HWND g_edtFilePath = nullptr;
static HWND g_btnBrowse = nullptr;
static HWND g_edtBaseAddr = nullptr;

static HWND g_edtTargetInfo = nullptr;
static HWND g_edtLog = nullptr;
static HWND g_progress = nullptr;
static HWND g_btnClearLog = nullptr;

// Debug Window controls
static HWND g_dbgBtnStart = nullptr, g_dbgBtnExit = nullptr;
static HWND g_dbgBtnHalt = nullptr, g_dbgBtnRun = nullptr;
static HWND g_dbgBtnStepInto = nullptr, g_dbgBtnStepOut = nullptr, g_dbgBtnRegs = nullptr;
static HWND g_dbgText = nullptr;

// Hex Viewer controls
static HWND g_hexAddr = nullptr, g_hexLen = nullptr, g_hexBtnRead = nullptr, g_hexText = nullptr;
static HWND g_m32Addr = nullptr, g_m32Val = nullptr, g_m32BtnRead = nullptr, g_m32BtnWrite = nullptr;

// UART controls
static HWND g_uartComCmb = nullptr, g_uartBaudCmb = nullptr, g_uartBtnConn = nullptr;
static HWND g_uartChkEcho = nullptr, g_uartChkCr = nullptr, g_uartChkLf = nullptr;
static HWND g_uartChkTab = nullptr, g_uartChkEsc = nullptr;
static HWND g_uartLog = nullptr, g_uartInput = nullptr, g_uartBtnSend = nullptr;
static WNDPROC g_origUartInputProc = nullptr;

// OpenOCD config controls
static HWND g_ocdPathEdt = nullptr;

// Runtime State
static std::vector<HidDeviceInfo> g_hidDevices;
static std::mutex g_hidMutex;
static bool g_probePresent = false;
static bool g_targetConnected = false;
static bool g_debugActive = false;
static std::atomic<bool> g_busy{false};

static std::wstring g_ocdExePath = L"openocd";
static OpenOcdRunner g_ocdRunner;
static SerialPort g_serial;

struct OcdTargetOption {
    const wchar_t* label;
    const wchar_t* cfg;
};

static const OcdTargetOption kOcdTargets[] = {
    {L"STM32F4 (stm32f4x.cfg)", L"target/stm32f4x.cfg"},
    {L"STM32F1 (stm32f1x.cfg)", L"target/stm32f1x.cfg"},
    {L"STM32F3 (stm32f3x.cfg)", L"target/stm32f3x.cfg"},
    {L"STM32G4 (stm32g4x.cfg)", L"target/stm32g4x.cfg"},
    {L"STM32L4 (stm32l4x.cfg)", L"target/stm32l4x.cfg"},
    {L"STM32F0 (stm32f0x.cfg)", L"target/stm32f0x.cfg"},
    {L"STM32G0 (stm32g0x.cfg)", L"target/stm32g0x.cfg"},
    {L"STM32H7 (stm32h7x.cfg)", L"target/stm32h7x.cfg"},
};

struct SpeedOption {
    const wchar_t* label;
    uint32_t hz;
};

static const SpeedOption kSwdSpeeds[] = {
    {L"4 MHz",   4000000},
    {L"2 MHz",   2000000},
    {L"1.8 MHz", 1800000},
    {L"1 MHz",   1000000},
    {L"500 kHz", 500000},
    {L"200 kHz", 200000},
    {L"100 kHz", 100000},
    {L"50 kHz",  50000},
    {L"20 kHz",  20000},
};
static constexpr int kDefaultSpeedIdx = 3; // 1 MHz

// ---------------------------------------------------------------------------
// String & Formatting Utilities
// ---------------------------------------------------------------------------
static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring normalizeCrLf(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size() + 16);
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == L'\n' && (i == 0 || in[i - 1] != L'\r')) {
            out += L"\r\n";
        } else {
            out += in[i];
        }
    }
    return out;
}

static std::wstring getWinText(HWND h) {
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return L"";
    std::wstring s(len + 1, L'\0');
    GetWindowTextW(h, &s[0], len + 1);
    s.resize(len);
    return s;
}

static uint32_t parseHexOrDec(const std::wstring& s, uint32_t defVal) {
    if (s.empty()) return defVal;
    try {
        size_t idx = 0;
        return static_cast<uint32_t>(std::stoul(s, &idx, 0));
    } catch (...) {
        return defVal;
    }
}

static void postLog(const std::string& msg) {
    if (!g_hMainWnd) return;
    auto* p = new std::string(msg);
    if (!PostMessageW(g_hMainWnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(p))) {
        delete p;
    }
}

struct DapBusyGuard {
    CmsisDapClient& dap;
    bool active;
    DapBusyGuard(CmsisDapClient& d, bool skip) : dap(d), active(!skip) {
        if (active) {
            try { dap.hostStatus(1, 1); } catch (...) {}
        }
    }
    ~DapBusyGuard() {
        if (active) {
            try { dap.hostStatus(1, 0); } catch (...) {}
        }
    }
};

static void appendEditText(HWND hEdit, const std::wstring& text) {
    if (!hEdit) return;
    int len = GetWindowTextLengthW(hEdit);
    if (len > 120000) {
        SetWindowTextW(hEdit, L"");
        len = 0;
    }
    SendMessageW(hEdit, EM_SETSEL, len, len);
    SendMessageW(hEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(hEdit, EM_SCROLLCARET, 0, 0);
}

static std::wstring formatHexDump(uint32_t baseAddr, const std::vector<uint8_t>& data) {
    std::wstring out;
    out.reserve(data.size() * 5);
    wchar_t lineBuf[128];
    for (size_t off = 0; off < data.size(); off += 16) {
        size_t n = (std::min)(size_t(16), data.size() - off);
        std::wstring hexPart;
        std::wstring ascPart;
        for (size_t i = 0; i < 16; ++i) {
            if (i < n) {
                uint8_t b = data[off + i];
                wchar_t hb[4];
                std::swprintf(hb, 4, L"%02X ", b);
                hexPart += hb;
                ascPart += (b >= 32 && b < 127) ? static_cast<wchar_t>(b) : L'.';
            } else {
                hexPart += L"   ";
            }
            if (i == 7) hexPart += L" ";
        }
        std::swprintf(lineBuf, 128, L"0x%08X: %ls |%ls|\r\n",
                      baseAddr + static_cast<uint32_t>(off), hexPart.c_str(), ascPart.c_str());
        out += lineBuf;
    }
    return out;
}

static bool useOpenOcd() {
    return SendMessageW(g_chkUseOcd, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static bool useUnderReset() {
    return SendMessageW(g_chkUnderReset, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static uint32_t getSwdSpeedHz() {
    int idx = static_cast<int>(SendMessageW(g_cmbSwdSpeed, CB_GETCURSEL, 0, 0));
    if (idx < 0 || idx >= static_cast<int>(sizeof(kSwdSpeeds) / sizeof(kSwdSpeeds[0]))) {
        return 1000000;
    }
    return kSwdSpeeds[idx].hz;
}

static std::wstring getSelectedOcdTargetCfg() {
    int idx = static_cast<int>(SendMessageW(g_cmbOcdTarget, CB_GETCURSEL, 0, 0));
    if (idx < 0 || idx >= static_cast<int>(sizeof(kOcdTargets) / sizeof(kOcdTargets[0]))) {
        return L"target/stm32f4x.cfg";
    }
    return kOcdTargets[idx].cfg;
}

static void updateActionButtonsState() {
    bool canOperate = g_targetConnected && !g_busy.load();
    bool notBusy = !g_busy.load();
    bool ocd = useOpenOcd();

    EnableWindow(g_btnConnect, notBusy && !g_targetConnected && (g_probePresent || ocd));
    EnableWindow(g_btnDisconnect, canOperate);
    EnableWindow(g_btnFlash, canOperate);
    EnableWindow(g_btnVerify, canOperate);
    EnableWindow(g_btnHwReset, canOperate);
    EnableWindow(g_btnSwdReset, canOperate);
    EnableWindow(g_btnErase, canOperate);
    EnableWindow(g_btnOpenDebug, canOperate);
    EnableWindow(g_cmbOcdTarget, ocd && notBusy);

    if (g_hDebugWnd) {
        bool dbgOp = canOperate && g_debugActive;
        EnableWindow(g_dbgBtnStart, canOperate && !g_debugActive);
        EnableWindow(g_dbgBtnExit, dbgOp);
        EnableWindow(g_dbgBtnHalt, dbgOp);
        EnableWindow(g_dbgBtnRun, dbgOp);
        EnableWindow(g_dbgBtnStepInto, dbgOp && !ocd);
        EnableWindow(g_dbgBtnStepOut, dbgOp && !ocd);
        EnableWindow(g_dbgBtnRegs, dbgOp && !ocd);
    }
}

static void refreshUsbAutoDetect(bool logChange = true) {
    auto devs = WinHidDevice::enumerateDapDevices();
    auto coms = SerialPort::enumeratePorts();

    std::wstring comStr;
    for (const auto& c : coms) {
        if (c.friendlyName.find(L"USB Serial") != std::wstring::npos) {
            comStr = c.portName;
            break;
        }
    }

    bool wasPresent = g_probePresent;
    {
        std::lock_guard<std::mutex> lk(g_hidMutex);
        g_hidDevices = devs;
        g_probePresent = !devs.empty();
    }

    std::wstring badge;
    if (g_probePresent) {
        const auto& d = devs[0];
        wchar_t buf[256];
        if (!comStr.empty()) {
            std::swprintf(buf, 256, L"USB: Connected - %ls (%04X:%04X) | %ls",
                          d.product.c_str(), d.vid, d.pid, comStr.c_str());
        } else {
            std::swprintf(buf, 256, L"USB: Connected - %ls (%04X:%04X)",
                          d.product.c_str(), d.vid, d.pid);
        }
        badge = buf;
        if (logChange && !wasPresent) {
            postLog("[USB] Device detected: " + wideToUtf8(d.product) +
                    (comStr.empty() ? "" : (" | " + wideToUtf8(comStr))));
        }
    } else {
        badge = L"USB: Waiting for CMSIS-DAP probe...";
        if (g_targetConnected) {
            g_targetConnected = false;
            g_debugActive = false;
            SetWindowTextW(g_edtTargetInfo, L"Disconnected.\r\nPlug in the CMSIS-DAP probe and click [Connect].");
            if (g_dbgText) SetWindowTextW(g_dbgText, L"Debug Session: Inactive.");
        }
        if (logChange && wasPresent) {
            postLog("[USB] Programmer disconnected.");
        }
    }
    SetWindowTextW(g_lblUsbStatus, badge.c_str());
    updateActionButtonsState();
}

static HidDeviceInfo getSelectedHidInfo() {
    std::lock_guard<std::mutex> lk(g_hidMutex);
    if (g_hidDevices.empty()) {
        throw std::runtime_error("No CMSIS-DAP probe detected");
    }
    return g_hidDevices[0];
}

static void runBgTask(std::function<void()> fn) {
    if (g_busy.exchange(true)) {
        postLog("[WARN] Another operation is currently running...");
        return;
    }
    updateActionButtonsState();
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);

    std::thread([fn]() {
        try {
            fn();
        } catch (const std::exception& ex) {
            postLog(std::string("[ERROR] ") + ex.what());
        }
        PostMessageW(g_hMainWnd, WM_APP_OP_DONE, 0, 0);
    }).detach();
}

static std::wstring formatTargetInfoText(const Stm32TargetInfo& info) {
    wchar_t buf[1024];
    std::wstring rdpStr = L"N/A";
    if (info.rdp == 0xAA) rdpStr = L"Level 0 (Unlocked)";
    else if (info.rdp >= 0) {
        wchar_t rb[32];
        std::swprintf(rb, 32, L"Protected (0x%02X)", info.rdp);
        rdpStr = rb;
    }

    std::swprintf(
        buf, 1024,
        L"Chip: %ls  |  DEV_ID: 0x%03X (Rev 0x%04X)  |  Family: %ls\r\n"
        L"DP_IDCODE: 0x%08X  |  DBGMCU: 0x%08X  |  Flash: %u KB (Sector/Page: %u B)  |  RDP: %ls\r\n"
        L"UID: %ls\r\n",
        utf8ToWide(info.name).c_str(), info.devId, info.revId, utf8ToWide(info.family).c_str(),
        info.dpId, info.dbgId, info.flashKB, info.pageSize, rdpStr.c_str(),
        utf8ToWide(info.uid).c_str());

    std::wstring out = buf;
    if (!info.flashPreview.empty() && info.flashPreview.size() >= 8) {
        uint32_t sp = info.flashPreview[0] | (info.flashPreview[1] << 8) |
                      (info.flashPreview[2] << 16) | (info.flashPreview[3] << 24);
        uint32_t resetVec = info.flashPreview[4] | (info.flashPreview[5] << 8) |
                            (info.flashPreview[6] << 16) | (info.flashPreview[7] << 24);
        bool isBlank = std::all_of(info.flashPreview.begin(), info.flashPreview.end(),
                                   [](uint8_t b) { return b == 0xFF; });

        wchar_t vbuf[256];
        std::swprintf(
            vbuf, 256,
            L"--- On-Chip Flash Preview (%zu bytes @ 0x08000000) ---\r\n"
            L"Flash Status: %ls  |  Initial SP: 0x%08X  |  Reset_Handler: 0x%08X\r\n\r\n",
            info.flashPreview.size(),
            isBlank ? L"BLANK (0xFF)" : L"PROGRAMMED",
            sp, resetVec);
        out += vbuf;
        out += formatHexDump(0x08000000, info.flashPreview);
    }
    return out;
}

static std::wstring formatDebugStatusText(const CpuDebugStatus& st) {
    wchar_t buf[1024];
    std::swprintf(
        buf, 1024,
        L"Debug Session: ACTIVE   |   CPU State: %ls\r\n"
        L"-------------------------------------------------------------\r\n"
        L"PC   = 0x%08X     SP   = 0x%08X     LR   = 0x%08X\r\n"
        L"xPSR = 0x%08X     MSP  = 0x%08X     PSP  = 0x%08X\r\n"
        L"R0   = 0x%08X     R1   = 0x%08X     R2   = 0x%08X\r\n"
        L"R3   = 0x%08X     R12  = 0x%08X\r\n"
        L"CFSR = 0x%08X     HFSR = 0x%08X\r\n",
        st.halted ? L"HALTED (Paused)" : L"RUNNING",
        st.pc, st.sp, st.lr,
        st.xpsr, st.msp, st.psp,
        st.r0, st.r1, st.r2,
        st.r3, st.r12,
        st.cfsr, st.hfsr);
    return buf;
}

// ---------------------------------------------------------------------------
// Main Actions (Connect, Disconnect, Flash, Verify, HW/SWD Reset, Erase)
// ---------------------------------------------------------------------------
static void doConnect() {
    uint32_t speed = getSwdSpeedHz();
    bool underReset = useUnderReset();
    bool ocd = useOpenOcd();
    std::wstring ocdExe = g_ocdExePath;
    std::wstring ocdCfg = getSelectedOcdTargetCfg();

    runBgTask([=]() {
        if (ocd) {
            postLog("[OCD] Connecting to target via OpenOCD...");
            int rc = OpenOcdRunner::runOnce(ocdExe, ocdCfg, speed / 1000,
                                            {L"init", L"targets", L"exit"},
                                            [](const std::string& l) { postLog(l); });
            if (rc == 0) {
                auto* txt = new std::wstring(
                    L"Mode:          OpenOCD (interface/cmsis-dap.cfg)\r\n"
                    L"Target Config: " + ocdCfg + L"\r\n"
                    L"Status:        Connected OK. Ready to Flash / Reset / Erase / GDB Debug.");
                PostMessageW(g_hMainWnd, WM_APP_CONNECT_DONE, 1, reinterpret_cast<LPARAM>(txt));
            } else {
                PostMessageW(g_hMainWnd, WM_APP_CONNECT_DONE, 0, 0);
            }
            return;
        }

        HidDeviceInfo devInfo = getSelectedHidInfo();
        WinHidDevice hid;
        std::string err;
        if (!hid.open(devInfo, err)) throw std::runtime_error(err);

        CmsisDapClient dap(hid);
        postLog("[SWD] Connecting and reading Chip ID + 512B Flash @ 0x08000000...");
        Stm32TargetInfo info = dap.probeTarget(speed, underReset, true);
        try { dap.hostStatus(0, true); } catch (...) {}
        postLog("[SWD] Connected: " + info.name + " | Flash=" + std::to_string(info.flashKB) + " KB");

        auto* txt = new std::wstring(formatTargetInfoText(info));
        PostMessageW(g_hMainWnd, WM_APP_CONNECT_DONE, 1, reinterpret_cast<LPARAM>(txt));
    });
}

static void doDisconnect() {
    uint32_t speed = getSwdSpeedHz();
    bool wasDebug = g_debugActive;
    bool ocd = useOpenOcd();

    runBgTask([=]() {
        if (!ocd && g_probePresent) {
            try {
                HidDeviceInfo devInfo = getSelectedHidInfo();
                WinHidDevice hid;
                std::string err;
                if (hid.open(devInfo, err)) {
                    CmsisDapClient dap(hid);
                    if (wasDebug) {
                        dap.debugExit(speed);
                    } else {
                        dap.disconnect();
                    }
                    try { dap.hostStatus(0, false); } catch (...) {}
                }
            } catch (...) {}
        }
        postLog("[SWD] Disconnected from target.");
        PostMessageW(g_hMainWnd, WM_APP_CONNECT_DONE, 2, 0);
    });
}

static void doFlash() {
    std::wstring fwPath = getWinText(g_edtFilePath);
    if (fwPath.empty()) {
        MessageBoxW(g_hMainWnd, L"Please select a firmware file (.bin / .hex / .elf) first.",
                    L"No Firmware File", MB_ICONWARNING);
        return;
    }
    uint32_t binAddr = parseHexOrDec(getWinText(g_edtBaseAddr), 0x08000000);
    uint32_t speed = getSwdSpeedHz();
    bool underReset = useUnderReset();
    bool ocd = useOpenOcd();
    std::wstring ocdExe = g_ocdExePath;
    std::wstring ocdCfg = getSelectedOcdTargetCfg();

    runBgTask([=]() {
        if (ocd) {
            postLog("[OCD] Flashing firmware via OpenOCD...");
            std::wstring p = fwPath;
            std::replace(p.begin(), p.end(), L'\\', L'/');
            std::wstring ext;
            size_t dot = p.find_last_of(L'.');
            if (dot != std::wstring::npos) {
                ext = p.substr(dot);
                for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
            }
            std::wstring cmd = (ext == L".bin")
                ? (L"program \"" + p + L"\" 0x08000000 verify reset exit")
                : (L"program \"" + p + L"\" verify reset exit");
            int rc = OpenOcdRunner::runOnce(ocdExe, ocdCfg, speed / 1000, {cmd},
                                            [](const std::string& l) { postLog(l); });
            if (rc == 0) {
                PostMessageW(g_hMainWnd, WM_APP_PROGRESS, 100, 0);
                postLog("[OK] OpenOCD Flash & Verify succeeded!");
            } else {
                postLog("[ERROR] OpenOCD Flash failed (exit=" + std::to_string(rc) + ")");
            }
            return;
        }

        FirmwareImage img = loadFirmwareImage(fwPath, binAddr);
        char addrBuf[32];
        std::snprintf(addrBuf, sizeof(addrBuf), "0x%08X", img.baseAddr);
        postLog("[FLASH] Programming " + std::to_string(img.data.size()) + " bytes @ " + addrBuf);

        HidDeviceInfo devInfo = getSelectedHidInfo();
        WinHidDevice hid;
        std::string err;
        if (!hid.open(devInfo, err)) throw std::runtime_error(err);

        CmsisDapClient dap(hid);
        if (g_debugActive) {
            try { dap.hostStatus(2, 0); } catch (...) {}
            auto* dbgTxt = new std::wstring(L"Debug Session: Inactive (Flash operation started).");
            PostMessageW(g_hMainWnd, WM_APP_DEBUG_INFO, 2, reinterpret_cast<LPARAM>(dbgTxt));
        }
        DapBusyGuard busy(dap, false);
        ULONGLONG t0 = GetTickCount64();
        Stm32TargetInfo info = dap.flashImage(
            img.baseAddr, img.data, speed, underReset, true,
            [](const std::string& stage, uint32_t done, uint32_t total) {
                int pct = 0;
                if (total > 0) {
                    if (stage == "erase") pct = static_cast<int>(15 * done / total);
                    else if (stage == "program") pct = 15 + static_cast<int>(65 * done / total);
                    else pct = 80 + static_cast<int>(20 * done / total);
                }
                PostMessageW(g_hMainWnd, WM_APP_PROGRESS, pct, 0);
            });
        double dt = (GetTickCount64() - t0) / 1000.0;
        char msg[160];
        std::snprintf(msg, sizeof(msg), "[OK] Flashed & verified %zu bytes on %s in %.2fs!",
                      img.data.size(), info.name.c_str(), dt);
        postLog(msg);

        Stm32TargetInfo refreshed = dap.probeTarget(speed, false, true);
        try { dap.hostStatus(0, true); } catch (...) {}
        auto* txt = new std::wstring(formatTargetInfoText(refreshed));
        PostMessageW(g_hMainWnd, WM_APP_CONNECT_DONE, 1, reinterpret_cast<LPARAM>(txt));
    });
}

static void doVerify() {
    std::wstring fwPath = getWinText(g_edtFilePath);
    if (fwPath.empty()) {
        MessageBoxW(g_hMainWnd, L"Please select a firmware file to verify.", L"No Firmware File", MB_ICONWARNING);
        return;
    }
    uint32_t binAddr = parseHexOrDec(getWinText(g_edtBaseAddr), 0x08000000);
    uint32_t speed = getSwdSpeedHz();
    bool underReset = useUnderReset();

    runBgTask([=]() {
        FirmwareImage img = loadFirmwareImage(fwPath, binAddr);
        HidDeviceInfo devInfo = getSelectedHidInfo();
        WinHidDevice hid;
        std::string err;
        if (!hid.open(devInfo, err)) throw std::runtime_error(err);

        CmsisDapClient dap(hid);
        DapBusyGuard busy(dap, false);
        auto res = dap.verifyImage(img.baseAddr, img.data, speed, underReset,
                                   [](const std::string&, uint32_t done, uint32_t total) {
                                       int pct = total ? static_cast<int>(100 * done / total) : 100;
                                       PostMessageW(g_hMainWnd, WM_APP_PROGRESS, pct, 0);
                                   });
        try { dap.hostStatus(0, true); } catch (...) {}
        if (res.mismatches == 0) {
            postLog("[OK] Verify PASSED 100% (" + std::to_string(img.data.size()) + " bytes) on " + res.info.name);
        } else {
            char msg[160];
            std::snprintf(msg, sizeof(msg), "[FAIL] Verify mismatch: %u words! First @ 0x%08X (File=0x%08X, Chip=0x%08X)",
                          res.mismatches, res.firstBadAddr, res.firstWant, res.firstGot);
            postLog(msg);
        }
    });
}

static void doHwReset() {
    uint32_t speed = getSwdSpeedHz();
    bool ocd = useOpenOcd();
    bool dbg = g_debugActive;
    std::wstring ocdExe = g_ocdExePath;
    std::wstring ocdCfg = getSelectedOcdTargetCfg();

    runBgTask([=]() {
        if (ocd) {
            OpenOcdRunner::runOnce(ocdExe, ocdCfg, speed / 1000,
                                   {L"init", L"reset run", L"exit"},
                                   [](const std::string& l) { postLog(l); });
            return;
        }
        HidDeviceInfo devInfo = getSelectedHidInfo();
        WinHidDevice hid;
        std::string err;
        if (!hid.open(devInfo, err)) throw std::runtime_error(err);
        CmsisDapClient dap(hid);
        DapBusyGuard busy(dap, dbg);
        dap.hardResetPulse(50);
        postLog("[HW RESET] Asserted nRESET pin (PB8) LOW 50ms -> HIGH.");
    });
}

static void doSwdReset() {
    uint32_t speed = getSwdSpeedHz();
    bool ocd = useOpenOcd();
    bool dbg = g_debugActive;
    bool conn = g_targetConnected;
    std::wstring ocdExe = g_ocdExePath;
    std::wstring ocdCfg = getSelectedOcdTargetCfg();

    runBgTask([=]() {
        if (ocd) {
            OpenOcdRunner::runOnce(ocdExe, ocdCfg, speed / 1000,
                                   {L"init", L"reset_config none", L"reset run", L"exit"},
                                   [](const std::string& l) { postLog(l); });
            return;
        }
        HidDeviceInfo devInfo = getSelectedHidInfo();
        WinHidDevice hid;
        std::string err;
        if (!hid.open(devInfo, err)) throw std::runtime_error(err);
        CmsisDapClient dap(hid);
        DapBusyGuard busy(dap, dbg);
        dap.swdSystemReset(speed);
        if (conn && !dbg) {
            try { dap.hostStatus(0, true); } catch (...) {}
        }
        postLog("[SWD RESET] Sent AIRCR.SYSRESETREQ over SWD.");
    });
}

static void doErase() {
    if (MessageBoxW(g_hMainWnd, L"Erase entire target Flash (Mass Erase)?",
                    L"Confirm Mass Erase", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    uint32_t speed = getSwdSpeedHz();
    bool underReset = useUnderReset();
    bool ocd = useOpenOcd();
    std::wstring ocdExe = g_ocdExePath;
    std::wstring ocdCfg = getSelectedOcdTargetCfg();

    runBgTask([=]() {
        if (ocd) {
            int rc = OpenOcdRunner::runOnce(ocdExe, ocdCfg, speed / 1000,
                                            {L"init", L"halt", L"flash erase_sector 0 0 last", L"exit"},
                                            [](const std::string& l) { postLog(l); });
            postLog(rc == 0 ? "[OCD] Mass Erase completed!" : "[OCD] Mass Erase failed!");
            return;
        }
        HidDeviceInfo devInfo = getSelectedHidInfo();
        WinHidDevice hid;
        std::string err;
        if (!hid.open(devInfo, err)) throw std::runtime_error(err);
        CmsisDapClient dap(hid);
        if (g_debugActive) {
            try { dap.hostStatus(2, 0); } catch (...) {}
            auto* dbgTxt = new std::wstring(L"Debug Session: Inactive (Mass Erase started).");
            PostMessageW(g_hMainWnd, WM_APP_DEBUG_INFO, 2, reinterpret_cast<LPARAM>(dbgTxt));
        }
        DapBusyGuard busy(dap, false);
        postLog("[ERASE] Erasing target Flash...");
        Stm32TargetInfo info = dap.massErase(speed, underReset);
        postLog("[OK] Mass Erase completed on " + info.name);

        Stm32TargetInfo refreshed = dap.probeTarget(speed, false, true);
        try { dap.hostStatus(0, true); } catch (...) {}
        auto* txt = new std::wstring(formatTargetInfoText(refreshed));
        PostMessageW(g_hMainWnd, WM_APP_CONNECT_DONE, 1, reinterpret_cast<LPARAM>(txt));
    });
}

// ---------------------------------------------------------------------------
// Debug Actions (for dedicated Debug Window)
// ---------------------------------------------------------------------------
static void doDebugStart() {
    uint32_t speed = getSwdSpeedHz();
    bool underReset = useUnderReset();
    runBgTask([=]() {
        HidDeviceInfo devInfo = getSelectedHidInfo();
        WinHidDevice hid;
        std::string err;
        if (!hid.open(devInfo, err)) throw std::runtime_error(err);
        CmsisDapClient dap(hid);
        CpuDebugStatus st = dap.debugStart(speed, underReset);
        try { dap.hostStatus(2, 1); } catch (...) {}
        auto* dbgTxt = new std::wstring(formatDebugStatusText(st));
        PostMessageW(g_hMainWnd, WM_APP_DEBUG_INFO, 1, reinterpret_cast<LPARAM>(dbgTxt));
        postLog("[DEBUG] Debug session started (CPU halted @ PC=0x" +
                [&]() { char b[16]; std::snprintf(b, sizeof(b), "%08X", st.pc); return std::string(b); }() + ")");
    });
}

static void doDebugExit() {
    uint32_t speed = getSwdSpeedHz();
    runBgTask([=]() {
        HidDeviceInfo devInfo = getSelectedHidInfo();
        WinHidDevice hid;
        std::string err;
        if (!hid.open(devInfo, err)) throw std::runtime_error(err);
        CmsisDapClient dap(hid);
        dap.debugExit(speed);
        try { dap.hostStatus(2, 0); dap.hostStatus(0, true); } catch (...) {}
        auto* dbgTxt = new std::wstring(L"Debug Session: EXITED (CPU running freely)\r\nClick [Start Debug] to attach and halt CPU.");
        PostMessageW(g_hMainWnd, WM_APP_DEBUG_INFO, 2, reinterpret_cast<LPARAM>(dbgTxt));
        postLog("[DEBUG] Exited debug mode; CPU running freely.");
    });
}

static void doCpuAction(const std::string& action) {
    uint32_t speed = getSwdSpeedHz();
    bool ocd = useOpenOcd();
    std::wstring ocdExe = g_ocdExePath;
    std::wstring ocdCfg = getSelectedOcdTargetCfg();

    runBgTask([=]() {
        if (ocd) {
            std::wstring cmd = (action == "halt") ? L"halt" : L"resume";
            OpenOcdRunner::runOnce(ocdExe, ocdCfg, speed / 1000,
                                   {L"init", cmd, L"exit"},
                                   [](const std::string& l) { postLog(l); });
            return;
        }
        HidDeviceInfo devInfo = getSelectedHidInfo();
        WinHidDevice hid;
        std::string err;
        if (!hid.open(devInfo, err)) throw std::runtime_error(err);
        CmsisDapClient dap(hid);
        std::string res = dap.cpuControl(action, speed);
        try { dap.hostStatus(2, 2); } catch (...) {}
        postLog("[DEBUG] " + res);
        if (action == "halt" || action == "step_into" || action == "step_out") {
            CpuDebugStatus st = dap.readCpuStatus(speed, false);
            auto* dbgTxt = new std::wstring(formatDebugStatusText(st));
            PostMessageW(g_hMainWnd, WM_APP_DEBUG_INFO, 1, reinterpret_cast<LPARAM>(dbgTxt));
        } else if (action == "run") {
            auto* dbgTxt = new std::wstring(L"Debug Session: ACTIVE   |   CPU State: RUNNING\r\nClick [Halt] or [Read Regs] to pause and inspect registers.");
            PostMessageW(g_hMainWnd, WM_APP_DEBUG_INFO, 1, reinterpret_cast<LPARAM>(dbgTxt));
        }
    });
}

static void doReadRegs() {
    uint32_t speed = getSwdSpeedHz();
    bool underReset = useUnderReset();

    runBgTask([=]() {
        HidDeviceInfo devInfo = getSelectedHidInfo();
        WinHidDevice hid;
        std::string err;
        if (!hid.open(devInfo, err)) throw std::runtime_error(err);
        CmsisDapClient dap(hid);
        CpuDebugStatus st = dap.readCpuStatus(speed, underReset);
        try { dap.hostStatus(2, 2); } catch (...) {}
        auto* dbgTxt = new std::wstring(formatDebugStatusText(st));
        PostMessageW(g_hMainWnd, WM_APP_DEBUG_INFO, 1, reinterpret_cast<LPARAM>(dbgTxt));
        char msg[96];
        std::snprintf(msg, sizeof(msg), "[DEBUG] PC=0x%08X SP=0x%08X LR=0x%08X xPSR=0x%08X",
                      st.pc, st.sp, st.lr, st.xpsr);
        postLog(msg);
    });
}

static void doBrowseFirmware() {
    wchar_t fileBuf[MAX_PATH] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"Firmware (*.bin;*.hex;*.elf)\0*.bin;*.hex;*.elf\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(g_edtFilePath, fileBuf);
        postLog("[FILE] Selected: " + wideToUtf8(fileBuf));
    }
}

static void doDumpFlashBin() {
    wchar_t fileBuf[MAX_PATH] = L"flash_dump.bin";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"Binary (*.bin)\0*.bin\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"bin";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;

    std::wstring outPath = fileBuf;
    uint32_t speed = getSwdSpeedHz();
    bool underReset = useUnderReset();
    bool dbg = g_debugActive;

    runBgTask([=]() {
        HidDeviceInfo devInfo = getSelectedHidInfo();
        WinHidDevice hid;
        std::string err;
        if (!hid.open(devInfo, err)) throw std::runtime_error(err);
        CmsisDapClient dap(hid);
        DapBusyGuard busy(dap, dbg);
        Stm32TargetInfo info = dap.probeTarget(speed, underReset, false);
        uint32_t len = (info.flashKB > 0) ? (info.flashKB * 1024U) : (64U * 1024U);
        postLog("[DUMP] Reading " + std::to_string(len / 1024) + " KB Flash from " + info.name + "...");
        auto bytes = dap.readMemoryBytes(0x08000000, len, speed, underReset,
                                         [](const std::string&, uint32_t done, uint32_t total) {
                                             int pct = total ? static_cast<int>(100 * done / total) : 100;
                                             PostMessageW(g_hMainWnd, WM_APP_PROGRESS, pct, 0);
                                         });
        std::ofstream f(outPath.c_str(), std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!dbg) { try { dap.hostStatus(0, true); } catch (...) {} }
        postLog("[OK] Saved Flash dump (" + std::to_string(bytes.size()) + " bytes).");
    });
}

// ---------------------------------------------------------------------------
// Tool Window 0: Dedicated Debug Window (Fixed Size)
// ---------------------------------------------------------------------------
static LRESULT CALLBACK DebugWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        int x = 12;
        g_dbgBtnStart = CreateWindowW(L"BUTTON", L"Start Debug", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                      x, 10, 94, 28, hwnd, reinterpret_cast<HMENU>(IDC_DBG_BTN_START), g_hInst, nullptr); x += 100;
        g_dbgBtnExit  = CreateWindowW(L"BUTTON", L"Exit Debug", WS_CHILD | WS_VISIBLE,
                                      x, 10, 86, 28, hwnd, reinterpret_cast<HMENU>(IDC_DBG_BTN_EXIT), g_hInst, nullptr); x += 96;
        g_dbgBtnHalt  = CreateWindowW(L"BUTTON", L"Halt", WS_CHILD | WS_VISIBLE,
                                      x, 10, 62, 28, hwnd, reinterpret_cast<HMENU>(IDC_DBG_BTN_HALT), g_hInst, nullptr); x += 68;
        g_dbgBtnRun   = CreateWindowW(L"BUTTON", L"Run", WS_CHILD | WS_VISIBLE,
                                      x, 10, 62, 28, hwnd, reinterpret_cast<HMENU>(IDC_DBG_BTN_RUN), g_hInst, nullptr); x += 68;
        g_dbgBtnStepInto = CreateWindowW(L"BUTTON", L"Step Into", WS_CHILD | WS_VISIBLE,
                                         x, 10, 82, 28, hwnd, reinterpret_cast<HMENU>(IDC_DBG_BTN_STEP_INTO), g_hInst, nullptr); x += 88;
        g_dbgBtnStepOut  = CreateWindowW(L"BUTTON", L"Step Out", WS_CHILD | WS_VISIBLE,
                                         x, 10, 82, 28, hwnd, reinterpret_cast<HMENU>(IDC_DBG_BTN_STEP_OUT), g_hInst, nullptr); x += 88;
        g_dbgBtnRegs     = CreateWindowW(L"BUTTON", L"Read Regs", WS_CHILD | WS_VISIBLE,
                                         x, 10, 86, 28, hwnd, reinterpret_cast<HMENU>(IDC_DBG_BTN_REGS), g_hInst, nullptr);

        g_dbgText = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT",
            L"Debug Session: Inactive.\r\nClick [Start Debug] to halt the CPU and inspect registers.",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            12, 46, 628, 340, hwnd, reinterpret_cast<HMENU>(IDC_DBG_TEXT), g_hInst, nullptr);

        EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontUi), TRUE);
            return TRUE;
        }, 0);
        SendMessageW(g_dbgText, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontMono), TRUE);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_DBG_BTN_START) doDebugStart();
        else if (id == IDC_DBG_BTN_EXIT) doDebugExit();
        else if (id == IDC_DBG_BTN_HALT) doCpuAction("halt");
        else if (id == IDC_DBG_BTN_RUN) doCpuAction("run");
        else if (id == IDC_DBG_BTN_STEP_INTO) doCpuAction("step_into");
        else if (id == IDC_DBG_BTN_STEP_OUT) doCpuAction("step_out");
        else if (id == IDC_DBG_BTN_REGS) doReadRegs();
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void openDebugWindow() {
    if (!g_hDebugWnd) {
        RECT rc{0, 0, 652, 398};
        AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
        g_hDebugWnd = CreateWindowExW(
            0, L"TueDebugWndClass", L"CPU Debug Session",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
            g_hMainWnd, nullptr, g_hInst, nullptr);
    }
    updateActionButtonsState();
    ShowWindow(g_hDebugWnd, SW_SHOW);
    SetForegroundWindow(g_hDebugWnd);
}

// ---------------------------------------------------------------------------
// Tool Window 1: Memory Hex Viewer & Mem32 (Fixed Size)
// ---------------------------------------------------------------------------
static LRESULT CALLBACK HexWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC", L"Address:", WS_CHILD | WS_VISIBLE, 12, 14, 58, 20, hwnd, nullptr, g_hInst, nullptr);
        g_hexAddr = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0x08000000", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    74, 11, 100, 24, hwnd, reinterpret_cast<HMENU>(IDC_HEX_ADDR), g_hInst, nullptr);
        CreateWindowW(L"STATIC", L"Bytes:", WS_CHILD | WS_VISIBLE, 184, 14, 44, 20, hwnd, nullptr, g_hInst, nullptr);
        g_hexLen = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"512", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   230, 11, 70, 24, hwnd, reinterpret_cast<HMENU>(IDC_HEX_LEN), g_hInst, nullptr);
        g_hexBtnRead = CreateWindowW(L"BUTTON", L"Read Hex", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     310, 10, 90, 26, hwnd, reinterpret_cast<HMENU>(IDC_HEX_BTN_READ), g_hInst, nullptr);

        CreateWindowW(L"STATIC", L"Mem32 Addr:", WS_CHILD | WS_VISIBLE, 12, 46, 80, 20, hwnd, nullptr, g_hInst, nullptr);
        g_m32Addr = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0xE0042000", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    96, 43, 100, 24, hwnd, reinterpret_cast<HMENU>(IDC_MEM32_ADDR), g_hInst, nullptr);
        CreateWindowW(L"STATIC", L"Value:", WS_CHILD | WS_VISIBLE, 206, 46, 44, 20, hwnd, nullptr, g_hInst, nullptr);
        g_m32Val = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0x00000000", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   252, 43, 105, 24, hwnd, reinterpret_cast<HMENU>(IDC_MEM32_VAL), g_hInst, nullptr);
        g_m32BtnRead = CreateWindowW(L"BUTTON", L"Read 32", WS_CHILD | WS_VISIBLE,
                                     366, 42, 80, 26, hwnd, reinterpret_cast<HMENU>(IDC_MEM32_BTN_READ), g_hInst, nullptr);
        g_m32BtnWrite = CreateWindowW(L"BUTTON", L"Write 32", WS_CHILD | WS_VISIBLE,
                                      452, 42, 80, 26, hwnd, reinterpret_cast<HMENU>(IDC_MEM32_BTN_WRITE), g_hInst, nullptr);

        g_hexText = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                    12, 78, 660, 360, hwnd, reinterpret_cast<HMENU>(IDC_HEX_TEXT), g_hInst, nullptr);

        EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontUi), TRUE);
            return TRUE;
        }, 0);
        SendMessageW(g_hexText, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontMono), TRUE);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_HEX_BTN_READ) {
            uint32_t addr = parseHexOrDec(getWinText(g_hexAddr), 0x08000000);
            uint32_t len = (std::min)(parseHexOrDec(getWinText(g_hexLen), 512), uint32_t(16384));
            uint32_t speed = getSwdSpeedHz();
            bool underReset = useUnderReset();
            bool dbg = g_debugActive;
            runBgTask([=]() {
                HidDeviceInfo devInfo = getSelectedHidInfo();
                WinHidDevice hid;
                std::string err;
                if (!hid.open(devInfo, err)) throw std::runtime_error(err);
                CmsisDapClient dap(hid);
                DapBusyGuard busy(dap, dbg);
                auto data = dap.readMemoryBytes(addr, len, speed, underReset);
                auto* txt = new std::wstring(formatHexDump(addr, data));
                PostMessageW(g_hMainWnd, WM_APP_HEX_DATA, 0, reinterpret_cast<LPARAM>(txt));
            });
        } else if (id == IDC_MEM32_BTN_READ) {
            uint32_t addr = parseHexOrDec(getWinText(g_m32Addr), 0xE0042000);
            uint32_t speed = getSwdSpeedHz();
            bool underReset = useUnderReset();
            bool dbg = g_debugActive;
            runBgTask([=]() {
                HidDeviceInfo devInfo = getSelectedHidInfo();
                WinHidDevice hid;
                std::string err;
                if (!hid.open(devInfo, err)) throw std::runtime_error(err);
                CmsisDapClient dap(hid);
                DapBusyGuard busy(dap, dbg);
                auto data = dap.readMemoryBytes(addr, 4, speed, underReset);
                uint32_t val = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
                PostMessageW(g_hMainWnd, WM_APP_MEM32_VAL, val, 0);
            });
        } else if (id == IDC_MEM32_BTN_WRITE) {
            uint32_t addr = parseHexOrDec(getWinText(g_m32Addr), 0xE0042000);
            uint32_t val  = parseHexOrDec(getWinText(g_m32Val), 0);
            uint32_t speed = getSwdSpeedHz();
            bool dbg = g_debugActive;
            runBgTask([=]() {
                HidDeviceInfo devInfo = getSelectedHidInfo();
                WinHidDevice hid;
                std::string err;
                if (!hid.open(devInfo, err)) throw std::runtime_error(err);
                CmsisDapClient dap(hid);
                DapBusyGuard busy(dap, dbg);
                if (dap.connectSwd() != 1) throw std::runtime_error("DAP_Connect failed");
                dap.swjClock(speed);
                dap.transferConfigure(2, 500, 0);
                dap.swdConfigure(0);
                dap.swdLineResetToSwd();
                dap.readDp(0x00);
                dap.writeAbort(0x1E);
                dap.writeDp(0x08, 0);
                dap.writeDp(0x04, 0x50000000);
                for (int i = 0; i < 30; ++i) {
                    if ((dap.readDp(0x04) & 0xA0000000) == 0xA0000000) break;
                    Sleep(2);
                }
                uint8_t c, a;
                std::vector<uint32_t> v;
                dap.transfer({{0x01, 0x23000012}}, c, a, v);
                dap.memWrite32(addr, val);
                dap.disconnect();
                char msg[96];
                std::snprintf(msg, sizeof(msg), "[MEM32] Wrote [0x%08X] = 0x%08X", addr, val);
                postLog(msg);
            });
        }
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Tool Window 2: UART Virtual COM Terminal
// ---------------------------------------------------------------------------
static void refreshUartComCombo() {
    if (!g_uartComCmb) return;
    SendMessageW(g_uartComCmb, CB_RESETCONTENT, 0, 0);
    auto ports = SerialPort::enumeratePorts();
    for (const auto& p : ports) {
        SendMessageW(g_uartComCmb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.friendlyName.c_str()));
    }
    if (!ports.empty()) {
        SendMessageW(g_uartComCmb, CB_SETCURSEL, 0, 0);
    }
}

static std::string decodeEscapeSequences(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            char next = in[i + 1];
            if (next == 'r')      { out.push_back('\r'); ++i; continue; }
            if (next == 'n')      { out.push_back('\n'); ++i; continue; }
            if (next == 't')      { out.push_back('\t'); ++i; continue; }
            if (next == '\\')     { out.push_back('\\'); ++i; continue; }
            if (next == '0')      { out.push_back('\0'); ++i; continue; }
            if (next == 'x' && i + 3 < in.size() &&
                std::isxdigit(static_cast<unsigned char>(in[i + 2])) &&
                std::isxdigit(static_cast<unsigned char>(in[i + 3]))) {
                auto hexVal = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    return c - 'A' + 10;
                };
                out.push_back(static_cast<char>((hexVal(in[i + 2]) << 4) | hexVal(in[i + 3])));
                i += 3;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

static void sendUartInput() {
    if (!g_serial.isOpen()) {
        appendEditText(g_uartLog, L"[WARN] COM port is not connected!\r\n");
        return;
    }
    std::wstring wtxt = getWinText(g_uartInput);
    SetWindowTextW(g_uartInput, L"");
    SetFocus(g_uartInput);

    auto isChecked = [](HWND h) {
        return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    };

    bool echoTx = isChecked(g_uartChkEcho);
    bool addTab = isChecked(g_uartChkTab);
    bool addCr  = isChecked(g_uartChkCr);
    bool addLf  = isChecked(g_uartChkLf);
    bool parseEsc = isChecked(g_uartChkEsc);

    std::string payload = wideToUtf8(wtxt);
    if (parseEsc) payload = decodeEscapeSequences(payload);
    if (addTab) payload.push_back('\t');
    if (addCr)  payload.push_back('\r');
    if (addLf)  payload.push_back('\n');

    if (payload.empty()) return;

    if (g_serial.writeText(payload)) {
        if (echoTx) {
            std::wstring suffix;
            if (addTab) suffix += L"\\t";
            if (addCr)  suffix += L"\\r";
            if (addLf)  suffix += L"\\n";
            std::wstring shown = wtxt.empty() ? suffix : (wtxt + (suffix.empty() ? L"" : L" [" + suffix + L"]"));
            appendEditText(g_uartLog, L"[TX] " + shown + L"\r\n");
        }
    } else {
        appendEditText(g_uartLog, L"[ERROR] Failed to write to COM port.\r\n");
    }
}

static LRESULT CALLBACK UartInputSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) { sendUartInput(); return 0; }
    if (msg == WM_CHAR && (wParam == L'\r' || wParam == L'\n')) return 0;
    return CallWindowProcW(g_origUartInputProc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK UartWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_uartComCmb = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                     12, 10, 224, 200, hwnd, reinterpret_cast<HMENU>(IDC_UART_COM_CMB), g_hInst, nullptr);
        g_uartBaudCmb = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                      244, 10, 84, 160, hwnd, reinterpret_cast<HMENU>(IDC_UART_BAUD_CMB), g_hInst, nullptr);
        SendMessageW(g_uartBaudCmb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"115200"));
        SendMessageW(g_uartBaudCmb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"9600"));
        SendMessageW(g_uartBaudCmb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"57600"));
        SendMessageW(g_uartBaudCmb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"921600"));
        SendMessageW(g_uartBaudCmb, CB_SETCURSEL, 0, 0);

        g_uartBtnConn = CreateWindowW(L"BUTTON", L"Connect COM", WS_CHILD | WS_VISIBLE,
                                      336, 9, 102, 26, hwnd, reinterpret_cast<HMENU>(IDC_UART_BTN_CONN), g_hInst, nullptr);
        CreateWindowW(L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE,
                      444, 9, 64, 26, hwnd, reinterpret_cast<HMENU>(IDC_UART_BTN_CLEAR), g_hInst, nullptr);
        g_uartChkEcho = CreateWindowW(L"BUTTON", L"Echo TX", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                      516, 11, 86, 22, hwnd, reinterpret_cast<HMENU>(IDC_UART_CHK_ECHO), g_hInst, nullptr);
        SendMessageW(g_uartChkEcho, BM_SETCHECK, BST_CHECKED, 0);

        CreateWindowW(L"STATIC", L"Append:", WS_CHILD | WS_VISIBLE, 12, 41, 52, 20, hwnd, nullptr, g_hInst, nullptr);
        g_uartChkCr = CreateWindowW(L"BUTTON", L"\\r (CR)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                    68, 39, 68, 22, hwnd, reinterpret_cast<HMENU>(IDC_UART_CHK_CR), g_hInst, nullptr);
        SendMessageW(g_uartChkCr, BM_SETCHECK, BST_CHECKED, 0);
        g_uartChkLf = CreateWindowW(L"BUTTON", L"\\n (LF)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                    142, 39, 68, 22, hwnd, reinterpret_cast<HMENU>(IDC_UART_CHK_LF), g_hInst, nullptr);
        SendMessageW(g_uartChkLf, BM_SETCHECK, BST_CHECKED, 0);
        g_uartChkTab = CreateWindowW(L"BUTTON", L"\\t (TAB)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     216, 39, 76, 22, hwnd, reinterpret_cast<HMENU>(IDC_UART_CHK_TAB), g_hInst, nullptr);
        g_uartChkEsc = CreateWindowW(L"BUTTON", L"Parse \\r \\n \\t \\xHH", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     306, 39, 165, 22, hwnd, reinterpret_cast<HMENU>(IDC_UART_CHK_ESC), g_hInst, nullptr);
        SendMessageW(g_uartChkEsc, BM_SETCHECK, BST_CHECKED, 0);

        g_uartLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                    12, 68, 620, 286, hwnd, reinterpret_cast<HMENU>(IDC_UART_LOG), g_hInst, nullptr);
        g_uartInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      12, 362, 520, 26, hwnd, reinterpret_cast<HMENU>(IDC_UART_INPUT), g_hInst, nullptr);
        g_uartBtnSend = CreateWindowW(L"BUTTON", L"Send", WS_CHILD | WS_VISIBLE,
                                      540, 361, 92, 28, hwnd, reinterpret_cast<HMENU>(IDC_UART_BTN_SEND), g_hInst, nullptr);

        g_origUartInputProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_uartInput, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(UartInputSubclassProc)));

        EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontUi), TRUE);
            return TRUE;
        }, 0);
        SendMessageW(g_uartLog, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontMono), TRUE);
        refreshUartComCombo();
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_UART_BTN_CONN) {
            if (g_serial.isOpen()) {
                g_serial.close();
                SetWindowTextW(g_uartBtnConn, L"Connect COM");
                appendEditText(g_uartLog, L"[COM] Disconnected.\r\n");
            } else {
                std::wstring sel = getWinText(g_uartComCmb);
                size_t p1 = sel.rfind(L"(COM");
                size_t p2 = sel.rfind(L')');
                if (p1 == std::wstring::npos || p2 == std::wstring::npos) {
                    MessageBoxW(hwnd, L"No valid COM port selected.", L"Error", MB_ICONWARNING);
                    return 0;
                }
                std::wstring port = sel.substr(p1 + 1, p2 - p1 - 1);
                uint32_t baud = parseHexOrDec(getWinText(g_uartBaudCmb), 115200);
                std::string err;
                if (g_serial.open(port, baud, err,
                                  [](const std::string& rx) {
                                      auto* p = new std::string(rx);
                                      PostMessageW(g_hMainWnd, WM_APP_UART_RX, 0, reinterpret_cast<LPARAM>(p));
                                  },
                                  []() {
                                      PostMessageW(g_hMainWnd, WM_APP_UART_CLOSED, 0, 0);
                                  })) {
                    SetWindowTextW(g_uartBtnConn, L"Disconnect");
                    appendEditText(g_uartLog, L"[COM] Opened " + port + L" @ " + std::to_wstring(baud) + L" bps\r\n");
                    SetFocus(g_uartInput);
                } else {
                    MessageBoxW(hwnd, utf8ToWide(err).c_str(), L"COM Error", MB_ICONERROR);
                }
            }
        } else if (id == IDC_UART_BTN_SEND) sendUartInput();
        else if (id == IDC_UART_BTN_CLEAR) SetWindowTextW(g_uartLog, L"");
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Tool Window 3: OpenOCD Settings Dialog
// ---------------------------------------------------------------------------
static LRESULT CALLBACK OcdDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC", L"openocd.exe path:", WS_CHILD | WS_VISIBLE,
                      12, 14, 150, 20, hwnd, nullptr, g_hInst, nullptr);
        g_ocdPathEdt = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_ocdExePath.c_str(),
                                       WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                       12, 38, 360, 24, hwnd, reinterpret_cast<HMENU>(IDC_OCD_PATH_EDT), g_hInst, nullptr);
        CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE,
                      380, 37, 75, 26, hwnd, reinterpret_cast<HMENU>(IDC_OCD_PATH_BROWSE), g_hInst, nullptr);
        CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                      295, 78, 75, 28, hwnd, reinterpret_cast<HMENU>(IDC_OCD_BTN_SAVE), g_hInst, nullptr);
        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                      380, 78, 75, 28, hwnd, reinterpret_cast<HMENU>(IDC_OCD_BTN_CANCEL), g_hInst, nullptr);

        EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontUi), TRUE);
            return TRUE;
        }, 0);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_OCD_PATH_BROWSE) {
            wchar_t buf[MAX_PATH] = {0};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"OpenOCD (openocd.exe)\0openocd.exe\0All (*.*)\0*.*\0";
            ofn.lpstrFile = buf;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) SetWindowTextW(g_ocdPathEdt, buf);
        } else if (id == IDC_OCD_BTN_SAVE) {
            g_ocdExePath = getWinText(g_ocdPathEdt);
            if (g_ocdExePath.empty()) g_ocdExePath = L"openocd";
            ShowWindow(hwnd, SW_HIDE);
        } else if (id == IDC_OCD_BTN_CANCEL) {
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Main Window Menu & Fixed Layout (900 x 600 client area)
// ---------------------------------------------------------------------------
static HMENU createMainMenu() {
    HMENU hMenu = CreateMenu();

    HMENU hFile = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, IDM_FILE_OPEN, L"Open Firmware (.bin/.hex/.elf)...\tCtrl+O");
    AppendMenuW(hFile, MF_STRING, IDM_FILE_DUMP_BIN, L"Dump Flash to .bin...");
    AppendMenuW(hFile, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT, L"Exit");
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hFile), L"&File");

    HMENU hTools = CreatePopupMenu();
    AppendMenuW(hTools, MF_STRING, IDM_TOOLS_DEBUG, L"CPU Debug Window...");
    AppendMenuW(hTools, MF_STRING, IDM_TOOLS_HEXVIEW, L"Memory Hex Viewer && Mem32...");
    AppendMenuW(hTools, MF_STRING, IDM_TOOLS_UART, L"UART Terminal (Virtual COM)...");
    AppendMenuW(hTools, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hTools, MF_STRING, IDM_TOOLS_OCD_CFG, L"OpenOCD Settings...");
    AppendMenuW(hTools, MF_STRING, IDM_TOOLS_GDB_SERVER, L"Toggle OpenOCD GDB Server (:3333)");
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hTools), L"&Tools");

    HMENU hHelp = CreatePopupMenu();
    AppendMenuW(hHelp, MF_STRING, IDM_HELP_LED, L"LED Status Legend");
    AppendMenuW(hHelp, MF_STRING, IDM_HELP_ABOUT, L"About");
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hHelp), L"&Help");

    return hMenu;
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_fontUi = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_fontUiBold = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_fontMono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

        // Row 1 (y = 10)
        g_lblUsbStatus = CreateWindowW(L"STATIC", L"USB: Scanning...", WS_CHILD | WS_VISIBLE,
                                       12, 13, 355, 20, hwnd, reinterpret_cast<HMENU>(IDC_LBL_USB_STATUS), g_hInst, nullptr);
        g_chkUseOcd = CreateWindowW(L"BUTTON", L"Use OpenOCD", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                    375, 11, 108, 22, hwnd, reinterpret_cast<HMENU>(IDC_CHK_USE_OCD), g_hInst, nullptr);
        g_cmbOcdTarget = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                       488, 10, 178, 220, hwnd, reinterpret_cast<HMENU>(IDC_CMB_OCD_TARGET), g_hInst, nullptr);
        for (const auto& t : kOcdTargets) SendMessageW(g_cmbOcdTarget, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.label));
        SendMessageW(g_cmbOcdTarget, CB_SETCURSEL, 0, 0);

        g_cmbSwdSpeed = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                      674, 10, 96, 220, hwnd, reinterpret_cast<HMENU>(IDC_CMB_SWD_SPEED), g_hInst, nullptr);
        for (const auto& s : kSwdSpeeds) SendMessageW(g_cmbSwdSpeed, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.label));
        SendMessageW(g_cmbSwdSpeed, CB_SETCURSEL, kDefaultSpeedIdx, 0);

        g_chkUnderReset = CreateWindowW(L"BUTTON", L"Under Reset", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                        778, 11, 110, 22, hwnd, reinterpret_cast<HMENU>(IDC_CHK_UNDER_RESET), g_hInst, nullptr);

        // Row 2 (y = 42): Connect, Disconnect, Flash, Verify, HW Reset, SWD Reset, Mass Erase, Debug...
        g_btnConnect    = CreateWindowW(L"BUTTON", L"Connect", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                        12, 42, 92, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_CONNECT), g_hInst, nullptr);
        g_btnDisconnect = CreateWindowW(L"BUTTON", L"Disconnect", WS_CHILD | WS_VISIBLE,
                                        110, 42, 96, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_DISCONNECT), g_hInst, nullptr);
        g_btnFlash      = CreateWindowW(L"BUTTON", L"Flash", WS_CHILD | WS_VISIBLE,
                                        214, 42, 86, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_FLASH), g_hInst, nullptr);
        g_btnVerify     = CreateWindowW(L"BUTTON", L"Verify", WS_CHILD | WS_VISIBLE,
                                        306, 42, 82, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_VERIFY), g_hInst, nullptr);
        g_btnHwReset    = CreateWindowW(L"BUTTON", L"HW Reset", WS_CHILD | WS_VISIBLE,
                                        396, 42, 96, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_HW_RESET), g_hInst, nullptr);
        g_btnSwdReset   = CreateWindowW(L"BUTTON", L"SWD Reset", WS_CHILD | WS_VISIBLE,
                                        498, 42, 98, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_SWD_RESET), g_hInst, nullptr);
        g_btnErase      = CreateWindowW(L"BUTTON", L"Mass Erase", WS_CHILD | WS_VISIBLE,
                                        604, 42, 102, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_ERASE), g_hInst, nullptr);
        g_btnOpenDebug  = CreateWindowW(L"BUTTON", L"Debug...", WS_CHILD | WS_VISIBLE,
                                        786, 42, 102, 28, hwnd, reinterpret_cast<HMENU>(IDC_BTN_OPEN_DEBUG), g_hInst, nullptr);

        // Row 3 (y = 80): Firmware File + Browse + Addr
        CreateWindowW(L"STATIC", L"Firmware:", WS_CHILD | WS_VISIBLE, 12, 83, 62, 20, hwnd, nullptr, g_hInst, nullptr);
        g_edtFilePath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                        78, 80, 560, 24, hwnd, reinterpret_cast<HMENU>(IDC_EDT_FILE_PATH), g_hInst, nullptr);
        g_btnBrowse   = CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE,
                                      644, 79, 88, 26, hwnd, reinterpret_cast<HMENU>(IDC_BTN_BROWSE), g_hInst, nullptr);
        CreateWindowW(L"STATIC", L"Addr:", WS_CHILD | WS_VISIBLE, 742, 83, 38, 20, hwnd, nullptr, g_hInst, nullptr);
        g_edtBaseAddr = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0x08000000", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                        782, 80, 106, 24, hwnd, reinterpret_cast<HMENU>(IDC_EDT_BASE_ADDR), g_hInst, nullptr);

        // Center Workspace (Full width 876px): Target Status & On-Chip Flash
        CreateWindowW(L"STATIC", L"Target Status && On-Chip Flash:", WS_CHILD | WS_VISIBLE,
                      12, 114, 320, 18, hwnd, nullptr, g_hInst, nullptr);
        g_edtTargetInfo = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT",
            L"Not connected.\r\nPlug in the CMSIS-DAP probe and click [Connect] to inspect chip & flash content.",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            12, 134, 876, 270, hwnd, reinterpret_cast<HMENU>(IDC_EDT_TARGET_INFO), g_hInst, nullptr);

        // Bottom: Progress + Clear Log + Log
        g_progress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                     12, 414, 782, 22, hwnd, reinterpret_cast<HMENU>(IDC_PROGRESS), g_hInst, nullptr);
        SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        g_btnClearLog = CreateWindowW(L"BUTTON", L"Clear Log", WS_CHILD | WS_VISIBLE,
                                      802, 413, 86, 24, hwnd, reinterpret_cast<HMENU>(IDC_BTN_CLEAR_LOG), g_hInst, nullptr);

        g_edtLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                   12, 444, 876, 144, hwnd, reinterpret_cast<HMENU>(IDC_EDT_LOG), g_hInst, nullptr);

        EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontUi), TRUE);
            return TRUE;
        }, 0);
        SendMessageW(g_lblUsbStatus, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontUiBold), TRUE);
        SendMessageW(g_edtTargetInfo, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontMono), TRUE);
        SendMessageW(g_edtLog, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontMono), TRUE);

        DEV_BROADCAST_DEVICEINTERFACE_W filter{};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid = WinHidDevice::getHidGuid();
        g_hDevNotifyHid = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

        refreshUsbAutoDetect(false);
        postLog("[READY] TUE CMSIS-DAP Native C++ GUI v1.0.0 ready. USB plug-and-play active.");
        return 0;
    }

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED) {
            SetTimer(hwnd, 1, 250, nullptr);
        }
        return TRUE;

    case WM_TIMER:
        if (wParam == 1) {
            KillTimer(hwnd, 1);
            refreshUsbAutoDetect(true);
            refreshUartComCombo();
        }
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        switch (id) {
        case IDC_CHK_USE_OCD:
            updateActionButtonsState();
            break;
        case IDC_BTN_CONNECT:
            doConnect();
            break;
        case IDC_BTN_DISCONNECT:
            doDisconnect();
            break;
        case IDC_BTN_FLASH:
            doFlash();
            break;
        case IDC_BTN_VERIFY:
            doVerify();
            break;
        case IDC_BTN_HW_RESET:
            doHwReset();
            break;
        case IDC_BTN_SWD_RESET:
            doSwdReset();
            break;
        case IDC_BTN_ERASE:
            doErase();
            break;
        case IDC_BTN_OPEN_DEBUG:
        case IDM_TOOLS_DEBUG:
            openDebugWindow();
            break;
        case IDC_BTN_BROWSE:
        case IDM_FILE_OPEN:
            doBrowseFirmware();
            break;
        case IDM_FILE_DUMP_BIN:
            doDumpFlashBin();
            break;
        case IDM_FILE_EXIT:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        case IDC_BTN_CLEAR_LOG:
            SetWindowTextW(g_edtLog, L"");
            break;
        case IDM_TOOLS_HEXVIEW:
            if (!g_hHexWnd) {
                RECT rc{0, 0, 684, 452};
                AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
                g_hHexWnd = CreateWindowExW(
                    0, L"TueHexViewerClass", L"Memory Hex Viewer & Mem32",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                    CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                    hwnd, nullptr, g_hInst, nullptr);
            }
            ShowWindow(g_hHexWnd, SW_SHOW);
            SetForegroundWindow(g_hHexWnd);
            break;
        case IDM_TOOLS_UART:
            if (!g_hUartWnd) {
                RECT rc{0, 0, 644, 400};
                AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
                g_hUartWnd = CreateWindowExW(
                    0, L"TueUartTermClass", L"UART Terminal (Virtual COM)",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                    CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                    hwnd, nullptr, g_hInst, nullptr);
            }
            refreshUartComCombo();
            ShowWindow(g_hUartWnd, SW_SHOW);
            SetForegroundWindow(g_hUartWnd);
            break;
        case IDM_TOOLS_OCD_CFG:
            if (!g_hOcdDlg) {
                g_hOcdDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"TueOcdCfgClass", L"OpenOCD Settings",
                                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                            CW_USEDEFAULT, CW_USEDEFAULT, 475, 155,
                                            hwnd, nullptr, g_hInst, nullptr);
            }
            ShowWindow(g_hOcdDlg, SW_SHOW);
            SetForegroundWindow(g_hOcdDlg);
            break;
        case IDM_TOOLS_GDB_SERVER:
            if (g_ocdRunner.isGdbRunning()) {
                g_ocdRunner.stopGdbServer();
                postLog("[GDB] OpenOCD GDB Server stopped.");
            } else {
                if (g_ocdRunner.startGdbServer(g_ocdExePath, getSelectedOcdTargetCfg(),
                                               getSwdSpeedHz() / 1000,
                                               [](const std::string& l) { postLog(l); },
                                               []() { postLog("[GDB] Server stopped."); })) {
                    postLog("[GDB] OpenOCD GDB Server listening on localhost:3333");
                }
            }
            break;
        case IDM_HELP_LED:
            MessageBoxW(hwnd,
                        L"Probe LED (PC13) Status Legend:\n\n"
                        L"• Single Blink (1.0s cycle): Idle / Standby (Not connected)\n"
                        L"• Double Blink (1.2s cycle): Target SWD Connected\n"
                        L"• Solid ON: Debug Mode Active\n"
                        L"• 3 Continuous Blinks: Debug Command Executed (Step / Halt / Run / Regs)\n"
                        L"• Rapid Continuous Flicker (12.5 Hz): Active Target Task (Flash / Verify / Erase / Reset / Memory)\n"
                        L"• 3 Fast Blinks (Error): SWD Error / Verify Mismatch\n\n"
                        L"On-board Button (PA0): Hardware reset button (pulls nRESET / PB8 LOW).",
                        L"LED Status Legend", MB_ICONINFORMATION);
            break;
        case IDM_HELP_ABOUT:
            MessageBoxW(hwnd,
                        L"TUE CMSIS-DAP Native C++ Programmer v1.0.0\n"
                        L"Pure Win32 C++17 with USB Plug & Play auto-detection.\n"
                        L"Supports direct SWD flashing (F1/F3/F4/G4/L4) without OpenOCD,\n"
                        L"dedicated CPU Debug Window (Step Into / Step Out),\n"
                        L"Memory Hex Viewer, and Virtual COM UART.",
                        L"About", MB_ICONINFORMATION);
            break;
        }
        return 0;
    }

    case WM_APP_LOG: {
        std::unique_ptr<std::string> p(reinterpret_cast<std::string*>(lParam));
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t tbuf[16];
        std::swprintf(tbuf, 16, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
        appendEditText(g_edtLog, std::wstring(tbuf) + utf8ToWide(*p) + L"\r\n");
        return 0;
    }
    case WM_APP_PROGRESS:
        SendMessageW(g_progress, PBM_SETPOS, wParam, 0);
        return 0;

    case WM_APP_CONNECT_DONE: {
        std::unique_ptr<std::wstring> p(reinterpret_cast<std::wstring*>(lParam));
        if (wParam == 1) {
            g_targetConnected = true;
            if (p) SetWindowTextW(g_edtTargetInfo, p->c_str());
        } else if (wParam == 2) {
            g_targetConnected = false;
            g_debugActive = false;
            SetWindowTextW(g_edtTargetInfo, L"Disconnected.\r\nClick [Connect] to inspect chip & flash content.");
            if (g_dbgText) SetWindowTextW(g_dbgText, L"Debug Session: Inactive.");
        } else {
            g_targetConnected = false;
            g_debugActive = false;
            SetWindowTextW(g_edtTargetInfo, L"Connection failed. Check SWD wiring and target power.");
        }
        updateActionButtonsState();
        return 0;
    }
    case WM_APP_OP_DONE:
        g_busy.store(false);
        updateActionButtonsState();
        return 0;

    case WM_APP_DEBUG_INFO: {
        std::unique_ptr<std::wstring> p(reinterpret_cast<std::wstring*>(lParam));
        if (wParam == 1) g_debugActive = true;
        else if (wParam == 2) g_debugActive = false;
        if (p && g_dbgText) SetWindowTextW(g_dbgText, p->c_str());
        updateActionButtonsState();
        return 0;
    }
    case WM_APP_HEX_DATA: {
        std::unique_ptr<std::wstring> p(reinterpret_cast<std::wstring*>(lParam));
        if (p && g_hexText) SetWindowTextW(g_hexText, p->c_str());
        return 0;
    }
    case WM_APP_MEM32_VAL: {
        wchar_t buf[16];
        std::swprintf(buf, 16, L"0x%08X", static_cast<uint32_t>(wParam));
        if (g_m32Val) SetWindowTextW(g_m32Val, buf);
        return 0;
    }
    case WM_APP_UART_RX: {
        std::unique_ptr<std::string> p(reinterpret_cast<std::string*>(lParam));
        if (p && g_uartLog) appendEditText(g_uartLog, normalizeCrLf(utf8ToWide(*p)));
        return 0;
    }
    case WM_APP_UART_CLOSED:
        if (g_uartBtnConn) SetWindowTextW(g_uartBtnConn, L"Connect COM");
        return 0;

    case WM_DESTROY:
        if ((g_targetConnected || g_debugActive) && !useOpenOcd() && g_probePresent) {
            try {
                HidDeviceInfo devInfo = getSelectedHidInfo();
                WinHidDevice hid;
                std::string err;
                if (hid.open(devInfo, err)) {
                    CmsisDapClient dap(hid);
                    dap.hostStatus(0, 0);
                }
            } catch (...) {}
        }
        g_serial.close();
        g_ocdRunner.stopGdbServer();
        if (g_hDevNotifyHid) UnregisterDeviceNotification(g_hDevNotifyHid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInst = hInstance;

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wcMain{};
    wcMain.cbSize = sizeof(wcMain);
    wcMain.lpfnWndProc = MainWndProc;
    wcMain.hInstance = hInstance;
    wcMain.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcMain.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wcMain.lpszClassName = L"TueDapMainClass";
    wcMain.hIcon = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    wcMain.hIconSm = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_SHARED));
    RegisterClassExW(&wcMain);

    WNDCLASSEXW wcDbg = wcMain;
    wcDbg.lpfnWndProc = DebugWndProc;
    wcDbg.lpszClassName = L"TueDebugWndClass";
    RegisterClassExW(&wcDbg);

    WNDCLASSEXW wcHex = wcMain;
    wcHex.lpfnWndProc = HexWndProc;
    wcHex.lpszClassName = L"TueHexViewerClass";
    RegisterClassExW(&wcHex);

    WNDCLASSEXW wcUart = wcMain;
    wcUart.lpfnWndProc = UartWndProc;
    wcUart.lpszClassName = L"TueUartTermClass";
    RegisterClassExW(&wcUart);

    WNDCLASSEXW wcOcd = wcMain;
    wcOcd.lpfnWndProc = OcdDlgProc;
    wcOcd.lpszClassName = L"TueOcdCfgClass";
    RegisterClassExW(&wcOcd);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, 900, 600};
    AdjustWindowRect(&rc, style, TRUE);

    g_hMainWnd = CreateWindowExW(
        0, L"TueDapMainClass", L"TUE CMSIS-DAP Programmer & Debugger v1.0.0",
        style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, createMainMenu(), hInstance, nullptr);

    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
