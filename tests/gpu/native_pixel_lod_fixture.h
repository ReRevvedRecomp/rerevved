#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::testing
{

inline constexpr std::uint32_t kNativePixelLodWidth         = 128U;
inline constexpr std::uint32_t kNativePixelLodHeight        = 128U;
inline constexpr std::uint32_t kNativePixelLodMipCount      = 8U;
inline constexpr std::uint32_t kNativePixelLodVertexStride  = 56U;
inline constexpr std::uint32_t kNativePixelLodRegisterCount = 0x5003U;

struct NativePixelLodMip
{
    std::uint32_t             level     = 0;
    std::uint32_t             width     = 0;
    std::uint32_t             height    = 0;
    std::uint32_t             row_pitch = 0;
    std::vector<std::uint8_t> linear_rgba8;
    // The bytes below are the exact guest layout for one independently stored
    // level. The base level lives in base_guest_bytes; levels 1..7 live in
    // mip_guest_bytes at their layout offsets.
    std::uint32_t guest_offset = 0;
    std::uint32_t guest_extent = 0;
};

struct NativePixelLodTexture
{
    std::uint32_t                  width         = kNativePixelLodWidth;
    std::uint32_t                  height        = kNativePixelLodHeight;
    std::uint32_t                  pitch         = kNativePixelLodWidth;
    std::uint32_t                  format        = 0;
    std::uint32_t                  dimension     = 0;
    bool                           tiled         = false;
    bool                           packed_mips   = false;
    std::uint32_t                  mip_min_level = 0;
    std::uint32_t                  mip_max_level = kNativePixelLodMipCount - 1U;
    std::array<std::uint32_t, 6>   fetch_words{};
    std::vector<std::uint8_t>      base_guest_bytes;
    std::vector<std::uint8_t>      mip_guest_bytes;
    std::vector<NativePixelLodMip> mips;
};

struct NativePixelLodFixture
{
    std::uint32_t                width           = kNativePixelLodWidth;
    std::uint32_t                height          = kNativePixelLodHeight;
    std::uint32_t                terminal_stride = kNativePixelLodWidth * 4U;
    std::uint32_t                vertex_stride   = kNativePixelLodVertexStride;
    std::array<std::uint32_t, 2> selected_lods   = { 2U, 5U };

    // Raw guest words and the complete register image are owned by the fixture
    // so a catalog/native consumer can use exactly the same input identity.
    std::vector<std::uint32_t>   vertex_shader_words;
    std::vector<std::uint32_t>   pixel_shader_words;
    std::vector<std::uint32_t>   register_image;
    std::array<std::uint32_t, 6> vertex_fetch_words{};
    std::vector<std::uint8_t>    vertex_bytes;
    std::vector<std::uint8_t>    index_bytes;
    NativePixelLodTexture        texture;

    // Expected terminal planes are byte-exact RGBA8 sums of the two selected
    // solid mip colors. There is one independent expected plane for every pair
    // so changing or removing any mip cannot leave the selected result intact.
    std::vector<std::vector<std::uint8_t>> expected_by_lod_pair;
    std::vector<std::uint8_t>              expected_selected_pixels;
};

NativePixelLodFixture MakeNativePixelLodFixture();

// Returns an empty vector for an invalid level. This derives the expected
// pixels from the fixture's current mip bytes, allowing mutation checks to use
// the same owned source data that is sent to the original GPU.
std::vector<std::uint8_t> ExpectedNativePixelLodPixels(const NativePixelLodFixture& fixture,
                                                       std::uint32_t                first_lod,
                                                       std::uint32_t                second_lod);

struct NativePixelLodOriginalParams
{
    NativePixelLodFixture fixture    = MakeNativePixelLodFixture();
    std::uint32_t         first_lod  = 2U;
    std::uint32_t         second_lod = 5U;
};

struct NativePixelLodDrawRecord
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
};

struct NativePixelLodCopyRecord
{
    std::uint64_t              event_ordinal = 0;
    std::uint64_t              id            = 0;
    std::uint64_t              frame         = 0;
    std::uint64_t              submission    = 0;
    std::string                mode;
    bool                       original_succeeded = false;
    bool                       cpu_data_complete  = false;
    std::uint32_t              written_address    = 0;
    std::uint32_t              written_length     = 0;
    std::uint32_t              planned_address    = 0;
    std::uint32_t              planned_length     = 0;
    std::vector<std::uint32_t> registers;
};

struct NativePixelLodSwapRecord
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

