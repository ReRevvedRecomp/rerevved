#include "gpu/d3d12/native_renderer_d3d12.h"
#include "gpu/d3d12/native_texture_upload.h"
#include "gpu/guest_d3d/gfx_draw_adapter.h"
#include "gpu/guest_d3d/leader_draw_adapter.h"
#include "logging_scope.hpp"
#include "resource_mirror_fixture.h"
#include "stream_queue_fixture.h"

#include <rex/graphics/xenos.h>
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
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <toml++/toml.hpp>

using namespace rerevved::gpu;
using namespace rerevved::gpu::guest_d3d;

namespace
{

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
};

constexpr std::uint32_t kStack = 0x70000000;

void addStride(FakeMemory& memory, std::uint32_t stride)
{
    std::vector<std::uint8_t> bytes(4);
    store(bytes, 0, stride);
    memory.Add(kStack + 84, false, bytes);
}

NativeDrawSubmission gfxSubmission(const std::array<std::uint32_t, 8>& arguments, std::uint32_t caller = kGfxDrawCaller)
{
    NativeDrawSubmission submission;
    submission.hook      = NativeDrawHook::IndexedUp;
    submission.caller    = caller;
    submission.stack     = kStack;
    submission.arguments = arguments;
    return submission;
}

// A pixel shader object whose microcode is `code`: header at 0x40000000,
// metadata after it, code at physical 0x1000.
void addPixelShader(FakeMemory& memory, std::vector<std::uint8_t> state, std::span<const std::uint8_t> code)
{
    constexpr std::uint32_t object = 0x40000000;
    store(state, 0x3194, object);
    store(state, 0x3198, 0x40001000);
    memory.Add(0x50000000, false, state);
    std::vector<std::uint8_t> header(68), metadata(32);
    store(header, 24, 0x1000); // resource base, physical
    store(header, 64, 100);    // metadata at object + 100 + 40
    store(metadata, 0, 0);
    store(metadata, 4, static_cast<std::uint32_t>(code.size()));
    memory.Add(object, false, header);
    memory.Add(object + 140, false, metadata);
    memory.Add(0x1000, true, { code.begin(), code.end() });
}

void unit()
{
    std::string error;
    FakeMemory  memory;
    ReadMemory  read = [&](std::uint64_t address, std::uint64_t size, bool physical)
    {
        return memory.Read(address, size, physical);
    };
    const std::array<std::uint32_t, 8> tuple{ 0x50000000, 4, 0, 12, 30, 0x50001000, 1, 0x50002000 };
    GfxDrawInputs                      inputs;
    // A catalog with the loading pair's identity only; the reader consults
    // its pixel digests and strides, never its DXIL.
    GfxDrawCatalog catalog;
    catalog.entries.push_back({});
    catalog.entries[0].shader.pixelGuestWordDigest = std::string(kGfxPixelWordDigest);
    catalog.entries[0].stride                      = kGfxStride;
    catalog.entries[0].attributeOffsets.assign(kGfxAttributeOffsets.begin(), kGfxAttributeOffsets.end());
    catalog.entries[0].textures = 1;

    // Selection happens before any shader read: hook, caller, then stride.
    auto other = gfxSubmission(tuple);
    other.hook = NativeDrawHook::NonindexedUp;
    UNSCOPED_INFO("another hook is foreign without reads");
    REQUIRE((!ReadGfxDrawInputs(other, read, catalog, inputs, error) && error.empty() && memory.reads == 0));
    UNSCOPED_INFO("the other panel call site is foreign without reads");
    REQUIRE((!ReadGfxDrawInputs(gfxSubmission(tuple, 0x82303E40), read, catalog, inputs, error) && error.empty() && memory.reads == 0));
    UNSCOPED_INFO("an unreadable stride at r1+84 is named: " + error);
    REQUIRE((!ReadGfxDrawInputs(gfxSubmission(tuple), read, catalog, inputs, error) && error == "gfx_input_extent"));
    // The pixel shader's catalog entry selects; stride is independent.
    addStride(memory, 8);
    UNSCOPED_INFO("a stride-8 call reads on to its state: " + error);
    REQUIRE((!ReadGfxDrawInputs(gfxSubmission(tuple), read, catalog, inputs, error) && error == "gfx_shadow_extent" && !inputs.gfxCandidate));
    memory.regions.clear();
    addStride(memory, 12);
    SolidDrawInputs panel;
    UNSCOPED_INFO("the panel reader keeps returning foreign for stride 12");
    REQUIRE((!ReadSolidPanelInputs(gfxSubmission(tuple), read, panel, error) && error.empty() && !panel.solidCandidate));
    UNSCOPED_INFO("a stride-12 call with unreadable state is named: " + error);
    REQUIRE((!ReadGfxDrawInputs(gfxSubmission(tuple), read, catalog, inputs, error) && error == "gfx_shadow_extent"));

    // A pixel shader outside the catalog is refused with the words and stride
    // its catalog entry would need; this VS object is unreadable.
    std::vector<std::uint8_t> state(0x3500), foreignCode(96, 0x5A);
    addPixelShader(memory, state, foreignCode);
    const auto unpinned = std::format("gfx_shader_unpinned ps={} vs=unreadable stride=12", GfxMicrocodeWordDigest(foreignCode));
    UNSCOPED_INFO("another pixel shader is unpinned: " + error);
    REQUIRE((!ReadGfxDrawInputs(gfxSubmission(tuple), read, catalog, inputs, error) && error == unpinned && !inputs.gfxCandidate));
    // A catalog pair at another stride is a candidate refused by stride.
    auto strideFour                                   = catalog;
    strideFour.entries[0].shader.pixelGuestWordDigest = GfxMicrocodeWordDigest(foreignCode);
    strideFour.entries[0].stride                      = 4;
    UNSCOPED_INFO("a pinned pixel shader at another stride is refused: " + error);
    REQUIRE((!ReadGfxDrawInputs(gfxSubmission(tuple), read, strideFour, inputs, error) && error == "gfx_stride_mismatch" && inputs.gfxCandidate));

    // The loading logo fetch: tiled k_8_8_8_8 1024x256, endian 8in32, one level.
    const std::array<std::uint32_t, 6> fetch{ 0x88004802, 0x1BBBA086, 0x001FE3FF, 0x00A80C14, 0x003F0003, 0x00000218 };
    NativeTextureMemoryRange           range;
    UNSCOPED_INFO("the logo range is the 1 MiB base level at 1BBBA000: " + error);
    REQUIRE((GetNativeTextureMemoryRange(fetch, range, error) && range.address == 0x1BBBA000 && range.size == 1024 * 256 * 4));
    NativeDrawReplayTexture texture;
    UNSCOPED_INFO("fetch 001FE3FF decodes to one 1024x256 RGBA8 level: " + error);
    REQUIRE((DecodeNativeTexture(fetch, std::vector<std::uint8_t>(range.size), texture, error) && texture.width == 1024 && texture.height == 256 &&
             texture.format == NativeDrawReplayTextureFormat::Rgba8 && texture.mipLevels == 1));
    NativeDrawReplaySampler sampler;
    UNSCOPED_INFO("clamped, linear logo sampler: " + error);
    REQUIRE((DecodeLabelSampler(fetch, sampler, error) && sampler.address == std::array<std::uint32_t, 3>{ 3, 3, 3 } && sampler.magLinear &&
             sampler.minLinear && sampler.mipLinear));

    // The GFx sampler is the label sampler less every field the host sampler
    // cannot reproduce. The logo itself carries a nonzero LOD bias with equal
    // filters, which a one-level texture ignores, so it stays admitted.
    NativeDrawReplaySampler gfxSampler;
    UNSCOPED_INFO("the captured logo fetch keeps its clamped, linear sampler: " + error);
    REQUIRE((DecodeGfxSampler(fetch, gfxSampler, error) && error.empty() && gfxSampler.address == sampler.address && gfxSampler.magLinear &&
             gfxSampler.minLinear && gfxSampler.mipLinear && gfxSampler.maxLod == sampler.maxLod));
    const auto with = [&](std::size_t index, std::uint32_t value)
    {
        auto words   = fetch;
        words[index] = value;
        return words;
    };
    const auto gfxRefusal = [&](const char* name, const std::array<std::uint32_t, 6>& words, const char* expected)
    {
        NativeDrawReplaySampler refused;
        std::string             reason;
        UNSCOPED_INFO(std::string(name) + ": " + reason);
        REQUIRE((!DecodeGfxSampler(words, refused, reason) && reason == expected && refused.address == NativeDrawReplaySampler{}.address &&
                 !refused.magLinear && !refused.minLinear));
    };
    constexpr std::uint32_t kMagPointMinLinear = 0x00A00C14, kMagLinearMinPoint = 0x00880C14, kMagPointMinPoint = 0x00800C14;
    // Linear magnification, point minification and a +4 bias: the guest picks
    // point filtering where a host sampler without the bias picks linear.
    auto plusFour = with(3, kMagLinearMinPoint);
    plusFour[4]   = 0x00080003;
    gfxRefusal("linear mag, point min and a +4 LOD bias", plusFour, "gfx_sampler_lod_bias");
    gfxRefusal("linear mag, point min and the captured -0.5 bias", with(3, kMagLinearMinPoint), "gfx_sampler_lod_bias");
    gfxRefusal("point mag, linear min and the captured -0.5 bias", with(3, kMagPointMinLinear), "gfx_sampler_lod_bias");
    auto gradientH = with(3, kMagLinearMinPoint), gradientV = with(3, kMagPointMinLinear);
    gradientH[4] = 0x00400003;
    gradientV[4] = 0x08000003;
    gfxRefusal("unequal filters with a horizontal gradient adjust", gradientH, "gfx_sampler_gradient_adjust");
    gfxRefusal("unequal filters with a vertical gradient adjust", gradientV, "gfx_sampler_gradient_adjust");
    gfxRefusal("a texture border", with(3, 0x00A80C14 | 0x80000000U), "gfx_sampler_border_size");
    gfxRefusal("the OpenGL nearest clamp policy", with(1, fetch[1] | 0x800), "gfx_sampler_nearest_clamp_policy");
    for (const std::uint32_t mode : { 4U, 5U, 6U, 7U })
    {
        gfxRefusal("an inexact X clamp mode", with(0, (fetch[0] & ~(7U << 10)) | (mode << 10)), "gfx_sampler_clamp_mode");
        gfxRefusal("an inexact Y clamp mode", with(0, (fetch[0] & ~(7U << 13)) | (mode << 13)), "gfx_sampler_clamp_mode");
    }
    gfxRefusal("a mip chain", with(4, fetch[4] | (1U << 6)), "gfx_texture_mipmapped");
    gfxRefusal("anisotropic filtering", with(3, fetch[3] | (1U << 25)), "gfx_sampler_unsupported");
    // Forms that stay exact: unequal filters with no bias, equal point filters
    // with a bias, and the clamp modes that map one to one.
    auto zeroBias = with(3, kMagLinearMinPoint);
    zeroBias[4]   = 0x00000003;
    UNSCOPED_INFO("unequal filters with no LOD bias or gradient adjust keep their own filters: " + error);
    REQUIRE((DecodeGfxSampler(zeroBias, gfxSampler, error) && gfxSampler.magLinear && !gfxSampler.minLinear));
    UNSCOPED_INFO("equal point filters ignore the captured bias on one level: " + error);
    REQUIRE((DecodeGfxSampler(with(3, kMagPointMinPoint), gfxSampler, error) && !gfxSampler.magLinear && !gfxSampler.minLinear));
    for (const auto [mode, host] : { std::pair{ 0U, 1U }, { 1U, 2U }, { 2U, 3U }, { 3U, 5U } })
    {
        UNSCOPED_INFO("clamp mode " + std::to_string(mode) + " maps to host mode " + std::to_string(host) + ": " + error);
        REQUIRE((DecodeGfxSampler(with(0, (fetch[0] & ~(7U << 10)) | (mode << 10)), gfxSampler, error) && gfxSampler.address[0] == host));
    }

    // The Hall of Glory loading background (live fetch 0 of the strict run):
    // linear DXT1 1024x1024, endian 8in16, packed mips 0..9 below the base,
    // linear filters and a -0.5 LOD bias. Only the full layout describes it.
    const std::array<std::uint32_t, 6> hall{ 0x08004802, 0x18943052, 0x007FE3FF, 0x00A80D10, 0x003F0243, 0x18913A18 };
    UNSCOPED_INFO("the base-level layout refuses packed mips: " + error);
    REQUIRE((!GetNativeTextureMemoryRange(hall, range, error) && error == "unsupported native texture fetch representation"));
    NativeTextureMemoryRanges hallRanges;
    UNSCOPED_INFO("the full layout names the 512 KiB base and the packed mips: " + error);
    REQUIRE((GetNativeTextureMemoryRanges(hall, hallRanges, error) && hallRanges.base.address == 0x18943000 && hallRanges.base.size == 512 * 1024 &&
             hallRanges.mips.address == 0x18913000 && hallRanges.mips.size));
    std::vector<std::uint8_t> hallBase(hallRanges.base.size), hallMips(hallRanges.mips.size);
    for (std::size_t i = 0; i < hallBase.size(); ++i)
        hallBase[i] = static_cast<std::uint8_t>(i * 7 + 1);
    for (std::size_t i = 0; i < hallMips.size(); ++i)
        hallMips[i] = static_cast<std::uint8_t>(i * 13 + 5);
    NativeDrawReplayTexture hallTexture;
    NativeDrawReplaySampler hallSampler;
    UNSCOPED_INFO("the loading background uploads all ten DXT1 levels: " + error);
    REQUIRE((DecodeGfxTexture(hall, hallBase, hallMips, hallTexture, hallSampler, error) && hallTexture.format == NativeDrawReplayTextureFormat::Bc1 &&
             hallTexture.width == 1024 && hallTexture.height == 1024 && hallTexture.mipLevels == 10 &&
             hallTexture.dimension == NativeDrawReplayTextureDimension::Texture2D));
    const auto hallPayload = hallTexture.payloadBytes();
    UNSCOPED_INFO("the base level is the guest rows with 8in16 swapping");
    REQUIRE((hallPayload.size() > hallBase.size() && hallPayload[0] == hallBase[1] && hallPayload[1] == hallBase[0] && hallPayload[2] == hallBase[3] &&
             hallPayload[2047] == hallBase[2046]));
    UNSCOPED_INFO("trilinear, clamped, with the SDK anisotropic override and the -0.5 bias as mip bias");
    REQUIRE((hallSampler.minLinear && hallSampler.magLinear && hallSampler.mipLinear && hallSampler.anisotropy == kLeaderAnisotropicOverride &&
             hallSampler.mipBias == -0.5F && hallSampler.maxLod == std::numeric_limits<float>::max() &&
             hallSampler.address == std::array<std::uint32_t, 3>{ 3, 3, 3 }));
    const auto hallRefusal = [&](const char* name, std::array<std::uint32_t, 6> words, std::span<const std::uint8_t> mips, const char* expected)
    {
        NativeDrawReplayTexture refusedTexture;
        NativeDrawReplaySampler refusedSampler;
        UNSCOPED_INFO(std::string(name) + ": " + error);
        REQUIRE((!DecodeGfxTexture(words, hallBase, mips, refusedTexture, refusedSampler, error) && error == expected && !refusedTexture.width));
    };
    auto tiledHall = hall, bc2Hall = hall, unequalHall = hall, anisoHall = hall;
    tiledHall[0] |= 1U << 31;
    bc2Hall[1]     = (bc2Hall[1] & ~0x3FU) | 19;
    unequalHall[3] = (unequalHall[3] & ~(3U << 21)) | (0U << 21);
    anisoHall[3] |= 1U << 25;
    hallRefusal("a tiled variant", tiledHall, hallMips, "gfx_texture_mipmapped");
    hallRefusal("a DXT2/3 variant", bc2Hall, hallMips, "gfx_texture_mipmapped");
    hallRefusal("unequal filters with the bias", unequalHall, hallMips, "gfx_texture_mipmapped");
    hallRefusal("guest anisotropic filtering", anisoHall, hallMips, "gfx_sampler_anisotropy");
    hallRefusal("mip storage that does not match the layout", hall, std::span<const std::uint8_t>(hallMips).first(hallMips.size() - 1), "gfx_texture_decode: native texture source does not match its declared memory ranges");
    hallRefusal("the base-level route", hall, {}, "gfx_texture_mipmapped");

    // The route's texture cache keys the fetch, base and mip bytes, so a
    // repeated draw reuses the version and a changed mip byte decodes again.
    LabelTextureCache       hallCache;
    NativeDrawReplayTexture cachedFirst, cachedSecond, cachedChanged;
    UNSCOPED_INFO("a repeated mipped draw reuses its cached decode: " + error);
    REQUIRE((DecodeGfxTexture(hall, hallBase, hallMips, cachedFirst, hallSampler, error, &hallCache) &&
             DecodeGfxTexture(hall, hallBase, hallMips, cachedSecond, hallSampler, error, &hallCache) && cachedFirst.textureVersion &&
             cachedFirst.textureVersion == cachedSecond.textureVersion && cachedFirst.mipLevels == 10 &&
             std::ranges::equal(cachedFirst.payloadBytes(), hallTexture.payloadBytes())));
    auto changedMips = hallMips;
    changedMips.back() ^= 0xFF;
    UNSCOPED_INFO("a changed mip byte decodes again: " + error);
    REQUIRE((DecodeGfxTexture(hall, hallBase, changedMips, cachedChanged, hallSampler, error, &hallCache) && cachedChanged.textureVersion != cachedFirst.textureVersion));

    // Composition: GFx is tried after the panel and label families.
    memory.reads                      = 0;
    const auto             shader     = std::make_shared<NativeGameplayShaderInterface>();
    auto                   labelsOnly = MakeBootDrawBuilder(read, nullptr, shader);
    NativeDrawReplayRecipe draw;
    std::string            detail;
    UNSCOPED_INFO("without a GFx shader the quads have no route");
    REQUIRE((!labelsOnly(gfxSubmission(tuple), draw, detail) && detail.empty() && memory.reads == 0));
    auto gfxOnly = MakeBootDrawBuilder(read, nullptr, nullptr, std::make_shared<GfxDrawCatalog>(catalog));
    UNSCOPED_INFO("the GFx reader runs and names the unpinned PS: " + detail);
    REQUIRE((!gfxOnly(gfxSubmission(tuple), draw, detail) && detail == unpinned && memory.reads > 0));
    std::cout << "GFx reader selection, logo texture extent and builder composition checks passed\n";
}

// The boot frames' scanout copy words (native resolve log:
// copy=00100340|00100040,1F6F8000,02D00500,01000300): both tiles go to the
// swapped frontbuffer's base in its 1280x720 view.
constexpr std::uint32_t kFrontbuffer = 0x1F6F8000, kScanoutDestPitch = 0x02D00500;

NativeFrameSwap swapRecord()
{
    NativeFrameSwap swap;
    swap.sequence            = 1;
    swap.frontbufferPhysical = kFrontbuffer;
    swap.fetch               = { 2, kFrontbuffer | 6, 1279 | (719U << 13), 0xD10, 0, 0 };
    swap.textureFormat       = 6;
    swap.width               = 1280;
    swap.height              = 720;
    for (std::uint32_t i = 0; i < swap.gammaTable.size(); ++i)
    {
        const auto value   = (i * 1023U + 127U) / 255U;
        swap.gammaTable[i] = (value << 20) | (value << 10) | value;
    }
    return swap;
}

