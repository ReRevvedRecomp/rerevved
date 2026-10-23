#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tools/rgba8_matrix_fixture.h"
#include <rex/graphics/draw_capture_mailbox.h>

namespace rex::graphics::testing
{

struct R8RegionWrite
{
    std::uint32_t source_width, source_height;
    std::uint32_t left, top, right, bottom, destination_x, destination_y;
};

// Source LTRB and destination XY are distinct coordinate spaces.
inline constexpr std::array<R8RegionWrite, 3> kR8RegionWrites{ { { 192, 160, 7, 11, 135, 139, 96, 128 },
                                                                 { 640, 640, 17, 23, 593, 599, 416, 416 },
                                                                 { 192, 192, 9, 5, 169, 165, 352, 336 } } };

inline std::vector<std::uint8_t> R8RegionSource(std::uint32_t index)
{
    const auto&               r = kR8RegionWrites.at(index);
    std::vector<std::uint8_t> source(std::size_t(r.source_width) * r.source_height);
    for (std::uint32_t y = 0; y < r.source_height; ++y)
        for (std::uint32_t x = 0; x < r.source_width; ++x)
            source[std::size_t(y) * r.source_width + x] = 8U + ((x * 3U + y * 5U + index * 2U) % 7U);
    return source;
}

// Scalar reference preserves every byte not addressed by this source rectangle.
inline void ModelR8Region(std::vector<std::uint8_t>& backing, std::uint32_t index)
{
    const auto& r      = kR8RegionWrites.at(index);
    const auto  source = R8RegionSource(index);
    for (auto sy = r.top; sy < r.bottom; ++sy)
        for (auto sx = r.left; sx < r.right; ++sx)
            backing.at(std::size_t(r.destination_y + sy - r.top) * 1024U + r.destination_x + sx - r.left) =
                source.at(std::size_t(sy) * r.source_width + sx);
}

enum class NativePreparedRgba8MatrixUpdateKind : std::uint32_t
{
    kCombined = 1,
};

struct NativePreparedRgba8MatrixOriginalParams
{
    NativePreparedRgba8MatrixFixture    fixture;
    std::uint32_t                       first_lod   = 0U;
    std::uint32_t                       second_lod  = 0U;
    NativePreparedRgba8MatrixUpdateKind update_kind = NativePreparedRgba8MatrixUpdateKind::kCombined;
};

struct NativePreparedRgba8MatrixDrawRecord
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

struct NativePreparedRgba8MatrixCopyRecord
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

struct NativePreparedRgba8MatrixSwapRecord
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

struct NativePreparedRgba8MatrixFrameEndRecord
{
    std::uint64_t event_ordinal      = 0;
    std::uint64_t frame              = 0;
    std::uint64_t submission         = 0;
    bool          original_succeeded = false;
};

struct NativePreparedRgba8MatrixOriginalResult
{
    std::vector<std::vector<std::uint8_t>> region_snapshots;
    bool                                   progressed                     = false;
    bool                                   completion_observed            = false;
    bool                                   baseline_completion_observed   = false;
    bool                                   baseline_terminal_capture      = false;
    bool                                   cpu_update_observed            = false;
    bool                                   terminal_capture               = false;
    bool                                   actual_readback_owner          = false;
    bool                                   cpu_partial_overlap_writes     = false;
    bool                                   source_raw_bytes_frozen        = false;
    std::uint32_t                          cpu_update_level               = 0;
    bool                                   source_draw_issued             = false;
    bool                                   terminal_pixels_match_expected = false;
    bool                                   cpu_update_overlap_written     = false;
    bool                                   cpu_update_outside_unchanged   = false;
    std::uint32_t                          terminal_width                 = 0;
    std::uint32_t                          terminal_height                = 0;
    std::uint32_t                          terminal_stride                = 0;
    std::uint32_t                          cpu_update_guest_offset        = 0;
    std::uint32_t                          cpu_update_guest_length        = 0;
    NativePreparedRgba8MatrixUpdateKind    update_kind                    = NativePreparedRgba8MatrixUpdateKind::kCombined;
    std::vector<std::uint8_t>              baseline_terminal_pixels;
    std::vector<std::uint8_t>              baseline_expected_pixels;
    std::vector<std::uint8_t>              terminal_pixels;
    std::vector<std::uint8_t>              expected_pixels;
    std::vector<std::uint8_t>              old_base_bytes;
    std::vector<std::uint8_t>              old_mip_bytes;
    std::vector<std::uint8_t>              new_base_bytes;
    std::vector<std::uint8_t>              new_mip_bytes;
    // The fixture after parameterized LOD bytes and guest mip storage have
    // been applied. Native replay can consume this exact source identity.
    NativePreparedRgba8MatrixFixture source_fixture;

    // These are immutable copies taken from callback views before the views are
    // released. Addresses remain process-local, while words and register banks
    // preserve the native comparison identity.
    std::vector<NativePreparedRgba8MatrixDrawRecord>     draws;
    std::vector<NativePreparedRgba8MatrixDrawRecord>     prepared_draws;
    std::vector<NativePreparedRgba8MatrixCopyRecord>     copies;
    std::vector<NativePreparedRgba8MatrixCopyRecord>     prepared_copies;
    std::vector<NativePreparedRgba8MatrixSwapRecord>     swaps;
    std::vector<NativePreparedRgba8MatrixSwapRecord>     prepared_swaps;
    std::vector<NativePreparedRgba8MatrixFrameEndRecord> frame_ends;
    std::vector<std::uint32_t>                           source_registers;
    std::array<std::uint32_t, 6>                         source_texture_fetch_words{};
    std::array<std::uint32_t, 6>                         source_vertex_fetch_words{};
    std::uint32_t                                        source_index_guest_base = 0;
    std::uint32_t                                        published_word_count    = 0;
    std::string                                          runtime_module_path;
    std::string                                          gpu_module_path;
    std::uint64_t                                        completed_fence       = 0;
    std::uint64_t                                        baseline_copy_id      = 0;
    std::uint64_t                                        baseline_swap_id      = 0;
    std::uint64_t                                        baseline_frame        = 0;
    std::uint64_t                                        baseline_submission   = 0;
    std::uint64_t                                        completion_copy_id    = 0;
    std::uint64_t                                        completion_swap_id    = 0;
    std::uint64_t                                        completion_frame      = 0;
    std::uint64_t                                        completion_submission = 0;
    std::string                                          readback_owner;
    std::string                                          failure;
};

// The caller owns one process-wide logging lifetime across all GPU runs.
// Consumes params.fixture into result so full decoded levels have one owner.
int RunNativePreparedRgba8MatrixOriginal(
    NativePreparedRgba8MatrixOriginalParams&                                         params,
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& observer,
    NativePreparedRgba8MatrixOriginalResult&                                         result,
    std::string&                                                                     error);

inline int RunNativePreparedRgba8MatrixOriginal(NativePreparedRgba8MatrixOriginalParams& params,
                                                NativePreparedRgba8MatrixOriginalResult& result,
                                                std::string&                             error)
{
    return RunNativePreparedRgba8MatrixOriginal(params, nullptr, result, error);
}

} // namespace rex::graphics::testing
