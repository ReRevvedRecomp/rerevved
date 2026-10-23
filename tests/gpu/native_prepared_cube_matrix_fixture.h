#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "native_prepared_cube_matrix_forms.h"
#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::testing
{

inline constexpr std::uint32_t kNativePreparedCubeMatrixOutputWidth   = 1280U;
inline constexpr std::uint32_t kNativePreparedCubeMatrixOutputHeight  = 720U;
inline constexpr std::uint32_t kNativePreparedCubeMatrixVertexStride  = 56U;
inline constexpr std::uint32_t kNativePreparedCubeMatrixRegisterCount = 0x5003U;

struct NativePreparedCubeMatrixTexture
{
    std::uint32_t                width         = kNativePreparedCubeMatrixWidth;
    std::uint32_t                height        = kNativePreparedCubeMatrixHeight;
    std::uint32_t                pitch         = cubeMatrixForm->pitch;
    std::uint32_t                format        = 0;
    std::uint32_t                endianness    = 0;
    std::uint32_t                swizzle       = 0;
    std::uint32_t                dimension     = 0;
    bool                         tiled         = false;
    bool                         packed_mips   = false;
    std::uint32_t                mip_min_level = 0;
    std::uint32_t                mip_max_level = kNativePreparedCubeMatrixMipCount - 1U;
    std::array<std::uint32_t, 6> fetch_words{};
    std::vector<std::uint8_t>    base_guest_bytes;
    std::vector<std::uint8_t>    mip_guest_bytes;
};

struct NativePreparedCubeMatrixFixture
{
    std::uint32_t                width           = CubeMatrixViewportWidth();
    std::uint32_t                height          = CubeMatrixViewportHeight();
    std::uint32_t                terminal_stride = kNativePreparedCubeMatrixOutputWidth * 4U;
    std::uint32_t                vertex_stride   = kNativePreparedCubeMatrixVertexStride;
    std::array<std::uint32_t, 2> selected_lods   = { 0U, 0U };

    // Raw guest words and the complete register image are owned by the fixture
    // so a catalog/native consumer can use exactly the same input identity.
    std::vector<std::uint32_t>      vertex_shader_words;
    std::vector<std::uint32_t>      pixel_shader_words;
    std::vector<std::uint32_t>      register_image;
    std::array<std::uint32_t, 6>    vertex_fetch_words{};
    std::vector<std::uint8_t>       vertex_bytes;
    std::vector<std::uint8_t>       index_bytes;
    NativePreparedCubeMatrixTexture texture;

    // Columns select faces; rows project RGB and alpha (constant one for RGBA8).
    // Presenter alpha is 255, including the untouched terminal background.
    std::vector<std::uint8_t> expected_selected_pixels;
};

// Independent scalar oracle. No SDK layout, endian, conversion or half helpers.
inline float CubeOracleHalf(std::uint16_t bits)
{
    const auto   exponent  = (bits >> 10) & 31;
    const auto   fraction  = bits & 1023;
    const double magnitude = exponent == 0   ? std::ldexp(double(fraction), -24)
                             : exponent < 31 ? std::ldexp(double(1024 + fraction), int(exponent) - 25)
                             : fraction      ? std::numeric_limits<double>::quiet_NaN()
                                             : std::numeric_limits<double>::infinity();
    return float(bits & 0x8000 ? -magnitude : magnitude);
}

// Xenos face IDs/orientation (emulator corroboration; oracle code is independent):
// https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/dxbc_shader_translator_alu.cc#L233
inline std::array<float, 3> CubeMatrixDirection(std::uint32_t face, float u, float v)
{
    const float s = 2 * u - 1, t = 2 * v - 1;
    switch (face)
    {
        case 0:
            return { 1, -t, -s };
        case 1:
            return { -1, -t, s };
        case 2:
            return { s, 1, t };
        case 3:
            return { s, -1, -t };
        case 4:
            return { s, -t, 1 };
        case 5:
            return { -s, -t, -1 };
        default:
            throw std::runtime_error("cube face exceeds six-face bound");
    }
}

struct CubeOracleLocation
{
    std::uint32_t face, x, y;
    double        u, v, major_margin;
};

// Xenos orientation source, with no executable code shared with the emulator:
// https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/dxbc_shader_translator_alu.cc#L233
inline CubeOracleLocation CubeOracleProject(std::array<double, 3> direction, std::uint32_t level, double epsilon = 0)
{
    const auto    x = direction[0], y = direction[1], z = direction[2];
    const auto    ax = std::abs(x), ay = std::abs(y), az = std::abs(z);
    double        a, s, t, margin;
    std::uint32_t face;
    if (az >= ax && az >= ay)
    {
        a      = az;
        s      = z < 0 ? -x : x;
        t      = -y;
        face   = z < 0 ? 5 : 4;
        margin = a - std::max(ax, ay);
    }
    else if (ay >= ax)
    {
        a      = ay;
        s      = x;
        t      = y < 0 ? -z : z;
        face   = y < 0 ? 3 : 2;
        margin = a - ax;
    }
    else
    {
        a      = ax;
        s      = x < 0 ? z : -z;
        t      = -y;
        face   = x < 0 ? 1 : 0;
        margin = a - ay;
    }
    // Record tie priority, but acceptance excludes every tie and seam.
    margin = a - (face < 2 ? std::max(ay, az) : face < 4 ? std::max(ax, az)
                                                         : std::max(ax, ay));
    if (level >= cubeMatrixForm->levels || margin <= 0 || !std::isfinite(a))
        throw std::runtime_error("cube oracle requires a strict finite major axis");
    const auto u = .5 * (s / a + 1) + epsilon, v = .5 * (t / a + 1) + epsilon;
    const auto n = cubeMatrixForm->width >> level;
    if (!(u > 0 && u < 1 && v > 0 && v < 1))
        throw std::runtime_error("cube oracle touches a face seam");
    return { face, std::uint32_t(std::floor(u * n)), std::uint32_t(std::floor(v * n)), u, v, margin };
}

inline float CubeMatrixVertexFloat(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size())
        throw std::runtime_error("cube vertex extent is incomplete");
    const auto bits = std::uint32_t(bytes[offset]) << 24 | std::uint32_t(bytes[offset + 1]) << 16 |
                      std::uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3];
    return std::bit_cast<float>(bits);
}

