#include "developer_checks.hpp"
#include "gpu/completion/completion_ledger.h"
#include "gpu/completion/guest_progress_hooks.h"
#include "gpu/guest_d3d/clear_consume.h"
#include "gpu/guest_d3d/clear_derive.h"
#include "gpu/guest_d3d/guest_render_capture.h"
#include "gpu/guest_d3d/native_frame_assembler.h"
#include "gpu/guest_d3d/resolve_consume.h"
#include "gpu/guest_d3d/resolve_derive.h"
#include "gpu/guest_d3d/ring_stubs.h"
#include "gpu/guest_gpu_service.h"
#include "gpu/guest_memory_watch.h"
#include "guest_import_doubles.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rex/cvar.h>
#include <rex/kernel/init.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

#if REREVVED_DEVELOPER
#include "gpu/diagnostics/route_trace.h"
#include "gpu/diagnostics/scene_resource_capture.h"
#include "gpu/diagnostics/shader_harvest.h"
#endif

DEV_CVAR_DECLARE(bool, native_clear_derive_check);
DEV_CVAR_DECLARE(bool, native_clear_effect_check);
REXCVAR_DECLARE(bool, native_consume_clear);
REXCVAR_DECLARE(bool, native_consume_clear_tiled);
REXCVAR_DECLARE(bool, native_consume_resolve);
REXCVAR_DECLARE(bool, native_consume_resolve_tiled);
#if REREVVED_DEVELOPER
REXCVAR_DECLARE(bool, native_ring_consume_publish);
#endif
DEV_CVAR_DECLARE(bool, native_resolve_derive_check);
DEV_CVAR_DECLARE(bool, native_resolve_effect_check);

using rerevved::gpu::CompletionDebt;
using rerevved::gpu::CompletionDebtMonitor;
using rerevved::gpu::GuestVideoServices;
using rex::X_STATUS;

namespace
{

constexpr uint32_t kMmioBaseAddress = 0x7FC80000;
constexpr uint32_t kCpRbWptr        = 0x01C5;

constexpr uint32_t kRingPointer            = 0x00100000;
constexpr uint32_t kWritebackPointer       = 0x00200000;
constexpr uint32_t kPhysicalHeapBase       = 0xA0000000;
constexpr uint32_t kRingSizeLog2           = 3;
constexpr uint32_t kRingBytes              = 1u << (kRingSizeLog2 + 3);
constexpr uint32_t kWritebackBlockSizeLog2 = 2;
constexpr uint32_t kWritebackSentinel      = 0xA5C3E1F0;

// The ring and pool of the device the resolve fixtures start from: room for every
// word of the body, a fixed pool buffer ending at 0x00204000 and four render targets.
constexpr uint32_t kRingCursor = 0x00200000, kRingLimit38 = 0x00203F00, kRingLimit34 = kRingLimit38 + 160;
constexpr uint32_t kPoolBase = 0x00203000, kPoolSize = 0x1000, kPoolUsed = 0x100, kPoolThreshold = 0x1000;
constexpr uint32_t kPoolBuffer = 0x00203FE8, kSideBuffer = 24, kRingTargets = 4;
// The null pool object's allocator from G+0x3A44 == 0 starts at G+0x34 + 60; its side buffer area ends at G+0x3A3C.
constexpr uint32_t kNullBufferEnd = kRingLimit34 + 60 + kSideBuffer, kNullSideEnd = 0x00205000;

// Every caller waits for a condition that must become true; the minutes-long
// ceiling only stops a hang, so a loaded machine slows the test without failing it.
bool waitFor(const std::function<bool()>& condition, uint32_t timeoutMs = 120000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!condition())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void setupRuntime(rex::Runtime& runtime)
{
    UNSCOPED_INFO("runtime setup must succeed");
    REQUIRE(runtime.Setup(rex::RuntimeConfig{
                .kernel_init = rex::kernel::InitializeKernel,
                .tool_mode   = true,
            }) == X_STATUS_SUCCESS);
}

GuestVideoServices::Options quietOptions()
{
    GuestVideoServices::Options options;
    options.debtThreshold = std::chrono::hours(1);
    return options;
}

uint32_t readMmio(rex::memory::Memory* memory, uint32_t reg)
{
    auto* mmio = memory->TranslateVirtual<volatile uint32_t*>(kMmioBaseAddress + reg * 4);
    return rex::byte_swap(uint32_t(*mmio));
}

void writeMmio(rex::memory::Memory* memory, uint32_t reg, uint32_t value)
{
    auto*             mmio       = memory->TranslateVirtual<volatile uint32_t*>(kMmioBaseAddress + reg * 4);
    volatile uint32_t guestValue = rex::byte_swap(value);
    *mmio                        = guestValue;
}

uint32_t readRawPhysicalWord(rex::memory::Memory* memory, uint32_t address)
{
    return rex::memory::load<uint32_t>(memory->TranslatePhysical(address));
}

void writeRawPhysicalWord(rex::memory::Memory* memory, uint32_t address, uint32_t value)
{
    rex::memory::store<uint32_t>(memory->TranslatePhysical(address), value);
}

std::vector<uint32_t> readRing(rex::memory::Memory* memory)
{
    std::vector<uint32_t> words;
    for (uint32_t offset = 0; offset < kRingBytes; offset += 4)
    {
        words.push_back(readRawPhysicalWord(memory, kRingPointer + offset));
    }
    return words;
}

// Allocates committed guest physical memory, as the guest does for its ring and
// writeback blocks. Returns the virtual base; physical = base - kPhysicalHeapBase.
uint32_t allocatePhysical(rex::memory::Memory* memory)
{
    auto*    heap    = memory->LookupHeap(kPhysicalHeapBase);
    uint32_t address = 0;
    UNSCOPED_INFO("physical allocation must succeed");
    REQUIRE((heap && heap->Alloc(0x10000,
                                 0x10000,
                                 rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                                 rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                                 false,
                                 &address)));
    return address;
}

void fillRing(rex::memory::Memory* memory)
{
    for (uint32_t offset = 0; offset < kRingBytes; offset += 4)
    {
        writeRawPhysicalWord(memory, kRingPointer + offset, 0xC0DE0000u | offset);
    }
}

void testVblankSchedule()
{
    using rerevved::gpu::AdvanceVblank;
    auto step = AdvanceVblank(1099, 1000, 100);
    UNSCOPED_INFO("vblank must wait a full interval");
    REQUIRE((step.due == 0 && step.lastFrameTime == 1000));
    step = AdvanceVblank(1250, 1000, 100);
    UNSCOPED_INFO("vblank must emit each elapsed interval");
    REQUIRE((step.due == 2 && step.lastFrameTime == 1200));
    step = AdvanceVblank(1400, 1000, 100);
    UNSCOPED_INFO("four elapsed intervals must all be emitted");
    REQUIRE((step.due == 4 && step.lastFrameTime == 1400));
    step = AdvanceVblank(2000, 1000, 100);
    UNSCOPED_INFO("catch-up must cap at four and resynchronize");
    REQUIRE((step.due == rerevved::gpu::kMaxVblankCatchUp && step.lastFrameTime == 2000));
    step = AdvanceVblank(2000, 1000, 0);
    UNSCOPED_INFO("zero interval must not emit");
    REQUIRE((step.due == 0 && step.lastFrameTime == 1000));
}

void testVblankJoinTimeout()
{
    using rerevved::gpu::VblankJoinTimeout;
    const auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    UNSCOPED_INFO("an expired close deadline leaves no join time");
    REQUIRE(VblankJoinTimeout(now, now - std::chrono::seconds(1)) == std::chrono::milliseconds(0));
    UNSCOPED_INFO("the join uses the remaining close deadline");
    REQUIRE(VblankJoinTimeout(now, now + std::chrono::milliseconds(300)) == std::chrono::milliseconds(300));
    UNSCOPED_INFO("the join is capped at one second");
    REQUIRE(VblankJoinTimeout(now, now + std::chrono::seconds(4)) == rerevved::gpu::kVblankJoinTimeout);
}

void testDebtMonitor()
{
    using Clock              = CompletionDebtMonitor::Clock;
    const auto            t0 = Clock::time_point{} + std::chrono::hours(1);
    CompletionDebtMonitor monitor(std::chrono::seconds(2));
    UNSCOPED_INFO("no publication is not debt");
    REQUIRE(!monitor.Observe(t0, 0, 8, 0));
    UNSCOPED_INFO("matching writeback is not debt");
    REQUIRE(!monitor.Observe(t0, 1, 8, 8));
    UNSCOPED_INFO("debt starts pending");
    REQUIRE(!monitor.Observe(t0, 1, 8, 0));
    UNSCOPED_INFO("debt waits for threshold");
    REQUIRE(!monitor.Observe(t0 + std::chrono::milliseconds(1999), 2, 12, 0));
    UNSCOPED_INFO("writeback progress restarts the threshold");
    REQUIRE(!monitor.Observe(t0 + std::chrono::milliseconds(2500), 3, 16, 4));
    UNSCOPED_INFO("restarted debt waits for threshold");
    REQUIRE(!monitor.Observe(t0 + std::chrono::milliseconds(4000), 3, 16, 4));
    UNSCOPED_INFO("unchanged writeback past threshold reports");
    REQUIRE(monitor.Observe(t0 + std::chrono::milliseconds(4500), 3, 16, 4));
    UNSCOPED_INFO("report is latched");
    REQUIRE(monitor.Reported());
    UNSCOPED_INFO("debt reports once");
    REQUIRE(!monitor.Observe(t0 + std::chrono::seconds(10), 3, 16, 4));

    monitor.Reset();
    UNSCOPED_INFO("a new writeback generation clears the report latch");
    REQUIRE(!monitor.Reported());
    UNSCOPED_INFO("a new generation starts pending");
    REQUIRE(!monitor.Observe(t0 + std::chrono::seconds(11), 1, 4, 0));
    UNSCOPED_INFO("a new generation reports its own debt");
    REQUIRE(monitor.Observe(t0 + std::chrono::seconds(13), 1, 4, 0));

    CompletionDebtMonitor cleared(std::chrono::seconds(2));
    UNSCOPED_INFO("second monitor starts pending");
    REQUIRE(!cleared.Observe(t0, 1, 8, 0));
    UNSCOPED_INFO("caught-up writeback clears debt");
    REQUIRE(!cleared.Observe(t0 + std::chrono::seconds(1), 1, 8, 8));
    UNSCOPED_INFO("new debt starts pending");
    REQUIRE(!cleared.Observe(t0 + std::chrono::seconds(2), 2, 12, 8));
    UNSCOPED_INFO("cleared debt does not keep its old start");
    REQUIRE(!cleared.Observe(t0 + std::chrono::seconds(3), 2, 12, 8));

    CompletionDebtMonitor unknown(std::chrono::seconds(2));
    UNSCOPED_INFO("known writeback starts pending");
    REQUIRE(!unknown.Observe(t0, 1, 8, 0));
    UNSCOPED_INFO("unreadable writeback is not progress");
    REQUIRE(!unknown.Observe(t0 + std::chrono::seconds(1), 1, 8, std::nullopt));
    UNSCOPED_INFO("unreadable writeback keeps the debt clock");
    REQUIRE(unknown.Observe(t0 + std::chrono::seconds(2), 1, 8, std::nullopt));
}

// Deferred publication preserves publisher index through kick, submit and RPTR.
void testDebtMonitorDeferredKickSequence()
{
    using Clock              = CompletionDebtMonitor::Clock;
    const auto            t0 = Clock::time_point{} + std::chrono::hours(1);
    CompletionDebtMonitor monitor(std::chrono::seconds(2));
    constexpr uint32_t    kIndex = 0x73;
    // The deferral: nothing was published and the writeback word is untouched.
    UNSCOPED_INFO("a deferral publishes nothing and is no debt");
    REQUIRE(!monitor.Observe(t0, 0, 0, 0x40));
    // The main kick publishes the index; the deferred span has not released yet.
    UNSCOPED_INFO("the main kick starts the debt clock");
    REQUIRE(!monitor.Observe(t0 + std::chrono::milliseconds(100), 1, kIndex, 0x40));
    UNSCOPED_INFO("the deferral is inside the bound");
    REQUIRE(!monitor.Observe(t0 + std::chrono::milliseconds(1500), 1, kIndex, 0x40));
    // The worker's submit releases RPTR to the existing index.
    UNSCOPED_INFO("the worker's release clears the debt");
    REQUIRE(!monitor.Observe(t0 + std::chrono::milliseconds(1900), 2, kIndex, kIndex));
    UNSCOPED_INFO("the caught-up writeback stays clear");
    REQUIRE(!monitor.Observe(t0 + std::chrono::seconds(30), 2, kIndex, kIndex));
    UNSCOPED_INFO("no debt report across the sequence");
    REQUIRE(!monitor.Reported());
    // An index that never reaches the writeback is still reported at the bound.
    CompletionDebtMonitor stalled(std::chrono::seconds(2));
    UNSCOPED_INFO("a stalled deferral waits for the bound");
    REQUIRE((!stalled.Observe(t0, 0, 0, 0x40) && !stalled.Observe(t0 + std::chrono::milliseconds(100), 1, kIndex, 0x40) && !stalled.Observe(t0 + std::chrono::seconds(1), 1, kIndex, 0x40)));
    UNSCOPED_INFO("a deferral that never releases is reported at the bound");
    REQUIRE(stalled.Observe(t0 + std::chrono::seconds(3), 1, kIndex, 0x40));
}

void testRegistrationGenerations()
{
    GuestVideoServices service;
    service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
    service.InitializeRingBuffer(kRingPointer + 0x1000, kRingSizeLog2);
    UNSCOPED_INFO("ring re-initialization is accepted as a new generation");
    REQUIRE((!service.ConfigurationFrozen() && service.RingGeneration() == 2));
    service.EnableReadPointerWriteBack(kWritebackPointer, kWritebackBlockSizeLog2);
    service.EnableReadPointerWriteBack(kWritebackPointer + 0x40, kWritebackBlockSizeLog2);
    UNSCOPED_INFO("the newest writeback registration wins");
    REQUIRE((service.WritebackGeneration() == 2 && service.WritebackPointer() == kWritebackPointer + 0x40));

    service.InitializeRingBuffer(0x1FFFFFFCu, kRingSizeLog2);
    UNSCOPED_INFO("an unsafe ring range freezes metadata");
    REQUIRE((service.ConfigurationFrozen() && service.RingGeneration() == 2));
    service.EnableReadPointerWriteBack(kWritebackPointer + 0x80, kWritebackBlockSizeLog2);
    UNSCOPED_INFO("frozen metadata ignores later registrations");
    REQUIRE((service.WritebackGeneration() == 2 && service.WritebackPointer() == kWritebackPointer + 0x40));

    GuestVideoServices unsafeWriteback;
    unsafeWriteback.EnableReadPointerWriteBack(0x1FFFFFFE, kWritebackBlockSizeLog2);
    UNSCOPED_INFO("an unsafe writeback range freezes metadata");
    REQUIRE((unsafeWriteback.ConfigurationFrozen() && unsafeWriteback.WritebackGeneration() == 0));
}

void testSwapEpochs()
{
    GuestVideoServices service;
    constexpr uint32_t kSizeDword = (1280 - 1) | ((720 - 1) << 13);
    for (uint32_t i = 1; i <= 70; ++i)
    {
        rex::system::DetachedVideoSwap swap{};
        swap.reservation          = 0x1000 + i * 0x100;
        swap.frontbuffer_physical = 0x00400000;
        swap.fetch[2]             = kSizeDword;
        swap.texture_format       = 6;
        swap.width                = 1280;
        swap.height               = i == 5 ? 640 : 720;
        service.OnSwap(swap);
    }
    UNSCOPED_INFO("every swap must be counted");
    REQUIRE(service.SwapCount() == 70);
    UNSCOPED_INFO("fetch size mismatch must be counted");
    REQUIRE(service.SwapSizeMismatches() == 1);
    const auto epochs = service.SwapEpochs();
    UNSCOPED_INFO("swap epochs must be bounded");
    REQUIRE(epochs.size() == GuestVideoServices::kMaxSwapEpochs);
    UNSCOPED_INFO("bounded epochs must retain the newest records in order");
    REQUIRE((epochs.front().sequence == 7 && epochs.back().sequence == 70));
    UNSCOPED_INFO("epoch must retain the swap reservation");
    REQUIRE((epochs.back().reservation == 0x1000 + 70 * 0x100 && epochs.back().sizeMatchesFetch));
}

void testMmioAndMetadata()
{
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto* memory = runtime.memory();

    GuestVideoServices service;
    UNSCOPED_INFO("service start must succeed");
    REQUIRE(service.Start(runtime, quietOptions()) == X_STATUS_SUCCESS);
    UNSCOPED_INFO("second start must fail");
    REQUIRE(service.Start(runtime, quietOptions()) == X_STATUS_INVALID_PARAMETER);

    rex::system::X_VIDEO_MODE videoMode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&videoMode);
    const uint32_t width  = std::min(uint32_t(videoMode.display_width), uint32_t(0x0FFF));
    const uint32_t height = std::min(uint32_t(videoMode.display_height), uint32_t(0x0FFF));
    UNSCOPED_INFO("RB_EDRAM_TIMING constant");
    REQUIRE(readMmio(memory, 0x0F00) == 0x08100748);
    UNSCOPED_INFO("RB_BC_CONTROL constant");
    REQUIRE(readMmio(memory, 0x0F01) == 0x0000200E);
    UNSCOPED_INFO("D1MODE_VCOUNTER follows the video mode");
    REQUIRE(readMmio(memory, 0x194C) == height);
    UNSCOPED_INFO("interrupt status constant");
    REQUIRE(readMmio(memory, 0x1951) == 1);
    UNSCOPED_INFO("viewport size follows the video mode");
    REQUIRE(readMmio(memory, 0x1961) == ((width << 16) | height));
    writeMmio(memory, 0x0100, 0x12345678);
    UNSCOPED_INFO("other registers use the shadow array");
    REQUIRE(readMmio(memory, 0x0100) == 0x12345678);

    service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
    service.EnableReadPointerWriteBack(kWritebackPointer, kWritebackBlockSizeLog2);
    fillRing(memory);
    writeRawPhysicalWord(memory, kWritebackPointer, kWritebackSentinel);
    const auto ringBefore = readRing(memory);

    writeMmio(memory, kCpRbWptr, 8);
    writeMmio(memory, kCpRbWptr, 12);
    UNSCOPED_INFO("CP_RB_WPTR writes must be recorded as metadata");
    REQUIRE((service.LastWritePointer() == 12 && service.WritePointerPublications() == 2));
    UNSCOPED_INFO("CP_RB_WPTR remains shadowed");
    REQUIRE(readMmio(memory, kCpRbWptr) == 12);
    // Spans without records release at their kicks, in order: RPTR becomes the last WPTR.
    UNSCOPED_INFO("empty spans release RPTR through the ledger");
    REQUIRE(waitFor([&]
                    {
                        return rex::memory::load_and_swap<uint32_t>(memory->TranslatePhysical(kWritebackPointer)) == 12;
                    }));
    UNSCOPED_INFO("ring bytes must not be consumed or rewritten");
    REQUIRE(readRing(memory) == ringBefore);
    UNSCOPED_INFO("debt must wait for its threshold");
    REQUIRE(!service.CompletionDebtReported());

    rex::system::DetachedVideoSwap swap{};
    swap.fetch[2] = (1280 - 1) | ((720 - 1) << 13);
    swap.width    = 1280;
    swap.height   = 720;
    service.OnSwap(swap);
    UNSCOPED_INFO("swap epoch must record the last WPTR");
    REQUIRE(service.SwapEpochs().back().writePointer == 12);

    UNSCOPED_INFO("vblank counter must advance without a callback");
    REQUIRE(waitFor([&]
                    {
                        return service.VblankCount() > 0;
                    }));
    UNSCOPED_INFO("producers must stop within the join deadline");
    REQUIRE(service.StopProducers(std::chrono::seconds(1)));
    UNSCOPED_INFO("a stopped producer joins without waiting");
    REQUIRE(service.StopProducers(std::chrono::milliseconds(0)));
    UNSCOPED_INFO("stopped producers must report idle");
    REQUIRE(!service.ProducersRunning());
    const uint64_t stoppedCount = service.VblankCount();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    UNSCOPED_INFO("stopped producers must not advance vblank");
    REQUIRE(service.VblankCount() == stoppedCount);
    UNSCOPED_INFO("MMIO stays bound after producers stop");
    REQUIRE(readMmio(memory, 0x0F00) == 0x08100748);

    std::atomic<bool>     reading{ true };
    std::atomic<uint32_t> badReads{ 0 };
    std::thread           reader([&]
                                 {
                           while (reading.load(std::memory_order_acquire))
                           {
                               const uint32_t value = readMmio(memory, 0x0F00);
                               if (value != 0x08100748 && value != 0)
                               {
                                   badReads.fetch_add(1);
                               }
                           }
                                 });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    UNSCOPED_INFO("shutdown reports the completed join");
    REQUIRE(service.Shutdown(std::chrono::milliseconds(0)));
    reading.store(false, std::memory_order_release);
    reader.join();
    UNSCOPED_INFO("MMIO reads racing shutdown must see the service or nothing");
    REQUIRE(badReads.load() == 0);
    UNSCOPED_INFO("MMIO must be detached after shutdown");
    REQUIRE(readMmio(memory, 0x0F00) == 0);
    service.Shutdown();
}

