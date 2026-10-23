#pragma once

#include "gpu/d3d12/native_frame_replay.h"
#include "gpu/d3d12/native_render_frame.h"
#include "gpu/native_gameplay_snapshot.h"
#include <rex/graphics/draw_capture_mailbox.h>

bool LoadGameplayCaptureInputs(const std::filesystem::path&                         path,
                               rex::graphics::diagnostic::DrawCaptureFrameSnapshot& snapshot,
                               std::string&                                         error);

bool LoadGameplayCaptureInputs(const std::filesystem::path&           path,
                               rerevved::gpu::NativeGameplaySnapshot& snapshot,
                               std::string&                           error);

// Loads the owned diagnostic inputs and pinned shader catalog named by a
// gameplay recipe, then applies the same admission path as live presentation.
bool LoadGameplayCaptureRecipe(const std::filesystem::path&            path,
                               rerevved::gpu::NativeFrameReplayRecipe& recipe,
                               std::string&                            error);

bool LoadGameplayRenderFrameRecipe(const std::filesystem::path&      path,
                                   rerevved::gpu::NativeRenderFrame& frame,
                                   std::string&                      error,
                                   rerevved::gpu::NativeRenderFrame* producer = nullptr);
