#include "DialogCodec.h"

#include "TextUtil.h"

#include <fmt/format.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cwctype>
#include <sstream>

namespace peres {

namespace {

constexpr uint32_t kDsSetFont = 0x00000040u;
constexpr uint32_t kWsChild = 0x40000000u;
constexpr uint32_t kMaxControls = 512;
constexpr uint32_t kMaxText = 4096;

uint16_t ReadU16(std::span<const uint8_t> data, size_t& pos) {
    const uint16_t value = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
    pos += 2;
    return value;
}

uint32_t ReadU32(std::span<const uint8_t> data, size_t& pos) {
    const uint32_t value = static_cast<uint32_t>(data[pos]) |
                           (static_cast<uint32_t>(data[pos + 1]) << 8) |
                           (static_cast<uint32_t>(data[pos + 2]) << 16) |
                           (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return value;
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

int16_t ToInt16(uint16_t value) {
    return static_cast<int16_t>(value);
}

Result<std::wstring> ReadWideText(std::span<const uint8_t> data, size_t& pos) {
    std::wstring text;
    while (pos + 2 <= data.size()) {
        const uint16_t ch = ReadU16(data, pos);
        if (ch == 0) {
            return text;
        }
        if (text.size() >= kMaxText) {
            return MakeError(L"对话框字符串过长");
        }
        text.push_back(static_cast<wchar_t>(ch));
    }
    return MakeError(L"对话框字符串被截断");
}

Result<DialogString> ReadDialogString(std::span<const uint8_t> data, size_t& pos) {
    if (pos + 2 > data.size()) {
        return MakeError(L"对话框字符串数组被截断");
    }
    const uint16_t first = ReadU16(data, pos);
    DialogString value;
    if (first == 0) {
        return value;
    }
    if (first == 0xFFFF) {
        if (pos + 2 > data.size()) {
            return MakeError(L"对话框字符串序数被截断");
        }
        value.ordinal = true;
        value.ordinalValue = ReadU16(data, pos);
        return value;
    }
    std::wstring text;
    text.push_back(static_cast<wchar_t>(first));
    while (pos + 2 <= data.size()) {
        const uint16_t ch = ReadU16(data, pos);
        if (ch == 0) {
            value.text = std::move(text);
            return value;
        }
        if (text.size() >= kMaxText) {
            return MakeError(L"对话框字符串过长");
        }
        text.push_back(static_cast<wchar_t>(ch));
    }
    return MakeError(L"对话框字符串被截断");
}

void WriteWideText(std::vector<uint8_t>& data, std::wstring_view text) {
    for (const wchar_t ch : text) {
        WriteU16(data, static_cast<uint16_t>(ch));
    }
    WriteU16(data, 0);
}

void WriteDialogString(std::vector<uint8_t>& data, const DialogString& value) {
    if (value.ordinal) {
        WriteU16(data, 0xFFFF);
        WriteU16(data, value.ordinalValue);
    } else if (value.text.empty()) {
        WriteU16(data, 0);
    } else {
        WriteWideText(data, value.text);
    }
}

std::wstring Hex32(uint32_t value) {
    return L"0x" + Utf8ToWide(fmt::format("{:08X}", value));
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

bool ParseUnsigned(std::wstring_view text, uint32_t& value) {
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

bool ParseSigned(std::wstring_view text, int32_t& value) {
    const std::wstring trimmed = Trim(text);
    if (trimmed.empty()) {
        return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const long long parsed = std::wcstoll(trimmed.c_str(), &end, 0);
    while (end != nullptr && *end != L'\0' && std::iswspace(*end) != 0) {
        ++end;
    }
    if (errno != 0 || end == nullptr || *end != L'\0' || parsed < INT16_MIN ||
        parsed > INT16_MAX) {
        return false;
    }
    value = static_cast<int32_t>(parsed);
    return true;
}

bool ParseQuoted(std::wstring_view text, std::wstring& value, std::wstring& error) {
    std::wstring trimmed = Trim(text);
    if (trimmed.size() < 2 || trimmed.front() != L'"' || trimmed.back() != L'"') {
        error = L"缺少双引号字符串";
        return false;
    }
    std::wstring body = trimmed.substr(1, trimmed.size() - 2);
    value.clear();
    for (size_t i = 0; i < body.size(); ++i) {
        const wchar_t ch = body[i];
        if (ch != L'\\') {
            value.push_back(ch);
            continue;
        }
        if (++i >= body.size()) {
            error = L"字符串转义不完整";
            return false;
        }
        switch (body[i]) {
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
                value.push_back(body[i]);
                break;
        }
    }
    return true;
}

bool ParseDialogStringToken(std::wstring_view token, DialogString& value,
                            bool noneAllowed, std::wstring& error) {
    const std::wstring trimmed = Trim(token);
    if (trimmed.empty()) {
        error = L"字符串参数为空";
        return false;
    }
    if (noneAllowed && IEqualsAscii(trimmed, L"NONE")) {
        value = DialogString{};
        return true;
    }
    if (trimmed.front() == L'#') {
        uint32_t ordinal = 0;
        if (!ParseUnsigned(trimmed.substr(1), ordinal) || ordinal > 0xFFFF) {
            error = L"序数 ID 无效";
            return false;
        }
        value.ordinal = true;
        value.ordinalValue = static_cast<uint16_t>(ordinal);
        return true;
    }
    if (trimmed.front() == L'"') {
        std::wstring text;
        if (!ParseQuoted(trimmed, text, error)) {
            return false;
        }
        value.ordinal = false;
        value.text = std::move(text);
        return true;
    }
    // Allow bare class atoms.
    if (IEqualsAscii(trimmed, L"BUTTON")) {
        value = DialogString{true, 0x0080, {}};
        return true;
    }
    if (IEqualsAscii(trimmed, L"EDIT")) {
        value = DialogString{true, 0x0081, {}};
        return true;
    }
    if (IEqualsAscii(trimmed, L"STATIC")) {
        value = DialogString{true, 0x0082, {}};
        return true;
    }
    if (IEqualsAscii(trimmed, L"LISTBOX")) {
        value = DialogString{true, 0x0083, {}};
        return true;
    }
    if (IEqualsAscii(trimmed, L"SCROLLBAR")) {
        value = DialogString{true, 0x0084, {}};
        return true;
    }
    if (IEqualsAscii(trimmed, L"COMBOBOX")) {
        value = DialogString{true, 0x0085, {}};
        return true;
    }
    error = L"字符串参数既不是引号文本、序数，也不是已知控件类";
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

std::vector<std::wstring> SplitTopLevel(std::wstring_view text, wchar_t separator) {
    std::vector<std::wstring> result;
    std::wstring current;
    bool inQuote = false;
    bool escape = false;
    for (const wchar_t ch : text) {
        if (inQuote) {
            current.push_back(ch);
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
            current.push_back(ch);
        } else if (ch == separator) {
            result.push_back(Trim(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    result.push_back(Trim(current));
    return result;
}

void AppendLine(std::wstring& output, std::wstring_view line) {
    if (!output.empty()) {
        output += L"\r\n";
    }
    output += line;
}

std::wstring FormatClassToken(const DialogString& value) {
    if (value.ordinal) {
        switch (value.ordinalValue) {
            case 0x0080:
                return L"BUTTON";
            case 0x0081:
                return L"EDIT";
            case 0x0082:
                return L"STATIC";
            case 0x0083:
                return L"LISTBOX";
            case 0x0084:
                return L"SCROLLBAR";
            case 0x0085:
                return L"COMBOBOX";
            default:
                return L"#" + std::to_wstring(value.ordinalValue);
        }
    }
    return L"\"" + EscapeText(value.text) + L"\"";
}

std::wstring FormatStringToken(const DialogString& value, bool noneAllowed) {
    if (value.ordinal) {
        return L"#" + std::to_wstring(value.ordinalValue);
    }
    if (noneAllowed && value.text.empty()) {
        return L"NONE";
    }
    return L"\"" + EscapeText(value.text) + L"\"";
}

Result<DialogString> ParseSzOrOrd(std::span<const uint8_t> data, size_t& pos) {
    if (pos + 2 > data.size()) {
        return MakeError(L"对话框模板字符串数组被截断");
    }
    const uint16_t first = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
    if (first == 0) {
        pos += 2;
        return DialogString{};
    }
    if (first == 0xFFFF) {
        if (pos + 4 > data.size()) {
            return MakeError(L"对话框模板字符串序数被截断");
        }
        DialogString value;
        value.ordinal = true;
        value.ordinalValue = static_cast<uint16_t>(data[pos + 2] | (data[pos + 3] << 8));
        pos += 4;
        return value;
    }
    size_t cursor = pos;
    auto text = ReadWideText(data, cursor);
    if (!text) {
        return std::unexpected(text.error());
    }
    pos = cursor;
    return DialogString{false, 0, std::move(*text)};
}

struct ParsedControl {
    DialogControl control;
    size_t next = 0;
};

Result<ParsedControl> TryParseControl(std::span<const uint8_t> data, size_t start,
                                     bool extended) {
    const size_t headerSize = extended ? 24 : 18;
    if (start + headerSize > data.size()) {
        return MakeError(L"控件头超出数据范围");
    }

    DialogControl control;
    size_t pos = start;
    if (extended) {
        control.helpId = ReadU32(data, pos);
        control.exStyle = ReadU32(data, pos);
        control.style = ReadU32(data, pos);
    } else {
        control.style = ReadU32(data, pos);
        control.exStyle = ReadU32(data, pos);
    }
    if ((control.style & kWsChild) == 0) {
        return MakeError(L"控件样式缺少 WS_CHILD");
    }
    const uint16_t x = ReadU16(data, pos);
    const uint16_t y = ReadU16(data, pos);
    const uint16_t cx = ReadU16(data, pos);
    const uint16_t cy = ReadU16(data, pos);
    control.x = ToInt16(x);
    control.y = ToInt16(y);
    control.cx = ToInt16(cx);
    control.cy = ToInt16(cy);
    if (control.cx < 0 || control.cy < 0) {
        return MakeError(L"控件尺寸为负");
    }
    if (extended) {
        control.id = ReadU32(data, pos);
    } else {
        control.id = ReadU16(data, pos);
    }

    auto windowClass = ParseSzOrOrd(data, pos);
    if (!windowClass) {
        return std::unexpected(windowClass.error());
    }
    control.windowClass = std::move(*windowClass);
    auto title = ParseSzOrOrd(data, pos);
    if (!title) {
        return std::unexpected(title.error());
    }
    control.title = std::move(*title);

    if (pos + 2 > data.size()) {
        return MakeError(L"控件创建数据长度被截断");
    }
    const uint16_t extraCount = ReadU16(data, pos);
    if (pos + extraCount > data.size()) {
        return MakeError(L"控件创建数据超出范围");
    }
    control.creationData.assign(data.begin() + pos, data.begin() + pos + extraCount);
    pos += extraCount;

    ParsedControl parsed;
    parsed.control = std::move(control);
    parsed.next = pos;
    return parsed;
}

bool ParseControlsRecursive(std::span<const uint8_t> data, size_t pos, uint32_t index,
                            uint32_t count, bool extended,
                            std::vector<DialogControl>& controls) {
    if (index == count) {
        return true;
    }
    const size_t limit = std::min(pos + 64, data.size());
    for (size_t start = pos; start <= limit; ++start) {
        auto parsed = TryParseControl(data, start, extended);
        if (!parsed) {
            continue;
        }
        controls.push_back(std::move(parsed->control));
        if (ParseControlsRecursive(data, parsed->next, index + 1, count, extended,
                                   controls)) {
            return true;
        }
        controls.pop_back();
    }
    return false;
}

Result<std::vector<DialogControl>> ParseControls(std::span<const uint8_t> data, size_t pos,
                                                uint32_t count, bool extended) {
    if (count > kMaxControls) {
        return MakeError(L"对话框控件数量过多");
    }
    std::vector<DialogControl> controls;
    if (!ParseControlsRecursive(data, pos, 0, count, extended, controls)) {
        return MakeError(L"无法解析对话框控件模板");
    }
    return controls;
}

void WriteCreationData(std::vector<uint8_t>& data, const std::vector<uint8_t>& creationData) {
    if (creationData.size() > 0xFFFF) {
        return;
    }
    WriteU16(data, static_cast<uint16_t>(creationData.size()));
    data.insert(data.end(), creationData.begin(), creationData.end());
}

}  // namespace

Result<DialogTemplate> ParseDialogResource(std::span<const uint8_t> data) {
    if (data.size() < 18) {
        return MakeError(L"对话框模板过短");
    }
    DialogTemplate dialog;
    size_t pos = 0;
    if (data.size() >= 4 && data[0] == 1 && data[1] == 0 && data[2] == 0xFF &&
        data[3] == 0xFF) {
        dialog.extended = true;
        if (data.size() < 26) {
            return MakeError(L"DLGTEMPLATEEX 头过短");
        }
        pos = 4;
        dialog.helpId = ReadU32(data, pos);
        dialog.exStyle = ReadU32(data, pos);
        dialog.style = ReadU32(data, pos);
        const uint16_t count = ReadU16(data, pos);
        const uint16_t x = ReadU16(data, pos);
        const uint16_t y = ReadU16(data, pos);
        const uint16_t cx = ReadU16(data, pos);
        const uint16_t cy = ReadU16(data, pos);
        dialog.x = ToInt16(x);
        dialog.y = ToInt16(y);
        dialog.cx = ToInt16(cx);
        dialog.cy = ToInt16(cy);

        auto menu = ParseSzOrOrd(data, pos);
        if (!menu) {
            return std::unexpected(menu.error());
        }
        dialog.menu = std::move(*menu);
        auto windowClass = ParseSzOrOrd(data, pos);
        if (!windowClass) {
            return std::unexpected(windowClass.error());
        }
        dialog.windowClass = std::move(*windowClass);
        auto title = ParseSzOrOrd(data, pos);
        if (!title) {
            return std::unexpected(title.error());
        }
        dialog.title = std::move(*title);

        if ((dialog.style & kDsSetFont) != 0) {
            if (pos + 6 > data.size()) {
                return MakeError(L"对话框字体头被截断");
            }
            dialog.hasFont = true;
            dialog.pointSize = ReadU16(data, pos);
            dialog.weight = ReadU16(data, pos);
            dialog.italic = data[pos++];
            dialog.charset = data[pos++];
            auto typeface = ReadWideText(data, pos);
            if (!typeface) {
                return std::unexpected(typeface.error());
            }
            dialog.typeface = std::move(*typeface);
        }

        auto controls = ParseControls(data, pos, count, true);
        if (!controls) {
            return std::unexpected(controls.error());
        }
        dialog.controls = std::move(*controls);
        return dialog;
    }

    dialog.extended = false;
    dialog.style = ReadU32(data, pos);
    dialog.exStyle = ReadU32(data, pos);
    const uint16_t count = ReadU16(data, pos);
    const uint16_t x = ReadU16(data, pos);
    const uint16_t y = ReadU16(data, pos);
    const uint16_t cx = ReadU16(data, pos);
    const uint16_t cy = ReadU16(data, pos);
    dialog.x = ToInt16(x);
    dialog.y = ToInt16(y);
    dialog.cx = ToInt16(cx);
    dialog.cy = ToInt16(cy);

    auto menu = ParseSzOrOrd(data, pos);
    if (!menu) {
        return std::unexpected(menu.error());
    }
    dialog.menu = std::move(*menu);
    auto windowClass = ParseSzOrOrd(data, pos);
    if (!windowClass) {
        return std::unexpected(windowClass.error());
    }
    dialog.windowClass = std::move(*windowClass);
    auto title = ParseSzOrOrd(data, pos);
    if (!title) {
        return std::unexpected(title.error());
    }
    dialog.title = std::move(*title);

    if ((dialog.style & kDsSetFont) != 0) {
        if (pos + 2 > data.size()) {
            return MakeError(L"对话框字体头被截断");
        }
        dialog.hasFont = true;
        dialog.pointSize = ReadU16(data, pos);
        dialog.weight = 400;
        dialog.italic = 0;
        dialog.charset = 1;
        auto typeface = ReadWideText(data, pos);
        if (!typeface) {
            return std::unexpected(typeface.error());
        }
        dialog.typeface = std::move(*typeface);
    }

    auto controls = ParseControls(data, pos, count, false);
    if (!controls) {
        return std::unexpected(controls.error());
    }
    dialog.controls = std::move(*controls);
    return dialog;
}

Result<std::vector<uint8_t>> BuildDialogResource(const DialogTemplate& dialog) {
    if (dialog.controls.size() > 0xFFFF) {
        return MakeError(L"对话框控件数量过多");
    }
    std::vector<uint8_t> data;
    data.reserve(4096);
    if (dialog.extended) {
        WriteU16(data, 1);
        WriteU16(data, 0xFFFF);
        WriteU32(data, dialog.helpId);
        WriteU32(data, dialog.exStyle);
        WriteU32(data, dialog.style);
        WriteU16(data, static_cast<uint16_t>(dialog.controls.size()));
        WriteU16(data, static_cast<uint16_t>(dialog.x));
        WriteU16(data, static_cast<uint16_t>(dialog.y));
        WriteU16(data, static_cast<uint16_t>(dialog.cx));
        WriteU16(data, static_cast<uint16_t>(dialog.cy));
    } else {
        WriteU32(data, dialog.style);
        WriteU32(data, dialog.exStyle);
        WriteU16(data, static_cast<uint16_t>(dialog.controls.size()));
        WriteU16(data, static_cast<uint16_t>(dialog.x));
        WriteU16(data, static_cast<uint16_t>(dialog.y));
        WriteU16(data, static_cast<uint16_t>(dialog.cx));
        WriteU16(data, static_cast<uint16_t>(dialog.cy));
    }

    WriteDialogString(data, dialog.menu);
    WriteDialogString(data, dialog.windowClass);
    WriteDialogString(data, dialog.title);

    if (dialog.hasFont) {
        if (dialog.extended) {
            WriteU16(data, dialog.pointSize);
            WriteU16(data, dialog.weight);
            data.push_back(dialog.italic);
            data.push_back(dialog.charset);
            WriteWideText(data, dialog.typeface);
        } else {
            WriteU16(data, dialog.pointSize);
            WriteWideText(data, dialog.typeface);
        }
    }
    Align4(data);

    for (const auto& control : dialog.controls) {
        if (control.creationData.size() > 0xFFFF) {
            return MakeError(L"控件创建数据超过 65535 字节");
        }
        if (dialog.extended) {
            WriteU32(data, control.helpId);
            WriteU32(data, control.exStyle);
            WriteU32(data, control.style);
            WriteU16(data, static_cast<uint16_t>(control.x));
            WriteU16(data, static_cast<uint16_t>(control.y));
            WriteU16(data, static_cast<uint16_t>(control.cx));
            WriteU16(data, static_cast<uint16_t>(control.cy));
            WriteU32(data, control.id);
        } else {
            if (control.id > 0xFFFF) {
                return MakeError(L"经典对话框控件 ID 超出 16 位范围");
            }
            WriteU32(data, control.style);
            WriteU32(data, control.exStyle);
            WriteU16(data, static_cast<uint16_t>(control.x));
            WriteU16(data, static_cast<uint16_t>(control.y));
            WriteU16(data, static_cast<uint16_t>(control.cx));
            WriteU16(data, static_cast<uint16_t>(control.cy));
            WriteU16(data, static_cast<uint16_t>(control.id));
        }
        WriteDialogString(data, control.windowClass);
        WriteDialogString(data, control.title);
        WriteCreationData(data, control.creationData);
        Align4(data);
    }
    return data;
}

std::wstring FormatDialogText(const DialogTemplate& dialog) {
    std::wstring output;
    AppendLine(output, std::wstring(dialog.extended ? L"DIALOGEX " : L"DIALOG ") +
                           std::to_wstring(dialog.x) + L", " + std::to_wstring(dialog.y) + L", " +
                           std::to_wstring(dialog.cx) + L", " + std::to_wstring(dialog.cy));
    AppendLine(output, L"STYLE " + Hex32(dialog.style));
    AppendLine(output, L"EXSTYLE " + Hex32(dialog.exStyle));
    if (dialog.extended) {
        AppendLine(output, L"HELPID " + Hex32(dialog.helpId));
    }
    AppendLine(output, L"CAPTION " + FormatStringToken(dialog.title, false));
    AppendLine(output, L"MENU " + FormatStringToken(dialog.menu, true));
    AppendLine(output, L"CLASS " + FormatStringToken(dialog.windowClass, true));
    if (dialog.hasFont) {
        if (dialog.extended) {
            AppendLine(output, L"FONT " + std::to_wstring(dialog.pointSize) + L", " +
                                   std::to_wstring(dialog.weight) + L", " +
                                   std::to_wstring(dialog.italic) + L", " +
                                   std::to_wstring(dialog.charset) + L", \"" +
                                   EscapeText(dialog.typeface) + L"\"");
        } else {
            AppendLine(output, L"FONT " + std::to_wstring(dialog.pointSize) + L", \"" +
                                   EscapeText(dialog.typeface) + L"\"");
        }
    }
    AppendLine(output, L"{");

    for (const auto& control : dialog.controls) {
        std::wstring line = L"  CONTROL " + std::to_wstring(control.id) + L", " +
                            FormatClassToken(control.windowClass) + L", " +
                            FormatStringToken(control.title, false) + L", " +
                            std::to_wstring(control.x) + L", " + std::to_wstring(control.y) +
                            L", " + std::to_wstring(control.cx) + L", " +
                            std::to_wstring(control.cy) + L", " + Hex32(control.style) + L", " +
                            Hex32(control.exStyle);
        if (dialog.extended) {
            line += L", " + Hex32(control.helpId);
        }
        if (!control.creationData.empty()) {
            line += L", data=";
            for (const uint8_t byte : control.creationData) {
                line += Utf8ToWide(fmt::format("{:02X}", byte));
            }
        }
        AppendLine(output, line);
    }
    AppendLine(output, L"}");
    return output;
}

Result<DialogTemplate> ParseDialogText(std::wstring_view text) {
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
    const auto nextSignificant = [&lines](size_t i) {
        while (i < lines.size()) {
            const std::wstring line = Trim(RemoveComments(lines[i]));
            if (!line.empty()) {
                break;
            }
            ++i;
        }
        return i;
    };
    index = nextSignificant(0);
    if (index >= lines.size()) {
        return MakeError(L"对话框文本为空");
    }

    const std::wstring header = Trim(RemoveComments(lines[index]));
    size_t space = 0;
    while (space < header.size() && std::iswspace(header[space]) == 0) {
        ++space;
    }
    const std::wstring keyword = header.substr(0, space);
    const bool extended = IEqualsAscii(keyword, L"DIALOGEX");
    if (!extended && !IEqualsAscii(keyword, L"DIALOG")) {
        return MakeError(L"对话框文本第一行必须是 DIALOG 或 DIALOGEX");
    }

    DialogTemplate dialog;
    dialog.extended = extended;
    std::vector<std::wstring> headerValues = SplitTopLevel(header.substr(space), L',');
    if (headerValues.size() != 4) {
        return MakeError(L"对话框头需要 4 个坐标值");
    }
    int32_t numbers[4] = {};
    for (size_t i = 0; i < 4; ++i) {
        if (!ParseSigned(headerValues[i], numbers[i])) {
            return MakeError(L"对话框坐标值无效");
        }
    }
    dialog.x = static_cast<int16_t>(numbers[0]);
    dialog.y = static_cast<int16_t>(numbers[1]);
    dialog.cx = static_cast<int16_t>(numbers[2]);
    dialog.cy = static_cast<int16_t>(numbers[3]);

    bool inControls = false;
    for (++index; index < lines.size(); ++index) {
        const std::wstring line = Trim(RemoveComments(lines[index]));
        if (line.empty()) {
            continue;
        }
        if (line == L"{") {
            inControls = true;
            continue;
        }
        if (line == L"}") {
            inControls = false;
            continue;
        }

        if (!inControls) {
            size_t directiveSpace = 0;
            while (directiveSpace < line.size() && std::iswspace(line[directiveSpace]) == 0) {
                ++directiveSpace;
            }
            const std::wstring directive = line.substr(0, directiveSpace);
            const std::wstring rest = Trim(line.substr(directiveSpace));
            if (IEqualsAscii(directive, L"STYLE")) {
                uint32_t value = 0;
                if (!ParseUnsigned(rest, value)) {
                    return MakeError(L"STYLE 数值无效");
                }
                dialog.style = value;
            } else if (IEqualsAscii(directive, L"EXSTYLE")) {
                uint32_t value = 0;
                if (!ParseUnsigned(rest, value)) {
                    return MakeError(L"EXSTYLE 数值无效");
                }
                dialog.exStyle = value;
            } else if (IEqualsAscii(directive, L"HELPID")) {
                uint32_t value = 0;
                if (!ParseUnsigned(rest, value)) {
                    return MakeError(L"HELPID 数值无效");
                }
                dialog.helpId = value;
            } else if (IEqualsAscii(directive, L"CAPTION")) {
                DialogString value;
                std::wstring error;
                if (!ParseDialogStringToken(rest, value, false, error)) {
                    return MakeError(L"CAPTION: " + error);
                }
                dialog.title = std::move(value);
            } else if (IEqualsAscii(directive, L"MENU")) {
                DialogString value;
                std::wstring error;
                if (!ParseDialogStringToken(rest, value, true, error)) {
                    return MakeError(L"MENU: " + error);
                }
                dialog.menu = std::move(value);
            } else if (IEqualsAscii(directive, L"CLASS")) {
                DialogString value;
                std::wstring error;
                if (!ParseDialogStringToken(rest, value, true, error)) {
                    return MakeError(L"CLASS: " + error);
                }
                dialog.windowClass = std::move(value);
            } else if (IEqualsAscii(directive, L"FONT")) {
                std::vector<std::wstring> values = SplitTopLevel(rest, L',');
                if (extended) {
                    if (values.size() != 5) {
                        return MakeError(L"DIALOGEX FONT 需要 5 个参数");
                    }
                    uint32_t pointSize = 0;
                    uint32_t weight = 0;
                    uint32_t italic = 0;
                    uint32_t charset = 0;
                    if (!ParseUnsigned(values[0], pointSize) ||
                        !ParseUnsigned(values[1], weight) ||
                        !ParseUnsigned(values[2], italic) ||
                        !ParseUnsigned(values[3], charset) || pointSize > 0xFFFF ||
                        weight > 0xFFFF || italic > 0xFF || charset > 0xFF) {
                        return MakeError(L"FONT 数值无效");
                    }
                    std::wstring typeface;
                    std::wstring error;
                    if (!ParseQuoted(values[4], typeface, error)) {
                        return MakeError(L"FONT: " + error);
                    }
                    dialog.hasFont = true;
                    dialog.pointSize = static_cast<uint16_t>(pointSize);
                    dialog.weight = static_cast<uint16_t>(weight);
                    dialog.italic = static_cast<uint8_t>(italic);
                    dialog.charset = static_cast<uint8_t>(charset);
                    dialog.typeface = std::move(typeface);
                } else {
                    if (values.size() != 2) {
                        return MakeError(L"DIALOG FONT 需要 2 个参数");
                    }
                    uint32_t pointSize = 0;
                    if (!ParseUnsigned(values[0], pointSize) || pointSize > 0xFFFF) {
                        return MakeError(L"FONT 字号无效");
                    }
                    std::wstring typeface;
                    std::wstring error;
                    if (!ParseQuoted(values[1], typeface, error)) {
                        return MakeError(L"FONT: " + error);
                    }
                    dialog.hasFont = true;
                    dialog.pointSize = static_cast<uint16_t>(pointSize);
                    dialog.typeface = std::move(typeface);
                }
                dialog.style |= kDsSetFont;
            } else {
                return MakeError(L"无法识别的对话框指令：" + directive);
            }
            continue;
        }

        if (line.size() >= 7 && IEqualsAscii(line.substr(0, 7), L"CONTROL")) {
            std::vector<std::wstring> values = SplitTopLevel(Trim(line.substr(7)), L',');
            const size_t minFields = extended ? 10 : 9;
            if (values.size() < minFields) {
                return MakeError(L"CONTROL 参数数量不足");
            }
            DialogControl control;
            uint32_t id = 0;
            if (!ParseUnsigned(values[0], id)) {
                return MakeError(L"CONTROL ID 无效");
            }
            control.id = id;
            std::wstring error;
            if (!ParseDialogStringToken(values[1], control.windowClass, false, error)) {
                return MakeError(L"CONTROL class: " + error);
            }
            if (!ParseDialogStringToken(values[2], control.title, false, error)) {
                return MakeError(L"CONTROL title: " + error);
            }
            int32_t coords[4] = {};
            for (size_t i = 0; i < 4; ++i) {
                if (!ParseSigned(values[3 + i], coords[i])) {
                    return MakeError(L"CONTROL 坐标无效");
                }
            }
            control.x = static_cast<int16_t>(coords[0]);
            control.y = static_cast<int16_t>(coords[1]);
            control.cx = static_cast<int16_t>(coords[2]);
            control.cy = static_cast<int16_t>(coords[3]);
            if (!ParseUnsigned(values[7], control.style) ||
                !ParseUnsigned(values[8], control.exStyle)) {
                return MakeError(L"CONTROL 样式无效");
            }
            size_t next = 9;
            if (extended) {
                if (!ParseUnsigned(values[9], control.helpId)) {
                    return MakeError(L"CONTROL helpId 无效");
                }
                ++next;
            }
            if (values.size() > next) {
                const std::wstring extra = Trim(values[next]);
                if (extra.substr(0, 5) == L"data=") {
                    const std::wstring hex = extra.substr(5);
                    if ((hex.size() & 1) != 0) {
                        return MakeError(L"CONTROL data 十六进制长度为奇数");
                    }
                    for (size_t i = 0; i < hex.size(); i += 2) {
                        const std::wstring pair = hex.substr(i, 2);
                        uint32_t byte = 0;
                        if (!ParseUnsigned(L"0x" + pair, byte) || byte > 0xFF) {
                            return MakeError(L"CONTROL data 不是有效十六进制");
                        }
                        control.creationData.push_back(static_cast<uint8_t>(byte));
                    }
                    ++next;
                }
            }
            if (values.size() > next) {
                return MakeError(L"CONTROL 多余参数");
            }
            if (!extended && control.helpId != 0) {
                return MakeError(L"经典对话框不能设置控件 helpId");
            }
            if (dialog.controls.size() >= kMaxControls) {
                return MakeError(L"对话框控件数量过多");
            }
            dialog.controls.push_back(std::move(control));
        } else {
            return MakeError(L"控件块中只允许 CONTROL 行");
        }
    }

    if (inControls) {
        return MakeError(L"对话框控件块缺少 }");
    }
    if (dialog.controls.empty() && dialog.style == 0) {
        // A completely empty text is not a dialog; other header fields are allowed.
    }
    return dialog;
}

}  // namespace peres
