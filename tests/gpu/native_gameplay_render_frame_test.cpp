#include "gpu/d3d12/native_gameplay_render_frame.h"
#include "native_gameplay_fixture_inputs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rex/crypto/sha256.h>

using namespace rerevved::gpu;
using rex::graphics::diagnostic::DrawCaptureFrameEvent;
using rex::graphics::diagnostic::DrawCaptureFrameSnapshot;

namespace
{

std::size_t chainPasses(std::span<const NativeRenderFrame> packets)
{
    std::size_t count = 0;
    for (const auto& packet : packets)
        count += packet.passes.size();
    return count;
}

// A pass of the full graph that a tail's chain packets and its own passes form.
const NativeRenderFramePass& chainPass(std::span<const NativeRenderFrame> packets, const NativeRenderFrame& tail, std::size_t index)
{
    for (const auto& packet : packets)
    {
        if (index < packet.passes.size())
            return packet.passes[index];
        index -= packet.passes.size();
    }
    return tail.passes.at(index);
}

NativeDrawReplayRecipe draw(std::uint32_t width,
                            std::uint32_t height,
                            std::uint32_t sampleCount,
                            std::uint32_t sampleMask)
{
    NativeDrawReplayRecipe result;
    result.schemaVersion    = 2;
    result.width            = width;
    result.height           = height;
    result.sampleCount      = sampleCount;
    result.sampleMask       = sampleMask;
    result.vertexShaderHash = 1;
    result.pixelShaderHash  = 2;
    result.vertexShaderDxil = { 0, 0, 0, 0 };
    result.pixelShaderDxil  = { 0, 0, 0, 0 };
    result.vertexData.resize(3 * 16);
    result.indices              = { 0, 1, 2 };
    result.indexCount           = 3;
    result.vertexAttributeCount = 1;
    result.vertexStrideBytes    = 16;
    result.vertexConstants.resize(4096);
    result.pixelConstants.resize(3584);
    result.sharedConstants.resize(336);
    result.viewport = { 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1 };
    result.scissor  = { 0, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height) };
    return result;
}

std::vector<std::uint32_t> mainCopyRegisters(std::uint32_t destinationBase,
                                             std::uint32_t copyControl)
{
    std::vector<std::uint32_t> registers(0x5003);
    registers[0x2000] = 0x0A010280;
    registers[0x2001] = 0;
    registers[0x2205] = 0x10000;
    registers[0x2208] = 6;
    registers[0x2318] = copyControl;
    registers[0x2319] = destinationBase;
    registers[0x231A] = 0x02D00500;
    registers[0x231B] = 0x01000300;
    registers[0x231C] = 0xFFF;
    registers[0x231D] = 0xFFFFFF00;
    return registers;
}

NativeGameplayRenderFrameCopy tiledCopy(std::uint64_t eventId,
                                        std::uint64_t triggerEventId,
                                        std::uint32_t destinationBase,
                                        std::uint32_t destinationX,
                                        std::uint32_t copyControl)
{
    NativeGameplayRenderFrameCopy copy;
    copy.eventId              = eventId;
    copy.triggerEventId       = triggerEventId;
    copy.semantics            = NativeGameplayRenderFrameCopy::Semantics::TiledMainColor;
    copy.registers            = mainCopyRegisters(destinationBase, copyControl);
    copy.sourceWidth          = 640;
    copy.sourceHeight         = 720;
    copy.destinationX         = destinationX;
    copy.destinationTextureId = 1;
    return copy;
}

NativeGameplayRenderFramePass mainPass(std::uint64_t drawEvent,
                                       std::uint64_t copyEvent,
                                       std::uint32_t destinationBase,
                                       std::uint32_t destinationX,
                                       std::uint32_t copyControl)
{
    NativeGameplayRenderFramePass pass;
    pass.firstEvent           = drawEvent;
    pass.lastEvent            = copyEvent;
    pass.attachmentAlias      = 10;
    pass.attachmentAliasValid = true;
    pass.draws.push_back({ drawEvent, draw(640, 720, 4, 9) });
    pass.copies.push_back(tiledCopy(copyEvent, drawEvent, destinationBase, destinationX, copyControl));
    pass.operations = {
        { NativeGameplayRenderFrameOperation::Kind::Draw, drawEvent, 0 },
        { NativeGameplayRenderFrameOperation::Kind::Copy, copyEvent, 0 },
    };
    return pass;
}

NativeGameplayRenderFrameInput completeMainInput()
{
    NativeGameplayRenderFrameInput input;
    input.resources            = { { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 }, true } };
    input.outputTextureId      = 1;
    input.returnResolvedOutput = true;
    input.tiledSceneBase       = 0x100000;
    input.passes.push_back(mainPass(100, 101, 0x100000, 0, 0x100340));
    input.passes.push_back(mainPass(200, 201, 0x114000, 640, 0x100040));
    return input;
}

NativeGameplayRenderFrameCopy explicitCopy(std::uint64_t eventId,
                                           std::uint64_t triggerEventId,
                                           std::uint64_t destinationTextureId,
                                           std::uint32_t width)
{
    NativeGameplayRenderFrameCopy copy;
    copy.eventId              = eventId;
    copy.triggerEventId       = triggerEventId;
    copy.semantics            = NativeGameplayRenderFrameCopy::Semantics::Explicit;
    copy.sourceWidth          = width;
    copy.sourceHeight         = 720;
    copy.destinationTextureId = destinationTextureId;
    return copy;
}

std::vector<std::uint32_t> snapshotRegisters()
{
    std::vector<std::uint32_t> registers(0x5003);
    registers[0x2000] = 0x0A010280;
    registers[0x2001] = 0;
    registers[0x210F] = std::bit_cast<std::uint32_t>(320.0F);
    registers[0x2110] = std::bit_cast<std::uint32_t>(320.0F);
    registers[0x2111] = std::bit_cast<std::uint32_t>(-360.0F);
    registers[0x2112] = std::bit_cast<std::uint32_t>(360.0F);
    registers[0x2113] = std::bit_cast<std::uint32_t>(1.0F);
    registers[0x2082] = 640U | (720U << 16);
    return registers;
}

DrawCaptureFrameEvent snapshotEvent(std::uint64_t id, DrawCaptureFrameEvent::Kind kind)
{
    DrawCaptureFrameEvent event;
    event.id                  = id;
    event.frame               = 1;
    event.submission          = 1;
    event.kind                = kind;
    event.success             = true;
    event.execution_status    = rex::graphics::diagnostic::DrawCaptureExecutionStatus::kIssued;
    event.draw.draw.registers = snapshotRegisters();
    event.frontbuffer_ptr     = 0x100000;
    return event;
}

DrawCaptureFrameSnapshot snapshotWithEvents(std::vector<DrawCaptureFrameEvent> events)
{
    DrawCaptureFrameSnapshot snapshot;
    snapshot.start_frame                = 1;
    snapshot.end_frame                  = 2;
    snapshot.start_submission           = 1;
    snapshot.end_submission             = 1;
    snapshot.final_swap.frontbuffer_ptr = 0x100000;
    snapshot.final_swap.output_format   = 24;
    snapshot.final_swap.output_width    = 1280;
    snapshot.final_swap.output_height   = 720;
    snapshot.final_swap.gamma.is_8bpc   = true;
    snapshot.final_swap.gamma.gamma_256.assign(256, 0);
    snapshot.events = std::move(events);
    return snapshot;
}

DrawCaptureFrameEvent nonIndexedDraw(std::uint64_t id)
{
    auto event                            = snapshotEvent(id, DrawCaptureFrameEvent::Kind::kDraw);
    event.host_issued                     = true;
    event.draw.guest_vertex_shader_hash   = 1;
    event.draw.guest_pixel_shader_hash    = 2;
    event.draw.draw.primitive_type        = 4;
    event.draw.draw.requested_index_count = 3;
    event.draw.draw.index.present         = false;
    return event;
}

NativeGameplayShaderInterface syntheticShader()
{
    NativeGameplayShaderInterface shader;
    shader.vertexShaderHash = 1;
    shader.pixelShaderHash  = 2;
    shader.sampleCount      = 4;
    shader.sampleMask       = 9;
    return shader;
}

void appendBigFloat(std::vector<std::uint8_t>& bytes, float value)
{
    const auto word = std::bit_cast<std::uint32_t>(value);
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>(word >> shift));
}

DrawCaptureFrameEvent clearEvent(std::uint64_t id)
{
    auto event                          = snapshotEvent(id, DrawCaptureFrameEvent::Kind::kDraw);
    event.host_issued                   = true;
    event.draw.guest_vertex_shader_hash = 0x1E6883FCCDE1F688ULL;
    event.draw.guest_pixel_shader_hash  = 0xA4A965C189287B99ULL;
    auto& draw                          = event.draw.draw;
    auto& r                             = draw.registers;
    r.assign(0x5003, 0);
    r[0x2000] = 0x0A010280;
    r[0x2002] = 0x530;
    r[0x200F] = r[0x2082] = 0x20002000;
    r[0x2100]             = 0xFFFF;
    r[0x2302]             = 4;
    r[0x2104]             = 0xF;
    r[0x210D]             = 0xFF0000;
    r[0x2200]             = 0x8777;
    r[0x2201]             = 0x00010001;
    r[0x2204] = r[0x2205]      = 0x10000;
    r[0x2206]                  = 0x300;
    r[0x2208]                  = 4;
    draw.primitive_type        = 8;
    draw.requested_index_count = 3;
    draw.vertex_ucode          = { 0x30052003, 0x00001200, 0xC4000000, 0x00001005, 0x00001200, 0xC2000000, 0x00001006, 0x10071200, 0x22000000, 0x30081000, 0x00393A88, 0x00000007, 0x00080000, 0x40263688, 0x00000300, 0xC80F8000, 0x00000000, 0xE2000000, 0xC80F803E, 0x00000000, 0xE2010100, 0xC8000000, 0x00000000, 0xE2000000, 0x00000000, 0x00000000, 0x00000000 };
    draw.pixel_ucode           = { 0x00000000, 0x1001C400, 0x22000000, 0xC80F8000, 0x00000000, 0xE2000000, 0x00000000, 0x00000000, 0x00000000 };
    rex::graphics::diagnostic::DrawCaptureVertexFetch fetch;
    fetch.base         = 0x1000;
    fetch.availability = rex::graphics::diagnostic::DrawCaptureAvailability::kComplete;
    for (const auto& vertex : std::array<std::array<float, 7>, 3>{ { { -0.5F, -0.5F, 1, 0, 0, 0, 0 }, { 639.5F, -0.5F, 1, 0, 0, 0, 0 }, { 639.5F, 719.5F, 1, 0, 0, 0, 0 } } })
        for (const auto component : vertex)
            appendBigFloat(fetch.bytes, component);
    r[0x4800] = fetch.base | 3U;
    r[0x4801] = 0x10000000U | static_cast<std::uint32_t>(fetch.bytes.size()) | 2U;
    draw.vertex_fetches.push_back(std::move(fetch));
    return event;
}

DrawCaptureFrameEvent copyEvent(std::uint64_t id, bool right = false)
{
    auto event                = snapshotEvent(id, DrawCaptureFrameEvent::Kind::kCopy);
    event.copy_mode           = "resolve";
    event.draw.draw.registers = mainCopyRegisters(right ? 0x114000 : 0x100000, right ? 0x100040 : 0x100340);
    rex::graphics::diagnostic::DrawCaptureVertexFetch fetch;
    fetch.availability = rex::graphics::diagnostic::DrawCaptureAvailability::kComplete;
    for (const auto value : std::array<float, 6>{ right ? 639.5F : -0.5F, -0.5F, right ? 1279.5F : 639.5F, -0.5F, right ? 1279.5F : 639.5F, 719.5F })
        appendBigFloat(fetch.bytes, value);
    event.draw.draw.vertex_fetches.push_back(std::move(fetch));
    return event;
}

DrawCaptureFrameEvent terminalEvent(std::uint64_t id);

void preparedCollectorClear()
{
    using namespace rex::graphics::diagnostic;
    using rerevved::gpu::diagnostics::NativeGameplayLiveInputs;
    std::array<NativeRenderFrame, 2> frames;
    for (const bool prepared : { false, true })
    {
        auto source = clearEvent(2);

        struct Memory
        {
            std::vector<std::uint8_t> bytes;

            static bool copy(void* context, std::uint32_t address, std::span<std::uint8_t> destination) noexcept
            {
                const auto& self = *static_cast<Memory*>(context);
                if (address != 0x1000 || destination.size() != self.bytes.size())
                    return false;
                std::ranges::copy(self.bytes, destination.begin());
                return true;
            }
        } memory{ source.draw.draw.vertex_fetches[0].bytes };

        const auto                originalBank         = source.draw.draw.registers;
        const auto                originalProgram      = source.draw.draw.vertex_ucode;
        const auto                originalPixelProgram = source.draw.draw.pixel_ucode;
        const auto                originalPayload      = memory.bytes;
        std::vector<std::uint8_t> leftRectangle;
        for (const auto value : std::array<float, 6>{ -0.5F, -0.5F, 639.5F, -0.5F, 639.5F, 719.5F })
            appendBigFloat(leftRectangle, value);
        Memory                               leftMemory{ leftRectangle };
        DrawCaptureFrameInputVertexFetchView vertex{ 0, 0x1000, static_cast<std::uint32_t>(memory.bytes.size()), { &memory, &Memory::copy } };
        DrawCaptureFrameInputDrawView        draw;
        draw.id    = source.id;
        draw.frame = draw.submission = 1;
        draw.registers               = source.draw.draw.registers;
        draw.vertex_shader           = { true, source.draw.guest_vertex_shader_hash, source.draw.draw.vertex_ucode };
        draw.pixel_shader            = { true, source.draw.guest_pixel_shader_hash, source.draw.draw.pixel_ucode };
        draw.primitive_type          = source.draw.draw.primitive_type;
        draw.requested_index_count   = source.draw.draw.requested_index_count;
        draw.vertex_fetches          = std::span(&vertex, 1);
        draw.original_host_issued    = !prepared;
        draw.execution_status        = prepared ? DrawCaptureExecutionStatus::kUnfinished : DrawCaptureExecutionStatus::kIssued;
        std::vector<std::uint32_t>    gamma(256);
        DrawCaptureFrameInputSwapView boundary;
        boundary.id = boundary.frame = boundary.submission = 1;
        boundary.registers                                 = source.draw.draw.registers;
        boundary.gamma_256                                 = gamma;
        boundary.is_8bpc                                   = true;
        NativeGameplayCommandFeed commands(std::span<const NativeGameplayShaderInterface>{});
        NativeGameplayLiveInputs  inputs;
        std::string               error;
        inputs.SetEventConsumer([&](const auto& event, const auto& bank, const auto& textures, const auto& cpu, std::string& failure)
                                {
                                    if (event.kind == DrawCaptureFrameEvent::Kind::kSwap)
                                        return prepared ? commands.ConsumePreparedBoundary(event, bank, failure) : commands.Consume(event, bank, textures, cpu, failure);
                                    if (event.kind == DrawCaptureFrameEvent::Kind::kCopy)
                                        return prepared ? commands.ConsumePreparedCopy(event, bank, textures, cpu, failure) : commands.Consume(event, bank, textures, cpu, failure);
                                    return prepared ? commands.ConsumePreparedDraw(event, bank, textures, cpu, failure) : commands.Consume(event, bank, textures, cpu, failure);
                                });
        UNSCOPED_INFO(error);
        REQUIRE(commands.Begin(1, 1, {}, error));
        UNSCOPED_INFO(error);
        REQUIRE(((prepared ? inputs.BeginPrepared(boundary, error) : inputs.Begin(boundary, error)) &&
                 (prepared ? inputs.PreparedDraw(draw, error) : inputs.Draw(draw, error))));
        std::fill(source.draw.draw.registers.begin(), source.draw.draw.registers.end(), 0);
        std::fill(source.draw.draw.vertex_ucode.begin(), source.draw.draw.vertex_ucode.end(), 0);
        std::fill(source.draw.draw.pixel_ucode.begin(), source.draw.draw.pixel_ucode.end(), 0);
        std::fill(memory.bytes.begin(), memory.bytes.end(), 0);
        const auto owned = inputs.Snapshot().events[1];
        UNSCOPED_INFO("collector-to-feed admission preserves exact CPU owners and original draw provenance");
        REQUIRE((owned.success == !prepared && owned.host_issued == !prepared && owned.execution_status == draw.execution_status &&
                 owned.draw.draw.registers == originalBank && owned.draw.draw.vertex_ucode == originalProgram &&
                 std::ranges::equal(owned.draw.draw.vertex_fetches[0].Payload(), originalPayload)));
        auto leftBank    = originalBank;
        leftBank[0x2080] = 0;
        leftBank[0x2081] = 0;
        leftBank[0x2082] = 0x02D00280;
        leftBank[0x2208] = 6;
        leftBank[0x2318] = 0x00100340;
        leftBank[0x2319] = 0x100000;
        leftBank[0x231A] = 0x02D00500;
        leftBank[0x231B] = 0x01000300;
        leftBank[0x231C] = 0xFFF;
        leftBank[0x231D] = 0xFFFFFF00;
        leftBank[0x231E] = leftBank[0x231F] = 0;
        DrawCaptureFrameInputCopyView leftCopy;
        leftCopy.id    = 3;
        leftCopy.frame = leftCopy.submission = 1;
        leftCopy.registers                   = leftBank;
        leftCopy.source_geometry             = { 0, 0x1000, 24, { &leftMemory, &Memory::copy } };
        leftCopy.source_rect_present         = true;
        leftCopy.source_width                = 640;
        leftCopy.source_height               = 720;
        leftCopy.cpu_data_complete           = prepared;
        leftCopy.planned_address             = 0x100000;
        leftCopy.planned_length              = 0x384000;
        leftCopy.original_succeeded          = !prepared;
        leftCopy.written_address             = prepared ? 0 : 0x100000;
        leftCopy.written_length              = prepared ? 0 : 0x384000;
        UNSCOPED_INFO(error);
        REQUIRE((prepared ? inputs.PreparedCopy(leftCopy, error) : inputs.Copy(leftCopy, true, error)));
        std::fill(leftBank.begin(), leftBank.end(), 0);
        std::fill(leftMemory.bytes.begin(), leftMemory.bytes.end(), 0);
        if (prepared)
        {
            const auto leading    = inputs.Snapshot().events[0];
            const auto sourceDraw = inputs.Snapshot().events[1];
            const auto sourceCopy = inputs.Snapshot().events[2];
            for (const unsigned invalid : { 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U })
            {
                NativeGameplayCommandFeed rejected(std::span<const NativeGameplayShaderInterface>{});
                std::size_t               prefixes = 0;
                rejected.SetPrefixConsumer([&](auto, std::string&)
                                           {
                                               ++prefixes;
                                               return true;
                                           });
                UNSCOPED_INFO(error);
                REQUIRE(rejected.Begin(1, 1, {}, error));
                auto priorDraw = sourceDraw;
                auto priorBank = sourceDraw.draw.draw.registers;
                if (invalid == 5)
                {
                    priorDraw.draw.draw.registers[0x2002] = 0x531;
                    priorBank[0x2002]                     = 0x531;
                }
                UNSCOPED_INFO(error);
                REQUIRE((rejected.ConsumePreparedBoundary(leading, leading.draw.draw.registers, error) &&
                         rejected.ConsumePreparedDraw(priorDraw, priorBank, {}, {}, error)));
                auto candidate = sourceCopy;
                if (invalid == 2 || invalid == 4)
                {
                    auto changed = candidate.draw.draw.vertex_fetches.front();
                    changed.bytes.assign(changed.Payload().begin(), changed.Payload().end());
                    changed.shared_bytes.reset();
                    candidate.draw.draw.vertex_fetches.front() = std::move(changed);
                }
                switch (invalid)
                {
                    case 0:
                        candidate.draw.draw.registers[0x231B] = 0x00000300;
                        break;
                    case 1:
                        candidate.draw.draw.registers[0x2318] ^= 1U << 4;
                        break;
                    case 2:
                        candidate.draw.draw.vertex_fetches[0].bytes[0] ^= 1U;
                        break;
                    case 3:
                        candidate.draw.draw.registers[0x2319] |= 1U;
                        break;
                    case 4:
                        candidate.draw.draw.vertex_fetches[0].bytes[0] = 0x44;
                        candidate.draw.draw.vertex_fetches[0].bytes[1] = 0x1F;
                        candidate.draw.draw.vertex_fetches[0].bytes[2] = 0xE0;
                        candidate.draw.draw.vertex_fetches[0].bytes[3] = 0x00;
                        break;
                    case 5:
                        break;
                    case 6:
                        candidate.draw.draw.registers[0x2080] = 1;
                        break;
                    case 7:
                        candidate.draw.draw.registers[0x2081] = 1;
                        break;
                    case 8:
                        candidate.draw.draw.registers[0x2082] ^= 1U;
                        break;
                }
                UNSCOPED_INFO("invalid prepared main-left clear state must fail before prefix publication");
                REQUIRE((!rejected.ConsumePreparedCopy(candidate, candidate.draw.draw.registers, {}, {}, error) && prefixes == 0));
            }
            NativeGameplayCommandFeed missing(std::span<const NativeGameplayShaderInterface>{});
            std::size_t               missingPrefixes = 0;
            missing.SetPrefixConsumer([&](auto, std::string&)
                                      {
                                          ++missingPrefixes;
                                          return true;
                                      });
            UNSCOPED_INFO(error);
            REQUIRE(missing.Begin(1, 1, {}, error));
            UNSCOPED_INFO("prepared main-left clear requires its preceding attachment before prefix publication");
            REQUIRE((missing.ConsumePreparedBoundary(leading, leading.draw.draw.registers, error) &&
                     !missing.ConsumePreparedCopy(sourceCopy, sourceCopy.draw.draw.registers, {}, {}, error) && !missingPrefixes));
        }
        auto afterDraw                 = draw;
        afterDraw.id                   = 4;
        afterDraw.registers            = originalBank;
        afterDraw.vertex_shader.words  = originalProgram;
        afterDraw.pixel_shader.words   = originalPixelProgram;
        afterDraw.original_host_issued = !prepared;
        afterDraw.execution_status     = prepared ? DrawCaptureExecutionStatus::kUnfinished : DrawCaptureExecutionStatus::kIssued;
        memory.bytes                   = originalPayload;
        UNSCOPED_INFO(error);
        REQUIRE((prepared ? inputs.PreparedDraw(afterDraw, error) : inputs.Draw(afterDraw, error)));
        auto rightEvent              = copyEvent(5, true);
        auto rightBank               = rightEvent.draw.draw.registers;
        auto rightCopy               = leftCopy;
        rightCopy.id                 = 5;
        rightCopy.registers          = rightBank;
        rightCopy.source_geometry    = { 0, 0x1000, 24, { &leftMemory, &Memory::copy } };
        rightCopy.source_x           = 640;
        rightCopy.cpu_data_complete  = prepared;
        rightCopy.original_succeeded = !prepared;
        rightCopy.written_address    = prepared ? 0 : 0x114000;
        rightCopy.written_length     = prepared ? 0 : 0x384000;
        leftMemory.bytes.clear();
        for (const auto value : std::array<float, 6>{ 639.5F, -0.5F, 1279.5F, -0.5F, 1279.5F, 719.5F })
            appendBigFloat(leftMemory.bytes, value);
        UNSCOPED_INFO(error);
        REQUIRE((prepared ? inputs.PreparedCopy(rightCopy, error) : inputs.Copy(rightCopy, true, error)));
        auto terminal             = terminalEvent(6);
        auto swap                 = snapshotWithEvents({ terminal }).final_swap;
        terminal.success          = !prepared;
        terminal.execution_status = prepared ? DrawCaptureExecutionStatus::kUnfinished : DrawCaptureExecutionStatus::kIssued;
        std::vector<NativeGameplayRetainedTexture> next;
        UNSCOPED_INFO(error);
        REQUIRE((prepared ? commands.FinishPrepared(terminal, swap, 2, 1, frames[prepared], next, error)
                          : commands.Finish(terminal, swap, 2, 1, frames[prepared], next, error)));
    }
    UNSCOPED_INFO("prepared and original issued collector inputs produce the same native clear target");
    REQUIRE((frames[0].passes.size() == frames[1].passes.size() && frames[1].passes[0].clears.size() == 1 &&
             frames[0].passes[0].target == frames[1].passes[0].target));
    const auto& issued   = frames[0].passes[0].clears[0];
    const auto& prepared = frames[1].passes[0].clears[0];
    UNSCOPED_INFO("prepared and original issued collector inputs produce identical native clear values");
    REQUIRE((issued.rectangle.left == prepared.rectangle.left && issued.rectangle.top == prepared.rectangle.top &&
             issued.rectangle.right == prepared.rectangle.right && issued.rectangle.bottom == prepared.rectangle.bottom &&
             issued.color == prepared.color && issued.depth == prepared.depth && issued.stencil == prepared.stencil &&
             issued.colorValue == prepared.colorValue && issued.depthValue == prepared.depthValue && issued.stencilValue == prepared.stencilValue));

    auto event    = clearEvent(1);
    event.success = event.host_issued = false;
    event.execution_status            = DrawCaptureExecutionStatus::kUnfinished;
    NativeGameplayCommandFeed legacy(std::span<const NativeGameplayShaderInterface>{});
    std::string               error;
    UNSCOPED_INFO("legacy feed rejects prepared provenance");
    REQUIRE((legacy.Begin(1, 1, {}, error) && !legacy.Consume(event, {}, {}, error)));
    for (int invalid = 0; invalid != 14; ++invalid)
    {
        NativeGameplayCommandFeed rejected(std::span<const NativeGameplayShaderInterface>{});
        std::size_t               prefixes = 0;
        rejected.SetPrefixConsumer([&](auto, std::string&)
                                   {
                                       ++prefixes;
                                       return true;
                                   });
        UNSCOPED_INFO(error);
        REQUIRE(rejected.Begin(1, 1, {}, error));
        auto candidate = event;
        switch (invalid)
        {
            case 0:
                candidate.success = true;
                break;
            case 1:
                candidate.host_issued = true;
                break;
            case 2:
                candidate.execution_status = DrawCaptureExecutionStatus::kIssued;
                break;
            case 3:
                candidate.execution_reason = DrawCaptureReason::kExecutionFailure;
                break;
            case 4:
                candidate.acquisition = DrawCaptureAvailability::kPartial;
                break;
            case 5:
                candidate.acquisition_reason = DrawCaptureReason::kMemexport;
                break;
            case 6:
                candidate.kind = DrawCaptureFrameEvent::Kind::kCopy;
                break;
            case 7:
                candidate.copy_mode = "resolve";
                break;
            case 8:
                candidate.draw.texture_uses.resize(1);
                break;
            case 9:
                candidate.draw.draw.registers[0x2208] = 6;
                break;
            case 10:
                candidate.draw.draw.vertex_ucode[0] ^= 1;
                break;
            case 11:
                candidate.draw.acquisition = DrawCaptureAvailability::kUnavailable;
                break;
            case 12:
                candidate.draw.acquisition_reason = DrawCaptureReason::kMemexport;
                break;
            case 13:
                candidate.draw.texture_bindings_complete = false;
                break;
        }
        UNSCOPED_INFO("prepared feed rejects forged provenance, texture, resolve and invalid native shader inputs");
        REQUIRE(!rejected.ConsumePreparedDraw(candidate, {}, {}, error));
        NativeRenderFrame                          frame;
        std::vector<NativeGameplayRetainedTexture> next;
        const auto                                 terminal = snapshotEvent(2, DrawCaptureFrameEvent::Kind::kSwap);
        const auto                                 swap     = snapshotWithEvents({ terminal }).final_swap;
        UNSCOPED_INFO("rejected prepared inputs cannot emit native prefix packets or terminal output");
        REQUIRE((prefixes == 0 && !rejected.Finish(terminal, swap, 2, 1, frame, next, error) && frame.passes.empty() && next.empty()));
    }
    for (int invalid = 0; invalid != 4; ++invalid)
    {
        NativeGameplayCommandFeed prior(std::span<const NativeGameplayShaderInterface>{});
        std::size_t               prefixes = 0;
        prior.SetPrefixConsumer([&](auto, std::string&)
                                {
                                    ++prefixes;
                                    return true;
                                },
                                NativeGameplayCommandFeed::PrefixRecording::TargetChanges);
        UNSCOPED_INFO(error);
        REQUIRE((prior.Begin(1, 1, {}, error) && prior.Consume(clearEvent(1), {}, {}, error)));
        auto candidate = event;
        candidate.id   = 2;
        if (invalid == 0)
            candidate.draw.draw.vertex_ucode[0] ^= 1;
        else if (invalid == 1)
            candidate.draw.acquisition = DrawCaptureAvailability::kUnavailable;
        else if (invalid == 2)
            candidate.draw.acquisition_reason = DrawCaptureReason::kMemexport;
        else
            candidate.draw.texture_bindings_complete = false;
        UNSCOPED_INFO("invalid prepared clear decoding or acquisition cannot emit the preceding valid clear prefix");
        REQUIRE((!prior.ConsumePreparedDraw(candidate, {}, {}, error) && prefixes == 0));
    }
}

void copyRectangle(DrawCaptureFrameEvent& event, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height)
{
    auto& fetch = event.draw.draw.vertex_fetches.front();
    fetch.base  = 0x300000;
    fetch.bytes.clear();
    const auto left = static_cast<float>(x) - 0.5F;
    const auto top  = static_cast<float>(y) - 0.5F;
    for (const auto value : std::array<float, 6>{ left, top, left + width, top, left + width, top + height })
        appendBigFloat(fetch.bytes, value);
}

