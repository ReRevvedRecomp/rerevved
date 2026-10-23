#include "gpu/d3d12/native_texture_upload.h"
#include "gpu/diagnostics/shader_harvest.h"
#include "gpu/guest_d3d/indexed_buffer_draw_inputs.h"
#include "gpu/guest_d3d/native_draw_hook.h"
#include "logging_scope.hpp"
#include "manual_cases.hpp"

#include <rex/hash.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <toml++/toml.hpp>

namespace
{

using namespace rerevved::gpu::diagnostics;
using rerevved::gpu::guest_d3d::NativeDrawHook;
using rerevved::gpu::guest_d3d::NativeDrawHookAddress;

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

// Guest memory as a few regions; a read outside them throws as the guest
// reader does for uncommitted pages.
struct FakeMemory
{
    struct Region
    {
        std::uint64_t             base     = 0;
        bool                      physical = false;
        std::vector<std::uint8_t> bytes;
    };

    std::vector<Region> regions;

    Region& region(std::uint64_t address, bool physical)
    {
        for (auto& r : regions)
            if (r.physical == physical && address >= r.base && address < r.base + r.bytes.size())
                return r;
        throw std::runtime_error("unmapped");
    }

    void put(std::uint64_t address, std::uint32_t value, bool physical = false)
    {
        auto& r = region(address, physical);
        for (unsigned i = 0; i < 4; ++i)
            r.bytes.at(address - r.base + i) = static_cast<std::uint8_t>(value >> (24 - i * 8));
    }

    void putByte(std::uint64_t address, std::uint8_t value)
    {
        auto& r                      = region(address, false);
        r.bytes.at(address - r.base) = value;
    }

