#include "scene_draw_coverage.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace rerevved::gpu::guest_d3d
{

namespace
{

// Full-target P20, K15/K16, K17(DE0359DB, 7C1DD533), K20(38691F81, F95D105E),
// late 66B6CC03 pairs: VS oPos=dot((attribute 0.xyz, 1), c0..c3)+half-pixel; PS
// kills only by alpha test.
constexpr std::array<std::pair<std::uint64_t, std::uint64_t>, 8> kQuadPrograms{ {
    { 0x3418D1CAA13CAF50ULL, 0x1262103F2F06927FULL },
    { 0x410718F3B9741E31ULL, 0x323C72B4BF8A37B7ULL },
    { 0xE88067C94CBD1FFCULL, 0xF36CCEFF549A2660ULL },
    { 0xDE0359DBD47D7FABULL, 0x560C753DAD5555ADULL },
    { 0x7C1DD533708BB131ULL, 0x818E4EAF9BA41B33ULL },
    { 0x38691F81BEA4AD28ULL, 0xB8C1B55C7CE2E502ULL },
    { 0xF95D105E51CF1A19ULL, 0x884C15F5AE654F89ULL },
    { 0x66B6CC03F44ED9B7ULL, 0x645E162E54094FF6ULL },
} };

// The fog composite's pixel shader (caller 0x8230686C).
constexpr std::uint64_t kCompositePixelHash = 0xF6CF41BEA360546BULL;

std::uint32_t littleWord(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) | (std::uint32_t(bytes[offset + 2]) << 16) |
           (std::uint32_t(bytes[offset + 3]) << 24);
}

float littleFloat(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return std::bit_cast<float>(littleWord(bytes, offset));
}

} // namespace

namespace
{

// Input points are snapped render-target coordinates of six indices splitting
// an axis-aligned rectangle. Return whole pixel squares, or centers strictly
// inside when centers selects single-sample coverage.
std::optional<NativeDrawReplayScissor> splitRectangle(const NativeDrawReplayRecipe& draw, const std::array<std::array<float, 2>, 6>& points, bool centers);

// Host render-target coordinates of each index, as the vertex shader and
// viewport place them: `clipOf` maps attribute 0 to a clip position. Empty
// when an index lies outside the vertices or the position would be clipped.
template <typename ClipOf>
std::optional<std::array<std::array<float, 2>, 6>> quadPoints(const NativeDrawReplayRecipe& draw, ClipOf clipOf)
{
    const auto vertices = draw.vertexBytes();
    const auto shared   = std::span<const std::uint8_t>(draw.sharedConstants);
    // Shared constants 288 and 324 are g_SwappedTexcoords and g_SintTexcoords,
    // whose bit 0 would reinterpret the position; 292 is g_HalfPixelOffset.
    const auto indices = draw.indexSpan();
    if (indices.size() != 6 || shared.size() < 328 || (littleWord(shared, 288) & 1) || (littleWord(shared, 324) & 1) || !(draw.viewport.width > 0) ||
        !(draw.viewport.height > 0) || shared.size() > kNativeGameplayUserClipPlaneOffset) // a user clip plane could cut the quad
        return std::nullopt;
    const float                         halfX = littleFloat(shared, 292), halfY = littleFloat(shared, 296);
    std::array<std::array<float, 2>, 6> points{};
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        const auto offset = std::size_t(indices[i]) * draw.vertexStrideBytes;
        if (offset + 12 > vertices.size())
            return std::nullopt;
        const auto clip = clipOf(std::array<float, 4>{ littleFloat(vertices, offset), littleFloat(vertices, offset + 4), littleFloat(vertices, offset + 8), 1.0F });
        // Clipping would cut the quad; without depth clip the depth clamps.
        if (!(clip[3] > 0) || !std::isfinite(clip[0]) || !std::isfinite(clip[1]) || !std::isfinite(clip[3]) ||
            (draw.rasterizer.depthClip && !(clip[2] >= 0 && clip[2] <= clip[3])))
            return std::nullopt;
        const auto x = (clip[0] + halfX * clip[3]) / clip[3], y = (clip[1] + halfY * clip[3]) / clip[3];
        // The rasterizer snaps vertices to 1/256 pixel, rounding to nearest.
        const auto snap = [](float value)
        {
            return std::nearbyint(value * 256.0F) / 256.0F;
        };
        points[i] = { snap(draw.viewport.x + (x + 1) * 0.5F * draw.viewport.width), snap(draw.viewport.y + (1 - y) * 0.5F * draw.viewport.height) };
    }
    return points;
}

} // namespace

