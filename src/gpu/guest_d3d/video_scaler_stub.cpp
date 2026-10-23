#include "video_scaler_stub.h"

#include "ring_stubs.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <format>
#include <functional>
#include <thread>

REXCVAR_DEFINE_BOOL(native_video_scaler_stub,
                    false,
                    "ReRevved",
                    "Skip only the VdInitializeScalerCommandBuffer import at 0x826B0430 in the display owner 0x826B02B0 and store its 200 NOP dwords and the "
                    "count 200 as the SDK export does, when ring stub services are published and the call has the owner's one shape; the owner's reservation, "
                    "copy and cursor store still run")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_video_scaler_exit_check,
                    false,
                    "ReRevved",
                    "Bring-up only for renderer=native: force the scaler import to run and compare its count, output, copy and cursor store with the SDK "
                    "contract after every call; observes only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{

namespace
{

std::atomic<std::uint64_t>                                        enterCount{ 0 }, consumedCount{ 0 }, originalCount{ 0 }, writeFailedCount{ 0 };
std::array<std::atomic<std::uint64_t>, kVideoScalerRefusalCount>  refusalCounts{};
std::atomic<std::uint64_t>                                        exitOkCount{ 0 }, exitUnreadableCount{ 0 }, exitUnpairedCount{ 0 }, finalSkippedCount{ 0 };
std::array<std::atomic<std::uint64_t>, kVideoScalerMismatchCount> mismatchCounts{};
// Started calls whose observations have not finished. A gauge: it survives a drain.
std::atomic<std::uint64_t>                pendingCount{ 0 };
std::atomic<std::uint64_t>                nextInvocation{ 1 };
std::atomic<std::uint32_t>                pendingLimit{ kVideoScalerMaxPending };
std::atomic<std::uint32_t>                okLogged{ 0 };
constexpr std::uint32_t                   kOkLogLimit = 16;
std::timed_mutex                          reportMutex;
std::atomic<const VideoScalerOperations*> testOperations{ nullptr };

// One call's observation state. Each guest thread has at most one live call in
// this owner; a call nested below a live frame, or one that finds a record it
// cannot pair, is inconclusive and never exit_ok.
struct Record
{
    bool                active = false, poisoned = false, replaced = false, returned = false;
    bool                countValid = false, outputBad = false, returnBad = false, copyBad = false, cursorBad = false, sentinelBad = false, unreadable = false;
    std::uint64_t       invocation = 0, count = 0;
    std::uint32_t       sp = 0, device = 0, reservation = 0, pc = 0;
    VideoScalerBaseline baseline;
};

thread_local Record        record;
thread_local std::uint64_t touches = 0;

// The checks and the big-endian access of a guest `lwz` and `stw` over the
// kernel's memory.
rex::memory::Memory* kernelMemory() noexcept
{
    auto* kernel = rex::system::kernel_state();
    return kernel ? kernel->memory() : nullptr;
}

bool rangeAllows(rex::memory::Memory* memory, std::uint32_t address, std::uint32_t size, rex::memory::PageAccess needed) noexcept
{
    if (!memory || size == 0 || address < 0x10000 || address > 0xFFFFFFFFU - (size - 1))
        return false;
    const auto last    = address + size - 1;
    auto*      heap    = memory->LookupHeap(address);
    const auto granted = heap && memory->LookupHeap(last) == heap ? static_cast<unsigned>(heap->QueryRangeAccess(address, last)) : 0U;
    return (granted & static_cast<unsigned>(needed)) == static_cast<unsigned>(needed);
}

bool readKernel32(std::uint32_t address, std::uint32_t& value) noexcept
{
    auto* memory = kernelMemory();
    if (!rangeAllows(memory, address, 4, rex::memory::PageAccess::kReadOnly))
        return false;
    std::uint32_t raw = 0;
    std::memcpy(&raw, memory->TranslateVirtual<const std::uint8_t*>(address), sizeof(raw));
    value = rex::byte_swap(raw);
    return true;
}

bool writeKernel32(std::uint32_t address, std::uint32_t value) noexcept
{
    auto* memory = kernelMemory();
    if (!rangeAllows(memory, address, 4, rex::memory::PageAccess::kReadWrite))
        return false;
    const std::uint32_t raw = rex::byte_swap(value);
    std::memcpy(memory->TranslateVirtual<std::uint8_t*>(address), &raw, sizeof(raw));
    return true;
}

std::optional<std::uint64_t> kernelIdentity(std::uint32_t address, std::uint32_t bytes, bool write) noexcept
{
    auto* memory = kernelMemory();
    if (!rangeAllows(memory, address, bytes, write ? rex::memory::PageAccess::kReadWrite : rex::memory::PageAccess::kReadOnly))
        return std::nullopt;
    auto* heap = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(address + bytes - 1) != heap)
        return std::nullopt;
    if (heap->heap_type() == rex::memory::HeapType::kGuestPhysical)
    {
        const auto physical = memory->GetPhysicalAddress(address);
        if (physical == 0xFFFFFFFFU || std::uint64_t{ physical } + bytes - 1 != memory->GetPhysicalAddress(address + bytes - 1))
            return std::nullopt;
        return physical;
    }
    if (heap->heap_type() == rex::memory::HeapType::kGuestVirtual)
        return (std::uint64_t{ 1 } << 32) + address;
    return std::nullopt;
}

