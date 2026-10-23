#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rex/graphics/draw_capture_mailbox.h>

namespace rex::graphics::testing
{

inline constexpr std::uint32_t                kNativePreparedBc1RectWidth                 = 256U;
inline constexpr std::uint32_t                kNativePreparedBc1RectHeight                = 128U;
inline constexpr std::uint32_t                kNativePreparedBc1RectOutputWidth           = 1280U;
inline constexpr std::uint32_t                kNativePreparedBc1RectOutputHeight          = 720U;
inline constexpr std::uint32_t                kNativePreparedBc1RectMipCount              = 9U;
inline constexpr std::uint32_t                kNativePreparedBc1RectVertexStride          = 56U;
inline constexpr std::uint32_t                kNativePreparedBc1RectRegisterCount         = 0x5003U;
inline constexpr std::uint32_t                kNativePreparedBc1RectBaseExtent            = 0x4000U;
inline constexpr std::uint32_t                kNativePreparedBc1RectMipExtent             = 0x4740U;
inline constexpr std::uint32_t                kNativePreparedBc1RectLegacyMipExtent       = 0x4000U;
inline constexpr std::uint32_t                kNativePreparedBc1RectTailOffset            = 0x4000U;
inline constexpr std::uint32_t                kNativePreparedBc1RectTailExtent            = 0x740U;
inline constexpr std::array<std::uint32_t, 6> kNativePreparedBc1RectSelectedRawFetchWords = {
    0x02000002U, 0x169F8052U, 0x000FE0FFU, 0x00A80D10U, 0x00000203U, 0x169F2A18U
};
inline constexpr std::array<std::uint32_t, kNativePreparedBc1RectMipCount> kNativePreparedBc1RectWidths = {
    256U, 128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U
};
inline constexpr std::array<std::uint32_t, kNativePreparedBc1RectMipCount> kNativePreparedBc1RectHeights = {
    128U, 64U, 32U, 16U, 8U, 4U, 2U, 1U, 1U
};
// Exact linear BC1 guest locations for the selected 256x128 packed layout.
// Levels 1 and 2 are ordinary mip allocations; levels 3..8 share the tail.
inline constexpr std::array<std::uint32_t, kNativePreparedBc1RectMipCount> kNativePreparedBc1RectGuestOffsets = {
    0x0000U, 0x0000U, 0x2000U, 0x4400U, 0x4200U, 0x4100U, 0x4020U, 0x4010U, 0x4008U
};
inline constexpr std::array<std::uint32_t, kNativePreparedBc1RectMipCount> kNativePreparedBc1RectRowPitches = {
    0x0200U, 0x0100U, 0x0100U, 0x0100U, 0x0100U, 0x0100U, 0x0100U, 0x0100U, 0x0100U
};

struct NativePreparedBc1RectMip
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

struct NativePreparedBc1RectTexture
{
    std::uint32_t                         width         = kNativePreparedBc1RectWidth;
    std::uint32_t                         height        = kNativePreparedBc1RectHeight;
    std::uint32_t                         pitch         = 8U;
    std::uint32_t                         format        = 0;
    std::uint32_t                         endianness    = 0;
    std::uint32_t                         swizzle       = 0;
    std::uint32_t                         dimension     = 0;
    bool                                  tiled         = false;
    bool                                  packed_mips   = false;
    std::uint32_t                         mip_min_level = 0;
    std::uint32_t                         mip_max_level = kNativePreparedBc1RectMipCount - 1U;
    std::array<std::uint32_t, 6>          fetch_words{};
    std::vector<std::uint8_t>             base_guest_bytes;
    std::vector<std::uint8_t>             mip_guest_bytes;
    std::vector<NativePreparedBc1RectMip> mips;
};

struct NativePreparedBc1RectFixture
{
    std::uint32_t                width           = kNativePreparedBc1RectWidth;
    std::uint32_t                height          = kNativePreparedBc1RectHeight;
    std::uint32_t                terminal_stride = kNativePreparedBc1RectOutputWidth * 4U;
    std::uint32_t                vertex_stride   = kNativePreparedBc1RectVertexStride;
    std::array<std::uint32_t, 2> selected_lods   = { 3U, 8U };

