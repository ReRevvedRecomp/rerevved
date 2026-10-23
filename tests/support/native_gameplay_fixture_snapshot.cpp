#include "native_gameplay_fixture_inputs.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <format>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/crypto/sha256.h>

namespace rerevved::gpu::diagnostics
{
namespace
{

using namespace rex::graphics::diagnostic;

// The capture schema contains only integers, booleans, strings, arrays and
// tables. Flatten nested values as they are built rather than retaining a
// per-field document tree for every event in the completed frame.
void appendMetadata(std::string& destination, std::string_view text)
{
    if (text.size() > NativeGameplayLiveInputs::MaxOwnedBytes - destination.size())
    {
        throw std::runtime_error("CPU snapshot metadata exceeds the frame byte bound");
    }
    destination.append(text);
}

struct Table;
struct Array;

struct Value
{
    std::string text;

    template <std::integral T>
    Value(T number)
    {
        std::array<char, std::numeric_limits<T>::digits10 + 3> buffer;
        const auto                                             result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
        if (result.ec != std::errc{})
        {
            throw std::runtime_error("could not serialize CPU snapshot integer");
        }
        text.assign(buffer.data(), result.ptr);
    }

    Value(bool value)
    : text(value ? "true" : "false")
    {
    }

    Value(const char* value)
    : Value(std::string_view(value))
    {
    }

    Value(const std::string& value)
    : Value(std::string_view(value))
    {
    }

    Value(std::string_view value)
    {
        appendMetadata(text, "\"");
        std::size_t start = 0;
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            const auto byte = static_cast<unsigned char>(value[i]);
            if (byte >= 0x20 && byte != 0x7F && byte != '\\' && byte != '"')
            {
                continue;
            }
            appendMetadata(text, value.substr(start, i - start));
            if (byte == '\\' || byte == '"')
            {
                const char escaped[]{ '\\', static_cast<char>(byte) };
                appendMetadata(text, std::string_view(escaped, sizeof(escaped)));
            }
            else
            {
                constexpr char digits[] = "0123456789ABCDEF";
                const char     escaped[]{ '\\', 'u', '0', '0', digits[byte >> 4], digits[byte & 15] };
                appendMetadata(text, std::string_view(escaped, sizeof(escaped)));
            }
            start = i + 1;
        }
        appendMetadata(text, value.substr(start));
        appendMetadata(text, "\"");
    }

    Value(const Table& table);
    Value(Array&& array);
};

struct Table
{
    // Keys are the fixed bare identifiers in this writer, never input strings.
    std::vector<std::pair<std::string_view, Value>> fields;

    Table(std::initializer_list<std::pair<std::string_view, Value>> values)
    : fields(values)
    {
    }

    void insert(std::string_view key, Value value)
    {
        fields.emplace_back(key, std::move(value));
    }

    void insertOrAssign(std::string_view key, Value value)
    {
        for (auto& field : fields)
        {
            if (field.first == key)
            {
                field.second = std::move(value);
                return;
            }
        }
        insert(key, std::move(value));
    }

    void erase(std::string_view key)
    {
        std::erase_if(fields, [&](const auto& field)
                      {
                          return field.first == key;
                      });
    }

    std::string document() const
    {
        std::string result;
        for (const auto& [key, value] : fields)
        {
            appendMetadata(result, key);
            appendMetadata(result, "=");
            appendMetadata(result, value.text);
            appendMetadata(result, "\n");
        }
        return result;
    }
};

struct Array
{
    std::string text;
    std::size_t count = 0;

    bool empty() const
    {
        return !count;
    }

    std::size_t size() const
    {
        return count;
    }