const VideoScalerOperations& productionOperations()
{
    static const VideoScalerOperations operations{
        { readKernel32, kernelIdentity, writeKernel32 },
        [](std::string_view reason)
        {
            REXLOG_ERROR("Native video scaler: {}; stopping because the output is partial and the import must not run over it", reason);
            std::terminate();
        },
        [](std::string_view message)
        {
            REXLOG_INFO("{}", message);
        },
    };
    return operations;
}

const VideoScalerOperations& activeOperations()
{
    const auto* test = testOperations.load(std::memory_order_acquire);
    return test ? *test : productionOperations();
}

constexpr std::uint32_t low32(std::uint64_t value) noexcept
{
    return static_cast<std::uint32_t>(value);
}

bool overlaps(std::uint64_t a, std::uint64_t sizeA, std::uint64_t b, std::uint64_t sizeB) noexcept
{
    return a < b + sizeB && b < a + sizeA;
}

// The words the owner's next steps leave alone: [SP+188], [SP+992], the ring
// header at R and the word after the copy.
VideoScalerBaseline captureBaseline(const VideoScalerMemory& memory, std::uint32_t sp, std::uint32_t reservation)
{
    VideoScalerBaseline baseline;
    if (sp > 0xFFFFFFFFU - kVideoScalerFrameBytes || reservation > 0xFFFFFFFFU - (kVideoScalerBytes + 7))
        return baseline;
    baseline.valid = memory.read32(sp + kVideoScalerBelowSentinel, baseline.below) && memory.read32(sp + kVideoScalerAboveSentinel, baseline.above) &&
                     memory.read32(reservation, baseline.header) && memory.read32(reservation + 4 + kVideoScalerBytes, baseline.tail);
    return baseline;
}

void logRecord(const VideoScalerOperations& operations, const Record& r, std::string_view verdict)
{
    if (!operations.log)
        return;
    const bool ok = verdict == "ok";
    if (ok && okLogged.fetch_add(1, std::memory_order_relaxed) >= kOkLogLimit)
        return;
    operations.log(std::format("Native video scaler check: pc={:08X} invocation={} thread={} sp={:08X} g={:08X} r={:08X} replaced={} count_valid={} count={} output={} return={} copy={} cursor={} sentinel={} unreadable={} verdict={}",
                               r.pc,
                               r.invocation,
                               std::hash<std::thread::id>{}(std::this_thread::get_id()),
                               r.sp,
                               r.device,
                               r.reservation,
                               r.replaced ? 1 : 0,
                               r.countValid ? 1 : 0,
                               r.count,
                               r.outputBad ? "bad" : "ok",
                               r.returnBad ? "bad" : "ok",
                               r.copyBad ? "bad" : "ok",
                               r.cursorBad ? "bad" : "ok",
                               r.sentinelBad ? "bad" : "ok",
                               r.unreadable || !r.baseline.valid ? 1 : 0,
                               verdict));
}

