#include "gameplay_input.h"
#include "gpu/d3d12/native_gameplay_clear.h"
#include "gpu/d3d12/native_gameplay_render_frame.h"
#include "gpu/d3d12/native_gameplay_shaders.h"
#include "manual_cases.hpp"
#include "native_gameplay_fixture_inputs.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rex/crypto/sha256.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <toml++/toml.hpp>

namespace
{

using Bytes = std::vector<std::uint8_t>;

struct FixtureMemory
{
    const rex::graphics::diagnostic::DrawCaptureSnapshot* draw = nullptr;

    static bool copy(void* context, std::uint32_t address, std::span<std::uint8_t> destination) noexcept
    {
        const auto* draw = static_cast<FixtureMemory*>(context)->draw;
        if (draw)
        {
            for (const auto& fetch : draw->vertex_fetches)
                if (address == fetch.base && destination.size() == fetch.Payload().size())
                {
                    std::copy(fetch.Payload().begin(), fetch.Payload().end(), destination.begin());
                    return true;
                }
            if (draw->index.present && address == draw->index.guest_base && destination.size() <= draw->index.bytes.size())
            {
                std::copy_n(draw->index.bytes.begin(), destination.size(), destination.begin());
                return true;
            }
        }
        // This fixture checks the complete live conversion contract. Captured
        // host texture pixels are deliberately excluded from its CPU reader.
        std::fill(destination.begin(), destination.end(), 0);
        return true;
    }
};

void capturedGeometryReuse(int argc, char** argv)
{
    using namespace rerevved::gpu;
    NativeGameplaySnapshot                     snapshot, producer;
    std::vector<NativeGameplayShaderInterface> shaders;
    std::string                                error;
    UNSCOPED_INFO(error);
    REQUIRE(LoadGameplayCaptureInputs(argv[2], producer, error));
    UNSCOPED_INFO(error);
    REQUIRE(LoadGameplayCaptureInputs(argv[3], snapshot, error));
    UNSCOPED_INFO(error);
    REQUIRE(LoadNativeGameplayShaders(argv[4], shaders, error));
    NativeRenderFrame             frame;
    NativeGameplayRetainedTexture initial;
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayProducer(producer, shaders, {}, static_cast<std::uint32_t>(std::stoul(argv[5])), frame, initial, error));
    std::vector<NativeGameplayRetainedTexture> retained{ initial }, next;
    for (int index = 6; index < argc; ++index)
    {
        NativeGameplaySnapshot carry;
        UNSCOPED_INFO(error);
        REQUIRE(LoadGameplayCaptureInputs(argv[index], carry, error));
        UNSCOPED_INFO(error);
        REQUIRE(BuildNativeGameplayLiveFrame(carry, shaders, {}, retained, frame, next, error));
        retained = std::move(next);
    }
    NativeGameplayGeometryCache geometry(512U * 1024U * 1024U);
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayLiveFrame(snapshot, shaders, {}, retained, frame, next, error, &geometry));
    NativeRenderFrame repeated, independent;
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayLiveFrame(snapshot, shaders, {}, retained, repeated, next, error, &geometry));
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayLiveFrame(snapshot, shaders, {}, retained, independent, next, error));
    UNSCOPED_INFO("cross-frame geometry reuse preserves the graph");
    REQUIRE((frame.passes.size() == repeated.passes.size() && frame.passes.size() == independent.passes.size()));
    std::shared_ptr<const Bytes> held;
    for (std::size_t pass = 0; pass < frame.passes.size(); ++pass)
    {
        const auto& cold  = frame.passes[pass];
        const auto& warm  = repeated.passes[pass];
        const auto& fresh = independent.passes[pass];
        UNSCOPED_INFO("cross-frame geometry reuse preserves every draw");
        REQUIRE((cold.DrawCount() == warm.DrawCount() && cold.DrawCount() == fresh.DrawCount()));
        for (std::size_t draw = 0; draw < cold.DrawCount(); ++draw)
        {
            UNSCOPED_INFO("cross-frame cached geometry matches fresh input conversion");
            REQUIRE((std::ranges::equal(cold.Draw(draw).vertexBytes(), warm.Draw(draw).vertexBytes()) &&
                     std::ranges::equal(warm.Draw(draw).vertexBytes(), fresh.Draw(draw).vertexBytes()) &&
                     cold.Draw(draw).indices == warm.Draw(draw).indices && warm.Draw(draw).indices == fresh.Draw(draw).indices));
            if (cold.Draw(draw).sharedVertexData && cold.Draw(draw).sharedVertexData == warm.Draw(draw).sharedVertexData)
                held = warm.Draw(draw).sharedVertexData;
        }
    }
    UNSCOPED_INFO("successive frames share immutable decoded geometry");
    REQUIRE(bool(held));
    const auto bytes = *held;
    geometry.SetBudget(0);
    UNSCOPED_INFO("a retired cache cannot invalidate a converted frame");
    REQUIRE(*held == bytes);
    NativeRenderFrame restarted;
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayLiveFrame(snapshot, shaders, {}, retained, restarted, next, error, &geometry));
    for (const auto& pass : restarted.passes)
        for (std::size_t draw = 0; draw < pass.DrawCount(); ++draw)
        {
            UNSCOPED_INFO("cache retirement removes preceding geometry owners");
            REQUIRE(pass.Draw(draw).sharedVertexData != held);
        }
    std::cout << "Cross-frame cached geometry matched independent conversion and survived retirement\n";
}

void capturedLiveRetention(const std::filesystem::path& producerPath,
                           const std::filesystem::path& framePath,
                           const std::filesystem::path& shaderDirectory,
                           std::uint32_t                terrainPage,
                           std::uint32_t                carryPage,
                           std::uint64_t                firstProducerEvent,
                           std::uint64_t                lastProducerEvent)
{
    using namespace rerevved::gpu;
    NativeGameplaySnapshot                     producer, source;
    std::vector<NativeGameplayShaderInterface> shaders;
    std::string                                error;
    UNSCOPED_INFO(error);
    REQUIRE(LoadGameplayCaptureInputs(producerPath, producer, error));
    UNSCOPED_INFO(error);
    REQUIRE(LoadGameplayCaptureInputs(framePath, source, error));
    UNSCOPED_INFO(error);
    REQUIRE(LoadNativeGameplayShaders(shaderDirectory, shaders, error));
    NativeRenderFrame             frame;
    NativeGameplayRetainedTexture terrain;
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayProducer(producer, shaders, {}, terrainPage, frame, terrain, error));
    std::vector<NativeGameplayRetainedTexture> first, next;
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayLiveFrame(source, shaders, {}, { &terrain, 1 }, frame, first, error));
    const auto findPage = [](auto& retained, std::uint32_t page)
    {
        return std::find_if(retained.begin(), retained.end(), [page](const auto& entry)
                            {
                                return entry.guestBasePage == page;
                            });
    };
    const auto carried = findPage(first, carryPage);
    UNSCOPED_INFO("live conversion must retain the completed secondary producer");
    REQUIRE((carried != first.end() && carried->texture.persistentKey == lastProducerEvent &&
             carried->texture.imported && carryPage != terrainPage));
    std::map<std::uint64_t, std::size_t> aliases;
    for (const auto& entry : first)
        ++aliases[entry.texture.persistentKey];
    UNSCOPED_INFO("the captured frame must exercise shared tiled-page versions");
    REQUIRE(std::any_of(aliases.begin(), aliases.end(), [](const auto& entry)
                        {
                            return entry.second > 1;
                        }));

    // Move the same draws to the next frame, omitting only the secondary
    // producer. Its consumers now require the completed preceding version.
    auto consumer   = source;
    consumer.events = {};
    ++consumer.start_frame;
    ++consumer.end_frame;
    const auto  offset  = source.events.LastId();
    std::size_t removed = 0;
    for (auto event : source.events)
        if (event.id >= firstProducerEvent && event.id <= lastProducerEvent)
            ++removed;
        else
        {
            event.id += offset;
            ++event.frame;
            consumer.events.push_back(std::move(event));
        }
    UNSCOPED_INFO("the producer event interval must be present");
    REQUIRE((removed && firstProducerEvent <= lastProducerEvent));
    auto missing = first;
    std::erase_if(missing, [carryPage](const auto& entry)
                  {
                      return entry.guestBasePage == carryPage;
                  });
    UNSCOPED_INFO("a later frame must fail without its actual earlier producer");
    REQUIRE((!BuildNativeGameplayLiveFrame(consumer, shaders, {}, missing, frame, next, error) && next.empty() &&
             error.find("texture input is incomplete") != std::string::npos));
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayLiveFrame(consumer, shaders, {}, first, frame, next, error));
    UNSCOPED_INFO("a secondary producer must survive an unwritten later frame");
    REQUIRE((findPage(next, carryPage) != next.end() &&
             findPage(next, carryPage)->texture.persistentKey == carried->texture.persistentKey));
    for (const auto& [version, count] : aliases)
    {
        UNSCOPED_INFO("shared tiled pages must import a completed resource only once");
        REQUIRE(std::count_if(frame.textures.begin(), frame.textures.end(), [version](const auto& texture)
                              {
                                  return texture.imported && texture.persistentKey == version;
                              }) == 1);
    }
    bool importedUsed = false;
    for (const auto& pass : frame.passes)
        for (std::size_t draw = 0; draw < pass.DrawCount(); ++draw)
            for (const auto& texture : pass.Draw(draw).textures)
                if (texture.frameTextureId)
                    importedUsed |= std::any_of(frame.textures.begin(), frame.textures.end(), [&](const auto& resource)
                                                {
                                                    return resource.id == texture.frameTextureId && resource.imported &&
                                                           resource.persistentKey == carried->texture.persistentKey;
                                                });
    UNSCOPED_INFO("the later draw must bind the earlier completed native texture");
    REQUIRE(importedUsed);

    auto mismatch = first;
    findPage(mismatch, carryPage)->texture.width /= 2;
    UNSCOPED_INFO("carried versions must preserve guest layout checks");
    REQUIRE((!BuildNativeGameplayLiveFrame(consumer, shaders, {}, mismatch, frame, next, error) && next.empty() &&
             error.find("guest resource layout") != std::string::npos));
    auto       aliasMismatch = first;
    const auto shared        = std::find_if(aliasMismatch.begin(), aliasMismatch.end(), [&](const auto& entry)
                                            {
                                         return aliases.at(entry.texture.persistentKey) > 1;
                                            });
    shared->texture.width /= 2;
    UNSCOPED_INFO("aliases cannot give one completed version incompatible descriptors");
    REQUIRE((!BuildNativeGameplayLiveFrame(consumer, shaders, {}, aliasMismatch, frame, next, error) && next.empty() &&
             error.find("conflicting descriptors") != std::string::npos));

    std::map<std::uint32_t, NativeDrawReplayTexture> cpuTextures;
    for (std::uint32_t index = 0; index < consumer.textures.size(); ++index)
        if (consumer.textures[index].guest_base_page == carryPage)
        {
            auto& texture  = cpuTextures[index];
            texture.width  = carried->texture.width;
            texture.height = carried->texture.height;
            texture.format = carried->texture.format;
            texture.bytes.assign(std::size_t(texture.width) * texture.height * 4, 0x7D);
        }
    UNSCOPED_INFO("the consumer must sample the secondary page");
    REQUIRE(!cpuTextures.empty());
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayLiveFrame(consumer, shaders, cpuTextures, first, frame, next, error));
    UNSCOPED_INFO("live CPU ownership must retire an old native page even with empty snapshot payload metadata");
    REQUIRE(findPage(next, carryPage) == next.end());
    bool cpuUsed = false;
    for (const auto& pass : frame.passes)
        for (std::size_t draw = 0; draw < pass.DrawCount(); ++draw)
            for (const auto& texture : pass.Draw(draw).textures)
                cpuUsed |= !texture.frameTextureId && !texture.payloadBytes().empty() && texture.payloadBytes()[0] == 0x7D;
    UNSCOPED_INFO("the live CPU overwrite must supply the draw pixels");
    REQUIRE(cpuUsed);

    auto rewritten   = source;
    rewritten.events = {};
    for (auto event : source.events)
    {
        event.id += offset;
        rewritten.events.push_back(std::move(event));
    }
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayLiveFrame(rewritten, shaders, {}, first, frame, next, error));
    UNSCOPED_INFO("a later native write must supersede the carried version");
    REQUIRE((findPage(next, carryPage) != next.end() &&
             findPage(next, carryPage)->texture.persistentKey == lastProducerEvent + offset));
    std::cout << "Live secondary producer carry, tiled aliases, missing input, layout, CPU overwrite and rewrite passed\n";
}