    void pushBack(Value value)
    {
        if (count)
        {
            appendMetadata(text, ",");
        }
        appendMetadata(text, value.text);
        ++count;
    }
};

Value::Value(const Table& table)
{
    appendMetadata(text, "{");
    bool first = true;
    for (const auto& [key, value] : table.fields)
    {
        if (!first)
        {
            appendMetadata(text, ",");
        }
        first = false;
        appendMetadata(text, key);
        appendMetadata(text, "=");
        appendMetadata(text, value.text);
    }
    appendMetadata(text, "}");
}

Value::Value(Array&& array)
{
    appendMetadata(text, "[");
    appendMetadata(text, array.text);
    appendMetadata(text, "]");
}

Table status(DrawCaptureAvailability availability, DrawCaptureReason reason)
{
    return Table{ { "availability", static_cast<unsigned>(availability) },
                  { "unavailable_reason", static_cast<unsigned>(reason) } };
}

struct Writer
{
    std::filesystem::path                             directory;
    std::size_t                                       remaining = NativeGameplayLiveInputs::MaxOwnedBytes;
    std::map<std::string, std::uint64_t>              offsets;
    std::map<const std::vector<std::uint8_t>*, Table> vertexRecords;
    std::ofstream                                     payloads;
    std::vector<std::uint32_t>                        previousRegisters;

    std::uint64_t storedBytes = 0;

    explicit Writer(const std::filesystem::path& path)
    : directory(path)
    , payloads(path / "payloads.bin", std::ios::binary | std::ios::noreplace)
    {
        if (!payloads)
            throw std::runtime_error("could not create CPU snapshot payload storage");
    }

    Table bytes(std::span<const std::uint8_t> data)
    {
        const auto view           = data.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
        const auto digest         = rex::crypto::sha256(view);
        const auto offset         = storedBytes;
        const auto [entry, added] = offsets.emplace(digest, offset);
        if (added)
        {
            if (data.size() > remaining)
                throw std::runtime_error("CPU snapshot exceeds the frame byte bound");
            remaining -= data.size();
            if (!data.empty() && !payloads.write(reinterpret_cast<const char*>(data.data()), data.size()))
                throw std::runtime_error("could not write CPU snapshot payload storage");
            storedBytes += data.size();
        }
        return Table{ { "file", "payloads.bin" }, { "offset", static_cast<std::int64_t>(entry->second) }, { "bytes", static_cast<std::int64_t>(data.size()) }, { "sha256", digest } };
    }

    Table vertex(const DrawCaptureVertexFetch& fetch)
    {
        // Cursor-owned vectors change at each event; only shared payloads have
        // stable identity across the complete input frame.
        if (!fetch.shared_bytes)
            return bytes(fetch.Payload());
        const auto* identity = fetch.shared_bytes.get();
        const auto  found    = vertexRecords.find(identity);
        if (found != vertexRecords.end())
            return found->second;
        auto record = bytes(fetch.Payload());
        vertexRecords.emplace(identity, record);
        return record;
    }

    Table words(std::span<const std::uint32_t> values)
    {
        std::vector<std::uint8_t> data(values.size() * 4);
        for (std::size_t index = 0; index < values.size(); ++index)
            for (unsigned byte = 0; byte < 4; ++byte)
                data[index * 4 + byte] = static_cast<std::uint8_t>(values[index] >> (byte * 8));
        return bytes(data);
    }

    Table metadata(const std::string& name, const Table& table)
    {
        const auto text = table.document();
        if (text.size() > remaining)
            throw std::runtime_error("CPU snapshot metadata exceeds the frame byte bound");
        remaining -= text.size();
        std::ofstream file(directory / name, std::ios::out | std::ios::noreplace | std::ios::binary);
        file << text;
        file.close();
        if (!file)
            throw std::runtime_error("could not publish CPU snapshot metadata");
        return Table{ { "file", name }, { "bytes", static_cast<std::int64_t>(text.size()) }, { "sha256", rex::crypto::sha256(text) } };
    }