std::optional<NativeDrawReplayScissor> SceneQuadCoverage(const NativeDrawReplayRecipe& draw)
{
    if (std::ranges::find(kQuadPrograms, std::pair{ draw.vertexShaderHash, draw.pixelShaderHash }) == kQuadPrograms.end())
        return std::nullopt;
    const auto constants = std::span<const std::uint8_t>(draw.vertexConstants);
    if (draw.vertexAttributeCount != 2 || draw.vertexStrideBytes != 32 || constants.size() < 64)
        return std::nullopt;
    const auto points = quadPoints(draw, [&](const std::array<float, 4>& position)
                                   {
                                       std::array<float, 4> clip{};
                                       for (std::size_t row = 0; row < 4; ++row)
                                           for (std::size_t column = 0; column < 4; ++column)
                                               clip[row] += position[column] * littleFloat(constants, row * 16 + column * 4);
                                       return clip;
                                   });
    return points ? splitRectangle(draw, *points, false) : std::nullopt;
}

std::optional<NativeDrawReplayScissor> SceneCompositeCoverage(const NativeDrawReplayRecipe& draw)
{
    if (draw.vertexShaderHash != kSceneCompositeVertexHash || draw.pixelShaderHash != kCompositePixelHash || draw.sampleCount != 1 || !draw.vertexAttributeCount ||
        draw.vertexStrideBytes != draw.vertexAttributeCount * 16)
        return std::nullopt;
    const auto points = quadPoints(draw, [](const std::array<float, 4>& position)
                                   {
                                       return position;
                                   });
    return points ? splitRectangle(draw, *points, true) : std::nullopt;
}

namespace
{

std::optional<NativeDrawReplayScissor> splitRectangle(const NativeDrawReplayRecipe& draw, const std::array<std::array<float, 2>, 6>& points, bool centers)
{
    // Every index lies on a corner of one axis-aligned rectangle, and each
    // triangle omits one corner, the two omitted corners being opposite: the
    // triangles then split the rectangle along the other diagonal.
    float minX = points[0][0], maxX = minX, minY = points[0][1], maxY = minY;
    for (const auto& point : points)
    {
        minX = std::min(minX, point[0]);
        maxX = std::max(maxX, point[0]);
        minY = std::min(minY, point[1]);
        maxY = std::max(maxY, point[1]);
    }
    if (!(minX < maxX) || !(minY < maxY))
        return std::nullopt;
    std::array<unsigned, 2> omitted{};
    for (std::size_t triangle = 0; triangle < 2; ++triangle)
    {
        unsigned                            corners = 0;
        std::array<std::array<float, 2>, 3> at{};
        for (std::size_t vertex = 0; vertex < 3; ++vertex)
        {
            const auto& point = points[triangle * 3 + vertex];
            if ((point[0] != minX && point[0] != maxX) || (point[1] != minY && point[1] != maxY))
                return std::nullopt;
            corners |= 1U << ((point[0] == maxX ? 1U : 0U) | (point[1] == maxY ? 2U : 0U));
            at[vertex] = point;
        }
        if (std::popcount(corners) != 3)
            return std::nullopt;
        omitted[triangle] = static_cast<unsigned>(std::countr_zero(~corners & 0xFU));
        // With Y down, a positive cross product turns clockwise on screen,
        // which D3D calls front unless FrontCounterClockwise is set.
        const auto cross     = (at[1][0] - at[0][0]) * (at[2][1] - at[0][1]) - (at[1][1] - at[0][1]) * (at[2][0] - at[0][0]);
        const bool clockwise = cross > 0;
        const bool front     = draw.rasterizer.frontCounterClockwise ? !clockwise : clockwise;
        if ((draw.rasterizer.cull == 1 && front) || (draw.rasterizer.cull == 2 && !front))
            return std::nullopt;
    }
    if ((omitted[0] ^ omitted[1]) != 3)
        return std::nullopt;

    // A pixel counts only when its whole square lies inside the viewport
    // scissor under the top-left rule.
    const auto bound = [](float value)
    {
        return std::clamp(value, float(std::numeric_limits<std::int32_t>::min() / 2), float(std::numeric_limits<std::int32_t>::max() / 2));
    };
    NativeDrawReplayScissor rect{ std::int32_t(bound(std::ceil(std::max(minX, draw.viewport.x)))),
                                  std::int32_t(bound(std::ceil(std::max(minY, draw.viewport.y)))),
                                  std::int32_t(bound(std::floor(std::min(maxX, draw.viewport.x + draw.viewport.width)))),
                                  std::int32_t(bound(std::floor(std::min(maxY, draw.viewport.y + draw.viewport.height)))) };
    if (centers)
    {
        // Pixel i's center i + 0.5 lies strictly between the edges; an
        // exact edge counts as uncovered whatever the fill rule.
        rect.left   = std::max(std::int32_t(bound(std::ceil(draw.viewport.x))), std::int32_t(bound(std::floor(minX - 0.5F) + 1)));
        rect.top    = std::max(std::int32_t(bound(std::ceil(draw.viewport.y))), std::int32_t(bound(std::floor(minY - 0.5F) + 1)));
        rect.right  = std::min(std::int32_t(bound(std::floor(draw.viewport.x + draw.viewport.width))), std::int32_t(bound(std::ceil(maxX - 0.5F))));
        rect.bottom = std::min(std::int32_t(bound(std::floor(draw.viewport.y + draw.viewport.height))), std::int32_t(bound(std::ceil(maxY - 0.5F))));
    }
    if (rect.right <= rect.left || rect.bottom <= rect.top)
        return std::nullopt;
    return rect;
}

} // namespace

