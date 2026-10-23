#include "native_gameplay_fixture_inputs.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace rerevved::gpu;
using namespace rerevved::gpu::diagnostics;
using namespace rex::graphics::diagnostic;

namespace
{

struct Memory
{
    std::vector<std::uint8_t> bytes   = std::vector<std::uint8_t>(0x300000);
    bool                      fail    = false;
    bool                      current = true;
    std::size_t               reads   = 0;

    static bool check(const void* context, std::uint64_t generation) noexcept
    {
        return static_cast<const Memory*>(context)->current && generation == 1;
    }

    static bool copy(void* context, std::uint32_t address, std::span<std::uint8_t> destination) noexcept
    {
        auto& memory = *static_cast<Memory*>(context);
        ++memory.reads;
        if (memory.fail || std::uint64_t(address) + destination.size() > memory.bytes.size())
            return false;
        std::memcpy(destination.data(), memory.bytes.data() + address, destination.size());
        return true;
    }
};

void eventTimings()
{
    std::size_t unprofiledBytes = 0;
    for (bool enabled : { false, true })
    {
        Memory memory;
        memory.bytes[0x100] = 37;
        std::vector<std::uint32_t>         registers(0x5003), gamma(256);
        const std::array<std::uint32_t, 1> program{ 1 };
        DrawCaptureFrameInputSwapView      boundary;
        boundary.id        = 1;
        boundary.frame     = 1;
        boundary.registers = registers;
        boundary.gamma_256 = gamma;
        boundary.is_8bpc   = true;
        NativeGameplayLiveInputs inputs;
        std::size_t              consumers = 0, reclaims = 0;
        inputs.SetEventConsumer([&](const auto&, const auto&, const auto&, const auto&, std::string&)
                                {
                                    ++consumers;
                                    return true;
                                });
        inputs.ResetEventTimings(enabled);
        std::string error;
        UNSCOPED_INFO(error);
        REQUIRE(inputs.Begin(boundary, error));
        inputs.SetOwnedByteBudget(inputs.OwnedBytes() + 1, [&](std::size_t)
                                  {
                                      ++reclaims;
                                      return NativeGameplayLiveInputs::MaxOwnedBytes;
                                  });
        DrawCaptureFrameInputVertexFetchView vertex{ 95, 0x100, 16, { &memory, &Memory::copy } };
        DrawCaptureFrameInputDrawView        draw;
        draw.id            = 2;
        draw.frame         = 2;
        draw.registers     = registers;
        draw.vertex_shader = draw.pixel_shader = { true, 1, program };
        draw.original_host_issued              = true;
        draw.execution_status                  = DrawCaptureExecutionStatus::kIssued;
        draw.vertex_fetches                    = std::span(&vertex, 1);
        auto start                             = std::chrono::steady_clock::now();
        inputs.BeginEventTiming(start, true);
        UNSCOPED_INFO(error);
        REQUIRE(inputs.Draw(draw, error));
        inputs.EndEventTiming(std::chrono::steady_clock::now());
        draw.id                   = 3;
        draw.original_host_issued = false;
        draw.execution_status     = DrawCaptureExecutionStatus::kNoEffect;
        start                     = std::chrono::steady_clock::now();
        inputs.BeginEventTiming(start, true);
        UNSCOPED_INFO(error);
        REQUIRE(inputs.Draw(draw, error));
        inputs.EndEventTiming(std::chrono::steady_clock::now());
        registers[0x2319] = 0x100000;
        DrawCaptureFrameInputCopyView copy;
        copy.id                 = 4;
        copy.frame              = 2;
        copy.registers          = registers;
        copy.original_succeeded = true;
        copy.written_address    = 0x100000;
        copy.written_length     = 4;
        start                   = std::chrono::steady_clock::now();
        inputs.BeginEventTiming(start, false);
        UNSCOPED_INFO(error);
        REQUIRE(inputs.Copy(copy, true, error));
        inputs.EndEventTiming(std::chrono::steady_clock::now());
        const auto timing = inputs.Timings();
        UNSCOPED_INFO("profiling preserves boundary consumption, reclamation and owned event payloads");
        REQUIRE((consumers == 3 && reclaims == 1 && memory.reads == 1 && inputs.Snapshot().events.size() == 3 &&
                 inputs.Snapshot().events[1].draw.draw.vertex_fetches[0].Payload()[0] == 37));
        if (enabled)
        {
            UNSCOPED_INFO("profiling does not change payload charges");
            REQUIRE(inputs.OwnedBytes() == unprofiledBytes);
            UNSCOPED_INFO("callback phases close without counting Begin or adding nested reclamation twice");
            REQUIRE((timing.drawCallbacks == 2 && timing.copyCallbacks == 1 && timing.encodedEvents == 2 && timing.consumerCalls == 2 &&
                     timing.reclaims == 1 && timing.clockReads == 8 && !timing.incompleteEvents &&
                     timing.collectionNanoseconds + timing.consumeNanoseconds + timing.encodingNanoseconds <= timing.callbackNanoseconds &&
                     timing.reclaimNanoseconds <= timing.collectionNanoseconds));
        }
        else
        {
            unprofiledBytes = inputs.OwnedBytes();
            UNSCOPED_INFO("disabled profiling adds no clock reads or phase counts");
            REQUIRE((!timing.enabled && !timing.clockReads && !timing.callbackNanoseconds && !timing.drawCallbacks && !timing.copyCallbacks &&
                     !timing.encodedEvents && !timing.consumerCalls && !timing.reclaims));
        }
        boundary.id    = 5;
        boundary.frame = 3;
        UNSCOPED_INFO(error);
        REQUIRE(inputs.Finish(boundary, error));
        NativeGameplayLiveInputs completed(std::move(inputs)), next;
        next.ResetEventTimings(enabled);
        UNSCOPED_INFO(error);
        REQUIRE(next.BeginAfterCompletion(completed, error));
        UNSCOPED_INFO("completed owners retain their own timings while the next boundary starts empty");
        REQUIRE((completed.Timings().callbackNanoseconds == timing.callbackNanoseconds &&
                 completed.Timings().encodedEvents == timing.encodedEvents && !next.Timings().callbackNanoseconds &&
                 !next.Timings().clockReads && !next.Timings().consumerCalls));
    }

    NativeGameplayLiveInputs rejected;
    rejected.SetEventConsumer([](const auto& event, const auto&, const auto&, const auto&, std::string& error)
                              {
                                  if (event.kind != DrawCaptureFrameEvent::Kind::kDraw)
                                      return true;
                                  error = "rejected profiled draw";
                                  return false;
                              });
    rejected.ResetEventTimings(true);
    std::vector<std::uint32_t>         registers(0x5003), gamma(256);
    const std::array<std::uint32_t, 1> program{ 1 };
    DrawCaptureFrameInputSwapView      boundary;
    boundary.id        = 1;
    boundary.registers = registers;
    boundary.gamma_256 = gamma;
    boundary.is_8bpc   = true;
    std::string error;
    UNSCOPED_INFO(error);
    REQUIRE(rejected.Begin(boundary, error));
    DrawCaptureFrameInputDrawView draw;
    draw.id            = 2;
    draw.registers     = registers;
    draw.vertex_shader = draw.pixel_shader = { true, 1, program };
    draw.original_host_issued              = true;
    draw.execution_status                  = DrawCaptureExecutionStatus::kIssued;
    rejected.BeginEventTiming(std::chrono::steady_clock::now(), true);
    const bool accepted = rejected.Draw(draw, error);
    rejected.EndEventTiming(std::chrono::steady_clock::now());
    UNSCOPED_INFO("profiling retains rejected inputs and their original error");
    REQUIRE((!accepted && error == "rejected profiled draw" && rejected.Snapshot().events.size() == 2 &&
             rejected.Timings().consumerCalls == 1 && rejected.Timings().encodedEvents == 1 && !rejected.Timings().incompleteEvents));
}

void preparedSwapOwnership()
{
    std::string                error;
    std::vector<std::uint32_t> registers(0x5003), gamma(256);
    registers[0x4801] = 0x00100006;
    gamma[127]        = 0x12345678;
    DrawCaptureFrameInputSwapView swap;
    swap.id                 = 1;
    swap.frame              = 7;
    swap.submission         = 8;
    swap.frontbuffer_ptr    = 0x100000;
    swap.frontbuffer_width  = 1280;
    swap.frontbuffer_height = 720;
    swap.registers          = registers;
    swap.gamma_256          = gamma;
    swap.is_8bpc            = true;
    NativeGameplayLiveInputs inputs;
    std::size_t              consumers = 0;
    inputs.SetEventConsumer([&](const auto& event, const auto& bank, const auto&, const auto&, std::string&)
                            {
                                ++consumers;
                                UNSCOPED_INFO("prepared leading boundary supplies owned prospective state");
                                REQUIRE((event.kind == DrawCaptureFrameEvent::Kind::kSwap && !event.success && !event.host_issued &&
                                         event.execution_status == DrawCaptureExecutionStatus::kUnfinished && event.execution_reason == DrawCaptureReason::kNone &&
                                         bank.size() == 0x5003 && bank[0x4801] == 0x00100006));
                                return true;
                            });
    UNSCOPED_INFO(error);
    REQUIRE((inputs.BeginPrepared(swap, error) && consumers == 1));
    ++swap.id;
    UNSCOPED_INFO("prepared terminal seals once without consuming a command");
    REQUIRE((inputs.PreparedSwap(swap, error) && consumers == 1));
    const auto ownedBytes = inputs.OwnedBytes();
    std::fill(registers.begin(), registers.end(), 0);
    std::fill(gamma.begin(), gamma.end(), 0);
    const auto terminal = inputs.Snapshot().events.back();
    UNSCOPED_INFO("prepared terminal owns final register and gamma values with truthful provenance");
    REQUIRE((!terminal.success && !terminal.host_issued && terminal.execution_status == DrawCaptureExecutionStatus::kUnfinished &&
             terminal.execution_reason == DrawCaptureReason::kNone && terminal.draw.draw.registers[0x4801] == 0x00100006 &&
             inputs.Snapshot().final_swap.gamma.gamma_256[127] == 0x12345678 && inputs.Snapshot().end_frame == 8));
    UNSCOPED_INFO("a duplicate terminal cannot append or close twice");
    REQUIRE((!inputs.PreparedSwap(swap, error) && inputs.Snapshot().events.size() == 2 && inputs.OwnedBytes() == ownedBytes));
    NativeGameplayLiveInputs successor;
    successor.SetEventConsumer([&](const auto& event, const auto& bank, const auto&, const auto&, std::string&)
                               {
                                   UNSCOPED_INFO("copied successor boundary preserves the held predecessor's prepared provenance");
                                   REQUIRE((event.id == terminal.id && !event.success && event.execution_status == DrawCaptureExecutionStatus::kUnfinished &&
                                            bank[0x4801] == terminal.draw.draw.registers[0x4801]));
                                   return true;
                               });
    UNSCOPED_INFO("copying the prepared boundary retains the prior failure journal");
    REQUIRE((successor.BeginAfterCompletion(inputs, error) && inputs.Snapshot().events.size() == 2 && inputs.OwnedBytes() == ownedBytes));
    ++swap.id;
    UNSCOPED_INFO("copied leading boundary preserves prepared terminal admission");
    REQUIRE(successor.PreparedSwap(swap, error));
    UNSCOPED_INFO("released leading boundary preserves prepared provenance");
    REQUIRE((inputs.BeginAfterCompletion(error) && consumers == 2 && inputs.Snapshot().events.size() == 1 &&
             !inputs.Snapshot().events.front().success));
    UNSCOPED_INFO("released leading boundary preserves prepared terminal admission");
    REQUIRE(inputs.PreparedSwap(swap, error));
}

void preparedSwapFailures()
{
    for (int failure = 0; failure != 10; ++failure)
    {
        std::string                   error;
        std::vector<std::uint32_t>    registers(0x5003), gamma(256);
        DrawCaptureFrameInputSwapView swap;
        swap.id         = 1;
        swap.frame      = 1;
        swap.submission = 8;
        swap.registers  = registers;
        swap.gamma_256  = gamma;
        swap.is_8bpc    = true;
        NativeGameplayLiveInputs inputs;
        UNSCOPED_INFO(error);
        REQUIRE(inputs.BeginPrepared(swap, error));
        swap.id = 2;
        switch (failure)
        {
            case 0:
                swap.original_succeeded = true;
                break;
            case 1:
                swap.id = 0;
                break;
            case 2:
                swap.id = 1;
                break;
            case 3:
                swap.registers = {};
                break;
            case 4:
                swap.gamma_256 = {};
                break;
            case 5:
                swap.use_pwl_gamma_ramp = true;
                break;
            case 6:
                swap.use_fxaa = true;
                break;
            case 7:
                swap.frame = std::numeric_limits<std::uint64_t>::max();
                break;
            case 8:
                swap.frame = 0;
                break;
            case 9:
                swap.submission = 7;
                break;
        }
        UNSCOPED_INFO("invalid prepared terminals leave the active journal unsealed");
        REQUIRE((!inputs.PreparedSwap(swap, error) && inputs.Snapshot().events.size() == 1 && inputs.Snapshot().end_frame == 0));
    }
}

void preparedDrawOwnership()
{
    Memory memory;
    memory.bytes[0x100] = 37;
    memory.bytes[0x200] = 1;
    std::vector<std::uint32_t> registers(0x5003), gamma(256), program{ 1, 2, 3 };
    registers[0x2104] = 15;
    DrawCaptureFrameInputSwapView boundary;
    boundary.id        = 1;
    boundary.registers = registers;
    boundary.gamma_256 = gamma;
    boundary.is_8bpc   = true;
    DrawCaptureFrameInputVertexFetchView vertex{ 95, 0x100, 16, { &memory, &Memory::copy } };
    DrawCaptureFrameInputDrawView        draw;
    draw.id            = 2;
    draw.registers     = registers;
    draw.vertex_shader = draw.pixel_shader = { true, 1, program };
    draw.vertex_fetches                    = std::span(&vertex, 1);
    draw.requested_index_count             = 1;
    draw.index.present                     = true;
    draw.index.physical_address            = 0x200;
    draw.index.size                        = 2;
    draw.index.count                       = 1;
    draw.index.memory                      = { &memory, &Memory::copy };
    NativeGameplayLiveInputs inputs;
    std::string              error;
    std::size_t              consumers = 0;
    inputs.SetEventConsumer([&](const auto& event, const auto& bank, const auto&, const auto&, std::string&)
                            {
                                if (event.kind != DrawCaptureFrameEvent::Kind::kDraw)
                                    return true;
                                ++consumers;
                                UNSCOPED_INFO("prepared collection must preserve unfinished original execution");
                                REQUIRE((!event.success && !event.host_issued && event.execution_status == DrawCaptureExecutionStatus::kUnfinished));
                                UNSCOPED_INFO("prepared callback borrows registers and owns shader, vertex and index bytes");
                                REQUIRE((bank[0x2104] == 15 && event.draw.draw.vertex_ucode[0] == 1 &&
                                         event.draw.draw.vertex_fetches[0].Payload()[0] == 37 && event.draw.draw.index.bytes[0] == 1));
                                return true;
                            });
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Begin(boundary, error));
    UNSCOPED_INFO(error);
    REQUIRE((inputs.PreparedDraw(draw, error) && consumers == 1));
    std::fill(registers.begin(), registers.end(), 0);
    std::fill(program.begin(), program.end(), 0);
    std::fill(memory.bytes.begin(), memory.bytes.end(), 0);
    const auto event = inputs.Snapshot().events[1];
    UNSCOPED_INFO("compact encoding preserves prepared input ownership and original execution provenance");
    REQUIRE((event.draw.draw.registers[0x2104] == 15 && event.draw.draw.vertex_ucode == std::vector<std::uint32_t>{ 1, 2, 3 } &&
             event.draw.draw.vertex_fetches[0].Payload()[0] == 37 && event.draw.draw.index.bytes == std::vector<std::uint8_t>{ 1, 0 } &&
             !event.success && !event.host_issued && event.execution_status == DrawCaptureExecutionStatus::kUnfinished));