struct NativePixelLodFrameEndRecord
{
    std::uint64_t event_ordinal      = 0;
    std::uint64_t frame              = 0;
    std::uint64_t submission         = 0;
    bool          original_succeeded = false;
};

struct NativePixelLodOriginalResult
{
    bool                      progressed                     = false;
    bool                      completion_observed            = false;
    bool                      terminal_capture               = false;
    bool                      actual_readback_owner          = false;
    bool                      source_draw_issued             = false;
    bool                      terminal_pixels_match_expected = false;
    std::uint32_t             terminal_width                 = 0;
    std::uint32_t             terminal_height                = 0;
    std::uint32_t             terminal_stride                = 0;
    std::vector<std::uint8_t> terminal_pixels;
    std::vector<std::uint8_t> expected_pixels;
    // The fixture after parameterized LOD bytes and guest mip storage have
    // been applied. Native replay can consume this exact source identity.
    NativePixelLodFixture source_fixture;

    // These are immutable copies taken from callback views before the views are
    // released. Addresses remain process-local, while words and register banks
    // preserve the native comparison identity.
    std::vector<NativePixelLodDrawRecord>     draws;
    std::vector<NativePixelLodDrawRecord>     prepared_draws;
    std::vector<NativePixelLodCopyRecord>     copies;
    std::vector<NativePixelLodCopyRecord>     prepared_copies;
    std::vector<NativePixelLodSwapRecord>     swaps;
    std::vector<NativePixelLodFrameEndRecord> frame_ends;
    std::vector<std::uint32_t>                source_registers;
    std::array<std::uint32_t, 6>              source_texture_fetch_words{};
    std::array<std::uint32_t, 6>              source_vertex_fetch_words{};
    std::uint32_t                             source_index_guest_base = 0;
    std::uint32_t                             published_word_count    = 0;
    std::uint64_t                             completed_fence         = 0;
    std::uint64_t                             completion_copy_id      = 0;
    std::uint64_t                             completion_swap_id      = 0;
    std::uint64_t                             completion_frame        = 0;
    std::uint64_t                             completion_submission   = 0;
    std::string                               readback_owner;
    std::string                               failure;
};

// The caller owns one process-wide logging lifetime across all GPU runs.
int RunNativePixelLodOriginal(
    const NativePixelLodOriginalParams&                                              params,
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& observer,
    NativePixelLodOriginalResult&                                                    result,
    std::string&                                                                     error);

inline int RunNativePixelLodOriginal(const NativePixelLodOriginalParams& params,
                                     NativePixelLodOriginalResult&       result,
                                     std::string&                        error)
{
    return RunNativePixelLodOriginal(params, nullptr, result, error);
}

// Construction pieces shared by the fixture builder (native_pixel_lod_fixture.cpp)
// and the Original oracle (native_pixel_lod_original.cpp).
namespace pixel_lod_parts
{

inline constexpr std::uint32_t kPageSize = 0x1000U;

inline constexpr std::uint32_t kNativePixelLodIndexCount = 3U;

inline constexpr auto kNativePixelLodIndexEndian = rex::graphics::xenos::Endian::k8in16;

inline constexpr std::uint32_t kNativePixelLodDrawInitiator =
    static_cast<std::uint32_t>(rex::graphics::xenos::PrimitiveType::kTriangleList) |
    (static_cast<std::uint32_t>(rex::graphics::xenos::SourceSelect::kDMA) << 6U) |
    (kNativePixelLodIndexCount << 16U);

// Match the clean installed control VS identity rows from SDK commit
// df8ec1c544c1e8f391dcfaff295ef387a1a7fea9,
// tests/installed_prepared_control/control.cpp:kConsumerVertexConstants.
inline constexpr std::array<std::uint32_t, 36> kVertexConstants = {
    0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U
};

inline constexpr std::array<std::uint32_t, 4> kPixelConstant0 = { 0U, 0U, 0U, 0x3F800000U };

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

std::vector<std::uint8_t> MakeExpectedPixels(const rex::graphics::testing::NativePixelLodFixture& fixture,
                                             std::uint32_t                                        first_lod,
                                             std::uint32_t                                        second_lod);

void WriteLinearMip(std::vector<std::uint8_t>& storage, std::uint32_t offset, const rex::graphics::testing::NativePixelLodMip& mip);

std::array<std::uint32_t, 6> MakeTextureFetch(std::uint32_t base_address,
                                              std::uint32_t mip_address);

std::array<std::uint32_t, 6> MakeVertexFetch(std::uint32_t address, std::uint32_t bytes);

} // namespace pixel_lod_parts

} // namespace rex::graphics::testing
