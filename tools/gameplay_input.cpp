#include "gameplay_input.h"

#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <type_traits>

#include <rex/crypto/sha256.h>
#include <toml++/toml.hpp>

#include "gpu/d3d12/native_gameplay_frame.h"
#include "gpu/d3d12/native_gameplay_render_frame.h"
#include "gpu/d3d12/native_gameplay_shaders.h"

namespace
{

using namespace rex::graphics::diagnostic;
using Node = toml::node_view<const toml::node>;

std::uint64_t number(Node node)
{
    const auto value = node.value<std::int64_t>();
    if (!value || *value < 0)
        throw std::runtime_error("missing or negative capture integer");
    return static_cast<std::uint64_t>(*value);
}

std::uint32_t word(Node node)
{
    const auto value = number(node);
    if (value > UINT32_MAX)
        throw std::runtime_error("capture integer exceeds a guest word");
    return static_cast<std::uint32_t>(value);
}

bool boolean(Node node)
{
    const auto value = node.value<bool>();
    if (!value)
        throw std::runtime_error("missing capture boolean");
    return *value;
}

std::string string(Node node)
{
    const auto value = node.value<std::string>();
    if (!value)
        throw std::runtime_error("missing capture string");
    return *value;
}

std::uint64_t hash(Node node)
{
    const auto    text   = string(node);
    std::uint64_t value  = 0;
    const auto    result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (text.size() != 16 || result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::runtime_error("invalid capture shader identity");
    return value;
}

const toml::array& array(Node node, std::size_t maximum)
{
    const auto* values = node.as_array();
    if (!values || values->size() > maximum)
        throw std::runtime_error("missing or oversized capture array");
    return *values;
}

std::filesystem::path contained(const std::filesystem::path& root, Node node)
{
    const std::filesystem::path name(string(node));
    if (name.empty() || name.is_absolute())
        throw std::runtime_error("capture input must be a relative path");
    const auto path     = std::filesystem::canonical(root / name);
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        throw std::runtime_error("capture input escapes its recipe directory");
    return path;
}

struct Reader
{
    std::filesystem::path root;
    std::size_t           remaining            = 512U * 1024U * 1024U;
    bool                  compactRegisters     = false;
    std::size_t           registerPayloadBytes = 0;
    using VertexKey                            = std::tuple<std::filesystem::path, std::uint64_t, std::uint64_t, std::string>;
    std::map<VertexKey, std::shared_ptr<const std::vector<std::uint8_t>>> vertices;
    std::ifstream                                                         file;
    std::filesystem::path                                                 openFile;
    std::uint64_t                                                         openFileSize = 0;
    std::vector<std::uint32_t>                                            previousRegisters;

    std::shared_ptr<const std::vector<std::uint8_t>> vertex(Node node)
    {
        const auto      path = contained(root, node["file"]);
        const VertexKey key{ path, node["offset"] ? number(node["offset"]) : 0, number(node["bytes"]), string(node["sha256"]) };
        const auto      found = vertices.find(key);
        if (found != vertices.end())
        {
            return found->second;
        }
        auto owned = std::make_shared<const std::vector<std::uint8_t>>(bytes(node));
        vertices.emplace(key, owned);
        return owned;
    }

    std::vector<std::uint8_t> bytes(Node node)
    {
        const auto path = contained(root, node["file"]);
        if (openFile != path)
        {
            file.close();
            file.clear();
            file.open(path, std::ios::binary);
            if (!file)
                throw std::runtime_error("could not open capture payload");
            openFile     = path;
            openFileSize = std::filesystem::file_size(path);
        }
        const auto size   = number(node["bytes"]);
        const auto offset = node["offset"] ? number(node["offset"]) : 0;
        if ((!node["offset"] && size != openFileSize) || offset > openFileSize || size > openFileSize - offset || size > remaining)
            throw std::runtime_error("capture input exceeds its declared or aggregate byte bound");
        remaining -= static_cast<std::size_t>(size);
        std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
        file.clear();
        file.seekg(static_cast<std::streamoff>(offset));
        if (!file.read(reinterpret_cast<char*>(result.data()), result.size()))
            throw std::runtime_error("could not read capture payload");
        const auto view = result.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(result.data()), result.size());
        if (rex::crypto::sha256(view) != string(node["sha256"]))
            throw std::runtime_error("capture payload digest mismatch");
        return result;
    }

    std::vector<std::uint32_t> words(Node node)
    {
        const auto data = bytes(node);
        if (data.size() % 4)
            throw std::runtime_error("capture word payload is unaligned");
        std::vector<std::uint32_t> result(data.size() / 4);
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = data[i * 4] | (std::uint32_t(data[i * 4 + 1]) << 8) |
                        (std::uint32_t(data[i * 4 + 2]) << 16) | (std::uint32_t(data[i * 4 + 3]) << 24);
        return result;
    }

    std::vector<std::uint32_t> registers(Node node)
    {
        const auto before = remaining;
        auto       values = words(node);
        registerPayloadBytes += before - remaining;
        if (node["register_delta"].value_or(false))
        {
            if (previousRegisters.empty() || number(node["register_word_count"]) != previousRegisters.size() ||
                values.size() % 2 || (!compactRegisters && previousRegisters.size() * 4 > remaining))
                throw std::runtime_error("invalid or oversized register delta");
            if (!compactRegisters)
                remaining -= previousRegisters.size() * 4;
            for (std::size_t index = 0; index < values.size(); index += 2)
            {
                if (values[index] >= previousRegisters.size() || (index && values[index] <= values[index - 2]))
                    throw std::runtime_error("register delta indices are invalid or unordered");
                previousRegisters[values[index]] = values[index + 1];
            }
            return previousRegisters;
        }
        previousRegisters = values;
        return values;
    }
};

DrawCaptureAvailability availability(Node node)
{
    const auto value = word(node);
    if (value > static_cast<unsigned>(DrawCaptureAvailability::kComplete))
        throw std::runtime_error("invalid capture availability");
    return static_cast<DrawCaptureAvailability>(value);
}

DrawCaptureReason reason(Node node)
{
    const auto value = word(node);
    if (value > static_cast<unsigned>(DrawCaptureReason::kSignMaskUnused))
        throw std::runtime_error("invalid capture availability reason");
    return static_cast<DrawCaptureReason>(value);
}

void status(Node node, DrawCaptureAvailability& availabilityValue, DrawCaptureReason& reasonValue)
{
    availabilityValue = availability(node["availability"]);
    reasonValue       = reason(node["unavailable_reason"]);
}

template <class Snapshot = DrawCaptureFrameSnapshot>
Snapshot loadSnapshot(const std::filesystem::path& path)
{
    Reader reader{ std::filesystem::canonical(path).parent_path() };
    reader.compactRegisters = std::is_same_v<Snapshot, rerevved::gpu::NativeGameplaySnapshot>;
    if (reader.compactRegisters)
        reader.remaining -= 2 * rerevved::gpu::NativeGameplayRegisterWords * sizeof(std::uint32_t);
    const auto size = std::filesystem::file_size(path);
    if (size > reader.remaining)
        throw std::runtime_error("capture manifest exceeds its byte bound");
    reader.remaining -= static_cast<std::size_t>(size);
    const auto table = toml::parse_file(path.string());
    Snapshot   snapshot;
    const auto storedSchema = word(table["schema_version"]);
    if (storedSchema != kDrawCaptureFrameSchemaVersion && storedSchema != 3)
        throw std::runtime_error("unsupported gameplay input schema");
    snapshot.schema_version   = kDrawCaptureFrameSchemaVersion;
    snapshot.start_frame      = number(table["start_frame"]);
    snapshot.end_frame        = number(table["end_frame"]);
    snapshot.start_submission = number(table["start_submission"]);
    snapshot.end_submission   = number(table["end_submission"]);
    const auto readEvents     = [&](const toml::array& entries)
    {
        for (const auto& entry : entries)
        {
            const Node            row(&entry);
            DrawCaptureFrameEvent event;
            event.id         = number(row["id"]);
            event.frame      = number(row["frame"]);
            event.submission = number(row["submission"]);
            const auto kind  = string(row["kind"]);
            if (kind != "draw" && kind != "copy" && kind != "swap")
                throw std::runtime_error("invalid capture event kind");
            event.kind                          = kind == "draw" ? DrawCaptureFrameEvent::Kind::kDraw : kind == "copy" ? DrawCaptureFrameEvent::Kind::kCopy
                                                                                                                       : DrawCaptureFrameEvent::Kind::kSwap;
            event.success                       = boolean(row["success"]);
            event.host_issued                   = boolean(row["host_issued"]);
            event.execution_status              = static_cast<DrawCaptureExecutionStatus>(word(row["execution_status"]));
            event.execution_reason              = reason(row["execution_reason"]);
            event.copy_mode                     = string(row["copy_mode"]);
            event.frontbuffer_ptr               = number(row["frontbuffer"]);
            event.frontbuffer_width             = word(row["frontbuffer_width"]);
            event.frontbuffer_height            = word(row["frontbuffer_height"]);
            event.draw.guest_vertex_shader_hash = hash(row["guest_vertex_shader_hash"]);
            event.draw.guest_pixel_shader_hash  = hash(row["guest_pixel_shader_hash"]);
            status(row["draw_acquisition"], event.draw.acquisition, event.draw.acquisition_reason);
            event.acquisition                    = availability(row["acquisition"]);
            event.acquisition_reason             = reason(row["acquisition_reason"]);
            event.draw.texture_bindings_complete = boolean(row["texture_bindings_complete"]);
            auto&      draw                      = event.draw.draw;
            const auto geometry                  = row["geometry"];
            draw.primitive_type                  = word(geometry["primitive_type"]);
            draw.requested_index_count           = word(geometry["requested_index_count"]);
            draw.guest_primitive_type            = word(geometry["guest_primitive_type"]);
            draw.host_primitive_type             = word(geometry["host_primitive_type"]);
            draw.host_vertex_shader_type         = word(geometry["host_vertex_shader_type"]);
            draw.tessellation_mode               = word(geometry["tessellation_mode"]);
            draw.host_primitive_reset_enabled    = boolean(geometry["host_primitive_reset_enabled"]);
            draw.guest_draw_vertex_count         = word(geometry["guest_draw_vertex_count"]);
            draw.host_draw_vertex_count          = word(geometry["host_draw_vertex_count"]);
            draw.guest_index_base                = word(geometry["guest_index_base"]);
            draw.native_topology                 = word(geometry["native_topology"]);
            draw.native_index_count              = word(geometry["native_index_count"]);
            draw.native_instance_count           = word(geometry["native_instance_count"]);
            draw.native_start_vertex             = word(geometry["native_start_vertex"]);
            draw.native_start_index              = word(geometry["native_start_index"]);
            const auto baseVertex                = geometry["native_base_vertex"].value<std::int64_t>();
            if (!baseVertex || *baseVertex < INT32_MIN || *baseVertex > INT32_MAX)
                throw std::runtime_error("invalid capture base vertex");
            draw.native_base_vertex    = static_cast<std::int32_t>(*baseVertex);
            draw.native_start_instance = word(geometry["native_start_instance"]);
            draw.half_pixel_offset     = boolean(geometry["half_pixel_offset"]);
            draw.registers             = reader.registers(row["registers"]);
            draw.vertex_ucode          = reader.words(row["vertex_ucode"]);
            draw.pixel_ucode           = reader.words(row["pixel_ucode"]);
            for (const auto& fetchEntry : array(row["vertex_fetches"], 96))
            {
                const Node             fetch(&fetchEntry);
                DrawCaptureVertexFetch value;
                value.fetch_constant = word(fetch["fetch_constant"]);
                value.base           = word(fetch["base"]);
                value.shared_bytes   = reader.vertex(fetch["data"]);
                status(fetch["status"], value.availability, value.unavailable_reason);
                draw.vertex_fetches.push_back(std::move(value));
            }
            const auto index      = row["index"];
            draw.index.present    = boolean(index["present"]);
            draw.index.guest_base = word(index["guest_base"]);
            draw.index.format     = word(index["format"]);
            draw.index.endianness = word(index["endianness"]);
            draw.index.count      = word(index["count"]);
            draw.index.bytes      = reader.bytes(index["data"]);
            draw.index.dma_count  = index["dma_count"] ? word(index["dma_count"]) : draw.index.count;
            draw.index.dma_length = index["dma_length"] ? word(index["dma_length"]) : static_cast<std::uint32_t>(draw.index.bytes.size());
            status(index["status"], draw.index.availability, draw.index.unavailable_reason);
            for (const auto& useEntry : array(row["texture_uses"], 128))
            {
                const Node            use(&useEntry);
                DrawCaptureTextureUse value;
                value.texture_index        = word(use["texture_index"]);
                value.stage                = use["stage"] ? word(use["stage"]) : 0;
                value.shader_binding_index = use["shader_binding_index"] ? word(use["shader_binding_index"]) : 0;
                value.fetch_constant       = word(use["fetch_constant"]);
                value.dimension            = word(use["dimension"]);
                value.sampler_value        = word(use["sampler_value"]);
                value.component_mapping    = word(use["component_mapping"]);
                const auto signs           = word(use["swizzled_signs"]);
                if (signs > UINT8_MAX)
                    throw std::runtime_error("invalid capture texture signs");
                value.swizzled_signs = static_cast<std::uint8_t>(signs);
                value.is_signed      = boolean(use["is_signed"]);
                value.required       = boolean(use["required"]);
                value.data_available = boolean(use["data_available"]);
                status(use["status"], value.availability, value.unavailable_reason);
                event.draw.texture_uses.push_back(value);
            }
            if constexpr (std::is_same_v<Snapshot, rerevved::gpu::NativeGameplaySnapshot>)
                snapshot.events.push_back(std::move(event), reader.remaining + reader.registerPayloadBytes);
            else
                snapshot.events.push_back(std::move(event));
        }
    };
    if (storedSchema == 3)
    {
        if (table.contains("events") || !boolean(table["complete"]))
            throw std::runtime_error("batched input frame is incomplete or ambiguous");
        std::uint64_t ordinal = 0, previousId = 0;
        for (const auto& entry : array(table["event_batches"], reader.remaining / 32))
        {
            const Node row(&entry);
            if (number(row["ordinal"]) != ++ordinal)
                throw std::runtime_error("input batches are missing or out of order");
            const auto  data   = reader.bytes(row);
            const auto  batch  = toml::parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
            const auto& events = array(batch["events"], rerevved::gpu::NativeGameplayBatchEvents);
            if (events.empty() || events.size() != number(row["event_count"]) ||
                number(Node(&events.front())["id"]) != number(row["first_event"]) ||
                number(Node(&events.back())["id"]) != number(row["last_event"]) ||
                number(row["first_event"]) <= previousId)
                throw std::runtime_error("input batch event extent is invalid or unordered");
            reader.previousRegisters.clear();
            readEvents(events);
            previousId = number(row["last_event"]);
        }
        if (!ordinal || snapshot.events.size() != number(table["event_count"]))
            throw std::runtime_error("input batch frame is missing events");
    }
    else
        readEvents(array(table["events"], rerevved::gpu::NativeGameplayBatchEvents));
    for (const auto& entry : array(table["textures"], 4096))
    {
        const Node         row(&entry);
        DrawCaptureTexture texture;
        if (word(row["index"]) != snapshot.textures.size())
            throw std::runtime_error("capture texture identity is out of order");
        texture.host_format              = word(row["host_format"]);
        texture.host_width               = word(row["host_width"]);
        texture.host_height              = word(row["host_height"]);
        texture.host_depth_or_array_size = word(row["host_depth_or_array_size"]);
        texture.host_mip_levels          = word(row["host_mip_levels"]);
        texture.mip_level                = word(row["mip_level"]);
        texture.row_size                 = word(row["row_size"]);
        texture.row_count                = word(row["row_count"]);
        texture.scaled_resolve           = boolean(row["scaled_resolve"]);
        if (row["guest_base_page"])
        {
            texture.guest_base_page           = word(row["guest_base_page"]);
            texture.guest_mip_page            = word(row["guest_mip_page"]);
            texture.guest_width               = word(row["guest_width"]);
            texture.guest_height              = word(row["guest_height"]);
            texture.guest_depth_or_array_size = word(row["guest_depth_or_array_size"]);
            texture.guest_pitch               = word(row["guest_pitch"]);
            texture.guest_format              = word(row["guest_format"]);
            texture.guest_endianness          = word(row["guest_endianness"]);
            texture.guest_tiled               = boolean(row["guest_tiled"]);
            texture.guest_packed_mips         = boolean(row["guest_packed_mips"]);
            texture.texture_instance_id       = number(row["texture_instance_id"]);
            texture.content_generation        = number(row["content_generation"]);
        }
        status(row["status"], texture.availability, texture.unavailable_reason);
        if (row["subresources"])
            for (const auto& subEntry : array(row["subresources"], 6 * 13))
            {
                const Node                    sub(&subEntry);
                DrawCaptureTextureSubresource value;
                value.mip_level   = word(sub["mip_level"]);
                value.array_slice = word(sub["array_slice"]);
                value.host_width  = word(sub["host_width"]);
                value.host_height = word(sub["host_height"]);
                value.row_size    = word(sub["row_size"]);
                value.row_count   = word(sub["row_count"]);
                value.bytes       = reader.bytes(sub["data"]);
                texture.subresources.push_back(std::move(value));
            }
        else
            texture.bytes = reader.bytes(row["data"]);
        snapshot.textures.push_back(std::move(texture));
    }
    const auto swap                     = table["final_swap"];
    snapshot.final_swap.frontbuffer_ptr = number(swap["frontbuffer"]);
    snapshot.final_swap.output_format   = word(swap["output_format"]);
    snapshot.final_swap.output_width    = word(swap["output_width"]);
    snapshot.final_swap.output_height   = word(swap["output_height"]);
    status(swap["source_status"], snapshot.final_swap.source_availability, snapshot.final_swap.source_unavailable_reason);
    status(swap["output_status"], snapshot.final_swap.output_availability, snapshot.final_swap.output_unavailable_reason);
    auto& gamma              = snapshot.final_swap.gamma;
    gamma.gamma_256          = reader.words(swap["gamma"]);
    gamma.use_pwl_gamma_ramp = boolean(swap["use_pwl_gamma_ramp"]);
    gamma.use_fxaa           = boolean(swap["use_fxaa"]);
    gamma.is_8bpc            = boolean(swap["is_8bpc"]);
    if (storedSchema == 3 && (snapshot.start_frame >= snapshot.end_frame ||
                              snapshot.events.front().kind != DrawCaptureFrameEvent::Kind::kSwap ||
                              snapshot.events.back().kind != DrawCaptureFrameEvent::Kind::kSwap ||
                              snapshot.events.back().frontbuffer_ptr != snapshot.final_swap.frontbuffer_ptr))
        throw std::runtime_error("batched input frame has no complete swap boundary");
    if constexpr (std::is_same_v<Snapshot, rerevved::gpu::NativeGameplaySnapshot>)
        if (snapshot.events.StoredBytes() > reader.remaining + reader.registerPayloadBytes)
            throw std::runtime_error("compact input records and payloads exceed the owned byte bound");
    return snapshot;
}

} // namespace

