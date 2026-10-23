#pragma once

#include "native_draw_dispatch.h"
#include "solid_draw_adapter.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Every Scaleform indexed draw: the GFx renderer draw 0x82303C38 calls
// IndexedUp here (RVA-F-0033). Panel, loading, menu and HUD quads share it.
inline constexpr std::uint32_t kGfxDrawCaller = 0x82303E90;
// The loading pair's 12-byte vertex: GFx format XY16iCF32, a SHORT2 position
// and two D3DCOLORs (declaration 0x82F6E818); texture coordinates come from
// the VS texgen constants. Format XY16i is 4 bytes (0x82F6E7AC).
inline constexpr std::uint32_t kGfxStride = 12;

// Translator identity of the loading GFx pair (native_menu_draw.cpp:40). The
// word digests serialize numeric microcode words in little endian; the vertex
// digest is of the words after the guest's fetch patch.
inline constexpr std::uint64_t    kGfxVertexShaderHash = 0x1EE55F3AB5213177ULL;
inline constexpr std::uint64_t    kGfxPixelShaderHash  = 0x47F2D46F3B8F1668ULL;
inline constexpr std::string_view kGfxVertexWordDigest = "e2cde9a88d5760b5b4291c5148306356f5182c53549dad55e2c1a30267536a03";
inline constexpr std::string_view kGfxPixelWordDigest  = "873ad152186e5cb71b2c3178f333a0468f932d34439265b434477f89e50536d2";
// The GFx VS words before the guest's first fetch patch, as the first quad
// batch of a loading frame finds them.
inline constexpr std::string_view kGfxSourceVertexWordDigest = "96aaf30e59048a45af92e0eaff193864f23afb1adcebaf0d7d3cbce8bbf23d47";
// The menu's textured stride-4 pair (native_menu_draw.cpp:39): menu buttons,
// logo and world-map HUD fills, after the guest's fetch patch.
inline constexpr std::string_view kGfxMenuVertexWordDigest = "6f1c41b00c0798638e9ac75401220379d4ed389d93a729136d50dccd34d4bad9";
inline constexpr std::string_view kGfxMenuPixelWordDigest  = "f2c35b1e6655282cedb7022de9b6374fc98069e78c930ec51dd3f6b313739e7e";

// One admitted GFx shader pair. A catalog entry with these translator hashes
// joins the family; its word digests, stride and texture slots (0 up to
// `textures`) must match.
struct GfxPairForm
{
    std::uint64_t    vertexHash, pixelHash;
    std::string_view vertexWords, pixelWords;
    std::uint32_t    stride;
    std::uint32_t    textures;
};

// 0x82302620 pairs loading one-texture Gouraud/menu bitmap/stencil solid PS.
// Stencil stride 4, color mask 0, no PS interpolator:0x826AD8E8 links menu VS
// with interpolator export disabled, using linked patched identity.
inline constexpr std::array<GfxPairForm, 5> kGfxPairs{ {
    { kGfxVertexShaderHash, kGfxPixelShaderHash, kGfxVertexWordDigest, kGfxPixelWordDigest, 12, 1 },
    { 0x5F6EB3BC96CE8FC0ULL, 0x6831098A8316F932ULL, kGfxMenuVertexWordDigest, kGfxMenuPixelWordDigest, 4, 1 },
    { 0xD3207A360ED87E80ULL, 0x54AAA6882BF85C23ULL, "0f01051f9220c3326cc84778016c0263e563d435309c1945739ac6c0bf356a4f", "548d9d6796d0dede6d1131aa80bd71e8aa8f4395cf25b72819728b0f1bace56d", 4, 0 },
    // Untextured two-color Gouraud PS uses stride 12 loading VS, linked at
    // 0x826AD8E8. Two-texture PS EA7BE45FBA3A4BA9 reads slots 0/1 with stride
    // 12 and linked VS E62E685831B79DE6.
    { 0x88253DFA6CE3E574ULL, 0x2DA1A34264D18533ULL, "0f6831323d7ade2d1a9056f9654d2079fb0162744854b2b1f2b75853de56be42", "f95b615c5cb9318bd0d8ba8b34d594b015bf7af3d2fe400d57e01cc8718a9bf1", 12, 0 },
    { 0x9413072B43F67E41ULL, 0xEA7BE45FBA3A4BA9ULL, "0d53abbb217e2b6b9eb72b0aa81d49796a76d582ad1e8b63519cb66a43dafd02", "deec18df4ddb0cfde7b25ca14585a029642b735c39bfe2bf4e1eec73ece76706", 12, 2 },
} };

struct GfxDrawShader
{
    NativeGameplayShaderInterface shader;
    std::uint32_t                 stride = 0;
    std::vector<std::uint32_t>    attributeOffsets; // declaration offsets the fetch patch admits
    std::uint32_t                 textures = 0;     // sampled fetch slots 0 up to this count
    // The loading pair's single-sample specialization, for its off-screen form.
    std::optional<NativeGameplayShaderInterface> offscreen;
};

// The GFx entries of one shader catalog, at most one per pixel word digest.
struct GfxDrawCatalog
{
    std::vector<GfxDrawShader> entries;

    const GfxDrawShader* Find(std::string_view pixelWords) const;
};

std::string GfxMicrocodeWordDigest(std::span<const std::uint8_t> bytes);

// One GFx quad batch as it stood at 0x826A3568 entry, with the solid
// adapter's field meanings and, for a textured pair, texture fetch constant 0
// and, for the two-texture pair, fetch constant 1 read the same way.
struct GfxDrawInputs : SolidDrawInputs
{
    bool                         gfxCandidate = false; // the pixel shader is an admitted catalog pair's
    std::array<std::uint32_t, 6> textureFetch{};       // G+0x480, host-order words
    std::uint32_t                textureAddress = 0;   // physical base of textureBytes
    GuestBytes                   textureBytes;         // the fetch's base level, guest layout
    std::array<std::uint32_t, 6> secondTextureFetch{}; // G+0x498
    std::uint32_t                secondTextureAddress = 0;
    GuestBytes                   secondTextureBytes;
    // The guest mip storage of a fetch whose base level only the full layout
    // describes (packed mips); empty otherwise.
    GuestBytes textureMipBytes, secondTextureMipBytes;
};

// Unreadable inputs report an error; a shader match sets gfxCandidate and
// retains raw reads.
bool ReadGfxDrawInputs(const NativeDrawSubmission& submission, const ReadMemory& read, const GfxDrawCatalog& catalog, GfxDrawInputs& inputs, std::string& error);

} // namespace rerevved::gpu::guest_d3d