void capturedLiveInputs(const std::filesystem::path& input,
                        const std::filesystem::path& shaderDirectory,
                        const std::filesystem::path& ownershipPath,
                        const std::filesystem::path& producerPath,
                        const std::filesystem::path& exportPath,
                        bool                         discoverProducer = false,
                        std::uint32_t                relocatedPage    = 0)
{
    using namespace rerevved::gpu;
    using namespace rerevved::gpu::diagnostics;
    using namespace rex::graphics::diagnostic;
    DrawCaptureFrameSnapshot                   snapshot;
    std::vector<NativeGameplayShaderInterface> shaders;
    std::string                                error;
    UNSCOPED_INFO(error);
    REQUIRE(LoadGameplayCaptureInputs(input, snapshot, error));
    if (relocatedPage)
        for (auto& event : snapshot.events)
            if (event.kind == DrawCaptureFrameEvent::Kind::kCopy && event.draw.draw.registers[0x2319] == 100242U * 0x1000)
                event.draw.draw.registers[0x2319] = relocatedPage * 0x1000;
    UNSCOPED_INFO(error);
    REQUIRE(LoadNativeGameplayShaders(shaderDirectory, shaders, error));
    NativeGameplayRetainedTexture retained;
    if (!producerPath.empty())
    {
        DrawCaptureFrameSnapshot producerInputs;
        NativeRenderFrame        producer;
        UNSCOPED_INFO(error);
        REQUIRE(LoadGameplayCaptureInputs(producerPath, producerInputs, error));
        UNSCOPED_INFO(error);
        REQUIRE(BuildNativeGameplayProducer(producerInputs, shaders, {}, 100242, producer, retained, error));
        UNSCOPED_INFO("terrain producer must retain its complete native resolve");
        REQUIRE((producer.retainOnly && retained.texture.width == 512 && retained.texture.height == 512));
    }
    const auto*                     prior = retained.guestBasePage ? &retained : nullptr;
    std::vector<NativeTextureFetch> gpuOwnedFetches;
    if (!ownershipPath.empty())
    {
        const auto  ownership = toml::parse_file(ownershipPath.string());
        const auto* textures  = ownership["textures"].as_array();
        UNSCOPED_INFO("live ownership record requires textures");
        REQUIRE(textures != nullptr);
        for (const auto& node : *textures)
        {
            const auto& texture = *node.as_table();
            if (texture["cpu_owned"].value<bool>().value())
                continue;
            const auto&        words = *texture["fetch"].as_array();
            NativeTextureFetch fetch;
            UNSCOPED_INFO("live ownership fetch extent");
            REQUIRE(words.size() == fetch.size());
            for (std::size_t word = 0; word < fetch.size(); ++word)
                fetch[word] = words[word].value<std::uint32_t>().value();
            gpuOwnedFetches.push_back(fetch);
        }
    }
    NativeGameplayLiveInputs                   inputs;
    NativeGameplayCommandFeed                  commands(shaders);
    NativeRenderFrame                          streamed;
    std::vector<NativeGameplayRetainedTexture> streamedRetained;
    if (!discoverProducer)
        inputs.SetEventConsumer([&](const DrawCaptureFrameEvent&                            event,
                                    std::span<const std::uint32_t>                          registers,
                                    std::span<const DrawCaptureTexture>                     textures,
                                    const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
                                    std::string&                                            error)
                                {
                                    return commands.Consume(event, registers, textures, cpuTextures, error);
                                });
    NativeGameplayPriorCapture         discovered(0, {});
    std::chrono::nanoseconds           collectionTime{};
    FixtureMemory                      memory;
    const DrawCaptureGuestMemoryReader reader{ &memory, &FixtureMemory::copy };
    const auto                         makeCopy = [&](const DrawCaptureFrameEvent& event, const DrawCaptureSnapshot* geometry)
    {
        DrawCaptureFrameInputCopyView copy;
        copy.id                 = event.id;
        copy.frame              = event.frame;
        copy.submission         = event.submission;
        copy.registers          = event.draw.draw.registers;
        copy.copy_mode          = event.copy_mode;
        copy.original_succeeded = true;
        copy.written_address    = copy.registers[0x2319];
        copy.written_length     = 1;
        if (geometry && !geometry->vertex_fetches.empty())
        {
            const auto& fetch    = geometry->vertex_fetches.front();
            copy.source_geometry = { fetch.fetch_constant, fetch.base, static_cast<std::uint32_t>(fetch.Payload().size()), reader };
        }
        return copy;
    };
    // Repeated gameplay frames must retain GPU destination history from the
    // preceding frame without mistaking unrelated CPU textures for resolves.
    std::vector<std::uint32_t> gpuDestinations;
    for (const auto& event : snapshot.events)
        if (event.kind == DrawCaptureFrameEvent::Kind::kCopy)
        {
            gpuDestinations.push_back(event.draw.draw.registers[0x2319]);
            UNSCOPED_INFO(error);
            REQUIRE(inputs.Copy(makeCopy(event, nullptr), false, error));
        }
    const auto collectFrame = [&]
    {
        const DrawCaptureSnapshot* pendingGeometry = nullptr;
        for (const auto& event : snapshot.events)
        {
            const auto& draw = event.draw.draw;
            memory.draw      = &draw;
            if (event.kind == DrawCaptureFrameEvent::Kind::kSwap)
            {
                DrawCaptureFrameInputSwapView swap;
                swap.id                 = event.id;
                swap.frame              = event.frame;
                swap.submission         = event.submission;
                swap.frontbuffer_ptr    = event.frontbuffer_ptr;
                swap.frontbuffer_width  = event.frontbuffer_width;
                swap.frontbuffer_height = event.frontbuffer_height;
                swap.registers          = draw.registers;
                swap.gamma_256          = snapshot.final_swap.gamma.gamma_256;
                swap.is_8bpc            = true;
                const bool first        = &event == &snapshot.events.front();
                if (discoverProducer)
                {
                    discovered.Swap(swap);
                    continue;
                }
                if (first)
                {
                    UNSCOPED_INFO(error);
                    REQUIRE(commands.Begin(swap.frame, swap.submission, { prior, prior ? 1U : 0U }, error));
                }
                const bool accepted = first ? inputs.Begin(swap, error) : inputs.Finish(swap, error);
                UNSCOPED_INFO(error);
                REQUIRE(accepted);
                if (!first)
                {
                    const auto& metadata = inputs.Snapshot();
                    UNSCOPED_INFO(error);
                    REQUIRE(commands.Finish(metadata.events.back(), metadata.final_swap, metadata.end_frame, metadata.end_submission, streamed, streamedRetained, error));
                }
                continue;
            }
            if (event.kind == DrawCaptureFrameEvent::Kind::kCopy)
            {
                memory.draw = !draw.vertex_fetches.empty() ? &draw : pendingGeometry;
                auto copy   = makeCopy(event, memory.draw);
                if (discoverProducer)
                    discovered.Copy(copy);
                else
                {
                    UNSCOPED_INFO(error);
                    REQUIRE(inputs.Copy(copy, true, error));
                }
                pendingGeometry = nullptr;
                continue;
            }
            if (!event.host_issued)
            {
                if (draw.registers[0x2208] == 6)
                    pendingGeometry = &draw;
                continue;
            }
            std::vector<DrawCaptureFrameInputVertexFetchView> vertexFetches;
            for (const auto& fetch : draw.vertex_fetches)
                vertexFetches.push_back({ fetch.fetch_constant, fetch.base, static_cast<std::uint32_t>(fetch.Payload().size()), reader });
            std::vector<DrawCaptureFrameInputTextureView> textures;
            for (const auto& use : event.draw.texture_uses)
            {
                DrawCaptureFrameInputTextureView texture;
                texture.fetch_words = std::span(draw.registers).subspan(0x4800 + use.fetch_constant * 6, 6);
                rex::graphics::xenos::xe_gpu_texture_fetch_t fetch;
                std::memcpy(&fetch, texture.fetch_words.data(), sizeof(fetch));
                rex::graphics::TextureInfo info{};
                UNSCOPED_INFO("fixture texture metadata");
                REQUIRE(rex::graphics::TextureInfo::Prepare(fetch, &info));
                texture.fetch_constant = use.fetch_constant;
                texture.format         = static_cast<std::uint32_t>(info.format);
                texture.endianness     = static_cast<std::uint32_t>(info.endianness);
                texture.dimension      = static_cast<std::uint32_t>(info.dimension);
                texture.width          = info.width;
                texture.height         = info.height;
                texture.depth          = info.depth;
                texture.pitch          = info.pitch;
                texture.base_address   = info.memory.base_address;
                texture.base_size      = info.memory.base_size;
                texture.mip_address    = info.memory.mip_address;
                texture.mip_size       = info.memory.mip_size;
                texture.gpu_written    = std::any_of(gpuDestinations.begin(), gpuDestinations.end(), [&](std::uint32_t address)
                                                     {
                                                      return (address >= texture.base_address && std::uint64_t(address) < std::uint64_t(texture.base_address) + texture.base_size) ||
                                                             (address >= texture.mip_address && std::uint64_t(address) < std::uint64_t(texture.mip_address) + texture.mip_size);
                                                     });
                texture.gpu_written |= std::any_of(gpuOwnedFetches.begin(), gpuOwnedFetches.end(), [&](const auto& owned)
                                                   {
                                                       return std::equal(owned.begin(), owned.end(), texture.fetch_words.begin());
                                                   });
                texture.stage                = use.stage;
                texture.shader_binding_index = use.shader_binding_index;
                texture.component_mapping    = use.component_mapping;
                texture.swizzled_signs       = use.swizzled_signs;
                texture.sampler_value        = use.sampler_value;
                texture.is_signed            = use.is_signed;
                texture.required             = use.required;
                texture.available            = use.data_available;
                texture.memory               = reader;
                textures.push_back(texture);
            }
            DrawCaptureFrameInputDrawView view;
            view.id                    = event.id;
            view.frame                 = event.frame;
            view.submission            = event.submission;
            view.registers             = draw.registers;
            view.vertex_shader         = { true, event.draw.guest_vertex_shader_hash, draw.vertex_ucode };
            view.pixel_shader          = { true, event.draw.guest_pixel_shader_hash, draw.pixel_ucode };
            view.primitive_type        = draw.primitive_type;
            view.requested_index_count = draw.requested_index_count;
            view.major_mode_explicit   = draw.major_mode_explicit;
            view.indexed               = draw.index.present;
            view.index                 = { draw.index.present, draw.index.format, draw.index.endianness, draw.index.dma_count, draw.index.guest_base, draw.index.dma_length, reader };
            view.vertex_fetches        = vertexFetches;
            view.textures              = textures;
            view.original_host_issued  = true;
            view.execution_status      = DrawCaptureExecutionStatus::kIssued;
            const auto captureStart    = std::chrono::steady_clock::now();
            if (discoverProducer)
            {
                discovered.Draw(view);
                collectionTime += std::chrono::steady_clock::now() - captureStart;
                continue;
            }
            const bool accepted = inputs.Draw(view, error);
            collectionTime += std::chrono::steady_clock::now() - captureStart;
            UNSCOPED_INFO("live fixture draw " + std::to_string(event.id) + ": " + error);
            REQUIRE(accepted);
        }
    };
    collectFrame();
    if (discoverProducer)
    {
        const auto* captured = discovered.Captured();
        UNSCOPED_INFO(discovered.CapturedError());
        REQUIRE((discovered.Done() && captured && discovered.CapturedError().empty()));
        UNSCOPED_INFO("discovered destination must follow the actual resolve");
        REQUIRE(discovered.Page() == (relocatedPage ? relocatedPage : 100242));
        NativeRenderFrame             producer;
        NativeGameplayRetainedTexture retained;
        UNSCOPED_INFO(error);
        REQUIRE(BuildNativeGameplayProducer(captured->Snapshot(), shaders, captured->Textures(), discovered.Page(), producer, retained, error));
        UNSCOPED_INFO("discovered inputs must convert to the complete terrain producer");
        REQUIRE((producer.retainOnly && retained.texture.width == 512 && retained.texture.height == 512));
        UNSCOPED_INFO(error);
        REQUIRE(captured->WriteSnapshot(exportPath, error));
        std::cout << "Discovered producer: page=" << discovered.Page() << " events=" << captured->Snapshot().events.size()
                  << " owned_bytes=" << captured->OwnedBytes() << " first_draw=" << captured->Snapshot().events[1].id
                  << "; synthetic CPU texture bytes, no pixel parity claim\n";
        return;
    }
    NativeRenderFrame                          frame;
    std::vector<NativeGameplayRetainedTexture> nextLive;
    const bool                                 converted = BuildNativeGameplayLiveFrame(inputs.Snapshot(), shaders, inputs.Textures(), { prior, prior ? 1U : 0U }, frame, nextLive, error);
    UNSCOPED_INFO(error);
    REQUIRE(converted);
    UNSCOPED_INFO("callback command feed preserves graph, output and proposed versions");
    REQUIRE((streamed.passes.size() == frame.passes.size() && streamed.textures.size() == frame.textures.size() &&
             streamed.outputTextureId == frame.outputTextureId && streamed.outputSwizzle == frame.outputSwizzle &&
             streamed.gammaTable == frame.gammaTable && streamedRetained.size() == nextLive.size()));
    for (std::size_t pass = 0; pass < frame.passes.size(); ++pass)
    {
        const auto& expected = frame.passes[pass];
        const auto& actual   = streamed.passes[pass];
        UNSCOPED_INFO("callback command feed preserves attachment and ordered pass state");
        REQUIRE((actual.DrawCount() == expected.DrawCount() && actual.resolves.size() == expected.resolves.size() &&
                 actual.clears.size() == expected.clears.size() && actual.target == expected.target &&
                 actual.colorSourcePass == expected.colorSourcePass &&
                 actual.preservePreviousAttachments == expected.preservePreviousAttachments));
        for (std::size_t draw = 0; draw < expected.DrawCount(); ++draw)
        {
            const auto& a = actual.Draw(draw);
            const auto& b = expected.Draw(draw);
            UNSCOPED_INFO("direct preparation preserves every owned draw input");
            REQUIRE((std::ranges::equal(a.vertexBytes(), b.vertexBytes()) && a.indices == b.indices &&
                     std::ranges::equal(a.vertexShaderBytes(), b.vertexShaderBytes()) &&
                     std::ranges::equal(a.pixelShaderBytes(), b.pixelShaderBytes()) &&
                     a.vertexConstants == b.vertexConstants && a.pixelConstants == b.pixelConstants &&
                     a.sharedConstants == b.sharedConstants && a.textures.size() == b.textures.size()));
            for (std::size_t texture = 0; texture < a.textures.size(); ++texture)
            {
                UNSCOPED_INFO("direct preparation preserves CPU and native texture ownership at each use");
                REQUIRE((a.textures[texture].frameTextureId == b.textures[texture].frameTextureId &&
                         a.textures[texture].swizzle == b.textures[texture].swizzle &&
                         std::ranges::equal(a.textures[texture].payloadBytes(), b.textures[texture].payloadBytes())));
            }
        }
    }
    if (prior)
    {
        NativeGameplayRetainedTexture next;
        NativeRenderFrame             carried;
        UNSCOPED_INFO(error);
        REQUIRE(BuildNativeGameplayRenderFrame(inputs.Snapshot(), shaders, inputs.Textures(), carried, error, prior, &next));
        UNSCOPED_INFO("a frame without a producer rewrite must carry the completed native texture version");
        REQUIRE((next.guestBasePage == prior->guestBasePage && next.texture.imported &&
                 next.texture.persistentKey == prior->texture.persistentKey));
        NativeRenderFrame invalidFrame;
        auto              mismatched = *prior;
        mismatched.texture.width /= 2;
        UNSCOPED_INFO("retained textures must reject a mismatched guest extent");
        REQUIRE((!BuildNativeGameplayRenderFrame(inputs.Snapshot(), shaders, inputs.Textures(), invalidFrame, error, &mismatched) &&
                 error.find("guest resource layout") != std::string::npos));
        UNSCOPED_INFO("live ownership must still reject the missing prior producer");
        REQUIRE((!BuildNativeGameplayRenderFrame(inputs.Snapshot(), shaders, inputs.Textures(), invalidFrame, error) &&
                 error.find("page 100242") != std::string::npos));
    }
    std::cout << "Live fixture graph converted: " << frame.passes.size() << " passes, "
              << inputs.Textures().size() << " CPU textures; zero texture data, no pixel parity claim\n";
    std::cout << "Owned input payload bytes: " << inputs.OwnedBytes() << '\n';
    std::cout << "Input draw collection microseconds: " << std::chrono::duration_cast<std::chrono::microseconds>(collectionTime).count() << '\n';
    const auto roundtripDirectory = !exportPath.empty() ? exportPath : std::filesystem::temp_directory_path() / ("rerevved-cpu-roundtrip-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto exportStart        = std::chrono::steady_clock::now();
    UNSCOPED_INFO(error);
    REQUIRE(inputs.WriteSnapshot(roundtripDirectory, error));
    std::cout << "Snapshot export microseconds: " << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - exportStart).count() << '\n';
    DrawCaptureFrameSnapshot reloaded;
    UNSCOPED_INFO(error);
    REQUIRE(LoadGameplayCaptureInputs(roundtripDirectory / "manifest.toml", reloaded, error));
    UNSCOPED_INFO("CPU snapshot roundtrip must retain all event and texture identities");
    REQUIRE((reloaded.events.size() == inputs.Snapshot().events.size() && reloaded.textures.size() == inputs.Snapshot().textures.size()));
    NativeRenderFrame reloadedFrame;
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayRenderFrame(reloaded, shaders, {}, reloadedFrame, error, prior));
    UNSCOPED_INFO("CPU snapshot roundtrip must preserve the complete graph");
    REQUIRE(reloadedFrame.passes.size() == frame.passes.size());
    for (std::size_t index = 0; index < reloaded.textures.size(); ++index)
        if (!inputs.Textures().contains(static_cast<std::uint32_t>(index)))
        {
            UNSCOPED_INFO("GPU-owned textures must remain payload-free after CPU snapshot export");
            REQUIRE((reloaded.textures[index].bytes.empty() && reloaded.textures[index].subresources.empty()));
        }
    std::error_code roundtripCleanup;
    if (exportPath.empty())
        std::filesystem::remove_all(roundtripDirectory, roundtripCleanup);
    UNSCOPED_INFO("CPU snapshot roundtrip cleanup");
    REQUIRE(!roundtripCleanup);
    std::cout << "CPU snapshot roundtrip preserved the complete graph and GPU ownership\n";
    auto rewrittenTextures = inputs.Textures();
    bool replacementAdded  = false;
    for (const auto& event : inputs.Snapshot().events)
    {
        for (const auto& use : event.draw.texture_uses)
        {
            if (!use.required || rewrittenTextures.contains(use.texture_index) ||
                inputs.Snapshot().textures.at(use.texture_index).guest_format != 6)
                continue;
            NativeTextureFetch fetch;
            std::copy_n(event.draw.draw.registers.begin() + 0x4800 + use.fetch_constant * 6,
                        fetch.size(),
                        fetch.begin());
            NativeTextureMemoryRanges ranges;
            NativeDrawReplayTexture   replacement;
            UNSCOPED_INFO(error);
            REQUIRE(GetNativeTextureMemoryRanges(fetch, ranges, error));
            const Bytes base(ranges.base.size, 0x7D), mips(ranges.mips.size, 0x7D);
            UNSCOPED_INFO(error);
            REQUIRE(DecodeNativeTexture(fetch, base, mips, replacement, error));
            replacement.sharedBytes = std::make_shared<const Bytes>(std::move(replacement.bytes));
            rewrittenTextures.emplace(use.texture_index, std::move(replacement));
            replacementAdded = true;
            break;
        }
        if (replacementAdded)
            break;
    }
    UNSCOPED_INFO("fixture has a resolved RGBA texture to test CPU reuse");
    REQUIRE(replacementAdded);
    const bool rewritten = BuildNativeGameplayRenderFrame(inputs.Snapshot(), shaders, rewrittenTextures, frame, error, prior);
    UNSCOPED_INFO(error);
    REQUIRE(rewritten);
    bool replacementUsed = false;
    for (const auto& pass : frame.passes)
        for (std::size_t draw = 0; draw < pass.DrawCount(); ++draw)
            for (const auto& texture : pass.Draw(draw).textures)
                replacementUsed |= !texture.frameTextureId && !texture.payloadBytes().empty() && texture.payloadBytes()[0] == 0x7D;
    UNSCOPED_INFO("a current CPU texture version must supersede the preceding native resolve");
    REQUIRE(replacementUsed);
    std::cout << "Live fixture CPU rewrite superseded its native resolve\n";
}