    DrawCaptureFrameInputTextureView texture;
    for (int invalid = 0; invalid != 10; ++invalid)
    {
        NativeGameplayLiveInputs rejected;
        UNSCOPED_INFO(error);
        REQUIRE(rejected.Begin(boundary, error));
        auto candidate = draw;
        auto fetch     = vertex;
        switch (invalid)
        {
            case 0:
                candidate.cpu_data_complete = false;
                break;
            case 1:
                candidate.original_host_issued = true;
                break;
            case 2:
                candidate.execution_status = DrawCaptureExecutionStatus::kIssued;
                break;
            case 3:
                candidate.execution_reason = DrawCaptureReason::kExecutionFailure;
                break;
            case 4:
                candidate.unavailable_reason = DrawCaptureReason::kMemexport;
                break;
            case 5:
                candidate.vertex_shader.present = false;
                break;
            case 6:
                candidate.pixel_shader.present = false;
                break;
            case 7:
                candidate.textures = std::span(&texture, 1);
                break;
            case 8:
                fetch.physical_address   = 0x1FFFFFFF;
                candidate.vertex_fetches = std::span(&fetch, 1);
                break;
            case 9:
                candidate.requested_index_count = 2;
                break;
        }
        UNSCOPED_INFO("prepared collection must reject incomplete, forged or invalid range inputs");
        REQUIRE(!rejected.PreparedDraw(candidate, error));
    }
    NativeGameplayLiveInputs legacy;
    UNSCOPED_INFO("legacy collection must reject prepared execution provenance");
    REQUIRE((legacy.Begin(boundary, error) && !legacy.Draw(draw, error)));
}

void preparedCreationWriters()
{
    for (bool floating : { false, true })
    {
        Memory                     memory;
        std::vector<std::uint32_t> registers(0x5003), gamma(256), program{ 1 };
        const NativeTextureFetch   fetch = floating ? NativeTextureFetch{ 0x84000156, 0x001000A4, 0x003FE1FF, 0x16D1, 0, 0x200 } : NativeTextureFetch{ 0x08000002, 0x00100002, 0x007FE3FF, 0x248, 0, 0x200 };
        std::copy(fetch.begin(), fetch.end(), registers.begin() + 0x4800);
        DrawCaptureFrameInputSwapView boundary;
        boundary.id        = 1;
        boundary.registers = registers;
        boundary.gamma_256 = gamma;
        boundary.is_8bpc   = true;
        DrawCaptureFrameInputTextureView texture;
        texture.fetch_words = fetch;
        texture.width = texture.height = floating ? 512 : 1024;
        texture.depth                  = 1;
        texture.pitch                  = floating ? 16 : 32;
        texture.dimension              = 1;
        texture.format                 = floating ? 36 : 2;
        texture.endianness             = floating ? 2 : 0;
        texture.tiled                  = floating;
        texture.component_mapping      = floating ? 0x1B68 : 0x1124;
        texture.swizzled_signs         = floating ? 0x55 : 0;
        texture.is_signed              = floating;
        texture.base_address           = 0x100000;
        texture.base_size              = 1048576;
        texture.memory                 = { &memory, &Memory::copy };
        DrawCaptureFrameInputDrawView draw;
        draw.id            = 2;
        draw.registers     = registers;
        draw.vertex_shader = draw.pixel_shader = { true, 1, program };
        draw.textures                          = { &texture, 1 };
        NativeGameplayLiveInputs inputs;
        std::string              error;
        UNSCOPED_INFO(error);
        REQUIRE((inputs.BeginPrepared(boundary, error) && inputs.PreparedDraw(draw, error)));
        const auto first = inputs.Textures().at(0).textureVersion;
        UNSCOPED_INFO("creation writer callback retains complete immutable level");
        REQUIRE((first && first->Payload()->size() == 1048576U && first->Subresources().size() == 1U));
        memory.bytes[0x100000 + 4097] = 0x3F;
        ++draw.id;
        UNSCOPED_INFO("creation writer callback replaces owner without modifying earlier bytes");
        REQUIRE((inputs.PreparedDraw(draw, error) && inputs.Textures().at(1).textureVersion != first &&
                 std::ranges::all_of(*first->Payload(), [](auto byte)
                                     {
                                         return byte == 0;
                                     })));
        const auto replacement = inputs.Textures().at(1).payloadBytes();
        UNSCOPED_INFO("creation writer replacement owner lost the written byte");
        REQUIRE((std::ranges::count(replacement, std::uint8_t{ 0x3F }) == 1U &&
                 std::ranges::count(replacement, std::uint8_t{ 0 }) == static_cast<std::ptrdiff_t>(replacement.size() - 1U)));
        // Tiled byte offset 4096 is texel (32,0); k8in32 moves byte 1 to byte 2.
        UNSCOPED_INFO("creation writer moved the written byte");
        REQUIRE(replacement[floating ? 32U * 4U + 2U : 4097U] == 0x3F);
        --texture.base_size;
        ++draw.id;
        const auto reads = memory.reads;
        UNSCOPED_INFO("creation writer truncated metadata rejects before guarded reads");
        REQUIRE((!inputs.PreparedDraw(draw, error) && memory.reads == reads));
    }
}

