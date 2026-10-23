#include "shader_harvest.h"

#include "scene_resource_capture.h"

#include "gpu/d3d12/native_texture_upload.h"
#include "gpu/guest_d3d/indexed_buffer_draw_inputs.h"
#include "gpu/guest_d3d/native_draw_hook.h"

#include <rex/hash.h>
#include <rex/logging.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <toml++/toml.hpp>

namespace rerevved::gpu::diagnostics
{
namespace
{

using guest_d3d::NativeDrawHook;
using guest_d3d::NativeDrawHookAddress;

constexpr std::uint64_t kShadowBytes = 0x3500;
constexpr std::uint64_t kMaxBlob     = 16ULL << 20;

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("shader_harvest_input_extent");
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) | (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

std::uint64_t xxh3(std::span<const std::uint8_t> bytes)
{
    return XXH3_64bits(bytes.data(), bytes.size());
}

std::uint64_t xxh3(std::string_view text)
{
    return XXH3_64bits(text.data(), text.size());
}

std::string hex(std::span<const std::uint8_t> bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string           text;
    text.reserve(bytes.size() * 2);
    for (const auto byte : bytes)
    {
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 15]);
    }
    return text;
}

std::string address(std::uint32_t value)
{
    return std::format("0x{:08X}", value);
}

void writeFile(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file)
        throw std::runtime_error("shader_harvest_write: " + path.filename().string());
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
    writeFile(path, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

toml::array literalRecords(const std::vector<guest_d3d::ShaderLiteralRange>& ranges, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& registers)
{
    toml::array records;
    // `registers` holds each record's values in record order, so overlapping
    // records keep their own values.
    std::size_t next = 0;
    for (const auto& range : ranges)
    {
        toml::array values;
        const auto  first = guest_d3d::kShaderLiteralRegisterBase + range.firstFloat4 * 4U;
        for (std::uint32_t i = 0; i < range.dwordCount; ++i, ++next)
        {
            if (next >= registers.size() || registers[next].first != first + i)
                throw std::runtime_error("shader_harvest_literal_order");
            values.push_back(static_cast<std::int64_t>(registers[next].second));
        }
        records.push_back(toml::table{ { "first_float4", range.firstFloat4 }, { "dword_count", range.dwordCount }, { "physical", address(range.physical) }, { "values", std::move(values) } });
    }
    return records;
}

// Texture fetch slots a stage samples, with each slot's fetch constant words
// (G+0x480 + 24 * slot) and their decoded shape.
toml::array textures(std::span<const std::uint8_t> code, std::span<const std::uint8_t> state)
{
    toml::array slots;
    if (code.empty())
        return slots;
    const auto mask = SceneTextureFetchMask(code);
    for (std::uint32_t slot = 0; slot < 32; ++slot)
    {
        if (!(mask >> slot & 1))
            continue;
        SceneFetch  fetch{};
        toml::array words;
        for (std::size_t i = 0; i < fetch.size(); ++i)
        {
            fetch[i] = word(state, 0x480 + slot * 24 + i * 4);
            words.push_back(address(fetch[i]));
        }
        const auto t = DecodeSceneFetch(fetch);
        slots.push_back(toml::table{ { "slot", slot },
                                     { "words", std::move(words) },
                                     { "format", t.format },
                                     { "dimension", t.dimension },
                                     { "width", t.width },
                                     { "height", t.height },
                                     { "depth", t.depth },
                                     { "min_mip", t.minMip },
                                     { "max_mip", t.maxMip } });
    }
    return slots;
}

// Shadow maps 0x4900..0x4907 and 0x4908..0x4927 from G+0x2780;
// 0x826A4CD0/0x826A50A0 place VS/PS at+0x2780/at+0x2790 and loops
// at+0x27A0/at+0x27E0.
constexpr std::size_t   kBoolConstants = 0x2780, kLoopConstants = 0x27A0;
constexpr std::uint32_t kBoolConstantWords = 8, kLoopConstantWords = 32;

toml::array constantWords(std::span<const std::uint8_t> state, std::size_t offset, std::uint32_t count)
{
    toml::array words;
    for (std::uint32_t i = 0; i < count; ++i)
        words.push_back(address(word(state, offset + 4 * i)));
    return words;
}

// The guest's physical alias of a CPU address, as the index and vertex
// buffer reads of the draw adapters convert it.
std::uint64_t physical(std::uint64_t raw)
{
    return (raw & 0x1FFFFFFFULL) + (((raw >> 20) + 512) & 0x1000);
}

// Target tokens use G+0x2880/G+0x2884/G+0x2888, surface objects
// G+0x3098..G+0x30A8, and RT0 extent +0x24.
struct TargetForm
{
    std::uint32_t colorFormat = 0, samples = 0;
    std::string   text;
};

TargetForm targetForm(std::span<const std::uint8_t> state, const ShaderHarvestRead& read)
{
    TargetForm    form;
    const auto    surfaceInfo = word(state, 0x2880), colorInfo = word(state, 0x2884), depthInfo = word(state, 0x2888);
    std::uint32_t mask = 0;
    for (std::uint32_t i = 0; i < 4; ++i)
        mask |= word(state, 0x3098 + 4 * i) ? 1U << i : 0;
    const auto    rt0    = word(state, 0x3098);
    std::uint32_t extent = 0;
    if (rt0 && read)
    {
        try
        {
            const auto bytes = read(std::uint64_t(rt0) + 0x24, 4, false);
            extent           = word(bytes, 0);
        }
        catch (...)
        {
        }
    }
    const bool tiling = (state[0x2ABC] & 0x20) != 0;
    const auto tiles  = tiling && rt0 && rt0 == word(state, 0x31B8) ? word(state, 0x31CC) : 0;
    form.colorFormat  = (colorInfo >> 16) & 0x3FF; // format bits 16..19, exponent bias 20..25
    form.samples      = (surfaceInfo >> 16) & 3;
    form.text         = std::format("rt {:x} pitch {} msaa {} color {:x} edram {:03x} depth {} edram {:03x} extent {}x{} tiles {}",
                                    mask,
                                    surfaceInfo & 0x3FFF,
                                    form.samples,
                                    form.colorFormat,
                                    colorInfo & 0xFFF,
                                    word(state, 0x30A8) ? 1 : 0,
                                    depthInfo & 0xFFF,
                                    extent ? ((extent >> 18) & 0x3FFF) + 1 : 0,
                                    extent ? ((extent >> 3) & 0x7FFF) + 1 : 0,
                                    tiles);
    return form;
}

bool tilingActive(std::span<const std::uint8_t> state)
{
    return (state[0x2ABC] & 0x20) != 0;
}

// G+0x2ABC tiling flags, G+0x31CC tile count, G+0x31AC predication and
// G+0x31A4 predicate mask.
std::string tilingToken(std::span<const std::uint8_t> state)
{
    return std::format("tiling {} predicated {:x} mask {:x}", tilingActive(state) ? word(state, 0x31CC) : 0, word(state, 0x31AC), word(state, 0x31A4));
}

// Index bytes the indexed-UP paths copy: 2 or 4 per index (format bit 2),
// rounded up to a word as 0x826A3568 rounds them.
std::uint64_t indexBytes(std::uint32_t format, std::uint32_t count)
{
    return (std::uint64_t((format & 4) ? 4 : 2) * count + 3) & ~std::uint64_t(3);
}

// Comma- or 'x'-separated decimal numbers starting at `text`.
template <std::size_t N>
bool numbersAt(std::string_view text, std::array<std::int64_t, N>& out)
{
    for (std::size_t i = 0; i < N; ++i)
    {
        const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), out[i]);
        if (code != std::errc())
            return false;
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
        if (i + 1 < N)
        {
            if (text.empty() || (text[0] != ',' && text[0] != 'x'))
                return false;
            text.remove_prefix(1);
        }
    }
    return true;
}

