#include "gpu/cached_sha256.h"
#include "gpu/d3d12/native_renderer_d3d12.h"
#include "gpu/d3d12/native_texture_upload.h"
#include "gpu/guest_d3d/native_draw_families.h"
#include "gpu/guest_d3d/native_draw_shader_words.h"
#include "gpu/guest_d3d/native_frame_assembler.h"
#include "gpu/guest_d3d/scene_draw_adapter.h"
#include "gpu/guest_d3d/scene_draw_coverage.h"
#if REREVVED_DEVELOPER
#include "gpu/guest_d3d/vs_code_probe.h"
#include "gpu/guest_d3d/vs_code_watch.h"
#endif
#include "logging_scope.hpp"
#include "manual_cases.hpp"
#include "resource_mirror_fixture.h"
#include "stream_queue_fixture.h"

#include <rex/crypto/sha256.h>
#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <toml++/toml.hpp>

#if REREVVED_DEVELOPER
REXCVAR_DECLARE(bool, native_vs_code_watch);
#endif

using namespace rerevved::gpu;
using namespace rerevved::gpu::guest_d3d;

namespace
{

namespace xenos = rex::graphics::xenos;

// The detail is read after the condition, so it names the call's own error.
// A refusal by name: the assembler appends the operation's form after " (".
bool refusedAs(const std::string& error, const std::string& expected)
{
    return error == expected || error.starts_with(expected + " (");
}

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

void store(GuestBytes& bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned c = 0; c < 4; ++c)
        bytes.MutableData()[offset + c] = static_cast<std::uint8_t>(value >> (24 - c * 8));
}

void put(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    if (bytes.size() < offset + 4)
        bytes.resize(offset + 4);
    store(bytes, offset, value);
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

std::string sha256(std::span<const std::uint8_t> bytes)
{
    return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

// A recipe's replayed content as bytes, field by field: owned and shared
// payloads by their bytes, so two recipes that replay alike serialize alike.
std::vector<std::uint8_t> recipeBytes(const NativeDrawReplayRecipe& draw)
{
    std::vector<std::uint8_t> out;
    const auto                scalar = [&](const auto& value)
    {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(value));
    };
    const auto payload = [&](std::span<const std::uint8_t> bytes)
    {
        scalar(std::uint64_t(bytes.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
    };
    const auto floats = [&](std::initializer_list<float> values)
    {
        for (const auto value : values)
            scalar(std::bit_cast<std::uint32_t>(value));
    };
    const auto blend = [&](const NativeDrawReplayBlendState& b)
    {
        for (const auto value : { b.enabled, b.alphaToCoverage, b.logicOpEnabled })
            scalar(value);
        for (const auto value : { std::uint32_t(b.sourceColor), std::uint32_t(b.destinationColor), std::uint32_t(b.colorOp), std::uint32_t(b.sourceAlpha), std::uint32_t(b.destinationAlpha), std::uint32_t(b.alphaOp), std::uint32_t(b.writeMask) })
            scalar(value);
    };
    const auto sampler = [&](const NativeDrawReplaySampler& s)
    {
        for (const auto value : { s.minLinear, s.magLinear, s.mipLinear })
            scalar(value);
        scalar(s.anisotropy);
        scalar(s.address);
        floats({ s.border[0], s.border[1], s.border[2], s.border[3], s.minLod, s.maxLod, s.mipBias });
    };
    // The schema selects the sample coverage rule (native_sample_coverage.h).
    scalar(draw.schemaVersion);
    payload(draw.vertexShaderBytes());
    payload(draw.pixelShaderBytes());
    payload(draw.vertexBytes());
    payload({ reinterpret_cast<const std::uint8_t*>(draw.indexSpan().data()), draw.indexSpan().size() * 4 });
    for (const auto* bytes : { &draw.vertexConstants, &draw.pixelConstants, &draw.sharedConstants, &draw.initialSample0, &draw.initialSample1, &draw.depth.initialSamples })
        payload(*bytes);
    scalar(draw.clearColor);
    const auto& v = draw.viewport;
    floats({ v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth });
    for (const auto value : { draw.scissor.left, draw.scissor.top, draw.scissor.right, draw.scissor.bottom })
        scalar(value);
    blend(draw.blend);
    for (const auto& texture : draw.textures)
    {
        payload(texture.payloadBytes());
        for (const auto value : { texture.width, texture.height, std::uint32_t(texture.format), texture.mipLevels, std::uint32_t(texture.dimension) })
            scalar(value);
        scalar(texture.swizzle);
        scalar(texture.frameTextureId);
        scalar(bool(texture.textureVersion));
        if (texture.textureVersion)
        {
            payload(texture.textureVersion->Payload() ? std::span<const std::uint8_t>(*texture.textureVersion->Payload()) : std::span<const std::uint8_t>());
            const auto& layout = texture.textureVersion->Layout();
            for (const auto value : { layout.width, layout.height, std::uint32_t(layout.format), layout.mipLevels, std::uint32_t(layout.dimension) })
                scalar(value);
        }
    }
    sampler(draw.sampler);
    scalar(std::uint64_t(draw.samplers.size()));
    for (const auto& s : draw.samplers)
        sampler(s);
    for (const auto value : { draw.depth.enabled, draw.depth.writeEnabled })
        scalar(value);
    scalar(draw.depth.compare);
    floats({ draw.depth.initialClear });
    scalar(draw.rasterizer.cull);
    for (const auto value : { draw.rasterizer.frontCounterClockwise, draw.rasterizer.depthClip })
        scalar(value);
    scalar(std::uint32_t(draw.targetFormat));
    scalar(draw.extraTargetCount);
    for (const auto& extra : draw.extraTargets)
    {
        scalar(std::uint32_t(extra.format));
        blend(extra.blend);
    }
    for (const auto value : { draw.width, draw.height, draw.sampleCount, draw.sampleMask, draw.vertexStrideBytes, draw.vertexAttributeCount, draw.indexCount, draw.textureMask })
        scalar(value);
    scalar(draw.vertexShaderHash);
    scalar(draw.pixelShaderHash);
    const auto& st = draw.stencil;
    for (const auto value : { std::uint32_t(st.enabled), std::uint32_t(st.reference), std::uint32_t(st.readMask), std::uint32_t(st.writeMask) })
        scalar(value);
    for (const auto* face : { &st.front, &st.back })
        for (const auto value : { face->compare, face->fail, face->depthFail, face->pass })
            scalar(value);
    scalar(draw.depthOnly);
    return out;
}

// Mapped regions return bytes; pending resolve regions shadow memory and block
// overlapping views.
struct FakeMemory
{
    struct Region
    {
        std::uint64_t             address  = 0;
        bool                      physical = false;
        std::vector<std::uint8_t> bytes;

        bool Overlaps(std::uint64_t begin, std::uint64_t size, bool inPhysical) const
        {
            return physical == inPhysical && begin < address + bytes.size() && address < begin + size;
        }
    };

    std::vector<Region> regions, pending;
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
            {
                std::vector<std::uint8_t> bytes(region.bytes.begin() + (address - region.address), region.bytes.begin() + (address - region.address + size));
                for (const auto& image : pending)
                    for (std::uint64_t i = 0; i < size; ++i)
                        if (image.Overlaps(address + i, 1, physical))
                            bytes[i] = image.bytes[address + i - image.address];
                return bytes;
            }
        return {};
    }

    // The same range in place, without counting a read.
    std::span<const std::uint8_t> View(std::uint64_t address, std::uint64_t size, bool physical) const
    {
        if (std::ranges::any_of(pending, [&](const Region& image)
                                {
                                    return image.Overlaps(address, size, physical);
                                }))
            return {};
        for (const auto& region : regions)
            if (region.physical == physical && address >= region.address && address - region.address + size <= region.bytes.size())
                return std::span(region.bytes).subspan(address - region.address, size);
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

// readerFor with `cache` over the memory's views.
ReadMemory cachedReaderFor(FakeMemory& memory, const std::shared_ptr<GuestShaderReadCache>& cache)
{
    auto read = readerFor(memory);
    read.view = [&memory](std::uint64_t address, std::uint64_t size, bool physical)
    {
        return memory.View(address, size, physical);
    };
    read.shaderCache = cache;
    return read;
}

// Takes every span and completes nothing.
struct FakeCompletion final : INativeCompletionSource
{
    std::vector<NativeCompletionSpan> spans;
    // Resolve images a read may peek, by op mark.
    std::map<std::uint64_t, completion::GuestImageTake> peeks;

    completion::GuestImageTake PeekGuestImage(std::uint64_t mark, std::chrono::milliseconds) override
    {
        const auto found = peeks.find(mark);
        return found == peeks.end() ? completion::GuestImageTake{ std::nullopt, "fixture_peek_missing", false } : found->second;
    }

    std::uint64_t Submit(NativeCompletionSpan span, std::string&) override
    {
        spans.push_back(std::move(span));
        return spans.size();
    }

    std::uint64_t Completed() const override
    {
        return 0;
    }

    bool WaitFor(std::uint64_t, std::chrono::milliseconds) override
    {
        return false;
    }
};

// The 0x3500-byte shadow offset of a hardware register GuestDrawRegisters reads.
std::size_t shadowOf(std::uint32_t reg)
{
    for (const auto [first, shadow, count] : { std::array<std::uint32_t, 3>{ 0x2000, 0x2880, 16 }, { 0x2080, 0x28C0, 3 }, { 0x2100, 0x28CC, 21 }, { 0x2180, 0x2920, 5 }, { 0x2200, 0x2934, 12 }, { 0x2300, 0x29B8, 38 } })
        if (reg >= first && reg < first + count)
            return shadow + (reg - first) * 4;
    throw std::runtime_error(std::format("no shadow word for {:#x}", reg));
}

std::array<std::uint32_t, 6> fetchWords(const xenos::xe_gpu_texture_fetch_t& fetch)
{
    std::array<std::uint32_t, 6> words{};
    static_assert(sizeof(fetch) == sizeof(words));
    std::memcpy(words.data(), &fetch, sizeof(words));
    return words;
}

// A linear, packed-mip DXT1 2D fetch with linear filters and repeat addressing.
xenos::xe_gpu_texture_fetch_t flatFetch(std::uint32_t base, std::uint32_t mips)
{
    xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.type                = xenos::FetchConstantType::kTexture;
    fetch.clamp_x             = xenos::ClampMode::kRepeat;
    fetch.clamp_y             = xenos::ClampMode::kRepeat;
    fetch.clamp_z             = xenos::ClampMode::kRepeat;
    fetch.pitch               = 128 / 32;
    fetch.format              = xenos::TextureFormat::k_DXT1;
    fetch.endianness          = xenos::Endian::k8in16;
    fetch.base_address        = base >> 12;
    fetch.size_2d.width       = 127;
    fetch.size_2d.height      = 127;
    fetch.swizzle             = 0x688;
    fetch.mag_filter          = xenos::TextureFilter::kLinear;
    fetch.min_filter          = xenos::TextureFilter::kLinear;
    fetch.mip_filter          = xenos::TextureFilter::kLinear;
    fetch.mip_max_level       = 7;
    fetch.tri_clamp           = 3;
    fetch.dimension           = xenos::DataDimension::k2DOrStacked;
    fetch.packed_mips         = 1;
    fetch.mip_address         = mips >> 12;
    fetch.size_2d.stack_depth = 0;
    return fetch;
}

std::string dxilDigest(const std::vector<std::uint8_t>& bytes)
{
    return sha256(bytes);
}

const NativeDrawFamilyRegistration& sceneRegistration()
{
    const auto registry = NativeDrawFamilyRegistry();
    const auto at       = std::ranges::find(registry, std::string_view("native_draw_scene_shaders"), &NativeDrawFamilyRegistration::option);
    UNSCOPED_INFO("the scene family is registered");
    REQUIRE(at != registry.end());
    return *at;
}

// Synthetic scene draws over synthetic shader words.
namespace synthetic
{

constexpr std::uint32_t kG = 0x50000000, kVs = 0x40010000, kPs = 0x40020000, kDeclaration = 0x40030000, kIndexBuffer = 0x40040000;
constexpr std::uint32_t kSurface = 0x40050000, kDepthSurface = 0x40060000;
constexpr std::uint32_t kVsResource = 0x00100000, kPsResource = 0x00200000, kIndices = 0x00300000, kVertices = 0x00400000;
constexpr std::uint32_t kFlat = 0x01000000, kFlatMips = 0x01100000;
// The vertex-stage texture's own storage, apart from the pixel slot's.
constexpr std::uint32_t kVertexFlat = 0x01200000, kVertexFlatMips = 0x01300000;
constexpr std::uint32_t kAlphaReference = 0x3F48C8CA, kVertexCount = 8, kIndexCount = 12;
// Declaration types: k_32_32_32_FLOAT, k_32_32_FLOAT and k_8_8_8_8.
constexpr std::uint32_t kFloat3 = 0x001A23B9, kFloat2 = 0x002C23A5, kBytes = 0x001A2086;

struct Element
{
    std::uint32_t offset = 0, type = 0, usage = 0, index = 0, format = 0;
};

// A VS layout and the PS words it pairs with; both layouts share the PS.
struct Pair
{
    std::uint32_t        psWord = 0x5678, program = 0x20510C12, context = 4, interpolators = 0x3F003F, stride = 0;
    std::vector<Element> elements;
};

// Position, UV and color at stride 24.
Pair meshPair()
{
    Pair pair;
    pair.stride   = 24;
    pair.elements = { { 0, kFloat3, 0, 0, 57 }, { 12, kFloat2, 5, 0, 37 }, { 20, kBytes, 1, 0, 6 } };
    return pair;
}

// Position, normal and UV at stride 32: another VS for the same PS.
Pair normalPair()
{
    Pair pair;
    pair.stride   = 32;
    pair.elements = { { 0, kFloat3, 0, 0, 57 }, { 12, kFloat3, 3, 0, 57 }, { 24, kFloat2, 5, 0, 37 } };
    return pair;
}

struct Draw
{
    Pair            pair  = meshPair();
    SceneTargetForm form  = kSceneTargetForms[0];
    std::uint32_t   depth = 0x24F00736, blend = 0x00010706, raster = 0x18002, color = 0x8700000C;
    std::uint32_t   caller = kSceneMeshDrawCaller;
    // The VS ends with a 2D texture fetch of constant 16.
    bool vertexTexture = false;
};

// A control-flow pair whose exec clause runs fetch slots 1..fetches,
// followed by those slots.
std::vector<std::uint8_t> execCode(std::uint32_t fetches, std::uint32_t last = 0)
{
    std::vector<std::uint8_t> code((fetches + 1) * 12);
    std::uint32_t             sequence = 0;
    for (std::uint32_t i = 0; i < fetches; ++i)
        sequence |= 1U << (i * 2);
    put(code, 0, 1 | fetches << 12 | sequence << 16);
    put(code, 4, 0x1000);
    put(code, 8, last);
    return code;
}

// The PS: one 2D texture fetch from constant 0.
std::vector<std::uint8_t> psCode(const Pair& pair)
{
    auto code = execCode(1, pair.psWord);
    put(code, 12, 1);
    put(code, 20, 1U << 14);
    return code;
}

const std::array<std::uint32_t, 6> kTexture       = fetchWords(flatFetch(kFlat, kFlatMips));
const std::array<std::uint32_t, 6> kVertexTexture = fetchWords(flatFetch(kVertexFlat, kVertexFlatMips));

std::array<std::uint32_t, 8> tuple()
{
    return { kG, 4, 0, 0, kIndexCount, 0, 0, 0 };
}

NativeDrawSubmission submissionFor(std::uint32_t caller)
{
    NativeDrawSubmission submission;
    submission.hook      = NativeDrawHook::IndexedBuffer;
    submission.caller    = caller;
    submission.stack     = 0x7018F930;
    submission.arguments = tuple();
    submission.stateRead = true;
    return submission;
}

FakeMemory memoryFor(const Draw& draw = {})
{
    const auto& pair = draw.pair;
    FakeMemory  memory;
    auto        vsCode = execCode(static_cast<std::uint32_t>(pair.elements.size() + (draw.vertexTexture ? 1 : 0)));
    const auto  ps     = psCode(pair);
    if (draw.vertexTexture)
    {
        put(vsCode, std::uint32_t(pair.elements.size() + 1) * 12, 1U | 16U << 20);
        put(vsCode, std::uint32_t(pair.elements.size() + 1) * 12 + 8, 1U << 14);
    }
    memory.Add(kVsResource, true, vsCode);
    memory.Add(kPsResource, true, ps);

    // VS object: header to 912, metadata at +0x100 + 872, no literal table.
    std::vector<std::uint8_t> vsHeader(912);
    put(vsHeader, 32, kVsResource);
    put(vsHeader, 896, 0x100);
    memory.Add(kVs, false, vsHeader);
    std::vector<std::uint8_t> vsMetadata(36 + pair.elements.size() * 4);
    put(vsMetadata, 4, static_cast<std::uint32_t>(vsCode.size()));
    put(vsMetadata, 8, pair.program);
    put(vsMetadata, 12, pair.context);
    put(vsMetadata, 28, static_cast<std::uint32_t>(pair.elements.size()));
    for (std::uint32_t i = 0; i < pair.elements.size(); ++i)
        put(vsMetadata, 36 + i * 4, (pair.elements[i].index << 16) | (pair.elements[i].usage << 12) | (i + 1));
    memory.Add(kVs + 0x100 + 872, false, vsMetadata);

    // PS object: header to 68, metadata at +0x100 + 40, no literal table.
    std::vector<std::uint8_t> psHeader(68), psMetadata(32);
    put(psHeader, 24, kPsResource);
    put(psHeader, 64, 0x100);
    put(psMetadata, 4, static_cast<std::uint32_t>(ps.size()));
    put(psMetadata, 24, pair.interpolators);
    memory.Add(kPs, false, psHeader);
    memory.Add(kPs + 0x100 + 40, false, psMetadata);

    std::vector<std::uint8_t> declaration(52 + pair.elements.size() * 12);
    put(declaration, 24, static_cast<std::uint32_t>(pair.elements.size()));
    for (std::size_t i = 0; i < pair.elements.size(); ++i)
    {
        put(declaration, 52 + i * 12, pair.elements[i].offset);
        put(declaration, 52 + i * 12 + 4, pair.elements[i].type);
        declaration[52 + i * 12 + 9]  = static_cast<std::uint8_t>(pair.elements[i].usage);
        declaration[52 + i * 12 + 10] = static_cast<std::uint8_t>(pair.elements[i].index);
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
    std::vector<std::uint8_t> vertices(kVertexCount * pair.stride);
    for (std::uint32_t v = 0; v < kVertexCount; ++v)
        for (const auto& element : pair.elements)
        {
            const auto at = v * pair.stride + element.offset;
            if (element.format == 6)
                store(vertices, at, 0xFF000000U | v);
            else
                for (std::uint32_t i = 0; i < (element.format == 57 ? 3U : 2U); ++i)
                    store(vertices, at + i * 4, std::bit_cast<std::uint32_t>(float(v + i) * 0.25F));
        }
    memory.Add(kVertices, true, vertices);

    NativeTextureMemoryRanges ranges;
    std::string               error;
    UNSCOPED_INFO(std::string("synthetic texture ranges: ") + (error));
    REQUIRE(GetNativeTextureMemoryRanges(kTexture, ranges, error));
    std::vector<std::uint8_t> base(ranges.base.size), mips(ranges.mips.size);
    for (std::size_t i = 0; i < base.size(); ++i)
        base[i] = static_cast<std::uint8_t>(i * 7 + 3);
    for (std::size_t i = 0; i < mips.size(); ++i)
        mips[i] = static_cast<std::uint8_t>(i * 5 + 3);
    memory.Add(ranges.base.address, true, base);
    memory.Add(ranges.mips.address, true, mips);

    // The device shadow: bindings, the shared and per-draw state, the target
    // form, its viewport, fetches, constants and the target words the
    // assembler decodes.
    std::vector<std::uint8_t> state(0x3500);
    put(state, 0x3194, kPs);
    put(state, 0x3198, kVs);
    put(state, 0x2E2C, kDeclaration);
    put(state, 0x3094, kIndexBuffer);
    put(state, 0x3098, kSurface);
    put(state, 0x30A8, kDepthSurface);
    put(state, 0x31A4, 0xFFFFFFFF);
    state[0x30F0] = static_cast<std::uint8_t>(pair.stride / 4);
    put(state, 0x778, kVertices | 3);
    put(state, 0x77C, (std::uint32_t(vertices.size()) / 4) << 2 | 2);
    for (std::size_t i = 0; i < 6; ++i)
        put(state, 0x480 + i * 4, kTexture[i]);
    for (const auto [reg, value] : kSceneFixedState)
        put(state, shadowOf(reg), value);
    const auto& form = draw.form;
    put(state, shadowOf(0x2000), form.surfaceInfo);
    put(state, shadowOf(0x2001), form.colorInfo);
    put(state, shadowOf(0x2002), form.depthInfo);
    put(state, shadowOf(0x2082), form.scissor);
    const float width = float(form.scissor & 0x3FFF), height = float((form.scissor >> 16) & 0x3FFF);
    put(state, shadowOf(0x210F), std::bit_cast<std::uint32_t>(width / 2));
    put(state, shadowOf(0x2110), std::bit_cast<std::uint32_t>(width / 2));
    put(state, shadowOf(0x2111), std::bit_cast<std::uint32_t>(-height / 2));
    put(state, shadowOf(0x2112), std::bit_cast<std::uint32_t>(height / 2));
    put(state, shadowOf(0x2200), draw.depth);
    put(state, shadowOf(0x2201), draw.blend);
    put(state, shadowOf(0x2202), draw.color);
    put(state, shadowOf(0x2205), draw.raster);
    put(state, shadowOf(0x210E), kAlphaReference);
    for (std::uint32_t i = 0; i < 1024; ++i)
        put(state, 0x780 + i * 4, 0x3E000000U + i);
    for (std::uint32_t i = 0; i < 896; ++i)
        put(state, 0x1780 + i * 4, 0x3D000000U + i);
    NativeDrawReplayViewport viewport;
    NativeDrawReplayScissor  scissor;
    if (!DecodeNativeGameplayViewportAndScissor(GuestDrawRegisters(state), viewport, scissor, error))
        throw std::runtime_error("synthetic viewport: " + error);
    for (unsigned i = 0; const float value : { viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth })
        put(state, 0x3168 + i++ * 4, std::bit_cast<std::uint32_t>(value));
    memory.Add(kG, false, state);

    // RT0 and depth surface objects: +0x18 pitch and MSAA, +0x1C info,
    // +0x24 the logical extent.
    for (const auto [surface, info] : { std::pair{ kSurface, form.colorInfo }, std::pair{ kDepthSurface, form.depthInfo } })
    {
        std::vector<std::uint8_t> object(0x28);
        put(object, 0x18, form.surfaceInfo & 0x3FFFF);
        put(object, 0x1C, info);
        put(object, 0x24, ((std::uint32_t(width) - 1) << 18) | ((std::uint32_t(height) - 1) << 3));
        memory.Add(surface, false, object);
    }
    return memory;
}

IndexedBufferDrawInputs readScene(FakeMemory& memory, std::span<const IndexedBufferPixelShader> shaders, std::uint32_t caller = kSceneMeshDrawCaller)
{
    IndexedBufferDrawInputs inputs;
    std::string             error;
    UNSCOPED_INFO(std::string("synthetic scene inputs: ") + (error));
    REQUIRE((ReadIndexedBufferDrawInputs(submissionFor(caller), readerFor(memory), shaders, inputs, error, IndexedBufferForm::Scene) && inputs.inputsComplete));
    return inputs;
}

// The catalog interface of `draw`'s pair in one specialization: its PS
// digest, the VS words after the fold and the fetch patch, its layout and
// the program words.
NativeGameplayShaderInterface interfaceFor(const Draw& draw, bool alphaTest, std::uint32_t samples, bool vertexTexture = false)
{
    const auto&                   pair = draw.pair;
    NativeGameplayShaderInterface shader;
    shader.pixelGuestWordDigest                          = IndexedBufferMicrocodeWordDigest(psCode(pair));
    auto                                          memory = memoryFor(draw);
    const std::array<IndexedBufferPixelShader, 1> shaders{ { { shader.pixelGuestWordDigest, 1 } } };
    auto                                          inputs = readScene(memory, shaders);
    std::vector<std::uint8_t>                     folded = inputs.vertexCode, effective, patched;
    std::string                                   error;
    UNSCOPED_INFO(std::string("synthetic fold: ") + (error));
    REQUIRE(FoldSolidShaderMetadata(inputs, folded, effective, error));
    SolidDrawInputs linked = inputs;
    linked.vertexCode      = folded;
    std::vector<std::uint32_t> offsets;
    for (const auto& element : pair.elements)
    {
        offsets.push_back(element.offset);
        shader.vertexAttributes.push_back({ 95, pair.stride, std::int32_t(element.offset), element.format, element.format != 6, element.format == 6 });
    }
    UNSCOPED_INFO(std::string("synthetic fetch patch: ") + (error));
    REQUIRE(PatchVertexFetch(linked, offsets, patched, error));
    shader.vertexGuestWordDigest = IndexedBufferMicrocodeWordDigest(patched);
    shader.vertexShaderHash      = pair.stride;
    shader.pixelShaderHash       = pair.psWord;
    shader.vertexShaderDxil      = { 'D', 'X', 'B', 'C' };
    shader.pixelShaderDxil       = { 'D', 'X', 'B', 'P' };
    shader.vertexDxilDigest      = dxilDigest(shader.vertexShaderDxil);
    shader.pixelDxilDigest       = dxilDigest(shader.pixelShaderDxil);
    shader.textureFetchSlots     = { 0 };
    shader.textureDimensions     = { NativeDrawReplayTextureDimension::Texture2D };
    if (vertexTexture)
    {
        shader.textureFetchSlots.push_back(16);
        shader.textureDimensions.push_back(NativeDrawReplayTextureDimension::Texture2D);
    }
    shader.alphaTestEnabled    = alphaTest;
    shader.alphaTestCompare    = alphaTest ? 4 : 0;
    shader.rasterizedDepth     = true;
    shader.independentSamplers = true;
    shader.sampleCount         = samples;
    shader.sampleMask          = samples == 4 ? 9 : 1;
    shader.shaderProgram       = pair.program;
    shader.shaderContext       = pair.context;
    shader.shaderInterpolators = pair.interpolators;
    return shader;
}

std::shared_ptr<SceneDrawCatalog> catalogOf(std::vector<NativeGameplayShaderInterface> shaders)
{
    auto        catalog = std::make_shared<SceneDrawCatalog>();
    std::string error;
    UNSCOPED_INFO(std::string("synthetic scene catalog: ") + (error));
    REQUIRE(AdmitSceneDrawShaders(std::move(shaders), *catalog, error));
    return catalog;
}

// Both alpha specializations of the mesh pair at 2X and 1X, and the normal
// pair at 2X with the alpha test.
std::shared_ptr<SceneDrawCatalog> fullCatalog()
{
    Draw mesh, normal;
    normal.pair = normalPair();
    return catalogOf({ interfaceFor(mesh, true, 4), interfaceFor(mesh, false, 4), interfaceFor(mesh, true, 1), interfaceFor(mesh, false, 1), interfaceFor(normal, true, 4) });
}

void catalog()
{
    const auto full    = fullCatalog();
    const auto shaders = ScenePixelShaders(*full);
    UNSCOPED_INFO("five specializations of two VS share one reader entry for their PS");
    REQUIRE((full->entries.size() == 5 && shaders.size() == 1 && shaders[0].textureFetches == 1 && full->WithPixel(shaders[0].wordDigest).size() == 5 &&
             full->entries[0].stride == 24 && full->entries[4].stride == 32 && full->entries[4].attributeOffsets == std::vector<std::uint32_t>{ 0, 12, 24 }));
    Draw       mesh;
    const auto refusal = [](std::vector<NativeGameplayShaderInterface> shaders)
    {
        SceneDrawCatalog loaded;
        std::string      error;
        UNSCOPED_INFO("a refused catalog loads nothing");
        REQUIRE((!AdmitSceneDrawShaders(std::move(shaders), loaded, error) && loaded.entries.empty()));
        return error;
    };
    auto vertexTexture = interfaceFor(mesh, true, 4, true);
    {
        const auto admitted = catalogOf({ vertexTexture });
        UNSCOPED_INFO("a vertex-stage slot 16 after the pixel slots is recorded apart");
        REQUIRE((admitted->entries[0].vertexTexture && admitted->entries[0].pixelTextures == 1));
    }
    auto gap              = interfaceFor(mesh, true, 4);
    gap.textureFetchSlots = { 1 };
    UNSCOPED_INFO("a pixel slot past a gap is named");
    REQUIRE(refusal({ gap }) == "scene_catalog_textures");
    auto early              = vertexTexture;
    early.textureFetchSlots = { 16, 0 };
    UNSCOPED_INFO("slot 16 before a pixel slot is named");
    REQUIRE(refusal({ early }) == "scene_catalog_textures");
    auto none              = interfaceFor(mesh, false, 4);
    none.textureFetchSlots = {};
    none.textureDimensions = {};
    UNSCOPED_INFO("two entries of one PS with different pixel slots are named");
    REQUIRE(refusal({ interfaceFor(mesh, true, 4), none }) == "scene_catalog_textures");
    auto fetch94                              = interfaceFor(mesh, true, 4);
    fetch94.vertexAttributes[1].fetchConstant = 94;
    UNSCOPED_INFO("an attribute off fetch 95 is named");
    REQUIRE(refusal({ fetch94 }) == "scene_catalog_layout");
    auto float4                       = interfaceFor(mesh, true, 4);
    float4.vertexAttributes[0].format = 38;
    UNSCOPED_INFO("a float4 attribute is named");
    REQUIRE(refusal({ float4 }) == "scene_catalog_layout");
    // Unsigned bytes may be integers (blend indices); signed bytes may not.
    auto bytes = interfaceFor(mesh, true, 4);
    bytes.vertexAttributes.push_back({ 95, bytes.vertexAttributes[0].strideBytes, 0, 6, false, false });
    UNSCOPED_INFO("an unsigned integer k_8_8_8_8 attribute is admitted");
    REQUIRE(catalogOf({ bytes })->entries[0].attributeOffsets.size() == bytes.vertexAttributes.size());
    bytes.vertexAttributes.back().isSigned = true;
    UNSCOPED_INFO("a signed k_8_8_8_8 attribute is named");
    REQUIRE(refusal({ bytes }) == "scene_catalog_layout");
    UNSCOPED_INFO("an empty catalog is named");
    REQUIRE(refusal({}) == "scene_catalog_empty");
    std::cout << "Scene catalog: entries share a PS across VS and specializations; slot, layout and empty refusals are named\n";
}

void reader()
{
    const auto catalog = fullCatalog();
    const auto shaders = ScenePixelShaders(*catalog);
    auto       memory  = memoryFor();
    for (const auto caller : kSceneDrawCallers)
    {
        const auto inputs = readScene(memory, shaders, caller);
        UNSCOPED_INFO(std::format("the Scene form reads draws from {:#x} with their textures", caller));
        REQUIRE((inputs.sceneCandidate && inputs.arguments.caller == caller && inputs.textureFetches.size() == 1 && inputs.textureFetches[0] == kTexture &&
                 !inputs.textureBaseBytes[0].empty() && !inputs.textureMipBytes[0].empty() && inputs.arguments.stride == 24));
    }
    IndexedBufferDrawInputs foreign;
    std::string             error;
    memory.reads = 0;
    UNSCOPED_INFO("a caller outside the scene allowlist is foreign without reads");
    REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(0x82510BFC), readerFor(memory), shaders, foreign, error, IndexedBufferForm::Scene) && error.empty() && memory.reads == 0));
    auto indexedUp = submissionFor(kSceneMeshDrawCaller);
    indexedUp.hook = NativeDrawHook::IndexedUp;
    UNSCOPED_INFO("another hook is foreign without reads");
    REQUIRE((!ReadIndexedBufferDrawInputs(indexedUp, readerFor(memory), shaders, foreign, error, IndexedBufferForm::Scene) && error.empty() && memory.reads == 0));
    std::cout << "Scene reader: every listed caller and its textures; other callers and hooks are foreign\n";
}

void family()
{
    const auto  catalog = fullCatalog();
    std::string detail;
    {
        auto                   memory = memoryFor();
        const auto             family = MakeSceneDrawFamily(readerFor(memory), catalog);
        NativeDrawReplayRecipe draw;
        UNSCOPED_INFO(std::string("the family builds a catalog draw: ") + (detail));
        REQUIRE((family(submissionFor(kSceneMeshDrawCaller), draw, detail) == NativeDrawFamilyResult::Built && draw.width == 1280 && draw.sampleCount == 4));
        memory.reads = 0;
        UNSCOPED_INFO("another caller is not the family's");
        REQUIRE((family(submissionFor(0x82510BFC), draw, detail) == NativeDrawFamilyResult::NotMine && detail.empty() && memory.reads == 0));
    }
    {
        auto memory = memoryFor();
        put(memory.At(kG, false).bytes, 0x3194, 0);
        NativeDrawReplayRecipe draw;
        UNSCOPED_INFO(std::string("a draw without a pixel shader is named: ") + (detail));
        REQUIRE((MakeSceneDrawFamily(readerFor(memory), catalog)(submissionFor(kLeaderDrawCaller), draw, detail) == NativeDrawFamilyResult::Refused &&
                 detail == "scene_no_pixel_shader"));
    }
    {
        auto memory = memoryFor();
        put(memory.At(kG, false).bytes, 0x3198, 0);
        NativeDrawReplayRecipe draw;
        UNSCOPED_INFO(std::string("a draw without a vertex shader is named: ") + (detail));
        REQUIRE((MakeSceneDrawFamily(readerFor(memory), catalog)(submissionFor(kSceneMeshDrawCaller), draw, detail) == NativeDrawFamilyResult::Refused &&
                 detail == "scene_no_vertex_shader"));
    }
    {
        Draw other;
        other.pair.psWord             = 0x9ABC;
        auto                   memory = memoryFor(other);
        NativeDrawReplayRecipe draw;
        const auto             expected = "scene_catalog_pixel: " + IndexedBufferMicrocodeWordDigest(psCode(other.pair)).substr(0, 16);
        UNSCOPED_INFO(std::string("a pixel shader outside the catalog is named: ") + (detail));
        REQUIRE((MakeSceneDrawFamily(readerFor(memory), catalog)(submissionFor(kFogDrawCaller), draw, detail) == NativeDrawFamilyResult::Refused && detail == expected));
    }
    const auto& registration = sceneRegistration();
    UNSCOPED_INFO("the scene family is registered last");
    REQUIRE((NativeDrawFamilyRegistry().back().option == registration.option && registration.name == "scene catalog"));
    std::cout << "Scene family: built, foreign, no-PS, no-VS and unknown-PS draws\n";
}

// A particle batch's 36-byte layout: position, UV, color and a second UV.
Pair particlePair()
{
    Pair pair;
    pair.stride   = 36;
    pair.elements = { { 0, kFloat3, 0, 0, 57 }, { 12, kFloat2, 5, 0, 37 }, { 20, kBytes, 1, 0, 6 }, { 24, kFloat3, 5, 1, 57 } };
    return pair;
}

// The lowerer's reservation for a particle batch (virtual addresses).
constexpr std::uint32_t kParticleVertices = 0x70100000, kParticleIndices = 0x70100200;

// A direct 0x826A3048 draw from sub_825107A0 as it reaches the scene family:
// the shadow snapshot at entry, with G+0x30F0, fetch 95 and the index buffer
// object stale, and the commit naming the geometry the caller wrote.
struct Particle
{
    FakeMemory           memory;
    NativeDrawSubmission submission;
};

Particle particleFor(const Draw& draw)
{
    Particle particle{ memoryFor(draw), {} };
    auto&    memory = particle.memory;
    auto     state  = memory.At(kG, false).bytes;
    state[0x30F0]   = 0;
    put(state, 0x778, 0);
    put(state, 0x77C, 0);
    put(state, 0x3094, 0);
    memory.Add(kParticleVertices, false, memory.At(kVertices, true).bytes);
    std::vector<std::uint8_t> indices;
    for (const std::uint16_t index : { 0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7 })
        indices.insert(indices.end(), { std::uint8_t(index >> 8), std::uint8_t(index) });
    memory.Add(kParticleIndices, false, indices);
    auto& submission  = particle.submission;
    submission.hook   = NativeDrawHook::IndexedUpLowerer;
    submission.caller = kParticleDrawCaller;
    submission.stack  = 0x7018F370;
    // r3..r10: G, triangle list, base vertex, vertices, indices, BE16, r9, r10.
    submission.arguments    = { kG, 4, 0, kVertexCount, kIndexCount, 1, draw.pair.stride, 0x7018F3D8 };
    submission.stateRead    = true;
    submission.vertexShader = kVs;
    submission.pixelShader  = kPs;
    submission.declaration  = kDeclaration;
    submission.entryState   = std::make_shared<const std::vector<std::uint8_t>>(state);
    NativeDrawCommit commit;
    commit.read         = true;
    commit.pendingEnd   = 0x40800000;
    commit.commitEnd    = 0x40800000;
    commit.vertexDest   = kParticleVertices;
    commit.vertexOut    = kParticleVertices;
    commit.indexDest    = kParticleIndices;
    commit.indexOut     = kParticleIndices;
    commit.vertexDwords = kVertexCount * draw.pair.stride / 4;
    commit.indexDwords  = (kIndexCount + 1) / 2;
    commit.vertexShader = kVs;
    commit.pixelShader  = kPs;
    commit.declaration  = kDeclaration;
    submission.commit   = commit;
    // The live shadow no longer holds the draw's state.
    auto& live = memory.At(kG, false).bytes;
    std::fill(live.begin(), live.end(), std::uint8_t(0));
    return particle;
}

// Particle uses entry/commit geometry instead of stale fetch/index state.
void particle()
{
    Draw draw;
    draw.pair           = particlePair();
    const auto  catalog = catalogOf({ interfaceFor(draw, true, 4), interfaceFor(draw, false, 4) });
    const auto  shaders = ScenePixelShaders(*catalog);
    std::string error;
    {
        auto                    particle = particleFor(draw);
        IndexedBufferDrawInputs inputs;
        UNSCOPED_INFO(std::string("the particle inputs read: ") + (error));
        REQUIRE((ReadIndexedBufferDrawInputs(particle.submission, readerFor(particle.memory), shaders, inputs, error, IndexedBufferForm::Particle) && inputs.inputsComplete));
        const auto physical = (kParticleVertices & 0x1FFFFFFFU) + (((kParticleVertices >> 20) + 512) & 0x1000U);
        UNSCOPED_INFO("the particle form takes stream 0 and the indices from the commit and the stride from r9");
        REQUIRE((inputs.arguments.stride == 36 && inputs.arguments.caller == kParticleDrawCaller && inputs.indexFormat == 0 && inputs.indexEndian == 1 &&
                 inputs.streamFetches.size() == 1 && inputs.streamFetches[0][0] == (physical | 3U) && inputs.streamFetches[0][1] == (0x10000002U | (72U << 2)) &&
                 inputs.streamBytes[0] == particle.memory.At(kParticleVertices, false).bytes && inputs.indexBytes == particle.memory.At(kParticleIndices, false).bytes));
        IndexedBufferDrawInputs scene;
        UNSCOPED_INFO("the Scene form leaves a lowerer draw alone");
        REQUIRE((!ReadIndexedBufferDrawInputs(particle.submission, readerFor(particle.memory), shaders, scene, error, IndexedBufferForm::Scene) && error.empty()));
        NativeDrawReplayRecipe recipe;
        std::string            detail;
        UNSCOPED_INFO(std::string("the scene family builds the particle batch: ") + (detail));
        REQUIRE((MakeSceneDrawFamily(readerFor(particle.memory), catalog)(particle.submission, recipe, detail) == NativeDrawFamilyResult::Built &&
                 recipe.vertexData.size() == std::size_t(kVertexCount) * recipe.vertexStrideBytes && recipe.indexCount == kIndexCount && recipe.width == 1280));
        auto strip         = particle.submission;
        strip.arguments[1] = 6;
        UNSCOPED_INFO(std::string("a particle strip is refused: ") + (detail));
        REQUIRE((MakeSceneDrawFamily(readerFor(particle.memory), catalog)(strip, recipe, detail) == NativeDrawFamilyResult::Refused && refusedAs(detail, "scene_draw_tuple")));
        auto mesh             = particle.submission;
        mesh.caller           = kSceneMeshDrawCaller;
        particle.memory.reads = 0;
        UNSCOPED_INFO("a lowerer draw from another caller is not the family's");
        REQUIRE((MakeSceneDrawFamily(readerFor(particle.memory), catalog)(mesh, recipe, detail) == NativeDrawFamilyResult::NotMine && particle.memory.reads == 0));
    }
    const auto refused = [&](const std::function<void(NativeDrawSubmission&)>& edit, const std::string& code)
    {
        auto particle = particleFor(draw);
        edit(particle.submission);
        IndexedBufferDrawInputs inputs;
        UNSCOPED_INFO(std::string("a particle draw is refused as " + code + ": ") + (error));
        REQUIRE((!ReadIndexedBufferDrawInputs(particle.submission, readerFor(particle.memory), shaders, inputs, error, IndexedBufferForm::Particle) && refusedAs(error, code)));
    };
    refused([](NativeDrawSubmission& s)
            {
                s.entryState.reset();
            },
            "particle_state_unread");
    refused([](NativeDrawSubmission& s)
            {
                s.commit.reset();
            },
            "particle_geometry_unread");
    refused([](NativeDrawSubmission& s)
            {
                s.commit->read = false;
            },
            "particle_geometry_unread");
    refused([](NativeDrawSubmission& s)
            {
                s.commit->commitEnd += 0x40;
            },
            "particle_geometry_pending_end");
    refused([](NativeDrawSubmission& s)
            {
                s.commit->indexOut += 2;
            },
            "particle_geometry_dest");
    refused([](NativeDrawSubmission& s)
            {
                s.commit->pixelShader = 0;
            },
            "particle_state_changed");
    refused([](NativeDrawSubmission& s)
            {
                s.commit->vertexDwords -= 9;
            },
            "particle_geometry_size");
    refused([](NativeDrawSubmission& s)
            {
                s.arguments[6] = 34;
            },
            "particle_geometry_size");
    refused([](NativeDrawSubmission& s)
            {
                s.commit->indexDest = s.commit->indexOut = kParticleIndices + 0x1000;
            },
            "particle_geometry_extent");
    std::cout << "Particle reader: the entry snapshot, the commit's geometry and r9 build a scene draw; each refusal is named\n";
}

void builder()
{
    const auto  catalog = fullCatalog();
    const auto  shaders = ScenePixelShaders(*catalog);
    std::string error;

    // Each RGBA8 target form, with the alpha test on and off; packed() covers the others.
    for (const auto& form : kSceneTargetForms)
        for (const std::uint32_t color : { 0x8700000CU, 0x87000004U, 0x87000007U })
            if (SceneTargetColorFormat(form) == 0)
            {
                Draw draw;
                draw.form              = form;
                draw.color             = color;
                auto            memory = memoryFor(draw);
                SceneDrawRecipe recipe;
                UNSCOPED_INFO(std::string(std::format("form {:#x}/{:#x} with RB_COLORCONTROL {:#x} builds: ", form.surfaceInfo, form.scissor, color)) + (error));
                REQUIRE(BuildSceneDrawRecipe(readScene(memory, shaders), *catalog, recipe, error));
                const auto& native = recipe.native;
                UNSCOPED_INFO(std::format("form {:#x}/{:#x} selects its specialization and extent", form.surfaceInfo, form.scissor));
                REQUIRE((recipe.form == &form && recipe.entry->shader.alphaTestEnabled == bool(color & 8) && recipe.entry->shader.sampleCount == form.sampleCount &&
                         native.width == form.width && native.height == form.height && native.sampleCount == form.sampleCount &&
                         native.scissor.right == std::int32_t(form.scissor & 0x3FFF) && native.scissor.bottom == std::int32_t((form.scissor >> 16) & 0x3FFF) &&
                         native.indexCount == kIndexCount && native.textureMask == 1 && native.depth.enabled && native.depth.writeEnabled));
            }
    {
        Draw normal;
        normal.pair            = normalPair();
        auto            memory = memoryFor(normal);
        SceneDrawRecipe recipe;
        const bool      built = BuildSceneDrawRecipe(readScene(memory, shaders), *catalog, recipe, error);
        UNSCOPED_INFO(std::string("the second VS of the PS builds through its own entry: ") + (error));
        REQUIRE((built && recipe.entry == &catalog->entries[4]));
        const auto meshOnly = catalogOf({ interfaceFor(Draw{}, true, 4) });
        UNSCOPED_INFO(std::string("a VS without an entry for the PS is named: ") + (error));
        REQUIRE((!BuildSceneDrawRecipe(readScene(memory, shaders), *meshOnly, recipe, error) &&
                 error == "scene_catalog_vertex: " + IndexedBufferMicrocodeWordDigest(memory.Read(kVsResource, (normal.pair.elements.size() + 1) * 12, true)).substr(0, 16)));
    }
    {
        // VS fetch 16 binds only when its paired shader samples it; stale fetch
        // 16 refuses that pair.
        Draw sampled, plain;
        sampled.vertexTexture    = true;
        const auto vertexCatalog = catalogOf({ interfaceFor(sampled, true, 4, true), interfaceFor(sampled, false, 4, true), interfaceFor(plain, true, 4), interfaceFor(plain, false, 4) });
        const auto vertexShaders = ScenePixelShaders(*vertexCatalog);
        UNSCOPED_INFO("only a PS paired with a vertex fetch reads fetch 16");
        REQUIRE((vertexShaders.size() == 1 && vertexShaders[0].vertexTexture && !shaders[0].vertexTexture));
        // Fetch 16 names storage with other bytes than the pixel slot's.
        NativeTextureMemoryRanges vertexRanges;
        UNSCOPED_INFO(std::string("vertex texture ranges: ") + (error));
        REQUIRE(GetNativeTextureMemoryRanges(kVertexTexture, vertexRanges, error));
        std::vector<std::uint8_t> vertexBase(vertexRanges.base.size), vertexMips(vertexRanges.mips.size);
        for (std::size_t i = 0; i < vertexBase.size(); ++i)
            vertexBase[i] = static_cast<std::uint8_t>(i * 11 + 5);
        for (std::size_t i = 0; i < vertexMips.size(); ++i)
            vertexMips[i] = static_cast<std::uint8_t>(i * 13 + 9);
        const auto withFetch16 = [&](const Draw& draw, bool stored)
        {
            auto memory = memoryFor(draw);
            for (std::size_t i = 0; i < kVertexTexture.size(); ++i)
                put(memory.At(kG, false).bytes, 0x480 + 16 * 24 + i * 4, stored ? kVertexTexture[i] : 0);
            memory.Add(vertexRanges.base.address, true, vertexBase);
            memory.Add(vertexRanges.mips.address, true, vertexMips);
            return memory;
        };
        auto       memory = withFetch16(sampled, true);
        const auto inputs = readScene(memory, vertexShaders);
        UNSCOPED_INFO("the reader snapshots fetch 16 after the pixel slots");
        REQUIRE((inputs.vertexTextureFetch && inputs.textureFetches.size() == 2 && inputs.textureFetches[0] == kTexture && inputs.textureFetches[1] == kVertexTexture &&
                 inputs.textureBaseBytes[1] == vertexBase && inputs.textureMipBytes[1] == vertexMips && inputs.textureBaseBytes[0] != vertexBase));
        SceneDrawRecipe recipe;
        const bool      built = BuildSceneDrawRecipe(inputs, *vertexCatalog, recipe, error);
        // Each slot's payload decodes from its own fetch constant's storage.
        NativeDrawReplayTexture pixelExpected, vertexExpected;
        UNSCOPED_INFO(std::string("the vertex-stage texture binds as the last texture, apart from the pixel slot: ") + (error));
        REQUIRE((built && recipe.native.textureMask == 3 && recipe.textures.size() == 2 &&
                 DecodeNativeTextureVersioned(kTexture, inputs.textureBaseBytes[0], inputs.textureMipBytes[0], pixelExpected, error) &&
                 DecodeNativeTextureVersioned(kVertexTexture, vertexBase, vertexMips, vertexExpected, error) &&
                 !std::ranges::equal(pixelExpected.payloadBytes(), vertexExpected.payloadBytes()) &&
                 std::ranges::equal(recipe.textures[0].payloadBytes(), pixelExpected.payloadBytes()) &&
                 std::ranges::equal(recipe.textures[1].payloadBytes(), vertexExpected.payloadBytes())));
        auto       plainMemory = withFetch16(plain, true);
        const auto plainInputs = readScene(plainMemory, vertexShaders);
        UNSCOPED_INFO(std::string("a pair of the same PS without the vertex fetch ignores fetch 16: ") + (error));
        REQUIRE((plainInputs.vertexTextureFetch && BuildSceneDrawRecipe(plainInputs, *vertexCatalog, recipe, error) && recipe.textures.size() == 1 &&
                 recipe.native.textureMask == 1));
        auto       stale  = withFetch16(sampled, false);
        const auto unread = readScene(stale, vertexShaders);
        UNSCOPED_INFO(std::string("a fetch 16 naming no storage refuses only its pair: ") + (error));
        REQUIRE((unread.vertexTextureFetch && unread.textureBaseBytes[1].empty() && !BuildSceneDrawRecipe(unread, *vertexCatalog, recipe, error) &&
                 error == "scene_texture_format 0"));
    }
    std::cout << "Scene builder: every target form in both alpha specializations, a second VS for one PS, and a vertex-stage fetch\n";

    // One rule each on the 2X form's inputs.
    auto       memory  = memoryFor();
    const auto base    = readScene(memory, shaders);
    const auto refused = [&](const std::string& name, auto mutate, const std::string& expected, const SceneDrawCatalog* other = nullptr)
    {
        auto broken = base;
        mutate(broken);
        SceneDrawRecipe rejected;
        std::string     reason;
        const bool      built = BuildSceneDrawRecipe(broken, other ? *other : *catalog, rejected, reason);
        UNSCOPED_INFO(name + ": " + (built ? "accepted" : "[" + reason + "]"));
        REQUIRE((!built && reason == expected));
    };
    const auto at = [](std::uint32_t reg, std::uint32_t value)
    {
        return [=](IndexedBufferDrawInputs& in)
        {
            store(in.state, shadowOf(reg), value);
        };
    };
    refused("a depth control outside the table", at(0x2200, 0x24F00716), "scene_depth_control: 0x24f00716");
    refused("a blend outside the table", at(0x2201, 0x07010701), "scene_blend_control: 0x7010701");
    refused("front-face culling", at(0x2205, 0x18001), "scene_raster_control: 0x18001");
    refused("alpha test LessEqual", at(0x2202, 0x8700000B), "scene_alpha_func: 0x8700000b");
    refused("alpha test GreaterEqual without its entry", at(0x2202, 0x8700000E), "scene_specialization: alpha 6 samples 4 program 0x20510c12 context 0x4 interpolators 0x3f003f");
    {
        // A GreaterEqual entry beside the Greater one: each function picks its own.
        auto greaterEqual             = interfaceFor(Draw{}, true, 4);
        greaterEqual.alphaTestCompare = 6;
        const auto both               = catalogOf({ interfaceFor(Draw{}, true, 4), greaterEqual });
        for (const std::uint32_t func : { 4U, 6U })
        {
            auto in = base;
            store(in.state, shadowOf(0x2202), 0x87000008U | func);
            SceneDrawRecipe recipe;
            UNSCOPED_INFO(std::string(std::format("alpha function {} selects its entry: ", func)) + (error));
            REQUIRE((BuildSceneDrawRecipe(in, *both, recipe, error) && recipe.entry->shader.alphaTestCompare == func));
        }
    }
    refused("alpha to mask", at(0x2202, 0x8700001C), "scene_color_control: 0x8700001c");
    refused("an RB_COLORCONTROL high byte", at(0x2202, 0x8600000C), "scene_color_control: 0x8600000c");
    {
        // Only the pitch differs from a valid 1X 1024x1024 draw.
        Draw offscreen;
        offscreen.form         = kSceneTargetForms[3];
        auto            large  = memoryFor(offscreen);
        auto            inputs = readScene(large, shaders);
        SceneDrawRecipe recipe;
        UNSCOPED_INFO(std::string("the 1X 1024x1024 fixture builds: ") + (error));
        REQUIRE(BuildSceneDrawRecipe(inputs, *catalog, recipe, error));
        store(inputs.state, shadowOf(0x2000), 0x10000400);
        UNSCOPED_INFO(std::string("a pitch of the width at 1X: ") + (error));
        REQUIRE((!BuildSceneDrawRecipe(inputs, *catalog, recipe, error) && error == "scene_target_form: 0x10000400 0x0 0x4c0 0x4000400"));
    }
    refused("an RT0 k_2_10_10_10 under the leader band", [](IndexedBufferDrawInputs& in)
            {
                store(in.state, shadowOf(0x2001), 0x20000);
                store(in.state, shadowOf(0x2082), 0x015F0500);
            },
            "scene_target_form: 0xa010280 0x20000 0x530 0x15f0500");
    refused("another depth base", at(0x2002, 0x690), "scene_target_form: 0xa010280 0x0 0x690 0x2d00500");
    refused("the 640x360 scissor", at(0x2082, 0x01680280), "scene_target_form: 0xa010280 0x0 0x530 0x1680280");
    refused("a color mask", at(0x2104, 7), "scene_fixed_state 0x2104: 0x7");
    {
        // No color writes in MODECONTROL 4: depth and kill stay, color does not.
        auto masked = base;
        at(0x2104, kSceneNoColorMask)(masked);
        SceneDrawRecipe recipe;
        UNSCOPED_INFO(std::string("color mask 0 builds a depth-writing draw without color writes: ") + (error));
        REQUIRE((BuildSceneDrawRecipe(masked, *catalog, recipe, error) && recipe.native.blend.writeMask == 0 && recipe.native.depth.writeEnabled));
    }
    refused("an index offset", at(0x2102, 4), "scene_fixed_state 0x2102: 0x4");
    refused("depth-only mode", [](IndexedBufferDrawInputs& in)
            {
                // The fold forces RB_MODECONTROL's low bits to 4; bit 3 survives it.
                store(in.state, shadowOf(0x2208), 0xC);
            },
            "scene_fixed_state 0x2208: 0xc");
    refused("guest clipping off", at(0x2204, 0x90000), "scene_fixed_state 0x2204: 0x90000");
    refused("another program word", [](IndexedBufferDrawInputs& in)
            {
                store(in.vertexMetadata, 8, 0x20510C13);
            },
            "scene_specialization: alpha 4 samples 4 program 0x20510c13 context 0x4 interpolators 0x3f003f");
    const auto twoX = catalogOf({ interfaceFor(Draw{}, true, 4), interfaceFor(Draw{}, false, 4) });
    {
        Draw offscreen;
        offscreen.form        = kSceneTargetForms[2];
        auto            small = memoryFor(offscreen);
        SceneDrawRecipe recipe;
        UNSCOPED_INFO(std::string("an off-screen draw without a 1X entry is named: ") + (error));
        REQUIRE((!BuildSceneDrawRecipe(readScene(small, shaders), *twoX, recipe, error) &&
                 error == "scene_specialization: alpha 4 samples 1 program 0x20510c12 context 0x4 interpolators 0x3f003f"));
    }
    refused("a caller outside the allowlist", [](IndexedBufferDrawInputs& in)
            {
                in.arguments.caller = 0x82510D4C; // the particle overflow path's IndexedBuffer
            },
            "scene_draw_tuple");
    refused("a base vertex", [](IndexedBufferDrawInputs& in)
            {
                in.baseVertex = 4;
            },
            "scene_draw_tuple");
    refused("a triangle fan", [](IndexedBufferDrawInputs& in)
            {
                in.primitive = 5;
            },
            "scene_draw_tuple");
    refused("a predicated shader", [](IndexedBufferDrawInputs& in)
            {
                in.state[0x2ABE] |= 0x40;
            },
            "scene_conditional_or_recorded_shader");
    // Inside the Z prepass bracket a depth-tested, depth-writing draw without
    // blending or stencil builds as its color pass; anything else names why.
    const auto zpass = [](std::uint32_t depth, std::uint32_t blend, std::uint8_t variant = 0)
    {
        return [=](IndexedBufferDrawInputs& in)
        {
            in.state[0x2ABC] |= 0x40;
            in.state[0x2ABE] |= variant;
            store(in.state, shadowOf(0x2200), depth);
            store(in.state, shadowOf(0x2201), blend);
        };
    };
    for (const auto depth : { 0x24F00736U, 0x24F00776U })
    {
        auto bracketed = base;
        zpass(depth, 0x00010001)(bracketed);
        SceneDrawRecipe recipe;
        std::string     reason;
        UNSCOPED_INFO(std::format("a Z prepass draw with depth {:#x} builds: ", depth) + reason);
        REQUIRE(BuildSceneDrawRecipe(bracketed, *catalog, recipe, reason));
    }
    refused("a Z prepass LESS compare", zpass(0x24F00716, 0x00010001), "scene_conditional_or_recorded_shader: zpass depth 0x24f00716");
    refused("a Z prepass draw without depth writes", zpass(0x24F00732, 0x00010001), "scene_conditional_or_recorded_shader: zpass depth 0x24f00732");
    refused("a Z prepass draw with stencil", zpass(0x24F00737, 0x00010001), "scene_conditional_or_recorded_shader: zpass depth 0x24f00737");
    refused("a blended Z prepass draw", zpass(0x24F00736, 0x00010706), "scene_conditional_or_recorded_shader: zpass blend 0x10706");
    refused("a Z prepass GEQUAL compare", zpass(0x24F00766, 0x00010001), "scene_conditional_or_recorded_shader: zpass depth 0x24f00766");
    refused("a Z prepass depth-only variant", zpass(0x24F00736, 0x00010001, 0x40), "scene_conditional_or_recorded_shader: zpass depth-only variant");
    refused("a Z prepass VS that selects the depth-only variant", [&](IndexedBufferDrawInputs& in)
            {
                zpass(0x24F00736, 0x00010001)(in);
                in.vertexHeader[875] |= 0x20; // header word +872, big-endian
            },
            "scene_conditional_or_recorded_shader: zpass depth-only variant");
    refused("a Z prepass draw without color writes", [&](IndexedBufferDrawInputs& in)
            {
                zpass(0x24F00736, 0x00010001)(in);
                store(in.state, shadowOf(0x2104), kSceneNoColorMask);
            },
            "scene_fixed_state 0x2104: 0x0");
    refused("a second stream", [](IndexedBufferDrawInputs& in)
            {
                in.streams.push_back(1);
            },
            "scene_stream_form");
    refused("a pixel literal in the vertex file", [](IndexedBufferDrawInputs& in)
            {
                in.pixelLiteralRegisters.emplace_back(0x4000, 0);
            },
            "scene_literal_stage");
    refused("incomplete inputs", [](IndexedBufferDrawInputs& in)
            {
                in.inputsComplete = false;
            },
            "scene_inputs_incomplete");
    const auto texture = [](auto edit)
    {
        return [=](IndexedBufferDrawInputs& in)
        {
            auto fetch = flatFetch(kFlat, kFlatMips);
            edit(fetch);
            in.textureFetches[0] = fetchWords(fetch);
            for (std::size_t i = 0; i < 6; ++i)
                store(in.state, 0x480 + i * 4, in.textureFetches[0][i]);
        };
    };
    for (const auto format : { xenos::TextureFormat::k_16_16, xenos::TextureFormat::k_2_10_10_10 })
        refused(std::format("texture format {}", unsigned(format)), texture([=](xenos::xe_gpu_texture_fetch_t& f)
                                                                            {
                                                                                f.format = format;
                                                                            }),
                std::format("scene_texture_format {}", unsigned(format)));
    refused("a LOD bias under point minification and linear magnification", texture([](xenos::xe_gpu_texture_fetch_t& f)
                                                                                    {
                                                                                        f.lod_bias   = 1;
                                                                                        f.min_filter = xenos::TextureFilter::kPoint;
                                                                                    }),
            "scene_sampler_lod_bias");
    {
        // The bake's -0.5 bias under linear filters becomes the host mip bias.
        auto biased = base;
        texture([](xenos::xe_gpu_texture_fetch_t& f)
                {
                    f.lod_bias = -16;
                })(biased);
        SceneDrawRecipe recipe;
        UNSCOPED_INFO(std::string("a LOD bias under equal filters builds as the host mip bias: ") + (error));
        REQUIRE((BuildSceneDrawRecipe(biased, *catalog, recipe, error) && recipe.samplers[0].mipBias == -0.5F && recipe.native.textureMask == 1));
    }
    {
        // The pass-3 depth resolve sampled as k_24_8: endian 2, swizzle XY11.
        auto depth = base;
        texture([](xenos::xe_gpu_texture_fetch_t& f)
                {
                    f.format     = xenos::TextureFormat::k_24_8;
                    f.endianness = xenos::Endian::k8in32;
                    f.swizzle    = 0xB48;
                })(depth);
        NativeTextureMemoryRanges ranges;
        UNSCOPED_INFO(std::string("the k_24_8 ranges: ") + (error));
        REQUIRE(GetNativeTextureMemoryRanges(depth.textureFetches[0], ranges, error));
        depth.textureBaseBytes[0].assign(ranges.base.size, 0xFF);
        depth.textureMipBytes[0].assign(ranges.mips.size, 0);
        SceneDrawRecipe recipe;
        const bool      built = BuildSceneDrawRecipe(depth, *catalog, recipe, error);
        float           first = 0;
        if (built && recipe.textures[0].payloadBytes().size() >= 4)
            std::memcpy(&first, recipe.textures[0].payloadBytes().data(), 4);
        UNSCOPED_INFO(std::string("a k_24_8 depth texture builds as R32Float depth: ") + (error));
        REQUIRE((built && recipe.textures[0].format == NativeDrawReplayTextureFormat::R32Float &&
                 recipe.textures[0].swizzle == std::array<std::uint8_t, 4>{ 0, 0, 5, 5 } && first == 1.0F));
    }
    {
        // K12 and K20 resolves preserve packed words through matching host
        // formats.
        struct Resolved
        {
            xenos::TextureFormat          format;
            xenos::Endian                 endian;
            std::uint32_t                 swizzle, signs;
            bool                          integer;
            NativeDrawReplayTextureFormat host;
            std::array<std::uint8_t, 4>   hostSwizzle;
            std::uint32_t                 word;
        };

        for (const auto& value : { Resolved{ xenos::TextureFormat::k_16_16_FLOAT, xenos::Endian::k8in32, 0xB48, 1, true, NativeDrawReplayTextureFormat::Rg16Float, { 0, 1, 5, 5 }, 0x3C00BC00 },
                                   Resolved{ xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16, xenos::Endian::kNone, 0x688, 0, false, NativeDrawReplayTextureFormat::Rgb10a2, { 0, 1, 2, 3 }, 0xC00FFC01 } })
        {
            auto resolved = base;
            texture([&](xenos::xe_gpu_texture_fetch_t& f)
                    {
                        f.format     = value.format;
                        f.endianness = value.endian;
                        f.swizzle    = value.swizzle;
                        f.sign_x = f.sign_y = f.sign_z = f.sign_w = static_cast<xenos::TextureSign>(value.signs);
                        f.num_format                              = value.integer;
                    })(resolved);
            NativeTextureMemoryRanges ranges;
            UNSCOPED_INFO(std::string("the resolve output ranges: ") + (error));
            REQUIRE(GetNativeTextureMemoryRanges(resolved.textureFetches[0], ranges, error));
            // Guest words are stored big-endian under 8in32 and as host words otherwise.
            const auto stored = value.endian == xenos::Endian::k8in32 ? std::byteswap(value.word) : value.word;
            resolved.textureBaseBytes[0].resize(ranges.base.size);
            for (std::size_t offset = 0; offset + 4 <= ranges.base.size; offset += 4)
                std::memcpy(resolved.textureBaseBytes[0].MutableData() + offset, &stored, 4);
            resolved.textureMipBytes[0].assign(ranges.mips.size, 0);
            SceneDrawRecipe recipe;
            const bool      built = BuildSceneDrawRecipe(resolved, *catalog, recipe, error);
            std::uint32_t   first = 0;
            if (built && recipe.textures[0].payloadBytes().size() >= 4)
                std::memcpy(&first, recipe.textures[0].payloadBytes().data(), 4);
            UNSCOPED_INFO(std::string(std::format("texture format {} builds from its guest words: ", unsigned(value.format))) + (error));
            REQUIRE((built && recipe.textures[0].format == value.host && recipe.textures[0].swizzle == value.hostSwizzle && first == value.word));
        }
    }
    refused("a texture fetch the shadow does not hold", [](IndexedBufferDrawInputs& in)
            {
                in.textureFetches[0][3] ^= 1;
            },
            "scene_texture_fetch_mismatch");
    const auto vertexTexture = catalogOf({ interfaceFor(Draw{}, true, 4, true) });
    refused("a vertex-stage texture the reader did not snapshot", [](IndexedBufferDrawInputs&)
            {
            },
            "scene_vertex_fetch_unread",
            vertexTexture.get());
    auto cube              = interfaceFor(Draw{}, true, 4);
    cube.textureDimensions = { NativeDrawReplayTextureDimension::TextureCube };
    refused("a catalog dimension the PS does not sample", [](IndexedBufferDrawInputs&)
            {
            },
            "scene_texture_fetch_layout",
            catalogOf({ cube }).get());
    std::cout << "Scene builder refusals: one per enumerated register, the target form, specialization, tuple, stream, literal and texture rules\n";
}

// K12/K20 build depthless host formats with fixed blend and mask rules.
void packed()
{
    std::string error;

    struct Case
    {
        const SceneTargetForm*       form;
        NativeDrawReplayTargetFormat host;
    };

    const std::array<Case, 4> cases{ { { &kSceneTargetForms[7], NativeDrawReplayTargetFormat::Rg16Snorm },
                                       { &kSceneTargetForms[8], NativeDrawReplayTargetFormat::Rg16Snorm },
                                       { &kSceneTargetForms[9], NativeDrawReplayTargetFormat::Rgb10a2 },
                                       { &kSceneTargetForms[10], NativeDrawReplayTargetFormat::Rgb10a2 } } };
    for (const auto& [form, host] : cases)
    {
        Draw draw;
        draw.form               = *form;
        draw.depth              = 0x24F00730;
        draw.blend              = host == NativeDrawReplayTargetFormat::Rg16Snorm ? 0x01010101 : 0x00010001;
        draw.color              = 0x87000007;
        auto entry              = interfaceFor(draw, false, form->sampleCount);
        auto rgba8              = entry;
        entry.colorTargetFormat = SceneTargetColorFormat(*form);
        const auto catalog      = catalogOf({ entry });
        const auto shaders      = ScenePixelShaders(*catalog);
        auto       memory       = memoryFor(draw);
        const auto base         = readScene(memory, shaders);
        const auto name         = std::format("form {:#x}/{:#x}", form->surfaceInfo, form->colorInfo);
        const bool fog          = host == NativeDrawReplayTargetFormat::Rg16Snorm;
        const auto blends       = fog ? std::vector<std::uint32_t>{ 0x01010101, 0x01810181, 0x01410141 } : std::vector<std::uint32_t>{ 0x00010001 };
        const auto masks        = fog ? std::vector<std::uint32_t>{ 0xF, 1, 2 } : std::vector<std::uint32_t>{ 0xF };
        for (const auto blend : blends)
            for (const auto mask : masks)
            {
                auto in = base;
                store(in.state, shadowOf(0x2201), blend);
                store(in.state, shadowOf(0x2104), mask);
                SceneDrawRecipe recipe;
                UNSCOPED_INFO(std::string(std::format("{} blend {:#x} mask {:#x} builds: ", name, blend, mask)) + (error));
                REQUIRE(BuildSceneDrawRecipe(in, *catalog, recipe, error));
                const auto& native = recipe.native;
                UNSCOPED_INFO(name + " builds into its host format without depth");
                REQUIRE((recipe.form == form && native.targetFormat == host && !native.depth.enabled && !native.depth.writeEnabled && !native.stencil.enabled &&
                         native.blend.writeMask == mask && native.width == form->width && native.height == form->height && native.sampleCount == form->sampleCount));
            }
        const auto refused = [&](const std::string& what, auto mutate, const std::string& expected, const SceneDrawCatalog* other = nullptr)
        {
            auto in = base;
            mutate(in);
            SceneDrawRecipe recipe;
            std::string     reason;
            const bool      built = BuildSceneDrawRecipe(in, other ? *other : *catalog, recipe, reason);
            UNSCOPED_INFO(name + " " + what + ": " + (built ? "accepted" : "[" + reason + "]"));
            REQUIRE((!built && reason == expected));
        };
        const auto at = [](std::uint32_t reg, std::uint32_t value)
        {
            return [=](IndexedBufferDrawInputs& in)
            {
                store(in.state, shadowOf(reg), value);
            };
        };
        refused("with depth testing", at(0x2200, 0x24F00732), "scene_packed_target_depth: 0x24f00732");
        refused("with depth writes", at(0x2200, 0x24F00736), "scene_packed_target_depth: 0x24f00736");
        refused("writing blue alone", at(0x2104, 4), "scene_fixed_state 0x2104: 0x4");
        refused("writing red and green", at(0x2104, 3), "scene_fixed_state 0x2104: 0x3");
        if (!fog)
            refused("writing red alone", at(0x2104, 1), "scene_fixed_state 0x2104: 0x1");
        const auto formError = [&](std::uint32_t colorInfo)
        {
            return std::format("scene_target_form: {:#x} {:#x} {:#x} {:#x}", form->surfaceInfo, colorInfo, form->depthInfo, form->scissor);
        };
        refused("at the next EDRAM tile", at(0x2001, form->colorInfo + 1), formError(form->colorInfo + 1));
        refused("with a nonzero exponent bias", at(0x2001, form->colorInfo | 0x100000), formError(form->colorInfo | 0x100000));
        refused("against an RGBA8 entry", [](IndexedBufferDrawInputs&)
                {
                },
                std::format("scene_specialization: alpha 0 samples {} program {:#x} context {:#x} interpolators {:#x}", form->sampleCount, draw.pair.program, draw.pair.context, draw.pair.interpolators),
                catalogOf({ rgba8 }).get());
    }
    {
        // An RGBA8 form keeps its full color mask and does not take a packed entry.
        Draw draw;
        draw.form                     = kSceneTargetForms[4];
        draw.depth                    = 0x24F00730;
        draw.blend                    = 0x01010101;
        draw.color                    = 0x87000007;
        auto entry                    = interfaceFor(draw, false, 1);
        auto packedEntry              = entry;
        packedEntry.colorTargetFormat = 4;
        auto            memory        = memoryFor(draw);
        const auto      plain         = catalogOf({ entry });
        auto            inputs        = readScene(memory, ScenePixelShaders(*plain));
        SceneDrawRecipe recipe;
        store(inputs.state, shadowOf(0x2104), 1);
        UNSCOPED_INFO(std::string("an RGBA8 form refuses a red-only mask: ") + (error));
        REQUIRE((!BuildSceneDrawRecipe(inputs, *plain, recipe, error) && error == "scene_fixed_state 0x2104: 0x1"));
        store(inputs.state, shadowOf(0x2104), 0xF);
        UNSCOPED_INFO(std::string("an RGBA8 form refuses a k_16_16 entry: ") + (error));
        REQUIRE((!BuildSceneDrawRecipe(inputs, *catalogOf({ packedEntry }), recipe, error) && error.starts_with("scene_specialization: alpha 0 samples 1 ")));
    }
    std::cout << "Scene packed targets: both k_16_16 fog forms and both 2X k_2_10_10_10 forms build without depth in their host format under each harvested blend and mask; depth, masks, bases, bias and entry format are refused by name\n";
}

// Plane-0 clipping binds c29; each clip word requires its matching entry.
void userClip()
{
    std::string error;
    Draw        draw;
    draw.color              = 0x87000004;
    auto entry              = interfaceFor(draw, false, draw.form.sampleCount);
    auto clipEntry          = entry;
    clipEntry.userClipPlane = true;
    const auto both         = catalogOf({ entry, clipEntry });
    auto       memory       = memoryFor(draw);
    auto       base         = readScene(memory, ScenePixelShaders(*both));
    // Plane 0 of key 2EE8D7A9's record, then a distinct plane 5.
    const std::array<std::uint32_t, 4> plane{ 0xAF55A600U, 0x3BB5CC6CU, 0xBF7FFEB1U, 0x3EF9D4B4U };
    for (std::size_t i = 0; i < plane.size(); ++i)
    {
        store(base.state, 0x2820 + i * 4, plane[i]);
        store(base.state, 0x2870 + i * 4, 0x3F800000U + std::uint32_t(i));
    }
    const auto registers = GuestDrawRegisters(base.state);
    for (std::size_t i = 0; i < plane.size(); ++i)
    {
        UNSCOPED_INFO("the shadow at G+0x2820 maps to PA_CL_UCP_0_X..5_W");
        REQUIRE((registers[0x2388 + i] == plane[i] && registers[0x239C + i] == 0x3F800000U + i));
    }

    const auto withClip = [&](std::uint32_t clip)
    {
        auto in = base;
        store(in.state, shadowOf(0x2204), clip);
        return in;
    };
    SceneDrawRecipe recipe;
    UNSCOPED_INFO(std::string("a plane 0 clipped draw builds: ") + (error));
    REQUIRE(BuildSceneDrawRecipe(withClip(0x80001), *both, recipe, error));
    UNSCOPED_INFO("a plane 0 clipped draw takes the clipping entry");
    REQUIRE((recipe.entry->shader.userClipPlane && recipe.native.sharedConstants.size() == 480));
    for (std::size_t i = 0; i < plane.size(); ++i)
    {
        std::uint32_t value;
        std::memcpy(&value, recipe.native.sharedConstants.data() + 464 + i * 4, 4);
        UNSCOPED_INFO("the recipe binds plane 0 as c29");
        REQUIRE(value == plane[i]);
    }
    UNSCOPED_INFO(std::string("an unclipped draw takes the plain entry: ") + (error));
    REQUIRE((BuildSceneDrawRecipe(withClip(0x80000), *both, recipe, error) && !recipe.entry->shader.userClipPlane &&
             recipe.native.sharedConstants.size() == 336));

    const auto refused = [&](const std::string& what, std::uint32_t clip, const SceneDrawCatalog& catalog, const std::string& expected)
    {
        SceneDrawRecipe rejected;
        std::string     reason;
        const bool      built = BuildSceneDrawRecipe(withClip(clip), catalog, rejected, reason);
        UNSCOPED_INFO(what + ": " + (built ? "accepted" : "[" + reason + "]"));
        REQUIRE((!built && reason == expected));
    };
    const auto specialization = std::format("scene_specialization: alpha 0 samples {} program {:#x} context {:#x} interpolators {:#x}", draw.form.sampleCount, draw.pair.program, draw.pair.context, draw.pair.interpolators);
    refused("plane 0 without a clipping entry", 0x80001, *catalogOf({ entry }), specialization + " user clip plane 0");
    refused("an unclipped draw with only the clipping entry", 0x80000, *catalogOf({ clipEntry }), specialization);
    refused("planes 0 and 1", 0x80003, *both, "scene_fixed_state 0x2204: 0x80003");
    refused("plane 1", 0x80002, *both, "scene_fixed_state 0x2204: 0x80002");
    refused("plane 0 cull-only", 0xA0001, *both, "scene_fixed_state 0x2204: 0xa0001");
    refused("plane 0 with clipping off", 0x90001, *both, "scene_fixed_state 0x2204: 0x90001");
    std::cout << "Scene user clip plane: PA_CL_CLIP_CNTL 0x80001 builds through the clipping entry with plane 0 as c29; other planes, cull-only, clipping off and missing entries are refused by name\n";
}

// The bake form clears once, draws its batch, then resolves once.
void bakeFrame()
{
    Draw draw;
    draw.form          = kSceneTargetForms[2];
    draw.color         = 0x87000004;
    draw.blend         = 0x00010001;
    auto       memory  = memoryFor(draw);
    const auto catalog = fullCatalog();

    FakeCompletion             completion;
    NativeFrameAssemblerConfig config;
    config.completion = &completion;
    config.readTarget = [&memory](std::uint32_t address, std::uint32_t& out)
    {
        const auto bytes = memory.Read(address, 4, false);
        if (bytes.size() != 4)
            return false;
        out = word(bytes, 0);
        return true;
    };
    config.build = ComposeNativeDrawFamilies({ MakeSceneDrawFamily(readerFor(memory), catalog) });
    NativeFrameAssembler assembler(config);

    GuestClear clear;
    clear.target.native    = { 512, 512, 1, NativeDrawReplayTargetFormat::Rgba8, 0 };
    clear.flags            = 0x31;
    clear.clear.rectangle  = { 0, 0, 512, 512 };
    clear.clear.color      = true;
    clear.clear.depth      = true;
    clear.clear.colorValue = { 0, 0, 0, 1 };
    clear.depth            = GuestDepthSurface{ kDepthSurface, 0x720, 0x720, 512, 512, 1 };
    std::string error;
    UNSCOPED_INFO("the bake target clears: " + error);
    REQUIRE(assembler.Clear(clear, error) != 0);
    for (std::uint64_t i = 0; i < 33; ++i)
    {
        auto submission          = submissionFor(kSceneMeshDrawCaller);
        submission.sequence      = i + 1;
        submission.operationMark = assembler.NumberOperation();
        assembler.Began(submission);
        const auto  verdict = assembler.Admit(submission);
        std::string refusal;
        assembler.Recorded(submission, verdict, &refusal);
        UNSCOPED_INFO(std::format("bake draw {} is admitted: {}", i, refusal));
        REQUIRE(!verdict);
    }
    GuestResolveCopy copy;
    copy.copyControl   = 0x100000;
    copy.destBase      = 0x1E3B0000;
    copy.destInfo      = 0x01000300;
    copy.rectWidth     = 512;
    copy.rectHeight    = 512;
    bool       partial = false;
    const auto mark    = assembler.Resolve(clear.target, copy, false, partial, error);
    UNSCOPED_INFO("the full-rect RGBA8 resolve is modeled: " + error);
    REQUIRE((mark != 0 && !partial));
    completion::DrawFlush flush;
    for (unsigned attempt = 0; attempt < 4 && !flush.fence; ++attempt)
        flush = assembler.FlushThrough(mark);
    UNSCOPED_INFO("the resolve frame is submitted: " + flush.error);
    REQUIRE((flush.fence != 0 && !assembler.Failure()));
    const auto span = std::ranges::find(completion.spans, mark, &NativeCompletionSpan::resolveMark);
    UNSCOPED_INFO("a span carries the resolve");
    REQUIRE(span != completion.spans.end());
    UNSCOPED_INFO(std::format("the bake resolve frame holds 33 draws and one resolve (status {}, {} passes)", NativeFrameStatusName(span->status), span->frame ? span->frame->passes.size() : 0));
    REQUIRE(((span->status == NativeFrameStatus::Complete || span->status == NativeFrameStatus::Provisional) && span->guestResolve && span->frame &&
             span->frame->passes.size() == 1 && span->frame->passes[0].DrawCount() == 33 && span->frame->passes[0].resolves.size() == 1 &&
             span->frame->passes[0].target.width == 512 && span->frame->passes[0].target.sampleCount == 1 && span->frame->passes[0].target.depthEdramBase == 0x720));
    std::cout << std::format("Bake frame: 33 scene draws into 512x512 at pitch 560 and one full-rect resolve are {}\n", NativeFrameStatusName(span->status));
}

// A float3 position at stride 12, as the depth-only VS fetches it, with the
// depth-only VS metadata's program and context words.
Pair depthPair()
{
    Pair pair;
    pair.program  = 0x10003;
    pair.context  = 0;
    pair.stride   = 12;
    pair.elements = { { 0, kFloat3, 0, 0, 57 } };
    return pair;
}

// A depth-only draw into `form`: no pixel shader, the VS words as the fetch
// patch leaves them, RB_COLOR_MASK 0, RB_MODECONTROL 5 and the unfolded SQ
// words 0x10003 / 0 / 0x10003 of the captured depth pass.
FakeMemory depthOnlyMemory(const SceneTargetForm& form = kSceneTargetForms[3])
{
    Draw draw;
    draw.pair                                            = depthPair();
    draw.form                                            = form;
    draw.depth                                           = 0x24F00776;
    draw.blend                                           = 0x00010001;
    draw.color                                           = 0x87000007;
    auto                                          memory = memoryFor(draw);
    const auto                                    digest = IndexedBufferMicrocodeWordDigest(psCode(draw.pair));
    const std::array<IndexedBufferPixelShader, 1> shaders{ { { digest, 1 } } };
    const auto                                    inputs = readScene(memory, shaders);
    std::vector<std::uint8_t>                     patched;
    std::string                                   error;
    UNSCOPED_INFO(std::string("synthetic depth-only fetch patch: ") + (error));
    REQUIRE(PatchVertexFetch(inputs, std::vector<std::uint32_t>{ 0 }, patched, error));
    memory.At(kVsResource, true).bytes = patched;
    auto& state                        = memory.At(kG, false).bytes;
    put(state, 0x3194, 0);
    for (const auto [reg, value] : { std::pair{ 0x2104U, 0U }, { 0x2208U, 5U }, { 0x2180U, 0x10003U }, { 0x2181U, 0U }, { 0x2182U, 0x10003U } })
        put(state, shadowOf(reg), value);
    return memory;
}

NativeGameplayShaderInterface depthOnlyInterface(std::span<const std::uint8_t> vertexCode)
{
    NativeGameplayShaderInterface shader;
    shader.depthOnly             = true;
    shader.vertexShaderHash      = 0x289B3AEDEC76A40FULL;
    shader.vertexGuestWordDigest = IndexedBufferMicrocodeWordDigest(vertexCode);
    shader.vertexShaderDxil      = { 'D', 'X', 'B', 'V' };
    shader.vertexDxilDigest      = dxilDigest(shader.vertexShaderDxil);
    shader.rasterizedDepth       = true;
    shader.independentSamplers   = true;
    shader.shaderProgram         = 0x10003;
    shader.shaderInterpolators   = 0x10003;
    return shader;
}

// The patch digest the Scene family hands its depth-only reader.
PatchedVertexDigest depthPatched()
{
    return [](const IndexedBufferDrawInputs& in, std::uint32_t stride)
    {
        std::vector<std::uint32_t> offsets;
        for (std::size_t at = 0; at + 12 <= in.declaration.size(); at += 12)
            offsets.push_back(word(in.declaration, at) & 0xFFFF);
        const auto identity = MemoizedVertexShaderIdentity(in, offsets, false, stride);
        return identity->ok ? identity->digest : std::string();
    };
}

// Depth-only reads use no PS and name every refused mask/color rule.
void depthOnly()
{
    auto       memory  = depthOnlyMemory();
    auto       catalog = fullCatalog();
    const auto vsCode  = memory.At(kVsResource, true).bytes;
    catalog->depthOnly.push_back({ depthOnlyInterface(vsCode) });
    const auto shaders = ScenePixelShaders(*catalog);
    const auto listed  = SceneDepthOnlyVertexShaders(*catalog);
    UNSCOPED_INFO("the reader lists the depth-only VS once");
    REQUIRE((listed.size() == 1 && listed[0] == catalog->depthOnly[0].shader.vertexGuestWordDigest));

    IndexedBufferDrawInputs inputs;
    std::string             error;
    UNSCOPED_INFO(std::string("the Scene form reads a listed VS without a pixel shader: ") + (error));
    REQUIRE((ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), readerFor(memory), shaders, inputs, error, IndexedBufferForm::Scene, listed, depthPatched()) && inputs.inputsComplete &&
             inputs.sceneCandidate && !inputs.pixelObject && inputs.pixelCode.empty() && inputs.pixelHeader.empty() && inputs.textureFetches.empty() &&
             inputs.variant == 0 && inputs.arguments.stride == 12 && inputs.vertexCode == vsCode));
    {
        IndexedBufferDrawInputs foreign;
        UNSCOPED_INFO("without a depth-only list a draw without a pixel shader is foreign");
        REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), readerFor(memory), shaders, foreign, error, IndexedBufferForm::Scene) && error.empty()));
        UNSCOPED_INFO("the words in guest memory never list a VS: without a patch digest even the listed words are foreign");
        REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), readerFor(memory), shaders, foreign, error, IndexedBufferForm::Scene, listed) && error.empty()));
        const std::array<std::string_view, 1> other{ std::string_view(catalog->entries[0].shader.vertexGuestWordDigest) };
        UNSCOPED_INFO("a VS outside the depth-only list is foreign");
        REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), readerFor(memory), shaders, foreign, error, IndexedBufferForm::Scene, other) && error.empty()));
    }

    SceneDrawRecipe recipe;
    UNSCOPED_INFO(std::string("the depth-only draw builds: ") + (error));
    REQUIRE(BuildSceneDrawRecipe(inputs, *catalog, recipe, error));
    const auto& native = recipe.native;
    UNSCOPED_INFO("a depth-only draw builds a pixel-less 1024x1024 recipe that writes depth only");
    REQUIRE((recipe.entry == &catalog->depthOnly[0] && recipe.form == &kSceneTargetForms[3] && native.depthOnly && native.pixelShaderHash == 0 &&
             native.pixelShaderBytes().empty() && native.width == 1024 && native.height == 1024 && native.sampleCount == 1 && native.blend.writeMask == 0 &&
             native.depth.enabled && native.depth.writeEnabled && native.vertexAttributeCount == 1 && native.textureMask == 0 &&
             native.indexCount == kIndexCount));
    {
        NativeDrawReplayRecipe draw;
        std::string            detail;
        UNSCOPED_INFO(std::string("the family builds the depth-only draw: ") + (detail));
        REQUIRE((MakeSceneDrawFamily(readerFor(memory), catalog)(submissionFor(kSceneMeshDrawCaller), draw, detail) == NativeDrawFamilyResult::Built && draw.depthOnly));
    }

    // 0x826ADD88 patches fetch words before execution; patched words still
    // select the entry.
    {
        std::vector<std::uint8_t> otherStride;
        auto                      wider = inputs;
        wider.arguments.stride          = 16;
        UNSCOPED_INFO(std::string("a stride-16 patch writes other words: ") + (error));
        REQUIRE((PatchDepthOnlyVertexFetch(wider, otherStride, error) && otherStride != vsCode));
        const auto patchedDigest = depthPatched();
        {
            auto stale                        = memory;
            stale.At(kVsResource, true).bytes = otherStride;
            IndexedBufferDrawInputs withoutPatch;
            UNSCOPED_INFO("without the patch digest a VS holding other words is foreign");
            REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), readerFor(stale), shaders, withoutPatch, error, IndexedBufferForm::Scene, listed) && error.empty()));
        }
        auto templates = vsCode;
        for (std::size_t i = 0; i * 4 < inputs.fetchRecords.size(); ++i)
            for (std::size_t j = 0; j < 3; ++j)
                store(templates, (word(inputs.fetchRecords, i * 4) & 0xFFF) * 12 + j * 4, word(inputs.fetchTemplates, i * 12 + j * 4));
        UNSCOPED_INFO("the fetch templates differ from the patched words");
        REQUIRE(templates != vsCode);
        for (const auto& [name, words] : { std::pair{ "the fetch templates", templates }, std::pair{ "a stride-16 patch", otherStride } })
        {
            auto stale                        = memory;
            stale.At(kVsResource, true).bytes = words;
            IndexedBufferDrawInputs staleInputs;
            UNSCOPED_INFO(std::string(std::format("the Scene form reads a depth-only draw whose VS holds {}: ", name)) + (error));
            REQUIRE((ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), readerFor(stale), shaders, staleInputs, error, IndexedBufferForm::Scene, listed, patchedDigest) &&
                     staleInputs.sceneCandidate && staleInputs.vertexCode == words));
            SceneDrawRecipe staleRecipe;
            UNSCOPED_INFO(std::string(std::format("a depth-only draw whose VS holds {} builds the patched draw: ", name)) + (error));
            REQUIRE((BuildSceneDrawRecipe(staleInputs, *catalog, staleRecipe, error) && staleRecipe.patchedVertexCode == vsCode &&
                     staleRecipe.entry == &catalog->depthOnly[0] && std::ranges::equal(staleRecipe.native.vertexShaderBytes(), native.vertexShaderBytes()) &&
                     staleRecipe.native.vertexData == native.vertexData && staleRecipe.native.sharedConstants == native.sharedConstants &&
                     staleRecipe.native.vertexAttributeCount == native.vertexAttributeCount));
            NativeDrawReplayRecipe draw;
            std::string            detail;
            UNSCOPED_INFO(std::string(std::format("the family builds the depth-only draw whose VS holds {}: ", name)) + (detail));
            REQUIRE((MakeSceneDrawFamily(readerFor(stale), catalog)(submissionFor(kSceneMeshDrawCaller), draw, detail) == NativeDrawFamilyResult::Built && draw.depthOnly));
        }
        // Words outside the list that no patch makes listed stay foreign.
        auto unlisted                        = memory;
        unlisted.At(kVsResource, true).bytes = templates;
        store(unlisted.At(kVsResource, true).bytes, 0, 0);
        IndexedBufferDrawInputs foreign;
        UNSCOPED_INFO("a VS whose patched words are not listed is foreign");
        REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), readerFor(unlisted), shaders, foreign, error, IndexedBufferForm::Scene, listed, patchedDigest) && error.empty()));
        NativeDrawReplayRecipe draw;
        std::string            detail;
        UNSCOPED_INFO(std::string("the family names an unlisted depth-only VS: ") + (detail));
        REQUIRE((MakeSceneDrawFamily(readerFor(unlisted), catalog)(submissionFor(kSceneMeshDrawCaller), draw, detail) != NativeDrawFamilyResult::Built && detail == "scene_no_pixel_shader"));
        // The listed words as read never admit, but a draw whose own patch is not
        // listed keeps the name the build gave it before the host-patched routing.
        auto strided                        = memory;
        strided.At(kG, false).bytes[0x30F0] = 4; // stride 16, which the listed words do not hold
        NativeDrawReplayRecipe stridedDraw;
        std::string            stridedDetail;
        UNSCOPED_INFO(std::string("the family names a listed VS whose stride patch is not listed: ") + (stridedDetail));
        REQUIRE((MakeSceneDrawFamily(readerFor(strided), catalog)(submissionFor(kSceneMeshDrawCaller), stridedDraw, stridedDetail) != NativeDrawFamilyResult::Built && stridedDetail == "scene_depth_only_fetch_patch"));
    }

    const auto refused = [&](const std::string& name, auto mutate, const std::string& expected)
    {
        auto broken = inputs;
        mutate(broken);
        SceneDrawRecipe rejected;
        std::string     reason;
        const bool      built = BuildSceneDrawRecipe(broken, *catalog, rejected, reason);
        UNSCOPED_INFO(name + ": " + (built ? "accepted" : "[" + reason + "]"));
        REQUIRE((!built && reason == expected));
    };
    const auto at = [](std::uint32_t reg, std::uint32_t value)
    {
        return [=](IndexedBufferDrawInputs& in)
        {
            store(in.state, shadowOf(reg), value);
        };
    };
    refused("pixel shader words on a depth-only draw", [](IndexedBufferDrawInputs& in)
            {
                in.pixelCode = psCode(meshPair());
            },
            "scene_inputs_incomplete");
    refused("a color mask in depth-only mode", at(0x2104, 0xF), "scene_fixed_state 0x2104: 0xf");
    refused("a depth-only draw inside the Z prepass", [](IndexedBufferDrawInputs& in)
            {
                in.state[0x2ABC] |= 0x40;
                store(in.state, shadowOf(0x2200), 0x24F00736);
                store(in.state, shadowOf(0x2201), 0x00010001);
            },
            "scene_conditional_or_recorded_shader: zpass depth-only draw");
    {
        // A7E9CB62 records the tested no-PS state; unused interpolator state is
        // ignored.
        auto tested = inputs, untested = inputs;
        for (auto* in : { &tested, &untested })
        {
            store(in->state, shadowOf(0x2200), 0x24F00736);
            store(in->state, shadowOf(0x210E), 0x3F169697);
        }
        store(tested.state, shadowOf(0x2182), 0x3F003F);
        store(tested.state, shadowOf(0x2202), 0x8700000C);
        store(untested.state, shadowOf(0x2202), 0x87000007);
        SceneDrawRecipe withTest, withoutTest;
        UNSCOPED_INFO(std::string("the alpha-tested depth-only draw builds: ") + (error));
        REQUIRE(BuildSceneDrawRecipe(tested, *catalog, withTest, error));
        UNSCOPED_INFO(std::string("its untested neighbor builds: ") + (error));
        REQUIRE(BuildSceneDrawRecipe(untested, *catalog, withoutTest, error));
        const auto& a = withTest.native;
        const auto& b = withoutTest.native;
        UNSCOPED_INFO("the alpha-tested depth-only draw builds its untested neighbor's pixel-less recipe");
        REQUIRE((withTest.entry == &catalog->depthOnly[0] && a.depthOnly && a.pixelShaderBytes().empty() && a.blend.writeMask == 0 && a.depth.enabled &&
                 a.depth.writeEnabled && a.depth.compare == b.depth.compare && a.sharedConstants == b.sharedConstants && a.vertexData == b.vertexData &&
                 std::ranges::equal(a.indexSpan(), b.indexSpan()) && a.viewport.width == b.viewport.width && a.scissor.right == b.scissor.right));
    }
    refused("alpha to mask", at(0x2202, 0x87000017), "scene_color_control: 0x87000017");
    refused("an alpha test with alpha-to-mask offsets", at(0x2202, 0x8701000C), "scene_color_control: 0x8701000c");
    // Under an unchanged depth-only mode the SQ words follow the dirty word
    // at G+20: 0x180000 rewrites them from the VS metadata (loc_826AE1F0);
    // other dirty bits, or none (no commit call), keep the stale ones.
    {
        auto rewritten = inputs;
        store(rewritten.state, shadowOf(0x2180), 0x10110201);
        store(rewritten.state, shadowOf(0x2181), 8);
        store(rewritten.state, 20, 0x001A0000);
        SceneDrawRecipe committed;
        UNSCOPED_INFO(std::string("SQ dirty bits rewrite stale SQ words under an unchanged depth-only mode: ") + (error));
        REQUIRE((BuildSceneDrawRecipe(rewritten, *catalog, committed, error) && committed.native.depthOnly &&
                 word(committed.effectiveState, shadowOf(0x2180)) == 0x10003 && word(committed.effectiveState, shadowOf(0x2181)) == 0));
    }
    for (const std::uint32_t dirty : { 0x00060000U, 0U })
        refused(std::format("stale SQ words under an unchanged depth-only mode with dirty word {:#x}", dirty), [=](IndexedBufferDrawInputs& in)
                {
                    store(in.state, shadowOf(0x2180), 0x10110201);
                    store(in.state, shadowOf(0x2181), 8);
                    store(in.state, 20, dirty);
                },
                "scene_specialization: depth-only program 0x10110201 context 0x8 interpolators 0x10003");
    refused("a color-mode shadow without the commit call", [](IndexedBufferDrawInputs& in)
            {
                store(in.state, shadowOf(0x2208), 4);
                store(in.state, 20, 0);
            },
            "scene_fixed_state 0x2208: 0x4");
    refused("a VS with exports to link", [](IndexedBufferDrawInputs& in)
            {
                store(in.vertexMetadata, 20, 1);
            },
            "scene_depth_only_linkage");
    refused("a 10:10:10:2 color format the commit keeps", [](IndexedBufferDrawInputs& in)
            {
                store(in.state, shadowOf(0x2208), 4);
                store(in.state, shadowOf(0x2001), 0x20000);
            },
            "scene_target_form: 0x10000410 0x20000 0x4c0 0x4000400");
    refused("the k_16_16 fog form, which has no depth attachment", [](IndexedBufferDrawInputs& in)
            {
                for (const auto [reg, value] : { std::pair{ 0x2000U, 0x14000500U }, { 0x2001U, 0x00040000U }, { 0x2002U, 0x530U }, { 0x2082U, 0x02D00500U } })
                    store(in.state, shadowOf(reg), value);
            },
            "scene_target_form: 0x14000500 0x40000 0x530 0x2d00500");

    // A shadow still in the previous draw's color mode and SQ words commits
    // as the depth-only draw: mode 5 and the VS metadata's words, and under
    // G+0x2ABF bit 3 RB_COLOR_INFO without its format bits.
    for (const bool clearFormat : { false, true })
    {
        auto stale = inputs;
        store(stale.state, shadowOf(0x2208), 4);
        store(stale.state, shadowOf(0x2180), 0x10110201);
        store(stale.state, shadowOf(0x2181), 8);
        // The common captured dirty word of a depth-only draw.
        store(stale.state, 20, 0x001A0000);
        if (clearFormat)
        {
            store(stale.state, shadowOf(0x2001), 0x20000);
            stale.state[0x2ABF] |= 8;
        }
        SceneDrawRecipe committed;
        UNSCOPED_INFO(std::string(std::format("a stale color-mode shadow{} commits as the depth-only draw: ", clearFormat ? " with the format clear" : "")) + (error));
        REQUIRE((BuildSceneDrawRecipe(stale, *catalog, committed, error) && committed.native.depthOnly && committed.native.blend.writeMask == 0 &&
                 committed.form == &kSceneTargetForms[3] && word(committed.effectiveState, shadowOf(0x2208)) == 5 &&
                 word(committed.effectiveState, shadowOf(0x2180)) == 0x10003 && word(committed.effectiveState, shadowOf(0x2181)) == 0 &&
                 word(committed.effectiveState, shadowOf(0x2001)) == 0));
    }
    refused("a depth control outside the table", at(0x2200, 0x24F00716), "scene_depth_control: 0x24f00716");
    refused("the width as the 1X pitch", at(0x2000, 0x10000400), "scene_target_form: 0x10000400 0x0 0x4c0 0x4000400");
    refused("VS variant one", [](IndexedBufferDrawInputs& in)
            {
                in.variant = 1;
            },
            "scene_depth_only_variant");
    refused("a declaration the fetch patch applies differently", [](IndexedBufferDrawInputs& in)
            {
                store(in.declaration, 4, kFloat2);
            },
            "scene_depth_only_fetch_patch");
    refused("a stream stride other than the listed words'", [](IndexedBufferDrawInputs& in)
            {
                in.arguments.stride = 16;
            },
            "scene_depth_only_fetch_patch");
    refused("a caller outside the allowlist", [](IndexedBufferDrawInputs& in)
            {
                in.arguments.caller = 0x82510D4C; // the particle overflow path's IndexedBuffer
            },
            "scene_draw_tuple");
    {
        // An entry with another SQ program or context leaves the draw without
        // a specialization; SQ_INTERPOLATOR_CNTL alone does not, as no PS reads it.
        auto other                               = std::make_shared<SceneDrawCatalog>(*catalog);
        other->depthOnly[0].shader.shaderContext = 1;
        SceneDrawRecipe rejected;
        UNSCOPED_INFO(std::string("a depth-only entry of other SQ words is named: ") + (error));
        REQUIRE((!BuildSceneDrawRecipe(inputs, *other, rejected, error) && error == "scene_specialization: depth-only program 0x10003 context 0x0 interpolators 0x10003"));
    }
    std::cout << "Scene depth-only form: a listed VS without a PS builds a pixel-less 1024x1024 recipe; a stale color mode commits to it; an alpha test is a no-op; masks, alpha-to-mask, SQ, linkage, variant, patch, stride and tuple refusals are named\n";
}

// One assembler over a target that `memory` holds, with the guest's own
// target, tiling and depth decode; clears, draws and resolves as the hooks
// report them.
struct OffscreenSession
{
    FakeMemory&                           memory;
    FakeCompletion                        completion;
    NativeFrameAssemblerConfig            config;
    std::unique_ptr<NativeFrameAssembler> assembler;
    std::uint64_t                         sequence = 0;

    OffscreenSession(FakeMemory& source, std::shared_ptr<SceneDrawCatalog> catalog)
    : memory(source)
    {
        config.completion = &completion;
        config.readTarget = [this](std::uint32_t address, std::uint32_t& out)
        {
            const auto bytes = memory.Read(address, 4, false);
            if (bytes.size() != 4)
                return false;
            out = word(bytes, 0);
            return true;
        };
        config.build = ComposeNativeDrawFamilies({ MakeSceneDrawFamily(readerFor(memory), std::move(catalog)) });
        assembler    = std::make_unique<NativeFrameAssembler>(config);
    }

    // The host holds no depth surface for a 10:10:10:2 target, so its clear leaves depth out.
    GuestTarget Clear(bool clearDepth = true)
    {
        GuestClear        clear;
        GuestTiling       tiling;
        GuestDepthSurface depth;
        UNSCOPED_INFO("the synthetic target, tiling and depth decode");
        REQUIRE((ReadGuestTargetSet(config.readTarget, kG, 0, clear.target) && ReadGuestTiling(config.readTarget, kG, tiling) &&
                 ReadGuestDepthSurface(config.readTarget, kG, tiling, depth)));
        clear.tiling           = tiling;
        clear.depth            = depth;
        clear.flags            = 0x31;
        clear.clear.rectangle  = { 0, 0, std::int32_t(clear.target.native.width), std::int32_t(clear.target.native.height) };
        clear.clear.color      = true;
        clear.clear.depth      = clearDepth;
        clear.clear.colorValue = { 0, 0, 0, 1 };
        std::string error;
        UNSCOPED_INFO("the synthetic target clears: " + error);
        REQUIRE(assembler->Clear(clear, error) != 0);
        return clear.target;
    }

    void Draws(unsigned count)
    {
        for (unsigned i = 0; i < count; ++i)
        {
            auto submission          = submissionFor(kSceneMeshDrawCaller);
            submission.sequence      = ++sequence;
            submission.operationMark = assembler->NumberOperation();
            assembler->Began(submission);
            const auto  verdict = assembler->Admit(submission);
            std::string refusal;
            assembler->Recorded(submission, verdict, &refusal);
            UNSCOPED_INFO(std::format("synthetic draw {} is admitted: {}", sequence, refusal));
            REQUIRE(!verdict);
        }
    }

    // The resolve's mark, or zero with `error` naming the refusal.
    std::uint64_t Resolve(const GuestTarget& target, std::uint32_t destBase, std::uint32_t destInfo, std::string& error)
    {
        GuestResolveCopy copy;
        copy.copyControl = 0x100000;
        copy.destBase    = destBase;
        copy.destInfo    = destInfo;
        copy.rectWidth   = target.native.width;
        copy.rectHeight  = target.native.height;
        bool partial     = false;
        return assembler->Resolve(target, copy, false, partial, error);
    }

    const NativeCompletionSpan& Flush(std::uint64_t mark)
    {
        completion::DrawFlush flush;
        for (unsigned attempt = 0; attempt < 4 && !flush.fence; ++attempt)
            flush = assembler->FlushThrough(mark);
        UNSCOPED_INFO("the resolve frame is submitted: " + flush.error);
        REQUIRE((flush.fence != 0 && !assembler->Failure()));
        const auto span = std::ranges::find(completion.spans, mark, &NativeCompletionSpan::resolveMark);
        UNSCOPED_INFO("a span carries the resolve");
        REQUIRE((span != completion.spans.end() && span->frame && span->frame->textures.size() == 1 && span->guestResolve));
        return *span;
    }
};

// Whether two reads of a draw yield the same inputs.
bool sameInputs(const IndexedBufferDrawInputs& a, const IndexedBufferDrawInputs& b)
{
    return a.inputsComplete == b.inputsComplete && a.sceneCandidate == b.sceneCandidate && a.pixelShader == b.pixelShader && a.variant == b.variant && a.state == b.state &&
           a.vertexHeader == b.vertexHeader && a.pixelHeader == b.pixelHeader && a.vertexMetadata == b.vertexMetadata && a.pixelMetadata == b.pixelMetadata &&
           a.vertexCode == b.vertexCode && a.pixelCode == b.pixelCode && a.vertexLiterals == b.vertexLiterals && a.pixelLiterals == b.pixelLiterals &&
           a.vertexLiteralRegisters == b.vertexLiteralRegisters && a.pixelLiteralRegisters == b.pixelLiteralRegisters &&
           a.vertexLiteralRanges.size() == b.vertexLiteralRanges.size() && a.pixelLiteralRanges.size() == b.pixelLiteralRanges.size() &&
           a.fetchRecords == b.fetchRecords && a.fetchTemplates == b.fetchTemplates && a.declarationHeader == b.declarationHeader &&
           a.declaration == b.declaration && a.swizzleTable == b.swizzleTable && a.formatTable == b.formatTable && std::ranges::equal(indexBytes(a), indexBytes(b)) &&
           a.streamBytes == b.streamBytes && a.textureFetches == b.textureFetches && a.textureBaseBytes == b.textureBaseBytes;
}

// Each texture range belongs to its fetch, independent of prior reads.
void textureRangeAnswers()
{
    const auto  catalog = fullCatalog();
    const auto  shaders = ScenePixelShaders(*catalog);
    auto        memory  = memoryFor();
    std::string error;
    for (std::uint32_t i = 0; i < 200; ++i)
    {
        auto fetch = kTexture;
        fetch[1] += i << 12; // the base address
        for (std::size_t word = 0; word < fetch.size(); ++word)
            put(memory.At(kG, false).bytes, 0x480 + word * 4, fetch[word]);
        IndexedBufferDrawInputs   inputs;
        NativeTextureMemoryRanges expected;
        ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), readerFor(memory), shaders, inputs, error, IndexedBufferForm::Scene);
        UNSCOPED_INFO(std::format("fetch {} reads its own texture ranges", i));
        REQUIRE((GetNativeTextureMemoryRanges(fetch, expected, error) && inputs.textureRanges.size() == 1 &&
                 inputs.textureRanges[0].base.address == expected.base.address && inputs.textureRanges[0].base.size == expected.base.size &&
                 inputs.textureRanges[0].mips.address == expected.mips.address && inputs.textureRanges[0].mips.size == expected.mips.size));
    }
    std::cout << "Scene reader: 200 texture fetches each read their own memory ranges\n";
}

// Reused input storage must match fresh reads, including other draw forms.
void inputReuse()
{
    const auto catalog = fullCatalog();
    const auto shaders = ScenePixelShaders(*catalog);
    const auto viewed  = [](FakeMemory& memory)
    {
        auto read = readerFor(memory);
        read.view = [&memory](std::uint64_t address, std::uint64_t size, bool physical)
        {
            return memory.View(address, size, physical);
        };
        return read;
    };
    const auto same = [](const IndexedBufferDrawInputs& a, const IndexedBufferDrawInputs& b)
    {
        return sameInputs(a, b) && a.streams == b.streams && a.streamFetches == b.streamFetches && a.textureMipBytes == b.textureMipBytes &&
               a.textureResolved.size() == b.textureResolved.size() && a.textureRanges.size() == b.textureRanges.size() && a.vertexObject == b.vertexObject &&
               a.pixelObject == b.pixelObject && a.indexAddress == b.indexAddress && a.arguments.stride == b.arguments.stride &&
               a.vertexTextureFetch == b.vertexTextureFetch && a.vertices == b.vertices;
    };
    std::string error;
    {
        // A draw that also reads fetch 16, then a plain one, into one inputs
        // object.
        Draw textured, plain;
        textured.vertexTexture   = true;
        const auto vertexCatalog = catalogOf({ interfaceFor(textured, true, 4, true), interfaceFor(textured, false, 4, true), interfaceFor(plain, true, 4), interfaceFor(plain, false, 4) });
        auto       first = memoryFor(textured), second = memoryFor();
        for (std::size_t i = 0; i < kVertexTexture.size(); ++i)
            put(first.At(kG, false).bytes, 0x480 + 16 * 24 + i * 4, kVertexTexture[i]);
        IndexedBufferDrawInputs reused, fresh;
        UNSCOPED_INFO(std::string("the first draw reads with fetch 16: ") + (error));
        REQUIRE((ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), viewed(first), ScenePixelShaders(*vertexCatalog), reused, error, IndexedBufferForm::Scene) &&
                 reused.vertexTextureFetch && reused.textureFetches.size() == 2));
        UNSCOPED_INFO(std::string("a read into used inputs yields a fresh read's inputs: ") + (error));
        REQUIRE((ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), viewed(second), shaders, reused, error, IndexedBufferForm::Scene) &&
                 ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), viewed(second), shaders, fresh, error, IndexedBufferForm::Scene) && same(reused, fresh)));
        ReturnDrawInputs(std::move(reused));
        IndexedBufferDrawInputs spared;
        UNSCOPED_INFO(std::string("a read from returned storage yields a fresh read's inputs: ") + (error));
        REQUIRE((ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), viewed(second), shaders, spared, error, IndexedBufferForm::Scene) && same(spared, fresh)));
        auto refused = memoryFor();
        put(refused.At(kG, false).bytes, 0x2E2C, 0);
        UNSCOPED_INFO(std::string("a refused read into used inputs keeps none of the last draw's: ") + (error));
        REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), viewed(refused), shaders, spared, error, IndexedBufferForm::Scene) &&
                 refusedAs(error, "indexed_buffer_missing_declaration") && spared.streams.empty() && spared.textureFetches.empty() && indexBytes(spared).empty() &&
                 spared.state.size() == 0x3500));
    }
    {
        // The second family call sees the first call's storage, emptied.
        std::size_t capacity = 0;
        bool        emptied  = false;
        unsigned    calls    = 0;
        const auto  family   = MakeReaderDrawFamily<IndexedBufferDrawInputs>(
            [&](const NativeDrawSubmission& submission, IndexedBufferDrawInputs& inputs, std::string& detail)
            {
                if (calls++ == 0)
                {
                    inputs.vertexCode.assign(4096, 1);
                    return true;
                }
                FakeMemory none;
                ReadIndexedBufferDrawInputs(submission, readerFor(none), shaders, inputs, detail, IndexedBufferForm::Scene);
                capacity = inputs.vertexCode.capacity();
                emptied  = inputs.vertexCode.empty();
                return false;
            },
            [](const IndexedBufferDrawInputs&, NativeDrawReplayRecipe&, std::string&)
            {
                return true;
            });
        NativeDrawReplayRecipe draw;
        std::string            detail;
        UNSCOPED_INFO(std::format("the family recycles a draw's storage ({} bytes reserved)", capacity));
        REQUIRE((family(submissionFor(kSceneMeshDrawCaller), draw, detail) == NativeDrawFamilyResult::Built &&
                 family(submissionFor(0x82510BFC), draw, detail) == NativeDrawFamilyResult::NotMine && capacity >= 4096 && emptied));
    }
    {
        // Prepass reads the scene mesh caller with one pixel shader.
        auto                                          memory = memoryFor();
        const auto                                    digest = IndexedBufferMicrocodeWordDigest(psCode(Draw{}.pair));
        const std::array<IndexedBufferPixelShader, 1> none{ { { "0000", 0 } } };
        IndexedBufferDrawInputs                       other, scene, broken;
        UNSCOPED_INFO(std::string("a Prepass read of another pixel shader copies no shadow: ") + (error));
        REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), viewed(memory), std::string_view("0000"), other, error, IndexedBufferForm::Prepass) &&
                 error.empty() && other.state.empty() && other.pixelObject));
        UNSCOPED_INFO(std::string("a Scene read of another pixel shader keeps the shadow for its family: ") + (error));
        REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), viewed(memory), none, scene, error, IndexedBufferForm::Scene) && error.empty() &&
                 scene.state.size() == 0x3500));
        put(memory.At(kG, false).bytes, 0x3194, 0x7FFF0000);
        UNSCOPED_INFO(std::string("a Prepass read refused before its pixel shader matched keeps the shadow: ") + (error));
        REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), viewed(memory), std::string_view(digest), broken, error, IndexedBufferForm::Prepass) &&
                 !error.empty() && broken.state == memory.At(kG, false).bytes));
    }
    std::cout << "Scene reader: reused and recycled inputs read as fresh ones; other forms copy the shadow only when kept\n";
}

// Shader bytes stay cached only while unchanged; cache and fresh reads agree.
void shaderCache()
{
    const auto catalog = fullCatalog();
    const auto shaders = ScenePixelShaders(*catalog);
    const auto same    = sameInputs;
    const auto read    = [&](FakeMemory& memory, const std::shared_ptr<GuestShaderReadCache>& cache, IndexedBufferDrawInputs& inputs, std::span<const std::string_view> depthOnly = {}, const PatchedVertexDigest& patched = {})
    {
        std::string error;
        const bool  ok = ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), cache ? cachedReaderFor(memory, cache) : readerFor(memory), shaders, inputs, error, IndexedBufferForm::Scene, depthOnly, patched);
        UNSCOPED_INFO(std::string("the synthetic draw reads or is foreign: ") + (error));
        REQUIRE((ok || error.empty()));
        return ok;
    };
    {
        auto                    memory = memoryFor();
        const auto              cache  = std::make_shared<GuestShaderReadCache>();
        IndexedBufferDrawInputs plain, first, second;
        UNSCOPED_INFO("the uncached draw reads");
        REQUIRE(read(memory, nullptr, plain));
        memory.reads = 0;
        UNSCOPED_INFO("a first cached read yields the uncached inputs");
        REQUIRE((read(memory, cache, first) && same(first, plain)));
        const auto missReads = memory.reads;
        auto       counts    = cache->TakeCounts();
        UNSCOPED_INFO(std::format("the first read builds two entries, not {} misses and {} hits", counts.misses, counts.hits));
        REQUIRE((counts.misses == 2 && counts.hits == 0));
        memory.reads = 0;
        UNSCOPED_INFO("a cached read yields the uncached inputs");
        REQUIRE((read(memory, cache, second) && same(second, plain)));
        counts = cache->TakeCounts();
        UNSCOPED_INFO(std::format("the second read hits both entries ({}) and reads less ({} of {})", counts.hits, memory.reads, missReads));
        REQUIRE((counts.hits == 2 && memory.reads < missReads));

        // Changed vertex microcode under the same objects is read again.
        auto& vs = memory.At(kVsResource, true).bytes;
        vs[0] ^= 0xFF;
        IndexedBufferDrawInputs plainChanged, changed;
        UNSCOPED_INFO("a changed VS is read again");
        REQUIRE((read(memory, nullptr, plainChanged) && read(memory, cache, changed) && same(changed, plainChanged) && changed.vertexCode == vs));
        counts = cache->TakeCounts();
        UNSCOPED_INFO("the PS entry hits and the interface entry is stale");
        REQUIRE((counts.hits == 1 && counts.stale == 1));
        // A resolve image pending over the VS code leaves guest memory as it
        // was, but reads see the image: the entry is not reused, and the
        // image's bytes are read as uncached.
        auto overlaid = vs;
        overlaid[4] ^= 0xFF;
        memory.pending.push_back({ kVsResource, true, overlaid });
        IndexedBufferDrawInputs plainPending, pending;
        UNSCOPED_INFO("a VS under a pending resolve image is read through the image");
        REQUIRE((read(memory, nullptr, plainPending) && read(memory, cache, pending) && same(pending, plainPending) && pending.vertexCode == overlaid));
        counts = cache->TakeCounts();
        UNSCOPED_INFO("the interface entry under a pending image is stale");
        REQUIRE(counts.stale == 1);
        IndexedBufferDrawInputs stillPending;
        UNSCOPED_INFO("an entry read under the image is not reused while it is pending");
        REQUIRE((read(memory, cache, stillPending) && same(stillPending, plainPending) && cache->TakeCounts().hits == 1));
        // Once the image lands in guest memory, the entry read through it holds.
        vs = overlaid;
        memory.pending.clear();
        IndexedBufferDrawInputs landed;
        UNSCOPED_INFO("the landed image's bytes match the entry read through it");
        REQUIRE((read(memory, cache, landed) && same(landed, plainPending) && cache->TakeCounts().hits == 2));
        // Changed pixel microcode no longer matches the catalog, as uncached.
        memory.At(kPsResource, true).bytes[0] ^= 0xFF;
        IndexedBufferDrawInputs foreign;
        UNSCOPED_INFO("a changed PS is foreign through the cache too");
        REQUIRE((!read(memory, nullptr, foreign) && !read(memory, cache, foreign)));
    }
    {
        // A family returns the state shadow's storage for the next draw.
        auto                   memory = memoryFor();
        NativeDrawReplayRecipe draw;
        std::string            detail;
        TakeStateBuffer(); // drops any spare storage earlier reads left
        UNSCOPED_INFO("taking the spare leaves none");
        REQUIRE(TakeStateBuffer().capacity() == 0);
        UNSCOPED_INFO(std::string("the cached family builds the draw: ") + (detail));
        REQUIRE(MakeSceneDrawFamily(cachedReaderFor(memory, std::make_shared<GuestShaderReadCache>()), catalog)(submissionFor(kSceneMeshDrawCaller), draw, detail) ==
                NativeDrawFamilyResult::Built);
        auto spare = TakeStateBuffer();
        UNSCOPED_INFO("the family keeps the shadow storage for the next read");
        REQUIRE((spare.empty() && spare.capacity() >= 0x3500));
    }
    {
        // A depth-only draw, and one whose VS words the fetch patch lists.
        auto       memory = depthOnlyMemory();
        auto       depth  = fullCatalog();
        const auto vsCode = memory.At(kVsResource, true).bytes;
        depth->depthOnly.push_back({ depthOnlyInterface(vsCode) });
        const auto              listed        = SceneDepthOnlyVertexShaders(*depth);
        const auto              patchedDigest = depthPatched();
        const auto              cache         = std::make_shared<GuestShaderReadCache>();
        IndexedBufferDrawInputs plain, first, second;
        UNSCOPED_INFO("cached depth-only reads yield the uncached inputs");
        REQUIRE((read(memory, nullptr, plain, listed, patchedDigest) && read(memory, cache, first, listed, patchedDigest) && read(memory, cache, second, listed, patchedDigest) &&
                 same(first, plain) && same(second, plain)));
        auto wider             = plain;
        wider.arguments.stride = 16;
        std::vector<std::uint8_t> otherStride;
        std::string               error;
        UNSCOPED_INFO(std::string("a stride-16 patch: ") + (error));
        REQUIRE(PatchDepthOnlyVertexFetch(wider, otherStride, error));
        memory.At(kVsResource, true).bytes = otherStride;
        IndexedBufferDrawInputs plainPatched, patchedFirst, patchedSecond;
        UNSCOPED_INFO("cached reads of a VS the patch lists yield the uncached inputs");
        REQUIRE((read(memory, nullptr, plainPatched, listed, patchedDigest) && read(memory, cache, patchedFirst, listed, patchedDigest) &&
                 read(memory, cache, patchedSecond, listed, patchedDigest) && same(patchedFirst, plainPatched) && same(patchedSecond, plainPatched)));
        IndexedBufferDrawInputs foreign;
        UNSCOPED_INFO("without the patch digest the cached VS is still foreign");
        REQUIRE(!read(memory, cache, foreign, listed));
    }
    std::cout << "Scene shader cache: hits yield uncached inputs; changed VS or PS bytes are read again\n";
}

// The VS words of the PS-bound mesh draw in `memory` after the fold and the
// fetch patch for stream 0's `stride`, as the catalog's digest is made.
std::vector<std::uint8_t> patchedMeshWords(FakeMemory& memory, std::uint32_t stride)
{
    const auto shaders               = ScenePixelShaders(*fullCatalog());
    auto       inputs                = readScene(memory, shaders);
    inputs.arguments.stride          = stride;
    std::vector<std::uint8_t> folded = inputs.vertexCode, effective, patched;
    std::string               error;
    UNSCOPED_INFO(std::string("mesh fold: ") + (error));
    REQUIRE(FoldSolidShaderMetadata(inputs, folded, effective, error));
    SolidDrawInputs linked = inputs;
    linked.vertexCode      = folded;
    std::vector<std::uint32_t> offsets;
    for (const auto& element : meshPair().elements)
        offsets.push_back(element.offset);
    UNSCOPED_INFO(std::string("mesh patch: ") + (error));
    REQUIRE(PatchVertexFetch(linked, offsets, patched, error));
    return patched;
}

FakeMemory withVertexWords(FakeMemory memory, const std::vector<std::uint8_t>& words)
{
    memory.At(kVsResource, true).bytes = words;
    return memory;
}

// Recipes use draw state/templates; consumes skip guest fetch patching.
void hostPatchedRouting()
{
    std::string digest, error;
    const auto  hostDigest = [&](FakeMemory& memory, std::uint32_t stride)
    {
        UNSCOPED_INFO(std::string("the host-patched digest reads: ") + (error));
        REQUIRE(HostPatchedVertexDigest(kG, stride, readerFor(memory), digest, error));
        return digest;
    };

    // A PS-bound mesh draw at stride 24 whose VS holds no patch yet.
    auto       memory    = memoryFor();
    const auto asRead    = memory.At(kVsResource, true).bytes;
    const auto current   = patchedMeshWords(memory, 24);
    const auto other     = patchedMeshWords(memory, 32);
    const auto reference = interfaceFor(Draw{}, false, 1).vertexGuestWordDigest;
    UNSCOPED_INFO("the stride-24 patch is the catalog's words and the stride-32 patch other words");
    REQUIRE((current != asRead && other != current && IndexedBufferMicrocodeWordDigest(current) == reference));
    const std::array<std::pair<const char*, const std::vector<std::uint8_t>*>, 3> held{ { { "unpatched words", &asRead }, { "the patch for this state", &current }, { "the patch for another stride", &other } } };
    const auto                                                                    psDigest = IndexedBufferMicrocodeWordDigest(psCode(meshPair()));
    for (const auto& [name, words] : held)
    {
        auto guest = withVertexWords(memory, *words);
        UNSCOPED_INFO(std::format("the host-patched digest is the catalog's whatever the guest holds: {}", name));
        REQUIRE(hostDigest(guest, 24) == reference);
        NativeDrawShaderWords shaderWords;
        UNSCOPED_INFO(std::format("the draw's shader words are the same whether the guest holds {}: {}", name, error));
        REQUIRE((ReadNativeDrawShaderWords(submissionFor(kSceneMeshDrawCaller), readerFor(guest), shaderWords, error) && shaderWords.vertex == reference && shaderWords.pixel == psDigest));
        // The wrappers find stream 0's stride in their frame or arguments, not in the shadow.
        for (const auto [stride, expected] : { std::pair{ 24U, reference }, std::pair{ 32U, IndexedBufferMicrocodeWordDigest(other) } })
        {
            auto                      indexedUp = guest;
            std::vector<std::uint8_t> strideWord(4);
            put(strideWord, 0, stride);
            auto up = submissionFor(kSceneMeshDrawCaller);
            up.hook = NativeDrawHook::IndexedUp;
            indexedUp.Add(up.stack + 84, false, strideWord);
            UNSCOPED_INFO(std::format("IndexedUp digests the VS patched for the stride at stack+84 ({}) whatever the guest holds: {} {}", stride, name, error));
            REQUIRE((ReadNativeDrawShaderWords(up, readerFor(indexedUp), shaderWords, error) && shaderWords.vertex == expected));
            up.hook         = NativeDrawHook::NonindexedUp;
            up.arguments[4] = stride;
            UNSCOPED_INFO(std::format("NonindexedUp digests the VS patched for its stride argument ({}) whatever the guest holds: {} {}", stride, name, error));
            REQUIRE((ReadNativeDrawShaderWords(up, readerFor(guest), shaderWords, error) && shaderWords.vertex == expected));
        }
    }
    UNSCOPED_INFO("the draw's state decides the digest: stride 32 is the other patch");
    REQUIRE((hostDigest(memory, 32) == IndexedBufferMicrocodeWordDigest(other) && digest != reference));

    // A pair whose exports need linkage: the fold changes the words, and the digest follows it.
    {
        auto  linkedMemory = memory;
        auto& vsMetadata   = linkedMemory.At(kVs + 0x100 + 872, false).bytes;
        vsMetadata.resize(56);
        put(vsMetadata, 20, 1U << 5); // one export
        put(vsMetadata, 48, 0);       // its record: slot 0, patch list 0
        put(vsMetadata, 52, 0x1000);  // the list: code word 0, last
        auto& psMetadata = linkedMemory.At(kPs + 0x100 + 40, false).bytes;
        psMetadata.resize(36);
        put(psMetadata, 20, 0x21);  // one input, a mismatched pair
        put(psMetadata, 32, 0x300); // its record: destination register 3
        const auto expected = patchedMeshWords(linkedMemory, 24);
        UNSCOPED_INFO("linkage changes the patched words");
        REQUIRE(expected != current);
        for (const auto& [name, words] : held)
        {
            auto guest = withVertexWords(linkedMemory, *words);
            UNSCOPED_INFO(std::format("the host-patched digest includes the linkage fold: {}", name));
            REQUIRE(hostDigest(guest, 24) == IndexedBufferMicrocodeWordDigest(expected));
        }
#if REREVVED_DEVELOPER
        auto       backing = withVertexWords(linkedMemory, current);
        const auto reading = ReadVsCodeState(kG, 24, readerFor(backing));
        UNSCOPED_INFO("a correct backing fetch patch stays fresh when inline linkage changes execution identity");
        REQUIRE((reading.state == VsCodeState::Fresh && reading.hostDigest == IndexedBufferMicrocodeWordDigest(current) &&
                 reading.hostDigest != IndexedBufferMicrocodeWordDigest(expected)));
#endif
    }

#if REREVVED_DEVELOPER
    // The VS as the guest left it is the host's only when the guest patched for this state.
    for (const auto& [name, words] : held)
    {
        auto       guest   = withVertexWords(memory, *words);
        const auto reading = ReadVsCodeState(kG, 24, readerFor(guest));
        UNSCOPED_INFO(std::format("the VS code reading of {} against stride 24", name));
        REQUIRE((reading.state == (words == &current ? VsCodeState::Fresh : VsCodeState::Stale) && reading.hostDigest == reference && reading.readDigest == IndexedBufferMicrocodeWordDigest(*words) &&
                 reading.error.empty()));
    }
    {
        auto guest = withVertexWords(memory, other);
        UNSCOPED_INFO("the patch for stride 32 is fresh for a stride-32 draw and stale for a stride-24 one");
        REQUIRE((ReadVsCodeState(kG, 32, readerFor(guest)).state == VsCodeState::Fresh && ReadVsCodeState(kG, 24, readerFor(guest)).state == VsCodeState::Stale));
    }
#endif

    // A shader the guest leaves alone (no declaration) is digested as read.
    for (const auto& [name, words] : held)
    {
        auto guest = withVertexWords(memory, *words);
        put(guest.At(kG, false).bytes, 0x2E2C, 0);
        UNSCOPED_INFO(std::format("a draw without a declaration is digested as read: {}", name));
        REQUIRE(hostDigest(guest, 24) == IndexedBufferMicrocodeWordDigest(*words));
    }
    {
        // Variant one has distinct storage, metadata, and a distinct key record.
        // No fetch records: the guest leaves these variant-one words alone.
        auto variant = memory;
        put(variant.At(kG, false).bytes, 0x3194, 0);
        put(variant.At(kG, false).bytes, 0x2E2C, 0);
        put(variant.At(kVs, false).bytes, 872, 0x20);
        put(variant.At(kVs, false).bytes, 904, 0x400);
        auto code = asRead;
        code[0] ^= 1;
        std::vector<std::uint8_t> metadata(32);
        put(metadata, 0, 0x10000);
        put(metadata, 4, static_cast<std::uint32_t>(code.size()));
        variant.Add(kVs + 872 + 0x400, false, metadata);
        variant.Add(kVsResource + 0x10000, true, code);
#if REREVVED_DEVELOPER
        const auto reading = ReadVsCodeState(kG, 24, readerFor(variant));
        UNSCOPED_INFO("the watch compares variant-one backing with variant-one expected words");
        REQUIRE((reading.state == VsCodeState::Fresh && reading.readDigest == IndexedBufferMicrocodeWordDigest(code) && reading.readDigest != IndexedBufferMicrocodeWordDigest(asRead)));
#endif
        UNSCOPED_INFO("execution identity selects the same active variant");
        REQUIRE(hostDigest(variant, 24) == IndexedBufferMicrocodeWordDigest(code));
    }

    // Named refusals.
    const auto refused = [&](FakeMemory& source, const NativeDrawSubmission& submission, const std::string& expected)
    {
        NativeDrawShaderWords shaderWords{ "x", "y" };
        UNSCOPED_INFO(std::format("expected {}, got {}", expected, error));
        REQUIRE((!ReadNativeDrawShaderWords(submission, readerFor(source), shaderWords, error) && error == expected && shaderWords.vertex.empty() && shaderWords.pixel.empty()));
    };
    {
        FakeMemory nothing;
        refused(nothing, submissionFor(kSceneMeshDrawCaller), "shader_objects_unreadable");
        FakeMemory                objects;
        std::vector<std::uint8_t> pair(8);
        put(pair, 0, kPs);
        put(pair, 4, kVs);
        objects.Add(kG + 0x3194, false, pair);
        refused(objects, submissionFor(kSceneMeshDrawCaller), "shader_stride_unreadable");
        auto upBare = submissionFor(kSceneMeshDrawCaller);
        upBare.hook = NativeDrawHook::IndexedUp;
        refused(objects, upBare, "shader_stride_unreadable");
        upBare.hook = NativeDrawHook::NonindexedUp;
        refused(objects, upBare, "indexed_buffer_shadow_extent");
        auto noPixel = memory, noVertex = memory;
        put(noPixel.At(kG, false).bytes, 0x3194, 0);
        put(noVertex.At(kG, false).bytes, 0x3198, 0);
        refused(noPixel, submissionFor(kSceneMeshDrawCaller), "shader_object_unset");
        refused(noVertex, submissionFor(kSceneMeshDrawCaller), "shader_object_unset");
        auto noCode = memory;
        std::erase_if(noCode.regions, [](const FakeMemory::Region& region)
                      {
                          return region.address == kVsResource;
                      });
        NativeDrawShaderWords shaderWords;
        UNSCOPED_INFO("a VS whose code is unreadable is refused");
        REQUIRE((!ReadNativeDrawShaderWords(submissionFor(kSceneMeshDrawCaller), readerFor(noCode), shaderWords, error) && !error.empty() && shaderWords.vertex.empty()));
#if REREVVED_DEVELOPER
        const auto reading = ReadVsCodeState(kG, 24, readerFor(noCode));
        UNSCOPED_INFO("an unreadable VS is neither fresh nor stale");
        REQUIRE((reading.state == VsCodeState::Unreadable && !reading.error.empty()));
        const auto unset = ReadVsCodeState(kG, 24, readerFor(noVertex));
        UNSCOPED_INFO("an unset VS is named");
        REQUIRE((unset.state == VsCodeState::Unreadable && unset.error == "shader_object_unset"));
        UNSCOPED_INFO("an unreadable shadow is named");
        REQUIRE(ReadVsCodeState(kG, 24, readerFor(nothing)).error == "shader_object_unreadable");
#endif
    }

    // A depth-only draw: no pixel shader, so the VS is digested unlinked; the listed words are the patch for stride 12.
    auto                            depth   = depthOnlyMemory();
    const std::vector<std::uint8_t> patched = depth.At(kVsResource, true).bytes;
    IndexedBufferDrawInputs         inputs;
    UNSCOPED_INFO(std::string("the depth-only inputs read: ") + (error));
    REQUIRE(ReadDrawShaderInputs(kG, readerFor(depth), inputs, error));
    inputs.arguments.stride = 16;
    std::vector<std::uint8_t> wider;
    UNSCOPED_INFO(std::string("a stride-16 patch writes other words: ") + (error));
    REQUIRE((PatchDepthOnlyVertexFetch(inputs, wider, error) && wider != patched));
    std::vector<std::uint8_t> templates = patched;
    for (std::size_t i = 0; i * 4 < inputs.fetchRecords.size(); ++i)
        for (std::size_t j = 0; j < 3; ++j)
            store(templates, (word(inputs.fetchRecords, i * 4) & 0xFFF) * 12 + j * 4, word(inputs.fetchTemplates, i * 12 + j * 4));
    UNSCOPED_INFO("the fetch templates differ from the patched words");
    REQUIRE(templates != patched);
    for (const auto& [name, words] : { std::pair<const char*, const std::vector<std::uint8_t>*>{ "the patch for this state", &patched }, std::pair<const char*, const std::vector<std::uint8_t>*>{ "the fetch templates", &templates }, std::pair<const char*, const std::vector<std::uint8_t>*>{ "the patch for another stride", &wider } })
    {
        auto guest = withVertexWords(depth, *words);
        UNSCOPED_INFO(std::format("a depth-only draw's host-patched digest follows its state, not the words in guest memory: {}", name));
        REQUIRE(hostDigest(guest, 12) == IndexedBufferMicrocodeWordDigest(patched));
        UNSCOPED_INFO(std::format("and a stride-16 state gives the stride-16 words: {}", name));
        REQUIRE(hostDigest(guest, 16) == IndexedBufferMicrocodeWordDigest(wider));
    }
    std::cout << "Host-patched VS routing: digests, shader words and code readings follow the draw's state, not the VS words in guest memory\n";
}

// Address reuse must invalidate dependencies even when generated code matches.
void identityCache()
{
    const auto catalog = fullCatalog();
    auto       memory  = memoryFor();
    auto       base    = readScene(memory, ScenePixelShaders(*catalog));
    // A lifetime stamp unique to this fixture keeps earlier unit cases from
    // pre-populating the individual invalidation entries.
    base.vertexHeader[4] = 0xA5;
    const auto offsets   = catalog->entries.front().attributeOffsets;
    const auto reference = [](const SolidDrawInputs& in, std::span<const std::uint32_t> admitted, bool linked)
    {
        VertexShaderIdentity      result;
        auto                      source = in.vertexCode;
        std::vector<std::uint8_t> ignored;
        if (linked && in.pixelObject && !FoldSolidShaderMetadata(in, source, ignored, result.error, true))
            return result;
        result.ok = PatchVertexFetch(in, source, admitted, result.code, result.error);
        if (result.ok)
            result.digest = IndexedBufferMicrocodeWordDigest(result.code);
        return result;
    };
    const auto equal = [&](const SolidDrawInputs& in, std::span<const std::uint32_t> admitted, bool linked, const VertexShaderIdentity& got)
    {
        const auto expected = reference(in, admitted, linked);
        UNSCOPED_INFO(std::string("memoized identity equals the direct fold and patch: ") + (got.error));
        REQUIRE((got.ok == expected.ok && got.code == expected.code && got.digest == expected.digest && got.error == expected.error));
    };
    const auto first = MemoizedVertexShaderIdentity(base, offsets, true);
    UNSCOPED_INFO(std::string("the identity cache fixture patches: ") + (first->error));
    REQUIRE(first->ok);
    TakeVertexShaderIdentityCounts();
    equal(base, offsets, true, *first);
    UNSCOPED_INFO("one direct PatchVertexFetch invocation is counted once");
    REQUIRE(TakeVertexShaderIdentityCounts().patches == 1);
    const auto again  = MemoizedVertexShaderIdentity(base, offsets, true);
    auto       counts = TakeVertexShaderIdentityCounts();
    UNSCOPED_INFO("a repeated draw reuses code and digest without another patch or guest copy");
    REQUIRE((again == first && counts.hits == 1 && counts.misses == 0 && counts.patches == 0 && counts.guestBytes == 0));

    using Edit = std::function<void(IndexedBufferDrawInputs&)>;
    const std::vector<std::pair<const char*, Edit>> edits{
        { "VS lifetime header", [](auto& in)
          {
              in.vertexHeader[0] ^= 1;
          } },
        { "VS object", [](auto& in)
          {
              ++in.vertexObject;
          } },
        { "PS object", [](auto& in)
          {
              ++in.pixelObject;
          } },
        { "declaration object", [](auto& in)
          {
              ++in.declarationAddress;
          } },
        { "PS header", [](auto& in)
          {
              in.pixelHeader[0] ^= 1;
          } },
        { "declaration header", [](auto& in)
          {
              in.declarationHeader[0] ^= 1;
          } },
        { "variant", [](auto& in)
          {
              in.variant = 1;
          } },
        { "effective stride bytes", [](auto& in)
          {
              in.arguments.stride += 4;
          } },
        { "same-address VS replacement", [](auto& in)
          {
              in.vertexCode[0] ^= 1;
          } },
        { "VS metadata", [](auto& in)
          {
              in.vertexMetadata[8] ^= 1;
          } },
        { "PS linkage metadata", [](auto& in)
          {
              in.pixelMetadata[20] ^= 1;
          } },
        { "fetch records", [](auto& in)
          {
              in.fetchRecords[0] ^= 1;
          } },
        { "fetch templates", [](auto& in)
          {
              in.fetchTemplates[0] ^= 1;
          } },
        { "declaration semantics", [](auto& in)
          {
              in.declaration[9] ^= 1;
          } },
        { "declaration types", [](auto& in)
          {
              in.declaration[7] ^= 1;
          } },
        { "declaration offsets", [](auto& in)
          {
              in.declaration[3] ^= 4;
          } },
        { "swizzle table", [](auto& in)
          {
              in.swizzleTable[0] ^= 1;
          } },
        { "format table", [](auto& in)
          {
              in.formatTable[0] ^= 1;
          } },
        { "invalid draw state extent", [](auto& in)
          {
              in.state.clear();
          } },
    };
    for (const auto& [name, edit] : edits)
    {
        // Warm immediately before each edit; direct-mapped collisions may evict.
        MemoizedVertexShaderIdentity(base, offsets, true);
        auto changed = base;
        edit(changed);
        TakeVertexShaderIdentityCounts();
        const auto result = MemoizedVertexShaderIdentity(changed, offsets, true);
        counts            = TakeVertexShaderIdentityCounts();
        UNSCOPED_INFO(std::string("identity invalidation: ") + name);
        REQUIRE((counts.misses == 1 && counts.hits == 0));
        equal(changed, offsets, true, *result);
    }
    for (const bool linked : { false, true })
    {
        MemoizedVertexShaderIdentity(base, offsets, linked);
        auto restricted = offsets;
        restricted.pop_back();
        TakeVertexShaderIdentityCounts();
        const auto result = MemoizedVertexShaderIdentity(base, restricted, linked);
        UNSCOPED_INFO("candidate admission offsets participate in the identity key");
        REQUIRE(TakeVertexShaderIdentityCounts().misses == 1);
        equal(base, restricted, linked, *result);
        UNSCOPED_INFO("cached code cannot bypass candidate offset restrictions");
        REQUIRE((!result->ok && result->error == "adapter_attribute_offset"));
    }
    {
        auto       link    = base;
        const auto records = (word(link.vertexMetadata, 24) + word(link.vertexMetadata, 28) + 9) * 4;
        link.vertexMetadata.resize(records + 8);
        store(link.vertexMetadata, 20, 1U << 5);
        store(link.vertexMetadata, records, 0);
        store(link.vertexMetadata, records + 4, 0x1000);
        link.pixelMetadata.resize(36);
        store(link.pixelMetadata, 20, 0x21);
        store(link.pixelMetadata, 32, 0x300);
        const auto linked  = MemoizedVertexShaderIdentity(link, offsets, true);
        const auto backing = MemoizedVertexShaderIdentity(link, offsets, false);
        UNSCOPED_INFO("linked identity is separate from expected backing identity");
        REQUIRE((linked->ok && backing->ok && linked->digest != backing->digest));
        equal(link, offsets, true, *linked);
        for (const bool pixel : { false, true })
        {
            MemoizedVertexShaderIdentity(link, offsets, true);
            auto changed = link;
            if (pixel)
                changed.pixelMetadata[35] ^= 1;
            else
                changed.vertexMetadata[records + 7] ^= 1;
            TakeVertexShaderIdentityCounts();
            const auto result = MemoizedVertexShaderIdentity(changed, offsets, true);
            UNSCOPED_INFO("full linkage records and patch lists invalidate independently");
            REQUIRE(TakeVertexShaderIdentityCounts().misses == 1);
            equal(changed, offsets, true, *result);
        }
    }

    // A shader hit never supplies an earlier draw's constants or register words.
    auto current = base;
    current.state[0x780 + 3] ^= 1;
    current.state[shadowOf(0x210E) + 3] ^= 1;
    MemoizedVertexShaderIdentity(base, offsets, true);
    TakeVertexShaderIdentityCounts();
    SceneDrawRecipe recipe;
    std::string     error;
    UNSCOPED_INFO(std::string("a draw with current constants builds on a shader hit: ") + (error));
    REQUIRE(BuildSceneDrawRecipe(current, *catalog, recipe, error));
    UNSCOPED_INFO("current constants and registers survive the shader-cache hit");
    REQUIRE((TakeVertexShaderIdentityCounts().hits > 0 && recipe.effectiveState[0x780 + 3] == current.state[0x780 + 3] &&
             word(recipe.effectiveState, shadowOf(0x210E)) == word(current.state, shadowOf(0x210E))));
    {
        auto depthMemory  = depthOnlyMemory();
        auto depthCatalog = fullCatalog();
        depthCatalog->depthOnly.push_back({ depthOnlyInterface(depthMemory.At(kVsResource, true).bytes) });
        auto                   family = MakeSceneDrawFamily(cachedReaderFor(depthMemory, std::make_shared<GuestShaderReadCache>()), depthCatalog);
        NativeDrawReplayRecipe draw;
        UNSCOPED_INFO(std::string("the depth-only family warms: ") + (error));
        REQUIRE(family(submissionFor(kSceneMeshDrawCaller), draw, error) == NativeDrawFamilyResult::Built);
        TakeVertexShaderIdentityCounts();
        UNSCOPED_INFO(std::string("the repeated depth-only family builds: ") + (error));
        REQUIRE(family(submissionFor(kSceneMeshDrawCaller), draw, error) == NativeDrawFamilyResult::Built);
        counts = TakeVertexShaderIdentityCounts();
        UNSCOPED_INFO("qualification and routing share the same shader result: two hits, zero patches or identity bytes");
        REQUIRE((counts.hits == 2 && counts.misses == 0 && counts.patches == 0 && counts.guestBytes == 0));
    }

    // Content validation consults the existing guest read cache, without a
    // full device copy or any PS code read, and catches in-place replacements.
    const auto cache               = std::make_shared<GuestShaderReadCache>();
    memory.At(kVs, false).bytes[4] = 0xA5;
    auto       read                = cachedReaderFor(memory, cache);
    const auto inner               = static_cast<const GuestMemoryRead&>(read);
    const auto innerView           = read.view;
    read.view                      = [innerView](std::uint64_t address, std::uint64_t size, bool physical)
    {
        UNSCOPED_INFO("identity never views PS code or the complete device shadow");
        REQUIRE((!(physical && address == kPsResource) && !(address == kG && size == 0x3500)));
        return innerView(address, size, physical);
    };
    static_cast<GuestMemoryRead&>(read) = [inner](std::uint64_t address, std::uint64_t size, bool physical)
    {
        UNSCOPED_INFO("identity never reads PS code or the complete device shadow");
        REQUIRE((!(physical && address == kPsResource) && !(address == kG && size == 0x3500)));
        return inner(address, size, physical);
    };
    std::string digest;
    TakeVertexShaderIdentityCounts();
    UNSCOPED_INFO(std::string("the identity reader warms: ") + (error));
    REQUIRE(HostPatchedVertexDigest(kG, 24, read, digest, error));
    UNSCOPED_INFO("cold shader identity payload copies are counted");
    REQUIRE(TakeVertexShaderIdentityCounts().guestBytes > 0);
    UNSCOPED_INFO(std::string("the repeated identity reader returns the same digest: ") + (error));
    REQUIRE((HostPatchedVertexDigest(kG, 24, read, digest, error) && digest == first->digest));
    counts = TakeVertexShaderIdentityCounts();
    UNSCOPED_INFO("a validated repeated identity read copies zero guest bytes and patches zero times");
    REQUIRE((counts.hits == 1 && counts.misses == 0 && counts.patches == 0 && counts.guestBytes == 0));
    NativeDrawShaderWords shaderWords;
    UNSCOPED_INFO(std::string("the ended-set identity query reuses the same VS result: ") + (error));
    REQUIRE((ReadNativeDrawShaderWords(submissionFor(kSceneMeshDrawCaller), cachedReaderFor(memory, cache), shaderWords, error) && shaderWords.vertex == digest));
    counts = TakeVertexShaderIdentityCounts();
    UNSCOPED_INFO("the native query counts only its bindings, stride, and separately required PS digest reads on a VS hit");
    REQUIRE((counts.hits == 1 && counts.misses == 0 && counts.patches == 0 && counts.guestBytes == 8 + 4 + 68 + 32 + psCode(meshPair()).size()));
    memory.At(kVsResource, true).bytes[0] ^= 1;
    auto        uncached = readerFor(memory);
    std::string expected;
    UNSCOPED_INFO(std::string("same-address guest code replacement invalidates the content journal and patched result: ") + (error));
    REQUIRE((HostPatchedVertexDigest(kG, 24, uncached, expected, error) && HostPatchedVertexDigest(kG, 24, read, digest, error) && digest == expected && digest != first->digest));
    std::cout << "VS identity cache: repeated draws reuse code and digest; every patch dependency invalidates; current draw state survives hits\n";
}

#if REREVVED_DEVELOPER
// Consumed dirty-fetch observations use the entry stride and final guest bytes.
void vsCodeWatch()
{
    auto       memory  = memoryFor();
    const auto asRead  = memory.At(kVsResource, true).bytes;
    const auto current = patchedMeshWords(memory, 24);
    const auto bytes   = [&](const std::vector<std::uint8_t>& words)
    {
        memory.At(kVsResource, true).bytes = words;
    };
    const auto consume = [](std::uint32_t graphics, const EntryStride* stride = nullptr)
    {
        NoteVertexFetchConsume({ nullptr, graphics, stride });
    };
    const bool saved = REXCVAR_GET(native_vs_code_watch);

    // Off, nothing is installed and nothing is counted.
    REXCVAR_SET(native_vs_code_watch, false);
    auto       diagnosticReads = std::make_shared<unsigned>(0);
    const auto diagnosticRead  = [inner = readerFor(memory), diagnosticReads](std::uint64_t address, std::uint64_t size, bool physical)
    {
        ++*diagnosticReads;
        return inner(address, size, physical);
    };
    InstallVsCodeWatch(diagnosticRead);
    TakeVertexShaderIdentityCounts();
    consume(kG);
    const auto off         = TakeVsCodeWatchCounts();
    const auto offIdentity = TakeVertexShaderIdentityCounts();
    UNSCOPED_INFO("without the cvar the watch performs zero reads, identity work, or counting");
    REQUIRE((off.consumed == 0 && off.bytes == 0 && *diagnosticReads == 0 && offIdentity.hits == 0 && offIdentity.misses == 0 && offIdentity.patches == 0 && offIdentity.guestBytes == 0));

    REXCVAR_SET(native_vs_code_watch, true);
    InstallVsCodeWatch(readerFor(memory));
    bytes(asRead);
    consume(kG);
    auto counts = TakeVsCodeWatchCounts();
    UNSCOPED_INFO("unpatched words under an IndexedBuffer consume are stale and diagnostic bytes counted");
    REQUIRE((counts.consumed == 1 && counts.stale == 1 && counts.fresh == 0 && counts.unreadable == 0 && counts.staleShaders == 1 && counts.bytes > 0));
    bytes(current);
    consume(kG);
    counts = TakeVsCodeWatchCounts();
    UNSCOPED_INFO("the words patched for the entry's stride (byte G+0x30F0) are fresh");
    REQUIRE((counts.consumed == 1 && counts.stale == 0 && counts.fresh == 1 && counts.staleShaders == 1));
    // A wrapper supplies its own stride, in a register or in its frame.
    const EntryStride registerStride{ false, 32 };
    consume(kG, &registerStride);
    std::vector<std::uint8_t> frame(4);
    put(frame, 0, 24);
    memory.Add(0x7018F9A0, false, frame);
    const EntryStride frameStride{ true, 0x7018F9A0 };
    consume(kG, &frameStride);
    counts = TakeVsCodeWatchCounts();
    UNSCOPED_INFO("a wrapper's stride decides, from its register or its frame");
    REQUIRE((counts.consumed == 2 && counts.stale == 1 && counts.fresh == 1 && counts.staleShaders == 1));
    const EntryStride missing{ true, 0x7018FA00 };
    consume(kG, &missing);
    consume(0x10);
    counts = TakeVsCodeWatchCounts();
    UNSCOPED_INFO("an unreadable stride or shadow counts as unreadable");
    REQUIRE((counts.consumed == 2 && counts.unreadable == 2 && counts.stale == 0 && counts.fresh == 0));
    UNSCOPED_INFO("the watch counts format");
    REQUIRE(FormatVsCodeWatchCounts({ 5, 4, 3, 2, 1, 7, 6, 9, 123 }) == " vs_watch_consumed=5 vs_watch_stale=4 vs_watch_fresh=3 vs_watch_unreadable=2 vs_watch_stale_shaders=1 vs_watch_key_cache_stale=7 vs_watch_key_record_stale=6 vs_watch_key_unreadable=9 vs_watch_bytes=123");

    // A read that fails after the first has answered still counts the draw once, as unreadable.
    for (int failing = 1; failing <= 3; ++failing)
    {
        bytes(asRead);
        auto inner = readerFor(memory);
        auto seen  = std::make_shared<int>(0);
        InstallVsCodeWatch([=](std::uint64_t address, std::uint64_t size, bool physical)
                           {
                               if (address == kG + 0x3198 && size == 4 && ++*seen == failing)
                                   throw std::runtime_error("memory gone");
                               return inner(address, size, physical);
                           });
        consume(kG);
        counts = TakeVsCodeWatchCounts();
        UNSCOPED_INFO(std::format("a failing read #{} counts the draw once", failing));
        REQUIRE((counts.consumed == 1 && counts.stale + counts.fresh + counts.unreadable == 1));
        // The second is the key cache's: the words still decide, the key is named unreadable.
        if (failing == 2)
        {
            UNSCOPED_INFO("an unreadable key cache leaves the words counted and the key unreadable");
            REQUIRE((counts.stale == 1 && counts.keyUnreadable == 1 && counts.keyCacheStale == 0 && counts.keyRecordStale == 0));
        }
        // The third read of the shader object is the stale draw's: it fails the whole outcome.
        if (failing == 3)
        {
            UNSCOPED_INFO("a failing read after the words were compared is unreadable, not stale");
            REQUIRE((counts.unreadable == 1 && counts.stale == 0 && counts.staleShaders == 0));
        }
    }
    InstallVsCodeWatch(readerFor(memory));

    // The key cache the skipped patch would have stored: G+0x2E38..0x2E47 and the variant record's key.
    {
        bytes(current);
        auto&                     state = memory.At(kG, false).bytes;
        auto&                     vs    = memory.At(kVs, false).bytes;
        std::vector<std::uint8_t> key(state.begin() + 0x30F0, state.begin() + 0x3100);
        UNSCOPED_INFO("the fixture's stream stride is six words");
        REQUIRE(key[0] == 6);
        const auto setKey = [&](std::vector<std::uint8_t>& target, std::size_t offset, std::uint8_t first)
        {
            auto copy = key;
            copy[0]   = first;
            std::ranges::copy(copy, target.begin() + offset);
        };
        const auto take = [&](const EntryStride* stride)
        {
            consume(kG, stride);
            return TakeVsCodeWatchCounts();
        };
        auto keys = take(nullptr);
        UNSCOPED_INFO("a cache and a record that hold other keys are both stale");
        REQUIRE((keys.consumed == 1 && keys.keyCacheStale == 1 && keys.keyRecordStale == 1 && keys.keyUnreadable == 0));
        setKey(state, 0x2E38, 6);
        keys = take(nullptr);
        UNSCOPED_INFO("a cache holding the key is current, the record is not");
        REQUIRE((keys.keyCacheStale == 0 && keys.keyRecordStale == 1));
        setKey(vs, 48, 6);
        keys = take(nullptr);
        UNSCOPED_INFO("the key in both leaves the draw fully current");
        REQUIRE((keys.keyCacheStale == 0 && keys.keyRecordStale == 0 && keys.fresh == 1));
        // A wrapper's nested call stores its own stride dwords first.
        const EntryStride wider{ false, 32 };
        keys = take(&wider);
        UNSCOPED_INFO("a wrapper's stride changes the key's first byte");
        REQUIRE((keys.keyCacheStale == 1 && keys.keyRecordStale == 1));
        setKey(state, 0x2E38, 8);
        setKey(vs, 48, 8);
        keys = take(&wider);
        UNSCOPED_INFO("the wrapper's key matches when both hold it");
        REQUIRE((keys.keyCacheStale == 0 && keys.keyRecordStale == 0));
        // The key's other bytes count too.
        state[0x2E38 + 9] ^= 1;
        keys = take(&wider);
        UNSCOPED_INFO("a differing byte past the first stales only that copy");
        REQUIRE((keys.keyCacheStale == 1 && keys.keyRecordStale == 0));
        state[0x2E38 + 9] ^= 1;
        // Without a pixel shader and with header bit 0x20 the record is variant one's, 416 bytes on.
        auto variantMemory = memory;
        put(variantMemory.At(kG, false).bytes, 0x3194, 0);
        put(variantMemory.At(kVs, false).bytes, 872, word(variantMemory.At(kVs, false).bytes, 872) | 0x20);
        UNSCOPED_INFO("variant one's record is not variant zero's");
        REQUIRE(ReadVsKeyState(kG, 24, readerFor(variantMemory)).recordDiffers);
        setKey(variantMemory.At(kVs, false).bytes, 48 + 416, 6);
        setKey(variantMemory.At(kG, false).bytes, 0x2E38, 6);
        const auto variantKeys = ReadVsKeyState(kG, 24, readerFor(variantMemory));
        UNSCOPED_INFO("variant one's record decides without a pixel shader");
        REQUIRE((variantKeys.readable && !variantKeys.recordDiffers && !variantKeys.cacheDiffers));
        UNSCOPED_INFO("an unreadable shadow has no key reading");
        REQUIRE(!ReadVsKeyState(0x10, 24, readerFor(variantMemory)).readable);
    }

    RemoveVsCodeWatch();
    consume(kG);
    UNSCOPED_INFO("a removed watch counts nothing");
    REQUIRE(TakeVsCodeWatchCounts().consumed == 0);
    REXCVAR_SET(native_vs_code_watch, saved);
    std::cout << "VS code watch: consumed dirty vertex fetch draws count stale and fresh VS words under the entry's stride\n";
}
#endif

// Pending shader ranges block reuse; available readback enters the cache.
void resolvedShaderCache()
{
    // Inside the resolve below, which starts at the PS microcode.
    constexpr std::uint32_t kLiteral = kPsResource + 0x80000;
    const auto              catalog  = fullCatalog();
    const auto              shaders  = ScenePixelShaders(*catalog);
    auto                    memory   = memoryFor();
    // One VS literal record, c0..c3 from kLiteral (header word 892).
    put(memory.At(kVs, false).bytes, 892, 0x200);
    std::vector<std::uint8_t> table(28), literals(64);
    put(table, 16, 8);
    put(table, 20, 16);
    put(table, 24, kLiteral - kVsResource);
    memory.Add(kVs + 0x200 + 872, false, table);
    for (std::uint32_t i = 0; i < 16; ++i)
        put(literals, i * 4, 0x3F800000U + i);
    memory.Add(kLiteral, true, literals);

    // An assembler with the resolve, over its own off-screen target memory.
    Draw targetDraw;
    targetDraw.form               = kSceneTargetForms[3];
    targetDraw.color              = 0x87000004;
    targetDraw.blend              = 0x00010001;
    auto             targetMemory = memoryFor(targetDraw);
    OffscreenSession session(targetMemory, catalog);
    const auto*      frames = session.assembler.get();
    // Physical reads go through ReadResolved and views through the title's
    // resolved view, as the title wires them.
    const auto reader = [&](std::shared_ptr<GuestShaderReadCache> cache)
    {
        ReadMemory read = [&memory, &session](std::uint64_t address, std::uint64_t size, bool physical) -> GuestBytes
        {
            if (!physical)
                return memory.Read(address, size, false);
            GuestBytes  bytes;
            std::string error;
            if (!session.assembler->ReadResolved(static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(size), [&]
                                                 {
                                                     return memory.View(address, size, true);
                                                 },
                                                 bytes,
                                                 error))
                return GuestBytes{};
            return bytes;
        };
        read.view        = MakeResolvedGuestView([frames]
                                                 {
                                              return frames;
                                                 },
                                                 [&memory](std::uint64_t address, std::uint64_t size, bool physical)
                                                 {
                                              return memory.View(address, size, physical);
                                                 });
        read.shaderCache = std::move(cache);
        return read;
    };
    const auto cache = std::make_shared<GuestShaderReadCache>();
    const auto read  = [&](const std::shared_ptr<GuestShaderReadCache>& with, IndexedBufferDrawInputs& inputs)
    {
        std::string error;
        return ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), reader(with), shaders, inputs, error, IndexedBufferForm::Scene);
    };
    const auto registers = [](const IndexedBufferDrawInputs& inputs)
    {
        std::vector<std::uint32_t> values;
        for (const auto& [reg, value] : inputs.vertexLiteralRegisters)
            values.push_back(value);
        return values;
    };
    IndexedBufferDrawInputs plain, first;
    UNSCOPED_INFO("the draw and its VS literals read before the resolve");
    REQUIRE((read(nullptr, plain) && read(cache, first) && sameInputs(first, plain) && registers(first).size() == 16 && registers(first)[0] == 0x3F800000U));
    cache->TakeCounts();

    auto        target = session.Clear();
    std::string error;
    session.Draws(2);
    const auto mark = session.Resolve(target, kPsResource, 0x01000300, error);
    UNSCOPED_INFO("the resolve's unwritten image covers the PS microcode and the literals: " + error);
    REQUIRE((mark != 0 && frames->ProbeResolved(kPsResource, 4).overlapped && frames->ProbeResolved(kLiteral, 64).overlapped));

    // No readback: the resolved reads fail, cached as uncached, and keep nothing.
    IndexedBufferDrawInputs failed;
    UNSCOPED_INFO("without its readback the draw under the image does not read, cached or not");
    REQUIRE((!read(nullptr, failed) && !read(cache, failed)));
    auto counts = cache->TakeCounts();
    UNSCOPED_INFO(std::format("no entry is reused under the image ({} hits)", counts.hits));
    REQUIRE(counts.hits == 0);
    UNSCOPED_INFO("a failed resolved read kept no entry to reuse");
    REQUIRE((!read(cache, failed) && cache->TakeCounts().hits == 0));

    // The readback: the PS microcode as memory holds it, new literals.
    GuestResolveImage image;
    image.physical = kPsResource;
    image.bytes.assign(kLiteral + 64 - kPsResource, 0);
    const auto& ps = memory.At(kPsResource, true).bytes;
    std::ranges::copy(ps, image.bytes.begin());
    for (std::uint32_t i = 0; i < 16; ++i)
        put(image.bytes, kLiteral - kPsResource + i * 4, 0x40000000U + i);
    image.runs                     = { { 0, static_cast<std::uint32_t>(image.bytes.size()) } };
    session.completion.peeks[mark] = completion::GuestImageTake{ image, {}, false };
    IndexedBufferDrawInputs plainImage, cachedImage;
    UNSCOPED_INFO("with its readback the cached draw reads the image's literals, as uncached");
    REQUIRE((read(nullptr, plainImage) && read(cache, cachedImage) && sameInputs(cachedImage, plainImage) && registers(cachedImage)[0] == 0x40000000U));
    UNSCOPED_INFO("entries read through the pending image are still not reused");
    REQUIRE(cache->TakeCounts().hits == 0);

    // Discarded, the image never lands: guest memory serves the reads again.
    session.assembler->DiscardGuestImagesThrough(mark);
    IndexedBufferDrawInputs plainAfter, cachedAfter;
    UNSCOPED_INFO("after the discard the cached draw reads guest memory's literals");
    REQUIRE((read(nullptr, plainAfter) && read(cache, cachedAfter) && sameInputs(cachedAfter, plainAfter) && registers(cachedAfter)[0] == 0x3F800000U));
    std::cout << "Scene shader cache over a resolve: no reuse under a pending image, no entry from a failed read, image bytes once read back\n";
}

// Identity uses snapshot ranges; GPU-lifetime bytes stay live.
void snapshotReads()
{
    const auto catalog    = fullCatalog();
    const auto shaders    = ScenePixelShaders(*catalog);
    auto       memory     = memoryFor();
    const auto submission = submissionFor(kSceneMeshDrawCaller);
    const auto reader     = SnapshotReads(cachedReaderFor(memory, std::make_shared<GuestShaderReadCache>()));
    const auto read       = [&](IndexedBufferDrawInputs& inputs)
    {
        std::string error;
        return ReadIndexedBufferDrawInputs(submission, reader, shaders, inputs, error, IndexedBufferForm::Scene);
    };
    const GuestMemoryView view = [&memory](std::uint64_t address, std::uint64_t size, bool physical)
    {
        return memory.View(address, size, physical);
    };
    GuestReadTemplates templates;
    GuestReadSnapshot  snapshot;
    UNSCOPED_INFO("a draw no build has read is not captured");
    REQUIRE(!templates.Capture(submission, view, snapshot));

    // Learning: a live read and family build note their virtual reads, the
    // shadow among them.
    const auto family = MakeSceneDrawFamily(reader, catalog);
    const auto built  = [&](NativeDrawReplayRecipe& draw)
    {
        std::string detail;
        return family(submission, draw, detail) == NativeDrawFamilyResult::Built;
    };
    const auto sameDraw = [](const NativeDrawReplayRecipe& a, const NativeDrawReplayRecipe& b)
    {
        return std::ranges::equal(a.indexSpan(), b.indexSpan()) && a.vertexConstants == b.vertexConstants && a.pixelConstants == b.pixelConstants && std::ranges::equal(a.vertexBytes(), b.vertexBytes()) &&
               a.width == b.width && a.height == b.height && a.vertexShaderHash == b.vertexShaderHash && a.pixelShaderHash == b.pixelShaderHash;
    };
    IndexedBufferDrawInputs live;
    NativeDrawReplayRecipe  liveDraw;
    GuestReadSet            reads;
    {
        GuestReadSnapshotScope scope(nullptr, &reads);
        UNSCOPED_INFO("the live draw reads and builds");
        REQUIRE((read(live) && built(liveDraw)));
    }
    UNSCOPED_INFO("the build read the device shadow");
    REQUIRE(std::ranges::find(reads, std::pair<std::uint32_t, std::uint32_t>{ kG, 0x3500 }) != reads.end());
    templates.Learn(submission, reads);
    UNSCOPED_INFO("the capture holds the shadow and the declaration, not the physical microcode");
    REQUIRE((templates.Capture(submission, view, snapshot) && snapshot.Find(kG, 0x3500).size() == 0x3500 && snapshot.Find(kDeclaration, 52).size() == 52 &&
             snapshot.Find(kVsResource, 4).empty()));
    const auto held = snapshot.Bytes();
    templates.Learn(submission, reads);
    GuestReadSnapshot again;
    UNSCOPED_INFO("learning the same reads again changes nothing");
    REQUIRE((templates.Capture(submission, view, again) && again.Bytes() == held));

    // The guest moves on: a new VS constant and an emptied declaration.
    put(memory.At(kG, false).bytes, 0x780, 0x12345678);
    put(memory.At(kDeclaration, false).bytes, 24, 0);
    IndexedBufferDrawInputs moved;
    UNSCOPED_INFO("guest memory no longer reads as the captured draw");
    REQUIRE((!read(moved) || !sameInputs(moved, live)));
    IndexedBufferDrawInputs deferred;
    {
        GuestReadSnapshotScope scope(&snapshot, nullptr);
        UNSCOPED_INFO("under the snapshot the draw reads as it was captured, with no late read");
        REQUIRE((read(deferred) && sameInputs(deferred, live) && scope.Late() == 0));
        NativeDrawReplayRecipe deferredDraw;
        UNSCOPED_INFO("and the family builds it as captured");
        REQUIRE((built(deferredDraw) && sameDraw(deferredDraw, liveDraw) && scope.Late() == 0));
    }
    NativeDrawReplayRecipe movedDraw;
    UNSCOPED_INFO("a live build after the guest moved on differs");
    REQUIRE((!built(movedDraw) || !sameDraw(movedDraw, liveDraw)));
    // A snapshot without the declaration reads it late, from guest memory.
    GuestReadSnapshot partial;
    for (const auto& range : snapshot.ranges)
        if (range.address == kG)
            partial.ranges.push_back(range);
    {
        GuestReadSnapshotScope  scope(&partial, nullptr);
        IndexedBufferDrawInputs late;
        static_cast<void>(read(late));
        UNSCOPED_INFO("a read the snapshot does not hold is late");
        REQUIRE(scope.Late() > 0);
    }
    // An identity whose ranges outgrow the bounds is never captured.
    auto wide = submission;
    wide.caller ^= 1;
    GuestReadSet many;
    for (std::uint32_t i = 0; i <= GuestReadTemplates::kMaxRanges; ++i)
        many.emplace_back(0x60000000U + i * 0x100, 4);
    templates.Learn(wide, many);
    UNSCOPED_INFO("an overgrown identity is not captured");
    REQUIRE(!templates.Capture(wide, view, snapshot));
    std::cout << "Scene snapshot reads: a captured draw reads as captured after the guest moves on; uncaptured reads are late; overgrown identities are not captured\n";
}

// Deferred reads need snapshots; user-pointer draws remain uncaptured.
void deferredSnapshots()
{
    Draw targetDraw;
    targetDraw.form          = kSceneTargetForms[3];
    targetDraw.color         = 0x87000004;
    targetDraw.blend         = 0x00010001;
    auto             memory  = memoryFor(targetDraw);
    const auto       catalog = fullCatalog();
    OffscreenSession session(memory, catalog);
    session.config.build = ComposeNativeDrawFamilies({ MakeSceneDrawFamily(SnapshotReads(readerFor(memory)), catalog) });
    session.assembler    = std::make_unique<NativeFrameAssembler>(session.config);
    session.Clear();
    auto&                 frames = *session.assembler;
    const GuestMemoryView view   = [&memory](std::uint64_t address, std::uint64_t size, bool physical)
    {
        return memory.View(address, size, physical);
    };
    const auto draw = [&](const GuestReadSnapshot* snapshot, std::string& refusal)
    {
        auto submission          = submissionFor(kSceneMeshDrawCaller);
        submission.sequence      = ++session.sequence;
        submission.operationMark = frames.NumberOperation();
        frames.BeganSequenced(submission, snapshot);
        const auto verdict = frames.Admit(submission);
        refusal.clear();
        frames.Recorded(submission, verdict, &refusal);
        return !verdict;
    };
    std::string refusal;
    UNSCOPED_INFO("an inline sequenced draw is admitted and teaches its reads: " + refusal);
    REQUIRE(draw(nullptr, refusal));

    // A draw of the identity over another index buffer captures that header.
    constexpr std::uint32_t kMoved = kIndexBuffer + 0x1000;
    memory.Add(kMoved, false, memory.At(kIndexBuffer, false).bytes);
    put(memory.At(kG, false).bytes, 0x3094, kMoved);
    const auto        submission = submissionFor(kSceneMeshDrawCaller);
    GuestReadSnapshot snapshot;
    UNSCOPED_INFO("the capture holds this draw's index buffer header, not the learned one");
    REQUIRE((frames.ReadTemplates().Capture(submission, view, snapshot) && snapshot.Find(kMoved, 32).size() == 32 && snapshot.Find(kIndexBuffer, 32).empty()));
    UNSCOPED_INFO("the deferred draw is admitted from its snapshot: " + refusal);
    REQUIRE(draw(&snapshot, refusal));

    // A snapshot without the declaration: the build reads it late.
    GuestReadSnapshot partial;
    for (const auto& range : snapshot.ranges)
        if (range.address == kG)
            partial.ranges.push_back(range);
    UNSCOPED_INFO("a deferred build that read late is refused: " + refusal);
    REQUIRE((!draw(&partial, refusal) && refusal.find("deferred_snapshot_incomplete") != std::string::npos));
    GuestReadSnapshot after;
    UNSCOPED_INFO("its identity is no longer captured");
    REQUIRE(!frames.ReadTemplates().Capture(submission, view, after));
    UNSCOPED_INFO("its draws still build inline: " + refusal);
    REQUIRE(draw(nullptr, refusal));

    // User-pointer draws read caller memory the guest reuses on return.
    for (const auto hook : { NativeDrawHook::IndexedUp, NativeDrawHook::NonindexedUp })
    {
        auto up = submission;
        up.hook = hook;
        frames.ReadTemplates().Learn(up, { { kG, 0x3500 } });
        UNSCOPED_INFO("a user-pointer draw is never captured");
        REQUIRE(!frames.ReadTemplates().Capture(up, view, after));
    }
    std::cout << "Deferred snapshots: dynamic index buffer header captured, late builds refused, user-pointer draws inline\n";
}

// Resolve keys use address/sequence; repeated bytes replay; no 10:10:10:2.
void aliasing()
{
    constexpr std::uint32_t kAlias = 0xFE7AF000;
    Draw                    draw;
    draw.form                = kSceneTargetForms[3];
    draw.color               = 0x87000004;
    draw.blend               = 0x00010001;
    auto             memory  = memoryFor(draw);
    const auto       catalog = fullCatalog();
    OffscreenSession session(memory, catalog);
    std::string      error;

    auto target = session.Clear();
    session.Draws(2);
    const auto first = session.Resolve(target, kAlias, 0x01000300, error);
    UNSCOPED_INFO("the 1024x1024 resolve is modeled: " + error);
    REQUIRE(first != 0);
    const auto second = session.Resolve(target, kAlias, 0x01000300, error);
    UNSCOPED_INFO("a second resolve with no draw between is modeled: " + error);
    REQUIRE((second != 0 && second != first));

    // A 512x512 target resolved to the same address in another format.
    draw.form = kSceneTargetForms[2];
    memory    = memoryFor(draw);
    target    = session.Clear();
    session.Draws(1);
    const auto third = session.Resolve(target, kAlias, 0x01000380, error);
    UNSCOPED_INFO("the 512x512 resolve to the same address is modeled: " + error);
    REQUIRE(third != 0);

    const std::array<std::uint64_t, 3> marks{ first, second, third };
    std::set<std::uint64_t>            keys;
    for (unsigned i = 0; i < marks.size(); ++i)
    {
        const auto& span    = session.Flush(marks[i]);
        const auto& texture = span.frame->textures[0];
        const auto  extent  = i < 2 ? 1024U : 512U;
        UNSCOPED_INFO(std::format("resolve {} to {:#x} keeps its own key, extent, destination and draws", i, kAlias));
        REQUIRE(((span.status == NativeFrameStatus::Complete || span.status == NativeFrameStatus::Provisional) && texture.persistentKey == ((std::uint64_t(kAlias) << 32) | marks[i]) &&
                 texture.width == extent && texture.height == extent && texture.format == NativeDrawReplayTextureFormat::Rgba8 &&
                 span.guestResolve->destBase == kAlias && span.guestResolve->destInfo == (i < 2 ? 0x01000300U : 0x01000380U) &&
                 span.frame->passes.size() == 1 && span.frame->passes[0].DrawCount() == (i < 2 ? 2U : 1U) && span.frame->passes[0].target.width == extent));
        keys.insert(texture.persistentKey);
    }
    UNSCOPED_INFO("three resolves to one address are three producers");
    REQUIRE(keys.size() == 3);

    // A 10:10:10:2 source at the same address packs k_2_10_10_10 words.
    draw.form           = kSceneTargetForms[3];
    draw.form.colorInfo = 0x00020000;
    memory              = memoryFor(draw);
    target              = session.Clear(false);
    UNSCOPED_INFO("the 10:10:10:2 target decodes");
    REQUIRE(target.native.format == NativeDrawReplayTargetFormat::Rgb10a2);
    const auto fourth = session.Resolve(target, kAlias, 0x01000380, error);
    UNSCOPED_INFO("a 10:10:10:2 source to the same address is modeled: " + error);
    REQUIRE(fourth != 0);
    const auto& packed = session.Flush(fourth);
    UNSCOPED_INFO("the 10:10:10:2 resolve packs its words");
    REQUIRE((packed.guestResolve && packed.guestResolve->packing == GuestResolvePacking::Color2101010Words));
    std::cout << "Resolve aliasing: two 1024x1024 resolves and a 512x512 one to one address are three producers with their own keys, extents and destinations; a 10:10:10:2 source packs its words\n";
}

// Scene loop pairs bind live bool/loop constants.
void loops()
{
    Draw looped;
    looped.pair.psWord                             = 0x70000010;
    auto                                   catalog = catalogOf({ interfaceFor(looped, true, 4), interfaceFor(looped, false, 4) });
    const auto                             shaders = ScenePixelShaders(*catalog);
    auto                                   memory  = memoryFor(looped);
    SceneDrawRecipe                        recipe;
    std::string                            error;
    auto                                   live = readScene(memory, shaders);
    constexpr std::array<std::uint32_t, 3> kTrophyLoops{ 0x00010003, 0x00010000, 0x00010001 };
    // World harvest 0ECFF8F5/4D5283C0: PS loops i16..i18 at G+0x27E0. Bool/loop
    // words start at G+0x2780; loop_start i16 uses these words.
    for (std::size_t i = 0; i < kTrophyLoops.size(); ++i)
        put(live.state, 0x27E0 + i * 4, kTrophyLoops[i]);
    put(live.state, 0x2790, 0x80000001);
    const auto shadowed = GuestDrawRegisters(live.state);
    UNSCOPED_INFO(std::string("the 40 bool and loop words map from G+0x2780") + (""));
    REQUIRE((shadowed[0x4918] == 0x10003 && shadowed[0x4919] == 0x10000 && shadowed[0x491A] == 0x10001 && shadowed[0x4904] == 0x80000001 &&
             shadowed[0x4908] == 0 && shadowed[0x4928] == 0));
    UNSCOPED_INFO(std::string("a loop pair converts with its captured loop words: ") + (error));
    REQUIRE(BuildSceneDrawRecipe(live, *catalog, recipe, error));
    // The draw binds the live words after the bools (bytes 336..463); the
    // gameplay harvest's words for the same pair convert with the same entry.
    const auto loopWords = [](const SceneDrawRecipe& built)
    {
        std::array<std::uint32_t, 32> words{};
        std::memcpy(words.data(), built.native.sharedConstants.data() + 336, sizeof(words));
        return words;
    };
    UNSCOPED_INFO(std::string("the world loop words bind at bytes 336..463") + (""));
    REQUIRE((recipe.native.sharedConstants.size() == 464 && loopWords(recipe)[16] == 0x10003 && loopWords(recipe)[17] == 0x10000 &&
             loopWords(recipe)[18] == 0x10001 && loopWords(recipe)[0] == 0));
    const auto* entry = recipe.entry;
    put(live.state, 0x27E0, 0x00010009);
    put(live.state, 0x27E8, 0x00010005);
    UNSCOPED_INFO(std::string("the gameplay loop words bind to the same entry: ") + (error));
    REQUIRE((BuildSceneDrawRecipe(live, *catalog, recipe, error) && recipe.entry == entry && loopWords(recipe)[16] == 0x10009 && loopWords(recipe)[18] == 0x10005));
    Draw       plain;
    const auto plainCatalog = catalogOf({ interfaceFor(plain, true, 4), interfaceFor(plain, false, 4) });
    auto       plainMemory  = memoryFor(plain);
    auto       plainInputs  = readScene(plainMemory, ScenePixelShaders(*plainCatalog));
    put(plainInputs.state, 0x27E0, 0x00010009);
    UNSCOPED_INFO(std::string("a loop-free pair keeps the 336-byte shared constants: ") + (error));
    REQUIRE((BuildSceneDrawRecipe(plainInputs, *plainCatalog, recipe, error) && recipe.native.sharedConstants.size() == 336));
    std::cout << "Scene loops: the 40 bool and loop words map from G+0x2780; a loop pair binds the live loop words at bytes 336..463 for one entry\n";
}

void sharedIndexBytes()
{
    const auto catalog    = fullCatalog();
    const auto shaders    = ScenePixelShaders(*catalog);
    auto       memory     = memoryFor();
    auto       indices    = std::make_shared<const std::vector<std::uint8_t>>(memory.At(kIndices, true).bytes);
    const auto vertices   = std::make_shared<const std::vector<std::uint8_t>>(memory.At(kVertices, true).bytes);
    auto       read       = readerFor(memory);
    read.mirroredResource = [&](const GuestResourceRead& request) -> std::optional<GuestBytes>
    {
        if (request.kind == GuestResourceKind::IndexBuffer && request.address == kIndices && request.size == indices->size())
            return GuestBytes::Shared(indices);
        if (request.kind == GuestResourceKind::VertexBuffer && request.address == kVertices && request.size == vertices->size())
            return GuestBytes::Shared(vertices);
        return std::nullopt;
    };
    IndexedBufferDrawInputs inputs;
    std::string             error;
    UNSCOPED_INFO(std::string("shared index inputs read: ") + (error));
    REQUIRE(ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), read, shaders, inputs, error, IndexedBufferForm::Scene));
    UNSCOPED_INFO("the indexed reader retains the mirror's shared Buffer");
    REQUIRE((inputs.indices.empty() && inputs.indexBytes.Buffer() == indices));
    NativeDrawReplayRecipe reference, first, again;
    auto                   cache = std::make_unique<SceneBuildCache>(catalog);
    UNSCOPED_INFO(std::string("shared indices build uncached: ") + (error));
    REQUIRE(BuildSceneDraw(inputs, *catalog, reference, error));
    const auto owners = indices.use_count();
    UNSCOPED_INFO(std::string("shared indices build cached: ") + (error));
    REQUIRE(BuildSceneDraw(inputs, *catalog, first, error, nullptr, cache.get()));
    UNSCOPED_INFO("the geometry entry retains the same index Buffer");
    REQUIRE((indices.use_count() == owners + 1 && first.sharedIndices && first.sharedVertexData));
    UNSCOPED_INFO(std::string("an identical index buffer and range hit the geometry cache: ") + (error));
    REQUIRE((BuildSceneDraw(inputs, *catalog, again, error, nullptr, cache.get()) && again.sharedIndices == first.sharedIndices && again.sharedVertexData == first.sharedVertexData &&
             recipeBytes(again) == recipeBytes(reference)));

    auto fallback       = inputs;
    fallback.indices    = std::vector<std::uint8_t>(fallback.indexBytes);
    fallback.indexBytes = {};
    NativeDrawReplayRecipe fallbackDraw;
    UNSCOPED_INFO(std::string("legacy vector inputs hit by content: ") + (error));
    REQUIRE((BuildSceneDraw(fallback, *catalog, fallbackDraw, error, nullptr, cache.get()) && fallbackDraw.sharedIndices == first.sharedIndices &&
             recipeBytes(fallbackDraw) == recipeBytes(reference)));

    auto                                                 deferred = inputs;
    const std::weak_ptr<const std::vector<std::uint8_t>> original = indices;
    indices                                                       = std::make_shared<const std::vector<std::uint8_t>>(*indices);
    UNSCOPED_INFO(std::string("refreshed index inputs read: ") + (error));
    REQUIRE(ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), read, shaders, inputs, error, IndexedBufferForm::Scene));
    NativeDrawReplayRecipe refreshed;
    UNSCOPED_INFO(std::string("equal refreshed index bytes hit by content: ") + (error));
    REQUIRE((BuildSceneDraw(inputs, *catalog, refreshed, error, nullptr, cache.get()) && refreshed.sharedIndices == first.sharedIndices &&
             recipeBytes(refreshed) == recipeBytes(reference)));
    NativeDrawReplayRecipe deferredDraw;
    UNSCOPED_INFO(std::string("a deferred input snapshot survives a refreshed mirror and reused reader: ") + (error));
    REQUIRE((!original.expired() && BuildSceneDraw(deferred, *catalog, deferredDraw, error) && recipeBytes(deferredDraw) == recipeBytes(reference)));
    deferred = {};
    UNSCOPED_INFO("a content hit adopts the refreshed shared buffer");
    REQUIRE(original.expired());

    auto  changed = inputs;
    auto* bytes   = changed.indexBytes.MutableData();
    std::swap(bytes[0], bytes[2]);
    std::swap(bytes[1], bytes[3]);
    changed.indexBytes = GuestBytes::Shared(std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>(changed.indexBytes)));
    NativeDrawReplayRecipe changedDraw;
    UNSCOPED_INFO(std::string("changed index bytes in a new buffer decode fresh geometry: ") + (error));
    REQUIRE((BuildSceneDraw(changed, *catalog, changedDraw, error, nullptr, cache.get()) && changedDraw.sharedIndices != first.sharedIndices &&
             !std::ranges::equal(changedDraw.indexSpan(), first.indexSpan())));
    UNSCOPED_INFO("copy-on-write leaves a pending draw's shared indices unchanged");
    REQUIRE((inputs.indexBytes.Buffer() == indices && recipeBytes(first) == recipeBytes(reference)));
    changed.indexBytes = {};

    const std::weak_ptr<const std::vector<std::uint8_t>> retained = indices;
    ReturnDrawInputs(std::move(inputs));
    UNSCOPED_INFO("returned inputs release their shared index buffer");
    REQUIRE(inputs.indexBytes.empty());
    indices.reset();
    UNSCOPED_INFO("the geometry entry outlives the inputs and spare reset");
    REQUIRE(!retained.expired());
    cache.reset();
    UNSCOPED_INFO("spare inputs retain no index mirror after the cache releases it");
    REQUIRE(retained.expired());

    // The reset at the next read must also release an unreturned mirror.
    indices = std::make_shared<const std::vector<std::uint8_t>>(memory.At(kIndices, true).bytes);
    UNSCOPED_INFO(std::string("shared indices read before refusal: ") + (error));
    REQUIRE(ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), read, shaders, inputs, error, IndexedBufferForm::Scene));
    const std::weak_ptr<const std::vector<std::uint8_t>> reset   = indices;
    auto                                                 refused = memoryFor();
    put(refused.At(kG, false).bytes, 0x2E2C, 0);
    UNSCOPED_INFO("a refused reused read resets index bytes");
    REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), readerFor(refused), shaders, inputs, error, IndexedBufferForm::Scene) &&
             indexBytes(inputs).empty()));
    indices.reset();
    UNSCOPED_INFO("a reset read releases its old index mirror");
    REQUIRE(reset.expired());
    std::cout << "Scene shared index bytes: identity and refreshed content hits, changed bytes, copy-on-write and spare lifetime\n";
}

// Only unchanged stream buffers reuse geometry; equal refreshed bytes rebuild.
void buildCache()
{
    auto        catalog = fullCatalog();
    std::string error;
    const auto  build = [&](const IndexedBufferDrawInputs& inputs, const SceneDrawCatalog& of, SceneBuildCache* cache, std::string& detail)
    {
        NativeDrawReplayRecipe draw;
        const bool             ok = BuildSceneDraw(inputs, of, draw, detail, nullptr, cache);
        return std::pair{ ok, std::move(draw) };
    };
    const auto alike = [&](const IndexedBufferDrawInputs& inputs, SceneBuildCache& cache, const std::string& what, const SceneDrawCatalog* of = nullptr)
    {
        std::string uncachedDetail, cachedDetail;
        const auto& in          = of ? *of : *catalog;
        auto [ok, reference]    = build(inputs, in, nullptr, uncachedDetail);
        auto [cachedOk, cached] = build(inputs, in, &cache, cachedDetail);
        UNSCOPED_INFO(std::string(what + " builds alike through the build cache: ") + (cachedDetail));
        REQUIRE((ok == cachedOk && uncachedDetail == cachedDetail && (!ok || recipeBytes(reference) == recipeBytes(cached))));
        UNSCOPED_INFO(what + " owns its payloads without the cache");
        REQUIRE((!ok || (!reference.sharedVertexData && !reference.sharedIndices && !reference.sharedVertexShaderDxil && !reference.shaderVersion)));
        return std::pair{ ok, std::move(cached) };
    };
    const auto shared = [](const GuestBytes& bytes)
    {
        return GuestBytes::Shared(std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>(bytes)));
    };

    auto       memory     = memoryFor();
    const auto shaders    = ScenePixelShaders(*catalog);
    auto       inputs     = readScene(memory, shaders);
    inputs.streamBytes[0] = shared(inputs.streamBytes[0]);
    SceneBuildCache cache(catalog);
    const auto [firstOk, first] = alike(inputs, cache, "a new draw");
    UNSCOPED_INFO("a new draw keeps its decoded geometry and shares its validated shaders");
    REQUIRE((firstOk && first.vertexData.empty() && first.sharedVertexData && first.indices.empty() && first.sharedIndices && first.sharedVertexShaderDxil && first.shaderVersion));
    const auto [againOk, again] = alike(inputs, cache, "a repeated draw");
    UNSCOPED_INFO("a repeated draw shares the decoded geometry and shaders");
    REQUIRE((againOk && again.sharedVertexData == first.sharedVertexData && again.sharedIndices == first.sharedIndices && again.sharedVertexShaderDxil == first.sharedVertexShaderDxil));

    // A recipe keeps the geometry it shares after its cache is gone, as a
    // deferred build's recipe does past a cache reset or sweep.
    {
        std::weak_ptr<const NativeDrawReplayIndices>   indices;
        std::weak_ptr<const std::vector<std::uint8_t>> vertices;
        std::optional<NativeDrawReplayRecipe>          pending;
        {
            SceneBuildCache transient(catalog);
            UNSCOPED_INFO("a transient cache's first draw builds");
            REQUIRE(alike(inputs, transient, "a transient cache's first draw").first);
            auto [hitOk, hit] = alike(inputs, transient, "a transient cache's repeated draw");
            UNSCOPED_INFO("a transient cache's repeated draw shares its own decode");
            REQUIRE((hitOk && hit.sharedIndices && hit.sharedIndices != first.sharedIndices));
            indices  = hit.sharedIndices;
            vertices = hit.sharedVertexData;
            pending.emplace(std::move(hit));
        }
        std::string pendingError;
        UNSCOPED_INFO(std::string("a pending recipe outlives its cache with the geometry it shares: ") + (pendingError));
        REQUIRE((!indices.expired() && !vertices.expired() && recipeBytes(*pending) == recipeBytes(first) && ValidateNativeDrawReplayRecipe(*pending, pendingError)));
        pending.reset();
        UNSCOPED_INFO("the pending recipe was the geometry's last owner");
        REQUIRE((indices.expired() && vertices.expired()));
    }
    // The selection memo hashes only the head of each input; a byte past it
    // that changes the uncached outcome still selects and patches as an
    // uncached build does.
    {
        SceneDrawRecipe warm;
        std::string     warmError;
        UNSCOPED_INFO(std::string("the warm draw builds: ") + (warmError));
        REQUIRE(BuildSceneDrawRecipe(inputs, *catalog, warm, warmError, nullptr, &cache));
        std::size_t changed = 0;
        for (const auto field : { &IndexedBufferDrawInputs::vertexCode, &IndexedBufferDrawInputs::vertexMetadata, &IndexedBufferDrawInputs::declaration, &IndexedBufferDrawInputs::fetchTemplates, &IndexedBufferDrawInputs::formatTable })
            for (std::size_t at = 32; at < (inputs.*field).size(); ++at)
            {
                auto lateChange = inputs;
                (lateChange.*field)[at] ^= 1;
                SceneDrawRecipe late, reference;
                std::string     lateError, referenceError;
                const bool      referenceOk = BuildSceneDrawRecipe(lateChange, *catalog, reference, referenceError);
                if (referenceOk && reference.pair == warm.pair && reference.patchedVertexCode == warm.patchedVertexCode)
                    continue;
                ++changed;
                const bool lateOk = BuildSceneDrawRecipe(lateChange, *catalog, late, lateError, nullptr, &cache);
                UNSCOPED_INFO(std::string(std::format("an input changed at byte {}, past the hashed head, selects as uncached: ", at)) + (lateError));
                REQUIRE((lateOk == referenceOk && lateError == referenceError && late.pair == reference.pair && late.patchedVertexCode == reference.patchedVertexCode));
            }
        UNSCOPED_INFO("a byte past the hashed head changes the outcome");
        REQUIRE(changed > 0);
    }

    // A mirror refresh with the same bytes is a new buffer of equal content.
    auto renewed           = inputs;
    renewed.streamBytes[0] = shared(inputs.streamBytes[0]);
    UNSCOPED_INFO("the renewed stream is another buffer");
    REQUIRE(!renewed.streamBytes[0].SharesBuffer(inputs.streamBytes[0]));
    const auto [renewedOk, renewedDraw] = alike(renewed, cache, "the same stream bytes in a new buffer");
    UNSCOPED_INFO("the same stream bytes in a new buffer decode again");
    REQUIRE((renewedOk && renewedDraw.sharedVertexData && renewedDraw.sharedVertexData != first.sharedVertexData));
    UNSCOPED_INFO("the new buffer then shares its own decode");
    REQUIRE(alike(renewed, cache, "the new buffer again").second.sharedVertexData == renewedDraw.sharedVertexData);

    // A stale stream: one byte of the first position changes.
    auto stale = inputs;
    {
        std::vector<std::uint8_t> bytes = inputs.streamBytes[0];
        bytes[3] ^= 1;
        stale.streamBytes[0] = GuestBytes::Shared(std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)));
    }
    const auto [staleOk, staleDraw] = alike(stale, cache, "a changed stream");
    UNSCOPED_INFO("a changed stream decodes its own geometry");
    REQUIRE((staleOk && staleDraw.sharedVertexData != first.sharedVertexData && !std::ranges::equal(staleDraw.vertexBytes(), again.vertexBytes())));

    // Stale indices: the first triangle's winding changes.
    auto  rewound      = inputs;
    auto* rewoundBytes = rewound.indexBytes.MutableData();
    std::swap(rewoundBytes[0], rewoundBytes[2]);
    std::swap(rewoundBytes[1], rewoundBytes[3]);
    const auto [rewoundOk, rewoundDraw] = alike(rewound, cache, "changed indices");
    UNSCOPED_INFO("changed indices decode their own triangles");
    REQUIRE((rewoundOk && !std::ranges::equal(rewoundDraw.indexSpan(), again.indexSpan()) && rewoundDraw.sharedIndices != first.sharedIndices && rewoundDraw.sharedVertexData != first.sharedVertexData));

    // Another VS of the PS, a PS outside the catalog, another
    // specialization, and the first draw again.
    Draw normal;
    normal.pair                       = normalPair();
    auto normalMemory                 = memoryFor(normal);
    const auto [normalOk, normalDraw] = alike(readScene(normalMemory, shaders), cache, "a second VS of the PS");
    UNSCOPED_INFO("a second VS of the PS validates its own entry");
    REQUIRE((normalOk && normalDraw.sharedVertexShaderDxil != again.sharedVertexShaderDxil));
    Draw other;
    other.pair.psWord               = 0x9ABC;
    auto otherInputs                = inputs;
    otherInputs.pixelCode           = psCode(other.pair);
    const auto [otherOk, otherDraw] = alike(otherInputs, cache, "a PS outside the catalog");
    UNSCOPED_INFO("a PS outside the catalog is not a candidate");
    REQUIRE(!otherOk);
    Draw tested;
    tested.color      = 0x87000004;
    auto testedMemory = memoryFor(tested);
    UNSCOPED_INFO("another alpha specialization builds");
    REQUIRE(alike(readScene(testedMemory, shaders), cache, "another alpha specialization").first);
    UNSCOPED_INFO("the first draw after others still shares its geometry");
    REQUIRE(alike(inputs, cache, "the first draw after others").second.sharedVertexData == first.sharedVertexData);

    // A depth-only draw through a cache of its catalog, and other catalog
    // objects through the first cache.
    auto depthMemory  = depthOnlyMemory();
    auto depthCatalog = fullCatalog();
    depthCatalog->depthOnly.push_back({ depthOnlyInterface(depthMemory.At(kVsResource, true).bytes) });
    IndexedBufferDrawInputs depthInputs;
    UNSCOPED_INFO(std::string("the depth-only draw reads: ") + (error));
    REQUIRE(ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), readerFor(depthMemory), ScenePixelShaders(*depthCatalog), depthInputs, error, IndexedBufferForm::Scene, SceneDepthOnlyVertexShaders(*depthCatalog), depthPatched()));
    depthInputs.streamBytes[0] = shared(depthInputs.streamBytes[0]);
    SceneBuildCache depthCache(depthCatalog);
    const auto [depthOk, depthDraw] = alike(depthInputs, depthCache, "a depth-only draw", depthCatalog.get());
    UNSCOPED_INFO("a depth-only draw shares its validated VS");
    REQUIRE((depthOk && depthDraw.depthOnly && depthDraw.sharedVertexShaderDxil));
    const auto [depthAgainOk, depthAgain] = alike(depthInputs, depthCache, "a repeated depth-only draw", depthCatalog.get());
    UNSCOPED_INFO("a repeated depth-only draw shares its geometry and VS");
    REQUIRE((depthAgainOk && depthAgain.sharedVertexData == depthDraw.sharedVertexData && depthAgain.sharedVertexShaderDxil == depthDraw.sharedVertexShaderDxil));
    auto depthStale = depthInputs;
    {
        std::vector<std::uint8_t> bytes = depthInputs.streamBytes[0];
        bytes[3] ^= 1;
        depthStale.streamBytes[0] = GuestBytes::Shared(std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)));
    }
    UNSCOPED_INFO("a changed depth-only stream decodes its own geometry");
    REQUIRE(alike(depthStale, depthCache, "a depth-only draw with a changed stream", depthCatalog.get()).second.sharedVertexData != depthDraw.sharedVertexData);
    // A reloaded catalog has equal entries at other addresses; the cache
    // serves only the catalog it holds.
    const auto reloaded                   = fullCatalog();
    const auto [reloadedOk, reloadedDraw] = alike(inputs, cache, "the first draw against a reloaded catalog", reloaded.get());
    UNSCOPED_INFO("a catalog the cache does not hold builds uncached");
    REQUIRE((reloadedOk && !reloadedDraw.sharedVertexData && !reloadedDraw.shaderVersion));
    const auto meshOnly           = catalogOf({ interfaceFor(Draw{}, true, 4) });
    const auto [meshOk, meshDraw] = alike(inputs, cache, "the first draw against another catalog", meshOnly.get());
    UNSCOPED_INFO("another catalog builds uncached");
    REQUIRE((meshOk && !meshDraw.sharedVertexData));
    UNSCOPED_INFO("the held catalog's memo survives builds against others");
    REQUIRE(alike(inputs, cache, "the first draw against its catalog again").second.sharedVertexData == first.sharedVertexData);
    std::weak_ptr<const SceneDrawCatalog> watched;
    {
        auto held = fullCatalog();
        watched   = held;
        const SceneBuildCache holding(held);
        held.reset();
        UNSCOPED_INFO("a cache keeps its catalog alive");
        REQUIRE(!watched.expired());
    }
    UNSCOPED_INFO("a cache releases its catalog with itself");
    REQUIRE(watched.expired());

    // Geometry no build used between two sweeps is decoded again.
    const auto swept = [&](bool sweep)
    {
        SceneBuildCache sweeping(catalog);
        const auto      before = alike(inputs, sweeping, "a draw before the sweeps").second.sharedVertexData;
        std::string     detail;
        for (std::size_t i = 0; i < (sweep ? 2 * SceneBuildCache::kSweepBuilds : SceneBuildCache::kSweepBuilds / 2); ++i)
        {
            UNSCOPED_INFO(std::string("another draw builds: ") + (detail));
            REQUIRE(build(stale, *catalog, &sweeping, detail).first);
        }
        const auto after = alike(inputs, sweeping, "a draw after other draws").second.sharedVertexData;
        return before && after && before == after;
    };
    UNSCOPED_INFO("geometry used before a sweep is kept");
    REQUIRE(swept(false));
    UNSCOPED_INFO("geometry unused between two sweeps is decoded again");
    REQUIRE(!swept(true));
    SceneBuildCache unbudgeted{ catalog, 0 };
    for (int pass = 0; pass < 2; ++pass)
    {
        UNSCOPED_INFO("a draw without a geometry budget decodes its geometry each time");
        REQUIRE(!alike(inputs, unbudgeted, "a draw without a geometry budget").second.sharedVertexData);
    }
    std::cout << "Scene build cache: changed streams, indices and shaders, refreshed buffers and catalogs the cache does not hold build alike; only an unchanged stream buffer reuses geometry; swept and unbudgeted geometry decodes again\n";
}

// Reused scratch builds must match fresh-thread builds, with or without cache.
void scratchReuse()
{
    const auto catalog = fullCatalog();
    const auto shaders = ScenePixelShaders(*catalog);
    auto       memory  = memoryFor();
    Draw       normal;
    normal.pair       = normalPair();
    auto normalMemory = memoryFor(normal);
    Draw tested;
    tested.color      = 0x87000004;
    auto testedMemory = memoryFor(tested);
    Draw other;
    other.pair.psWord     = 0x9ABC;
    auto otherInputs      = readScene(memory, shaders);
    otherInputs.pixelCode = psCode(other.pair);
    auto depthMemory      = depthOnlyMemory();
    auto depthCatalog     = fullCatalog();
    depthCatalog->depthOnly.push_back({ depthOnlyInterface(depthMemory.At(kVsResource, true).bytes) });
    IndexedBufferDrawInputs depthInputs;
    std::string             error;
    UNSCOPED_INFO(std::string("the depth-only draw reads: ") + (error));
    REQUIRE(ReadIndexedBufferDrawInputs(submissionFor(kSceneMeshDrawCaller), readerFor(depthMemory), ScenePixelShaders(*depthCatalog), depthInputs, error, IndexedBufferForm::Scene, SceneDepthOnlyVertexShaders(*depthCatalog), depthPatched()));
    const std::shared_ptr<const SceneDrawCatalog> meshOnly = catalogOf({ interfaceFor(Draw{}, true, 4) });

    struct Case
    {
        std::string                             name;
        IndexedBufferDrawInputs                 inputs;
        std::shared_ptr<const SceneDrawCatalog> catalog;
        bool                                    builds;
    };

    const std::vector<Case> cases{
        { "the default draw", readScene(memory, shaders), catalog, true },
        { "a second VS of the PS", readScene(normalMemory, shaders), catalog, true },
        { "an alpha-tested draw", readScene(testedMemory, shaders), catalog, true },
        { "a PS outside the catalog", otherInputs, catalog, false },
        { "a VS outside the catalog", readScene(normalMemory, shaders), meshOnly, false },
        { "a depth-only draw", depthInputs, depthCatalog, true },
    };

    // Every output of both builds as bytes, the catalog objects by address.
    const auto outcome = [](const Case& draw, SceneBuildCache* cache, SceneDrawRecipe& recipe)
    {
        std::vector<std::uint8_t> out;
        const auto                append = [&](std::span<const std::uint8_t> bytes)
        {
            const auto size = std::uint64_t(bytes.size());
            out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&size), reinterpret_cast<const std::uint8_t*>(&size) + sizeof(size));
            out.insert(out.end(), bytes.begin(), bytes.end());
        };
        const auto text = [&](const std::string& value)
        {
            append({ reinterpret_cast<const std::uint8_t*>(value.data()), value.size() });
        };
        const auto address = [&](const void* pointer)
        {
            const auto value = reinterpret_cast<std::uintptr_t>(pointer);
            append({ reinterpret_cast<const std::uint8_t*>(&value), sizeof(value) });
        };
        NativeDrawReplayRecipe built;
        std::string            detail;
        out.push_back(BuildSceneDraw(draw.inputs, *draw.catalog, built, detail, nullptr, cache));
        text(detail);
        append(recipeBytes(built));
        out.push_back(BuildSceneDrawRecipe(draw.inputs, *draw.catalog, recipe, detail, nullptr, cache));
        text(detail);
        append(recipeBytes(recipe.native));
        append(recipe.patchedVertexCode);
        append(recipe.effectiveState);
        NativeDrawReplayRecipe bound;
        UNSCOPED_INFO(draw.name + " binds at most the replay's texture slots");
        REQUIRE(recipe.textures.size() <= bound.textures.size());
        out.push_back(std::uint8_t(recipe.textures.size()));
        std::ranges::copy(recipe.textures, bound.textures.begin());
        bound.samplers = recipe.samplers;
        append(recipeBytes(bound));
        address(recipe.entry);
        address(recipe.form);
        for (const auto* entry : recipe.pair)
            address(entry);
        return out;
    };

    // Each draw first on its own thread, whose working buffers are new.
    std::vector<std::vector<std::uint8_t>> fresh(cases.size());
    for (std::size_t i = 0; i < cases.size(); ++i)
    {
        std::exception_ptr failure;
        std::thread([&]
                    {
                        try
                        {
                            SceneDrawRecipe recipe;
                            fresh[i] = outcome(cases[i], nullptr, recipe);
                        }
                        catch (...)
                        {
                            failure = std::current_exception();
                        }
                    })
            .join();
        if (failure)
            std::rethrow_exception(failure);
        UNSCOPED_INFO(cases[i].name + " builds as expected on a fresh thread");
        REQUIRE(fresh[i][0] == cases[i].builds);
    }
    SceneDrawRecipe reused;
    for (const bool cached : { false, true })
        for (const bool reversed : { false, true })
        {
            SceneBuildCache colorCache(catalog), depthCache(depthCatalog), meshCache(meshOnly);
            for (std::size_t n = 0; n < cases.size(); ++n)
            {
                const auto  i     = reversed ? cases.size() - 1 - n : n;
                const auto& draw  = cases[i];
                auto*       cache = !cached ? nullptr : draw.catalog == depthCatalog ? &depthCache
                                                    : draw.catalog == meshOnly       ? &meshCache
                                                                                     : &colorCache;
                UNSCOPED_INFO(std::format("{} builds as on a fresh thread after other draws{}{}", draw.name, reversed ? " in reverse" : "", cached ? " through a build cache" : ""));
                REQUIRE(outcome(draw, cache, reused) == fresh[i]);
            }
        }
    std::cout << "Scene build scratch: color, depth-only and refused draws build alike after other draws on one thread, cached or not\n";
}

// Digest hits skip byte validation; byte hits still detect in-place DXIL edits.
void shaderHits()
{
    using Hit         = NativeGameplayShaderValidationCache::Hit;
    auto       memory = depthOnlyMemory();
    const auto vsCode = memory.At(kVsResource, true).bytes;
    // The guest's big-endian words, as the scene build reads them.
    std::vector<std::uint32_t> words;
    for (std::size_t i = 0; i + 4 <= vsCode.size(); i += 4)
        words.push_back(std::uint32_t(vsCode[i]) << 24 | std::uint32_t(vsCode[i + 1]) << 16 | std::uint32_t(vsCode[i + 2]) << 8 | vsCode[i + 3]);
    NativeGameplayDrawView view;
    view.vertexMicrocode = words;
    std::string error;
    for (const auto hit : { Hit::Bytes, Hit::Digests })
    {
        const bool                          digests = hit == Hit::Digests;
        auto                                shader  = depthOnlyInterface(vsCode);
        NativeGameplayShaderValidationCache cache{ hit };
        UNSCOPED_INFO(std::string("a miss validates the pinned bytes: ") + (error));
        REQUIRE(cache.Validate(view, shader, error));
        const auto validated = cache.VertexBytecode();
        shader.vertexShaderDxil[3] ^= 1;
        UNSCOPED_INFO((digests ? "a digest hit compares no bytes" : "a byte hit validates changed DXIL again"));
        REQUIRE(cache.Validate(view, shader, error) == digests);
        UNSCOPED_INFO("a digest hit keeps the validated bytes");
        REQUIRE((!digests || cache.VertexBytecode() == validated));
        shader.vertexDxilDigest = dxilDigest(shader.vertexShaderDxil);
        UNSCOPED_INFO(std::string("other digests validate again: ") + (error));
        REQUIRE((cache.Validate(view, shader, error) && cache.VertexBytecode() != validated && *cache.VertexBytecode() == shader.vertexShaderDxil));
    }
    std::cout << "Scene shader hits: digest hits compare no bytes, other digests validate again, byte hits validate changed DXIL again\n";
}

} // namespace synthetic

void unit()
{
    synthetic::loops();
    synthetic::catalog();
    synthetic::reader();
    synthetic::family();
    synthetic::particle();
    synthetic::builder();
    synthetic::packed();
    synthetic::userClip();
    synthetic::depthOnly();
    synthetic::shaderCache();
    synthetic::hostPatchedRouting();
    synthetic::identityCache();
#if REREVVED_DEVELOPER
    synthetic::vsCodeWatch();
#endif
    synthetic::inputReuse();
    synthetic::textureRangeAnswers();
    synthetic::resolvedShaderCache();
    synthetic::snapshotReads();
    synthetic::deferredSnapshots();
    synthetic::bakeFrame();
    synthetic::aliasing();
    synthetic::sharedIndexBytes();
    synthetic::buildCache();
    synthetic::scratchReuse();
    synthetic::shaderHits();
}

// Each harvested record of a scene caller, with its own RB_COLORCONTROL and
// the other alpha specialization, built against the catalog over synthetic
// geometry and zero texture bytes.
namespace harvest
{

std::vector<std::uint8_t> hexBytes(std::string_view text)
{
    UNSCOPED_INFO("even hex text");
    REQUIRE(text.size() % 2 == 0);
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < text.size(); i += 2)
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(std::string(text.substr(i, 2)), nullptr, 16)));
    return bytes;
}

std::uint32_t address(const toml::node_view<const toml::node>& node)
{
    return static_cast<std::uint32_t>(std::stoull(node.value_or(std::string("0")), nullptr, 16));
}

void literals(const toml::node_view<const toml::node>& records, std::vector<ShaderLiteralRange>& ranges, std::vector<std::pair<std::uint32_t, std::uint32_t>>& registers)
{
    const auto* all = records.as_array();
    UNSCOPED_INFO("literal records");
    REQUIRE(all != nullptr);
    for (const auto& node : *all)
    {
        const auto  record = toml::node_view<const toml::node>(node);
        const auto  first  = std::uint32_t(record["first_float4"].value_or<std::int64_t>(0));
        const auto* values = record["values"].as_array();
        UNSCOPED_INFO("literal values");
        REQUIRE((values != nullptr && values->size() == std::size_t(record["dword_count"].value_or<std::int64_t>(0))));
        ranges.push_back({ static_cast<std::uint16_t>(first), static_cast<std::uint16_t>(values->size()), address(record["physical"]) });
        for (std::uint32_t i = 0; i < values->size(); ++i)
            registers.emplace_back(kShaderLiteralRegisterBase + first * 4 + i, static_cast<std::uint32_t>((*values)[i].value_or<std::int64_t>(0)));
    }
}

// The guest's static fetch swizzle and format tables, from a capture's payloads.
std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> fetchTables(const std::filesystem::path& capture)
{
    const auto                manifest = toml::parse_file((capture / "payload-manifest.toml").string());
    const auto*               bindings = manifest["bindings"].as_array();
    std::vector<std::uint8_t> swizzle, formats;
    UNSCOPED_INFO("payload manifest bindings");
    REQUIRE(bindings != nullptr);
    for (const auto& node : *bindings)
    {
        const auto& binding = *node.as_table();
        const auto  role    = binding["role"].value_or(std::string());
        auto&       table   = role == "fetch_swizzle_table" ? swizzle : formats;
        if ((role == "fetch_swizzle_table" || role == "fetch_format_table") && table.empty())
            table = read(capture / binding["file"].value_or(std::string()));
    }
    UNSCOPED_INFO("the capture holds the 16-byte swizzle and 64-byte format tables");
    REQUIRE((swizzle.size() == 16 && formats.size() == 64));
    return { swizzle, formats };
}

IndexedBufferDrawInputs inputsOf(const std::filesystem::path& directory, const toml::table& table, const std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>& tables)
{
    const auto                   record = toml::node_view<const toml::node>(table);
    IndexedBufferDrawInputs      inputs;
    std::array<std::uint32_t, 8> r{};
    for (std::size_t i = 0; i < r.size(); ++i)
        r[i] = address(record["arguments_r3_r10"][i]);
    inputs.sceneCandidate = true;
    inputs.inputsComplete = true;
    inputs.primitive      = r[1];
    inputs.baseVertex     = r[2];
    inputs.startIndex     = r[3];
    inputs.indexCount     = r[4];
    inputs.state          = read(directory / "state.be.bin");
    inputs.vertexCode     = read(directory / "vs_code.be.bin");
    // A vertex-only record (no pixel shader) is a depth-only draw.
    if (!record["vertex_only"].value_or(false))
        inputs.pixelCode = read(directory / "ps_code.be.bin");
    inputs.vertexObject       = address(record["vertex_object"]);
    inputs.pixelObject        = address(record["pixel_object"]);
    inputs.declarationAddress = address(record["declaration_address"]);
    inputs.vertexHeader       = hexBytes(record["vertex_header"].value_or(std::string()));
    inputs.vertexMetadata     = hexBytes(record["vertex_metadata"].value_or(std::string()));
    inputs.pixelHeader        = hexBytes(record["pixel_header"].value_or(std::string()));
    inputs.pixelMetadata      = hexBytes(record["pixel_metadata"].value_or(std::string()));
    inputs.fetchRecords       = hexBytes(record["fetch_records"].value_or(std::string()));
    inputs.fetchTemplates     = hexBytes(record["fetch_templates"].value_or(std::string()));
    inputs.declarationHeader  = hexBytes(record["declaration_header"].value_or(std::string()));
    inputs.declaration        = hexBytes(record["declaration"].value_or(std::string()));
    inputs.swizzleTable       = tables.first;
    inputs.formatTable        = tables.second;
    literals(record["vertex_literals"], inputs.vertexLiteralRanges, inputs.vertexLiteralRegisters);
    literals(record["pixel_literals"], inputs.pixelLiteralRanges, inputs.pixelLiteralRegisters);
    // Zero 16-bit big-endian indices name vertex 0 alone.
    inputs.indexFormat = 0;
    inputs.indexEndian = 1;
    inputs.indexBytes.assign(std::size_t(inputs.indexCount) * 2, 0);
    inputs.streams = { 0 };
    inputs.streamFetches.push_back({ word(inputs.state, 0x778), word(inputs.state, 0x77C) });
    // Vertex fetch 95 spans its dword count from the shadow.
    inputs.streamBytes.emplace_back(std::size_t((inputs.streamFetches[0][1] >> 2) & 0xFFFFFF) * 4);
    if (const auto* textures = record["pixel_textures"].as_array())
        for (const auto& node : *textures)
        {
            const auto texture = toml::node_view<const toml::node>(node);
            UNSCOPED_INFO(directory.string() + " texture slots in order");
            REQUIRE(std::size_t(texture["slot"].value_or<std::int64_t>(-1)) == inputs.textureFetches.size());
            auto& fetch = inputs.textureFetches.emplace_back();
            for (std::size_t i = 0; i < fetch.size(); ++i)
                fetch[i] = address(texture["words"][i]);
            auto&       ranges = inputs.textureRanges.emplace_back();
            std::string error;
            // A fetch without a decodable range keeps empty bytes; the builder names it.
            if (GetNativeTextureMemoryRanges(fetch, ranges, error))
            {
                inputs.textureBaseBytes.emplace_back(ranges.base.size);
                inputs.textureMipBytes.emplace_back(ranges.mips.size);
            }
            else
            {
                inputs.textureBaseBytes.emplace_back();
                inputs.textureMipBytes.emplace_back();
            }
        }
    // The reader's fetch 16 follows the pixel slots.
    if (const auto* textures = record["vertex_textures"].as_array())
        for (const auto& node : *textures)
        {
            const auto texture = toml::node_view<const toml::node>(node);
            UNSCOPED_INFO(directory.string() + " vertex texture slot 16");
            REQUIRE(texture["slot"].value_or<std::int64_t>(-1) == 16);
            auto& fetch = inputs.textureFetches.emplace_back();
            for (std::size_t i = 0; i < fetch.size(); ++i)
                fetch[i] = address(texture["words"][i]);
            auto&       ranges = inputs.textureRanges.emplace_back();
            std::string error;
            const bool  addressed = GetNativeTextureMemoryRanges(fetch, ranges, error);
            inputs.textureBaseBytes.emplace_back(addressed ? ranges.base.size : 0);
            inputs.textureMipBytes.emplace_back(addressed ? ranges.mips.size : 0);
            inputs.vertexTextureFetch = true;
        }
    inputs.arguments = { r[0], r[1], 0, 0, r[4], 0, 0, 0, std::uint32_t(inputs.state.at(0x30F0)) * 4, address(record["caller"]) };
    return inputs;
}

// Refusals a pair drawn in a table form may still carry: a texture format
// the family does not sample.
bool knownGap(const std::string& code)
{
    return code.starts_with("scene_texture_format ");
}

// Captured with the direct fold/fetch-patch implementation in 3129fb65.
// State word digests distinguish records with the same shader/caller name.
// Catalog sizes identify the five captured catalog revisions.
struct CaptureBaseline
{
    std::string_view name, stateDigest, catalogs;
    bool             built;
    std::size_t      pairSize;
    std::ptrdiff_t   entry;
    std::string_view routingDigest, refusal;
};

constexpr CaptureBaseline kCaptureBaselines[]{
    { "8230686C-233A47EBAED69A97-243D0AB207824289", "09bf4d4068a5817d99efceee6b764a974cce704f4b2a0557a636b1bb5fc1802f", ",169,208,210,226,238,", true, 2, 0, "f68b8a5d7ffd9c9a7fe142c27258306e76a64812f3ada8063a0dd4edd21291d7", "" },
    { "8230686C-233A47EBAED69A97-243D0AB207824289", "42a05b85088b546f850a395c1d49574a6d28d841ea8ab068949b3dfe5d5bc403", ",169,208,210,226,238,", true, 2, 0, "f68b8a5d7ffd9c9a7fe142c27258306e76a64812f3ada8063a0dd4edd21291d7", "" },
    { "8230686C-233A47EBAED69A97-243D0AB207824289-74788BC7", "0cf45457e34f7d3ecb27eb211b77307df1049b59a32b9ac306b1ec87e4b6b360", ",169,208,210,226,238,", true, 2, 0, "f68b8a5d7ffd9c9a7fe142c27258306e76a64812f3ada8063a0dd4edd21291d7", "" },
    { "8230686C-233A47EBAED69A97-243D0AB207824289-74788BC7", "2764472ec740a18c372e04c1066db99d447dd9e5d8f8f4f01b1a32b11c1e9a1e", ",169,208,210,226,238,", true, 2, 0, "f68b8a5d7ffd9c9a7fe142c27258306e76a64812f3ada8063a0dd4edd21291d7", "" },
    { "8230686C-C8D900E1F00F9A47-243D0AB207824289", "1c306f36e3e3f8871aeb9f8b8e2d3f51f0d71bad0122fa9a6945611127376b36", ",169,208,210,226,238,", true, 2, 0, "34b5fd4e7ca918a1e2ce62c1e611adfb24ad51f4bfb89c092e57f2cf2deda1dc", "" },
    { "8230686C-C8D900E1F00F9A47-243D0AB207824289", "a7726164bb523fbc21f8ae14a684a62eaa08ecca0ec415027102a5daa27f8cf2", ",169,208,210,226,238,", true, 2, 0, "34b5fd4e7ca918a1e2ce62c1e611adfb24ad51f4bfb89c092e57f2cf2deda1dc", "" },
    { "8230686C-C8D900E1F00F9A47-243D0AB207824289-63A25D50", "15df4c57bfe55980b8987b2753f9082349f8a4b71766fe1ee5de44b9b68d033f", ",169,208,210,226,238,", true, 2, 0, "34b5fd4e7ca918a1e2ce62c1e611adfb24ad51f4bfb89c092e57f2cf2deda1dc", "" },
    { "8230686C-C8D900E1F00F9A47-243D0AB207824289-63A25D50", "d55b33b76a3fc5cd7f7ea73f398a133713abb4f82e79d144ceaabbdbfcd6eaf4", ",169,208,210,226,238,", true, 2, 0, "34b5fd4e7ca918a1e2ce62c1e611adfb24ad51f4bfb89c092e57f2cf2deda1dc", "" },
    { "8230686C-F65866A40B248418-F6CF41BEA360546B", "54eafdd2f48c03505b6d9963b4bbe101c682883bb8e2955a95ed3908c907d2ab", ",169,208,210,226,238,", true, 2, 0, "cfca7274f8c626afecd8855971e06b0a4544db65a8044ba04cb09513b92deb89", "" },
    { "8230686C-F65866A40B248418-F6CF41BEA360546B", "68a2aec1f158e49c74db0ca03c95b206f8b38077344de7999923050e8be7bd51", ",169,208,210,226,238,", true, 2, 0, "cfca7274f8c626afecd8855971e06b0a4544db65a8044ba04cb09513b92deb89", "" },
    { "8230686C-F65866A40B248418-F6CF41BEA360546B-0F7A46D5", "05036361796d30b43c93630a4ba13d61723102a943508be00b873a047ab9e303", ",169,208,210,226,238,", true, 2, 0, "cfca7274f8c626afecd8855971e06b0a4544db65a8044ba04cb09513b92deb89", "" },
    { "8230686C-F65866A40B248418-F6CF41BEA360546B-0F7A46D5", "0fb997a73b24f1e8221cb71168d0c267d76547e712cbbc2724448ede2be47467", ",169,208,210,226,238,", true, 2, 0, "cfca7274f8c626afecd8855971e06b0a4544db65a8044ba04cb09513b92deb89", "" },
    { "8250C768-03D23D9DBC9317EA-44AFBBA4A831EAF7", "51e1c2b9cba2411261f1960a69d38cb486f17e4d90106b81f5f41c65987be4f5", ",169,208,210,226,238,", true, 2, 1, "fa8578a9462b7dc92e484748c1b3698eba8988cbb37315b63e76ac44b5949773", "" },
    { "8250C768-03D23D9DBC9317EA-44AFBBA4A831EAF7", "ae87e388ddcd9a9dac598115c51c612b37b41462e6fe90714d0f32c163e80078", ",169,208,210,226,238,", true, 2, 1, "fa8578a9462b7dc92e484748c1b3698eba8988cbb37315b63e76ac44b5949773", "" },
    { "8250C768-03D23D9DBC9317EA-44AFBBA4A831EAF7-D5D1DA61", "0d7a93f0be42bf77d75b93acfca164e26d12f49636b0d5b3611b56e0c8c8a69d", ",169,208,210,226,238,", true, 2, 1, "fa8578a9462b7dc92e484748c1b3698eba8988cbb37315b63e76ac44b5949773", "" },
    { "8250C768-03D23D9DBC9317EA-44AFBBA4A831EAF7-D5D1DA61", "c05146da8c65f772bd05fbcd5e8c7e04447d5ef0f0852a5134a811271fdaf403", ",169,208,210,226,238,", true, 2, 1, "fa8578a9462b7dc92e484748c1b3698eba8988cbb37315b63e76ac44b5949773", "" },
    { "8250C768-0830F4D5076D94E3-6091C8E2D66D0943", "5131e797368c9b816d3b941698b1795c46bb22c2355782323266e3ea7a6d7d06", ",169,208,210,", true, 2, 0, "530a102ec5ed24f39627fda768a5955e82cc411122da83f26616c6a0a808abaf", "" },
    { "8250C768-0830F4D5076D94E3-6091C8E2D66D0943", "5131e797368c9b816d3b941698b1795c46bb22c2355782323266e3ea7a6d7d06", ",226,238,", true, 2, 0, "847a0fcf32d6b4e573daffe8f54d1bd8ed7b5ddba0ebfef4e5775a974cfb4711", "" },
    { "8250C768-0830F4D5076D94E3-6091C8E2D66D0943", "e748094494237ba42765e98d6ad4126ffd3a4356668b508c7d59647dac7af415", ",169,208,210,", true, 2, 0, "530a102ec5ed24f39627fda768a5955e82cc411122da83f26616c6a0a808abaf", "" },
    { "8250C768-0830F4D5076D94E3-6091C8E2D66D0943", "e748094494237ba42765e98d6ad4126ffd3a4356668b508c7d59647dac7af415", ",226,238,", true, 2, 0, "847a0fcf32d6b4e573daffe8f54d1bd8ed7b5ddba0ebfef4e5775a974cfb4711", "" },
    { "8250C768-0830F4D5076D94E3-6091C8E2D66D0943-74C5C46D", "3b51c7b08357d5365074dadff89dda6a94ce57898b658d98f68bacc8172e67e7", ",169,208,210,", true, 2, 0, "530a102ec5ed24f39627fda768a5955e82cc411122da83f26616c6a0a808abaf", "" },
    { "8250C768-0830F4D5076D94E3-6091C8E2D66D0943-74C5C46D", "3b51c7b08357d5365074dadff89dda6a94ce57898b658d98f68bacc8172e67e7", ",226,238,", true, 2, 0, "847a0fcf32d6b4e573daffe8f54d1bd8ed7b5ddba0ebfef4e5775a974cfb4711", "" },
    { "8250C768-0830F4D5076D94E3-6091C8E2D66D0943-74C5C46D", "a20fe7999e41c37a1cba4d985f1ee8181eb9e572929d4f8000fbf6770d7dff25", ",169,208,210,", true, 2, 0, "530a102ec5ed24f39627fda768a5955e82cc411122da83f26616c6a0a808abaf", "" },
    { "8250C768-0830F4D5076D94E3-6091C8E2D66D0943-74C5C46D", "a20fe7999e41c37a1cba4d985f1ee8181eb9e572929d4f8000fbf6770d7dff25", ",226,238,", true, 2, 0, "847a0fcf32d6b4e573daffe8f54d1bd8ed7b5ddba0ebfef4e5775a974cfb4711", "" },
    { "8250C768-0830F4D5076D94E3-6091C8E2D66D0943-80A65BAF", "2d5ef08b61f29402ee383b5bbae31bc78b9411d9c4dd4211b953f41d28c1ee2c", ",169,208,210,", true, 2, 1, "530a102ec5ed24f39627fda768a5955e82cc411122da83f26616c6a0a808abaf", "" },
    { "8250C768-0830F4D5076D94E3-6091C8E2D66D0943-80A65BAF", "2d5ef08b61f29402ee383b5bbae31bc78b9411d9c4dd4211b953f41d28c1ee2c", ",226,238,", true, 2, 1, "847a0fcf32d6b4e573daffe8f54d1bd8ed7b5ddba0ebfef4e5775a974cfb4711", "" },
    { "8250C768-0830F4D5076D94E3-6091C8E2D66D0943-80A65BAF", "e492734de9c6d72451bc968c45945955e0416e2c222b5852e11ea5c8fb1e7b32", ",169,208,210,", true, 2, 1, "530a102ec5ed24f39627fda768a5955e82cc411122da83f26616c6a0a808abaf", "" },
    { "8250C768-0830F4D5076D94E3-6091C8E2D66D0943-80A65BAF", "e492734de9c6d72451bc968c45945955e0416e2c222b5852e11ea5c8fb1e7b32", ",226,238,", true, 2, 1, "847a0fcf32d6b4e573daffe8f54d1bd8ed7b5ddba0ebfef4e5775a974cfb4711", "" },
    { "8250C768-0830F4D5076D94E3-6091C8E2D66D0943-9B756214", "0d3f3fcefe28851f73c42ddfe3eb7069dc48a65355d2a331e93497cecb514629", ",169,208,210,", true, 2, 1, "530a102ec5ed24f39627fda768a5955e82cc411122da83f26616c6a0a808abaf", "" },
    { "8250C768-0830F4D5076D94E3-6091C8E2D66D0943-9B756214", "0d3f3fcefe28851f73c42ddfe3eb7069dc48a65355d2a331e93497cecb514629", ",226,238,", true, 2, 1, "847a0fcf32d6b4e573daffe8f54d1bd8ed7b5ddba0ebfef4e5775a974cfb4711", "" },
    { "8250C768-0B4B10685C1CE8B2-CE177871D3ABC6AE", "b8dcd0f3a532d992bb76d26054fc84104fe87220449521f3beb325debd186cab", ",169,208,210,226,238,", true, 2, 1, "c712e693568e8f760698d191d08f76f61447339bac1a74fbd36765e02b5cb247", "" },
    { "8250C768-0B4B10685C1CE8B2-CE177871D3ABC6AE", "e2ec1d7eb821b5fca3155110b129f83855b68f5c3741944e4783d8ef4e35bf41", ",169,208,210,226,238,", true, 2, 1, "c712e693568e8f760698d191d08f76f61447339bac1a74fbd36765e02b5cb247", "" },
    { "8250C768-0B4B10685C1CE8B2-CE177871D3ABC6AE-1135A0D5", "6a177d6a1176dd96e2ef5034876baa93ff54e2779e2a6cd263165e41b5301bd1", ",169,208,210,226,238,", true, 2, 1, "c712e693568e8f760698d191d08f76f61447339bac1a74fbd36765e02b5cb247", "" },
    { "8250C768-0B4B10685C1CE8B2-CE177871D3ABC6AE-1135A0D5", "a1e4c84ceb6e4288836c0352bc6d4187163d34a9c547127f2e05f66b135be653", ",169,208,210,226,238,", true, 2, 1, "c712e693568e8f760698d191d08f76f61447339bac1a74fbd36765e02b5cb247", "" },
    { "8250C768-0BB53F1E922635DB-CE177871D3ABC6AE", "bcefcd3d9e44bb9817b70d9c2b98c94a3a58140ecd7edd571258b0a00304ae17", ",169,208,210,226,238,", true, 2, 0, "469b5761f50c336c6025686118aba2e617f19560aa1a62b1ea84e30f47c1eaea", "" },
    { "8250C768-0C5787BE54255AE3-9D1270EA1688C778", "4a4db8660b5d3ea3b24eaced51544dd298c5058ee51f96531dffc5e3fee8df73", ",169,208,210,", true, 2, 1, "a48bd11f1fcab64710a8e90f7a1c4dba0be965ad203f66193383ece9cade4a4e", "" },
    { "8250C768-0C5787BE54255AE3-9D1270EA1688C778", "4a4db8660b5d3ea3b24eaced51544dd298c5058ee51f96531dffc5e3fee8df73", ",226,238,", true, 2, 1, "ebd83b6ec5e0bed76c7f913330725d7daee273a938035e997fe5f7912ee61fbb", "" },
    { "8250C768-0C5787BE54255AE3-9D1270EA1688C778", "ea7ea708ddf64b01534924ededcc31205870a0cf1ce8c3a307ac2404de756395", ",169,208,210,", true, 2, 1, "a48bd11f1fcab64710a8e90f7a1c4dba0be965ad203f66193383ece9cade4a4e", "" },
    { "8250C768-0C5787BE54255AE3-9D1270EA1688C778", "ea7ea708ddf64b01534924ededcc31205870a0cf1ce8c3a307ac2404de756395", ",226,238,", true, 2, 1, "ebd83b6ec5e0bed76c7f913330725d7daee273a938035e997fe5f7912ee61fbb", "" },
    { "8250C768-0C5787BE54255AE3-9D1270EA1688C778-2CEE3B43", "4527447f45e4183d25421daf8c881487228bcc0071c66e0b09b1c0c8133884ed", ",169,208,210,", true, 2, 1, "a48bd11f1fcab64710a8e90f7a1c4dba0be965ad203f66193383ece9cade4a4e", "" },
    { "8250C768-0C5787BE54255AE3-9D1270EA1688C778-2CEE3B43", "4527447f45e4183d25421daf8c881487228bcc0071c66e0b09b1c0c8133884ed", ",226,238,", true, 2, 1, "ebd83b6ec5e0bed76c7f913330725d7daee273a938035e997fe5f7912ee61fbb", "" },
    { "8250C768-0C5787BE54255AE3-9D1270EA1688C778-2CEE3B43", "62594dc9f7d2d0a0f1ee25def5b01ffb5b17b8d3088249eaac888c2dc69d1517", ",169,208,210,", true, 2, 1, "a48bd11f1fcab64710a8e90f7a1c4dba0be965ad203f66193383ece9cade4a4e", "" },
    { "8250C768-0C5787BE54255AE3-9D1270EA1688C778-2CEE3B43", "62594dc9f7d2d0a0f1ee25def5b01ffb5b17b8d3088249eaac888c2dc69d1517", ",226,238,", true, 2, 1, "ebd83b6ec5e0bed76c7f913330725d7daee273a938035e997fe5f7912ee61fbb", "" },
    { "8250C768-0C5787BE54255AE3-9D1270EA1688C778-3AE13DF3", "295f2b7c8c011cfaa1264aeeb24a0701879ad6420d995ed21c7d747c0086684b", ",169,208,210,", true, 2, 0, "a48bd11f1fcab64710a8e90f7a1c4dba0be965ad203f66193383ece9cade4a4e", "" },
    { "8250C768-0C5787BE54255AE3-9D1270EA1688C778-3AE13DF3", "295f2b7c8c011cfaa1264aeeb24a0701879ad6420d995ed21c7d747c0086684b", ",226,238,", true, 2, 0, "ebd83b6ec5e0bed76c7f913330725d7daee273a938035e997fe5f7912ee61fbb", "" },
    { "8250C768-0C5787BE54255AE3-9D1270EA1688C778-3AE13DF3", "76a69fdf93c8c0507f8dd20204425469982b2bbba87c393a2ad72d5f1b24880e", ",169,208,210,", true, 2, 0, "a48bd11f1fcab64710a8e90f7a1c4dba0be965ad203f66193383ece9cade4a4e", "" },
    { "8250C768-0C5787BE54255AE3-9D1270EA1688C778-3AE13DF3", "76a69fdf93c8c0507f8dd20204425469982b2bbba87c393a2ad72d5f1b24880e", ",226,238,", true, 2, 0, "ebd83b6ec5e0bed76c7f913330725d7daee273a938035e997fe5f7912ee61fbb", "" },
    { "8250C768-0E4D07171517188F-EEE151C03C1B6093-BB68585F", "51745714ea8036e5f71772587ecba06c527c6e479e5c5b487cbc2b465c2dcb75", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-0E4D07171517188F-EEE151C03C1B6093-BB68585F", "51745714ea8036e5f71772587ecba06c527c6e479e5c5b487cbc2b465c2dcb75", ",226,238,", true, 2, 1, "dfca7e274442d4a967c2e00ae10bc9e21e4339f3bafc04578964b4db802b99c3", "" },
    { "8250C768-0ECFF8F50E06C277-4D5283C0E22D38BB", "02a756f1bf3de36d5a693a78a852d7e1f23a730dfb64b78c33f6bdac82dd93e0", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-0ECFF8F50E06C277-4D5283C0E22D38BB", "02a756f1bf3de36d5a693a78a852d7e1f23a730dfb64b78c33f6bdac82dd93e0", ",226,238,", true, 2, 0, "ac4d33639e48820e5b4ab30e1ac704a138cf3602dceb4ecb8a7f69e3d6807172", "" },
    { "8250C768-0ECFF8F50E06C277-4D5283C0E22D38BB-B4EFE5BC", "2faeb391a01e1b20c114ec859c09c82228320220188788bdae1af644a587e124", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-0ECFF8F50E06C277-4D5283C0E22D38BB-B4EFE5BC", "2faeb391a01e1b20c114ec859c09c82228320220188788bdae1af644a587e124", ",226,238,", true, 2, 0, "ac4d33639e48820e5b4ab30e1ac704a138cf3602dceb4ecb8a7f69e3d6807172", "" },
    { "8250C768-0ECFF8F50E06C277-4D5283C0E22D38BB-B4EFE5BC", "512c08c2247b256c5a454432c1b75e53acedf9866f16fc502fbeb79d5ead87c2", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-0ECFF8F50E06C277-4D5283C0E22D38BB-B4EFE5BC", "512c08c2247b256c5a454432c1b75e53acedf9866f16fc502fbeb79d5ead87c2", ",226,238,", true, 2, 0, "ac4d33639e48820e5b4ab30e1ac704a138cf3602dceb4ecb8a7f69e3d6807172", "" },
    { "8250C768-121F1F3E6D8BFB91-92FD6D98A1BA57AC", "2fb17174667f22fcc0a2548ea9f8818637bb386088eed1bc7ce2209dfa79a85c", ",169,208,210,226,238,", true, 2, 0, "259ee64f317d35af71eba1649b4b4f0b26a19fc2a9030b6e7251cfc835c1e040", "" },
    { "8250C768-121F1F3E6D8BFB91-92FD6D98A1BA57AC", "a0d2c21e1c18dd68ce4e0eb6ba9e48824295bbdd24163d48cee6fb987fe12bec", ",169,208,210,226,238,", true, 2, 0, "259ee64f317d35af71eba1649b4b4f0b26a19fc2a9030b6e7251cfc835c1e040", "" },
    { "8250C768-121F1F3E6D8BFB91-92FD6D98A1BA57AC-500DAEAE", "9bfac41cc42891adde68325c29fd550b0688fe41734afe97944fae66d2bf31cc", ",169,208,210,226,238,", true, 2, 0, "259ee64f317d35af71eba1649b4b4f0b26a19fc2a9030b6e7251cfc835c1e040", "" },
    { "8250C768-121F1F3E6D8BFB91-92FD6D98A1BA57AC-500DAEAE", "f12d343e61161432ef1b4b59bbf0b1a853e9b05a52a501323308cb50c001bf0e", ",169,208,210,226,238,", true, 2, 0, "259ee64f317d35af71eba1649b4b4f0b26a19fc2a9030b6e7251cfc835c1e040", "" },
    { "8250C768-1359AC4984885FB3-6174F2CAABC2C726", "a0d9abb57905185c6026a0b9b04eefa873e9647d24d71765be6809ac14d80236", ",169,208,210,226,238,", true, 2, 0, "8f872f6a07da899057dba577707278d61eea2c36ced9b53633ed8ae147e08e22", "" },
    { "8250C768-1359AC4984885FB3-6174F2CAABC2C726", "bd8a5eb02d1b1aa7a46fbd30f24f3cca621f1be87af6341d99e43db5927672da", ",169,208,210,226,238,", true, 2, 0, "8f872f6a07da899057dba577707278d61eea2c36ced9b53633ed8ae147e08e22", "" },
    { "8250C768-1359AC4984885FB3-6174F2CAABC2C726-18AF8C60", "09ec487f4cf762efeb09455d6941f734036b7cce47d6c954f3c811af52530662", ",169,208,210,226,238,", true, 2, 0, "8f872f6a07da899057dba577707278d61eea2c36ced9b53633ed8ae147e08e22", "" },
    { "8250C768-1359AC4984885FB3-6174F2CAABC2C726-18AF8C60", "50955e0176edcf2a13ec74ab3940ee4507ff955b0a88076d32a4110c1793a365", ",169,208,210,226,238,", true, 2, 0, "8f872f6a07da899057dba577707278d61eea2c36ced9b53633ed8ae147e08e22", "" },
    { "8250C768-1B1444A0B0485DDF-79CE4E74AA45F3FF-DBE8ACD6", "6ad850822fa9f4d6e3769b577bc4f884338814dde40c99325319cb8367ce99f2", ",169,", false, 0, -1, "", "" },
    { "8250C768-1B1444A0B0485DDF-79CE4E74AA45F3FF-DBE8ACD6", "6ad850822fa9f4d6e3769b577bc4f884338814dde40c99325319cb8367ce99f2", ",226,238,", true, 2, 1, "5c453ec9dff092f6b87fce5f174a314604447f808902f10cf37c262dfee8d11a", "" },
    { "8250C768-1B1444A0B0485DDF-79CE4E74AA45F3FF-DBE8ACD6", "6ad850822fa9f4d6e3769b577bc4f884338814dde40c99325319cb8367ce99f2", ",208,210,", true, 2, 1, "91049474d18bd4b55fa71c7dde2b8ae1804152109bc0b058d856e174bfffb07e", "" },
    { "8250C768-24688FFB8E2D07B7-CD99817770DB94B6", "676b9e9bcfe602da6d223db71b4723cd817cc7d851d2dfb6aa15b8b1d7ee9d84", ",169,208,210,226,238,", true, 2, 1, "eb439e5c69ba68e09a98869565f94ce6cdadd8d7757cdb47a7e161783ce3ad77", "" },
    { "8250C768-24688FFB8E2D07B7-CD99817770DB94B6-3BD42884", "0733eb6f4d21e75fe31ae61e91da9d8cc06374928cff70caad97004cfcdf1e12", ",169,208,210,226,238,", true, 2, 1, "eb439e5c69ba68e09a98869565f94ce6cdadd8d7757cdb47a7e161783ce3ad77", "" },
    { "8250C768-24C20DA03C14CBD4-9DE14D48B5AD8B56-22CCA360", "015b2279d2d6c7f9f370d89043ba507fa3dfea7e765bc2cd000d6f74acc5ecdf", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-24C20DA03C14CBD4-9DE14D48B5AD8B56-22CCA360", "015b2279d2d6c7f9f370d89043ba507fa3dfea7e765bc2cd000d6f74acc5ecdf", ",226,238,", true, 2, 0, "8a92f9f2345452270f31b49817ff6439900657644483d60168bf10c70d664716", "" },
    { "8250C768-2583D2D48AC48C9E-CE177871D3ABC6AE", "e342ec9dde88ef3a2c3894b1075a2cc47522950ae39275a90b63d40604180bb9", ",169,208,210,226,238,", true, 2, 1, "f48a291cfd06e10ef65d332ac729c3f9d5457a54fab8a9650165a843ec59bdca", "" },
    { "8250C768-2583D2D48AC48C9E-CE177871D3ABC6AE-27D28D3C", "b47dd69d09707fc031181f612fdc9aeac94469a0f50a0c302ae221a6a8259bbd", ",169,208,210,226,238,", true, 2, 1, "f48a291cfd06e10ef65d332ac729c3f9d5457a54fab8a9650165a843ec59bdca", "" },
    { "8250C768-289B3AEDEC76A40F-0000000000000000-C9D1EBC7", "197c3e67a9f13ee69c6e1ae98b4d96ddb69e591074fc00a1cbbaa12e59f91e0f", ",169,208,210,226,238,", true, 1, 0, "9406383ca9ac521c2b9610e2dfecec25ea3d3e813161b7aa84464f5a5c36c5c6", "" },
    { "8250C768-289B3AEDEC76A40F-0000000000000000-C9D1EBC7", "86c53f0152f93f48d91ada9bed7fd2eaa164fcb59e41464486eed551e1ace176", ",169,208,210,226,238,", true, 1, 0, "9406383ca9ac521c2b9610e2dfecec25ea3d3e813161b7aa84464f5a5c36c5c6", "" },
    { "8250C768-289B3AEDEC76A40F-0000000000000000-F3BF1896", "218cc479d3a44463282b344a8d896dd3b8129f4a0d9d6b7a7fde4d6e18f2e8ec", ",169,208,210,226,238,", true, 1, 0, "9406383ca9ac521c2b9610e2dfecec25ea3d3e813161b7aa84464f5a5c36c5c6", "" },
    { "8250C768-30A5D43A5285CCB8-B8F799D8F8A6A577", "a6a2c5f51a23afd93f7ade3740ae461f5e2466c07954475df15799329ab3d7a5", ",169,208,210,226,238,", true, 2, 0, "20b02c0de16f9ed29eccda2a14ed8ee41ecd8d17a3045423fe101ea74c99493c", "" },
    { "8250C768-30A5D43A5285CCB8-B8F799D8F8A6A577", "ef3416a93bc0f941ce18628335182ea38ac04ea301763e75e9355bb755667934", ",169,208,210,226,238,", true, 2, 0, "20b02c0de16f9ed29eccda2a14ed8ee41ecd8d17a3045423fe101ea74c99493c", "" },
    { "8250C768-30A5D43A5285CCB8-B8F799D8F8A6A577-4C6386C8", "74695ed29d90dae8ea3f7279d8a996ac1f44ccd361872955d8ef4843262e5b1b", ",169,208,210,226,238,", true, 2, 0, "20b02c0de16f9ed29eccda2a14ed8ee41ecd8d17a3045423fe101ea74c99493c", "" },
    { "8250C768-30A5D43A5285CCB8-B8F799D8F8A6A577-4C6386C8", "79367db0affbf55f0b00ba845f7f0103b9679e45a0f0326dc694fb883940025a", ",169,208,210,226,238,", true, 2, 0, "20b02c0de16f9ed29eccda2a14ed8ee41ecd8d17a3045423fe101ea74c99493c", "" },
    { "8250C768-3418D1CAA13CAF50-1262103F2F06927F", "0fd17c053e60b42df67e1a79e6d881b40db0675b2bb52aa55ac77de59d9433d6", ",169,208,210,", true, 2, 0, "a6ed028e127c990e1fa67f5fe247a11c86acc637bddcd96a3466332d3d1b438c", "" },
    { "8250C768-3418D1CAA13CAF50-1262103F2F06927F", "0fd17c053e60b42df67e1a79e6d881b40db0675b2bb52aa55ac77de59d9433d6", ",226,238,", true, 2, 0, "cfcf5d90564405da9d608decc25e7f8a74344a8d975a69373fc37d5d12ab5a53", "" },
    { "8250C768-3418D1CAA13CAF50-1262103F2F06927F", "95790da445ef029f15ebbe966d8216acedda80c720c526ee7fb8d5adf5845690", ",169,208,210,", true, 2, 0, "a6ed028e127c990e1fa67f5fe247a11c86acc637bddcd96a3466332d3d1b438c", "" },
    { "8250C768-3418D1CAA13CAF50-1262103F2F06927F", "95790da445ef029f15ebbe966d8216acedda80c720c526ee7fb8d5adf5845690", ",226,238,", true, 2, 0, "cfcf5d90564405da9d608decc25e7f8a74344a8d975a69373fc37d5d12ab5a53", "" },
    { "8250C768-3418D1CAA13CAF50-1262103F2F06927F-694FDD64", "0fddda8f5291381aa7b49fad47e280185cf9c8477140547452f7cfdef3883f1e", ",169,208,210,", true, 2, 0, "a6ed028e127c990e1fa67f5fe247a11c86acc637bddcd96a3466332d3d1b438c", "" },
    { "8250C768-3418D1CAA13CAF50-1262103F2F06927F-694FDD64", "0fddda8f5291381aa7b49fad47e280185cf9c8477140547452f7cfdef3883f1e", ",226,238,", true, 2, 0, "cfcf5d90564405da9d608decc25e7f8a74344a8d975a69373fc37d5d12ab5a53", "" },
    { "8250C768-3418D1CAA13CAF50-1262103F2F06927F-694FDD64", "d278bf3ddb813c416bd09d0b9cb90091d875596665b0c8dd96c956df1ef4fb27", ",169,208,210,", true, 2, 0, "a6ed028e127c990e1fa67f5fe247a11c86acc637bddcd96a3466332d3d1b438c", "" },
    { "8250C768-3418D1CAA13CAF50-1262103F2F06927F-694FDD64", "d278bf3ddb813c416bd09d0b9cb90091d875596665b0c8dd96c956df1ef4fb27", ",226,238,", true, 2, 0, "cfcf5d90564405da9d608decc25e7f8a74344a8d975a69373fc37d5d12ab5a53", "" },
    { "8250C768-3648CFB8645444C7-0B8225BDBB2FEE74", "3d6cc90b67d22cd816b0b5b9ececd5ee4c875e14dbdb299caa7107de7cade998", ",169,208,210,226,238,", false, 2, -1, "40595f970219e2a775a3e3416fd4eccf02071cb300ec34086d6766c3307b55d3", "scene_target_form: 0x70101e0 0x0 0x5e4 0x2d00500" },
    { "8250C768-3648CFB8645444C7-0B8225BDBB2FEE74", "5f772665bbbf7b40710fd43c162bb4a8ef199e3456deb3a3fd30edbc86399a25", ",169,208,210,226,238,", false, 2, -1, "40595f970219e2a775a3e3416fd4eccf02071cb300ec34086d6766c3307b55d3", "scene_target_form: 0x70101e0 0x0 0x5e4 0x2d00500" },
    { "8250C768-3648CFB8645444C7-0B8225BDBB2FEE74", "b53dfe1a2da082045fbb21279959224a0dc2d5177dd661411f9760a83a8fe272", ",169,208,210,226,238,", false, 2, -1, "40595f970219e2a775a3e3416fd4eccf02071cb300ec34086d6766c3307b55d3", "scene_target_form: 0x70101e0 0x0 0x5e4 0x2d00500" },
    { "8250C768-3648CFB8645444C7-0B8225BDBB2FEE74-BC89AAF9", "6c0d3acb65c8d24b5ac891a867a3c04ebf123fb5c1edbb1a0b81a9fdc9ab8c2d", ",169,208,210,226,238,", false, 2, -1, "40595f970219e2a775a3e3416fd4eccf02071cb300ec34086d6766c3307b55d3", "scene_target_form: 0x70101e0 0x0 0x5e4 0x2d00500" },
    { "8250C768-3648CFB8645444C7-0B8225BDBB2FEE74-BC89AAF9", "ccb788b06939edebd8ce94986c34a4a1663c685dc3a82ee101aec8f18ad3d6fd", ",169,208,210,226,238,", false, 2, -1, "40595f970219e2a775a3e3416fd4eccf02071cb300ec34086d6766c3307b55d3", "scene_target_form: 0x70101e0 0x0 0x5e4 0x2d00500" },
    { "8250C768-3697D7CA0AE23D3A-4E5DAB4565FD29CF", "57546f92d923926ef4d3d8df54f7d0c81adf128b967b3d833d721d897aa9e627", ",169,208,210,226,238,", true, 2, 1, "bad345c668c58ac65ff95c4681e6617faea6c23a0881b18d0cef8a5135ce45bd", "" },
    { "8250C768-3697D7CA0AE23D3A-4E5DAB4565FD29CF-8E337842", "1fc29c3e8186b48a6aee5d481b2752931332ecbce9dab4ca784b2cb418ad396b", ",169,208,210,226,238,", true, 2, 1, "bad345c668c58ac65ff95c4681e6617faea6c23a0881b18d0cef8a5135ce45bd", "" },
    { "8250C768-3697D7CA0AE23D3A-4E5DAB4565FD29CF-8E337842", "2e4fbd0d4e693b2b1f3654a09ad0f18bbf32b64d288c1a3608265e352b8e93b7", ",169,208,210,226,238,", true, 2, 1, "bad345c668c58ac65ff95c4681e6617faea6c23a0881b18d0cef8a5135ce45bd", "" },
    { "8250C768-37DADA2ECBF6BD1D-CE177871D3ABC6AE", "57c87a3f8713c2a08d19b897a6e4b8caee2b7ad81882f9aa1a92c56f7397e065", ",169,208,210,226,238,", true, 2, 1, "8e54db0d8e4129ba3b3c79255d3cb0647a02fcaba0213aab4de482412e4d182f", "" },
    { "8250C768-37DADA2ECBF6BD1D-CE177871D3ABC6AE", "975f5669761ad19554e6d73654a6307dd27a844aa5cef372cfa1261412261213", ",169,208,210,226,238,", true, 2, 1, "8e54db0d8e4129ba3b3c79255d3cb0647a02fcaba0213aab4de482412e4d182f", "" },
    { "8250C768-37DADA2ECBF6BD1D-CE177871D3ABC6AE-294B2704", "06fe600eead0332ca5c7a5faf7dad9e70d5d233f95a69231d970fa1371a01d83", ",169,208,210,226,238,", true, 2, 1, "8e54db0d8e4129ba3b3c79255d3cb0647a02fcaba0213aab4de482412e4d182f", "" },
    { "8250C768-37DADA2ECBF6BD1D-CE177871D3ABC6AE-294B2704", "deb34db7c2971eb6251cb17fc91d7b909b27236e3e770202eda2ed05bbd21d7c", ",169,208,210,226,238,", true, 2, 1, "8e54db0d8e4129ba3b3c79255d3cb0647a02fcaba0213aab4de482412e4d182f", "" },
    { "8250C768-38691F81BEA4AD28-B8C1B55C7CE2E502", "33bc3da5fc46240bc2afa196917530c13540bc48f9ec3c2efbd567f9df3ba9e3", ",169,208,210,226,238,", true, 2, 0, "2dd033289c81b489e1052ac8945cdefdd8ff29574c9e1daac844b1a5653f8695", "" },
    { "8250C768-38691F81BEA4AD28-B8C1B55C7CE2E502", "d87a86488aa35b211b81854fec11bea6cbd4b475e5ad521ecf9a5ff449b7a74e", ",169,208,210,226,238,", true, 2, 0, "2dd033289c81b489e1052ac8945cdefdd8ff29574c9e1daac844b1a5653f8695", "" },
    { "8250C768-38691F81BEA4AD28-B8C1B55C7CE2E502-5A092B02", "8420d4f25428256271b40c9575547bfd802c2652a43033d722ae51d498ac153d", ",169,208,210,226,238,", true, 2, 0, "2dd033289c81b489e1052ac8945cdefdd8ff29574c9e1daac844b1a5653f8695", "" },
    { "8250C768-38691F81BEA4AD28-B8C1B55C7CE2E502-5A092B02", "e1180ef92bf534a717432f1ab120fe8bfddc9bf81c2f3667962ebc64728981e4", ",169,208,210,226,238,", true, 2, 0, "2dd033289c81b489e1052ac8945cdefdd8ff29574c9e1daac844b1a5653f8695", "" },
    { "8250C768-3E63F6D03B053096-CE177871D3ABC6AE", "c389e44639935bc84335d416514bfefa66df5d841ebca87598481754787cfe89", ",169,208,210,226,238,", true, 2, 0, "15630b2f3b0385a12e7cd0dee1c23884112672313834c9327a990ba9f67afb4e", "" },
    { "8250C768-3E63F6D03B053096-CE177871D3ABC6AE", "d57f8f3ed99c8ec6ba9882f8ae883adfe544730dd0e18f03645320c69b768a7f", ",169,208,210,226,238,", true, 2, 1, "15630b2f3b0385a12e7cd0dee1c23884112672313834c9327a990ba9f67afb4e", "" },
    { "8250C768-3E63F6D03B053096-CE177871D3ABC6AE-994E2C44", "f3d986d57cf82801c3b17f26a4e8d0d1e273c241b989a766dd7a868276947289", ",169,208,210,226,238,", true, 2, 0, "15630b2f3b0385a12e7cd0dee1c23884112672313834c9327a990ba9f67afb4e", "" },
    { "8250C768-3E63F6D03B053096-CE177871D3ABC6AE-E8C31543", "d96c48dcf906258be1867524e2c733d631869f7309d1cc723868cc63326bc948", ",169,208,210,226,238,", true, 2, 1, "15630b2f3b0385a12e7cd0dee1c23884112672313834c9327a990ba9f67afb4e", "" },
    { "8250C768-3E63F6D03B053096-CE177871D3ABC6AE-E8C31543", "e4b27009026967416b4ab1213805e0f0595ca103f4f9d32ee43ed0d6796a6c4a", ",169,208,210,226,238,", true, 2, 1, "15630b2f3b0385a12e7cd0dee1c23884112672313834c9327a990ba9f67afb4e", "" },
    { "8250C768-410718F3B9741E31-323C72B4BF8A37B7", "08225679bacc4b9628e8a1f79fa446c71ce145ff2f25b8699836b1b6b519e8fe", ",169,208,210,226,238,", true, 2, 0, "715d5b4d4a03da6b24800f15454189de4e38391ddb8a94f9cdcce7f20d4448d9", "" },
    { "8250C768-410718F3B9741E31-323C72B4BF8A37B7", "79ba6a77a252d333dfb1db0f2073744efa6ee5777cc5dabe1b005ff7360c0108", ",169,208,210,226,238,", true, 2, 0, "715d5b4d4a03da6b24800f15454189de4e38391ddb8a94f9cdcce7f20d4448d9", "" },
    { "8250C768-410718F3B9741E31-323C72B4BF8A37B7-0F3204AD", "0f19e17ca7e72508e1104dccab207977f42a377d90d3c2556bca81f389c09da3", ",169,208,210,226,238,", true, 2, 0, "715d5b4d4a03da6b24800f15454189de4e38391ddb8a94f9cdcce7f20d4448d9", "" },
    { "8250C768-410718F3B9741E31-323C72B4BF8A37B7-0F3204AD", "4d7c8d3d0b703bd5f618a3efbadc0b320f6eaec086098c47ae0249dbf51f56ba", ",169,208,210,226,238,", true, 2, 0, "715d5b4d4a03da6b24800f15454189de4e38391ddb8a94f9cdcce7f20d4448d9", "" },
    { "8250C768-410718F3B9741E31-323C72B4BF8A37B7-416F2F61", "623ad9e154f7070e36e6fca71ea87d8a88d3b6e0aa085453ffb072c98d05c40a", ",169,208,210,226,238,", true, 2, 0, "715d5b4d4a03da6b24800f15454189de4e38391ddb8a94f9cdcce7f20d4448d9", "" },
    { "8250C768-410718F3B9741E31-323C72B4BF8A37B7-416F2F61", "a6197395bf3dc22c317b61609641b80407ef8297a8f85eebb5391f21b22bfe2c", ",169,208,210,226,238,", true, 2, 0, "715d5b4d4a03da6b24800f15454189de4e38391ddb8a94f9cdcce7f20d4448d9", "" },
    { "8250C768-49CA06EF2450528E-46311EE61337FF62", "7e62f26d0abb02bae3d8cf40f64779d68ab757666cc470440f0a23fc438062ae", ",226,238,", true, 2, 1, "838e84ac585d3922317d8aa42de1ab4cc9b846522540f8c125d90ca6e2c4f739", "" },
    { "8250C768-49CA06EF2450528E-46311EE61337FF62", "7e62f26d0abb02bae3d8cf40f64779d68ab757666cc470440f0a23fc438062ae", ",169,208,210,", true, 2, 1, "923b3d32970eb59d883cd7555bb15c5907efb5f13c089432cc2b581e6c3091db", "" },
    { "8250C768-49CA06EF2450528E-46311EE61337FF62", "9d8cf41f6734edd003d44a442878991028e9bda27a204fa188ddfbf569817f8c", ",226,238,", true, 2, 1, "838e84ac585d3922317d8aa42de1ab4cc9b846522540f8c125d90ca6e2c4f739", "" },
    { "8250C768-49CA06EF2450528E-46311EE61337FF62", "9d8cf41f6734edd003d44a442878991028e9bda27a204fa188ddfbf569817f8c", ",169,208,210,", true, 2, 1, "923b3d32970eb59d883cd7555bb15c5907efb5f13c089432cc2b581e6c3091db", "" },
    { "8250C768-49CA06EF2450528E-46311EE61337FF62-F60566FF", "2f7f9a912515d6844f9ad2f5918f1386431cdb896010d3da759a52b20295d6d9", ",226,238,", true, 2, 1, "838e84ac585d3922317d8aa42de1ab4cc9b846522540f8c125d90ca6e2c4f739", "" },
    { "8250C768-49CA06EF2450528E-46311EE61337FF62-F60566FF", "2f7f9a912515d6844f9ad2f5918f1386431cdb896010d3da759a52b20295d6d9", ",169,208,210,", true, 2, 1, "923b3d32970eb59d883cd7555bb15c5907efb5f13c089432cc2b581e6c3091db", "" },
    { "8250C768-49CA06EF2450528E-46311EE61337FF62-F60566FF", "42cc0cd03bdaca86e84e8765c73b602fc0cb827eab820d4ddf8755e2cb61938d", ",226,238,", true, 2, 1, "838e84ac585d3922317d8aa42de1ab4cc9b846522540f8c125d90ca6e2c4f739", "" },
    { "8250C768-49CA06EF2450528E-46311EE61337FF62-F60566FF", "42cc0cd03bdaca86e84e8765c73b602fc0cb827eab820d4ddf8755e2cb61938d", ",169,208,210,", true, 2, 1, "923b3d32970eb59d883cd7555bb15c5907efb5f13c089432cc2b581e6c3091db", "" },
    { "8250C768-505017D02F731835-E8ACC856CED675F4", "b850b22e524874f91448a3d9fa7b51067a8354cb8949446412904109ce5493c6", ",226,238,", true, 2, 1, "9cd75a5c1d442a2c21760ad5b9c38afa183f163e67f64bbab3ba8b1793711823", "" },
    { "8250C768-505017D02F731835-E8ACC856CED675F4", "b850b22e524874f91448a3d9fa7b51067a8354cb8949446412904109ce5493c6", ",169,208,210,", true, 2, 1, "cd853fa88bacc53ad8e314d14bd13b97bb339b912e00a5883ca09a6950c8341c", "" },
    { "8250C768-505017D02F731835-E8ACC856CED675F4-03731EBB", "1e51e57fafbece67aa6f160240d4f01eb6512d872abef92356627458ba92f05b", ",226,238,", true, 2, 1, "9cd75a5c1d442a2c21760ad5b9c38afa183f163e67f64bbab3ba8b1793711823", "" },
    { "8250C768-505017D02F731835-E8ACC856CED675F4-03731EBB", "1e51e57fafbece67aa6f160240d4f01eb6512d872abef92356627458ba92f05b", ",169,208,210,", true, 2, 1, "cd853fa88bacc53ad8e314d14bd13b97bb339b912e00a5883ca09a6950c8341c", "" },
    { "8250C768-505017D02F731835-E8ACC856CED675F4-03731EBB", "2c81f0dc5c665d9c79125ff33c525ee22762b4e4222a2dfc71f1968b74d77256", ",226,238,", true, 2, 1, "9cd75a5c1d442a2c21760ad5b9c38afa183f163e67f64bbab3ba8b1793711823", "" },
    { "8250C768-505017D02F731835-E8ACC856CED675F4-03731EBB", "2c81f0dc5c665d9c79125ff33c525ee22762b4e4222a2dfc71f1968b74d77256", ",169,208,210,", true, 2, 1, "cd853fa88bacc53ad8e314d14bd13b97bb339b912e00a5883ca09a6950c8341c", "" },
    { "8250C768-53CA57944E7FCE98-D43A634FB95EBB59", "109e8f171ef97a2773578d2d6c9d555b3823a7aba246738f1a3ed7a3bd0f5ae7", ",169,208,210,226,238,", true, 2, 0, "4bfb6b58bbb7aba7a8ce71d230a5038a6d7058dee5293df44abf47393520c6b0", "" },
    { "8250C768-53CA57944E7FCE98-D43A634FB95EBB59", "7be46bc20191add32c8cd0bc43faabb44d150b323eb74383339fd1def05ce37a", ",169,208,210,226,238,", true, 2, 0, "4bfb6b58bbb7aba7a8ce71d230a5038a6d7058dee5293df44abf47393520c6b0", "" },
    { "8250C768-53CA57944E7FCE98-D43A634FB95EBB59-4E6ED938", "0996eb681110ccf4ca8934742a579940e56f454ea43260b3207fb6a1d1c212ee", ",169,208,210,226,238,", true, 2, 0, "4bfb6b58bbb7aba7a8ce71d230a5038a6d7058dee5293df44abf47393520c6b0", "" },
    { "8250C768-53CA57944E7FCE98-D43A634FB95EBB59-4E6ED938", "ded5345e3798958782239d3bbd57e92ae4adb95b8f2d36b3a05e8f02c787966e", ",169,208,210,226,238,", true, 2, 0, "4bfb6b58bbb7aba7a8ce71d230a5038a6d7058dee5293df44abf47393520c6b0", "" },
    { "8250C768-571D6127CBF96DD1-2DEEA7E9B28316E1", "2682e1494b292a6a4227eceb959ab82ce73717823a6a93d58b80facbee961089", ",169,208,210,226,238,", true, 2, 0, "810dd4b3a2026ba7df4b28e917686960e5c936d27fa83f426dcf7ed39a24aa7f", "" },
    { "8250C768-571D6127CBF96DD1-2DEEA7E9B28316E1", "3051511f639e76acdfa1ece55b3ec16724eb09b5b1e89c89f1878790f079bdd2", ",169,208,210,226,238,", true, 2, 0, "810dd4b3a2026ba7df4b28e917686960e5c936d27fa83f426dcf7ed39a24aa7f", "" },
    { "8250C768-571D6127CBF96DD1-2DEEA7E9B28316E1-D4D3E79B", "35ca8cc5fcef4d02d4ea29471969d82e156dbc029bb92f260e6733505017f79b", ",169,208,210,226,238,", true, 2, 0, "810dd4b3a2026ba7df4b28e917686960e5c936d27fa83f426dcf7ed39a24aa7f", "" },
    { "8250C768-571D6127CBF96DD1-2DEEA7E9B28316E1-D4D3E79B", "e9e604ac68c097f0daee256d56a512a0d7bf707533a569e521149868c3dc84af", ",169,208,210,226,238,", true, 2, 0, "810dd4b3a2026ba7df4b28e917686960e5c936d27fa83f426dcf7ed39a24aa7f", "" },
    { "8250C768-6109F5EF0D30D03D-D48360A752689A4F", "2f38ad29d600ae503d5ca383a75f107fa5eba3b9b2daf2fa07f9e097e52e38a8", ",169,208,210,", true, 2, 1, "a9e0fc45904634760eb9676c2069ec677c4c8f04b8b725a85bf1bc6b632fd688", "" },
    { "8250C768-6109F5EF0D30D03D-D48360A752689A4F", "2f38ad29d600ae503d5ca383a75f107fa5eba3b9b2daf2fa07f9e097e52e38a8", ",226,238,", true, 2, 1, "f80bf00e0694aa8c328507e196300c3cc43489b61a9a64bbcd32571906656fcf", "" },
    { "8250C768-6109F5EF0D30D03D-D48360A752689A4F", "cc89847dde701894b48d92c89f814f61243c6c1dc9261e515bf1318b1e91eb1c", ",169,208,210,", true, 2, 1, "a9e0fc45904634760eb9676c2069ec677c4c8f04b8b725a85bf1bc6b632fd688", "" },
    { "8250C768-6109F5EF0D30D03D-D48360A752689A4F", "cc89847dde701894b48d92c89f814f61243c6c1dc9261e515bf1318b1e91eb1c", ",226,238,", true, 2, 1, "f80bf00e0694aa8c328507e196300c3cc43489b61a9a64bbcd32571906656fcf", "" },
    { "8250C768-6109F5EF0D30D03D-D48360A752689A4F-FDA1C424", "0532eadee2d9b42f079c252b211bf41a953f6b84386a5a57d6c956596da33b78", ",169,208,210,", true, 2, 1, "a9e0fc45904634760eb9676c2069ec677c4c8f04b8b725a85bf1bc6b632fd688", "" },
    { "8250C768-6109F5EF0D30D03D-D48360A752689A4F-FDA1C424", "0532eadee2d9b42f079c252b211bf41a953f6b84386a5a57d6c956596da33b78", ",226,238,", true, 2, 1, "f80bf00e0694aa8c328507e196300c3cc43489b61a9a64bbcd32571906656fcf", "" },
    { "8250C768-6109F5EF0D30D03D-D48360A752689A4F-FDA1C424", "8251059912d28afebbad4ec533dbdd61be7e093ef628c5b171a98328da0324fe", ",169,208,210,", true, 2, 1, "a9e0fc45904634760eb9676c2069ec677c4c8f04b8b725a85bf1bc6b632fd688", "" },
    { "8250C768-6109F5EF0D30D03D-D48360A752689A4F-FDA1C424", "8251059912d28afebbad4ec533dbdd61be7e093ef628c5b171a98328da0324fe", ",226,238,", true, 2, 1, "f80bf00e0694aa8c328507e196300c3cc43489b61a9a64bbcd32571906656fcf", "" },
    { "8250C768-63A8EFA78048289F-69E3BF5AC77001AE-B8D1E567", "6c931a989842eec5f03c2d8b7997de1e8c381dbfe4fe9d0d02a79c785928813c", ",169,", false, 0, -1, "", "" },
    { "8250C768-63A8EFA78048289F-69E3BF5AC77001AE-B8D1E567", "6c931a989842eec5f03c2d8b7997de1e8c381dbfe4fe9d0d02a79c785928813c", ",226,238,", true, 2, 1, "3332df2da2fa61092f90728b6bb8ebd1b5faab45d18be7f476d65f59a363b697", "" },
    { "8250C768-63A8EFA78048289F-69E3BF5AC77001AE-B8D1E567", "6c931a989842eec5f03c2d8b7997de1e8c381dbfe4fe9d0d02a79c785928813c", ",208,210,", true, 2, 1, "6a3965294fd1909c6c9e9877dff17bc3c4a827587716ab46f6a77e1eab2cd865", "" },
    { "8250C768-63A8EFA78048289F-69E3BF5AC77001AE-B8D1E567", "982a55ac28b484efe6574ea6179bf6ea60ee0cfe708ba97d357ef31e32e0878c", ",169,", false, 0, -1, "", "" },
    { "8250C768-63A8EFA78048289F-69E3BF5AC77001AE-B8D1E567", "982a55ac28b484efe6574ea6179bf6ea60ee0cfe708ba97d357ef31e32e0878c", ",226,238,", true, 2, 1, "3332df2da2fa61092f90728b6bb8ebd1b5faab45d18be7f476d65f59a363b697", "" },
    { "8250C768-63A8EFA78048289F-69E3BF5AC77001AE-B8D1E567", "982a55ac28b484efe6574ea6179bf6ea60ee0cfe708ba97d357ef31e32e0878c", ",208,210,", true, 2, 1, "6a3965294fd1909c6c9e9877dff17bc3c4a827587716ab46f6a77e1eab2cd865", "" },
    { "8250C768-66B6CC03F44ED9B7-645E162E54094FF6", "ef953f74760f71d8ecd884ddd1c8a2d0f57a795ab25a172a4dd21630835b05b3", ",169,208,210,226,238,", true, 2, 0, "83f95d5bf896385a99d74711aa175da5835a9c152f8658be9df786892c700ba9", "" },
    { "8250C768-66B6CC03F44ED9B7-645E162E54094FF6-1651D169", "cf6bc02755f8e14bbf0008d2fd97e700b8823cb68c6089bb9f0e4ebda79280e3", ",169,208,210,226,238,", true, 2, 0, "83f95d5bf896385a99d74711aa175da5835a9c152f8658be9df786892c700ba9", "" },
    { "8250C768-697BFF83356D799E-CE177871D3ABC6AE", "114331a91ce0ff33d00e6f5028947020f7a765d862bd9100317b33464a97d73e", ",169,208,210,226,238,", true, 2, 1, "f5fafff14dd86f5aa83ef8b1a5f46e2e0ff3379bc1ec28588dbead545339ebb1", "" },
    { "8250C768-697BFF83356D799E-CE177871D3ABC6AE", "dfdd40c7e7fa5dfdf8834372298669b2ce2bb8f9792c5a2568b0eb0fbea3f2df", ",169,208,210,226,238,", true, 2, 1, "f5fafff14dd86f5aa83ef8b1a5f46e2e0ff3379bc1ec28588dbead545339ebb1", "" },
    { "8250C768-697BFF83356D799E-CE177871D3ABC6AE-116723CE", "91b12898b7b4b551e7fb80b6a4c84c63fa5d06634682740d2a975ceb717cf482", ",169,208,210,226,238,", true, 2, 1, "f5fafff14dd86f5aa83ef8b1a5f46e2e0ff3379bc1ec28588dbead545339ebb1", "" },
    { "8250C768-697BFF83356D799E-CE177871D3ABC6AE-116723CE", "e599b4ce20b77697fe8b1fc26e66a02aa94ea9a9cf5fecec48310a6513b1b893", ",169,208,210,226,238,", true, 2, 1, "f5fafff14dd86f5aa83ef8b1a5f46e2e0ff3379bc1ec28588dbead545339ebb1", "" },
    { "8250C768-72B783D12FF72FF0-E8ACC856CED675F4", "23c526f879e8911a55a28435ed9ab2cfa1dd9a81d00eabef50f55f62babccda2", ",169,208,210,226,", false, 0, -1, "", "scene_catalog_vertex: e3640e7902dea1f6" },
    { "8250C768-72B783D12FF72FF0-E8ACC856CED675F4", "23c526f879e8911a55a28435ed9ab2cfa1dd9a81d00eabef50f55f62babccda2", ",238,", true, 2, 0, "2122b5b8caf6e6e3b8fa1973486542fe2e3b37616ca86b4beee1245a9cc1190c", "" },
    { "8250C768-72B783D12FF72FF0-E8ACC856CED675F4-22C73980", "424371f5106bcd14afb64e07f60ed5dc218ca54946fa17bc3e8e918e887de886", ",169,208,210,226,", false, 0, -1, "", "scene_catalog_vertex: e3640e7902dea1f6" },
    { "8250C768-72B783D12FF72FF0-E8ACC856CED675F4-22C73980", "424371f5106bcd14afb64e07f60ed5dc218ca54946fa17bc3e8e918e887de886", ",238,", true, 2, 0, "2122b5b8caf6e6e3b8fa1973486542fe2e3b37616ca86b4beee1245a9cc1190c", "" },
    { "8250C768-7C1DD533708BB131-818E4EAF9BA41B33", "4febfd744ec89111096fc0162fb18e0721ebaba2bb2635a2ba5d39a83c84e42b", ",169,208,210,226,238,", true, 2, 0, "b0f5bde78606b731633d81f831cabcb0ef8856acd13968e55efb985019672553", "" },
    { "8250C768-7C1DD533708BB131-818E4EAF9BA41B33", "a3fd2c91d572f118099cb60b90e9e48287f9a58870becedd7841655264fe993d", ",169,208,210,226,238,", true, 2, 0, "b0f5bde78606b731633d81f831cabcb0ef8856acd13968e55efb985019672553", "" },
    { "8250C768-7C1DD533708BB131-818E4EAF9BA41B33-F43F2A6E", "027903d67ac911256f6d7ce4cbd8bf3c83b55f19be97eff149431b8570000d50", ",169,208,210,226,238,", true, 2, 0, "b0f5bde78606b731633d81f831cabcb0ef8856acd13968e55efb985019672553", "" },
    { "8250C768-7C1DD533708BB131-818E4EAF9BA41B33-F43F2A6E", "8be4449bf1e7a94d58c16b9677bd1b341af1df6a408aa466f65e5abe2e69c347", ",169,208,210,226,238,", true, 2, 0, "b0f5bde78606b731633d81f831cabcb0ef8856acd13968e55efb985019672553", "" },
    { "8250C768-7E796429CA85BC30-DFF33307688C1842", "04856df4c01149262086c25955cdd561cec7d90d204a2962f68edb719c33ae77", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-7E796429CA85BC30-DFF33307688C1842", "04856df4c01149262086c25955cdd561cec7d90d204a2962f68edb719c33ae77", ",226,238,", true, 2, 1, "0b2072808a41ca778386c52da0bfd71635f81590b3e811643cd802ead282e7de", "" },
    { "8250C768-7E796429CA85BC30-DFF33307688C1842", "983bca1f0ee756f9c7390d9dcdd48174c70b07b5f77e8eeb7be72ea5893ab6e7", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-7E796429CA85BC30-DFF33307688C1842", "983bca1f0ee756f9c7390d9dcdd48174c70b07b5f77e8eeb7be72ea5893ab6e7", ",226,238,", true, 2, 1, "0b2072808a41ca778386c52da0bfd71635f81590b3e811643cd802ead282e7de", "" },
    { "8250C768-7E796429CA85BC30-DFF33307688C1842-009FDEAA", "b1c6324c50fe5924fbb7bda28f76fd477ec02db989c3c8e8c29c17a62efbdbae", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-7E796429CA85BC30-DFF33307688C1842-009FDEAA", "b1c6324c50fe5924fbb7bda28f76fd477ec02db989c3c8e8c29c17a62efbdbae", ",226,238,", true, 2, 1, "0b2072808a41ca778386c52da0bfd71635f81590b3e811643cd802ead282e7de", "" },
    { "8250C768-7E796429CA85BC30-DFF33307688C1842-009FDEAA", "b3ec8c62ac365053ede5197026ec8b4bff10ecb0b9da9e0ea9ac79c03340dde6", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-7E796429CA85BC30-DFF33307688C1842-009FDEAA", "b3ec8c62ac365053ede5197026ec8b4bff10ecb0b9da9e0ea9ac79c03340dde6", ",226,238,", true, 2, 1, "0b2072808a41ca778386c52da0bfd71635f81590b3e811643cd802ead282e7de", "" },
    { "8250C768-8CB6BAB2CA551EF0-E060E45C42635B47", "25777deac27cf9c116db89961803e2286fd010b5f39aa4a7268a5a2ea9c9df33", ",169,208,210,226,238,", true, 2, 1, "618f787eb09953a2bde983ffd33f3a7c7eae595c0a1b28adc93af1aed85a0668", "" },
    { "8250C768-8CB6BAB2CA551EF0-E060E45C42635B47", "68cc32a4f8e6f56b77df20624d048997a95d603538151a3674212426cf09d73e", ",169,208,210,226,238,", true, 2, 1, "618f787eb09953a2bde983ffd33f3a7c7eae595c0a1b28adc93af1aed85a0668", "" },
    { "8250C768-8CB6BAB2CA551EF0-E060E45C42635B47-C17E06EE", "448d8850370a3aac259e064a780fa94ce0188834f3b966286205d5cb7126d0eb", ",169,208,210,226,238,", true, 2, 1, "618f787eb09953a2bde983ffd33f3a7c7eae595c0a1b28adc93af1aed85a0668", "" },
    { "8250C768-8CB6BAB2CA551EF0-E060E45C42635B47-C17E06EE", "b1d68a1766dbc72f0533b3a3ff96eeab7ffc304ce40896ba68568321abe45739", ",169,208,210,226,238,", true, 2, 1, "618f787eb09953a2bde983ffd33f3a7c7eae595c0a1b28adc93af1aed85a0668", "" },
    { "8250C768-8D86FD5003421DDD-18517DB74F3982FA", "16a0b6cb7f0c5450d18ab318ca43e65b5baaff02e7503b031484678a19cd0c5b", ",169,208,210,226,238,", true, 2, 0, "1d51430708283194a9012d6629ff08b9628d98453d1792ea79c2bd83741d0270", "" },
    { "8250C768-8D86FD5003421DDD-18517DB74F3982FA", "3c84a837f98d07f0d8a261bdd66dd8e60e2599e8e2d74242d2c9235bb7c75294", ",169,208,210,226,238,", true, 2, 0, "1d51430708283194a9012d6629ff08b9628d98453d1792ea79c2bd83741d0270", "" },
    { "8250C768-8D86FD5003421DDD-18517DB74F3982FA-A479A79D", "49cc9b384282dd5434144842818645ff23bcfdb5f5d86709e138099d85661f10", ",169,208,210,226,238,", true, 2, 0, "1d51430708283194a9012d6629ff08b9628d98453d1792ea79c2bd83741d0270", "" },
    { "8250C768-8D86FD5003421DDD-18517DB74F3982FA-A479A79D", "603e837f5909177827094adf37a71cb411c1c7c39002993a76c3fa57b18aa076", ",169,208,210,226,238,", true, 2, 0, "1d51430708283194a9012d6629ff08b9628d98453d1792ea79c2bd83741d0270", "" },
    { "8250C768-8EA45EE83AB383D6-E2ED28779C5F424F", "0241809136027961b63987ec6148e822cf3fb326ae9285e27bb6e7f650d967ed", ",169,208,210,226,238,", true, 2, 1, "02d0db6ecd043da3e3d2e7e411be57f1810c52e7c166efcef7110bd1ca6248c5", "" },
    { "8250C768-8EA45EE83AB383D6-E2ED28779C5F424F", "a0a1327a60e45deed893e24a175e5f1ac43ebd8e375d2401f5c5a291b8cf1aed", ",169,208,210,226,238,", true, 2, 1, "02d0db6ecd043da3e3d2e7e411be57f1810c52e7c166efcef7110bd1ca6248c5", "" },
    { "8250C768-8EA45EE83AB383D6-E2ED28779C5F424F", "e7db6d7fa538a507318326debf911474b5c370030988d3e6f4e2dff929066657", ",169,208,210,226,238,", true, 2, 1, "02d0db6ecd043da3e3d2e7e411be57f1810c52e7c166efcef7110bd1ca6248c5", "" },
    { "8250C768-8EA45EE83AB383D6-E2ED28779C5F424F-65F354E6", "2fdea4bb8321289e45c28c01d0878e5ce45d3d35e4b87c978d85f7a8743124fe", ",169,208,210,226,238,", true, 2, 1, "02d0db6ecd043da3e3d2e7e411be57f1810c52e7c166efcef7110bd1ca6248c5", "" },
    { "8250C768-8F2968A027E92D01-29B347CD0CA8F048", "3008187e2f53988ec131cf3e41424f30e139bb78eb78ff80e81f9bdd827f0816", ",226,238,", true, 2, 1, "5fabca93f7bb78369582f7419880175c4b334d63a2121c57ebe572f63673cca9", "" },
    { "8250C768-8F2968A027E92D01-29B347CD0CA8F048", "3008187e2f53988ec131cf3e41424f30e139bb78eb78ff80e81f9bdd827f0816", ",169,208,210,", true, 2, 1, "76a32ff8db568a28e856717b74b38e555a6499fe9c14a34792b8350d8d80cb35", "" },
    { "8250C768-8F2968A027E92D01-29B347CD0CA8F048-B87EE83D", "585467cacb3a439d899733d74332fbedb6ffde86cf02828682ab001251bddf4b", ",226,238,", true, 2, 1, "5fabca93f7bb78369582f7419880175c4b334d63a2121c57ebe572f63673cca9", "" },
    { "8250C768-8F2968A027E92D01-29B347CD0CA8F048-B87EE83D", "585467cacb3a439d899733d74332fbedb6ffde86cf02828682ab001251bddf4b", ",169,208,210,", true, 2, 1, "76a32ff8db568a28e856717b74b38e555a6499fe9c14a34792b8350d8d80cb35", "" },
    { "8250C768-8F2968A027E92D01-29B347CD0CA8F048-B87EE83D", "b134bd1ed951d7ad482d2cff5d5acff47e7c356423acbcbf7ab0347b6a3b0b53", ",226,238,", true, 2, 1, "5fabca93f7bb78369582f7419880175c4b334d63a2121c57ebe572f63673cca9", "" },
    { "8250C768-8F2968A027E92D01-29B347CD0CA8F048-B87EE83D", "b134bd1ed951d7ad482d2cff5d5acff47e7c356423acbcbf7ab0347b6a3b0b53", ",169,208,210,", true, 2, 1, "76a32ff8db568a28e856717b74b38e555a6499fe9c14a34792b8350d8d80cb35", "" },
    { "8250C768-96AB8DCB35A93C52-9CAD281685361215", "a9c847206ae1e3bb111846b1bfef52a49ab93b5eb60d2d406be1da8bcff17555", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-96AB8DCB35A93C52-9CAD281685361215", "a9c847206ae1e3bb111846b1bfef52a49ab93b5eb60d2d406be1da8bcff17555", ",226,238,", true, 2, 1, "9eed0e07400ae7866ff247d62e7158be219a1b8b2117f5f14752464b5f274ed2", "" },
    { "8250C768-96AB8DCB35A93C52-9CAD281685361215", "c34de08ef62b12cf17e41358175f9f5661f3cb87888fbc02d1262f752b64e463", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-96AB8DCB35A93C52-9CAD281685361215", "c34de08ef62b12cf17e41358175f9f5661f3cb87888fbc02d1262f752b64e463", ",226,238,", true, 2, 1, "9eed0e07400ae7866ff247d62e7158be219a1b8b2117f5f14752464b5f274ed2", "" },
    { "8250C768-96AB8DCB35A93C52-9CAD281685361215-E26C02FC", "5497de7a7dc215794c74f84f81e22df2387e915cf9a8fa78d425c6fa51ee218f", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-96AB8DCB35A93C52-9CAD281685361215-E26C02FC", "5497de7a7dc215794c74f84f81e22df2387e915cf9a8fa78d425c6fa51ee218f", ",226,238,", true, 2, 1, "9eed0e07400ae7866ff247d62e7158be219a1b8b2117f5f14752464b5f274ed2", "" },
    { "8250C768-96AB8DCB35A93C52-9CAD281685361215-E26C02FC", "f8e6f6c584e2765d98eee9e9e51645ed2a6eef5493712ccccb800a5bc40b53db", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-96AB8DCB35A93C52-9CAD281685361215-E26C02FC", "f8e6f6c584e2765d98eee9e9e51645ed2a6eef5493712ccccb800a5bc40b53db", ",226,238,", true, 2, 1, "9eed0e07400ae7866ff247d62e7158be219a1b8b2117f5f14752464b5f274ed2", "" },
    { "8250C768-A032C90DE2197B2A-CE177871D3ABC6AE", "219c6af66aeeba39dfbaa7f63cd56c69fabb224ff20d01ccac24a2af2b9bbe76", ",169,208,210,226,238,", true, 2, 0, "0b30a4e88f82cfb4a9a43308fd9c8498490f91e2ab841e6604c9af650da9aad7", "" },
    { "8250C768-A032C90DE2197B2A-CE177871D3ABC6AE", "72507e679230468b6ce833abbf896d2da80aef5ec5142f641761c650dcee33d0", ",169,208,210,226,238,", true, 2, 0, "0b30a4e88f82cfb4a9a43308fd9c8498490f91e2ab841e6604c9af650da9aad7", "" },
    { "8250C768-A032C90DE2197B2A-CE177871D3ABC6AE-52B3CBAA", "6b4a86c9a50e2af55c7af152be4e0860f73eb11156de01142ab520ebfc39fda4", ",169,208,210,226,238,", true, 2, 0, "0b30a4e88f82cfb4a9a43308fd9c8498490f91e2ab841e6604c9af650da9aad7", "" },
    { "8250C768-A032C90DE2197B2A-CE177871D3ABC6AE-52B3CBAA", "8c7cfa2928286f4f6a6fa998b7e69312ae0d4afb1829eda146bf2ab5b03af7d5", ",169,208,210,226,238,", true, 2, 0, "0b30a4e88f82cfb4a9a43308fd9c8498490f91e2ab841e6604c9af650da9aad7", "" },
    { "8250C768-A032C90DE2197B2A-CE177871D3ABC6AE-A86180E6", "8e76f6867c830488beb77a2619efc174bd39436a232af515924b4712bf719d7f", ",169,208,210,226,238,", true, 2, 1, "0b30a4e88f82cfb4a9a43308fd9c8498490f91e2ab841e6604c9af650da9aad7", "" },
    { "8250C768-A285DE0198E37310-454675E80C55A32E", "3d87654c341c0597f5b13cb886d607f2fc7885b2e50e9ebee182d38034bd4f97", ",169,208,210,226,238,", true, 2, 1, "9fd70b69cbd176d6246480df25b73104a116a1dc63f179fc6c2661a7f690a1b9", "" },
    { "8250C768-A285DE0198E37310-454675E80C55A32E", "e15251d23ea5adfd0a6da19d94f5dbc2fa8e6a6d12351dfbe18e26c8a1071ee7", ",169,208,210,226,238,", true, 2, 1, "9fd70b69cbd176d6246480df25b73104a116a1dc63f179fc6c2661a7f690a1b9", "" },
    { "8250C768-A285DE0198E37310-454675E80C55A32E-F85BC79A", "c4711556467a48a29216276baaac90236b3cf48509a3052d0eb221939f1d3667", ",169,208,210,226,238,", true, 2, 1, "9fd70b69cbd176d6246480df25b73104a116a1dc63f179fc6c2661a7f690a1b9", "" },
    { "8250C768-A2E337A474B6C402-D713E61CF34EC80A", "87a901007aeb2b93c6362c42ea779b1b8f28b5ef243b2adebd28f228bc23eaba", ",226,", true, 2, 1, "7560ed36cbd118f67bb61e5293fe7a5c139fb3cc04f13a6211ff015959ee9d2a", "" },
    { "8250C768-A2E337A474B6C402-D713E61CF34EC80A", "87a901007aeb2b93c6362c42ea779b1b8f28b5ef243b2adebd28f228bc23eaba", ",169,208,210,", true, 2, 1, "99adb53d5e1a4cf323ad1f759b89429981422b2ac7974594c2d28ef2e734d79a", "" },
    { "8250C768-A2E337A474B6C402-D713E61CF34EC80A", "87a901007aeb2b93c6362c42ea779b1b8f28b5ef243b2adebd28f228bc23eaba", ",238,", true, 4, 1, "e02ca8be95118e2f70526039ddb254c28fb145880da91e9be460d38d094f6a98", "" },
    { "8250C768-A2E337A474B6C402-D713E61CF34EC80A", "946d9036d9c0557d15ba20d52cf4c6e3495f97fa73e9b490114a69c3efcfb127", ",226,", true, 2, 1, "7560ed36cbd118f67bb61e5293fe7a5c139fb3cc04f13a6211ff015959ee9d2a", "" },
    { "8250C768-A2E337A474B6C402-D713E61CF34EC80A", "946d9036d9c0557d15ba20d52cf4c6e3495f97fa73e9b490114a69c3efcfb127", ",169,208,210,", true, 2, 1, "99adb53d5e1a4cf323ad1f759b89429981422b2ac7974594c2d28ef2e734d79a", "" },
    { "8250C768-A2E337A474B6C402-D713E61CF34EC80A", "946d9036d9c0557d15ba20d52cf4c6e3495f97fa73e9b490114a69c3efcfb127", ",238,", true, 4, 1, "e02ca8be95118e2f70526039ddb254c28fb145880da91e9be460d38d094f6a98", "" },
    { "8250C768-A2E337A474B6C402-D713E61CF34EC80A-F1C3C277", "0cd128e5415645b0bde7df5dae911221909ddad8d54ee7f08e2fd14319a5f59d", ",226,", false, 2, -1, "7560ed36cbd118f67bb61e5293fe7a5c139fb3cc04f13a6211ff015959ee9d2a", "scene_specialization: alpha 4 samples 4 program 0x10710909 context 0x8 interpolators 0x7f00ff user clip plane 0" },
    { "8250C768-A2E337A474B6C402-D713E61CF34EC80A-F1C3C277", "0cd128e5415645b0bde7df5dae911221909ddad8d54ee7f08e2fd14319a5f59d", ",169,208,210,", false, 2, -1, "99adb53d5e1a4cf323ad1f759b89429981422b2ac7974594c2d28ef2e734d79a", "scene_specialization: alpha 4 samples 4 program 0x10710909 context 0x8 interpolators 0x7f00ff user clip plane 0" },
    { "8250C768-A2E337A474B6C402-D713E61CF34EC80A-F1C3C277", "0cd128e5415645b0bde7df5dae911221909ddad8d54ee7f08e2fd14319a5f59d", ",238,", true, 4, 3, "e02ca8be95118e2f70526039ddb254c28fb145880da91e9be460d38d094f6a98", "" },
    { "8250C768-A2E337A474B6C402-D713E61CF34EC80A-F1C3C277", "e0ec77416456d47af4d82ee387a82b8f505aacd525d070a927ebd0d56a53f950", ",226,", true, 2, 1, "7560ed36cbd118f67bb61e5293fe7a5c139fb3cc04f13a6211ff015959ee9d2a", "" },
    { "8250C768-A2E337A474B6C402-D713E61CF34EC80A-F1C3C277", "e0ec77416456d47af4d82ee387a82b8f505aacd525d070a927ebd0d56a53f950", ",169,208,210,", true, 2, 1, "99adb53d5e1a4cf323ad1f759b89429981422b2ac7974594c2d28ef2e734d79a", "" },
    { "8250C768-A2E337A474B6C402-D713E61CF34EC80A-F1C3C277", "e0ec77416456d47af4d82ee387a82b8f505aacd525d070a927ebd0d56a53f950", ",238,", true, 4, 1, "e02ca8be95118e2f70526039ddb254c28fb145880da91e9be460d38d094f6a98", "" },
    { "8250C768-AC66B9099D8327CD-9630340F17DC9765", "1de51308382b392387fe18c9e141e6059652bcd33d9dee1d327e830fa0c44430", ",169,208,210,", true, 2, 1, "220be447955e7c55f604385fd5eb9f95b1f557db4b54ff623098e7f1f0ca35d9", "" },
    { "8250C768-AC66B9099D8327CD-9630340F17DC9765", "1de51308382b392387fe18c9e141e6059652bcd33d9dee1d327e830fa0c44430", ",226,238,", true, 2, 1, "5c8a9d3986f173af486e70b4e9c1d21a281de6688757c513d47cb798dc121648", "" },
    { "8250C768-AC66B9099D8327CD-9630340F17DC9765", "3de93468163fac7a687b5141ba25e47436a2029b4fac1597b63b977307dbf799", ",169,208,210,", true, 2, 1, "220be447955e7c55f604385fd5eb9f95b1f557db4b54ff623098e7f1f0ca35d9", "" },
    { "8250C768-AC66B9099D8327CD-9630340F17DC9765", "3de93468163fac7a687b5141ba25e47436a2029b4fac1597b63b977307dbf799", ",226,238,", true, 2, 1, "5c8a9d3986f173af486e70b4e9c1d21a281de6688757c513d47cb798dc121648", "" },
    { "8250C768-AC66B9099D8327CD-9630340F17DC9765-D9D12C03", "93c9b0d55ffb7ff7392da6918730a55fdcf19eb51e81179af82ec25cfbf4303c", ",169,208,210,", true, 2, 1, "220be447955e7c55f604385fd5eb9f95b1f557db4b54ff623098e7f1f0ca35d9", "" },
    { "8250C768-AC66B9099D8327CD-9630340F17DC9765-D9D12C03", "93c9b0d55ffb7ff7392da6918730a55fdcf19eb51e81179af82ec25cfbf4303c", ",226,238,", true, 2, 1, "5c8a9d3986f173af486e70b4e9c1d21a281de6688757c513d47cb798dc121648", "" },
    { "8250C768-AC66B9099D8327CD-9630340F17DC9765-D9D12C03", "c3c333b6cb075a77cde21c7295249d7eab8e25fd1456d0615eaceb5f3857d6b8", ",169,208,210,", true, 2, 1, "220be447955e7c55f604385fd5eb9f95b1f557db4b54ff623098e7f1f0ca35d9", "" },
    { "8250C768-AC66B9099D8327CD-9630340F17DC9765-D9D12C03", "c3c333b6cb075a77cde21c7295249d7eab8e25fd1456d0615eaceb5f3857d6b8", ",226,238,", true, 2, 1, "5c8a9d3986f173af486e70b4e9c1d21a281de6688757c513d47cb798dc121648", "" },
    { "8250C768-B2B037E121007357-045CB69365B1063F", "1078962912b77aea545ee3b4da2191dd3b6d1b50415dfae840094cb9c2ca153e", ",169,208,210,226,238,", true, 2, 1, "14faca97810432b2eb4abc5450a8808f33a6de1594b4f53d769204c2fb8816a4", "" },
    { "8250C768-B2B037E121007357-045CB69365B1063F", "9c394938a8cf956985cf5b515e5ed231ceeb216b781b3cbae1a9df7e2c811481", ",169,208,210,226,238,", true, 2, 1, "14faca97810432b2eb4abc5450a8808f33a6de1594b4f53d769204c2fb8816a4", "" },
    { "8250C768-B2B037E121007357-045CB69365B1063F-F97E632D", "72deed3fd14417819a7ba5774f6d3b52df4e6986674fb6f630582d448ca84dca", ",169,208,210,226,238,", true, 2, 1, "14faca97810432b2eb4abc5450a8808f33a6de1594b4f53d769204c2fb8816a4", "" },
    { "8250C768-B2B037E121007357-045CB69365B1063F-F97E632D", "8618cef0ab7fa8b87ca828383f4d8ecb86657a2afb12a1f400ff0e57b2de3039", ",169,208,210,226,238,", true, 2, 1, "14faca97810432b2eb4abc5450a8808f33a6de1594b4f53d769204c2fb8816a4", "" },
    { "8250C768-B57F2F4D1864C161-5E950629CCB7C2FE-9A45F6F8", "3b7276c03565b9e9f1981bcc5f59bc71e9ed103c19affae2820438a18cd2c96d", ",169,", false, 0, -1, "", "" },
    { "8250C768-B57F2F4D1864C161-5E950629CCB7C2FE-9A45F6F8", "3b7276c03565b9e9f1981bcc5f59bc71e9ed103c19affae2820438a18cd2c96d", ",208,210,226,238,", true, 2, 1, "1f70582ff401df6cc35ce54824932eca9ea72c0c2ed77a7e3c085b153938ceb4", "" },
    { "8250C768-BAD6D2B0762A0E40-2D48FE86E3E13CAB-FEBEBF84", "35d0a29772e9e4fec492c3509f430b64de4afe0514d81263003291aea5eaedd7", ",169,", false, 0, -1, "", "" },
    { "8250C768-BAD6D2B0762A0E40-2D48FE86E3E13CAB-FEBEBF84", "35d0a29772e9e4fec492c3509f430b64de4afe0514d81263003291aea5eaedd7", ",208,210,226,238,", true, 2, 1, "f937954de89758a7490b71fa75d482a6b1c4e8a3d20041d26dc5189a97e5b5df", "" },
    { "8250C768-BAD6D2B0762A0E40-2D48FE86E3E13CAB-FEBEBF84", "f7da7538c8ab814e5b2af0f6c41db76ed2bf107921e7ba5cfbffffd66a9c14ca", ",169,", false, 0, -1, "", "" },
    { "8250C768-BAD6D2B0762A0E40-2D48FE86E3E13CAB-FEBEBF84", "f7da7538c8ab814e5b2af0f6c41db76ed2bf107921e7ba5cfbffffd66a9c14ca", ",208,210,226,238,", true, 2, 1, "f937954de89758a7490b71fa75d482a6b1c4e8a3d20041d26dc5189a97e5b5df", "" },
    { "8250C768-C3BAB94E67553E12-64D8E6A475FFCB2D", "24dba5ccc68d1609c72490661632a2639564d381884377a05bdb4b00cee1ddd5", ",169,208,210,226,238,", true, 2, 1, "3c60e619657a2dedb11143e91aaeb056ec7b98ce1ddcb72ce57a671fadd2054f", "" },
    { "8250C768-C3BAB94E67553E12-64D8E6A475FFCB2D", "5bd581640bcb9eecc7d88daab2a64b0963113b21f3916d4b9a9954b1dd68904e", ",169,208,210,226,238,", true, 2, 1, "3c60e619657a2dedb11143e91aaeb056ec7b98ce1ddcb72ce57a671fadd2054f", "" },
    { "8250C768-C3BAB94E67553E12-64D8E6A475FFCB2D", "a4b58e82291bce75efdb82b55e33ecf19638806ecd7904c07c32ea30ab93631e", ",169,208,210,226,238,", true, 2, 1, "3c60e619657a2dedb11143e91aaeb056ec7b98ce1ddcb72ce57a671fadd2054f", "" },
    { "8250C768-C3BAB94E67553E12-64D8E6A475FFCB2D-ED28364D", "77f4d782bc6ea7404d7ef7e29b0ff5f4cad618ebcefca466d3757d4eb067e065", ",169,208,210,226,238,", true, 2, 1, "3c60e619657a2dedb11143e91aaeb056ec7b98ce1ddcb72ce57a671fadd2054f", "" },
    { "8250C768-C3BAB94E67553E12-64D8E6A475FFCB2D-ED28364D", "f72ae9ec377a214b7b47bb88725e862c67887337a234e6639e35a4c8cecce94f", ",169,208,210,226,238,", true, 2, 1, "3c60e619657a2dedb11143e91aaeb056ec7b98ce1ddcb72ce57a671fadd2054f", "" },
    { "8250C768-C5468492753D1577-5E950629CCB7C2FE-0D8CA976", "d048a492618df9f1abb670a67fc1b78d9049a03a6174ab76c731a612330b2ea9", ",169,", false, 0, -1, "", "" },
    { "8250C768-C5468492753D1577-5E950629CCB7C2FE-0D8CA976", "d048a492618df9f1abb670a67fc1b78d9049a03a6174ab76c731a612330b2ea9", ",208,210,226,238,", true, 2, 0, "752c695e702783a7762b550a560e14eda449bcd119ef5ecf1ace5c5dde070b9b", "" },
    { "8250C768-C5DC55C091E02AC9-732DADED00CF4DCB", "2da054503ce129488c84241e537b89f3f04d2eba5d4dfdf7bdd742c81fc8616b", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-C5DC55C091E02AC9-732DADED00CF4DCB", "2da054503ce129488c84241e537b89f3f04d2eba5d4dfdf7bdd742c81fc8616b", ",226,238,", true, 2, 0, "bf6d04d938d473089b20b96fc4fdc49ef21aa94d08d83e9b042c509ae941bce5", "" },
    { "8250C768-C5DC55C091E02AC9-732DADED00CF4DCB-948F930C", "04285f66545d0f14f1c26fceb5c83964a9f7d37b1fcc3193c7baacb1ed1b947b", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-C5DC55C091E02AC9-732DADED00CF4DCB-948F930C", "04285f66545d0f14f1c26fceb5c83964a9f7d37b1fcc3193c7baacb1ed1b947b", ",226,238,", true, 2, 0, "bf6d04d938d473089b20b96fc4fdc49ef21aa94d08d83e9b042c509ae941bce5", "" },
    { "8250C768-C5DC55C091E02AC9-732DADED00CF4DCB-948F930C", "f6fced6e6b5f7b5929ba3225caeaa93def89b639aedb1b1084a5796fad57356a", ",169,208,210,", false, 0, -1, "", "" },
    { "8250C768-C5DC55C091E02AC9-732DADED00CF4DCB-948F930C", "f6fced6e6b5f7b5929ba3225caeaa93def89b639aedb1b1084a5796fad57356a", ",226,238,", true, 2, 0, "bf6d04d938d473089b20b96fc4fdc49ef21aa94d08d83e9b042c509ae941bce5", "" },
    { "8250C768-C899B8B9922B81DB-F11CB3E44117D02C", "8a18d755d262a75ea0873d68379061625745ace2970d7cb1c54ce53e9f6c5514", ",169,208,210,226,238,", true, 2, 1, "131ad25eda392d24db4d3b67b6f83a527505a711a46d3e37c78f11e386a408c4", "" },
    { "8250C768-C899B8B9922B81DB-F11CB3E44117D02C", "ae05d6293a81fa793d7e89ef599c91c6b97b55a5b3105d9c0920f928abb89234", ",169,208,210,226,238,", true, 2, 1, "131ad25eda392d24db4d3b67b6f83a527505a711a46d3e37c78f11e386a408c4", "" },
    { "8250C768-C899B8B9922B81DB-F11CB3E44117D02C", "fa79a1d10cc4cc4963ded4eeb50dd47165183369e09952e647eb933419cb66a1", ",169,208,210,226,238,", true, 2, 1, "131ad25eda392d24db4d3b67b6f83a527505a711a46d3e37c78f11e386a408c4", "" },
    { "8250C768-C899B8B9922B81DB-F11CB3E44117D02C-860E4B47", "2f5c4ed35905312d36910b06ff3b2eed95f1783699f17d1c798d88da286eb640", ",169,208,210,226,238,", true, 2, 1, "131ad25eda392d24db4d3b67b6f83a527505a711a46d3e37c78f11e386a408c4", "" },
    { "8250C768-C899B8B9922B81DB-F11CB3E44117D02C-860E4B47", "7df3a8c989562ac975937cf8a77772e99ee17696bf13e589371ce1450ae8a172", ",169,208,210,226,238,", true, 2, 1, "131ad25eda392d24db4d3b67b6f83a527505a711a46d3e37c78f11e386a408c4", "" },
    { "8250C768-CB65245EB0D71196-CE177871D3ABC6AE", "5442f740ef8a6516a6d063368b32ed158a361909cfbf0afd20925c147366eca9", ",169,208,210,226,238,", true, 2, 1, "0327fbd9aaa822b174908053fdb8ccfa86e4cb620c764b63e43eed4e0f5b2967", "" },
    { "8250C768-CB65245EB0D71196-CE177871D3ABC6AE", "f6ad7177a5b684d1b854364e07bb630553efbe612cbec9c632627ff7cc8806de", ",169,208,210,226,238,", true, 2, 1, "0327fbd9aaa822b174908053fdb8ccfa86e4cb620c764b63e43eed4e0f5b2967", "" },
    { "8250C768-CB65245EB0D71196-CE177871D3ABC6AE-B975653D", "2843fad16429cce3421895ff002f1e5bd97ad4ddaffce26d7e317d1c1384ee10", ",169,208,210,226,238,", true, 2, 1, "0327fbd9aaa822b174908053fdb8ccfa86e4cb620c764b63e43eed4e0f5b2967", "" },
    { "8250C768-CF935A2CC1550D5D-CD99817770DB94B6", "5e4263e522c606560f226c8a3a0341598e2f4095eddc582d170bbea99cbcd90c", ",169,208,210,226,238,", true, 2, 1, "9a36bd5ccba076060ce3e21ab387246e2b94dd7479c314540f96a5d1c26c644f", "" },
    { "8250C768-CF935A2CC1550D5D-CD99817770DB94B6-4D1B813C", "34ce0f903f9ac31085c363c715771644c433717d063d48fe2516c7026c6b592d", ",169,208,210,226,238,", true, 2, 1, "9a36bd5ccba076060ce3e21ab387246e2b94dd7479c314540f96a5d1c26c644f", "" },
    { "8250C768-CF935A2CC1550D5D-CD99817770DB94B6-4D1B813C", "64d1d4c7fd94df7ac902074f9f3ed83379af8c81b2d1cbe8b7b4114ea0fccc19", ",169,208,210,226,238,", true, 2, 1, "9a36bd5ccba076060ce3e21ab387246e2b94dd7479c314540f96a5d1c26c644f", "" },
    { "8250C768-CFBD83BE32B7E4B1-8BE08622275C6D98", "1740fabdd1f0652bfb9e7ea5c02242e85af614510d0f4cd9e2ed1b3f27259cec", ",169,208,210,226,238,", true, 2, 0, "f79cf6e597b907156981095d3326fd1a7cc07a85634c2c283c9a4207421c8b37", "" },
    { "8250C768-CFBD83BE32B7E4B1-8BE08622275C6D98", "dcef36695214d035cf42275f1f8d640bc7816b9fee29dcfad6d5b568869a7454", ",169,208,210,226,238,", true, 2, 0, "f79cf6e597b907156981095d3326fd1a7cc07a85634c2c283c9a4207421c8b37", "" },
    { "8250C768-CFBD83BE32B7E4B1-8BE08622275C6D98-2D7C7BD9", "ae602138904c4a623ea75ba5f6e28424a88b99df5430d17a7cb2896ee7be8b58", ",169,208,210,226,238,", true, 2, 0, "f79cf6e597b907156981095d3326fd1a7cc07a85634c2c283c9a4207421c8b37", "" },
    { "8250C768-CFBD83BE32B7E4B1-8BE08622275C6D98-2D7C7BD9", "d513823ab4fbf81d30f89ae66f08762d82d12cab4753e9e6adce0c974027dc7b", ",169,208,210,226,238,", true, 2, 0, "f79cf6e597b907156981095d3326fd1a7cc07a85634c2c283c9a4207421c8b37", "" },
    { "8250C768-D0B9BEEA47A2E2DD-EBC911293961470A", "94e9c88d39938134d2200f7c2a09f847f2fe9c7e57ededfdb23c43ed306f0d29", ",169,208,210,226,238,", true, 2, 1, "553b218b875072ddb5144b126ac7a7d7777b83c91cae2ae383d3796c256b21eb", "" },
    { "8250C768-D0B9BEEA47A2E2DD-EBC911293961470A", "b5055aa4b117c049ae4e3e07233865c8cfe52123b84fc6a196d1f6ae7d8aa066", ",169,208,210,226,238,", true, 2, 1, "553b218b875072ddb5144b126ac7a7d7777b83c91cae2ae383d3796c256b21eb", "" },
    { "8250C768-D0B9BEEA47A2E2DD-EBC911293961470A-E7B4D13C", "a56d1e0f6b067705214ee4bab0ea321c7edfb1c5ec7ecf7a5ce33c4808090baa", ",169,208,210,226,238,", true, 2, 1, "553b218b875072ddb5144b126ac7a7d7777b83c91cae2ae383d3796c256b21eb", "" },
    { "8250C768-D0B9BEEA47A2E2DD-EBC911293961470A-E7B4D13C", "fbb1542d1cd9a0b4b258335992fc0c7328e5104e06cfe3e91eba69ba76eb3dde", ",169,208,210,226,238,", true, 2, 1, "553b218b875072ddb5144b126ac7a7d7777b83c91cae2ae383d3796c256b21eb", "" },
    { "8250C768-D21B0DC560EBE802-800C21DB00774B9D", "b1051c7db7faceaefd0ef97a6f7dabf6fe832dabd35dd574d9e7c7db08f3c4a6", ",169,208,210,", true, 2, 1, "c9b8fa7bbeaa7fd0dc04619b66017e823d88da07df94dc8963641d5f947d75f5", "" },
    { "8250C768-D21B0DC560EBE802-800C21DB00774B9D", "b1051c7db7faceaefd0ef97a6f7dabf6fe832dabd35dd574d9e7c7db08f3c4a6", ",226,238,", true, 2, 1, "d324355c3a0979aa29765a7a7dbeee276e3cee91fb6a04d63e6bd6b288ee62fa", "" },
    { "8250C768-D21B0DC560EBE802-800C21DB00774B9D-A52D4D87", "c4d80401b16e54389b9c04e277339cc43e88af1b7204efbee5fb296662f4032c", ",169,208,210,", true, 2, 1, "c9b8fa7bbeaa7fd0dc04619b66017e823d88da07df94dc8963641d5f947d75f5", "" },
    { "8250C768-D21B0DC560EBE802-800C21DB00774B9D-A52D4D87", "c4d80401b16e54389b9c04e277339cc43e88af1b7204efbee5fb296662f4032c", ",226,238,", true, 2, 1, "d324355c3a0979aa29765a7a7dbeee276e3cee91fb6a04d63e6bd6b288ee62fa", "" },
    { "8250C768-DBCB39509F915608-CE177871D3ABC6AE", "057fce56fa01256a49436526c90d0f084c1e2fafeef03c74a065ae7d50170430", ",169,208,210,226,238,", true, 2, 0, "6ba91e05fd00381d18e44c28921d9e0b3f3509a3c9f130cb13af2b3de39855b7", "" },
    { "8250C768-DBCF02DFB9E3F8FB-F375BB83352BFB1B", "11f2df60e5e43f6908cade14a63c2917a63e26375fbee092a2b2afa515d51801", ",169,208,210,226,238,", true, 2, 0, "f1913b1f597e0948cca0d6f28d5ec2a8fcef9f31d4dd884497e6756900f8a66b", "" },
    { "8250C768-DBCF02DFB9E3F8FB-F375BB83352BFB1B", "f7c761b33a039e5a4fc457b6d8dfe850116b928beab87a0e4c472a838a3553b7", ",169,208,210,226,238,", true, 2, 0, "f1913b1f597e0948cca0d6f28d5ec2a8fcef9f31d4dd884497e6756900f8a66b", "" },
    { "8250C768-DBCF02DFB9E3F8FB-F375BB83352BFB1B-E60C3CA4", "653b77c77a94d42d6f40319c29fc82219f8965a93ce9220a59581e7497c9d49b", ",169,208,210,226,238,", true, 2, 0, "f1913b1f597e0948cca0d6f28d5ec2a8fcef9f31d4dd884497e6756900f8a66b", "" },
    { "8250C768-DBCF02DFB9E3F8FB-F375BB83352BFB1B-E60C3CA4", "e74c9a864e44134110b4c13854a7a3623c8707038debd19d3622d79dac7341c5", ",169,208,210,226,238,", true, 2, 0, "f1913b1f597e0948cca0d6f28d5ec2a8fcef9f31d4dd884497e6756900f8a66b", "" },
    { "8250C768-DE0359DBD47D7FAB-560C753DAD5555AD", "917e8b2ff5a330dea5275e2c7069b194db42dfc300993c567dc7368f7c656207", ",169,208,210,226,238,", true, 2, 0, "c74461826f3c8c45864396076f25747732b8044adb246c694d4cdf6b31a3ca60", "" },
    { "8250C768-DE0359DBD47D7FAB-560C753DAD5555AD", "e4fc51a7d05d83f29ef425d77a4f76c77abeb826c4b731105e5e803aa1e9c0ad", ",169,208,210,226,238,", true, 2, 0, "c74461826f3c8c45864396076f25747732b8044adb246c694d4cdf6b31a3ca60", "" },
    { "8250C768-DE0359DBD47D7FAB-560C753DAD5555AD-0AC8AC8F", "3cdfba38e91918aeab2e377ee60be0fb6bea0053e2c3ae2ce9e87110e5687041", ",169,208,210,226,238,", true, 2, 0, "c74461826f3c8c45864396076f25747732b8044adb246c694d4cdf6b31a3ca60", "" },
    { "8250C768-DE0359DBD47D7FAB-560C753DAD5555AD-0AC8AC8F", "be3a41454266d0f57fa650ccd355751865bfeff6f77632e40003dbea58d42dea", ",169,208,210,226,238,", true, 2, 0, "c74461826f3c8c45864396076f25747732b8044adb246c694d4cdf6b31a3ca60", "" },
    { "8250C768-DE1CFCF0B0BBBC28-A40FB814B90C42C3", "07fb78eee8807afb8f61c38d38fb13a052216bd5a13a8cd367f88e12aaff15b5", ",169,208,210,226,238,", true, 2, 0, "fc81d48e84840dfbabee82794dbcab94d9e1b11952a8786694d25e38ff117efa", "" },
    { "8250C768-DE1CFCF0B0BBBC28-A40FB814B90C42C3", "840364e686233d5ff26af3f3e84b0b93c288a8446655cefebc4b81e5cc269ebf", ",169,208,210,226,238,", true, 2, 0, "fc81d48e84840dfbabee82794dbcab94d9e1b11952a8786694d25e38ff117efa", "" },
    { "8250C768-DE1CFCF0B0BBBC28-A40FB814B90C42C3-E01C738E", "0f113db8fb7f66704de8bf7241cc4a71d76bbacd57ee6d3dd393c653849c014f", ",169,208,210,226,238,", true, 2, 0, "fc81d48e84840dfbabee82794dbcab94d9e1b11952a8786694d25e38ff117efa", "" },
    { "8250C768-DE1CFCF0B0BBBC28-A40FB814B90C42C3-E01C738E", "8c45609263cc283eb2c2c9e7235c705b0fab2975274aef1a90bf4626b9e18ab9", ",169,208,210,226,238,", true, 2, 0, "fc81d48e84840dfbabee82794dbcab94d9e1b11952a8786694d25e38ff117efa", "" },
    { "8250C768-E49029E25209733B-B52D04B35FF2B558-238CF51C", "0cb8f4d518b971cada7fda5c77114ec6fd8a8e49f68443f29439c0c2f5d7d8d7", ",169,", false, 0, -1, "", "scene_catalog_vertex: 5977de9b3accea10" },
    { "8250C768-E49029E25209733B-B52D04B35FF2B558-238CF51C", "0cb8f4d518b971cada7fda5c77114ec6fd8a8e49f68443f29439c0c2f5d7d8d7", ",208,210,226,238,", true, 2, 1, "ebb1d3950b3507c02b498e6336dcde4441409b9320176206fb13e4a16033a008", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3", "91ebb17b3a034328fa663981fba328ef904101d7a802c44a405e1d58242a71d2", ",169,208,", true, 2, 1, "f7920ef416246010151f53e52ee68ea2112ff84ea8aadecdbacda49d30714ea3", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3", "91ebb17b3a034328fa663981fba328ef904101d7a802c44a405e1d58242a71d2", ",226,", true, 3, 1, "27854c2032bcde953853e81e62e1ea0025e5355fe9966a020189339556237708", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3", "91ebb17b3a034328fa663981fba328ef904101d7a802c44a405e1d58242a71d2", ",210,", true, 3, 1, "8367f67ce3ada7698f9eab7961603b2d10f96ee11c6aceffa91b1c89c5616232", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3", "91ebb17b3a034328fa663981fba328ef904101d7a802c44a405e1d58242a71d2", ",238,", true, 6, 1, "b66d882291fdf9db265c68df24df06491269a5adf954ffc60960fcf59103d215", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3", "f94475d715a1d562a7f307987a45993f19b1a892e24529d2f1be33060df9002b", ",169,208,", true, 2, 1, "f7920ef416246010151f53e52ee68ea2112ff84ea8aadecdbacda49d30714ea3", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3", "f94475d715a1d562a7f307987a45993f19b1a892e24529d2f1be33060df9002b", ",226,", true, 3, 1, "27854c2032bcde953853e81e62e1ea0025e5355fe9966a020189339556237708", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3", "f94475d715a1d562a7f307987a45993f19b1a892e24529d2f1be33060df9002b", ",210,", true, 3, 1, "8367f67ce3ada7698f9eab7961603b2d10f96ee11c6aceffa91b1c89c5616232", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3", "f94475d715a1d562a7f307987a45993f19b1a892e24529d2f1be33060df9002b", ",238,", true, 6, 1, "b66d882291fdf9db265c68df24df06491269a5adf954ffc60960fcf59103d215", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-0662A8C9", "7a55901f4c6a4a6f84bafb93276122b2d876771ce13d8d60b163ca0598dc3f52", ",169,208,", true, 2, 0, "f7920ef416246010151f53e52ee68ea2112ff84ea8aadecdbacda49d30714ea3", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-0662A8C9", "7a55901f4c6a4a6f84bafb93276122b2d876771ce13d8d60b163ca0598dc3f52", ",226,", true, 3, 0, "27854c2032bcde953853e81e62e1ea0025e5355fe9966a020189339556237708", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-0662A8C9", "7a55901f4c6a4a6f84bafb93276122b2d876771ce13d8d60b163ca0598dc3f52", ",210,", true, 3, 0, "8367f67ce3ada7698f9eab7961603b2d10f96ee11c6aceffa91b1c89c5616232", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-0662A8C9", "7a55901f4c6a4a6f84bafb93276122b2d876771ce13d8d60b163ca0598dc3f52", ",238,", true, 6, 0, "b66d882291fdf9db265c68df24df06491269a5adf954ffc60960fcf59103d215", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-0662A8C9", "845fcc820ae70d0acb52be901b888185c37a518094ec4a9dc673468f0e68f182", ",169,208,", true, 2, 0, "f7920ef416246010151f53e52ee68ea2112ff84ea8aadecdbacda49d30714ea3", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-0662A8C9", "845fcc820ae70d0acb52be901b888185c37a518094ec4a9dc673468f0e68f182", ",226,", true, 3, 0, "27854c2032bcde953853e81e62e1ea0025e5355fe9966a020189339556237708", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-0662A8C9", "845fcc820ae70d0acb52be901b888185c37a518094ec4a9dc673468f0e68f182", ",210,", true, 3, 0, "8367f67ce3ada7698f9eab7961603b2d10f96ee11c6aceffa91b1c89c5616232", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-0662A8C9", "845fcc820ae70d0acb52be901b888185c37a518094ec4a9dc673468f0e68f182", ",238,", true, 6, 0, "b66d882291fdf9db265c68df24df06491269a5adf954ffc60960fcf59103d215", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-711F61D9", "555dc5427ae4a1e703a4b0f9c684eb8917298a15fcc982203feffc875167b7ce", ",169,208,", true, 2, 1, "f7920ef416246010151f53e52ee68ea2112ff84ea8aadecdbacda49d30714ea3", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-711F61D9", "555dc5427ae4a1e703a4b0f9c684eb8917298a15fcc982203feffc875167b7ce", ",226,", true, 3, 1, "27854c2032bcde953853e81e62e1ea0025e5355fe9966a020189339556237708", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-711F61D9", "555dc5427ae4a1e703a4b0f9c684eb8917298a15fcc982203feffc875167b7ce", ",210,", true, 3, 1, "8367f67ce3ada7698f9eab7961603b2d10f96ee11c6aceffa91b1c89c5616232", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-711F61D9", "555dc5427ae4a1e703a4b0f9c684eb8917298a15fcc982203feffc875167b7ce", ",238,", true, 6, 1, "b66d882291fdf9db265c68df24df06491269a5adf954ffc60960fcf59103d215", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-711F61D9", "cf3ee6c2a67de38f171c3a9cd06c05373c150dc81f2c76ee7981f2f638e35097", ",169,208,", true, 2, 1, "f7920ef416246010151f53e52ee68ea2112ff84ea8aadecdbacda49d30714ea3", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-711F61D9", "cf3ee6c2a67de38f171c3a9cd06c05373c150dc81f2c76ee7981f2f638e35097", ",226,", true, 3, 1, "27854c2032bcde953853e81e62e1ea0025e5355fe9966a020189339556237708", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-711F61D9", "cf3ee6c2a67de38f171c3a9cd06c05373c150dc81f2c76ee7981f2f638e35097", ",210,", true, 3, 1, "8367f67ce3ada7698f9eab7961603b2d10f96ee11c6aceffa91b1c89c5616232", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-711F61D9", "cf3ee6c2a67de38f171c3a9cd06c05373c150dc81f2c76ee7981f2f638e35097", ",238,", true, 6, 1, "b66d882291fdf9db265c68df24df06491269a5adf954ffc60960fcf59103d215", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-EC2C7144", "ff7fd80537720853682be27b4bb18f8bdf3fd83531820525451dc1771e71e493", ",169,208,", false, 2, -1, "f7920ef416246010151f53e52ee68ea2112ff84ea8aadecdbacda49d30714ea3", "scene_specialization: alpha 6 samples 4 program 0x10710909 context 0x8 interpolators 0x7f00ff" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-EC2C7144", "ff7fd80537720853682be27b4bb18f8bdf3fd83531820525451dc1771e71e493", ",226,", true, 3, 2, "27854c2032bcde953853e81e62e1ea0025e5355fe9966a020189339556237708", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-EC2C7144", "ff7fd80537720853682be27b4bb18f8bdf3fd83531820525451dc1771e71e493", ",210,", true, 3, 2, "8367f67ce3ada7698f9eab7961603b2d10f96ee11c6aceffa91b1c89c5616232", "" },
    { "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-EC2C7144", "ff7fd80537720853682be27b4bb18f8bdf3fd83531820525451dc1771e71e493", ",238,", true, 6, 2, "b66d882291fdf9db265c68df24df06491269a5adf954ffc60960fcf59103d215", "" },
    { "8250C768-E6559262B85B46FF-CD99817770DB94B6-BA6C3695", "54dbc9a9553ebae83323cd59bb96dfeb2deb6728165269db4e7d2f2faa1c3d42", ",169,", false, 0, -1, "", "scene_catalog_vertex: 3e235bbb08f4b023" },
    { "8250C768-E6559262B85B46FF-CD99817770DB94B6-BA6C3695", "54dbc9a9553ebae83323cd59bb96dfeb2deb6728165269db4e7d2f2faa1c3d42", ",208,210,226,238,", true, 2, 1, "beb31ee62ea4425c9ae62197b19a9b620b257a10dd6d14249c894313a5625292", "" },
    { "8250C768-E6E9B891D8079B89-6174F2CAABC2C726", "08d937a8b2724eb0aa13c1e62312424d3b60bd283d1720aee18152241b124bbb", ",169,208,210,226,238,", true, 2, 0, "0c88b18b710f6a435d4750e16a36a2f0945740876aa6d4c26e4fe845d33dd43f", "" },
    { "8250C768-E6E9B891D8079B89-6174F2CAABC2C726", "6a021a51f24956fe53261727ad510d6d8053a52c78d828a3168c268eb7e2e155", ",169,208,210,226,238,", true, 2, 0, "0c88b18b710f6a435d4750e16a36a2f0945740876aa6d4c26e4fe845d33dd43f", "" },
    { "8250C768-E6E9B891D8079B89-6174F2CAABC2C726-C2EC27F4", "1dc16f14493706af29ccc5bb0df4948597bcb40a535427c85603ae5b7b2e762f", ",169,208,210,226,238,", true, 2, 0, "0c88b18b710f6a435d4750e16a36a2f0945740876aa6d4c26e4fe845d33dd43f", "" },
    { "8250C768-E88067C94CBD1FFC-F36CCEFF549A2660", "26c5d2bcc9f463fe2fb3328bdecb857fcfd4e0c3328fea643301879e67c0ec11", ",169,208,210,226,238,", true, 2, 0, "537b49c4255581b5c450a0c96daf19451c0e7d0f422bca40d301f494b697a26c", "" },
    { "8250C768-E88067C94CBD1FFC-F36CCEFF549A2660", "b8a9d00c764a8b49632060713cf7f14df7b20bd82c8e4e93ec1b344cde4bd24e", ",169,208,210,226,238,", true, 2, 0, "537b49c4255581b5c450a0c96daf19451c0e7d0f422bca40d301f494b697a26c", "" },
    { "8250C768-E88067C94CBD1FFC-F36CCEFF549A2660-B122BE50", "523c0d462d3186309a9036afa9f04a0f9a366b96e40cdc0242698750542ca5ea", ",169,208,210,226,238,", true, 2, 0, "537b49c4255581b5c450a0c96daf19451c0e7d0f422bca40d301f494b697a26c", "" },
    { "8250C768-E88067C94CBD1FFC-F36CCEFF549A2660-B122BE50", "52c543075150d8af954558258b55e601e8aae19099a0f93837743375a2d8a709", ",169,208,210,226,238,", true, 2, 0, "537b49c4255581b5c450a0c96daf19451c0e7d0f422bca40d301f494b697a26c", "" },
    { "8250C768-EDD258C38F189C07-5F5846543451445E", "f670925fc82d6946e5c6ade58fc1aa0df358744fa2cda64e67d043b0ce3f49fd", ",169,208,210,226,238,", true, 2, 1, "a0c79667d59845b9e5563347ebfdba0b14405829118aaf52ab54283b6d611076", "" },
    { "8250C768-EDD258C38F189C07-5F5846543451445E-3D5A7413", "f87b39b9e0960c027b548074f8cd797fa5702a7625deb96998e49df7d1447150", ",169,208,210,226,238,", true, 2, 1, "a0c79667d59845b9e5563347ebfdba0b14405829118aaf52ab54283b6d611076", "" },
    { "8250C768-EDD258C38F189C07-CE177871D3ABC6AE", "1f31abb403332cf9e15bc3a6ace0f170f347a9b4db3efa82d2199c03545cb95b", ",169,208,210,226,238,", true, 2, 0, "7bc07e48da991118231ab560f1db25c05a05494cb5f971d90646b5e2025d8306", "" },
    { "8250C768-EDD258C38F189C07-CE177871D3ABC6AE", "98517a864f5aad847a33043a917619f1b307dd6ef060884b31632f38626dee60", ",169,208,210,226,238,", true, 2, 1, "7bc07e48da991118231ab560f1db25c05a05494cb5f971d90646b5e2025d8306", "" },
    { "8250C768-EDD258C38F189C07-CE177871D3ABC6AE-5961BA14", "5c115711ceedc55642078483c052a369baedec036c65ed3a4892242dddce6618", ",169,208,210,226,238,", true, 2, 1, "7bc07e48da991118231ab560f1db25c05a05494cb5f971d90646b5e2025d8306", "" },
    { "8250C768-EDD258C38F189C07-CE177871D3ABC6AE-5961BA14", "b226ee7fd5665fedadac3500dbaa187180db3d8da8d08cdb3e1462e0edbcc037", ",169,208,210,226,238,", true, 2, 1, "7bc07e48da991118231ab560f1db25c05a05494cb5f971d90646b5e2025d8306", "" },
    { "8250C768-EDD258C38F189C07-CE177871D3ABC6AE-FAD9C851", "ad8a2fd733e604b7d8988fbc6215fdb0fbca645ec67bb0eb3aa386766f6955e6", ",169,208,210,226,238,", true, 2, 0, "7bc07e48da991118231ab560f1db25c05a05494cb5f971d90646b5e2025d8306", "" },
    { "8250C768-EDD258C38F189C07-CE177871D3ABC6AE-FAD9C851", "f701ab56015914044b5e5b504789456dbac8eb13c641c2f1386a6698a927f06f", ",169,208,210,226,238,", true, 2, 0, "7bc07e48da991118231ab560f1db25c05a05494cb5f971d90646b5e2025d8306", "" },
    { "8250C768-F34C58CCE2C2DE45-ECA19F8D70F05679", "4fc1806eba0d77b31895bf06c0c2986aafadd56fcfa8d56923fcb2d88985c405", ",226,238,", true, 2, 1, "149c045d0ac84eb30c08fac5df8976b662197fe87e1313a41af1810d47b149f5", "" },
    { "8250C768-F34C58CCE2C2DE45-ECA19F8D70F05679", "4fc1806eba0d77b31895bf06c0c2986aafadd56fcfa8d56923fcb2d88985c405", ",169,208,210,", true, 2, 1, "6ec5c201b9615bea853136c91829fa725006636e823e9476025729f8e885d2dc", "" },
    { "8250C768-F34C58CCE2C2DE45-ECA19F8D70F05679", "5a1d9ca4bf432805351af15754e651090341b3f8c8319cc665b4598498ad7a22", ",226,238,", true, 2, 1, "149c045d0ac84eb30c08fac5df8976b662197fe87e1313a41af1810d47b149f5", "" },
    { "8250C768-F34C58CCE2C2DE45-ECA19F8D70F05679", "5a1d9ca4bf432805351af15754e651090341b3f8c8319cc665b4598498ad7a22", ",169,208,210,", true, 2, 1, "6ec5c201b9615bea853136c91829fa725006636e823e9476025729f8e885d2dc", "" },
    { "8250C768-F34C58CCE2C2DE45-ECA19F8D70F05679-AA075757", "6e7ee33c13921479d3568b31f3e314585f6f86a61f3acd78ad287989b1fca91f", ",226,238,", true, 2, 1, "149c045d0ac84eb30c08fac5df8976b662197fe87e1313a41af1810d47b149f5", "" },
    { "8250C768-F34C58CCE2C2DE45-ECA19F8D70F05679-AA075757", "6e7ee33c13921479d3568b31f3e314585f6f86a61f3acd78ad287989b1fca91f", ",169,208,210,", true, 2, 1, "6ec5c201b9615bea853136c91829fa725006636e823e9476025729f8e885d2dc", "" },
    { "8250C768-F34C58CCE2C2DE45-ECA19F8D70F05679-AA075757", "a8353c3941c7be33a6c6c6304aaefa79fb7500abae68cfdcb4847c6df8ebfd89", ",226,238,", true, 2, 1, "149c045d0ac84eb30c08fac5df8976b662197fe87e1313a41af1810d47b149f5", "" },
    { "8250C768-F34C58CCE2C2DE45-ECA19F8D70F05679-AA075757", "a8353c3941c7be33a6c6c6304aaefa79fb7500abae68cfdcb4847c6df8ebfd89", ",169,208,210,", true, 2, 1, "6ec5c201b9615bea853136c91829fa725006636e823e9476025729f8e885d2dc", "" },
    { "8250C768-F6340E3997B46233-6174F2CAABC2C726-FE30AE00", "5397713890eda50125a2e628843d5eea6516afb4f00c20d71f96874b54a99522", ",169,", false, 0, -1, "", "scene_catalog_vertex: 158617168cf848bc" },
    { "8250C768-F6340E3997B46233-6174F2CAABC2C726-FE30AE00", "5397713890eda50125a2e628843d5eea6516afb4f00c20d71f96874b54a99522", ",208,210,226,238,", true, 2, 1, "9c9a5a7e07dc1a328c98ee1c8f66760771b8e48b3358f06a85aebd0bc5cf9226", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558", "054a556802385aa96ac920c8276b875fcebddc4af3db957c35332da502bbf02e", ",169,208,", true, 2, 1, "9a5c2a748ca2764103a78df05e1f7669499b89cb62ac9893f45cd62ae984d9ea", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558", "054a556802385aa96ac920c8276b875fcebddc4af3db957c35332da502bbf02e", ",210,226,", true, 3, 1, "9f18779b31e46c10bf1e97b3ad2d3f7b7b29ca20f7aa6bf1ce4082d24cc9906e", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558", "054a556802385aa96ac920c8276b875fcebddc4af3db957c35332da502bbf02e", ",238,", true, 6, 1, "6ffe39fac045fd0457f01b02f1d9b8b13952d313d713fc144f3ef5d7426aa99a", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558", "30c74a1fae4f03e58c7a649be62a16f06342e83e3894183f5ceaccfc16d5694d", ",169,208,", true, 2, 1, "9a5c2a748ca2764103a78df05e1f7669499b89cb62ac9893f45cd62ae984d9ea", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558", "30c74a1fae4f03e58c7a649be62a16f06342e83e3894183f5ceaccfc16d5694d", ",210,226,", true, 3, 1, "9f18779b31e46c10bf1e97b3ad2d3f7b7b29ca20f7aa6bf1ce4082d24cc9906e", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558", "30c74a1fae4f03e58c7a649be62a16f06342e83e3894183f5ceaccfc16d5694d", ",238,", true, 6, 1, "6ffe39fac045fd0457f01b02f1d9b8b13952d313d713fc144f3ef5d7426aa99a", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-16119AAF", "5257a1c043702b4c581b6a5d1a20ba37efe236b3010819b676be3b33e4832f87", ",169,208,", false, 2, -1, "9a5c2a748ca2764103a78df05e1f7669499b89cb62ac9893f45cd62ae984d9ea", "scene_specialization: alpha 6 samples 1 program 0x10010002 context 0x4 interpolators 0x10001" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-16119AAF", "5257a1c043702b4c581b6a5d1a20ba37efe236b3010819b676be3b33e4832f87", ",210,226,", true, 3, 2, "9f18779b31e46c10bf1e97b3ad2d3f7b7b29ca20f7aa6bf1ce4082d24cc9906e", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-16119AAF", "5257a1c043702b4c581b6a5d1a20ba37efe236b3010819b676be3b33e4832f87", ",238,", true, 6, 2, "6ffe39fac045fd0457f01b02f1d9b8b13952d313d713fc144f3ef5d7426aa99a", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-2B8F293B", "435d9087a2c4d9f09b0355eeaa6e0592491adb428dee9cc5d4e05cbbb0b6294e", ",169,208,", false, 2, -1, "9a5c2a748ca2764103a78df05e1f7669499b89cb62ac9893f45cd62ae984d9ea", "scene_specialization: alpha 4 samples 1 program 0x10010002 context 0x4 interpolators 0x10001 user clip plane 0" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-2B8F293B", "435d9087a2c4d9f09b0355eeaa6e0592491adb428dee9cc5d4e05cbbb0b6294e", ",210,226,", false, 3, -1, "9f18779b31e46c10bf1e97b3ad2d3f7b7b29ca20f7aa6bf1ce4082d24cc9906e", "scene_specialization: alpha 4 samples 1 program 0x10010002 context 0x4 interpolators 0x10001 user clip plane 0" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-2B8F293B", "435d9087a2c4d9f09b0355eeaa6e0592491adb428dee9cc5d4e05cbbb0b6294e", ",238,", true, 6, 4, "6ffe39fac045fd0457f01b02f1d9b8b13952d313d713fc144f3ef5d7426aa99a", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-2B8F293B", "ba13e7597a0abd7321f6e3f8e4e9387281190c170bfe86fa9cca5cf0c2fde090", ",169,208,", true, 2, 1, "9a5c2a748ca2764103a78df05e1f7669499b89cb62ac9893f45cd62ae984d9ea", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-2B8F293B", "ba13e7597a0abd7321f6e3f8e4e9387281190c170bfe86fa9cca5cf0c2fde090", ",210,226,", true, 3, 1, "9f18779b31e46c10bf1e97b3ad2d3f7b7b29ca20f7aa6bf1ce4082d24cc9906e", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-2B8F293B", "ba13e7597a0abd7321f6e3f8e4e9387281190c170bfe86fa9cca5cf0c2fde090", ",238,", true, 6, 1, "6ffe39fac045fd0457f01b02f1d9b8b13952d313d713fc144f3ef5d7426aa99a", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-FE0E000C", "265ae63ce64e63ad5ca4e1b4cd4a1775b9ae267db66235c4e45cb6a00872acb2", ",169,208,", true, 2, 0, "9a5c2a748ca2764103a78df05e1f7669499b89cb62ac9893f45cd62ae984d9ea", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-FE0E000C", "265ae63ce64e63ad5ca4e1b4cd4a1775b9ae267db66235c4e45cb6a00872acb2", ",210,226,", true, 3, 0, "9f18779b31e46c10bf1e97b3ad2d3f7b7b29ca20f7aa6bf1ce4082d24cc9906e", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-FE0E000C", "265ae63ce64e63ad5ca4e1b4cd4a1775b9ae267db66235c4e45cb6a00872acb2", ",238,", true, 6, 0, "6ffe39fac045fd0457f01b02f1d9b8b13952d313d713fc144f3ef5d7426aa99a", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-FE0E000C", "a131272329eb37e7e2463caaddbdf54aac1d9fa5d51f64f57bb33c1b42a38017", ",169,208,", true, 2, 0, "9a5c2a748ca2764103a78df05e1f7669499b89cb62ac9893f45cd62ae984d9ea", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-FE0E000C", "a131272329eb37e7e2463caaddbdf54aac1d9fa5d51f64f57bb33c1b42a38017", ",210,226,", true, 3, 0, "9f18779b31e46c10bf1e97b3ad2d3f7b7b29ca20f7aa6bf1ce4082d24cc9906e", "" },
    { "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-FE0E000C", "a131272329eb37e7e2463caaddbdf54aac1d9fa5d51f64f57bb33c1b42a38017", ",238,", true, 6, 0, "6ffe39fac045fd0457f01b02f1d9b8b13952d313d713fc144f3ef5d7426aa99a", "" },
    { "8250C768-F95D105E51CF1A19-884C15F5AE654F89", "64c30b62df57c9e657ace70c0918797703d18a08aa52b2d71086037d521e39a0", ",169,208,210,226,238,", true, 2, 0, "878d2028a78ae38a9e9595e9c4d6bb2dc06e682e051548407576713f0aab7d1d", "" },
    { "8250C768-F95D105E51CF1A19-884C15F5AE654F89", "a7d6b5bb4f7b40a0bcfea528dec0f830ae0e55285e54d66b3a026c627bd6f6fb", ",169,208,210,226,238,", true, 2, 0, "878d2028a78ae38a9e9595e9c4d6bb2dc06e682e051548407576713f0aab7d1d", "" },
    { "8250C768-F95D105E51CF1A19-884C15F5AE654F89-8904A42E", "3b57d787c95a0ae278589ad831270e1807dc769636896d0aba185c431afbce90", ",169,208,210,226,238,", true, 2, 0, "878d2028a78ae38a9e9595e9c4d6bb2dc06e682e051548407576713f0aab7d1d", "" },
    { "8250C768-F95D105E51CF1A19-884C15F5AE654F89-8904A42E", "c682f36e6e6833508525a97ac9135723605b3e597e15ae0dfc0587657ae3339c", ",169,208,210,226,238,", true, 2, 0, "878d2028a78ae38a9e9595e9c4d6bb2dc06e682e051548407576713f0aab7d1d", "" },
    { "8250C768-F97503ABD33B9CE7-8DB49F09453923E2", "411e978aee79845a74fb897c301d88ad6522aa03f50ffa010af0aca7297e67c1", ",169,208,210,226,238,", true, 2, 0, "ceeccc43c9fc4a4b2ddc09465205fce7c67f209ee2548c7fed854787fa6006cf", "" },
    { "8250C768-F97503ABD33B9CE7-8DB49F09453923E2", "c8c3dfd606ef6ef5c1abdc3c939d619db01ee50cf5379366ee127bd415a6749c", ",169,208,210,226,238,", true, 2, 0, "ceeccc43c9fc4a4b2ddc09465205fce7c67f209ee2548c7fed854787fa6006cf", "" },
    { "8250C768-F97503ABD33B9CE7-8DB49F09453923E2-FEF21929", "6681e8466563a8a07a28634fb2f3e6342b2886a5e1c68f8d85c1d472f08bbd70", ",169,208,210,226,238,", true, 2, 0, "ceeccc43c9fc4a4b2ddc09465205fce7c67f209ee2548c7fed854787fa6006cf", "" },
    { "8250C768-F97503ABD33B9CE7-8DB49F09453923E2-FEF21929", "6812255bbfee2a1a1062bbb0afbd8da23f21faf7de10cc3265f51665bb79f2b8", ",169,208,210,226,238,", true, 2, 0, "ceeccc43c9fc4a4b2ddc09465205fce7c67f209ee2548c7fed854787fa6006cf", "" },
    { "8250C768-FFFA242DF0B3CEE5-3B221871C058B01A", "e54c9c555c3f5622e0dc40bb8890e1b56331799309ac0c922ed1ee6820b0cb5a", ",169,208,210,226,238,", true, 2, 0, "a7c34a28a5afbf5e290c28bbfaa8d61ab977b430e686adef5763b1e698db20b8", "" },
    { "8250C768-FFFA242DF0B3CEE5-3B221871C058B01A-AC4C8410", "405383a0cef1db197993338ef8d3a72d0219065a113d95e0f2a7c20d76cc596f", ",169,208,210,226,238,", true, 2, 0, "a7c34a28a5afbf5e290c28bbfaa8d61ab977b430e686adef5763b1e698db20b8", "" },
    { "8250C768-FFFA242DF0B3CEE5-3B221871C058B01A-AC4C8410", "d2465a782275661bbfb57ab5de493b481e01bbac01443d04290479a5ab860d86", ",169,208,210,226,238,", true, 2, 0, "a7c34a28a5afbf5e290c28bbfaa8d61ab977b430e686adef5763b1e698db20b8", "" },
    { "8250CB98-05627AB1EC0253DD-F879A7E6293CDAE1", "598cd2ebddcbd7399fedc3a764914eb0e0f92ef3189f0298b48c876fd72f2a95", ",226,238,", true, 2, 1, "63f477e393bcbc626beef4f4fe6c28bb7b430bf5d55dc742d8e825c163473403", "" },
    { "8250CB98-05627AB1EC0253DD-F879A7E6293CDAE1", "598cd2ebddcbd7399fedc3a764914eb0e0f92ef3189f0298b48c876fd72f2a95", ",169,208,210,", true, 2, 1, "8617e2043d35ab068d5fc474a3b08d54c82e9ab3daf9863e19a9513ae728f664", "" },
    { "8250CB98-05627AB1EC0253DD-F879A7E6293CDAE1", "ece83803939636b1b666aca66d4d92bd9bb72bc04482bfadbd9b85f1f39b9233", ",226,238,", true, 2, 1, "63f477e393bcbc626beef4f4fe6c28bb7b430bf5d55dc742d8e825c163473403", "" },
    { "8250CB98-05627AB1EC0253DD-F879A7E6293CDAE1", "ece83803939636b1b666aca66d4d92bd9bb72bc04482bfadbd9b85f1f39b9233", ",169,208,210,", true, 2, 1, "8617e2043d35ab068d5fc474a3b08d54c82e9ab3daf9863e19a9513ae728f664", "" },
    { "8250CB98-05627AB1EC0253DD-F879A7E6293CDAE1-61A48F7B", "43757094dd50cfaa50d55e47e98bca757cddfe7607943eff065bbb3d71177775", ",226,238,", true, 2, 1, "63f477e393bcbc626beef4f4fe6c28bb7b430bf5d55dc742d8e825c163473403", "" },
    { "8250CB98-05627AB1EC0253DD-F879A7E6293CDAE1-61A48F7B", "43757094dd50cfaa50d55e47e98bca757cddfe7607943eff065bbb3d71177775", ",169,208,210,", true, 2, 1, "8617e2043d35ab068d5fc474a3b08d54c82e9ab3daf9863e19a9513ae728f664", "" },
    { "8250CB98-05627AB1EC0253DD-F879A7E6293CDAE1-61A48F7B", "5c0332a9654f91027ec74ee54a8728626a955121043bf9551456b940f8c8d35f", ",226,238,", true, 2, 1, "63f477e393bcbc626beef4f4fe6c28bb7b430bf5d55dc742d8e825c163473403", "" },
    { "8250CB98-05627AB1EC0253DD-F879A7E6293CDAE1-61A48F7B", "5c0332a9654f91027ec74ee54a8728626a955121043bf9551456b940f8c8d35f", ",169,208,210,", true, 2, 1, "8617e2043d35ab068d5fc474a3b08d54c82e9ab3daf9863e19a9513ae728f664", "" },
    { "8250CB98-16250FF33CB232CD-21AE55916585DF1E", "8794aa79cf3f982132bb8315054b4832d677688ce0c1a6cf62ec81305c046e04", ",169,208,210,226,238,", true, 2, 1, "2f75847cd27b69d25a1dde4d1990aebb46423606ebe1342a96d303d9069a462f", "" },
    { "8250CB98-1F2CA6D6F91FE257-14B8C2B288D59748-86ED160E", "b85d879093f53aee2b7dd9778fab0db781e844e550a94753c06ef21398ac35eb", ",169,", false, 0, -1, "", "scene_catalog_vertex: 56e4e3b37fc91aa5" },
    { "8250CB98-1F2CA6D6F91FE257-14B8C2B288D59748-86ED160E", "b85d879093f53aee2b7dd9778fab0db781e844e550a94753c06ef21398ac35eb", ",208,210,226,238,", true, 2, 1, "e575f3e123d0315830f3c0d863378eabd0a71b2c47d5c2caf3539af510e4d121", "" },
    { "8250CB98-2565EED48174DDF1-DD419115B6929970", "78a0d80d1c02513edc098e49b39121b19f2a677ed06d90aeff7eb665aebeb583", ",169,208,210,226,", false, 0, -1, "", "" },
    { "8250CB98-2565EED48174DDF1-DD419115B6929970", "78a0d80d1c02513edc098e49b39121b19f2a677ed06d90aeff7eb665aebeb583", ",238,", true, 2, 1, "8ff8e5364e1808e02a4168482c70fb99f6fda67f29317a555c4a6ceb39dd6523", "" },
    { "8250CB98-2565EED48174DDF1-DD419115B6929970-E6E8B3C2", "c185f608ba8aeb8ed77dd34129721cce1895c7ee7e629a8aa598ac3812253636", ",169,208,210,226,", false, 0, -1, "", "" },
    { "8250CB98-2565EED48174DDF1-DD419115B6929970-E6E8B3C2", "c185f608ba8aeb8ed77dd34129721cce1895c7ee7e629a8aa598ac3812253636", ",238,", true, 2, 1, "8ff8e5364e1808e02a4168482c70fb99f6fda67f29317a555c4a6ceb39dd6523", "" },
    { "8250CB98-2846DBCE4A341430-C7061656274D3A19", "407e39a80d82a583e86f24de3395078032d5c70460a7ae83bc5384d5dc5a6882", ",169,208,210,226,238,", true, 2, 1, "98ad0ace796d050c73b87483a6e1440171558530e3ef4cbd1e70055c6b6e32a3", "" },
    { "8250CB98-2A09437689C6B225-5F5846543451445E-39CD276A", "e899187c539ee4286f0487aa6a1d9f79172d2646677c689b1daabc467e5ecf85", ",169,", false, 0, -1, "", "scene_catalog_vertex: 550656a3ed4856be" },
    { "8250CB98-2A09437689C6B225-5F5846543451445E-39CD276A", "e899187c539ee4286f0487aa6a1d9f79172d2646677c689b1daabc467e5ecf85", ",208,210,226,238,", true, 2, 1, "2cf3d7f4dbed8dc2960cc7333905cdc1d955db5057b3ea7d5b71e97538368567", "" },
    { "8250CB98-2B310A88379361F1-49A35B7DF1662579-873B07FF", "4d7bedb6d8111f2a9c22a297f680fb44da2393cc6fa4cc44ffaf9ffff1f57d9b", ",169,", false, 0, -1, "", "" },
    { "8250CB98-2B310A88379361F1-49A35B7DF1662579-873B07FF", "4d7bedb6d8111f2a9c22a297f680fb44da2393cc6fa4cc44ffaf9ffff1f57d9b", ",208,210,", true, 2, 1, "d675b4a051452ff263432736ea2f02cb7bc12b86cdf70f8ce4e0a63a3c69cbf4", "" },
    { "8250CB98-2B310A88379361F1-49A35B7DF1662579-873B07FF", "4d7bedb6d8111f2a9c22a297f680fb44da2393cc6fa4cc44ffaf9ffff1f57d9b", ",226,238,", true, 2, 1, "f10e15158c39bef29a34a1f2b4f496a65dcef39cf7d388345f4c762148ed74b5", "" },
    { "8250CB98-2B310A88379361F1-49A35B7DF1662579-873B07FF", "deb0b0eb638697f38cb2792033429d9cbddf7b8434139bd3ae3d9d52968a31f7", ",169,", false, 0, -1, "", "" },
    { "8250CB98-2B310A88379361F1-49A35B7DF1662579-873B07FF", "deb0b0eb638697f38cb2792033429d9cbddf7b8434139bd3ae3d9d52968a31f7", ",208,210,", true, 2, 1, "d675b4a051452ff263432736ea2f02cb7bc12b86cdf70f8ce4e0a63a3c69cbf4", "" },
    { "8250CB98-2B310A88379361F1-49A35B7DF1662579-873B07FF", "deb0b0eb638697f38cb2792033429d9cbddf7b8434139bd3ae3d9d52968a31f7", ",226,238,", true, 2, 1, "f10e15158c39bef29a34a1f2b4f496a65dcef39cf7d388345f4c762148ed74b5", "" },
    { "8250CB98-3327A2EDA4C90A07-4A47AB50DBEBE0E1", "65fba43a19e029e42adb3be0261dc6f7823f85268c3f84aacfb714c7540a878d", ",169,208,210,", true, 2, 1, "b1575eb667d9baf414ee1894bd36b7c4a942af63b760daad262e916cd995ed73", "" },
    { "8250CB98-3327A2EDA4C90A07-4A47AB50DBEBE0E1", "65fba43a19e029e42adb3be0261dc6f7823f85268c3f84aacfb714c7540a878d", ",226,238,", true, 2, 1, "bf3ef88c6afaa2edc8b22d3aa7ec0cc65ffde5d133519378b554360bf8c19445", "" },
    { "8250CB98-3327A2EDA4C90A07-4A47AB50DBEBE0E1", "a522c8029e14241698a6d65a7395fd42ea1dfe751da4688bd7878ad7a52b0203", ",169,208,210,", true, 2, 1, "b1575eb667d9baf414ee1894bd36b7c4a942af63b760daad262e916cd995ed73", "" },
    { "8250CB98-3327A2EDA4C90A07-4A47AB50DBEBE0E1", "a522c8029e14241698a6d65a7395fd42ea1dfe751da4688bd7878ad7a52b0203", ",226,238,", true, 2, 1, "bf3ef88c6afaa2edc8b22d3aa7ec0cc65ffde5d133519378b554360bf8c19445", "" },
    { "8250CB98-3327A2EDA4C90A07-4A47AB50DBEBE0E1-80BC42C8", "6c64bbdb69696d720c1d94a4c8dd1f53ae386b4a90d8ebca1919b8cdea2eca28", ",169,208,210,", true, 2, 1, "b1575eb667d9baf414ee1894bd36b7c4a942af63b760daad262e916cd995ed73", "" },
    { "8250CB98-3327A2EDA4C90A07-4A47AB50DBEBE0E1-80BC42C8", "6c64bbdb69696d720c1d94a4c8dd1f53ae386b4a90d8ebca1919b8cdea2eca28", ",226,238,", true, 2, 1, "bf3ef88c6afaa2edc8b22d3aa7ec0cc65ffde5d133519378b554360bf8c19445", "" },
    { "8250CB98-3327A2EDA4C90A07-4A47AB50DBEBE0E1-80BC42C8", "7347da5b2330ca0c4bbe752db83f4230bdf02b9fb1bb4d9ec00fce6c312f3298", ",169,208,210,", true, 2, 1, "b1575eb667d9baf414ee1894bd36b7c4a942af63b760daad262e916cd995ed73", "" },
    { "8250CB98-3327A2EDA4C90A07-4A47AB50DBEBE0E1-80BC42C8", "7347da5b2330ca0c4bbe752db83f4230bdf02b9fb1bb4d9ec00fce6c312f3298", ",226,238,", true, 2, 1, "bf3ef88c6afaa2edc8b22d3aa7ec0cc65ffde5d133519378b554360bf8c19445", "" },
    { "8250CB98-3327A2EDA4C90A07-4A47AB50DBEBE0E1-ED518EC3", "81cda94b68e0a3b14b05b830d7cb15009d4d34fd8437f819882374d9ff33ed9e", ",169,208,210,", true, 2, 0, "b1575eb667d9baf414ee1894bd36b7c4a942af63b760daad262e916cd995ed73", "" },
    { "8250CB98-3327A2EDA4C90A07-4A47AB50DBEBE0E1-ED518EC3", "81cda94b68e0a3b14b05b830d7cb15009d4d34fd8437f819882374d9ff33ed9e", ",226,238,", true, 2, 0, "bf3ef88c6afaa2edc8b22d3aa7ec0cc65ffde5d133519378b554360bf8c19445", "" },
    { "8250CB98-3327A2EDA4C90A07-4A47AB50DBEBE0E1-ED518EC3", "8b1fa60783510b62e2255951543e6e1ec900192f858d6710d801b9682766785d", ",169,208,210,", true, 2, 0, "b1575eb667d9baf414ee1894bd36b7c4a942af63b760daad262e916cd995ed73", "" },
    { "8250CB98-3327A2EDA4C90A07-4A47AB50DBEBE0E1-ED518EC3", "8b1fa60783510b62e2255951543e6e1ec900192f858d6710d801b9682766785d", ",226,238,", true, 2, 0, "bf3ef88c6afaa2edc8b22d3aa7ec0cc65ffde5d133519378b554360bf8c19445", "" },
    { "8250CB98-4A4A5E1389F8A092-FCAEC4EC10703647", "505d37dedd85185cb8f020ff598b498e04cda021e7be2babbdc241c0f6e93c40", ",169,208,210,226,238,", true, 2, 1, "17de43d386c1f0c917e831ae580e86c474b448adfbde2978e5c7aad435778a71", "" },
    { "8250CB98-4AD503F4FFFFDBFE-5C21BE9B15F47834", "db0c3ffcb7327f62fc8270b8c62d37d318f908f696fbdfc671b25ad587fb6a67", ",169,208,210,", true, 2, 1, "154ec10e75f45946a17e393eff8c145ba20413a1f0067a5b98ddeaeec54f6a1f", "" },
    { "8250CB98-4AD503F4FFFFDBFE-5C21BE9B15F47834", "db0c3ffcb7327f62fc8270b8c62d37d318f908f696fbdfc671b25ad587fb6a67", ",226,238,", true, 2, 1, "1a4c374ac91ed0d6f84d4cf41bf0951ccfbf3c09f5c5e5c2765d504e24f7cd5e", "" },
    { "8250CB98-4AD503F4FFFFDBFE-5C21BE9B15F47834", "de4b74e1fbcbc7c4e06178136d9016c68fa44a8ce8fdcfce14eea7bc3dc8133f", ",169,208,210,", true, 2, 1, "154ec10e75f45946a17e393eff8c145ba20413a1f0067a5b98ddeaeec54f6a1f", "" },
    { "8250CB98-4AD503F4FFFFDBFE-5C21BE9B15F47834", "de4b74e1fbcbc7c4e06178136d9016c68fa44a8ce8fdcfce14eea7bc3dc8133f", ",226,238,", true, 2, 1, "1a4c374ac91ed0d6f84d4cf41bf0951ccfbf3c09f5c5e5c2765d504e24f7cd5e", "" },
    { "8250CB98-4AD503F4FFFFDBFE-5C21BE9B15F47834-446AB03A", "4be736b283496d329f04aeb7406c0b083b81e9391ca08873dfef170ae1e42057", ",169,208,210,", true, 2, 1, "154ec10e75f45946a17e393eff8c145ba20413a1f0067a5b98ddeaeec54f6a1f", "" },
    { "8250CB98-4AD503F4FFFFDBFE-5C21BE9B15F47834-446AB03A", "4be736b283496d329f04aeb7406c0b083b81e9391ca08873dfef170ae1e42057", ",226,238,", true, 2, 1, "1a4c374ac91ed0d6f84d4cf41bf0951ccfbf3c09f5c5e5c2765d504e24f7cd5e", "" },
    { "8250CB98-4AD503F4FFFFDBFE-5C21BE9B15F47834-446AB03A", "eb640e4d714dfb106eb86db25af0d2a34461bfb9b2ea992433ed063665697226", ",169,208,210,", true, 2, 1, "154ec10e75f45946a17e393eff8c145ba20413a1f0067a5b98ddeaeec54f6a1f", "" },
    { "8250CB98-4AD503F4FFFFDBFE-5C21BE9B15F47834-446AB03A", "eb640e4d714dfb106eb86db25af0d2a34461bfb9b2ea992433ed063665697226", ",226,238,", true, 2, 1, "1a4c374ac91ed0d6f84d4cf41bf0951ccfbf3c09f5c5e5c2765d504e24f7cd5e", "" },
    { "8250CB98-4AD503F4FFFFDBFE-5C21BE9B15F47834-89401D34", "01bf55537bc61f0cb22d008bdbbc567468e3851e93370197103d9694e7a0b848", ",169,208,210,", true, 2, 0, "154ec10e75f45946a17e393eff8c145ba20413a1f0067a5b98ddeaeec54f6a1f", "" },
    { "8250CB98-4AD503F4FFFFDBFE-5C21BE9B15F47834-89401D34", "01bf55537bc61f0cb22d008bdbbc567468e3851e93370197103d9694e7a0b848", ",226,238,", true, 2, 0, "1a4c374ac91ed0d6f84d4cf41bf0951ccfbf3c09f5c5e5c2765d504e24f7cd5e", "" },
    { "8250CB98-4AD503F4FFFFDBFE-5C21BE9B15F47834-89401D34", "301a9680ca30eb146aed0183fb07bb5e9cad7f93ee0c5b58718992bc9f4d71b6", ",169,208,210,", true, 2, 0, "154ec10e75f45946a17e393eff8c145ba20413a1f0067a5b98ddeaeec54f6a1f", "" },
    { "8250CB98-4AD503F4FFFFDBFE-5C21BE9B15F47834-89401D34", "301a9680ca30eb146aed0183fb07bb5e9cad7f93ee0c5b58718992bc9f4d71b6", ",226,238,", true, 2, 0, "1a4c374ac91ed0d6f84d4cf41bf0951ccfbf3c09f5c5e5c2765d504e24f7cd5e", "" },
    { "8250CB98-4E236D01C99BE6EE-D1D753CBF272C36D", "29b4e03802fa643c321dc0f16b41972dfda454e7461d6c8c36e8c74ca661e745", ",169,208,210,226,238,", true, 2, 1, "e38fe695d654429c876a93760dc0ecf11c3eaefa4c5a8a65e0b527e0f2a60bea", "" },
    { "8250CB98-5AF4C0870A9FA442-98DB1F3A779A8EB2", "d2a319fac8db9f8ec4077852d50fb480f84283b91d01b22f2f524fea200c4cd5", ",169,208,210,226,238,", true, 2, 1, "21b7674e090b82fdc8ac81942b900e6a9354fd64ae1829a30c5a397d0bef8626", "" },
    { "8250CB98-68F5B5F29EC06AED-832DEA08487412B7", "8f738786ca06346993a64b093eba5f6f7b06a74555a21fad72fa993582892252", ",169,208,210,226,238,", true, 2, 1, "be9e677fd39529115fb12cff47534378eb4af0f177534a92fdac759177ce946e", "" },
    { "8250CB98-6B20B3B0BD515350-676432BDE9C49AA5", "c0bf1196da99ac2def79dfe5bdbbd4a03d5b3943d8b2afa4e3b67fb6388387c3", ",169,208,210,226,238,", true, 2, 1, "1b2301a7eb91e906be4faea666dccacf28e3a319098fb0b0896a4184ea83743e", "" },
    { "8250CB98-6C0EF7B23AA82F3B-60D068C6BC344BEA", "8a1de6f383494a7dd9cd3615e2bbaa0566fa0e0b05fe34502d33c87e57e14f8d", ",169,208,210,226,238,", true, 2, 1, "68ebe82cc35880dd8dd3f47395703f337f7554bb782fa17ff3ba46210d81b872", "" },
    { "8250CB98-75EABF3467B9930B-078EFF45A531A625-6F5FCECC", "316b7a1450af99084642637d536635ba83f80743e124b0d91211f51e048837e3", ",169,208,210,", false, 0, -1, "", "" },
    { "8250CB98-75EABF3467B9930B-078EFF45A531A625-6F5FCECC", "316b7a1450af99084642637d536635ba83f80743e124b0d91211f51e048837e3", ",226,238,", true, 2, 1, "767261f556bb1024e7b8c37cba03984d8fadf30153a9eb2779fb2fde56efc806", "" },
    { "8250CB98-7808944337B1C1C3-85222C88DE4F1473", "791443259dfa2eed9cf08a2b8161cc3ee4725a6387ca231416a64c26ccd6fcf7", ",169,208,210,226,238,", true, 2, 1, "916929137f10fafb0688366bf88fc376e50ddffd6a745869e4fc2a066de5c3ad", "" },
    { "8250CB98-7808944337B1C1C3-85222C88DE4F1473", "96faed1c16e0021a4f1676c21221a9504d7921f86c02af665383d6bd514c877d", ",169,208,210,226,238,", true, 2, 1, "916929137f10fafb0688366bf88fc376e50ddffd6a745869e4fc2a066de5c3ad", "" },
    { "8250CB98-7808944337B1C1C3-85222C88DE4F1473", "d4e4ad638ded126438bb38943a90133cc2348ab14216eb7495fdf1c6519f28da", ",169,208,210,226,238,", true, 2, 1, "916929137f10fafb0688366bf88fc376e50ddffd6a745869e4fc2a066de5c3ad", "" },
    { "8250CB98-7808944337B1C1C3-85222C88DE4F1473", "f3720bfba8f5bd3496965f28c21f51dcc6da04bd75ae775ddbf621e1762734a7", ",169,208,210,226,238,", true, 2, 1, "916929137f10fafb0688366bf88fc376e50ddffd6a745869e4fc2a066de5c3ad", "" },
    { "8250CB98-7808944337B1C1C3-85222C88DE4F1473-52BEBD65", "b8e0f10e9e44c44e5ca72a48e5b293f1c68897772e4db10e194558143fed0da1", ",169,208,210,226,238,", true, 2, 1, "916929137f10fafb0688366bf88fc376e50ddffd6a745869e4fc2a066de5c3ad", "" },
    { "8250CB98-7808944337B1C1C3-85222C88DE4F1473-52BEBD65", "e8b8931aeb89b35b91d4a077eec622cf4865eeaaa08915d56dcb4a7797d3e64c", ",169,208,210,226,238,", true, 2, 1, "916929137f10fafb0688366bf88fc376e50ddffd6a745869e4fc2a066de5c3ad", "" },
    { "8250CB98-7C60508805C12747-3D56B688867843B4", "9b7c9bdf54a276f90cdc0def8bff97d3e54bb237556ff35868be40d9c50c1668", ",169,208,210,226,238,", true, 2, 1, "8a3542b6a5e61f56b651e4e4d1951f8253f61d0c80a503b2d839271ce165e01b", "" },
    { "8250CB98-831E45F15F42D9C6-91B8F571DDAAFCE9", "06b098e14a3a1ce77bf6accc04f2d374179b1c7a0293ddbb01e39eb491bc7d0f", ",169,208,210,226,238,", true, 2, 1, "637f5e638d858caeded10d019ce6da726926eafa74ecc1f8169d170af27cbde0", "" },
    { "8250CB98-8E994116CF8B8B0E-71578AF8C05B3842", "e8c37a06d60fa4ead9c7ca0ea605860d176c3956ec438bcbe59a08a9e6cfd130", ",169,208,210,226,238,", true, 2, 1, "e1cdc489d120457b661d58494b83da6d409abb70cd8a9ad12b50cbc30ae7a1b3", "" },
    { "8250CB98-917B18A3BEDED08E-5DC69D36E1C59F95", "a478615e8b4ba53d8c7d1f478139f386bcbf88f07ed691ebbc957ed9b9e8c5cf", ",169,208,210,226,238,", true, 2, 1, "db2f66da2d2049cca9c45d04c75baa3e8e01eec9e90eaee2b97055387b840e88", "" },
    { "8250CB98-966E17C22B7FBEF5-078EFF45A531A625", "dd70d27cf081b5e77d0176c0680eb327b85cc1a143aa7d997f5615f9e19af5c2", ",169,208,210,", false, 0, -1, "", "" },
    { "8250CB98-966E17C22B7FBEF5-078EFF45A531A625", "dd70d27cf081b5e77d0176c0680eb327b85cc1a143aa7d997f5615f9e19af5c2", ",226,238,", true, 2, 1, "fc603225f2c08ddb9cfbb35cae20d84e4b83df8bebde245a1baf77fa28268d48", "" },
    { "8250CB98-966E17C22B7FBEF5-078EFF45A531A625-DC6335DB", "98e0bede55601ce484a922800bedcfe3e2dd274c6b8b5626d80c5f7a2853f4d2", ",169,208,210,", false, 0, -1, "", "" },
    { "8250CB98-966E17C22B7FBEF5-078EFF45A531A625-DC6335DB", "98e0bede55601ce484a922800bedcfe3e2dd274c6b8b5626d80c5f7a2853f4d2", ",226,238,", true, 2, 1, "fc603225f2c08ddb9cfbb35cae20d84e4b83df8bebde245a1baf77fa28268d48", "" },
    { "8250CB98-A1283AB245B36814-AFC4EF61FC7A0AB7", "2f158eef30177bff6ef5e1dd55ddede26f42f589a8ae4f47604cc1401955a95f", ",169,208,210,226,238,", true, 2, 1, "53f73839ce24dadf3b504c6d09638a5c2a41ed8ccf396a920790f9773e26dd26", "" },
    { "8250CB98-A6A87EB96AA36F7E-36BBAA8F247C6953", "4456f1c0d250c55e4a7102b3705971741b06e2e8e94185435750be11c0ed6df7", ",169,208,210,226,238,", true, 2, 1, "b4432324b98ad5516b1c020d911cfce9abe7596c1cb47232f96742af4c26ddc4", "" },
    { "8250CB98-B58B01926365ADEC-2E46F81AC55F31D8", "4dbadd3f2fe6f09348fd16232af1cb462f53747a1bd86c807909b7107feb2907", ",169,208,210,226,238,", true, 2, 1, "25e4c940cb7016fce120ce597dd28eb38e01d3b96d7b86a7dcc545ee9820b748", "" },
    { "8250CB98-C1CAAFCB5EB76397-24F4430C7DDCA67F", "09a987579e05fd536634fa969227c0f1d08939b383f27691ad04a13b7c20609e", ",169,208,210,226,238,", true, 2, 1, "d7ebf607bb1268a661e104c3e858bfd25ec2bfe6cb003ffded65fb480cd7cd61", "" },
    { "8250CB98-C1CAAFCB5EB76397-24F4430C7DDCA67F", "8397c047a8a2838b0223e7eb1f9652b5f4d51d8904b769e42c7380108c28c442", ",169,208,210,226,238,", true, 2, 1, "d7ebf607bb1268a661e104c3e858bfd25ec2bfe6cb003ffded65fb480cd7cd61", "" },
    { "8250CB98-C1CAAFCB5EB76397-24F4430C7DDCA67F", "d1ffc3de7156b1ade0ade47301f3c21f1bc7a004a8214ac20c8ffcaa167b8261", ",169,208,210,226,238,", true, 2, 1, "d7ebf607bb1268a661e104c3e858bfd25ec2bfe6cb003ffded65fb480cd7cd61", "" },
    { "8250CB98-C1CAAFCB5EB76397-24F4430C7DDCA67F", "f5881a60c256014264d9a9fa4d7614140deca3260824d2d218b630784c56cc07", ",169,208,210,226,238,", true, 2, 1, "d7ebf607bb1268a661e104c3e858bfd25ec2bfe6cb003ffded65fb480cd7cd61", "" },
    { "8250CB98-C1CAAFCB5EB76397-24F4430C7DDCA67F-8B88B852", "7504f5d8d063575409e12c51374558911e56d70f032acbd2002cff61e6430dc0", ",169,208,210,226,238,", true, 2, 1, "d7ebf607bb1268a661e104c3e858bfd25ec2bfe6cb003ffded65fb480cd7cd61", "" },
    { "8250CB98-C1CAAFCB5EB76397-24F4430C7DDCA67F-8B88B852", "e8d3afb70eff9af4f29517eebe33f9f3d3d40e9c5c58675285b24f90cf05e81d", ",169,208,210,226,238,", true, 2, 1, "d7ebf607bb1268a661e104c3e858bfd25ec2bfe6cb003ffded65fb480cd7cd61", "" },
    { "8250CB98-C6844A73CDDCB8DD-7D9E6F6217D90847", "37f9155a587736df66d3063fee6c910b0d03363aafb668c6d8e4b9f8192b1fad", ",169,208,210,226,238,", true, 2, 1, "ef8756f38c74cce1e82f1c8a292f397577a139462c48ec12ef685b9d33d50af8", "" },
    { "8250CB98-D6CA48CAC4816528-EFF153CF2D0CFDCB", "e06dc39d252968b84a0cc77d0940a8175b0fa8d0988728e4593da69f528584c7", ",169,208,210,226,238,", true, 2, 1, "34fe4641f19cc1cead0563c1fc467a0326bbcef45260ac6af0eb7b64f408f701", "" },
    { "8250CB98-DD2E5D260F8514B2-14B8C2B288D59748", "424d20121bc53f24be29013a45da5a53b9b22b23bf6d301c34355734210c947f", ",169,208,210,226,238,", true, 2, 1, "e32105ea57f81f3ab6c06e606b32d5f38cb088270da94d9681bfcc130efac9e8", "" },
    { "8250CB98-DD2E5D260F8514B2-14B8C2B288D59748", "fe68be0c5d71f86ca5c596f74b6536a7d91c8ea5a8798740889981bdd37c948a", ",169,208,210,226,238,", true, 2, 1, "e32105ea57f81f3ab6c06e606b32d5f38cb088270da94d9681bfcc130efac9e8", "" },
    { "8250CB98-DD2E5D260F8514B2-14B8C2B288D59748-3BE4D7D2", "781ae789c371b7e666ef1b810c4715c55ecb032d979609b1616f661127957097", ",169,208,210,226,238,", true, 2, 0, "e32105ea57f81f3ab6c06e606b32d5f38cb088270da94d9681bfcc130efac9e8", "" },
    { "8250CB98-DD2E5D260F8514B2-14B8C2B288D59748-3BE4D7D2", "ed1e610b3dac4da9df40c72d2ee79a9799fe9c8045cf403f5e2153629a6ec32d", ",169,208,210,226,238,", true, 2, 0, "e32105ea57f81f3ab6c06e606b32d5f38cb088270da94d9681bfcc130efac9e8", "" },
    { "8250CB98-DD2E5D260F8514B2-14B8C2B288D59748-FEFD96E3", "1f78bc49beeeb58eb252a7ed393cb3e492896c5de0f9ff58db99ccecdaf7e5fd", ",169,208,210,226,238,", true, 2, 1, "e32105ea57f81f3ab6c06e606b32d5f38cb088270da94d9681bfcc130efac9e8", "" },
    { "8250CB98-DD2E5D260F8514B2-14B8C2B288D59748-FEFD96E3", "88ced30bf046e5443da794a05e5aa57dfcfda1180768757cccf65426e433113c", ",169,208,210,226,238,", true, 2, 1, "e32105ea57f81f3ab6c06e606b32d5f38cb088270da94d9681bfcc130efac9e8", "" },
    { "8250CB98-E6F72861DE5584D7-3D35355D4F6569B4", "d2b3f940de4136511a69786afb7afa18c6a3661052fb42d3e29fc90232bf916d", ",169,208,210,226,238,", true, 2, 1, "7877233ed0cae9267c02d539a72662f22a0c1f7f1c41420dbb65f2c86e934243", "" },
    { "8250CB98-EE31B94AA1FB79CA-3BDFFDF26D62EAE1", "a2e0fecc463dd27b917bbc5605f60537d50d6e5965437444d48c634e82f69377", ",169,208,210,226,238,", true, 2, 1, "2f7664a82c62441347dcc98005e59fbd6dea29adb065510489169ebde9331e00", "" },
    { "8250CB98-FA0D23C4A6188031-DF58AEFA1B5A089A", "c76835d4436e5c238683abbcb4c957f210d8e526e98391ac9f17847dbdb29e82", ",169,208,210,226,238,", true, 2, 1, "272b07db803f8abc17c8cd196ee9325b56457680ec0a536e5faed5a37c57f2f3", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3", "210c15773a6edf9b398684b008c438042c020b8cc73289e7058cbbc2ccb5bec1", ",169,208,", true, 2, 1, "f7920ef416246010151f53e52ee68ea2112ff84ea8aadecdbacda49d30714ea3", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3", "210c15773a6edf9b398684b008c438042c020b8cc73289e7058cbbc2ccb5bec1", ",226,", true, 3, 1, "27854c2032bcde953853e81e62e1ea0025e5355fe9966a020189339556237708", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3", "210c15773a6edf9b398684b008c438042c020b8cc73289e7058cbbc2ccb5bec1", ",210,", true, 3, 1, "8367f67ce3ada7698f9eab7961603b2d10f96ee11c6aceffa91b1c89c5616232", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3", "210c15773a6edf9b398684b008c438042c020b8cc73289e7058cbbc2ccb5bec1", ",238,", true, 6, 1, "b66d882291fdf9db265c68df24df06491269a5adf954ffc60960fcf59103d215", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3-92886B59", "8a9ab628402d436ea61f86083beb3e91ca64efac74f05c73a4ed9ca9de581641", ",169,208,", true, 2, 0, "f7920ef416246010151f53e52ee68ea2112ff84ea8aadecdbacda49d30714ea3", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3-92886B59", "8a9ab628402d436ea61f86083beb3e91ca64efac74f05c73a4ed9ca9de581641", ",226,", true, 3, 0, "27854c2032bcde953853e81e62e1ea0025e5355fe9966a020189339556237708", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3-92886B59", "8a9ab628402d436ea61f86083beb3e91ca64efac74f05c73a4ed9ca9de581641", ",210,", true, 3, 0, "8367f67ce3ada7698f9eab7961603b2d10f96ee11c6aceffa91b1c89c5616232", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3-92886B59", "8a9ab628402d436ea61f86083beb3e91ca64efac74f05c73a4ed9ca9de581641", ",238,", true, 6, 0, "b66d882291fdf9db265c68df24df06491269a5adf954ffc60960fcf59103d215", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3-E036AF89", "419956bf12c43903d008f566af7581c6a6f064be3d55d805e1b9bdc6e40c5f00", ",169,208,", true, 2, 1, "f7920ef416246010151f53e52ee68ea2112ff84ea8aadecdbacda49d30714ea3", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3-E036AF89", "419956bf12c43903d008f566af7581c6a6f064be3d55d805e1b9bdc6e40c5f00", ",226,", true, 3, 1, "27854c2032bcde953853e81e62e1ea0025e5355fe9966a020189339556237708", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3-E036AF89", "419956bf12c43903d008f566af7581c6a6f064be3d55d805e1b9bdc6e40c5f00", ",210,", true, 3, 1, "8367f67ce3ada7698f9eab7961603b2d10f96ee11c6aceffa91b1c89c5616232", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3-E036AF89", "419956bf12c43903d008f566af7581c6a6f064be3d55d805e1b9bdc6e40c5f00", ",238,", true, 6, 1, "b66d882291fdf9db265c68df24df06491269a5adf954ffc60960fcf59103d215", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3-E036AF89", "e3d459c346be94c7c3a357c5578dc1c8acd79a8d9d7fc6b3026d0f9dacf970f3", ",169,208,", true, 2, 1, "f7920ef416246010151f53e52ee68ea2112ff84ea8aadecdbacda49d30714ea3", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3-E036AF89", "e3d459c346be94c7c3a357c5578dc1c8acd79a8d9d7fc6b3026d0f9dacf970f3", ",226,", true, 3, 1, "27854c2032bcde953853e81e62e1ea0025e5355fe9966a020189339556237708", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3-E036AF89", "e3d459c346be94c7c3a357c5578dc1c8acd79a8d9d7fc6b3026d0f9dacf970f3", ",210,", true, 3, 1, "8367f67ce3ada7698f9eab7961603b2d10f96ee11c6aceffa91b1c89c5616232", "" },
    { "825118D8-E5D23DF7CBC77548-5A8502A3C960E9F3-E036AF89", "e3d459c346be94c7c3a357c5578dc1c8acd79a8d9d7fc6b3026d0f9dacf970f3", ",238,", true, 6, 1, "b66d882291fdf9db265c68df24df06491269a5adf954ffc60960fcf59103d215", "" },
};

// Pin the ordered pairs by guest words and translated shader identities.
std::string captureRoutingDigest(const SceneDrawRecipe& recipe)
{
    std::string routing;
    for (const auto* candidate : recipe.pair)
    {
        const auto& shader = candidate->shader;
        routing += shader.vertexGuestWordDigest + "/" + shader.pixelGuestWordDigest + "/" + shader.vertexDxilDigest + "/" + shader.pixelDxilDigest + ";";
    }
    return routing.empty() ? std::string() : rex::crypto::sha256(routing);
}

void requireCaptureBaseline(const IndexedBufferDrawInputs& inputs, const SceneDrawCatalog& catalog, const std::string& name, bool built, const SceneDrawRecipe& recipe, const std::string& refusal)
{
    const auto stateDigest = IndexedBufferMicrocodeWordDigest(inputs.state);
    const auto catalogSize = std::format(",{},", catalog.entries.size());
    const auto expected    = std::ranges::find_if(kCaptureBaselines, [&](const CaptureBaseline& baseline)
                                                  {
                                                   return baseline.name == name && baseline.stateDigest == stateDigest && baseline.catalogs.find(catalogSize) != std::string_view::npos;
                                                  });
    UNSCOPED_INFO(name + " has a 3129fb65 routing/refusal baseline for this state and catalog");
    REQUIRE(expected != std::end(kCaptureBaselines));
    const auto entry   = recipe.entry ? std::ranges::find(recipe.pair, recipe.entry) - recipe.pair.begin() : -1;
    const auto routing = captureRoutingDigest(recipe);
    UNSCOPED_INFO(std::format("{} routing/refusal differs from 3129fb65: expected built={} pair={} entry={} routing={} refusal=[{}], got built={} pair={} entry={} routing={} refusal=[{}]",
                              name,
                              expected->built,
                              expected->pairSize,
                              expected->entry,
                              expected->routingDigest,
                              expected->refusal,
                              built,
                              recipe.pair.size(),
                              entry,
                              routing,
                              refusal));
    REQUIRE((built == expected->built && refusal == expected->refusal && recipe.pair.size() == expected->pairSize && entry == expected->entry && routing == expected->routingDigest));
}

// Watch-off builds match full baseline refusals even through the shared memo.
void defaultOffCaptureRouting(const IndexedBufferDrawInputs& inputs, const SceneDrawCatalog& catalog, const std::string& name)
{
#if REREVVED_DEVELOPER
    const bool saved = REXCVAR_GET(native_vs_code_watch);
    REXCVAR_SET(native_vs_code_watch, false);
#endif
    SceneDrawRecipe first, repeated;
    std::string     firstError, repeatedError;
    const bool      firstOk    = BuildSceneDrawRecipe(inputs, catalog, first, firstError);
    const bool      repeatedOk = BuildSceneDrawRecipe(inputs, catalog, repeated, repeatedError);
#if REREVVED_DEVELOPER
    REXCVAR_SET(native_vs_code_watch, saved);
#endif
    requireCaptureBaseline(inputs, catalog, name, firstOk, first, firstError);
    requireCaptureBaseline(inputs, catalog, name, repeatedOk, repeated, repeatedError);
    UNSCOPED_INFO(std::string(name + " default-off repeated routing, refusal bytes and recipe are unchanged: ") + (repeatedError));
    REQUIRE((firstOk == repeatedOk && firstError == repeatedError && first.pair == repeated.pair && first.entry == repeated.entry &&
             first.patchedVertexCode == repeated.patchedVertexCode && first.effectiveState == repeated.effectiveState &&
             (!firstOk || recipeBytes(first.native) == recipeBytes(repeated.native))));

    std::vector<const SceneDrawShader*> expected;
    std::vector<std::uint8_t>           selectedCode;
    std::string                         error;
    if (!inputs.pixelObject)
    {
        std::vector<std::uint32_t> offsets;
        for (std::size_t at = 0; at + 12 <= inputs.declaration.size(); at += 12)
            offsets.push_back(word(inputs.declaration, at) & 0xFFFF);
        if (PatchVertexFetch(inputs, offsets, selectedCode, error))
            for (const auto& entry : catalog.depthOnly)
                if (entry.shader.vertexGuestWordDigest == IndexedBufferMicrocodeWordDigest(selectedCode))
                    expected.push_back(&entry);
        UNSCOPED_INFO(name + " depth-only routing equals the direct patch selection");
        REQUIRE(first.pair == expected);
    }
    else if (!first.effectiveState.empty())
    {
        auto                      folded = inputs.vertexCode;
        std::vector<std::uint8_t> effective;
        UNSCOPED_INFO(std::string(name + " reference fold: ") + (error));
        REQUIRE(FoldSolidShaderMetadata(inputs, folded, effective, error));
        for (const auto* candidate : catalog.WithPixel(IndexedBufferMicrocodeWordDigest(inputs.pixelCode)))
        {
            if (!expected.empty())
            {
                if (candidate->shader.vertexGuestWordDigest == expected.front()->shader.vertexGuestWordDigest)
                    expected.push_back(candidate);
                continue;
            }
            if (word(inputs.vertexMetadata, 28) != candidate->attributeOffsets.size() || inputs.arguments.stride != candidate->stride)
                continue;
            std::vector<std::uint8_t> patched;
            if (PatchVertexFetch(inputs, folded, candidate->attributeOffsets, patched, error) &&
                IndexedBufferMicrocodeWordDigest(patched) == candidate->shader.vertexGuestWordDigest)
            {
                expected.push_back(candidate);
                selectedCode = std::move(patched);
            }
        }
        UNSCOPED_INFO(name + " routing equals the direct fold and candidate-restricted patch selection");
        REQUIRE(first.pair == expected);
        if (expected.empty())
        {
            UNSCOPED_INFO(name + " the default-off catalog refusal is byte-identical to the direct selection");
            REQUIRE((!firstOk && firstError == "scene_catalog_vertex: " + IndexedBufferMicrocodeWordDigest(inputs.vertexCode).substr(0, 16)));
        }
    }
    if (!expected.empty())
    {
        UNSCOPED_INFO(name + " routes the direct patch's execution words");
        REQUIRE(first.patchedVertexCode == selectedCode);
    }
}

int run(const std::string& roots, const std::filesystem::path& catalogDirectory, const std::filesystem::path& capture)
{
    SceneDrawCatalog catalog;
    std::string      error;
    UNSCOPED_INFO(std::string("the scene catalog loads: ") + (error));
    REQUIRE(LoadSceneDrawCatalog(catalogDirectory, catalog, error));
    const auto tables = fetchTables(capture);

    // Per entry, the refusals of draws that reached its pair in a table form
    // and outside one.
    std::set<const SceneDrawShader*>                        built;
    std::map<const SceneDrawShader*, std::set<std::string>> inTable, outside;
    std::map<std::string, unsigned>                         outcomes;
    unsigned                                                records = 0, foreign = 0, matched = 0, greaterEqual = 0;
    // Per loop entry, the live i16..i18 words its built draws bind.
    std::map<const SceneDrawShader*, std::set<std::array<std::uint32_t, 3>>> loopWords;
    for (std::size_t begin = 0; begin < roots.size();)
    {
        const auto end  = std::min(roots.find(';', begin), roots.size());
        const auto root = std::filesystem::path(roots.substr(begin, end - begin));
        begin           = end + 1;
        UNSCOPED_INFO("a harvest root: " + root.string());
        REQUIRE(std::filesystem::is_directory(root));
        for (const auto& directory : std::filesystem::directory_iterator(root))
        {
            const auto name = directory.path().filename().string();
            if (!directory.is_directory() || !std::ranges::any_of(kSceneDrawCallers, [&](std::uint32_t caller)
                                                                  {
                                                                      return name.starts_with(std::format("{:08X}-", caller));
                                                                  }))
                continue;
            const auto table  = toml::parse_file((directory.path() / "record.toml").string());
            auto       inputs = inputsOf(directory.path(), table, tables);
            defaultOffCaptureRouting(inputs, catalog, name);
            ++records;
            if (!inputs.pixelObject)
            {
                // A depth-only draw whose VS has a depth-only entry reaches
                // it; alpha-to-mask is refused by name.
                const auto vertexDigest = IndexedBufferMicrocodeWordDigest(inputs.vertexCode);
                if (std::ranges::none_of(catalog.depthOnly, [&](const SceneDrawShader& entry)
                                         {
                                             return entry.shader.vertexGuestWordDigest == vertexDigest;
                                         }))
                {
                    ++foreign;
                    continue;
                }
                SceneDrawRecipe recipe;
                const bool      ok      = BuildSceneDrawRecipe(inputs, catalog, recipe, error);
                const auto      control = word(inputs.state, 0x293C);
                UNSCOPED_INFO(std::string(name + " reaches its depth-only entry: ") + (error));
                REQUIRE((ok || (!recipe.pair.empty() && !error.empty())));
                if ((control & ~0xFU) != 0x87000000U)
                {
                    UNSCOPED_INFO(std::string(name + " refuses its alpha-to-mask or offsets: ") + (error));
                    REQUIRE((!ok && error == std::format("scene_color_control: {:#x}", control)));
                }
                ++outcomes[ok ? "depth-only built" : "depth-only " + error.substr(0, error.find(':'))];
                if (ok)
                {
                    // Before the VS's first draw its fetch words are the
                    // shader file's templates, which 0x826ADD88 patches into
                    // the harvested words before the GPU runs them.
                    auto loaded = inputs;
                    for (std::size_t i = 0; i * 4 < inputs.fetchRecords.size(); ++i)
                        for (std::size_t j = 0; j < 3; ++j)
                            store(loaded.vertexCode, (word(inputs.fetchRecords, i * 4) & 0xFFF) * 12 + j * 4, word(inputs.fetchTemplates, i * 12 + j * 4));
                    SceneDrawRecipe fromTemplates;
                    UNSCOPED_INFO(std::string(name + " builds the same draw from its fetch templates: ") + (error));
                    REQUIRE((loaded.vertexCode != inputs.vertexCode && BuildSceneDrawRecipe(loaded, catalog, fromTemplates, error) &&
                             fromTemplates.entry == recipe.entry && fromTemplates.patchedVertexCode == inputs.vertexCode &&
                             fromTemplates.native.vertexData == recipe.native.vertexData && fromTemplates.native.sharedConstants == recipe.native.sharedConstants &&
                             fromTemplates.native.vertexAttributeCount == recipe.native.vertexAttributeCount));
                    ++outcomes["depth-only built from fetch templates"];
                }
                continue;
            }
            if (catalog.WithPixel(IndexedBufferMicrocodeWordDigest(inputs.pixelCode)).empty())
            {
                ++foreign;
                continue;
            }
            // Harvest words are post-patch, so a record whose own pair has an
            // entry must reach that pair unless a rule before the VS match refuses it.
            const auto vertexDigest = IndexedBufferMicrocodeWordDigest(inputs.vertexCode), pixelDigest = IndexedBufferMicrocodeWordDigest(inputs.pixelCode);
            const bool cataloged = std::ranges::any_of(catalog.entries, [&](const SceneDrawShader& entry)
                                                       {
                                                           return entry.shader.vertexGuestWordDigest == vertexDigest && entry.shader.pixelGuestWordDigest == pixelDigest;
                                                       });
            // The recorded RB_COLORCONTROL and the other alpha specialization.
            const auto recorded = word(inputs.state, 0x293C);
            for (const auto color : { recorded, (recorded & 8) ? 0x87000004U : 0x8700000CU })
            {
                store(inputs.state, 0x293C, color);
                SceneDrawRecipe recipe;
                if (BuildSceneDrawRecipe(inputs, catalog, recipe, error))
                {
                    built.insert(recipe.entry);
                    if (recipe.native.sharedConstants.size() == 464)
                    {
                        std::array<std::uint32_t, 3> words{};
                        std::memcpy(words.data(), recipe.native.sharedConstants.data() + 336 + 16 * 4, sizeof(words));
                        loopWords[recipe.entry].insert(words);
                    }
                    matched += cataloged ? 1U : 0U;
                    ++outcomes["built"];
                    greaterEqual += (color & 0xF) == 0xE ? 1U : 0U;
                    continue;
                }
                UNSCOPED_INFO(name + " is refused without a code");
                REQUIRE(!error.empty());
                const auto code = error.starts_with("scene_texture_format") ? error : error.substr(0, error.find(':'));
                ++outcomes[code];
                const bool beforeMatch = code == "scene_inputs_incomplete" || code == "scene_draw_tuple" || code == "scene_conditional_or_recorded_shader" ||
                                         code == "scene_stream_form" || code == "scene_literal_stage" || code.starts_with("adapter_");
                UNSCOPED_INFO(std::string(name + " does not reach its cataloged pair: ") + (error));
                REQUIRE((!cataloged || !recipe.pair.empty() || beforeMatch));
                if (cataloged && !recipe.pair.empty())
                    ++matched;
                const bool form = FindSceneTargetForm(GuestDrawRegisters(inputs.state)) != nullptr;
                UNSCOPED_INFO(std::string(name + " outside the table forms is refused with another code: ") + (error));
                REQUIRE((form || code == "scene_target_form" || recipe.pair.empty()));
                for (const auto* entry : recipe.pair)
                    (form ? inTable : outside)[entry].insert(code);
            }
        }
    }
    for (const auto& [code, count] : outcomes)
        std::cout << std::format("  {}: {}\n", code, count);
    for (const auto& [entry, sets] : loopWords)
    {
        std::string text;
        for (const auto& words : sets)
            text += std::format(" {:#x}/{:#x}/{:#x}", words[0], words[1], words[2]);
        std::cout << std::format("  loop entry {:016X}/{:016X} alpha {} binds i16..i18:{}\n", entry->shader.vertexShaderHash, entry->shader.pixelShaderHash, entry->shader.alphaTestEnabled ? entry->shader.alphaTestCompare : 0, text);
    }

    // Every entry a table-form draw reaches builds, or names a known gap.
    std::map<std::string, unsigned> unbuilt;
    std::vector<std::string>        failures;
    for (const auto& entry : catalog.entries)
    {
        if (built.contains(&entry))
            continue;
        const auto describe = std::format("{:016X}/{:016X} alpha {} samples {}", entry.shader.vertexShaderHash, entry.shader.pixelShaderHash, entry.shader.alphaTestEnabled ? entry.shader.alphaTestCompare : 0, entry.shader.sampleCount);
        if (const auto found = inTable.find(&entry); found != inTable.end())
        {
            if (!std::ranges::all_of(found->second, knownGap))
            {
                std::string codes;
                for (const auto& code : found->second)
                    codes += " " + code;
                failures.push_back(describe + ":" + codes);
            }
            for (const auto& code : found->second)
                ++unbuilt[code];
        }
        else
            ++unbuilt[outside.contains(&entry) ? "pair drawn outside the table forms" : "pair not reached by a harvested draw"];
    }
    for (const auto& [reason, count] : unbuilt)
        std::cout << std::format("  unbuilt entries, {}: {}\n", reason, count);
    for (const auto& failure : failures)
        std::cout << "  table-form entry refused: " << failure << '\n';
    UNSCOPED_INFO("a harvested record reaches its cataloged pair");
    REQUIRE(matched > 0);
    // Census B3: the GreaterEqual entries exist for recorded RB_COLORCONTROL 0x8700000E draws.
    const bool greaterEqualEntries = std::ranges::any_of(catalog.entries, [](const SceneDrawShader& entry)
                                                         {
                                                             return entry.shader.alphaTestEnabled && entry.shader.alphaTestCompare == 6;
                                                         });
    UNSCOPED_INFO("a recorded GreaterEqual alpha test builds through its entry");
    REQUIRE((!greaterEqualEntries || greaterEqual > 0));
    if (greaterEqualEntries)
        std::cout << std::format("Scene harvest: {} recorded GreaterEqual alpha tests build\n", greaterEqual);
    UNSCOPED_INFO(std::format("every table-form entry builds or names a known gap ({} do not)", failures.size()));
    REQUIRE((records > 0 && !built.empty() && failures.empty()));
    std::cout << std::format("Scene harvest: {} records ({} with a pixel shader outside the catalog), {} outcomes past a cataloged pair, {} of {} catalog entries built\n", records, foreign, matched, built.size(), catalog.entries.size());
    return 0;
}

// A world frame draw of `pair` (a key record directory): its own device
// block, arguments, indices and stream 0 over the key record's shaders; its
// texture fetches come from its own block with zero texels.
IndexedBufferDrawInputs frameDrawInputs(const std::filesystem::path& world, toml::node_view<const toml::node> event, const std::string& pair, const std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>& tables, const std::string& name)
{
    std::string error;
    const auto  record = toml::parse_file((world / pair / "record.toml").string());
    auto        inputs = inputsOf(world / pair, record, tables);
    inputs.state       = read(world / event["state"].value_or(std::string()));
    std::array<std::uint32_t, 8> r{};
    for (std::size_t i = 0; i < r.size(); ++i)
        r[i] = address(event["arguments_r3_r10"][i]);
    inputs.primitive   = r[1];
    inputs.baseVertex  = r[2];
    inputs.startIndex  = r[3];
    inputs.indexCount  = r[4];
    const auto* memory = event["memory"].as_array();
    UNSCOPED_INFO(name + " memory");
    REQUIRE(memory != nullptr);
    for (const auto& row : *memory)
    {
        const auto entry = toml::node_view<const toml::node>(row);
        const auto role  = entry["role"].value_or(std::string());
        // A small row keeps its bytes inline; a larger one names a payload.
        const auto bytes = [&]
        {
            const auto file = entry["file"].value_or(std::string());
            return file.empty() ? hexBytes(entry["bytes"].value_or(std::string())) : read(world / file);
        };
        if (role == "indices")
            inputs.indexBytes = bytes();
        else if (role == "stream 0")
            inputs.streamBytes[0] = bytes();
        else if (role == "index_buffer")
        {
            const auto flags   = word(hexBytes(entry["bytes"].value_or(std::string())), 0);
            inputs.indexFormat = flags >> 31;
            inputs.indexEndian = (flags >> 29) & 3;
        }
    }
    inputs.streamFetches[0] = { word(inputs.state, 0x778), word(inputs.state, 0x77C) };
    for (std::size_t slot = 0; slot < inputs.textureFetches.size(); ++slot)
    {
        // A vertex-stage fetch, read last, is guest fetch 16.
        const auto guestSlot = inputs.vertexTextureFetch && slot + 1 == inputs.textureFetches.size() ? 16 : slot;
        auto&      fetch     = inputs.textureFetches[slot];
        for (std::size_t i = 0; i < fetch.size(); ++i)
            fetch[i] = word(inputs.state, 0x480 + guestSlot * 24 + i * 4);
        UNSCOPED_INFO(std::string(name + " texture range: ") + (error));
        REQUIRE(GetNativeTextureMemoryRanges(fetch, inputs.textureRanges[slot], error));
        inputs.textureBaseBytes[slot].assign(inputs.textureRanges[slot].base.size, 0);
        inputs.textureMipBytes[slot].assign(inputs.textureRanges[slot].mips.size, 0);
    }
    return inputs;
}

// Shared texture bytes stop creating/uploading after both contexts warm.
int benchFrame(const std::filesystem::path& world, const std::filesystem::path& catalogDirectory, const std::filesystem::path& capture, const std::string& signature, int iterations, std::size_t segments = 1, bool freshPayloads = false)
{
    SceneDrawCatalog catalog;
    std::string      error;
    UNSCOPED_INFO(std::string("the scene catalog loads: ") + (error));
    REQUIRE(LoadSceneDrawCatalog(catalogDirectory, catalog, error));
    const auto  tables = fetchTables(capture);
    const auto  frame  = toml::parse_file((world / "frames" / signature / "frame.toml").string());
    const auto* events = frame["events"].as_array();
    UNSCOPED_INFO("frame events");
    REQUIRE(events != nullptr);
    std::vector<std::pair<NativeRenderFramePass::Target, std::vector<NativeDrawReplayRecipe>>> byTarget;
    std::size_t                                                                                attempted = 0, built = 0;
    std::map<std::string, NativeDrawReplayTexture>                                             payloads;
    for (const auto& node : *events)
    {
        const auto event = toml::node_view<const toml::node>(node);
        const auto pair  = event["pair"].value_or(std::string());
        if (event["kind"].value_or(std::string()) != "draw" || !pair.starts_with("8250C768-"))
            continue;
        ++attempted;
        const auto name   = std::format("frame draw {}", event["index"].value_or<std::int64_t>(-1));
        auto       inputs = frameDrawInputs(world, event, pair, tables, name);
        for (std::size_t slot = 0; slot < inputs.textureFetches.size(); ++slot)
        {
            const auto seed = static_cast<std::uint8_t>((inputs.textureFetches[slot][1] >> 12) * 0x9E3779B1U >> 24);
            inputs.textureBaseBytes[slot].assign(inputs.textureRanges[slot].base.size, seed);
            inputs.textureMipBytes[slot].assign(inputs.textureRanges[slot].mips.size, static_cast<std::uint8_t>(seed ^ 0x5A));
        }
        SceneDrawRecipe recipe;
        if (!BuildSceneDrawRecipe(inputs, catalog, recipe, error))
            continue;
        ++built;
        // Equal decoded textures share the first one's payload and version.
        for (auto& texture : recipe.native.textures)
            if (!texture.frameTextureId)
            {
                const auto key    = std::format("{}x{}x{} {} {} {}", texture.width, texture.height, texture.mipLevels, int(texture.format), int(texture.dimension), sha256(texture.payloadBytes()));
                auto&      shared = payloads.try_emplace(key, texture).first->second;
                if (shared.sharedBytes)
                    texture.bytes.clear();
                texture.sharedBytes    = shared.sharedBytes;
                texture.textureVersion = shared.textureVersion;
            }
        const auto target = NativeRenderFrameDrawTarget(recipe.native);
        auto       found  = std::ranges::find_if(byTarget, [&](const auto& entry)
                                                 {
                                              return entry.first == target;
                                                 });
        if (found == byTarget.end())
            found = byTarget.insert(byTarget.end(), { target, {} });
        found->second.push_back(std::move(recipe.native));
    }
    UNSCOPED_INFO("a scene draw of the frame builds");
    REQUIRE(!byTarget.empty());
    const auto& [target, draws] = *std::ranges::max_element(byTarget, {}, [](const auto& entry)
                                                            {
                                                                return entry.second.size();
                                                            });
    std::cout << std::format("Draw-cost bench: frame {} builds {} of {} scene draws over {} textures; {} into {}x{}x{}\n", signature, built, attempted, payloads.size(), draws.size(), target.width, target.height, target.sampleCount);

    {
        std::size_t                vertex = 0, index = 0, textures = 0;
        std::array<std::size_t, 3> constants{}, repeated{};
        for (std::size_t drawIndex = 0; drawIndex < draws.size(); ++drawIndex)
        {
            const auto& draw = draws[drawIndex];
            vertex += draw.vertexBytes().size();
            index += draw.indices.size() * 4;
            textures += std::popcount(draw.textureMask);
            const std::array<const std::vector<std::uint8_t>*, 3> stages{ &draw.vertexConstants, &draw.pixelConstants, &draw.sharedConstants };
            for (std::size_t stage = 0; stage < stages.size(); ++stage)
            {
                constants[stage] += stages[stage]->size();
                if (drawIndex)
                {
                    const auto& previous = draws[drawIndex - 1];
                    repeated[stage] += *stages[stage] == (stage == 0 ? previous.vertexConstants : stage == 1 ? previous.pixelConstants
                                                                                                             : previous.sharedConstants);
                }
            }
        }
        const auto n = double(draws.size());
        std::cout << std::format("  per draw: {:.0f} vertex bytes, {:.0f} index bytes, {:.0f}/{:.0f}/{:.0f} vertex/pixel/shared constant bytes "
                                 "({}/{}/{} draws repeat the preceding draw's), {:.2f} textures\n",
                                 vertex / n,
                                 index / n,
                                 constants[0] / n,
                                 constants[1] / n,
                                 constants[2] / n,
                                 repeated[0],
                                 repeated[1],
                                 repeated[2],
                                 textures / n);
    }
    NativeRendererD3D12 renderer;
    UNSCOPED_INFO("native stream: " + error);
    REQUIRE(renderer.StartDrawStream(error));
#if REREVVED_DEVELOPER
    renderer.SetPipelineTiming(true);
#endif
    const auto frameOf = [&]
    {
        NativeRenderFrame render;
        render.textures.push_back({ 1, target.width, target.height, NativeDrawReplayTextureFormat::Rgba8, 1 });
        auto frameDraws = draws;
        if (freshPayloads)
        {
            std::map<const void*, std::shared_ptr<const std::vector<std::uint8_t>>> copies;
            for (auto& draw : frameDraws)
                for (auto& texture : draw.textures)
                    if (texture.sharedBytes)
                    {
                        auto& copy = copies[texture.sharedBytes.get()];
                        if (!copy)
                            copy = std::make_shared<const std::vector<std::uint8_t>>(*texture.sharedBytes);
                        texture.sharedBytes = copy;
                        texture.textureVersion.reset();
                    }
        }
        const auto count = std::max<std::size_t>(1, std::min(segments, frameDraws.size()));
        for (std::size_t index = 0; index < count; ++index)
        {
            NativeRenderFramePass pass;
            pass.target = target;
            if (!index)
            {
                NativeRenderFramePass::Clear clear;
                clear.rectangle = { 0, 0, std::int32_t(target.width), std::int32_t(target.height) };
                clear.color = clear.depth = clear.stencil = true;
                pass.clears.push_back(clear);
            }
            const auto first = frameDraws.size() * index / count, last = frameDraws.size() * (index + 1) / count;
            pass.draws.assign(std::make_move_iterator(frameDraws.begin() + first), std::make_move_iterator(frameDraws.begin() + last));
            if (index + 1 == count)
            {
                NativeRenderFrameResolve resolve;
                resolve.textureId = 1;
                resolve.width     = target.width;
                resolve.height    = target.height;
                pass.resolves.push_back(resolve);
            }
            render.passes.push_back(std::move(pass));
        }
        render.outputTextureId = 1;
        render.retainOnly = render.returnRawOutput = true;
        return render;
    };
    std::vector<std::shared_ptr<NativeRenderFrameTicket>> tickets;
    for (int iteration = 0; iteration < iterations + 2; ++iteration)
    {
#if REREVVED_DEVELOPER
        const auto before = renderer.ReplayTimingStats();
#endif
        const auto                               cacheBefore = renderer.ReplayCacheStats();
        std::shared_ptr<NativeRenderFrameTicket> ticket;
        auto                                     render = frameOf();
        for (std::size_t index = 0; index + 1 < render.passes.size(); ++index)
        {
            NativeRenderFrame segment;
            segment.passes = { render.passes[index] };
            std::shared_future<NativeDrawReplayResult> segmentRecording;
            const bool                                 segmentQueued = renderer.QueueRenderFramePasses(std::move(segment), segmentRecording, error);
            UNSCOPED_INFO("the bench segment queues: " + error);
            REQUIRE(segmentQueued);
            const auto recorded = segmentRecording.get();
            UNSCOPED_INFO("the bench segment records: " + recorded.error);
            REQUIRE(recorded.success);
        }
        // Queued segments leave the last pass as their tail. The message is
        // built after the call, which sets the error.
        const bool queued = (render.passes.size() < 2 || SplitNativeRenderFrameTail(render, render.passes.size() - 1, error)) &&
                            renderer.QueueRenderFrame(std::move(render), ticket, error);
        UNSCOPED_INFO("the bench frame queues: " + error);
        REQUIRE(queued);
        const auto recording = ticket->Recording().get();
        UNSCOPED_INFO("the bench frame records: " + recording.error);
        REQUIRE(recording.success);
        tickets.push_back(std::move(ticket));
        const auto cacheAfter = renderer.ReplayCacheStats();
        const auto created    = cacheAfter.textureCreations - cacheBefore.textureCreations;
        const auto uploaded   = cacheAfter.textureUploadCreations + cacheAfter.textureUploadReuses - cacheBefore.textureUploadCreations - cacheBefore.textureUploadReuses;
#if REREVVED_DEVELOPER
        const auto after       = renderer.ReplayTimingStats();
        const auto descriptors = cacheAfter.descriptorReuses - cacheBefore.descriptorReuses;
        const auto matched     = cacheAfter.textureContentReuses - cacheBefore.textureContentReuses;
        const auto recorded    = after.recordedDraws - before.recordedDraws;
        const auto us          = [&](std::uint64_t later, std::uint64_t earlier)
        {
            return double(later - earlier) / 1e3 / double(recorded ? recorded : 1);
        };
        std::string phases;
        for (std::size_t phase = 0; phase < after.recordingPhaseNanoseconds.size(); ++phase)
            phases += std::format(" {}={:.2f}", NativeDrawReplayTimingStats::kRecordingPhases[phase], us(after.recordingPhaseNanoseconds[phase], before.recordingPhaseNanoseconds[phase]));
        phases += std::format("; texture prepare {:.2f} us/draw; texture slots", us(after.texturePrepareNanoseconds, before.texturePrepareNanoseconds));
        for (std::size_t outcome = 0; outcome < after.textureOutcomes.size(); ++outcome)
            phases += std::format(" {}={}", NativeDrawReplayTimingStats::kTextureOutcomes[outcome], after.textureOutcomes[outcome] - before.textureOutcomes[outcome]);
        std::cout << std::format("  frame {}: {} draws, {} texture creations, {} uploads, {} descriptor reuses, {} content matches; recording {:.2f} us/draw (pipeline creation {:.2f}, texture upload {:.2f}, draw upload {:.2f});{}\n", iteration + 1, recorded, created, uploaded, descriptors, matched, us(after.recordingNanoseconds, before.recordingNanoseconds), us(after.pipelineNanoseconds, before.pipelineNanoseconds), us(after.textureUploadNanoseconds, before.textureUploadNanoseconds), us(after.drawUploadNanoseconds, before.drawUploadNanoseconds), phases);
#endif
        UNSCOPED_INFO(std::format("frame {} records {} texture creations and {} uploads", iteration + 1, created, uploaded));
        REQUIRE((iteration < 2 || (!created && !uploaded)));
    }
    // Reused storage must render every frame as the first, uploaded one.
    std::vector<std::uint8_t> first;
    for (const auto& ticket : tickets)
    {
        const auto completion = ticket->Completion().get();
        UNSCOPED_INFO("the bench frame completes: " + completion.error);
        REQUIRE(completion.success);
        UNSCOPED_INFO("the bench frame returns its raw output");
        REQUIRE(!completion.rawOutput.empty());
        if (first.empty())
            first = completion.rawOutput;
        UNSCOPED_INFO("every bench frame renders the first frame's pixels");
        REQUIRE(completion.rawOutput == first);
    }
    renderer.Shutdown();
    return 0;
}

// Profile models reader copies and times BuildSceneDrawRecipe stages.
int costFrame(const std::filesystem::path& world, const std::filesystem::path& catalogDirectory, const std::filesystem::path& capture, const std::string& signature, int iterations)
{
    using Clock = std::chrono::steady_clock;
    UNSCOPED_INFO("the build-cost profile times at least one pass");
    REQUIRE(iterations > 0);
    const auto  held    = std::make_shared<SceneDrawCatalog>();
    auto&       catalog = *held;
    std::string error;
    UNSCOPED_INFO(std::string("the scene catalog loads: ") + (error));
    REQUIRE(LoadSceneDrawCatalog(catalogDirectory, catalog, error));
    const auto  tables = fetchTables(capture);
    const auto  frame  = toml::parse_file((world / "frames" / signature / "frame.toml").string());
    const auto* events = frame["events"].as_array();
    UNSCOPED_INFO("frame events");
    REQUIRE(events != nullptr);
    std::vector<IndexedBufferDrawInputs>                                                                    draws;
    std::map<std::string, unsigned>                                                                         skipped;
    std::map<std::tuple<std::uint32_t, std::size_t, int>, std::shared_ptr<const std::vector<std::uint8_t>>> buffers;
    for (const auto& node : *events)
    {
        const auto event = toml::node_view<const toml::node>(node);
        const auto pair  = event["pair"].value_or(std::string());
        if (event["kind"].value_or(std::string()) != "draw" || pair.size() < 9)
            continue;
        const auto caller = static_cast<std::uint32_t>(std::stoul(pair.substr(0, 8), nullptr, 16));
        if (std::ranges::find(kSceneDrawCallers, caller) == kSceneDrawCallers.end() || !std::filesystem::exists(world / pair / "record.toml"))
        {
            ++skipped[pair.substr(0, 8)];
            continue;
        }
        auto inputs = frameDrawInputs(world, event, pair, tables, std::format("frame draw {}", event["index"].value_or<std::int64_t>(-1)));
        // Texture and stream reads share one retained buffer per range, as
        // the resource mirrors serve them.
        const auto shared = [&](std::uint32_t address, std::size_t size, std::uint8_t fill, const GuestBytes* bytes)
        {
            auto& buffer = buffers[{ address, size, bytes ? -1 : int(fill) }];
            if (!buffer)
                buffer = std::make_shared<const std::vector<std::uint8_t>>(bytes ? std::vector<std::uint8_t>(*bytes) : std::vector<std::uint8_t>(size, fill));
            return GuestBytes::Shared(buffer);
        };
        for (std::size_t slot = 0; slot < inputs.textureFetches.size(); ++slot)
        {
            const auto  seed              = static_cast<std::uint8_t>((inputs.textureFetches[slot][1] >> 12) * 0x9E3779B1U >> 24);
            const auto& ranges            = inputs.textureRanges[slot];
            inputs.textureBaseBytes[slot] = shared(ranges.base.address, ranges.base.size, seed, nullptr);
            inputs.textureMipBytes[slot]  = shared(ranges.mips.address, ranges.mips.size, static_cast<std::uint8_t>(seed ^ 0x5A), nullptr);
        }
        inputs.streamBytes[0] = shared(inputs.streamFetches[0][0] & ~3U, inputs.streamBytes[0].size(), 0, &inputs.streamBytes[0]);
        draws.push_back(std::move(inputs));
    }
    UNSCOPED_INFO("the frame has scene-family draws");
    REQUIRE(!draws.empty());
    // Shadow equality excludes dirty words, ring, fetch, constants and index.
    std::size_t                           sameState = 0;
    std::array<std::size_t, 0x3500 / 256> changed{};
    for (std::size_t index = 1; index < draws.size(); ++index)
    {
        const auto& a    = draws[index - 1].state;
        const auto& b    = draws[index].state;
        bool        same = true;
        for (std::size_t offset = 0; offset < a.size(); ++offset)
            if (a[offset] != b[offset])
            {
                ++changed[offset / 256];
                offset = (offset / 256 + 1) * 256 - 1;
            }
        for (std::size_t offset = 0x20; offset < a.size() && same; ++offset)
            same = (offset >= 0x480 && offset < 0x2780) || (offset >= 0x3094 && offset < 0x3098) || (offset >= 0x30 && offset < 0x34) || a[offset] == b[offset];
        sameState += same;
    }
    std::string blocks;
    for (std::size_t block = 0; block < changed.size(); ++block)
        if (changed[block] * 10 >= draws.size())
            blocks += std::format(" {:#x}:{}", block * 256, changed[block]);
    // The words outside those ranges that differ most often.
    std::map<std::size_t, std::size_t> words;
    for (std::size_t index = 1; index < draws.size(); ++index)
        for (std::size_t offset = 0x20; offset < 0x3500; offset += 4)
            if (!(offset >= 0x480 && offset < 0x2780) && offset != 0x3094 && offset != 0x30 && !std::equal(draws[index].state.begin() + offset, draws[index].state.begin() + offset + 4, draws[index - 1].state.begin() + offset))
                ++words[offset];
    std::vector<std::pair<std::size_t, std::size_t>> ranked(words.begin(), words.end());
    std::ranges::sort(ranked, std::greater<>(), &std::pair<std::size_t, std::size_t>::second);
    blocks += "; words:";
    for (std::size_t i = 0; i < std::min<std::size_t>(ranked.size(), 12); ++i)
        blocks += std::format(" {:#x}:{}", ranked[i].first, ranked[i].second);
    std::cout << std::format("  {} of {} draws repeat the previous draw's shadow outside dirty words, ring cursor, fetch and float constants and indices; blocks changed in 10%+ of neighbours:{}\n", sameState, draws.size() - 1, blocks);
    const auto ns = [](Clock::time_point a, Clock::time_point b)
    {
        return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
    };

    // Totals: the modeled read and the family's build, after one warm pass;
    // then the same passes through a build cache.
    LeaderTextureCache              cache;
    std::vector<bool>               built(draws.size());
    std::vector<std::string>        details(draws.size()), digests(draws.size());
    std::map<std::string, unsigned> refusals;
    std::uint64_t                   readNs = 0, buildNs = 0, colorNs = 0, depthNs = 0, copyNs = 0;
    std::size_t                     colorDraws = 0, depthDraws = 0, builtDraws = 0, payloadBytes = 0;
    std::string                     frameDigests;
    for (int iteration = 0; iteration < iterations + 1; ++iteration)
        for (std::size_t index = 0; index < draws.size(); ++index)
        {
            const auto& source = draws[index];
            const auto  t0     = Clock::now();
            auto        inputs = source;
            if (!inputs.pixelCode.empty())
                (void)IndexedBufferMicrocodeWordDigest(inputs.pixelCode);
            for (const auto& fetch : inputs.textureFetches)
            {
                NativeTextureMemoryRanges ranges;
                (void)GetNativeTextureMemoryRanges(fetch, ranges, error);
            }
            const auto             t1 = Clock::now();
            NativeDrawReplayRecipe draw;
            std::string            detail;
            const bool             ok = BuildSceneDraw(inputs, catalog, draw, detail, &cache);
            const auto             t2 = Clock::now();
            // One downstream copy of the recipe, as the assembler keeps it.
            NativeDrawReplayRecipe kept = draw;
            const auto             t3   = Clock::now();
            if (!iteration)
            {
                built[index]   = ok;
                details[index] = detail;
                if (!ok)
                    ++refusals[detail.substr(0, detail.find(':'))];
                else
                {
                    // Each recipe's digest; the frame's digest is over them in order.
                    digests[index] = sha256(recipeBytes(draw));
                    frameDigests += digests[index];
                }
                continue;
            }
            readNs += ns(t0, t1);
            buildNs += ns(t1, t2);
            copyNs += ns(t2, t3);
            if (ok)
            {
                ++builtDraws;
                payloadBytes += kept.vertexBytes().size() + kept.indices.size() * 4 + kept.vertexConstants.size() + kept.pixelConstants.size() + kept.sharedConstants.size() +
                                kept.vertexShaderBytes().size() + kept.pixelShaderBytes().size();
                (source.pixelObject ? colorNs : depthNs) += ns(t1, t2);
                ++(source.pixelObject ? colorDraws : depthDraws);
            }
        }
    const double n = double(draws.size()) * iterations;
    std::cout << std::format("Draw build cost: frame {} {} scene-family draws ({} color, {} depth-only built per pass), {} iterations\n", signature, draws.size(), colorDraws / iterations, depthDraws / iterations, iterations);
    std::cout << std::format("  recipes of the first pass: sha256 {}\n", sha256({ reinterpret_cast<const std::uint8_t*>(frameDigests.data()), frameDigests.size() }));
    for (const auto& [caller, count] : skipped)
        std::cout << std::format("  skipped caller {}: {}\n", caller, count);
    for (const auto& [code, count] : refusals)
        std::cout << std::format("  refused {}: {}\n", code, count);
    std::cout << std::format("  per draw: read model {:.2f} us, family build {:.2f} us (color {:.2f}, depth-only {:.2f}), recipe copy {:.2f} us, {:.0f} recipe payload bytes\n",
                             readNs / 1e3 / n,
                             buildNs / 1e3 / n,
                             colorDraws ? colorNs / 1e3 / colorDraws : 0.0,
                             depthDraws ? depthNs / 1e3 / depthDraws : 0.0,
                             copyNs / 1e3 / n,
                             builtDraws ? double(payloadBytes) / builtDraws : 0.0);

    // Cached and uncached builds must match; budget 0 keeps no geometry,
    // fastest uses the warm pass.
    const auto cachedPasses = [&](SceneBuildCache& sceneCache, int passes, bool timed, std::uint64_t& colorCached, std::uint64_t& depthCached, std::uint64_t& copyCached, std::pair<std::uint64_t, std::uint64_t>* fastest = nullptr)
    {
        LeaderTextureCache textures;
        for (int iteration = 0; iteration < passes + 2; ++iteration)
        {
            const bool checked     = !iteration || iteration == passes + 1;
            const auto colorBefore = colorCached, depthBefore = depthCached;
            for (std::size_t index = 0; index < draws.size(); ++index)
            {
                // The read model copies the inputs, as the uncached passes do.
                const auto             inputs = draws[index];
                NativeDrawReplayRecipe draw;
                std::string            detail;
                const auto             t0   = Clock::now();
                const bool             ok   = BuildSceneDraw(inputs, catalog, draw, detail, &textures, &sceneCache);
                const auto             t1   = Clock::now();
                NativeDrawReplayRecipe kept = draw;
                const auto             t2   = Clock::now();
                if (checked)
                {
                    UNSCOPED_INFO(std::string(std::format("draw {} builds alike through the build cache: ", index)) + (detail));
                    REQUIRE((ok == built[index] && detail == details[index]));
                    UNSCOPED_INFO(std::format("draw {} has the uncached recipe's bytes through the build cache", index));
                    REQUIRE((!ok || sha256(recipeBytes(draw)) == digests[index]));
                }
                if (checked || !timed || !ok)
                    continue;
                (inputs.pixelObject ? colorCached : depthCached) += ns(t0, t1);
                copyCached += ns(t1, t2);
            }
            if (fastest && !checked && (iteration == 1 || colorCached - colorBefore < fastest->first))
                *fastest = { colorCached - colorBefore, depthCached - depthBefore };
        }
    };
    {
        SceneBuildCache                         sceneCache(held);
        std::uint64_t                           colorCached = 0, depthCached = 0, copyCached = 0;
        std::pair<std::uint64_t, std::uint64_t> fastest;
        cachedPasses(sceneCache, iterations, true, colorCached, depthCached, copyCached, &fastest);
        std::cout << std::format("  cached per draw: family build {:.2f} us (color {:.2f}, depth-only {:.2f}), recipe copy {:.2f} us; {:.1f} MiB geometry kept; recipes identical on the first and last pass\n",
                                 (colorCached + depthCached) / 1e3 / std::max<double>(1, colorDraws + depthDraws),
                                 colorDraws ? colorCached / 1e3 / colorDraws : 0.0,
                                 depthDraws ? depthCached / 1e3 / depthDraws : 0.0,
                                 copyCached / 1e3 / std::max<double>(1, builtDraws),
                                 sceneCache.GeometryBytes() / 1048576.0);
        std::cout << std::format("  cached fastest pass per draw: color {:.2f}, depth-only {:.2f}\n",
                                 colorDraws ? fastest.first * iterations / 1e3 / colorDraws : 0.0,
                                 depthDraws ? fastest.second * iterations / 1e3 / depthDraws : 0.0);
        // Geometry over budget is decoded again; recipes stay the same.
        SceneBuildCache bounded{ held, 0 };
        colorCached = depthCached = copyCached = 0;
        cachedPasses(bounded, iterations, true, colorCached, depthCached, copyCached);
        std::cout << std::format("  cached with no geometry budget: color {:.2f} us, depth-only {:.2f} us; recipes identical\n", colorDraws ? colorCached / 1e3 / colorDraws : 0.0, depthDraws ? depthCached / 1e3 / depthDraws : 0.0);
    }

    // Decomposition of the color draws' build, step by step.
    static constexpr std::array<const char*, 10>                          kNames{ "pixel_digest", "fold_metadata", "inputs_copy", "fetch_patch_digest", "guest_registers", "fetch_match", "textures", "microcode", "shader_digests", "native_recipe" };
    std::array<std::uint64_t, kNames.size()>                              spent{};
    std::size_t                                                           decomposed = 0;
    LeaderTextureCache                                                    phaseCache;
    std::map<const SceneDrawShader*, NativeGameplayShaderValidationCache> validations;
    NativeGameplayGeometryCache                                           geometry{ std::size_t(512) << 20 };
    std::uint64_t                                                         cachedNs = 0;
    for (int iteration = 0; iteration < iterations + 1; ++iteration)
        for (std::size_t index = 0; index < draws.size(); ++index)
        {
            const auto& inputs = draws[index];
            if (!built[index] || !inputs.pixelObject)
                continue;
            SceneDrawRecipe recipe;
            UNSCOPED_INFO(std::string("a built draw rebuilds: ") + (error));
            REQUIRE(BuildSceneDrawRecipe(inputs, catalog, recipe, error, &phaseCache));
            std::array<Clock::time_point, kNames.size() + 1> t;
            t[0]              = Clock::now();
            const auto digest = IndexedBufferMicrocodeWordDigest(inputs.pixelCode);
            (void)catalog.WithPixel(digest);
            t[1]                             = Clock::now();
            std::vector<std::uint8_t> folded = inputs.vertexCode, state;
            UNSCOPED_INFO(std::string("fold: ") + (error));
            REQUIRE(FoldSolidShaderMetadata(inputs, folded, state, error));
            t[2]                   = Clock::now();
            SolidDrawInputs linked = inputs;
            linked.vertexCode      = folded;
            t[3]                   = Clock::now();
            std::vector<std::uint8_t> patched;
            UNSCOPED_INFO(std::string("patch: ") + (error));
            REQUIRE(PatchVertexFetch(linked, recipe.entry->attributeOffsets, patched, error));
            (void)IndexedBufferMicrocodeWordDigest(patched);
            t[4]           = Clock::now();
            auto registers = GuestDrawRegisters(state);
            t[5]           = Clock::now();
            (void)MatchShaderFetches(patched, inputs.pixelCode, recipe.entry->shader);
            t[6] = Clock::now();
            std::vector<NativeDrawReplayTexture> textures(recipe.textures.size());
            std::vector<NativeDrawReplaySampler> samplers(recipe.textures.size());
            for (std::uint32_t slot = 0; slot < textures.size(); ++slot)
            {
                (void)DecodeLeaderSampler(inputs.textureFetches[slot], recipe.entry->shader.textureDimensions[slot], samplers[slot], error, kLeaderAnisotropicOverride);
                UNSCOPED_INFO(std::string("decode: ") + (error));
                REQUIRE(phaseCache.Decode(inputs.textureFetches[slot], inputs.textureBaseBytes[slot], inputs.textureMipBytes[slot], textures[slot], error));
            }
            t[7]               = Clock::now();
            const auto toWords = [](std::span<const std::uint8_t> bytes)
            {
                std::vector<std::uint32_t> words;
                for (std::size_t i = 0; i < bytes.size(); i += 4)
                    words.push_back(word(bytes, i));
                return words;
            };
            const auto vs = toWords(recipe.patchedVertexCode), ps = toWords(inputs.pixelCode);
            t[8]               = Clock::now();
            const auto& shader = recipe.entry->shader;
            (void)CachedSha256::Of({ reinterpret_cast<const std::uint8_t*>(vs.data()), vs.size() * 4 });
            (void)CachedSha256::Of(shader.vertexShaderDxil);
            (void)CachedSha256::Of({ reinterpret_cast<const std::uint8_t*>(ps.data()), ps.size() * 4 });
            (void)CachedSha256::Of(shader.pixelShaderDxil);
            t[9] = Clock::now();
            // The adapter's view over the recipe's effective state.
            auto view = GuestDrawRegisters(recipe.effectiveState);
            for (const auto* literals : { &inputs.vertexLiteralRegisters, &inputs.pixelLiteralRegisters })
                for (const auto [reg, value] : *literals)
                    view[reg] = value;
            view[0x48BE] = inputs.streamFetches[0][0];
            view[0x48BF] = inputs.streamFetches[0][1];
            const std::array<NativeGameplayVertexFetchSpan, 1> fetches{ { { 95, inputs.streamFetches[0][0] & ~3U, inputs.streamBytes[0] } } };
            std::vector<NativeGameplayTextureBinding>          bindings;
            for (std::uint32_t slot = 0; slot < recipe.textures.size(); ++slot)
                bindings.push_back({ shader.textureFetchSlots[slot], &recipe.textures[slot], &recipe.samplers[slot] });
            NativeGameplayDrawView drawView;
            drawView.registers       = view;
            drawView.vertexMicrocode = vs;
            drawView.pixelMicrocode  = ps;
            drawView.vertexFetches   = fetches;
            drawView.textureBindings = bindings;
            drawView.indexBytes      = indexBytes(inputs);
            drawView.primitive       = inputs.primitive;
            drawView.indexCount      = inputs.indexCount;
            drawView.indexFormat     = inputs.indexFormat;
            drawView.indexEndian     = inputs.indexEndian;
            NativeGameplayDrawTarget target;
            const auto&              form = *recipe.form;
            target.width                  = form.width;
            target.height                 = form.height;
            const auto format             = SceneTargetColorFormat(form);
            target.format                 = format == 2 ? NativeDrawReplayTargetFormat::Rgb10a2 : format == 4 ? NativeDrawReplayTargetFormat::Rg16Snorm
                                                                                                              : NativeDrawReplayTargetFormat::Rgba8;
            if (form.sampleCount == 4 && form.width == 1280 && form.height == 720)
                target.fullViewportTarget = true;
            else
            {
                target.sampleCount = form.sampleCount;
                target.sampleMask  = form.sampleCount == 4 ? 9 : 1;
            }
            UNSCOPED_INFO(std::string("viewport: ") + (error));
            REQUIRE(DecodeNativeGameplayViewportAndScissor(view, target.viewport, target.scissor, error));
            NativeDrawReplayRecipe native;
            const auto             before = Clock::now();
            UNSCOPED_INFO(std::string("native: ") + (error));
            REQUIRE(BuildNativeGameplayDrawRecipe(drawView, shader, target, native, error));
            t[10] = Clock::now();
            // The same recipe with per-shader validation and geometry caches.
            NativeDrawReplayRecipe cachedNative;
            UNSCOPED_INFO(std::string("cached native: ") + (error));
            REQUIRE(BuildNativeGameplayDrawRecipe(drawView, shader, target, cachedNative, error, &validations[recipe.entry], &geometry));
            const auto cachedEnd = Clock::now();
            if (!iteration)
                continue;
            cachedNs += ns(t[10], cachedEnd);
            ++decomposed;
            for (std::size_t phase = 0; phase + 1 < kNames.size(); ++phase)
                spent[phase] += ns(t[phase], t[phase + 1]);
            spent.back() += ns(before, t[10]);
        }
    std::string   line;
    std::uint64_t total = 0;
    for (std::size_t phase = 0; phase < kNames.size(); ++phase)
    {
        line += std::format(" {}={:.2f}", kNames[phase], decomposed ? spent[phase] / 1e3 / decomposed : 0.0);
        // The native recipe performs the shader digests itself; they are its
        // submeasurement, outside the sum.
        if (std::string_view(kNames[phase]) != "shader_digests")
            total += spent[phase];
    }
    std::cout << std::format("  color draw steps, us per draw:{} (sum without shader_digests, which native_recipe includes, {:.2f}); native_recipe with validation and geometry caches {:.2f}\n", line, decomposed ? total / 1e3 / decomposed : 0.0, decomposed ? cachedNs / 1e3 / decomposed : 0.0);
    return 0;
}

// Zero color-mask draws retain their recorded routing.
int colorMaskFrame(const std::filesystem::path& world, const std::filesystem::path& catalogDirectory, const std::filesystem::path& capture)
{
    SceneDrawCatalog catalog;
    std::string      error;
    UNSCOPED_INFO(std::string("the scene catalog loads: ") + (error));
    REQUIRE(LoadSceneDrawCatalog(catalogDirectory, catalog, error));
    const auto tables = fetchTables(capture);
    // Census B5: keys A0FBD9A9, AB9C8D63, CDA354C5 at 0x8250C768. RB_COLOR_MASK
    // 0 occurs between 0xF draws under RB_MODECONTROL 4.
    const auto  frame  = toml::parse_file((world / "frames" / "021E7B3453487804-0" / "frame.toml").string());
    const auto* events = frame["events"].as_array();
    UNSCOPED_INFO("frame events");
    REQUIRE(events != nullptr);
    constexpr std::array<std::string_view, 3>                 kPairs{ "8250C768-A2E337A474B6C402-D713E61CF34EC80A-", "8250C768-6109F5EF0D30D03D-D48360A7", "8250C768-F34C58CCE2C2DE45-ECA19F8D7" };
    std::map<std::string_view, std::pair<unsigned, unsigned>> counts;
    for (const auto& node : *events)
    {
        const auto event = toml::node_view<const toml::node>(node);
        const auto pair  = event["pair"].value_or(std::string());
        const auto key   = std::ranges::find_if(kPairs, [&](std::string_view prefix)
                                                {
                                                  return pair.starts_with(prefix);
                                                });
        if (event["kind"].value_or(std::string()) != "draw" || key == kPairs.end())
            continue;
        const auto name   = std::format("frame draw {}", event["index"].value_or<std::int64_t>(-1));
        auto       inputs = frameDrawInputs(world, event, pair, tables, name);
        const auto mask   = word(inputs.state, 0x28DC);
        UNSCOPED_INFO(name + std::format(" color mask {:#x}", mask));
        REQUIRE((mask == 0xF || mask == kSceneNoColorMask));
        SceneDrawRecipe recipe;
        UNSCOPED_INFO(std::string(name + " builds: ") + (error));
        REQUIRE(BuildSceneDrawRecipe(inputs, catalog, recipe, error));
        UNSCOPED_INFO(name + " writes its color mask");
        REQUIRE((recipe.native.blend.writeMask == mask && recipe.native.indexCount == inputs.indexCount));
        auto& [masked, colored] = counts[*key];
        ++(mask ? colored : masked);
    }
    for (const auto prefix : kPairs)
    {
        const auto [masked, colored] = counts[prefix];
        UNSCOPED_INFO(std::string(prefix) + " draws with color mask 0 and 0xF");
        REQUIRE((masked > 0 && colored > 0));
        std::cout << std::format("Scene color mask 0: frame 021E7B3453487804-0 builds {} draws of {} without and {} with color writes\n", masked, prefix, colored);
    }
    return 0;
}

// B7 quads need full scissor; half-width/off-corner prove partial/no coverage.
int quadFrame(const std::filesystem::path& world, const std::filesystem::path& catalogDirectory, const std::filesystem::path& capture)
{
    SceneDrawCatalog catalog;
    std::string      error;
    UNSCOPED_INFO(std::string("the scene catalog loads: ") + (error));
    REQUIRE(LoadSceneDrawCatalog(catalogDirectory, catalog, error));
    const auto  tables = fetchTables(capture);
    const auto  frame  = toml::parse_file((world / "frames" / "D7AA1B9B851613F9-0" / "frame.toml").string());
    const auto* events = frame["events"].as_array();
    UNSCOPED_INFO("frame events");
    REQUIRE(events != nullptr);
    constexpr std::array<std::string_view, 2> kPairs{ "8250C768-410718F3B9741E31-323C72B4BF8A37B7-", "8250C768-E88067C94CBD1FFC-F36CCEFF549A2660-" };
    std::map<std::string, unsigned>           forms;
    for (const auto& node : *events)
    {
        const auto event = toml::node_view<const toml::node>(node);
        const auto pair  = event["pair"].value_or(std::string());
        if (event["kind"].value_or(std::string()) != "draw" || std::ranges::none_of(kPairs, [&](std::string_view prefix)
                                                                                    {
                                                                                        return pair.starts_with(prefix);
                                                                                    }))
            continue;
        const auto      name   = std::format("frame draw {}", event["index"].value_or<std::int64_t>(-1));
        const auto      inputs = frameDrawInputs(world, event, pair, tables, name);
        SceneDrawRecipe recipe;
        const bool      built = BuildSceneDrawRecipe(inputs, catalog, recipe, error);
        UNSCOPED_INFO(std::string(name + " builds in a 2X off-screen form: ") + (error));
        REQUIRE((built && (recipe.form == &kSceneTargetForms[5] || recipe.form == &kSceneTargetForms[6])));
        const auto covered = SceneQuadCoverage(recipe.native);
        const auto width = std::int32_t(recipe.form->scissor & 0x3FFF), height = std::int32_t(recipe.form->scissor >> 16);
        UNSCOPED_INFO(name + std::format(" covers its {}x{} scissor", width, height));
        REQUIRE((covered && covered->left == 0 && covered->top == 0 && covered->right == width && covered->bottom == height));

        // Stream 0 positions are big-endian floats at the start of each vertex.
        const auto stride   = std::size_t(inputs.arguments.stride);
        const auto vertices = inputs.streamBytes[0].size() / stride;
        UNSCOPED_INFO(name + " stream 0 holds the quad");
        REQUIRE((stride >= 12 && vertices >= 4));
        float minX = std::bit_cast<float>(word(inputs.streamBytes[0], 0)), maxX = minX;
        for (std::size_t v = 1; v < vertices; ++v)
        {
            const auto x = std::bit_cast<float>(word(inputs.streamBytes[0], v * stride));
            minX         = x < minX ? x : minX;
            maxX         = x > maxX ? x : maxX;
        }
        UNSCOPED_INFO(name + " quad has width");
        REQUIRE(minX < maxX);
        const auto mutated = [&](bool allCorners)
        {
            auto in = inputs;
            for (std::size_t v = 0; v < vertices; ++v)
                if (std::bit_cast<float>(word(in.streamBytes[0], v * stride)) == maxX)
                {
                    store(in.streamBytes[0], v * stride, std::bit_cast<std::uint32_t>((minX + maxX) / 2));
                    if (!allCorners)
                        break;
                }
            SceneDrawRecipe changed;
            UNSCOPED_INFO(std::string(name + " builds with a changed quad: ") + (error));
            REQUIRE(BuildSceneDrawRecipe(in, catalog, changed, error));
            return SceneQuadCoverage(changed.native);
        };
        const auto half = mutated(true);
        UNSCOPED_INFO(name + " narrowed to half width proves a smaller rectangle");
        REQUIRE((half && half->top == 0 && half->bottom == height && (half->left > 0 || half->right < width) && half->right - half->left < width));
        UNSCOPED_INFO(name + " with one corner moved proves no coverage");
        REQUIRE(!mutated(false));
        ++forms[std::format("{}x{}", recipe.native.width, recipe.native.height)];
    }
    std::string summary;
    for (const auto& [form, count] : forms)
        summary += std::format(" {} x{}", form, count);
    UNSCOPED_INFO("frame D7AA1B9B851613F9-0 draws both 2X off-screen forms:" + summary);
    REQUIRE(forms.size() == 2);
    std::cout << std::format("Scene 2X off-screen: frame D7AA1B9B851613F9-0 builds and covers{}\n", summary);
    return 0;
}

// A frame draw of harvest v3 that census h3 names as a target-form fixture,
// and the form it must build in.
struct FormFixture
{
    std::string_view frame, pair;
    std::size_t      form;
};

// Census h3 covers B1 k_16_16 fog and B3 packed 2X post forms.
constexpr std::array<FormFixture, 11> kFormFixtures{ {
    // B1 0x8230686C fog: A14A470B at 0x2D0, 35F5070D at 0, world frames
    // 1734/1737.
    { "FACE0A8FF1B328BE-0", "8230686C-C8D900E1F00F9A47-243D0AB207824289-", 8 },
    { "FACE0A8FF1B328BE-0", "8230686C-233A47EBAED69A97-243D0AB207824289-", 7 },
    { "FADA316BAEBA5E33-0", "8230686C-C8D900E1F00F9A47-243D0AB207824289-", 8 },
    { "FADA316BAEBA5E33-0", "8230686C-233A47EBAED69A97-243D0AB207824289-", 7 },
    // B3 2X 0x8250C768: 1280x720 F6B698CE/30CF19C3; 640x368 D4623A63/FB6B0B82
    // in Hall 1031 and world 2116/5127.
    { "66753FC1304530CE-0", "8250C768-DE0359DBD47D7FAB-560C753DAD5555AD-", 9 },
    { "1945EBA8AE5FAF4A-0", "8250C768-7C1DD533708BB131-818E4EAF9BA41B33-", 9 },
    { "1945EBA8AE5FAF4A-0", "8250C768-38691F81BEA4AD28-B8C1B55C7CE2E502-", 10 },
    { "1945EBA8AE5FAF4A-0", "8250C768-F95D105E51CF1A19-884C15F5AE654F89-", 10 },
    { "16FE8B2D8D174869-0", "8250C768-7C1DD533708BB131-818E4EAF9BA41B33-", 9 },
    { "16FE8B2D8D174869-0", "8250C768-38691F81BEA4AD28-B8C1B55C7CE2E502-", 10 },
    { "16FE8B2D8D174869-0", "8250C768-F95D105E51CF1A19-884C15F5AE654F89-", 10 },
} };

// Fixture pairs keep recorded color masks and disable depth for packed targets.
int formFrames(const std::filesystem::path& world, const std::filesystem::path& catalogDirectory, const std::filesystem::path& capture)
{
    SceneDrawCatalog catalog;
    std::string      error;
    UNSCOPED_INFO(std::string("the scene catalog loads: ") + (error));
    REQUIRE(LoadSceneDrawCatalog(catalogDirectory, catalog, error));
    const auto tables = fetchTables(capture);
    for (const auto& fixture : kFormFixtures)
    {
        const auto  frame  = toml::parse_file((world / "frames" / std::string(fixture.frame) / "frame.toml").string());
        const auto* events = frame["events"].as_array();
        UNSCOPED_INFO("frame events");
        REQUIRE(events != nullptr);
        const auto&                       form = kSceneTargetForms[fixture.form];
        std::map<std::uint32_t, unsigned> masks;
        for (const auto& node : *events)
        {
            const auto event = toml::node_view<const toml::node>(node);
            const auto pair  = event["pair"].value_or(std::string());
            if (event["kind"].value_or(std::string()) != "draw" || !pair.starts_with(fixture.pair))
                continue;
            const auto      name   = std::format("{} draw {}", fixture.frame, event["index"].value_or<std::int64_t>(-1));
            const auto      inputs = frameDrawInputs(world, event, pair, tables, name);
            SceneDrawRecipe recipe;
            UNSCOPED_INFO(std::string(name + " builds in its form: ") + (error));
            REQUIRE((BuildSceneDrawRecipe(inputs, catalog, recipe, error) && recipe.form == &form));
            const auto mask = word(inputs.state, 0x28DC);
            UNSCOPED_INFO(name + " keeps its color mask and a packed target's depth off");
            REQUIRE((recipe.native.blend.writeMask == mask && (SceneTargetColorFormat(form) == 0 || (!recipe.native.depth.enabled && !recipe.native.depth.writeEnabled))));
            ++masks[mask];
        }
        UNSCOPED_INFO(std::format("{} draws {}", fixture.frame, fixture.pair));
        REQUIRE(!masks.empty());
        std::string summary;
        for (const auto& [mask, count] : masks)
            summary += std::format(" mask {:#x} x{}", mask, count);
        std::cout << std::format("Scene target forms: {} builds {} in {:#x}/{:#x}:{}\n", fixture.frame, fixture.pair, form.surfaceInfo, form.colorInfo, summary);
    }
    return 0;
}

// Fog composite recipe covers 1280x720 with alpha 1 for its resolve source.
int compositeFrames(const std::filesystem::path& world, const std::filesystem::path& catalogDirectory, const std::filesystem::path& capture)
{
    // Fog key 0F7A46D5 at 0x8230686C proves 1280x720 alpha-1 overwrite for
    // 0x8250AFEC resolve.
    SceneDrawCatalog catalog;
    std::string      error;
    UNSCOPED_INFO(std::string("the scene catalog loads: ") + (error));
    REQUIRE(LoadSceneDrawCatalog(catalogDirectory, catalog, error));
    const auto     tables = fetchTables(capture);
    constexpr auto kPair  = std::string_view("8230686C-F65866A40B248418-F6CF41BEA360546B-");
    constexpr auto g      = std::uint64_t(0x40000000);
    for (const auto frameName : { "66753FC1304530CE-0", "536D6C34FA08452D-0", "FADA316BAEBA5E33-0" })
    {
        const auto  frame  = toml::parse_file((world / "frames" / frameName / "frame.toml").string());
        const auto* events = frame["events"].as_array();
        UNSCOPED_INFO("frame events");
        REQUIRE(events != nullptr);
        unsigned covered = 0;
        for (const auto& node : *events)
        {
            const auto event = toml::node_view<const toml::node>(node);
            const auto pair  = event["pair"].value_or(std::string());
            if (event["kind"].value_or(std::string()) != "draw" || !pair.starts_with(kPair))
                continue;
            const auto      name   = std::format("{} draw {}", frameName, event["index"].value_or<std::int64_t>(-1));
            const auto      inputs = frameDrawInputs(world, event, pair, tables, name);
            SceneDrawRecipe recipe;
            UNSCOPED_INFO(std::string(name + " builds: ") + (error));
            REQUIRE(BuildSceneDrawRecipe(inputs, catalog, recipe, error));
            UNSCOPED_INFO(name + std::format(" carries vertex hash {:016X}", recipe.native.vertexShaderHash));
            REQUIRE(recipe.native.vertexShaderHash == kSceneCompositeVertexHash);
            const auto           coverage = MakeSceneDrawCoverage([&](std::uint64_t address, std::uint64_t size, bool physical)
                                                                  {
                                                            if (physical || size != 4 || address < g || address - g + 4 > inputs.state.size())
                                                                return std::vector<std::uint8_t>{};
                                                            const auto at = std::size_t(address - g);
                                                            return std::vector<std::uint8_t>(inputs.state.begin() + at, inputs.state.begin() + at + 4);
                                                                  });
            NativeDrawSubmission submission;
            submission.arguments[0] = static_cast<std::uint32_t>(g);
            const auto cover        = coverage(submission, recipe.native);
            UNSCOPED_INFO(name + " proves the 1280x720 target overwritten with alpha 1");
            REQUIRE((cover && cover->alphaOne && cover->rect.left == 0 && cover->rect.top == 0 && cover->rect.right == 1280 && cover->rect.bottom == 720));
            ++covered;
        }
        UNSCOPED_INFO(std::format("{} draws the composite once, got {}", frameName, covered));
        REQUIRE(covered == 1);
        std::cout << std::format("Scene fog composite: {} builds and overwrites the 1280x720 target with alpha 1\n", frameName);
    }
    return 0;
}

// B4 reflection draws accept clip plane 0 only; planes 0 and 1 refuse.
int clipFrames(const std::filesystem::path& world, const std::filesystem::path& catalogDirectory, const std::filesystem::path& capture)
{
    // B4 keys 2EE8D7A9/A0FBD9A9 use 053F3E43's pair in world frames 1075/1122.
    SceneDrawCatalog catalog;
    std::string      error;
    UNSCOPED_INFO(std::string("the scene catalog loads: ") + (error));
    REQUIRE(LoadSceneDrawCatalog(catalogDirectory, catalog, error));
    const auto                                tables = fetchTables(capture);
    constexpr std::array<std::string_view, 3> kPairs{ "8250C768-F7EB47B270EC78CE-B52D04B35FF2B558-", "8250C768-A2E337A474B6C402-D713E61CF34EC80A-", "8250C768-E5D23DF7CBC77548-5A8502A3C960E9F3-" };
    std::map<std::string, unsigned>           clipped;
    for (const auto frameName : { "009B9D2A0BF5FD98-0", "7202C85B063061BA-0" })
    {
        const auto  frame  = toml::parse_file((world / "frames" / frameName / "frame.toml").string());
        const auto* events = frame["events"].as_array();
        UNSCOPED_INFO("frame events");
        REQUIRE(events != nullptr);
        for (const auto& node : *events)
        {
            const auto event = toml::node_view<const toml::node>(node);
            const auto pair  = event["pair"].value_or(std::string());
            if (event["kind"].value_or(std::string()) != "draw" || std::ranges::none_of(kPairs, [&](std::string_view prefix)
                                                                                        {
                                                                                            return pair.starts_with(prefix);
                                                                                        }))
                continue;
            const auto name   = std::format("{} draw {}", frameName, event["index"].value_or<std::int64_t>(-1));
            auto       inputs = frameDrawInputs(world, event, pair, tables, name);
            if (word(inputs.state, 0x2944) != 0x80001)
                continue;
            SceneDrawRecipe recipe;
            UNSCOPED_INFO(std::string(name + " builds with user clip plane 0: ") + (error));
            REQUIRE(BuildSceneDrawRecipe(inputs, catalog, recipe, error));
            UNSCOPED_INFO(name + " takes its pair's clipping entry");
            REQUIRE((recipe.entry->shader.userClipPlane && recipe.native.sharedConstants.size() == 480));
            for (std::size_t i = 0; i < 4; ++i)
            {
                std::uint32_t value;
                std::memcpy(&value, recipe.native.sharedConstants.data() + 464 + i * 4, 4);
                UNSCOPED_INFO(name + " binds its own plane 0");
                REQUIRE(value == word(inputs.state, 0x2820 + i * 4));
            }
            store(inputs.state, 0x2944, 0x80003);
            UNSCOPED_INFO(std::string(name + " with planes 0 and 1 is refused: ") + (error));
            REQUIRE((!BuildSceneDrawRecipe(inputs, catalog, recipe, error) && error == "scene_fixed_state 0x2204: 0x80003"));
            ++clipped[pair.substr(9, 8)];
        }
    }
    std::string summary;
    for (const auto& [pair, count] : clipped)
        summary += std::format(" {} x{}", pair, count);
    UNSCOPED_INFO("each hall reflection pair draws with user clip plane 0:" + summary);
    REQUIRE(clipped.size() == kPairs.size());
    std::cout << std::format("Scene user clip plane: frames 1075 and 1122 build{}\n", summary);
    return 0;
}

// Particle lowerer output supplies post-lowerer commit words; fetch 95 and
// empty geometry are the fixture.
void particleInputs(IndexedBufferDrawInputs& inputs, const std::array<std::uint32_t, 8>& r, const std::string& name)
{
    // 0x826A3334 emits fetch 95 from the post-lowerer commit; r9 is stride and
    // r8 index format.
    const auto vertexDest = word(inputs.state, 0x3488), vertexDwords = word(inputs.state, 0x3490), indexDwords = word(inputs.state, 0x3494);
    const auto physical = (vertexDest & 0x1FFFFFFFU) + (((vertexDest >> 20) + 512) & 0x1000U);
    inputs.startIndex   = 0;
    inputs.indexFormat  = (r[5] >> 2) & 1;
    inputs.indexEndian  = r[5] & 3;
    const auto width    = inputs.indexFormat ? 4U : 2U;
    UNSCOPED_INFO(name + std::format(" commit words match r6 {} * r9 {} and r7 {}", r[3], r[6], r[4]));
    REQUIRE((r[6] && std::uint64_t(r[3]) * r[6] / 4 == vertexDwords && indexDwords == (inputs.indexFormat ? r[4] : (r[4] + 1) / 2)));
    inputs.streamFetches[0] = { physical | 3U, 0x10000002U | ((vertexDwords << 2) & 0x03FFFFFCU) };
    if (inputs.streamBytes[0].empty() || inputs.streamBytes[0].size() != std::size_t(vertexDwords) * 4)
        inputs.streamBytes[0].assign(std::size_t(vertexDwords) * 4, 0);
    if (indexBytes(inputs).size() != std::size_t(inputs.indexCount) * width)
        inputs.indexBytes.assign(std::size_t(inputs.indexCount) * width, 0);
    inputs.arguments = { r[0], r[1], 0, 0, r[4], 0, inputs.indexFormat ? 4U : 0U, 0, r[6], kParticleDrawCaller };
}

// Builds `inputs` with its recorded RB_COLORCONTROL; a cataloged pair must be
// reached past the tuple rule. Counts the outcome under `outcomes`.
bool particleBuild(IndexedBufferDrawInputs& inputs, const SceneDrawCatalog& catalog, const std::string& name, std::map<std::string, unsigned>& outcomes)
{
    std::string     error;
    SceneDrawRecipe recipe;
    const bool      built = BuildSceneDrawRecipe(inputs, catalog, recipe, error);
    const auto      code  = built ? std::string("built") : error.substr(0, error.find(':'));
    ++outcomes[code];
    const auto vertexDigest = IndexedBufferMicrocodeWordDigest(inputs.vertexCode), pixelDigest = IndexedBufferMicrocodeWordDigest(inputs.pixelCode);
    const bool cataloged = std::ranges::any_of(catalog.entries, [&](const SceneDrawShader& entry)
                                               {
                                                   return entry.shader.vertexGuestWordDigest == vertexDigest && entry.shader.pixelGuestWordDigest == pixelDigest;
                                               });
    UNSCOPED_INFO(std::string(name + " reaches its cataloged pair past the tuple rule: ") + (error));
    REQUIRE((code != "scene_draw_tuple" && (!cataloged || !recipe.pair.empty())));
    if (built)
    {
        UNSCOPED_INFO(name + " draws its r7 indices");
        REQUIRE(recipe.native.indexCount == inputs.indexCount);
    }
    return built;
}

// Census B2: twelve harvest v3 particle keys and seven B2 frames.
int particleFrames(const std::filesystem::path& world, const std::filesystem::path& catalogDirectory, const std::filesystem::path& capture)
{
    SceneDrawCatalog catalog;
    std::string      error;
    UNSCOPED_INFO(std::string("the scene catalog loads: ") + (error));
    REQUIRE(LoadSceneDrawCatalog(catalogDirectory, catalog, error));
    const auto                      tables = fetchTables(capture);
    std::map<std::string, unsigned> keyOutcomes;
    unsigned                        keys = 0;
    for (const auto& directory : std::filesystem::directory_iterator(world))
    {
        const auto name = directory.path().filename().string();
        if (!directory.is_directory() || !name.starts_with(std::format("{:08X}-", kParticleDrawCaller)))
            continue;
        const auto table  = toml::parse_file((directory.path() / "record.toml").string());
        auto       inputs = inputsOf(directory.path(), table, tables);
        UNSCOPED_INFO(name + " is a direct lowerer record");
        REQUIRE(address(toml::node_view<const toml::node>(table)["entry"]) == 0x826A3048);
        std::array<std::uint32_t, 8> r{};
        for (std::size_t i = 0; i < r.size(); ++i)
            r[i] = address(toml::node_view<const toml::node>(table)["arguments_r3_r10"][i]);
        particleInputs(inputs, r, name);
        const bool built = particleBuild(inputs, catalog, name, keyOutcomes);
        std::cout << std::format("  particle key {}: {}\n", name, built ? "built" : "refused");
        ++keys;
    }
    UNSCOPED_INFO(std::format("the harvest holds the twelve particle keys ({})", keys));
    REQUIRE(keys == 12);
    std::string summary;
    for (const auto& [code, count] : keyOutcomes)
        summary += std::format(" {} x{}", code, count);
    std::cout << std::format("Particle keys: {} records:{}\n", keys, summary);

    constexpr std::array<std::string_view, 7> kFrames{ "66753FC1304530CE-0", "8B0C86769C2E86B0-0", "7202C85B063061BA-0", "7B862D4AC3E4F4E0-0", "E3AA356DAD7A4AB9-0", "00530A2D5C918DC3-0", "3A7EAE67AAEBFF7D-0" };
    for (const auto frame : kFrames)
    {
        const auto  form   = toml::parse_file((world / "frames" / frame / "frame.toml").string());
        const auto* events = form["events"].as_array();
        UNSCOPED_INFO("frame events");
        REQUIRE(events != nullptr);
        std::map<std::string, unsigned> outcomes;
        unsigned                        draws = 0;
        for (const auto& node : *events)
        {
            const auto event = toml::node_view<const toml::node>(node);
            if (event["kind"].value_or(std::string()) != "draw" || address(event["caller"]) != kParticleDrawCaller)
                continue;
            const auto name = std::format("frame {} draw {}", frame, event["index"].value_or<std::int64_t>(-1));
            UNSCOPED_INFO(name + " is a direct lowerer draw");
            REQUIRE(address(event["entry"]) == 0x826A3048);
            auto                         inputs = frameDrawInputs(world, event, event["pair"].value_or(std::string()), tables, name);
            std::array<std::uint32_t, 8> r{};
            for (std::size_t i = 0; i < r.size(); ++i)
                r[i] = address(event["arguments_r3_r10"][i]);
            // The vertices row the caller filled at G+0x3488 (0x82508BC8).
            std::vector<std::uint8_t> vertices;
            bool                      indexed = false;
            for (const auto& row : *event["memory"].as_array())
            {
                const auto entry = toml::node_view<const toml::node>(row);
                const auto role  = entry["role"].value_or(std::string());
                const auto bytes = [&]
                {
                    const auto file = entry["file"].value_or(std::string());
                    return file.empty() ? hexBytes(entry["bytes"].value_or(std::string())) : read(world / file);
                };
                if (role == "vertices")
                {
                    UNSCOPED_INFO(name + " vertices at G+0x3488");
                    REQUIRE(address(entry["address"]) == word(inputs.state, 0x3488));
                    vertices = bytes();
                }
                else if (role == "indices")
                {
                    UNSCOPED_INFO(name + " indices at G+0x348C");
                    REQUIRE(address(entry["address"]) == word(inputs.state, 0x348C));
                    indexed = true;
                }
            }
            UNSCOPED_INFO(name + " carries its indices and r6 * r9 vertex bytes");
            REQUIRE((indexed && vertices.size() == std::size_t(r[3]) * r[6]));
            inputs.streamBytes[0] = std::move(vertices);
            particleInputs(inputs, r, name);
            particleBuild(inputs, catalog, name, outcomes);
            ++draws;
        }
        UNSCOPED_INFO(std::string(frame) + " draws particles");
        REQUIRE(draws > 0);
        std::string text;
        for (const auto& [code, count] : outcomes)
            text += std::format(" {} x{}", code, count);
        std::cout << std::format("Particle frame {}: {} draws:{}\n", frame, draws, text);
    }
    return 0;
}

} // namespace harvest

// M2 replays captured order and registers with synthetic geometry/shaders;
// flags 0 imply copy control 0x00100000.
namespace passes
{

struct Event
{
    std::uint64_t sequence = 0, frame = 0;
    bool          draw = false;
    toml::table   table;
};

std::uint64_t integer(const toml::node_view<const toml::node>& node)
{
    if (const auto text = node.value<std::string>())
        return std::stoull(*text);
    return std::uint64_t(node.value_or<std::int64_t>(-1));
}

// The draw and resolve events of frames [first, last), one chunk at a time.
std::vector<Event> readEvents(const std::filesystem::path& file, std::uint64_t first, std::uint64_t last)
{
    std::ifstream in(file);
    UNSCOPED_INFO("cannot open " + file.string());
    REQUIRE(bool(in));
    std::vector<Event> events;
    std::string        chunk, line;
    const auto         flush = [&]
    {
        const bool draw = chunk.find("\nkind = 'draw'\n") != std::string::npos;
        if (!draw && chunk.find("\nkind = 'resolve'\n") == std::string::npos)
            return;
        const auto at = chunk.find("\nframe = ");
        UNSCOPED_INFO("an event without a frame");
        REQUIRE(at != std::string::npos);
        const auto frame = std::stoull(chunk.substr(at + 9, 12));
        if (frame < first || frame >= last)
            return;
        auto        parsed = toml::parse(chunk);
        const auto* array  = parsed["events"].as_array();
        UNSCOPED_INFO("one event per chunk");
        REQUIRE((array && array->size() == 1 && array->front().is_table()));
        Event event;
        event.table    = *array->front().as_table();
        event.frame    = frame;
        event.draw     = draw;
        event.sequence = integer(toml::node_view<const toml::node>(event.table)["sequence"]);
        events.push_back(std::move(event));
    };
    while (std::getline(in, line))
    {
        if (line.starts_with("[[events]]"))
        {
            if (!chunk.empty())
                flush();
            chunk.clear();
        }
        if (!chunk.empty() || line.starts_with("[[events]]"))
            chunk.append(line).push_back('\n');
    }
    if (!chunk.empty())
        flush();
    return events;
}

struct Built
{
    bool                   built = false, depthOnly = false;
    NativeDrawReplayRecipe native;
    std::string            detail;
};

// A depth-only draw: the captured registers, committed with the captured VS
// metadata, and VS words against the catalog entry, over three synthetic
// vertices at the VS's stride.
Built depthOnlyDraw(std::vector<std::uint8_t> state, const std::vector<std::uint8_t>& vertexCode, const std::vector<std::uint8_t>& vertexMetadata, const SceneDrawCatalog& catalog)
{
    Built built;
    built.depthOnly                            = true;
    const auto                          digest = IndexedBufferMicrocodeWordDigest(vertexCode);
    std::vector<const SceneDrawShader*> pair;
    for (const auto& entry : catalog.depthOnly)
        if (entry.shader.vertexGuestWordDigest == digest)
            pair.push_back(&entry);
    UNSCOPED_INFO("the captured depth-only VS is the catalog's: " + digest);
    REQUIRE(!pair.empty());
    if (!CommitSceneDepthOnlyState(state, vertexMetadata, built.detail))
        return built;
    auto                   registers = GuestDrawRegisters(state);
    const SceneTargetForm* form      = nullptr;
    const SceneDrawShader* entry     = nullptr;
    if (!CheckSceneDepthOnlyState(registers, pair, form, entry, built.detail))
        return built;
    std::vector<NativeGameplayVertexAttribute> attributes;
    UNSCOPED_INFO("the captured depth-only VS decodes");
    REQUIRE((DecodeShaderVertexFetches(vertexCode, attributes) && !attributes.empty()));
    const auto stride = attributes[0].strideBytes;
    UNSCOPED_INFO("the VS fetch stride is the shadow's stream stride");
    REQUIRE(stride == state[0x30F0] * 4U);
    std::vector<std::uint8_t> vertices(3 * stride);
    for (std::uint32_t v = 0; v < 3; ++v)
        for (std::uint32_t c = 0; c < 3; ++c)
            store(vertices, v * stride + c * 4, std::bit_cast<std::uint32_t>(float(v == c) * 0.5F));
    registers[0x48BE] = synthetic::kVertices | 3;
    registers[0x48BF] = std::uint32_t(vertices.size() / 4) << 2 | 2;
    std::vector<std::uint32_t> words;
    for (std::size_t i = 0; i < vertexCode.size(); i += 4)
        words.push_back(word(vertexCode, i));
    const std::array<NativeGameplayVertexFetchSpan, 1> fetches{ { { 95, synthetic::kVertices, vertices } } };
    const std::vector<std::uint8_t>                    indices{ 0, 0, 0, 1, 0, 2 };
    NativeGameplayDrawView                             view;
    view.registers       = registers;
    view.vertexMicrocode = words;
    view.vertexFetches   = fetches;
    view.indexBytes      = indices;
    view.primitive       = 4;
    view.indexCount      = 3;
    view.indexFormat     = 0;
    view.indexEndian     = 1;
    built.built          = BuildSceneDepthOnlyNative(view, state, *entry, *form, std::move(attributes), built.native, built.detail);
    return built;
}

// A draw with a pixel shader: the synthetic mesh pair under the captured
// target form, fixed state, depth, blend, color and raster controls.
Built pixelDraw(const std::vector<std::uint8_t>& state, const SceneDrawCatalog& catalog)
{
    Built       built;
    auto        registers = GuestDrawRegisters(state);
    const auto* form      = FindSceneTargetForm(registers);
    if (!form)
    {
        built.detail = std::format("scene_target_form: {:#x} {:#x} {:#x} {:#x}", registers[0x2000], registers[0x2001], registers[0x2002], registers[0x2082]);
        return built;
    }
    synthetic::Draw draw;
    draw.form    = *form;
    draw.depth   = registers[0x2200];
    draw.blend   = registers[0x2201];
    draw.color   = registers[0x2202];
    draw.raster  = registers[0x2205];
    auto  memory = synthetic::memoryFor(draw);
    auto& bytes  = memory.At(synthetic::kG, false).bytes;
    for (const auto [index, value] : kSceneFixedState)
        put(bytes, shadowOf(index), registers[index]);
    const auto      shaders = ScenePixelShaders(catalog);
    const auto      inputs  = synthetic::readScene(memory, shaders);
    SceneDrawRecipe recipe;
    built.built  = BuildSceneDrawRecipe(inputs, catalog, recipe, built.detail);
    built.native = recipe.native;
    return built;
}

// The physical address of a guest A0000000 or E0000000 view, as the SDK's
// physical heaps alias them (src/system/xmemory.cpp).
std::uint32_t physicalView(std::uint32_t address)
{
    return address >= 0xE0000000U ? address - 0xE0000000U + 0x1000 : address >= 0xA0000000U ? address - 0xA0000000U
                                                                                            : address;
}

// M2 records Resolve 0x8269E520; sample/clear flags and fetch formats choose
// copy words.
GuestResolveCopy capturedCopy(const toml::node_view<const toml::node>& record, std::uint32_t samples)
{
    GuestResolveCopy copy;
    const auto       flags = std::uint32_t(integer(record["flags"])), source = std::uint32_t(integer(record["source_index"]));
    UNSCOPED_INFO(std::format("resolve flags {:#x} hold only source {}, sample and clear bits", flags, source));
    REQUIRE((((flags & 7) == 0 || (flags & 7) == source) && !(flags & ~0x377U) && source <= kGuestDepthSlot));
    copy.copyControl = 0x00100000 | source | (flags & 0x370) | (samples == 4 && source < kGuestDepthSlot && !(flags & 0x70) ? 0x40 : 0);
    for (std::size_t i = 0; i < copy.destFetch.size(); ++i)
        copy.destFetch[i] = std::uint32_t(integer(record["texture"]["words"][i]));
    const auto fetchFormat = copy.destFetch[1] & 63;
    UNSCOPED_INFO("the event's destination format is the fetch format");
    REQUIRE(integer(record["destination_format"]) == fetchFormat);
    // 0x8269E750..0x8269E7C0 maps fetch 22/23 to copy 6 and fetch 54 to copy 7.
    const auto copyFormat = fetchFormat == 22 || fetchFormat == 23 ? 6 : fetchFormat == 54 ? 7
                                                                                           : fetchFormat;
    // RB_COPY_DEST_BASE is the physical base the fetch names; the event's
    // destination is the call's A0000000 or E0000000 view of it.
    copy.destBase = copy.destFetch[1] & 0xFFFFF000U;
    UNSCOPED_INFO("the event's destination views the fetch's physical base");
    REQUIRE((integer(record["texture"]["base"]) == copy.destBase && physicalView(std::uint32_t(integer(record["destination_base"]))) == copy.destBase));
    copy.destPitch  = std::uint32_t(integer(record["raw_resolve_pitch"]) | integer(record["resolve_height"]) << 16);
    copy.destInfo   = std::uint32_t(copyFormat << 7 | integer(record["raw_resolve_endian"]));
    const auto rect = [&](std::size_t i)
    {
        return std::uint32_t(integer(record["rectangle"][i]));
    };
    copy.rectX          = rect(0);
    copy.rectY          = rect(1);
    copy.rectWidth      = rect(2) - rect(0);
    copy.rectHeight     = rect(3) - rect(1);
    copy.destX          = std::uint32_t(integer(record["destination_offset_xy"][0]));
    copy.destY          = std::uint32_t(integer(record["destination_offset_xy"][1]));
    std::uint32_t color = 0;
    for (std::size_t i = 0; i < 4; ++i)
    {
        const auto value = std::clamp(std::bit_cast<float>(std::uint32_t(integer(record["raw_clear_words"][i]))), 0.0F, 1.0F);
        color |= std::uint32_t(std::lround(value * 255.0F)) << (8 * i);
    }
    copy.colorClear  = color;
    const auto depth = std::clamp(std::bit_cast<double>(integer(record["raw_f1_bits"])), 0.0, 1.0);
    copy.depthClear  = std::uint32_t(std::llround(depth * 16777215.0)) << 8;
    return copy;
}

// Captured and harvest frame forms share ordered draws/resolves; missing clears
// are synthesized.
struct FrameForm
{
    std::string                                        name;
    std::uint64_t                                      frame = 0;
    std::vector<const Event*>                          window;
    std::map<std::uint64_t, Built>                     built;
    std::map<std::uint64_t, std::vector<std::uint8_t>> states;
    std::array<unsigned, 8>                            counts{}; // draws before each resolve
    unsigned                                           depthOnly = 0, pixel = 0, unrouted = 0;
};

// One pass-3 tile resolve as the hook would report it.
struct TileResolve
{
    std::uint64_t    sequence = 0;
    std::uint32_t    tile = 0, slot = 0;
    GuestResolveCopy copy;
};

struct FrameRun
{
    std::vector<std::uint64_t>        keys; // passes 0-2
    std::set<std::string>             statuses;
    std::vector<std::uint64_t>        tileMarks;
    std::vector<NativeCompletionSpan> tileSpans;
    std::string                       tileError;
    bool                              tilePartial      = false;
    unsigned                          tileDraws        = 0;
    unsigned                          tileFrontbuffers = 0; // scanout candidates by the hook's classifier
};

constexpr std::array<std::uint32_t, 4> kOffscreenDestinations{ 0xFE7AF000, 0xFE3AF000, 0xFE7AF000, 0xFDFAF000 };
constexpr std::uint32_t                kSceneDepthCaller = 0x82510BFC;

// Frame run covers passes 0-2 and presented pass 3; first refusal stops the
// run.
FrameRun driveFrame(const FrameForm& form, const std::function<void(std::vector<TileResolve>&)>& edit = {})
{
    // The assembler reads the current draw's shadow at its G and decodes its
    // RT0 and depth objects from the shadow's registers; `overrides` holds
    // G-relative words a resolve or clear sees beyond that shadow.
    const std::vector<std::uint8_t>*       current = nullptr;
    std::uint32_t                          g       = 0;
    std::map<std::uint32_t, std::uint32_t> overrides;
    FakeCompletion                         completion;
    NativeFrameAssemblerConfig             config;
    config.completion = &completion;
    // Caller 0x82510BFC has no route (CG-10); skip mode keeps the frame and
    // marks what it read partial.
    if (form.unrouted)
        config.mode = NativeDrawMode::Skip;
    config.readTarget = [&](std::uint32_t address, std::uint32_t& out)
    {
        const auto& state = *current;
        if (address >= g && overrides.contains(address - g))
        {
            out = overrides.at(address - g);
            return true;
        }
        if (address >= g && address - g <= state.size() - 4)
        {
            out = word(state, address - g);
            return true;
        }
        const auto scissor = word(state, shadowOf(0x2082));
        for (const auto [object, info] : { std::pair{ word(state, 0x3098), shadowOf(0x2001) }, std::pair{ word(state, 0x30A8), shadowOf(0x2002) } })
            if (object && address >= object && address - object < 0x28)
                switch (address - object)
                {
                    case 0x18:
                        out = word(state, shadowOf(0x2000)) & 0x3FFFF;
                        return true;
                    case 0x1C:
                        out = word(state, info);
                        return true;
                    case 0x24:
                        out = (((scissor & 0x3FFF) - 1) << 18) | ((((scissor >> 16) & 0x3FFF) - 1) << 3);
                        return true;
                    default:
                        return false;
                }
        return false;
    };
    config.build = [&](const NativeDrawSubmission& submission, NativeDrawReplayRecipe& recipe, std::string& detail)
    {
        const auto& draw = form.built.at(submission.sequence);
        recipe           = draw.native;
        detail           = draw.detail;
        return draw.built;
    };
    NativeFrameAssembler assembler(config);
    std::string          error;
    const auto           at = [&](const Event* event)
    {
        current = &form.states.at(event->sequence);
        g       = std::uint32_t(integer(toml::node_view<const toml::node>(event->table)["raw_r3_r10_u64"][0]));
    };
    const auto flushed = [&](std::uint64_t mark) -> const NativeCompletionSpan&
    {
        completion::DrawFlush flush;
        for (unsigned attempt = 0; attempt < 4 && !flush.fence; ++attempt)
            flush = assembler.FlushThrough(mark);
        UNSCOPED_INFO("the resolve frame is submitted: " + flush.error);
        REQUIRE((flush.fence != 0 && !assembler.Failure()));
        const auto span = std::ranges::find(completion.spans, mark, &NativeCompletionSpan::resolveMark);
        UNSCOPED_INFO("the resolve has its own producer span");
        REQUIRE((span != completion.spans.end() && span->frame && span->guestResolve));
        return *span;
    };

    // Pass 0's assumed full clear of the 1024x1024 target and its depth.
    at(form.window.front());
    GuestClear        clear;
    GuestTiling       tiling;
    GuestDepthSurface depth;
    UNSCOPED_INFO("the pass 0 target, tiling and depth decode");
    REQUIRE((ReadGuestTargetSet(config.readTarget, g, 0, clear.target) && ReadGuestTiling(config.readTarget, g, tiling) &&
             ReadGuestDepthSurface(config.readTarget, g, tiling, depth)));
    UNSCOPED_INFO("pass 0 draws a 1024x1024 1X target over depth base 0x4C0");
    REQUIRE((clear.target.native.width == 1024 && clear.target.native.height == 1024 && clear.target.native.sampleCount == 1 && depth.edramBase == 0x4C0));
    clear.tiling           = tiling;
    clear.depth            = depth;
    clear.flags            = 0x31;
    clear.clear.rectangle  = { 0, 0, 1024, 1024 };
    clear.clear.color      = true;
    clear.clear.depth      = true;
    clear.clear.colorValue = { 0, 0, 0, 1 };
    // CG-18: this assumed full color/depth clear is absent from capture.
    UNSCOPED_INFO("the assumed pass 0 clear: " + error);
    REQUIRE(assembler.Clear(clear, error) != 0);

    FrameRun                 run;
    unsigned                 admitted = 0, resolves = 0;
    bool                     tiledCleared = false;
    std::vector<TileResolve> tiles;
    for (const auto* event : form.window)
    {
        const auto record = toml::node_view<const toml::node>(event->table);
        if (event->draw)
        {
            at(event);
            if (resolves == 4 && !tiledCleared)
            {
                tiledCleared = true;
                // Pass 3's assumed tile-0 predicated clear of the logical
                // screen and its depth, as BeginTiling's first clear issues it.
                overrides = { { 0x31A4, 3 }, { 0x31AC, 1 } };
                GuestClear tiled;
                UNSCOPED_INFO("the pass 3 target, tiling and depth decode as the saved tile surfaces");
                REQUIRE((ReadGuestTargetSet(config.readTarget, g, 0, tiled.target) && ReadGuestTiling(config.readTarget, g, tiled.tiling) &&
                         tiled.target.tiling && ReadGuestDepthSurface(config.readTarget, g, tiled.tiling, depth) && depth.tiled));
                UNSCOPED_INFO("pass 3 draws the 1280x720 2X presented target as two 640x720 tiles over depth 0x530");
                REQUIRE((tiled.target.native == NativeRenderFramePass::Target{ 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 } && depth.edramBase == 0x530 &&
                         tiled.tiling.count == 2 && tiled.tiling.rects[0] == GuestRect{ 0, 0, 640, 720 } && tiled.tiling.rects[1] == GuestRect{ 640, 0, 1280, 720 }));
                tiled.depth           = depth;
                tiled.flags           = 0x3F;
                tiled.predicate       = tiled.tiling.predicated;
                tiled.clear.rectangle = { 0, 0, 1280, 720 };
                tiled.clear.color = tiled.clear.depth = tiled.clear.stencil = true;
                tiled.clear.colorTargets                                    = 1;
                tiled.clear.depthValue                                      = 1.0F;
                UNSCOPED_INFO("the assumed pass 3 tiled clear: " + error);
                REQUIRE(assembler.Clear(tiled, error) != 0);
                overrides.clear();
            }
            NativeDrawSubmission submission;
            submission.hook   = NativeDrawHook::IndexedBuffer;
            submission.caller = std::uint32_t(integer(record["caller_lr"]));
            for (std::size_t i = 0; i < submission.arguments.size(); ++i)
                submission.arguments[i] = std::uint32_t(integer(record["raw_r3_r10_u64"][i]));
            submission.sequence      = event->sequence;
            submission.frame         = form.frame;
            submission.stateRead     = true;
            submission.operationMark = assembler.NumberOperation();
            assembler.Began(submission);
            const auto  verdict = assembler.Admit(submission);
            std::string refusal;
            assembler.Recorded(submission, verdict, &refusal);
            const bool routed = submission.caller != kSceneDepthCaller;
            UNSCOPED_INFO(std::format("frame {} draw {} admission: {}", form.frame, event->sequence, refusal));
            REQUIRE(!verdict == routed);
            if (resolves == 4)
                run.tileDraws += routed ? 1 : 0;
            else
                ++admitted;
            continue;
        }
        const auto index = resolves++;
        if (index >= 4)
        {
            const auto copy = capturedCopy(record, 4);
            tiles.push_back({ event->sequence, copy.rectX / 640, copy.copyControl & 7, copy });
            continue;
        }
        const auto destination = std::uint32_t(integer(record["destination_base"]));
        const auto copy        = capturedCopy(record, 1);
        UNSCOPED_INFO("the copy writes the destination's physical base");
        REQUIRE(copy.destBase == physicalView(destination));
        UNSCOPED_INFO(std::format("frame {} resolve {} is the full-rect 1024x1024 RGBA8 copy to {:#x}", form.frame, event->sequence, kOffscreenDestinations[index]));
        REQUIRE((destination == kOffscreenDestinations[index] && copy.copyControl == 0x100000 && copy.destInfo == 0x300 && copy.rectX == 0 &&
                 copy.rectY == 0 && copy.rectWidth == 1024 && copy.rectHeight == 1024));
        GuestTarget target;
        UNSCOPED_INFO("the resolve source is the pass 0 target");
        REQUIRE((ReadGuestTargetSet(config.readTarget, g, 0, target) && target.native == clear.target.native));
        bool partial = false;
        UNSCOPED_INFO("a 1024x1024 copy is no scanout candidate");
        REQUIRE(!assembler.ScanoutCandidate(target, copy));
        const auto mark = assembler.Resolve(target, copy, false, partial, error);
        UNSCOPED_INFO(std::format("frame {} resolve {} is modeled: {}", form.frame, event->sequence, error));
        REQUIRE((mark != 0 && !partial));
        const auto& span = flushed(mark);
        UNSCOPED_INFO(std::format("frame {} resolve {} replays {} draws into its own producer ({})", form.frame, event->sequence, admitted, NativeFrameStatusName(span.status)));
        REQUIRE(((span.status == NativeFrameStatus::Complete || span.status == NativeFrameStatus::Provisional) && span.frame->passes.size() == 1 &&
                 span.frame->passes[0].DrawCount() == admitted && span.frame->passes[0].target.width == 1024 &&
                 span.frame->passes[0].target.depthEdramBase == 0x4C0 && span.frame->textures.size() == 1 &&
                 span.frame->textures[0].persistentKey == ((std::uint64_t(copy.destBase) << 32) | mark)));
        run.keys.push_back(span.frame->textures[0].persistentKey);
        run.statuses.insert(NativeFrameStatusName(span.status));
    }
    if (edit)
        edit(tiles);
    for (const auto& resolve : tiles)
    {
        // 0x8250AC90 predicates tile i's resolves to that tile alone.
        overrides = { { 0x31A4, 3U << (2 * resolve.tile) }, { 0x31AC, 1 } };
        GuestTarget target;
        UNSCOPED_INFO("the tile resolve source decodes as the tiled target");
        REQUIRE((ReadGuestTargetSet(config.readTarget, g, resolve.slot, target) && target.tiling));
        // The hook's classifier routes each tile: the depth tile opens the
        // texture pass, so its color tiles are no scanout candidates.
        const bool frontbuffer = assembler.ScanoutCandidate(target, resolve.copy);
        run.tileFrontbuffers += frontbuffer ? 1U : 0U;
        const auto mark = assembler.Resolve(target, resolve.copy, frontbuffer, run.tilePartial, run.tileError);
        if (!mark)
            break;
        run.tileMarks.push_back(mark);
        // A scanout candidate waits for its VdSwap rather than producing.
        if (!frontbuffer)
            run.tileSpans.push_back(flushed(mark));
    }
    return run;
}

int run(const std::string& captures, const std::filesystem::path& catalogDirectory)
{
    SceneDrawCatalog catalog;
    std::string      error;
    UNSCOPED_INFO("the scene catalog loads with its depth-only list: " + error);
    REQUIRE((LoadSceneDrawCatalog(catalogDirectory, catalog, error) && !catalog.depthOnly.empty()));
    const auto  synthetic = synthetic::fullCatalog();
    std::size_t start     = 0;
    bool        mutated   = false;
    for (std::size_t end = 0; end <= captures.size(); ++end)
    {
        if (end < captures.size() && captures[end] != ';')
            continue;
        const std::filesystem::path capture = captures.substr(start, end - start);
        start                               = end + 1;
        const auto result                   = toml::parse_file((capture / "result.toml").string());
        const auto begin = std::uint64_t(result["begin_frame"].value_or<std::int64_t>(0)), last = std::uint64_t(result["end_frame"].value_or<std::int64_t>(0));
        UNSCOPED_INFO("a complete capture: " + capture.string());
        REQUIRE((result["complete"].value_or(false) && begin < last));
        const auto events = readEvents(capture / "events.toml", begin, last);

        std::map<std::string, std::vector<std::uint8_t>> vertexCode;
        for (auto frame = begin; frame < last; ++frame)
        {
            // From the frame's first draw through pass 3's fourth resolve.
            FrameForm form;
            form.name      = capture.filename().string();
            form.frame     = frame;
            unsigned count = 0;
            for (const auto& event : events)
                if (event.frame == frame && (event.draw || !form.window.empty()) && count < form.counts.size())
                {
                    form.window.push_back(&event);
                    if (event.draw)
                        ++form.counts[count];
                    else
                        ++count;
                }
            UNSCOPED_INFO(std::format("frame {} reaches the pass 3 resolves", frame));
            REQUIRE(count == form.counts.size());
            for (const auto* event : form.window)
            {
                if (!event->draw)
                    continue;
                const auto record = toml::node_view<const toml::node>(event->table);
                auto&      state = form.states[event->sequence] = read(capture / record["state_file"].value_or(std::string()));
                UNSCOPED_INFO("a 0x3500-byte draw state");
                REQUIRE(state.size() == 0x3500);
                Built      draw;
                const bool unrouted = std::uint32_t(integer(record["caller_lr"])) == kSceneDepthCaller;
                if (unrouted)
                {
                    draw.detail = "caller 0x82510BFC has no route (CG-10)";
                    ++form.unrouted;
                }
                else if (integer(record["ps"]["object"]) == 0)
                {
                    const auto sha  = record["vs"]["source_sha256"].value_or(std::string());
                    auto&      code = vertexCode[sha];
                    if (code.empty())
                        code = read(capture / std::format("vs-{}.be.bin", sha));
                    draw = depthOnlyDraw(state, code, harvest::hexBytes(record["vs"]["metadata_hex"].value_or(std::string())), catalog);
                }
                else
                    draw = pixelDraw(state, *synthetic);
                UNSCOPED_INFO(std::format("frame {} draw {} builds: [{}]", frame, event->sequence, draw.detail));
                REQUIRE((draw.built || unrouted));
                if (draw.built)
                    ++(draw.depthOnly ? form.depthOnly : form.pixel);
                form.built[event->sequence] = std::move(draw);
            }
            const auto& c = form.counts;
            UNSCOPED_INFO(std::format("frame {} holds {} draws per resolve: passes 0-2 then pass 3 before its four tile resolves", frame, c[0] + c[1] + c[2] + c[3] + c[4]));
            REQUIRE((c[0] > 0 && c[1] == 1 && c[2] == 0 && c[3] == 1 && c[4] > 0 && c[5] == 0 && c[6] == 0 && c[7] == 0));

            const auto run = driveFrame(form);
            // CG-06: the two resolves to 0xFE7AF000 are distinct producers.
            UNSCOPED_INFO("the two resolves to 0xFE7AF000 have their own keys");
            REQUIRE((run.keys.size() == 4 && run.keys[0] != run.keys[2] && (run.keys[0] >> 32) == (run.keys[2] >> 32)));
            std::string status;
            for (const auto& name : run.statuses)
                status += (status.empty() ? "" : "/") + name;
            std::cout << std::format("Scene passes 0-3, {} frame {}: {} depth-only and {} pixel draws built; the four pass 0-2 resolves {}\n", form.name, frame, form.depthOnly, form.pixel, status);

            if (form.unrouted)
            {
                // The skipped draws leave the tiled source partial.
                UNSCOPED_INFO(std::format("frame {} pass 3 with {} unrouted draws reports its first tile resolve partial: {}", frame, form.unrouted, run.tileError));
                REQUIRE((run.tileMarks.empty() && run.tilePartial && refusedAs(run.tileError, "resolve_source_partial")));
                std::cout << std::format("Scene pass 3, {} frame {}: {} draws and {} unrouted 0x82510BFC draws (CG-10); the tile resolves report partial\n", form.name, frame, run.tileDraws, form.unrouted);
                continue;
            }
            // Pass 3's per-tile resolves: depth k_24_8 (k1) then color (k01,
            // clear bits on tile 0), each a producer of the whole pass.
            UNSCOPED_INFO(std::format("frame {} pass 3 resolves every tile as a texture: {}", frame, run.tileError));
            REQUIRE((run.tileError.empty() && run.tileSpans.size() == 4 && run.tileFrontbuffers == 0));
            constexpr std::array<std::uint32_t, 4> kSlots{ kGuestDepthSlot, 0, kGuestDepthSlot, 0 };
            constexpr std::array<std::uint32_t, 4> kControls{ 0x00100014, 0x00100340, 0x00100014, 0x00100040 };
            std::set<std::uint64_t>                tileKeys;
            std::set<std::string>                  tileStatuses;
            for (std::size_t n = 0; n < run.tileSpans.size(); ++n)
            {
                const auto& span  = run.tileSpans[n];
                const auto& pass  = span.frame->passes[0];
                const auto& copy  = *span.guestResolve;
                const bool  depth = kSlots[n] == kGuestDepthSlot;
                UNSCOPED_INFO(std::format("frame {} tile resolve {} replays {} draws over the tile clears and copies its tile ({})", frame, n, run.tileDraws, NativeFrameStatusName(span.status)));
                REQUIRE(((span.status == NativeFrameStatus::Complete || span.status == NativeFrameStatus::Provisional) && span.frame->passes.size() == 1 &&
                         pass.DrawCount() == run.tileDraws && pass.target.width == 1280 && pass.target.sampleCount == 4 &&
                         pass.target.depthEdramBase == 0x530 && pass.clears.size() == (n < 2 ? 1U : 3U) && pass.resolves[0].depth == depth &&
                         pass.resolves[0].sourceX == (n / 2) * 640 && pass.resolves[0].width == 640 && copy.copyControl == kControls[n] &&
                         copy.packing == (depth ? GuestResolvePacking::Depth24Stencil8Words : GuestResolvePacking::Color8888Words) &&
                         copy.destBase == (depth ? 0x1D0F0000U : 0x1D818000U) && span.frame->textures[0].format == NativeDrawReplayTextureFormat::R32Uint));
                // The guest image the producer's words encode to is admitted.
                const std::vector<std::uint8_t> texels(std::size_t(copy.rectWidth) * copy.rectHeight * 4);
                GuestResolveImage               image;
                UNSCOPED_INFO(std::format("frame {} tile resolve {} encodes: {}", frame, n, error));
                REQUIRE(EncodeGuestResolve(copy, texels, copy.rectWidth * 4, image, error));
                tileKeys.insert(span.frame->textures[0].persistentKey);
                tileStatuses.insert(NativeFrameStatusName(span.status));
            }
            UNSCOPED_INFO("each tile resolve is its own producer");
            REQUIRE(tileKeys.size() == 4);
            status.clear();
            for (const auto& name : tileStatuses)
                status += (status.empty() ? "" : "/") + name;
            std::cout << std::format("Scene pass 3, {} frame {}: {} draws on the 2X tiled target; depth and color resolves of both tiles {}\n", form.name, frame, run.tileDraws, status);

            if (mutated)
                continue;
            mutated = true;

            const std::array<std::pair<std::function<void(std::vector<TileResolve>&)>, std::string>, 4> mutants{ {
                { [](std::vector<TileResolve>& tiles)
                  {
                      std::swap(tiles[0], tiles[1]);
                  },
                  "tiled_texture_pass_after_scanout" },
                { [](std::vector<TileResolve>& tiles)
                  {
                      tiles[1].copy.copyControl &= ~0x300U;
                  },
                  "tiled_background_undefined: tile 1 slot 4" },
                { [](std::vector<TileResolve>& tiles)
                  {
                      tiles[0].copy.rectX = tiles[0].copy.destX = 640;
                  },
                  "tiled_resolve_form: tile 0 rect 640,0,1280,720 point 640,0 mask 0x3" },
                { [](std::vector<TileResolve>& tiles)
                  {
                      tiles[0].copy.copyControl &= ~7U;
                  },
                  "resolve_form_unsupported: control 00100010" },
            } };
            for (const auto& [edit, expected] : mutants)
            {
                const auto refused = driveFrame(form, edit);
                UNSCOPED_INFO(std::format("frame {} mutant is refused as {}: [{}]", frame, expected, refused.tileError));
                REQUIRE(refusedAs(refused.tileError, expected));
            }
            std::cout << std::format("Scene pass 3, {} frame {}: order, clear-bit, rectangle and source-select mutants are refused by name\n", form.name, frame);
        }
    }
    return 0;
}

} // namespace passes

} // namespace

// The draw-cost benchmark over the harvest v3 world frame; only unset inputs skip.
void bench(const std::string& signature, int iterations, std::size_t segments, bool fresh)
{
    const auto world = environment("REREVVED_WORLD_HARVEST3"), catalog = environment("REREVVED_WORLD_SCENE_SHADERS"), capture = environment("REREVVED_LEADER_CAPTURE");
    if (world.empty() || catalog.empty() || capture.empty() || environment("REREVVED_TEST_NATIVE_D3D12") != "1")
    {
        SKIP("Set REREVVED_WORLD_HARVEST3, REREVVED_WORLD_SCENE_SHADERS, REREVVED_LEADER_CAPTURE and REREVVED_TEST_NATIVE_D3D12=1 to run the draw-cost benchmark");
    }
    harvest::benchFrame(world, catalog, capture, signature, iterations, segments, fresh);
}

TEST_CASE("scene_draw_adapter", "[ctest][scene-draw-adapter][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    unit();
}

TEST_CASE("scene_draw_adapter_harvest", "[.][renderer-research][scene-draw-adapter][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    // Only an unset input skips; a set but unusable path fails.
    const auto roots = environment("REREVVED_SCENE_HARVEST");
    if (roots.empty())
    {
        SKIP("Set REREVVED_SCENE_HARVEST to the shader harvests (separated by ;) to run these checks");
    }
    const auto catalog = environment("REREVVED_SCENE_SHADERS"), capture = environment("REREVVED_LEADER_CAPTURE");
    UNSCOPED_INFO("REREVVED_SCENE_HARVEST is set; the harvest checks also need REREVVED_SCENE_SHADERS and REREVVED_LEADER_CAPTURE");
    REQUIRE((!catalog.empty() && !capture.empty()));
    harvest::run(roots, catalog, capture);
    // The catalog rebuilt with the world harvest's pairs, over every root.
    const auto worldCatalog = environment("REREVVED_WORLD_SCENE_SHADERS"), world = environment("REREVVED_WORLD_HARVEST");
    if (worldCatalog.empty())
        return;
    UNSCOPED_INFO("REREVVED_WORLD_SCENE_SHADERS is set; its check also needs REREVVED_WORLD_HARVEST");
    REQUIRE(!world.empty());
    harvest::run(roots + ";" + world, worldCatalog, capture);
    harvest::colorMaskFrame(world, worldCatalog, capture);
    harvest::quadFrame(world, worldCatalog, capture);
    // Census h3's target-form, clip and particle fixtures, from the armed
    // harvest v3.
    const auto world3 = environment("REREVVED_WORLD_HARVEST3");
    if (world3.empty())
    {
        std::cout << "Set REREVVED_WORLD_HARVEST3 to the harvest v3 world capture to run the target-form, clip and particle fixtures\n";
        return;
    }
    harvest::formFrames(world3, worldCatalog, capture);
    harvest::clipFrames(world3, worldCatalog, capture);
    harvest::compositeFrames(world3, worldCatalog, capture);
    harvest::particleFrames(world3, worldCatalog, capture);
}

TEST_CASE("scene_draw_adapter_passes", "[.][renderer-research][scene-draw-adapter][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    const auto                     captures = environment("REREVVED_SCENE_PASSES");
    if (captures.empty())
    {
        SKIP("Set REREVVED_SCENE_PASSES to the M2 scene resource captures (separated by ;) to run these checks");
    }
    const auto catalog = environment("REREVVED_SCENE_SHADERS");
    UNSCOPED_INFO("REREVVED_SCENE_PASSES is set; the pass checks also need REREVVED_SCENE_SHADERS");
    REQUIRE(!catalog.empty());
    passes::run(captures, catalog);
}

TEST_CASE("scene_draw_adapter_draw_cost", "[.][renderer-research][scene-draw-adapter][native_gpu][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    bench("BF090116ED516AFB-0", 5, 1, false);
}

// Graph segments over new payload buffers of unchanged bytes, as uncached families decode.
TEST_CASE("scene_draw_adapter_draw_cost_segments", "[.][renderer-research][scene-draw-adapter][native_gpu][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    bench("BF090116ED516AFB-0", 5, 4, true);
}

// The cache identity check: scene-draw-adapter.identity.
TEST_CASE("scene_draw_adapter_identity", "[.][manual][scene-draw-adapter]")
{
    rerevved::tests::ScopedLogging logging;
    synthetic::identityCache();
}

#if REREVVED_DEVELOPER
// The vertex-shader code watch: scene-draw-adapter.vs-code.
TEST_CASE("scene_draw_adapter_vs_code", "[.][manual][scene-draw-adapter]")
{
    rerevved::tests::ScopedLogging logging;
    synthetic::hostPatchedRouting();
    synthetic::vsCodeWatch();
}
#endif

// The draw-cost benchmark: scene-draw-adapter.bench [frame [iterations [segments [fresh]]]].
TEST_CASE("scene_draw_adapter_bench", "[.][manual][scene-draw-adapter]")
{
    auto&                          args = rerevved::tests::CurrentManualArgv();
    rerevved::tests::ScopedLogging logging;
    char* const*                   argv = args.argv();
    const int                      argc = args.argc() + 1;
    bench(argc > 1 ? argv[1] : "BF090116ED516AFB-0", argc > 2 ? std::stoi(argv[2]) : 5, argc > 3 ? std::stoul(argv[3]) : 1, argc > 4 && std::string_view(argv[4]) == "fresh");
}

// The per-draw build-cost profile: scene-draw-adapter.cost [frame [iterations]].
TEST_CASE("scene_draw_adapter_cost", "[.][manual][scene-draw-adapter]")
{
    auto&                          args = rerevved::tests::CurrentManualArgv();
    rerevved::tests::ScopedLogging logging;
    const auto                     world = environment("REREVVED_WORLD_HARVEST3"), catalog = environment("REREVVED_WORLD_SCENE_SHADERS"), capture = environment("REREVVED_LEADER_CAPTURE");
    if (world.empty() || catalog.empty() || capture.empty())
    {
        SKIP("Set REREVVED_WORLD_HARVEST3, REREVVED_WORLD_SCENE_SHADERS and REREVVED_LEADER_CAPTURE to run the build-cost profile");
    }
    char* const* argv = args.argv();
    const int    argc = args.argc() + 1;
    harvest::costFrame(world, catalog, capture, argc > 1 ? argv[1] : "BF090116ED516AFB-0", argc > 2 ? std::stoi(argv[2]) : 5);
}

REREVVED_MANUAL_HELPER(sceneDrawAdapterIdentity, "scene-draw-adapter.identity", "scene_draw_adapter_identity")
#if REREVVED_DEVELOPER
REREVVED_MANUAL_HELPER(sceneDrawAdapterVsCode, "scene-draw-adapter.vs-code", "scene_draw_adapter_vs_code")
#endif
REREVVED_MANUAL_HELPER(sceneDrawAdapterBench, "scene-draw-adapter.bench", "scene_draw_adapter_bench")
REREVVED_MANUAL_HELPER(sceneDrawAdapterCost, "scene-draw-adapter.cost", "scene_draw_adapter_cost")