void testCompletionDebt()
{
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto* memory = runtime.memory();

    std::mutex                  reportMutex;
    std::vector<CompletionDebt> reports;
    GuestVideoServices::Options options;
    options.debtThreshold = std::chrono::milliseconds(50);
    options.drawMode      = rerevved::gpu::guest_d3d::NativeDrawMode::Skip;
    options.reportDebt    = [&](const CompletionDebt& debt)
    {
        std::lock_guard lock(reportMutex);
        reports.push_back(debt);
    };
    const auto reportCount = [&]
    {
        std::lock_guard lock(reportMutex);
        return reports.size();
    };

    const uint32_t blockBase        = allocatePhysical(memory);
    const uint32_t ringPointer      = blockBase - kPhysicalHeapBase;
    const uint32_t writebackPointer = ringPointer + 0x1000;
    const uint32_t secondBlockBase  = allocatePhysical(memory);
    const uint32_t secondWriteback  = secondBlockBase - kPhysicalHeapBase + 0x40;
    const auto     writebackWordAt  = [&](uint32_t pointer)
    {
        return rex::memory::load_and_swap<uint32_t>(memory->TranslatePhysical(pointer));
    };

    GuestVideoServices service;
    service.InitializeRingBuffer(ringPointer, kRingSizeLog2);
    service.EnableReadPointerWriteBack(writebackPointer, kWritebackBlockSizeLog2);
    rex::memory::store_and_swap<uint32_t>(memory->TranslatePhysical(writebackPointer), 2);
    UNSCOPED_INFO("debt service start must succeed");
    REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
    // Unmodeled content holds the span, so its kick never reaches RPTR.
    service.Ledger()->Unmodeled(0x826A9CA8);
    writeMmio(memory, kCpRbWptr, 10);

    UNSCOPED_INFO("completion debt must be reported");
    REQUIRE(waitFor([&]
                    {
                        return reportCount() == 1;
                    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    UNSCOPED_INFO("completion debt must be reported once per generation");
    REQUIRE(reportCount() == 1);
    {
        std::lock_guard lock(reportMutex);
        const auto&     debt = reports.front();
        UNSCOPED_INFO("debt must name the ring");
        REQUIRE((debt.ringPointer == ringPointer && debt.ringSizeLog2 == kRingSizeLog2));
        UNSCOPED_INFO("debt must name the retained writeback generation");
        REQUIRE((debt.writebackPointer == writebackPointer && debt.writebackGeneration == 1));
        UNSCOPED_INFO("debt must report WPTR and RPTR");
        REQUIRE((debt.writePointer == 10 && debt.readPointerReadable && debt.readPointer == 2));
        UNSCOPED_INFO("debt must report counts");
        REQUIRE((debt.writePointerPublications == 1 && debt.swapCount == 0));
        UNSCOPED_INFO("debt must name the held span and its unmodeled emitter");
        REQUIRE((debt.ledger.pending && debt.ledger.oldestClosed && debt.ledger.writePointer == 10 &&
                 debt.ledger.unowned == "unmodeled_emitter 0x826a9ca8" && debt.ledger.unmodeledEmitter == 0x826A9CA8 &&
                 !debt.ledger.failure));
    }
    UNSCOPED_INFO("a held span must not write the writeback word");
    REQUIRE(writebackWordAt(writebackPointer) == 2);

    // The guest frees its blocks and registers a new writeback before more work.
    auto* heap = memory->LookupHeap(kPhysicalHeapBase);
    UNSCOPED_INFO("second block release must succeed");
    REQUIRE(heap->Release(secondBlockBase));
    service.EnableReadPointerWriteBack(secondWriteback, kWritebackBlockSizeLog2);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    UNSCOPED_INFO("a new generation without publications is not debt");
    REQUIRE(reportCount() == 1);
    writeMmio(memory, kCpRbWptr, 4);
    UNSCOPED_INFO("an unreadable writeback in a new generation must still report debt");
    REQUIRE(waitFor([&]
                    {
                        return reportCount() == 2;
                    }));
    {
        std::lock_guard lock(reportMutex);
        const auto&     debt = reports.back();
        UNSCOPED_INFO("the second report names the new generation");
        REQUIRE((debt.writebackGeneration == 2 && debt.writebackPointer == secondWriteback));
        UNSCOPED_INFO("a released writeback page is reported unreadable");
        REQUIRE((!debt.readPointerReadable && debt.writePointerPublications == 1));
    }
    UNSCOPED_INFO("debt service must stop");
    REQUIRE(service.Shutdown());
}

#if REREVVED_DEVELOPER
class NoKickServices final : public rerevved::gpu::guest_d3d::IRingStubServices
{
public:
    void Kick(uint32_t) override
    {
    }
};

// native_ring_consume_publish watchdog reports a stuck wait without new calls.
void testRingConsumeWatchdogRunsInTheServiceLoop()
{
    namespace d3d = rerevved::gpu::guest_d3d;
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto*                       memory = runtime.memory();
    GuestVideoServices::Options options;
    options.debtThreshold      = std::chrono::milliseconds(50);
    options.drawMode           = d3d::NativeDrawMode::Skip;
    const uint32_t blockBase   = allocatePhysical(memory);
    const uint32_t ringPointer = blockBase - kPhysicalHeapBase;

    GuestVideoServices service;
    service.InitializeRingBuffer(ringPointer, kRingSizeLog2);
    service.EnableReadPointerWriteBack(ringPointer + 0x1000, kWritebackBlockSizeLog2);
    UNSCOPED_INFO("watchdog service start must succeed");
    REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
    REXCVAR_SET(native_ring_consume_publish, true);
    d3d::PublishRingStubServices(std::make_shared<NoKickServices>());
    (void)d3d::TakeRingConsumeCounts();
    {
        // Only the open scope exists: nothing calls Publish, Submit or the monitor.
        const d3d::RingWaitScope stalled(d3d::RingWaitKind::Fence, 0x1000, 0x8269C61C, 0x13);
        std::uint64_t            reported = 0;
        UNSCOPED_INFO("a stalled fence wait must be reported by the vblank loop");
        REQUIRE(waitFor([&]
                        {
                            reported += d3d::TakeRingConsumeCounts().watchdogFence;
                            return reported != 0;
                        },
                        10000));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        reported += d3d::TakeRingConsumeCounts().watchdogFence;
        UNSCOPED_INFO("a stalled wait is reported once, not on every loop pass");
        REQUIRE(reported == 1);
    }
    d3d::WithdrawRingStubServices();
    REXCVAR_SET(native_ring_consume_publish, false);
    UNSCOPED_INFO("watchdog service must stop");
    REQUIRE(service.Shutdown());
}

#endif

// Registration permits an early kick; missing writeback registration is debt.
void testRegistrationDebt()
{
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto* memory = runtime.memory();

    std::mutex                  reportMutex;
    std::vector<CompletionDebt> reports;
    GuestVideoServices::Options options;
    options.debtThreshold = std::chrono::milliseconds(50);
    options.reportDebt    = [&](const CompletionDebt& debt)
    {
        std::lock_guard lock(reportMutex);
        reports.push_back(debt);
    };
    const auto reportCount = [&]
    {
        std::lock_guard lock(reportMutex);
        return reports.size();
    };

    GuestVideoServices service;
    UNSCOPED_INFO("registration service start must succeed");
    REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
    service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
    writeMmio(memory, kCpRbWptr, 0x8);
    UNSCOPED_INFO("a span held for a missing writeback registration is reported");
    REQUIRE(waitFor([&]
                    {
                        return reportCount() == 1;
                    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    UNSCOPED_INFO("the registration hold is reported once");
    REQUIRE(reportCount() == 1);
    {
        std::lock_guard lock(reportMutex);
        const auto&     debt = reports.front();
        UNSCOPED_INFO("the report names the held span and the missing registration without failing");
        REQUIRE((debt.writebackGeneration == 0 && debt.writePointer == 0x8 && debt.ledger.pending && debt.ledger.oldestClosed &&
                 debt.ledger.writePointer == 0x8 && debt.ledger.oldestRecord == "awaiting writeback registration" &&
                 debt.ledger.unowned.empty() && !debt.ledger.failure));
    }

    const uint32_t rptr = allocatePhysical(memory) - kPhysicalHeapBase + 0x3C;
    service.EnableReadPointerWriteBack(rptr, kWritebackBlockSizeLog2);
    UNSCOPED_INFO("the late registration releases the held span");
    REQUIRE(waitFor([&]
                    {
                        return rex::memory::load_and_swap<uint32_t>(memory->TranslatePhysical(rptr)) == 0x8;
                    }));
    UNSCOPED_INFO("registration service must stop");
    REQUIRE(service.Shutdown());
}

struct VblankObservation
{
    GuestVideoServices*   service = nullptr;
    std::atomic<uint32_t> calls{ 0 };
    std::atomic<uint64_t> counterAtCallback{ std::numeric_limits<uint64_t>::max() };
    std::atomic<uint64_t> userData{ 0 };
    std::atomic<uint64_t> source{ std::numeric_limits<uint64_t>::max() };
};

std::atomic<VblankObservation*> g_vblankObservation{ nullptr };

void VblankCallback(PPCContext& context, uint8_t*)
{
    auto* observation = g_vblankObservation.load(std::memory_order_acquire);
    if (!observation)
    {
        return;
    }
    uint64_t expected = std::numeric_limits<uint64_t>::max();
    observation->counterAtCallback.compare_exchange_strong(expected, observation->service->VblankCount());
    observation->source.store(context.r3.u64, std::memory_order_release);
    observation->userData.store(context.r4.u64, std::memory_order_release);
    observation->calls.fetch_add(1, std::memory_order_release);
}

void testInterruptRetention()
{
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    GuestVideoServices service;
    VblankObservation  observation{ &service };
    g_vblankObservation.store(&observation, std::memory_order_release);

    constexpr uint32_t kCodeBase        = 0x82000000;
    constexpr uint32_t kImageSize       = 0x00010000;
    constexpr uint32_t kCallbackAddress = kCodeBase + 0x100;
    UNSCOPED_INFO("vblank callback function table must initialize");
    REQUIRE(runtime.function_dispatcher()->InitializeFunctionTable(kCodeBase, kImageSize, kCodeBase, kImageSize, true));
    UNSCOPED_INFO("vblank callback function must register");
    REQUIRE(runtime.function_dispatcher()->SetFunction(kCallbackAddress, &VblankCallback));

    service.SetInterruptCallback(kCallbackAddress, 0xCAFE);
    UNSCOPED_INFO("callback registered before start is retained");
    REQUIRE(service.InterruptCallback() == kCallbackAddress);
    UNSCOPED_INFO("callback service start must succeed");
    REQUIRE(service.Start(runtime, quietOptions()) == X_STATUS_SUCCESS);
    UNSCOPED_INFO("vblank callback must execute");
    REQUIRE(waitFor([&]
                    {
                        return observation.calls.load(std::memory_order_acquire) != 0;
                    }));
    UNSCOPED_INFO("first callback must observe vblank one");
    REQUIRE(observation.counterAtCallback.load() == 1);
    UNSCOPED_INFO("callback must receive its user data");
    REQUIRE(observation.userData.load() == 0xCAFE);
    UNSCOPED_INFO("vblank must raise interrupt source 0");
    REQUIRE(observation.source.load() == 0);

    service.SetInterruptCallback(0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint32_t calls = observation.calls.load(std::memory_order_acquire);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    UNSCOPED_INFO("cleared callback must not dispatch");
    REQUIRE(observation.calls.load(std::memory_order_acquire) == calls);
    service.Shutdown();
    service.SetInterruptCallback(kCallbackAddress, 1);
    UNSCOPED_INFO("shutdown must ignore later callbacks");
    REQUIRE(service.InterruptCallback() == 0);
    g_vblankObservation.store(nullptr, std::memory_order_release);
}

void storeGuest(rex::memory::Memory* memory, uint32_t address, uint32_t value)
{
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(address), value);
}

uint32_t loadPhysical(rex::memory::Memory* memory, uint32_t address)
{
    return rex::memory::load_and_swap<uint32_t>(memory->TranslatePhysical(address));
}

// 0x8269BCA8 entry fields let the kick release B+4, B+0 and RPTR.
void testFenceHookRelease()
{
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto* memory = runtime.memory();

    std::mutex                                            failureMutex;
    std::vector<rerevved::gpu::completion::LedgerFailure> failures;
    auto                                                  options = quietOptions();
    options.reportFailure                                         = [&](const rerevved::gpu::completion::LedgerFailure& failure)
    {
        std::lock_guard lock(failureMutex);
        failures.push_back(failure);
    };

    const uint32_t device        = allocatePhysical(memory);
    const uint32_t block         = allocatePhysical(memory);
    const uint32_t blockPhysical = block - kPhysicalHeapBase;
    storeGuest(memory, device + 0x2A90, block);
    storeGuest(memory, device + 0x2A9C, 0x25);
    storeGuest(memory, device + 0x30, 0x00C0FFE0);
    storeGuest(memory, device + 0x3A48, 0x7);

    GuestVideoServices service;
    UNSCOPED_INFO("fence service start must succeed");
    REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
    service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
    service.EnableReadPointerWriteBack(blockPhysical + 0x3C, kWritebackBlockSizeLog2);

    PPCContext ctx{};
    ctx.r3.u64 = device;
    ctx.r4.u64 = 0x25;
    // The fence wait names B before any fence record exists.
    rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269BE70, ctx);
    const auto waiting = service.Ledger()->Snapshot();
    UNSCOPED_INFO("the fence wait records the requested fence and phys(*(G+0x2A90))");
    REQUIRE((waiting.fenceRequested && waiting.requestedFence == 0x25 && waiting.fenceBlock == blockPhysical));
    rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269BCA8, ctx);
    UNSCOPED_INFO("a recorded fence is not written before its kick");
    REQUIRE(loadPhysical(memory, blockPhysical) == 0);
    writeMmio(memory, kCpRbWptr, 0x16);
    UNSCOPED_INFO("the kick releases the span through RPTR");
    REQUIRE(waitFor([&]
                    {
                        return loadPhysical(memory, blockPhysical + 0x3C) == 0x16;
                    }));
    UNSCOPED_INFO("B+0 holds the fence and B+4 the cursor the emitter encoded");
    REQUIRE((loadPhysical(memory, blockPhysical) == 0x25 && loadPhysical(memory, blockPhysical + 4) == (0x00C0FFE0 | 0x3)));

    // The guest's CPU fence mode makes the emitter complete fences itself.
    memory->TranslateVirtual<uint8_t*>(device + 0x2ABD)[0] = 0x2;
    rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269BCA8, ctx);
    {
        std::lock_guard lock(failureMutex);
        UNSCOPED_INFO("CPU fence mode fails the session");
        REQUIRE((failures.size() == 1 && failures[0].detail.starts_with("cpu_fence_mode")));
    }
    UNSCOPED_INFO("fence service must stop");
    REQUIRE(service.Shutdown());
}

// Stands in for the native frame assembler behind the clear and resolve
// hooks. It reports every resolve's source pass partial, which a host-only
// frontbuffer resolve must ignore.
struct FakeRenderSink final : rerevved::gpu::guest_d3d::IGuestRenderSink
{
    std::atomic<uint32_t> resolves{ 0 }, frontbuffers{ 0 }, unmodeled{ 0 }, unmodeledRead{ 0 };

    // What each submitted resolve and each reported write carried, in call order.
    struct Submitted
    {
        rerevved::gpu::guest_d3d::GuestTarget      target;
        rerevved::gpu::guest_d3d::GuestResolveCopy copy;
        bool                                       frontbuffer = false;
    };

    std::vector<Submitted>                                                 submitted;
    std::vector<std::optional<rerevved::gpu::guest_d3d::GuestResolveCopy>> reported;

    // Whether a resolve it takes is reported partial, which the ledger fails unless it is a scanout.
    bool partialResolves = true;

    // Off, a clear is an error; on, it is recorded in call order.
    bool                     acceptClears = false;
    std::vector<std::string> clears;

    uint64_t Clear(const rerevved::gpu::guest_d3d::GuestClear& clear, std::string& error) override;

    void ClearUnreadable(const std::optional<rerevved::gpu::guest_d3d::GuestTarget>&) override
    {
    }

    void ResolveUnmodeled(const std::optional<rerevved::gpu::guest_d3d::GuestResolveCopy>& copy) override
    {
        reported.push_back(copy);
        ++unmodeled;
        unmodeledRead += copy ? 1 : 0;
    }

    uint64_t Resolve(const rerevved::gpu::guest_d3d::GuestTarget&      target,
                     const rerevved::gpu::guest_d3d::GuestResolveCopy& copy,
                     bool                                              frontbuffer,
                     bool&                                             partial,
                     std::string&) override
    {
        submitted.push_back({ target, copy, frontbuffer });
        frontbuffers += frontbuffer ? 1 : 0;
        partial         = partialResolves;
        const auto mark = ++resolves;
        {
            std::lock_guard lock(destinationMutex);
            destinations[mark] = copy.destBase;
        }
        return mark;
    }

    // The destination of the resolve admitted at `mark`, read by the completion worker.
    std::optional<uint32_t> Destination(uint64_t mark)
    {
        std::lock_guard lock(destinationMutex);
        const auto      found = destinations.find(mark);
        return found == destinations.end() ? std::nullopt : std::optional<uint32_t>(found->second);
    }

private:
    std::mutex                   destinationMutex;
    std::map<uint64_t, uint32_t> destinations;
};

// Host completion that finishes every flush at once. It has no guest images,
// unless `images` names the sink whose admitted resolves it encodes, as the
// native completion source does: a small image at the resolve's destination.
struct FakeCompletion final : rerevved::gpu::completion::IDrawCompletion
{
    std::mutex            mutex;
    std::vector<uint64_t> flushed;
    std::atomic<uint32_t> takes{ 0 }, written{ 0 };
    FakeRenderSink*       images = nullptr;

    rerevved::gpu::completion::DrawFlush FlushThrough(uint64_t sequence) override
    {
        std::lock_guard lock(mutex);
        flushed.push_back(sequence);
        return { 1, false, {} };
    }

    uint64_t Completed() const override
    {
        return 1;
    }

    rerevved::gpu::completion::GuestImageTake TakeGuestImage(uint64_t mark) override
    {
        ++takes;
        const auto destination = images ? images->Destination(mark) : std::nullopt;
        if (!destination)
            return { std::nullopt, "fixture_no_image", false };
        return { rerevved::gpu::guest_d3d::GuestResolveImage{ *destination, { 0xFF, 0xFF, 0xFF, 0xFF }, {} }, {}, false };
    }

    void GuestImageWritten(uint64_t) override
    {
        ++written;
    }

    std::vector<uint64_t> Flushed()
    {
        std::lock_guard lock(mutex);
        return flushed;
    }
};

// Resolve fixture reads the bound 1280x720 surface, destination fetch, and
// G+0x2A18..G+0x2A24 copy words.
struct ResolveFixture
{
    rex::memory::Memory* memory      = nullptr;
    uint32_t             device      = 0;
    uint32_t             descriptor  = 0;
    uint32_t             scanout     = 0; // virtual
    uint32_t             frontbuffer = 0; // physical
    uint32_t             rptr        = 0;
    uint32_t             poolObject  = 0;
    uint32_t             pcr         = 0; // the guest KPCR r13 names, holding the thread's stack bounds
    PPCContext           ctx{};

    // The thread's stack for a call entered with r1 = `sp`: [limit, sp + 0x400), the limit 0x400 below r1 unless given.
    void Stack(uint32_t sp, std::optional<uint32_t> limit = std::nullopt)
    {
        ctx.r1.u64 = sp;
        storeGuest(memory, pcr + 0x70, sp + 0x400);
        storeGuest(memory, pcr + 0x74, limit.value_or(sp - 0x400));
    }

    explicit ResolveFixture(rex::memory::Memory* guest)
    : memory(guest)
    {
        // Before 0x8269E8A0: source G+0x3098, destination fetch r6+0x1C, copy
        // words G+0x2A18..0x2A24.
        auto* view = memory->LookupHeap(0xE0000000);
        UNSCOPED_INFO("frontbuffer allocation in the 0xE0000000 view must succeed");
        REQUIRE((view && view->Alloc(0x2000,
                                     0x1000,
                                     rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                                     rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                                     false,
                                     &scanout)));
        frontbuffer = (scanout & 0x1FFFFFFF) + 0x1000;
        UNSCOPED_INFO("the view's physical form differs from the masked virtual base");
        REQUIRE((scanout & 0x1FFFFFFF) != frontbuffer);
        device                 = allocatePhysical(memory);
        descriptor             = allocatePhysical(memory);
        rptr                   = allocatePhysical(memory) - kPhysicalHeapBase + 0x3C;
        const uint32_t surface = allocatePhysical(memory);
        storeGuest(memory, surface + 0x24, (uint32_t(1280 - 1) << 18) | (uint32_t(720 - 1) << 3));
        storeGuest(memory, surface + 0x18, 1U << 16); // 2X, the presented form
        storeGuest(memory, surface + 0x1C, 0);        // k_8_8_8_8 at EDRAM 0
        storeGuest(memory, device + 0x3098, surface);
        storeGuest(memory, device + 0x2A18, 0x00100000);
        storeGuest(memory, device + 0x2A1C, frontbuffer);
        storeGuest(memory, device + 0x2A20, (720u << 16) | 1280);
        storeGuest(memory, device + 0x2A24, 0x01000300);
        // The ring cursor and limits, the pool and the render target count.
        poolObject = allocatePhysical(memory);
        storeGuest(memory, device + 0x30, kRingCursor);
        storeGuest(memory, device + 0x34, kRingLimit34);
        storeGuest(memory, device + 0x38, kRingLimit38);
        storeGuest(memory, device + 0x3A44, 0);
        storeGuest(memory, device + 0x3A54, kPoolUsed);
        storeGuest(memory, device + 0x3A58, kPoolThreshold);
        storeGuest(memory, device + 0x31CC, kRingTargets);
        storeGuest(memory, device + 0x34CC, poolObject);
        storeGuest(memory, poolObject + 152, kPoolBase);
        storeGuest(memory, poolObject + 156, kPoolSize);
        Target(scanout);
        storeGuest(memory, scanout, kWritebackSentinel);
        // PPCRegister leaves its value uninitialized, so every argument is set:
        // flags 0, no rect, no point, mip 0 and slice 0.
        for (auto* reg : { &ctx.r4, &ctx.r5, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 })
            reg->u64 = 0;
        ctx.r3.u64 = device;
        ctx.r6.u64 = descriptor;
        ctx.lr     = 0x8250AFEC + 4;
        // The KPCR in a page of its own, after the others, and a default stack.
        pcr         = allocatePhysical(memory) + 0x100;
        ctx.r13.u64 = pcr;
        Stack(0x70000000);
    }

    void Target(uint32_t base)
    {
        storeGuest(memory, descriptor + 0x1C, 2);
        storeGuest(memory, descriptor + 0x20, base | 6);
        storeGuest(memory, descriptor + 0x24, (1280 - 1) | ((720 - 1) << 13));
    }

    // The entry observer, then the copy-word hook, as the generated Resolve runs them.
    void Resolve(bool copyHook = true)
    {
        rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269E520, ctx);
        if (copyHook)
            rerevved::gpu::completion::detail::RecordGuestResolveCopy(device);
    }

    rex::system::DetachedVideoSwap Swap() const
    {
        rex::system::DetachedVideoSwap swap{};
        swap.frontbuffer_physical = frontbuffer;
        swap.fetch[1]             = frontbuffer | 6;
        swap.fetch[2]             = (1280 - 1) | ((720 - 1) << 13);
        swap.width                = 1280;
        swap.height               = 720;
        return swap;
    }
};

struct FailureLog
{
    std::mutex                                            mutex;
    std::vector<rerevved::gpu::completion::LedgerFailure> failures;

    void Attach(GuestVideoServices::Options& options)
    {
        options.reportFailure = [this](const rerevved::gpu::completion::LedgerFailure& failure)
        {
            std::lock_guard lock(mutex);
            failures.push_back(failure);
        };
    }

    std::vector<std::string> Details()
    {
        std::lock_guard          lock(mutex);
        std::vector<std::string> details;
        for (const auto& failure : failures)
            details.push_back(failure.detail);
        return details;
    }
};

// Host-only resolve: VdSwap owns output; physical identity releases.
void testFrontbufferResolveOwnership(rerevved::gpu::guest_d3d::NativeDrawMode mode)
{
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    ResolveFixture guest(runtime.memory());
    auto*          memory    = runtime.memory();
    auto           assembler = std::make_shared<FakeRenderSink>();
    FakeCompletion completion;
    FailureLog     log;
    rerevved::gpu::completion::PublishGuestRenderAssembler(assembler);

    auto options     = quietOptions();
    options.drawMode = mode;
    options.draws    = &completion;
    log.Attach(options);
    GuestVideoServices service;
    UNSCOPED_INFO("resolve service start must succeed");
    REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
    service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
    service.EnableReadPointerWriteBack(guest.rptr, kWritebackBlockSizeLog2);

    guest.Resolve();
    UNSCOPED_INFO("the copy hook submits a 1280x720 resolve as a frontbuffer candidate");
    REQUIRE(assembler->frontbuffers.load() == 1);
    writeMmio(memory, kCpRbWptr, 0x8);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    UNSCOPED_INFO("the frontbuffer resolve waits for its VdSwap");
    REQUIRE(loadPhysical(memory, guest.rptr) == 0);

    service.OnSwap(guest.Swap());
    UNSCOPED_INFO("the resolve's physical destination matches the swap and its span releases");
    REQUIRE(waitFor([&]
                    {
                        return loadPhysical(memory, guest.rptr) == 0x8;
                    }));
    UNSCOPED_INFO("the swap's native output owns the resolve through its op mark; no guest bytes are taken or written");
    REQUIRE((completion.Flushed() == std::vector<uint64_t>{ 1 } && completion.takes.load() == 0 &&
             rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(guest.scanout)) == kWritebackSentinel));
    UNSCOPED_INFO("a host-only frontbuffer resolve labels neither the swap nor its span partial");
    REQUIRE((!service.SwapEpochs().back().partial && service.Ledger()->Snapshot().partialSpans == 0 && log.Details().empty()));

    // A candidate resolved to another surface is not this swap's output.
    guest.Target(guest.scanout + 0x1000);
    guest.Resolve();
    writeMmio(memory, kCpRbWptr, 0x10);
    service.OnSwap(guest.Swap());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto snapshot = service.Ledger()->Snapshot();
    UNSCOPED_INFO("a resolve to another surface never releases: " + snapshot.unowned);
    REQUIRE((loadPhysical(memory, guest.rptr) == 0x8 && snapshot.unowned.starts_with("frontbuffer_resolve_mismatch")));
    const auto details = log.Details();
    if (mode == rerevved::gpu::guest_d3d::NativeDrawMode::Strict)
    {
        UNSCOPED_INFO("strict fails at the mismatched frontbuffer resolve");
        REQUIRE((details.size() == 1 && details[0].starts_with("frontbuffer_resolve_mismatch")));
    }
    else
    {
        UNSCOPED_INFO("skip names the mismatch as the blocking record");
        REQUIRE(details.empty());
    }
    UNSCOPED_INFO("resolve service must stop");
    REQUIRE(service.Shutdown());
    rerevved::gpu::completion::PublishGuestRenderAssembler({});
}

// Missing copy hooks or assemblers fail at a defined point instead of hanging.
void testResolveCaptureGuards()
{
    for (const bool published : { true, false })
    {
        rex::Runtime runtime(std::filesystem::path{});
        setupRuntime(runtime);
        ResolveFixture guest(runtime.memory());
        auto*          memory    = runtime.memory();
        auto           assembler = std::make_shared<FakeRenderSink>();
        FailureLog     log;
        if (published)
            rerevved::gpu::completion::PublishGuestRenderAssembler(assembler);

        auto options = quietOptions();
        log.Attach(options);
        GuestVideoServices service;
        UNSCOPED_INFO("guard service start must succeed");
        REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
        service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
        service.EnableReadPointerWriteBack(guest.rptr, kWritebackBlockSizeLog2);

        guest.Resolve(false);
        if (published)
        {
            UNSCOPED_INFO("an open resolve waits only for its copy hook");
            REQUIRE((log.Details().empty() && service.Ledger()->Snapshot().oldestRecord.ends_with("awaiting its copy words")));
            // The next record on the resolving thread proves the hook was missed.
            writeMmio(memory, kCpRbWptr, 0x8);
        }
        const std::string expected = published ? "resolve_copy_not_captured" : "resolve_no_native_assembler";
        const auto        details  = log.Details();
        UNSCOPED_INFO("strict fails visibly with " + expected);
        REQUIRE((details.size() == 1 && details[0] == expected));
        // A copy hook that arrives later settles nothing, but its guest write
        // still reaches the assembler with the copy words it read.
        rerevved::gpu::completion::detail::RecordGuestResolveCopy(guest.device);
        UNSCOPED_INFO("no resolve is submitted after the failure");
        REQUIRE((assembler->resolves.load() == 0 && log.Details().size() == 1));
        UNSCOPED_INFO(std::format("the unsettled copy reports its write: {} of {} read", assembler->unmodeled.load(), assembler->unmodeledRead.load()));
        REQUIRE((assembler->unmodeled.load() == (published ? 1U : 0U) && assembler->unmodeledRead.load() == assembler->unmodeled.load()));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        UNSCOPED_INFO("the failed resolve's span never releases");
        REQUIRE(loadPhysical(memory, guest.rptr) == 0);
        UNSCOPED_INFO("guard service must stop");
        REQUIRE(service.Shutdown());
        rerevved::gpu::completion::PublishGuestRenderAssembler({});
    }
}

// A refused entry still reports the copy's destination or an unreadable write.
void testEntryRefusedResolveReported()
{
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    ResolveFixture guest(runtime.memory());
    auto*          memory    = runtime.memory();
    auto           assembler = std::make_shared<FakeRenderSink>();
    FailureLog     log;
    rerevved::gpu::completion::PublishGuestRenderAssembler(assembler);
    auto options = quietOptions();
    log.Attach(options);
    GuestVideoServices service;
    UNSCOPED_INFO("entry refusal service start must succeed");
    REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
    service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
    service.EnableReadPointerWriteBack(guest.rptr, kWritebackBlockSizeLog2);

    storeGuest(memory, guest.device + 0x3098, 0); // no color target 0
    guest.Resolve();
    const auto details = log.Details();
    UNSCOPED_INFO("the entry refuses the undecoded source");
    REQUIRE((details.size() == 1 && details[0].starts_with("resolve_state_unreadable")));
    UNSCOPED_INFO(std::format("the refused resolve reports its write with its copy words: {} of {} read", assembler->unmodeled.load(), assembler->unmodeledRead.load()));
    REQUIRE((assembler->resolves.load() == 0 && assembler->unmodeled.load() == 1 && assembler->unmodeledRead.load() == 1));
    rerevved::gpu::completion::detail::RecordGuestResolveCopy(0);
    UNSCOPED_INFO("unreadable copy words report a write anywhere");
    REQUIRE((assembler->unmodeled.load() == 2 && assembler->unmodeledRead.load() == 1));
    UNSCOPED_INFO("entry refusal service must stop");
    REQUIRE(service.Shutdown());
    rerevved::gpu::completion::PublishGuestRenderAssembler({});
}

// Set slots, depth and 1X never scan out; tiled flags are restricted.
void testResolveHookSourceRules()
{
    enum class Source
    {
        Set,
        Depth,
        Tiled,
        OneSample
    };

    const auto run = [](Source source, uint32_t flags, uint32_t frontbuffers, uint32_t resolves, const std::string& failure)
    {
        rex::Runtime runtime(std::filesystem::path{});
        setupRuntime(runtime);
        ResolveFixture guest(runtime.memory());
        auto*          memory    = runtime.memory();
        auto           assembler = std::make_shared<FakeRenderSink>();
        FailureLog     log;
        rerevved::gpu::completion::PublishGuestRenderAssembler(assembler);
        auto options = quietOptions();
        log.Attach(options);
        GuestVideoServices service;
        UNSCOPED_INFO("source rule service start must succeed");
        REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
        service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
        service.EnableReadPointerWriteBack(guest.rptr, kWritebackBlockSizeLog2);

        const auto rt0   = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(guest.device + 0x3098));
        const auto other = allocatePhysical(memory);
        storeGuest(memory, other + 0x24, (uint32_t(1280 - 1) << 18) | (uint32_t(720 - 1) << 3));
        storeGuest(memory, other + 0x18, 1280 | (1U << 16)); // 2X like RT0
        if (source == Source::OneSample)
            storeGuest(memory, rt0 + 0x18, 0);
        else if (source == Source::Set)
        {
            storeGuest(memory, other + 0x1C, 0x200); // k_8_8_8_8 RT1 at EDRAM 0x200
            storeGuest(memory, guest.device + 0x309C, other);
        }
        else if (source == Source::Depth)
        {
            storeGuest(memory, other + 0x1C, 0x500); // D24S8 at EDRAM 0x500
            storeGuest(memory, guest.device + 0x30A8, other);
        }
        else
        {
            // RT0 saved by BeginTiling for three tiles, read by tile 0's
            // predicated resolve into a 448x720 texture.
            storeGuest(memory, guest.device + 0x2ABC, 0x20U << 24);
            storeGuest(memory, guest.device + 0x31CC, 3);
            storeGuest(memory, guest.device + 0x337C, 1280);
            storeGuest(memory, guest.device + 0x3380, 720);
            storeGuest(memory, guest.device + 0x31B8, rt0);
            storeGuest(memory, guest.device + 0x31A4, 3);
            storeGuest(memory, guest.device + 0x31AC, 1);
            storeGuest(memory, guest.descriptor + 0x24, (448 - 1) | ((720 - 1) << 13));
        }
        guest.ctx.r4.u64 = flags;
        guest.Resolve();
        const auto details = log.Details();
        // A refused resolve still reports its write, with the copy words it read.
        const auto refused = failure.empty() ? 0U : 1U;
        UNSCOPED_INFO(std::format("flags {:#x}: unmodeled resolve writes {} of {} read", flags, assembler->unmodeled.load(), assembler->unmodeledRead.load()));
        REQUIRE((assembler->unmodeled.load() == refused && assembler->unmodeledRead.load() == refused));
        UNSCOPED_INFO(std::format("flags {:#x}: frontbuffers {} resolves {} failure {}", flags, assembler->frontbuffers.load(), assembler->resolves.load(), details.empty() ? "none" : details[0]));
        REQUIRE((assembler->frontbuffers.load() == frontbuffers && assembler->resolves.load() == resolves &&
                 (failure.empty() || (details.size() == 1 && details[0] == failure))));
        UNSCOPED_INFO("source rule service must stop");
        REQUIRE(service.Shutdown());
        rerevved::gpu::completion::PublishGuestRenderAssembler({});
    };
    run(Source::Set, 0, 0, 1, {});
    run(Source::Depth, 4, 0, 0, "resolve_flags_unsupported");
    run(Source::Tiled, 0x1000, 0, 0, "resolve_flags_unsupported 0x1000");
    run(Source::Tiled, 0x340, 0, 1, {});
    // A 1X source into a 1280x720 texture, such as the world's K14 pass.
    run(Source::OneSample, 0, 0, 1, {});
}