std::string digest(const Bytes& bytes)
{
    return rex::crypto::sha256(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::string fileValue(const std::string& name, const Bytes& bytes)
{
    return "{ file = '" + name + "', bytes = " + std::to_string(bytes.size()) + ", sha256 = '" + digest(bytes) + "' }";
}

void write(const std::filesystem::path& path, const Bytes& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    UNSCOPED_INFO("could not open binary fixture");
    REQUIRE(bool(file));
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    UNSCOPED_INFO("could not write binary fixture");
    REQUIRE(bool(file));
}

void write(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    UNSCOPED_INFO("could not open text fixture");
    REQUIRE(bool(file));
    file << text;
    UNSCOPED_INFO("could not write text fixture");
    REQUIRE(bool(file));
}

std::string inputManifest(const Bytes& registers,
                          const Bytes& empty,
                          const Bytes& gamma,
                          const Bytes& texture,
                          bool         malformedReason = false)
{
    const auto reason = [malformedReason](unsigned value)
    {
        return std::to_string(malformedReason && value == 6 ? 999 : value);
    };
    std::ostringstream out;
    out << "schema_version = 2\nstart_frame = 40\nend_frame = 41\n"
           "start_submission = 100\nend_submission = 102\n\n"
           "[[events]]\nid = 1\nframe = 40\nsubmission = 102\nkind = 'swap'\n"
           "success = true\nhost_issued = false\nexecution_status = 1\nexecution_reason = 0\n"
           "copy_mode = ''\nfrontbuffer = 1048576\nfrontbuffer_width = 1280\nfrontbuffer_height = 720\n"
           "guest_vertex_shader_hash = '0000000000000000'\n"
           "guest_pixel_shader_hash = '0000000000000000'\n"
        << "draw_acquisition = { availability = 1, unavailable_reason = " << reason(6) << " }\n"
                                                                                          "acquisition = 1\nacquisition_reason = 6\ntexture_bindings_complete = false\n"
                                                                                          "geometry = { primitive_type = 0, requested_index_count = 0, guest_primitive_type = 0, "
                                                                                          "host_primitive_type = 0, host_vertex_shader_type = 0, tessellation_mode = 0, "
                                                                                          "host_primitive_reset_enabled = false, guest_draw_vertex_count = 0, host_draw_vertex_count = 0, "
                                                                                          "guest_index_base = 0, native_topology = 0, native_index_count = 0, native_instance_count = 1, "
                                                                                          "native_start_vertex = 0, native_start_index = 0, native_base_vertex = 0, native_start_instance = 0, "
                                                                                          "half_pixel_offset = false }\n"
        << "registers = " << fileValue("registers.bin", registers) << "\n"
        << "vertex_ucode = " << fileValue("empty.bin", empty) << "\n"
        << "pixel_ucode = " << fileValue("empty.bin", empty) << "\n"
        << "vertex_fetches = [{ index = 0, fetch_constant = 0, base = 0, status = { availability = 1, unavailable_reason = "
        << reason(6) << " }, data = " << fileValue("empty.bin", empty) << " }]\n"
        << "index = { present = false, guest_base = 0, format = 0, endianness = 0, count = 0, status = { availability = 0, unavailable_reason = 7 }, data = "
        << fileValue("empty.bin", empty) << " }\n"
        << "texture_uses = [{ index = 0, texture_index = 4294967295, stage = 0, shader_binding_index = 0, fetch_constant = 0, dimension = 0, sampler_value = 0, component_mapping = 0, host_swizzle = 0, swizzled_signs = 0, is_signed = false, required = false, data_available = true, status = { availability = 0, unavailable_reason = 21 } }]\n\n"
           "[[textures]]\nindex = 0\nhost_format = 0\nhost_width = 0\nhost_height = 0\nhost_depth_or_array_size = 1\n"
           "host_mip_levels = 1\nmip_level = 0\nrow_size = 0\nrow_count = 0\nscaled_resolve = false\n"
           "status = { availability = 0, unavailable_reason = 10 }\n"
        << "data = " << fileValue("empty.bin", texture) << "\n\n"
                                                           "[final_swap]\nfrontbuffer = 1048576\noutput_format = 24\noutput_width = 1280\noutput_height = 720\n"
                                                           "source_status = { availability = 0, unavailable_reason = 14 }\n"
                                                           "output_status = { availability = 0, unavailable_reason = 14 }\n"
        << "gamma = " << fileValue("gamma.bin", gamma) << "\nuse_pwl_gamma_ramp = false\nuse_fxaa = false\nis_8bpc = true\n";
    return out.str();
}

std::string shaderCatalog(const std::string& shaderDigest, const std::string& wordDigest)
{
    return "schema_version = 1\n[[shaders]]\n"
           "vertex_hash = '0000000000000001'\npixel_hash = '0000000000000002'\n"
           "vertex_file = 'vertex.dxil'\npixel_file = 'pixel.dxil'\nvertex_word_sha256 = '" +
           wordDigest + "'\npixel_word_sha256 = '" + wordDigest + "'\nvertex_dxil_sha256 = '" +
           shaderDigest + "'\npixel_dxil_sha256 = '" + shaderDigest + "'\n"
                                                                      "alpha_test = false\nalpha_compare = 0\nrasterized_depth = true\nindependent_samplers = true\n"
                                                                      "program = 0\ncontext = 0\ninterpolators = 0\ntexture_fetches = []\ntexture_dimensions = []\n"
                                                                      "[[shaders.attributes]]\nfetch = 0\nstride_bytes = 16\noffset_bytes = 0\nformat = 57\nsigned = false\nnormalized = false\n";
}

void smallClearAttachment()
{
    using namespace rerevved::gpu;
    using namespace rex::graphics::diagnostic;
    DrawCaptureSnapshot draw;
    draw.registers.resize(0x5003);
    auto& r   = draw.registers;
    r[0x2000] = 0x00800050;
    r[0x2002] = 0x7FE;
    r[0x200F] = r[0x2082] = 0x20002000;
    // Captured clears keep the full index range and the D3D pixel center.
    r[0x2100] = 0xFFFF;
    r[0x2302] = 4;
    r[0x2104] = 0xF;
    r[0x210D] = 0xFF0000;
    r[0x2200] = 0x8777;
    r[0x2201] = 0x00010001;
    r[0x2204] = r[0x2205]      = 0x10000;
    r[0x2206]                  = 0x300;
    r[0x2208]                  = 4;
    draw.primitive_type        = 8;
    draw.requested_index_count = 3;
    // Pinned rectangle shaders use screen XYZ and constant per-rectangle RGBA.
    draw.vertex_ucode = { 0x30052003, 0x00001200, 0xC4000000, 0x00001005, 0x00001200, 0xC2000000, 0x00001006, 0x10071200, 0x22000000, 0x30081000, 0x00393A88, 0x00000007, 0x00080000, 0x40263688, 0x00000300, 0xC80F8000, 0x00000000, 0xE2000000, 0xC80F803E, 0x00000000, 0xE2010100, 0xC8000000, 0x00000000, 0xE2000000, 0x00000000, 0x00000000, 0x00000000 };
    draw.pixel_ucode  = { 0x00000000, 0x1001C400, 0x22000000, 0xC80F8000, 0x00000000, 0xE2000000, 0x00000000, 0x00000000, 0x00000000 };
    DrawCaptureVertexFetch fetch;
    fetch.base         = 0x1000;
    fetch.availability = DrawCaptureAvailability::kComplete;
    const std::array<std::array<float, 7>, 3> vertices{ {
        { -0.5F, -0.5F, 1, 0, 0, 0, 0 },
        { 31.5F, -0.5F, 1, 0, 0, 0, 0 },
        { 31.5F, 31.5F, 1, 0, 0, 0, 0 },
    } };
    for (const auto& vertex : vertices)
        for (const auto value : vertex)
        {
            const auto word = std::bit_cast<std::uint32_t>(value);
            for (int shift = 24; shift >= 0; shift -= 8)
                fetch.bytes.push_back(static_cast<std::uint8_t>(word >> shift));
        }
    r[0x4800] = fetch.base | 3U;
    r[0x4801] = 0x10000000U | static_cast<std::uint32_t>(fetch.bytes.size()) | 2U;
    draw.vertex_fetches.push_back(std::move(fetch));
    NativeRenderFramePass pass;
    std::string           error;
    const auto            accepted = BuildNativeGameplayClearPass(draw, pass, error);
    UNSCOPED_INFO("captured 32x32 clear: " + error);
    REQUIRE(accepted);
    UNSCOPED_INFO("the 32x32 clear preserves its padded 80-pixel single-sample attachment");
    REQUIRE((pass.target.width == 80 && pass.target.height == 32 && pass.target.sampleCount == 1 &&
             pass.target.format == NativeDrawReplayTargetFormat::Rgba8 && pass.clears.size() == 1));
    const auto& clear = pass.clears.front();
    UNSCOPED_INFO("the recorded square retains all captured color, depth and stencil clears");
    REQUIRE((clear.rectangle.left == 0 && clear.rectangle.top == 0 && clear.rectangle.right == 32 && clear.rectangle.bottom == 32 &&
             clear.color && clear.depth && clear.stencil && clear.depthValue == 1 && clear.colorValue == std::array<float, 4>{}));
    draw.registers[0x2000] ^= 1U;
    UNSCOPED_INFO("unobserved small-clear pitches remain rejected");
    REQUIRE(!BuildNativeGameplayClearPass(draw, pass, error));
    draw.registers[0x2000] ^= 1U;
    // Each state that keeps the rectangle from replacing every covered sample
    // is refused by name; inert forms of the same fields stay admitted.
    const std::array<std::tuple<std::uint32_t, std::uint32_t, const char*>, 12> refusals{ {
        { 0x2100, 0, "clear_vertex_index_clamped" },
        { 0x2100, 1, "clear_vertex_index_clamped" },
        { 0x2101, 1, "clear_vertex_index_clamped" },
        { 0x2180, 0x00040000, "clear_param_gen" },
        { 0x2180, 0x07000000, "clear_vertex_export_mode" },
        { 0x2180, 0x04000000, "clear_vertex_export_mode" },
        { 0x2293, 0x81, "clear_viz_query_kill" },
        { 0x2202, 0x0C, "clear_alpha_test" },
        { 0x2202, 0x10, "clear_alpha_to_mask" },
        { 0x2201, 0x00010000, "clear_blend_not_replace" },
        { 0x2201, 0x07060001, "clear_blend_not_replace" },
        { 0x2201, 0x00010021, "clear_blend_not_replace" },
    } };
    for (const auto& [index, value, code] : refusals)
    {
        const auto original   = draw.registers[index];
        draw.registers[index] = value;
        UNSCOPED_INFO(std::string("clear mutation refused: ") + code);
        REQUIRE((!BuildNativeGameplayClearPass(draw, pass, error) && error == code));
        draw.registers[index] = original;
    }
    draw.registers[0x2284] = 1;
    draw.registers[0x21FC] = 0x100;
    UNSCOPED_INFO("explicit tessellated clears are refused");
    REQUIRE((!BuildNativeGameplayClearPass(draw, pass, error) && error == "clear_tessellation"));
    draw.registers[0x21FC] = 0;
    for (const auto& [index, value] : std::array<std::pair<std::uint32_t, std::uint32_t>, 6>{
             { { 0x2202, 0x0F }, { 0x2293, 0x01 }, { 0x2201, 0xE0016001 }, { 0x2284, 1 }, { 0x2100, 2 }, { 0x2302, 0x3E } } })
    {
        const auto original   = draw.registers[index];
        draw.registers[index] = value;
        UNSCOPED_INFO("inert clear state stays admitted: " + error);
        REQUIRE(BuildNativeGameplayClearPass(draw, pass, error));
        draw.registers[index] = original;
    }
    // Generated pixel parameters past the 16 interpolators replace no input.
    draw.registers[0x2180] = 1U << 18;
    draw.registers[0x2181] = 16U << 8;
    UNSCOPED_INFO("param_gen beyond the interpolators stays admitted: " + error);
    REQUIRE(BuildNativeGameplayClearPass(draw, pass, error));
    draw.registers[0x2181] = 15U << 8;
    UNSCOPED_INFO("param_gen inside the interpolators is refused");
    REQUIRE((!BuildNativeGameplayClearPass(draw, pass, error) && error == "clear_param_gen"));
    draw.registers[0x2181] = 0;
    // Depth-only clears run no pixel shader in the SDK, so color state is inert.
    draw.registers[0x2208] = 5;
    draw.registers[0x2202] = 0x1C;
    draw.registers[0x2201] = 0;
    UNSCOPED_INFO("depth-only clears ignore color state: " + error);
    REQUIRE((BuildNativeGameplayClearPass(draw, pass, error) && !pass.clears.front().color));
    draw.registers[0x2208] = 4;
    draw.registers[0x2202] = 0;
    draw.registers[0x2201] = 0x00010001;
    draw.registers[0x2180] = 0;
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayClearPass(draw, pass, error));
    // The OpenGL half pixel center drops the SDK's half-pixel offset, so the
    // captured half-integer rectangle no longer covers whole pixels.
    const auto setCorners = [&](float left, float top, float right, float bottom)
    {
        const std::array<std::array<float, 2>, 3> corners{ { { left, top }, { right, top }, { right, bottom } } };
        for (std::size_t vertex = 0; vertex < 3; ++vertex)
            for (std::size_t axis = 0; axis < 2; ++axis)
            {
                const auto word = std::bit_cast<std::uint32_t>(corners[vertex][axis]);
                for (unsigned byte = 0; byte < 4; ++byte)
                    draw.vertex_fetches[0].bytes[vertex * 28 + axis * 4 + byte] = static_cast<std::uint8_t>(word >> (24 - byte * 8));
            }
    };
    draw.registers[0x2302] = 5;
    UNSCOPED_INFO("an OpenGL pixel center refuses the half-integer rectangle");
    REQUIRE((!BuildNativeGameplayClearPass(draw, pass, error) && error.find("whole native pixels") != std::string::npos));
    setCorners(0, 0, 32, 32);
    UNSCOPED_INFO("an OpenGL pixel center clears the integer rectangle unshifted: " + error);
    REQUIRE((BuildNativeGameplayClearPass(draw, pass, error) && pass.clears.front().rectangle.left == 0 &&
             pass.clears.front().rectangle.top == 0 && pass.clears.front().rectangle.right == 32 &&
             pass.clears.front().rectangle.bottom == 32));
    draw.registers[0x2302] = 4;
    setCorners(-0.5F, -0.5F, 31.5F, 31.5F);
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayClearPass(draw, pass, error));
    // Raise the final rectangle edge by one row without changing its vertex layout.
    draw.vertex_fetches[0].bytes[60] = 0x42;
    draw.vertex_fetches[0].bytes[61] = 0x02;
    UNSCOPED_INFO("the square clear rejects geometry beyond its captured 32 rows");
    REQUIRE((!BuildNativeGameplayClearPass(draw, pass, error) && error.find("normalized attachment") != std::string::npos));
}

