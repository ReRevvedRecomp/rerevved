#include "gameplay_input.h"
#include "gpu/d3d12/native_gameplay_render_frame.h"
#include "gpu/d3d12/native_gameplay_shaders.h"
#include "gpu/d3d12/native_renderer_d3d12.h"
#include "vulkan_draw.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <rex/crypto/sha256.h>
#include <rex/logging.h>
#include <toml++/toml.hpp>

namespace
{

using namespace rerevved::gpu;
using Event                        = rex::graphics::diagnostic::DrawCaptureFrameEvent;
constexpr std::size_t kShaderBound = 4U * 1024U * 1024U;

struct ShaderPair
{
    std::uint64_t    vertexHash;
    std::uint64_t    pixelHash;
    std::string_view vertexSpirv;
    std::string_view pixelSpirv;
};

constexpr std::array<ShaderPair, 2> kShaderPairs{ { { 0xC3BAB94E67553E12ULL, 0x64D8E6A475FFCB2DULL, "b708550e7c1aeb9f1ae803e33ed958d13988d6c19893e19bf89f2a87985dc838", "cb726312ca28321f5693036962643fae00e8cdc7fe5ff8473fd3c3c54bd977ee" },
                                                    { 0x1EE55F3AB5213177ULL, 0x47F2D46F3B8F1668ULL, "59a4482f6f70fa4921bb20312df7eb00ceed7f75856369d52c5aad3e599a8f0f", "1155606c45e81667aea44725623f70b84ca1ed31be943962af25cb688cfad8cb" } } };

void require(bool condition, const std::string& error)
{
    if (!condition)
    {
        throw std::runtime_error(error.empty() ? "operation failed without a diagnostic" : error);
    }
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path, std::size_t maximum)
{
    const auto size = std::filesystem::file_size(path);
    require(size <= maximum, "input exceeds byte bound: " + path.string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream             file(path, std::ios::binary);
    require(bool(file), "could not open input: " + path.string());
    require(bool(file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))), "could not read input: " + path.string());
    require(file.peek() == std::char_traits<char>::eof(), "input grew during read: " + path.string());
    return bytes;
}

