#include "gpu/guest_d3d/solid_draw_reference.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <utility>

using namespace rerevved::gpu::guest_d3d;
using namespace rex::graphics::diagnostic;

namespace
{

void store(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned c = 0; c < 4; ++c)
        bytes[offset + c] = static_cast<std::uint8_t>(value >> (24 - c * 8));
}

void testDiskLoader()
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("solid-reference-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    UNSCOPED_INFO("fresh synthetic SDK fixture directory");
    REQUIRE(std::filesystem::create_directory(root));
    const auto write = [&](const char* name, const std::vector<std::uint8_t>& bytes)
    {
        std::ofstream file(root / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        file.close();
        UNSCOPED_INFO("write synthetic SDK fixture");
        REQUIRE(bool(file));
    };
    std::vector<std::uint8_t> registers(0x5003 * 4);
    registers[0x4000 * 4]     = 0x78;
    registers[0x4000 * 4 + 1] = 0x56;
    registers[0x4000 * 4 + 2] = 0x34;
    registers[0x4000 * 4 + 3] = 0x12;
    write("registers.bin", registers);
    write("vertex.ucode.bin", { 0x21, 0x43, 0x65, 0x87 });
    write("pixel.ucode.bin", { 0x78, 0x56, 0x34, 0x12 });
    write("vertex_fetch_95.bin", { 0, 1, 2, 3 });
    write("index.bin", { 0, 0, 0, 1, 0, 0, 0, 0 });
    write("edram_before.bin", std::vector<std::uint8_t>(10 * 1024 * 1024));
    write("edram_after.bin", std::vector<std::uint8_t>(10 * 1024 * 1024));
    const std::string manifest = R"({
        "schema_version":1,"status":"complete","valid":true,"encoding":"uint32-little-endian",
        "provenance":{"frame":730,"submission":2196},
        "draw":{
            "guest":{"primitive_type":4,"index_count":6,"major_mode_explicit":true,
                     "guest_draw_vertex_count":3,"source":"guest_dma",
                     "dma":{"base":520320000,"length_bytes":8,"count":3,"format":0,"endianness":1}},
            "processed":{"guest_primitive_type":4,"host_primitive_type":4,"host_vertex_shader_type":0,
                         "tessellation_mode":0,"host_draw_vertex_count":3,"host_index_format":57,
                         "host_shader_index_endian":0,"primitive_reset":false},
            "native":{"indexed":true,"topology":4,"vertex_count":0,"index_count":3,"instance_count":1,
                      "start_vertex":0,"start_index":0,"base_vertex":0,"start_instance":0},
            "used_texture_mask":0,"normalized_color_mask":15,"resolution_scale":[1,1],
            "resources":{"vertex_fetch_ranges":[{"fetch_constant":95,"base":520319904,"size_bytes":4,
                                                  "file":"vertex_fetch_95.bin"}],
                         "index":{"base":520320000,"size_bytes":8,"file":"index.bin"}}},
        "shaders":{"vertex":{"dword_count":1,"file":"vertex.ucode.bin"},
                   "pixel":{"dword_count":1,"file":"pixel.ucode.bin"}},
        "registers":{"dword_count":20483,"file":"registers.bin"},
        "edram":{"path":"rov","scale_x":1,"scale_y":1,"snapshot_size_bytes":10485760,
                 "before_file":"edram_before.bin","after_file":"edram_after.bin"}
    })";
    const auto        load     = [&](const std::string& text, DrawCaptureSnapshot& snapshot, std::string& error)
    {
        write("manifest.json", std::vector<std::uint8_t>(text.begin(), text.end()));
        return LoadSdkSolidDrawReference(root, snapshot, error);
    };
    DrawCaptureSnapshot snapshot;
    std::string         error;
    UNSCOPED_INFO("complete SDK disk manifest loads");
    REQUIRE(load(manifest, snapshot, error));
    UNSCOPED_INFO("disk snapshot preserves SDK processed count and DMA padding semantics");
    REQUIRE((snapshot.frame == 730 && snapshot.submission == 2196 && snapshot.index.count == 3 &&
             snapshot.requested_index_count == 6 && snapshot.index.bytes.size() == 8));
    UNSCOPED_INFO("register and shader files decode as little-endian words");
    REQUIRE((snapshot.registers[0x4000] == 0x12345678 && snapshot.vertex_ucode[0] == 0x87654321 &&
             snapshot.pixel_ucode[0] == 0x12345678 && snapshot.half_pixel_offset));
    for (const auto change : { std::pair{ std::string("\"complete\""), std::string("\"failed\"") },
                               std::pair{ std::string("\"valid\":true"), std::string("\"valid\":false") },
                               std::pair{ std::string("uint32-little-endian"), std::string("uint32-big-endian") } })
    {
        auto invalid = manifest;
        invalid.replace(invalid.find(change.first), change.first.size(), change.second);
        UNSCOPED_INFO("incomplete, invalid or differently encoded SDK manifests are rejected");
        REQUIRE((!load(invalid, snapshot, error) && error == "adapter_reference_manifest_status_or_encoding"));
    }
    const auto truncated = manifest.substr(0, manifest.size() - 1);
    UNSCOPED_INFO("truncated SDK JSON is rejected");
    REQUIRE(!load(truncated, snapshot, error));
    for (const auto name : { "manifest.json", "registers.bin", "vertex.ucode.bin", "pixel.ucode.bin", "vertex_fetch_95.bin", "index.bin", "edram_before.bin", "edram_after.bin" })
    {
        UNSCOPED_INFO("remove exact synthetic fixture file");
        REQUIRE(std::filesystem::remove(root / name));
    }
    UNSCOPED_INFO("remove empty synthetic fixture directory");
    REQUIRE(std::filesystem::remove(root));
}

} // namespace

