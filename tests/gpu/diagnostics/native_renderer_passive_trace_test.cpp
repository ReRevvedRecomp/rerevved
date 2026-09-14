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

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "native_renderer_passive_trace_test: " << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path testPath(const char* name)
{
    return std::filesystem::temp_directory_path() /
           (std::string("rerevved-passive-trace-") + name + ".csv");
}

void removeTestOutput(const std::filesystem::path& path)
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

std::vector<TraceRow> readTraceRows(const std::filesystem::path& path)
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
        require(static_cast<bool>(std::getline(stream, sequence, ',')) &&
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
    PassiveTraceEvent disabledEvent{};
    disabledEvent.point                    = PassiveTracePoint::ReservationEnter;
    disabledEvent.devicePosition           = 0x11223344;
    const PassiveTraceEvent disabledBefore = disabledEvent;
    require(!buffer->Enabled(), "default closed");
    require(!buffer->Record(disabledEvent), "disabled record rejected");
    require(disabledEvent.devicePosition == disabledBefore.devicePosition,
            "disabled input unchanged");
    require(buffer->Statistics().stored == 0 &&
                buffer->Statistics().overflow == 0 &&
                buffer->Statistics().lastSequence == 0,
            "disabled state inert");

    const std::filesystem::path basicPath = testPath("basic");
    removeTestOutput(basicPath);
    require(buffer->Start(basicPath), "basic start");
    require(buffer->Enabled(), "enabled after start");
    require(!std::filesystem::exists(basicPath),
            "start performs no artifact write");

    PassiveTraceEvent resolve{};
    resolve.point                              = PassiveTracePoint::ResolveEnter;
    resolve.validFields                        = TRACE_DESCRIPTOR;
    resolve.resolveResourceAddress             = 0x40100000;
    resolve.descriptorAddress                  = 0x4010001C;
    resolve.resolveCallAddress                 = 0x8250AFEC;
    resolve.resolveFlags                       = 0x10;
    resolve.resolveMipLevel                    = 2;
    resolve.resolveSlice                       = 3;
    resolve.descriptor                         = { 1, 2, 3, 4, 5, 6 };
    const PassiveTraceEvent      resolveBefore = resolve;
    std::array<std::uint32_t, 4> guestState    = {
        0x01020304,
        0x11223344,
        0x55667788,
        0xAABBCCDD,
    };
    const auto guestStateBefore = guestState;
    require(buffer->Record(resolve), "resolve record");
    require(resolve.descriptor == resolveBefore.descriptor,
            "record input unchanged");
    require(guestState == guestStateBefore, "guest state unchanged");

    PassiveTraceEvent published{};
    published.point       = PassiveTracePoint::VdSwapPublished;
    published.validFields = TRACE_RESERVATION_WORDS;
    for (std::size_t index = 0; index < published.reservationWords.size();
         ++index)
    {
        published.reservationWords[index] =
            0xA0000000u + static_cast<std::uint32_t>(index);
    }
    require(buffer->Record(published), "post-emission reservation record");

    PassiveTraceEvent reset{};
    reset.point = PassiveTracePoint::RingResetBegin;
    require(buffer->BeginObservationEpoch(reset), "new epoch");
    PassiveTraceEvent afterReset{};
    afterReset.point = PassiveTracePoint::RingResetReturn;
    require(buffer->Record(afterReset), "post-reset record");

    require(buffer->StopAndFlush(), "basic flush");
    require(!buffer->Enabled(), "closed after flush");
    require(buffer->StopAndFlush(), "idempotent stop");
    require(std::filesystem::exists(basicPath), "artifact published");
    require(!buffer->Start(basicPath), "existing artifact preserved");

    std::ifstream     basicInput(basicPath);
    const std::string basicText((std::istreambuf_iterator<char>(basicInput)),
                                std::istreambuf_iterator<char>());
    require(basicText.find("# overflow=0") != std::string::npos,
            "zero overflow explicit");
    require(basicText.find("descriptor_05") != std::string::npos &&
                basicText.find("reservation_63") != std::string::npos,
            "fixed descriptor and reservation schema");
    require(basicText.find("A0000000") != std::string::npos &&
                basicText.find("A000003F") != std::string::npos,
            "exact 64-word snapshot serialized");
    const auto basicRows = readTraceRows(basicPath);
    require(basicRows.size() == 5, "all basic records serialized");
    for (std::size_t index = 0; index < basicRows.size(); ++index)
    {
        require(basicRows[index].sequence == index + 1,
                "serialized sequence is monotonic");
    }
    require(basicRows[0].epoch == 1 && basicRows[1].epoch == 1 &&
                basicRows[2].epoch == 1 && basicRows[3].epoch == 2 &&
                basicRows[4].epoch == 2,
            "serialized reset separates epochs");
    for (const char* forbidden : { "texture", "framebuffer", "filename", "shader", "xuid", "gamertag", "civilization", "save_bytes" })
    {
        require(basicText.find(forbidden) == std::string::npos,
                "forbidden field absent");
    }
    removeTestOutput(basicPath);

    buffer                                = std::make_unique<PassiveTraceBuffer>();
    const std::filesystem::path epochPath = testPath("epoch-exclusive");
    removeTestOutput(epochPath);
    require(buffer->Start(epochPath), "exclusive epoch start");
    auto oldEpochLease = buffer->BeginRecord();
    require(static_cast<bool>(oldEpochLease), "old epoch lease acquired");
    std::atomic<bool> epochResult{ false };
    std::atomic<bool> epochFinished{ false };
    std::thread       epochThread([&buffer, &epochResult, &epochFinished]()
                                  {
                                PassiveTraceEvent resetEvent{};
                                resetEvent.point =
                                    PassiveTracePoint::RingResetBegin;
                                epochResult.store(
                                    buffer->BeginObservationEpoch(resetEvent),
                                    std::memory_order_release);
                                epochFinished.store(true, std::memory_order_release);
                                  });
    // Observe the closed admission gate before attempting a competing reset.
    // A scheduling delay must not let this thread claim the transition first.
    while (buffer->Enabled() && !epochFinished.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    require(!buffer->Enabled(), "epoch transition closed admission");
    PassiveTraceEvent competingReset{};
    competingReset.point = PassiveTracePoint::RingResetBegin;
    require(!buffer->BeginObservationEpoch(competingReset),
            "concurrent epoch transition rejected");
    PassiveTraceEvent oldEpochEvent{};
    oldEpochEvent.point = PassiveTracePoint::ResolveEnter;
    require(oldEpochLease.Commit(oldEpochEvent),
            "old epoch lease committed");
    epochThread.join();
    require(epochResult.load(std::memory_order_acquire),
            "epoch transition waited for prior capture");
    require(buffer->StopAndFlush(), "exclusive epoch flush");
    const auto epochRows = readTraceRows(epochPath);
    const auto oldRow    = std::find_if(epochRows.begin(),
                                        epochRows.end(),
                                        [](const TraceRow& row)
                                        {
                                         return row.event == "resolve_enter";
                                        });
    const auto resetRow  = std::find_if(epochRows.begin(),
                                        epochRows.end(),
                                        [](const TraceRow& row)
                                        {
                                           return row.event == "ring_reset_begin";
                                        });
    require(oldRow != epochRows.end() && resetRow != epochRows.end() &&
                oldRow->sequence < resetRow->sequence && oldRow->epoch == 1 &&
                resetRow->epoch == 2,
            "epoch cannot mix an admitted stale record");
    removeTestOutput(epochPath);

    buffer                                = std::make_unique<PassiveTraceBuffer>();
    const std::filesystem::path retryPath = testPath("flush-retry");
    removeTestOutput(retryPath);
    require(buffer->Start(retryPath), "flush retry start");
    auto heldLease = buffer->BeginRecord();
    require(static_cast<bool>(heldLease), "held writer acquired");
    require(!buffer->StopAndFlush(), "in-flight flush fails without writing");
    require(!std::filesystem::exists(retryPath),
            "failed drain publishes no artifact");
    PassiveTraceEvent heldEvent{};
    heldEvent.point = PassiveTracePoint::VdSwapPublished;
    require(heldLease.Commit(heldEvent), "held writer committed after close");
    require(buffer->StopAndFlush(), "closed trace flush retries");
    require(std::filesystem::exists(retryPath),
            "retry publishes drained artifact");
    removeTestOutput(retryPath);

    buffer                                     = std::make_unique<PassiveTraceBuffer>();
    const std::filesystem::path concurrentPath = testPath("concurrent");
    removeTestOutput(concurrentPath);
    require(buffer->Start(concurrentPath), "concurrent start");

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
                                           PassiveTracePoint::EmitterCd20Enter;
                                       event.requestedDwords =
                                           static_cast<std::uint32_t>(thread);
                                       (void)buffer->Record(event);
                                   }
                               });
    }
    for (auto& producer : producers)
    {
        producer.join();
    }

    const PassiveTraceStatistics full = buffer->Statistics();
    require(full.stored == kPassiveTraceCapacity, "capacity bounded");
    require(full.overflow ==
                1 + kThreadCount * kRecordsPerThread - kPassiveTraceCapacity,
            "overflow explicit and exact");

    require(buffer->StopAndFlush(), "concurrent flush");

    std::ifstream     concurrentInput(concurrentPath);
    const std::string concurrentText(
        (std::istreambuf_iterator<char>(concurrentInput)),
        std::istreambuf_iterator<char>());
    require(concurrentText.find("# overflow=705") != std::string::npos,
            "serialized overflow explicit");
    require(concurrentText.find("# in_flight_at_flush=0") !=
                std::string::npos,
            "serialized drain complete");
    const auto              concurrentRows = readTraceRows(concurrentPath);
    std::set<std::uint64_t> sequences;
    bool                    sawThreadIdentity = false;
    for (const auto& row : concurrentRows)
    {
        sequences.insert(row.sequence);
        sawThreadIdentity = sawThreadIdentity || row.thread != 0;
    }
    require(concurrentRows.size() == kPassiveTraceCapacity,
            "all concurrent records serialized");
    require(sequences.size() == kPassiveTraceCapacity,
            "concurrent sequences unique");
    require(sawThreadIdentity, "thread identity recorded");
    removeTestOutput(concurrentPath);

    std::cout << "native_renderer_passive_trace_test: PASS\n";
    return 0;
}
