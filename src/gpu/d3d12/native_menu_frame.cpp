#include "native_menu_frame.h"

#include <algorithm>
#include <bit>

namespace rerevved::gpu
{
namespace
{

bool fail(std::string& error, const std::string& reason)
{
    error = reason;
    return false;
}

bool resolveState(const NativeMenuFrameEventView& event, std::size_t half, std::uint32_t destination, std::uint8_t clearAlpha, std::string& error)
{
    const auto                                    r          = event.draw.registers;
    const std::pair<std::uint32_t, std::uint32_t> expected[] = {
        { 0x2000, 0x0A010280 },
        { 0x2001, 0 },
        { 0x2002, 0x530 },
        { 0x200E, 0 },
        { 0x200F, 0x20002000 },
        { 0x2080, half ? 0x7D80U : 0U },
        { 0x2081, half ? 640U : 0U },
        { 0x2082, half ? 0x02D00500U : 0x02D00280U },
        { 0x2205, 0x10000 },
        { 0x2208, 6 },
        { 0x2302, 4 },
        { 0x2318, half ? 0x100040U : 0x100340U },
        { 0x2319, destination },
        { 0x231A, 0x02D00500 },
        { 0x231B, 0x01000300 },
        { 0x231C, 0xFFF },
        { 0x231D, 0xFFFFFF00 },
        { 0x231E, std::uint32_t(clearAlpha) << 24 },
        { 0x231F, std::uint32_t(clearAlpha) << 24 },
    };
    for (const auto [index, value] : expected)
        if (r[index] != value)
            return fail(error, "unsupported half-frame resolve register " + std::to_string(index));
    if (!event.resolve || event.draw.vertexBytes.size() != 24 ||
        (r[0x4800] & 3) != 3 || (r[0x4801] & 3) != 2 ||
        (r[0x4800] & ~3U) != event.draw.vertexGuestBase || ((r[0x4801] >> 2) & 0xFFFFFF) * 4 != 24)
        return fail(error, "half-frame resolve is missing its original rectangle bytes");
    const float left          = float(half * 640) - 0.5F;
    const float right         = left + 640;
    const float coordinates[] = { left, -0.5F, right, -0.5F, right, 719.5F };
    for (std::size_t i = 0; i < 6; ++i)
    {
        const auto* b    = event.draw.vertexBytes.data() + i * 4;
        const auto  word = (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) |
                           (std::uint32_t(b[2]) << 8) | b[3];
        if (word != std::bit_cast<std::uint32_t>(coordinates[i]))
            return fail(error, "unsupported half-frame resolve rectangle");
    }
    return true;
}

} // namespace

bool BuildNativeMenuFrameRecipe(std::span<const NativeMenuFrameEventView> events,
                                const NativeMenuShaders&                  shaders,
                                std::span<const std::uint32_t>            gamma,
                                NativeFrameReplayRecipe&                  recipe,
                                std::string&                              error)
{
    recipe = {};
    error.clear();
    if (events.empty() || events.size() > 1024 || gamma.size() != 256)
        return fail(error, "incomplete or oversized native menu frame");
    std::copy(gamma.begin(), gamma.end(), recipe.gammaTable.begin());
    bool                            initialized    = false;
    bool                            depthDiscarded = false;
    bool                            uiSeen         = false;
    bool                            swapped        = false;
    std::size_t                     half           = 0;
    std::uint32_t                   frontbuffer    = 0;
    std::uint8_t                    clearAlpha     = 0;
    const NativeMenuFrameEventView* pendingCopy    = nullptr;
    std::uint64_t                   previousId     = 0;
    for (const auto& event : events)
    {
        if (event.id <= previousId || !event.success || swapped || event.draw.registers.size() != 0x5003)
            return fail(error, "invalid menu event ordering, completion or register snapshot");
        previousId   = event.id;
        const auto r = event.draw.registers;
        if (pendingCopy && event.kind != NativeMenuFrameEventView::Kind::Copy)
            return fail(error, "resolve trigger was not followed by its copy event");
        if (event.kind == NativeMenuFrameEventView::Kind::Swap)
        {
            if (&event == events.data() && !initialized && half == 0 &&
                event.frontbufferWidth == 1280 && event.frontbufferHeight == 720)
                continue;
            if (half != 2 || event.frontbuffer != frontbuffer || event.frontbufferWidth != 1280 || event.frontbufferHeight != 720)
                return fail(error, "swap does not present the two completed native menu halves");
            swapped = true;
            continue;
        }
        if (event.kind == NativeMenuFrameEventView::Kind::Copy)
        {
            if (!pendingCopy || half >= 2 || !uiSeen || recipe.halves[half].empty() ||
                !std::equal(r.begin(), r.end(), pendingCopy->draw.registers.begin()))
                return fail(error, "unpaired or premature menu resolve");
            if (!half)
            {
                frontbuffer                    = r[0x2319];
                constexpr auto tiledFrameBytes = 1280U * ((720U + 31U) & ~31U) * 4U;
                if (!frontbuffer || frontbuffer > 0x20000000U - tiledFrameBytes || (frontbuffer & 0xFFF))
                    return fail(error, "invalid menu resolve destination range");
            }
            if (!resolveState(event, half, frontbuffer + (half ? 0x14000U : 0U), clearAlpha, error))
                return false;
            ++half;
            pendingCopy    = nullptr;
            depthDiscarded = uiSeen = false;
            continue;
        }
        if (r[0x2208] == 6 && !event.hostIssued && event.primitiveType == 8)
        {
            pendingCopy = &event;
            continue;
        }
        if (!event.hostIssued)
            return fail(error, "unsupported skipped menu draw");
        if (event.primitiveType == 1 && event.hostPixelShaderHash == 0 &&
            r[0x2104] == 0 && r[0x2200] == 0 && r[0x2208] == 4)
            continue;
        if (event.primitiveType == 8 && r[0x2208] == 5 && initialized && half < 2)
        {
            // Later draws in this half must not consume depth discarded here.
            depthDiscarded = true;
            continue;
        }
        if (event.primitiveType == 8 && !initialized && !half)
        {
            if (!ValidateNativeMenuInitialClear(event.draw, clearAlpha, error))
                return false;
            initialized = true;
            continue;
        }
        if ((event.primitiveType != 4 && event.primitiveType != 6) || !initialized || half >= 2 ||
            event.draw.primitive != event.primitiveType)
            return fail(error, "unexpected image-producing operation in native menu frame");
        NativeDrawReplayRecipe draw;
        if (!BuildNativeMenuDrawRecipe(event.draw, shaders, draw, error))
            return fail(error, "menu event " + std::to_string(event.id) + ": " + error);
        if (recipe.halves[half].empty())
            draw.clearColor[3] = clearAlpha;
        if (draw.viewport.x != (half ? -640.0F : 0.0F) ||
            (draw.depth.enabled && (depthDiscarded || uiSeen)))
            return fail(error, "menu draw crosses its resolve or discarded depth boundary");
        uiSeen |= !draw.depth.enabled;
        recipe.halves[half].push_back(std::move(draw));
    }
    if (!swapped || pendingCopy)
        return fail(error, "native menu frame has no completed final swap");
    return ValidateNativeFrameReplayRecipe(recipe, error);
}

} // namespace rerevved::gpu
