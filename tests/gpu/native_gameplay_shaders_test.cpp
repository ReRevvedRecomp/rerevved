#include "gpu/d3d12/native_gameplay_shaders.h"
#include "manual_cases.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>
#include <rex/crypto/sha256.h>

using namespace rerevved::gpu;

namespace
{

void write(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary);
    file << text;
    UNSCOPED_INFO("could not write test fixture");
    REQUIRE(bool(file));
}

std::string manifest(const std::string& digest, const std::string& specialization = {})
{
    return "schema_version = 1\n[[shaders]]\n"
           "vertex_hash = '0000000000000001'\npixel_hash = '0000000000000002'\n"
           "vertex_file = 'shader.dxil'\npixel_file = 'shader.dxil'\n"
           "vertex_word_sha256 = '" +
           digest + "'\npixel_word_sha256 = '" + digest + "'\n"
                                                          "vertex_dxil_sha256 = '" +
           digest + "'\npixel_dxil_sha256 = '" + digest + "'\n"
                                                          "alpha_test = true\nalpha_compare = 4\nrasterized_depth = true\n"
                                                          "independent_samplers = true\n" +
           specialization +
           "program = 1\ncontext = 0\ninterpolators = 1\n"
           "texture_fetches = [16, 3]\ntexture_dimensions = ['2d', 'cube']\n"
           "[[shaders.attributes]]\nfetch = 95\nstride_bytes = 12\noffset_bytes = 0\n"
           "format = 57\nsigned = true\nnormalized = false\n";
}

std::string artifact(const std::string& table, const std::string& file, const std::string& digest)
{
    return "[" + table + "]\nfile = '" + file + "'\nsha256 = '" + digest + "'\n";
}

// Schema 2 keeps the schema 1 identity and specialization keys; each stage
// and format artifact is a sub-table appended after them.
std::string manifest2(const std::string& wordDigest, const std::string& artifacts, const std::string& identity = {})
{
    return "schema_version = 2\n[[shaders]]\n" + identity +
           "vertex_hash = '0000000000000001'\npixel_hash = '0000000000000002'\n"
           "vertex_word_sha256 = '" +
           wordDigest + "'\npixel_word_sha256 = '" + wordDigest +
           "'\nalpha_test = true\nalpha_compare = 4\nrasterized_depth = true\n"
           "independent_samplers = true\nprogram = 1\ncontext = 0\ninterpolators = 1\n"
           "texture_fetches = [16, 3]\ntexture_dimensions = ['2d', 'cube']\n" +
           artifacts +
           "[[shaders.attributes]]\nfetch = 95\nstride_bytes = 12\noffset_bytes = 0\n"
           "format = 57\nsigned = true\nnormalized = false\n";
}

std::string dualArtifacts(const std::string& dxil, const std::string& spirv, const std::string& spirvFile = "shader.spv")
{
    return artifact("shaders.vertex_dxil", "shader.dxil", dxil) + artifact("shaders.pixel_dxil", "shader.dxil", dxil) +
           artifact("shaders.vertex_spirv", spirvFile, spirv) + artifact("shaders.pixel_spirv", spirvFile, spirv);
}

void depthOnlyDual(const std::filesystem::path& root, const std::string& dxil, const std::string& spirv);

