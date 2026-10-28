#include "directory_removal.hpp"
#include "gpu/diagnostics/draw_capture_frame_inputs.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <rex/crypto/sha256.h>
#include <rex/graphics/draw_capture_mailbox.h>
#include <toml++/toml.hpp>

namespace
{

using namespace rex::graphics::diagnostic;

void checkPayload(const std::filesystem::path& root, toml::node_view<const toml::node> entry, std::string_view filename, std::string_view expected, std::string_view sha256)
{
    CAPTURE(filename);
    REQUIRE(entry["file"].value<std::string>() == filename);
    REQUIRE(entry["bytes"].value<std::int64_t>() == expected.size());
    REQUIRE(entry["sha256"].value<std::string>() == sha256);
    std::ifstream file(root / filename, std::ios::binary);
    REQUIRE(file.is_open());
    const std::string actual{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    REQUIRE(actual == expected);
    REQUIRE(rex::crypto::sha256(actual) == sha256);
}

} // namespace

TEST_CASE("draw_capture_frame_inputs", "[ctest][draw-capture-frame-inputs]")
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("rerevved-owned-frame-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    INFO("Saved fixture directory (removed at scope exit) " << root.string());
    REQUIRE(!std::filesystem::exists(root));
    const rerevved::tests::ScopedDirectoryRemoval removeDirectory(root);

    DrawCaptureFrameSnapshot snapshot;
    snapshot.start_frame                 = 31;
    snapshot.end_frame                   = 32;
    snapshot.start_submission            = 41;
    snapshot.end_submission              = 42;
    snapshot.acquisition                 = DrawCaptureAvailability::kComplete;
    snapshot.acquisition_reason          = DrawCaptureReason::kNone;
    auto& event                          = snapshot.events.emplace_back();
    event.id                             = 7;
    event.frame                          = 32;
    event.submission                     = 42;
    event.success                        = true;
    event.host_issued                    = true;
    event.execution_status               = DrawCaptureExecutionStatus::kIssued;
    event.execution_reason               = DrawCaptureReason::kNone;
    event.acquisition                    = DrawCaptureAvailability::kPartial;
    event.acquisition_reason             = DrawCaptureReason::kTextureSource;
    event.draw.acquisition               = DrawCaptureAvailability::kUnavailable;
    event.draw.acquisition_reason        = DrawCaptureReason::kReadbackBudget;
    event.draw.texture_bindings_complete = false;
    auto& draw                           = event.draw.draw;
    draw.frame                           = 32;
    draw.submission                      = 42;
    draw.major_mode_explicit             = true;
    draw.indexed                         = true;
    draw.guest_index_size                = 2;
    draw.host_index_format               = 57;
    draw.host_shader_index_endian        = 1;
    draw.color_target_written            = true;
    draw.registers                       = { 0x04030201 };
    draw.vertex_ucode                    = { 0x08070605 };
    draw.pixel_ucode                     = { 0x0C0B0A09 };
    draw.index.present                   = true;
    draw.index.dma_count                 = 6;
    draw.index.dma_length                = 12;
    draw.index.bytes                     = { 'I', 'N', 'D', 'X' };
    draw.index.availability              = DrawCaptureAvailability::kPartial;
    draw.index.unavailable_reason        = DrawCaptureReason::kIndexSource;
    auto& fetch                          = draw.vertex_fetches.emplace_back();
    fetch.fetch_constant                 = 3;
    fetch.base                           = 0x123400;
    fetch.bytes                          = { 'b', 'a', 'd' };
    fetch.shared_bytes                   = std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{ 'F', 'E', 'T', 'C' });
    fetch.availability                   = DrawCaptureAvailability::kUnavailable;
    fetch.unavailable_reason             = DrawCaptureReason::kVertexFetch;

    DrawCaptureTextureUse required;
    required.stage                = 1;
    required.fetch_constant       = 4;
    required.shader_binding_index = 5;
    required.unavailable_reason   = DrawCaptureReason::kTextureSource;
    event.draw.texture_uses.push_back(required);
    auto optional               = required;
    optional.required           = false;
    optional.is_signed          = true;
    optional.unavailable_reason = DrawCaptureReason::kSignMaskUnused;
    event.draw.texture_uses.push_back(optional);
    auto partial               = required;
    partial.texture_index      = 0;
    partial.data_available     = true;
    partial.availability       = DrawCaptureAvailability::kPartial;
    partial.unavailable_reason = DrawCaptureReason::kReadbackBudget;
    event.draw.texture_uses.push_back(partial);