// The old executable argv of a captured-input mode: the program, the mode flag, then the
// helper arguments.
class ModeArgv
{
public:
    explicit ModeArgv(const char* mode)
    : mode_(mode)
    {
        auto& args = rerevved::tests::CurrentManualArgv();
        pointers_.push_back(args.argv()[0]);
        pointers_.push_back(mode_.data());
        for (int index = 1; index <= args.argc(); ++index)
            pointers_.push_back(args.argv()[index]);
    }

    int argc() const
    {
        return static_cast<int>(pointers_.size());
    }

    char** argv()
    {
        return pointers_.data();
    }

private:
    std::string        mode_;
    std::vector<char*> pointers_;
};

void sequenceInputs(char** argv)
{
    using namespace rerevved::gpu;
    using namespace rex::graphics::diagnostic;
    NativeGameplaySnapshot                     producer, consumer;
    std::vector<NativeGameplayShaderInterface> shaders;
    std::string                                error;
    UNSCOPED_INFO(error);
    REQUIRE(LoadGameplayCaptureInputs(argv[2], producer, error));
    UNSCOPED_INFO(error);
    REQUIRE(LoadGameplayCaptureInputs(argv[3], consumer, error));
    UNSCOPED_INFO(error);
    REQUIRE(LoadNativeGameplayShaders(argv[4], shaders, error));
    const std::array<std::uint32_t, 1>         pages{ static_cast<std::uint32_t>(std::stoul(argv[5])) };
    NativeRenderFrame                          frame;
    std::vector<NativeGameplayRetainedTexture> first, next;
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplaySequenceFrame(producer, shaders, {}, pages, frame, first, error));
    UNSCOPED_INFO("a full frame must publish its requested producer without a separate producer snapshot");
    REQUIRE((first.size() == 1 && first[0].guestBasePage == pages[0] && first[0].texture.imported));
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplaySequenceFrame(consumer, shaders, first, pages, frame, next, error));
    UNSCOPED_INFO("an unchanged producer must survive later frames");
    REQUIRE((next.size() == 1 && next[0].texture.persistentKey == first[0].texture.persistentKey));
    UNSCOPED_INFO("retention requests must not invent a missing producer");
    REQUIRE((!BuildNativeGameplaySequenceFrame(consumer, shaders, {}, pages, frame, next, error) && next.empty()));
    auto mismatch = first;
    mismatch[0].texture.width /= 2;
    UNSCOPED_INFO("sequence imports must preserve descriptor validation");
    REQUIRE((!BuildNativeGameplaySequenceFrame(consumer, shaders, mismatch, pages, frame, next, error) &&
             error.find("guest resource layout") != std::string::npos));
    NativeGameplayEvents rewritten;
    const auto           offset = consumer.events.LastId();
    for (auto event : producer.events)
    {
        event.id += offset;
        rewritten.push_back(std::move(event));
    }
    producer.events = std::move(rewritten);
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplaySequenceFrame(producer, shaders, first, pages, frame, next, error));
    UNSCOPED_INFO("a later resolve must supersede the retained producer version");
    REQUIRE((next.size() == 1 && next[0].texture.persistentKey == first[0].texture.persistentKey + offset));
    bool changed = false;
    for (auto& texture : consumer.textures)
        if (texture.guest_base_page == pages[0])
        {
            texture.host_width               = first[0].texture.width;
            texture.host_height              = first[0].texture.height;
            texture.host_depth_or_array_size = 1;
            texture.host_mip_levels          = 1;
            texture.host_format              = 28;
            texture.row_size                 = texture.host_width * 4;
            texture.row_count                = texture.host_height;
            texture.availability             = DrawCaptureAvailability::kComplete;
            texture.bytes.assign(std::size_t(texture.row_size) * texture.row_count, 0x7D);
            texture.subresources.clear();
            changed = true;
        }
    UNSCOPED_INFO("consumer must exercise the retained page");
    REQUIRE(changed);
    NativeGameplayEvents cpuEvents;
    for (auto event : consumer.events)
    {
        for (auto& use : event.draw.texture_uses)
            if (use.texture_index < consumer.textures.size() && consumer.textures[use.texture_index].guest_base_page == pages[0])
            {
                use.availability   = DrawCaptureAvailability::kComplete;
                use.data_available = true;
            }
        cpuEvents.push_back(std::move(event));
    }
    consumer.events = std::move(cpuEvents);
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplaySequenceFrame(consumer, shaders, first, pages, frame, next, error));
    UNSCOPED_INFO("a recorded CPU overwrite must retire the old native page");
    REQUIRE(next.empty());
    bool cpuUsed = false;
    for (const auto& pass : frame.passes)
        for (std::size_t draw = 0; draw < pass.DrawCount(); ++draw)
            for (const auto& texture : pass.Draw(draw).textures)
                cpuUsed |= !texture.frameTextureId && !texture.payloadBytes().empty() && texture.payloadBytes()[0] == 0x7D;
    UNSCOPED_INFO("the CPU overwrite must supply the draw pixels");
    REQUIRE(cpuUsed);
    std::cout << "Sequence producer creation, carry, rewrite, missing input, extent and CPU overwrite passed\n";
}

