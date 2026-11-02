#include "VersionCodec.h"

#include "TextUtil.h"

#include <fmt/format.h>

#include <algorithm>

namespace peres {

namespace {

struct Block {
    std::wstring key;
    uint16_t type = 0;
    uint16_t valueLength = 0;
    size_t start = 0;
    size_t length = 0;
    size_t valueStart = 0;
    size_t blockEnd = 0;
    std::vector<Block> children;
};

size_t Align4(size_t value) {
    return (value + 3u) & ~static_cast<size_t>(3u);
}

bool ReadU16(std::span<const uint8_t> data, size_t offset, uint16_t& value) {
    if (offset + 2 > data.size()) {
        return false;
    }
    value = static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
    return true;
}

bool ReadU32(std::span<const uint8_t> data, size_t offset, uint32_t& value) {
    if (offset + 4 > data.size()) {
        return false;
    }
    value = static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8) |
            (static_cast<uint32_t>(data[offset + 2]) << 16) |
            (static_cast<uint32_t>(data[offset + 3]) << 24);
    return true;
}

Result<std::wstring> ReadKey(std::span<const uint8_t> data, size_t start, size_t blockEnd,
                            size_t& afterKey) {
    std::wstring key;
    size_t pos = start + 6;
    while (pos + 2 <= blockEnd) {
        uint16_t ch = 0;
        if (!ReadU16(data, pos, ch)) {
            return MakeError(L"版本信息键名被截断");
        }
        pos += 2;
        if (ch == 0) {
            afterKey = pos;
            return key;
        }
        if (key.size() > 4096) {
            return MakeError(L"版本信息键名过长");
        }
        key.push_back(static_cast<wchar_t>(ch));
    }
    return MakeError(L"版本信息键名缺少终止符");
}

Result<Block> ParseBlock(std::span<const uint8_t> data, size_t start, size_t limit,
                        uint32_t depth) {
    if (depth > 16 || start + 6 > limit || start + 6 > data.size()) {
        return MakeError(L"版本信息块层级或数据长度无效");
    }
    uint16_t length = 0;
    uint16_t valueLength = 0;
    uint16_t type = 0;
    if (!ReadU16(data, start, length) || !ReadU16(data, start + 2, valueLength) ||
        !ReadU16(data, start + 4, type)) {
        return MakeError(L"版本信息块头被截断");
    }
    if (length == 0 || start + length > limit || start + length > data.size()) {
        return MakeError(L"版本信息块长度超出资源范围");
    }

    Block block;
    block.start = start;
    block.length = length;
    block.blockEnd = start + length;
    block.type = type;
    block.valueLength = valueLength;

    size_t afterKey = 0;
    auto key = ReadKey(data, start, block.blockEnd, afterKey);
    if (!key) {
        return std::unexpected(key.error());
    }
    block.key = std::move(*key);

    block.valueStart = Align4(afterKey);
    if (block.valueStart > block.blockEnd) {
        return MakeError(L"版本信息值起点超出块范围");
    }

    // Container nodes have no textual value and are the only nodes that need
    // recursive child parsing. String/Var nodes store their value inline.
    // VS_VERSION_INFO is special: it is a container but also carries the
    // 52-byte VS_FIXEDFILEINFO value.
    const bool isRoot = IEqualsAscii(block.key, L"VS_VERSION_INFO");
    const bool container = block.valueLength == 0 || isRoot;
    if (!container) {
        return block;
    }

    size_t childStart = Align4(block.valueStart + static_cast<size_t>(block.valueLength));
    while (childStart + 6 <= block.blockEnd) {
        auto child = ParseBlock(data, childStart, block.blockEnd, depth + 1);
        if (!child) {
            return std::unexpected(child.error());
        }
        if (child->length == 0) {
            break;
        }
        const size_t childLength = child->length;
        const size_t next = Align4(childStart + childLength);
        block.children.push_back(std::move(*child));
        if (next <= childStart) {
            break;
        }
        childStart = next;
    }
    return block;
}

std::wstring ReadTextValue(std::span<const uint8_t> data, const Block& block) {
    std::wstring text;
    const size_t end = block.blockEnd;
    size_t pos = block.valueStart;
    while (pos + 2 <= end) {
        uint16_t ch = 0;
        if (!ReadU16(data, pos, ch)) {
            break;
        }
        pos += 2;
        if (ch == 0) {
            break;
        }
        if (text.size() >= 65535) {
            break;
        }
        text.push_back(static_cast<wchar_t>(ch));
    }
    return text;
}

void ParseFixedInfo(std::span<const uint8_t> data, const Block& root, VersionFixedInfo& fixed) {
    if (root.valueStart + 52 > root.blockEnd) {
        return;
    }
    uint32_t values[13] = {};
    for (size_t i = 0; i < 13; ++i) {
        if (!ReadU32(data, root.valueStart + i * 4, values[i])) {
            return;
        }
    }
    fixed.present = true;
    fixed.signature = values[0];
    fixed.structureVersion = values[1];
    fixed.fileVersionMs = values[2];
    fixed.fileVersionLs = values[3];
    fixed.productVersionMs = values[4];
    fixed.productVersionLs = values[5];
    fixed.fileFlagsMask = values[6];
    fixed.fileFlags = values[7];
    fixed.fileOs = values[8];
    fixed.fileType = values[9];
    fixed.fileSubtype = values[10];
    fixed.fileDateMs = values[11];
    fixed.fileDateLs = values[12];
}

void CollectStrings(std::span<const uint8_t> data, const Block& root, VersionInfo& info) {
    for (const Block& block : root.children) {
        if (!IEqualsAscii(block.key, L"StringFileInfo")) {
            continue;
        }
        for (const Block& table : block.children) {
            VersionStringTable stringTable;
            stringTable.key = table.key;
            for (const Block& entry : table.children) {
                VersionString string;
                string.key = entry.key;
                string.value = ReadTextValue(data, entry);
                stringTable.strings.push_back(std::move(string));
            }
            info.stringTables.push_back(std::move(stringTable));
        }
    }
}

void CollectTranslations(std::span<const uint8_t> data, const Block& root, VersionInfo& info) {
    for (const Block& block : root.children) {
        if (!IEqualsAscii(block.key, L"VarFileInfo")) {
            continue;
        }
        for (const Block& var : block.children) {
            if (!IEqualsAscii(var.key, L"Translation")) {
                continue;
            }
            size_t valueEnd = var.valueStart + var.valueLength;
            if (var.valueLength == 0 || valueEnd > var.blockEnd) {
                valueEnd = var.blockEnd;
            }
            size_t pos = var.valueStart;
            while (pos + 4 <= valueEnd) {
                uint16_t language = 0;
                uint16_t codePage = 0;
                if (!ReadU16(data, pos, language) || !ReadU16(data, pos + 2, codePage)) {
                    break;
                }
                info.translations.push_back(VersionTranslation{language, codePage});
                pos += 4;
            }
        }
    }
}

std::wstring Hex32(uint32_t value) {
    return L"0x" + Utf8ToWide(fmt::format("{:08X}", value));
}

void AppendLine(std::wstring& output, std::wstring_view line) {
    if (!output.empty()) {
        output += L"\r\n";
    }
    output += line;
}

std::wstring FieldLine(std::wstring_view name, std::wstring_view value) {
    return L"  " + std::wstring(name) + L": " + std::wstring(value);
}

}  // namespace