    std::vector<std::uint8_t> read(std::uint64_t address, std::uint64_t size, bool physical)
    {
        auto& r = region(address, physical);
        UNSCOPED_INFO("unmapped");
        REQUIRE(address - r.base + size <= r.bytes.size());
        return { r.bytes.begin() + std::ptrdiff_t(address - r.base), r.bytes.begin() + std::ptrdiff_t(address - r.base + size) };
    }
};

constexpr std::uint32_t kDevice = 0x40000000, kPixel = 0x40010000, kVertex = 0x40020000, kDeclaration = 0x40030000;
constexpr std::uint32_t kPixelCode = 0x100000, kVertexCode = 0x200000, kLeaderCaller = 0x8250CB98;
constexpr std::uint32_t kStack = 0x70000000, kData = 0x50000000;

// Harvest fixture supplies three-word VS/PS NOPs, c508-c511 literals, no
// fetches and an empty declaration.
FakeMemory memoryFor(std::uint32_t vertexWord, std::uint32_t pixelWord)
{
    FakeMemory m;
    m.regions = { { kDevice, false, std::vector<std::uint8_t>(0x3500) }, { kPixel, false, std::vector<std::uint8_t>(0x400) }, { kVertex, false, std::vector<std::uint8_t>(0x1000) }, { kDeclaration, false, std::vector<std::uint8_t>(64) }, { 0x82091300, false, std::vector<std::uint8_t>(0x200) }, { kPixelCode, true, std::vector<std::uint8_t>(0x1000) }, { kVertexCode, true, std::vector<std::uint8_t>(0x1000) }, { kStack, false, std::vector<std::uint8_t>(0x200) }, { kData, false, std::vector<std::uint8_t>(0x1000) } };
    m.put(kDevice + 0x3194, kPixel);
    m.put(kDevice + 0x3198, kVertex);
    m.put(kDevice + 0x2E2C, kDeclaration);
    m.put(kDevice + 0x30F0, 13U << 24); // stream 0 stride, 13 words
    // PS: code base at +24, metadata at +40 + 0x100, literal table at +40 + 0x200.
    m.put(kPixel + 24, kPixelCode);
    m.put(kPixel + 40 + 24, 0x100);
    m.put(kPixel + 40 + 20, 0x200);
    m.put(kPixel + 40 + 0x100 + 4, 12);
    m.put(kPixel + 40 + 0x200 + 16, 8);
    m.put(kPixel + 40 + 0x200 + 20, 508U << 16 | 4);
    m.put(kPixel + 40 + 0x200 + 24, 0x800);
    for (std::uint32_t i = 0; i < 4; ++i)
        m.put(kPixelCode + 0x800 + i * 4, 0x3F800000 + i, true);
    m.put(kPixelCode, pixelWord, true);
    // VS: code base at +32, variant 0 metadata at +872 + 0x400, variant 1 at
    // +872 + 0x500 with code at +0x100; neither has fetch records.
    m.put(kVertex + 32, kVertexCode);
    m.put(kVertex + 872 + 24, 0x400);
    m.put(kVertex + 872 + 0x400 + 4, 12);
    m.put(kVertex + 872 + 32, 0x500);
    m.put(kVertex + 872 + 0x500, 0x100);
    m.put(kVertex + 872 + 0x500 + 4, 12);
    m.put(kVertexCode, vertexWord, true);
    return m;
}

ShaderHarvestRead readerFor(FakeMemory& memory)
{
    return [&memory](std::uint64_t address, std::uint64_t size, bool physical)
    {
        return memory.read(address, size, physical);
    };
}

ShaderHarvestCall callFrom(std::uint32_t caller, std::uint32_t primitive = 4)
{
    return { 0x826A39F8, caller, { kDevice, primitive, 0, 0, 36, 0, 0, 0 }, kStack };
}

std::uint64_t codeHash(std::uint32_t first)
{
    const std::uint8_t bytes[12]{ static_cast<std::uint8_t>(first >> 24), static_cast<std::uint8_t>(first >> 16), static_cast<std::uint8_t>(first >> 8), static_cast<std::uint8_t>(first) };
    return XXH3_64bits(bytes, sizeof(bytes));
}

std::filesystem::path freshRoot(const char* name)
{
    const auto root = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(root);
    return root;
}

void callerParsing()
{
    std::uint32_t caller = 0;
    UNSCOPED_INFO("an exact 0x caller parses");
    REQUIRE((ParseShaderHarvestCaller("0x8250CB98", caller) && caller == kLeaderCaller));
    UNSCOPED_INFO("case does not matter");
    REQUIRE((ParseShaderHarvestCaller("0X8250cb98", caller) && caller == kLeaderCaller));
    for (const auto* text : { "8250CB98", "0x8250CB9", "0x8250CB980", "0x8250CB98 ", "0xG250CB98", "" })
    {
        UNSCOPED_INFO(std::format("'{}' is refused", text));
        REQUIRE(!ParseShaderHarvestCaller(text, caller));
    }
}

void harvest()
{
    const auto    root = freshRoot("rerevved-shader-harvest-test");
    ShaderHarvest harvest;
    std::string   error;
    UNSCOPED_INFO("a fresh directory starts: " + error);
    REQUIRE(harvest.Start(root, { kLeaderCaller }, error));
    ShaderHarvest second;
    UNSCOPED_INFO("an existing directory is refused");
    REQUIRE((!second.Start(root, {}, error) && !error.empty()));

    auto body = memoryFor(0x11111111, 0x22222222);
    auto read = readerFor(body);
    // VS bool word 1 (register 0x4901) and PS loop constant 0 (register 0x4918).
    body.put(kDevice + 0x2784, 0x80000001);
    body.put(kDevice + 0x27E0, 0x00010003);
    // Three draws of one key, a foreign caller, then a second pixel shader.
    for (int i = 0; i < 3; ++i)
    {
        HarvestOperation(harvest, callFrom(kLeaderCaller), read);
        harvest.Boundary(read);
    }
    HarvestOperation(harvest, callFrom(0x8250C768), read);
    auto glow = memoryFor(0x11111111, 0x33333333);
    HarvestOperation(harvest, callFrom(kLeaderCaller), readerFor(glow));
    // The same pixel shader behind a re-patched vertex shader is a third key.
    auto patched = memoryFor(0x44444444, 0x22222222);
    HarvestOperation(harvest, callFrom(kLeaderCaller), readerFor(patched));
    HarvestOperation(harvest, callFrom(kLeaderCaller), read);

    auto pairs = harvest.Pairs();
    UNSCOPED_INFO(std::format("three keys ({} found)", pairs.size()));
    REQUIRE(pairs.size() == 3);
    const auto& first = pairs[0];
    UNSCOPED_INFO("the first key counts its four draws from frame 0 under its caller, hashes and key");
    REQUIRE((first.caller == kLeaderCaller && first.entry == 0x826A39F8 && first.firstFrame == 0 && first.draws == 4 && first.vertexHash == codeHash(0x11111111) &&
             first.pixelHash == codeHash(0x22222222) && first.primitive == 4 && !first.vertexOnly &&
             first.directory == std::format("8250CB98-{:016X}-{:016X}-{:08X}", first.vertexHash, first.pixelHash, static_cast<std::uint32_t>(first.keyHash))));
    UNSCOPED_INFO("a new pixel or vertex shader starts a key at its frame");
    REQUIRE((pairs[1].firstFrame == 3 && pairs[1].draws == 1 && pairs[1].pixelHash == codeHash(0x33333333) && pairs[2].vertexHash == codeHash(0x44444444)));

    // The representative record of the first key.
    const auto directory = root / first.directory;
    UNSCOPED_INFO("the key keeps both stages' microcode and the device shadow");
    REQUIRE((readFile(directory / "ps_code.be.bin") == body.read(kPixelCode, 12, true) && readFile(directory / "vs_code.be.bin") == body.read(kVertexCode, 12, true) &&
             readFile(directory / "state.be.bin") == body.read(kDevice, 0x3500, false)));
    const auto record = toml::parse_file((directory / "record.toml").string());
    UNSCOPED_INFO("record.toml names the caller, entry, stride, frame, digests, arguments and key");
    REQUIRE((record["caller"].value_or(std::string()) == "0x8250CB98" && record["entry"].value_or(std::string()) == "0x826A39F8" &&
             record["stream0_stride_bytes"].value_or<std::int64_t>(0) == 52 && record["first_frame"].value_or<std::int64_t>(-1) == 0 &&
             record["vertex_word_sha256"].value_or(std::string()) == rerevved::gpu::guest_d3d::IndexedBufferMicrocodeWordDigest(body.read(kVertexCode, 12, true)) &&
             record["pixel_hash"].value_or(std::string()) == std::format("{:016X}", first.pixelHash) &&
             record["arguments_r3_r10"][0].value_or(std::string()) == "0x40000000" && record["stack_r1"].value_or(std::string()) == "0x70000000" &&
             record["schema_version"].value_or<std::int64_t>(0) == 3 && record["primitive"].value_or<std::int64_t>(0) == 4 &&
             record["key_hash"].value_or(std::string()) == std::format("{:016X}", first.keyHash)));
    const auto* literals = record["pixel_literals"].as_array();
    UNSCOPED_INFO("the PS literal record keeps its values");
    REQUIRE((literals && literals->size() == 1 && record["pixel_literals"][0]["first_float4"].value_or<std::int64_t>(0) == 508 &&
             record["pixel_literals"][0]["values"][3].value_or<std::int64_t>(0) == 0x3F800003 && record["vertex_literals"].as_array()->empty()));
    UNSCOPED_INFO("the record names registers 0x4900..0x4927 from the shadow at G+0x2780");
    REQUIRE((record["bool_constants"].as_array() && record["bool_constants"].as_array()->size() == 8 && record["loop_constants"].as_array() &&
             record["loop_constants"].as_array()->size() == 32 && record["bool_constants"][1].value_or(std::string()) == "0x80000001" &&
             record["loop_constants"][16].value_or(std::string()) == "0x00010003" && record["loop_constants"][0].value_or(std::string()) == "0x00000000"));

    // The key's other fields: an enabled alpha test (RB_COLORCONTROL bit 3 at
    // G+0x293C) or a changed alpha function under it, the primitive and the
    // RT0 format each start a key; an alpha function without the enable does not.
    body.put(kDevice + 0x293C, 0x4);
    HarvestOperation(harvest, callFrom(kLeaderCaller), read);
    UNSCOPED_INFO("a disabled alpha test keeps the key");
    REQUIRE((harvest.Pairs().size() == 3 && harvest.Pairs()[0].draws == 5));
    body.put(kDevice + 0x293C, 0xC);
    HarvestOperation(harvest, callFrom(kLeaderCaller), read);
    body.put(kDevice + 0x293C, 0xD);
    HarvestOperation(harvest, callFrom(kLeaderCaller), read);
    body.put(kDevice + 0x293C, 0);
    HarvestOperation(harvest, callFrom(kLeaderCaller, 5), read);
    body.put(kDevice + 0x2884, 2U << 16);
    HarvestOperation(harvest, callFrom(kLeaderCaller), read);
    body.put(kDevice + 0x2884, 0);
    pairs = harvest.Pairs();
    UNSCOPED_INFO("alpha test, alpha function, primitive and RT0 format are part of the key");
    REQUIRE((pairs.size() == 7 && pairs[3].alphaTest == 0xC && pairs[4].alphaTest == 0xD && pairs[5].primitive == 5 && pairs[6].colorFormat == 2 &&
             pairs[6].targetForm.find("color 2") != std::string::npos));

    // A draw without a pixel shader is a vertex-only record: no PS file, pixel
    // hash 0, and VS variant 1 when the VS header word +872 has bit 0x20.
    body.put(kDevice + 0x3194, 0);
    HarvestOperation(harvest, callFrom(kLeaderCaller), read);
    body.put(kVertex + 872, 0x20);
    HarvestOperation(harvest, callFrom(kLeaderCaller), read);
    body.put(kVertex + 872, 0);
    pairs = harvest.Pairs();
    UNSCOPED_INFO("vertex-only draws are recorded with the variant the guest selects");
    REQUIRE((pairs.size() == 9 && pairs[7].vertexOnly && pairs[7].pixelHash == 0 && pairs[7].variant == 0 && pairs[7].vertexHash == codeHash(0x11111111) &&
             pairs[8].vertexOnly && pairs[8].variant == 1 && pairs[8].vertexHash == codeHash(0) && pairs[8].pixelWordDigest.empty()));
    UNSCOPED_INFO("a vertex-only record keeps the selected variant's microcode alone");
    REQUIRE((!std::filesystem::exists(root / pairs[7].directory / "ps_code.be.bin") && readFile(root / pairs[7].directory / "vs_code.be.bin") == body.read(kVertexCode, 12, true) &&
             readFile(root / pairs[8].directory / "vs_code.be.bin") == body.read(kVertexCode + 0x100, 12, true)));
    const auto vertexOnly = toml::parse_file((root / pairs[8].directory / "record.toml").string());
    UNSCOPED_INFO("the vertex-only record names its variant");
    REQUIRE((vertexOnly["vertex_only"].value_or(false) && vertexOnly["vertex_variant"].value_or<std::int64_t>(0) == 1 &&
             vertexOnly["pixel_hash"].value_or(std::string()) == "0000000000000000"));

    // A draw with an unreadable shader is counted, not recorded; so is one
    // without any vertex shader.
    body.put(kDevice + 0x3194, 0x60000000);
    HarvestOperation(harvest, callFrom(kLeaderCaller), read);
    body.put(kDevice + 0x3194, kPixel);
    body.put(kDevice + 0x3198, 0);
    HarvestOperation(harvest, callFrom(kLeaderCaller), read);
    body.put(kDevice + 0x3198, kVertex);
    UNSCOPED_INFO("skipped draws add no key");
    REQUIRE(harvest.Pairs().size() == 9);

    // The index lists the keys in discovery order; a count-only change waits for 60 frames.
    UNSCOPED_INFO("the index writes: " + error);
    REQUIRE(harvest.WriteIndex(error));
    HarvestOperation(harvest, callFrom(kLeaderCaller), readerFor(glow));
    for (int i = 0; i < 59; ++i)
        harvest.Boundary(read);
    auto index = toml::parse_file((root / "index.toml").string());
    UNSCOPED_INFO("a count-only change is not written before 60 frames");
    REQUIRE(index["pairs"][1]["draws"].value_or<std::int64_t>(0) == 1);
    harvest.Boundary(read);
    index = toml::parse_file((root / "index.toml").string());
    UNSCOPED_INFO("the index counts frames, draws, skipped draws and draws by entry");
    REQUIRE((index["schema_version"].value_or<std::int64_t>(0) == 3 && index["caller_filter"].value_or(std::string()) == "0x8250CB98" &&
             index["frames"].value_or<std::int64_t>(0) == 63 && index["draws"].value_or<std::int64_t>(0) == 14 &&
             index["draws_without_shaders"].value_or<std::int64_t>(0) == 1 && index["unreadable_draws"].value_or<std::int64_t>(0) == 1 &&
             index["write_failures"].value_or<std::int64_t>(-1) == 0 && !index["first_error"].value_or(std::string()).empty() &&
             index["draws_by_entry"]["0x826A39F8"].value_or<std::int64_t>(0) == 16));
    const auto* listed = index["pairs"].as_array();
    UNSCOPED_INFO("the index lists nine keys");
    REQUIRE((listed && listed->size() == 9));
    for (std::size_t i = 0; i < 9; ++i)
    {
        const auto row = index["pairs"][i];
        UNSCOPED_INFO(std::format("index row {} matches key {}", i, i));
        REQUIRE((row["directory"].value_or(std::string()) == pairs[i].directory && row["caller"].value_or(std::string()) == "0x8250CB98" &&
                 row["vertex_hash"].value_or(std::string()) == std::format("{:016X}", pairs[i].vertexHash) &&
                 row["pixel_word_sha256"].value_or(std::string()) == pairs[i].pixelWordDigest &&
                 row["key_hash"].value_or(std::string()) == std::format("{:016X}", pairs[i].keyHash) &&
                 row["first_frame"].value_or<std::int64_t>(-1) == static_cast<std::int64_t>(pairs[i].firstFrame)));
    }
    UNSCOPED_INFO("the 60th frame writes the new count");
    REQUIRE(index["pairs"][1]["draws"].value_or<std::int64_t>(0) == 2);
    UNSCOPED_INFO("frame forms stay off by default");
    REQUIRE(!std::filesystem::exists(root / "frames"));

    // After Stop nothing is written.
    harvest.Stop();
    auto late = memoryFor(0x55555555, 0x66666666);
    HarvestOperation(harvest, callFrom(kLeaderCaller), readerFor(late));
    UNSCOPED_INFO("a draw after Stop writes nothing");
    REQUIRE(toml::parse_file((root / "index.toml").string())["pairs"].as_array()->size() == 9);

    // Without a filter every caller is harvested.
    const auto    open = freshRoot("rerevved-shader-harvest-test-open");
    ShaderHarvest all;
    UNSCOPED_INFO("an unfiltered harvest starts: " + error);
    REQUIRE(all.Start(open, {}, error));
    auto again = memoryFor(0x11111111, 0x22222222);
    HarvestOperation(all, callFrom(kLeaderCaller), readerFor(again));
    HarvestOperation(all, callFrom(0x8250C768), readerFor(again));
    UNSCOPED_INFO("the caller is part of the key");
    REQUIRE((all.Pairs().size() == 2 && all.Pairs()[1].caller == 0x8250C768));
    all.Stop();
    std::filesystem::remove_all(root);
    std::filesystem::remove_all(open);
}

// All six draw hooks report; the outer UP wrappers' own child calls do not.
void sixHooks()
{
    const auto    root = freshRoot("rerevved-shader-harvest-test-hooks");
    ShaderHarvest harvest;
    std::string   error;
    UNSCOPED_INFO("the hook harvest starts: " + error);
    REQUIRE(harvest.Start(root, {}, error));
    auto       memory = memoryFor(0x11111111, 0x22222222);
    auto       read   = readerFor(memory);
    const auto draw   = [&](NativeDrawHook hook, std::uint32_t caller)
    {
        HarvestOperation(harvest, { NativeDrawHookAddress(hook), caller, { kDevice, 4, 0, 4, 6, 0, 0, 0 }, kStack }, read);
    };
    draw(NativeDrawHook::IndexedBuffer, 0x8250C768);
    draw(NativeDrawHook::IndexedUp, 0x82303E90);
    draw(NativeDrawHook::NonindexedUp, 0x82304258);
    draw(NativeDrawHook::IndexedUpLowerer, rerevved::gpu::guest_d3d::kIndexedUpLowererReturn);
    draw(NativeDrawHook::NonindexedSink, rerevved::gpu::guest_d3d::kNonindexedSinkReturn);
    draw(NativeDrawHook::IndexedUpLowerer, 0x82510BFC);
    draw(NativeDrawHook::NonindexedSink, 0x82300000);
    draw(NativeDrawHook::AutomaticIndex, 0x82400000);
    const auto pairs = harvest.Pairs();
    UNSCOPED_INFO(std::format("six keys, one per reporting hook ({} found)", pairs.size()));
    REQUIRE(pairs.size() == 6);
    for (std::size_t i = 0; i < pairs.size(); ++i)
    {
        UNSCOPED_INFO(std::format("key {} names hook {:#x}", i, pairs[i].entry));
        REQUIRE(pairs[i].entry == rerevved::gpu::guest_d3d::kNativeDrawHookAddresses[i]);
    }
    UNSCOPED_INFO("a direct IndexedUpLowerer call keeps its caller");
    REQUIRE(pairs[3].caller == 0x82510BFC);
    UNSCOPED_INFO("the hook index writes: " + error);
    REQUIRE(harvest.WriteIndex(error));
    const auto index = toml::parse_file((root / "index.toml").string());
    for (const auto entry : rerevved::gpu::guest_d3d::kNativeDrawHookAddresses)
    {
        UNSCOPED_INFO(std::format("hook {:#x} counted once", entry));
        REQUIRE(index["draws_by_entry"][std::format("0x{:08X}", entry)].value_or<std::int64_t>(0) == 1);
    }
    harvest.Stop();
    std::filesystem::remove_all(root);
}

// A second PS record that rewrites c508.xy keeps its own values in record.toml.
void overlappingLiterals()
{
    const auto    root = freshRoot("rerevved-shader-harvest-test-overlap");
    ShaderHarvest harvest;
    std::string   error;
    UNSCOPED_INFO("the overlap harvest starts: " + error);
    REQUIRE(harvest.Start(root, { kLeaderCaller }, error));
    auto memory = memoryFor(0x11111111, 0x22222222);
    memory.put(kPixel + 40 + 0x200 + 16, 16);
    memory.put(kPixel + 40 + 0x200 + 28, 508U << 16 | 2);
    memory.put(kPixel + 40 + 0x200 + 32, 0x900);
    for (std::uint32_t i = 0; i < 2; ++i)
        memory.put(kPixelCode + 0x900 + i * 4, 0x40000000 + i, true);
    HarvestOperation(harvest, callFrom(kLeaderCaller), readerFor(memory));
    UNSCOPED_INFO("the overlap draw is harvested");
    REQUIRE(harvest.Pairs().size() == 1);
    const auto record   = toml::parse_file((root / harvest.Pairs()[0].directory / "record.toml").string());
    const auto literals = record["pixel_literals"];
    UNSCOPED_INFO("each overlapping record keeps its own values");
    REQUIRE((literals.as_array() && literals.as_array()->size() == 2 && literals[0]["values"][0].value_or<std::int64_t>(0) == 0x3F800000 &&
             literals[0]["values"][3].value_or<std::int64_t>(0) == 0x3F800003 && literals[1]["values"][0].value_or<std::int64_t>(0) == 0x40000000 &&
             literals[1]["values"][1].value_or<std::int64_t>(0) == 0x40000001));
    harvest.Stop();
    std::filesystem::remove_all(root);
}

// Frame forms over FakeGpu-ordered operations.
struct FrameFixture
{
    FakeMemory        memory = memoryFor(0x11111111, 0x22222222);
    ShaderHarvestRead read   = readerFor(memory);
    ShaderHarvest     harvest;