void compactInputs(int argc, char** argv)
{
    for (int file = 2; file < argc; ++file)
    {
        rex::graphics::diagnostic::DrawCaptureFrameSnapshot original;
        rerevved::gpu::NativeGameplaySnapshot               compact;
        std::string                                         error;
        UNSCOPED_INFO(error);
        REQUIRE(LoadGameplayCaptureInputs(argv[file], original, error));
        UNSCOPED_INFO(error);
        REQUIRE(LoadGameplayCaptureInputs(argv[file], compact, error));
        UNSCOPED_INFO("saved compact input must preserve event/texture extents and gamma");
        REQUIRE((original.events.size() == compact.events.size() && original.textures.size() == compact.textures.size() &&
                 original.final_swap.gamma.gamma_256 == compact.final_swap.gamma.gamma_256));
        std::size_t index = 0;
        for (const auto& event : compact.events)
        {
            const auto& old = original.events.at(index++);
            const auto& a   = old.draw.draw;
            const auto& b   = event.draw.draw;
            UNSCOPED_INFO("saved compact event must preserve identities, registers, shaders and geometry");
            REQUIRE((old.id == event.id && old.kind == event.kind && old.frame == event.frame && old.submission == event.submission &&
                     a.registers == b.registers && a.vertex_ucode == b.vertex_ucode && a.pixel_ucode == b.pixel_ucode &&
                     a.index.bytes == b.index.bytes && a.vertex_fetches.size() == b.vertex_fetches.size() &&
                     old.draw.texture_uses.size() == event.draw.texture_uses.size()));
            for (std::size_t fetch = 0; fetch < a.vertex_fetches.size(); ++fetch)
            {
                UNSCOPED_INFO("saved compact vertex payload must retain exact bytes");
                REQUIRE(std::ranges::equal(a.vertex_fetches[fetch].Payload(), b.vertex_fetches[fetch].Payload()));
            }
            for (std::size_t use = 0; use < old.draw.texture_uses.size(); ++use)
            {
                UNSCOPED_INFO("saved compact event must preserve texture version references");
                REQUIRE(old.draw.texture_uses[use].texture_index == event.draw.texture_uses[use].texture_index);
            }
        }
        std::cout << "Compact saved input: " << argv[file] << " events=" << index
                  << " stored_event_bytes=" << compact.events.StoredBytes()
                  << " expanded_register_bytes=" << index * rerevved::gpu::NativeGameplayRegisterWords * sizeof(std::uint32_t) << '\n';
    }
}