inline CubeOracleLocation CubeOracleSample(const NativePreparedCubeMatrixFixture& fixture, std::uint32_t panel, std::uint32_t x, std::uint32_t y, double center, double epsilon = 0)
{
    std::array<double, 3> direction{};
    const auto            base = panel * 3 * fixture.vertex_stride;
    for (std::uint32_t lane = 0; lane < 3; ++lane)
    {
        const auto a    = CubeMatrixVertexFloat(fixture.vertex_bytes, base + 16 + lane * 4);
        const auto b    = CubeMatrixVertexFloat(fixture.vertex_bytes, base + fixture.vertex_stride + 16 + lane * 4);
        const auto c    = CubeMatrixVertexFloat(fixture.vertex_bytes, base + 2 * fixture.vertex_stride + 16 + lane * 4);
        direction[lane] = a + (b - a) * (y + center) / (2 * cubeMatrixForm->width) + (c - a) * (x + center) / (2 * cubeMatrixForm->width);
    }
    return CubeOracleProject(direction, fixture.selected_lods[0], epsilon);
}

inline std::vector<std::uint8_t> CubeOraclePayload(const NativePreparedCubeMatrixFixture& fixture)
{
    const auto&               form = *cubeMatrixForm;
    std::vector<std::uint8_t> payload;
    payload.reserve(form.payload_bytes);
    for (std::uint32_t face = 0; face < 6; ++face)
        for (std::uint32_t level = 0; level < cubeMatrixForm->levels; ++level)
        {
            const auto& raw = level ? fixture.texture.mip_guest_bytes : fixture.texture.base_guest_bytes;
            for (std::uint32_t y = 0; y < CubeMatrixLevelUnits(level); ++y)
                for (std::uint32_t x = 0; x < CubeMatrixLevelUnits(level); ++x)
                {
                    const auto at = CubeMatrixAddress(form, face, level, x, y);
                    if (at + form.bytes_per_texel > raw.size())
                        throw std::runtime_error("cube raw allocation omits a face/level texel");
                    for (std::uint32_t byte = 0; byte < form.bytes_per_texel; ++byte)
                        payload.push_back(raw[at + (form.format == 6 ? 3 - byte : byte ^ 1U)]);
                }
        }
    return payload;
}