// One C10 draw event: its captured payloads by role and its entry tuple
// (events.toml raw_r3_r10_u64, pinned here).
struct Event
{
    std::uint64_t                                sequence = 0;
    std::array<std::uint32_t, 8>                 arguments{};
    std::map<std::string, std::filesystem::path> files;
    std::map<std::string, std::uint64_t>         addresses;
    std::map<std::string, bool>                  physical;
};

// The loading frame 429: one GFx quad batch, then one label batch. 2670 is
// the first GFx draw (pre-patch VS) and 2674 the next (the guest's patched VS).
constexpr std::uint64_t            kGfx = 2937, kLabel = 2941, kFirst = 2670, kSecond = 2674;
const std::array<std::uint32_t, 8> kGfxTuple{ 0x50235580, 4, 0, 12, 30, 0x50BCF2DC, 1, 0x50BCF33C };
const std::array<std::uint32_t, 8> kLabelTuple{ 0x50235580, 4, 60, 0x50B33244, 28, 0, 0, 0 };

// Each event's captured payloads by role.
void bindPayloads(const std::filesystem::path& capture, std::map<std::uint64_t, Event>& events)
{
    const auto  manifest = toml::parse_file((capture / "payload-manifest.toml").string());
    const auto* bindings = manifest["bindings"].as_array();
    UNSCOPED_INFO("payload manifest bindings");
    REQUIRE(bindings != nullptr);
    for (const auto& node : *bindings)
    {
        const auto& binding = *node.as_table();
        const auto  event   = binding["event"].value_or<std::int64_t>(-1);
        if (!events.contains(std::uint64_t(event)))
            continue;
        auto&      e    = events[std::uint64_t(event)];
        const auto role = binding["role"].value_or(std::string());
        UNSCOPED_INFO("payload error on event " + std::to_string(event));
        REQUIRE(binding["error"].value_or(std::string("x")).empty());
        e.files[role]     = capture / binding["file"].value_or(std::string());
        e.addresses[role] = binding["address"].value_or<std::int64_t>(0);
        e.physical[role]  = binding["physical"].value_or(false);
    }
}

std::map<std::uint64_t, Event> loadEvents(const std::filesystem::path& capture)
{
    std::map<std::uint64_t, Event> events;
    for (const auto sequence : { kGfx, kLabel, kFirst, kSecond })
        events[sequence].sequence = sequence;
    bindPayloads(capture, events);
    for (const auto sequence : { kGfx, kFirst, kSecond })
        events[sequence].arguments = kGfxTuple;
    events[kLabel].arguments = kLabelTuple;
    return events;
}

// One root submission of a captured frame: an outer draw entry, not a call
// nested in one, with its entry tuple (raw_r3_r10_u64) and payloads.
struct RootDraw
{
    Event          event;
    NativeDrawHook hook   = NativeDrawHook::IndexedBuffer;
    std::uint32_t  caller = 0;
};

// Every root submission of `frame`, in sequence order.
std::vector<RootDraw> frameDraws(const std::filesystem::path& capture, std::int64_t frame)
{
    const auto  table = toml::parse_file((capture / "events.toml").string());
    const auto* all   = table["events"].as_array();
    UNSCOPED_INFO("capture events");
    REQUIRE(all != nullptr);
    std::map<std::uint64_t, Event>          events;
    std::map<std::uint64_t, NativeDrawHook> hooks;
    std::map<std::uint64_t, std::uint32_t>  callers;
    for (const auto& node : *all)
    {
        const auto& record = *node.as_table();
        if (record["kind"].value_or(std::string()) != "draw" || record["frame"].value_or<std::int64_t>(-1) != frame ||
            record["parent_call_id"].value_or<std::int64_t>(-1) != 0)
            continue;
        const auto  sequence = std::uint64_t(record["sequence"].value_or<std::int64_t>(0));
        const auto  hook     = FindNativeDrawHook(std::uint32_t(record["entry"].value_or<std::int64_t>(0)));
        const auto* tuple    = record["raw_r3_r10_u64"].as_array();
        UNSCOPED_INFO("root draw " + std::to_string(sequence) + " entry and tuple");
        REQUIRE((hook && tuple && tuple->size() == 8));
        auto& e    = events[sequence];
        e.sequence = sequence;
        for (std::size_t i = 0; i < 8; ++i)
            e.arguments[i] = static_cast<std::uint32_t>(std::stoull((*tuple)[i].value_or(std::string("0"))));
        hooks[sequence]   = *hook;
        callers[sequence] = static_cast<std::uint32_t>(std::stoull(record["caller_lr"].value_or(std::string("0"))));
    }
    bindPayloads(capture, events);
    std::vector<RootDraw> draws;
    for (auto& [sequence, e] : events)
        draws.push_back({ std::move(e), hooks.at(sequence), callers.at(sequence) });
    return draws;
}

// A fresh uniquely named temporary directory, removed with its contents.
struct TemporaryDirectory
{
    std::filesystem::path path;

    explicit TemporaryDirectory(std::string_view stem)
    {
        const auto root = std::filesystem::temp_directory_path();
        for (unsigned attempt = 0; attempt < 64; ++attempt)
        {
            path = root / std::format("{}-{}-{}-{}", stem, GetCurrentProcessId(), std::chrono::steady_clock::now().time_since_epoch().count(), attempt);
            if (std::filesystem::create_directory(path))
                return;
        }
        throw std::runtime_error("no fresh temporary directory for " + std::string(stem));
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&)            = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
};

// The draw's state after Original: the captured pre-state plus its recorded
// (offset, before, after) word deltas.
std::vector<std::uint8_t> postState(const std::filesystem::path& capture, const Event& e)
{
    auto       state = read(capture / ("draw-" + std::to_string(e.sequence) + ".state.be.bin"));
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

// Guest memory at one draw's entry. The GFx vertices before the lowest index
// were not retained (the capture keeps the indexed range); they are zero here
// and in the control, and no index names them.
FakeMemory memoryAt(const std::filesystem::path& capture, const Event& e, std::uint32_t stride)
{
    FakeMemory memory;
    addStride(memory, stride);
    for (const auto& [role, path] : e.files)
    {
        if (role.starts_with("state_"))
            continue;
        auto bytes = read(path);
        if (role == "vertices" && e.addresses.at(role) != e.arguments[stride == kGfxStride ? 7 : 3])
        {
            const auto gap = e.addresses.at(role) - e.arguments[7];
            UNSCOPED_INFO("the retained GFx vertices start at index 4");
            REQUIRE((stride == kGfxStride && gap == 4 * kGfxStride));
            bytes.insert(bytes.begin(), gap, 0);
            memory.Add(e.arguments[7], false, std::move(bytes));
            continue;
        }
        memory.Add(e.addresses.at(role), e.physical.at(role), std::move(bytes));
    }
    memory.Add(e.arguments[0], false, read(capture / ("draw-" + std::to_string(e.sequence) + ".state.be.bin")));
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

std::uint32_t physicalOf(std::uint32_t raw)
{
    return (raw & 0x1FFFFFFFU) + (((raw >> 20) + 512) & 0x1000);
}

// State reads cover G and bound RT/depth words; surface metadata is rebuilt
// from G.
bool stateWord(std::span<const std::uint8_t> state, std::uint32_t g, std::uint32_t address, std::uint32_t& out)
{
    // Capture has no surface objects; shadow words and the first tile
    // reconstruct them.
    if (state.size() != 0x3500)
        return false;
    if (address >= g && address - g + 4 <= state.size())
    {
        out = word(state, address - g);
        return true;
    }
    for (const auto [binding, info] : { std::pair{ 0x3098U, 0x2884U }, std::pair{ 0x309CU, 0x288CU }, std::pair{ 0x30A8U, 0x2888U } })
    {
        const auto surface = word(state, binding);
        if (!surface || address < surface || address - surface > 0x24)
            continue;
        if (address - surface == 0x18)
            out = word(state, 0x2880);
        else if (address - surface == 0x1C)
            out = word(state, info);
        else if (address - surface == 0x24)
            out = ((word(state, 0x31D8) - word(state, 0x31D0) - 1) << 18) | ((word(state, 0x31DC) - word(state, 0x31D4) - 1) << 3);
        else
            continue;
        return true;
    }
    return false;
}

// Completes every span at submission and keeps the last one's frame; frames
// are not rendered.
struct AcceptingCompletion final : INativeCompletionSource
{
    std::uint64_t                    submitted = 0;
    std::optional<NativeRenderFrame> last;

    std::uint64_t Submit(NativeCompletionSpan span, std::string&) override
    {
        last = std::move(span.frame);
        return ++submitted;
    }

    std::uint64_t Completed() const override
    {
        return submitted;
    }

    bool WaitFor(std::uint64_t value, std::chrono::milliseconds) override
    {
        return value <= submitted;
    }
};

// The title's scanout form, from the native resolve log (copy=...,01000300)
// and a captured VdSwap fetch (dword 3 0x1414, ZYX1, no endian swap).
constexpr std::uint32_t kScanoutDestInfo = 0x01000300, kSwapFetchWord3 = 0x1414;

// Raw k8in32 k_8_8_8_8 fetch/bytes oracle; alpha-weighted mean RGB.
std::array<double, 3> presentedTextureColor(const std::array<std::uint32_t, 6>& fetch, std::span<const std::uint8_t> bytes, std::uint32_t destInfo, std::uint32_t swapWord3)
{
    UNSCOPED_INFO("a k8in32 k_8_8_8_8 texture");
    REQUIRE(((fetch[1] & 63) == 6 && ((fetch[1] >> 6) & 3) == 2 && !bytes.empty() && bytes.size() % 4 == 0));
    // xenos.h xe_gpu_texture_fetch_t: endian dword 1 bits 6..7; swizzle dword 3
    // bits 1..12, three bits per output channel.
    const auto select = [](std::uint32_t word3, const std::array<double, 4>& in)
    {
        std::array<double, 4> out{};
        for (std::size_t i = 0; i < 4; ++i)
        {
            // TextureCache::GuestToHostSwizzle selectors 4/5 are UNORM 0/1.
            const auto selector = (word3 >> (1 + 3 * i)) & 7;
            UNSCOPED_INFO("a defined selector");
            REQUIRE(selector < 6);
            out[i] = selector < 4 ? in[selector] : selector == 4 ? 0.0
                                                                 : 255.0;
        }
        return out;
    };
    std::array<double, 4> sum{};
    for (std::size_t i = 0; i < bytes.size(); i += 4)
    {
        // k8in32 makes host byte k the guest word's bits 8k..8k+7 (RGBA).
        const auto texel = select(fetch[3], { double(bytes[i + 3]), double(bytes[i + 2]), double(bytes[i + 1]), double(bytes[i]) });
        for (std::size_t c = 0; c < 3; ++c)
            sum[c] += texel[c] * texel[3];
        sum[3] += texel[3];
    }
    UNSCOPED_INFO("the texture has coverage");
    REQUIRE(sum[3] > 0);
    std::array<double, 4> target{ sum[0] / sum[3], sum[1] / sum[3], sum[2] / sum[3], 255.0 };
    // RB_COPY_DEST_INFO bit 24 swaps red/blue; swap fetch reads frontbuffer.
    if (destInfo & 0x01000000)
        std::swap(target[0], target[2]);
    const auto presented = select(swapWord3, target);
    return { presented[0], presented[1], presented[2] };
}

// The red-blue balance of the colored pixels of a presented R10G10B10A2
// plane: sum(R - B) / sum(R + B) over pixels whose red and blue differ.
double presentedRedBlueBalance(std::span<const std::uint8_t> plane, std::size_t& colored)
{
    double difference = 0, total = 0;
    colored = 0;
    for (std::size_t i = 0; i + 3 < plane.size(); i += 4)
    {
        const auto pixel = std::uint32_t(plane[i]) | (std::uint32_t(plane[i + 1]) << 8) | (std::uint32_t(plane[i + 2]) << 16) | (std::uint32_t(plane[i + 3]) << 24);
        const auto red = double(pixel & 1023), blue = double((pixel >> 20) & 1023);
        if (std::abs(red - blue) < 16)
            continue;
        ++colored;
        difference += red - blue;
        total += red + blue;
    }
    return total > 0 ? difference / total : 0;
}

// The control replays the guest's own Original outputs: the post-draw state,
// the VS words the guest patched, the captured geometry and texture. It shares
// no code with the readers, fold, fetch patch, fixed-state checks or assembler.
NativeDrawReplayRecipe controlDraw(const std::filesystem::path& capture, const Event& e, bool gfx, const NativeGameplayShaderInterface& shader, NativeDrawReplayTexture& texture, NativeDrawReplaySampler& sampler)
{
    const auto                   state = postState(capture, e);
    std::array<std::uint32_t, 6> fetch{};
    for (std::size_t i = 0; i < fetch.size(); ++i)
        fetch[i] = word(state, 0x480 + i * 4);
    std::string error;
    UNSCOPED_INFO("control texture: " + error);
    REQUIRE(DecodeNativeTexture(fetch, read(e.files.at("sampled_base")), texture, error));
    // Both fetches clamp on every axis with linear filters (fetch words 0 and 3).
    sampler           = {};
    sampler.address   = { 3, 3, 3 };
    sampler.magLinear = sampler.minLinear = sampler.mipLinear = true;
    sampler.maxLod                                            = std::numeric_limits<float>::max();
    auto       registers                                      = registersOf(state);
    const auto source                                         = gfx ? e.arguments[7] : e.arguments[3];
    auto       vertices                                       = read(e.files.at("vertices"));
    if (gfx)
        vertices.insert(vertices.begin(), 4 * kGfxStride, 0);
    const auto base                                       = physicalOf(source);
    registers[0x48BE]                                     = base | 3;
    registers[0x48BF]                                     = static_cast<std::uint32_t>(vertices.size()) | 0x10000002U;
    const auto                                         vs = words(read(e.files.at("vs_code"))), ps = words(read(e.files.at("ps_code")));
    const std::array<NativeGameplayVertexFetchSpan, 1> fetches{ { { 95, base, vertices } } };
    const std::array<NativeGameplayTextureBinding, 1>  bindings{ { { 0, &texture, &sampler } } };
    const auto                                         indices = gfx ? read(e.files.at("indices")) : std::vector<std::uint8_t>{};
    NativeGameplayDrawView                             view;
    view.registers       = registers;
    view.vertexMicrocode = vs;
    view.pixelMicrocode  = ps;
    view.vertexFetches   = fetches;
    view.textureBindings = bindings;
    view.primitive       = 4;
    if (gfx)
    {
        view.indexBytes  = indices;
        view.indexCount  = e.arguments[4];
        view.indexFormat = 0;
    }
    else
    {
        view.indexCount = e.arguments[2];
        view.indexed    = false;
    }
    NativeGameplayDrawTarget target;
    target.fullViewportTarget = true;
    target.width              = 1280;
    target.height             = 720;
    target.viewport           = { 0, 0, 1280, 720, 0, 0 };
    target.scissor            = { 0, 0, 1280, 720 };
    NativeDrawReplayRecipe recipe;
    UNSCOPED_INFO("control recipe: " + error);
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, target, recipe, error));
    return recipe;
}

// GuestDrawRegisters' shadow source of a render-state register.
std::size_t shadowOffset(std::uint32_t index)
{
    return index >= 0x2300   ? 0x29B8 + (index - 0x2300) * 4
           : index >= 0x2200 ? 0x2934 + (index - 0x2200) * 4
           : index >= 0x2180 ? 0x2920 + (index - 0x2180) * 4
           : index >= 0x2100 ? 0x28CC + (index - 0x2100) * 4
           : index >= 0x2080 ? 0x28C0 + (index - 0x2080) * 4
                             : 0x2880 + (index - 0x2000) * 4;
}

std::vector<std::uint8_t> hexBytes(std::string_view hex)
{
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
    return bytes;
}

// One world-map draw at 0x82303E90 and the guest's fold of it.
struct WorldDraw
{
    std::int64_t              sequence = 0;
    GfxDrawInputs             inputs;
    std::vector<std::uint8_t> code, state;
    // Captured shader objects: header bytes, object and microcode addresses.
    std::vector<std::uint8_t> vertexHeader, pixelHeader;
    std::uint32_t             vertexObject = 0, pixelObject = 0, vertexSource = 0, pixelSource = 0;
};

// INFERENCE: missing patch list names o0 export ALU instructions 6 and 7.
void foldWorldDraw(WorldDraw& draw, bool linked)
{
    // 0x826ADA38 folds state; 0x826AD8E8 disables VS o0 for a PS with no
    // interpolator.
    const auto& vs = draw.inputs.vertexMetadata;
    const auto& ps = draw.inputs.pixelMetadata;
    draw.code      = draw.inputs.vertexCode;
    draw.state     = draw.inputs.state;
    auto program   = word(vs, 8);
    if (linked)
    {
        for (const std::size_t instruction : { 6, 7 })
        {
            UNSCOPED_INFO("VS instruction is an o0 export");
            REQUIRE((word(draw.code, instruction * 12) & 0x803F) == 0x8000);
            store(draw.code, instruction * 12, 0xC8000000);
            store(draw.code, instruction * 12 + 4, 0);
            store(draw.code, instruction * 12 + 8, 0x02000000);
        }
        const auto count = word(ps, 20) & 31;
        program          = (program & ~0xF00000U) | (((count ? count - 1 : 0) & 15) << 20);
    }
    store(draw.state, 0x2920, program | word(ps, 8));
    store(draw.state, 0x2924, word(vs, 12) | word(ps, 12));
    store(draw.state, 0x2928, word(ps, 24));
    store(draw.state, 0x2954, (word(draw.state, 0x2954) & ~7U) | 4U);
}

// The world-map capture's GFx draws with synthetic zero geometry and texels:
// the recipe reads their state, microcode, metadata and fetch words.
std::vector<WorldDraw> loadWorldDraws(const std::filesystem::path& capture)
{
    const auto  table = toml::parse_file((capture / "events.toml").string());
    const auto* all   = table["events"].as_array();
    UNSCOPED_INFO("world capture events");
    REQUIRE(all != nullptr);
    std::vector<WorldDraw> draws;
    for (const auto& node : *all)
    {
        const auto& event = *node.as_table();
        if (event["kind"].value_or(std::string()) != "draw" || event["entry"].value_or<std::int64_t>(0) != 0x826A3568 ||
            event["caller_lr"].value_or(std::string()) != std::to_string(kGfxDrawCaller))
            continue;
        WorldDraw   draw;
        auto&       in = draw.inputs;
        const auto* r  = event["raw_r3_r10_u64"].as_array();
        const auto* sp = event["raw_stack_54_70"].as_array();
        UNSCOPED_INFO("world draw arguments");
        REQUIRE((r && r->size() == 8 && sp && !sp->empty()));
        std::array<std::uint32_t, 8> a{};
        for (std::size_t i = 0; i < a.size(); ++i)
            a[i] = static_cast<std::uint32_t>(std::stoull((*r)[i].value_or(std::string("0"))));
        in.arguments      = { a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], static_cast<std::uint32_t>((*sp)[0].value_or<std::int64_t>(0)), kGfxDrawCaller };
        draw.sequence     = event["sequence"].value_or<std::int64_t>(-1);
        in.state          = read(capture / event["state_file"].value_or(std::string()));
        const auto shader = [&](const char* stage, std::vector<std::uint8_t>& code, std::vector<std::uint8_t>& metadata, std::vector<std::uint8_t>& header, std::uint32_t& object, std::uint32_t& source)
        {
            code     = read(capture / std::format("{}-{}.be.bin", stage, event[stage]["source_sha256"].value_or(std::string())));
            metadata = hexBytes(event[stage]["metadata_hex"].value_or(std::string()));
            header   = hexBytes(event[stage]["header_hex"].value_or(std::string()));
            object   = static_cast<std::uint32_t>(event[stage]["object"].value_or<std::int64_t>(0));
            source   = static_cast<std::uint32_t>(event[stage]["source_address"].value_or<std::int64_t>(0));
            UNSCOPED_INFO("world draw metadata");
            REQUIRE(metadata.size() == 32);
        };
        shader("vs", in.vertexCode, in.vertexMetadata, draw.vertexHeader, draw.vertexObject, draw.vertexSource);
        shader("ps", in.pixelCode, in.pixelMetadata, draw.pixelHeader, draw.pixelObject, draw.pixelSource);
        in.vertices.assign(std::size_t(in.arguments.vertexCount) * in.arguments.stride, 0);
        in.indices.assign(std::size_t(in.arguments.indexCount) * 2, 0);
        for (std::size_t i = 0; i < in.textureFetch.size(); ++i)
            in.textureFetch[i] = word(in.state, 0x480 + i * 4);
        in.inputsComplete = in.gfxCandidate = true;
        draws.push_back(std::move(draw));
    }
    return draws;
}