void savedStatusIngestion()
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("rerevved-gameplay-input-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    UNSCOPED_INFO("test directory must be fresh");
    REQUIRE(std::filesystem::create_directories(root / "input" / "shaders"));
    const Bytes empty;
    const Bytes registers(0x5003U * 4U);
    const Bytes gamma(256U * 4U);
    const Bytes texture;
    const Bytes shader = { 'D', 'X', 'B', 'C' };
    write(root / "input" / "empty.bin", empty);
    write(root / "input" / "registers.bin", registers);
    write(root / "input" / "gamma.bin", gamma);
    write(root / "input" / "shaders" / "vertex.dxil", shader);
    write(root / "input" / "shaders" / "pixel.dxil", shader);
    write(root / "input" / "shaders" / "shaders.toml", shaderCatalog(digest(shader), digest(empty)));
    write(root / "input" / "manifest.toml", inputManifest(registers, empty, gamma, texture));
    write(root / "recipe.toml", "gameplay_frame_schema_version = 1\ninput = 'input/manifest.toml'\nshaders = 'input/shaders'\n");

    rerevved::gpu::NativeFrameReplayRecipe recipe;
    std::string                            error;
    UNSCOPED_INFO("incomplete synthetic frame must be rejected");
    REQUIRE(!LoadGameplayCaptureRecipe(root / "recipe.toml", recipe, error));
    UNSCOPED_INFO("saved status fields must parse before frame admission rejects the scene");
    REQUIRE(error.find("incomplete native gameplay scene") != std::string::npos);

    write(root / "input" / "manifest.toml", inputManifest(registers, empty, gamma, texture, true));
    UNSCOPED_INFO("malformed saved status reason must be rejected");
    REQUIRE((!LoadGameplayCaptureRecipe(root / "recipe.toml", recipe, error) &&
             error.find("invalid capture availability reason") != std::string::npos));

    rerevved::gpu::diagnostics::NativeGameplayLiveInputs     live;
    std::vector<std::uint32_t>                               boundaryRegisters(0x5003), boundaryGamma(256);
    rex::graphics::diagnostic::DrawCaptureFrameInputSwapView boundary;
    boundary.id        = 1;
    boundary.frame     = 1;
    boundary.registers = boundaryRegisters;
    boundary.gamma_256 = boundaryGamma;
    boundary.is_8bpc   = true;
    UNSCOPED_INFO(error);
    REQUIRE(live.Begin(boundary, error));
    boundary.id    = 2;
    boundary.frame = 2;
    UNSCOPED_INFO(error);
    REQUIRE(live.Finish(boundary, error));
    UNSCOPED_INFO(error);
    REQUIRE(live.WriteSnapshot(root / "cpu-inputs", error));
    rex::graphics::diagnostic::DrawCaptureFrameSnapshot cpuRoundtrip;
    UNSCOPED_INFO(error);
    REQUIRE(LoadGameplayCaptureInputs(root / "cpu-inputs/manifest.toml", cpuRoundtrip, error));
    UNSCOPED_INFO("CPU snapshot must preserve swap identities and gamma");
    REQUIRE((cpuRoundtrip.events.size() == 2 && cpuRoundtrip.events.back().id == 2 &&
             cpuRoundtrip.final_swap.gamma.gamma_256 == boundaryGamma));
    const auto packed       = toml::parse_file((root / "cpu-inputs/manifest.toml").string());
    const auto rejectPacked = [&](toml::table malformed, std::string_view expected)
    {
        std::ostringstream text;
        text << malformed;
        write(root / "cpu-inputs/malformed.toml", text.str());
        UNSCOPED_INFO("malformed packed input must reject its exact integrity violation: " + error);
        REQUIRE((!LoadGameplayCaptureInputs(root / "cpu-inputs/malformed.toml", cpuRoundtrip, error) &&
                 error.find(expected) != std::string::npos));
    };
    auto malformedPacked = packed;
    malformedPacked["events"][0]["registers"].as_table()->insert_or_assign("offset",
                                                                           static_cast<std::int64_t>(std::filesystem::file_size(root / "cpu-inputs/payloads.bin") + 1));
    rejectPacked(std::move(malformedPacked), "byte bound");
    malformedPacked = packed;
    malformedPacked["events"][0]["registers"].as_table()->insert_or_assign("register_delta", true);
    malformedPacked["events"][0]["registers"].as_table()->insert_or_assign("register_word_count", static_cast<std::int64_t>(boundaryRegisters.size()));
    rejectPacked(std::move(malformedPacked), "register delta");
    malformedPacked = packed;
    malformedPacked["events"][1]["registers"].as_table()->insert_or_assign("register_word_count", static_cast<std::int64_t>(boundaryRegisters.size() + 1));
    rejectPacked(std::move(malformedPacked), "register delta");
    UNSCOPED_INFO("CPU snapshot export must not overwrite a consumed capture");
    REQUIRE(!live.WriteSnapshot(root / "cpu-inputs", error));
    {
        using namespace rerevved::gpu;
        using namespace rex::graphics::diagnostic;
        diagnostics::NativeGameplayLiveInputs large;
        boundary.id    = 1;
        boundary.frame = 1;
        UNSCOPED_INFO(error);
        REQUIRE(large.Begin(boundary, error));
        const std::array<std::uint32_t, 3> program{ 1, 2, 3 };
        DrawCaptureFrameInputDrawView      draw;
        draw.frame                  = 2;
        draw.registers              = boundaryRegisters;
        draw.vertex_shader          = { true, 1, program };
        draw.pixel_shader           = { true, 2, program };
        draw.original_host_issued   = true;
        draw.cpu_data_complete      = true;
        draw.execution_status       = DrawCaptureExecutionStatus::kIssued;
        constexpr std::size_t draws = NativeGameplayBatchEvents + 2;
        for (std::size_t i = 0; i < draws; ++i)
        {
            draw.id              = i + 2;
            boundaryRegisters[0] = static_cast<std::uint32_t>(i);
            UNSCOPED_INFO(error);
            REQUIRE(large.Draw(draw, error));
        }
        boundary.id    = draws + 2;
        boundary.frame = 2;
        UNSCOPED_INFO(error);
        REQUIRE(large.Finish(boundary, error));
        UNSCOPED_INFO(error);
        REQUIRE(large.WriteSnapshot(root / "batched", error));
        NativeGameplaySnapshot restored;
        UNSCOPED_INFO(error);
        REQUIRE(LoadGameplayCaptureInputs(root / "batched/manifest.toml", restored, error));
        UNSCOPED_INFO("batched storage preserves the real final boundary and gamma");
        REQUIRE((restored.events.size() == draws + 2 && restored.events.BatchCount() == 2 &&
                 restored.events.back().id == boundary.id && restored.final_swap.gamma.gamma_256 == boundaryGamma));
        std::size_t index = 0;
        for (const auto& event : restored.events)
        {
            if (index && index <= draws)
            {
                UNSCOPED_INFO("batched reload preserves exact register and shader state");
                REQUIRE((event.draw.draw.registers[0] == index - 1 && event.draw.draw.vertex_ucode == std::vector<std::uint32_t>(program.begin(), program.end())));
            }
            ++index;
        }
        const auto manifest = toml::parse_file((root / "batched/manifest.toml").string());
        UNSCOPED_INFO("large frames use an explicit batched schema");
        REQUIRE((manifest["schema_version"].value_or(0) == 3 && manifest["event_batches"].as_array()->size() == 2));
        const auto reject = [&](toml::table altered, const char* expected)
        {
            std::ostringstream stream;
            stream << altered;
            write(root / "batched/rejected.toml", stream.str());
            UNSCOPED_INFO("malformed batch must fail its integrity gate: " + error);
            REQUIRE((!LoadGameplayCaptureInputs(root / "batched/rejected.toml", restored, error) && error.find(expected) != std::string::npos));
        };
        auto altered = manifest;
        altered.insert_or_assign("complete", false);
        reject(std::move(altered), "incomplete");
        altered = manifest;
        altered["event_batches"].as_array()->erase(altered["event_batches"].as_array()->begin());
        reject(std::move(altered), "out of order");
        altered = manifest;
        altered["event_batches"][1].as_table()->insert_or_assign("sha256", std::string(64, '0'));
        reject(std::move(altered), "digest mismatch");
        altered = manifest;
        altered.insert_or_assign("event_count", static_cast<std::int64_t>(draws + 3));
        reject(std::move(altered), "missing events");
        altered = manifest;
        altered.insert_or_assign("end_frame", altered["start_frame"].value<std::int64_t>().value());
        reject(std::move(altered), "swap boundary");
        std::fill(boundaryRegisters.begin(), boundaryRegisters.end(), 0);
    }
    UNSCOPED_INFO("prior capture fixture directory");
    REQUIRE(std::filesystem::create_directory(root / "prior"));
    rerevved::gpu::diagnostics::NativeGameplayPriorCapture prior(0x100, root / "prior");
    boundary.id    = 1;
    boundary.frame = 1;
    prior.Swap(boundary);
    rex::graphics::diagnostic::DrawCaptureFrameInputCopyView copy;
    copy.id                 = 2;
    copy.frame              = 1;
    copy.registers          = boundaryRegisters;
    copy.original_succeeded = true;
    copy.written_address    = 0x200000;
    copy.written_length     = 4096;
    prior.Copy(copy);
    boundary.id    = 3;
    boundary.frame = 2;
    prior.Swap(boundary);
    UNSCOPED_INFO("unrelated copy must not complete prior producer capture");
    REQUIRE(!prior.Done());
    copy.id              = 4;
    copy.frame           = 2;
    copy.written_address = 0x100000;
    copy.written_length  = 0;
    prior.Copy(copy);
    UNSCOPED_INFO("a clear-only operation must not count as a texture write");
    REQUIRE(!prior.Done());
    copy.id             = 5;
    copy.written_length = 4096;
    prior.Copy(copy);
    copy.id              = 6;
    copy.written_address = 0x300000;
    prior.Copy(copy);
    boundary.id    = 7;
    boundary.frame = 3;
    prior.Swap(boundary);
    UNSCOPED_INFO("producer capture must wait for the requested gameplay boundary");
    REQUIRE(!prior.Done());
    copy.id              = 8;
    copy.frame           = 3;
    copy.written_address = 0x100000;
    copy.written_length  = 0;
    prior.Copy(copy);
    const std::string escapedCopyMode = std::string("quote\" slash\\ tab\t line\r\n nul") + '\0' + " control\x01\x7f utf8\xc3\xa9";
    copy.copy_mode                    = escapedCopyMode;
    copy.id                           = 9;
    copy.written_address              = 0x100800;
    copy.written_length               = 2048;
    prior.Copy(copy);
    copy.id              = 10;
    copy.written_address = 0x300000;
    prior.Copy(copy);
    boundary.id    = 11;
    boundary.frame = 4;
    prior.Swap(boundary);
    boundary.id    = 12;
    boundary.frame = 5;
    prior.Swap(boundary, true);
    UNSCOPED_INFO("matching producer frame must be exported");
    REQUIRE((prior.Done() && prior.Error().empty()));
    UNSCOPED_INFO(error);
    REQUIRE(LoadGameplayCaptureInputs(root / "prior/prior-inputs/manifest.toml", cpuRoundtrip, error));
    UNSCOPED_INFO("prior capture must retain the latest complete producer frame across later unrelated frames");
    REQUIRE((cpuRoundtrip.events.size() == 5 && cpuRoundtrip.events.front().id == 7 && cpuRoundtrip.events[1].copy_mode == "clear" &&
             cpuRoundtrip.events[2].id == 9 && cpuRoundtrip.events.back().id == 11));
    UNSCOPED_INFO("CPU snapshot strings must preserve quotes, backslashes, controls and UTF-8 through TOML");
    REQUIRE(cpuRoundtrip.events[2].copy_mode == escapedCopyMode);
    copy.copy_mode = {};
    boundary.id    = 13;
    prior.Swap(boundary);
    UNSCOPED_INFO("completed prior capture must not rewrite its evidence");
    REQUIRE((prior.Done() && prior.Error().empty()));
    UNSCOPED_INFO("failed prior capture fixture directory");
    REQUIRE(std::filesystem::create_directory(root / "prior-failure"));
    rerevved::gpu::diagnostics::NativeGameplayPriorCapture failedPrior(0x100, root / "prior-failure");
    boundary.id = 1;
    failedPrior.Swap(boundary);
    copy.id                 = 2;
    copy.written_address    = 0x100000;
    copy.original_succeeded = false;
    failedPrior.Copy(copy);
    copy.id                 = 3;
    copy.original_succeeded = true;
    failedPrior.Copy(copy);
    boundary.id = 4;
    failedPrior.Swap(boundary, true);
    UNSCOPED_INFO("an incomplete producer frame must not be published as replayable input");
    REQUIRE((failedPrior.Done() && failedPrior.Error().find("original guest resolve failed") != std::string::npos &&
             !std::filesystem::exists(root / "prior-failure/prior-inputs/manifest.toml")));

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    UNSCOPED_INFO("test fixture cleanup");
    REQUIRE(!cleanupError);
}