void admittedResolveState()
{
    std::string                    error;
    NativeGameplayCommandFeed      commands(std::span<const NativeGameplayShaderInterface>{});
    std::vector<NativeRenderFrame> packets;
    commands.SetPrefixConsumer([&](NativeRenderFrame packet, std::string&)
                               {
                                   packets.push_back(std::move(packet));
                                   return true;
                               },
                               NativeGameplayCommandFeed::PrefixRecording::TargetChanges);
    UNSCOPED_INFO(error);
    REQUIRE((commands.Begin(1, 1, {}, error) && commands.Consume(clearEvent(1), {}, {}, error)));
    auto event              = copyEvent(2);
    auto bank               = std::move(event.draw.draw.registers);
    bank[0x2318]            = 0x100040;
    const auto originalBank = bank;
    copyRectangle(event, 0, 0, 32, 32);
    const auto* rectangleAddress = event.draw.draw.vertex_fetches[0].bytes.data();
    UNSCOPED_INFO(error);
    REQUIRE(commands.Consume(event, bank, {}, {}, error));
    UNSCOPED_INFO("live resolve admission owns scalar values without a native register bank");
    REQUIRE((commands.Copy(2) && commands.Copy(2)->admittedOperation && commands.Copy(2)->registers.empty()));
    const auto               first = *commands.Copy(2);
    NativeRenderFrameResolve resolved;
    UNSCOPED_INFO("the source creates one immutable interpreted resolve: " + error);
    REQUIRE((DeriveNativeGameplayResolve(first, 0, resolved, error) && resolved.width == 32 && resolved.height == 32 &&
             resolved.samples == std::array<std::uint32_t, 2>{ 0, 3 } && resolved.swapRedBlue));
    std::fill(bank.begin(), bank.end(), 0xDEADBEEF);
    UNSCOPED_INFO("overwriting the borrowed bank cannot change an earlier admitted resolve");
    REQUIRE((DeriveNativeGameplayResolve(first, 0, resolved, error) && resolved.width == 32 && resolved.sourceX == 0));
    bank     = originalBank;
    event.id = 3;
    copyRectangle(event, 16, 8, 16, 16);
    UNSCOPED_INFO("unchanged words still admit changed rectangle bytes at the same address: " + error);
    REQUIRE((event.draw.draw.vertex_fetches[0].bytes.data() == rectangleAddress && commands.Consume(event, bank, {}, {}, error)));
    const auto second = *commands.Copy(3);
    UNSCOPED_INFO("each event owns current geometry and resource identity while earlier packets remain fixed");
    REQUIRE((second.admittedOperation != first.admittedOperation && second.sourceX == 16 && second.sourceY == 8 &&
             second.sourceWidth == 16 && second.sourceHeight == 16 && second.destinationTextureId != first.destinationTextureId &&
             packets.size() == 2 && packets[0].passes[0].resolves[0].width == 32 && packets[1].passes[0].resolves[0].width == 16));

    event.id     = 4;
    bank[0x2000] = 0x04000140;
    bank[0x2001] = 0x00020000;
    bank[0x2319] = 0x200000;
    bank[0x231A] = 0x00100010;
    bank[0x231B] = 0x300;
    UNSCOPED_INFO(error);
    REQUIRE(commands.Consume(event, bank, {}, {}, error));
    const auto third = *commands.Copy(4);
    UNSCOPED_INFO("changed surface and destination words replace every prepared resolve value");
    REQUIRE((DeriveNativeGameplayResolve(third, 0, resolved, error) && resolved.samples == std::array<std::uint32_t, 2>{ 0, 0 } &&
             !resolved.swapRedBlue && resolved.valueScale == 1.0F && packets.back().textures.back().format == NativeDrawReplayTextureFormat::Rgba8));
    event.id     = 5;
    bank[0x2001] = 0;
    bank[0x2318] = 0x100340;
    bank[0x231B] = 0x01000300;
    bank[0x231D] = 0x123456AB;
    bank[0x231E] = 0x44332211;
    bank[0x231F] = 0x55667788;
    copyRectangle(event, 0, 0, 16, 16);
    UNSCOPED_INFO(error);
    REQUIRE(commands.Consume(event, bank, {}, {}, error));
    const auto fourth = *commands.Copy(5);
    const auto clear  = fourth.admittedOperation->Clear();
    UNSCOPED_INFO("changed clear words belong to the same immutable operation as their preceding resolve");
    REQUIRE((fourth.admittedOperation->HasClear() && clear.rectangle.right == 16 && clear.rectangle.bottom == 16 &&
             clear.depthValue == static_cast<float>(0x123456) / 16777215.0F && clear.stencilValue == 0xAB &&
             clear.colorValue == std::array<float, 4>{ 17.0F / 255, 34.0F / 255, 51.0F / 255, 68.0F / 255 }));
    NativeRenderFrame frame;
    UNSCOPED_INFO("terminal assembly consumes admitted resolves before their post-copy clears: " + error);
    REQUIRE((commands.FinishProducer(0x200, 5, frame, error) && frame.continuedPrefixPasses == chainPasses(packets) &&
             frame.continuedPrefixPasses + frame.passes.size() == 5 &&
             chainPass(packets, frame, 3).resolves.size() == 1 && chainPass(packets, frame, 3).clears.empty() &&
             chainPass(packets, frame, 4).resolves.empty() && chainPass(packets, frame, 4).clears[0].stencilValue == 0xAB));
    UNSCOPED_INFO(error);
    REQUIRE((commands.Begin(1, 1, {}, error) && !commands.Copy(5) && commands.Consume(clearEvent(1), {}, {}, error)));
    event.id = 2;
    copyRectangle(event, 0, 0, 24, 24);
    UNSCOPED_INFO("Begin carries scalar state while rectangle and destination identities remain frame owned: " + error);
    REQUIRE((commands.Consume(event, bank, {}, {}, error) && commands.Copy(2)->destinationTextureId == 1 &&
             commands.Copy(2)->sourceWidth == 24 && commands.Copy(2)->admittedOperation->Clear().stencilValue == 0xAB));
    bank.clear();
    bank.shrink_to_fit();
    event.draw.draw.vertex_fetches.clear();
    UNSCOPED_INFO("destroyed callback banks and geometry do not invalidate owned operations: " + error);
    REQUIRE((DeriveNativeGameplayResolve(first, 0, resolved, error) && resolved.width == 32 && resolved.height == 32 &&
             commands.FinishProducer(0x200, 2, frame, error)));

    NativeGameplayRenderFrameInput input;
    input.resources                          = { { { first.destinationTextureId, 32, 32, NativeDrawReplayTextureFormat::Rgba8 }, true } };
    input.outputTextureId                    = first.destinationTextureId;
    input.retainOnly                         = true;
    input.resources[0].texture.persistentKey = first.eventId;
    NativeGameplayRenderFramePass pass;
    pass.firstEvent = 1;
    pass.lastEvent  = 2;
    pass.target     = { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 };
    NativeRenderFramePass::Clear initialize;
    initialize.rectangle = { 0, 0, 640, 720 };
    initialize.color = initialize.depth = initialize.stencil = true;
    pass.clears                                              = { initialize };
    pass.copies                                              = { first };
    pass.operations                                          = { { NativeGameplayRenderFrameOperation::Kind::Copy, 2, 0 } };
    input.passes                                             = { pass };
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayRenderFrame(input, frame, error));
    using CopyMutator = void (*)(NativeGameplayRenderFrameCopy&);
    const std::array<CopyMutator, 16> mutations{ [](auto& value)
                                                 {
                                                     ++value.eventId;
                                                 },
                                                 [](auto& value)
                                                 {
                                                     ++value.triggerEventId;
                                                 },
                                                 [](auto& value)
                                                 {
                                                     value.sourceGeometryOwnedByCopy = !value.sourceGeometryOwnedByCopy;
                                                 },
                                                 [](auto& value)
                                                 {
                                                     value.semantics = NativeGameplayRenderFrameCopy::Semantics::TiledMainColor;
                                                 },
                                                 [](auto& value)
                                                 {
                                                     ++value.sourceX;
                                                 },
                                                 [](auto& value)
                                                 {
                                                     ++value.sourceY;
                                                 },
                                                 [](auto& value)
                                                 {
                                                     ++value.sourceWidth;
                                                 },
                                                 [](auto& value)
                                                 {
                                                     ++value.sourceHeight;
                                                 },
                                                 [](auto& value)
                                                 {
                                                     ++value.destinationX;
                                                 },
                                                 [](auto& value)
                                                 {
                                                     ++value.destinationTextureId;
                                                 },
                                                 [](auto& value)
                                                 {
                                                     value.depth = !value.depth;
                                                 },
                                                 [](auto& value)
                                                 {
                                                     ++value.colorTarget;
                                                 },
                                                 [](auto& value)
                                                 {
                                                     ++value.samples[0];
                                                 },
                                                 [](auto& value)
                                                 {
                                                     value.swapRedBlue = !value.swapRedBlue;
                                                 },
                                                 [](auto& value)
                                                 {
                                                     value.valueScale = 2;
                                                 },
                                                 [](auto& value)
                                                 {
                                                     ++value.tiledSceneBase;
                                                 } };
    for (const auto mutate : mutations)
    {
        auto wrong = first;
        mutate(wrong);
        UNSCOPED_INFO("every wrapper identity and semantic field must match its private admitted owner");
        REQUIRE((!DeriveNativeGameplayResolve(wrong, 0, resolved, error) && error.find("fields disagree") != std::string::npos));
    }
    auto wrong      = first;
    wrong.registers = originalBank;
    UNSCOPED_INFO("raw registers cannot accompany an admitted operation");
    REQUIRE((!DeriveNativeGameplayResolve(wrong, 0, resolved, error) && error.find("mixes raw") != std::string::npos));
    wrong                   = first;
    wrong.admittedOperation = std::shared_ptr<const NativeGameplayResolveAdmission>(first.admittedOperation.get(), [](const auto*)
                                                                                    {
                                                                                    });
    UNSCOPED_INFO("a fabricated non-owning wrapper cannot claim the canonical object");
    REQUIRE((!DeriveNativeGameplayResolve(wrong, 0, resolved, error) && error.find("canonical wrapper") != std::string::npos));
    wrong                     = first;
    wrong.admittedOperation   = second.admittedOperation;
    input.passes[0].copies[0] = wrong;
    UNSCOPED_INFO("generic assembly rejects a canonical owner paired with another event's fields");
    REQUIRE((!BuildNativeGameplayRenderFrame(input, frame, error) && error.find("fields disagree") != std::string::npos && frame.passes.empty()));
}

void changedTiledResolveState()
{
    for (const auto index : { 0x2205U, 0x2208U, 0x231AU, 0x231CU, 0x231DU, 0x231EU, 0x231FU })
    {
        std::string               error;
        std::size_t               emitted = 0;
        NativeGameplayCommandFeed commands(std::span<const NativeGameplayShaderInterface>{});
        commands.SetPrefixConsumer([&](NativeRenderFrame, std::string&)
                                   {
                                       ++emitted;
                                       return true;
                                   },
                                   NativeGameplayCommandFeed::PrefixRecording::TargetChanges);
        UNSCOPED_INFO(error);
        REQUIRE((commands.Begin(1, 1, {}, error) && commands.Consume(clearEvent(1), {}, {}, error) &&
                 commands.Consume(copyEvent(2), {}, {}, error)));
        const auto earlier = *commands.Copy(2);
        auto       changed = copyEvent(3, true);
        changed.draw.draw.registers[index] ^= 1;
        UNSCOPED_INFO("invalid changed tiled state must reject before a second packet is admitted");
        REQUIRE((!commands.Consume(changed, {}, {}, error) && emitted == 1 && !commands.Copy(3)));
        NativeRenderFrameResolve resolved;
        UNSCOPED_INFO("changed-state failure preserves earlier operations and poisons further frame preparation");
        REQUIRE((DeriveNativeGameplayResolve(earlier, 0, resolved, error) && resolved.destinationX == 0 &&
                 !commands.Consume(copyEvent(4, true), {}, {}, error)));
    }
}

void rejectChangedResolveBeforePendingClear()
{
    for (unsigned failure = 0; failure < 3; ++failure)
    {
        std::string               error;
        std::size_t               emitted = 0;
        NativeGameplayCommandFeed commands(std::span<const NativeGameplayShaderInterface>{});
        commands.SetPrefixConsumer([&](NativeRenderFrame, std::string&)
                                   {
                                       ++emitted;
                                       return true;
                                   },
                                   NativeGameplayCommandFeed::PrefixRecording::TargetChanges);
        UNSCOPED_INFO(error);
        REQUIRE((commands.Begin(1, 1, {}, error) && commands.Consume(clearEvent(1), {}, {}, error) &&
                 commands.Consume(copyEvent(2), {}, {}, error) && emitted == 1));
        const auto earlier = *commands.Copy(2);
        auto       changed = copyEvent(3, true);
        auto&      words   = changed.draw.draw.registers;
        if (failure < 2)
        {
            words[0x2319] = 0x14000;
            if (failure == 1)
            {
                words[0x2001] = 0x00020000;
                words[0x231B] = 0x380;
            }
        }
        else
        {
            words[0x2318] = 0x100244;
            words[0x2319] = 0x200000;
            words[0x231B] = 0x302;
            copyRectangle(changed, 0, 0, 32, 32);
        }
        const auto expected = failure < 2 ? "aligned nonzero scene base" : "depth resolve cannot clear";
        UNSCOPED_INFO("invalid changed resolve must reject before emitting even the preceding pending clear packet");
        REQUIRE((!commands.Consume(changed, {}, {}, error) && error.find(expected) != std::string::npos &&
                 emitted == 1 && !commands.Copy(3)));
        NativeRenderFrameResolve resolved;
        UNSCOPED_INFO("rejected current state cannot reuse stale validation or change an earlier admitted operation");
        REQUIRE((DeriveNativeGameplayResolve(earlier, 0, resolved, error) && resolved.textureId == earlier.destinationTextureId &&
                 resolved.destinationX == 0 && !commands.Consume(copyEvent(4, true), {}, {}, error) && emitted == 1));
    }
}

void admittedTiledResolveOrder()
{
    std::string                    error;
    NativeGameplayCommandFeed      commands(std::span<const NativeGameplayShaderInterface>{});
    std::vector<NativeRenderFrame> packets;
    commands.SetPrefixConsumer([&](NativeRenderFrame packet, std::string&)
                               {
                                   packets.push_back(std::move(packet));
                                   return true;
                               },
                               NativeGameplayCommandFeed::PrefixRecording::TargetChanges);
    UNSCOPED_INFO(error);
    REQUIRE((commands.Begin(1, 1, {}, error) && commands.Consume(clearEvent(1), {}, {}, error) &&
             commands.Consume(copyEvent(2), {}, {}, error)));
    const auto color                  = *commands.Copy(2);
    auto       depth                  = copyEvent(3);
    depth.draw.draw.registers[0x2318] = 0x100014;
    depth.draw.draw.registers[0x2319] = 0x200000;
    depth.draw.draw.registers[0x231B] = 0x302;
    UNSCOPED_INFO(error);
    REQUIRE(commands.Consume(depth, {}, {}, error));
    const auto firstDepth             = *commands.Copy(3);
    depth.id                          = 4;
    depth.draw.draw.registers[0x2319] = 0x214000;
    depth.draw.draw.registers[0x2318] = 0x100044;
    copyRectangle(depth, 640, 0, 640, 720);
    UNSCOPED_INFO(error);
    REQUIRE((commands.Consume(depth, {}, {}, error) && commands.Consume(copyEvent(5, true), {}, {}, error)));
    const auto secondDepth = *commands.Copy(4);
    UNSCOPED_INFO("tiled depth state changes select current samples while both halves share one event-time depth resource");
    REQUIRE((firstDepth.depth && secondDepth.depth && firstDepth.destinationTextureId == secondDepth.destinationTextureId &&
             firstDepth.destinationTextureId != color.destinationTextureId && secondDepth.destinationX == 640 &&
             firstDepth.samples == std::array<std::uint32_t, 2>{ 3, 3 } && secondDepth.samples == std::array<std::uint32_t, 2>{ 0, 0 } &&
             !firstDepth.admittedOperation->HasClear() && !secondDepth.admittedOperation->HasClear()));
    NativeRenderFrame frame;
    const auto        pass = [&](std::size_t index) -> const NativeRenderFramePass&
    {
        return chainPass(packets, frame, index);
    };
    // Every pass was a packet, so the producer tail carries only its output.
    UNSCOPED_INFO("prefix and terminal assembly preserve color resolve, post-copy clear, depth halves and color half order: " + error);
    REQUIRE((commands.FinishProducer(0x114, 5, frame, error) && frame.passes.empty() && frame.continuedPrefixPasses == 5 &&
             packets.size() == 5 && chainPasses(packets) == 5 &&
             pass(0).resolves[0].depth == false && pass(0).resolves[0].swapRedBlue &&
             pass(1).resolves.empty() && pass(1).clears.size() == 1 &&
             pass(2).resolves[0].depth && pass(2).resolves[0].samples[0] == 3 &&
             pass(3).resolves[0].depth && pass(3).resolves[0].samples[0] == 0 &&
             !pass(4).resolves[0].depth && pass(4).resolves[0].destinationX == 640));
}

DrawCaptureFrameEvent terminalEvent(std::uint64_t id)
{
    auto  event = snapshotEvent(id, DrawCaptureFrameEvent::Kind::kSwap);
    auto& r     = event.draw.draw.registers;
    r[0x4800]   = 2;
    r[0x4801]   = event.frontbuffer_ptr | 6;
    r[0x4802]   = 1279U | (719U << 13);
    r[0x4803]   = 0xD10;
    return event;
}

NativeDrawReplayResult recordingResult(bool success = true, std::string error = {})
{
    NativeDrawReplayResult result;
    result.success = success;
    result.error   = std::move(error);
    return result;
}

void admittedTerminalFailures()
{
    for (unsigned failure = 0; failure < 4; ++failure)
    {
        std::string               error;
        NativeGameplayCommandFeed commands(std::span<const NativeGameplayShaderInterface>{});
        UNSCOPED_INFO(error);
        REQUIRE((commands.Begin(1, 1, {}, error) && commands.Consume(clearEvent(1), {}, {}, error) &&
                 commands.Consume(copyEvent(2), {}, {}, error) && commands.Consume(clearEvent(3), {}, {}, error)));
        const auto earlier = *commands.Copy(2);
        if (failure != 2)
        {
            UNSCOPED_INFO(error);
            REQUIRE(commands.Consume(copyEvent(4, true), {}, {}, error));
        }
        auto terminal = terminalEvent(5);
        auto boundary = snapshotWithEvents({ terminal });
        if (failure == 0)
            terminal.draw.draw.registers[0x4801] ^= 1;
        else if (failure == 1)
            boundary.final_swap.gamma.gamma_256.pop_back();
        else if (failure == 3)
            terminal.draw.draw.registers[0x4803] = 0xFFFFFFFF;
        NativeRenderFrame                          frame;
        std::vector<NativeGameplayRetainedTexture> next;
        UNSCOPED_INFO("admitted operations still require valid final fetch, gamma, output coverage and swizzle");
        REQUIRE((!commands.Finish(terminal, boundary.final_swap, 2, 1, frame, next, error) && !error.empty() && next.empty()));
        NativeRenderFrameResolve resolve;
        UNSCOPED_INFO("terminal failure keeps earlier semantic owners immutable and ends frame preparation");
        REQUIRE((DeriveNativeGameplayResolve(earlier, 0, resolve, error) && resolve.width == 640 &&
                 !commands.Consume(copyEvent(6, true), {}, {}, error)));
    }
}

void preparedTerminalAdmission()
{
    using namespace rex::graphics::diagnostic;
    std::array<NativeRenderFrame, 2>                          frames;
    std::array<std::vector<NativeGameplayRetainedTexture>, 2> banks;
    for (const bool prepared : { false, true })
    {
        std::string               error;
        NativeGameplayCommandFeed commands(std::span<const NativeGameplayShaderInterface>{});
        UNSCOPED_INFO(error);
        REQUIRE(commands.Begin(1, 1, {}, error));
        auto leading = snapshotEvent(1, DrawCaptureFrameEvent::Kind::kSwap);
        if (prepared)
        {
            leading.success          = false;
            leading.execution_status = DrawCaptureExecutionStatus::kUnfinished;
        }
        UNSCOPED_INFO(error);
        REQUIRE((prepared ? commands.ConsumePreparedBoundary(leading, leading.draw.draw.registers, error) : commands.Consume(leading, {}, {}, error)));
        for (auto event : { clearEvent(2), copyEvent(3), clearEvent(4), copyEvent(5, true) })
        {
            UNSCOPED_INFO(error);
            REQUIRE(commands.Consume(event, {}, {}, error));
        }
        auto terminal             = terminalEvent(6);
        auto swap                 = snapshotWithEvents({ terminal }).final_swap;
        swap.gamma.gamma_256[127] = 0x12345678;
        if (prepared)
        {
            terminal.success          = false;
            terminal.execution_status = DrawCaptureExecutionStatus::kUnfinished;
        }
        UNSCOPED_INFO(error);
        REQUIRE((prepared ? commands.FinishPrepared(terminal, swap, 2, 1, frames[prepared], banks[prepared], error) : commands.Finish(terminal, swap, 2, 1, frames[prepared], banks[prepared], error)));
        auto repeated     = frames[prepared];
        auto repeatedBank = banks[prepared];
        UNSCOPED_INFO("terminal admission consumes the recording owner exactly once and clears repeated outputs");
        REQUIRE((!commands.FinishPrepared(terminal, swap, 2, 1, repeated, repeatedBank, error) && repeated.passes.empty() && !repeated.outputTextureId && repeatedBank.empty()));
    }
    UNSCOPED_INFO("prepared and legacy terminals retain the same complete output graph, gamma and bank descriptors");
    REQUIRE((frames[0].passes.size() == frames[1].passes.size() && frames[0].textures.size() == frames[1].textures.size() &&
             frames[0].outputTextureId == frames[1].outputTextureId && frames[0].outputSwizzle == frames[1].outputSwizzle &&
             frames[0].gammaTable == frames[1].gammaTable && banks[0].size() == banks[1].size()));
    for (std::size_t index = 0; index < frames[0].passes.size(); ++index)
    {
        const auto& issued   = frames[0].passes[index];
        const auto& prepared = frames[1].passes[index];
        UNSCOPED_INFO("prepared terminal preserves every clear and resolve operation in order");
        REQUIRE((issued.target == prepared.target && issued.clears.size() == prepared.clears.size() && issued.resolves.size() == prepared.resolves.size()));
        for (std::size_t resolve = 0; resolve < issued.resolves.size(); ++resolve)
        {
            UNSCOPED_INFO("prepared terminal preserves resolve resources and coverage");
            REQUIRE((issued.resolves[resolve].textureId == prepared.resolves[resolve].textureId && issued.resolves[resolve].width == prepared.resolves[resolve].width &&
                     issued.resolves[resolve].height == prepared.resolves[resolve].height && issued.resolves[resolve].destinationX == prepared.resolves[resolve].destinationX));
        }
    }
    for (std::size_t index = 0; index < banks[0].size(); ++index)
    {
        UNSCOPED_INFO("prepared candidate bank matches legacy descriptors");
        REQUIRE((banks[0][index].guestBasePage == banks[1][index].guestBasePage && banks[0][index].texture.width == banks[1][index].texture.width &&
                 banks[0][index].texture.height == banks[1][index].texture.height && banks[0][index].texture.format == banks[1][index].texture.format));
    }
}

void preparedTerminalFailures()
{
    using namespace rex::graphics::diagnostic;
    for (unsigned invalid = 0; invalid != 15; ++invalid)
    {
        std::string               error;
        NativeGameplayCommandFeed commands(std::span<const NativeGameplayShaderInterface>{});
        UNSCOPED_INFO(error);
        REQUIRE((commands.Begin(1, 1, {}, error) && commands.Consume(clearEvent(1), {}, {}, error) && commands.Consume(copyEvent(2), {}, {}, error) &&
                 commands.Consume(clearEvent(3), {}, {}, error)));
        if (invalid != 9)
        {
            UNSCOPED_INFO(error);
            REQUIRE(commands.Consume(copyEvent(4, true), {}, {}, error));
        }
        auto terminal             = terminalEvent(5);
        terminal.success          = false;
        terminal.execution_status = DrawCaptureExecutionStatus::kUnfinished;
        auto swap                 = snapshotWithEvents({ terminal }).final_swap;
        switch (invalid)
        {
            case 0:
                terminal.success = true;
                break;
            case 1:
                terminal.host_issued = true;
                break;
            case 2:
                terminal.execution_status = DrawCaptureExecutionStatus::kIssued;
                break;
            case 3:
                terminal.execution_reason = DrawCaptureReason::kExecutionFailure;
                break;
            case 4:
                terminal.kind = DrawCaptureFrameEvent::Kind::kCopy;
                break;
            case 5:
                terminal.id = 4;
                break;
            case 6:
                terminal.draw.draw.registers.pop_back();
                break;
            case 7:
                terminal.draw.draw.registers[0x4801] ^= 1;
                break;
            case 8:
                swap.gamma.gamma_256.pop_back();
                break;
            case 9:
                break;
            case 10:
                terminal.draw.draw.registers[0x4803] = 0xFFFFFFFF;
                break;
            case 11:
                terminal.acquisition = DrawCaptureAvailability::kUnavailable;
                break;
            case 12:
                swap.output_width = 640;
                break;
            case 13:
                swap.gamma.use_fxaa = true;
                break;
            case 14:
                swap.output_format = 6;
                break;
        }
        NativeRenderFrame frame;
        frame.outputTextureId = 99;
        frame.passes.resize(1);
        std::vector<NativeGameplayRetainedTexture> next(1);
        UNSCOPED_INFO("prepared terminal failures clear prior outputs and candidate banks without relaxing graph checks");
        REQUIRE((!commands.FinishPrepared(terminal, swap, 2, 1, frame, next, error) && !error.empty() && !frame.outputTextureId && frame.passes.empty() && next.empty()));
    }
    for (unsigned invalid = 0; invalid != 6; ++invalid)
    {
        std::string               error;
        NativeGameplayCommandFeed commands(std::span<const NativeGameplayShaderInterface>{});
        UNSCOPED_INFO(error);
        REQUIRE(commands.Begin(1, 1, {}, error));
        auto event             = snapshotEvent(1, DrawCaptureFrameEvent::Kind::kSwap);
        event.success          = false;
        event.execution_status = DrawCaptureExecutionStatus::kUnfinished;
        if (invalid == 0)
            event.success = true;
        if (invalid == 1)
            event.host_issued = true;
        if (invalid == 2)
            event.execution_status = DrawCaptureExecutionStatus::kIssued;
        if (invalid == 3)
            event.kind = DrawCaptureFrameEvent::Kind::kDraw;
        if (invalid == 4)
            event.acquisition_reason = DrawCaptureReason::kTextureSource;
        if (invalid == 5)
        {
            UNSCOPED_INFO(error);
            REQUIRE(commands.ConsumePreparedBoundary(event, event.draw.draw.registers, error));
        }
        UNSCOPED_INFO("prepared leading boundary rejects forged provenance, wrong kinds and duplicates");
        REQUIRE(!commands.ConsumePreparedBoundary(event, event.draw.draw.registers, error));
    }
    std::string               error;
    NativeGameplayCommandFeed legacy(std::span<const NativeGameplayShaderInterface>{});
    auto                      terminal = terminalEvent(1);
    terminal.success                   = false;
    terminal.execution_status          = DrawCaptureExecutionStatus::kUnfinished;
    NativeRenderFrame                          frame;
    std::vector<NativeGameplayRetainedTexture> next;
    UNSCOPED_INFO("legacy terminal admission retains original completion requirements");
    REQUIRE((legacy.Begin(1, 1, {}, error) && !legacy.Finish(terminal, snapshotWithEvents({ terminal }).final_swap, 2, 1, frame, next, error)));
}

NativeRenderFrame clearPacket(NativeRenderFramePass::Target target, bool preserve = false)
{
    NativeRenderFrame packet;
    packet.passes.resize(1);
    auto& pass                       = packet.passes.front();
    pass.target                      = target;
    pass.preservePreviousAttachments = preserve;
    pass.clears                      = { { { 0, 0, static_cast<std::int32_t>(target.width), static_cast<std::int32_t>(target.height) }, true } };
    return packet;
}

void appendTargetChanges()
{
    std::string                                        error;
    const std::array<NativeRenderFramePass::Target, 6> targets{ {
        { 160, 32, 1, NativeDrawReplayTargetFormat::Rgba8 },
        { 320, 32, 1, NativeDrawReplayTargetFormat::Rgba8 },
        { 320, 64, 1, NativeDrawReplayTargetFormat::Rgba8 },
        { 320, 64, 4, NativeDrawReplayTargetFormat::Rgba8 },
        { 320, 64, 4, NativeDrawReplayTargetFormat::Rgb10a2 },
        { 160, 32, 1, NativeDrawReplayTargetFormat::Rgba8 },
    } };
    NativeRenderFramePrefixState                       state;
    NativeRenderFrame                                  chain;
    for (const auto target : targets)
    {
        auto fresh     = clearPacket(target);
        auto preserved = clearPacket(target, true);
        UNSCOPED_INFO("fresh target changes and exact adjacent preservation remain cumulative: " + error);
        REQUIRE((state.Append(fresh, error) && state.Append(preserved, error)));
        chain.passes.push_back(fresh.passes.front());
        chain.passes.push_back(preserved.passes.front());
    }
    NativeRenderFramePrefixState packetState;
    UNSCOPED_INFO("one packet may contain multiple sealed unsigned layouts: " + error);
    REQUIRE(packetState.Append(chain, error));
    UNSCOPED_INFO("standalone prefix validation retains one compatible chain");
    REQUIRE(!ValidateNativeRenderFramePrefix(chain, error));
    auto legacy = clearPacket(targets.front());
    legacy.passes.push_back(clearPacket(targets.front(), true).passes.front());
    UNSCOPED_INFO(error);
    REQUIRE(ValidateNativeRenderFramePrefix(legacy, error));
    legacy.passes.back().preservePreviousAttachments = false;
    UNSCOPED_INFO("standalone prefix batches still require explicit preservation");
    REQUIRE((!ValidateNativeRenderFramePrefix(legacy, error) && error.find("preserve") != std::string::npos));

    for (std::size_t index = 1; index < targets.size() - 1; ++index)
    {
        NativeRenderFramePrefixState incompatible;
        UNSCOPED_INFO(error);
        REQUIRE(incompatible.Append(clearPacket(targets[index - 1]), error));
        UNSCOPED_INFO("preservation rejects every changed target field");
        REQUIRE((!incompatible.Append(clearPacket(targets[index], true), error) && error.find("incompatible") != std::string::npos));
        UNSCOPED_INFO("a rejected transition permanently ends cumulative admission");
        REQUIRE((!incompatible.Append(clearPacket(targets[index]), error) && error.find("already failed") != std::string::npos));
    }

    auto initialized                                = clearPacket(targets.front());
    initialized.passes.front().clears.front().depth = true;
    NativeRenderFrame depth;
    depth.textures = { { 1, 160, 32, NativeDrawReplayTextureFormat::R32Float } };
    depth.passes.resize(1);
    depth.passes.front().target                      = targets.front();
    depth.passes.front().preservePreviousAttachments = true;
    depth.passes.front().resolves                    = { { 1, true, 0, 0, 0, 160, 32 } };
    NativeRenderFramePrefixState keptDepth;
    UNSCOPED_INFO("exact preservation carries initialized depth across packets: " + error);
    REQUIRE((keptDepth.Append(initialized, error) && keptDepth.Append(depth, error)));
    NativeRenderFramePrefixState resetDepth;
    UNSCOPED_INFO(error);
    REQUIRE((resetDepth.Append(initialized, error) && resetDepth.Append(clearPacket(targets[1]), error) &&
             resetDepth.Append(clearPacket(targets.front()), error)));
    UNSCOPED_INFO("returning to an earlier layout never restores stale depth initialization");
    REQUIRE((!resetDepth.Append(depth, error) && error.find("resolve rectangle") != std::string::npos));

    auto partial                    = clearPacket(targets.front());
    partial.textures                = { { 1, 160, 32, NativeDrawReplayTextureFormat::Rgba8 } };
    partial.passes.front().resolves = { { 1, false, 0, 0, 0, 80, 32 } };
    NativeRenderFrame sampled;
    auto              recipe          = draw(320, 64, 4, 9);
    recipe.textureMask                = 1;
    recipe.textures[0]                = { {}, 160, 32, NativeDrawReplayTextureFormat::Rgba8 };
    recipe.textures[0].frameTextureId = 1;
    sampled.passes                    = { { false, { recipe }, {} } };
    NativeRenderFramePrefixState incomplete;
    UNSCOPED_INFO(error);
    REQUIRE(incomplete.Append(partial, error));
    UNSCOPED_INFO("a target change cannot hide incomplete earlier resolve strips");
    REQUIRE((!incomplete.Append(sampled, error) && error.find("incompletely resolved") != std::string::npos));
    NativeRenderFramePrefixState resolved;
    UNSCOPED_INFO(error);
    REQUIRE(resolved.Append(partial, error));
    auto remainder                    = clearPacket(targets.front(), true);
    remainder.passes.front().resolves = { { 1, false, 0, 80, 0, 80, 32, 80 } };
    UNSCOPED_INFO("a changed target may sample an exact completely resolved earlier resource: " + error);
    REQUIRE((resolved.Append(remainder, error) && resolved.Append(sampled, error)));
    auto rgb10                        = clearPacket(targets[4]);
    rgb10.textures                    = { { 2, 320, 64, NativeDrawReplayTextureFormat::Rgb10a2 } };
    rgb10.passes.front().resolves     = { { 2, false, 0, 0, 0, 320, 64, 0, { 0, 3 } } };
    recipe                            = draw(160, 32, 1, 1);
    recipe.textureMask                = 1;
    recipe.textures[0]                = { {}, 320, 64, NativeDrawReplayTextureFormat::Rgb10a2 };
    recipe.textures[0].frameTextureId = 2;
    sampled.passes                    = { { false, { recipe }, {} } };
    UNSCOPED_INFO("format and sample transitions preserve previously resolved texture descriptors: " + error);
    REQUIRE((resolved.Append(rgb10, error) && resolved.Append(sampled, error)));
    auto changedDescriptor     = clearPacket(targets.front());
    changedDescriptor.textures = { { 1, 80, 32, NativeDrawReplayTextureFormat::Rgba8 } };
    UNSCOPED_INFO("a target transition cannot redefine an earlier resource identity");
    REQUIRE((!resolved.Append(changedDescriptor, error) && error.find("changed a recorded resource descriptor") != std::string::npos));

    NativeRenderFramePrefixState imports;
    auto                         imported = clearPacket(targets.front());
    imported.textures                     = { { 1, 160, 32, NativeDrawReplayTextureFormat::Rgba8, 99, true } };
    UNSCOPED_INFO(error);
    REQUIRE(imports.Append(imported, error));
    auto rewritten                    = clearPacket(targets[1]);
    rewritten.passes.front().resolves = { { 1, false, 0, 0, 0, 160, 32 } };
    UNSCOPED_INFO("imported native versions remain immutable across target changes");
    REQUIRE((!imports.Append(rewritten, error) && error.find("immutable") != std::string::npos));

    for (const bool alias : { false, true })
    {
        NativeRenderFramePrefixState chain;
        UNSCOPED_INFO(error);
        REQUIRE(chain.Append(clearPacket(targets.front()), error));
        auto next = clearPacket(targets[1]);
        if (alias)
            next.passes.front().colorSourcePass = 1;
        else
            next.passes.front().target.format = NativeDrawReplayTargetFormat::Rg16Snorm;
        if (alias)
        {
            UNSCOPED_INFO("raw color aliases remain terminal-only");
            REQUIRE((!chain.Append(next, error) && error.find("without color aliasing") != std::string::npos));
        }
        else
        {
            UNSCOPED_INFO("a k_16_16 target appends to a chain: " + error);
            REQUIRE(chain.Append(next, error));
        }
    }
    NativeRenderFramePrefixState output;
    auto                         presented = clearPacket(targets.front());
    presented.returnGpuOutput              = true;
    UNSCOPED_INFO("the existing GPU-output hint admits a prefix without terminal publication: " + error);
    REQUIRE(output.Append(presented, error));
}

DrawCaptureFrameEvent targetDrawEvent(std::uint64_t id, std::uint32_t surface, NativeRenderFramePass::Target target, std::uint32_t colorBase)
{
    auto  event = nonIndexedDraw(id);
    auto& r     = event.draw.draw.registers;
    r[0x2000]   = surface;
    r[0x2001]   = colorBase | (target.format == NativeDrawReplayTargetFormat::Rgb10a2 ? 2U << 16 : 0);
    r[0x2002]   = 0x530;
    r[0x200F]   = 0x20002000;
    r[0x2082]   = target.width | (target.height << 16);
    r[0x2100]   = 0xFFFFFF;
    r[0x2104]   = 0xF;
    r[0x210F] = r[0x2110]        = std::bit_cast<std::uint32_t>(target.width / 2.0F);
    r[0x2111]                    = std::bit_cast<std::uint32_t>(-static_cast<float>(target.height) / 2.0F);
    r[0x2112]                    = std::bit_cast<std::uint32_t>(target.height / 2.0F);
    r[0x2201]                    = 0x00010001;
    r[0x2202]                    = 0x87000000;
    r[0x2204]                    = 0x80000;
    r[0x2205]                    = 0x18002;
    r[0x2206]                    = 0x43F;
    r[0x2208]                    = 4;
    r[0x2302]                    = 4;
    r[0x2307]                    = 0xFF000;
    r[0x2308]                    = 0xFF100;
    event.draw.draw.vertex_ucode = event.draw.draw.pixel_ucode = { 0 };
    rex::graphics::diagnostic::DrawCaptureVertexFetch fetch;
    fetch.base         = 0x2000;
    fetch.availability = rex::graphics::diagnostic::DrawCaptureAvailability::kComplete;
    fetch.bytes.resize(3 * 12);
    r[0x4800] = fetch.base | 3U;
    r[0x4801] = static_cast<std::uint32_t>(fetch.bytes.size()) | 2U;
    event.draw.draw.vertex_fetches.push_back(std::move(fetch));
    return event;
}

