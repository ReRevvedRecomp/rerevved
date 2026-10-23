#include "solid_draw_adapter.h"

#include "gpu/cached_sha256.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <fstream>
#include <optional>
#include <rex/crypto/sha256.h>
#include <stdexcept>
#include <toml++/toml.hpp>

namespace rerevved::gpu::guest_d3d
{
namespace
{

std::atomic<std::uint64_t> identityHits{ 0 }, identityMisses{ 0 }, identityPatches{ 0 };

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("adapter_input_extent");
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

void store(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("adapter_output_extent");
    for (unsigned c = 0; c < 4; ++c)
        bytes[offset + c] = static_cast<std::uint8_t>(value >> (24 - c * 8));
}

std::uint32_t mask(unsigned first, unsigned last)
{
    const auto left = UINT32_MAX >> first, right = UINT32_MAX << (31 - last);
    return first <= last ? left & right : left | right;
}

std::uint32_t rotate(std::uint32_t value, unsigned shift, unsigned first, unsigned last)
{
    return std::rotl(value, static_cast<int>(shift)) & mask(first, last);
}

std::uint32_t insert(std::uint32_t into, std::uint32_t value, unsigned shift, unsigned first, unsigned last)
{
    return (into & ~mask(first, last)) | rotate(value, shift, first, last);
}

void require(bool condition, const char* reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}

void write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.close();
    require(bool(file), "adapter_save_failed");
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    const auto size = std::filesystem::file_size(path);
    require(size <= 16 * 1024 * 1024, "adapter_file_extent");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream             file(path, std::ios::binary);
    require(bool(file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), "adapter_file_read");
    return bytes;
}

std::uint64_t physical(std::uint64_t raw, bool pixel = false)
{
    require(raw <= UINT32_MAX, "adapter_shader_address_overflow");
    return (raw & (pixel ? 0x1FFFFFFEULL : 0x1FFFFFFFULL)) + (((raw >> 20) + 512) & 0x1000);
}

// Whole 12-byte instruction slots; the panel and label VS are 132 bytes, the GFx VS 156.
bool validVertexCode(std::span<const std::uint8_t> code)
{
    return !code.empty() && code.size() % 12 == 0 && code.size() <= 64 * 1024;
}

// Fetch slots of each exec clause in the control-flow pairs before the lowest
// exec address, each slot once, as build-native-shaders.py fetch_instructions.
std::vector<std::array<std::uint32_t, 3>> fetchInstructions(std::span<const std::uint8_t> code)
{
    require(!code.empty() && code.size() % 12 == 0, "adapter_fetch_walk_extent");
    const auto                                slots    = code.size() / 12;
    auto                                      boundary = slots;
    std::vector<bool>                         seen(slots);
    std::vector<std::array<std::uint32_t, 3>> result;
    for (std::size_t pair = 0; pair < boundary; ++pair)
    {
        const auto w0 = word(code, pair * 12), w1 = word(code, pair * 12 + 4), w2 = word(code, pair * 12 + 8);
        for (const std::uint64_t cf : { w0 | std::uint64_t(w1 & 0xFFFF) << 32, w1 >> 16 | std::uint64_t(w2) << 16 })
        {
            // kExec..kCondExecPredEnd (1..6) and kCondExecPredClean(End) (13, 14).
            const auto opcode = unsigned(cf >> 44);
            if (!(opcode >= 1 && opcode <= 6) && opcode != 13 && opcode != 14)
                continue;
            const auto address = std::size_t(cf & 4095), count = std::size_t(cf >> 12 & 7);
            const auto sequence = unsigned(cf >> 16 & 4095);
            require(address && address + count <= slots, "adapter_fetch_walk_extent");
            boundary = std::min(boundary, address);
            for (std::size_t i = 0; i < count; ++i)
                if (sequence >> (i * 2) & 1 && !seen[address + i])
                {
                    seen[address + i] = true;
                    const auto at     = (address + i) * 12;
                    result.push_back({ word(code, at), word(code, at + 4), word(code, at + 8) });
                }
        }
    }
    return result;
}

bool needsLinkage(std::span<const std::uint8_t> vs, std::span<const std::uint8_t> ps)
{
    const auto vw = word(vs, 20), pw = word(ps, 20);
    return ((vw ^ pw) & 0x7FFFF) && (vs[8] & 7) != 7 && !(vw & 0x40000) && !(pw & 0x20000);
}

void saveRawInputs(const std::filesystem::path& root, const SolidDrawInputs& in)
{
    require(std::filesystem::create_directory(root), "adapter_directory_not_fresh");
    write(root / "state.be.bin", in.state);
    write(root / "vs-header.be.bin", in.vertexHeader);
    write(root / "ps-header.be.bin", in.pixelHeader);
    write(root / "vs-source.be.bin", in.vertexCode);
    write(root / "ps.be.bin", in.pixelCode);
    write(root / "vs-metadata.be.bin", in.vertexMetadata);
    write(root / "ps-metadata.be.bin", in.pixelMetadata);
    write(root / "vs-literals.be.bin", in.vertexLiterals);
    write(root / "ps-literals.be.bin", in.pixelLiterals);
    write(root / "fetch-templates.be.bin", in.fetchTemplates);
    write(root / "fetch-records.be.bin", in.fetchRecords);
    write(root / "declaration-header.be.bin", in.declarationHeader);
    write(root / "declaration.be.bin", in.declaration);
    write(root / "swizzle-table.be.bin", in.swizzleTable);
    write(root / "format-table.bin", in.formatTable);
    write(root / "vertices.be.bin", in.vertices);
    write(root / "indices.be.bin", in.indices);
    const auto&   a = in.arguments;
    toml::table   metadata{ { "adapter_schema", 1 }, { "boundary", 0x826A3568LL }, { "before_original", true }, { "inputs_complete", in.inputsComplete }, { "caller", static_cast<std::int64_t>(a.caller) }, { "graphics", a.graphics }, { "primitive", a.primitive }, { "minimum_vertex", a.minimumVertex }, { "vertex_count", a.vertexCount }, { "index_count", a.indexCount }, { "source_indices", a.indices }, { "index_format", a.indexFormat }, { "source_vertices", a.vertices }, { "stride", a.stride }, { "vertex_object", in.vertexObject }, { "pixel_object", in.pixelObject }, { "declaration", in.declarationAddress }, { "variant", in.variant } };
    std::ofstream file(root / "recipe.toml");
    file << metadata;
    file.close();
    require(bool(file), "adapter_manifest_save");
}

} // namespace