std::string digest(std::span<const std::uint8_t> bytes)
{
    return rex::crypto::sha256(bytes.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::span<const std::uint8_t> wordBytes(std::span<const std::uint32_t> words)
{
    return { reinterpret_cast<const std::uint8_t*>(words.data()), words.size_bytes() };
}

std::string hex(std::uint64_t value)
{
    std::ostringstream text;
    text << std::uppercase << std::hex << std::setfill('0') << std::setw(16) << value;
    return text.str();
}

template <class Values>
toml::array array(const Values& values)
{
    toml::array result;
    for (const auto& value : values)
    {
        result.push_back(value);
    }
    return result;
}

toml::table payload(std::span<const std::uint8_t> bytes)
{
    return toml::table{ { "bytes", static_cast<std::int64_t>(bytes.size()) }, { "sha256", digest(bytes) } };
}

toml::table samplerState(const NativeDrawReplaySampler& state)
{
    return toml::table{
        { "min_linear", state.minLinear }, { "mag_linear", state.magLinear }, { "mip_linear", state.mipLinear }, { "anisotropy", state.anisotropy }, { "address", array(state.address) }, { "border", array(state.border) }, { "min_lod", state.minLod }, { "max_lod", state.maxLod }, { "mip_bias", state.mipBias }
    };
}

toml::table stencilFace(const NativeDrawReplayStencilState::Face& face)
{
    return toml::table{ { "compare", face.compare }, { "fail", face.fail }, { "depth_fail", face.depthFail }, { "pass", face.pass } };
}

toml::table passState(const NativeRenderFramePass& pass)
{
    const auto  target = GetNativeRenderFrameTarget(pass);
    toml::array clears, draws;
    for (const auto& clear : pass.clears)
    {
        clears.push_back(toml::table{
            { "rectangle", toml::array{ clear.rectangle.left, clear.rectangle.top, clear.rectangle.right, clear.rectangle.bottom } }, { "color", clear.color }, { "depth", clear.depth }, { "stencil", clear.stencil }, { "color_targets", clear.colorTargets }, { "color_value", array(clear.colorValue) }, { "depth_value", clear.depthValue }, { "stencil_value", clear.stencilValue } });
    }
    for (std::size_t index = 0; index < pass.DrawCount(); ++index)
    {
        const auto& recipe = pass.Draw(index);
        toml::array producers;
        for (const auto& texture : recipe.textures)
        {
            if (texture.frameTextureId)
            {
                producers.push_back(hex(texture.frameTextureId));
            }
        }
        draws.push_back(toml::table{ { "draw_index", static_cast<std::int64_t>(index) }, { "vertex_shader_hash", hex(recipe.vertexShaderHash) }, { "pixel_shader_hash", hex(recipe.pixelShaderHash) }, { "producer_texture_ids", std::move(producers) } });
    }
    return toml::table{
        { "width", target.width }, { "height", target.height }, { "sample_count", target.sampleCount }, { "target_format", static_cast<int>(target.format) }, { "preserve_previous_attachments", pass.preservePreviousAttachments }, { "color_source_pass", pass.colorSourcePass }, { "resolve_count", static_cast<std::int64_t>(pass.resolves.size()) }, { "draw_count", static_cast<std::int64_t>(pass.DrawCount()) }, { "clears", std::move(clears) }, { "draws", std::move(draws) }
    };
}

toml::table recipeState(const NativeDrawReplayRecipe& recipe)
{
    toml::table result{
        { "schema_version", recipe.schemaVersion }, { "width", recipe.width }, { "height", recipe.height }, { "target_format", static_cast<int>(recipe.targetFormat) }, { "sample_count", recipe.sampleCount }, { "sample_mask", recipe.sampleMask }, { "vertex_shader_hash", hex(recipe.vertexShaderHash) }, { "pixel_shader_hash", hex(recipe.pixelShaderHash) }, { "vertex_stride_bytes", recipe.vertexStrideBytes }, { "vertex_attribute_count", recipe.vertexAttributeCount }, { "index_count", recipe.indexCount }, { "texture_mask", recipe.textureMask }, { "clear_color", array(recipe.clearColor) }, { "vertex_dxil", payload(recipe.vertexShaderBytes()) }, { "pixel_dxil", payload(recipe.pixelShaderBytes()) }, { "decoded_vertices", payload(recipe.vertexBytes()) }, { "decoded_indices", payload(wordBytes(recipe.indices)) }, { "vertex_constants", payload(recipe.vertexConstants) }, { "pixel_constants", payload(recipe.pixelConstants) }, { "shared_constants", payload(recipe.sharedConstants) }, { "initial_sample0", payload(recipe.initialSample0) }, { "initial_sample3", payload(recipe.initialSample1) }
    };
    result.insert("viewport", toml::array{ recipe.viewport.x, recipe.viewport.y, recipe.viewport.width, recipe.viewport.height, recipe.viewport.minDepth, recipe.viewport.maxDepth });
    result.insert("scissor", toml::array{ recipe.scissor.left, recipe.scissor.top, recipe.scissor.right, recipe.scissor.bottom });
    result.insert("depth", toml::table{ { "enabled", recipe.depth.enabled }, { "write_enabled", recipe.depth.writeEnabled }, { "compare", recipe.depth.compare }, { "initial_clear", recipe.depth.initialClear }, { "initial_samples", payload(recipe.depth.initialSamples) } });
    result.insert("stencil", toml::table{ { "enabled", recipe.stencil.enabled }, { "reference", recipe.stencil.reference }, { "read_mask", recipe.stencil.readMask }, { "write_mask", recipe.stencil.writeMask }, { "front", stencilFace(recipe.stencil.front) }, { "back", stencilFace(recipe.stencil.back) } });
    result.insert("rasterizer", toml::table{ { "cull", recipe.rasterizer.cull }, { "front_counter_clockwise", recipe.rasterizer.frontCounterClockwise } });
    const auto& blend = recipe.blend;
    result.insert("blend", toml::table{ { "enabled", blend.enabled }, { "alpha_to_coverage", blend.alphaToCoverage }, { "logic_op_enabled", blend.logicOpEnabled }, { "source_color", static_cast<int>(blend.sourceColor) }, { "destination_color", static_cast<int>(blend.destinationColor) }, { "color_op", static_cast<int>(blend.colorOp) }, { "source_alpha", static_cast<int>(blend.sourceAlpha) }, { "destination_alpha", static_cast<int>(blend.destinationAlpha) }, { "alpha_op", static_cast<int>(blend.alphaOp) }, { "write_mask", blend.writeMask } });
    result.insert("independent_samplers", !recipe.samplers.empty());
    result.insert("shared_sampler", samplerState(recipe.sampler));
    toml::array textures;
    for (std::size_t slot = 0; slot < recipe.textures.size(); ++slot)
    {
        if (!(recipe.textureMask & (1U << slot)))
        {
            continue;
        }
        const auto& texture = recipe.textures[slot];
        toml::table value{
            { "slot", static_cast<std::int64_t>(slot) }, { "width", texture.width }, { "height", texture.height }, { "mip_levels", texture.mipLevels }, { "dimension", static_cast<int>(texture.dimension) }, { "format", static_cast<int>(texture.format) }, { "swizzle", array(texture.swizzle) }, { "frame_texture_id", hex(texture.frameTextureId) }, { "payload", payload(texture.payloadBytes()) }, { "sampler", samplerState(recipe.samplers.empty() ? recipe.sampler : recipe.samplers.at(slot)) }
        };
        textures.push_back(std::move(value));
    }
    result.insert("textures", std::move(textures));
    return result;
}

std::string serialize(const toml::table& table)
{
    std::ostringstream output;
    output << table;
    return output.str();
}

void writeReport(const std::filesystem::path& path, const toml::table& report)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(bool(output), "could not open report: " + path.string());
    output << report << '\n';
    output.close();
    require(bool(output), "could not finish report: " + path.string());
}

void writeBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    std::ofstream output(path, std::ios::binary);
    require(bool(output), "could not open output: " + path.string());
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(bool(output), "could not finish output: " + path.string());
}

std::vector<std::uint32_t> readSpirv(const std::filesystem::path& path, std::string_view expectedDigest)
{
    const auto bytes = readBytes(path, kShaderBound);
    require(bytes.size() >= 20 && bytes.size() % 4 == 0, "SPIR-V module is empty, truncated or unaligned: " + path.string());
    std::vector<std::uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    require(words[0] == 0x07230203U, "invalid little-endian SPIR-V magic: " + path.string());
    const auto actualDigest = digest(bytes);
    require(actualDigest == expectedDigest, "SPIR-V identity mismatch: " + path.string() + "; loaded_sha256=" + actualDigest + "; expected_sha256=" + std::string(expectedDigest));
    return words;
}