// Depth-only ffffffff rect:whole 1280x720, inside/nonempty, else outside/empty.
// See native_frame_assembler.cpp:2017, native_render_frame.cpp:541-546. Other
// clears keep rect; UI stencil varied 21818 times/58 minutes.
std::string coverageToken(const std::string& token)
{
    const auto at = token.find(" rect ");
    if (!token.starts_with("C ") || token.find(" index ffffffff ") == std::string::npos || at == std::string::npos)
        return token;
    std::array<std::int64_t, 4> rect{};
    if (!numbersAt(std::string_view(token).substr(at + 6), rect))
        return token;
    if (!rect[0] && !rect[1] && rect[2] == 1280 && rect[3] == 720)
        return token;
    const bool inside = rect[0] >= 0 && rect[1] >= 0 && rect[2] > rect[0] && rect[3] > rect[1] && rect[2] <= 1280 && rect[3] <= 720;
    return token.substr(0, at) + (inside ? " rect inside" : " rect outside") + token.substr(token.find(' ', at + 6));
}

// The canonical tokens of a written frame. toml++ writes `canonical` first,
// so the array is parsed alone; another layout parses the whole file.
std::vector<std::string> readCanonical(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::string   line, text;
    if (std::getline(file, line) && line.starts_with("canonical = ["))
    {
        text = line + '\n';
        while (!line.ends_with("]") && std::getline(file, line))
            text += line + '\n';
    }
    file.close();
    const auto  table = text.empty() || !text.ends_with("]\n") ? toml::parse_file(path.string()) : toml::parse(text);
    const auto* list  = table["canonical"].as_array();
    if (!list)
        throw std::runtime_error("shader_harvest_known_canonical: " + path.string());
    std::vector<std::string> tokens;
    for (const auto& node : *list)
        tokens.push_back(node.value_or(std::string()));
    return tokens;
}

std::uint64_t parseHash(const std::string& text)
{
    std::uint64_t value    = 0;
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (code != std::errc() || end != text.data() + text.size() || text.empty())
        throw std::runtime_error("shader_harvest_known_hash: " + text);
    return value;
}

} // namespace

ShaderHarvestCoverage ShaderHarvestCoverageOf(const std::vector<std::string>& canonical)
{
    // Order matters to the assembler (an MRT pair clears both slots before
    // the set draws), so non-draw operations stay in order and draws are
    // compared per segment.
    ShaderHarvestCoverage      coverage;
    std::string                shape;
    std::vector<std::uint64_t> segment;
    for (const auto& token : canonical)
    {
        if (token.starts_with("{") && token.ends_with("}"))
        {
            std::string_view draws = std::string_view(token).substr(1, token.size() - 2);
            for (std::size_t next = 0; next != std::string_view::npos;)
            {
                const auto end = draws.find(" | ", next);
                segment.push_back(xxh3(draws.substr(next, end == std::string_view::npos ? end : end - next)));
                next = end == std::string_view::npos ? end : end + 3;
            }
            continue;
        }
        std::ranges::sort(segment);
        shape += (segment.empty() ? "- " : "D ") + coverageToken(token) + '\n';
        coverage.segments.push_back(std::move(segment));
        segment.clear();
    }
    std::ranges::sort(segment);
    shape += segment.empty() ? "-" : "D";
    coverage.segments.push_back(std::move(segment));
    coverage.shape = xxh3(shape);
    return coverage;
}

bool ShaderHarvestCoverageSet::Covers(const ShaderHarvestCoverage& coverage) const
{
    const auto found = shapes.find(coverage.shape);
    return found != shapes.end() && std::ranges::any_of(found->second, [&](const std::vector<std::vector<std::uint64_t>>& known)
                                                        {
                                                            if (known.size() != coverage.segments.size())
                                                                return false;
                                                            for (std::size_t i = 0; i < known.size(); ++i)
                                                                if (!std::ranges::includes(known[i], coverage.segments[i]))
                                                                    return false;
                                                            return true;
                                                        });
}

void ShaderHarvestCoverageSet::Add(const ShaderHarvestCoverage& coverage)
{
    if (!Covers(coverage))
        shapes[coverage.shape].push_back(coverage.segments);
}

struct ShaderHarvestBlobReader
{
    const ShaderHarvestRead& read;

    template <typename Blob>
    Blob blob(std::string role, std::uint64_t at, std::uint64_t size, bool physicalRead = false, bool deferred = false) const
    {
        Blob b;
        b.role     = std::move(role);
        b.address  = at;
        b.size     = size;
        b.physical = physicalRead;
        b.deferred = deferred;
        b.timing   = deferred ? "next_operation" : "operation";
        if (deferred)
            return b;
        fill(b);
        return b;
    }

    template <typename Blob>
    void fill(Blob& b) const
    {
        try
        {
            if (!read)
                throw std::runtime_error("no_reader");
            if (b.size > kMaxBlob)
                throw std::runtime_error(std::format("blob_extent {}", b.size));
            b.bytes = read(b.address, b.size, b.physical);
            if (b.bytes.size() != b.size)
                throw std::runtime_error("blob_short");
        }
        catch (const std::exception& exception)
        {
            b.bytes.clear();
            b.error = exception.what();
        }
        b.deferred = false;
    }

    std::uint32_t wordAt(std::uint64_t at) const
    {
        return word(read(at, 4, false), 0);
    }
};

bool ShaderHarvest::Start(const std::filesystem::path& directory, const ShaderHarvestOptions& start, std::string& error)
{
    std::lock_guard lock(mutex);
    error.clear();
    std::error_code code;
    if (running || std::filesystem::exists(directory, code))
    {
        error = "shader harvest requires a fresh directory";
        return false;
    }
    try
    {
        std::set<std::filesystem::path> loading;
        for (const auto& prior : start.known)
            loadKnown(prior, loading);
    }
    catch (const std::exception& exception)
    {
        error = std::string("shader harvest known harvest: ") + exception.what();
        return false;
    }
    if (!std::filesystem::create_directories(directory, code))
    {
        error = "shader harvest requires a fresh directory";
        return false;
    }
    if (start.frameInstances && (!std::filesystem::create_directory(directory / "frames", code) || !std::filesystem::create_directory(directory / "payloads", code)))
    {
        error = "shader harvest frame directories";
        return false;
    }
    root    = directory;
    options = start;
    running = true;
    dirty   = true;
    return writeIndexLocked(error);
}