    auto& texture                    = snapshot.textures.emplace_back();
    texture.host_format              = 28;
    texture.host_width               = 2;
    texture.host_height              = 2;
    texture.host_depth_or_array_size = 3;
    texture.host_mip_levels          = 2;
    texture.texture_instance_id      = 0xFEDCBA9876543210ULL;
    texture.content_generation       = 0x8123456789ABCDEFULL;
    texture.row_pitch                = 256;
    texture.row_size                 = 8;
    texture.row_count                = 2;
    texture.bytes                    = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
    texture.availability             = DrawCaptureAvailability::kPartial;
    texture.unavailable_reason       = DrawCaptureReason::kReadbackBudget;
    texture.subresources             = {
        { 0, 0, 2, 2, 256, 8, 2, texture.bytes },
        { 1, 0, 1, 1, 256, 4, 1, { 'M', 'I', 'P', '1' } },
        { 1, 2, 1, 1, 512, 4, 1, { 'A', 'R', 'R', '2' } },
    };
    auto& swap                     = snapshot.final_swap;
    swap.frontbuffer_ptr           = 0x12345678;
    swap.source_format             = 28;
    swap.source_width              = 1;
    swap.source_height             = 1;
    swap.source_row_pitch          = 256;
    swap.source_row_size           = 4;
    swap.source_row_count          = 1;
    swap.source_bytes              = { 'S', 'R', 'C', '!' };
    swap.output_format             = 24;
    swap.output_width              = 1;
    swap.output_height             = 1;
    swap.output_row_pitch          = 512;
    swap.output_row_size           = 4;
    swap.output_row_count          = 1;
    swap.output_bytes              = { 'R', 'E', 'F', '!' };
    swap.source_availability       = DrawCaptureAvailability::kPartial;
    swap.source_unavailable_reason = DrawCaptureReason::kReadback;
    swap.output_availability       = DrawCaptureAvailability::kUnavailable;
    swap.output_unavailable_reason = DrawCaptureReason::kFence;
    swap.gamma.use_pwl_gamma_ramp  = true;
    swap.gamma.use_fxaa            = true;
    swap.gamma.is_8bpc             = true;
    swap.gamma.source_format       = 87;
    swap.gamma.output_format       = 10;
    swap.gamma.output_width        = 3;
    swap.gamma.output_height       = 2;
    swap.gamma.gamma_256           = { 0x04030201 };
    swap.gamma.gamma_pwl_rgb       = { 0x100F0E0D, 0x14131211 };

