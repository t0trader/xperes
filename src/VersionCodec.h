#pragma once

#include "Result.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace peres {

struct VersionFixedInfo {
    bool present = false;
    uint32_t signature = 0;
    uint32_t structureVersion = 0;
    uint32_t fileVersionMs = 0;
    uint32_t fileVersionLs = 0;
    uint32_t productVersionMs = 0;
    uint32_t productVersionLs = 0;
    uint32_t fileFlagsMask = 0;
    uint32_t fileFlags = 0;
    uint32_t fileOs = 0;
    uint32_t fileType = 0;
    uint32_t fileSubtype = 0;
    uint32_t fileDateMs = 0;
    uint32_t fileDateLs = 0;
};

struct VersionString {
    std::wstring key;
    std::wstring value;
};

struct VersionStringTable {
    std::wstring key;
    std::vector<VersionString> strings;
};

struct VersionTranslation {
    uint16_t language = 0;
    uint16_t codePage = 0;
};

struct VersionInfo {
    VersionFixedInfo fixed;
    std::vector<VersionStringTable> stringTables;
    std::vector<VersionTranslation> translations;
};

Result<VersionInfo> ParseVersionResource(std::span<const uint8_t> data);
std::wstring FormatVersionText(const VersionInfo& info);
std::wstring FormatVersionDwords(uint32_t ms, uint32_t ls);

}  // namespace peres
