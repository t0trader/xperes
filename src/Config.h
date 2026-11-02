#pragma once

#include "Result.h"

#include <filesystem>
#include <string>

namespace peres {

struct AppConfig {
    std::filesystem::path iniPath;
    std::filesystem::path upxPath;
};

std::filesystem::path ExecutableDirectory();
std::filesystem::path ConfigIniPath();
Result<AppConfig> LoadConfig();

// Creates a template peres.ini next to the executable if it does not exist.
Result<void> EnsureConfigIniExists();

}  // namespace peres