// Counts only what a prior harvest holds on disk: a key whose record.toml
// exists, a signature with an instance whose frame.toml exists. A row lent
// from a further harvest loads that harvest, which then vouches for it.
void ShaderHarvest::loadKnown(const std::filesystem::path& directory, std::set<std::filesystem::path>& loading)
{
    if (!loading.insert(std::filesystem::absolute(directory).lexically_normal()).second)
        return;
    const auto harvest = directory.string();
    const auto index   = toml::parse_file((directory / "index.toml").string());
    const auto chained = [&](const std::string& other)
    {
        if (other.empty() || other == harvest)
            return;
        try
        {
            loadKnown(other, loading);
        }
        catch (const std::exception& exception)
        {
            REXLOG_WARN("Shader harvest: known {} names {}, which is unreadable: {}", harvest, other, exception.what());
        }
    };
    // Schema 3 records carry the loop and bool constants; older ones are
    // written again.
    if (index["schema_version"].value_or<std::int64_t>(0) >= 3)
        if (const auto* rows = index["pairs"].as_array())
            for (const auto& node : *rows)
            {
                const auto* row = node.as_table();
                if (!row || row->contains("error"))
                    continue;
                if (const auto other = (*row)["known_harvest"].value_or(std::string()); !other.empty())
                {
                    chained(other);
                    continue;
                }
                const auto at = (*row)["directory"].value_or(std::string());
                if (!at.empty() && std::filesystem::exists(directory / at / "record.toml"))
                    knownKeys.try_emplace(parseHash((*row)["key_hash"].value_or(std::string())), KnownKey{ at, harvest });
            }
    const auto framesIndex = directory / "frames" / "index.toml";
    if (!std::filesystem::exists(framesIndex))
        return;
    const auto  frames = toml::parse_file(framesIndex.string());
    const auto* rows   = frames["signatures"].as_array();
    if (!rows)
        return;
    const auto present = [&](const std::string& name)
    {
        return !name.empty() && std::filesystem::exists(directory / "frames" / name / "frame.toml");
    };
    for (const auto& node : *rows)
    {
        const auto* row = node.as_table();
        if (!row)
            continue;
        const auto signature = parseHash((*row)["signature"].value_or(std::string()));
        chained((*row)["known_harvest"].value_or(std::string()));
        if (const auto* instances = (*row)["instances"].as_array())
            for (const auto& name : *instances)
                if (const auto text = name.value_or(std::string()); present(text))
                {
                    knownSignatures.try_emplace(signature, harvest);
                    coverage.Add(ShaderHarvestCoverageOf(readCanonical(directory / "frames" / text / "frame.toml")));
                }
        if (const auto* marked = (*row)["draw_before_clear_instances"].as_array())
            if (std::ranges::any_of(*marked, [&](const toml::node& name)
                                    {
                                        return present(name.value_or(std::string()));
                                    }))
                knownMarked.try_emplace(signature, harvest);
    }
    REXLOG_INFO("Shader harvest: known {}: {} keys, {} written signatures, {} draw-before-clear signatures so far", harvest, knownKeys.size(), knownSignatures.size(), knownMarked.size());
}

bool ShaderHarvest::armedLocked()
{
    if (armedFrame || options.armFile.empty())
        return true;
    std::error_code code;
    if (!std::filesystem::exists(options.armFile, code))
        return false;
    armedFrame = frame;
    REXLOG_INFO("Shader harvest armed by {} at frame {}", options.armFile.string(), frame);
    return true;
}

bool ShaderHarvest::Selects(std::uint32_t caller) const noexcept
{
    return !options.caller || *options.caller == caller;
}

void ShaderHarvest::skip(bool readFailure, const std::string& error)
{
    std::lock_guard lock(mutex);
    ++(readFailure ? unreadable : withoutShaders);
    if (readFailure && firstError.empty())
    {
        firstError = error;
        REXLOG_WARN("Shader harvest: {}", error);
    }
    dirty = true;
}

std::size_t ShaderHarvest::record(const ShaderHarvestCall& call, const ShaderHarvestPair& key, const std::string& keyText, const guest_d3d::IndexedBufferDrawInputs& inputs)
{
    ShaderHarvestPair pair = key;
    toml::table       table;
    try
    {
        pair.vertexWordDigest = guest_d3d::IndexedBufferMicrocodeWordDigest(inputs.vertexCode);
        pair.pixelWordDigest  = pair.vertexOnly ? std::string() : guest_d3d::IndexedBufferMicrocodeWordDigest(inputs.pixelCode);
        pair.textureMask      = SceneTextureFetchMask(inputs.vertexCode) | (pair.vertexOnly ? 0 : SceneTextureFetchMask(inputs.pixelCode));
        pair.draws            = 1;

        toml::array arguments;
        for (const auto value : call.arguments)
            arguments.push_back(address(value));
        table = toml::table{
            { "schema_version", 3 },
            { "caller", address(call.caller) },
            { "entry", address(call.entry) },
            { "arguments_r3_r10", std::move(arguments) },
            { "stack_r1", address(call.stack) },
            { "result_r3", address(call.result) },
            { "key", keyText },
            { "key_hash", std::format("{:016X}", pair.keyHash) },
            { "alpha_test", pair.alphaTest },
            { "color_format", pair.colorFormat },
            { "samples", pair.samples },
            { "primitive", pair.primitive },
            { "target_form", pair.targetForm },
            { "vertex_only", pair.vertexOnly },
            { "vertex_variant", pair.variant },
            { "texture_mask", address(pair.textureMask) },
            { "vertex_hash", std::format("{:016X}", pair.vertexHash) },
            { "pixel_hash", std::format("{:016X}", pair.pixelHash) },
            { "vertex_word_sha256", pair.vertexWordDigest },
            { "pixel_word_sha256", pair.pixelWordDigest },
            { "vertex_object", address(inputs.vertexObject) },
            { "pixel_object", address(inputs.pixelObject) },
            { "declaration_address", address(inputs.declarationAddress) },
            // SetStreamSource keeps stream i's stride in words at G+0x30F0 + i.
            { "stream0_stride_bytes", inputs.state.at(0x30F0) * 4 },
            { "vertex_header", hex(inputs.vertexHeader) },
            { "vertex_metadata", hex(inputs.vertexMetadata) },
            { "pixel_header", hex(inputs.pixelHeader) },
            { "pixel_metadata", hex(inputs.pixelMetadata) },
            { "fetch_records", hex(inputs.fetchRecords) },
            { "fetch_templates", hex(inputs.fetchTemplates) },
            { "declaration_header", hex(inputs.declarationHeader) },
            { "declaration", hex(inputs.declaration) },
            { "vertex_literals", literalRecords(inputs.vertexLiteralRanges, inputs.vertexLiteralRegisters) },
            { "pixel_literals", literalRecords(inputs.pixelLiteralRanges, inputs.pixelLiteralRegisters) },
            { "vertex_textures", textures(inputs.vertexCode, inputs.state) },
            { "pixel_textures", textures(inputs.pixelCode, inputs.state) },
            { "bool_constants", constantWords(inputs.state, kBoolConstants, kBoolConstantWords) },
            { "loop_constants", constantWords(inputs.state, kLoopConstants, kLoopConstantWords) },
        };
    }
    catch (const std::exception& exception)
    {
        skip(true, exception.what());
        return SIZE_MAX;
    }

    std::lock_guard lock(mutex);
    if (!running)
        return SIZE_MAX;
    if (const auto found = known.find(keyText); found != known.end())
    {
        ++pairs[found->second].draws;
        ++draws;
        return found->second;
    }
    // A key is known from its first draw even if its files fail, so later
    // draws only count it.
    pair.firstFrame = frame;
    pair.directory  = std::format("{:08X}-{:016X}-{:016X}-{:08X}", pair.caller, pair.vertexHash, pair.pixelHash, static_cast<std::uint32_t>(pair.keyHash));
    for (std::size_t n = 1; std::ranges::any_of(pairs, [&](const auto& p)
                                                {
                                                    return p.directory == pair.directory;
                                                });
         ++n)
        pair.directory = std::format("{:08X}-{:016X}-{:016X}-{:08X}-{}", pair.caller, pair.vertexHash, pair.pixelHash, static_cast<std::uint32_t>(pair.keyHash), n);
    table.insert("first_frame", static_cast<std::int64_t>(frame));
    const auto index = pairs.size();
    known.emplace(keyText, index);
    pairs.push_back(pair);
    ++draws;
    dirty = true;
    try
    {
        const auto directory = root / pair.directory;
        if (!std::filesystem::create_directory(directory))
            throw std::runtime_error("shader_harvest_directory_exists: " + pair.directory);
        writeFile(directory / "vs_code.be.bin", inputs.vertexCode);
        if (!pair.vertexOnly)
            writeFile(directory / "ps_code.be.bin", inputs.pixelCode);
        writeFile(directory / "state.be.bin", inputs.state);
        std::ostringstream text;
        text << table << '\n';
        writeText(directory / "record.toml", text.str());
    }
    catch (const std::exception& exception)
    {
        pairs.back().error = exception.what();
        ++failures;
        if (firstError.empty())
            firstError = exception.what();
        REXLOG_WARN("Shader harvest: {}", exception.what());
    }
    REXLOG_INFO("Shader harvest: new key {} at frame {}", pair.directory, frame);
    std::string indexError;
    writeIndexLocked(indexError);
    return index;
}

