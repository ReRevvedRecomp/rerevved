#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "gpu/d3d12/native_guest_menu_draw.h"

namespace rerevved::gpu::diagnostics
{

// Captures bounded, owned CPU inputs to the original GFx and label UP draws. The
// caller creates an arm file only after reaching the intended game state.
using NativeGuestDrawConsumer  = std::function<bool(const NativeGuestMenuDraw&,
                                                    const std::filesystem::path&,
                                                    std::string&)>;
using NativeGuestFrameConsumer = std::function<bool(const std::filesystem::path&, bool, std::string&)>;
bool StartNativeGuestDrawCapture(const std::filesystem::path& directory, std::string& error, NativeGuestDrawConsumer consumer = {}, NativeGuestFrameConsumer frameConsumer = {});
void NotifyNativeGuestFrameBoundary();
void StopNativeGuestDrawCapture();

} // namespace rerevved::gpu::diagnostics