void sizeWorldTexture(GfxDrawInputs& in)
{
    NativeTextureMemoryRange range;
    std::string              error;
    in.textureBytes.clear();
    in.textureMipBytes.clear();
    in.textureAddress = 0;
    if (GetNativeTextureMemoryRange(in.textureFetch, range, error))
    {
        in.textureAddress = range.address;
        in.textureBytes.assign(range.size, 0);
        return;
    }
    // As the reader: packed mips need the full layout's mip storage.
    NativeTextureMemoryRanges ranges;
    if (GetNativeTextureMemoryRanges(in.textureFetch, ranges, error) && ranges.mips.size)
    {
        in.textureAddress = ranges.base.address;
        in.textureBytes.assign(ranges.base.size, 0);
        in.textureMipBytes.assign(ranges.mips.size, 0);
    }
}

// The title's fetch patch tables at 0x82091394 and 0x820913A8, as the C10
// capture's fetch_swizzle_table and fetch_format_table payloads hold them.
constexpr std::string_view kFetchSwizzleTable = "e0001c00038000700008000a0008000e";
constexpr std::string_view kFetchFormatTable  = "00000000000001010000000000000000010100000000000000010200000000010201020401020400000000000000000000000000000000000003000000000000";

// Menu-pair memory includes shader headers, metadata, microcode, geometry and
// texel; patched VS identity gates rebuild.
FakeMemory worldMenuMemory(const WorldDraw& draw, std::span<const std::uint8_t> vertices, std::span<const std::uint8_t> indices, std::uint8_t texel)
{
    const auto& in = draw.inputs;
    const auto& a  = in.arguments;
    UNSCOPED_INFO("world draw shader headers");
    REQUIRE((draw.vertexHeader.size() == 912 && draw.pixelHeader.size() == 68));
    FakeMemory memory;
    addStride(memory, a.stride);
    memory.Add(a.graphics, false, in.state);
    memory.Add(draw.vertexObject, false, draw.vertexHeader);
    auto       metadata = in.vertexMetadata;
    const auto records  = (std::size_t(word(metadata, 24)) + 9) * 4;
    metadata.resize(records + 4, 0);
    store(metadata, records, 3);
    memory.Add(draw.vertexObject + word(draw.vertexHeader, 872 + 24) + 872, false, metadata);
    memory.Add(draw.vertexSource, true, in.vertexCode);
    memory.Add(draw.pixelObject, false, draw.pixelHeader);
    memory.Add(draw.pixelObject + word(draw.pixelHeader, 40 + 24) + 40, false, in.pixelMetadata);
    memory.Add(draw.pixelSource, true, in.pixelCode);
    // 0x82F6E7AC declares POSITION 0 as XY16i SHORT2 at offset 0.
    std::vector<std::uint8_t> declaration(52 + 12);
    store(declaration, 24, 1);
    store(declaration, 52 + 4, 0x002C2359);
    memory.Add(word(in.state, 0x2E2C), false, declaration);
    memory.Add(0x82091394, false, hexBytes(kFetchSwizzleTable));
    memory.Add(0x820913A8, false, hexBytes(kFetchFormatTable));
    memory.Add(a.vertices, false, { vertices.begin(), vertices.end() });
    memory.Add(a.indices, false, { indices.begin(), indices.end() });
    NativeTextureMemoryRange range;
    std::string              error;
    const bool               ranged = GetNativeTextureMemoryRange(in.textureFetch, range, error);
    UNSCOPED_INFO("world menu texture range: " + error);
    REQUIRE(ranged);
    memory.Add(range.address, true, std::vector<std::uint8_t>(range.size, texel));
    return memory;
}

// XY16i vertices as the guest stores them: each one big-endian word whose
// high half is the first SHORT and low half the second.
std::vector<std::uint8_t> xy16Vertices(std::span<const std::array<std::int16_t, 2>> corners)
{
    std::vector<std::uint8_t> bytes(corners.size() * 4);
    for (std::size_t i = 0; i < corners.size(); ++i)
        store(bytes, i * 4, (std::uint32_t(std::uint16_t(corners[i][0])) << 16) | std::uint16_t(corners[i][1]));
    return bytes;
}

