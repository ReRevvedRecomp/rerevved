#include "native_pixel_lod_fixture.h"

#include "gpu/d3d12/native_gameplay_shaders.h"
#include "gpu/d3d12/native_renderer_d3d12.h"
#include "helper_commands.hpp"
#include "native_argv.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rex/crypto/sha256.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

using namespace rerevved::gpu;
using rex::graphics::testing::ExpectedNativePixelLodPixels;
using rex::graphics::testing::MakeNativePixelLodFixture;
using rex::graphics::testing::NativePixelLodDrawRecord;
using rex::graphics::testing::NativePixelLodFixture;
using rex::graphics::testing::NativePixelLodOriginalParams;
using rex::graphics::testing::NativePixelLodOriginalResult;
using rex::graphics::testing::RunNativePixelLodOriginal;

namespace
{

constexpr std::uint32_t kMipCount = rex::graphics::testing::kNativePixelLodMipCount;
constexpr std::uint32_t kWidth    = rex::graphics::testing::kNativePixelLodWidth;
constexpr std::uint32_t kHeight   = rex::graphics::testing::kNativePixelLodHeight;
constexpr std::size_t   kPlane    = static_cast<std::size_t>(kWidth) * kHeight * 4;

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message)
{
    if (!condition)
        fail(message);
}

void phase(std::uint32_t pairIndex, const char* name)
{
    std::cerr << "native_pixel_lod pair=" << pairIndex << " phase=" << name << std::endl;
}

struct HarnessLogging
{
    HarnessLogging()
    {
        rex::LogConfig config;
        config.log_to_console = true;
        rex::InitLogging(config);
    }

    ~HarnessLogging()
    {
        rex::ShutdownLogging();
    }
};

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "could not open " + path.string());
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    require(size >= 0, "could not size " + path.string());
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty())
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(input.good() || input.eof(), "could not read " + path.string());
    return bytes;
}

void writeBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "could not create " + path.string());
    if (!bytes.empty())
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    require(output.good(), "could not write " + path.string());
}

std::vector<std::uint8_t> wordsBytes(std::span<const std::uint32_t> words)
{
    std::vector<std::uint8_t> bytes(words.size() * 4);
    for (std::size_t index = 0; index < words.size(); ++index)
    {
        bytes[index * 4]     = static_cast<std::uint8_t>(words[index]);
        bytes[index * 4 + 1] = static_cast<std::uint8_t>(words[index] >> 8);
        bytes[index * 4 + 2] = static_cast<std::uint8_t>(words[index] >> 16);
        bytes[index * 4 + 3] = static_cast<std::uint8_t>(words[index] >> 24);
    }
    return bytes;
}

