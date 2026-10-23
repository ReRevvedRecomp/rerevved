#include "solid_draw_reference.h"

#include "gpu/d3d12/native_menu_panel.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string_view>
#include <toml++/toml.hpp>
#include <utility>

namespace rerevved::gpu::guest_d3d
{
namespace
{

using namespace rex::graphics::diagnostic;

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("adapter_reference_word_extent");
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

std::vector<std::uint8_t> words(std::span<const std::uint32_t> source)
{
    std::vector<std::uint8_t> bytes(source.size_bytes());
    for (std::size_t i = 0; i < source.size(); ++i)
        for (unsigned c = 0; c < 4; ++c)
            bytes[i * 4 + c] = static_cast<std::uint8_t>(source[i] >> (24 - c * 8));
    return bytes;
}

void write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.close();
    if (!file)
        throw std::runtime_error("adapter_reference_save_failed");
}

std::vector<std::uint8_t> read(const std::filesystem::path& path)
{
    const auto size = std::filesystem::file_size(path);
    if (size > 16 * 1024 * 1024)
        throw std::runtime_error("adapter_reference_file_extent");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream             file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("adapter_reference_file_read");
    return bytes;
}

// The SDK schema uses objects, arrays, ASCII strings, integers and booleans.
// Keep a bounded reader here because the runtime package has no JSON parser.
struct Json
{
    enum class Kind
    {
        Object,
        Array,
        String,
        Integer,
        Boolean,
        Null
    } kind = Kind::Null;
    std::map<std::string, Json> fields;
    std::vector<Json>           values;
    std::string                 text;

    const Json& at(const std::string& key) const
    {
        if (kind != Kind::Object || !fields.contains(key))
            throw std::runtime_error("adapter_reference_manifest_field: " + key);
        return fields.at(key);
    }

    std::uint64_t number(std::uint64_t maximum = UINT32_MAX) const
    {
        std::uint64_t result = 0;
        const auto    parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (kind != Kind::Integer || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || result > maximum)
            throw std::runtime_error("adapter_reference_manifest_integer");
        return result;
    }

    std::int32_t signedWord() const
    {
        std::int32_t result = 0;
        const auto   parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (kind != Kind::Integer || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            throw std::runtime_error("adapter_reference_manifest_signed_integer");
        return result;
    }

    bool boolean() const
    {
        if (kind != Kind::Boolean)
            throw std::runtime_error("adapter_reference_manifest_boolean");
        return text == "true";
    }

    bool is(const char* value) const
    {
        return kind == Kind::String && text == value;
    }
};

struct JsonReader
{
    std::string_view text;
    std::size_t      cursor = 0;

    void space()
    {
        while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\r' || text[cursor] == '\n' || text[cursor] == '\t'))
            ++cursor;
    }

    bool take(char c)
    {
        space();
        if (cursor == text.size() || text[cursor] != c)
            return false;
        ++cursor;
        return true;
    }

    void expect(char c)
    {
        if (!take(c))
            throw std::runtime_error("adapter_reference_manifest_syntax");
    }

    std::string string()
    {
        expect('"');
        std::string result;
        while (cursor < text.size())
        {
            char c = text[cursor++];
            if (c == '"')
                return result;
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
                throw std::runtime_error("adapter_reference_manifest_string");
            if (c == '\\')
            {
                if (cursor == text.size())
                    break;
                c = text[cursor++];
                switch (c)
                {
                    case '"':
                    case '\\':
                    case '/':
                        break;
                    case 'b':
                        c = '\b';
                        break;
                    case 'f':
                        c = '\f';
                        break;
                    case 'n':
                        c = '\n';
                        break;
                    case 'r':
                        c = '\r';
                        break;
                    case 't':
                        c = '\t';
                        break;
                    default:
                        throw std::runtime_error("adapter_reference_manifest_escape");
                }
            }
            result += c;
        }
        throw std::runtime_error("adapter_reference_manifest_string");
    }