// Endpoint-only BC3 oracle: S3TC endpoint expansion, no interpolation claims.
inline std::uint8_t CubeOracleBc3(std::span<const std::uint8_t> raw, std::size_t at, unsigned x, unsigned y, unsigned channel)
{
    std::array<std::uint8_t, 16> b{};
    for (unsigned i = 0; i < 16; ++i)
        b[i] = raw[at + (i ^ 1U)];
    const auto    texel = (y % 4) * 4 + x % 4;
    std::uint64_t alpha = 0;
    for (unsigned i = 0; i < 6; ++i)
        alpha |= std::uint64_t(b[i + 2]) << (8 * i);
    const auto a  = unsigned((alpha >> (3 * texel)) & 7);
    const auto c0 = unsigned(b[8] | unsigned(b[9]) << 8), c1 = unsigned(b[10] | unsigned(b[11]) << 8);
    const auto colors = unsigned(b[12]) | unsigned(b[13]) << 8 | unsigned(b[14]) << 16 | unsigned(b[15]) << 24;
    const auto c      = (colors >> (2 * texel)) & 3;
    if (b[0] == b[1] || a > 1 || c0 <= c1 || c > 1)
        throw std::runtime_error("BC3 oracle requires distinct alpha endpoints and color/alpha endpoint codes");
    if (channel == 3)
        return b[a];
    const auto endpoint = c ? c1 : c0;
    const auto code     = channel == 0 ? endpoint >> 11 : channel == 1 ? (endpoint >> 5) & 63
                                                                       : endpoint & 31;
    // Positives use extreme color codes, avoiding device-dependent 565 rounding.
    if (code != 0 && code != (channel == 1 ? 63U : 31U))
        throw std::runtime_error("BC3 endpoint color is outside the exact extreme palette");
    return code ? 255 : 0;
}

inline std::vector<std::uint8_t> CubeOraclePixels(const NativePreparedCubeMatrixFixture& fixture)
{
    if (cubeMatrixInterpolationMode >= 0)
        return {};
    const auto&               form = *cubeMatrixForm;
    std::vector<std::uint8_t> pixels(std::size_t(kNativePreparedCubeMatrixOutputWidth) * kNativePreparedCubeMatrixOutputHeight * 4, 0);
    for (std::size_t at = 3; at < pixels.size(); at += 4)
        pixels[at] = 255;
    for (std::uint32_t channel = 0; channel < 4; ++channel)
        for (std::uint32_t face = 0; face < 6; ++face)
            for (std::uint32_t y = 0; y < form.width; ++y)
                for (std::uint32_t x = 0; x < form.width; ++x)
                {
                    const auto   location = CubeOracleSample(fixture, channel * 6 + face, x, y, 0, 1.5 / (1024 * form.width));
                    const auto&  raw      = fixture.selected_lods[0] ? fixture.texture.mip_guest_bytes : fixture.texture.base_guest_bytes;
                    const auto   at       = CubeMatrixAddress(form, location.face, fixture.selected_lods[0], form.format == 20 ? location.x / 4 : location.x, form.format == 20 ? location.y / 4 : location.y);
                    std::uint8_t value;
                    if (form.format == 20)
                        value = CubeOracleBc3(raw, at, location.x, location.y, channel);
                    else if (form.format == 6)
                    {
                        constexpr std::array<unsigned, 3> selectors{ 2, 1, 0 };
                        value = channel == 3 ? 255 : raw.at(at + 3 - selectors[channel]);
                    }
                    else
                    {
                        const auto half   = std::uint16_t(std::uint16_t(raw.at(at + channel * 2)) << 8 | raw.at(at + channel * 2 + 1));
                        const auto mapped = (double(CubeOracleHalf(half)) + 2) / 4;
                        if (!(mapped > 0 && mapped < 1))
                            throw std::runtime_error("cube positive readout saturates");
                        value = std::uint8_t(std::floor(mapped * 255 + .5));
                    }
                    const auto out = (std::size_t(channel * form.width + y) * kNativePreparedCubeMatrixOutputWidth + face * form.width + x) * 4;
                    pixels[out] = pixels[out + 1] = pixels[out + 2] = value;
                }
    return pixels;
}