    rerevved::gpu::diagnostics::SaveDrawCaptureFrameInputs(root, snapshot);
    const auto manifest = toml::parse_file((root / "manifest.toml").string());
    REQUIRE(manifest["schema_version"].value<int>() == 1);
    REQUIRE(manifest["sdk_schema_version"].value<int>() == 2);
    REQUIRE(manifest["start_frame"].value<int>() == 31);
    REQUIRE(manifest["end_frame"].value<int>() == 32);
    REQUIRE(manifest["start_submission"].value<int>() == 41);
    REQUIRE(manifest["end_submission"].value<int>() == 42);
    REQUIRE(manifest["acquisition"].value<int>() == 2);
    REQUIRE(manifest["acquisition_reason"].value<int>() == 0);
    REQUIRE(manifest["events"].as_array()->size() == 1);
    const auto savedEvent = manifest["events"][0];
    REQUIRE(savedEvent["id"].value<int>() == 7);
    REQUIRE(savedEvent["kind"].value<int>() == 0);
    REQUIRE(savedEvent["success"].value<bool>() == true);
    REQUIRE(savedEvent["host_issued"].value<bool>() == true);
    REQUIRE(savedEvent["execution_status"].value<int>() == 1);
    REQUIRE(savedEvent["execution_reason"].value<int>() == 0);
    REQUIRE(savedEvent["acquisition"].value<int>() == 1);
    REQUIRE(savedEvent["acquisition_reason"].value<int>() == 13);
    REQUIRE(savedEvent["draw_acquisition"].value<int>() == 0);
    REQUIRE(savedEvent["draw_acquisition_reason"].value<int>() == 8);
    REQUIRE(savedEvent["texture_bindings_complete"].value<bool>() == false);
    REQUIRE(savedEvent["draw_frame"].value<int>() == 32);
    REQUIRE(savedEvent["draw_submission"].value<int>() == 42);
    REQUIRE(savedEvent["major_mode_explicit"].value<bool>() == true);
    REQUIRE(savedEvent["draw_indexed"].value<bool>() == true);
    REQUIRE(savedEvent["guest_index_size"].value<int>() == 2);
    REQUIRE(savedEvent["host_index_format"].value<int>() == 57);
    REQUIRE(savedEvent["host_shader_index_endian"].value<int>() == 1);
    REQUIRE(savedEvent["color_target_written"].value<bool>() == true);
    REQUIRE(savedEvent["index_dma_count"].value<int>() == 6);
    REQUIRE(savedEvent["index_dma_length"].value<int>() == 12);
    REQUIRE(savedEvent["index_availability"].value<int>() == 1);
    REQUIRE(savedEvent["index_unavailable_reason"].value<int>() == 7);
    REQUIRE(savedEvent["vertex_fetches"].as_array()->size() == 1);
    const auto savedFetch = savedEvent["vertex_fetches"][0];
    REQUIRE(savedFetch["slot"].value<int>() == 3);
    REQUIRE(savedFetch["base"].value<int>() == 0x123400);
    REQUIRE(savedFetch["availability"].value<int>() == 0);
    REQUIRE(savedFetch["unavailable_reason"].value<int>() == 6);
    REQUIRE(savedEvent["texture_uses"].as_array()->size() == 3);
    const auto savedRequired = savedEvent["texture_uses"][0];
    REQUIRE(savedRequired["texture"].value<std::int64_t>() == 4294967295LL);
    REQUIRE(savedRequired["stage"].value<int>() == 1);
    REQUIRE(savedRequired["fetch"].value<int>() == 4);
    REQUIRE(savedRequired["binding"].value<int>() == 5);
    REQUIRE(savedRequired["required"].value<bool>() == true);
    REQUIRE(savedRequired["available"].value<bool>() == false);
    REQUIRE(savedRequired["availability"].value<int>() == 0);
    REQUIRE(savedRequired["unavailable_reason"].value<int>() == 13);
    const auto savedOptional = savedEvent["texture_uses"][1];
    REQUIRE(savedOptional["required"].value<bool>() == false);
    REQUIRE(savedOptional["signed"].value<bool>() == true);
    REQUIRE(savedOptional["available"].value<bool>() == false);
    REQUIRE(savedOptional["availability"].value<int>() == 0);
    REQUIRE(savedOptional["unavailable_reason"].value<int>() == 21);
    const auto savedPartial = savedEvent["texture_uses"][2];
    REQUIRE(savedPartial["texture"].value<int>() == 0);
    REQUIRE(savedPartial["required"].value<bool>() == true);
    REQUIRE(savedPartial["available"].value<bool>() == true);
    REQUIRE(savedPartial["availability"].value<int>() == 1);
    REQUIRE(savedPartial["unavailable_reason"].value<int>() == 8);
    REQUIRE(manifest["textures"].as_array()->size() == 1);
    const auto savedTexture = manifest["textures"][0];
    REQUIRE(savedTexture["format"].value<int>() == 28);
    REQUIRE(savedTexture["width"].value<int>() == 2);
    REQUIRE(savedTexture["height"].value<int>() == 2);
    REQUIRE(savedTexture["depth_or_array_size"].value<int>() == 3);
    REQUIRE(savedTexture["mip_level"].value<int>() == 0);
    REQUIRE(savedTexture["row_pitch"].value<int>() == 256);
    REQUIRE(savedTexture["row_size"].value<int>() == 8);
    REQUIRE(savedTexture["row_count"].value<int>() == 2);
    REQUIRE(savedTexture["host_mip_levels"].value<int>() == 2);
    REQUIRE(savedTexture["texture_instance_id"].value<std::string>() == "FEDCBA9876543210");
    REQUIRE(savedTexture["content_generation"].value<std::string>() == "8123456789ABCDEF");
    REQUIRE(savedTexture["availability"].value<int>() == 1);
    REQUIRE(savedTexture["unavailable_reason"].value<int>() == 8);
    REQUIRE(savedTexture["subresources"].as_array()->size() == 3);
    const auto base  = savedTexture["subresources"][0];
    const auto mip   = savedTexture["subresources"][1];
    const auto slice = savedTexture["subresources"][2];
    for (const auto entry : { base, mip, slice })
    {
        REQUIRE(entry["row_pitch"].value<int>() > entry["row_size"].value<int>());
    }
    REQUIRE(base["mip_level"].value<int>() == 0);
    REQUIRE(base["array_slice"].value<int>() == 0);
    REQUIRE(base["width"].value<int>() == 2);
    REQUIRE(base["height"].value<int>() == 2);
    REQUIRE(base["row_pitch"].value<int>() == 256);
    REQUIRE(base["row_size"].value<int>() == 8);
    REQUIRE(base["row_count"].value<int>() == 2);
    REQUIRE(mip["mip_level"].value<int>() == 1);
    REQUIRE(mip["array_slice"].value<int>() == 0);
    REQUIRE(mip["width"].value<int>() == 1);
    REQUIRE(mip["height"].value<int>() == 1);
    REQUIRE(mip["row_pitch"].value<int>() == 256);
    REQUIRE(mip["row_size"].value<int>() == 4);
    REQUIRE(mip["row_count"].value<int>() == 1);
    REQUIRE(slice["mip_level"].value<int>() == 1);
    REQUIRE(slice["array_slice"].value<int>() == 2);
    REQUIRE(slice["width"].value<int>() == 1);
    REQUIRE(slice["height"].value<int>() == 1);
    REQUIRE(slice["row_pitch"].value<int>() == 512);
    REQUIRE(slice["row_size"].value<int>() == 4);
    REQUIRE(slice["row_count"].value<int>() == 1);
    REQUIRE(manifest["final_swap_frontbuffer"].value<int>() == 0x12345678);
    REQUIRE(manifest["source_availability"].value<int>() == 1);
    REQUIRE(manifest["source_unavailable_reason"].value<int>() == 14);
    REQUIRE(manifest["output_availability"].value<int>() == 0);
    REQUIRE(manifest["output_unavailable_reason"].value<int>() == 16);
    REQUIRE(manifest["source_format"].value<int>() == 28);
    REQUIRE(manifest["source_width"].value<int>() == 1);
    REQUIRE(manifest["source_height"].value<int>() == 1);
    REQUIRE(manifest["source_row_pitch"].value<int>() == 256);
    REQUIRE(manifest["source_row_size"].value<int>() == 4);
    REQUIRE(manifest["source_row_count"].value<int>() == 1);
    REQUIRE(manifest["output_format"].value<int>() == 24);
    REQUIRE(manifest["output_width"].value<int>() == 1);
    REQUIRE(manifest["output_height"].value<int>() == 1);
    REQUIRE(manifest["output_row_pitch"].value<int>() == 512);
    REQUIRE(manifest["output_row_size"].value<int>() == 4);
    REQUIRE(manifest["output_row_count"].value<int>() == 1);
    REQUIRE(manifest["use_pwl_gamma"].value<bool>() == true);
    REQUIRE(manifest["use_fxaa"].value<bool>() == true);
    REQUIRE(manifest["is_8bpc"].value<bool>() == true);
    REQUIRE(manifest["gamma_source_format"].value<int>() == 87);
    REQUIRE(manifest["gamma_output_format"].value<int>() == 10);
    REQUIRE(manifest["gamma_output_width"].value<int>() == 3);
    REQUIRE(manifest["gamma_output_height"].value<int>() == 2);
    checkPayload(root, savedTexture["data"], "texture-0.bin", "0123456789abcdef", "9f9f5111f7b27a781f1f1ddde5ebc2dd2b796bfc7365c9c28b548e564176929f");
    checkPayload(root, base["data"], "texture-0-subresource-0.bin", "0123456789abcdef", "9f9f5111f7b27a781f1f1ddde5ebc2dd2b796bfc7365c9c28b548e564176929f");
    checkPayload(root, mip["data"], "texture-0-subresource-1.bin", "MIP1", "f66e90397b9ba55a69aa465cbf779003a1e0b4e9ce8507a3abbd265d8a616663");
    checkPayload(root, slice["data"], "texture-0-subresource-2.bin", "ARR2", "d3baacf22674955b424e500c2d1b8665febc765f8ccfd85597418c9a27b0934d");
    checkPayload(root, savedFetch["data"], "event-7-fetch-3.bin", "FETC", "ada2f0db7587c0a091b377a3b53bf253fa31095f74440c9343dc50502e720bfa");
    checkPayload(root, savedEvent["indices"], "event-7-indices.bin", "INDX", "4d48695d82e291b4446a1892a7207442379809310b3b6d5585b8347037001bf7");
    checkPayload(root, savedEvent["registers"], "event-7-registers.bin", "\x01\x02\x03\x04", "9f64a747e1b97f131fabb6b447296c9b6f0201e79fb3c5356e6c77e89b6a806a");
    checkPayload(root, savedEvent["vertex_ucode"], "event-7-vs.bin", "\x05\x06\x07\x08", "55e5509f8052998294266ee5b50cb592938191fb5d67f73cac2e60b0276b1bdd");
    checkPayload(root, savedEvent["pixel_ucode"], "event-7-ps.bin", "\x09\x0A\x0B\x0C", "e1e853684a206f162ee800a54b695c9cc1a8d1d554a47fcb13fe51229c17773f");
    checkPayload(root, manifest["gamma"], "gamma.bin", "\x01\x02\x03\x04", "9f64a747e1b97f131fabb6b447296c9b6f0201e79fb3c5356e6c77e89b6a806a");
    checkPayload(root, manifest["gamma_pwl"], "gamma-pwl.bin", "\x0D\x0E\x0F\x10\x11\x12\x13\x14", "201b091d3aaf94113bbc398677ce282953c68c476ebfb8ee37e91b9f75f214e2");
    checkPayload(root, manifest["source"], "source.bin", "SRC!", "bed2f941abbfabf0a5056a83595bbe52e80257f57141f8e9187f57cf113ab817");
    checkPayload(root, manifest["reference"], "reference.rgb10", "REF!", "bc07e584a62c803d459284c625fef54f7c423a4e01fe71c4c96bfdfca86294a4");
}
