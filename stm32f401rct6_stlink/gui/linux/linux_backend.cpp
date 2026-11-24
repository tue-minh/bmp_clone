#include "linux_backend.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <poll.h>
#include <stdexcept>
#include <termios.h>
#include <unistd.h>
#include <unordered_map>

namespace {

inline void msleep(uint32_t ms) {
    usleep(ms * 1000U);
}

inline uint64_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

inline uint16_t getLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t getLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline void pushLE16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

inline void pushLE32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

std::string hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

std::string strLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

constexpr uint8_t ID_DAP_CONNECT            = 0x02;
constexpr uint8_t ID_DAP_DISCONNECT         = 0x03;
constexpr uint8_t ID_DAP_TRANSFER_CONFIGURE = 0x04;
constexpr uint8_t ID_DAP_TRANSFER           = 0x05;
constexpr uint8_t ID_DAP_TRANSFER_BLOCK     = 0x06;
constexpr uint8_t ID_DAP_WRITE_ABORT        = 0x08;
constexpr uint8_t ID_DAP_RESET_TARGET       = 0x0A;
constexpr uint8_t ID_DAP_SWJ_PINS           = 0x10;
constexpr uint8_t ID_DAP_SWJ_CLOCK          = 0x11;
constexpr uint8_t ID_DAP_SWJ_SEQUENCE       = 0x12;
constexpr uint8_t ID_DAP_SWD_CONFIGURE      = 0x13;

constexpr uint32_t FLASH_KEY1 = 0x45670123;
constexpr uint32_t FLASH_KEY2 = 0xCDEF89AB;

struct DeviceEntry {
    const char* family;
    uint32_t pageSize;
    const char* name;
};

const std::unordered_map<uint16_t, DeviceEntry>& getDeviceTable() {
    static const std::unordered_map<uint16_t, DeviceEntry> tbl = {
        {0x412, {"F1", 1024,  "STM32F10x Low-density"}},
        {0x410, {"F1", 1024,  "STM32F10x Medium-density (F103C8/RB)"}},
        {0x414, {"F1", 2048,  "STM32F10x High-density (F103RC/ZE)"}},
        {0x418, {"F1", 2048,  "STM32F105/F107 Connectivity"}},
        {0x420, {"F1", 1024,  "STM32F100 Value Line"}},
        {0x428, {"F1", 2048,  "STM32F100 High-density Value Line"}},
        {0x430, {"F1", 2048,  "STM32F10x XL-density"}},
        {0x422, {"F3", 2048,  "STM32F302/F303xB/C"}},
        {0x432, {"F3", 2048,  "STM32F373"}},
        {0x438, {"F3", 2048,  "STM32F303x6/8 / F334"}},
        {0x439, {"F3", 2048,  "STM32F301/F302x6/8"}},
        {0x446, {"F3", 2048,  "STM32F302/F303xD/E"}},
        {0x411, {"F2", 16384, "STM32F20x/F21x"}},
        {0x413, {"F4", 16384, "STM32F405/F407/F415/F417"}},
        {0x419, {"F4", 16384, "STM32F427/F429/F437/F439"}},
        {0x421, {"F4", 16384, "STM32F446"}},
        {0x423, {"F4", 16384, "STM32F401xB/C"}},
        {0x431, {"F4", 16384, "STM32F411xC/E"}},
        {0x433, {"F4", 16384, "STM32F401xD/E"}},
        {0x434, {"F4", 16384, "STM32F469/F479"}},
        {0x441, {"F4", 16384, "STM32F412"}},
        {0x458, {"F4", 16384, "STM32F410"}},
        {0x463, {"F4", 16384, "STM32F413/F423"}},
        {0x468, {"G4", 2048,  "STM32G431/G441 (Cat.2)"}},
        {0x469, {"G4", 2048,  "STM32G473/G474/G484 (Cat.3)"}},
        {0x479, {"G4", 2048,  "STM32G491/G4A1 (Cat.4)"}},
        {0x415, {"L4", 2048,  "STM32L475/L476/L486"}},
        {0x435, {"L4", 2048,  "STM32L43x/L44x"}},
        {0x462, {"L4", 2048,  "STM32L45x/L46x"}},
        {0x464, {"L4", 2048,  "STM32L41x/L42x"}},
        {0x440, {"F0", 1024,  "STM32F05x/F030x8"}},
        {0x444, {"F0", 1024,  "STM32F03x/F04x"}},
        {0x445, {"F0", 2048,  "STM32F07x"}},
        {0x448, {"F0", 2048,  "STM32F09x/F030xC"}},
        {0x460, {"G0", 2048,  "STM32G07x/G08x"}},
        {0x466, {"G0", 2048,  "STM32G03x/G04x"}},
        {0x467, {"G0", 2048,  "STM32G0Bx/G0Cx"}},
    };
    return tbl;
}

} // namespace

// ---------------------------------------------------------------------------
// Firmware Loader (.bin / .hex / .elf)
// ---------------------------------------------------------------------------
static uint8_t hexNib(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    throw std::runtime_error("Invalid hex char");
}

