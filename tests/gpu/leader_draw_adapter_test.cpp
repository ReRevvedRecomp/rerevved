#include "gpu/d3d12/native_renderer_d3d12.h"
#include "gpu/d3d12/native_texture_upload.h"
#include "gpu/guest_d3d/leader_draw_adapter.h"
#include "gpu/guest_d3d/native_draw_families.h"
#include "gpu/guest_d3d/scene_mesh_draw_adapter.h"
#include "logging_scope.hpp"
#include "resource_mirror_fixture.h"
#include "stream_queue_fixture.h"

#include <rex/crypto/sha256.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>

#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <toml++/toml.hpp>

using namespace rerevved::gpu;
using namespace rerevved::gpu::guest_d3d;

namespace
{

namespace xenos = rex::graphics::xenos;

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

std::uint32_t little(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) | (std::uint32_t(bytes[offset + 2]) << 16) |
           (std::uint32_t(bytes[offset + 3]) << 24);
}

std::string environment(const char* name)
{
    char       buffer[4096]{};
    const auto size = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return size && size < sizeof(buffer) ? std::string(buffer) : std::string();
}

// The capture records a texture's whole allocation; the reader reads its layout.
bool startsWith(const std::vector<std::uint8_t>& payload, std::span<const std::uint8_t> bytes)
{
    return bytes.size() <= payload.size() && std::equal(bytes.begin(), bytes.end(), payload.begin());
}