    Table event(const DrawCaptureFrameEvent& source)
    {
        const auto& draw = source.draw.draw;
        Array       vertices, uses;
        for (const auto& fetch : draw.vertex_fetches)
            vertices.pushBack(Table{
                { "fetch_constant", fetch.fetch_constant }, { "base", fetch.base }, { "status", status(fetch.availability, fetch.unavailable_reason) }, { "data", vertex(fetch) } });
        for (const auto& use : source.draw.texture_uses)
            uses.pushBack(Table{
                { "texture_index", use.texture_index }, { "stage", use.stage }, { "shader_binding_index", use.shader_binding_index }, { "fetch_constant", use.fetch_constant }, { "dimension", use.dimension }, { "sampler_value", use.sampler_value }, { "component_mapping", use.component_mapping }, { "swizzled_signs", use.swizzled_signs }, { "is_signed", use.is_signed }, { "required", use.required }, { "data_available", use.data_available }, { "status", status(use.availability, use.unavailable_reason) } });
        const auto& index = draw.index;
        return Table{
            { "id", static_cast<std::int64_t>(source.id) },
            { "frame", static_cast<std::int64_t>(source.frame) },
            { "submission", static_cast<std::int64_t>(source.submission) },
            { "kind", source.kind == DrawCaptureFrameEvent::Kind::kDraw ? "draw" : source.kind == DrawCaptureFrameEvent::Kind::kCopy ? "copy"
                                                                                                                                     : "swap" },
            { "success", source.success },
            { "host_issued", source.host_issued },
            { "execution_status", static_cast<unsigned>(source.execution_status) },
            { "execution_reason", static_cast<unsigned>(source.execution_reason) },
            { "copy_mode", source.copy_mode },
            { "frontbuffer", static_cast<std::int64_t>(source.frontbuffer_ptr) },
            { "frontbuffer_width", source.frontbuffer_width },
            { "frontbuffer_height", source.frontbuffer_height },
            { "guest_vertex_shader_hash", std::format("{:016X}", source.draw.guest_vertex_shader_hash) },
            { "guest_pixel_shader_hash", std::format("{:016X}", source.draw.guest_pixel_shader_hash) },
            { "draw_acquisition", status(source.draw.acquisition, source.draw.acquisition_reason) },
            { "acquisition", static_cast<unsigned>(source.acquisition) },
            { "acquisition_reason", static_cast<unsigned>(source.acquisition_reason) },
            { "texture_bindings_complete", source.draw.texture_bindings_complete },
            { "geometry", Table{ { "primitive_type", draw.primitive_type }, { "requested_index_count", draw.requested_index_count }, { "guest_primitive_type", draw.guest_primitive_type }, { "host_primitive_type", draw.host_primitive_type }, { "host_vertex_shader_type", draw.host_vertex_shader_type }, { "tessellation_mode", draw.tessellation_mode }, { "host_primitive_reset_enabled", draw.host_primitive_reset_enabled }, { "guest_draw_vertex_count", draw.guest_draw_vertex_count }, { "host_draw_vertex_count", draw.host_draw_vertex_count }, { "guest_index_base", draw.guest_index_base }, { "native_topology", draw.native_topology }, { "native_index_count", draw.native_index_count }, { "native_instance_count", draw.native_instance_count }, { "native_start_vertex", draw.native_start_vertex }, { "native_start_index", draw.native_start_index }, { "native_base_vertex", draw.native_base_vertex }, { "native_start_instance", draw.native_start_instance }, { "half_pixel_offset", draw.half_pixel_offset } } },
            { "registers", registers(draw.registers) },
            { "vertex_ucode", words(draw.vertex_ucode) },
            { "pixel_ucode", words(draw.pixel_ucode) },
            { "vertex_fetches", std::move(vertices) },
            { "index", Table{ { "present", index.present }, { "guest_base", index.guest_base }, { "format", index.format }, { "endianness", index.endianness }, { "count", index.count }, { "dma_count", index.dma_count }, { "dma_length", index.dma_length }, { "data", bytes(index.bytes) }, { "status", status(index.availability, index.unavailable_reason) } } },
            { "texture_uses", std::move(uses) },
        };
    }

    Table registers(std::span<const std::uint32_t> values)
    {
        std::vector<std::uint32_t> changes;
        if (previousRegisters.size() == values.size())
            for (std::size_t index = 0; index < values.size(); ++index)
                if (previousRegisters[index] != values[index])
                {
                    changes.push_back(static_cast<std::uint32_t>(index));
                    changes.push_back(values[index]);
                }
        const bool delta = previousRegisters.size() == values.size() && changes.size() < values.size();
        auto       row   = words(delta ? std::span<const std::uint32_t>(changes) : values);
        if (delta)
        {
            row.insert("register_delta", true);
            row.insert("register_word_count", static_cast<std::int64_t>(values.size()));
        }
        previousRegisters.assign(values.begin(), values.end());
        return row;
    }