    Json value(unsigned depth = 0)
    {
        space();
        if (depth > 16 || cursor == text.size())
            throw std::runtime_error("adapter_reference_manifest_depth");
        Json result;
        if (take('{'))
        {
            result.kind = Json::Kind::Object;
            if (!take('}'))
            {
                do
                {
                    auto key = string();
                    expect(':');
                    if (!result.fields.emplace(std::move(key), value(depth + 1)).second)
                        throw std::runtime_error("adapter_reference_manifest_duplicate");
                } while (take(','));
                expect('}');
            }
        }
        else if (take('['))
        {
            result.kind = Json::Kind::Array;
            if (!take(']'))
            {
                do
                {
                    result.values.push_back(value(depth + 1));
                } while (take(','));
                expect(']');
            }
        }
        else if (text[cursor] == '"')
        {
            result.kind = Json::Kind::String;
            result.text = string();
        }
        else
        {
            const auto start = cursor;
            while (cursor < text.size() && text[cursor] != ',' && text[cursor] != '}' && text[cursor] != ']' &&
                   text[cursor] != ' ' && text[cursor] != '\r' && text[cursor] != '\n' && text[cursor] != '\t')
                ++cursor;
            result.text = text.substr(start, cursor - start);
            if (result.text == "true" || result.text == "false")
                result.kind = Json::Kind::Boolean;
            else if (result.text != "null")
            {
                result.kind       = Json::Kind::Integer;
                const auto digits = std::string_view(result.text).substr(result.text.starts_with('-') ? 1 : 0);
                if (digits.empty() || (digits.size() > 1 && digits.front() == '0') ||
                    !std::all_of(digits.begin(), digits.end(), [](char c)
                                 {
                                     return c >= '0' && c <= '9';
                                 }))
                    throw std::runtime_error("adapter_reference_manifest_number");
            }
        }
        return result;
    }
};

std::vector<std::uint32_t> littleWords(const std::filesystem::path& directory, const Json& metadata, const char* name)
{
    if (!metadata.at("file").is(name))
        throw std::runtime_error("adapter_reference_manifest_filename");
    const auto bytes = read(directory / name);
    if (bytes.size() != metadata.at("dword_count").number() * 4)
        throw std::runtime_error("adapter_reference_manifest_word_extent");
    std::vector<std::uint32_t> result(bytes.size() / 4);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = std::uint32_t(bytes[i * 4]) | (std::uint32_t(bytes[i * 4 + 1]) << 8) |
                    (std::uint32_t(bytes[i * 4 + 2]) << 16) | (std::uint32_t(bytes[i * 4 + 3]) << 24);
    return result;
}

std::vector<std::uint8_t> payload(const std::filesystem::path& directory, const Json& metadata, const char* name)
{
    if (!metadata.at("file").is(name))
        throw std::runtime_error("adapter_reference_manifest_filename");
    auto bytes = read(directory / name);
    if (bytes.size() != metadata.at("size_bytes").number())
        throw std::runtime_error("adapter_reference_manifest_payload_extent");
    return bytes;
}

} // namespace

bool CorrelateSolidDrawReference(const DrawCaptureSnapshot& reference, const SolidDrawInputs& inputs, const SolidDrawRecipe& recipe, std::uint32_t vertexOutput, std::uint32_t indexOutput, SolidReferenceCorrelation& correlation, std::string& error)
{
    correlation = {};
    error.clear();
    try
    {
        if (reference.registers.size() != 0x5003 || reference.vertex_fetches.size() != 1 ||
            reference.vertex_fetches[0].fetch_constant != 95 || !reference.index.present ||
            reference.index.count != inputs.arguments.indexCount || reference.index.format != 0 ||
            reference.index.endianness != 1 || !reference.half_pixel_offset || reference.used_texture_mask ||
            reference.primitive_type != 4 || !reference.color_target_written ||
            reference.native_instance_count != 1 || reference.native_start_vertex || reference.native_start_index ||
            reference.native_base_vertex || reference.native_start_instance || reference.tessellation_mode ||
            reference.host_primitive_reset_enabled)
            throw std::runtime_error("adapter_reference_draw_tuple");
        if (recipe.patchedVertexCode.empty() || inputs.pixelCode.empty() ||
            words(reference.vertex_ucode) != recipe.patchedVertexCode || words(reference.pixel_ucode) != inputs.pixelCode)
            throw std::runtime_error("adapter_reference_foreign_shaders");
        const auto& fetch = reference.vertex_fetches[0];
        // Compare the submitted prefixes; fetch capacity and DMA padding are
        // retained in the artifacts but are not shader-visible geometry.
        if (inputs.vertices.empty() || inputs.indices.empty() ||
            fetch.availability != DrawCaptureAvailability::kComplete || reference.index.availability != DrawCaptureAvailability::kComplete ||
            fetch.Payload().size() < inputs.vertices.size() ||
            !std::equal(inputs.vertices.begin(), inputs.vertices.end(), fetch.Payload().begin()) ||
            reference.index.bytes.size() < inputs.indices.size() ||
            !std::equal(inputs.indices.begin(), inputs.indices.end(), reference.index.bytes.begin()))
            throw std::runtime_error("adapter_reference_foreign_geometry");
        for (const auto mapping : kSolidFixedWords)
            if (reference.registers[mapping.hardware] != word(recipe.effectiveState, mapping.shadow))
                throw std::runtime_error("adapter_reference_foreign_fixed_state");
        for (const auto group : kSolidReferenceConstants)
            for (unsigned i = 0; i < group[2]; ++i)
                if (reference.registers[group[1] + i] != word(inputs.state, group[0] + i * 4))
                    throw std::runtime_error("adapter_reference_foreign_referenced_constants");
        const auto alias = [](std::uint32_t raw)
        {
            return (raw & 0x1FFFFFFFU) + (((raw >> 20) + 512U) & 0x1000U);
        };
        correlation.adapterVertexBase   = alias(vertexOutput);
        correlation.adapterIndexBase    = alias(indexOutput);
        correlation.referenceVertexBase = fetch.base;
        correlation.referenceIndexBase  = reference.index.guest_base;
        correlation.addressCorrelated   = fetch.base == correlation.adapterVertexBase &&
                                          reference.index.guest_base == correlation.adapterIndexBase;
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool ProcessSolidDrawReference(const DrawCaptureSnapshot& reference, const SolidDrawInputs& inputs, const SolidDrawRecipe& recipe, std::uint32_t vertexOutput, std::uint32_t indexOutput, const std::filesystem::path& output, bool offline, std::string& error)
{
    try
    {
        SolidReferenceCorrelation correlation;
        if (!CorrelateSolidDrawReference(reference, inputs, recipe, vertexOutput, indexOutput, correlation, error))
            return false;
        std::vector<std::uint8_t> before, after;
        if (reference.edram_before.size() != 10 * 1024 * 1024 || reference.edram_after.size() != 10 * 1024 * 1024)
            throw std::runtime_error("adapter_reference_edram_extent");
        if (!DecodeNativeMenuPanelColor(reference.edram_before, reference.registers, before, error) ||
            !DecodeNativeMenuPanelColor(reference.edram_after, reference.registers, after, error))
            return false;
        if (!std::filesystem::create_directory(output))
            throw std::runtime_error("adapter_reference_output_not_fresh");
        write(output / "registers.be.bin", words(reference.registers));
        write(output / "vs.be.bin", words(reference.vertex_ucode));
        write(output / "ps.be.bin", words(reference.pixel_ucode));
        write(output / "vertices.be.bin", reference.vertex_fetches[0].Payload());
        write(output / "indices.be.bin", reference.index.bytes);
        write(output / "edram-before.bin", reference.edram_before);
        write(output / "edram-after.bin", reference.edram_after);
        write(output / "before.samples", before);
        write(output / "after.samples", after);
        std::ofstream metadata(output / "event.toml");
        metadata << toml::table{
            { "reference_schema", 1 }, { "complete", true }, { "offline", offline }, { "frame", static_cast<std::int64_t>(reference.frame) }, { "submission", static_cast<std::int64_t>(reference.submission) }, { "window_offset", reference.registers[0x2080] }, { "width", 640 }, { "height", 720 }, { "sample_planes", 2 }, { "geometry_and_inline_constants_correlated", true }, { "content_correlated", true }, { "constant_policy", "VS c0-c15, PS c0-c223, all 8 boolean words" }, { "address_correlated", correlation.addressCorrelated }, { "adapter_vertex_base", correlation.adapterVertexBase }, { "adapter_index_base", correlation.adapterIndexBase }, { "reference_vertex_base", correlation.referenceVertexBase }, { "reference_index_base", correlation.referenceIndexBase }, { "half_pixel_offset", reference.half_pixel_offset }, { "half_pixel_offset_source", offline ? "register plus assumed SDK default true" : "mailbox snapshot" }
        };
        metadata.close();
        if (!metadata)
            throw std::runtime_error("adapter_reference_metadata_save");
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool LoadSdkSolidDrawReference(const std::filesystem::path& directory, DrawCaptureSnapshot& reference, std::string& error)
{
    reference = {};
    error.clear();
    try
    {
        if (std::filesystem::file_size(directory / "manifest.json") > 1024 * 1024)
            throw std::runtime_error("adapter_reference_manifest_extent");
        const auto bytes = read(directory / "manifest.json");
        JsonReader reader{ std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()) };
        const auto manifest = reader.value();
        reader.space();
        if (reader.cursor != reader.text.size() || manifest.at("schema_version").number() != 1 ||
            !manifest.at("status").is("complete") || !manifest.at("valid").boolean() ||
            !manifest.at("encoding").is("uint32-little-endian"))
            throw std::runtime_error("adapter_reference_manifest_status_or_encoding");
        const auto& provenance            = manifest.at("provenance");
        reference.frame                   = provenance.at("frame").number(INT64_MAX);
        reference.submission              = provenance.at("submission").number(INT64_MAX);
        const auto& draw                  = manifest.at("draw");
        const auto& guest                 = draw.at("guest");
        const auto& processed             = draw.at("processed");
        const auto& native                = draw.at("native");
        reference.primitive_type          = guest.at("primitive_type").number();
        reference.requested_index_count   = guest.at("index_count").number();
        reference.major_mode_explicit     = guest.at("major_mode_explicit").boolean();
        reference.guest_draw_vertex_count = guest.at("guest_draw_vertex_count").number();
        reference.indexed                 = native.at("indexed").boolean();
        if (!reference.indexed || !guest.at("source").is("guest_dma"))
            throw std::runtime_error("adapter_reference_manifest_index_source");
        const auto& dma                        = guest.at("dma");
        reference.index.present                = true;
        reference.index.guest_base             = dma.at("base").number();
        reference.index.format                 = dma.at("format").number();
        reference.index.endianness             = dma.at("endianness").number();
        reference.index.count                  = reference.guest_draw_vertex_count;
        reference.index.dma_count              = dma.at("count").number();
        reference.index.dma_length             = dma.at("length_bytes").number();
        reference.tessellation_mode            = processed.at("tessellation_mode").number();
        reference.guest_primitive_type         = processed.at("guest_primitive_type").number();
        reference.host_primitive_type          = processed.at("host_primitive_type").number();
        reference.host_vertex_shader_type      = processed.at("host_vertex_shader_type").number();
        reference.host_draw_vertex_count       = processed.at("host_draw_vertex_count").number();
        reference.host_index_format            = processed.at("host_index_format").number();
        reference.host_shader_index_endian     = processed.at("host_shader_index_endian").number();
        reference.host_primitive_reset_enabled = processed.at("primitive_reset").boolean();
        reference.native_topology              = native.at("topology").number();
        reference.native_vertex_count          = native.at("vertex_count").number();
        reference.native_index_count           = native.at("index_count").number();
        reference.native_instance_count        = native.at("instance_count").number();
        reference.native_start_vertex          = native.at("start_vertex").number();
        reference.native_start_index           = native.at("start_index").number();
        reference.native_base_vertex           = native.at("base_vertex").signedWord();
        reference.native_start_instance        = native.at("start_instance").number();
        reference.used_texture_mask            = draw.at("used_texture_mask").number();
        reference.normalized_color_mask        = draw.at("normalized_color_mask").number();
        reference.color_target_written         = reference.normalized_color_mask != 0;
        const auto& scale                      = draw.at("resolution_scale");
        if (scale.kind != Json::Kind::Array || scale.values.size() != 2 ||
            scale.values[0].number() != 1 || scale.values[1].number() != 1)
            throw std::runtime_error("adapter_reference_manifest_resolution_scale");
        reference.registers    = littleWords(directory, manifest.at("registers"), "registers.bin");
        reference.vertex_ucode = littleWords(directory, manifest.at("shaders").at("vertex"), "vertex.ucode.bin");
        reference.pixel_ucode  = littleWords(directory, manifest.at("shaders").at("pixel"), "pixel.ucode.bin");
        if (reference.registers.size() != 0x5003)
            throw std::runtime_error("adapter_reference_register_extent");
        // SDK disk schema 1 omits the half_pixel_offset cvar. Reconstruction
        // assumes its default true and records that assumption in event.toml.
        reference.half_pixel_offset = !(reference.registers[0x2302] & 1);
        const auto& resources       = draw.at("resources");
        const auto& ranges          = resources.at("vertex_fetch_ranges");
        if (ranges.kind != Json::Kind::Array || ranges.values.size() != 1)
            throw std::runtime_error("adapter_reference_manifest_fetch_count");
        const auto& range    = ranges.values[0];
        auto&       fetch    = reference.vertex_fetches.emplace_back();
        fetch.fetch_constant = range.at("fetch_constant").number();
        fetch.base           = range.at("base").number();
        fetch.bytes          = payload(directory, range, "vertex_fetch_95.bin");
        const auto& index    = resources.at("index");
        if (index.at("base").number() != reference.index.guest_base)
            throw std::runtime_error("adapter_reference_manifest_index_base");
        reference.index.bytes      = payload(directory, index, "index.bin");
        reference.guest_index_base = reference.index.guest_base;
        reference.guest_index_size = static_cast<std::uint32_t>(reference.index.bytes.size());
        const auto& edram          = manifest.at("edram");
        if (!edram.at("path").is("rov") || edram.at("scale_x").number() != 1 || edram.at("scale_y").number() != 1 ||
            edram.at("snapshot_size_bytes").number() != 10 * 1024 * 1024 ||
            !edram.at("before_file").is("edram_before.bin") || !edram.at("after_file").is("edram_after.bin"))
            throw std::runtime_error("adapter_reference_manifest_edram");
        reference.edram_before = read(directory / "edram_before.bin");
        reference.edram_after  = read(directory / "edram_after.bin");
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

} // namespace rerevved::gpu::guest_d3d