bool LoadGameplayCaptureInputs(const std::filesystem::path&                         path,
                               rex::graphics::diagnostic::DrawCaptureFrameSnapshot& snapshot,
                               std::string&                                         error)
{
    snapshot = {};
    error.clear();
    try
    {
        snapshot = loadSnapshot(path);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool LoadGameplayCaptureInputs(const std::filesystem::path&           path,
                               rerevved::gpu::NativeGameplaySnapshot& snapshot,
                               std::string&                           error)
{
    snapshot = {};
    error.clear();
    try
    {
        snapshot = loadSnapshot<rerevved::gpu::NativeGameplaySnapshot>(path);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool LoadGameplayCaptureRecipe(const std::filesystem::path&            path,
                               rerevved::gpu::NativeFrameReplayRecipe& recipe,
                               std::string&                            error)
{
    recipe = {};
    error.clear();
    try
    {
        const auto root = std::filesystem::canonical(path).parent_path();
        if (std::filesystem::file_size(path) > 1024U * 1024U)
            throw std::runtime_error("gameplay recipe exceeds its metadata bound");
        const auto table = toml::parse_file(path.string());
        if (word(table["gameplay_frame_schema_version"]) != 1)
            throw std::runtime_error("unsupported gameplay replay recipe schema");
        const auto                                                input   = contained(root, table["input"]);
        const auto                                                catalog = contained(root, table["shaders"]);
        std::vector<rerevved::gpu::NativeGameplayShaderInterface> shaders;
        if (!rerevved::gpu::LoadNativeGameplayShaders(catalog, shaders, error))
            return false;
        const auto                                 snapshot = loadSnapshot(input);
        rerevved::gpu::NativeGameplayFrameCoverage coverage;
        return rerevved::gpu::BuildNativeGameplayFrameRecipe(snapshot, shaders, recipe, coverage, error);
    }
    catch (const std::exception& exception)
    {
        recipe = {};
        error  = exception.what();
        return false;
    }
}

bool LoadGameplayRenderFrameRecipe(const std::filesystem::path&      path,
                                   rerevved::gpu::NativeRenderFrame& frame,
                                   std::string&                      error,
                                   rerevved::gpu::NativeRenderFrame* producer)
{
    frame = {};
    error.clear();
    try
    {
        const auto root = std::filesystem::canonical(path).parent_path();
        if (std::filesystem::file_size(path) > 1024U * 1024U)
            throw std::runtime_error("gameplay recipe exceeds its metadata bound");
        const auto table = toml::parse_file(path.string());
        if (word(table["gameplay_render_frame_schema_version"]) != 1)
            throw std::runtime_error("unsupported complete gameplay replay recipe schema");
        const auto                                                input   = contained(root, table["input"]);
        const auto                                                catalog = contained(root, table["shaders"]);
        std::vector<rerevved::gpu::NativeGameplayShaderInterface> shaders;
        if (!rerevved::gpu::LoadNativeGameplayShaders(catalog, shaders, error))
            return false;
        const auto                                   snapshot = loadSnapshot<rerevved::gpu::NativeGameplaySnapshot>(input);
        rerevved::gpu::NativeGameplayRetainedTexture retained;
        if (table.contains("producer_input"))
        {
            if (!producer)
                throw std::runtime_error("producer replay requires a native producer submission");
            const auto prior         = contained(root, table["producer_input"]);
            const auto priorMetadata = toml::parse_file(prior.string());
            if (priorMetadata["input_origin"].value_or<std::string>("") != "guest_cpu_observer")
                throw std::runtime_error("producer replay requires guest CPU observer inputs");
            if (!rerevved::gpu::BuildNativeGameplayProducer(loadSnapshot<rerevved::gpu::NativeGameplaySnapshot>(prior), shaders, {}, word(table["producer_page"]), *producer, retained, error))
                return false;
        }
        return rerevved::gpu::BuildNativeGameplayRenderFrame(snapshot, shaders, {}, frame, error, retained.guestBasePage ? &retained : nullptr);
    }
    catch (const std::exception& exception)
    {
        frame = {};
        error = exception.what();
        return false;
    }
}
