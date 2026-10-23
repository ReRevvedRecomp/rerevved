#include "movie_draw_inputs.h"

#include "gpu/d3d12/native_texture_upload.h"

#include <algorithm>
#include <cstring>
#include <rex/crypto/sha256.h>
#include <rex/graphics/xenos.h>
#include <stdexcept>

namespace rerevved::gpu::guest_d3d
{
namespace
{

constexpr std::uint64_t kMaxCodeBytes = 64 * 1024;

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("movie_input_extent");
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

void require(bool condition, const char* reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}

// The guest's physical alias calculation (0x826ADF14 for pixel shaders).
std::uint64_t physical(std::uint64_t raw, bool pixel = false)
{
    require(raw <= UINT32_MAX, "movie_shader_address_overflow");
    return (raw & (pixel ? 0x1FFFFFFEULL : 0x1FFFFFFFULL)) + (((raw >> 20) + 512) & 0x1000);
}

// 0x826AD8E8 links exports only for a mismatched, unskipped VS/PS pair.
bool needsLinkage(std::span<const std::uint8_t> vs, std::span<const std::uint8_t> ps)
{
    const auto vw = word(vs, 20), pw = word(ps, 20);
    return ((vw ^ pw) & 0x7FFFF) && (vs[8] & 7) != 7 && !(vw & 0x40000) && !(pw & 0x20000);
}

std::uint64_t codeBytes(std::span<const std::uint8_t> metadata)
{
    const auto size = word(metadata, 4);
    require(size && size % 4 == 0 && size <= kMaxCodeBytes, "movie_microcode_extent");
    return size;
}

// The luma plane is 1280x720 at pitch 1280; each chroma plane is 640x360 at
// the 256-byte aligned pitch 768.
bool planeFormat(const std::array<std::uint32_t, 6>& words, std::size_t slot)
{
    namespace xenos = rex::graphics::xenos;
    xenos::xe_gpu_texture_fetch_t fetch{};
    static_assert(sizeof(fetch) == sizeof(words));
    std::memcpy(&fetch, words.data(), sizeof(fetch));
    const std::uint32_t width = slot ? 640 : 1280, height = slot ? 360 : 720, pitch = slot ? 768 : 1280;
    return fetch.type == xenos::FetchConstantType::kTexture && fetch.dimension == xenos::DataDimension::k2DOrStacked &&
           fetch.format == xenos::TextureFormat::k_8 && !fetch.tiled && fetch.endianness == xenos::Endian::kNone &&
           !fetch.mip_max_level && !fetch.stacked && fetch.size_2d.width + 1 == width && fetch.size_2d.height + 1 == height &&
           fetch.pitch * 32 == pitch;
}

} // namespace

std::string MovieMicrocodeWordDigest(std::span<const std::uint8_t> bytes)
{
    require(bytes.size() % 4 == 0, "movie_microcode_extent");
    std::vector<std::uint8_t> swapped(bytes.begin(), bytes.end());
    for (std::size_t i = 0; i < swapped.size(); i += 4)
        std::reverse(swapped.begin() + i, swapped.begin() + i + 4);
    return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(swapped.data()), swapped.size()));
}

bool IsMoviePixelShader(std::span<const std::uint8_t> pixelCode)
{
    return !pixelCode.empty() && pixelCode.size() % 4 == 0 && MovieMicrocodeWordDigest(pixelCode) == kMoviePixelWordDigest;
}

