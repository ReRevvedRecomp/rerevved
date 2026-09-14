#pragma once

#include <filesystem>
#include <string>

namespace rerevved::gpu::diagnostics
{

// Captures bounded, owned CPU inputs to the original indexed UP draw. The
// caller creates an arm file only after reaching the intended game state.
bool StartNativeGuestDrawCapture(const std::filesystem::path& directory, std::string& error);
void StopNativeGuestDrawCapture();

} // namespace rerevved::gpu::diagnostics