std::string sha256(std::span<const std::uint8_t> bytes)
{
    return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

// Guest memory as a set of regions. A read inside one region returns its
// bytes; any other read returns nothing, as an unmapped guest range does.
struct FakeMemory
{
    struct Region
    {
        std::uint64_t             address  = 0;
        bool                      physical = false;
        std::vector<std::uint8_t> bytes;
    };

    std::vector<Region>                                  regions;
    unsigned                                             reads = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> log; // (address, size) per read

    void Add(std::uint64_t address, bool physical, std::vector<std::uint8_t> bytes)
    {
        regions.push_back({ address, physical, std::move(bytes) });
    }

    std::vector<std::uint8_t> Read(std::uint64_t address, std::uint64_t size, bool physical)
    {
        ++reads;
        log.emplace_back(address, size);
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

NativeDrawSubmission submissionFor(const std::array<std::uint32_t, 8>& arguments, std::uint32_t caller = kLeaderDrawCaller)
{
    NativeDrawSubmission submission;
    submission.hook      = NativeDrawHook::IndexedBuffer;
    submission.caller    = caller;
    submission.stack     = 0x7018F930;
    submission.arguments = arguments;
    return submission;
}

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

// A linear, packed-mip fetch in the captured leader forms: linear filters,
// identity swizzle, TriJuice 3.
xenos::xe_gpu_texture_fetch_t leaderFetch(xenos::TextureFormat format, bool cube, std::uint32_t size, unsigned mipMaxLevel, std::uint32_t base, std::uint32_t mips)
{
    xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.type                = xenos::FetchConstantType::kTexture;
    fetch.clamp_x             = cube ? xenos::ClampMode::kClampToEdge : xenos::ClampMode::kRepeat;
    fetch.clamp_y             = cube ? xenos::ClampMode::kClampToEdge : xenos::ClampMode::kRepeat;
    fetch.clamp_z             = cube ? xenos::ClampMode::kClampToEdge : xenos::ClampMode::kRepeat;
    fetch.pitch               = size / 32;
    fetch.format              = format;
    fetch.endianness          = format == xenos::TextureFormat::k_8_8_8_8 ? xenos::Endian::k8in32 : xenos::Endian::k8in16;
    fetch.base_address        = base >> 12;
    fetch.size_2d.width       = size - 1;
    fetch.size_2d.height      = size - 1;
    fetch.size_2d.stack_depth = cube ? 5 : 0;
    fetch.swizzle             = 0x688;
    fetch.mag_filter          = xenos::TextureFilter::kLinear;
    fetch.min_filter          = xenos::TextureFilter::kLinear;
    fetch.mip_filter          = xenos::TextureFilter::kLinear;
    fetch.mip_max_level       = mipMaxLevel;
    fetch.tri_clamp           = 3;
    fetch.dimension           = cube ? xenos::DataDimension::kCube : xenos::DataDimension::k2DOrStacked;
    fetch.packed_mips         = mipMaxLevel ? 1 : 0;
    fetch.mip_address         = mips >> 12;
    return fetch;
}

// Six stream-0 fetch records, more than the boot families patch: each record
// names its instruction slot and its declaration semantic.
SolidDrawInputs sixFetches()
{
    SolidDrawInputs in;
    in.arguments.stride = 52;
    in.state.resize(0x3500);
    in.vertexCode.resize(7 * 12, 0xA5);
    in.vertexMetadata.resize(32);
    store(in.vertexMetadata, 28, 6);
    in.fetchTemplates.resize(6 * 12);
    in.fetchRecords.resize(6 * 4);
    in.declaration.resize(6 * 12);
    in.swizzleTable.resize(16);
    in.formatTable.resize(64);
    const std::array<std::array<std::uint32_t, 3>, 6> elements{ { { 0, 0x001A23B9, 0 }, { 12, 0x002C23A5, 5 }, { 20, 0x001A23B9, 3 }, { 32, 0x001A23B9, 6 }, { 44, 0x001A2086, 1 }, { 48, 0x001A2086, 2 } } };
    for (unsigned i = 0; i < 6; ++i)
    {
        store(in.fetchRecords, i * 4, (elements[i][2] << 12) | (i + 1));
        store(in.declaration, i * 12, elements[i][0]);
        store(in.declaration, i * 12 + 4, elements[i][1]);
        in.declaration[i * 12 + 9] = static_cast<std::uint8_t>(elements[i][2]);
    }
    return in;
}

void patchBeyondThree()
{
    std::string                            error;
    std::vector<std::uint8_t>              patched;
    constexpr std::array<std::uint32_t, 6> offsets{ 0, 12, 20, 32, 44, 48 };
    auto                                   in = sixFetches();
    UNSCOPED_INFO(std::string("six fetch records patch: ") + (error));
    REQUIRE(PatchVertexFetch(in, offsets, patched, error));
    // Each record's slot gets its declaration offset and stride / 4 (13).
    for (unsigned i = 0; i < 6; ++i)
    {
        UNSCOPED_INFO(std::format("record {} reaches slot {} with offset {}", i, i + 1, offsets[i]));
        REQUIRE((word(patched, (i + 1) * 12 + 8) & 0x7FFFFFFF) == ((offsets[i] << 6) | 13));
    }
    UNSCOPED_INFO("only the six designated slots change");
    REQUIRE(std::equal(patched.begin(), patched.begin() + 12, in.vertexCode.begin()));
    UNSCOPED_INFO("an unadmitted sixth offset is named");
    REQUIRE((!PatchVertexFetch(in, std::span(offsets).first(5), patched, error) && error == "adapter_attribute_offset"));
    // 0x826AD150 searches the whole declaration: an element no record fetches
    // leaves the patch unchanged, and a record without an element is named.
    UNSCOPED_INFO(std::string("six records patch again: ") + (error));
    REQUIRE(PatchVertexFetch(in, offsets, patched, error));
    const auto reference = patched;
    in.declaration.resize(7 * 12);
    store(in.declaration, 6 * 12, 64);
    store(in.declaration, 6 * 12 + 4, 0x002C23A5);
    in.declaration[6 * 12 + 9]  = 5;
    in.declaration[6 * 12 + 10] = 1;
    UNSCOPED_INFO(std::string("an unfetched seventh element leaves the six-record patch: ") + (error));
    REQUIRE((PatchVertexFetch(in, offsets, patched, error) && patched == reference));
    // Two unfetched elements ahead of the six put the last match at element 7.
    const auto unfetched = std::vector<std::uint8_t>(in.declaration.end() - 12, in.declaration.end());
    in.declaration.resize(6 * 12);
    for (unsigned i = 0; i < 2; ++i)
        in.declaration.insert(in.declaration.begin(), unfetched.begin(), unfetched.end());
    UNSCOPED_INFO(std::string("two leading unfetched elements leave the six-record patch: ") + (error));
    REQUIRE((PatchVertexFetch(in, offsets, patched, error) && patched == reference));
    in.declaration.erase(in.declaration.begin(), in.declaration.begin() + 24);
    in.declaration.resize(5 * 12);
    // 0x826AD228: the sixth record then fetches k_8_8_8_8 at offset 0 of fetch
    // constant 95; the other slots keep their patch.
    UNSCOPED_INFO(std::string("a record past a shorter declaration takes the unmatched fetch: ") + (error));
    REQUIRE((PatchVertexFetch(in, offsets, patched, error) && (word(patched, 72) >> 20 & 0x3FF) == 95 && (word(patched, 76) >> 16 & 63) == 6 &&
             word(patched, 80) == 13 && std::equal(patched.begin(), patched.begin() + 72, reference.begin())));
    in.declaration.resize(33 * 12);
    UNSCOPED_INFO(std::string("a declaration stays bounded by 32 elements: ") + (error));
    REQUIRE((!PatchVertexFetch(in, offsets, patched, error) && error == "adapter_patch_input_extent"));
    in = sixFetches();
    store(in.vertexMetadata, 28, 33);
    UNSCOPED_INFO("the record count stays bounded by 32");
    REQUIRE((!PatchVertexFetch(in, offsets, patched, error) && error == "adapter_fetch_count"));
    std::cout << "The fetch patch admits six records among five to eight elements and keeps its offset, extent and count refusals\n";
}

// Hand-built shader microcode exposes the slots one exec pair runs.
void fetchLayout()
{
    const auto code = [](const std::vector<std::array<std::uint32_t, 3>>& fetches)
    {
        std::vector<std::uint8_t> bytes((fetches.size() + 1) * 12);
        std::uint32_t             sequence = 0;
        for (std::size_t i = 0; i < fetches.size(); ++i)
            sequence |= 1U << (i * 2);
        put(bytes, 0, 1 | std::uint32_t(fetches.size()) << 12 | sequence << 16);
        put(bytes, 4, 0x1000);
        for (std::size_t i = 0; i < fetches.size(); ++i)
            for (std::size_t j = 0; j < 3; ++j)
                put(bytes, (i + 1) * 12 + j * 4, fetches[i][j]);
        return bytes;
    };
    // Fetch constant 95 (index 31, select 2); floats are signed and not normalized.
    constexpr std::uint32_t            kFetch95 = 31U << 20 | 2U << 25, kFloat = 1U << 12 | 1U << 13, kMini = 1U << 30;
    const std::array<std::uint32_t, 3> full{ kFetch95, 57U << 16 | kFloat, 13 }, mini{ 0, 37U << 16 | kFloat | kMini, 3U << 8 };
    NativeGameplayShaderInterface      shader;
    shader.vertexAttributes  = { { 95, 52, 0, 57, true, false }, { 95, 52, 12, 37, true, false } };
    shader.textureFetchSlots = { 2 };
    shader.textureDimensions = { NativeDrawReplayTextureDimension::TextureCube };
    // A cube fetch from constant 2, after a set-texture-LOD (24) that binds nothing.
    const auto ps = code({ { 24U | 5U << 20, 0, 0 }, { 1U | 2U << 20, 0, 3U << 14 } });
    UNSCOPED_INFO("a mini fetch takes the previous full fetch's constant and stride");
    REQUIRE(MatchShaderFetches(code({ full, mini }), ps, shader) == ShaderFetchMatch::Matched);
    UNSCOPED_INFO("a mini fetch without a full fetch is a mismatch");
    REQUIRE(MatchShaderFetches(code({ mini, full }), ps, shader) == ShaderFetchMatch::VertexLayout);
    auto adjusted = full;
    adjusted[1] |= 1U << 24;
    UNSCOPED_INFO("an exponent adjust is a mismatch");
    REQUIRE(MatchShaderFetches(code({ adjusted, mini }), ps, shader) == ShaderFetchMatch::VertexLayout);
    UNSCOPED_INFO("a 2D fetch of a cube slot is a mismatch");
    REQUIRE(MatchShaderFetches(code({ full, mini }), code({ { 1U | 2U << 20, 0, 1U << 14 } }), shader) == ShaderFetchMatch::TextureLayout);
    UNSCOPED_INFO("a catalog slot the PS never samples is a mismatch");
    REQUIRE(MatchShaderFetches(code({ full, mini }), code({}), shader) == ShaderFetchMatch::TextureLayout);
    // A normalized 2D texture fetch of constant 16 in the VS binds slot 16.
    const std::array<std::uint32_t, 3> vertexTexture{ 1U | 16U << 20, 0, 1U << 14 };
    UNSCOPED_INFO("a VS slot the catalog lacks is a mismatch");
    REQUIRE(MatchShaderFetches(code({ full, vertexTexture, mini }), ps, shader) == ShaderFetchMatch::TextureLayout);
    auto sampled              = shader;
    sampled.textureFetchSlots = { 2, 16 };
    sampled.textureDimensions = { NativeDrawReplayTextureDimension::TextureCube, NativeDrawReplayTextureDimension::Texture2D };
    UNSCOPED_INFO("a VS texture fetch binds its slot");
    REQUIRE(MatchShaderFetches(code({ full, vertexTexture, mini }), ps, sampled) == ShaderFetchMatch::Matched);
    auto lod = vertexTexture;
    lod[2] |= 1U << 2;
    UNSCOPED_INFO("a VS fetch with a LOD bias is a mismatch");
    REQUIRE(MatchShaderFetches(code({ full, lod, mini }), ps, sampled) == ShaderFetchMatch::TextureLayout);
    std::vector<NativeGameplayVertexAttribute> attributes;
    UNSCOPED_INFO("the attribute walk alone refuses a VS texture fetch");
    REQUIRE(!DecodeShaderVertexFetches(code({ full, vertexTexture, mini }), attributes));
    std::cout << "The live fetch walk: mini fetches, exponent adjusts, texture dimensions, unsampled slots and VS texture fetches\n";
}

void sampler()
{
    std::string             error;
    NativeDrawReplaySampler decoded;
    const auto              flat = leaderFetch(xenos::TextureFormat::k_DXT1, false, 128, 7, 0x01000000, 0x01100000);
    const auto              cube = leaderFetch(xenos::TextureFormat::k_8_8_8_8, true, 128, 7, 0x01200000, 0x01300000);
    UNSCOPED_INFO(std::string("a mipmapped 2D fetch keeps trilinear filtering over every level with repeat addressing: ") + (error));
    REQUIRE((DecodeLeaderSampler(fetchWords(flat), NativeDrawReplayTextureDimension::Texture2D, decoded, error) && decoded.magLinear && decoded.minLinear &&
             decoded.mipLinear && decoded.anisotropy == 0 && decoded.address == std::array<std::uint32_t, 3>{ 1, 1, 3 } && decoded.minLod == 0 &&
             decoded.maxLod == std::numeric_limits<float>::max() && decoded.mipBias == 0));
    UNSCOPED_INFO(std::string("a mipmapped cube fetch decodes with clamp addressing: ") + (error));
    REQUIRE((DecodeLeaderSampler(fetchWords(cube), NativeDrawReplayTextureDimension::TextureCube, decoded, error) && decoded.mipLinear &&
             decoded.address == std::array<std::uint32_t, 3>{ 3, 3, 3 }));
    auto point       = flat;
    point.mag_filter = point.min_filter = xenos::TextureFilter::kPoint;
    point.mip_filter                    = xenos::TextureFilter::kPoint;
    point.clamp_x                       = xenos::ClampMode::kMirroredRepeat;
    point.clamp_y                       = xenos::ClampMode::kMirrorClampToEdge;
    UNSCOPED_INFO(std::string("point filters and the mirror modes map: ") + (error));
    REQUIRE((DecodeLeaderSampler(fetchWords(point), NativeDrawReplayTextureDimension::Texture2D, decoded, error) && !decoded.magLinear && !decoded.minLinear &&
             !decoded.mipLinear && decoded.address == std::array<std::uint32_t, 3>{ 2, 5, 3 }));
    auto baseMap       = flat;
    baseMap.mip_filter = xenos::TextureFilter::kBaseMap;
    UNSCOPED_INFO(std::string("the base-map mip filter samples level 0 only: ") + (error));
    REQUIRE((DecodeLeaderSampler(fetchWords(baseMap), NativeDrawReplayTextureDimension::Texture2D, decoded, error) && decoded.maxLod == 0));
    // The SDK's anisotropic_override reaches mipped linear fetches only.
    UNSCOPED_INFO(std::string("an override of 3 samples a mipped linear fetch at 4x: ") + (error));
    REQUIRE((DecodeLeaderSampler(fetchWords(flat), NativeDrawReplayTextureDimension::Texture2D, decoded, error, 3) && decoded.anisotropy == 3 && decoded.mipLinear));
    UNSCOPED_INFO(std::string("the override reaches a mipped cube: ") + (error));
    REQUIRE((DecodeLeaderSampler(fetchWords(cube), NativeDrawReplayTextureDimension::TextureCube, decoded, error, 5) && decoded.anisotropy == 5));
    auto pointMip       = flat;
    pointMip.mip_filter = xenos::TextureFilter::kPoint;
    UNSCOPED_INFO(std::string("a point mip filter is eligible and becomes linear under anisotropy: ") + (error));
    REQUIRE((DecodeLeaderSampler(fetchWords(pointMip), NativeDrawReplayTextureDimension::Texture2D, decoded, error, 3) && decoded.anisotropy == 3 && decoded.mipLinear));
    const auto single = leaderFetch(xenos::TextureFormat::k_DXT1, false, 128, 0, 0x01000000, 0);
    for (const auto& [name, fetch, value] : std::initializer_list<std::tuple<const char*, xenos::xe_gpu_texture_fetch_t, std::int32_t>>{
             { "one level", single, 3 }, { "base map", baseMap, 3 }, { "point filters", point, 3 }, { "no override", flat, -1 }, { "override 0", flat, 0 }, { "override 6", flat, 6 } })
    {
        UNSCOPED_INFO(std::string(std::string(name) + " keeps the fetch's filters: ") + (error));
        REQUIRE((DecodeLeaderSampler(fetchWords(fetch), NativeDrawReplayTextureDimension::Texture2D, decoded, error, value) && decoded.anisotropy == 0));
    }

    const auto refused = [&](const std::string& name, auto edit, const std::string& expected, NativeDrawReplayTextureDimension dimension = NativeDrawReplayTextureDimension::Texture2D)
    {
        auto fetch = dimension == NativeDrawReplayTextureDimension::TextureCube ? cube : flat;
        edit(fetch);
        const bool admitted = DecodeLeaderSampler(fetchWords(fetch), dimension, decoded, error);
        UNSCOPED_INFO(name + ": " + (admitted ? "admitted" : error));
        REQUIRE((!admitted && error == expected));
    };
    using Fetch = xenos::xe_gpu_texture_fetch_t;
    refused("a cube for a 2D slot", [](Fetch& f)
            {
                f.dimension = xenos::DataDimension::kCube;
            },
            "leader_sampler_dimension");
    refused("a 2D fetch for a cube slot", [](Fetch& f)
            {
                f.dimension = xenos::DataDimension::k2DOrStacked;
            },
            "leader_sampler_dimension",
            NativeDrawReplayTextureDimension::TextureCube);
    refused("a 3D fetch", [](Fetch& f)
            {
                f.dimension = xenos::DataDimension::k3D;
            },
            "leader_sampler_dimension");
    refused("a stacked 2D fetch", [](Fetch& f)
            {
                f.stacked = 1;
            },
            "leader_sampler_dimension");
    refused("a vertex fetch", [](Fetch& f)
            {
                f.type = xenos::FetchConstantType::kVertex;
            },
            "leader_sampler_dimension");
    refused("a fetch-constant mag filter", [](Fetch& f)
            {
                f.mag_filter = xenos::TextureFilter::kUseFetchConst;
            },
            "leader_sampler_filter");
    refused("a base-map min filter", [](Fetch& f)
            {
                f.min_filter = xenos::TextureFilter::kBaseMap;
            },
            "leader_sampler_filter");
    refused("a fetch-constant mip filter", [](Fetch& f)
            {
                f.mip_filter = xenos::TextureFilter::kUseFetchConst;
            },
            "leader_sampler_mip_filter");
    refused("anisotropy", [](Fetch& f)
            {
                f.aniso_filter = xenos::AnisoFilter::kMax_2_1;
            },
            "leader_sampler_anisotropy");
    refused("a LOD bias", [](Fetch& f)
            {
                f.lod_bias = 1;
            },
            "leader_sampler_lod_bias");
    refused("a horizontal gradient adjust", [](Fetch& f)
            {
                f.grad_exp_adjust_h = 1;
            },
            "leader_sampler_gradient_adjust");
    refused("a vertical gradient adjust", [](Fetch& f)
            {
                f.grad_exp_adjust_v = -1;
            },
            "leader_sampler_gradient_adjust");
    refused("a border texel", [](Fetch& f)
            {
                f.border_size = 1;
            },
            "leader_sampler_border_size");
    refused("the nearest clamp policy", [](Fetch& f)
            {
                f.nearest_clamp_policy = 1;
            },
            "leader_sampler_nearest_clamp_policy");
    refused("clamp to halfway on X", [](Fetch& f)
            {
                f.clamp_x = xenos::ClampMode::kClampToHalfway;
            },
            "leader_sampler_clamp_mode");
    refused("clamp to border on Y", [](Fetch& f)
            {
                f.clamp_y = xenos::ClampMode::kClampToBorder;
            },
            "leader_sampler_clamp_mode");
    refused("a minimum level", [](Fetch& f)
            {
                f.mip_min_level = 1;
            },
            "leader_sampler_min_level");
    std::cout << "The leader sampler admits point or linear 2D and cube fetches with mips; each other form is named\n";
}

// Texture bytes for `fetch`: a byte pattern over its base and mip spans.
std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> textureBytes(const std::array<std::uint32_t, 6>& fetch, std::uint8_t seed)
{
    NativeTextureMemoryRanges ranges;
    std::string               error;
    UNSCOPED_INFO(std::string("synthetic texture ranges: ") + (error));
    REQUIRE(GetNativeTextureMemoryRanges(fetch, ranges, error));
    std::vector<std::uint8_t> base(ranges.base.size), mips(ranges.mips.size);
    for (std::size_t i = 0; i < base.size(); ++i)
        base[i] = static_cast<std::uint8_t>(i * 7 + seed);
    for (std::size_t i = 0; i < mips.size(); ++i)
        mips[i] = static_cast<std::uint8_t>(i * 5 + seed);
    return { base, mips };
}

void cache()
{
    std::string             error;
    LeaderTextureCache      textures;
    NativeDrawReplayTexture first, again;
    const auto              fetch = fetchWords(leaderFetch(xenos::TextureFormat::k_DXT1, false, 128, 7, 0x01000000, 0x01100000));
    const auto              bytes = textureBytes(fetch, 3);
    const auto&             base  = bytes.first;
    const auto&             mips  = bytes.second;
    UNSCOPED_INFO(std::string("a packed-mip BC1 texture decodes all eight levels: ") + (error));
    REQUIRE((textures.Decode(fetch, base, mips, first, error) && first.mipLevels == 8 && first.format == NativeDrawReplayTextureFormat::Bc1 && first.textureVersion));
    UNSCOPED_INFO("the same fetch and bytes reuse the decoded version");
    REQUIRE((textures.Decode(fetch, base, mips, again, error) && again.textureVersion == first.textureVersion));
    // Equal bytes in another retained buffer, as the read mirror returns after
    // a refresh: the hit keeps that buffer, so its next read shares it.
    const auto heldBase = std::make_shared<const std::vector<std::uint8_t>>(base);
    const auto heldMips = std::make_shared<const std::vector<std::uint8_t>>(mips);
    const bool hit      = textures.Decode(fetch, GuestBytes::Shared(heldBase), GuestBytes::Shared(heldMips), again, error);
    UNSCOPED_INFO("an equal hit in another retained buffer keeps that buffer");
    REQUIRE((hit && again.textureVersion == first.textureVersion && heldBase.use_count() == 2 && heldMips.use_count() == 2));
    // Other base bytes are other textures.
    const auto decode = [&](LeaderTextureCache& cache, std::uint32_t i)
    {
        auto other = base;
        other[0] ^= static_cast<std::uint8_t>(i + 1);
        other[1] ^= static_cast<std::uint8_t>((i + 1) >> 8);
        NativeDrawReplayTexture texture;
        UNSCOPED_INFO(std::string("a decode: ") + (error));
        REQUIRE((cache.Decode(fetch, other, mips, texture, error) && texture.textureVersion));
        return texture.textureVersion;
    };
    // A world-sized working set, more than the former 32 entries, stays
    // resident: its second frame decodes nothing, so nothing uploads again.
    constexpr std::uint32_t                                            kWorkingSet = 160;
    std::vector<std::shared_ptr<const NativeDrawReplayTextureVersion>> frame;
    for (std::uint32_t i = 0; i < kWorkingSet; ++i)
        frame.push_back(decode(textures, i));
    const auto [entryBytes, kept] = textures.Retained();
    UNSCOPED_INFO(std::format("{} textures are kept within the budget", kept));
    REQUIRE((kept == kWorkingSet + 1 && entryBytes <= LeaderTextureCache::kBudgetBytes));
    for (std::uint32_t i = 0; i < kWorkingSet; ++i)
    {
        UNSCOPED_INFO(std::format("texture {} reuses its version in the next frame", i));
        REQUIRE(decode(textures, i) == frame[i]);
    }
    UNSCOPED_INFO("the first texture is still kept");
    REQUIRE((textures.Decode(fetch, base, mips, again, error) && again.textureVersion == first.textureVersion));

    // Over the budget the least recently used goes first.
    const auto         one = entryBytes / (kWorkingSet + 1);
    LeaderTextureCache bounded(one * 3);
    const auto         a = decode(bounded, 0), b = decode(bounded, 1), c = decode(bounded, 2);
    UNSCOPED_INFO("three textures fill the budget");
    REQUIRE((decode(bounded, 0) == a && bounded.Retained() == std::pair{ one * 3, std::size_t(3) }));
    const auto d = decode(bounded, 3);
    UNSCOPED_INFO("a fourth keeps the three most recently used");
    REQUIRE((bounded.Retained().second == 3 && decode(bounded, 0) == a && decode(bounded, 2) == c && decode(bounded, 3) == d));
    UNSCOPED_INFO("the least recently used texture was evicted");
    REQUIRE(decode(bounded, 1) != b);
    LeaderTextureCache tiny(one - 1);
    UNSCOPED_INFO("a texture over the whole budget decodes but is not kept");
    REQUIRE((decode(tiny, 0) && tiny.Retained() == std::pair{ std::size_t(0), std::size_t(0) }));
    std::cout << "The leader texture cache reuses versions by fetch and bytes and evicts the least recently used\n";
}

std::string dxilDigest(const std::vector<std::uint8_t>& bytes)
{
    return sha256(bytes);
}

// One catalog entry as shaders.toml text; the word digests repeat one hex
// character.
struct TextPair
{
    std::uint64_t                                        vertexHash = 0, pixelHash = 0;
    char                                                 vertexWords = 0, pixelWords = 0;
    std::uint32_t                                        program = 0, context = 0, interpolators = 0, stride = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> attributes; // byte offset, format
    std::vector<std::string>                             textures;
};

// The civ select body and glow, then another leader's material: seven fetches
// at stride 72 over a 2D texture and a cube.
std::vector<TextPair> textPairs()
{
    return {
        { 0x7808944337B1C1C3ULL, 0x85222C88DE4F1473ULL, 'a', 'b', 0x20510C12, 4, 0x3F003F, 52, { { 0, 57 }, { 12, 37 }, { 20, 57 }, { 32, 57 }, { 44, 6 }, { 48, 6 } }, { "2d", "cube", "2d", "2d", "2d", "cube", "2d", "cube", "cube", "cube" } },
        { 0xC1CAAFCB5EB76397ULL, 0x24F4430C7DDCA67FULL, 'c', 'd', 0x3081080B, 8, 0xFF01FF, 68, { { 0, 57 }, { 12, 57 }, { 24, 57 }, { 36, 57 }, { 48, 6 }, { 52, 6 }, { 56, 6 }, { 60, 37 } }, { "2d" } },
        { 0x68F5B5F29EC06AEDULL, 0x832DEA08487412B7ULL, 'e', 'f', 0x20510709, 4, 0x3F003F, 72, { { 0, 57 }, { 12, 37 }, { 20, 57 }, { 32, 57 }, { 44, 57 }, { 56, 6 }, { 60, 6 } }, { "2d", "cube" } },
    };
}

// Loads `pairs` as a catalog directory; `from`/`to` edits the text once.
std::string loadCatalog(const std::string& name, LeaderDrawCatalog& catalog, const std::string& from = {}, const std::string& to = {}, const std::vector<TextPair>& pairs = textPairs())
{
    const auto directory = std::filesystem::temp_directory_path() / ("leader-catalog-" + name);
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const std::vector<std::uint8_t> vs{ 'D', 'X', 'B', 'C' }, ps{ 'D', 'X', 'B', 'P' };
    std::ofstream(directory / "vs.dxil", std::ios::binary).write(reinterpret_cast<const char*>(vs.data()), vs.size());
    std::ofstream(directory / "ps.dxil", std::ios::binary).write(reinterpret_cast<const char*>(ps.data()), ps.size());
    std::string text = "schema_version = 1\n";
    for (const auto& pair : pairs)
    {
        std::string dimensions, slots;
        for (std::size_t i = 0; i < pair.textures.size(); ++i)
        {
            dimensions += std::format("{}\"{}\"", i ? ", " : "", pair.textures[i]);
            slots += std::format("{}{}", i ? ", " : "", i);
        }
        text += std::format("\n[[shaders]]\nvertex_hash = \"{:016X}\"\npixel_hash = \"{:016X}\"\nvertex_word_sha256 = \"{}\"\npixel_word_sha256 = \"{}\"\n"
                            "vertex_dxil_sha256 = \"{}\"\npixel_dxil_sha256 = \"{}\"\nvertex_file = \"vs.dxil\"\npixel_file = \"ps.dxil\"\n"
                            "alpha_test = true\nalpha_compare = 4\nrasterized_depth = true\nindependent_samplers = true\n"
                            "program = {}\ncontext = {}\ninterpolators = {}\ntexture_fetches = [{}]\ntexture_dimensions = [{}]\n",
                            pair.vertexHash,
                            pair.pixelHash,
                            std::string(64, pair.vertexWords),
                            std::string(64, pair.pixelWords),
                            dxilDigest(vs),
                            dxilDigest(ps),
                            pair.program,
                            pair.context,
                            pair.interpolators,
                            slots,
                            dimensions);
        for (const auto [offset, format] : pair.attributes)
            text += std::format("[[shaders.attributes]]\nfetch = 95\nstride_bytes = {}\noffset_bytes = {}\nformat = {}\nsigned = {}\nnormalized = {}\n",
                                pair.stride,
                                offset,
                                format,
                                format == 6 ? "false" : "true",
                                format == 6 ? "true" : "false");
    }
    if (!from.empty())
    {
        const auto at = text.find(from);
        UNSCOPED_INFO("catalog text found for " + name);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
    }
    std::ofstream(directory / "shaders.toml", std::ios::binary) << text;
    std::string error;
    const bool  loaded = LoadLeaderDrawCatalog(directory, catalog, error);
    std::filesystem::remove_all(directory);
    return loaded ? std::string("loaded") : error;
}

void catalog()
{
    LeaderDrawCatalog loaded;
    UNSCOPED_INFO("three leader pairs load: " + loadCatalog("three", loaded));
    REQUIRE(loadCatalog("three", loaded) == "loaded");
    const auto  shaders = LeaderPixelShaders(loaded);
    const auto& second  = loaded.entries.back();
    UNSCOPED_INFO("each pair keeps its own stride, attribute offsets, texture set and program words");
    REQUIRE((loaded.entries.size() == 3 && loaded.entries[0].stride == 52 && loaded.entries[1].stride == 68 && second.stride == 72 &&
             loaded.entries[0].attributeOffsets == std::vector<std::uint32_t>{ 0, 12, 20, 32, 44, 48 } &&
             second.attributeOffsets == std::vector<std::uint32_t>{ 0, 12, 20, 32, 44, 56, 60 } && loaded.entries[0].shader.textureDimensions.size() == 10 &&
             second.shader.textureDimensions == std::vector<NativeDrawReplayTextureDimension>{ NativeDrawReplayTextureDimension::Texture2D, NativeDrawReplayTextureDimension::TextureCube } &&
             second.shader.shaderProgram == 0x20510709));
    UNSCOPED_INFO("the reader list and lookup follow the catalog's pixel word digests");
    REQUIRE((shaders.size() == 3 && shaders[0].wordDigest == std::string(64, 'b') && shaders[0].textureFetches == 10 && shaders[1].textureFetches == 1 &&
             shaders[2].wordDigest == std::string(64, 'f') && shaders[2].textureFetches == 2 && loaded.Find(std::string(64, 'f')) == &second &&
             !loaded.Find(std::string(64, 'a'))));
    auto duplicate = textPairs();
    duplicate.push_back(duplicate[0]);
    duplicate.back().vertexHash ^= 1;
    const auto twice = loadCatalog("duplicate", loaded, {}, {}, duplicate);
    UNSCOPED_INFO("a second entry for one pixel shader is named: " + twice);
    REQUIRE(twice == "leader_catalog_duplicate_pixel");
    for (const auto& [name, from, to, expected] : std::vector<std::array<std::string, 4>>{
             { "glow-slot-1", "texture_fetches = [0]\n", "texture_fetches = [1]\n", "leader_catalog_textures" },
             { "second-slots-swapped", "texture_fetches = [0, 1]\n", "texture_fetches = [1, 0]\n", "leader_catalog_textures" },
             { "no-alpha-test", "alpha_test = true", "alpha_test = false", "leader_catalog_interface" },
             { "alpha-less", "alpha_compare = 4", "alpha_compare = 1", "leader_catalog_interface" },
             { "screen-space", "rasterized_depth = true\n", "rasterized_depth = true\nscreen_space_position = true\n", "leader_catalog_interface" },
             { "two-targets", "rasterized_depth = true\n", "rasterized_depth = true\ncolor_targets = 2\ncolor1_format = 2\n", "leader_catalog_color_targets" },
             { "stride-48", "stride_bytes = 52\noffset_bytes = 12", "stride_bytes = 48\noffset_bytes = 12", "leader_catalog_layout" },
             { "uv-at-14", "stride_bytes = 52\noffset_bytes = 12", "stride_bytes = 52\noffset_bytes = 14", "leader_catalog_layout" },
             { "negative-offset", "stride_bytes = 52\noffset_bytes = 0", "stride_bytes = 52\noffset_bytes = -4", "leader_catalog_layout" },
             { "bytes-past-stride", "stride_bytes = 52\noffset_bytes = 48", "stride_bytes = 52\noffset_bytes = 52", "leader_catalog_layout" },
             { "float2-past-stride", "stride_bytes = 68\noffset_bytes = 60", "stride_bytes = 68\noffset_bytes = 64", "leader_catalog_layout" },
             { "float3-past-stride", "stride_bytes = 72\noffset_bytes = 44", "stride_bytes = 72\noffset_bytes = 64", "leader_catalog_layout" },
             { "float4", "offset_bytes = 20\nformat = 57", "offset_bytes = 20\nformat = 38", "leader_catalog_layout" },
             { "fetch-94", "fetch = 95\nstride_bytes = 68\noffset_bytes = 60", "fetch = 94\nstride_bytes = 68\noffset_bytes = 60", "leader_catalog_layout" },
             { "unsigned-float", "offset_bytes = 0\nformat = 57\nsigned = true", "offset_bytes = 0\nformat = 57\nsigned = false", "leader_catalog_layout" },
             { "normalized-float", "offset_bytes = 12\nformat = 37\nsigned = true\nnormalized = false", "offset_bytes = 12\nformat = 37\nsigned = true\nnormalized = true", "leader_catalog_layout" },
             { "signed-bytes", "offset_bytes = 44\nformat = 6\nsigned = false", "offset_bytes = 44\nformat = 6\nsigned = true", "leader_catalog_layout" },
             { "raw-bytes", "offset_bytes = 48\nformat = 6\nsigned = false\nnormalized = true", "offset_bytes = 48\nformat = 6\nsigned = false\nnormalized = false", "leader_catalog_layout" },
         })
    {
        const auto reason = loadCatalog(name, loaded, from, to);
        UNSCOPED_INFO(name + ": " + reason);
        REQUIRE((reason == expected && loaded.entries.empty()));
    }
    std::cout << "Leader catalog admits every entry with its own interface; duplicate, texture slot, interface, target and layout refusals are named\n";
}

// Synthetic leader draws over synthetic shader words: a body-shaped pair and
// a second leader's material, in the harvested forms.
namespace synthetic
{

constexpr std::uint32_t kG = 0x50000000, kVs = 0x40010000, kPs = 0x40020000, kDeclaration = 0x40030000, kIndexBuffer = 0x40040000;
// Physical: VS and PS resource bases (literal values at +0x400), indices,
// vertices and the texture spans.
constexpr std::uint32_t kVsResource = 0x00100000, kPsResource = 0x00200000, kIndices = 0x00300000, kVertices = 0x00400000;
constexpr std::uint32_t kFlat = 0x01000000, kFlatMips = 0x01100000, kCube = 0x01200000, kCubeMips = 0x01300000, kDetail = 0x01400000;
constexpr std::uint32_t kLiteralOffset = 0x400, kAlphaReference = 0x3F48C8CA, kVertexCount = 8, kIndexCount = 12;
// Declaration types: k_32_32_32_FLOAT, k_32_32_FLOAT and k_8_8_8_8.
constexpr std::uint32_t kFloat3 = 0x001A23B9, kFloat2 = 0x002C23A5, kBytes = 0x001A2086;

// One declaration element: byte offset, type, usage and usage index, the
// translated format, and whether a VS fetch record names it.
struct Element
{
    std::uint32_t offset = 0, type = 0, usage = 0, index = 0, format = 0;
    bool          fetched = true;
};

// A synthetic pair: its PS words, program words, stream layout, sampled
// textures and per-pair state.
struct Form
{
    std::uint32_t                                 psWord = 0, program = 0, context = 0, interpolators = 0, stride = 0;
    std::uint32_t                                 depth = 0, blend = 0, raster = 0;
    std::vector<Element>                          elements;
    std::vector<std::array<std::uint32_t, 6>>     textures;
    std::vector<NativeDrawReplayTextureDimension> dimensions;
};

// The body's form: six fetches, a mipmapped BC1 2D and a mipmapped RGBA8
// cube, LessEqual depth write, source-alpha blend, back-face culling.
Form bodyForm()
{
    Form form;
    form.psWord        = 0x5678;
    form.program       = 0x20510C12;
    form.context       = 4;
    form.interpolators = 0x3F003F;
    form.stride        = 52;
    form.depth         = 0x24F00736;
    form.blend         = 0x00010706;
    form.raster        = 0x18002;
    form.elements      = { { 0, kFloat3, 0, 0, 57 }, { 12, kFloat2, 5, 0, 37 }, { 20, kFloat3, 3, 0, 57 }, { 32, kFloat3, 6, 0, 57 }, { 44, kBytes, 1, 0, 6 }, { 48, kBytes, 2, 0, 6 } };
    form.textures      = { fetchWords(leaderFetch(xenos::TextureFormat::k_DXT1, false, 128, 7, kFlat, kFlatMips)),
                           fetchWords(leaderFetch(xenos::TextureFormat::k_8_8_8_8, true, 128, 7, kCube, kCubeMips)) };
    form.dimensions    = { NativeDrawReplayTextureDimension::Texture2D, NativeDrawReplayTextureDimension::TextureCube };
    return form;
}

// Another leader's material: seven fetches over an eight-element declaration
// at stride 72, the element at 32 fetched before the one at 20, its own program words, a BC2 2D, the cube and a one-level BC1
// detail map, source-alpha color and alpha blend, no culling.
Form secondForm()
{
    Form form;
    form.psWord        = 0x9ABC;
    form.program       = 0x20510709;
    form.context       = 4;
    form.interpolators = 0x3F003F;
    form.stride        = 72;
    form.depth         = 0x24F00736;
    form.blend         = 0x07060706;
    form.raster        = 0x18000;
    form.elements      = { { 0, kFloat3, 0, 0, 57 }, { 12, kFloat2, 5, 0, 37 }, { 32, kFloat3, 7, 0, 57 }, { 20, kFloat3, 3, 0, 57 }, { 44, kFloat3, 6, 0, 57 }, { 56, kBytes, 1, 0, 6 }, { 60, kBytes, 2, 0, 6 }, { 64, kFloat2, 5, 1, 37, false } };
    form.textures      = { fetchWords(leaderFetch(xenos::TextureFormat::k_DXT2_3, false, 128, 7, kFlat, kFlatMips)),
                           fetchWords(leaderFetch(xenos::TextureFormat::k_8_8_8_8, true, 128, 7, kCube, kCubeMips)),
                           fetchWords(leaderFetch(xenos::TextureFormat::k_DXT1, false, 64, 0, kDetail, 0)) };
    form.dimensions    = { NativeDrawReplayTextureDimension::Texture2D, NativeDrawReplayTextureDimension::TextureCube, NativeDrawReplayTextureDimension::Texture2D };
    return form;
}

std::uint32_t literal(std::uint32_t i, bool pixel)
{
    return (pixel ? 0x41000000U : 0x40000000U) + i * 0x00010003U;
}

// A control-flow pair whose exec clauses run fetch slots 1..fetches, at most
// six per clause, followed by those slots.
std::vector<std::uint8_t> execCode(std::uint32_t fetches, std::uint32_t last = 0)
{
    std::vector<std::uint8_t> code((fetches + 1) * 12);
    const auto                first = std::min(fetches, 6U), rest = fetches - first;
    const auto                sequence = [](std::uint32_t n)
    {
        std::uint32_t bits = 0;
        for (std::uint32_t i = 0; i < n; ++i)
            bits |= 1U << (i * 2);
        return bits;
    };
    put(code, 0, 1 | first << 12 | sequence(first) << 16);
    put(code, 4, 0x1000 | (rest ? (1 + first) << 16 | rest << 28 : 0));
    put(code, 8, rest ? 0x10000000 | sequence(rest) : last);
    return code;
}

// The PS: one texture fetch per form texture, from fetch constant i with the
// form's dimension, after a pair whose last word makes the digest the form's.
std::vector<std::uint8_t> psCode(const Form& form)
{
    auto code = execCode(static_cast<std::uint32_t>(form.dimensions.size()), form.psWord);
    for (std::uint32_t i = 0; i < form.dimensions.size(); ++i)
    {
        put(code, (i + 1) * 12, 1 | i << 20);
        put(code, (i + 1) * 12 + 8, (form.dimensions[i] == NativeDrawReplayTextureDimension::TextureCube ? 3U : 1U) << 14);
    }
    return code;
}

// The harvested target forms: window scissor bottom right, viewport Y scale
// and viewport Y offset. Civ select draws the 1280x351 band; the world draws
// the same pairs over the whole 1280x720 window.
constexpr std::array<std::array<std::uint32_t, 3>, 2> kTargetForms{ { { 0x015F0500, 0xC32F8000, 0x432F8000 }, { 0x02D00500, 0xC3B40000, 0x43B40000 } } };
constexpr std::array<std::uint32_t, 3>                kTargetRegisters{ 0x2082, 0x2111, 0x2112 };

struct Options
{
    Form          form             = bodyForm();
    std::uint32_t pixelRecordFirst = 508;
    std::size_t   target           = 0;
};

Options second()
{
    Options options;
    options.form = secondForm();
    return options;
}

std::vector<Element> fetched(const Form& form)
{
    std::vector<Element> result;
    for (const auto& element : form.elements)
        if (element.fetched)
            result.push_back(element);
    return result;
}

void literalTable(FakeMemory& memory, std::uint64_t table, std::uint32_t first, std::uint32_t resource, bool pixel)
{
    std::vector<std::uint8_t> bytes(20);
    put(bytes, 16, 8);
    put(bytes, 20, (first << 16) | 16);
    put(bytes, 24, kLiteralOffset);
    memory.Add(table, false, bytes);
    std::vector<std::uint8_t> values;
    for (std::uint32_t i = 0; i < 16; ++i)
        put(values, i * 4, literal(i, pixel));
    memory.Add(resource + kLiteralOffset, true, values);
}

FakeMemory memoryFor(const Options& options = {})
{
    const auto& form    = options.form;
    const auto  records = fetched(form);
    FakeMemory  memory;
    // VS code: EXECs from address 1 bound the control flow and run one fetch
    // slot per record for the patch to fill.
    const auto vsCode = execCode(static_cast<std::uint32_t>(records.size()));
    const auto ps     = psCode(form);
    memory.Add(kVsResource, true, vsCode);
    memory.Add(kPsResource, true, ps);

    // VS object: header to 912, metadata at +0x100 + 872, literal table at
    // +0x200 + 872 (header words 896 and 892).
    std::vector<std::uint8_t> vsHeader(912);
    put(vsHeader, 32, kVsResource);
    put(vsHeader, 892, 0x200);
    put(vsHeader, 896, 0x100);
    memory.Add(kVs, false, vsHeader);
    std::vector<std::uint8_t> vsMetadata(36 + records.size() * 4);
    put(vsMetadata, 4, static_cast<std::uint32_t>(vsCode.size()));
    put(vsMetadata, 8, form.program);  // SQ_PROGRAM_CNTL
    put(vsMetadata, 12, form.context); // SQ_CONTEXT_MISC
    put(vsMetadata, 28, static_cast<std::uint32_t>(records.size()));
    for (std::uint32_t i = 0; i < records.size(); ++i)
        put(vsMetadata, 36 + i * 4, (records[i].index << 16) | (records[i].usage << 12) | (i + 1));
    memory.Add(kVs + 0x100 + 872, false, vsMetadata);
    literalTable(memory, kVs + 0x200 + 872, 252, kVsResource, false);

    // PS object: header to 68 (metadata at +0x100 + 40, literal table at
    // +0x200 + 40).
    std::vector<std::uint8_t> psHeader(68), psMetadata(32);
    put(psHeader, 24, kPsResource);
    put(psHeader, 60, 0x200);
    put(psHeader, 64, 0x100);
    put(psMetadata, 4, static_cast<std::uint32_t>(ps.size()));
    put(psMetadata, 24, form.interpolators); // SQ_INTERPOLATOR_CNTL
    memory.Add(kPs, false, psHeader);
    memory.Add(kPs + 0x100 + 40, false, psMetadata);
    literalTable(memory, kPs + 0x200 + 40, options.pixelRecordFirst, kPsResource, true);

    std::vector<std::uint8_t> declaration(52 + form.elements.size() * 12);
    put(declaration, 24, static_cast<std::uint32_t>(form.elements.size()));
    for (std::size_t i = 0; i < form.elements.size(); ++i)
    {
        put(declaration, 52 + i * 12, form.elements[i].offset);
        put(declaration, 52 + i * 12 + 4, form.elements[i].type);
        declaration[52 + i * 12 + 9]  = static_cast<std::uint8_t>(form.elements[i].usage);
        declaration[52 + i * 12 + 10] = static_cast<std::uint8_t>(form.elements[i].index);
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
    // Positions in two quads, other float elements fixed per element, colors by vertex.
    std::vector<std::uint8_t> vertices(kVertexCount * form.stride);
    for (std::uint32_t v = 0; v < kVertexCount; ++v)
        for (std::size_t e = 0; e < form.elements.size(); ++e)
        {
            const auto&        element = form.elements[e];
            const auto         at      = v * form.stride + element.offset;
            std::vector<float> values;
            if (e == 0)
                values = { float(v % 2) * 100.0F, float(v / 2) * 50.0F, 0.5F };
            else if (element.format == 57)
                values = { e % 2 ? 0.0F : 1.0F, 0.0F, e % 2 ? 1.0F : 0.0F };
            else if (element.format == 37)
                values = { 0.25F * float(v), 0.75F };
            else
                store(vertices, at, element.usage == 1 ? 0xFF000000U | v : 0x00010203U);
            for (std::size_t i = 0; i < values.size(); ++i)
                store(vertices, at + i * 4, std::bit_cast<std::uint32_t>(values[i]));
        }
    memory.Add(kVertices, true, vertices);

    for (std::size_t slot = 0; slot < form.textures.size(); ++slot)
    {
        NativeTextureMemoryRanges ranges;
        std::string               error;
        if (!GetNativeTextureMemoryRanges(form.textures[slot], ranges, error))
            continue; // a refused fetch maps nothing
        auto [base, mips] = textureBytes(form.textures[slot], static_cast<std::uint8_t>(3 + slot * 6));
        memory.Add(ranges.base.address, true, base);
        if (ranges.mips.size)
            memory.Add(ranges.mips.address, true, mips);
    }

    // The device shadow: bindings, fixed and per-pair state, fetches,
    // constants, viewport.
    std::vector<std::uint8_t> state(0x3500);
    put(state, 0x3194, kPs);
    put(state, 0x3198, kVs);
    put(state, 0x2E2C, kDeclaration);
    put(state, 0x3094, kIndexBuffer);
    state[0x30F0] = static_cast<std::uint8_t>(form.stride / 4);
    put(state, 0x778, kVertices | 3);
    put(state, 0x77C, (std::uint32_t(vertices.size()) / 4) << 2 | 2);
    for (std::size_t slot = 0; slot < form.textures.size(); ++slot)
        for (std::size_t i = 0; i < 6; ++i)
            put(state, 0x480 + slot * 24 + i * 4, form.textures[slot][i]);
    for (const auto [reg, value] : kLeaderFixedState)
        put(state, shadowOf(reg), value);
    for (std::size_t i = 0; i < kTargetRegisters.size(); ++i)
        put(state, shadowOf(kTargetRegisters[i]), kTargetForms[options.target][i]);
    put(state, shadowOf(0x2200), form.depth);
    put(state, shadowOf(0x2201), form.blend);
    put(state, shadowOf(0x2205), form.raster);
    put(state, shadowOf(0x210E), kAlphaReference);
    // VS constants c0..c255 with stale c252..c255; PS constants c0..c223.
    for (std::uint32_t i = 0; i < 1024; ++i)
        put(state, 0x780 + i * 4, i >= 1008 ? 0xDEAD0000U + i : 0x3E000000U + i);
    for (std::uint32_t i = 0; i < 896; ++i)
        put(state, 0x1780 + i * 4, 0x3D000000U + i);
    NativeDrawReplayViewport viewport;
    NativeDrawReplayScissor  scissor;
    std::string              error;
    if (!DecodeNativeGameplayViewportAndScissor(GuestDrawRegisters(state), viewport, scissor, error))
        throw std::runtime_error("synthetic viewport: " + error);
    for (unsigned i = 0; const float value : { viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth })
        put(state, 0x3168 + i++ * 4, std::bit_cast<std::uint32_t>(value));
    memory.Add(kG, false, state);
    return memory;
}

const std::array<std::uint32_t, 8> kTuple{ kG, 4, 0, 0, kIndexCount, 0, 0, 0 };

// The catalog entry for a form: its PS digest, the VS words after the fold
// and the fetch patch, and the form's layout, textures and program words.
LeaderDrawShader entryFor(const Options& options)
{
    const auto&      form = options.form;
    LeaderDrawShader entry;
    auto&            shader                              = entry.shader;
    shader.pixelGuestWordDigest                          = IndexedBufferMicrocodeWordDigest(psCode(form));
    auto                                          memory = memoryFor(options);
    IndexedBufferDrawInputs                       inputs;
    std::string                                   error;
    const std::array<IndexedBufferPixelShader, 1> shaders{ { { shader.pixelGuestWordDigest, static_cast<std::uint32_t>(form.textures.size()) } } };
    UNSCOPED_INFO(std::string("synthetic leader inputs: ") + (error));
    REQUIRE(ReadIndexedBufferDrawInputs(submissionFor(kTuple), readerFor(memory), shaders, inputs, error, IndexedBufferForm::Leader));
    std::vector<std::uint8_t> folded = inputs.vertexCode, effective, patched;
    UNSCOPED_INFO(std::string("synthetic fold: ") + (error));
    REQUIRE(FoldSolidShaderMetadata(inputs, folded, effective, error));
    SolidDrawInputs linked = inputs;
    linked.vertexCode      = folded;
    entry.stride           = form.stride;
    for (const auto& element : fetched(form))
    {
        entry.attributeOffsets.push_back(element.offset);
        shader.vertexAttributes.push_back({ 95, form.stride, std::int32_t(element.offset), element.format, element.format != 6, element.format == 6 });
    }
    UNSCOPED_INFO(std::string("synthetic fetch patch: ") + (error));
    REQUIRE(PatchVertexFetch(linked, entry.attributeOffsets, patched, error));
    shader.vertexGuestWordDigest = IndexedBufferMicrocodeWordDigest(patched);
    shader.vertexShaderHash      = form.program;
    shader.pixelShaderHash       = form.psWord;
    shader.vertexShaderDxil      = { 'D', 'X', 'B', 'C' };
    shader.pixelShaderDxil       = { 'D', 'X', 'B', 'P' };
    shader.vertexDxilDigest      = dxilDigest(shader.vertexShaderDxil);
    shader.pixelDxilDigest       = dxilDigest(shader.pixelShaderDxil);
    for (std::uint32_t i = 0; i < form.dimensions.size(); ++i)
        shader.textureFetchSlots.push_back(i);
    shader.textureDimensions   = form.dimensions;
    shader.alphaTestEnabled    = true;
    shader.alphaTestCompare    = 4;
    shader.rasterizedDepth     = true;
    shader.independentSamplers = true;
    shader.shaderProgram       = form.program;
    shader.shaderContext       = form.context;
    shader.shaderInterpolators = form.interpolators;
    return entry;
}

// Whether two recipes hold the same draw: payload and layout bytes, not the
// identity of shared buffers or decoded versions.
bool sameRecipe(const NativeDrawReplayRecipe& left, const NativeDrawReplayRecipe& right)
{
    const auto bytes = [](std::span<const std::uint8_t> span)
    {
        return std::vector<std::uint8_t>(span.begin(), span.end());
    };
    const auto payload = [&](const NativeDrawReplayTexture& texture)
    {
        return texture.textureVersion ? *texture.textureVersion->Payload() : bytes(texture.payloadBytes());
    };
    for (std::size_t slot = 0; slot < left.textures.size(); ++slot)
    {
        const auto &a = left.textures[slot], &b = right.textures[slot];
        if (payload(a) != payload(b) || a.width != b.width || a.height != b.height || a.format != b.format || a.swizzle != b.swizzle || a.mipLevels != b.mipLevels ||
            a.dimension != b.dimension || a.frameTextureId != b.frameTextureId)
            return false;
    }
    for (std::size_t i = 0; i < std::min(left.samplers.size(), right.samplers.size()); ++i)
    {
        const auto &a = left.samplers[i], &b = right.samplers[i];
        if (std::tie(a.minLinear, a.magLinear, a.mipLinear, a.anisotropy, a.address, a.border, a.minLod, a.maxLod, a.mipBias) !=
            std::tie(b.minLinear, b.magLinear, b.mipLinear, b.anisotropy, b.address, b.border, b.minLod, b.maxLod, b.mipBias))
            return false;
    }
    const auto state = [](const NativeDrawReplayRecipe& recipe)
    {
        const auto& v    = recipe.viewport;
        const auto& d    = recipe.depth;
        const auto& s    = recipe.scissor;
        const auto& r    = recipe.rasterizer;
        const auto& t    = recipe.stencil;
        const auto  face = [](const NativeDrawReplayStencilState::Face& f)
        {
            return std::array{ f.compare, f.fail, f.depthFail, f.pass };
        };
        return std::tuple{ std::array{ v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth }, std::array{ s.left, s.top, s.right, s.bottom }, d.enabled, d.writeEnabled, d.compare, d.initialClear, d.initialSamples, r.cull, r.frontCounterClockwise, r.depthClip, t.enabled, t.reference, t.readMask, t.writeMask, face(t.front), face(t.back), recipe.sampleCount, recipe.sampleMask, recipe.depthOnly };
    };
    return state(left) == state(right) && bytes(left.vertexBytes()) == bytes(right.vertexBytes()) && left.indices == right.indices && left.vertexConstants == right.vertexConstants &&
           left.pixelConstants == right.pixelConstants && left.sharedConstants == right.sharedConstants && bytes(left.vertexShaderBytes()) == bytes(right.vertexShaderBytes()) &&
           bytes(left.pixelShaderBytes()) == bytes(right.pixelShaderBytes()) && left.blend == right.blend && left.samplers.size() == right.samplers.size() &&
           left.textureMask == right.textureMask && left.indexCount == right.indexCount && left.vertexStrideBytes == right.vertexStrideBytes &&
           left.vertexAttributeCount == right.vertexAttributeCount && left.width == right.width && left.height == right.height && left.targetFormat == right.targetFormat;
}

IndexedBufferDrawInputs readLeader(FakeMemory& memory, const LeaderDrawCatalog& catalog)
{
    IndexedBufferDrawInputs inputs;
    std::string             error;
    UNSCOPED_INFO(std::string("synthetic leader inputs: ") + (error));
    REQUIRE((ReadIndexedBufferDrawInputs(submissionFor(kTuple), readerFor(memory), LeaderPixelShaders(catalog), inputs, error, IndexedBufferForm::Leader) && inputs.inputsComplete));
    return inputs;
}

void unit()
{
    std::string       error;
    const Options     body, other = second();
    LeaderDrawCatalog catalog, secondOnly;
    catalog.entries    = { entryFor(body), entryFor(other) };
    secondOnly.entries = { entryFor(other) };

    // Reader: caller selection, the matched shader, literal records and textures.
    auto memory = memoryFor();
    {
        IndexedBufferDrawInputs foreign;
        UNSCOPED_INFO("the scene mesh call site is foreign to the Leader form without reads");
        REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kTuple, kSceneMeshDrawCaller), readerFor(memory), LeaderPixelShaders(catalog), foreign, error, IndexedBufferForm::Leader) &&
                 error.empty() && memory.reads == 0));
        UNSCOPED_INFO("the leader call site is foreign to the Menu form without reads");
        REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kTuple), readerFor(memory), std::string_view(catalog.entries[0].shader.pixelGuestWordDigest), foreign, error, IndexedBufferForm::Menu) &&
                 error.empty() && memory.reads == 0));
        UNSCOPED_INFO("a catalog without the body PS finds it foreign");
        REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kTuple), readerFor(memory), LeaderPixelShaders(secondOnly), foreign, error, IndexedBufferForm::Leader) && error.empty() &&
                 !foreign.sceneCandidate));
    }
    const auto inputs = readLeader(memory, catalog);
    UNSCOPED_INFO("the Leader reader keeps the common inputs and none of the Menu or Prepass extras");
    REQUIRE((inputs.sceneCandidate && inputs.pixelShader == 0 && inputs.arguments.stride == body.form.stride && inputs.indexCount == kIndexCount &&
             inputs.streams == std::vector<std::uint32_t>{ 0 } && inputs.streamBytes[0].size() == kVertexCount * body.form.stride && inputs.vertexLiterals.empty() &&
             inputs.pixelLiterals.empty() && inputs.textureFetch == std::array<std::uint32_t, 6>{} && inputs.textureBytes.empty()));
    UNSCOPED_INFO("both stages' records land at 0x43F0 and 0x47F0");
    REQUIRE((inputs.vertexLiteralRanges.size() == 1 && inputs.vertexLiteralRegisters.size() == 16 && inputs.vertexLiteralRegisters[0] == std::make_pair(0x43F0U, literal(0, false)) &&
             inputs.pixelLiteralRanges.size() == 1 && inputs.pixelLiteralRanges[0].firstFloat4 == 508 && inputs.pixelLiteralRegisters.size() == 16 &&
             inputs.pixelLiteralRegisters[0] == std::make_pair(0x47F0U, literal(0, true)) && inputs.pixelLiteralRegisters[15] == std::make_pair(0x47FFU, literal(15, true))));
    {
        const auto& textures      = body.form.textures;
        auto [flatBase, flatMips] = textureBytes(textures[0], 3);
        auto [cubeBase, cubeMips] = textureBytes(textures[1], 9);
        UNSCOPED_INFO("texture fetches 0 and 1 and their base and mip spans are read");
        REQUIRE((inputs.textureFetches == textures && inputs.textureRanges.size() == 2 && inputs.textureRanges[0].base.address == kFlat &&
                 inputs.textureRanges[1].mips.address == kCubeMips && inputs.textureBaseBytes.size() == 2 && inputs.textureBaseBytes[0] == flatBase && inputs.textureBaseBytes[1] == cubeBase &&
                 inputs.textureMipBytes.size() == 2 && inputs.textureMipBytes[0] == flatMips && inputs.textureMipBytes[1] == cubeMips && !flatMips.empty() && !cubeMips.empty()));
        IndexedBufferDrawInputs broken;
        auto                    untyped = memoryFor();
        put(untyped.At(kG, false).bytes, 0x480 + 24, textures[1][0] & ~3U);
        UNSCOPED_INFO(std::string("a non-texture fetch 1 is named: ") + (error));
        REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kTuple), readerFor(untyped), LeaderPixelShaders(catalog), broken, error, IndexedBufferForm::Leader) &&
                 error.starts_with("indexed_buffer_texture_range: ") && broken.sceneCandidate));
        auto                      shortMips = memoryFor();
        NativeTextureMemoryRanges ranges;
        UNSCOPED_INFO("cube ranges");
        REQUIRE(GetNativeTextureMemoryRanges(textures[1], ranges, error));
        shortMips.At(ranges.mips.address, true).bytes.resize(4096);
        UNSCOPED_INFO(std::string("short mip bytes are named: ") + (error));
        REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kTuple), readerFor(shortMips), LeaderPixelShaders(catalog), broken, error, IndexedBufferForm::Leader) &&
                 error == "indexed_buffer_texture_extent"));
        Options vertexRange;
        vertexRange.pixelRecordFirst = 252;
        auto stageError              = memoryFor(vertexRange);
        UNSCOPED_INFO(std::string("a pixel record in the vertex file is named: ") + (error));
        REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kTuple), readerFor(stageError), LeaderPixelShaders(catalog), broken, error, IndexedBufferForm::Leader) &&
                 error == "indexed_buffer_literals: shader_literal_stage"));
    }
    std::cout << "Leader reader: caller, matched shader, both literal records, texture spans and their refusals\n";

    LeaderDrawRecipe recipe;
    UNSCOPED_INFO("a catalog without the body PS finds it foreign");
    REQUIRE((!BuildLeaderDrawRecipe(inputs, secondOnly, recipe, error) && error.empty()));
    UNSCOPED_INFO(std::string("synthetic leader recipe: ") + (error));
    REQUIRE(BuildLeaderDrawRecipe(inputs, catalog, recipe, error));
    const auto& native = recipe.native;
    UNSCOPED_INFO("one 1280x720 4-sample RGBA8 target with 12 indices over 8 six-attribute vertices");
    REQUIRE((native.width == 1280 && native.height == 720 && native.sampleCount == 4 && native.sampleMask == 9 && native.extraTargetCount == 0 &&
             native.targetFormat == NativeDrawReplayTargetFormat::Rgba8 && native.indexCount == kIndexCount && native.vertexAttributeCount == 6 &&
             native.vertexBytes().size() == kVertexCount * 6 * 16));
    UNSCOPED_INFO("the 1280x351 viewport and scissor");
    REQUIRE((native.viewport.height == 351 && native.viewport.width == 1280 && native.scissor.bottom == 351 && native.scissor.right == 1280));
    UNSCOPED_INFO("the body's depth test and write, back-face culling and source-alpha blend");
    REQUIRE((native.depth.enabled && native.depth.writeEnabled && native.depth.compare == 4 && !native.stencil.enabled && native.rasterizer.cull == 2 &&
             native.rasterizer.frontCounterClockwise && native.blend.enabled && native.blend.sourceColor == NativeDrawReplayBlendFactor::SourceAlpha &&
             native.blend.destinationColor == NativeDrawReplayBlendFactor::InverseSourceAlpha && native.blend.sourceAlpha == NativeDrawReplayBlendFactor::One &&
             native.blend.destinationAlpha == NativeDrawReplayBlendFactor::Zero && native.blend.writeMask == 0xF));
    UNSCOPED_INFO("the alpha reference reaches shared byte 300");
    REQUIRE((std::uint32_t(native.sharedConstants[300]) == (kAlphaReference & 0xFF) && std::uint32_t(native.sharedConstants[303]) == kAlphaReference >> 24));
    UNSCOPED_INFO("fetch 0 binds the BC1 mip chain and fetch 1 the RGBA8 cube chain, each with its sampler");
    REQUIRE((native.textureMask == 3 && native.textures[0].format == NativeDrawReplayTextureFormat::Bc1 && native.textures[0].mipLevels == 8 &&
             native.textures[0].dimension == NativeDrawReplayTextureDimension::Texture2D && native.textures[1].format == NativeDrawReplayTextureFormat::Rgba8 &&
             native.textures[1].dimension == NativeDrawReplayTextureDimension::TextureCube && native.textures[1].mipLevels == 8 && native.samplers.size() == 2 &&
             native.samplers[0].mipLinear && native.samplers[1].address == std::array<std::uint32_t, 3>{ 3, 3, 3 }));
    const auto constant = [&](const std::vector<std::uint8_t>& bank, std::size_t index)
    {
        return little(bank, index * 4);
    };
    bool overlaid = native.vertexConstants.size() == 4096 && native.pixelConstants.size() == 4096 && constant(native.vertexConstants, 1007) == 0x3E000000U + 1007 &&
                    constant(native.pixelConstants, 895) == 0x3D000000U + 895;
    for (std::uint32_t i = 0; overlaid && i < 16; ++i)
        overlaid = constant(native.vertexConstants, 1008 + i) == literal(i, false) && constant(native.pixelConstants, 1008 + i) == literal(i, true);
    UNSCOPED_INFO("each stage's record replaces its c252..c255 and leaves the shadow's constants");
    REQUIRE(overlaid);
    std::cout << "Synthetic leader draw builds: target, viewport, depth, cull, blend, alpha reference, mip and cube textures, literal overlay\n";

    // The second pair: its own stride, unfetched declaration element, program
    // words, three-texture set and state forms, from its catalog entry.
    auto secondMemory = memoryFor(other);
    {
        const auto secondInputs = readLeader(secondMemory, catalog);
        UNSCOPED_INFO("the reader takes the second pair's three fetches, eight elements and seven records");
        REQUIRE((secondInputs.pixelShader == 1 && secondInputs.textureFetches == other.form.textures && secondInputs.declaration.size() == 8 * 12 &&
                 word(secondInputs.vertexMetadata, 28) == 7 && secondInputs.arguments.stride == 72));
        LeaderDrawRecipe secondRecipe;
        UNSCOPED_INFO(std::string("the second pair builds: ") + (error));
        REQUIRE(BuildLeaderDrawRecipe(secondInputs, catalog, secondRecipe, error));
        const auto& draw = secondRecipe.native;
        UNSCOPED_INFO("the second pair binds its BC2 chain, the cube and a one-level BC1 with their samplers");
        REQUIRE((draw.vertexAttributeCount == 7 && draw.vertexBytes().size() == kVertexCount * 7 * 16 && draw.textureMask == 7 &&
                 draw.textures[0].format == NativeDrawReplayTextureFormat::Bc2 && draw.textures[0].mipLevels == 8 &&
                 draw.textures[0].dimension == NativeDrawReplayTextureDimension::Texture2D && draw.textures[1].format == NativeDrawReplayTextureFormat::Rgba8 &&
                 draw.textures[1].dimension == NativeDrawReplayTextureDimension::TextureCube && draw.textures[2].format == NativeDrawReplayTextureFormat::Bc1 &&
                 draw.textures[2].mipLevels == 1 && draw.samplers.size() == 3 && draw.samplers[0].anisotropy == kLeaderAnisotropicOverride &&
                 draw.samplers[2].anisotropy == 0));
        UNSCOPED_INFO("the second pair blends color and alpha by source alpha without culling");
        REQUIRE((draw.depth.writeEnabled && draw.rasterizer.cull == 0 && draw.blend.sourceColor == NativeDrawReplayBlendFactor::SourceAlpha &&
                 draw.blend.destinationColor == NativeDrawReplayBlendFactor::InverseSourceAlpha && draw.blend.sourceAlpha == NativeDrawReplayBlendFactor::SourceAlpha &&
                 draw.blend.destinationAlpha == NativeDrawReplayBlendFactor::InverseSourceAlpha));
        UNSCOPED_INFO(std::string("a catalog of only the second pair builds it: ") + (error));
        REQUIRE(BuildLeaderDrawRecipe(secondInputs, secondOnly, secondRecipe, error));
        // The body's entry cannot serve the second pair's words.
        auto swapped                                   = catalog;
        swapped.entries[0].shader.pixelGuestWordDigest = catalog.entries[1].shader.pixelGuestWordDigest;
        swapped.entries[1].shader.pixelGuestWordDigest = catalog.entries[0].shader.pixelGuestWordDigest;
        UNSCOPED_INFO(std::string("the body's entry refuses the second pair's three textures: ") + (error));
        REQUIRE((!BuildLeaderDrawRecipe(secondInputs, swapped, secondRecipe, error) && error == "leader_inputs_incomplete"));
    }
    std::cout << "A second pair with its own layout, program words, texture set and state forms builds from its entry\n";

    // Named refusals, one rule each.
    const auto refused = [&](const std::string& name, auto mutate, const std::string& expected, std::optional<IndexedBufferDrawInputs> base = std::nullopt, const LeaderDrawCatalog* otherCatalog = nullptr)
    {
        auto broken = base ? *base : inputs;
        mutate(broken);
        LeaderDrawRecipe rejected;
        std::string      reason;
        const bool       built = BuildLeaderDrawRecipe(broken, otherCatalog ? *otherCatalog : catalog, rejected, reason);
        UNSCOPED_INFO(name + ": " + (built ? "accepted" : reason));
        REQUIRE((!built && reason.starts_with(expected)));
    };
    const auto none = [](IndexedBufferDrawInputs&)
    {
    };
    using Inputs = IndexedBufferDrawInputs;
    refused("incomplete inputs", [](Inputs& in)
            {
                in.inputsComplete = false;
            },
            "leader_inputs_incomplete");
    refused("one texture read", [](Inputs& in)
            {
                in.textureFetches.pop_back();
            },
            "leader_inputs_incomplete");
    refused("one mip span read", [](Inputs& in)
            {
                in.textureMipBytes.pop_back();
            },
            "leader_inputs_incomplete");
    refused("one base span read", [](Inputs& in)
            {
                in.textureBaseBytes.pop_back();
            },
            "leader_inputs_incomplete");
    refused("the scene mesh caller", [](Inputs& in)
            {
                in.arguments.caller = kSceneMeshDrawCaller;
            },
            "leader_unsupported_draw_tuple");
    refused("a triangle strip", [](Inputs& in)
            {
                in.primitive = 6;
            },
            "leader_unsupported_draw_tuple");
    refused("a nonzero base vertex", [](Inputs& in)
            {
                in.baseVertex = 1;
            },
            "leader_unsupported_draw_tuple");
    refused("32-bit indices", [](Inputs& in)
            {
                in.indexFormat = 1;
            },
            "leader_unsupported_draw_tuple");
    refused("index endian 2", [](Inputs& in)
            {
                in.indexEndian = 2;
            },
            "leader_unsupported_draw_tuple");
    refused("no indices", [](Inputs& in)
            {
                in.indexCount = 0;
                in.indexBytes.clear();
            },
            "leader_unsupported_draw_tuple");
    refused("a partial triangle", [](Inputs& in)
            {
                in.indexCount = 11;
                in.indexBytes.resize(22);
            },
            "leader_unsupported_draw_tuple");
    refused("indices short of the count", [](Inputs& in)
            {
                in.indexBytes.resize(22);
            },
            "leader_unsupported_draw_tuple");
    refused("a predicated shader", [](Inputs& in)
            {
                in.state[0x2ABC] |= 0x40;
            },
            "leader_conditional_or_recorded_shader");
    refused("a recorded shader", [](Inputs& in)
            {
                in.state[0x2ABE] |= 0x40;
            },
            "leader_conditional_or_recorded_shader");
    refused("five fetch records", [](Inputs& in)
            {
                store(in.vertexMetadata, 28, 5);
            },
            "leader_fetch_count");
    // The unmatched sixth record patches to a VS the entry does not hold.
    refused("a five-element declaration", [](Inputs& in)
            {
                in.declaration.resize(60);
            },
            "leader_patched_vs_identity");
    refused("the second pair's stride", [](Inputs& in)
            {
                in.arguments.stride = 72;
            },
            "leader_stream_form");
    refused("a second stream", [](Inputs& in)
            {
                in.streams.push_back(1);
                in.streamFetches.push_back(in.streamFetches[0]);
                in.streamBytes.push_back(in.streamBytes[0]);
            },
            "leader_stream_form");
    refused("a VS literal register in the pixel constants", [](Inputs& in)
            {
                in.vertexLiteralRegisters[15].first = 0x4400;
            },
            "leader_literal_stage");
    refused("a PS literal register in the vertex constants", [](Inputs& in)
            {
                in.pixelLiteralRegisters[3].first = 0x43FF;
            },
            "leader_literal_stage");
    refused("a PS literal register past the constants", [](Inputs& in)
            {
                in.pixelLiteralRegisters[3].first = 0x4800;
            },
            "leader_literal_stage");
    refused("a changed VS instruction", [](Inputs& in)
            {
                in.vertexCode[8] ^= 1;
            },
            "leader_patched_vs_identity");
    refused("an unadmitted UV offset", [](Inputs& in)
            {
                store(in.declaration, 12, 16);
            },
            "adapter_attribute_offset");

    // Literal records overlay in guest order, whichever float4s they name.
    {
        auto bare = inputs;
        bare.vertexLiteralRanges.clear();
        bare.vertexLiteralRegisters.clear();
        bare.pixelLiteralRanges.clear();
        bare.pixelLiteralRegisters.clear();
        UNSCOPED_INFO(std::string("a pair without literal records keeps the shadow's constants: ") + (error));
        REQUIRE((BuildLeaderDrawRecipe(bare, catalog, recipe, error) && constant(recipe.native.vertexConstants, 1008) == 0xDEAD0000U + 1008 &&
                 constant(recipe.native.pixelConstants, 895) == 0x3D000000U + 895));
        auto layered = inputs;
        layered.vertexLiteralRanges.push_back({ 248, 1, 0x1000 });
        layered.vertexLiteralRegisters.push_back({ 0x43E0, 0xCAFEF00D });
        layered.pixelLiteralRanges.push_back({ 508, 1, 0x1000 });
        layered.pixelLiteralRegisters.push_back({ 0x47F0, 0x12345678 });
        UNSCOPED_INFO(std::string("a further record lands at its own float4 and a later one overrides an earlier one: ") + (error));
        REQUIRE((BuildLeaderDrawRecipe(layered, catalog, recipe, error) && constant(recipe.native.vertexConstants, 992) == 0xCAFEF00D &&
                 constant(recipe.native.pixelConstants, 1008) == 0x12345678 && constant(recipe.native.pixelConstants, 1009) == literal(1, true)));
    }

    // Every pinned register, one change each; the program words come from the
    // shader metadata through the fold and must equal the entry's.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pinned(kLeaderFixedState.begin(), kLeaderFixedState.end());
    pinned.insert(pinned.end(), { { 0x2180, body.form.program }, { 0x2181, body.form.context }, { 0x2182, body.form.interpolators } });
    for (const auto& [reg, value] : pinned)
    {
        const auto changed = reg == 0x2208 ? value ^ 0x100U : value ^ 1U;
        const auto mutate  = [&](Inputs& in)
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
        refused(std::format("register {:#x}", reg), mutate, std::format("leader_fixed_state {:#x}: {:#x}", reg, changed));
    }
    // The world's full-window form builds for both pairs; apart from the
    // viewport, the scissor and the reciprocal viewport height in shared
    // bytes 296..299, the draw is the band's byte for byte.
    {
        LeaderDrawRecipe band;
        UNSCOPED_INFO(std::string("the band form builds: ") + (error));
        REQUIRE(BuildLeaderDrawRecipe(inputs, catalog, band, error));
        Options full;
        full.target                 = 1;
        auto             fullMemory = memoryFor(full);
        const auto       fullInputs = readLeader(fullMemory, catalog);
        LeaderDrawRecipe window;
        UNSCOPED_INFO(std::string("the full-window form builds: ") + (error));
        REQUIRE(BuildLeaderDrawRecipe(fullInputs, catalog, window, error));
        const auto& drawn = window.native;
        UNSCOPED_INFO("the full-window form draws the 1280x720 viewport and scissor");
        REQUIRE((drawn.viewport.x == 0 && drawn.viewport.y == 0 && drawn.viewport.width == 1280 && drawn.viewport.height == 720 && drawn.scissor.left == 0 &&
                 drawn.scissor.top == 0 && drawn.scissor.right == 1280 && drawn.scissor.bottom == 720));
        bool sharedOtherwise = drawn.sharedConstants.size() == band.native.sharedConstants.size();
        for (std::size_t i = 0; sharedOtherwise && i < drawn.sharedConstants.size(); ++i)
            sharedOtherwise = (i >= 296 && i < 300) || drawn.sharedConstants[i] == band.native.sharedConstants[i];
        UNSCOPED_INFO("the shared constants differ only in the reciprocal viewport height");
        REQUIRE((sharedOtherwise && little(drawn.sharedConstants, 296) == std::bit_cast<std::uint32_t>(-1.0F / 720.0F) &&
                 little(band.native.sharedConstants, 296) == std::bit_cast<std::uint32_t>(-1.0F / 351.0F)));
        auto expected            = band.native;
        expected.viewport        = drawn.viewport;
        expected.scissor         = drawn.scissor;
        expected.sharedConstants = drawn.sharedConstants;
        UNSCOPED_INFO("the full-window draw is otherwise the band's");
        REQUIRE(sameRecipe(expected, drawn));

        Options fullSecond = second();
        fullSecond.target  = 1;
        auto secondWindow  = memoryFor(fullSecond);
        UNSCOPED_INFO(std::string("the second pair builds in the full-window form: ") + (error));
        REQUIRE((BuildLeaderDrawRecipe(readLeader(secondWindow, catalog), catalog, window, error) && window.native.viewport.height == 720 && window.native.scissor.bottom == 720));
        NativeDrawReplayRecipe draw;
        std::string            detail;
        auto                   family = MakeLeaderDrawFamily(readerFor(fullMemory), std::make_shared<LeaderDrawCatalog>(catalog));
        UNSCOPED_INFO(std::string("the family builds the full-window draw: ") + (detail));
        REQUIRE((family(submissionFor(kTuple), draw, detail) == NativeDrawFamilyResult::Built && detail.empty() && draw.viewport.height == 720));

        // Each form holds whole: the other form's scissor, or one word off,
        // is named with all three words.
        refused("the full-window scissor in the band viewport", [](Inputs& in)
                {
                    store(in.state, shadowOf(0x2082), 0x02D00500);
                },
                "leader_target_form: 0x2d00500 0xc32f8000 0x432f8000");
        refused("the band scissor in the full-window viewport", [](Inputs& in)
                {
                    store(in.state, shadowOf(0x2082), 0x015F0500);
                },
                "leader_target_form: 0x15f0500 0xc3b40000 0x43b40000",
                fullInputs);
        for (std::size_t target = 0; target < kTargetForms.size(); ++target)
            for (std::size_t i = 0; i < kTargetRegisters.size(); ++i)
            {
                auto words = kTargetForms[target];
                words[i] ^= 1U;
                refused(std::format("target form {} register {:#x}", target, kTargetRegisters[i]), [&](Inputs& in)
                        {
                            store(in.state, shadowOf(kTargetRegisters[i]), words[i]);
                        },
                        std::format("leader_target_form: {:#x} {:#x} {:#x}", words[0], words[1], words[2]),
                        target ? fullInputs : inputs);
            }
        refused("a full-window viewport shadow of the band's height", [](Inputs& in)
                {
                    store(in.state, 0x3168 + 12, 0x43AF8000);
                },
                "leader_viewport_shadow_scale_mismatch",
                fullInputs);
    }
    {
        auto programmed                            = catalog;
        programmed.entries[0].shader.shaderProgram = 0x20510709;
        refused("an entry with the second pair's program words", none, "leader_fixed_state 0x2180: 0x20510c12", std::nullopt, &programmed);
    }
    // Each per-pair form builds; a word outside the forms is named.
    for (const auto& [reg, forms, code] : { std::tuple{ 0x2200U, std::span<const std::uint32_t>(kLeaderDepthControls), "leader_depth_control" },
                                            std::tuple{ 0x2201U, std::span<const std::uint32_t>(kLeaderBlendControls), "leader_blend_control" },
                                            std::tuple{ 0x2205U, std::span<const std::uint32_t>(kLeaderRasterControls), "leader_raster_control" } })
    {
        for (const auto value : forms)
        {
            auto formed = inputs;
            store(formed.state, shadowOf(reg), value);
            UNSCOPED_INFO(std::string(std::format("{} {:#x} builds: ", code, value)) + (error));
            REQUIRE(BuildLeaderDrawRecipe(formed, catalog, recipe, error));
        }
        const auto changed = forms[0] ^ 1U;
        refused(std::format("{} {:#x}", code, changed), [&](Inputs& in)
                {
                    store(in.state, shadowOf(reg), changed);
                },
                std::format("{}: {:#x}", code, changed));
    }
    {
        auto glowState = inputs;
        store(glowState.state, shadowOf(0x2200), 0x24F00732);
        store(glowState.state, shadowOf(0x2201), 0x01060106);
        store(glowState.state, shadowOf(0x2205), 0x18000);
        UNSCOPED_INFO(std::string("the depth-test-only, additive and unculled forms reach the draw: ") + (error));
        REQUIRE((BuildLeaderDrawRecipe(glowState, catalog, recipe, error) && recipe.native.depth.enabled && !recipe.native.depth.writeEnabled &&
                 recipe.native.blend.destinationColor == NativeDrawReplayBlendFactor::One && recipe.native.rasterizer.cull == 0));
    }

    refused("a fetch other than the shadow's", [](Inputs& in)
            {
                in.textureFetches[1][3] ^= 1U << 19;
            },
            "leader_texture_fetch_mismatch");
    {
        Options aniso;
        auto    fetch          = leaderFetch(xenos::TextureFormat::k_8_8_8_8, true, 128, 7, kCube, kCubeMips);
        fetch.aniso_filter     = xenos::AnisoFilter::kMax_4_1;
        aniso.form.textures[1] = fetchWords(fetch);
        auto at                = memoryFor(aniso);
        refused("an anisotropic cube", none, "leader_sampler_anisotropy", readLeader(at, catalog));
        Options flatCube;
        flatCube.form.textures[1] = fetchWords(leaderFetch(xenos::TextureFormat::k_8_8_8_8, false, 128, 7, kCube, kCubeMips));
        auto flat                 = memoryFor(flatCube);
        refused("a 2D texture in the cube slot", none, "leader_sampler_dimension", readLeader(flat, catalog));
    }
    refused("short base bytes", [](Inputs& in)
            {
                in.textureBaseBytes[0].resize(in.textureBaseBytes[0].size() / 2);
            },
            "leader_texture_decode: ");
    refused("a viewport shadow that disagrees", [](Inputs& in)
            {
                store(in.state, 0x3168 + 12, 0x43B40000);
            },
            "leader_viewport_shadow_scale_mismatch");
    // The interface the build uses is the matched entry's.
    {
        auto changed                               = catalog;
        changed.entries[0].shader.alphaTestEnabled = false;
        refused("an interface without the alpha test", none, "leader_recipe_rejected: ", std::nullopt, &changed);
        changed                                     = catalog;
        changed.entries[0].shader.textureDimensions = { NativeDrawReplayTextureDimension::TextureCube, NativeDrawReplayTextureDimension::Texture2D };
        refused("an entry with the slots' dimensions swapped", none, "leader_texture_fetch_layout", std::nullopt, &changed);
        changed                                              = catalog;
        changed.entries[0].shader.vertexAttributes[2].format = 37;
        refused("an entry reading a float3 as a float2", none, "leader_vertex_fetch_layout", std::nullopt, &changed);
        changed                                                = catalog;
        changed.entries[0].shader.vertexAttributes[4].isSigned = true;
        refused("an entry reading color bytes as signed", none, "leader_vertex_fetch_layout", std::nullopt, &changed);
        changed = catalog;
        std::swap(changed.entries[0].shader.vertexAttributes[2], changed.entries[0].shader.vertexAttributes[3]);
        refused("an entry with two attributes reordered", none, "leader_vertex_fetch_layout", std::nullopt, &changed);
        // Without slots the reader reads no textures, so only the live PS shows them.
        changed = catalog;
        changed.entries[0].shader.textureFetchSlots.clear();
        changed.entries[0].shader.textureDimensions.clear();
        refused("an entry without texture slots", none, "leader_texture_fetch_layout", readLeader(memory, changed), &changed);
        changed                   = catalog;
        changed.entries[0].stride = 72;
        refused("an entry with another stride", none, "leader_stream_form", std::nullopt, &changed);
        changed = catalog;
        changed.entries[0].attributeOffsets.pop_back();
        refused("an entry with five attributes", none, "leader_fetch_count", std::nullopt, &changed);
        changed                                = catalog;
        changed.entries[0].attributeOffsets[1] = 16;
        refused("an entry without the UV offset", none, "adapter_attribute_offset", std::nullopt, &changed);
        changed                                         = catalog;
        changed.entries[0].shader.vertexGuestWordDigest = catalog.entries[1].shader.vertexGuestWordDigest;
        refused("an entry with the second pair's VS words", none, "leader_patched_vs_identity", std::nullopt, &changed);
    }
    std::cout << "Leader tuple, stream, literal, VS identity, fixed-state, target-form, state-form, texture, sampler and entry refusals are named\n";

    // The family: foreign without reads for other callers, "not mine" for
    // other PS words, both synthetic pairs built, textures reused.
    NativeDrawReplayRecipe draw;
    std::string            detail;
    const auto             catalogPtr    = std::make_shared<LeaderDrawCatalog>(catalog);
    auto                   foreignFamily = MakeLeaderDrawFamily(readerFor(memory), std::make_shared<LeaderDrawCatalog>(secondOnly));
    UNSCOPED_INFO("a family without the body PS finds the body draw not its own");
    REQUIRE((foreignFamily(submissionFor(kTuple), draw, detail) == NativeDrawFamilyResult::NotMine && detail.empty()));
    auto family  = MakeLeaderDrawFamily(readerFor(memory), catalogPtr);
    memory.reads = 0;
    UNSCOPED_INFO("another IndexedBuffer caller is foreign without reads");
    REQUIRE((family(submissionFor(kTuple, kSceneMeshDrawCaller), draw, detail) == NativeDrawFamilyResult::NotMine && detail.empty() && memory.reads == 0));
    UNSCOPED_INFO(std::string("the family admits the synthetic draw: ") + (detail));
    REQUIRE((family(submissionFor(kTuple), draw, detail) == NativeDrawFamilyResult::Built && detail.empty() && draw.textureMask == 3));
    const auto first = draw.textures[1].textureVersion;
    UNSCOPED_INFO("a held cube reuses its decoded version");
    REQUIRE((family(submissionFor(kTuple), draw, detail) == NativeDrawFamilyResult::Built && first && draw.textures[1].textureVersion == first));

    // The resolved texture seam: a miss builds the recipe the bytes built, a
    // resolved slot binds its frame texture without reading that slot's
    // storage, and the query sees the submission's op mark.
    {
        auto submission          = submissionFor(kTuple);
        submission.operationMark = 0x51;
        NativeDrawReplayRecipe bytesDraw, missDraw, resolvedDraw;
        UNSCOPED_INFO(std::string("the byte family builds: ") + (detail));
        REQUIRE(MakeLeaderDrawFamily(readerFor(memory), catalogPtr)(submission, bytesDraw, detail) == NativeDrawFamilyResult::Built);
        const auto                           inputs = readLeader(memory, catalog);
        const auto&                          ranges = inputs.textureRanges[0];
        std::vector<std::uint64_t>           marks;
        std::optional<NativeResolvedTexture> answer;
        auto                                 resolving = readerFor(memory);
        resolving.resolvedTexture                      = [&](const std::array<std::uint32_t, 6>& fetch, std::uint64_t opMark, NativeResolvedTextureMiss& miss) -> std::optional<NativeResolvedTexture>
        {
            marks.push_back(opMark);
            miss = NativeResolvedTextureMiss::Coverage;
            return fetch == inputs.textureFetches[0] ? answer : std::nullopt;
        };
        const auto readsSlot0 = [&]
        {
            return std::ranges::any_of(memory.log, [&](const auto& read)
                                       {
                                           return std::ranges::any_of(std::array{ ranges.base, ranges.mips }, [&](const auto& range)
                                                                      {
                                                                          return range.size && read.first < range.address + range.size && range.address < read.first + read.second;
                                                                      });
                                       });
        };
        auto resolvingFamily = MakeLeaderDrawFamily(resolving, catalogPtr);
        memory.log.clear();
        UNSCOPED_INFO(std::string("a miss reads the slot and builds the byte recipe, and each fetch's query sees the op mark: ") + (detail));
        REQUIRE((resolvingFamily(submission, missDraw, detail) == NativeDrawFamilyResult::Built && sameRecipe(missDraw, bytesDraw) && readsSlot0() &&
                 marks == std::vector<std::uint64_t>(2, 0x51)));
        const auto& decoded = bytesDraw.textures[0];
        // A resolve's frame texture holds one 2D level.
        UNSCOPED_INFO("slot 0 samples a 2D texture");
        REQUIRE(decoded.dimension == NativeDrawReplayTextureDimension::Texture2D);
        answer = NativeResolvedTexture{ 0x77, { decoded.width, decoded.height, decoded.format, 1, NativeDrawReplayTextureDimension::Texture2D }, decoded.swizzle };
        memory.log.clear();
        UNSCOPED_INFO(std::string("a resolved slot reads none of its storage: ") + (detail));
        REQUIRE((resolvingFamily(submission, resolvedDraw, detail) == NativeDrawFamilyResult::Built && !readsSlot0()));
        const auto& bound = resolvedDraw.textures[0];
        UNSCOPED_INFO("the slot binds the frame texture with the resolved layout");
        REQUIRE((bound.frameTextureId == 0x77 && bound.payloadBytes().empty() && !bound.textureVersion && bound.width == decoded.width && bound.height == decoded.height &&
                 bound.format == decoded.format && bound.mipLevels == 1 && bound.dimension == decoded.dimension && bound.swizzle == decoded.swizzle));
        resolvedDraw.textures[0] = decoded;
        UNSCOPED_INFO("and the rest of the recipe is unchanged");
        REQUIRE(sameRecipe(resolvedDraw, bytesDraw));
    }
    auto secondFamily = MakeLeaderDrawFamily(readerFor(secondMemory), catalogPtr);
    UNSCOPED_INFO(std::string("the family admits the second pair from the same catalog: ") + (detail));
    REQUIRE((secondFamily(submissionFor(kTuple), draw, detail) == NativeDrawFamilyResult::Built && draw.textureMask == 7 && draw.vertexAttributeCount == 7));
    store(memory.At(kG, false).bytes, shadowOf(0x2205), 0x18001);
    UNSCOPED_INFO(std::string("the family names a refusal: ") + (detail));
    REQUIRE((family(submissionFor(kTuple), draw, detail) == NativeDrawFamilyResult::Refused && detail == "leader_raster_control: 0x18001"));
    // A kSceneMeshDrawCaller catalog (the menu band): the SceneLeader form
    // reads only the scene call site, and each catalog admits only its own.
    {
        auto scene                         = catalog;
        scene.caller                       = kSceneMeshDrawCaller;
        auto                    bandMemory = memoryFor(other);
        IndexedBufferDrawInputs band;
        UNSCOPED_INFO("the leader call site is foreign to the SceneLeader form without reads");
        REQUIRE((!ReadIndexedBufferDrawInputs(submissionFor(kTuple), readerFor(bandMemory), LeaderPixelShaders(scene), band, error, IndexedBufferForm::SceneLeader) && error.empty() &&
                 bandMemory.reads == 0));
        UNSCOPED_INFO(std::string("the SceneLeader form reads the scene call site as the Leader form does: ") + (error));
        REQUIRE((ReadIndexedBufferDrawInputs(submissionFor(kTuple, kSceneMeshDrawCaller), readerFor(bandMemory), LeaderPixelShaders(scene), band, error, IndexedBufferForm::SceneLeader) &&
                 band.inputsComplete && band.pixelShader == 1 && band.pixelLiteralRegisters.size() == 16 && band.textureFetches == other.form.textures));
        LeaderDrawRecipe bandRecipe;
        UNSCOPED_INFO(std::string("the scene catalog builds the second pair from 0x8250C768: ") + (error));
        REQUIRE((BuildLeaderDrawRecipe(band, scene, bandRecipe, error) && bandRecipe.native.textureMask == 7));
        UNSCOPED_INFO(std::string("the leader catalog refuses a scene-caller draw: ") + (error));
        REQUIRE((!BuildLeaderDrawRecipe(band, catalog, bandRecipe, error) && error == "leader_unsupported_draw_tuple"));
        UNSCOPED_INFO(std::string("the scene catalog refuses a leader-caller draw: ") + (error));
        REQUIRE((!BuildLeaderDrawRecipe(readLeader(secondMemory, catalog), scene, bandRecipe, error) && error == "leader_unsupported_draw_tuple"));
        auto sceneFamily = MakeLeaderDrawFamily(readerFor(bandMemory), std::make_shared<LeaderDrawCatalog>(scene));
        UNSCOPED_INFO(std::string("the scene family admits the scene-caller draw: ") + (detail));
        REQUIRE((sceneFamily(submissionFor(kTuple, kSceneMeshDrawCaller), draw, detail) == NativeDrawFamilyResult::Built && draw.textureMask == 7));
        bandMemory.reads = 0;
        UNSCOPED_INFO("the scene family finds the leader caller foreign without reads");
        REQUIRE((sceneFamily(submissionFor(kTuple), draw, detail) == NativeDrawFamilyResult::NotMine && detail.empty() && bandMemory.reads == 0));
        // The menu catalog family tries the backdrop first, then the band.
        auto menu = MakeSceneMenuCatalogFamily(readerFor(bandMemory), { std::make_shared<NativeGameplayShaderInterface>(), std::make_shared<LeaderDrawCatalog>(scene) });
        UNSCOPED_INFO(std::string("the menu family builds a band draw the backdrop finds not its own: ") + (detail));
        REQUIRE((menu(submissionFor(kTuple, kSceneMeshDrawCaller), draw, detail) == NativeDrawFamilyResult::Built && draw.textureMask == 7));
        auto emptyBand    = scene;
        emptyBand.entries = {};
        auto backdropOnly = MakeSceneMenuCatalogFamily(readerFor(bandMemory), { std::make_shared<NativeGameplayShaderInterface>(), std::make_shared<LeaderDrawCatalog>(emptyBand) });
        UNSCOPED_INFO("a menu family without band pairs finds the band draw not its own");
        REQUIRE((backdropOnly(submissionFor(kTuple, kSceneMeshDrawCaller), draw, detail) == NativeDrawFamilyResult::NotMine && detail.empty()));
        store(bandMemory.At(kG, false).bytes, shadowOf(0x2205), 0x18001);
        UNSCOPED_INFO(std::string("the menu family names a band refusal: ") + (detail));
        REQUIRE((menu(submissionFor(kTuple, kSceneMeshDrawCaller), draw, detail) == NativeDrawFamilyResult::Refused && detail == "leader_raster_control: 0x18001"));
    }
    std::cout << "Scene-caller leader catalog: SceneLeader reads, per-caller admission and the menu family's band fallback\n";

    // The registry places the family after the menu backdrop.
    const auto registry = NativeDrawFamilyRegistry();
    const auto at       = std::ranges::find(registry, std::string_view("native_draw_leader_shaders"), &NativeDrawFamilyRegistration::option);
    UNSCOPED_INFO("the leader family follows the menu backdrop");
    REQUIRE((at != registry.end() && at != registry.begin() && (at - 1)->option == "native_draw_menu_shaders" && at->name == "leader model"));
    std::string      summary;
    NativeDrawFamily admitted;
    UNSCOPED_INFO("a missing catalog is not admitted");
    REQUIRE((!at->admit(std::filesystem::temp_directory_path() / "leader-catalog-missing", readerFor(memory), admitted, summary, error) && !error.empty() && !admitted));
    std::cout << "Leader family: selection, admission, both pairs, texture reuse, registry order and named refusal checks passed\n";
}

} // namespace synthetic

