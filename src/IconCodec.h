#pragma once

#include "ResourceModel.h"
#include "Result.h"

#include <cstdint>
#include <span>
#include <vector>

namespace peres {

struct IconImage {
    uint8_t width = 0;       // 0 means 256
    uint8_t height = 0;      // 0 means 256
    uint8_t colorCount = 0;
    uint8_t reserved = 0;
    uint16_t planes = 1;
    uint16_t bitCount = 32;
    std::vector<uint8_t> data;
};

struct GroupIconEntry {
    uint8_t width = 0;
    uint8_t height = 0;
    uint8_t colorCount = 0;
    uint8_t reserved = 0;
    uint16_t planes = 1;
    uint16_t bitCount = 32;
    uint16_t id = 0;
    uint32_t bytesInRes = 0;
    std::vector<uint8_t> data;
};

struct IconImageInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t bitCount = 0;
    bool png = false;
};

Result<std::vector<IconImage>> ParseIco(std::span<const uint8_t> bytes);
Result<std::vector<uint8_t>> BuildIco(std::span<const IconImage> images);
Result<std::vector<GroupIconEntry>> ParseGroupIcon(std::span<const uint8_t> bytes);
Result<std::vector<uint8_t>> BuildGroupIcon(std::span<const GroupIconEntry> entries);
Result<IconImageInfo> InspectIconData(std::span<const uint8_t> bytes);

// Wraps a single RT_ICON blob into a one-image .ico file.
Result<std::vector<uint8_t>> BuildIcoFromResource(std::span<const uint8_t> iconData);

// Extracts the whole RT_GROUP_ICON + its RT_ICON entries as a real .ico file.
Result<std::vector<uint8_t>> BuildIcoFromGroup(const ResourceModel& model,
                                               const ResourceEntry& groupEntry);

// Replaces an icon group using a complete .ico file.  New RT_ICON entries are
// created; old icons are removed only when no other group references them.
Result<void> ReplaceIconGroup(ResourceModel& model, const ResourceEntry& groupEntry,
                              std::span<const uint8_t> icoFile);

// Removes a group and its unshared RT_ICON entries.
Result<void> RemoveIconGroup(ResourceModel& model, const ResourceEntry& groupEntry);

}  // namespace peres
