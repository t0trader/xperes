#pragma once

#include "Result.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace peres {

enum class MenuTemplateFormat {
    Classic,
    Extended,
};

struct MenuItem {
    bool popup = false;
    bool separator = false;
    bool last = false;
    uint32_t id = 0;
    uint32_t type = 0;       // MENUEX MFT flags
    uint32_t state = 0;      // MENUEX MFS flags
    uint32_t helpId = 0;     // MENUEX popup help id
    uint16_t classicFlags = 0;
    std::wstring text;
    std::vector<MenuItem> children;
};

struct MenuResource {
    MenuTemplateFormat format = MenuTemplateFormat::Classic;
    uint32_t helpId = 0;
    std::vector<MenuItem> items;
};

Result<MenuResource> ParseMenuResource(std::span<const uint8_t> data);
Result<std::vector<uint8_t>> BuildMenuResource(const MenuResource& menu);
std::wstring FormatMenuText(const MenuResource& menu);
Result<MenuResource> ParseMenuText(std::wstring_view text);

}  // namespace peres