void unit()
{
    patchBeyondThree();
    fetchLayout();
    sampler();
    cache();
    catalog();
    synthetic::unit();
}

// The civ select capture: frames 11777..11780, nine leader draws per frame.
namespace capture
{

constexpr std::uint64_t kFirstFrame = 11777, kFrames = 4;

// The capture's two pairs and their sampled texture counts.
struct CapturePair
{
    std::string_view name;
    std::uint64_t    vertexHash = 0, pixelHash = 0;
    std::string_view vertexWords, pixelWords;
    std::size_t      textures = 0;
};

constexpr std::array<CapturePair, 2> kCapturePairs{ {
    { "body", 0x7808944337B1C1C3ULL, 0x85222C88DE4F1473ULL, "d89fe1f3b6cc2afc424a9e75a7b0a739632162f6a80af9370a86e047fc95c213", "22f1d804d1317e7d4646507e3891d41e8a255d66844f8c4ed83b7168c0d903f9", 10 },
    { "glow", 0xC1CAAFCB5EB76397ULL, 0x24F4430C7DDCA67FULL, "3c7c55f6db341e5c8af2c387a06abc99c153dcd05d61651984fc59aa015ed031", "d3bc8bf39dd194800fe0314ecbaa7ed4584b53b8cb4329dab37beb789fb141a3", 1 },
} };

struct Draw
{
    std::uint64_t                sequence = 0, frame = 0;
    std::uint32_t                entry = 0, caller = 0, stack = 0;
    std::array<std::uint32_t, 8> arguments{};

