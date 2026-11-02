#include "IconCodec.h"

#include "ResourceTypes.h"
#include "TextUtil.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <set>

namespace peres {

namespace {

constexpr uint16_t kGroupIconType = 14;
constexpr uint16_t kIconType = 3;
constexpr uint32_t kMaxIconImages = 4096;
constexpr uint32_t kMaxIconBytes = 512u * 1024u * 1024u;

uint16_t U16(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

uint32_t U32(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

uint32_t BigU32(std::span<const uint8_t> bytes, size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

void Put16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value & 0xFF);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value & 0xFF);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    bytes[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    bytes[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

bool IsPng(std::span<const uint8_t> bytes) {
    static constexpr uint8_t kPngSignature[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    return bytes.size() >= 8 && std::memcmp(bytes.data(), kPngSignature, 8) == 0;
}

Result<IconImageInfo> InspectPng(std::span<const uint8_t> bytes) {
    if (bytes.size() < 33) {
        return MakeError(L"PNG 图标数据过短");
    }
    const uint32_t width = BigU32(bytes, 16);
    const uint32_t height = BigU32(bytes, 20);
    const uint8_t bitDepth = bytes[24];
    const uint8_t colorType = bytes[25];
    if (width == 0 || height == 0) {
        return MakeError(L"PNG 图标尺寸无效");
    }
    uint32_t bitCount = bitDepth;
    switch (colorType) {
        case 2:  // truecolor
            bitCount = bitDepth * 3u;
            break;
        case 4:  // grayscale + alpha
            bitCount = bitDepth * 2u;
            break;
        case 6:  // RGBA
            bitCount = bitDepth * 4u;
            break;
        default:
            break;
    }
    return IconImageInfo{width, height, static_cast<uint16_t>(bitCount), true};
}

Result<IconImageInfo> InspectDib(std::span<const uint8_t> bytes) {
    if (bytes.size() < 40) {
        return MakeError(L"图标 DIB 数据过短");
    }
    const uint32_t headerSize = U32(bytes, 0);
    if (headerSize < 40 || headerSize > bytes.size()) {
        return MakeError(L"图标 DIB 头尺寸无效");
    }
    const int32_t rawWidth = static_cast<int32_t>(U32(bytes, 4));
    const int32_t rawHeight = static_cast<int32_t>(U32(bytes, 8));
    if (rawWidth <= 0 || rawHeight == 0) {
        return MakeError(L"图标 DIB 尺寸无效");
    }
    const uint32_t width = static_cast<uint32_t>(rawWidth);
    const uint32_t doubleHeight = static_cast<uint32_t>(std::abs(rawHeight));
    const uint32_t height = (doubleHeight >= 2) ? doubleHeight / 2 : doubleHeight;
    const uint16_t bitCount = U16(bytes, 14);
    if (bitCount != 1 && bitCount != 4 && bitCount != 8 && bitCount != 16 && bitCount != 24 &&
        bitCount != 32) {
        return MakeError(Utf8ToWide(fmt::format("不支持的图标位深：{}", bitCount)));
    }
    return IconImageInfo{width, height, bitCount, false};
}

uint8_t WidthByte(uint32_t value) {
    return value >= 256 ? 0 : static_cast<uint8_t>(value);
}

std::set<uint16_t> ReferencedIconIds(const ResourceModel& model, uint16_t language,
                                     std::optional<ResourceKey> excluded) {
    std::set<uint16_t> ids;
    for (const auto& entry : model.Entries()) {
        if (entry.type.named || entry.type.id != kGroupIconType || entry.language != language) {
            continue;
        }
        if (excluded && entry.type == excluded->type && entry.name == excluded->name &&
            entry.language == excluded->language) {
            continue;
        }
        auto group = ParseGroupIcon(entry.data);
        if (!group) {
            continue;
        }
        for (const auto& icon : *group) {
            ids.insert(icon.id);
        }
    }
    return ids;
}

}  // namespace

Result<std::vector<IconImage>> ParseIco(std::span<const uint8_t> bytes) {
    if (bytes.size() < 6) {
        return MakeError(L"ICO 文件头不完整");
    }
    if (U16(bytes, 0) != 0 || U16(bytes, 2) != 1) {
        return MakeError(L"不是有效的 ICO 文件");
    }
    const uint16_t count = U16(bytes, 4);
    if (count == 0 || count > kMaxIconImages) {
        return MakeError(Utf8ToWide(fmt::format("ICO 图像数量无效：{}", count)));
    }
    const uint64_t directoryEnd = 6ull + static_cast<uint64_t>(count) * 16ull;
    if (directoryEnd > bytes.size()) {
        return MakeError(L"ICO 目录项不完整");
    }

    std::vector<IconImage> images;
    images.reserve(count);
    uint64_t total = 0;
    for (uint16_t i = 0; i < count; ++i) {
        const size_t at = 6 + static_cast<size_t>(i) * 16;
        const uint32_t dataSize = U32(bytes, at + 8);
        const uint32_t dataOffset = U32(bytes, at + 12);
        if (dataSize == 0 || dataSize > kMaxIconBytes) {
            return MakeError(L"ICO 单个图像大小无效");
        }
        if (dataOffset < directoryEnd || static_cast<uint64_t>(dataOffset) + dataSize > bytes.size()) {
            return MakeError(L"ICO 图像数据偏移超出文件");
        }
        total += dataSize;
        if (total > kMaxIconBytes) {
            return MakeError(L"ICO 图像总大小过大");
        }

        IconImage image;
        image.width = bytes[at + 0];
        image.height = bytes[at + 1];
        image.colorCount = bytes[at + 2];
        image.reserved = bytes[at + 3];
        image.planes = U16(bytes, at + 4);
        image.bitCount = U16(bytes, at + 6);
        image.data.assign(bytes.begin() + dataOffset, bytes.begin() + dataOffset + dataSize);
        images.push_back(std::move(image));
    }
    return images;
}

Result<std::vector<uint8_t>> BuildIco(std::span<const IconImage> images) {
    if (images.empty() || images.size() > 0xFFFF) {
        return MakeError(L"ICO 图像数量无效");
    }
    uint64_t totalSize = 6ull + static_cast<uint64_t>(images.size()) * 16ull;
    uint64_t offset = (totalSize + 3ull) & ~3ull;
    std::vector<uint64_t> offsets;
    offsets.reserve(images.size());
    for (const auto& image : images) {
        if (image.data.empty()) {
            return MakeError(L"ICO 图像数据为空");
        }
        offsets.push_back(offset);
        offset += image.data.size();
        offset = (offset + 3ull) & ~3ull;
        if (offset > 0xFFFFFFFFull) {
            return MakeError(L"ICO 文件大小超出 32 位偏移范围");
        }
    }

    std::vector<uint8_t> out(static_cast<size_t>(offset), 0);
    Put16(out, 0, 0);
    Put16(out, 2, 1);
    Put16(out, 4, static_cast<uint16_t>(images.size()));
    for (size_t i = 0; i < images.size(); ++i) {
        const size_t at = 6 + i * 16;
        out[at + 0] = images[i].width;
        out[at + 1] = images[i].height;
        out[at + 2] = images[i].colorCount;
        out[at + 3] = images[i].reserved;
        Put16(out, at + 4, images[i].planes);
        Put16(out, at + 6, images[i].bitCount);
        Put32(out, at + 8, static_cast<uint32_t>(images[i].data.size()));
        Put32(out, at + 12, static_cast<uint32_t>(offsets[i]));
        std::memcpy(out.data() + offsets[i], images[i].data.data(), images[i].data.size());
    }
    return out;
}

Result<std::vector<GroupIconEntry>> ParseGroupIcon(std::span<const uint8_t> bytes) {
    if (bytes.size() < 6) {
        return MakeError(L"图标组数据过短");
    }
    if (U16(bytes, 0) != 0 || U16(bytes, 2) != 1) {
        return MakeError(L"图标组数据不是有效的 GRPICONDIR");
    }
    const uint16_t count = U16(bytes, 4);
    if (count == 0) {
        return MakeError(L"图标组图像数量无效");
    }
    const uint64_t expected = 6ull + static_cast<uint64_t>(count) * 14ull;
    if (expected > bytes.size()) {
        return MakeError(L"图标组目录项不完整");
    }

    std::vector<GroupIconEntry> entries;
    entries.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        const size_t at = 6 + static_cast<size_t>(i) * 14;
        GroupIconEntry entry;
        entry.width = bytes[at + 0];
        entry.height = bytes[at + 1];
        entry.colorCount = bytes[at + 2];
        entry.reserved = bytes[at + 3];
        entry.planes = U16(bytes, at + 4);
        entry.bitCount = U16(bytes, at + 6);
        entry.bytesInRes = U32(bytes, at + 8);
        entry.id = U16(bytes, at + 12);
        entries.push_back(entry);
    }
    return entries;
}

Result<std::vector<uint8_t>> BuildGroupIcon(std::span<const GroupIconEntry> entries) {
    if (entries.empty() || entries.size() > 0xFFFF) {
        return MakeError(L"图标组图像数量无效");
    }
    const uint64_t size = 6ull + static_cast<uint64_t>(entries.size()) * 14ull;
    std::vector<uint8_t> out(static_cast<size_t>(size), 0);
    Put16(out, 0, 0);
    Put16(out, 2, 1);
    Put16(out, 4, static_cast<uint16_t>(entries.size()));
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const size_t at = 6 + i * 14;
        out[at + 0] = entry.width;
        out[at + 1] = entry.height;
        out[at + 2] = entry.colorCount;
        out[at + 3] = entry.reserved;
        Put16(out, at + 4, entry.planes);
        Put16(out, at + 6, entry.bitCount);
        Put32(out, at + 8, entry.bytesInRes);
        Put16(out, at + 12, entry.id);
    }
    return out;
}

Result<IconImageInfo> InspectIconData(std::span<const uint8_t> bytes) {
    if (IsPng(bytes)) {
        return InspectPng(bytes);
    }
    return InspectDib(bytes);
}

Result<std::vector<uint8_t>> BuildIcoFromResource(std::span<const uint8_t> iconData) {
    auto info = InspectIconData(iconData);
    if (!info) {
        return std::unexpected(info.error());
    }
    IconImage image;
    image.width = WidthByte(info->width);
    image.height = WidthByte(info->height);
    image.colorCount = 0;
    image.reserved = 0;
    image.planes = 1;
    image.bitCount = info->bitCount;
    image.data.assign(iconData.begin(), iconData.end());
    std::vector<IconImage> images;
    images.push_back(std::move(image));
    return BuildIco(images);
}

Result<std::vector<uint8_t>> BuildIcoFromGroup(const ResourceModel& model,
                                               const ResourceEntry& groupEntry) {
    auto group = ParseGroupIcon(groupEntry.data);
    if (!group) {
        return std::unexpected(group.error());
    }

    std::vector<IconImage> images;
    images.reserve(group->size());
    for (const auto& entry : *group) {
        const ResourceEntry* icon = model.Find(ResourceId::FromId(kIconType),
                                               ResourceId::FromId(entry.id),
                                               groupEntry.language);
        if (icon == nullptr) {
            return MakeError(Utf8ToWide(
                fmt::format("图标组引用的 RT_ICON #{} 不存在", entry.id)));
        }
        IconImage image;
        image.width = entry.width;
        image.height = entry.height;
        image.colorCount = entry.colorCount;
        image.reserved = entry.reserved;
        image.planes = entry.planes;
        image.bitCount = entry.bitCount;
        image.data = icon->data;
        images.push_back(std::move(image));
    }
    return BuildIco(images);
}

Result<void> ReplaceIconGroup(ResourceModel& model, const ResourceEntry& groupEntry,
                              std::span<const uint8_t> icoFile) {
    auto images = ParseIco(icoFile);
    if (!images) {
        return std::unexpected(images.error());
    }

    const ResourceId groupType = groupEntry.type;
    const ResourceId groupName = groupEntry.name;
    const uint16_t language = groupEntry.language;
    const uint32_t codePage = groupEntry.codePage;
    ResourceKey key{groupType, groupName, language};

    std::set<uint16_t> oldIds;
    auto oldGroup = ParseGroupIcon(groupEntry.data);
    if (oldGroup) {
        for (const auto& entry : *oldGroup) {
            oldIds.insert(entry.id);
        }
    }

    const auto referencedByOthers = ReferencedIconIds(model, language, key);

    std::set<uint16_t> existingIconIds;
    for (const auto& entry : model.Entries()) {
        if (!entry.type.named && entry.type.id == kIconType && entry.language == language &&
            !entry.name.named) {
            existingIconIds.insert(entry.name.id);
        }
    }

    std::set<uint16_t> deletable;
    for (const uint16_t id : oldIds) {
        if (!referencedByOthers.contains(id)) {
            deletable.insert(id);
        }
    }

    std::set<uint16_t> occupied;
    for (const uint16_t id : existingIconIds) {
        if (!deletable.contains(id)) {
            occupied.insert(id);
        }
    }
    for (const uint16_t id : referencedByOthers) {
        occupied.insert(id);
    }

    std::vector<uint16_t> newIds;
    newIds.reserve(images->size());
    uint32_t candidate = 1;
    for (size_t i = 0; i < images->size(); ++i) {
        while (candidate <= 0xFFFF) {
            if (!occupied.contains(static_cast<uint16_t>(candidate))) {
                break;
            }
            ++candidate;
        }
        if (candidate > 0xFFFF) {
            return MakeError(L"没有可用的 RT_ICON 资源 ID");
        }
        const uint16_t id = static_cast<uint16_t>(candidate);
        newIds.push_back(id);
        occupied.insert(id);
        ++candidate;
    }

    for (const uint16_t id : oldIds) {
        if (!referencedByOthers.contains(id)) {
            model.Remove(ResourceId::FromId(kIconType), ResourceId::FromId(id), language);
        }
    }

    std::vector<GroupIconEntry> groupEntries;
    groupEntries.reserve(images->size());
    for (size_t i = 0; i < images->size(); ++i) {
        const auto& image = (*images)[i];
        const uint16_t id = newIds[i];
        GroupIconEntry entry;
        entry.width = image.width;
        entry.height = image.height;
        entry.colorCount = image.colorCount;
        entry.reserved = image.reserved;
        entry.planes = image.planes;
        entry.bitCount = image.bitCount;
        entry.id = id;
        entry.bytesInRes = static_cast<uint32_t>(image.data.size());
        groupEntries.push_back(entry);

        model.SetData(ResourceId::FromId(kIconType), ResourceId::FromId(id), language, 0,
                      image.data);
    }

    auto groupData = BuildGroupIcon(groupEntries);
    if (!groupData) {
        return std::unexpected(groupData.error());
    }
    model.SetData(groupType, groupName, language, codePage, std::move(*groupData));
    return {};
}

Result<void> RemoveIconGroup(ResourceModel& model, const ResourceEntry& groupEntry) {
    const ResourceId groupType = groupEntry.type;
    const ResourceId groupName = groupEntry.name;
    const uint16_t language = groupEntry.language;
    ResourceKey key{groupType, groupName, language};

    std::set<uint16_t> oldIds;
    auto oldGroup = ParseGroupIcon(groupEntry.data);
    if (oldGroup) {
        for (const auto& entry : *oldGroup) {
            oldIds.insert(entry.id);
        }
    }
    const auto referencedByOthers = ReferencedIconIds(model, language, key);

    if (!model.Remove(groupType, groupName, language)) {
        return MakeError(L"要删除的图标组不存在");
    }
    for (const uint16_t id : oldIds) {
        if (!referencedByOthers.contains(id)) {
            model.Remove(ResourceId::FromId(kIconType), ResourceId::FromId(id), language);
        }
    }
    return {};
}

}  // namespace peres
