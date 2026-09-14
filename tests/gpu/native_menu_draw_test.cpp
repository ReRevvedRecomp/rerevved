#include "gpu/d3d12/native_menu_draw.h"
#include "gpu/d3d12/native_menu_frame.h"

#include <bit>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <toml++/toml.hpp>

using namespace rerevved::gpu;

namespace
{

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "native_menu_draw_test: " << message << '\n';
        std::exit(1);
    }
}

std::vector<std::uint8_t> read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    const auto    size = std::filesystem::file_size(path);
    require(size <= 16U * 1024U * 1024U, "fixture byte bound");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    require(bool(file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), path.string());
    return bytes;
}

std::vector<std::uint32_t> words(const std::vector<std::uint8_t>& bytes)
{
    require(bytes.size() % 4 == 0, "fixture word alignment");
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
    require(DecodeNativeMenuDrawGeometry(view, recipe, error), error);
    auto decoded = words(recipe.vertexData);
    require(recipe.indices == std::vector<std::uint32_t>{ 0, 2, 1 }, "big endian indices");
    require(decoded[0] == std::bit_cast<std::uint32_t>(-1.0F) &&
                decoded[1] == std::bit_cast<std::uint32_t>(2.0F),
            "signed vertex pair without destination swizzle");
    require(decoded[5] == std::bit_cast<std::uint32_t>(128.0F / 255.0F) &&
                decoded[7] == std::bit_cast<std::uint32_t>(1.0F),
            "packed normalized vertex color");
    indices[5] = 3;
    require(!DecodeNativeMenuDrawGeometry(view, recipe, error), "out of range geometry rejected");
    indices[5] = 1;
    registers[0x48BF] ^= 1;
    require(!DecodeNativeMenuDrawGeometry(view, recipe, error), "foreign fetch endian rejected");
    registers[0x48BF] ^= 1;
    view.indexed    = false;
    view.indexBytes = {};
    require(DecodeNativeMenuDrawGeometry(view, recipe, error), error);
    require(recipe.indices == std::vector<std::uint32_t>{ 0, 1, 2 }, "nonindexed triangle sequence");
    NativeMenuShaders shaders;
    require(!BuildNativeMenuDrawRecipe(view, shaders, recipe, error), "unverified guest shader rejected");
}

void fixture(const std::filesystem::path& root, const NativeMenuShaders& shaders)
{
    const auto             table     = toml::parse_file((root / "view.toml").string());
    const auto             registers = words(read(root / "registers.bin"));
    const auto             vs        = words(read(root / "vertex.ucode.bin"));
    const auto             ps        = words(read(root / "pixel.ucode.bin"));
    const auto             vertices  = read(root / "vertex-fetch.bin");
    const auto             indices   = read(root / "guest-indices.bin");
    NativeDrawReplayRecipe expected;
    std::string            error;
    require(LoadNativeDrawReplayRecipe(root / "frame.toml", expected, error), error);
    NativeMenuDrawView view;
    view.registers        = registers;
    view.vertexMicrocode  = vs;
    view.pixelMicrocode   = ps;
    view.vertexShaderHash = expected.vertexShaderHash;
    view.pixelShaderHash  = expected.pixelShaderHash;
    view.vertexGuestBase  = registers[0x48BE] & ~3U;
    view.vertexBytes      = vertices;
    view.indexBytes       = indices;
    view.indexCount       = expected.indexCount;
    view.indexed          = table["indexed"].value_or(true);
    view.indexEndian      = static_cast<std::uint32_t>(table["index_endian"].value_or(1));
    view.indexFormat      = static_cast<std::uint32_t>(table["index_format"].value_or(0));
    view.texture          = &expected.texture;
    view.sampler          = &expected.sampler;
    NativeDrawReplayRecipe actual;
    require(BuildNativeMenuDrawRecipe(view, shaders, actual, error), root.string() + ": " + error);
    require(actual.vertexData == expected.vertexData && actual.indices == expected.indices,
            root.string() + " geometry matches independently prepared bytes");
    require(actual.vertexConstants == expected.vertexConstants && actual.pixelConstants == expected.pixelConstants &&
                actual.sharedConstants == expected.sharedConstants,
            root.string() + " constant bytes");
    require(actual.vertexShaderDxil == expected.vertexShaderDxil && actual.pixelShaderDxil == expected.pixelShaderDxil,
            root.string() + " shader bytes");
    require(actual.vertexStrideBytes == expected.vertexStrideBytes && actual.vertexAttributeCount == expected.vertexAttributeCount &&
                actual.sampleMask == expected.sampleMask && actual.textureMask == expected.textureMask,
            root.string() + " input layout and masks");
    require(actual.viewport.x == expected.viewport.x && actual.viewport.y == expected.viewport.y &&
                actual.viewport.width == expected.viewport.width && actual.viewport.height == expected.viewport.height &&
                actual.viewport.minDepth == expected.viewport.minDepth && actual.viewport.maxDepth == expected.viewport.maxDepth &&
                actual.scissor.left == expected.scissor.left && actual.scissor.top == expected.scissor.top &&
                actual.scissor.right == expected.scissor.right && actual.scissor.bottom == expected.scissor.bottom,
            root.string() + " viewport and scissor");
    require(actual.depth.enabled == expected.depth.enabled && actual.depth.writeEnabled == expected.depth.writeEnabled &&
                actual.depth.compare == expected.depth.compare && actual.rasterizer.cull == expected.rasterizer.cull &&
                actual.rasterizer.frontCounterClockwise == expected.rasterizer.frontCounterClockwise &&
                actual.blend.sourceColor == expected.blend.sourceColor && actual.blend.destinationColor == expected.blend.destinationColor &&
                actual.blend.sourceAlpha == expected.blend.sourceAlpha && actual.blend.destinationAlpha == expected.blend.destinationAlpha &&
                actual.blend.colorOp == expected.blend.colorOp && actual.blend.alphaOp == expected.blend.alphaOp,
            root.string() + " depth, rasterizer and blend state");
    require(actual.initialSample0.empty() && actual.initialSample1.empty() && actual.depth.initialSamples.empty(),
            "native clears have no captured attachments");
}