    struct File
    {
        std::string           role;
        std::filesystem::path path;
        std::uint64_t         address  = 0;
        bool                  physical = false;
    };

    std::vector<File> files;

    std::vector<std::uint8_t> payload(const std::string& role) const
    {
        const auto found = std::ranges::find(files, role, &File::role);
        UNSCOPED_INFO(std::format("draw {} has one {} payload", sequence, role));
        REQUIRE((found != files.end() && std::ranges::count(files, role, &File::role) == 1));
        return read(found->path);
    }

    std::vector<const File*> all(const std::string& role) const
    {
        std::vector<const File*> result;
        for (const auto& file : files)
            if (file.role == role)
                result.push_back(&file);
        return result;
    }
};

std::vector<Draw> loadDraws(const std::filesystem::path& capture)
{
    const auto status = toml::parse_file((capture / "result.toml").string());
    UNSCOPED_INFO("a complete capture of frames 11777..11780 armed on the body PS");
    REQUIRE((status["complete"].value_or(false) && status["payload_complete"].value_or(false) && status["trigger"].value_or(std::string()) == "pixel_digest" &&
             status["arm_pixel_digest"].value_or(std::string()) == std::string(kCapturePairs[0].pixelWords.substr(0, 8)) &&
             std::uint64_t(status["begin_frame"].value_or<std::int64_t>(0)) == kFirstFrame &&
             std::uint64_t(status["end_frame"].value_or<std::int64_t>(0)) == kFirstFrame + kFrames));
    std::vector<Draw> draws;
    const auto        table = toml::parse_file((capture / "events.toml").string());
    const auto*       all   = table["events"].as_array();
    UNSCOPED_INFO("capture events");
    REQUIRE(all != nullptr);
    for (const auto& node : *all)
    {
        const auto event = toml::node_view<const toml::node>(node);
        if (event["kind"].value_or(std::string()) != "draw")
            continue;
        Draw draw;
        draw.sequence   = std::uint64_t(event["sequence"].value_or<std::int64_t>(0));
        draw.frame      = std::uint64_t(event["frame"].value_or<std::int64_t>(0));
        draw.entry      = std::uint32_t(event["entry"].value_or<std::int64_t>(0));
        draw.caller     = static_cast<std::uint32_t>(std::stoull(event["caller_lr"].value_or(std::string("0"))));
        draw.stack      = std::uint32_t(event["raw_r1"].value_or<std::int64_t>(0));
        const auto* raw = event["raw_r3_r10_u64"].as_array();
        UNSCOPED_INFO("eight raw words");
        REQUIRE((raw && raw->size() == 8));
        for (std::size_t i = 0; i < 8; ++i)
            draw.arguments[i] = static_cast<std::uint32_t>(std::stoull((*raw)[i].value_or(std::string("0"))));
        if (draw.caller == kLeaderDrawCaller)
            draws.push_back(std::move(draw));
    }
    const auto  manifest = toml::parse_file((capture / "payload-manifest.toml").string());
    const auto* bindings = manifest["bindings"].as_array();
    UNSCOPED_INFO("payload manifest bindings");
    REQUIRE(bindings != nullptr);
    for (const auto& node : *bindings)
    {
        const auto& binding = *node.as_table();
        const auto  event   = std::uint64_t(binding["event"].value_or<std::int64_t>(-1));
        const auto  draw    = std::ranges::find(draws, event, &Draw::sequence);
        if (draw == draws.end())
            continue;
        UNSCOPED_INFO(std::format("a clean payload for draw {}", event));
        REQUIRE(binding["error"].value_or(std::string("x")).empty());
        draw->files.push_back({ binding["role"].value_or(std::string()), capture / binding["file"].value_or(std::string()), std::uint64_t(binding["address"].value_or<std::int64_t>(0)), binding["physical"].value_or(false) });
    }
    return draws;
}

std::vector<std::uint8_t> preState(const std::filesystem::path& capture, const Draw& draw)
{
    return read(capture / std::format("draw-{}.state.be.bin", draw.sequence));
}

std::vector<std::uint8_t> postState(const std::filesystem::path& capture, const Draw& draw)
{
    auto       state = preState(capture, draw);
    const auto delta = draw.payload("state_delta_offset_before_after_be32");
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
    for (const auto& file : draw.files)
        if (!file.role.starts_with("state_"))
            memory.Add(file.address, file.physical, read(file.path));
    memory.Add(draw.arguments[0], false, preState(capture, draw));
    return memory;
}

NativeDrawSubmission submissionOf(const Draw& draw)
{
    auto submission  = submissionFor(draw.arguments, draw.caller);
    submission.stack = draw.stack;
    return submission;
}

// Registers from the shadow by its own map, independent of GuestDrawRegisters.
std::vector<std::uint32_t> registersOf(std::span<const std::uint8_t> state)
{
    std::vector<std::uint32_t> registers(0x5003);
    for (const auto [from, to, count] : { std::array<unsigned, 3>{ 0x2880, 0x2000, 16 }, { 0x28C0, 0x2080, 3 }, { 0x28CC, 0x2100, 21 }, { 0x2920, 0x2180, 5 }, { 0x2934, 0x2200, 12 }, { 0x29B8, 0x2300, 38 }, { 0x780, 0x4000, 1024 }, { 0x1780, 0x4400, 896 }, { 0x2780, 0x4900, 40 } })
        for (unsigned i = 0; i < count; ++i)
            registers[to + i] = word(state, from + i * 4);
    return registers;
}

struct Error
{
    unsigned    maximum = 0;
    std::size_t over = 0, pixels = 0, worst = 0, sum = 0, uncovered = 0;
};

// The draws over a full-frame color, depth 1.0 and stencil 0 clear, resolved
// at guest samples 0 and 1 (host 0 and 3) through the gamma table.
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

// A draw's rasterized footprint: every alpha test passes, the source replaces
// the target and depth is off, so each covered sample takes the pixel
// shader's color whatever its alpha.
NativeDrawReplayRecipe footprintOf(NativeDrawReplayRecipe draw)
{
    const auto lowest = std::bit_cast<std::uint32_t>(-std::numeric_limits<float>::infinity());
    for (unsigned i = 0; i < 4; ++i)
        draw.sharedConstants[300 + i] = static_cast<std::uint8_t>(lowest >> (i * 8));
    draw.blend.enabled = false;
    draw.depth         = {};
    return draw;
}

// Which pixels a footprint covers in full, in part, or not at all.
struct Coverage
{
    std::vector<std::uint8_t> full, partial;
};

int run(const std::filesystem::path& capture, const std::filesystem::path& catalogDirectory)
{
    std::string       error;
    LeaderDrawCatalog catalog;
    UNSCOPED_INFO(std::string("leader catalog: ") + (error));
    REQUIRE(LoadLeaderDrawCatalog(catalogDirectory, catalog, error));
    std::array<const LeaderDrawShader*, 2>   entries{};
    std::array<std::vector<std::uint8_t>, 2> catalogVs;
    for (std::size_t p = 0; p < kCapturePairs.size(); ++p)
    {
        const auto& pair = kCapturePairs[p];
        entries[p]       = catalog.Find(pair.pixelWords);
        UNSCOPED_INFO(std::format("the catalog holds the {} pair", pair.name));
        REQUIRE((entries[p] && entries[p]->shader.vertexShaderHash == pair.vertexHash && entries[p]->shader.pixelShaderHash == pair.pixelHash &&
                 entries[p]->shader.vertexGuestWordDigest == pair.vertexWords && entries[p]->shader.textureDimensions.size() == pair.textures));
        const auto& vs = catalogVs[p] = read(catalogDirectory / std::format("{:016X}-{:016X}", pair.vertexHash, pair.pixelHash) / "vs.ucode.be.bin");
        UNSCOPED_INFO(std::format("the catalog's {} VS words are the pair's", pair.name));
        REQUIRE((XXH3_64bits(vs.data(), vs.size()) == pair.vertexHash && IndexedBufferMicrocodeWordDigest(vs) == pair.vertexWords));
    }

    // Census: each frame draws eight body meshes, then the glow, from the leader call site.
    const auto draws = loadDraws(capture);
    UNSCOPED_INFO("36 leader draws in four frames");
    REQUIRE(draws.size() == kFrames * 9);
    for (std::size_t i = 0; i < draws.size(); ++i)
    {
        UNSCOPED_INFO(std::format("draw {} is IndexedBuffer in frame order", draws[i].sequence));
        REQUIRE((draws[i].frame == kFirstFrame + i / 9 && draws[i].entry == 0x826A39F8 && (i == 0 || draws[i - 1].sequence < draws[i].sequence)));
    }
    std::cout << "Capture: " << draws.size() << " leader draws from 0x8250CB98 in frames 11777..11780\n";

    std::map<std::uint64_t, NativeDrawReplayRecipe> recipes;
    LeaderTextureCache                              cache;
    const auto                                      shaders = LeaderPixelShaders(catalog);
    for (std::size_t i = 0; i < draws.size(); ++i)
    {
        const auto&             draw   = draws[i];
        const bool              glow   = i % 9 == 8;
        const auto&             pair   = kCapturePairs[glow ? 1 : 0];
        const auto&             entry  = *entries[glow ? 1 : 0];
        const auto              name   = std::format("draw {} ({})", draw.sequence, pair.name);
        auto                    memory = memoryAt(capture, draw);
        IndexedBufferDrawInputs inputs;
        UNSCOPED_INFO(std::string(name + " reader: ") + (error));
        REQUIRE((ReadIndexedBufferDrawInputs(submissionOf(draw), readerFor(memory), shaders, inputs, error, IndexedBufferForm::Leader) && inputs.inputsComplete &&
                 catalog.entries[inputs.pixelShader].shader.pixelGuestWordDigest == pair.pixelWords));
        const auto  declaration = draw.payload("declaration");
        std::string missing;
        for (const auto& [field, equal] : std::initializer_list<std::pair<const char*, bool>>{
                 { " vs_code", inputs.vertexCode == draw.payload("vs_code") },
                 { " ps_code", inputs.pixelCode == draw.payload("ps_code") },
                 { " indices", indexBytes(inputs).size() == inputs.indexCount * 2U && startsWith(draw.payload("indices"), indexBytes(inputs)) },
                 { " vertices", inputs.streamBytes.size() == 1 && startsWith(draw.payload("vertices"), inputs.streamBytes[0]) },
                 { " fetch_records", inputs.fetchRecords == draw.payload("fetch_records") },
                 { " declaration", inputs.declaration == std::vector<std::uint8_t>(declaration.begin() + 52, declaration.end()) } })
            if (!equal)
                missing += field;
        UNSCOPED_INFO(name + " reader inputs differ from the capture payloads:" + missing);
        REQUIRE(missing.empty());
        // Each sampled span is one captured payload at the same address.
        const auto  bases = draw.all("sampled_base"), mips = draw.all("sampled_mip_base");
        std::size_t mipSpans = 0;
        for (std::size_t slot = 0; slot < inputs.textureRanges.size(); ++slot)
        {
            const auto& ranges = inputs.textureRanges[slot];
            const auto  base   = std::ranges::find(bases, std::uint64_t(ranges.base.address), &Draw::File::address);
            UNSCOPED_INFO(std::format("{} texture {} base is its payload's", name, slot));
            REQUIRE((base != bases.end() && startsWith(read((*base)->path), inputs.textureBaseBytes[slot])));
            if (ranges.mips.size)
            {
                const auto mip = std::ranges::find(mips, std::uint64_t(ranges.mips.address), &Draw::File::address);
                UNSCOPED_INFO(std::format("{} texture {} mips are their payload's", name, slot));
                REQUIRE((mip != mips.end() && startsWith(read((*mip)->path), inputs.textureMipBytes[slot])));
                ++mipSpans;
            }
        }
        UNSCOPED_INFO(name + " reads every sampled texture span");
        REQUIRE((inputs.textureRanges.size() == pair.textures && mipSpans == mips.size()));
        const auto vsLiterals = draw.payload("vs_literal_values:252"), psLiterals = draw.payload("ps_literal_values:508");
        bool       literal = vsLiterals.size() == 64 && psLiterals.size() == 64 && inputs.vertexLiteralRegisters.size() == 16 && inputs.pixelLiteralRegisters.size() == 16;
        for (std::uint32_t k = 0; literal && k < 16; ++k)
            literal = inputs.vertexLiteralRegisters[k] == std::make_pair(0x43F0U + k, word(vsLiterals, k * 4)) &&
                      inputs.pixelLiteralRegisters[k] == std::make_pair(0x47F0U + k, word(psLiterals, k * 4));
        UNSCOPED_INFO(name + " reads the VS c252..c255 and PS c508..c511 records");
        REQUIRE(literal);

        LeaderDrawRecipe recipe;
        UNSCOPED_INFO(std::string(name + " recipe: ") + (error));
        REQUIRE(BuildLeaderDrawRecipe(inputs, catalog, recipe, error, &cache));
        // The captured VS is already the linked, fetch-patched word set.
        UNSCOPED_INFO(name + " patches its fetch records to the captured and catalog VS words");
        REQUIRE((recipe.patchedVertexCode == inputs.vertexCode && recipe.patchedVertexCode == catalogVs[glow]));
        // As at the menu scene, Original may set RB_HIZCONTROL (0x2203) bit 0;
        // the fold keeps the entry word and no native draw state reads it.
        auto effective = GuestDrawRegisters(recipe.effectiveState), original = registersOf(postState(capture, draw));
        UNSCOPED_INFO(name + " differs from Original's HiZ word at most in bit 0");
        REQUIRE((effective.size() == original.size() && (effective[0x2203] | 1) == (original[0x2203] | 1)));
        effective[0x2203] = original[0x2203];
        std::string differing;
        for (std::size_t r = 0; r < effective.size(); ++r)
            if (effective[r] != original[r] && differing.size() < 300)
                differing += std::format(" {:#x}: {:#x} for {:#x}", r, effective[r], original[r]);
        UNSCOPED_INFO(name + " effective registers and constants equal Original's state:" + differing);
        REQUIRE(differing.empty());
        const auto& native = recipe.native;
        UNSCOPED_INFO(name + " binds the catalog DXIL");
        REQUIRE((std::ranges::equal(native.vertexShaderBytes(), entry.shader.vertexShaderDxil) && std::ranges::equal(native.pixelShaderBytes(), entry.shader.pixelShaderDxil)));
        UNSCOPED_INFO(name + " keeps its depth, blend, cull and 1280x351 viewport");
        REQUIRE((native.indexCount == inputs.indexCount && native.textureMask == (1U << pair.textures) - 1 && native.depth.enabled && native.depth.compare == 4 &&
                 native.depth.writeEnabled == !glow && native.blend.enabled &&
                 native.blend.destinationColor == (glow ? NativeDrawReplayBlendFactor::One : NativeDrawReplayBlendFactor::InverseSourceAlpha) &&
                 native.rasterizer.cull == 2 && native.viewport.height == 351));
        for (std::size_t slot = 0; slot < pair.textures; ++slot)
        {
            UNSCOPED_INFO(std::format("{} texture {} keeps its dimension and every level", name, slot));
            REQUIRE((native.textures[slot].dimension == entry.shader.textureDimensions[slot] &&
                     native.textures[slot].mipLevels == (word(inputs.state, 0x480 + slot * 24 + 16) >> 6 & 15) + 1));
        }
        recipes[draw.sequence] = native;

        // The registry's admission binds the same catalog to this memory.
        NativeDrawFamily registered;
        std::string      summary;
        UNSCOPED_INFO(std::string(name + " registry admission: ") + (error));
        REQUIRE((LeaderDrawFamily().admit(catalogDirectory, readerFor(memory), registered, summary, error) && registered &&
                 summary == std::format("({} pairs)", catalog.entries.size())));
        NativeDrawReplayRecipe registeredDraw;
        std::string            detail;
        UNSCOPED_INFO(std::string(name + " is Built by the registered leader family: ") + (detail));
        REQUIRE((registered(submissionOf(draw), registeredDraw, detail) == NativeDrawFamilyResult::Built && registeredDraw.indexCount == inputs.indexCount &&
                 registeredDraw.textureMask == native.textureMask));
        // The scene mesh call site's families find it foreign.
        auto boot = MakeBootDrawBuilder(readerFor(memory), nullptr, nullptr, nullptr, std::make_shared<NativeGameplayShaderInterface>(entries[0]->shader));
        UNSCOPED_INFO(name + " is foreign to the boot chain");
        REQUIRE((!boot(submissionOf(draw), registeredDraw, detail) && detail.empty()));
    }
    {
        // A body draw with a blend outside the leader forms is refused.
        IndexedBufferDrawInputs inputs;
        auto                    memory = memoryAt(capture, draws[0]);
        UNSCOPED_INFO("first draw");
        REQUIRE(ReadIndexedBufferDrawInputs(submissionOf(draws[0]), readerFor(memory), shaders, inputs, error, IndexedBufferForm::Leader));
        store(inputs.state, shadowOf(0x2201), 0x07010701);
        LeaderDrawRecipe rejected;
        UNSCOPED_INFO(std::string("an alpha-composite blend on a body draw is refused: ") + (error));
        REQUIRE((!BuildLeaderDrawRecipe(inputs, catalog, rejected, error) && error == "leader_blend_control: 0x7010701"));
    }
    std::cout << std::format("Capture leader draws against a {}-pair catalog: reader inputs equal the payloads, six- and eight-record patches give the captured VS, effective state is Original's\n",
                             catalog.entries.size());

    const auto referenceDirectory = environment("REREVVED_LEADER_REFERENCE");
    if (referenceDirectory.empty())
    {
        std::cout << "Set REREVVED_LEADER_REFERENCE to the civ select Xenos frame reference to compare the leader\n";
        return 77;
    }
    const auto directory = std::filesystem::path(referenceDirectory);
    const auto input     = directory / "input";
    const auto report    = toml::parse_file((directory / "reference.toml").string());
    const auto manifest  = toml::parse_file((input / "manifest.toml").string());
    UNSCOPED_INFO("a complete reference armed on the body PS");
    REQUIRE((report["status"].value_or(std::string()) == "complete" && report["arm_pixel_digest"].value_or(std::string()) == std::string(kCapturePairs[0].pixelWords.substr(0, 8))));
    UNSCOPED_INFO("a 1280x720 RGB10 output through the 256-entry gamma table");
    REQUIRE((manifest["output_format"].value_or<std::int64_t>(0) == 24 && manifest["output_width"].value_or<std::int64_t>(0) == 1280 &&
             manifest["output_height"].value_or<std::int64_t>(0) == 720 && !manifest["use_fxaa"].value_or(true) && !manifest["use_pwl_gamma"].value_or(true) &&
             manifest["is_8bpc"].value_or(false)));
    const auto file = [&](const toml::node_view<const toml::node>& entry)
    {
        return read(input / entry["file"].value_or(std::string()));
    };

    // Attribution: each Xenos leader event's constant banks (VS and PS c0..c255,
    // literal records included) equal exactly one capture recipe's.
    const auto* events = manifest["events"].as_array();
    UNSCOPED_INFO("frame events");
    REQUIRE(events != nullptr);
    std::vector<std::uint64_t> matched;
    for (const auto& node : *events)
    {
        const auto event  = toml::node_view<const toml::node>(node);
        const auto vertex = event["guest_vertex_hash"].value_or(std::string());
        if (vertex != std::format("{:016X}", kCapturePairs[0].vertexHash) && vertex != std::format("{:016X}", kCapturePairs[1].vertexHash))
            continue;
        const auto registers = file(event["registers"]);
        UNSCOPED_INFO("reference registers");
        REQUIRE(registers.size() == 0x5003 * 4);
        const auto                 vsBank = std::span(registers).subspan(0x4000 * 4, 4096), psBank = std::span(registers).subspan(0x4400 * 4, 4096);
        std::vector<std::uint64_t> hits;
        for (const auto& [sequence, native] : recipes)
            if (std::ranges::equal(native.vertexConstants, vsBank) && std::ranges::equal(native.pixelConstants, psBank))
                hits.push_back(sequence);
        const auto id = event["id"].value_or<std::int64_t>(0);
        UNSCOPED_INFO(std::format("Xenos event {} matches one capture recipe's constants ({} found)", id, hits.size()));
        REQUIRE(hits.size() == 1);
        matched.push_back(hits[0]);
        // Each texture use's SDK sampler (D3D12TextureCache::SamplerParameters)
        // has the recipe sampler's filters and anisotropy.
        const auto& native = recipes.at(hits[0]);
        const auto* uses   = event["texture_uses"].as_array();
        UNSCOPED_INFO(std::format("Xenos event {} records its texture uses", id));
        REQUIRE((uses && !uses->empty()));
        for (const auto& use : *uses)
        {
            const auto slot  = std::size_t(use.as_table()->at("fetch").value_or<std::int64_t>(-1));
            const auto value = std::uint32_t(use.as_table()->at("sampler").value_or<std::int64_t>(0));
            UNSCOPED_INFO(std::format("Xenos event {} samples fetch {} of the recipe", id, slot));
            REQUIRE(slot < native.samplers.size());
            const auto& host = native.samplers[slot];
            UNSCOPED_INFO(std::format("Xenos event {} fetch {} sampler {:#x} matches the recipe's", id, slot, value));
            REQUIRE((((value >> 11) & 1) == host.magLinear && ((value >> 12) & 1) == host.minLinear && ((value >> 13) & 1) == host.mipLinear &&
                     ((value >> 14) & 7) == host.anisotropy && ((value >> 17) & 15) == 0 && !((value >> 21) & 1)));
        }
    }
    // Tile 0 and tile 1 replay the same nine draws.
    UNSCOPED_INFO("both tiles draw the same nine leader draws");
    REQUIRE((matched.size() == 18 && std::equal(matched.begin(), matched.begin() + 9, matched.begin() + 9)));
    for (std::size_t i = 0; i < 9; ++i)
    {
        UNSCOPED_INFO("the reference frame shows capture frame 11777's leader draws in order");
        REQUIRE(matched[i] == draws[i].sequence);
    }
    std::cout << std::format("Xenos frame {} draws capture frame {}'s nine leader draws in both tiles, with equal constant banks\n",
                             report["start_frame"].value_or<std::int64_t>(0),
                             kFirstFrame);

    const auto reference = file(manifest["reference"]);
    UNSCOPED_INFO("the RGB10 reference matches its recorded digest");
    REQUIRE((reference.size() == 1280 * 720 * 4 && sha256(reference) == report["reference_sha256"].value_or(std::string())));
    const auto                     gammaBytes = file(manifest["gamma"]);
    std::array<std::uint32_t, 256> gamma{};
    UNSCOPED_INFO("a 256-entry gamma table");
    REQUIRE(gammaBytes.size() == gamma.size() * 4);
    for (std::size_t i = 0; i < gamma.size(); ++i)
        gamma[i] = little(gammaBytes, i * 4);

    if (environment("REREVVED_TEST_NATIVE_D3D12") != "1")
    {
        std::cout << "REREVVED_TEST_NATIVE_D3D12 is not 1; the leader was not compared with the Xenos frame\n";
        return 77;
    }
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
    {
        std::cout << "No D3D12 device; the leader was not compared with the Xenos frame\n";
        return 77;
    }
    std::vector<NativeDrawReplayRecipe> frame;
    for (std::size_t i = 0; i < 9; ++i)
        frame.push_back(recipes.at(draws[i].sequence));
    constexpr std::size_t kPixels = 1280 * 720, kTileColumns = 40, kTileRows = 11;
    // Coverage follows captured raster footprints; 2X and 4X sample positions
    // leave partial pixels out.
    std::vector<Coverage> coverage;
    for (const auto& draw : frame)
    {
        const auto alone = std::vector<NativeDrawReplayRecipe>{ footprintOf(draw) };
        const auto b = render(alone, { 0, 0, 0, 1 }, gamma), w = render(alone, { 1, 1, 1, 1 }, gamma);
        // The last pixel lies below the 1280x351 viewport.
        const auto clearBlack = little(b, (kPixels - 1) * 4), clearWhite = little(w, (kPixels - 1) * 4);
        Coverage   covered{ std::vector<std::uint8_t>(kPixels), std::vector<std::uint8_t>(kPixels) };
        for (std::size_t i = 0; i < kPixels; ++i)
        {
            const auto pb = little(b, i * 4), pw = little(w, i * 4);
            covered.full[i]    = pb == pw;
            covered.partial[i] = pb != pw && (pb != clearBlack || pw != clearWhite);
        }
        coverage.push_back(std::move(covered));
    }
    // The body mask: pixels a body draw covers in full that the glow does not
    // reach. The glow mask: pixels the glow covers in full over a body draw.
    std::vector<std::uint8_t> bodyMask(kPixels), glowMask(kPixels);
    std::size_t               edges = 0, outside = 0;
    for (std::size_t i = 0; i < kPixels; ++i)
    {
        bool partial = false, body = false;
        for (std::size_t d = 0; d < coverage.size(); ++d)
        {
            partial = partial || coverage[d].partial[i];
            body    = body || (d < 8 && coverage[d].full[i]);
        }
        const bool glowed = coverage[8].full[i] || coverage[8].partial[i];
        edges += partial && (body || glowed);
        bodyMask[i] = body && !partial && !glowed;
        glowMask[i] = body && !partial && coverage[8].full[i];
        outside += (bodyMask[i] || glowMask[i]) && i / 1280 >= 351;
    }
    const auto bodyPixels = std::ranges::count(bodyMask, std::uint8_t(1)), glowPixels = std::ranges::count(glowMask, std::uint8_t(1));
    std::cout << std::format("Footprints: {} body and {} glow mask pixels, {} draw-edge pixels excluded\n", bodyPixels, glowPixels, edges);
    UNSCOPED_INFO("the leader's footprint covers body and glow pixels, all inside the 1280x351 viewport");
    REQUIRE((bodyPixels > 10000 && glowPixels > 256 && outside == 0));

    // Xenos's alpha is the source's fourth byte. Body draws write their source
    // alpha, so where one draws last the pixel's stack is at least that opaque.
    const auto source = file(manifest["source"]);
    UNSCOPED_INFO("the Xenos source matches its recorded digest");
    REQUIRE((source.size() == kPixels * 4 && sha256(source) == report["source_sha256"].value_or(std::string())));

    // The menu backdrop's per-pixel bound: two gamma table steps, the largest
    // RGB10 span g[i + 2] - g[i].
    unsigned bound = 0;
    for (unsigned c = 0; c < 3; ++c)
        for (std::size_t i = 0; i + 2 < gamma.size(); ++i)
            bound = std::max(bound, ((gamma[i + 2] >> (c * 10)) & 1023) - ((gamma[i] >> (c * 10)) & 1023));
    // Each channel's gamma entries rise, so an RGB10 output names one level.
    std::array<std::array<std::uint32_t, 256>, 3> levels{};
    for (unsigned c = 0; c < 3; ++c)
        for (std::size_t i = 0; i < gamma.size(); ++i)
        {
            levels[c][i] = (gamma[i] >> (c * 10)) & 1023;
            UNSCOPED_INFO("the gamma table rises in every channel");
            REQUIRE((i == 0 || levels[c][i] > levels[c][i - 1]));
        }
    const auto level = [&](std::uint32_t pixel, unsigned c)
    {
        return unsigned(std::ranges::lower_bound(levels[c], (pixel >> (c * 10)) & 1023) - levels[c].begin());
    };

    // Black/white replay bounds translucent pixels; opacity below Xenos alpha
    // by kAlphaSlack marks uncovered.
    struct Comparison
    {
        std::vector<unsigned>     errors;
        std::vector<std::uint8_t> uncovered;
    };

    constexpr unsigned kAlphaSlack = 13;
    const auto         compare     = [&](const std::vector<NativeDrawReplayRecipe>& draws)
    {
        const auto b = render(draws, { 0, 0, 0, 1 }, gamma), w = render(draws, { 1, 1, 1, 1 }, gamma);
        Comparison result{ std::vector<unsigned>(kPixels), std::vector<std::uint8_t>(kPixels) };
        for (std::size_t i = 0; i < kPixels; ++i)
        {
            const auto pb = little(b, i * 4), pw = little(w, i * 4), pr = little(reference, i * 4);
            unsigned   span = 0;
            for (unsigned c = 0; c < 3; ++c)
            {
                const auto x = (pb >> (c * 10)) & 1023, y = (pw >> (c * 10)) & 1023, r = (pr >> (c * 10)) & 1023;
                const auto lo = std::min(x, y), hi = std::max(x, y);
                result.errors[i] = std::max(result.errors[i], r < lo ? lo - r : r > hi ? r - hi
                                                                                       : 0u);
                const auto lb = level(pb, c), lw = level(pw, c);
                span = std::max(span, lw > lb ? lw - lb : lb - lw);
            }
            result.uncovered[i] = 255 - span + kAlphaSlack < source[i * 4 + 3];
        }
        return result;
    };

    // Edge criteria bound mean error, per-pixel excess and uncovered shares
    // across masks and tiles.
    struct Limit
    {
        std::size_t meanTimes100 = 0, overPerMille = 0, uncoveredPerMille = 0;
    };

    constexpr Limit       kBody{ 400, 60, 40 }, kGlow{ 600, 150, 40 }, kDraw{ 1500, 250, 200 }, kTile{ 2000, 300, 1000 };
    constexpr std::size_t kLocalPixels = 256;
    const auto            exceeds      = [](const Error& error, const Limit& limit)
    {
        return error.sum * 100 > limit.meanTimes100 * error.pixels || error.over * 1000 > limit.overPerMille * error.pixels ||
               error.uncovered * 1000 > limit.uncoveredPerMille * error.pixels;
    };
    const auto describe = [&](const Error& error)
    {
        return std::format("mean {:.2f}, {} of {} above {}, {} uncovered, max {} at ({}, {})",
                           double(error.sum) / double(std::max<std::size_t>(error.pixels, 1)),
                           error.over,
                           error.pixels,
                           bound,
                           error.uncovered,
                           error.maximum,
                           error.worst % 1280,
                           error.worst / 1280);
    };
    // The first criterion a comparison exceeds, or empty.
    const auto judge = [&](const Comparison& comparison, bool verbose)
    {
        Error                body, glow;
        std::array<Error, 8> draws{};
        std::vector<Error>   tiles(kTileColumns * kTileRows);
        const auto           add = [&](Error& error, std::size_t i)
        {
            const auto value = comparison.errors[i];
            ++error.pixels;
            error.over += value > bound;
            error.sum += value;
            error.uncovered += comparison.uncovered[i];
            if (value > error.maximum)
            {
                error.maximum = value;
                error.worst   = i;
            }
        };
        for (std::size_t i = 0; i < kPixels; ++i)
        {
            if (glowMask[i])
                add(glow, i);
            if (!bodyMask[i])
                continue;
            add(body, i);
            add(tiles[(i / 1280 / 32) * kTileColumns + (i % 1280) / 32], i);
            for (std::size_t d = 0; d < draws.size(); ++d)
                if (coverage[d].full[i])
                    add(draws[d], i);
        }
        // The local error with the largest mean, then the largest uncovered share.
        const auto worst = [&](std::span<const Error> locals, bool byCoverage)
        {
            const Error* result = nullptr;
            for (const auto& local : locals)
                if (local.pixels >= kLocalPixels &&
                    (!result || (byCoverage ? local.uncovered * result->pixels > result->uncovered * local.pixels : local.sum * result->pixels > result->sum * local.pixels)))
                    result = &local;
            return result ? describe(*result) : std::string("none");
        };
        if (verbose)
            std::cout << std::format("Body mask: {}\nGlow mask: {}\nWorst body draw: {}; by coverage: {}\nWorst tile: {}; by coverage: {}\n",
                                     describe(body),
                                     describe(glow),
                                     worst(draws, false),
                                     worst(draws, true),
                                     worst(tiles, false),
                                     worst(tiles, true));
        if (exceeds(body, kBody))
            return std::string("the body mask");
        if (exceeds(glow, kGlow))
            return std::string("the glow mask");
        for (std::size_t d = 0; d < draws.size(); ++d)
            if (draws[d].pixels >= kLocalPixels && exceeds(draws[d], kDraw))
                return std::format("body draw {}", d);
        for (std::size_t t = 0; t < tiles.size(); ++t)
            if (tiles[t].pixels >= kLocalPixels && exceeds(tiles[t], kTile))
                return std::format("the tile at ({}, {})", t % kTileColumns * 32, t / kTileColumns * 32);
        return std::string();
    };
    const auto measured = compare(frame);
    const auto verdict  = judge(measured, true);
    const auto show     = [](const Limit& limit)
    {
        return std::format("mean {:.2f}, {:.1f}% above and {:.1f}% uncovered", limit.meanTimes100 / 100.0, limit.overPerMille / 10.0, limit.uncoveredPerMille / 10.0);
    };
    std::cout << std::format("Native leader against Xenos frame {} (bound {}): body {}; glow {}; each draw {}; each tile {}\n",
                             report["start_frame"].value_or<std::int64_t>(0),
                             bound,
                             show(kBody),
                             show(kGlow),
                             show(kDraw),
                             show(kTile));
    UNSCOPED_INFO("the native leader exceeds the RGB10 error criterion against Xenos over " + verdict);
    REQUIRE(verdict.empty());

    // The criteria detect a missing body draw (the largest), a missing glow,
    // 240 body pixels off by 100 within one tile, 300 spread over one draw,
    // and 300 uncovered pixels spread over one draw.
    std::size_t largest = 0;
    for (std::size_t i = 1; i < 8; ++i)
        if (frame[i].indexCount > frame[largest].indexCount)
            largest = i;
    for (const auto& [name, omitted] : { std::pair{ "the largest body draw", largest }, std::pair{ "the glow", std::size_t(8) } })
    {
        auto without = frame;
        without.erase(without.begin() + std::ptrdiff_t(omitted));
        const auto failed = judge(compare(without), false);
        std::cout << std::format("Without {}: exceeds over {}\n", name, failed);
        UNSCOPED_INFO(std::format("the error criteria detect a frame without {}", name));
        REQUIRE(!failed.empty());
    }
    std::vector<std::size_t> busiest, sparsest;
    for (std::size_t t = 0; t < kTileColumns * kTileRows; ++t)
    {
        std::vector<std::size_t> tile;
        for (std::size_t y = t / kTileColumns * 32; y < std::min<std::size_t>(t / kTileColumns * 32 + 32, 351); ++y)
            for (std::size_t x = t % kTileColumns * 32; x < t % kTileColumns * 32 + 32; ++x)
                if (bodyMask[y * 1280 + x])
                    tile.push_back(y * 1280 + x);
        if (tile.size() > busiest.size())
            busiest = std::move(tile);
    }
    for (std::size_t d = 0; d < 8; ++d)
    {
        std::vector<std::size_t> covered;
        for (std::size_t i = 0; i < kPixels; ++i)
            if (bodyMask[i] && coverage[d].full[i])
                covered.push_back(i);
        if (covered.size() >= 300 && (sparsest.empty() || covered.size() < sparsest.size()))
            sparsest = std::move(covered);
    }
    UNSCOPED_INFO("a tile and a draw hold 300 mask pixels");
    REQUIRE((busiest.size() >= 300 && sparsest.size() >= 300));
    for (const auto& [name, pixels, uncover, count] : { std::tuple{ "one tile", &busiest, false, std::size_t(240) }, std::tuple{ "one draw", &sparsest, false, std::size_t(300) }, std::tuple{ "one draw", &sparsest, true, std::size_t(300) } })
    {
        auto perturbed = measured;
        for (std::size_t k = 0; k < count; ++k)
        {
            const auto i = (*pixels)[k * pixels->size() / count];
            if (uncover)
                perturbed.uncovered[i] = 1;
            else
                perturbed.errors[i] = 100;
        }
        const auto failed = judge(perturbed, false);
        const auto what   = uncover ? "uncovered" : "off by 100";
        std::cout << std::format("{} pixels {} within {} of {} mask pixels: exceeds over {}\n", count, what, name, pixels->size(), failed);
        UNSCOPED_INFO(std::format("the error criteria detect {} pixels {} within {}", count, what, name));
        REQUIRE(!failed.empty());
    }
    return 0;
}

} // namespace capture

