#include "completion_ledger.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace rerevved::gpu::completion
{
namespace
{

using guest_d3d::NativeDrawHook;
using guest_d3d::NativeDrawMode;
using guest_d3d::NativeDrawReason;

struct Record
{
    ProgressKind   kind     = ProgressKind::Unmodeled;
    std::uint32_t  address  = 0; // emitter, call site or draw caller
    std::uint32_t  target   = 0; // physical block or word
    std::uint32_t  value    = 0; // fence, callback, scratch value, wait reference or resolve destination
    std::uint32_t  extra    = 0; // fence cursor or callback parameter
    std::uint32_t  flags    = 0; // normalized 0x8269BF40 flags
    std::uint32_t  mask     = 0; // callback CPU mask
    std::uint64_t  sequence = 0; // dispatcher draw sequence
    NativeDrawHook hook     = NativeDrawHook::IndexedBuffer;
    bool           pending  = false; // a draw without its verdict, or a resolve awaiting its swap
    bool           skipped  = false; // census mode: unowned, so its guest effect is not applied
};

struct Span
{
    std::uint64_t                  id = 0;
    std::vector<Record>            records;
    std::optional<std::uint32_t>   writePointer;   // set by the kick that closes the span
    bool                           sealed = false; // closed by a Silent submit: retires on host completion, no RPTR
    std::optional<std::uint32_t>   deferred;       // physical segment the worker must submit first
    std::uint32_t                  deferredDwords = 0;
    std::uint64_t                  kickOrder      = 0; // order of the kick that set writePointer
    std::optional<std::uint64_t>   startsList;         // the tiled list its 0x826AC32C callback starts
    std::optional<std::thread::id> thread;
    std::uint32_t                  pending = 0;
    std::string                    unowned;
    std::size_t                    unownedRecord = 0;
    bool                           partial       = false;
    std::uint64_t                  lastAdmitted = 0, hostTarget = 0;
    bool                           flushed = false;
    std::size_t                    applied = 0;
    std::string                    flushWait; // why the last flush asked to be retried
    std::uint8_t                   stage = 0; // callback progress within records[applied]
};

// A submit waiting for the 0x8269B900 kick that follows it on this thread.
struct PendingSubmit
{
    std::uint32_t physical = 0, dwords = 0, site = 0;
    std::uint64_t epoch  = 0;
    bool          worker = false;
};

// Per-thread worker state, keyed by ledger. A thread_local dies with its
// thread, so a later thread that reuses a host thread id inherits none of it.
struct ThreadMarks
{
    std::set<std::uint64_t>                replay; // ledgers that saw this thread replay
    std::map<std::uint64_t, PendingSubmit> pending;
};

thread_local ThreadMarks threadMarks;

std::atomic<std::uint64_t> nextLedgerId{ 1 };

std::string describe(const Record& record)
{
    if (record.kind == ProgressKind::Draw)
        return std::format("draw {} from {:#010x}", guest_d3d::NativeDrawHookName(record.hook), record.address);
    return std::format("{} at {:#010x}", ProgressKindName(record.kind), record.address);
}

// Publishes a resolve image: whole when it has no runs, otherwise one write per
// run so destination padding keeps its old bytes. Runs are checked before the
// first write; a failing write stops the rest, and earlier runs stay written.
bool writeGuestImage(IGuestProgressSink& sink, const guest_d3d::GuestResolveImage& image)
{
    if (image.runs.empty())
        return sink.WriteGuestImage(image.physical, image.bytes);
    for (const auto& run : image.runs)
    {
        if (!run.length || run.offset > image.bytes.size() || run.length > image.bytes.size() - run.offset ||
            image.physical > UINT32_MAX - run.offset)
            return false;
    }
    for (const auto& run : image.runs)
    {
        if (!sink.WriteGuestImage(image.physical + run.offset, std::span(image.bytes).subspan(run.offset, run.length)))
            return false;
    }
    return true;
}

// A census form's key. Decimal counters after these words differ per
// operation or epoch, so they collapse; every other text stays significant.
std::string censusKey(const LedgerFailure& failure)
{
    static constexpr std::string_view kCounters[] = { "op mark ", "discard point ", "epoch ", "recorded ", "through " };
    auto                              key         = std::format("{}|{:08X}|", ProgressKindName(failure.kind), failure.address);
    const std::string_view            detail      = failure.detail;
    for (std::size_t i = 0; i < detail.size();)
    {
        key += detail[i++];
        if (i < detail.size() && detail[i] >= '0' && detail[i] <= '9' && std::any_of(std::begin(kCounters), std::end(kCounters), [&](std::string_view counter)
                                                                                     {
                                                                                         return key.ends_with(counter);
                                                                                     }))
        {
            while (i < detail.size() && detail[i] >= '0' && detail[i] <= '9')
                ++i;
            key += '#';
        }
    }
    return key;
}

} // namespace

const char* ProgressKindName(ProgressKind kind) noexcept
{
    switch (kind)
    {
        case ProgressKind::InvalidateState:
            return "invalidate_state";
        case ProgressKind::Fence:
            return "fence";
        case ProgressKind::Callback:
            return "callback";
        case ProgressKind::ScratchWrite:
            return "scratch_write";
        case ProgressKind::GpuWait:
            return "gpu_wait";
        case ProgressKind::Draw:
            return "draw";
        case ProgressKind::Clear:
            return "clear";
        case ProgressKind::Resolve:
            return "resolve";
        case ProgressKind::Unmodeled:
            return "unmodeled";
        case ProgressKind::Swap:
            return "swap";
        case ProgressKind::Barrier:
            return "barrier";
    }
    return "unknown";
}

struct CompletionLedger::State
{
    struct FrontbufferResolve
    {
        std::uint64_t span        = 0;
        std::size_t   index       = 0;
        std::uint32_t destination = 0;
    };

    LedgerConfig                                                     config;
    mutable std::mutex                                               mutex;
    std::mutex                                                       imageWrites;
    std::mutex                                                       pumping;
    std::map<std::thread::id, std::pair<std::uint64_t, std::size_t>> openResolves;
    std::deque<Span>                                                 spans; // back() is the open span
    std::uint64_t                                                    nextSpan = 1;
    std::map<std::thread::id, std::pair<std::uint64_t, std::size_t>> openDraws;
    std::vector<FrontbufferResolve>                                  frontbuffer;
    std::uint32_t                                                    writeback    = 0;
    bool                                                             closed       = false;
    bool                                                             epochPartial = false;
    std::optional<LedgerFailure>                                     failure;
    // The failing record's span; it and every later span never release.
    std::optional<std::uint64_t> failedSpan;
    LedgerSnapshot               stats;
    // Census forms in first-seen order, their index by key, and forms not yet reported.
    std::vector<CensusForm>            census;
    std::map<std::string, std::size_t> censusIndex;
    std::vector<LedgerFailure>         censusFresh;

    // A record emitted into an EndTiling side buffer, released with the span
    // that submits that buffer. The first-pass callback has no buffer until
    // the 0x826AC32C callback names the worker-kick buffer.
    struct Held
    {
        std::optional<std::uint32_t> buffer;
        Record                       record;
        std::optional<std::uint64_t> list; // the tiled list a 0x826AC32C callback starts
    };

    // One BeginTiling..EndTiling list: physical segment -> dwords.
    struct TiledList
    {
        std::uint64_t                          id = 0;
        std::map<std::uint32_t, std::uint32_t> segments;
    };

    struct ReplayPointer
    {
        std::uint32_t pointer = 0;
        std::uint64_t order   = 0;
    };

    const std::uint64_t id          = nextLedgerId++;
    std::uint64_t       submitEpoch = 0; // pending submits from an earlier epoch are void
    // Lists recorded and not yet retired, oldest first. The worker replays only
    // the active list: the newest whose callback buffer was submitted.
    std::deque<TiledList>        lists;
    std::optional<std::uint64_t> activeList;
    std::uint64_t                nextList = 1;
    std::vector<Held>            held;
    // The newest replay kick; RPTR may pass it once nothing kicked before it is unreleased.
    std::optional<ReplayPointer> replayPointer;
    std::uint64_t                kicks     = 0;
    std::uint32_t                timestamp = 0;
    // Between device suspend and resume. The thread in a device-owner swap
    // records as the device thread; it must swap before the bracket ends.
    bool                           suspended = false;
    std::optional<std::thread::id> ownerSwapThread;
    bool                           ownerSwapSawSwap = false;
    // The thread between AcquireDevice and ReleaseDevice; unset when released
    // or before the first acquire.
    std::optional<std::thread::id> deviceOwner;
    bool                           epochWork = false; // a draw, clear or resolve since the last swap

    using Raised = std::optional<LedgerFailure>;

    void openSpan()
    {
        Span span;
        span.id = nextSpan++;
        spans.push_back(std::move(span));
    }

    Span* find(std::uint64_t id)
    {
        if (spans.empty() || id < spans.front().id || id - spans.front().id >= spans.size())
            return nullptr;
        return &spans[static_cast<std::size_t>(id - spans.front().id)];
    }

    bool accepting() const
    {
        return !closed && !failure;
    }

    // Spans closed and fully owned before the failing one still release.
    bool releasable(const Span& span) const
    {
        return !closed && (!failedSpan || span.id < *failedSpan);
    }

    void fail(std::string detail, ProgressKind kind, std::uint32_t address, std::uint64_t span, Raised& raised)
    {
        if (failure)
            return;
        failure    = LedgerFailure{ std::move(detail), kind, address };
        failedSpan = span;
        raised     = failure;
    }

    bool censusing() const
    {
        return config.mode == NativeDrawMode::Census;
    }

    // Counts a census failure; a new form is queued for one report.
    void count(LedgerFailure failure)
    {
        auto key   = censusKey(failure);
        auto found = censusIndex.find(key);
        if (found == censusIndex.end() && census.size() + 1 >= kCensusFormLimit)
        {
            failure = { "census_forms_overflow", ProgressKind::Unmodeled, 0 };
            key     = "overflow";
            found   = censusIndex.find(key);
        }
        if (found == censusIndex.end())
        {
            found = censusIndex.emplace(std::move(key), census.size()).first;
            census.push_back({ failure, 0 });
            censusFresh.push_back(std::move(failure));
        }
        ++census[found->second].count;
    }

    // Strict mode fails the session at the first unowned record; skip mode
    // leaves the span blocked for the completion-debt report. Census mode
    // counts it, marks the record skipped and returns true: the span goes on.
    bool unown(Span& span, std::size_t index, std::string reason, bool alwaysFail, Raised& raised)
    {
        const Record record = index < span.records.size() ? span.records[index] : Record{};
        if (record.kind == ProgressKind::Unmodeled)
            stats.unmodeledEmitter = record.address;
        if (!alwaysFail && censusing())
        {
            count({ std::move(reason), record.kind, record.address });
            if (index < span.records.size())
                span.records[index].skipped = true;
            span.partial = true;
            epochPartial = true;
            return true;
        }
        if (alwaysFail || config.mode == NativeDrawMode::Strict)
            fail(reason, record.kind, record.address, span.id, raised);
        if (!span.unowned.empty())
            return false;
        span.unowned       = std::move(reason);
        span.unownedRecord = index;
        return false;
    }

    bool replayThread() const
    {
        return ownerSwapThread != std::this_thread::get_id() && threadMarks.replay.contains(id);
    }

    // The worker only replays recorded segments, so no progress record may
    // come from its thread.
    bool onReplayThread(std::string_view what, ProgressKind kind, std::uint32_t address, Raised& raised)
    {
        if (!replayThread())
            return false;
        fail(std::format("progress_on_replay_thread {} at {:#010x}", what, address), kind, address, spans.back().id, raised);
        return true;
    }

    std::optional<PendingSubmit> takePending()
    {
        const auto found = threadMarks.pending.find(id);
        if (found == threadMarks.pending.end())
            return std::nullopt;
        const auto pending = found->second;
        threadMarks.pending.erase(found);
        if (pending.epoch != submitEpoch)
            return std::nullopt;
        return pending;
    }

    bool hasPending() const
    {
        const auto found = threadMarks.pending.find(id);
        return found != threadMarks.pending.end() && found->second.epoch == submitEpoch;
    }

    const Held* sideHeld() const
    {
        for (const auto& entry : held)
            if (entry.buffer)
                return &entry;
        return nullptr;
    }

    void failHeld(const Held& entry, Raised& raised)
    {
        fail(entry.buffer ? std::format("end_tiling_buffer_not_submitted {:#010x}", *entry.buffer) : std::string("first_pass_callback_not_submitted"),
             entry.record.kind,
             entry.record.address,
             spans.back().id,
             raised);
    }

    // Appends, in record order, the held records the submission of `buffer` carries.
    void attachHeld(Span& span, std::uint32_t buffer)
    {
        std::erase_if(held, [&](const Held& entry)
                      {
                          if (entry.buffer != buffer)
                              return false;
                          append(span, entry.record);
                          if (entry.list)
                              span.startsList = entry.list;
                          return true;
                      });
    }

    // The submission of a span carrying a 0x826AC32C callback: the worker
    // finished every older list before that buffer, chained last on the
    // previous list, or submitted it directly while idle.
    void startList(const Span& span)
    {
        if (!span.startsList)
            return;
        activeList = span.startsList;
        while (!lists.empty() && lists.front().id < *span.startsList)
            lists.pop_front();
        ++stats.tiledListsStarted;
    }

    const TiledList* findList(std::uint64_t list) const
    {
        for (const auto& entry : lists)
            if (entry.id == list)
                return &entry;
        return nullptr;
    }

    // The open span for a guest-thread record. A record from a thread other
    // than the span's fails the session; AcquireDevice hands the span over.
    // While an EndTiling side buffer is held, only submissions may proceed.
    Span& admit(Raised& raised, ProgressKind kind, std::uint32_t address, bool allowHeld = false)
    {
        auto&      span   = spans.back();
        const auto thread = std::this_thread::get_id();
        onReplayThread(ProgressKindName(kind), kind, address, raised);
        if (suspended && ownerSwapThread != thread)
            fail(std::format("device_suspended_record {} at {:#010x}", ProgressKindName(kind), address), kind, address, span.id, raised);
        if (const auto* entry = sideHeld(); entry && !allowHeld)
            failHeld(*entry, raised);
        if (!span.thread)
            span.thread = thread;
        else if (*span.thread != thread && !span.records.empty())
            fail("second_submit_thread", ProgressKind::Unmodeled, 0, span.id, raised);
        return span;
    }

    // Settles a pending draw record with its verdict.
    void settleDraw(Span& span, std::size_t index, std::uint64_t sequence, NativeDrawHook hook, std::uint32_t caller, std::optional<NativeDrawReason> verdict, std::string_view refusal, Raised& raised)
    {
        span.records[index].sequence = sequence;
        if (!verdict)
            span.lastAdmitted = std::max(span.lastAdmitted, sequence);
        else
        {
            const bool skip = config.mode == NativeDrawMode::Skip;
            if (config.mode != NativeDrawMode::Strict)
                ++stats.skipped[static_cast<std::size_t>(*verdict)];
            // Only a raster draw with no native route may be skipped; other
            // reasons carry guest effects or an unknown contract. Its shaders are not
            // inspected, so skip mode assumes such a draw does no memexport.
            if (skip && *verdict == NativeDrawReason::NoNativeRoute)
            {
                span.partial = true;
                epochPartial = true;
            }
            else
                unown(span,
                      index,
                      std::format("draw {} from {:#010x}: {}{}{}{}", guest_d3d::NativeDrawHookName(hook), caller, guest_d3d::NativeDrawReasonName(*verdict), skip ? "_not_skippable" : "", refusal.empty() ? "" : "; ", refusal),
                      false,
                      raised);
        }
    }

    // Return the detached pending record, or null when gone/nonpending. After
    // failure, only spans before it still release and settle records; later
    // spans clear records and remain held.
    Span* detached(const CompletionLedger::DetachedDraw& draw)
    {
        if (closed)
            return nullptr;
        auto* span = find(draw.span);
        if (!span || draw.index >= span->records.size() || !span->records[draw.index].pending || span->records[draw.index].kind != ProgressKind::Draw)
            return nullptr;
        if (accepting() || releasable(*span))
            return span;
        span->records[draw.index].pending = false;
        --span->pending;
        return nullptr;
    }

    // The record of the span's last admitted draw, for failure attribution.
    std::size_t admittedRecord(const Span& span) const
    {
        for (std::size_t index = span.records.size(); index-- > 0;)
            if (span.records[index].sequence == span.lastAdmitted)
                return index;
        return span.records.size();
    }

    // Resolve 0x8269E520 has no return before its copy-word hook at 0x8269E8A0
    // and reserves no ring space before it, so another record from the same
    // thread proves the hook never ran for the open resolve.
    void abandonOpenResolve(Raised& raised)
    {
        const auto open = openResolves.find(std::this_thread::get_id());
        if (open == openResolves.end())
            return;
        const auto [id, index] = open->second;
        openResolves.erase(open);
        auto* span = find(id);
        if (!span || !span->records[index].pending)
            return;
        span->records[index].pending = false;
        --span->pending;
        unown(*span, index, "resolve_copy_not_captured", false, raised);
    }

    std::size_t append(Span& span, Record record)
    {
        if (record.kind == ProgressKind::Draw || record.kind == ProgressKind::Clear || record.kind == ProgressKind::Resolve)
            epochWork = true;
        span.records.push_back(record);
        return span.records.size() - 1;
    }

    // Returns the newest dropped Resolve op mark; zero when none was dropped.
    std::uint64_t drop()
    {
        std::uint64_t resolved = 0;
        for (const auto& span : spans)
        {
            if (span.writePointer || !span.records.empty())
                ++stats.spansDropped;
            for (const auto& record : span.records)
                if (record.kind == ProgressKind::Resolve)
                    resolved = std::max(resolved, record.sequence);
        }
        spans.clear();
        openDraws.clear();
        frontbuffer.clear();
        openResolves.clear();
        ++submitEpoch;
        lists.clear();
        activeList.reset();
        held.clear();
        replayPointer.reset();
        epochPartial = false;
        openSpan();
        return resolved;
    }

    // Outside both ledger locks, like FlushThrough: the draws side takes its own.
    void discard(std::uint64_t mark)
    {
        if (!mark || !config.draws)
            return;
        try
        {
            config.draws->DiscardGuestImagesThrough(mark);
        }
        catch (...)
        {
        }
    }

    bool write(std::uint32_t address, std::uint32_t value)
    {
        if (closed || !config.sink)
            return false;
        if (config.draws)
            config.draws->HostWriting(address, sizeof(value));
        const bool written = config.sink->WritePhysical(address, value);
        if (config.draws)
            config.draws->HostWrote(address, sizeof(value));
        return written;
    }

    void notify(const Raised& raised)
    {
        if (censusing() && config.census)
        {
            std::vector<LedgerFailure> fresh;
            {
                std::lock_guard lock(mutex);
                fresh.swap(censusFresh);
            }
            for (const auto& form : fresh)
            {
                try
                {
                    config.census(form);
                }
                catch (...)
                {
                }
            }
        }
        if (!raised || !config.failed)
            return;
        try
        {
            config.failed(*raised);
        }
        catch (...)
        {
        }
    }

    // Applies span effects in record order. Returns true when every record is
    // applied, false while it waits on the guest or after the span became unowned.
    bool apply(std::unique_lock<std::mutex>& lock, std::uint64_t id, Raised& raised);
};

CompletionLedger::CompletionLedger(LedgerConfig config)
: state(std::make_unique<State>())
{
    state->config = std::move(config);
    state->openSpan();
}

CompletionLedger::~CompletionLedger() = default;

void CompletionLedger::InvalidateState(std::uint32_t emitter)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        state->abandonOpenResolve(raised);
        state->append(state->admit(raised, ProgressKind::InvalidateState, emitter), { .kind = ProgressKind::InvalidateState, .address = emitter, .value = 0x7FFF });
    }
    state->notify(raised);
}