struct CallbackObservation
{
    rex::memory::Memory*  memory  = nullptr;
    uint32_t              scratch = 0; // physical S2
    std::atomic<bool>     clears{ true };
    std::atomic<uint32_t> sourceOne{ 0 };
    std::atomic<uint64_t> userData{ 0 };
    // S2+0x14 as each source-1 handler saw it, in raise order.
    std::array<std::atomic<uint32_t>, 4> parameters{};
    // Holds the vblank thread inside a source-0 interrupt until released.
    std::atomic<bool> holdVblank{ false }, vblankHeld{ false }, releaseVblank{ false };
};

std::atomic<CallbackObservation*> g_callbackObservation{ nullptr };

// Stands in for 0x8269B450: source 1 clears the CPU 2 bit in S2+0.
void CallbackHandler(PPCContext& context, uint8_t*)
{
    auto* observation = g_callbackObservation.load(std::memory_order_acquire);
    if (!observation)
    {
        return;
    }
    if (context.r3.u64 == 0)
    {
        if (observation->holdVblank.exchange(false, std::memory_order_acq_rel))
        {
            observation->vblankHeld.store(true, std::memory_order_release);
            while (!observation->releaseVblank.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return;
    }
    observation->userData.store(context.r4.u64, std::memory_order_release);
    if (observation->clears.load(std::memory_order_acquire))
    {
        auto*          mask  = observation->memory->TranslatePhysical(observation->scratch);
        const uint32_t value = rex::memory::load_and_swap<uint32_t>(mask);
        rex::memory::store_and_swap<uint32_t>(mask, value & ~uint32_t(1u << 2));
    }
    const uint32_t parameter = rex::memory::load_and_swap<uint32_t>(observation->memory->TranslatePhysical(observation->scratch + 0x14));
    const uint32_t raised    = observation->sourceOne.load(std::memory_order_acquire);
    if (raised < observation->parameters.size())
    {
        observation->parameters[raised].store(parameter, std::memory_order_release);
    }
    observation->sourceOne.fetch_add(1, std::memory_order_acq_rel);
}

void testCallbackInterruptAndTeardown()
{
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto* memory = runtime.memory();

    constexpr uint32_t kCodeBase        = 0x82000000;
    constexpr uint32_t kImageSize       = 0x00010000;
    constexpr uint32_t kCallbackAddress = kCodeBase + 0x200;
    UNSCOPED_INFO("callback function table must initialize");
    REQUIRE(runtime.function_dispatcher()->InitializeFunctionTable(kCodeBase, kImageSize, kCodeBase, kImageSize, true));
    UNSCOPED_INFO("callback handler must register");
    REQUIRE(runtime.function_dispatcher()->SetFunction(kCallbackAddress, &CallbackHandler));

    const uint32_t      block   = allocatePhysical(memory);
    const uint32_t      scratch = allocatePhysical(memory) - kPhysicalHeapBase;
    const uint32_t      rptr    = block - kPhysicalHeapBase + 0x3C;
    CallbackObservation observation;
    observation.memory  = memory;
    observation.scratch = scratch;
    g_callbackObservation.store(&observation, std::memory_order_release);

    GuestVideoServices service;
    service.SetInterruptCallback(kCallbackAddress, 0x50235580);
    UNSCOPED_INFO("callback service start must succeed");
    REQUIRE(service.Start(runtime, quietOptions()) == X_STATUS_SUCCESS);
    service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
    service.EnableReadPointerWriteBack(rptr, kWritebackBlockSizeLog2);
    auto ledger = service.Ledger();
    ledger->Callback(scratch, 0x826A3FB8, 0x1234, 0, std::nullopt);
    writeMmio(memory, kCpRbWptr, 0x8);
    UNSCOPED_INFO("the callback span releases after its interrupt");
    REQUIRE(waitFor([&]
                    {
                        return loadPhysical(memory, rptr) == 0x8;
                    }));
    UNSCOPED_INFO("the vblank thread raised source 1 once with the registered user data");
    REQUIRE((observation.sourceOne.load() == 1 && observation.userData.load() == 0x50235580 && service.CallbackInterrupts() == 1));
    UNSCOPED_INFO("the handler saw the slot, cleared its bit, and the slot was retired");
    REQUIRE((loadPhysical(memory, scratch + 0x10) == 0x0BADF00D && loadPhysical(memory, scratch + 0x14) == 0x1234 &&
             loadPhysical(memory, scratch) == 0));

    // A handler that never clears S2+0 holds the span; teardown still joins in bound.
    observation.clears.store(false, std::memory_order_release);
    ledger->Callback(scratch, 0x826A3FB8, 0x5678, 0, std::nullopt);
    writeMmio(memory, kCpRbWptr, 0x10);
    UNSCOPED_INFO("the second callback is raised");
    REQUIRE(waitFor([&]
                    {
                        return observation.sourceOne.load() == 2;
                    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    UNSCOPED_INFO("the held span writes no RPTR");
    REQUIRE((loadPhysical(memory, rptr) == 0x8 && loadPhysical(memory, scratch + 0x10) == 0x826A3FB8));
    const auto start   = std::chrono::steady_clock::now();
    const bool stopped = service.StopProducers(rerevved::gpu::kVblankJoinTimeout);
    UNSCOPED_INFO("the completion and vblank threads join within the bound");
    REQUIRE((stopped && std::chrono::steady_clock::now() - start < rerevved::gpu::kVblankJoinTimeout && !service.ProducersRunning()));
    UNSCOPED_INFO("callback service must shut down");
    REQUIRE(service.Shutdown(std::chrono::milliseconds(0)));
    g_callbackObservation.store(nullptr, std::memory_order_release);
}

// Stopped producers drop source-1 work; callbacks share 0x826AC020.
void testPredicatedCallbackPair()
{
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto* memory = runtime.memory();

    constexpr uint32_t kCodeBase        = 0x82000000;
    constexpr uint32_t kImageSize       = 0x00010000;
    constexpr uint32_t kCallbackAddress = kCodeBase + 0x200;
    UNSCOPED_INFO("predicated callback function table must initialize");
    REQUIRE(runtime.function_dispatcher()->InitializeFunctionTable(kCodeBase, kImageSize, kCodeBase, kImageSize, true));
    UNSCOPED_INFO("predicated callback handler must register");
    REQUIRE(runtime.function_dispatcher()->SetFunction(kCallbackAddress, &CallbackHandler));

    const uint32_t      block   = allocatePhysical(memory);
    const uint32_t      scratch = allocatePhysical(memory) - kPhysicalHeapBase;
    const uint32_t      rptr    = block - kPhysicalHeapBase + 0x3C;
    CallbackObservation observation;
    observation.memory  = memory;
    observation.scratch = scratch;
    g_callbackObservation.store(&observation, std::memory_order_release);

    GuestVideoServices service;
    service.SetInterruptCallback(kCallbackAddress, 0x50235580);
    UNSCOPED_INFO("predicated callback service start must succeed");
    REQUIRE(service.Start(runtime, quietOptions()) == X_STATUS_SUCCESS);
    service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
    service.EnableReadPointerWriteBack(rptr, kWritebackBlockSizeLog2);
    auto ledger = service.Ledger();
    ledger->Callback(scratch, 0x826AC020, 0x1234, 0x04000006, rerevved::gpu::completion::kFirstPassPredicate);
    // Both wait for the 0x826AC32C callback buffer's submission at 0x8269BE5C.
    ledger->Callback(scratch, 0x826AC020, 0x5678, 0x04000007, std::nullopt, 0x00150000);
    rerevved::gpu::completion::detail::RecordDirectSubmit(0x00150000, 0x20);
    writeMmio(memory, kCpRbWptr, 0x8);
    UNSCOPED_INFO("the predicated callback span releases after both interrupts");
    REQUIRE(waitFor([&]
                    {
                        return loadPhysical(memory, rptr) == 0x8;
                    }));
    UNSCOPED_INFO("the predicated callback and the unpredicated one raise source 1 once each, in record order");
    REQUIRE((observation.sourceOne.load() == 2 && service.CallbackInterrupts() == 2 && ledger->Snapshot().callbacksReleased == 2 &&
             observation.parameters[0].load() == 0x1234 && observation.parameters[1].load() == 0x5678));
    UNSCOPED_INFO("the last slot was retired with its bit cleared");
    REQUIRE((loadPhysical(memory, scratch + 0x10) == 0x0BADF00D && loadPhysical(memory, scratch) == 0));
    UNSCOPED_INFO("predicated callback producers must stop");
    REQUIRE(service.StopProducers(rerevved::gpu::kVblankJoinTimeout));
    UNSCOPED_INFO("predicated callback service must shut down");
    REQUIRE(service.Shutdown(std::chrono::milliseconds(0)));
    g_callbackObservation.store(nullptr, std::memory_order_release);
}

// No source-1 handler leaves fence/RPTR pending; registration restores order.
void testNoHandlerHoldsCallbackSpan()
{
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto* memory = runtime.memory();

    constexpr uint32_t kCodeBase        = 0x82000000;
    constexpr uint32_t kImageSize       = 0x00010000;
    constexpr uint32_t kCallbackAddress = kCodeBase + 0x400;
    UNSCOPED_INFO("no-handler function table must initialize");
    REQUIRE(runtime.function_dispatcher()->InitializeFunctionTable(kCodeBase, kImageSize, kCodeBase, kImageSize, true));
    UNSCOPED_INFO("no-handler callback handler must register");
    REQUIRE(runtime.function_dispatcher()->SetFunction(kCallbackAddress, &CallbackHandler));

    const uint32_t      block   = allocatePhysical(memory) - kPhysicalHeapBase;
    const uint32_t      scratch = allocatePhysical(memory) - kPhysicalHeapBase;
    const uint32_t      rptr    = block + 0x3C;
    CallbackObservation observation;
    observation.memory  = memory;
    observation.scratch = scratch;
    g_callbackObservation.store(&observation, std::memory_order_release);

    // The completion worker polls after every Pump, so this count observes
    // whole Pump passes, each of which retries the pending callback.
    std::atomic<uint64_t> pumpPasses{ 0 };
    auto                  options = quietOptions();
    options.pollDrawFailure       = [&]() -> std::optional<GuestVideoServices::DrawFailure>
    {
        pumpPasses.fetch_add(1, std::memory_order_acq_rel);
        return std::nullopt;
    };
    GuestVideoServices service; // no SetInterruptCallback yet
    UNSCOPED_INFO("no-handler service start must succeed");
    REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
    service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
    service.EnableReadPointerWriteBack(rptr, kWritebackBlockSizeLog2);
    auto ledger = service.Ledger();
    ledger->Fence(block, 3, 0x40);
    writeMmio(memory, kCpRbWptr, 0x8);
    UNSCOPED_INFO("the span before the callback releases");
    REQUIRE(waitFor([&]
                    {
                        return loadPhysical(memory, rptr) == 0x8;
                    }));
    UNSCOPED_INFO("its fence and cursor landed");
    REQUIRE((loadPhysical(memory, block) == 3 && loadPhysical(memory, block + 4) == 0x40));

    // Flags 2 raise source 1 without waiting for the handler to clear S2+0, so
    // only the interrupt result can hold the span.
    ledger->Callback(scratch, 0x826A3FB8, 0x1234, 2, std::nullopt);
    ledger->Fence(block, 5, 0x80);
    writeMmio(memory, kCpRbWptr, 0x10);
    UNSCOPED_INFO("the callback slot stores land");
    REQUIRE(waitFor([&]
                    {
                        return loadPhysical(memory, scratch) == 4;
                    }));
    // The pass in flight when the stores landed may have polled already; two
    // more polls prove one whole pass began after the stores and retried the
    // callback against the missing handler. A timeout fails the test.
    const uint64_t passesAtStores = pumpPasses.load(std::memory_order_acquire);
    UNSCOPED_INFO("a whole completion pass retried the callback with no handler registered");
    REQUIRE(waitFor([&]
                    {
                        return pumpPasses.load(std::memory_order_acquire) >= passesAtStores + 2;
                    }));
    UNSCOPED_INFO("the slot holds the callback, its parameter and the CPU 2 mask");
    REQUIRE((loadPhysical(memory, scratch + 0x10) == 0x826A3FB8 && loadPhysical(memory, scratch + 0x14) == 0x1234 && loadPhysical(memory, scratch) == 4));
    UNSCOPED_INFO("no fence or RPTR is written behind a callback that no handler received");
    REQUIRE((loadPhysical(memory, rptr) == 0x8 && loadPhysical(memory, block) == 3 && loadPhysical(memory, block + 4) == 0x40));
    UNSCOPED_INFO("no source-1 interrupt was raised and the callback is still owed");
    REQUIRE((service.CallbackInterrupts() == 0 && observation.sourceOne.load() == 0 && ledger->Snapshot().callbacksReleased == 0 && ledger->Snapshot().pending));

    service.SetInterruptCallback(kCallbackAddress, 0x50235580);
    UNSCOPED_INFO("registering the handler releases the span");
    REQUIRE(waitFor([&]
                    {
                        return loadPhysical(memory, rptr) == 0x10;
                    }));
    UNSCOPED_INFO("the fence follows one source-1 interrupt carrying the slot's parameter");
    REQUIRE((loadPhysical(memory, block) == 5 && loadPhysical(memory, block + 4) == 0x80 && loadPhysical(memory, scratch + 0x10) == 0x826A3FB8 &&
             service.CallbackInterrupts() == 1 && observation.sourceOne.load() == 1 && observation.parameters[0].load() == 0x1234));
    UNSCOPED_INFO("no-handler producers must stop");
    REQUIRE(service.StopProducers(rerevved::gpu::kVblankJoinTimeout));
    UNSCOPED_INFO("no-handler service must shut down");
    REQUIRE(service.Shutdown(std::chrono::milliseconds(0)));
    g_callbackObservation.store(nullptr, std::memory_order_release);
}

void testQueuedInterruptDroppedAtClose()
{
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto* memory = runtime.memory();

    constexpr uint32_t kCodeBase        = 0x82000000;
    constexpr uint32_t kImageSize       = 0x00010000;
    constexpr uint32_t kCallbackAddress = kCodeBase + 0x300;
    UNSCOPED_INFO("close test function table must initialize");
    REQUIRE(runtime.function_dispatcher()->InitializeFunctionTable(kCodeBase, kImageSize, kCodeBase, kImageSize, true));
    UNSCOPED_INFO("close test handler must register");
    REQUIRE(runtime.function_dispatcher()->SetFunction(kCallbackAddress, &CallbackHandler));

    const uint32_t      block   = allocatePhysical(memory);
    const uint32_t      scratch = allocatePhysical(memory) - kPhysicalHeapBase;
    const uint32_t      rptr    = block - kPhysicalHeapBase + 0x3C;
    CallbackObservation observation;
    observation.memory  = memory;
    observation.scratch = scratch;
    g_callbackObservation.store(&observation, std::memory_order_release);

    GuestVideoServices service;
    service.SetInterruptCallback(kCallbackAddress, 0x50235580);
    UNSCOPED_INFO("close test service start must succeed");
    REQUIRE(service.Start(runtime, quietOptions()) == X_STATUS_SUCCESS);
    service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
    service.EnableReadPointerWriteBack(rptr, kWritebackBlockSizeLog2);

    // A held interrupt keeps the global critical region, which a host thread
    // takes once at start, so the completion thread must run before the hold.
    writeMmio(memory, kCpRbWptr, 0x4);
    UNSCOPED_INFO("the completion thread released an empty span before the hold");
    REQUIRE(waitFor([&]
                    {
                        return loadPhysical(memory, rptr) == 0x4;
                    }));

    observation.holdVblank.store(true, std::memory_order_release);
    UNSCOPED_INFO("the vblank thread is held in a source-0 handler");
    REQUIRE(waitFor([&]
                    {
                        return observation.vblankHeld.load(std::memory_order_acquire);
                    }));
    service.Ledger()->Callback(scratch, 0x826A3FB8, 0x1234, 0, std::nullopt);
    writeMmio(memory, kCpRbWptr, 0x8);
    UNSCOPED_INFO("the completion thread queued source 1 behind the held vblank handler");
    REQUIRE(waitFor([&]
                    {
                        return service.CallbackInterruptPending();
                    }));

    bool        stopped = false;
    std::thread stopper([&]
                        {
                            stopped = service.StopProducers(std::chrono::milliseconds(2000));
                        });
    // StopProducers drops queued source-1 requests, then closes and withdraws
    // the ledger; vblank is still held.
    UNSCOPED_INFO("the close drops the queued source-1 request before vblank is released");
    REQUIRE(waitFor([]
                    {
                        return !rerevved::gpu::completion::guestProgressActive.load(std::memory_order_acquire);
                    }));
    observation.releaseVblank.store(true, std::memory_order_release);
    stopper.join();
    UNSCOPED_INFO("producers stop once the vblank handler returns");
    REQUIRE((stopped && !service.ProducersRunning()));
    UNSCOPED_INFO("a source-1 request queued before the close is never raised");
    REQUIRE((observation.sourceOne.load() == 0 && service.CallbackInterrupts() == 0));
    UNSCOPED_INFO("the closed span wrote neither the retired slot nor RPTR");
    REQUIRE((loadPhysical(memory, rptr) == 0x4 && loadPhysical(memory, scratch + 0x10) == 0x826A3FB8));
    UNSCOPED_INFO("close test service must shut down");
    REQUIRE(service.Shutdown(std::chrono::milliseconds(0)));
    g_callbackObservation.store(nullptr, std::memory_order_release);
}

// Timestamp words write at kick; EndTiling waits for CP_RB_WPTR.
void testTileReplayHooks()
{
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto* memory = runtime.memory();

    std::mutex                                            failureMutex;
    std::vector<rerevved::gpu::completion::LedgerFailure> failures;
    auto                                                  options = quietOptions();
    options.reportFailure                                         = [&](const rerevved::gpu::completion::LedgerFailure& failure)
    {
        std::lock_guard lock(failureMutex);
        failures.push_back(failure);
    };

    const uint32_t device        = allocatePhysical(memory);
    const uint32_t block         = allocatePhysical(memory);
    const uint32_t side          = allocatePhysical(memory);
    const uint32_t blockPhysical = block - kPhysicalHeapBase;
    const uint32_t sidePhysical  = side - kPhysicalHeapBase;
    storeGuest(memory, device + 0x2A90, block);
    storeGuest(memory, device + 0x2A9C, 0x31);
    storeGuest(memory, device + 0x30, 0x00C0FFE0);

    GuestVideoServices service;
    UNSCOPED_INFO("replay service start must succeed");
    REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
    service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
    service.EnableReadPointerWriteBack(blockPhysical + 0x3C, kWritebackBlockSizeLog2);

    // Timestamp writes to B+0x40..0x5F publish at the kick.
    rerevved::gpu::completion::detail::RecordGpuTimestamp(block + 0x44, device);
    rerevved::gpu::completion::detail::RecordGpuTimestamp(block + 0x40, device);
    writeMmio(memory, kCpRbWptr, 0x8);
    UNSCOPED_INFO("the timestamp span releases at its kick");
    REQUIRE(waitFor([&]
                    {
                        return loadPhysical(memory, blockPhysical + 0x3C) == 0x8;
                    }));
    UNSCOPED_INFO("the timestamps are nonzero and increase in record order");
    REQUIRE((loadPhysical(memory, blockPhysical + 0x44) == 1 && loadPhysical(memory, blockPhysical + 0x40) == 2));

    PPCContext ctx{};
    ctx.r3.u64 = device;
    // Fence 0x826AC33C waits for worker CP_RB_WPTR; G+0x33B0 set fails.
    ctx.r4.u64 = side - 4;
    ctx.lr     = 0x826AC340;
    rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269BCA8, ctx);
    // The 0x826AC32C callback buffer is kicked directly, then the fence buffer deferred.
    rerevved::gpu::completion::detail::RecordDirectSubmit(0x00150000, 0x81000020);
    writeMmio(memory, kCpRbWptr, 0x10);
    rerevved::gpu::completion::detail::RecordDeferredKick(sidePhysical, 0x8);
    UNSCOPED_INFO("the callback buffer's span releases");
    REQUIRE(waitFor([&]
                    {
                        return loadPhysical(memory, blockPhysical + 0x3C) == 0x10;
                    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    UNSCOPED_INFO("the EndTiling fence waits for the worker");
    REQUIRE((loadPhysical(memory, blockPhysical) == 0 && service.Ledger()->Snapshot().oldestRecord == "awaiting worker submit " + std::format("{:#010x}", sidePhysical)));

    std::thread worker([&]
                       {
                           rerevved::gpu::completion::detail::MarkReplayWorker();
                           rerevved::gpu::completion::detail::RecordReplaySubmit(sidePhysical, 0x81000008, 0x826AB890);
                           writeMmio(memory, kCpRbWptr, 0x18);
                       });
    worker.join();
    UNSCOPED_INFO("the worker's submit releases the deferred span");
    REQUIRE(waitFor([&]
                    {
                        return loadPhysical(memory, blockPhysical + 0x3C) == 0x18;
                    }));
    UNSCOPED_INFO("the EndTiling fence is written");
    REQUIRE((loadPhysical(memory, blockPhysical) == 0x31 && loadPhysical(memory, blockPhysical + 4) == 0x00C0FFE0));

    // With the worker already done, 0x8269BD98 submits the next fence buffer directly.
    const uint32_t nextSide = allocatePhysical(memory);
    storeGuest(memory, device + 0x2A9C, 0x32);
    ctx.r4.u64 = nextSide - 4;
    rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269BCA8, ctx);
    rerevved::gpu::completion::detail::RecordDirectSubmit(nextSide - kPhysicalHeapBase, 0x8);
    writeMmio(memory, kCpRbWptr, 0x20);
    UNSCOPED_INFO("a directly submitted EndTiling fence buffer releases its fence");
    REQUIRE((waitFor([&]
                     {
                         return loadPhysical(memory, blockPhysical + 0x3C) == 0x20;
                     }) &&
             loadPhysical(memory, blockPhysical) == 0x32));

    storeGuest(memory, device + 0x33B0, 1);
    ctx.lr = 0;
    rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269BCA8, ctx);
    {
        std::lock_guard lock(failureMutex);
        UNSCOPED_INFO("a fence while tiling fails the session");
        REQUIRE((failures.size() == 1 && failures[0].detail == "tiled_progress_fence at 0x8269bca8"));
    }
    UNSCOPED_INFO("replay service must stop");
    REQUIRE(service.Shutdown());
}

// Mirrored reads recompare after guest-view, host, ledger or recommit writes.
void testWatchedMirrorWriters()
{
    using rerevved::gpu::guest_d3d::GuestBytes;
    using rerevved::gpu::guest_d3d::NativeFrameAssembler;
    constexpr uint32_t kSize       = NativeFrameAssembler::kMirroredReadBytes;
    constexpr uint32_t kAllocation = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
    constexpr uint32_t kWritable   = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
    rex::Runtime       runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto*    memory = runtime.memory();
    auto*    heap   = memory->LookupHeap(kPhysicalHeapBase);
    uint32_t guest  = 0;
    UNSCOPED_INFO("the mirrored range is allocated");
    REQUIRE((heap && heap->Alloc(kSize, 0x10000, kAllocation, kWritable, false, &guest)));
    const uint32_t physical = memory->GetPhysicalAddress(guest);
    auto*          bytes    = memory->TranslatePhysical<uint8_t*>(physical);

    rerevved::gpu::guest_d3d::NativeFrameAssemblerConfig config;
    config.timing      = true;
    config.watchMirror = [memory](uint32_t address, uint32_t length)
    {
        return rerevved::gpu::WatchGuestPhysicalWrites(*memory, address, length);
    };
    NativeFrameAssembler assembler(std::move(config));
    void*                notice = memory->RegisterPhysicalMemoryInvalidationCallback(&NativeFrameAssembler::WriteNotice, &assembler);
    std::string          error;
    const auto           read = [&]
    {
        GuestBytes out;
        UNSCOPED_INFO("a watched read: " + error);
        REQUIRE(assembler.ReadResolved(physical, kSize, [&]
                                       {
                                           return std::span<const uint8_t>(bytes, kSize);
                                       },
                                       out,
                                       error));
        return out;
    };
    const auto watchedHits = [&]
    {
        return assembler.ReadMirror().watchedHits;
    };
    const auto ends = [&](std::string_view writer, uint32_t offset, uint8_t value, const std::function<void()>& write)
    {
        const auto before = watchedHits();
        for (int n = 0; n < 256 && watchedHits() == before; ++n)
            read();
        UNSCOPED_INFO(std::format("the range is watched before {}", writer));
        REQUIRE((watchedHits() > before && memory->IsPhysicalRangeWriteWatched(physical, kSize)));
        write();
        const auto hits = watchedHits();
        const auto seen = read();
        UNSCOPED_INFO(std::format("{} ends the watch", writer));
        REQUIRE((seen[offset] == value && watchedHits() == hits));
    };
    const auto store = [&](uint32_t address, uint8_t value)
    {
        *memory->TranslateVirtual<volatile uint8_t*>(address) = value;
    };

    ends("a guest store", 0x100, 1, [&]
         {
             store(guest + 0x100, 1);
         });
    ends("a store through the 0xC0000000 view", 0x20000, 2, [&]
         {
             store(0xC0000000 + physical + 0x20000, 2);
         });
    ends("a store through the 0xE0000000 view", 0x30000, 3, [&]
         {
             store(0xE0000000 + physical - 0x1000 + 0x30000, 3);
         });
    // As the XMA decoder writes its output.
    ends("a reported host write", 0x40000, 4, [&]
         {
             bytes[0x40000] = 4;
             memory->NotifyPhysicalWrite(physical + 0x40000, 1);
         });
    ends("a ledger word", 0x50000, 5, [&]
         {
             assembler.HostWriting(physical + 0x50000, 4);
             bytes[0x50000] = 5;
             assembler.HostWrote(physical + 0x50000, 4);
         });
    // The unreported byte stands for what the recommitted page may now hold.
    ends("a recommit", 0x60000, 6, [&]
         {
             bytes[0x60000] = 6;
             UNSCOPED_INFO("the page is recommitted");
             REQUIRE(heap->AllocFixed(guest + 0x60000, 0x10000, 0x10000, kAllocation, kWritable));
         });

    memory->UnregisterPhysicalMemoryInvalidationCallback(notice);
    UNSCOPED_INFO("the mirrored range is released");
    REQUIRE(heap->Release(guest, nullptr));
}

} // namespace

// Close precedes swap recording; one failed frame stops both draw modes once.
void testDisplayWaitHooks()
{
    // Hook bodies record 0x826A4458 scaler-update waits.
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto* memory = runtime.memory();

    std::mutex                                            failureMutex;
    std::vector<rerevved::gpu::completion::LedgerFailure> failures;
    auto                                                  options = quietOptions();
    options.reportFailure                                         = [&](const rerevved::gpu::completion::LedgerFailure& failure)
    {
        std::lock_guard lock(failureMutex);
        failures.push_back(failure);
    };

    const uint32_t block         = allocatePhysical(memory);
    const uint32_t blockPhysical = block - kPhysicalHeapBase;

    GuestVideoServices service;
    UNSCOPED_INFO("display wait service start must succeed");
    REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
    service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
    service.EnableReadPointerWriteBack(blockPhysical + 0x3C, kWritebackBlockSizeLog2);

    rerevved::gpu::completion::detail::RecordDisplayWait(0x826A44E0);
    rerevved::gpu::completion::detail::RecordDisplayWait(0x826A45C8);
    writeMmio(memory, kCpRbWptr, 0x8);
    UNSCOPED_INFO("the display wait span releases at its kick");
    REQUIRE(waitFor([&]
                    {
                        return loadPhysical(memory, blockPhysical + 0x3C) == 0x8;
                    }));
    const auto snapshot = service.Ledger()->Snapshot();
    {
        std::lock_guard lock(failureMutex);
        UNSCOPED_INFO("display waits do not fail the session");
        REQUIRE((failures.empty() && !snapshot.failure && snapshot.unmodeledEmitter == 0));
    }
    UNSCOPED_INFO("display wait service must stop");
    REQUIRE(service.Shutdown());
}

void testAssembledSwapFailure()
{
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);

    std::mutex                                            failureMutex;
    std::vector<rerevved::gpu::completion::LedgerFailure> failures;
    std::atomic<uint32_t>                                 assembled{ 0 };
    std::atomic<bool>                                     fail{ false };
    auto                                                  options = quietOptions();
    options.drawMode                                              = rerevved::gpu::guest_d3d::NativeDrawMode::Skip;
    options.reportFailure                                         = [&](const rerevved::gpu::completion::LedgerFailure& failure)
    {
        std::lock_guard lock(failureMutex);
        failures.push_back(failure);
    };
    options.assembleSwap = [&](const rex::system::DetachedVideoSwap& swap) -> std::optional<std::string>
    {
        ++assembled;
        if (!fail.load())
            return std::nullopt;
        return "epoch " + std::to_string(swap.width) + ": fixture_frame_failed";
    };
    GuestVideoServices service;
    UNSCOPED_INFO("swap service start must succeed");
    REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
    rex::system::DetachedVideoSwap swap{};
    swap.frontbuffer_physical = 0x1F000000;
    swap.width                = 1280;
    swap.height               = 720;
    service.OnSwap(swap);
    {
        std::lock_guard lock(failureMutex);
        UNSCOPED_INFO("an assembled frame does not fail the ledger");
        REQUIRE((assembled == 1 && failures.empty() && service.Ledger()->Snapshot().swaps == 1));
    }
    fail = true;
    service.OnSwap(swap);
    {
        std::lock_guard lock(failureMutex);
        UNSCOPED_INFO("a failed frame reaches reportFailure in skip mode, naming the swap");
        REQUIRE((assembled == 2 && failures.size() == 1 && failures[0].kind == rerevved::gpu::completion::ProgressKind::Swap &&
                 failures[0].address == 0x1F000000 && failures[0].detail.find("fixture_frame_failed") != std::string::npos));
    }
    service.OnSwap(swap);
    {
        std::lock_guard lock(failureMutex);
        UNSCOPED_INFO("the failure is reported once");
        REQUIRE(failures.size() == 1);
    }
    UNSCOPED_INFO("swap service producers stop");
    REQUIRE(service.StopProducers(rerevved::gpu::kVblankJoinTimeout));
    service.OnSwap(swap);
    UNSCOPED_INFO("no frame is assembled once the producers stopped");
    REQUIRE(assembled == 3);
    UNSCOPED_INFO("swap service must stop");
    REQUIRE(service.Shutdown());
}

// Census counts skipped draws; fatal source failures fail either mode once.
void testDeviceFailureWithoutUi(rerevved::gpu::guest_d3d::NativeDrawMode mode)
{
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    const auto            ownerThread = std::this_thread::get_id();
    const bool            census      = mode == rerevved::gpu::guest_d3d::NativeDrawMode::Census;
    std::atomic<bool>     lost{ false }, skippedPolled{ false };
    std::atomic<unsigned> reports{ 0 }, failedPolls{ 0 };
    auto                  options = quietOptions();
    options.drawMode              = mode;
    options.pollDrawFailure       = [&]() -> std::optional<GuestVideoServices::DrawFailure>
    {
        UNSCOPED_INFO("source failure is polled on the completion worker");
        REQUIRE(std::this_thread::get_id() != ownerThread);
        if (!lost.load())
            return std::nullopt;
        if (census && !skippedPolled.exchange(true))
            return GuestVideoServices::DrawFailure{ "native_frame_failed: fixture_refused", false };
        ++failedPolls;
        return GuestVideoServices::DrawFailure{ "native_device_lost", true };
    };
    options.reportFailure = [&](const rerevved::gpu::completion::LedgerFailure& failure)
    {
        UNSCOPED_INFO("device loss fails the ledger on its worker without UI dispatch");
        REQUIRE((std::this_thread::get_id() != ownerThread && failure.detail == "native_device_lost"));
        ++reports;
    };
    GuestVideoServices service;
    UNSCOPED_INFO("device failure service starts");
    REQUIRE(service.Start(runtime, std::move(options)) == X_STATUS_SUCCESS);
    lost = true;
    UNSCOPED_INFO("device failure is reported without UI pumping or guest work");
    REQUIRE(waitFor([&]
                    {
                        return reports.load() != 0;
                    }));
    UNSCOPED_INFO("device failure is recorded in the ledger");
    REQUIRE(service.Ledger()->Snapshot().failure.has_value());
    // Leave the worker many poll cycles in which it must not poll again.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    UNSCOPED_INFO("device failure worker stops");
    REQUIRE(service.StopProducers(rerevved::gpu::kVblankJoinTimeout));
    UNSCOPED_INFO("the source failure is forwarded exactly once");
    REQUIRE((reports == 1 && failedPolls == 1));
    UNSCOPED_INFO("device failure service shuts down");
    REQUIRE(service.Shutdown());
}

// Clear hooks pair device-derived entry, 0x826A1D90 record and exit; other
// devices are unpaired.
struct ClearProbeFixture
{
    rex::memory::Memory* memory = nullptr;
    uint32_t             device = 0, color = 0, sp = 0, frame = 0;
    uint32_t             pcr = 0; // the guest KPCR r13 names, holding the thread's stack bounds
    PPCContext           ctx{};

    // The thread's stack for a call entered with r1 = `at`: [limit, at + 0x400), the limit 0x400 below r1 unless given.
    void Stack(uint32_t at, std::optional<uint32_t> limit = std::nullopt)
    {
        ctx.r1.u64 = at;
        storeGuest(memory, pcr + 0x70, at + 0x400);
        storeGuest(memory, pcr + 0x74, limit.value_or(at - 0x400));
    }

    explicit ClearProbeFixture(rex::memory::Memory* guest)
    : memory(guest)
    {
        device                 = allocatePhysical(memory);
        const uint32_t surface = device + 0x8000;
        color                  = device + 0x9000;
        sp                     = device + 0xB000;
        frame                  = sp - 368;
        // RT0 bound, no depth surface, no tiling or scissor, a 1280x720 viewport.
        storeGuest(memory, surface + 0x24, (uint32_t(1280 - 1) << 18) | (uint32_t(720 - 1) << 3));
        storeGuest(memory, device + 0x3098, surface);
        storeGuest(memory, device + 0x3170, std::bit_cast<uint32_t>(1280.0f));
        storeGuest(memory, device + 0x3174, std::bit_cast<uint32_t>(720.0f));
        // A pool object (zero fixed buffer words) so the watch read needs no null pool allocator words.
        storeGuest(memory, device + 0x34CC, device + 0xC000);
        for (uint32_t i = 0; i < 4; ++i)
            storeGuest(memory, color + 4 * i, 0x3F000000u + i);
        storeGuest(memory, frame + 92, 0x55);
        // Color slot 0, null rectangle, the color words at `color`, stencil 0x55.
        for (auto* reg : { &ctx.r5, &ctx.r7, &ctx.r9, &ctx.r10 })
            reg->u64 = 0;
        ctx.r1.u64 = sp;
        ctx.r3.u64 = device;
        ctx.r4.u64 = 0x1;
        ctx.r6.u64 = color;
        ctx.r8.u64 = 0x55;
        ctx.f1.f64 = 1.0;
        // The KPCR in a page of its own, and the stack of the call.
        pcr         = allocatePhysical(memory) + 0x100;
        ctx.r13.u64 = pcr;
        Stack(sp);
    }

    void Enter()
    {
        rerevved::gpu::completion::ObserveGuestProgressEntry(0x826A2970, ctx);
    }

    // The hook on 0x826A1D90: r3 device, r4 flags, r5 index, r6..r9 the
    // rectangle, r10 the color pointer.
    void Call(uint32_t onDevice, int32_t right)
    {
        const std::array<uint32_t, 8> arguments{ onDevice, 0x1, 0, 0, 0, uint32_t(right), 720, color };
        rerevved::gpu::completion::detail::RecordGuestClear(arguments, frame, 1.0);
    }

    void Leave()
    {
        rerevved::gpu::completion::detail::RecordGuestClearReturn(frame);
    }
};

// The Clear and Resolve consumes default on. A probe test sets all four off so
// its entry observers run alone, and restores what it found.
struct ConsumeCvars
{
    bool clear, clearTiled, resolve, resolveTiled;

    ConsumeCvars()
    : clear(REXCVAR_GET(native_consume_clear))
    , clearTiled(REXCVAR_GET(native_consume_clear_tiled))
    , resolve(REXCVAR_GET(native_consume_resolve))
    , resolveTiled(REXCVAR_GET(native_consume_resolve_tiled))
    {
        REXCVAR_SET(native_consume_clear, false);
        REXCVAR_SET(native_consume_clear_tiled, false);
        REXCVAR_SET(native_consume_resolve, false);
        REXCVAR_SET(native_consume_resolve_tiled, false);
    }

    ~ConsumeCvars()
    {
        REXCVAR_SET(native_consume_clear, clear);
        REXCVAR_SET(native_consume_clear_tiled, clearTiled);
        REXCVAR_SET(native_consume_resolve, resolve);
        REXCVAR_SET(native_consume_resolve_tiled, resolveTiled);
    }

    ConsumeCvars(const ConsumeCvars&)            = delete;
    ConsumeCvars& operator=(const ConsumeCvars&) = delete;
};

#if REREVVED_DEVELOPER
void testClearDeriveHooks()
{
    const ConsumeCvars consumeCvars;
    using rerevved::gpu::guest_d3d::TakeClearDeriveCounts;
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    ClearProbeFixture fixture(runtime.memory());

    // Disabled, the entry stacks no probe and the guest's call is not compared.
    REXCVAR_SET(native_clear_derive_check, false);
    TakeClearDeriveCounts();
    fixture.Enter();
    fixture.Call(fixture.device, 1280);
    fixture.Leave();
    auto counts = TakeClearDeriveCounts();
    UNSCOPED_INFO("a disabled probe records nothing");
    REQUIRE((counts.ok == 0 && counts.unsupported == 0 && counts.unpaired == 0 && counts.mismatch == decltype(counts.mismatch){}));

    REXCVAR_SET(native_clear_derive_check, true);
    // The guest makes the derived call.
    fixture.Enter();
    fixture.Call(fixture.device, 1280);
    fixture.Leave();
    counts = TakeClearDeriveCounts();
    UNSCOPED_INFO("a matching clear counts ok at the exit hook");
    REQUIRE((counts.ok == 1 && counts.unsupported == 0 && counts.unpaired == 0 && counts.mismatch == decltype(counts.mismatch){}));

    // A different right edge is the rectangle mismatch and nothing else.
    fixture.Enter();
    fixture.Call(fixture.device, 1279);
    fixture.Leave();
    counts = TakeClearDeriveCounts();
    decltype(counts.mismatch) rectOnly{};
    rectOnly[3] = 1;
    UNSCOPED_INFO("a different rectangle counts one rect mismatch");
    REQUIRE((counts.ok == 0 && counts.unpaired == 0 && counts.mismatch == rectOnly));

    // A call on another device is not this clear's: unpaired, and the exit hook
    // compares nothing.
    fixture.Enter();
    fixture.Call(fixture.device + 0x100, 1280);
    fixture.Leave();
    counts = TakeClearDeriveCounts();
    UNSCOPED_INFO("a call on another device is unpaired and not compared");
    REQUIRE((counts.ok == 0 && counts.unsupported == 0 && counts.unpaired == 1 && counts.mismatch == decltype(counts.mismatch){}));

    // An exit hook without an entry is ignored.
    fixture.Leave();
    counts = TakeClearDeriveCounts();
    UNSCOPED_INFO("an exit without an entry counts nothing");
    REQUIRE((counts.ok == 0 && counts.unpaired == 0));
    REXCVAR_SET(native_clear_derive_check, false);
}

// Clear effect hooks compare generated dirty words for slot 0 at 1280x720.
void testClearEffectHooks()
{
    // Dirty words come from generated stores (g158:13166-13924;
    // g88:13109-13389; g96:13268-13306).
    const ConsumeCvars consumeCvars;
    using namespace rerevved::gpu::guest_d3d;
    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto*                memory = runtime.memory();
    ClearProbeFixture    fixture(memory);
    constexpr ClearWatch entryWatch{ { 0xA5C30F0F5A5E3F0FULL, 0xFEDCBA9876543210ULL, 0x0F0F0F0F0F0F0F0FULL }, 0x08, 0x00, 0, 0, 0 };
    constexpr ClearWatch exitWatch{ { 0xA78013A0101E0FE8ULL, 0xFE800008F6543210ULL, 0x0F0F0F1000080000ULL }, 0x0C, 0x00, 0, 0, 0 };

    const auto store = [&](const ClearWatch& watch)
    {
        for (uint32_t i = 0; i < kClearDirtyWords; ++i)
        {
            storeGuest(memory, fixture.device + 0x10 + 8 * i, uint32_t(watch.dirty[i] >> 32));
            storeGuest(memory, fixture.device + 0x14 + 8 * i, uint32_t(watch.dirty[i]));
        }
        storeGuest(memory, fixture.device + 0x2ABC, (uint32_t(watch.allocByte) << 16) | (uint32_t(watch.stateByte) << 8));
    };
    const auto takeAll = [&]
    {
        return std::pair{ TakeClearEffectCounts(), TakeClearDeriveCounts() };
    };
    const auto effectsQuiet = [](const ClearEffectCounts& counts)
    {
        return counts.exactOk == 0 && counts.earlyOk == 0 && counts.earlyChanged == 0 && counts.allocFailed == 0 && counts.unsupported == 0 && counts.unpaired == 0 &&
               counts.mismatch == decltype(counts.mismatch){} && counts.pathUntiled == 0 && counts.color == 0 && counts.fast == 0;
    };
    // One clear: the entry state in guest memory, the entry hook, the guest's
    // call, the exit state stored by `leave`, the exit hook.
    const auto clear = [&](const ClearWatch& from, const ClearWatch& to)
    {
        store(from);
        fixture.Enter();
        fixture.Call(fixture.device, 1280);
        store(to);
        fixture.Leave();
        return takeAll();
    };

    // Disabled, the entry stacks no probe and the exit compares nothing.
    REXCVAR_SET(native_clear_derive_check, false);
    REXCVAR_SET(native_clear_effect_check, false);
    takeAll();
    {
        const auto [effects, derive] = clear(entryWatch, exitWatch);
        UNSCOPED_INFO("a disabled effect probe records nothing");
        REQUIRE((effectsQuiet(effects) && derive.ok == 0 && derive.unpaired == 0));
    }

    // The effect check alone: a matching exit is ok, and the derive check stays silent.
    REXCVAR_SET(native_clear_effect_check, true);
    {
        const auto [effects, derive] = clear(entryWatch, exitWatch);
        UNSCOPED_INFO("a matching exit counts ok");
        REQUIRE((effects.exactOk == 1 && effects.unsupported == 0 && effects.unpaired == 0 && effects.mismatch == decltype(effects.mismatch){}));
        UNSCOPED_INFO("the path coverage of the fixture call");
        REQUIRE((effects.pathUntiled == 1 && effects.fast == 1 && effects.color == 1 && effects.depthOnly == 0));
        UNSCOPED_INFO("the derive check is off");
        REQUIRE((derive.ok == 0 && derive.unsupported == 0 && derive.unpaired == 0));
    }

    // One corrupted word gives exactly its mismatch.
    for (std::size_t field = 0; field < kClearEffectFieldCount; ++field)
    {
        auto corrupted = exitWatch;
        if (field < kClearDirtyWords)
            corrupted.dirty[field] ^= 1ULL << 36;
        else
            corrupted.allocByte ^= 0x01;
        const auto [effects, derive] = clear(entryWatch, corrupted);
        decltype(effects.mismatch) expected{};
        expected[field] = 1;
        UNSCOPED_INFO(std::string("a corrupted ") + kClearEffectFieldNames[field] + " counts only its field");
        REQUIRE((effects.exactOk == 0 && effects.mismatch == expected && effects.unsupported == 0 && effects.unpaired == 0));
    }

    // A marker that appears during the call is an allocation failure, not compared.
    {
        auto failed = exitWatch;
        failed.allocByte |= 0x20;
        failed.dirty[0]              = 0;
        const auto [effects, derive] = clear(entryWatch, failed);
        UNSCOPED_INFO("an allocation failure is counted and not compared");
        REQUIRE((effects.allocFailed == 1 && effects.exactOk == 0 && effects.mismatch == decltype(effects.mismatch){}));
    }

    // An early exit (no depth surface, only depth flags) leaves the words alone.
    {
        fixture.ctx.r4.u64 = 0x10;
        auto unchanged     = clear(entryWatch, entryWatch).first;
        UNSCOPED_INFO("an early exit with unchanged words counts early_ok");
        REQUIRE((unchanged.earlyOk == 1 && unchanged.earlyChanged == 0 && unchanged.exactOk == 0 && unchanged.pathUntiled == 0));
        auto touched = entryWatch;
        touched.dirty[1] |= 1;
        const auto changed = clear(entryWatch, touched).first;
        UNSCOPED_INFO("an early exit that changed a word counts early_changed");
        REQUIRE((changed.earlyOk == 0 && changed.earlyChanged == 1));
        fixture.ctx.r4.u64 = 0x1;
    }

    // An exit without an entry is ignored.
    fixture.Leave();
    {
        const auto [effects, derive] = takeAll();
        UNSCOPED_INFO("an exit without an entry counts nothing");
        REQUIRE(effectsQuiet(effects));
    }

    // Both checks on: each pairs and compares its own.
    REXCVAR_SET(native_clear_derive_check, true);
    {
        const auto [effects, derive] = clear(entryWatch, exitWatch);
        UNSCOPED_INFO("both checks compare one clear");
        REQUIRE((effects.exactOk == 1 && effects.unpaired == 0 && derive.ok == 1 && derive.unpaired == 0));
    }

    // A call on another device drops the probe: unpaired under each check that
    // had one, and nothing compared. A call that never reached its exit hook is
    // dropped by the next entry at the same r1, with the same counting.
    for (const bool derive : { false, true })
        for (const bool effect : { false, true })
        {
            REXCVAR_SET(native_clear_derive_check, derive);
            REXCVAR_SET(native_clear_effect_check, effect);
            takeAll();
            store(entryWatch);
            fixture.Enter();
            fixture.Call(fixture.device + 0x100, 1280);
            fixture.Leave();
            {
                const auto [effects, deriveCounts] = takeAll();
                UNSCOPED_INFO(std::format("effect unpaired for another device with derive={} effect={}: {}", derive, effect, effects.unpaired));
                REQUIRE((effects.unpaired == (effect ? 1U : 0U) && effects.exactOk == 0 && effects.unsupported == 0));
                UNSCOPED_INFO(std::format("derive unpaired for another device with derive={} effect={}: {}", derive, effect, deriveCounts.unpaired));
                REQUIRE((deriveCounts.unpaired == (derive ? 1U : 0U) && deriveCounts.ok == 0));
            }
            store(entryWatch);
            fixture.Enter();
            const auto [effects, deriveCounts] = clear(entryWatch, exitWatch);
            UNSCOPED_INFO(std::format("effect unpaired for a dropped call with derive={} effect={}: {}", derive, effect, effects.unpaired));
            REQUIRE((effects.unpaired == (effect ? 1U : 0U) && effects.exactOk == (effect ? 1U : 0U)));
            UNSCOPED_INFO(std::format("derive unpaired for a dropped call with derive={} effect={}: {}", derive, effect, deriveCounts.unpaired));
            REQUIRE((deriveCounts.unpaired == (derive ? 1U : 0U) && deriveCounts.ok == (derive ? 1U : 0U)));
            // The hook already counted a probe of another device; its skipped exit is not counted again.
            takeAll();
            store(entryWatch);
            fixture.Enter();
            fixture.Call(fixture.device + 0x100, 1280);
            const auto [afterEffects, afterDerive] = clear(entryWatch, exitWatch);
            UNSCOPED_INFO(std::format("effect unpaired once for an invalidated dropped call with derive={} effect={}: {}", derive, effect, afterEffects.unpaired));
            REQUIRE((afterEffects.unpaired == (effect ? 1U : 0U) && afterEffects.exactOk == (effect ? 1U : 0U)));
            UNSCOPED_INFO(std::format("derive unpaired once for an invalidated dropped call with derive={} effect={}: {}", derive, effect, afterDerive.unpaired));
            REQUIRE((afterDerive.unpaired == (derive ? 1U : 0U) && afterDerive.ok == (derive ? 1U : 0U)));
        }
    REXCVAR_SET(native_clear_derive_check, false);
    REXCVAR_SET(native_clear_effect_check, false);
}

// Resolve effect hooks compare entry/return words against generated exit state.
void testResolveEffectHooks()
{
    const ConsumeCvars consumeCvars;
    using namespace rerevved::gpu::guest_d3d;
    constexpr uint32_t kImageAllocation = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
    constexpr uint32_t kImageWritable   = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;

    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto* memory = runtime.memory();
    // The pages of the per-format table 0x8208EA10 and of the zero point
    // 0x820D783C, which the derivation reads from the title image.
    auto* image = memory->LookupHeap(0x82080000);
    UNSCOPED_INFO("the title image pages are committed");
    REQUIRE((image && image->AllocFixed(0x82080000, 0x10000, 0x10000, kImageAllocation, kImageWritable) &&
             image->AllocFixed(0x820D0000, 0x10000, 0x10000, kImageAllocation, kImageWritable)));
    ResolveFixture guest(memory);
    auto           assembler = std::make_shared<FakeRenderSink>();
    FailureLog     log;
    rerevved::gpu::completion::PublishGuestRenderAssembler(assembler);
    auto options = quietOptions();
    log.Attach(options);
    GuestVideoServices service;
    UNSCOPED_INFO("effect probe service start must succeed");
    REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
    service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
    service.EnableReadPointerWriteBack(guest.rptr, kWritebackBlockSizeLog2);

    const GuestWordReader reader = [memory](uint32_t address, uint32_t& value)
    {
        value = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
        return true;
    };
    const std::array<uint32_t, 8> regs{ uint32_t(guest.ctx.r3.u64), 0, 0, uint32_t(guest.ctx.r6.u64), 0, 0, 0, 0 };
    constexpr uint32_t            kFence = 0x1234, kSp = 0x70000000, kFrame = kSp - kResolveFrameSize;
    storeGuest(memory, guest.device + 0x2A9C, kFence);
    guest.Stack(kSp);
    const auto effects = DeriveResolveEffects(reader, regs);
    UNSCOPED_INFO("the fixture call is modeled");
    REQUIRE((effects.has_value() && effects->words == *DeriveResolveWords(reader, regs)));

    const auto store = [&](const ResolveWatch& watch)
    {
        for (uint32_t i = 0; i < 4; ++i)
            storeGuest(memory, guest.device + 0x2A18 + 4 * i, watch.copy[i]);
        for (uint32_t i = 0; i < kResolveDirtyWords; ++i)
        {
            storeGuest(memory, guest.device + 0x10 + 8 * i, uint32_t(watch.dirty[i] >> 32));
            storeGuest(memory, guest.device + 0x14 + 8 * i, uint32_t(watch.dirty[i]));
        }
        storeGuest(memory, guest.device + 0x2A44, watch.size);
        storeGuest(memory, guest.device + 0x2A9C, watch.fence);
        storeGuest(memory, guest.device + 0x2ABC, (uint32_t(watch.allocByte) << 16) | (uint32_t(watch.stateByte) << 8));
        storeGuest(memory, guest.descriptor + 8, watch.dest8);
        storeGuest(memory, guest.descriptor + 12, watch.dest12);
        storeGuest(memory, guest.device + 0x30, watch.cursor);
        storeGuest(memory, guest.device + 0x34, watch.limit34);
        storeGuest(memory, guest.device + 0x38, watch.limit38);
        storeGuest(memory, guest.device + 0x3A44, watch.poolPos);
        storeGuest(memory, guest.device + 0x3A4C, watch.wrapPos);
        storeGuest(memory, guest.device + 0x3A54, watch.poolUsed);
        storeGuest(memory, guest.device + 0x3A58, watch.poolThreshold);
        storeGuest(memory, guest.device + 0x31CC, watch.targetCount);
        storeGuest(memory, guest.device + 0x34CC, watch.poolObject);
        if (watch.poolObject)
        {
            storeGuest(memory, watch.poolObject + 152, watch.poolBase);
            storeGuest(memory, watch.poolObject + 156, watch.poolSize);
        }
    };
    ResolveWatch entry;
    entry.copy          = { 0x00100000, 0, (720u << 16) | 1280, 0x01000300 };
    entry.size          = 0x77;
    entry.dirty         = { 0x1234, 0x40, 0x4 };
    entry.allocByte     = 0x80;
    entry.stateByte     = 0x0F;
    entry.fence         = kFence;
    entry.cursor        = kRingCursor;
    entry.limit34       = kRingLimit34;
    entry.limit38       = kRingLimit38;
    entry.poolUsed      = kPoolUsed;
    entry.poolThreshold = kPoolThreshold;
    entry.targetCount   = kRingTargets;
    entry.poolObject    = guest.poolObject;
    entry.poolBase      = kPoolBase;
    entry.poolSize      = kPoolSize;
    // What the body leaves: the zeroed copy words around the derived base and
    // pitch, the predicted bits ORed in, the header words and the bytes.
    const auto body = [&](const ResolveWatch& from)
    {
        ResolveWatch exit = from;
        exit.copy         = { 0, effects->words.destBase, effects->words.destPitch, 0 };
        for (std::size_t i = 0; i < kResolveDirtyWords; ++i)
            exit.dirty[i] |= effects->setBeforeAlloc[i] | effects->setAfterAlloc[i];
        exit.stateByte &= uint8_t(~kResolveStateClearedBit);
        exit.dest8    = exit.fence;
        exit.dest12   = exit.fence;
        exit.limit34  = from.limit34 - kSideBuffer;
        exit.limit38  = from.limit38 - kSideBuffer;
        exit.poolPos  = kPoolBuffer;
        exit.poolUsed = from.poolUsed + kSideBuffer;
        return exit;
    };

    // One call: the entry observer and the copy hook with the entry state in
    // guest memory, then `leave` changes the return state, which is stored
    // before the return hook runs.
    const auto call = [&](const ResolveWatch& from, const std::function<void(ResolveWatch&)>& leave)
    {
        store(from);
        rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269E520, guest.ctx);
        rerevved::gpu::completion::detail::RecordGuestResolveCopy(guest.device, kFrame);
        auto exit = body(from);
        leave(exit);
        store(exit);
        rerevved::gpu::completion::detail::RecordGuestResolveReturn(kFrame);
        return TakeResolveEffectCounts();
    };
    const auto none = [](ResolveWatch&)
    {
    };
    const auto quiet = [](const ResolveEffectCounts& counts)
    {
        return counts.ok == 0 && counts.unsupported == 0 && counts.unpaired == 0 && counts.dirtyCleared == 0 && counts.cleared1E0000 == 0 && counts.allocFailed == 0 &&
               counts.listPath == 0 && counts.mismatch == decltype(counts.mismatch){};
    };

    // Disabled, the entry stacks no probe and the return compares nothing.
    REXCVAR_SET(native_resolve_effect_check, false);
    TakeResolveEffectCounts();
    UNSCOPED_INFO("a disabled probe records nothing");
    REQUIRE(quiet(call(entry, none)));

    REXCVAR_SET(native_resolve_effect_check, true);
    auto counts = call(entry, none);
    UNSCOPED_INFO("a matching exit counts ok");
    REQUIRE((counts.ok == 1 && counts.unsupported == 0 && counts.unpaired == 0 && counts.mismatch == decltype(counts.mismatch){} && counts.listPath == 0 && counts.allocFailed == 0 &&
             counts.dirtyCleared == 0));

    // A thread stack that reaches the device words ([limit, sp + 104)): Original's saves would alias device
    // words, so the probe derives nothing and counts the call as a skipped exact exit with the stack_overlap
    // reason. The frame is below the entry r1 by the body's 368 bytes either way.
    {
        const uint32_t overlapSp    = guest.device + 0x1000;
        const uint32_t overlapFrame = overlapSp - kResolveFrameSize;
        guest.Stack(overlapSp);
        store(entry);
        rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269E520, guest.ctx);
        rerevved::gpu::completion::detail::RecordGuestResolveCopy(guest.device, overlapFrame);
        store(body(entry));
        rerevved::gpu::completion::detail::RecordGuestResolveReturn(overlapFrame);
        guest.Stack(kSp);
        const auto                   taken = TakeResolveEffectCounts();
        decltype(taken.exactRefused) refused{};
        refused[static_cast<std::size_t>(ResolveExitRefusal::StackOverlap)] = 1;
        UNSCOPED_INFO("a stack over the device words is skipped as stack_overlap and nothing is derived");
        REQUIRE((taken.ok == 0 && taken.unsupported == 0 && taken.exactOk == 0 && taken.exactSkipped == 1 && taken.exactRefused == refused && taken.exactMismatch == decltype(taken.exactMismatch){}));
        UNSCOPED_INFO("the probe counter is named stack_overlap");
        REQUIRE(FormatResolveEffectCounts(taken).contains(" resolve_effect_exact_skipped_stack_overlap=1"));
    }

    // One corrupted watched word gives exactly its mismatch field.
    const auto corrupt = [&](const char* name, ResolveEffectField field, const std::function<void(ResolveWatch&)>& change)
    {
        const auto               taken = call(entry, change);
        decltype(taken.mismatch) expected{};
        expected[static_cast<std::size_t>(field)] = 1;
        UNSCOPED_INFO(std::string("a corrupted ") + name + " counts only its field");
        REQUIRE((taken.ok == 0 && taken.mismatch == expected && taken.unsupported == 0 && taken.unpaired == 0));
    };
    corrupt("copy word", ResolveEffectField::CopyWords, [](ResolveWatch& exit)
            {
                exit.copy[0] = 7;
            });
    corrupt("size word", ResolveEffectField::Size, [](ResolveWatch& exit)
            {
                exit.size ^= 1;
            });
    corrupt("missing dirty bit", ResolveEffectField::DirtySetMissing, [](ResolveWatch& exit)
            {
                exit.dirty[0] &= ~(1ULL << 41);
            });
    corrupt("extra dirty bit", ResolveEffectField::DirtyExtra, [](ResolveWatch& exit)
            {
                exit.dirty[2] |= 0x4000;
            });
    corrupt("state byte", ResolveEffectField::Bytes, [](ResolveWatch& exit)
            {
                exit.stateByte |= kResolveStateClearedBit;
            });
    corrupt("header word", ResolveEffectField::Dest, [](ResolveWatch& exit)
            {
                exit.dest8 ^= 1;
            });

    // The ring space wait moves the fence by two between the entry and the
    // return; the header words then hold the fence at the return (g167:15104).
    counts = call(entry, [](ResolveWatch& exit)
                  {
                      exit.fence += 2;
                      exit.dest8 = exit.dest12 = exit.fence;
                  });
    UNSCOPED_INFO("header words holding the fence at the return count ok");
    REQUIRE((counts.ok == 1 && counts.listPath == 0 && counts.mismatch == decltype(counts.mismatch){}));
    // The exact exit predicts both words from the fence at the ENTRY: a call that
    // waited had the ring flush, which the admission of the consume refuses, so
    // the old probe's accepted pair is a dest_8 and dest_12 mismatch of it.
    UNSCOPED_INFO("a header pair at the moved fence is an exact mismatch");
    REQUIRE((counts.exactOk == 0 && counts.exactMismatch[10] == 1 && counts.exactMismatch[11] == 1 && counts.exactSkipped == 0));
    corrupt("header pair left at the entry fence", ResolveEffectField::Dest, [](ResolveWatch& exit)
            {
                exit.fence += 2;
            });
    corrupt("header pair outside the fence sequence", ResolveEffectField::Dest, [](ResolveWatch& exit)
            {
                exit.fence += 2;
                exit.dest8 = exit.dest12 = exit.fence - 8;
            });
    // The list shape without a zero in the fence sequence is no path of the body.
    corrupt("list shape without a zero reload", ResolveEffectField::Dest, [](ResolveWatch& exit)
            {
                exit.fence += 2;
                exit.dest8  = exit.fence - 2;
                exit.dest12 = 0;
            });
    // A fence that rolled over to zero takes the list path.
    auto rolled  = entry;
    rolled.fence = 0xFFFFFFFE;
    counts       = call(rolled, [](ResolveWatch& exit)
                        {
                      exit.fence  = 0;
                      exit.dest8  = 0xFFFFFFFE;
                      exit.dest12 = 0;
                        });
    UNSCOPED_INFO("a zero fence at the return is the list path");
    REQUIRE((counts.ok == 1 && counts.listPath == 1 && counts.mismatch == decltype(counts.mismatch){}));
    // The list path's own allocation failing after the body's succeeded sets the
    // marker and changes nothing else.
    counts = call(rolled, [](ResolveWatch& exit)
                  {
                      exit.fence  = 0;
                      exit.dest8  = 0xFFFFFFFE;
                      exit.dest12 = 0;
                      exit.allocByte |= kResolveAllocFailedBit;
                  });
    UNSCOPED_INFO("a late allocation failure counts ok and failed");
    REQUIRE((counts.ok == 1 && counts.allocFailed == 1 && counts.listPath == 1 && counts.mismatch == decltype(counts.mismatch){}));

    // A flush that drops a 0x1E0000 bit of G+0x10 is counted, not a mismatch.
    auto flushed = entry;
    flushed.dirty[0] |= 0x40000;
    counts = call(flushed, [](ResolveWatch& exit)
                  {
                      exit.dirty[0] &= ~0x40000ULL;
                  });
    UNSCOPED_INFO("a cleared 0x1E0000 bit is counted without a mismatch");
    REQUIRE((counts.ok == 1 && counts.cleared1E0000 == 1 && counts.dirtyCleared == 1 && counts.mismatch == decltype(counts.mismatch){}));

    // An exit without an entry compares nothing.
    rerevved::gpu::completion::detail::RecordGuestResolveReturn(kFrame);
    UNSCOPED_INFO("an exit without an entry counts nothing");
    REQUIRE(quiet(TakeResolveEffectCounts()));

    // A call that never returned is dropped by the next one at the same r1 and
    // counted unpaired; the second pairs.
    store(entry);
    rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269E520, guest.ctx);
    counts = call(entry, none);
    UNSCOPED_INFO("a call that left without its return hook is unpaired");
    REQUIRE((counts.ok == 1 && counts.unpaired == 1));

    // Independent exit (flags 0, pair 0): masks g167:14299-14323,
    // g167:14455-14523, g167:14935-14976; copy g167:15083-15086; headers
    // g167:14928, g167:15112/15114.
    {
        const uint32_t base     = guest.scanout;
        const uint32_t destBase = (((base >> 20) + 512) & 0x1000) + (base & 0x1FFFFFFF);
        ResolveWatch   exit     = entry;
        exit.copy               = { 0, destBase, 720u << 16, 0 };
        exit.dirty              = { entry.dirty[0] | 0x00001380000989E8ULL, entry.dirty[1] | 0x0000000880000000ULL, entry.dirty[2] | 0x80000ULL };
        exit.stateByte          = 0x07;
        exit.dest8 = exit.dest12 = kFence;
        exit.limit34             = kRingLimit34 - kSideBuffer;
        exit.limit38             = kRingLimit38 - kSideBuffer;
        exit.poolPos             = kPoolBuffer;
        exit.poolUsed            = kPoolUsed + kSideBuffer;
        store(entry);
        rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269E520, guest.ctx);
        rerevved::gpu::completion::detail::RecordGuestResolveCopy(guest.device, kFrame);
        store(exit);
        rerevved::gpu::completion::detail::RecordGuestResolveReturn(kFrame);
        counts = TakeResolveEffectCounts();
        UNSCOPED_INFO("the hand-written exit of the fixture call matches the prediction");
        REQUIRE((counts.ok == 1 && counts.mismatch == decltype(counts.mismatch){} && counts.unsupported == 0));
        // Flush drops G+0x10 bits 0..16 and G+0x20 bits 0..37; G+0x18 keeps
        // 0x40 because only bits 34..54 drop.
        UNSCOPED_INFO("the unflushed hand exit is an exact mismatch");
        REQUIRE((counts.exactOk == 0 && counts.exactMismatch[5] == 1 && counts.exactMismatch[7] == 1 && counts.exactSkipped == 0));
        {
            ResolveWatch flushed = exit;
            flushed.dirty        = { 0x00001380000989E8ULL, 0x0000000880000040ULL, 0x80000ULL };
            store(entry);
            rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269E520, guest.ctx);
            rerevved::gpu::completion::detail::RecordGuestResolveCopy(guest.device, kFrame);
            store(flushed);
            rerevved::gpu::completion::detail::RecordGuestResolveReturn(kFrame);
            counts = TakeResolveEffectCounts();
            UNSCOPED_INFO("the flushed hand-written exit is exact");
            REQUIRE((counts.ok == 1 && counts.exactOk == 1 && counts.exactMismatch == decltype(counts.exactMismatch){} && counts.exactSkipped == 0));
        }
        // The side buffer's pool moves are part of the exact exit: a pool word left at
        // its entry value is a mismatch of its own.
        {
            ResolveWatch flushed = exit;
            flushed.dirty        = { 0x00001380000989E8ULL, 0x0000000880000040ULL, 0x80000ULL };
            flushed.poolPos      = 0;
            store(entry);
            rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269E520, guest.ctx);
            rerevved::gpu::completion::detail::RecordGuestResolveCopy(guest.device, kFrame);
            store(flushed);
            rerevved::gpu::completion::detail::RecordGuestResolveReturn(kFrame);
            counts = TakeResolveEffectCounts();
            UNSCOPED_INFO("a pool position left at its entry value is an exact mismatch");
            REQUIRE((counts.exactOk == 0 && counts.exactMismatch[14] == 1 && counts.exactSkipped == 0));
        }
        // A call whose ring might flush, or whose allocation might fail, is skipped
        // by reason and no mismatch of the exact exit: the consume refuses it too.
        {
            auto skipped = [&](const char* name, ResolveExitRefusal reason, const std::function<void(ResolveWatch&)>& change)
            {
                auto from = entry;
                change(from);
                const auto taken = call(from, none);
                UNSCOPED_INFO(std::string(name) + " is skipped by its reason");
                REQUIRE((taken.exactOk == 0 && taken.exactSkipped == 1 && taken.exactRefused[static_cast<std::size_t>(reason)] == 1 && taken.exactMismatch == decltype(taken.exactMismatch){}));
            };
            skipped("a cursor close to the soft limit", ResolveExitRefusal::RingSpace, [](ResolveWatch& w)
                    {
                        w.cursor = w.limit38 - 16;
                    });
            skipped("a side buffer count that passes the threshold", ResolveExitRefusal::RingSpace, [](ResolveWatch& w)
                    {
                        w.poolUsed = w.poolThreshold;
                    });
            skipped("a pool top that leaves no room", ResolveExitRefusal::AllocCapacity, [](ResolveWatch& w)
                    {
                        w.poolPos = w.cursor + 64;
                    });
            skipped("a pool object without a fixed buffer", ResolveExitRefusal::AllocPath, [](ResolveWatch& w)
                    {
                        w.poolBase = 0;
                    });
            // The old probe is untouched by all four.
            const auto taken = call(entry, none);
            UNSCOPED_INFO("the unchanged entry is modeled again");
            REQUIRE((taken.ok == 1 && taken.exactSkipped == 0));
        }
        // Dropping any one of the written bits is a mismatch.
        exit.dirty[0] &= ~0x0000100000000000ULL;
        store(entry);
        rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269E520, guest.ctx);
        store(exit);
        rerevved::gpu::completion::detail::RecordGuestResolveReturn(kFrame);
        counts = TakeResolveEffectCounts();
        UNSCOPED_INFO("the hand-written exit without a bit is a mismatch");
        REQUIRE((counts.ok == 0 && counts.mismatch[static_cast<std::size_t>(ResolveEffectField::DirtySetMissing)] == 1));
    }

    // A call that never returned is dropped by the next one at the same r1. The
    // drop is counted under each check that had a probe on it.
    for (const bool derive : { false, true })
        for (const bool effect : { false, true })
        {
            REXCVAR_SET(native_resolve_derive_check, derive);
            REXCVAR_SET(native_resolve_effect_check, effect);
            TakeResolveDeriveCounts();
            TakeResolveEffectCounts();
            store(entry);
            rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269E520, guest.ctx);
            const auto effectCounts = call(entry, none);
            const auto deriveCounts = TakeResolveDeriveCounts();
            UNSCOPED_INFO(std::format("derive unpaired with derive={} effect={}: {}", derive, effect, deriveCounts.unpaired));
            REQUIRE(deriveCounts.unpaired == (derive ? 1U : 0U));
            UNSCOPED_INFO(std::format("effect unpaired with derive={} effect={}: {}", derive, effect, effectCounts.unpaired));
            REQUIRE(effectCounts.unpaired == (effect ? 1U : 0U));
        }
    REXCVAR_SET(native_resolve_derive_check, false);
    REXCVAR_SET(native_resolve_effect_check, false);
    UNSCOPED_INFO("effect probe service must stop");
    REQUIRE(service.Shutdown());
    rerevved::gpu::completion::PublishGuestRenderAssembler({});
}
#endif