// M2 stride-4 menu/world-map GFx recipes refuse each unsupported field.
int world(const std::filesystem::path& capture, const std::filesystem::path& shaders)
{
    std::string    error;
    GfxDrawCatalog catalog;
    UNSCOPED_INFO("world GFx catalog: " + error);
    REQUIRE(LoadGfxDrawCatalog(shaders, catalog, error));
    const auto* menu = catalog.Find(kGfxMenuPixelWordDigest);
    const auto* mask = catalog.Find(kGfxPairs[2].pixelWords);
    UNSCOPED_INFO("the catalog holds both stride-4 pairs");
    REQUIRE((menu && mask && menu->textures == 1 && mask->textures == 0 && menu->stride == 4 && mask->stride == 4));

    auto                       draws = loadWorldDraws(capture);
    std::map<std::string, int> outcomes;
    const WorldDraw*           menuSample = nullptr;
    const WorldDraw*           maskSample = nullptr;
    for (auto& draw : draws)
    {
        auto&       in    = draw.inputs;
        const auto  label = "world draw " + std::to_string(draw.sequence);
        const auto* entry = catalog.Find(GfxMicrocodeWordDigest(in.pixelCode));
        if (entry == nullptr)
        {
            // The panel pair is not a GFx catalog form.
            GfxDrawRecipe rejected;
            UNSCOPED_INFO(label + " names its unpinned PS: " + error);
            REQUIRE((!BuildGfxDrawRecipe(in, catalog, rejected, error) && error.starts_with("gfx_shader_unpinned ps=") &&
                     error.ends_with(std::format("stride={}", in.arguments.stride))));
            ++outcomes["unpinned stride " + std::to_string(in.arguments.stride)];
            continue;
        }
        if (entry->stride != 4)
            continue; // The loading pair's recipes are the C10 check's.
        const bool linked = entry == mask;
        if (entry->textures)
            sizeWorldTexture(in);
        std::vector<std::uint8_t> code = in.vertexCode, folded;
        if (linked)
        {
            UNSCOPED_INFO(label + ": the mask pair links, and the capture lacks its patch list: " + error);
            REQUIRE((!FoldSolidShaderMetadata(in, code, folded, error) && error == "adapter_linkage_record_extent"));
        }
        foldWorldDraw(draw, linked);
        if (!linked)
        {
            UNSCOPED_INFO(label + ": the menu pair's fold equals the title fold: " + error);
            REQUIRE((FoldSolidShaderMetadata(in, code, folded, error) && code == draw.code && folded == draw.state));
            UNSCOPED_INFO(label + ": captured VS is the patched menu VS");
            REQUIRE(GfxMicrocodeWordDigest(in.vertexCode) == kGfxMenuVertexWordDigest);
        }
        const auto    registers = GuestDrawRegisters(draw.state);
        const bool    full      = registers[0x2081] == 0 && registers[0x2082] == 0x02D00500;
        GfxDrawRecipe recipe;
        const bool    built = BuildGfxEffectiveDrawRecipe(in, *entry, draw.code, draw.state, recipe, error);
        const auto    name  = linked ? std::string("mask") : std::string("menu");
        // The HUD movies' display rectangles, each window scissor equal to its
        // viewport: 425,24 600x499 and 830,624 360x96.
        const bool left  = registers[0x2081] == 0x001801A9 && registers[0x2082] == 0x020B0401;
        const bool right = registers[0x2081] == 0x0270033E && registers[0x2082] == 0x02D004A6;
        UNSCOPED_INFO(label + std::format(" ({}) has a known display rectangle: window scissor {:#010x} {:#010x}", name, registers[0x2081], registers[0x2082]));
        REQUIRE((full || left || right));
        UNSCOPED_INFO(label + " (" + name + ") builds: " + error);
        REQUIRE(built);
        const bool stencil = registers[0x2200] & 1;
        UNSCOPED_INFO(label + " carries its stencil test");
        REQUIRE((recipe.native.stencil.enabled == stencil && (!stencil || recipe.native.stencil.reference == (registers[0x210D] & 0xFF))));
        UNSCOPED_INFO(label + " write mask");
        REQUIRE(recipe.native.blend.writeMask == (linked ? 0 : 15));
        const auto& v      = recipe.native.viewport;
        const auto& s      = recipe.native.scissor;
        const auto  placed = [&](int x, int y, int width, int height)
        {
            return v.x == x && v.y == y && v.width == width && v.height == height && s.left == x && s.top == y && s.right == x + width && s.bottom == y + height;
        };
        UNSCOPED_INFO(label + " carries its display rectangle");
        REQUIRE((full ? placed(0, 0, 1280, 720) : left ? placed(425, 24, 600, 499)
                                                       : placed(830, 624, 360, 96)));
        ++outcomes[name + (full ? " built" : left ? " built left"
                                                  : " built right")];
        if (full)
            (linked ? maskSample : menuSample) = &draw;
    }
    for (const auto& [outcome, count] : outcomes)
        std::cout << outcome << ": " << count << '\n';
    UNSCOPED_INFO("world capture outcome counts");
    REQUIRE((outcomes["menu built"] == 36 && outcomes["menu built left"] == 0 && outcomes["menu built right"] == 0 && outcomes["mask built"] == 18 &&
             outcomes["mask built left"] == 14 && outcomes["mask built right"] == 4 &&
             outcomes["unpinned stride 8"] == 22));

    // One mutation per enumerated register on each stride-4 form.
    const auto refused = [&](const WorldDraw& sample, const std::string& name, auto mutate, const std::string& expected)
    {
        auto draw = sample;
        mutate(draw);
        const auto*   entry = catalog.Find(GfxMicrocodeWordDigest(draw.inputs.pixelCode));
        GfxDrawRecipe rejected;
        std::string   reason;
        const bool    built = entry ? BuildGfxEffectiveDrawRecipe(draw.inputs, *entry, draw.code, draw.state, rejected, reason)
                                    : BuildGfxDrawRecipe(draw.inputs, catalog, rejected, reason);
        UNSCOPED_INFO(name + ": " + (built ? "accepted" : reason));
        REQUIRE((!built && reason.starts_with(expected)));
    };
    const auto setRegister = [](std::uint32_t index, std::uint32_t value)
    {
        return [index, value](WorldDraw& draw)
        {
            store(draw.state, shadowOffset(index), value);
        };
    };
    std::size_t mutants = 0;
    for (const auto* sample : { menuSample, maskSample })
    {
        UNSCOPED_INFO("a full-viewport sample of each stride-4 form");
        REQUIRE(sample != nullptr);
        const auto registers = GuestDrawRegisters(sample->state);
        const auto mutant    = [&](const std::string& name, auto mutate, const std::string& expected)
        {
            refused(*sample, name, mutate, expected);
            ++mutants;
        };
        for (const auto [index, value] : kGfxFixedState)
            mutant(std::format("fixed register {:#x}", index), setRegister(index, value ^ 0x100), std::format("gfx_fixed_state {:#x}: {:#x}", index, value ^ 0x100));
        for (const std::uint32_t index : { 0x2180U, 0x2181U, 0x2182U })
            mutant(std::format("program word {:#x}", index), setRegister(index, registers[index] ^ 0x8000), std::format("gfx_program_state {:#x}: {:#x}", index, registers[index] ^ 0x8000));
        // Only the loading pair has the off-screen panel form.
        mutant("the panel surface", setRegister(0x2000, kGfxOffscreenForm.surfaceInfo), std::format("gfx_fixed_state 0x2000: {:#x}", kGfxOffscreenForm.surfaceInfo));
        mutant("blend", setRegister(0x2201, 0x00010706), "gfx_blend_control 0x2201: 0x10706");
        mutant("color mask", setRegister(0x2104, 7), "gfx_color_mask 0x2104: 0x7");
        mutant("depth control", setRegister(0x2200, 0x24F00777), "gfx_depth_control 0x2200: 0x24f00777");
        mutant("color control", setRegister(0x2202, 0x8700000C), "gfx_color_control 0x2202: 0x8700000c");
        mutant("stencil mask", [](WorldDraw& draw)
               {
                   store(draw.state, shadowOffset(0x2200), 0x24F00271);
                   store(draw.state, shadowOffset(0x210D), 0x00FF7F01);
               },
               "gfx_stencil_mask 0x210d: 0xff7f01");
        mutant("pixel shader", [](WorldDraw& draw)
               {
                   draw.inputs.pixelCode[0] ^= 1;
               },
               "gfx_shader_unpinned ps=");
        mutant("linked VS", [](WorldDraw& draw)
               {
                   draw.code[0] ^= 1;
               },
               "gfx_patched_vs_identity");
        mutant("stride", [](WorldDraw& draw)
               {
                   draw.inputs.arguments.stride = 12;
               },
               "gfx_unsupported_indexed_up_tuple");
    }

    // Every path B blend word of 0x823036D0 builds with its factors. Alpha
    // copies color, and D3D12 takes a color factor in an alpha slot only in its
    // alpha form.
    {
        using Factor = NativeDrawReplayBlendFactor;
        using Op     = NativeDrawReplayBlendOp;

        struct Blend
        {
            std::uint32_t word;
            Op            op;
            Factor        source, destination, sourceAlpha, destinationAlpha;
        };

        constexpr std::array<Blend, kGfxBlendControls.size()> blends{ {
            { 0x07060706, Op::Add, Factor::SourceAlpha, Factor::InverseSourceAlpha, Factor::SourceAlpha, Factor::InverseSourceAlpha },
            { 0x00080008, Op::Add, Factor::DestinationColor, Factor::Zero, Factor::DestinationAlpha, Factor::Zero },
            { 0x01660166, Op::Maximum, Factor::SourceAlpha, Factor::One, Factor::SourceAlpha, Factor::One },
            { 0x01460146, Op::Minimum, Factor::SourceAlpha, Factor::One, Factor::SourceAlpha, Factor::One },
            { 0x01060106, Op::Add, Factor::SourceAlpha, Factor::One, Factor::SourceAlpha, Factor::One },
            { 0x01860186, Op::ReverseSubtract, Factor::SourceAlpha, Factor::One, Factor::SourceAlpha, Factor::One },
            { 0x01000100, Op::Add, Factor::Zero, Factor::One, Factor::Zero, Factor::One },
            { 0x00010001, Op::Add, Factor::One, Factor::Zero, Factor::One, Factor::Zero },
        } };
        for (std::size_t i = 0; i < blends.size(); ++i)
        {
            const auto& expected = blends[i];
            UNSCOPED_INFO(std::format("blend table entry {}", i));
            REQUIRE(kGfxBlendControls[i] == expected.word);
            auto draw = *menuSample;
            store(draw.state, shadowOffset(0x2201), expected.word);
            GfxDrawRecipe recipe;
            const bool    built = BuildGfxEffectiveDrawRecipe(draw.inputs, *menu, draw.code, draw.state, recipe, error);
            UNSCOPED_INFO(std::format("blend {:#x} builds: {}", expected.word, error));
            REQUIRE(built);
            const auto& b = recipe.native.blend;
            UNSCOPED_INFO(std::format("blend {:#x} factors and operations", expected.word));
            REQUIRE((b.enabled && b.colorOp == expected.op && b.alphaOp == expected.op && b.sourceColor == expected.source &&
                     b.destinationColor == expected.destination && b.sourceAlpha == expected.sourceAlpha &&
                     b.destinationAlpha == expected.destinationAlpha));
        }
    }

    // Each stencil form of the mask stack, by D3D12 value: compare equal=3 and
    // always=8; operations keep=1, replace=3, and the wrapping increment=7 and
    // decrement=8. Back-face stencil is off, so both faces match.
    {
        struct Stencil
        {
            std::uint32_t control;
            std::uint8_t  compare, fail, depthFail, pass;
        };

        constexpr std::array<Stencil, 4> stencils{ {
            { 0x24F00271, 3, 1, 1, 1 },
            { 0x24F08771, 8, 1, 1, 3 },
            { 0x24F18271, 3, 1, 1, 7 },
            { 0x24F1C271, 3, 1, 1, 8 },
        } };
        for (const auto control : kGfxDepthControls)
        {
            const auto found = std::ranges::find(stencils, control, &Stencil::control);
            auto       draw  = *menuSample;
            store(draw.state, shadowOffset(0x2200), control);
            store(draw.state, shadowOffset(0x210D), kGfxStencilMasks | 0x5A);
            GfxDrawRecipe recipe;
            const bool    built = BuildGfxEffectiveDrawRecipe(draw.inputs, *menu, draw.code, draw.state, recipe, error);
            UNSCOPED_INFO(std::format("depth control {:#x} builds: {}", control, error));
            REQUIRE(built);
            const auto& st = recipe.native.stencil;
            if (found == stencils.end())
            {
                UNSCOPED_INFO(std::format("depth control {:#x} tests nothing", control));
                REQUIRE((!st.enabled && !recipe.native.depth.enabled));
                continue;
            }
            const auto face = [&](const NativeDrawReplayStencilState::Face& f)
            {
                return f.compare == found->compare && f.fail == found->fail && f.depthFail == found->depthFail && f.pass == found->pass;
            };
            UNSCOPED_INFO(std::format("depth control {:#x} stencil faces: compare {} fail {} depth fail {} pass {}", control, st.front.compare, st.front.fail, st.front.depthFail, st.front.pass));
            REQUIRE((st.enabled && !recipe.native.depth.enabled && st.reference == 0x5A && st.readMask == 0xFF && st.writeMask == 0xFF &&
                     face(st.front) && face(st.back)));
        }
    }
    // The alpha-composite words of path A are refused by name.
    for (const auto blend : kGfxAlphaCompositeBlendControls)
    {
        refused(*menuSample, std::format("alpha composite blend {:#x}", blend), setRegister(0x2201, blend), std::format("gfx_blend_alpha_composite 0x2201: {:#x}", blend));
        ++mutants;
    }
    // Display rectangles outside the modeled form are refused by name. The
    // guest's viewport shadow follows each viewport edit.
    {
        const auto viewport = [&](float x, float y, float width, float height)
        {
            return [=](WorldDraw& draw)
            {
                store(draw.state, shadowOffset(0x210F), std::bit_cast<std::uint32_t>(width / 2));
                store(draw.state, shadowOffset(0x2110), std::bit_cast<std::uint32_t>(x + width / 2));
                store(draw.state, shadowOffset(0x2111), std::bit_cast<std::uint32_t>(-height / 2));
                store(draw.state, shadowOffset(0x2112), std::bit_cast<std::uint32_t>(y + height / 2));
                for (const auto [i, value] : std::array<std::pair<unsigned, float>, 4>{ { { 0, x }, { 1, y }, { 2, width }, { 3, height } } })
                    store(draw.state, 0x3168 + i * 4, std::bit_cast<std::uint32_t>(value));
            };
        };
        const auto both = [](auto first, auto second)
        {
            return [=](WorldDraw& draw)
            {
                first(draw);
                second(draw);
            };
        };
        refused(*menuSample, "the window offset disable bit", setRegister(0x2081, 0x80000000), "gfx_scissor_form 0x2081: 0x80000000");
        refused(*menuSample, "an empty window scissor", setRegister(0x2082, 0x02D00000), "gfx_scissor_empty 0x2082: 0x2d00000");
        refused(*menuSample, "a half-pixel viewport", viewport(0.5F, 0, 1279, 720), "gfx_viewport_form: 0.5,0 1279x720");
        refused(*menuSample, "a viewport past the target", both(viewport(640, 0, 1280, 720), setRegister(0x2081, 0x00000280)), "gfx_viewport_form: 640,0 1280x720");
        refused(*menuSample, "a scissor past the viewport", viewport(0, 24, 1280, 696), "gfx_scissor_beyond_viewport: scissor 0,0,1280,720 viewport 0,24 1280x696");
        refused(*menuSample, "an inverted window scissor", both(setRegister(0x2081, 0x00000280), setRegister(0x2082, 0x02D00000)), "gfx_scissor_form 0x2082: 0x2d00000");
        refused(*menuSample, "a zero-width viewport", viewport(0, 0, 0, 720), "gfx_viewport_form: unsupported gameplay viewport scale or depth range");
        mutants += 7;
        auto draw = *menuSample;
        both(viewport(425, 24, 600, 499), [](WorldDraw& d)
             {
                 store(d.state, shadowOffset(0x2081), 0x001801A9);
                 store(d.state, shadowOffset(0x2082), 0x020B0401);
             })(draw);
        GfxDrawRecipe recipe;
        UNSCOPED_INFO("the menu pair builds at the HUD display rectangle: " + error);
        REQUIRE((BuildGfxEffectiveDrawRecipe(draw.inputs, *menu, draw.code, draw.state, recipe, error) && recipe.native.viewport.x == 425 &&
                 recipe.native.scissor.right == 1025));
    }

    // Reader, fold and one-fetch patch: a menu draw rebuilt as guest memory at
    // entry, with signed XY16 geometry and `edits` to its raw shadow registers.
    // Vertex 4 only feeds a degenerate triangle.
    const auto menuRecipe = [&](std::span<const std::array<std::int16_t, 2>> corners, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& edits)
    {
        auto draw = *menuSample;
        for (const auto [index, value] : edits)
            store(draw.inputs.state, shadowOffset(index), value);
        const auto                vertices = xy16Vertices(corners);
        std::vector<std::uint8_t> indices;
        for (const std::uint8_t index : { 0, 1, 2, 0, 2, 3, 4, 4, 4 })
            indices.insert(indices.end(), { 0, index });
        auto             memory = worldMenuMemory(draw, vertices, indices, 0xC0);
        const ReadMemory reader = testing::MirrorFixtureReads([&memory](std::uint64_t address, std::uint64_t size, bool physical)
                                                              {
                                                                  return memory.Read(address, size, physical);
                                                              });
        const auto&      a      = draw.inputs.arguments;
        GfxDrawInputs    inputs;
        const bool       inputsRead = ReadGfxDrawInputs(gfxSubmission({ a.graphics, 4, 0, static_cast<std::uint32_t>(corners.size()), static_cast<std::uint32_t>(indices.size() / 2), a.indices, a.indexFormat, a.vertices }),
                                                        reader,
                                                        catalog,
                                                        inputs,
                                                        error);
        UNSCOPED_INFO("world menu draw read from guest memory: " + error);
        REQUIRE((inputsRead && inputs.inputsComplete));
        GfxDrawRecipe recipe;
        const bool    built = BuildGfxDrawRecipe(inputs, catalog, recipe, error);
        UNSCOPED_INFO("world menu recipe through the reader and fetch patch: " + error);
        REQUIRE(built);
        UNSCOPED_INFO("the one-fetch patch reproduces the captured menu VS");
        REQUIRE(GfxMicrocodeWordDigest(recipe.patchedVertexCode) == kGfxMenuVertexWordDigest);
        return std::move(recipe.native);
    };
    // Quad B lies inside quad A; both straddle zero on each axis.
    constexpr std::array<std::array<std::int16_t, 2>, 5> quadA{ { { -3000, -30000 }, { 3000, -30000 }, { 3000, 30000 }, { -3000, 30000 }, { -32768, 32767 } } };
    constexpr std::array<std::array<std::int16_t, 2>, 5> quadB{ { { -1000, -10000 }, { 1500, -10000 }, { 1500, 15000 }, { -1000, 15000 }, { 32767, -32768 } } };
    {
        // The 8in32 k_16_16 fetch: component 0 is the low half of each
        // big-endian word, both halves signed integers.
        const auto native = menuRecipe(quadA, {});
        const auto bytes  = native.vertexBytes();
        UNSCOPED_INFO("one float4 position per menu vertex");
        REQUIRE((native.vertexAttributeCount == 1 && native.vertexStrideBytes == 16 && bytes.size() == quadA.size() * 16 && native.indexCount == 9));
        for (std::size_t i = 0; i < quadA.size(); ++i)
        {
            std::array<float, 4> position{};
            std::memcpy(position.data(), bytes.data() + i * 16, sizeof(position));
            UNSCOPED_INFO(std::format("vertex {} decodes to ({}, {}, {}, {})", i, position[0], position[1], position[2], position[3]));
            REQUIRE((position == std::array<float, 4>{ float(quadA[i][1]), float(quadA[i][0]), 0.0F, 0.0F }));
        }
    }

    // One mutation per sampler field on the textured menu form; the fetch in
    // the state follows the inputs.
    namespace xenos    = rex::graphics::xenos;
    using Fetch        = xenos::xe_gpu_texture_fetch_t;
    const auto sampler = [&](const std::string& name, void (*edit)(Fetch&), const std::string& expected)
    {
        refused(*menuSample, name, [edit](WorldDraw& draw)
                {
                    Fetch fetch{};
                    std::memcpy(&fetch, draw.inputs.textureFetch.data(), sizeof(fetch));
                    edit(fetch);
                    std::memcpy(draw.inputs.textureFetch.data(), &fetch, sizeof(fetch));
                    for (std::size_t i = 0; i < draw.inputs.textureFetch.size(); ++i)
                        store(draw.state, 0x480 + i * 4, draw.inputs.textureFetch[i]);
                    sizeWorldTexture(draw.inputs);
                },
                expected);
        ++mutants;
    };
    sampler("mip_max_level", [](Fetch& f)
            {
                f.mip_max_level = 1;
            },
            "gfx_texture_mipmapped");
    sampler("type", [](Fetch& f)
            {
                f.type = xenos::FetchConstantType::kVertex;
            },
            "gfx_sampler_unsupported");
    sampler("dimension", [](Fetch& f)
            {
                f.dimension = xenos::DataDimension::k3D;
            },
            "gfx_sampler_unsupported");
    sampler("stacked", [](Fetch& f)
            {
                f.stacked = 1;
            },
            "gfx_sampler_unsupported");
    sampler("mip_min_level", [](Fetch& f)
            {
                f.mip_min_level = 1;
            },
            "gfx_sampler_unsupported");
    sampler("mip_address", [](Fetch& f)
            {
                f.mip_address = 1;
            },
            "gfx_sampler_unsupported");
    sampler("packed_mips", [](Fetch& f)
            {
                f.packed_mips = 1;
            },
            "gfx_sampler_unsupported");
    sampler("mag_filter", [](Fetch& f)
            {
                f.mag_filter = xenos::TextureFilter::kBaseMap;
            },
            "gfx_sampler_unsupported");
    sampler("min_filter", [](Fetch& f)
            {
                f.min_filter = xenos::TextureFilter::kBaseMap;
            },
            "gfx_sampler_unsupported");
    sampler("mip_filter", [](Fetch& f)
            {
                f.mip_filter = xenos::TextureFilter::kUseFetchConst;
            },
            "gfx_sampler_unsupported");
    sampler("aniso_filter", [](Fetch& f)
            {
                f.aniso_filter = xenos::AnisoFilter::kMax_2_1;
            },
            "gfx_sampler_unsupported");
    sampler("border_color", [](Fetch& f)
            {
                f.border_color = xenos::BorderColor::k_ABGR_White;
            },
            "gfx_sampler_unsupported");
    sampler("border_size", [](Fetch& f)
            {
                f.border_size = 1;
            },
            "gfx_sampler_border_size");
    sampler("nearest_clamp_policy", [](Fetch& f)
            {
                f.nearest_clamp_policy = 1;
            },
            "gfx_sampler_nearest_clamp_policy");
    sampler("clamp_x", [](Fetch& f)
            {
                f.clamp_x = xenos::ClampMode::kClampToHalfway;
            },
            "gfx_sampler_clamp_mode");
    sampler("clamp_y", [](Fetch& f)
            {
                f.clamp_y = xenos::ClampMode::kClampToBorder;
            },
            "gfx_sampler_clamp_mode");
    sampler("lod_bias", [](Fetch& f)
            {
                f.min_filter = xenos::TextureFilter::kPoint;
                f.lod_bias   = 32;
            },
            "gfx_sampler_lod_bias");
    sampler("grad_exp_adjust_h", [](Fetch& f)
            {
                f.min_filter        = xenos::TextureFilter::kPoint;
                f.lod_bias          = 0;
                f.grad_exp_adjust_h = 1;
            },
            "gfx_sampler_gradient_adjust");
    sampler("grad_exp_adjust_v", [](Fetch& f)
            {
                f.min_filter        = xenos::TextureFilter::kPoint;
                f.lod_bias          = 0;
                f.grad_exp_adjust_v = 1;
            },
            "gfx_sampler_gradient_adjust");
    sampler("num_format", [](Fetch& f)
            {
                f.num_format = 1;
            },
            "gfx_texture_decode: ");
    sampler("exp_adjust", [](Fetch& f)
            {
                f.exp_adjust = 1;
            },
            "gfx_texture_decode: ");
    sampler("sign_x", [](Fetch& f)
            {
                f.sign_x = xenos::TextureSign::kSigned;
            },
            "gfx_texture_decode: ");
    sampler("force_bc_w_to_max", [](Fetch& f)
            {
                f.force_bc_w_to_max = 1;
            },
            "gfx_texture_decode: ");
    sampler("format", [](Fetch& f)
            {
                f.format     = xenos::TextureFormat::k_8;
                f.endianness = xenos::Endian::kNone;
            },
            "gfx_texture_form ");
    std::cout << "World-map stride-4 GFx recipes build; " << mutants << " register and sampler mutants are refused by name\n";
    std::cout << "A signed XY16 menu draw builds through the reader and one-fetch patch with its decoded positions\n";

    if (environment("REREVVED_TEST_NATIVE_D3D12") != "1")
    {
        std::cout << "REREVVED_TEST_NATIVE_D3D12 is not 1; the menu stencil stack and Multiply blend were not rendered\n";
        return 0;
    }
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
    {
        std::cout << "No D3D12 device; the menu stencil stack and Multiply blend were not rendered\n";
        return 77;
    }
    // Overlap wraps stencil 255 to 0; A-only drops to 254; B restores 255.
    constexpr std::array<std::uint8_t, 4> clearBytes{ 102, 153, 204, 51 };
    const auto                            state = [](std::uint32_t depth, std::uint8_t reference, std::uint32_t colorMask, std::uint32_t blend)
    {
        return std::vector<std::pair<std::uint32_t, std::uint32_t>>{ { 0x2200, depth }, { 0x210D, kGfxStencilMasks | reference }, { 0x2104, colorMask }, { 0x2201, blend } };
    };
    std::vector<std::vector<NativeDrawReplayRecipe>> frames;
    frames.push_back({ menuRecipe(quadA, state(0x24F00770, 0, 15, 0x00010001)) });
    frames.push_back({ menuRecipe(quadB, state(0x24F00770, 0, 15, 0x00010001)) });
    frames.push_back({ menuRecipe(quadA, state(0x24F08771, 255, 0, 0x00010001)),
                       menuRecipe(quadB, state(0x24F18271, 255, 0, 0x00010001)),
                       menuRecipe(quadA, state(0x24F1C271, 255, 0, 0x00010001)),
                       menuRecipe(quadB, state(0x24F1C271, 0, 0, 0x00010001)),
                       menuRecipe(quadA, state(0x24F00271, 254, 15, 0x00010001)),
                       menuRecipe(quadA, state(0x24F00271, 255, 15, 0x00080008)) });
    NativeRendererD3D12 renderer;
    const bool          started = renderer.StartDrawStream(error);
    UNSCOPED_INFO("native stream: " + error);
    REQUIRE(started);
    std::vector<NativeDrawReplayResult> rendered;
    for (auto& draws : frames)
    {
        NativeRenderFrame frame;
        frame.textures.push_back({ 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, rendered.size() + 1 });
        NativeRenderFramePass pass;
        pass.target = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
        NativeRenderFramePass::Clear clear;
        clear.rectangle = { 0, 0, 1280, 720 };
        clear.color = clear.depth = clear.stencil = true;
        clear.colorValue                          = { 0.4F, 0.6F, 0.8F, 0.2F };
        pass.clears.push_back(clear);
        pass.draws = std::move(draws);
        NativeRenderFrameResolve resolve;
        resolve.textureId = 1;
        resolve.width     = 1280;
        resolve.height    = 720;
        pass.resolves.push_back(resolve);
        frame.passes.push_back(std::move(pass));
        frame.outputTextureId = 1;
        frame.retainOnly = frame.returnRawOutput = true;
        rendered.push_back(renderer.SubmitRenderFrame(std::move(frame)));
    }
    renderer.Shutdown();
    for (const auto& result : rendered)
    {
        UNSCOPED_INFO("menu frame renders: " + result.error);
        REQUIRE((result.success && result.rawRowPitch == 1280 * 4 && result.rawOutput.size() == 1280 * 720 * 4));
    }
    const auto& controlA = rendered[0].rawOutput;
    const auto& controlB = rendered[1].rawOutput;
    const auto& stack    = rendered[2].rawOutput;
    const auto  covered  = [&](const std::vector<std::uint8_t>& image, std::size_t offset)
    {
        return !std::equal(clearBytes.begin(), clearBytes.end(), image.begin() + offset);
    };
    std::size_t onlyA = 0, both = 0, multiplied = 0, mismatches = 0;
    for (std::size_t offset = 0; offset < stack.size(); offset += 4)
    {
        const bool inA = covered(controlA, offset), inB = covered(controlB, offset);
        onlyA += inA && !inB;
        both += inA && inB;
        for (std::size_t c = 0; c < 4; ++c)
        {
            const int source   = controlA[offset + c];
            const int expected = !inA ? clearBytes[c] : !inB ? source
                                                             : (source * clearBytes[c] + 127) / 255;
            mismatches += std::abs(stack[offset + c] - expected) > 1;
            multiplied += inA && inB && c == 0 && std::abs(expected - source) > 2 && std::abs(expected - clearBytes[c]) > 2;
        }
    }
    std::cout << std::format("Menu stack: {} pixels in A only, {} in A and B, {} Multiply-distinct\n", onlyA, both, multiplied);
    UNSCOPED_INFO("the stack covers both regions and Multiply changes B");
    REQUIRE((onlyA > 1000 && both > 1000 && multiplied > 1000));
    UNSCOPED_INFO(std::format("the rendered stencil stack differs from its expected regions at {} channels", mismatches));
    REQUIRE(mismatches == 0);
    std::cout << "The rendered menu stencil stack keeps A outside B and multiplies B by the destination\n";
    return 0;
}

struct Mutation
{
    bool swapUvColor = false, endianZero = false, blendOff = false;
};

