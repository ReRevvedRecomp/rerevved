#include "gpu/diagnostics/native_renderer_passive_trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

using rerevved::gpu::diagnostics::PassiveTraceBuffer;
using rerevved::gpu::diagnostics::PassiveTraceEvent;
using rerevved::gpu::diagnostics::PassiveTracePoint;

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "native_renderer_passive_trace_test: " << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path TestPath(const char* name)
{
    return std::filesystem::temp_directory_path() /
           (std::string("rerevved-passive-trace-") + name + ".csv");
}

void RemoveTestOutput(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::path partial = path;
    partial += ".partial";
    std::filesystem::remove(partial, error);
}

struct TraceRow
{
    std::uint64_t sequence = 0;
    std::uint64_t thread   = 0;
    std::uint32_t epoch    = 0;
    std::string   event;
};

std::vector<TraceRow> ReadTraceRows(const std::filesystem::path& path)
{
    std::ifstream         input(path);
    std::vector<TraceRow> rows;
    std::string           line;
    while (std::getline(input, line))
    {
        if (line.empty() || line[0] == '#' || line.starts_with("sequence,"))
        {
            continue;
        }
        std::istringstream stream(line);
        std::string        sequence;
        std::string        thread;
        std::string        epoch;
        std::string        event;
        Require(static_cast<bool>(std::getline(stream, sequence, ',')) &&
                    static_cast<bool>(std::getline(stream, thread, ',')) &&
                    static_cast<bool>(std::getline(stream, epoch, ',')) &&
                    static_cast<bool>(std::getline(stream, event, ',')),
                "serialized trace row prefix");
        rows.push_back({ std::stoull(sequence),
                         std::stoull(thread),
                         static_cast<std::uint32_t>(std::stoul(epoch)),
                         std::move(event) });
    }
    return rows;
}

} // namespace