void preparedCubeResources()
{
    for (unsigned form = 0; form < 3; ++form)
    {
        const bool                 floating = form == 1, bc3 = form == 2;
        const auto                 size   = bc3 ? 128U : 64U;
        const auto                 levels = bc3 ? 8U : 7U;
        Memory                     memory;
        std::vector<std::uint32_t> registers(0x5003), gamma(256), program{ 1 };
        NativeTextureFetch         fetch = bc3 ? NativeTextureFetch{ 0x01000002, 0x00100054, 0x140FE07F, 0x00000D10, 0x1C0, 0x00180E00 } : floating ? NativeTextureFetch{ 0x00824956, 0x00100060, 0x1407E03F, 0x00A80D11, 0x183, 0x00180E18 }
                                                                                                                                                    : NativeTextureFetch{ 0x00824802, 0x00100086, 0x1407E03F, 0x00A81414, 0x183, 0x00180E18 };
        std::copy(fetch.begin(), fetch.end(), registers.begin() + 0x4800);
        NativeTextureMemoryRanges ranges;
        std::string               error;
        UNSCOPED_INFO(error);
        REQUIRE(GetNativeTextureMemoryRanges(fetch, ranges, error));
        UNSCOPED_INFO("cube footprint includes all six packed tails");
        REQUIRE((ranges.base.size == (floating ? 196608U : 98304U) && ranges.mips.size == (bc3 ? 280192U : floating ? 94208U
                                                                                                                    : 94080U)));
        DrawCaptureFrameInputSwapView boundary;
        boundary.id        = 1;
        boundary.registers = registers;
        boundary.gamma_256 = gamma;
        boundary.is_8bpc   = true;
        DrawCaptureFrameInputTextureView texture;
        texture.fetch_words = fetch;
        texture.width = texture.height = size;
        texture.depth                  = 6;
        texture.pitch                  = bc3 ? 4 : 2;
        texture.dimension              = 3;
        texture.format                 = bc3 ? 20 : floating ? 32
                                                             : 6;
        texture.endianness             = (floating || bc3) ? 1 : 2;
        texture.mip_max_level          = levels - 1;
        texture.packed_mips            = true;
        texture.component_mapping      = (floating || bc3) ? 0x1688 : 0x1A0A;
        texture.swizzled_signs         = floating ? 0x55 : 0;
        texture.is_signed              = floating;
        texture.base_address           = ranges.base.address;
        texture.base_size              = ranges.base.size;
        texture.mip_address            = ranges.mips.address;
        texture.mip_size               = ranges.mips.size;
        texture.memory                 = { &memory, &Memory::copy };
        DrawCaptureFrameInputDrawView draw;
        draw.id            = 2;
        draw.registers     = registers;
        draw.vertex_shader = draw.pixel_shader = { true, 1, program };
        draw.textures                          = { &texture, 1 };
        NativeGameplayLiveInputs inputs;
        UNSCOPED_INFO(error);
        REQUIRE((inputs.Begin(boundary, error) && inputs.PreparedDraw(draw, error)));
        const auto first = inputs.Textures().at(0).textureVersion;
        UNSCOPED_INFO("Prepared cube owns every immutable face/level subresource");
        REQUIRE((first && first->Subresources().size() == 6 * levels && first->Layout().dimension == NativeDrawReplayTextureDimension::TextureCube &&
                 first->Payload()->size() == (bc3 ? 131232U : floating ? 262128U
                                                                       : 131064U)));
        // The highest live byte belongs to the final face's packed tail.
        memory.bytes[ranges.mips.address + ranges.mips.size - 1] = 0x7F;
        ++draw.id;
        UNSCOPED_INFO("final-face tail updates retain the old immutable owner");
        REQUIRE((inputs.PreparedDraw(draw, error) && inputs.Textures().at(1).textureVersion != first &&
                 std::all_of(first->Payload()->begin(), first->Payload()->end(), [](auto b)
                             {
                                 return b == 0;
                             })));
        const auto reject = [&](auto mutate)
        {
            auto bad   = texture;
            auto words = fetch;
            mutate(bad, words);
            bad.fetch_words = words;
            std::copy(words.begin(), words.end(), registers.begin() + 0x4800);
            draw.textures    = { &bad, 1 };
            const auto reads = memory.reads;
            ++draw.id;
            UNSCOPED_INFO("malformed Prepared cube fails before CPU reads");
            REQUIRE((!inputs.PreparedDraw(draw, error) && memory.reads == reads));
            std::copy(fetch.begin(), fetch.end(), registers.begin() + 0x4800);
            draw.textures = { &texture, 1 };
        };
        reject([](auto& t, auto&)
               {
                   t.depth = 5;
               });
        reject([](auto& t, auto&)
               {
                   t.dimension = 1;
               });
        reject([](auto&, auto& w)
               {
                   w[2] = (w[2] & 0x03FFFFFFU) | (4U << 26);
               });
        reject([](auto&, auto& w)
               {
                   w[5] = (w[5] & ~(3U << 9)) | (1U << 9);
               });
        reject([](auto& t, auto&)
               {
                   --t.width;
               });
        reject([](auto& t, auto&)
               {
                   --t.mip_size;
               });
        reject([](auto& t, auto&)
               {
                   t.is_signed = !t.is_signed;
               });
        reject([](auto& t, auto&)
               {
                   t.swizzled_signs ^= 2;
               });
        reject([](auto&, auto& w)
               {
                   w[3] |= 1U << 13;
               });
        reject([](auto& t, auto&)
               {
                   t.gpu_written = true;
               });
        reject([](auto& t, auto&)
               {
                   t.available = false;
               });
        reject([](auto& t, auto& w)
               {
                   ++t.pitch;
                   w[0] += 1U << 22;
               });
        reject([](auto& t, auto& w)
               {
                   --t.mip_max_level;
                   w[4] -= 1U << 6;
               });
        reject([](auto&, auto& w)
               {
                   w[3] ^= 1U;
               });
        reject([](auto& t, auto& w)
               {
                   t.endianness = 0;
                   w[1] &= ~(3U << 6);
               });
        if (floating)
            for (auto signs : { 0xAAU, 0xFFU })
                reject([signs](auto& t, auto& w)
                       {
                           w[0]             = (w[0] & ~0x3FCU) | (signs << 2);
                           t.swizzled_signs = signs;
                       });
    }
}

void preparedResources()
{
    Memory                        memory;
    std::vector<std::uint32_t>    registers(0x5003), gamma(256), program{ 1 };
    DrawCaptureFrameInputSwapView boundary;
    boundary.id        = 1;
    boundary.registers = registers;
    boundary.gamma_256 = gamma;
    boundary.is_8bpc   = true;
    NativeTextureFetch        fetch{ 2U | (1U << 22), 0x00100006, 0, 0xD10, 0, 0x200 };
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
    texture.memory                                                 = { &memory, &Memory::copy };
    memory.bytes[ranges.base.address]                              = 17;
    DrawCaptureFrameInputDrawView draw;
    draw.id            = 2;
    draw.registers     = registers;
    draw.vertex_shader = draw.pixel_shader = { true, 1, program };
    draw.textures                          = { &texture, 1 };
    NativeGameplayLiveInputs inputs;
    UNSCOPED_INFO(error);
    REQUIRE((inputs.Begin(boundary, error) && inputs.PreparedDraw(draw, error)));
    const auto first = inputs.Textures().at(0).textureVersion;
    UNSCOPED_INFO("missing watches use guarded reads for prepared CPU textures");
    REQUIRE((first && first->Payload()->front() == 17 && memory.reads == 1));
    ++draw.id;
    texture.memory_version            = { 1, 1, ranges.base.address, ranges.base.size, 0, 0, &memory, &Memory::check };
    memory.current                    = false;
    memory.bytes[ranges.base.address] = 29;
    UNSCOPED_INFO("stale watches reread CPU overwrites without changing earlier owners");
    REQUIRE((inputs.PreparedDraw(draw, error) && memory.reads == 2 && first->Payload()->front() == 17 &&
             inputs.Textures().at(1).payloadBytes().front() == 29));
    texture.gpu_written = true;
    ++draw.id;
    const auto reads = memory.reads;
    UNSCOPED_INFO("prepared GPU-owned required textures fail before reads or event publication");
    REQUIRE((!inputs.PreparedDraw(draw, error) && memory.reads == reads && inputs.Snapshot().events.size() == 3));
    texture.gpu_written = false;
    texture.available   = false;
    UNSCOPED_INFO("unavailable required prepared textures fail before reads");
    REQUIRE((!inputs.PreparedDraw(draw, error) && memory.reads == reads));
    texture.available = true;
    memory.fail       = true;
    UNSCOPED_INFO("guarded read failure cannot publish a prepared texture event");
    REQUIRE((!inputs.PreparedDraw(draw, error) && inputs.Snapshot().events.size() == 3));
    memory.fail = false;
    for (int invalid = 0; invalid != 4; ++invalid)
    {
        NativeGameplayLiveInputs rejected;
        UNSCOPED_INFO(error);
        REQUIRE(rejected.Begin(boundary, error));
        auto source = texture;
        auto words  = fetch;
        if (invalid == 0)
        {
            source.dimension = 3;
            source.depth     = 6;
            words[5]         = (words[5] & ~(3U << 9)) | (3U << 9);
            words[2] |= 5U << 26;
        }
        else if (invalid == 1)
        {
            source.depth = 2;
            words[1] |= 1U << 10;
            words[2] |= 1U << 26;
        }
        else if (invalid == 2)
        {
            source.depth = 2;
            words[2] |= 1U << 26;
        }
        else
            --source.base_size;
        source.fetch_words = words;
        auto candidate     = draw;
        candidate.textures = { &source, 1 };
        const auto before  = memory.reads;
        UNSCOPED_INFO("matching unsupported prepared dimensions, layers or incomplete footprints fail before reads or publication");
        REQUIRE((!rejected.PreparedDraw(candidate, error) && memory.reads == before && rejected.Snapshot().events.size() == 1));
    }
    registers[0x2319] = 0x100000;
    registers[0x2208] = 6;
    DrawCaptureFrameInputCopyView copy;
    copy.id                  = 4;
    copy.registers           = registers;
    copy.source_geometry     = { 0, 0x100, 24, { &memory, &Memory::copy } };
    copy.cpu_data_complete   = true;
    copy.planned_address     = 0x100000;
    copy.planned_length      = 4096;
    copy.source_rect_present = true;
    copy.source_width = copy.source_height = 16;
    for (int invalid = 0; invalid != 13; ++invalid)
    {
        NativeGameplayLiveInputs rejected;
        UNSCOPED_INFO(error);
        REQUIRE(rejected.Begin(boundary, error));
        auto candidate = copy;
        switch (invalid)
        {
            case 0:
                candidate.cpu_data_complete = false;
                break;
            case 1:
                candidate.unavailable_reason = DrawCaptureReason::kTextureSource;
                break;
            case 2:
                candidate.original_succeeded = true;
                break;
            case 3:
                candidate.written_address = 0x100000;
                break;
            case 4:
                candidate.written_length = 1;
                break;
            case 5:
                candidate.planned_length = 0;
                break;
            case 6:
                candidate.planned_address = 0x1FFFFFFF;
                break;
            case 7:
                candidate.source_rect_present = false;
                break;
            case 8:
                candidate.source_width = 0;
                break;
            case 9:
                candidate.source_geometry.fetch_constant = 1;
                break;
            case 10:
                candidate.source_geometry.size = 28;
                break;
            case 11:
                candidate.copy_mode = "clear";
                break;
            case 12:
                candidate.source_geometry.physical_address = 0x1FFFFFFF;
                break;
        }
        UNSCOPED_INFO("invalid prepared copy plans cannot publish an event");
        REQUIRE((!rejected.PreparedCopy(candidate, error) && rejected.Snapshot().events.size() == 1));
    }
    for (std::uint32_t selector = 1; selector <= 3; ++selector)
    {
        NativeGameplayLiveInputs rejected;
        UNSCOPED_INFO(error);
        REQUIRE(rejected.Begin(boundary, error));
        auto bank           = registers;
        bank[0x2318]        = selector;
        auto candidate      = copy;
        candidate.registers = bank;
        const auto before   = memory.reads;
        UNSCOPED_INFO("prepared copy rejects RT" + std::to_string(selector) + " before geometry reads or publication: " + error);
        REQUIRE((!rejected.PreparedCopy(candidate, error) && memory.reads == before && rejected.Snapshot().events.size() == 1));
    }
    {
        NativeGameplayLiveInputs rejected;
        UNSCOPED_INFO(error);
        REQUIRE(rejected.Begin(boundary, error));
        auto bank = registers;
        bank[0x231B] |= 1U << 3;
        auto candidate      = copy;
        candidate.registers = bank;
        const auto before   = memory.reads;
        const bool accepted = rejected.PreparedCopy(candidate, error);
        UNSCOPED_INFO("prepared array copy rejects before geometry reads or publication: " + error);
        REQUIRE((!accepted && memory.reads == before && rejected.Snapshot().events.size() == 1));
    }
    UNSCOPED_INFO(error);
    REQUIRE(inputs.PreparedCopy(copy, error));
    const auto owned    = inputs.Snapshot().events.back();
    memory.bytes[0x100] = 99;
    registers[0x2319]   = 0;
    UNSCOPED_INFO("prepared copy owns geometry and state while retaining unfinished provenance");
    REQUIRE((!owned.success && !owned.host_issued && owned.execution_status == DrawCaptureExecutionStatus::kUnfinished &&
             owned.draw.draw.registers[0x2319] == 0x100000 && owned.draw.draw.vertex_fetches[0].Payload()[0] == 0));
    std::vector<std::uint32_t> selectedRegisters = registers;
    selectedRegisters[0x2000]                    = 0x0A010280;
    selectedRegisters[0x2001]                    = 0;
    selectedRegisters[0x2002]                    = 0x530;
    selectedRegisters[0x2205]                    = 0x10000;
    selectedRegisters[0x2208]                    = 6;
    selectedRegisters[0x2080]                    = 0;
    selectedRegisters[0x2081]                    = 0;
    selectedRegisters[0x2082]                    = 0x02D00280;
    selectedRegisters[0x2318]                    = 0x00100340;
    selectedRegisters[0x2319]                    = 0x100000;
    selectedRegisters[0x231A]                    = 0x02D00500;
    selectedRegisters[0x231B]                    = 0x01000300;
    selectedRegisters[0x231C]                    = 0xFFF;
    selectedRegisters[0x231D]                    = 0xFFFFFF00;
    selectedRegisters[0x231E] = selectedRegisters[0x231F] = 0;
    const auto words                                      = std::array<std::uint32_t, 6>{ 0xBF000000, 0xBF000000, 0x441FE000, 0xBF000000, 0x441FE000, 0x4433E000 };
    for (std::size_t index = 0; index != words.size(); ++index)
    {
        const auto word = words[index];
        for (unsigned byte = 0; byte != 4; ++byte)
            memory.bytes[0x100 + index * 4 + byte] = static_cast<std::uint8_t>(word >> (24 - byte * 8));
    }
    DrawCaptureFrameInputCopyView selected = copy;
    selected.id                            = 5;
    selected.registers                     = selectedRegisters;
    selected.source_width                  = 640;
    selected.source_height                 = 720;
    selected.source_x = selected.source_y = 0;
    NativeGameplayLiveInputs selectedInputs;
    UNSCOPED_INFO(error);
    REQUIRE((selectedInputs.Begin(boundary, error) && selectedInputs.PreparedCopy(selected, error)));
    const auto& selectedEvent = selectedInputs.Snapshot().events.back();
    UNSCOPED_INFO("prepared main-left copy owns both source clear state and geometry with unfinished provenance");
    REQUIRE((!selectedEvent.success && !selectedEvent.host_issued && selectedEvent.execution_status == DrawCaptureExecutionStatus::kUnfinished &&
             selectedEvent.draw.draw.registers[0x2319] == 0x100000 &&
             selectedEvent.draw.draw.vertex_fetches.front().Payload().front() == 0xBF &&
             selectedEvent.draw.draw.vertex_fetches.front().Payload().size() == 24));
    selectedRegisters[0x2319] = 0;
    memory.bytes[0x100]       = 0;
    UNSCOPED_INFO("prepared main-left copy retains immutable state and geometry after borrowed input mutation");
    REQUIRE((selectedEvent.draw.draw.registers[0x2319] == 0x100000 && selectedEvent.draw.draw.vertex_fetches.front().Payload().front() == 0xBF));
    selectedRegisters[0x2319] = 0x100000;
    memory.bytes[0x100]       = 0xBF;
    for (const unsigned invalid : { 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U })
    {
        NativeGameplayLiveInputs rejected;
        UNSCOPED_INFO(error);
        REQUIRE(rejected.Begin(boundary, error));
        auto candidate = selected;
        auto bank      = selectedRegisters;
        switch (invalid)
        {
            case 0:
                bank[0x2318] = 0x00100240;
                break;
            case 1:
                bank[0x2318] = 0x00100340 | (1U << 4);
                break;
            case 2:
                bank[0x231B] = 0x00000300;
                break;
            case 3:
                candidate.source_x = 1;
                break;
            case 4:
                bank[0x2319]       = 0x114000;
                candidate.source_x = 640;
                break;
            case 5:
                memory.bytes[0x100] ^= 1U;
                break;
            case 6:
                bank[0x2080] = 1;
                break;
            case 7:
                bank[0x2081] = 1;
                break;
            case 8:
                bank[0x2082] ^= 1U;
                break;
        }
        candidate.registers = bank;
        const bool accepted = rejected.PreparedCopy(candidate, error);
        if (invalid == 5)
            memory.bytes[0x100] ^= 1U;
        UNSCOPED_INFO("prepared main-left copy rejects unsupported clear selector, format or geometry before publication");
        REQUIRE((!accepted && rejected.Snapshot().events.size() == 1));
    }
    NativeGameplayLiveInputs legacy;
    UNSCOPED_INFO("legacy copies still require actual original success");
    REQUIRE((legacy.Begin(boundary, error) && !legacy.Copy(copy, true, error)));
}