// Schema 2 fills only requested format fields; schema 1 is DXIL-only.
void dualFormat(const std::filesystem::path& parent, const std::string& dxilBytes, const std::string& spirvBytes)
{
    // The catalog is a child directory so that an escaping path names a real
    // file beside it.
    const auto root  = parent / "dual";
    const auto dxil  = rex::crypto::sha256(dxilBytes);
    const auto spirv = rex::crypto::sha256(spirvBytes);
    UNSCOPED_INFO("dual-format fixture directory must be fresh");
    REQUIRE(std::filesystem::create_directory(root));
    write(root / "shader.dxil", dxilBytes);
    write(root / "shader.spv", spirvBytes);
    write(parent / "shader.spv", spirvBytes);
    std::vector<NativeGameplayShaderInterface> shaders;
    std::string                                error;
    write(root / "shaders.toml", manifest(dxil));
    UNSCOPED_INFO("a schema 1 DXIL load fills no SPIR-V fields: " + error);
    REQUIRE((LoadNativeGameplayShaders(root, shaders, error) && shaders[0].vertexShaderSpirv.empty() && shaders[0].pixelSpirvDigest.empty()));
    UNSCOPED_INFO("a schema 1 catalog has no SPIR-V artifacts: " + error);
    REQUIRE((!LoadNativeGameplayShaders(root, shaders, error, NativeShaderFormat::Spirv) && shaders.empty() &&
             error == "shader_format_missing: 0000000000000001-0000000000000002"));

    write(root / "shaders.toml", manifest2(dxil, dualArtifacts(dxil, spirv)));
    UNSCOPED_INFO("a schema 2 DXIL load fills only the DXIL fields: " + error);
    REQUIRE((LoadNativeGameplayShaders(root, shaders, error) && shaders.size() == 1 && shaders[0].vertexDxilDigest == dxil &&
             shaders[0].pixelDxilDigest == dxil && !shaders[0].vertexShaderDxil.empty() && !shaders[0].pixelShaderDxil.empty() &&
             shaders[0].vertexShaderSpirv.empty() && shaders[0].pixelShaderSpirv.empty() && shaders[0].vertexSpirvDigest.empty() &&
             shaders[0].pixelSpirvDigest.empty() && shaders[0].textureFetchSlots == std::vector<std::uint32_t>{ 16, 3 } &&
             shaders[0].vertexAttributes.size() == 1));
    UNSCOPED_INFO("a schema 2 SPIR-V load fills only the SPIR-V fields: " + error);
    REQUIRE((LoadNativeGameplayShaders(root, shaders, error, NativeShaderFormat::Spirv) && shaders.size() == 1 &&
             shaders[0].vertexSpirvDigest == spirv && shaders[0].pixelSpirvDigest == spirv && !shaders[0].vertexShaderSpirv.empty() &&
             !shaders[0].pixelShaderSpirv.empty() && shaders[0].vertexShaderDxil.empty() && shaders[0].pixelShaderDxil.empty() &&
             shaders[0].vertexDxilDigest.empty() && shaders[0].pixelDxilDigest.empty() && shaders[0].alphaTestEnabled &&
             shaders[0].vertexAttributes.size() == 1));

    const auto refused = [&](const std::string& name, const std::string& text, NativeShaderFormat format, const std::string& expected = {})
    {
        write(root / "shaders.toml", text);
        UNSCOPED_INFO(name + " refuses the catalog: " + error);
        REQUIRE((!LoadNativeGameplayShaders(root, shaders, error, format) && shaders.empty() && !error.empty() &&
                 (expected.empty() || error == expected)));
    };
    const auto dxilOnly = artifact("shaders.vertex_dxil", "shader.dxil", dxil) + artifact("shaders.pixel_dxil", "shader.dxil", dxil);
    write(root / "shaders.toml", manifest2(dxil, dxilOnly));
    UNSCOPED_INFO("a DXIL-only schema 2 entry loads for DXIL: " + error);
    REQUIRE((LoadNativeGameplayShaders(root, shaders, error) && shaders.size() == 1));
    refused("a DXIL-only schema 2 entry", manifest2(dxil, dxilOnly), NativeShaderFormat::Spirv, "shader_format_missing: 0000000000000001-0000000000000002");
    refused("an entry without a pixel SPIR-V artifact",
            manifest2(dxil, dxilOnly + artifact("shaders.vertex_spirv", "shader.spv", spirv), "id = 'named-entry'\n"),
            NativeShaderFormat::Spirv,
            "shader_format_missing: named-entry");
    refused("an empty SPIR-V table", manifest2(dxil, dxilOnly + "[shaders.vertex_spirv]\n" + artifact("shaders.pixel_spirv", "shader.spv", spirv)), NativeShaderFormat::Spirv, "shader_format_missing: 0000000000000001-0000000000000002");
    refused("a SPIR-V-only entry", manifest2(dxil, artifact("shaders.vertex_spirv", "shader.spv", spirv) + artifact("shaders.pixel_spirv", "shader.spv", spirv)), NativeShaderFormat::Dxil, "shader_format_missing: 0000000000000001-0000000000000002");
    refused("a SPIR-V digest the file does not have", manifest2(dxil, dualArtifacts(dxil, std::string(64, 'a'))), NativeShaderFormat::Spirv, "gameplay SPIR-V does not match its interface digest");
    refused("an absolute SPIR-V path", manifest2(dxil, dualArtifacts(dxil, spirv, (root / "shader.spv").generic_string())), NativeShaderFormat::Spirv, "gameplay shader path must be relative");
    refused("a SPIR-V path outside the directory", manifest2(dxil, dualArtifacts(dxil, spirv, "../shader.spv")), NativeShaderFormat::Spirv, "gameplay shader path escapes its directory");
    refused("a SPIR-V table without a digest", manifest2(dxil, dxilOnly + "[shaders.vertex_spirv]\nfile = 'shader.spv'\n" + artifact("shaders.pixel_spirv", "shader.spv", spirv)), NativeShaderFormat::Spirv);
    refused("schema 1 artifact keys in schema 2", manifest2(dxil, dualArtifacts(dxil, spirv), "vertex_file = 'shader.dxil'\n"), NativeShaderFormat::Dxil, "gameplay shader schema 2 entry carries schema 1 artifact keys");
    auto schema3 = manifest2(dxil, dualArtifacts(dxil, spirv));
    schema3.replace(0, std::string("schema_version = 2").size(), "schema_version = 3");
    refused("schema 3", schema3, NativeShaderFormat::Dxil);
    depthOnlyDual(root, dxil, spirv);
    UNSCOPED_INFO("dual-format fixture cleanup");
    REQUIRE((std::filesystem::remove(root / "shaders.toml") && std::filesystem::remove(root / "shader.dxil") &&
             std::filesystem::remove(root / "shader.spv") && std::filesystem::remove(root) && std::filesystem::remove(parent / "shader.spv")));
    std::cout << "Dual-format shader catalog: schema 1 is DXIL-only; schema 2 loads either format; missing, digest, path and schema forms are refused\n";
}

