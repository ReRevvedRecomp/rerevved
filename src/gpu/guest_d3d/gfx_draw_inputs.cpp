#include "gfx_draw_inputs.h"

#include "gpu/cached_sha256.h"
#include "gpu/d3d12/native_texture_upload.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace rerevved::gpu::guest_d3d
{
namespace
{

constexpr std::uint64_t kMaxCodeBytes = 64 * 1024;
constexpr std::uint64_t kMaxGeometry  = 16 * 1024 * 1024;

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("gfx_input_extent");
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
    require(raw <= UINT32_MAX, "gfx_shader_address_overflow");
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
    require(size && size % 4 == 0 && size <= kMaxCodeBytes, "gfx_microcode_extent");
    return size;
}

// Literal tables as 0x826ADB98 and 0x826ADEF8 consume them (label reader form).
void readLiterals(const ReadMemory& read, std::uint64_t headerAddress, std::span<const std::uint8_t> header, std::vector<std::uint8_t>& literals)
{
    const auto relative = word(header, 20);
    if (!relative)
        return;
    literals        = read(headerAddress + relative, 20, false);
    const auto size = word(literals, 16);
    require(size <= 128 && size % 8 == 0, "gfx_literal_table_extent");
    if (size)
        literals = read(headerAddress + relative, 20 + size, false);
}

} // namespace

std::string GfxMicrocodeWordDigest(std::span<const std::uint8_t> bytes)
{
    require(bytes.size() % 4 == 0, "gfx_microcode_extent");
    return gpu::CachedSha256::OfSwappedWords(bytes);
}

const GfxDrawShader* GfxDrawCatalog::Find(std::string_view pixelWords) const
{
    const auto found = std::ranges::find_if(entries, [&](const GfxDrawShader& entry)
                                            {
                                                return entry.shader.pixelGuestWordDigest == pixelWords;
                                            });
    return found == entries.end() ? nullptr : &*found;
}