// Ends the thread's record: inconclusive when `conclusive` is false or the
// record was poisoned, never exit_ok then.
void finishRecord(const VideoScalerOperations& operations, bool conclusive) noexcept
{
    try
    {
        auto finished = record;
        record        = {};
        pendingCount.fetch_sub(1, std::memory_order_relaxed);
        if (!conclusive || finished.poisoned)
        {
            exitUnpairedCount.fetch_add(1, std::memory_order_relaxed);
            logRecord(operations, finished, "unpaired");
            return;
        }
        const bool bad[kVideoScalerMismatchCount]{ finished.outputBad, finished.returnBad, finished.copyBad, finished.cursorBad, finished.sentinelBad };
        bool       any = false;
        for (std::size_t i = 0; i < kVideoScalerMismatchCount; ++i)
            if (bad[i])
            {
                mismatchCounts[i].fetch_add(1, std::memory_order_relaxed);
                any = true;
            }
        const bool unreadable = finished.unreadable || !finished.baseline.valid;
        if (unreadable)
            exitUnreadableCount.fetch_add(1, std::memory_order_relaxed);
        else if (!any)
            exitOkCount.fetch_add(1, std::memory_order_relaxed);
        logRecord(operations, finished, unreadable ? "unreadable" : any ? "mismatch"
                                                                        : "ok");
    }
    catch (...)
    {
        record = {};
    }
}

// The thread's observation record for this call, or why there is none.
std::optional<VideoScalerRefusal> armRecord(const VideoScalerOperations& operations, std::uint32_t sp, std::uint32_t device, std::uint32_t reservation) noexcept
{
    try
    {
        ++touches;
        if (record.active)
        {
            if (sp < record.sp)
            {
                // Below the live frame: nested, so neither call can be paired.
                record.poisoned = true;
                return VideoScalerRefusal::Reentrant;
            }
            // The earlier frame is gone; its observations can no longer arrive.
            finishRecord(operations, false);
        }
        if (pendingCount.load(std::memory_order_relaxed) >= pendingLimit.load(std::memory_order_relaxed))
            return VideoScalerRefusal::Unpaired;
        record             = {};
        record.active      = true;
        record.invocation  = nextInvocation.fetch_add(1, std::memory_order_relaxed);
        record.sp          = sp;
        record.device      = device;
        record.reservation = reservation;
        record.baseline    = captureBaseline(operations.memory, sp, reservation);
        pendingCount.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    catch (...)
    {
        record = {};
        return VideoScalerRefusal::Unpaired;
    }
}

bool matches(std::uint32_t sp, std::uint32_t device, std::uint32_t reservation) noexcept
{
    return record.active && record.sp == sp && record.device == device && record.reservation == reservation;
}

// Reads `words` dwords from `address`; reports whether every word is the NOP.
// Returns false for an unreadable word.
bool readNops(const VideoScalerMemory& memory, std::uint32_t address, bool& matches)
{
    matches = true;
    for (std::uint32_t i = 0; i < kVideoScalerWords; ++i)
    {
        std::uint32_t value = 0;
        if (!memory.read32(address + 4 * i, value))
            return false;
        if (value != kVideoScalerNop)
            matches = false;
    }
    return true;
}

void checkSentinels(const VideoScalerMemory& memory, Record& r)
{
    if (!r.baseline.valid)
        return;
    std::uint32_t below = 0, above = 0;
    if (!memory.read32(r.sp + kVideoScalerBelowSentinel, below) || !memory.read32(r.sp + kVideoScalerAboveSentinel, above))
    {
        r.unreadable = true;
        return;
    }
    if (below != r.baseline.below || above != r.baseline.above)
        r.sentinelBad = true;
}

} // namespace