NativeFrameDrawCoverage MakeSceneDrawCoverage(ReadMemory read)
{
    return [read = std::move(read)](const NativeDrawSubmission& submission, const NativeDrawReplayRecipe& draw) -> std::optional<NativeFrameDrawCover>
    {
        const auto covered   = SceneQuadCoverage(draw);
        const auto composite = covered ? std::nullopt : SceneCompositeCoverage(draw);
        if (!covered && !composite)
            return std::nullopt;
        const auto guest = [&](std::uint32_t offset, std::uint32_t& out)
        {
            const auto bytes = read(std::uint64_t(submission.Graphics()) + offset, 4, false);
            if (bytes.size() != 4)
                return false;
            out = (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) | bytes[3];
            return true;
        };
        // The scene family's admitted RB_COLORCONTROL form with ALPHA_TEST_ENABLE
        // (bit 3) and ALPHA_TO_MASK_ENABLE (bit 4) clear; ALPHA_FUNC is inert.
        std::uint32_t control = 0;
        if (!guest(0x293C, control) || (control & ~7U) != 0x87000000U)
            return std::nullopt;
        if (covered)
            return *covered;
        // PS F6CF41BE:oC0.w=(c255.x-tf 2.w)*k+tf2.w, k in[0, 1]. Alpha is1 only
        // with c252..c255 literal c255.x=1 and fetch 2 swizzle W=1; pinned
        // harvested blend/mask/depth/mode/window state must also hold.
        static constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 8> kState{ {
            { 0x2938, 0x07060706 }, // RB_BLENDCONTROL0
            { 0x28DC, 0xF },        // RB_COLOR_MASK
            { 0x2934, 0x24F00730 }, // RB_DEPTHCONTROL
            { 0x2954, 4 },          // RB_MODECONTROL (0x2208)
            { 0x30A8, 0 },          // bound depth surface
            { 0x28C0, 0 },          // PA_SC_WINDOW_OFFSET
            { 0x28C4, 0 },          // window scissor top left
            { 0x28C8, 0x02D00500 }, // window scissor bottom right, 1280x720
        } };
        for (const auto [offset, value] : kState)
            if (std::uint32_t word = 0; !guest(offset, word) || word != value)
                return std::nullopt;
        // Fetch 2 (G+0x480 + 2 * 24) dword 3 bits 10..12: the swizzle's W
        // selector, 5 for constant 1.
        std::uint32_t swizzle = 0;
        if (!guest(0x480 + 2 * 24 + 12, swizzle) || ((swizzle >> 10) & 7) != 5)
            return std::nullopt;
        // The recipe's pixel constants carry the literal records over the
        // shadow; c255.x is bytes 4080..4083.
        const auto constants = std::span<const std::uint8_t>(draw.pixelConstants);
        if (constants.size() < 256 * 16 || littleWord(constants, 255 * 16) != 0x3F800000U || draw.width != 1280 || draw.height != 720 ||
            draw.targetFormat != NativeDrawReplayTargetFormat::Rgba8)
            return std::nullopt;
        return NativeFrameDrawCover{ *composite, true };
    };
}

} // namespace rerevved::gpu::guest_d3d
