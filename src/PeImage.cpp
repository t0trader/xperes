#include "PeImage.h"

#include "ResourceSection.h"
#include "TextUtil.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>

namespace peres {

namespace {

constexpr uint16_t kDosSignature = 0x5A4D;
constexpr uint32_t kNtSignature = 0x00004550;
constexpr uint16_t kPe32Magic = 0x010B;
constexpr uint16_t kPe32PlusMagic = 0x020B;
constexpr uint16_t kMachineI386 = 0x014C;
constexpr uint16_t kMachineAmd64 = 0x8664;
constexpr uint16_t kMachineArm64 = 0xAA64;
constexpr uint16_t kMachineArm = 0x01C0;
constexpr uint16_t kMachineThumb = 0x01C2;
constexpr uint16_t kMachineArmNt = 0x01C4;
constexpr uint16_t kMachineIa64 = 0x0200;
constexpr uint16_t kFileDll = 0x2000;
constexpr uint32_t kResourceDirectoryIndex = 2;
constexpr uint32_t kSecurityDirectoryIndex = 4;
constexpr uint32_t kMaxResourceEntries = 200000;
constexpr uint32_t kMaxDirectoryEntries = 100000;
constexpr uint32_t kMaxResourceSize = 0x40000000;  // 1 GiB per resource
constexpr uint32_t kMaxSections = 96;

uint16_t ReadU16(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

uint64_t ReadU64(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint64_t>(ReadU32(bytes, offset)) |
           (static_cast<uint64_t>(ReadU32(bytes, offset + 4)) << 32);
}

void WriteU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value & 0xFF);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void WriteU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value & 0xFF);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    bytes[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    bytes[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

bool IsPowerOfTwo(uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

std::optional<uint32_t> AlignUp32(uint64_t value, uint32_t alignment) {
    if (alignment == 0) {
        return std::nullopt;
    }
    const uint64_t aligned = (value + alignment - 1) / alignment * alignment;
    if (aligned > 0xFFFFFFFFull) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(aligned);
}

bool SectionNameEquals(const std::string& name, const char* expected) {
    const size_t length = std::strlen(expected);
    if (name.size() < length) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        char a = name[i];
        char b = expected[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

uint32_t SectionUsedSize(const PeSectionInfo& section) {
    return std::max(section.virtualSize, section.rawSize);
}

}  // namespace

std::wstring PeInfo::MachineLabel() const {
    switch (machine) {
        case kMachineI386:
            return L"x86";
        case kMachineAmd64:
            return L"x64";
        case kMachineArm64:
            return L"ARM64";
        case kMachineArm:
            return L"ARM";
        case kMachineThumb:
            return L"Thumb";
        case kMachineArmNt:
            return L"ARM NT";
        case kMachineIa64:
            return L"IA64";
        default:
            return Utf8ToWide(fmt::format("0x{:04X}", machine));
    }
}

std::wstring PeInfo::FileKind() const {
    return isDll ? L"DLL" : L"EXE";
}

std::wstring PeInfo::ArchitectureLabel() const {
    return is64 ? L"PE32+" : L"PE32";
}

std::wstring SuggestedPeExtension(const PeInfo& info) {
    if (info.isDll) {
        return L"dll";
    }
    if (info.subsystem == 1) {  // IMAGE_SUBSYSTEM_NATIVE
        return L"sys";
    }
    return L"exe";
}

Result<PeImage> PeImage::LoadFromFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return MakeError(L"无法打开文件：" + path.wstring());
    }
    const std::streamoff size = static_cast<std::streamoff>(file.tellg());
    if (size <= 0) {
        return MakeError(L"文件为空：" + path.wstring());
    }
    if (size > 0x7FFFFFFF) {
        return MakeError(L"文件超过 2 GB，暂不支持：" + path.wstring());
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        return MakeError(L"读取文件失败：" + path.wstring());
    }
    return LoadFromBytes(std::move(bytes));
}

Result<PeImage> PeImage::LoadFromBytes(std::vector<uint8_t> bytes) {
    PeImage image;
    image.m_bytes = std::move(bytes);
    auto result = image.Parse();
    if (!result) {
        return std::unexpected(result.error());
    }
    return image;
}

Result<uint32_t> PeImage::RvaToOffset(uint32_t rva, uint32_t size) const {
    const uint64_t end = static_cast<uint64_t>(rva) + size;
    for (const auto& section : m_sections) {
        if (rva < section.virtualAddress) {
            continue;
        }
        const uint64_t sectionEnd = static_cast<uint64_t>(section.virtualAddress) + section.rawSize;
        if (end <= sectionEnd) {
            const uint64_t offset =
                static_cast<uint64_t>(section.rawOffset) + (rva - section.virtualAddress);
            if (offset <= 0xFFFFFFFFull && offset + size <= m_bytes.size()) {
                return static_cast<uint32_t>(offset);
            }
        }
    }

    if (rva < m_sizeOfHeaders && end <= m_sizeOfHeaders && end <= m_bytes.size()) {
        return rva;
    }
    return MakeError(Utf8ToWide(fmt::format("RVA 0x{:08X} 无法映射到文件偏移", rva)));
}

Result<uint32_t> PeImage::ResourceSectionFileLimit(uint32_t rva) const {
    for (const auto& section : m_sections) {
        if (rva >= section.virtualAddress &&
            rva < static_cast<uint64_t>(section.virtualAddress) + section.rawSize) {
            const uint64_t end = static_cast<uint64_t>(section.rawOffset) + section.rawSize;
            return static_cast<uint32_t>(std::min<uint64_t>(end, m_bytes.size()));
        }
    }
    if (rva < m_sizeOfHeaders && m_sizeOfHeaders <= m_bytes.size()) {
        return m_sizeOfHeaders;
    }
    return MakeError(L"资源目录不在任何已初始化的节中");
}

Result<void> PeImage::Parse() {
    if (m_bytes.size() < 0x40) {
        return MakeError(L"文件太小，不是有效的 PE 文件");
    }
    if (ReadU16(m_bytes, 0) != kDosSignature) {
        return MakeError(L"缺少 MZ 头，不是有效的 DOS/PE 文件");
    }

    const uint32_t peOffset = ReadU32(m_bytes, 0x3C);
    if (peOffset > m_bytes.size() - 4 || peOffset < 0x40) {
        return MakeError(L"e_lfanew 超出文件范围");
    }
    if (ReadU32(m_bytes, peOffset) != kNtSignature) {
        return MakeError(L"缺少 PE\\0\\0 签名");
    }

    m_peOffset = peOffset;
    m_fileHeaderOffset = peOffset + 4;
    if (m_fileHeaderOffset + 20 > m_bytes.size()) {
        return MakeError(L"PE 文件头不完整");
    }

    const uint16_t machine = ReadU16(m_bytes, m_fileHeaderOffset);
    const uint16_t numberOfSections = ReadU16(m_bytes, m_fileHeaderOffset + 2);
    const uint16_t sizeOfOptionalHeader = ReadU16(m_bytes, m_fileHeaderOffset + 16);
    const uint16_t fileCharacteristics = ReadU16(m_bytes, m_fileHeaderOffset + 18);

    m_optOffset = m_fileHeaderOffset + 20;
    if (sizeOfOptionalHeader < 2 || m_optOffset + sizeOfOptionalHeader > m_bytes.size()) {
        return MakeError(L"可选头尺寸无效");
    }
    m_sizeOfOptionalHeader = sizeOfOptionalHeader;
    m_numberOfSections = numberOfSections;
    m_fileCharacteristics = fileCharacteristics;

    const uint16_t magic = ReadU16(m_bytes, m_optOffset);
    if (magic != kPe32Magic && magic != kPe32PlusMagic) {
        return MakeError(Utf8ToWide(fmt::format("不支持的可选头魔数 0x{:04X}", magic)));
    }
    m_optionalMagic = magic;
    const bool is64 = magic == kPe32PlusMagic;

    const uint32_t dataDirOffset = is64 ? 112u : 96u;
    if (sizeOfOptionalHeader < dataDirOffset + 4) {
        return MakeError(L"可选头太短，缺少数据目录");
    }

    const uint32_t declaredDirCount = ReadU32(m_bytes, m_optOffset + (is64 ? 108u : 92u));
    const uint32_t availableDirCount = (sizeOfOptionalHeader - dataDirOffset) / 8;
    m_dataDirCount = std::min({declaredDirCount, availableDirCount, 16u});
    m_dataDirOffset = m_optOffset + dataDirOffset;

    m_sectionAlignment = ReadU32(m_bytes, m_optOffset + 32);
    m_fileAlignment = ReadU32(m_bytes, m_optOffset + 36);
    m_sizeOfImage = ReadU32(m_bytes, m_optOffset + 56);
    m_sizeOfHeaders = ReadU32(m_bytes, m_optOffset + 60);
    const uint64_t imageBase = is64 ? ReadU64(m_bytes, m_optOffset + 24)
                                   : ReadU32(m_bytes, m_optOffset + 28);
    const uint16_t subsystem = (sizeOfOptionalHeader >= 70) ? ReadU16(m_bytes, m_optOffset + 68) : 0;

    if (numberOfSections > kMaxSections) {
        return MakeError(Utf8ToWide(fmt::format("节数量 {} 超过支持的 96 个", numberOfSections)));
    }

    const uint64_t sectionTableOffset = static_cast<uint64_t>(m_optOffset) + sizeOfOptionalHeader;
    const uint64_t sectionTableEnd = sectionTableOffset + static_cast<uint64_t>(numberOfSections) * 40;
    if (sectionTableEnd > m_bytes.size()) {
        return MakeError(L"节表超出文件范围");
    }

    m_sections.reserve(numberOfSections);
    for (uint16_t i = 0; i < numberOfSections; ++i) {
        const uint64_t offset = sectionTableOffset + static_cast<uint64_t>(i) * 40;
        PeSectionInfo section;
        std::string name;
        for (size_t c = 0; c < 8 && m_bytes[offset + c] != 0; ++c) {
            name.push_back(static_cast<char>(m_bytes[offset + c]));
        }
        section.name = std::move(name);
        section.virtualSize = ReadU32(m_bytes, offset + 8);
        section.virtualAddress = ReadU32(m_bytes, offset + 12);
        section.rawSize = ReadU32(m_bytes, offset + 16);
        section.rawOffset = ReadU32(m_bytes, offset + 20);
        section.characteristics = ReadU32(m_bytes, offset + 36);

        if (section.rawSize != 0) {
            const uint64_t end = static_cast<uint64_t>(section.rawOffset) + section.rawSize;
            if (section.rawOffset == 0 || end > m_bytes.size()) {
                return MakeError(Utf8ToWide(
                    fmt::format("节 {} 的原始数据范围超出文件", section.name)));
            }
        }
        m_sections.push_back(std::move(section));
    }

    m_info.is64 = is64;
    m_info.machine = machine;
    m_info.fileSize = m_bytes.size();
    m_info.imageBase = imageBase;
    m_info.subsystem = subsystem;
    m_info.sectionCount = numberOfSections;
    m_info.isDll = (fileCharacteristics & kFileDll) != 0;

    const uint8_t* peBytes = m_bytes.data();
    for (size_t i = 0; i + 4 <= std::min<size_t>(m_bytes.size(), 2u * 1024u * 1024u); ++i) {
        if (peBytes[i] == 'U' && peBytes[i + 1] == 'P' && peBytes[i + 2] == 'X' &&
            peBytes[i + 3] == '!') {
            m_info.upxPacked = true;
            break;
        }
    }
    for (const auto& section : m_sections) {
        if (section.name.size() >= 3) {
            const char a = section.name[0];
            const char b = section.name[1];
            const char c = section.name[2];
            const bool upx = (a == 'U' || a == 'u') && (b == 'P' || b == 'p') && (c == 'X' || c == 'x');
            if (upx) {
                m_info.upxPacked = true;
                break;
            }
        }
    }

    if (m_dataDirCount > kResourceDirectoryIndex) {
        m_resourceRva = ReadU32(m_bytes, m_dataDirOffset + kResourceDirectoryIndex * 8);
        m_resourceSize = ReadU32(m_bytes, m_dataDirOffset + kResourceDirectoryIndex * 8 + 4);
        m_info.hasResources = m_resourceRva != 0 && m_resourceSize != 0;
    }
    if (m_dataDirCount > kSecurityDirectoryIndex) {
        m_securityOffset = ReadU32(m_bytes, m_dataDirOffset + kSecurityDirectoryIndex * 8);
        m_securitySize = ReadU32(m_bytes, m_dataDirOffset + kSecurityDirectoryIndex * 8 + 4);
        m_info.hasSignature = m_securityOffset != 0 && m_securitySize >= 8 &&
                              static_cast<uint64_t>(m_securityOffset) + m_securitySize <= m_bytes.size();
    }

    if (m_info.hasResources) {
        auto dirOffset = RvaToOffset(m_resourceRva, 1);
        if (!dirOffset) {
            return std::unexpected(dirOffset.error());
        }
        auto limit = ResourceSectionFileLimit(m_resourceRva);
        if (!limit) {
            return std::unexpected(limit.error());
        }

        const uint32_t directoryStart = *dirOffset;
        const uint32_t directoryEnd = *limit;
        if (directoryStart >= directoryEnd || directoryEnd - directoryStart < 16) {
            return MakeError(L"资源目录起始偏移无效");
        }

        uint32_t parsedCount = 0;
        uint32_t directoryVisits = 0;
        std::function<Result<void>(uint32_t, int, std::vector<ResourceId>&)> parseDirectory;
        parseDirectory = [&](uint32_t relativeOffset, int depth,
                             std::vector<ResourceId>& path) -> Result<void> {
            if (++directoryVisits > kMaxDirectoryEntries) {
                return MakeError(L"资源目录结构异常（引用过多）");
            }
            if (relativeOffset > directoryEnd - directoryStart - 16) {
                return MakeError(L"资源目录偏移超出资源节");
            }
            const uint32_t at = directoryStart + relativeOffset;
            const uint16_t namedCount = ReadU16(m_bytes, at + 12);
            const uint16_t idCount = ReadU16(m_bytes, at + 14);
            const uint32_t total = static_cast<uint32_t>(namedCount) + idCount;
            if (total > kMaxDirectoryEntries) {
                return MakeError(L"资源目录条目数异常");
            }
            const uint64_t tableEnd =
                static_cast<uint64_t>(at) + 16ull + static_cast<uint64_t>(total) * 8ull;
            if (tableEnd > directoryEnd) {
                return MakeError(L"资源目录条目表超出资源节");
            }

            for (uint32_t i = 0; i < total; ++i) {
                const uint32_t entryAt = at + 16 + i * 8;
                const uint32_t nameField = ReadU32(m_bytes, entryAt);
                const uint32_t offsetField = ReadU32(m_bytes, entryAt + 4);

                ResourceId id;
                if ((nameField & 0x80000000u) != 0) {
                    const uint32_t stringOffset = nameField & 0x7FFFFFFFu;
                    if (stringOffset > directoryEnd - directoryStart - 2) {
                        return MakeError(L"资源名称偏移超出资源节");
                    }
                    const uint32_t stringAt = directoryStart + stringOffset;
                    const uint16_t nameLength = ReadU16(m_bytes, stringAt);
                    const uint64_t stringEnd = static_cast<uint64_t>(stringAt) + 2ull +
                                               static_cast<uint64_t>(nameLength) * 2ull;
                    if (stringEnd > directoryEnd) {
                        return MakeError(L"资源名称字符串超出资源节");
                    }
                    std::wstring name;
                    name.reserve(nameLength);
                    for (uint16_t c = 0; c < nameLength; ++c) {
                        const uint16_t ch = ReadU16(m_bytes, stringAt + 2 + c * 2);
                        name.push_back(static_cast<wchar_t>(ch));
                    }
                    id = ResourceId::FromName(std::move(name));
                } else {
                    id = ResourceId::FromId(static_cast<uint16_t>(nameField & 0xFFFFu));
                }

                if (depth > 2) {
                    return MakeError(L"资源树层级超过 3，暂不支持");
                }

                if ((offsetField & 0x80000000u) != 0) {
                    if (depth >= 2) {
                        return MakeError(L"资源树层级超过 3，暂不支持");
                    }
                    const uint32_t childOffset = offsetField & 0x7FFFFFFFu;
                    path.push_back(id);
                    auto result = parseDirectory(childOffset, depth + 1, path);
                    path.pop_back();
                    if (!result) {
                        return result;
                    }
                } else {
                    if (depth != 2) {
                        return MakeError(L"资源树缺少语言层，无法解析");
                    }
                    if (id.named) {
                        return MakeError(L"资源语言使用了字符串名称，无法解析");
                    }
                    const uint32_t dataEntryOffset = offsetField & 0x7FFFFFFFu;
                    if (dataEntryOffset > directoryEnd - directoryStart - 16) {
                        return MakeError(L"资源数据条目偏移超出资源节");
                    }
                    const uint32_t dataEntryAt = directoryStart + dataEntryOffset;
                    const uint32_t dataRva = ReadU32(m_bytes, dataEntryAt);
                    const uint32_t dataSize = ReadU32(m_bytes, dataEntryAt + 4);
                    const uint32_t codePage = ReadU32(m_bytes, dataEntryAt + 8);

                    if (dataSize > kMaxResourceSize) {
                        return MakeError(L"单个资源数据过大");
                    }
                    if (parsedCount >= kMaxResourceEntries) {
                        return MakeError(L"资源条目数量异常");
                    }
                    if (path.size() != 2) {
                        return MakeError(L"资源路径不完整");
                    }

                    std::vector<uint8_t> data;
                    if (dataSize != 0) {
                        auto dataOffset = RvaToOffset(dataRva, dataSize);
                        if (!dataOffset) {
                            return std::unexpected(dataOffset.error());
                        }
                        data.assign(m_bytes.begin() + *dataOffset,
                                    m_bytes.begin() + *dataOffset + dataSize);
                    }

                    m_resources.push_back(ResourceEntry{
                        path[0],
                        path[1],
                        id.id,
                        codePage,
                        std::move(data),
                    });
                    ++parsedCount;
                }
            }
            return {};
        };

        std::vector<ResourceId> path;
        auto parseResult = parseDirectory(0, 0, path);
        if (!parseResult) {
            return std::unexpected(parseResult.error());
        }
    }

    m_info.hasResources = !m_resources.empty();
    return {};
}

Result<std::vector<uint8_t>> PeImage::CompileResources(const ResourceModel& model) const {
    if (m_dataDirCount <= kResourceDirectoryIndex) {
        return MakeError(L"该文件没有资源数据目录，无法写入资源");
    }
    if (!IsPowerOfTwo(m_fileAlignment) || !IsPowerOfTwo(m_sectionAlignment)) {
        return MakeError(L"文件/节对齐值无效，无法重建资源节");
    }
    if (m_fileAlignment < 0x200 || m_sectionAlignment < m_fileAlignment) {
        return MakeError(L"文件/节对齐值不符合 PE 规范，无法重建资源节");
    }

    // The blob size does not depend on the section RVA, so use a placeholder
    // RVA first to decide whether the old resource section can be reused.
    auto sizingResult = BuildResourceSection(model, 0);
    if (!sizingResult) {
        return std::unexpected(sizingResult.error());
    }
    const uint32_t blobSize = static_cast<uint32_t>(sizingResult->size());

    std::vector<uint8_t> out = m_bytes;

    // A modified signed image cannot validate its Authenticode signature.
    // Clear the certificate directory and, when the certificate lives at the
    // very end of the file, trim it to avoid leaving stale signature bytes.
    if (m_dataDirCount > kSecurityDirectoryIndex) {
        const uint64_t directoryAt =
            static_cast<uint64_t>(m_dataDirOffset) + kSecurityDirectoryIndex * 8ull;
        WriteU32(out, static_cast<size_t>(directoryAt), 0);
        WriteU32(out, static_cast<size_t>(directoryAt) + 4, 0);

        if (m_securityOffset != 0 && m_securitySize >= 8) {
            const uint64_t certEnd = static_cast<uint64_t>(m_securityOffset) + m_securitySize;
            if (certEnd <= out.size()) {
                uint64_t lastSectionEnd = 0;
                for (const auto& section : m_sections) {
                    if (section.rawSize != 0) {
                        lastSectionEnd =
                            std::max<uint64_t>(lastSectionEnd,
                                               static_cast<uint64_t>(section.rawOffset) +
                                                   section.rawSize);
                    }
                }
                const uint64_t trailing = out.size() - certEnd;
                if (m_securityOffset >= lastSectionEnd && trailing <= 0x200) {
                    out.resize(m_securityOffset);
                }
            }
        }
    }

    const uint64_t sectionTableOffset =
        static_cast<uint64_t>(m_optOffset) + m_sizeOfOptionalHeader;

    // Try to update the existing .rsrc section in place.
    std::optional<size_t> oldResourceIndex;
    if (m_resourceRva != 0) {
        for (size_t i = 0; i < m_sections.size(); ++i) {
            const auto& section = m_sections[i];
            if (m_resourceRva >= section.virtualAddress &&
                m_resourceRva < static_cast<uint64_t>(section.virtualAddress) + section.rawSize) {
                oldResourceIndex = i;
                break;
            }
        }
    }

    bool inPlace = false;
    if (oldResourceIndex) {
        const auto& section = m_sections[*oldResourceIndex];
        inPlace = SectionNameEquals(section.name, ".rsrc") &&
                  section.virtualAddress == m_resourceRva &&
                  section.rawOffset != 0 &&
                  static_cast<uint64_t>(section.rawOffset) + section.rawSize <= out.size() &&
                  blobSize <= section.rawSize;
    }

    uint32_t newRva = 0;
    uint32_t rawOffset = 0;
    uint32_t rawSize = 0;
    uint32_t sectionHeaderOffset = 0;

    if (inPlace) {
        newRva = m_sections[*oldResourceIndex].virtualAddress;
    } else {
        if (m_numberOfSections >= kMaxSections) {
            return MakeError(L"节数量已达上限，无法追加资源节");
        }

        uint64_t endVa = m_sizeOfHeaders;
        for (const auto& section : m_sections) {
            endVa = std::max<uint64_t>(endVa,
                                       static_cast<uint64_t>(section.virtualAddress) +
                                           SectionUsedSize(section));
        }
        const auto alignedVa = AlignUp32(endVa, m_sectionAlignment);
        if (!alignedVa) {
            return MakeError(L"新的资源节 RVA 超出 32 位范围");
        }
        newRva = *alignedVa;

        const uint64_t newSectionEnd = sectionTableOffset +
                                       static_cast<uint64_t>(m_numberOfSections + 1) * 40ull;
        uint64_t firstRaw = 0;
        for (const auto& section : m_sections) {
            if (section.rawSize != 0) {
                firstRaw = firstRaw == 0 ? section.rawOffset
                                         : std::min<uint64_t>(firstRaw, section.rawOffset);
            }
        }
        uint64_t headerLimit = m_sizeOfHeaders;
        if (firstRaw != 0) {
            headerLimit = headerLimit == 0 ? firstRaw : std::min(headerLimit, firstRaw);
        }
        if (headerLimit == 0) {
            return MakeError(L"文件头尺寸无效");
        }
        if (newSectionEnd > headerLimit || newSectionEnd > out.size()) {
            return MakeError(L"PE 头后没有足够空间追加节表项，无法保存资源");
        }

        const auto alignedRaw = AlignUp32(out.size(), m_fileAlignment);
        if (!alignedRaw) {
            return MakeError(L"新的资源节文件偏移超出 32 位范围");
        }
        rawOffset = *alignedRaw;
        const auto alignedRawSize = AlignUp32(blobSize, m_fileAlignment);
        if (!alignedRawSize) {
            return MakeError(L"新的资源节大小超出 32 位范围");
        }
        rawSize = *alignedRawSize;

        if (static_cast<uint64_t>(rawOffset) + rawSize > 0xFFFFFFFFull) {
            return MakeError(L"文件大小超出 PE 32 位范围");
        }
        sectionHeaderOffset =
            static_cast<uint32_t>(sectionTableOffset + static_cast<uint64_t>(m_numberOfSections) * 40);
    }

    auto blobResult = BuildResourceSection(model, newRva);
    if (!blobResult) {
        return std::unexpected(blobResult.error());
    }
    if (!inPlace && blobResult->size() != blobSize) {
        return MakeError(L"资源节序列化大小不一致");
    }

    if (inPlace) {
        auto& section = m_sections[*oldResourceIndex];
        // Re-check after possible signature trimming.
        if (static_cast<uint64_t>(section.rawOffset) + section.rawSize > out.size()) {
            return MakeError(L"资源节原始数据超出文件范围");
        }
        std::memset(out.data() + section.rawOffset, 0, section.rawSize);
        std::memcpy(out.data() + section.rawOffset, blobResult->data(), blobResult->size());

        const uint64_t sectionHeader = sectionTableOffset + static_cast<uint64_t>(*oldResourceIndex) * 40;
        WriteU32(out, static_cast<size_t>(sectionHeader + 8), static_cast<uint32_t>(blobResult->size()));

        const uint64_t directoryAt =
            static_cast<uint64_t>(m_dataDirOffset) + kResourceDirectoryIndex * 8ull;
        WriteU32(out, static_cast<size_t>(directoryAt), newRva);
        WriteU32(out, static_cast<size_t>(directoryAt) + 4, static_cast<uint32_t>(blobResult->size()));

        const auto newImageEnd =
            AlignUp32(static_cast<uint64_t>(newRva) + blobResult->size(), m_sectionAlignment);
        if (!newImageEnd) {
            return MakeError(L"资源节超出 SizeOfImage 可表示范围");
        }
        if (*newImageEnd > m_sizeOfImage) {
            WriteU32(out, m_optOffset + 56, *newImageEnd);
        }
    } else {
        out.resize(rawOffset, 0);
        out.resize(static_cast<size_t>(rawOffset) + rawSize, 0);
        std::memcpy(out.data() + rawOffset, blobResult->data(), blobResult->size());

        std::memset(out.data() + sectionHeaderOffset, 0, 40);
        std::memcpy(out.data() + sectionHeaderOffset, ".rsrc", 5);
        WriteU32(out, sectionHeaderOffset + 8, static_cast<uint32_t>(blobResult->size()));
        WriteU32(out, sectionHeaderOffset + 12, newRva);
        WriteU32(out, sectionHeaderOffset + 16, rawSize);
        WriteU32(out, sectionHeaderOffset + 20, rawOffset);
        WriteU32(out, sectionHeaderOffset + 36, 0x40000040u);

        WriteU16(out, m_fileHeaderOffset + 2,
                 static_cast<uint16_t>(m_numberOfSections + 1));

        if (oldResourceIndex) {
            auto& section = m_sections[*oldResourceIndex];
            if (SectionNameEquals(section.name, ".rsrc")) {
                const uint64_t oldHeader = sectionTableOffset +
                                           static_cast<uint64_t>(*oldResourceIndex) * 40;
                for (size_t c = 0; c < 8; ++c) {
                    out[oldHeader + c] = 0;
                }
                std::memcpy(out.data() + oldHeader, ".oldrsrc", 8);
            }
        }

        const auto newImageEnd =
            AlignUp32(static_cast<uint64_t>(newRva) + blobResult->size(), m_sectionAlignment);
        if (!newImageEnd) {
            return MakeError(L"资源节超出 SizeOfImage 可表示范围");
        }
        WriteU32(out, m_optOffset + 56, *newImageEnd);

        if (m_sizeOfOptionalHeader >= 12) {
            const uint64_t initialized = static_cast<uint64_t>(ReadU32(out, m_optOffset + 8)) + rawSize;
            if (initialized > 0xFFFFFFFFull) {
                return MakeError(L"SizeOfInitializedData 溢出");
            }
            WriteU32(out, m_optOffset + 8, static_cast<uint32_t>(initialized));
        }

        const uint64_t directoryAt =
            static_cast<uint64_t>(m_dataDirOffset) + kResourceDirectoryIndex * 8ull;
        WriteU32(out, static_cast<size_t>(directoryAt), newRva);
        WriteU32(out, static_cast<size_t>(directoryAt) + 4,
                 static_cast<uint32_t>(blobResult->size()));
    }

    // Refresh the optional-header checksum.
    if (m_sizeOfOptionalHeader >= 68) {
        const uint32_t checksumOffset = m_optOffset + 64;
        WriteU32(out, checksumOffset, 0);
        uint32_t sum = 0;
        for (size_t i = 0; i < out.size(); i += 2) {
            if (i == checksumOffset || i == checksumOffset + 2) {
                continue;
            }
            const uint16_t word = static_cast<uint16_t>(
                out[i] | ((i + 1 < out.size()) ? (static_cast<uint32_t>(out[i + 1]) << 8) : 0));
            sum += word;
            sum = (sum & 0xFFFFu) + (sum >> 16);
        }
        sum = (sum & 0xFFFFu) + (sum >> 16);
        sum += static_cast<uint32_t>(out.size());
        WriteU32(out, checksumOffset, sum);
    }

    return out;
}

}  // namespace peres