// Civ-select shader harvest checks catalog admission and recipe structure
// without frame comparison.
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

// The guest's static swizzle and format tables, as the leader capture holds them.
std::pair<GuestBytes, GuestBytes> guestTables(const std::filesystem::path& capture)
{
    const auto draws = capture::loadDraws(capture);
    UNSCOPED_INFO("the leader capture holds draws");
    REQUIRE(!draws.empty());
    auto       memory = capture::memoryAt(capture, draws[0]);
    const auto guest  = readerFor(memory);
    return { guest(0x82091394, 16, false), guest(0x820913A8, 64, false) };
}

// A harvested key record as reader inputs for `pixelShader` from `caller`.
IndexedBufferDrawInputs recordInputs(const std::filesystem::path& directory, const toml::node_view<const toml::node>& record, const std::string& name, std::size_t pixelShader, std::uint32_t caller, const std::pair<GuestBytes, GuestBytes>& tables)
{
    std::string                  error;
    IndexedBufferDrawInputs      inputs;
    std::array<std::uint32_t, 8> r{};
    for (std::size_t i = 0; i < r.size(); ++i)
        r[i] = address(record["arguments_r3_r10"][i]);
    inputs.sceneCandidate     = true;
    inputs.inputsComplete     = true;
    inputs.pixelShader        = pixelShader;
    inputs.primitive          = r[1];
    inputs.baseVertex         = r[2];
    inputs.startIndex         = r[3];
    inputs.indexCount         = r[4];
    inputs.state              = read(directory / "state.be.bin");
    inputs.vertexCode         = read(directory / "vs_code.be.bin");
    inputs.pixelCode          = read(directory / "ps_code.be.bin");
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
    const auto stride = std::uint32_t(inputs.state.at(0x30F0)) * 4;
    inputs.streams    = { 0 };
    inputs.streamFetches.push_back({ word(inputs.state, 0x778), word(inputs.state, 0x77C) });
    // Vertex fetch 95 spans its dword count from the shadow.
    inputs.streamBytes.emplace_back(std::size_t((inputs.streamFetches[0][1] >> 2) & 0xFFFFFF) * 4);
    const auto* textures = record["pixel_textures"].as_array();
    UNSCOPED_INFO(name + " pixel textures");
    REQUIRE(textures != nullptr);
    for (const auto& node : *textures)
    {
        const auto texture = toml::node_view<const toml::node>(node);
        UNSCOPED_INFO(name + " texture slots in order");
        REQUIRE(std::size_t(texture["slot"].value_or<std::int64_t>(-1)) == inputs.textureFetches.size());
        auto& fetch = inputs.textureFetches.emplace_back();
        for (std::size_t i = 0; i < fetch.size(); ++i)
            fetch[i] = address(texture["words"][i]);
        auto& ranges = inputs.textureRanges.emplace_back();
        UNSCOPED_INFO(std::string(name + " texture ranges: ") + (error));
        REQUIRE(GetNativeTextureMemoryRanges(fetch, ranges, error));
        inputs.textureBaseBytes.emplace_back(ranges.base.size);
        inputs.textureMipBytes.emplace_back(ranges.mips.size);
    }
    inputs.arguments = { r[0], r[1], 0, 0, r[4], 0, 0, 0, stride, caller };
    return inputs;
}