// native_consume_resolve compares replay and Original on identical guest
// memory; replay changes only exit state.

std::string copyKey(const rerevved::gpu::guest_d3d::GuestResolveCopy& copy)
{
    std::string key = std::format("{:08X},{:08X},{:08X},{:08X} fetch", copy.copyControl, copy.destBase, copy.destPitch, copy.destInfo);
    for (const auto word : copy.destFetch)
        key += std::format(" {:08X}", word);
    return key + std::format(" rect {},{},{},{} point {},{} clear {:08X},{:08X},{:08X} packing {}", copy.rectX, copy.rectY, copy.rectWidth, copy.rectHeight, copy.destX, copy.destY, copy.depthClear, copy.colorClear, copy.colorClearLow, static_cast<int>(copy.packing));
}

std::vector<uint8_t> dumpGuest(rex::memory::Memory* memory, uint32_t address, uint32_t size)
{
    const auto* bytes = memory->TranslateVirtual<const uint8_t*>(address);
    return std::vector<uint8_t>(bytes, bytes + size);
}

struct ReplayOutcome
{
    std::vector<std::string> submitted; // target and copy of every resolve the assembler took
    std::vector<std::string> reported;  // every write it was told of
    std::vector<uint8_t>     deviceBefore, deviceAfter, headerBefore, headerAfter;
    uint32_t                 device = 0, header = 0, frontbuffer = 0;
    uint32_t                 released = 0; // RPTR after the kick and the swap
    std::vector<uint64_t>    flushed;
    uint64_t                 spansReleased = 0, partialSpans = 0, swaps = 0;
    uint32_t                 imagesWritten = 0; // guest images the ledger wrote for the resolve
    std::string              unowned;
    std::vector<std::string> failures;
    bool                     consumed = false, registersKept = false;
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ResolveConsumeCounts counts;
#endif
};