    Table texture(std::uint32_t index, const DrawCaptureTexture& source, const NativeDrawReplayTexture* cpu)
    {
        Table row{
            { "index", index },
            { "input_origin", cpu ? "guest_cpu" : "native_producer_required" },
            { "guest_base_page", source.guest_base_page },
            { "guest_mip_page", source.guest_mip_page },
            { "guest_width", source.guest_width },
            { "guest_height", source.guest_height },
            { "guest_depth_or_array_size", source.guest_depth_or_array_size },
            { "guest_pitch", source.guest_pitch },
            { "guest_format", source.guest_format },
            { "guest_endianness", source.guest_endianness },
            { "guest_tiled", source.guest_tiled },
            { "guest_packed_mips", source.guest_packed_mips },
            { "texture_instance_id", 0 },
            { "content_generation", 0 },
            { "host_format", 0 },
            { "host_width", 0 },
            { "host_height", 0 },
            { "host_depth_or_array_size", 1 },
            { "host_mip_levels", 1 },
            { "mip_level", 0 },
            { "row_size", 0 },
            { "row_count", 0 },
            { "scaled_resolve", false },
            { "status", status(cpu ? DrawCaptureAvailability::kComplete : DrawCaptureAvailability::kUnavailable, cpu ? DrawCaptureReason::kNone : DrawCaptureReason::kTextureSource) },
        };
        if (!cpu)
        {
            row.insert("data", bytes({}));
            return row;
        }
        // The schema uses DXGI layout identifiers. These bytes were decoded
        // from guest CPU memory; no original-renderer texture is read here.
        std::uint32_t format = 0, block = 1, unit = 0;
        switch (cpu->format)
        {
            case NativeDrawReplayTextureFormat::Rgba8:
                format = 27;
                unit   = 4;
                break;
            case NativeDrawReplayTextureFormat::Rgb10a2:
                // RGB10 currently enters through native resolves, not CPU uploads.
                break;
            case NativeDrawReplayTextureFormat::R8:
                format = 60;
                unit   = 1;
                break;
            case NativeDrawReplayTextureFormat::Bc1:
                format = 70;
                block  = 4;
                unit   = 8;
                break;
            case NativeDrawReplayTextureFormat::Bc2:
                format = 73;
                block  = 4;
                unit   = 16;
                break;
            case NativeDrawReplayTextureFormat::Bc3:
                format = 76;
                block  = 4;
                unit   = 16;
                break;
            case NativeDrawReplayTextureFormat::Rgba16Float:
                format = 9;
                unit   = 8;
                break;
            case NativeDrawReplayTextureFormat::Rg16Float:
                format = 33;
                unit   = 4;
                break;
            case NativeDrawReplayTextureFormat::R32Float:
                format = 39;
                unit   = 4;
                break;
        }
        if (!format || cpu->frameTextureId || !cpu->width || !cpu->height || !cpu->mipLevels || cpu->mipLevels > 13)
            throw std::runtime_error("CPU snapshot texture layout is unsupported");
        const auto slices = cpu->dimension == NativeDrawReplayTextureDimension::TextureCube ? 6U : 1U;
        row.insertOrAssign("host_format", format);
        row.insertOrAssign("host_width", cpu->width);
        row.insertOrAssign("host_height", cpu->height);
        row.insertOrAssign("host_depth_or_array_size", slices);
        row.insertOrAssign("host_mip_levels", cpu->mipLevels);
        row.insertOrAssign("row_size", ((cpu->width + block - 1) / block) * unit);
        row.insertOrAssign("row_count", (cpu->height + block - 1) / block);
        Array       subresources;
        const auto  payload = cpu->payloadBytes();
        std::size_t offset  = 0;
        for (std::uint32_t slice = 0; slice < slices; ++slice)
            for (std::uint32_t mip = 0; mip < cpu->mipLevels; ++mip)
            {
                const auto width = std::max(1U, cpu->width >> mip), height = std::max(1U, cpu->height >> mip);
                const auto rowSize = ((width + block - 1) / block) * unit, rowCount = (height + block - 1) / block;
                const auto size = std::size_t(rowSize) * rowCount;
                if (size > payload.size() - offset)
                    throw std::runtime_error("CPU snapshot texture payload is incomplete");
                subresources.pushBack(Table{
                    { "mip_level", mip }, { "array_slice", slice }, { "host_width", width }, { "host_height", height }, { "row_size", rowSize }, { "row_count", rowCount }, { "data", bytes(payload.subspan(offset, size)) } });
                offset += size;
            }
        if (offset != payload.size())
            throw std::runtime_error("CPU snapshot texture payload has trailing bytes");
        row.insert("subresources", std::move(subresources));
        return row;
    }
};

} // namespace

