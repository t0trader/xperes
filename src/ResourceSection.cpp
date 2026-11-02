#include "ResourceSection.h"

#include "TextUtil.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>

namespace peres {

namespace {

constexpr uint64_t kMaxDirectoryOffset = 0x7FFFFFFF;
constexpr uint64_t kMaxSectionSize = 0x7FFFFFFF;
constexpr uint16_t kMaxDirectoryEntries = 0xFFFF;

struct LeafNode {
    uint16_t language = 0;
    uint32_t codePage = 0;
    const std::vector<uint8_t>* data = nullptr;
    uint32_t dataEntryOffset = 0;
    uint32_t dataOffset = 0;
};

struct ChildDir;

struct DirNode {
    uint32_t offset = 0;
    std::vector<std::unique_ptr<ChildDir>> subdirs;
    std::vector<LeafNode> leaves;

    size_t EntryCount() const { return subdirs.size() + leaves.size(); }
};

struct ChildDir {
    ResourceId id;
    std::unique_ptr<DirNode> dir;
    uint32_t nameOffset = 0;
};

void PutU16(std::vector<uint8_t>& bytes, uint32_t offset, uint16_t value) {
    bytes[offset + 0] = static_cast<uint8_t>(value & 0xFF);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void PutU32(std::vector<uint8_t>& bytes, uint32_t offset, uint32_t value) {
    bytes[offset + 0] = static_cast<uint8_t>(value & 0xFF);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    bytes[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    bytes[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

uint32_t Align4(uint32_t value) {
    return (value + 3u) & ~3u;
}

uint32_t Align2(uint32_t value) {
    return (value + 1u) & ~1u;
}

Result<std::unique_ptr<DirNode>> BuildTree(const ResourceModel& model) {
    auto root = std::make_unique<DirNode>();
    const auto& entries = model.Entries();
    size_t index = 0;

    while (index < entries.size()) {
        const ResourceId typeId = entries[index].type;
        auto typeNode = std::make_unique<DirNode>();

        while (index < entries.size() && entries[index].type == typeId) {
            const ResourceId nameId = entries[index].name;
            auto nameNode = std::make_unique<DirNode>();

            while (index < entries.size() && entries[index].type == typeId &&
                   entries[index].name == nameId) {
                nameNode->leaves.push_back(LeafNode{
                    entries[index].language,
                    entries[index].codePage,
                    &entries[index].data,
                });
                ++index;
            }

            auto child = std::make_unique<ChildDir>();
            child->id = nameId;
            child->dir = std::move(nameNode);
            typeNode->subdirs.push_back(std::move(child));
        }

        auto child = std::make_unique<ChildDir>();
        child->id = typeId;
        child->dir = std::move(typeNode);
        root->subdirs.push_back(std::move(child));
    }

    return root;
}

Result<void> AssignDirectoryOffsets(DirNode& node, uint32_t& next) {
    const uint64_t entries = node.EntryCount();
    if (entries > kMaxDirectoryEntries) {
        return MakeError(L"单个资源目录条目过多，无法序列化");
    }

    node.offset = next;
    const uint64_t size = 16ull + entries * 8ull;
    if (next + size > kMaxDirectoryOffset) {
        return MakeError(L"资源目录过大，超出 PE 资源目录的 31 位偏移限制");
    }
    next = static_cast<uint32_t>(next + size);

    for (const auto& child : node.subdirs) {
        auto result = AssignDirectoryOffsets(*child->dir, next);
        if (!result) {
            return result;
        }
    }
    return {};
}

using StringOffsetMap = std::unordered_map<std::wstring, uint32_t>;

Result<void> AssignStringOffsets(DirNode& node, uint32_t& next, StringOffsetMap& offsets,
                                 std::vector<std::pair<std::wstring, uint32_t>>& order) {
    for (const auto& child : node.subdirs) {
        if (child->id.named) {
            if (child->id.name.size() > 0xFFFF) {
                return MakeError(Utf8ToWide(
                    fmt::format("资源名称过长（{} 个字符）", child->id.name.size())));
            }
            next = Align2(next);
            auto [it, inserted] = offsets.try_emplace(child->id.name, next);
            if (inserted) {
                const uint64_t stringSize = 2ull + static_cast<uint64_t>(child->id.name.size()) * 2ull;
                if (next + stringSize > kMaxDirectoryOffset) {
                    return MakeError(L"资源名称表过大，超出 31 位偏移限制");
                }
                order.emplace_back(child->id.name, next);
                next = static_cast<uint32_t>(next + stringSize);
            }
            child->nameOffset = it->second;
        }
        auto result = AssignStringOffsets(*child->dir, next, offsets, order);
        if (!result) {
            return result;
        }
    }
    return {};
}

Result<void> AssignDataEntryOffsets(DirNode& node, uint32_t& next) {
    for (auto& leaf : node.leaves) {
        next = Align4(next);
        if (next + 16ull > kMaxDirectoryOffset) {
            return MakeError(L"资源数据条目过多，超出 31 位偏移限制");
        }
        leaf.dataEntryOffset = next;
        next += 16;
    }
    for (const auto& child : node.subdirs) {
        auto result = AssignDataEntryOffsets(*child->dir, next);
        if (!result) {
            return result;
        }
    }
    return {};
}

Result<void> AssignDataOffsets(DirNode& node, uint32_t& next) {
    for (auto& leaf : node.leaves) {
        next = Align4(next);
        const uint64_t size = leaf.data->size();
        if (next + size > kMaxSectionSize) {
            return MakeError(L"资源数据总大小超过 2 GB 限制");
        }
        leaf.dataOffset = next;
        next = static_cast<uint32_t>(next + size);
    }
    for (const auto& child : node.subdirs) {
        auto result = AssignDataOffsets(*child->dir, next);
        if (!result) {
            return result;
        }
    }
    return {};
}

void WriteDirectory(std::vector<uint8_t>& bytes, const DirNode& node) {
    uint16_t namedCount = 0;
    uint16_t idCount = 0;
    for (const auto& child : node.subdirs) {
        if (child->id.named) {
            ++namedCount;
        } else {
            ++idCount;
        }
    }
    idCount = static_cast<uint16_t>(idCount + node.leaves.size());

    const uint32_t header = node.offset;
    PutU16(bytes, header + 12, namedCount);
    PutU16(bytes, header + 14, idCount);

    uint32_t entryOffset = header + 16;
    auto writeSubdir = [&](const ChildDir& child) {
        if (child.id.named) {
            PutU32(bytes, entryOffset, 0x80000000u | child.nameOffset);
        } else {
            PutU32(bytes, entryOffset, child.id.id);
        }
        PutU32(bytes, entryOffset + 4, 0x80000000u | child.dir->offset);
        entryOffset += 8;
    };

    for (const auto& child : node.subdirs) {
        if (child->id.named) {
            writeSubdir(*child);
        }
    }
    for (const auto& child : node.subdirs) {
        if (!child->id.named) {
            writeSubdir(*child);
        }
    }
    for (const auto& leaf : node.leaves) {
        PutU32(bytes, entryOffset, leaf.language);
        PutU32(bytes, entryOffset + 4, leaf.dataEntryOffset);
        entryOffset += 8;
    }

    for (const auto& child : node.subdirs) {
        WriteDirectory(bytes, *child->dir);
    }
}

void WriteStrings(std::vector<uint8_t>& bytes,
                  const std::vector<std::pair<std::wstring, uint32_t>>& strings) {
    for (const auto& [text, offset] : strings) {
        PutU16(bytes, offset, static_cast<uint16_t>(text.size()));
        for (size_t i = 0; i < text.size(); ++i) {
            PutU16(bytes, offset + 2 + static_cast<uint32_t>(i * 2),
                   static_cast<uint16_t>(text[i]));
        }
    }
}

void WriteDataEntries(std::vector<uint8_t>& bytes, const DirNode& node, uint32_t sectionRva) {
    for (const auto& leaf : node.leaves) {
        PutU32(bytes, leaf.dataEntryOffset, sectionRva + leaf.dataOffset);
        PutU32(bytes, leaf.dataEntryOffset + 4, static_cast<uint32_t>(leaf.data->size()));
        PutU32(bytes, leaf.dataEntryOffset + 8, leaf.codePage);
        PutU32(bytes, leaf.dataEntryOffset + 12, 0);
    }
    for (const auto& child : node.subdirs) {
        WriteDataEntries(bytes, *child->dir, sectionRva);
    }
}

void WriteData(std::vector<uint8_t>& bytes, const DirNode& node) {
    for (const auto& leaf : node.leaves) {
        if (!leaf.data->empty()) {
            std::memcpy(bytes.data() + leaf.dataOffset, leaf.data->data(), leaf.data->size());
        }
    }
    for (const auto& child : node.subdirs) {
        WriteData(bytes, *child->dir);
    }
}

}  // namespace

Result<std::vector<uint8_t>> BuildResourceSection(const ResourceModel& model, uint32_t sectionRva) {
    auto treeResult = BuildTree(model);
    if (!treeResult) {
        return std::unexpected(treeResult.error());
    }
    const auto tree = std::move(*treeResult);

    uint32_t next = 0;
    auto result = AssignDirectoryOffsets(*tree, next);
    if (!result) {
        return std::unexpected(result.error());
    }

    StringOffsetMap stringOffsets;
    std::vector<std::pair<std::wstring, uint32_t>> stringOrder;
    result = AssignStringOffsets(*tree, next, stringOffsets, stringOrder);
    if (!result) {
        return std::unexpected(result.error());
    }

    result = AssignDataEntryOffsets(*tree, next);
    if (!result) {
        return std::unexpected(result.error());
    }

    result = AssignDataOffsets(*tree, next);
    if (!result) {
        return std::unexpected(result.error());
    }

    if (static_cast<uint64_t>(sectionRva) + next > 0xFFFFFFFFull) {
        return MakeError(L"资源节 RVA 超出 32 位范围");
    }

    std::vector<uint8_t> bytes(next, 0);
    WriteDirectory(bytes, *tree);
    WriteStrings(bytes, stringOrder);
    WriteDataEntries(bytes, *tree, sectionRva);
    WriteData(bytes, *tree);
    return bytes;
}

}  // namespace peres
