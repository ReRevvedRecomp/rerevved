#include "label_draw_inputs.h"

#include "gpu/d3d12/native_texture_upload.h"

#include <algorithm>
#include <fstream>
#include <rex/crypto/sha256.h>
#include <stdexcept>
#include <toml++/toml.hpp>

namespace rerevved::gpu::guest_d3d
{
namespace
{

constexpr std::uint64_t kMaxCodeBytes = 64 * 1024;
constexpr std::uint64_t kMaxFileBytes = 16 * 1024 * 1024;

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("label_input_extent");
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
    require(raw <= UINT32_MAX, "label_shader_address_overflow");
    return (raw & (pixel ? 0x1FFFFFFEULL : 0x1FFFFFFFULL)) + (((raw >> 20) + 512) & 0x1000);
}

// 0x826AD8E8 links exports only for a mismatched, unskipped VS/PS pair.
bool needsLinkage(std::span<const std::uint8_t> vs, std::span<const std::uint8_t> ps)
{
    const auto vw = word(vs, 20), pw = word(ps, 20);
    return ((vw ^ pw) & 0x7FFFF) && (vs[8] & 7) != 7 && !(vw & 0x40000) && !(pw & 0x20000);
}

std::string sha(std::span<const std::uint8_t> bytes)
{
    return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::uint64_t codeBytes(std::span<const std::uint8_t> metadata)
{
    const auto size = word(metadata, 4);
    require(size && size % 4 == 0 && size <= kMaxCodeBytes, "label_microcode_extent");
    return size;
}

void write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.close();
    require(bool(file), "label_save_failed");
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    const auto size = std::filesystem::file_size(path);
    require(size <= kMaxFileBytes, "label_file_extent");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream             file(path, std::ios::binary);
    require(bool(file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), "label_file_read");
    return bytes;
}

std::array<std::uint8_t, 24> fetchBytes(const std::array<std::uint32_t, 6>& fetch)
{
    std::array<std::uint8_t, 24> bytes{};
    for (std::size_t i = 0; i < fetch.size(); ++i)
        for (unsigned c = 0; c < 4; ++c)
            bytes[i * 4 + c] = static_cast<std::uint8_t>(fetch[i] >> (24 - c * 8));
    return bytes;
}

// Literal tables are read as 0x826ADB98 and 0x826ADEF8 consume them: the
// header word at +20 locates a table whose records follow its 20-byte head.
void readLiterals(const ReadMemory& read, std::uint64_t headerAddress, std::span<const std::uint8_t> header, std::vector<std::uint8_t>& literals)
{
    const auto relative = word(header, 20);
    if (!relative)
        return;
    literals        = read(headerAddress + relative, 20, false);
    const auto size = word(literals, 16);
    require(size <= 128 && size % 8 == 0, "label_literal_table_extent");
    if (size)
        literals = read(headerAddress + relative, 20 + size, false);
}

} // namespace

std::string LabelMicrocodeWordDigest(std::span<const std::uint8_t> bytes)
{
    require(bytes.size() % 4 == 0, "label_microcode_extent");
    std::vector<std::uint8_t> swapped(bytes.begin(), bytes.end());
    for (std::size_t i = 0; i < swapped.size(); i += 4)
        std::reverse(swapped.begin() + i, swapped.begin() + i + 4);
    return sha(swapped);
}

bool IsLabelPixelShader(std::span<const std::uint8_t> pixelCode)
{
    return !pixelCode.empty() && pixelCode.size() % 4 == 0 && LabelMicrocodeWordDigest(pixelCode) == kLabelPixelWordDigest;
}

bool ReadLabelDrawInputs(const NativeDrawSubmission& submission, const ReadMemory& read, LabelDrawInputs& inputs, std::string& error)
{
    return ReadLabelDrawInputsForDigest(kLabelPixelWordDigest, submission, read, inputs, error);
}

