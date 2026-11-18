#include "place_badges.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{

using Bytes = std::vector<uint8_t>;

constexpr size_t   kHeader = 128;
constexpr uint32_t kRed    = 0xffff0000;
constexpr uint32_t kRing   = 0xffc0c0c0;

void put32(Bytes& out, size_t at, uint32_t value)
{
    for (size_t b = 0; b < 4; ++b)
    {
        out[at + b] = static_cast<uint8_t>(value >> (8 * b));
    }
}

uint32_t pixel(const Bytes& dds, int x, int y)
{
    const size_t at = kHeader + 4 * static_cast<size_t>(y * 64 + x);
    return uint32_t{ dds[at] } | (uint32_t{ dds[at + 1] } << 8) | (uint32_t{ dds[at + 2] } << 16) |
           (uint32_t{ dds[at + 3] } << 24);
}

float radius(int x, int y)
{
    return std::hypot(x - 31.5f, y - 31.5f);
}

// A 64x64 A8R8G8B8 badge: an opaque ring from radius 24 to 30 around a small red digit.
Bytes ringBadge()
{
    Bytes dds(kHeader + 64 * 64 * 4, 0);
    std::memcpy(dds.data(), "DDS ", 4);
    put32(dds, 4, 124);
    put32(dds, 8, 0x100f);
    put32(dds, 12, 64);
    put32(dds, 16, 64);
    put32(dds, 76, 32);
    put32(dds, 80, 0x41);
    put32(dds, 88, 32);
    put32(dds, 92, 0x00ff0000);
    put32(dds, 96, 0x0000ff00);
    put32(dds, 100, 0x000000ff);
    put32(dds, 104, 0xff000000);
    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            const float r     = radius(x, y);
            uint32_t    value = 0;
            if (r >= 24 && r <= 30)
            {
                value = kRing;
            }
            else if (r < 5)
            {
                value = kRed;
            }
            put32(dds, kHeader + 4 * static_cast<size_t>(y * 64 + x), value);
        }
    }
    return dds;
}

} // namespace

TEST_CASE("place_badges_find_badge", "[ctest][place-badges]")
{
    using rerevved::place_badges::FindBadge;
    CHECK(FindBadge("Ribbon6.dds") == 6);
    CHECK(FindBadge("ui\\sub/RIBBON7.DDS") == 7);
    CHECK(!FindBadge("Ribbon5.dds"));
    CHECK(!FindBadge("Ribbon8.dds"));
    CHECK(!FindBadge("Ribbon6.dds.bak"));
    CHECK(!FindBadge("ui/"));
}

TEST_CASE("place_badges_draws_digit_inside_ring", "[ctest][place-badges]")
{
    const Bytes source = ringBadge();
    const Bytes six    = rerevved::place_badges::BuildBadge(source, 6);
    const Bytes seven  = rerevved::place_badges::BuildBadge(source, 7);
    REQUIRE(six.size() == source.size());
    REQUIRE(seven.size() == source.size());
    CHECK(std::equal(source.begin(), source.begin() + kHeader, six.begin()));
    CHECK(six != seven);

    for (const Bytes* badge : { &six, &seven })
    {
        int white = 0;
        for (int y = 0; y < 64; ++y)
        {
            for (int x = 0; x < 64; ++x)
            {
                const float r = radius(x, y);
                if (r >= 24.5f && r <= 29.5f)
                {
                    CHECK(pixel(*badge, x, y) == kRing);
                }
                if (r < 23)
                {
                    CHECK(pixel(*badge, x, y) != kRed);
                    const uint32_t value = pixel(*badge, x, y);
                    white += (value >> 24) == 0xff && (value & 0xff) == 0xff ? 1 : 0;
                }
            }
        }
        UNSCOPED_INFO("the digit has a white core");
        CHECK(white > 10);
    }
}

TEST_CASE("place_badges_refuses_other_sources", "[ctest][place-badges]")
{
    using rerevved::place_badges::BuildBadge;
    const Bytes source = ringBadge();
    CHECK(BuildBadge(source, 5).empty());
    CHECK(BuildBadge(source, 8).empty());
    CHECK(BuildBadge(Bytes(source.begin(), source.end() - 1), 6).empty());

    Bytes compressed = source;
    std::memcpy(compressed.data() + 84, "DXT5", 4);
    CHECK(BuildBadge(compressed, 6).empty());

    Bytes noRing = source;
    std::fill(noRing.begin() + kHeader, noRing.end(), uint8_t{ 0 });
    CHECK(BuildBadge(noRing, 6).empty());
}