std::wstring FormatVersionDwords(uint32_t ms, uint32_t ls) {
    return std::to_wstring(ms >> 16) + L"." + std::to_wstring(ms & 0xFFFF) + L"." +
           std::to_wstring(ls >> 16) + L"." + std::to_wstring(ls & 0xFFFF);
}

Result<VersionInfo> ParseVersionResource(std::span<const uint8_t> data) {
    if (data.size() < 6) {
        return MakeError(L"版本信息资源过短");
    }
    auto root = ParseBlock(data, 0, data.size(), 0);
    if (!root) {
        return std::unexpected(root.error());
    }
    if (!IEqualsAscii(root->key, L"VS_VERSION_INFO")) {
        return MakeError(L"RT_VERSION 根节点不是 VS_VERSION_INFO");
    }

    VersionInfo info;
    ParseFixedInfo(data, *root, info.fixed);
    CollectStrings(data, *root, info);
    CollectTranslations(data, *root, info);
    return info;
}

std::wstring FormatVersionText(const VersionInfo& info) {
    std::wstring output;
    AppendLine(output, L"VS_VERSION_INFO");

    if (info.fixed.present) {
        AppendLine(output, L"");
        AppendLine(output, L"[固定文件信息]");
        AppendLine(output, FieldLine(L"Signature", Hex32(info.fixed.signature)));
        AppendLine(output, FieldLine(L"StructureVersion", Hex32(info.fixed.structureVersion)));
        AppendLine(output, FieldLine(L"FileVersion",
                                     FormatVersionDwords(info.fixed.fileVersionMs,
                                                         info.fixed.fileVersionLs)));
        AppendLine(output, FieldLine(L"ProductVersion",
                                     FormatVersionDwords(info.fixed.productVersionMs,
                                                         info.fixed.productVersionLs)));
        AppendLine(output, FieldLine(L"FileFlagsMask", Hex32(info.fixed.fileFlagsMask)));
        AppendLine(output, FieldLine(L"FileFlags", Hex32(info.fixed.fileFlags)));
        AppendLine(output, FieldLine(L"FileOS", Hex32(info.fixed.fileOs)));
        AppendLine(output, FieldLine(L"FileType", Hex32(info.fixed.fileType)));
        AppendLine(output, FieldLine(L"FileSubtype", Hex32(info.fixed.fileSubtype)));
        AppendLine(output, FieldLine(L"FileDate",
                                     Hex32(info.fixed.fileDateMs) + L" " +
                                         Hex32(info.fixed.fileDateLs)));
    } else {
        AppendLine(output, L"");
        AppendLine(output, L"[固定文件信息] 缺失");
    }

    for (const VersionStringTable& table : info.stringTables) {
        AppendLine(output, L"");
        AppendLine(output, L"[StringFileInfo " + table.key + L"]");
        for (const VersionString& string : table.strings) {
            AppendLine(output, L"  " + string.key + L": " + string.value);
        }
    }

    if (!info.translations.empty()) {
        AppendLine(output, L"");
        AppendLine(output, L"[VarFileInfo]");
        for (const VersionTranslation& translation : info.translations) {
            AppendLine(output, L"  Translation: 0x" +
                                   Utf8ToWide(fmt::format("{:04X}", translation.language)) +
                                   L", 0x" +
                                   Utf8ToWide(fmt::format("{:04X}", translation.codePage)));
        }
    }
    return output;
}

}  // namespace peres
