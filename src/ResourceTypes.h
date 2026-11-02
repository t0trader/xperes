#pragma once

#include "ResourceId.h"

#include <cstdint>
#include <string>

namespace peres {

enum class ResourceType : uint16_t {
    Cursor = 1,
    Bitmap = 2,
    Icon = 3,
    Menu = 4,
    Dialog = 5,
    String = 6,
    FontDir = 7,
    Font = 8,
    Accelerator = 9,
    RcData = 10,
    MessageTable = 11,
    GroupCursor = 12,
    GroupIcon = 14,
    Version = 16,
    DlgInclude = 17,
    PlugPlay = 19,
    Vxd = 20,
    AniCursor = 21,
    AniIcon = 22,
    Html = 23,
    Manifest = 24,
};

bool IsResourceType(const ResourceId& type, ResourceType expected);
std::wstring ResourceTypeLabel(const ResourceId& type);
std::wstring LanguageLabel(uint16_t language);
std::wstring QuoteResourceName(const ResourceId& id);

}  // namespace peres