void CompletionLedger::Fence(std::uint32_t block, std::uint32_t fence, std::uint32_t cursor, std::optional<std::uint32_t> deferredBuffer)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        state->abandonOpenResolve(raised);
        // EndTiling records this fence after holding the worker-kick callback's buffer.
        auto&        span = state->admit(raised, ProgressKind::Fence, 0x8269BCA8, deferredBuffer.has_value());
        const Record record{ .kind = ProgressKind::Fence, .address = 0x8269BCA8, .target = block, .value = fence, .extra = cursor };
        // EndTiling emits this fence into a side buffer before it kicks the
        // callback buffer, then submits the side buffer, usually deferred.
        if (deferredBuffer)
        {
            state->held.push_back({ deferredBuffer, { .kind = ProgressKind::InvalidateState, .address = 0x8269BCA8, .value = 0x7FFF } });
            state->held.push_back({ deferredBuffer, record });
        }
        else
            state->append(span, record);
        state->stats.fenceBlock = block;
        if (state->writeback && block + kReadPointerOffset != state->writeback)
            state->fail(std::format("identity_mismatch: B+0x3C {:#010x} is not writeback {:#010x}", block + kReadPointerOffset, state->writeback),
                        ProgressKind::Fence,
                        0x8269BCA8,
                        span.id,
                        raised);
    }
    state->notify(raised);
}