void ShaderHarvest::observeDraw(const ShaderHarvestCall& call, const ShaderHarvestRead& read)
{
    // The two outer UP wrappers' own child calls fold into the wrapper's record.
    if ((call.entry == NativeDrawHookAddress(NativeDrawHook::IndexedUpLowerer) && call.caller == guest_d3d::kIndexedUpLowererReturn) ||
        (call.entry == NativeDrawHookAddress(NativeDrawHook::NonindexedSink) && call.caller == guest_d3d::kNonindexedSinkReturn) || !Selects(call.caller))
        return;
    const auto                g = call.arguments[0];
    std::vector<std::uint8_t> state;
    try
    {
        state = read(g, kShadowBytes, false);
        if (state.size() != kShadowBytes)
            throw std::runtime_error("shader_harvest_shadow_extent");
    }
    catch (const std::exception& exception)
    {
        skip(true, exception.what());
        return;
    }
    const auto        form = targetForm(state, read);
    ShaderHarvestPair key;
    key.caller = call.caller;
    key.entry  = call.entry;
    // RB_COLORCONTROL (G+0x293C): alpha function bits 0..2 under the enable bit 3.
    const auto colorControl = word(state, 0x293C);
    key.alphaTest           = (colorControl & 8) ? colorControl & 0xF : 0;
    key.colorFormat         = form.colorFormat;
    key.samples             = form.samples;
    key.primitive           = call.arguments[1];
    key.targetForm          = form.text;

    guest_d3d::IndexedBufferDrawInputs inputs;
    std::string                        error;
    const bool                         shaders = guest_d3d::ReadDrawShaderCode(g, read, inputs, error);
    if (shaders)
    {
        key.vertexHash = xxh3(inputs.vertexCode);
        key.pixelHash  = inputs.pixelObject ? xxh3(inputs.pixelCode) : 0;
        key.vertexOnly = !inputs.pixelObject;
        key.variant    = inputs.variant;
    }
    else
        skip(!error.empty(), error);
    const auto keyText = std::format("caller {:08X} entry {:08X} vs {:016X} ps {:016X} alpha {:x} color {:x} msaa {} primitive {} target [{}]",
                                     key.caller,
                                     key.entry,
                                     key.vertexHash,
                                     key.pixelHash,
                                     key.alphaTest,
                                     key.colorFormat,
                                     key.samples,
                                     key.primitive,
                                     key.targetForm);
    key.keyHash        = xxh3(keyText);
    std::size_t pair   = SIZE_MAX;
    if (shaders)
    {
        {
            std::lock_guard lock(mutex);
            if (!running)
                return;
            if (const auto found = known.find(keyText); found != known.end())
            {
                pair = found->second;
                ++pairs[pair].draws;
                ++draws;
                dirty = true;
            }
        }
        // Read-only after Start.
        const auto lent = knownKeys.find(key.keyHash);
        if (pair == SIZE_MAX && lent != knownKeys.end())
        {
            // A known harvest holds this key's record; it is only counted here.
            ShaderHarvestPair adopted = key;
            adopted.vertexWordDigest  = guest_d3d::IndexedBufferMicrocodeWordDigest(inputs.vertexCode);
            adopted.pixelWordDigest   = adopted.vertexOnly ? std::string() : guest_d3d::IndexedBufferMicrocodeWordDigest(inputs.pixelCode);
            adopted.textureMask       = SceneTextureFetchMask(inputs.vertexCode) | (adopted.vertexOnly ? 0 : SceneTextureFetchMask(inputs.pixelCode));
            adopted.directory         = lent->second.directory;
            adopted.knownHarvest      = lent->second.harvest;
            std::lock_guard lock(mutex);
            if (!running)
                return;
            ++draws;
            dirty = true;
            if (const auto found = known.find(keyText); found != known.end())
                pair = found->second;
            else
            {
                adopted.firstFrame = frame;
                pair               = pairs.size();
                known.emplace(keyText, pair);
                pairs.push_back(std::move(adopted));
                REXLOG_INFO("Shader harvest: known key {} at frame {}", pairs.back().directory, frame);
            }
            ++pairs[pair].draws;
        }
        if (pair == SIZE_MAX)
        {
            guest_d3d::IndexedBufferDrawInputs full;
            if (guest_d3d::ReadDrawShaderInputs(g, read, full, error))
                pair = record(call, key, keyText, full);
            else
                skip(!error.empty(), error);
        }
    }
    // The key's fetch slots either stage samples; the frame end reads their textures.
    std::uint32_t textureMask = 0;
    {
        std::lock_guard lock(mutex);
        ++entryDraws[call.entry];
        if (pair != SIZE_MAX)
            textureMask = pairs[pair].textureMask;
    }
    if (!options.frameInstances)
        return;

    Operation operation;
    operation.call        = call;
    operation.kind        = "draw";
    operation.form        = form.text;
    operation.pair        = pair;
    operation.tiled       = tilingActive(state);
    operation.token       = std::format("D {:016X} {}", key.keyHash, tilingToken(state));
    operation.textureMask = textureMask;
    const ShaderHarvestBlobReader reader{ read };
    const auto&                   r = call.arguments;
    try
    {
        if (call.entry == NativeDrawHookAddress(NativeDrawHook::IndexedUp))
        {
            // r1+84 holds the vertex stride; r5..r10 the minimum vertex,
            // vertex count, index count, indices, index format and vertices.
            operation.blobs.push_back(reader.blob<Blob>("stack", call.stack + 80ULL, 16));
            const auto stride = reader.wordAt(call.stack + 84ULL);
            operation.blobs.push_back(reader.blob<Blob>("vertices", r[7] + std::uint64_t(r[2]) * stride, std::uint64_t(r[3]) * stride));
            operation.blobs.push_back(reader.blob<Blob>("indices", r[5], indexBytes(r[6], r[4])));
        }
        else if (call.entry == NativeDrawHookAddress(NativeDrawHook::NonindexedUp))
            operation.blobs.push_back(reader.blob<Blob>("vertices", r[3], std::uint64_t(r[2]) * r[4]));
        else if (call.entry == NativeDrawHookAddress(NativeDrawHook::IndexedUpLowerer) && call.result == 0)
        {
            // A direct caller fills the destinations 0x826A3048 reserved after
            // it returns: the vertex pointer is stored where r1+84 points, the
            // index pointer at r10; r6 vertices of r9 bytes, r7 indices of format r8.
            operation.blobs.push_back(reader.blob<Blob>("stack", call.stack + 80ULL, 16));
            const auto vertices = reader.wordAt(reader.wordAt(call.stack + 84ULL));
            const auto indices  = reader.wordAt(r[7]);
            operation.blobs.push_back(reader.blob<Blob>("vertices", vertices, std::uint64_t(r[3]) * r[6], false, true));
            operation.blobs.push_back(reader.blob<Blob>("indices", indices, indexBytes(r[5], r[4]), false, true));
        }
        else if (call.entry == NativeDrawHookAddress(NativeDrawHook::NonindexedSink) && call.result != 0)
            // 0x826A2B58 returns the destination of r5 vertices of r6 bytes.
            operation.blobs.push_back(reader.blob<Blob>("vertices", call.result, std::uint64_t(r[2]) * r[3], false, true));
    }
    catch (const std::exception& exception)
    {
        Blob failed;
        failed.role  = "payload";
        failed.error = exception.what();
        operation.blobs.push_back(std::move(failed));
    }
    operation.state = std::move(state);
    std::lock_guard lock(mutex);
    if (running)
        append(std::move(operation), read);
}

