#include "gpu/d3d12/native_completion_source.h"
#include "gpu/d3d12/native_renderer_d3d12.h"
#include "gpu/d3d12/native_texture_upload.h"
#include "gpu/guest_d3d/gfx_draw_inputs.h"
#include "gpu/guest_d3d/label_draw_adapter.h"
#include "gpu/guest_d3d/native_frame_assembler.h"
#include "gpu/guest_d3d/scene_mesh_draw_adapter.h"
#include "logging_scope.hpp"
#include "resource_mirror_fixture.h"
#include "stream_queue_fixture.h"

#include <rex/crypto/sha256.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>

#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <toml++/toml.hpp>

using namespace rerevved::gpu;
using namespace rerevved::gpu::guest_d3d;

namespace
{

// The detail is read after the condition, so it names the call's own error.
std::vector<std::uint8_t> read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    UNSCOPED_INFO("cannot open " + path.string());
    REQUIRE(bool(file));
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

void store(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned c = 0; c < 4; ++c)
        bytes[offset + c] = static_cast<std::uint8_t>(value >> (24 - c * 8));
}

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

std::string environment(const char* name)
{
    char       buffer[4096]{};
    const auto size = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return size && size < sizeof(buffer) ? std::string(buffer) : std::string();
}

// Guest memory as a set of captured regions. A read inside one region returns
// its bytes; any other read returns nothing, as an unmapped guest range does.
struct FakeMemory
{
    struct Region
    {
        std::uint64_t             address  = 0;
        bool                      physical = false;
        std::vector<std::uint8_t> bytes;
    };

    std::vector<Region> regions;
    unsigned            reads = 0;

    void Add(std::uint64_t address, bool physical, std::vector<std::uint8_t> bytes)
    {
        regions.push_back({ address, physical, std::move(bytes) });
    }

    std::vector<std::uint8_t> Read(std::uint64_t address, std::uint64_t size, bool physical)
    {
        ++reads;
        for (const auto& region : regions)
            if (region.physical == physical && address >= region.address && address - region.address + size <= region.bytes.size())
                return { region.bytes.begin() + (address - region.address), region.bytes.begin() + (address - region.address + size) };
        return {};
    }

    Region& At(std::uint64_t address, bool physical)
    {
        for (auto& region : regions)
            if (region.address == address && region.physical == physical)
                return region;
        throw std::runtime_error(std::format("no region at {:#x}", address));
    }
};

ReadMemory readerFor(FakeMemory& memory)
{
    return testing::MirrorFixtureReads([&memory](std::uint64_t address, std::uint64_t size, bool physical)
                                       {
                                           return memory.Read(address, size, physical);
                                       });
}

// Draw 1895 at entry (events.toml raw_r3_r10_u64 and raw_r1, pinned here).
constexpr std::uint64_t            kMesh  = 1895;
constexpr std::uint32_t            kG     = 0x50235580;
constexpr std::uint32_t            kStack = 0x7018F930;
const std::array<std::uint32_t, 8> kMeshTuple{ kG, 4, 0, 0, 6, 16, 3, 0 };

NativeDrawSubmission meshSubmission(const std::array<std::uint32_t, 8>& arguments, std::uint32_t caller = kSceneMeshDrawCaller)
{
    NativeDrawSubmission submission;
    submission.hook      = NativeDrawHook::IndexedBuffer;
    submission.caller    = caller;
    submission.stack     = kStack;
    submission.arguments = arguments;
    return submission;
}

// A pixel shader object whose microcode is `code`: header at 0x40000000,
// metadata after it, code at physical 0x1000. The VS object is unmapped.
void addPixelShader(FakeMemory& memory, std::vector<std::uint8_t> state, std::span<const std::uint8_t> code)
{
    constexpr std::uint32_t object = 0x40000000;
    store(state, 0x3194, object);
    store(state, 0x3198, 0x40001000);
    memory.Add(0x50000000, false, state);
    std::vector<std::uint8_t> header(68), metadata(32);
    store(header, 24, 0x1001); // resource base, physical; bit 0 is not a PS address bit
    store(header, 64, 100);    // metadata at object + 100 + 40
    store(metadata, 4, static_cast<std::uint32_t>(code.size()));
    memory.Add(object, false, header);
    memory.Add(object + 140, false, metadata);
    memory.Add(0x1000, true, { code.begin(), code.end() });
}

void unit()
{
    std::string             error;
    FakeMemory              memory;
    const auto              read  = readerFor(memory);
    const auto              tuple = std::array<std::uint32_t, 8>{ 0x50000000, 4, 0, 0, 6, 0, 0, 0 };
    IndexedBufferDrawInputs inputs;

    // Selection happens before any read: hook, then caller.
    auto other = meshSubmission(tuple);
    other.hook = NativeDrawHook::IndexedUp;
    UNSCOPED_INFO("another hook is foreign without reads");
    REQUIRE((!ReadIndexedBufferDrawInputs(other, read, kSceneMeshPixelWordDigest, inputs, error) && error.empty() && memory.reads == 0));
    UNSCOPED_INFO("another IndexedBuffer call site is foreign without reads");
    REQUIRE((!ReadIndexedBufferDrawInputs(meshSubmission(tuple, 0x8250C700), read, kSceneMeshPixelWordDigest, inputs, error) && error.empty() &&
             memory.reads == 0));
    UNSCOPED_INFO(std::string("an unreadable device shadow is named: ") + (error));
    REQUIRE((!ReadIndexedBufferDrawInputs(meshSubmission(tuple), read, kSceneMeshPixelWordDigest, inputs, error) && error == "indexed_buffer_shadow_extent"));

    // Unbound shaders, then a pixel shader other than the pinned one, are foreign.
    std::vector<std::uint8_t> state(0x3500), foreignCode(96, 0x5A);
    memory.Add(0x50000000, false, state);
    UNSCOPED_INFO("unbound shaders are foreign");
    REQUIRE((!ReadIndexedBufferDrawInputs(meshSubmission(tuple), read, kSceneMeshPixelWordDigest, inputs, error) && error.empty() && !inputs.sceneCandidate));
    memory.regions.clear();
    addPixelShader(memory, state, foreignCode);
    UNSCOPED_INFO("another pixel shader is foreign");
    REQUIRE((!ReadIndexedBufferDrawInputs(meshSubmission(tuple), read, kSceneMeshPixelWordDigest, inputs, error) && error.empty() && !inputs.sceneCandidate));
    // The digest selects the candidate; an unreadable VS after it is named.
    UNSCOPED_INFO(std::string("a matched candidate with an unreadable VS is named: ") + (error));
    REQUIRE((!ReadIndexedBufferDrawInputs(meshSubmission(tuple), read, IndexedBufferMicrocodeWordDigest(foreignCode), inputs, error) &&
             error == "indexed_buffer_shader_header_extent" && inputs.sceneCandidate && !inputs.inputsComplete));
    // The word digest hashes each big-endian word in little-endian order.
    const std::vector<std::uint8_t> bigEndian{ 1, 2, 3, 4, 5, 6, 7, 8 };
    UNSCOPED_INFO("word digest byte order");
    REQUIRE(IndexedBufferMicrocodeWordDigest(bigEndian) == rex::crypto::sha256(std::string_view("\x04\x03\x02\x01\x08\x07\x06\x05", 8)));
    UNSCOPED_INFO("the foreign words are not the prepass PS");
    REQUIRE(!IsSceneMeshPixelShader(foreignCode));
    NativeDrawReplayRecipe draw;
    std::string            detail;
    UNSCOPED_INFO("a non-candidate has no route and no refusal");
    REQUIRE((!BuildSceneMeshDraw(IndexedBufferDrawInputs{}, NativeGameplayShaderInterface{}, draw, detail) && detail.empty()));

    // The pinned fixed state names each register once, in order.
    for (std::size_t i = 1; i < kSceneMeshFixedState.size(); ++i)
    {
        UNSCOPED_INFO("fixed state registers ascend");
        REQUIRE(kSceneMeshFixedState[i - 1].first < kSceneMeshFixedState[i].first);
    }

    // Composition: the mesh is tried after the panel, label and GFx families.
    memory.reads           = 0;
    const auto shader      = std::make_shared<NativeGameplayShaderInterface>();
    auto       withoutMesh = MakeBootDrawBuilder(read, nullptr, nullptr, nullptr);
    UNSCOPED_INFO("without a mesh shader the draw has no route");
    REQUIRE((!withoutMesh(meshSubmission(tuple), draw, detail) && detail.empty() && memory.reads == 0));
    auto meshOnly = MakeBootDrawBuilder(read, nullptr, nullptr, nullptr, shader);
    UNSCOPED_INFO("the mesh reader runs and finds a foreign PS");
    REQUIRE((!meshOnly(meshSubmission(tuple), draw, detail) && detail.empty() && memory.reads > 0));
    memory.reads = 0;
    UNSCOPED_INFO("the mesh builder ignores other IndexedBuffer callers without reads");
    REQUIRE((!meshOnly(meshSubmission(tuple, 0x8250C700), draw, detail) && detail.empty() && memory.reads == 0));
    std::cout << "IndexedBuffer reader selection, word digest and builder composition checks passed\n";
}