void preparedNativeSource()
{
    Memory                        memory;
    std::vector<std::uint32_t>    registers(0x5003), gamma(256), program{ 1 };
    DrawCaptureFrameInputSwapView boundary;
    boundary.id        = 1;
    boundary.registers = registers;
    boundary.gamma_256 = gamma;
    boundary.is_8bpc   = true;

    const NativeTextureFetch  fetch{ 2U | (1U << 22), 0x00100006, 0, 0xD10, 0, 0x200 };
    NativeTextureMemoryRanges ranges;
    std::string               error;
    UNSCOPED_INFO(error);
    REQUIRE(GetNativeTextureMemoryRanges(fetch, ranges, error));
    DrawCaptureFrameInputTextureView texture;
    texture.fetch_words = fetch;
    texture.format      = 6;
    texture.endianness  = 0;
    texture.dimension   = 1;
    texture.width = texture.height = 4;
    texture.depth                  = 1;
    texture.pitch                  = 1;
    texture.base_address           = ranges.base.address;
    texture.base_size              = ranges.base.size;
    texture.available              = false;
    texture.unavailable_reason     = DrawCaptureReason::kTextureUnresolved;
    texture.required               = true;
    texture.memory                 = { &memory, &Memory::copy };

    DrawCaptureFrameInputDrawView draw;
    draw.id                 = 2;
    draw.registers          = registers;
    draw.vertex_shader      = { true, 1, program };
    draw.pixel_shader       = { true, 2, program };
    draw.cpu_data_complete  = false;
    draw.unavailable_reason = DrawCaptureReason::kTextureUnresolved;
    draw.textures           = { &texture, 1 };
    NativeGameplayLiveInputs inputs;
    std::size_t              consumers = 0;
    inputs.SetEventConsumer([&](const auto& event, const auto&, const auto&, const auto&, std::string&)
                            {
                                if (event.kind != DrawCaptureFrameEvent::Kind::kDraw)
                                    return true;
                                ++consumers;
                                UNSCOPED_INFO("prepared collector must publish an unresolved native texture without CPU data");
                                REQUIRE((event.kind == DrawCaptureFrameEvent::Kind::kDraw &&
                                         event.draw.texture_uses.size() == 1 && !event.draw.texture_uses[0].data_available &&
                                         event.draw.texture_uses[0].availability == DrawCaptureAvailability::kUnavailable &&
                                         event.draw.texture_uses[0].unavailable_reason == DrawCaptureReason::kTextureUnresolved));
                                return true;
                            });
    UNSCOPED_INFO(error);
    REQUIRE((inputs.Begin(boundary, error) && inputs.PreparedDraw(draw, error) && consumers == 1));
    const auto index = inputs.Snapshot().events.back().draw.texture_uses.front().texture_index;
    UNSCOPED_INFO("native collector must retain only exact metadata and no stale CPU payload");
    REQUIRE((!inputs.Textures().contains(index) && inputs.Snapshot().textures.at(index).availability == DrawCaptureAvailability::kUnavailable &&
             inputs.Snapshot().textures.at(index).unavailable_reason == DrawCaptureReason::kTextureUnresolved &&
             memory.reads == 0));

    auto gpu_texture        = texture;
    gpu_texture.gpu_written = true;
    auto gpu_draw           = draw;
    gpu_draw.id             = 3;
    gpu_draw.textures       = { &gpu_texture, 1 };
    NativeGameplayLiveInputs gpu_inputs;
    UNSCOPED_INFO(error);
    REQUIRE((gpu_inputs.Begin(boundary, error) && gpu_inputs.PreparedDraw(gpu_draw, error)));
    const auto gpu_index = gpu_inputs.Snapshot().events.back().draw.texture_uses.front().texture_index;
    UNSCOPED_INFO("native collector accepts truthful GPU ownership without creating a CPU payload");
    REQUIRE((!gpu_inputs.Textures().contains(gpu_index) &&
             gpu_inputs.Snapshot().textures.at(gpu_index).unavailable_reason == DrawCaptureReason::kTextureUnresolved &&
             memory.reads == 0));

    for (const bool signed_view : { true, false })
    {
        NativeGameplayLiveInputs rejected;
        auto                     invalid_texture = texture;
        invalid_texture.is_signed                = signed_view;
        invalid_texture.swizzled_signs           = signed_view ? 1 : 2;
        auto invalid_draw                        = draw;
        invalid_draw.id                          = 4 + static_cast<std::uint64_t>(signed_view);
        invalid_draw.textures                    = { &invalid_texture, 1 };
        const auto reads                         = memory.reads;
        UNSCOPED_INFO("native collector rejects signed or non-unsigned swizzle demand before publication");
        REQUIRE((rejected.Begin(boundary, error) && !rejected.PreparedDraw(invalid_draw, error) &&
                 memory.reads == reads && rejected.Snapshot().events.size() == 1));
    }

    const NativeTextureFetch  selectedFetch{ 0x8A024956, 0x1E3B009F, 0x0059E4FF, 0x00001691, 0, 0x00000218 };
    NativeTextureMemoryRanges selectedRanges;
    UNSCOPED_INFO(error);
    REQUIRE(GetNativeTextureMemoryRanges(selectedFetch, selectedRanges, error));
    auto selectedTexture              = texture;
    selectedTexture.fetch_words       = selectedFetch;
    selectedTexture.format            = 31;
    selectedTexture.width             = 1280;
    selectedTexture.height            = 720;
    selectedTexture.pitch             = 40;
    selectedTexture.tiled             = true;
    selectedTexture.base_address      = selectedRanges.base.address;
    selectedTexture.base_size         = selectedRanges.base.size;
    selectedTexture.mip_address       = selectedRanges.mips.address;
    selectedTexture.mip_size          = selectedRanges.mips.size;
    selectedTexture.is_signed         = true;
    selectedTexture.swizzled_signs    = 0x55;
    selectedTexture.component_mapping = 6984;
    auto selectedDraw                 = draw;
    selectedDraw.id                   = 6;
    selectedDraw.textures             = { &selectedTexture, 1 };
    NativeGameplayLiveInputs selectedInputs;
    UNSCOPED_INFO(error);
    REQUIRE((selectedInputs.Begin(boundary, error) && selectedInputs.PreparedDraw(selectedDraw, error)));
    const auto selectedIndex = selectedInputs.Snapshot().events.back().draw.texture_uses.front().texture_index;
    UNSCOPED_INFO("native collector admits only the selected signed RG16 float source without CPU payload");
    REQUIRE((!selectedInputs.Textures().contains(selectedIndex) && selectedInputs.Snapshot().textures.at(selectedIndex).guest_format == 31 &&
             selectedInputs.Snapshot().textures.at(selectedIndex).host_format == 33 &&
             selectedInputs.Snapshot().events.back().draw.texture_uses.front().is_signed));

    auto rawSignFetch = selectedFetch;
    rawSignFetch[0] &= ~(0xFFU << 2);
    auto rawSignMismatch        = selectedTexture;
    rawSignMismatch.fetch_words = rawSignFetch;
    auto rawSignDraw            = selectedDraw;
    rawSignDraw.id              = 7;
    rawSignDraw.textures        = { &rawSignMismatch, 1 };
    NativeGameplayLiveInputs rawSignInputs;
    UNSCOPED_INFO("native collector rejects raw RG16 float sign bits that disagree with signed metadata");
    REQUIRE((rawSignInputs.Begin(boundary, error) && !rawSignInputs.PreparedDraw(rawSignDraw, error)));

    auto rawSwizzleFetch = selectedFetch;
    rawSwizzleFetch[3] ^= 1U << 1;
    auto rawSwizzleMismatch        = selectedTexture;
    rawSwizzleMismatch.fetch_words = rawSwizzleFetch;
    auto rawSwizzleDraw            = selectedDraw;
    rawSwizzleDraw.id              = 8;
    rawSwizzleDraw.textures        = { &rawSwizzleMismatch, 1 };
    NativeGameplayLiveInputs rawSwizzleInputs;
    UNSCOPED_INFO("native collector rejects raw RG16 float swizzle that disagrees with mapping metadata");
    REQUIRE((rawSwizzleInputs.Begin(boundary, error) && !rawSwizzleInputs.PreparedDraw(rawSwizzleDraw, error)));
}