int main()
{
    using namespace rerevved::gpu::diagnostics;

    auto              buffer = std::make_unique<PassiveTraceBuffer>();
    PassiveTraceEvent disabled_event{};
    disabled_event.point                    = PassiveTracePoint::kReservationEnter;
    disabled_event.device_position          = 0x11223344;
    const PassiveTraceEvent disabled_before = disabled_event;
    Require(!buffer->enabled(), "default closed");
    Require(!buffer->Record(disabled_event), "disabled record rejected");
    Require(disabled_event.device_position == disabled_before.device_position,
            "disabled input unchanged");
    Require(buffer->statistics().stored == 0 &&
                buffer->statistics().overflow == 0 &&
                buffer->statistics().last_sequence == 0,
            "disabled state inert");

    const std::filesystem::path basic_path = TestPath("basic");
    RemoveTestOutput(basic_path);
    Require(buffer->Start(basic_path), "basic start");
    Require(buffer->enabled(), "enabled after start");
    Require(!std::filesystem::exists(basic_path),
            "start performs no artifact write");

    PassiveTraceEvent resolve{};
    resolve.point                               = PassiveTracePoint::kResolveEnter;
    resolve.valid_fields                        = kTraceDescriptor;
    resolve.resolve_resource_address            = 0x40100000;
    resolve.descriptor_address                  = 0x4010001C;
    resolve.resolve_call_address                = 0x8250AFEC;
    resolve.resolve_flags                       = 0x10;
    resolve.resolve_mip_level                   = 2;
    resolve.resolve_slice                       = 3;
    resolve.descriptor                          = { 1, 2, 3, 4, 5, 6 };
    const PassiveTraceEvent      resolve_before = resolve;
    std::array<std::uint32_t, 4> guest_state    = {
        0x01020304,
        0x11223344,
        0x55667788,
        0xAABBCCDD,
    };
    const auto guest_state_before = guest_state;
    Require(buffer->Record(resolve), "resolve record");
    Require(resolve.descriptor == resolve_before.descriptor,
            "record input unchanged");
    Require(guest_state == guest_state_before, "guest state unchanged");

    PassiveTraceEvent published{};
    published.point        = PassiveTracePoint::kVdSwapPublished;
    published.valid_fields = kTraceReservationWords;
    for (std::size_t index = 0; index < published.reservation_words.size();
         ++index)
    {
        published.reservation_words[index] =
            0xA0000000u + static_cast<std::uint32_t>(index);
    }
    Require(buffer->Record(published), "post-emission reservation record");

    PassiveTraceEvent reset{};
    reset.point = PassiveTracePoint::kRingResetBegin;
    Require(buffer->BeginObservationEpoch(reset), "new epoch");
    PassiveTraceEvent after_reset{};
    after_reset.point = PassiveTracePoint::kRingResetReturn;
    Require(buffer->Record(after_reset), "post-reset record");

    Require(buffer->StopAndFlush(), "basic flush");
    Require(!buffer->enabled(), "closed after flush");
    Require(buffer->StopAndFlush(), "idempotent stop");
    Require(std::filesystem::exists(basic_path), "artifact published");
    Require(!buffer->Start(basic_path), "existing artifact preserved");

    std::ifstream     basic_input(basic_path);
    const std::string basic_text((std::istreambuf_iterator<char>(basic_input)),
                                 std::istreambuf_iterator<char>());
    Require(basic_text.find("# overflow=0") != std::string::npos,
            "zero overflow explicit");
    Require(basic_text.find("descriptor_05") != std::string::npos &&
                basic_text.find("reservation_63") != std::string::npos,
            "fixed descriptor and reservation schema");
    Require(basic_text.find("A0000000") != std::string::npos &&
                basic_text.find("A000003F") != std::string::npos,
            "exact 64-word snapshot serialized");
    const auto basic_rows = ReadTraceRows(basic_path);
    Require(basic_rows.size() == 5, "all basic records serialized");
    for (std::size_t index = 0; index < basic_rows.size(); ++index)
    {
        Require(basic_rows[index].sequence == index + 1,
                "serialized sequence is monotonic");
    }
    Require(basic_rows[0].epoch == 1 && basic_rows[1].epoch == 1 &&
                basic_rows[2].epoch == 1 && basic_rows[3].epoch == 2 &&
                basic_rows[4].epoch == 2,
            "serialized reset separates epochs");
    for (const char* forbidden : { "texture", "framebuffer", "filename", "shader", "xuid", "gamertag", "civilization", "save_bytes" })
    {
        Require(basic_text.find(forbidden) == std::string::npos,
                "forbidden field absent");
    }
    RemoveTestOutput(basic_path);

    buffer                                 = std::make_unique<PassiveTraceBuffer>();
    const std::filesystem::path epoch_path = TestPath("epoch-exclusive");
    RemoveTestOutput(epoch_path);
    Require(buffer->Start(epoch_path), "exclusive epoch start");
    auto old_epoch_lease = buffer->BeginRecord();
    Require(static_cast<bool>(old_epoch_lease), "old epoch lease acquired");
    std::atomic<bool> epoch_result{ false };
    std::thread       epoch_thread([&buffer, &epoch_result]()
                                   {
                                 PassiveTraceEvent reset_event{};
                                 reset_event.point =
                                     PassiveTracePoint::kRingResetBegin;
                                 epoch_result.store(
                                     buffer->BeginObservationEpoch(reset_event),
                                     std::memory_order_release);
                                   });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    PassiveTraceEvent competing_reset{};
    competing_reset.point = PassiveTracePoint::kRingResetBegin;
    Require(!buffer->BeginObservationEpoch(competing_reset),
            "concurrent epoch transition rejected");
    PassiveTraceEvent old_epoch_event{};
    old_epoch_event.point = PassiveTracePoint::kResolveEnter;
    Require(old_epoch_lease.Commit(old_epoch_event),
            "old epoch lease committed");
    epoch_thread.join();
    Require(epoch_result.load(std::memory_order_acquire),
            "epoch transition waited for prior capture");
    Require(buffer->StopAndFlush(), "exclusive epoch flush");
    const auto epoch_rows = ReadTraceRows(epoch_path);
    const auto old_row    = std::find_if(epoch_rows.begin(),
                                         epoch_rows.end(),
                                         [](const TraceRow& row)
                                         {
                                          return row.event == "resolve_enter";
                                         });
    const auto reset_row  = std::find_if(epoch_rows.begin(),
                                         epoch_rows.end(),
                                         [](const TraceRow& row)
                                         {
                                            return row.event == "ring_reset_begin";
                                         });
    Require(old_row != epoch_rows.end() && reset_row != epoch_rows.end() &&
                old_row->sequence < reset_row->sequence && old_row->epoch == 1 &&
                reset_row->epoch == 2,
            "epoch cannot mix an admitted stale record");
    RemoveTestOutput(epoch_path);

    buffer                                 = std::make_unique<PassiveTraceBuffer>();
    const std::filesystem::path retry_path = TestPath("flush-retry");
    RemoveTestOutput(retry_path);
    Require(buffer->Start(retry_path), "flush retry start");
    auto held_lease = buffer->BeginRecord();
    Require(static_cast<bool>(held_lease), "held writer acquired");
    Require(!buffer->StopAndFlush(), "in-flight flush fails without writing");
    Require(!std::filesystem::exists(retry_path),
            "failed drain publishes no artifact");
    PassiveTraceEvent held_event{};
    held_event.point = PassiveTracePoint::kVdSwapPublished;
    Require(held_lease.Commit(held_event), "held writer committed after close");
    Require(buffer->StopAndFlush(), "closed trace flush retries");
    Require(std::filesystem::exists(retry_path),
            "retry publishes drained artifact");
    RemoveTestOutput(retry_path);

    buffer                                      = std::make_unique<PassiveTraceBuffer>();
    const std::filesystem::path concurrent_path = TestPath("concurrent");
    RemoveTestOutput(concurrent_path);
    Require(buffer->Start(concurrent_path), "concurrent start");

    constexpr std::size_t    kThreadCount      = 8;
    constexpr std::size_t    kRecordsPerThread = 600;
    std::vector<std::thread> producers;
    producers.reserve(kThreadCount);
    for (std::size_t thread = 0; thread < kThreadCount; ++thread)
    {
        producers.emplace_back([&buffer, thread]()
                               {
                                   for (std::size_t index = 0;
                                        index < kRecordsPerThread;
                                        ++index)
                                   {
                                       PassiveTraceEvent event{};
                                       event.point =
                                           PassiveTracePoint::kEmitterCd20Enter;
                                       event.requested_dwords =
                                           static_cast<std::uint32_t>(thread);
                                       (void)buffer->Record(event);
                                   }
                               });
    }
    for (auto& producer : producers)
    {
        producer.join();
    }

    const PassiveTraceStatistics full = buffer->statistics();
    Require(full.stored == kPassiveTraceCapacity, "capacity bounded");
    Require(full.overflow ==
                1 + kThreadCount * kRecordsPerThread - kPassiveTraceCapacity,
            "overflow explicit and exact");

    Require(buffer->StopAndFlush(), "concurrent flush");

    std::ifstream     concurrent_input(concurrent_path);
    const std::string concurrent_text(
        (std::istreambuf_iterator<char>(concurrent_input)),
        std::istreambuf_iterator<char>());
    Require(concurrent_text.find("# overflow=705") != std::string::npos,
            "serialized overflow explicit");
    Require(concurrent_text.find("# in_flight_at_flush=0") !=
                std::string::npos,
            "serialized drain complete");
    const auto              concurrent_rows = ReadTraceRows(concurrent_path);
    std::set<std::uint64_t> sequences;
    bool                    saw_thread_identity = false;
    for (const auto& row : concurrent_rows)
    {
        sequences.insert(row.sequence);
        saw_thread_identity = saw_thread_identity || row.thread != 0;
    }
    Require(concurrent_rows.size() == kPassiveTraceCapacity,
            "all concurrent records serialized");
    Require(sequences.size() == kPassiveTraceCapacity,
            "concurrent sequences unique");
    Require(saw_thread_identity, "thread identity recorded");
    RemoveTestOutput(concurrent_path);

    std::cout << "native_renderer_passive_trace_test: PASS\n";
    return 0;
}