std::string depthOnlyList(const std::string& digest, const std::string& state = "mode_control = 5\ncolor_mask = 0\n")
{
    return "schema_version = 1\n[[depth_only]]\nid = 'depth-0000000000000003'\ncaller = '0x8250C768'\n"
           "vertex_hash = '0000000000000003'\nvertex_word_sha256 = '" +
           digest + "'\nvertex_dxil_sha256 = '" + digest +
           "'\nvertex_file = 'shader.dxil'\n"
           "program = 65539\ncontext = 0\ninterpolators = 65539\n" +
           state;
}

// Depth-only lists have vertex identity only; malformed forms refuse.
void depthOnly(const std::filesystem::path& root, const std::string& digest)
{
    std::vector<NativeGameplayShaderInterface> shaders;
    std::string                                error;
    UNSCOPED_INFO("a directory without depth-only.toml has no depth-only entries: " + error);
    REQUIRE((LoadNativeDepthOnlyShaders(root, shaders, error) && shaders.empty() && error.empty()));
    write(root / "depth-only.toml", depthOnlyList(digest));
    UNSCOPED_INFO("the depth-only list loads: " + error);
    REQUIRE((LoadNativeDepthOnlyShaders(root, shaders, error) && shaders.size() == 1));
    const auto& shader = shaders[0];
    UNSCOPED_INFO("a depth-only entry has a vertex identity, its SQ words and no pixel stage");
    REQUIRE((shader.depthOnly && shader.vertexShaderHash == 3 && !shader.pixelShaderHash && shader.pixelShaderDxil.empty() && shader.pixelGuestWordDigest.empty() &&
             shader.pixelDxilDigest.empty() && shader.vertexGuestWordDigest == digest && !shader.vertexShaderDxil.empty() && shader.vertexAttributes.empty() &&
             shader.textureFetchSlots.empty() && !shader.alphaTestEnabled && shader.rasterizedDepth && shader.independentSamplers &&
             shader.shaderProgram == 0x10003 && shader.shaderContext == 0 && shader.shaderInterpolators == 0x10003));
    const auto refused = [&](const std::string& name, const std::string& list)
    {
        write(root / "depth-only.toml", list);
        UNSCOPED_INFO(name + " invalidates the depth-only list");
        REQUIRE((!LoadNativeDepthOnlyShaders(root, shaders, error) && shaders.empty() && !error.empty()));
    };
    refused("RB_MODECONTROL 4", depthOnlyList(digest, "mode_control = 4\ncolor_mask = 0\n"));
    refused("a color mask", depthOnlyList(digest, "mode_control = 5\ncolor_mask = 15\n"));
    refused("a missing color mask", depthOnlyList(digest, "mode_control = 5\n"));
    refused("a DXIL digest the file does not have", depthOnlyList(std::string(64, 'a')));
    const auto list = depthOnlyList(digest);
    refused("a duplicate entry", list + list.substr(list.find("[[depth_only]]")));
    auto generated = list;
    generated.replace(generated.find("program = 65539"), std::string("program = 65539").size(), "program = 327683");
    refused("generated pixel parameters", generated);
    auto noId = list;
    noId.erase(noId.find("id = "), std::string("id = 'depth-0000000000000003'\n").size());
    refused("an entry without an id", noId);
    refused("schema 3", "schema_version = 3" + list.substr(list.find('\n')));
    UNSCOPED_INFO("depth-only fixture cleanup");
    REQUIRE(std::filesystem::remove(root / "depth-only.toml"));
    std::cout << "Depth-only shader list: vertex-only entries load; mode, mask, digest, duplicate, param_gen, id and schema forms are refused\n";
}