NativeGameplayShaderInterface targetShader(std::uint32_t sampleCount, std::uint32_t colorFormat)
{
    auto       shader = syntheticShader();
    const auto digest = [](const std::vector<std::uint8_t>& bytes)
    {
        return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    };
    shader.vertexShaderDxil      = { 'D', 'X', 'B', 'C' };
    shader.pixelShaderDxil       = { 'P', 'S', 'D', 'X' };
    shader.vertexGuestWordDigest = shader.pixelGuestWordDigest = digest({ 0, 0, 0, 0 });
    shader.vertexDxilDigest                                    = digest(shader.vertexShaderDxil);
    shader.pixelDxilDigest                                     = digest(shader.pixelShaderDxil);
    shader.vertexAttributes                                    = { { 0, 12, 0, 37, false, false } };
    shader.rasterizedDepth = shader.independentSamplers = true;
    shader.sampleCount                                  = sampleCount;
    shader.sampleMask                                   = sampleCount == 4 ? 9 : 1;
    shader.colorTargetFormat                            = colorFormat;
    return shader;
}

// Presented depth work names its surface; preserves keep both attachments.
void depthSurfaceIdentity()
{
    std::string                         error;
    const NativeRenderFramePass::Target presented{ 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0, 0x530 };
    auto                                unnamed = presented;
    unnamed.depthEdramBase                      = kNoDepthEdramBase;
    const auto depthClear                       = [](NativeRenderFramePass::Target target, bool preserve = false)
    {
        auto  packet = clearPacket(target, preserve);
        auto& clear  = packet.passes.front().clears.front();
        clear.color  = false;
        clear.depth = clear.stencil     = true;
        packet.passes.front().presented = true;
        return packet;
    };
    UNSCOPED_INFO("a presented depth clear names its depth surface: " + error);
    REQUIRE(ValidateNativeRenderFramePrefix(depthClear(presented), error));
    UNSCOPED_INFO("a presented depth clear without a depth surface is rejected");
    REQUIRE((!ValidateNativeRenderFramePrefix(depthClear(unnamed), error) && error.find("depth surface identity") != std::string::npos));
    auto colorOnly                     = clearPacket(unnamed);
    colorOnly.passes.front().presented = true;
    auto offline                       = depthClear(unnamed);
    offline.passes.front().presented   = false;
    UNSCOPED_INFO("presented color work and offline depth work need no depth identity: " + error);
    REQUIRE((ValidateNativeRenderFramePrefix(colorOnly, error) && ValidateNativeRenderFramePrefix(offline, error)));
    auto other           = presented;
    other.depthEdramBase = 0x400;
    auto chain           = depthClear(presented);
    chain.passes.push_back(depthClear(other, true).passes.front());
    UNSCOPED_INFO("a preserve onto another depth surface is rejected");
    REQUIRE((!ValidateNativeRenderFramePrefix(chain, error) && error.find("incompatible") != std::string::npos));

    auto  batched = depthClear(presented);
    auto& pass    = batched.passes.front();
    pass.clears.resize(NativeRenderBatchCommands + 1, pass.clears.front());
    UNSCOPED_INFO("every batch of a presented pass stays presented on the same depth surface: " + error);
    REQUIRE((BatchNativeRenderFrame(batched, error) && batched.passes.size() == 2 && batched.passes[0].presented && batched.passes[1].presented &&
             batched.passes[1].preservePreviousAttachments && batched.passes[1].target == presented));
}

void preparedCollectorResources()
{
    using namespace rex::graphics::diagnostic;
    using rerevved::gpu::diagnostics::NativeGameplayLiveInputs;
    std::array<NativeDrawReplayRecipe, 2> recipes;
    std::array<NativeRenderFrame, 2>      frames;
    auto                                  shader = targetShader(4, 0);
    shader.textureFetchSlots                     = { 1 };
    shader.textureDimensions                     = { NativeDrawReplayTextureDimension::Texture2D };
    const std::vector<NativeGameplayShaderInterface> shaders{ shader };
    for (const bool prepared : { false, true })
    {
        auto               source = targetDrawEvent(2, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0);
        NativeTextureFetch fetch{ 2U | (1U << 22), 0x00100006, 0, 0xD10, 0, 0x200 };
        std::copy(fetch.begin(), fetch.end(), source.draw.draw.registers.begin() + 0x4806);

        struct Memory
        {
            std::vector<std::uint8_t> vertex, rectangle;
            std::uint8_t              pixel = 37;

            static bool copy(void* context, std::uint32_t address, std::span<std::uint8_t> destination) noexcept
            {
                const auto& self  = *static_cast<Memory*>(context);
                const auto* bytes = address == 0x2000 ? &self.vertex : address == 0x300000 ? &self.rectangle
                                                                                           : nullptr;
                if (bytes)
                {
                    if (destination.size() != bytes->size())
                        return false;
                    std::ranges::copy(*bytes, destination.begin());
                    return true;
                }
                if (address != 0x100000)
                    return false;
                std::ranges::fill(destination, self.pixel);
                return true;
            }
        } memory{ source.draw.draw.vertex_fetches[0].bytes, {} };

        NativeTextureMemoryRanges ranges;
        std::string               error;
        UNSCOPED_INFO(error);
        REQUIRE(GetNativeTextureMemoryRanges(fetch, ranges, error));
        DrawCaptureFrameInputTextureView texture;
        texture.fetch_words  = fetch;
        texture.base_address = ranges.base.address;
        texture.base_size    = ranges.base.size;
        texture.width = texture.height = texture.depth = texture.pitch = 1;
        texture.dimension                                              = 1;
        texture.format                                                 = 6;
        texture.fetch_constant                                         = 1;
        texture.stage                                                  = 1;
        texture.component_mapping                                      = 0x16C0;
        texture.sampler_value                                          = 1U << 11;
        texture.memory                                                 = { &memory, &Memory::copy };
        DrawCaptureFrameInputVertexFetchView vertex{ 0, 0x2000, static_cast<std::uint32_t>(memory.vertex.size()), { &memory, &Memory::copy } };
        DrawCaptureFrameInputDrawView        draw;
        draw.id    = 2;
        draw.frame = draw.submission = 1;
        draw.registers               = source.draw.draw.registers;
        draw.vertex_shader           = { true, source.draw.guest_vertex_shader_hash, source.draw.draw.vertex_ucode };
        draw.pixel_shader            = { true, source.draw.guest_pixel_shader_hash, source.draw.draw.pixel_ucode };
        draw.primitive_type          = source.draw.draw.primitive_type;
        draw.requested_index_count   = source.draw.draw.requested_index_count;
        draw.vertex_fetches          = { &vertex, 1 };
        draw.textures                = { &texture, 1 };
        draw.original_host_issued    = !prepared;
        draw.execution_status        = prepared ? DrawCaptureExecutionStatus::kUnfinished : DrawCaptureExecutionStatus::kIssued;
        std::vector<std::uint32_t>    gamma(256);
        DrawCaptureFrameInputSwapView boundary;
        boundary.id = boundary.frame = boundary.submission = 1;
        boundary.registers                                 = source.draw.draw.registers;
        boundary.gamma_256                                 = gamma;
        boundary.is_8bpc                                   = true;
        NativeGameplayGeometryCache      cache(preparation::Budget);
        NativeGameplayPreparationContext context;
        NativeGameplayCommandFeed        commands(shaders, &cache, context);
        NativeGameplayLiveInputs         inputs;
        UNSCOPED_INFO(error);
        REQUIRE(commands.Begin(1, 1, {}, error));
        inputs.SetEventConsumer([&](const auto& event, const auto& bank, const auto& textures, const auto& cpu, std::string& failure)
                                {
                                    if (!prepared || event.kind == DrawCaptureFrameEvent::Kind::kSwap)
                                        return commands.Consume(event, bank, textures, cpu, failure);
                                    return event.kind == DrawCaptureFrameEvent::Kind::kCopy ? commands.ConsumePreparedCopy(event, bank, textures, cpu, failure) : commands.ConsumePreparedDraw(event, bank, textures, cpu, failure);
                                });
        UNSCOPED_INFO(error);
        REQUIRE((inputs.Begin(boundary, error) && (prepared ? inputs.PreparedDraw(draw, error) : inputs.Draw(draw, error))));
        UNSCOPED_INFO("collector owns a CPU textured native draw");
        REQUIRE((commands.Draw(2) && commands.Draw(2)->admittedRecipe));
        recipes[prepared]  = commands.Draw(2)->Recipe();
        const auto version = recipes[prepared].textures[0].textureVersion;
        memory.pixel       = 99;
        ++draw.id;
        UNSCOPED_INFO("CPU overwrite supersedes current texture bytes while prior native recipes remain immutable");
        REQUIRE(((prepared ? inputs.PreparedDraw(draw, error) : inputs.Draw(draw, error)) &&
                 commands.Draw(3)->Recipe().textures[0].payloadBytes().front() == 99 && version->Payload()->front() == 37));
        auto copySource = copyEvent(4);
        copySource.draw.draw.registers[0x2318] &= ~0x300U;
        copyRectangle(copySource, 0, 0, 32, 32);
        memory.rectangle = copySource.draw.draw.vertex_fetches[0].bytes;
        DrawCaptureFrameInputCopyView copy;
        copy.id    = 4;
        copy.frame = copy.submission = 1;
        copy.registers               = copySource.draw.draw.registers;
        copy.source_geometry         = { 0, 0x300000, 24, { &memory, &Memory::copy } };
        copy.source_rect_present     = true;
        copy.source_width = copy.source_height = 32;
        copy.cpu_data_complete                 = true;
        copy.planned_address                   = 0x100000;
        copy.planned_length                    = 4096;
        copy.original_succeeded                = !prepared;
        copy.written_address                   = prepared ? 0 : copy.planned_address;
        copy.written_length                    = prepared ? 0 : copy.planned_length;
        UNSCOPED_INFO(error);
        REQUIRE((prepared ? inputs.PreparedCopy(copy, error) : inputs.Copy(copy, true, error)));
        auto nativeCopy = *commands.Copy(4);
        std::ranges::fill(memory.vertex, 0xEE);
        std::ranges::fill(memory.rectangle, 0xEE);
        std::ranges::fill(copySource.draw.draw.registers, 0xEE);
        NativeRenderFrameResolve resolve;
        UNSCOPED_INFO("borrowed copy mutation cannot change admitted native geometry");
        REQUIRE((DeriveNativeGameplayResolve(nativeCopy, 0, resolve, error) && resolve.width == 32 && resolve.height == 32));
        const auto owned = inputs.Snapshot().events.back();
        UNSCOPED_INFO("copy journal preserves actual versus prepared execution provenance");
        REQUIRE((owned.success == !prepared && !owned.host_issued && owned.execution_status == (prepared ? DrawCaptureExecutionStatus::kUnfinished : DrawCaptureExecutionStatus::kIssued)));
        UNSCOPED_INFO(error);
        REQUIRE(commands.FinishProducer(0x100, 4, frames[prepared], error));
    }
    UNSCOPED_INFO("prepared and issued collector paths preserve exact texture, sampler, geometry and constants");
    REQUIRE((std::ranges::equal(recipes[0].vertexBytes(), recipes[1].vertexBytes()) && recipes[0].indices == recipes[1].indices &&
             recipes[0].vertexConstants == recipes[1].vertexConstants && recipes[0].pixelConstants == recipes[1].pixelConstants &&
             recipes[0].sharedConstants == recipes[1].sharedConstants && recipes[0].textures[0].swizzle == recipes[1].textures[0].swizzle &&
             recipes[0].textures[0].textureVersion->Layout() == recipes[1].textures[0].textureVersion->Layout() &&
             std::ranges::equal(recipes[0].textures[0].payloadBytes(), recipes[1].textures[0].payloadBytes()) &&
             recipes[0].samplers.size() == 1 && recipes[1].samplers.size() == 1 &&
             recipes[0].samplers[0].magLinear == recipes[1].samplers[0].magLinear &&
             recipes[0].samplers[0].minLinear == recipes[1].samplers[0].minLinear &&
             recipes[0].samplers[0].mipLinear == recipes[1].samplers[0].mipLinear &&
             recipes[0].samplers[0].anisotropy == recipes[1].samplers[0].anisotropy &&
             recipes[0].samplers[0].address == recipes[1].samplers[0].address &&
             recipes[0].samplers[0].border == recipes[1].samplers[0].border &&
             recipes[0].samplers[0].minLod == recipes[1].samplers[0].minLod &&
             recipes[0].samplers[0].maxLod == recipes[1].samplers[0].maxLod &&
             recipes[0].samplers[0].mipBias == recipes[1].samplers[0].mipBias));
    UNSCOPED_INFO("prepared and issued collector resolves retain native pass ordering and extents");
    REQUIRE((frames[0].passes.size() == frames[1].passes.size() && frames[1].passes.back().resolves.size() == 1 &&
             frames[0].passes.back().resolves[0].width == frames[1].passes.back().resolves[0].width &&
             frames[0].passes.back().resolves[0].height == frames[1].passes.back().resolves[0].height));
}

void preparedResourceRejection()
{
    using namespace rex::graphics::diagnostic;
    auto event             = copyEvent(2);
    event.success          = false;
    event.execution_status = DrawCaptureExecutionStatus::kUnfinished;
    event.draw.draw.registers[0x2318] &= ~0x300U;
    copyRectangle(event, 0, 0, 32, 32);
    std::string error;
    for (int invalid = 0; invalid != 17; ++invalid)
    {
        NativeGameplayCommandFeed commands(std::span<const NativeGameplayShaderInterface>{});
        std::size_t               prefixes = 0;
        commands.SetPrefixConsumer([&](auto, std::string&)
                                   {
                                       ++prefixes;
                                       return true;
                                   });
        UNSCOPED_INFO(error);
        REQUIRE((commands.Begin(1, 1, {}, error) && commands.Consume(clearEvent(1), {}, {}, error)));
        auto candidate = event;
        switch (invalid)
        {
            case 0:
                candidate.success = true;
                break;
            case 1:
                candidate.host_issued = true;
                break;
            case 2:
                candidate.execution_status = DrawCaptureExecutionStatus::kIssued;
                break;
            case 3:
                candidate.execution_reason = DrawCaptureReason::kExecutionFailure;
                break;
            case 4:
                candidate.acquisition = DrawCaptureAvailability::kPartial;
                break;
            case 5:
                candidate.kind = DrawCaptureFrameEvent::Kind::kDraw;
                break;
            case 6:
                candidate.copy_mode = "clear";
                break;
            case 7:
                candidate.draw.draw.vertex_fetches.push_back(candidate.draw.draw.vertex_fetches[0]);
                break;
            case 8:
                candidate.draw.draw.vertex_fetches[0].bytes.resize(20);
                break;
            case 9:
                candidate.draw.draw.registers[0x2318] |= 0x100U;
                break;
            case 10:
                candidate.draw.draw.registers[0x2319] = 0;
                break;
            case 11:
                candidate.draw.draw.vertex_fetches[0].bytes.assign(24, 0xFF);
                break;
            case 12:
                candidate.draw.draw.registers[0x231B] = (candidate.draw.draw.registers[0x231B] & ~(0x3FU << 7)) | (0x3FU << 7);
                break;
            case 13:
            case 14:
            case 15:
                candidate.draw.draw.registers[0x2318] = (candidate.draw.draw.registers[0x2318] & ~7U) | (invalid - 12);
                break;
            case 16:
                candidate.draw.draw.registers[0x231B] |= 1U << 3;
                break;
        }
        const bool accepted = commands.ConsumePreparedCopy(candidate, {}, {}, error);
        UNSCOPED_INFO("forged or malformed prepared copy variant " + std::to_string(invalid) + " cannot emit its pending prefix: " + error);
        REQUIRE((!accepted && !prefixes && !commands.Copy(2)));
        NativeRenderFrame                          frame;
        std::vector<NativeGameplayRetainedTexture> next;
        const auto                                 terminal = terminalEvent(3);
        UNSCOPED_INFO("rejected prepared copy cannot emit terminal output");
        REQUIRE((!commands.Finish(terminal, snapshotWithEvents({ terminal }).final_swap, 2, 1, frame, next, error) &&
                 frame.passes.empty() && next.empty()));
    }
    NativeGameplayCommandFeed wrong(std::span<const NativeGameplayShaderInterface>{});
    UNSCOPED_INFO("prepared draw entry rejects a prepared copy kind");
    REQUIRE((wrong.Begin(1, 1, {}, error) && !wrong.ConsumePreparedDraw(event, {}, {}, error)));
    auto shader              = targetShader(4, 0);
    shader.textureFetchSlots = { 1 };
    shader.textureDimensions = { NativeDrawReplayTextureDimension::Texture2D };
    std::vector<DrawCaptureTexture>                  sources(1);
    std::map<std::uint32_t, NativeDrawReplayTexture> cpu;
    cpu[0].width = cpu[0].height = 1;
    cpu[0].bytes                 = { 1, 2, 3, 4 };
    for (int invalid = 0; invalid != 7; ++invalid)
    {
        NativeGameplayCommandFeed commands(std::span(&shader, 1));
        std::size_t               prefixes = 0;
        commands.SetPrefixConsumer([&](auto, std::string&)
                                   {
                                       ++prefixes;
                                       return true;
                                   });
        UNSCOPED_INFO(error);
        REQUIRE((commands.Begin(1, 1, {}, error) && commands.Consume(clearEvent(1), {}, {}, error)));
        auto candidate                            = targetDrawEvent(2, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0);
        candidate.draw.draw.registers[0x4806]     = 2;
        candidate.draw.draw.registers[0x4806 + 5] = 1U << 9;
        candidate.success = candidate.host_issued = false;
        candidate.execution_status                = DrawCaptureExecutionStatus::kUnfinished;
        DrawCaptureTextureUse use;
        use.texture_index           = 0;
        use.fetch_constant          = 1;
        use.dimension               = 1;
        use.component_mapping       = 0x1688;
        use.data_available          = true;
        use.availability            = DrawCaptureAvailability::kComplete;
        candidate.draw.texture_uses = { use };
        auto owned                  = cpu;
        switch (invalid)
        {
            case 0:
                candidate.draw.texture_uses[0].data_available = false;
                break;
            case 1:
                candidate.draw.texture_uses[0].availability = DrawCaptureAvailability::kUnavailable;
                break;
            case 2:
                owned.clear();
                break;
            case 3:
                candidate.draw.texture_uses[0].dimension = 3;
                break;
            case 4:
                candidate.draw.texture_uses[0].sampler_value = 0xFFFFFFFFU;
                break;
            case 5:
                candidate.draw.texture_uses.push_back(use);
                candidate.draw.texture_uses.back().component_mapping ^= 1;
                break;
            case 6:
                candidate.draw.texture_uses[0].unavailable_reason = DrawCaptureReason::kTextureSource;
                break;
        }
        UNSCOPED_INFO("prepared texture dependencies preserve CPU ownership, dimension, sampler and ambiguity checks");
        REQUIRE((!commands.ConsumePreparedDraw(candidate, sources, owned, error) && !prefixes && !commands.Draw(2)));
    }
    for (int unsupported = 0; unsupported != 4; ++unsupported)
    {
        auto interface    = shader;
        auto candidate    = targetDrawEvent(2, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0);
        candidate.success = candidate.host_issued = false;
        candidate.execution_status                = DrawCaptureExecutionStatus::kUnfinished;
        candidate.draw.draw.registers[0x4806]     = 2;
        candidate.draw.draw.registers[0x4806 + 5] = 1U << 9;
        DrawCaptureTextureUse use;
        use.texture_index           = 0;
        use.fetch_constant          = 1;
        use.dimension               = 1;
        use.component_mapping       = 0x1688;
        use.data_available          = true;
        use.availability            = DrawCaptureAvailability::kComplete;
        candidate.draw.texture_uses = { use };
        auto owned                  = cpu;
        auto metadata               = sources;
        if (unsupported == 0 || unsupported == 3)
        {
            owned[0].dimension = NativeDrawReplayTextureDimension::TextureCube;
            owned[0].bytes.assign(24, 17);
            if (unsupported == 0)
            {
                interface.textureDimensions[0]            = NativeDrawReplayTextureDimension::TextureCube;
                candidate.draw.texture_uses[0].dimension  = 3;
                candidate.draw.draw.registers[0x4806 + 5] = 3U << 9;
                candidate.draw.draw.registers[0x4806 + 2] = 5U << 26;
                metadata[0].guest_depth_or_array_size     = 6;
                NativeGameplayCommandFeed legacy(std::span(&interface, 1));
                auto                      issued = candidate;
                issued.success = issued.host_issued = true;
                issued.execution_status             = DrawCaptureExecutionStatus::kIssued;
                UNSCOPED_INFO("legacy native draw admission retains matching CPU Cube support");
                REQUIRE((legacy.Begin(1, 1, {}, error) && legacy.Consume(issued, metadata, owned, error) && legacy.Draw(2)));
            }
            else
            {
                UNSCOPED_INFO(error);
                REQUIRE(CopyNativeDrawReplayTextureVersion(owned[0], error));
                owned[0].dimension = NativeDrawReplayTextureDimension::Texture2D;
            }
        }
        else
        {
            candidate.draw.draw.registers[0x4806 + 2] = 1U << 26;
            metadata[0].guest_depth_or_array_size     = 2;
            if (unsupported == 1)
                candidate.draw.draw.registers[0x4806 + 1] |= 1U << 10;
        }
        NativeGameplayCommandFeed commands(std::span(&interface, 1));
        std::size_t               prefixes = 0;
        commands.SetPrefixConsumer([&](auto, std::string&)
                                   {
                                       ++prefixes;
                                       return true;
                                   });
        UNSCOPED_INFO(error);
        REQUIRE((commands.Begin(1, 1, {}, error) && commands.Consume(clearEvent(1), {}, {}, error)));
        UNSCOPED_INFO("matching Cube, stacked, layered or mismatched actual CPU owners cannot emit a prepared prefix");
        REQUIRE((!commands.ConsumePreparedDraw(candidate, metadata, owned, error) && !prefixes && !commands.Draw(2)));
        NativeRenderFrame                          frame;
        std::vector<NativeGameplayRetainedTexture> next;
        const auto                                 terminal = terminalEvent(3);
        UNSCOPED_INFO("unsupported prepared texture shapes cannot emit terminal output");
        REQUIRE((!commands.Finish(terminal, snapshotWithEvents({ terminal }).final_swap, 2, 1, frame, next, error) && frame.passes.empty() && next.empty()));
    }
}

void preparedDrawOperation()
{
    using namespace rex::graphics::diagnostic;
    const std::vector<NativeGameplayShaderInterface> shaders{ targetShader(4, 0) };
    std::array<NativeDrawReplayRecipe, 2>            recipes;
    for (const bool prepared : { false, true })
    {
        NativeGameplayGeometryCache      cache(preparation::Budget);
        NativeGameplayPreparationContext context;
        NativeGameplayCommandFeed        commands(shaders, &cache, context);
        auto                             event = targetDrawEvent(1, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0);
        if (prepared)
        {
            event.success = event.host_issued = false;
            event.execution_status            = DrawCaptureExecutionStatus::kUnfinished;
        }
        std::string error;
        UNSCOPED_INFO(error);
        REQUIRE((commands.Begin(1, 1, {}, error) &&
                 (prepared ? commands.ConsumePreparedDraw(event, {}, {}, error) : commands.Consume(event, {}, {}, error))));
        const auto* operation = commands.Draw(1);
        UNSCOPED_INFO("prepared draws receive canonical native operation ownership");
        REQUIRE((operation && operation->admittedRecipe && operation->Recipe().textureMask == 0));
        recipes[prepared] = operation->Recipe();
        std::fill(event.draw.draw.vertex_fetches[0].bytes.begin(), event.draw.draw.vertex_fetches[0].bytes.end(), 0xFF);
        event.draw.draw.registers.clear();
        event.draw.draw.vertex_ucode.clear();
        UNSCOPED_INFO("prepared operation owns geometry after callback mutation");
        REQUIRE(operation->Recipe().vertexBytes()[0] == 0);
    }
    UNSCOPED_INFO("prepared and issued texture-free draws produce identical native geometry and constants");
    REQUIRE((recipes[0].indices == recipes[1].indices && std::ranges::equal(recipes[0].vertexBytes(), recipes[1].vertexBytes()) &&
             recipes[0].vertexConstants == recipes[1].vertexConstants && recipes[0].pixelConstants == recipes[1].pixelConstants &&
             recipes[0].sharedConstants == recipes[1].sharedConstants &&
             recipes[0].viewport.x == recipes[1].viewport.x && recipes[0].viewport.y == recipes[1].viewport.y &&
             recipes[0].viewport.width == recipes[1].viewport.width && recipes[0].viewport.height == recipes[1].viewport.height &&
             recipes[0].viewport.minDepth == recipes[1].viewport.minDepth && recipes[0].viewport.maxDepth == recipes[1].viewport.maxDepth &&
             recipes[0].scissor.left == recipes[1].scissor.left && recipes[0].scissor.top == recipes[1].scissor.top &&
             recipes[0].scissor.right == recipes[1].scissor.right && recipes[0].scissor.bottom == recipes[1].scissor.bottom));
    auto textured                 = shaders;
    textured[0].textureFetchSlots = { 1 };
    NativeGameplayCommandFeed rejected(textured);
    auto                      event = targetDrawEvent(1, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0);
    event.success = event.host_issued = false;
    event.execution_status            = DrawCaptureExecutionStatus::kUnfinished;
    std::string error;
    UNSCOPED_INFO("prepared shader dependencies cannot bypass texture admission by omitting texture uses");
    REQUIRE((rejected.Begin(1, 1, {}, error) && !rejected.ConsumePreparedDraw(event, {}, {}, error) &&
             error.find("required texture binding") != std::string::npos));
}

void preparedDrawFunding()
{
    using namespace std::chrono_literals;
    using namespace rex::graphics::diagnostic;
    if (!preparation::SupportedAbi)
        return;
    const std::vector<NativeGameplayShaderInterface> shaders{ targetShader(4, 0) };
    NativeGameplayGeometryCache                      cache(preparation::Budget);
    NativeGameplayPreparationContext                 first;
    NativeGameplayCommandFeed                        producer(shaders, &cache, first);
    auto                                             event = targetDrawEvent(1, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0);
    std::string                                      error;
    UNSCOPED_INFO(error);
    REQUIRE((producer.Begin(1, 1, {}, error) && producer.Consume(event, {}, {}, error) &&
             producer.Consume(copyEvent(2), {}, {}, error) && producer.Consume(copyEvent(3, true), {}, {}, error)));
    const auto                                 terminal = terminalEvent(4);
    const auto                                 swap     = snapshotWithEvents({ terminal }).final_swap;
    NativeRenderFrame                          predecessor;
    std::vector<NativeGameplayRetainedTexture> bank;
    UNSCOPED_INFO(error);
    REQUIRE(producer.Finish(terminal, swap, 2, 1, predecessor, bank, error));
    const auto receipt = producer.PreparationReceipt(predecessor, bank);
    UNSCOPED_INFO("prepared funding uses a real completed predecessor receipt");
    REQUIRE(receipt.known);
    event.success = event.host_issued      = false;
    event.execution_status                 = DrawCaptureExecutionStatus::kUnfinished;
    unsigned                         waits = 0;
    NativeGameplayPreparationContext roomy{ receipt, NativeGameplayPreparationStatus::Pending, 0, &waits, [](void* value, bool wait, std::string&)
                                            {
                                                if (!wait)
                                                    return NativeGameplayPreparationStatus::Pending;
                                                ++*static_cast<unsigned*>(value);
                                                return NativeGameplayPreparationStatus::Success;
                                            } };
    NativeGameplayCommandFeed admitted(shaders, &cache, roomy);
    UNSCOPED_INFO("funded prepared draw owns the issued counterpart operation while predecessor Recording remains pending");
    REQUIRE((admitted.Begin(1, 1, {}, error) && admitted.ConsumePreparedDraw(event, {}, {}, error) && admitted.Draw(1) &&
             waits == 0 && roomy.state == NativeGameplayPreparationStatus::Pending &&
             admitted.Draw(1)->Recipe().indices == predecessor.passes[0].Draw(0).indices &&
             std::ranges::equal(admitted.Draw(1)->Recipe().vertexBytes(), predecessor.passes[0].Draw(0).vertexBytes())));

    auto large                            = event;
    large.draw.draw.requested_index_count = 4095;
    large.draw.draw.registers[0x2100]     = 4094;
    large.draw.draw.vertex_fetches[0].bytes.resize(4095 * 12);
    large.draw.draw.registers[0x4801] = static_cast<std::uint32_t>(large.draw.draw.vertex_fetches[0].bytes.size()) | 2U;
    auto issuedEvent                  = large;
    issuedEvent.success = issuedEvent.host_issued = true;
    issuedEvent.execution_status                  = DrawCaptureExecutionStatus::kIssued;
    NativeGameplayCommandFeed issued(shaders);
    UNSCOPED_INFO(error);
    REQUIRE((issued.Begin(1, 1, {}, error) && issued.Consume(issuedEvent, {}, {}, error) && issued.Draw(1)));
    const auto expected = issued.Draw(1)->Recipe();
    for (const auto result : { NativeGameplayPreparationStatus::Success, NativeGameplayPreparationStatus::Failed })
    {
        struct Pressure
        {
            std::promise<void>                waiting, released;
            std::shared_future<void>          release = released.get_future().share();
            std::vector<std::uint8_t>         sidecar;
            NativeGameplayPreparationContext* context = nullptr;
            NativeGameplayPreparationStatus   result;
            std::size_t                       charge = 0;
            unsigned                          waits  = 0;
        } pressure;

        pressure.result                          = result;
        auto                             waiting = pressure.waiting.get_future();
        NativeGameplayPreparationContext context{ receipt, NativeGameplayPreparationStatus::Pending, 0, &pressure, [](void* value, bool wait, std::string&)
                                                  {
                                                      auto& owner = *static_cast<Pressure*>(value);
                                                      if (!wait)
                                                          return NativeGameplayPreparationStatus::Pending;
                                                      ++owner.waits;
                                                      owner.waiting.set_value();
                                                      owner.release.wait();
                                                      if (owner.result == NativeGameplayPreparationStatus::Success)
                                                      {
                                                          std::vector<std::uint8_t>().swap(owner.sidecar);
                                                          owner.context->callerBankBytes -= owner.charge;
                                                      }
                                                      return owner.result;
                                                  } };
        pressure.context = &context;
        NativeGameplayCommandFeed commands(shaders, &cache, context);
        std::size_t               prefixes = 0;
        commands.SetPrefixConsumer([&](auto, std::string&)
                                   {
                                       ++prefixes;
                                       return true;
                                   });
        UNSCOPED_INFO(error);
        REQUIRE(commands.Begin(1, 1, {}, error));
        pressure.sidecar.reserve(preparation::Budget - receipt.bytes - 64U * 1024U);
        pressure.charge = preparation::Vector(pressure.sidecar);
        context.callerBankBytes += pressure.charge;
        auto       consuming = std::async(std::launch::async, [&]
                                          {
                                        return commands.ConsumePreparedDraw(large, {}, {}, error);
                                          });
        const bool held      = waiting.wait_for(2s) == std::future_status::ready && consuming.wait_for(20ms) == std::future_status::timeout &&
                               !commands.Draw(1) && prefixes == 0;
        pressure.released.set_value();
        const bool accepted = consuming.get();
        UNSCOPED_INFO("insufficient prepared headroom waits before admitting an operation or emitting a prefix");
        REQUIRE(held);
        if (result == NativeGameplayPreparationStatus::Success)
        {
            UNSCOPED_INFO("successful retirement refunds only destroyed sidecar storage and admits the exact issued counterpart operation");
            REQUIRE((accepted && pressure.waits == 1 && commands.Draw(1) && pressure.sidecar.capacity() == 0 &&
                     context.receipt.bytes == 0 && context.callerBankBytes == receipt.callerBankBytes &&
                     commands.Draw(1)->Recipe().indices == expected.indices &&
                     std::ranges::equal(commands.Draw(1)->Recipe().vertexBytes(), expected.vertexBytes()) &&
                     commands.Draw(1)->Recipe().sharedConstants == expected.sharedConstants));
        }
        else
        {
            UNSCOPED_INFO("failed predecessor retains its receipt and caller allocation without prepared operation or prefix output");
            REQUIRE((!accepted && pressure.waits == 1 && !commands.Draw(1) && prefixes == 0 &&
                     pressure.sidecar.capacity() > 0 && context.callerBankBytes == pressure.charge && context.receipt.bytes == receipt.bytes));
            NativeRenderFrame                          frame;
            std::vector<NativeGameplayRetainedTexture> next;
            auto                                       preparedTerminal = terminal;
            preparedTerminal.success                                    = false;
            preparedTerminal.execution_status                           = DrawCaptureExecutionStatus::kUnfinished;
            frame.outputTextureId                                       = 99;
            frame.passes.resize(1);
            next.resize(1);
            UNSCOPED_INFO("failed predecessor cannot finalize prepared output");
            REQUIRE((!commands.FinishPrepared(preparedTerminal, swap, 2, 1, frame, next, error) && !frame.outputTextureId && frame.passes.empty() && next.empty()));
        }
    }
    std::cout << "Prepared draw pending funding, pressure and failed retirement passed\n";
}

