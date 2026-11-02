#pragma once

#include "Result.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace peres {

// Editable representation: one "#<string-id>: <escaped text>" line per entry.
Result<std::wstring> FormatStringTable(std::span<const uint8_t> data, uint16_t blockId);
Result<std::vector<uint8_t>> BuildStringTable(std::wstring_view text, uint16_t blockId);

}  // namespace peres