std::string depthOnlyList2(const std::string& wordDigest, const std::string& artifacts, const std::string& extra = {})
{
    return "schema_version = 2\n[[depth_only]]\nid = 'depth-0000000000000003'\ncaller = '0x8250C768'\n"
           "vertex_hash = '0000000000000003'\nvertex_word_sha256 = '" +
           wordDigest + "'\nprogram = 65539\ncontext = 0\ninterpolators = 65539\nmode_control = 5\ncolor_mask = 0\n" + extra + artifacts;
}

// Depth-only lists follow catalog format rules with vertex artifacts only.
void depthOnlyDual(const std::filesystem::path& root, const std::string& dxil, const std::string& spirv)
{
    std::vector<NativeGameplayShaderInterface> shaders;
    std::string                                error;
    write(root / "depth-only.toml", depthOnlyList(dxil));
    UNSCOPED_INFO("a schema 1 depth-only DXIL load fills no SPIR-V fields: " + error);
    REQUIRE((LoadNativeDepthOnlyShaders(root, shaders, error) && shaders.size() == 1 && shaders[0].vertexSpirvDigest.empty() &&
             shaders[0].vertexShaderSpirv.empty()));
    UNSCOPED_INFO("a schema 1 depth-only list has no SPIR-V artifacts: " + error);
    REQUIRE((!LoadNativeDepthOnlyShaders(root, shaders, error, NativeShaderFormat::Spirv) && shaders.empty() &&
             error == "shader_format_missing: depth-0000000000000003"));
    const auto vertexDxil  = artifact("depth_only.vertex_dxil", "shader.dxil", dxil);
    const auto vertexSpirv = artifact("depth_only.vertex_spirv", "shader.spv", spirv);
    write(root / "depth-only.toml", depthOnlyList2(dxil, vertexDxil + vertexSpirv));
    UNSCOPED_INFO("a schema 2 depth-only DXIL load fills only the vertex DXIL fields: " + error);
    REQUIRE((LoadNativeDepthOnlyShaders(root, shaders, error) && shaders.size() == 1 && shaders[0].depthOnly && shaders[0].vertexDxilDigest == dxil &&
             !shaders[0].vertexShaderDxil.empty() && shaders[0].vertexShaderSpirv.empty() && shaders[0].vertexSpirvDigest.empty() &&
             shaders[0].pixelShaderDxil.empty() && shaders[0].pixelShaderSpirv.empty()));
    UNSCOPED_INFO("a schema 2 depth-only SPIR-V load fills only the vertex SPIR-V fields: " + error);
    REQUIRE((LoadNativeDepthOnlyShaders(root, shaders, error, NativeShaderFormat::Spirv) && shaders.size() == 1 && shaders[0].depthOnly &&
             shaders[0].vertexSpirvDigest == spirv && !shaders[0].vertexShaderSpirv.empty() && shaders[0].vertexShaderDxil.empty() &&
             shaders[0].vertexDxilDigest.empty() && shaders[0].pixelShaderSpirv.empty() && shaders[0].pixelSpirvDigest.empty() &&
             shaders[0].shaderProgram == 0x10003));
    const auto refused = [&](const std::string& name, const std::string& list, NativeShaderFormat format, const std::string& expected = {})
    {
        write(root / "depth-only.toml", list);
        UNSCOPED_INFO(name + " refuses the depth-only list: " + error);
        REQUIRE((!LoadNativeDepthOnlyShaders(root, shaders, error, format) && shaders.empty() && !error.empty() &&
                 (expected.empty() || error == expected)));
    };
    refused("a DXIL-only depth-only entry", depthOnlyList2(dxil, vertexDxil), NativeShaderFormat::Spirv, "shader_format_missing: depth-0000000000000003");
    refused("a SPIR-V-only depth-only entry", depthOnlyList2(dxil, vertexSpirv), NativeShaderFormat::Dxil, "shader_format_missing: depth-0000000000000003");
    refused("an empty depth-only SPIR-V table", depthOnlyList2(dxil, vertexDxil + "[depth_only.vertex_spirv]\n"), NativeShaderFormat::Spirv, "shader_format_missing: depth-0000000000000003");
    refused("a depth-only SPIR-V digest the file does not have",
            depthOnlyList2(dxil, vertexDxil + artifact("depth_only.vertex_spirv", "shader.spv", std::string(64, 'a'))),
            NativeShaderFormat::Spirv,
            "gameplay SPIR-V does not match its interface digest");
    refused("an absolute depth-only SPIR-V path",
            depthOnlyList2(dxil, vertexDxil + artifact("depth_only.vertex_spirv", (root / "shader.spv").generic_string(), spirv)),
            NativeShaderFormat::Spirv,
            "gameplay shader path must be relative");
    refused("a depth-only SPIR-V path outside the directory",
            depthOnlyList2(dxil, vertexDxil + artifact("depth_only.vertex_spirv", "../shader.spv", spirv)),
            NativeShaderFormat::Spirv,
            "gameplay shader path escapes its directory");
    refused("a depth-only pixel artifact",
            depthOnlyList2(dxil, vertexDxil + vertexSpirv + artifact("depth_only.pixel_spirv", "shader.spv", spirv)),
            NativeShaderFormat::Dxil,
            "depth-only shader entry carries a pixel artifact");
    for (const std::string legacyPixel : { std::string("pixel_file = 'shader.dxil'\n"), "pixel_dxil_sha256 = '" + dxil + "'\n" })
        refused("a schema 1 pixel artifact key in a schema 2 depth-only list", depthOnlyList2(dxil, vertexDxil + vertexSpirv, legacyPixel), NativeShaderFormat::Dxil, "depth-only shader entry carries a pixel artifact");
    refused("schema 1 artifact keys in a schema 2 depth-only list", depthOnlyList2(dxil, vertexDxil + vertexSpirv, "vertex_dxil_sha256 = '" + dxil + "'\n"), NativeShaderFormat::Dxil, "gameplay shader schema 2 entry carries schema 1 artifact keys");
    auto schema3 = depthOnlyList2(dxil, vertexDxil + vertexSpirv);
    schema3.replace(0, std::string("schema_version = 2").size(), "schema_version = 3");
    refused("a schema 3 depth-only list", schema3, NativeShaderFormat::Dxil);
    UNSCOPED_INFO("dual-format depth-only fixture cleanup");
    REQUIRE(std::filesystem::remove(root / "depth-only.toml"));
    std::cout << "Dual-format depth-only list: schema 1 is DXIL-only; schema 2 loads either vertex format; missing, digest, path, pixel and schema forms are refused\n";
}

} // namespace