void clearFixture(const char* path)
{
    using namespace rerevved::gpu;
    using namespace rex::graphics::diagnostic;
    std::string              error;
    DrawCaptureFrameSnapshot snapshot;
    UNSCOPED_INFO(error);
    REQUIRE(LoadGameplayCaptureInputs(path, snapshot, error));
    bool        fogZero = false, fogOne = false, fastMain = false, stencilOnly = false;
    std::size_t clearCount     = 0;
    std::size_t wideClearCount = 0;
    for (const auto& event : snapshot.events)
    {
        if (event.kind != DrawCaptureFrameEvent::Kind::kDraw ||
            event.draw.guest_vertex_shader_hash != 0x1E6883FCCDE1F688ULL)
            continue;
        const auto&           draw = event.draw.draw;
        NativeRenderFramePass pass;
        UNSCOPED_INFO(error);
        REQUIRE(BuildNativeGameplayClearPass(draw, pass, error));
        auto                  borrowedClear     = draw;
        auto                  admittedRegisters = std::move(borrowedClear.registers);
        NativeRenderFramePass borrowedPass;
        UNSCOPED_INFO("live clear preparation uses the admitted bank without a diagnostic register copy");
        REQUIRE((BuildNativeGameplayClearPass(borrowedClear, admittedRegisters, borrowedPass, error) &&
                 borrowedPass.target == pass.target && borrowedPass.clears.size() == pass.clears.size()));
        std::ranges::fill(admittedRegisters, 0);
        for (std::size_t index = 0; index < pass.clears.size(); ++index)
        {
            const auto& a = borrowedPass.clears[index];
            const auto& b = pass.clears[index];
            UNSCOPED_INFO("prepared clear values survive mutation of the borrowed register bank");
            REQUIRE((a.rectangle.left == b.rectangle.left && a.rectangle.top == b.rectangle.top &&
                     a.rectangle.right == b.rectangle.right && a.rectangle.bottom == b.rectangle.bottom &&
                     a.color == b.color && a.depth == b.depth && a.stencil == b.stencil &&
                     a.colorTargets == b.colorTargets && a.colorValue == b.colorValue &&
                     a.depthValue == b.depthValue && a.stencilValue == b.stencilValue));
        }
        UNSCOPED_INFO("a clear draw must produce clear operations");
        REQUIRE(!pass.clears.empty());
        ++clearCount;
        if (draw.registers[0x2000] == 0x020000A0 || draw.registers[0x2000] == 0x01020050)
        {
            UNSCOPED_INFO("small clear views must share the padded single-sample attachment");
            REQUIRE((pass.target.width == 160 && pass.target.height == 32 && pass.target.sampleCount == 1 &&
                     pass.target.format == NativeDrawReplayTargetFormat::Rgba8 && pass.clears.size() == 1));
            const auto& rectangle    = pass.clears.front().rectangle;
            const bool  multisampled = draw.registers[0x2000] == 0x01020050;
            UNSCOPED_INFO("small clear views must cover complementary regions of the 128x32 resolve");
            REQUIRE((rectangle.left == (multisampled ? 0 : 80) && rectangle.top == 0 &&
                     rectangle.right == (multisampled ? 80 : 128) && rectangle.bottom == 32));
            UNSCOPED_INFO("small clear views must preserve captured color, depth and stencil clears");
            REQUIRE((pass.clears.front().color && pass.clears.front().depth && pass.clears.front().stencil &&
                     pass.clears.front().depthValue == 1 && pass.clears.front().colorValue == std::array<float, 4>{}));
            auto unknownSurface = draw;
            unknownSurface.registers[0x2000] ^= 1;
            UNSCOPED_INFO("unobserved small-surface pitches must remain rejected");
            REQUIRE(!BuildNativeGameplayClearPass(unknownSurface, pass, error));
            UNSCOPED_INFO(error);
            REQUIRE(BuildNativeGameplayClearPass(draw, pass, error));
        }
        if (draw.registers[0x2000] == 0x04000140 || draw.registers[0x2000] == 0x020200A0)
        {
            ++wideClearCount;
            UNSCOPED_INFO("wide clear views must share the padded single-sample attachment");
            REQUIRE((pass.target.width == 320 && pass.target.height == 32 && pass.target.sampleCount == 1 &&
                     pass.target.format == NativeDrawReplayTargetFormat::Rgba8 && pass.clears.size() == 1));
            const auto& rectangle    = pass.clears.front().rectangle;
            const bool  multisampled = draw.registers[0x2000] == 0x020200A0;
            UNSCOPED_INFO("wide clear views must cover complementary regions of the 256x32 resolve");
            REQUIRE((rectangle.left == (multisampled ? 0 : 240) && rectangle.top == 0 &&
                     rectangle.right == (multisampled ? 240 : 256) && rectangle.bottom == 32));
            UNSCOPED_INFO("wide clear views must preserve captured color, depth and stencil clears");
            REQUIRE((pass.clears.front().color && pass.clears.front().depth && pass.clears.front().stencil &&
                     pass.clears.front().depthValue == 1 && pass.clears.front().colorValue == std::array<float, 4>{}));
            auto unknownSurface = draw;
            unknownSurface.registers[0x2000] ^= 1;
            UNSCOPED_INFO("unobserved wide-surface pitches must remain rejected");
            REQUIRE(!BuildNativeGameplayClearPass(unknownSurface, pass, error));
            UNSCOPED_INFO(error);
            REQUIRE(BuildNativeGameplayClearPass(draw, pass, error));
        }
        for (const auto& clear : pass.clears)
        {
            if (pass.target.format == NativeDrawReplayTargetFormat::Rg16Snorm)
            {
                UNSCOPED_INFO("fog depth aliases must become signed color clears");
                REQUIRE((clear.color && !clear.depth && !clear.stencil && clear.colorValue[0] == 0));
                fogZero |= clear.colorValue[1] == 0;
                fogOne |= clear.colorValue[1] == 1;
            }
            fastMain |= draw.registers[0x2000] == 0x05020140 && pass.target.width == 640 &&
                        pass.target.height == 720 && pass.target.sampleCount == 4 &&
                        clear.rectangle.right == 640 && clear.depthValue == 1;
            stencilOnly |= clear.stencil && !clear.depth && !clear.color;
        }
        auto changed = draw;
        changed.vertex_ucode[0] ^= 1;
        UNSCOPED_INFO("changed clear shader bytes must fail closed");
        REQUIRE(!BuildNativeGameplayClearPass(changed, pass, error));
        changed = draw;
        changed.vertex_fetches[0].bytes.assign(draw.vertex_fetches[0].Payload().begin(), draw.vertex_fetches[0].Payload().end());
        changed.vertex_fetches[0].shared_bytes.reset();
        changed.vertex_fetches[0].bytes.pop_back();
        UNSCOPED_INFO("truncated clear geometry must fail closed");
        REQUIRE(!BuildNativeGameplayClearPass(changed, pass, error));
        if (draw.registers[0x2000] == 0x0A020280)
        {
            changed = draw;
            changed.vertex_fetches[0].bytes.assign(draw.vertex_fetches[0].Payload().begin(), draw.vertex_fetches[0].Payload().end());
            changed.vertex_fetches[0].shared_bytes.reset();
            auto& geometry = changed.vertex_fetches[0].bytes;
            geometry[0]    = 0x3F;
            geometry[1] = geometry[2] = geometry[3] = 0;
            UNSCOPED_INFO("partial depth-to-color alias clears must fail closed");
            REQUIRE(!BuildNativeGameplayClearPass(changed, pass, error));
        }
    }
    UNSCOPED_INFO("fixture must exercise both fog aliases, fast main clears and stencil-only HUD clears");
    REQUIRE((clearCount && fogZero && fogOne && fastMain && stencilOnly));
    std::cout << "Pinned clear fixture admission and alias checks passed: " << clearCount << " draws\n";
    std::cout << "Wide clear views checked: " << wideClearCount << '\n';
}

} // namespace

