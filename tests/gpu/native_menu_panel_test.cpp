#include "gpu/d3d12/native_menu_panel.h"
#include "manual_cases.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace
{

using namespace rerevved::gpu;

constexpr std::uint32_t kSurface = 0x2000U;
constexpr std::uint32_t kColor   = 0x2001U;
constexpr std::uint32_t kFetch   = 0x48BEU;

std::vector<std::uint32_t> makeRegisters(std::uint32_t base = 0)
{
    std::vector<std::uint32_t> registers(kNativeMenuPanelRegisterCount, 0U);
    registers[kSurface]    = 0x0A010280U;
    registers[kColor]      = base & 0x7FFU;
    registers[0x2208U]     = 4U;
    registers[kFetch]      = 0x00004003U;
    registers[kFetch + 1U] = 0x10000062U;
    return registers;
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    UNSCOPED_INFO("fixture file open");
    REQUIRE(static_cast<bool>(file));
    const auto end = file.tellg();
    UNSCOPED_INFO("fixture file size");
    REQUIRE(end >= 0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty())
    {
        file.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    UNSCOPED_INFO("fixture file read");
    REQUIRE(static_cast<bool>(file));
    return bytes;
}

std::vector<std::uint32_t> decodeLittleDwords(const std::vector<std::uint8_t>& bytes)
{
    UNSCOPED_INFO("fixture dword alignment");
    REQUIRE((bytes.size() % 4U) == 0U);
    std::vector<std::uint32_t> dwords(bytes.size() / 4U);
    for (std::size_t index = 0; index < dwords.size(); ++index)
    {
        const auto offset = index * 4U;
        dwords[index]     = static_cast<std::uint32_t>(bytes[offset]) |
                            (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
                            (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
                            (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
    }
    return dwords;
}

void requireBytes(std::span<const std::uint8_t>    actual,
                  const std::vector<std::uint8_t>& expected,
                  const char*                      message)
{
    UNSCOPED_INFO(message);
    REQUIRE((actual.size() == expected.size() &&
             std::equal(actual.begin(), actual.end(), expected.begin())));
}

float getLittleFloat(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    const std::uint32_t bits  = static_cast<std::uint32_t>(bytes[offset]) |
                                (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
                                (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
                                (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
    float               value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

NativeMenuPanelView makeGeometryView(std::vector<std::uint32_t>& registers,
                                     std::vector<std::uint8_t>&  vertices,
                                     std::vector<std::uint8_t>&  indices)
{
    vertices.assign(96U, 0U);
    // Xenos k8in32 reverses the bytes in each dword before format unpacking.
    // The first attribute is k_16_16 and the second is normalized RGBA8.
    const std::array<std::array<std::uint8_t, 4>, 4> source = {
        std::array<std::uint8_t, 4>{ 0x00, 0x02, 0xFF, 0xFF },
        std::array<std::uint8_t, 4>{ 0x44, 0x33, 0x22, 0x11 },
        std::array<std::uint8_t, 4>{ 0xFF, 0xFF, 0x00, 0x01 },
        std::array<std::uint8_t, 4>{ 0x80, 0x70, 0x60, 0x50 },
    };
    for (std::size_t vertex = 0; vertex < 2U; ++vertex)
    {
        std::copy(source[vertex * 2U].begin(),
                  source[vertex * 2U].end(),
                  vertices.begin() + vertex * 8U);
        std::copy(source[vertex * 2U + 1U].begin(),
                  source[vertex * 2U + 1U].end(),
                  vertices.begin() + vertex * 8U + 4U);
    }
    constexpr std::array<std::uint16_t, 6> indexValues = { 0, 0, 0, 1, 1, 0 };
    indices.resize(indexValues.size() * 2U);
    for (std::size_t index = 0; index < indexValues.size(); ++index)
    {
        const auto value         = indexValues[index];
        indices[index * 2U]      = static_cast<std::uint8_t>(value >> 8U);
        indices[index * 2U + 1U] = static_cast<std::uint8_t>(value & 0xFFU);
    }

    NativeMenuPanelView view;
    view.registers       = registers;
    view.vertexBytes     = vertices;
    view.vertexGuestBase = 0x4000U;
    view.indexBytes      = indices;
    view.indexCount      = 6U;
    view.indexFormat     = 0U;
    view.indexEndian     = 1U;
    return view;
}

void testGeometryEndianAndBounds()
{
    auto                      registers = makeRegisters();
    std::vector<std::uint8_t> vertices;
    std::vector<std::uint8_t> indices;
    auto                      view = makeGeometryView(registers, vertices, indices);

    std::vector<std::uint8_t>  decodedVertices;
    std::vector<std::uint32_t> decodedIndices;
    std::string                error;
    UNSCOPED_INFO("synthetic geometry decode");
    REQUIRE(DecodeNativeMenuPanelGeometry(view, decodedVertices, decodedIndices, error));
    UNSCOPED_INFO("big endian uint16 indices");
    REQUIRE(decodedIndices == std::vector<std::uint32_t>{ 0, 0, 0, 1, 1, 0 });
    UNSCOPED_INFO("decoded float4 vertex size");
    REQUIRE(decodedVertices.size() == 64U);
    UNSCOPED_INFO("signed 16_16 components");
    REQUIRE((getLittleFloat(decodedVertices, 0U) == -1.0F &&
             getLittleFloat(decodedVertices, 4U) == 2.0F));
    UNSCOPED_INFO("RGBA8 normalized components");
    REQUIRE((getLittleFloat(decodedVertices, 16U) == 0x11 / 255.0F &&
             getLittleFloat(decodedVertices, 20U) == 0x22 / 255.0F &&
             getLittleFloat(decodedVertices, 24U) == 0x33 / 255.0F &&
             getLittleFloat(decodedVertices, 28U) == 0x44 / 255.0F));
    UNSCOPED_INFO("second signed vertex");
    REQUIRE((getLittleFloat(decodedVertices, 32U) == 1.0F &&
             getLittleFloat(decodedVertices, 36U) == -1.0F));

    view.indexEndian = 2U;
    UNSCOPED_INFO("foreign index endian rejection");
    REQUIRE(!DecodeNativeMenuPanelGeometry(view, decodedVertices, decodedIndices, error));
    view.indexEndian     = 1U;
    view.vertexGuestBase = 0x4004U;
    UNSCOPED_INFO("guest base mismatch rejection");
    REQUIRE(!DecodeNativeMenuPanelGeometry(view, decodedVertices, decodedIndices, error));
    view.vertexGuestBase = 0x4000U;
    indices[0]           = 0;
    indices[1]           = 100;
    UNSCOPED_INFO("vertex range rejection");
    REQUIRE(!DecodeNativeMenuPanelGeometry(view, decodedVertices, decodedIndices, error));
}

std::size_t edramOffset(std::uint32_t base, std::uint32_t x, std::uint32_t y, std::uint32_t sample)
{
    const auto physicalY = y * 2U + sample;
    const auto tile      = (base + (physicalY / 16U) * 8U + x / 80U) % 2048U;
    return static_cast<std::size_t>(tile) * 80U * 16U * 4U +
           static_cast<std::size_t>(physicalY % 16U) * 80U * 4U +
           static_cast<std::size_t>(x % 80U) * 4U;
}

void putEdramPixel(std::vector<std::uint8_t>&  edram,
                   std::uint32_t               base,
                   std::uint32_t               x,
                   std::uint32_t               y,
                   std::uint32_t               sample,
                   std::array<std::uint8_t, 4> value)
{
    const auto offset = edramOffset(base, x, y, sample);
    std::copy(value.begin(), value.end(), edram.begin() + offset);
}

void testEdramMappingAndAdmission()
{
    constexpr std::uint32_t   base      = 17U;
    auto                      registers = makeRegisters(base);
    std::vector<std::uint8_t> edram(10U * 1024U * 1024U, 0U);
    putEdramPixel(edram, base, 0U, 0U, 0U, { 1, 2, 3, 4 });
    putEdramPixel(edram, base, 0U, 0U, 1U, { 5, 6, 7, 8 });
    putEdramPixel(edram, base, 80U, 8U, 0U, { 9, 10, 11, 12 });
    putEdramPixel(edram, base, 80U, 8U, 1U, { 13, 14, 15, 16 });

    std::vector<std::uint8_t> combined;
    std::string               error;
    UNSCOPED_INFO(std::string("synthetic EDRAM decode") + (error));
    REQUIRE(DecodeNativeMenuPanelColor(edram, registers, combined, error));
    UNSCOPED_INFO("combined sample size");
    REQUIRE(combined.size() == 2U * 640U * 720U * 4U);
    UNSCOPED_INFO("sample zero tile mapping");
    REQUIRE(std::equal(combined.begin(), combined.begin() + 4U, std::array<std::uint8_t, 4>{ 1, 2, 3, 4 }.begin()));
    const auto sampleOne = 640U * 720U * 4U;
    UNSCOPED_INFO("sample one y mapping");
    REQUIRE(std::equal(combined.begin() + sampleOne,
                       combined.begin() + sampleOne + 4U,
                       std::array<std::uint8_t, 4>{ 5, 6, 7, 8 }.begin()));
    const auto second = (8U * 640U + 80U) * 4U;
    UNSCOPED_INFO("tile row and column mapping");
    REQUIRE(std::equal(combined.begin() + second,
                       combined.begin() + second + 4U,
                       std::array<std::uint8_t, 4>{ 9, 10, 11, 12 }.begin()));
    UNSCOPED_INFO("sample one tile row mapping");
    REQUIRE(std::equal(combined.begin() + sampleOne + second,
                       combined.begin() + sampleOne + second + 4U,
                       std::array<std::uint8_t, 4>{ 13, 14, 15, 16 }.begin()));

    registers[kSurface] ^= 1U;
    UNSCOPED_INFO("foreign EDRAM surface rejection");
    REQUIRE(!DecodeNativeMenuPanelColor(edram, registers, combined, error));
    registers[kSurface] ^= 1U;
    edram.resize(10U * 1024U * 1024U + 1U);
    UNSCOPED_INFO("EDRAM upper bound rejection");
    REQUIRE(!DecodeNativeMenuPanelColor(edram, registers, combined, error));
    edram.resize(10U * 1024U * 1024U);
    registers.resize(kNativeMenuPanelRegisterCount - 1U);
    UNSCOPED_INFO("register file bound rejection");
    REQUIRE(!DecodeNativeMenuPanelColor(edram, registers, combined, error));
}

void testShaderAdmission()
{
    std::string error;
    UNSCOPED_INFO("empty shader rejection");
    REQUIRE(!ValidateNativeMenuPanelShaders({}, {}, error));
}

void testCapturedFixture(const std::filesystem::path& captureRoot,
                         const std::filesystem::path& shaderRoot,
                         const std::filesystem::path& preparedRoot)
{
    const auto registerBytes = readFile(captureRoot / "registers.bin");
    auto       registers     = decodeLittleDwords(registerBytes);
    const auto vertexUcode   = decodeLittleDwords(readFile(captureRoot / "vertex.ucode.bin"));
    const auto pixelUcode    = decodeLittleDwords(readFile(captureRoot / "pixel.ucode.bin"));
    auto       vertexBytes   = readFile(captureRoot / "vertex_fetch_95.bin");
    auto       indexBytes    = readFile(captureRoot / "index.bin");
    auto       vertexDxil    = readFile(shaderRoot / "vs.dxil");
    auto       pixelDxil     = readFile(shaderRoot / "ps.dxil");
    auto       edramBefore   = readFile(captureRoot / "edram_before.bin");

    NativeMenuPanelView view;
    view.registers         = registers;
    view.vertexUcodeDwords = vertexUcode;
    view.pixelUcodeDwords  = pixelUcode;
    view.vertexBytes       = vertexBytes;
    view.vertexGuestBase =
        static_cast<std::uint64_t>(registers[kFetch] >> 2U) * 4ULL;
    view.indexBytes       = indexBytes;
    view.indexCount       = static_cast<std::uint32_t>(indexBytes.size() / 2U);
    view.indexFormat      = 0U;
    view.indexEndian      = 1U;
    view.vertexShaderHash = 0x11213E38D7154104ULL;
    view.pixelShaderHash  = 0x3A92D78FE55C7B83ULL;

    NativeDrawReplayRecipe recipe;
    std::string            error;
    UNSCOPED_INFO(std::string("captured menu panel recipe admission") + (error));
    REQUIRE(BuildNativeMenuPanelRecipe(view,
                                       vertexDxil,
                                       pixelDxil,
                                       edramBefore,
                                       recipe,
                                       error));
    requireBytes(recipe.vertexShaderDxil, vertexDxil, "captured vertex DXIL copy");
    requireBytes(recipe.pixelShaderDxil, pixelDxil, "captured pixel DXIL copy");
    requireBytes(recipe.vertexData,
                 readFile(preparedRoot / "vertices.bin"),
                 "captured geometry conversion");
    const auto preparedIndices = decodeLittleDwords(readFile(preparedRoot / "indices.bin"));
    UNSCOPED_INFO("captured index conversion");
    REQUIRE(recipe.indices == preparedIndices);
    requireBytes(recipe.vertexConstants,
                 readFile(preparedRoot / "vs-constants.bin"),
                 "captured VS constants");
    requireBytes(recipe.pixelConstants,
                 readFile(preparedRoot / "ps-constants.bin"),
                 "captured PS constants");
    requireBytes(recipe.sharedConstants,
                 readFile(preparedRoot / "shared.bin"),
                 "captured shared constants");
    const auto preparedSamples = readFile(preparedRoot / "before.rgba");
    UNSCOPED_INFO("captured sample plane size");
    REQUIRE(preparedSamples.size() == recipe.initialSample0.size() * 2U);
    requireBytes(recipe.initialSample0,
                 std::vector<std::uint8_t>(preparedSamples.begin(),
                                           preparedSamples.begin() + recipe.initialSample0.size()),
                 "captured sample zero");
    requireBytes(recipe.initialSample1,
                 std::vector<std::uint8_t>(preparedSamples.begin() + recipe.initialSample0.size(),
                                           preparedSamples.end()),
                 "captured sample one");

    auto rightRegisters     = registers;
    rightRegisters[0x2080U] = 0x00007D80U; // signed window X offset -640
    rightRegisters[0x2081U] = 0x00000280U;
    rightRegisters[0x2082U] = 0x02D00500U;
    view.registers          = rightRegisters;
    NativeDrawReplayRecipe rightRecipe;
    UNSCOPED_INFO(std::string("captured right-half menu panel recipe admission") + (error));
    REQUIRE(BuildNativeMenuPanelRecipe(view,
                                       vertexDxil,
                                       pixelDxil,
                                       edramBefore,
                                       rightRecipe,
                                       error));
    UNSCOPED_INFO("captured signed window offset derivation");
    REQUIRE((rightRecipe.viewport.x == -640.0F && rightRecipe.viewport.y == 0.0F &&
             rightRecipe.scissor.left == 0 && rightRecipe.scissor.right == 640));
}

} // namespace

TEST_CASE("native_menu_panel", "[ctest][native-menu-panel]")
{
    testGeometryEndianAndBounds();
    testEdramMappingAndAdmission();
    testShaderAdmission();
}

// Replays a saved menu panel capture: native-menu-panel.fixture <capture> <shader> <prepared>.
TEST_CASE("native_menu_panel_fixture", "[.][manual][native-menu-panel]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() == 0)
    {
        SKIP("pass the capture, shader and prepared roots through --helper native-menu-panel.fixture");
    }
    UNSCOPED_INFO("optional fixture arguments are capture, shader, and prepared roots");
    REQUIRE(args.argc() == 3);
    testCapturedFixture(args.argv()[1], args.argv()[2], args.argv()[3]);
}

REREVVED_MANUAL_HELPER(nativeMenuPanelFixture, "native-menu-panel.fixture", "native_menu_panel_fixture")