std::string wordDigest(std::span<const std::uint32_t> words)
{
    const auto bytes = wordsBytes(words);
    return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::string tomlString(std::string_view value)
{
    std::string result = "\"";
    for (const auto character : value)
    {
        if (character == '\\' || character == '"')
            result.push_back('\\');
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}

std::vector<std::uint8_t> textureBytes(const NativePixelLodFixture& fixture)
{
    std::vector<std::uint8_t> bytes;
    for (const auto& mip : fixture.texture.mips)
        bytes.insert(bytes.end(), mip.linear_rgba8.begin(), mip.linear_rgba8.end());
    return bytes;
}

std::vector<std::uint8_t> textureBytesWithoutMip(const NativePixelLodFixture& fixture, std::uint32_t omittedMip)
{
    require(omittedMip < fixture.texture.mips.size(), "omitted mip is outside the fixture range");
    std::vector<std::uint8_t> bytes;
    for (std::uint32_t index = 0; index < fixture.texture.mips.size(); ++index)
    {
        if (index != omittedMip)
            bytes.insert(bytes.end(), fixture.texture.mips[index].linear_rgba8.begin(), fixture.texture.mips[index].linear_rgba8.end());
    }
    return bytes;
}

void setBigEndianFloat(std::vector<std::uint8_t>& bytes, std::size_t offset, float value)
{
    const auto bits = std::bit_cast<std::uint32_t>(value);
    require(offset + 4 <= bytes.size(), "fixture vertex scalar offset exceeds owned input");
    bytes[offset]     = static_cast<std::uint8_t>(bits >> 24);
    bytes[offset + 1] = static_cast<std::uint8_t>(bits >> 16);
    bytes[offset + 2] = static_cast<std::uint8_t>(bits >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(bits);
}

NativePixelLodFixture fixtureForPair(std::uint32_t first, std::uint32_t second)
{
    auto fixture                     = MakeNativePixelLodFixture();
    fixture.selected_lods            = { first, second };
    fixture.expected_selected_pixels = ExpectedNativePixelLodPixels(fixture, first, second);
    require(!fixture.expected_selected_pixels.empty(), "fixture did not provide expected pair pixels");
    for (std::size_t vertex = 0; vertex < 3; ++vertex)
    {
        const auto base = vertex * fixture.vertex_stride;
        setBigEndianFloat(fixture.vertex_bytes, base + 24, static_cast<float>(first));
        setBigEndianFloat(fixture.vertex_bytes, base + 28, static_cast<float>(second));
    }
    return fixture;
}

bool sameAttributes(std::span<const NativeGameplayVertexAttribute> left,
                    std::span<const NativeGameplayVertexAttribute> right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.fetchConstant != b.fetchConstant || a.strideBytes != b.strideBytes || a.offsetBytes != b.offsetBytes ||
            a.format != b.format || a.isSigned != b.isSigned || a.normalized != b.normalized)
            return false;
    }
    return true;
}

NativeGameplayShaderInterface loadShader(const std::filesystem::path&   catalog,
                                         std::span<const std::uint32_t> vertexWords,
                                         std::span<const std::uint32_t> pixelWords,
                                         std::string_view               role)
{
    std::vector<NativeGameplayShaderInterface> shaders;
    std::string                                error;
    const bool                                 loaded = LoadNativeGameplayShaders(catalog, shaders, error);
    require(loaded, std::string(role) + " catalog: " + error);
    require(shaders.size() == 1, std::string(role) + " catalog must contain one shader pair");
    auto shader = std::move(shaders.front());
    require(shader.vertexGuestWordDigest == wordDigest(vertexWords) && shader.pixelGuestWordDigest == wordDigest(pixelWords),
            std::string(role) + " catalog is not bound to the exact fixture words");
    require(shader.textureFetchSlots.size() == 1 && shader.textureFetchSlots[0] == 0 &&
                shader.textureDimensions[0] == NativeDrawReplayTextureDimension::Texture2D,
            std::string(role) + " catalog must bind one guest slot-0 Texture2D");
    require(!shader.vertexAttributes.empty(), std::string(role) + " catalog has no vertex attributes");
    return shader;
}

struct DrawInputs
{
    std::vector<std::uint32_t>    registers;
    std::vector<std::uint8_t>     vertices;
    std::vector<std::uint8_t>     indices;
    std::vector<std::uint8_t>     textureBytes;
    NativeDrawReplayTexture       texture;
    NativeDrawReplaySampler       sampler;
    NativeGameplayVertexFetchSpan fetch;
    NativeGameplayTextureBinding  binding;
    NativeGameplayDrawView        view;
};

DrawInputs makeInputs(const NativePixelLodFixture&   fixture,
                      std::vector<std::uint8_t>      texturePayload,
                      std::span<const std::uint32_t> sourceRegisters,
                      std::uint32_t                  vertexGuestBase,
                      std::uint32_t                  mipLevels = kMipCount)
{
    DrawInputs result;
    require(!sourceRegisters.empty(), "native replay requires the actual original source register bank");
    result.registers         = std::vector<std::uint32_t>(sourceRegisters.begin(), sourceRegisters.end());
    result.vertices          = fixture.vertex_bytes;
    result.indices           = fixture.index_bytes;
    result.textureBytes      = std::move(texturePayload);
    result.texture.width     = fixture.width;
    result.texture.height    = fixture.height;
    result.texture.format    = NativeDrawReplayTextureFormat::Rgba8;
    result.texture.mipLevels = mipLevels;
    result.texture.dimension = NativeDrawReplayTextureDimension::Texture2D;
    result.sampler.minLinear = false;
    result.sampler.magLinear = false;
    result.sampler.mipLinear = false;
    result.sampler.address   = { 3, 3, 3 };
    result.sampler.minLod    = 0.0F;
    result.sampler.maxLod    = 7.0F;
    result.view.primitive    = 4;
    result.view.indexCount   = 3;
    result.view.indexFormat  = 0;
    result.view.indexEndian  = 1;
    result.view.indexed      = true;
    require(result.registers.size() == rex::graphics::testing::kNativePixelLodRegisterCount,
            "fixture register bank has the wrong extent");
    return result;
}

NativeDrawReplayRecipe buildRecipe(const NativePixelLodFixture&         fixture,
                                   const NativeGameplayShaderInterface& shader,
                                   const std::filesystem::path&         output,
                                   const std::vector<std::uint8_t>&     payload,
                                   std::uint32_t                        mipLevels       = kMipCount,
                                   std::span<const std::uint32_t>       sourceRegisters = {},
                                   std::uint32_t                        vertexGuestBase = 0)
{
    auto inputs = makeInputs(fixture, payload, sourceRegisters, vertexGuestBase, mipLevels);
    // makeInputs owns the vectors, but its returned view may have been moved.
    // Rebind every span and pointer to the final local owner before conversion.
    inputs.texture.bytes        = inputs.textureBytes;
    inputs.fetch                = { 95, vertexGuestBase, inputs.vertices };
    inputs.binding              = { 0, &inputs.texture, &inputs.sampler };
    inputs.view.registers       = inputs.registers;
    inputs.view.vertexMicrocode = fixture.vertex_shader_words;
    inputs.view.pixelMicrocode  = fixture.pixel_shader_words;
    inputs.view.vertexFetches   = std::span<const NativeGameplayVertexFetchSpan>(&inputs.fetch, 1);
    inputs.view.indexBytes      = inputs.indices;
    inputs.view.textureBindings = std::span<const NativeGameplayTextureBinding>(&inputs.binding, 1);
    NativeGameplayDrawTarget target;
    target.width       = kWidth;
    target.height      = kHeight;
    target.sampleCount = 1;
    target.sampleMask  = 1;
    target.format      = NativeDrawReplayTargetFormat::Rgba8;
    const auto zScale  = std::bit_cast<float>(inputs.registers[0x2113U]);
    const auto zOffset = std::bit_cast<float>(inputs.registers[0x2114U]);
    target.viewport    = { 0.0F, 0.0F, static_cast<float>(kWidth), static_cast<float>(kHeight), zOffset, zOffset + zScale };
    target.scissor     = { 0, 0, static_cast<std::int32_t>(kWidth), static_cast<std::int32_t>(kHeight) };
    NativeDrawReplayRecipe recipe;
    std::string            error;
    const bool             built = BuildNativeGameplayDrawRecipe(inputs.view, shader, target, recipe, error);
    require(built, error);
    recipe.outputPath = output;
    return recipe;
}

std::vector<std::uint8_t> readNativeOutput(const std::filesystem::path& path)
{
    const auto bytes = readBytes(path);
    require(bytes.size() == 2 * kPlane, "native output must contain two complete RGBA8 planes");
    return bytes;
}

void compare(std::span<const std::uint8_t> actual, std::span<const std::uint8_t> expected, std::string_view label)
{
    require(actual.size() == expected.size(), std::string(label) + " size differs");
    const auto mismatch = std::ranges::mismatch(actual, expected);
    if (mismatch.in1 != actual.end())
        fail(std::string(label) + " differs at byte " + std::to_string(mismatch.in1 - actual.begin()));
}

std::vector<std::uint8_t> duplicatePlane(std::span<const std::uint8_t> plane)
{
    require(plane.size() == kPlane, "original terminal must be one complete RGBA8 plane");
    std::vector<std::uint8_t> result;
    result.insert(result.end(), plane.begin(), plane.end());
    result.insert(result.end(), plane.begin(), plane.end());
    return result;
}

std::uint32_t sourceVertexGuestBase(const NativePixelLodOriginalResult& original)
{
    require(original.source_vertex_fetch_words[0] != 0, "original source vertex fetch was absent");
    require((original.source_vertex_fetch_words[0] & 3U) == static_cast<std::uint32_t>(rex::graphics::xenos::FetchConstantType::kVertex),
            "original source vertex fetch lost its guest vertex type");
    return original.source_vertex_fetch_words[0] & ~3U;
}

std::array<std::uint32_t, 6> fetchWords(std::span<const std::uint32_t> registers, std::uint32_t slot)
{
    const auto address = 0x4800U + slot * 2U;
    require(registers.size() >= address + 6U, "source register bank lacks a fetch descriptor");
    std::array<std::uint32_t, 6> result{};
    std::copy_n(registers.begin() + address, result.size(), result.begin());
    return result;
}

void assertOriginalSource(const NativePixelLodFixture&        expected,
                          const NativePixelLodOriginalResult& original)
{
    constexpr std::uint32_t kCpRbWptr         = 0x1C5U;
    constexpr std::uint32_t kVgtDmaBase       = 0x21FAU;
    constexpr std::uint32_t kVgtDmaSize       = 0x21FBU;
    constexpr std::uint32_t kVgtDrawInitiator = 0x21FCU;
    constexpr std::uint32_t kTextureFetch     = 0x4800U;
    constexpr std::uint32_t kVertexFetch      = kTextureFetch + 2U * 95U;

    require(original.source_fixture.vertex_shader_words == expected.vertex_shader_words &&
                original.source_fixture.pixel_shader_words == expected.pixel_shader_words,
            "original source fixture changed its raw shader words");
    require(original.source_fixture.vertex_bytes == expected.vertex_bytes &&
                original.source_fixture.index_bytes == expected.index_bytes &&
                original.source_fixture.texture.base_guest_bytes == expected.texture.base_guest_bytes &&
                original.source_fixture.texture.mip_guest_bytes == expected.texture.mip_guest_bytes,
            "original source fixture changed its owned raw input bytes");
    require(original.source_registers.size() == rex::graphics::testing::kNativePixelLodRegisterCount,
            "original source draw did not retain the full register bank");
    require(original.source_fixture.register_image == original.source_registers,
            "original source fixture did not retain the issued register image");
    require(original.source_vertex_fetch_words == fetchWords(original.source_registers, 95),
            "original source vertex fetch identity was not retained");
    require(original.source_texture_fetch_words == fetchWords(original.source_registers, 0),
            "original source texture fetch identity was not retained");
    require(original.source_fixture.texture.fetch_words == original.source_texture_fetch_words &&
                original.source_fixture.vertex_fetch_words == original.source_vertex_fetch_words,
            "original source fixture did not retain the issued fetch descriptors");

    const auto expectedTexture = fetchWords(expected.register_image, 0U);
    const auto expectedVertex  = fetchWords(expected.register_image, 95U);
    require((expectedTexture[1] & 0xFFFFF000U) == 0U && (expectedTexture[5] & 0xFFFFF000U) == 0U,
            "fixture texture fetch address fields changed from their guest-image placeholders");
    require(original.published_word_count != 0, "original source did not retain its published ring word count");
    require(original.source_index_guest_base != 0, "original source did not retain its index guest base");
    require(original.source_texture_fetch_words[1] != 0 && original.source_texture_fetch_words[5] != 0,
            "original source texture fetch did not retain physical base and mip addresses");
    require((original.source_vertex_fetch_words[0] & ~3U) != 0,
            "original source vertex fetch did not retain its physical address");

    // The CPU ring progress, DMA index address, and three physical fetch
    // address words are the only per-run values. Every other register and
    // every non-address fetch field must retain the fixture's exact state.
    for (std::uint32_t index = 0; index < original.source_registers.size(); ++index)
    {
        const auto actual = original.source_registers[index];
        if (index == kCpRbWptr)
        {
            require(actual == original.published_word_count,
                    "original CP ring write pointer did not match the published word count");
            continue;
        }
        if (index == kVgtDmaBase)
        {
            require(actual == original.source_index_guest_base,
                    "original DMA base did not match the issued index guest base");
            continue;
        }
        if (index == kVgtDmaSize)
        {
            require(expected.register_image[index] == 0x40000003U && actual == expected.register_image[index],
                    "original DMA size changed from the authored index contract");
            continue;
        }
        if (index == kVgtDrawInitiator)
        {
            require(expected.register_image[index] == 0x30004U && actual == expected.register_image[index],
                    "original draw initiator changed from the authored index contract");
            continue;
        }
        if (index == kTextureFetch + 1U || index == kTextureFetch + 5U)
        {
            const auto word = index - kTextureFetch;
            require((actual & 0xFFFFF000U) != 0U && (actual & 0xFFFU) == (expectedTexture[word] & 0xFFFU),
                    "original texture physical address or descriptor flags changed");
            continue;
        }
        if (index == kVertexFetch)
        {
            require((actual & 3U) == (expectedVertex[0] & 3U),
                    "original vertex fetch changed its non-address type bits");
            require((actual & ~3U) == (original.source_vertex_fetch_words[0] & ~3U),
                    "original vertex physical address was not retained in the source descriptor");
            continue;
        }
        require(actual == expected.register_image[index],
                "original source register state differs from the exact fixture state at " + std::to_string(index));
    }

    const auto draw = std::ranges::find_if(
        original.draws,
        [&](const auto& item)
        {
            return item.vertex_shader_words == expected.vertex_shader_words &&
                   item.pixel_shader_words == expected.pixel_shader_words;
        });
    require(draw != original.draws.end(), "original source draw identity was not observed");
    require(draw->registers == original.source_registers && draw->original_host_issued && draw->cpu_data_complete &&
                draw->execution_status == 1U && draw->execution_reason == 0U && draw->primitive_type == 4U &&
                draw->requested_index_count == 3U && original.source_draw_issued,
            "original source draw did not retain the issued DMA triangle contract");
}

void writeOriginalDiagnostics(const std::filesystem::path&        outputRoot,
                              std::uint32_t                       pairIndex,
                              std::uint32_t                       first,
                              std::uint32_t                       second,
                              const NativePixelLodOriginalResult& original)
{
    const auto terminalPath       = outputRoot / ("original-" + std::to_string(pairIndex) + "-terminal.rgba8.bin");
    const auto expectedPath       = outputRoot / ("original-" + std::to_string(pairIndex) + "-expected.rgba8.bin");
    const auto sourceRegisterPath = outputRoot / ("original-" + std::to_string(pairIndex) + "-source-registers.le.bin");
    writeBytes(terminalPath, original.terminal_pixels);
    writeBytes(expectedPath, original.expected_pixels);
    writeBytes(sourceRegisterPath, wordsBytes(original.source_registers));
    const auto sourceDraw = std::ranges::find_if(
        original.draws,
        [&](const auto& item)
        {
            return item.vertex_shader_words == original.source_fixture.vertex_shader_words &&
                   item.pixel_shader_words == original.source_fixture.pixel_shader_words;
        });
    const bool                      sourceDrawObserved = sourceDraw != original.draws.end();
    const NativePixelLodDrawRecord* sourceDrawRecord   = sourceDrawObserved ? &*sourceDraw : nullptr;
    std::ofstream                   report(outputRoot / ("original-" + std::to_string(pairIndex) + ".toml"),
                                           std::ios::binary | std::ios::trunc);
    require(report.good(), "could not create original pixel LOD diagnostics");
    report << "schema_version = 1\nuv_bias_base_texels = 0.25\ntexel_edge_guard = true\npair_index = " << pairIndex << "\nfirst_lod = " << first
           << "\nsecond_lod = " << second << "\nprogressed = " << (original.progressed ? "true" : "false")
           << "\ncompletion_observed = " << (original.completion_observed ? "true" : "false")
           << "\nterminal_capture = " << (original.terminal_capture ? "true" : "false")
           << "\nactual_readback_owner = " << (original.actual_readback_owner ? "true" : "false")
           << "\nfailure = " << tomlString(original.failure)
           << "\nsource_draw_issued = " << (original.source_draw_issued ? "true" : "false")
           << "\nsource_draw_observed = " << (sourceDrawObserved ? "true" : "false")
           << "\nsource_draw_event_order = " << (sourceDrawRecord ? sourceDrawRecord->event_ordinal : 0)
           << "\nsource_draw_id = " << (sourceDrawRecord ? sourceDrawRecord->id : 0)
           << "\nsource_draw_frame = " << (sourceDrawRecord ? sourceDrawRecord->frame : 0)
           << "\nsource_draw_submission = " << (sourceDrawRecord ? sourceDrawRecord->submission : 0)
           << "\nsource_original_host_issued = "
           << (sourceDrawRecord && sourceDrawRecord->original_host_issued ? "true" : "false")
           << "\nsource_cpu_data_complete = "
           << (sourceDrawRecord && sourceDrawRecord->cpu_data_complete ? "true" : "false")
           << "\nsource_execution_status = " << (sourceDrawRecord ? sourceDrawRecord->execution_status : 0)
           << "\nsource_execution_reason = " << (sourceDrawRecord ? sourceDrawRecord->execution_reason : 0)
           << "\nsource_draw_primitive_type = " << (sourceDrawRecord ? sourceDrawRecord->primitive_type : 0)
           << "\nsource_draw_requested_index_count = "
           << (sourceDrawRecord ? sourceDrawRecord->requested_index_count : 0)
           << "\nsource_index_guest_base = " << original.source_index_guest_base
           << "\npublished_word_count = " << original.published_word_count
           << "\nterminal_pixels_match_expected = " << (original.terminal_pixels_match_expected ? "true" : "false")
           << "\nterminal_width = " << original.terminal_width << "\nterminal_height = " << original.terminal_height
           << "\nterminal_stride = " << original.terminal_stride
           << "\nsource_register_count = " << original.source_registers.size()
           << "\ncompleted_fence = " << original.completed_fence
           << "\ncompletion_copy_id = " << original.completion_copy_id
           << "\ncompletion_swap_id = " << original.completion_swap_id
           << "\ncompletion_frame = " << original.completion_frame
           << "\ncompletion_submission = " << original.completion_submission
           << "\ncompletion_fence_reported = false\nsource_vertex_word_sha256 = \""
           << wordDigest(original.source_fixture.vertex_shader_words) << "\"\nsource_pixel_word_sha256 = \""
           << wordDigest(original.source_fixture.pixel_shader_words) << "\"\nsource_texture_fetch_words = [";
    for (std::size_t index = 0; index < original.source_texture_fetch_words.size(); ++index)
        report << (index ? ", " : "") << original.source_texture_fetch_words[index];
    report << "]\nsource_vertex_fetch_words = [";
    for (std::size_t index = 0; index < original.source_vertex_fetch_words.size(); ++index)
        report << (index ? ", " : "") << original.source_vertex_fetch_words[index];
    report << "]\nreadback_owner = " << tomlString(original.readback_owner) << "\nsource_register_file = \""
           << sourceRegisterPath.filename().string() << "\"\nexpected_file = \""
           << expectedPath.filename().string() << "\"\nterminal_file = \""
           << terminalPath.filename().string() << "\"\nresolve_readback_readback_owner = false\n";

    const auto writeCopies = [&](std::string_view label, const auto& records)
    {
        report << label << "_count = " << records.size() << "\n";
        for (std::size_t index = 0; index < records.size(); ++index)
        {
            const auto& record = records[index];
            const auto  key    = std::string(label) + "_" + std::to_string(index);
            report << key << "_event_order = " << record.event_ordinal << "\n"
                   << key << "_id = " << record.id << "\n"
                   << key << "_frame = " << record.frame << "\n"
                   << key << "_submission = " << record.submission << "\n"
                   << key << "_mode = " << tomlString(record.mode) << "\n"
                   << key << "_mode_is_resolve_readback = "
                   << (record.mode == "resolve_readback" ? "true" : "false") << "\n"
                   << key << "_original_succeeded = " << (record.original_succeeded ? "true" : "false") << "\n"
                   << key << "_cpu_data_complete = " << (record.cpu_data_complete ? "true" : "false") << "\n"
                   << key << "_written_address = " << record.written_address << "\n"
                   << key << "_written_length = " << record.written_length << "\n"
                   << key << "_planned_address = " << record.planned_address << "\n"
                   << key << "_planned_length = " << record.planned_length << "\n"
                   << key << "_register_count = " << record.registers.size() << "\n"
                   << key << "_readback_owner = false\n";
        }
    };
    const auto writeSwaps = [&](const auto& records)
    {
        report << "swap_count = " << records.size() << "\n";
        for (std::size_t index = 0; index < records.size(); ++index)
        {
            const auto& record = records[index];
            const auto  key    = std::string("swap_") + std::to_string(index);
            report << key << "_event_order = " << record.event_ordinal << "\n"
                   << key << "_id = " << record.id << "\n"
                   << key << "_frame = " << record.frame << "\n"
                   << key << "_submission = " << record.submission << "\n"
                   << key << "_frontbuffer_ptr = " << record.frontbuffer_ptr << "\n"
                   << key << "_width = " << record.width << "\n"
                   << key << "_height = " << record.height << "\n"
                   << key << "_original_succeeded = " << (record.original_succeeded ? "true" : "false") << "\n";
        }
    };
    const auto writeFrameEnds = [&](const auto& records)
    {
        report << "frame_end_count = " << records.size() << "\n";
        for (std::size_t index = 0; index < records.size(); ++index)
        {
            const auto& record = records[index];
            const auto  key    = std::string("frame_end_") + std::to_string(index);
            report << key << "_event_order = " << record.event_ordinal << "\n"
                   << key << "_frame = " << record.frame << "\n"
                   << key << "_submission = " << record.submission << "\n"
                   << key << "_original_succeeded = " << (record.original_succeeded ? "true" : "false") << "\n";
        }
    };
    writeCopies("copy", original.copies);
    writeCopies("prepared_copy", original.prepared_copies);
    writeSwaps(original.swaps);
    writeFrameEnds(original.frame_ends);
    report.flush();
    require(report.good(), "could not flush original pixel LOD diagnostics");
}

void assertOriginalCompletion(const NativePixelLodOriginalResult& original)
{
    require(original.completion_copy_id != 0 && original.completion_swap_id != 0,
            "original completion did not retain copy and swap identities");
    const auto copy = std::ranges::find_if(
        original.copies,
        [&](const auto& record)
        {
            return record.id == original.completion_copy_id;
        });
    const auto swap = std::ranges::find_if(
        original.swaps,
        [&](const auto& record)
        {
            return record.id == original.completion_swap_id;
        });
    require(copy != original.copies.end() && swap != original.swaps.end(),
            "original completion identities were not retained in callback records");
    require(copy->original_succeeded && copy->written_length != 0 && copy->event_ordinal < swap->event_ordinal &&
                copy->frame == swap->frame && copy->submission == swap->submission && swap->frontbuffer_ptr != 0,
            "original completion copy and swap were not ordered in one successful submission");
    require(copy->frame == original.completion_frame && copy->submission == original.completion_submission,
            "original completion frame identity differs from the matched copy");
    const auto frameEnd = std::ranges::find_if(
        original.frame_ends,
        [&](const auto& record)
        {
            return record.frame == swap->frame && record.submission == swap->submission &&
                   record.event_ordinal > swap->event_ordinal && record.original_succeeded;
        });
    require(frameEnd != original.frame_ends.end(),
            "original completion lacks a successful frame end after the matched swap");
}

void assertCandidateSource(const std::filesystem::path& source)
{
    const auto bytes       = readBytes(source);
    const auto text        = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto setter      = text.find("registerLod = r0.z;");
    const auto second      = text.find("registerLod = r0.w;");
    const auto firstFetch  = text.find("= tfetch2DRegisterLod");
    const auto secondFetch = firstFetch == std::string::npos
                                 ? std::string::npos
                                 : text.find("= tfetch2DRegisterLod", firstFetch + 1);
    const auto thirdFetch  = secondFetch == std::string::npos
                                 ? std::string::npos
                                 : text.find("= tfetch2DRegisterLod", secondFetch + 1);
    require(setter != std::string::npos && second != std::string::npos && firstFetch != std::string::npos &&
                secondFetch != std::string::npos && thirdFetch == std::string::npos && setter < firstFetch &&
                firstFetch < second && second < secondFetch,
            "candidate source does not retain two ordered non-x register LOD pairs");
    require(text.find("SampleLevel") != std::string::npos, "candidate source lacks explicit SampleLevel");
    require(text.find("= tfetch2D(") == std::string::npos,
            "candidate source retained an implicit computed-LOD fetch");
}

float readUvFloat(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    const auto word = (std::uint32_t(bytes[offset]) << 24U) | (std::uint32_t(bytes[offset + 1U]) << 16U) |
                      (std::uint32_t(bytes[offset + 2U]) << 8U) | bytes[offset + 3U];
    return std::bit_cast<float>(word);
}

void assertFixtureGuards()
{
    const auto fixture = MakeNativePixelLodFixture();
    require(fixture.width == kWidth && fixture.height == kHeight && fixture.vertex_stride == 56,
            "fixture dimensions or vertex stride changed");
    require(fixture.register_image.size() == rex::graphics::testing::kNativePixelLodRegisterCount,
            "fixture register image extent changed");
    require(fixture.vertex_bytes.size() == 3 * 56 && fixture.index_bytes.size() == 6,
            "fixture geometry extent changed");
    for (std::size_t vertex = 0; vertex < 3U; ++vertex)
        require(readUvFloat(fixture.vertex_bytes, vertex * fixture.vertex_stride + 16U) == 0.5F + 0.25F / kWidth &&
                    readUvFloat(fixture.vertex_bytes, vertex * fixture.vertex_stride + 20U) == 0.5F + 0.25F / kHeight,
                "fixture fixed UV lost its quarter-base-texel bias");
    // Original's +0.5 geometry shift interpolates at integer positions.
    // SDK dxbc_translator_fetch.cpp adds 1.5/1024 of a base texel.
    for (const auto& mip : fixture.texture.mips)
        for (const auto axis : std::array<std::array<std::uint32_t, 2>, 2>{ { { kWidth, mip.width }, { kHeight, mip.height } } })
        {
            const double margin   = (1.5 / 1024.0) * axis[1] / axis[0];
            const double original = (0.5 + 0.25 / axis[0]) * axis[1];
            const double center   = original;
            const auto   texel    = static_cast<std::uint32_t>(original);
            require(texel == static_cast<std::uint32_t>(center) &&
                        original > texel + margin && original < texel + 1.0 - margin &&
                        center > texel + margin && center < texel + 1.0 - margin &&
                        original < axis[1] && center < axis[1],
                    "fixed UV hits a texel edge or leaves the level");
        }
    require(fixture.texture.mips.size() == kMipCount && fixture.expected_by_lod_pair.size() == kMipCount * kMipCount,
            "fixture mip or expected-pair extent changed");
    for (std::uint32_t first = 0; first < kMipCount; ++first)
        for (std::uint32_t second = 0; second < kMipCount; ++second)
            require(ExpectedNativePixelLodPixels(fixture, first, second).size() == kPlane,
                    "fixture expected-pair pixels are incomplete");
}

void emitFixture(const std::filesystem::path& output)
{
    require(!std::filesystem::exists(output), "fixture output must be fresh");
    std::filesystem::create_directories(output);
    const auto fixture = MakeNativePixelLodFixture();
    writeBytes(output / "registers.le.bin", wordsBytes(fixture.register_image));
    writeBytes(output / "vertex-ucode.le.bin", wordsBytes(fixture.vertex_shader_words));
    writeBytes(output / "pixel-ucode.le.bin", wordsBytes(fixture.pixel_shader_words));
    writeBytes(output / "vertices.bin", fixture.vertex_bytes);
    writeBytes(output / "indices.bin", fixture.index_bytes);
    writeBytes(output / "texture.rgba8.bin", textureBytes(fixture));
    std::ofstream metadata(output / "fixture.json", std::ios::binary | std::ios::trunc);
    require(metadata.good(), "could not create fixture metadata");
    metadata << "{\n  \"schema_version\": 1,\n  \"uv_bias_base_texels\": 0.25,\n  \"width\": 128,\n  \"height\": 128,\n"
             << "  \"mip_levels\": 8,\n  \"vertex_fetch_constant\": 95,\n"
             << "  \"vertex_stride_bytes\": 56,\n  \"lod_source_register\": 0,\n"
             << "  \"lod_source_swizzles\": [\"z\", \"w\"],\n"
             << "  \"computed_lod_enabled\": false,\n  \"register_lod_enabled\": true,\n"
             << "  \"setter_fetch_pairs\": 2,\n  \"vertex_word_sha256\": \""
             << wordDigest(fixture.vertex_shader_words) << "\",\n  \"pixel_word_sha256\": \""
             << wordDigest(fixture.pixel_shader_words) << "\"\n}\n";
    metadata.flush();
    require(metadata.good(), "could not flush fixture metadata");
    std::cout << output / "fixture.json" << '\n';
}

NativePixelLodOriginalResult runOriginal(std::uint32_t first, std::uint32_t second)
{
    NativePixelLodOriginalParams params;
    params.fixture    = fixtureForPair(first, second);
    params.first_lod  = first;
    params.second_lod = second;
    NativePixelLodOriginalResult result;
    const auto                   fallbackFixture  = params.fixture;
    const auto                   fallbackExpected = ExpectedNativePixelLodPixels(params.fixture, first, second);
    std::string                  error;
    try
    {
        const auto status = RunNativePixelLodOriginal(params, result, error);
        if (result.source_fixture.vertex_shader_words.empty())
            result.source_fixture = fallbackFixture;
        if (result.expected_pixels.empty())
            result.expected_pixels = fallbackExpected;
        if (status != 0 && result.failure.empty())
            result.failure = error.empty() ? "original runner failed without an error" : error;
    }
    catch (const std::exception& exception)
    {
        result.source_fixture  = fallbackFixture;
        result.expected_pixels = fallbackExpected;
        result.failure         = exception.what();
    }
    return result;
}

int run(const std::filesystem::path& candidateCatalog,
        const std::filesystem::path& controlCatalog,
        const std::filesystem::path& candidateSource,
        const std::filesystem::path& outputRoot)
{
    require(!std::filesystem::exists(outputRoot), "native pixel LOD output must be fresh");
    std::filesystem::create_directories(outputRoot);
    phase(0, "load_catalogs");
    const auto fixture = MakeNativePixelLodFixture();
    assertFixtureGuards();
    const auto candidate = loadShader(candidateCatalog, fixture.vertex_shader_words, fixture.pixel_shader_words, "candidate");
    const auto control   = loadShader(controlCatalog, fixture.vertex_shader_words, fixture.pixel_shader_words, "control");
    require(sameAttributes(candidate.vertexAttributes, control.vertexAttributes), "candidate/control attributes differ");
    assertCandidateSource(candidateSource);

    HarnessLogging                                    harnessLogging;
    NativeRendererD3D12                               candidateRenderer;
    NativeRendererD3D12                               controlRenderer;
    const std::array<std::array<std::uint32_t, 2>, 8> pairs = {
        { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 4 }, { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 0 } }
    };

    struct PairSource
    {
        NativePixelLodFixture      fixture;
        std::vector<std::uint32_t> registers;
        std::uint32_t              vertexGuestBase = 0;
        std::vector<std::uint8_t>  expectedPlane;
    };

    std::array<PairSource, 8> pairSources;
    std::uint32_t             pairIndex = 0;
    for (const auto& pair : pairs)
    {
        phase(pairIndex, "before_original");
        const auto requested = fixtureForPair(pair[0], pair[1]);
        auto       original  = runOriginal(pair[0], pair[1]);
        phase(pairIndex, "original_returned");
        writeOriginalDiagnostics(outputRoot, pairIndex, pair[0], pair[1], original);
        phase(pairIndex, "original_diagnostics");
        require(original.failure.empty(), "original runner failed: " + original.failure);
        require(original.progressed && original.completion_observed && original.terminal_capture &&
                    original.actual_readback_owner && original.source_draw_issued,
                "original runner did not report an issued completed terminal readback");
        require(original.terminal_pixels_match_expected,
                "original terminal pixels did not match its independent expected values");
        require(original.terminal_width == kWidth && original.terminal_height == kHeight &&
                    original.terminal_stride == kWidth * 4,
                "original terminal dimensions are outside the bounded schema");
        assertOriginalCompletion(original);
        assertOriginalSource(requested, original);
        phase(pairIndex, "original_validated");
        auto& source           = pairSources[pairIndex];
        source.fixture         = std::move(original.source_fixture);
        source.registers       = std::move(original.source_registers);
        source.vertexGuestBase = sourceVertexGuestBase(original);
        source.expectedPlane   = std::move(original.expected_pixels);
        require(source.expectedPlane.size() == kPlane, "original expected plane is incomplete");
        const auto expected      = duplicatePlane(original.terminal_pixels);
        const auto baseTexture   = textureBytes(source.fixture);
        const auto candidatePath = outputRoot / ("candidate-" + std::to_string(pairIndex) + ".bin");
        const auto controlPath   = outputRoot / ("control-" + std::to_string(pairIndex) + ".bin");
        phase(pairIndex, "candidate_recipe");
        const auto candidateResult = candidateRenderer.ReplayOffscreen(
            buildRecipe(source.fixture, candidate, candidatePath, baseTexture, kMipCount, source.registers, source.vertexGuestBase));
        phase(pairIndex, "candidate_replay_returned");
        require(candidateResult.success && candidateResult.completionFenceValue == 0,
                "candidate native replay failed: " + candidateResult.error);
        const auto controlResult = controlRenderer.ReplayOffscreen(
            buildRecipe(source.fixture, control, controlPath, baseTexture, kMipCount, source.registers, source.vertexGuestBase));
        phase(pairIndex, "control_replay_returned");
        require(controlResult.success && controlResult.completionFenceValue == 0,
                "control native replay failed: " + controlResult.error);
        const auto candidateOutput = readNativeOutput(candidatePath);
        const auto controlOutput   = readNativeOutput(controlPath);
        compare(candidateOutput, expected, "candidate pair " + std::to_string(pairIndex) + " vs original");
        compare(controlOutput, expected, "control pair " + std::to_string(pairIndex) + " vs original");
        compare(candidateOutput, controlOutput, "candidate pair " + std::to_string(pairIndex) + " vs control");
        phase(pairIndex, "pair_complete");
        ++pairIndex;
    }

    const auto& firstSource   = pairSources.front();
    const auto  missingParent = outputRoot / "missing-output-parent";
    require(!std::filesystem::exists(missingParent), "missing-output-parent must be absent before rejection");
    const auto missingPath   = missingParent / "candidate.bin";
    const auto missingResult = candidateRenderer.ReplayOffscreen(
        buildRecipe(firstSource.fixture, candidate, missingPath, textureBytes(firstSource.fixture), kMipCount, firstSource.registers, firstSource.vertexGuestBase));
    require(!missingResult.success && missingResult.error.find("open replay output file") != std::string::npos,
            "absent native output path did not fail at the existing output owner");
    require(!std::filesystem::exists(missingPath), "absent native output path unexpectedly produced a file");
    const auto recoveryPath   = outputRoot / "recovery-after-missing-output.bin";
    const auto recoveryResult = candidateRenderer.ReplayOffscreen(
        buildRecipe(firstSource.fixture, candidate, recoveryPath, textureBytes(firstSource.fixture), kMipCount, firstSource.registers, firstSource.vertexGuestBase));
    require(recoveryResult.success && recoveryResult.completionFenceValue == 0,
            "candidate owner did not recover after absent output rejection: " + recoveryResult.error);
    compare(readNativeOutput(recoveryPath), duplicatePlane(firstSource.expectedPlane), "candidate recovery after absent output");

    std::uint32_t sensitivity = 0;
    for (std::uint32_t level = 0; level < kMipCount; ++level)
    {
        auto& mutatedFixture = pairSources[level].fixture;
        for (auto& byte : mutatedFixture.texture.mips[level].linear_rgba8)
            byte ^= 0xA5;
        const auto expectedMutated = ExpectedNativePixelLodPixels(mutatedFixture, level, (level + 1) % kMipCount);
        require(expectedMutated.size() == kPlane && expectedMutated != pairSources[level].expectedPlane,
                "mutated mip did not produce an independent full expected plane");
        const auto mutatedTexture = textureBytes(mutatedFixture);
        const auto candidatePath  = outputRoot / ("mutated-candidate-" + std::to_string(level) + ".bin");
        const auto controlPath    = outputRoot / ("mutated-control-" + std::to_string(level) + ".bin");
        const auto controlResult  = controlRenderer.ReplayOffscreen(
            buildRecipe(mutatedFixture, control, controlPath, mutatedTexture, kMipCount, pairSources[level].registers, pairSources[level].vertexGuestBase));
        require(controlResult.success && controlResult.completionFenceValue == 0,
                "control mutated mip replay failed: " + controlResult.error);
        const auto controlOutput = readNativeOutput(controlPath);
        compare(controlOutput, duplicatePlane(expectedMutated), "control mutated mip " + std::to_string(level));
        bool                   candidateBuilt = true;
        NativeDrawReplayRecipe candidateRecipe;
        std::string            candidateBuildError;
        try
        {
            candidateRecipe = buildRecipe(mutatedFixture, candidate, candidatePath, mutatedTexture, kMipCount, pairSources[level].registers, pairSources[level].vertexGuestBase);
        }
        catch (const std::exception& error)
        {
            candidateBuilt      = false;
            candidateBuildError = error.what();
        }
        if (candidateBuilt)
        {
            const auto candidateResult = candidateRenderer.ReplayOffscreen(candidateRecipe);
            if (!candidateResult.success)
            {
                require(!candidateResult.error.empty(), "mutated mip rejection has no error");
                ++sensitivity;
                continue;
            }
            require(candidateResult.completionFenceValue == 0, "candidate mutated mip reported a completion fence");
            const auto candidateOutput = readNativeOutput(candidatePath);
            compare(candidateOutput, duplicatePlane(expectedMutated), "candidate mutated mip " + std::to_string(level));
            compare(candidateOutput, controlOutput, "candidate/control mutated mip " + std::to_string(level));
        }
        else
            require(!candidateBuildError.empty(), "mutated mip rejection has no error");
        ++sensitivity;
    }

    struct OmissionDiagnostic
    {
        std::uint32_t level = 0;
        std::string   stage;
        std::string   error;
        bool          outputExists = false;
    };

    std::vector<OmissionDiagnostic> omissions;
    omissions.reserve(kMipCount);
    for (std::uint32_t level = 0; level < kMipCount; ++level)
    {
        const auto&            source  = pairSources[level];
        const auto             path    = outputRoot / ("omitted-mip-" + std::to_string(level) + ".bin");
        const auto             payload = textureBytesWithoutMip(source.fixture, level);
        bool                   built   = true;
        NativeDrawReplayRecipe recipe;
        try
        {
            recipe = buildRecipe(source.fixture, candidate, path, payload, kMipCount, source.registers, source.vertexGuestBase);
        }
        catch (const std::exception& error)
        {
            built = false;
            omissions.push_back({ level, "BuildNativeGameplayDrawRecipe", error.what(), std::filesystem::exists(path) });
        }
        if (built)
        {
            const auto result = candidateRenderer.ReplayOffscreen(recipe);
            require(!result.success && !result.error.empty(), "omitted selected mip was not explicitly rejected");
            omissions.push_back({ level, "ReplayOffscreen", result.error, std::filesystem::exists(path) });
        }
        require(!omissions.back().outputExists, "omitted mip rejection unexpectedly produced output");
        ++sensitivity;
    }

    std::ofstream report(outputRoot / "result.toml", std::ios::binary | std::ios::trunc);
    require(report.good(), "could not create native pixel LOD result");
    report << "schema_version = 1\nuv_bias_base_texels = 0.25\ntexel_edge_guard = true\ncompleted_pairs = 8\nsensitivity_checks = " << sensitivity
           << "\noutput_planes = 2\nabsent_output_rejected = true\nabsent_output_error = "
           << tomlString(missingResult.error) << "\nomission_count = " << omissions.size() << "\n";
    for (std::size_t index = 0; index < omissions.size(); ++index)
    {
        report << "omission_" << index << "_level = " << omissions[index].level << "\n"
               << "omission_" << index << "_stage = " << tomlString(omissions[index].stage) << "\n"
               << "omission_" << index << "_error = " << tomlString(omissions[index].error) << "\n"
               << "omission_" << index << "_output_exists = "
               << (omissions[index].outputExists ? "true" : "false") << "\n";
    }
    report.flush();
    require(report.good(), "could not flush native pixel LOD result");
    std::cout << "Native pixel register LOD matched completed original/control outputs for levels 0..7; sensitivity checks "
              << sensitivity << '\n';
    return 0;
}

} // namespace