int c10(const std::filesystem::path& capture)
{
    const auto gfxCatalog   = environment("REREVVED_GFX_SHADERS");
    const auto labelCatalog = environment("REREVVED_LABEL_SHADERS");
    const auto solidCapture = environment("REREVVED_SOLID_ADAPTER_CAPTURE");
    UNSCOPED_INFO("the GFx checks also need REREVVED_GFX_SHADERS, REREVVED_LABEL_SHADERS and REREVVED_SOLID_ADAPTER_CAPTURE");
    REQUIRE((!gfxCatalog.empty() && !labelCatalog.empty() && !solidCapture.empty()));
    std::string                   error;
    const auto                    events = loadEvents(capture);
    NativeGameplayShaderInterface labelShader, panelShader;
    GfxDrawCatalog                catalog;
    UNSCOPED_INFO("GFx catalog: " + error);
    REQUIRE((LoadGfxDrawCatalog(gfxCatalog, catalog, error) && catalog.Find(kGfxPixelWordDigest)));
    {
        // The registry admits the same catalog and names its pair count.
        NativeDrawFamily family;
        std::string      summary;
        UNSCOPED_INFO("GFx family admission: " + error);
        REQUIRE((GfxDrawFamily().admit(gfxCatalog, ReadMemory{}, family, summary, error) && family &&
                 summary == std::format("({} pairs)", catalog.entries.size())));
    }
    const auto& gfxShader = catalog.Find(kGfxPixelWordDigest)->shader;
    UNSCOPED_INFO("label catalog: " + error);
    REQUIRE(LoadLabelDrawShader(labelCatalog, labelShader, error));
    UNSCOPED_INFO("panel shader: " + error);
    REQUIRE(LoadSolidPanelShader(solidCapture, panelShader, error));

    // A flipped signed flag on any attribute changes how its bytes decode.
    for (const std::size_t flipped : { std::size_t(0), std::size_t(1), std::size_t(2) })
    {
        const TemporaryDirectory scratch("rerevved-gfx-catalog-signed");
        const auto               copy = scratch.path / "catalog";
        std::filesystem::copy(gfxCatalog, copy, std::filesystem::copy_options::recursive);
        std::string text;
        {
            std::ifstream in(copy / "shaders.toml", std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), {});
        }
        std::size_t at = 0;
        for (std::size_t i = 0; i <= flipped; ++i)
            at = text.find("signed = ", at + 1);
        UNSCOPED_INFO("signed flag found in the catalog copy");
        REQUIRE(at != std::string::npos);
        text.replace(at + 9, text[at + 9] == 't' ? 4 : 5, text[at + 9] == 't' ? "false" : "true");
        {
            std::ofstream out(copy / "shaders.toml", std::ios::binary | std::ios::trunc);
            out << text;
        }
        GfxDrawCatalog rejected;
        std::string    rejectedError;
        UNSCOPED_INFO("a flipped signed flag on attribute " + std::to_string(flipped) + " is rejected by name: " + rejectedError);
        REQUIRE((!LoadGfxDrawCatalog(copy, rejected, rejectedError) && rejectedError == "gfx_catalog_layout"));
    }
    {
        // A catalog built from other pixel words is not the pinned pair.
        const TemporaryDirectory scratch("rerevved-gfx-catalog-word-digest");
        const auto               copy = scratch.path / "catalog";
        std::filesystem::copy(gfxCatalog, copy, std::filesystem::copy_options::recursive);
        std::string text;
        {
            std::ifstream in(copy / "shaders.toml", std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), {});
        }
        const auto at = text.find(std::string(kGfxPixelWordDigest));
        UNSCOPED_INFO("pixel word digest found in the catalog copy");
        REQUIRE(at != std::string::npos);
        text[at] = text[at] == '0' ? '1' : '0';
        {
            std::ofstream out(copy / "shaders.toml", std::ios::binary | std::ios::trunc);
            out << text;
        }
        GfxDrawCatalog rejected;
        std::string    rejectedError;
        UNSCOPED_INFO("a changed pixel word digest is rejected by name: " + rejectedError);
        REQUIRE((!LoadGfxDrawCatalog(copy, rejected, rejectedError) && rejectedError == "gfx_catalog_word_digest"));
    }

    const auto readerFor = [](FakeMemory& memory)
    {
        return testing::MirrorFixtureReads(ReadMemory([&memory](std::uint64_t address, std::uint64_t size, bool physical)
                                                      {
                                                          return memory.Read(address, size, physical);
                                                      }));
    };
    // Patch: the first draw's pre-patch VS folds and patches to the words the
    // guest left for the next draw. That draw targets the MRT set, which the
    // fixed state refuses by register.
    {
        const auto&   first  = events.at(kFirst);
        auto          memory = memoryAt(capture, first, kGfxStride);
        GfxDrawInputs inputs;
        UNSCOPED_INFO("first GFx inputs: " + error);
        REQUIRE((ReadGfxDrawInputs(gfxSubmission(first.arguments), readerFor(memory), catalog, inputs, error) && inputs.gfxCandidate && inputs.inputsComplete));
        UNSCOPED_INFO("the first draw still holds the pre-patch VS");
        REQUIRE(GfxMicrocodeWordDigest(inputs.vertexCode) != kGfxVertexWordDigest);
        GfxDrawRecipe recipe;
        UNSCOPED_INFO("a GFx draw into the MRT set is refused by register: " + error);
        REQUIRE((!BuildGfxDrawRecipe(inputs, catalog, recipe, error) && error == "gfx_fixed_state 0x2000: 0x70101e0"));
        std::vector<std::uint8_t> code = inputs.vertexCode, folded, patched;
        UNSCOPED_INFO("fold: " + error);
        REQUIRE(FoldSolidShaderMetadata(inputs, code, folded, error));
        SolidDrawInputs linked = inputs;
        linked.vertexCode      = code;
        UNSCOPED_INFO("the GFx fetch patch equals the guest's patched VS: " + error);
        REQUIRE((PatchVertexFetch(linked, kGfxAttributeOffsets, patched, error) && patched == read(events.at(kSecond).files.at("vs_code"))));
    }
    std::cout << "GFx fetch patch equals the guest's patched VS; a draw into the MRT set is refused by register\n";

    // Inputs: the reader returns the captured bytes, and the recipe consumes
    // the guest's own Original outputs.
    const auto&   gfxEvent  = events.at(kGfx);
    auto          gfxMemory = memoryAt(capture, gfxEvent, kGfxStride);
    GfxDrawInputs inputs;
    UNSCOPED_INFO("GFx inputs: " + error);
    REQUIRE((ReadGfxDrawInputs(gfxSubmission(gfxEvent.arguments), readerFor(gfxMemory), catalog, inputs, error) && inputs.gfxCandidate && inputs.inputsComplete));
    UNSCOPED_INFO("reader inputs equal the C10 payloads");
    REQUIRE((inputs.vertexCode == read(gfxEvent.files.at("vs_code")) && inputs.pixelCode == read(gfxEvent.files.at("ps_code")) &&
             inputs.indices == read(gfxEvent.files.at("indices")) && inputs.textureBytes == read(gfxEvent.files.at("sampled_base")) &&
             inputs.textureAddress == 0x1BBBA000 && inputs.vertices.size() == 144));
    GfxDrawRecipe recipe;
    UNSCOPED_INFO("GFx recipe: " + error);
    REQUIRE(BuildGfxDrawRecipe(inputs, catalog, recipe, error));
    UNSCOPED_INFO("the steady-state VS is already the patched word set");
    REQUIRE(recipe.patchedVertexCode == inputs.vertexCode);
    UNSCOPED_INFO("effective registers and constants equal C10's Original state");
    REQUIRE(GuestDrawRegisters(recipe.effectiveState) == registersOf(postState(capture, gfxEvent)));
    UNSCOPED_INFO("catalog DXIL bound");
    REQUIRE((std::ranges::equal(recipe.native.vertexShaderBytes(), gfxShader.vertexShaderDxil) && std::ranges::equal(recipe.native.pixelShaderBytes(), gfxShader.pixelShaderDxil)));
    UNSCOPED_INFO("logo texture and 30 indices");
    REQUIRE((recipe.texture.width == 1024 && recipe.texture.height == 256 && recipe.native.indexCount == 30));
    // The logo fetch selects ZYXW (dword 3 0x00A80C14): its SRV must swap red
    // and blue back, as the SDK's component mapping does.
    UNSCOPED_INFO("the logo texture and its recipe binding keep the captured ZYXW swizzle");
    REQUIRE((inputs.textureFetch[3] == 0x00A80C14 && recipe.texture.swizzle == std::array<std::uint8_t, 4>{ 2, 1, 0, 3 } &&
             recipe.native.textures[0].swizzle == recipe.texture.swizzle));

    // Every loading-screen draw in the capture (both call sites) carries a
    // fetch the stricter sampler still admits: equal linear filters, a LOD bias
    // a one-level texture ignores, and repeat or edge clamps.
    {
        const auto  table = toml::parse_file((capture / "events.toml").string());
        const auto* all   = table["events"].as_array();
        UNSCOPED_INFO("capture events");
        REQUIRE(all != nullptr);
        std::size_t draws = 0;
        for (const auto& node : *all)
        {
            const auto& event = *node.as_table();
            if (event["kind"].value_or(std::string()) != "draw")
                continue;
            const auto caller = std::stoull(event["caller_lr"].value_or(std::string("0")));
            if (caller != kGfxDrawCaller && caller != kLabelDrawCaller)
                continue;
            const auto                   state = read(capture / event["state_file"].value_or(std::string()));
            std::array<std::uint32_t, 6> fetch{};
            for (std::size_t i = 0; i < fetch.size(); ++i)
                fetch[i] = word(state, 0x480 + i * 4);
            NativeDrawReplaySampler admitted;
            UNSCOPED_INFO("loading draw " + std::to_string(event["sequence"].value_or<std::int64_t>(-1)) + " sampler: " + error);
            REQUIRE((DecodeGfxSampler(fetch, admitted, error) && admitted.magLinear && admitted.minLinear));
            ++draws;
        }
        UNSCOPED_INFO("the capture holds 88 loading-screen draws, found " + std::to_string(draws));
        REQUIRE(draws == 88);
        std::cout << "All " << draws << " captured loading-screen fetches keep an admitted GFx sampler\n";
    }

    // Named refusals on the real inputs, one rule each.
    const auto refused = [&](const char* name, auto mutate, const std::string& expected)
    {
        auto broken = inputs;
        mutate(broken);
        GfxDrawRecipe rejected;
        std::string   reason;
        const bool    built = BuildGfxDrawRecipe(broken, catalog, rejected, reason);
        UNSCOPED_INFO(std::string(name) + ": " + (built ? "accepted" : reason));
        REQUIRE((!built && reason.starts_with(expected)));
    };
    // One mutation per enumerated register: each value outside its table.
    refused("an unpinned blend", [](GfxDrawInputs& in)
            {
                store(in.state, 0x2938, 0x00010706);
            },
            "gfx_blend_control 0x2201: 0x10706");
    refused("an unpinned color mask", [](GfxDrawInputs& in)
            {
                store(in.state, 0x28DC, 0x7);
            },
            "gfx_color_mask 0x2104: 0x7");
    refused("an unadmitted depth control", [](GfxDrawInputs& in)
            {
                store(in.state, 0x2934, 0x24F00776);
            },
            "gfx_depth_control 0x2200: 0x24f00776");
    refused("an alpha test", [](GfxDrawInputs& in)
            {
                store(in.state, 0x293C, 0x8700000C);
            },
            "gfx_color_control 0x2202: 0x8700000c");
    refused("a stencil test with a partial read mask", [](GfxDrawInputs& in)
            {
                store(in.state, 0x2934, 0x24F00271);
                store(in.state, 0x2900, 0x00FF0F01);
            },
            "gfx_stencil_mask 0x210d: 0xff0f01");
    // The fold ORs each program word from both metadata blocks; set a bit
    // neither carries so the folded register changes.
    const auto unsetBit = [](const GfxDrawInputs& in, std::size_t offset)
    {
        const auto both = word(in.vertexMetadata, offset) | word(in.pixelMetadata, offset);
        return both == ~0U ? 0U : 1U << std::countr_one(both);
    };
    refused("a program word the catalog entry does not name", [&](GfxDrawInputs& in)
            {
                store(in.pixelMetadata, 8, word(in.pixelMetadata, 8) | unsetBit(in, 8));
            },
            "gfx_program_state 0x2180: ");
    refused("a shader context the catalog entry does not name", [&](GfxDrawInputs& in)
            {
                store(in.pixelMetadata, 12, word(in.pixelMetadata, 12) | unsetBit(in, 12));
            },
            "gfx_program_state 0x2181: ");
    refused("interpolators the catalog entry does not name", [](GfxDrawInputs& in)
            {
                store(in.pixelMetadata, 24, word(in.pixelMetadata, 24) ^ 0x10000);
            },
            "gfx_program_state 0x2182: ");
    for (const auto [index, value] : kGfxFixedState)
        refused("a changed fixed register", [shadow = shadowOffset(index), value](GfxDrawInputs& in)
                {
                    store(in.state, shadow, value ^ 0x100);
                },
                std::format("gfx_fixed_state {:#x}: {:#x}", index, value ^ 0x100));
    refused("a pixel shader outside the catalog", [](GfxDrawInputs& in)
            {
                in.pixelCode[0] ^= 1;
            },
            "gfx_shader_unpinned ps=");
    {
        // Census B5: the loading pair draws the Hall of Glory panels (key
        // 14AEC15D) into the 928x688 1X surface with a 900x675 viewport and
        // window scissor. It builds only with a single-sample entry.
        const auto panel = [&](std::uint32_t depth, std::uint32_t bottomRight, float width, float height)
        {
            auto in = inputs;
            store(in.state, 0x2880, kGfxOffscreenForm.surfaceInfo);
            store(in.state, 0x2888, depth);
            store(in.state, shadowOffset(0x2081), 0);
            store(in.state, shadowOffset(0x2082), bottomRight);
            store(in.state, shadowOffset(0x210F), std::bit_cast<std::uint32_t>(width / 2));
            store(in.state, shadowOffset(0x2110), std::bit_cast<std::uint32_t>(width / 2));
            store(in.state, shadowOffset(0x2111), std::bit_cast<std::uint32_t>(-height / 2));
            store(in.state, shadowOffset(0x2112), std::bit_cast<std::uint32_t>(height / 2));
            for (const auto [i, value] : std::array<std::pair<unsigned, float>, 4>{ { { 0, 0.0F }, { 1, 0.0F }, { 2, width }, { 3, height } } })
                store(in.state, 0x3168 + i * 4, std::bit_cast<std::uint32_t>(value));
            return in;
        };
        const auto    form = panel(kGfxOffscreenForm.depthInfo, kGfxOffscreenForm.scissor, 900, 675);
        GfxDrawRecipe offscreen;
        UNSCOPED_INFO("the panel form without a 1X entry is refused by name: " + error);
        REQUIRE((!catalog.Find(kGfxPixelWordDigest)->offscreen && !BuildGfxDrawRecipe(form, catalog, offscreen, error) && error == "gfx_offscreen_unpinned"));
        auto single                     = catalog;
        auto loading                    = std::ranges::find_if(single.entries, [](const GfxDrawShader& entry)
                                                               {
                                                return entry.shader.pixelGuestWordDigest == kGfxPixelWordDigest;
                                                               });
        loading->offscreen              = loading->shader;
        loading->offscreen->sampleCount = loading->offscreen->sampleMask = 1;
        UNSCOPED_INFO("the panel form builds at one sample on the 928x688 surface: " + error);
        REQUIRE((BuildGfxDrawRecipe(form, single, offscreen, error) && offscreen.native.width == 928 && offscreen.native.height == 688 &&
                 offscreen.native.sampleCount == 1 && offscreen.native.viewport.width == 900 && offscreen.native.viewport.height == 675 &&
                 offscreen.native.scissor.right == 900 && offscreen.native.scissor.bottom == 675));
        const std::array<std::pair<GfxDrawInputs, std::string>, 4> panels{ {
            { panel(0x530, kGfxOffscreenForm.scissor, 900, 675), "gfx_fixed_state 0x2002: 0x530" },
            { panel(kGfxOffscreenForm.depthInfo, 0x02B003A0, 900, 675), "gfx_offscreen_viewport: scissor 0x0 0x2b003a0 viewport 0,0 900x675" },
            { panel(kGfxOffscreenForm.depthInfo, kGfxOffscreenForm.scissor, 928, 688), "gfx_offscreen_viewport: scissor 0x0 0x2a30384 viewport 0,0 928x688" },
            { panel(kGfxOffscreenForm.depthInfo, 0x02A30380, 896, 675), "gfx_offscreen_viewport: scissor 0x0 0x2a30380 viewport 0,0 896x675" },
        } };
        for (const auto& [in, expected] : panels)
        {
            UNSCOPED_INFO("the panel form is refused as " + expected + ": " + error);
            REQUIRE((!BuildGfxDrawRecipe(in, single, offscreen, error) && error == expected));
        }
        // Another pitch of the same 1X surface is not the panel form.
        auto pitch = form;
        store(pitch.state, 0x2880, kGfxOffscreenForm.surfaceInfo + 80);
        UNSCOPED_INFO("another surface is refused by its pin: " + error);
        REQUIRE((!BuildGfxDrawRecipe(pitch, single, offscreen, error) && error == std::format("gfx_fixed_state 0x2000: {:#x}", kGfxOffscreenForm.surfaceInfo + 80)));

        // The catalog loads a single-sample loading entry as that pair's
        // off-screen specialization; one sample that keeps the 4X mask 9 is
        // refused by the shader directory.
        const auto withEntry = [&](const std::string& tag, bool mask, unsigned copies)
        {
            const TemporaryDirectory scratch("rerevved-gfx-catalog-" + tag);
            const auto               copy = scratch.path / "catalog";
            std::filesystem::copy(gfxCatalog, copy, std::filesystem::copy_options::recursive);
            std::string text;
            {
                std::ifstream in(copy / "shaders.toml", std::ios::binary);
                text.assign(std::istreambuf_iterator<char>(in), {});
            }
            // The entry runs from its [[shaders]] header to the next one.
            const auto at = text.find(std::string(kGfxPixelWordDigest));
            UNSCOPED_INFO("the entry is in the catalog copy");
            REQUIRE(at != std::string::npos);
            const auto begin = text.rfind("[[shaders]]\n", at);
            const auto end   = text.find("[[shaders]]\n", at);
            UNSCOPED_INFO("the entry has its header");
            REQUIRE(begin != std::string::npos);
            auto entry = text.substr(begin, (end == std::string::npos ? text.size() : end) - begin);
            for (const auto& [from, to] : { std::pair<std::string_view, std::string_view>{ "sample_count = 4", "sample_count = 1" },
                                            std::pair<std::string_view, std::string_view>{ "sample_mask = 9", mask ? "sample_mask = 1" : "sample_mask = 9" } })
            {
                const auto field = entry.find(from);
                UNSCOPED_INFO(std::string("the entry names ") + std::string(from));
                REQUIRE(field != std::string::npos);
                entry.replace(field, from.size(), to);
            }
            for (unsigned i = 0; i < copies; ++i)
                text += "\n" + entry;
            {
                std::ofstream out(copy / "shaders.toml", std::ios::binary | std::ios::trunc);
                out << text;
            }
            GfxDrawCatalog loaded;
            std::string    loadError;
            const bool     ok = LoadGfxDrawCatalog(copy, loaded, loadError);
            return std::pair{ ok ? std::optional<GfxDrawCatalog>(std::move(loaded)) : std::nullopt, loadError };
        };
        const auto [pinned, pinnedError] = withEntry("offscreen", true, 1);
        UNSCOPED_INFO("a 1X loading entry is the off-screen specialization: " + pinnedError);
        REQUIRE((pinned && pinned->entries.size() == catalog.entries.size() && pinned->Find(kGfxPixelWordDigest)->offscreen &&
                 pinned->Find(kGfxPixelWordDigest)->offscreen->sampleCount == 1));
        UNSCOPED_INFO("the panel form builds with the loaded entry: " + error);
        REQUIRE((BuildGfxDrawRecipe(form, *pinned, offscreen, error) && offscreen.native.sampleCount == 1));
        // The shader directory itself refuses a repeated specialization.
        const auto twice = withEntry("offscreen-twice", true, 2).second;
        UNSCOPED_INFO("a second 1X loading entry is refused: " + twice);
        REQUIRE(twice == "duplicate gameplay shader specialization");
        const auto mask = withEntry("offscreen-mask", false, 1).second;
        UNSCOPED_INFO("a 1X entry with the 4X mask is refused: " + mask);
        REQUIRE(mask == "unsupported gameplay shader sample state");
    }
    {
        // The stencil forms build with the presented target's stencil test.
        auto masked = inputs;
        store(masked.state, 0x2934, 0x24F00271);
        store(masked.state, 0x2900, 0x00FFFF02);
        GfxDrawRecipe stencil;
        UNSCOPED_INFO("an equal stencil test inside the mask stack builds: " + error);
        REQUIRE((BuildGfxDrawRecipe(masked, catalog, stencil, error) && stencil.native.stencil.enabled && stencil.native.stencil.reference == 2 &&
                 stencil.native.stencil.readMask == 0xFF && stencil.native.stencil.writeMask == 0xFF));
        // A BC2 texture of the same extent is an accepted M2 form.
        auto bc2            = inputs;
        bc2.textureFetch[1] = (bc2.textureFetch[1] & ~0x3FU) | 19;
        store(bc2.state, 0x484, bc2.textureFetch[1]);
        NativeTextureMemoryRange range;
        UNSCOPED_INFO("BC2 range: " + error);
        REQUIRE(GetNativeTextureMemoryRange(bc2.textureFetch, range, error));
        bc2.textureBytes.resize(range.size);
        GfxDrawRecipe block;
        UNSCOPED_INFO("a one-level BC2 texture builds: " + error);
        REQUIRE((BuildGfxDrawRecipe(bc2, catalog, block, error) && block.texture.format == NativeDrawReplayTextureFormat::Bc2));
    }
    refused("a mip chain", [](GfxDrawInputs& in)
            {
                in.textureFetch[4] |= 1U << 6;
                store(in.state, 0x480 + 16, in.textureFetch[4]);
            },
            "gfx_texture_mipmapped");
    refused("a fetch that differs from the state", [](GfxDrawInputs& in)
            {
                in.textureFetch[1] ^= 0x40;
            },
            "gfx_texture_fetch_mismatch");
    // The recipe must refuse a sampler the host cannot match before it builds.
    const auto samplerFetch = [](std::vector<std::pair<std::size_t, std::uint32_t>> edits)
    {
        return [edits](GfxDrawInputs& in)
        {
            for (const auto [index, value] : edits)
            {
                in.textureFetch[index] = value;
                store(in.state, 0x480 + index * 4, value);
            }
        };
    };
    refused("linear magnification, point minification and a +4 LOD bias", samplerFetch({ { 3, 0x00880C14 }, { 4, 0x00080003 } }), "gfx_sampler_lod_bias");
    refused("a gradient adjust with unequal filters", samplerFetch({ { 3, 0x00A00C14 }, { 4, 0x08000003 } }), "gfx_sampler_gradient_adjust");
    refused("a halfway clamp", samplerFetch({ { 0, (inputs.textureFetch[0] & ~(7U << 10)) | (4U << 10) } }), "gfx_sampler_clamp_mode");
    refused("a texture border", samplerFetch({ { 3, inputs.textureFetch[3] | 0x80000000U } }), "gfx_sampler_border_size");
    refused("the OpenGL nearest clamp policy", samplerFetch({ { 1, inputs.textureFetch[1] | 0x800U } }), "gfx_sampler_nearest_clamp_policy");
    refused("32-bit indices", [](GfxDrawInputs& in)
            {
                in.arguments.indexFormat = 4;
            },
            "gfx_unsupported_indexed_up_tuple");
    refused("a nonzero minimum vertex", [](GfxDrawInputs& in)
            {
                in.arguments.minimumVertex = 4;
            },
            "gfx_unsupported_indexed_up_tuple");
    refused("a changed VS instruction", [](GfxDrawInputs& in)
            {
                in.vertexCode[0] ^= 1;
            },
            "gfx_patched_vs_identity");
    refused("incomplete inputs", [](GfxDrawInputs& in)
            {
                in.inputsComplete = false;
            },
            "gfx_inputs_incomplete");
    refused("a predicated shader", [](GfxDrawInputs& in)
            {
                in.state[0x2ABC] |= 0x40;
            },
            "gfx_conditional_or_recorded_shader");
    refused("a two-element declaration", [](GfxDrawInputs& in)
            {
                in.declaration.resize(24);
            },
            "gfx_fetch_count");
    refused("a resource literal", [](GfxDrawInputs& in)
            {
                in.pixelLiterals.assign(28, 0);
                in.pixelLiterals[23] = 1;
            },
            "gfx_shader_resource_literals");
    refused("a viewport shadow that disagrees", [](GfxDrawInputs& in)
            {
                store(in.state, 0x3168 + 8, 0x44000000);
            },
            "gfx_viewport_shadow_scale_mismatch");
    refused("a k_16_16_FLOAT texture", [](GfxDrawInputs& in)
            {
                in.textureFetch[1] = (in.textureFetch[1] & ~0x3FU) | 31;
                store(in.state, 0x484, in.textureFetch[1]);
            },
            "gfx_texture_form");
    std::cout << "GFx recipe inputs equal C10's Original outputs; fixed-state, texture and tuple refusals are named\n";

    // Pinned color-only ended-set roots discard; others refuse.
    std::optional<NativeRenderFrame> presentedFrame;
    std::array<double, 3>            logoReference{};
    {
        constexpr std::uint32_t kClearStack = 0x70001000, kClearColor = 0x70002000, kMrt0 = 0x50263120, kMrt1 = 0x5044C6B0;
        const auto              g     = kGfxTuple[0];
        const auto              roots = frameDraws(capture, 428);
        UNSCOPED_INFO("frame 428 starts with the MRT draw 1895");
        REQUIRE((!roots.empty() && roots[0].event.sequence == 1895 && roots[0].hook == NativeDrawHook::IndexedBuffer && roots[0].event.arguments[0] == g));
        const auto&                  passDraw = roots[0];
        const auto                   pass     = read(capture / "draw-1895.state.be.bin");
        std::vector<const RootDraw*> ended;
        std::size_t                  gfxSource = 0, gfxPatched = 0, labels = 0;
        for (std::size_t i = 1; i < roots.size(); ++i)
        {
            const auto& root  = roots[i];
            const auto  state = read(capture / ("draw-" + std::to_string(root.event.sequence) + ".state.be.bin"));
            if (word(state, 0x3098) != kMrt0 || word(state, 0x309C) != kMrt1)
                continue;
            // The set's draws precede every presented draw of the frame.
            UNSCOPED_INFO("post-pass draw " + std::to_string(root.event.sequence) + " follows a presented draw");
            REQUIRE(ended.size() + 1 == i);
            const auto vs = GfxMicrocodeWordDigest(read(root.event.files.at("vs_code"))), ps = GfxMicrocodeWordDigest(read(root.event.files.at("ps_code")));
            const bool gfx = root.hook == NativeDrawHook::IndexedUp && root.caller == kGfxDrawCaller && ps == kGfxPixelWordDigest;
            gfxSource += gfx && vs == kGfxSourceVertexWordDigest;
            gfxPatched += gfx && vs == kGfxVertexWordDigest;
            labels += root.hook == NativeDrawHook::NonindexedUp && root.caller == kLabelDrawCaller && vs == kLabelVertexWordDigest && ps == kLabelPixelWordDigest;
            ended.push_back(&root);
        }
        UNSCOPED_INFO(std::format("the ended set receives 57 GFx batches, the first before the fetch patch, and one label batch: {} {} {} of {}", gfxSource, gfxPatched, labels, ended.size()));
        REQUIRE((ended.size() == 58 && gfxSource == 1 && gfxPatched == 56 && labels == 1 && ended.front()->event.sequence == kFirst));

        const std::vector<std::uint8_t>* at      = &pass;
        bool                             pin     = true;
        std::uint32_t                    mask    = 3;
        FakeMemory*                      shaders = nullptr;
        AcceptingCompletion              completion;
        NativeFrameAssemblerConfig       config;
        config.completion = &completion;
        // A draw's state as the clears and resolves see it, predicated to a
        // tile while `pin` holds; the clear values (stencil 0, opaque black).
        config.readTarget = [&](std::uint32_t address, std::uint32_t& out)
        {
            const std::map<std::uint32_t, std::uint32_t> values{ { kClearStack + 92, 0 },
                                                                 { kClearColor, 0 },
                                                                 { kClearColor + 4, 0 },
                                                                 { kClearColor + 8, 0 },
                                                                 { kClearColor + 12, std::bit_cast<std::uint32_t>(1.0F) } };
            const std::map<std::uint32_t, std::uint32_t> pinned{ { g + 0x31A4, mask }, { g + 0x31AC, 1 } };
            if (const auto found = values.find(address); found != values.end())
            {
                out = found->second;
                return true;
            }
            if (const auto found = pinned.find(address); pin && found != pinned.end())
            {
                out = found->second;
                return true;
            }
            return stateWord(*at, g, address, out);
        };
        // The presented draws build through the boot builder over their own
        // captured memory.
        FakeMemory* drawMemory = nullptr;
        auto        boot       = MakeBootDrawBuilder(testing::MirrorFixtureReads(ReadMemory([&drawMemory](std::uint64_t address, std::uint64_t size, bool physical)
                                                                                            {
                                                                                   return drawMemory ? drawMemory->Read(address, size, physical) : std::vector<std::uint8_t>{};
                                                                                            })),
                                                     std::make_shared<NativeGameplayShaderInterface>(panelShader),
                                                     std::make_shared<NativeGameplayShaderInterface>(labelShader),
                                                     std::make_shared<GfxDrawCatalog>(catalog));
        // Draw 1895 stands in with its decoded set's shape and no depth work;
        // the resolve frames replay it, and nothing renders them here.
        config.build = [&](const NativeDrawSubmission& submission, NativeDrawReplayRecipe& draw, std::string& detail)
        {
            if (submission.caller != passDraw.caller)
                return drawMemory && boot(submission, draw, detail);
            GuestTarget set;
            if (!ReadGuestTargetSet(config.readTarget, g, 0, set, &detail))
                return false;
            draw                  = {};
            draw.schemaVersion    = 2;
            draw.width            = set.native.width;
            draw.height           = set.native.height;
            draw.sampleCount      = set.native.sampleCount;
            draw.sampleMask       = 9;
            draw.targetFormat     = set.native.format;
            draw.extraTargetCount = set.native.extraColorTargets;
            for (std::uint32_t i = 0; i < set.native.extraColorTargets; ++i)
                draw.extraTargets[i].format = set.native.extraFormats[i];
            draw.vertexShaderHash = 1;
            draw.pixelShaderHash  = 2;
            draw.vertexShaderDxil.resize(4);
            draw.pixelShaderDxil.resize(4);
            draw.vertexAttributeCount = 2;
            draw.vertexStrideBytes    = 32;
            draw.vertexData.resize(3 * 32);
            draw.indices    = { 0, 1, 2 };
            draw.indexCount = 3;
            draw.vertexConstants.resize(256 * 16);
            draw.pixelConstants.resize(224 * 16);
            draw.sharedConstants.resize(336);
            draw.viewport = { 0, 0, float(set.native.width), float(set.native.height), 0, 1 };
            draw.scissor  = { 0, 0, std::int32_t(set.native.width), std::int32_t(set.native.height) };
            return true;
        };
        config.readShaders = [&](const NativeDrawSubmission& submission, NativeDrawShaderWords& words, std::string& error)
        {
            if (!shaders)
            {
                error = "fixture_no_shader_memory";
                return false;
            }
            return ReadNativeDrawShaderWords(submission, readerFor(*shaders), words, error);
        };
        // The draw's whole captured memory: the host-patched VS digest reads the declaration and the
        // fetch records, and an indexed-UP draw's stride is a word of its caller's frame.
        const auto endedMemory = [&](const RootDraw& root)
        {
            return memoryAt(capture, root.event, root.hook == NativeDrawHook::IndexedUp ? kGfxStride : root.event.arguments[4]);
        };
        const auto submit = [](NativeFrameAssembler& assembler, NativeDrawSubmission submission, std::uint64_t sequence)
        {
            submission.stateRead = true;
            submission.sequence  = sequence;
            assembler.Began(submission);
            const auto verdict = assembler.Admit(submission);
            assembler.Recorded(submission, verdict);
            return verdict;
        };
        const auto rootSubmission = [](const RootDraw& root)
        {
            NativeDrawSubmission submission;
            submission.hook      = root.hook;
            submission.caller    = root.caller;
            submission.stack     = kStack;
            submission.arguments = root.event.arguments;
            return submission;
        };
        // Copy control per tile and slot: depth k1, colors k01, clear bits on tiles 0 and 1.
        constexpr std::array<std::array<std::uint32_t, 3>, 3> controls{ { { 0x00100014, 0x00100340, 0x00100141 },
                                                                          { 0x00100014, 0x00100340, 0x00100141 },
                                                                          { 0x00100014, 0x00100040, 0x00100041 } } };
        constexpr std::array<std::uint32_t, 3>                slots{ kGuestDepthSlot, 0, 1 };
        // Draw 1895 is sequence 1; returns the next draw sequence.
        const auto runPass = [&](NativeFrameAssembler& assembler)
        {
            std::string detail;
            at      = &pass;
            pin     = true;
            mask    = 3;
            shaders = nullptr;
            for (const auto& [index, flags] : { std::pair{ 0U, 0x3FU }, std::pair{ 1U, 0x0FU } })
            {
                GuestClear clear;
                UNSCOPED_INFO("MRT clear " + std::to_string(index) + ": " + detail);
                REQUIRE((ReadGuestClear(config.readTarget, { g, flags, index, 0, 0, 1280, 720, kClearColor }, kClearStack, 1.0, clear, &detail) &&
                         assembler.Clear(clear, detail) != 0));
            }
            // Draw 1895 itself is unpredicated: every tile replays it.
            pin = false;
            UNSCOPED_INFO("draw 1895 draws the pass: " + assembler.Failure().value_or(""));
            REQUIRE(!submit(assembler, rootSubmission(passDraw), 1));
            pin = true;
            for (std::uint32_t tile = 0; tile < 3; ++tile)
                for (std::size_t n = 0; n < slots.size(); ++n)
                {
                    mask = 3U << (2 * tile);
                    GuestTarget target;
                    UNSCOPED_INFO("MRT source: " + detail);
                    REQUIRE(ReadGuestTargetSet(config.readTarget, g, slots[n], target, &detail));
                    GuestResolveCopy copy;
                    copy.copyControl = controls[tile][n];
                    copy.rectX = copy.destX = word(pass, 0x31D0 + 16 * tile);
                    copy.rectWidth          = word(pass, 0x31D8 + 16 * tile) - copy.rectX;
                    copy.rectHeight         = 720;
                    bool partial            = false;
                    UNSCOPED_INFO(std::format("tile {} slot {}: {}", tile, slots[n], detail));
                    REQUIRE(assembler.Resolve(target, copy, false, partial, detail) != 0);
                }
            pin = false;
            return std::uint64_t(2);
        };
        // One draw after the pass, on its own state and shader memory.
        const auto draw = [&](NativeFrameAssembler& assembler, const NativeDrawSubmission& submission, const std::vector<std::uint8_t>& state, FakeMemory& memory, std::uint64_t sequence)
        {
            at      = &state;
            shaders = &memory;
            return submit(assembler, submission, sequence);
        };
        {
            NativeFrameAssembler assembler(config);
            auto                 sequence = runPass(assembler);
            for (const auto* root : ended)
            {
                const auto state  = read(capture / ("draw-" + std::to_string(root->event.sequence) + ".state.be.bin"));
                auto       memory = endedMemory(*root);
                UNSCOPED_INFO(std::format("post-pass draw {} is discarded: {}", root->event.sequence, assembler.Failure().value_or("")));
                REQUIRE(!draw(assembler, rootSubmission(*root), state, memory, sequence++));
            }
            auto swap            = swapRecord();
            swap.numberedThrough = sequence - 1;
            const auto kept      = assembler.Swap(swap);
            UNSCOPED_INFO(std::format("nine resolve frames and no presented work: {} {} admitted {} spans {}", NativeFrameStatusName(kept.status), kept.detail, kept.coverage.admitted, completion.submitted));
            REQUIRE((kept.status == NativeFrameStatus::Empty && kept.coverage.admitted == 59 && completion.submitted == 10 && !assembler.Failure()));
        }
        std::cout << "Frame 428's MRT pass and all " << ended.size() << " post-pass draws (57 GFx, 1 label) are admitted; the post-pass draws are discarded\n";

        // Frame 428 presents root draws 2904 and 2908 after tile and depth
        // clears.
        {
            std::vector<const RootDraw*> presented;
            for (std::size_t i = ended.size() + 1; i < roots.size(); ++i)
                presented.push_back(&roots[i]);
            UNSCOPED_INFO("frame 428 presents GFx draw 2904 and label draw 2908");
            REQUIRE((presented.size() == 2 && presented[0]->event.sequence == 2904 && presented[0]->caller == kGfxDrawCaller &&
                     presented[1]->event.sequence == 2908 && presented[1]->caller == kLabelDrawCaller));
            const auto stateOf = [&](const RootDraw& root)
            {
                return read(capture / ("draw-" + std::to_string(root.event.sequence) + ".state.be.bin"));
            };
            const auto first = stateOf(*ended.front()), last = stateOf(*ended.back()), gfxState = stateOf(*presented[0]),
                       labelState = stateOf(*presented[1]);
            const auto clearOn    = [&](NativeFrameAssembler& assembler, const std::vector<std::uint8_t>& state, std::uint32_t flags, std::uint32_t index, std::uint32_t width, std::uint32_t color, const char* name)
            {
                at = &state;
                std::string detail;
                GuestClear  clear;
                UNSCOPED_INFO(std::string(name) + " is admitted: " + detail);
                REQUIRE((ReadGuestClear(config.readTarget, { g, flags, index, 0, 0, width, 720, color }, kClearStack, 1.0, clear, &detail) &&
                         assembler.Clear(clear, detail) != 0 && detail.empty()));
            };
            completion.submitted = 0;
            NativeFrameAssembler assembler(config);
            auto                 sequence = runPass(assembler);
            // Capture misses 0x8250E718 depth clears; adjacent draw state
            // supplies them.
            clearOn(assembler, first, 0x30, kGuestDepthOnlyClearIndex, 448, 0, "the ended set's first depth clear");
            for (const auto* root : ended)
            {
                const auto state  = stateOf(*root);
                auto       memory = endedMemory(*root);
                UNSCOPED_INFO(std::format("post-pass draw {} is discarded: {}", root->event.sequence, assembler.Failure().value_or("")));
                REQUIRE(!draw(assembler, rootSubmission(*root), state, memory, sequence++));
            }
            clearOn(assembler, last, 0x30, kGuestDepthOnlyClearIndex, 448, 0, "the ended set's closing depth clear");
            pin  = true;
            mask = 3;
            clearOn(assembler, gfxState, 0x3F, 0, 1280, kClearColor, "the presented tile-0 clear");
            pin = false;
            clearOn(assembler, gfxState, 0x30, kGuestDepthOnlyClearIndex, 1280, 0, "the presented depth clear");
            for (const auto* root : presented)
            {
                const auto state  = stateOf(*root);
                auto       memory = memoryAt(capture, root->event, root->caller == kGfxDrawCaller ? kGfxStride : 28);
                drawMemory        = &memory;
                UNSCOPED_INFO(std::format("presented draw {} is admitted: {}", root->event.sequence, assembler.Failure().value_or("")));
                REQUIRE(!draw(assembler, rootSubmission(*root), state, memory, sequence++));
                drawMemory = nullptr;
            }
            clearOn(assembler, labelState, 0x30, kGuestDepthOnlyClearIndex, 1280, 0, "the presented closing depth clear");
            pin = true;
            // Prior scanout clear words initialize tile 0; its clear bits seed
            // tile 1.
            const auto frameStart = stateOf(roots.front());
            for (std::uint32_t tile = 0; tile < 2; ++tile)
            {
                mask = 3U << (2 * tile);
                std::string detail;
                GuestTarget target;
                UNSCOPED_INFO("scanout source: " + detail);
                REQUIRE(ReadGuestTarget(config.readTarget, g, 0, target, &detail));
                GuestResolveCopy copy;
                copy.copyControl = tile ? 0x00100040 : 0x00100340;
                copy.destBase    = kFrontbuffer;
                copy.destPitch   = kScanoutDestPitch;
                copy.destInfo    = kScanoutDestInfo;
                // Prior scanout G+0x2A2C depth/G+0x2A30 color are far/opaque
                // black; tile 0 clears seed tile 1.
                copy.depthClear = word(frameStart, 0x2A2C);
                copy.colorClear = word(frameStart, 0x2A30);
                copy.rectX = copy.destX = tile * 640;
                copy.rectWidth          = 640;
                copy.rectHeight         = 720;
                bool partial            = false;
                UNSCOPED_INFO(std::format("tile {} scanout: {}", tile, detail));
                REQUIRE((assembler.Resolve(target, copy, true, partial, detail) != 0 && detail.empty()));
            }
            pin                  = false;
            auto swap            = swapRecord();
            swap.fetch[3]        = kSwapFetchWord3;
            swap.numberedThrough = sequence - 1;
            const auto frame     = assembler.Swap(swap);
            UNSCOPED_INFO(std::format("the presented frame follows the discarded set: {} {} admitted {} spans {}", NativeFrameStatusName(frame.status), frame.detail, frame.coverage.admitted, completion.submitted));
            REQUIRE((frame.status == NativeFrameStatus::Provisional && frame.hostFence == 10 && completion.last && frame.coverage.admitted == 61 &&
                     completion.submitted == 10 && !assembler.Failure()));
            std::vector<std::size_t> draws;
            std::string              shape;
            for (const auto& pass : completion.last->passes)
            {
                draws.push_back(pass.DrawCount());
                shape += std::format(" {}x{} depth {:X} clears {} draws {} resolves {}", pass.target.width, pass.target.height, pass.target.depthEdramBase, pass.clears.size(), pass.DrawCount(), pass.resolves.size());
            }
            UNSCOPED_INFO("one pass holds the GFx and label draws on the presented depth:" + shape);
            REQUIRE((draws == std::vector<std::size_t>{ 2, 0 } && completion.last->passes[0].target.depthEdramBase == 0x530));
            // The swapped scanout undoes the ZYX1 fetch's red/blue exchange.
            UNSCOPED_INFO("the scanout's red/blue swap composes into the swap fetch swizzle");
            REQUIRE((completion.last->outputSwizzle == std::array<std::uint8_t, 4>{ 0, 1, 2, 5 }));
            presentedFrame = std::move(completion.last);
            std::array<std::uint32_t, 6> fetch{};
            for (std::size_t i = 0; i < fetch.size(); ++i)
                fetch[i] = word(gfxState, 0x480 + i * 4);
            logoReference = presentedTextureColor(fetch, read(presented[0]->event.files.at("sampled_base")), kScanoutDestInfo, kSwapFetchWord3);
        }
        std::cout << "Frame 428 runs from draw 1895 through the ended set's depth clears and post-pass draws to the presented scanout and swap\n";

        // One refused post-pass draw: draw 2674 with one input changed.
        const auto& second  = **std::find_if(ended.begin(), ended.end(), [](const RootDraw* root)
                                             {
                                                return root->event.sequence == kSecond;
                                             });
        const auto  refusal = [&](const char* name, auto mutate, const std::string& expected)
        {
            auto state      = read(capture / "draw-2674.state.be.bin");
            auto memory     = endedMemory(second);
            auto submission = rootSubmission(second);
            mutate(state, memory, submission);
            NativeFrameAssembler assembler(config);
            const auto           sequence = runPass(assembler);
            const auto           verdict  = draw(assembler, submission, state, memory, sequence);
            auto                 swap     = swapRecord();
            swap.numberedThrough          = sequence;
            const auto failed             = assembler.Swap(swap);
            UNSCOPED_INFO(std::string(name) + " is refused: " + failed.detail);
            REQUIRE((verdict && failed.status == NativeFrameStatus::Failed && failed.detail.find(expected) != std::string::npos));
        };
        // The bytes one shader payload holds in the draw's memory.
        const auto payload = [&](FakeMemory& memory, const char* role) -> std::vector<std::uint8_t>&
        {
            for (auto& region : memory.regions)
                if (region.address == second.event.addresses.at(role) && region.physical == second.event.physical.at(role))
                    return region.bytes;
            throw std::runtime_error(std::string("payload ") + role);
        };
        refusal("a depth-writing draw into the ended set", [](std::vector<std::uint8_t>& state, FakeMemory&, NativeDrawSubmission&)
                {
                    store(state, 0x2934, 0x24F00776);
                },
                "ended_set_write_unsupported: depth_control 24F00776 color_mask 0xff");
        // Conditional or recorded shader modes and occlusion query state.
        refusal("a conditional VS", [](std::vector<std::uint8_t>& state, FakeMemory&, NativeDrawSubmission&)
                {
                    store(state, 0x2ABC, word(state, 0x2ABC) | 0x40000000);
                },
                "ended_set_conditional_or_recorded_shader: 40042400");
        refusal("a recorded shader mode", [](std::vector<std::uint8_t>& state, FakeMemory&, NativeDrawSubmission&)
                {
                    store(state, 0x2ABC, word(state, 0x2ABC) | 0x4000);
                },
                "ended_set_conditional_or_recorded_shader: 00046400");
        refusal("a sample count control", [](std::vector<std::uint8_t>& state, FakeMemory&, NativeDrawSubmission&)
                {
                    store(state, 0x2A48, 0);
                },
                "ended_set_query_state: sample_count_ctl 00000000 addr 00000000");
        refusal("a sample count address", [](std::vector<std::uint8_t>& state, FakeMemory&, NativeDrawSubmission&)
                {
                    store(state, 0x2A4C, 0x1FC00000);
                },
                "ended_set_query_state: sample_count_ctl 00000002 addr 1FC00000");
        // Any other PS, a memory-exporting one included, behind the GFx VS.
        auto foreignPs = read(second.event.files.at("ps_code"));
        foreignPs[3] ^= 1;
        refusal("a foreign PS", [&](std::vector<std::uint8_t>&, FakeMemory& memory, NativeDrawSubmission&)
                {
                    payload(memory, "ps_code") = foreignPs;
                },
                std::format("ended_set_shader_unpinned: vs {} ps {}", kGfxVertexWordDigest, GfxMicrocodeWordDigest(foreignPs)));
        auto foreignVs = read(second.event.files.at("vs_code"));
        foreignVs[3] ^= 1;
        refusal("a foreign VS", [&](std::vector<std::uint8_t>&, FakeMemory& memory, NativeDrawSubmission&)
                {
                    payload(memory, "vs_code") = foreignVs;
                },
                std::format("ended_set_shader_unpinned: vs {} ps {}", GfxMicrocodeWordDigest(foreignVs), kGfxPixelWordDigest));
        refusal("the GFx pair from the label call site", [](std::vector<std::uint8_t>&, FakeMemory&, NativeDrawSubmission& submission)
                {
                    // The nonindexed-UP lowerer takes its stride from r7, which the GFx call passes elsewhere.
                    submission.hook         = NativeDrawHook::NonindexedUp;
                    submission.caller       = kLabelDrawCaller;
                    submission.arguments[4] = kGfxStride;
                },
                "ended_set_shader_unpinned");
        refusal("unreadable PS words", [&](std::vector<std::uint8_t>&, FakeMemory& memory, NativeDrawSubmission&)
                {
                    payload(memory, "ps_code").resize(8);
                },
                "ended_set_shader_unreadable: pixel_microcode_extent");
        refusal("unreadable VS words", [&](std::vector<std::uint8_t>&, FakeMemory& memory, NativeDrawSubmission&)
                {
                    payload(memory, "vs_code").resize(8);
                },
                "ended_set_shader_unreadable: adapter_linkage_input_extent");
        std::cout << "A depth write, a conditional or recorded shader mode, query state, a foreign VS or PS, another call site or unreadable shader words into the ended set are refused by name\n";
    }

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
    {
        std::cout << "No D3D12 device; the loading frame was not rendered\n";
        return 77;
    }

    // Acceptance: clear, the GFx quads and the label batch of loading frame
    // 429 through the boot builder and assembler equal a control frame.
    GuestClear clear;
    clear.target.native   = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    clear.flags           = 0x3F;
    clear.clear.rectangle = { 0, 0, 1280, 720 };
    clear.clear.color = clear.clear.depth = clear.clear.stencil = true;
    clear.clear.colorValue                                      = { 0, 0, 0, 1 };
    clear.depth                                                 = GuestDepthSurface{ 0x00700000, 0x530, 0x530, 1280, 720, 4, false };
    const auto& labelEvent                                      = events.at(kLabel);
    const auto  assembled                                       = [&](const Mutation& mutation)
    {
        auto gfx = std::make_shared<GfxDrawCatalog>(catalog);
        if (mutation.swapUvColor)
            for (auto& entry : gfx->entries)
                if (entry.shader.pixelGuestWordDigest == kGfxPixelWordDigest)
                    std::swap(entry.shader.vertexAttributes[1].offsetBytes, entry.shader.vertexAttributes[2].offsetBytes);
        auto gfxAt = memoryAt(capture, gfxEvent, kGfxStride);
        if (mutation.endianZero)
            for (auto& region : gfxAt.regions)
                if (region.address == gfxEvent.arguments[0] && !region.physical)
                    region.bytes[0x480 + 7] &= 0x3F;
        auto                       labelAt = memoryAt(capture, labelEvent, 28);
        FakeMemory*                current = &gfxAt;
        auto                       boot    = MakeBootDrawBuilder(testing::MirrorFixtureReads(ReadMemory([&current](std::uint64_t address, std::uint64_t size, bool physical)
                                                                                                        {
                                                                                   return current->Read(address, size, physical);
                                                                                                        })),
                                                                 std::make_shared<NativeGameplayShaderInterface>(panelShader),
                                                                 std::make_shared<NativeGameplayShaderInterface>(labelShader),
                                                                 gfx);
        NativeFrameAssemblerConfig config;
        config.readback = true;
        // Each draw decodes its target set from its own captured state: the
        // saved 640x720 tile surface standing for the presented target.
        GuestTarget decoded;
        UNSCOPED_INFO("the loading draws decode the presented target");
        REQUIRE((ReadGuestTargetSet([&](std::uint32_t address, std::uint32_t& out)
                                    {
                                        return stateWord(current->Read(kGfxTuple[0], 0x3500, false), kGfxTuple[0], address, out);
                                    },
                                    kGfxTuple[0],
                                    0,
                                    decoded) &&
                 decoded.native == clear.target.native && decoded.tiling && decoded.colorCount == 1));
        config.readTarget = [&current](std::uint32_t address, std::uint32_t& out)
        {
            return stateWord(current->Read(kGfxTuple[0], 0x3500, false), kGfxTuple[0], address, out);
        };
        config.build = [&](const NativeDrawSubmission& submission, NativeDrawReplayRecipe& draw, std::string& detail)
        {
            if (!boot(submission, draw, detail))
                return false;
            if (mutation.blendOff && submission.caller == kGfxDrawCaller)
                draw.blend.enabled = false;
            return true;
        };
        NativeFrameAssembler assembler(config);
        std::string          detail;
        UNSCOPED_INFO("clear admitted: " + detail);
        REQUIRE(assembler.Clear(clear, detail) != 0);
        const auto submit = [&](NativeDrawSubmission submission, std::uint64_t sequence)
        {
            submission.stateRead = true;
            submission.sequence  = sequence;
            assembler.Began(submission);
            const auto verdict = assembler.Admit(submission);
            UNSCOPED_INFO("draw admitted");
            REQUIRE(!verdict);
            assembler.Recorded(submission, verdict);
        };
        submit(gfxSubmission(gfxEvent.arguments), 1);
        current = &labelAt;
        NativeDrawSubmission label;
        label.hook      = NativeDrawHook::NonindexedUp;
        label.caller    = kLabelDrawCaller;
        label.stack     = kStack;
        label.arguments = labelEvent.arguments;
        submit(label, 2);
        // An unswapped scanout, so the control frame's fetch swizzle stands.
        GuestTarget scanned;
        scanned.native = clear.target.native;
        GuestResolveCopy scan;
        scan.copyControl = 0x00100040;
        scan.destBase    = kFrontbuffer;
        scan.destPitch   = kScanoutDestPitch;
        scan.destInfo    = 0x300;
        scan.rectWidth   = 1280;
        scan.rectHeight  = 720;
        bool partial     = false;
        UNSCOPED_INFO("the control frame is scanned out: " + detail);
        REQUIRE(assembler.Resolve(scanned, scan, true, partial, detail) != 0);
        auto swap            = swapRecord();
        swap.numberedThrough = 2;
        auto frame           = assembler.Swap(swap);
        UNSCOPED_INFO("one presented pass with the GFx and label draws: " + frame.detail);
        REQUIRE((frame.status == NativeFrameStatus::Provisional && frame.frame && frame.frame->passes.size() == 1 &&
                 frame.frame->passes[0].DrawCount() == 2));
        return std::move(*frame.frame);
    };

    NativeDrawReplayTexture logo, glyphs;
    NativeDrawReplaySampler logoSampler, glyphSampler;
    const auto              controlGfx   = controlDraw(capture, gfxEvent, true, gfxShader, logo, logoSampler);
    const auto              controlLabel = controlDraw(capture, labelEvent, false, labelShader, glyphs, glyphSampler);
    NativeRenderFrame       controlFrame;
    controlFrame.textures.push_back({ 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 });
    NativeRenderFramePass pass;
    pass.target = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    NativeRenderFramePass::Clear full;
    full.rectangle = { 0, 0, 1280, 720 };
    full.color = full.depth = full.stencil = true;
    full.colorValue                        = { 0, 0, 0, 1 };
    pass.clears.push_back(full);
    pass.draws.push_back(controlGfx);
    pass.draws.push_back(controlLabel);
    NativeRenderFrameResolve resolve;
    resolve.textureId = 1;
    resolve.width     = 1280;
    resolve.height    = 720;
    resolve.samples   = { 0, 3 };
    pass.resolves.push_back(resolve);
    controlFrame.passes.push_back(std::move(pass));
    controlFrame.outputTextureId      = 1;
    controlFrame.gammaTable           = swapRecord().gammaTable;
    controlFrame.returnResolvedOutput = true;
    auto labelOnly                    = controlFrame;
    labelOnly.passes[0].draws.erase(labelOnly.passes[0].draws.begin());

    NativeRendererD3D12 renderer;
    UNSCOPED_INFO("native stream: " + error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto                            expected = renderer.SubmitRenderFrame(std::move(controlFrame));
    const auto                            base     = renderer.SubmitRenderFrame(std::move(labelOnly));
    const auto                            actual   = renderer.SubmitRenderFrame(assembled({}));
    const std::array<Mutation, 3>         mutations{ { { true, false, false }, { false, true, false }, { false, false, true } } };
    std::array<NativeDrawReplayResult, 3> mutated;
    for (std::size_t i = 0; i < mutations.size(); ++i)
        mutated[i] = renderer.SubmitRenderFrame(assembled(mutations[i]));
    renderer.Shutdown();
    UNSCOPED_INFO("renders: " + expected.error + base.error + actual.error);
    REQUIRE((expected.success && base.success && actual.success));
    std::size_t lit = 0;
    for (std::size_t i = 0; i + 3 < expected.resolvedOutput.size(); i += 4)
        lit += !std::equal(expected.resolvedOutput.begin() + i, expected.resolvedOutput.begin() + i + 4, base.resolvedOutput.begin() + i);
    std::cout << "GFx pixels over the label frame: " << lit << "\n";
    UNSCOPED_INFO("the control GFx quads change the frame");
    REQUIRE(lit > 1000);
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i + 3 < expected.resolvedOutput.size(); i += 4)
        mismatches += !std::equal(expected.resolvedOutput.begin() + i, expected.resolvedOutput.begin() + i + 4, actual.resolvedOutput.begin() + i);
    UNSCOPED_INFO("assembled loading frame differs from the control at " + std::to_string(mismatches) + " pixels");
    REQUIRE((actual.resolvedOutput.size() == expected.resolvedOutput.size() && mismatches == 0));
    constexpr std::array<const char*, 3> names{ "UV and color offsets swapped", "texture endian 0", "blend disabled" };
    for (std::size_t i = 0; i < mutated.size(); ++i)
    {
        UNSCOPED_INFO(std::string(names[i]) + " render: " + mutated[i].error);
        REQUIRE(mutated[i].success);
        UNSCOPED_INFO(std::string(names[i]) + " leaves the control comparison passing");
        REQUIRE(mutated[i].resolvedOutput != expected.resolvedOutput);
    }
    std::cout << "Assembled clear, GFx quads and label batch equal the control bit-exactly; mutations fail it\n";

    // Logo balance depends on raw fetch words and the composed scanout swap.
    {
        UNSCOPED_INFO("frame 428 was assembled");
        REQUIRE(presentedFrame.has_value());
        presentedFrame->returnGpuOutput      = false;
        presentedFrame->returnResolvedOutput = true;
        NativeRendererD3D12 presenter;
        UNSCOPED_INFO("presented stream: " + error);
        REQUIRE(presenter.StartDrawStream(error));
        const auto presented = presenter.SubmitRenderFrame(std::move(*presentedFrame));
        presenter.Shutdown();
        UNSCOPED_INFO("frame 428 renders: " + presented.error);
        REQUIRE((presented.success && presented.resolvedOutput.size() == 1280U * 720U * 4U));
        std::size_t colored  = 0;
        const auto  balance  = presentedRedBlueBalance(presented.resolvedOutput, colored);
        const auto  expected = (logoReference[0] - logoReference[2]) / (logoReference[0] + logoReference[2]);
        std::cout << std::format("Frame 428 presented red-blue balance {:.3f} over {} colored pixels; texture reference {:.3f} (RGB {:.1f} {:.1f} {:.1f})\n",
                                 balance,
                                 colored,
                                 expected,
                                 logoReference[0],
                                 logoReference[1],
                                 logoReference[2]);
        UNSCOPED_INFO("the presented logo keeps the texture's red over blue");
        REQUIRE((expected > 0.1 && colored > 1000 && balance > 0.1));
    }
    return 0;
}

