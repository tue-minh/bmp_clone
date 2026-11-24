#include "cmsis_dap.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace {

constexpr uint8_t ID_DAP_INFO               = 0x00;
constexpr uint8_t ID_DAP_HOST_STATUS        = 0x01;
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

inline uint16_t getLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t getLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

std::string hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

} // namespace

std::vector<uint8_t> CmsisDapClient::cmd(const std::vector<uint8_t>& payload) {
    auto resp = dev_.exchange(payload);
    if (resp.empty() || resp[0] != payload[0]) {
        throw std::runtime_error("DAP command failed or invalid response");
    }
    return resp;
}

std::string CmsisDapClient::infoStr(uint8_t infoId) {
    auto r = cmd({ID_DAP_INFO, infoId});
    uint8_t n = r.size() > 1 ? r[1] : 0;
    if (n == 0 || r.size() < static_cast<size_t>(2 + n)) return "";
    std::string s(reinterpret_cast<const char*>(&r[2]), n);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

void CmsisDapClient::hostStatus(uint8_t ledType, uint8_t status) {
    cmd({ID_DAP_HOST_STATUS, ledType, status});
}

uint8_t CmsisDapClient::connectSwd() {
    auto r = cmd({ID_DAP_CONNECT, 0x01});
    return r.size() > 1 ? r[1] : 0;
}

void CmsisDapClient::disconnect() {
    cmd({ID_DAP_DISCONNECT});
}

void CmsisDapClient::swjClock(uint32_t hz) {
    std::vector<uint8_t> req = {ID_DAP_SWJ_CLOCK};
    pushLE32(req, hz);
    cmd(req);
}

void CmsisDapClient::transferConfigure(uint8_t idle, uint16_t waitRetry, uint16_t matchRetry) {
    std::vector<uint8_t> req = {ID_DAP_TRANSFER_CONFIGURE, idle};
    pushLE16(req, waitRetry);
    pushLE16(req, matchRetry);
    cmd(req);
}

void CmsisDapClient::swdConfigure(uint8_t cfg) {
    cmd({ID_DAP_SWD_CONFIGURE, cfg});
}

void CmsisDapClient::swjSequence(uint16_t bitCount, const std::vector<uint8_t>& data) {
    uint8_t cnt = (bitCount >= 256) ? 0 : static_cast<uint8_t>(bitCount);
    std::vector<uint8_t> req = {ID_DAP_SWJ_SEQUENCE, cnt};
    req.insert(req.end(), data.begin(), data.end());
    cmd(req);
}

uint8_t CmsisDapClient::swjPins(uint8_t pinOut, uint8_t pinSel, uint32_t waitUs) {
    std::vector<uint8_t> req = {ID_DAP_SWJ_PINS, pinOut, pinSel};
    pushLE32(req, waitUs);
    auto r = cmd(req);
    return r.size() > 1 ? r[1] : 0;
}

uint8_t CmsisDapClient::resetTarget() {
    auto r = cmd({ID_DAP_RESET_TARGET});
    return r.size() > 2 ? r[2] : 0;
}

void CmsisDapClient::writeAbort(uint32_t val) {
    std::vector<uint8_t> req = {ID_DAP_WRITE_ABORT, 0x00};
    pushLE32(req, val);
    cmd(req);
}

void CmsisDapClient::transfer(const std::vector<std::pair<uint8_t, uint32_t>>& ops,
                              uint8_t& countOut, uint8_t& ackOut, std::vector<uint32_t>& valsOut) {
    std::vector<uint8_t> req = {ID_DAP_TRANSFER, 0x00, static_cast<uint8_t>(ops.size())};
    size_t readCount = 0;
    for (const auto& op : ops) {
        req.push_back(op.first);
        if ((op.first & 0x02) == 0) {
            pushLE32(req, op.second);
        } else {
            readCount++;
        }
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

void CmsisDapClient::transferBlockWrite(uint8_t reqByte, const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); i += 14) {
        size_t cnt = (std::min)(size_t(14), words.size() - i);
        std::vector<uint8_t> req = {ID_DAP_TRANSFER_BLOCK, 0x00};
        pushLE16(req, static_cast<uint16_t>(cnt));
        req.push_back(reqByte & ~0x02);
        for (size_t j = 0; j < cnt; ++j) {
            pushLE32(req, words[i + j]);
        }
        auto r = cmd(req);
        uint8_t ack = r.size() > 3 ? r[3] : 0;
        if (ack != 1) {
            throw std::runtime_error("DAP_TransferBlock write failed (ACK=" + std::to_string(ack) + ")");
        }
    }
}

std::vector<uint32_t> CmsisDapClient::transferBlockRead(uint8_t reqByte, uint32_t count) {
    std::vector<uint32_t> out;
    out.reserve(count);
    uint32_t rem = count;
    while (rem > 0) {
        uint32_t n = (std::min)(rem, uint32_t(15));
        std::vector<uint8_t> req = {ID_DAP_TRANSFER_BLOCK, 0x00};
        pushLE16(req, static_cast<uint16_t>(n));
        req.push_back(reqByte | 0x02);
        auto r = cmd(req);
        uint16_t gotCnt = r.size() >= 3 ? getLE16(&r[1]) : 0;
        uint8_t ack = r.size() > 3 ? r[3] : 0;
        if (ack != 1 || gotCnt != n || r.size() < 4 + n * 4) {
            throw std::runtime_error("DAP_TransferBlock read failed (ACK=" + std::to_string(ack) + ")");
        }
        for (uint32_t j = 0; j < n; ++j) {
            out.push_back(getLE32(&r[4 + j * 4]));
        }
        rem -= n;
    }
    return out;
}

void CmsisDapClient::swdLineResetToSwd() {
    // 1. Line reset (56 clocks HIGH) + JTAG-to-SWD (0x9E, 0xE7) + Line reset (56 clocks HIGH)
    swjSequence(56, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    swjSequence(16, {0x9E, 0xE7});
    swjSequence(56, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    swjSequence(8, {0x00});

    // 2. ARM ADIv5.2 Dormant-to-SWD sequence (for STM32G4, STM32L4+, STM32G0)
    static const uint8_t s_dormantSeq[] = {
        0xFF,
        0x92, 0xF3, 0x09, 0x62, 0x95, 0x2D, 0x85, 0x86,
        0xE9, 0xAF, 0xDD, 0xE3, 0xA2, 0x0E, 0xBC, 0x19,
        0xA0, 0xF1,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    swjSequence(sizeof(s_dormantSeq) * 8, std::vector<uint8_t>(s_dormantSeq, s_dormantSeq + sizeof(s_dormantSeq)));
    swjSequence(8, {0x00});
}

uint32_t CmsisDapClient::readDp(uint8_t addr) {
    uint8_t cnt = 0, ack = 0;
    std::vector<uint32_t> vals;
    transfer({{static_cast<uint8_t>(0x02 | (addr & 0x0C)), 0}}, cnt, ack, vals);
    if (ack != 1 || vals.empty()) {
        throw std::runtime_error("SWD DP read failed (ACK=" + std::to_string(ack) + ")");
    }
    return vals[0];
}

void CmsisDapClient::writeDp(uint8_t addr, uint32_t val) {
    uint8_t cnt = 0, ack = 0;
    std::vector<uint32_t> vals;
    transfer({{static_cast<uint8_t>(0x00 | (addr & 0x0C)), val}}, cnt, ack, vals);
    if (ack != 1) {
        throw std::runtime_error("SWD DP write failed (ACK=" + std::to_string(ack) + ")");
    }
}

uint32_t CmsisDapClient::memRead32(uint32_t addr) {
    uint8_t cnt = 0, ack = 0;
    std::vector<uint32_t> vals;
    transfer({{0x01 | 0x04, addr}, {0x01 | 0x02 | 0x0C, 0}}, cnt, ack, vals);
    if (ack != 1 || vals.empty()) {
        throw std::runtime_error("SWD memRead32 " + hex32(addr) + " failed (ACK=" + std::to_string(ack) + ")");
    }
    return vals[0];
}

void CmsisDapClient::memWrite32(uint32_t addr, uint32_t val) {
    uint8_t cnt = 0, ack = 0;
    std::vector<uint32_t> vals;
    transfer({{0x01 | 0x04, addr}, {0x01 | 0x0C, val}}, cnt, ack, vals);
    if (ack != 1) {
        throw std::runtime_error("SWD memWrite32 " + hex32(addr) + " failed (ACK=" + std::to_string(ack) + ")");
    }
}

void CmsisDapClient::memWrite16Batch(uint32_t startAddr, const std::vector<uint16_t>& halfwords) {
    uint8_t cnt = 0, ack = 0;
    std::vector<uint32_t> vals;
    transfer({{0x01 | 0x00, 0x23000011}}, cnt, ack, vals); // 16-bit auto-inc
    if (ack != 1) throw std::runtime_error("AP_CSW 16-bit write failed");

    try {
        uint32_t addr = startAddr;
        for (size_t i = 0; i < halfwords.size(); i += 6) {
            size_t n = (std::min)(size_t(6), halfwords.size() - i);
            std::vector<std::pair<uint8_t, uint32_t>> ops;
            ops.reserve(n * 2);
            for (size_t j = 0; j < n; ++j) {
                uint16_t hw = halfwords[i + j];
                uint32_t packed = (addr & 2) ? (static_cast<uint32_t>(hw) << 16) : hw;
                ops.emplace_back(0x01 | 0x04, addr);
                ops.emplace_back(0x01 | 0x0C, packed);
                addr += 2;
            }
            transfer(ops, cnt, ack, vals);
            if (ack != 1) {
                throw std::runtime_error("SWD write16 batch failed near " + hex32(addr));
            }
        }
    } catch (...) {
        try { transfer({{0x01 | 0x00, 0x23000012}}, cnt, ack, vals); } catch (...) {}
        throw;
    }
    transfer({{0x01 | 0x00, 0x23000012}}, cnt, ack, vals);
}

void CmsisDapClient::memWriteBlock(uint32_t addr, const std::vector<uint32_t>& words) {
    size_t idx = 0;
    size_t total = words.size();
    while (idx < total) {
        uint32_t room = (0x400 - (addr & 0x3FF)) / 4;
        size_t n = (std::min)(total - idx, static_cast<size_t>(room));
        uint8_t cnt = 0, ack = 0;
        std::vector<uint32_t> vals;
        transfer({{0x01 | 0x04, addr}}, cnt, ack, vals);
        if (ack != 1) throw std::runtime_error("AP_TAR write failed at " + hex32(addr));
        std::vector<uint32_t> chunk(words.begin() + idx, words.begin() + idx + n);
        transferBlockWrite(0x01 | 0x0C, chunk);
        addr += static_cast<uint32_t>(n) * 4;
        idx += n;
    }
}

std::vector<uint32_t> CmsisDapClient::memReadBlock(uint32_t addr, uint32_t count) {
    std::vector<uint32_t> out;
    out.reserve(count);
    while (count > 0) {
        uint32_t room = (0x400 - (addr & 0x3FF)) / 4;
        uint32_t n = (std::min)(count, room);
        uint8_t cnt = 0, ack = 0;
        std::vector<uint32_t> vals;
        transfer({{0x01 | 0x04, addr}}, cnt, ack, vals);
        if (ack != 1) throw std::runtime_error("AP_TAR write failed at " + hex32(addr));
        auto chunk = transferBlockRead(0x01 | 0x02 | 0x0C, n);
        out.insert(out.end(), chunk.begin(), chunk.end());
        addr += n * 4;
        count -= n;
    }
    return out;
}

uint32_t CmsisDapClient::attachAndHalt(uint32_t clockHz, bool connectUnderReset) {
    if (connectSwd() != 1) {
        throw std::runtime_error("DAP_Connect(SWD) failed");
    }
    swjClock(clockHz);
    transferConfigure(2, 500, 0);
    swdConfigure(0);

    if (connectUnderReset) {
        swjPins(0x00, 0x80, 0);
        Sleep(15);
    }

    uint32_t dpId = 0;
    for (int retry = 0; retry < 3; ++retry) {
        try {
            swdLineResetToSwd();
            dpId = readDp(0x00);
            if (dpId != 0 && dpId != 0xFFFFFFFF) break;
        } catch (...) {
            if (retry == 2) throw;
            Sleep(10);
        }
    }
    writeAbort(0x1E);
    writeDp(0x08, 0x00000000);
    writeDp(0x04, 0x50000000);

    bool powered = false;
    for (int i = 0; i < 50; ++i) {
        if ((readDp(0x04) & 0xA0000000) == 0xA0000000) {
            powered = true;
            break;
        }
        Sleep(2);
    }
    if (!powered) {
        throw std::runtime_error("Debug power-up timeout (kiem tra day SWD / nguon target)");
    }

    uint8_t cnt = 0, ack = 0;
    std::vector<uint32_t> vals;
    transfer({{0x01 | 0x00, 0x23000012}}, cnt, ack, vals);
    if (ack != 1) throw std::runtime_error("AP_CSW write failed");

    if (connectUnderReset) {
        memWrite32(0xE000EDF0, 0xA05F0003);
        memWrite32(0xE000EDFC, 0x00000001);
        swjPins(0x80, 0x80, 0);
        Sleep(20);
    } else {
        memWrite32(0xE000EDF0, 0xA05F0003);
    }

    bool halted = false;
    for (int i = 0; i < 100; ++i) {
        if (memRead32(0xE000EDF0) & (1U << 17)) {
            halted = true;
            break;
        }
        Sleep(2);
    }
    if (!halted) throw std::runtime_error("CPU halt timeout");

    if (connectUnderReset) {
        memWrite32(0xE000EDFC, 0x00000000);
    }
    return dpId;
}

Stm32TargetInfo CmsisDapClient::identifyHaltedTarget(uint32_t dpId) {
    uint32_t dbgId = memRead32(0xE0042000);
    if ((dbgId & 0xFFF) == 0) {
        dbgId = memRead32(0x40015800);
    }
    uint16_t devId = static_cast<uint16_t>(dbgId & 0xFFF);
    uint16_t revId = static_cast<uint16_t>((dbgId >> 16) & 0xFFFF);

    Stm32TargetInfo info;
    info.dpId = dpId;
    info.dbgId = dbgId;
    info.devId = devId;
    info.revId = revId;

    const auto& tbl = getDeviceTable();
    auto it = tbl.find(devId);
    if (it != tbl.end()) {
        info.family = it->second.family;
        info.pageSize = it->second.pageSize;
        info.name = it->second.name;
    } else {
        info.family = "?";
        info.pageSize = 2048;
        info.name = "Unknown STM32 / Cortex-M";
    }

    uint32_t uidBase = 0;
    if (info.family == "F1" || info.family == "F3") {
        info.flashKB = memRead32(0x1FFFF7E0) & 0xFFFF;
        uint32_t obr = memRead32(0x4002201C);
        info.rdp = (obr & 2) ? 0xBB : 0xAA;
        uidBase = 0x1FFFF7E8;
    } else if (info.family == "F2" || info.family == "F4") {
        info.flashKB = (memRead32(0x1FFF7A20) >> 16) & 0xFFFF;
        uint32_t optcr = memRead32(0x40023C14);
        info.rdp = static_cast<int>((optcr >> 8) & 0xFF);
        uidBase = 0x1FFF7A10;
    } else if (info.family == "G4" || info.family == "L4") {
        info.flashKB = memRead32(0x1FFF75E0) & 0xFFFF;
        uint32_t optr = memRead32(0x40022020);
        info.rdp = static_cast<int>(optr & 0xFF);
        uidBase = 0x1FFF7590;
    } else if (info.family == "F0") {
        info.flashKB = memRead32(0x1FFFF7CC) & 0xFFFF;
        uidBase = 0x1FFFF7AC;
    } else if (info.family == "G0") {
        info.flashKB = memRead32(0x1FFF75E0) & 0xFFFF;
        uidBase = 0x1FFF7590;
    }

    if (uidBase != 0) {
        uint32_t u0 = memRead32(uidBase);
        uint32_t u1 = memRead32(uidBase + 4);
        uint32_t u2 = memRead32(uidBase + 8);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%08X-%08X-%08X", u2, u1, u0);
        info.uid = buf;
    } else {
        info.uid = "N/A";
    }

    if (info.flashKB == 0xFFFF) info.flashKB = 0;

    if (info.family != "F0" && info.family != "G0") {
        uint32_t fz = memRead32(0xE0042008);
        memWrite32(0xE0042008, fz | (1U << 11) | (1U << 12));
    }
    return info;
}

Stm32TargetInfo CmsisDapClient::probeTarget(uint32_t clockHz, bool connectUnderReset, bool readPreview) {
    try {
        uint32_t dpId = attachAndHalt(clockHz, connectUnderReset);
        Stm32TargetInfo info = identifyHaltedTarget(dpId);
        if (readPreview) {
            try {
                auto words = memReadBlock(0x08000000, 128); // 512 bytes
                info.flashPreview.resize(words.size() * 4);
                for (size_t i = 0; i < words.size(); ++i) {
                    uint32_t w = words[i];
                    info.flashPreview[i * 4 + 0] = static_cast<uint8_t>(w & 0xFF);
                    info.flashPreview[i * 4 + 1] = static_cast<uint8_t>((w >> 8) & 0xFF);
                    info.flashPreview[i * 4 + 2] = static_cast<uint8_t>((w >> 16) & 0xFF);
                    info.flashPreview[i * 4 + 3] = static_cast<uint8_t>((w >> 24) & 0xFF);
                }
            } catch (...) {}
        }
        memWrite32(0xE000EDF0, 0xA05F0001); // Resume
        disconnect();
        return info;
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

void CmsisDapClient::waitFlashReady(uint32_t srAddr, uint32_t bsyMask, uint32_t errMask, uint32_t eopMask, uint32_t timeoutMs) {
    ULONGLONG t0 = GetTickCount64();
    while (true) {
        uint32_t sr = memRead32(srAddr);
        if (sr & errMask) {
            memWrite32(srAddr, errMask | eopMask);
            throw std::runtime_error("Flash controller error SR=" + hex32(sr));
        }
        if ((sr & bsyMask) == 0) return;
        if (GetTickCount64() - t0 > timeoutMs) {
            throw std::runtime_error("Flash timeout SR=" + hex32(sr));
        }
        Sleep(2);
    }
}

void CmsisDapClient::flashF1F3(uint32_t baseAddr, const std::vector<uint8_t>& padded, uint32_t pageSz, ProgressCallback cb) {
    const uint32_t CR = 0x40022010, SR = 0x4002200C, KEYR = 0x40022004, AR = 0x40022014;
    if (memRead32(CR) & (1U << 7)) {
        memWrite32(KEYR, FLASH_KEY1);
        memWrite32(KEYR, FLASH_KEY2);
        if (memRead32(CR) & (1U << 7)) throw std::runtime_error("F1/F3 Flash unlock failed");
    }
    try {
        uint32_t startPage = (baseAddr - 0x08000000) / pageSz;
        uint32_t endPage = (baseAddr + static_cast<uint32_t>(padded.size()) - 1 - 0x08000000) / pageSz;
        uint32_t numPages = endPage - startPage + 1;
        for (uint32_t idx = 0; idx < numPages; ++idx) {
            if (cb) cb("erase", idx, numPages);
            waitFlashReady(SR, 1, 0x14, 0x20, 500);
            memWrite32(CR, 0x02);
            memWrite32(AR, 0x08000000 + (startPage + idx) * pageSz);
            memWrite32(CR, 0x42);
            waitFlashReady(SR, 1, 0x14, 0x20, 1000);
        }
        if (cb) cb("erase", numPages, numPages);

        memWrite32(CR, 0x01);
        std::vector<uint16_t> hwAll(padded.size() / 2);
        for (size_t i = 0; i < hwAll.size(); ++i) {
            hwAll[i] = getLE16(&padded[i * 2]);
        }
        const size_t batchHw = 128;
        for (size_t off = 0; off < hwAll.size(); off += batchHw) {
            size_t cnt = (std::min)(batchHw, hwAll.size() - off);
            std::vector<uint16_t> chunk(hwAll.begin() + off, hwAll.begin() + off + cnt);
            memWrite16Batch(baseAddr + static_cast<uint32_t>(off * 2), chunk);
            waitFlashReady(SR, 1, 0x14, 0x20, 500);
            if (cb) cb("program", static_cast<uint32_t>((off + cnt) * 2), static_cast<uint32_t>(padded.size()));
        }
        memWrite32(CR, 1U << 7);
    } catch (...) {
        try { memWrite32(CR, 1U << 7); } catch (...) {}
        throw;
    }
}

void CmsisDapClient::flashF2F4(uint32_t baseAddr, const std::vector<uint8_t>& padded, ProgressCallback cb) {
    const uint32_t CR = 0x40023C10, SR = 0x40023C0C, KEYR = 0x40023C04;
    if (memRead32(CR) & (1U << 31)) {
        memWrite32(KEYR, FLASH_KEY1);
        memWrite32(KEYR, FLASH_KEY2);
        if (memRead32(CR) & (1U << 31)) throw std::runtime_error("F4 Flash unlock failed");
    }
    try {
        static const uint32_t sectorSizes[12] = {
            16384, 16384, 16384, 16384, 65536,
            131072, 131072, 131072, 131072, 131072, 131072, 131072
        };
        std::vector<uint32_t> sectors;
        uint32_t cur = 0x08000000;
        uint32_t imgEnd = baseAddr + static_cast<uint32_t>(padded.size());
        for (uint32_t s = 0; s < 12; ++s) {
            uint32_t sz = sectorSizes[s];
            if (cur + sz > baseAddr && cur < imgEnd) sectors.push_back(s);
            cur += sz;
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
        const size_t step = 128;
        for (size_t off = 0; off < words.size(); off += step) {
            size_t cnt = (std::min)(step, words.size() - off);
            std::vector<uint32_t> chunk(words.begin() + off, words.begin() + off + cnt);
            memWriteBlock(baseAddr + static_cast<uint32_t>(off * 4), chunk);
            waitFlashReady(SR, 1U << 16, 0x1F2, 1, 500);
            if (cb) cb("program", static_cast<uint32_t>((off + cnt) * 4), static_cast<uint32_t>(padded.size()));
        }
        memWrite32(CR, 1U << 31);
    } catch (...) {
        try { memWrite32(CR, 1U << 31); } catch (...) {}
        throw;
    }
}

void CmsisDapClient::flashG4L4(uint32_t baseAddr, const std::vector<uint8_t>& padded, uint32_t pageSz, ProgressCallback cb) {
    const uint32_t CR = 0x40022014, SR = 0x40022010, KEYR = 0x40022008;
    if (memRead32(CR) & (1U << 31)) {
        memWrite32(KEYR, FLASH_KEY1);
        memWrite32(KEYR, FLASH_KEY2);
        if (memRead32(CR) & (1U << 31)) throw std::runtime_error("G4/L4 Flash unlock failed");
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
        const size_t step = 128;
        for (size_t off = 0; off < words.size(); off += step) {
            size_t cnt = (std::min)(step, words.size() - off);
            std::vector<uint32_t> chunk(words.begin() + off, words.begin() + off + cnt);
            memWriteBlock(baseAddr + static_cast<uint32_t>(off * 4), chunk);
            waitFlashReady(SR, 1U << 16, 0x3FA, 1, 500);
            if (cb) cb("program", static_cast<uint32_t>((off + cnt) * 4), static_cast<uint32_t>(padded.size()));
        }
        memWrite32(CR, 1U << 31);
    } catch (...) {
        try { memWrite32(CR, 1U << 31); } catch (...) {}
        throw;
    }
}

Stm32TargetInfo CmsisDapClient::flashImage(uint32_t baseAddr, const std::vector<uint8_t>& data,
                                           uint32_t clockHz, bool connectUnderReset,
                                           bool verify, ProgressCallback cb) {
    try {
        uint32_t dpId = attachAndHalt(clockHz, connectUnderReset);
        Stm32TargetInfo info = identifyHaltedTarget(dpId);
        if (info.rdp >= 0 && info.rdp != 0xAA) {
            throw std::runtime_error("Chip dang bat Read-Out Protection (RDP)");
        }
        if (info.family != "F1" && info.family != "F3" &&
            info.family != "F2" && info.family != "F4" &&
            info.family != "G4" && info.family != "L4") {
            throw std::runtime_error("Chua ho tro nap truc tiep dong " + info.name);
        }
        if (info.flashKB > 0) {
            uint32_t flashEnd = 0x08000000 + info.flashKB * 1024;
            if (baseAddr + data.size() > flashEnd) {
                throw std::runtime_error("Firmware vuot qua dung luong Flash chip");
            }
        }

        std::vector<uint8_t> padded = data;
        while (padded.size() % 8 != 0) padded.push_back(0xFF);
        uint32_t totalLen = static_cast<uint32_t>(padded.size());

        if (info.family == "F1" || info.family == "F3") {
            flashF1F3(baseAddr, padded, info.pageSize, cb);
        } else if (info.family == "F2" || info.family == "F4") {
            flashF2F4(baseAddr, padded, cb);
        } else {
            flashG4L4(baseAddr, padded, info.pageSize, cb);
        }

        if (verify) {
            uint32_t totalWords = totalLen / 4;
            const uint32_t step = 256;
            for (uint32_t offW = 0; offW < totalWords; offW += step) {
                uint32_t cnt = (std::min)(step, totalWords - offW);
                auto got = memReadBlock(baseAddr + offW * 4, cnt);
                for (uint32_t i = 0; i < cnt; ++i) {
                    uint32_t want = getLE32(&padded[(offW + i) * 4]);
                    if (got[i] != want) {
                        throw std::runtime_error("Verify mismatch @ " + hex32(baseAddr + (offW + i) * 4));
                    }
                }
                if (cb) cb("verify", (std::min)((offW + cnt) * 4, totalLen), totalLen);
            }
        }

        memWrite32(0xE000EDFC, 0x00000000);
        memWrite32(0xE000EDF0, 0xA05F0001);
        try { memWrite32(0xE000ED0C, 0x05FA0004); } catch (...) {}
        Sleep(30);
        resetTarget();
        disconnect();
        return info;
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

VerifyResult CmsisDapClient::verifyImage(uint32_t baseAddr, const std::vector<uint8_t>& data,
                                         uint32_t clockHz, bool connectUnderReset,
                                         ProgressCallback cb) {
    try {
        uint32_t dpId = attachAndHalt(clockHz, connectUnderReset);
        VerifyResult res;
        res.info = identifyHaltedTarget(dpId);

        std::vector<uint8_t> padded = data;
        while (padded.size() % 4 != 0) padded.push_back(0xFF);
        uint32_t totalWords = static_cast<uint32_t>(padded.size() / 4);
        const uint32_t step = 256;
        for (uint32_t offW = 0; offW < totalWords; offW += step) {
            uint32_t cnt = (std::min)(step, totalWords - offW);
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
            if (cb) cb("verify", (std::min)((offW + cnt) * 4, static_cast<uint32_t>(padded.size())),
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

Stm32TargetInfo CmsisDapClient::massErase(uint32_t clockHz, bool connectUnderReset) {
    try {
        uint32_t dpId = attachAndHalt(clockHz, connectUnderReset);
        Stm32TargetInfo info = identifyHaltedTarget(dpId);
        if (info.rdp >= 0 && info.rdp != 0xAA) {
            throw std::runtime_error("Chip dang bat Read-Out Protection (RDP)");
        }
        if (info.family == "F1" || info.family == "F3") {
            const uint32_t CR = 0x40022010, SR = 0x4002200C, KEYR = 0x40022004;
            if (memRead32(CR) & (1U << 7)) {
                memWrite32(KEYR, FLASH_KEY1);
                memWrite32(KEYR, FLASH_KEY2);
            }
            waitFlashReady(SR, 1, 0x14, 0x20, 1000);
            memWrite32(CR, 1U << 2);
            memWrite32(CR, (1U << 2) | (1U << 6));
            waitFlashReady(SR, 1, 0x14, 0x20, 5000);
            memWrite32(CR, 1U << 7);
        } else if (info.family == "F2" || info.family == "F4") {
            const uint32_t CR = 0x40023C10, SR = 0x40023C0C, KEYR = 0x40023C04;
            if (memRead32(CR) & (1U << 31)) {
                memWrite32(KEYR, FLASH_KEY1);
                memWrite32(KEYR, FLASH_KEY2);
            }
            waitFlashReady(SR, 1U << 16, 0x1F2, 1, 1000);
            memWrite32(SR, 0x1F3);
            memWrite32(CR, (1U << 2) | (2U << 8));
            memWrite32(CR, (1U << 2) | (2U << 8) | (1U << 16));
            waitFlashReady(SR, 1U << 16, 0x1F2, 1, 15000);
            memWrite32(CR, 1U << 31);
        } else if (info.family == "G4" || info.family == "L4") {
            const uint32_t CR = 0x40022014, SR = 0x40022010, KEYR = 0x40022008;
            if (memRead32(CR) & (1U << 31)) {
                memWrite32(KEYR, FLASH_KEY1);
                memWrite32(KEYR, FLASH_KEY2);
            }
            waitFlashReady(SR, 1U << 16, 0x3FA, 1, 1000);
            memWrite32(SR, 0x3FB);
            memWrite32(CR, 1U << 2);
            memWrite32(CR, (1U << 2) | (1U << 16));
            waitFlashReady(SR, 1U << 16, 0x3FA, 1, 10000);
            memWrite32(CR, 1U << 31);
        } else {
            throw std::runtime_error("Chua ho tro Mass Erase cho " + info.name);
        }
        disconnect();
        return info;
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

std::vector<uint8_t> CmsisDapClient::readMemoryBytes(uint32_t addr, uint32_t length,
                                                     uint32_t clockHz, bool connectUnderReset,
                                                     ProgressCallback cb) {
    try {
        attachAndHalt(clockHz, connectUnderReset);
        uint32_t alignedStart = addr & ~3U;
        uint32_t alignedEnd = (addr + length + 3U) & ~3U;
        uint32_t totalWords = (alignedEnd - alignedStart) / 4U;

        std::vector<uint8_t> raw;
        raw.reserve(totalWords * 4U);
        const uint32_t step = 256;
        for (uint32_t offW = 0; offW < totalWords; offW += step) {
            uint32_t cnt = (std::min)(step, totalWords - offW);
            auto chunk = memReadBlock(alignedStart + offW * 4, cnt);
            for (uint32_t w : chunk) {
                raw.push_back(static_cast<uint8_t>(w & 0xFF));
                raw.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
                raw.push_back(static_cast<uint8_t>((w >> 16) & 0xFF));
                raw.push_back(static_cast<uint8_t>((w >> 24) & 0xFF));
            }
            if (cb) cb("read", (std::min)((offW + cnt) * 4, totalWords * 4), totalWords * 4);
        }
        memWrite32(0xE000EDF0, 0xA05F0001);
        disconnect();

        uint32_t skip = addr - alignedStart;
        std::vector<uint8_t> res(raw.begin() + skip, raw.begin() + skip + length);
        return res;
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

uint32_t CmsisDapClient::readCoreReg(uint8_t regIdx) {
    memWrite32(0xE000EDF4, regIdx & 0x1F);
    for (int i = 0; i < 30; ++i) {
        if (memRead32(0xE000EDF0) & (1U << 16)) {
            return memRead32(0xE000EDF8);
        }
    }
    throw std::runtime_error("DCRSR reg timeout");
}

CpuDebugStatus CmsisDapClient::readCpuStatus(uint32_t clockHz, bool connectUnderReset) {
    try {
        uint32_t dpId = attachAndHalt(clockHz, connectUnderReset);
        CpuDebugStatus st;
        st.info = identifyHaltedTarget(dpId);
        st.halted = (memRead32(0xE000EDF0) & (1U << 17)) != 0;
        st.r0   = readCoreReg(0);
        st.r1   = readCoreReg(1);
        st.r2   = readCoreReg(2);
        st.r3   = readCoreReg(3);
        st.r12  = readCoreReg(12);
        st.sp   = readCoreReg(13);
        st.lr   = readCoreReg(14);
        st.pc   = readCoreReg(15);
        st.xpsr = readCoreReg(16);
        st.msp  = readCoreReg(17);
        st.psp  = readCoreReg(18);
        st.cfsr = memRead32(0xE000ED28);
        st.hfsr = memRead32(0xE000ED2C);
        disconnect();
        return st;
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

CpuDebugStatus CmsisDapClient::debugStart(uint32_t clockHz, bool connectUnderReset) {
    return readCpuStatus(clockHz, connectUnderReset);
}

void CmsisDapClient::debugExit(uint32_t clockHz) {
    if (connectSwd() != 1) throw std::runtime_error("DAP_Connect(SWD) failed");
    try {
        swjClock(clockHz);
        transferConfigure(2, 500, 0);
        swdConfigure(0);
        swdLineResetToSwd();
        readDp(0x00);
        writeAbort(0x1E);
        writeDp(0x08, 0);
        writeDp(0x04, 0x50000000);
        for (int i = 0; i < 30; ++i) {
            if ((readDp(0x04) & 0xA0000000) == 0xA0000000) break;
            Sleep(2);
        }
        uint8_t cnt = 0, ack = 0;
        std::vector<uint32_t> vals;
        transfer({{0x01 | 0x00, 0x23000012}}, cnt, ack, vals);
        try { memWrite32(0xE0002008, 0x00000000); } catch (...) {} // Clear FPB COMP0
        try { memWrite32(0xE0002000, 0x00000002); } catch (...) {} // Disable FPB
        memWrite32(0xE000EDFC, 0x00000000); // Clear DEMCR
        memWrite32(0xE000EDF0, 0xA05F0000); // Clear C_DEBUGEN & C_HALT -> free run
        disconnect();
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

std::string CmsisDapClient::cpuControl(const std::string& action, uint32_t clockHz) {
    if (connectSwd() != 1) throw std::runtime_error("DAP_Connect(SWD) failed");
    try {
        swjClock(clockHz);
        transferConfigure(2, 500, 0);
        swdConfigure(0);
        swdLineResetToSwd();
        readDp(0x00);
        writeAbort(0x1E);
        writeDp(0x08, 0);
        writeDp(0x04, 0x50000000);
        for (int i = 0; i < 30; ++i) {
            if ((readDp(0x04) & 0xA0000000) == 0xA0000000) break;
            Sleep(2);
        }
        uint8_t cnt = 0, ack = 0;
        std::vector<uint32_t> vals;
        transfer({{0x01 | 0x00, 0x23000012}}, cnt, ack, vals);

        std::string msg;
        if (action == "halt") {
            memWrite32(0xE000EDF0, 0xA05F0003);
            msg = "CPU HALTED";
        } else if (action == "run") {
            memWrite32(0xE000EDF0, 0xA05F0001);
            msg = "CPU RUNNING";
        } else if (action == "step_into" || action == "step") {
            memWrite32(0xE000EDF0, 0xA05F0003);
            memWrite32(0xE000EDF0, 0xA05F0005);
            uint32_t pc = readCoreReg(15);
            msg = "STEP INTO -> PC=" + hex32(pc);
        } else if (action == "step_out") {
            memWrite32(0xE000EDF0, 0xA05F0003);
            uint32_t lr = readCoreReg(14);
            uint32_t retAddr = lr & ~1U;
            if (retAddr < 0x08000000U || retAddr >= 0x20000000U) {
                throw std::runtime_error("Cannot Step Out: LR (" + hex32(lr) + ") is not a Flash return address");
            }
            uint32_t compVal = (retAddr & 0x1FFFFFFCU) | 1U | ((retAddr & 2U) ? (1U << 31) : (1U << 30));
            memWrite32(0xE0002000, 0x00000003); // FP_CTRL: KEY | ENABLE
            memWrite32(0xE0002008, compVal);    // FP_COMP0
            memWrite32(0xE000EDF0, 0xA05F0001); // Run until return breakpoint
            bool hit = false;
            ULONGLONG t0 = GetTickCount64();
            while (GetTickCount64() - t0 < 1500) {
                if (memRead32(0xE000EDF0) & (1U << 17)) {
                    hit = true;
                    break;
                }
                Sleep(5);
            }
            if (!hit) memWrite32(0xE000EDF0, 0xA05F0003);
            memWrite32(0xE0002008, 0x00000000);
            memWrite32(0xE0002000, 0x00000002);
            uint32_t pc = readCoreReg(15);
            msg = hit ? ("STEP OUT -> PC=" + hex32(pc)) : ("STEP OUT timeout (halted @ PC=" + hex32(pc) + ")");
        } else {
            throw std::runtime_error("Unknown CPU action");
        }
        disconnect();
        return msg;
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

void CmsisDapClient::swdSystemReset(uint32_t clockHz) {
    if (connectSwd() != 1) throw std::runtime_error("DAP_Connect(SWD) failed");
    try {
        swjClock(clockHz);
        transferConfigure(2, 500, 0);
        swdConfigure(0);
        swdLineResetToSwd();
        readDp(0x00);
        writeAbort(0x1E);
        writeDp(0x08, 0);
        writeDp(0x04, 0x50000000);
        for (int i = 0; i < 30; ++i) {
            if ((readDp(0x04) & 0xA0000000) == 0xA0000000) break;
            Sleep(2);
        }
        uint8_t cnt = 0, ack = 0;
        std::vector<uint32_t> vals;
        transfer({{0x01 | 0x00, 0x23000012}}, cnt, ack, vals);
        memWrite32(0xE000EDFC, 0x00000000); // Clear DEMCR
        memWrite32(0xE000EDF0, 0xA05F0000); // Clear C_DEBUGEN
        try { memWrite32(0xE000ED0C, 0x05FA0004); } catch (...) {} // AIRCR.SYSRESETREQ
        disconnect();
    } catch (...) {
        try { disconnect(); } catch (...) {}
        throw;
    }
}

void CmsisDapClient::hardResetPulse(uint32_t pulseMs) {
    connectSwd();
    swjPins(0x00, 0x80, 0);
    Sleep(pulseMs);
    swjPins(0x80, 0x80, 0);
    disconnect();
}
