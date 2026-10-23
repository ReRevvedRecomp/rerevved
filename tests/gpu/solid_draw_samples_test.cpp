#include "gpu/d3d12/native_menu_panel.h"
#include "gpu/guest_d3d/solid_draw_samples.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

using namespace rerevved::gpu;
using namespace rerevved::gpu::guest_d3d;

namespace
{

template <typename F>
void rejects(F operation, const char* message)
{
    try
    {
        operation();
    }
    catch (const std::runtime_error&)
    {
        return;
    }
    UNSCOPED_INFO(message);
    REQUIRE(false);
}

std::uint8_t value(unsigned sample, unsigned x, unsigned y, unsigned channel)
{
    return static_cast<std::uint8_t>(1 + (sample * 97 + x * 13 + y * 17 + channel * 31) % 255);
}

} // namespace

TEST_CASE("solid_draw_samples", "[ctest][solid-draw-samples]")
{
    constexpr std::size_t      planeBytes = 640 * 720 * 4;
    constexpr unsigned         base       = 17;
    std::vector<std::uint32_t> registers(kNativeMenuPanelRegisterCount);
    registers[0x2000] = 0x0A010280;
    registers[0x2001] = base;
    registers[0x2208] = 4;
    std::vector<std::uint8_t> edram(10 * 1024 * 1024), expected(2 * planeBytes);
    for (unsigned sample = 0; sample < 2; ++sample)
        for (unsigned y = 0; y < 720; ++y)
            for (unsigned x = 0; x < 640; ++x)
            {
                // Independent EDRAM fixture: samples alternate physical rows.
                const auto physicalY = y * 2 + sample;
                const auto tile      = (base + physicalY / 16 * 8 + x / 80) % 2048;
                const auto offset    = tile * 80 * 16 * 4 + (physicalY % 16 * 80 + x % 80) * 4;
                for (unsigned c = 0; c < 4; ++c)
                    edram[offset + c] = expected[sample * planeBytes + (y * 640 + x) * 4 + c] = value(sample, x, y, c);
            }
    std::vector<std::uint8_t> decoded;
    std::string               error;
    UNSCOPED_INFO("decoder must emit sample 0 then sample 1 in row-major RGBA byte order");
    REQUIRE((DecodeNativeMenuPanelColor(edram, registers, decoded, error) && decoded == expected));
    for (const unsigned tileX : { 0U, 640U })
    {
        NativeDrawReplayRecipe recipe;
        recipe.width           = 1280;
        recipe.height          = 720;
        recipe.sampleCount     = 4;
        recipe.clearColor      = {};
        recipe.vertexConstants = { 23, 41 };
        recipe.scissor         = { 1, 2, 1279, 719 };
        SeedSolidDrawReferenceDestination(recipe, decoded, tileX);
        UNSCOPED_INFO("seeding must preserve draw inputs");
        REQUIRE((recipe.vertexConstants == std::vector<std::uint8_t>{ 23, 41 } && recipe.scissor.left == 1 && recipe.scissor.right == 1279));
        const std::array<std::span<const std::uint8_t>, 2> planes{ recipe.initialSample0, recipe.initialSample1 };
        // One assertion for the 7.4M byte comparisons below.
        std::size_t planeMismatches = 0;
        for (unsigned sample = 0; sample < 2; ++sample)
            for (unsigned y = 0; y < 720; ++y)
                for (unsigned x = 0; x < 1280; ++x)
                    for (unsigned c = 0; c < 4; ++c)
                        if (planes[sample][(y * 1280 + x) * 4 + c] != (x >= tileX && x < tileX + 640 ? value(sample, x - tileX, y, c) : 0))
                            ++planeMismatches;
        UNSCOPED_INFO("planes must keep sample/channel/row order and clear the other tile");
        REQUIRE(planeMismatches == 0);
        auto full = recipe.initialSample0;
        full.insert(full.end(), recipe.initialSample1.begin(), recipe.initialSample1.end());
        UNSCOPED_INFO("seeded planes must round-trip exactly");
        REQUIRE(ExtractSolidDrawReferenceTile(full, tileX) == expected);
        const auto other = ExtractSolidDrawReferenceTile(full, 640 - tileX);
        UNSCOPED_INFO("other tile must be transparent black");
        REQUIRE(std::all_of(other.begin(), other.end(), [](auto byte)
                            {
                                return byte == 0;
                            }));
        const auto saved0 = recipe.initialSample0, saved1 = recipe.initialSample1;
        rejects([&]
                {
                    SeedSolidDrawReferenceDestination(recipe, std::span(decoded).first(decoded.size() - 1), tileX);
                },
                "reject truncated reference");
        rejects([&]
                {
                    SeedSolidDrawReferenceDestination(recipe, decoded, 1);
                },
                "reject unsupported tile offset");
        recipe.width = 640;
        rejects([&]
                {
                    SeedSolidDrawReferenceDestination(recipe, decoded, tileX);
                },
                "reject unsupported target");
        UNSCOPED_INFO("rejected seeding must preserve existing planes");
        REQUIRE((recipe.initialSample0 == saved0 && recipe.initialSample1 == saved1));
        full.pop_back();
        rejects([&]
                {
                    ExtractSolidDrawReferenceTile(full, tileX);
                },
                "reject truncated readback");
    }
}