bool NativeGameplayLiveInputs::WriteSnapshot(const std::filesystem::path& directory, std::string& error) const
{
    error.clear();
    try
    {
        if (!std::filesystem::create_directory(directory))
            throw std::runtime_error("CPU snapshot requires a fresh directory");
        Writer        writer{ directory };
        const auto&   snapshot = Snapshot();
        const auto&   swap     = snapshot.final_swap;
        Array         events, textures, batches;
        const bool    batched    = snapshot.events.size() > NativeGameplayBatchEvents;
        std::uint64_t firstEvent = 0, lastEvent = 0;
        const auto    flush = [&]()
        {
            const auto count = events.size();
            auto       row   = writer.metadata(std::format("events-{:06}.toml", batches.size() + 1),
                                               Table{ { "events", std::move(events) } });
            row.insert("ordinal", static_cast<std::int64_t>(batches.size() + 1));
            row.insert("event_count", static_cast<std::int64_t>(count));
            row.insert("first_event", static_cast<std::int64_t>(firstEvent));
            row.insert("last_event", static_cast<std::int64_t>(lastEvent));
            batches.pushBack(std::move(row));
            events = {};
            writer.previousRegisters.clear();
        };
        for (const auto& event : snapshot.events)
        {
            if (events.empty())
                firstEvent = event.id;
            lastEvent = event.id;
            events.pushBack(writer.event(event));
            if (batched && events.size() == NativeGameplayBatchEvents)
                flush();
        }
        if (batched && !events.empty())
            flush();
        for (std::size_t index = 0; index < snapshot.textures.size(); ++index)
        {
            const auto cpu = Textures().find(static_cast<std::uint32_t>(index));
            textures.pushBack(writer.texture(static_cast<std::uint32_t>(index), snapshot.textures[index], cpu == Textures().end() ? nullptr : &cpu->second));
        }
        const auto unavailable = status(DrawCaptureAvailability::kUnavailable, DrawCaptureReason::kReadback);
        Table      table{
            { "schema_version", batched ? 3U : snapshot.schema_version },
            { "input_origin", "guest_cpu_observer" },
            { "capture_scope", "frame" },
            { "start_frame", static_cast<std::int64_t>(snapshot.start_frame) },
            { "end_frame", static_cast<std::int64_t>(snapshot.end_frame) },
            { "start_submission", static_cast<std::int64_t>(snapshot.start_submission) },
            { "end_submission", static_cast<std::int64_t>(snapshot.end_submission) },
            { "events", std::move(events) },
            { "textures", std::move(textures) },
            { "final_swap", Table{ { "frontbuffer", static_cast<std::int64_t>(swap.frontbuffer_ptr) }, { "output_format", swap.output_format }, { "output_width", swap.output_width }, { "output_height", swap.output_height }, { "source_status", unavailable }, { "output_status", unavailable }, { "gamma", writer.words(swap.gamma.gamma_256) }, { "use_pwl_gamma_ramp", swap.gamma.use_pwl_gamma_ramp }, { "use_fxaa", swap.gamma.use_fxaa }, { "is_8bpc", swap.gamma.is_8bpc } } },
        };
        if (batched)
        {
            table.erase("events");
            table.insert("event_batches", std::move(batches));
            table.insert("event_count", static_cast<std::int64_t>(snapshot.events.size()));
            table.insert("complete", snapshot.end_frame > snapshot.start_frame && !snapshot.events.empty() && snapshot.events.back().kind == DrawCaptureFrameEvent::Kind::kSwap);
        }
        writer.payloads.close();
        if (!writer.payloads)
            throw std::runtime_error("could not close CPU snapshot payload storage");
        writer.metadata("manifest.toml", table);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

NativeGameplayPriorCapture::NativeGameplayPriorCapture(std::uint32_t page, std::filesystem::path directory)
: directory(std::move(directory))
, page(page)
, discoverTerrain(!page)
{
    if (page >= 0x20000)
        throw std::runtime_error("prior producer capture requires a valid physical page");
}

void NativeGameplayPriorCapture::Draw(const DrawCaptureFrameInputDrawView& draw)
{
    if (!collecting)
        return;
    bool missingClear = false;
    if (discoverTerrain)
    {
        const bool issued   = draw.original_host_issued && draw.execution_status == DrawCaptureExecutionStatus::kIssued;
        const bool producer = issued && draw.vertex_shader.hash == 0xDBCF02DFB9E3F8FBULL &&
                              draw.pixel_shader.hash == 0xF375BB83352BFB1BULL;
        // Keep the color clear and its following depth alias before the terrain
        // draws. The converter still verifies shader bytes, coverage and state.
        if (!candidate && issued && draw.registers.size() == 0x5003 &&
            draw.vertex_shader.hash == 0x1E6883FCCDE1F688ULL &&
            draw.pixel_shader.hash == 0xA4A965C189287B99ULL && draw.registers[0x2000] == 0x08000230)
            candidate = true;
        if (producer && !candidate)
        {
            candidate    = true;
            missingClear = true;
        }
        terrain |= producer;
        if (!candidate)
            return;
    }
    std::string error;
    if (failure.empty() && !inputs->Draw(draw, error))
        failure = "draw " + std::to_string(draw.id) + ": " + error;
    if (failure.empty() && missingClear)
        failure = "terrain producer arrived without its leading clear; reload the game";
}

void NativeGameplayPriorCapture::Copy(const DrawCaptureFrameInputCopyView& copy)
{
    if (!collecting || (discoverTerrain && !candidate))
        return;
    if (discoverTerrain && terrain && !found && copy.original_succeeded && copy.written_length &&
        copy.copy_mode == "resolve" && copy.registers.size() == 0x5003 &&
        copy.registers[0x2000] == 0x08000230 && copy.registers[0x231A] == 0x02000200 &&
        (copy.registers[0x2318] & 7U) == 0)
    {
        const auto destination = copy.registers[0x2319];
        if (destination && destination < 0x20000000U && !(destination & 0xFFFU) &&
            copy.written_address <= destination &&
            destination < std::uint64_t(copy.written_address) + copy.written_length)
            page = destination >> 12;
    }
    const auto address = std::uint64_t(page) << 12;
    found |= page && copy.original_succeeded && copy.written_length &&
             copy.written_address < address + 0x1000 && address < std::uint64_t(copy.written_address) + copy.written_length;
    std::string error;
    if (failure.empty() && !inputs->Copy(copy, true, error, true))
        failure = "copy " + std::to_string(copy.id) + ": " + error;
}

void NativeGameplayPriorCapture::Swap(const DrawCaptureFrameInputSwapView& swap, bool finish)
{
    if (done)
        return;
    std::string error;
    if (discoverTerrain && terrain && !found && failure.empty())
        failure = "terrain producer has no completed resolve to a physical texture page";
    if (collecting && (found || (discoverTerrain && terrain)))
    {
        if (!inputs->Finish(swap, error) && failure.empty())
            failure = error;
        captured        = std::move(inputs);
        capturedFailure = failure;
        if (discoverTerrain)
        {
            collecting = false;
            done       = true;
            return;
        }
    }
    if (finish)
    {
        failure = captured ? capturedFailure : "no write to the selected prior producer page was observed";
        if (failure.empty() && !captured->WriteSnapshot(directory / "prior-inputs", error))
            failure = error;
        if (captured && !captured->WriteOwnership(directory / "prior-ownership.toml", error) && failure.empty())
            failure = error;
        collecting = false;
        done       = true;
        return;
    }
    found     = false;
    candidate = false;
    terrain   = false;
    failure.clear();
    if (!inputs)
        inputs = std::make_unique<NativeGameplayLiveInputs>();
    collecting = inputs->Begin(swap, failure);
    if (!collecting)
        done = true;
}

} // namespace rerevved::gpu::diagnostics