void CompletionLedger::Callback(std::uint32_t                scratch,
                                std::uint32_t                function,
                                std::uint32_t                parameter,
                                std::uint32_t                flags,
                                std::optional<std::uint32_t> predicateMask,
                                std::optional<std::uint32_t> sideBuffer)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        state->abandonOpenResolve(raised);
        // The emitter's own defaults: flags 2|4 when neither is set, mask 4 when zero.
        if ((flags & 6) == 0)
            flags |= 6;
        std::uint32_t mask = (flags >> 24) & 0x3F;
        if (!mask)
            mask = kCallbackMask;
        auto&        span = state->admit(raised, ProgressKind::Callback, 0x8269BF40);
        const Record record{ .kind = ProgressKind::Callback, .address = 0x8269BF40, .target = scratch, .value = function, .extra = parameter, .flags = flags, .mask = mask };
        if (predicateMask && *predicateMask != kFirstPassPredicate)
            state->unown(span, state->append(span, record), std::format("callback_predicated {:#x}", *predicateMask), false, raised);
        else if (mask != kCallbackMask)
            state->unown(span, state->append(span, record), std::format("callback_mask_{:#x}", mask), false, raised);
        // Both worker-kick callbacks wait for the callback buffer's own
        // submission: 0x8269BD98 raises G+0x2B00 at 0x8269BE4C before that kick,
        // and a worker woken earlier could lower it from zero at 0x826ABB2C.
        else if (sideBuffer)
        {
            for (auto& entry : state->held)
                if (!entry.buffer)
                    entry.buffer = sideBuffer;
            // EndTiling emits this callback for the list BeginTiling started last.
            state->held.push_back({ sideBuffer, record, state->lists.empty() ? std::nullopt : std::optional{ state->lists.back().id } });
        }
        else if (predicateMask)
            state->held.push_back({ std::nullopt, record });
        else
            state->append(span, record);
    }
    state->notify(raised);
}