void bufferMemoryVersions()
{
    struct VersionMemory
    {
        std::uint64_t generation = 1;
        std::size_t   reads      = 0;
        std::uint8_t  value      = 0x11;
        bool          current = true, invalidateOnCopy = false;

        static bool check(const void* context, std::uint64_t generation) noexcept
        {
            const auto& self = *static_cast<const VersionMemory*>(context);
            return self.current && self.generation == generation;
        }

        static bool copy(void* context, std::uint32_t, std::span<std::uint8_t> bytes) noexcept
        {
            auto& self = *static_cast<VersionMemory*>(context);
            ++self.reads;
            std::fill(bytes.begin(), bytes.end(), self.value);
            if (self.invalidateOnCopy)
            {
                ++self.generation;
                self.invalidateOnCopy = false;
            }
            return true;
        }
    } memory;

    std::vector<std::uint32_t>         registers(0x5003), gamma(256);
    const std::array<std::uint32_t, 1> program{ 1 };
    DrawCaptureFrameInputSwapView      boundary;
    boundary.id        = 1;
    boundary.frame     = 1;
    boundary.registers = registers;
    boundary.gamma_256 = gamma;
    boundary.is_8bpc   = true;
    NativeGameplayLiveInputs inputs;
    std::string              error;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Begin(boundary, error));
    DrawCaptureFrameInputVertexFetchView vertex;
    vertex.physical_address = 0x100;
    vertex.size             = 16;
    vertex.memory           = { &memory, &VersionMemory::copy };
    vertex.memory_version   = { 1, 1, 0x100, 16, 0, 0, &memory, &VersionMemory::check };
    DrawCaptureFrameInputDrawView draw;
    draw.id            = 1;
    draw.frame         = 2;
    draw.registers     = registers;
    draw.vertex_shader = draw.pixel_shader = { true, 1, program };
    draw.original_host_issued              = true;
    draw.execution_status                  = DrawCaptureExecutionStatus::kIssued;
    draw.vertex_fetches                    = std::span(&vertex, 1);
    auto collect                           = [&](std::size_t reads)
    {
        const auto before = memory.reads;
        ++draw.id;
        UNSCOPED_INFO(error);
        REQUIRE(inputs.Draw(draw, error));
        UNSCOPED_INFO("buffer read count differs from validated reuse");
        REQUIRE(memory.reads - before == reads);
        return inputs.Snapshot().events.back().draw.draw.vertex_fetches.front().shared_bytes;
    };
    const auto first = collect(1);
    UNSCOPED_INFO("unchanged exact buffer version must share immutable bytes");
    REQUIRE(collect(0) == first);
    memory.value                     = 0x22;
    vertex.memory_version.generation = ++memory.generation;
    const auto changed               = collect(1);
    UNSCOPED_INFO("CPU writes must capture replacement bytes and preserve earlier events");
    REQUIRE((changed != first && first->front() == 0x11 && changed->front() == 0x22));
    UNSCOPED_INFO("replacement version must reuse after its first read");
    REQUIRE(collect(0) == changed);
    memory.current = false;
    collect(1);
    collect(1);
    memory.current                 = true;
    vertex.memory_version.identity = 2;
    UNSCOPED_INFO("new identity must read even when bytes deduplicate");
    REQUIRE(collect(1) == changed);
    collect(0);
    --vertex.memory_version.base_size;
    collect(1);
    ++vertex.memory_version.base_size;
    --vertex.memory_version.base_address;
    collect(1);
    ++vertex.memory_version.base_address;
    vertex.memory_version.mip_size = 1;
    collect(1);
    vertex.memory_version.mip_size = 0;
    vertex.memory_version.check    = nullptr;
    collect(1);
    collect(1);
    vertex.memory_version.check      = &VersionMemory::check;
    vertex.memory_version.generation = ++memory.generation;
    memory.invalidateOnCopy          = true;
    collect(1);
    vertex.memory_version.generation = memory.generation;
    collect(1);
    collect(0);

    draw.indexed               = true;
    draw.requested_index_count = 8;
    draw.index                 = { true, 0, 0, 16, 0x100, 32, vertex.memory, vertex.memory_version };
    collect(0);
    const auto& index = inputs.Snapshot().events.back().draw.draw.index;
    UNSCOPED_INFO("index reuse must preserve padding metadata and own only the submitted prefix");
    REQUIRE((index.bytes.size() == 16 && index.bytes.front() == 0x22 && index.dma_length == 32));
    draw.requested_index_count = 4;
    collect(1);
    draw.index.memory_version.base_size = 8;
    collect(1);
    collect(0);
    draw.index.memory_version = {};
    collect(1);
    collect(1);
    draw.cpu_data_complete = false;
    ++draw.id;
    const auto before = memory.reads;
    UNSCOPED_INFO("GPU-dependent geometry must reject cached CPU bytes");
    REQUIRE(!inputs.Draw(draw, error));
    UNSCOPED_INFO("rejected GPU input must not enter a memory reader");
    REQUIRE(memory.reads == before);
    draw.cpu_data_complete = true;
    draw.index             = {};
    draw.indexed           = false;
    boundary.id            = draw.id + 1;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Finish(boundary, error));
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Begin(boundary, error));
    draw.id = boundary.id;
    collect(1);
    UNSCOPED_INFO("frame reset must preserve externally held immutable bytes");
    REQUIRE(first->front() == 0x11);
}

void copyRectangleVersions()
{
    Memory                         memory;
    std::vector<std::uint32_t>     registers(0x5003);
    std::array<std::uint32_t, 256> gamma{};
    DrawCaptureFrameInputSwapView  boundary;
    boundary.id = boundary.frame = 1;
    boundary.registers           = registers;
    boundary.gamma_256           = gamma;
    boundary.is_8bpc             = true;
    NativeGameplayLiveInputs           inputs;
    std::string                        error;
    std::vector<DrawCaptureFrameEvent> callbacks;
    inputs.SetEventConsumer([&](const DrawCaptureFrameEvent& event, std::span<const std::uint32_t> bank, std::span<const DrawCaptureTexture>, const std::map<std::uint32_t, NativeDrawReplayTexture>&, std::string&)
                            {
                                if (event.kind == DrawCaptureFrameEvent::Kind::kCopy)
                                {
                                    UNSCOPED_INFO("native consumers borrow the callback bank independently of the compact journal");
                                    REQUIRE((event.draw.draw.registers.empty() && bank.size() == 0x5003));
                                    callbacks.push_back(event);
                                }
                                return true;
                            });
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Begin(boundary, error));
    DrawCaptureFrameInputCopyView copy;
    copy.id                 = 2;
    copy.frame              = 2;
    copy.registers          = registers;
    copy.original_succeeded = true;
    copy.written_address = registers[0x2319] = 0x100000;
    copy.written_length                      = 0x1000;
    copy.source_geometry                     = { 0, 0x200000, 24, { &memory, &Memory::copy } };
    memory.bytes[0x200000]                   = 0x11;
    const auto before                        = memory.reads;
    UNSCOPED_INFO(error);
    REQUIRE((inputs.Copy(copy, true, error) && callbacks.size() == 1));
    const auto first = callbacks[0].draw.draw.vertex_fetches[0].shared_bytes;
    UNSCOPED_INFO("copy callbacks own the current 24-byte rectangle before native admission");
    REQUIRE((first && first->size() == 24 && first->front() == 0x11 && memory.reads == before + 1));
    ++copy.id;
    memory.bytes[0x200000] = 0x22;
    UNSCOPED_INFO("same-address rectangles are read again even when all copy register words are unchanged");
    REQUIRE((inputs.Copy(copy, true, error) && callbacks.size() == 2 && memory.reads == before + 2 &&
             callbacks[1].draw.draw.vertex_fetches[0].shared_bytes != first && first->front() == 0x11 &&
             callbacks[1].draw.draw.vertex_fetches[0].Payload()[0] == 0x22));
    registers[0x231D] = 0x12345678;
    ++copy.id;
    UNSCOPED_INFO(error);
    REQUIRE((inputs.Copy(copy, true, error) && callbacks.size() == 3 && memory.reads == before + 3));
    registers.clear();
    registers.shrink_to_fit();
    std::fill(memory.bytes.begin(), memory.bytes.end(), 0xFF);
    UNSCOPED_INFO("destroyed callback banks and overwritten memory preserve exact journal state and earlier rectangle owners");
    REQUIRE((first->front() == 0x11 && callbacks[1].draw.draw.vertex_fetches[0].Payload()[0] == 0x22 &&
             inputs.Snapshot().events.at(1).draw.draw.registers[0x231D] == 0 &&
             inputs.Snapshot().events.at(3).draw.draw.registers[0x231D] == 0x12345678));
}