toml::table sourceState(const Event& event, const NativeGameplaySnapshot& snapshot)
{
    const auto& draw = event.draw.draw;
    toml::table result{
        { "event_id", static_cast<std::int64_t>(event.id) }, { "guest_frame", static_cast<std::int64_t>(event.frame) }, { "submission", static_cast<std::int64_t>(event.submission) }, { "registers", payload(wordBytes(draw.registers)) }, { "vertex_words", payload(wordBytes(draw.vertex_ucode)) }, { "pixel_words", payload(wordBytes(draw.pixel_ucode)) }, { "index_bytes", payload(draw.index.bytes) }, { "primitive_type", draw.primitive_type }, { "index_present", draw.index.present }, { "source_index_count", draw.index.count }
    };
    toml::array fetches;
    for (const auto& fetch : draw.vertex_fetches)
    {
        fetches.push_back(toml::table{ { "fetch_constant", fetch.fetch_constant }, { "guest_base", fetch.base }, { "payload", payload(fetch.Payload()) } });
    }
    result.insert("vertex_fetches", std::move(fetches));
    toml::array uses;
    for (const auto& use : event.draw.texture_uses)
    {
        if (!use.required)
        {
            continue;
        }
        require(use.texture_index < snapshot.textures.size(), "required source texture identity is out of bounds");
        const auto& texture = snapshot.textures[use.texture_index];
        toml::array subresources;
        for (const auto& subresource : texture.subresources)
        {
            subresources.push_back(toml::table{ { "mip", subresource.mip_level }, { "slice", subresource.array_slice }, { "payload", payload(subresource.bytes) } });
        }
        uses.push_back(toml::table{
            { "texture_index", use.texture_index }, { "stage", use.stage }, { "binding", use.shader_binding_index }, { "fetch_constant", use.fetch_constant }, { "guest_base_page", texture.guest_base_page }, { "legacy_payload", payload(texture.bytes) }, { "subresources", std::move(subresources) } });
    }
    result.insert("texture_uses", std::move(uses));
    return result;
}

toml::table comparePlane(std::span<const std::uint8_t> d3d, std::span<const std::uint8_t> vulkan, const std::array<std::uint8_t, 4>& clear, std::uint32_t sample)
{
    std::int64_t differingBytes = 0, differingPixels = 0, d3dNonclear = 0, vulkanNonclear = 0;
    int          maximumError = 0;
    for (std::size_t offset = 0; offset < d3d.size(); offset += 4)
    {
        bool differs = false, d3dClear = true, vulkanClear = true;
        for (std::size_t channel = 0; channel < 4; ++channel)
        {
            const auto error = std::abs(int(d3d[offset + channel]) - int(vulkan[offset + channel]));
            differingBytes += error != 0;
            differs |= error != 0;
            maximumError = std::max(maximumError, error);
            d3dClear &= d3d[offset + channel] == clear[channel];
            vulkanClear &= vulkan[offset + channel] == clear[channel];
        }
        differingPixels += differs;
        d3dNonclear += !d3dClear;
        vulkanNonclear += !vulkanClear;
    }
    return toml::table{
        { "host_sample", sample }, { "differing_bytes", differingBytes }, { "differing_pixels", differingPixels }, { "maximum_channel_error", maximumError }, { "d3d12_nonclear_pixels", d3dNonclear }, { "vulkan_nonclear_pixels", vulkanNonclear }, { "d3d12_sha256", digest(d3d) }, { "vulkan_sha256", digest(vulkan) }
    };
}

toml::table differencePlane(std::span<const std::uint8_t> ordered, std::span<const std::uint8_t> reset, std::uint32_t sample)
{
    std::int64_t differingBytes = 0, differingPixels = 0;
    int          maximumError = 0;
    for (std::size_t offset = 0; offset < ordered.size(); offset += 4)
    {
        bool differs = false;
        for (std::size_t channel = 0; channel < 4; ++channel)
        {
            const auto error = std::abs(int(ordered[offset + channel]) - int(reset[offset + channel]));
            differingBytes += error != 0;
            differs |= error != 0;
            maximumError = std::max(maximumError, error);
        }
        differingPixels += differs;
    }
    return toml::table{ { "host_sample", sample }, { "differing_bytes", differingBytes }, { "differing_pixels", differingPixels }, { "maximum_channel_error", maximumError }, { "ordered_sha256", digest(ordered) }, { "reset_sha256", digest(reset) } };
}

std::size_t parseIndex(std::string_view text, const char* name)
{
    std::size_t value  = 0;
    const auto  parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), std::string(name) + " must be a nonnegative decimal integer");
    return value;
}