// The color target RT1 and the depth surface that BeginTiling saved beside RT0, for the per-tile
// calls of slot 1 and of the depth slot 4: bound, of RT0's extent and samples, at their own EDRAM bases.
void bindSavedTargets(rex::memory::Memory* memory, uint32_t g, bool rt1, bool depth)
{
    const auto bind = [&](uint32_t slotWord, uint32_t savedWord, uint32_t info)
    {
        const uint32_t surface = allocatePhysical(memory);
        storeGuest(memory, surface + 0x24, (uint32_t(1280 - 1) << 18) | (uint32_t(720 - 1) << 3));
        storeGuest(memory, surface + 0x18, 1U << 16);
        storeGuest(memory, surface + 0x1C, info);
        storeGuest(memory, g + slotWord, surface);
        storeGuest(memory, g + savedWord, surface);
    };
    if (rt1)
        bind(0x309C, 0x31BC, 0x400);
    if (depth)
        bind(0x30A8, 0x31C8, 0x800);
}

// The fixture call as Original runs it, or as the consume replays it.
ReplayOutcome runResolve(bool consume, bool nullPool, bool tiled = false, uint32_t flags = 0, uint32_t predicateMask = 0)
{
    using namespace rerevved::gpu::guest_d3d;
    constexpr uint32_t kImageAllocation = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
    constexpr uint32_t kImageWritable   = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
    constexpr uint32_t kFence = 0x1234, kSp = 0x820D8000, kFrame = kSp - kResolveFrameSize;

    rex::Runtime runtime(std::filesystem::path{});
    setupRuntime(runtime);
    auto* memory = runtime.memory();
    auto* image  = memory->LookupHeap(0x82080000);
    UNSCOPED_INFO("the title image pages are committed");
    REQUIRE((image && image->AllocFixed(0x82080000, 0x10000, 0x10000, kImageAllocation, kImageWritable) &&
             image->AllocFixed(0x820D0000, 0x10000, 0x10000, kImageAllocation, kImageWritable) &&
             image->AllocFixed(0x82000000, 0x10000, 0x10000, kImageAllocation, kImageWritable)));
    ResolveFixture guest(memory);
    auto           assembler = std::make_shared<FakeRenderSink>();
    FakeCompletion completion;
    FailureLog     log;
    // A tile's depth or MRT slot is taken whole, as the assembler takes a modeled per-tile resolve.
    assembler->partialResolves = (flags & 7) == 0;
    // Its guest image reaches memory through the ledger, as the native completion source's does.
    completion.images = assembler.get();
    rerevved::gpu::completion::PublishGuestRenderAssembler(assembler);
    auto options     = quietOptions();
    options.drawMode = rerevved::gpu::guest_d3d::NativeDrawMode::Strict;
    options.draws    = &completion;
    log.Attach(options);
    GuestVideoServices service;
    UNSCOPED_INFO("replay service start must succeed");
    REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
    service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
    service.EnableReadPointerWriteBack(guest.rptr, kWritebackBlockSizeLog2);

    // The entry state: dirty bits the flush groups drop and keep, the size, the
    // tiling byte clear, an allocation byte and a state byte, the fence, the
    // header words and the clear words.
    const uint32_t g = guest.device, header = guest.descriptor;
    storeGuest(memory, g + 0x10, 0xFC002000);
    storeGuest(memory, g + 0x14, 0x4003FFFF);
    storeGuest(memory, g + 0x18, 0x80700000);
    storeGuest(memory, g + 0x1C, 0x00000007);
    storeGuest(memory, g + 0x20, 0x40000020);
    storeGuest(memory, g + 0x24, 0x00000004);
    storeGuest(memory, g + 0x2A44, 0x77);
    storeGuest(memory, g + 0x2ABC, 0x00800F00);
    storeGuest(memory, g + 0x2A9C, kFence);
    storeGuest(memory, g + 0x2A2C, 0xAAAA0001);
    storeGuest(memory, g + 0x2A30, 0xBBBB0002);
    storeGuest(memory, g + 0x2A34, 0xCCCC0003);
    storeGuest(memory, header + 8, 0x1111);
    storeGuest(memory, header + 12, 0x2222);
    // The copy words the previous call left, which a consumed call must not read.
    storeGuest(memory, g + 0x2A18, 0xDEAD0001);
    storeGuest(memory, g + 0x2A1C, 0xDEAD0002);
    storeGuest(memory, g + 0x2A20, 0xDEAD0003);
    storeGuest(memory, g + 0x2A24, 0xDEAD0004);
    if (nullPool)
    {
        // A null pool object whose allocator takes the next 24 bytes forward from G+0x34 + 60:
        // the area ends at G+0x3A3C, the segment G+0x3A48 is 0 and the GPU's progress word is level.
        storeGuest(memory, g + 0x34CC, 0);
        storeGuest(memory, g + 0x3A3C, kNullSideEnd);
        storeGuest(memory, g + 0x2A90, guest.poolObject);
        storeGuest(memory, guest.poolObject + 4, 0);
    }
    if (tiled)
    {
        // G+0x33B4 equals the null-pool segment; G+0x33B0 past take end selects
        // the same-segment branch.
        storeGuest(memory, g + 0x2ABC, 0x20800F00);
        storeGuest(memory, g + 0x33B0, 0x80000000);
        storeGuest(memory, g + 0x33B4, 0);
        storeGuest(memory, g + 0x31AC, predicateMask ? 1 : 0);
        storeGuest(memory, g + 0x31A4, predicateMask);
        storeGuest(memory, g + 0x31B8, rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(g + 0x3098)));
        storeGuest(memory, g + 0x337C, 1280);
        storeGuest(memory, g + 0x3380, 720);
        bindSavedTargets(memory, g, (flags & 7) == 1, (flags & 7) == 4);
        for (uint32_t i = 0; i < kRingTargets; ++i)
        {
            storeGuest(memory, g + 0x31D0 + 16 * i, 0);
            storeGuest(memory, g + 0x31D4 + 16 * i, 0);
            storeGuest(memory, g + 0x31D8 + 16 * i, 1280);
            storeGuest(memory, g + 0x31DC + 16 * i, 720);
        }
    }

    if (flags & 0x300)
    {
        // The inputs of the clear blocks (the values of resolve_consume_test.cpp): depth 1.0 with stencil 0 in
        // the packed layout G+0x2888 selects, color format 0 and the image default color (0, 1, 0.5, 0.25).
        guest.ctx.f1.f64  = 1.0;
        guest.ctx.r10.u64 = 0;
        storeGuest(memory, kSp + 92, 0);
        storeGuest(memory, kSp + 100, 0);
        storeGuest(memory, g + 0x2884, 0);
        storeGuest(memory, g + 0x2888, 0x10000);
        storeGuest(memory, g + 0x2940, 3);
        storeGuest(memory, 0x8208EB90, std::bit_cast<uint32_t>(2047.0F));
        storeGuest(memory, 0x8208EB94, std::bit_cast<uint32_t>(16777215.0F));
        storeGuest(memory, 0x820068A8, std::bit_cast<uint32_t>(1.0F));
        for (uint32_t i = 0; i < 4; ++i)
            storeGuest(memory, 0x8208EB50 + 4 * i, 0);
        storeGuest(memory, 0x8208EBD0, 0x08000200);
        storeGuest(memory, 0x8208EBD4, 0x08000A00);
        const float color[4] = { 0.0F, 1.0F, 0.5F, 0.25F };
        for (uint32_t i = 0; i < 4; ++i)
            storeGuest(memory, 0x8208EBE0 + 4 * i, std::bit_cast<uint32_t>(color[i]));
    }

    ReplayOutcome out;
    out.device       = g;
    out.frontbuffer  = guest.frontbuffer;
    out.header       = header;
    out.deviceBefore = dumpGuest(memory, g, 0x10000);
    out.headerBefore = dumpGuest(memory, header, 0x10000);
    guest.Stack(kSp);
    guest.ctx.r4.u64 = flags;

    const GuestWordReader reader = [memory](uint32_t address, uint32_t& value)
    {
        value = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
        return true;
    };
    const std::array<uint32_t, 8> regs{ g, flags, 0, header, 0, 0, 0, 0 };
    const auto                    words = DeriveResolveWords(reader, regs);
    UNSCOPED_INFO("the fixture call is modeled");
    REQUIRE(words.has_value());
    // Written out from the descriptor: the destination base of the scanout view
    // and the pitch word with pitch 0 and height 720.
    UNSCOPED_INFO("the fixture's derived base and pitch");
    REQUIRE((words->destBase == guest.frontbuffer && words->destPitch == (720u << 16)));

    rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269E520, guest.ctx);
    if (consume)
    {
        ringStubsActive.store(true);
        ringStubsValidatedDevice.store(g);
        PPCContext saved;
        std::memcpy(&saved, &guest.ctx, sizeof(saved));
        out.consumed      = rerevved::gpu::completion::detail::ConsumeGuestResolve(guest.ctx, false);
        out.registersKept = std::memcmp(&saved, &guest.ctx, sizeof(saved)) == 0;
        ringStubsActive.store(false);
        ringStubsValidatedDevice.store(0);
        // A fallen-back call records nothing for the span to wait on.
        UNSCOPED_INFO("the fixture call is consumed");
        REQUIRE(out.consumed);
    }
    else
    {
        // What Original does: the body's copy words, the copy hook, the exit
        // stores and the return hook.
        storeGuest(memory, g + 0x2A18, words->copyControl);
        storeGuest(memory, g + 0x2A1C, words->destBase);
        storeGuest(memory, g + 0x2A20, words->destPitch);
        storeGuest(memory, g + 0x2A24, words->destInfo);
        rerevved::gpu::completion::detail::RecordGuestResolveCopy(g, kFrame);
        // The exit state worked out by hand from g167 (see resolve_effect_test.cpp).
        storeGuest(memory, g + 0x2A18, 0);
        storeGuest(memory, g + 0x2A1C, guest.frontbuffer);
        storeGuest(memory, g + 0x2A20, 720u << 16);
        storeGuest(memory, g + 0x2A24, 0);
        storeGuest(memory, g + 0x10, 0xFC001380);
        storeGuest(memory, g + 0x14, 0x000B89E8);
        storeGuest(memory, g + 0x18, 0x80000008);
        storeGuest(memory, g + 0x1C, 0x80000007);
        storeGuest(memory, g + 0x20, 0x40000000);
        storeGuest(memory, g + 0x24, 0x00080000);
        memory->TranslateVirtual<uint8_t*>(g + 0x2ABE)[0] = 0x07;
        // The clear blocks run after the copy hook: the control and packed depth words, the depth clear bit of
        // the allocation byte, and the packed color (worked out by hand from g50:12389-12552 and g209:13049-13171).
        if (flags & 0x200)
        {
            storeGuest(memory, g + 0x2A28, 0x00000F80);
            storeGuest(memory, g + 0x2A2C, 0xF0000000);
            memory->TranslateVirtual<uint8_t*>(g + 0x2ABD)[0] |= 0x04;
        }
        if (flags & 0x100)
        {
            storeGuest(memory, g + 0x2A30, 0x4080FF00);
            storeGuest(memory, g + 0x2A34, 0x4080FF00);
        }
        storeGuest(memory, header + 8, kFence);
        storeGuest(memory, header + 12, kFence);
        // The pool moves of the side buffer: a fixed pool lowers both limits by 24 and stores the buffer
        // top; the null pool object's allocator stores the end of its forward take and leaves the limits.
        if (nullPool)
            storeGuest(memory, g + 0x3A44, kNullBufferEnd);
        else
        {
            storeGuest(memory, g + 0x34, kRingLimit34 - kSideBuffer);
            storeGuest(memory, g + 0x38, kRingLimit38 - kSideBuffer);
            storeGuest(memory, g + 0x3A44, kPoolBuffer);
        }
        storeGuest(memory, g + 0x3A54, kPoolUsed + kSideBuffer);
        rerevved::gpu::completion::detail::RecordGuestResolveReturn(kFrame);
    }
    DEV_DO(out.counts = TakeResolveConsumeCounts());

    out.deviceAfter = dumpGuest(memory, g, 0x10000);
    out.headerAfter = dumpGuest(memory, header, 0x10000);

    // Resolve swap releases the admitted span; worker completion orders
    // readback and RPTR.
    writeMmio(memory, kCpRbWptr, 0x8);
    service.OnSwap(guest.Swap());
    const bool released = waitFor([&]
                                  {
                                      return service.Ledger()->Snapshot().spansReleased >= 1;
                                  });
    UNSCOPED_INFO((consume ? "the consumed resolve's span releases" : "the original resolve's span releases"));
    REQUIRE(released);
    out.released = loadPhysical(memory, guest.rptr);
    UNSCOPED_INFO("the released span's RPTR covers the kick");
    REQUIRE(out.released == 0x8);
    out.imagesWritten = completion.written.load();
    out.flushed       = completion.Flushed();
    const auto ledger = service.Ledger()->Snapshot();
    out.spansReleased = ledger.spansReleased;
    out.partialSpans  = ledger.partialSpans;
    out.swaps         = ledger.swaps;
    out.unowned       = ledger.unowned;
    out.failures      = log.Details();
    for (const auto& call : assembler->submitted)
        out.submitted.push_back(std::format("surface {:08X} {}x{}x{} edram {:03X} tiling {} frontbuffer {} copy {}", call.target.surface, call.target.native.width, call.target.native.height, call.target.native.sampleCount, call.target.native.edramBase, call.target.tiling ? call.target.tiling->count : 0, call.frontbuffer, copyKey(call.copy)) +
                                (call.target.tiling ? std::format(" predicated {} mask {:X}", call.target.tiling->predicated, call.target.tiling->predicateMask) : std::string()));
    for (const auto& copy : assembler->reported)
        out.reported.push_back(copy ? copyKey(*copy) : std::string("unread"));
    UNSCOPED_INFO("replay service must stop");
    REQUIRE(service.Shutdown());
    rerevved::gpu::completion::PublishGuestRenderAssembler({});
    return out;
}