void textureMemoryVersions()
{
    using namespace rerevved::gpu;
    using namespace rex::graphics::diagnostic;

    struct Memory
    {
        std::uint64_t generation = 1;
        std::size_t   copies     = 0;
        bool          current = true, invalidateOnCopy = false;
        std::uint8_t  base = 0x11, mip = 0x12;

        static bool check(const void* context, std::uint64_t generation) noexcept
        {
            const auto& self = *static_cast<const Memory*>(context);
            return self.current && self.generation == generation;
        }

        static bool copy(void* context, std::uint32_t address, std::span<std::uint8_t> bytes) noexcept
        {
            auto& self = *static_cast<Memory*>(context);
            ++self.copies;
            std::fill(bytes.begin(), bytes.end(), address == 0x100000 ? self.base : self.mip);
            if (self.invalidateOnCopy)
            {
                ++self.generation;
                self.invalidateOnCopy = false;
            }
            return true;
        }
    } memory;

    NativeTextureFetch        fetch{ 2U | (1U << 22), 0x00100006, 3U | (3U << 13), 0xD10, 1U << 6, 0x00200200 };
    NativeTextureMemoryRanges ranges;
    std::string               error;
    UNSCOPED_INFO(error);
    REQUIRE(GetNativeTextureMemoryRanges(fetch, ranges, error));
    DrawCaptureFrameInputTextureView texture;
    texture.fetch_words  = fetch;
    texture.base_address = ranges.base.address;
    texture.base_size    = ranges.base.size;
    texture.mip_address  = ranges.mips.address;
    texture.mip_size     = ranges.mips.size;
    texture.width = texture.height = 4;
    texture.depth                  = 1;
    texture.format                 = 6;
    texture.dimension              = 1;
    texture.memory                 = { &memory, &Memory::copy };
    texture.memory_version         = { 1, 1, ranges.base.address, ranges.base.size, ranges.mips.address, ranges.mips.size, &memory, &Memory::check };
    std::vector<std::uint32_t>    registers(0x5003), gamma(256);
    DrawCaptureFrameInputSwapView boundary;
    boundary.id        = 1;
    boundary.frame     = 1;
    boundary.registers = registers;
    boundary.gamma_256 = gamma;
    boundary.is_8bpc   = true;
    diagnostics::NativeGameplayLiveInputs inputs;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Begin(boundary, error));
    const std::array<std::uint32_t, 1> program{ 1 };
    DrawCaptureFrameInputDrawView      draw;
    draw.id            = 1;
    draw.frame         = 2;
    draw.registers     = registers;
    draw.vertex_shader = draw.pixel_shader = { true, 1, program };
    draw.original_host_issued              = true;
    draw.execution_status                  = DrawCaptureExecutionStatus::kIssued;
    draw.textures                          = { &texture, 1 };
    {
        diagnostics::NativeGameplayLiveInputs accounting;
        auto                                  accountingDraw = draw;
        accountingDraw.id                                    = 2;
        UNSCOPED_INFO(error);
        REQUIRE((accounting.Begin(boundary, error) && accounting.Draw(accountingDraw, error)));
        const auto version     = accounting.Textures().at(0).textureVersion;
        const auto owned       = version->Payload()->size() + version->MetadataBytes();
        const auto shaderBytes = 2 * program.size() * sizeof(std::uint32_t);
        UNSCOPED_INFO("live current bytes charge payload, version metadata and raw comparison ranges exactly once");
        REQUIRE(accounting.OwnedBytes() == accounting.Snapshot().events.StoredBytes() + shaderBytes + owned + ranges.base.size + ranges.mips.size);
        auto accountingBoundary = boundary;
        accountingBoundary.id   = 3;
        UNSCOPED_INFO("finish releases comparison bytes while retaining the complete version charge");
        REQUIRE((accounting.Finish(accountingBoundary, error) &&
                 accounting.OwnedBytes() == accounting.Snapshot().events.StoredBytes() + shaderBytes + owned));
        UNSCOPED_INFO("previous-frame carry retains both immutable layout metadata and payload charges");
        REQUIRE((accounting.BeginAfterCompletion(error) && accounting.OwnedBytes() == accounting.Snapshot().events.StoredBytes() + owned));
        accountingDraw.id = 4;
        UNSCOPED_INFO("current reuse transfers the complete charge without duplicating version metadata");
        REQUIRE((accounting.Draw(accountingDraw, error) && accounting.Textures().at(0).textureVersion == version &&
                 accounting.OwnedBytes() == accounting.Snapshot().events.StoredBytes() + shaderBytes + owned));
        accountingBoundary.id = 5;
        UNSCOPED_INFO(error);
        REQUIRE((accounting.Finish(accountingBoundary, error) && accounting.BeginAfterCompletion(error)));
        auto gpuTexture         = texture;
        gpuTexture.gpu_written  = true;
        accountingDraw.textures = { &gpuTexture, 1 };
        accountingDraw.id       = 6;
        UNSCOPED_INFO("GPU ownership retires the previous CPU payload and its version metadata together");
        REQUIRE((accounting.Draw(accountingDraw, error) && accounting.Textures().empty() &&
                 accounting.OwnedBytes() == accounting.Snapshot().events.StoredBytes() + shaderBytes));
        UNSCOPED_INFO("retirement cannot alter an independently retained earlier immutable version");
        REQUIRE((version->Payload()->front() == 0x11 && version->Payload()->back() == 0x12));
    }
    const auto collect = [&](std::size_t expectedCopies)
    {
        const auto before = memory.copies;
        ++draw.id;
        UNSCOPED_INFO(error);
        REQUIRE(inputs.Draw(draw, error));
        UNSCOPED_INFO("texture reuse must obey version validity and footprint");
        REQUIRE(memory.copies - before == expectedCopies);
        return inputs.Snapshot().events.back().draw.texture_uses.front().texture_index;
    };
    const auto first        = collect(2);
    const auto firstVersion = inputs.Textures().at(first).textureVersion;
    UNSCOPED_INFO("live CPU decoder publishes a private immutable version");
    REQUIRE((firstVersion && firstVersion->Payload() == inputs.Textures().at(first).sharedBytes));
    for (int i = 0; i < 16; ++i)
    {
        UNSCOPED_INFO("unchanged texture must retain its exact immutable owned version");
        REQUIRE((collect(0) == first && inputs.Textures().at(first).textureVersion == firstVersion));
    }
    memory.base                       = 0x21;
    texture.memory_version.generation = ++memory.generation;
    const auto baseChanged            = collect(2);
    UNSCOPED_INFO("CPU overwrite must preserve previous bytes and capture the replacement");
    REQUIRE((baseChanged != first && inputs.Textures().at(first).payloadBytes().front() == 0x11 &&
             inputs.Textures().at(baseChanged).payloadBytes().front() == 0x21 &&
             inputs.Textures().at(baseChanged).textureVersion != firstVersion));
    memory.mip                        = 0x22;
    texture.memory_version.generation = ++memory.generation;
    const auto mipChanged             = collect(2);
    UNSCOPED_INFO("mip-only write must invalidate the full texture version");
    REQUIRE((mipChanged != baseChanged && inputs.Textures().at(mipChanged).payloadBytes().back() == 0x22));
    const auto mipVersion = inputs.Textures().at(mipChanged).textureVersion;
    UNSCOPED_INFO("mip-only CPU mutation publishes a replacement immutable version");
    REQUIRE((mipVersion && mipVersion != inputs.Textures().at(baseChanged).textureVersion));
    memory.current = false;
    collect(2);
    collect(2);
    memory.current                  = true;
    texture.memory_version.identity = 2;
    UNSCOPED_INFO("cache recreation must reread even when bytes deduplicate");
    REQUIRE(collect(2) == mipChanged);
    collect(0);
    --texture.memory_version.mip_size;
    collect(2);
    ++texture.memory_version.mip_size;
    texture.memory_version.check = nullptr;
    collect(2);
    texture.memory_version.check = &Memory::check;
    fetch[3] ^= 1U << 1;
    collect(2);
    texture.gpu_written = true;
    const auto gpu      = collect(0);
    UNSCOPED_INFO("GPU-owned input must never reuse CPU bytes");
    REQUIRE(!inputs.Textures().contains(gpu));
    texture.gpu_written               = false;
    memory.base                       = 0x31;
    texture.memory_version.generation = ++memory.generation;
    UNSCOPED_INFO("CPU overwrite after GPU ownership must supply CPU bytes");
    REQUIRE(collect(2) != gpu);
    texture.memory_version.generation = ++memory.generation;
    memory.invalidateOnCopy           = true;
    collect(2);
    texture.memory_version.generation = memory.generation;
    collect(2);
    const auto latest          = collect(0);
    const auto retained        = inputs.Textures().at(latest).sharedBytes;
    const auto retainedVersion = inputs.Textures().at(latest).textureVersion;
    boundary.id                = draw.id + 1;
    boundary.frame             = 2;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Finish(boundary, error));
    const auto finishedId = boundary.id;
    registers[0]          = 123;
    UNSCOPED_INFO("pending consumer copying must retain the completed owned boundary");
    REQUIRE(inputs.Snapshot().events.back().draw.draw.registers[0] == 0);
    {
        const auto                            eventCount = inputs.Snapshot().events.size();
        const auto                            ownedBytes = inputs.OwnedBytes();
        diagnostics::NativeGameplayLiveInputs nextInputs;
        UNSCOPED_INFO(error);
        REQUIRE(nextInputs.BeginAfterCompletion(inputs, error));
        UNSCOPED_INFO("overlapping input owners must preserve the complete old journal and use its owned boundary");
        REQUIRE((inputs.Snapshot().events.size() == eventCount && inputs.OwnedBytes() == ownedBytes &&
                 inputs.Snapshot().end_frame == 3 &&
                 nextInputs.Snapshot().start_frame == 2 && nextInputs.Snapshot().events.front().id == finishedId &&
                 nextInputs.Snapshot().events.front().draw.draw.registers[0] == 0));
        auto nextDraw           = draw;
        nextDraw.id             = finishedId + 1;
        const auto beforeCopies = memory.copies;
        UNSCOPED_INFO(error);
        REQUIRE(nextInputs.Draw(nextDraw, error));
        const auto nextIndex = nextInputs.Snapshot().events.back().draw.texture_uses.front().texture_index;
        UNSCOPED_INFO("overlapping owners may share decoded pixels only with the exact current memory version");
        REQUIRE((memory.copies == beforeCopies && nextInputs.Textures().at(nextIndex).sharedBytes == retained &&
                 nextInputs.Textures().at(nextIndex).textureVersion == retainedVersion));
        auto gpuTexture        = texture;
        gpuTexture.gpu_written = true;
        nextDraw.textures      = { &gpuTexture, 1 };
        ++nextDraw.id;
        UNSCOPED_INFO(error);
        REQUIRE(nextInputs.Draw(nextDraw, error));
        UNSCOPED_INFO("new GPU ownership cannot alter or reuse an older frame's CPU inputs");
        REQUIRE((!nextInputs.Textures().contains(nextInputs.Snapshot().events.back().draw.texture_uses.front().texture_index) &&
                 inputs.Textures().at(latest).sharedBytes == retained && inputs.Snapshot().events.size() == eventCount));
    }
    UNSCOPED_INFO(error);
    REQUIRE(inputs.BeginAfterCompletion(error));
    UNSCOPED_INFO("retirement must restart from the owned final boundary rather than borrowed SDK state");
    REQUIRE((inputs.Snapshot().start_frame == 2 && inputs.Snapshot().events.front().id == finishedId &&
             inputs.Snapshot().events.front().draw.draw.registers[0] == 0));
    UNSCOPED_INFO("an unfinished frame cannot be retired");
    REQUIRE(!inputs.BeginAfterCompletion(error));
    registers[0]    = 0;
    draw.id         = finishedId;
    const auto next = collect(0);
    UNSCOPED_INFO("completed frames must reuse immutable pixels only with a current full-footprint version");
    REQUIRE((inputs.Textures().at(next).sharedBytes == retained && inputs.Textures().at(next).textureVersion == retainedVersion));
    boundary.id = draw.id + 1;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Finish(boundary, error));
    memory.mip                        = 0x42;
    texture.memory_version.generation = ++memory.generation;
    boundary.id                       = 1;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Begin(boundary, error));
    const auto nextMip = collect(2);
    UNSCOPED_INFO("mip writes between frames must replace decoded pixels without changing prior owners");
    REQUIRE((inputs.Textures().at(nextMip).payloadBytes().back() == 0x42 && retained->back() == 0x22 &&
             inputs.Textures().at(nextMip).textureVersion != retainedVersion));
    boundary.id = draw.id + 1;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Finish(boundary, error));
    boundary.id = 1;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Begin(boundary, error));
    texture.gpu_written = true;
    const auto nextGpu  = collect(0);
    UNSCOPED_INFO("GPU ownership must reject prior-frame CPU pixels");
    REQUIRE(!inputs.Textures().contains(nextGpu));
    texture.gpu_written = false;
    collect(2);
    memory.current = false;
    collect(2);
    memory.current                  = true;
    texture.memory_version.identity = 3;
    collect(2);
    boundary.id    = draw.id + 1;
    boundary.frame = 2;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Finish(boundary, error));
    memory.current = false;
    memory.base = memory.mip          = 0xEE;
    texture.memory_version.generation = ++memory.generation;
    UNSCOPED_INFO("finished inputs must own bytes independently of borrowed callbacks");
    REQUIRE((retained->front() == 0x31 && retained->back() == 0x22));
    boundary.id    = 1;
    boundary.frame = 1;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Begin(boundary, error));
    memory.current = true;
    collect(2);
    boundary.id = draw.id + 1;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Finish(boundary, error));
    boundary.id = 1;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Begin(boundary, error));
    UNSCOPED_INFO("unused prior pixels must count against the owned byte bound");
    REQUIRE(inputs.OwnedBytes() > inputs.Snapshot().events.StoredBytes());
    ++boundary.id;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Finish(boundary, error));
    UNSCOPED_INFO("a completed frame must retire unused prior pixels");
    REQUIRE(inputs.OwnedBytes() == inputs.Snapshot().events.StoredBytes());
}