TEST_CASE("native_gameplay_input", "[ctest][native-gameplay-input]")
{
    smallClearAttachment();
    savedStatusIngestion();
    std::cout << "Gameplay saved status ingestion passed\n";
}

// Captured-input modes of the old executable, run as native-gameplay-input.<mode>; the
// arguments follow the mode exactly as they followed the old mode flag.

TEST_CASE("native_gameplay_input_geometry_cache", "[.][manual][native-gameplay-input]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() == 0)
    {
        SKIP("pass the captured inputs through --helper native-gameplay-input.geometry-cache");
    }
    UNSCOPED_INFO("native-gameplay-input.geometry-cache takes a different number of arguments");
    REQUIRE((args.argc() >= 4 && args.argc() <= 7));
    ModeArgv  old("--geometry-cache-inputs");
    const int argc = old.argc();
    char**    argv = old.argv();
    smallClearAttachment();
    capturedGeometryReuse(argc, argv);
}

TEST_CASE("native_gameplay_input_live_retention", "[.][manual][native-gameplay-input]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() == 0)
    {
        SKIP("pass the captured inputs through --helper native-gameplay-input.live-retention");
    }
    UNSCOPED_INFO("native-gameplay-input.live-retention takes a different number of arguments");
    REQUIRE(args.argc() == 7);
    ModeArgv  old("--live-retention-inputs");
    const int argc = old.argc();
    char**    argv = old.argv();
    smallClearAttachment();
    capturedLiveRetention(argv[2], argv[3], argv[4], static_cast<std::uint32_t>(std::stoul(argv[5])), static_cast<std::uint32_t>(std::stoul(argv[6])), std::stoull(argv[7]), std::stoull(argv[8]));
}

TEST_CASE("native_gameplay_input_sequence", "[.][manual][native-gameplay-input]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() == 0)
    {
        SKIP("pass the captured inputs through --helper native-gameplay-input.sequence");
    }
    UNSCOPED_INFO("native-gameplay-input.sequence takes a different number of arguments");
    REQUIRE(args.argc() == 4);
    ModeArgv  old("--sequence-inputs");
    const int argc = old.argc();
    char**    argv = old.argv();
    smallClearAttachment();
    sequenceInputs(argv);
}

TEST_CASE("native_gameplay_input_compact", "[.][manual][native-gameplay-input]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() == 0)
    {
        SKIP("pass the captured inputs through --helper native-gameplay-input.compact");
    }
    UNSCOPED_INFO("native-gameplay-input.compact takes a different number of arguments");
    REQUIRE(args.argc() >= 1);
    ModeArgv  old("--compact-inputs");
    const int argc = old.argc();
    char**    argv = old.argv();
    smallClearAttachment();
    compactInputs(argc, argv);
}

TEST_CASE("native_gameplay_input_clear_fixture", "[.][manual][native-gameplay-input]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() == 0)
    {
        SKIP("pass the captured inputs through --helper native-gameplay-input.clear-fixture");
    }
    UNSCOPED_INFO("native-gameplay-input.clear-fixture takes a different number of arguments");
    REQUIRE(args.argc() == 1);
    smallClearAttachment();
    savedStatusIngestion();
    clearFixture(args.argv()[1]);
}

TEST_CASE("native_gameplay_input_live_inputs", "[.][manual][native-gameplay-input]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() == 0)
    {
        SKIP("pass the captured inputs through --helper native-gameplay-input.live-inputs");
    }
    UNSCOPED_INFO("native-gameplay-input.live-inputs takes a different number of arguments");
    REQUIRE((args.argc() >= 2 && args.argc() <= 5));
    ModeArgv  old("--live-inputs");
    const int argc = old.argc();
    char**    argv = old.argv();
    smallClearAttachment();
    savedStatusIngestion();
    capturedLiveInputs(argv[2], argv[3], argc >= 5 ? argv[4] : "", argc >= 6 ? argv[5] : "", argc == 7 ? argv[6] : "");
}

TEST_CASE("native_gameplay_input_discover_producer", "[.][manual][native-gameplay-input]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() == 0)
    {
        SKIP("pass the captured inputs through --helper native-gameplay-input.discover-producer");
    }
    UNSCOPED_INFO("native-gameplay-input.discover-producer takes a different number of arguments");
    REQUIRE((args.argc() == 3 || args.argc() == 4));
    ModeArgv  old("--discover-producer");
    const int argc = old.argc();
    char**    argv = old.argv();
    smallClearAttachment();
    savedStatusIngestion();
    capturedLiveInputs(argv[2], argv[3], {}, {}, argv[4], true, argc == 6 ? std::stoul(argv[5]) : 0);
}

REREVVED_MANUAL_HELPER(nativeGameplayInputGeometryCache, "native-gameplay-input.geometry-cache", "native_gameplay_input_geometry_cache")
REREVVED_MANUAL_HELPER(nativeGameplayInputLiveRetention, "native-gameplay-input.live-retention", "native_gameplay_input_live_retention")
REREVVED_MANUAL_HELPER(nativeGameplayInputSequence, "native-gameplay-input.sequence", "native_gameplay_input_sequence")
REREVVED_MANUAL_HELPER(nativeGameplayInputCompact, "native-gameplay-input.compact", "native_gameplay_input_compact")
REREVVED_MANUAL_HELPER(nativeGameplayInputClearFixture, "native-gameplay-input.clear-fixture", "native_gameplay_input_clear_fixture")
REREVVED_MANUAL_HELPER(nativeGameplayInputLiveInputs, "native-gameplay-input.live-inputs", "native_gameplay_input_live_inputs")
REREVVED_MANUAL_HELPER(nativeGameplayInputDiscoverProducer, "native-gameplay-input.discover-producer", "native_gameplay_input_discover_producer")