// Menu fixture uses synthetic shader words; capture must confirm pinned
// digests, fetches, literals and texture forms.
namespace menu
{

// No capture holds the menu pair microcode; hybrid sources are
// native_menu_draw.cpp:42 and :92-176.
constexpr std::uint32_t kG = 0x50000000, kVs = 0x40010000, kPs = 0x40020000, kDeclaration = 0x40030000, kIndexBuffer = 0x40040000;
// Physical: VS resource base (code at +0, literal values at +0x400), PS
// resource base, indices, vertices and the texture base level.
constexpr std::uint32_t kVsResource = 0x00100000, kPsResource = 0x00200000, kIndices = 0x00300000, kVertices = 0x00400000, kTexture = 0x01000000;
constexpr std::uint32_t kLiteralOffset = 0x400, kAlphaReference = 0x3F000000;
constexpr std::uint32_t kVertexCount = 8, kIndexCount = 12;

void put(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    if (bytes.size() < offset + 4)
        bytes.resize(offset + 4);
    store(bytes, offset, value);
}

// The 0x3500-byte shadow offset of a hardware register GuestDrawRegisters reads.
std::size_t shadowOf(std::uint32_t reg)
{
    for (const auto [first, shadow, count] : { std::array<std::uint32_t, 3>{ 0x2000, 0x2880, 16 }, { 0x2080, 0x28C0, 3 }, { 0x2100, 0x28CC, 21 }, { 0x2180, 0x2920, 5 }, { 0x2200, 0x2934, 12 }, { 0x2300, 0x29B8, 38 } })
        if (reg >= first && reg < first + count)
            return shadow + (reg - first) * 4;
    throw std::runtime_error(std::format("no shadow word for {:#x}", reg));
}

std::uint32_t literal(std::uint32_t i)
{
    return 0x40000000U + i * 0x00010003U;
}

std::array<std::uint32_t, 6> textureFetch(rex::graphics::xenos::TextureFormat format, std::uint32_t width, std::uint32_t height, unsigned mipMaxLevel = 0)
{
    namespace xenos = rex::graphics::xenos;
    xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.type           = xenos::FetchConstantType::kTexture;
    fetch.clamp_x        = xenos::ClampMode::kRepeat;
    fetch.clamp_y        = xenos::ClampMode::kClampToEdge;
    fetch.pitch          = width / 32;
    fetch.format         = format;
    fetch.endianness     = xenos::Endian::k8in16;
    fetch.base_address   = kTexture >> 12;
    fetch.size_2d.width  = width - 1;
    fetch.size_2d.height = height - 1;
    fetch.mag_filter     = xenos::TextureFilter::kLinear;
    fetch.min_filter     = xenos::TextureFilter::kLinear;
    fetch.mip_filter     = xenos::TextureFilter::kBaseMap;
    fetch.mip_max_level  = mipMaxLevel;
    fetch.dimension      = xenos::DataDimension::k2DOrStacked;
    std::array<std::uint32_t, 6> words{};
    std::memcpy(words.data(), &fetch, sizeof(words));
    return words;
}

struct Options
{
    std::array<std::uint32_t, 6>              fetch      = textureFetch(rex::graphics::xenos::TextureFormat::k_DXT1, 1024, 256);
    std::vector<std::array<std::uint32_t, 2>> records    = { { 252, 16 } }; // first float4, dword count
    std::uint32_t                             tableBytes = 20;
};

// PS code: an EXEC (address 1) running one 2D texture fetch from constant 0.
std::vector<std::uint8_t> menuPixelCode()
{
    std::vector<std::uint8_t> code(24);
    put(code, 0, 1 | 1U << 12 | 1U << 16);
    put(code, 4, 0x1000);
    put(code, 8, 0x5678);
    put(code, 12, 1);
    put(code, 20, 1U << 14);
    return code;
}

// Guest memory at the synthetic draw's entry.
FakeMemory memoryFor(const Options& options = {})
{
    FakeMemory memory;
    // VS code: an EXEC (address 1) bounds the control flow and runs the three
    // fetch slots the patch fills.
    std::vector<std::uint8_t> vsCode(48);
    put(vsCode, 0, 1 | 3U << 12 | 0x15U << 16);
    put(vsCode, 4, 0x1000);
    const auto psCode = menuPixelCode();
    memory.Add(kVsResource, true, vsCode);
    memory.Add(kPsResource, true, psCode);

    // VS object: header to 912, metadata at +0x100 + 872, literal table at
    // +0x200 + 872 (header words 896 and 892), fetch templates at +68.
    std::vector<std::uint8_t> vsHeader(912);
    put(vsHeader, 32, kVsResource);
    put(vsHeader, 892, 0x200);
    put(vsHeader, 896, 0x100);
    memory.Add(kVs, false, vsHeader);
    std::vector<std::uint8_t> vsMetadata(48);
    put(vsMetadata, 4, static_cast<std::uint32_t>(vsCode.size()));
    put(vsMetadata, 8, 0x10210204); // SQ_PROGRAM_CNTL with the PS word's zero
    put(vsMetadata, 12, 8);         // SQ_CONTEXT_MISC
    put(vsMetadata, 28, 3);         // fetch records at (word 24 + 9) * 4 = 36
    // Position (usage 0), texcoord 0 (usage 5) and normal (usage 3) patch
    // instruction slots 1, 2 and 3; no record opens a coalescing group.
    put(vsMetadata, 36, 0x00000001);
    put(vsMetadata, 40, 0x00005002);
    put(vsMetadata, 44, 0x00003003);
    memory.Add(kVs + 0x100 + 872, false, vsMetadata);
    std::vector<std::uint8_t> table(20);
    put(table, 16, options.tableBytes);
    for (std::size_t i = 0; i < options.records.size(); ++i)
    {
        put(table, 20 + i * 8, (options.records[i][0] << 16) | options.records[i][1]);
        put(table, 24 + i * 8, kLiteralOffset + std::uint32_t(i) * 0x100);
    }
    table.resize(std::max<std::size_t>(table.size(), 20 + options.tableBytes), 0);
    memory.Add(kVs + 0x200 + 872, false, table);
    for (std::size_t r = 0; r < options.records.size(); ++r)
    {
        std::vector<std::uint8_t> values;
        for (std::uint32_t i = 0; i < options.records[r][1]; ++i)
            put(values, i * 4, literal(std::uint32_t(r) * 16 + i));
        memory.Add(kVsResource + kLiteralOffset + r * 0x100, true, values);
    }

    // PS object: header to 68 (metadata at +0x100 + 40), no literal table.
    std::vector<std::uint8_t> psHeader(68), psMetadata(32);
    put(psHeader, 24, kPsResource);
    put(psHeader, 64, 0x100);
    put(psMetadata, 4, static_cast<std::uint32_t>(psCode.size()));
    put(psMetadata, 24, 0x10007); // SQ_INTERPOLATOR_CNTL
    memory.Add(kPs, false, psHeader);
    memory.Add(kPs + 0x100 + 40, false, psMetadata);

    // Declaration: float3 position at 0, float2 UV at 12, float3 normal at 20.
    std::vector<std::uint8_t> declaration(52 + 36);
    put(declaration, 24, 3);
    const std::array<std::array<std::uint32_t, 3>, 3> elements{ { { 0, 0x001A23B9, 0 }, { 12, 0x002C23A5, 5 }, { 20, 0x001A23B9, 3 } } };
    for (unsigned i = 0; i < 3; ++i)
    {
        put(declaration, 52 + i * 12, elements[i][0]);
        put(declaration, 52 + i * 12 + 4, elements[i][1]);
        declaration[52 + i * 12 + 9] = static_cast<std::uint8_t>(elements[i][2]);
    }
    memory.Add(kDeclaration, false, declaration);
    memory.Add(0x82091394, false, std::vector<std::uint8_t>(16));
    memory.Add(0x820913A8, false, std::vector<std::uint8_t>(64));

    // Index buffer object: BE16 (endian 1) at kIndices; two quads.
    std::vector<std::uint8_t> indexBuffer(32), indices;
    put(indexBuffer, 0, 0x20000000);
    put(indexBuffer, 24, kIndices);
    memory.Add(kIndexBuffer, false, indexBuffer);
    for (const std::uint16_t index : { 0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7 })
        indices.insert(indices.end(), { std::uint8_t(index >> 8), std::uint8_t(index) });
    memory.Add(kIndices, true, indices);
    std::vector<std::uint8_t> vertices;
    for (std::uint32_t v = 0; v < kVertexCount; ++v)
        for (const float value : { float(v % 2) * 100.0F, float(v / 2) * 50.0F, 0.5F, 0.25F * float(v), 0.75F, 0.0F, 0.0F, 1.0F })
            put(vertices, vertices.size(), std::bit_cast<std::uint32_t>(value));
    memory.Add(kVertices, true, vertices);

    // Texture fetch 0's base level.
    NativeTextureMemoryRange range;
    std::string              error;
    if (!GetNativeTextureMemoryRange(options.fetch, range, error))
        throw std::runtime_error("synthetic texture range: " + error);
    std::vector<std::uint8_t> texels(range.size);
    for (std::size_t i = 0; i < texels.size(); ++i)
        texels[i] = static_cast<std::uint8_t>(i * 7 + 3);
    memory.Add(range.address, true, texels);

    // The device shadow: bindings, fixed state, fetches, constants, viewport.
    std::vector<std::uint8_t> state(0x3500);
    put(state, 0x3194, kPs);
    put(state, 0x3198, kVs);
    put(state, 0x2E2C, kDeclaration);
    put(state, 0x3094, kIndexBuffer);
    state[0x30F0] = kSceneMenuStride / 4;
    put(state, 0x778, kVertices | 3);
    put(state, 0x77C, (std::uint32_t(vertices.size()) / 4) << 2 | 2);
    for (std::size_t i = 0; i < options.fetch.size(); ++i)
        put(state, 0x480 + i * 4, options.fetch[i]);
    for (const auto [reg, value] : kSceneMenuFixedState)
        put(state, shadowOf(reg), value);
    put(state, shadowOf(0x210E), kAlphaReference);
    // VS constants c0..c255: c252..c255 hold stale words the literal replaces.
    for (std::uint32_t i = 0; i < 1024; ++i)
        put(state, 0x780 + i * 4, i >= 1008 ? 0xDEAD0000U + i : 0x3E000000U + i);
    NativeDrawReplayViewport viewport;
    NativeDrawReplayScissor  scissor;
    if (!DecodeNativeGameplayViewportAndScissor(GuestDrawRegisters(state), viewport, scissor, error))
        throw std::runtime_error("synthetic viewport: " + error);
    for (unsigned i = 0; const float value : { viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth })
        put(state, 0x3168 + i++ * 4, std::bit_cast<std::uint32_t>(value));
    memory.Add(kG, false, state);
    return memory;
}

const std::array<std::uint32_t, 8> kTuple{ kG, 4, 0, 0, kIndexCount, 0, 0, 0 };

std::string pixelDigest()
{
    return IndexedBufferMicrocodeWordDigest(menuPixelCode());
}

IndexedBufferDrawInputs readMenu(FakeMemory& memory, const std::string& digest)
{
    IndexedBufferDrawInputs inputs;
    std::string             error;
    UNSCOPED_INFO(std::string("synthetic menu inputs: ") + (error));
    REQUIRE((ReadIndexedBufferDrawInputs(meshSubmission(kTuple), readerFor(memory), digest, inputs, error, IndexedBufferForm::Menu) && inputs.inputsComplete));
    return inputs;
}

std::string dxilDigest(const std::vector<std::uint8_t>& bytes)
{
    return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

// The menu interface the catalog loader admits, over synthetic words and DXIL.
NativeGameplayShaderInterface syntheticShader(const std::string& vertexDigest, const std::string& pixelDigest)
{
    NativeGameplayShaderInterface shader;
    shader.vertexShaderHash      = kSceneMenuVertexShaderHash;
    shader.pixelShaderHash       = kSceneMenuPixelShaderHash;
    shader.vertexShaderDxil      = { 'D', 'X', 'B', 'C' };
    shader.pixelShaderDxil       = { 'D', 'X', 'B', 'P' };
    shader.vertexDxilDigest      = dxilDigest(shader.vertexShaderDxil);
    shader.pixelDxilDigest       = dxilDigest(shader.pixelShaderDxil);
    shader.vertexGuestWordDigest = vertexDigest;
    shader.pixelGuestWordDigest  = pixelDigest;
    for (std::size_t i = 0; i < kSceneMenuAttributeOffsets.size(); ++i)
        shader.vertexAttributes.push_back({ 95, kSceneMenuStride, std::int32_t(kSceneMenuAttributeOffsets[i]), i == 1 ? 37U : 57U, true, false });
    shader.textureFetchSlots   = { 0 };
    shader.textureDimensions   = { NativeDrawReplayTextureDimension::Texture2D };
    shader.alphaTestEnabled    = true;
    shader.alphaTestCompare    = 4;
    shader.rasterizedDepth     = true;
    shader.independentSamplers = true;
    shader.shaderProgram       = 0x10210204;
    shader.shaderContext       = 8;
    shader.shaderInterpolators = 0x10007;
    return shader;
}

const std::vector<std::uint8_t> kCatalogVs{ 'D', 'X', 'B', 'C' }, kCatalogPs{ 'D', 'X', 'B', 'P' };

// A fresh catalog directory holding both synthetic DXIL files and `text`.
std::filesystem::path writeCatalog(const std::string& name, const std::string& text)
{
    const auto directory = std::filesystem::temp_directory_path() / ("scene-menu-catalog-" + name);
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "vs.dxil", std::ios::binary).write(reinterpret_cast<const char*>(kCatalogVs.data()), kCatalogVs.size());
    std::ofstream(directory / "ps.dxil", std::ios::binary).write(reinterpret_cast<const char*>(kCatalogPs.data()), kCatalogPs.size());
    std::ofstream(directory / "shaders.toml", std::ios::binary) << text;
    return directory;
}

// A leader-form band entry over the same DXIL: two float3 attributes at
// stride 24 and one 2D texture.
std::string bandEntry(const std::string& pixelHash, const std::string& pixelWords, const std::string& alphaTest = "true", std::uint32_t format = 57)
{
    return std::format("\n[[shaders]]\nvertex_hash = \"1111111111111111\"\npixel_hash = \"{}\"\nvertex_word_sha256 = \"{}\"\npixel_word_sha256 = \"{}\"\n"
                       "vertex_dxil_sha256 = \"{}\"\npixel_dxil_sha256 = \"{}\"\nvertex_file = \"vs.dxil\"\npixel_file = \"ps.dxil\"\nalpha_test = {}\n"
                       "alpha_compare = 4\nrasterized_depth = true\nindependent_samplers = true\nprogram = 813762568\ncontext = 8\ninterpolators = 16712191\n"
                       "texture_fetches = [0]\ntexture_dimensions = [\"2d\"]\n"
                       "[[shaders.attributes]]\nfetch = 95\nstride_bytes = 24\noffset_bytes = 0\nformat = 57\nsigned = true\nnormalized = false\n"
                       "[[shaders.attributes]]\nfetch = 95\nstride_bytes = 24\noffset_bytes = 12\nformat = {}\nsigned = true\nnormalized = false\n",
                       pixelHash,
                       std::string(64, 'a'),
                       pixelWords,
                       dxilDigest(kCatalogVs),
                       dxilDigest(kCatalogPs),
                       alphaTest,
                       format);
}

// The pinned backdrop entry's catalog text.
std::string backdropText()
{
    return std::format(
        "schema_version = 1\n\n[[shaders]]\nvertex_hash = \"C3BAB94E67553E12\"\npixel_hash = \"64D8E6A475FFCB2D\"\n"
        "vertex_word_sha256 = \"{}\"\npixel_word_sha256 = \"{}\"\nvertex_dxil_sha256 = \"{}\"\npixel_dxil_sha256 = \"{}\"\n"
        "vertex_file = \"vs.dxil\"\npixel_file = \"ps.dxil\"\nalpha_test = true\nalpha_compare = 4\nrasterized_depth = true\n"
        "independent_samplers = true\nprogram = 270598660\ncontext = 8\ninterpolators = 65543\ntexture_fetches = [0]\ntexture_dimensions = [\"2d\"]\n"
        "[[shaders.attributes]]\nfetch = 95\nstride_bytes = 32\noffset_bytes = 0\nformat = 57\nsigned = true\nnormalized = false\n"
        "[[shaders.attributes]]\nfetch = 95\nstride_bytes = 32\noffset_bytes = 12\nformat = 37\nsigned = true\nnormalized = false\n"
        "[[shaders.attributes]]\nfetch = 95\nstride_bytes = 32\noffset_bytes = 20\nformat = 57\nsigned = true\nnormalized = false\n",
        kSceneMenuVertexWordDigest,
        kSceneMenuPixelWordDigest,
        dxilDigest(kCatalogVs),
        dxilDigest(kCatalogPs));
}

// A synthetic menu catalog with the pinned identity; `edit` replaces one text.
std::string loadCatalog(const std::string& name, const std::string& from = {}, const std::string& to = {})
{
    auto text = backdropText();
    if (!from.empty())
    {
        const auto at = text.find(from);
        UNSCOPED_INFO("catalog text found for " + name);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
    }
    const auto                    directory = writeCatalog(name, text);
    NativeGameplayShaderInterface shader;
    std::string                   error;
    const bool                    loaded = LoadSceneMenuDrawShader(directory, shader, error);
    std::filesystem::remove_all(directory);
    if (loaded && (shader.vertexShaderHash != kSceneMenuVertexShaderHash || shader.textureFetchSlots != std::vector<std::uint32_t>{ 0 }))
        return "loaded the wrong entry";
    return loaded ? std::string("loaded") : error;
}

void unit()
{
    std::string error;

    // Catalog: the pinned entry loads; each rule refuses one edit.
    UNSCOPED_INFO("the pinned menu catalog entry loads: " + loadCatalog("pinned"));
    REQUIRE(loadCatalog("pinned") == "loaded");
    for (const auto& [name, from, to, expected] : std::vector<std::array<std::string, 4>>{
             { "other-pair", "pixel_hash = \"64D8E6A475FFCB2D\"", "pixel_hash = \"64D8E6A475FFCB2E\"", "scene_mesh_menu_catalog_entry" },
             { "vertex-digest", std::string(kSceneMenuVertexWordDigest), "0" + std::string(kSceneMenuVertexWordDigest).substr(1), "scene_mesh_menu_catalog_word_digest" },
             { "pixel-digest", std::string(kSceneMenuPixelWordDigest), "0" + std::string(kSceneMenuPixelWordDigest).substr(1), "scene_mesh_menu_catalog_word_digest" },
             { "no-alpha-test", "alpha_test = true", "alpha_test = false", "scene_mesh_menu_catalog_interface" },
             { "alpha-less", "alpha_compare = 4", "alpha_compare = 1", "scene_mesh_menu_catalog_interface" },
             { "texture-slot-1", "texture_fetches = [0]", "texture_fetches = [1]", "scene_mesh_menu_catalog_interface" },
             { "cube", "texture_dimensions = [\"2d\"]", "texture_dimensions = [\"cube\"]", "scene_mesh_menu_catalog_interface" },
             { "program", "program = 270598660", "program = 270598661", "scene_mesh_menu_catalog_interface" },
             { "context", "context = 8", "context = 4", "scene_mesh_menu_catalog_interface" },
             { "interpolators", "interpolators = 65543", "interpolators = 65537", "scene_mesh_menu_catalog_interface" },
             { "screen-space", "rasterized_depth = true\n", "rasterized_depth = true\nscreen_space_position = true\n", "scene_mesh_menu_catalog_interface" },
             { "two-targets", "rasterized_depth = true\n", "rasterized_depth = true\ncolor_targets = 2\ncolor1_format = 2\n", "scene_mesh_menu_catalog_color_targets" },
             { "stride-28", "stride_bytes = 32\noffset_bytes = 20", "stride_bytes = 28\noffset_bytes = 20", "scene_mesh_menu_catalog_layout" },
             { "uv-at-16", "offset_bytes = 12", "offset_bytes = 16", "scene_mesh_menu_catalog_layout" },
             { "normal-float4", "offset_bytes = 20\nformat = 57", "offset_bytes = 20\nformat = 38", "scene_mesh_menu_catalog_layout" },
             { "fetch-94", "fetch = 95\nstride_bytes = 32\noffset_bytes = 12", "fetch = 94\nstride_bytes = 32\noffset_bytes = 12", "scene_mesh_menu_catalog_layout" },
             { "unsigned", "offset_bytes = 0\nformat = 57\nsigned = true", "offset_bytes = 0\nformat = 57\nsigned = false", "scene_mesh_menu_catalog_layout" },
             { "normalized", "format = 37\nsigned = true\nnormalized = false", "format = 37\nsigned = true\nnormalized = true", "scene_mesh_menu_catalog_layout" },
         })
    {
        const auto reason = loadCatalog(name, from, to);
        UNSCOPED_INFO(name + ": " + reason);
        REQUIRE(reason == expected);
    }
    std::cout << "Menu catalog loads; entry, digest, interface, target and layout refusals are named\n";

    // Band entries: every entry but the backdrop is a leader-form pair drawn
    // from the scene call site.
    {
        const std::string first = std::string(64, 'b'), second = std::string(64, 'c');
        const auto        load = [](const std::string& name, const std::string& text, SceneMenuDrawCatalog& catalog)
        {
            std::string error;
            const auto  directory = writeCatalog(name, text);
            const bool  loaded    = LoadSceneMenuDrawCatalog(directory, catalog, error);
            std::filesystem::remove_all(directory);
            return loaded ? std::string("loaded") : error;
        };
        SceneMenuDrawCatalog catalog;
        UNSCOPED_INFO("a backdrop-only catalog loads with an empty band");
        REQUIRE((load("band-none", backdropText(), catalog) == "loaded" && catalog.backdrop && catalog.backdrop->pixelGuestWordDigest == kSceneMenuPixelWordDigest &&
                 catalog.band && catalog.band->entries.empty() && catalog.band->caller == kSceneMeshDrawCaller));
        UNSCOPED_INFO("the backdrop and two band pairs load, each band pair with its own interface");
        REQUIRE((load("band-two", backdropText() + bandEntry("2222222222222222", first) + bandEntry("3333333333333333", second), catalog) == "loaded" &&
                 catalog.backdrop->vertexShaderHash == kSceneMenuVertexShaderHash && catalog.band->entries.size() == 2 && catalog.band->caller == kSceneMeshDrawCaller &&
                 catalog.band->entries[1].shader.pixelGuestWordDigest == second && catalog.band->entries[1].stride == 24 &&
                 catalog.band->entries[1].attributeOffsets == std::vector<std::uint32_t>{ 0, 12 }));
        for (const auto& [name, text, expected] : std::vector<std::array<std::string, 3>>{
                 { "band-only", "schema_version = 1\n" + bandEntry("2222222222222222", first), "scene_mesh_menu_catalog_entry" },
                 { "band-backdrop-ps", backdropText() + bandEntry("2222222222222222", std::string(kSceneMenuPixelWordDigest)), "scene_mesh_menu_catalog_duplicate_pixel" },
                 { "band-duplicate", backdropText() + bandEntry("2222222222222222", first) + bandEntry("3333333333333333", first), "leader_catalog_duplicate_pixel" },
                 { "band-no-alpha", backdropText() + bandEntry("2222222222222222", first, "false"), "leader_catalog_interface" },
                 { "band-float4", backdropText() + bandEntry("2222222222222222", first, "true", 38), "leader_catalog_layout" },
             })
        {
            const auto reason = load(name, text, catalog);
            UNSCOPED_INFO(name + ": " + reason);
            REQUIRE((reason == expected && !catalog.backdrop && !catalog.band));
        }
        const auto       directory = writeCatalog("band-registry", backdropText() + bandEntry("2222222222222222", first));
        NativeDrawFamily family;
        std::string      summary;
        UNSCOPED_INFO("the registration admits the backdrop and the band: " + summary + error);
        REQUIRE((SceneMenuDrawFamily().admit(directory, ReadMemory{}, family, summary, error) && family && summary == "(backdrop and 1 leader-band pairs)"));
        std::filesystem::remove_all(directory);
    }
    std::cout << "Menu catalog band pairs load as a scene-caller leader catalog; band refusals are named\n";

    // Reader: the Menu form reads the literal records and texture fetch 0.
    const auto psDigest = pixelDigest();
    auto       memory   = memoryFor();
    auto       inputs   = readMenu(memory, psDigest);
    UNSCOPED_INFO("the menu reader keeps the common inputs and no raw literal bytes");
    REQUIRE((inputs.sceneCandidate && inputs.arguments.stride == 32 && inputs.indexCount == kIndexCount && inputs.streams == std::vector<std::uint32_t>{ 0 } &&
             inputs.streamBytes[0].size() == kVertexCount * 32 && inputs.vertexLiterals.empty() && inputs.pixelLiterals.empty()));
    UNSCOPED_INFO("the VS record lands at 0x43F0..0x43FF");
    REQUIRE((inputs.vertexLiteralRanges.size() == 1 && inputs.vertexLiteralRanges[0].firstFloat4 == 252 && inputs.vertexLiteralRanges[0].dwordCount == 16 &&
             inputs.vertexLiteralRanges[0].physical == kVsResource + kLiteralOffset && inputs.pixelLiteralRanges.empty() &&
             inputs.vertexLiteralRegisters.size() == 16 && inputs.vertexLiteralRegisters[0] == std::make_pair(0x43F0U, literal(0)) &&
             inputs.vertexLiteralRegisters[15] == std::make_pair(0x43FFU, literal(15))));
    UNSCOPED_INFO("texture fetch 0 and its BC1 base level are read");
    REQUIRE((inputs.textureFetch == Options{}.fetch && inputs.textureAddress == kTexture && inputs.textureBytes.size() == 1024 * 256 / 2));
    {
        // The prepass form's raw table read refuses the 20-byte table.
        IndexedBufferDrawInputs raw;
        UNSCOPED_INFO(std::string("the raw literal form refuses a 20-byte table: ") + (error));
        REQUIRE((!ReadIndexedBufferDrawInputs(meshSubmission(kTuple), readerFor(memory), psDigest, raw, error) && error == "indexed_buffer_literal_table_extent"));
        UNSCOPED_INFO("other PS words are foreign to the menu form");
        REQUIRE((!ReadIndexedBufferDrawInputs(meshSubmission(kTuple), readerFor(memory), kSceneMenuPixelWordDigest, raw, error, IndexedBufferForm::Menu) && error.empty() &&
                 !raw.sceneCandidate));
        Options shortTable;
        shortTable.tableBytes = 4;
        auto broken           = memoryFor(shortTable);
        UNSCOPED_INFO(std::string("a refused literal table is named: ") + (error));
        REQUIRE((!ReadIndexedBufferDrawInputs(meshSubmission(kTuple), readerFor(broken), psDigest, raw, error, IndexedBufferForm::Menu) &&
                 error == "indexed_buffer_literals: shader_literal_extent" && raw.sceneCandidate));
        auto untyped = memoryFor();
        put(untyped.At(kG, false).bytes, 0x480, Options{}.fetch[0] & ~3U);
        UNSCOPED_INFO(std::string("a non-texture fetch 0 is named: ") + (error));
        REQUIRE((!ReadIndexedBufferDrawInputs(meshSubmission(kTuple), readerFor(untyped), psDigest, raw, error, IndexedBufferForm::Menu) &&
                 error.starts_with("indexed_buffer_texture_range: ")));
        auto unmapped = memoryFor();
        unmapped.At(kTexture, true).bytes.resize(4096);
        UNSCOPED_INFO(std::string("short texture bytes are named: ") + (error));
        REQUIRE((!ReadIndexedBufferDrawInputs(meshSubmission(kTuple), readerFor(unmapped), psDigest, raw, error, IndexedBufferForm::Menu) &&
                 error == "indexed_buffer_texture_extent"));
    }
    std::cout << "Menu reader: literal records, texture fetch 0 and its refusals are named\n";

    // The synthetic identity: the PS words, and the VS words after the fold
    // and the fetch patch.
    std::vector<std::uint8_t> folded, effective, patched;
    folded = inputs.vertexCode;
    UNSCOPED_INFO(std::string("synthetic fold: ") + (error));
    REQUIRE(FoldSolidShaderMetadata(inputs, folded, effective, error));
    SolidDrawInputs linked = inputs;
    linked.vertexCode      = folded;
    UNSCOPED_INFO(std::string("synthetic fetch patch: ") + (error));
    REQUIRE(PatchVertexFetch(linked, kSceneMenuAttributeOffsets, patched, error));
    const auto              vsDigest = IndexedBufferMicrocodeWordDigest(patched);
    const SceneMeshIdentity identity{ vsDigest, psDigest };
    const auto              shader = syntheticShader(vsDigest, psDigest);

    SceneMenuDrawRecipe recipe;
    UNSCOPED_INFO("the pinned identity finds the synthetic PS foreign");
    REQUIRE((!BuildSceneMenuDrawRecipe(inputs, shader, recipe, error) && error.empty()));
    UNSCOPED_INFO(std::string("synthetic menu recipe: ") + (error));
    REQUIRE(BuildSceneMenuDrawRecipe(inputs, shader, recipe, error, nullptr, identity));
    const auto& native = recipe.native;
    UNSCOPED_INFO("one 1280x720 4-sample RGBA8 target, mask 9, with 12 indices over 8 three-attribute vertices");
    REQUIRE((recipe.patchedVertexCode == patched && native.width == 1280 && native.height == 720 && native.sampleCount == 4 && native.sampleMask == 9 &&
             native.extraTargetCount == 0 && native.targetFormat == NativeDrawReplayTargetFormat::Rgba8 && native.indexCount == kIndexCount &&
             native.vertexAttributeCount == 3 && native.vertexBytes().size() == kVertexCount * 3 * 16));
    UNSCOPED_INFO("depth test and write LessEqual, stencil off, back faces culled with CCW fronts");
    REQUIRE((native.depth.enabled && native.depth.writeEnabled && native.depth.compare == 4 && !native.stencil.enabled && native.rasterizer.cull == 2 &&
             native.rasterizer.frontCounterClockwise));
    UNSCOPED_INFO("blend SRC_ALPHA/INV_SRC_ALPHA, alpha ONE/ZERO, RT0 mask 0xF");
    REQUIRE((native.blend.enabled && native.blend.sourceColor == NativeDrawReplayBlendFactor::SourceAlpha &&
             native.blend.destinationColor == NativeDrawReplayBlendFactor::InverseSourceAlpha && native.blend.sourceAlpha == NativeDrawReplayBlendFactor::One &&
             native.blend.destinationAlpha == NativeDrawReplayBlendFactor::Zero && native.blend.writeMask == 0xF));
    UNSCOPED_INFO("the alpha reference reaches shared byte 300");
    REQUIRE((native.sharedConstants.size() >= 304 && std::uint32_t(native.sharedConstants[300]) == (kAlphaReference & 0xFF) &&
             std::uint32_t(native.sharedConstants[303]) == kAlphaReference >> 24));
    UNSCOPED_INFO("texture fetch 0 binds the BC1 1024x256 level with its linear repeat/clamp sampler");
    REQUIRE((native.textureMask == 1 && native.textures[0].format == NativeDrawReplayTextureFormat::Bc1 && native.textures[0].width == 1024 &&
             native.textures[0].height == 256 && native.samplers.size() == 1 && native.samplers[0].magLinear &&
             native.samplers[0].address == std::array<std::uint32_t, 3>{ 1, 3, 3 }));
    const auto constant = [&](std::size_t index)
    {
        const auto* bytes = native.vertexConstants.data() + index * 4;
        return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) | (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
    };
    bool overlaid = native.vertexConstants.size() == 4096 && constant(1007) == 0x3E000000U + 1007;
    for (std::uint32_t i = 0; overlaid && i < 16; ++i)
        overlaid = constant(1008 + i) == literal(i);
    UNSCOPED_INFO("the literal record replaces c252..c255 and leaves c251 from the shadow");
    REQUIRE(overlaid);
    {
        // BC2 2048x512 is the other admitted form.
        Options bc2;
        bc2.fetch      = textureFetch(rex::graphics::xenos::TextureFormat::k_DXT2_3, 2048, 512);
        auto other     = memoryFor(bc2);
        auto bc2Inputs = readMenu(other, psDigest);
        UNSCOPED_INFO(std::string("the BC2 2048x512 form builds: ") + (error));
        REQUIRE((BuildSceneMenuDrawRecipe(bc2Inputs, shader, recipe, error, nullptr, identity) &&
                 recipe.native.textures[0].format == NativeDrawReplayTextureFormat::Bc2 && recipe.native.textures[0].width == 2048));
    }
    std::cout << "Synthetic menu draw builds: depth, cull, alpha reference, blend, BC texture and literal overlay\n";

    // Named refusals, one rule each.
    const auto refused = [&](const std::string& name, auto mutate, const std::string& expected, std::optional<IndexedBufferDrawInputs> base = std::nullopt, const NativeGameplayShaderInterface* other = nullptr)
    {
        auto broken = base ? *base : inputs;
        mutate(broken);
        SceneMenuDrawRecipe rejected;
        std::string         reason;
        const bool          built = BuildSceneMenuDrawRecipe(broken, other ? *other : shader, rejected, reason, nullptr, identity);
        UNSCOPED_INFO(name + ": " + (built ? "accepted" : reason));
        REQUIRE((!built && reason.starts_with(expected)));
    };
    const auto none = [](IndexedBufferDrawInputs&)
    {
    };
    refused("incomplete inputs", [](IndexedBufferDrawInputs& in)
            {
                in.inputsComplete = false;
            },
            "scene_mesh_inputs_incomplete");
    refused("another caller", [](IndexedBufferDrawInputs& in)
            {
                in.arguments.caller = 0x8250CB98;
            },
            "scene_mesh_unsupported_draw_tuple");
    refused("a triangle strip", [](IndexedBufferDrawInputs& in)
            {
                in.primitive = 6;
            },
            "scene_mesh_unsupported_draw_tuple");
    refused("a nonzero base vertex", [](IndexedBufferDrawInputs& in)
            {
                in.baseVertex = 1;
            },
            "scene_mesh_unsupported_draw_tuple");
    refused("32-bit indices", [](IndexedBufferDrawInputs& in)
            {
                in.indexFormat = 1;
            },
            "scene_mesh_unsupported_draw_tuple");
    refused("index endian 2", [](IndexedBufferDrawInputs& in)
            {
                in.indexEndian = 2;
            },
            "scene_mesh_unsupported_draw_tuple");
    refused("a partial triangle", [](IndexedBufferDrawInputs& in)
            {
                in.indexCount = 11;
                in.indexBytes.resize(22);
            },
            "scene_mesh_unsupported_draw_tuple");
    refused("indices short of the count", [](IndexedBufferDrawInputs& in)
            {
                in.indexBytes.resize(22);
            },
            "scene_mesh_unsupported_draw_tuple");
    refused("a predicated shader", [](IndexedBufferDrawInputs& in)
            {
                in.state[0x2ABC] |= 0x40;
            },
            "scene_mesh_conditional_or_recorded_shader");
    refused("a recorded shader", [](IndexedBufferDrawInputs& in)
            {
                in.state[0x2ABE] |= 0x40;
            },
            "scene_mesh_conditional_or_recorded_shader");
    refused("two fetch records", [](IndexedBufferDrawInputs& in)
            {
                store(in.vertexMetadata, 28, 2);
            },
            "scene_mesh_fetch_count");
    refused("a two-element declaration", [](IndexedBufferDrawInputs& in)
            {
                in.declaration.resize(24);
            },
            "scene_mesh_fetch_count");
    refused("stride 28", [](IndexedBufferDrawInputs& in)
            {
                in.arguments.stride = 28;
            },
            "scene_mesh_stream_form");
    refused("the prepass stride 20", [](IndexedBufferDrawInputs& in)
            {
                in.arguments.stride = 20;
            },
            "scene_mesh_stream_form");
    refused("a second stream", [](IndexedBufferDrawInputs& in)
            {
                in.streams.push_back(1);
                in.streamFetches.push_back(in.streamFetches[0]);
                in.streamBytes.push_back(in.streamBytes[0]);
            },
            "scene_mesh_stream_form");
    refused("no literal record", [](IndexedBufferDrawInputs& in)
            {
                in.vertexLiteralRanges.clear();
                in.vertexLiteralRegisters.clear();
            },
            "scene_mesh_literal_shape");
    refused("a pixel literal record", [](IndexedBufferDrawInputs& in)
            {
                in.pixelLiteralRanges.push_back({ 508, 16, 0x1000 });
            },
            "scene_mesh_literal_shape");
    refused("a record at c248", [](IndexedBufferDrawInputs& in)
            {
                in.vertexLiteralRanges[0].firstFloat4 = 248;
            },
            "scene_mesh_literal_shape");
    refused("an 8-dword record", [](IndexedBufferDrawInputs& in)
            {
                in.vertexLiteralRanges[0].dwordCount = 8;
                in.vertexLiteralRegisters.resize(8);
            },
            "scene_mesh_literal_shape");
    refused("a literal register off its record", [](IndexedBufferDrawInputs& in)
            {
                in.vertexLiteralRegisters[3].first = 0x43EF;
            },
            "scene_mesh_literal_shape");
    refused("a second range beside the record's registers", [](IndexedBufferDrawInputs& in)
            {
                in.vertexLiteralRanges.push_back({ 248, 16, 0x1000 });
            },
            "scene_mesh_literal_shape");
    {
        // A second literal record, as the reader returns it.
        Options twoRecords;
        twoRecords.records    = { { 252, 16 }, { 248, 16 } };
        twoRecords.tableBytes = 28;
        auto two              = memoryFor(twoRecords);
        auto twoInputs        = readMenu(two, psDigest);
        UNSCOPED_INFO("the reader returns both records");
        REQUIRE(twoInputs.vertexLiteralRanges.size() == 2);
        refused("a second literal record", none, "scene_mesh_literal_shape", twoInputs);
    }
    auto twoTargets             = shader;
    twoTargets.colorTargetCount = 2;
    refused("a two-target interface", none, "scene_mesh_color_targets", std::nullopt, &twoTargets);
    refused("a changed VS instruction", [](IndexedBufferDrawInputs& in)
            {
                in.vertexCode[8] ^= 1;
            },
            "scene_mesh_patched_vs_identity");
    refused("an unadmitted UV offset", [](IndexedBufferDrawInputs& in)
            {
                store(in.declaration, 12, 16);
            },
            "adapter_attribute_offset");

    // Every pinned register, one change each; the program words come from the
    // shader metadata through the fold.
    for (const auto& pinnedWord : kSceneMenuFixedState)
    {
        const auto reg     = pinnedWord.first;
        const auto changed = reg == 0x2208 ? pinnedWord.second ^ 0x100U : pinnedWord.second ^ 1U;
        const auto mutate  = [&](IndexedBufferDrawInputs& in)
        {
            if (reg == 0x2180)
                store(in.vertexMetadata, 8, changed);
            else if (reg == 0x2181)
                store(in.vertexMetadata, 12, changed);
            else if (reg == 0x2182)
                store(in.pixelMetadata, 24, changed);
            else
                store(in.state, shadowOf(reg), changed);
        };
        refused(std::format("register {:#x}", reg), mutate, std::format("scene_mesh_fixed_state {:#x}: {:#x}", reg, changed));
    }
    refused("the alpha test off", [](IndexedBufferDrawInputs& in)
            {
                store(in.state, shadowOf(0x2202), 0x87000004);
            },
            "scene_mesh_fixed_state 0x2202: 0x87000004");
    refused("depth writes off", [](IndexedBufferDrawInputs& in)
            {
                store(in.state, shadowOf(0x2200), 0x24F00732);
            },
            "scene_mesh_fixed_state 0x2200: 0x24f00732");
    refused("culling off", [](IndexedBufferDrawInputs& in)
            {
                store(in.state, shadowOf(0x2205), 0x18000);
            },
            "scene_mesh_fixed_state 0x2205: 0x18000");
    refused("the prepass blend", [](IndexedBufferDrawInputs& in)
            {
                store(in.state, shadowOf(0x2201), 0x00010001);
            },
            "scene_mesh_fixed_state 0x2201: 0x10001");

    refused("a fetch other than the shadow's", [](IndexedBufferDrawInputs& in)
            {
                in.textureFetch[3] ^= 1U << 19;
            },
            "scene_mesh_texture_fetch_mismatch");
    const auto withFetch = [&](const std::array<std::uint32_t, 6>& fetch)
    {
        Options options;
        options.fetch = fetch;
        auto at       = memoryFor(options);
        return readMenu(at, psDigest);
    };
    {
        namespace xenos = rex::graphics::xenos;
        refused("a mipmapped texture", none, "scene_mesh_texture_mipmapped", withFetch(textureFetch(xenos::TextureFormat::k_DXT1, 1024, 256, 1)));
        auto border = textureFetch(xenos::TextureFormat::k_DXT1, 1024, 256);
        border[0] |= std::uint32_t(xenos::ClampMode::kClampToBorder) << 10;
        refused("border addressing", none, "scene_mesh_sampler_clamp_mode", withFetch(border));
        refused("BC1 at 512x256", none, "scene_mesh_texture_form", withFetch(textureFetch(xenos::TextureFormat::k_DXT1, 512, 256)));
        refused("BC2 at 1024x256", none, "scene_mesh_texture_form", withFetch(textureFetch(xenos::TextureFormat::k_DXT2_3, 1024, 256)));
        refused("RGBA8 at 1024x256", none, "scene_mesh_texture_form", withFetch(textureFetch(xenos::TextureFormat::k_8_8_8_8, 1024, 256)));
    }
    refused("short texture bytes", [](IndexedBufferDrawInputs& in)
            {
                in.textureBytes.resize(in.textureBytes.size() / 2);
            },
            "scene_mesh_texture_decode: ");
    refused("a viewport shadow that disagrees", [](IndexedBufferDrawInputs& in)
            {
                store(in.state, 0x3168 + 8, 0x44000000);
            },
            "scene_mesh_viewport_shadow_scale_mismatch");
    auto noAlphaTest             = shader;
    noAlphaTest.alphaTestEnabled = false;
    refused("an interface without the alpha test", none, "scene_mesh_recipe_rejected: ", std::nullopt, &noAlphaTest);
    // The live microcode's fetches decide; an entry that disagrees is refused.
    auto float2Normal                       = shader;
    float2Normal.vertexAttributes[2].format = 37;
    refused("an entry reading the normal as a float2", none, "scene_mesh_vertex_fetch_layout", std::nullopt, &float2Normal);
    auto slotOne              = shader;
    slotOne.textureFetchSlots = { 1 };
    refused("an entry binding fetch 1", none, "scene_mesh_texture_fetch_layout", std::nullopt, &slotOne);
    std::cout << "Menu tuple, stream, literal, target, VS identity, fixed-state, texture and sampler refusals are named\n";

    // The family: foreign without reads for other callers, "not mine" for
    // other PS words, and the synthetic draw through the menu form.
    NativeDrawReplayRecipe draw;
    std::string            detail;
    UNSCOPED_INFO("a null shader leaves no menu family");
    REQUIRE(!MakeSceneMenuDrawBuilder(readerFor(memory), nullptr));
    auto pinned = MakeSceneMenuDrawBuilder(readerFor(memory), std::make_shared<NativeGameplayShaderInterface>(shader));
    UNSCOPED_INFO("the pinned family finds the synthetic PS not its own");
    REQUIRE((!pinned(meshSubmission(kTuple), draw, detail) && detail.empty()));
    auto family  = MakeSceneMenuDrawBuilder(readerFor(memory), std::make_shared<NativeGameplayShaderInterface>(shader), identity);
    memory.reads = 0;
    UNSCOPED_INFO("another IndexedBuffer caller is foreign without reads");
    REQUIRE((!family(meshSubmission(kTuple, 0x8250CB98), draw, detail) && detail.empty() && memory.reads == 0));
    UNSCOPED_INFO(std::string("the family admits the synthetic draw: ") + (detail));
    REQUIRE((family(meshSubmission(kTuple), draw, detail) && detail.empty() && draw.textures[0].format == NativeDrawReplayTextureFormat::Bc1));
    const auto first = draw.textures[0].textureVersion;
    UNSCOPED_INFO("a held texture reuses its decoded version");
    REQUIRE((family(meshSubmission(kTuple), draw, detail) && first && draw.textures[0].textureVersion == first));
    // With band pairs, a backdrop draw is still the backdrop's, built or refused.
    LeaderDrawCatalog band;
    band.caller                                             = kSceneMeshDrawCaller;
    band.entries.emplace_back().shader.pixelGuestWordDigest = std::string(64, 'b');
    auto catalogFamily                                      = MakeSceneMenuCatalogFamily(readerFor(memory), { std::make_shared<NativeGameplayShaderInterface>(shader), std::make_shared<LeaderDrawCatalog>(band) }, identity);
    UNSCOPED_INFO(std::string("the catalog family builds the backdrop draw: ") + (detail));
    REQUIRE(catalogFamily(meshSubmission(kTuple), draw, detail) == NativeDrawFamilyResult::Built);
    store(memory.At(kG, false).bytes, shadowOf(0x2205), 0x18000);
    UNSCOPED_INFO(std::string("the family names a refusal: ") + (detail));
    REQUIRE((!family(meshSubmission(kTuple), draw, detail) && detail == "scene_mesh_fixed_state 0x2205: 0x18000"));
    UNSCOPED_INFO(std::string("the catalog family keeps the backdrop's refusal: ") + (detail));
    REQUIRE((catalogFamily(meshSubmission(kTuple), draw, detail) == NativeDrawFamilyResult::Refused && detail == "scene_mesh_fixed_state 0x2205: 0x18000"));
    // The prepass family still owns its own PS and finds the menu draw foreign.
    auto boot = MakeBootDrawBuilder(readerFor(memory), nullptr, nullptr, nullptr, std::make_shared<NativeGameplayShaderInterface>(shader));
    UNSCOPED_INFO("the prepass form finds the menu draw foreign");
    REQUIRE((!boot(meshSubmission(kTuple), draw, detail) && detail.empty()));
    std::cout << "Menu family: selection, admission, texture reuse and named refusal checks passed\n";
}

} // namespace menu