int run(const std::filesystem::path& root, const std::filesystem::path& catalogDirectory, const std::filesystem::path& capture)
{
    SceneMenuDrawCatalog menu;
    std::string          error;
    UNSCOPED_INFO(std::string("the menu catalog loads with band pairs: ") + (error));
    REQUIRE((LoadSceneMenuDrawCatalog(catalogDirectory, menu, error) && !menu.band->entries.empty()));
    const auto tables = guestTables(capture);

    std::vector<std::string> built;
    const auto               index = toml::parse_file((root / "index.toml").string());
    const auto*              pairs = index["pairs"].as_array();
    UNSCOPED_INFO("harvest pairs");
    REQUIRE(pairs != nullptr);
    for (const auto& row : *pairs)
    {
        const auto view = toml::node_view<const toml::node>(row);
        if (view["caller"].value_or(std::string()) != "0x8250C768")
            continue;
        const auto  directory = root / view["directory"].value_or(std::string());
        const auto  table     = toml::parse_file((directory / "record.toml").string());
        const auto  record    = toml::node_view<const toml::node>(table);
        const auto* entry     = menu.band->Find(record["pixel_word_sha256"].value_or(std::string()));
        if (!entry)
            continue;
        const auto name   = view["directory"].value_or(std::string());
        const auto inputs = recordInputs(directory, record, name, static_cast<std::size_t>(entry - menu.band->entries.data()), kSceneMeshDrawCaller, tables);

        LeaderDrawRecipe recipe;
        UNSCOPED_INFO(std::string(name + " builds through the band catalog: ") + (error));
        REQUIRE(BuildLeaderDrawRecipe(inputs, *menu.band, recipe, error));
        UNSCOPED_INFO(name + " re-patches to its harvested VS words");
        REQUIRE(recipe.patchedVertexCode == inputs.vertexCode);
        const auto& native = recipe.native;
        std::cout << std::format("{}: {} indices, {} attributes, {} textures (mask {:#x}), viewport {}x{}, depth write {}, blend {}/{}\n",
                                 name,
                                 native.indexCount,
                                 native.vertexAttributeCount,
                                 inputs.textureFetches.size(),
                                 native.textureMask,
                                 native.viewport.width,
                                 native.viewport.height,
                                 native.depth.writeEnabled,
                                 int(native.blend.sourceColor),
                                 int(native.blend.destinationColor));
        built.push_back(name);
    }
    UNSCOPED_INFO(std::format("each of the {} band pairs is harvested and built ({} built)", menu.band->entries.size(), built.size()));
    REQUIRE(built.size() == menu.band->entries.size());
    std::cout << std::format("Civ select harvest: {} band pairs build from their records through the scene-caller leader catalog\n", built.size());
    return 0;
}