TEST_CASE("native_gameplay_shaders", "[ctest][native-gameplay-shaders]")
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("rerevved-gameplay-shaders-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    UNSCOPED_INFO("test directory must be fresh");
    REQUIRE(std::filesystem::create_directory(root));
    const std::string shaderBytes = "synthetic catalog payload";
    const auto        text        = manifest(rex::crypto::sha256(shaderBytes));
    write(root / "shader.dxil", shaderBytes);
    write(root / "shaders.toml", text);
    std::vector<NativeGameplayShaderInterface> shaders;
    std::string                                error;
    UNSCOPED_INFO(error);
    REQUIRE(LoadNativeGameplayShaders(root, shaders, error));
    UNSCOPED_INFO("catalog must preserve specialization and packed input ordering");
    REQUIRE((shaders.size() == 1 && shaders[0].shaderContext == 0 &&
             shaders[0].independentSamplers &&
             shaders[0].textureFetchSlots == std::vector<std::uint32_t>{ 16, 3 } &&
             shaders[0].textureDimensions[1] == NativeDrawReplayTextureDimension::TextureCube &&
             shaders[0].vertexAttributes[0].strideBytes == 12 &&
             shaders[0].colorTargetFormat == 0 && shaders[0].colorExpBias == 0 &&
             shaders[0].sampleCount == 4 && shaders[0].sampleMask == 9));
    const std::string targetSpecialization =
        "color_format = 4\ncolor_exp_bias = 0\nsample_count = 1\nsample_mask = 1\n";
    write(root / "shaders.toml", manifest(rex::crypto::sha256(shaderBytes), targetSpecialization));
    UNSCOPED_INFO("catalog must load the explicit single-sample target specialization");
    REQUIRE((LoadNativeGameplayShaders(root, shaders, error) && shaders.size() == 1 &&
             shaders[0].colorTargetFormat == 4 && shaders[0].colorExpBias == 0 &&
             shaders[0].sampleCount == 1 && shaders[0].sampleMask == 1));
    write(root / "shaders.toml", manifest(rex::crypto::sha256(shaderBytes), "color_format = 2\ncolor_exp_bias = 0\nsample_count = 4\nsample_mask = 9\n"));
    UNSCOPED_INFO("catalog must preserve the RGB10A2 target specialization and sample mapping");
    REQUIRE((LoadNativeGameplayShaders(root, shaders, error) && shaders[0].colorTargetFormat == 2 &&
             shaders[0].colorExpBias == 0 && shaders[0].sampleCount == 4 && shaders[0].sampleMask == 9));
    auto generated = text;
    generated.replace(generated.find("program = 1\n"), std::string("program = 1\n").size(), "program = 262145\n");
    write(root / "shaders.toml", generated);
    UNSCOPED_INFO("a catalog without generated pixel parameter specialization must reject param_gen draws");
    REQUIRE((!LoadNativeGameplayShaders(root, shaders, error) && error.find("generated pixel parameter") != std::string::npos));
    generated.insert(generated.find("program = "), "generated_pixel_parameters = true\n");
    write(root / "shaders.toml", generated);
    UNSCOPED_INFO("a catalog must preserve the generated pixel parameter specialization");
    REQUIRE((LoadNativeGameplayShaders(root, shaders, error) && shaders[0].generatedPixelParameters));
    UNSCOPED_INFO("an entry without screen_space_position expects the guest viewport transform");
    REQUIRE((LoadNativeGameplayShaders(root, shaders, error) && !shaders[0].screenSpacePosition));
    for (const auto& [value, expected] : { std::pair<std::string, bool>{ "true", true }, { "false", false } })
    {
        auto screenSpace = text;
        screenSpace.insert(screenSpace.find("program = "), "screen_space_position = " + value + "\n");
        write(root / "shaders.toml", screenSpace);
        UNSCOPED_INFO("a catalog must preserve screen_space_position = " + value);
        REQUIRE((LoadNativeGameplayShaders(root, shaders, error) && shaders[0].screenSpacePosition == expected));
    }
    {
        // The flag selects a different translated VS, so it is part of the
        // entry identity; a non-boolean value invalidates the catalog.
        auto pair = text, flagged = text;
        flagged.insert(flagged.find("program = "), "screen_space_position = true\n");
        pair.append(flagged.substr(flagged.find("[[shaders]]")));
        write(root / "shaders.toml", pair);
        UNSCOPED_INFO("entries that differ only in screen_space_position are distinct specializations: " + error);
        REQUIRE((LoadNativeGameplayShaders(root, shaders, error) && shaders.size() == 2 && !shaders[0].screenSpacePosition &&
                 shaders[1].screenSpacePosition));
        auto invalid = text;
        invalid.insert(invalid.find("program = "), "screen_space_position = 'true'\n");
        write(root / "shaders.toml", invalid);
        UNSCOPED_INFO("a non-boolean screen_space_position invalidates the catalog");
        REQUIRE((!LoadNativeGameplayShaders(root, shaders, error) && shaders.empty()));
    }
    {
        // user_clip_plane selects the VS that writes SV_ClipDistance0; it
        // is part of the identity and excludes a screen-space VS.
        write(root / "shaders.toml", text);
        UNSCOPED_INFO("an entry without user_clip_plane clips by no user plane");
        REQUIRE((LoadNativeGameplayShaders(root, shaders, error) && !shaders[0].userClipPlane));
        auto pair = text, flagged = text;
        flagged.insert(flagged.find("program = "), "user_clip_plane = true\n");
        pair.append(flagged.substr(flagged.find("[[shaders]]")));
        write(root / "shaders.toml", pair);
        UNSCOPED_INFO("entries that differ only in user_clip_plane are distinct specializations: " + error);
        REQUIRE((LoadNativeGameplayShaders(root, shaders, error) && shaders.size() == 2 && !shaders[0].userClipPlane && shaders[1].userClipPlane));
        auto both = flagged;
        both.insert(both.find("program = "), "screen_space_position = true\n");
        write(root / "shaders.toml", both);
        UNSCOPED_INFO("a screen-space entry cannot clip by a user plane");
        REQUIRE((!LoadNativeGameplayShaders(root, shaders, error) && error == "gameplay shader user clip plane requires guest clipping"));
        auto invalid = text;
        invalid.insert(invalid.find("program = "), "user_clip_plane = 'true'\n");
        write(root / "shaders.toml", invalid);
        UNSCOPED_INFO("a non-boolean user_clip_plane invalidates the catalog");
        REQUIRE((!LoadNativeGameplayShaders(root, shaders, error) && shaders.empty()));
    }
    for (const std::string invalidSpecialization : {
             "color_format = 7\ncolor_exp_bias = 0\nsample_count = 1\nsample_mask = 1\n",
             "color_format = 4\ncolor_exp_bias = 32\nsample_count = 1\nsample_mask = 1\n",
             "color_format = 2\ncolor_exp_bias = 1\nsample_count = 4\nsample_mask = 9\n",
             "color_format = 4\ncolor_exp_bias = 0\nsample_count = 1\nsample_mask = 9\n",
             "color_format = '4'\ncolor_exp_bias = 0\nsample_count = 1\nsample_mask = 1\n" })
    {
        write(root / "shaders.toml", manifest(rex::crypto::sha256(shaderBytes), invalidSpecialization));
        UNSCOPED_INFO("unsupported target specialization must invalidate the catalog");
        REQUIRE((!LoadNativeGameplayShaders(root, shaders, error) && shaders.empty()));
    }
    write(root / "shaders.toml", text);
    write(root / "shader.dxil", "changed payload");
    UNSCOPED_INFO("changed shader bytes must invalidate the entire catalog");
    REQUIRE((!LoadNativeGameplayShaders(root, shaders, error) && shaders.empty()));
    write(root / "shader.dxil", shaderBytes);
    for (const std::string samplerMode : { "independent_samplers = false\n", "" })
    {
        auto invalid = text;
        invalid.replace(invalid.find("independent_samplers = true\n"), std::string("independent_samplers = true\n").size(), samplerMode);
        write(root / "shaders.toml", invalid);
        UNSCOPED_INFO("shared or undeclared sampler mode must invalidate the catalog");
        REQUIRE((!LoadNativeGameplayShaders(root, shaders, error) && shaders.empty()));
    }
    auto escaped = text;
    escaped.replace(escaped.find("vertex_file = 'shader.dxil'"), std::string("vertex_file = 'shader.dxil'").size(), "vertex_file = '../shader.dxil'");
    write(root / "shaders.toml", escaped);
    UNSCOPED_INFO("catalog cannot use a shader outside its owned directory");
    REQUIRE((!LoadNativeGameplayShaders(root, shaders, error) && shaders.empty()));
    auto duplicate = text + text.substr(text.find("[[shaders]]"));
    write(root / "shaders.toml", duplicate);
    UNSCOPED_INFO("ambiguous shader specializations must be rejected");
    REQUIRE((!LoadNativeGameplayShaders(root, shaders, error) && shaders.empty()));
    depthOnly(root, rex::crypto::sha256(shaderBytes));
    dualFormat(root, shaderBytes, "synthetic SPIR-V payload");
    UNSCOPED_INFO("test fixture cleanup");
    REQUIRE((std::filesystem::remove(root / "shaders.toml") && std::filesystem::remove(root / "shader.dxil") &&
             std::filesystem::remove(root)));
}

// Loads a saved gameplay shader catalog: native-gameplay-shaders.catalog <dir>.
TEST_CASE("native_gameplay_shaders_catalog", "[.][manual][native-gameplay-shaders]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() == 0)
    {
        SKIP("pass one catalog directory through --helper native-gameplay-shaders.catalog");
    }
    UNSCOPED_INFO("pass exactly one catalog directory");
    REQUIRE(args.argc() == 1);
    std::vector<NativeGameplayShaderInterface> shaders;
    std::string                                error;
    UNSCOPED_INFO(error);
    REQUIRE(LoadNativeGameplayShaders(args.argv()[1], shaders, error));
}

REREVVED_MANUAL_HELPER(nativeGameplayShadersCatalog, "native-gameplay-shaders.catalog", "native_gameplay_shaders_catalog")