    // Raw guest words and the complete register image are owned by the fixture
    // so a catalog/native consumer can use exactly the same input identity.
    std::vector<std::uint32_t>   vertex_shader_words;
    std::vector<std::uint32_t>   pixel_shader_words;
    std::vector<std::uint32_t>   register_image;
    std::array<std::uint32_t, 6> vertex_fetch_words{};
    std::vector<std::uint8_t>    vertex_bytes;
    std::vector<std::uint8_t>    index_bytes;
    NativePreparedBc1RectTexture texture;

    // Selected plane is a byte-exact RGBA8 sum; only selected entry owns full
    // canvas.
    std::vector<std::vector<std::uint8_t>> expected_by_lod_pair;
    std::vector<std::uint8_t>              expected_selected_pixels;
};

NativePreparedBc1RectFixture MakeNativePreparedBc1RectFixture();

// Returns an empty vector for an invalid level. This derives the expected
// pixels from the fixture's current mip bytes, allowing mutation checks to use
// the same owned source data that is sent to the original GPU.
std::vector<std::uint8_t> ExpectedNativePreparedBc1RectPixels(const NativePreparedBc1RectFixture& fixture,
                                                              std::uint32_t                       first_lod,
                                                              std::uint32_t                       second_lod);

struct NativePreparedBc1RectOriginalParams
{
    NativePreparedBc1RectFixture fixture    = MakeNativePreparedBc1RectFixture();
    std::uint32_t                first_lod  = 3U;
    std::uint32_t                second_lod = 8U;
};

struct NativePreparedBc1RectDrawRecord
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

struct NativePreparedBc1RectCopyRecord
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

struct NativePreparedBc1RectSwapRecord
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

struct NativePreparedBc1RectFrameEndRecord
{
    std::uint64_t event_ordinal      = 0;
    std::uint64_t frame              = 0;
    std::uint64_t submission         = 0;
    bool          original_succeeded = false;
};

struct NativePreparedBc1RectOriginalResult
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
    NativePreparedBc1RectFixture source_fixture;

    // These are immutable copies taken from callback views before the views are
    // released. Addresses remain process-local, while words and register banks
    // preserve the native comparison identity.
    std::vector<NativePreparedBc1RectDrawRecord>     draws;
    std::vector<NativePreparedBc1RectDrawRecord>     prepared_draws;
    std::vector<NativePreparedBc1RectCopyRecord>     copies;
    std::vector<NativePreparedBc1RectCopyRecord>     prepared_copies;
    std::vector<NativePreparedBc1RectSwapRecord>     swaps;
    std::vector<NativePreparedBc1RectSwapRecord>     prepared_swaps;
    std::vector<NativePreparedBc1RectFrameEndRecord> frame_ends;
    std::vector<std::uint32_t>                       source_registers;
    std::array<std::uint32_t, 6>                     source_texture_fetch_words{};
    std::array<std::uint32_t, 6>                     source_vertex_fetch_words{};
    std::uint32_t                                    source_index_guest_base = 0;
    std::uint32_t                                    published_word_count    = 0;
    std::string                                      runtime_module_path;
    std::string                                      gpu_module_path;
    std::uint64_t                                    completed_fence       = 0;
    std::uint64_t                                    baseline_copy_id      = 0;
    std::uint64_t                                    baseline_swap_id      = 0;
    std::uint64_t                                    baseline_frame        = 0;
    std::uint64_t                                    baseline_submission   = 0;
    std::uint64_t                                    completion_copy_id    = 0;
    std::uint64_t                                    completion_swap_id    = 0;
    std::uint64_t                                    completion_frame      = 0;
    std::uint64_t                                    completion_submission = 0;
    std::string                                      readback_owner;
    std::string                                      failure;
};

// The caller owns one process-wide logging lifetime across all GPU runs.
int RunNativePreparedBc1RectOriginal(
    const NativePreparedBc1RectOriginalParams&                                       params,
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& observer,
    NativePreparedBc1RectOriginalResult&                                             result,
    std::string&                                                                     error);

inline int RunNativePreparedBc1RectOriginal(const NativePreparedBc1RectOriginalParams& params,
                                            NativePreparedBc1RectOriginalResult&       result,
                                            std::string&                               error)
{
    return RunNativePreparedBc1RectOriginal(params, nullptr, result, error);
}

} // namespace rex::graphics::testing
