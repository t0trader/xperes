#include "ImageCodec.h"

#include "TextUtil.h"

#include <fmt/format.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace peres {

namespace {

constexpr uint32_t kMaxImageDimension = 16384;
constexpr uint64_t kMaxImagePixels = 64ull * 1024ull * 1024ull;

class ComScope {
public:
    ComScope() {
        result_ = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        uninit_ = SUCCEEDED(result_);
    }
    ~ComScope() {
        if (uninit_) {
            ::CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

    bool usable() const { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT result_ = E_FAIL;
    bool uninit_ = false;
};

Result<void> CheckHresult(HRESULT result, std::wstring_view what) {
    if (SUCCEEDED(result)) {
        return {};
    }
    return MakeError(Utf8ToWide(
        fmt::format("{}失败（HRESULT 0x{:08X}）", WideToUtf8(what), static_cast<uint32_t>(result))));
}

bool StartsWith(std::span<const uint8_t> bytes, std::initializer_list<uint8_t> prefix) {
    if (bytes.size() < prefix.size()) {
        return false;
    }
    size_t index = 0;
    for (const uint8_t value : prefix) {
        if (bytes[index++] != value) {
            return false;
        }
    }
    return true;
}

ImageFileFormat FormatFromContainer(const GUID& container) {
    if (container == GUID_ContainerFormatBmp) {
        return ImageFileFormat::Bmp;
    }
    if (container == GUID_ContainerFormatPng) {
        return ImageFileFormat::Png;
    }
    if (container == GUID_ContainerFormatJpeg) {
        return ImageFileFormat::Jpeg;
    }
    if (container == GUID_ContainerFormatGif) {
        return ImageFileFormat::Gif;
    }
    if (container == GUID_ContainerFormatTiff) {
        return ImageFileFormat::Tiff;
    }
    if (container == GUID_ContainerFormatIco) {
        return ImageFileFormat::Ico;
    }
    return ImageFileFormat::Unknown;
}

void Unpremultiply(std::vector<uint8_t>& pixels) {
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const uint32_t alpha = pixels[i + 3];
        if (alpha == 0) {
            pixels[i + 0] = 0;
            pixels[i + 1] = 0;
            pixels[i + 2] = 0;
            continue;
        }
        const auto straight = [alpha](uint8_t value) {
            const uint32_t scaled = (static_cast<uint32_t>(value) * 255u + alpha / 2u) / alpha;
            return static_cast<uint8_t>(std::min<uint32_t>(scaled, 255u));
        };
        pixels[i + 0] = straight(pixels[i + 0]);
        pixels[i + 1] = straight(pixels[i + 1]);
        pixels[i + 2] = straight(pixels[i + 2]);
    }
}

}  // namespace

ImageFileFormat DetectImageFormat(std::span<const uint8_t> bytes) {
    if (StartsWith(bytes, {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A})) {
        return ImageFileFormat::Png;
    }
    if (StartsWith(bytes, {0xFF, 0xD8, 0xFF})) {
        return ImageFileFormat::Jpeg;
    }
    if (StartsWith(bytes, {0x47, 0x49, 0x46, 0x38})) {
        return ImageFileFormat::Gif;
    }
    if (StartsWith(bytes, {0x42, 0x4D}) && bytes.size() >= 18) {
        return ImageFileFormat::Bmp;
    }
    if (bytes.size() >= 4 &&
        ((bytes[0] == 0x49 && bytes[1] == 0x49 && bytes[2] == 0x2A && bytes[3] == 0x00) ||
         (bytes[0] == 0x4D && bytes[1] == 0x4D && bytes[2] == 0x00 && bytes[3] == 0x2A))) {
        return ImageFileFormat::Tiff;
    }
    if (StartsWith(bytes, {0x00, 0x00, 0x01, 0x00}) && bytes.size() >= 22) {
        // ICO: reserved=0, type=1 and at least one directory entry.
        const uint16_t count = static_cast<uint16_t>(bytes[4] | (bytes[5] << 8));
        if (count != 0 && count <= 4096 && 6ull + static_cast<uint64_t>(count) * 16ull <= bytes.size()) {
            return ImageFileFormat::Ico;
        }
    }
    return ImageFileFormat::Unknown;
}

bool IsImageData(std::span<const uint8_t> bytes) {
    return DetectImageFormat(bytes) != ImageFileFormat::Unknown;
}

std::wstring ImageFormatName(ImageFileFormat format) {
    switch (format) {
        case ImageFileFormat::Bmp:
            return L"BMP";
        case ImageFileFormat::Png:
            return L"PNG";
        case ImageFileFormat::Jpeg:
            return L"JPEG";
        case ImageFileFormat::Gif:
            return L"GIF";
        case ImageFileFormat::Tiff:
            return L"TIFF";
        case ImageFileFormat::Ico:
            return L"ICO";
        default:
            return L"未知";
    }
}

std::wstring ImageFormatExtension(ImageFileFormat format) {
    switch (format) {
        case ImageFileFormat::Bmp:
            return L"bmp";
        case ImageFileFormat::Png:
            return L"png";
        case ImageFileFormat::Jpeg:
            return L"jpg";
        case ImageFileFormat::Gif:
            return L"gif";
        case ImageFileFormat::Tiff:
            return L"tif";
        case ImageFileFormat::Ico:
            return L"ico";
        default:
            return L"bin";
    }
}

Result<DecodedImage> DecodeImage(std::span<const uint8_t> bytes) {
    if (bytes.empty()) {
        return MakeError(L"图像数据为空");
    }
    ComScope com;
    if (!com.usable()) {
        return MakeError(L"无法初始化 COM，WIC 图像解码不可用");
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return MakeError(Utf8ToWide(fmt::format("创建 WIC 工厂失败（0x{:08X}）",
                                                static_cast<uint32_t>(hr))));
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    auto check = CheckHresult(hr, L"创建 WIC 流");
    if (!check) {
        return std::unexpected(check.error());
    }
    hr = stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                      static_cast<DWORD>(bytes.size()));
    check = CheckHresult(hr, L"初始化 WIC 流");
    if (!check) {
        return std::unexpected(check.error());
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                          &decoder);
    if (FAILED(hr)) {
        return MakeError(Utf8ToWide(fmt::format("WIC 无法识别该图像（0x{:08X}）",
                                                static_cast<uint32_t>(hr))));
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    check = CheckHresult(hr, L"读取图像帧");
    if (!check) {
        return std::unexpected(check.error());
    }

    uint32_t width = 0;
    uint32_t height = 0;
    hr = frame->GetSize(&width, &height);
    check = CheckHresult(hr, L"读取图像尺寸");
    if (!check) {
        return std::unexpected(check.error());
    }
    if (width == 0 || height == 0 || width > kMaxImageDimension || height > kMaxImageDimension ||
        static_cast<uint64_t>(width) * height > kMaxImagePixels) {
        return MakeError(Utf8ToWide(
            fmt::format("图像尺寸过大：{} × {}", width, height)));
    }

    GUID container{};
    decoder->GetContainerFormat(&container);

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    check = CheckHresult(hr, L"创建 WIC 格式转换器");
    if (!check) {
        return std::unexpected(check.error());
    }
    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                               nullptr, 0.0, WICBitmapPaletteTypeCustom);
    check = CheckHresult(hr, L"转换图像像素格式");
    if (!check) {
        return std::unexpected(check.error());
    }

    DecodedImage image;
    image.width = width;
    image.height = height;
    image.format = FormatFromContainer(container);
    image.pixels.resize(static_cast<size_t>(width) * height * 4ull);
    const uint32_t stride = width * 4;
    hr = converter->CopyPixels(nullptr, stride, static_cast<UINT32>(image.pixels.size()),
                               image.pixels.data());
    check = CheckHresult(hr, L"复制图像像素");
    if (!check) {
        return std::unexpected(check.error());
    }
    return image;
}

Result<std::vector<uint8_t>> ImageToDib(std::span<const uint8_t> imageFile) {
    auto decoded = DecodeImage(imageFile);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    std::vector<uint8_t> pixels = decoded->pixels;
    Unpremultiply(pixels);

    const uint32_t width = decoded->width;
    const uint32_t height = decoded->height;
    const uint32_t imageBytes = width * height * 4;
    std::vector<uint8_t> dib(40ull + imageBytes, 0);
    const auto put32 = [&dib](size_t offset, uint32_t value) {
        dib[offset + 0] = static_cast<uint8_t>(value & 0xFF);
        dib[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        dib[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
        dib[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
    };
    put32(0, 40);  // biSize
    put32(4, width);
    put32(8, height);  // positive height => bottom-up rows
    dib[12] = 1;      // biPlanes
    dib[13] = 0;
    dib[14] = 32;     // biBitCount
    dib[15] = 0;
    put32(16, 0);     // BI_RGB
    put32(20, imageBytes);

    // Flip rows to bottom-up.
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* source = pixels.data() + static_cast<size_t>(y) * width * 4;
        uint8_t* target = dib.data() + 40 + static_cast<size_t>(height - 1 - y) * width * 4;
        std::memcpy(target, source, static_cast<size_t>(width) * 4);
    }
    return dib;
}

}  // namespace peres