int runPass(int argc, char** argv, int first, const ShaderPair& selected)
{
    const auto input = first + 1;
    if (argc != input + 8)
    {
        std::cerr << "usage: native_vulkan_draw_compare [--shaders <VS-hash> <PS-hash>] --pass <manifest> <shader-catalog> <selected-pass-index> <companion-pass-index> <vertex.spv> <pixel.spv> <samples.spv> <fresh-output-dir>\n";
        return 2;
    }
    const auto manifest      = std::filesystem::canonical(argv[input]);
    const auto catalog       = std::filesystem::canonical(argv[input + 1]);
    const auto selectedPass  = parseIndex(argv[input + 2], "selected pass index");
    const auto companionPass = parseIndex(argv[input + 3], "companion pass index");
    const auto output        = std::filesystem::weakly_canonical(std::filesystem::absolute(argv[input + 7]));
    for (const auto& immutableRoot : { manifest.parent_path(), catalog })
    {
        const auto relative = output.lexically_relative(immutableRoot);
        require(relative.empty() || *relative.begin() == "..", "output directory must be outside capture inputs and shader catalog");
        for (auto ancestor = output.parent_path(); !ancestor.empty();)
        {
            require(!std::filesystem::exists(ancestor) || !std::filesystem::equivalent(ancestor, immutableRoot), "output directory aliases capture inputs or shader catalog");
            const auto parent = ancestor.parent_path();
            if (parent == ancestor)
            {
                break;
            }
            ancestor = parent;
        }
    }
    require(!std::filesystem::exists(output), "output directory must be fresh: " + output.string());
    require(std::filesystem::create_directories(output), "could not create fresh output directory");
    toml::table report{ { "compare_schema_version", 1 }, { "mode", "pass" }, { "status", "loading" }, { "manifest", manifest.string() }, { "shader_catalog", catalog.string() }, { "selected_pass_index", static_cast<std::int64_t>(selectedPass) }, { "companion_pass_index", static_cast<std::int64_t>(companionPass) }, { "scope", "complete selected two-draw pass with original companion; raw color samples 0/3" }, { "depth_plane_parity_tested", false } };
    const auto  reportPath = output / "report.toml";
    try
    {
        std::string                                error;
        NativeGameplaySnapshot                     snapshot;
        std::vector<NativeGameplayShaderInterface> shaders;
        NativeRenderFrame                          frame;
        report.insert("manifest_identity", payload(readBytes(manifest, 64U * 1024U * 1024U)));
        report.insert("catalog_identity", payload(readBytes(catalog / "shaders.toml", kShaderBound)));
        require(LoadGameplayCaptureInputs(manifest, snapshot, error), error);
        require(LoadNativeGameplayShaders(catalog, shaders, error), error);
        require(BuildNativeGameplayRenderFrame(snapshot, shaders, {}, frame, error), error);
        require(selectedPass < frame.passes.size() && companionPass < frame.passes.size(), "pass index exceeds the complete admitted frame");
        require(selectedPass != companionPass, "selected and companion pass indices must differ");

        const auto& selectedFramePass  = frame.passes[selectedPass];
        const auto& companionFramePass = frame.passes[companionPass];
        const auto  target             = GetNativeRenderFrameTarget(selectedFramePass);
        require(selectedFramePass.DrawCount() == 2 && companionFramePass.DrawCount() == 2, "pass comparison requires exactly two draws in each complete pass");
        require(!selectedFramePass.preservePreviousAttachments, "selected pass carries a prior attachment dependency");
        require(target.width == 640 && target.height == 720 && target.sampleCount == 4 && target.format == NativeDrawReplayTargetFormat::Rgba8, "pass comparison requires the selected 640x720 RGBA8 MSAA4 target");
        require(GetNativeRenderFrameTarget(companionFramePass) == target, "selected and companion targets differ");
        require(selectedFramePass.resolves.empty() && companionFramePass.resolves.empty() && selectedFramePass.colorSourcePass == 0 && companionFramePass.colorSourcePass == 0, "pass comparison requires complete clear-only pass inputs without resolve or color-source dependencies");

        const auto validateClear = [&](const NativeRenderFramePass& pass, const char* name)
        {
            require(pass.clears.size() == 1, std::string(name) + " must have one complete clear");
            const auto& clear = pass.clears.front();
            require(clear.color && clear.depth && clear.stencil && clear.colorTargets == 1 && clear.rectangle.left == 0 && clear.rectangle.top == 0 && clear.rectangle.right == 640 && clear.rectangle.bottom == 720 && clear.colorValue == std::array<float, 4>{ 0, 0, 0, 0 } && clear.depthValue == 1.0F && clear.stencilValue == 0, std::string(name) + " clear is not the selected full black/far/stencil-zero clear");
        };
        validateClear(selectedFramePass, "selected pass");
        validateClear(companionFramePass, "companion pass");

        const auto validateDraw = [&](const NativeDrawReplayRecipe& draw, const char* name)
        {
            require(draw.vertexShaderHash == selected.vertexHash && draw.pixelShaderHash == selected.pixelHash, std::string(name) + " uses a shader pair outside the selected module pins");
            require(draw.initialSample0.empty() && draw.initialSample1.empty() && draw.depth.initialSamples.empty(), std::string(name) + " carries a preloaded attachment payload");
            require(draw.clearColor == std::array<std::uint8_t, 4>{ 0, 0, 0, 0 } && draw.depth.initialClear == 1.0F, std::string(name) + " recipe clear differs from the admitted pass clear");
            for (const auto& texture : draw.textures)
            {
                require(!texture.frameTextureId, std::string(name) + " carries a producer texture dependency");
            }
            require(ValidateNativeDrawReplayRecipe(draw, error), error);
        };
        std::vector<NativeDrawReplayRecipe> selectedDraws, companionDraws;
        for (std::size_t index = 0; index < 2; ++index)
        {
            selectedDraws.push_back(selectedFramePass.Draw(index));
            companionDraws.push_back(companionFramePass.Draw(index));
            validateDraw(selectedDraws.back(), "selected draw");
            validateDraw(companionDraws.back(), "companion draw");
        }
        require(ValidateVulkanOffscreenPass(selectedDraws, error), error);

        struct SourceDraw
        {
            std::size_t            eventIndex;
            NativeDrawReplayRecipe recipe;
        };

        NativeGameplayCommandFeed feed(shaders);
        require(feed.Begin(snapshot.start_frame, snapshot.start_submission, {}, error, {}, false, false), error);
        std::vector<SourceDraw> sourceDraws;
        for (std::size_t eventIndex = 0; eventIndex < snapshot.events.size(); ++eventIndex)
        {
            const auto& event = snapshot.events[eventIndex];
            if (event.id == snapshot.events.back().id)
            {
                break;
            }
            require(feed.Consume(event, snapshot.textures, {}, error), error);
            if (event.kind != Event::Kind::kDraw || event.draw.guest_vertex_shader_hash != selected.vertexHash || event.draw.guest_pixel_shader_hash != selected.pixelHash)
            {
                continue;
            }
            const auto* admitted = feed.Draw(event.id);
            require(admitted, "selected source draw was not retained by the admitted command feed");
            sourceDraws.push_back({ eventIndex, admitted->Recipe() });
        }
        std::vector<std::size_t> passOffsets(frame.passes.size());
        std::size_t              frameDrawCount = 0;
        for (std::size_t passIndex = 0; passIndex < frame.passes.size(); ++passIndex)
        {
            passOffsets[passIndex] = frameDrawCount;
            for (std::size_t drawIndex = 0; drawIndex < frame.passes[passIndex].DrawCount(); ++drawIndex)
            {
                const auto& draw = frame.passes[passIndex].Draw(drawIndex);
                frameDrawCount += draw.vertexShaderHash == selected.vertexHash && draw.pixelShaderHash == selected.pixelHash;
            }
        }
        require(frameDrawCount == sourceDraws.size(), "complete frame selected-pair inventory does not match the ordered source event inventory");
        const auto requireSourceEvents = [&](std::size_t passIndex, std::size_t expectedPass, std::uint64_t firstEvent, std::uint64_t lastEvent, const char* name)
        {
            require(passIndex == expectedPass, std::string(name) + " has the wrong canonical pass index");
            const auto offset = passOffsets[passIndex];
            require(snapshot.events[sourceDraws[offset].eventIndex].id == firstEvent && snapshot.events[sourceDraws[offset + 1].eventIndex].id == lastEvent, std::string(name) + " source events do not match the canonical event pair");
        };
        requireSourceEvents(selectedPass, 0, 51, 52, "selected pass");
        requireSourceEvents(companionPass, 3, 131, 132, "companion pass");
        const auto verifyLineage = [&](std::size_t passIndex, const NativeRenderFramePass& pass, const char* name)
        {
            const auto  offset = passOffsets[passIndex];
            toml::array sources;
            for (std::size_t drawIndex = 0; drawIndex < pass.DrawCount(); ++drawIndex)
            {
                const auto& source = sourceDraws[offset + drawIndex];
                require(serialize(recipeState(pass.Draw(drawIndex))) == serialize(recipeState(source.recipe)), std::string(name) + " source event does not reproduce the exact admitted recipe");
                sources.push_back(sourceState(snapshot.events[source.eventIndex], snapshot));
            }
            return sources;
        };
        report.insert("selected_pass", passState(selectedFramePass));
        report.insert("companion_pass", passState(companionFramePass));
        report.insert("selected_source", verifyLineage(selectedPass, selectedFramePass, "selected pass"));
        report.insert("companion_source", verifyLineage(companionPass, companionFramePass, "companion pass"));
        report.insert("source_lineage_verified", true);

        const auto vertex  = readSpirv(argv[input + 4], selected.vertexSpirv);
        const auto pixel   = readSpirv(argv[input + 5], selected.pixelSpirv);
        const auto samples = readSpirv(argv[input + 6], "d4c20e78814ae1f939670ba8a465c992987af3b8f35ff3e0547c4215fa651c7c");
        report.insert("spirv", toml::table{ { "vertex", payload(wordBytes(vertex)) }, { "pixel", payload(wordBytes(pixel)) }, { "samples", payload(wordBytes(samples)) } });
        report.insert("spirv_admission", "exact independently validated original stage and layout identities; Vulkan1.2 scalar block layout");
        writeReport(reportPath, report);

        NativeFrameReplayRecipe d3dRecipe;
        d3dRecipe.halves[0]        = selectedDraws;
        d3dRecipe.halves[1]        = companionDraws;
        d3dRecipe.outputPath       = output / "d3d12.resolved.rgba";
        d3dRecipe.sampleOutputPath = output / "d3d12.samples.bin";
        NativeRendererD3D12::ConfigureReplayDiagnostics();
        NativeRendererD3D12 renderer;
        const auto          d3dResult = renderer.ReplayFrame(std::move(d3dRecipe));
        renderer.Shutdown();
        require(d3dResult.success, "D3D12: " + d3dResult.error);
        constexpr std::size_t planeBytes = 640U * 720U * 4U;
        const auto            d3d        = readBytes(output / "d3d12.samples.bin", planeBytes * 4);
        require(d3d.size() == planeBytes * 4, "D3D12 output plane count or extent mismatch");
        report.insert("d3d12_output", payload(d3d));
        report.insert("d3d12_completion_fence", static_cast<std::int64_t>(d3dResult.completionFenceValue));

        VulkanDrawPassOptions options;
        options.clearColor   = selectedFramePass.clears.front().colorValue;
        options.clearDepth   = selectedFramePass.clears.front().depthValue;
        options.clearStencil = selectedFramePass.clears.front().stencilValue;
        VulkanDrawOutput ordered, reset;
        require(ReplayVulkanOffscreenPass(selectedDraws, vertex, pixel, samples, options, ordered, error), "Vulkan ordered: " + error);
        std::vector<std::uint8_t> orderedSamples = ordered.sample0;
        orderedSamples.insert(orderedSamples.end(), ordered.sample3.begin(), ordered.sample3.end());
        require(orderedSamples.size() == planeBytes * 2, "Vulkan ordered output plane count or extent mismatch");
        writeBytes(output / "vulkan.samples.bin", orderedSamples);
        report.insert("vulkan_adapter_report", ordered.adapterReport);
        require(ReplayVulkanOffscreenPass(selectedDraws, vertex, pixel, samples, [&]()
                                          {
                                              auto control              = options;
                                              control.resetBetweenDraws = true;
                                              return control;
                                          }(),
                                          reset,
                                          error),
                "Vulkan reset control: " + error);
        std::vector<std::uint8_t> resetSamples = reset.sample0;
        resetSamples.insert(resetSamples.end(), reset.sample3.begin(), reset.sample3.end());
        require(resetSamples.size() == planeBytes * 2, "Vulkan reset output plane count or extent mismatch");
        writeBytes(output / "vulkan.reset.samples.bin", resetSamples);
        toml::array comparisons;
        bool        identical = true, nonclear = true;
        for (std::size_t plane = 0; plane < 2; ++plane)
        {
            const auto comparison = comparePlane(std::span(d3d).subspan(plane * planeBytes, planeBytes), std::span(orderedSamples).subspan(plane * planeBytes, planeBytes), selectedDraws.front().clearColor, plane ? 3 : 0);
            identical &= comparison["differing_bytes"].value_or<std::int64_t>(-1) == 0;
            nonclear &= comparison["d3d12_nonclear_pixels"].value_or<std::int64_t>(0) > 0 && comparison["vulkan_nonclear_pixels"].value_or<std::int64_t>(0) > 0;
            comparisons.push_back(comparison);
        }
        toml::array resetComparisons;
        bool        resetSensitive = false;
        for (std::size_t plane = 0; plane < 2; ++plane)
        {
            const auto comparison = differencePlane(std::span(orderedSamples).subspan(plane * planeBytes, planeBytes), std::span(resetSamples).subspan(plane * planeBytes, planeBytes), plane ? 3 : 0);
            resetSensitive |= comparison["differing_bytes"].value_or<std::int64_t>(0) > 0;
            resetComparisons.push_back(comparison);
        }
        report.insert("planes", std::move(comparisons));
        report.insert("reset_between_draws", toml::table{ { "clear_color", array(options.clearColor) }, { "clear_depth", options.clearDepth }, { "clear_stencil", options.clearStencil }, { "ordered_output", payload(orderedSamples) }, { "reset_output", payload(resetSamples) }, { "planes", std::move(resetComparisons) }, { "output_sensitive", resetSensitive } });
        report.insert("exact_color_match", identical);
        report.insert("nonclear_output", nonclear);
        report.insert("depth_inheritance_proved", false);
        report.insert_or_assign("status", identical && nonclear && resetSensitive ? "passed" : "failed");
        writeReport(reportPath, report);
        require(nonclear, "all-clear sample plane cannot establish draw execution; outputs and report retained");
        require(identical, "exact selected-pass color comparison failed; outputs and report retained");
        require(resetSensitive, "reset-between-draws control was not output-sensitive; inheritance is unproved and outputs/report retained");
        std::cout << "native_vulkan_draw_compare: exact two-draw pass samples matched " << reportPath.string() << '\n';
        return 0;
    }
    catch (const std::exception& exception)
    {
        report.insert_or_assign("status", "failed");
        report.insert_or_assign("error", exception.what());
        writeReport(reportPath, report);
        throw;
    }
}

