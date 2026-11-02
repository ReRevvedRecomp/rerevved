#pragma once

#include <cstdint>

namespace rerevved::version_label
{

// Narrow "v1.30" literal that main-menu initializer 0x82E516F0 passes to SetVersion.
constexpr uint32_t kRetailVersionLiteral = 0x82139CA8;

} // namespace rerevved::version_label