void CompletionLedger::ScratchWrite(std::uint32_t address, std::uint32_t value, std::uint32_t emitter)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        state->abandonOpenResolve(raised);
        state->append(state->admit(raised, ProgressKind::ScratchWrite, emitter), { .kind = ProgressKind::ScratchWrite, .address = emitter, .target = address, .value = value });
    }
    state->notify(raised);
}

void CompletionLedger::GpuTimestamp(std::uint32_t address, std::uint32_t block)
{
    constexpr std::uint32_t kEmitter = 0x826A9BDC;
    State::Raised           raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        state->abandonOpenResolve(raised);
        auto& span = state->admit(raised, ProgressKind::ScratchWrite, kEmitter);
        if (address < block + kTimestampOffset || address >= block + kTimestampEnd || (address & 3) != 0)
            state->fail(std::format("timestamp_address_outside_block {:#010x}", address), ProgressKind::ScratchWrite, kEmitter, span.id, raised);
        else
        {
            // Readers treat zero as not yet written and handle 32-bit wrap.
            if (++state->timestamp == 0)
                ++state->timestamp;
            state->append(span, { .kind = ProgressKind::ScratchWrite, .address = kEmitter, .target = address, .value = state->timestamp });
        }
    }
    state->notify(raised);
}

void CompletionLedger::GpuWait(std::uint32_t address, std::uint32_t reference, std::uint32_t emitter)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        state->abandonOpenResolve(raised);
        state->append(state->admit(raised, ProgressKind::GpuWait, emitter), { .kind = ProgressKind::GpuWait, .address = emitter, .target = address, .value = reference });
    }
    state->notify(raised);
}

void CompletionLedger::BeginDraw(NativeDrawHook hook, std::uint32_t caller)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        state->abandonOpenResolve(raised);
        const auto thread = std::this_thread::get_id();
        // A root whose Original unwound never reports a verdict.
        if (const auto stale = state->openDraws.find(thread); stale != state->openDraws.end())
        {
            if (auto* span = state->find(stale->second.first); span && span->records[stale->second.second].pending)
            {
                span->records[stale->second.second].pending = false;
                --span->pending;
                state->unown(*span, stale->second.second, "draw_abandoned", false, raised);
            }
            state->openDraws.erase(stale);
        }
        auto&      span  = state->admit(raised, ProgressKind::Draw, caller);
        const auto index = state->append(span, { .kind = ProgressKind::Draw, .address = caller, .hook = hook, .pending = true });
        ++span.pending;
        state->openDraws[thread] = { span.id, index };
    }
    state->notify(raised);
}

void CompletionLedger::FinishDraw(std::uint64_t sequence, NativeDrawHook hook, std::uint32_t caller, std::optional<NativeDrawReason> verdict, std::string_view refusal)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        state->abandonOpenResolve(raised);
        Span*       span  = nullptr;
        std::size_t index = 0;
        if (const auto open = state->openDraws.find(std::this_thread::get_id()); open != state->openDraws.end())
        {
            span  = state->find(open->second.first);
            index = open->second.second;
            state->openDraws.erase(open);
            if (span && (!span->records[index].pending || span->records[index].hook != hook || span->records[index].address != caller))
            {
                if (span->records[index].pending)
                {
                    span->records[index].pending = false;
                    --span->pending;
                    state->unown(*span, index, "draw_abandoned", false, raised);
                }
                span = nullptr;
            }
        }
        if (!span)
        {
            // No matching begin: record the draw where its verdict arrived.
            span  = &state->admit(raised, ProgressKind::Draw, caller);
            index = state->append(*span, { .kind = ProgressKind::Draw, .address = caller, .hook = hook });
        }
        else
        {
            span->records[index].pending = false;
            --span->pending;
        }
        state->settleDraw(*span, index, sequence, hook, caller, verdict, refusal, raised);
    }
    state->notify(raised);
}

std::optional<CompletionLedger::DetachedDraw> CompletionLedger::DetachDraw()
{
    std::lock_guard lock(state->mutex);
    const auto      open = state->openDraws.find(std::this_thread::get_id());
    if (open == state->openDraws.end())
        return std::nullopt;
    const DetachedDraw draw{ open->second.first, open->second.second };
    state->openDraws.erase(open);
    return draw;
}

void CompletionLedger::FinishDraw(const DetachedDraw& draw, std::uint64_t sequence, NativeDrawHook hook, std::uint32_t caller, std::optional<NativeDrawReason> verdict, std::string_view refusal)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        auto*           span = state->detached(draw);
        if (!span)
            return;
        auto& record   = span->records[draw.index];
        record.pending = false;
        --span->pending;
        if (record.hook != hook || record.address != caller)
            state->unown(*span, draw.index, "draw_abandoned", false, raised);
        else
            state->settleDraw(*span, draw.index, sequence, hook, caller, verdict, refusal, raised);
    }
    state->notify(raised);
}

void CompletionLedger::AbandonDraw(const DetachedDraw& draw)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        auto*           span = state->detached(draw);
        if (!span)
            return;
        span->records[draw.index].pending = false;
        --span->pending;
        state->unown(*span, draw.index, "draw_abandoned", false, raised);
    }
    state->notify(raised);
}

void CompletionLedger::Resolve(std::uint32_t call, std::optional<std::uint32_t> destination)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        state->abandonOpenResolve(raised);
        auto&      span             = state->admit(raised, ProgressKind::Resolve, call);
        const auto index            = state->append(span, { .kind = ProgressKind::Resolve, .address = call, .value = destination.value_or(0) });
        span.records[index].pending = true;
        ++span.pending;
        state->openResolves[std::this_thread::get_id()] = { span.id, index };
    }
    state->notify(raised);
}

bool CompletionLedger::AwaitingResolveCopy() const
{
    std::lock_guard lock(state->mutex);
    return state->accepting() && state->openResolves.contains(std::this_thread::get_id());
}

