#include "window_writer_consume.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <exception>
#include <format>
#include <new>

REXCVAR_DEFINE_BOOL(native_consume_window_writer,
                    true,
                    "ReRevved",
                    "On by default for renderer=native with native_ring_stubs: skip the window packet writer's (0x8268D3A0) Original on calls whose exit "
                    "the host derives exactly (r3), when it cannot flush and no diagnostic needs its body")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{

namespace
{

#if REREVVED_DEVELOPER
// The return addresses of the call sites (see WindowWriterCaller).
constexpr std::uint32_t kReturn8268F9C0 = 0x8268FAA0, kReturn82690040 = 0x826900F8, kReturn826922D0 = 0x82693110, kReturn82698720 = 0x82698AA0, kReturnClear = 0x826A2948;
constexpr std::uint32_t kReturnResolveA = 0x8269E964, kReturnResolveB = 0x8269F1C4;

std::atomic<std::uint64_t>                                         consumedCount{ 0 }, ringWordsConsumed{ 0 };
std::array<std::atomic<std::uint64_t>, kWindowWriterCallerCount>   callerCounts{};
std::atomic<std::uint64_t>                                         ringWordsOriginal{ 0 }, originalCount{ 0 }, originalUnbounded{ 0 };
std::array<std::atomic<std::uint64_t>, kWindowWriterFallbackCount> fallbackCounts{};

void noteFallback(WindowWriterFallback reason) noexcept
{
    fallbackCounts[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void noteConsumed(std::uint32_t caller, std::uint32_t words) noexcept
{
    consumedCount.fetch_add(1, std::memory_order_relaxed);
    callerCounts[static_cast<std::size_t>(WindowWriterCallerOf(caller))].fetch_add(1, std::memory_order_relaxed);
    ringWordsConsumed.fetch_add(words, std::memory_order_relaxed);
}
#else
void noteFallback(WindowWriterFallback) noexcept
{
}

void noteConsumed(std::uint32_t, std::uint32_t) noexcept
{
}
#endif

GuestWordReader ReaderOf(IRingStubMemory* memory)
{
    return [memory](std::uint32_t address, std::uint32_t& value)
    {
        if (!memory)
            return false;
        const auto word = memory->Read32(address);
        if (!word)
            return false;
        value = *word;
        return true;
    };
}

WindowWriterFallback FromRefusal(WindowWriterRefusal refusal) noexcept
{
    switch (refusal)
    {
        case WindowWriterRefusal::TileCount:
            return WindowWriterFallback::TileCount;
        case WindowWriterRefusal::RingSpace:
            return WindowWriterFallback::RingSpace;
        default:
            return WindowWriterFallback::Unreadable;
    }
}

} // namespace

bool WindowWriterConsumeRequested() noexcept
{
    return RingStubsPublished() && REXCVAR_GET(native_consume_window_writer);
}

#if REREVVED_DEVELOPER
WindowWriterCaller WindowWriterCallerOf(std::uint32_t returnAddress) noexcept
{
    switch (returnAddress)
    {
        case kReturn8268F9C0:
            return WindowWriterCaller::Setter8268F9C0;
        case kReturn82690040:
            return WindowWriterCaller::Setter82690040;
        case kReturn826922D0:
            return WindowWriterCaller::Setter826922D0;
        case kReturn82698720:
            return WindowWriterCaller::Setter82698720;
        case kReturnClear:
            return WindowWriterCaller::ClearBody;
        case kReturnResolveA:
        case kReturnResolveB:
            return WindowWriterCaller::Resolve;
        default:
            return WindowWriterCaller::Other;
    }
}
#endif

std::optional<WindowWriterFallback> WindowWriterConsumeVerdict(const WindowWriterConsumeState& state, const GuestWordReader& read, const WindowWriterEntry& entry, WindowWriterPlan* plan) noexcept
{
    try
    {
        if (!state.stubsReady)
            return WindowWriterFallback::Stubs;
        // A device that wraps its span cannot be placed: unreadable, as in ReadWindowWriterWatch.
        if (entry.g > 0xFFFFFFFFU - kWindowWriterDeviceSpan)
            return WindowWriterFallback::Unreadable;
        // Before any other guest word is read: a skipped Original's saves would change the words below.
        StackReach reach;
        if (WindowWriterStackOverlaps(read, { entry.pcr, entry.sp }, entry.g, reach))
            return WindowWriterFallback::StackOverlap;
        WindowWriterWatch watch;
        if (!ReadWindowWriterWatch(read, entry.g, false, watch))
            return WindowWriterFallback::Unreadable;
        WindowWriterRefusal refusal = WindowWriterRefusal::None;
        const auto          exit    = DeriveWindowWriterExitState(watch, &refusal);
        if (!exit)
            return FromRefusal(refusal);
        if (state.diagnostics)
            return WindowWriterFallback::Diagnostics;
        if (plan)
        {
            plan->entry = entry;
            plan->watch = watch;
            plan->exit  = *exit;
        }
        return std::nullopt;
    }
    catch (...)
    {
        return WindowWriterFallback::Unreadable;
    }
}

bool ConsumeWindowWriter(PPCContext& ctx, IRingStubMemory* memory, bool diagnostics) noexcept
{
    if (!WindowWriterConsumeRequested()) [[likely]]
        return false;
    // Held until the decision (and any guest store) is done; a withdrawal waits for it.
    const RingStubLease lease;
    if (!lease) [[unlikely]]
        return false;
    const WindowWriterEntry  entry{ ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r1.u32, ctx.r13.u32 };
    WindowWriterConsumeState state;
    state.stubsReady = RingStubsReady(entry.g);
    // The probe reads the body's exit state, which a skipped Original never produces.
    state.diagnostics = diagnostics || WindowWriterExitCheckRequested();
    WindowWriterPlan plan;
    const auto       verdict = WindowWriterConsumeVerdict(state, ReaderOf(memory), entry, &plan);
    if (verdict)
    {
        noteFallback(*verdict);
        return false;
    }
    // Nothing is stored: the ring words, the cursor and the stack saves are skipped.
    ctx.r3.u64 = plan.exit.result;
    noteConsumed(static_cast<std::uint32_t>(ctx.lr), plan.exit.words);
    return true;
}

#if REREVVED_DEVELOPER
void NoteWindowWriterOriginal(std::uint32_t before, std::uint32_t after) noexcept
{
    const std::uint32_t delta = after - before;
    if (after < before || delta % 4 || delta / 4 > kWindowWriterMaxWords)
    {
        originalUnbounded.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    originalCount.fetch_add(1, std::memory_order_relaxed);
    ringWordsOriginal.fetch_add(delta / 4, std::memory_order_relaxed);
}

WindowWriterConsumeCounts TakeWindowWriterConsumeCounts() noexcept
{
    WindowWriterConsumeCounts counts;
    counts.consumed          = consumedCount.exchange(0, std::memory_order_relaxed);
    counts.ringWordsConsumed = ringWordsConsumed.exchange(0, std::memory_order_relaxed);
    counts.ringWordsOriginal = ringWordsOriginal.exchange(0, std::memory_order_relaxed);
    counts.original          = originalCount.exchange(0, std::memory_order_relaxed);
    counts.originalUnbounded = originalUnbounded.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kWindowWriterCallerCount; ++i)
        counts.callers[i] = callerCounts[i].exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kWindowWriterFallbackCount; ++i)
        counts.fallback[i] = fallbackCounts[i].exchange(0, std::memory_order_relaxed);
    return counts;
}

std::string FormatWindowWriterConsumeCounts(const WindowWriterConsumeCounts& counts)
{
    std::string fields = std::format(" window_writer_consumed={}", counts.consumed);
    for (std::size_t i = 0; i < kWindowWriterCallerCount; ++i)
        if (counts.callers[i])
            fields += std::format(" window_writer_consumed_{}={}", kWindowWriterCallerNames[i], counts.callers[i]);
    fields += std::format(" window_writer_ring_words_consumed={} window_writer_ring_words_original={} window_writer_original_calls={}", counts.ringWordsConsumed, counts.ringWordsOriginal, counts.original);
    if (counts.originalUnbounded)
        fields += std::format(" window_writer_original_unbounded={}", counts.originalUnbounded);
    for (std::size_t i = 0; i < kWindowWriterFallbackCount; ++i)
        if (counts.fallback[i])
            fields += std::format(" window_writer_fallback_{}={}", kWindowWriterFallbackNames[i], counts.fallback[i]);
    return fields;
}

struct WindowWriterExitProbe::State
{
    WindowWriterEntry          entry;
    WindowWriterWatch          before;
    WindowWriterExitState      predicted;
    std::vector<std::uint32_t> expected;
};

WindowWriterExitProbe::WindowWriterExitProbe(const PPCContext& ctx, IRingStubMemory* guestMemory) noexcept
{
    const bool compare = WindowWriterExitCheckRequested();
    if (!compare && !WindowWriterConsumeRequested()) [[likely]]
        return;
    if (!guestMemory)
        return;
    try
    {
        const auto              read = ReaderOf(guestMemory);
        const WindowWriterEntry entry{ ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r1.u32, ctx.r13.u32 };
        if (!compare)
        {
            // The consume's own Original runs: only the ring words they store are counted.
            if (read(entry.g + 0x30, cursor))
            {
                g          = entry.g;
                memory     = guestMemory;
                observing  = true;
                context    = &ctx;
                exceptions = std::uncaught_exceptions();
            }
            return;
        }
        auto armed   = std::make_unique<State>();
        armed->entry = entry;
        // Before any other guest word is read, as in the consume verdict.
        StackReach reach;
        if (entry.g <= 0xFFFFFFFFU - kWindowWriterDeviceSpan && WindowWriterStackOverlaps(read, { entry.pcr, entry.sp }, entry.g, reach))
        {
            NoteWindowWriterExitUnmodeled(WindowWriterRefusal::StackOverlap);
            return;
        }
        if (!ReadWindowWriterWatch(read, entry.g, true, armed->before))
        {
            NoteWindowWriterExitUnreadable();
            return;
        }
        WindowWriterRefusal refusal = WindowWriterRefusal::None;
        const auto          exit    = DeriveWindowWriterExitState(armed->before, &refusal);
        if (!exit)
        {
            NoteWindowWriterExitUnmodeled(refusal);
            return;
        }
        // The ring words the end of the probe reads, behind the cursor word just read.
        if (WindowWriterRingOverlaps(reach, armed->before.cursor, exit->words))
        {
            NoteWindowWriterExitUnmodeled(WindowWriterRefusal::StackOverlap);
            return;
        }
        armed->predicted = *exit;
        armed->expected  = WindowWriterRingWords(entry, armed->before, *exit);
        state            = std::move(armed);
        memory           = guestMemory;
        context          = &ctx;
        g                = entry.g;
        exceptions       = std::uncaught_exceptions();
    }
    catch (...)
    {
        state.reset();
        observing = false;
    }
}

WindowWriterExitProbe::~WindowWriterExitProbe()
{
    if (!state && !observing)
        return;
    // A call that unwound has no exit to compare.
    if (std::uncaught_exceptions() > exceptions)
        return;
    try
    {
        const auto read = ReaderOf(memory);
        if (!state)
        {
            // Original may have disabled the stubs, or the services may have been
            // withdrawn, since the constructor; then it counts nothing and reads nothing.
            if (!WindowWriterConsumeRequested())
                return;
            std::uint32_t after = 0;
            if (read(g + 0x30, after))
                NoteWindowWriterOriginal(cursor, after);
            return;
        }
        WindowWriterWatch after;
        if (!ReadWindowWriterWatch(read, state->entry.g, true, after))
        {
            NoteWindowWriterExitUnreadable();
            return;
        }
        // The words Original stored after the entry cursor, as many as the model says.
        std::vector<std::uint32_t> observed;
        observed.reserve(state->predicted.words);
        for (std::uint32_t i = 0; i < state->predicted.words; ++i)
        {
            std::uint32_t word = 0;
            if (!read(state->before.cursor + 4 * (i + 1), word))
            {
                NoteWindowWriterExitUnreadable();
                return;
            }
            observed.push_back(word);
        }
        const auto mismatch = WindowWriterExitMismatches(state->predicted, state->before, after, context->r3.u64, state->expected, observed);
        NoteWindowWriterExit(mismatch, state->predicted.form == WindowWriterForm::Tiled, WindowWriterObservedWords(state->before, after));
    }
    catch (...)
    {
    }
}
#endif

} // namespace rerevved::gpu::guest_d3d
