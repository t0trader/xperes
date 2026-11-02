#include "StringTableCodec.h"

#include "TextUtil.h"

#include <fmt/format.h>

#include <cstdlib>
#include <cwctype>
#include <sstream>

namespace peres {

namespace {

constexpr uint32_t kSlots = 16;

std::wstring EscapeString(std::wstring_view text) {
    std::wstring output;
    for (const wchar_t ch : text) {
        switch (ch) {
            case L'\\':
                output += L"\\\\";
                break;
            case L'\r':
                output += L"\\r";
                break;
            case L'\n':
                output += L"\\n";
                break;
            case L'\t':
                output += L"\\t";
                break;
            default:
                output.push_back(ch);
                break;
        }
    }
    return output;
}

std::wstring UnescapeString(std::wstring_view text, bool& ok) {
    ok = true;
    std::wstring output;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch != L'\\') {
            output.push_back(ch);
            continue;
        }
        if (i + 1 >= text.size()) {
            ok = false;
            return {};
        }
        const wchar_t next = text[++i];
        switch (next) {
            case L'\\':
                output.push_back(L'\\');
                break;
            case L'r':
                output.push_back(L'\r');
                break;
            case L'n':
                output.push_back(L'\n');
                break;
            case L't':
                output.push_back(L'\t');
                break;
            default:
                output.push_back(next);
                break;
        }
    }
    return output;
}

bool ParseUnsigned(std::wstring_view text, uint32_t& value) {
    const std::wstring trimmed = Trim(text);
    if (trimmed.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const unsigned long long parsed = std::wcstoull(trimmed.c_str(), &end, 0);
    while (end != nullptr && *end != L'\0' && std::iswspace(*end) != 0) {
        ++end;
    }
    if (end == nullptr || *end != L'\0' || parsed > 0xFFFFFFFFull) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

}  // namespace

Result<std::wstring> FormatStringTable(std::span<const uint8_t> data, uint16_t blockId) {
    std::wstring output;
    size_t offset = 0;
    for (uint32_t slot = 0; slot < kSlots; ++slot) {
        if (offset + 2 > data.size()) {
            return MakeError(Utf8ToWide(
                fmt::format("字符串表数据在第 {} 项处被截断", slot)));
        }
        const uint16_t length = static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
        offset += 2;
        if (offset + static_cast<size_t>(length) * 2 > data.size()) {
            return MakeError(Utf8ToWide(
                fmt::format("字符串表第 {} 项长度超出数据范围", slot)));
        }
        std::wstring text;
        text.reserve(length);
        for (uint16_t i = 0; i < length; ++i) {
            const uint16_t ch = static_cast<uint16_t>(
                data[offset + i * 2] | (data[offset + i * 2 + 1] << 8));
            text.push_back(static_cast<wchar_t>(ch));
        }
        offset += static_cast<size_t>(length) * 2;

        const uint32_t id =
            blockId > 0 ? static_cast<uint32_t>(blockId - 1) * kSlots + slot : slot;
        output += L"#" + std::to_wstring(id) + L": " + EscapeString(text) + L"\r\n";
    }
    return output;
}

Result<std::vector<uint8_t>> BuildStringTable(std::wstring_view text, uint16_t blockId) {
    std::vector<std::wstring> slots(kSlots);
    std::vector<bool> used(kSlots, false);

    std::wstringstream stream((std::wstring(text)));
    std::wstring line;
    while (std::getline(stream, line)) {
        const std::wstring trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == L';' ||
            (trimmed.size() >= 2 && trimmed[0] == L'/' && trimmed[1] == L'/')) {
            continue;
        }
        if (trimmed[0] != L'#') {
            return MakeError(L"字符串表每一行应以 #<ID>: 开头");
        }
        const size_t colon = trimmed.find(L':');
        if (colon == std::wstring::npos) {
            return MakeError(L"字符串表行缺少冒号：#<ID>: <文本>");
        }
        uint32_t id = 0;
        if (!ParseUnsigned(trimmed.substr(1, colon - 1), id)) {
            return MakeError(L"字符串表的字符串 ID 无效");
        }
        uint32_t slot = 0;
        if (blockId > 0) {
            const uint32_t base = static_cast<uint32_t>(blockId - 1) * kSlots;
            if (id < base || id >= base + kSlots) {
                return MakeError(Utf8ToWide(fmt::format(
                    "字符串 ID {} 不在当前字符串块 [{}, {}) 范围内", id, base, base + kSlots)));
            }
            slot = id - base;
        } else {
            if (id >= kSlots) {
                return MakeError(Utf8ToWide(
                    fmt::format("字符串 ID {} 超出 0-15 范围", id)));
            }
            slot = id;
        }
        if (used[slot]) {
            return MakeError(Utf8ToWide(fmt::format("字符串 ID {} 重复", id)));
        }
        used[slot] = true;

        std::wstring raw = colon + 1 < trimmed.size() ? trimmed.substr(colon + 1) : L"";
        raw = Trim(raw);
        bool ok = true;
        slots[slot] = UnescapeString(raw, ok);
        if (!ok) {
            return MakeError(Utf8ToWide(fmt::format("字符串 {} 的转义序列无效", id)));
        }
    }

    std::vector<uint8_t> output;
    output.reserve(2 * kSlots + 4096);
    for (uint32_t slot = 0; slot < kSlots; ++slot) {
        const std::wstring& value = slots[slot];
        if (value.size() > 0xFFFF) {
            return MakeError(L"单个字符串过长（超过 65535 个字符）");
        }
        output.push_back(static_cast<uint8_t>(value.size() & 0xFF));
        output.push_back(static_cast<uint8_t>((value.size() >> 8) & 0xFF));
        for (const wchar_t ch : value) {
            output.push_back(static_cast<uint8_t>(ch & 0xFF));
            output.push_back(static_cast<uint8_t>((ch >> 8) & 0xFF));
        }
    }
    return output;
}

}  // namespace peres