NativePreparedCubeMatrixFixture MakeNativePreparedCubeMatrixFixture();

enum class NativePreparedCubeMatrixUpdateKind : std::uint32_t
{
    kCombined       = 1,
    kGuestAlphaOnly = 2,
};

// Expected pixels read the current copied guest allocations through the
// independent scalar oracle. The fixture carries the selected absolute LOD.
std::vector<std::uint8_t> ExpectedNativePreparedCubeMatrixPixels(const NativePreparedCubeMatrixFixture& fixture,
                                                                 std::uint32_t                          first_lod,
                                                                 std::uint32_t                          second_lod);

struct NativePreparedCubeMatrixOriginalParams
{
    NativePreparedCubeMatrixFixture    fixture;
    std::uint32_t                      first_lod   = 0U;
    std::uint32_t                      second_lod  = 0U;
    NativePreparedCubeMatrixUpdateKind update_kind = NativePreparedCubeMatrixUpdateKind::kCombined;
};

struct NativePreparedCubeMatrixDrawRecord
{
    std::uint64_t              event_ordinal         = 0;
    std::uint64_t              id                    = 0;
    std::uint64_t              frame                 = 0;
    std::uint64_t              submission            = 0;
    bool                       original_host_issued  = false;
    bool                       cpu_data_complete     = false;
    std::uint32_t              execution_status      = 0;
    std::uint32_t              execution_reason      = 0;
    std::uint32_t              primitive_type        = 0;
    std::uint32_t              requested_index_count = 0;
    std::uint64_t              vertex_shader_hash    = 0;
    std::uint64_t              pixel_shader_hash     = 0;
    std::vector<std::uint32_t> registers;
    std::vector<std::uint32_t> vertex_shader_words;
    std::vector<std::uint32_t> pixel_shader_words;

    struct TextureRecord
    {
        std::uint32_t              fetch_constant       = 0;
        std::uint32_t              format               = 0;
        std::uint32_t              endianness           = 0;
        std::uint32_t              dimension            = 0;
        std::uint32_t              width                = 0;
        std::uint32_t              height               = 0;
        std::uint32_t              pitch                = 0;
        std::uint32_t              mip_min_level        = 0;
        std::uint32_t              mip_max_level        = 0;
        bool                       tiled                = false;
        bool                       packed_mips          = false;
        bool                       available            = false;
        bool                       gpu_written          = false;
        std::uint32_t              unavailable_reason   = 0;
        std::uint32_t              base_address         = 0;
        std::uint32_t              base_size            = 0;
        std::uint32_t              mip_address          = 0;
        std::uint32_t              mip_size             = 0;
        std::uint32_t              depth                = 0;
        std::uint32_t              stage                = 0;
        std::uint32_t              shader_binding_index = 0;
        std::uint32_t              sampler_value        = 0;
        std::uint32_t              component_mapping    = 0;
        std::uint8_t               swizzled_signs       = 0;
        bool                       is_signed            = false;
        bool                       required             = true;
        std::vector<std::uint32_t> fetch_words;
        std::vector<std::uint8_t>  base_bytes;
        std::vector<std::uint8_t>  mip_bytes;
        bool                       base_bytes_copied       = false;
        bool                       mip_bytes_copied        = false;
        bool                       memory_current_before   = false;
        bool                       memory_current_after    = false;
        bool                       memory_watch_supplied   = false;
        std::uint64_t              memory_watch_identity   = 0;
        std::uint64_t              memory_watch_generation = 0;
        bool                       memory_watch_stale      = false;
    };

    std::vector<TextureRecord> textures;
};

struct NativePreparedCubeMatrixCopyRecord
{
    std::uint64_t              event_ordinal = 0;
    std::uint64_t              id            = 0;
    std::uint64_t              frame         = 0;
    std::uint64_t              submission    = 0;
    std::string                mode;
    bool                       original_succeeded             = false;
    bool                       cpu_data_complete              = false;
    std::uint32_t              unavailable_reason             = 0;
    bool                       source_rect_present            = false;
    std::uint32_t              source_x                       = 0;
    std::uint32_t              source_y                       = 0;
    std::uint32_t              source_width                   = 0;
    std::uint32_t              source_height                  = 0;
    std::uint32_t              source_geometry_fetch_constant = 0;
    std::uint32_t              source_geometry_address        = 0;
    std::uint32_t              source_geometry_size           = 0;
    std::uint32_t              written_address                = 0;
    std::uint32_t              written_length                 = 0;
    std::uint32_t              planned_address                = 0;
    std::uint32_t              planned_length                 = 0;
    std::vector<std::uint32_t> registers;
};

