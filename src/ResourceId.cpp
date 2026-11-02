#include "ResourceId.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "TextUtil.h"

namespace peres {

std::wstring ResourceId::Display() const {
    if (named) {
        return name;
    }
    return L"#" + std::to_wstring(id);
}

int CompareResourceIds(const ResourceId& lhs, const ResourceId& rhs) {
    if (lhs.named != rhs.named) {
        return lhs.named ? -1 : 1;
    }
    if (!lhs.named) {
        if (lhs.id == rhs.id) {
            return 0;
        }
        return lhs.id < rhs.id ? -1 : 1;
    }

    const int insensitive = ::CompareStringOrdinal(lhs.name.c_str(), -1, rhs.name.c_str(), -1, TRUE);
    if (insensitive == CSTR_LESS_THAN) {
        return -1;
    }
    if (insensitive == CSTR_GREATER_THAN) {
        return 1;
    }
    // Same name ignoring case: use a deterministic case-sensitive tie-break so
    // that the output order is stable.
    const int sensitive = ::CompareStringOrdinal(lhs.name.c_str(), -1, rhs.name.c_str(), -1, FALSE);
    if (sensitive == CSTR_LESS_THAN) {
        return -1;
    }
    if (sensitive == CSTR_GREATER_THAN) {
        return 1;
    }
    return 0;
}

bool ResourceKeyLess::operator()(const ResourceKey& lhs, const ResourceKey& rhs) const {
    int result = CompareResourceIds(lhs.type, rhs.type);
    if (result != 0) {
        return result < 0;
    }
    result = CompareResourceIds(lhs.name, rhs.name);
    if (result != 0) {
        return result < 0;
    }
    return lhs.language < rhs.language;
}

}  // namespace peres