void ShaderHarvest::observeOther(const ShaderHarvestCall& entered, const ShaderHarvestRead& read)
{
    if (!options.frameInstances)
        return;
    auto call = entered;
    if (call.entry == kHarvestResolveEntry)
    {
        std::lock_guard lock(mutex);
        resolveEntries[std::this_thread::get_id()] = call;
        return;
    }
    Operation operation;
    if (call.entry == kHarvestResolveCopyEntry)
    {
        // D3DDevice_Resolve's own arguments, from its entry on this thread.
        std::lock_guard lock(mutex);
        const auto      found = resolveEntries.find(std::this_thread::get_id());
        if (found != resolveEntries.end() && found->second.arguments[0] == call.arguments[0])
        {
            call       = found->second;
            call.entry = kHarvestResolveCopyEntry;
            resolveEntries.erase(found);
        }
        else
        {
            Blob unseen;
            unseen.role  = "resolve_entry";
            unseen.error = "resolve_entry_unseen";
            operation.blobs.push_back(std::move(unseen));
        }
    }
    const ShaderHarvestBlobReader reader{ read };
    try
    {
        operation.state = read(call.arguments[0], kShadowBytes, false);
        if (operation.state.size() != kShadowBytes)
            throw std::runtime_error("shader_harvest_shadow_extent");
    }
    catch (const std::exception& exception)
    {
        skip(true, exception.what());
        return;
    }
    const auto& state = operation.state;
    const auto& r     = call.arguments;
    operation.call    = call;
    operation.form    = targetForm(state, read).text;
    operation.tiled   = tilingActive(state);
    if (call.entry == kHarvestClearEntry)
    {
        // 0x826A1D90: r4 flags, r5 target index, r6..r9 the clipped rectangle,
        // r10 the color, f1 the depth and r1+92 the stencil.
        operation.kind  = "clear";
        operation.token = std::format("C flags {:x} index {:x} rect {},{},{},{} [{}] {}", r[1], r[2], r[3], r[4], r[5], r[6], operation.form, tilingToken(state));
        operation.blobs.push_back(reader.blob<Blob>("stack", call.stack + 80ULL, 16));
        if (r[7])
            operation.blobs.push_back(reader.blob<Blob>("color", r[7], 16));
    }
    else if (call.entry == kHarvestResolveCopyEntry)
    {
        // D3DDevice_Resolve: r4 source flags, r5 rectangle, r6 destination
        // texture, r7 destination point; the copy words at G+0x2A18..0x2A24.
        operation.kind   = "resolve";
        std::string rect = "full", point = "origin", texture = "none";
        if (r[2])
        {
            auto b = reader.blob<Blob>("rect", r[2], 16);
            if (b.error.empty())
                rect = std::format("{},{},{},{}", word(b.bytes, 0), word(b.bytes, 4), word(b.bytes, 8), word(b.bytes, 12));
            operation.blobs.push_back(std::move(b));
        }
        if (r[4])
        {
            auto b = reader.blob<Blob>("point", r[4], 8);
            if (b.error.empty())
                point = std::format("{},{}", word(b.bytes, 0), word(b.bytes, 4));
            operation.blobs.push_back(std::move(b));
        }
        if (r[3])
        {
            auto b = reader.blob<Blob>("texture", r[3], 0x34);
            if (b.error.empty())
            {
                const auto size = word(b.bytes, 0x1C + 8);
                texture         = std::format("{}x{} format {:x}", (size & 0x1FFF) + 1, ((size >> 13) & 0x1FFF) + 1, word(b.bytes, 0x1C + 4) & 0x3F);
            }
            operation.blobs.push_back(std::move(b));
        }
        operation.token = std::format("R source {:x} copy {:x} info {:x} pitch {:x} rect {} point {} texture {} [{}] {}",
                                      r[1],
                                      word(state, 0x2A18),
                                      word(state, 0x2A24),
                                      word(state, 0x2A20),
                                      rect,
                                      point,
                                      texture,
                                      operation.form,
                                      tilingToken(state));
    }
    else if (call.entry == kHarvestBeginTilingEntry)
    {
        operation.kind = "begin_tiling";
        std::string rects;
        for (std::uint32_t i = 0; operation.tiled && i < std::min<std::uint32_t>(word(state, 0x31CC), 4); ++i)
            rects += std::format(" {},{},{},{}", word(state, 0x31D0 + 16 * i), word(state, 0x31D4 + 16 * i), word(state, 0x31D8 + 16 * i), word(state, 0x31DC + 16 * i));
        operation.token = std::format("B {} screen {}x{} rects{}", tilingToken(state), word(state, 0x337C), word(state, 0x3380), rects);
    }
    else
        return;
    std::lock_guard lock(mutex);
    if (running)
        append(std::move(operation), read);
}

void ShaderHarvest::append(Operation operation, const ShaderHarvestRead& read)
{
    fillDeferred(read);
    // The bound surface objects: RT0..RT3 and depth, 0x30 bytes each.
    const ShaderHarvestBlobReader reader{ read };
    for (std::uint32_t slot = 0; slot < 5 && !operation.state.empty(); ++slot)
        if (const auto surface = word(operation.state, 0x3098 + 4 * slot))
            operation.blobs.push_back(reader.blob<Blob>(slot < 4 ? std::format("surface RT{}", slot) : std::string("surface depth"), surface, 0x30));
    std::uint64_t size = operation.state.size();
    for (const auto& blob : operation.blobs)
        size += std::max<std::uint64_t>(blob.bytes.size(), blob.deferred ? blob.size : 0);
    if (truncated || buffered + size > options.frameBufferBytes)
    {
        // Release the storage: a long frame keeps only its tokens.
        truncated = true;
        std::vector<std::uint8_t>().swap(operation.state);
        std::vector<Blob>().swap(operation.blobs);
    }
    else
        buffered += size;
    if (std::ranges::any_of(operation.blobs, &Blob::deferred))
        deferredOperations.push_back(operations.size());
    operations.push_back(std::move(operation));
}

void ShaderHarvest::fillDeferred(const ShaderHarvestRead& read)
{
    const ShaderHarvestBlobReader reader{ read };
    for (const auto index : deferredOperations)
        for (auto& blob : operations[index].blobs)
            if (blob.deferred)
                reader.fill(blob);
    deferredOperations.clear();
}

void ShaderHarvest::Observe(const ShaderHarvestCall& call, const ShaderHarvestRead& read) noexcept
{
    try
    {
        if (guest_d3d::FindNativeDrawHook(call.entry))
            observeDraw(call, read);
        else
            observeOther(call, read);
    }
    catch (const std::exception& exception)
    {
        skip(true, exception.what());
    }
    catch (...)
    {
        skip(true, "shader_harvest_observe_failed");
    }
}

std::string ShaderHarvest::writeBlob(const std::vector<std::uint8_t>& bytes, std::uint64_t& written)
{
    const auto hash = xxh3(bytes);
    const auto name = std::format("payloads/{:016X}.bin", hash);
    if (writtenBlobs.insert(hash).second)
    {
        writeFile(root / name, bytes);
        written += bytes.size();
    }
    return name;
}

