#pragma once

#include <asset_file_overrides.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace rerevved::asset_file_overrides
{

using Payload = std::shared_ptr<const std::vector<uint8_t>>;

bool TryGetPayload(std::string_view path, Payload& payload);
void ResetForTests();

} // namespace rerevved::asset_file_overrides