void preparedCopyFunding()
{
    using namespace rex::graphics::diagnostic;
    if (!preparation::SupportedAbi)
        return;
    NativeGameplayPreparationContext first;
    NativeGameplayCommandFeed        producer(std::span<const NativeGameplayShaderInterface>{}, nullptr, first);
    std::string                      error;
    auto                             copy = copyEvent(2);
    copy.draw.draw.registers[0x2318] &= ~0x300U;
    copyRectangle(copy, 0, 0, 32, 32);
    NativeRenderFrame                          predecessor;
    std::vector<NativeGameplayRetainedTexture> bank;
    const auto                                 terminal = terminalEvent(4);
    const auto                                 swap     = snapshotWithEvents({ terminal }).final_swap;
    UNSCOPED_INFO(error);
    REQUIRE((producer.Begin(1, 1, {}, error) && producer.Consume(clearEvent(1), {}, {}, error) &&
             producer.Consume(copyEvent(2), {}, {}, error) && producer.Consume(copyEvent(3, true), {}, {}, error) &&
             producer.Finish(terminal, swap, 2, 1, predecessor, bank, error)));
    const auto receipt = producer.PreparationReceipt(predecessor, bank);
    UNSCOPED_INFO("prepared copy pressure uses a real completed predecessor receipt and retained bank");
    REQUIRE((receipt.known && receipt.bytes && !bank.empty() && receipt.callerBankBytes == preparation::Vector(bank)));
    copy.success          = false;
    copy.execution_status = DrawCaptureExecutionStatus::kUnfinished;
    for (const bool pressure : { false, true })
        for (const auto result : { NativeGameplayPreparationStatus::Success, NativeGameplayPreparationStatus::Failed })
        {
            struct Retirement
            {
                std::vector<std::uint8_t>         sidecar;
                NativeGameplayPreparationContext* context = nullptr;
                NativeGameplayCommandFeed*        feed    = nullptr;
                NativeGameplayPreparationStatus   result;
                std::size_t                       charge = 0, prefixes = 0;
                unsigned                          waits = 0;
            } retirement;

            retirement.result = result;
            NativeGameplayPreparationContext context{ receipt, NativeGameplayPreparationStatus::Pending, 0, &retirement, [](void* value, bool wait, std::string&)
                                                      {
                                                          auto& owner = *static_cast<Retirement*>(value);
                                                          if (!wait)
                                                              return NativeGameplayPreparationStatus::Pending;
                                                          ++owner.waits;
                                                          UNSCOPED_INFO("prepared copy waits before admission or prefix emission");
                                                          REQUIRE((!owner.feed->Copy(2) && !owner.prefixes));
                                                          if (owner.result == NativeGameplayPreparationStatus::Success)
                                                          {
                                                              std::vector<std::uint8_t>().swap(owner.sidecar);
                                                              owner.context->callerBankBytes -= owner.charge;
                                                          }
                                                          return owner.result;
                                                      } };
            retirement.context = &context;
            NativeGameplayCommandFeed commands(std::span<const NativeGameplayShaderInterface>{}, nullptr, context);
            retirement.feed = &commands;
            commands.SetPrefixConsumer([&](auto, std::string&)
                                       {
                                           ++retirement.prefixes;
                                           return true;
                                       });
            UNSCOPED_INFO(error);
            REQUIRE((commands.Begin(1, 1, {}, error) && commands.Consume(clearEvent(1), {}, {}, error)));
            if (pressure)
            {
                retirement.sidecar.reserve(preparation::Budget - receipt.bytes - sizeof(retirement.sidecar));
                retirement.charge = preparation::Vector(retirement.sidecar);
                context.callerBankBytes += retirement.charge;
            }
            const bool accepted = commands.ConsumePreparedCopy(copy, {}, {}, error);
            if (!pressure)
            {
                UNSCOPED_INFO("funded prepared copy retains Pending ownership without forcing predecessor retirement");
                REQUIRE((accepted && commands.Copy(2) && retirement.waits == 0 && !retirement.prefixes && context.state == NativeGameplayPreparationStatus::Pending));
            }
            else if (result == NativeGameplayPreparationStatus::Success)
            {
                UNSCOPED_INFO("prepared copy waits and refunds only successfully retired ownership");
                REQUIRE((accepted && commands.Copy(2) && retirement.waits == 1 && context.receipt.bytes == 0 &&
                         context.callerBankBytes == receipt.callerBankBytes && retirement.sidecar.capacity() == 0));
            }
            else
            {
                UNSCOPED_INFO("failed retirement retains actual predecessor and caller charges without prepared copy output");
                REQUIRE((!accepted && !commands.Copy(2) && retirement.waits == 1 && !retirement.prefixes && retirement.sidecar.capacity() > 0 &&
                         context.callerBankBytes == retirement.charge && context.receipt.bytes == receipt.bytes));
                NativeRenderFrame failed;
                UNSCOPED_INFO("failed prepared copy funding cannot publish terminal output");
                REQUIRE((!commands.FinishProducer(0x100, 2, failed, error) && failed.passes.empty()));
            }
        }
}

// Finishes a gameplay tail on the chain its packets appended, joins them as
// the renderer does, and requires the graph the feed builds without packets.
void requireJoinedTail(NativeRenderFramePrefixState& chain, NativeRenderFrame tail, const std::vector<NativeRenderFrame>& packets, const NativeRenderFrame& full, const std::string& context)
{
    std::string                           error;
    std::vector<NativeRenderFramePass>    passes;
    std::vector<NativeRenderFrameTexture> textures;
    bool                                  typed = false;
    for (const auto& packet : packets)
    {
        passes.insert(passes.end(), packet.passes.begin(), packet.passes.end());
        for (const auto& texture : packet.textures)
            if (std::none_of(textures.begin(), textures.end(), [&](const auto& prior)
                             {
                                 return prior.id == texture.id;
                             }))
                textures.push_back(texture);
        typed |= packet.typedGuestWords;
    }
    UNSCOPED_INFO(context + ": the tail continues exactly its appended passes: " + error);
    REQUIRE((!full.continuedPrefixPasses && tail.continuedPrefixPasses == passes.size() && chain.FinishTerminal(tail, error)));
    UNSCOPED_INFO(context + ": " + error);
    REQUIRE(JoinNativeRenderFrameTail(tail, std::move(passes), textures, typed, error));
    UNSCOPED_INFO(context + ": the joined tail declares the full graph's keyed resources and passes");
    REQUIRE((tail.textures == full.textures && tail.outputTextureId == full.outputTextureId && tail.passes.size() == full.passes.size()));
    for (std::size_t index = 0; index < full.passes.size(); ++index)
    {
        const auto& joined   = tail.passes[index];
        const auto& expected = full.passes[index];
        const auto  resolved = [](const NativeRenderFramePass& pass)
        {
            std::vector<std::array<std::uint64_t, 3>> result;
            for (const auto& resolve : pass.resolves)
                result.push_back({ resolve.textureId, resolve.destinationX, resolve.width });
            return result;
        };
        UNSCOPED_INFO(context + ": joined pass " + std::to_string(index) + " matches the full graph");
        REQUIRE((GetNativeRenderFrameTarget(joined) == GetNativeRenderFrameTarget(expected) &&
                 joined.preservePreviousAttachments == expected.preservePreviousAttachments &&
                 joined.colorSourcePass == expected.colorSourcePass && joined.clears.size() == expected.clears.size() &&
                 resolved(joined) == resolved(expected) && joined.DrawCount() == expected.DrawCount()));
    }
}

void targetChangeFeed()
{
    const std::array<NativeRenderFramePass::Target, 6> targets{ {
        { 160, 32, 1, NativeDrawReplayTargetFormat::Rgba8 },
        { 320, 32, 1, NativeDrawReplayTargetFormat::Rgba8 },
        { 560, 512, 1, NativeDrawReplayTargetFormat::Rgba8 },
        { 320, 184, 4, NativeDrawReplayTargetFormat::Rgba8 },
        { 320, 184, 4, NativeDrawReplayTargetFormat::Rgb10a2 },
        { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 },
    } };
    const std::array<std::uint32_t, 6>                 surfaces{ 0x020000A0, 0x04000140, 0x08000230, 0x05010140, 0x05010140, 0x0A010280 };
    const std::vector<NativeGameplayShaderInterface>   shaders{ targetShader(1, 0), targetShader(4, 0), targetShader(4, 2) };
    for (const auto mode : { NativeGameplayCommandFeed::PrefixRecording::FirstPass,
                             NativeGameplayCommandFeed::PrefixRecording::CompatibleChain,
                             NativeGameplayCommandFeed::PrefixRecording::TargetChanges })
    {
        std::string                                error;
        NativeRenderFrame                          frame;
        std::vector<NativeGameplayRetainedTexture> next;
        NativeRenderFramePrefixState               validation;
        std::vector<NativeRenderFrame>             packets;
        NativeGameplayCommandFeed                  commands(shaders);
        const auto                                 consume = [&](NativeRenderFrame packet, std::string& failure)
        {
            if (!validation.Append(packet, failure))
                return false;
            packets.push_back(std::move(packet));
            return true;
        };
        if (mode == NativeGameplayCommandFeed::PrefixRecording::FirstPass)
            commands.SetPrefixConsumer(consume);
        else
            commands.SetPrefixConsumer(consume, mode);
        UNSCOPED_INFO(error);
        REQUIRE(commands.Begin(1, 1, {}, error));
        for (std::size_t index = 0; index < targets.size(); ++index)
        {
            const auto event = targetDrawEvent(index + 1, surfaces[index], targets[index], index + 1 == targets.size() ? 0 : static_cast<std::uint32_t>(index + 1));
            UNSCOPED_INFO(error);
            REQUIRE(commands.Consume(event, {}, {}, error));
            const auto expected = mode == NativeGameplayCommandFeed::PrefixRecording::TargetChanges ? index : std::min<std::size_t>(index, 1);
            UNSCOPED_INFO("callbacks emit only previously sealed passes under the selected policy");
            REQUIRE(packets.size() == expected);
        }
        UNSCOPED_INFO(error);
        REQUIRE((commands.Consume(copyEvent(7), {}, {}, error) && commands.Consume(clearEvent(8), {}, {}, error) &&
                 commands.Consume(copyEvent(9, true), {}, {}, error)));
        const auto expected = mode == NativeGameplayCommandFeed::PrefixRecording::TargetChanges ? 8U : 1U;
        UNSCOPED_INFO("only TargetChanges continues across dimension, sample and unsigned format changes");
        REQUIRE(packets.size() == expected);
        const auto        terminal = terminalEvent(10);
        const auto        boundary = snapshotWithEvents({ terminal });
        NativeRenderFrame full;
        {
            NativeGameplayCommandFeed                  plain(shaders);
            std::vector<NativeGameplayRetainedTexture> plainNext;
            UNSCOPED_INFO(error);
            REQUIRE(plain.Begin(1, 1, {}, error));
            for (std::size_t index = 0; index < targets.size(); ++index)
            {
                UNSCOPED_INFO(error);
                REQUIRE(plain.Consume(targetDrawEvent(index + 1, surfaces[index], targets[index], index + 1 == targets.size() ? 0 : static_cast<std::uint32_t>(index + 1)), {}, {}, error));
            }
            UNSCOPED_INFO(error);
            REQUIRE((plain.Consume(copyEvent(7), {}, {}, error) && plain.Consume(clearEvent(8), {}, {}, error) &&
                     plain.Consume(copyEvent(9, true), {}, {}, error) && plain.Finish(terminal, boundary.final_swap, 2, 1, full, plainNext, error) &&
                     full.passes.size() == 8));
        }
        // Every TargetChanges pass was a packet, so its tail carries only the output.
        const std::size_t held = expected;
        UNSCOPED_INFO("the terminal drops the passes its packets recorded: " + error);
        REQUIRE((commands.Finish(terminal, boundary.final_swap, 2, 1, frame, next, error) && frame.continuedPrefixPasses == held &&
                 frame.passes.size() == 8 - held && !next.empty()));
        if (mode == NativeGameplayCommandFeed::PrefixRecording::TargetChanges)
        {
            for (std::size_t index = 0; index < targets.size(); ++index)
            {
                UNSCOPED_INFO("target changes own only their newly sealed fresh pass");
                REQUIRE((packets[index].passes.size() == 1 && GetNativeRenderFrameTarget(packets[index].passes.front()) == targets[index] &&
                         !packets[index].passes.front().preservePreviousAttachments));
            }
            UNSCOPED_INFO("same-target preservation keeps generated versions unkeyed until terminal publication");
            REQUIRE((packets.back().passes.front().preservePreviousAttachments && packets.back().textures.front().persistentKey == 0 &&
                     frame.textures.front().persistentKey != 0));
        }
        requireJoinedTail(validation, std::move(frame), packets, full, "target change feed");
    }

    std::string                                error;
    NativeRenderFrame                          frame;
    std::vector<NativeGameplayRetainedTexture> next;
    std::size_t                                emitted = 0;
    NativeGameplayCommandFeed                  signedStop(std::span<const NativeGameplayShaderInterface>{});
    signedStop.SetPrefixConsumer([&](NativeRenderFrame, std::string&)
                                 {
                                     ++emitted;
                                     return true;
                                 },
                                 NativeGameplayCommandFeed::PrefixRecording::TargetChanges);
    UNSCOPED_INFO(error);
    REQUIRE((signedStop.Begin(1, 1, {}, error) && signedStop.Consume(clearEvent(1), {}, {}, error)));
    auto  signedClear = clearEvent(2);
    auto& r           = signedClear.draw.draw.registers;
    r[0x2000]         = 0x0A020280;
    r[0x2001]         = 0x00040530;
    r[0x2208]         = 5;
    auto& vertices    = signedClear.draw.draw.vertex_fetches.front().bytes;
    vertices.clear();
    for (const auto& vertex : std::array<std::array<float, 7>, 3>{ { { -0.5F, -0.5F, 1, 0, 0, 0, 0 }, { 639.5F, -0.5F, 1, 0, 0, 0, 0 }, { 639.5F, 359.5F, 1, 0, 0, 0, 0 } } })
        for (const auto component : vertex)
            appendBigFloat(vertices, component);
    UNSCOPED_INFO("the first signed sealed pass permanently defers even later compatible unsigned passes: " + error);
    REQUIRE((signedStop.Consume(signedClear, {}, {}, error) && emitted == 1 &&
             signedStop.Consume(clearEvent(3), {}, {}, error) && signedStop.Consume(copyEvent(4), {}, {}, error) &&
             signedStop.Consume(clearEvent(5), {}, {}, error) && signedStop.Consume(copyEvent(6, true), {}, {}, error) && emitted == 1));
    const auto terminal = terminalEvent(7);
    const auto boundary = snapshotWithEvents({ terminal });
    UNSCOPED_INFO("signed deferral keeps every pass after the one sealed packet in the tail: " + error);
    REQUIRE((signedStop.Finish(terminal, boundary.final_swap, 2, 1, frame, next, error) && frame.continuedPrefixPasses == 1 && frame.passes.size() == 4));
}

void queuedPrefixOwnership()
{
    using namespace std::chrono_literals;
    std::string                                error;
    NativeRenderFrame                          frame, owned;
    std::vector<NativeGameplayRetainedTexture> next;
    std::promise<NativeDrawReplayResult>       recorded;
    std::size_t                                admitted = 0, synchronous = 0;
    NativeGameplayCommandFeed                  commands(std::span<const NativeGameplayShaderInterface>{});
    commands.SetPrefixConsumer([&](NativeRenderFrame, std::string&)
                               {
                                   ++synchronous;
                                   return true;
                               });
    commands.SetPrefixQueueConsumer([&](NativeRenderFrame packet, std::shared_future<NativeDrawReplayResult>& ticket, std::string&)
                                    {
                                        ++admitted;
                                        owned  = std::move(packet);
                                        ticket = recorded.get_future().share();
                                        return true;
                                    });
    UNSCOPED_INFO(error);
    REQUIRE(commands.Begin(1, 1, {}, error));
    auto first = clearEvent(1);
    UNSCOPED_INFO(error);
    REQUIRE((commands.Consume(first, {}, {}, error) && commands.Consume(copyEvent(2), {}, {}, error)));
    first.draw.draw.vertex_fetches[0].bytes.assign(84, 0);
    UNSCOPED_INFO("queue admission owns copied clear values and unkeyed generated descriptors across Begin");
    REQUIRE((admitted == 1 && synchronous == 0 && owned.passes.size() == 1 && owned.passes[0].clears.size() == 1 &&
             owned.passes[0].clears[0].rectangle.right == 640 && owned.textures.size() == 1 && owned.textures[0].persistentKey == 0));
    UNSCOPED_INFO("later callbacks continue while first-pass CPU recording is pending: " + error);
    REQUIRE((commands.Consume(clearEvent(3), {}, {}, error) && commands.Consume(copyEvent(4, true), {}, {}, error) && admitted == 1));
    const auto terminal  = terminalEvent(5);
    const auto boundary  = snapshotWithEvents({ terminal });
    auto       finishing = std::async(std::launch::async, [&]
                                      {
                                    return commands.Finish(terminal, boundary.final_swap, 2, 1, frame, next, error);
                                      });
    UNSCOPED_INFO("terminal graph construction waits for the queued CPU recipe owner");
    REQUIRE(finishing.wait_for(50ms) == std::future_status::timeout);
    owned = {};
    recorded.set_value(recordingResult());
    UNSCOPED_INFO(error);
    REQUIRE((finishing.get() && !frame.passes.empty() && !next.empty()));

    commands.SetPrefixQueueConsumer([](NativeRenderFrame, std::shared_future<NativeDrawReplayResult>&, std::string&)
                                    {
                                        return false;
                                    });
    commands.SetPrefixConsumer([&](NativeRenderFrame, std::string&)
                               {
                                   ++synchronous;
                                   return true;
                               });
    UNSCOPED_INFO("the synchronous setter clears queue admission and carries across Begin: " + error);
    REQUIRE((commands.Begin(1, 1, {}, error) && commands.Consume(clearEvent(1), {}, {}, error) && commands.Consume(copyEvent(2), {}, {}, error) && synchronous == 1));
    UNSCOPED_INFO(error);
    REQUIRE((commands.Consume(clearEvent(3), {}, {}, error) && commands.Consume(copyEvent(4, true), {}, {}, error) &&
             commands.FinishProducer(0x114, 4, frame, error)));
}

void queuedPrefixFailures()
{
    using namespace std::chrono_literals;
    std::string                                error;
    NativeRenderFrame                          frame;
    std::vector<NativeGameplayRetainedTexture> next;
    NativeGameplayCommandFeed                  invalid(std::span<const NativeGameplayShaderInterface>{});
    invalid.SetPrefixQueueConsumer([](NativeRenderFrame, std::shared_future<NativeDrawReplayResult>&, std::string&)
                                   {
                                       return true;
                                   });
    UNSCOPED_INFO(error);
    REQUIRE((invalid.Begin(1, 1, {}, error) && invalid.Consume(clearEvent(1), {}, {}, error)));
    UNSCOPED_INFO("successful queue admission requires a valid recording ticket");
    REQUIRE((!invalid.Consume(copyEvent(2), {}, {}, error) && error.find("event 2") != std::string::npos && error.find("invalid recording ticket") != std::string::npos));
    const auto invalidError = error;
    UNSCOPED_INFO("invalid-ticket failure survives Begin and FinishProducer without a replacement owner");
    REQUIRE((!invalid.Begin(1, 1, {}, error) && error == invalidError && !invalid.FinishProducer(0x100, 2, frame, error) && error == invalidError));

    std::promise<NativeDrawReplayResult> recorded;
    NativeGameplayCommandFeed            deferred(std::span<const NativeGameplayShaderInterface>{});
    deferred.SetPrefixQueueConsumer([&](NativeRenderFrame, std::shared_future<NativeDrawReplayResult>& ticket, std::string&)
                                    {
                                        ticket = recorded.get_future().share();
                                        return true;
                                    });
    UNSCOPED_INFO(error);
    REQUIRE((deferred.Begin(1, 1, {}, error) && deferred.Consume(clearEvent(1), {}, {}, error) && deferred.Consume(copyEvent(2), {}, {}, error)));
    UNSCOPED_INFO("a separate preparation failure ends recording while its prefix remains owned");
    REQUIRE(!deferred.Consume(copyEvent(2), {}, {}, error));
    auto beginning = std::async(std::launch::async, [&]
                                {
                                    return deferred.Begin(1, 1, {}, error);
                                });
    UNSCOPED_INFO("Begin drains pending ownership even after recording ended on a preparation failure");
    REQUIRE(beginning.wait_for(50ms) == std::future_status::timeout);
    recorded.set_value(recordingResult(false, "delayed recording rejection"));
    UNSCOPED_INFO("Begin retains the admitted event identity of a deferred failure");
    REQUIRE((!beginning.get() && error.find("event 2: delayed recording rejection") != std::string::npos));
    const auto deferredError = error;
    const auto terminal      = terminalEvent(5);
    const auto boundary      = snapshotWithEvents({ terminal });
    UNSCOPED_INFO("a deferred failure cannot publish a terminal graph or proposed versions");
    REQUIRE((!deferred.Finish(terminal, boundary.final_swap, 2, 1, frame, next, error) && error == deferredError && next.empty() && frame.passes.empty()));

    std::array<std::promise<NativeDrawReplayResult>, 2> recordings;
    std::size_t                                         admitted = 0;
    NativeGameplayCommandFeed                           ordered(std::span<const NativeGameplayShaderInterface>{});
    ordered.SetPrefixQueueConsumer([&](NativeRenderFrame, std::shared_future<NativeDrawReplayResult>& ticket, std::string&)
                                   {
                                       ticket = recordings[admitted++].get_future().share();
                                       return true;
                                   },
                                   NativeGameplayCommandFeed::PrefixRecording::CompatibleChain);
    UNSCOPED_INFO(error);
    REQUIRE((ordered.Begin(1, 1, {}, error) && ordered.Consume(clearEvent(1), {}, {}, error) && ordered.Consume(clearEvent(2), {}, {}, error) && ordered.Consume(clearEvent(3), {}, {}, error) && admitted == 2));
    recordings[1].set_value(recordingResult(false, "second packet rejection"));
    auto ignored = snapshotEvent(4, DrawCaptureFrameEvent::Kind::kDraw);
    UNSCOPED_INFO("a later ready ticket remains ordered behind the pending first ticket");
    REQUIRE(ordered.Consume(ignored, {}, {}, error));
    recordings[0].set_value(recordingResult());
    ignored.id = 5;
    UNSCOPED_INFO("producer polling reports deferred errors in admission order before later preparation");
    REQUIRE((!ordered.Consume(ignored, {}, {}, error) && error.find("event 2: second packet rejection") != std::string::npos));
}

void queuedPrefixBudget(bool textureVersioned = false)
{
    using namespace std::chrono_literals;
    constexpr std::size_t                                      maximum     = 512U * 1024U * 1024U;
    constexpr auto                                             eventBytes  = sizeof(NativeGameplayRenderFramePass) + sizeof(NativeGameplayRenderFrameOperation);
    constexpr auto                                             copyBytes   = sizeof(NativeGameplayRenderFrameCopy) + sizeof(NativeGameplayResolveAdmission);
    const auto                                                 prefixBytes = sizeof(NativeRenderFrame) + sizeof(NativeRenderFrameTexture) + sizeof(NativeRenderFramePass) +
                                                                             (textureVersioned ? sizeof(std::shared_ptr<const NativeDrawReplayAdmission>) : sizeof(NativeRenderFramePass::Clear)) +
                                                                             sizeof(NativeRenderFrameResolve) + sizeof(std::shared_future<NativeDrawReplayResult>) + sizeof(std::size_t) + sizeof(std::uint64_t);
    std::string                                                error;
    NativeRenderFrame                                          owned;
    std::promise<NativeDrawReplayResult>                       recorded;
    std::vector<NativeGameplayShaderInterface>                 shaders;
    std::vector<rex::graphics::diagnostic::DrawCaptureTexture> textures;
    std::map<std::uint32_t, NativeDrawReplayTexture>           cpu;
    auto                                                       first = clearEvent(1);
    if (textureVersioned)
    {
        auto shader              = targetShader(4, 0);
        shader.textureFetchSlots = { 1 };
        shader.textureDimensions = { NativeDrawReplayTextureDimension::Texture2D };
        shaders.push_back(std::move(shader));
        textures.resize(1);
        textures[0].guest_base_page = 0x300;
        textures[0].availability    = rex::graphics::diagnostic::DrawCaptureAvailability::kUnavailable;
        cpu[0].width = cpu[0].height = 1;
        cpu[0].format                = NativeDrawReplayTextureFormat::R8;
        cpu[0].bytes                 = { 0x7F };
        UNSCOPED_INFO(error);
        REQUIRE(CopyNativeDrawReplayTextureVersion(cpu[0], error));
        first = targetDrawEvent(1, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0);
        rex::graphics::diagnostic::DrawCaptureTextureUse use;
        use.texture_index       = 0;
        use.fetch_constant      = 1;
        use.dimension           = 1;
        use.component_mapping   = 0x1688;
        use.data_available      = true;
        use.availability        = rex::graphics::diagnostic::DrawCaptureAvailability::kComplete;
        first.draw.texture_uses = { use };
    }
    NativeGameplayCommandFeed commands(shaders);
    commands.SetPrefixQueueConsumer([&](NativeRenderFrame packet, std::shared_future<NativeDrawReplayResult>& ticket, std::string&)
                                    {
                                        owned  = std::move(packet);
                                        ticket = recorded.get_future().share();
                                        return true;
                                    });
    UNSCOPED_INFO(error);
    REQUIRE((commands.Begin(1, 1, {}, error) && commands.Consume(first, textures, cpu, error) && commands.Consume(copyEvent(2), {}, {}, error)));
    auto        ignored       = snapshotEvent(3, DrawCaptureFrameEvent::Kind::kDraw);
    std::size_t originalBytes = NativeGameplayCommandFeed::CopyStateBytes() + 2 * eventBytes + copyBytes;
    if (textureVersioned)
    {
        const auto& recipe = owned.passes[0].Draw(0);
        originalBytes += sizeof(NativeGameplayRenderFrameDraw) + owned.passes[0].admittedDraws[0]->MetadataBytes() +
                         sizeof(NativeDrawReplayShaderVersion) + recipe.vertexShaderBytes().size() + recipe.pixelShaderBytes().size() +
                         recipe.vertexBytes().size() + recipe.indices.size() * sizeof(std::uint32_t) + recipe.samplers.size() * sizeof(NativeDrawReplaySampler) +
                         recipe.vertexConstants.size() + recipe.pixelConstants.size() + recipe.sharedConstants.size() +
                         recipe.textures[0].payloadBytes().size() + recipe.textures[0].textureVersion->MetadataBytes();
        cpu.clear();
        textures.clear();
    }
    // Supported non-host draws still consume the existing per-event planning
    // charge, allowing the real 512 MiB bound without a test-only limit.
    while (originalBytes + prefixBytes + eventBytes <= maximum)
    {
        UNSCOPED_INFO(error);
        REQUIRE(commands.Consume(ignored, {}, {}, error));
        originalBytes += eventBytes;
        ++ignored.id;
    }
    auto preparing = std::async(std::launch::async, [&]
                                {
                                    return commands.Consume(ignored, {}, {}, error);
                                });
    UNSCOPED_INFO("otherwise-fitting original preparation waits for pending copied-byte retirement");
    REQUIRE(preparing.wait_for(50ms) == std::future_status::timeout);
    owned = {};
    recorded.set_value(recordingResult());
    UNSCOPED_INFO(error);
    REQUIRE(preparing.get());
    originalBytes += eventBytes;
    ++ignored.id;
    while (originalBytes + eventBytes <= maximum)
    {
        UNSCOPED_INFO(error);
        REQUIRE(commands.Consume(ignored, {}, {}, error));
        originalBytes += eventBytes;
        ++ignored.id;
    }
    UNSCOPED_INFO("retirement preserves every charge added after admission in the current planning total");
    REQUIRE((!commands.Consume(ignored, {}, {}, error) && error.find("owned byte bound") != std::string::npos));
}

NativeGameplayRenderFrameDraw fullColorWrite(std::uint64_t eventId, std::uint32_t width)
{
    NativeGameplayRenderFrameDraw output{ eventId, draw(width, 720, 4, 9), false };
    auto&                         r = output.recipe;
    r.vertexShaderHash              = 0xDE0359DBD47D7FABULL;
    r.pixelShaderHash               = 0x560C753DAD5555ADULL;
    r.targetFormat                  = NativeDrawReplayTargetFormat::Rgb10a2;
    r.vertexAttributeCount          = 2;
    r.vertexStrideBytes             = 32;
    r.vertexData.resize(128);
    const std::array<std::array<float, 3>, 4> corners{ { { 0, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 } } };
    for (std::size_t index = 0; index < corners.size(); ++index)
        std::memcpy(r.vertexData.data() + index * 32, corners[index].data(), 12);
    const std::array<float, 16> matrix{ 2, 0, 0, -1, 0, -2, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1 };
    std::memcpy(r.vertexConstants.data(), matrix.data(), sizeof(matrix));
    r.indices                          = { 0, 1, 2, 0, 2, 3 };
    r.indexCount                       = 6;
    r.rasterizer.cull                  = 2;
    r.rasterizer.frontCounterClockwise = true;
    return output;
}

void admitDraws(NativeGameplayRenderFrameInput& input, std::string& error)
{
    for (auto& pass : input.passes)
        for (auto& draw : pass.draws)
        {
            UNSCOPED_INFO(error);
            REQUIRE(AdmitNativeDrawReplayRecipe(std::move(draw.recipe), draw.admittedRecipe, error));
            draw.recipe = {};
        }
}

void admittedOperations()
{
    std::string error;
    auto        input = completeMainInput();
    admitDraws(input, error);
    const auto        owner   = input.passes[0].draws[0].admittedRecipe;
    const auto*       indices = owner->Recipe().indices.data();
    NativeRenderFrame frame;
    UNSCOPED_INFO("const gameplay graph assembly reuses the exact canonical admitted operation");
    REQUIRE((BuildNativeGameplayRenderFrame(input, frame, error) && input.passes[0].draws[0].admittedRecipe == owner &&
             frame.passes[0].draws.empty() && frame.passes[0].admittedDraws[0] == owner &&
             frame.passes[0].Draw(0).indices.data() == indices));
    auto movedInput = input;
    UNSCOPED_INFO("owned graph assembly transfers handles without materializing leaf recipes");
    REQUIRE((BuildNativeGameplayRenderFrame(std::move(movedInput), frame, error) &&
             frame.passes[0].admittedDraws[0] == owner && !movedInput.passes[0].draws[0].admittedRecipe));
    const auto* handles = frame.passes[0].admittedDraws.data();
    UNSCOPED_INFO("an admitted pass within the batch bound preserves handle storage");
    REQUIRE((BatchNativeRenderFrame(frame, error) && frame.passes[0].admittedDraws.data() == handles));
    auto large = frame;
    large.passes[0].admittedDraws.assign(NativeRenderBatchCommands + 1, owner);
    large.passes[1].colorSourcePass = 1;
    UNSCOPED_INFO("admitted batches move canonical handles and remap aliases to completed source batches");
    REQUIRE((BatchNativeRenderFrame(large, error) && large.passes.size() == 3 &&
             large.passes[0].DrawCount() == NativeRenderBatchCommands &&
             large.passes[1].admittedDraws[0] == owner && large.passes[1].preservePreviousAttachments &&
             large.passes[2].colorSourcePass == 2));
    auto mixed = frame;
    mixed.passes[0].draws.push_back(owner->Recipe());
    UNSCOPED_INFO("batch and graph admission reject ambiguous mixed draw storage");
    REQUIRE((!BatchNativeRenderFrame(mixed, error) && error.find("mixes") != std::string::npos &&
             !ValidateNativeRenderFrame(mixed, error)));
    bool threw = false;
    try
    {
        (void)mixed.passes[0].DrawCount();
    }
    catch (const std::logic_error&)
    {
        threw = true;
    }
    UNSCOPED_INFO("common draw access cannot silently select one mixed storage representation");
    REQUIRE(threw);
    mixed = frame;
    mixed.passes[0].admittedDraws[0].reset();
    UNSCOPED_INFO("graph admission rejects empty operation handles");
    REQUIRE((!ValidateNativeRenderFrame(mixed, error) && error.find("empty admitted") != std::string::npos));
    auto incomplete = frame;
    incomplete.passes[1].resolves.clear();
    UNSCOPED_INFO("admitted leaves cannot bypass complete output resolves");
    REQUIRE(!ValidateNativeRenderFrame(incomplete, error));
    auto incompatible = frame;
    incompatible.passes[0].target.width += 1;
    UNSCOPED_INFO("admitted leaves retain contextual target checks");
    REQUIRE(!ValidateNativeRenderFrame(incompatible, error));
    auto imported                      = frame;
    imported.textures[0].imported      = true;
    imported.textures[0].persistentKey = 1;
    UNSCOPED_INFO("admitted leaves cannot overwrite imported native versions");
    REQUIRE((!ValidateNativeRenderFrame(imported, error) && error.find("immutable") != std::string::npos));
    auto changed             = draw(640, 720, 4, 9);
    auto geometry            = std::make_shared<std::vector<std::uint8_t>>(std::move(changed.vertexData));
    changed.sharedVertexData = geometry;
    std::shared_ptr<const NativeDrawReplayAdmission> mutableOwner;
    UNSCOPED_INFO(error);
    REQUIRE(AdmitNativeDrawReplayRecipe(std::move(changed), mutableOwner, error));
    input.passes[0].draws[0].admittedRecipe = mutableOwner;
    geometry->clear();
    UNSCOPED_INFO("shared extents are checked before terminal alias lookahead can read admitted geometry");
    REQUIRE((!BuildNativeGameplayRenderFrame(input, frame, error) && error.find("extent changed") != std::string::npos));
    NativeRenderFrame packet;
    packet.passes.resize(1);
    packet.passes[0].admittedDraws = { mutableOwner };
    NativeRenderFramePrefixState state;
    UNSCOPED_INFO("changed shared extents permanently end cumulative prefix admission");
    REQUIRE((!state.Append(packet, error) && error.find("extent changed") != std::string::npos &&
             !state.Append(packet, error) && error.find("already failed") != std::string::npos));
}

