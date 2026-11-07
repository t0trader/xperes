#pragma once

#include "ResourceModel.h"
#include "Result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace peres {

struct PeSectionInfo {
    std::string name;
    uint32_t virtualSize = 0;
    uint32_t virtualAddress = 0;
    uint32_t rawSize = 0;
    uint32_t rawOffset = 0;
    uint32_t characteristics = 0;
};

struct PeInfo {
    bool is64 = false;
    bool isDll = false;
    bool hasResources = false;
    bool hasSignature = false;
    bool upxPacked = false;
    uint16_t machine = 0;
    uint32_t sectionCount = 0;
    uint32_t subsystem = 0;
    uint64_t fileSize = 0;
    uint64_t imageBase = 0;

    std::wstring MachineLabel() const;
    std::wstring FileKind() const;
    std::wstring ArchitectureLabel() const;
};

// Suggested file extension when exporting an embedded PE image.
std::wstring SuggestedPeExtension(const PeInfo& info);

struct PeExportSymbol {
    std::string name;       // Empty for exports that are only addressable by ordinal.
    uint32_t ordinal = 0;
    uint32_t rva = 0;
    std::string forwarder;  // Non-empty when the export is forwarded to another DLL.
};

struct PeExportInfo {
    std::string moduleName;
    uint32_t baseOrdinal = 0;
    uint32_t functionCount = 0;
    uint32_t nameCount = 0;
    uint32_t timestamp = 0;
    uint32_t directoryRva = 0;
    uint32_t directorySize = 0;
    std::vector<PeExportSymbol> symbols;
};

struct PeImportSymbol {
    std::string name;  // Empty for imports resolved by ordinal.
    uint16_t hint = 0;
    uint32_t ordinal = 0;
    bool byOrdinal = false;
    uint32_t iatRva = 0;
};

struct PeImportModule {
    std::string name;
    uint32_t descriptorRva = 0;
    uint32_t originalFirstThunkRva = 0;
    uint32_t firstThunkRva = 0;
    uint32_t timestamp = 0;
    std::vector<PeImportSymbol> symbols;
};

class PeImage {
public:
    static Result<PeImage> LoadFromFile(const std::filesystem::path& path);
    static Result<PeImage> LoadFromBytes(std::vector<uint8_t> bytes);

    const PeInfo& Info() const noexcept { return m_info; }
    const std::vector<PeSectionInfo>& Sections() const noexcept { return m_sections; }
    const std::vector<ResourceEntry>& Resources() const noexcept { return m_resources; }
    const std::optional<PeExportInfo>& Exports() const noexcept { return m_exports; }
    const std::vector<PeImportModule>& Imports() const noexcept { return m_imports; }
    const std::vector<uint8_t>& OriginalBytes() const noexcept { return m_bytes; }

    // Rebuilds the resource section from |model| and returns the complete new
    // file image.  The original resource section is updated in place when it
    // has enough room; otherwise a new .rsrc section is appended.  A digital
    // signature (if any) is removed because it cannot survive modification.
    Result<std::vector<uint8_t>> CompileResources(const ResourceModel& model) const;

private:
    PeImage() = default;

    Result<void> Parse();
    Result<uint32_t> RvaToOffset(uint32_t rva, uint32_t size) const;
    Result<uint32_t> ResourceSectionFileLimit(uint32_t rva) const;
    std::optional<std::string> ReadAsciiStringAtRva(uint32_t rva, size_t maxLength) const;
    void ParseExports();
    void ParseImports();

    std::vector<uint8_t> m_bytes;
    PeInfo m_info;
    std::vector<PeSectionInfo> m_sections;
    std::vector<ResourceEntry> m_resources;
    std::optional<PeExportInfo> m_exports;
    std::vector<PeImportModule> m_imports;

    uint32_t m_peOffset = 0;
    uint32_t m_fileHeaderOffset = 0;
    uint32_t m_optOffset = 0;
    uint32_t m_sizeOfOptionalHeader = 0;
    uint32_t m_dataDirOffset = 0;
    uint32_t m_dataDirCount = 0;
    uint16_t m_fileCharacteristics = 0;
    uint32_t m_sectionAlignment = 0;
    uint32_t m_fileAlignment = 0;
    uint32_t m_sizeOfHeaders = 0;
    uint32_t m_sizeOfImage = 0;
    uint32_t m_resourceRva = 0;
    uint32_t m_resourceSize = 0;
    uint32_t m_securityOffset = 0;
    uint32_t m_securitySize = 0;
    uint16_t m_numberOfSections = 0;
    uint16_t m_optionalMagic = 0;
};

}  // namespace peres