bool ShaderHarvest::writeFrame(const std::string& name, std::uint64_t signature, bool marked, const std::vector<std::string>& canonical, const ShaderHarvestRead& read, std::uint64_t& bytes)
{
    const ShaderHarvestBlobReader reader{ read };
    try
    {
        const auto directory = root / "frames" / name;
        if (!std::filesystem::create_directory(directory))
            throw std::runtime_error("shader_harvest_frame_exists: " + name);
        toml::array events;
        for (std::size_t i = 0; i < operations.size(); ++i)
        {
            auto& operation = operations[i];
            // Memory the guest no longer changes at the swap: an IndexedBuffer
            // draw's indices, the declared streams of IndexedBuffer and
            // AutomaticIndex draws, and every sampled texture's storage.
            std::vector<Blob> late;
            const auto        lateBlob = [&](std::string role, std::uint64_t at, std::uint64_t size)
            {
                auto b   = reader.blob<Blob>(std::move(role), at, size, true);
                b.timing = "frame_end";
                late.push_back(std::move(b));
            };
            const auto& state = operation.state;
            const auto  entry = operation.call.entry;
            if (operation.kind == "draw" && !state.empty())
            {
                try
                {
                    if (entry == NativeDrawHookAddress(NativeDrawHook::IndexedBuffer))
                    {
                        // G+0x3094: flags bit 31 selects 32-bit indices, +0x18
                        // the CPU address of index zero; r6 start, r7 count.
                        const auto object = word(state, 0x3094);
                        auto       header = reader.blob<Blob>("index_buffer", object, 32);
                        if (header.error.empty())
                        {
                            const auto width = (word(header.bytes, 0) & 0x80000000U) ? 4U : 2U;
                            lateBlob("indices", physical(word(header.bytes, 24) + std::uint64_t(operation.call.arguments[3]) * width), std::uint64_t(operation.call.arguments[4]) * width);
                        }
                        late.push_back(std::move(header));
                    }
                    if (entry == NativeDrawHookAddress(NativeDrawHook::IndexedBuffer) || entry == NativeDrawHookAddress(NativeDrawHook::AutomaticIndex))
                    {
                        // Stream i is vertex fetch 95 - i (G+0x480 + 8 * (95 - i)).
                        auto declaration = reader.blob<Blob>("declaration_header", word(state, 0x2E2C), 52);
                        if (declaration.error.empty())
                        {
                            const auto elements = std::min<std::uint32_t>(word(declaration.bytes, 24), 32);
                            auto       list     = reader.blob<Blob>("declaration", word(state, 0x2E2C) + 52ULL, elements * 12ULL);
                            if (list.error.empty())
                            {
                                std::set<std::uint32_t> streams;
                                for (std::uint32_t e = 0; e < elements; ++e)
                                    streams.insert((word(list.bytes, e * 12) >> 16) & 15);
                                for (const auto stream : streams)
                                {
                                    const auto fetch = word(state, 0x480 + (95 - stream) * 8), size = word(state, 0x484 + (95 - stream) * 8);
                                    lateBlob(std::format("stream {}", stream), fetch & ~3U, std::uint64_t((size >> 2) & 0xFFFFFF) * 4);
                                }
                            }
                            late.push_back(std::move(list));
                        }
                        late.push_back(std::move(declaration));
                    }
                    for (std::uint32_t slot = 0; slot < 32; ++slot)
                    {
                        if (!(operation.textureMask >> slot & 1))
                            continue;
                        NativeTextureFetch fetch{};
                        for (std::size_t w = 0; w < fetch.size(); ++w)
                            fetch[w] = word(state, 0x480 + slot * 24 + w * 4);
                        NativeTextureMemoryRanges ranges;
                        std::string               error;
                        if (!GetNativeTextureMemoryRanges(fetch, ranges, error))
                        {
                            Blob failed;
                            failed.role  = std::format("texture {}", slot);
                            failed.error = error;
                            late.push_back(std::move(failed));
                            continue;
                        }
                        lateBlob(std::format("texture {} base", slot), ranges.base.address, ranges.base.size);
                        if (ranges.mips.size)
                            lateBlob(std::format("texture {} mips", slot), ranges.mips.address, ranges.mips.size);
                    }
                }
                catch (const std::exception& exception)
                {
                    Blob failed;
                    failed.role  = "frame_end";
                    failed.error = exception.what();
                    late.push_back(std::move(failed));
                }
            }

            toml::array arguments;
            for (const auto value : operation.call.arguments)
                arguments.push_back(address(value));
            toml::table event{ { "index", static_cast<std::int64_t>(i) },
                               { "kind", operation.kind },
                               { "entry", address(operation.call.entry) },
                               { "caller", address(operation.call.caller) },
                               { "arguments_r3_r10", std::move(arguments) },
                               { "stack_r1", address(operation.call.stack) },
                               { "result_r3", address(operation.call.result) },
                               { "token", operation.token },
                               { "target_form", operation.form } };
            if (operation.kind == "clear")
                event.insert("depth_f1", operation.call.depth);
            if (operation.pair != SIZE_MAX)
                event.insert("pair", pairs[operation.pair].directory);
            if (operation.kind == "draw" && !state.empty())
            {
                event.insert("bool_constants", constantWords(state, kBoolConstants, kBoolConstantWords));
                event.insert("loop_constants", constantWords(state, kLoopConstants, kLoopConstantWords));
            }
            if (!state.empty())
                event.insert("state", writeBlob(state, bytes));
            toml::array memory;
            for (auto* list : { &operation.blobs, &late })
                for (auto& blob : *list)
                {
                    toml::table row{ { "role", blob.role }, { "address", address(static_cast<std::uint32_t>(blob.address)) }, { "size", static_cast<std::int64_t>(blob.size) }, { "physical", blob.physical } };
                    // Read when the operation was observed, at the next one, or at the swap.
                    row.insert("read", blob.timing);
                    if (!blob.error.empty())
                        row.insert("error", blob.error);
                    else if (blob.bytes.size() <= 64)
                        row.insert("bytes", hex(blob.bytes));
                    else
                        row.insert("file", writeBlob(blob.bytes, bytes));
                    memory.push_back(std::move(row));
                }
            event.insert("memory", std::move(memory));
            events.push_back(std::move(event));
        }
        toml::array tokens;
        for (const auto& token : canonical)
            tokens.push_back(token);
        toml::table        frameTable{ { "schema_version", 1 },
                                       { "signature", std::format("{:016X}", signature) },
                                       { "frame", static_cast<std::int64_t>(frame) },
                                       { "draw_before_clear", marked },
                                       { "canonical", std::move(tokens) },
                                       { "events", std::move(events) } };
        std::ostringstream text;
        text << frameTable << '\n';
        writeText(directory / "frame.toml", text.str());
        bytes += text.str().size();
        REXLOG_INFO("Shader harvest: frame form {} at frame {} ({} operations)", name, frame, operations.size());
        return true;
    }
    catch (const std::exception& exception)
    {
        ++failures;
        if (firstError.empty())
            firstError = exception.what();
        REXLOG_WARN("Shader harvest: {}", exception.what());
        return false;
    }
}