namespace
{

// Catalog word digests serialize numeric microcode words in little endian.
std::string microcodeWordDigest(std::vector<std::uint8_t> bytes)
{
    require(bytes.size() % 4 == 0, "adapter_microcode_extent");
    for (std::size_t i = 0; i < bytes.size(); i += 4)
        std::reverse(bytes.begin() + i, bytes.begin() + i + 4);
    return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

constexpr std::string_view kSolidVertexWordDigest = "7a8473e246709b45e6895b7e20ebff38949e02da014ac760cfc6caa71d13cb77";
constexpr std::string_view kSolidPixelWordDigest  = "f15792acab07e97384dbaff573da53b2641cedd0e4ecbf1c5f3e1b34d2171921";

} // namespace

bool IsSolidDrawCandidate(const SolidDrawInputs& inputs)
{
    if (inputs.vertexMetadata.size() < 32 || inputs.pixelMetadata.size() < 32 ||
        word(inputs.vertexMetadata, 4) != 132 || word(inputs.pixelMetadata, 4) != 36 || inputs.pixelCode.size() != 36)
        return false;
    return microcodeWordDigest(inputs.pixelCode) == kSolidPixelWordDigest;
}

std::string PixelShaderWordDigest(std::uint32_t pixelObject, const ReadMemory& read)
{
    // The same header, metadata and code reads as ReadSolidDrawInputs.
    const auto header = read(pixelObject, 68, false);
    CountVertexShaderIdentityBytes(header.size());
    const auto metadata = read(std::uint64_t(pixelObject) + word(header, 64) + 40, 32, false);
    CountVertexShaderIdentityBytes(metadata.size());
    const auto codeBytes = word(metadata, 4);
    require(codeBytes && codeBytes % 4 == 0 && codeBytes <= 64 * 1024, "pixel_microcode_extent");
    auto code = read(physical(std::uint64_t(word(header, 24)) + word(metadata, 0), true), codeBytes, true);
    CountVertexShaderIdentityBytes(code.size());
    require(code.size() == codeBytes, "pixel_microcode_extent");
    return microcodeWordDigest(std::move(code));
}

std::string VertexShaderWordDigest(std::uint32_t vertexObject, const ReadMemory& read)
{
    // Variant zero's header, metadata and code reads, as ReadSolidDrawInputs makes them.
    const auto header = read(vertexObject, 912, false);
    CountVertexShaderIdentityBytes(header.size());
    require(header.size() == 912, "vertex_shader_header_extent");
    const auto metadata = read(std::uint64_t(vertexObject) + word(header, 872 + 24) + 872, 32, false);
    CountVertexShaderIdentityBytes(metadata.size());
    const auto codeBytes = word(metadata, 4);
    require(codeBytes && codeBytes % 4 == 0 && codeBytes <= 64 * 1024, "vertex_microcode_extent");
    auto code = read(physical(std::uint64_t(word(header, 32)) + word(metadata, 0)), codeBytes, true);
    CountVertexShaderIdentityBytes(code.size());
    require(code.size() == codeBytes, "vertex_microcode_extent");
    return microcodeWordDigest(std::move(code));
}

bool LoadSolidPanelShader(const std::filesystem::path& capture, NativeGameplayShaderInterface& shader, std::string& error)
{
    shader = {};
    error.clear();
    try
    {
        const auto adapter = capture / "adapter";
        // The saved guest words must be the pinned panel pair this interface names.
        require(microcodeWordDigest(readFile(adapter / "vs-patched.be.bin")) == kSolidVertexWordDigest &&
                    microcodeWordDigest(readFile(adapter / "ps.be.bin")) == kSolidPixelWordDigest,
                "panel_capture_shader_identity");
        const auto digest = [](const std::vector<std::uint8_t>& bytes)
        {
            return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        };
        shader.vertexShaderHash      = 0x11213E38D7154104ULL;
        shader.pixelShaderHash       = 0x3A92D78FE55C7B83ULL;
        shader.vertexShaderDxil      = readFile(adapter / "vs.dxil");
        shader.pixelShaderDxil       = readFile(adapter / "ps.dxil");
        shader.vertexDxilDigest      = digest(shader.vertexShaderDxil);
        shader.pixelDxilDigest       = digest(shader.pixelShaderDxil);
        shader.vertexGuestWordDigest = std::string(kSolidVertexWordDigest);
        shader.pixelGuestWordDigest  = std::string(kSolidPixelWordDigest);
        shader.vertexAttributes      = { { 95, 8, 0, 25, true, false }, { 95, 8, 4, 6, false, true } };
        // Program and render-target state come from the capture's folded state.
        const auto state           = readFile(adapter / "effective-state.be.bin");
        const auto color           = word(state, 0x2884);
        shader.colorTargetFormat   = (color >> 16) & 0xFU;
        shader.colorExpBias        = static_cast<std::int32_t>(((color >> 20) & 0x3FU) ^ 0x20U) - 0x20;
        shader.shaderProgram       = word(state, 0x2920);
        shader.shaderContext       = word(state, 0x2924);
        shader.shaderInterpolators = word(state, 0x2928);
        // Every admitted catalog entry declares both translator properties.
        shader.rasterizedDepth          = true;
        shader.independentSamplers      = true;
        shader.generatedPixelParameters = (shader.shaderProgram & (1U << 18)) != 0;
        require(!shader.vertexShaderDxil.empty() && !shader.pixelShaderDxil.empty(), "panel_capture_dxil_missing");
        return true;
    }
    catch (const std::exception& exception)
    {
        shader = {};
        error  = exception.what();
        return false;
    }
}

bool ReadSolidDrawInputs(const IndexedUpArguments& a, const ReadMemory& read, SolidDrawInputs& inputs, std::string& error)
{
    inputs = {};
    error.clear();
    try
    {
        inputs.arguments = a;
        if (a.stride != 8)
            return false;
        inputs.state      = read(a.graphics, 0x3500, false);
        const auto& state = inputs.state;
        require(state.size() == 0x3500, "adapter_shadow_extent");
        inputs.vertexObject       = word(state, 0x3198);
        inputs.pixelObject        = word(state, 0x3194);
        inputs.declarationAddress = word(state, 0x2E2C);
        if (!inputs.vertexObject || !inputs.pixelObject)
            return false;
        inputs.vertexHeader = read(inputs.vertexObject, 912, false);
        inputs.pixelHeader  = read(inputs.pixelObject, 68, false);
        require(inputs.vertexHeader.size() == 912 && inputs.pixelHeader.size() == 68, "adapter_shader_header_extent");
        const auto vsHeader = std::span<const std::uint8_t>(inputs.vertexHeader).subspan(872, 40);
        const auto psHeader = std::span<const std::uint8_t>(inputs.pixelHeader).subspan(40, 28);
        // With an active PS, 0x826ADD88 selects regular VS variant zero.
        inputs.variant               = 0;
        const auto vsMetadataAddress = std::uint64_t(inputs.vertexObject) + word(vsHeader, 24) + 872;
        const auto psMetadataAddress = std::uint64_t(inputs.pixelObject) + word(psHeader, 24) + 40;
        inputs.vertexMetadata        = read(vsMetadataAddress, 32, false);
        inputs.pixelMetadata         = read(psMetadataAddress, 32, false);
        if (word(inputs.vertexMetadata, 4) != 132 || word(inputs.pixelMetadata, 4) != 36)
            return false;
        const auto psResource = word(inputs.pixelHeader, 24);
        inputs.pixelCode      = read(physical(std::uint64_t(psResource) + word(inputs.pixelMetadata, 0), true), 36, true);
        if (!IsSolidDrawCandidate(inputs))
            return false;
        inputs.solidCandidate   = true;
        const auto vsResource   = word(inputs.vertexHeader, 32);
        inputs.vertexCode       = read(physical(std::uint64_t(vsResource) + word(inputs.vertexMetadata, 0)), 132, true);
        const auto readLiterals = [&](std::uint64_t headerAddress, std::span<const std::uint8_t> header, std::vector<std::uint8_t>& literals)
        {
            const auto relative = word(header, 20);
            if (relative)
            {
                literals        = read(headerAddress + relative, 20, false);
                const auto size = word(literals, 16);
                require(size <= 128 && size % 8 == 0, "adapter_literal_table_extent");
                if (size)
                    literals = read(headerAddress + relative, 20 + size, false);
            }
        };
        readLiterals(std::uint64_t(inputs.vertexObject) + 872, vsHeader, inputs.vertexLiterals);
        readLiterals(std::uint64_t(inputs.pixelObject) + 40, psHeader, inputs.pixelLiterals);
        const auto fetchCount = word(inputs.vertexMetadata, 28);
        require(fetchCount <= 32, "adapter_fetch_record_extent");
        const auto recordsOffset = (std::uint64_t(word(inputs.vertexMetadata, 24)) + 9) * 4;
        require(recordsOffset <= 64 * 1024, "adapter_fetch_record_extent");
        if (fetchCount)
        {
            inputs.fetchRecords   = read(vsMetadataAddress + recordsOffset, fetchCount * 4, false);
            inputs.fetchTemplates = read(std::uint64_t(inputs.vertexObject) + 68, fetchCount * 12, false);
        }
        const bool link          = needsLinkage(inputs.vertexMetadata, inputs.pixelMetadata);
        const auto vsCount       = link ? (word(inputs.vertexMetadata, 20) >> 5) & 31 : 0;
        const auto psCount       = link ? (word(inputs.pixelMetadata, 20) >> 5) & 31 : 0;
        const auto linkageOffset = recordsOffset + fetchCount * 4;
        require(linkageOffset + vsCount * 4 <= 64 * 1024, "adapter_linkage_metadata_extent");
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
                require(offset + 4 <= 64 * 1024, "adapter_linkage_metadata_extent");
                if (offset + 4 > inputs.vertexMetadata.size())
                    inputs.vertexMetadata = read(vsMetadataAddress, offset + 4, false);
                if (word(inputs.vertexMetadata, offset) & 0x1000)
                {
                    terminated = true;
                    break;
                }
            }
            require(terminated, "adapter_linkage_patch_list_unterminated");
        }
        require(inputs.declarationAddress, "adapter_missing_declaration");
        inputs.declarationHeader = read(inputs.declarationAddress, 52, false);
        const auto elements      = word(inputs.declarationHeader, 24);
        require(elements <= 32, "adapter_declaration_extent");
        if (elements)
            inputs.declaration = read(std::uint64_t(inputs.declarationAddress) + 52, elements * 12, false);
        inputs.swizzleTable = read(0x82091394, 16, false);
        inputs.formatTable  = read(0x820913A8, 64, false);
        require(std::uint64_t(a.vertexCount) * a.stride <= 16 * 1024 * 1024 &&
                    std::uint64_t(a.indexCount) * ((a.indexFormat & 4) ? 4 : 2) <= 16 * 1024 * 1024,
                "adapter_geometry_extent");
        if (a.vertexCount)
            inputs.vertices = read(a.vertices, std::uint64_t(a.vertexCount) * a.stride, false);
        if (a.indexCount)
            inputs.indices = read(a.indices, std::uint64_t(a.indexCount) * ((a.indexFormat & 4) ? 4 : 2), false);
        inputs.inputsComplete = true;
        return true;
    }
    catch (const std::exception& exception)
    {
        if (inputs.solidCandidate)
            error = exception.what();
        return false;
    }
}