bool ReadMovieDrawInputs(const NativeDrawSubmission& submission, const ReadMemory& read, MovieDrawInputs& inputs, std::string& error)
{
    inputs = {};
    error.clear();
    // r3..r7 at entry: device, primitive, vertex count, CPU vertices, stride.
    const auto& r = submission.arguments;
    if (submission.hook != NativeDrawHook::NonindexedUp || submission.caller != kMovieDrawCaller || r[4] != kMovieStride)
        return false;
    try
    {
        inputs.arguments  = { r[0], r[1], 0, r[2], 0, 0, 0, r[3], r[4], submission.caller };
        inputs.state      = read(r[0], 0x3500, false);
        const auto& state = inputs.state;
        require(state.size() == 0x3500, "movie_shadow_extent");
        inputs.vertexObject       = word(state, 0x3198);
        inputs.pixelObject        = word(state, 0x3194);
        inputs.declarationAddress = word(state, 0x2E2C);
        if (!inputs.vertexObject || !inputs.pixelObject)
            return false;
        inputs.pixelHeader = read(inputs.pixelObject, 68, false);
        require(inputs.pixelHeader.size() == 68, "movie_shader_header_extent");
        const auto psHeader          = std::span<const std::uint8_t>(inputs.pixelHeader).subspan(40, 28);
        const auto psMetadataAddress = std::uint64_t(inputs.pixelObject) + word(psHeader, 24) + 40;
        inputs.pixelMetadata         = read(psMetadataAddress, 32, false);
        inputs.pixelCode             = read(physical(std::uint64_t(word(inputs.pixelHeader, 24)) + word(inputs.pixelMetadata, 0), true), codeBytes(inputs.pixelMetadata), true);
        if (MovieMicrocodeWordDigest(inputs.pixelCode) != kMoviePixelWordDigest)
            return false;
        inputs.movieCandidate = true;
        require(r[1] == kMoviePrimitive && r[2] == kMovieVertices, "movie_unsupported_nonindexed_up_tuple");

        // With an active PS, 0x826ADD88 selects regular VS variant zero.
        inputs.variant      = 0;
        inputs.vertexHeader = read(inputs.vertexObject, 912, false);
        require(inputs.vertexHeader.size() == 912, "movie_shader_header_extent");
        const auto vsHeader          = std::span<const std::uint8_t>(inputs.vertexHeader).subspan(872, 40);
        const auto vsMetadataAddress = std::uint64_t(inputs.vertexObject) + word(vsHeader, 24) + 872;
        inputs.vertexMetadata        = read(vsMetadataAddress, 32, false);
        inputs.vertexCode            = read(physical(std::uint64_t(word(inputs.vertexHeader, 32)) + word(inputs.vertexMetadata, 0)), codeBytes(inputs.vertexMetadata), true);

        // The pixel stage loads c252..c255 from its literal table; the vertex
        // stage has none.
        std::vector<ShaderLiteralRange>                      vertexRanges;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> vertexLiterals;
        std::string                                          literalError;
        if (!ReadShaderResourceLiterals(read, inputs.pixelObject, true, inputs.literalRanges, inputs.literals, literalError) ||
            !ReadShaderResourceLiterals(read, inputs.vertexObject, false, vertexRanges, vertexLiterals, literalError))
            throw std::runtime_error("movie_literals: " + literalError);
        require(inputs.literalRanges.size() == 1 && inputs.literalRanges[0].firstFloat4 == kMovieLiteralFirst &&
                    inputs.literalRanges[0].dwordCount == kMovieLiteralCount,
                "movie_literal_shape");
        require(vertexRanges.empty(), "movie_vertex_literals");

        const auto fetchCount = word(inputs.vertexMetadata, 28);
        require(fetchCount <= 32, "movie_fetch_record_extent");
        const auto recordsOffset = (std::uint64_t(word(inputs.vertexMetadata, 24)) + 9) * 4;
        require(recordsOffset <= kMaxCodeBytes, "movie_fetch_record_extent");
        if (fetchCount)
        {
            inputs.fetchRecords   = read(vsMetadataAddress + recordsOffset, fetchCount * 4, false);
            inputs.fetchTemplates = read(std::uint64_t(inputs.vertexObject) + 68, fetchCount * 12, false);
        }
        const bool link          = needsLinkage(inputs.vertexMetadata, inputs.pixelMetadata);
        const auto vsCount       = link ? (word(inputs.vertexMetadata, 20) >> 5) & 31 : 0;
        const auto psCount       = link ? (word(inputs.pixelMetadata, 20) >> 5) & 31 : 0;
        const auto linkageOffset = recordsOffset + fetchCount * 4;
        require(linkageOffset + vsCount * 4 <= kMaxCodeBytes, "movie_linkage_metadata_extent");
        inputs.vertexMetadata  = read(vsMetadataAddress, std::max<std::uint64_t>(32, linkageOffset + vsCount * 4), false);
        inputs.pixelMetadata   = read(psMetadataAddress, 32 + psCount * 4, false);
        const auto listsOffset = linkageOffset + vsCount * 4;
        for (unsigned i = 0; i < vsCount; ++i)
        {
            const auto listIndex  = (word(inputs.vertexMetadata, linkageOffset + i * 4) >> 16) & 0xFFF;
            bool       terminated = false;
            for (unsigned j = 0; j < 4096; ++j)
            {
                const auto offset = listsOffset + (listIndex + j) * 4;
                require(offset + 4 <= kMaxCodeBytes, "movie_linkage_metadata_extent");
                if (offset + 4 > inputs.vertexMetadata.size())
                    inputs.vertexMetadata = read(vsMetadataAddress, offset + 4, false);
                if (word(inputs.vertexMetadata, offset) & 0x1000)
                {
                    terminated = true;
                    break;
                }
            }
            require(terminated, "movie_linkage_patch_list_unterminated");
        }

        require(inputs.declarationAddress, "movie_missing_declaration");
        inputs.declarationHeader = read(inputs.declarationAddress, 52, false);
        const auto elements      = word(inputs.declarationHeader, 24);
        require(elements <= 32, "movie_declaration_extent");
        if (elements)
            inputs.declaration = read(std::uint64_t(inputs.declarationAddress) + 52, elements * 12, false);
        inputs.swizzleTable = read(0x82091394, 16, false);
        inputs.formatTable  = read(0x820913A8, 64, false);

        inputs.vertices = read(r[3], std::uint64_t(kMovieVertices) * kMovieStride, false);
        require(inputs.vertices.size() == std::uint64_t(kMovieVertices) * kMovieStride, "movie_geometry_extent");

        // Snapshot each plane's base level at entry, as the guest's decoder
        // last wrote it.
        for (std::size_t slot = 0; slot < inputs.planes.size(); ++slot)
        {
            auto& plane = inputs.planes[slot];
            for (std::size_t i = 0; i < plane.fetch.size(); ++i)
                plane.fetch[i] = word(state, kMovieFetchShadows[slot] + i * 4);
            require(planeFormat(plane.fetch, slot), "movie_plane_format");
            NativeTextureMemoryRange range;
            std::string              textureError;
            require(GetNativeTextureMemoryRange(plane.fetch, range, textureError), "movie_plane_format");
            plane.address = range.address;
            plane.bytes   = read.Resource({ GuestResourceKind::Texture, range.address, range.size, 0, plane.fetch });
            require(plane.bytes.size() == range.size, "movie_plane_read");
        }
        inputs.inputsComplete = true;
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

} // namespace rerevved::gpu::guest_d3d