FirmwareImageLinux loadFirmwareImageLinux(const std::string& path, uint32_t defaultBinAddr) {
    size_t dot = path.find_last_of('.');
    std::string ext = (dot == std::string::npos) ? "" : strLower(path.substr(dot));

    if (ext == ".hex") {
        std::ifstream f(path);
        if (!f) throw std::runtime_error("Cannot open .hex file");
        uint32_t extAddr = 0;
        std::map<uint32_t, uint8_t> chunks;
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
                line.pop_back();
            if (line.size() < 11 || line[0] != ':') continue;
            std::vector<uint8_t> raw;
            for (size_t i = 1; i + 1 < line.size(); i += 2) {
                raw.push_back(static_cast<uint8_t>((hexNib(line[i]) << 4) | hexNib(line[i + 1])));
            }
            if (raw.size() < 5) continue;
            uint8_t len = raw[0];
            uint16_t addrLo = static_cast<uint16_t>((raw[1] << 8) | raw[2]);
            uint8_t rtype = raw[3];
            if (raw.size() < static_cast<size_t>(4 + len)) continue;
            if (rtype == 0x00) {
                for (uint8_t i = 0; i < len; ++i) chunks[extAddr + addrLo + i] = raw[4 + i];
            } else if (rtype == 0x01) {
                break;
            } else if (rtype == 0x02 && len == 2) {
                extAddr = static_cast<uint32_t>((raw[4] << 8) | raw[5]) << 4;
            } else if (rtype == 0x04 && len == 2) {
                extAddr = static_cast<uint32_t>((raw[4] << 8) | raw[5]) << 16;
            }
        }
        if (chunks.empty()) throw std::runtime_error("No flash data in .hex");
        uint32_t minAddr = chunks.begin()->first;
        uint32_t maxAddr = chunks.rbegin()->first;
        FirmwareImageLinux img;
        img.baseAddr = minAddr;
        img.data.assign(maxAddr - minAddr + 1, 0xFF);
        for (const auto& kv : chunks) img.data[kv.first - minAddr] = kv.second;
        return img;
    }

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open firmware file");
    std::streamsize sz = f.tellg();
    if (sz <= 0) throw std::runtime_error("Empty firmware file");
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(buf.data()), sz)) throw std::runtime_error("Read error");

    if (ext == ".elf") {
        if (buf.size() < 52 || buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F' ||
            buf[4] != 1 || buf[5] != 1) {
            throw std::runtime_error("Invalid 32-bit little-endian ELF");
        }
        uint32_t e_phoff = getLE32(&buf[28]);
        uint16_t e_phentsize = getLE16(&buf[42]);
        uint16_t e_phnum = getLE16(&buf[44]);
        struct Seg { uint32_t paddr; std::vector<uint8_t> bytes; };
        std::vector<Seg> segs;
        for (uint16_t i = 0; i < e_phnum; ++i) {
            size_t off = static_cast<size_t>(e_phoff) + static_cast<size_t>(i) * e_phentsize;
            if (off + 32 > buf.size()) break;
            uint32_t p_type = getLE32(&buf[off]);
            uint32_t p_offset = getLE32(&buf[off + 4]);
            uint32_t p_paddr = getLE32(&buf[off + 12]);
            uint32_t p_filesz = getLE32(&buf[off + 16]);
            if (p_type == 1 && p_filesz > 0 && p_paddr >= 0x08000000 && p_paddr < 0x08400000) {
                if (static_cast<size_t>(p_offset) + p_filesz <= buf.size()) {
                    segs.push_back({p_paddr, std::vector<uint8_t>(buf.begin() + p_offset, buf.begin() + p_offset + p_filesz)});
                }
            }
        }
        if (segs.empty()) throw std::runtime_error("No Flash segments in .elf");
        std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) { return a.paddr < b.paddr; });
        uint32_t base = segs.front().paddr;
        uint32_t end = base;
        for (const auto& s : segs) end = std::max(end, s.paddr + static_cast<uint32_t>(s.bytes.size()));
        FirmwareImageLinux img;
        img.baseAddr = base;
        img.data.assign(end - base, 0xFF);
        for (const auto& s : segs) std::copy(s.bytes.begin(), s.bytes.end(), img.data.begin() + (s.paddr - base));
        return img;
    }

    if (buf.size() < 4) throw std::runtime_error(".bin file too small");
    return {defaultBinAddr, std::move(buf)};
}

// ---------------------------------------------------------------------------
// Linux HID Device (/dev/hidraw*)
// ---------------------------------------------------------------------------
std::vector<LinuxHidInfo> LinuxHidDevice::enumerateDapDevices() {
    std::vector<LinuxHidInfo> out;
    DIR* d = opendir("/sys/class/hidraw");
    if (!d) return out;
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name.rfind("hidraw", 0) != 0) continue;
        std::ifstream ue("/sys/class/hidraw/" + name + "/device/uevent");
        if (!ue) continue;
        std::string line;
        uint16_t vid = 0, pid = 0;
        std::string prod;
        while (std::getline(ue, line)) {
            if (line.rfind("HID_NAME=", 0) == 0) {
                prod = line.substr(9);
            } else if (line.rfind("HID_ID=", 0) == 0) {
                unsigned int bus = 0, v = 0, p = 0;
                if (std::sscanf(line.c_str() + 7, "%x:%x:%x", &bus, &v, &p) == 3) {
                    vid = static_cast<uint16_t>(v);
                    pid = static_cast<uint16_t>(p);
                }
            }
        }
        std::string low = strLower(prod);
        bool isTue = (vid == 0x0483 && (pid == 0x5752 || pid == 0x5750));
        bool isDap = (low.find("cmsis-dap") != std::string::npos || low.find("tue") != std::string::npos);
        if (isTue || isDap) {
            out.push_back({"/dev/" + name, prod.empty() ? "CMSIS-DAP HID" : prod, vid, pid});
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const LinuxHidInfo& a, const LinuxHidInfo& b) {
        bool aTue = (a.vid == 0x0483 && a.pid == 0x5752);
        bool bTue = (b.vid == 0x0483 && b.pid == 0x5752);
        return aTue > bTue;
    });
    return out;
}

LinuxHidDevice::~LinuxHidDevice() {
    close();
}

bool LinuxHidDevice::open(const LinuxHidInfo& info, std::string& errOut) {
    close();
    fd_ = ::open(info.devPath.c_str(), O_RDWR);
    if (fd_ < 0) {
        errOut = "Cannot open " + info.devPath + " (" + std::strerror(errno) + "). Check udev permissions.";
        return false;
    }
    return true;
}

void LinuxHidDevice::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::vector<uint8_t> LinuxHidDevice::exchange(const std::vector<uint8_t>& payload, uint32_t timeoutMs) {
    if (fd_ < 0) throw std::runtime_error("HID device not open");
    std::vector<uint8_t> outBuf(65, 0);
    size_t nCopy = std::min(payload.size(), size_t(64));
    outBuf[0] = 0x00;
    for (size_t i = 0; i < nCopy; ++i) outBuf[1 + i] = payload[i];

    ssize_t w = ::write(fd_, outBuf.data(), outBuf.size());
    if (w < 0) throw std::runtime_error(std::string("HID write failed: ") + std::strerror(errno));

    struct pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, static_cast<int>(timeoutMs));
    if (pr <= 0) throw std::runtime_error("HID read timeout");

    std::vector<uint8_t> inBuf(64, 0);
    ssize_t r = ::read(fd_, inBuf.data(), inBuf.size());
    if (r <= 0) throw std::runtime_error("HID read failed");
    inBuf.resize(static_cast<size_t>(r));
    return inBuf;
}

// ---------------------------------------------------------------------------
// Linux CMSIS-DAP Client (identical protocol to Windows)
// ---------------------------------------------------------------------------
std::vector<uint8_t> LinuxCmsisDap::cmd(const std::vector<uint8_t>& payload) {
    auto r = dev_.exchange(payload);
    if (r.empty() || r[0] != payload[0]) throw std::runtime_error("DAP invalid response");
    return r;
}