void sharedInputBudget()
{
    diagnostics::NativeGameplayLiveInputs inputs;
    std::vector<std::uint32_t>            registers(0x5003);
    std::array<std::uint32_t, 256>        gamma{};
    DrawCaptureFrameInputSwapView         boundary;
    boundary.id        = 1;
    boundary.frame     = 1;
    boundary.registers = registers;
    boundary.gamma_256 = gamma;
    boundary.is_8bpc   = true;
    std::string error;
    const auto  boundaryBytes = diagnostics::NativeGameplayLiveInputs::BoundaryByteBound();
    unsigned    reclaimed     = 0;
    inputs.SetOwnedByteBudget(boundaryBytes, [&](std::size_t requested)
                              {
                                  UNSCOPED_INFO("requested shared input allowance preserves 512 MiB");
                                  REQUIRE(requested <= diagnostics::NativeGameplayLiveInputs::MaxOwnedBytes);
                                  ++reclaimed;
                                  return diagnostics::NativeGameplayLiveInputs::MaxOwnedBytes;
                              });
    UNSCOPED_INFO("owned boundary must fit its conservative fork allowance");
    REQUIRE((inputs.Begin(boundary, error) && reclaimed == 0 && inputs.OwnedBytes() <= boundaryBytes));
    boundary.id    = 2;
    boundary.frame = 2;
    UNSCOPED_INFO("input growth must reclaim an older consumer owner before allocation");
    REQUIRE((inputs.Finish(boundary, error) && reclaimed == 1));
    diagnostics::NativeGameplayLiveInputs bounded;
    bounded.SetOwnedByteBudget(boundaryBytes, [](std::size_t)
                               {
                                   return diagnostics::NativeGameplayLiveInputs::BoundaryByteBound();
                               });
    boundary.id = 1;
    UNSCOPED_INFO(error);
    REQUIRE(bounded.Begin(boundary, error));
    const auto fixedBudget = bounded.OwnedBytes();
    bounded.SetOwnedByteBudget(fixedBudget, [=](std::size_t)
                               {
                                   return fixedBudget;
                               });
    ++boundary.id;
    UNSCOPED_INFO("failed reclamation must reject input growth within the shared bound");
    REQUIRE((!bounded.Finish(boundary, error) && error.find("byte bound") != std::string::npos && bounded.OwnedBytes() <= boundaryBytes));
}

void terrainDiscovery()
{
    for (const auto page : { 0x100U, 0x18000U })
    {
        NativeGameplayPriorCapture    capture(0, {});
        std::vector<std::uint32_t>    registers(0x5003), gamma(256);
        DrawCaptureFrameInputSwapView swap;
        swap.id        = 1;
        swap.frame     = 1;
        swap.registers = registers;
        swap.gamma_256 = gamma;
        swap.is_8bpc   = true;
        capture.Swap(swap);
        Memory                               memory;
        DrawCaptureFrameInputVertexFetchView vertex{ 0, 0, 12, { &memory, &Memory::copy } };
        const std::array<std::uint32_t, 1>   words{ 1 };
        DrawCaptureFrameInputDrawView        draw;
        draw.registers            = registers;
        draw.vertex_fetches       = { &vertex, 1 };
        draw.vertex_shader        = { true, 1, words };
        draw.pixel_shader         = { true, 2, words };
        draw.original_host_issued = true;
        draw.execution_status     = DrawCaptureExecutionStatus::kIssued;
        for (std::uint64_t id = 2; id < 100; ++id)
        {
            draw.id = id;
            capture.Draw(draw);
        }
        swap.id = 100;
        ++swap.frame;
        capture.Swap(swap);
        UNSCOPED_INFO("waiting for terrain must not copy unrelated gameplay input payloads");
        REQUIRE((memory.reads == 0 && !capture.Captured() && !capture.Done()));
        draw.id                 = 101;
        draw.vertex_shader.hash = 0x1E6883FCCDE1F688ULL;
        draw.pixel_shader.hash  = 0xA4A965C189287B99ULL;
        registers[0x2000]       = 0x08000230;
        capture.Draw(draw);
        draw.id           = 102;
        registers[0x2000] = 0x04020118;
        capture.Draw(draw);
        draw.id                 = 103;
        registers[0x2000]       = 0x08000230;
        draw.vertex_shader.hash = 0xDBCF02DFB9E3F8FBULL;
        draw.pixel_shader.hash  = 0xF375BB83352BFB1BULL;
        capture.Draw(draw);
        DrawCaptureFrameInputCopyView copy;
        copy.id                 = 104;
        copy.registers          = registers;
        copy.copy_mode          = "resolve";
        copy.original_succeeded = true;
        copy.written_address    = page << 12;
        copy.written_length     = 0x100000;
        registers[0x2319]       = copy.written_address;
        registers[0x231A]       = 0x02000200;
        capture.Copy(copy);
        swap.id = 105;
        ++swap.frame;
        capture.Swap(swap);
        UNSCOPED_INFO("terrain discovery must use the observed allocation instead of a fixture page");
        REQUIRE((capture.Done() && capture.Captured() && capture.CapturedError().empty() && capture.Page() == page));
        UNSCOPED_INFO("the producer must own both leading clears, terrain draw and resolve");
        REQUIRE((capture.Captured()->Snapshot().events.size() == 6 && memory.reads == 3));
    }
    for (const bool leadingClear : { false, true })
    {
        NativeGameplayPriorCapture    capture(0, {});
        std::vector<std::uint32_t>    registers(0x5003), gamma(256);
        DrawCaptureFrameInputSwapView swap;
        swap.id        = 1;
        swap.frame     = 1;
        swap.registers = registers;
        swap.gamma_256 = gamma;
        swap.is_8bpc   = true;
        capture.Swap(swap);
        const std::array<std::uint32_t, 1> words{ 1 };
        DrawCaptureFrameInputDrawView      draw;
        draw.id                   = 2;
        draw.registers            = registers;
        draw.original_host_issued = true;
        draw.execution_status     = DrawCaptureExecutionStatus::kIssued;
        if (leadingClear)
        {
            registers[0x2000]  = 0x08000230;
            draw.vertex_shader = { true, 0x1E6883FCCDE1F688ULL, words };
            draw.pixel_shader  = { true, 0xA4A965C189287B99ULL, words };
            capture.Draw(draw);
            ++draw.id;
        }
        draw.vertex_shader = { true, 0xDBCF02DFB9E3F8FBULL, words };
        draw.pixel_shader  = { true, 0xF375BB83352BFB1BULL, words };
        capture.Draw(draw);
        swap.id = draw.id + 1;
        ++swap.frame;
        capture.Swap(swap);
        UNSCOPED_INFO("incomplete observed terrain must produce bounded failure inputs instead of waiting forever");
        REQUIRE((capture.Done() && capture.Captured() && !capture.CapturedError().empty() && !capture.Page()));
    }
}

} // namespace

