#pragma once

#include "Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace peres {

std::wstring Utf8ToWide(std::string_view text);
std::string WideToUtf8(std::wstring_view text);

// Returns decoded text when the blob looks like UTF-8/UTF-16 text; returns
// nullopt for binary data or data larger than the printable sanity limits.
std::optional<std::wstring> TryDecodeText(std::span<const uint8_t> data, size_t maxBytes);

std::wstring HumanSize(uint64_t bytes);
std::wstring ToLowerAscii(std::wstring_view text);
std::wstring Trim(std::wstring_view text);
bool IEqualsAscii(std::wstring_view lhs, std::wstring_view rhs);

// Encodes editor text using the same encoding/BOM convention as the original
// resource data (UTF-8, UTF-8 BOM, UTF-16LE/BE BOM or BOM-less UTF-16LE).
Result<std::vector<uint8_t>> EncodeTextPreservingEncoding(
    std::span<const uint8_t> original, std::wstring_view text);

}  // namespace peres
