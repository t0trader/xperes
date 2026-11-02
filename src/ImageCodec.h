#pragma once

#include "Result.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace peres {

enum class ImageFileFormat {
    Unknown,
    Bmp,
    Png,
    Jpeg,
    Gif,
    Tiff,
    Ico,
};

struct DecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    // Top-down BGRA with premultiplied alpha (WIC 32bppPBGRA), ready for
    // AlphaBlend with AC_SRC_ALPHA.
    std::vector<uint8_t> pixels;
    ImageFileFormat format = ImageFileFormat::Unknown;
};

ImageFileFormat DetectImageFormat(std::span<const uint8_t> bytes);
bool IsImageData(std::span<const uint8_t> bytes);
std::wstring ImageFormatName(ImageFileFormat format);
std::wstring ImageFormatExtension(ImageFileFormat format);

// Decodes PNG/JPEG/GIF/BMP/TIFF/ICO data with the Windows Imaging Component.
Result<DecodedImage> DecodeImage(std::span<const uint8_t> bytes);

// Converts an image file to a 32bpp RT_BITMAP DIB (straight alpha BGRA,
// bottom-up rows), suitable for replacing a bitmap resource.
Result<std::vector<uint8_t>> ImageToDib(std::span<const uint8_t> imageFile);

}  // namespace peres