void shaderVersionFeed()
{
    std::string                                error;
    std::vector<NativeGameplayShaderInterface> shaders{ targetShader(4, 0) };
    NativeGameplayCommandFeed                  commands(shaders);
    std::array<NativeRenderFrame, 2>           frames;
    std::vector<NativeGameplayRetainedTexture> next;
    for (auto& frame : frames)
    {
        UNSCOPED_INFO(error);
        REQUIRE(commands.Begin(1, 1, {}, error));
        UNSCOPED_INFO(error);
        REQUIRE((commands.Consume(targetDrawEvent(1, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0), {}, {}, error) &&
                 commands.Consume(copyEvent(2), {}, {}, error) && commands.Consume(clearEvent(3), {}, {}, error) &&
                 commands.Consume(copyEvent(4, true), {}, {}, error)));
        const auto terminal = terminalEvent(5);
        const auto boundary = snapshotWithEvents({ terminal });
        UNSCOPED_INFO(error);
        REQUIRE(commands.Finish(terminal, boundary.final_swap, 2, 1, frame, next, error));
    }
    const auto version = frames[0].passes[0].Draw(0).shaderVersion;
    UNSCOPED_INFO("Begin preserves the current exact shader version across completed frames");
    REQUIRE((version && frames[1].passes[0].Draw(0).shaderVersion == version));
    const auto expectedPixel = *version->PixelBytecode();
    shaders[0].pixelShaderDxil[0] ^= 1;
    UNSCOPED_INFO("a reused version still rejects caller catalog mutations before admitting another draw");
    REQUIRE((commands.Begin(1, 1, {}, error) &&
             !commands.Consume(targetDrawEvent(1, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0), {}, {}, error) &&
             error.find("pinned interface") != std::string::npos &&
             *version->PixelBytecode() == expectedPixel));
}

DrawCaptureFrameEvent texturedDrawEvent(std::uint64_t id, std::uint32_t dimension = 1)
{
    auto  event       = targetDrawEvent(id, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0);
    auto& registers   = event.draw.draw.registers;
    registers[0x4806] = 2;
    registers[0x4807] = 0x300000 | 6;
    registers[0x4808] = 3U | (3U << 13);
    rex::graphics::diagnostic::DrawCaptureTextureUse use;
    use.texture_index     = 0;
    use.fetch_constant    = 1;
    use.dimension         = dimension;
    use.component_mapping = 0x1688;
    use.data_available    = true;
    use.availability      = rex::graphics::diagnostic::DrawCaptureAvailability::kComplete;
    event.draw.texture_uses.push_back(use);
    return event;
}

void preparedCreationWriterFeed()
{
    using namespace rex::graphics::diagnostic;
    for (bool floating : { false, true })
    {
        std::string error;
        auto        shader       = targetShader(4, 0);
        shader.textureFetchSlots = { 1 };
        shader.textureDimensions = { NativeDrawReplayTextureDimension::Texture2D };
        const std::vector<NativeGameplayShaderInterface> shaders{ shader };
        const NativeTextureFetch                         fetch = floating ? NativeTextureFetch{ 0x84000156, 0x001000A4, 0x003FE1FF, 0x16D1, 0, 0x200 } : NativeTextureFetch{ 0x08000002, 0x00100002, 0x007FE3FF, 0x248, 0, 0x200 };
        std::vector<std::uint8_t>                        base(1048576);
        NativeDrawReplayTexture                          owner;
        UNSCOPED_INFO(error);
        REQUIRE(DecodeNativeTextureVersioned(fetch, base, {}, owner, error));
        std::map<std::uint32_t, NativeDrawReplayTexture> cpu{ { 0, owner } };
        std::vector<DrawCaptureTexture>                  sources(1);
        auto&                                            metadata = sources[0];
        metadata.guest_width = metadata.guest_height = floating ? 512 : 1024;
        metadata.guest_depth_or_array_size           = 1;
        metadata.guest_base_page                     = fetch[1] >> 12;
        metadata.guest_pitch                         = floating ? 16 : 32;
        metadata.guest_format                        = floating ? 36 : 2;
        metadata.guest_endianness                    = floating ? 2 : 0;
        metadata.guest_tiled                         = floating;
        metadata.availability                        = DrawCaptureAvailability::kUnavailable;
        metadata.unavailable_reason                  = DrawCaptureReason::kTextureSource;
        auto event                                   = targetDrawEvent(1, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0);
        event.success = event.host_issued = false;
        event.execution_status            = DrawCaptureExecutionStatus::kUnfinished;
        event.draw.draw.registers[0x4400] = 0x3FC00000;
        std::copy(fetch.begin(), fetch.end(), event.draw.draw.registers.begin() + 0x4806);
        DrawCaptureTextureUse use;
        use.stage               = 1;
        use.texture_index       = 0;
        use.fetch_constant      = 1;
        use.dimension           = 1;
        use.data_available      = true;
        use.availability        = DrawCaptureAvailability::kComplete;
        use.component_mapping   = floating ? 0x1B68 : 0x1124;
        use.swizzled_signs      = floating ? 0x55 : 0;
        use.is_signed           = floating;
        use.sampler_value       = 1U << 11;
        event.draw.texture_uses = { use };
        NativeGameplayCommandFeed commands(shaders);
        UNSCOPED_INFO(error);
        REQUIRE((commands.Begin(1, 1, {}, error) && commands.ConsumePreparedDraw(event, sources, cpu, error)));
        UNSCOPED_INFO("creation writer feed retains exact complete CPU owner");
        REQUIRE((commands.Draw(1) && commands.Draw(1)->Recipe().textures[0].textureVersion == owner.textureVersion));
        cpu[0].textureVersion.reset();
        cpu[0].sharedBytes.reset();
        cpu[0].bytes.assign(1048575U, 0);
        NativeGameplayCommandFeed rejected(shaders);
        UNSCOPED_INFO("creation writer feed rejects truncated full owner");
        REQUIRE((rejected.Begin(1, 1, {}, error) && !rejected.ConsumePreparedDraw(event, sources, cpu, error)));
    }
}

void preparedCpuCubeFeed()
{
    using namespace rex::graphics::diagnostic;
    for (unsigned form = 0; form < 3; ++form)
    {
        const bool floating = form == 1, bc3 = form == 2;
        const auto size = bc3 ? 128U : 64U;

        std::string error;
        auto        shader       = targetShader(4, 0);
        shader.textureFetchSlots = { 1 };
        shader.textureDimensions = { NativeDrawReplayTextureDimension::TextureCube };
        const std::vector<NativeGameplayShaderInterface> shaders{ shader };
        NativeTextureFetch                               fetch = bc3 ? NativeTextureFetch{ 0x01000002, 0x00100054, 0x140FE07F, 0x00000D10, 0x1C0, 0x00180E00 } : floating ? NativeTextureFetch{ 0x00824956, 0x00100060, 0x1407E03F, 0x00A80D11, 0x183, 0x00180E18 }
                                                                                                                                                                          : NativeTextureFetch{ 0x00824802, 0x00100086, 0x1407E03F, 0x00A81414, 0x183, 0x00180E18 };
        NativeDrawReplayTexture                          owner;
        std::vector<std::uint8_t>                        base(floating ? 196608 : 98304), mips(bc3 ? 280192 : floating ? 94208
                                                                                                                       : 94080);
        UNSCOPED_INFO(error);
        REQUIRE(DecodeNativeTextureVersioned(fetch, base, mips, owner, error));
        std::map<std::uint32_t, NativeDrawReplayTexture> cpu{ { 0, owner } };
        std::vector<DrawCaptureTexture>                  sources(1);
        auto&                                            metadata = sources[0];
        metadata.guest_width = metadata.guest_height = size;
        metadata.guest_depth_or_array_size           = 6;
        metadata.guest_base_page                     = fetch[1] >> 12;
        metadata.guest_mip_page                      = fetch[5] >> 12;
        metadata.guest_pitch                         = bc3 ? 4 : 2;
        metadata.guest_format                        = bc3 ? 20 : floating ? 32
                                                                           : 6;
        metadata.guest_endianness                    = (floating || bc3) ? 1 : 2;
        metadata.guest_packed_mips                   = true;
        // Snapshot metadata omits cube payloads; the Prepared CPU owner supplies them.
        metadata.availability       = DrawCaptureAvailability::kUnavailable;
        metadata.unavailable_reason = DrawCaptureReason::kTextureSource;
        auto event                  = targetDrawEvent(1, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0);
        event.success = event.host_issued = false;
        event.execution_status            = DrawCaptureExecutionStatus::kUnfinished;
        event.draw.draw.registers[0x4400] = 0x3FC00000;
        std::copy(fetch.begin(), fetch.end(), event.draw.draw.registers.begin() + 0x4806);
        DrawCaptureTextureUse use;
        use.stage               = 1;
        use.texture_index       = 0;
        use.fetch_constant      = 1;
        use.dimension           = 3;
        use.data_available      = true;
        use.availability        = DrawCaptureAvailability::kComplete;
        use.component_mapping   = (floating || bc3) ? 0x1688 : 0x1A0A;
        use.swizzled_signs      = floating ? 0x55 : 0;
        use.is_signed           = floating;
        use.sampler_value       = 1U << 11;
        event.draw.texture_uses = { use };
        NativeGameplayCommandFeed commands(shaders);
        UNSCOPED_INFO(error);
        REQUIRE((commands.Begin(1, 1, {}, error) && commands.ConsumePreparedDraw(event, sources, cpu, error)));
        UNSCOPED_INFO("Prepared command feed retains the exact six-face CPU owner");
        REQUIRE((commands.Draw(1) && commands.Draw(1)->Recipe().textures[0].textureVersion == owner.textureVersion));
        const auto reject = [&](auto mutate)
        {
            auto badEvent   = event;
            auto badSources = sources;
            auto badCpu     = cpu;
            mutate(badEvent, badSources[0], badCpu.at(0));
            NativeGameplayCommandFeed failed(shaders);
            UNSCOPED_INFO("Prepared cube command feed rejects inconsistent owners");
            REQUIRE((failed.Begin(1, 1, {}, error) && !failed.ConsumePreparedDraw(badEvent, badSources, badCpu, error)));
        };
        reject([](auto& e, auto&, auto&)
               {
                   e.draw.draw.registers[0x4400] = 0x3F800000;
               });
        reject([](auto&, auto& s, auto&)
               {
                   s.availability       = DrawCaptureAvailability::kUnavailable;
                   s.unavailable_reason = DrawCaptureReason::kTextureUnresolved;
               });
        reject([](auto&, auto& s, auto&)
               {
                   s.guest_depth_or_array_size = 5;
               });
        reject([](auto&, auto& s, auto&)
               {
                   --s.guest_height;
               });
        reject([](auto& e, auto&, auto&)
               {
                   e.draw.texture_uses[0].dimension = 1;
               });
        reject([](auto& e, auto&, auto&)
               {
                   e.draw.texture_uses[0].is_signed = !e.draw.texture_uses[0].is_signed;
               });
        reject([](auto& e, auto&, auto&)
               {
                   e.draw.draw.registers[0x4808] = (e.draw.draw.registers[0x4808] & 0x03FFFFFFU) | (4U << 26);
               });
        reject([](auto&, auto&, auto& t)
               {
                   t.dimension = NativeDrawReplayTextureDimension::Texture2D;
               });
        reject([](auto&, auto&, auto& t)
               {
                   --t.mipLevels;
               });
        reject([](auto&, auto&, auto& t)
               {
                   t.textureVersion.reset();
               });
        reject([](auto&, auto&, auto& t)
               {
                   t.swizzle = { 0, 0, 0, 0 };
               });
        reject([](auto& e, auto&, auto&)
               {
                   e.draw.draw.registers[0x4809] |= 1U << 13;
               });
        reject([](auto& e, auto& s, auto&)
               {
                   ++s.guest_pitch;
                   e.draw.draw.registers[0x4806] += 1U << 22;
               });
        reject([](auto& e, auto&, auto&)
               {
                   e.draw.draw.registers[0x480A] -= 1U << 6;
               });
        reject([](auto& e, auto&, auto&)
               {
                   e.draw.draw.registers[0x4809] ^= 1U;
               });
        reject([](auto& e, auto& s, auto&)
               {
                   s.guest_endianness = 0;
                   e.draw.draw.registers[0x4807] &= ~(3U << 6);
               });
        if (floating)
            reject([](auto& e, auto&, auto&)
                   {
                       e.draw.draw.registers[0x4806] = (e.draw.draw.registers[0x4806] & ~0x3FCU) | (0xAAU << 2);
                   });
    }
}

void preparedTextureFeed()
{
    using namespace rex::graphics::diagnostic;
    std::string error;
    auto        shader       = targetShader(4, 0);
    shader.textureFetchSlots = { 1 };
    shader.textureDimensions = { NativeDrawReplayTextureDimension::TextureCube };
    std::vector<NativeGameplayShaderInterface> shaders{ shader };
    std::vector<DrawCaptureTexture>            sources(1);
    auto&                                      source = sources[0];
    source.guest_base_page                            = 0x300;
    source.host_width = source.host_height = 4;
    source.host_depth_or_array_size        = 6;
    source.host_mip_levels                 = 3;
    source.host_format                     = 60;
    for (std::uint32_t slice = 0; slice < 6; ++slice)
        for (std::uint32_t mip = 0; mip < 3; ++mip)
        {
            DrawCaptureTextureSubresource subresource;
            subresource.array_slice = slice;
            subresource.mip_level   = mip;
            subresource.host_width = subresource.host_height = 4U >> mip;
            subresource.row_size                             = subresource.host_width;
            subresource.row_count                            = subresource.host_height;
            subresource.bytes.assign(subresource.row_size * subresource.row_count, static_cast<std::uint8_t>(slice * 3 + mip));
            source.subresources.push_back(std::move(subresource));
        }
    NativeRenderFrame                          frame;
    std::vector<NativeGameplayRetainedTexture> next;
    {
        NativeGameplayCommandFeed commands(shaders);
        auto                      first               = texturedDrawEvent(1, 3);
        auto                      second              = texturedDrawEvent(2, 3);
        second.draw.texture_uses[0].component_mapping = 0x16C0;
        second.draw.texture_uses[0].sampler_value     = 1U << 11;
        UNSCOPED_INFO(error);
        REQUIRE((commands.Begin(1, 1, {}, error) && commands.Consume(first, sources, {}, error) &&
                 commands.Consume(second, sources, {}, error) && commands.Consume(copyEvent(3), {}, {}, error) &&
                 commands.Consume(clearEvent(4), {}, {}, error) && commands.Consume(copyEvent(5, true), {}, {}, error)));
        const auto terminal = terminalEvent(6);
        const auto boundary = snapshotWithEvents({ terminal });
        UNSCOPED_INFO(error);
        REQUIRE(commands.Finish(terminal, boundary.final_swap, 2, 1, frame, next, error));
    }
    const auto& first   = frame.passes.front().admittedDraws[0];
    const auto& second  = frame.passes.front().admittedDraws[1];
    const auto  version = first->Recipe().textures[0].textureVersion;
    UNSCOPED_INFO("saved cube packing retains one exact slice/mip layout shared across admitted draws");
    REQUIRE((version && second->Recipe().textures[0].textureVersion == version && version->Subresources().size() == 18 &&
             version->Payload()->size() == 6 * (16 + 4 + 1) && version->Subresources()[3].arraySlice == 1 &&
             version->Subresources()[3].sourceOffset == 21 && version->Subresources()[17].sourceOffset == 125));
    UNSCOPED_INFO("saved source reuse preserves each draw's swizzle and effective sampler");
    REQUIRE((first->TextureBindings()[0].swizzle != second->TextureBindings()[0].swizzle &&
             !first->TextureBindings()[0].sampler.magLinear && second->TextureBindings()[0].sampler.magLinear));
    sources[0].subresources[0].bytes[0] = 0xEE;
    sources.clear();
    UNSCOPED_INFO("source mutation and feed retirement cannot change admitted saved texture bytes");
    REQUIRE((version->Payload()->front() == 0 && version->Payload()->back() == 17 && ValidateNativeRenderFrame(frame, error)));

    shaders[0].textureDimensions = { NativeDrawReplayTextureDimension::Texture2D };
    NativeGameplayCommandFeed commands(shaders);
    sources.resize(1);
    sources[0].guest_base_page                                          = 0x300;
    sources[0].availability                                             = DrawCaptureAvailability::kUnavailable;
    const auto                                                   marker = std::make_shared<int>(1);
    const std::shared_ptr<const NativeRenderFrameTextureVersion> proof(marker, reinterpret_cast<const NativeRenderFrameTextureVersion*>(marker.get()));
    const std::array<NativeGameplayRetainedTexture, 2>           retained{
        NativeGameplayRetainedTexture{ 0x300, { 1, 4, 4, NativeDrawReplayTextureFormat::Rgba8, 99, true, proof } },
        NativeGameplayRetainedTexture{ 0x500, { 2, 4, 4, NativeDrawReplayTextureFormat::Rgba8, 100, true, proof } }
    };
    bool                           prefixProof = false;
    std::vector<NativeRenderFrame> packets;
    commands.SetPrefixConsumer([&](NativeRenderFrame packet, std::string&)
                               {
                                   prefixProof = std::ranges::any_of(packet.textures, [&](const auto& texture)
                                                                     {
                                                                         return texture.imported && texture.persistentKey == 99 && texture.producerVersion == proof;
                                                                     });
                                   packets.push_back(std::move(packet));
                                   return true;
                               });
    std::map<std::uint32_t, NativeDrawReplayTexture> cpu;
    UNSCOPED_INFO(error);
    REQUIRE((commands.Begin(1, 1, retained, error) && commands.Consume(texturedDrawEvent(1), sources, cpu, error)));
    cpu[0].width = cpu[0].height = 4;
    cpu[0].bytes.assign(64, 0x7F);
    UNSCOPED_INFO(error);
    REQUIRE(CopyNativeDrawReplayTextureVersion(cpu[0], error));
    const auto replacement = cpu[0].textureVersion;
    UNSCOPED_INFO(error);
    REQUIRE((commands.Consume(texturedDrawEvent(2), sources, cpu, error) && commands.Consume(copyEvent(3), {}, {}, error) &&
             commands.Consume(clearEvent(4), {}, {}, error) && commands.Consume(copyEvent(5, true), {}, {}, error)));
    cpu.clear();
    const auto terminal = terminalEvent(6);
    const auto boundary = snapshotWithEvents({ terminal });
    UNSCOPED_INFO(error);
    REQUIRE(commands.Finish(terminal, boundary.final_swap, 2, 1, frame, next, error));
    const bool  nativeImport           = std::ranges::any_of(frame.textures, [&](const auto& texture)
                                                             {
                                                      return texture.imported && texture.persistentKey == 99 && texture.producerVersion == proof;
                                                             });
    const bool  overwrittenPageOmitted = std::ranges::none_of(next, [](const auto& value)
                                                              {
                                                                 return value.guestBasePage == 0x300;
                                                              });
    const auto& drawn                  = chainPass(packets, frame, 0);
    UNSCOPED_INFO("a CPU rewrite supersedes a retained native page without changing an earlier admitted native binding");
    REQUIRE((prefixProof && nativeImport && overwrittenPageOmitted && frame.continuedPrefixPasses == chainPasses(packets) &&
             drawn.Draw(0).textures[0].frameTextureId == 1 && !drawn.Draw(0).textures[0].textureVersion &&
             drawn.Draw(1).textures[0].frameTextureId == 0 && drawn.Draw(1).textures[0].textureVersion == replacement));
    UNSCOPED_INFO("unused carried pages preserve their exact proof through the complete candidate snapshot");
    REQUIRE(std::ranges::any_of(next, [&](const auto& value)
                                {
                                    return value.guestBasePage == 0x500 && value.texture.persistentKey == 100 && value.texture.producerVersion == proof;
                                }));
}

void preparedNativeTextureFeed()
{
    using namespace rex::graphics::diagnostic;
    std::string error;
    auto        shader       = targetShader(4, 0);
    shader.textureFetchSlots = { 1 };
    shader.textureDimensions = { NativeDrawReplayTextureDimension::Texture2D };
    const std::vector<NativeGameplayShaderInterface> shaders{ shader };

    const NativeTextureFetch  fetch{ 2U | (1U << 22) | (1U << 31), 0x00100006, 0, 0xD10, 0, 0x200 };
    NativeTextureMemoryRanges ranges;
    UNSCOPED_INFO(error);
    REQUIRE(GetNativeTextureMemoryRanges(fetch, ranges, error));
    auto native    = texturedDrawEvent(1);
    native.success = native.host_issued = false;
    native.execution_status             = DrawCaptureExecutionStatus::kUnfinished;
    std::copy(fetch.begin(), fetch.end(), native.draw.draw.registers.begin() + 0x4806);
    native.draw.texture_uses[0].data_available     = false;
    native.draw.texture_uses[0].availability       = DrawCaptureAvailability::kUnavailable;
    native.draw.texture_uses[0].unavailable_reason = DrawCaptureReason::kTextureUnresolved;

    DrawCaptureTexture source;
    source.availability              = DrawCaptureAvailability::kUnavailable;
    source.unavailable_reason        = DrawCaptureReason::kTextureUnresolved;
    source.guest_base_page           = ranges.base.address >> 12;
    source.guest_mip_page            = ranges.mips.address >> 12;
    source.guest_width               = 1;
    source.guest_height              = 1;
    source.guest_depth_or_array_size = 1;
    source.guest_pitch               = (fetch[0] >> 22) & 0x1FFU;
    source.guest_format              = 6;
    source.guest_endianness          = (fetch[1] >> 6) & 3U;
    source.guest_tiled               = (fetch[0] >> 31) != 0;
    source.guest_packed_mips         = false;
    source.host_width = source.host_height = 1;
    source.host_depth_or_array_size        = 1;
    source.host_mip_levels                 = 1;
    source.host_format                     = 27;
    std::vector<DrawCaptureTexture> sources{ source };

    const auto                                                   marker = std::make_shared<int>(1);
    const std::shared_ptr<const NativeRenderFrameTextureVersion> proof(
        marker, reinterpret_cast<const NativeRenderFrameTextureVersion*>(marker.get()));
    NativeGameplayGuestTextureLayout producerLayout;
    producerLayout.valid            = true;
    producerLayout.basePage         = source.guest_base_page;
    producerLayout.mipPage          = source.guest_mip_page;
    producerLayout.width            = source.guest_width;
    producerLayout.height           = source.guest_height;
    producerLayout.depthOrArraySize = source.guest_depth_or_array_size;
    producerLayout.pitch            = source.guest_pitch;
    producerLayout.format           = source.guest_format;
    producerLayout.endianness       = source.guest_endianness;
    producerLayout.tiled            = source.guest_tiled;
    std::vector<NativeGameplayRetainedTexture> retained{
        { source.guest_base_page, { 1, 1, 1, NativeDrawReplayTextureFormat::Rgba8, 99, true, proof }, producerLayout }
    };
    if (source.guest_mip_page && source.guest_mip_page != source.guest_base_page)
        retained.push_back({ source.guest_mip_page, { 1, 1, 1, NativeDrawReplayTextureFormat::Rgba8, 99, true, proof }, producerLayout });

    NativeGameplayCommandFeed commands(shaders);
    UNSCOPED_INFO(error);
    REQUIRE((commands.Begin(1, 1, retained, error) && commands.ConsumePreparedDraw(native, sources, {}, error)));
    UNSCOPED_INFO("a prepared native texture binds the retained resource without a CPU payload or version");
    REQUIRE((commands.Draw(1) && commands.Draw(1)->Recipe().textures[0].frameTextureId == 1 &&
             !commands.Draw(1)->Recipe().textures[0].textureVersion));
    UNSCOPED_INFO(error);
    REQUIRE((commands.Consume(copyEvent(2), {}, {}, error) && commands.Consume(copyEvent(3, true), {}, {}, error)));
    const auto                                 terminal = terminalEvent(4);
    const auto                                 boundary = snapshotWithEvents({ terminal });
    NativeRenderFrame                          frame;
    std::vector<NativeGameplayRetainedTexture> next;
    UNSCOPED_INFO(error);
    REQUIRE(commands.Finish(terminal, boundary.final_swap, 2, 1, frame, next, error));
    UNSCOPED_INFO("native prepared draw retains the completed producer proof through the terminal frame");
    REQUIRE(std::ranges::any_of(frame.textures, [](const auto& texture)
                                {
                                    return texture.id == 1 && texture.imported && texture.persistentKey == 99;
                                }));

    NativeGameplayCommandFeed                        stale(shaders);
    std::map<std::uint32_t, NativeDrawReplayTexture> cpu;
    cpu[0].width = cpu[0].height = 1;
    cpu[0].bytes.assign(4, 0x7F);
    UNSCOPED_INFO(error);
    REQUIRE(stale.Begin(1, 1, retained, error));
    UNSCOPED_INFO("a stale CPU owner cannot override a prepared native source");
    REQUIRE(!stale.ConsumePreparedDraw(native, sources, cpu, error));

    auto changedNative  = native;
    auto changedSources = sources;
    changedNative.draw.draw.registers[0x4806 + 1] |= 1U << 6;
    changedSources[0].guest_endianness = 1;
    NativeGameplayCommandFeed changed(shaders);
    UNSCOPED_INFO(error);
    REQUIRE(changed.Begin(1, 1, retained, error));
    UNSCOPED_INFO("a consumer endian change cannot self-authorize against the retained producer layout");
    REQUIRE((!changed.ConsumePreparedDraw(changedNative, changedSources, {}, error) &&
             error.find("producer") != std::string::npos));

    auto changedProducer                           = retained;
    changedProducer.front().guestLayout.endianness = 1;
    NativeGameplayCommandFeed producerChange(shaders);
    UNSCOPED_INFO(error);
    REQUIRE(producerChange.Begin(1, 1, changedProducer, error));
    UNSCOPED_INFO("a changed producer layout cannot authorize an unchanged consumer fetch");
    REQUIRE((!producerChange.ConsumePreparedDraw(native, sources, {}, error) &&
             error.find("producer") != std::string::npos));

    changedProducer                           = retained;
    changedProducer.front().guestLayout.tiled = false;
    NativeGameplayCommandFeed untiledProducer(shaders);
    UNSCOPED_INFO(error);
    REQUIRE(untiledProducer.Begin(1, 1, changedProducer, error));
    UNSCOPED_INFO("a linear producer layout cannot authorize the SDK tiled resolve consumer");
    REQUIRE((!untiledProducer.ConsumePreparedDraw(native, sources, {}, error) &&
             error.find("producer") != std::string::npos));

    auto missingLayout                = retained;
    missingLayout.front().guestLayout = {};
    NativeGameplayCommandFeed missing(shaders);
    UNSCOPED_INFO(error);
    REQUIRE(missing.Begin(1, 1, missingLayout, error));
    UNSCOPED_INFO("a native source without a retained producer layout is rejected");
    REQUIRE((!missing.ConsumePreparedDraw(native, sources, {}, error) && error.find("producer layout") != std::string::npos));

    auto badAliasSources              = sources;
    badAliasSources[0].guest_mip_page = source.guest_base_page + 1;
    NativeGameplayCommandFeed badAlias(shaders);
    UNSCOPED_INFO(error);
    REQUIRE(badAlias.Begin(1, 1, retained, error));
    UNSCOPED_INFO("a native source with an unproved mip alias is rejected");
    REQUIRE((!badAlias.ConsumePreparedDraw(native, badAliasSources, {}, error) && error.find("aliases") != std::string::npos));
}

