#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace peres {

struct ResourceId {
    bool named = false;
    uint16_t id = 0;
    std::wstring name;

    static ResourceId FromId(uint16_t value) { return ResourceId{false, value, {}}; }
    static ResourceId FromName(std::wstring value) {
        return ResourceId{true, 0, std::move(value)};
    }

    bool IsNamed() const noexcept { return named; }
    std::wstring Display() const;
    bool operator==(const ResourceId&) const = default;
};

// Windows resource directories place named entries before numeric IDs and use
// a case-insensitive comparison for the named entries.
int CompareResourceIds(const ResourceId& lhs, const ResourceId& rhs);

struct ResourceKey {
    ResourceId type;
    ResourceId name;
    uint16_t language = 0;

    bool operator==(const ResourceKey&) const = default;
};

struct ResourceKeyLess {
    bool operator()(const ResourceKey& lhs, const ResourceKey& rhs) const;
};

}  // namespace peres