bool VideoScalerStubRequested() noexcept
{
    return RingStubsPublished() && REXCVAR_GET(native_video_scaler_stub);
}

bool VideoScalerExitCheckRequested() noexcept
{
    return REXCVAR_GET(native_video_scaler_exit_check);
}

bool VideoScalerActive() noexcept
{
    return VideoScalerExitCheckRequested() || VideoScalerStubRequested();
}

bool VideoScalerStubReportingRequested() noexcept
{
    return REXCVAR_GET(native_video_scaler_stub) || REXCVAR_GET(native_video_scaler_exit_check);
}

std::optional<VideoScalerRefusal> DeriveVideoScalerStub(const VideoScalerMemory& memory, const VideoScalerCall& call, const VideoScalerState& state, VideoScalerPlan& plan)
{
    using R = VideoScalerRefusal;
    if (!state.services)
        return R::Services;
    if (state.diagnostics)
        return R::Diagnostics;

    const std::uint32_t sp = low32(call.r1), device = low32(call.r31), reservation = low32(call.r30);
    // The frame must fit without wrapping, and the stack is 16-byte aligned.
    if ((sp & 15U) != 0 || sp > 0xFFFFFFFFU - kVideoScalerFrameBytes)
        return R::Stack;
    // The stack arguments sit in the low words of the 8-byte slots at SP+84, +92, +100 and +108.
    std::uint32_t slot[4]{};
    for (std::uint32_t i = 0; i < 4; ++i)
        if (!memory.read32(sp + 84 + 8 * i, slot[i]))
            return R::Stack;

    if (low32(call.arguments[6]) != sp + 156 || slot[0] != sp + 172 || slot[1] != sp + kVideoScalerAboveSentinel || slot[2] != sp + kVideoScalerOutputOffset || slot[3] != kVideoScalerWords)
        return R::Shape;

    const auto output = memory.identity(sp + kVideoScalerOutputOffset, kVideoScalerBytes, true);
    if (!output)
        return R::Unwritable;
    VideoScalerBaseline baseline;
    if (!memory.read32(sp + kVideoScalerBelowSentinel, baseline.below) || !memory.read32(sp + kVideoScalerAboveSentinel, baseline.above))
        return R::Unreadable;

    // Arithmetic through the word after the copy, R + 807.
    if (reservation == 0 || (reservation & 3U) != 0 || reservation > 0xFFFFFFFFU - (kVideoScalerBytes + 7))
        return R::Reservation;
    const auto copy = memory.identity(reservation + 4, kVideoScalerBytes, true);
    if (!copy || device > 0xFFFFFFFFU - kVideoScalerCursorOffset - 3 || !memory.identity(device + kVideoScalerCursorOffset, 4, true))
        return R::Reservation;
    if (!memory.read32(reservation, baseline.header) || !memory.read32(reservation + 4 + kVideoScalerBytes, baseline.tail))
        return R::Reservation;
    baseline.valid = true;

    // Physical as well as virtual overlap; an unknown identity refuses.
    if (device > 0xFFFFFFFFU - kVideoScalerDeviceBytes)
        return R::Alias;
    const auto deviceRange = memory.identity(device, kVideoScalerDeviceBytes, false);
    const auto below       = memory.identity(sp, kVideoScalerOutputOffset, false);
    const auto above       = memory.identity(sp + kVideoScalerAboveSentinel, kVideoScalerFrameBytes - kVideoScalerAboveSentinel, false);
    const auto header      = memory.identity(reservation, 4, false);
    const auto tail        = memory.identity(reservation + 4 + kVideoScalerBytes, 4, false);
    if (!deviceRange || !below || !above || !header || !tail)
        return R::Alias;
    constexpr std::uint64_t out = kVideoScalerBytes;
    // The output may not touch the rest of its frame, the device, the copy or the words around it.
    if (overlaps(*output, out, *below, kVideoScalerOutputOffset) || overlaps(*output, out, *above, kVideoScalerFrameBytes - kVideoScalerAboveSentinel) ||
        overlaps(*output, out, *deviceRange, kVideoScalerDeviceBytes) || overlaps(*output, out, *copy, out) || overlaps(*output, out, *header, 4) || overlaps(*output, out, *tail, 4))
        return R::Alias;
    // The copy destination may not touch the owner's frame or the device.
    if (overlaps(*copy, out, *below, kVideoScalerOutputOffset) || overlaps(*copy, out, *above, kVideoScalerFrameBytes - kVideoScalerAboveSentinel) ||
        overlaps(*copy, out, *deviceRange, kVideoScalerDeviceBytes))
        return R::Alias;

    if (state.record)
        return *state.record;

    plan             = {};
    plan.sp          = sp;
    plan.device      = device;
    plan.reservation = reservation;
    plan.output      = sp + kVideoScalerOutputOffset;
    plan.copy        = reservation + 4;
    plan.cursor      = device + kVideoScalerCursorOffset;
    plan.cursorValue = reservation + kVideoScalerBytes;
    plan.baseline    = baseline;
    return std::nullopt;
}

