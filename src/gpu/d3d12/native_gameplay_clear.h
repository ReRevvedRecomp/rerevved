#pragma once

#include <span>

#include "native_render_frame.h"

#include <rex/graphics/draw_capture_mailbox.h>

namespace rerevved::gpu
{

// Normalize the title's rectangle clear shader and EDRAM sample aliases.
// Attachment base identity and preservation across events belong to the caller.
bool BuildNativeGameplayClearPass(const rex::graphics::diagnostic::DrawCaptureSnapshot& draw,
                                  NativeRenderFramePass&                                pass,
                                  std::string&                                          error);
bool BuildNativeGameplayClearPass(const rex::graphics::diagnostic::DrawCaptureSnapshot& draw,
                                  std::span<const std::uint32_t>                        registers,
                                  NativeRenderFramePass&                                pass,
                                  std::string&                                          error);

} // namespace rerevved::gpu
