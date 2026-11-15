#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace rerevved
{

struct Release
{
    const char*   id;
    const char*   name;
    const char*   fileSha256;
    std::uint32_t mediaId;
    std::uint32_t version;
    std::uint32_t region;
};

// The first release is the one the guest image is built from.
std::span<const Release> Releases();

const Release* FindRelease(std::string_view fileSha256);

} // namespace rerevved