// Harvest admits gameplay and label pairs; unpinned forms report
// gfx_shader_unpinned.
constexpr std::string_view kSolidPanelPixelWords = "f15792acab07e97384dbaff573da53b2641cedd0e4ecbf1c5f3e1b34d2171921";

int harvest(const std::string& roots, const std::filesystem::path& shaders)
{
    GfxDrawCatalog catalog;
    std::string    error;
    UNSCOPED_INFO("harvest GFx catalog: " + error);
    REQUIRE(LoadGfxDrawCatalog(shaders, catalog, error));
    // The off-screen label catalog, when set, adds the 256x32 form's 1X entry.
    LabelDrawShaders labelShaders;
    const auto       offscreenCatalog = environment("REREVVED_LABEL_OFFSCREEN_SHADERS");
    const auto       labelCatalog     = offscreenCatalog.empty() ? environment("REREVVED_LABEL_SHADERS") : offscreenCatalog;
    UNSCOPED_INFO("the harvest checks also need REREVVED_LABEL_SHADERS: " + error);
    REQUIRE((!labelCatalog.empty() && LoadLabelDrawShaders(labelCatalog, labelShaders, error)));
    const auto address = [](const toml::node_view<const toml::node>& node)
    {
        return static_cast<std::uint32_t>(std::stoull(node.value_or(std::string("0")), nullptr, 16));
    };
    // The base fields every 0x826A3568 and 0x826A3000 record carries.
    const auto shared = [&](SolidDrawInputs& in, const std::filesystem::path& directory, const toml::node_view<const toml::node>& record)
    {
        in.state              = read(directory / "state.be.bin");
        in.vertexCode         = read(directory / "vs_code.be.bin");
        in.pixelCode          = read(directory / "ps_code.be.bin");
        in.vertexObject       = address(record["vertex_object"]);
        in.pixelObject        = address(record["pixel_object"]);
        in.declarationAddress = address(record["declaration_address"]);
        in.vertexHeader       = hexBytes(record["vertex_header"].value_or(std::string()));
        in.vertexMetadata     = hexBytes(record["vertex_metadata"].value_or(std::string()));
        in.pixelHeader        = hexBytes(record["pixel_header"].value_or(std::string()));
        in.pixelMetadata      = hexBytes(record["pixel_metadata"].value_or(std::string()));
        in.fetchRecords       = hexBytes(record["fetch_records"].value_or(std::string()));
        in.fetchTemplates     = hexBytes(record["fetch_templates"].value_or(std::string()));
        in.declarationHeader  = hexBytes(record["declaration_header"].value_or(std::string()));
        in.declaration        = hexBytes(record["declaration"].value_or(std::string()));
        in.swizzleTable       = hexBytes(kFetchSwizzleTable);
        in.formatTable        = hexBytes(kFetchFormatTable);
        in.inputsComplete     = true;
    };
    const auto label = [&](const std::filesystem::path& directory, const std::string& name)
    {
        const auto table  = toml::parse_file((directory / "record.toml").string());
        const auto record = toml::node_view<const toml::node>(table);
        UNSCOPED_INFO(name + " is the label pair");
        REQUIRE(record["pixel_word_sha256"].value_or(std::string()) == kLabelPixelWordDigest);
        LabelDrawInputs              in;
        std::array<std::uint32_t, 8> a{};
        for (std::size_t i = 0; i < a.size(); ++i)
            a[i] = address(record["arguments_r3_r10"][i]);
        in.arguments = { a[0], a[1], 0, a[2], 0, 0, 0, a[3], a[4], kLabelDrawCaller };
        shared(in, directory, record);
        in.vertices.assign(std::size_t(a[2]) * a[4], 0);
        for (std::size_t i = 0; i < in.textureFetch.size(); ++i)
            in.textureFetch[i] = word(in.state, 0x480 + i * 4);
        NativeTextureMemoryRange range;
        if (GetNativeTextureMemoryRange(in.textureFetch, range, error))
        {
            in.textureAddress = range.address;
            in.textureBytes.assign(range.size, 0);
        }
        in.labelCandidate = true;
        LabelDrawRecipe recipe;
        const bool      ok        = BuildLabelDrawRecipe(in, labelShaders, recipe, error);
        const auto      registers = GuestDrawRegisters(in.state);
        // Census B8: key 735031E1 draws the label pair into the 256x32 target.
        if (registers[0x2000] == kLabelOffscreenForm.surfaceInfo)
        {
            if (!labelShaders.offscreen)
            {
                UNSCOPED_INFO(name + " is refused without a 1X entry: " + error);
                REQUIRE((!ok && error == "label_offscreen_unpinned"));
                return "label off-screen unpinned";
            }
            UNSCOPED_INFO(name + " builds off-screen: " + error);
            REQUIRE((ok && recipe.native.width == 256 && recipe.native.height == 32 && recipe.native.sampleCount == 1));
            return "label built off-screen";
        }
        if (registers[0x2000] != kLabelPresentedForm.surfaceInfo)
        {
            UNSCOPED_INFO(name + " is refused by its surface: " + error);
            REQUIRE((!ok && error == std::format("label_fixed_state 0x2000: {:#x}", registers[0x2000])));
            std::cout << std::format("{}: refused {}\n", name, error);
            return "label refused by its surface";
        }
        UNSCOPED_INFO(name + " builds from its harvested state: " + error);
        REQUIRE(ok);
        return "label built";
    };
    // Frame-form label draws inside a Scaleform mask (RB_DEPTHCONTROL
    // 0x24F00271), from the event's own state, arguments and vertices.
    const auto maskedLabels = [&](const std::filesystem::path& root, std::string_view frameName)
    {
        const auto  frame  = toml::parse_file((root / "frames" / std::string(frameName) / "frame.toml").string());
        const auto* events = frame["events"].as_array();
        UNSCOPED_INFO(std::string(frameName) + " events");
        REQUIRE(events != nullptr);
        std::set<std::uint32_t> references;
        for (const auto& node : *events)
        {
            const auto event = toml::node_view<const toml::node>(node);
            if (event["kind"].value_or(std::string()) != "draw" || event["caller"].value_or(std::string()) != "0x82304258")
                continue;
            auto state = read(root / event["state"].value_or(std::string()));
            if (word(state, 0x2934) != kLabelMaskedDepthControl)
                continue;
            const auto name      = std::format("{} draw {}", frameName, event["index"].value_or<std::int64_t>(-1));
            const auto directory = root / event["pair"].value_or(std::string());
            const auto table     = toml::parse_file((directory / "record.toml").string());
            const auto record    = toml::node_view<const toml::node>(table);
            UNSCOPED_INFO(name + " is the label pair");
            REQUIRE(record["pixel_word_sha256"].value_or(std::string()) == kLabelPixelWordDigest);
            LabelDrawInputs              in;
            std::array<std::uint32_t, 8> a{};
            for (std::size_t i = 0; i < a.size(); ++i)
                a[i] = address(event["arguments_r3_r10"][i]);
            in.arguments = { a[0], a[1], 0, a[2], 0, 0, 0, a[3], a[4], kLabelDrawCaller };
            shared(in, directory, record);
            in.state = std::move(state);
            if (const auto* memory = event["memory"].as_array())
                for (const auto& region : *memory)
                    if (const auto view = toml::node_view<const toml::node>(region); view["role"].value_or(std::string()) == "vertices")
                        in.vertices = read(root / view["file"].value_or(std::string()));
            UNSCOPED_INFO(name + " holds its vertices");
            REQUIRE(in.vertices.size() == std::size_t(a[2]) * a[4]);
            for (std::size_t i = 0; i < in.textureFetch.size(); ++i)
                in.textureFetch[i] = word(in.state, 0x480 + i * 4);
            NativeTextureMemoryRange range;
            if (GetNativeTextureMemoryRange(in.textureFetch, range, error))
            {
                in.textureAddress = range.address;
                in.textureBytes.assign(range.size, 0);
            }
            in.labelCandidate = true;
            LabelDrawRecipe recipe;
            UNSCOPED_INFO(name + " builds under its mask: " + error);
            REQUIRE(BuildLabelDrawRecipe(in, labelShaders, recipe, error));
            const auto  refMask = word(in.state, 0x2900);
            const auto& stencil = recipe.native.stencil;
            UNSCOPED_INFO(name + " tests stencil equal to its mask depth and keeps it");
            REQUIRE((stencil.enabled && stencil.reference == (refMask & 0xFF) && stencil.readMask == 0xFF && stencil.writeMask == 0xFF && stencil.front.compare == 3 &&
                     stencil.front.pass == 1 && stencil.front.fail == 1 && stencil.front.depthFail == 1));
            references.insert(refMask & 0xFF);
        }
        return references;
    };
    std::size_t harvests = 0;
    // The two-texture pair first draws in the late world harvest only.
    const auto              dualPixel       = kGfxPairs[4].pixelWords;
    bool                    dualBuilt       = false;
    unsigned                offscreenLabels = 0, offscreenPanels = 0;
    std::set<std::uint32_t> maskedReferences;
    for (std::size_t begin = 0; begin <= roots.size();)
    {
        const auto end  = std::min(roots.find(';', begin), roots.size());
        const auto root = std::filesystem::path(roots.substr(begin, end - begin));
        begin           = end + 1;
        if (root.empty())
            continue;
        ++harvests;
        const auto                 index = toml::parse_file((root / "index.toml").string());
        const auto*                pairs = index["pairs"].as_array();
        std::map<std::string, int> outcomes;
        std::vector<std::string>   built;
        UNSCOPED_INFO("harvest pairs");
        REQUIRE(pairs != nullptr);
        for (const auto& row : *pairs)
        {
            const auto view   = toml::node_view<const toml::node>(row);
            const auto caller = view["caller"].value_or(std::string());
            if (caller == "0x82304258")
            {
                const auto name    = view["directory"].value_or(std::string());
                const auto outcome = label(root / name, name);
                offscreenLabels += outcome == std::string_view("label built off-screen") ? 1U : 0U;
                ++outcomes[outcome];
                continue;
            }
            if (caller != "0x82303E90")
                continue;
            const auto                   name      = view["directory"].value_or(std::string());
            const auto                   directory = root / name;
            const auto                   table     = toml::parse_file((directory / "record.toml").string());
            const auto                   record    = toml::node_view<const toml::node>(table);
            const auto                   pixel     = record["pixel_word_sha256"].value_or(std::string());
            const auto*                  entry     = catalog.Find(pixel);
            GfxDrawInputs                in;
            std::array<std::uint32_t, 8> a{};
            for (std::size_t i = 0; i < a.size(); ++i)
                a[i] = address(record["arguments_r3_r10"][i]);
            if (!entry && pixel == kSolidPanelPixelWords)
            {
                ++outcomes["solid panel"];
                continue;
            }
            if (!entry)
            {
                UNSCOPED_INFO(name + " is a GFx form missing from the catalog");
                REQUIRE(std::ranges::none_of(kGfxPairs, [&](const GfxPairForm& form)
                                             {
                                                 return form.pixelWords == pixel;
                                             }));
                // One dword per declared element, as the catalog strides are.
                shared(in, directory, record);
                const auto stride = static_cast<std::uint32_t>(in.declaration.size() / 12 * 4);
                in.arguments      = { a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], stride, kGfxDrawCaller };
                GfxDrawRecipe recipe;
                const auto    expected = std::format("gfx_shader_unpinned ps={} vs={} stride={}", pixel, record["vertex_word_sha256"].value_or(std::string()), stride);
                UNSCOPED_INFO(name + " is refused by name: " + error);
                REQUIRE((!BuildGfxDrawRecipe(in, catalog, recipe, error) && error == expected));
                std::cout << std::format("{}: refused {}\n", name, error);
                ++outcomes["refused unpinned"];
                continue;
            }
            in.arguments = { a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], entry->stride, kGfxDrawCaller };
            shared(in, directory, record);
            // GFx declarations hold one element per vertex dword.
            UNSCOPED_INFO(name + " declaration matches the catalog stride");
            REQUIRE(in.declaration.size() / 12 * 4 == entry->stride);
            in.vertices.assign(std::size_t(in.arguments.vertexCount) * entry->stride, 0);
            in.indices.assign(std::size_t(in.arguments.indexCount) * 2, 0);
            const auto* textures = record["pixel_textures"].as_array();
            UNSCOPED_INFO(name + " samples the catalog pair's textures");
            REQUIRE(entry->textures == (textures ? textures->size() : 0));
            for (std::uint32_t slot = 0; slot < entry->textures; ++slot)
            {
                const auto texture = toml::node_view<const toml::node>((*textures)[slot]);
                auto&      fetch   = slot ? in.secondTextureFetch : in.textureFetch;
                UNSCOPED_INFO(name + " samples slots in order");
                REQUIRE(texture["slot"].value_or<std::int64_t>(-1) == slot);
                for (std::size_t i = 0; i < fetch.size(); ++i)
                {
                    fetch[i] = word(in.state, 0x480 + (slot * 6 + i) * 4);
                    UNSCOPED_INFO(std::format("{} fetch {} is the harvested texture", name, slot));
                    REQUIRE(fetch[i] == address(texture["words"][i]));
                }
            }
            if (entry->textures)
                sizeWorldTexture(in);
            if (entry->textures > 1)
            {
                NativeTextureMemoryRange range;
                UNSCOPED_INFO(name + " fetch 1 range: " + error);
                REQUIRE(GetNativeTextureMemoryRange(in.secondTextureFetch, range, error));
                in.secondTextureAddress = range.address;
                in.secondTextureBytes.assign(range.size, 0);
            }
            in.inputsComplete = in.gfxCandidate = true;
            GfxDrawRecipe recipe;
            const bool    ok        = BuildGfxDrawRecipe(in, catalog, recipe, error);
            const auto    registers = GuestDrawRegisters(in.state);
            // Census B5: key 14AEC15D draws the loading pair into the Hall of
            // Glory panels' 928x688 surface.
            if (registers[0x2000] == kGfxOffscreenForm.surfaceInfo && entry->shader.pixelGuestWordDigest == kGfxPixelWordDigest)
            {
                if (!entry->offscreen)
                {
                    UNSCOPED_INFO(name + " is refused without a 1X entry: " + error);
                    REQUIRE((!ok && error == "gfx_offscreen_unpinned"));
                    std::cout << std::format("{}: refused {}\n", name, error);
                    ++outcomes["loading pair off-screen unpinned"];
                    continue;
                }
                // The panels sample the Hall background's packed-mip DXT1
                // chain: 1024x1024, levels 0..9.
                UNSCOPED_INFO(name + " builds off-screen: " + error);
                REQUIRE(ok);
                UNSCOPED_INFO(name + " builds off-screen at 1X");
                REQUIRE((recipe.native.width == 928 && recipe.native.height == 688 && recipe.native.sampleCount == 1));
                UNSCOPED_INFO(name + " uploads the ten-level DXT1 chain");
                REQUIRE((recipe.texture.format == NativeDrawReplayTextureFormat::Bc1 && recipe.texture.width == 1024 && recipe.texture.height == 1024 &&
                         recipe.texture.mipLevels == 10));
                UNSCOPED_INFO(name + " samples with the fetch's -0.5 mip bias");
                REQUIRE(recipe.sampler.mipBias == -0.5F);
                std::cout << std::format("{}: built off-screen, {} levels, mip bias {}\n", name, recipe.texture.mipLevels, recipe.sampler.mipBias);
                ++outcomes["loading pair built off-screen"];
                continue;
            }
            if (registers[0x2000] != 0x0A010280)
            {
                UNSCOPED_INFO(name + " (loading pair in the boot MRT pass) is refused by its surface: " + error);
                REQUIRE((!ok && entry->shader.pixelGuestWordDigest == kGfxPixelWordDigest && error == std::format("gfx_fixed_state 0x2000: {:#x}", registers[0x2000])));
                ++outcomes["loading pair in the boot MRT pass"];
                continue;
            }
            UNSCOPED_INFO(name + " builds from its harvested state: " + error);
            REQUIRE(ok);
            UNSCOPED_INFO(name + " links and patches to the catalog VS");
            REQUIRE(GfxMicrocodeWordDigest(recipe.patchedVertexCode) == entry->shader.vertexGuestWordDigest);
            if (entry->textures > 1)
            {
                const auto second = toml::node_view<const toml::node>((*textures)[1]);
                UNSCOPED_INFO(name + " binds its second texture");
                REQUIRE((std::int64_t(recipe.secondTexture.width) == second["width"].value_or<std::int64_t>(0) &&
                         std::int64_t(recipe.secondTexture.height) == second["height"].value_or<std::int64_t>(0) &&
                         recipe.native.textures[1].width == recipe.secondTexture.width));
            }
            std::cout << std::format("{}: VS {:016X} stride {} indices {} blend {:#x} stencil {} write mask {:#x}\n",
                                     name,
                                     entry->shader.vertexShaderHash,
                                     entry->stride,
                                     recipe.native.indexCount,
                                     registers[0x2201],
                                     recipe.native.stencil.enabled,
                                     recipe.native.blend.writeMask);
            ++outcomes["built"];
            built.push_back(entry->shader.pixelGuestWordDigest);
        }
        for (const auto& [outcome, count] : outcomes)
            std::cout << root.string() << ": " << outcome << ": " << count << '\n';
        std::ranges::sort(built);
        // Schema 2 keys a record by alpha test, target form and primitive as
        // well, so one shader pair may have several.
        if (index["schema_version"].value_or<std::int64_t>(1) >= 2)
        {
            built.erase(std::ranges::unique(built).begin(), built.end());
            UNSCOPED_INFO("the harvest holds the solid panel pair");
            REQUIRE(outcomes["solid panel"] >= 1);
        }
        else
        {
            UNSCOPED_INFO("each harvested GFx pair appears once, with the solid panel pair");
            REQUIRE((std::ranges::unique(built).empty() && outcomes["solid panel"] == 1));
        }
        dualBuilt = dualBuilt || std::ranges::binary_search(built, dualPixel);
        offscreenPanels += outcomes["loading pair built off-screen"];
        // The 20261015 world harvest's frames draw masked labels at mask
        // depths 1 and 2.
        for (const auto frameName : { "007561C9EFB23973-0", "0C1FBAB22D0F4293-0" })
            if (std::filesystem::exists(root / "frames" / frameName))
                for (const auto reference : maskedLabels(root, frameName))
                    maskedReferences.insert(reference);
        for (const auto& shader : catalog.entries)
        {
            UNSCOPED_INFO(std::format("catalog pair {:016X}/{:016X} builds from the harvest", shader.shader.vertexShaderHash, shader.shader.pixelShaderHash));
            REQUIRE((std::ranges::binary_search(built, shader.shader.pixelGuestWordDigest) || shader.shader.pixelGuestWordDigest == kGfxPixelWordDigest ||
                     shader.shader.pixelGuestWordDigest == dualPixel));
        }
    }
    UNSCOPED_INFO("REREVVED_GFX_HARVEST names at least one harvest");
    REQUIRE(harvests > 0);
    UNSCOPED_INFO("the two-texture pair builds from a harvest");
    REQUIRE((dualBuilt || !catalog.Find(dualPixel)));
    // The world harvest holds the off-screen label key 735031E1.
    UNSCOPED_INFO("the off-screen label form builds from the world harvest");
    REQUIRE((!labelShaders.offscreen || environment("REREVVED_WORLD_HARVEST").empty() || offscreenLabels > 0));
    // The 20261015 world harvest holds the Hall panel key 14AEC15D.
    const auto* loading = catalog.Find(kGfxPixelWordDigest);
    UNSCOPED_INFO("the off-screen loading panel form builds from the world harvest");
    REQUIRE((!loading || !loading->offscreen || environment("REREVVED_WORLD_HARVEST3").empty() || offscreenPanels > 0));
    UNSCOPED_INFO("the world harvest's masked labels build at mask depths 1 and 2");
    REQUIRE((environment("REREVVED_WORLD_HARVEST3").empty() || maskedReferences == std::set<std::uint32_t>{ 1, 2 }));
    if (!maskedReferences.empty())
        std::cout << std::format("GFx harvests: masked labels build at {} mask depths\n", maskedReferences.size());
    std::cout << "GFx harvests: each catalog pair at 0x82303E90 builds from its first harvested state\n";
    return 0;
}

} // namespace