static void fixtureGuards()
{
    require(kMipCount == 8 && kPlane == 65536, "native pixel LOD fixture guards changed");
    assertFixtureGuards();
    std::cout << "Native pixel LOD fixture guards passed; use the emit and run helper commands\n";
}

static int preparedMain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--emit-fixture")
        {
            emitFixture(argv[2]);
            return 0;
        }
        if (argc != 9 || std::wstring_view(argv[1]) != L"--candidate-catalog" ||
            std::wstring_view(argv[3]) != L"--control-catalog" || std::wstring_view(argv[5]) != L"--candidate-source" ||
            std::wstring_view(argv[7]) != L"--output")
            fail("usage: rerevved_prepared_tests --helper native-pixel-lod.run --candidate-catalog <dir> --control-catalog <dir> --candidate-source <file> --output <fresh-dir>");
        return run(argv[2], argv[4], argv[6], argv[8]);
    }
    catch (const std::exception& error)
    {
        std::cerr << "native_pixel_lod_tests: " << error.what() << '\n';
        return 1;
    }
}

// native-pixel-lod.emit --emit-fixture <fresh-dir>: writes the fixture inputs.
REREVVED_HELPER_COMMAND(nativePixelLodEmit, "native-pixel-lod.emit")
{
    rerevved::tests::NativeArgv argv(args);
    if (argv.argc() != 3 || std::wstring_view(argv.argv()[1]) != L"--emit-fixture")
    {
        std::cerr << "usage: rerevved_prepared_tests --helper native-pixel-lod.emit --emit-fixture <fresh-dir>" << std::endl;
        return 2;
    }
    return preparedMain(argv.argc(), argv.argv());
}

// native-pixel-lod.run --candidate-catalog <dir> --control-catalog <dir> --candidate-source <file> --output <fresh-dir>:
// the GPU proof.
REREVVED_HELPER_COMMAND(nativePixelLodRun, "native-pixel-lod.run")
{
    rerevved::tests::NativeArgv argv(args);
    if (argv.argc() != 9)
    {
        std::cerr << "usage: rerevved_prepared_tests --helper native-pixel-lod.run --candidate-catalog <dir> --control-catalog <dir> --candidate-source <file> --output <fresh-dir>" << std::endl;
        return 2;
    }
    return preparedMain(argv.argc(), argv.argv());
}

TEST_CASE("native_pixel_lod_fixture", "[ctest][native-pixel-lod]")
{
    UNSCOPED_INFO("the fixture guards hold");
    REQUIRE_NOTHROW(fixtureGuards());
}
