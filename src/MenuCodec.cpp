#include "MenuCodec.h"

#include "TextUtil.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cwctype>
#include <optional>

namespace peres {

namespace {

constexpr uint16_t kMfPopup = 0x0010;
constexpr uint16_t kMfEnd = 0x0080;
constexpr uint16_t kMfSeparator = 0x0800;
constexpr uint16_t kMfMaskFixed = kMfPopup | kMfEnd;
constexpr uint32_t kMaxText = 65535;
constexpr uint32_t kMaxItems = 100000;
constexpr uint32_t kMaxDepth = 32;

bool ReadU16(std::span<const uint8_t> data, size_t& pos, uint16_t& value) {
    if (pos + 2 > data.size()) {
        return false;
    }
    value = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
    pos += 2;
    return true;
}

bool ReadU32(std::span<const uint8_t> data, size_t& pos, uint32_t& value) {
    if (pos + 4 > data.size()) {
        return false;
    }
    value = static_cast<uint32_t>(data[pos]) |
            (static_cast<uint32_t>(data[pos + 1]) << 8) |
            (static_cast<uint32_t>(data[pos + 2]) << 16) |
            (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return true;
}

void WriteU16(std::vector<uint8_t>& data, uint16_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void WriteU32(std::vector<uint8_t>& data, uint32_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void Align4(std::vector<uint8_t>& data) {
    while ((data.size() & 3u) != 0) {
        data.push_back(0);
    }
}

Result<std::wstring> ReadText(std::span<const uint8_t> data, size_t& pos) {
    std::wstring text;
    for (;;) {
        uint16_t ch = 0;
        if (!ReadU16(data, pos, ch)) {
            return MakeError(L"菜单字符串在数据末尾被截断");
        }
        if (ch == 0) {
            break;
        }
        if (text.size() >= kMaxText) {
            return MakeError(L"菜单字符串过长");
        }
        text.push_back(static_cast<wchar_t>(ch));
    }
    return text;
}

void WriteText(std::vector<uint8_t>& data, std::wstring_view text) {
    for (const wchar_t ch : text) {
        WriteU16(data, static_cast<uint16_t>(ch));
    }
    WriteU16(data, 0);
}

std::wstring Hex32(uint32_t value) {
    return L"0x" + Utf8ToWide(fmt::format("{:08X}", value));
}

std::wstring Hex16(uint16_t value) {
    return L"0x" + Utf8ToWide(fmt::format("{:04X}", value));
}

std::wstring EscapeText(std::wstring_view text) {
    std::wstring result;
    for (const wchar_t ch : text) {
        switch (ch) {
            case L'\\':
                result += L"\\\\";
                break;
            case L'"':
                result += L"\\\"";
                break;
            case L'\r':
                result += L"\\r";
                break;
            case L'\n':
                result += L"\\n";
                break;
            case L'\t':
                result += L"\\t";
                break;
            default:
                result.push_back(ch);
                break;
        }
    }
    return result;
}

bool ParseNumber(std::wstring_view text, uint32_t& value) {
    const std::wstring trimmed = Trim(text);
    if (trimmed.empty()) {
        return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long parsed = std::wcstoull(trimmed.c_str(), &end, 0);
    while (end != nullptr && *end != L'\0' && std::iswspace(*end) != 0) {
        ++end;
    }
    if (errno != 0 || end == nullptr || *end != L'\0' || parsed > 0xFFFFFFFFull) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool ParseQuoted(std::wstring_view text, size_t& pos, std::wstring& value,
                 std::wstring& error) {
    while (pos < text.size() && std::iswspace(text[pos]) != 0) {
        ++pos;
    }
    if (pos >= text.size() || text[pos] != L'"') {
        error = L"缺少双引号字符串";
        return false;
    }
    ++pos;
    value.clear();
    while (pos < text.size()) {
        const wchar_t ch = text[pos++];
        if (ch == L'"') {
            return true;
        }
        if (ch != L'\\') {
            value.push_back(ch);
            continue;
        }
        if (pos >= text.size()) {
            error = L"字符串转义不完整";
            return false;
        }
        const wchar_t escaped = text[pos++];
        switch (escaped) {
            case L'\\':
                value.push_back(L'\\');
                break;
            case L'"':
                value.push_back(L'"');
                break;
            case L'r':
                value.push_back(L'\r');
                break;
            case L'n':
                value.push_back(L'\n');
                break;
            case L't':
                value.push_back(L'\t');
                break;
            default:
                value.push_back(escaped);
                break;
        }
    }
    error = L"字符串未闭合";
    return false;
}

std::wstring RemoveComments(std::wstring_view line) {
    bool inQuote = false;
    bool escape = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const wchar_t ch = line[i];
        if (inQuote) {
            if (escape) {
                escape = false;
            } else if (ch == L'\\') {
                escape = true;
            } else if (ch == L'"') {
                inQuote = false;
            }
            continue;
        }
        if (ch == L'"') {
            inQuote = true;
            continue;
        }
        if (ch == L';' ||
            (ch == L'/' && i + 1 < line.size() && line[i + 1] == L'/')) {
            return std::wstring(line.substr(0, i));
        }
    }
    return std::wstring(line);
}

bool ParseNumberList(std::wstring_view text, std::vector<uint32_t>& values,
                     std::wstring& error) {
    std::wstring tail = Trim(text);
    if (tail.empty()) {
        return true;
    }
    if (tail.front() != L',') {
        error = L"数值参数之间缺少逗号";
        return false;
    }
    tail.erase(tail.begin());
    while (true) {
        const size_t comma = tail.find(L',');
        const std::wstring piece = Trim(tail.substr(0, comma));
        uint32_t value = 0;
        if (!ParseNumber(piece, value)) {
            error = L"数值参数无效：" + piece;
            return false;
        }
        values.push_back(value);
        if (comma == std::wstring::npos) {
            break;
        }
        tail.erase(0, comma + 1);
    }
    return true;
}

bool IsKeyword(std::wstring_view text, std::wstring_view keyword) {
    return IEqualsAscii(Trim(text), keyword);
}

Result<MenuItem> ParseTextItem(const std::wstring& rawLine, MenuTemplateFormat format) {
    std::wstring line = Trim(RemoveComments(rawLine));
    if (line.empty()) {
        return MakeError(L"空的菜单项行");
    }
    size_t space = 0;
    while (space < line.size() && std::iswspace(line[space]) == 0) {
        ++space;
    }
    const std::wstring keyword = line.substr(0, space);
    std::wstring rest = Trim(line.substr(space));

    MenuItem item;
    std::wstring error;

    if (IsKeyword(keyword, L"MENUITEM")) {
        if (rest.size() >= 9 && IEqualsAscii(rest.substr(0, 9), L"SEPARATOR")) {
            item.separator = true;
            item.text.clear();
            item.id = 0;
            std::vector<uint32_t> values;
            if (!ParseNumberList(rest.substr(9), values, error)) {
                return MakeError(error);
            }
            if (format == MenuTemplateFormat::Classic) {
                if (values.size() > 1) {
                    return MakeError(L"classic MENUITEM SEPARATOR 最多接受一个 flags 参数");
                }
                item.classicFlags = values.empty() ? 0 : static_cast<uint16_t>(values[0]);
            } else {
                if (values.size() > 2) {
                    return MakeError(L"MENUEX SEPARATOR 最多接受 type 与 state 两个参数");
                }
                item.type = values.empty() ? 0x00000800u : values[0];
                item.state = values.size() > 1 ? values[1] : 0;
            }
            return item;
        }

        size_t pos = 0;
        if (!ParseQuoted(rest, pos, item.text, error)) {
            return MakeError(error);
        }
        std::vector<uint32_t> values;
        if (!ParseNumberList(rest.substr(pos), values, error)) {
            return MakeError(error);
        }
        if (format == MenuTemplateFormat::Classic) {
            if (values.empty()) {
                return MakeError(L"classic MENUITEM 缺少菜单 ID");
            }
            if (values.size() > 2 || values[0] > 0xFFFF || (values.size() > 1 && values[1] > 0xFFFF)) {
                return MakeError(L"classic MENUITEM 的 ID 或 flags 超出 16 位范围");
            }
            item.id = values[0];
            item.classicFlags = values.size() > 1 ? static_cast<uint16_t>(values[1]) : 0;
        } else {
            if (values.empty()) {
                return MakeError(L"MENUEX MENUITEM 缺少菜单 ID");
            }
            if (values.size() > 3) {
                return MakeError(L"MENUEX MENUITEM 参数过多");
            }
            item.id = values[0];
            item.type = values.size() > 1 ? values[1] : 0;
            item.state = values.size() > 2 ? values[2] : 0;
        }
        return item;
    }

    if (IsKeyword(keyword, L"POPUP")) {
        size_t pos = 0;
        if (!ParseQuoted(rest, pos, item.text, error)) {
            return MakeError(error);
        }
        item.popup = true;

        std::vector<uint32_t> values;
        if (!ParseNumberList(rest.substr(pos), values, error)) {
            return MakeError(error);
        }
        if (format == MenuTemplateFormat::Classic) {
            if (values.size() > 1 || (values.size() == 1 && values[0] > 0xFFFF)) {
                return MakeError(L"classic POPUP 最多接受一个 16 位 flags 参数");
            }
            item.classicFlags = values.empty() ? 0 : static_cast<uint16_t>(values[0]);
        } else {
            if (values.empty()) {
                return MakeError(L"MENUEX POPUP 缺少 ID 参数");
            }
            if (values.size() > 4) {
                return MakeError(L"MENUEX POPUP 参数过多");
            }
            item.id = values[0];
            item.type = values.size() > 1 ? values[1] : 0;
            item.state = values.size() > 2 ? values[2] : 0;
            item.helpId = values.size() > 3 ? values[3] : 0;
        }
        return item;
    }

    return MakeError(L"未知的菜单项关键字：" + keyword);
}

class MenuTextParser {
public:
    MenuTextParser(std::vector<std::wstring> lines, MenuTemplateFormat format)
        : lines_(std::move(lines)), format_(format) {}

    Result<MenuResource> Parse() {
        Resource = MenuResource{};
        Resource.format = format_;

        size_t index = NextSignificant(0);
        if (index >= lines_.size()) {
            return MakeError(L"菜单文本为空");
        }
        std::wstring helpLine = Trim(lines_[index]);
        size_t helpSpace = 0;
        while (helpSpace < helpLine.size() && std::iswspace(helpLine[helpSpace]) == 0) {
            ++helpSpace;
        }
        if (IEqualsAscii(helpLine.substr(0, helpSpace), L"HELPID")) {
            uint32_t value = 0;
            if (!ParseNumber(helpLine.substr(helpSpace), value)) {
                return MakeError(L"HELPID 数值无效");
            }
            Resource.helpId = value;
            ++index;
            index = NextSignificant(index);
        }
        if (index >= lines_.size() || Trim(lines_[index]) != L"{") {
            return MakeError(L"菜单主体缺少 {");
        }
        index_ = index;
        auto block = ParseBlock(Resource.items, 0);
        if (!block) {
            return std::unexpected(block.error());
        }
        if (NextSignificant(index_) < lines_.size()) {
            return MakeError(L"菜单文本在结束括号后还有多余内容");
        }
        return Resource;
    }

private:
    size_t NextSignificant(size_t index) const {
        while (index < lines_.size()) {
            const std::wstring line = Trim(RemoveComments(lines_[index]));
            if (!line.empty()) {
                break;
            }
            ++index;
        }
        return index;
    }

    Result<void> ParseBlock(std::vector<MenuItem>& items, uint32_t depth) {
        if (depth > kMaxDepth) {
            return MakeError(L"菜单嵌套层级过深");
        }
        if (index_ >= lines_.size() || Trim(lines_[index_]) != L"{") {
            return MakeError(L"菜单块缺少 {");
        }
        ++index_;

        while (true) {
            index_ = NextSignificant(index_);
            if (index_ >= lines_.size()) {
                return MakeError(L"菜单块缺少 }");
            }
            const std::wstring line = Trim(RemoveComments(lines_[index_]));
            if (line == L"}") {
                ++index_;
                return {};
            }
            if (line == L"{") {
                return MakeError(L"意外的菜单块起始括号");
            }

            auto item = ParseTextItem(line, format_);
            if (!item) {
                return std::unexpected(item.error());
            }
            ++index_;

            if (item->popup) {
                const size_t next = NextSignificant(index_);
                if (next < lines_.size() && Trim(RemoveComments(lines_[next])) == L"{") {
                    index_ = next;
                    auto child = ParseBlock(item->children, depth + 1);
                    if (!child) {
                        return child;
                    }
                }
            }

            if (items.size() >= kMaxItems) {
                return MakeError(L"菜单项过多");
            }
            items.push_back(std::move(*item));
        }
    }

    std::vector<std::wstring> lines_;
    MenuTemplateFormat format_ = MenuTemplateFormat::Classic;
    size_t index_ = 0;
    MenuResource Resource;
};

Result<void> ParseClassicLevel(std::span<const uint8_t> data, size_t& pos,
                              std::vector<MenuItem>& items, uint32_t depth) {
    if (depth > kMaxDepth) {
        return MakeError(L"菜单嵌套层级过深");
    }
    while (true) {
        uint16_t flags = 0;
        if (!ReadU16(data, pos, flags)) {
            return MakeError(L"菜单项标志在数据末尾被截断");
        }
        MenuItem item;
        item.classicFlags = flags & static_cast<uint16_t>(~kMfMaskFixed);
        item.popup = (flags & kMfPopup) != 0;
        item.last = (flags & kMfEnd) != 0;

        if (item.popup) {
            auto text = ReadText(data, pos);
            if (!text) {
                return std::unexpected(text.error());
            }
            item.text = std::move(*text);
            auto children = ParseClassicLevel(data, pos, item.children, depth + 1);
            if (!children) {
                return children;
            }
        } else {
            uint16_t id = 0;
            if (!ReadU16(data, pos, id)) {
                return MakeError(L"菜单项 ID 在数据末尾被截断");
            }
            item.id = id;
            auto text = ReadText(data, pos);
            if (!text) {
                return std::unexpected(text.error());
            }
            item.text = std::move(*text);
            item.separator = (flags & kMfSeparator) != 0 || (id == 0 && item.text.empty());
        }

        items.push_back(std::move(item));
        if (items.size() > kMaxItems) {
            return MakeError(L"菜单项过多");
        }
        if (item.last) {
            break;
        }
    }
    return {};
}

Result<void> ParseExtendedLevel(std::span<const uint8_t> data, size_t& pos,
                               std::vector<MenuItem>& items, uint32_t depth) {
    if (depth > kMaxDepth) {
        return MakeError(L"菜单嵌套层级过深");
    }
    while (true) {
        uint32_t type = 0;
        uint32_t state = 0;
        uint32_t id = 0;
        uint16_t resInfo = 0;
        if (!ReadU32(data, pos, type) || !ReadU32(data, pos, state) ||
            !ReadU32(data, pos, id) || !ReadU16(data, pos, resInfo)) {
            return MakeError(L"MENUEX 菜单项头在数据末尾被截断");
        }

        MenuItem item;
        item.type = type;
        item.state = state;
        item.id = id;
        item.popup = (resInfo & 0x0001u) != 0;
        item.last = (resInfo & 0x0080u) != 0;

        auto text = ReadText(data, pos);
        if (!text) {
            return std::unexpected(text.error());
        }
        item.text = std::move(*text);
        item.separator = (type & 0x00000800u) != 0 && item.text.empty();

        if (item.popup) {
            while ((pos & 3u) != 0) {
                if (pos >= data.size()) {
                    return MakeError(L"MENUEX 弹出菜单对齐数据被截断");
                }
                ++pos;
            }
            uint32_t helpId = 0;
            if (!ReadU32(data, pos, helpId)) {
                return MakeError(L"MENUEX 弹出菜单 helpId 被截断");
            }
            item.helpId = helpId;
            auto children = ParseExtendedLevel(data, pos, item.children, depth + 1);
            if (!children) {
                return children;
            }
        }

        items.push_back(std::move(item));
        if (items.size() > kMaxItems) {
            return MakeError(L"菜单项过多");
        }
        if (item.last) {
            break;
        }
    }
    return {};
}

Result<void> BuildClassicLevel(const std::vector<MenuItem>& items, std::vector<uint8_t>& data,
                              uint32_t depth) {
    if (depth > kMaxDepth) {
        return MakeError(L"菜单嵌套层级过深");
    }
    for (size_t i = 0; i < items.size(); ++i) {
        const MenuItem& item = items[i];
        const bool last = i + 1 == items.size();
        uint16_t flags = static_cast<uint16_t>(item.classicFlags & ~kMfMaskFixed);
        if (item.popup) {
            flags |= kMfPopup;
        }
        if (last) {
            flags |= kMfEnd;
        }
        WriteU16(data, flags);
        if (item.popup) {
            WriteText(data, item.text);
            if (!item.children.empty()) {
                auto result = BuildClassicLevel(item.children, data, depth + 1);
                if (!result) {
                    return result;
                }
            }
        } else {
            if (item.id > 0xFFFF) {
                return MakeError(L"classic 菜单 ID 超出 16 位范围");
            }
            WriteU16(data, static_cast<uint16_t>(item.id));
            WriteText(data, item.separator ? L"" : item.text);
        }
    }
    return {};
}

Result<void> BuildExtendedLevel(const std::vector<MenuItem>& items, std::vector<uint8_t>& data,
                               uint32_t depth) {
    if (depth > kMaxDepth) {
        return MakeError(L"菜单嵌套层级过深");
    }
    for (size_t i = 0; i < items.size(); ++i) {
        const MenuItem& item = items[i];
        const bool last = i + 1 == items.size();
        WriteU32(data, item.type);
        WriteU32(data, item.state);
        WriteU32(data, item.id);
        uint16_t resInfo = static_cast<uint16_t>((item.popup ? 0x0001u : 0u) |
                                                 (last ? 0x0080u : 0u));
        WriteU16(data, resInfo);
        WriteText(data, item.text);
        if (item.popup) {
            Align4(data);
            WriteU32(data, item.helpId);
            if (!item.children.empty()) {
                auto result = BuildExtendedLevel(item.children, data, depth + 1);
                if (!result) {
                    return result;
                }
            }
        }
    }
    return {};
}

std::wstring Indent(uint32_t depth) {
    return std::wstring(static_cast<size_t>(depth) * 2, L' ');
}

void FormatItems(const std::vector<MenuItem>& items, uint32_t depth,
                 MenuTemplateFormat format, std::wstring& output) {
    for (const MenuItem& item : items) {
        const std::wstring indent = Indent(depth);
        if (format == MenuTemplateFormat::Classic) {
            const uint16_t extra =
                static_cast<uint16_t>(item.classicFlags & ~kMfMaskFixed);
            if (item.popup) {
                output += indent + L"POPUP \"" + EscapeText(item.text) + L"\"";
                if (extra != 0) {
                    output += L", " + Hex16(extra);
                }
                output += L"\r\n";
                output += indent + L"{\r\n";
                FormatItems(item.children, depth + 1, format, output);
                output += indent + L"}\r\n";
            } else if (item.separator) {
                output += indent + L"MENUITEM SEPARATOR";
                if (extra != 0) {
                    output += L", " + Hex16(extra);
                }
                output += L"\r\n";
            } else {
                output += indent + L"MENUITEM \"" + EscapeText(item.text) + L"\", " +
                          std::to_wstring(item.id);
                if (extra != 0) {
                    output += L", " + Hex16(extra);
                }
                output += L"\r\n";
            }
        } else {
            if (item.popup) {
                output += indent + L"POPUP \"" + EscapeText(item.text) + L"\", " +
                          std::to_wstring(item.id) + L", " + Hex32(item.type) + L", " +
                          Hex32(item.state) + L", " + Hex32(item.helpId) + L"\r\n";
                output += indent + L"{\r\n";
                FormatItems(item.children, depth + 1, format, output);
                output += indent + L"}\r\n";
            } else if (item.separator) {
                output += indent + L"MENUITEM SEPARATOR, " + Hex32(item.type) + L", " +
                          Hex32(item.state) + L"\r\n";
            } else {
                output += indent + L"MENUITEM \"" + EscapeText(item.text) + L"\", " +
                          std::to_wstring(item.id) + L", " + Hex32(item.type) + L", " +
                          Hex32(item.state) + L"\r\n";
            }
        }
    }
}

}  // namespace

Result<MenuResource> ParseMenuResource(std::span<const uint8_t> data) {
    MenuResource menu;
    size_t pos = 0;
    uint16_t version = 0;
    if (!ReadU16(data, pos, version)) {
        return MakeError(L"菜单资源过短");
    }
    if (version == 0) {
        menu.format = MenuTemplateFormat::Classic;
        uint16_t offset = 0;
        if (!ReadU16(data, pos, offset)) {
            return MakeError(L"菜单头不完整");
        }
        (void)offset;
        auto result = ParseClassicLevel(data, pos, menu.items, 0);
        if (!result) {
            return std::unexpected(result.error());
        }
        return menu;
    }
    if (version == 1) {
        menu.format = MenuTemplateFormat::Extended;
        uint16_t offset = 0;
        uint32_t helpId = 0;
        if (!ReadU16(data, pos, offset) || !ReadU32(data, pos, helpId)) {
            return MakeError(L"MENUEX 头不完整");
        }
        menu.helpId = helpId;
        const size_t itemStart = offset == 0 ? 8 : static_cast<size_t>(4) + offset;
        if (itemStart < 8 || itemStart > data.size()) {
            return MakeError(L"MENUEX 菜单项起始偏移无效");
        }
        pos = itemStart;
        auto result = ParseExtendedLevel(data, pos, menu.items, 0);
        if (!result) {
            return std::unexpected(result.error());
        }
        return menu;
    }
    return MakeError(Utf8ToWide(fmt::format("不支持的菜单模板版本 {}", version)));
}

Result<std::vector<uint8_t>> BuildMenuResource(const MenuResource& menu) {
    std::vector<uint8_t> data;
    data.reserve(4096);
    if (menu.format == MenuTemplateFormat::Classic) {
        WriteU16(data, 0);
        WriteU16(data, 0);
        auto result = BuildClassicLevel(menu.items, data, 0);
        if (!result) {
            return std::unexpected(result.error());
        }
    } else {
        WriteU16(data, 1);
        WriteU16(data, 4);
        WriteU32(data, menu.helpId);
        auto result = BuildExtendedLevel(menu.items, data, 0);
        if (!result) {
            return std::unexpected(result.error());
        }
    }
    if (data.size() > 0x7FFFFFFF) {
        return MakeError(L"菜单资源过大");
    }
    return data;
}

std::wstring FormatMenuText(const MenuResource& menu) {
    std::wstring output;
    if (menu.format == MenuTemplateFormat::Classic) {
        output = L"MENU\r\n";
    } else {
        output = L"MENUEX\r\n";
        output += L"HELPID " + Hex32(menu.helpId) + L"\r\n";
    }
    output += L"{\r\n";
    FormatItems(menu.items, 1, menu.format, output);
    output += L"}\r\n";
    return output;
}

Result<MenuResource> ParseMenuText(std::wstring_view text) {
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find_first_of(L"\r\n", start);
        const size_t length = end == std::wstring_view::npos ? text.size() - start : end - start;
        lines.emplace_back(text.substr(start, length));
        if (end == std::wstring_view::npos) {
            break;
        }
        if (text[end] == L'\r' && end + 1 < text.size() && text[end + 1] == L'\n') {
            start = end + 2;
        } else {
            start = end + 1;
        }
    }

    size_t index = 0;
    while (index < lines.size() &&
           Trim(RemoveComments(lines[index])).empty()) {
        ++index;
    }
    if (index >= lines.size()) {
        return MakeError(L"菜单文本为空");
    }
    const std::wstring header = Trim(RemoveComments(lines[index]));
    MenuTemplateFormat format;
    if (IEqualsAscii(header, L"MENU")) {
        format = MenuTemplateFormat::Classic;
    } else if (IEqualsAscii(header, L"MENUEX")) {
        format = MenuTemplateFormat::Extended;
    } else {
        return MakeError(L"菜单文本第一行必须是 MENU 或 MENUEX");
    }
    ++index;
    lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(index));

    MenuTextParser parser(std::move(lines), format);
    return parser.Parse();
}

}  // namespace peres
