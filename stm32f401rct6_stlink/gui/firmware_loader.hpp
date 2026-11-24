#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct FirmwareImage {
    uint32_t baseAddr = 0x08000000;
    std::vector<uint8_t> data;
};

FirmwareImage loadFirmwareImage(const std::wstring& path, uint32_t defaultBinAddr = 0x08000000);