void CompletionLedger::FinishResolve(std::uint64_t opMark, bool frontbuffer, bool partial, std::string error)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        const auto open = state->openResolves.find(std::this_thread::get_id());
        if (open == state->openResolves.end())
            return;
        auto*      span  = state->find(open->second.first);
        const auto index = open->second.second;
        state->openResolves.erase(open);
        if (!span)
            return;
        auto& record    = span->records[index];
        record.sequence = opMark;
        if (frontbuffer && opMark && error.empty())
            state->frontbuffer.push_back({ span->id, index, record.value });
        else
        {
            record.pending = false;
            --span->pending;
            if (partial)
                error = "resolve_source_partial";
            if (!opMark && error.empty())
                error = "resolve_not_admitted";
            if (!error.empty())
                state->unown(*span, index, std::move(error), false, raised);
            else
                span->lastAdmitted = std::max(span->lastAdmitted, opMark);
        }
    }
    state->notify(raised);
}

void CompletionLedger::Clear(std::uint64_t opMark, std::string error)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        state->abandonOpenResolve(raised);
        auto&      span  = state->admit(raised, ProgressKind::Clear, 0x826A1D90);
        const auto index = state->append(span, { .kind = ProgressKind::Clear, .address = 0x826A1D90, .sequence = opMark });
        if ((!opMark || !error.empty()) && state->config.mode == NativeDrawMode::Skip)
        {
            // A clear writes only EDRAM. The assembler marks a rejected color
            // target's source partial, so a later guest-memory resolve still holds.
            span.partial        = true;
            state->epochPartial = true;
        }
        else if (!opMark || !error.empty())
            state->unown(span, index, error.empty() ? "clear_not_admitted" : std::move(error), false, raised);
        else
            span.lastAdmitted = std::max(span.lastAdmitted, opMark);
    }
    state->notify(raised);
}

void CompletionLedger::Unmodeled(std::uint32_t emitter)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        state->abandonOpenResolve(raised);
        auto&      span  = state->admit(raised, ProgressKind::Unmodeled, emitter);
        const auto index = state->append(span, { .kind = ProgressKind::Unmodeled, .address = emitter });
        state->unown(span, index, std::format("unmodeled_emitter {:#010x}", emitter), false, raised);
    }
    state->notify(raised);
}

void CompletionLedger::Barrier(std::uint32_t emitter)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        state->abandonOpenResolve(raised);
        state->append(state->admit(raised, ProgressKind::Barrier, emitter), { .kind = ProgressKind::Barrier, .address = emitter });
    }
    state->notify(raised);
}

bool CompletionLedger::Swap(std::uint32_t frontbufferPhysical)
{
    State::Raised raised;
    bool          partial = false;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return false;
        state->abandonOpenResolve(raised);
        // EndTiling submits its side buffers before the frame's VdSwap.
        if (!state->held.empty())
            state->failHeld(state->held.front(), raised);
        state->append(state->admit(raised, ProgressKind::Swap, frontbufferPhysical), { .kind = ProgressKind::Swap, .value = frontbufferPhysical });
        for (const auto& resolve : state->frontbuffer)
        {
            auto* span = state->find(resolve.span);
            if (!span || !span->records[resolve.index].pending)
                continue;
            span->records[resolve.index].pending = false;
            --span->pending;
            if (resolve.destination != frontbufferPhysical)
                state->unown(*span, resolve.index, std::format("frontbuffer_resolve_mismatch: {:#010x} is not swap {:#010x}", resolve.destination, frontbufferPhysical), false, raised);
            else
            {
                span->records[resolve.index].flags = 1; // Host-only, covered by the swap output.
                span->lastAdmitted                 = std::max(span->lastAdmitted, span->records[resolve.index].sequence);
            }
        }
        state->frontbuffer.clear();
        state->epochWork = false;
        if (state->ownerSwapThread == std::this_thread::get_id())
            state->ownerSwapSawSwap = true;
        partial             = state->epochPartial;
        state->epochPartial = false;
        ++state->stats.swaps;
        if (partial)
            ++state->stats.partialSwaps;
    }
    state->notify(raised);
    return partial;
}

void CompletionLedger::Kick(std::uint32_t writePointer)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        const auto order   = ++state->kicks;
        const auto pending = state->takePending();
        if (pending && pending->worker)
        {
            Span* oldest = nullptr;
            bool  named  = false; // an unsubmitted deferral names this address
            for (auto& span : state->spans)
            {
                if (!span.deferred || span.writePointer)
                    continue;
                if (!oldest)
                    oldest = &span;
                if (*span.deferred == pending->physical)
                    named = true;
            }
            const auto tiled = [&](const State::TiledList& list)
            {
                const auto found = list.segments.find(pending->physical);
                return found != list.segments.end() && found->second == pending->dwords;
            };
            const auto* active = state->activeList ? state->findList(*state->activeList) : nullptr;
            const auto  other  = std::find_if(state->lists.begin(), state->lists.end(), tiled);
            if (oldest && *oldest->deferred == pending->physical && oldest->deferredDwords == pending->dwords)
            {
                oldest->writePointer = writePointer;
                oldest->kickOrder    = order;
                state->startList(*oldest);
            }
            // An address still deferred is never taken for a replay, whatever its size.
            else if (!named && active && tiled(*active))
            {
                state->replayPointer = State::ReplayPointer{ writePointer, order };
                ++state->stats.replaySubmits;
            }
            else if (!named && other != state->lists.end())
                state->fail(std::format("replay_submit_inactive_list {:#010x} dwords {}, list {} active {}",
                                        pending->physical,
                                        pending->dwords,
                                        other->id,
                                        state->activeList ? std::to_string(*state->activeList) : std::string("none")),
                            ProgressKind::Unmodeled,
                            pending->site,
                            oldest ? oldest->id : state->spans.back().id,
                            raised);
            else
                state->fail(std::format("replay_submit_unmatched {:#010x} dwords {}{}",
                                        pending->physical,
                                        pending->dwords,
                                        oldest ? std::format(", oldest deferred {:#010x} dwords {}", *oldest->deferred, oldest->deferredDwords) : std::string(", none deferred")),
                            ProgressKind::Unmodeled,
                            pending->site,
                            oldest ? oldest->id : state->spans.back().id,
                            raised);
        }
        else if (state->replayThread())
            state->fail(std::format("worker_kick_unattributed {:#010x}", writePointer), ProgressKind::Unmodeled, 0x8269BA90, state->spans.back().id, raised);
        else
        {
            state->abandonOpenResolve(raised);
            auto& span = state->admit(raised, ProgressKind::Unmodeled, 0x8269BA90, true);
            // Every main kick passes 0x8269BE5C; without it a held buffer cannot be matched.
            if (pending)
                state->attachHeld(span, pending->physical);
            else if (state->sideHeld())
                state->fail(std::format("main_kick_unattributed {:#010x}", writePointer), ProgressKind::Unmodeled, 0x8269BE5C, span.id, raised);
            span.writePointer = writePointer;
            span.kickOrder    = order;
            state->startList(span);
            state->openSpan();
        }
    }
    state->notify(raised);
}

void CompletionLedger::TiledProgress(ProgressKind kind, std::uint32_t site)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        if (!state->onReplayThread(ProgressKindName(kind), kind, site, raised))
            state->fail(std::format("tiled_progress_{} at {:#010x}", ProgressKindName(kind), site), kind, site, state->spans.back().id, raised);
    }
    state->notify(raised);
}

void CompletionLedger::TiledSegment(std::uint32_t physical, std::uint32_t dwords)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        if (!state->onReplayThread("tiled_segment", ProgressKind::Unmodeled, 0x8269C4E8, raised))
        {
            // 0x8269C400 appends only while G+0x33B0 != 0, which BeginTiling sets.
            if (state->lists.empty())
                state->fail(std::format("tiled_segment_without_list {:#010x}", physical), ProgressKind::Unmodeled, 0x8269C4E8, state->spans.back().id, raised);
            else
                state->lists.back().segments[physical] = dwords;
        }
    }
    state->notify(raised);
}

