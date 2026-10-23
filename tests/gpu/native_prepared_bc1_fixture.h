#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::testing
{

inline constexpr std::uint32_t                                         kNativePreparedBc1Width         = 128U;
inline constexpr std::uint32_t                                         kNativePreparedBc1Height        = 128U;
inline constexpr std::uint32_t                                         kNativePreparedBc1OutputWidth   = 1280U;
inline constexpr std::uint32_t                                         kNativePreparedBc1OutputHeight  = 720U;
inline constexpr std::uint32_t                                         kNativePreparedBc1MipCount      = 8U;
inline constexpr std::uint32_t                                         kNativePreparedBc1VertexStride  = 56U;
inline constexpr std::uint32_t                                         kNativePreparedBc1RegisterCount = 0x5003U;
inline constexpr std::array<std::uint32_t, kNativePreparedBc1MipCount> kNativePreparedBc1Widths        = {
    128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U
};
inline constexpr std::array<std::uint32_t, kNativePreparedBc1MipCount> kNativePreparedBc1Heights = {
    128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U
};
// Exact linear BC1 guest locations for the selected 128x128 packed layout.
// Levels 1 and 2 are ordinary mip allocations; levels 3..7 share the tail.
inline constexpr std::array<std::uint32_t, kNativePreparedBc1MipCount> kNativePreparedBc1GuestOffsets = {
    0x0000U, 0x0000U, 0x2000U, 0x4020U, 0x4010U, 0x4008U, 0x4200U, 0x4100U
};
inline constexpr std::array<std::uint32_t, kNativePreparedBc1MipCount> kNativePreparedBc1RowPitches = {
    0x0100U, 0x0100U, 0x0100U, 0x0100U, 0x0100U, 0x0100U, 0x0100U, 0x0100U
};

struct NativePreparedBc1Mip
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

struct NativePreparedBc1Texture
{
    std::uint32_t                     width         = kNativePreparedBc1Width;
    std::uint32_t                     height        = kNativePreparedBc1Height;
    std::uint32_t                     pitch         = kNativePreparedBc1Width;
    std::uint32_t                     format        = 0;
    std::uint32_t                     endianness    = 0;
    std::uint32_t                     swizzle       = 0;
    std::uint32_t                     dimension     = 0;
    bool                              tiled         = false;
    bool                              packed_mips   = false;
    std::uint32_t                     mip_min_level = 0;
    std::uint32_t                     mip_max_level = kNativePreparedBc1MipCount - 1U;
    std::array<std::uint32_t, 6>      fetch_words{};
    std::vector<std::uint8_t>         base_guest_bytes;
    std::vector<std::uint8_t>         mip_guest_bytes;
    std::vector<NativePreparedBc1Mip> mips;
};

struct NativePreparedBc1Fixture
{
    std::uint32_t                width           = kNativePreparedBc1Width;
    std::uint32_t                height          = kNativePreparedBc1Height;
    std::uint32_t                terminal_stride = kNativePreparedBc1OutputWidth * 4U;
    std::uint32_t                vertex_stride   = kNativePreparedBc1VertexStride;
    std::array<std::uint32_t, 2> selected_lods   = { 2U, 5U };

    // Raw guest words and the complete register image are owned by the fixture
    // so a catalog/native consumer can use exactly the same input identity.
    std::vector<std::uint32_t>   vertex_shader_words;
    std::vector<std::uint32_t>   pixel_shader_words;
    std::vector<std::uint32_t>   register_image;
    std::array<std::uint32_t, 6> vertex_fetch_words{};
    std::vector<std::uint8_t>    vertex_bytes;
    std::vector<std::uint8_t>    index_bytes;
    NativePreparedBc1Texture     texture;

    // Selected plane is a byte-exact RGBA8 sum; only selected entry owns full
    // canvas.
    std::vector<std::vector<std::uint8_t>> expected_by_lod_pair;
    std::vector<std::uint8_t>              expected_selected_pixels;
};

NativePreparedBc1Fixture MakeNativePreparedBc1Fixture();

// Returns an empty vector for an invalid level. This derives the expected
// pixels from the fixture's current mip bytes, allowing mutation checks to use
// the same owned source data that is sent to the original GPU.
std::vector<std::uint8_t> ExpectedNativePreparedBc1Pixels(const NativePreparedBc1Fixture& fixture,
                                                          std::uint32_t                   first_lod,
                                                          std::uint32_t                   second_lod);

struct NativePreparedBc1OriginalParams
{
    NativePreparedBc1Fixture fixture    = MakeNativePreparedBc1Fixture();
    std::uint32_t            first_lod  = 2U;
    std::uint32_t            second_lod = 5U;
};

struct NativePreparedBc1DrawRecord
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

struct NativePreparedBc1CopyRecord
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

struct NativePreparedBc1SwapRecord
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

struct NativePreparedBc1FrameEndRecord
{
    std::uint64_t event_ordinal      = 0;
    std::uint64_t frame              = 0;
    std::uint64_t submission         = 0;
    bool          original_succeeded = false;
};

struct NativePreparedBc1OriginalResult
{
    bool                      progressed                     = false;
    bool                      completion_observed            = false;
    bool                      baseline_completion_observed   = false;
    bool                      baseline_terminal_capture      = false;
    bool                      cpu_update_observed            = false;
    bool                      terminal_capture               = false;
    bool                      actual_readback_owner          = false;
    bool                      cpu_partial_overlap_writes     = false;
    bool                      source_raw_bytes_frozen        = false;
    std::uint32_t             cpu_update_level               = 0;
    bool                      source_draw_issued             = false;
    bool                      terminal_pixels_match_expected = false;
    bool                      cpu_update_overlap_written     = false;
    bool                      cpu_update_outside_unchanged   = false;
    std::uint32_t             terminal_width                 = 0;
    std::uint32_t             terminal_height                = 0;
    std::uint32_t             terminal_stride                = 0;
    std::uint32_t             cpu_update_guest_offset        = 0;
    std::uint32_t             cpu_update_guest_length        = 0;
    std::vector<std::uint8_t> baseline_terminal_pixels;
    std::vector<std::uint8_t> baseline_expected_pixels;
    std::vector<std::uint8_t> terminal_pixels;
    std::vector<std::uint8_t> expected_pixels;
    std::vector<std::uint8_t> old_base_bytes;
    std::vector<std::uint8_t> old_mip_bytes;
    std::vector<std::uint8_t> new_base_bytes;
    std::vector<std::uint8_t> new_mip_bytes;
    // The fixture after parameterized LOD bytes and guest mip storage have
    // been applied. Native replay can consume this exact source identity.
    NativePreparedBc1Fixture source_fixture;

    // These are immutable copies taken from callback views before the views are
    // released. Addresses remain process-local, while words and register banks
    // preserve the native comparison identity.
    std::vector<NativePreparedBc1DrawRecord>     draws;
    std::vector<NativePreparedBc1DrawRecord>     prepared_draws;
    std::vector<NativePreparedBc1CopyRecord>     copies;
    std::vector<NativePreparedBc1CopyRecord>     prepared_copies;
    std::vector<NativePreparedBc1SwapRecord>     swaps;
    std::vector<NativePreparedBc1SwapRecord>     prepared_swaps;
    std::vector<NativePreparedBc1FrameEndRecord> frame_ends;
    std::vector<std::uint32_t>                   source_registers;
    std::array<std::uint32_t, 6>                 source_texture_fetch_words{};
    std::array<std::uint32_t, 6>                 source_vertex_fetch_words{};
    std::uint32_t                                source_index_guest_base = 0;
    std::uint32_t                                published_word_count    = 0;
    std::string                                  runtime_module_path;
    std::string                                  gpu_module_path;
    std::uint64_t                                completed_fence       = 0;
    std::uint64_t                                baseline_copy_id      = 0;
    std::uint64_t                                baseline_swap_id      = 0;
    std::uint64_t                                baseline_frame        = 0;
    std::uint64_t                                baseline_submission   = 0;
    std::uint64_t                                completion_copy_id    = 0;
    std::uint64_t                                completion_swap_id    = 0;
    std::uint64_t                                completion_frame      = 0;
    std::uint64_t                                completion_submission = 0;
    std::string                                  readback_owner;
    std::string                                  failure;
};

// The caller owns one process-wide logging lifetime across all GPU runs.
int RunNativePreparedBc1Original(
    const NativePreparedBc1OriginalParams&                                           params,
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& observer,
    NativePreparedBc1OriginalResult&                                                 result,
    std::string&                                                                     error);

inline int RunNativePreparedBc1Original(const NativePreparedBc1OriginalParams& params,
                                        NativePreparedBc1OriginalResult&       result,
                                        std::string&                           error)
{
    return RunNativePreparedBc1Original(params, nullptr, result, error);
}

// Construction pieces shared by the fixture builder (native_prepared_bc1_fixture.cpp)
// and the Original oracle (native_prepared_bc1_original.cpp).
namespace bc1_parts
{

inline constexpr std::uint32_t kPageSize = 0x1000U;

inline constexpr std::uint32_t kNativePreparedBc1IndexCount = 3U;

inline constexpr auto kNativePreparedBc1IndexEndian = rex::graphics::xenos::Endian::k8in16;

inline constexpr std::uint32_t kNativePreparedBc1DrawInitiator =
    static_cast<std::uint32_t>(rex::graphics::xenos::PrimitiveType::kTriangleList) |
    (static_cast<std::uint32_t>(rex::graphics::xenos::SourceSelect::kDMA) << 6U) |
    (kNativePreparedBc1IndexCount << 16U);

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

std::vector<std::uint8_t> MakeExpectedPixels(const rex::graphics::testing::NativePreparedBc1Fixture& fixture,
                                             std::uint32_t                                           first_lod,
                                             std::uint32_t                                           second_lod);

void WriteMipStorage(std::vector<std::uint8_t>&                             storage,
                     const rex::graphics::texture_util::TextureGuestLayout& layout,
                     std::uint32_t                                          level,
                     std::uint16_t                                          color);

std::uint16_t MipColor565(const rex::graphics::testing::NativePreparedBc1Mip& mip,
                          std::uint32_t                                       level);

std::array<std::uint32_t, 6> MakeTextureFetch(std::uint32_t base_address,
                                              std::uint32_t mip_address);

std::array<std::uint32_t, 6> MakeVertexFetch(std::uint32_t address, std::uint32_t bytes);

} // namespace bc1_parts

} // namespace rex::graphics::testing