// 20261015: first full-window frame 1978 and a two-leader frame with mipped
// body texture. 20261014: no texture bytes; full-window and band frames.
struct WorldFrame
{
    std::string_view instance;
    std::size_t      draws  = 0;
    bool             window = false, texels = false;
};

constexpr std::array<WorldFrame, 4> kWorldFrames{ {
    { "13698FB36E7D73F0-0", 9, true, true },
    { "22C51CE191620A47-0", 18, true, true },
    { "10F26E9B11C30408-0", 9, true, false },
    { "65F635C045DC7E08-0", 7, false, false },
} };

// A frame draw over its key record's inputs: its own device block,
// arguments, indices, stream 0 and texture fetches, with their recorded bytes
// when `texels` holds and zero texels otherwise.
IndexedBufferDrawInputs frameDrawInputs(const std::filesystem::path& root, const toml::node_view<const toml::node>& event, IndexedBufferDrawInputs inputs, const std::string& name, bool texels)
{
    std::string error;
    inputs.state = read(root / event["state"].value_or(std::string()));
    std::array<std::uint32_t, 8> r{};
    for (std::size_t i = 0; i < r.size(); ++i)
        r[i] = address(event["arguments_r3_r10"][i]);
    inputs.primitive  = r[1];
    inputs.baseVertex = r[2];
    inputs.startIndex = r[3];
    inputs.indexCount = r[4];
    inputs.arguments  = { r[0], r[1], 0, 0, r[4], 0, 0, 0, std::uint32_t(inputs.state.at(0x30F0)) * 4, inputs.arguments.caller };
    std::map<std::string, std::vector<std::uint8_t>> bytes;
    const auto*                                      memory = event["memory"].as_array();
    UNSCOPED_INFO(name + " memory");
    REQUIRE(memory != nullptr);
    for (const auto& row : *memory)
    {
        const auto entry = toml::node_view<const toml::node>(row);
        const auto file  = entry["file"].value_or(std::string());
        // A small row keeps its bytes inline; a larger one names a payload.
        bytes[entry["role"].value_or(std::string())] = file.empty() ? hexBytes(entry["bytes"].value_or(std::string())) : read(root / file);
    }
    UNSCOPED_INFO(name + " records indices, stream 0 and the index buffer");
    REQUIRE((bytes.contains("indices") && bytes.contains("stream 0") && bytes.contains("index_buffer")));
    inputs.indexBytes       = bytes["indices"];
    inputs.indexFormat      = word(bytes["index_buffer"], 0) >> 31;
    inputs.indexEndian      = (word(bytes["index_buffer"], 0) >> 29) & 3;
    inputs.streamFetches[0] = { word(inputs.state, 0x778), word(inputs.state, 0x77C) };
    inputs.streamBytes[0]   = bytes["stream 0"];
    for (std::size_t slot = 0; slot < inputs.textureFetches.size(); ++slot)
    {
        auto& fetch = inputs.textureFetches[slot];
        for (std::size_t i = 0; i < fetch.size(); ++i)
            fetch[i] = word(inputs.state, 0x480 + slot * 24 + i * 4);
        auto& ranges = inputs.textureRanges[slot];
        UNSCOPED_INFO(std::string(name + " texture range: ") + (error));
        REQUIRE(GetNativeTextureMemoryRanges(fetch, ranges, error));
        // The harvest records each span from its start; the reader reads its layout.
        for (const auto& [part, size, target] : { std::tuple{ "base", ranges.base.size, &inputs.textureBaseBytes[slot] }, std::tuple{ "mips", ranges.mips.size, &inputs.textureMipBytes[slot] } })
        {
            if (!size || !texels)
            {
                *target = GuestBytes(std::size_t(size));
                continue;
            }
            const auto& recorded = bytes[std::format("texture {} {}", slot, part)];
            UNSCOPED_INFO(std::format("{} records texture {} {} ({} of {} bytes)", name, slot, part, recorded.size(), size));
            REQUIRE(recorded.size() >= size);
            *target = std::vector<std::uint8_t>(recorded.begin(), recorded.begin() + std::ptrdiff_t(size));
        }
    }
    return inputs;
}