void checkConsumeResolveReplay(bool nullPool, bool tiled = false, uint32_t flags = 0, uint32_t predicateMask = 0)
{
    using namespace rerevved::gpu::guest_d3d;
    REXCVAR_SET(native_consume_resolve, true);
    REXCVAR_SET(native_consume_resolve_tiled, tiled);
    DEV_DO(TakeResolveConsumeCounts());

    const auto original = runResolve(false, nullPool, tiled, flags, predicateMask);
    UNSCOPED_INFO("the original path leaves the ledger without a failure: " + (original.failures.empty() ? std::string() : original.failures[0]) + std::format(" (flags {:X} mask {:X})", flags, predicateMask));
    REQUIRE(original.failures.empty());
    UNSCOPED_INFO("the original path submits one resolve to the assembler");
    REQUIRE((original.submitted.size() == 1 && original.reported.empty()));
    UNSCOPED_INFO("the target carries the tiling the entry observer read: " + original.submitted[0]);
    REQUIRE(original.submitted[0].contains(tiled ? " tiling 4 " : " tiling 0 "));
    // A scanout candidate waits for the swap's native output; any other resolve's image is written once.
    UNSCOPED_INFO("the original path writes one image for a resolve that is no scanout: " + original.submitted[0]);
    REQUIRE(original.imagesWritten == (original.submitted[0].contains(" frontbuffer false ") ? 1U : 0U));
    UNSCOPED_INFO("and the predicate and mask it read: " + original.submitted[0]);
    REQUIRE((!tiled || original.submitted[0].ends_with(std::format(" predicated {} mask {:X}", predicateMask ? 1 : 0, predicateMask))));

    const auto replayed = runResolve(true, nullPool, tiled, flags, predicateMask);

    UNSCOPED_INFO("the fixture call is consumed");
    REQUIRE(replayed.consumed);
    UNSCOPED_INFO("a consumed call changes no register");
    REQUIRE(replayed.registersKept);
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("one consume, no fallback");
    REQUIRE((replayed.counts.consumed == 1 && replayed.counts.consumedTiled == (tiled ? 1U : 0U) && replayed.counts.fallback == std::array<uint64_t, kResolveFallbackCount>{}));
#endif

    // Identical assembler input and ledger outcome.
    UNSCOPED_INFO("the assembler takes the same resolve: " + (replayed.submitted.empty() ? std::string("none") : replayed.submitted[0]) + " vs " + original.submitted[0]);
    REQUIRE(replayed.submitted == original.submitted);
    UNSCOPED_INFO("the same writes are reported to the assembler");
    REQUIRE(replayed.reported == original.reported);
    UNSCOPED_INFO("the same ledger outcome");
    REQUIRE((replayed.released == original.released && replayed.flushed == original.flushed && replayed.spansReleased == original.spansReleased &&
             replayed.partialSpans == original.partialSpans && replayed.imagesWritten == original.imagesWritten && replayed.swaps == original.swaps && replayed.unowned == original.unowned && replayed.failures == original.failures));

    // The copy hook saw the derived words and the clear words of the device,
    // not the stale words the previous call left in G+0x2A18..0x2A24.
    UNSCOPED_INFO("the stale copy words are not read");
    REQUIRE(!replayed.submitted[0].contains("DEAD000"));
    UNSCOPED_INFO("the clear words of the device reach the copy");
    REQUIRE(replayed.submitted[0].contains("clear AAAA0001,BBBB0002,CCCC0003"));

    // Memory: exactly the exit state changed, nowhere else, by hand.
    const auto expected = [&](std::vector<uint8_t> bytes, const std::vector<std::pair<uint32_t, uint32_t>>& words, const std::vector<std::pair<uint32_t, uint8_t>>& singles)
    {
        for (const auto& [offset, value] : words)
            rex::memory::store_and_swap<uint32_t>(bytes.data() + offset, value);
        for (const auto& [offset, value] : singles)
            bytes[offset] = value;
        return bytes;
    };
    // The worked exit: the copy words around the derived base and pitch, the flushed dirty words, the state byte,
    // and the side buffer allocation's moves (see runResolve).
    std::vector<std::pair<uint32_t, uint32_t>> deviceWords = { { 0x2A18, 0 },
                                                               { 0x2A1C, replayed.frontbuffer },
                                                               { 0x2A20, 720u << 16 },
                                                               { 0x2A24, 0 },
                                                               { 0x10, 0xFC001380 },
                                                               { 0x14, 0x000B89E8 },
                                                               { 0x18, 0x80000008 },
                                                               { 0x1C, 0x80000007 },
                                                               { 0x20, 0x40000000 },
                                                               { 0x24, 0x00080000 },
                                                               { 0x3A54, kPoolUsed + kSideBuffer } };
    if (nullPool)
        deviceWords.push_back({ 0x3A44, kNullBufferEnd });
    else
        deviceWords.insert(deviceWords.end(), { { 0x34, kRingLimit34 - kSideBuffer }, { 0x38, kRingLimit38 - kSideBuffer }, { 0x3A44, kPoolBuffer } });
    std::vector<std::pair<uint32_t, uint8_t>> deviceBytes = { { 0x2ABE, 0x07 } };
    if (flags & 0x200)
    {
        deviceWords.insert(deviceWords.end(), { { 0x2A28, 0x00000F80 }, { 0x2A2C, 0xF0000000 } });
        deviceBytes.push_back({ 0x2ABD, 0x84 });
    }
    if (flags & 0x100)
        deviceWords.insert(deviceWords.end(), { { 0x2A30, 0x4080FF00 }, { 0x2A34, 0x4080FF00 } });
    const auto wantDevice = expected(replayed.deviceBefore, deviceWords, deviceBytes);
    UNSCOPED_INFO("the device changed by exactly the exit state");
    REQUIRE(replayed.deviceAfter == wantDevice);
    const auto wantHeader = expected(replayed.headerBefore, { { 8, 0x1234 }, { 12, 0x1234 } }, {});
    UNSCOPED_INFO("the destination header changed by exactly its two words");
    REQUIRE(replayed.headerAfter == wantHeader);
    UNSCOPED_INFO("the memory equals Original's");
    REQUIRE((replayed.deviceAfter == original.deviceAfter && replayed.headerAfter == original.headerAfter));
    // The 0x10000-byte device block is compared whole, so the first and last
    // words of the device and every byte around the stores are unchanged.
    REXCVAR_SET(native_consume_resolve_tiled, false);
}

void testConsumeResolveReplay()
{
    checkConsumeResolveReplay(false);
    checkConsumeResolveReplay(true);
    checkConsumeResolveReplay(false, true);
    checkConsumeResolveReplay(true, true);
    // The per-tile calls of 0x8250AC90: the predicate set to the tile's mask 3 << 2i, the depth Resolve
    // (flags 4), the color targets, and the clear flags of a tile that is not the last.
    for (const bool nullPool : { false, true })
        for (const auto& [flags, mask] : { std::pair{ 4U, 3U }, std::pair{ 0x300U, 3U }, std::pair{ 0x101U, 0xCU }, std::pair{ 0x4U, 0x30U }, std::pair{ 0U, 0x30U }, std::pair{ 1U, 0x30U } })
            checkConsumeResolveReplay(nullPool, true, flags, mask);
    // Clear flags: the same exit plus the words of the clear blocks, with and without the bracket.
    checkConsumeResolveReplay(false, false, 0x200);
    checkConsumeResolveReplay(true, false, 0x100);
    checkConsumeResolveReplay(false, false, 0x300);
    checkConsumeResolveReplay(true, true, 0x300);
}

// The runtime, device, ledger and ring stubs of one fallback scenario.
struct ConsumeRig
{
    // A page of the title image, which the runtime maps: the clear blocks read the caller's stack words.
    static constexpr uint32_t kSp = 0x820D8000;

    rex::Runtime                    runtime{ std::filesystem::path{} };
    rex::memory::Memory*            memory = nullptr;
    std::optional<ResolveFixture>   guest;
    std::shared_ptr<FakeRenderSink> assembler = std::make_shared<FakeRenderSink>();
    FailureLog                      log;
    GuestVideoServices              service;
    std::vector<uint8_t>            before;
    std::optional<uint32_t>         limit; // the thread's stack limit for the next Enter, by default 0x400 below r1

    explicit ConsumeRig(bool publishAssembler = true)
    {
        constexpr uint32_t kImageAllocation = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
        constexpr uint32_t kImageWritable   = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
        setupRuntime(runtime);
        memory      = runtime.memory();
        auto* image = memory->LookupHeap(0x82080000);
        UNSCOPED_INFO("the title image pages are committed");
        REQUIRE((image && image->AllocFixed(0x82080000, 0x10000, 0x10000, kImageAllocation, kImageWritable) &&
                 image->AllocFixed(0x820D0000, 0x10000, 0x10000, kImageAllocation, kImageWritable) &&
                 image->AllocFixed(0x82000000, 0x10000, 0x10000, kImageAllocation, kImageWritable)));
        guest.emplace(memory);
        if (publishAssembler)
            rerevved::gpu::completion::PublishGuestRenderAssembler(assembler);
        // Census keeps the ledger accepting after a record it cannot own, so one scenario can run many calls.
        auto options     = quietOptions();
        options.drawMode = rerevved::gpu::guest_d3d::NativeDrawMode::Census;
        log.Attach(options);
        UNSCOPED_INFO("fallback service start must succeed");
        REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
        service.InitializeRingBuffer(kRingPointer, kRingSizeLog2);
        service.EnableReadPointerWriteBack(guest->rptr, kWritebackBlockSizeLog2);
        storeGuest(memory, guest->device + 0x2A9C, 0x1234);
        storeGuest(memory, guest->device + 0x2ABC, 0x00000F00);
        rerevved::gpu::guest_d3d::ringStubsActive.store(true);
        rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(guest->device);
        DEV_DO(rerevved::gpu::guest_d3d::TakeResolveConsumeCounts());
        before = dumpGuest(memory, guest->device, 0x10000);
    }

    ~ConsumeRig()
    {
        rerevved::gpu::guest_d3d::ringStubsActive.store(false);
        rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(0);
        rerevved::gpu::completion::PublishGuestRenderAssembler({});
        REXCVAR_SET(native_consume_resolve, false);
        REXCVAR_SET(native_consume_resolve_tiled, false);
    }

    uint32_t G() const
    {
        return guest->device;
    }

    // The entry observer, then the consume, for a call entered with r1 = sp.
    bool Enter(uint32_t sp, bool diagnostics)
    {
        guest->Stack(sp, limit);
        rerevved::gpu::completion::ObserveGuestProgressEntry(0x8269E520, guest->ctx);
        return rerevved::gpu::completion::detail::ConsumeGuestResolve(guest->ctx, diagnostics);
    }

    // Original's copy and return hooks for a call that was not consumed.
    void Finish(uint32_t sp)
    {
        rerevved::gpu::completion::detail::RecordGuestResolveCopy(G(), sp - rerevved::gpu::guest_d3d::kResolveFrameSize);
        rerevved::gpu::completion::detail::RecordGuestResolveReturn(sp - rerevved::gpu::guest_d3d::kResolveFrameSize);
    }

    // True when the counts since the last take are one fallback of this reason and no consume.
    static bool OnlyFallback([[maybe_unused]] rerevved::gpu::guest_d3d::ResolveFallback reason)
    {
#if REREVVED_DEVELOPER
        const auto                                                            counts = rerevved::gpu::guest_d3d::TakeResolveConsumeCounts();
        std::array<uint64_t, rerevved::gpu::guest_d3d::kResolveFallbackCount> want{};
        want[static_cast<std::size_t>(reason)] = 1;
        return counts.consumed == 0 && counts.fallback == want;
#else
        return true;
#endif
    }

    bool Untouched() const
    {
        return dumpGuest(memory, guest->device, 0x10000) == before;
    }

    // Puts the device page back as the rig found it, after a scenario consumed a call.
    void Restore()
    {
        std::memcpy(memory->TranslateVirtual<uint8_t*>(guest->device), before.data(), before.size());
    }
};

// Fallback leaves guest bytes and the probe stack unchanged.
void testConsumeResolveFallbacks()
{
    using namespace rerevved::gpu::guest_d3d;
    ConsumeRig rig;
    auto&      guest  = *rig.guest;
    auto*      memory = rig.memory;
    const auto kSp    = ConsumeRig::kSp;

    // The cvar off consumes nothing and counts nothing, even for an admitted call.
    REXCVAR_SET(native_consume_resolve, false);
    UNSCOPED_INFO("the cvar off consumes nothing");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    {
#if REREVVED_DEVELOPER
        const auto counts = TakeResolveConsumeCounts();
        UNSCOPED_INFO("the cvar off counts nothing");
        REQUIRE((counts.consumed == 0 && counts.fallback == std::array<uint64_t, kResolveFallbackCount>{}));
#endif
        UNSCOPED_INFO("the cvar off writes nothing");
        REQUIRE(rig.Untouched());
    }

    REXCVAR_SET(native_consume_resolve, true);
    // Diagnostics need the body.
    UNSCOPED_INFO("a diagnostic keeps Original");
    REQUIRE(!rig.Enter(kSp, true));
    rig.Finish(kSp);
    UNSCOPED_INFO("diagnostics counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::Diagnostics));
#if REREVVED_DEVELOPER
    // The effect and derive probes need the body.
    REXCVAR_SET(native_resolve_effect_check, true);
    UNSCOPED_INFO("the effect probe keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    REXCVAR_SET(native_resolve_effect_check, false);
    TakeResolveEffectCounts();
    UNSCOPED_INFO("the effect probe counts diagnostics");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::Diagnostics));
    REXCVAR_SET(native_resolve_derive_check, true);
    UNSCOPED_INFO("the derive probe keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    REXCVAR_SET(native_resolve_derive_check, false);
    TakeResolveDeriveCounts();
    UNSCOPED_INFO("the derive probe counts diagnostics");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::Diagnostics));
#endif

    // Ring stubs not ready for this device.
    ringStubsValidatedDevice.store(rig.G() + 0x10000);
    UNSCOPED_INFO("stubs not ready keep Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("stubs counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::Stubs));
    ringStubsValidatedDevice.store(rig.G());

    // The thread's stack reach [limit, sp + 104) overlaps the device words: Original's saves would land in words
    // the model reads. Tested after the state and before the tiling words, with nothing written; the edges
    // are exact.
    {
        const uint32_t g = rig.G();
        for (const uint32_t sp : { g + 0x40, g + 0x2000, g + 0x3A5C + 0x400 - 1 })
        {
            UNSCOPED_INFO("a stack over the device keeps Original");
            REQUIRE(!rig.Enter(sp, false));
            rig.Finish(sp);
            UNSCOPED_INFO("the stack counted");
            REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::StackOverlap));
        }
        UNSCOPED_INFO("a refused stack writes nothing");
        REQUIRE(rig.Untouched());
        storeGuest(memory, g + 0x2ABC, 0x10000F00);
        UNSCOPED_INFO("a stack over a tiled device keeps Original");
        REQUIRE(!rig.Enter(g + 0x40, false));
        rig.Finish(g + 0x40);
        UNSCOPED_INFO("the stack is reported before the tiling words");
        REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::StackOverlap));
        storeGuest(memory, g + 0x2ABC, 0x00000F00);
        ringStubsValidatedDevice.store(g + 0x10000);
        UNSCOPED_INFO("stubs not ready keep Original");
        REQUIRE(!rig.Enter(g + 0x40, false));
        rig.Finish(g + 0x40);
        UNSCOPED_INFO("the stubs are reported before the stack");
        REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::Stubs));
        ringStubsValidatedDevice.store(g);

        // The edges, by the stack limit: the reach [limit, sp + 104) against [G, G + 0x3A5C).
        const auto verdict = [&](uint32_t limit, uint32_t sp)
        {
            rig.limit           = limit;
            const bool consumed = rig.Enter(sp, false);
            rig.limit.reset();
            if (!consumed)
                rig.Finish(sp);
#if REREVVED_DEVELOPER
            const auto counts = TakeResolveConsumeCounts();
            if (consumed)
                return std::optional<ResolveFallback>{};
            for (std::size_t i = 0; i < kResolveFallbackCount; ++i)
                if (counts.fallback[i])
                    return std::optional<ResolveFallback>{ static_cast<ResolveFallback>(i) };
            return std::optional<ResolveFallback>{};
#else
            if (consumed)
                return std::optional<ResolveFallback>{};
            return std::optional<ResolveFallback>{ ResolveFallback::StackOverlap };
#endif
        };
        UNSCOPED_INFO("a reach ending exactly at G is consumed");
        REQUIRE(verdict(g - 0x400, g - 104) == std::nullopt);
        UNSCOPED_INFO("a reach one byte into the device words is refused");
        REQUIRE(verdict(g - 0x400, g - 103) == ResolveFallback::StackOverlap);
        UNSCOPED_INFO("a reach starting at the end of the device words is consumed");
        REQUIRE(verdict(g + 0x3A5C, g + 0x3A5C + 0x400) == std::nullopt);
        UNSCOPED_INFO("a reach starting on the last device byte is refused");
        REQUIRE(verdict(g + 0x3A5C - 1, g + 0x3A5C + 0x400) == ResolveFallback::StackOverlap);
        // The pool object G+0x34CC names, put in the frame at r1 - 160 where Original's link register save lands.
        {
            const uint32_t sp     = ConsumeRig::kSp;
            const uint32_t pool   = sp - 160;
            const auto     load32 = [&](uint32_t address)
            {
                return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
            };
            const uint32_t oldPool = load32(g + 0x34CC);
            storeGuest(memory, pool + 152, load32(oldPool + 152));
            storeGuest(memory, pool + 156, load32(oldPool + 156));
            storeGuest(memory, g + 0x34CC, pool);
            UNSCOPED_INFO("a pool object in the frame is refused");
            REQUIRE(verdict(sp - 0x400, sp) == ResolveFallback::StackOverlap);
            storeGuest(memory, g + 0x34CC, oldPool);
            UNSCOPED_INFO("the same stack with the pool object elsewhere is consumed");
            REQUIRE(verdict(sp - 0x400, sp) == std::nullopt);
        }
        rig.Restore();
    }

    // Tiling, the clear color, the list path and the allocation marker read the device and the registers.
    storeGuest(memory, rig.G() + 0x33B0, 0x40);
    UNSCOPED_INFO("a bracket word without the tiling byte keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the disagreement counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::TilingState));
    storeGuest(memory, rig.G() + 0x33B0, 0);
    storeGuest(memory, rig.G() + 0x2ABC, 0x10000F00);
    UNSCOPED_INFO("tiling byte bit 0x10 keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("tiling counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::Tiling));
    storeGuest(memory, rig.G() + 0x2ABC, 0x00000F00);
    storeGuest(memory, rig.G() + 0x31AC, 1);
    UNSCOPED_INFO("a predicate keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the predicate counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::TilingPredicate));
    storeGuest(memory, rig.G() + 0x31AC, 0);
    // The bracket form: Original with the tiled cvar off, even for a predicate-free call; with it on, with a predicate it is the predicate.
    storeGuest(memory, rig.G() + 0x2ABC, 0x20000F00);
    storeGuest(memory, rig.G() + 0x33B0, 0x80000000);
    UNSCOPED_INFO("the bracket form keeps Original with the tiled cvar off");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the bracket form counted as tiling");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::Tiling));
    storeGuest(memory, rig.G() + 0x31AC, 1);
    UNSCOPED_INFO("a predicated bracket form keeps Original with the tiled cvar off");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("and counts as tiling_predicate");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::TilingPredicate));
    storeGuest(memory, rig.G() + 0x33B0, 0);
    REXCVAR_SET(native_consume_resolve_tiled, true);
    UNSCOPED_INFO("a predicate without the bracket word keeps Original with the cvar on");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the predicate outside the bracket counted with the cvar on");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::TilingPredicate));
    storeGuest(memory, rig.G() + 0x33B0, 0x80000000);
    storeGuest(memory, rig.G() + 0x31AC, 0);
    // A color clear whose format the host does not model keeps Original, in the bracket form too.
    storeGuest(memory, rig.G() + 0x2884, 0x30000);
    guest.ctx.r4.u64 = 0x100;
    UNSCOPED_INFO("an unmodeled clear color in the bracket form keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("clear color counted in the bracket form");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::ClearColor));
    guest.ctx.r4.u64 = 0;
    REXCVAR_SET(native_consume_resolve_tiled, false);
    storeGuest(memory, rig.G() + 0x2ABC, 0x00000F00);
    storeGuest(memory, rig.G() + 0x33B0, 0);
    guest.ctx.r4.u64 = 0x300;
    UNSCOPED_INFO("an unmodeled clear color keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("clear color counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::ClearColor));
    guest.ctx.r4.u64 = 0;
    storeGuest(memory, rig.G() + 0x2884, 0);
    storeGuest(memory, rig.G() + 0x2A9C, 0);
    UNSCOPED_INFO("the list path keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the list path counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::Fence));
    storeGuest(memory, rig.G() + 0x2A9C, 0x1234);
    storeGuest(memory, rig.G() + 0x2ABC, 0x00200F00);
    UNSCOPED_INFO("the allocation marker keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the allocation marker counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::AllocMarker));
    storeGuest(memory, rig.G() + 0x2ABC, 0x00000F00);

    // The ring and the allocator: Original might flush, wait for ring space or
    // fail its allocation, which the exit state cannot predict.
    const auto word = [&](uint32_t offset)
    {
        return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(rig.G() + offset));
    };
    // The cursor close enough to the soft limit for the words to pass it.
    storeGuest(memory, rig.G() + 0x30, kRingLimit38 - 16);
    UNSCOPED_INFO("a ring that might flush keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the near-limit ring counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::RingSpace));
    storeGuest(memory, rig.G() + 0x30, kRingCursor);
    // The side buffer count past its threshold: the retarget makes the next ring check flush.
    storeGuest(memory, rig.G() + 0x3A54, kPoolThreshold);
    UNSCOPED_INFO("a side buffer count past the threshold keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the threshold counted as ring space");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::RingSpace));
    storeGuest(memory, rig.G() + 0x3A54, kPoolUsed);
    // The pool top so close to the cursor that 0x826A6848 returns zero and Original keeps the copy words.
    storeGuest(memory, rig.G() + 0x3A44, kRingCursor + 64);
    UNSCOPED_INFO("an exhausted pool keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the exhausted pool counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::AllocCapacity));
    storeGuest(memory, rig.G() + 0x3A44, 0);
    // A null pool object is the allocator 0x8269B618. It reads the GPU's progress word through
    // G+0x2A90, which the fixture leaves null.
    const uint32_t poolObject = word(0x34CC);
    storeGuest(memory, rig.G() + 0x34CC, 0);
    UNSCOPED_INFO("a null pool object without a progress block keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the missing progress block counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::Unreadable));
    // With the block (the pool object's page, its word +4 zero) but no side buffer area (G+0x3A3C and
    // G+0x3A50 are zero), the allocator fails.
    storeGuest(memory, rig.G() + 0x2A90, poolObject);
    UNSCOPED_INFO("a null pool allocator with no room keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the failing null pool allocator counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::AllocCapacity));
    // With room, a GPU progress word in another segment makes it wait.
    storeGuest(memory, rig.G() + 0x3A3C, kNullSideEnd);
    storeGuest(memory, poolObject + 4, 3);
    UNSCOPED_INFO("a null pool allocator that would wait keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the waiting null pool allocator counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::AllocWait));
    storeGuest(memory, rig.G() + 0x3A3C, 0);
    storeGuest(memory, rig.G() + 0x2A90, 0);
    storeGuest(memory, poolObject + 4, 0);
    // A pool object without a fixed buffer is the indirect allocator 0x826A6AB8.
    storeGuest(memory, rig.G() + 0x34CC, poolObject);
    storeGuest(memory, poolObject + 152, 0);
    UNSCOPED_INFO("a pool object without a fixed buffer keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the callback allocator counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::AllocPath));
    storeGuest(memory, poolObject + 152, kPoolBase);

    // The fallbacks wrote nothing: the copy hook and the probes never store.
    UNSCOPED_INFO("no fallback wrote guest memory");
    REQUIRE(rig.Untouched());

    // After the fallbacks the probe stack is clean: the admitted calls are consumed.
    UNSCOPED_INFO("the admitted call is consumed after the fallbacks");
    REQUIRE(rig.Enter(kSp, false));
    UNSCOPED_INFO("a consumed call leaves no probe behind");
    REQUIRE(rig.Enter(kSp, false));
    // A deeper call right after a consume is not nested: the consume popped its probe.
    UNSCOPED_INFO("a consumed call leaves no probe for a deeper call");
    REQUIRE(rig.Enter(kSp - 0x1000, false));
    {
#if REREVVED_DEVELOPER
        const auto counts = TakeResolveConsumeCounts();
        UNSCOPED_INFO("three consumes");
        REQUIRE((counts.consumed == 3 && counts.fallback == std::array<uint64_t, kResolveFallbackCount>{}));
#endif
    }
    UNSCOPED_INFO("the ledger saw no failure");
    REQUIRE(rig.log.Details().empty());

    // The consumed call leaves both header words at the fence of its entry and the
    // fence itself alone: it flushes nothing, so Original would not have moved it.
    {
        // The three consumes before moved the pool; start it again from the entry values.
        storeGuest(memory, rig.G() + 0x34, kRingLimit34);
        storeGuest(memory, rig.G() + 0x38, kRingLimit38);
        storeGuest(memory, rig.G() + 0x3A44, 0);
        storeGuest(memory, rig.G() + 0x3A54, kPoolUsed);
        storeGuest(memory, rig.G() + 0x2A9C, 0x5678);
        UNSCOPED_INFO("a call at another fence is consumed");
        REQUIRE(rig.Enter(kSp, false));
        const auto dest = [&](uint32_t offset)
        {
            return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(guest.descriptor + offset));
        };
        UNSCOPED_INFO("both header words are the entry fence");
        REQUIRE((dest(8) == 0x5678 && dest(12) == 0x5678 && word(0x2A9C) == 0x5678));
        UNSCOPED_INFO("the pool words moved by the side buffer");
        REQUIRE((word(0x34) == kRingLimit34 - kSideBuffer && word(0x38) == kRingLimit38 - kSideBuffer && word(0x3A44) == kPoolBuffer && word(0x3A54) == kPoolUsed + kSideBuffer));
        DEV_DO(TakeResolveConsumeCounts());
        storeGuest(memory, rig.G() + 0x2A9C, 0x1234);
    }

    // A call the entry observer could not decode.
    const auto rt0 = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(rig.G() + 0x3098));
    storeGuest(memory, rig.G() + 0x3098, 0);
    UNSCOPED_INFO("an undecoded entry keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("entry state counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::EntryState));
    storeGuest(memory, rig.G() + 0x3098, rt0);
    UNSCOPED_INFO("fallback service must stop");
    REQUIRE(rig.service.Shutdown());
}

// Predicated tile resolves must match Original memory, observations, assembler
// and ledger outcomes.
struct PredicatedSequence
{
    std::vector<std::vector<uint8_t>> devices;   // the device after each call, the predicate words zeroed
    std::vector<std::string>          submitted; // the assembler's resolves without the predicate text
    std::vector<std::string>          masks;     // the tiling the assembler's target carried
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ResolveConsumeCounts counts;
#endif
    std::vector<std::string> failures;
};

PredicatedSequence runPredicatedSequence(bool predicated)
{
    using namespace rerevved::gpu::guest_d3d;
    ConsumeRig rig;
    auto&      guest  = *rig.guest;
    auto*      memory = rig.memory;
    const auto g      = rig.G();
    REXCVAR_SET(native_consume_resolve, true);
    REXCVAR_SET(native_consume_resolve_tiled, true);
    rig.assembler->partialResolves = false;
    // The bracket form: the byte bit 0x20, the bracket word, the saved surfaces (RT0 bound), the screen and three tiles.
    storeGuest(memory, g + 0x2ABC, 0x20000F00);
    storeGuest(memory, g + 0x33B0, 0x80000000);
    storeGuest(memory, g + 0x31CC, 3);
    storeGuest(memory, g + 0x31B8, rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(g + 0x3098)));
    storeGuest(memory, g + 0x337C, 1280);
    storeGuest(memory, g + 0x3380, 720);
    bindSavedTargets(memory, g, true, true);
    const uint32_t xs[] = { 0, 448, 864, 1280 };
    for (uint32_t i = 0; i < 3; ++i)
    {
        storeGuest(memory, g + 0x31D0 + 16 * i, xs[i]);
        storeGuest(memory, g + 0x31D4 + 16 * i, 0);
        storeGuest(memory, g + 0x31D8 + 16 * i, xs[i + 1]);
        storeGuest(memory, g + 0x31DC + 16 * i, 720);
    }
    // The inputs of the clear blocks (see runResolve).
    storeGuest(memory, g + 0x2940, 3);
    storeGuest(memory, g + 0x2888, 0x10000);
    storeGuest(memory, g + 0x2884, 0);
    storeGuest(memory, 0x8208EB90, std::bit_cast<uint32_t>(2047.0F));
    storeGuest(memory, 0x8208EB94, std::bit_cast<uint32_t>(16777215.0F));
    storeGuest(memory, 0x820068A8, std::bit_cast<uint32_t>(1.0F));
    storeGuest(memory, 0x8208EBD0, 0x08000200);
    storeGuest(memory, 0x8208EBD4, 0x08000A00);
    const float color[4] = { 0.0F, 1.0F, 0.5F, 0.25F };
    for (uint32_t i = 0; i < 4; ++i)
        storeGuest(memory, 0x8208EBE0 + 4 * i, std::bit_cast<uint32_t>(color[i]));
    guest.ctx.f1.f64  = 1.0;
    guest.ctx.r10.u64 = 0;

    const struct
    {
        uint32_t tile, flags;
    } calls[] = { { 0, 4 }, { 0, 0x300 }, { 0, 0x101 }, { 2, 4 }, { 2, 0 }, { 2, 1 } };

    PredicatedSequence out;
    for (const auto& call : calls)
    {
        storeGuest(memory, g + 0x31AC, predicated ? 1 : 0);
        storeGuest(memory, g + 0x31A4, predicated ? 3U << (2 * call.tile) : 0U);
        guest.ctx.r4.u64 = call.flags;
        UNSCOPED_INFO(std::format("tile {} flags {:X} is consumed", call.tile, call.flags));
        REQUIRE(rig.Enter(ConsumeRig::kSp, false));
        auto device = dumpGuest(memory, g, 0x10000);
        for (const uint32_t offset : { 0x31A4U, 0x31ACU })
            std::fill_n(device.begin() + offset, 4, uint8_t{ 0 });
        out.devices.push_back(std::move(device));
    }
    DEV_DO(out.counts = TakeResolveConsumeCounts());
    for (const auto& call : rig.assembler->submitted)
    {
        const auto tiling = call.target.tiling;
        out.submitted.push_back(std::format("surface {:08X} {}x{}x{} edram {:03X} tiling {} frontbuffer {} copy {}", call.target.surface, call.target.native.width, call.target.native.height, call.target.native.sampleCount, call.target.native.edramBase, tiling ? tiling->count : 0, call.frontbuffer, copyKey(call.copy)));
        out.masks.push_back(tiling ? std::format("predicated {} mask {:X}", tiling->predicated, tiling->predicateMask) : std::string("none"));
    }
    out.failures = rig.log.Details();
    UNSCOPED_INFO("sequence service must stop");
    REQUIRE(rig.service.Shutdown());
    return out;
}