NativeFrameSwap swapRecord()
{
    NativeFrameSwap swap;
    swap.sequence            = 1;
    swap.frontbufferPhysical = 0x1F000000;
    swap.fetch               = { 2, 0x1F000000 | 6, 1279 | (719U << 13), 0xD10, 0, 0 };
    swap.textureFormat       = 6;
    swap.width               = 1280;
    swap.height              = 720;
    return swap;
}

// The renderer stream as the completion source drives it.
struct RendererProducer final : INativeFrameProducer
{
    NativeRendererD3D12& renderer;
    ID3D12Fence*         fence = nullptr;

    explicit RendererProducer(NativeRendererD3D12& value)
    : renderer(value)
    , fence(value.AcquireStreamFence())
    {
        UNSCOPED_INFO("the started stream exposes its fence");
        REQUIRE(fence != nullptr);
    }

    ~RendererProducer() override
    {
        fence->Release();
    }

    bool QueueFrame(NativeRenderFrame frame, std::shared_future<NativeDrawReplayResult>& completion, std::string& error) override
    {
        std::shared_ptr<NativeRenderFrameTicket> ticket;
        if (!renderer.QueueRenderFrame(std::move(frame), ticket, error))
            return false;
        completion = ticket->Completion();
        return true;
    }

    bool QueuePasses(NativeRenderFrame passes, std::shared_future<NativeDrawReplayResult>& recording, std::string& error) override
    {
        return renderer.QueueRenderFramePasses(std::move(passes), recording, error);
    }

