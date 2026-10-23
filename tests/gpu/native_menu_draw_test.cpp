#include "gpu/d3d12/native_menu_draw.h"
#include "gpu/d3d12/native_menu_frame.h"
#include "manual_cases.hpp"

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iostream>
#include <toml++/toml.hpp>

using namespace rerevved::gpu;

namespace
{

std::vector<std::uint8_t> read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    const auto    size = std::filesystem::file_size(path);
    UNSCOPED_INFO("fixture byte bound");
    REQUIRE(size <= 16U * 1024U * 1024U);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    UNSCOPED_INFO(path.string());
    REQUIRE(bool(file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())));
    return bytes;
}

std::vector<std::uint32_t> words(const std::vector<std::uint8_t>& bytes)
{
    UNSCOPED_INFO("fixture word alignment");
    REQUIRE(bytes.size() % 4 == 0);
    std::vector<std::uint32_t> result(bytes.size() / 4);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = bytes[i * 4] | (std::uint32_t(bytes[i * 4 + 1]) << 8) |
                    (std::uint32_t(bytes[i * 4 + 2]) << 16) | (std::uint32_t(bytes[i * 4 + 3]) << 24);
    return result;
}

void geometry()
{
    std::vector<std::uint32_t> registers(0x5003);
    registers[0x48BE]                        = 0x4003;
    registers[0x48BF]                        = 0x1000001A;
    const std::vector<std::uint8_t> vertices = {
        0, 2, 0xFF, 0xFF, 0xFF, 0, 0x80, 0, 0, 2, 0xFF, 0xFF, 0xFF, 0, 0x80, 0, 0, 2, 0xFF, 0xFF, 0xFF, 0, 0x80, 0
    };
    std::vector<std::uint8_t> indices = { 0, 0, 0, 2, 0, 1 };
    NativeMenuDrawView        view;
    view.registers        = registers;
    view.vertexShaderHash = 0x11213E38D7154104ULL;
    view.pixelShaderHash  = 0x3A92D78FE55C7B83ULL;
    view.vertexGuestBase  = 0x4000;
    view.vertexBytes      = vertices;
    view.indexBytes       = indices;
    view.indexCount       = 3;
    NativeDrawReplayRecipe recipe;
    std::string            error;
    UNSCOPED_INFO(error);
    REQUIRE(DecodeNativeMenuDrawGeometry(view, recipe, error));
    auto decoded = words(recipe.vertexData);
    UNSCOPED_INFO("big endian indices");
    REQUIRE(recipe.indices == std::vector<std::uint32_t>{ 0, 2, 1 });
    UNSCOPED_INFO("signed vertex pair without destination swizzle");
    REQUIRE((decoded[0] == std::bit_cast<std::uint32_t>(-1.0F) &&
             decoded[1] == std::bit_cast<std::uint32_t>(2.0F)));
    UNSCOPED_INFO("packed normalized vertex color");
    REQUIRE((decoded[5] == std::bit_cast<std::uint32_t>(128.0F / 255.0F) &&
             decoded[7] == std::bit_cast<std::uint32_t>(1.0F)));
    indices[5] = 3;
    UNSCOPED_INFO("out of range geometry rejected");
    REQUIRE(!DecodeNativeMenuDrawGeometry(view, recipe, error));
    indices[5] = 1;
    registers[0x48BF] ^= 1;
    UNSCOPED_INFO("foreign fetch endian rejected");
    REQUIRE(!DecodeNativeMenuDrawGeometry(view, recipe, error));
    registers[0x48BF] ^= 1;
    view.indexed    = false;
    view.indexBytes = {};
    UNSCOPED_INFO(error);
    REQUIRE(DecodeNativeMenuDrawGeometry(view, recipe, error));
    UNSCOPED_INFO("nonindexed triangle sequence");
    REQUIRE(recipe.indices == std::vector<std::uint32_t>{ 0, 1, 2 });
    NativeMenuShaders shaders;
    UNSCOPED_INFO("unverified guest shader rejected");
    REQUIRE(!BuildNativeMenuDrawRecipe(view, shaders, recipe, error));
    registers[0x48BE] = registers[0x48BF] = 0;
    view.guestSourceVertices              = true;
    UNSCOPED_INFO("CPU source geometry does not require an uploaded fetch descriptor");
    REQUIRE(DecodeNativeMenuDrawGeometry(view, recipe, error));
    view.indexed    = true;
    view.indexBytes = indices;
    indices[5]      = 3;
    UNSCOPED_INFO("CPU source indices remain bounded by owned vertex bytes");
    REQUIRE(!DecodeNativeMenuDrawGeometry(view, recipe, error));
    std::vector<std::uint8_t> movieVertices(80);
    view.vertexShaderHash = 0xC33871A8CFA8967AULL;
    view.pixelShaderHash  = 0xA8D1E41B3FBB2D15ULL;
    view.vertexBytes      = movieVertices;
    view.indexBytes       = {};
    view.indexed          = false;
    view.primitive        = 6;
    view.indexCount       = 4;
    UNSCOPED_INFO("four-vertex movie strip admitted");
    REQUIRE(DecodeNativeMenuDrawGeometry(view, recipe, error));
    UNSCOPED_INFO("movie strip preserves alternating winding");
    REQUIRE(recipe.indices == std::vector<std::uint32_t>{ 0, 1, 2, 2, 1, 3 });
    view.primitive = 4;
    UNSCOPED_INFO("movie list topology rejected");
    REQUIRE(!DecodeNativeMenuDrawGeometry(view, recipe, error));
}