void testConsumePredicatedSequence()
{
    using namespace rerevved::gpu::guest_d3d;
    const auto predicated = runPredicatedSequence(true);
    const auto plain      = runPredicatedSequence(false);
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("six predicated calls consumed, no fallback");
    REQUIRE((predicated.counts.consumed == 6 && predicated.counts.consumedTiled == 6 && predicated.counts.consumedPredicated == 6 && predicated.counts.fallback == std::array<uint64_t, kResolveFallbackCount>{}));
    UNSCOPED_INFO("six bracket calls consumed");
    REQUIRE((plain.counts.consumed == 6 && plain.counts.consumedTiled == 6 && plain.counts.consumedPredicated == 0 && plain.counts.fallback == std::array<uint64_t, kResolveFallbackCount>{}));
    UNSCOPED_INFO("the shape counts: six predicated, two with clear flags, three tiles");
    REQUIRE((predicated.counts.shape.predicated == 6 && predicated.counts.shape.predicatedClear == 2 && predicated.counts.shape.tiles == decltype(predicated.counts.shape.tiles){ 0, 6, 0, 0 }));
    UNSCOPED_INFO("the bracket form has no shape");
    REQUIRE(plain.counts.shape.predicated == 0);
#endif
    for (std::size_t i = 0; i < 6; ++i)
    {
        UNSCOPED_INFO(std::format("call {} leaves the memory of the bracket form", i));
        REQUIRE(predicated.devices[i] == plain.devices[i]);
    }
    UNSCOPED_INFO("the calls move the pool words");
    REQUIRE(predicated.devices.back() != predicated.devices.front());
    UNSCOPED_INFO("the assembler takes the same resolves");
    REQUIRE((predicated.submitted.size() == 6 && predicated.submitted == plain.submitted));
    UNSCOPED_INFO("the ledger outcome is the same, without a failure");
    REQUIRE((predicated.failures == plain.failures && predicated.failures.empty()));
    // The tile of each resolve is identified by the mask the entry observer read: 3 << 2i.
    const uint32_t tiles[] = { 0, 0, 0, 2, 2, 2 };
    for (std::size_t i = 0; i < 6; ++i)
    {
        UNSCOPED_INFO("the predicated target carries the tile mask: " + predicated.masks[i]);
        REQUIRE(predicated.masks[i] == std::format("predicated 1 mask {:X}", 3U << (2 * tiles[i])));
        UNSCOPED_INFO("the bracket target carries none: " + plain.masks[i]);
        REQUIRE(plain.masks[i] == "predicated 0 mask 0");
    }
}

// Nested Resolve falls back and abandons its outer ledger record.
void testConsumeResolveNested()
{
    using namespace rerevved::gpu::guest_d3d;
    ConsumeRig rig;
    REXCVAR_SET(native_consume_resolve, true);
    constexpr uint32_t kInnerSp = 0x6FFFF000;
    UNSCOPED_INFO("the outer call keeps Original");
    REQUIRE(!rig.Enter(ConsumeRig::kSp, true));
    DEV_DO(TakeResolveConsumeCounts());
    UNSCOPED_INFO("the inner call keeps Original");
    REQUIRE(!rig.Enter(kInnerSp, false));
    UNSCOPED_INFO("nested counted");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::Nested));
    rig.Finish(kInnerSp);
    rig.Finish(ConsumeRig::kSp);
    // Both returned: the stack is empty again, so the next call is judged on its own.
    UNSCOPED_INFO("the call after a nested pair is consumed");
    REQUIRE(rig.Enter(ConsumeRig::kSp, false));
    DEV_DO(TakeResolveConsumeCounts());
    rig.service.Shutdown();
}

// Resolve consume stays inactive until ring-stub publication; then it counts.
void testConsumeResolveNeedsPublishedStubs()
{
    using namespace rerevved::gpu::guest_d3d;
    ConsumeRig rig;
    REXCVAR_SET(native_consume_resolve, true);
    constexpr uint32_t kInnerSp = 0x6FFFF000;
    ringStubsActive.store(false);
    UNSCOPED_INFO("unpublished stubs keep Original");
    REQUIRE(!rig.Enter(ConsumeRig::kSp, false));
    {
#if REREVVED_DEVELOPER
        const auto counts = TakeResolveConsumeCounts();
        UNSCOPED_INFO("an unpublished consume counts nothing");
        REQUIRE((counts.consumed == 0 && counts.fallback == std::array<uint64_t, kResolveFallbackCount>{}));
#endif
        UNSCOPED_INFO("an unpublished consume writes nothing");
        REQUIRE(rig.Untouched());
    }
    ringStubsActive.store(true);
    // A withdrawal that lands between the cvar test and the lease: Original runs,
    // nothing is counted and no guest byte changes.
    SetRingStubLeaseProbe([]() noexcept
                          {
                              ringStubsActive.store(false, std::memory_order_seq_cst);
                          });
    UNSCOPED_INFO("a withdrawal before the lease keeps Original");
    REQUIRE(!rig.Enter(ConsumeRig::kSp, false));
    SetRingStubLeaseProbe(nullptr);
    ringStubsActive.store(true);
    {
#if REREVVED_DEVELOPER
        const auto counts = TakeResolveConsumeCounts();
        UNSCOPED_INFO("a withdrawal before the lease counts nothing");
        REQUIRE((counts.consumed == 0 && counts.fallback == std::array<uint64_t, kResolveFallbackCount>{}));
#endif
        UNSCOPED_INFO("a withdrawal before the lease writes nothing");
        REQUIRE(rig.Untouched());
    }
    rig.Finish(ConsumeRig::kSp);
    UNSCOPED_INFO("the call after an unpublished one is consumed, not nested");
    REQUIRE(rig.Enter(kInnerSp, false));
    DEV_DO(TakeResolveConsumeCounts());
    rig.service.Shutdown();
}

// Without an assembler, the observer settles the error and Original runs.
void testConsumeResolveWithoutAssembler()
{
    using namespace rerevved::gpu::guest_d3d;
    ConsumeRig rig(false);
    REXCVAR_SET(native_consume_resolve, true);
    UNSCOPED_INFO("no assembler keeps Original");
    REQUIRE(!rig.Enter(ConsumeRig::kSp, false));
    rig.Finish(ConsumeRig::kSp);
    UNSCOPED_INFO("no assembler counted as ledger");
    REQUIRE(ConsumeRig::OnlyFallback(ResolveFallback::Ledger));
    rig.service.Shutdown();
}

// native_consume_clear matches Original; only modeled exit bytes change.

std::string clearKey(const rerevved::gpu::guest_d3d::GuestClear& clear)
{
    const auto& c = clear.clear;
    return std::format("surface {:08X} {}x{}x{} edram {:03X} flags {:08X} index {} predicate {} tiles {} rect {},{},{},{} color {} depth {} stencil {} targets {} values {:g},{:g},{:g},{:g} depthv {:g} stencilv {}",
                       clear.target.surface,
                       clear.target.native.width,
                       clear.target.native.height,
                       clear.target.native.sampleCount,
                       clear.target.native.edramBase,
                       clear.flags,
                       clear.index,
                       clear.predicate,
                       clear.tiling.Active() ? clear.tiling.count : 0,
                       c.rectangle.left,
                       c.rectangle.top,
                       c.rectangle.right,
                       c.rectangle.bottom,
                       c.color,
                       c.depth,
                       c.stencil,
                       c.colorTargets,
                       c.colorValue[0],
                       c.colorValue[1],
                       c.colorValue[2],
                       c.colorValue[3],
                       c.depthValue,
                       c.stencilValue);
}

uint64_t FakeRenderSink::Clear(const rerevved::gpu::guest_d3d::GuestClear& clear, std::string& error)
{
    if (!acceptClears)
    {
        error = "fixture_clear_unexpected";
        return 0;
    }
    clears.push_back(clearKey(clear));
    return clears.size();
}

// The runtime, device, ledger and ring stubs of one Clear scenario: the probe
// fixture's 1280x720 color clear of slot 0 on an untiled device, with ring room
// and a pool whose fixed buffer ends at 0x00204000.
struct ClearConsumeRig
{
    rex::Runtime                     runtime{ std::filesystem::path{} };
    rex::memory::Memory*             memory    = nullptr;
    std::shared_ptr<FakeRenderSink>  assembler = std::make_shared<FakeRenderSink>();
    FailureLog                       log;
    GuestVideoServices               service;
    std::optional<ClearProbeFixture> fixture;
    uint32_t                         poolObject = 0;
    std::vector<uint8_t>             before;
    std::optional<uint32_t>          limit; // the thread's stack limit for the next Enter, by default 0x400 below r1

    explicit ClearConsumeRig(bool publishAssembler = true, bool nullPool = false)
    {
        constexpr uint32_t kImageAllocation = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
        constexpr uint32_t kImageWritable   = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
        setupRuntime(runtime);
        memory      = runtime.memory();
        auto* image = memory->LookupHeap(0x82F10000);
        UNSCOPED_INFO("the page of the strip word is committed");
        REQUIRE((image && image->AllocFixed(0x82F10000, 0x10000, 0x10000, kImageAllocation, kImageWritable)));
        storeGuest(memory, rerevved::gpu::guest_d3d::kClearStripWord, 1);
        assembler->acceptClears = true;
        fixture.emplace(memory);
        if (publishAssembler)
            rerevved::gpu::completion::PublishGuestRenderAssembler(assembler);
        auto options     = quietOptions();
        options.drawMode = rerevved::gpu::guest_d3d::NativeDrawMode::Census;
        log.Attach(options);
        UNSCOPED_INFO("clear service start must succeed");
        REQUIRE(service.Start(runtime, options) == X_STATUS_SUCCESS);
        const uint32_t g = G();
        poolObject       = allocatePhysical(memory);
        // The flush groups' entry bits, the ring cursor and limits, the pool and the target count.
        storeGuest(memory, g + 0x10, 0xFC002000);
        storeGuest(memory, g + 0x14, 0x4003FFFF);
        storeGuest(memory, g + 0x18, 0x80700000);
        storeGuest(memory, g + 0x1C, 0x00000007);
        storeGuest(memory, g + 0x20, 0x40000020);
        storeGuest(memory, g + 0x24, 0x00000004);
        storeGuest(memory, g + 0x2ABC, 0x00000F00);
        ResetPool();
        storeGuest(memory, g + 0x3A58, kPoolThreshold);
        storeGuest(memory, g + 0x31CC, kRingTargets);
        storeGuest(memory, g + 0x34CC, poolObject);
        storeGuest(memory, poolObject + 152, kPoolBase);
        storeGuest(memory, poolObject + 156, kPoolSize);
        if (nullPool)
            UseNullPool();
        rerevved::gpu::guest_d3d::ringStubsActive.store(true);
        rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(g);
        DEV_DO(rerevved::gpu::guest_d3d::TakeClearConsumeCounts());
        before = dumpGuest(memory, g, 0x10000);
    }

    // The world's allocator: no pool object, so 0x8269B618 takes forward from G+0x34 + 60 inside a side buffer
    // area ending at G+0x3A3C. The progress word is read through G+0x2A90, here the pool object's page, whose
    // word +4 is zero: segment 0 against progress 0, so no take waits.
    void UseNullPool()
    {
        storeGuest(memory, G() + 0x34CC, 0);
        storeGuest(memory, G() + 0x3A3C, kNullSideEnd);
        storeGuest(memory, G() + 0x2A90, poolObject);
    }

    // The device as BeginTiling leaves it for two tiles of a 1280x720 screen: bit 0x20 of G+0x2ABC with the saved
    // slot equal to the bound surface, the bracket set, every tile selected. Tile 1 moves by y 380, so the takes of
    // its strips differ from the untiled call's single one.
    void MakeTiled()
    {
        const uint32_t g = G();
        storeGuest(memory, g + 0x2ABC, 0x20000F00);
        storeGuest(memory, g + 0x31B8, load32(g + 0x3098));
        storeGuest(memory, g + 0x31A4, 0xF);
        storeGuest(memory, g + 0x31CC, 2);
        storeGuest(memory, g + 0x337C, 1280);
        storeGuest(memory, g + 0x3380, 720);
        storeGuest(memory, g + 0x33B0, 1);
        storeGuest(memory, g + 0x33B4, 1); // the null pool allocator's bracket check passes: the tile group waits on a later segment
        const uint32_t rects[2][4] = { { 0, 0, 1280, 384 }, { 0, 384, 1280, 720 } };
        for (uint32_t i = 0; i < 2; ++i)
            for (uint32_t j = 0; j < 4; ++j)
                storeGuest(memory, g + 0x31D0 + 16 * i + 4 * j, rects[i][j]);
        storeGuest(memory, g + 0x32C4 + 8, 380);
    }

    uint32_t load32(uint32_t address) const
    {
        return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
    }

    ~ClearConsumeRig()
    {
        rerevved::gpu::guest_d3d::ringStubsActive.store(false);
        rerevved::gpu::guest_d3d::ringStubsValidatedDevice.store(0);
        rerevved::gpu::completion::PublishGuestRenderAssembler({});
        REXCVAR_SET(native_consume_clear, false);
        REXCVAR_SET(native_consume_clear_tiled, false);
        DEV_CVAR_SET(native_clear_derive_check, false);
        DEV_CVAR_SET(native_clear_effect_check, false);
    }

    uint32_t G() const
    {
        return fixture->device;
    }

    uint32_t Word(uint32_t offset) const
    {
        return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(G() + offset));
    }

    // The ring cursor, limits and pool as the entry state of an admitted call.
    void ResetPool()
    {
        const uint32_t g = G();
        storeGuest(memory, g + 0x30, kRingCursor);
        storeGuest(memory, g + 0x34, kRingLimit34);
        storeGuest(memory, g + 0x38, kRingLimit38);
        storeGuest(memory, g + 0x3A44, 0);
        storeGuest(memory, g + 0x3A54, kPoolUsed);
    }

    // The entry observer, then the consume, for a call entered with r1 = sp.
    bool Enter(uint32_t sp, bool diagnostics)
    {
        fixture->Stack(sp, limit);
        fixture->Enter();
        return rerevved::gpu::completion::detail::ConsumeGuestClear(fixture->ctx, diagnostics);
    }

    // What Original does for a call that was not consumed: the hook on 0x826A1D90 and the exit hook.
    void Finish(uint32_t sp)
    {
        rerevved::gpu::completion::detail::RecordGuestClearReturn(sp - rerevved::gpu::guest_d3d::kClearFrameSize);
    }

    // True when the counts since the last take are one fallback of this reason and no consume.
    static bool OnlyFallback([[maybe_unused]] rerevved::gpu::guest_d3d::ClearFallback reason)
    {
#if REREVVED_DEVELOPER
        const auto                                                          counts = rerevved::gpu::guest_d3d::TakeClearConsumeCounts();
        std::array<uint64_t, rerevved::gpu::guest_d3d::kClearFallbackCount> want{};
        want[static_cast<std::size_t>(reason)] = 1;
        return counts.consumed == 0 && counts.fallback == want;
#else
        return true;
#endif
    }

    bool Untouched() const
    {
        return dumpGuest(memory, G(), 0x10000) == before;
    }

    // Puts the device page back as the rig found it, after a scenario wrote into it.
    void Restore()
    {
        std::memcpy(memory->TranslateVirtual<uint8_t*>(G()), before.data(), before.size());
    }

    rerevved::gpu::guest_d3d::GuestWordReader Reader() const
    {
        return [this](uint32_t address, uint32_t& value)
        {
            value = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
            return true;
        };
    }

    rerevved::gpu::guest_d3d::ClearEntry Entry() const
    {
        const auto& ctx = fixture->ctx;
        return { ctx.r1.u32, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.f1.f64, ctx.r8.u32, ctx.r13.u32 };
    }
};

struct ClearOutcome
{
    std::vector<std::string>            clears; // every clear the assembler took
    std::vector<std::string>            failures;
    std::vector<uint8_t>                deviceBefore, deviceAfter;
    uint64_t                            spansReleased = 0, partialSpans = 0;
    std::string                         unowned;
    bool                                consumed = false;
    rerevved::gpu::guest_d3d::ClearPlan plan;
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ClearConsumeCounts counts;
#endif
    uint32_t device = 0;
};

// The fixture call as Original runs it (the entry observer, the hook on
// 0x826A1D90 with the guest's own arguments, the exit hook) or as the consume
// replays it.
ClearOutcome runClear(bool consume, bool nullPool, bool tiled = false)
{
    using namespace rerevved::gpu::guest_d3d;
    ClearConsumeRig rig(true, nullPool);
    ClearOutcome    out;
    if (tiled)
        rig.MakeTiled();
    out.device        = rig.G();
    out.deviceBefore  = dumpGuest(rig.memory, rig.G(), 0x10000);
    const uint32_t sp = rig.fixture->sp;
    if (consume)
    {
        REXCVAR_SET(native_consume_clear, true);
        REXCVAR_SET(native_consume_clear_tiled, tiled);
        const auto verdict = ClearConsumeVerdict({ true, false, true, true, false, tiled }, rig.Reader(), rig.Entry(), &out.plan);
        UNSCOPED_INFO(std::string("the fixture call is admitted, got ") + (verdict ? std::string(kClearFallbackNames[static_cast<std::size_t>(*verdict)]) : std::string("none")));
        REQUIRE(verdict == std::nullopt);
        out.consumed = rig.Enter(sp, false);
    }
    else
    {
        rig.fixture->Enter();
        rig.fixture->Call(rig.G(), 1280);
        rig.Finish(sp);
    }
    DEV_DO(out.counts = TakeClearConsumeCounts());
    out.deviceAfter   = dumpGuest(rig.memory, rig.G(), 0x10000);
    const auto ledger = rig.service.Ledger()->Snapshot();
    out.spansReleased = ledger.spansReleased;
    out.partialSpans  = ledger.partialSpans;
    out.unowned       = ledger.unowned;
    out.failures      = rig.log.Details();
    out.clears        = rig.assembler->clears;
    UNSCOPED_INFO("clear service must stop");
    REQUIRE(rig.service.Shutdown());
    return out;
}

void testConsumeClearReplay(bool nullPool, bool tiled = false)
{
    using namespace rerevved::gpu::guest_d3d;
    const auto original = runClear(false, nullPool, tiled);
    UNSCOPED_INFO("the original path makes one clear without a failure");
    REQUIRE((original.failures.empty() && original.clears.size() == 1));

    const auto replayed = runClear(true, nullPool, tiled);
    UNSCOPED_INFO("the fixture call is consumed");
    REQUIRE(replayed.consumed);
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("one consume, no fallback");
    REQUIRE((replayed.counts.consumed == 1 && replayed.counts.fallback == std::array<uint64_t, kClearFallbackCount>{}));
#endif
    UNSCOPED_INFO("the fixture call makes one per-target clear");
    REQUIRE(replayed.plan.calls.count == 1);

    // Identical assembler input and ledger outcome.
    UNSCOPED_INFO("the assembler takes the same clear: " + (replayed.clears.empty() ? std::string("none") : replayed.clears[0]) + " vs " + original.clears[0]);
    REQUIRE(replayed.clears == original.clears);
    UNSCOPED_INFO("the same ledger outcome");
    REQUIRE((replayed.spansReleased == original.spansReleased && replayed.partialSpans == original.partialSpans && replayed.unowned == original.unowned && replayed.failures == original.failures));

    // Memory: exactly the plan's stores changed, nowhere else.
    auto want = replayed.deviceBefore;
    for (const auto& write : ClearExitWrites(replayed.device, replayed.plan.watch, replayed.plan.exit))
    {
        const auto offset = write.address - replayed.device;
        UNSCOPED_INFO("every store is inside the device block");
        REQUIRE(offset < want.size());
        if (write.byte)
            want[offset] = static_cast<uint8_t>(write.value);
        else
            rex::memory::store_and_swap<uint32_t>(want.data() + offset, write.value);
    }
    UNSCOPED_INFO("the device changed by exactly the exit state");
    REQUIRE(replayed.deviceAfter == want);
    UNSCOPED_INFO("the consume stored something");
    REQUIRE(replayed.deviceAfter != replayed.deviceBefore);
    UNSCOPED_INFO("the harness original path stores nothing, so the consume stores are the only change");
    REQUIRE(original.deviceAfter == original.deviceBefore);

    // By hand for the fixture numbers: one 84 byte side buffer, or for the tiled device the three takes of its two tiles
    // (84, then the 168 and 84 of tile 1, whose rectangle the origin offset moves off the strip grid).
    const uint32_t taken = tiled ? 84 + 168 + 84 : 84;
    const auto     word  = [&](uint32_t offset)
    {
        return rex::memory::load_and_swap<uint32_t>(replayed.deviceAfter.data() + offset);
    };
    if (nullPool)
    {
        // From G+0x34 + 60 forward: the end is stored, neither limit moves.
        UNSCOPED_INFO("the null pool moved G+0x3A44 and G+0x3A54 only");
        REQUIRE((word(0x34) == kRingLimit34 && word(0x38) == kRingLimit38 && word(0x3A44) == kRingLimit34 + 60 + taken && word(0x3A54) == kPoolUsed + taken));
    }
    else
    {
        const uint32_t kBuffer = kPoolBase + kPoolSize - taken;
        UNSCOPED_INFO("the pool words moved by one 84 byte side buffer");
        REQUIRE((word(0x34) == kRingLimit34 - taken && word(0x38) == kRingLimit38 - taken && word(0x3A44) == kBuffer && word(0x3A54) == kPoolUsed + taken));
    }
    UNSCOPED_INFO("the ring cursor and the retarget words are not modeled");
    REQUIRE((word(0x30) == kRingCursor && word(0x3A48) == 0 && word(0x3A4C) == 0));
    UNSCOPED_INFO("the byte G+0x2ABD is 4 and its neighbors are kept");
    REQUIRE((replayed.deviceAfter[0x2ABD] == 0x04 && replayed.deviceAfter[0x2ABC] == (tiled ? 0x20 : 0) && replayed.deviceAfter[0x2ABE] == 0x0F));
}

REX_EXTERN(sub_826A2970);

// The Clear census wrapper itself. An idle wrapper consumes and runs no Original.
// A developer build keeps exactly one Original, with no consume, while a harvest,
// scene capture or route trace is active; the shipping build has no recorder to
// wait for and the wrapper's gate is the constexpr-false DeveloperRecorderActive().
void testClearWrapperDiagnosticGate()
{
    using namespace rerevved::gpu;
    enum class Subscriber
    {
        None,
        Harvest,
        Scene,
        Route
    };

    struct Outcome
    {
        unsigned originals = 0;
        bool     untouched = false;
        uint64_t consumed = 0, diagnostics = 0;
    };

    const auto run = []([[maybe_unused]] Subscriber subscriber)
    {
#if REREVVED_DEVELOPER
        struct Restore
        {
            ~Restore()
            {
                diagnostics::shaderHarvestObserver.store(nullptr);
                diagnostics::sceneCaptureSession.store(nullptr);
                diagnostics::routeTraceEnabled.store(0);
            }
        } restore;
#endif
        ClearConsumeRig                                      rig;
        Outcome                                              out;
        rerevved::tests::guest_imports::ScopedFallbackImport original([&](uint32_t, PPCContext&, uint8_t*)
                                                                      {
                                                                          ++out.originals;
                                                                      });
        REXCVAR_SET(native_consume_clear, true);
#if REREVVED_DEVELOPER
        switch (subscriber)
        {
            case Subscriber::None:
                break;
            case Subscriber::Harvest:
                diagnostics::shaderHarvestObserver.store([](const diagnostics::ShaderHarvestCall&) noexcept
                                                         {
                                                         });
                break;
            case Subscriber::Scene:
                diagnostics::sceneCaptureSession.store(&out);
                break;
            case Subscriber::Route:
                diagnostics::routeTraceEnabled.store(1);
                break;
        }
#endif
        const uint32_t sp = rig.fixture->sp;
        rig.fixture->Stack(sp, rig.limit);
        sub_826A2970(rig.fixture->ctx, rig.memory->virtual_membase());
        if (out.originals)
            rig.Finish(sp);
        out.untouched = rig.Untouched();
#if REREVVED_DEVELOPER
        const auto counts = guest_d3d::TakeClearConsumeCounts();
        out.consumed      = counts.consumed;
        out.diagnostics   = counts.fallback[static_cast<std::size_t>(guest_d3d::ClearFallback::Diagnostics)];
#else
        // The call stored its exit state, or Original ran instead.
        out.consumed    = out.originals == 0 && !out.untouched;
        out.diagnostics = out.originals;
#endif
        return out;
    };

    const auto idle = run(Subscriber::None);
    UNSCOPED_INFO("with no subscriber the wrapper consumes and runs no Original");
    REQUIRE((idle.consumed == 1 && idle.originals == 0 && idle.diagnostics == 0 && !idle.untouched));
#if REREVVED_DEVELOPER
    for (const auto subscriber : { Subscriber::Harvest, Subscriber::Scene, Subscriber::Route })
    {
        const auto held = run(subscriber);
        UNSCOPED_INFO("a harvest, scene capture or route trace gets exactly one Original and no consume");
        REQUIRE((held.consumed == 0 && held.originals == 1 && held.diagnostics == 1 && held.untouched));
    }
#endif
}

