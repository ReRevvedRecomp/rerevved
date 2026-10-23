#pragma once

#include <filesystem>

namespace rex::graphics::diagnostic
{

struct DrawCaptureFrameSnapshot;

}

namespace rerevved::gpu::diagnostics
{

// Saves an owned Xenos frame snapshot under root: manifest.toml with each
// event's registers, microcode, fetches and texture uses, the textures, the
// gamma table, the pre-gamma swap source and the RGB10 output reference.
void SaveDrawCaptureFrameInputs(const std::filesystem::path& root, const rex::graphics::diagnostic::DrawCaptureFrameSnapshot& snapshot);

} // namespace rerevved::gpu::diagnostics
