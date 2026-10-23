#include "native_gameplay_clear.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <span>
#include <string_view>

#include <rex/crypto/sha256.h>

namespace rerevved::gpu
{
namespace
{

std::string wordDigest(std::span<const std::uint32_t> words)
{
    std::string bytes(words.size_bytes(), '\0');
    for (std::size_t index = 0; index < words.size(); ++index)
        for (unsigned byte = 0; byte < 4; ++byte)
            bytes[index * 4 + byte] = static_cast<char>(words[index] >> (byte * 8));
    return rex::crypto::sha256(bytes);
}

float bigFloat(const std::uint8_t* bytes)
{
    return std::bit_cast<float>((std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
                                (std::uint32_t(bytes[2]) << 8) | bytes[3]);
}

} // namespace

bool BuildNativeGameplayClearPass(const rex::graphics::diagnostic::DrawCaptureSnapshot& draw,
                                  NativeRenderFramePass&                                pass,
                                  std::string&                                          error)
{
    return BuildNativeGameplayClearPass(draw, draw.registers, pass, error);
}

bool BuildNativeGameplayClearPass(const rex::graphics::diagnostic::DrawCaptureSnapshot& draw,
                                  std::span<const std::uint32_t>                        registers,
                                  NativeRenderFramePass&                                pass,
                                  std::string&                                          error)
{
    pass = {};
    error.clear();
    const auto fail = [&](const char* message)
    {
        error = message;
        return false;
    };
    // These shaders copy screen XYZ and a constant per-rectangle RGBA value.
    // Verify bytes as well as identity before replacing rasterization by clears.
    if (registers.size() != 0x5003 || draw.vertex_ucode.size() != 27 || draw.pixel_ucode.size() != 9 ||
        wordDigest(draw.vertex_ucode) != "2b2fe8f96319434015034dc955df9b2ab30d0e979645821cb592e667c4f58295" ||
        wordDigest(draw.pixel_ucode) != "f73f655ea80c22bde4bc93575f87664e81056b6f4714bef3a4defaa18994d18a")
        return fail("gameplay clear does not match the pinned rectangle shader bytes");
    const auto r = registers;
    if (draw.primitive_type != 8 || draw.index.present || !draw.requested_index_count ||
        draw.requested_index_count % 3 || draw.requested_index_count > 4096 ||
        r[0x2204] != 0x10000 || r[0x2205] != 0x10000 || r[0x2206] != 0x300 ||
        (r[0x2208] != 4 && r[0x2208] != 5) ||
        (r[0x2200] != 0x8777 && r[0x2200] != 0x8701) || r[0x210D] != 0xFF0000 ||
        r[0x2080] != 0 || r[0x2081] != 0 || r[0x2082] != 0x20002000 ||
        r[0x200E] != 0 || r[0x200F] != 0x20002000 || r[0x2102] != 0 ||
        (r[0x2002] & 0xFFFFF800U) != 0)
        return fail("gameplay clear has unsupported rectangle, clipping or depth/stencil state");
    // A clear writes covered samples only after SDK raster coverage and
    // unchanged shader color pass.
    if (((r[0x2180] >> 24) & 7U) != 0)
        return fail("clear_vertex_export_mode");
    if ((r[0x2293] & 0x81U) == 0x81U)
        return fail("clear_viz_query_kill");
    if ((r[0x2284] & 3U) == 1U && ((r[0x21FC] >> 8) & 3U) != 0)
        return fail("clear_tessellation");
    // The SDK VS clamps each auto-generated vertex index to VGT_MIN/MAX_VTX_INDX
    // (dxbc_translator.cpp RemapAndConvertVertexIndices); a clamp moves vertices.
    if ((r[0x2101] & 0xFFFFFFU) != 0 || (r[0x2100] & 0xFFFFFFU) < draw.requested_index_count - 1)
        return fail("clear_vertex_index_clamped");
    if (r[0x2208] == 4)
    {
        // SQ_PROGRAM_CNTL.param_gen overwrites the interpolator at
        // SQ_CONTEXT_MISC.param_gen_pos when it is below the PS interpolator
        // count (at most 16), replacing the color the pinned PS exports.
        if ((r[0x2180] & (1U << 18)) != 0 && ((r[0x2181] >> 8) & 0xFFU) < 16)
            return fail("clear_param_gen");
        if ((r[0x2202] & 8U) != 0 && (r[0x2202] & 7U) != 7U)
            return fail("clear_alpha_test");
        if ((r[0x2202] & 0x10U) != 0)
            return fail("clear_alpha_to_mask");
        // RB_BLENDCONTROL bits the SDK pipeline cache reads: ONE, ZERO, ADD.
        if ((r[0x2201] & 0x1FFF1FFFU) != 0x00010001U)
            return fail("clear_blend_not_replace");
    }
    if (draw.vertex_fetches.size() != 1 || draw.vertex_fetches.front().fetch_constant != 0)
        return fail("gameplay clear requires its single position/color vertex stream");
    const auto& fetch = draw.vertex_fetches.front();
    if (fetch.Payload().size() != std::size_t(draw.requested_index_count) * 28 ||
        fetch.availability != rex::graphics::diagnostic::DrawCaptureAvailability::kComplete ||
        (r[0x4800] & 3U) != 3 || (r[0x4800] & ~3U) != fetch.base ||
        (r[0x4801] & 3U) != 2 || ((r[0x4801] >> 2) & 0xFFFFFFU) * 4ULL != fetch.Payload().size())
        return fail("gameplay clear geometry does not match its fetch descriptor");

    unsigned scaleX = 1, scaleY = 1;
    bool     depthAliasColor = false;
    switch (r[0x2000])
    {
        case 0x00800050:
            // The captured 32x32 resolve uses an 80-pixel color pitch.
            pass.target = { 80, 32, 1, NativeDrawReplayTargetFormat::Rgba8 };
            break;
        case 0x020000A0:
            pass.target = { 160, 32, 1, NativeDrawReplayTargetFormat::Rgba8 };
            break;
        case 0x01020050:
            // The 128x32 resolve uses a 160-sample pitch in both clear views.
            pass.target = { 160, 32, 1, NativeDrawReplayTargetFormat::Rgba8 };
            scaleX = scaleY = 2;
            break;
        case 0x04000140:
            pass.target = { 320, 32, 1, NativeDrawReplayTargetFormat::Rgba8 };
            break;
        case 0x020200A0:
            // The 256x32 resolve uses a 320-sample pitch in both clear views.
            pass.target = { 320, 32, 1, NativeDrawReplayTargetFormat::Rgba8 };
            scaleX = scaleY = 2;
            break;
        case 0x08000230:
            pass.target = { 560, 512, 1, NativeDrawReplayTargetFormat::Rgba8 };
            break;
        case 0x04020118:
            pass.target = { 560, 512, 1, NativeDrawReplayTargetFormat::Rgba8 };
            scaleX = scaleY = 2;
            break;
        case 0x10000410:
            pass.target = { 1040, 1024, 1, NativeDrawReplayTargetFormat::Rgba8 };
            break;
        case 0x08020208:
            pass.target = { 1040, 1024, 1, NativeDrawReplayTargetFormat::Rgba8 };
            scaleX = scaleY = 2;
            break;
        case 0x05020140:
            scaleX = 2;
            [[fallthrough]];
        case 0x0A010280:
            pass.target = { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 };
            break;
        case 0x0A020280:
            pass.target = { 1280, 720, 1, NativeDrawReplayTargetFormat::Rg16Snorm };
            scaleX = scaleY = 2;
            depthAliasColor = true;
            break;
        default:
            return fail("gameplay clear uses an unobserved EDRAM sample/pitch alias");
    }
    const auto colorFormat = (r[0x2001] >> 16) & 0xFU;
    if ((depthAliasColor && (colorFormat != 4 || r[0x2208] != 5 || r[0x2200] != 0x8777 ||
                             (r[0x2001] & 0x7FFU) != (r[0x2002] & 0x7FFU))) ||
        (!depthAliasColor && (r[0x2001] & 0xFFFFF800U) != 0) ||
        (r[0x2208] == 4 && (r[0x2104] & 0xFU) != 0xFU))
        return fail("gameplay clear color target or depth alias is unsupported");
    // draw.cpp GetHostViewportInfo: only the D3D zero pixel center adds the
    // half-pixel offset; the OpenGL half center keeps vertex XY unshifted.
    const float centerOffset = (r[0x2302] & 1U) == 0 ? 0.5F : 0.0F;
    for (std::uint32_t first = 0; first < draw.requested_index_count; first += 3)
    {
        std::array<std::array<float, 7>, 3> vertices;
        for (unsigned vertex = 0; vertex < 3; ++vertex)
            for (unsigned component = 0; component < 7; ++component)
            {
                vertices[vertex][component] = bigFloat(fetch.Payload().data() + (first + vertex) * 28 + component * 4);
                if (!std::isfinite(vertices[vertex][component]))
                    return fail("gameplay clear contains non-finite geometry or color");
            }
        const auto& a = vertices[0];
        const auto& b = vertices[1];
        const auto& c = vertices[2];
        if (a[1] != b[1] || b[0] != c[0] || a[0] >= b[0] || a[1] >= c[1] ||
            !std::equal(a.begin() + 2, a.end(), b.begin() + 2) ||
            !std::equal(a.begin() + 2, a.end(), c.begin() + 2))
            return fail("gameplay clear requires an axis-aligned rectangle with constant depth/color");
        std::array<float, 4> edges{ (a[0] + centerOffset) * scaleX, (a[1] + centerOffset) * scaleY, (b[0] + centerOffset) * scaleX, (c[1] + centerOffset) * scaleY };
        for (const auto edge : edges)
            if (edge < 0 || edge > 4096 || std::floor(edge) != edge)
                return fail("gameplay clear rectangle does not cover whole native pixels");
        NativeRenderFramePass::Clear clear;
        clear.rectangle = { static_cast<std::int32_t>(edges[0]), static_cast<std::int32_t>(edges[1]), static_cast<std::int32_t>(edges[2]), static_cast<std::int32_t>(edges[3]) };
        if (edges[2] > pass.target.width || edges[3] > pass.target.height)
            return fail("gameplay clear rectangle exceeds its normalized attachment");
        clear.color      = r[0x2208] == 4;
        clear.depth      = (r[0x2200] & 6U) == 6U;
        clear.stencil    = true;
        clear.depthValue = std::clamp(a[2], 0.0F, 1.0F);
        std::copy(a.begin() + 3, a.end(), clear.colorValue.begin());
        if (depthAliasColor)
        {
            // D24S8 stores depth in bits 8..31. A full EDRAM alias clear
            // reinterprets those bits as the two signed fixed-point channels.
            if (edges != std::array<float, 4>{ 0, 0, 1280, 720 })
                return fail("partial depth-to-color clear aliases are unsupported");
            const auto packed = static_cast<std::uint32_t>(std::floor(double(clear.depthValue) * 0xFFFFFFU + 0.5)) << 8;
            clear.colorValue  = { std::max(-1.0F, float(std::bit_cast<std::int16_t>(std::uint16_t(packed))) / 32767.0F),
                                  std::max(-1.0F, float(std::bit_cast<std::int16_t>(std::uint16_t(packed >> 16))) / 32767.0F),
                                  0,
                                  0 };
            clear.color       = true;
            clear.depth = clear.stencil = false;
        }
        pass.clears.push_back(clear);
    }
    return true;
}

} // namespace rerevved::gpu