// A check returns 77 when its D3D12 part could not run.
static void skipIfUnavailable(int status, const char* message)
{
    if (status == 77)
    {
        SKIP(message);
    }
}

TEST_CASE("gfx_draw_adapter", "[ctest][gfx-draw-adapter][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    unit();
}

TEST_CASE("gfx_draw_adapter_capture", "[.][renderer-research][gfx-draw-adapter][native_gpu][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    // Only an unset capture skips; a set but unusable path fails.
    const auto capture = environment("REREVVED_GFX_ADAPTER_CAPTURE");
    if (capture.empty())
    {
        SKIP("Set REREVVED_GFX_ADAPTER_CAPTURE to the C10 scene capture directory to run this check");
    }
    skipIfUnavailable(c10(capture), "no D3D12 device for the loading frame");
}

TEST_CASE("gfx_draw_adapter_world", "[.][renderer-research][gfx-draw-adapter][native_gpu][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    const auto                     worldCapture = environment("REREVVED_GFX_WORLD_CAPTURE");
    const auto                     worldShaders = environment("REREVVED_GFX_WORLD_SHADERS");
    if (worldCapture.empty() || worldShaders.empty())
    {
        SKIP("Set REREVVED_GFX_WORLD_CAPTURE to the world-map scene capture and REREVVED_GFX_WORLD_SHADERS to the stride-4 GFx catalog to run this check");
    }
    skipIfUnavailable(world(worldCapture, worldShaders), "no D3D12 device for the menu stencil stack");
}

TEST_CASE("gfx_draw_adapter_harvest", "[.][renderer-research][gfx-draw-adapter][timeout-300]")
{
    rerevved::tests::ScopedLogging logging;
    // The frame-form world harvests, when named, are more roots. Their
    // two-texture pair needs a catalog that holds it.
    auto       roots   = environment("REREVVED_GFX_HARVEST");
    const auto world   = environment("REREVVED_WORLD_HARVEST");
    const auto world3  = environment("REREVVED_WORLD_HARVEST3");
    auto       shaders = environment("REREVVED_GFX_HARVEST_SHADERS");
    if (shaders.empty())
        shaders = environment("REREVVED_GFX_WORLD_SHADERS");
    if (roots.empty() || shaders.empty())
    {
        SKIP("Set REREVVED_GFX_HARVEST to the gameplay shader harvests (separated by ;) and REREVVED_GFX_HARVEST_SHADERS (or REREVVED_GFX_WORLD_SHADERS) to the GFx catalog to run this check");
    }
    for (const auto& extra : { world, world3 })
        if (!extra.empty())
            roots += ";" + extra;
    harvest(roots, shaders);
}