TEST_CASE("solid_draw_reference", "[ctest][solid-draw-reference]")
{
    SolidDrawInputs inputs;
    inputs.arguments.indexCount = 3;
    inputs.state.resize(0x3500);
    inputs.vertices  = { 0, 1, 2, 3, 4, 5, 6, 7 };
    inputs.indices   = { 0, 0, 0, 1, 0, 0 };
    inputs.pixelCode = { 0x12, 0x34, 0x56, 0x78 };
    SolidDrawRecipe recipe;
    recipe.patchedVertexCode = { 0x87, 0x65, 0x43, 0x21 };
    recipe.effectiveState.resize(0x3500);
    DrawCaptureSnapshot reference;
    reference.registers.resize(0x5003);
    reference.vertex_ucode         = { 0x87654321 };
    reference.pixel_ucode          = { 0x12345678 };
    reference.primitive_type       = 4;
    reference.half_pixel_offset    = true;
    reference.color_target_written = true;
    auto& fetch                    = reference.vertex_fetches.emplace_back();
    fetch.fetch_constant           = 95;
    fetch.base                     = 0x1F037380;
    fetch.bytes                    = inputs.vertices;
    reference.index.present        = true;
    reference.index.guest_base     = 0x1F0373E0;
    reference.index.count          = 3;
    reference.index.endianness     = 1;
    reference.index.bytes          = inputs.indices;
    for (const auto mapping : kSolidFixedWords)
    {
        reference.registers[mapping.hardware] = mapping.hardware;
        store(recipe.effectiveState, mapping.shadow, mapping.hardware);
    }
    for (const auto group : kSolidReferenceConstants)
        for (unsigned i = 0; i < group[2]; ++i)
        {
            reference.registers[group[1] + i] = group[1] + i;
            store(inputs.state, group[0] + i * 4, group[1] + i);
        }
    std::string               error;
    SolidReferenceCorrelation correlation;
    const auto                correlate = [&]
    {
        return CorrelateSolidDrawReference(reference, inputs, recipe, 0x9EFE3F80, 0x9EFE3FE0, correlation, error);
    };
    UNSCOPED_INFO("identical tuple at different submitted bases is accepted by content");
    REQUIRE((correlate() && !correlation.addressCorrelated && error.empty()));
    UNSCOPED_INFO("both submitted base pairs are reported");
    REQUIRE((correlation.adapterVertexBase == 0x1EFE3F80 && correlation.adapterIndexBase == 0x1EFE3FE0 &&
             correlation.referenceVertexBase == 0x1F037380 && correlation.referenceIndexBase == 0x1F0373E0));
    // The same physical allocation may also be named by its virtual alias.
    UNSCOPED_INFO("matching aliases report address correlation");
    REQUIRE((CorrelateSolidDrawReference(reference, inputs, recipe, 0x9F037380, 0x9F0373E0, correlation, error) &&
             correlation.addressCorrelated));
    fetch.bytes[3] ^= 1;
    UNSCOPED_INFO("one vertex byte change is rejected");
    REQUIRE((!correlate() && error == "adapter_reference_foreign_geometry"));
    fetch.bytes[3] ^= 1;
    reference.index.bytes[3] ^= 1;
    UNSCOPED_INFO("one index byte change is rejected");
    REQUIRE((!correlate() && error == "adapter_reference_foreign_geometry"));
    reference.index.bytes[3] ^= 1;
    for (const auto reg : { 0x4003U, 0x403FU, 0x4400U, 0x477FU, 0x4900U })
    {
        reference.registers[reg] ^= 1;
        UNSCOPED_INFO("referenced or conservatively retained constant change is rejected");
        REQUIRE((!correlate() && error == "adapter_reference_foreign_referenced_constants"));
        reference.registers[reg] ^= 1;
    }
    reference.registers[0x43F0] ^= 1;
    UNSCOPED_INFO("unused VS constant tail does not reject content identity");
    REQUIRE(correlate());
    reference.vertex_ucode[0] ^= 1;
    UNSCOPED_INFO("changed VS microcode is rejected");
    REQUIRE((!correlate() && error == "adapter_reference_foreign_shaders"));
    reference.vertex_ucode[0] ^= 1;
    reference.pixel_ucode[0] ^= 1;
    UNSCOPED_INFO("changed PS microcode is rejected");
    REQUIRE((!correlate() && error == "adapter_reference_foreign_shaders"));
    reference.pixel_ucode[0] ^= 1;
    for (const auto mapping : kSolidFixedWords)
    {
        reference.registers[mapping.hardware] ^= 1;
        UNSCOPED_INFO("each fixed word participates in identity");
        REQUIRE((!correlate() && error == "adapter_reference_foreign_fixed_state"));
        reference.registers[mapping.hardware] ^= 1;
    }
    reference.native_instance_count = 2;
    UNSCOPED_INFO("existing draw tuple gate remains enforced");
    REQUIRE((!correlate() && error == "adapter_reference_draw_tuple"));
    reference.native_instance_count = 1;
    UNSCOPED_INFO("restoring tuple recovers content correlation");
    REQUIRE(correlate());
    testDiskLoader();
}