uint8_t LinuxCmsisDap::connectSwd() {
    auto r = cmd({ID_DAP_CONNECT, 0x01});
    return r.size() > 1 ? r[1] : 0;
}

void LinuxCmsisDap::disconnect() {
    cmd({ID_DAP_DISCONNECT});
}

void LinuxCmsisDap::swjClock(uint32_t hz) {
    std::vector<uint8_t> req = {ID_DAP_SWJ_CLOCK};
    pushLE32(req, hz);
    cmd(req);
}

void LinuxCmsisDap::transferConfigure(uint8_t idle, uint16_t waitRetry, uint16_t matchRetry) {
    std::vector<uint8_t> req = {ID_DAP_TRANSFER_CONFIGURE, idle};
    pushLE16(req, waitRetry);
    pushLE16(req, matchRetry);
    cmd(req);
}

void LinuxCmsisDap::swdConfigure(uint8_t cfg) {
    cmd({ID_DAP_SWD_CONFIGURE, cfg});
}

void LinuxCmsisDap::swjSequence(uint16_t bitCount, const std::vector<uint8_t>& data) {
    uint8_t cnt = (bitCount >= 256) ? 0 : static_cast<uint8_t>(bitCount);
    std::vector<uint8_t> req = {ID_DAP_SWJ_SEQUENCE, cnt};
    req.insert(req.end(), data.begin(), data.end());
    cmd(req);
}

uint8_t LinuxCmsisDap::swjPins(uint8_t pinOut, uint8_t pinSel, uint32_t waitUs) {
    std::vector<uint8_t> req = {ID_DAP_SWJ_PINS, pinOut, pinSel};
    pushLE32(req, waitUs);
    auto r = cmd(req);
    return r.size() > 1 ? r[1] : 0;
}

uint8_t LinuxCmsisDap::resetTarget() {
    auto r = cmd({ID_DAP_RESET_TARGET});
    return r.size() > 2 ? r[2] : 0;
}

void LinuxCmsisDap::writeAbort(uint32_t val) {
    std::vector<uint8_t> req = {ID_DAP_WRITE_ABORT, 0x00};
    pushLE32(req, val);
    cmd(req);
}

void LinuxCmsisDap::transfer(const std::vector<std::pair<uint8_t, uint32_t>>& ops,
                             uint8_t& countOut, uint8_t& ackOut, std::vector<uint32_t>& valsOut) {
    std::vector<uint8_t> req = {ID_DAP_TRANSFER, 0x00, static_cast<uint8_t>(ops.size())};
    size_t readCount = 0;
    for (const auto& op : ops) {
        req.push_back(op.first);
        if ((op.first & 0x02) == 0) pushLE32(req, op.second);
        else readCount++;
    }
    auto r = cmd(req);
    countOut = r.size() > 1 ? r[1] : 0;
    ackOut   = r.size() > 2 ? r[2] : 0;
    valsOut.clear();
    size_t off = 3;
    while (off + 4 <= r.size() && valsOut.size() < readCount) {
        valsOut.push_back(getLE32(&r[off]));
        off += 4;
    }
}

void LinuxCmsisDap::transferBlockWrite(uint8_t reqByte, const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); i += 14) {
        size_t cnt = std::min(size_t(14), words.size() - i);
        std::vector<uint8_t> req = {ID_DAP_TRANSFER_BLOCK, 0x00};
        pushLE16(req, static_cast<uint16_t>(cnt));
        req.push_back(reqByte & ~0x02);
        for (size_t j = 0; j < cnt; ++j) pushLE32(req, words[i + j]);
        auto r = cmd(req);
        uint8_t ack = r.size() > 3 ? r[3] : 0;
        if (ack != 1) throw std::runtime_error("TransferBlock write failed");
    }
}

std::vector<uint32_t> LinuxCmsisDap::transferBlockRead(uint8_t reqByte, uint32_t count) {
    std::vector<uint32_t> out;
    out.reserve(count);
    uint32_t rem = count;
    while (rem > 0) {
        uint32_t n = std::min(rem, uint32_t(15));
        std::vector<uint8_t> req = {ID_DAP_TRANSFER_BLOCK, 0x00};
        pushLE16(req, static_cast<uint16_t>(n));
        req.push_back(reqByte | 0x02);
        auto r = cmd(req);
        uint16_t got = r.size() >= 3 ? getLE16(&r[1]) : 0;
        uint8_t ack = r.size() > 3 ? r[3] : 0;
        if (ack != 1 || got != n || r.size() < 4 + n * 4) throw std::runtime_error("TransferBlock read failed");
        for (uint32_t j = 0; j < n; ++j) out.push_back(getLE32(&r[4 + j * 4]));
        rem -= n;
    }
    return out;
}