void fixture(const std::filesystem::path& root, const NativeMenuShaders& shaders)
{
    const auto             table     = toml::parse_file((root / "view.toml").string());
    auto                   registers = words(read(root / "registers.bin"));
    const auto             vs        = words(read(root / "vertex.ucode.bin"));
    const auto             ps        = words(read(root / "pixel.ucode.bin"));
    const auto             vertices  = read(root / "vertex-fetch.bin");
    const auto             indices   = read(root / "guest-indices.bin");
    NativeDrawReplayRecipe expected;
    std::string            error;
    UNSCOPED_INFO(error);
    REQUIRE(LoadNativeDrawReplayRecipe(root / "frame.toml", expected, error));
    NativeMenuDrawView view;
    view.registers        = registers;
    view.vertexMicrocode  = vs;
    view.pixelMicrocode   = ps;
    view.vertexShaderHash = expected.vertexShaderHash;
    view.pixelShaderHash  = expected.pixelShaderHash;
    view.vertexGuestBase  = registers[0x48BE] & ~3U;
    view.vertexBytes      = vertices;
    view.indexBytes       = indices;
    view.primitive        = table["primitive"].value_or(4U);
    view.indexCount       = table["requested_count"].value_or(expected.indexCount);
    view.indexed          = table["indexed"].value_or(true);
    view.indexEndian      = static_cast<std::uint32_t>(table["index_endian"].value_or(1));
    view.indexFormat      = static_cast<std::uint32_t>(table["index_format"].value_or(0));
    for (std::size_t i = 0; i < view.textures.size(); ++i)
        view.textures[i] = &expected.textures[i];
    view.sampler = &expected.sampler;
    NativeDrawReplayRecipe actual;
    UNSCOPED_INFO(root.string() + ": " + error);
    REQUIRE(BuildNativeMenuDrawRecipe(view, shaders, actual, error));
    UNSCOPED_INFO(root.string() + " geometry matches independently prepared bytes");
    REQUIRE((actual.vertexData == expected.vertexData && actual.indices == expected.indices));
    UNSCOPED_INFO(root.string() + " constant bytes");
    REQUIRE((actual.vertexConstants == expected.vertexConstants && actual.pixelConstants == expected.pixelConstants &&
             actual.sharedConstants == expected.sharedConstants));
    UNSCOPED_INFO(root.string() + " shader bytes");
    REQUIRE((actual.vertexShaderDxil == expected.vertexShaderDxil && actual.pixelShaderDxil == expected.pixelShaderDxil));
    UNSCOPED_INFO(root.string() + " input layout and masks");
    REQUIRE((actual.vertexStrideBytes == expected.vertexStrideBytes && actual.vertexAttributeCount == expected.vertexAttributeCount &&
             actual.sampleMask == expected.sampleMask && actual.textureMask == expected.textureMask));
    for (std::size_t slot = 0; slot < expected.textures.size(); ++slot)
    {
        UNSCOPED_INFO(root.string() + " ordered texture payload and mapping");
        REQUIRE((actual.textures[slot].bytes == expected.textures[slot].bytes &&
                 actual.textures[slot].width == expected.textures[slot].width &&
                 actual.textures[slot].height == expected.textures[slot].height &&
                 actual.textures[slot].format == expected.textures[slot].format &&
                 actual.textures[slot].swizzle == expected.textures[slot].swizzle));
    }
    UNSCOPED_INFO(root.string() + " viewport and scissor");
    REQUIRE((actual.viewport.x == expected.viewport.x && actual.viewport.y == expected.viewport.y &&
             actual.viewport.width == expected.viewport.width && actual.viewport.height == expected.viewport.height &&
             actual.viewport.minDepth == expected.viewport.minDepth && actual.viewport.maxDepth == expected.viewport.maxDepth &&
             actual.scissor.left == expected.scissor.left && actual.scissor.top == expected.scissor.top &&
             actual.scissor.right == expected.scissor.right && actual.scissor.bottom == expected.scissor.bottom));
    UNSCOPED_INFO(root.string() + " depth, rasterizer and blend state");
    REQUIRE((actual.depth.enabled == expected.depth.enabled && actual.depth.writeEnabled == expected.depth.writeEnabled &&
             actual.depth.compare == expected.depth.compare && actual.rasterizer.cull == expected.rasterizer.cull &&
             actual.rasterizer.frontCounterClockwise == expected.rasterizer.frontCounterClockwise &&
             actual.blend.enabled == expected.blend.enabled &&
             actual.blend.sourceColor == expected.blend.sourceColor && actual.blend.destinationColor == expected.blend.destinationColor &&
             actual.blend.sourceAlpha == expected.blend.sourceAlpha && actual.blend.destinationAlpha == expected.blend.destinationAlpha &&
             actual.blend.colorOp == expected.blend.colorOp && actual.blend.alphaOp == expected.blend.alphaOp));
    UNSCOPED_INFO("native clears have no captured attachments");
    REQUIRE((actual.initialSample0.empty() && actual.initialSample1.empty() && actual.depth.initialSamples.empty()));
    if (!actual.depth.enabled && view.primitive != 6)
    {
        const auto colorControl = registers[0x2202];
        registers[0x2202]       = 0x87000004U;
        UNSCOPED_INFO("menu disabled alpha test");
        REQUIRE(BuildNativeMenuDrawRecipe(view, shaders, actual, error));
        registers[0x2202] = 0x87000007U;
        UNSCOPED_INFO("copyright disabled alpha test");
        REQUIRE(BuildNativeMenuDrawRecipe(view, shaders, actual, error));
        registers[0x2202] |= 8;
        UNSCOPED_INFO("enabled UI alpha test rejected");
        REQUIRE(!BuildNativeMenuDrawRecipe(view, shaders, actual, error));
        registers[0x2202]       = colorControl;
        const auto depthControl = registers[0x2200];
        registers[0x2200]       = 0x24F00770U;
        UNSCOPED_INFO("menu disabled stencil test");
        REQUIRE(BuildNativeMenuDrawRecipe(view, shaders, actual, error));
        registers[0x2200] = 0x24F00270U;
        UNSCOPED_INFO("loading disabled stencil test");
        REQUIRE(BuildNativeMenuDrawRecipe(view, shaders, actual, error));
        registers[0x2200] |= 1;
        UNSCOPED_INFO("enabled UI stencil test rejected");
        REQUIRE(!BuildNativeMenuDrawRecipe(view, shaders, actual, error));
        registers[0x2200] = depthControl;
    }
}

