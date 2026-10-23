#pragma once

#include "solid_draw_adapter.h"

#include <rex/graphics/draw_capture_mailbox.h>

namespace rerevved::gpu::guest_d3d
{

struct SolidReferenceCorrelation
{
    bool          addressCorrelated = false;
    std::uint32_t adapterVertexBase = 0, adapterIndexBase = 0;
    std::uint32_t referenceVertexBase = 0, referenceIndexBase = 0;
};

// The pinned solid VS reads c0..c3. Conservatively retain c0..c15 and
// every captured PS, boolean and loop constant, excluding the unused VS tail.
inline constexpr std::array<std::array<std::uint32_t, 3>, 3> kSolidReferenceConstants{
    { { 0x780, 0x4000, 16 * 4 }, { 0x1780, 0x4400, 224 * 4 }, { 0x2780, 0x4900, 40 } }
};

bool CorrelateSolidDrawReference(const rex::graphics::diagnostic::DrawCaptureSnapshot& reference,
                                 const SolidDrawInputs&                                inputs,
                                 const SolidDrawRecipe&                                recipe,
                                 std::uint32_t                                         vertexOutput,
                                 std::uint32_t                                         indexOutput,
                                 SolidReferenceCorrelation&                            correlation,
                                 std::string&                                          error);

// Both mailbox and disk snapshots pass through this validation and writer.
// The output must be fresh; event.toml is written last as the completion record.
bool ProcessSolidDrawReference(const rex::graphics::diagnostic::DrawCaptureSnapshot& reference,
                               const SolidDrawInputs&                                inputs,
                               const SolidDrawRecipe&                                recipe,
                               std::uint32_t                                         vertexOutput,
                               std::uint32_t                                         indexOutput,
                               const std::filesystem::path&                          output,
                               bool                                                  offline,
                               std::string&                                          error);

bool LoadSdkSolidDrawReference(const std::filesystem::path&                    directory,
                               rex::graphics::diagnostic::DrawCaptureSnapshot& reference,
                               std::string&                                    error);

} // namespace rerevved::gpu::guest_d3d