    std::uint64_t CompletedFence() const override
    {
        return fence->GetCompletedValue();
    }
};

// The draw's captured payloads by role.
struct Event
{
    std::map<std::string, std::filesystem::path> files;
    std::map<std::string, std::uint64_t>         addresses;
    std::map<std::string, bool>                  physical;
};

Event loadEvent(const std::filesystem::path& capture)
{
    Event       e;
    const auto  manifest = toml::parse_file((capture / "payload-manifest.toml").string());
    const auto* bindings = manifest["bindings"].as_array();
    UNSCOPED_INFO("payload manifest bindings");
    REQUIRE(bindings != nullptr);
    for (const auto& node : *bindings)
    {
        const auto& binding = *node.as_table();
        if (binding["event"].value_or<std::int64_t>(-1) != std::int64_t(kMesh))
            continue;
        const auto role = binding["role"].value_or(std::string());
        UNSCOPED_INFO("payload error on role " + role);
        REQUIRE(binding["error"].value_or(std::string("x")).empty());
        e.files[role]     = capture / binding["file"].value_or(std::string());
        e.addresses[role] = binding["address"].value_or<std::int64_t>(0);
        e.physical[role]  = binding["physical"].value_or(false);
    }
    for (const auto* role : { "vs_code", "ps_code", "indices", "vertices", "declaration", "fetch_records", "index_header", "state_delta_offset_before_after_be32" })
    {
        UNSCOPED_INFO(std::string("draw 1895 payload ") + role);
        REQUIRE(e.files.contains(role));
    }
    return e;
}

std::vector<std::uint8_t> preState(const std::filesystem::path& capture)
{
    return read(capture / ("draw-" + std::to_string(kMesh) + ".state.be.bin"));
}

// The draw's state after Original: the captured pre-state plus its recorded
// (offset, before, after) word deltas.
std::vector<std::uint8_t> postState(const std::filesystem::path& capture, const Event& e)
{
    auto       state = preState(capture);
    const auto delta = read(e.files.at("state_delta_offset_before_after_be32"));
    UNSCOPED_INFO("state and delta extents");
    REQUIRE((state.size() == 0x3500 && delta.size() % 12 == 0));
    for (std::size_t i = 0; i < delta.size(); i += 12)
    {
        const auto offset = word(delta, i);
        UNSCOPED_INFO("delta before-word matches the pre-state");
        REQUIRE((offset + 4 <= state.size() && word(state, offset) == word(delta, i + 4)));
        store(state, offset, word(delta, i + 8));
    }
    return state;
}

// The bound MRT set's surface objects. +0x18 and +0x1C are the resolve source
// headers of events 1898..1902; +0x24 is the 448x720 tile extent
// (RB_SURFACE_INFO pitch 480, tile rects 448/416/416).
constexpr std::uint32_t kRt0 = 0x50263120, kRt1 = 0x5044C6B0, kDepth = 0x5044C390;

void addSurfaces(FakeMemory& memory)
{
    for (const auto [object, info] : { std::pair{ kRt0, 0U }, { kRt1, 0x0002021CU }, { kDepth, 0x5E4U } })
    {
        std::vector<std::uint8_t> surface(0x28);
        store(surface, 0x18, 0x070101E0);
        store(surface, 0x1C, info);
        store(surface, 0x24, (447U << 18) | (719U << 3));
        memory.Add(object, false, surface);
    }
}

// Guest memory at the draw's entry.
FakeMemory memoryAt(const std::filesystem::path& capture, const Event& e)
{
    FakeMemory memory;
    for (const auto& [role, path] : e.files)
        if (!role.starts_with("state_"))
            memory.Add(e.addresses.at(role), e.physical.at(role), read(path));
    memory.Add(kG, false, preState(capture));
    return memory;
}

std::vector<std::uint32_t> registersOf(std::span<const std::uint8_t> state)
{
    std::vector<std::uint32_t> registers(0x5003);
    for (const auto [from, to, count] : { std::array<unsigned, 3>{ 0x2880, 0x2000, 16 }, { 0x28C0, 0x2080, 3 }, { 0x28CC, 0x2100, 21 }, { 0x2920, 0x2180, 5 }, { 0x2934, 0x2200, 12 }, { 0x29B8, 0x2300, 38 }, { 0x780, 0x4000, 1024 }, { 0x1780, 0x4400, 896 }, { 0x2780, 0x4900, 40 } })
        for (unsigned i = 0; i < count; ++i)
            registers[to + i] = word(state, from + i * 4);
    return registers;
}

std::vector<std::uint32_t> words(const std::vector<std::uint8_t>& bytes)
{
    std::vector<std::uint32_t> result(bytes.size() / 4);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = word(bytes, i * 4);
    return result;
}

std::string catalogText(const std::filesystem::path& directory)
{
    std::ifstream in(directory / "shaders.toml", std::ios::binary);
    return { std::istreambuf_iterator<char>(in), {} };
}

// Loads a copy of the catalog with one text edit and returns the refusal.
std::string mutatedCatalog(const std::filesystem::path& catalog, const std::string& name, const std::string& from, const std::string& to)
{
    const auto copy = std::filesystem::temp_directory_path() / ("scene-mesh-catalog-" + name);
    std::filesystem::remove_all(copy);
    std::filesystem::copy(catalog, copy, std::filesystem::copy_options::recursive);
    auto       text = catalogText(copy);
    const auto at   = text.find(from);
    UNSCOPED_INFO("catalog text found for " + name);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    {
        std::ofstream out(copy / "shaders.toml", std::ios::binary | std::ios::trunc);
        out << text;
    }
    NativeGameplayShaderInterface rejected;
    std::string                   error;
    const bool                    loaded = LoadSceneMeshDrawShader(copy, rejected, error);
    std::filesystem::remove_all(copy);
    return loaded ? std::string("loaded") : error;
}

// The frame 428 pass: three tiles of 448, 416 and 416 columns, and per tile
// depth, RT0 and RT1 resolves (events 1898..1914).
constexpr std::uint32_t                kWidth = 1280, kHeight = 720, kStorage = 1280 * 736 * 4;
constexpr std::array<GuestRect, 3>     kRects{ { { 0, 0, 448, 720 }, { 448, 0, 864, 720 }, { 864, 0, 1280, 720 } } };
constexpr std::array<std::uint32_t, 3> kSlots{ kGuestDepthSlot, 0, 1 };
// RB_COLOR_CLEAR and RB_DEPTH_CLEAR are not in the capture: opaque black as
// a packed 8888 word for both color slots (not black as 2_10_10_10, design
// open question 2) and depth 1.0 with stencil 0.
constexpr std::uint32_t kColorClear = 0xFF000000, kDepthClear = 0xFFFFFF00;

struct Destination
{
    std::array<std::uint32_t, 6> fetch;
    std::uint32_t                physical, copyFormat, endian;
};

// In slot order: the C10 destination descriptors as the guest holds them
// (depth in the A0000000 view, colors in E0000000) and Resolve's physical
// copy bases.
constexpr std::array<Destination, 3> kDestinations{ {
    { { 0x8A000002, 0xBD0F0096, 0x0059E4FF, 0x1690, 0x280, 0xBD488A00 }, 0x1D0F0000, 6, 2 },
    { { 0x8A000002, 0xFD817006, 0x0059E4FF, 0x1414, 0, 0x200 }, 0x1D818000, 6, 0 },
    { { 0x8A000002, 0xFD47F036, 0x0059E4FF, 0xD10, 0, 0x200 }, 0x1D480000, 7, 0 },
} };

struct Mutation
{
    const char* name           = "";
    bool        rt1Rgba8       = false; // the draw's RT1 format
    bool        flipWinding    = false; // the first triangle's index order
    bool        dropBackground = false; // tile 0's RT1 resolve without its clear bit
    bool        cullFront      = false; // front faces culled instead of back faces
    bool        rt1Masked      = false; // RT1's write mask cleared
};

GuestResolveCopy copyFor(std::uint32_t tile, std::size_t d, const Mutation& mutation)
{
    const auto&      destination = kDestinations[d];
    const auto       slot        = kSlots[d];
    std::uint32_t    clearBits   = 0;
    GuestResolveCopy copy;
    // Color copies RT0 with color and depth clear bits, RT1 with its color bit,
    // on every tile but the last (events 1900/1902 flags 0x300 and 0x101).
    if (tile < 2 && slot == 0)
        clearBits = 0x300;
    else if (tile < 2 && slot == 1 && !(mutation.dropBackground && tile == 0))
        clearBits = 0x100;
    copy.copyControl = 0x00100000 | slot | (slot == kGuestDepthSlot ? 0x10 : 0x40) | clearBits;
    copy.destBase    = destination.physical;
    copy.destPitch   = (kHeight << 16) | kWidth;
    copy.destInfo    = (destination.copyFormat << 7) | destination.endian;
    copy.destFetch   = destination.fetch;
    copy.rectX = copy.destX = kRects[tile].x0;
    copy.rectWidth          = kRects[tile].x1 - kRects[tile].x0;
    copy.rectHeight         = kHeight;
    copy.colorClear         = kColorClear;
    copy.depthClear         = kDepthClear;
    return copy;
}

using GuestImages = std::array<std::vector<std::uint8_t>, 3>;

GuestImages sentinelImages()
{
    GuestImages images;
    for (auto& bytes : images)
        bytes.assign(kStorage, 0xA5);
    return images;
}

void apply(const GuestResolveImage& image, std::vector<std::uint8_t>& memory)
{
    if (image.runs.empty())
    {
        UNSCOPED_INFO("the image lies inside the destination");
        REQUIRE(image.bytes.size() <= memory.size());
        std::copy(image.bytes.begin(), image.bytes.end(), memory.begin());
        return;
    }
    for (const auto& run : image.runs)
    {
        UNSCOPED_INFO("a run lies inside the destination");
        REQUIRE(run.offset + run.length <= memory.size());
        std::copy_n(image.bytes.begin() + run.offset, run.length, memory.begin() + run.offset);
    }
}

// Control replay uses captured state, patched VS words and geometry
// independently of reader, fold and assembler.
NativeDrawReplayRecipe controlDraw(const std::filesystem::path& capture, const Event& e, const NativeGameplayShaderInterface& shader, const std::filesystem::path& catalog)
{
    const auto state     = postState(capture, e);
    auto       registers = registersOf(state);
    UNSCOPED_INFO("RT1 blends as RT0 with both mask nibbles set");
    REQUIRE((registers[0x2209] == registers[0x2201] && registers[0x2104] == 0xFF));
    const std::array<std::uint32_t, 2> fetch{ word(state, 0x778), word(state, 0x77C) };
    registers[0x48BE] = fetch[0];
    registers[0x48BF] = fetch[1];
    // The draw conversion binds RT0 alone at the presented surface pitch.
    registers[0x2000]                                           = (registers[0x2000] & ~0x3FFFU) | 640;
    registers[0x2104]                                           = 0xF;
    const auto                                         vertices = read(e.files.at("vertices"));
    const auto                                         indices  = read(e.files.at("indices"));
    const auto                                         vs = words(read(catalog / "vs.ucode.be.bin")), ps = words(read(e.files.at("ps_code")));
    const std::array<NativeGameplayVertexFetchSpan, 1> fetches{ { { 95, fetch[0] & ~3U, vertices } } };
    NativeGameplayDrawView                             view;
    view.registers       = registers;
    view.vertexMicrocode = vs;
    view.pixelMicrocode  = ps;
    view.vertexFetches   = fetches;
    view.indexBytes      = indices;
    view.primitive       = 4;
    view.indexCount      = 6;
    view.indexFormat     = 0;
    NativeGameplayDrawTarget target;
    target.fullViewportTarget = true;
    target.width              = kWidth;
    target.height             = kHeight;
    target.viewport           = { 0, 0, 1280, 720, 0, 1 };
    target.scissor            = { 0, 0, 1280, 720 };
    NativeDrawReplayRecipe recipe;
    std::string            error;
    UNSCOPED_INFO(std::string("control recipe: ") + (error));
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, target, recipe, error));
    recipe.extraTargetCount       = 1;
    recipe.extraTargets[0].format = NativeDrawReplayTargetFormat::Rgb10a2;
    recipe.extraTargets[0].blend  = recipe.blend;
    return recipe;
}

