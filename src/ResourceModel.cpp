#include "ResourceModel.h"

#include <algorithm>

namespace peres {

namespace {

bool EntryLess(const ResourceEntry& lhs, const ResourceEntry& rhs) {
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

bool SameKey(const ResourceEntry& lhs, const ResourceEntry& rhs) {
    return lhs.type == rhs.type && lhs.name == rhs.name && lhs.language == rhs.language;
}

}  // namespace

ResourceModel::ResourceModel(std::vector<ResourceEntry> entries) {
    Reset(std::move(entries));
}

void ResourceModel::Reset(std::vector<ResourceEntry> entries) {
    m_entries = std::move(entries);
    Sort();
    m_entries.erase(std::unique(m_entries.begin(), m_entries.end(), SameKey), m_entries.end());
    m_dirty = false;
}

void ResourceModel::Sort() {
    std::stable_sort(m_entries.begin(), m_entries.end(), EntryLess);
}

ResourceEntry* ResourceModel::Find(const ResourceId& type, const ResourceId& name, uint16_t language) {
    const auto it = std::ranges::find_if(m_entries, [&](const ResourceEntry& entry) {
        return entry.type == type && entry.name == name && entry.language == language;
    });
    return it == m_entries.end() ? nullptr : &*it;
}

const ResourceEntry* ResourceModel::Find(const ResourceId& type, const ResourceId& name,
                                         uint16_t language) const {
    const auto it = std::ranges::find_if(m_entries, [&](const ResourceEntry& entry) {
        return entry.type == type && entry.name == name && entry.language == language;
    });
    return it == m_entries.end() ? nullptr : &*it;
}

std::vector<const ResourceEntry*> ResourceModel::FindByName(const ResourceId& type,
                                                            const ResourceId& name) const {
    std::vector<const ResourceEntry*> result;
    for (const auto& entry : m_entries) {
        if (entry.type == type && entry.name == name) {
            result.push_back(&entry);
        }
    }
    return result;
}

bool ResourceModel::SetData(const ResourceId& type, const ResourceId& name, uint16_t language,
                            uint32_t codePage, std::vector<uint8_t> data) {
    ResourceEntry* existing = Find(type, name, language);
    if (existing != nullptr) {
        existing->codePage = codePage;
        existing->data = std::move(data);
        m_dirty = true;
        return true;
    }

    m_entries.push_back(ResourceEntry{type, name, language, codePage, std::move(data)});
    Sort();
    m_dirty = true;
    return false;
}

bool ResourceModel::Remove(const ResourceId& type, const ResourceId& name, uint16_t language) {
    const auto it = std::ranges::find_if(m_entries, [&](const ResourceEntry& entry) {
        return entry.type == type && entry.name == name && entry.language == language;
    });
    if (it == m_entries.end()) {
        return false;
    }
    m_entries.erase(it);
    m_dirty = true;
    return true;
}

}  // namespace peres