void CompletionLedger::BeginTiledList()
{
    std::lock_guard lock(state->mutex);
    if (!state->accepting())
        return;
    // Older lists stay until a later list starts: the worker may lag several lists.
    state->lists.push_back({ state->nextList++, {} });
    state->stats.maxTiledLists = std::max<std::uint64_t>(state->stats.maxTiledLists, state->lists.size());
}

void CompletionLedger::Defer(std::uint32_t physical, std::uint32_t dwords)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        if (!state->onReplayThread("deferred_kick", ProgressKind::Unmodeled, 0x8269BDE8, raised))
        {
            state->abandonOpenResolve(raised);
            auto& span = state->admit(raised, ProgressKind::Unmodeled, 0x8269BDE8, true);
            state->attachHeld(span, physical);
            span.deferred       = physical;
            span.deferredDwords = dwords;
            ++state->stats.deferredSpans;
            state->openSpan();
        }
    }
    state->notify(raised);
}

void CompletionLedger::MarkReplayThread()
{
    std::lock_guard lock(state->mutex);
    threadMarks.replay.insert(state->id);
}

void CompletionLedger::Suspend()
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        if (state->suspended)
            state->fail("suspend_nested", ProgressKind::Unmodeled, 0x8268CFDC, state->spans.back().id, raised);
        state->suspended = true;
        ++state->stats.deviceSuspends;
    }
    state->notify(raised);
}

void CompletionLedger::Resume()
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        // Resume takes the section the bracket holds, so it cannot interleave.
        if (!state->suspended)
            state->fail("resume_unsuspended", ProgressKind::Unmodeled, 0x8268D060, state->spans.back().id, raised);
        else if (state->ownerSwapThread)
            state->fail("resume_during_deferred_swap", ProgressKind::Unmodeled, 0x8268D060, state->spans.back().id, raised);
        state->suspended = false;
    }
    state->notify(raised);
}

void CompletionLedger::AcquireDevice()
{
    constexpr std::uint32_t kSite = 0x8268CF3C;
    State::Raised           raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        const auto thread = std::this_thread::get_id();
        auto&      span   = state->spans.back();
        // Without the owner's release nothing orders the two threads' records.
        if (state->deviceOwner && *state->deviceOwner != thread)
            state->fail("device_acquired_while_owned", ProgressKind::Unmodeled, kSite, span.id, raised);
        else
        {
            state->deviceOwner = thread;
            if (span.thread && *span.thread != thread && !state->ownerSwapThread)
            {
                if (!span.records.empty())
                    ++state->stats.deviceHandoffs;
                span.thread = thread;
            }
        }
    }
    state->notify(raised);
}

void CompletionLedger::ReleaseDevice()
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        // A resolve left open here would otherwise wait on a thread that no
        // longer records.
        state->abandonOpenResolve(raised);
        // Another thread's release orders nothing; the owner keeps the device.
        if (state->deviceOwner == std::this_thread::get_id())
            state->deviceOwner.reset();
    }
    state->notify(raised);
}

void CompletionLedger::BeginDeviceOwnerSwap()
{
    constexpr std::uint32_t kSite = 0x826ABF7C;
    State::Raised           raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        auto&       span = state->spans.back();
        std::string error;
        // Suspend drained the device, so the bracket starts on an empty open
        // span with nothing held, and its swap closes an epoch with no work.
        if (state->ownerSwapThread)
            error = "deferred_swap_nested";
        else if (!state->suspended)
            error = "deferred_swap_unsuspended";
        else if (!span.records.empty())
            error = "deferred_swap_span_open: " + describe(span.records.front());
        else if (!state->held.empty())
            error = "deferred_swap_held";
        else if (state->epochWork)
            error = "deferred_swap_epoch_open";
        if (!error.empty())
            state->fail(std::move(error), ProgressKind::Swap, kSite, span.id, raised);
        else
        {
            state->ownerSwapThread  = std::this_thread::get_id();
            state->ownerSwapSawSwap = false;
            span.thread             = state->ownerSwapThread;
            ++state->stats.deferredSwaps;
        }
    }
    state->notify(raised);
}

void CompletionLedger::EndDeviceOwnerSwap()
{
    constexpr std::uint32_t kSite = 0x826ABF94;
    State::Raised           raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        auto&       span = state->spans.back();
        std::string error;
        // The swap's own 0x8269C270 kick closes every record of the bracket.
        if (state->ownerSwapThread != std::this_thread::get_id())
            error = "deferred_swap_unopened";
        else if (!span.records.empty())
            error = "deferred_swap_unkicked: " + describe(span.records.front());
        else if (!state->ownerSwapSawSwap)
            error = "deferred_swap_unswapped";
        if (!error.empty())
            state->fail(std::move(error), ProgressKind::Swap, kSite, span.id, raised);
        else
        {
            state->ownerSwapThread.reset();
            span.thread.reset();
        }
    }
    state->notify(raised);
}

void CompletionLedger::ReplaySubmit(std::uint32_t physical, std::uint32_t dwords, std::uint32_t site)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        threadMarks.replay.insert(state->id);
        // 0x8269B900 kicks every packet it writes, so a second submit means a missed kick.
        if (state->hasPending())
            state->fail(std::format("replay_submit_unkicked {:#010x}", threadMarks.pending[state->id].physical), ProgressKind::Unmodeled, site, state->spans.back().id, raised);
        else
            threadMarks.pending[state->id] = { physical, dwords, site, state->submitEpoch, true };
    }
    state->notify(raised);
}

void CompletionLedger::DirectSubmit(std::uint32_t physical, std::uint32_t dwords)
{
    constexpr std::uint32_t kSite = 0x8269BE5C;
    State::Raised           raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        if (!state->onReplayThread("direct_submit", ProgressKind::Unmodeled, kSite, raised))
        {
            if (state->hasPending())
                state->fail(std::format("direct_submit_unkicked {:#010x}", threadMarks.pending[state->id].physical), ProgressKind::Unmodeled, kSite, state->spans.back().id, raised);
            else
                threadMarks.pending[state->id] = { physical, dwords, kSite, state->submitEpoch, false };
        }
    }
    state->notify(raised);
}

void CompletionLedger::SilentSubmit()
{
    constexpr std::uint32_t kSite = 0x8269BE5C;
    State::Raised           raised;
    {
        std::lock_guard lock(state->mutex);
        if (!state->accepting())
            return;
        const auto pending = state->takePending();
        if (!pending)
            return;
        state->abandonOpenResolve(raised);
        auto& span = state->admit(raised, ProgressKind::Unmodeled, kSite, true);
        state->attachHeld(span, pending->physical);
        // The work recorded so far retires through the normal host-completion path; nothing
        // is kicked, so the span carries no write pointer and releasing it writes no RPTR.
        if (!span.records.empty())
        {
            span.sealed = true;
            state->startList(span);
            state->openSpan();
        }
    }
    state->notify(raised);
}

void CompletionLedger::RequestFence(std::uint32_t fence, std::uint32_t block)
{
    std::lock_guard lock(state->mutex);
    state->stats.requestedFence = fence;
    state->stats.fenceRequested = true;
    if (block)
        state->stats.fenceBlock = block;
}

