#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "BitmapCodec.h"
#include "Config.h"
#include "DialogCodec.h"
#include "IconCodec.h"
#include "ImageCodec.h"
#include "MenuCodec.h"
#include "PeImage.h"
#include "ProcessRunner.h"
#include "ResourceModel.h"
#include "ResourceTypes.h"
#include "StringTableCodec.h"
#include "TextUtil.h"
#include "UpXTool.h"
#include "VersionCodec.h"
#include "XmlFormat.h"

#include <winver.h>

#include <fmt/format.h>

#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace peres;

namespace {

int g_failures = 0;
int g_checks = 0;

void Fail(const std::string& message, int line) {
    ++g_failures;
    std::cout << "FAIL(line " << line << "): " << message << "\n";
}

void Check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (!condition) {
        Fail(expression, line);
    }
}

void FailResult(const Error& error, const char* what, int line) {
    ++g_failures;
    std::cout << "FAIL(line " << line << "): " << what << ": " << WideToUtf8(error) << "\n";
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

#define CHECK_MSG(expr, message) \
    do { \
        ++g_checks; \
        if (!(expr)) Fail((message), __LINE__); \
    } while (false)

template <typename T>
std::optional<T> Expect(Result<T>&& result, const char* what, int line) {
    ++g_checks;
    if (!result) {
        FailResult(result.error(), what, line);
        return std::nullopt;
    }
    return std::move(*result);
}

bool ExpectVoid(Result<void>&& result, const char* what, int line) {
    ++g_checks;
    if (!result) {
        FailResult(result.error(), what, line);
        return false;
    }
    return true;
}

#define EXPECT_VALUE(result, what) Expect((result), what, __LINE__)
#define EXPECT_OK(result, what) ExpectVoid((result), what, __LINE__)

void Put16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value & 0xFF);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value & 0xFF);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    bytes[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    bytes[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

std::vector<uint8_t> MakeMinimalPe32(const char* sectionName = ".text") {
    std::vector<uint8_t> bytes(0x400, 0);
    const uint32_t pe = 0x80;
    const uint32_t opt = 0x98;
    const uint32_t sectionTable = 0x178;

    Put16(bytes, 0x00, 0x5A4D);
    Put32(bytes, 0x3C, pe);
    Put32(bytes, pe, 0x00004550);
    Put16(bytes, pe + 4, 0x014C);          // IMAGE_FILE_MACHINE_I386
    Put16(bytes, pe + 6, 1);               // NumberOfSections
    Put16(bytes, pe + 20, 0xE0);           // SizeOfOptionalHeader
    Put16(bytes, pe + 22, 0x0102);          // EXECUTABLE_IMAGE | 32BIT_MACHINE

    Put16(bytes, opt + 0, 0x010B);
    Put32(bytes, opt + 4, 0x200);          // SizeOfCode
    Put32(bytes, opt + 8, 0x200);          // SizeOfInitializedData
    Put32(bytes, opt + 16, 0x1000);        // AddressOfEntryPoint
    Put32(bytes, opt + 20, 0x1000);        // BaseOfCode
    Put32(bytes, opt + 24, 0x2000);        // BaseOfData
    Put32(bytes, opt + 28, 0x400000);      // ImageBase
    Put32(bytes, opt + 32, 0x1000);        // SectionAlignment
    Put32(bytes, opt + 36, 0x200);         // FileAlignment
    Put16(bytes, opt + 40, 6);             // MajorOperatingSystemVersion
    Put16(bytes, opt + 48, 6);             // MajorSubsystemVersion
    Put32(bytes, opt + 56, 0x2000);        // SizeOfImage
    Put32(bytes, opt + 60, 0x200);         // SizeOfHeaders
    Put32(bytes, opt + 64, 0);             // CheckSum
    Put16(bytes, opt + 68, 2);             // Subsystem: GUI
    Put32(bytes, opt + 72, 0x100000);      // SizeOfStackReserve
    Put32(bytes, opt + 76, 0x1000);        // SizeOfStackCommit
    Put32(bytes, opt + 80, 0x100000);      // SizeOfHeapReserve
    Put32(bytes, opt + 84, 0x1000);        // SizeOfHeapCommit
    Put32(bytes, opt + 92, 16);            // NumberOfRvaAndSizes

    std::memcpy(bytes.data() + sectionTable, sectionName, 8);
    Put32(bytes, sectionTable + 8, 0x10);   // VirtualSize
    Put32(bytes, sectionTable + 12, 0x1000);  // VirtualAddress
    Put32(bytes, sectionTable + 16, 0x200);   // SizeOfRawData
    Put32(bytes, sectionTable + 20, 0x200);   // PointerToRawData
    Put32(bytes, sectionTable + 36, 0x60000020);  // CODE | EXECUTE | READ
    bytes[0x200] = 0xC3;  // ret
    return bytes;
}

const ResourceEntry* FindEntry(const PeImage& pe, uint16_t type, uint16_t name,
                               uint16_t language) {
    for (const auto& entry : pe.Resources()) {
        if (!entry.type.named && entry.type.id == type && !entry.name.named &&
            entry.name.id == name && entry.language == language) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamoff size = static_cast<std::streamoff>(file.tellg());
    if (size <= 0) {
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        return {};
    }
    return bytes;
}

bool WriteFile(const fs::path& path, std::span<const uint8_t> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    if (!bytes.empty()) {
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(file);
}

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                (L"peres_tests_" + std::to_wstring(::GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void TestMinimalPe32() {
    std::cout << "[test] minimal PE32 resource rewrite\n";
    std::vector<uint8_t> peBytes = MakeMinimalPe32();
    auto image = EXPECT_VALUE(PeImage::LoadFromBytes(peBytes), "load minimal PE32");
    if (!image) {
        return;
    }
    CHECK(!image->Info().is64);
    CHECK(image->Info().machine == 0x014C);
    CHECK(image->Info().sectionCount == 1);
    CHECK(image->Resources().empty());

    const std::vector<uint8_t> manifest = {
        '<', 'm', 'a', 'n', 'i', 'f', 'e', 's', 't', '/', '>',
    };
    const std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 0xFE, 0xFF};

    ResourceModel model;
    model.SetData(ResourceId::FromId(24), ResourceId::FromId(1), 1033, 0, manifest);
    model.SetData(ResourceId::FromName(L"CUSTOM"), ResourceId::FromName(L"DATA"), 0, 1200, payload);

    auto output = EXPECT_VALUE(image->CompileResources(model), "compile 32-bit resources");
    if (!output) {
        return;
    }
    auto rewritten = EXPECT_VALUE(PeImage::LoadFromBytes(*output), "reparse compiled PE32");
    if (!rewritten) {
        return;
    }
    CHECK(rewritten->Info().sectionCount == 2);
    CHECK(!rewritten->Info().is64);
    CHECK(rewritten->Resources().size() == 2);
    CHECK(rewritten->Resources().front().type.named);  // named type sorts first

    const ResourceEntry* manifestEntry = FindEntry(*rewritten, 24, 1, 1033);
    CHECK(manifestEntry != nullptr);
    if (manifestEntry != nullptr) {
        CHECK(manifestEntry->data == manifest);
    }
    const ResourceEntry* customEntry = nullptr;
    for (const auto& entry : rewritten->Resources()) {
        if (entry.type.named && entry.type.name == L"CUSTOM" && entry.name.named &&
            entry.name.name == L"DATA") {
            customEntry = &entry;
            break;
        }
    }
    CHECK(customEntry != nullptr);
    if (customEntry != nullptr) {
        CHECK(customEntry->data == payload);
    }

    // The first save created an .rsrc section.  A small edit must reuse it.
    ResourceModel model2(rewritten->Resources());
    model2.SetData(ResourceId::FromId(24), ResourceId::FromId(1), 1033, 0,
                   std::vector<uint8_t>{'x'});
    auto output2 = EXPECT_VALUE(rewritten->CompileResources(model2), "in-place resource update");
    if (!output2) {
        return;
    }
    auto rewritten2 = EXPECT_VALUE(PeImage::LoadFromBytes(*output2), "reparse in-place update");
    if (!rewritten2) {
        return;
    }
    CHECK(rewritten2->Info().sectionCount == 2);

    // A large edit cannot fit in the existing raw section and must append one.
    ResourceModel model3(rewritten2->Resources());
    std::vector<uint8_t> large(16 * 1024, 0x5A);
    model3.SetData(ResourceId::FromId(24), ResourceId::FromId(1), 1033, 0, large);
    auto output3 = EXPECT_VALUE(rewritten2->CompileResources(model3), "append resource section");
    if (!output3) {
        return;
    }
    auto rewritten3 = EXPECT_VALUE(PeImage::LoadFromBytes(*output3), "reparse appended section");
    if (!rewritten3) {
        return;
    }
    CHECK(rewritten3->Info().sectionCount == 3);
    const ResourceEntry* largeEntry = FindEntry(*rewritten3, 24, 1, 1033);
    CHECK(largeEntry != nullptr && largeEntry->data.size() == large.size());

    // Emptying the model must still produce a valid resource directory.
    ResourceModel empty;
    auto output4 = EXPECT_VALUE(rewritten3->CompileResources(empty), "empty resource model");
    if (!output4) {
        return;
    }
    auto rewritten4 = EXPECT_VALUE(PeImage::LoadFromBytes(*output4), "reparse empty resources");
    if (!rewritten4) {
        return;
    }
    CHECK(rewritten4->Resources().empty());
}

void TestSampleDll() {
    std::cout << "[test] 64-bit DLL resource rewrite + Windows load\n";
    const fs::path samplePath(L"" PERES_TEST_SAMPLE_DLL);
    auto image = EXPECT_VALUE(PeImage::LoadFromFile(samplePath), "load sample DLL");
    if (!image) {
        return;
    }
    CHECK(image->Info().is64);
    CHECK(image->Info().machine == 0x8664);
    CHECK(image->Info().isDll);
    CHECK(!image->Resources().empty());

    const ResourceEntry* group = FindEntry(*image, 14, 1, 1033);
    const ResourceEntry* manifest = FindEntry(*image, 24, 2, 1033);
    const ResourceEntry* rcdata = FindEntry(*image, 10, 201, 1033);
    CHECK(group != nullptr);
    CHECK(manifest != nullptr);
    CHECK(rcdata != nullptr);
    if (group == nullptr || manifest == nullptr || rcdata == nullptr) {
        return;
    }
    CHECK(!rcdata->data.empty());

    auto findNamed = [&](std::wstring_view typeName, std::wstring_view name) {
        for (const auto& entry : image->Resources()) {
            if (entry.type.named && entry.type.name == typeName && entry.name.named &&
                entry.name.name == name) {
                return &entry;
            }
        }
        return static_cast<const ResourceEntry*>(nullptr);
    };
    const ResourceEntry* pngEntry = findNamed(L"PNG", L"PNGDATA");
    const ResourceEntry* jpegEntry = findNamed(L"JPEG", L"JPEGDATA");
    CHECK(pngEntry != nullptr);
    CHECK(jpegEntry != nullptr);
    if (pngEntry != nullptr) {
        CHECK(DetectImageFormat(pngEntry->data) == ImageFileFormat::Png);
    }
    if (jpegEntry != nullptr) {
        CHECK(DetectImageFormat(jpegEntry->data) == ImageFileFormat::Jpeg);
    }

    auto groupEntries = EXPECT_VALUE(ParseGroupIcon(group->data), "parse sample group icon");
    if (!groupEntries) {
        return;
    }
    CHECK(!groupEntries->empty());

    const fs::path icoPath = fs::path(L"" PERES_TEST_ASSET_DIR) / "app.ico";
    auto icoBytes = ReadFile(icoPath);
    CHECK(!icoBytes.empty());
    auto icoImages = EXPECT_VALUE(ParseIco(icoBytes), "parse app.ico");
    if (!icoImages) {
        return;
    }
    CHECK(icoImages->size() == groupEntries->size());

    ResourceModel imageModel(image->Resources());
    auto beforeIco = EXPECT_VALUE(BuildIcoFromGroup(imageModel, *group), "build ICO from group");
    if (!beforeIco) {
        return;
    }
    auto beforeIcoParsed = EXPECT_VALUE(ParseIco(*beforeIco), "reparse built ICO");
    CHECK(beforeIcoParsed && beforeIcoParsed->size() == groupEntries->size());

    ResourceModel model(image->Resources());
    CHECK(EXPECT_OK(ReplaceIconGroup(model, *group, icoBytes), "replace icon group"));

    const std::vector<uint8_t> newManifest = {
        '<', 'm', 'a', 'n', 'i', 'f', 'e', 's', 't', '>', 'n', 'e', 'w', '<', '/', 'm', 'a',
        'n', 'i', 'f', 'e', 's', 't', '>',
    };
    model.SetData(ResourceId::FromId(24), ResourceId::FromId(2), 1033, 0, newManifest);

    auto output = EXPECT_VALUE(image->CompileResources(model), "compile sample DLL");
    if (!output) {
        return;
    }
    auto rewritten = EXPECT_VALUE(PeImage::LoadFromBytes(*output), "reparse sample DLL");
    if (!rewritten) {
        return;
    }
    const ResourceEntry* newGroup = FindEntry(*rewritten, 14, 1, 1033);
    CHECK(newGroup != nullptr);

    TempDir temp;
    const fs::path outputPath = temp.path() / L"peres_sample_rewritten.dll";
    CHECK(WriteFile(outputPath, *output));

    HMODULE module = ::LoadLibraryExW(outputPath.c_str(), nullptr,
                                      LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    CHECK(module != nullptr);
    if (module != nullptr) {
        HRSRC manifestResource =
            ::FindResourceExW(module, MAKEINTRESOURCEW(24), MAKEINTRESOURCEW(2), 1033);
        CHECK(manifestResource != nullptr);
        if (manifestResource != nullptr) {
            const DWORD manifestSize = ::SizeofResource(module, manifestResource);
            HGLOBAL global = ::LoadResource(module, manifestResource);
            const void* data = ::LockResource(global);
            CHECK(data != nullptr);
            CHECK(manifestSize == newManifest.size());
            if (data != nullptr && manifestSize == newManifest.size()) {
                CHECK(std::memcmp(data, newManifest.data(), newManifest.size()) == 0);
            }
        }

        HRSRC groupResource =
            ::FindResourceExW(module, MAKEINTRESOURCEW(14), MAKEINTRESOURCEW(1), 1033);
        CHECK(groupResource != nullptr);
        if (groupResource != nullptr) {
            const DWORD groupSize = ::SizeofResource(module, groupResource);
            HGLOBAL global = ::LoadResource(module, groupResource);
            const uint8_t* data = static_cast<const uint8_t*>(::LockResource(global));
            CHECK(data != nullptr);
            if (data != nullptr) {
                auto parsed = EXPECT_VALUE(
                    ParseGroupIcon(std::span<const uint8_t>(data, groupSize)),
                    "parse group from loaded DLL");
                CHECK(parsed && parsed->size() == icoImages->size());
            }
        }

        ::FreeLibrary(module);
    }

    if (newGroup != nullptr) {
        ResourceModel rewrittenModel(rewritten->Resources());
        auto afterIco = EXPECT_VALUE(BuildIcoFromGroup(rewrittenModel, *newGroup),
                                     "build ICO from rewritten group");
        CHECK(afterIco.has_value());
    }
}

void TestCodecs() {
    std::cout << "[test] icon and bitmap codecs\n";
    const fs::path icoPath = fs::path(L"" PERES_TEST_ASSET_DIR) / "app.ico";
    auto bytes = ReadFile(icoPath);
    CHECK(!bytes.empty());
    auto images = EXPECT_VALUE(ParseIco(bytes), "parse app.ico");
    if (images) {
        CHECK(images->size() == 3);
        std::vector<uint32_t> iconSizes;
        for (const auto& image : *images) {
            iconSizes.push_back(image.width);
        }
        std::sort(iconSizes.begin(), iconSizes.end());
        CHECK(iconSizes.size() == 3 && iconSizes[0] == 16 && iconSizes[1] == 32 &&
              iconSizes[2] == 64);
        auto rebuilt = EXPECT_VALUE(BuildIco(*images), "rebuild app.ico");
        if (rebuilt) {
            auto reparsed = EXPECT_VALUE(ParseIco(*rebuilt), "reparse rebuilt app.ico");
            CHECK(reparsed && reparsed->size() == images->size());
            if (reparsed) {
                for (size_t i = 0; i < images->size(); ++i) {
                    CHECK((*images)[i].data.size() == (*reparsed)[i].data.size());
                    CHECK((*images)[i].width == (*reparsed)[i].width);
                    CHECK((*images)[i].bitCount == (*reparsed)[i].bitCount);
                }
            }
        }
        auto info = EXPECT_VALUE(InspectIconData((*images)[0].data), "inspect icon image");
        if (info) {
            CHECK(!info->png);
            CHECK(info->width == 16 && info->height == 16);
            CHECK(info->bitCount == 32);
        }
        auto solo = EXPECT_VALUE(BuildIcoFromResource((*images)[0].data), "wrap icon resource");
        if (solo) {
            auto soloParsed = EXPECT_VALUE(ParseIco(*solo), "reparse single icon");
            CHECK(soloParsed && soloParsed->size() == 1);
        }
    }

    std::vector<uint8_t> bmp(14 + 40 + 24, 0);
    bmp[0] = 'B';
    bmp[1] = 'M';
    Put32(bmp, 2, static_cast<uint32_t>(bmp.size()));
    Put32(bmp, 10, 54);
    Put32(bmp, 14 + 0, 40);
    Put32(bmp, 14 + 4, 3);
    Put32(bmp, 14 + 8, 2);
    Put16(bmp, 14 + 12, 1);
    Put16(bmp, 14 + 14, 24);
    Put32(bmp, 14 + 16, 0);
    Put32(bmp, 14 + 20, 24);
    for (size_t i = 0; i < 24; ++i) {
        bmp[54 + i] = static_cast<uint8_t>(i * 7);
    }

    auto dib = EXPECT_VALUE(BmpFileToDib(bmp), "BMP -> DIB");
    if (dib) {
        CHECK(dib->size() == bmp.size() - 14);
        auto info = EXPECT_VALUE(InspectBitmapDib(*dib), "inspect DIB");
        if (info) {
            CHECK(info->width == 3);
            CHECK(info->height == 2);
            CHECK(info->bitCount == 24);
            CHECK(info->bitsOffset == 40);
        }
        auto roundtrip = EXPECT_VALUE(DibToBmpFile(*dib), "DIB -> BMP");
        CHECK(roundtrip && *roundtrip == bmp);
    }
}

void TestImageCodec() {
    std::cout << "[test] WIC PNG/JPEG image decoding\n";
    const fs::path dir(L"" PERES_TEST_IMAGE_DIR);
    for (const wchar_t* name : {L"sample.png", L"sample.jpg"}) {
        const fs::path path = dir / name;
        auto bytes = ReadFile(path);
        CHECK(!bytes.empty());
        const ImageFileFormat format = DetectImageFormat(bytes);
        const bool isPng = std::wcscmp(name, L"sample.png") == 0;
        CHECK(format == (isPng ? ImageFileFormat::Png : ImageFileFormat::Jpeg));
        auto decoded = EXPECT_VALUE(DecodeImage(bytes), "decode test image");
        if (!decoded) {
            continue;
        }
        CHECK(decoded->width == 96);
        CHECK(decoded->height == 64);
        CHECK(decoded->pixels.size() == 96ull * 64ull * 4ull);
        CHECK(decoded->format == format);
        auto dib = EXPECT_VALUE(ImageToDib(bytes), "convert test image to DIB");
        if (dib) {
            auto info = EXPECT_VALUE(InspectBitmapDib(*dib), "inspect converted DIB");
            if (info) {
                CHECK(info->width == 96);
                CHECK(info->height == 64);
                CHECK(info->bitCount == 32);
                CHECK(info->bitsOffset == 40);
            }
        }
    }
}

void TestStringTableAndMenu(const fs::path& samplePath) {
    std::cout << "[test] string table and menu codecs\n";
    auto image = EXPECT_VALUE(PeImage::LoadFromFile(samplePath), "load sample for menu test");
    if (!image) {
        return;
    }
    const ResourceEntry* strings = FindEntry(*image, 6, 7, 1033);
    CHECK(strings != nullptr);
    if (strings != nullptr) {
        auto text = EXPECT_VALUE(FormatStringTable(strings->data, 7), "format string table");
        if (text) {
            CHECK(text->find(L"#101: peres sample string") != std::wstring::npos);
            auto bytes = EXPECT_VALUE(BuildStringTable(*text, 7), "rebuild string table");
            if (bytes) {
                CHECK(bytes->size() == strings->data.size());
                auto roundtrip = EXPECT_VALUE(FormatStringTable(*bytes, 7), "reparse string table");
                CHECK(roundtrip && roundtrip->find(L"#101: peres sample string") != std::wstring::npos);
            }
            auto edited = text->find(L"#101: peres sample string");
            if (edited != std::wstring::npos) {
                std::wstring changed = *text;
                changed.replace(edited, std::wstring(L"#101: peres sample string").size(),
                                L"#101: 修改后的字符串");
                CHECK(changed.find(L"修改后的字符串") != std::wstring::npos);
                auto editedBytes = EXPECT_VALUE(BuildStringTable(changed, 7), "rebuild edited string table");
                CHECK(editedBytes ? editedBytes->size() > 0 : false);
            }
        }
    }

    const ResourceEntry* classicEntry = FindEntry(*image, 4, 301, 1033);
    const ResourceEntry* extendedEntry = FindEntry(*image, 4, 303, 1033);
    CHECK(classicEntry != nullptr);
    CHECK(extendedEntry != nullptr);
    CHECK(!classicEntry->data.empty());
    CHECK(!extendedEntry->data.empty());

    for (const ResourceEntry* entry : {classicEntry, extendedEntry}) {
        if (entry == nullptr) {
            continue;
        }
        auto menu = EXPECT_VALUE(ParseMenuResource(entry->data), "parse menu resource");
        if (!menu) {
            continue;
        }
        const std::wstring text = FormatMenuText(*menu);
        if (menu->format == MenuTemplateFormat::Classic) {
            CHECK(text.find(L"MENUITEM \"Open\", 40001") != std::wstring::npos);
            CHECK(text.find(L"MENUITEM SEPARATOR") != std::wstring::npos);
        } else {
            CHECK(text.find(L"POPUP \"FileEx\"") != std::wstring::npos);
        }
        auto reparsedText = EXPECT_VALUE(ParseMenuText(text), "parse formatted menu text");
        if (!reparsedText) {
            continue;
        }
        auto rebuilt = EXPECT_VALUE(BuildMenuResource(*reparsedText), "build menu resource");
        if (!rebuilt) {
            continue;
        }
        HMENU handle = ::LoadMenuIndirectW(rebuilt->data());
        CHECK(handle != nullptr);
        if (handle != nullptr) {
            const int count = ::GetMenuItemCount(handle);
            CHECK(count == static_cast<int>(menu->items.size()));
            wchar_t buffer[128] = {};
            if (count > 0) {
                CHECK(::GetMenuStringW(handle, 0, buffer, 128, MF_BYPOSITION) > 0);
                CHECK(std::wstring(buffer) == (menu->format == MenuTemplateFormat::Classic
                                                   ? L"File"
                                                   : L"FileEx"));
            }
            ::DestroyMenu(handle);
        }

        // Edit the first label and verify the compiled menu can still be loaded.
        std::wstring editedText = text;
        const size_t label = editedText.find(menu->format == MenuTemplateFormat::Classic ? L"\"Open\""
                                                                                         : L"\"OpenEx\"");
        if (label != std::wstring::npos) {
            const size_t labelLength =
                menu->format == MenuTemplateFormat::Classic ? 6 : 8;
            editedText.replace(label, labelLength, L"\"Edited\"");
        }
        auto editedMenu = EXPECT_VALUE(ParseMenuText(editedText), "parse edited menu text");
        if (!editedMenu) {
            continue;
        }
        auto editedBytes = EXPECT_VALUE(BuildMenuResource(*editedMenu), "build edited menu");
        if (!editedBytes) {
            continue;
        }
        HMENU editedHandle = ::LoadMenuIndirectW(editedBytes->data());
        CHECK(editedHandle != nullptr);
        if (editedHandle != nullptr) {
            ::DestroyMenu(editedHandle);
        }
    }
}

INT_PTR CALLBACK TestDialogProc(HWND, UINT, WPARAM, LPARAM) {
    return FALSE;
}

BOOL CALLBACK CountChildProc(HWND, LPARAM parameter) {
    ++*reinterpret_cast<int*>(parameter);
    return TRUE;
}

void TestDialogCodec(const fs::path& samplePath) {
    std::cout << "[test] dialog codec\n";
    auto image = EXPECT_VALUE(PeImage::LoadFromFile(samplePath), "load sample for dialog test");
    if (!image) {
        return;
    }
    for (uint16_t name : {static_cast<uint16_t>(302), static_cast<uint16_t>(304),
                          static_cast<uint16_t>(305), static_cast<uint16_t>(306)}) {
        const ResourceEntry* entry = FindEntry(*image, 5, name, 1033);
        CHECK(entry != nullptr);
        if (entry == nullptr) {
            continue;
        }
        auto dialog = EXPECT_VALUE(ParseDialogResource(entry->data), "parse dialog resource");
        if (!dialog) {
            continue;
        }
        const std::wstring text = FormatDialogText(*dialog);
        CHECK(text.find(L"CONTROL ") != std::wstring::npos);
        if (name == 304 || name == 305) {
            CHECK(dialog->extended);
        } else {
            CHECK(!dialog->extended);
        }
        auto reparsed = EXPECT_VALUE(ParseDialogText(text), "parse formatted dialog text");
        if (!reparsed) {
            continue;
        }
        auto rebuilt = EXPECT_VALUE(BuildDialogResource(*reparsed), "build dialog resource");
        if (!rebuilt) {
            continue;
        }
        HWND window = ::CreateDialogIndirectParamW(
            ::GetModuleHandleW(nullptr), reinterpret_cast<DLGTEMPLATE*>(rebuilt->data()), nullptr,
            TestDialogProc, 0);
        CHECK(window != nullptr);
        if (window != nullptr) {
            int childCount = 0;
            ::EnumChildWindows(window, CountChildProc, reinterpret_cast<LPARAM>(&childCount));
            CHECK(childCount == static_cast<int>(dialog->controls.size()));
            for (const auto& control : dialog->controls) {
                if (!control.title.ordinal && !control.title.text.empty()) {
                    HWND child = ::GetDlgItem(window, static_cast<int>(control.id));
                    CHECK(child != nullptr);
                    if (child != nullptr) {
                        wchar_t buffer[128] = {};
                        ::GetWindowTextW(child, buffer, 128);
                        CHECK(std::wstring(buffer) == control.title.text);
                    }
                }
            }
            ::DestroyWindow(window);
        }
    }
}

void TestTextEncoding() {
    std::cout << "[test] text encoding preservation\n";
    const std::wstring text = L"<?xml version=\"1.0\"?><a>中文 &amp; test</a>";

    const std::vector<uint8_t> utf8Original = {'<', 'a', '/', '>'};
    auto utf8 = EXPECT_VALUE(EncodeTextPreservingEncoding(utf8Original, text), "encode UTF-8");
    if (utf8) {
        const std::string expected = WideToUtf8(text);
        const std::string actual(reinterpret_cast<const char*>(utf8->data()), utf8->size());
        CHECK(actual == expected);
    }

    const std::vector<uint8_t> utf8BomOriginal = {0xEF, 0xBB, 0xBF, '<'};
    auto utf8Bom = EXPECT_VALUE(EncodeTextPreservingEncoding(utf8BomOriginal, L"abc"),
                                "encode UTF-8 BOM");
    CHECK(utf8Bom && utf8Bom->size() == 6 && (*utf8Bom)[0] == 0xEF && (*utf8Bom)[3] == 'a');

    const std::vector<uint8_t> utf16LeOriginal = {0xFF, 0xFE, 0x41, 0x00};
    auto utf16Le = EXPECT_VALUE(EncodeTextPreservingEncoding(utf16LeOriginal, L"AB"),
                                "encode UTF-16LE");
    CHECK(utf16Le && utf16Le->size() == 6 && (*utf16Le)[0] == 0xFF && (*utf16Le)[1] == 0xFE &&
          (*utf16Le)[2] == 'A' && (*utf16Le)[4] == 'B');

    const std::vector<uint8_t> utf16BeOriginal = {0xFE, 0xFF, 0x00, 0x41};
    auto utf16Be = EXPECT_VALUE(EncodeTextPreservingEncoding(utf16BeOriginal, L"AB"),
                                "encode UTF-16BE");
    CHECK(utf16Be && utf16Be->size() == 6 && (*utf16Be)[0] == 0xFE && (*utf16Be)[1] == 0xFF &&
          (*utf16Be)[2] == 0x00 && (*utf16Be)[3] == 'A');
}

void TestUpxRoundTrip(const fs::path& sourcePath) {
    std::cout << "[test] optional UPX unpack integration\n";
    auto config = LoadConfig();
    if (!config) {
        std::cout << "  [skip] " << WideToUtf8(config.error()) << "\n";
        return;
    }
    if (!fs::exists(config->upxPath)) {
        std::cout << "  [skip] UPX not found at " << config->upxPath.string() << "\n";
        return;
    }

    TempDir temp;
    const fs::path input = temp.path() / L"upx_input.exe";
    const fs::path packed = temp.path() / L"upx_packed.exe";
    const fs::path unpacked = temp.path() / L"upx_unpacked.exe";
    std::error_code ec;
    fs::copy_file(sourcePath, input, fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    if (ec) {
        return;
    }

    const std::wstring compressCommand =
        QuoteWindowsArgument(config->upxPath.wstring()) + L" -1 -o " +
        QuoteWindowsArgument(packed.wstring()) + L" " +
        QuoteWindowsArgument(input.wstring());
    auto compressed = EXPECT_VALUE(RunProcessCapture(compressCommand), "run UPX compression");
    if (!compressed) {
        return;
    }
    CHECK(compressed->exitCode == 0);
    if (compressed->exitCode != 0) {
        std::cout << "  upx output: " << compressed->output << "\n";
        return;
    }

    auto packedImage = EXPECT_VALUE(PeImage::LoadFromFile(packed), "load compressed DLL");
    if (!packedImage) {
        return;
    }
    CHECK(packedImage->Info().upxPacked);

    auto unpackedResult = EXPECT_VALUE(
        UnpackWithUpx(packed, config->upxPath, unpacked), "unpack with UPX");
    if (!unpackedResult) {
        return;
    }
    CHECK(fs::exists(unpackedResult->outputPath));
    auto restored = EXPECT_VALUE(PeImage::LoadFromFile(unpackedResult->outputPath),
                                 "load unpacked DLL");
    if (restored) {
        CHECK(!restored->Info().upxPacked);
        CHECK(!restored->Resources().empty());
    }
}

void TestVersionCodec(const fs::path& samplePath) {
    std::cout << "[test] version info codec\n";
    auto image = EXPECT_VALUE(PeImage::LoadFromFile(samplePath), "load sample for version test");
    if (!image) {
        return;
    }
    const ResourceEntry* entry = FindEntry(*image, 16, 1, 1033);
    CHECK(entry != nullptr);
    if (entry == nullptr) {
        return;
    }

    auto version = EXPECT_VALUE(ParseVersionResource(entry->data), "parse version resource");
    if (!version) {
        return;
    }
    CHECK(version->fixed.present);
    CHECK(version->fixed.signature == 0xFEEF04BDu);
    CHECK(FormatVersionDwords(version->fixed.fileVersionMs, version->fixed.fileVersionLs) ==
          L"1.2.3.4");
    CHECK(FormatVersionDwords(version->fixed.productVersionMs, version->fixed.productVersionLs) ==
          L"1.2.3.4");
    CHECK(!version->stringTables.empty());
    if (!version->stringTables.empty()) {
        CHECK(IEqualsAscii(version->stringTables[0].key, L"040904B0"));
        bool foundCompany = false;
        for (const auto& string : version->stringTables[0].strings) {
            if (IEqualsAscii(string.key, L"CompanyName")) {
                foundCompany = true;
                CHECK(string.value == L"Peres Test");
            }
        }
        CHECK(foundCompany);
    }
    CHECK(version->translations.size() == 1);
    if (!version->translations.empty()) {
        CHECK(version->translations[0].language == 0x0409);
        CHECK(version->translations[0].codePage == 0x04B0);
    }
    const std::wstring text = FormatVersionText(*version);
    CHECK(text.find(L"FileVersion: 1.2.3.4") != std::wstring::npos);
    CHECK(text.find(L"CompanyName: Peres Test") != std::wstring::npos);

    // Cross-check the same resource through the Windows version API.
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(samplePath.c_str(), &ignored);
    CHECK(size > 0);
    if (size > 0) {
        std::vector<uint8_t> buffer(size);
        CHECK(::GetFileVersionInfoW(samplePath.c_str(), 0, size, buffer.data()));
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT fixedLength = 0;
        CHECK(::VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed),
                               &fixedLength));
        if (fixed != nullptr) {
            CHECK(fixed->dwSignature == 0xFEEF04BDu);
            CHECK(fixed->dwFileVersionMS == version->fixed.fileVersionMs);
            CHECK(fixed->dwFileVersionLS == version->fixed.fileVersionLs);
        }
        wchar_t* company = nullptr;
        UINT companyLength = 0;
        CHECK(::VerQueryValueW(buffer.data(),
                               L"\\StringFileInfo\\040904b0\\CompanyName",
                               reinterpret_cast<LPVOID*>(&company), &companyLength));
        CHECK(company != nullptr && std::wstring(company) == L"Peres Test");
    }
}

void TestXmlFormatter() {
    std::cout << "[test] XML node formatting\n";
    const std::wstring source =
        L"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        L"<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
        L"<assemblyIdentity type=\"win32\" name=\"Test\" version=\"1.0.0.0\" processorArchitecture=\"*\"/>"
        L"<description>Demo &amp; test</description>"
        L"<dependency><dependentAssembly>"
        L"<assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\" version=\"6.0.0.0\"/>"
        L"</dependentAssembly></dependency></assembly>";
    auto formatted = EXPECT_VALUE(FormatXmlText(source), "format XML");
    if (!formatted) {
        return;
    }
    CHECK(formatted->find(L"<?xml") != std::wstring::npos);
    CHECK(formatted->find(L"\r\n<assembly ") != std::wstring::npos);
    CHECK(formatted->find(L"\r\n  <assemblyIdentity ") != std::wstring::npos);
    CHECK(formatted->find(L"\r\n  <description>Demo &amp; test</description>") !=
          std::wstring::npos);
    CHECK(formatted->find(L"\r\n    <dependentAssembly>") != std::wstring::npos);
    CHECK(LooksLikeXml(source));
    CHECK(LooksLikeXml(L"  <?xml version=\"1.0\"?>"));
    CHECK(!LooksLikeXml(L"plain text"));

    CHECK(!FormatXmlText(L"<root><child></root>").has_value());
}

void TestUpxDetection() {
    std::cout << "[test] UPX section detection\n";
    auto normal = EXPECT_VALUE(PeImage::LoadFromBytes(MakeMinimalPe32(".text")), "load normal PE");
    if (normal) {
        CHECK(!normal->Info().upxPacked);
    }
    auto packed = EXPECT_VALUE(PeImage::LoadFromBytes(MakeMinimalPe32("UPX0")), "load UPX PE");
    if (packed) {
        CHECK(packed->Info().upxPacked);
    }
}

void TestApplicationExe() {
    std::cout << "[test] application executable resources\n";
    const fs::path exePath(L"" PERES_TEST_EXE);
    auto image = EXPECT_VALUE(PeImage::LoadFromFile(exePath), "load peres.exe");
    if (!image) {
        return;
    }
    CHECK(image->Info().is64);
    CHECK(image->Info().isDll == false);
    CHECK(!image->Resources().empty());

    bool hasIconGroup = false;
    bool hasManifest = false;
    for (const auto& entry : image->Resources()) {
        if (!entry.type.named && entry.type.id == 14) {
            hasIconGroup = true;
        }
        if (!entry.type.named && entry.type.id == 24) {
            hasManifest = true;
        }
    }
    CHECK(hasIconGroup);
    CHECK(hasManifest);
}

}  // namespace

int main() {
    ::SetConsoleOutputCP(CP_UTF8);
    std::cout << "peres core tests\n";

    TestMinimalPe32();
    TestCodecs();
    TestImageCodec();
    TestXmlFormatter();
    TestTextEncoding();
    TestUpxDetection();
    TestStringTableAndMenu(fs::path(L"" PERES_TEST_SAMPLE_DLL));
    TestDialogCodec(fs::path(L"" PERES_TEST_SAMPLE_DLL));
    TestVersionCodec(fs::path(L"" PERES_TEST_SAMPLE_DLL));
    TestUpxRoundTrip(fs::path(L"" PERES_TEST_EXE));
    TestSampleDll();
    TestApplicationExe();

    std::cout << fmt::format("checks: {}, failures: {}\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
