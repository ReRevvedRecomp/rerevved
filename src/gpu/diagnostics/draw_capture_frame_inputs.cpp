#include "draw_capture_frame_inputs.h"

#include <fstream>
#include <span>
#include <stdexcept>

#include <fmt/format.h>

#include <rex/crypto/sha256.h>
#include <rex/graphics/draw_capture_mailbox.h>
#include <toml++/toml.hpp>

namespace rerevved::gpu::diagnostics
{
namespace
{

using namespace rex::graphics::diagnostic;

std::string digest(std::span<const std::uint8_t> bytes)
{
    return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.close();
    return bool(file);
}

} // namespace

void SaveDrawCaptureFrameInputs(const std::filesystem::path& root, const DrawCaptureFrameSnapshot& snapshot)
{
    std::filesystem::create_directory(root);
    toml::array events, textures;
    const auto  bytes = [&](const std::string& name, std::span<const std::uint8_t> data)
    {
        if (!write(root / name, data))
            throw std::runtime_error("could not save owned frame input " + name);
        return toml::table{ { "file", name }, { "bytes", std::int64_t(data.size()) }, { "sha256", digest(data) } };
    };
    const auto words = [&](const std::string& name, std::span<const std::uint32_t> data)
    {
        std::vector<std::uint8_t> packed(data.size_bytes());
        for (std::size_t i = 0; i < data.size(); ++i)
            for (unsigned c = 0; c < 4; ++c)
                packed[i * 4 + c] = static_cast<std::uint8_t>(data[i] >> (c * 8));
        return bytes(name, packed);
    };
    for (const auto& event : snapshot.events)
    {
        const auto  stem = "event-" + std::to_string(event.id);
        const auto& draw = event.draw.draw;
        toml::table row{ { "id", std::int64_t(event.id) }, { "kind", std::int64_t(event.kind) }, { "frame", std::int64_t(event.frame) }, { "submission", std::int64_t(event.submission) }, { "success", event.success }, { "host_issued", event.host_issued }, { "primitive", draw.primitive_type }, { "requested_count", draw.requested_index_count }, { "indexed", draw.index.present }, { "index_format", draw.index.format }, { "index_endian", draw.index.endianness }, { "registers", words(stem + "-registers.bin", draw.registers) }, { "vertex_ucode", words(stem + "-vs.bin", draw.vertex_ucode) }, { "pixel_ucode", words(stem + "-ps.bin", draw.pixel_ucode) }, { "indices", bytes(stem + "-indices.bin", draw.index.bytes) }, { "copy_mode", event.copy_mode }, { "frontbuffer", std::int64_t(event.frontbuffer_ptr) } };
        row.insert("guest_vertex_hash", fmt::format("{:016X}", event.draw.guest_vertex_shader_hash));
        row.insert("guest_pixel_hash", fmt::format("{:016X}", event.draw.guest_pixel_shader_hash));
        row.insert("host_vertex_hash", fmt::format("{:016X}", draw.vertex_shader_hash));
        row.insert("host_pixel_hash", fmt::format("{:016X}", draw.pixel_shader_hash));
        row.insert("host_primitive", draw.host_primitive_type);
        row.insert("host_vertex_count", draw.host_draw_vertex_count);
        row.insert("host_vertex_shader_type", draw.host_vertex_shader_type);
        row.insert("host_tessellation", draw.tessellation_mode);
        row.insert("guest_primitive", draw.guest_primitive_type);
        row.insert("guest_vertex_count", draw.guest_draw_vertex_count);
        row.insert("guest_index_base", draw.guest_index_base);
        row.insert("index_base", draw.index.guest_base);
        row.insert("index_count", draw.index.count);
        row.insert("native_topology", draw.native_topology);
        row.insert("native_vertex_count", draw.native_vertex_count);
        row.insert("native_index_count", draw.native_index_count);
        row.insert("native_instance_count", draw.native_instance_count);
        row.insert("native_start_vertex", draw.native_start_vertex);
        row.insert("native_start_index", draw.native_start_index);
        row.insert("native_base_vertex", draw.native_base_vertex);
        row.insert("native_start_instance", draw.native_start_instance);
        row.insert("host_primitive_reset", draw.host_primitive_reset_enabled);
        row.insert("used_texture_mask", draw.used_texture_mask);
        row.insert("normalized_color_mask", draw.normalized_color_mask);
        row.insert("half_pixel_offset", draw.half_pixel_offset);
        row.insert("texture_bindings_complete", event.draw.texture_bindings_complete);
        row.insert("frontbuffer_width", event.frontbuffer_width);
        row.insert("frontbuffer_height", event.frontbuffer_height);
        toml::array fetches, uses;
        for (const auto& fetch : draw.vertex_fetches)
            fetches.push_back(toml::table{ { "slot", fetch.fetch_constant }, { "base", fetch.base }, { "data", bytes(stem + "-fetch-" + std::to_string(fetch.fetch_constant) + ".bin", fetch.Payload()) } });
        for (const auto& use : event.draw.texture_uses)
            uses.push_back(toml::table{ { "texture", use.texture_index }, { "stage", use.stage }, { "fetch", use.fetch_constant }, { "binding", use.shader_binding_index }, { "dimension", use.dimension }, { "available", use.data_available }, { "signed", use.is_signed }, { "component_mapping", use.component_mapping }, { "host_swizzle", use.host_swizzle }, { "swizzled_signs", use.swizzled_signs }, { "sampler", use.sampler_value } });
        row.insert("vertex_fetches", std::move(fetches));
        row.insert("texture_uses", std::move(uses));
        events.push_back(std::move(row));
    }
    for (std::size_t i = 0; i < snapshot.textures.size(); ++i)
    {
        const auto& texture = snapshot.textures[i];
        textures.push_back(toml::table{ { "index", std::int64_t(i) }, { "format", texture.host_format }, { "width", texture.host_width }, { "height", texture.host_height }, { "depth_or_array_size", texture.host_depth_or_array_size }, { "mip_level", texture.mip_level }, { "row_pitch", texture.row_pitch }, { "row_size", texture.row_size }, { "row_count", texture.row_count }, { "scaled_resolve", texture.scaled_resolve }, { "guest_base_page", texture.guest_base_page }, { "guest_mip_page", texture.guest_mip_page }, { "guest_width", texture.guest_width }, { "guest_height", texture.guest_height }, { "guest_depth_or_array_size", texture.guest_depth_or_array_size }, { "guest_pitch", texture.guest_pitch }, { "guest_format", texture.guest_format }, { "guest_endianness", texture.guest_endianness }, { "guest_tiled", texture.guest_tiled }, { "guest_packed_mips", texture.guest_packed_mips }, { "data", bytes("texture-" + std::to_string(i) + ".bin", texture.bytes) } });
    }
    toml::table manifest{ { "schema_version", 1 }, { "start_frame", std::int64_t(snapshot.start_frame) }, { "end_frame", std::int64_t(snapshot.end_frame) }, { "start_submission", std::int64_t(snapshot.start_submission) }, { "end_submission", std::int64_t(snapshot.end_submission) }, { "events", std::move(events) }, { "textures", std::move(textures) }, { "gamma", words("gamma.bin", snapshot.final_swap.gamma.gamma_256) }, { "source", bytes("source.bin", snapshot.final_swap.source_bytes) }, { "reference", bytes("reference.rgb10", snapshot.final_swap.output_bytes) } };
    manifest.insert("output_format", snapshot.final_swap.output_format);
    manifest.insert("output_width", snapshot.final_swap.output_width);
    manifest.insert("output_height", snapshot.final_swap.output_height);
    manifest.insert("source_format", snapshot.final_swap.source_format);
    manifest.insert("source_width", snapshot.final_swap.source_width);
    manifest.insert("source_height", snapshot.final_swap.source_height);
    manifest.insert("use_fxaa", snapshot.final_swap.gamma.use_fxaa);
    manifest.insert("use_pwl_gamma", snapshot.final_swap.gamma.use_pwl_gamma_ramp);
    manifest.insert("is_8bpc", snapshot.final_swap.gamma.is_8bpc);
    std::ofstream file(root / "manifest.toml");
    file << manifest;
    file.close();
    if (!file)
        throw std::runtime_error("could not save live frame input manifest");
}

} // namespace rerevved::gpu::diagnostics
