#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rex/graphics/draw_capture_mailbox.h>

namespace rex::graphics::testing
{

inline constexpr std::uint32_t                                             kNativePreparedBc2RectWidth         = 128U;
inline constexpr std::uint32_t                                             kNativePreparedBc2RectHeight        = 256U;
inline constexpr std::uint32_t                                             kNativePreparedBc2RectOutputWidth   = 1280U;
inline constexpr std::uint32_t                                             kNativePreparedBc2RectOutputHeight  = 720U;
inline constexpr std::uint32_t                                             kNativePreparedBc2RectMipCount      = 9U;
inline constexpr std::uint32_t                                             kNativePreparedBc2RectVertexStride  = 56U;
inline constexpr std::uint32_t                                             kNativePreparedBc2RectRegisterCount = 0x5003U;
inline constexpr std::uint32_t                                             kNativePreparedBc2RectBaseExtent    = 0x8000U;
inline constexpr std::uint32_t                                             kNativePreparedBc2RectMipExtent     = 0x8E80U;
inline constexpr std::uint32_t                                             kNativePreparedBc2RectTailOffset    = 0x8000U;
inline constexpr std::uint32_t                                             kNativePreparedBc2RectTailExtent    = 0xE80U;
inline constexpr std::array<std::uint32_t, kNativePreparedBc2RectMipCount> kNativePreparedBc2RectWidths        = {
    128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U, 1U
};
inline constexpr std::array<std::uint32_t, kNativePreparedBc2RectMipCount> kNativePreparedBc2RectHeights = {
    256U, 128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U
};
// Exact linear BC2 guest locations for the selected 128x256 packed layout.
// Levels 1 and 2 are ordinary mip allocations; levels 3..8 share the tail.
inline constexpr std::array<std::uint32_t, kNativePreparedBc2RectMipCount> kNativePreparedBc2RectGuestOffsets = {
    0x0000U, 0x0000U, 0x4000U, 0x8040U, 0x8020U, 0x8010U, 0x8800U, 0x8400U, 0x8200U
};
inline constexpr std::array<std::uint32_t, kNativePreparedBc2RectMipCount> kNativePreparedBc2RectRowPitches = {
    0x0200U, 0x0200U, 0x0200U, 0x0200U, 0x0200U, 0x0200U, 0x0200U, 0x0200U, 0x0200U
};
// Retained words from the selected source descriptor. Physical address words
// remain process-local in the synthetic PM4 fixture and are patched at issue.
inline constexpr std::array<std::uint32_t, 6> kNativePreparedBc2RectSelectedRawFetchWords = {
    0x01000002U, 0x168B6053U, 0x001FE07FU, 0x00A80D10U, 0x00000203U, 0x168AAA18U
};

struct NativePreparedBc2Mip
{
    std::uint32_t             level     = 0;
    std::uint32_t             width     = 0;
    std::uint32_t             height    = 0;
    std::uint32_t             row_pitch = 0;
    std::vector<std::uint8_t> linear_rgba8;
    // The bytes below are the exact guest layout for one independently stored
    // level. The base level lives in base_guest_bytes; levels 1..8 live in
    // mip_guest_bytes at their layout offsets.
    std::uint32_t guest_offset = 0;
    std::uint32_t guest_extent = 0;
};

struct NativePreparedBc2Texture
{
    std::uint32_t                     width         = kNativePreparedBc2RectWidth;
    std::uint32_t                     height        = kNativePreparedBc2RectHeight;
    std::uint32_t                     pitch         = 4U;
    std::uint32_t                     format        = 0;
    std::uint32_t                     endianness    = 0;
    std::uint32_t                     swizzle       = 0;
    std::uint32_t                     dimension     = 0;
    bool                              tiled         = false;
    bool                              packed_mips   = false;
    std::uint32_t                     mip_min_level = 0;
    std::uint32_t                     mip_max_level = kNativePreparedBc2RectMipCount - 1U;
    std::array<std::uint32_t, 6>      fetch_words{};
    std::vector<std::uint8_t>         base_guest_bytes;
    std::vector<std::uint8_t>         mip_guest_bytes;
    std::vector<NativePreparedBc2Mip> mips;
};

struct NativePreparedBc2Fixture
{
    std::uint32_t                width           = kNativePreparedBc2RectWidth;
    std::uint32_t                height          = kNativePreparedBc2RectHeight;
    std::uint32_t                terminal_stride = kNativePreparedBc2RectOutputWidth * 4U;
    std::uint32_t                vertex_stride   = kNativePreparedBc2RectVertexStride;
    std::array<std::uint32_t, 2> selected_lods   = { 1U, 4U };

    // Raw guest words and the complete register image are owned by the fixture
    // so a catalog/native consumer can use exactly the same input identity.
    std::vector<std::uint32_t>   vertex_shader_words;
    std::vector<std::uint32_t>   pixel_shader_words;
    std::vector<std::uint32_t>   register_image;
    std::array<std::uint32_t, 6> vertex_fetch_words{};
    std::vector<std::uint8_t>    vertex_bytes;
    std::vector<std::uint8_t>    index_bytes;
    NativePreparedBc2Texture     texture;

