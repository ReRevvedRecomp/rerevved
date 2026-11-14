#include "release_id.h"

#include <array>

namespace rerevved
{

namespace
{

// Every row has the same executable image; only header fields differ.
constexpr std::array kReleases = {
    Release{ "360-region-free", "Sid Meier's Civilization Revolution (Xbox 360, region-free)", "b59b8957a3ed9dd90e9296c96d5c7ab1b16078d3f08b015582714a06c7d6a7bd", 0x7DC1293Bu, 0x00000002u, 0xFFFFFFFFu },
    Release{ "360-usa-europe", "Sid Meier's Civilization Revolution (Xbox 360, USA, Europe)", "2a0c38091926ab41f459526dbdb26c455ff1f415aff1173762182779279e446b", 0x479CC0CCu, 0x00000001u, 0x00FF00FFu },
};

} // namespace

std::span<const Release> Releases()
{
    return kReleases;
}

const Release* FindRelease(std::string_view fileSha256)
{
    for (const auto& release : kReleases)
    {
        if (fileSha256 == release.fileSha256)
        {
            return &release;
        }
    }
    return nullptr;
}

} // namespace rerevved
