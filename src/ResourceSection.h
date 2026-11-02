#pragma once

#include "ResourceModel.h"
#include "Result.h"

#include <cstdint>
#include <vector>

namespace peres {

// Serializes the complete IMAGE_RESOURCE_DIRECTORY tree into a section blob.
// Data entries are written with absolute RVAs based on |sectionRva|.
Result<std::vector<uint8_t>> BuildResourceSection(const ResourceModel& model, uint32_t sectionRva);

}  // namespace peres