void LinuxCmsisDap::swdLineResetToSwd() {
    swjSequence(56, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    swjSequence(16, {0x9E, 0xE7});
    swjSequence(56, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    swjSequence(8, {0x00});
}

uint32_t LinuxCmsisDap::readDp(uint8_t addr) {
    uint8_t cnt = 0, ack = 0;
    std::vector<uint32_t> vals;
    transfer({{static_cast<uint8_t>(0x02 | (addr & 0x0C)), 0}}, cnt, ack, vals);
    if (ack != 1 || vals.empty()) throw std::runtime_error("DP read failed");
    return vals[0];
}

void LinuxCmsisDap::writeDp(uint8_t addr, uint32_t val) {
    uint8_t cnt = 0, ack = 0;
    std::vector<uint32_t> vals;
    transfer({{static_cast<uint8_t>(0x00 | (addr & 0x0C)), val}}, cnt, ack, vals);
    if (ack != 1) throw std::runtime_error("DP write failed");
}

uint32_t LinuxCmsisDap::memRead32(uint32_t addr) {
    uint8_t cnt = 0, ack = 0;
    std::vector<uint32_t> vals;
    transfer({{0x05, addr}, {0x0F, 0}}, cnt, ack, vals);
    if (ack != 1 || vals.empty()) throw std::runtime_error("memRead32 failed at " + hex32(addr));
    return vals[0];
}

void LinuxCmsisDap::memWrite32(uint32_t addr, uint32_t val) {
    uint8_t cnt = 0, ack = 0;
    std::vector<uint32_t> vals;
    transfer({{0x05, addr}, {0x0D, val}}, cnt, ack, vals);
    if (ack != 1) throw std::runtime_error("memWrite32 failed at " + hex32(addr));
}

void LinuxCmsisDap::memWrite16Batch(uint32_t startAddr, const std::vector<uint16_t>& halfwords) {
    uint8_t cnt = 0, ack = 0;
    std::vector<uint32_t> vals;
    transfer({{0x01, 0x23000011}}, cnt, ack, vals);
    if (ack != 1) throw std::runtime_error("AP_CSW 16-bit write failed");
    try {
        uint32_t addr = startAddr;
        for (size_t i = 0; i < halfwords.size(); i += 6) {
            size_t n = std::min(size_t(6), halfwords.size() - i);
            std::vector<std::pair<uint8_t, uint32_t>> ops;
            for (size_t j = 0; j < n; ++j) {
                uint16_t hw = halfwords[i + j];
                ops.emplace_back(0x05, addr);
                ops.emplace_back(0x0D, (addr & 2) ? (static_cast<uint32_t>(hw) << 16) : hw);
                addr += 2;
            }
            transfer(ops, cnt, ack, vals);
            if (ack != 1) throw std::runtime_error("write16 batch failed");
        }
    } catch (...) {
        try { transfer({{0x01, 0x23000012}}, cnt, ack, vals); } catch (...) {}
        throw;
    }
    transfer({{0x01, 0x23000012}}, cnt, ack, vals);
}

void LinuxCmsisDap::memWriteBlock(uint32_t addr, const std::vector<uint32_t>& words) {
    size_t idx = 0;
    while (idx < words.size()) {
        uint32_t room = (0x400 - (addr & 0x3FF)) / 4;
        size_t n = std::min(words.size() - idx, static_cast<size_t>(room));
        uint8_t cnt = 0, ack = 0;
        std::vector<uint32_t> vals;
        transfer({{0x05, addr}}, cnt, ack, vals);
        if (ack != 1) throw std::runtime_error("AP_TAR write failed");
        transferBlockWrite(0x0D, std::vector<uint32_t>(words.begin() + idx, words.begin() + idx + n));
        addr += static_cast<uint32_t>(n) * 4;
        idx += n;
    }
}

std::vector<uint32_t> LinuxCmsisDap::memReadBlock(uint32_t addr, uint32_t count) {
    std::vector<uint32_t> out;
    out.reserve(count);
    while (count > 0) {
        uint32_t room = (0x400 - (addr & 0x3FF)) / 4;
        uint32_t n = std::min(count, room);
        uint8_t cnt = 0, ack = 0;
        std::vector<uint32_t> vals;
        transfer({{0x05, addr}}, cnt, ack, vals);
        if (ack != 1) throw std::runtime_error("AP_TAR write failed");
        auto chunk = transferBlockRead(0x0F, n);
        out.insert(out.end(), chunk.begin(), chunk.end());
        addr += n * 4;
        count -= n;
    }
    return out;
}

uint32_t LinuxCmsisDap::attachAndHalt(uint32_t clockHz, bool connectUnderReset) {
    if (connectSwd() != 1) throw std::runtime_error("DAP_Connect(SWD) failed");
    swjClock(clockHz);
    transferConfigure(2, 500, 0);
    swdConfigure(0);
    if (connectUnderReset) { swjPins(0x00, 0x80, 0); msleep(15); }
    swdLineResetToSwd();
    uint32_t dpId = readDp(0x00);
    writeAbort(0x1E);
    writeDp(0x08, 0);
    writeDp(0x04, 0x50000000);
    bool powered = false;
    for (int i = 0; i < 50; ++i) {
        if ((readDp(0x04) & 0xA0000000) == 0xA0000000) { powered = true; break; }
        msleep(2);
    }
    if (!powered) throw std::runtime_error("Debug power-up timeout");
    uint8_t cnt = 0, ack = 0;
    std::vector<uint32_t> vals;
    transfer({{0x01, 0x23000012}}, cnt, ack, vals);
    if (ack != 1) throw std::runtime_error("AP_CSW write failed");
    if (connectUnderReset) {
        memWrite32(0xE000EDF0, 0xA05F0003);
        memWrite32(0xE000EDFC, 0x00000001);
        swjPins(0x80, 0x80, 0);
        msleep(20);
    } else {
        memWrite32(0xE000EDF0, 0xA05F0003);
    }
    bool halted = false;
    for (int i = 0; i < 100; ++i) {
        if (memRead32(0xE000EDF0) & (1U << 17)) { halted = true; break; }
        msleep(2);
    }
    if (!halted) throw std::runtime_error("CPU halt timeout");
    if (connectUnderReset) memWrite32(0xE000EDFC, 0x00000000);
    return dpId;
}

Stm32TargetInfo LinuxCmsisDap::identifyHaltedTarget(uint32_t dpId) {
    uint32_t dbgId = memRead32(0xE0042000);
    if ((dbgId & 0xFFF) == 0) dbgId = memRead32(0x40015800);
    uint16_t devId = static_cast<uint16_t>(dbgId & 0xFFF);
    uint16_t revId = static_cast<uint16_t>((dbgId >> 16) & 0xFFFF);

    Stm32TargetInfo info;
    info.dpId = dpId; info.dbgId = dbgId; info.devId = devId; info.revId = revId;
    const auto& tbl = getDeviceTable();
    auto it = tbl.find(devId);
    if (it != tbl.end()) {
        info.family = it->second.family; info.pageSize = it->second.pageSize; info.name = it->second.name;
    } else {
        info.family = "?"; info.pageSize = 2048; info.name = "Unknown STM32 / Cortex-M";
    }

    uint32_t uidBase = 0;
    if (info.family == "F1" || info.family == "F3") {
        info.flashKB = memRead32(0x1FFFF7E0) & 0xFFFF;
        info.rdp = (memRead32(0x4002201C) & 2) ? 0xBB : 0xAA;
        uidBase = 0x1FFFF7E8;
    } else if (info.family == "F2" || info.family == "F4") {
        info.flashKB = (memRead32(0x1FFF7A20) >> 16) & 0xFFFF;
        info.rdp = static_cast<int>((memRead32(0x40023C14) >> 8) & 0xFF);
        uidBase = 0x1FFF7A10;
    } else if (info.family == "G4" || info.family == "L4") {
        info.flashKB = memRead32(0x1FFF75E0) & 0xFFFF;
        info.rdp = static_cast<int>(memRead32(0x40022020) & 0xFF);
        uidBase = 0x1FFF7590;
    } else if (info.family == "F0") {
        info.flashKB = memRead32(0x1FFFF7CC) & 0xFFFF;
        uidBase = 0x1FFFF7AC;
    } else if (info.family == "G0") {
        info.flashKB = memRead32(0x1FFF75E0) & 0xFFFF;
        uidBase = 0x1FFF7590;
    }
    if (uidBase) {
        uint32_t u0 = memRead32(uidBase), u1 = memRead32(uidBase + 4), u2 = memRead32(uidBase + 8);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%08X-%08X-%08X", u2, u1, u0);
        info.uid = buf;
    } else {
        info.uid = "N/A";
    }
    if (info.flashKB == 0xFFFF) info.flashKB = 0;
    if (info.family != "F0" && info.family != "G0") {
        memWrite32(0xE0042008, memRead32(0xE0042008) | (1U << 11) | (1U << 12));
    }
    return info;
}

Stm32TargetInfo LinuxCmsisDap::probeTarget(uint32_t clockHz, bool connectUnderReset, bool readPreview) {
    try {
        uint32_t dpId = attachAndHalt(clockHz, connectUnderReset);
        Stm32TargetInfo info = identifyHaltedTarget(dpId);
        if (readPreview) {
            try {
                auto words = memReadBlock(0x08000000, 128);
                info.flashPreview.resize(words.size() * 4);
                for (size_t i = 0; i < words.size(); ++i) {
                    uint32_t w = words[i];
                    info.flashPreview[i * 4 + 0] = w & 0xFF;
                    info.flashPreview[i * 4 + 1] = (w >> 8) & 0xFF;
                    info.flashPreview[i * 4 + 2] = (w >> 16) & 0xFF;
                    info.flashPreview[i * 4 + 3] = (w >> 24) & 0xFF;
                }
            } catch (...) {}
        }
        memWrite32(0xE000EDF0, 0xA05F0001);
        disconnect();
        return info;
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

void LinuxCmsisDap::waitFlashReady(uint32_t srAddr, uint32_t bsyMask, uint32_t errMask, uint32_t eopMask, uint32_t timeoutMs) {
    uint64_t t0 = nowMs();
    while (true) {
        uint32_t sr = memRead32(srAddr);
        if (sr & errMask) {
            memWrite32(srAddr, errMask | eopMask);
            throw std::runtime_error("Flash error SR=" + hex32(sr));
        }
        if ((sr & bsyMask) == 0) return;
        if (nowMs() - t0 > timeoutMs) throw std::runtime_error("Flash timeout");
        msleep(2);
    }
}

void LinuxCmsisDap::flashF1F3(uint32_t baseAddr, const std::vector<uint8_t>& padded, uint32_t pageSz, ProgressCallback cb) {
    const uint32_t CR = 0x40022010, SR = 0x4002200C, KEYR = 0x40022004, AR = 0x40022014;
    if (memRead32(CR) & (1U << 7)) {
        memWrite32(KEYR, FLASH_KEY1); memWrite32(KEYR, FLASH_KEY2);
        if (memRead32(CR) & (1U << 7)) throw std::runtime_error("F1/F3 unlock failed");
    }
    try {
        uint32_t startPage = (baseAddr - 0x08000000) / pageSz;
        uint32_t endPage = (baseAddr + static_cast<uint32_t>(padded.size()) - 1 - 0x08000000) / pageSz;
        uint32_t numPages = endPage - startPage + 1;
        for (uint32_t idx = 0; idx < numPages; ++idx) {
            if (cb) cb("erase", idx, numPages);
            waitFlashReady(SR, 1, 0x14, 0x20, 500);
            memWrite32(CR, 0x02); memWrite32(AR, 0x08000000 + (startPage + idx) * pageSz); memWrite32(CR, 0x42);
            waitFlashReady(SR, 1, 0x14, 0x20, 1000);
        }
        if (cb) cb("erase", numPages, numPages);
        memWrite32(CR, 0x01);
        std::vector<uint16_t> hw(padded.size() / 2);
        for (size_t i = 0; i < hw.size(); ++i) hw[i] = getLE16(&padded[i * 2]);
        for (size_t off = 0; off < hw.size(); off += 128) {
            size_t cnt = std::min(size_t(128), hw.size() - off);
            memWrite16Batch(baseAddr + static_cast<uint32_t>(off * 2), std::vector<uint16_t>(hw.begin() + off, hw.begin() + off + cnt));
            waitFlashReady(SR, 1, 0x14, 0x20, 500);
            if (cb) cb("program", static_cast<uint32_t>((off + cnt) * 2), static_cast<uint32_t>(padded.size()));
        }
        memWrite32(CR, 1U << 7);
    } catch (...) {
        try { memWrite32(CR, 1U << 7); } catch (...) {}
        throw;
    }
}

void LinuxCmsisDap::flashF2F4(uint32_t baseAddr, const std::vector<uint8_t>& padded, ProgressCallback cb) {
    const uint32_t CR = 0x40023C10, SR = 0x40023C0C, KEYR = 0x40023C04;
    if (memRead32(CR) & (1U << 31)) {
        memWrite32(KEYR, FLASH_KEY1); memWrite32(KEYR, FLASH_KEY2);
        if (memRead32(CR) & (1U << 31)) throw std::runtime_error("F4 unlock failed");
    }
    try {
        static const uint32_t sectorSizes[12] = {16384,16384,16384,16384,65536,131072,131072,131072,131072,131072,131072,131072};
        std::vector<uint32_t> sectors;
        uint32_t cur = 0x08000000, imgEnd = baseAddr + static_cast<uint32_t>(padded.size());
        for (uint32_t s = 0; s < 12; ++s) {
            if (cur + sectorSizes[s] > baseAddr && cur < imgEnd) sectors.push_back(s);
            cur += sectorSizes[s];
        }
        for (size_t i = 0; i < sectors.size(); ++i) {
            if (cb) cb("erase", static_cast<uint32_t>(i), static_cast<uint32_t>(sectors.size()));
            waitFlashReady(SR, 1U << 16, 0x1F2, 1, 500);
            memWrite32(SR, 0x1F3);
            uint32_t s = sectors[i];
            memWrite32(CR, (1U << 1) | (s << 3) | (2U << 8));
            memWrite32(CR, (1U << 1) | (s << 3) | (2U << 8) | (1U << 16));
            waitFlashReady(SR, 1U << 16, 0x1F2, 1, 5000);
        }
        if (cb) cb("erase", static_cast<uint32_t>(sectors.size()), static_cast<uint32_t>(sectors.size()));
        memWrite32(CR, (1U << 0) | (2U << 8));
        std::vector<uint32_t> words(padded.size() / 4);
        for (size_t i = 0; i < words.size(); ++i) words[i] = getLE32(&padded[i * 4]);
        for (size_t off = 0; off < words.size(); off += 128) {
            size_t cnt = std::min(size_t(128), words.size() - off);
            memWriteBlock(baseAddr + static_cast<uint32_t>(off * 4), std::vector<uint32_t>(words.begin() + off, words.begin() + off + cnt));
            waitFlashReady(SR, 1U << 16, 0x1F2, 1, 500);
            if (cb) cb("program", static_cast<uint32_t>((off + cnt) * 4), static_cast<uint32_t>(padded.size()));
        }
        memWrite32(CR, 1U << 31);
    } catch (...) {
        try { memWrite32(CR, 1U << 31); } catch (...) {}
        throw;
    }
}

void LinuxCmsisDap::flashG4L4(uint32_t baseAddr, const std::vector<uint8_t>& padded, uint32_t pageSz, ProgressCallback cb) {
    const uint32_t CR = 0x40022014, SR = 0x40022010, KEYR = 0x40022008;
    if (memRead32(CR) & (1U << 31)) {
        memWrite32(KEYR, FLASH_KEY1); memWrite32(KEYR, FLASH_KEY2);
        if (memRead32(CR) & (1U << 31)) throw std::runtime_error("G4/L4 unlock failed");
    }
    try {
        uint32_t startPage = (baseAddr - 0x08000000) / pageSz;
        uint32_t endPage = (baseAddr + static_cast<uint32_t>(padded.size()) - 1 - 0x08000000) / pageSz;
        uint32_t numPages = endPage - startPage + 1;
        for (uint32_t idx = 0; idx < numPages; ++idx) {
            if (cb) cb("erase", idx, numPages);
            waitFlashReady(SR, 1U << 16, 0x3FA, 1, 500);
            memWrite32(SR, 0x3FB);
            uint32_t p = startPage + idx;
            memWrite32(CR, (1U << 1) | ((p & 0x7F) << 3));
            memWrite32(CR, (1U << 1) | ((p & 0x7F) << 3) | (1U << 16));
            waitFlashReady(SR, 1U << 16, 0x3FA, 1, 1000);
        }
        if (cb) cb("erase", numPages, numPages);
        memWrite32(CR, 1U << 0);
        std::vector<uint32_t> words(padded.size() / 4);
        for (size_t i = 0; i < words.size(); ++i) words[i] = getLE32(&padded[i * 4]);
        for (size_t off = 0; off < words.size(); off += 128) {
            size_t cnt = std::min(size_t(128), words.size() - off);
            memWriteBlock(baseAddr + static_cast<uint32_t>(off * 4), std::vector<uint32_t>(words.begin() + off, words.begin() + off + cnt));
            waitFlashReady(SR, 1U << 16, 0x3FA, 1, 500);
            if (cb) cb("program", static_cast<uint32_t>((off + cnt) * 4), static_cast<uint32_t>(padded.size()));
        }
        memWrite32(CR, 1U << 31);
    } catch (...) {
        try { memWrite32(CR, 1U << 31); } catch (...) {}
        throw;
    }
}

Stm32TargetInfo LinuxCmsisDap::flashImage(uint32_t baseAddr, const std::vector<uint8_t>& data,
                                          uint32_t clockHz, bool connectUnderReset,
                                          bool verify, ProgressCallback cb) {
    try {
        uint32_t dpId = attachAndHalt(clockHz, connectUnderReset);
        Stm32TargetInfo info = identifyHaltedTarget(dpId);
        if (info.rdp >= 0 && info.rdp != 0xAA) throw std::runtime_error("RDP active");
        std::vector<uint8_t> padded = data;
        while (padded.size() % 8 != 0) padded.push_back(0xFF);
        uint32_t totalLen = static_cast<uint32_t>(padded.size());

        if (info.family == "F1" || info.family == "F3") flashF1F3(baseAddr, padded, info.pageSize, cb);
        else if (info.family == "F2" || info.family == "F4") flashF2F4(baseAddr, padded, cb);
        else if (info.family == "G4" || info.family == "L4") flashG4L4(baseAddr, padded, info.pageSize, cb);
        else throw std::runtime_error("Unsupported family: " + info.name);

        if (verify) {
            uint32_t totalWords = totalLen / 4;
            for (uint32_t offW = 0; offW < totalWords; offW += 256) {
                uint32_t cnt = std::min(uint32_t(256), totalWords - offW);
                auto got = memReadBlock(baseAddr + offW * 4, cnt);
                for (uint32_t i = 0; i < cnt; ++i) {
                    if (got[i] != getLE32(&padded[(offW + i) * 4])) {
                        throw std::runtime_error("Verify mismatch @ " + hex32(baseAddr + (offW + i) * 4));
                    }
                }
                if (cb) cb("verify", std::min((offW + cnt) * 4, totalLen), totalLen);
            }
        }
        memWrite32(0xE000EDFC, 0);
        memWrite32(0xE000EDF0, 0xA05F0001);
        try { memWrite32(0xE000ED0C, 0x05FA0004); } catch (...) {}
        msleep(30);
        resetTarget();
        disconnect();
        return info;
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

VerifyResult LinuxCmsisDap::verifyImage(uint32_t baseAddr, const std::vector<uint8_t>& data,
                                        uint32_t clockHz, bool connectUnderReset, ProgressCallback cb) {
    try {
        uint32_t dpId = attachAndHalt(clockHz, connectUnderReset);
        VerifyResult res;
        res.info = identifyHaltedTarget(dpId);
        std::vector<uint8_t> padded = data;
        while (padded.size() % 4 != 0) padded.push_back(0xFF);
        uint32_t totalWords = static_cast<uint32_t>(padded.size() / 4);
        for (uint32_t offW = 0; offW < totalWords; offW += 256) {
            uint32_t cnt = std::min(uint32_t(256), totalWords - offW);
            auto got = memReadBlock(baseAddr + offW * 4, cnt);
            for (uint32_t i = 0; i < cnt; ++i) {
                uint32_t want = getLE32(&padded[(offW + i) * 4]);
                if (got[i] != want) {
                    if (res.mismatches == 0) {
                        res.firstBadAddr = baseAddr + (offW + i) * 4;
                        res.firstWant = want;
                        res.firstGot = got[i];
                    }
                    res.mismatches++;
                }
            }
            if (cb) cb("verify", std::min((offW + cnt) * 4, static_cast<uint32_t>(padded.size())),
                       static_cast<uint32_t>(padded.size()));
        }
        memWrite32(0xE000EDF0, 0xA05F0001);
        disconnect();
        return res;
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

Stm32TargetInfo LinuxCmsisDap::massErase(uint32_t clockHz, bool connectUnderReset) {
    try {
        uint32_t dpId = attachAndHalt(clockHz, connectUnderReset);
        Stm32TargetInfo info = identifyHaltedTarget(dpId);
        if (info.rdp >= 0 && info.rdp != 0xAA) throw std::runtime_error("RDP active");
        if (info.family == "F1" || info.family == "F3") {
            const uint32_t CR = 0x40022010, SR = 0x4002200C, KEYR = 0x40022004;
            if (memRead32(CR) & (1U << 7)) { memWrite32(KEYR, FLASH_KEY1); memWrite32(KEYR, FLASH_KEY2); }
            waitFlashReady(SR, 1, 0x14, 0x20, 1000);
            memWrite32(CR, 1U << 2); memWrite32(CR, (1U << 2) | (1U << 6));
            waitFlashReady(SR, 1, 0x14, 0x20, 5000);
            memWrite32(CR, 1U << 7);
        } else if (info.family == "F2" || info.family == "F4") {
            const uint32_t CR = 0x40023C10, SR = 0x40023C0C, KEYR = 0x40023C04;
            if (memRead32(CR) & (1U << 31)) { memWrite32(KEYR, FLASH_KEY1); memWrite32(KEYR, FLASH_KEY2); }
            waitFlashReady(SR, 1U << 16, 0x1F2, 1, 1000);
            memWrite32(SR, 0x1F3);
            memWrite32(CR, (1U << 2) | (2U << 8)); memWrite32(CR, (1U << 2) | (2U << 8) | (1U << 16));
            waitFlashReady(SR, 1U << 16, 0x1F2, 1, 15000);
            memWrite32(CR, 1U << 31);
        } else if (info.family == "G4" || info.family == "L4") {
            const uint32_t CR = 0x40022014, SR = 0x40022010, KEYR = 0x40022008;
            if (memRead32(CR) & (1U << 31)) { memWrite32(KEYR, FLASH_KEY1); memWrite32(KEYR, FLASH_KEY2); }
            waitFlashReady(SR, 1U << 16, 0x3FA, 1, 1000);
            memWrite32(SR, 0x3FB);
            memWrite32(CR, 1U << 2); memWrite32(CR, (1U << 2) | (1U << 16));
            waitFlashReady(SR, 1U << 16, 0x3FA, 1, 10000);
            memWrite32(CR, 1U << 31);
        } else {
            throw std::runtime_error("Mass Erase unsupported for " + info.name);
        }
        disconnect();
        return info;
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

std::vector<uint8_t> LinuxCmsisDap::readMemoryBytes(uint32_t addr, uint32_t length,
                                                    uint32_t clockHz, bool connectUnderReset,
                                                    ProgressCallback cb) {
    try {
        attachAndHalt(clockHz, connectUnderReset);
        uint32_t alignedStart = addr & ~3U;
        uint32_t alignedEnd = (addr + length + 3U) & ~3U;
        uint32_t totalWords = (alignedEnd - alignedStart) / 4U;
        std::vector<uint8_t> raw;
        raw.reserve(totalWords * 4U);
        for (uint32_t offW = 0; offW < totalWords; offW += 256) {
            uint32_t cnt = std::min(uint32_t(256), totalWords - offW);
            auto chunk = memReadBlock(alignedStart + offW * 4, cnt);
            for (uint32_t w : chunk) {
                raw.push_back(w & 0xFF); raw.push_back((w >> 8) & 0xFF);
                raw.push_back((w >> 16) & 0xFF); raw.push_back((w >> 24) & 0xFF);
            }
            if (cb) cb("read", std::min((offW + cnt) * 4, totalWords * 4), totalWords * 4);
        }
        memWrite32(0xE000EDF0, 0xA05F0001);
        disconnect();
        uint32_t skip = addr - alignedStart;
        return std::vector<uint8_t>(raw.begin() + skip, raw.begin() + skip + length);
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

uint32_t LinuxCmsisDap::readCoreReg(uint8_t regIdx) {
    memWrite32(0xE000EDF4, regIdx & 0x1F);
    for (int i = 0; i < 30; ++i) {
        if (memRead32(0xE000EDF0) & (1U << 16)) return memRead32(0xE000EDF8);
    }
    throw std::runtime_error("DCRSR reg timeout");
}

CpuDebugStatus LinuxCmsisDap::readCpuStatus(uint32_t clockHz, bool connectUnderReset) {
    try {
        uint32_t dpId = attachAndHalt(clockHz, connectUnderReset);
        CpuDebugStatus st;
        st.info = identifyHaltedTarget(dpId);
        st.halted = (memRead32(0xE000EDF0) & (1U << 17)) != 0;
        st.r0 = readCoreReg(0); st.r1 = readCoreReg(1); st.r2 = readCoreReg(2); st.r3 = readCoreReg(3);
        st.r12 = readCoreReg(12); st.sp = readCoreReg(13); st.lr = readCoreReg(14);
        st.pc = readCoreReg(15); st.xpsr = readCoreReg(16); st.msp = readCoreReg(17); st.psp = readCoreReg(18);
        st.cfsr = memRead32(0xE000ED28); st.hfsr = memRead32(0xE000ED2C);
        disconnect();
        return st;
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

CpuDebugStatus LinuxCmsisDap::debugStart(uint32_t clockHz, bool connectUnderReset) {
    return readCpuStatus(clockHz, connectUnderReset);
}

void LinuxCmsisDap::debugExit(uint32_t clockHz) {
    if (connectSwd() != 1) throw std::runtime_error("DAP_Connect failed");
    try {
        swjClock(clockHz); transferConfigure(2, 500, 0); swdConfigure(0);
        swdLineResetToSwd(); readDp(0); writeAbort(0x1E); writeDp(8, 0); writeDp(4, 0x50000000);
        for (int i = 0; i < 30; ++i) { if ((readDp(4) & 0xA0000000) == 0xA0000000) break; msleep(2); }
        uint8_t c, a; std::vector<uint32_t> v;
        transfer({{0x01, 0x23000012}}, c, a, v);
        try { memWrite32(0xE0002008, 0); } catch (...) {}
        try { memWrite32(0xE0002000, 2); } catch (...) {}
        memWrite32(0xE000EDFC, 0);
        memWrite32(0xE000EDF0, 0xA05F0000);
        disconnect();
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

std::string LinuxCmsisDap::cpuControl(const std::string& action, uint32_t clockHz) {
    if (connectSwd() != 1) throw std::runtime_error("DAP_Connect failed");
    try {
        swjClock(clockHz);
        transferConfigure(2, 500, 0);
        swdConfigure(0);
        swdLineResetToSwd();
        readDp(0x00); writeAbort(0x1E); writeDp(0x08, 0); writeDp(0x04, 0x50000000);
        for (int i = 0; i < 30; ++i) {
            if ((readDp(0x04) & 0xA0000000) == 0xA0000000) break;
            msleep(2);
        }
        uint8_t c, a; std::vector<uint32_t> v;
        transfer({{0x01, 0x23000012}}, c, a, v);
        std::string msg;
        if (action == "halt") { memWrite32(0xE000EDF0, 0xA05F0003); msg = "CPU HALTED"; }
        else if (action == "run") { memWrite32(0xE000EDF0, 0xA05F0001); msg = "CPU RUNNING"; }
        else if (action == "step_into" || action == "step") {
            memWrite32(0xE000EDF0, 0xA05F0003); memWrite32(0xE000EDF0, 0xA05F0005);
            msg = "STEP INTO -> PC=" + hex32(readCoreReg(15));
        } else if (action == "step_out") {
            memWrite32(0xE000EDF0, 0xA05F0003);
            uint32_t lr = readCoreReg(14);
            uint32_t retAddr = lr & ~1U;
            if (retAddr < 0x08000000U || retAddr >= 0x20000000U) {
                throw std::runtime_error("Cannot Step Out: LR (" + hex32(lr) + ") not in Flash");
            }
            uint32_t compVal = (retAddr & 0x1FFFFFFCU) | 1U | ((retAddr & 2U) ? (1U << 31) : (1U << 30));
            memWrite32(0xE0002000, 3);
            memWrite32(0xE0002008, compVal);
            memWrite32(0xE000EDF0, 0xA05F0001);
            bool hit = false;
            uint64_t t0 = nowMs();
            while (nowMs() - t0 < 1500) {
                if (memRead32(0xE000EDF0) & (1U << 17)) { hit = true; break; }
                msleep(5);
            }
            if (!hit) memWrite32(0xE000EDF0, 0xA05F0003);
            memWrite32(0xE0002008, 0);
            memWrite32(0xE0002000, 2);
            uint32_t pc = readCoreReg(15);
            msg = hit ? ("STEP OUT -> PC=" + hex32(pc)) : ("STEP OUT timeout (halted @ PC=" + hex32(pc) + ")");
        } else {
            throw std::runtime_error("Unknown action");
        }
        disconnect();
        return msg;
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

void LinuxCmsisDap::swdSystemReset(uint32_t clockHz) {
    if (connectSwd() != 1) throw std::runtime_error("DAP_Connect failed");
    try {
        swjClock(clockHz); transferConfigure(2, 500, 0); swdConfigure(0);
        swdLineResetToSwd(); readDp(0); writeAbort(0x1E); writeDp(8, 0); writeDp(4, 0x50000000);
        for (int i = 0; i < 30; ++i) { if ((readDp(4) & 0xA0000000) == 0xA0000000) break; msleep(2); }
        uint8_t c, a; std::vector<uint32_t> v;
        transfer({{0x01, 0x23000012}}, c, a, v);
        memWrite32(0xE000EDFC, 0);
        memWrite32(0xE000EDF0, 0xA05F0000);
        try { memWrite32(0xE000ED0C, 0x05FA0004); } catch (...) {}
        disconnect();
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

void LinuxCmsisDap::hardResetPulse(uint32_t pulseMs) {
    connectSwd();
    swjPins(0x00, 0x80, 0);
    msleep(pulseMs);
    swjPins(0x80, 0x80, 0);
    disconnect();
}

// ---------------------------------------------------------------------------
// Linux Serial Port (/dev/ttyACM*, /dev/ttyUSB*)
// ---------------------------------------------------------------------------
std::vector<std::string> LinuxSerialPort::enumeratePorts() {
    std::vector<std::string> out;
    DIR* d = opendir("/dev");
    if (!d) return out;
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        std::string n = ent->d_name;
        if (n.rfind("ttyACM", 0) == 0 || n.rfind("ttyUSB", 0) == 0) {
            out.push_back("/dev/" + n);
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

LinuxSerialPort::~LinuxSerialPort() {
    close();
}

static speed_t baudToSpeed(uint32_t baud) {
    switch (baud) {
        case 9600: return B9600;
        case 57600: return B57600;
        case 115200: return B115200;
        case 921600: return B921600;
        default: return B115200;
    }
}

bool LinuxSerialPort::open(const std::string& devPath, uint32_t baudRate, std::string& errOut,
                           std::function<void(const std::string&)> onRx,
                           std::function<void()> onClosed) {
    close();
    fd_ = ::open(devPath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        errOut = "Cannot open " + devPath + " (" + std::strerror(errno) + ")";
        return false;
    }
    struct termios tty{};
    if (tcgetattr(fd_, &tty) == 0) {
        cfmakeraw(&tty);
        cfsetispeed(&tty, baudToSpeed(baudRate));
        cfsetospeed(&tty, baudToSpeed(baudRate));
        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~CRTSCTS;
        tcsetattr(fd_, TCSANOW, &tty);
    }
    tcflush(fd_, TCIOFLUSH);

    running_.store(true);
    rxThread_ = std::thread([this, onRx, onClosed]() {
        char buf[512];
        while (running_.load()) {
            struct pollfd pfd{fd_, POLLIN, 0};
            int pr = ::poll(&pfd, 1, 100);
            if (!running_.load()) break;
            if (pr > 0 && (pfd.revents & POLLIN)) {
                ssize_t n = ::read(fd_, buf, sizeof(buf));
                if (n > 0 && onRx) onRx(std::string(buf, buf + n));
                else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) break;
            } else if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                break;
            }
        }
        bool was = running_.exchange(false);
        if (was && onClosed) onClosed();
    });
    return true;
}

void LinuxSerialPort::close() {
    running_.store(false);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (rxThread_.joinable()) {
        if (rxThread_.get_id() == std::this_thread::get_id()) rxThread_.detach();
        else rxThread_.join();
    }
}

bool LinuxSerialPort::writeText(const std::string& text) {
    if (!isOpen() || fd_ < 0 || text.empty()) return false;
    ssize_t w = ::write(fd_, text.data(), text.size());
    return w == static_cast<ssize_t>(text.size());
}
