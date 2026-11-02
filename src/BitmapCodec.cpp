#include "BitmapCodec.h"

#include "TextUtil.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace peres {

namespace {

constexpr uint16_t kBitmapFileType = 0x4D42;
constexpr uint32_t kBiRgb = 0;
constexpr uint32_t kBiBitfields = 3;
constexpr uint32_t kBiAlphaBitfields = 6;

uint16_t U16(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

uint32_t U32(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value & 0xFF);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    bytes[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    bytes[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

size_t PaletteAndMaskBytes(uint32_t headerSize, uint16_t bitCount, uint32_t compression,
                           uint32_t colorsUsed) {
    size_t maskBytes = 0;
    if (headerSize == 40) {
        if (compression == kBiBitfields) {
            maskBytes = 12;
        } else if (compression == kBiAlphaBitfields) {
            maskBytes = 16;
        }
    }
    size_t paletteBytes = 0;
    if (bitCount <= 8) {
        const uint32_t colors = colorsUsed != 0 ? colorsUsed : (1u << bitCount);
        paletteBytes = static_cast<size_t>(colors) * 4;
    }
    return maskBytes + paletteBytes;
}

}  // namespace

Result<BitmapDibInfo> InspectBitmapDib(std::span<const uint8_t> dib) {
    if (dib.size() < 40) {
        return MakeError(L"DIB 数据过短");
    }
    const uint32_t headerSize = U32(dib, 0);
    if (headerSize < 40 || headerSize > dib.size()) {
        return MakeError(L"DIB 头尺寸无效");
    }
    const int32_t signedWidth = static_cast<int32_t>(U32(dib, 4));
    const int32_t signedHeight = static_cast<int32_t>(U32(dib, 8));
    if (signedWidth <= 0 || signedHeight == 0) {
        return MakeError(L"DIB 尺寸无效");
    }
    const uint16_t bitCount = U16(dib, 14);
    const uint32_t compression = U32(dib, 16);
    if (bitCount != 1 && bitCount != 4 && bitCount != 8 && bitCount != 16 && bitCount != 24 &&
        bitCount != 32) {
        return MakeError(Utf8ToWide(fmt::format("不支持的位图位深：{}", bitCount)));
    }
    if (compression != kBiRgb && compression != kBiBitfields &&
        compression != kBiAlphaBitfields) {
        return MakeError(Utf8ToWide(fmt::format("不支持的位图压缩方式：{}", compression)));
    }

    const uint32_t colorsUsed = U32(dib, 32);
    const size_t bitsOffset = headerSize + PaletteAndMaskBytes(headerSize, bitCount, compression, colorsUsed);
    if (bitsOffset > dib.size()) {
        return MakeError(L"DIB 调色板/像素偏移超出数据范围");
    }

    return BitmapDibInfo{
        static_cast<uint32_t>(signedWidth),
        static_cast<uint32_t>(std::abs(signedHeight)),
        bitCount,
        compression,
        bitsOffset,
    };
}

Result<std::vector<uint8_t>> BmpFileToDib(std::span<const uint8_t> bmpFile) {
    if (bmpFile.size() < 14 + 40) {
        return MakeError(L"BMP 文件过短");
    }
    if (U16(bmpFile, 0) != kBitmapFileType) {
        return MakeError(L"不是有效的 BMP 文件（缺少 BM 标识）");
    }
    const uint32_t pixelOffset = U32(bmpFile, 10);
    if (pixelOffset >= bmpFile.size()) {
        return MakeError(L"BMP 像素数据偏移超出文件");
    }

    std::vector<uint8_t> dib(bmpFile.begin() + 14, bmpFile.end());
    auto info = InspectBitmapDib(dib);
    if (!info) {
        return std::unexpected(info.error());
    }
    if (14ull + info->bitsOffset != pixelOffset) {
        return MakeError(L"BMP 像素数据偏移与 DIB 头不一致");
    }
    return dib;
}

Result<std::vector<uint8_t>> DibToBmpFile(std::span<const uint8_t> dib) {
    auto info = InspectBitmapDib(dib);
    if (!info) {
        return std::unexpected(info.error());
    }
    const uint64_t fileSize = 14ull + dib.size();
    if (fileSize > 0xFFFFFFFFull) {
        return MakeError(L"位图文件大小超出 32 位范围");
    }
    std::vector<uint8_t> out(static_cast<size_t>(fileSize), 0);
    out[0] = 'B';
    out[1] = 'M';
    Put32(out, 2, static_cast<uint32_t>(fileSize));
    Put32(out, 6, 0);
    Put32(out, 10, static_cast<uint32_t>(14ull + info->bitsOffset));
    std::memcpy(out.data() + 14, dib.data(), dib.size());
    return out;
}

}  // namespace peres