bool ReadGfxDrawInputs(const NativeDrawSubmission& submission, const ReadMemory& read, const GfxDrawCatalog& catalog, GfxDrawInputs& inputs, std::string& error)
{
    inputs = {};
    error.clear();
    if (submission.hook != NativeDrawHook::IndexedUp || submission.caller != kGfxDrawCaller)
        return false;
    try
    {
        // Indexed-UP passes its vertex stride at r1+84 (the panel reader's read).
        const auto strideBytes = read(std::uint64_t(submission.stack) + 84, 4, false);
        const auto stride      = word(strideBytes, 0);
        // r3..r10 at entry: device, primitive, minimum vertex, vertex count,
        // index count, CPU indices, index format, CPU vertices.
        const auto& r     = submission.arguments;
        inputs.arguments  = { r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], stride, submission.caller };
        inputs.state      = read(r[0], 0x3500, false);
        const auto& state = inputs.state;
        require(state.size() == 0x3500, "gfx_shadow_extent");
        inputs.vertexObject       = word(state, 0x3198);
        inputs.pixelObject        = word(state, 0x3194);
        inputs.declarationAddress = word(state, 0x2E2C);
        require(inputs.vertexObject && inputs.pixelObject, "gfx_shader_object_missing");
        inputs.pixelHeader = read(inputs.pixelObject, 68, false);
        require(inputs.pixelHeader.size() == 68, "gfx_shader_header_extent");
        const auto psHeader          = std::span<const std::uint8_t>(inputs.pixelHeader).subspan(40, 28);
        const auto psMetadataAddress = std::uint64_t(inputs.pixelObject) + word(psHeader, 24) + 40;
        inputs.pixelMetadata         = read(psMetadataAddress, 32, false);
        inputs.pixelCode             = read(physical(std::uint64_t(word(inputs.pixelHeader, 24)) + word(inputs.pixelMetadata, 0), true), codeBytes(inputs.pixelMetadata), true);
        const auto  pixelWords       = GfxMicrocodeWordDigest(inputs.pixelCode);
        const auto* entry            = catalog.Find(pixelWords);
        if (!entry)
        {
            // Name the pair and stride a catalog entry would need.
            std::string vertexWords;
            try
            {
                vertexWords = VertexShaderWordDigest(inputs.vertexObject, read);
            }
            catch (const std::exception&)
            {
                vertexWords = "unreadable";
            }
            error = std::format("gfx_shader_unpinned ps={} vs={} stride={}", pixelWords, vertexWords, stride);
            return false;
        }
        inputs.gfxCandidate = true;
        require(stride == entry->stride, "gfx_stride_mismatch");

        // With an active PS, 0x826ADD88 selects regular VS variant zero.
        inputs.variant      = 0;
        inputs.vertexHeader = read(inputs.vertexObject, 912, false);
        require(inputs.vertexHeader.size() == 912, "gfx_shader_header_extent");
        const auto vsHeader          = std::span<const std::uint8_t>(inputs.vertexHeader).subspan(872, 40);
        const auto vsMetadataAddress = std::uint64_t(inputs.vertexObject) + word(vsHeader, 24) + 872;
        inputs.vertexMetadata        = read(vsMetadataAddress, 32, false);
        inputs.vertexCode            = read(physical(std::uint64_t(word(inputs.vertexHeader, 32)) + word(inputs.vertexMetadata, 0)), codeBytes(inputs.vertexMetadata), true);
        readLiterals(read, std::uint64_t(inputs.vertexObject) + 872, vsHeader, inputs.vertexLiterals);
        readLiterals(read, std::uint64_t(inputs.pixelObject) + 40, psHeader, inputs.pixelLiterals);

        const auto fetchCount = word(inputs.vertexMetadata, 28);
        require(fetchCount <= 32, "gfx_fetch_record_extent");
        const auto recordsOffset = (std::uint64_t(word(inputs.vertexMetadata, 24)) + 9) * 4;
        require(recordsOffset <= kMaxCodeBytes, "gfx_fetch_record_extent");
        if (fetchCount)
        {
            inputs.fetchRecords   = read(vsMetadataAddress + recordsOffset, fetchCount * 4, false);
            inputs.fetchTemplates = read(std::uint64_t(inputs.vertexObject) + 68, fetchCount * 12, false);
        }
        const bool link          = needsLinkage(inputs.vertexMetadata, inputs.pixelMetadata);
        const auto vsCount       = link ? (word(inputs.vertexMetadata, 20) >> 5) & 31 : 0;
        const auto psCount       = link ? (word(inputs.pixelMetadata, 20) >> 5) & 31 : 0;
        const auto linkageOffset = recordsOffset + fetchCount * 4;
        require(linkageOffset + vsCount * 4 <= kMaxCodeBytes, "gfx_linkage_metadata_extent");
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
                require(offset + 4 <= kMaxCodeBytes, "gfx_linkage_metadata_extent");
                if (offset + 4 > inputs.vertexMetadata.size())
                    inputs.vertexMetadata = read(vsMetadataAddress, offset + 4, false);
                if (word(inputs.vertexMetadata, offset) & 0x1000)
                {
                    terminated = true;
                    break;
                }
            }
            require(terminated, "gfx_linkage_patch_list_unterminated");
        }

        require(inputs.declarationAddress, "gfx_missing_declaration");
        inputs.declarationHeader = read(inputs.declarationAddress, 52, false);
        const auto elements      = word(inputs.declarationHeader, 24);
        require(elements <= 32, "gfx_declaration_extent");
        if (elements)
            inputs.declaration = read(std::uint64_t(inputs.declarationAddress) + 52, elements * 12, false);
        inputs.swizzleTable = read(0x82091394, 16, false);
        inputs.formatTable  = read(0x820913A8, 64, false);

        const auto& a          = inputs.arguments;
        const auto  indexBytes = std::uint64_t(a.indexCount) * ((a.indexFormat & 4) ? 4 : 2);
        require(a.vertexCount && a.indexCount && std::uint64_t(a.vertexCount) * stride <= kMaxGeometry && indexBytes <= kMaxGeometry,
                "gfx_geometry_extent");
        inputs.vertices = read(a.vertices, std::uint64_t(a.vertexCount) * stride, false);
        inputs.indices  = read(a.indices, indexBytes, false);
        require(inputs.vertices.size() == std::uint64_t(a.vertexCount) * stride && inputs.indices.size() == indexBytes, "gfx_geometry_extent");

        // A pair samples the fetch slots below its texture count; the others
        // hold stale state.
        const auto texture = [&](std::uint32_t slot, std::array<std::uint32_t, 6>& fetch, std::uint32_t& address, GuestBytes& bytes, GuestBytes& mipBytes)
        {
            for (std::size_t i = 0; i < fetch.size(); ++i)
                fetch[i] = word(state, 0x480 + (slot * 6 + i) * 4);
            NativeTextureMemoryRange range;
            std::string              textureError;
            if (!GetNativeTextureMemoryRange(fetch, range, textureError))
            {
                // The base-level layout refuses packed mips; the loading
                // screen's DXT1 backgrounds carry them.
                NativeTextureMemoryRanges ranges;
                std::string               fullError;
                if (!GetNativeTextureMemoryRanges(fetch, ranges, fullError) || !ranges.mips.size)
                    throw std::runtime_error("gfx_texture_range: " + textureError);
                mipBytes = read.Resource({ GuestResourceKind::Texture, ranges.mips.address, ranges.mips.size, 0, fetch });
                require(mipBytes.size() == ranges.mips.size, "gfx_texture_extent");
                range = ranges.base;
            }
            address = range.address;
            bytes   = read.Resource({ GuestResourceKind::Texture, range.address, range.size, 0, fetch });
            require(bytes.size() == range.size, "gfx_texture_extent");
        };
        if (entry->textures > 0)
            texture(0, inputs.textureFetch, inputs.textureAddress, inputs.textureBytes, inputs.textureMipBytes);
        if (entry->textures > 1)
            texture(1, inputs.secondTextureFetch, inputs.secondTextureAddress, inputs.secondTextureBytes, inputs.secondTextureMipBytes);
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