// One producer frame per resolve, as the guest replays the pass per tile:
// the paired clears on tile 0, the clear words earlier resolves left on later
// tiles, the draw, then the copy's samples as guest words.
GuestImages controlImages(const std::optional<NativeDrawReplayRecipe>& draw)
{
    using Target = NativeRenderFramePass::Target;
    const Target set{ kWidth, kHeight, 4, NativeDrawReplayTargetFormat::Rgba8, 0, 0x5E4, 1, { NativeDrawReplayTargetFormat::Rgb10a2 }, { 0x21C } };
    const auto   rect = [](std::uint32_t tile)
    {
        return NativeDrawReplayScissor{ std::int32_t(kRects[tile].x0), std::int32_t(kRects[tile].y0), std::int32_t(kRects[tile].x1), std::int32_t(kRects[tile].y1) };
    };
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(std::string("control stream: ") + (error));
    REQUIRE(renderer.StartDrawStream(error));
    auto images = sentinelImages();
    for (std::uint32_t tile = 0; tile < 3; ++tile)
        for (std::size_t d = 0; d < kSlots.size(); ++d)
        {
            const auto            slot = kSlots[d];
            NativeRenderFramePass pass;
            pass.target = set;
            NativeRenderFramePass::Clear rt0, rt1;
            rt0.rectangle = rt1.rectangle = rect(0);
            rt0.color = rt0.depth = rt0.stencil = rt1.color = true;
            rt0.colorTargets                                = 1;
            rt1.colorTargets                                = 2;
            rt0.colorValue = rt1.colorValue = { 0, 0, 0, 1 };
            rt0.depthValue                  = 1;
            pass.clears                     = { rt0, rt1 };
            for (std::uint32_t later = 1; later <= tile; ++later)
            {
                // 0xFF000000 as 8888 is opaque black; as 2_10_10_10 it is blue
                // 0x3F0 with alpha 3. 0xFFFFFF00 is depth 1.0, stencil 0.
                NativeRenderFramePass::Clear color0, color1, depth;
                color0.rectangle = color1.rectangle = depth.rectangle = rect(later);
                color0.color = color1.color = depth.depth = depth.stencil = true;
                color0.colorTargets                                       = 1;
                color1.colorTargets                                       = 2;
                color0.colorValue                                         = { 0, 0, 0, 1 };
                color1.colorValue                                         = { 0, 0, 1008.0F / 1023.0F, 1 };
                depth.depthValue                                          = 1;
                pass.clears.insert(pass.clears.end(), { color0, depth, color1 });
            }
            if (draw)
                pass.draws.push_back(*draw);
            NativeRenderFrameResolve resolve;
            resolve.textureId   = 1;
            resolve.width       = kRects[tile].x1 - kRects[tile].x0;
            resolve.height      = kHeight;
            resolve.depth       = slot == kGuestDepthSlot;
            resolve.colorTarget = resolve.depth ? 0 : slot;
            resolve.sourceX     = kRects[tile].x0;
            // Depth copies guest sample 1 (k1, host sample 3); color averages both.
            resolve.samples    = resolve.depth ? std::array<std::uint32_t, 2>{ 3, 3 } : std::array<std::uint32_t, 2>{ 0, 3 };
            resolve.guestWords = resolve.depth ? NativeGuestWordPacking::Depth24Stencil8 : slot == 1 ? NativeGuestWordPacking::Color2101010
                                                                                                     : NativeGuestWordPacking::Color8888;
            pass.resolves.push_back(resolve);
            NativeRenderFrame frame;
            frame.passes.push_back(std::move(pass));
            frame.textures.push_back({ 1, resolve.width, kHeight, NativeDrawReplayTextureFormat::R32Uint, 100 + tile * 3 + d });
            frame.outputTextureId = 1;
            frame.retainOnly = frame.returnRawOutput = true;
            const auto raw                           = renderer.SubmitRenderFrame(std::move(frame));
            UNSCOPED_INFO(std::format("control tile {} slot {}: {}", tile, slot, raw.error));
            REQUIRE(raw.success);
            auto copy    = copyFor(tile, d, {});
            copy.packing = resolve.depth ? GuestResolvePacking::Depth24Stencil8Words : slot == 1 ? GuestResolvePacking::Color2101010Words
                                                                                                 : GuestResolvePacking::Color8888Words;
            GuestResolveImage image;
            UNSCOPED_INFO(std::string(std::format("control tile {} slot {} encode: ", tile, slot)) + (error));
            REQUIRE(EncodeGuestResolve(copy, raw.rawOutput, raw.rawRowPitch, image, error));
            apply(image, images[d]);
        }
    renderer.Shutdown();
    return images;
}

// The pass through the boot builder, assembler, completion source and
// encoder; a refusal anywhere returns it instead of images.
std::optional<GuestImages> assembledImages(const std::filesystem::path& capture, const Event& e, const NativeGameplayShaderInterface& meshShader, const Mutation& mutation, std::string& refusal)
{
    auto memory = memoryAt(capture, e);
    addSurfaces(memory);
    if (mutation.flipWinding)
    {
        auto& indices = memory.At(e.addresses.at("indices"), true).bytes;
        std::swap_ranges(indices.begin() + 2, indices.begin() + 4, indices.begin() + 4);
    }
    const auto            read       = readerFor(memory);
    const GuestWordReader readTarget = [&memory](std::uint32_t address, std::uint32_t& value)
    {
        const auto bytes = memory.Read(address, 4, false);
        if (bytes.size() != 4)
            return false;
        value = word(bytes, 0);
        return true;
    };
    auto boot = MakeBootDrawBuilder(read, nullptr, nullptr, nullptr, std::make_shared<NativeGameplayShaderInterface>(meshShader));

    // The set as the guest decodes it, under the predicate each operation had.
    const auto decoded = [&](std::uint32_t slot, std::uint32_t mask)
    {
        GuestTarget target;
        std::string why;
        UNSCOPED_INFO(std::string(std::format("set slot {} decodes tiled: ", slot)) + (why));
        REQUIRE((ReadGuestTargetSet(readTarget, kG, slot, target, &why) && target.tiling));
        target.tiling->predicateMask = mask;
        target.tiling->predicated    = 1;
        return target;
    };
    const auto clear = [&](std::uint32_t index)
    {
        GuestClear value;
        value.target             = decoded(index, 3);
        value.flags              = index ? 0x0F : 0x3F;
        value.index              = index;
        value.predicate          = 1;
        value.tiling             = *value.target.tiling;
        value.clear.rectangle    = { 0, 0, std::int32_t(kWidth), std::int32_t(kHeight) };
        value.clear.color        = true;
        value.clear.depth        = index == 0;
        value.clear.stencil      = index == 0;
        value.clear.colorTargets = static_cast<std::uint8_t>(1U << index);
        value.clear.colorValue   = { 0, 0, 0, 1 };
        value.clear.depthValue   = 1;
        GuestDepthSurface depth;
        UNSCOPED_INFO("the set's depth surface decodes tiled");
        REQUIRE((ReadGuestDepthSurface(readTarget, kG, value.tiling, depth) && depth.tiled));
        value.depth = depth;
        return value;
    };

    NativeRendererD3D12 stream;
    std::string         error;
    UNSCOPED_INFO(std::string("assembled stream: ") + (error));
    REQUIRE(stream.StartDrawStream(error));
    auto images = sentinelImages();
    bool ok     = true;
    {
        auto                       producer = std::make_shared<RendererProducer>(stream);
        D3D12CompletionSource      source({ producer });
        NativeFrameAssemblerConfig config;
        config.completion = &source;
        config.readTarget = readTarget;
        config.build      = [&](const NativeDrawSubmission& submission, NativeDrawReplayRecipe& draw, std::string& detail)
        {
            if (!boot(submission, draw, detail))
                return false;
            if (mutation.rt1Rgba8)
                draw.extraTargets[0].format = NativeDrawReplayTargetFormat::Rgba8;
            if (mutation.cullFront)
                draw.rasterizer.cull = 1;
            if (mutation.rt1Masked)
                draw.extraTargets[0].blend.writeMask = 0;
            return true;
        };
        NativeFrameAssembler assembler(config);
        UNSCOPED_INFO(std::string("the paired clears: ") + (error));
        REQUIRE((assembler.Clear(clear(0), error) && assembler.Clear(clear(1), error)));
        auto submission      = meshSubmission(kMeshTuple);
        submission.stateRead = true;
        submission.sequence  = 1;
        assembler.Began(submission);
        const auto verdict = assembler.Admit(submission);
        assembler.Recorded(submission, verdict);
        if (verdict)
        {
            // The strict epoch names the refusal at its swap.
            auto swap            = swapRecord();
            swap.numberedThrough = 1;
            refusal              = assembler.Swap(swap).detail;
            ok                   = false;
        }
        for (std::uint32_t tile = 0; ok && tile < 3; ++tile)
            for (std::size_t d = 0; ok && d < kSlots.size(); ++d)
            {
                const auto copy    = copyFor(tile, d, mutation);
                bool       partial = false;
                const auto mark    = assembler.Resolve(decoded(kSlots[d], 3U << (2 * tile)), copy, false, partial, error);
                if (!mark)
                {
                    refusal = std::format("tile {} slot {}: {}", tile, kSlots[d], error);
                    ok      = false;
                    break;
                }
                const auto flush = assembler.FlushThrough(mark);
                UNSCOPED_INFO(std::format("tile {} slot {} completes: {}", tile, kSlots[d], flush.error));
                REQUIRE((flush.fence && source.WaitFor(flush.fence, std::chrono::seconds(10))));
                const auto take = assembler.TakeGuestImage(mark);
                UNSCOPED_INFO(std::format("tile {} slot {} image: {}", tile, kSlots[d], take.error));
                REQUIRE((take.image && take.image->physical == copy.destBase));
                apply(*take.image, images[d]);
            }
        UNSCOPED_INFO("the pass leaves no failure");
        REQUIRE((!ok || (!assembler.Failure() && !source.Failure())));
    }
    stream.Shutdown();
    if (!ok)
        return std::nullopt;
    return images;
}

std::size_t differingBytes(const GuestImages& left, const GuestImages& right)
{
    std::size_t count = 0;
    for (std::size_t d = 0; d < left.size(); ++d)
        for (std::size_t i = 0; i < left[d].size(); ++i)
            count += left[d][i] != right[d][i];
    return count;
}

