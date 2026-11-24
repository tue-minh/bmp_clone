#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

struct LinuxHidInfo {
    std::string devPath;  // e.g. /dev/hidraw0
    std::string product;
    uint16_t vid = 0;
    uint16_t pid = 0;
};

struct FirmwareImageLinux {
    uint32_t baseAddr = 0x08000000;
    std::vector<uint8_t> data;
};

FirmwareImageLinux loadFirmwareImageLinux(const std::string& path, uint32_t defaultBinAddr = 0x08000000);

class LinuxHidDevice {
public:
    LinuxHidDevice() = default;
    ~LinuxHidDevice();

    bool open(const LinuxHidInfo& info, std::string& errOut);
    void close();
    bool isOpen() const { return fd_ >= 0; }
    std::vector<uint8_t> exchange(const std::vector<uint8_t>& payload, uint32_t timeoutMs = 2000);

    static std::vector<LinuxHidInfo> enumerateDapDevices();

private:
    int fd_ = -1;
};

struct Stm32TargetInfo {
    uint32_t dpId = 0;
    uint32_t dbgId = 0;
    uint16_t devId = 0;
    uint16_t revId = 0;
    std::string family;
    uint32_t pageSize = 2048;
    std::string name;
    uint32_t flashKB = 0;
    int rdp = -1;
    std::string uid;
    std::vector<uint8_t> flashPreview;
};

struct CpuDebugStatus {
    Stm32TargetInfo info;
    bool halted = false;
    uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, r12 = 0;
    uint32_t sp = 0, lr = 0, pc = 0, xpsr = 0, msp = 0, psp = 0;
    uint32_t cfsr = 0, hfsr = 0;
};

struct VerifyResult {
    Stm32TargetInfo info;
    uint32_t mismatches = 0;
    uint32_t firstBadAddr = 0;
    uint32_t firstWant = 0;
    uint32_t firstGot = 0;
};

using ProgressCallback = std::function<void(const std::string& stage, uint32_t done, uint32_t total)>;

class LinuxCmsisDap {
public:
    explicit LinuxCmsisDap(LinuxHidDevice& dev) : dev_(dev) {}

    void hostStatus(uint8_t ledType, uint8_t status);
    uint8_t connectSwd();
    void disconnect();
    void swjClock(uint32_t hz);
    void transferConfigure(uint8_t idle = 2, uint16_t waitRetry = 500, uint16_t matchRetry = 0);
    void swdConfigure(uint8_t cfg = 0);
    void swjSequence(uint16_t bitCount, const std::vector<uint8_t>& data);
    uint8_t swjPins(uint8_t pinOut, uint8_t pinSel, uint32_t waitUs = 0);
    uint8_t resetTarget();
    void writeAbort(uint32_t val = 0x1E);

    void transfer(const std::vector<std::pair<uint8_t, uint32_t>>& ops,
                  uint8_t& countOut, uint8_t& ackOut, std::vector<uint32_t>& valsOut);
    void transferBlockWrite(uint8_t reqByte, const std::vector<uint32_t>& words);
    std::vector<uint32_t> transferBlockRead(uint8_t reqByte, uint32_t count);

    void swdLineResetToSwd();
    uint32_t readDp(uint8_t addr);
    void writeDp(uint8_t addr, uint32_t val);
    uint32_t memRead32(uint32_t addr);
    void memWrite32(uint32_t addr, uint32_t val);
    void memWrite16Batch(uint32_t startAddr, const std::vector<uint16_t>& halfwords);
    void memWriteBlock(uint32_t addr, const std::vector<uint32_t>& words);
    std::vector<uint32_t> memReadBlock(uint32_t addr, uint32_t count);

    Stm32TargetInfo probeTarget(uint32_t clockHz = 1000000, bool connectUnderReset = false, bool readPreview = true);
    Stm32TargetInfo flashImage(uint32_t baseAddr, const std::vector<uint8_t>& data,
                               uint32_t clockHz = 1000000, bool connectUnderReset = false,
                               bool verify = true, ProgressCallback cb = nullptr);
    VerifyResult verifyImage(uint32_t baseAddr, const std::vector<uint8_t>& data,
                             uint32_t clockHz = 1000000, bool connectUnderReset = false,
                             ProgressCallback cb = nullptr);
    Stm32TargetInfo massErase(uint32_t clockHz = 1000000, bool connectUnderReset = false);
    std::vector<uint8_t> readMemoryBytes(uint32_t addr, uint32_t length,
                                         uint32_t clockHz = 1000000, bool connectUnderReset = false,
                                         ProgressCallback cb = nullptr);
    CpuDebugStatus readCpuStatus(uint32_t clockHz = 1000000, bool connectUnderReset = false);
    CpuDebugStatus debugStart(uint32_t clockHz = 1000000, bool connectUnderReset = false);
    void debugExit(uint32_t clockHz = 1000000);
    std::string cpuControl(const std::string& action, uint32_t clockHz = 1000000);
    void swdSystemReset(uint32_t clockHz = 1000000);
    void hardResetPulse(uint32_t pulseMs = 50);

private:
    LinuxHidDevice& dev_;
    std::vector<uint8_t> cmd(const std::vector<uint8_t>& payload);
    uint32_t attachAndHalt(uint32_t clockHz, bool connectUnderReset);
    Stm32TargetInfo identifyHaltedTarget(uint32_t dpId);
    void waitFlashReady(uint32_t srAddr, uint32_t bsyMask, uint32_t errMask, uint32_t eopMask, uint32_t timeoutMs);
    void flashF1F3(uint32_t baseAddr, const std::vector<uint8_t>& padded, uint32_t pageSz, ProgressCallback cb);
    void flashF2F4(uint32_t baseAddr, const std::vector<uint8_t>& padded, ProgressCallback cb);
    void flashG4L4(uint32_t baseAddr, const std::vector<uint8_t>& padded, uint32_t pageSz, ProgressCallback cb);
    uint32_t readCoreReg(uint8_t regIdx);
};

class LinuxSerialPort {
public:
    LinuxSerialPort() = default;
    ~LinuxSerialPort();

    bool open(const std::string& devPath, uint32_t baudRate, std::string& errOut,
              std::function<void(const std::string&)> onRx,
              std::function<void()> onClosed);
    void close();
    bool isOpen() const { return running_.load(); }
    bool writeText(const std::string& text);

    static std::vector<std::string> enumeratePorts();

private:
    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread rxThread_;
};
