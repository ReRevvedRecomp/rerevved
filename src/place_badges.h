#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace rerevved::place_badges
{

// The win/lose screen has badge art for places 1-5. Places 6 and 7 reuse the
// ring of the player's own place 5 badge with a digit drawn here.
inline constexpr std::string_view kSourceImage = "Ribbon5.dds";
inline constexpr int              kFirstPlace  = 6;
inline constexpr int              kLastPlace   = 7;

// "Ribbon6.dds" and "Ribbon7.dds" name places 6 and 7, ignoring case and any path.
std::optional<int> FindBadge(std::string_view fileName);

// Same header and pixel format as the 64x64 uncompressed 32-bit source; empty
// when the source is not in that form or its ring cannot be found.
std::vector<uint8_t> BuildBadge(std::span<const uint8_t> sourceDds, int place);

} // namespace rerevved::place_badges