void frameFixture(const std::filesystem::path& root, const NativeMenuShaders& shaders)
{
    const auto  table   = toml::parse_file((root / "events.toml").string());
    const auto* records = table["events"].as_array();
    require(records && !records->empty() && records->size() <= 1024, "frame event metadata");

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
        if (event.primitiveType == 4)
        {
            std::string error;
            const auto  expectedPath = root.parent_path() / metadata["prepared"].value_or(std::string{}) / "frame.toml";
            require(LoadNativeDrawReplayRecipe(expectedPath, storage.expected, error), error);
            event.draw.texture = &storage.expected.texture;
            event.draw.sampler = &storage.expected.sampler;
        }
    }
    const auto              gamma = words(read(root.parent_path() / "gamma.bin"));
    NativeFrameReplayRecipe frame;
    std::string             error;
    require(BuildNativeMenuFrameRecipe(events, shaders, gamma, frame, error), error);
    require(frame.halves[0].size() == 77 && frame.halves[1].size() == 77, "saved full frame draw order");
    auto withStart = events;
    auto start     = events.back();
    start.id       = 1;
    for (auto& event : withStart)
        ++event.id;
    withStart.insert(withStart.begin(), start);
    require(BuildNativeMenuFrameRecipe(withStart, shaders, gamma, frame, error), "completed start swap boundary: " + error);
    auto damaged = events;
    damaged.erase(damaged.begin() + 48);
    require(!BuildNativeMenuFrameRecipe(damaged, shaders, gamma, frame, error), "missing initial clear rejected");
    damaged                        = events;
    damaged[0].hostPixelShaderHash = 1;
    require(!BuildNativeMenuFrameRecipe(damaged, shaders, gamma, frame, error), "image-producing point rejected");
    damaged              = events;
    damaged[129].resolve = false;
    require(!BuildNativeMenuFrameRecipe(damaged, shaders, gamma, frame, error), "foreign resolve rejected");
    damaged = events;
    damaged.back().frontbuffer += 4096;
    require(!BuildNativeMenuFrameRecipe(damaged, shaders, gamma, frame, error), "unrelated presentation buffer rejected");
    std::cout << "verified ordered frame and four admission failures\n";
}

} // namespace

int main(int argc, char** argv)
{
    require(argc == 1 || argc == 3, "optional arguments: fixture root, shader root");
    geometry();
    if (argc == 3)
    {
        NativeMenuShaders shaders;
        std::string       error;
        require(LoadNativeMenuShaders(argv[2], shaders, error), error);
        unsigned count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(argv[1]))
            if (entry.is_directory() && std::filesystem::exists(entry.path() / "view.toml"))
            {
                fixture(entry.path(), shaders);
                ++count;
            }
        require(count != 0, "fixture set must be nonempty");
        frameFixture(std::filesystem::path(argv[1]) / "events", shaders);
        std::cout << "verified " << count << " draw fixtures\n";
    }
    std::cout << "native_menu_draw_test: PASS\n";
}
