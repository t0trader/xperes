#pragma once

#include "Result.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace peres {

struct DialogString {
    bool ordinal = false;
    uint16_t ordinalValue = 0;
    std::wstring text;
};

struct DialogControl {
    uint32_t helpId = 0;
    uint32_t exStyle = 0;
    uint32_t style = 0;
    int16_t x = 0;
    int16_t y = 0;
    int16_t cx = 0;
    int16_t cy = 0;
    uint32_t id = 0;
    DialogString windowClass;
    DialogString title;
    std::vector<uint8_t> creationData;
};

struct DialogTemplate {
    bool extended = false;
    uint32_t helpId = 0;
    uint32_t exStyle = 0;
    uint32_t style = 0;
    int16_t x = 0;
    int16_t y = 0;
    int16_t cx = 0;
    int16_t cy = 0;
    DialogString menu;
    DialogString windowClass;
    DialogString title;
    bool hasFont = false;
    uint16_t pointSize = 0;
    uint16_t weight = 400;
    uint8_t italic = 0;
    uint8_t charset = 1;
    std::wstring typeface;
    std::vector<DialogControl> controls;
};

Result<DialogTemplate> ParseDialogResource(std::span<const uint8_t> data);
Result<std::vector<uint8_t>> BuildDialogResource(const DialogTemplate& dialog);
std::wstring FormatDialogText(const DialogTemplate& dialog);
Result<DialogTemplate> ParseDialogText(std::wstring_view text);

}  // namespace peres