VideoScalerApply ApplyVideoScalerStub(const VideoScalerMemory& memory, const VideoScalerPlan& plan) noexcept
{
    for (std::uint32_t i = 0; i < kVideoScalerWords; ++i)
    {
        bool stored = false;
        try
        {
            stored = memory.write32(plan.output + 4 * i, kVideoScalerNop);
        }
        catch (...)
        {
        }
        if (!stored)
            return i == 0 ? VideoScalerApply::Unstarted : VideoScalerApply::Failed;
    }
    return VideoScalerApply::Done;
}

bool ReplaceVideoScalerCall(const VideoScalerCall& call, std::uint64_t& r3) noexcept
{
    if (!VideoScalerActive()) [[likely]]
        return false;
    return ReplaceVideoScalerCallWith(activeOperations(), call, r3);
}

bool ReplaceVideoScalerCallWith(const VideoScalerOperations& operations, const VideoScalerCall& call, std::uint64_t& r3) noexcept
{
    // Set from the first store until the call is known to be either complete or untouched. A failed
    // store leaves it set through counting, formatting, logging and the terminal callback, so an
    // exception on that path still ends in the fail-stop and never in the import.
    bool writing = false, failureCounted = false, terminalCalled = false;
    try
    {
        const bool stub = VideoScalerStubRequested(), check = VideoScalerExitCheckRequested();
        if (!stub && !check)
            return false;
        enterCount.fetch_add(1, std::memory_order_relaxed);
        const std::uint32_t sp = low32(call.r1), device = low32(call.r31), reservation = low32(call.r30);
        // Held through the last store; a withdrawal waits for it.
        std::optional<RingStubLease> lease;
        if (stub)
            lease.emplace();
        const auto armed = armRecord(operations, sp, device, reservation);
        if (stub)
        {
            VideoScalerState state;
            state.services    = lease && static_cast<bool>(*lease) && RingStubsReady(device);
            state.diagnostics = check;
            state.record      = armed;
            VideoScalerPlan plan;
            auto            refusal = DeriveVideoScalerStub(operations.memory, call, state, plan);
            if (!refusal)
            {
                writing            = true;
                const auto applied = ApplyVideoScalerStub(operations.memory, plan);
                if (applied == VideoScalerApply::Done)
                {
                    writing = false;
                    r3      = kVideoScalerWords;
                    if (!armed && record.active)
                        record.replaced = true;
                    consumedCount.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                if (applied == VideoScalerApply::Failed)
                {
                    // The output is partial; Original must not run over it.
                    writeFailedCount.fetch_add(1, std::memory_order_relaxed);
                    failureCounted    = true;
                    const auto reason = std::format("write failed after the output started: sp={:08X} g={:08X} r={:08X}", sp, device, reservation);
                    if (operations.log)
                        operations.log(reason);
                    terminalCalled = true;
                    if (operations.terminal)
                        operations.terminal(reason);
                    return true;
                }
                // The first store failed: nothing changed.
                writing = false;
                refusal = VideoScalerRefusal::Unwritable;
            }
            refusalCounts[static_cast<std::size_t>(*refusal)].fetch_add(1, std::memory_order_relaxed);
        }
        originalCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    catch (...)
    {
        // A fault after the output started cannot fall back to the import.
        if (writing)
        {
            if (!failureCounted)
                writeFailedCount.fetch_add(1, std::memory_order_relaxed);
            if (!terminalCalled && operations.terminal)
            {
                try
                {
                    operations.terminal("exception while recording the output");
                }
                catch (...)
                {
                }
            }
            return true;
        }
        return false;
    }
}

void ObserveVideoScalerReturn(std::uint32_t sp, std::uint64_t r3, std::uint32_t r30, std::uint32_t r31) noexcept
{
    if (!VideoScalerStubReportingRequested()) [[likely]]
        return;
    try
    {
        if (!VideoScalerActive() && !record.active)
            return;
        ++touches;
        const auto& operations = activeOperations();
        if (!matches(sp, r31, r30) || record.returned)
        {
            exitUnpairedCount.fetch_add(1, std::memory_order_relaxed);
            // A second return for the live call cannot be paired either.
            if (matches(sp, r31, r30))
                record.poisoned = true;
            return;
        }
        auto& r      = record;
        r.pc         = kVideoScalerReturnAddress;
        r.count      = r3;
        r.countValid = (r3 >> 32) == 0;
        r.returnBad  = r3 != kVideoScalerWords;
        bool nops    = true;
        if (!readNops(operations.memory, sp + kVideoScalerOutputOffset, nops))
            r.unreadable = true;
        else if (!nops)
            r.outputBad = true;
        checkSentinels(operations.memory, r);
        r.returned = true;
    }
    catch (...)
    {
        if (record.active)
            record.poisoned = true;
    }
}

void ObserveVideoScalerCopy(std::uint32_t sp, std::uint32_t r30, std::uint32_t r31) noexcept
{
    if (!VideoScalerStubReportingRequested()) [[likely]]
        return;
    try
    {
        if (!VideoScalerActive() && !record.active)
            return;
        ++touches;
        const auto& operations = activeOperations();
        if (!matches(sp, r31, r30))
        {
            exitUnpairedCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!record.returned)
        {
            // The return was not observed: the pair is incomplete.
            record.pc = kVideoScalerCopyAddress;
            finishRecord(operations, false);
            return;
        }
        auto& r   = record;
        r.pc      = kVideoScalerCopyAddress;
        bool nops = true;
        if (!readNops(operations.memory, r30 + 4, nops))
            r.unreadable = true;
        else if (!nops)
            r.copyBad = true;
        std::uint32_t cursor = 0, header = 0, tail = 0;
        if (!operations.memory.read32(r31 + kVideoScalerCursorOffset, cursor))
            r.unreadable = true;
        else if (cursor != r30 + kVideoScalerBytes)
            r.cursorBad = true;
        if (r.baseline.valid)
        {
            if (!operations.memory.read32(r30, header) || !operations.memory.read32(r30 + 4 + kVideoScalerBytes, tail))
                r.unreadable = true;
            else if (header != r.baseline.header || tail != r.baseline.tail)
                r.sentinelBad = true;
        }
        checkSentinels(operations.memory, r);
        finishRecord(operations, true);
    }
    catch (...)
    {
        if (record.active)
            record.poisoned = true;
    }
}

VideoScalerStubCounts TakeVideoScalerStubCounts() noexcept
{
    VideoScalerStubCounts counts;
    counts.enter       = enterCount.exchange(0, std::memory_order_relaxed);
    counts.consumed    = consumedCount.exchange(0, std::memory_order_relaxed);
    counts.original    = originalCount.exchange(0, std::memory_order_relaxed);
    counts.writeFailed = writeFailedCount.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kVideoScalerRefusalCount; ++i)
        counts.refusal[i] = refusalCounts[i].exchange(0, std::memory_order_relaxed);
    counts.exitOk         = exitOkCount.exchange(0, std::memory_order_relaxed);
    counts.exitUnreadable = exitUnreadableCount.exchange(0, std::memory_order_relaxed);
    counts.exitUnpaired   = exitUnpairedCount.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kVideoScalerMismatchCount; ++i)
        counts.mismatch[i] = mismatchCounts[i].exchange(0, std::memory_order_relaxed);
    counts.finalSkipped = finalSkippedCount.exchange(0, std::memory_order_relaxed);
    counts.pending      = pendingCount.load(std::memory_order_relaxed);
    return counts;
}

