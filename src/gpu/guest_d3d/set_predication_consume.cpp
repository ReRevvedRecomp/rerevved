#include "set_predication_consume.h"

#include "ring_stubs.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <format>

REXCVAR_DEFINE_BOOL(native_consume_set_predication,
                    true,
                    "ReRevved",
                    "On by default for renderer=native with native_ring_stubs: skip the SetPredication (0x8269CD20) Original on calls whose exit the "
                    "host derives exactly (the predicate, the mask, G+0x31A8, G+0x2AB4, G+0x2AB8 and the tiling byte G+0x2ABC), when the cursor is "
                    "not above the soft limit and no diagnostic needs its body; pre-swap calls require native_consume_set_predication_preswap")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_consume_set_predication_preswap,
                    true,
                    "ReRevved",
                    "Consume the host pre-swap stub's SetPredication clear with exact packet words, cursor and r3; requires "
                    "native_consume_set_predication and validated ring stubs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if REREVVED_DEVELOPER
REXCVAR_DEFINE_BOOL(native_set_predication_exit_check,
                    false,
                    "ReRevved",
                    "Bring-up only for renderer=native: compare the exit state the SetPredication consume (native_consume_set_predication) would "
                    "leave with the guest words after every SetPredication call whose Original runs. Forces the consume to fall back; observes "
                    "only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif

namespace rerevved::gpu::guest_d3d
{

namespace
{

thread_local SetPredicationPreSwapScope* preSwapScope = nullptr;
thread_local SetPredicationCallScope*    callScope    = nullptr;

#if REREVVED_DEVELOPER
std::atomic<std::uint64_t>                                           consumedCount{ 0 }, consumedClearCount{ 0 };
std::array<std::atomic<std::uint64_t>, kSetPredicationFallbackCount> fallbackCounts{};
std::atomic<std::uint64_t>                                           exitOk{ 0 }, exitFlushed{ 0 }, exitUnreadable{ 0 }, exitUnmodeledStackOverlap{ 0 };
std::array<std::atomic<std::uint64_t>, kSetPredicationExitWordCount> exitMismatch{};
std::atomic<std::uint64_t>                                           preSwapEntered{ 0 }, preSwapOriginal{ 0 }, preSwapConsumed{ 0 };
std::array<std::atomic<std::uint64_t>, kSetPredicationFallbackCount> preSwapFallback{};
std::atomic<std::uint64_t>                                           preSwapExitOk{ 0 }, preSwapExitExact{ 0 }, preSwapExitFlushed{ 0 }, preSwapExitUnreadable{ 0 }, preSwapExitStack{ 0 }, preSwapExitUnpaired{ 0 };
std::array<std::atomic<std::uint64_t>, kSetPredicationExitWordCount> preSwapExitMismatch{};

void noteFallback(SetPredicationFallback reason, const SetPredicationConsumeState& state) noexcept
{
    fallbackCounts[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
    if (state.reportPreSwap)
        preSwapFallback[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void noteConsumed(const SetPredicationConsumeState& state, bool clear) noexcept
{
    if (state.caller == kSetPredicationPreSwapReturn && state.reportPreSwap)
        preSwapConsumed.fetch_add(1, std::memory_order_relaxed);
    consumedCount.fetch_add(1, std::memory_order_relaxed);
    if (clear)
        consumedClearCount.fetch_add(1, std::memory_order_relaxed);
}
#else
void noteFallback(SetPredicationFallback, const SetPredicationConsumeState&) noexcept
{
}

void noteConsumed(const SetPredicationConsumeState&, bool) noexcept
{
}
#endif

// The checks and the big-endian access of a guest `lwz`, `stw` and `stb` over
// the kernel's memory.
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

bool readKernelWord(std::uint32_t address, std::uint32_t& value) noexcept
{
    auto* memory = kernelMemory();
    if (!rangeAllows(memory, address, 4, rex::memory::PageAccess::kReadOnly))
        return false;
    std::uint32_t raw = 0;
    std::memcpy(&raw, memory->TranslateVirtual<const std::uint8_t*>(address), sizeof(raw));
    value = rex::byte_swap(raw);
    return true;
}

bool writableKernel(std::uint32_t address, std::uint32_t bytes) noexcept
{
    return rangeAllows(kernelMemory(), address, bytes, rex::memory::PageAccess::kReadWrite);
}

std::optional<std::uint64_t> kernelIdentity(std::uint32_t address, std::uint32_t bytes) noexcept
{
    auto* memory = kernelMemory();
    if (!memory || !bytes || address > 0xFFFFFFFFU - (bytes - 1))
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

void writeKernel(const SetPredicationWrite& write) noexcept
{
    auto* memory = kernelMemory();
    if (!rangeAllows(memory, write.address, write.byte ? 1 : 4, rex::memory::PageAccess::kReadWrite))
        return;
    if (write.byte)
    {
        *memory->TranslateVirtual<std::uint8_t*>(write.address) = static_cast<std::uint8_t>(write.value);
        return;
    }
    const std::uint32_t raw = rex::byte_swap(write.value);
    std::memcpy(memory->TranslateVirtual<std::uint8_t*>(write.address), &raw, sizeof(raw));
}

} // namespace

bool SetPredicationConsumeRequested() noexcept
{
    return RingStubsPublished() && REXCVAR_GET(native_consume_set_predication);
}

bool SetPredicationPreSwapRequested() noexcept
{
    return REXCVAR_GET(native_consume_set_predication_preswap);
}

#if REREVVED_DEVELOPER
bool SetPredicationExitCheckRequested() noexcept
{
    return REXCVAR_GET(native_set_predication_exit_check);
}

bool SetPredicationPreSwapReportingRequested() noexcept
{
    return SetPredicationPreSwapRequested() || SetPredicationExitCheckRequested();
}
#endif

SetPredicationPreSwapScope::SetPredicationPreSwapScope(const PPCContext& ctx, std::uint32_t g) noexcept
: context(&ctx)
, device(g)
, previous(preSwapScope)
{
    preSwapScope = this;
}

SetPredicationPreSwapScope::~SetPredicationPreSwapScope()
{
#if REREVVED_DEVELOPER
    if (!paired && SetPredicationExitCheckRequested())
        preSwapExitUnpaired.fetch_add(1, std::memory_order_relaxed);
#endif
    preSwapScope = previous;
}

SetPredicationCallScope::SetPredicationCallScope(const PPCContext& ctx) noexcept
: previous(callScope)
{
    if (preSwapScope && !preSwapScope->claimed)
    {
        preSwapScope->claimed = true;
        trusted               = preSwapScope->context == &ctx && preSwapScope->device == ctx.r3.u32 &&
                                ctx.r4.u32 == 0 && static_cast<std::uint32_t>(ctx.lr) == kSetPredicationPreSwapReturn;
        preSwapScope->paired  = trusted;
    }
#if REREVVED_DEVELOPER
    reporting = trusted && SetPredicationPreSwapReportingRequested();
    if (reporting)
        preSwapEntered.fetch_add(1, std::memory_order_relaxed);
#endif
    callScope = this;
}

SetPredicationCallScope::~SetPredicationCallScope()
{
    callScope = previous;
}

void SetPredicationCallScope::Original() const noexcept
{
#if REREVVED_DEVELOPER
    if (reporting)
        preSwapOriginal.fetch_add(1, std::memory_order_relaxed);
#endif
}

bool SetPredicationCallScope::TrustedPreSwap() noexcept
{
    return callScope && callScope->trusted;
}

std::optional<SetPredicationFallback> SetPredicationConsumeVerdict(const SetPredicationConsumeState& state,
                                                                   const GuestWordReader&            read,
                                                                   const SetPredicationWritable&     writable,
                                                                   std::uint32_t                     g,
                                                                   std::uint32_t                     r4,
                                                                   SetPredicationPlan*               plan)
{
    if (!state.stubsReady)
        return SetPredicationFallback::Stubs;
    if (state.diagnostics)
        return SetPredicationFallback::Diagnostics;
    const bool preSwap = state.caller == kSetPredicationPreSwapReturn;
    if (preSwap && (!state.preSwapEnabled || !state.trustedPreSwap || r4 != 0))
        return SetPredicationFallback::PreSwap;
    // A device that wraps its span cannot be placed: unreadable, as in ReadSetPredicationWatch.
    if (g > 0xFFFFFFFFU - kSetPredicationDeviceSpan)
        return SetPredicationFallback::Unreadable;
    // Before any other guest word is read: a skipped Original's saves would change the words below.
    StackReach reach;
    if (SetPredicationStackOverlaps(read, { state.pcr, state.sp }, g, reach))
        return SetPredicationFallback::StackOverlap;
    SetPredicationWatch watch;
    if (!ReadSetPredicationWatch(read, g, watch))
        return SetPredicationFallback::Unreadable;
    const auto exit = DeriveSetPredicationExit(r4, watch);
    if (preSwap)
    {
        // Original may flush only before appending. Retain that call, without replaying any store.
        if (exit.flush)
            return SetPredicationFallback::RingFlush;
        if ((watch.cursor & 3U) || watch.cursor > 0xFFFFFFFFU - 11)
            return SetPredicationFallback::Unreadable;
        if (SetPredicationRingOverlaps(reach, watch.cursor))
            return SetPredicationFallback::StackOverlap;
        const auto packet = std::uint64_t{ watch.cursor } + 4;
        // A packet cannot alias a watched device word or the KPCR that established the reach.
        if ((packet < std::uint64_t{ g } + ring_layout::kDeviceEnd && g < packet + 8) ||
            (packet < std::uint64_t{ state.pcr } + kPcrStackBytes && state.pcr < packet + 8))
            return SetPredicationFallback::Unreadable;
        if (state.memoryIdentity)
        {
            const auto ring   = state.memoryIdentity(watch.cursor + 4, 8);
            const auto device = state.memoryIdentity(g, ring_layout::kDeviceEnd);
            const auto stack  = state.memoryIdentity(static_cast<std::uint32_t>(reach.Low()), static_cast<std::uint32_t>(reach.High() - reach.Low()));
            const auto pcr    = state.memoryIdentity(state.pcr, kPcrStackBytes);
            if (!ring || !device || !stack || !pcr)
                return SetPredicationFallback::Unreadable;
            const auto overlaps = [](std::uint64_t a, std::uint64_t sizeA, std::uint64_t b, std::uint64_t sizeB)
            {
                return a < b + sizeB && b < a + sizeA;
            };
            if (overlaps(*stack, reach.High() - reach.Low(), *ring, 8) ||
                overlaps(*stack, reach.High() - reach.Low(), *device, kSetPredicationDeviceSpan) ||
                overlaps(*stack, reach.High() - reach.Low(), *pcr, kPcrStackBytes))
                return SetPredicationFallback::StackOverlap;
            if (overlaps(*ring, 8, *device, ring_layout::kDeviceEnd) || overlaps(*ring, 8, *pcr, kPcrStackBytes))
                return SetPredicationFallback::Unreadable;
        }
    }
    const auto writes = SetPredicationExitWrites(g, watch, exit, preSwap);
    // A store that cannot land would leave a half-replayed call, so check them all first.
    for (std::size_t i = 0; i < writes.count; ++i)
        if (!writable(writes.list[i].address, writes.list[i].byte ? 1 : 4))
            return SetPredicationFallback::Unreadable;
    if (exit.flush)
        return SetPredicationFallback::RingFlush;
    if (plan)
    {
        plan->watch  = watch;
        plan->exit   = exit;
        plan->writes = writes;
    }
    return std::nullopt;
}

bool ConsumeSetPredicationWith(const SetPredicationConsumeState& state,
                               const GuestWordReader&            read,
                               const SetPredicationWritable&     writable,
                               const SetPredicationWriter&       write,
                               std::uint32_t                     g,
                               std::uint32_t                     r4,
                               std::uint64_t*                    result) noexcept
{
    bool replaying = false;
    try
    {
        SetPredicationPlan plan;
        if (const auto verdict = SetPredicationConsumeVerdict(state, read, writable, g, r4, &plan))
        {
            noteFallback(*verdict, state);
            return false;
        }
        replaying = true;
        for (std::size_t i = 0; i < plan.writes.count; ++i)
            write(plan.writes.list[i]);
        if (state.caller == kSetPredicationPreSwapReturn)
        {
            if (result)
                *result = plan.watch.cursor + 8;
        }
        noteConsumed(state, r4 == 0);
        return true;
    }
    catch (...)
    {
        // The production writer is noexcept. Never run Original after a partial replay.
        if (replaying)
            std::terminate();
        return false;
    }
}

bool ConsumeSetPredication(PPCContext& ctx, bool stubsReady, bool diagnostics) noexcept
{
    if (!SetPredicationConsumeRequested()) [[likely]]
        return false;
    // Held until the decision (and any guest store) is done; a withdrawal waits for it.
    const RingStubLease lease;
    if (!lease) [[unlikely]]
    {
#if REREVVED_DEVELOPER
        if (SetPredicationCallScope::TrustedPreSwap() && SetPredicationPreSwapReportingRequested())
            preSwapFallback[static_cast<std::size_t>(SetPredicationFallback::Stubs)].fetch_add(1, std::memory_order_relaxed);
#endif
        return false;
    }
    SetPredicationConsumeState state;
    state.stubsReady     = stubsReady;
    state.diagnostics    = diagnostics || SetPredicationExitCheckRequested();
    state.caller         = static_cast<std::uint32_t>(ctx.lr);
    state.sp             = ctx.r1.u32;
    state.pcr            = ctx.r13.u32;
    state.preSwapEnabled = SetPredicationPreSwapRequested();
    state.trustedPreSwap = SetPredicationCallScope::TrustedPreSwap();
#if REREVVED_DEVELOPER
    state.reportPreSwap = state.trustedPreSwap && SetPredicationPreSwapReportingRequested();
#endif
    state.memoryIdentity = kernelIdentity;
    return ConsumeSetPredicationWith(state, readKernelWord, writableKernel, writeKernel, ctx.r3.u32, ctx.r4.u32, &ctx.r3.u64);
}

#if REREVVED_DEVELOPER
SetPredicationConsumeCounts TakeSetPredicationConsumeCounts() noexcept
{
    SetPredicationConsumeCounts counts;
    counts.consumed      = consumedCount.exchange(0, std::memory_order_relaxed);
    counts.consumedClear = consumedClearCount.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kSetPredicationFallbackCount; ++i)
        counts.fallback[i] = fallbackCounts[i].exchange(0, std::memory_order_relaxed);
    return counts;
}

std::string FormatSetPredicationConsumeCounts(const SetPredicationConsumeCounts& counts)
{
    std::string fields = std::format(" setpred_consume_ok={} setpred_consume_ok_clear={}", counts.consumed, counts.consumedClear);
    for (std::size_t i = 0; i < kSetPredicationFallbackCount; ++i)
        if (counts.fallback[i])
            fields += std::format(" setpred_consume_fallback_{}={}", kSetPredicationFallbackNames[i], counts.fallback[i]);
    return fields;
}

SetPredicationPreSwapCounts TakeSetPredicationPreSwapCounts() noexcept
{
    SetPredicationPreSwapCounts counts;
    counts.entered  = preSwapEntered.exchange(0, std::memory_order_relaxed);
    counts.original = preSwapOriginal.exchange(0, std::memory_order_relaxed);
    counts.consumed = preSwapConsumed.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kSetPredicationFallbackCount; ++i)
        counts.fallback[i] = preSwapFallback[i].exchange(0, std::memory_order_relaxed);
    return counts;
}

std::string FormatSetPredicationPreSwapCounts(const SetPredicationPreSwapCounts& counts)
{
    auto fields = std::format(" setpred_preswap_enter={} setpred_preswap_original={} setpred_consume_ok_preswap={}", counts.entered, counts.original, counts.consumed);
    for (std::size_t i = 0; i < kSetPredicationFallbackCount; ++i)
        fields += std::format(" setpred_consume_fallback_preswap_{}={}", kSetPredicationFallbackNames[i], counts.fallback[i]);
    return fields;
}

std::optional<SetPredicationProbeEntry> BeginSetPredicationProbe(const GuestWordReader& read,
                                                                 std::uint32_t          g,
                                                                 std::uint32_t          r4,
                                                                 std::uint32_t          sp,
                                                                 std::uint32_t          pcr,
                                                                 bool                   preSwap) noexcept
{
    try
    {
        StackReach reach;
        if (g <= 0xFFFFFFFFU - kSetPredicationDeviceSpan && SetPredicationStackOverlaps(read, { pcr, sp }, g, reach))
        {
            exitUnmodeledStackOverlap.fetch_add(1, std::memory_order_relaxed);
            if (preSwap)
                preSwapExitStack.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        SetPredicationProbeEntry entry;
        entry.g       = g;
        entry.r4      = r4;
        entry.preSwap = preSwap;
        if (!ReadSetPredicationWatch(read, g, entry.before))
        {
            exitUnreadable.fetch_add(1, std::memory_order_relaxed);
            if (preSwap)
                preSwapExitUnreadable.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        entry.predicted = DeriveSetPredicationExit(r4, entry.before);
        // The ring words the end of the probe reads, behind the cursor word just read.
        if (!entry.predicted.flush && SetPredicationRingOverlaps(reach, entry.before.cursor))
        {
            exitUnmodeledStackOverlap.fetch_add(1, std::memory_order_relaxed);
            if (preSwap)
                preSwapExitStack.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        return entry;
    }
    catch (...)
    {
        if (preSwap)
            preSwapExitUnpaired.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
}

void EndSetPredicationProbe(const GuestWordReader& read, const SetPredicationProbeEntry& entry, std::uint32_t result) noexcept
{
    try
    {
        SetPredicationObserved observed;
        observed.result = result;
        if (!ReadSetPredicationWatch(read, entry.g, observed.watch))
        {
            exitUnreadable.fetch_add(1, std::memory_order_relaxed);
            if (entry.preSwap)
                preSwapExitUnreadable.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Without a flush Original writes the two ring words just past the entry cursor.
        if (!entry.predicted.flush && (!read(entry.before.cursor + 4, observed.ringHeader) || !read(entry.before.cursor + 8, observed.ringMask)))
        {
            exitUnreadable.fetch_add(1, std::memory_order_relaxed);
            if (entry.preSwap)
                preSwapExitUnreadable.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto mismatch = SetPredicationExitMismatches(entry.predicted, entry.before, observed);
        if (!mismatch)
            exitOk.fetch_add(1, std::memory_order_relaxed);
        if (entry.predicted.flush)
            exitFlushed.fetch_add(1, std::memory_order_relaxed);
        for (std::size_t i = 0; i < kSetPredicationExitWordCount; ++i)
            if (mismatch & (1U << i))
                exitMismatch[i].fetch_add(1, std::memory_order_relaxed);
        if (entry.preSwap)
        {
            if (!mismatch)
            {
                preSwapExitOk.fetch_add(1, std::memory_order_relaxed);
                if (!entry.predicted.flush)
                    preSwapExitExact.fetch_add(1, std::memory_order_relaxed);
            }
            if (entry.predicted.flush)
                preSwapExitFlushed.fetch_add(1, std::memory_order_relaxed);
            for (std::size_t i = 0; i < kSetPredicationExitWordCount; ++i)
                if (mismatch & (1U << i))
                    preSwapExitMismatch[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
    catch (...)
    {
        if (entry.preSwap)
            preSwapExitUnpaired.fetch_add(1, std::memory_order_relaxed);
    }
}

SetPredicationExitCounts TakeSetPredicationExitCounts() noexcept
{
    SetPredicationExitCounts counts;
    counts.ok                    = exitOk.exchange(0, std::memory_order_relaxed);
    counts.flushed               = exitFlushed.exchange(0, std::memory_order_relaxed);
    counts.unreadable            = exitUnreadable.exchange(0, std::memory_order_relaxed);
    counts.unmodeledStackOverlap = exitUnmodeledStackOverlap.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kSetPredicationExitWordCount; ++i)
        counts.mismatch[i] = exitMismatch[i].exchange(0, std::memory_order_relaxed);
    counts.preSwap.ok                    = preSwapExitOk.exchange(0, std::memory_order_relaxed);
    counts.preSwap.exact                 = preSwapExitExact.exchange(0, std::memory_order_relaxed);
    counts.preSwap.flushed               = preSwapExitFlushed.exchange(0, std::memory_order_relaxed);
    counts.preSwap.unreadable            = preSwapExitUnreadable.exchange(0, std::memory_order_relaxed);
    counts.preSwap.unmodeledStackOverlap = preSwapExitStack.exchange(0, std::memory_order_relaxed);
    counts.preSwap.unpaired              = preSwapExitUnpaired.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kSetPredicationExitWordCount; ++i)
        counts.preSwap.mismatch[i] = preSwapExitMismatch[i].exchange(0, std::memory_order_relaxed);
    return counts;
}

std::string FormatSetPredicationExitCounts(const SetPredicationExitCounts& counts)
{
    std::string fields = std::format(" setpred_exit_ok={} setpred_exit_flushed={} setpred_exit_unreadable={}", counts.ok, counts.flushed, counts.unreadable);
    if (counts.unmodeledStackOverlap)
        fields += std::format(" setpred_exit_unmodeled_stack_overlap={}", counts.unmodeledStackOverlap);
    for (std::size_t i = 0; i < kSetPredicationExitWordCount; ++i)
        fields += std::format(" setpred_exit_mismatch_{}={}", kSetPredicationExitWordNames[i], counts.mismatch[i]);
    if (SetPredicationPreSwapReportingRequested())
    {
        const auto& pre = counts.preSwap;
        fields += std::format(" setpred_exit_preswap_ok={} setpred_exit_preswap_exact={} setpred_exit_preswap_flushed={} setpred_exit_preswap_unreadable={} setpred_exit_preswap_unmodeled_stack_overlap={} setpred_exit_preswap_unpaired={}",
                              pre.ok,
                              pre.exact,
                              pre.flushed,
                              pre.unreadable,
                              pre.unmodeledStackOverlap,
                              pre.unpaired);
        for (std::size_t i = 0; i < kSetPredicationExitWordCount; ++i)
            fields += std::format(" setpred_exit_preswap_mismatch_{}={}", kSetPredicationExitWordNames[i], pre.mismatch[i]);
    }
    return fields;
}

SetPredicationExitProbe::SetPredicationExitProbe(const PPCContext& ctx) noexcept
{
    if (!SetPredicationExitCheckRequested()) [[likely]]
        return;
    try
    {
        auto armed = BeginSetPredicationProbe(readKernelWord, ctx.r3.u32, ctx.r4.u32, ctx.r1.u32, ctx.r13.u32, SetPredicationCallScope::TrustedPreSwap());
        if (!armed)
            return;
        entry      = std::make_unique<SetPredicationProbeEntry>(*armed);
        context    = &ctx;
        exceptions = std::uncaught_exceptions();
    }
    catch (...)
    {
        if (SetPredicationCallScope::TrustedPreSwap())
            preSwapExitUnpaired.fetch_add(1, std::memory_order_relaxed);
        entry.reset();
    }
}

SetPredicationExitProbe::~SetPredicationExitProbe()
{
    if (!entry)
        return;
    // A call that unwound has no exit to compare.
    if (std::uncaught_exceptions() > exceptions)
    {
        if (entry->preSwap)
            preSwapExitUnpaired.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    EndSetPredicationProbe(readKernelWord, *entry, context->r3.u32);
}
#endif

} // namespace rerevved::gpu::guest_d3d
