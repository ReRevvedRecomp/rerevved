#pragma once

#include <filesystem>

namespace rex::graphics::diagnostic
{

struct DrawCaptureFrameSnapshot;

}

namespace rerevved::gpu::diagnostics
{

// Saves an owned Xenos frame in diagnostic schema 1 with its SDK schema,
// independent execution/acquisition status and all captured texture subresources.
// Texture identities and generations are fixed-width uppercase hexadecimal strings.
void SaveDrawCaptureFrameInputs(const std::filesystem::path& root, const rex::graphics::diagnostic::DrawCaptureFrameSnapshot& snapshot);

} // namespace rerevved::gpu::diagnostics