std::string FormatVideoScalerStubCounts(const VideoScalerStubCounts& counts)
{
    auto fields = std::format(" video_scaler_enter={} video_scaler_consumed={} video_scaler_original={}", counts.enter, counts.consumed, counts.original);
    for (std::size_t i = 0; i < kVideoScalerRefusalCount; ++i)
        fields += std::format(" video_scaler_refusal_{}={}", kVideoScalerRefusalNames[i], counts.refusal[i]);
    fields += std::format(" video_scaler_write_failed={} video_scaler_exit_ok={}", counts.writeFailed, counts.exitOk);
    for (std::size_t i = 0; i < kVideoScalerMismatchCount; ++i)
        fields += std::format(" video_scaler_exit_mismatch_{}={}", kVideoScalerMismatchNames[i], counts.mismatch[i]);
    fields += std::format(" video_scaler_exit_unreadable={} video_scaler_exit_unpaired={} video_scaler_pending={}", counts.exitUnreadable, counts.exitUnpaired, counts.pending);
    if (counts.final)
        fields += std::format(" video_scaler_final_incomplete={} video_scaler_final_skipped={}", counts.pending != 0 || counts.finalSkipped != 0 ? 1 : 0, counts.finalSkipped);
    return fields;
}

namespace detail
{

std::timed_mutex& VideoScalerReportMutex() noexcept
{
    return reportMutex;
}

} // namespace detail

