#pragma once

#include "ResourceId.h"

#include <cstdint>
#include <vector>

namespace peres {

struct ResourceEntry {
    ResourceId type;
    ResourceId name;
    uint16_t language = 0;
    uint32_t codePage = 0;
    std::vector<uint8_t> data;

    ResourceKey Key() const { return ResourceKey{type, name, language}; }
};

class ResourceModel {
public:
    ResourceModel() = default;
    explicit ResourceModel(std::vector<ResourceEntry> entries);

    void Reset(std::vector<ResourceEntry> entries);

    const std::vector<ResourceEntry>& Entries() const noexcept { return m_entries; }

    ResourceEntry* Find(const ResourceId& type, const ResourceId& name, uint16_t language);
    const ResourceEntry* Find(const ResourceId& type, const ResourceId& name, uint16_t language) const;

    std::vector<const ResourceEntry*> FindByName(const ResourceId& type, const ResourceId& name) const;

    // Adds the entry, or replaces its data/codepage when the key already exists.
    // Returns true when an existing entry was replaced.
    bool SetData(const ResourceId& type, const ResourceId& name, uint16_t language,
                 uint32_t codePage, std::vector<uint8_t> data);

    bool Remove(const ResourceId& type, const ResourceId& name, uint16_t language);

    bool IsDirty() const noexcept { return m_dirty; }
    void ClearDirty() noexcept { m_dirty = false; }
    void MarkDirty() noexcept { m_dirty = true; }

    void Sort();

private:
    std::vector<ResourceEntry> m_entries;
    bool m_dirty = false;
};

}  // namespace peres
