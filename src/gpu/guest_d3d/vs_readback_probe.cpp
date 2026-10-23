#include "vs_readback_probe.h"

#include "guest_memory.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <format>
#include <utility>

REXCVAR_DEFINE_BOOL(native_vs_readback_probe, false, "ReRevved", "Observe the VS readback entries 0x82662D28 and 0x82694480; Original still runs; rb_read_* timing fields")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{
namespace
{

constexpr std::uint64_t kAddressSpace = std::uint64_t{ 1 } << 32;

// Same heap validation as the completion probe's reads: nonzero base, widened
// end, one readable heap, then a guarded copy into owned bytes.
bool readGuest(void*, std::uint32_t address, std::uint8_t* bytes, std::size_t size)
{
    auto* kernel = rex::system::kernel_state();
    if (!kernel || !kernel->memory() || !address || !size || size - 1 > UINT32_MAX - address)
        return false;
    auto*      memory = kernel->memory();
    auto*      heap   = memory->LookupHeap(address);
    const auto last   = address + static_cast<std::uint32_t>(size - 1);
    if (!heap || memory->LookupHeap(last) != heap ||
        !(static_cast<unsigned>(heap->QueryRangeAccess(address, last)) & static_cast<unsigned>(rex::memory::PageAccess::kReadOnly)))
        return false;
    return CopyGuestGuarded(bytes, memory->TranslateVirtual<const std::uint8_t*>(address), size);
}

std::atomic<const VsReadbackAccess*> accessOverride{ nullptr };

VsReadbackAccess resolveAccess(const VsReadbackAccess& named) noexcept
{
    if (named.reader)
        return named;
    if (const auto* installed = accessOverride.load(std::memory_order_acquire))
        if (installed->reader)
            return *installed;
    return { readGuest, nullptr };
}

// Scalar BE32 reads. A failed attempt, including an unusable base, counts once.
struct Sampler
{
    VsReadbackAccess access;
    std::uint32_t    unreadable = 0;
    std::uint64_t    bytes      = 0;

    std::optional<std::uint32_t> Word(std::uint32_t base, std::uint32_t offset) noexcept
    {
        if (!base || offset > UINT32_MAX - base || base + offset > UINT32_MAX - 3 || !access.reader)
        {
            ++unreadable;
            return std::nullopt;
        }
        std::array<std::uint8_t, 4> raw{};
        bool                        ok = false;
        try
        {
            ok = access.reader(access.context, base + offset, raw.data(), raw.size());
        }
        catch (...)
        {
            ok = false;
        }
        if (!ok)
        {
            ++unreadable;
            return std::nullopt;
        }
        bytes += raw.size();
        return (std::uint32_t(raw[0]) << 24) | (std::uint32_t(raw[1]) << 16) | (std::uint32_t(raw[2]) << 8) | raw[3];
    }

    void Take(std::array<std::uint32_t, 4>& words, std::uint32_t& valid, std::size_t index, std::optional<std::uint32_t> value) noexcept
    {
        if (!value)
            return;
        words[index] = *value;
        valid |= 1U << index;
    }
};

VsReadbackInterval interval(bool baseKnown, std::uint64_t base, bool lengthKnown, std::uint64_t length, bool& arithmetic) noexcept
{
    VsReadbackInterval out;
    if (!lengthKnown)
        return out;
    if (length == 0)
    {
        out.begin = baseKnown ? base : 0;
        out.valid = true;
        return out;
    }
    if (!baseKnown)
        return out;
    if (base == 0 || base + length > kAddressSpace)
    {
        arithmetic = true;
        return out;
    }
    out.begin  = base;
    out.length = length;
    out.valid  = true;
    return out;
}

bool intersects(const VsReadbackInterval& a, const VsReadbackInterval& b) noexcept
{
    return a.valid && b.valid && a.length && b.length && a.begin < b.begin + b.length && b.begin < a.begin + a.length;
}

constexpr std::size_t index(VsReadbackKind kind) noexcept
{
    return static_cast<std::size_t>(kind);
}

constexpr std::array<std::string_view, kVsReadbackKinds>                                        kindNames{ "refresh", "query", "copy" };
constexpr std::array<std::string_view, 3>                                                       parentNames{ "in_refresh", "outside_refresh", "parent_unknown" };
constexpr std::array<std::string_view, static_cast<std::size_t>(VsReadbackParentReason::Count)> reasonNames{
    "none", "no_active_refresh", "not_nested", "thread_unknown", "lr_mismatch", "vs_unknown", "vs_mismatch", "untracked"
};

void add(VsReadbackCounts& a, const VsReadbackCounts& b) noexcept
{
    for (std::size_t i = 0; i < kVsReadbackKinds; ++i)
    {
        a.enter[i] += b.enter[i];
        a.returned[i] += b.returned[i];
        a.abandon[i] += b.abandon[i];
    }
    for (std::size_t i = 0; i < 3; ++i)
    {
        a.refreshCaller[i] += b.refreshCaller[i];
        a.refreshQ[i] += b.refreshQ[i];
        a.queryParent[i] += b.queryParent[i];
        a.copyParent[i] += b.copyParent[i];
    }
    a.refreshNegativeResult += b.refreshNegativeResult;
    a.querySizeValid += b.querySizeValid;
    a.copyHeaderValid += b.copyHeaderValid;
    a.requestedMetaBytes += b.requestedMetaBytes;
    a.requestedCodeBytes += b.requestedCodeBytes;
    a.probeBytes += b.probeBytes;
    a.unreadable += b.unreadable;
    a.rangeOverflow += b.rangeOverflow;
    a.overlap += b.overlap;
    a.pairError += b.pairError;
    a.correlationDrop += b.correlationDrop;
    a.eventDrop += b.eventDrop;
    a.reportingFailures += b.reportingFailures;
    a.finalSkipped += b.finalSkipped;
}

} // namespace

std::atomic<std::int8_t> vsReadbackProbeState{ -1 };

bool RefreshVsReadbackProbeRequested() noexcept
{
    const bool requested = REXCVAR_GET(native_vs_readback_probe);
    vsReadbackProbeState.store(requested ? 1 : 0, std::memory_order_relaxed);
    return requested;
}

VsReadbackKind VsReadbackKindOf(const VsReadbackEntry& entry) noexcept
{
    if (entry.function == VsReadbackFunction::Refresh)
        return VsReadbackKind::Refresh;
    return entry.destination == 0 ? VsReadbackKind::Query : VsReadbackKind::Copy;
}

VsReadbackCaller VsReadbackRefreshCaller(std::uint64_t lr) noexcept
{
    if (lr == kVsReadbackRefreshPrepLr)
        return VsReadbackCaller::Prep;
    if (lr == kVsReadbackRefreshDescriptorLr)
        return VsReadbackCaller::Descriptor;
    return VsReadbackCaller::Other;
}

VsReadbackExtents AnalyzeVsReadbackCopy(const VsReadbackEntry& entry) noexcept
{
    VsReadbackExtents out;
    const bool        haveM = (entry.valid & kVsReadbackM) != 0, haveS = (entry.valid & kVsReadbackS) != 0, haveC = (entry.valid & kVsReadbackC) != 0;
    const auto        m = entry.words[0], s = entry.words[1], c = entry.words[2];
    out.headerComplete = haveM && haveS && haveC;
    out.sumWide        = std::uint64_t{ m } + s;
    out.sum32          = static_cast<std::uint32_t>(out.sumWide);
    bool arithmetic    = false;
    out.metadata       = interval(true, std::uint64_t{ entry.vs } + 0x368, haveM, m, arithmetic);
    out.code           = interval(haveC, c, haveS, s, arithmetic);
    out.output         = interval(true, entry.destination, haveM && haveS, out.sumWide, arithmetic);
    // The size word is optional and the copy never touches it.
    if (entry.sizePointer)
        out.sizeWord = interval(true, entry.sizePointer, true, 4, arithmetic);
    out.rangeInvalid = arithmetic;
    out.headerValid  = out.headerComplete && out.metadata.valid && out.code.valid && out.output.valid;
    if (intersects(out.output, out.code))
        out.overlap |= kVsReadbackOverlapOutputCode;
    if (intersects(out.output, out.metadata))
        out.overlap |= kVsReadbackOverlapOutputMeta;
    if (intersects(out.output, out.sizeWord))
        out.overlap |= kVsReadbackOverlapOutputSize;
    if (intersects(out.code, out.metadata))
        out.overlap |= kVsReadbackOverlapCodeMeta;
    if (intersects(out.code, out.sizeWord))
        out.overlap |= kVsReadbackOverlapCodeSize;
    if (intersects(out.metadata, out.sizeWord))
        out.overlap |= kVsReadbackOverlapMetaSize;
    return out;
}

void VsReadbackProbe::Classify(VsReadbackToken& token) noexcept
{
    const auto& entry = token.entry;
    const auto  site  = token.kind == VsReadbackKind::Query ? kVsReadbackQueryLr : kVsReadbackCopyLr;
    token.parent      = VsReadbackParent::Unknown;
    if (entry.thread == 0)
    {
        token.reason = VsReadbackParentReason::ThreadUnknown;
        return;
    }
    // An untracked refresh could be this call's parent: no association is proved.
    if (untracked_ != 0)
    {
        token.reason = VsReadbackParentReason::Untracked;
        return;
    }
    Slot*       nearest      = nullptr;
    std::size_t nearestIndex = 0;
    bool        sameThread   = false;
    for (std::size_t i = 0; i < slots_.size(); ++i)
    {
        auto& slot = slots_[i];
        if (!slot.active || slot.thread != entry.thread)
            continue;
        sameThread = true;
        if (slot.sp > entry.sp && (!nearest || slot.sp < nearest->sp))
        {
            nearest      = &slot;
            nearestIndex = i;
        }
    }
    if (!nearest)
    {
        token.parent = sameThread ? VsReadbackParent::Unknown : VsReadbackParent::OutsideRefresh;
        token.reason = sameThread ? VsReadbackParentReason::NotNested : VsReadbackParentReason::NoActiveRefresh;
        return;
    }
    if (entry.lr != site)
        token.reason = VsReadbackParentReason::LrMismatch;
    else if (!nearest->vsValid || nearest->vs == 0 || entry.vs == 0)
        token.reason = VsReadbackParentReason::VsUnknown;
    else if (nearest->vs != entry.vs)
        token.reason = VsReadbackParentReason::VsMismatch;
    else
    {
        token.parent     = VsReadbackParent::InRefresh;
        token.reason     = VsReadbackParentReason::None;
        token.parentCall = nearest->call;
        token.parentSlot = nearestIndex;
        ++nearest->children;
    }
}

VsReadbackToken VsReadbackProbe::Begin(const VsReadbackEntry& entry) noexcept
{
    VsReadbackToken token;
    token.entry = entry;
    token.kind  = VsReadbackKindOf(entry);
    if (token.kind == VsReadbackKind::Copy)
        token.extents = AnalyzeVsReadbackCopy(entry);
    try
    {
        std::lock_guard lock(mutex_);
        const auto      k = index(token.kind);
        token.call        = ++call_;
        ++counts_.enter[k];
        ++enterTotal_[k];
        ++pending_[k];
        token.counted = true;
        counts_.unreadable += entry.unreadable;
        counts_.probeBytes += entry.probeBytes;
        if (entry.unreadable)
            token.quality |= kVsReadbackQualityUnreadable;
        if (token.kind == VsReadbackKind::Refresh)
        {
            ++counts_.refreshCaller[static_cast<std::size_t>(VsReadbackRefreshCaller(entry.lr))];
            const bool known = (entry.valid & kVsReadbackQFlags) != 0;
            ++counts_.refreshQ[static_cast<std::size_t>(!known ? VsReadbackQState::Unknown : (entry.words[2] & kVsReadbackQDirty) ? VsReadbackQState::Dirty
                                                                                                                                  : VsReadbackQState::Clean)];
            const auto free = std::find_if(slots_.begin(), slots_.end(), [](const Slot& slot)
                                           {
                                               return !slot.active;
                                           });
            if (free == slots_.end())
            {
                ++counts_.correlationDrop;
                ++untracked_;
                token.quality |= kVsReadbackQualityCorrelationDrop;
            }
            else
            {
                *free         = { true, token.call, entry.sp, entry.thread, (entry.valid & kVsReadbackQVs) ? entry.words[0] : 0, 0, (entry.valid & kVsReadbackQVs) != 0 };
                token.slot    = static_cast<std::size_t>(free - slots_.begin());
                token.tracked = true;
            }
        }
        else
        {
            Classify(token);
            ++(token.kind == VsReadbackKind::Query ? counts_.queryParent : counts_.copyParent)[static_cast<std::size_t>(token.parent)];
            if (token.parent == VsReadbackParent::Unknown)
                token.quality |= kVsReadbackQualityParentUnknown;
            if (token.kind == VsReadbackKind::Copy)
            {
                if (token.extents.headerValid)
                {
                    ++counts_.copyHeaderValid;
                    counts_.requestedMetaBytes += entry.words[0];
                    counts_.requestedCodeBytes += entry.words[1];
                }
                if (token.extents.rangeInvalid)
                {
                    ++counts_.rangeOverflow;
                    token.quality |= kVsReadbackQualityRange;
                }
                if (token.extents.overlap)
                {
                    ++counts_.overlap;
                    token.quality |= kVsReadbackQualityOverlap;
                }
            }
        }
    }
    catch (...)
    {
    }
    return token;
}

void VsReadbackProbe::Finish(VsReadbackToken& token, const VsReadbackExit* exit, VsReadbackTerminal terminal) noexcept
{
    try
    {
        std::lock_guard lock(mutex_);
        if (token.done)
        {
            // A second terminal event for one call changes no total.
            ++counts_.pairError;
            return;
        }
        token.done = true;
        if (!token.counted)
            return;
        const auto k         = index(token.kind);
        bool       pairError = false;
        if (terminal == VsReadbackTerminal::Returned)
        {
            ++counts_.returned[k];
            ++returnTotal_[k];
        }
        else
        {
            ++counts_.abandon[k];
            ++abandonTotal_[k];
        }
        if (pending_[k] == 0)
            pairError = true;
        else
            --pending_[k];
        VsReadbackExit sampled;
        if (exit)
        {
            sampled = *exit;
            if (exit->thread != token.entry.thread)
                pairError = true;
            counts_.unreadable += exit->unreadable;
            counts_.probeBytes += exit->probeBytes;
            if (exit->unreadable)
                token.quality |= kVsReadbackQualityUnreadable;
            if (terminal == VsReadbackTerminal::Returned && token.kind == VsReadbackKind::Refresh && static_cast<std::int32_t>(static_cast<std::uint32_t>(exit->result)) < 0)
                ++counts_.refreshNegativeResult;
            if (terminal == VsReadbackTerminal::Returned && token.kind == VsReadbackKind::Query && (exit->valid & kVsReadbackSizeWord))
                ++counts_.querySizeValid;
        }
        if (token.kind == VsReadbackKind::Refresh)
        {
            if (token.tracked && token.slot < slots_.size())
            {
                auto& slot = slots_[token.slot];
                if (!slot.active || slot.call != token.call)
                    pairError = true;
                else
                {
                    // A parent that finishes with a child still pending is a parent unwind.
                    if (slot.children != 0)
                        pairError = true;
                    slot = {};
                }
            }
            else if (!token.tracked)
            {
                if (untracked_ == 0)
                    pairError = true;
                else
                    --untracked_;
            }
        }
        else if (token.parent == VsReadbackParent::InRefresh)
        {
            auto& slot = slots_[token.parentSlot];
            if (!slot.active || slot.call != token.parentCall || slot.children == 0)
                pairError = true;
            else
                --slot.children;
        }
        if (pairError)
        {
            ++counts_.pairError;
            token.quality |= kVsReadbackQualityPairError;
        }
        if (detailCount_ < details_.size())
        {
            auto& detail      = details_[detailCount_++];
            detail.kind       = token.kind;
            detail.terminal   = terminal;
            detail.parent     = token.parent;
            detail.reason     = token.reason;
            detail.call       = token.call;
            detail.parentCall = token.parentCall;
            detail.entry      = token.entry;
            detail.exit       = sampled;
            detail.extents    = token.extents;
            detail.quality    = token.quality;
        }
        else
            ++counts_.eventDrop;
    }
    catch (...)
    {
    }
}

void VsReadbackProbe::End(VsReadbackToken& token, const VsReadbackExit& exit) noexcept
{
    Finish(token, &exit, VsReadbackTerminal::Returned);
}

void VsReadbackProbe::Abandon(VsReadbackToken& token) noexcept
{
    Finish(token, nullptr, VsReadbackTerminal::Abandoned);
}

VsReadbackReport VsReadbackProbe::Take(bool shutdown) noexcept
{
    VsReadbackReport report;
    try
    {
        std::unique_lock lock(mutex_, std::defer_lock);
        if (shutdown)
        {
            // A terminated guest writer may still own the lock.
            if (!lock.try_lock_for(kVsReadbackShutdownWait))
            {
                report.complete = false;
                return report;
            }
        }
        else
            lock.lock();
        report.counts = counts_;
        add(report.counts, retained_);
        report.counts.reportingFailures += reportingFailures_.exchange(0, std::memory_order_relaxed);
        report.counts.finalSkipped += finalSkipped_.exchange(0, std::memory_order_relaxed);
        counts_   = {};
        retained_ = {};
        for (std::size_t i = 0; i < kVsReadbackKinds; ++i)
        {
            report.enterTotal[i]   = enterTotal_[i];
            report.returnTotal[i]  = returnTotal_[i];
            report.abandonTotal[i] = abandonTotal_[i];
            report.pending[i]      = pending_[i];
        }
        std::copy_n(details_.begin(), detailCount_, report.details.begin());
        report.detailCount = detailCount_;
        detailCount_       = 0;
        for (const auto& slot : slots_)
            if (slot.active)
                report.pendingRefresh[report.pendingRefreshCount++] = { slot.call, slot.sp, slot.thread, slot.vs, slot.vsValid };
        report.untrackedRefresh = untracked_;
    }
    catch (...)
    {
        report.complete = false;
    }
    return report;
}

bool VsReadbackProbe::Retain(const VsReadbackCounts& counts, bool bounded) noexcept
{
    try
    {
        std::unique_lock lock(mutex_, std::defer_lock);
        if (bounded)
        {
            // A terminated guest writer may still own the lock.
            if (!lock.try_lock_for(kVsReadbackShutdownWait))
                return false;
        }
        else
            lock.lock();
        add(retained_, counts);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void VsReadbackProbe::NoteReportingFailure() noexcept
{
    reportingFailures_.fetch_add(1, std::memory_order_relaxed);
}

void VsReadbackProbe::NoteFinalSkipped() noexcept
{
    finalSkipped_.fetch_add(1, std::memory_order_relaxed);
    reportingFailures_.fetch_add(1, std::memory_order_relaxed);
}

VsReadbackProbe& PublishedVsReadbackProbe() noexcept
{
    static VsReadbackProbe probe;
    return probe;
}

void SetVsReadbackAccess(const VsReadbackAccess* access) noexcept
{
    accessOverride.store(access, std::memory_order_release);
}

VsReadbackToken BeginVsReadbackProbe(VsReadbackProbe& probe, VsReadbackFunction function, const PPCContext& context, const VsReadbackAccess& named) noexcept
{
    VsReadbackEntry entry;
    entry.function = function;
    entry.thread   = context.r13.u32;
    entry.sp       = context.r1.u64;
    entry.lr       = context.lr;
    entry.r3       = context.r3.u64;
    entry.r4       = context.r4.u64;
    entry.r5       = context.r5.u64;
    try
    {
        Sampler sampler{ resolveAccess(named) };
        if (function == VsReadbackFunction::Refresh)
        {
            entry.object = context.r3.u32;
            entry.q      = context.r4.u32;
            // The refresh clears Qdirty before its size query, so Q is sampled now.
            sampler.Take(entry.words, entry.valid, 0, sampler.Word(entry.q, 0x04));
            sampler.Take(entry.words, entry.valid, 1, sampler.Word(entry.q, 0x08));
            sampler.Take(entry.words, entry.valid, 2, sampler.Word(entry.q, 0x14));
            sampler.Take(entry.words, entry.valid, 3, sampler.Word(entry.object, 0x0C));
        }
        else
        {
            entry.vs          = context.r3.u32;
            entry.destination = context.r4.u32;
            entry.sizePointer = context.r5.u32;
            sampler.Take(entry.words, entry.valid, 0, sampler.Word(entry.vs, 0x36C));
            sampler.Take(entry.words, entry.valid, 1, sampler.Word(entry.vs, 0x370));
            // Only a copy reads the code pointer; a query reads no code or output.
            if (entry.destination != 0)
                sampler.Take(entry.words, entry.valid, 2, sampler.Word(entry.vs, 0x20));
        }
        entry.unreadable = sampler.unreadable;
        entry.probeBytes = sampler.bytes;
    }
    catch (...)
    {
    }
    return probe.Begin(entry);
}

void EndVsReadbackProbe(VsReadbackProbe& probe, VsReadbackToken& token, const PPCContext& context, const VsReadbackAccess& named) noexcept
{
    VsReadbackExit exit;
    exit.result = context.r3.u64;
    exit.thread = context.r13.u32;
    try
    {
        Sampler     sampler{ resolveAccess(named) };
        const auto& entry = token.entry;
        if (token.kind == VsReadbackKind::Refresh)
        {
            // The saved Q and owner, never the return registers.
            sampler.Take(exit.words, exit.valid, 0, sampler.Word(entry.q, 0x04));
            sampler.Take(exit.words, exit.valid, 1, sampler.Word(entry.q, 0x08));
            sampler.Take(exit.words, exit.valid, 2, sampler.Word(entry.q, 0x14));
            sampler.Take(exit.words, exit.valid, 3, sampler.Word(entry.object, 0x0C));
        }
        else if (token.kind == VsReadbackKind::Query)
            sampler.Take(exit.words, exit.valid, 0, sampler.Word(entry.sizePointer, 0));
        exit.unreadable = sampler.unreadable;
        exit.probeBytes = sampler.bytes;
    }
    catch (...)
    {
    }
    probe.End(token, exit);
}

void AbandonVsReadbackProbe(VsReadbackProbe& probe, VsReadbackToken& token) noexcept
{
    probe.Abandon(token);
}

VsReadbackReport TakeVsReadbackReport(bool shutdown) noexcept
{
    return PublishedVsReadbackProbe().Take(shutdown);
}

VsReadbackScope::VsReadbackScope(VsReadbackFunction function, const PPCContext& context) noexcept
: probe_(&PublishedVsReadbackProbe())
, token_(BeginVsReadbackProbe(*probe_, function, context))
{
}

VsReadbackScope::~VsReadbackScope()
{
    if (!token_.done)
        AbandonVsReadbackProbe(*probe_, token_);
}

void VsReadbackScope::End(const PPCContext& context) noexcept
{
    if (!token_.done)
        EndVsReadbackProbe(*probe_, token_, context);
}

namespace
{

std::string formatWords(const VsReadbackEntry& entry, const std::array<std::uint32_t, 4>& words, std::uint32_t valid, bool refresh, bool exit, bool query)
{
    std::string out;
    if (refresh)
    {
        constexpr std::array<std::string_view, 4> names{ "q_vs", "q_buffer", "q_flags", "device" };
        for (std::size_t i = 0; i < names.size(); ++i)
            if (valid & (1U << i))
                out += std::format(" {}:{:08X}", names[i], words[i]);
    }
    else if (exit)
    {
        if (query && (valid & kVsReadbackSizeWord))
            out += std::format(" size:{:08X}", words[0]);
    }
    else
    {
        constexpr std::array<std::string_view, 3> names{ "m", "s", "c" };
        for (std::size_t i = 0; i < names.size(); ++i)
            if (valid & (1U << i))
                out += std::format(" {}:{:08X}", names[i], words[i]);
    }
    (void)entry;
    return out;
}

std::string formatDetail(const VsReadbackDetail& detail)
{
    const auto& e       = detail.entry;
    const auto& x       = detail.exit;
    const bool  refresh = detail.kind == VsReadbackKind::Refresh;
    const bool  query   = detail.kind == VsReadbackKind::Query;
    const auto  caller  = refresh ? std::array<std::string_view, 3>{ "prep", "descriptor", "other" }[static_cast<std::size_t>(VsReadbackRefreshCaller(e.lr))] : std::string_view("na");
    std::string line    = std::format("kind={} call={} parent_call={} parent={} parent_reason={} terminal={} caller={} thread={:08X} sp={:016X} lr={:016X} r3={:016X} r4={:016X} r5={:016X} result={:016X} object={:08X} q={:08X} vs={:08X} destination={:08X} size_pointer={:08X} entry_valid={:X} exit_valid={:X} entry=[{}] exit=[{}] quality={:X} unreadable={} probe_bytes={} generation=unknown outer_activation=unknown",
                                      kindNames[index(detail.kind)],
                                      detail.call,
                                      detail.parentCall,
                                      refresh ? std::string_view("na") : parentNames[static_cast<std::size_t>(detail.parent)],
                                      refresh ? std::string_view("na") : reasonNames[static_cast<std::size_t>(detail.reason)],
                                      detail.terminal == VsReadbackTerminal::Returned ? "return" : "abandon",
                                      caller,
                                      e.thread,
                                      e.sp,
                                      e.lr,
                                      e.r3,
                                      e.r4,
                                      e.r5,
                                      x.result,
                                      e.object,
                                      e.q,
                                      e.vs,
                                      e.destination,
                                      e.sizePointer,
                                      e.valid,
                                      x.valid,
                                      formatWords(e, e.words, e.valid, refresh, false, query),
                                      formatWords(e, x.words, x.valid, refresh, true, query),
                                      detail.quality,
                                      e.unreadable + x.unreadable,
                                      e.probeBytes + x.probeBytes);
    if (detail.kind == VsReadbackKind::Copy)
    {
        const auto& g = detail.extents;
        line += std::format(" header_complete={} header_valid={} sum32={:08X} sum_wide={:X} range_invalid={} overlap={:X} meta=[{:X},{:X},{}] code=[{:X},{:X},{}] output=[{:X},{:X},{}] size_word=[{:X},{:X},{}]",
                            g.headerComplete ? 1 : 0,
                            g.headerValid ? 1 : 0,
                            g.sum32,
                            g.sumWide,
                            g.rangeInvalid ? 1 : 0,
                            g.overlap,
                            g.metadata.begin,
                            g.metadata.length,
                            g.metadata.valid ? 1 : 0,
                            g.code.begin,
                            g.code.length,
                            g.code.valid ? 1 : 0,
                            g.output.begin,
                            g.output.length,
                            g.output.valid ? 1 : 0,
                            g.sizeWord.begin,
                            g.sizeWord.length,
                            g.sizeWord.valid ? 1 : 0);
    }
    return line;
}

void logLine(const VsReadbackReportOperations& operations, bool detail, std::string_view line)
{
    if (operations.log)
        operations.log(detail, line);
    else
        REXLOG_INFO("{}", line);
}

constexpr std::string_view kFinalIncomplete = " rb_read_final_incomplete=1 rb_read_final_skipped=1";

} // namespace

std::string FormatVsReadbackCounts(const VsReadbackReport& report)
{
    const auto& c = report.counts;
    std::string fields;
    bool        conserved = true;
    for (std::size_t k = 0; k < kVsReadbackKinds; ++k)
    {
        const auto name = kindNames[k];
        fields += std::format(" rb_read_{0}_enter={1} rb_read_{0}_return={2} rb_read_{0}_abandon={3} rb_read_{0}_pending={4} rb_read_{0}_enter_total={5} rb_read_{0}_return_total={6} rb_read_{0}_abandon_total={7}",
                              name,
                              c.enter[k],
                              c.returned[k],
                              c.abandon[k],
                              report.pending[k],
                              report.enterTotal[k],
                              report.returnTotal[k],
                              report.abandonTotal[k]);
        conserved = conserved && report.enterTotal[k] == report.returnTotal[k] + report.abandonTotal[k] + report.pending[k];
    }
    fields += std::format(" rb_read_refresh_prep={} rb_read_refresh_descriptor={} rb_read_refresh_other={} rb_read_refresh_dirty={} rb_read_refresh_clean={} rb_read_refresh_q_unknown={} rb_read_refresh_negative_result={}",
                          c.refreshCaller[0],
                          c.refreshCaller[1],
                          c.refreshCaller[2],
                          c.refreshQ[0],
                          c.refreshQ[1],
                          c.refreshQ[2],
                          c.refreshNegativeResult);
    fields += std::format(" rb_read_query_in_refresh={} rb_read_query_outside_refresh={} rb_read_query_parent_unknown={} rb_read_copy_in_refresh={} rb_read_copy_outside_refresh={} rb_read_copy_parent_unknown={}",
                          c.queryParent[0],
                          c.queryParent[1],
                          c.queryParent[2],
                          c.copyParent[0],
                          c.copyParent[1],
                          c.copyParent[2]);
    fields += std::format(" rb_read_query_size_valid={} rb_read_copy_header_valid={} rb_read_requested_meta_bytes={} rb_read_requested_code_bytes={} rb_read_probe_bytes={}",
                          c.querySizeValid,
                          c.copyHeaderValid,
                          c.requestedMetaBytes,
                          c.requestedCodeBytes,
                          c.probeBytes);
    fields += std::format(" rb_read_unreadable={} rb_read_range_overflow={} rb_read_overlap={} rb_read_pair_error={} rb_read_correlation_drop={} rb_read_event_drop={}",
                          c.unreadable,
                          c.rangeOverflow,
                          c.overlap,
                          c.pairError,
                          c.correlationDrop,
                          c.eventDrop);
    fields += std::format(" rb_read_untracked_refresh={} rb_read_conserved={} rb_read_reporting_failures={} rb_read_final_skipped={} rb_read_final_incomplete={}",
                          report.untrackedRefresh,
                          conserved ? 1 : 0,
                          c.reportingFailures,
                          c.finalSkipped,
                          c.finalSkipped ? 1 : 0);
    return fields;
}

std::string ReportVsReadbackProbe(bool shutdown, const VsReadbackReportOperations& operations) noexcept
{
    if (!VsReadbackProbeRequested())
        return {};
    auto& probe = operations.probe ? *operations.probe : PublishedVsReadbackProbe();
    try
    {
        static std::timed_mutex reportMutex;
        std::unique_lock        lock(reportMutex, std::defer_lock);
        if (shutdown)
        {
            if (!lock.try_lock_for(kVsReadbackShutdownWait))
            {
                probe.NoteFinalSkipped();
                try
                {
                    logLine(operations, false, std::format("Native VS readback probe: shutdown{}", kFinalIncomplete));
                }
                catch (...)
                {
                }
                return std::string(kFinalIncomplete);
            }
        }
        else
            lock.lock();
        auto report = probe.Take(shutdown);
        if (!report.complete)
        {
            probe.NoteFinalSkipped();
            try
            {
                logLine(operations, false, std::format("Native VS readback probe: shutdown{}", kFinalIncomplete));
            }
            catch (...)
            {
            }
            return std::string(kFinalIncomplete);
        }
        // Formatting and logging occur here, never at a guest observation. Lost
        // records are counted into this report, which is not formatted yet.
        std::uint64_t logFailures = 0;
        for (std::size_t i = 0; i < report.detailCount; ++i)
        {
            try
            {
                logLine(operations, true, "Native VS readback probe: " + formatDetail(report.details[i]));
            }
            catch (...)
            {
                ++logFailures;
            }
        }
        if (shutdown)
        {
            for (std::size_t i = 0; i < report.pendingRefreshCount; ++i)
            {
                const auto& pending = report.pendingRefresh[i];
                try
                {
                    logLine(operations, true, std::format("Native VS readback probe: kind=refresh call={} terminal=pending thread={:08X} sp={:016X} vs={:08X} vs_valid={} generation=unknown outer_activation=unknown", pending.call, pending.thread, pending.sp, pending.vs, pending.vsValid ? 1 : 0));
                }
                catch (...)
                {
                    ++logFailures;
                }
            }
        }
        report.counts.reportingFailures += logFailures;
        std::string fields;
        try
        {
            fields = FormatVsReadbackCounts(report);
            if (shutdown)
                logLine(operations, false, std::format("Native VS readback probe: shutdown{}", fields));
        }
        catch (...)
        {
            // The interval counts survive for the next drain.
            const bool kept = probe.Retain(report.counts, shutdown);
            probe.NoteReportingFailure();
            if (shutdown && !kept)
            {
                // The final batch could not be kept in time: mark the line incomplete.
                probe.NoteFinalSkipped();
                try
                {
                    logLine(operations, false, std::format("Native VS readback probe: shutdown{}", kFinalIncomplete));
                }
                catch (...)
                {
                }
                return std::string(kFinalIncomplete);
            }
            return {};
        }
        return fields;
    }
    catch (...)
    {
        probe.NoteReportingFailure();
    }
    return {};
}

} // namespace rerevved::gpu::guest_d3d