    void clear(std::uint32_t flags = 1)
    {
        // r4 flags, r5 index, r6..r9 rectangle, r10 color; f1 depth.
        HarvestOperation(harvest, { kHarvestClearEntry, 0, { kDevice, flags, 0, 0, 0, 1280, 720, kData + 0x100 }, kStack, 0, 1.0 }, read);
    }

    void draw(std::uint32_t caller = kLeaderCaller)
    {
        HarvestOperation(harvest, callFrom(caller), read);
    }

    void resolve(std::uint32_t copyControl)
    {
        memory.put(kDevice + 0x2A18, copyControl);
        // D3DDevice_Resolve r4 source, r5 rectangle, r6 destination texture.
        HarvestOperation(harvest, { kHarvestResolveEntry, 0x8250AE24, { kDevice, 0, kData + 0x200, kData + 0x300, 0, 0, 0, 0 }, kStack }, read);
        HarvestOperation(harvest, { kHarvestResolveCopyEntry, 0, { kDevice } }, read);
    }

    void swap()
    {
        harvest.Boundary(read);
    }
};

void frameForms()
{
    const auto   root = freshRoot("rerevved-shader-harvest-test-frames");
    FrameFixture f;
    std::string  error;
    // One byte of budget: the first instance of each class is written, later
    // unmarked ones are not.
    UNSCOPED_INFO("the frame harvest starts: " + error);
    REQUIRE(f.harvest.Start(root, { std::nullopt, 2, 1 }, error));
    f.memory.put(kData + 0x100, 0x3F800000);
    f.memory.put(kDevice + 0x27E0, 0x00010003);
    f.memory.put(kData + 0x208, 1280);
    f.memory.put(kData + 0x20C, 720);
    f.memory.put(kData + 0x300 + 0x1C + 8, 1279U | 719U << 13);

    // Frame 0: the menu form, clear then draws then a resolve.
    f.clear();
    f.draw();
    f.draw();
    f.draw();
    f.resolve(0x100040);
    f.swap();
    // Frame 1: the same form with five draws of the key.
    f.clear();
    for (int i = 0; i < 5; ++i)
        f.draw();
    f.resolve(0x100040);
    f.swap();
    // Frame 2: presented work before any clear, the game-start form.
    f.draw();
    f.clear();
    f.draw();
    f.resolve(0x100040);
    f.swap();
    // Frame 3: a new unmarked form past the budget is counted, not written.
    f.clear();
    f.draw();
    f.resolve(0x100340);
    f.swap();

    const auto forms = f.harvest.FrameForms();
    UNSCOPED_INFO(std::format("three signatures ({} found)", forms.size()));
    REQUIRE(forms.size() == 3);
    UNSCOPED_INFO("a repeated draw count keeps the signature; the budget stops its second instance");
    REQUIRE((forms[0].frames == 2 && forms[0].instances.size() == 1 && forms[0].firstFrame == 0));
    UNSCOPED_INFO("the draw-before-clear frame is written from its own budget");
    REQUIRE((forms[1].markedFrames == 1 && forms[1].markedInstances.size() == 1 && forms[1].instances.empty()));
    UNSCOPED_INFO("a new unmarked signature past the budget is not written");
    REQUIRE((forms[2].frames == 1 && forms[2].instances.empty()));

    const auto  menu   = toml::parse_file((root / "frames" / forms[0].instances[0] / "frame.toml").string());
    const auto* events = menu["events"].as_array();
    UNSCOPED_INFO("the menu frame keeps its five operations in order");
    REQUIRE((events && events->size() == 5));
    UNSCOPED_INFO("clear, draws and resolve are recorded in order");
    REQUIRE((menu["events"][0]["kind"].value_or(std::string()) == "clear" && menu["events"][1]["kind"].value_or(std::string()) == "draw" &&
             menu["events"][4]["kind"].value_or(std::string()) == "resolve" && !menu["draw_before_clear"].value_or(true) &&
             menu["events"][0]["depth_f1"].value_or(0.0) == 1.0));
    UNSCOPED_INFO("a draw names its key record");
    REQUIRE(menu["events"][1]["pair"].value_or(std::string()) == f.harvest.Pairs()[0].directory);
    UNSCOPED_INFO("a draw names its loop and bool constants; a clear does not");
    REQUIRE((menu["events"][1]["loop_constants"][16].value_or(std::string()) == "0x00010003" && menu["events"][1]["bool_constants"].as_array() &&
             menu["events"][1]["bool_constants"].as_array()->size() == 8 && !menu["events"][0]["loop_constants"]));
    UNSCOPED_INFO("the resolve keeps D3DDevice_Resolve's caller, arguments, copy control, rectangle and texture");
    REQUIRE((menu["events"][4]["caller"].value_or(std::string()) == "0x8250AE24" && menu["events"][4]["arguments_r3_r10"][2].value_or(std::string()) == "0x50000200" &&
             menu["events"][4]["token"].value_or(std::string()).find("copy 100040") != std::string::npos &&
             menu["events"][4]["token"].value_or(std::string()).find("rect 0,0,1280,720") != std::string::npos &&
             menu["events"][4]["token"].value_or(std::string()).find("texture 1280x720") != std::string::npos));
    // The shadow as frame 0's draws saw it, before any copy control was set.
    auto shadow = f.memory.read(kDevice, 0x3500, false);
    std::fill_n(shadow.begin() + 0x2A18, 4, 0);
    const auto state = menu["events"][1]["state"].value_or(std::string());
    UNSCOPED_INFO("each operation keeps the device shadow");
    REQUIRE((!state.empty() && readFile(root / state) == shadow));
    const auto* tokens = menu["canonical"].as_array();
    UNSCOPED_INFO("the signature is clear, the draw set, resolve and swap");
    REQUIRE((tokens && tokens->size() == 4 && menu["canonical"][0].value_or(std::string()).starts_with("C ") &&
             menu["canonical"][1].value_or(std::string()).starts_with("{D ") && menu["canonical"][3].value_or(std::string()) == "S swap"));

    const auto start = toml::parse_file((root / "frames" / forms[1].markedInstances[0] / "frame.toml").string());
    UNSCOPED_INFO("the game-start frame begins with a draw set");
    REQUIRE((start["draw_before_clear"].value_or(false) && start["canonical"][0].value_or(std::string()).starts_with("{D ") &&
             forms[1].markedInstances[0].find("marked") != std::string::npos));

    f.swap();
    UNSCOPED_INFO("the frame index writes: " + error);
    REQUIRE(f.harvest.WriteIndex(error));
    const auto index = toml::parse_file((root / "frames" / "index.toml").string());
    UNSCOPED_INFO("frames/index.toml lists every signature, the empty frame included");
    REQUIRE((index["signatures"].as_array() && index["signatures"].as_array()->size() == 4 && index["instances_per_signature"].value_or<std::int64_t>(0) == 2 &&
             index["signatures"][1]["draw_before_clear_instances"][0].value_or(std::string()) == forms[1].markedInstances[0]));
    UNSCOPED_INFO("every signature's canonical form is logged");
    REQUIRE(std::filesystem::file_size(root / "frames" / "signatures.log") > 0);
    f.harvest.Stop();
    std::filesystem::remove_all(root);
}

// Capture UP bytes now, lowerer destinations next, and tiling tokens.
void framePayloads()
{
    const auto   root = freshRoot("rerevved-shader-harvest-test-payloads");
    FrameFixture f;
    std::string  error;
    UNSCOPED_INFO("the payload harvest starts: " + error);
    REQUIRE(f.harvest.Start(root, { std::nullopt, 1, 1ULL << 30 }, error));
    auto& m = f.memory;
    m.put(kData + 0x100, 0x3F800000);
    // BeginTiling leaves two tiles active.
    m.putByte(kDevice + 0x2ABC, 0x20);
    m.put(kDevice + 0x31CC, 2);
    HarvestOperation(f.harvest, { kHarvestBeginTilingEntry, 0x8250F9F0, { kDevice }, kStack }, f.read);
    f.clear();
    // IndexedUp: r5 minimum vertex 0, r6 two vertices, r7 three indices at
    // r8 (16-bit), r10 vertices, stride 8 at r1+84.
    m.put(kStack + 84, 8);
    for (std::uint32_t i = 0; i < 4; ++i)
        m.put(kData + 0x400 + 4 * i, 0xA0000000 + i);
    m.put(kData + 0x500, 0x00000001);
    m.put(kData + 0x504, 0x00020000);
    HarvestOperation(f.harvest, { NativeDrawHookAddress(NativeDrawHook::IndexedUp), 0x82303E90, { kDevice, 4, 0, 2, 3, kData + 0x500, 0, kData + 0x400 }, kStack }, f.read);
    // A direct IndexedUpLowerer: r1+84 points at the vertex pointer, r10 at
    // the index pointer; the caller fills both after it returns.
    m.put(kStack + 84, kStack + 96);
    m.put(kStack + 96, kData + 0x600);
    m.put(kStack + 100, kData + 0x700);
    HarvestOperation(f.harvest, { NativeDrawHookAddress(NativeDrawHook::IndexedUpLowerer), 0x82510BFC, { kDevice, 4, 0, 1, 2, 0, 4, kStack + 100 }, kStack, 0 }, f.read);
    m.put(kData + 0x600, 0xCAFEF00D);
    m.put(kData + 0x700, 0x00000001);
    // Tiling ends without an operation of its own.
    m.putByte(kDevice + 0x2ABC, 0);
    f.resolve(0x100040);
    f.swap();

    const auto forms = f.harvest.FrameForms();
    UNSCOPED_INFO("the payload frame is written");
    REQUIRE((forms.size() == 1 && forms[0].instances.size() == 1));
    const auto frame  = toml::parse_file((root / "frames" / forms[0].instances[0] / "frame.toml").string());
    const auto memory = [&](std::size_t event, const std::string& role) -> toml::node_view<const toml::node>
    {
        const auto* rows = frame["events"][event]["memory"].as_array();
        UNSCOPED_INFO(std::format("event {} memory", event));
        REQUIRE(rows != nullptr);
        for (std::size_t i = 0; i < rows->size(); ++i)
            if (frame["events"][event]["memory"][i]["role"].value_or(std::string()) == role)
                return frame["events"][event]["memory"][i];
        throw std::runtime_error(std::format("event {} has no {} memory", event, role));
    };
    UNSCOPED_INFO("BeginTiling is an operation with its tile count");
    REQUIRE((frame["events"][0]["kind"].value_or(std::string()) == "begin_tiling" && frame["events"][0]["token"].value_or(std::string()).find("tiling 2") != std::string::npos));
    UNSCOPED_INFO("the clear keeps its color");
    REQUIRE(memory(1, "color")["bytes"].value_or(std::string()) == "3f800000000000000000000000000000");
    UNSCOPED_INFO("indexed-UP vertices and indices are read at the operation");
    REQUIRE((memory(2, "vertices")["bytes"].value_or(std::string()) == "a0000000a0000001a0000002a0000003" && memory(2, "vertices")["read"].value_or(std::string()) == "operation" &&
             memory(2, "indices")["bytes"].value_or(std::string()) == "0000000100020000"));
    UNSCOPED_INFO("a direct IndexedUpLowerer's destinations are read at the next operation");
    REQUIRE((memory(3, "vertices")["bytes"].value_or(std::string()) == "cafef00d" && memory(3, "vertices")["read"].value_or(std::string()) == "next_operation" &&
             memory(3, "indices")["bytes"].value_or(std::string()) == "00000001"));
    bool ended = false;
    for (std::size_t i = 0; i < frame["canonical"].as_array()->size(); ++i)
        ended |= frame["canonical"][i].value_or(std::string()) == "E tiling ended";
    UNSCOPED_INFO("BeginTiling opens the signature and the tiling end is derived");
    REQUIRE((ended && frame["canonical"][0].value_or(std::string()).starts_with("B tiling 2")));
    f.harvest.Stop();
    std::filesystem::remove_all(root);
}

// A PS that samples fetch slot 0 has that texture's storage read at the swap.
void frameTextures()
{
    const auto   root = freshRoot("rerevved-shader-harvest-test-textures");
    FrameFixture f;
    std::string  error;
    UNSCOPED_INFO("the texture harvest starts: " + error);
    REQUIRE(f.harvest.Start(root, { std::nullopt, 1, 1ULL << 30 }, error));
    // Two CF words: exec_end at address 1, one fetch; then a texture fetch of slot 0.
    const std::array<std::uint32_t, 6> program{ 1 | 1 << 12 | 1 << 16, 0x2000, 0, 1, 0, 0 };
    for (std::size_t i = 0; i < program.size(); ++i)
        f.memory.put(kPixelCode + i * 4, program[i], true);
    f.memory.put(kPixel + 40 + 0x100 + 4, 24);
    // A 1x1 8_8_8_8 texture at physical 0x300000.
    const rerevved::gpu::NativeTextureFetch fetch{ 2U | (1U << 22), 0x00300006, 0, 0xD10, 0, 0x200 };
    for (std::size_t w = 0; w < fetch.size(); ++w)
        f.memory.put(kDevice + 0x480 + w * 4, fetch[w]);
    rerevved::gpu::NativeTextureMemoryRanges ranges;
    UNSCOPED_INFO("the fetch describes its storage: " + error);
    REQUIRE((rerevved::gpu::GetNativeTextureMemoryRanges(fetch, ranges, error) && ranges.base.size));
    std::vector<std::uint8_t> texels(ranges.base.size);
    for (std::size_t i = 0; i < texels.size(); ++i)
        texels[i] = static_cast<std::uint8_t>(i * 7 + 1);
    f.memory.regions.push_back({ ranges.base.address, true, texels });

    f.clear();
    f.draw();
    f.swap();
    UNSCOPED_INFO("the key samples slot 0");
    REQUIRE((f.harvest.Pairs().size() == 1 && f.harvest.Pairs()[0].textureMask == 1));
    const auto forms = f.harvest.FrameForms();
    UNSCOPED_INFO("the texture frame is written");
    REQUIRE((forms.size() == 1 && forms[0].instances.size() == 1));
    const auto  frame = toml::parse_file((root / "frames" / forms[0].instances[0] / "frame.toml").string());
    const auto* rows  = frame["events"][1]["memory"].as_array();
    UNSCOPED_INFO("the draw lists its memory");
    REQUIRE(rows != nullptr);
    bool found = false;
    for (std::size_t i = 0; i < rows->size(); ++i)
    {
        const auto row = frame["events"][1]["memory"][i];
        if (row["role"].value_or(std::string()) != "texture 0 base")
            continue;
        found = true;
        UNSCOPED_INFO("the sampled texture is read at the swap from its physical base");
        REQUIRE((row["error"].value_or(std::string()).empty() && row["read"].value_or(std::string()) == "frame_end" && row["physical"].value_or(false) &&
                 row["address"].value_or(std::string()) == std::format("0x{:08X}", ranges.base.address) &&
                 row["size"].value_or<std::int64_t>(0) == static_cast<std::int64_t>(ranges.base.size)));
        const auto bytes = row["bytes"].value_or(std::string());
        if (bytes.empty())
        {
            UNSCOPED_INFO("the texture payload keeps its bytes");
            REQUIRE(readFile(root / row["file"].value_or(std::string())) == texels);
        }
        else
        {
            UNSCOPED_INFO("the texture bytes are kept inline");
            REQUIRE((bytes.size() == texels.size() * 2 && bytes.starts_with("01080f")));
        }
    }
    UNSCOPED_INFO("the draw's sampled texture is written");
    REQUIRE(found);
    f.harvest.Stop();
    std::filesystem::remove_all(root);
}

// Past the frame buffer a long frame releases each operation's storage.
void frameStorage()
{
    const auto           root = freshRoot("rerevved-shader-harvest-test-storage");
    FrameFixture         f;
    std::string          error;
    ShaderHarvestOptions options;
    options.frameInstances   = 1;
    options.frameBytes       = 1ULL << 30;
    options.frameBufferBytes = 0x8000;
    UNSCOPED_INFO("the storage harvest starts: " + error);
    REQUIRE(f.harvest.Start(root, options, error));
    f.clear();
    for (int i = 0; i < 20; ++i)
        f.draw();
    UNSCOPED_INFO(std::format("a truncated frame holds no more than its buffer ({} bytes)", f.harvest.RetainedFrameBytes()));
    REQUIRE(f.harvest.RetainedFrameBytes() <= options.frameBufferBytes);
    f.swap();
    UNSCOPED_INFO("the truncated frame is counted, not written");
    REQUIRE((f.harvest.RetainedFrameBytes() == 0 && f.harvest.FrameForms().size() == 1 && f.harvest.FrameForms()[0].instances.empty()));
    UNSCOPED_INFO("the storage index writes: " + error);
    REQUIRE(f.harvest.WriteIndex(error));
    UNSCOPED_INFO("the index counts the truncated frame");
    REQUIRE(toml::parse_file((root / "frames" / "index.toml").string())["truncated_frames"].value_or<std::int64_t>(0) == 1);
    f.harvest.Stop();
    std::filesystem::remove_all(root);
}

// Past the signature cap a new signature is counted, not listed or logged.
void signatureCap()
{
    const auto           root = freshRoot("rerevved-shader-harvest-test-signatures");
    FrameFixture         f;
    std::string          error;
    ShaderHarvestOptions options;
    options.frameInstances  = 1;
    options.frameBytes      = 1ULL << 30;
    options.frameSignatures = 1;
    UNSCOPED_INFO("the capped harvest starts: " + error);
    REQUIRE(f.harvest.Start(root, options, error));
    f.clear();
    f.draw();
    f.swap();
    f.clear();
    f.clear();
    f.swap();
    f.clear();
    f.draw();
    f.swap();
    const auto forms = f.harvest.FrameForms();
    UNSCOPED_INFO(std::format("one listed signature counts its two frames ({} listed)", forms.size()));
    REQUIRE((forms.size() == 1 && forms[0].frames == 2));
    UNSCOPED_INFO("the capped index writes: " + error);
    REQUIRE(f.harvest.WriteIndex(error));
    const auto index = toml::parse_file((root / "frames" / "index.toml").string());
    UNSCOPED_INFO("the index names the cap and counts the unlisted frame");
    REQUIRE((index["signature_limit"].value_or<std::int64_t>(0) == 1 && index["unlisted_frames"].value_or<std::int64_t>(0) == 1 && index["signatures"].as_array()->size() == 1));
    std::size_t logged = 0;
    {
        std::ifstream log(root / "frames" / "signatures.log");
        for (std::string line; std::getline(log, line);)
            logged += line.starts_with("signature ");
    }
    UNSCOPED_INFO(std::format("signatures.log lists only the listed signature ({} found)", logged));
    REQUIRE(logged == 1);
    f.harvest.Stop();
    std::filesystem::remove_all(root);
}

// Coverage preserves operation order and compares draws within each segment.
void coverageForms()
{
    const std::string form    = "[rt 1 pitch 640 msaa 1 color 0 edram 000 depth 1 edram 530 extent 640x720 tiles 2] tiling 2";
    const std::string full    = "C flags 3f index 0 rect 0,0,1280,720 " + form;
    const auto        stencil = [&](const char* rect)
    {
        return std::format("C flags 20 index ffffffff rect {} {}", rect, form);
    };
    const auto frame = [&](const char* rect, const char* draws)
    {
        return ShaderHarvestCoverageOf({ full, stencil(rect), draws, "R source 0", "S swap" });
    };
    const auto inside = frame("10,20,300,400", "{D 1 | D 2}");
    UNSCOPED_INFO("rectangles inside the output share a class");
    REQUIRE(frame("100,100,200,200", "{D 2}").shape == inside.shape);
    UNSCOPED_INFO("a rectangle past the output is another class");
    REQUIRE(frame("830,624,1290,720", "{D 1 | D 2}").shape != inside.shape);
    UNSCOPED_INFO("an empty rectangle fails as one past the output does");
    REQUIRE(frame("10,10,10,20", "{D 1}").shape == frame("830,624,1290,720", "{D 1}").shape);
    UNSCOPED_INFO("the whole output is its own class");
    REQUIRE(frame("0,0,1280,720", "{D 1}").shape != inside.shape);
    const auto color = [&](const char* rect)
    {
        return ShaderHarvestCoverageOf({ std::format("C flags 3f index 0 rect {} {}", rect, form), "{D 1}", "S swap" }).shape;
    };
    UNSCOPED_INFO("any other clear keeps its rectangle");
    REQUIRE(color("0,0,512,720") != color("0,0,256,720"));

    ShaderHarvestCoverageSet set;
    set.Add(inside);
    UNSCOPED_INFO("fewer draws of a segment are covered");
    REQUIRE(set.Covers(frame("100,100,200,200", "{D 2}")));
    UNSCOPED_INFO("another draw is not covered");
    REQUIRE(!set.Covers(frame("10,20,300,400", "{D 1 | D 3}")));
    // An MRT pair: the assembler refuses a set draw between its two clears.
    const std::string        rt0 = "C flags 3f index 0 rect 0,0,1280,720 " + form, rt1 = "C flags f index 1 rect 0,0,1280,720 " + form;
    ShaderHarvestCoverageSet pair;
    pair.Add(ShaderHarvestCoverageOf({ rt0, rt1, "{D 1}", "R source 0", "S swap" }));
    UNSCOPED_INFO("a draw between the pair's clears is not covered");
    REQUIRE(!pair.Covers(ShaderHarvestCoverageOf({ rt0, "{D 1}", rt1, "R source 0", "S swap" })));
    UNSCOPED_INFO("a segment without draws is another shape");
    REQUIRE(!pair.Covers(ShaderHarvestCoverageOf({ rt0, rt1, "R source 0", "S swap" })));
    UNSCOPED_INFO("the same order is covered");
    REQUIRE(pair.Covers(ShaderHarvestCoverageOf({ rt0, rt1, "{D 1}", "R source 0", "S swap" })));
    // One witness frame must hold every segment.
    ShaderHarvestCoverageSet two;
    two.Add(frame("10,20,300,400", "{D 1}"));
    two.Add(frame("10,20,300,400", "{D 2}"));
    UNSCOPED_INFO("draws of one segment from two frames are not covered");
    REQUIRE(!two.Covers(frame("10,20,300,400", "{D 1 | D 2}")));
    const auto split = [&](const char* first, const char* second)
    {
        return ShaderHarvestCoverageOf({ full, first, "R source 0", second, "S swap" });
    };
    ShaderHarvestCoverageSet segments;
    segments.Add(split("{D 1}", "{D 3}"));
    segments.Add(split("{D 2}", "{D 4}"));
    UNSCOPED_INFO("segments from two frames are not covered");
    REQUIRE(!segments.Covers(split("{D 1}", "{D 4}")));
    UNSCOPED_INFO("both segments of one frame are covered");
    REQUIRE(segments.Covers(split("{D 2}", "{D 4}")));
}

// Known forms count only; new forms write even after the signature cap.
void knownHarvests()
{
    const auto first = freshRoot("rerevved-shader-harvest-test-known-a");
    const auto rect  = [](FrameFixture& f, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1)
    {
        HarvestOperation(f.harvest, { kHarvestClearEntry, 0, { kDevice, 0x20, 0xFFFFFFFF, x0, y0, x1, y1, 0 }, kStack, 0, 1.0 }, f.read);
    };
    std::string error;
    {
        FrameFixture f;
        UNSCOPED_INFO("the first harvest starts: " + error);
        REQUIRE(f.harvest.Start(first, { std::nullopt, 1, 1ULL << 30 }, error));
        f.clear();
        f.draw();
        f.resolve(0x100040);
        f.swap();
        f.clear();
        rect(f, 10, 10, 20, 20);
        f.draw();
        f.resolve(0x100040);
        f.swap();
        UNSCOPED_INFO("the first harvest writes both forms");
        REQUIRE((f.harvest.FrameForms().size() == 2 && f.harvest.FrameForms()[1].instances.size() == 1));
        f.harvest.Stop();
    }
    const auto keyDirectory = toml::parse_file((first / "index.toml").string())["pairs"][0]["directory"].value_or(std::string());

    const auto           second = freshRoot("rerevved-shader-harvest-test-known-b");
    FrameFixture         f;
    ShaderHarvestOptions options;
    options.frameInstances  = 2;
    options.frameBytes      = 1ULL << 30;
    options.frameSignatures = 3;
    options.known           = { first };
    UNSCOPED_INFO("the known harvest starts: " + error);
    REQUIRE(f.harvest.Start(second, options, error));
    // The first form again: its signature has a written instance.
    f.clear();
    f.draw();
    f.resolve(0x100040);
    f.swap();
    // A new signature (another stencil rectangle) whose coverage is known.
    f.clear();
    rect(f, 30, 40, 50, 60);
    f.draw();
    f.resolve(0x100040);
    f.swap();
    // A new pixel shader is new content: written, twice.
    auto other = memoryFor(0x11111111, 0x77777777);
    for (int i = 0; i < 3; ++i)
    {
        f.clear();
        HarvestOperation(f.harvest, callFrom(kLeaderCaller), readerFor(other));
        f.resolve(0x100040);
        f.swap();
    }
    // Past the cap of three a new signature is listed only when it is new content.
    f.clear();
    rect(f, 70, 80, 90, 100);
    f.draw();
    f.resolve(0x100040);
    f.swap();
    f.clear();
    f.clear();
    HarvestOperation(f.harvest, callFrom(0x8250C768), f.read);
    f.resolve(0x100040);
    f.swap();

    const auto forms = f.harvest.FrameForms();
    const auto pairs = f.harvest.Pairs();
    UNSCOPED_INFO(std::format("three listed signatures and one new past the cap ({} listed)", forms.size()));
    REQUIRE(forms.size() == 4);
    UNSCOPED_INFO("the known signature is not written again");
    REQUIRE((forms[0].instances.empty() && forms[0].knownHarvest == first.string()));
    UNSCOPED_INFO("the covered signature is not written");
    REQUIRE((forms[1].instances.empty() && forms[1].frames == 1));
    UNSCOPED_INFO("the new form is written up to its instance count");
    REQUIRE((forms[2].instances.size() == 2 && forms[2].frames == 3));
    UNSCOPED_INFO("a new form past the cap is still written");
    REQUIRE(forms[3].instances.size() == 1);
    UNSCOPED_INFO("the known key is counted under its known record; a new key is recorded");
    REQUIRE((pairs.size() == 3 && pairs[0].knownHarvest == first.string() && pairs[0].directory == keyDirectory && pairs[0].draws == 3 &&
             !std::filesystem::exists(second / keyDirectory) && pairs[1].knownHarvest.empty() && std::filesystem::exists(second / pairs[1].directory / "record.toml")));
    UNSCOPED_INFO("the known index writes: " + error);
    REQUIRE(f.harvest.WriteIndex(error));
    const auto index  = toml::parse_file((second / "index.toml").string());
    const auto frames = toml::parse_file((second / "frames" / "index.toml").string());
    UNSCOPED_INFO("index.toml names the known record and counts its draws");
    REQUIRE((index["pairs"][0]["known_harvest"].value_or(std::string()) == first.string() && index["known_harvests"][0].value_or(std::string()) == first.string() &&
             index["pairs"][0]["draws"].value_or<std::int64_t>(0) == 3));
    UNSCOPED_INFO("frames/index.toml counts known, covered and unlisted frames");
    REQUIRE((frames["known_frames"].value_or<std::int64_t>(0) == 1 && frames["covered_frames"].value_or<std::int64_t>(0) == 1 &&
             frames["unlisted_frames"].value_or<std::int64_t>(0) == 1 && frames["signatures"][0]["known_harvest"].value_or(std::string()) == first.string()));
    f.harvest.Stop();

    // An index before schema 3 lends no key records: their state lacks the constants.
    {
        std::ifstream in(first / "index.toml", std::ios::binary);
        std::string   text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        in.close();
        const auto at = text.find("schema_version = 3");
        UNSCOPED_INFO("the first index is schema 3");
        REQUIRE(at != std::string::npos);
        text.replace(at, 18, "schema_version = 2");
        std::ofstream(first / "index.toml", std::ios::binary) << text;
    }
    const auto   third = freshRoot("rerevved-shader-harvest-test-known-c");
    FrameFixture g;
    UNSCOPED_INFO("the schema 2 harvest starts: " + error);
    REQUIRE(g.harvest.Start(third, options, error));
    g.draw();
    UNSCOPED_INFO("a schema 2 key is recorded again");
    REQUIRE((g.harvest.Pairs().size() == 1 && g.harvest.Pairs()[0].knownHarvest.empty() && std::filesystem::exists(third / g.harvest.Pairs()[0].directory / "record.toml")));
    g.harvest.Stop();

    ShaderHarvest missing;
    const auto    fourth = freshRoot("rerevved-shader-harvest-test-known-d");
    options.known        = { first / "absent" };
    UNSCOPED_INFO("an unreadable known harvest refuses to start: " + error);
    REQUIRE((!missing.Start(fourth, options, error) && !error.empty() && !std::filesystem::exists(fourth)));
    for (const auto& root : { first, second, third })
        std::filesystem::remove_all(root);
}

// Arm files gate frame writes while keys/signatures count from the start.
void armFile()
{
    const auto           root = freshRoot("rerevved-shader-harvest-test-arm");
    FrameFixture         f;
    std::string          error;
    ShaderHarvestOptions options;
    options.frameInstances = 1;
    options.frameBytes     = 1ULL << 30;
    options.armFile        = root / "arm";
    UNSCOPED_INFO("the armed harvest starts: " + error);
    REQUIRE(f.harvest.Start(root, options, error));
    for (int i = 0; i < 2; ++i)
    {
        if (i)
            std::ofstream(root / "arm") << "";
        f.clear();
        f.draw();
        f.resolve(0x100040);
        f.swap();
    }
    const auto forms = f.harvest.FrameForms();
    UNSCOPED_INFO("the unarmed frame is counted and the armed one written");
    REQUIRE((forms.size() == 1 && forms[0].frames == 2 && forms[0].instances.size() == 1 && forms[0].instances[0].ends_with("-0") && f.harvest.Pairs().size() == 1));
    UNSCOPED_INFO("the armed index writes: " + error);
    REQUIRE(f.harvest.WriteIndex(error));
    const auto frames = toml::parse_file((root / "frames" / "index.toml").string());
    UNSCOPED_INFO("frames/index.toml names the arm file and its frame");
    REQUIRE((frames["unarmed_frames"].value_or<std::int64_t>(0) == 1 && frames["armed_frame"].value_or<std::int64_t>(-1) == 1 &&
             frames["arm_file"].value_or(std::string()) == (root / "arm").string()));
    f.harvest.Stop();
    std::filesystem::remove_all(root);
}

// The fixture's first form (clear, draw, resolve) and a stencil form.
void plainFrame(FrameFixture& f)
{
    f.clear();
    f.draw();
    f.resolve(0x100040);
    f.swap();
}

void stencilFrame(FrameFixture& f)
{
    f.clear();
    HarvestOperation(f.harvest, { kHarvestClearEntry, 0, { kDevice, 0x20, 0xFFFFFFFF, 10, 10, 20, 20, 0 }, kStack, 0, 1.0 }, f.read);
    f.draw();
    f.resolve(0x100040);
    f.swap();
}

// Presented work before the clear: a marked form.
void markedFrame(FrameFixture& f)
{
    f.draw();
    f.clear();
    f.draw();
    f.resolve(0x100040);
    f.swap();
}

ShaderHarvestOptions knownOptions(std::vector<std::filesystem::path> known)
{
    ShaderHarvestOptions options;
    options.frameInstances = 1;
    options.frameBytes     = 1ULL << 30;
    options.known          = std::move(known);
    return options;
}

// Missing frame.toml/record.toml or source harvests require fresh writes.
void knownMissing()
{
    const auto  first = freshRoot("rerevved-shader-harvest-test-missing-a");
    std::string error;
    {
        FrameFixture f;
        UNSCOPED_INFO("the first harvest starts: " + error);
        REQUIRE(f.harvest.Start(first, knownOptions({}), error));
        plainFrame(f);
        stencilFrame(f);
        f.harvest.Stop();
    }
    const auto    keyDirectory = toml::parse_file((first / "index.toml").string())["pairs"][0]["directory"].value_or(std::string());
    std::uint64_t stencil      = 0;
    {
        const auto frames = toml::parse_file((first / "frames" / "index.toml").string());
        stencil           = std::stoull(frames["signatures"][1]["signature"].value_or(std::string()), nullptr, 16);
        std::filesystem::remove(first / "frames" / frames["signatures"][1]["instances"][0].value_or(std::string()) / "frame.toml");
    }
    const auto second = freshRoot("rerevved-shader-harvest-test-missing-b");
    {
        FrameFixture f;
        UNSCOPED_INFO("the second harvest starts: " + error);
        REQUIRE(f.harvest.Start(second, knownOptions({ first }), error));
        plainFrame(f);
        stencilFrame(f);
        const auto forms = f.harvest.FrameForms();
        UNSCOPED_INFO("a known instance without its frame.toml is written again");
        REQUIRE((forms.size() == 2 && forms[0].instances.empty() && forms[0].knownHarvest == first.string() && forms[1].signature == stencil &&
                 forms[1].instances.size() == 1 && forms[1].knownHarvest.empty()));
        UNSCOPED_INFO("the key record is lent");
        REQUIRE(f.harvest.Pairs()[0].knownHarvest == first.string());
        f.harvest.Stop();
    }
    // The second harvest lends the plain form and the key from the first,
    // which is gone now.
    std::filesystem::remove_all(first);
    const auto third = freshRoot("rerevved-shader-harvest-test-missing-c");
    {
        FrameFixture f;
        UNSCOPED_INFO("the third harvest starts: " + error);
        REQUIRE(f.harvest.Start(third, knownOptions({ second }), error));
        plainFrame(f);
        stencilFrame(f);
        const auto forms = f.harvest.FrameForms();
        UNSCOPED_INFO("a row lent from a missing harvest is written again");
        REQUIRE((forms.size() == 2 && forms[0].instances.size() == 1 && forms[0].knownHarvest.empty() && forms[1].instances.empty() && forms[1].knownHarvest == second.string()));
        UNSCOPED_INFO("a key lent from a missing harvest is recorded again");
        REQUIRE((f.harvest.Pairs()[0].knownHarvest.empty() && std::filesystem::exists(third / keyDirectory / "record.toml")));
        f.harvest.Stop();
    }
    // A key whose record.toml is missing is recorded again.
    std::filesystem::remove(third / keyDirectory / "record.toml");
    const auto fourth = freshRoot("rerevved-shader-harvest-test-missing-d");
    {
        FrameFixture f;
        UNSCOPED_INFO("the fourth harvest starts: " + error);
        REQUIRE(f.harvest.Start(fourth, knownOptions({ third }), error));
        plainFrame(f);
        UNSCOPED_INFO("a key without its record.toml is recorded again");
        REQUIRE((f.harvest.Pairs()[0].knownHarvest.empty() && std::filesystem::exists(fourth / keyDirectory / "record.toml")));
        f.harvest.Stop();
    }
    for (const auto& root : { second, third, fourth })
        std::filesystem::remove_all(root);
}

// Marked forms keep source harvests; missing frame.toml rewrites the instance.
void knownMarked()
{
    const auto  first = freshRoot("rerevved-shader-harvest-test-marked-a");
    std::string error;
    {
        FrameFixture f;
        UNSCOPED_INFO("the first harvest starts: " + error);
        REQUIRE(f.harvest.Start(first, knownOptions({}), error));
        markedFrame(f);
        UNSCOPED_INFO("the first harvest writes the marked form");
        REQUIRE(f.harvest.FrameForms()[0].markedInstances.size() == 1);
        f.harvest.Stop();
    }
    const auto second = freshRoot("rerevved-shader-harvest-test-marked-b");
    {
        FrameFixture f;
        UNSCOPED_INFO("the second harvest starts: " + error);
        REQUIRE(f.harvest.Start(second, knownOptions({ first }), error));
        markedFrame(f);
        const auto forms = f.harvest.FrameForms();
        UNSCOPED_INFO("a known marked form is counted and names its harvest");
        REQUIRE((forms.size() == 1 && forms[0].markedInstances.empty() && forms[0].knownHarvest == first.string() && f.harvest.FrameCounts().known == 1));
        f.harvest.Stop();
    }
    const auto third = freshRoot("rerevved-shader-harvest-test-marked-c");
    {
        FrameFixture f;
        UNSCOPED_INFO("the third harvest starts: " + error);
        REQUIRE(f.harvest.Start(third, knownOptions({ second }), error));
        markedFrame(f);
        const auto forms = f.harvest.FrameForms();
        UNSCOPED_INFO("the lent marked form is found through its name");
        REQUIRE((forms.size() == 1 && forms[0].markedInstances.empty() && forms[0].knownHarvest == first.string()));
        f.harvest.Stop();
    }
    {
        const auto frames = toml::parse_file((first / "frames" / "index.toml").string());
        std::filesystem::remove(first / "frames" / frames["signatures"][0]["draw_before_clear_instances"][0].value_or(std::string()) / "frame.toml");
    }
    const auto fourth = freshRoot("rerevved-shader-harvest-test-marked-d");
    {
        FrameFixture f;
        UNSCOPED_INFO("the fourth harvest starts: " + error);
        REQUIRE(f.harvest.Start(fourth, knownOptions({ first }), error));
        markedFrame(f);
        const auto forms = f.harvest.FrameForms();
        UNSCOPED_INFO("a marked instance without its frame.toml is written again");
        REQUIRE((forms.size() == 1 && forms[0].markedInstances.size() == 1 && forms[0].knownHarvest.empty()));
        f.harvest.Stop();
    }
    for (const auto& root : { first, second, third, fourth })
        std::filesystem::remove_all(root);
}

// Pre-arm signatures consume no post-arm slots, marked or unmarked.
void lateArming()
{
    const auto           root = freshRoot("rerevved-shader-harvest-test-late-arm");
    FrameFixture         f;
    std::string          error;
    ShaderHarvestOptions options;
    options.frameInstances  = 1;
    options.frameBytes      = 1ULL << 30;
    options.frameSignatures = 2;
    options.armFile         = root / "arm";
    UNSCOPED_INFO("the late-armed harvest starts: " + error);
    REQUIRE(f.harvest.Start(root, options, error));
    plainFrame(f);
    stencilFrame(f);
    f.clear();
    f.clear();
    f.draw();
    f.resolve(0x100040);
    f.swap();
    std::ofstream(root / "arm") << "";
    f.clear();
    HarvestOperation(f.harvest, callFrom(0x8250C768), f.read);
    f.resolve(0x100040);
    f.swap();
    markedFrame(f);
    const auto forms  = f.harvest.FrameForms();
    const auto counts = f.harvest.FrameCounts();
    UNSCOPED_INFO(std::format("the forms first seen after arming are written ({} listed)", forms.size()));
    REQUIRE((forms.size() == 4 && forms[0].instances.empty() && forms[1].instances.empty() && forms[2].instances.size() == 1 && forms[3].markedInstances.size() == 1));
    UNSCOPED_INFO(std::format("pre-arm frames are counted ({} unarmed, {} unlisted)", counts.unarmed, counts.unlisted));
    REQUIRE((counts.unarmed == 2 && counts.unlisted == 1));
    f.harvest.Stop();
    std::filesystem::remove_all(root);
}

// Failed writes do not retry; novel forms past the cap remain bounded.
void failedWrites()
{
    const auto  known = freshRoot("rerevved-shader-harvest-test-failed-a");
    std::string error;
    {
        FrameFixture f;
        UNSCOPED_INFO("the known harvest starts: " + error);
        REQUIRE(f.harvest.Start(known, knownOptions({}), error));
        plainFrame(f);
        f.harvest.Stop();
    }
    std::uint64_t stencil = 0;
    {
        const auto   probe = freshRoot("rerevved-shader-harvest-test-failed-b");
        FrameFixture f;
        UNSCOPED_INFO("the probe starts: " + error);
        REQUIRE(f.harvest.Start(probe, knownOptions({}), error));
        stencilFrame(f);
        stencil = f.harvest.FrameForms()[0].signature;
        f.harvest.Stop();
        std::filesystem::remove_all(probe);
    }
    const auto   root    = freshRoot("rerevved-shader-harvest-test-failed-c");
    auto         options = knownOptions({ known });
    FrameFixture f;
    options.frameSignatures = 1;
    UNSCOPED_INFO("the capped harvest starts: " + error);
    REQUIRE(f.harvest.Start(root, options, error));
    const auto blocker = root / "frames" / std::format("{:016X}-0", stencil);
    std::filesystem::create_directory(blocker);
    stencilFrame(f);
    std::filesystem::remove(blocker);
    stencilFrame(f);
    UNSCOPED_INFO("a failed form is not written again");
    REQUIRE(!std::filesystem::exists(blocker));
    // Two further novel forms: one is listed past the cap of one, the other only counted.
    f.clear();
    HarvestOperation(f.harvest, callFrom(0x8250C768), f.read);
    f.resolve(0x100040);
    f.swap();
    f.clear();
    f.clear();
    f.draw();
    f.resolve(0x100040);
    f.swap();
    const auto forms  = f.harvest.FrameForms();
    const auto counts = f.harvest.FrameCounts();
    UNSCOPED_INFO(std::format("the failed form and one novel form are listed ({} listed)", forms.size()));
    REQUIRE((forms.size() == 2 && forms[0].writeFailed && forms[0].frames == 2 && forms[0].instances.empty() && forms[1].instances.size() == 1));
    UNSCOPED_INFO(std::format("both failed frames and the unlisted one are counted ({} failed, {} unlisted)", counts.failed, counts.unlisted));
    REQUIRE((counts.failed == 2 && counts.unlisted == 1));
    UNSCOPED_INFO("the capped index writes: " + error);
    REQUIRE(f.harvest.WriteIndex(error));
    const auto frames = toml::parse_file((root / "frames" / "index.toml").string());
    UNSCOPED_INFO("frames/index.toml marks the failed form");
    REQUIRE((frames["failed_frames"].value_or<std::int64_t>(0) == 2 && frames["signatures"][0]["write_failed"].value_or(false)));
    f.harvest.Stop();
    for (const auto& path : { known, root })
        std::filesystem::remove_all(path);
}

// Replay keeps frame order and arm frame; instance bytes use recorded means.
void replay(const std::filesystem::path& known, const std::filesystem::path& recorded, std::uint64_t split, std::uint64_t budgetMib, std::uint32_t signatures, std::uint32_t perForm, std::uint64_t armAt)
{
    // Replay defaults: 8192 MiB, 4096 signatures, two instances, no arm frame.
    std::map<std::uint64_t, std::vector<std::string>> canonical;
    {
        std::ifstream             log(recorded / "frames" / "signatures.log", std::ios::binary);
        std::vector<std::string>* tokens = nullptr;
        for (std::string line; std::getline(log, line);)
        {
            if (line.ends_with('\r'))
                line.pop_back();
            if (line.starts_with("signature "))
                tokens = &canonical[std::stoull(line.substr(10, 16), nullptr, 16)];
            else if (line.empty())
                tokens = nullptr;
            else if (tokens)
                tokens->push_back(line);
        }
    }
    const auto index = toml::parse_file((recorded / "frames" / "index.toml").string());

    struct Row
    {
        std::uint64_t signature = 0, first = 0, frames = 0, marked = 0;
    };

    std::vector<Row> rows;
    std::uint64_t    instances = 0, markedInstances = 0;
    for (const auto& node : *index["signatures"].as_array())
    {
        const auto& row = *node.as_table();
        rows.push_back({ std::stoull(row["signature"].value_or(std::string()), nullptr, 16), static_cast<std::uint64_t>(row["first_frame"].value_or<std::int64_t>(0)), static_cast<std::uint64_t>(row["frames"].value_or<std::int64_t>(0)), static_cast<std::uint64_t>(row["draw_before_clear_frames"].value_or<std::int64_t>(0)) });
        instances += row["instances"].as_array()->size();
        markedInstances += row["draw_before_clear_instances"].as_array()->size();
    }
    std::ranges::stable_sort(rows, {}, &Row::first);
    const auto perInstance = index["bytes_written"].value_or<std::int64_t>(0) / std::max<std::int64_t>(1, static_cast<std::int64_t>(instances));
    const auto perMarked   = index["draw_before_clear_bytes_written"].value_or<std::int64_t>(0) / std::max<std::int64_t>(1, static_cast<std::int64_t>(markedInstances));

    const auto           root = freshRoot("rerevved-shader-harvest-replay");
    ShaderHarvest        harvest;
    ShaderHarvestOptions options;
    options.frameInstances = perForm;
    options.frameBytes     = budgetMib << 20;
    if (armAt)
        options.armFile = root / "arm";
    options.frameSignatures = signatures;
    options.known           = { known };
    std::string error;
    UNSCOPED_INFO("the replay starts: " + error);
    REQUIRE(harvest.Start(root, options, error));
    std::uint64_t missing = 0, writing[2] = {}, writingMarked = 0, written = 0;
    for (const auto& row : rows)
    {
        const auto found = canonical.find(row.signature);
        if (found == canonical.end())
        {
            ++missing;
            continue;
        }
        if (armAt && row.first >= armAt && !std::filesystem::exists(root / "arm"))
            std::ofstream(root / "arm") << "";
        const auto before = harvest.FrameCounts();
        for (std::uint64_t i = 0; i < std::min<std::uint64_t>(row.frames - row.marked, 3); ++i)
            harvest.ReplayFrame(row.signature, found->second, false, perInstance);
        for (std::uint64_t i = 0; i < std::min<std::uint64_t>(row.marked, 3); ++i)
            harvest.ReplayFrame(row.signature, found->second, true, perMarked);
        const auto after = harvest.FrameCounts();
        if (after.bytes > before.bytes)
            ++writing[row.first >= split];
        writingMarked += after.markedBytes > before.markedBytes;
    }
    for (const auto& form : harvest.FrameForms())
        written += form.instances.size() + form.markedInstances.size();
    const auto counts = harvest.FrameCounts();
    std::cout << std::format("recorded {}: {} signatures ({} without canonical tokens), {} frames, {} B per instance, {} B per draw-before-clear instance\n", recorded.string(), rows.size(), missing, index["frames"].value_or<std::int64_t>(0), perInstance, perMarked);
    std::cout << std::format("against {}: {} signatures write ({} before frame {}, {} from it), {} draw-before-clear signatures write\n", known.string(), writing[0] + writing[1], writing[0], split, writing[1], writingMarked);
    std::cout << std::format("{} instances, {} B unmarked + {} B draw-before-clear; {} listed forms; frames: {} known, {} covered, {} unlisted\n", written, counts.bytes, counts.markedBytes, harvest.FrameForms().size(), counts.known, counts.covered, counts.unlisted);
    harvest.Stop();
    std::filesystem::remove_all(root);
}

} // namespace