void preparedSignedNativeProducerFeed()
{
    using namespace rex::graphics::diagnostic;
    std::string               error;
    const NativeTextureFetch  firstFetch{ 0x8A024956, 0x1E7B009F, 0x0059E4FF, 0x00001691, 0, 0x00000218 };
    const NativeTextureFetch  secondFetch{ 0x8A024956, 0x1E3B009F, 0x0059E4FF, 0x00001691, 0, 0x00000218 };
    NativeTextureMemoryRanges firstRanges, secondRanges;
    UNSCOPED_INFO(error);
    REQUIRE((GetNativeTextureMemoryRanges(firstFetch, firstRanges, error) && GetNativeTextureMemoryRanges(secondFetch, secondRanges, error)));

    const auto prepared = [](DrawCaptureFrameEvent event)
    {
        event.success = event.host_issued = false;
        event.execution_status            = DrawCaptureExecutionStatus::kUnfinished;
        event.execution_reason            = DrawCaptureReason::kNone;
        return event;
    };
    const auto floatCopy = [&](std::uint64_t id, std::uint32_t destinationBase, std::uint32_t colorInfo)
    {
        auto  event = prepared(copyEvent(id));
        auto& r     = event.draw.draw.registers;
        r[0x2000]   = 0x14000500;
        r[0x2001]   = colorInfo;
        r[0x2318]   = 0x00100000;
        r[0x2319]   = destinationBase;
        r[0x231B]   = 0x0000EF82;
        auto& bytes = event.draw.draw.vertex_fetches.front().bytes;
        bytes.clear();
        for (const auto value : std::array<float, 6>{ -0.5F, -0.5F, 1279.5F, -0.5F, 1279.5F, 719.5F })
            appendBigFloat(bytes, value);
        return event;
    };
    const auto sourceFor = [](const NativeTextureFetch& fetch, const NativeTextureMemoryRanges& ranges)
    {
        DrawCaptureTexture source;
        source.availability              = DrawCaptureAvailability::kUnavailable;
        source.unavailable_reason        = DrawCaptureReason::kTextureUnresolved;
        source.guest_base_page           = ranges.base.address >> 12;
        source.guest_mip_page            = ranges.mips.address >> 12;
        source.guest_width               = 1280;
        source.guest_height              = 720;
        source.guest_depth_or_array_size = 1;
        source.guest_pitch               = (fetch[0] >> 22) & 0x1FFU;
        source.guest_format              = 31;
        source.guest_endianness          = (fetch[1] >> 6) & 3U;
        source.guest_tiled               = true;
        source.host_width                = 1280;
        source.host_height               = 720;
        source.host_depth_or_array_size  = 1;
        source.host_mip_levels           = 1;
        source.host_format               = 33;
        return source;
    };
    std::vector<DrawCaptureTexture> sources{ sourceFor(firstFetch, firstRanges), sourceFor(secondFetch, secondRanges) };
    const auto                      makeConsumer = [&](std::uint64_t id, bool signedMetadata, bool changedExtent)
    {
        auto  event = prepared(targetDrawEvent(id, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0));
        auto& r     = event.draw.draw.registers;
        std::copy(firstFetch.begin(), firstFetch.end(), r.begin() + 0x4806);
        std::copy(secondFetch.begin(), secondFetch.end(), r.begin() + 0x480C);
        for (std::uint32_t index = 0; index != 2; ++index)
        {
            DrawCaptureTextureUse use;
            use.texture_index      = index;
            use.fetch_constant     = index + 1;
            use.dimension          = 1;
            use.component_mapping  = signedMetadata ? 6984 : 6983;
            use.swizzled_signs     = signedMetadata ? 0x55 : 0x54;
            use.is_signed          = signedMetadata;
            use.data_available     = false;
            use.availability       = DrawCaptureAvailability::kUnavailable;
            use.unavailable_reason = DrawCaptureReason::kTextureUnresolved;
            event.draw.texture_uses.push_back(use);
        }
        if (changedExtent)
            event.draw.draw.registers[0x4808] = (719U << 13) | 1278U;
        return event;
    };
    const auto makeProducer = [&](std::uint64_t id, std::uint32_t colorBase)
    {
        auto event                        = prepared(targetDrawEvent(id, 0x14000500, { 1280, 720, 1, NativeDrawReplayTargetFormat::Rg16Snorm }, colorBase));
        event.draw.draw.registers[0x2001] = 0x00040000 | colorBase;
        return event;
    };

    auto                          producerA   = makeProducer(1, 0);
    auto                          firstCopy   = floatCopy(2, firstRanges.base.address, 0x00040000);
    auto                          producerB   = makeProducer(3, 0x2D0);
    auto                          secondCopy  = floatCopy(4, secondRanges.base.address, 0x000402D0);
    auto                          consumer    = makeConsumer(5, true, false);
    auto                          outputLeft  = copyEvent(6);
    auto                          outputRight = copyEvent(7, true);
    auto                          terminal    = terminalEvent(8);
    const auto                    boundary    = snapshotWithEvents({ terminal });
    NativeGameplayShaderInterface shader      = targetShader(1, 4);
    shader.textureFetchSlots.clear();
    shader.textureDimensions.clear();
    shader                   = targetShader(4, 0);
    shader.textureFetchSlots = { 1, 2 };
    shader.textureDimensions = { NativeDrawReplayTextureDimension::Texture2D, NativeDrawReplayTextureDimension::Texture2D };
    const std::vector<NativeGameplayShaderInterface> shaders{ shader, targetShader(1, 4) };

    NativeGameplayCommandFeed commands(shaders);
    UNSCOPED_INFO(error);
    REQUIRE(commands.Begin(1, 1, {}, error));
    UNSCOPED_INFO("producer A: " + error);
    REQUIRE(commands.ConsumePreparedDraw(producerA, {}, {}, error));
    UNSCOPED_INFO("first copy: " + error);
    REQUIRE(commands.ConsumePreparedCopy(firstCopy, {}, {}, error));
    UNSCOPED_INFO("producer B: " + error);
    REQUIRE(commands.ConsumePreparedDraw(producerB, {}, {}, error));
    UNSCOPED_INFO("second copy: " + error);
    REQUIRE(commands.ConsumePreparedCopy(secondCopy, {}, {}, error));
    UNSCOPED_INFO("consumer: " + error);
    REQUIRE(commands.ConsumePreparedDraw(consumer, sources, {}, error));
    UNSCOPED_INFO("output left: " + error);
    REQUIRE(commands.Consume(outputLeft, {}, {}, error));
    UNSCOPED_INFO("output right: " + error);
    REQUIRE(commands.Consume(outputRight, {}, {}, error));
    NativeRenderFrame                          frame;
    std::vector<NativeGameplayRetainedTexture> next;
    UNSCOPED_INFO(error);
    REQUIRE(commands.Finish(terminal, boundary.final_swap, 2, 1, frame, next, error));
    UNSCOPED_INFO("same-frame RG16 float graph retains its consumer pass");
    REQUIRE((frame.passes.size() >= 3 && frame.passes[2].DrawCount()));
    const auto& rg16Recipe = frame.passes[2].Draw(0);
    UNSCOPED_INFO("same-frame complete RG16 float producers feed both signed consumer textures before presentation");
    REQUIRE((frame.passes.size() >= 4 && rg16Recipe.textures.size() >= 2 &&
             rg16Recipe.textures[0].frameTextureId && rg16Recipe.textures[1].frameTextureId));

    const auto rejectsRawConsumer = [&](DrawCaptureFrameEvent badConsumer, const char* message)
    {
        NativeGameplayCommandFeed rawMismatch(shaders);
        UNSCOPED_INFO(error);
        REQUIRE(rawMismatch.Begin(1, 1, {}, error));
        UNSCOPED_INFO(error);
        REQUIRE(rawMismatch.ConsumePreparedDraw(producerA, {}, {}, error));
        UNSCOPED_INFO(error);
        REQUIRE(rawMismatch.ConsumePreparedCopy(firstCopy, {}, {}, error));
        UNSCOPED_INFO(error);
        REQUIRE(rawMismatch.ConsumePreparedDraw(producerB, {}, {}, error));
        UNSCOPED_INFO(error);
        REQUIRE(rawMismatch.ConsumePreparedCopy(secondCopy, {}, {}, error));
        UNSCOPED_INFO(message);
        REQUIRE(!rawMismatch.ConsumePreparedDraw(badConsumer, sources, {}, error));
    };
    auto rawSignConsumer = consumer;
    rawSignConsumer.draw.draw.registers[0x4806] &= ~(0xFFU << 2);
    rejectsRawConsumer(std::move(rawSignConsumer), "signed RG16 consumer rejects raw sign bits that disagree with metadata");
    auto rawSwizzleConsumer = consumer;
    rawSwizzleConsumer.draw.draw.registers[0x4806 + 3] ^= 1U << 1;
    rejectsRawConsumer(std::move(rawSwizzleConsumer), "signed RG16 consumer rejects raw swizzle that disagrees with metadata");

    auto                      wrongColorProducer = makeProducer(3, 0x2D1);
    auto                      wrongColorInfo     = floatCopy(4, secondRanges.base.address, 0x000402D1);
    NativeGameplayCommandFeed wrongColor(shaders);
    UNSCOPED_INFO("signed RG16 producer rejects an unsupported second producer color encoding");
    REQUIRE((wrongColor.Begin(1, 1, {}, error) && wrongColor.ConsumePreparedDraw(producerA, {}, {}, error) &&
             wrongColor.ConsumePreparedCopy(firstCopy, {}, {}, error) && wrongColor.ConsumePreparedDraw(wrongColorProducer, {}, {}, error) &&
             !wrongColor.ConsumePreparedCopy(wrongColorInfo, {}, {}, error)));

    NativeGameplayCommandFeed missingA(shaders);
    UNSCOPED_INFO("signed RG16 producer rejects a missing A source attachment writer");
    REQUIRE((missingA.Begin(1, 1, {}, error) && !missingA.ConsumePreparedCopy(firstCopy, {}, {}, error)));
    auto                      mismatchedAProducer = makeProducer(1, 1);
    NativeGameplayCommandFeed mismatchedA(shaders);
    UNSCOPED_INFO("signed RG16 producer rejects a mismatched A source attachment writer");
    REQUIRE((mismatchedA.Begin(1, 1, {}, error) && mismatchedA.ConsumePreparedDraw(mismatchedAProducer, {}, {}, error) &&
             !mismatchedA.ConsumePreparedCopy(firstCopy, {}, {}, error)));
    NativeGameplayCommandFeed missingB(shaders);
    UNSCOPED_INFO("signed RG16 producer rejects a missing B source attachment writer");
    REQUIRE((missingB.Begin(1, 1, {}, error) && missingB.ConsumePreparedDraw(producerA, {}, {}, error) &&
             missingB.ConsumePreparedCopy(firstCopy, {}, {}, error) && !missingB.ConsumePreparedCopy(secondCopy, {}, {}, error)));
    auto                      mismatchedBProducer = makeProducer(3, 0);
    NativeGameplayCommandFeed mismatchedB(shaders);
    UNSCOPED_INFO("signed RG16 producer rejects a mismatched B source attachment writer");
    REQUIRE((mismatchedB.Begin(1, 1, {}, error) && mismatchedB.ConsumePreparedDraw(producerA, {}, {}, error) &&
             mismatchedB.ConsumePreparedCopy(firstCopy, {}, {}, error) && mismatchedB.ConsumePreparedDraw(mismatchedBProducer, {}, {}, error) &&
             !mismatchedB.ConsumePreparedCopy(secondCopy, {}, {}, error)));

    const auto rejectsUnsupportedSource = [&](std::vector<DrawCaptureTexture> badSources, const char* message)
    {
        NativeGameplayCommandFeed unsupported(shaders);
        UNSCOPED_INFO(error);
        REQUIRE(unsupported.Begin(1, 1, {}, error));
        UNSCOPED_INFO(error);
        REQUIRE(unsupported.ConsumePreparedDraw(producerA, {}, {}, error));
        UNSCOPED_INFO(error);
        REQUIRE(unsupported.ConsumePreparedCopy(firstCopy, {}, {}, error));
        UNSCOPED_INFO(error);
        REQUIRE(unsupported.ConsumePreparedDraw(producerB, {}, {}, error));
        UNSCOPED_INFO(error);
        REQUIRE(unsupported.ConsumePreparedCopy(secondCopy, {}, {}, error));
        UNSCOPED_INFO(message);
        REQUIRE(!unsupported.ConsumePreparedDraw(makeConsumer(5, true, false), badSources, {}, error));
    };
    auto packedSources                 = sources;
    packedSources[0].guest_packed_mips = true;
    rejectsUnsupportedSource(std::move(packedSources), "signed native consumer rejects packed mips");
    auto arraySources                         = sources;
    arraySources[0].guest_depth_or_array_size = 2;
    rejectsUnsupportedSource(std::move(arraySources), "signed native consumer rejects array textures");
    auto mipSources               = sources;
    mipSources[0].host_mip_levels = 2;
    rejectsUnsupportedSource(std::move(mipSources), "signed native consumer rejects mipmapped host textures");
    auto depthSources                        = sources;
    depthSources[0].host_depth_or_array_size = 2;
    rejectsUnsupportedSource(std::move(depthSources), "signed native consumer rejects host depth textures");

    NativeGameplayCommandFeed clearForm(shaders);
    auto                      clearCopy = firstCopy;
    clearCopy.draw.draw.registers[0x2318] |= 1U << 8;
    UNSCOPED_INFO("signed RG16 float producer rejects clear form");
    REQUIRE((clearForm.Begin(1, 1, {}, error) && clearForm.ConsumePreparedDraw(producerA, {}, {}, error) &&
             !clearForm.ConsumePreparedCopy(clearCopy, {}, {}, error)));

    NativeGameplayCommandFeed missing(shaders);
    UNSCOPED_INFO("signed native consumer rejects a missing same-frame producer");
    REQUIRE((missing.Begin(1, 1, {}, error) && !missing.ConsumePreparedDraw(makeConsumer(1, true, false), sources, {}, error) &&
             error.find("producer page") != std::string::npos));

    NativeGameplayCommandFeed malformed(shaders);
    UNSCOPED_INFO(error);
    REQUIRE(malformed.Begin(1, 1, {}, error));
    UNSCOPED_INFO(error);
    REQUIRE(malformed.ConsumePreparedDraw(producerA, {}, {}, error));
    UNSCOPED_INFO(error);
    REQUIRE(malformed.ConsumePreparedCopy(firstCopy, {}, {}, error));
    UNSCOPED_INFO(error);
    REQUIRE(malformed.ConsumePreparedDraw(producerB, {}, {}, error));
    UNSCOPED_INFO(error);
    REQUIRE(malformed.ConsumePreparedCopy(secondCopy, {}, {}, error));
    const bool malformedRejected = malformed.ConsumePreparedDraw(makeConsumer(5, true, true), sources, {}, error);
    UNSCOPED_INFO("signed native consumer rejects changed physical extent coverage: " + error);
    REQUIRE((!malformedRejected && error.find("source range") != std::string::npos));

    NativeGameplayCommandFeed wrongMetadata(shaders);
    UNSCOPED_INFO("signed native consumer rejects incorrect sign metadata and mapping");
    REQUIRE((wrongMetadata.Begin(1, 1, {}, error) && wrongMetadata.ConsumePreparedDraw(producerA, {}, {}, error) &&
             wrongMetadata.ConsumePreparedCopy(firstCopy, {}, {}, error) && wrongMetadata.ConsumePreparedDraw(producerB, {}, {}, error) &&
             wrongMetadata.ConsumePreparedCopy(secondCopy, {}, {}, error) && !wrongMetadata.ConsumePreparedDraw(makeConsumer(5, false, false), sources, {}, error)));
}

// Shared indices count wrapper/capacity once; batching counts their bytes once.
void sharedIndexOwners()
{
    std::string                error;
    auto                       recipe = draw(4, 4, 4, 9);
    std::vector<std::uint32_t> values(recipe.indices.begin(), recipe.indices.end());
    values.reserve(1U << 20);
    recipe.indices.clear();
    recipe.sharedIndices         = std::make_shared<const NativeDrawReplayIndices>(std::move(values));
    const auto             heavy = sizeof(NativeDrawReplayIndices) + recipe.sharedIndices->Capacity() * sizeof(std::uint32_t);
    detail::OwnerSet       owners;
    detail::OwnedCollector collector{ owners };
    detail::VisitNativeDrawOwnedBytes(recipe, collector.Visitor(), false);
    UNSCOPED_INFO("the owned-byte walk charges a shared index buffer's wrapper and full capacity");
    REQUIRE((recipe.sharedIndices->Capacity() >= (1U << 20) && collector.bytes == heavy));
    const auto copy = recipe;
    detail::VisitNativeDrawOwnedBytes(copy, collector.Visitor(), false);
    UNSCOPED_INFO("a second recipe sharing the index buffer charges it no more");
    REQUIRE(collector.bytes == heavy);

    // Fifty admissions sharing 12 MiB of indices fit the 512 MiB frame bound
    // only when the buffer is charged once.
    recipe.sharedIndices = std::make_shared<const NativeDrawReplayIndices>(std::vector<std::uint32_t>(3U << 20));
    recipe.indexCount    = static_cast<std::uint32_t>(recipe.sharedIndices->Values().size());
    NativeRenderFrame frame;
    frame.passes.resize(1);
    for (int index = 0; index < 50; ++index)
    {
        std::shared_ptr<const NativeDrawReplayAdmission> admission;
        UNSCOPED_INFO(error);
        REQUIRE(AdmitNativeDrawReplayRecipe(recipe, admission, error));
        frame.passes[0].admittedDraws.push_back(std::move(admission));
    }
    UNSCOPED_INFO("batching charges a shared index buffer once across admissions: " + error);
    REQUIRE(BatchNativeRenderFrame(frame, error));

    // Prefix validation charges it once across raw draws too.
    NativeRenderFrame raw;
    raw.passes.resize(1);
    raw.passes[0].draws.assign(50, recipe);
    NativeRenderFramePrefixState state;
    UNSCOPED_INFO("prefix validation charges raw draws' shared index buffer once: " + error);
    REQUIRE(state.Append(raw, error));
}

void admittedBudget(bool versioned, bool textureVersioned = false, bool distinctTextures = false, bool bindingStorageBoundary = false, bool sharedIndices = false)
{
    constexpr std::size_t maximum = 512U * 1024U * 1024U;
    std::string           error;
    auto                  recipe   = draw(4, 4, 4, 9);
    auto                  geometry = std::make_shared<const std::vector<std::uint8_t>>(16U * 1024U * 1024U);
    recipe.vertexData.clear();
    recipe.sharedVertexData = geometry;
    if (sharedIndices)
    {
        // 12 MiB of indices both admissions share, charged once.
        recipe.sharedIndices = std::make_shared<const NativeDrawReplayIndices>(std::vector<std::uint32_t>(3U << 20));
        recipe.indices.clear();
        recipe.indexCount = static_cast<std::uint32_t>(recipe.sharedIndices->Values().size());
    }
    recipe.sharedVertexShaderDxil = std::make_shared<const std::vector<std::uint8_t>>(std::move(recipe.vertexShaderDxil));
    recipe.sharedPixelShaderDxil  = std::make_shared<const std::vector<std::uint8_t>>(std::move(recipe.pixelShaderDxil));
    if (versioned)
    {
        const std::vector<std::uint32_t> words{ 0 };
        NativeGameplayDrawView           view;
        view.vertexMicrocode = view.pixelMicrocode = words;
        NativeGameplayShaderValidationCache validation;
        UNSCOPED_INFO(error);
        REQUIRE(validation.Validate(view, targetShader(4, 0), error));
        recipe.sharedVertexShaderDxil = validation.VertexBytecode();
        recipe.sharedPixelShaderDxil  = validation.PixelBytecode();
        recipe.shaderVersion          = validation.Version();
    }
    if (textureVersioned)
    {
        recipe.textureMask       = 1;
        recipe.textures[0].width = recipe.textures[0].height = 1;
        recipe.textures[0].format                            = NativeDrawReplayTextureFormat::R8;
        recipe.textures[0].bytes                             = { 0x7F };
        UNSCOPED_INFO(error);
        REQUIRE(CopyNativeDrawReplayTextureVersion(recipe.textures[0], error));
    }
    std::shared_ptr<const NativeDrawReplayAdmission> first, second;
    UNSCOPED_INFO(error);
    REQUIRE(AdmitNativeDrawReplayRecipe(recipe, first, error));
    if (distinctTextures)
    {
        UNSCOPED_INFO(error);
        REQUIRE(CopyNativeDrawReplayTextureVersion(recipe.textures[0], error));
    }
    UNSCOPED_INFO(error);
    REQUIRE(AdmitNativeDrawReplayRecipe(std::move(recipe), second, error));
    if (textureVersioned)
    {
        UNSCOPED_INFO("budget cases retain either one shared version or two separately allocated immutable versions");
        REQUIRE((first->Recipe().textures[0].textureVersion != second->Recipe().textures[0].textureVersion) == distinctTextures);
    }
    const auto ownedBytes = [](const NativeDrawReplayAdmission& admission)
    {
        const auto& recipe = admission.Recipe();
        return admission.MetadataBytes() + recipe.indices.size() * sizeof(std::uint32_t) + recipe.samplers.size() * sizeof(NativeDrawReplaySampler) +
               recipe.vertexConstants.size() + recipe.pixelConstants.size() + recipe.sharedConstants.size();
    };
    const auto textureBytes = [](const NativeDrawReplayAdmission& admission)
    {
        const auto& texture = admission.Recipe().textures[0];
        return texture.textureVersion ? texture.textureVersion->MetadataBytes() + texture.payloadBytes().size() : 0;
    };
    const auto                                       canonicalBytes = ownedBytes(*first) + ownedBytes(*second) + geometry->size() + (sharedIndices ? first->Recipe().indexSpan().size_bytes() : 0) +
                                                                      first->Recipe().vertexShaderBytes().size() + first->Recipe().pixelShaderBytes().size() +
                                                                      (versioned ? sizeof(NativeDrawReplayShaderVersion) : 0) + textureBytes(*first) + (distinctTextures ? textureBytes(*second) : 0);
    std::shared_ptr<const NativeDrawReplayAdmission> finalTextureOwner;
    if (textureVersioned)
    {
        auto finalRecipe = first->Recipe();
        if (bindingStorageBoundary)
        {
            finalRecipe.textureMask = std::numeric_limits<std::uint32_t>::max();
            finalRecipe.textures.fill(finalRecipe.textures[0]);
        }
        else
        {
            UNSCOPED_INFO(error);
            REQUIRE(CopyNativeDrawReplayTextureVersion(finalRecipe.textures[0], error));
        }
        UNSCOPED_INFO(error);
        REQUIRE(AdmitNativeDrawReplayRecipe(std::move(finalRecipe), finalTextureOwner, error));
    }
    NativeRenderFrame packet;
    packet.passes.resize(1);
    auto& pass         = packet.passes[0];
    pass.admittedDraws = { first, second };
    NativeRenderFramePass::Clear clear;
    clear.color     = true;
    clear.rectangle = { 0, 0, 4, 4 };
    pass.clears.assign(NativeRenderBatchCommands - 2, clear);
    const auto                   baseBytes         = sizeof(NativeRenderFrame) + sizeof(NativeRenderFramePass) +
                                                     pass.admittedDraws.size() * sizeof(std::shared_ptr<const NativeDrawReplayAdmission>);
    const auto                   packetBytes       = baseBytes + pass.clears.size() * sizeof(clear);
    const auto                   finalBindingBytes = bindingStorageBoundary ? finalTextureOwner->MetadataBytes() - sizeof(NativeDrawReplayAdmission) : 0;
    const auto                   finalBytes        = sizeof(NativeRenderFrame) + sizeof(NativeRenderFramePass) + sizeof(std::shared_ptr<const NativeDrawReplayAdmission>) +
                                                     ownedBytes(finalTextureOwner ? *finalTextureOwner : *first) - finalBindingBytes;
    const auto                   fullPackets       = (maximum - canonicalBytes - (versioned || textureVersioned ? baseBytes + finalBytes : 0)) / packetBytes;
    NativeRenderFramePrefixState state;
    for (std::size_t index = 0; index < fullPackets; ++index)
    {
        UNSCOPED_INFO("canonical operations and all shared payloads are charged once across requests: " + error);
        REQUIRE(state.Append(packet, error));
    }
    auto remaining = maximum - canonicalBytes - fullPackets * packetBytes;
    // Fills the residue above the final packet's `finalBytes` to less than
    // one clear. Where the residue lands depends on sizeof(recipe); one the
    // batch cap cannot fill first takes a half-capped packet.
    const auto fillToFinal = [&](const std::string& message)
    {
        const auto cap = pass.clears.size();
        if ((remaining - baseBytes - finalBytes) / sizeof(clear) > cap)
        {
            pass.clears.resize(cap / 2);
            UNSCOPED_INFO(message + error);
            REQUIRE(state.Append(packet, error));
            remaining -= baseBytes + pass.clears.size() * sizeof(clear);
        }
        pass.clears.resize((remaining - baseBytes - finalBytes) / sizeof(clear));
        UNSCOPED_INFO(message + error);
        REQUIRE((pass.clears.size() <= cap && state.Append(packet, error)));
        remaining -= baseBytes + pass.clears.size() * sizeof(clear);
        UNSCOPED_INFO("the final packet's gap is below one clear whatever the recipe size");
        REQUIRE(remaining - finalBytes < sizeof(clear));
    };
    if (textureVersioned)
    {
        fillToFinal("deduplicated texture versions retain exact cumulative metadata charges: ");
        const auto        gap = remaining - finalBytes;
        NativeRenderFrame finalPacket;
        finalPacket.passes.resize(1);
        finalPacket.passes[0].admittedDraws = { finalTextureOwner };
        if (bindingStorageBoundary)
        {
            UNSCOPED_INFO("only the final owner's retained bindings cross the real 512 MiB bound with all resource owners already charged");
            REQUIRE((finalTextureOwner->TextureBindings().size() == kNativeDrawReplayTextureSlots &&
                     finalTextureOwner->Recipe().textures[0].textureVersion == first->Recipe().textures[0].textureVersion &&
                     gap < finalBindingBytes && !state.Append(finalPacket, error) && error.find("owned byte bound") != std::string::npos));
            return;
        }
        UNSCOPED_INFO("a fresh texture wrapper and tight layout crossing the real 512 MiB bound is rejected");
        REQUIRE((gap < finalTextureOwner->Recipe().textures[0].textureVersion->MetadataBytes() &&
                 !state.Append(finalPacket, error) && error.find("owned byte bound") != std::string::npos));
        return;
    }
    if (versioned)
    {
        fillToFinal("repeated version and DXIL owners are charged once: ");
        const auto gap         = remaining - finalBytes;
        const auto extra       = (gap / 16 + 1) * 16;
        auto       finalRecipe = first->Recipe();
        finalRecipe.sharedConstants.resize(finalRecipe.sharedConstants.size() + extra);
        std::shared_ptr<const NativeDrawReplayAdmission> last;
        UNSCOPED_INFO(error);
        REQUIRE(AdmitNativeDrawReplayRecipe(std::move(finalRecipe), last, error));
        NativeRenderFrame finalPacket;
        finalPacket.passes.resize(1);
        finalPacket.passes[0].admittedDraws = { last };
        UNSCOPED_INFO("the one retained version wrapper participates in exact cumulative storage rejection");
        REQUIRE((extra > gap && extra - gap <= sizeof(NativeDrawReplayShaderVersion) &&
                 !state.Append(finalPacket, error) && error.find("owned byte bound") != std::string::npos));
        return;
    }
    if (remaining >= baseBytes)
    {
        pass.clears.resize((remaining - baseBytes) / sizeof(clear));
        UNSCOPED_INFO(error);
        REQUIRE(state.Append(packet, error));
        remaining -= baseBytes + pass.clears.size() * sizeof(clear);
    }
    pass.clears.clear();
    UNSCOPED_INFO("every retained handle and each canonical owner's private storage count in the cumulative byte bound");
    REQUIRE((remaining < baseBytes && !state.Append(packet, error) && error.find("owned byte bound") != std::string::npos));
}

void preparationRetirementGate()
{
    using namespace std::chrono_literals;
    std::string                      error;
    NativeGameplayGeometryCache      cache(preparation::Budget);
    NativeGameplayPreparationContext first;
    NativeGameplayCommandFeed        producer({}, &cache, first);
    UNSCOPED_INFO(error);
    REQUIRE((producer.Begin(1, 1, {}, error) && producer.Consume(clearEvent(1), {}, {}, error) && producer.Consume(copyEvent(2), {}, {}, error) && producer.Consume(copyEvent(3, true), {}, {}, error)));
    auto                                       terminal = terminalEvent(4);
    const auto                                 boundary = snapshotWithEvents({ terminal });
    NativeRenderFrame                          frame;
    std::vector<NativeGameplayRetainedTexture> bank;
    UNSCOPED_INFO(error);
    REQUIRE(producer.Finish(terminal, boundary.final_swap, 2, 1, frame, bank, error));
    bank.reserve(32);
    const auto receipt = producer.PreparationReceipt(frame, bank);
    UNSCOPED_INFO("the feed receipt covers the actual caller bank allocation, including slack");
    REQUIRE((!preparation::SupportedAbi || receipt.known && receipt.callerBankBytes == preparation::Vector(bank) && bank.capacity() > bank.size()));
    std::vector<std::uint64_t> exports;
    exports.reserve(32);
    exports.push_back(1);
    auto supplemented = receipt;
    supplemented.RetainCallerStorage(preparation::Vector(exports));
    UNSCOPED_INFO("a caller sidecar adds its actual capacity to the successful aggregate receipt and nonrefundable bank scalar");
    REQUIRE((supplemented.callerBankBytes == receipt.callerBankBytes + preparation::Vector(exports) &&
             (!receipt.known || supplemented.known && supplemented.bytes == receipt.bytes + preparation::Vector(exports))));

    struct Coordinator
    {
        std::promise<NativeGameplayPreparationStatus>       promise;
        std::shared_future<NativeGameplayPreparationStatus> future = promise.get_future().share();
        std::atomic<unsigned>                               waits{ 0 };
    } coordinator;

    NativeGameplayPreparationContext context{ receipt, NativeGameplayPreparationStatus::Pending, 0, &coordinator, [](void* value, bool wait, std::string&)
                                              {
                                                  auto& owner = *static_cast<Coordinator*>(value);
                                                  if (wait)
                                                      ++owner.waits;
                                                  if (!wait && owner.future.wait_for(0ms) != std::future_status::ready)
                                                      return NativeGameplayPreparationStatus::Pending;
                                                  return owner.future.get();
                                              } };
    auto preparing = std::async(std::launch::async, [&]
                                {
                                    NativeGameplayCommandFeed next({}, &cache, context);
                                    unsigned                  prefixes = 0;
                                    next.SetPrefixConsumer([&](NativeRenderFrame, std::string&)
                                                           {
                                                               ++prefixes;
                                                               return true;
                                                           });
                                    if (!next.Begin(1, 1, bank, error) || !next.Consume(clearEvent(5), {}, {}, error) || !next.Consume(copyEvent(6), {}, {}, error) || !next.Consume(copyEvent(7, true), {}, {}, error))
                                        return false;
                                    NativeRenderFrame finished;
                                    finished.textures.push_back({ 99, 1, 1 });
                                    std::vector<NativeGameplayRetainedTexture> nextBank;
                                    const auto                                 end = terminalEvent(8);
                                    if (!next.Finish(end, boundary.final_swap, 2, 1, finished, nextBank, error))
                                        return false;
                                    return prefixes == 0 && finished.passes.size() == frame.passes.size() && next.PreparationReceipt(finished, nextBank).known == preparation::SupportedAbi;
                                });
    UNSCOPED_INFO("held Recording blocks the complete Finish or an unknown-ABI unit");
    REQUIRE(preparing.wait_for(20ms) == std::future_status::timeout);
    coordinator.promise.set_value(NativeGameplayPreparationStatus::Success);
    UNSCOPED_INFO(error);
    REQUIRE(preparing.get());
    UNSCOPED_INFO("only successful Recording refunds recipe carry, retaining the caller bank scalar");
    REQUIRE((coordinator.waits == 1 && context.receipt.bytes == 0 && context.callerBankBytes == receipt.callerBankBytes));
    bank.clear();
    UNSCOPED_INFO("clearing a caller bank does not release its allocation charge");
    REQUIRE(context.callerBankBytes == receipt.callerBankBytes);
    UNSCOPED_INFO("a repeated successful poll cannot transfer the caller bank scalar twice");
    REQUIRE((context.Settle(false, error) && context.callerBankBytes == receipt.callerBankBytes));
    std::vector<NativeGameplayRetainedTexture>().swap(bank);
    context.callerBankBytes -= receipt.callerBankBytes;
    UNSCOPED_INFO("the coordinator releases the scalar once after actual bank allocation destruction");
    REQUIRE((bank.capacity() == 0 && context.Settle(false, error) && context.callerBankBytes == 0));

    for (auto failure : { NativeGameplayPreparationStatus::Failed, NativeGameplayPreparationStatus::Stopped })
    {
        NativeGameplayPreparationContext rejected{ receipt, NativeGameplayPreparationStatus::Pending, 0, &failure, [](void* value, bool, std::string&)
                                                   {
                                                       return *static_cast<NativeGameplayPreparationStatus*>(value);
                                                   } };
        bool closed = false;
        try
        {
            NativeGameplayCommandFeed next({}, &cache, rejected);
        }
        catch (const std::runtime_error&)
        {
            closed = true;
        }
        UNSCOPED_INFO("a ready failed or stopped Recording closes constructor admission without a refund");
        REQUIRE((closed && rejected.receipt.bytes == receipt.bytes));
    }
}

void fundedAggregateDrawPressure()
{
    using namespace std::chrono_literals;
    if (!preparation::SupportedAbi)
        return;
    std::string error;
    auto        shader       = targetShader(4, 0);
    shader.textureFetchSlots = { 1, 2, 3, 4, 5 };
    shader.textureDimensions.assign(5, NativeDrawReplayTextureDimension::Texture2D);
    const std::vector<NativeGameplayShaderInterface>           shaders{ shader };
    auto                                                       event = targetDrawEvent(1, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0);
    std::vector<rex::graphics::diagnostic::DrawCaptureTexture> textures(5);
    std::map<std::uint32_t, NativeDrawReplayTexture>           cpu;
    for (std::uint32_t index = 0; index < 5; ++index)
    {
        auto& texture  = cpu[index];
        texture.width  = 4096;
        texture.height = 3840;
        texture.bytes.resize(60U * 1024U * 1024U);
        UNSCOPED_INFO(error);
        REQUIRE(CopyNativeDrawReplayTextureVersion(texture, error));
        textures[index].guest_base_page = 0x300 + index;
        textures[index].availability    = rex::graphics::diagnostic::DrawCaptureAvailability::kUnavailable;
        rex::graphics::diagnostic::DrawCaptureTextureUse use;
        use.texture_index     = index;
        use.fetch_constant    = index + 1;
        use.dimension         = 1;
        use.component_mapping = 0x1688;
        use.data_available    = true;
        use.availability      = rex::graphics::diagnostic::DrawCaptureAvailability::kComplete;
        event.draw.texture_uses.push_back(use);
    }
    NativeGameplayGeometryCache      cache(preparation::Budget);
    NativeGameplayPreparationContext first;
    NativeGameplayCommandFeed        producer(shaders, &cache, first);
    auto                             repeated = event;
    repeated.id                               = 2;
    UNSCOPED_INFO(error);
    REQUIRE((producer.Begin(1, 1, {}, error) && producer.Consume(event, textures, cpu, error) && producer.Consume(repeated, textures, cpu, error) && producer.Consume(copyEvent(3), {}, {}, error) && producer.Consume(copyEvent(4, true), {}, {}, error)));
    const auto                                 terminal = terminalEvent(5);
    const auto                                 boundary = snapshotWithEvents({ terminal });
    NativeRenderFrame                          frame;
    std::vector<NativeGameplayRetainedTexture> bank;
    UNSCOPED_INFO(error);
    REQUIRE(producer.Finish(terminal, boundary.final_swap, 2, 1, frame, bank, error));
    const auto receipt = producer.PreparationReceipt(frame, bank);
    UNSCOPED_INFO("multiple supported bounded textures create a real aggregate predecessor charge");
    REQUIRE((receipt.known && receipt.bytes >= 300U * 1024U * 1024U && receipt.bytes < preparation::Budget));
    const auto decoded = frame.passes[0].Draw(1).sharedVertexData;
    UNSCOPED_INFO("the predecessor's reused draw owns a decoded cache payload");
    REQUIRE((decoded && cache.ManagedBytes() > 0));
    cache.SetBudget(0);
    UNSCOPED_INFO("cache eviction leaves the predecessor's immutable decoded owner alive");
    REQUIRE((cache.ManagedBytes() == 0 && !decoded->empty()));

    struct Pressure
    {
        std::promise<void>       waiting, released;
        std::shared_future<void> release = released.get_future().share();
        std::atomic<unsigned>    waits{ 0 };
    } pressure;

    auto                             waiting = pressure.waiting.get_future();
    NativeGameplayPreparationContext context{ receipt, NativeGameplayPreparationStatus::Pending, 0, &pressure, [](void* value, bool wait, std::string&)
                                              {
                                                  auto& owner = *static_cast<Pressure*>(value);
                                                  if (!wait)
                                                      return NativeGameplayPreparationStatus::Pending;
                                                  if (++owner.waits == 1)
                                                      owner.waiting.set_value();
                                                  owner.release.wait();
                                                  return NativeGameplayPreparationStatus::Success;
                                              } };
    NativeGameplayCommandFeed consumer(shaders, &cache, context);
    UNSCOPED_INFO(error);
    REQUIRE(consumer.Begin(1, 1, bank, error));
    auto consuming = std::async(std::launch::async, [&]
                                {
                                    return consumer.Consume(event, textures, cpu, error);
                                });
    UNSCOPED_INFO("aggregate pressure waits before any operation entry, even when each frame fits separately");
    REQUIRE((waiting.wait_for(2s) == std::future_status::ready && consuming.wait_for(20ms) == std::future_status::timeout && consumer.Draw(event.id) == nullptr));
    UNSCOPED_INFO("cache eviction cannot refund the immutable predecessor receipt");
    REQUIRE(context.receipt.bytes == receipt.bytes);
    pressure.released.set_value();
    UNSCOPED_INFO(error);
    REQUIRE((consuming.get() && pressure.waits == 1 && consumer.Draw(event.id) && consumer.Consume(copyEvent(2), {}, {}, error) && consumer.Consume(copyEvent(3, true), {}, {}, error)));
    NativeRenderFrame                          second;
    std::vector<NativeGameplayRetainedTexture> next;
    UNSCOPED_INFO("the pressured original draw executes exactly once after successful Recording: " + error);
    REQUIRE((consumer.Finish(terminal, boundary.final_swap, 2, 1, second, next, error) && second.passes.size() == frame.passes.size() && second.passes[0].DrawCount() == 1));
    UNSCOPED_INFO("the next successful receipt retains every distinct CPU owner");
    REQUIRE(consumer.PreparationReceipt(second, next).bytes >= 300U * 1024U * 1024U);
}

struct StorageCheckpoint
{
    std::size_t live = 0, peak = 0;
};