int c10(const std::filesystem::path& capture)
{
    const auto catalog = environment("REREVVED_SCENE_MESH_SHADERS");
    UNSCOPED_INFO("the scene mesh checks also need REREVVED_SCENE_MESH_SHADERS (the MRT prepass shader catalog)");
    REQUIRE(!catalog.empty());
    std::string                   error;
    const auto                    e = loadEvent(capture);
    NativeGameplayShaderInterface meshShader;
    UNSCOPED_INFO(std::string("mesh catalog: ") + (error));
    REQUIRE(LoadSceneMeshDrawShader(catalog, meshShader, error));

    // Catalog refusals, one rule each.
    const auto catalogRefusal = [&](const char* name, const std::string& from, const std::string& to, const std::string& expected)
    {
        const auto reason = mutatedCatalog(catalog, name, from, to);
        UNSCOPED_INFO(std::string(name) + ": " + reason);
        REQUIRE(reason == expected);
    };
    catalogRefusal("single-target", "color_targets = 2\ncolor1_format = 2\n", "", "scene_mesh_catalog_color_targets");
    catalogRefusal("rt1-rgba8", "color1_format = 2", "color1_format = 0", "scene_mesh_catalog_color_targets");
    catalogRefusal("three-targets", "color_targets = 2", "color_targets = 3", "unsupported gameplay shader color targets");
    catalogRefusal("orphan-rt1-format", "color_targets = 2\n", "", "unsupported gameplay shader color targets");
    catalogRefusal("pixel-word-digest", std::string(kSceneMeshPixelWordDigest), "1" + std::string(kSceneMeshPixelWordDigest).substr(1), "scene_mesh_catalog_word_digest");
    catalogRefusal("unsigned-uv", "offset_bytes = 12\nformat = 37\nsigned = true", "offset_bytes = 12\nformat = 37\nsigned = false", "scene_mesh_catalog_layout");
    catalogRefusal("alpha-test", "alpha_test = false", "alpha_test = true", "scene_mesh_catalog_interface");
    catalogRefusal("rt1-format-1", "color1_format = 2", "color1_format = 1", "unsupported gameplay shader color targets");
    std::cout << "Mesh catalog loads; color target, word digest, layout and interface refusals are named\n";

    // Reader: the inputs equal the C10 payloads.
    auto                    memory = memoryAt(capture, e);
    IndexedBufferDrawInputs inputs;
    UNSCOPED_INFO(std::string("mesh inputs: ") + (error));
    REQUIRE((ReadIndexedBufferDrawInputs(meshSubmission(kMeshTuple), readerFor(memory), kSceneMeshPixelWordDigest, inputs, error) && inputs.sceneCandidate &&
             inputs.inputsComplete));
    const auto declaration = read(e.files.at("declaration"));
    UNSCOPED_INFO("reader inputs equal the C10 payloads");
    REQUIRE((inputs.vertexCode == read(e.files.at("vs_code")) && inputs.pixelCode == read(e.files.at("ps_code")) &&
             inputs.indexBytes == read(e.files.at("indices")) && inputs.streamBytes.size() == 1 && inputs.streamBytes[0] == read(e.files.at("vertices")) &&
             inputs.fetchRecords == read(e.files.at("fetch_records")) && inputs.declaration == std::vector<std::uint8_t>(declaration.begin() + 52, declaration.end())));
    UNSCOPED_INFO("reader tuple, index buffer and stream 0 fetch equal C10's");
    REQUIRE((inputs.primitive == 4 && inputs.baseVertex == 0 && inputs.startIndex == 0 && inputs.indexCount == 6 && inputs.indexBuffer == 0x401D3CA0 &&
             inputs.indexAddress == 0x1FB15000 && inputs.indexFormat == 0 && inputs.indexEndian == 1 && inputs.streams == std::vector<std::uint32_t>{ 0 } &&
             inputs.streamFetches == std::vector<std::array<std::uint32_t, 2>>{ std::array<std::uint32_t, 2>{ 0x1FB16003, 0x10000052 } } && inputs.arguments.stride == 20 &&
             inputs.vertexObject == 0x5048FAA0 && inputs.pixelObject == 0x50300000));
    UNSCOPED_INFO("the PS words are the pinned translator pair's");
    REQUIRE((IndexedBufferMicrocodeWordDigest(inputs.pixelCode) == kSceneMeshPixelWordDigest && XXH3_64bits(inputs.pixelCode.data(), inputs.pixelCode.size()) == kSceneMeshPixelShaderHash));
    UNSCOPED_INFO("the captured VS is the pre-patch word set");
    REQUIRE(XXH3_64bits(inputs.vertexCode.data(), inputs.vertexCode.size()) == 0x773E99EC4AF9407CULL);

    // Foreign: another caller reads nothing; other PS words are foreign.
    memory.reads = 0;
    IndexedBufferDrawInputs foreign;
    UNSCOPED_INFO("another caller is foreign without reads");
    REQUIRE((!ReadIndexedBufferDrawInputs(meshSubmission(kMeshTuple, 0x8250C700), readerFor(memory), kSceneMeshPixelWordDigest, foreign, error) && error.empty() &&
             memory.reads == 0));
    {
        auto other = memoryAt(capture, e);
        other.At(e.addresses.at("ps_code"), true).bytes[3] ^= 1;
        UNSCOPED_INFO("another PS is foreign");
        REQUIRE((!ReadIndexedBufferDrawInputs(meshSubmission(kMeshTuple), readerFor(other), kSceneMeshPixelWordDigest, foreign, error) && error.empty() &&
                 !foreign.sceneCandidate));
    }
    // A later start index reads its own indices from the same buffer.
    {
        auto at    = memoryAt(capture, e);
        auto tuple = kMeshTuple;
        tuple[3]   = 2;
        tuple[4]   = 4;
        IndexedBufferDrawInputs later;
        const auto              indices = read(e.files.at("indices"));
        UNSCOPED_INFO(std::string("start index 2 reads indices 2..5: ") + (error));
        REQUIRE((ReadIndexedBufferDrawInputs(meshSubmission(tuple), readerFor(at), kSceneMeshPixelWordDigest, later, error) && later.indexAddress == 0x1FB15004 &&
                 later.indexBytes == std::vector<std::uint8_t>(indices.begin() + 4, indices.end())));
    }
    // Index buffer flags bit 31 selects 32-bit indices, which the recipe refuses.
    {
        auto at     = memoryAt(capture, e);
        auto header = at.At(e.addresses.at("index_header"), false).bytes;
        header[0] |= 0x80;
        at.At(e.addresses.at("index_header"), false).bytes = header;
        at.At(e.addresses.at("indices"), true).bytes.resize(24);
        IndexedBufferDrawInputs wide;
        UNSCOPED_INFO(std::string("32-bit indices read 24 bytes: ") + (error));
        REQUIRE((ReadIndexedBufferDrawInputs(meshSubmission(kMeshTuple), readerFor(at), kSceneMeshPixelWordDigest, wide, error) && wide.indexFormat == 1 &&
                 indexBytes(wide).size() == 24 && wide.arguments.indexFormat == 4));
        SceneMeshDrawRecipe rejected;
        UNSCOPED_INFO(std::string("32-bit indices are refused: ") + (error));
        REQUIRE((!BuildSceneMeshDrawRecipe(wide, meshShader, rejected, error) && error == "scene_mesh_unsupported_draw_tuple"));
    }
    // Vertex fetch 95 must be a vertex fetch constant (type 3).
    {
        auto at = memoryAt(capture, e);
        at.At(kG, false).bytes[0x778 + 3] &= 0xFC;
        IndexedBufferDrawInputs untyped;
        UNSCOPED_INFO(std::string("a non-vertex fetch 95 is named: ") + (error));
        REQUIRE((!ReadIndexedBufferDrawInputs(meshSubmission(kMeshTuple), readerFor(at), kSceneMeshPixelWordDigest, untyped, error) &&
                 error == "indexed_buffer_vertex_fetch_type" && untyped.sceneCandidate));
    }

    // Patch: fold and fetch patch reproduce the words the translator hash names.
    SceneMeshDrawRecipe recipe;
    UNSCOPED_INFO(std::string("mesh recipe: ") + (error));
    REQUIRE(BuildSceneMeshDrawRecipe(inputs, meshShader, recipe, error));
    UNSCOPED_INFO("the patched VS equals the catalog's guest words and hashes to the Xenos pipeline VS");
    REQUIRE((recipe.patchedVertexCode == read(std::filesystem::path(catalog) / "vs.ucode.be.bin") &&
             XXH3_64bits(recipe.patchedVertexCode.data(), recipe.patchedVertexCode.size()) == kSceneMeshVertexShaderHash));
    {
        auto effective = GuestDrawRegisters(recipe.effectiveState), original = registersOf(postState(capture, e));
        // Original clears RB_HIZCONTROL (0x2203) from 1 to 0 for this
        // depth-off draw. The fold leaves it; no native draw state reads it.
        UNSCOPED_INFO("only Original's HiZ disable separates the fold from C10");
        REQUIRE((effective[0x2203] == 1 && original[0x2203] == 0));
        effective[0x2203] = 0;
        std::string differing;
        for (std::size_t i = 0; i < std::min(effective.size(), original.size()); ++i)
            if (effective[i] != original[i] && differing.size() < 200)
                differing += std::format(" {:#x}: {:#x} for {:#x}", i, effective[i], original[i]);
        UNSCOPED_INFO("effective registers and constants equal C10's Original state:" + differing);
        REQUIRE((effective.size() == original.size() && differing.empty()));
    }
    const auto& native = recipe.native;
    UNSCOPED_INFO("catalog DXIL bound");
    REQUIRE((std::ranges::equal(native.vertexShaderBytes(), meshShader.vertexShaderDxil) && std::ranges::equal(native.pixelShaderBytes(), meshShader.pixelShaderDxil)));
    UNSCOPED_INFO("one 1280x720 4-sample RGBA8 draw with a blended RT1 2_10_10_10, depth and stencil off");
    REQUIRE((native.width == 1280 && native.height == 720 && native.sampleCount == 4 && native.indexCount == 6 && native.extraTargetCount == 1 &&
             native.extraTargets[0].format == NativeDrawReplayTargetFormat::Rgb10a2 && native.extraTargets[0].blend.writeMask == 0xF &&
             native.blend.writeMask == 0xF && !native.depth.enabled && !native.stencil.enabled));
    std::cout << "Mesh reader inputs equal C10's; fold and fetch patch give the translator's VS 3648CFB8645444C7\n";

    // Named refusals on the real inputs, one rule each.
    const auto refused = [&](const char* name, auto mutate, const std::string& expected, const NativeGameplayShaderInterface* shader = nullptr)
    {
        auto broken = inputs;
        mutate(broken);
        SceneMeshDrawRecipe rejected;
        std::string         reason;
        const bool          built = BuildSceneMeshDrawRecipe(broken, shader ? *shader : meshShader, rejected, reason);
        UNSCOPED_INFO(std::string(name) + ": " + (built ? "accepted" : reason));
        REQUIRE((!built && reason.starts_with(expected)));
    };
    refused("incomplete inputs", [](IndexedBufferDrawInputs& in)
            {
                in.inputsComplete = false;
            },
            "scene_mesh_inputs_incomplete");
    refused("another caller", [](IndexedBufferDrawInputs& in)
            {
                in.arguments.caller = 0x8250C700;
            },
            "scene_mesh_unsupported_draw_tuple");
    refused("a partial triangle", [](IndexedBufferDrawInputs& in)
            {
                in.indexCount = 4;
                in.indexBytes.resize(8);
            },
            "scene_mesh_unsupported_draw_tuple");
    refused("indices short of the count", [](IndexedBufferDrawInputs& in)
            {
                in.indexBytes.resize(10);
            },
            "scene_mesh_unsupported_draw_tuple");
    refused("a nonzero base vertex", [](IndexedBufferDrawInputs& in)
            {
                in.baseVertex = 1;
            },
            "scene_mesh_unsupported_draw_tuple");
    refused("32-bit indices", [](IndexedBufferDrawInputs& in)
            {
                in.indexFormat = 1;
            },
            "scene_mesh_unsupported_draw_tuple");
    refused("a triangle strip", [](IndexedBufferDrawInputs& in)
            {
                in.primitive = 6;
            },
            "scene_mesh_unsupported_draw_tuple");
    refused("index endian 0", [](IndexedBufferDrawInputs& in)
            {
                in.indexEndian = 0;
            },
            "scene_mesh_unsupported_draw_tuple");
    refused("a predicated shader", [](IndexedBufferDrawInputs& in)
            {
                in.state[0x2ABC] |= 0x40;
            },
            "scene_mesh_conditional_or_recorded_shader");
    refused("a recorded shader", [](IndexedBufferDrawInputs& in)
            {
                in.state[0x2ABE] |= 0x40;
            },
            "scene_mesh_conditional_or_recorded_shader");
    refused("three fetch records", [](IndexedBufferDrawInputs& in)
            {
                store(in.vertexMetadata, 28, 3);
            },
            "scene_mesh_fetch_count");
    refused("a one-element declaration", [](IndexedBufferDrawInputs& in)
            {
                in.declaration.resize(12);
            },
            "scene_mesh_fetch_count");
    refused("a second stream", [](IndexedBufferDrawInputs& in)
            {
                in.streams.push_back(1);
                in.streamFetches.push_back(in.streamFetches[0]);
                in.streamBytes.push_back(in.streamBytes[0]);
            },
            "scene_mesh_stream_form");
    refused("stride 24", [](IndexedBufferDrawInputs& in)
            {
                in.arguments.stride = 24;
            },
            "scene_mesh_stream_form");
    refused("a resource literal", [](IndexedBufferDrawInputs& in)
            {
                in.pixelLiterals.assign(28, 0);
                in.pixelLiterals[23] = 1;
            },
            "scene_mesh_shader_resource_literals");
    refused("a vertex resource literal", [](IndexedBufferDrawInputs& in)
            {
                in.vertexLiterals.assign(28, 0);
                in.vertexLiterals[22] = 1;
            },
            "scene_mesh_shader_resource_literals");
    refused("a changed VS instruction", [](IndexedBufferDrawInputs& in)
            {
                in.vertexCode[0] ^= 1;
            },
            "scene_mesh_patched_vs_identity");
    refused("an unpinned RT1 blend", [](IndexedBufferDrawInputs& in)
            {
                store(in.state, 0x2958, 0x00010005);
            },
            "scene_mesh_fixed_state 0x2209: 0x10005");
    refused("RT1 masked off", [](IndexedBufferDrawInputs& in)
            {
                store(in.state, 0x28DC, 0x0F);
            },
            "scene_mesh_fixed_state 0x2104: 0xf");
    refused("RT1 as k_8_8_8_8", [](IndexedBufferDrawInputs& in)
            {
                store(in.state, 0x288C, 0x0000021C);
            },
            "scene_mesh_fixed_state 0x2003: 0x21c");
    refused("depth testing on", [](IndexedBufferDrawInputs& in)
            {
                store(in.state, 0x2934, 0x24F00776);
            },
            "scene_mesh_fixed_state 0x2200: 0x24f00776");
    refused("culling off", [](IndexedBufferDrawInputs& in)
            {
                store(in.state, 0x2948, 0x18000);
            },
            "scene_mesh_fixed_state 0x2205: 0x18000");
    refused("a viewport shadow that disagrees", [](IndexedBufferDrawInputs& in)
            {
                store(in.state, 0x3168 + 8, 0x44000000);
            },
            "scene_mesh_viewport_shadow_scale_mismatch");
    auto singleTarget             = meshShader;
    singleTarget.colorTargetCount = 1;
    refused("a single-target catalog entry", [](IndexedBufferDrawInputs&)
            {
            },
            "scene_mesh_color_targets",
            &singleTarget);
    std::cout << "Mesh tuple, stream, literal, VS identity and fixed-state refusals are named\n";

    // The boot builder routes the draw through the mesh family alone.
    {
        auto                   at   = memoryAt(capture, e);
        auto                   boot = MakeBootDrawBuilder(readerFor(at), nullptr, nullptr, nullptr, std::make_shared<NativeGameplayShaderInterface>(meshShader));
        NativeDrawReplayRecipe draw;
        std::string            detail;
        UNSCOPED_INFO(std::string("the boot builder admits draw 1895: ") + (detail));
        REQUIRE((boot(meshSubmission(kMeshTuple), draw, detail) && detail.empty() && draw.extraTargetCount == 1));
        auto without = MakeBootDrawBuilder(readerFor(at), nullptr, nullptr, nullptr);
        UNSCOPED_INFO("without the mesh catalog the draw has no route");
        REQUIRE((!without(meshSubmission(kMeshTuple), draw, detail) && detail.empty()));
        // A loaded GFx family finds the IndexedBuffer draw foreign and falls through.
        auto withGfx = MakeBootDrawBuilder(readerFor(at), nullptr, nullptr, std::make_shared<GfxDrawCatalog>(), std::make_shared<NativeGameplayShaderInterface>(meshShader));
        UNSCOPED_INFO(std::string("the mesh follows a loaded GFx family: ") + (detail));
        REQUIRE((withGfx(meshSubmission(kMeshTuple), draw, detail) && detail.empty()));
    }

    const auto device = environment("REREVVED_TEST_NATIVE_D3D12");
    if (device != "1")
    {
        std::cout << "Set REREVVED_TEST_NATIVE_D3D12=1 to render the MRT pass control\n";
        return 77;
    }
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
    {
        std::cout << "No D3D12 device; the MRT pass was not rendered\n";
        return 77;
    }

    // Acceptance: the nine guest images of the frame 428 pass through the boot
    // builder and assembler equal a control rendered from C10's saved inputs.
    const auto  control  = controlDraw(capture, e, meshShader, catalog);
    const auto  expected = controlImages(control);
    const auto  undrawn  = controlImages(std::nullopt);
    std::string refusal;
    const auto  actual = assembledImages(capture, e, meshShader, {}, refusal);
    UNSCOPED_INFO("the assembled pass: " + refusal);
    REQUIRE(actual.has_value());
    const auto drawn = differingBytes(expected, undrawn);
    std::cout << "Control bytes the prepass draw changes: " << drawn << "\n";
    UNSCOPED_INFO("the control's prepass draw writes the pass");
    REQUIRE(drawn > 0);
    for (std::size_t d = 0; d < kSlots.size(); ++d)
    {
        std::size_t mismatches = 0, first = kStorage;
        for (std::size_t i = 0; i < kStorage; ++i)
            if ((*actual)[d][i] != expected[d][i])
            {
                first = std::min(first, i);
                ++mismatches;
            }
        UNSCOPED_INFO(std::format("destination {} differs from the control at {} bytes, first at {:#x}", d, mismatches, first));
        REQUIRE(mismatches == 0);
        UNSCOPED_INFO("the control wrote the destination");
        REQUIRE(std::count(expected[d].begin(), expected[d].end(), std::uint8_t(0xA5)) < std::ptrdiff_t(kStorage / 2));
    }
    std::cout << "Assembled MRT pass equals the control bit-exactly in all nine guest resolve images\n";

    // The constant-zero PS covers every pixel over uniform clears, so sample
    // selection (k0, k1, k01) cannot change these bytes and is not mutated.
    const std::array<Mutation, 5> mutations{ {
        { "RT1 target RGBA8", true },
        { "first triangle winding flipped", false, true },
        { "tile-1 RT1 background dropped", false, false, true },
        { "front faces culled", false, false, false, true },
        { "RT1 write mask cleared", false, false, false, false, true },
    } };
    for (const auto& mutation : mutations)
    {
        std::string reason;
        const auto  images = assembledImages(capture, e, meshShader, mutation, reason);
        if (!images)
            std::cout << "Mutation '" << mutation.name << "' is refused: " << reason << "\n";
        else
        {
            const auto differing = differingBytes(*images, expected);
            std::cout << "Mutation '" << mutation.name << "' changes " << differing << " bytes\n";
            UNSCOPED_INFO(std::string(mutation.name) + " leaves the control comparison passing");
            REQUIRE(differing > 0);
        }
    }
    std::cout << "Every mutation fails the control comparison\n";
    return 0;
}

// C1 frames 1203-1206 open with scene draws and then UI; frame 1203 is the
// Xenos reference.
namespace title
{

// C1 frames 1203..1206 were captured 600 frames after first menu PS 2cac681f;
// Xenos reference is 1203.
constexpr std::uint64_t kBegin = 1203, kFrame = 1203;
constexpr std::uint32_t kGfxCaller = 0x82303E90, kLabelCaller = 0x82304258, kGfxEntry = 0x826A3568, kLabelEntry = 0x826A3000, kSceneEntry = 0x826A39F8;
// Nested records of the same GFx and label submissions, from the inner lowerers.
constexpr std::uint32_t kGfxInner = 0x826A35B0, kLabelInner = 0x826A3024;

std::uint32_t little(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) | (std::uint32_t(bytes[offset + 2]) << 16) |
           (std::uint32_t(bytes[offset + 3]) << 24);
}

struct Draw
{
    std::uint64_t                                sequence = 0, frame = 0;
    std::uint32_t                                entry = 0, caller = 0, stack = 0;
    std::array<std::uint32_t, 8>                 arguments{}, stackWords{};
    std::map<std::string, std::filesystem::path> files;
    std::map<std::string, std::uint64_t>         addresses;
    std::map<std::string, bool>                  physical;
};

struct Resolve
{
    std::uint64_t                frame = 0;
    std::uint32_t                base  = 0;
    std::array<std::uint32_t, 4> rectangle{};
};

struct Capture
{
    std::vector<Draw>                                    draws;
    std::vector<Resolve>                                 resolves;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> presents; // frame, texture base
    std::uint64_t                                        begin = 0, end = 0;
};

Capture loadCapture(const std::filesystem::path& capture)
{
    Capture    result;
    const auto status = toml::parse_file((capture / "result.toml").string());
    UNSCOPED_INFO("C1 is a complete capture armed on the menu scene PS");
    REQUIRE((status["complete"].value_or(false) && status["payload_complete"].value_or(false) && status["trigger"].value_or(std::string()) == "pixel_digest" &&
             status["arm_pixel_digest"].value_or(std::string()) == std::string(kSceneMenuPixelWordDigest.substr(0, 8))));
    result.begin = std::uint64_t(status["begin_frame"].value_or<std::int64_t>(0));
    result.end   = std::uint64_t(status["end_frame"].value_or<std::int64_t>(0));
    UNSCOPED_INFO("C1 captures frames 1203..1206");
    REQUIRE((result.begin == kBegin && result.end == kBegin + 4));

    const auto  table = toml::parse_file((capture / "events.toml").string());
    const auto* all   = table["events"].as_array();
    UNSCOPED_INFO("capture events");
    REQUIRE(all != nullptr);
    const auto integers = [](const toml::node_view<const toml::node>& node, bool text)
    {
        std::array<std::uint32_t, 8> values{};
        const auto*                  array = node.as_array();
        UNSCOPED_INFO("eight raw words");
        REQUIRE((array && array->size() == 8));
        for (std::size_t i = 0; i < 8; ++i)
            values[i] = text ? static_cast<std::uint32_t>(std::stoull((*array)[i].value_or(std::string("0"))))
                             : static_cast<std::uint32_t>((*array)[i].value_or<std::int64_t>(0));
        return values;
    };
    for (const auto& node : *all)
    {
        const auto event = toml::node_view<const toml::node>(node);
        const auto kind  = event["kind"].value_or(std::string());
        const auto frame = std::uint64_t(event["frame"].value_or<std::int64_t>(0));
        const auto base  = std::uint32_t(event["texture"]["base"].value_or<std::int64_t>(0));
        if (kind == "resolve" && frame + 1 >= kBegin)
        {
            Resolve     resolve{ frame, base };
            const auto* rectangle = event["rectangle"].as_array();
            UNSCOPED_INFO("a resolve rectangle");
            REQUIRE((rectangle && rectangle->size() == 4));
            for (std::size_t i = 0; i < 4; ++i)
                resolve.rectangle[i] = std::uint32_t((*rectangle)[i].value_or<std::int64_t>(-1));
            result.resolves.push_back(resolve);
        }
        else if (kind == "present" && frame >= kBegin)
            result.presents.emplace_back(frame, base);
        else if (kind == "draw")
        {
            Draw draw;
            draw.sequence   = std::uint64_t(event["sequence"].value_or<std::int64_t>(0));
            draw.frame      = frame;
            draw.entry      = std::uint32_t(event["entry"].value_or<std::int64_t>(0));
            draw.caller     = static_cast<std::uint32_t>(std::stoull(event["caller_lr"].value_or(std::string("0"))));
            draw.stack      = std::uint32_t(event["raw_r1"].value_or<std::int64_t>(0));
            draw.arguments  = integers(event["raw_r3_r10_u64"], true);
            draw.stackWords = integers(event["raw_stack_54_70"], false);
            result.draws.push_back(std::move(draw));
        }
    }

    const auto  manifest = toml::parse_file((capture / "payload-manifest.toml").string());
    const auto* bindings = manifest["bindings"].as_array();
    UNSCOPED_INFO("payload manifest bindings");
    REQUIRE(bindings != nullptr);
    for (const auto& node : *bindings)
    {
        const auto& binding = *node.as_table();
        const auto  event   = std::uint64_t(binding["event"].value_or<std::int64_t>(-1));
        const auto  draw    = std::ranges::find(result.draws, event, &Draw::sequence);
        if (draw == result.draws.end())
            continue;
        const auto role = binding["role"].value_or(std::string());
        UNSCOPED_INFO(std::format("one clean {} payload for draw {}", role, event));
        REQUIRE((binding["error"].value_or(std::string("x")).empty() && !draw->files.contains(role)));
        draw->files[role]     = capture / binding["file"].value_or(std::string());
        draw->addresses[role] = std::uint64_t(binding["address"].value_or<std::int64_t>(0));
        draw->physical[role]  = binding["physical"].value_or(false);
    }
    return result;
}