// Fallback leaves guest bytes and the probe stack unchanged.
void testConsumeClearFallbacks()
{
    using namespace rerevved::gpu::guest_d3d;
    ClearConsumeRig rig;
    auto&           ctx    = rig.fixture->ctx;
    auto*           memory = rig.memory;
    const uint32_t  kSp    = rig.fixture->sp;

    // The cvar off consumes nothing and counts nothing, even for an admitted call.
    REXCVAR_SET(native_consume_clear, false);
    UNSCOPED_INFO("the cvar off consumes nothing");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    {
#if REREVVED_DEVELOPER
        const auto counts = TakeClearConsumeCounts();
        UNSCOPED_INFO("the cvar off counts nothing");
        REQUIRE((counts.consumed == 0 && counts.fallback == std::array<uint64_t, kClearFallbackCount>{}));
#endif
        UNSCOPED_INFO("the cvar off writes nothing");
        REQUIRE(rig.Untouched());
    }

    REXCVAR_SET(native_consume_clear, true);
    // Diagnostics need the body and its midasm hooks.
    UNSCOPED_INFO("a diagnostic keeps Original");
    REQUIRE(!rig.Enter(kSp, true));
    rig.Finish(kSp);
    UNSCOPED_INFO("diagnostics counted");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::Diagnostics));
#if REREVVED_DEVELOPER
    // The probes read the body's own stores: a consume must not run beside them.
    REXCVAR_SET(native_clear_effect_check, true);
    UNSCOPED_INFO("the effect probe keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    REXCVAR_SET(native_clear_effect_check, false);
    TakeClearEffectCounts();
    TakeClearExitCounts();
    TakeClearDeriveCounts();
    UNSCOPED_INFO("the effect probe counts diagnostics");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::Diagnostics));
    REXCVAR_SET(native_clear_derive_check, true);
    UNSCOPED_INFO("the derive probe keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    REXCVAR_SET(native_clear_derive_check, false);
    TakeClearDeriveCounts();
    UNSCOPED_INFO("the derive probe counts diagnostics");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::Diagnostics));
#endif

    // Ring stubs not ready for this device.
    ringStubsValidatedDevice.store(rig.G() + 0x10000);
    UNSCOPED_INFO("stubs not ready keep Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("stubs counted");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::Stubs));
    ringStubsValidatedDevice.store(rig.G());

    // The thread's stack reach [limit, sp) overlaps the device words: Original's saves would land in words the
    // model reads. Tested after the state and before the tiling, with nothing written; the edges are exact.
    {
        const uint32_t g = rig.G();
        for (const uint32_t sp : { g + 0x40, g + 1, g + 0x3A5C + 0x400 - 1 })
        {
            UNSCOPED_INFO("a stack over the device keeps Original");
            REQUIRE(!rig.Enter(sp, false));
            rig.Finish(sp);
            UNSCOPED_INFO("the stack counted");
            REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::StackOverlap));
        }
        UNSCOPED_INFO("a refused stack writes nothing");
        REQUIRE(rig.Untouched());
        storeGuest(memory, g + 0x2ABC, 0x10000F00);
        UNSCOPED_INFO("a stack over a tiled device keeps Original");
        REQUIRE(!rig.Enter(g + 0x40, false));
        rig.Finish(g + 0x40);
        UNSCOPED_INFO("the stack is reported before the tiling");
        REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::StackOverlap));
        storeGuest(memory, g + 0x2ABC, 0x00000F00);
        ringStubsValidatedDevice.store(g + 0x10000);
        UNSCOPED_INFO("stubs not ready keep Original");
        REQUIRE(!rig.Enter(g + 0x40, false));
        rig.Finish(g + 0x40);
        UNSCOPED_INFO("the stubs are reported before the stack");
        REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::Stubs));
        ringStubsValidatedDevice.store(g);

        auto                    entry = rig.Entry();
        const ClearConsumeState state{ true, false, true, true, false, false };
        const auto              verdictAt = [&](uint32_t limit, uint32_t sp)
        {
            rig.fixture->Stack(sp, limit);
            entry = rig.Entry();
            return ClearConsumeVerdict(state, rig.Reader(), entry);
        };
        UNSCOPED_INFO("a reach ending exactly at G is admitted");
        REQUIRE(verdictAt(g - 0x400, g) == std::nullopt);
        UNSCOPED_INFO("a reach ending one byte into the device words is refused");
        REQUIRE(verdictAt(g - 0x400, g + 1) == ClearFallback::StackOverlap);
        UNSCOPED_INFO("a reach starting exactly at the end of the device words is admitted");
        REQUIRE(verdictAt(g + 0x3A5C, g + 0x3A5C + 0x400) == std::nullopt);
        UNSCOPED_INFO("a reach starting on the last device byte is refused");
        REQUIRE(verdictAt(g + 0x3A5C - 1, g + 0x3A5C + 0x400) == ClearFallback::StackOverlap);
        // The no-overflow domain: the entry function's own frame (112 bytes, g216:12901) must fit between the limit
        // and r1, wherever the stack lies.
        UNSCOPED_INFO("exactly the frame between the limit and r1 is admitted");
        REQUIRE(verdictAt(kSp - 112, kSp) == std::nullopt);
        UNSCOPED_INFO("one byte short of the frame is refused");
        REQUIRE(verdictAt(kSp - 111, kSp) == ClearFallback::StackOverlap);
        UNSCOPED_INFO("r1 at the limit is refused");
        REQUIRE(verdictAt(kSp, kSp) == ClearFallback::StackOverlap);

        // The pool object G+0x34CC names, put in the frame at r1 - 160 where Original's link register save lands.
        {
            const uint32_t sp      = rig.fixture->sp;
            const uint32_t pool    = sp - 160;
            const uint32_t oldPool = rig.Word(0x34CC);
            storeGuest(memory, pool + 152, rig.load32(oldPool + 152));
            storeGuest(memory, pool + 156, rig.load32(oldPool + 156));
            storeGuest(memory, g + 0x34CC, pool);
            UNSCOPED_INFO("a pool object in the frame is refused");
            REQUIRE(verdictAt(sp - 0x400, sp) == ClearFallback::StackOverlap);
            storeGuest(memory, g + 0x34CC, oldPool);
            UNSCOPED_INFO("the same stack with the pool object elsewhere is admitted");
            REQUIRE(verdictAt(sp - 0x400, sp) == std::nullopt);
        }
        rig.fixture->Stack(rig.fixture->sp);
        rig.Restore();
    }

    // The tiled device, the bracket, the marker and the registers the derivation reads.
    storeGuest(memory, rig.G() + 0x2ABC, 0x10000F00);
    UNSCOPED_INFO("a tiled device keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("tiling counted");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::Tiled));
    storeGuest(memory, rig.G() + 0x2ABC, 0x00000F00);
    storeGuest(memory, rig.G() + 0x33B0, 1);
    UNSCOPED_INFO("the bracket keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the bracket counted as tiled");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::Tiled));
    storeGuest(memory, rig.G() + 0x33B0, 0);
    // A null rectangle with no bound target has nothing to clear.
    const uint32_t boundTarget = rig.Word(0x3098);
    storeGuest(memory, rig.G() + 0x3098, 0);
    UNSCOPED_INFO("an unsupported rectangle keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the unsupported rectangle counted");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::DeriveUnsupported));
    storeGuest(memory, rig.G() + 0x3098, boundTarget);
    storeGuest(memory, rig.G() + 0x2ABC, 0x00200F00);
    UNSCOPED_INFO("the allocation marker keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the allocation marker counted");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::AllocMarker));
    storeGuest(memory, rig.G() + 0x2ABC, 0x00000F00);

    // The ring and the allocator: Original might flush, wait for ring space or
    // fail its allocation, which the exit state cannot predict.
    storeGuest(memory, rig.G() + 0x30, kRingLimit38 - 16);
    UNSCOPED_INFO("a ring that might flush keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the near-limit ring counted");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::RingSpace));
    rig.ResetPool();
    storeGuest(memory, rig.G() + 0x3A54, kPoolThreshold);
    UNSCOPED_INFO("a side buffer count past the threshold keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the threshold counted as ring space");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::RingSpace));
    rig.ResetPool();
    storeGuest(memory, rig.G() + 0x3A44, kRingCursor + 64);
    UNSCOPED_INFO("an exhausted pool keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the exhausted pool counted");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::AllocCapacity));
    rig.ResetPool();
    storeGuest(memory, rig.poolObject + 152, 0);
    UNSCOPED_INFO("a pool object without a fixed buffer keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the indirect allocator counted");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::AllocPath));
    storeGuest(memory, rig.poolObject + 152, kPoolBase);
    // A null pool object is the allocator 0x8269B618, which reads the GPU progress word through G+0x2A90,
    // null in this fixture.
    storeGuest(memory, rig.G() + 0x34CC, 0);
    UNSCOPED_INFO("a null pool object without a progress block keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the missing progress block counted");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::Unreadable));
    // With the block but no side buffer area (G+0x3A3C and G+0x3A50 are zero) the allocator fails.
    storeGuest(memory, rig.G() + 0x2A90, rig.poolObject);
    UNSCOPED_INFO("a null pool allocator with no room keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the failing null pool allocator counted");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::AllocCapacity));
    // With room, a GPU progress word in another segment makes it wait.
    storeGuest(memory, rig.G() + 0x3A3C, kNullSideEnd);
    storeGuest(memory, rig.poolObject + 4, 3);
    UNSCOPED_INFO("a null pool allocator that would wait keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the waiting null pool allocator counted");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::AllocWait));
    storeGuest(memory, rig.poolObject + 4, 0);
    // Forced flush: the count past the threshold with a null pool object.
    storeGuest(memory, rig.G() + 0x3A54, kPoolThreshold);
    UNSCOPED_INFO("a null pool count past the threshold keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the null pool threshold counted as ring space");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::RingSpace));
    rig.ResetPool();
    storeGuest(memory, rig.G() + 0x3A3C, 0);
    storeGuest(memory, rig.G() + 0x2A90, 0);
    storeGuest(memory, rig.G() + 0x34CC, rig.poolObject);

    // The fallbacks wrote nothing, and made no clear: the assembler is fed by Original's hook only.
    UNSCOPED_INFO("no fallback wrote guest memory");
    REQUIRE(rig.Untouched());
    UNSCOPED_INFO("no fallback made a clear");
    REQUIRE(rig.assembler->clears.empty());

    // After the fallbacks the probe stack is clean: the admitted calls are consumed.
    UNSCOPED_INFO("the admitted call is consumed after the fallbacks");
    REQUIRE(rig.Enter(kSp, false));
    rig.ResetPool();
    UNSCOPED_INFO("a consumed call leaves no probe behind");
    REQUIRE(rig.Enter(kSp, false));
    rig.ResetPool();
    // A deeper call right after a consume is not nested: the consume popped its placeholder.
    UNSCOPED_INFO("a consumed call leaves no probe for a deeper call");
    REQUIRE(rig.Enter(kSp - 0x1000, false));
    {
#if REREVVED_DEVELOPER
        const auto counts = TakeClearConsumeCounts();
        UNSCOPED_INFO("three consumes");
        REQUIRE((counts.consumed == 3 && counts.fallback == std::array<uint64_t, kClearFallbackCount>{}));
#endif
    }
    UNSCOPED_INFO("each consume made its clear");
    REQUIRE(rig.assembler->clears.size() == 3);
    UNSCOPED_INFO("the ledger saw no failure");
    REQUIRE(rig.log.Details().empty());

#if REREVVED_DEVELOPER
    // A consumed call never feeds the probes: they stay at zero.
    {
        const auto effects = TakeClearEffectCounts();
        const auto derive  = TakeClearDeriveCounts();
        const auto exits   = TakeClearExitCounts();
        UNSCOPED_INFO("a consumed call counts nothing in the probes");
        REQUIRE((effects.exactOk == 0 && effects.unsupported == 0 && effects.unpaired == 0 && effects.earlyOk == 0 && derive.ok == 0 && derive.unpaired == 0 && exits.ok == 0));
    }
#endif

    // The next call after a consume returns at once through the same entry hooks and sees an empty stack.
    rig.ResetPool();
    UNSCOPED_INFO("the stack is empty after the consumes");
    REQUIRE(rig.Enter(kSp, false));
    DEV_DO(TakeClearConsumeCounts());

    // A call the entry observer did not stack: no placeholder.
    rig.ResetPool();
    UNSCOPED_INFO("a call without an entry observation keeps Original");
    REQUIRE(!rerevved::gpu::completion::detail::ConsumeGuestClear(ctx, false));
    UNSCOPED_INFO("entry state counted");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::EntryState));
    UNSCOPED_INFO("fallback service must stop");
    REQUIRE(rig.service.Shutdown());
}

// native_consume_clear_tiled gates tiled clear; block-A keeps Original reason.
void testConsumeClearTiledFallbacks()
{
    using namespace rerevved::gpu::guest_d3d;
    ClearConsumeRig rig;
    rig.MakeTiled();
    const uint32_t kSp = rig.fixture->sp;
    const auto     fed = dumpGuest(rig.memory, rig.G(), 0x10000);

    REXCVAR_SET(native_consume_clear, true);
    REXCVAR_SET(native_consume_clear_tiled, false);
    UNSCOPED_INFO("a tiled device keeps Original without the tiled option");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("tiling counted");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::Tiled));
    UNSCOPED_INFO("the fallback writes nothing");
    REQUIRE(dumpGuest(rig.memory, rig.G(), 0x10000) == fed);

    // Block A: G+0x2ABF bit 0x20, G+0x2ABC bit 0x40 and a mask bit of 0x15555555.
    REXCVAR_SET(native_consume_clear_tiled, true);
    storeGuest(rig.memory, rig.G() + 0x2ABC, 0x60000F20);
    UNSCOPED_INFO("the tile block A keeps Original");
    REQUIRE(!rig.Enter(kSp, false));
    rig.Finish(kSp);
    UNSCOPED_INFO("the tile block A counted");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::TiledBlock));
    storeGuest(rig.memory, rig.G() + 0x31A4, 0xA);
    UNSCOPED_INFO("a mask outside 0x15555555 skips the block and is consumed");
    REQUIRE(rig.Enter(kSp, false));
    {
#if REREVVED_DEVELOPER
        const auto counts = TakeClearConsumeCounts();
        UNSCOPED_INFO("the tiled consume is counted apart");
        REQUIRE((counts.consumed == 1 && counts.consumedTiled == 1));
#endif
    }
    rig.service.Shutdown();
}

// A Clear entered inside another falls back as nested.
void testConsumeClearNested()
{
    using namespace rerevved::gpu::guest_d3d;
    ClearConsumeRig rig;
    REXCVAR_SET(native_consume_clear, true);
    constexpr uint32_t kInnerOffset = 0x1000;
    const uint32_t     outer        = rig.fixture->sp;
    UNSCOPED_INFO("the outer call keeps Original");
    REQUIRE(!rig.Enter(outer, true));
    DEV_DO(TakeClearConsumeCounts());
    UNSCOPED_INFO("the inner call keeps Original");
    REQUIRE(!rig.Enter(outer - kInnerOffset, false));
    UNSCOPED_INFO("nested counted");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::Nested));
    rig.Finish(outer - kInnerOffset);
    rig.Finish(outer);
    // Both returned: the stack is empty again, so the next call is judged on its own.
    UNSCOPED_INFO("the call after a nested pair is consumed");
    REQUIRE(rig.Enter(outer, false));
    DEV_DO(TakeClearConsumeCounts());
    rig.service.Shutdown();
}

// Without published ring services, Clear stacks/counts nothing; Original runs.
void testConsumeClearNeedsPublishedStubs()
{
    using namespace rerevved::gpu::guest_d3d;
    ClearConsumeRig rig;
    REXCVAR_SET(native_consume_clear, true);
    constexpr uint32_t kInnerOffset = 0x1000;
    const uint32_t     outer        = rig.fixture->sp;
    ringStubsActive.store(false);
    UNSCOPED_INFO("unpublished stubs keep Original");
    REQUIRE(!rig.Enter(outer, false));
    {
#if REREVVED_DEVELOPER
        const auto counts = TakeClearConsumeCounts();
        UNSCOPED_INFO("an unpublished consume counts nothing");
        REQUIRE((counts.consumed == 0 && counts.fallback == std::array<uint64_t, kClearFallbackCount>{}));
#endif
        UNSCOPED_INFO("an unpublished consume writes nothing");
        REQUIRE(rig.Untouched());
    }
    ringStubsActive.store(true);
    // A withdrawal that lands between the cvar test and the lease: Original runs,
    // nothing is counted and no guest byte changes.
    SetRingStubLeaseProbe([]() noexcept
                          {
                              ringStubsActive.store(false, std::memory_order_seq_cst);
                          });
    UNSCOPED_INFO("a withdrawal before the lease keeps Original");
    REQUIRE(!rig.Enter(outer, false));
    SetRingStubLeaseProbe(nullptr);
    ringStubsActive.store(true);
    {
#if REREVVED_DEVELOPER
        const auto counts = TakeClearConsumeCounts();
        UNSCOPED_INFO("a withdrawal before the lease counts nothing");
        REQUIRE((counts.consumed == 0 && counts.fallback == std::array<uint64_t, kClearFallbackCount>{}));
#endif
        UNSCOPED_INFO("a withdrawal before the lease writes nothing");
        REQUIRE(rig.Untouched());
    }
    rig.Finish(outer);
    UNSCOPED_INFO("the call after an unpublished one is consumed, not nested");
    REQUIRE(rig.Enter(outer - kInnerOffset, false));
    DEV_DO(TakeClearConsumeCounts());
    rig.service.Shutdown();
}

// No assembler: the consume leaves the call to Original.
void testConsumeClearWithoutAssembler()
{
    using namespace rerevved::gpu::guest_d3d;
    ClearConsumeRig rig(false);
    REXCVAR_SET(native_consume_clear, true);
    UNSCOPED_INFO("no assembler keeps Original");
    REQUIRE(!rig.Enter(rig.fixture->sp, false));
    rig.Finish(rig.fixture->sp);
    UNSCOPED_INFO("no assembler counted as ledger");
    REQUIRE(ClearConsumeRig::OnlyFallback(ClearFallback::Ledger));
    rig.service.Shutdown();
}

#if REREVVED_DEVELOPER
// Clear exit distinguishes modeled, pool-mismatch and stack-overlap results.
void testClearExitStackOverlap()
{
    using namespace rerevved::gpu::guest_d3d;
    ClearConsumeRig rig;
    REXCVAR_SET(native_clear_effect_check, true);
    REXCVAR_SET(native_consume_clear, true);
    TakeClearExitCounts();
    TakeClearEffectCounts();
    const auto probe = [&](uint32_t sp, const std::function<void()>& prepare = {}, uint64_t* enterReads = nullptr)
    {
        rig.fixture->frame = sp - kClearFrameSize;
        storeGuest(rig.memory, rig.fixture->frame + 92, 0x55);
        rig.ResetPool();
        if (prepare)
            prepare();
        const uint64_t before = rerevved::gpu::completion::GuestProgressReadCount();
        UNSCOPED_INFO("the effect probe keeps Original");
        REQUIRE(!rig.Enter(sp, false));
        if (enterReads)
            *enterReads = rerevved::gpu::completion::GuestProgressReadCount() - before;
        rig.fixture->Call(rig.G(), 1280);
        rig.Finish(sp);
        TakeClearEffectCounts();
        return TakeClearExitCounts();
    };
    const auto overlap = static_cast<std::size_t>(ClearExitRefusal::StackOverlap);
    {
        const auto counts = probe(rig.G() + 0x2000);
        UNSCOPED_INFO("a stack over the device block counts clear_exit_unmodeled_stack_overlap");
        REQUIRE((counts.ok == 0 && counts.unmodeled[overlap] == 1));
        UNSCOPED_INFO("the probe counter is named stack_overlap");
        REQUIRE(FormatClearExitCounts(counts).ends_with(" clear_exit_unmodeled_stack_overlap=1"));
    }
    {
        // The overlap ends the probe at the entry: the hooks read exactly the two KPCR words the stack test needs
        // (the device test reads no word) and nothing after it, while the probe of a clear stack goes on to read
        // the calls and the watch.
        uint64_t overlapping = 0, clear = 0;
        probe(rig.G() + 0x2000, {}, &overlapping);
        probe(rig.G() + 0x3A5C + 0x400, {}, &clear);
        UNSCOPED_INFO(std::format("an overlapping probe reads only the KPCR words (read {})", overlapping));
        REQUIRE(overlapping == 2);
        UNSCOPED_INFO(std::format("the probe of a clear stack reads more (read {})", clear));
        REQUIRE(clear > 2);
    }
    {
        // The overlap ends the probe: no word is read after it, so a watched word that cannot be read (a null
        // pool object without a progress block, G+0x34CC and G+0x2A90 zero) cannot mask it.
        const auto counts = probe(rig.G() + 0x2000, [&]
                                  {
                                      storeGuest(rig.memory, rig.G() + 0x34CC, 0);
                                      storeGuest(rig.memory, rig.G() + 0x2A90, 0);
                                  });
        UNSCOPED_INFO("an overlap with an unreadable watched word still counts clear_exit_unmodeled_stack_overlap");
        REQUIRE((counts.ok == 0 && counts.unmodeled[overlap] == 1));
        UNSCOPED_INFO("the same unreadable watched word without an overlap is not a stack_overlap");
        REQUIRE(probe(rig.G() + 0x3A5C + 0x400, [&]
                      {
                          storeGuest(rig.memory, rig.G() + 0x34CC, 0);
                          storeGuest(rig.memory, rig.G() + 0x2A90, 0);
                      })
                    .unmodeled[overlap] == 0);
    }
    {
        const auto counts = probe(rig.G() + 0x3A5C + 0x400);
        UNSCOPED_INFO("a stack starting at the end of the device words is modeled");
        REQUIRE(counts.unmodeled[overlap] == 0);
    }
    {
        const auto counts = probe(rig.G() + 0x3A5C + 0x400 - 1);
        UNSCOPED_INFO("a stack starting one byte inside the device words is refused");
        REQUIRE(counts.unmodeled[overlap] == 1);
    }
    TakeClearConsumeCounts();
    rig.service.Shutdown();
}

void testClearExitProbeHooks(bool nullPool, bool tiled = false)
{
    using namespace rerevved::gpu::guest_d3d;
    ClearConsumeRig rig(true, nullPool);
    if (tiled)
        rig.MakeTiled();
    REXCVAR_SET(native_clear_effect_check, true);
    REXCVAR_SET(native_consume_clear, true);
    const uint32_t sp = rig.fixture->sp;
    TakeClearExitCounts();
    TakeClearEffectCounts();

    ClearPlan plan;
    UNSCOPED_INFO("the fixture call is admitted");
    REQUIRE(ClearConsumeVerdict({ true, false, true, true, false, tiled }, rig.Reader(), rig.Entry(), &plan) == std::nullopt);
    const auto writes = ClearExitWrites(rig.G(), plan.watch, plan.exit);
    const auto run    = [&](const std::function<void()>& corrupt)
    {
        rig.ResetPool();
        storeGuest(rig.memory, rig.G() + 0x3A4C, 0);
        UNSCOPED_INFO("the effect probe keeps Original");
        REQUIRE(!rig.Enter(sp, false));
        rig.fixture->Call(rig.G(), 1280);
        for (const auto& write : writes)
        {
            if (write.byte)
                rig.memory->TranslateVirtual<uint8_t*>(write.address)[0] = static_cast<uint8_t>(write.value);
            else
                storeGuest(rig.memory, write.address, write.value);
        }
        if (corrupt)
            corrupt();
        rig.Finish(sp);
        return TakeClearExitCounts();
    };
    {
        const auto counts = run({});
        UNSCOPED_INFO("the stored exit state counts clear_exit_ok");
        REQUIRE((counts.ok == 1 && counts.mismatch == decltype(counts.mismatch){}));
        if (tiled)
        {
            // The tiled exit is counted apart, by the words that made the device tiled: the tile loop and the bracket.
            decltype(counts.shapes) shapes{};
            shapes[0] = shapes[1] = 1;
            UNSCOPED_INFO("the tiled exit counts clear_exit_tiled and clear_exit_tiled_ok with its shape");
            REQUIRE((counts.tiled == 1 && counts.tiledOk == 1 && counts.shapes == shapes));
        }
        else
        {
            UNSCOPED_INFO("an untiled exit counts no tiled field");
            REQUIRE((counts.tiled == 0 && counts.tiledOk == 0));
        }
        TakeClearEffectCounts();
    }
    {
        const auto                counts = run([&]
                                               {
                                    storeGuest(rig.memory, rig.G() + 0x3A54, kPoolUsed + 80);
                                               });
        decltype(counts.mismatch) want{};
        want[7] = 1; // pool_used
        UNSCOPED_INFO("a wrong pool count is the pool_used mismatch only");
        REQUIRE((counts.ok == 0 && counts.mismatch == want));
        UNSCOPED_INFO("a mismatching tiled exit counts clear_exit_tiled but not clear_exit_tiled_ok");
        REQUIRE((counts.tiled == (tiled ? 1U : 0U) && counts.tiledOk == 0));
        TakeClearEffectCounts();
    }
    if (nullPool)
    {
        const auto                counts = run([&]
                                               {
                                    storeGuest(rig.memory, rig.G() + 0x3A4C, 0x00204000);
                                               });
        decltype(counts.mismatch) want{};
        want[8] = 1; // wrap_pos
        UNSCOPED_INFO("a stored wrap word is the wrap_pos mismatch only");
        REQUIRE((counts.ok == 0 && counts.mismatch == want));
        TakeClearEffectCounts();
    }
    // The probe call is not consumed: nothing counted as consumed.
    const auto consume = TakeClearConsumeCounts();
    UNSCOPED_INFO("the probed calls are diagnostics fallbacks");
    REQUIRE((consume.consumed == 0 && consume.fallback[static_cast<std::size_t>(ClearFallback::Diagnostics)] == (nullPool ? 3U : 2U)));
    rig.service.Shutdown();
}
#endif

TEST_CASE("guest_gpu_service", "[ctest][guest-gpu-service][run_serial]")
{
    const ConsumeCvars consumeCvars;
    testVblankSchedule();
    testVblankJoinTimeout();
    testDebtMonitor();
    testDebtMonitorDeferredKickSequence();
#if REREVVED_DEVELOPER
    testRingConsumeWatchdogRunsInTheServiceLoop();
#endif
    testRegistrationGenerations();
    testSwapEpochs();
    testMmioAndMetadata();
    testCompletionDebt();
    testRegistrationDebt();
    testInterruptRetention();
    testFenceHookRelease();
    testFrontbufferResolveOwnership(rerevved::gpu::guest_d3d::NativeDrawMode::Skip);
    testFrontbufferResolveOwnership(rerevved::gpu::guest_d3d::NativeDrawMode::Strict);
    testResolveCaptureGuards();
    testResolveHookSourceRules();
    testEntryRefusedResolveReported();
    testCallbackInterruptAndTeardown();
    testPredicatedCallbackPair();
    testNoHandlerHoldsCallbackSpan();
    testQueuedInterruptDroppedAtClose();
    testTileReplayHooks();
    testDisplayWaitHooks();
    testAssembledSwapFailure();
    testDeviceFailureWithoutUi(rerevved::gpu::guest_d3d::NativeDrawMode::Strict);
    testDeviceFailureWithoutUi(rerevved::gpu::guest_d3d::NativeDrawMode::Census);
    testWatchedMirrorWriters();
#if REREVVED_DEVELOPER
    testClearDeriveHooks();
    testClearEffectHooks();
    testResolveEffectHooks();
#endif
    testConsumeResolveReplay();
    testConsumeResolveFallbacks();
    testConsumePredicatedSequence();
    testConsumeResolveNested();
    testConsumeResolveNeedsPublishedStubs();
    testConsumeResolveWithoutAssembler();
    testConsumeClearReplay(false);
    testConsumeClearReplay(true);
    testConsumeClearReplay(false, true);
    testConsumeClearReplay(true, true);
    testConsumeClearFallbacks();
    testClearWrapperDiagnosticGate();
    testConsumeClearNested();
    testConsumeClearNeedsPublishedStubs();
    testConsumeClearWithoutAssembler();
#if REREVVED_DEVELOPER
    testClearExitStackOverlap();
    testClearExitProbeHooks(false);
    testClearExitProbeHooks(true);
    testClearExitProbeHooks(false, true);
    testClearExitProbeHooks(true, true);
#endif
    testConsumeClearTiledFallbacks();
}