void CompletionLedger::Fail(std::string detail, ProgressKind kind, std::uint32_t address, bool fatal)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (state->closed)
            return;
        if (state->censusing() && !fatal)
            state->count({ std::move(detail), kind, address });
        else
            state->fail(std::move(detail), kind, address, state->spans.back().id, raised);
    }
    state->notify(raised);
}

void CompletionLedger::CloseEpochWrites()
{
    std::uint64_t dropped = 0;
    {
        std::lock_guard writes(state->imageWrites);
        std::lock_guard lock(state->mutex);
        dropped = state->drop();
        // The old block is about to be freed; RPTR waits for the next registration,
        // and the debt view shows no B until the new epoch reveals it.
        state->writeback        = 0;
        state->stats.fenceBlock = 0;
    }
    state->discard(dropped);
}

void CompletionLedger::BeginRingEpoch()
{
    std::uint64_t dropped = 0;
    {
        std::lock_guard writes(state->imageWrites);
        std::lock_guard lock(state->mutex);
        dropped                 = state->drop();
        state->writeback        = 0;
        state->stats.fenceBlock = 0;
        ++state->stats.ringEpoch;
    }
    state->discard(dropped);
}

void CompletionLedger::RegisterWriteback(std::uint32_t pointer)
{
    State::Raised raised;
    {
        std::lock_guard lock(state->mutex);
        if (state->closed)
            return;
        state->writeback = pointer;
        for (const auto& span : state->spans)
            for (const auto& record : span.records)
                if (record.kind == ProgressKind::Fence && record.target + kReadPointerOffset != pointer)
                    state->fail(std::format("identity_mismatch: B+0x3C {:#010x} is not writeback {:#010x}", record.target + kReadPointerOffset, pointer),
                                ProgressKind::Fence,
                                0x8269BCA8,
                                span.id,
                                raised);
    }
    state->notify(raised);
}

bool CompletionLedger::State::apply(std::unique_lock<std::mutex>& lock, std::uint64_t id, Raised& raised)
{
    for (;;)
    {
        auto* span = find(id);
        if (!span || !releasable(*span))
            return false;
        if (span->applied == span->records.size())
            return true;
        // Census mode: an unowned record has no effect to apply. A skipped
        // admitted resolve's image is never written, so its reads stop waiting.
        if (span->records[span->applied].skipped)
        {
            const auto skipped = span->records[span->applied];
            if (skipped.kind == ProgressKind::Resolve && skipped.sequence && skipped.flags != 1 && config.draws)
            {
                lock.unlock();
                try
                {
                    config.draws->GuestImageSkipped(skipped.sequence);
                }
                catch (...)
                {
                }
                lock.lock();
                span = find(id);
                if (!span || !releasable(*span))
                    return false;
            }
            span->stage = 0;
            ++span->applied;
            continue;
        }
        const auto index  = span->applied;
        const auto record = span->records[index];
        const auto failed = [&](std::uint32_t address)
        {
            unown(*span, index, std::format("guest_write_failed at {:#010x}", address), false, raised);
            return false;
        };
        switch (record.kind)
        {
            case ProgressKind::Resolve:
            {
                if (record.flags == 1)
                    break;
                // Teardown acquires this gate before the ledger lock. Drop the
                // ledger lock before acquiring it, then revalidate the span.
                lock.unlock();
                std::unique_lock writes(imageWrites);
                lock.lock();
                span = find(id);
                if (!span || !releasable(*span))
                    return false;
                lock.unlock();
                GuestImageTake take;
                bool           written = false;
                try
                {
                    if (config.draws)
                        take = config.draws->TakeGuestImage(record.sequence);
                    // Older held images over this one land first, in op-mark order.
                    for (const auto& [mark, image] : take.before)
                    {
                        if (take.closing || !config.sink || !writeGuestImage(*config.sink, image))
                        {
                            if (!take.closing)
                                take.error = std::format("held_image_unwritten: op mark {}", mark);
                            take.image.reset();
                            take.held = false;
                            break;
                        }
                        config.draws->GuestImageWritten(mark);
                    }
                    const auto& image = take.image;
                    written           = !take.closing && take.error.empty() &&
                                        (take.held || (image && image->physical == record.value && !image->bytes.empty() && config.sink && writeGuestImage(*config.sink, *image)));
                    if (written && !take.held && config.draws)
                        config.draws->GuestImageWritten(record.sequence);
                }
                catch (...)
                {
                    take = { std::nullopt, "guest_image_threw", false };
                }
                lock.lock();
                span = find(id);
                if (!span || !releasable(*span))
                    return false;
                // A closing source holds the span; ledger close follows and ends it quietly.
                if (take.closing)
                    return false;
                if (!written)
                {
                    unown(*span, index, take.error.empty() ? "resolve_readback_failed" : "resolve_readback_failed: " + take.error, false, raised);
                    return false;
                }
                break;
            }
            case ProgressKind::Fence:
                // The cursor lands before the fence, as the emitter's two packets do.
                if (!write(record.target + kCursorOffset, record.extra))
                    return failed(record.target + kCursorOffset);
                if (!write(record.target + kFenceOffset, record.value))
                    return failed(record.target + kFenceOffset);
                stats.lastReleasedFence = record.value;
                stats.fenceReleased     = true;
                break;
            case ProgressKind::ScratchWrite:
                if (!write(record.target, record.value))
                    return failed(record.target);
                break;
            case ProgressKind::GpuWait:
            {
                const auto word = config.sink ? config.sink->ReadPhysical(record.target) : std::nullopt;
                if (!word)
                {
                    unown(*span, index, std::format("wait_unreadable at {:#010x}", record.target), false, raised);
                    return false;
                }
                if (*word != record.value)
                    return false;
                break;
            }
            case ProgressKind::Callback:
            {
                const auto scratch = record.target;
                if (span->stage == 0)
                {
                    // SCRATCH_REG4/5 then SCRATCH_REG0, the order 0x8269BF40 writes them.
                    if (record.flags & 2)
                    {
                        if (!write(scratch + kScratchCallbackOffset, record.value))
                            return failed(scratch + kScratchCallbackOffset);
                        if (!write(scratch + kScratchParameterOffset, record.extra))
                            return failed(scratch + kScratchParameterOffset);
                        if (!write(scratch + kScratchMaskOffset, record.mask))
                            return failed(scratch + kScratchMaskOffset);
                    }
                    span->stage = 1;
                }
                if (span->stage == 1)
                {
                    if (record.flags & 2)
                    {
                        lock.unlock();
                        const auto result = config.sink ? config.sink->RaiseCallbackInterrupt(kCallbackCpu) : InterruptResult::Stopped;
                        lock.lock();
                        span = find(id);
                        if (!span || !releasable(*span) || result != InterruptResult::Raised)
                            return false;
                        ++stats.callbacksReleased;
                        stats.lastCallback = record.value;
                    }
                    span->stage = 2;
                }
                if (record.flags & 4)
                {
                    // The GPU waits until every handler cleared its CPU bit.
                    const auto pendingMask = config.sink ? config.sink->ReadPhysical(scratch + kScratchMaskOffset) : std::nullopt;
                    if (!pendingMask)
                    {
                        unown(*span, index, std::format("wait_unreadable at {:#010x}", scratch + kScratchMaskOffset), false, raised);
                        return false;
                    }
                    if (*pendingMask != 0)
                        return false;
                    if (!write(scratch + kScratchCallbackOffset, kRetiredCallback))
                        return failed(scratch + kScratchCallbackOffset);
                }
                span->stage = 0;
                break;
            }
            case ProgressKind::Barrier:
                // Nothing to write. Effects apply in record order, and apply runs
                // only after the host fence for every admitted draw in the span,
                // so all earlier work is complete before any later record.
                break;
            default:
                break;
        }
        ++span->applied;
    }
}