std::string ReportVideoScalerStub(bool shutdown, const VideoScalerReportOperations& operations) noexcept
{
    if (!VideoScalerStubReportingRequested())
        return {};
    const auto log = [&](std::string_view fields)
    {
        try
        {
            if (operations.log)
                operations.log(fields);
            else
                REXLOG_INFO("Native video scaler: shutdown{}", fields);
        }
        catch (...)
        {
        }
    };
    try
    {
        std::unique_lock lock(reportMutex, std::defer_lock);
        if (shutdown)
        {
            // A drain on another thread may still hold the lock; the final waits only briefly.
            if (!lock.try_lock_for(kVideoScalerShutdownWait))
            {
                finalSkippedCount.fetch_add(1, std::memory_order_relaxed);
                auto fields = std::format(" video_scaler_pending={} video_scaler_final_incomplete=1 video_scaler_final_skipped=1", pendingCount.load(std::memory_order_relaxed));
                log(fields);
                return fields;
            }
        }
        else
            lock.lock();
        auto counts  = TakeVideoScalerStubCounts();
        counts.final = shutdown;
        auto fields  = operations.format(counts);
        if (shutdown)
            log(fields);
        return fields;
    }
    catch (...)
    {
    }
    return {};
}

void SetVideoScalerOperationsForTesting(const VideoScalerOperations* operations) noexcept
{
    testOperations.store(operations, std::memory_order_release);
}

std::uint64_t VideoScalerThreadTouchesForTesting() noexcept
{
    return touches;
}

void SetVideoScalerPendingLimitForTesting(std::uint32_t limit) noexcept
{
    pendingLimit.store(limit, std::memory_order_relaxed);
}

} // namespace rerevved::gpu::guest_d3d