void preparationUnknownAndEarlySuccess()
{
    using namespace std::chrono_literals;
    std::string                      error;
    NativeGameplayGeometryCache      cache(preparation::Budget);
    NativeGameplayPreparationContext first;
    NativeGameplayCommandFeed        producer({}, &cache, first);
    auto                             trigger = copyEvent(2);
    trigger.kind                             = DrawCaptureFrameEvent::Kind::kDraw;
    trigger.host_issued                      = false;
    trigger.draw.draw.primitive_type         = 8;
    UNSCOPED_INFO(error);
    REQUIRE((producer.Begin(1, 1, {}, error) && producer.Consume(clearEvent(1), {}, {}, error) && producer.Consume(trigger, {}, {}, error) && producer.Consume(copyEvent(3), {}, {}, error) && producer.Consume(copyEvent(4, true), {}, {}, error)));
    const auto                                 terminal = terminalEvent(5);
    const auto                                 boundary = snapshotWithEvents({ terminal });
    NativeRenderFrame                          frame;
    std::vector<NativeGameplayRetainedTexture> bank;
    UNSCOPED_INFO("a copied legacy DTO remains Unknown after its trigger flag is cleared: " + error);
    REQUIRE((producer.Finish(terminal, boundary.final_swap, 2, 1, frame, bank, error) && !producer.PreparationReceipt(frame, bank).known));
    unsigned                         waits = 0;
    NativeGameplayPreparationContext unknown{ {}, NativeGameplayPreparationStatus::Pending, 0, &waits, [](void* value, bool wait, std::string&)
                                              {
                                                  if (!wait)
                                                      return NativeGameplayPreparationStatus::Pending;
                                                  ++*static_cast<unsigned*>(value);
                                                  return NativeGameplayPreparationStatus::Success;
                                              } };
    NativeGameplayCommandFeed sequential({}, &cache, unknown);
    UNSCOPED_INFO("Unknown predecessor waits before construction, then preserves sequential input");
    REQUIRE((waits == 1 && sequential.Begin(1, 1, {}, error) && sequential.Consume(clearEvent(1), {}, {}, error)));
    NativeGameplayPreparationContext ready{ {}, NativeGameplayPreparationStatus::Pending, 0, nullptr, [](void*, bool, std::string&)
                                            {
                                                return NativeGameplayPreparationStatus::Success;
                                            } };
    NativeGameplayCommandFeed early({}, &cache, ready);
    unsigned                  prefixes = 0;
    early.SetPrefixConsumer([&](NativeRenderFrame, std::string&)
                            {
                                ++prefixes;
                                return true;
                            });
    UNSCOPED_INFO(error);
    REQUIRE((early.Begin(1, 1, {}, error) && early.Consume(clearEvent(1), {}, {}, error) && early.Consume(copyEvent(2), {}, {}, error)));
    UNSCOPED_INFO("early successful poll preserves deferral for a feed that began unresolved");
    REQUIRE(prefixes == 0);

    if (!preparation::SupportedAbi)
        return;
    UNSCOPED_INFO(error);
    REQUIRE((producer.Begin(1, 1, {}, error) && producer.Consume(clearEvent(1), {}, {}, error) && producer.Consume(copyEvent(2), {}, {}, error) && producer.Consume(copyEvent(3, true), {}, {}, error)));
    const auto knownEnd = terminalEvent(4);
    UNSCOPED_INFO(error);
    REQUIRE(producer.Finish(knownEnd, boundary.final_swap, 2, 1, frame, bank, error));
    const auto knownReceipt = producer.PreparationReceipt(frame, bank);
    UNSCOPED_INFO("the raw-shape test begins with a feed-issued predecessor receipt");
    REQUIRE(knownReceipt.known);
    auto shader              = targetShader(4, 0);
    shader.textureFetchSlots = { 1 };
    shader.textureDimensions = { NativeDrawReplayTextureDimension::Texture2D };
    const std::vector<NativeGameplayShaderInterface> shaders{ shader };
    auto                                             rawDraw = targetDrawEvent(1, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0);
    rex::graphics::diagnostic::DrawCaptureTextureUse use;
    use.texture_index         = 0;
    use.fetch_constant        = 1;
    use.dimension             = 1;
    use.component_mapping     = 0x1688;
    use.data_available        = true;
    use.availability          = rex::graphics::diagnostic::DrawCaptureAvailability::kComplete;
    rawDraw.draw.texture_uses = { use };
    std::vector<rex::graphics::diagnostic::DrawCaptureTexture> sources(1);
    sources[0].guest_base_page = 0x300;
    std::map<std::uint32_t, NativeDrawReplayTexture> cpu;
    cpu[0].width = cpu[0].height = 1;
    cpu[0].format                = NativeDrawReplayTextureFormat::R8;
    cpu[0].bytes                 = { 0x7F };

    struct Pending
    {
        std::promise<void>       waiting, released;
        std::shared_future<void> release = released.get_future().share();
    } pending;

    auto                             waiting = pending.waiting.get_future();
    NativeGameplayPreparationContext held{ knownReceipt, NativeGameplayPreparationStatus::Pending, 0, &pending, [](void* value, bool wait, std::string&)
                                           {
                                               auto& owner = *static_cast<Pending*>(value);
                                               if (!wait)
                                                   return NativeGameplayPreparationStatus::Pending;
                                               owner.waiting.set_value();
                                               owner.release.wait();
                                               return NativeGameplayPreparationStatus::Success;
                                           } };
    NativeGameplayCommandFeed feed(shaders, &cache, held);
    UNSCOPED_INFO(error);
    REQUIRE(feed.Begin(1, 1, {}, error));
    auto consuming = std::async(std::launch::async, [&]
                                {
                                    return feed.Consume(rawDraw, sources, cpu, error);
                                });
    UNSCOPED_INFO("raw caller-owned bytes wait before the original conversion and are not canonicalized by forecasting");
    REQUIRE((waiting.wait_for(2s) == std::future_status::ready && consuming.wait_for(20ms) == std::future_status::timeout && !cpu[0].textureVersion));
    pending.released.set_value();
    UNSCOPED_INFO(error);
    REQUIRE((consuming.get() && feed.Consume(copyEvent(2), {}, {}, error) && feed.Consume(copyEvent(3, true), {}, {}, error)));
    const auto end = terminalEvent(4);
    UNSCOPED_INFO("raw Unknown input converts once through the original owned recipe copy after retirement: " + error);
    REQUIRE((feed.Finish(end, boundary.final_swap, 2, 1, frame, bank, error) && frame.passes[0].DrawCount() == 1 && frame.passes[0].Draw(0).textures[0].bytes == cpu[0].bytes && frame.passes[0].Draw(0).textures[0].bytes.data() != cpu[0].bytes.data()));
}

void preparationGenericSharedOwner()
{
    using namespace std::chrono_literals;
    using Payload = std::vector<std::uint8_t>;
    std::string error;
    auto        shader       = targetShader(4, 0);
    shader.textureFetchSlots = { 1 };
    shader.textureDimensions = { NativeDrawReplayTextureDimension::Texture2D };
    const std::vector<NativeGameplayShaderInterface> shaders{ shader };
    auto                                             event = targetDrawEvent(1, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0);
    rex::graphics::diagnostic::DrawCaptureTextureUse use;
    use.texture_index       = 0;
    use.fetch_constant      = 1;
    use.dimension           = 1;
    use.component_mapping   = 0x1688;
    use.data_available      = true;
    use.availability        = rex::graphics::diagnostic::DrawCaptureAvailability::kComplete;
    event.draw.texture_uses = { use };
    std::vector<rex::graphics::diagnostic::DrawCaptureTexture> sources(1);
    sources[0].guest_base_page = 0x300;
    std::map<std::uint32_t, NativeDrawReplayTexture> cpu;
    cpu[0].width = cpu[0].height = 1;
    cpu[0].format                = NativeDrawReplayTextureFormat::R8;
    // Valid callers can supply control blocks larger than the private factory's
    // combined vector/control allocation; sequential admission still accepts it.
    const auto deleter = [state = std::array<std::uint8_t, 4096>{}](const Payload* payload)
    {
        static_cast<void>(state);
        delete payload;
    };
    cpu[0].sharedBytes = std::shared_ptr<const Payload>(new Payload{ 0x7F }, deleter);
    NativeGameplayGeometryCache      cache(preparation::Budget);
    NativeGameplayPreparationContext first;
    NativeGameplayCommandFeed        producer(shaders, &cache, first);
    UNSCOPED_INFO(error);
    REQUIRE((producer.Begin(1, 1, {}, error) && producer.Consume(event, sources, cpu, error) && producer.Consume(copyEvent(2), {}, {}, error) && producer.Consume(copyEvent(3, true), {}, {}, error)));
    const auto                                 end      = terminalEvent(4);
    const auto                                 boundary = snapshotWithEvents({ end });
    NativeRenderFrame                          frame;
    std::vector<NativeGameplayRetainedTexture> bank;
    UNSCOPED_INFO(error);
    REQUIRE(producer.Finish(end, boundary.final_swap, 2, 1, frame, bank, error));
    bank.reserve(32);
    const auto receipt = producer.PreparationReceipt(frame, bank);
    UNSCOPED_INFO("generic shared ownership keeps sequential input unchanged and the successful receipt Unknown with actual bank capacity");
    REQUIRE((!receipt.known && receipt.bytes == 0 && receipt.basis == preparation::Basis && receipt.callerBankBytes == preparation::Vector(bank) &&
             frame.passes[0].Draw(0).textures[0].sharedBytes == cpu[0].sharedBytes && !cpu[0].textureVersion));

    struct Pending
    {
        std::promise<void>       waiting, released;
        std::shared_future<void> release = released.get_future().share();
        std::atomic<unsigned>    waits{ 0 };
    } pending;

    auto                             waiting = pending.waiting.get_future();
    NativeGameplayPreparationContext held{ receipt, NativeGameplayPreparationStatus::Pending, 0, &pending, [](void* value, bool wait, std::string&)
                                           {
                                               auto& owner = *static_cast<Pending*>(value);
                                               if (!wait)
                                                   return NativeGameplayPreparationStatus::Pending;
                                               if (++owner.waits == 1)
                                                   owner.waiting.set_value();
                                               owner.release.wait();
                                               return NativeGameplayPreparationStatus::Success;
                                           } };
    std::atomic<bool> constructed{ false };
    auto              preparing = std::async(std::launch::async, [&]
                                             {
                                    NativeGameplayCommandFeed next(shaders, &cache, held);
                                    constructed.store(true, std::memory_order_release);
                                    if (!next.Begin(1, 1, {}, error) || !next.Consume(event, sources, cpu, error) || !next.Draw(event.id) || !next.Consume(copyEvent(2), {}, {}, error) || !next.Consume(copyEvent(3, true), {}, {}, error))
                                        return false;
                                    NativeRenderFrame                          terminal;
                                    std::vector<NativeGameplayRetainedTexture> nextBank;
                                    return next.Finish(end, boundary.final_swap, 2, 1, terminal, nextBank, error) && terminal.passes[0].DrawCount() == 1 && !next.PreparationReceipt(terminal, nextBank).known;
                                             });
    UNSCOPED_INFO("unproved generic predecessor ownership waits before the successor constructor's whole unit");
    REQUIRE((waiting.wait_for(2s) == std::future_status::ready && !constructed.load(std::memory_order_acquire) && preparing.wait_for(20ms) == std::future_status::timeout));
    pending.released.set_value();
    const auto prepared = preparing.get();
    UNSCOPED_INFO("successful Recording releases the whole successor once while the real caller bank stays charged: " + error);
    REQUIRE((prepared && pending.waits == 1 && held.callerBankBytes == receipt.callerBankBytes));

    NativeGameplayPreparationReceipt overflow{ true, preparation::Unknown - 8, receipt.callerBankBytes, preparation::Basis };
    overflow.RetainCallerStorage(9);
    UNSCOPED_INFO("aggregate receipt overflow becomes Unknown while preserving the added nonrefundable caller storage");
    REQUIRE((!overflow.known && overflow.bytes == 0 && overflow.callerBankBytes == receipt.callerBankBytes + 9 && overflow.basis == preparation::Basis));
}

template <class T>
struct StorageAllocator
{
    using value_type = T;
    StorageCheckpoint* checkpoint;

    explicit StorageAllocator(StorageCheckpoint* value)
    : checkpoint(value)
    {
    }

    template <class U>
    StorageAllocator(const StorageAllocator<U>& value)
    : checkpoint(value.checkpoint)
    {
    }

    T* allocate(std::size_t count)
    {
        auto* result = std::allocator<T>{}.allocate(count);
        checkpoint->live += count * sizeof(T);
        checkpoint->peak = std::max(checkpoint->peak, checkpoint->live);
        return result;
    }

    void deallocate(T* value, std::size_t count) noexcept
    {
        checkpoint->live -= count * sizeof(T);
        std::allocator<T>{}.deallocate(value, count);
    }

    template <class U>
    bool operator==(const StorageAllocator<U>& value) const noexcept
    {
        return checkpoint == value.checkpoint;
    }
};

void preparationStorageRequests()
{
    using namespace preparation;
    if (!SupportedAbi)
        return;
    StorageCheckpoint vectorCheckpoint;
    {
        std::vector<std::uint32_t, StorageAllocator<std::uint32_t>> values{ StorageAllocator<std::uint32_t>(&vectorCheckpoint) };
        values.resize(8);
        const auto prior = vectorCheckpoint.live;
        const auto grant = Pushes<std::uint32_t>(values.capacity(), values.size(), 1);
        values.push_back(9);
        UNSCOPED_INFO("the grant covers the observed old/new vector allocation coexistence");
        REQUIRE((values.capacity() == 12 && Add(prior, grant) >= vectorCheckpoint.peak));
    }
    UNSCOPED_INFO("typed vector storage retires after destruction");
    REQUIRE(vectorCheckpoint.live == 0);
    StorageCheckpoint hashCheckpoint;
    {
        using Set = std::unordered_set<std::uint32_t, std::hash<std::uint32_t>, std::equal_to<std::uint32_t>, StorageAllocator<std::uint32_t>>;
        Set        values{ 0U, std::hash<std::uint32_t>{}, std::equal_to<std::uint32_t>{}, StorageAllocator<std::uint32_t>(&hashCheckpoint) };
        const auto prior = Hash<std::uint32_t>(values.size(), values.bucket_count());
        const auto grant = HashInsertions<std::uint32_t>(values.size(), values.bucket_count(), 513);
        for (std::uint32_t value = 0; value < 513; ++value)
        {
            values.insert(value);
            if (value == 8 || value == 64 || value == 512)
            {
                UNSCOPED_INFO("the held hash growth changes at the source-derived thresholds");
                REQUIRE(values.bucket_count() == (value == 8 ? 64 : value == 64 ? 512
                                                                                : 1024));
            }
        }
        UNSCOPED_INFO("hash node and full replacement bucket forecasts cover actual allocation coexistence");
        REQUIRE(Add(prior, grant) >= hashCheckpoint.peak);
    }
    UNSCOPED_INFO("hash heads, nodes and buckets retire together");
    REQUIRE(hashCheckpoint.live == 0);
    StorageCheckpoint treeCheckpoint;
    {
        using Pair = std::pair<const std::uint32_t, std::uint64_t>;
        std::map<std::uint32_t, std::uint64_t, std::less<std::uint32_t>, StorageAllocator<Pair>> values{ std::less<std::uint32_t>{}, StorageAllocator<Pair>(&treeCheckpoint) };
        values.emplace(1, 2);
        UNSCOPED_INFO("tree accounting includes the actual sentinel and inserted node");
        REQUIRE(Tree<Pair>(1) >= treeCheckpoint.peak);
    }
    UNSCOPED_INFO("tree sentinel ownership ends only at destruction");
    REQUIRE(treeCheckpoint.live == 0);
    StorageCheckpoint stringCheckpoint;
    {
        std::basic_string<char, std::char_traits<char>, StorageAllocator<char>> value{ StorageAllocator<char>(&stringCheckpoint) };
        value.assign(64, 'a');
        const auto prior      = stringCheckpoint.live;
        stringCheckpoint.peak = prior;
        const auto grant      = StringGrowth<char>(value.capacity(), 1000);
        value.assign(1000, 'b');
        UNSCOPED_INFO("string growth grants its terminator and the coexisting prior digest buffer");
        REQUIRE(Add(prior, grant) >= stringCheckpoint.peak);
    }
    UNSCOPED_INFO("string storage retires after the owning buffer");
    REQUIRE(stringCheckpoint.live == 0);
}

void preparationLayoutAndCapacity()
{
    std::size_t                         bytes = 0, count = 0;
    const NativeDrawReplayTextureLayout layout{ 8, 8, NativeDrawReplayTextureFormat::Rgba8, 4, NativeDrawReplayTextureDimension::TextureCube };
    UNSCOPED_INFO("allocation-free sizing covers every cube slice and mip");
    REQUIRE((SizeNativeDrawReplayTextureLayout(layout, bytes, count) && bytes == 2040 && count == 24));
    std::vector<std::vector<std::uint8_t>>     payloads;
    std::vector<std::span<const std::uint8_t>> sources;
    payloads.reserve(count);
    sources.reserve(count);
    for (std::size_t slice = 0; slice < 6; ++slice)
        for (std::size_t mip = 0; mip < 4; ++mip)
            payloads.emplace_back((8U >> mip) * (8U >> mip) * 4);
    for (const auto& payload : payloads)
        sources.push_back(payload);
    NativeDrawReplayTexture texture;
    std::string             error;
    UNSCOPED_INFO("forecast sizing agrees with the original two-table pack/adopt path: " + error);
    REQUIRE((PackNativeDrawReplayTextureVersion(layout, sources, texture, error) && texture.payloadBytes().size() == bytes && texture.textureVersion->Subresources().size() == count));
    auto invalid   = layout;
    invalid.height = 4;
    UNSCOPED_INFO("non-square cube input takes whole-unit fallback");
    REQUIRE(!SizeNativeDrawReplayTextureLayout(invalid, bytes, count));
    UNSCOPED_INFO("held growth and allocator alignment include requested coexistence storage");
    REQUIRE((preparation::Allocation<std::uint8_t>(4095) == 4095 && preparation::Allocation<std::uint8_t>(4096) == 4135 && preparation::Grow(8, 9) == 12));
    if (!preparation::SupportedAbi)
        return;
    NativeGameplayShaderValidationCache validation;
    std::vector<std::uint32_t>          words(4096, 0);
    auto                                shader = targetShader(4, 0);
    NativeGameplayDrawView              view;
    view.vertexMicrocode = view.pixelMicrocode = words;
    auto large                                 = shader;
    large.vertexGuestWordDigest = large.pixelGuestWordDigest = rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(words[0])));
    UNSCOPED_INFO(error);
    REQUIRE(validation.Validate(view, large, error));
    words.resize(1);
    view.vertexMicrocode = view.pixelMicrocode = words;
    UNSCOPED_INFO(error);
    REQUIRE(validation.Validate(view, shader, error));
    const auto before = validation.ManagedBytes();
    UNSCOPED_INFO("inherited validator slack, bytecode controls and all digest capacities remain committed after a smaller shader");
    REQUIRE(before >= preparation::Separate<NativeDrawReplayShaderVersion>() + preparation::SharedBytes(4) * 2 + preparation::StringGrowth<char>(15, 64) * 4 + preparation::Allocation<std::uint32_t>(4096) * 2);
    NativeGameplayGeometryCache                      cache(preparation::Budget);
    const std::vector<NativeGameplayShaderInterface> shaders{ shader };
    NativeGameplayPreparationContext                 context;
    NativeGameplayCommandFeed                        feed(shaders, &cache, context);
    auto                                             event = targetDrawEvent(1, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0);
    event.draw.draw.primitive_type                         = 6;
    event.draw.draw.requested_index_count                  = 7;
    event.draw.draw.index.present                          = true;
    event.draw.draw.index.availability                     = rex::graphics::diagnostic::DrawCaptureAvailability::kComplete;
    event.draw.draw.index.format                           = 0;
    event.draw.draw.index.endianness                       = 1;
    event.draw.draw.index.bytes                            = { 0, 0, 0, 1, 0, 2, 0xFF, 0xFF, 0, 0, 0, 1, 0, 2 };
    event.draw.draw.registers[0x2100]                      = 2;
    event.draw.draw.registers[0x2103]                      = 0xFFFF;
    event.draw.draw.registers[0x2205] |= 1U << 21;
    UNSCOPED_INFO(error);
    REQUIRE((feed.Begin(1, 1, {}, error) && feed.Consume(event, {}, {}, error) && feed.Consume(copyEvent(2), {}, {}, error) && feed.Consume(copyEvent(3, true), {}, {}, error)));
    const auto                                 end      = terminalEvent(4);
    const auto                                 boundary = snapshotWithEvents({ end });
    NativeRenderFrame                          frame;
    std::vector<NativeGameplayRetainedTexture> bank;
    UNSCOPED_INFO(error);
    REQUIRE(feed.Finish(end, boundary.final_swap, 2, 1, frame, bank, error));
    const auto& indices = frame.passes[0].Draw(0).indices;
    UNSCOPED_INFO("restart filtering preserves the reserved strip buffer");
    REQUIRE((indices.size() == 6 && indices.capacity() >= 21));
    bank.reserve(NativeRenderBatchCommands);
    const auto                       stripReceipt = feed.PreparationReceipt(frame, bank);
    NativeGameplayGeometryCache      listCache(preparation::Budget);
    NativeGameplayPreparationContext listContext;
    NativeGameplayCommandFeed        listFeed(shaders, &listCache, listContext);
    const auto                       listEvent = targetDrawEvent(1, 0x0A010280, { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 }, 0);
    UNSCOPED_INFO(error);
    REQUIRE((listFeed.Begin(1, 1, {}, error) && listFeed.Consume(listEvent, {}, {}, error) && listFeed.Consume(copyEvent(2), {}, {}, error) && listFeed.Consume(copyEvent(3, true), {}, {}, error)));
    NativeRenderFrame                          listFrame;
    std::vector<NativeGameplayRetainedTexture> listBank;
    UNSCOPED_INFO(error);
    REQUIRE(listFeed.Finish(end, boundary.final_swap, 2, 1, listFrame, listBank, error));
    listBank.reserve(NativeRenderBatchCommands);
    const auto  listReceipt       = listFeed.PreparationReceipt(listFrame, listBank);
    const auto& listIndices       = listFrame.passes[0].Draw(0).indices;
    const auto  stripWithoutCache = stripReceipt.bytes - cache.ManagedBytes();
    const auto  listWithoutCache  = listReceipt.bytes - listCache.ManagedBytes();
    UNSCOPED_INFO("receipts excluding geometry-cache storage differ by actual reserved capacity, including strip slack after restart");
    REQUIRE(stripWithoutCache - listWithoutCache == preparation::Allocation<std::uint32_t>(indices.capacity()) - preparation::Allocation<std::uint32_t>(listIndices.capacity()));
}

} // namespace

// Terminals seal declared zero keys; full graphs split after recorded prefixes.
void tailSplitAndKeySeal()
{
    std::string                         error;
    const NativeRenderFramePass::Target target{ 160, 32, 1, NativeDrawReplayTargetFormat::Rgba8 };
    const auto                          packet = [&](std::uint64_t key, std::uint32_t width = 160)
    {
        auto result                    = clearPacket(target);
        result.textures                = { { 1, 160, 32, NativeDrawReplayTextureFormat::Rgba8, key }, { 2, 160, 32, NativeDrawReplayTextureFormat::Rgba8, 99, true } };
        result.passes.front().resolves = { { 1, false, 0, 0, 0, width, 32 } };
        return result;
    };
    const auto tail = [&](std::uint64_t key, std::uint64_t importKey = 99)
    {
        NativeRenderFrame result;
        result.textures              = { { 1, 160, 32, NativeDrawReplayTextureFormat::Rgba8, key }, { 2, 160, 32, NativeDrawReplayTextureFormat::Rgba8, importKey, true } };
        result.outputTextureId       = 1;
        result.retainOnly            = true;
        result.continuedPrefixPasses = 1;
        return result;
    };
    const auto finish = [&](const NativeRenderFrame& first, const NativeRenderFrame& terminal)
    {
        NativeRenderFramePrefixState chain;
        return chain.Append(first, error) && chain.FinishTerminal(terminal, error);
    };
    UNSCOPED_INFO("a terminal seals the zero key of a generated prefix texture, with no passes of its own: " + error);
    REQUIRE(finish(packet(0), tail(7)));
    UNSCOPED_INFO("a terminal cannot replace a nonzero generated key");
    REQUIRE((!finish(packet(5), tail(7)) && error.find("changed a recorded resource descriptor") != std::string::npos));
    UNSCOPED_INFO("a terminal cannot change an imported key");
    REQUIRE((!finish(packet(0), tail(7, 98)) && error.find("changed a recorded resource descriptor") != std::string::npos));
    UNSCOPED_INFO("a sealed key stays unique among the chain's retained keys");
    REQUIRE((!finish(packet(0), tail(99)) && error.find("unique") != std::string::npos));
    UNSCOPED_INFO("a sealed key requires the completely resolved texture any retained key requires");
    REQUIRE((!finish(packet(0, 80), tail(7)) && error.find("completely resolved") != std::string::npos));
    NativeRenderFramePrefixState appended;
    auto                         keyed = clearPacket(target, true);
    keyed.textures                     = tail(7).textures;
    UNSCOPED_INFO("only a terminal seals a key; a later packet cannot");
    REQUIRE((appended.Append(packet(0), error) && !appended.Append(keyed, error) && error.find("changed a recorded resource descriptor") != std::string::npos));
    auto standalone                  = tail(7);
    standalone.continuedPrefixPasses = 0;
    UNSCOPED_INFO("only a tail continuing a chain may carry no passes");
    REQUIRE((!ValidateNativeRenderFrame(standalone, error) && error.find("bounded passes") != std::string::npos));

    NativeRenderFrame full;
    full.passes.resize(4);
    full.passes[3].colorSourcePass = 3;
    full.textures                  = { { 1, 160, 32, NativeDrawReplayTextureFormat::Rgba8, 7 } };
    auto split                     = full;
    UNSCOPED_INFO("splitting drops the recorded passes and shifts a later alias back past them: " + error);
    REQUIRE((SplitNativeRenderFrameTail(split, 2, error) && split.continuedPrefixPasses == 2 && split.passes.size() == 2 &&
             split.passes[1].colorSourcePass == 1 && split.textures == full.textures));
    for (const std::size_t count : { 3U, 5U, 0U })
    {
        auto refused = full;
        UNSCOPED_INFO("a split that would record an alias source or more passes than the graph has changes nothing");
        REQUIRE((!SplitNativeRenderFrameTail(refused, count, error) && refused.passes.size() == 4 && !refused.continuedPrefixPasses &&
                 refused.passes[3].colorSourcePass == 3));
    }
    auto aliased                      = full;
    aliased.passes[3].colorSourcePass = 0;
    aliased.passes[1].colorSourcePass = 1;
    UNSCOPED_INFO("a recorded pass cannot alias");
    REQUIRE((!SplitNativeRenderFrameTail(aliased, 2, error) && aliased.passes.size() == 4));
    aliased.passes[1].colorSourcePass = 4;
    UNSCOPED_INFO("a recorded pass cannot alias a later pass");
    REQUIRE((!SplitNativeRenderFrameTail(aliased, 2, error) && aliased.passes.size() == 4));
    UNSCOPED_INFO("a tail cannot split again");
    REQUIRE((!SplitNativeRenderFrameTail(split, 1, error) && split.continuedPrefixPasses == 2));
}

// An over-bound prefix splits into two passes that the tail continues.
void batchedPrefixTail()
{
    const NativeRenderFramePass::Target              target{ 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 };
    const std::vector<NativeGameplayShaderInterface> shaders{ targetShader(4, 0) };
    constexpr std::uint64_t                          draws = NativeRenderBatchCommands + 1;
    const auto                                       run   = [&](NativeGameplayCommandFeed& commands, NativeRenderFrame& frame)
    {
        std::string                                error;
        std::vector<NativeGameplayRetainedTexture> next;
        UNSCOPED_INFO(error);
        REQUIRE(commands.Begin(1, 1, {}, error));
        for (std::uint64_t id = 1; id <= draws; ++id)
        {
            UNSCOPED_INFO(error);
            REQUIRE(commands.Consume(targetDrawEvent(id, 0x0A010280, target, 0), {}, {}, error));
        }
        const auto terminal = terminalEvent(draws + 4);
        const auto boundary = snapshotWithEvents({ terminal });
        UNSCOPED_INFO(error);
        REQUIRE((commands.Consume(copyEvent(draws + 1), {}, {}, error) && commands.Consume(clearEvent(draws + 2), {}, {}, error) &&
                 commands.Consume(copyEvent(draws + 3, true), {}, {}, error) && commands.Finish(terminal, boundary.final_swap, 2, 1, frame, next, error)));
    };
    NativeRenderFrame full;
    {
        NativeGameplayCommandFeed plain(shaders);
        run(plain, full);
    }
    std::string                    error;
    NativeRenderFramePrefixState   chain;
    std::vector<NativeRenderFrame> packets;
    NativeGameplayCommandFeed      commands(shaders);
    commands.SetPrefixConsumer([&](NativeRenderFrame packet, std::string& failure)
                               {
                                   if (!chain.Append(packet, failure))
                                       return false;
                                   packets.push_back(std::move(packet));
                                   return true;
                               });
    NativeRenderFrame frame;
    run(commands, frame);
    UNSCOPED_INFO("the tail continues both batches of its one oversized prefix pass");
    REQUIRE((packets.size() == 1 && packets.front().passes.size() == 2 && frame.continuedPrefixPasses == 2));
    requireJoinedTail(chain, std::move(frame), packets, full, "batched prefix");
}