std::vector<std::uint8_t> payload(const Draw& draw, const std::string& role)
{
    UNSCOPED_INFO(std::format("draw {} payload {}", draw.sequence, role));
    REQUIRE(draw.files.contains(role));
    return read(draw.files.at(role));
}

std::vector<std::uint8_t> preState(const std::filesystem::path& capture, const Draw& draw)
{
    return read(capture / std::format("draw-{}.state.be.bin", draw.sequence));
}

std::vector<std::uint8_t> postState(const std::filesystem::path& capture, const Draw& draw)
{
    auto       state = preState(capture, draw);
    const auto delta = payload(draw, "state_delta_offset_before_after_be32");
    UNSCOPED_INFO("state and delta extents");
    REQUIRE((state.size() == 0x3500 && delta.size() % 12 == 0));
    for (std::size_t i = 0; i < delta.size(); i += 12)
    {
        const auto offset = word(delta, i);
        UNSCOPED_INFO("delta before-word matches the pre-state");
        REQUIRE((offset + 4 <= state.size() && word(state, offset) == word(delta, i + 4)));
        store(state, offset, word(delta, i + 8));
    }
    return state;
}

// Guest memory at the draw's entry: its payloads and its pre-state at G.
FakeMemory memoryAt(const std::filesystem::path& capture, const Draw& draw)
{
    FakeMemory memory;
    for (const auto& [role, path] : draw.files)
        if (!role.starts_with("state_"))
            memory.Add(draw.addresses.at(role), draw.physical.at(role), read(path));
    memory.Add(draw.arguments[0], false, preState(capture, draw));
    return memory;
}

NativeDrawSubmission submissionOf(const Draw& draw)
{
    auto submission  = meshSubmission(draw.arguments, draw.caller);
    submission.stack = draw.stack;
    return submission;
}

// The draw through the reader's Menu form and the recipe builder.
SceneMenuDrawRecipe menuRecipe(const std::filesystem::path& capture, const Draw& draw, const NativeGameplayShaderInterface& shader, IndexedBufferDrawInputs& inputs)
{
    auto        memory = memoryAt(capture, draw);
    std::string error;
    UNSCOPED_INFO(std::string(std::format("draw {} menu inputs: ", draw.sequence)) + (error));
    REQUIRE((ReadIndexedBufferDrawInputs(submissionOf(draw), readerFor(memory), kSceneMenuPixelWordDigest, inputs, error, IndexedBufferForm::Menu) &&
             inputs.sceneCandidate && inputs.inputsComplete));
    SceneMenuDrawRecipe recipe;
    UNSCOPED_INFO(std::string(std::format("draw {} menu recipe: ", draw.sequence)) + (error));
    REQUIRE(BuildSceneMenuDrawRecipe(inputs, shader, recipe, error));
    return recipe;
}

// Same rule as the movie check: shader microcode that allocates a memory
// export or has an exec-clause ALU export to eA or eM0-eM4 can write memory.
bool exportsMemory(std::span<const std::uint32_t> ucode)
{
    using namespace rex::graphics::ucode;
    std::size_t                             end = ucode.size() / 3;
    std::vector<ControlFlowExecInstruction> clauses;
    for (std::size_t slot = 0; slot < end; ++slot)
    {
        ControlFlowInstruction pair[2];
        UnpackControlFlowInstructions(ucode.data() + slot * 3, pair);
        for (const auto& instruction : pair)
            if (IsControlFlowOpcodeExec(instruction.opcode()))
            {
                end = std::min<std::size_t>(end, instruction.exec.address());
                clauses.push_back(instruction.exec);
            }
            else if (instruction.opcode() == ControlFlowOpcode::kAlloc && instruction.alloc.alloc_type() == AllocType::kMemory)
                return true;
    }
    for (const auto& clause : clauses)
        for (std::uint32_t i = 0; i < clause.count(); ++i)
        {
            const std::size_t slot = clause.address() + i;
            if ((slot + 1) * 3 > ucode.size())
                return true;
            if ((clause.sequence() >> (i * 2)) & 1)
                continue;
            AluInstruction alu;
            std::memcpy(&alu, ucode.data() + slot * 3, sizeof(alu));
            if (alu.is_export() && alu.vector_dest() >= std::uint32_t(ExportRegister::kExportAddress) &&
                alu.vector_dest() <= std::uint32_t(ExportRegister::kExportData4))
                return true;
        }
    return false;
}

// UI mask uses half-pixel/2X-expanded bounds from captured clip XY and
// viewport.
void markUi(const std::filesystem::path& capture, const Draw& draw, std::vector<std::uint8_t>& mask)
{
    // C1 VS translations 5F6EB3BC, 1EE55F3A and 2BA2325A compute clip
    // XY=(v.c0,v.c1) at position offset 0; PA_CL_VTE_CNTL 0x43F applies
    // viewport.
    const auto state     = preState(capture, draw);
    const auto registers = GuestDrawRegisters(state);
    UNSCOPED_INFO(std::format("UI draw {} scales XY by the full-frame viewport", draw.sequence));
    REQUIRE((registers[0x2206] == 0x43F && registers[0x2080] == 0 && registers[0x2081] == 0 && registers[0x2082] == 0x02D00500));
    const auto vertices    = payload(draw, "vertices");
    const auto declaration = payload(draw, "declaration");
    const bool label       = draw.caller == kLabelCaller;
    const auto stride      = label ? draw.arguments[4] : draw.stackWords[0];
    const auto base        = label ? draw.arguments[3] : draw.arguments[7];
    const auto start       = draw.addresses.at("vertices");
    UNSCOPED_INFO(std::format("UI draw {} has a float4 or XY16 position at offset 0", draw.sequence));
    REQUIRE((declaration.size() >= 64 && word(declaration, 52) == 0 && declaration[52 + 9] == 0 &&
             word(declaration, 56) == (label ? 0x001A23A6U : 0x002C2359U) && (label ? stride == 28 : stride == 4 || stride == 8 || stride == 12)));
    std::vector<std::uint32_t> order;
    if (label)
        for (std::uint32_t i = 0; i < draw.arguments[2]; ++i)
            order.push_back(i);
    else
    {
        UNSCOPED_INFO(std::format("UI draw {} has the GFx 16-bit index form", draw.sequence));
        REQUIRE(draw.arguments[6] == 1);
        const auto indices = payload(draw, "indices");
        UNSCOPED_INFO(std::format("UI draw {} index payload", draw.sequence));
        REQUIRE(indices.size() == std::size_t(draw.arguments[4]) * 2);
        for (std::size_t i = 0; i < indices.size(); i += 2)
            order.push_back((std::uint32_t(indices[i]) << 8) | indices[i + 1]);
    }
    UNSCOPED_INFO(std::format("UI draw {} is a triangle list", draw.sequence));
    REQUIRE((draw.arguments[1] == 4 && !order.empty() && order.size() % 3 == 0));
    const auto constant = [&](unsigned index, unsigned component)
    {
        return double(std::bit_cast<float>(registers[0x4000 + index * 4 + component]));
    };
    const auto real = [&](std::uint32_t reg)
    {
        return double(std::bit_cast<float>(registers[reg]));
    };
    for (std::size_t t = 0; t < order.size(); t += 3)
    {
        double left = 1e9, top = 1e9, right = -1e9, bottom = -1e9;
        for (std::size_t k = 0; k < 3; ++k)
        {
            const auto offset = std::uint64_t(base) + std::uint64_t(order[t + k]) * stride - start;
            UNSCOPED_INFO(std::format("UI draw {} vertex {} is captured", draw.sequence, order[t + k]));
            REQUIRE(offset + (label ? 16 : 4) <= vertices.size());
            std::array<double, 4> v{ 0, 0, 0, 1 };
            if (label)
                for (unsigned c = 0; c < 4; ++c)
                    v[c] = std::bit_cast<float>(word(vertices, offset + c * 4));
            else
                for (unsigned c = 0; c < 2; ++c)
                    v[c] = std::int16_t((vertices[offset + c * 2] << 8) | vertices[offset + c * 2 + 1]);
            UNSCOPED_INFO(std::format("UI draw {} has W 1", draw.sequence));
            REQUIRE(v[3] == 1.0);
            double x = 0, y = 0;
            for (unsigned c = 0; c < 4; ++c)
            {
                x += v[c] * constant(0, c);
                y += v[c] * constant(1, c);
            }
            x      = x * real(0x210F) + real(0x2110);
            y      = y * real(0x2111) + real(0x2112);
            left   = std::min(left, x);
            right  = std::max(right, x);
            top    = std::min(top, y);
            bottom = std::max(bottom, y);
        }
        const auto x0 = std::clamp(int(std::floor(left)) - 1, 0, 1280), x1 = std::clamp(int(std::ceil(right)) + 1, 0, 1280);
        const auto y0 = std::clamp(int(std::floor(top)) - 1, 0, 720), y1 = std::clamp(int(std::ceil(bottom)) + 1, 0, 720);
        for (int y = y0; y < y1; ++y)
            std::fill(mask.begin() + y * 1280 + x0, mask.begin() + y * 1280 + x1, std::uint8_t(1));
    }
}

struct Error
{
    unsigned    maximum = 0;
    std::size_t over = 0, pixels = 0, worst = 0;
};

// The largest RGB10 channel error over the pixels whose mask is `selected`.
Error rgb10Error(const std::vector<std::uint8_t>& output, const std::vector<std::uint8_t>& reference, const std::vector<std::uint8_t>& mask, std::uint8_t selected, unsigned bound)
{
    Error result;
    for (std::size_t i = 0; i < mask.size(); ++i)
    {
        if (mask[i] != selected)
            continue;
        ++result.pixels;
        const auto a = little(output, i * 4), b = little(reference, i * 4);
        unsigned   worst = 0;
        for (unsigned c = 0; c < 3; ++c)
            worst = std::max(worst, unsigned(std::abs(int((a >> (c * 10)) & 1023) - int((b >> (c * 10)) & 1023))));
        result.over += worst > bound;
        if (worst > result.maximum)
        {
            result.maximum = worst;
            result.worst   = i;
        }
    }
    return result;
}

// The scene draws over a full-frame color, depth 1.0 and stencil 0 clear,
// resolved at guest samples 0 and 1 (host 0 and 3) through the gamma table.
std::vector<std::uint8_t> render(const std::vector<NativeDrawReplayRecipe>& draws, const std::array<float, 4>& clearColor, const std::array<std::uint32_t, 256>& gamma)
{
    NativeRenderFrame frame;
    frame.textures.push_back({ 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 });
    NativeRenderFramePass pass;
    pass.target = NativeRenderFrameDrawTarget(draws.front());
    NativeRenderFramePass::Clear clear;
    clear.rectangle = { 0, 0, 1280, 720 };
    clear.color = clear.depth = clear.stencil = true;
    clear.colorValue                          = clearColor;
    pass.clears.push_back(clear);
    pass.draws = draws;
    NativeRenderFrameResolve resolve;
    resolve.textureId = 1;
    resolve.width     = 1280;
    resolve.height    = 720;
    resolve.samples   = { 0, 3 };
    pass.resolves.push_back(resolve);
    frame.passes.push_back(std::move(pass));
    frame.outputTextureId      = 1;
    frame.gammaTable           = gamma;
    frame.returnResolvedOutput = true;
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(std::string("native stream: ") + (error));
    REQUIRE(renderer.StartDrawStream(error));
    const auto result = renderer.SubmitRenderFrame(std::move(frame));
    renderer.Shutdown();
    UNSCOPED_INFO(std::string("native render: ") + (result.error));
    REQUIRE((result.success && result.resolvedOutput.size() == 1280 * 720 * 4));
    return result.resolvedOutput;
}

