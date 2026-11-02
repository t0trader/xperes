#pragma once

#include "Result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace peres {

struct BitmapDibInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t bitCount = 0;
    uint32_t compression = 0;
    size_t bitsOffset = 0;
};

// Converts a complete .bmp file to the DIB payload stored in RT_BITMAP.
Result<std::vector<uint8_t>> BmpFileToDib(std::span<const uint8_t> bmpFile);
Result<std::vector<uint8_t>> DibToBmpFile(std::span<const uint8_t> dib);
Result<BitmapDibInfo> InspectBitmapDib(std::span<const uint8_t> dib);

}  // namespace peres