void ShaderHarvest::closeFrame(const ShaderHarvestRead& read)
{
    fillDeferred(read);
    // Non-draw operations in order; the draws between two of them as the
    // sorted set of their tokens. A change of the tiling flag that no
    // BeginTiling announced is its own token.
    std::vector<std::string> canonical;
    std::set<std::string>    run;
    const auto               flush = [&]
    {
        if (run.empty())
            return;
        std::string joined = "{";
        for (const auto& token : run)
            joined += (joined.size() > 1 ? " | " : "") + token;
        canonical.push_back(joined + "}");
        run.clear();
    };
    bool        tiled     = false;
    std::size_t firstDraw = SIZE_MAX, firstClear = SIZE_MAX;
    for (std::size_t i = 0; i < operations.size(); ++i)
    {
        const auto& operation = operations[i];
        if (operation.tiled != tiled && operation.kind != "begin_tiling")
        {
            flush();
            canonical.push_back(operation.tiled ? "T tiling set" : "E tiling ended");
        }
        tiled = operation.tiled;
        if (operation.kind == "draw")
        {
            firstDraw = std::min(firstDraw, i);
            run.insert(operation.token);
            continue;
        }
        if (operation.kind == "clear")
            firstClear = std::min(firstClear, i);
        flush();
        canonical.push_back(operation.token);
    }
    flush();
    canonical.push_back("S swap");
    std::string joined;
    for (const auto& token : canonical)
        joined += token + '\n';
    const auto signature = xxh3(joined);
    // Presented work before any clear: the game-start form.
    const bool marked = firstDraw != SIZE_MAX && firstDraw < firstClear;

    admitFrame(signature, canonical, joined, marked, operations.size(), [&](const std::string& name) -> std::optional<std::uint64_t>
               {
                   std::uint64_t bytes = 0;
                   const bool    wrote = writeFrame(name, signature, marked, canonical, read, bytes);
                   (marked ? markedBytes : frameBytes) += bytes;
                   return wrote ? std::optional(bytes) : std::nullopt;
               });
    operations.clear();
    deferredOperations.clear();
    buffered  = 0;
    truncated = false;
    dirty     = true;
}

void ShaderHarvest::admitFrame(std::uint64_t signature, const std::vector<std::string>& canonical, const std::string& joined, bool marked, std::size_t operationCount, const std::function<std::optional<std::uint64_t>(const std::string&)>& write)
{
    // Known harvests: a signature they wrote, or a first instance whose
    // coverage they or this run's written frames hold, is counted only.
    const auto&                          knownForms = marked ? knownMarked : knownSignatures;
    const auto                           knownAt    = knownForms.find(signature);
    const auto                           listed     = forms.find(signature);
    const std::size_t                    written    = listed == forms.end() ? 0 : (marked ? listed->second.markedInstances : listed->second.instances).size();
    const bool                           failed     = listed != forms.end() && listed->second.writeFailed;
    std::optional<ShaderHarvestCoverage> cover;
    if (!options.known.empty() && !marked && !written && !failed && knownAt == knownForms.end())
        cover = ShaderHarvestCoverageOf(canonical);
    const bool covered = cover && coverage.Covers(*cover);
    const bool armed   = armedLocked();
    // Forms listed before arming do not consume armed slots, so post-arm frames
    // are not starved.
    const bool fresh = cover ? !covered : marked && !options.known.empty() && !written && !failed && knownAt == knownForms.end();
    const bool novel = fresh && armed && !truncated && (marked ? markedBytes : frameBytes) < options.frameBytes && novelForms < options.frameSignatures;
    if (listed == forms.end() && (armed ? armedForms : forms.size()) >= options.frameSignatures && !novel)
    {
        // Past the signature cap nothing new is listed or logged.
        ++unlistedFrames;
        return;
    }
    auto [found, inserted] = forms.try_emplace(signature);
    auto& form             = found->second;
    if (inserted)
    {
        if (armed)
            ++(armedForms < options.frameSignatures ? armedForms : novelForms);
        form.signature  = signature;
        form.firstFrame = frame;
        form.operations = operationCount;
        if (const auto at = knownSignatures.find(signature); at != knownSignatures.end())
            form.knownHarvest = at->second;
        else if (const auto lent = knownMarked.find(signature); lent != knownMarked.end())
            form.knownHarvest = lent->second;
        formOrder.push_back(signature);
        if (!joined.empty())
            try
            {
                std::ofstream file(root / "frames" / "signatures.log", std::ios::app | std::ios::binary);
                file << std::format("signature {:016X} frame {} operations {}\n", signature, frame, operationCount) << joined << '\n';
            }
            catch (...)
            {
            }
    }
    ++form.frames;
    form.markedFrames += marked;
    auto&       instances = marked ? form.markedInstances : form.instances;
    const auto& spent     = marked ? markedBytes : frameBytes;
    if (truncated)
        ++truncatedFrames;
    else if (!armed)
        ++unarmedFrames;
    else if (form.writeFailed)
        ++failedFrames;
    else if (instances.size() < options.frameInstances && spent < options.frameBytes)
    {
        if (knownAt != knownForms.end())
            ++knownFrames;
        else if (covered)
            ++coveredFrames;
        else if (const auto name = std::format("{:016X}-{}{}", signature, marked ? "marked-" : "", instances.size()); write(name))
        {
            instances.push_back(name);
            if (cover)
                coverage.Add(*cover);
        }
        else
        {
            // Not retried: a failing write would otherwise list ever more
            // forms past the cap without spending the budget.
            form.writeFailed = true;
            ++failedFrames;
        }
    }
}

void ShaderHarvest::ReplayFrame(std::uint64_t signature, const std::vector<std::string>& canonical, bool marked, std::uint64_t bytes)
{
    std::lock_guard lock(mutex);
    if (!running)
        return;
    admitFrame(signature, canonical, {}, marked, canonical.size(), [&](const std::string&) -> std::optional<std::uint64_t>
               {
                   (marked ? markedBytes : frameBytes) += bytes;
                   return bytes;
               });
    ++frame;
}

void ShaderHarvest::Boundary(const ShaderHarvestRead& read)
{
    std::lock_guard lock(mutex);
    if (running && options.frameInstances)
        closeFrame(read);
    ++frame;
    if (running && dirty && frame - indexedFrame >= 60)
    {
        std::string error;
        writeIndexLocked(error);
    }
}

bool ShaderHarvest::WriteIndex(std::string& error)
{
    std::lock_guard lock(mutex);
    return writeIndexLocked(error);
}

void ShaderHarvest::Stop()
{
    std::lock_guard lock(mutex);
    if (!running)
        return;
    std::string error;
    dirty = true;
    writeIndexLocked(error);
    running = false;
}

std::vector<ShaderHarvestPair> ShaderHarvest::Pairs() const
{
    std::lock_guard lock(mutex);
    return pairs;
}

std::vector<ShaderHarvestFrameForm> ShaderHarvest::FrameForms() const
{
    std::lock_guard                     lock(mutex);
    std::vector<ShaderHarvestFrameForm> list;
    for (const auto signature : formOrder)
        list.push_back(forms.at(signature));
    return list;
}

std::uint64_t ShaderHarvest::Frame() const
{
    std::lock_guard lock(mutex);
    return frame;
}

ShaderHarvestFrameCounts ShaderHarvest::FrameCounts() const
{
    std::lock_guard lock(mutex);
    return { knownFrames, coveredFrames, unarmedFrames, unlistedFrames, truncatedFrames, failedFrames, frameBytes, markedBytes };
}

std::uint64_t ShaderHarvest::RetainedFrameBytes() const
{
    std::lock_guard lock(mutex);
    std::uint64_t   bytes = 0;
    for (const auto& operation : operations)
    {
        bytes += operation.state.capacity() + operation.blobs.capacity() * sizeof(Blob);
        for (const auto& blob : operation.blobs)
            bytes += blob.bytes.capacity();
    }
    return bytes;
}

