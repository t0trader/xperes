#pragma once

#include "Result.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace peres {

struct ProcessResult {
    uint32_t exitCode = 0;
    std::string output;
};

std::wstring QuoteWindowsArgument(std::wstring_view argument);
Result<ProcessResult> RunProcessCapture(const std::wstring& commandLine,
                                        uint32_t timeoutMs = 60000);

}  // namespace peres
