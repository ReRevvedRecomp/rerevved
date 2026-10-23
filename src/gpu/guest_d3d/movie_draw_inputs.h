#pragma once

#include "native_draw_dispatch.h"
#include "shader_resource_literals.h"
#include "solid_draw_adapter.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// The intro movie quad: DrawPrimitiveUP 0x826A3000 from 0x82E39878. The
// nested NonindexedSink call it makes is folded into this root submission.
inline constexpr std::uint32_t kMovieDrawCaller = 0x82E3987C;
inline constexpr std::uint32_t kMovieStride     = 20;
inline constexpr std::uint32_t kMoviePrimitive  = 6; // triangle strip
inline constexpr std::uint32_t kMovieVertices   = 4;

// Translator identity of the movie pair. The word digests serialize numeric
// microcode words in little endian; the vertex digest is of the words after
// the guest's linkage and fetch patch.
inline constexpr std::uint64_t    kMovieVertexShaderHash = 0xC33871A8CFA8967AULL;
inline constexpr std::uint64_t    kMoviePixelShaderHash  = 0xA8D1E41B3FBB2D15ULL;
inline constexpr std::string_view kMovieVertexWordDigest = "e610becd7e1f7e103fb68e444ac8ddee22b14278b13ea214808182bd3c3b110c";
inline constexpr std::string_view kMoviePixelWordDigest  = "5e1c86b144a49960dba3d0cd0f86727bd268859df562dc64bb28188eca99dcce";

// Texture fetch constants 0, 1 and 2 in the device shadow: the luma plane,
// then the two half-resolution chroma planes.
inline constexpr std::array<std::uint32_t, 3> kMovieFetchShadows{ 0x480, 0x498, 0x4B0 };

// The one admitted pixel literal record: c252..c255, 16 dwords.
inline constexpr std::uint16_t kMovieLiteralFirst = 508;
inline constexpr std::uint16_t kMovieLiteralCount = 16;

struct MoviePlane
{
    std::array<std::uint32_t, 6> fetch{};     // host-order fetch words
    std::uint32_t                address = 0; // physical base of bytes
    GuestBytes                   bytes;       // the base level, guest layout
};

// Movie quad snapshot at 0x826A3000 maps nonindexed r5/r6/r7 to
// count/base/stride and keeps literals separate.
struct MovieDrawInputs : SolidDrawInputs
{
    bool                                                 movieCandidate = false; // the pixel shader is the pinned movie PS
    std::array<MoviePlane, 3>                            planes;                 // fetch constants 0, 1, 2
    std::vector<ShaderLiteralRange>                      literalRanges;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> literals; // registers 0x47F0..0x47FF
};

std::string MovieMicrocodeWordDigest(std::span<const std::uint8_t> bytes);
bool        IsMoviePixelShader(std::span<const std::uint8_t> pixelCode);

// CPU-only:other hook/caller(not 0x82E3987C), stride!=20 or PS is
// foreign(false, empty). Match sets movieCandidate; tuple/plane/literal/extent
// failures then carry errors and retained raw reads.
bool ReadMovieDrawInputs(const NativeDrawSubmission& submission, const ReadMemory& read, MovieDrawInputs& inputs, std::string& error);

} // namespace rerevved::gpu::guest_d3d
