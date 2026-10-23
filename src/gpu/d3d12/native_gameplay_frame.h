#pragma once

#include "native_frame_replay.h"
#include "native_gameplay_draw.h"

#include <rex/graphics/draw_capture_mailbox.h>

namespace rerevved::gpu
{

struct NativeGameplayFrameCoverage
{
    std::uint64_t firstEvent       = 0;
    std::uint64_t lastEvent        = 0;
    std::uint64_t sceneResolveBase = 0;
    std::size_t   draws            = 0;
    std::size_t   textures         = 0;
};

// Checks the owned CPU inputs needed by one main-scene draw before conversion.
// Inactive signedness companions are deliberately ignored; they may retain
// diagnostic metadata without being inputs to the selected fetch.
bool ValidateNativeGameplayDrawCaptureInputs(const rex::graphics::diagnostic::DrawCaptureFrameDraw& draw,
                                             std::span<const std::uint32_t>                         textureFetchSlots,
                                             std::string&                                           error);

// Returns whether a vertex fetch payload is safe for a native scene consumer.
// Clear, resolve, and main-draw conversion all consume these bytes directly.
bool ValidateNativeGameplayVertexFetch(const rex::graphics::diagnostic::DrawCaptureVertexFetch& fetch);

// Decodes the captured Xenos sampler word into host-independent replay state.
// The anisotropy field retains the source enum, including the distinct 1x form.
bool DecodeNativeGameplaySampler(std::uint32_t            value,
                                 NativeDrawReplaySampler& sampler,
                                 std::string&             error);

// Composition and UI remain outside the scene-image contract; swap pixels never
// become native inputs.
bool BuildNativeGameplayFrameRecipe(const rex::graphics::diagnostic::DrawCaptureFrameSnapshot& snapshot,
                                    std::span<const NativeGameplayShaderInterface>             shaders,
                                    NativeFrameReplayRecipe&                                   recipe,
                                    NativeGameplayFrameCoverage&                               coverage,
                                    std::string&                                               error);

} // namespace rerevved::gpu