struct NativePreparedCubeMatrixSwapRecord
{
    std::uint64_t event_ordinal      = 0;
    std::uint64_t id                 = 0;
    std::uint64_t frame              = 0;
    std::uint64_t submission         = 0;
    std::uint64_t frontbuffer_ptr    = 0;
    std::uint32_t width              = 0;
    std::uint32_t height             = 0;
    bool          original_succeeded = false;
};

struct NativePreparedCubeMatrixFrameEndRecord
{
    std::uint64_t event_ordinal      = 0;
    std::uint64_t frame              = 0;
    std::uint64_t submission         = 0;
    bool          original_succeeded = false;
};

struct NativePreparedCubeMatrixOriginalResult
{
    bool                               original_adapter_identity_available = false;
    std::uint32_t                      original_adapter_luid_low           = 0;
    std::int32_t                       original_adapter_luid_high          = 0;
    bool                               progressed                          = false;
    bool                               completion_observed                 = false;
    bool                               baseline_completion_observed        = false;
    bool                               baseline_terminal_capture           = false;
    bool                               cpu_update_observed                 = false;
    bool                               terminal_capture                    = false;
    bool                               actual_readback_owner               = false;
    bool                               cpu_partial_overlap_writes          = false;
    bool                               source_raw_bytes_frozen             = false;
    std::uint32_t                      cpu_update_level                    = 0;
    bool                               source_draw_issued                  = false;
    bool                               terminal_pixels_match_expected      = false;
    bool                               cpu_update_overlap_written          = false;
    bool                               cpu_update_outside_unchanged        = false;
    std::uint32_t                      terminal_width                      = 0;
    std::uint32_t                      terminal_height                     = 0;
    std::uint32_t                      terminal_stride                     = 0;
    std::uint32_t                      cpu_update_guest_offset             = 0;
    std::uint32_t                      cpu_update_guest_length             = 0;
    NativePreparedCubeMatrixUpdateKind update_kind                         = NativePreparedCubeMatrixUpdateKind::kCombined;
    std::vector<std::uint8_t>          baseline_terminal_pixels;
    std::vector<std::uint8_t>          baseline_expected_pixels;
    std::vector<std::uint8_t>          terminal_pixels;
    std::vector<std::uint8_t>          expected_pixels;
    std::vector<std::uint8_t>          old_base_bytes;
    std::vector<std::uint8_t>          old_mip_bytes;
    std::vector<std::uint8_t>          new_base_bytes;
    std::vector<std::uint8_t>          new_mip_bytes;
    // The fixture after parameterized LOD bytes and guest mip storage have
    // been applied. Native replay can consume this exact source identity.
    NativePreparedCubeMatrixFixture source_fixture;

    // These are immutable copies taken from callback views before the views are
    // released. Addresses remain process-local, while words and register banks
    // preserve the native comparison identity.
    std::vector<NativePreparedCubeMatrixDrawRecord>     draws;
    std::vector<NativePreparedCubeMatrixDrawRecord>     prepared_draws;
    std::vector<NativePreparedCubeMatrixCopyRecord>     copies;
    std::vector<NativePreparedCubeMatrixCopyRecord>     prepared_copies;
    std::vector<NativePreparedCubeMatrixSwapRecord>     swaps;
    std::vector<NativePreparedCubeMatrixSwapRecord>     prepared_swaps;
    std::vector<NativePreparedCubeMatrixFrameEndRecord> frame_ends;
    std::vector<std::uint32_t>                          source_registers;
    std::array<std::uint32_t, 6>                        source_texture_fetch_words{};
    std::array<std::uint32_t, 6>                        source_vertex_fetch_words{};
    std::uint32_t                                       source_index_guest_base = 0;
    std::uint32_t                                       published_word_count    = 0;
    std::string                                         runtime_module_path;
    std::string                                         gpu_module_path;
    std::uint64_t                                       completed_fence       = 0;
    std::uint64_t                                       baseline_copy_id      = 0;
    std::uint64_t                                       baseline_swap_id      = 0;
    std::uint64_t                                       baseline_frame        = 0;
    std::uint64_t                                       baseline_submission   = 0;
    std::uint64_t                                       completion_copy_id    = 0;
    std::uint64_t                                       completion_swap_id    = 0;
    std::uint64_t                                       completion_frame      = 0;
    std::uint64_t                                       completion_submission = 0;
    std::string                                         readback_owner;
    std::string                                         failure;
};