TEST_CASE("native_gameplay_render_frame", "[ctest][native-gameplay-render-frame]")
{
    tailSplitAndKeySeal();
    batchedPrefixTail();
    preparedTerminalAdmission();
    preparedTerminalFailures();
    preparedCollectorClear();
    preparedCollectorResources();
    preparedResourceRejection();
    preparedNativeTextureFeed();
    preparedSignedNativeProducerFeed();
    preparedDrawOperation();
    preparedCopyFunding();
    preparedDrawFunding();
    preparationRetirementGate();
    fundedAggregateDrawPressure();
    preparationLayoutAndCapacity();
    preparationStorageRequests();
    preparationUnknownAndEarlySuccess();
    preparationGenericSharedOwner();
    appendTargetChanges();
    depthSurfaceIdentity();
    targetChangeFeed();
    queuedPrefixOwnership();
    admittedResolveState();
    changedTiledResolveState();
    rejectChangedResolveBeforePendingClear();
    admittedTiledResolveOrder();
    admittedTerminalFailures();
    queuedPrefixFailures();
    queuedPrefixBudget();
    queuedPrefixBudget(true);
    admittedOperations();
    shaderVersionFeed();
    preparedTextureFeed();
    preparedCreationWriterFeed();
    preparedCpuCubeFeed();
    admittedBudget(false);
    admittedBudget(false, false, false, false, true);
    admittedBudget(true, false, false, false, true);
    sharedIndexOwners();
    admittedBudget(true);
    admittedBudget(false, true);
    admittedBudget(false, true, true);
    admittedBudget(false, true, false, true);
    auto              input = completeMainInput();
    NativeRenderFrame frame;
    std::string       error;
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayRenderFrame(input, frame, error));
    UNSCOPED_INFO("main copies preserve ordered 0x14000 left/right strip placement");
    REQUIRE((frame.passes.size() == 2 && frame.passes[0].resolves[0].destinationX == 0 &&
             frame.passes[1].resolves[0].destinationX == 640));

    {
        auto              owned     = completeMainInput();
        const auto*       vertices  = owned.passes[0].draws[0].recipe.vertexData.data();
        const auto*       indices   = owned.passes[0].draws[0].recipe.indices.data();
        const auto*       shader    = owned.passes[0].draws[0].recipe.vertexShaderDxil.data();
        const auto*       constants = owned.passes[0].draws[0].recipe.vertexConstants.data();
        NativeRenderFrame moved;
        UNSCOPED_INFO(error);
        REQUIRE(BuildNativeGameplayRenderFrame(std::move(owned), moved, error));
        const auto& recipe = moved.passes[0].draws[0];
        UNSCOPED_INFO("owned gameplay conversion must transfer draw buffers without copying them");
        REQUIRE((recipe.vertexData.data() == vertices && recipe.indices.data() == indices &&
                 recipe.vertexShaderDxil.data() == shader && recipe.vertexConstants.data() == constants));
        UNSCOPED_INFO("owned and borrowed gameplay conversion must preserve the same draw bytes");
        REQUIRE((recipe.vertexData == frame.passes[0].draws[0].vertexData &&
                 recipe.indices == frame.passes[0].draws[0].indices &&
                 recipe.vertexShaderDxil == frame.passes[0].draws[0].vertexShaderDxil &&
                 recipe.vertexConstants == frame.passes[0].draws[0].vertexConstants));
        UNSCOPED_INFO("borrowed gameplay conversion must preserve independent input buffers");
        REQUIRE((!input.passes[0].draws[0].recipe.vertexData.empty() &&
                 input.passes[0].draws[0].recipe.vertexData.data() != frame.passes[0].draws[0].vertexData.data()));
        const auto* draws = moved.passes[0].draws.data();
        UNSCOPED_INFO(error);
        REQUIRE(BatchNativeRenderFrame(moved, error));
        UNSCOPED_INFO("passes within the command bound must retain their draw storage");
        REQUIRE(moved.passes[0].draws.data() == draws);
    }

    {
        auto       large  = completeMainInput();
        auto&      pass   = large.passes[0];
        const auto recipe = pass.draws[0].recipe;
        pass.draws.clear();
        pass.operations.clear();
        for (std::uint32_t i = 0; i <= NativeRenderBatchCommands; ++i)
        {
            pass.draws.push_back({ i + 1, recipe });
            pass.operations.push_back({ NativeGameplayRenderFrameOperation::Kind::Draw, i + 1, i });
        }
        pass.firstEvent               = 1;
        pass.lastEvent                = NativeRenderBatchCommands + 2;
        pass.copies[0].eventId        = pass.lastEvent;
        pass.copies[0].triggerEventId = pass.lastEvent - 1;
        pass.operations.push_back({ NativeGameplayRenderFrameOperation::Kind::Copy, pass.lastEvent, 0 });
        large.passes[1] = mainPass(pass.lastEvent + 1, pass.lastEvent + 2, 0x114000, 640, 0x100040);
        NativeRenderFrame converted;
        UNSCOPED_INFO(error);
        REQUIRE(BuildNativeGameplayRenderFrame(large, converted, error));
        UNSCOPED_INFO("gameplay conversion must batch expanded commands without an early resolve");
        REQUIRE((converted.passes.size() == 3 && converted.passes[0].draws.size() == NativeRenderBatchCommands &&
                 converted.passes[0].resolves.empty() && converted.passes[1].preservePreviousAttachments &&
                 converted.passes[1].resolves.size() == 1));

        auto alias = frame;
        alias.passes[0].draws.assign(NativeRenderBatchCommands + 1, recipe);
        alias.passes[1].colorSourcePass = 1;
        UNSCOPED_INFO(error);
        REQUIRE(BatchNativeRenderFrame(alias, error));
        UNSCOPED_INFO("color aliases must name the final batch of their original producer pass");
        REQUIRE((alias.passes[2].colorSourcePass == 2 && alias.passes[1].colorSourcePass == 0));
    }

    auto observed              = tiledCopy(716, 715, 0x1D818000, 0, 0x100340);
    observed.registers[0x231B] = 0x01000300;
    NativeRenderFrameResolve observedResolve;
    UNSCOPED_INFO("observed 0x1D818000 color copy accepts the RB destination swap bit");
    REQUIRE((DeriveNativeGameplayResolve(observed, 0x1D818000, observedResolve, error) &&
             observedResolve.swapRedBlue));
    auto rgb10              = tiledCopy(2, 1, 0x100000, 0, 0x100040);
    rgb10.registers[0x2001] = 0x20000;
    rgb10.registers[0x231B] = 0x380;
    UNSCOPED_INFO("RGB10 left half resolves without a post-copy clear or channel swap");
    REQUIRE((DeriveNativeGameplayResolve(rgb10, 0x100000, observedResolve, error) &&
             !observedResolve.swapRedBlue && observedResolve.destinationX == 0));
    rgb10.registers[0x2319] += 0x14000;
    rgb10.destinationX = 640;
    UNSCOPED_INFO("RGB10 right half retains its 640-pixel destination offset");
    REQUIRE((DeriveNativeGameplayResolve(rgb10, 0x100000, observedResolve, error) &&
             observedResolve.destinationX == 640));
    rgb10.registers[0x231B] = 0x01000300;
    UNSCOPED_INFO("RGB10 tiled copies must not silently resolve through the RGBA8 format");
    REQUIRE(!DeriveNativeGameplayResolve(rgb10, 0x100000, observedResolve, error));
    auto observedDepth                 = observed;
    observedDepth.eventId              = 714;
    observedDepth.triggerEventId       = 713;
    observedDepth.registers[0x2318]    = 4;
    observedDepth.registers[0x2319]    = 0x1D0F0000;
    observedDepth.registers[0x231B]    = 0x00000302;
    observedDepth.destinationTextureId = 2;
    observedDepth.depth                = true;
    observedDepth.semantics            = NativeGameplayRenderFrameCopy::Semantics::TiledMainDepth;
    UNSCOPED_INFO("observed depth copy selects sample zero without averaging at geometry edges");
    REQUIRE((DeriveNativeGameplayResolve(observedDepth, 0x1D0F0000, observedResolve, error) &&
             observedResolve.depth && observedResolve.samples == std::array<std::uint32_t, 2>{ 0, 0 }));
    const std::array<std::uint32_t, 8> depthSamples  = { 0, 3, 0, 3, 0, 0, 0, 0 };
    auto                               rejectedDepth = observedDepth;
    rejectedDepth.registers[0x231D]                  = 0x12345678;
    UNSCOPED_INFO("unsupported copy state must identify the exact rejected register and values");
    REQUIRE((!DeriveNativeGameplayResolve(rejectedDepth, 0x1D0F0000, observedResolve, error) &&
             error.find("0x231D is 0x12345678, expected 0xFFFFFF00") != std::string::npos));
    for (std::uint32_t selection = 0; selection < depthSamples.size(); ++selection)
    {
        observedDepth.registers[0x2318] = 4 | (selection << 4);
        UNSCOPED_INFO("2X depth selectors select a single mapped sample, including averaging selectors");
        REQUIRE((DeriveNativeGameplayResolve(observedDepth, 0x1D0F0000, observedResolve, error) &&
                 observedResolve.samples == std::array<std::uint32_t, 2>{ depthSamples[selection], depthSamples[selection] }));
    }

    auto badBase                                  = completeMainInput();
    badBase.passes[1].copies[0].registers[0x2319] = 0x118000;
    UNSCOPED_INFO("a non-tiled destination base is rejected");
    REQUIRE((!BuildNativeGameplayRenderFrame(badBase, frame, error) &&
             error.find("0x14000") != std::string::npos));

    auto alias                                     = completeMainInput();
    alias.passes[1].draws[0].recipe.width          = 1280;
    alias.passes[1].draws[0].recipe.viewport.width = 1280;
    alias.passes[1].draws[0].recipe.scissor.right  = 1280;
    alias.passes[1].copies[0]                      = explicitCopy(201, 200, 1, 1280);
    alias.passes[1].operations[1]                  = { NativeGameplayRenderFrameOperation::Kind::Copy, 201, 0 };
    UNSCOPED_INFO("an implicit clear-view alias transition is rejected");
    REQUIRE((!BuildNativeGameplayRenderFrame(alias, frame, error) &&
             error.find("alias") != std::string::npos));
    alias.passes[1].clearAliasTransition = true;
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayRenderFrame(alias, frame, error));

    auto postCopyClear          = completeMainInput();
    postCopyClear.outputSwizzle = { 2, 1, 0, 5 };
    NativeGameplayRenderFramePass clearAfterCopy;
    clearAfterCopy.firstEvent                  = 201;
    clearAfterCopy.lastEvent                   = 201;
    clearAfterCopy.attachmentAlias             = 0;
    clearAfterCopy.attachmentAliasValid        = true;
    clearAfterCopy.clearAliasTransition        = true;
    clearAfterCopy.preservePreviousAttachments = true;
    clearAfterCopy.postCopyClear               = true;
    clearAfterCopy.target                      = { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 };
    NativeRenderFramePass::Clear clear;
    clear.rectangle  = { 0, 0, 640, 720 };
    clear.color      = true;
    clear.depth      = true;
    clear.stencil    = true;
    clear.colorValue = { 0, 0, 0, 0 };
    clear.depthValue = 1.0F;
    clearAfterCopy.clears.push_back(clear);
    postCopyClear.passes.push_back(clearAfterCopy);

    NativeGameplayRenderFramePass targetSwitch;
    targetSwitch.firstEvent           = 202;
    targetSwitch.lastEvent            = 202;
    targetSwitch.attachmentAlias      = 0x2D0;
    targetSwitch.attachmentAliasValid = true;
    targetSwitch.draws.push_back({ 202, draw(1280, 720, 1, 1) });
    targetSwitch.operations.push_back({ NativeGameplayRenderFrameOperation::Kind::Draw, 202, 0 });
    postCopyClear.passes.push_back(targetSwitch);
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayRenderFrame(postCopyClear, frame, error));
    UNSCOPED_INFO("same-event post-copy clear must execute before the target switch");
    REQUIRE((frame.outputSwizzle == std::array<std::uint8_t, 4>{ 2, 1, 0, 5 } &&
             frame.passes.size() == 4 && frame.passes[2].draws.empty() &&
             frame.passes[2].resolves.empty() && frame.passes[2].clears.size() == 1 &&
             frame.passes[2].preservePreviousAttachments && frame.passes[3].draws.size() == 1 &&
             GetNativeRenderFrameTarget(frame.passes[3]).width == 1280));

    auto zeroAlias = completeMainInput();
    zeroAlias.passes.push_back(clearAfterCopy);
    zeroAlias.passes.back().postCopyClear = false;
    zeroAlias.passes.back().firstEvent    = 202;
    zeroAlias.passes.back().lastEvent     = 202;
    zeroAlias.passes.back().target        = { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 };
    NativeGameplayRenderFramePass zeroAliasChange;
    zeroAliasChange.firstEvent           = 203;
    zeroAliasChange.lastEvent            = 203;
    zeroAliasChange.attachmentAlias      = 0;
    zeroAliasChange.attachmentAliasValid = true;
    zeroAliasChange.draws.push_back({ 203, draw(1280, 720, 1, 1) });
    zeroAliasChange.operations.push_back({ NativeGameplayRenderFrameOperation::Kind::Draw, 203, 0 });
    zeroAlias.passes.push_back(zeroAliasChange);
    UNSCOPED_INFO("zero is a valid attachment alias and cannot silently change target");
    REQUIRE((!BuildNativeGameplayRenderFrame(zeroAlias, frame, error) &&
             error.find("alias") != std::string::npos));

    NativeGameplayRenderFrameInput producer;
    producer.resources = {
        { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 }, true },
        { { 2, 1040, 720, NativeDrawReplayTextureFormat::Rgba8 }, true },
    };
    producer.outputTextureId      = 1;
    producer.returnResolvedOutput = true;
    NativeGameplayRenderFramePass aux;
    aux.firstEvent = 10;
    aux.lastEvent  = 11;
    aux.draws.push_back({ 10, draw(1040, 720, 1, 1) });
    aux.copies.push_back(explicitCopy(11, 10, 2, 1040));
    aux.operations = {
        { NativeGameplayRenderFrameOperation::Kind::Draw, 10, 0 },
        { NativeGameplayRenderFrameOperation::Kind::Copy, 11, 0 },
    };
    producer.passes.push_back(aux);
    NativeGameplayRenderFramePass consumer;
    consumer.firstEvent                     = 20;
    consumer.lastEvent                      = 21;
    auto consumerDraw                       = draw(1280, 720, 1, 1);
    consumerDraw.textureMask                = 1;
    consumerDraw.textures[0].frameTextureId = 2;
    consumerDraw.textures[0].width          = 1040;
    consumerDraw.textures[0].height         = 720;
    consumerDraw.textures[0].format         = NativeDrawReplayTextureFormat::Rgba8;
    consumer.draws.push_back({ 20, std::move(consumerDraw) });
    consumer.copies.push_back(explicitCopy(21, 20, 1, 1280));
    consumer.operations = {
        { NativeGameplayRenderFrameOperation::Kind::Draw, 20, 0 },
        { NativeGameplayRenderFrameOperation::Kind::Copy, 21, 0 },
    };
    producer.passes.push_back(std::move(consumer));
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayRenderFrame(producer, frame, error));

    auto overwrite                                         = producer;
    overwrite.resources[1].texture.format                  = NativeDrawReplayTextureFormat::Rgb10a2;
    overwrite.passes[0].attachmentAlias                    = 1;
    overwrite.passes[0].draws[0]                           = fullColorWrite(10, 1040);
    overwrite.passes[0].copies[0].samples                  = { 0, 3 };
    overwrite.passes[1].draws[0].recipe.textures[0].format = NativeDrawReplayTextureFormat::Rgb10a2;
    NativeGameplayRenderFramePass previous;
    previous.firstEvent = previous.lastEvent = 8;
    previous.attachmentAlias                 = 1;
    previous.draws.push_back({ 8, draw(1040, 720, 4, 9) });
    previous.operations.push_back({ NativeGameplayRenderFrameOperation::Kind::Draw, 8, 0 });
    overwrite.passes.insert(overwrite.passes.begin(), previous);
    for (const auto [vertexHash, pixelHash] : { std::pair{ 0xDE0359DBD47D7FABULL, 0x560C753DAD5555ADULL },
                                                std::pair{ 0x7C1DD533708BB131ULL, 0x818E4EAF9BA41B33ULL },
                                                std::pair{ 0x38691F81BEA4AD28ULL, 0xB8C1B55C7CE2E502ULL },
                                                std::pair{ 0xF95D105E51CF1A19ULL, 0x884C15F5AE654F89ULL } })
    {
        auto& completeWrite            = overwrite.passes[1].draws[0].recipe;
        completeWrite.vertexShaderHash = vertexHash;
        completeWrite.pixelShaderHash  = pixelHash;
        UNSCOPED_INFO("a proved full RGB10 color write may replace an unused attachment alias: " + error);
        REQUIRE(BuildNativeGameplayRenderFrame(overwrite, frame, error));
        auto admittedOverwrite = overwrite;
        admitDraws(admittedOverwrite, error);
        UNSCOPED_INFO("terminal overwrite lookahead reads the same admitted operations: " + error);
        REQUIRE((BuildNativeGameplayRenderFrame(admittedOverwrite, frame, error) &&
                 frame.passes[1].admittedDraws[0] == admittedOverwrite.passes[1].draws[0].admittedRecipe));
        for (unsigned mutation = 0; mutation < 9; ++mutation)
        {
            auto  partial = overwrite;
            auto& write   = partial.passes[1].draws[0];
            switch (mutation)
            {
                case 0:
                    write.alphaTestEnabled = true;
                    break;
                case 1:
                    write.recipe.scissor.right -= 1;
                    break;
                case 2:
                    write.recipe.blend.writeMask = 7;
                    break;
                case 3:
                    write.recipe.rasterizer.frontCounterClockwise = false;
                    break;
                case 4:
                    write.recipe.viewport.width -= 1;
                    break;
                case 5:
                    write.recipe.blend.enabled          = true;
                    write.recipe.blend.destinationColor = NativeDrawReplayBlendFactor::One;
                    break;
                case 6:
                    partial.passes[2].draws[0].recipe.depth.enabled = true;
                    break;
                case 7:
                    partial.passes[2].copies[0].depth = true;
                    break;
                case 8:
                    write.recipe.pixelShaderHash ^= 1;
                    break;
            }
            UNSCOPED_INFO("unproved color writes and later depth reads cannot discard attachment contents");
            REQUIRE((!BuildNativeGameplayRenderFrame(std::move(partial), frame, error) && error.find("alias") != std::string::npos));
        }
    }

    auto partialOverwrite                                         = overwrite;
    partialOverwrite.resources[1].texture.height                  = 360;
    auto& blur                                                    = partialOverwrite.passes[1];
    blur.draws[0].recipe.vertexShaderHash                         = 0x38691F81BEA4AD28ULL;
    blur.draws[0].recipe.pixelShaderHash                          = 0xB8C1B55C7CE2E502ULL;
    blur.draws[0].recipe.viewport.height                          = 360;
    blur.draws[0].recipe.scissor.bottom                           = 360;
    blur.copies[0].sourceHeight                                   = 360;
    partialOverwrite.passes[2].draws[0].recipe.textures[0].height = 360;
    NativeGameplayRenderFramePass replacement;
    replacement.firstEvent = replacement.lastEvent = 18;
    replacement.attachmentAlias                    = 0x2001;
    replacement.draws.push_back(fullColorWrite(18, 1040));
    replacement.operations.push_back({ NativeGameplayRenderFrameOperation::Kind::Draw, 18, 0 });
    partialOverwrite.passes.insert(partialOverwrite.passes.begin() + 2, replacement);
    auto& depthPass                         = partialOverwrite.passes[3];
    depthPass.attachmentAlias               = 0x3001;
    depthPass.draws[0].recipe.depth.enabled = true;
    NativeRenderFramePass::Clear depthClear;
    depthClear.rectangle = { 0, 0, 1280, 720 };
    depthClear.depth = depthClear.stencil = true;
    depthPass.clears.push_back(depthClear);
    auto laterDepth       = depthPass;
    laterDepth.firstEvent = laterDepth.lastEvent = 22;
    laterDepth.draws[0].eventId                  = 22;
    laterDepth.preservePreviousAttachments       = true;
    laterDepth.clears.clear();
    laterDepth.copies.clear();
    laterDepth.operations = { { NativeGameplayRenderFrameOperation::Kind::Draw, 22, 0 } };
    partialOverwrite.passes.push_back(laterDepth);
    UNSCOPED_INFO("a partial color overwrite may resolve its initialized rows before full replacement and independently cleared depth: " + error);
    REQUIRE(BuildNativeGameplayRenderFrame(partialOverwrite, frame, error));
    {
        auto repeated         = partialOverwrite;
        auto secondBlur       = repeated.passes[1];
        secondBlur.firstEvent = secondBlur.lastEvent = 12;
        secondBlur.draws[0].eventId                  = 12;
        secondBlur.draws[0].recipe.vertexShaderHash  = 0xF95D105E51CF1A19ULL;
        secondBlur.draws[0].recipe.pixelShaderHash   = 0x884C15F5AE654F89ULL;
        secondBlur.preservePreviousAttachments       = true;
        secondBlur.copies.clear();
        secondBlur.operations = { { NativeGameplayRenderFrameOperation::Kind::Draw, 12, 0 } };
        repeated.passes.insert(repeated.passes.begin() + 2, secondBlur);
        UNSCOPED_INFO("successive blur draws may preserve the same initialized region: " + error);
        REQUIRE(BuildNativeGameplayRenderFrame(repeated, frame, error));
        repeated.passes[2].draws[0].recipe.scissor.bottom = 361;
        UNSCOPED_INFO("a later draw cannot read color outside the initialized region");
        REQUIRE((!BuildNativeGameplayRenderFrame(repeated, frame, error) && error.find("alias") != std::string::npos));
    }
    for (unsigned mutation = 0; mutation < 8; ++mutation)
    {
        auto invalid = partialOverwrite;
        switch (mutation)
        {
            case 0:
                invalid.passes[1].copies[0].sourceHeight = 361;
                break;
            case 1:
                invalid.passes[2].draws[0].alphaTestEnabled = true;
                break;
            case 2:
                invalid.passes[2].attachmentAlias ^= 1;
                break;
            case 3:
                invalid.passes[3].clears.clear();
                break;
            case 4:
                invalid.passes[3].clears[0].stencil = false;
                break;
            case 5:
                invalid.passes[3].clears[0].rectangle.bottom = 360;
                break;
            case 6:
                invalid.passes[4].preservePreviousAttachments = false;
                break;
            case 7:
                invalid.passes[4].attachmentAlias ^= 0x1000;
                break;
        }
        UNSCOPED_INFO("partial overwrite must reject out-of-region copies, unproved replacements and lost depth initialization");
        REQUIRE((!BuildNativeGameplayRenderFrame(std::move(invalid), frame, error) && error.find("alias") != std::string::npos));
    }

    NativeGameplayRenderFrameInput colorAlias;
    colorAlias.resources            = { { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 }, true } };
    colorAlias.outputTextureId      = 1;
    colorAlias.returnResolvedOutput = true;
    NativeGameplayRenderFramePass original;
    original.firstEvent = original.lastEvent = 1;
    original.attachmentAlias                 = 0x530000;
    original.attachmentAliasValid            = true;
    original.draws.push_back({ 1, draw(640, 720, 4, 9) });
    original.operations.push_back({ NativeGameplayRenderFrameOperation::Kind::Draw, 1, 0 });
    NativeGameplayRenderFramePass signedAlias;
    signedAlias.firstEvent = signedAlias.lastEvent = 2;
    signedAlias.attachmentAliasValid               = true;
    signedAlias.clearAliasTransition               = true;
    signedAlias.target                             = { 1280, 720, 1, NativeDrawReplayTargetFormat::Rg16Snorm };
    NativeRenderFramePass::Clear aliasClear;
    aliasClear.rectangle = { 0, 0, 1280, 720 };
    aliasClear.color     = true;
    signedAlias.clears.push_back(aliasClear);
    auto adjacent       = signedAlias;
    adjacent.firstEvent = adjacent.lastEvent = 3;
    adjacent.attachmentAlias                 = 720;
    NativeGameplayRenderFramePass composition;
    composition.firstEvent           = 4;
    composition.lastEvent            = 5;
    composition.attachmentAlias      = 0x530000;
    composition.attachmentAliasValid = true;
    composition.draws.push_back({ 4, draw(1280, 720, 1, 1) });
    composition.copies.push_back(explicitCopy(5, 4, 1, 1280));
    composition.operations = { { NativeGameplayRenderFrameOperation::Kind::Draw, 4, 0 },
                               { NativeGameplayRenderFrameOperation::Kind::Copy, 5, 0 } };
    colorAlias.passes      = { original, signedAlias, adjacent, composition };
    UNSCOPED_INFO(error);
    REQUIRE(BuildNativeGameplayRenderFrame(NativeGameplayRenderFrameInput(colorAlias), frame, error));
    UNSCOPED_INFO("composition restores the last physical color writer across a disjoint alias");
    REQUIRE(frame.passes.back().colorSourcePass == 2);
    colorAlias.passes[2].attachmentAlias = 100;
    UNSCOPED_INFO("an intervening overlapping EDRAM write invalidates color restoration");
    REQUIRE((!BuildNativeGameplayRenderFrame(std::move(colorAlias), frame, error) && error.find("compatible native color writer") != std::string::npos));

    auto consumerFirst = producer;
    std::swap(consumerFirst.passes[0], consumerFirst.passes[1]);
    consumerFirst.passes[0].firstEvent               = 10;
    consumerFirst.passes[0].lastEvent                = 11;
    consumerFirst.passes[0].draws[0].eventId         = 10;
    consumerFirst.passes[0].copies[0].eventId        = 11;
    consumerFirst.passes[0].copies[0].triggerEventId = 10;
    consumerFirst.passes[0].operations[0].eventId    = 10;
    consumerFirst.passes[0].operations[1].eventId    = 11;
    consumerFirst.passes[1].firstEvent               = 20;
    consumerFirst.passes[1].lastEvent                = 21;
    consumerFirst.passes[1].draws[0].eventId         = 20;
    consumerFirst.passes[1].copies[0].eventId        = 21;
    consumerFirst.passes[1].copies[0].triggerEventId = 20;
    consumerFirst.passes[1].operations[0].eventId    = 20;
    consumerFirst.passes[1].operations[1].eventId    = 21;
    UNSCOPED_INFO("a generated texture cannot be sampled before its producer pass");
    REQUIRE((!BuildNativeGameplayRenderFrame(consumerFirst, frame, error) &&
             error.find("incompletely resolved") != std::string::npos));

    auto drawAfterCopy                                          = completeMainInput();
    drawAfterCopy.passes[0].draws[0].eventId                    = 101;
    drawAfterCopy.passes[0].copies[0].eventId                   = 100;
    drawAfterCopy.passes[0].copies[0].sourceGeometryOwnedByCopy = true;
    drawAfterCopy.passes[0].operations                          = {
        { NativeGameplayRenderFrameOperation::Kind::Copy, 100, 0 },
        { NativeGameplayRenderFrameOperation::Kind::Draw, 101, 0 },
    };
    UNSCOPED_INFO("draws after resolves require a separate native pass");
    REQUIRE((!BuildNativeGameplayRenderFrame(drawAfterCopy, frame, error) &&
             error.find("draw-after-copy") != std::string::npos));

    auto nonMonotonic                               = completeMainInput();
    nonMonotonic.passes[0].draws[0].eventId         = 101;
    nonMonotonic.passes[0].copies[0].eventId        = 100;
    nonMonotonic.passes[0].copies[0].triggerEventId = 99;
    nonMonotonic.passes[0].operations               = {
        { NativeGameplayRenderFrameOperation::Kind::Draw, 101, 0 },
        { NativeGameplayRenderFrameOperation::Kind::Copy, 100, 0 },
    };
    UNSCOPED_INFO("operations must be strictly increasing within a native pass");
    REQUIRE((!BuildNativeGameplayRenderFrame(nonMonotonic, frame, error) &&
             error.find("unique ordered event identities") != std::string::npos));

    auto                                       invalidSwap      = snapshotWithEvents({ snapshotEvent(1, DrawCaptureFrameEvent::Kind::kSwap),
                                                                                       snapshotEvent(2, DrawCaptureFrameEvent::Kind::kDraw),
                                                                                       snapshotEvent(3, DrawCaptureFrameEvent::Kind::kSwap),
                                                                                       snapshotEvent(4, DrawCaptureFrameEvent::Kind::kSwap) });
    std::vector<NativeGameplayShaderInterface> syntheticShaders = { syntheticShader() };
    {
        NativeGameplayCommandFeed                  commands(syntheticShaders);
        std::vector<NativeGameplayRetainedTexture> next;
        UNSCOPED_INFO(error);
        REQUIRE(commands.Begin(1, 1, {}, error));
        UNSCOPED_INFO("a command feed cannot replace an owner before its terminal boundary");
        REQUIRE((!commands.Begin(1, 1, {}, error) && error.find("unfinished frame") != std::string::npos));
        UNSCOPED_INFO(error);
        REQUIRE(commands.Consume(snapshotEvent(1, DrawCaptureFrameEvent::Kind::kSwap), {}, {}, error));
        auto rejected                        = nonIndexedDraw(2);
        rejected.draw.draw.registers[0x2205] = 0x12345678;
        auto admittedRegisters               = std::move(rejected.draw.draw.registers);
        UNSCOPED_INFO("direct preparation reads the admitted bank without a materialized diagnostic event bank");
        REQUIRE((rejected.draw.draw.registers.empty() &&
                 !commands.Consume(rejected, admittedRegisters, {}, {}, error) && error.find("event 2") != std::string::npos));
        auto terminal = snapshotEvent(3, DrawCaptureFrameEvent::Kind::kSwap);
        auto boundary = snapshotWithEvents({ terminal });
        UNSCOPED_INFO("a failed preparation cannot submit work or publish proposed native versions");
        REQUIRE((!commands.Finish(terminal, boundary.final_swap, 2, 1, frame, next, error) && next.empty() && frame.passes.empty()));
        UNSCOPED_INFO(error);
        REQUIRE(commands.Begin(1, 1, {}, error));
        UNSCOPED_INFO(error);
        REQUIRE(commands.Consume(snapshotEvent(1, DrawCaptureFrameEvent::Kind::kSwap), {}, {}, error));
        UNSCOPED_INFO("direct event identities remain strictly ordered");
        REQUIRE((!commands.Consume(snapshotEvent(1, DrawCaptureFrameEvent::Kind::kCopy), {}, {}, error) &&
                 error.find("order") != std::string::npos));
    }
    std::map<std::uint32_t, NativeDrawReplayTexture> noCpuTextures;
    UNSCOPED_INFO("an intermediate non-final swap is rejected with its event ID");
    REQUIRE((!BuildNativeGameplayRenderFrame(invalidSwap, syntheticShaders, noCpuTextures, frame, error) &&
             error.find("event 3") != std::string::npos &&
             error.find("leading non-final swap") != std::string::npos));

    {
        NativeGameplaySnapshot compact;
        compact.start_frame      = invalidSwap.start_frame;
        compact.end_frame        = invalidSwap.end_frame;
        compact.start_submission = invalidSwap.start_submission;
        compact.end_submission   = invalidSwap.end_submission;
        compact.final_swap       = invalidSwap.final_swap;
        for (const auto& event : invalidSwap.events)
            compact.events.push_back(event);
        NativeGameplayRetainedTexture              prior{ 0x100, { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 99, true } };
        std::vector<NativeGameplayRetainedTexture> retained{ prior, prior }, next{ prior };
        UNSCOPED_INFO("live carry must reject duplicate pages without publishing a next version");
        REQUIRE((!BuildNativeGameplayLiveFrame(compact, {}, {}, retained, frame, next, error) && next.empty() &&
                 error.find("pages must be unique") != std::string::npos));
        retained[1].guestBasePage += 0x14;
        retained[1].texture.width /= 2;
        UNSCOPED_INFO("tiled aliases must agree on the completed resource descriptor");
        REQUIRE((!BuildNativeGameplayLiveFrame(compact, {}, {}, retained, frame, next, error) &&
                 error.find("conflicting descriptors") != std::string::npos));
        retained[1].texture.width = prior.texture.width;
        UNSCOPED_INFO("compatible completed aliases must pass import admission and retain frame-boundary checks");
        REQUIRE((!BuildNativeGameplayLiveFrame(compact, {}, {}, retained, frame, next, error) &&
                 error.find("leading non-final swap") != std::string::npos));
        retained.assign(NativeRenderBatchCommands + 1, prior);
        UNSCOPED_INFO("live carry must preserve the existing resource count bound");
        REQUIRE((!BuildNativeGameplayLiveFrame(compact, {}, {}, retained, frame, next, error) && next.empty() &&
                 error.find("resource identity bound") != std::string::npos));
    }

    auto triggerOrder                                  = snapshotWithEvents({ snapshotEvent(1, DrawCaptureFrameEvent::Kind::kSwap),
                                                                              snapshotEvent(2, DrawCaptureFrameEvent::Kind::kDraw),
                                                                              snapshotEvent(3, DrawCaptureFrameEvent::Kind::kDraw),
                                                                              snapshotEvent(4, DrawCaptureFrameEvent::Kind::kCopy),
                                                                              snapshotEvent(5, DrawCaptureFrameEvent::Kind::kSwap) });
    triggerOrder.events[1].draw.draw.primitive_type    = 8;
    triggerOrder.events[1].draw.draw.registers[0x2208] = 6;
    triggerOrder.events[2].host_issued                 = false;
    triggerOrder.events[3].copy_mode                   = "resolve";
    UNSCOPED_INFO("a legacy resolve trigger cannot skip an intervening event");
    REQUIRE((!BuildNativeGameplayRenderFrame(triggerOrder, syntheticShaders, noCpuTextures, frame, error) &&
             error.find("event 3") != std::string::npos &&
             error.find("immediately followed") != std::string::npos));

    auto triggerMismatch                                  = snapshotWithEvents({ snapshotEvent(1, DrawCaptureFrameEvent::Kind::kSwap),
                                                                                 snapshotEvent(2, DrawCaptureFrameEvent::Kind::kDraw),
                                                                                 snapshotEvent(3, DrawCaptureFrameEvent::Kind::kCopy),
                                                                                 snapshotEvent(4, DrawCaptureFrameEvent::Kind::kSwap) });
    triggerMismatch.events[1].draw.draw.primitive_type    = 8;
    triggerMismatch.events[1].draw.draw.registers[0x2208] = 6;
    triggerMismatch.events[2].copy_mode                   = "resolve";
    triggerMismatch.events[2].draw.draw.registers[0x2000] ^= 1U;
    UNSCOPED_INFO("a legacy resolve copy must retain the trigger register state");
    REQUIRE((!BuildNativeGameplayRenderFrame(triggerMismatch, syntheticShaders, noCpuTextures, frame, error) &&
             error.find("event 3") != std::string::npos && error.find("state do not match") != std::string::npos));

    auto  nonIndexed = snapshotWithEvents({ snapshotEvent(1, DrawCaptureFrameEvent::Kind::kSwap),
                                            nonIndexedDraw(2),
                                            snapshotEvent(3, DrawCaptureFrameEvent::Kind::kSwap) });
    auto  emptyHud   = nonIndexed;
    auto& hud        = emptyHud.events[1].draw.draw.registers;
    hud[0x2080]      = 0x00007D80;
    hud[0x2081] = hud[0x2082] = 0;
    hud[0x210F] = hud[0x2110] = 0x438DC000;
    hud[0x2111]               = 0xC3728000;
    hud[0x2112]               = 0x43728000;
    hud[0x2113]               = 0;
    UNSCOPED_INFO("an empty HUD scissor offset to x=-640 must reach shader validation");
    REQUIRE((!BuildNativeGameplayRenderFrame(emptyHud, {}, noCpuTextures, frame, error) &&
             error.find("no shader specialization") != std::string::npos));
    hud[0x2082] = 1U | (1U << 16);
    UNSCOPED_INFO("nonempty off-target HUD scissors must retain their bounds check");
    REQUIRE((!BuildNativeGameplayRenderFrame(emptyHud, {}, noCpuTextures, frame, error) &&
             error.find("scissor is outside") != std::string::npos));
    auto  narrow  = nonIndexed;
    auto& strip   = narrow.events[1].draw.draw.registers;
    strip[0x2000] = 0x04000140;
    strip[0x210F] = strip[0x2110] = std::bit_cast<std::uint32_t>(128.0F);
    strip[0x2111]                 = std::bit_cast<std::uint32_t>(-16.0F);
    strip[0x2112]                 = std::bit_cast<std::uint32_t>(16.0F);
    strip[0x2082]                 = 256U | (32U << 16);
    UNSCOPED_INFO("the 320-pixel pitch admits its captured 256x32 offscreen viewport");
    REQUIRE((!BuildNativeGameplayRenderFrame(narrow, {}, noCpuTextures, frame, error) &&
             error.find("no shader specialization") != std::string::npos));
    strip[0x2082] = 256U | (33U << 16);
    UNSCOPED_INFO("the narrow attachment still rejects scissors past its measured rows");
    REQUIRE((!BuildNativeGameplayRenderFrame(narrow, {}, noCpuTextures, frame, error) &&
             error.find("attachment height") != std::string::npos));
    auto  smallStrip = nonIndexed;
    auto& strip128   = smallStrip.events[1].draw.draw.registers;
    strip128[0x2000] = 0x020000A0;
    strip128[0x210F] = strip128[0x2110] = std::bit_cast<std::uint32_t>(64.0F);
    strip128[0x2111]                    = std::bit_cast<std::uint32_t>(-16.0F);
    strip128[0x2112]                    = std::bit_cast<std::uint32_t>(16.0F);
    strip128[0x2082]                    = 128U | (32U << 16);
    UNSCOPED_INFO("the captured 128x32 strip admits its 160-pixel attachment before shader lookup");
    REQUIRE((!BuildNativeGameplayRenderFrame(smallStrip, {}, noCpuTextures, frame, error) &&
             error.find("no shader specialization") != std::string::npos));
    strip128[0x2082] = 128U | (33U << 16);
    UNSCOPED_INFO("the 128-pixel strip still rejects scissors beyond its measured 32 rows");
    REQUIRE((!BuildNativeGameplayRenderFrame(smallStrip, {}, noCpuTextures, frame, error) &&
             error.find("attachment height") != std::string::npos));
    auto  downsample = nonIndexed;
    auto& small      = downsample.events[1].draw.draw.registers;
    small[0x2000]    = 0x05010140;
    small[0x210F] = small[0x2110] = std::bit_cast<std::uint32_t>(160.0F);
    small[0x2111]                 = std::bit_cast<std::uint32_t>(-90.0F);
    small[0x2112]                 = std::bit_cast<std::uint32_t>(90.0F);
    small[0x2082]                 = 320U | (180U << 16);
    UNSCOPED_INFO("the padded 320x184 attachment admits its 320x180 viewport before shader lookup");
    REQUIRE((!BuildNativeGameplayRenderFrame(downsample, {}, noCpuTextures, frame, error) &&
             error.find("no shader specialization") != std::string::npos));
    small[0x2082] = 320U | (185U << 16);
    UNSCOPED_INFO("the downsample attachment still rejects scissors past its measured padded rows");
    REQUIRE((!BuildNativeGameplayRenderFrame(downsample, {}, noCpuTextures, frame, error) &&
             error.find("attachment height") != std::string::npos));
    UNSCOPED_INFO("nonindexed raw draws pass index acquisition and report the later conversion error");
    REQUIRE((!BuildNativeGameplayRenderFrame(nonIndexed, syntheticShaders, noCpuTextures, frame, error) &&
             error.find("event 2") != std::string::npos && error.find("index input") == std::string::npos));
    nonIndexed.events[1].draw.texture_bindings_complete = false;
    UNSCOPED_INFO("a textureless pinned program needs no omitted host pixel-stage binding list");
    REQUIRE((!BuildNativeGameplayRenderFrame(nonIndexed, syntheticShaders, noCpuTextures, frame, error) &&
             error.find("event 2") != std::string::npos && error.find("required texture bindings") == std::string::npos));
    syntheticShaders[0].textureFetchSlots = { 0 };
    UNSCOPED_INFO("a textured program still rejects an incomplete host binding list");
    REQUIRE((!BuildNativeGameplayRenderFrame(nonIndexed, syntheticShaders, noCpuTextures, frame, error) &&
             error.find("required texture bindings") != std::string::npos));
}
