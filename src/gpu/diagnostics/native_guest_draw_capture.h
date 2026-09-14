#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "gpu/d3d12/native_guest_menu_draw.h"

namespace rerevved::gpu::diagnostics
{

// Captures owned CPU inputs to the original menu scene, GFx and label draws. The
// caller creates an arm file only after reaching the intended game state.
using NativeGuestDrawConsumer  = std::function<bool(const NativeGuestMenuDraw&,
                                                    const std::filesystem::path&,
                                                    std::string&)>;
using NativeGuestFrameConsumer = std::function<bool(const std::filesystem::path&, bool, std::string&)>;
bool StartNativeGuestDrawCapture(const std::filesystem::path& directory, std::string& error, NativeGuestDrawConsumer consumer = {}, NativeGuestFrameConsumer frameConsumer = {});
// Called before VdSwap at 0x826A4884; records a CPU submission interval, not GPU
// completion. Draws must use this device and finish before its next boundary.
void NotifyNativeGuestFrameBoundary(std::uint32_t graphics, std::uint32_t reservation, std::uint32_t descriptor);
void StopNativeGuestDrawCapture();

} // namespace rerevved::gpu::diagnostics
