#pragma once

#include "native_draw_dispatch.h"
#include "solid_draw_adapter.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// The GFx text batches are the only nonindexed-UP draws returning to this site.
inline constexpr std::uint32_t kLabelDrawCaller = 0x82304258;
inline constexpr std::uint32_t kLabelStride     = 28;

// Translator identity of the label pair (native_menu_draw.cpp:41). The word
// digests serialize numeric microcode words in little endian.
inline constexpr std::uint64_t    kLabelVertexShaderHash = 0x2BA2325A7EA93DE3ULL;
inline constexpr std::uint64_t    kLabelPixelShaderHash  = 0xC3BEC99768EF0D6BULL;
inline constexpr std::string_view kLabelVertexWordDigest = "80e55d6dee0e08d066bbfdb6bdc6f1ee077f27f04862c57cce91decc76b113b4";
inline constexpr std::string_view kLabelPixelWordDigest  = "da7bca278e09ebf26d765b3dfc7a755a47fb04ef955476f352028ddfbaaa488b";

// Label batch snapshot is taken at 0x826A3000; nonindexed tuple maps r5/r6/r7
// to vertex count/base/stride.
struct LabelDrawInputs : SolidDrawInputs
{
    bool                         labelCandidate = false; // the pixel shader is the pinned label PS
    std::array<std::uint32_t, 6> textureFetch{};         // G+0x480, host-order words
    std::uint32_t                textureAddress = 0;     // physical base of textureBytes
    GuestBytes                   textureBytes;           // the fetch's base level, guest layout
};

// Catalog word digest of guest microcode bytes in guest (big-endian) order.
std::string LabelMicrocodeWordDigest(std::span<const std::uint8_t> bytes);
bool        IsLabelPixelShader(std::span<const std::uint8_t> pixelCode);

// Label input reads CPU state only; foreign calls return false with an empty
// error.
bool ReadLabelDrawInputs(const NativeDrawSubmission& submission, const ReadMemory& read, LabelDrawInputs& inputs, std::string& error);
// The same with an explicit pixel shader word digest, for fixtures whose
// microcode is not the retail pixel shader.
bool ReadLabelDrawInputsForDigest(std::string_view pixelWordDigest, const NativeDrawSubmission& submission, const ReadMemory& read, LabelDrawInputs& inputs, std::string& error);

// The root must not exist. Load checks the recorded texture digest and extent.
bool SaveLabelDrawInputs(const std::filesystem::path& root, const LabelDrawInputs& inputs, std::string& error);
bool LoadLabelDrawInputs(const std::filesystem::path& root, LabelDrawInputs& inputs, std::string& error);

} // namespace rerevved::gpu::guest_d3d