int run(int argc, char** argv)
{
    const auto* selected = &kShaderPairs.front();
    int         first    = 1;
    if (argc > first && std::string_view(argv[first]) == "--shaders")
    {
        require(argc >= 4, "--shaders requires vertex and pixel guest hashes");
        const auto pair = std::find_if(kShaderPairs.begin(), kShaderPairs.end(), [&](const ShaderPair& candidate)
                                       {
                                           return hex(candidate.vertexHash) == argv[2] && hex(candidate.pixelHash) == argv[3];
                                       });
        require(pair != kShaderPairs.end(), "unsupported guest shader pair; use an exact approved uppercase pair");
        selected = &*pair;
        first    = 4;
    }
    if (argc > first && std::string_view(argv[first]) == "--pass")
    {
        return runPass(argc, argv, first, *selected);
    }
    const bool inspect = argc > first && std::string_view(argv[first]) == "--inspect";
    const auto input   = first + (inspect ? 1 : 0);
    if (argc != input + (inspect ? 4 : 7))
    {
        std::cerr << "usage: native_vulkan_draw_compare [--shaders <VS-hash> <PS-hash>] <manifest> <shader-catalog> <matching-draw-index> <vertex.spv> <pixel.spv> <samples.spv> <fresh-output-dir>\n"
                     "       native_vulkan_draw_compare [--shaders <VS-hash> <PS-hash>] --inspect <manifest> <shader-catalog> <matching-draw-index> <fresh-output-dir>\n";
        return 2;
    }
    const auto             manifest      = std::filesystem::canonical(argv[input]);
    const auto             catalog       = std::filesystem::canonical(argv[input + 1]);
    std::size_t            matchingIndex = 0;
    const std::string_view indexText     = argv[input + 2];
    const auto             parsed        = std::from_chars(indexText.data(), indexText.data() + indexText.size(), matchingIndex);
    require(!indexText.empty() && parsed.ec == std::errc{} && parsed.ptr == indexText.data() + indexText.size(), "matching draw index must be a nonnegative decimal integer");
    const auto output = std::filesystem::weakly_canonical(std::filesystem::absolute(argv[input + (inspect ? 3 : 6)]));
    for (const auto& immutableRoot : { manifest.parent_path(), catalog })
    {
        const auto relative = output.lexically_relative(immutableRoot);
        require(relative.empty() || *relative.begin() == "..", "output directory must be outside capture inputs and shader catalog");
        for (auto ancestor = output.parent_path(); !ancestor.empty();)
        {
            require(!std::filesystem::exists(ancestor) || !std::filesystem::equivalent(ancestor, immutableRoot), "output directory aliases capture inputs or shader catalog");
            const auto parent = ancestor.parent_path();
            if (parent == ancestor)
            {
                break;
            }
            ancestor = parent;
        }
    }
    require(!std::filesystem::exists(output), "output directory must be fresh: " + output.string());
    require(std::filesystem::create_directories(output), "could not create fresh output directory");
    toml::table report{ { "compare_schema_version", 1 }, { "mode", inspect ? "inspect" : "compare" }, { "status", "loading" }, { "manifest", manifest.string() }, { "shader_catalog", catalog.string() }, { "matching_draw_index", static_cast<std::int64_t>(matchingIndex) }, { "scope", "one admitted draw on native clear; color sample0/sample3 only" }, { "depth_plane_parity_tested", false } };
    const auto  reportPath = output / "report.toml";
    try
    {
        std::string                                error;
        NativeGameplaySnapshot                     snapshot;
        std::vector<NativeGameplayShaderInterface> shaders;
        NativeRenderFrame                          frame;
        report.insert("manifest_identity", payload(readBytes(manifest, 64U * 1024U * 1024U)));
        report.insert("catalog_identity", payload(readBytes(catalog / "shaders.toml", kShaderBound)));
        require(LoadGameplayCaptureInputs(manifest, snapshot, error), error);
        require(LoadNativeGameplayShaders(catalog, shaders, error), error);
        require(BuildNativeGameplayRenderFrame(snapshot, shaders, {}, frame, error), error);
        NativeDrawReplayRecipe recipe;
        std::size_t            ordinal = 0, selectedPass = 0, selectedDraw = 0;
        bool                   found = false;
        for (std::size_t passIndex = 0; passIndex < frame.passes.size(); ++passIndex)
        {
            const auto& pass = frame.passes[passIndex];
            for (std::size_t drawIndex = 0; drawIndex < pass.DrawCount(); ++drawIndex)
            {
                const auto& candidate = pass.Draw(drawIndex);
                if (candidate.vertexShaderHash == selected->vertexHash && candidate.pixelShaderHash == selected->pixelHash)
                {
                    if (ordinal++ == matchingIndex)
                    {
                        recipe       = candidate;
                        selectedPass = passIndex;
                        selectedDraw = drawIndex;
                        found        = true;
                    }
                }
            }
        }
        require(found, "matching draw index exceeds admitted specialization occurrences");
        report.insert("matching_draw_count", static_cast<std::int64_t>(ordinal));
        report.insert("pass_index", static_cast<std::int64_t>(selectedPass));
        report.insert("draw_index", static_cast<std::int64_t>(selectedDraw));
        const auto state = recipeState(recipe);
        report.insert("recipe", state);

        NativeGameplayCommandFeed feed(shaders);
        require(feed.Begin(snapshot.start_frame, snapshot.start_submission, {}, error, {}, false, false), error);
        ordinal           = 0;
        bool lineageFound = false;
        for (const auto& event : snapshot.events)
        {
            if (event.id == snapshot.events.back().id)
            {
                break;
            }
            require(feed.Consume(event, snapshot.textures, {}, error), error);
            if (event.kind == Event::Kind::kDraw && event.draw.guest_vertex_shader_hash == selected->vertexHash && event.draw.guest_pixel_shader_hash == selected->pixelHash)
            {
                if (ordinal++ != matchingIndex)
                {
                    continue;
                }
                const auto* admitted = feed.Draw(event.id);
                require(admitted && serialize(recipeState(admitted->Recipe())) == serialize(state), "selected source event does not reproduce the ordered admitted draw recipe");
                report.insert("source", sourceState(event, snapshot));
                report.insert("source_lineage_verified", true);
                lineageFound = true;
                break;
            }
        }
        require(lineageFound, "no source event for selected admitted occurrence");
        require(recipe.initialSample0.empty() && recipe.initialSample1.empty() && recipe.depth.initialSamples.empty(), "standalone comparison requires native clear inputs");
        require(recipe.targetFormat == NativeDrawReplayTargetFormat::Rgba8, "comparison requires RGBA8 color target");
        require(recipe.sampleCount == 4 && recipe.sampleMask == 9, "comparison requires four samples and sample mask 0x9");
        for (const auto& texture : recipe.textures)
        {
            require(!texture.frameTextureId, "selected draw depends on a native producer texture");
        }
        require(ValidateNativeDrawReplayRecipe(recipe, error), error);
        if (inspect)
        {
            report.insert("selected_pass", passState(frame.passes[selectedPass]));
        }
        report.insert_or_assign("status", "inspected");
        writeReport(reportPath, report);
        if (inspect)
        {
            std::cout << "native_vulkan_draw_compare: inspected " << reportPath.string() << '\n';
            return 0;
        }
        // These validated modules own the specialization's stage and layout
        // contract. Compiler changes require validated identity updates.
        const auto vertex  = readSpirv(argv[input + 3], selected->vertexSpirv);
        const auto pixel   = readSpirv(argv[input + 4], selected->pixelSpirv);
        const auto samples = readSpirv(argv[input + 5], "d4c20e78814ae1f939670ba8a465c992987af3b8f35ff3e0547c4215fa651c7c");
        report.insert("spirv", toml::table{ { "vertex", payload(wordBytes(vertex)) }, { "pixel", payload(wordBytes(pixel)) }, { "samples", payload(wordBytes(samples)) } });
        report.insert("spirv_admission", "exact independently validated stage and layout identities; Vulkan1.2 scalar block layout");
        recipe.outputPath = output / "d3d12.samples.bin";
        NativeRendererD3D12::ConfigureReplayDiagnostics();
        NativeRendererD3D12 renderer;
        const auto          result = renderer.ReplayOffscreen(recipe);
        renderer.Shutdown();
        require(result.success, "D3D12: " + result.error);
        const auto planeBytes = static_cast<std::size_t>(recipe.width) * recipe.height * 4;
        const auto d3d        = readBytes(recipe.outputPath, planeBytes * (recipe.depth.enabled ? 4 : 2));
        require(d3d.size() == planeBytes * (recipe.depth.enabled ? 4 : 2), "D3D12 output plane count or extent mismatch");
        report.insert("d3d12_output", payload(d3d));
        report.insert("d3d12_depth_planes_preserved", recipe.depth.enabled ? 2 : 0);
        writeReport(reportPath, report);
        VulkanDrawOutput vulkan;
        const bool       vulkanSuccess = ReplayVulkanOffscreen(recipe, vertex, pixel, samples, vulkan, error);
        report.insert("vulkan_adapter_report", vulkan.adapterReport);
        require(vulkanSuccess, "Vulkan: " + error);
        require(vulkan.sample0.size() == planeBytes && vulkan.sample3.size() == planeBytes, "Vulkan output plane count or extent mismatch");
        std::vector<std::uint8_t> vulkanSamples = std::move(vulkan.sample0);
        vulkanSamples.insert(vulkanSamples.end(), vulkan.sample3.begin(), vulkan.sample3.end());
        writeBytes(output / "vulkan.samples.bin", vulkanSamples);
        report.insert("vulkan_output", payload(vulkanSamples));
        report.insert("d3d12_adapter_identity", "renderer console log: Native D3D12 adapter selected vendor/device");
        toml::array comparisons;
        bool        identical = true, nonclear = true;
        for (std::size_t plane = 0; plane < 2; ++plane)
        {
            auto comparison = comparePlane(std::span(d3d).subspan(plane * planeBytes, planeBytes), std::span(vulkanSamples).subspan(plane * planeBytes, planeBytes), recipe.clearColor, plane ? 3 : 0);
            identical &= comparison["differing_bytes"].value_or<std::int64_t>(-1) == 0;
            nonclear &= comparison["d3d12_nonclear_pixels"].value_or<std::int64_t>(0) > 0 && comparison["vulkan_nonclear_pixels"].value_or<std::int64_t>(0) > 0;
            comparisons.push_back(std::move(comparison));
        }
        report.insert("planes", std::move(comparisons));
        report.insert("exact_color_match", identical);
        report.insert("nonclear_output", nonclear);
        report.insert_or_assign("status", identical && nonclear ? "passed" : "failed");
        writeReport(reportPath, report);
        require(nonclear, "all-clear sample plane cannot establish draw execution; outputs and report retained");
        require(identical, "exact color comparison failed; outputs and report retained");
        std::cout << "native_vulkan_draw_compare: exact color samples matched " << reportPath.string() << '\n';
        return 0;
    }
    catch (const std::exception& exception)
    {
        report.insert_or_assign("status", "failed");
        report.insert_or_assign("error", exception.what());
        writeReport(reportPath, report);
        throw;
    }
}

} // namespace

int main(int argc, char** argv)
{
    rex::LogConfig logging;
    logging.log_to_console = true;
    rex::InitLogging(logging);
    int status = 1;
    try
    {
        status = run(argc, argv);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "native_vulkan_draw_compare: " << exception.what() << '\n';
    }
    rex::ShutdownLogging();
    return status;
}