bool PatchVertexFetch(const SolidDrawInputs& in, std::span<const std::uint32_t> admittedOffsets, std::vector<std::uint8_t>& code, std::string& error)
{
    return PatchVertexFetch(in, in.vertexCode, admittedOffsets, code, error);
}

bool PatchVertexFetch(const SolidDrawInputs& in, std::span<const std::uint8_t> vertexCode, std::span<const std::uint32_t> admittedOffsets, std::vector<std::uint8_t>& code, std::string& error, std::optional<std::uint32_t> strideOverride)
{
    identityPatches.fetch_add(1, std::memory_order_relaxed);
    code.clear();
    try
    {
        const auto stride = strideOverride.value_or(in.arguments.stride);
        require(in.vertexMetadata.size() >= 32, "adapter_patch_input_extent");
        const auto count = word(in.vertexMetadata, 28);
        require(count >= 1 && count <= 32, "adapter_fetch_count");
        // 0x826AD150 searches every declaration element; the VS may leave some unfetched.
        const auto elements = static_cast<unsigned>(in.declaration.size() / 12);
        require(validVertexCode(vertexCode) && in.fetchTemplates.size() == count * 12 && in.fetchRecords.size() == count * 4 &&
                    in.declaration.size() % 12 == 0 && elements && elements <= 32 && (strideOverride || in.state.size() == 0x3500) && in.variant == 0 &&
                    in.swizzleTable.size() == 16 && in.formatTable.size() == 64 && stride &&
                    stride % 4 == 0 && stride / 4 <= 0xFF,
                "adapter_patch_input_extent");
        std::vector<std::array<std::uint32_t, 3>> fetches(count);
        std::vector<unsigned>                     elementIndices(count);
        std::array<std::uint8_t, 16>              patchState{};
        // Nonzero streams are refused below; only stream zero affects the words.
        // The UP lowerers supply stream zero's stride temporarily.
        patchState[0]      = static_cast<std::uint8_t>(stride / 4);
        const auto records = [&](unsigned i)
        {
            return word(in.fetchRecords, i * 4);
        };
        for (unsigned i = 0; i < count; ++i)
        {
            const auto record = records(i);
            const auto usage = rotate(record, 20, 28, 31), usageIndex = rotate(record, 16, 28, 31);
            unsigned   element = 0;
            while (element < elements && (in.declaration[element * 12 + 9] != usage || in.declaration[element * 12 + 10] != usageIndex))
                ++element;
            auto          w0 = word(in.fetchTemplates, i * 12);
            const auto    w1 = word(in.fetchTemplates, i * 12 + 4), w2 = word(in.fetchTemplates, i * 12 + 8);
            std::uint32_t swizzle = 0, patched1 = 0, patched2 = 0;
            elementIndices[i] = element;
            if (element == elements)
            {
                // 0x826AD228: a semantic without an element fetches k_8_8_8_8 at
                // offset 0 of fetch constant 95 with swizzle 0x9250.
                require(std::find(admittedOffsets.begin(), admittedOffsets.end(), 0U) != admittedOffsets.end(), "adapter_attribute_offset");
                w0       = insert(w0, 95, 20, 2, 11);
                patched1 = (w1 & 0xBFC0CFFF) | 0x60000;
                patched2 = patchState[0] | (w2 & 0x80000000);
                swizzle  = 0x9250 | 14;
            }
            else
            {
                const auto offset = element * 12;
                const auto stream = word(in.declaration, offset) >> 16;
                require(stream == 0, "adapter_nonzero_vertex_stream");
                const auto type = word(in.declaration, offset + 4);
                swizzle         = ((rotate(type, 22, 26, 28) | rotate(type, 16, 29, 31)) << 3) | rotate(type, 13, 29, 31);
                swizzle         = (((swizzle << 1) | (type & 0xFC00)) << 3) & 0xFFFF;
                if ((type & 0xC0) == 0x40)
                    for (const auto shift : { 13U, 10U, 7U, 4U })
                        if ((swizzle & (7U << shift)) <= (3U << shift))
                            swizzle ^= 1U << shift;
                swizzle |= 14;
                const auto byteOffset = word(in.declaration, offset) & 0xFFFF;
                require(std::find(admittedOffsets.begin(), admittedOffsets.end(), byteOffset) != admittedOffsets.end() &&
                            byteOffset < stride,
                        "adapter_attribute_offset");
                patched2                 = rotate(byteOffset, 6, 1, 23) | patchState[stream] | (w2 & 0x80000000);
                const auto packedFetch   = (95U - stream) * 0x01560056U;
                const auto expandedFetch = insert(packedFetch, packedFetch, 11, 8, 12);
                w0                       = insert(w0, expandedFetch, 1, 5, 11);
                patched1                 = ((rotate(type, 12, 14, 19) | (type & 0x300)) << 4) | (w1 & 0xBFC0CFFF);
                w0                       = rotate(w0, 0, 5, 1);
            }
            const auto remap = [&](unsigned selector)
            {
                const auto index = selector * 2;
                const auto value = (std::uint32_t(in.swizzleTable[index]) << 8) | in.swizzleTable[index + 1];
                return value & swizzle;
            };
            auto v        = remap(rotate(patched1, 1, 28, 30) / 2);
            auto repeated = rotate(v, 29, 19, 31) | v;
            for (unsigned j = 0; j < 3; ++j)
                repeated = rotate(repeated, 29, 3, 31) | v;
            patched1    = insert(patched1, repeated, 31, 29, 31);
            v           = remap(rotate(patched1, 30, 28, 30) / 2);
            repeated    = rotate(v, 29, 19, 31) | v;
            repeated    = rotate(repeated, 29, 3, 31) | v;
            repeated    = rotate(repeated, 29, 3, 31) | v;
            auto result = rotate(v, 2, 14, 29) | rotate(repeated, 31, 1, 31);
            result      = insert(result, patched1, 0, 29, 25);
            v           = remap(rotate(result, 27, 28, 30) / 2);
            repeated    = rotate(v, 29, 19, 31) | v;
            repeated    = rotate(repeated, 29, 3, 31) | v;
            repeated    = rotate(repeated, 29, 3, 31) | v;
            patched1    = rotate(rotate(v, 3, 13, 28) | v, 2, 0, 29) | rotate(repeated, 31, 1, 31);
            patched1    = insert(patched1, result, 0, 26, 22);
            v           = remap(rotate(patched1, 24, 28, 30) / 2);
            repeated    = rotate(v, 3, 13, 28) | v;
            repeated    = rotate(repeated, 3, 0, 28) | v;
            result      = rotate(rotate(v, 29, 19, 31) | v, 31, 1, 31) | rotate(repeated, 2, 0, 29);
            patched1    = insert(result, patched1, 0, 23, 19);
            fetches[i]  = { w0, patched1, patched2 };
        }
        for (unsigned i = 0; i < count; ++i)
            for (unsigned j = i + 1; j < count; ++j)
                require(elementIndices[i] == elements || elementIndices[i] != elementIndices[j], "adapter_duplicate_fetch_semantic");
        const auto fetchId = [](std::uint32_t w)
        {
            return rotate(w, 14, 25, 29) | rotate(w, 7, 30, 31);
        };
        const auto offsetOf = [](std::uint32_t w)
        {
            return static_cast<std::int32_t>(w << 1) >> 9;
        };
        const auto size = [&](const std::array<std::uint32_t, 3>& f)
        {
            return in.formatTable[(f[1] >> 16) & 63];
        };
        const auto key = [](std::uint32_t w)
        {
            return rotate(insert(w, w, 25, 12, 13), 0, 7, 13);
        };
        // 0x826AD4B0..0x826AD710 sorts and coalesces each group that runs
        // from one 0x300000-flagged record through the next flagged one.
        for (unsigned open = 0; open < count;)
        {
            while (open < count && !(records(open) & 0x300000))
                ++open;
            auto close = open + 1;
            while (close < count && !(records(close) & 0x300000))
                ++close;
            if (close >= count)
                break;
            const auto   n      = close - open + 1;
            unsigned     passes = n - 1, bound = n;
            std::int64_t pinned = -1;
            // 0x826AD54C..0x826AD570: a closing 0x300000 record keeps its fetch
            // out of the sort and moves fetches sharing its id after the others.
            if ((records(close) & 0x300000) == 0x300000)
            {
                passes = n - 2;
                bound  = n - 1;
                pinned = fetchId(fetches[close][0]);
            }
            for (unsigned i = 0; i < passes; ++i)
                for (unsigned j = i + 1; j < bound; ++j)
                {
                    auto&      a   = fetches[open + i];
                    auto&      b   = fetches[open + j];
                    const auto idA = fetchId(a[0]), idB = fetchId(b[0]);
                    bool       swap = false;
                    if (idA != idB)
                        swap = pinned == idA || (pinned != idB && idB > idA);
                    else
                    {
                        auto difference = offsetOf(a[2]) - offsetOf(b[2]);
                        if (!difference)
                            difference = int(size(a)) - int(size(b));
                        swap = difference > 0;
                    }
                    if (swap)
                        std::swap(a, b);
                }
            // The coalescing comparisons are unsigned, as in the guest.
            for (unsigned anchor = open, i = open + 1; i <= close; ++i)
            {
                auto&      first  = fetches[anchor];
                auto&      second = fetches[i];
                const auto lower = static_cast<std::uint32_t>(offsetOf(first[2])), upper = static_cast<std::uint32_t>(offsetOf(second[2]));
                const auto span = std::uint32_t(size(second)) - lower + upper;
                if (key(first[0]) != key(second[0]) || lower > upper || span > 8)
                {
                    anchor = i;
                    continue;
                }
                first[0] = insert(first[0], std::max(span - 1, rotate(first[0], 5, 29, 31)), 27, 2, 4);
                second[1] |= 0x40000000;
            }
            open = close + 1;
        }
        code.assign(vertexCode.begin(), vertexCode.end());
        std::vector<std::uint32_t> targets(count);
        for (unsigned i = 0; i < count; ++i)
        {
            targets[i] = records(i) & 0xFFF;
            require(targets[i] < code.size() / 12 && std::find(targets.begin(), targets.begin() + i, targets[i]) == targets.begin() + i,
                    "adapter_fetch_code_offset");
            for (unsigned j = 0; j < 3; ++j)
                store(code, targets[i] * 12 + j * 4, fetches[i][j]);
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        code.clear();
        error = exception.what();
        return false;
    }
}

bool DecodeShaderVertexFetches(std::span<const std::uint8_t> vertexCode, std::vector<NativeGameplayVertexAttribute>& attributes, std::vector<std::array<std::uint32_t, 3>>* textures)
{
    attributes.clear();
    if (textures)
        textures->clear();
    try
    {
        std::optional<std::pair<std::uint32_t, std::uint32_t>> full; // fetch constant, stride dwords
        for (const auto& [w0, w1, w2] : fetchInstructions(vertexCode))
        {
            if (textures && (w0 & 31))
            {
                textures->push_back({ w0, w1, w2 });
                continue;
            }
            // Opcode 0 is a vertex fetch. A signed repeating fraction mode or
            // an exponent adjust has no catalog form.
            if (w0 & 31 || w1 >> 14 & 1 || w1 >> 24 & 63)
                return false;
            if (!(w1 >> 30 & 1))
                full = { (w0 >> 20 & 31) * 3 + (w0 >> 25 & 3), w2 & 0xFF };
            if (!full)
                return false;
            // The dword offset is a signed 23-bit field at bit 8.
            attributes.push_back({ full->first, full->second * 4, (static_cast<std::int32_t>(w2 << 1) >> 9) * 4, w1 >> 16 & 63, bool(w1 >> 12 & 1), !(w1 >> 13 & 1) });
        }
        return true;
    }
    catch (const std::exception&)
    {
        attributes.clear();
        return false;
    }
}

ShaderFetchMatch MatchShaderFetches(std::span<const std::uint8_t> vertexCode, std::span<const std::uint8_t> pixelCode, const NativeGameplayShaderInterface& shader)
{
    std::vector<NativeGameplayVertexAttribute> attributes;
    std::vector<std::array<std::uint32_t, 3>>  vertexTextures;
    if (!DecodeShaderVertexFetches(vertexCode, attributes, &vertexTextures))
        return ShaderFetchMatch::VertexLayout;
    if (!std::ranges::equal(attributes, shader.vertexAttributes, [](const NativeGameplayVertexAttribute& a, const NativeGameplayVertexAttribute& b)
                            {
                                return a.fetchConstant == b.fetchConstant && a.strideBytes == b.strideBytes && a.offsetBytes == b.offsetBytes && a.format == b.format &&
                                       a.isSigned == b.isSigned && a.normalized == b.normalized;
                            }))
        return ShaderFetchMatch::VertexLayout;

    using Slot = std::pair<std::uint32_t, NativeDrawReplayTextureDimension>;
    std::vector<Slot> bound, catalog;
    // As build-native-shaders.py admits a vertex texture fetch: a normalized
    // 2D fetch (opcode 1) without computed LOD, LOD bias, gradients or offsets.
    for (const auto& [w0, w1, w2] : vertexTextures)
    {
        if ((w0 & 31) != 1 || w0 & (1U << 25) || w1 & (1U << 29) || w2 & 0x1FD || (w2 >> 14 & 3) != 1)
            return ShaderFetchMatch::TextureLayout;
        const Slot slot{ w0 >> 20 & 31, NativeDrawReplayTextureDimension::Texture2D };
        if (std::ranges::find(bound, slot.first, &Slot::first) == bound.end())
            bound.push_back(slot);
    }
    try
    {
        for (const auto& [w0, w1, w2] : fetchInstructions(pixelCode))
        {
            const auto opcode = w0 & 31;
            if (opcode == 0)
                return ShaderFetchMatch::VertexLayout;
            // kSetTextureLod and the gradient setters (24..26) bind nothing.
            if (opcode >= 24 && opcode <= 26)
                continue;
            // FetchOpDimension: 1 is 2D, 3 is cube.
            const auto dimension = w2 >> 14 & 3;
            if (dimension != 1 && dimension != 3)
                return ShaderFetchMatch::TextureLayout;
            const Slot slot{ w0 >> 20 & 31, dimension == 3 ? NativeDrawReplayTextureDimension::TextureCube : NativeDrawReplayTextureDimension::Texture2D };
            const auto known = std::ranges::find(bound, slot.first, &Slot::first);
            if (known == bound.end())
                bound.push_back(slot);
            else if (known->second != slot.second)
                return ShaderFetchMatch::TextureLayout;
        }
    }
    catch (const std::exception&)
    {
        return ShaderFetchMatch::TextureLayout;
    }
    if (shader.textureFetchSlots.size() != shader.textureDimensions.size())
        return ShaderFetchMatch::TextureLayout;
    for (std::size_t i = 0; i < shader.textureFetchSlots.size(); ++i)
        catalog.emplace_back(shader.textureFetchSlots[i], shader.textureDimensions[i]);
    std::ranges::sort(bound);
    std::ranges::sort(catalog);
    return bound == catalog ? ShaderFetchMatch::Matched : ShaderFetchMatch::TextureLayout;
}

std::vector<std::uint32_t> GuestDrawRegisters(std::span<const std::uint8_t> state)
{
    std::vector<std::uint32_t> registers(kGuestDrawRegisterCount);
    FillGuestDrawRegisters(state, registers);
    return registers;
}

void FillGuestDrawRegisters(std::span<const std::uint8_t> state, std::span<std::uint32_t> registers)
{
    require(registers.size() == kGuestDrawRegisterCount, "adapter_register_extent");
    const auto copy = [&](unsigned source, unsigned target, unsigned count)
    {
        if (source > state.size() || (state.size() - source) / 4 < count)
            throw std::runtime_error("adapter_input_extent");
        const auto* from = state.data() + source;
        auto*       to   = registers.data() + target;
        for (unsigned i = 0; i < count; ++i)
        {
            std::uint32_t value;
            std::memcpy(&value, from + i * 4, 4);
            to[i] = std::byteswap(value);
        }
    };
    copy(0x2880, 0x2000, 16);
    copy(0x28C0, 0x2080, 3);
    copy(0x28CC, 0x2100, 21);
    copy(0x2920, 0x2180, 5);
    copy(0x2934, 0x2200, 12);
    copy(0x29B8, 0x2300, 38);
    copy(0x780, 0x4000, 1024);
    copy(0x1780, 0x4400, 896);
    // VS and PS bools at G+0x2780 and 0x2790, VS and PS loops at 0x27A0 and
    // 0x27E0, as the state-block copy sub_826A4CD0 moves them.
    copy(0x2780, 0x4900, 40);
    // User clip planes 0..5 (PA_CL_UCP_0_X..5_W) from G+0x2820, as
    // sub_826ACD20 writes them to 0x2388 + 4 * i.
    copy(0x2820, 0x2388, 24);
}

bool FoldSolidShaderMetadata(const SolidDrawInputs& in, std::vector<std::uint8_t>& code, std::vector<std::uint8_t>& state, std::string& error, bool codeOnly)
{
    error.clear();
    try
    {
        require(in.vertexMetadata.size() >= 32 && in.pixelMetadata.size() >= 32 && validVertexCode(code) &&
                    (codeOnly || in.state.size() == 0x3500) && in.variant == 0,
                "adapter_linkage_input_extent");
        const auto& vs      = in.vertexMetadata;
        const auto& ps      = in.pixelMetadata;
        auto        program = word(vs, 8);
        const auto  vw = word(vs, 20), pw = word(ps, 20);
        // 0x826AD8E8 links exports on the mismatched-pair inline-copy path.
        if (needsLinkage(vs, ps))
        {
            const auto vsCount = (vw >> 5) & 31, psCount = (pw >> 5) & 31;
            const auto records = (std::uint64_t(word(vs, 24)) + word(vs, 28) + 9) * 4;
            const auto lists   = records + vsCount * 4;
            require(records + vsCount * 4 <= vs.size() && 32 + psCount * 4 <= ps.size(), "adapter_linkage_record_extent");
            const auto patch = [&](std::uint32_t record, bool disable, std::uint32_t destination)
            {
                auto offset = lists + ((record >> 16) & 0xFFF) * 4;
                for (unsigned i = 0; i < 4096; ++i, offset += 4)
                {
                    require(offset + 4 <= vs.size(), "adapter_linkage_patch_list_unterminated");
                    const auto entry  = word(vs, offset);
                    const auto target = (entry & 0xFFF) * 12;
                    require(target + 12 <= code.size(), "adapter_linkage_code_offset");
                    if (disable)
                    {
                        // 0x826AD760 replaces an unused export with this no-op.
                        store(code, target, 0xC8000000);
                        store(code, target + 4, 0);
                        store(code, target + 8, 0x02000000);
                    }
                    else
                    {
                        // 0x826AD830 copies PS record bits 8..11 to ALU dest.
                        store(code, target, (word(code, target) & ~15U) | ((destination >> 8) & 15));
                    }
                    if (entry & 0x1000)
                        return;
                }
                throw std::runtime_error("adapter_linkage_patch_list_unterminated");
            };
            unsigned cursor = 0;
            for (unsigned i = 0; i < psCount; ++i)
            {
                const auto input = word(ps, 32 + i * 4);
                while (cursor < vsCount && (word(vs, records + cursor * 4) & 255) < (input & 255))
                    patch(word(vs, records + cursor++ * 4), true, 0);
                require(cursor < vsCount, "adapter_linkage_missing_vs_export");
                const auto output = word(vs, records + cursor++ * 4);
                if ((output ^ input) & 0xF00)
                    patch(output, false, input);
            }
            while (cursor < vsCount)
                patch(word(vs, records + cursor++ * 4), true, 0);
            const auto count = pw & 31;
            program          = (program & ~0xF00000U) | (((count ? count - 1 : 0) & 15) << 20);
        }
        if (codeOnly)
            return true;
        state = in.state;
        // loc_826AE1F0 uses the possibly linked program word and PS metadata.
        store(state, 0x2920, program | word(ps, 8));
        store(state, 0x2924, word(vs, 12) | word(ps, 12));
        store(state, 0x2928, word(ps, 24));
        store(state, 0x2954, (word(state, 0x2954) & ~7U) | 4U);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

std::shared_ptr<const VertexShaderIdentity> MemoizedVertexShaderIdentity(const SolidDrawInputs& in, std::span<const std::uint32_t> admittedOffsets, bool linked, std::optional<std::uint32_t> strideOverride)
{
    const std::array<std::uint32_t, 7> scalars{
        in.vertexObject, in.pixelObject, in.declarationAddress, in.variant, strideOverride.value_or(in.arguments.stride), std::uint32_t(linked), std::uint32_t(strideOverride.has_value() || in.state.size() == 0x3500)
    };
    const std::array<std::span<const std::uint8_t>, 13> key{
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(scalars.data()), sizeof(scalars)),
        in.vertexHeader,
        in.pixelHeader,
        in.declarationHeader,
        in.vertexCode,
        in.vertexMetadata,
        in.pixelMetadata,
        in.fetchRecords,
        in.fetchTemplates,
        in.declaration,
        in.swizzleTable,
        in.formatTable,
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(admittedOffsets.data()), admittedOffsets.size_bytes())
    };

    struct Entry
    {
        std::array<std::vector<std::uint8_t>, 13>   key;
        std::shared_ptr<const VertexShaderIdentity> result;
    };

    // Hashes locate a slot only. Exact byte equality validates every hit, so
    // collisions and same-address guest replacements cannot return old code.
    thread_local std::array<Entry, 256> entries;
    std::uint64_t                       hash = 0;
    for (const auto bytes : key)
        hash = (hash * 0x9E3779B97F4A7C15ULL) ^ XXH3_64bits(bytes.data(), bytes.size());
    auto& entry = entries[hash % entries.size()];
    bool  same  = bool(entry.result);
    for (std::size_t i = 0; same && i < key.size(); ++i)
        same = std::ranges::equal(key[i], entry.key[i]);
    if (same)
    {
        identityHits.fetch_add(1, std::memory_order_relaxed);
        return entry.result;
    }
    identityMisses.fetch_add(1, std::memory_order_relaxed);
    auto                          result = std::make_shared<VertexShaderIdentity>();
    std::vector<std::uint8_t>     folded, ignored;
    std::span<const std::uint8_t> source   = in.vertexCode;
    bool                          foldedOk = true;
    if (linked && in.pixelObject)
    {
        folded   = in.vertexCode;
        foldedOk = FoldSolidShaderMetadata(in, folded, ignored, result->error, true);
        source   = folded;
    }
    result->ok = foldedOk && PatchVertexFetch(in, source, admittedOffsets, result->code, result->error, strideOverride);
    if (result->ok)
        result->digest = gpu::CachedSha256::OfSwappedWords(result->code);
    for (std::size_t i = 0; i < key.size(); ++i)
        entry.key[i].assign(key[i].begin(), key[i].end());
    entry.result = result;
    return result;
}

VertexShaderIdentityCounts TakeVertexShaderIdentityCounts() noexcept
{
    return { identityHits.exchange(0, std::memory_order_relaxed), identityMisses.exchange(0, std::memory_order_relaxed), identityPatches.exchange(0, std::memory_order_relaxed), detail::shaderIdentityGuestBytes.exchange(0, std::memory_order_relaxed) };
}

bool LoadSolidDrawShader(const std::filesystem::path& directory, NativeGameplayShaderInterface& shader, std::string& error)
{
    shader = {};
    try
    {
        std::vector<NativeGameplayShaderInterface> catalog;
        if (!LoadNativeGameplayShaders(directory, catalog, error))
            return false;
        const auto  table   = toml::parse_file((directory / "shaders.toml").string());
        const auto* entries = table["shaders"].as_array();
        require(entries && entries->size() == catalog.size(), "adapter_catalog_extent");
        bool found = false;
        for (std::size_t i = 0; i < entries->size(); ++i)
            if (const auto* entry = (*entries)[i].as_table(); entry && (*entry)["id"].value_or(std::string{}) == "specialization-0002")
            {
                require(!found, "adapter_duplicate_catalog_entry");
                shader = catalog[i];
                found  = true;
            }
        require(found && shader.vertexShaderHash == 0x11213E38D7154104ULL &&
                    shader.pixelShaderHash == 0x3A92D78FE55C7B83ULL &&
                    shader.vertexGuestWordDigest == "7a8473e246709b45e6895b7e20ebff38949e02da014ac760cfc6caa71d13cb77" &&
                    shader.pixelGuestWordDigest == "f15792acab07e97384dbaff573da53b2641cedd0e4ecbf1c5f3e1b34d2171921" &&
                    shader.vertexAttributes.size() == 2 && shader.textureFetchSlots.empty(),
                "adapter_catalog_solid_identity");
        for (unsigned i = 0; i < 2; ++i)
        {
            const auto& attribute = shader.vertexAttributes[i];
            require(attribute.fetchConstant == 95 && attribute.strideBytes == 8 &&
                        attribute.offsetBytes == int(i * 4) && attribute.format == (i ? 6U : 25U) &&
                        attribute.isSigned == !i && attribute.normalized == bool(i),
                    "adapter_catalog_solid_layout");
        }
        // registers_sha256 is capture provenance, not recipe input. The gameplay
        // converter admits specialization state against guest-derived registers.
        return true;
    }
    catch (const std::exception& exception)
    {
        shader = {};
        error  = exception.what();
        return false;
    }
}

bool BuildSolidDrawRecipe(const SolidDrawInputs& inputs, const NativeGameplayShaderInterface& shader, SolidDrawRecipe& recipe, std::string& error)
{
    recipe = {};
    error.clear();
    if (!IsSolidDrawCandidate(inputs))
        return false;
    try
    {
        const auto& a = inputs.arguments;
        require((a.caller == 0x82303E40 || a.caller == 0x82303E90) && a.primitive == 4 && a.stride == 8 && a.minimumVertex == 0 && a.indexFormat == 1 &&
                    a.vertexCount && a.indexCount && a.indexCount % 3 == 0,
                "adapter_unsupported_indexed_up_tuple");
        require(inputs.state.size() == 0x3500 && !(inputs.state[0x2ABC] & 0x40) && !(inputs.state[0x2ABE] & 0x40), "adapter_conditional_or_recorded_shader");
        require(word(inputs.vertexMetadata, 28) == 2, "adapter_solid_fetch_count");
        require(inputs.declaration.size() == 24, "adapter_solid_declaration_count");
        for (const auto* literals : { &inputs.vertexLiterals, &inputs.pixelLiterals })
            require(literals->size() <= 20 || (word(*literals, 20) & 0xFFFF) == 0, "adapter_shader_resource_literals");
        recipe.patchedVertexCode = inputs.vertexCode;
        recipe.linked            = needsLinkage(inputs.vertexMetadata, inputs.pixelMetadata);
        if (!FoldSolidShaderMetadata(inputs, recipe.patchedVertexCode, recipe.effectiveState, error))
            return false;
        // 0x826ADA38 applies linkage before 0x826AD150's fetch patch.
        auto linkedInputs       = inputs;
        linkedInputs.vertexCode = recipe.patchedVertexCode;
        if (!PatchVertexFetch(linkedInputs, kSolidAttributeOffsets, recipe.patchedVertexCode, error))
            return false;
        auto& state = recipe.effectiveState;
        require(inputs.vertices.size() == std::uint64_t(a.vertexCount) * 8 &&
                    inputs.indices.size() == std::uint64_t(a.indexCount) * 2 && !(a.vertices & 3U),
                "adapter_geometry_extent_or_alignment");
        auto registers = GuestDrawRegisters(state);
        // The converter's fetch descriptor describes our CPU source span. It
        // does not borrow the Original lowerer's allocation or its packet.
        const auto base   = static_cast<std::uint32_t>(physical(a.vertices));
        registers[0x48BE] = base | 3U;
        registers[0x48BF] = static_cast<std::uint32_t>(inputs.vertices.size()) | 0x10000002U;
        const std::array<NativeGameplayVertexFetchSpan, 1> fetches{ { { 95, base, inputs.vertices } } };
        const auto                                         microcode = [&](std::span<const std::uint8_t> bytes)
        {
            require(!bytes.empty() && bytes.size() % 4 == 0, "adapter_microcode_extent");
            std::vector<std::uint32_t> result;
            for (std::size_t i = 0; i < bytes.size(); i += 4)
                result.push_back(word(bytes, i));
            return result;
        };
        const auto             vs = microcode(recipe.patchedVertexCode), ps = microcode(inputs.pixelCode);
        NativeGameplayDrawView view;
        view.registers       = registers;
        view.vertexMicrocode = vs;
        view.pixelMicrocode  = ps;
        view.vertexFetches   = fetches;
        view.indexBytes      = inputs.indices;
        view.primitive       = a.primitive;
        view.indexCount      = a.indexCount;
        view.indexFormat     = 0;
        NativeGameplayDrawTarget target;
        target.fullViewportTarget = true;
        target.width              = 1280;
        target.height             = 720;
        if (!DecodeNativeGameplayViewportAndScissor(registers, target.viewport, target.scissor, error))
            return false;
        const std::array<float, 6> viewport{ target.viewport.x, target.viewport.y, target.viewport.width, target.viewport.height, target.viewport.minDepth, target.viewport.maxDepth };
        for (unsigned i = 0; i < 6; ++i)
        {
            require(word(state, 0x3168 + i * 4) == std::bit_cast<std::uint32_t>(viewport[i]),
                    "adapter_viewport_shadow_scale_mismatch");
        }
        if (!BuildNativeGameplayDrawRecipe(view, shader, target, recipe.native, error))
        {
            error = "adapter_recipe_rejected: " + error;
            return false;
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool SaveSolidDrawInputs(const std::filesystem::path& root, const SolidDrawInputs& in, const SolidDrawRecipe& recipe, std::string& error)
{
    try
    {
        saveRawInputs(root, in);
        write(root / "vs-patched.be.bin", recipe.patchedVertexCode);
        write(root / "effective-state.be.bin", recipe.effectiveState);
        write(root / "vs.dxil", recipe.native.vertexShaderBytes());
        write(root / "ps.dxil", recipe.native.pixelShaderBytes());
        const auto& native = recipe.native;
        write(root / "decoded-vertices.le.bin", native.vertexBytes());
        std::vector<std::uint8_t> decodedIndices(native.indices.size() * 4);
        for (std::size_t i = 0; i < native.indices.size(); ++i)
            for (unsigned c = 0; c < 4; ++c)
                decodedIndices[i * 4 + c] = static_cast<std::uint8_t>(native.indices[i] >> (c * 8));
        write(root / "decoded-indices.le.bin", decodedIndices);
        write(root / "vs-constants.le.bin", native.vertexConstants);
        write(root / "ps-constants.le.bin", native.pixelConstants);
        write(root / "shared-constants.le.bin", native.sharedConstants);
        require(native.blend.sourceColor == NativeDrawReplayBlendFactor::SourceAlpha &&
                    native.blend.destinationColor == NativeDrawReplayBlendFactor::InverseSourceAlpha &&
                    native.blend.sourceAlpha == NativeDrawReplayBlendFactor::SourceAlpha &&
                    native.blend.destinationAlpha == NativeDrawReplayBlendFactor::InverseSourceAlpha &&
                    native.blend.colorOp == NativeDrawReplayBlendOp::Add && native.blend.alphaOp == NativeDrawReplayBlendOp::Add &&
                    !native.depth.enabled && !native.stencil.enabled && !native.rasterizer.cull && !native.textureMask,
                "adapter_native_export_tuple");
        toml::table                          nativeMetadata{ { "schema_version", native.schemaVersion },
                                                             { "paths", toml::table{ { "vertex_dxil", "vs.dxil" }, { "pixel_dxil", "ps.dxil" }, { "vertices", "decoded-vertices.le.bin" }, { "indices_u32", "decoded-indices.le.bin" }, { "vertex_constants", "vs-constants.le.bin" }, { "pixel_constants", "ps-constants.le.bin" }, { "shared_constants", "shared-constants.le.bin" } } },
                                                             { "target", toml::table{ { "width", native.width }, { "height", native.height }, { "sample_count", native.sampleCount }, { "sample_mask", native.sampleMask }, { "clear_color_rgba8", toml::array{ native.clearColor[0], native.clearColor[1], native.clearColor[2], native.clearColor[3] } } } },
                                                             { "draw", toml::table{ { "topology", "triangle_list" }, { "indexed", true }, { "vertex_stride_bytes", native.vertexStrideBytes }, { "vertex_attribute_count", native.vertexAttributeCount }, { "index_count", native.indexCount }, { "texture_mask", native.textureMask } } },
                                                             { "shader", toml::table{ { "vertex_hash", "11213E38D7154104" }, { "pixel_hash", "3A92D78FE55C7B83" } } },
                                                             { "viewport", toml::table{ { "x", native.viewport.x }, { "y", native.viewport.y }, { "width", native.viewport.width }, { "height", native.viewport.height }, { "min_depth", native.viewport.minDepth }, { "max_depth", native.viewport.maxDepth } } },
                                                             { "scissor", toml::table{ { "left", native.scissor.left }, { "top", native.scissor.top }, { "right", native.scissor.right }, { "bottom", native.scissor.bottom } } },
                                                             { "blend", toml::table{ { "enabled", native.blend.enabled }, { "alpha_to_coverage", native.blend.alphaToCoverage }, { "logic_op_enabled", native.blend.logicOpEnabled }, { "source_color", "source_alpha" }, { "destination_color", "inverse_source_alpha" }, { "source_alpha", "source_alpha" }, { "destination_alpha", "inverse_source_alpha" }, { "color_op", "add" }, { "alpha_op", "add" }, { "write_mask", native.blend.writeMask } } } };
        constexpr std::array<const char*, 8> comparisons{ "never", "less", "equal", "less_equal", "greater", "not_equal", "greater_equal", "always" };
        require(native.depth.compare >= 1 && native.depth.compare <= comparisons.size(), "adapter_native_depth_compare");
        nativeMetadata.insert("depth", toml::table{ { "enabled", native.depth.enabled }, { "write_enabled", native.depth.writeEnabled }, { "compare", comparisons[native.depth.compare - 1] }, { "initial_clear", native.depth.initialClear } });
        nativeMetadata.insert("rasterizer", toml::table{ { "cull", "none" }, { "front_counter_clockwise", native.rasterizer.frontCounterClockwise } });
        std::ofstream nativeFile(root / "native-recipe.toml");
        nativeFile << nativeMetadata;
        nativeFile.close();
        require(bool(nativeFile), "adapter_native_recipe_save");
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool SaveRejectedSolidDrawInputs(const std::filesystem::path& root, const SolidDrawInputs& inputs, const std::string& reason, std::string& error)
{
    try
    {
        saveRawInputs(root, inputs);
        std::ofstream file(root / "reason.toml");
        file << toml::table{ { "reason", reason }, { "before_original", true }, { "inputs_complete", inputs.inputsComplete } };
        file.close();
        require(bool(file), "adapter_rejection_reason_save");
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool LoadSolidDrawInputs(const std::filesystem::path& root, SolidDrawInputs& in, std::string& error)
{
    in = {};
    try
    {
        require(std::filesystem::file_size(root / "recipe.toml") <= 4096, "adapter_manifest_extent");
        const auto table = toml::parse_file((root / "recipe.toml").string());
        require(table["adapter_schema"].value_or(0) == 1 && table["before_original"].value_or(false), "adapter_manifest_schema");
        const auto value = [&](const char* key)
        {
            const auto v = table[key].value<std::uint32_t>();
            require(v.has_value(), "adapter_manifest_field");
            return *v;
        };
        auto& a               = in.arguments;
        a                     = { value("graphics"), value("primitive"), value("minimum_vertex"), value("vertex_count"), value("index_count"), value("source_indices"), value("index_format"), value("source_vertices"), value("stride"), value("caller") };
        in.vertexObject       = value("vertex_object");
        in.pixelObject        = value("pixel_object");
        in.declarationAddress = value("declaration");
        in.variant            = value("variant");
        in.inputsComplete     = table["inputs_complete"].value_or(true);
        in.state              = readFile(root / "state.be.bin");
        in.vertexCode         = readFile(root / "vs-source.be.bin");
        in.pixelCode          = readFile(root / "ps.be.bin");
        in.vertexMetadata     = readFile(root / "vs-metadata.be.bin");
        in.pixelMetadata      = readFile(root / "ps-metadata.be.bin");
        in.fetchTemplates     = readFile(root / "fetch-templates.be.bin");
        in.fetchRecords       = readFile(root / "fetch-records.be.bin");
        in.declaration        = readFile(root / "declaration.be.bin");
        in.swizzleTable       = readFile(root / "swizzle-table.be.bin");
        in.formatTable        = readFile(root / "format-table.bin");
        in.vertices           = readFile(root / "vertices.be.bin");
        in.indices            = readFile(root / "indices.be.bin");
        const auto optional   = [&](const char* name, std::vector<std::uint8_t>& bytes)
        {
            if (std::filesystem::exists(root / name))
                bytes = readFile(root / name);
        };
        optional("vs-header.be.bin", in.vertexHeader);
        optional("ps-header.be.bin", in.pixelHeader);
        optional("declaration-header.be.bin", in.declarationHeader);
        optional("vs-literals.be.bin", in.vertexLiterals);
        optional("ps-literals.be.bin", in.pixelLiterals);
        in.solidCandidate = IsSolidDrawCandidate(in);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool CompleteIndexedUpAbi(const IndexedUpArguments& a, const IndexedUpStorage& storage, std::span<std::uint8_t> memory, std::uint32_t memoryBase, std::uint32_t& returnRegister, std::string& error)
{
    try
    {
        const auto range = [&](std::uint64_t address, std::uint64_t size)
        {
            require(address >= memoryBase && address - memoryBase <= memory.size() &&
                        size <= memory.size() - (address - memoryBase),
                    "adapter_abi_memory_extent");
            return static_cast<std::size_t>(address - memoryBase);
        };
        const auto vertexBytes = std::uint64_t(a.vertexCount) * a.stride;
        const auto indexBytes  = (std::uint64_t(a.indexCount) * ((a.indexFormat & 4) ? 4 : 2) + 3) & ~3ULL;
        require(vertexBytes <= UINT32_MAX && indexBytes <= UINT32_MAX && a.stride % 4 == 0,
                "adapter_abi_copy_extent");
        const auto g  = range(a.graphics, 0x3498);
        const auto vs = range(std::uint64_t(a.vertices) + std::uint64_t(a.minimumVertex) * a.stride, vertexBytes);
        const auto is = range(a.indices, indexBytes);
        const auto vd = range(storage.vertexOutput, vertexBytes), id = range(storage.indexOutput, indexBytes);
        // Validate all ranges before making any guest-visible change.
        std::memmove(memory.data() + vd, memory.data() + vs, static_cast<std::size_t>(vertexBytes));
        std::memmove(memory.data() + id, memory.data() + is, static_cast<std::size_t>(indexBytes));
        store(memory, g + 0x3484, storage.pendingCursor);
        store(memory, g + 0x3488, storage.vertexOutput);
        store(memory, g + 0x348C, storage.indexOutput);
        store(memory, g + 0x3490, static_cast<std::uint32_t>(vertexBytes / 4));
        store(memory, g + 0x3494, static_cast<std::uint32_t>(indexBytes / 4));
        store(memory, g + 0x30, storage.pendingCursor);
        returnRegister = storage.indexOutput;
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

} // namespace rerevved::gpu::guest_d3d