int c1(const std::filesystem::path& capture, const std::filesystem::path& catalog)
{
    std::string                   error;
    NativeGameplayShaderInterface shader;
    UNSCOPED_INFO(std::string("menu catalog: ") + (error));
    REQUIRE(LoadSceneMenuDrawShader(catalog, shader, error));
    // The backdrop's guest words sit beside its VS DXIL, in its pair directory
    // when the catalog holds band pairs.
    std::filesystem::path pairDirectory;
    const auto            entries = toml::parse_file((catalog / "shaders.toml").string());
    for (const auto& node : *entries["shaders"].as_array())
        if (toml::node_view<const toml::node>(node)["vertex_hash"].value_or(std::string()) == "C3BAB94E67553E12")
            pairDirectory = (catalog / toml::node_view<const toml::node>(node)["vertex_file"].value_or(std::string())).parent_path();
    const auto catalogVs = read(pairDirectory / "vs.ucode.be.bin");
    UNSCOPED_INFO("the catalog VS words are the pinned pair's");
    REQUIRE((XXH3_64bits(catalogVs.data(), catalogVs.size()) == kSceneMenuVertexShaderHash && IndexedBufferMicrocodeWordDigest(catalogVs) == kSceneMenuVertexWordDigest));

    // C1 census has two scene draws before UI; frame 1203 has 15 UI roots,
    // later frames 24.
    const auto                                          c = loadCapture(capture);
    std::vector<Draw>                                   scene, ui;
    std::map<std::uint64_t, std::array<std::size_t, 3>> counts; // scene, UI roots, nested, per frame
    for (const auto& draw : c.draws)
    {
        UNSCOPED_INFO(std::format("draw {} is in frames 1203..1206", draw.sequence));
        REQUIRE((draw.frame >= kBegin && draw.frame < c.end));
        auto& count = counts[draw.frame];
        if (draw.caller == kSceneMeshDrawCaller)
        {
            UNSCOPED_INFO("scene draws enter IndexedBuffer");
            REQUIRE(draw.entry == kSceneEntry);
            UNSCOPED_INFO(std::format("scene draw {} precedes its frame's UI draws", draw.sequence));
            REQUIRE((count[1] == 0 && count[2] == 0));
            ++count[0];
            if (draw.frame == kFrame)
                scene.push_back(draw);
        }
        else if ((draw.caller == kGfxCaller && draw.entry == kGfxEntry) || (draw.caller == kLabelCaller && draw.entry == kLabelEntry))
        {
            ++count[1];
            if (draw.frame == kFrame)
                ui.push_back(draw);
        }
        else
        {
            UNSCOPED_INFO(std::format("draw {} from {:#x} is a nested lowerer record", draw.sequence, draw.caller));
            REQUIRE((draw.caller == kGfxInner || draw.caller == kLabelInner));
            ++count[2];
        }
    }
    const std::map<std::uint64_t, std::array<std::size_t, 3>> expectedCounts{ { 1203, { 2, 15, 15 } }, { 1204, { 2, 24, 24 } }, { 1205, { 2, 24, 24 } }, { 1206, { 2, 24, 24 } } };
    UNSCOPED_INFO("each frame is two scene draws, then its UI roots and their nested records: 15 and 15 in frame 1203, 24 and 24 in frames 1204..1206");
    REQUIRE((counts == expectedCounts && scene[0].sequence < scene[1].sequence && scene[1].sequence < ui.front().sequence));
    std::cout << "C1 frame 1203: scene draws " << scene[0].sequence << " and " << scene[1].sequence << " from 0x8250C768, then " << ui.size() << " UI draws\n";

    // Reader and recipe on both scene draws.
    struct Expected
    {
        std::uint32_t                 indexCount, vertexBytes, width, height, alphaReference;
        NativeDrawReplayTextureFormat format;
    };

    const std::array<Expected, 2>       expected{ {
        { 12, 8 * 32, 1024, 256, 0, NativeDrawReplayTextureFormat::Bc1 },
        { 1536, 289 * 32, 2048, 512, 0x3DE8E8EA, NativeDrawReplayTextureFormat::Bc2 },
    } };
    std::vector<NativeDrawReplayRecipe> draws;
    for (std::size_t i = 0; i < scene.size(); ++i)
    {
        const auto&             draw = scene[i];
        const auto&             want = expected[i];
        IndexedBufferDrawInputs inputs;
        const auto              recipe = menuRecipe(capture, draw, shader, inputs);
        const auto              name   = std::format("draw {}", draw.sequence);
        const auto              decl   = payload(draw, "declaration");
        UNSCOPED_INFO(name + " reader inputs equal the C1 payloads");
        REQUIRE((inputs.vertexCode == payload(draw, "vs_code") && inputs.pixelCode == payload(draw, "ps_code") && inputs.indexBytes == payload(draw, "indices") &&
                 inputs.streamBytes.size() == 1 && inputs.streamBytes[0] == payload(draw, "vertices") &&
                 inputs.fetchRecords == payload(draw, "fetch_records") && inputs.declaration == std::vector<std::uint8_t>(decl.begin() + 52, decl.end()) &&
                 inputs.textureBytes == payload(draw, "sampled_base") && inputs.textureAddress == draw.addresses.at("sampled_base")));
        const auto literals = payload(draw, "vs_literal_values:252");
        bool       literal  = inputs.vertexLiteralRanges.size() == 1 && inputs.vertexLiteralRanges[0].physical == draw.addresses.at("vs_literal_values:252") &&
                              inputs.vertexLiteralRegisters.size() == 16 && literals.size() == 64;
        for (std::uint32_t k = 0; literal && k < 16; ++k)
            literal = inputs.vertexLiteralRegisters[k] == std::make_pair(0x43F0U + k, word(literals, k * 4));
        UNSCOPED_INFO(name + " reads the one c252..c255 VS literal record");
        REQUIRE(literal);
        UNSCOPED_INFO(name + " has its index count and 32-byte vertices");
        REQUIRE((inputs.indexCount == want.indexCount && inputs.streamBytes[0].size() == want.vertexBytes && inputs.arguments.stride == 32));
        // The captured VS is already the linked, fetch-patched word set, so
        // the fold and patch leave it unchanged.
        UNSCOPED_INFO(name + " patches to the catalog's VS words");
        REQUIRE((recipe.patchedVertexCode == inputs.vertexCode && recipe.patchedVertexCode == catalogVs));
        // Original sets RB_HIZCONTROL (0x2203) bit 0 where the sky draw left
        // it clear (0x10 to 0x11); the fold keeps the entry word and no native
        // draw state reads it. Every other register and constant is Original's.
        auto effective = GuestDrawRegisters(recipe.effectiveState), original = registersOf(postState(capture, draw));
        UNSCOPED_INFO(name + " differs from Original's HiZ word at most in bit 0");
        REQUIRE((effective.size() == original.size() && (effective[0x2203] | 1) == original[0x2203]));
        effective[0x2203] = original[0x2203];
        std::string differing;
        for (std::size_t r = 0; r < effective.size(); ++r)
            if (effective[r] != original[r] && differing.size() < 300)
                differing += std::format(" {:#x}: {:#x} for {:#x}", r, effective[r], original[r]);
        UNSCOPED_INFO(name + " effective registers and constants equal Original's state:" + differing);
        REQUIRE(differing.empty());
        const auto& native = recipe.native;
        UNSCOPED_INFO(name + " binds the catalog DXIL");
        REQUIRE((std::ranges::equal(native.vertexShaderBytes(), shader.vertexShaderDxil) && std::ranges::equal(native.pixelShaderBytes(), shader.pixelShaderDxil)));
        UNSCOPED_INFO(name + " samples its BC level");
        REQUIRE((native.indexCount == want.indexCount && native.textureMask == 1 && native.textures[0].format == want.format &&
                 native.textures[0].width == want.width && native.textures[0].height == want.height));
        UNSCOPED_INFO(name + " keeps the pinned depth, cull and blend");
        REQUIRE((native.depth.enabled && native.depth.writeEnabled && native.depth.compare == 4 && native.rasterizer.cull == 2 && native.rasterizer.frontCounterClockwise &&
                 native.blend.enabled && native.blend.sourceColor == NativeDrawReplayBlendFactor::SourceAlpha &&
                 native.blend.destinationColor == NativeDrawReplayBlendFactor::InverseSourceAlpha && native.sampleMask == 9 && native.extraTargetCount == 0));
        UNSCOPED_INFO(name + " carries its RB_ALPHA_REF");
        REQUIRE((std::uint32_t(native.sharedConstants[300]) == (want.alphaReference & 0xFF) && std::uint32_t(native.sharedConstants[303]) == want.alphaReference >> 24));
        draws.push_back(native);

        // The family admits the draw; the prepass family finds it foreign.
        auto                   memory = memoryAt(capture, draw);
        NativeDrawReplayRecipe admitted;
        std::string            detail;
        auto                   family = MakeSceneMenuDrawBuilder(readerFor(memory), std::make_shared<NativeGameplayShaderInterface>(shader));
        UNSCOPED_INFO(std::string(name + " is admitted by the menu family: ") + (detail));
        REQUIRE((family(submissionOf(draw), admitted, detail) && detail.empty() && admitted.indexCount == want.indexCount));
        auto boot = MakeBootDrawBuilder(readerFor(memory), nullptr, nullptr, nullptr, std::make_shared<NativeGameplayShaderInterface>(shader));
        UNSCOPED_INFO(name + " is foreign to the boot chain's prepass form");
        REQUIRE((!boot(submissionOf(draw), admitted, detail) && detail.empty()));
        // The registry's admission binds the same catalog to this memory.
        NativeDrawFamily registered;
        std::string      summary;
        UNSCOPED_INFO(std::string(name + " registry admission: ") + (error));
        REQUIRE((SceneMenuDrawFamily().admit(catalog, readerFor(memory), registered, summary, error) && registered));
        NativeDrawReplayRecipe registeredDraw;
        detail.clear();
        UNSCOPED_INFO(name + " is Built by the registered menu family");
        REQUIRE((registered(submissionOf(draw), registeredDraw, detail) == NativeDrawFamilyResult::Built && registeredDraw.indexCount == want.indexCount));
    }
    {
        // The island draw with the sky draw's texture fetch is refused.
        IndexedBufferDrawInputs sky, island;
        menuRecipe(capture, scene[0], shader, sky);
        menuRecipe(capture, scene[1], shader, island);
        island.textureFetch = sky.textureFetch;
        SceneMenuDrawRecipe rejected;
        UNSCOPED_INFO(std::string("a foreign texture fetch is refused: ") + (error));
        REQUIRE((!BuildSceneMenuDrawRecipe(island, shader, rejected, error) && error == "scene_mesh_texture_fetch_mismatch"));
        menuRecipe(capture, scene[1], shader, island);
        store(island.state, 0x2948, 0x18000);
        UNSCOPED_INFO(std::string("culling off is refused: ") + (error));
        REQUIRE((!BuildSceneMenuDrawRecipe(island, shader, rejected, error) && error == "scene_mesh_fixed_state 0x2205: 0x18000"));
    }
    std::cout << "C1 scene draws: reader inputs equal the payloads, the VS patch is the catalog's, recipes pin depth, cull, blend and BC1/BC2 levels\n";

    const auto referenceDirectory = environment("REREVVED_MENU_SCENE_REFERENCE");
    if (referenceDirectory.empty())
    {
        std::cout << "Set REREVVED_MENU_SCENE_REFERENCE to the C1 Xenos frame reference to compare the scene\n";
        return 77;
    }

    // Frames 1202-1206 resolve two frontbuffer halves; captured frames present
    // alternating buffers.
    UNSCOPED_INFO("frames 1202..1206 each resolve two halves and frames 1203..1206 present");
    REQUIRE((c.resolves.size() == 10 && c.presents.size() == 4));
    std::map<std::uint64_t, std::uint32_t> frontbuffers;
    for (std::size_t k = 0; k < 5; ++k)
    {
        const auto  frame = kBegin - 1 + k;
        const auto &left = c.resolves[k * 2], right = c.resolves[k * 2 + 1];
        UNSCOPED_INFO(std::format("frame {} resolves both halves of its frontbuffer and a captured frame presents it", frame));
        REQUIRE((left.frame == frame && right.frame == frame && left.base == right.base && left.rectangle == std::array<std::uint32_t, 4>{ 0, 0, 640, 720 } &&
                 right.rectangle == std::array<std::uint32_t, 4>{ 640, 0, 1280, 720 } && (k == 0 || c.presents[k - 1] == std::make_pair(frame, left.base))));
        frontbuffers[frame] = left.base;
    }
    UNSCOPED_INFO("the frames alternate between two frontbuffers");
    REQUIRE((frontbuffers[1202] == frontbuffers[1204] && frontbuffers[1204] == frontbuffers[1206] && frontbuffers[1203] == frontbuffers[1205] &&
             frontbuffers[1202] != frontbuffers[1203]));

    // Xenos 1204 spans title 1202's swap to 1203's; the scrolling sky rejects
    // title 1205 by up to 201 RGB10.
    const auto directory = std::filesystem::path(referenceDirectory);
    const auto input     = directory / "input";
    const auto report    = toml::parse_file((directory / "reference.toml").string());
    const auto manifest  = toml::parse_file((input / "manifest.toml").string());
    const auto start     = std::uint64_t(report["start_frame"].value_or<std::int64_t>(0));
    UNSCOPED_INFO("a complete reference of Xenos frame 1204, requested at capture frame 1203");
    REQUIRE((report["status"].value_or(std::string()) == "complete" && start == kFrame + 1 && report["end_frame"].value_or<std::int64_t>(0) == std::int64_t(kFrame + 2) &&
             report["request_boundary"].value_or<std::int64_t>(0) == std::int64_t(c.begin) && report["request_gpu_frame"].value_or<std::int64_t>(0) + 1 == std::int64_t(c.begin)));
    UNSCOPED_INFO("a 1280x720 RGB10 output through the 256-entry gamma table");
    REQUIRE((manifest["output_format"].value_or<std::int64_t>(0) == 24 && manifest["output_width"].value_or<std::int64_t>(0) == 1280 &&
             manifest["output_height"].value_or<std::int64_t>(0) == 720 && !manifest["use_fxaa"].value_or(true) && !manifest["use_pwl_gamma"].value_or(true) &&
             manifest["is_8bpc"].value_or(false)));
    const auto file = [&](const toml::node_view<const toml::node>& entry)
    {
        return read(input / entry["file"].value_or(std::string()));
    };
    const auto* events = manifest["events"].as_array();
    UNSCOPED_INFO("frame events");
    REQUIRE((events && events->size() >= 2));
    std::size_t inert = 0, drawn = 0, resolved = 0;
    for (std::size_t i = 0; i < events->size(); ++i)
    {
        const auto event = toml::node_view<const toml::node>((*events)[i]);
        const auto kind  = event["kind"].value_or<std::int64_t>(-1);
        if (i == 0 || i + 1 == events->size())
        {
            UNSCOPED_INFO("the reference frame runs from the swap of frame 1202's frontbuffer to the swap of frame 1203's");
            REQUIRE((kind == 2 && std::uint32_t(event["frontbuffer"].value_or<std::int64_t>(0)) == frontbuffers[i == 0 ? kFrame - 1 : kFrame] &&
                     event["frontbuffer_width"].value_or<std::int64_t>(0) == 1280 && event["frontbuffer_height"].value_or<std::int64_t>(0) == 720));
            continue;
        }
        if (kind == 1)
        {
            UNSCOPED_INFO("a resolve between the swaps");
            REQUIRE(event["copy_mode"].value_or(std::string()) == "resolve");
            ++resolved;
            continue;
        }
        const auto bytes = file(event["registers"]);
        UNSCOPED_INFO("only draws and resolves between the swaps");
        REQUIRE((kind == 0 && bytes.size() == 0x5003 * 4));
        ++drawn;
        const auto ucode = [&](const char* key)
        {
            const auto                 code = file(event[key]);
            std::vector<std::uint32_t> result(code.size() / 4);
            for (std::size_t k = 0; k < result.size(); ++k)
                result[k] = little(code, k * 4);
            return result;
        };
        if (little(bytes, 0x2104 * 4) == 0 && (little(bytes, 0x2200 * 4) & 7) == 0 && !exportsMemory(ucode("vertex_ucode")) && !exportsMemory(ucode("pixel_ucode")))
            ++inert;
    }
    // Frame 1203's own work: 113 draws, of which 72 write no color, depth,
    // stencil or memory, and the two frontbuffer resolves.
    UNSCOPED_INFO(std::format("the reference frame holds 113 draws, 72 of them inert, and two resolves: {} draws, {} inert, {} resolves", drawn, inert, resolved));
    REQUIRE((drawn == 113 && inert == 72 && resolved == 2));
    const auto reference = file(manifest["reference"]);
    UNSCOPED_INFO("the RGB10 reference matches its recorded digest");
    REQUIRE((reference.size() == 1280 * 720 * 4 && rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(reference.data()), reference.size())) ==
                                                       report["reference_sha256"].value_or(std::string())));
    const auto                     gammaBytes = file(manifest["gamma"]);
    std::array<std::uint32_t, 256> gamma{};
    UNSCOPED_INFO("a 256-entry gamma table");
    REQUIRE(gammaBytes.size() == gamma.size() * 4);
    for (std::size_t i = 0; i < gamma.size(); ++i)
        gamma[i] = little(gammaBytes, i * 4);

    // The compared pixels are those no UI draw can touch.
    std::vector<std::uint8_t> mask(1280 * 720, 0);
    for (const auto& draw : ui)
        markUi(capture, draw, mask);
    const auto masked = std::size_t(std::ranges::count(mask, std::uint8_t(1)));
    std::cout << "Xenos frame " << start << " shows frame 1203 (" << drawn << " draws, " << inert << " inert, " << resolved << " resolves between its swaps); UI bounds cover "
              << masked << " of 921600 pixels\n";

    if (environment("REREVVED_TEST_NATIVE_D3D12") != "1")
    {
        std::cout << "REREVVED_TEST_NATIVE_D3D12 is not 1; the scene was not compared with the Xenos frame\n";
        return 77;
    }
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
    {
        std::cout << "No D3D12 device; the scene was not compared with the Xenos frame\n";
        return 77;
    }
    // Two clear colors find any compared pixel the scene leaves uncovered.
    const auto  black = render(draws, { 0, 0, 0, 1 }, gamma), white = render(draws, { 1, 1, 1, 1 }, gamma);
    std::size_t uncovered = 0;
    for (std::size_t i = 0; i < mask.size(); ++i)
        uncovered += !mask[i] && little(black, i * 4) != little(white, i * 4);

    // Island actual/expected: (350, 531) R675/684; (453, 664) B492/484;
    // (449, 688) R790/799. Bilinear precision is presumed.
    // Scene bound: max g[i+2]-g[i] (two steps); movie 6 is one max step.
    unsigned bound = 0;
    for (unsigned c = 0; c < 3; ++c)
        for (std::size_t i = 0; i + 2 < gamma.size(); ++i)
            bound = std::max(bound, ((gamma[i + 2] >> (c * 10)) & 1023) - ((gamma[i] >> (c * 10)) & 1023));
    const auto scenePixels = rgb10Error(black, reference, mask, 0, 6), uiPixels = rgb10Error(black, reference, mask, 1, 6);
    std::cout << std::format("Native scene against Xenos frame {}: max RGB10 error {} (bound {}) over {} scene-only pixels, {} above 6 (worst at {}, {}), {} "
                             "uncovered; {} over the {} UI pixels\n",
                             start,
                             scenePixels.maximum,
                             bound,
                             scenePixels.pixels,
                             scenePixels.over,
                             scenePixels.worst % 1280,
                             scenePixels.worst / 1280,
                             uncovered,
                             uiPixels.maximum,
                             uiPixels.pixels);
    UNSCOPED_INFO("the scene draws cover every compared pixel");
    REQUIRE(uncovered == 0);
    UNSCOPED_INFO("the UI draws change the pixels the comparison excludes");
    REQUIRE(uiPixels.maximum > bound);
    UNSCOPED_INFO("the native menu scene exceeds the RGB10 error bound against Xenos");
    REQUIRE(scenePixels.maximum <= bound);
    // Until the cause of the three outliers is known, the wider bound covers only them.
    std::vector<std::size_t> outliers;
    for (std::size_t i = 0; i < mask.size(); ++i)
    {
        const auto a = little(black, i * 4), b = little(reference, i * 4);
        for (unsigned c = 0; !mask[i] && c < 3; ++c)
            if (std::abs(int((a >> (c * 10)) & 1023) - int((b >> (c * 10)) & 1023)) > 6)
            {
                outliers.push_back(i);
                break;
            }
    }
    UNSCOPED_INFO("exactly the three known pixels exceed the movie bound of 6");
    REQUIRE((scenePixels.over == 3 && outliers == std::vector<std::size_t>{ 531 * 1280 + 350, 664 * 1280 + 453, 688 * 1280 + 449 }));

    // The bound detects a missing island draw.
    const auto skyOnly = rgb10Error(render({ draws[0] }, { 0, 0, 0, 1 }, gamma), reference, mask, 0, bound);
    std::cout << "Without the island draw: max RGB10 error " << skyOnly.maximum << ", " << skyOnly.over << " pixels above the bound\n";
    UNSCOPED_INFO("the error bound detects a missing island draw");
    REQUIRE(skyOnly.maximum > bound);
    return 0;
}

} // namespace title

} // namespace

TEST_CASE("scene_mesh_draw_adapter", "[ctest][scene-mesh-draw-adapter][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    unit();
    menu::unit();
}

TEST_CASE("scene_mesh_draw_adapter_capture", "[.][renderer-research][scene-mesh-draw-adapter][native_gpu][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    // Only unset captures skip; a set but unusable path fails. With both
    // set, the result is a skip when either check skipped its D3D12 part.
    const auto capture = environment("REREVVED_SCENE_MESH_CAPTURE"), menuCapture = environment("REREVVED_MENU_SCENE_CAPTURE");
    if (capture.empty() && menuCapture.empty())
    {
        SKIP("Set REREVVED_SCENE_MESH_CAPTURE to the C10 scene capture directory, or REREVVED_MENU_SCENE_CAPTURE to the C1 title capture, to run these checks");
    }
    int result = 0;
    if (!capture.empty())
        result = c10(capture);
    if (!menuCapture.empty())
    {
        const auto catalog = environment("REREVVED_MENU_SCENE_SHADERS");
        UNSCOPED_INFO("REREVVED_MENU_SCENE_CAPTURE is set; the menu checks also need REREVVED_MENU_SCENE_SHADERS");
        REQUIRE(!catalog.empty());
        result = std::max(result, title::c1(menuCapture, catalog));
    }
    if (result == 77)
    {
        SKIP("no D3D12 device for the scene capture");
    }
}