TEST_CASE("shader_harvest", "[ctest][shader-harvest]")
{
    rerevved::tests::ScopedLogging logging;
    callerParsing();
    harvest();
    sixHooks();
    overlappingLiterals();
    frameForms();
    framePayloads();
    frameTextures();
    frameStorage();
    signatureCap();
    coverageForms();
    knownHarvests();
    armFile();
    knownMissing();
    failedWrites();
    knownMarked();
    lateArming();
    std::cout << "Shader harvest: caller parsing, key dedupe, vertex-only records, six hooks, records, skipped draws, index, Stop, overlapping literals, frame forms, payload, "
                 "texture, storage, signature cap, coverage, known harvest, arm file, missing known file, failed write, known marked and late arming checks passed\n";
}

// Replays a recorded harvest against a known one:
//   shader-harvest.replay <known harvest> <recorded harvest> [split frame] [budget MiB] [signatures] [instances] [arm frame]
TEST_CASE("shader_harvest_replay", "[.][manual][shader-harvest]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() == 0)
    {
        SKIP("pass the harvests through --helper shader-harvest.replay");
    }
    UNSCOPED_INFO("replay takes a known harvest, a recorded harvest and up to five numbers");
    REQUIRE((args.argc() >= 2 && args.argc() <= 7));
    rerevved::tests::ScopedLogging logging;
    char* const*                   argv = args.argv();
    const int                      argc = args.argc() + 1;
    replay(argv[1], argv[2], argc > 3 ? std::stoull(argv[3]) : 114000, argc > 4 ? std::stoull(argv[4]) : 8192, argc > 5 ? static_cast<std::uint32_t>(std::stoul(argv[5])) : 4096, argc > 6 ? static_cast<std::uint32_t>(std::stoul(argv[6])) : 2, argc > 7 ? std::stoull(argv[7]) : 0);
}

REREVVED_MANUAL_HELPER(shaderHarvestReplay, "shader-harvest.replay", "shader_harvest_replay")