// Catalog leader pairs need at least one full-window form.
int world(const std::vector<std::filesystem::path>& roots, const std::filesystem::path& catalogDirectory, const std::filesystem::path& capture)
{
    LeaderDrawCatalog catalog;
    std::string       error;
    UNSCOPED_INFO(std::string("the leader catalog loads: ") + (error));
    REQUIRE((LoadLeaderDrawCatalog(catalogDirectory, catalog, error) && !catalog.entries.empty()));
    const auto  tables = guestTables(capture);
    std::size_t full = 0, band = 0;
    for (const auto& root : roots)
    {
        const auto  index = toml::parse_file((root / "index.toml").string());
        const auto* pairs = index["pairs"].as_array();
        UNSCOPED_INFO("harvest pairs");
        REQUIRE(pairs != nullptr);
        for (const auto& row : *pairs)
        {
            const auto view = toml::node_view<const toml::node>(row);
            if (view["caller"].value_or(std::string()) != "0x8250CB98")
                continue;
            const auto  directory = root / view["directory"].value_or(std::string());
            const auto  table     = toml::parse_file((directory / "record.toml").string());
            const auto  record    = toml::node_view<const toml::node>(table);
            const auto* entry     = catalog.Find(record["pixel_word_sha256"].value_or(std::string()));
            if (!entry)
                continue;
            const auto       name   = root.filename().string() + "/" + view["directory"].value_or(std::string());
            const auto       inputs = recordInputs(directory, record, name, static_cast<std::size_t>(entry - catalog.entries.data()), kLeaderDrawCaller, tables);
            LeaderDrawRecipe recipe;
            UNSCOPED_INFO(std::string(name + " builds through the leader catalog: ") + (error));
            REQUIRE(BuildLeaderDrawRecipe(inputs, catalog, recipe, error));
            const auto& native = recipe.native;
            const bool  window = native.viewport.height == 720 && native.scissor.bottom == 720;
            UNSCOPED_INFO(name + " draws one of the two target forms");
            REQUIRE((native.viewport.width == 1280 && native.scissor.right == 1280 && (window || (native.viewport.height == 351 && native.scissor.bottom == 351))));
            ++(window ? full : band);
            std::cout << std::format("{}: {} indices, {} textures, viewport {}x{}\n", name, native.indexCount, inputs.textureFetches.size(), native.viewport.width, native.viewport.height);
        }
    }
    UNSCOPED_INFO("a world harvest record takes the full-window form");
    REQUIRE(full > 0);
    std::cout << std::format("World harvests: {} full-window and {} band leader records build through the leader catalog\n", full, band);

    for (const auto& root : roots)
    {
        std::size_t frames = 0;
        for (const auto& fixture : kWorldFrames)
        {
            const auto path = root / "frames" / std::string(fixture.instance) / "frame.toml";
            if (!std::filesystem::exists(path))
                continue;
            ++frames;
            const auto  frame  = toml::parse_file(path.string());
            const auto* events = frame["events"].as_array();
            UNSCOPED_INFO("frame events");
            REQUIRE(events != nullptr);
            std::map<std::string, IndexedBufferDrawInputs> records;
            std::size_t                                    draws = 0;
            for (const auto& node : *events)
            {
                const auto event = toml::node_view<const toml::node>(node);
                const auto pair  = event["pair"].value_or(std::string());
                if (event["kind"].value_or(std::string()) != "draw" || !pair.starts_with("8250CB98-"))
                    continue;
                if (!records.contains(pair))
                {
                    const auto  table  = toml::parse_file((root / pair / "record.toml").string());
                    const auto  record = toml::node_view<const toml::node>(table);
                    const auto* entry  = catalog.Find(record["pixel_word_sha256"].value_or(std::string()));
                    records.emplace(pair, entry ? recordInputs(root / pair, record, pair, static_cast<std::size_t>(entry - catalog.entries.data()), kLeaderDrawCaller, tables) : IndexedBufferDrawInputs{});
                }
                const auto& base = records.at(pair);
                if (!base.inputsComplete)
                    continue;
                const auto       name   = std::format("{} draw {}", fixture.instance, event["index"].value_or<std::int64_t>(-1));
                const auto       inputs = frameDrawInputs(root, event, base, name, fixture.texels);
                LeaderDrawRecipe recipe;
                UNSCOPED_INFO(std::string(name + " builds through the leader catalog: ") + (error));
                REQUIRE(BuildLeaderDrawRecipe(inputs, catalog, recipe, error));
                const auto height = fixture.window ? 720 : 351;
                UNSCOPED_INFO(name + " draws its frame's target form");
                REQUIRE((recipe.native.viewport.height == height && recipe.native.scissor.bottom == height));
                ++draws;
            }
            UNSCOPED_INFO(std::format("{} draws {} leader draws ({} built)", fixture.instance, fixture.draws, draws));
            REQUIRE(draws == fixture.draws);
            std::cout << std::format("World frame {}: {} leader draws build with {}, viewport 1280x{}\n", fixture.instance, draws, fixture.texels ? "their recorded texels" : "zero texels", fixture.window ? 720 : 351);
        }
        UNSCOPED_INFO(root.string() + " holds a named leader frame");
        REQUIRE(frames > 0);
    }
    return 0;
}

} // namespace harvest

} // namespace

// A check returns 77 when its D3D12 part could not run.
static void skipIfUnavailable(int status, const char* message)
{
    if (status == 77)
    {
        SKIP(message);
    }
}

TEST_CASE("leader_draw_adapter", "[ctest][leader-draw-adapter][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    unit();
}

TEST_CASE("leader_draw_adapter_capture", "[.][renderer-research][leader-draw-adapter][native_gpu][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    // Only an unset capture skips; a set but unusable path fails.
    const auto capture = environment("REREVVED_LEADER_CAPTURE");
    if (capture.empty())
    {
        SKIP("Set REREVVED_LEADER_CAPTURE to the civ select capture directory to run these checks");
    }
    const auto catalog = environment("REREVVED_LEADER_SHADERS");
    UNSCOPED_INFO("REREVVED_LEADER_CAPTURE is set; the leader checks also need REREVVED_LEADER_SHADERS");
    REQUIRE(!catalog.empty());
    skipIfUnavailable(capture::run(capture, catalog), "no D3D12 device for the leader capture");
}

TEST_CASE("leader_draw_adapter_harvest", "[.][renderer-research][leader-draw-adapter][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    // Only an unset harvest skips; it also needs the menu catalog and
    // the leader capture's static tables.
    const auto root = environment("REREVVED_CIVSELECT_HARVEST");
    if (root.empty())
    {
        SKIP("Set REREVVED_CIVSELECT_HARVEST to the civ select shader harvest to run these checks");
    }
    const auto catalog = environment("REREVVED_MENU_SCENE_SHADERS"), leaderCapture = environment("REREVVED_LEADER_CAPTURE");
    UNSCOPED_INFO("REREVVED_CIVSELECT_HARVEST is set; the harvest checks also need REREVVED_MENU_SCENE_SHADERS and REREVVED_LEADER_CAPTURE");
    REQUIRE((!catalog.empty() && !leaderCapture.empty()));
    harvest::run(root, catalog, leaderCapture);
}

TEST_CASE("leader_draw_adapter_world", "[.][renderer-research][leader-draw-adapter][timeout-300]")
{
    rerevved::tests::ScopedLogging     logging;
    std::vector<std::filesystem::path> roots;
    for (const auto* name : { "REREVVED_WORLD_HARVEST", "REREVVED_WORLD_HARVEST3" })
        if (const auto root = environment(name); !root.empty())
            roots.emplace_back(root);
    if (roots.empty())
    {
        SKIP("Set REREVVED_WORLD_HARVEST or REREVVED_WORLD_HARVEST3 to a world shader harvest to run these checks");
    }
    const auto catalog = environment("REREVVED_LEADER_SHADERS"), leaderCapture = environment("REREVVED_LEADER_CAPTURE");
    UNSCOPED_INFO("a world harvest is set; the world checks also need REREVVED_LEADER_SHADERS and REREVVED_LEADER_CAPTURE");
    REQUIRE((!catalog.empty() && !leaderCapture.empty()));
    harvest::world(roots, catalog, leaderCapture);
}