TEST_CASE("native_gameplay_fixture_inputs", "[ctest][native-gameplay-fixture-inputs]")
{
    eventTimings();
    preparedSwapOwnership();
    preparedSwapFailures();
    preparedDrawOwnership();
    preparedResources();
    preparedCreationWriters();
    preparedCubeResources();
    preparedNativeSource();
    bufferMemoryVersions();
    copyRectangleVersions();
    textureMemoryVersions();
    sharedInputBudget();
    terrainDiscovery();
    Memory                             memory;
    const DrawCaptureGuestMemoryReader reader{ &memory, &Memory::copy };
    std::vector<std::uint32_t>         registers(0x5003);
    std::array<std::uint32_t, 3>       words{ 1, 2, 3 };
    const NativeTextureFetch           fetch{ 2U | (1U << 22), 0x00100006, 0, 0xD10, 0, 0x200 };
    NativeTextureMemoryRanges          ranges;
    std::string                        error;
    UNSCOPED_INFO(error);
    REQUIRE(GetNativeTextureMemoryRanges(fetch, ranges, error));
    memory.bytes[0x100000] = 10;
    memory.bytes[0x100001] = 20;
    memory.bytes[0x100002] = 30;
    memory.bytes[0x100003] = 40;
    memory.bytes[0x200000] = 91;
    DrawCaptureFrameInputTextureView texture;
    texture.fetch_words = fetch;
    texture.width = texture.height = texture.depth = 1;
    texture.format                                 = 6;
    texture.dimension                              = 1;
    texture.pitch                                  = 1;
    texture.base_address                           = ranges.base.address;
    texture.base_size                              = ranges.base.size;
    texture.memory                                 = reader;
    DrawCaptureFrameInputVertexFetchView vertex{ 95, 0x200000, 16, reader };
    DrawCaptureFrameInputDrawView        draw;
    draw.id                    = 2;
    draw.frame                 = 40;
    draw.submission            = 100;
    draw.registers             = registers;
    draw.vertex_shader         = { true, 1, words };
    draw.pixel_shader          = { true, 2, words };
    draw.primitive_type        = 4;
    draw.requested_index_count = 3;
    draw.original_host_issued  = true;
    draw.execution_status      = DrawCaptureExecutionStatus::kIssued;
    draw.vertex_fetches        = std::span(&vertex, 1);
    draw.textures              = std::span(&texture, 1);
    std::array<std::uint32_t, 256> gamma{};
    DrawCaptureFrameInputSwapView  boundary;
    boundary.id                 = 1;
    boundary.frame              = 40;
    boundary.submission         = 99;
    boundary.frontbuffer_width  = 1280;
    boundary.frontbuffer_height = 720;
    boundary.is_8bpc            = true;
    boundary.gamma_256          = gamma;
    boundary.registers          = registers;
    NativeGameplayLiveInputs inputs;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Begin(boundary, error));
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Draw(draw, error));
    draw.id = 3;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Draw(draw, error));
    UNSCOPED_INFO("unchanged point-of-use texture bytes must share owned storage");
    REQUIRE(inputs.Textures().size() == 1);
    UNSCOPED_INFO("repeated vertex data must share one immutable owned payload");
    REQUIRE((inputs.Snapshot().events[1].draw.draw.vertex_fetches[0].shared_bytes ==
                 inputs.Snapshot().events[2].draw.draw.vertex_fetches[0].shared_bytes &&
             inputs.Snapshot().events[1].draw.draw.vertex_fetches[0].bytes.empty()));
    const auto original = inputs.Textures().at(0).sharedBytes;
    UNSCOPED_INFO("canonical texture must own decoded CPU pixels");
    REQUIRE((original && original->size() == 4 && (*original)[0] == 10));
    memory.bytes[0x100000] = 99;
    memory.bytes[0x200000] = 77;
    words[0]               = 9;
    registers[0]           = 7;
    draw.id                = 4;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Draw(draw, error));
    UNSCOPED_INFO("a guest texture update must retain both point-of-use versions");
    REQUIRE((inputs.Textures().size() == 2 && (*original)[0] == 10 &&
             inputs.Textures().at(1).payloadBytes()[0] == 99));
    const auto& first = inputs.Snapshot().events.at(1).draw.draw;
    UNSCOPED_INFO("owned geometry, shader and register inputs must survive guest mutation");
    REQUIRE((first.vertex_fetches.at(0).Payload()[0] == 91 && first.vertex_ucode[0] == 1 && first.registers[0] == 0));
    boundary.id             = 5;
    boundary.frame          = 41;
    const auto beforeFinish = inputs.OwnedBytes();
    const auto beforeState  = inputs.Snapshot().events.StoredBytes();
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Finish(boundary, error));
    UNSCOPED_INFO("completed frames release both raw texture comparison versions while retaining replay inputs");
    REQUIRE(inputs.OwnedBytes() + 2 * (ranges.base.size + ranges.mips.size) ==
            beforeFinish + inputs.Snapshot().events.StoredBytes() - beforeState);
    UNSCOPED_INFO("completed input frames are immutable until the next Begin");
    REQUIRE((!inputs.Finish(boundary, error) && !inputs.Draw(draw, error)));
    UNSCOPED_INFO("swap inputs retain owned registers and an exclusive frame end boundary");
    REQUIRE((inputs.Snapshot().end_frame == 42 && inputs.Snapshot().events.back().frame == 41 &&
             inputs.Snapshot().events.back().draw.draw.registers[0] == 7));
    UNSCOPED_INFO("CPU input snapshots must not invent host GPU capture payloads");
    REQUIRE((inputs.Snapshot().final_swap.output_bytes.empty() && inputs.Snapshot().textures.at(0).bytes.empty()));

    {
        NativeGameplayLiveInputs large;
        auto                     start = boundary;
        start.id                       = 1;
        start.frame                    = 100;
        UNSCOPED_INFO(error);
        REQUIRE(large.Begin(start, error));
        auto repeated                     = draw;
        repeated.frame                    = 101;
        constexpr std::uint32_t drawCount = NativeGameplayBatchEvents + 3;
        for (std::uint32_t i = 1; i <= drawCount; ++i)
        {
            registers[0] = i;
            if (i == NativeGameplayBatchEvents)
            {
                memory.bytes[0x100000] = 35;
                memory.bytes[0x200000] = 36;
            }
            repeated.id = i + 1;
            UNSCOPED_INFO(error);
            REQUIRE(large.Draw(repeated, error));
        }
        start.id    = drawCount + 2;
        start.frame = 101;
        UNSCOPED_INFO(error);
        REQUIRE(large.Finish(start, error));
        UNSCOPED_INFO("large input frames retain every event across bounded batches");
        REQUIRE((large.Snapshot().events.BatchCount() == 2 &&
                 large.Snapshot().events.size() == drawCount + 2));
        std::size_t index = 0;
        for (const auto& event : large.Snapshot().events)
        {
            if (index && index <= drawCount)
            {
                UNSCOPED_INFO("delta cursor must reconstruct every event in order");
                REQUIRE(event.draw.draw.registers[0] == index);
            }
            ++index;
        }
        const auto before = large.Snapshot().events.at(NativeGameplayBatchEvents - 1);
        const auto after  = large.Snapshot().events.at(NativeGameplayBatchEvents);
        UNSCOPED_INFO("a CPU overwrite at a batch edge must preserve both resource versions");
        REQUIRE((before.draw.draw.vertex_fetches[0].Payload()[0] == 77 &&
                 after.draw.draw.vertex_fetches[0].Payload()[0] == 36 &&
                 before.draw.texture_uses[0].texture_index != after.draw.texture_uses[0].texture_index));
        UNSCOPED_INFO("sparse state must not retain expanded per-event register banks");
        REQUIRE(large.OwnedBytes() < (drawCount * registers.size() * sizeof(std::uint32_t)) / 8);
        NativeGameplayEvents bounded;
        bool                 rejected = false;
        try
        {
            bounded.push_back(after, 1);
        }
        catch (const std::runtime_error&)
        {
            rejected = true;
        }
        UNSCOPED_INFO("byte admission failure must not publish an event");
        REQUIRE((rejected && bounded.empty()));
        bounded.push_back(after);
        auto dense = after;
        ++dense.id;
        for (auto& word : dense.draw.draw.registers)
            word ^= 0xFFFFFFFF;
        bounded.push_back(dense);
        UNSCOPED_INFO("dense register updates must reconstruct through full-state fallback");
        REQUIRE(bounded.back().draw.draw.registers == dense.draw.draw.registers);
        auto tail = bounded.back();
        tail.draw.draw.registers[0] ^= 1;
        auto rejectedTail = tail;
        ++rejectedTail.id;
        rejected = false;
        try
        {
            bounded.push_back(rejectedTail, 1);
        }
        catch (const std::runtime_error&)
        {
            rejected = true;
        }
        UNSCOPED_INFO("returned tail mutation and rejected admission must preserve the owned final bank");
        REQUIRE((rejected && bounded.back().id == dense.id &&
                 bounded.back().draw.draw.registers == dense.draw.draw.registers));

        NativeGameplayEvents borrowed;
        auto                 metadata = after;
        auto                 callback = std::move(metadata.draw.draw.registers);
        rejected                      = false;
        try
        {
            borrowed.push_back(metadata, callback, 1);
        }
        catch (const std::runtime_error&)
        {
            rejected = true;
        }
        UNSCOPED_INFO("borrowed byte admission failure must not publish an event");
        REQUIRE((rejected && borrowed.empty()));
        borrowed.push_back(metadata, callback);
        ++metadata.id;
        for (auto& word : callback)
            word ^= 0xFFFFFFFF;
        borrowed.push_back(metadata, callback);
        const auto denseRegisters = callback;
        ++metadata.id;
        callback[0] ^= 1;
        const auto sparseRegisters = callback;
        borrowed.push_back(metadata, callback);
        const auto storedBytes = borrowed.StoredBytes();
        ++metadata.id;
        callback.back() ^= 1;
        rejected = false;
        try
        {
            borrowed.push_back(metadata, callback, 1);
        }
        catch (const std::runtime_error&)
        {
            rejected = true;
        }
        std::fill(callback.begin(), callback.end(), 0);
        UNSCOPED_INFO("borrowed checkpoints, dense and sparse updates own their banks after return and rejection");
        REQUIRE((rejected && borrowed.size() == 3 && borrowed.StoredBytes() == storedBytes &&
                 borrowed.at(0).draw.draw.registers == after.draw.draw.registers &&
                 borrowed.at(1).draw.draw.registers == denseRegisters &&
                 borrowed.back().draw.draw.registers == sparseRegisters));
        const auto final = borrowed.ReleaseFinalEvent();
        UNSCOPED_INFO("retiring borrowed inputs must move the final owned bank");
        REQUIRE((borrowed.empty() && borrowed.StoredBytes() == 0 &&
                 final.draw.draw.registers == sparseRegisters));
        std::cout << "Large input events=" << large.Snapshot().events.size()
                  << " batches=" << large.Snapshot().events.BatchCount()
                  << " owned_bytes=" << large.OwnedBytes() << '\n';
        registers[0]           = 7;
        memory.bytes[0x100000] = 99;
        memory.bytes[0x200000] = 77;
    }

    {
        NativeGameplayLiveInputs indexedInputs;
        UNSCOPED_INFO(error);
        REQUIRE(indexedInputs.Begin(boundary, error));
        auto indexedDraw                   = draw;
        indexedDraw.id                     = boundary.id + 1;
        indexedDraw.indexed                = true;
        indexedDraw.requested_index_count  = 48;
        indexedDraw.index.present          = true;
        indexedDraw.index.format           = 1;
        indexedDraw.index.endianness       = 2;
        indexedDraw.index.count            = 48;
        indexedDraw.index.physical_address = 0x200000;
        indexedDraw.index.size             = 384;
        indexedDraw.index.memory           = reader;
        UNSCOPED_INFO(error);
        REQUIRE(indexedInputs.Draw(indexedDraw, error));
        const auto& indices = indexedInputs.Snapshot().events.back().draw.draw.index;
        UNSCOPED_INFO("48 32-bit indices must retain 192 bytes from a padded 384-byte DMA allocation");
        REQUIRE((indices.bytes.size() == 192 && indices.count == 48 && indices.dma_length == 384));
        ++indexedDraw.id;
        indexedDraw.index.size = 191;
        UNSCOPED_INFO("short index allocation must fail closed");
        REQUIRE(!indexedInputs.Draw(indexedDraw, error));
        indexedDraw.index.size  = 384;
        indexedDraw.index.count = 47;
        UNSCOPED_INFO("short DMA index count must fail closed");
        REQUIRE(!indexedInputs.Draw(indexedDraw, error));
    }

    boundary.id = 6;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Begin(boundary, error));
    registers[0x2319] = 0x100000;
    DrawCaptureFrameInputCopyView copy;
    copy.id        = 7;
    copy.registers = registers;
    UNSCOPED_INFO("failed original resolve must not become a native producer");
    REQUIRE(!inputs.Copy(copy, true, error));
    UNSCOPED_INFO("failed pre-arm resolve must not create a producer");
    REQUIRE(inputs.Copy(copy, false, error));
    copy.original_succeeded = true;
    UNSCOPED_INFO("unsupported clear-only resolve must not become a texture producer");
    REQUIRE(!inputs.Copy(copy, true, error));
    UNSCOPED_INFO("rejected copies must not publish frame events");
    REQUIRE(inputs.Snapshot().events.size() == 1);
    copy.written_address = 0x100000;
    copy.written_length  = 4;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Copy(copy, true, error));
    const auto copyRegister = registers[0];
    ++registers[0];
    UNSCOPED_INFO("resolve register inputs must remain owned after callback mutation");
    REQUIRE(inputs.Snapshot().events.back().draw.draw.registers[0] == copyRegister);
    registers[0]        = copyRegister;
    texture.gpu_written = true;
    draw.id             = 8;
    draw.vertex_fetches = {};
    memory.fail         = true;
    auto readsBefore    = memory.reads;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Draw(draw, error));
    UNSCOPED_INFO("native resolve inputs must never read stale CPU bytes for a GPU-written texture");
    REQUIRE((inputs.Textures().empty() && memory.reads == readsBefore));
    UNSCOPED_INFO("live GPU metadata without pixels must not report a complete host payload");
    REQUIRE((inputs.Snapshot().textures.at(0).availability == DrawCaptureAvailability::kUnavailable &&
             inputs.Snapshot().textures.at(0).unavailable_reason == DrawCaptureReason::kTextureSource));
    UNSCOPED_INFO("generated texture retains its guest identity for producer matching");
    REQUIRE(inputs.Snapshot().textures.at(0).guest_base_page == 0x100);
    boundary.id = 9;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Finish(boundary, error));
    boundary.id = 10;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Begin(boundary, error));
    draw.id = 11;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Draw(draw, error));
    UNSCOPED_INFO("a previous frame GPU write must not become a static CPU input after frame reset");
    REQUIRE((inputs.Textures().empty() && memory.reads == readsBefore));
    texture.gpu_written = false;
    memory.fail         = false;
    draw.id             = 12;
    UNSCOPED_INFO(error);
    REQUIRE(inputs.Draw(draw, error));
    UNSCOPED_INFO("a CPU rewrite must replace historical GPU ownership and provide fresh input bytes");
    REQUIRE((inputs.Textures().size() == 1 && inputs.Textures().begin()->second.payloadBytes()[0] == 99 &&
             memory.reads > readsBefore));
    readsBefore            = memory.reads;
    draw.id                = 13;
    draw.cpu_data_complete = false;
    UNSCOPED_INFO("incomplete or memexport-dependent CPU geometry must fail closed");
    REQUIRE(!inputs.Draw(draw, error));
    draw.cpu_data_complete  = true;
    vertex.physical_address = 0x1FFFFFFF;
    draw.vertex_fetches     = std::span(&vertex, 1);
    memory.fail             = false;
    UNSCOPED_INFO("physical address overflow must fail before memory access");
    REQUIRE(!inputs.Draw(draw, error));
    UNSCOPED_INFO("invalid physical ranges must not reach the memory callback");
    REQUIRE(memory.reads == readsBefore);
}