bool ShaderHarvest::writeIndexLocked(std::string& error)
{
    if (!running)
        return true;
    toml::array list;
    for (const auto& pair : pairs)
    {
        toml::table row{ { "directory", pair.directory },
                         { "caller", address(pair.caller) },
                         { "entry", address(pair.entry) },
                         { "key_hash", std::format("{:016X}", pair.keyHash) },
                         { "vertex_hash", std::format("{:016X}", pair.vertexHash) },
                         { "pixel_hash", std::format("{:016X}", pair.pixelHash) },
                         { "vertex_word_sha256", pair.vertexWordDigest },
                         { "pixel_word_sha256", pair.pixelWordDigest },
                         { "vertex_only", pair.vertexOnly },
                         { "alpha_test", pair.alphaTest },
                         { "color_format", pair.colorFormat },
                         { "samples", pair.samples },
                         { "primitive", pair.primitive },
                         { "target_form", pair.targetForm },
                         { "first_frame", static_cast<std::int64_t>(pair.firstFrame) },
                         { "draws", static_cast<std::int64_t>(pair.draws) } };
        if (!pair.error.empty())
            row.insert("error", pair.error);
        if (!pair.knownHarvest.empty())
            row.insert("known_harvest", pair.knownHarvest);
        list.push_back(std::move(row));
    }
    toml::table entries;
    for (const auto& [entry, count] : entryDraws)
        entries.insert(address(entry), static_cast<std::int64_t>(count));
    toml::array knownList;
    for (const auto& prior : options.known)
        knownList.push_back(prior.string());
    toml::table index{ { "schema_version", 3 },
                       { "frames", static_cast<std::int64_t>(frame) },
                       { "draws", static_cast<std::int64_t>(draws) },
                       { "draws_without_shaders", static_cast<std::int64_t>(withoutShaders) },
                       { "unreadable_draws", static_cast<std::int64_t>(unreadable) },
                       { "write_failures", static_cast<std::int64_t>(failures) },
                       { "draws_by_entry", std::move(entries) },
                       { "known_harvests", knownList },
                       { "pairs", std::move(list) } };
    if (options.caller)
        index.insert("caller_filter", address(*options.caller));
    if (!firstError.empty())
        index.insert("first_error", firstError);
    try
    {
        // Replaced whole so a reader never sees a partial index.
        const auto replace = [](const std::filesystem::path& path, const toml::table& table)
        {
            std::ostringstream text;
            text << table << '\n';
            writeText(path.string() + ".tmp", text.str());
            std::filesystem::rename(path.string() + ".tmp", path);
        };
        replace(root / "index.toml", index);
        if (options.frameInstances)
        {
            toml::array signatures;
            for (const auto signature : formOrder)
            {
                const auto& form = forms.at(signature);
                toml::array instances, marked;
                for (const auto& name : form.instances)
                    instances.push_back(name);
                for (const auto& name : form.markedInstances)
                    marked.push_back(name);
                toml::table row{ { "signature", std::format("{:016X}", signature) },
                                 { "first_frame", static_cast<std::int64_t>(form.firstFrame) },
                                 { "frames", static_cast<std::int64_t>(form.frames) },
                                 { "draw_before_clear_frames", static_cast<std::int64_t>(form.markedFrames) },
                                 { "operations", static_cast<std::int64_t>(form.operations) },
                                 { "instances", std::move(instances) },
                                 { "draw_before_clear_instances", std::move(marked) } };
                if (!form.knownHarvest.empty())
                    row.insert("known_harvest", form.knownHarvest);
                if (form.writeFailed)
                    row.insert("write_failed", true);
                signatures.push_back(std::move(row));
            }
            toml::table frames{ { "schema_version", 2 },
                                { "frames", static_cast<std::int64_t>(frame) },
                                { "instances_per_signature", options.frameInstances },
                                { "budget_bytes", static_cast<std::int64_t>(options.frameBytes) },
                                { "bytes_written", static_cast<std::int64_t>(frameBytes) },
                                { "draw_before_clear_bytes_written", static_cast<std::int64_t>(markedBytes) },
                                { "truncated_frames", static_cast<std::int64_t>(truncatedFrames) },
                                { "signature_limit", static_cast<std::int64_t>(options.frameSignatures) },
                                { "unlisted_frames", static_cast<std::int64_t>(unlistedFrames) },
                                { "known_harvests", std::move(knownList) },
                                { "known_frames", static_cast<std::int64_t>(knownFrames) },
                                { "covered_frames", static_cast<std::int64_t>(coveredFrames) },
                                { "unarmed_frames", static_cast<std::int64_t>(unarmedFrames) },
                                { "failed_frames", static_cast<std::int64_t>(failedFrames) },
                                { "signatures", std::move(signatures) } };
            if (!options.armFile.empty())
            {
                frames.insert("arm_file", options.armFile.string());
                frames.insert("armed_frame", armedFrame ? static_cast<std::int64_t>(*armedFrame) : -1);
            }
            replace(root / "frames" / "index.toml", frames);
        }
        indexedFrame = frame;
        dirty        = false;
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        ++failures;
        return false;
    }
}

void HarvestOperation(ShaderHarvest& harvest, const ShaderHarvestCall& call, const ShaderHarvestRead& read) noexcept
{
    harvest.Observe(call, read);
}

bool ParseShaderHarvestCaller(const std::string& text, std::uint32_t& caller)
{
    if (text.size() != 10 || !(text.starts_with("0x") || text.starts_with("0X")))
        return false;
    std::uint32_t value = 0;
    for (const auto c : std::string_view(text).substr(2))
    {
        const auto digit = std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : std::isxdigit(static_cast<unsigned char>(c)) ? (std::tolower(static_cast<unsigned char>(c)) - 'a' + 10)
                                                                                                                                : -1;
        if (digit < 0)
            return false;
        value = value << 4 | static_cast<std::uint32_t>(digit);
    }
    caller = value;
    return true;
}

namespace
{

// Never freed: an operation that loaded the observer before Stop may still run.
ShaderHarvest     processHarvest;
ShaderHarvestRead processRead;
std::mutex        processLifecycle;
bool              processStarted = false;

void observeProcessOperation(const ShaderHarvestCall& call) noexcept
{
    processHarvest.Observe(call, processRead);
}

void observeProcessBoundary() noexcept
{
    try
    {
        processHarvest.Boundary(processRead);
    }
    catch (...)
    {
    }
}

} // namespace

bool StartShaderHarvest(const std::filesystem::path& directory, const ShaderHarvestOptions& options, ShaderHarvestRead read, std::string& error)
{
    std::lock_guard lock(processLifecycle);
    if (processStarted || !read)
    {
        error = "shader harvest runs once per process with a guest memory reader";
        return false;
    }
    if (!processHarvest.Start(directory, options, error))
        return false;
    processRead    = std::move(read);
    processStarted = true;
    shaderHarvestBoundary.store(observeProcessBoundary, std::memory_order_release);
    shaderHarvestObserver.store(observeProcessOperation, std::memory_order_release);
    REXLOG_INFO("Shader harvest recording {}{}{}{}{}",
                directory.string(),
                options.caller ? std::format(" for caller 0x{:08X}", *options.caller) : std::string(),
                options.frameInstances ? std::format(" with {} frame form instances per signature", options.frameInstances) : std::string(),
                options.known.empty() ? std::string() : std::format(", skipping what {} known harvests hold", options.known.size()),
                options.armFile.empty() ? std::string() : ", writing frames once " + options.armFile.string() + " exists");
    return true;
}

void StopShaderHarvest() noexcept
{
    shaderHarvestBoundary.store(nullptr, std::memory_order_release);
    if (shaderHarvestObserver.exchange(nullptr, std::memory_order_acq_rel))
        processHarvest.Stop();
}

} // namespace rerevved::gpu::diagnostics