bool ReadLabelDrawInputsForDigest(std::string_view pixelWordDigest, const NativeDrawSubmission& submission, const ReadMemory& read, LabelDrawInputs& inputs, std::string& error)
{
    inputs = {};
    error.clear();
    // r3..r10 at entry: device, primitive, vertex count, CPU vertices, stride.
    const auto& r = submission.arguments;
    if (submission.hook != NativeDrawHook::NonindexedUp || submission.caller != kLabelDrawCaller ||
        r[1] != 4 || r[4] != kLabelStride || !r[2] || r[2] % 3)
        return false;
    try
    {
        inputs.arguments  = { r[0], r[1], 0, r[2], 0, 0, 0, r[3], r[4], submission.caller };
        inputs.state      = read(r[0], 0x3500, false);
        const auto& state = inputs.state;
        require(state.size() == 0x3500, "label_shadow_extent");
        inputs.vertexObject       = word(state, 0x3198);
        inputs.pixelObject        = word(state, 0x3194);
        inputs.declarationAddress = word(state, 0x2E2C);
        if (!inputs.vertexObject || !inputs.pixelObject)
            return false;
        inputs.pixelHeader = read(inputs.pixelObject, 68, false);
        require(inputs.pixelHeader.size() == 68, "label_shader_header_extent");
        const auto psHeader          = std::span<const std::uint8_t>(inputs.pixelHeader).subspan(40, 28);
        const auto psMetadataAddress = std::uint64_t(inputs.pixelObject) + word(psHeader, 24) + 40;
        inputs.pixelMetadata         = read(psMetadataAddress, 32, false);
        inputs.pixelCode             = read(physical(std::uint64_t(word(inputs.pixelHeader, 24)) + word(inputs.pixelMetadata, 0), true), codeBytes(inputs.pixelMetadata), true);
        if (LabelMicrocodeWordDigest(inputs.pixelCode) != pixelWordDigest)
            return false;
        inputs.labelCandidate = true;

        // With an active PS, 0x826ADD88 selects regular VS variant zero.
        inputs.variant      = 0;
        inputs.vertexHeader = read(inputs.vertexObject, 912, false);
        require(inputs.vertexHeader.size() == 912, "label_shader_header_extent");
        const auto vsHeader          = std::span<const std::uint8_t>(inputs.vertexHeader).subspan(872, 40);
        const auto vsMetadataAddress = std::uint64_t(inputs.vertexObject) + word(vsHeader, 24) + 872;
        inputs.vertexMetadata        = read(vsMetadataAddress, 32, false);
        inputs.vertexCode            = read(physical(std::uint64_t(word(inputs.vertexHeader, 32)) + word(inputs.vertexMetadata, 0)), codeBytes(inputs.vertexMetadata), true);
        readLiterals(read, std::uint64_t(inputs.vertexObject) + 872, vsHeader, inputs.vertexLiterals);
        readLiterals(read, std::uint64_t(inputs.pixelObject) + 40, psHeader, inputs.pixelLiterals);

        const auto fetchCount = word(inputs.vertexMetadata, 28);
        require(fetchCount <= 32, "label_fetch_record_extent");
        const auto recordsOffset = (std::uint64_t(word(inputs.vertexMetadata, 24)) + 9) * 4;
        require(recordsOffset <= kMaxCodeBytes, "label_fetch_record_extent");
        if (fetchCount)
        {
            inputs.fetchRecords   = read(vsMetadataAddress + recordsOffset, fetchCount * 4, false);
            inputs.fetchTemplates = read(std::uint64_t(inputs.vertexObject) + 68, fetchCount * 12, false);
        }
        const bool link          = needsLinkage(inputs.vertexMetadata, inputs.pixelMetadata);
        const auto vsCount       = link ? (word(inputs.vertexMetadata, 20) >> 5) & 31 : 0;
        const auto psCount       = link ? (word(inputs.pixelMetadata, 20) >> 5) & 31 : 0;
        const auto linkageOffset = recordsOffset + fetchCount * 4;
        require(linkageOffset + vsCount * 4 <= kMaxCodeBytes, "label_linkage_metadata_extent");
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
                require(offset + 4 <= kMaxCodeBytes, "label_linkage_metadata_extent");
                if (offset + 4 > inputs.vertexMetadata.size())
                    inputs.vertexMetadata = read(vsMetadataAddress, offset + 4, false);
                if (word(inputs.vertexMetadata, offset) & 0x1000)
                {
                    terminated = true;
                    break;
                }
            }
            require(terminated, "label_linkage_patch_list_unterminated");
        }

        require(inputs.declarationAddress, "label_missing_declaration");
        inputs.declarationHeader = read(inputs.declarationAddress, 52, false);
        const auto elements      = word(inputs.declarationHeader, 24);
        require(elements <= 32, "label_declaration_extent");
        if (elements)
            inputs.declaration = read(std::uint64_t(inputs.declarationAddress) + 52, elements * 12, false);
        inputs.swizzleTable = read(0x82091394, 16, false);
        inputs.formatTable  = read(0x820913A8, 64, false);

        require(std::uint64_t(r[2]) * r[4] <= kMaxFileBytes, "label_geometry_extent");
        inputs.vertices = read(r[3], std::uint64_t(r[2]) * r[4], false);

        for (std::size_t i = 0; i < inputs.textureFetch.size(); ++i)
            inputs.textureFetch[i] = word(state, 0x480 + i * 4);
        NativeTextureMemoryRange range;
        std::string              textureError;
        if (!GetNativeTextureMemoryRange(inputs.textureFetch, range, textureError))
            throw std::runtime_error("label_texture_range: " + textureError);
        inputs.textureAddress = range.address;
        inputs.textureBytes   = read.Resource({ GuestResourceKind::Texture, range.address, range.size, 0, inputs.textureFetch });
        require(inputs.textureBytes.size() == range.size, "label_texture_extent");
        inputs.inputsComplete = true;
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool SaveLabelDrawInputs(const std::filesystem::path& root, const LabelDrawInputs& in, std::string& error)
{
    try
    {
        require(std::filesystem::create_directory(root), "label_directory_not_fresh");
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
        write(root / "texture-fetch.be.bin", fetchBytes(in.textureFetch));
        write(root / "texture.bin", in.textureBytes);
        const auto&   a = in.arguments;
        toml::table   metadata{ { "adapter_schema", 1 }, { "family", "label" }, { "boundary", 0x826A3000LL }, { "before_original", true }, { "inputs_complete", in.inputsComplete }, { "caller", static_cast<std::int64_t>(a.caller) }, { "graphics", a.graphics }, { "primitive", a.primitive }, { "vertex_count", a.vertexCount }, { "source_vertices", a.vertices }, { "stride", a.stride }, { "vertex_object", in.vertexObject }, { "pixel_object", in.pixelObject }, { "declaration", in.declarationAddress }, { "variant", in.variant }, { "texture_address", in.textureAddress }, { "texture_size", static_cast<std::int64_t>(in.textureBytes.size()) }, { "texture_sha256", sha(in.textureBytes) } };
        std::ofstream file(root / "recipe.toml");
        file << metadata;
        file.close();
        require(bool(file), "label_manifest_save");
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool LoadLabelDrawInputs(const std::filesystem::path& root, LabelDrawInputs& in, std::string& error)
{
    in = {};
    try
    {
        require(std::filesystem::file_size(root / "recipe.toml") <= 4096, "label_manifest_extent");
        const auto table = toml::parse_file((root / "recipe.toml").string());
        require(table["adapter_schema"].value_or(0) == 1 && table["family"].value_or(std::string{}) == "label" &&
                    table["before_original"].value_or(false),
                "label_manifest_schema");
        const auto value = [&](const char* key)
        {
            const auto v = table[key].value<std::uint32_t>();
            require(v.has_value(), "label_manifest_field");
            return *v;
        };
        in.arguments          = { value("graphics"), value("primitive"), 0, value("vertex_count"), 0, 0, 0, value("source_vertices"), value("stride"), value("caller") };
        in.vertexObject       = value("vertex_object");
        in.pixelObject        = value("pixel_object");
        in.declarationAddress = value("declaration");
        in.variant            = value("variant");
        in.textureAddress     = value("texture_address");
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
        in.textureBytes       = readFile(root / "texture.bin");
        in.vertexHeader       = readFile(root / "vs-header.be.bin");
        in.pixelHeader        = readFile(root / "ps-header.be.bin");
        in.declarationHeader  = readFile(root / "declaration-header.be.bin");
        in.vertexLiterals     = readFile(root / "vs-literals.be.bin");
        in.pixelLiterals      = readFile(root / "ps-literals.be.bin");
        const auto fetch      = readFile(root / "texture-fetch.be.bin");
        require(fetch.size() == 24, "label_texture_fetch_extent");
        for (std::size_t i = 0; i < in.textureFetch.size(); ++i)
            in.textureFetch[i] = word(fetch, i * 4);
        require(in.vertices.size() == std::uint64_t(in.arguments.vertexCount) * in.arguments.stride, "label_vertex_extent");
        const auto recorded = table["texture_size"].value<std::uint64_t>();
        require(recorded && in.textureBytes.size() == *recorded && table["texture_sha256"].value_or(std::string{}) == sha(in.textureBytes), "label_texture_digest");
        // The fetch alone must still describe the saved range.
        NativeTextureMemoryRange range;
        std::string              textureError;
        require(GetNativeTextureMemoryRange(in.textureFetch, range, textureError) && range.address == in.textureAddress &&
                    range.size == in.textureBytes.size(),
                "label_texture_range");
        in.labelCandidate = IsLabelPixelShader(in.pixelCode);
        return true;
    }
    catch (const std::exception& exception)
    {
        in    = {};
        error = exception.what();
        return false;
    }
}

} // namespace rerevved::gpu::guest_d3d
