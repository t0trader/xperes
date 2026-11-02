#pragma once

#include "Result.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace peres {

struct UnpackResult {
    std::filesystem::path outputPath;
    std::string toolOutput;
};

Result<UnpackResult> UnpackWithUpx(const std::filesystem::path& inputPath,
                                   const std::filesystem::path& upxPath,
                                   const std::filesystem::path& outputPath,
                                   uint32_t timeoutMs = 60000);

}  // namespace peres