    // Selected plane blends two BC2 mip patterns; presenter normalizes alpha to
    // 255.
    std::vector<std::vector<std::uint8_t>> expected_by_lod_pair;
    std::vector<std::uint8_t>              expected_selected_pixels;
};

NativePreparedBc2Fixture MakeNativePreparedBc2RectFixture();

enum class NativePreparedBc2UpdateKind : std::uint32_t
{
    kBaseline = 0,
    kCombined,
    kColorOnly,
    kAlphaOnly,
};

// Returns an empty vector for an invalid level. This derives the expected
// pixels from the fixture's current mip bytes, allowing mutation checks to use
// the same owned source data that is sent to the original GPU.
std::vector<std::uint8_t> ExpectedNativePreparedBc2RectPixels(const NativePreparedBc2Fixture& fixture,
                                                              std::uint32_t                   first_lod,
                                                              std::uint32_t                   second_lod);

struct NativePreparedBc2OriginalParams
{
    NativePreparedBc2Fixture    fixture     = MakeNativePreparedBc2RectFixture();
    std::uint32_t               first_lod   = 1U;
    std::uint32_t               second_lod  = 4U;
    NativePreparedBc2UpdateKind update_kind = NativePreparedBc2UpdateKind::kCombined;
};

struct NativePreparedBc2DrawRecord
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

struct NativePreparedBc2CopyRecord
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

struct NativePreparedBc2SwapRecord
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

struct NativePreparedBc2FrameEndRecord
{
    std::uint64_t event_ordinal      = 0;
    std::uint64_t frame              = 0;
    std::uint64_t submission         = 0;
    bool          original_succeeded = false;
};

struct NativePreparedBc2OriginalResult
{
    bool                        progressed                     = false;
    bool                        completion_observed            = false;
    bool                        baseline_completion_observed   = false;
    bool                        baseline_terminal_capture      = false;
    bool                        cpu_update_observed            = false;
    bool                        terminal_capture               = false;
    bool                        actual_readback_owner          = false;
    bool                        cpu_partial_overlap_writes     = false;
    bool                        source_raw_bytes_frozen        = false;
    std::uint32_t               cpu_update_level               = 0;
    bool                        source_draw_issued             = false;
    bool                        terminal_pixels_match_expected = false;
    bool                        cpu_update_overlap_written     = false;
    bool                        cpu_update_outside_unchanged   = false;
    bool                        cpu_update_opposite_unchanged  = false;
    std::uint32_t               terminal_width                 = 0;
    std::uint32_t               terminal_height                = 0;
    std::uint32_t               terminal_stride                = 0;
    std::uint32_t               cpu_update_guest_offset        = 0;
    std::uint32_t               cpu_update_guest_length        = 0;
    NativePreparedBc2UpdateKind update_kind                    = NativePreparedBc2UpdateKind::kCombined;
    std::vector<std::uint8_t>   baseline_terminal_pixels;
    std::vector<std::uint8_t>   baseline_expected_pixels;
    std::vector<std::uint8_t>   terminal_pixels;
    std::vector<std::uint8_t>   expected_pixels;
    std::vector<std::uint8_t>   old_base_bytes;
    std::vector<std::uint8_t>   old_mip_bytes;
    std::vector<std::uint8_t>   new_base_bytes;
    std::vector<std::uint8_t>   new_mip_bytes;
    // The fixture after parameterized LOD bytes and guest mip storage have
    // been applied. Native replay can consume this exact source identity.
    NativePreparedBc2Fixture source_fixture;

    // These are immutable copies taken from callback views before the views are
    // released. Addresses remain process-local, while words and register banks
    // preserve the native comparison identity.
    std::vector<NativePreparedBc2DrawRecord>     draws;
    std::vector<NativePreparedBc2DrawRecord>     prepared_draws;
    std::vector<NativePreparedBc2CopyRecord>     copies;
    std::vector<NativePreparedBc2CopyRecord>     prepared_copies;
    std::vector<NativePreparedBc2SwapRecord>     swaps;
    std::vector<NativePreparedBc2SwapRecord>     prepared_swaps;
    std::vector<NativePreparedBc2FrameEndRecord> frame_ends;
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
int RunNativePreparedBc2RectOriginal(
    const NativePreparedBc2OriginalParams&                                           params,
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& observer,
    NativePreparedBc2OriginalResult&                                                 result,
    std::string&                                                                     error);

inline int RunNativePreparedBc2RectOriginal(const NativePreparedBc2OriginalParams& params,
                                            NativePreparedBc2OriginalResult&       result,
                                            std::string&                           error)
{
    return RunNativePreparedBc2RectOriginal(params, nullptr, result, error);
}

} // namespace rex::graphics::testing
