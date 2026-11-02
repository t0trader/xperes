#include "TextUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fmt/format.h>

#include <algorithm>
#include <cwctype>

namespace peres {

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                              static_cast<int>(text.size()), result.data(), length) != length) {
        return {};
    }
    return result;
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                              static_cast<int>(text.size()), result.data(), length, nullptr, nullptr) != length) {
        return {};
    }
    return result;
}

namespace {

std::optional<std::wstring> DecodeUtf16Bytes(std::span<const uint8_t> bytes, bool bigEndian) {
    if ((bytes.size() % 2) != 0) {
        return std::nullopt;
    }
    std::wstring result;
    result.reserve(bytes.size() / 2);
    for (size_t i = 0; i < bytes.size(); i += 2) {
        const uint16_t value = bigEndian
                                   ? static_cast<uint16_t>((bytes[i] << 8) | bytes[i + 1])
                                   : static_cast<uint16_t>((bytes[i + 1] << 8) | bytes[i]);
        result.push_back(static_cast<wchar_t>(value));
    }
    return result;
}

bool LooksPrintable(std::wstring_view text, size_t& printable, size_t& meaningful) {
    printable = 0;
    meaningful = 0;
    for (const wchar_t ch : text) {
        if (ch == L'\0') {
            return false;
        }
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
            ++printable;
            continue;
        }
        ++meaningful;
        if (ch >= 0x20 && ch != 0x7F) {
            ++printable;
        }
    }
    return meaningful > 0 && printable * 100 >= meaningful * 90;
}

}  // namespace

std::optional<std::wstring> TryDecodeText(std::span<const uint8_t> data, size_t maxBytes) {
    if (data.empty() || data.size() > maxBytes) {
        return std::nullopt;
    }

    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        const std::wstring text = Utf8ToWide(std::string_view(
            reinterpret_cast<const char*>(data.data() + 3), data.size() - 3));
        size_t printable = 0;
        size_t meaningful = 0;
        if (!text.empty() && LooksPrintable(text, printable, meaningful)) {
            return text;
        }
        return std::nullopt;
    }

    if (data.size() >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        auto text = DecodeUtf16Bytes(data.subspan(2), false);
        size_t printable = 0;
        size_t meaningful = 0;
        if (text && LooksPrintable(*text, printable, meaningful)) {
            return text;
        }
        return std::nullopt;
    }

    if (data.size() >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        auto text = DecodeUtf16Bytes(data.subspan(2), true);
        size_t printable = 0;
        size_t meaningful = 0;
        if (text && LooksPrintable(*text, printable, meaningful)) {
            return text;
        }
        return std::nullopt;
    }

    // No BOM: reject anything with embedded NULs (typical for binary data).
    if (std::any_of(data.begin(), data.end(), [](uint8_t value) { return value == 0; })) {
        return std::nullopt;
    }

    const std::wstring text = Utf8ToWide(std::string_view(
        reinterpret_cast<const char*>(data.data()), data.size()));
    size_t printable = 0;
    size_t meaningful = 0;
    if (!text.empty() && LooksPrintable(text, printable, meaningful)) {
        return text;
    }
    return std::nullopt;
}

std::wstring HumanSize(uint64_t bytes) {
    if (bytes < 1024) {
        return Utf8ToWide(fmt::format("{} B", bytes));
    }
    if (bytes < 1024ull * 1024ull) {
        return Utf8ToWide(fmt::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0));
    }
    if (bytes < 1024ull * 1024ull * 1024ull) {
        return Utf8ToWide(fmt::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0)));
    }
    return Utf8ToWide(fmt::format("{:.1f} GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)));
}

std::wstring ToLowerAscii(std::wstring_view text) {
    std::wstring result(text);
    for (auto& ch : result) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return result;
}

std::wstring Trim(std::wstring_view text) {
    size_t begin = 0;
    while (begin < text.size() && std::iswspace(text[begin]) != 0) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::iswspace(text[end - 1]) != 0) {
        --end;
    }
    return std::wstring(text.substr(begin, end - begin));
}

bool IEqualsAscii(std::wstring_view lhs, std::wstring_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        wchar_t a = lhs[i];
        wchar_t b = rhs[i];
        if (a >= L'A' && a <= L'Z') {
            a = static_cast<wchar_t>(a - L'A' + L'a');
        }
        if (b >= L'A' && b <= L'Z') {
            b = static_cast<wchar_t>(b - L'A' + L'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

Result<std::vector<uint8_t>> EncodeTextPreservingEncoding(
    std::span<const uint8_t> original, std::wstring_view text) {
    bool utf8Bom = false;
    bool utf16LeBom = false;
    bool utf16BeBom = false;
    bool utf16LeWithoutBom = false;

    if (original.size() >= 3 && original[0] == 0xEF && original[1] == 0xBB &&
        original[2] == 0xBF) {
        utf8Bom = true;
    } else if (original.size() >= 2 && original[0] == 0xFF && original[1] == 0xFE) {
        utf16LeBom = true;
    } else if (original.size() >= 2 && original[0] == 0xFE && original[1] == 0xFF) {
        utf16BeBom = true;
    } else if (original.size() >= 4 && original[1] == 0 && original[3] == 0) {
        utf16LeWithoutBom = true;
    }

    if (utf16LeBom || utf16LeWithoutBom) {
        std::vector<uint8_t> output;
        output.reserve(text.size() * 2 + 2);
        if (utf16LeBom) {
            output.push_back(0xFF);
            output.push_back(0xFE);
        }
        for (const wchar_t ch : text) {
            output.push_back(static_cast<uint8_t>(ch & 0xFF));
            output.push_back(static_cast<uint8_t>((ch >> 8) & 0xFF));
        }
        return output;
    }
    if (utf16BeBom) {
        std::vector<uint8_t> output;
        output.reserve(text.size() * 2 + 2);
        output.push_back(0xFE);
        output.push_back(0xFF);
        for (const wchar_t ch : text) {
            output.push_back(static_cast<uint8_t>((ch >> 8) & 0xFF));
            output.push_back(static_cast<uint8_t>(ch & 0xFF));
        }
        return output;
    }

    const std::string utf8 = WideToUtf8(text);
    if (!text.empty() && utf8.empty()) {
        return MakeError(L"无法把文本转换为 UTF-8");
    }
    std::vector<uint8_t> output;
    output.reserve(utf8.size() + 3);
    if (utf8Bom) {
        output.push_back(0xEF);
        output.push_back(0xBB);
        output.push_back(0xBF);
    }
    output.insert(output.end(), utf8.begin(), utf8.end());
    return output;
}

}  // namespace peres
