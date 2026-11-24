#include "firmware_loader.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>

static std::wstring getLowerExt(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"";
    std::wstring ext = path.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext;
}

static uint8_t hexNibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    throw std::runtime_error("Invalid hex character");
}

static uint8_t hexByte(const char* p) {
    return static_cast<uint8_t>((hexNibble(p[0]) << 4) | hexNibble(p[1]));
}

static uint32_t readLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static uint16_t readLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static std::vector<uint8_t> readAllBytes(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary | std::ios::ate);
    if (!f) {
        throw std::runtime_error("Khong the mo file firmware");
    }
    std::streamsize sz = f.tellg();
    if (sz <= 0) {
        throw std::runtime_error("File firmware rong");
    }
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(buf.data()), sz)) {
        throw std::runtime_error("Loi doc file firmware");
    }
    return buf;
}

static FirmwareImage parseIntelHex(const std::wstring& path) {
    std::ifstream f(path.c_str());
    if (!f) throw std::runtime_error("Khong the mo file .hex");

    uint32_t extAddr = 0;
    std::map<uint32_t, uint8_t> chunks;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.size() < 11 || line[0] != ':') continue;
        std::vector<uint8_t> raw;
        raw.reserve((line.size() - 1) / 2);
        for (size_t i = 1; i + 1 < line.size(); i += 2) {
            raw.push_back(hexByte(&line[i]));
        }
        if (raw.size() < 5) continue;
        uint8_t len = raw[0];
        uint16_t addrLo = static_cast<uint16_t>((raw[1] << 8) | raw[2]);
        uint8_t rtype = raw[3];
        if (raw.size() < static_cast<size_t>(4 + len)) continue;

        if (rtype == 0x00) {
            uint32_t absAddr = extAddr + addrLo;
            for (uint8_t i = 0; i < len; ++i) {
                chunks[absAddr + i] = raw[4 + i];
            }
        } else if (rtype == 0x01) {
            break;
        } else if (rtype == 0x02 && len == 2) {
            extAddr = static_cast<uint32_t>((raw[4] << 8) | raw[5]) << 4;
        } else if (rtype == 0x04 && len == 2) {
            extAddr = static_cast<uint32_t>((raw[4] << 8) | raw[5]) << 16;
        }
    }

    if (chunks.empty()) {
        throw std::runtime_error("File .hex khong co du lieu Flash");
    }
    uint32_t minAddr = chunks.begin()->first;
    uint32_t maxAddr = chunks.rbegin()->first;
    FirmwareImage img;
    img.baseAddr = minAddr;
    img.data.assign(static_cast<size_t>(maxAddr - minAddr + 1), 0xFF);
    for (const auto& kv : chunks) {
        img.data[kv.first - minAddr] = kv.second;
    }
    return img;
}

static FirmwareImage parseElf32(const std::wstring& path) {
    auto elf = readAllBytes(path);
    if (elf.size() < 52 || elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F' ||
        elf[4] != 1 || elf[5] != 1) {
        throw std::runtime_error("Khong phai file ELF 32-bit little-endian hop le");
    }

    uint32_t e_phoff = readLE32(&elf[28]);
    uint16_t e_phentsize = readLE16(&elf[42]);
    uint16_t e_phnum = readLE16(&elf[44]);

    struct Seg {
        uint32_t paddr;
        std::vector<uint8_t> bytes;
    };
    std::vector<Seg> segs;

    for (uint16_t i = 0; i < e_phnum; ++i) {
        size_t off = static_cast<size_t>(e_phoff) + static_cast<size_t>(i) * e_phentsize;
        if (off + 32 > elf.size()) break;
        uint32_t p_type   = readLE32(&elf[off + 0]);
        uint32_t p_offset = readLE32(&elf[off + 4]);
        uint32_t p_paddr  = readLE32(&elf[off + 12]);
        uint32_t p_filesz = readLE32(&elf[off + 16]);

        if (p_type == 1 && p_filesz > 0 && p_paddr >= 0x08000000 && p_paddr < 0x08400000) {
            if (static_cast<size_t>(p_offset) + p_filesz <= elf.size()) {
                Seg s;
                s.paddr = p_paddr;
                s.bytes.assign(elf.begin() + p_offset, elf.begin() + p_offset + p_filesz);
                segs.push_back(std::move(s));
            }
        }
    }

    if (segs.empty()) {
        throw std::runtime_error("Khong tim thay segment Flash (0x08000000+) trong file .elf");
    }

    std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) {
        return a.paddr < b.paddr;
    });

    uint32_t base = segs.front().paddr;
    uint32_t end = base;
    for (const auto& s : segs) {
        end = (std::max)(end, s.paddr + static_cast<uint32_t>(s.bytes.size()));
    }

    FirmwareImage img;
    img.baseAddr = base;
    img.data.assign(static_cast<size_t>(end - base), 0xFF);
    for (const auto& s : segs) {
        std::copy(s.bytes.begin(), s.bytes.end(), img.data.begin() + (s.paddr - base));
    }
    return img;
}

FirmwareImage loadFirmwareImage(const std::wstring& path, uint32_t defaultBinAddr) {
    std::wstring ext = getLowerExt(path);
    if (ext == L".hex") return parseIntelHex(path);
    if (ext == L".elf") return parseElf32(path);

    auto data = readAllBytes(path);
    if (data.size() < 4) {
        throw std::runtime_error("File .bin qua nho (< 4 bytes)");
    }
    FirmwareImage img;
    img.baseAddr = defaultBinAddr;
    img.data = std::move(data);
    return img;
}