void frameFixture(const std::filesystem::path& root, const NativeMenuShaders& shaders)
{
    const auto  table   = toml::parse_file((root / "events.toml").string());
    const auto* records = table["events"].as_array();
    UNSCOPED_INFO("frame event metadata");
    REQUIRE((records && !records->empty() && records->size() <= 1024));

    struct Owned
    {
        std::vector<std::uint32_t> registers, vs, ps;
        std::vector<std::uint8_t>  vertex, indices;
        NativeDrawReplayRecipe     expected;
    };

    std::vector<Owned>                    owned(records->size());
    std::vector<NativeMenuFrameEventView> events(records->size());
    for (std::size_t i = 0; i < records->size(); ++i)
    {
        const auto path     = root / (*records)[i].value_or(std::string{});
        const auto metadata = toml::parse_file((path / "event.toml").string());
        auto&      storage  = owned[i];
        auto&      event    = events[i];
        const auto optional = [&](const char* name)
        {
            return std::filesystem::exists(path / name) ? read(path / name) : std::vector<std::uint8_t>{};
        };
        storage.registers  = words(read(path / "registers.bin"));
        storage.vs         = words(optional("vertex.ucode.bin"));
        storage.ps         = words(optional("pixel.ucode.bin"));
        storage.vertex     = optional("vertex-fetch.bin");
        storage.indices    = optional("guest-indices.bin");
        const auto integer = [&](const char* key)
        {
            return metadata[key].value_or(std::int64_t{});
        };
        const auto hash = [&](const char* key)
        {
            return std::stoull(metadata[key].value_or(std::string{ "0" }), nullptr, 16);
        };
        event.id                    = integer("id");
        event.kind                  = static_cast<NativeMenuFrameEventView::Kind>(integer("kind"));
        event.success               = metadata["success"].value_or(false);
        event.hostIssued            = metadata["host_issued"].value_or(false);
        event.resolve               = metadata["resolve"].value_or(false);
        event.primitiveType         = static_cast<std::uint32_t>(integer("primitive_type"));
        event.draw.primitive        = event.primitiveType;
        event.hostPixelShaderHash   = hash("host_pixel_hash");
        event.frontbuffer           = hash("frontbuffer");
        event.frontbufferWidth      = static_cast<std::uint32_t>(integer("frontbuffer_width"));
        event.frontbufferHeight     = static_cast<std::uint32_t>(integer("frontbuffer_height"));
        event.draw.registers        = storage.registers;
        event.draw.vertexMicrocode  = storage.vs;
        event.draw.pixelMicrocode   = storage.ps;
        event.draw.vertexShaderHash = hash("vertex_hash");
        event.draw.pixelShaderHash  = hash("pixel_hash");
        event.draw.vertexBytes      = storage.vertex;
        event.draw.indexBytes       = storage.indices;
        event.draw.vertexGuestBase  = static_cast<std::uint32_t>(integer("vertex_base"));
        event.draw.indexCount       = static_cast<std::uint32_t>(integer("index_count"));
        event.draw.indexed          = metadata["indexed"].value_or(false);
        event.draw.indexFormat      = static_cast<std::uint32_t>(integer("index_format"));
        event.draw.indexEndian      = static_cast<std::uint32_t>(integer("index_endian"));
        if (event.primitiveType == 4 || event.primitiveType == 6)
        {
            std::string error;
            const auto  expectedPath = root.parent_path() / metadata["prepared"].value_or(std::string{}) / "frame.toml";
            UNSCOPED_INFO(error);
            REQUIRE(LoadNativeDrawReplayRecipe(expectedPath, storage.expected, error));
            for (std::size_t slot = 0; slot < event.draw.textures.size(); ++slot)
                event.draw.textures[slot] = &storage.expected.textures[slot];
            event.draw.sampler = &storage.expected.sampler;
        }
    }
    const auto              gamma = words(read(root.parent_path() / "gamma.bin"));
    NativeFrameReplayRecipe frame;
    std::string             error;
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeMenuFrameRecipe(events, shaders, gamma, frame, error));
    const auto expectedDraws = table["draws_per_half"].value_or(77U);
    UNSCOPED_INFO("captured full frame draw order");
    REQUIRE((frame.halves[0].size() == expectedDraws && frame.halves[1].size() == expectedDraws));
    const auto expectedAlpha = table["clear_alpha"].value_or(0U);
    UNSCOPED_INFO("captured clear alpha retained in each native half");
    REQUIRE((frame.halves[0].front().clearColor[3] == expectedAlpha &&
             frame.halves[1].front().clearColor[3] == expectedAlpha));
    if (events.front().kind != NativeMenuFrameEventView::Kind::Swap)
    {
        auto withStart = events;
        auto start     = events.back();
        start.id       = 1;
        for (auto& event : withStart)
            ++event.id;
        withStart.insert(withStart.begin(), start);
        UNSCOPED_INFO("completed start swap boundary: " + error);
        REQUIRE(BuildNativeMenuFrameRecipe(withStart, shaders, gamma, frame, error));
    }
    auto       damaged      = events;
    const auto initialClear = std::find_if(damaged.begin(), damaged.end(), [](const auto& event)
                                           {
                                               return event.kind == NativeMenuFrameEventView::Kind::Draw &&
                                                      event.hostIssued && event.primitiveType == 8 &&
                                                      event.draw.vertexBytes.size() == 84;
                                           });
    UNSCOPED_INFO("frame fixture initial clear metadata");
    REQUIRE(initialClear != damaged.end());
    auto                       clearView = initialClear->draw;
    std::vector<std::uint32_t> clearRegisters(clearView.registers.begin(), clearView.registers.end());
    clearView.registers     = clearRegisters;
    std::uint8_t clearAlpha = 0;
    clearRegisters[0x2203]  = 0x05;
    UNSCOPED_INFO("gameplay hierarchical-depth clear retains black/far clear alpha");
    REQUIRE((ValidateNativeMenuInitialClear(clearView, clearAlpha, error) && clearAlpha == expectedAlpha));
    clearRegisters[0x2203] = 0x25;
    UNSCOPED_INFO("unknown hierarchical-depth clear rejected");
    REQUIRE(!ValidateNativeMenuInitialClear(clearView, clearAlpha, error));
    clearRegisters[0x2203] = 0x05;
    clearRegisters[0x2200] &= ~4U;
    UNSCOPED_INFO("gameplay clear without depth writes rejected");
    REQUIRE(!ValidateNativeMenuInitialClear(clearView, clearAlpha, error));
    damaged.erase(initialClear);
    UNSCOPED_INFO("missing initial clear rejected");
    REQUIRE(!BuildNativeMenuFrameRecipe(damaged, shaders, gamma, frame, error));
    damaged              = events;
    const auto firstDraw = std::find_if(damaged.begin(), damaged.end(), [](const auto& event)
                                        {
                                            return event.kind == NativeMenuFrameEventView::Kind::Draw &&
                                                   event.primitiveType == 1;
                                        });
    UNSCOPED_INFO("frame fixture image-producing metadata");
    REQUIRE(firstDraw != damaged.end());
    firstDraw->hostPixelShaderHash = 1;
    UNSCOPED_INFO("image-producing point rejected");
    REQUIRE(!BuildNativeMenuFrameRecipe(damaged, shaders, gamma, frame, error));
    damaged              = events;
    const auto firstCopy = std::find_if(damaged.begin(), damaged.end(), [](const auto& event)
                                        {
                                            return event.kind == NativeMenuFrameEventView::Kind::Copy;
                                        });
    UNSCOPED_INFO("frame fixture resolve metadata");
    REQUIRE(firstCopy != damaged.end());
    firstCopy->resolve = false;
    UNSCOPED_INFO("foreign resolve rejected");
    REQUIRE(!BuildNativeMenuFrameRecipe(damaged, shaders, gamma, frame, error));
    damaged = events;
    damaged.back().frontbuffer += 4096;
    UNSCOPED_INFO("unrelated presentation buffer rejected");
    REQUIRE(!BuildNativeMenuFrameRecipe(damaged, shaders, gamma, frame, error));
    std::cout << "verified ordered frame and four admission failures\n";
}

} // namespace

TEST_CASE("native_menu_draw", "[ctest][native-menu-draw]")
{
    geometry();
}

// Verifies saved menu draw fixtures: native-menu-draw.fixtures <fixture root> <shader root>.
TEST_CASE("native_menu_draw_fixtures", "[.][manual][native-menu-draw]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() == 0)
    {
        SKIP("pass the fixture and shader roots through --helper native-menu-draw.fixtures");
    }
    UNSCOPED_INFO("optional arguments: fixture root, shader root");
    REQUIRE(args.argc() == 2);
    NativeMenuShaders shaders;
    std::string       error;
    UNSCOPED_INFO(error);
    REQUIRE(LoadNativeMenuShaders(args.argv()[2], shaders, error));
    unsigned count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(args.argv()[1]))
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "view.toml"))
        {
            fixture(entry.path(), shaders);
            ++count;
        }
    UNSCOPED_INFO("fixture set must be nonempty");
    REQUIRE(count != 0);
    frameFixture(std::filesystem::path(args.argv()[1]) / "events", shaders);
    std::cout << "verified " << count << " draw fixtures\n";
}

REREVVED_MANUAL_HELPER(nativeMenuDrawFixtures, "native-menu-draw.fixtures", "native_menu_draw_fixtures")
