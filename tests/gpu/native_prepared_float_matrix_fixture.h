#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "native_prepared_float_matrix_forms.h"
#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::testing
{

inline constexpr std::uint32_t kNativePreparedFloatMatrixOutputWidth   = 1280U;
inline constexpr std::uint32_t kNativePreparedFloatMatrixOutputHeight  = 720U;
inline constexpr std::uint32_t kNativePreparedFloatMatrixVertexStride  = 56U;
inline constexpr std::uint32_t kNativePreparedFloatMatrixRegisterCount = 0x5003U;

struct NativePreparedFloatMatrixMip
{
    std::uint32_t             level     = 0;
    std::uint32_t             width     = 0;
    std::uint32_t             height    = 0;
    std::uint32_t             row_pitch = 0;
    std::vector<std::uint8_t> linear_r32;
    // The bytes below are the exact guest layout for one independently stored
    // level. The base level lives in base_guest_bytes; all nonzero levels live in
    // mip_guest_bytes at their layout offsets.
    std::uint32_t guest_offset = 0;
    std::uint32_t guest_extent = 0;
};

struct NativePreparedFloatMatrixTexture
{
    std::uint32_t                             width         = kNativePreparedFloatMatrixWidth;
    std::uint32_t                             height        = kNativePreparedFloatMatrixHeight;
    std::uint32_t                             pitch         = floatMatrixForm->pitch;
    std::uint32_t                             format        = 0;
    std::uint32_t                             endianness    = 0;
    std::uint32_t                             swizzle       = 0;
    std::uint32_t                             dimension     = 0;
    bool                                      tiled         = false;
    bool                                      packed_mips   = false;
    std::uint32_t                             mip_min_level = 0;
    std::uint32_t                             mip_max_level = kNativePreparedFloatMatrixMipCount - 1U;
    std::array<std::uint32_t, 6>              fetch_words{};
    std::vector<std::uint8_t>                 base_guest_bytes;
    std::vector<std::uint8_t>                 mip_guest_bytes;
    std::vector<NativePreparedFloatMatrixMip> mips;
};

struct NativePreparedFloatMatrixFixture
{
    std::uint32_t                width           = FloatMatrixViewportWidth();
    std::uint32_t                height          = FloatMatrixViewportHeight();
    std::uint32_t                terminal_stride = kNativePreparedFloatMatrixOutputWidth * 4U;
    std::uint32_t                vertex_stride   = kNativePreparedFloatMatrixVertexStride;
    std::array<std::uint32_t, 2> selected_lods   = { 0U, 0U };

    // Raw guest words and the complete register image are owned by the fixture
    // so a catalog/native consumer can use exactly the same input identity.
    std::vector<std::uint32_t>       vertex_shader_words;
    std::vector<std::uint32_t>       pixel_shader_words;
    std::vector<std::uint32_t>       register_image;
    std::array<std::uint32_t, 6>     vertex_fetch_words{};
    std::vector<std::uint8_t>        vertex_bytes;
    std::vector<std::uint8_t>        index_bytes;
    NativePreparedFloatMatrixTexture texture;

    // RGB reads R/8 + 1/4 from two identical samples.
    // Presenter alpha is 255; only the selected pair owns its output canvas.
    std::vector<std::vector<std::uint8_t>> expected_by_lod_pair;
    std::vector<std::uint8_t>              expected_selected_pixels;
};

NativePreparedFloatMatrixFixture MakeNativePreparedFloatMatrixFixture();

enum class NativePreparedFloatMatrixUpdateKind : std::uint32_t
{
    kCombined = 1,
};

// Returns an empty vector for an invalid level. This derives the expected
// pixels from the fixture's current mip bytes, allowing mutation checks to use
// the same owned source data that is sent to the original GPU.
std::vector<std::uint8_t> ExpectedNativePreparedFloatMatrixPixels(const NativePreparedFloatMatrixFixture& fixture,
                                                                  std::uint32_t                           first_lod,
                                                                  std::uint32_t                           second_lod);

struct NativePreparedFloatMatrixOriginalParams
{
    NativePreparedFloatMatrixFixture    fixture;
    std::uint32_t                       first_lod   = 0U;
    std::uint32_t                       second_lod  = 0U;
    NativePreparedFloatMatrixUpdateKind update_kind = NativePreparedFloatMatrixUpdateKind::kCombined;
};

struct NativePreparedFloatMatrixDrawRecord
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

struct NativePreparedFloatMatrixCopyRecord
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

struct NativePreparedFloatMatrixSwapRecord
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

struct NativePreparedFloatMatrixFrameEndRecord
{
    std::uint64_t event_ordinal      = 0;
    std::uint64_t frame              = 0;
    std::uint64_t submission         = 0;
    bool          original_succeeded = false;
};

struct NativePreparedFloatMatrixOriginalResult
{
    bool                                progressed                     = false;
    bool                                completion_observed            = false;
    bool                                baseline_completion_observed   = false;
    bool                                baseline_terminal_capture      = false;
    bool                                cpu_update_observed            = false;
    bool                                terminal_capture               = false;
    bool                                actual_readback_owner          = false;
    bool                                cpu_partial_overlap_writes     = false;
    bool                                source_raw_bytes_frozen        = false;
    std::uint32_t                       cpu_update_level               = 0;
    bool                                source_draw_issued             = false;
    bool                                terminal_pixels_match_expected = false;
    bool                                cpu_update_overlap_written     = false;
    bool                                cpu_update_outside_unchanged   = false;
    std::uint32_t                       terminal_width                 = 0;
    std::uint32_t                       terminal_height                = 0;
    std::uint32_t                       terminal_stride                = 0;
    std::uint32_t                       cpu_update_guest_offset        = 0;
    std::uint32_t                       cpu_update_guest_length        = 0;
    NativePreparedFloatMatrixUpdateKind update_kind                    = NativePreparedFloatMatrixUpdateKind::kCombined;
    std::vector<std::uint8_t>           baseline_terminal_pixels;
    std::vector<std::uint8_t>           baseline_expected_pixels;
    std::vector<std::uint8_t>           terminal_pixels;
    std::vector<std::uint8_t>           expected_pixels;
    std::vector<std::uint8_t>           old_base_bytes;
    std::vector<std::uint8_t>           old_mip_bytes;
    std::vector<std::uint8_t>           new_base_bytes;
    std::vector<std::uint8_t>           new_mip_bytes;
    // The fixture after parameterized LOD bytes and guest mip storage have
    // been applied. Native replay can consume this exact source identity.
    NativePreparedFloatMatrixFixture source_fixture;

    // These are immutable copies taken from callback views before the views are
    // released. Addresses remain process-local, while words and register banks
    // preserve the native comparison identity.
    std::vector<NativePreparedFloatMatrixDrawRecord>     draws;
    std::vector<NativePreparedFloatMatrixDrawRecord>     prepared_draws;
    std::vector<NativePreparedFloatMatrixCopyRecord>     copies;
    std::vector<NativePreparedFloatMatrixCopyRecord>     prepared_copies;
    std::vector<NativePreparedFloatMatrixSwapRecord>     swaps;
    std::vector<NativePreparedFloatMatrixSwapRecord>     prepared_swaps;
    std::vector<NativePreparedFloatMatrixFrameEndRecord> frame_ends;
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
int RunNativePreparedFloatMatrixOriginal(
    NativePreparedFloatMatrixOriginalParams&                                         params,
    const std::shared_ptr<rex::graphics::diagnostic::DrawCaptureFrameInputObserver>& observer,
    NativePreparedFloatMatrixOriginalResult&                                         result,
    std::string&                                                                     error);

inline int RunNativePreparedFloatMatrixOriginal(NativePreparedFloatMatrixOriginalParams& params,
                                                NativePreparedFloatMatrixOriginalResult& result,
                                                std::string&                             error)
{
    return RunNativePreparedFloatMatrixOriginal(params, nullptr, result, error);
}

// Construction pieces shared by the fixture builder (native_prepared_float_matrix_fixture.cpp)
// and the Original oracle (native_prepared_float_matrix_original.cpp).
namespace float_matrix_parts
{

inline constexpr std::uint32_t kPageSize = 0x1000U;

inline constexpr std::uint32_t kNativePreparedFloatMatrixIndexCount = 3U;

inline constexpr auto kNativePreparedFloatMatrixIndexEndian = rex::graphics::xenos::Endian::k8in16;

inline constexpr std::uint32_t kNativePreparedFloatMatrixDrawInitiator =
    static_cast<std::uint32_t>(rex::graphics::xenos::PrimitiveType::kTriangleList) |
    (static_cast<std::uint32_t>(rex::graphics::xenos::SourceSelect::kDMA) << 6U) |
    (kNativePreparedFloatMatrixIndexCount << 16U);

// Match the clean installed control VS identity rows from SDK commit
// df8ec1c544c1e8f391dcfaff295ef387a1a7fea9,
// tests/installed_prepared_control/control.cpp:kConsumerVertexConstants.
inline constexpr std::array<std::uint32_t, 36> kVertexConstants = {
    0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U, 0x3F800000U, 0U, 0U, 0U, 0U
};

inline constexpr std::array<std::uint32_t, 4> kPixelConstant0 = { 0x3D800000U, 0x3C000000U, 0x3D600000U, 0x3D800000U };

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

std::vector<std::uint8_t> MakeExpectedPixels(const rex::graphics::testing::NativePreparedFloatMatrixFixture& fixture,
                                             std::uint32_t                                                   first_lod,
                                             std::uint32_t                                                   second_lod);

void WriteMipStorage(std::vector<std::uint8_t>& storage,
                     const rex::graphics::texture_util::TextureGuestLayout&,
                     std::uint32_t                                               level,
                     const rex::graphics::testing::NativePreparedFloatMatrixMip& mip);

std::array<std::uint32_t, 6> MakeTextureFetch(std::uint32_t base_address,
                                              std::uint32_t mip_address);

std::array<std::uint32_t, 6> MakeVertexFetch(std::uint32_t address, std::uint32_t bytes);

} // namespace float_matrix_parts

} // namespace rex::graphics::testing