bool CompletionLedger::Pump()
{
    std::lock_guard serialized(state->pumping);
    State::Raised   raised;
    bool            released = false;
    {
        std::unique_lock lock(state->mutex);
        while (!state->spans.empty() && state->releasable(state->spans.front()))
        {
            auto* span = &state->spans.front();
            // Nothing kicked before the newest replay is unreleased, so the GPU
            // consumed its IB packet; the worker polls RPTR for ring space.
            if (!span->writePointer && state->replayPointer && state->writeback)
            {
                if (!state->write(state->writeback, state->replayPointer->pointer) &&
                    !state->unown(*span, span->records.size(), std::format("guest_write_failed at {:#010x}", state->writeback), false, raised))
                    break;
                state->replayPointer.reset();
            }
            // A new epoch's span waits for its writeback registration before any effect.
            if ((!span->writePointer && !span->sealed) || span->pending || !span->unowned.empty() || !state->writeback)
                break;
            const auto id = span->id;
            if (span->lastAdmitted)
            {
                auto* draws = state->config.draws;
                if (!span->flushed)
                {
                    // Census mode releases the span without a host wait.
                    if (!draws)
                    {
                        if (state->unown(*span, state->admittedRecord(*span), "no_draw_completion", false, raised))
                            span->lastAdmitted = 0;
                        break;
                    }
                    const auto    last = span->lastAdmitted;
                    DrawFlush     flush;
                    std::uint64_t submitted = 0;
                    lock.unlock();
                    try
                    {
                        flush = draws->FlushThrough(last);
                        if (!flush.fence && !flush.retry && state->censusing())
                            submitted = draws->LastSubmitted();
                    }
                    catch (...)
                    {
                        flush = { 0, false, "draw_flush_threw" };
                    }
                    lock.lock();
                    span = state->find(id);
                    if (!span || !state->releasable(*span))
                        break;
                    if (!flush.fence && flush.retry)
                    {
                        span->flushWait = flush.error;
                        break;
                    }
                    if (!flush.fence)
                    {
                        // Census mode still waits for the work submitted before the failure.
                        if (state->unown(*span, state->admittedRecord(*span), "draw_flush_failed: " + flush.error, false, raised))
                        {
                            span->flushWait.clear();
                            span->hostTarget = submitted;
                            span->flushed    = true;
                        }
                        break;
                    }
                    span->flushWait.clear();
                    span->hostTarget = flush.fence;
                    span->flushed    = true;
                }
                if (draws->Completed() < span->hostTarget)
                    break;
            }
            if (!state->apply(lock, id, raised))
                break;
            span = state->find(id);
            if (!span || !state->releasable(*span) || !state->writeback)
                break;
            // RPTR last: the span's raw dword write pointer. A sealed span has none.
            if (span->writePointer && !state->write(state->writeback, *span->writePointer) &&
                !state->unown(*span, span->records.size(), std::format("guest_write_failed at {:#010x}", state->writeback), false, raised))
                break;
            ++state->stats.spansReleased;
            if (span->partial)
                ++state->stats.partialSpans;
            // RPTR now covers a replay kicked before this span.
            if (state->replayPointer && state->replayPointer->order < span->kickOrder)
                state->replayPointer.reset();
            state->spans.pop_front();
            released = true;
        }
    }
    state->notify(raised);
    return released;
}

void CompletionLedger::Close()
{
    std::lock_guard writes(state->imageWrites);
    std::lock_guard lock(state->mutex);
    state->closed = true;
}

bool CompletionLedger::WithImageWrites(const std::function<void(const GuestImageWriter&)>& body)
{
    std::lock_guard writes(state->imageWrites);
    {
        std::lock_guard lock(state->mutex);
        if (state->closed)
            return false;
    }
    body([&](const guest_d3d::GuestResolveImage& image)
         {
             return state->config.sink && !image.bytes.empty() && writeGuestImage(*state->config.sink, image);
         });
    return true;
}

LedgerSnapshot CompletionLedger::Snapshot() const
{
    std::lock_guard lock(state->mutex);
    auto            snapshot  = state->stats;
    snapshot.writebackPointer = state->writeback;
    if (state->failure)
        snapshot.failure = state->failure->detail;
    if (state->spans.empty())
        return snapshot;
    const auto& span = state->spans.front();
    if (!span.writePointer && span.records.empty())
        return snapshot;
    snapshot.pending      = true;
    snapshot.oldestSpan   = span.id;
    snapshot.oldestClosed = span.writePointer.has_value() || span.sealed;
    snapshot.writePointer = span.writePointer.value_or(0);
    snapshot.unowned      = span.unowned;
    snapshot.hostTarget   = span.hostTarget;
    if (state->config.draws)
        snapshot.hostCompleted = state->config.draws->Completed();
    if (!span.unowned.empty() && span.unownedRecord < span.records.size())
        snapshot.oldestRecord = describe(span.records[span.unownedRecord]);
    else if (span.pending)
    {
        const auto                                  holding = std::find_if(span.records.begin(), span.records.end(), [](const Record& record)
                                                                           {
                                              return record.pending;
                                                                           });
        const std::pair<std::uint64_t, std::size_t> key{ span.id, static_cast<std::size_t>(holding - span.records.begin()) };
        const bool                                  copying = std::any_of(state->openResolves.begin(), state->openResolves.end(), [&](const auto& open)
                                                                          {
                                             return open.second == key;
                                                                          });
        if (holding != span.records.end())
            snapshot.oldestRecord = describe(*holding) + (holding->kind == ProgressKind::Draw ? " awaiting its verdict"
                                                          : copying                           ? " awaiting its copy words"
                                                                                              : " awaiting its VdSwap");
    }
    else if (span.deferred && !span.writePointer)
        snapshot.oldestRecord = std::format("awaiting worker submit {:#010x}", *span.deferred);
    else if (!span.writePointer && !span.sealed)
        snapshot.oldestRecord = "awaiting CP_RB_WPTR kick";
    else if (!state->writeback)
        snapshot.oldestRecord = "awaiting writeback registration";
    else if (!span.flushWait.empty())
        snapshot.oldestRecord = "awaiting draw flush: " + span.flushWait;
    else if (span.lastAdmitted && snapshot.hostCompleted < span.hostTarget)
        snapshot.oldestRecord = "host fence";
    else if (span.applied < span.records.size())
        snapshot.oldestRecord = describe(span.records[span.applied]);
    return snapshot;
}

bool CompletionLedger::AwaitingWritebackRegistration() const
{
    std::lock_guard lock(state->mutex);
    if (state->writeback || state->spans.empty())
        return false;
    const auto& span = state->spans.front();
    return (span.writePointer || span.sealed) && !span.pending && span.unowned.empty() && state->releasable(span);
}

std::optional<std::string> CompletionLedger::Failure() const
{
    std::lock_guard lock(state->mutex);
    if (!state->failure)
        return std::nullopt;
    return state->failure->detail;
}

std::vector<CensusForm> CompletionLedger::Census() const
{
    std::lock_guard lock(state->mutex);
    return state->census;
}

} // namespace rerevved::gpu::completion