// The caller owns one process-wide logging lifetime across all GPU runs.
// Consumes params.fixture into result so full decoded levels have one owner.
int RunNativePreparedCubeMatrixOriginal(
    NativePreparedCubeMatrixOriginalParams&                                          params,
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& observer,
    NativePreparedCubeMatrixOriginalResult&                                          result,
    std::string&                                                                     error);

inline int RunNativePreparedCubeMatrixOriginal(NativePreparedCubeMatrixOriginalParams& params,
                                               NativePreparedCubeMatrixOriginalResult& result,
                                               std::string&                            error)
{
    return RunNativePreparedCubeMatrixOriginal(params, nullptr, result, error);
}

// Construction pieces shared by the fixture builder (native_prepared_cube_matrix_fixture.cpp)
// and the Original oracle (native_prepared_cube_matrix_original.cpp).
namespace cube_matrix_parts
{

inline constexpr std::uint32_t kPageSize = 0x1000U;

inline constexpr std::uint32_t kNativePreparedCubeMatrixIndexCount = 3U;

// The SDK clamps each vertex index to VGT_MAX_VTX_INDX; 24 panels use 72 vertices.
inline constexpr std::uint32_t kNativePreparedCubeMatrixMaxVertexIndex = 24U * kNativePreparedCubeMatrixIndexCount - 1U;

inline constexpr auto kNativePreparedCubeMatrixIndexEndian = rex::graphics::xenos::Endian::k8in16;

inline constexpr std::uint32_t kNativePreparedCubeMatrixDrawInitiator =
    static_cast<std::uint32_t>(rex::graphics::xenos::PrimitiveType::kTriangleList) |
    (static_cast<std::uint32_t>(rex::graphics::xenos::SourceSelect::kDMA) << 6U) |
    (kNativePreparedCubeMatrixIndexCount << 16U);

// Match the clean installed control VS identity rows from SDK commit
// df8ec1c544c1e8f391dcfaff295ef387a1a7fea9,
// tests/installed_prepared_control/control.cpp:kConsumerVertexConstants.
inline constexpr std::array<std::uint32_t, 36> kVertexConstants = {
    0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U
};

inline constexpr std::array<std::uint32_t, 4> kPixelConstant0 = { 0x3FC00000U, 0x3FC00000U, 0x3FC00000U, 0x3FC00000U };

inline constexpr std::array<std::uint32_t, 4> kPixelConstant255 = {
    0x3DCCCCCDU, 0x3E4CCCCDU, 0x3E99999AU, 0x3F800000U
};

inline std::uint32_t AlignToPage(std::uint32_t bytes)
{
    return bytes == 0U ? 0U : (bytes + kPageSize - 1U) & ~(kPageSize - 1U);
}

inline void AppendBigEndianFloat(std::vector<std::uint8_t>& bytes, float value)
{
    const auto bits = std::bit_cast<std::uint32_t>(value);
    bytes.push_back(static_cast<std::uint8_t>(bits >> 24U));
    bytes.push_back(static_cast<std::uint8_t>(bits >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(bits >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(bits));
}

std::vector<std::uint8_t> MakeExpectedPixels(const rex::graphics::testing::NativePreparedCubeMatrixFixture& fixture, std::uint32_t, std::uint32_t);

void WriteCubeTexel(rex::graphics::testing::NativePreparedCubeMatrixFixture& fixture, std::uint32_t face, std::uint32_t level, std::uint32_t x, std::uint32_t y, std::uint32_t phase);

std::array<std::uint32_t, 6> MakeTextureFetch(std::uint32_t base_address,
                                              std::uint32_t mip_address);

std::array<std::uint32_t, 6> MakeVertexFetch(std::uint32_t address, std::uint32_t bytes);

} // namespace cube_matrix_parts

} // namespace rex::graphics::testing
