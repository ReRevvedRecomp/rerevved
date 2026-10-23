#pragma once

#include "gpu/guest_d3d/label_draw_inputs.h"

#include <filesystem>
#include <memory>
#include <string>

struct PPCContext;

namespace rerevved::gpu::diagnostics
{

struct LabelAdapterCapture;

struct LabelAdapterOptions
{
    // Committed guest memory as the reader sees it; throws for any unreadable byte.
    guest_d3d::ReadMemory read;
    // Pair the capture with the SDK's Xenos draw snapshot of the same draw.
    bool xenos = true;
    // Pixel shader word digest to admit; empty is the pinned label shader.
    std::string pixelWordDigest;
};

// Captures the first pinned label batch at 0x826A3000 entry, before Original.
// It starts armed, because the first label draw comes at frame 3 of cold boot,
// and needs no shader directory. The directory must not exist.
bool                                 StartLabelAdapterCapture(const std::filesystem::path& directory, LabelAdapterOptions options, std::string& error);
void                                 StopLabelAdapterCapture();
std::shared_ptr<LabelAdapterCapture> BeginLabelAdapterDraw(const guest_d3d::NativeDrawSubmission& submission);
std::shared_ptr<LabelAdapterCapture> BeginLabelAdapterDraw(const PPCContext& ctx);
// `result` is r3 after Original returned.
void FinishLabelAdapterDraw(const std::shared_ptr<LabelAdapterCapture>& capture, std::uint32_t result);
void ObserveLabelAdapterPatch(std::uint32_t shader, std::uint32_t code, std::uint32_t patchState, std::uint32_t variant, std::uint64_t caller);
void ObserveLabelAdapterSelection(std::uint32_t graphics, std::uint32_t shader, std::uint32_t conditional, std::uint32_t variant);

} // namespace rerevved::gpu::diagnostics
