#include "draw_effect_probe.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <ostream>

namespace rerevved::gpu::guest_d3d
{

std::atomic<DrawEffectProbe*> drawEffectProbe{ nullptr };

namespace detail
{

void (*drawEffectHook)(DrawEffectHookPoint point, std::uint64_t value) = nullptr;

} // namespace detail

namespace
{

// The sampled copies of G on this thread, allocated on its first sampled root
// so a thread that never samples pays nothing for them.
struct ThreadSample
{
    DrawEffectProbe*                                owner    = nullptr;
    std::uint32_t                                   graphics = 0;
    std::uint64_t                                   swap = 0, marker = 0;
    bool                                            concurrent = false;
    bool                                            tiled      = false;
    std::array<std::uint8_t, kDrawEffectGlobalSize> before, after;

    // A thread that exits holding a sample (its draw scope never ended) still
    // accounts for it.
    ~ThreadSample()
    {
        if (owner)
            DrawEffectProbe::AbandonAtThreadExit(owner);
    }
};

// Probes that exist, so a thread exiting after its probe was destroyed does
// not touch it. Leaked on purpose: thread teardown may outlive static destruction.
struct LiveProbes
{
    std::mutex                          mutex;
    std::vector<const DrawEffectProbe*> probes;
};

LiveProbes& liveProbes()
{
    static auto* live = new LiveProbes;
    return *live;
}

void hook(DrawEffectHookPoint point, std::uint64_t value = 0) noexcept
{
    if (auto* test = detail::drawEffectHook)
        test(point, value);
}

thread_local std::unique_ptr<ThreadSample> threadSample;

std::uint64_t mix(std::uint64_t value) noexcept
{
    // splitmix64 finalizer
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

void raiseTo(std::atomic<std::uint64_t>& slot, std::uint64_t value) noexcept
{
    auto seen = slot.load(std::memory_order_relaxed);
    while ((seen == kDrawEffectNoSwap || seen < value) && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed))
    {
    }
}

void lowerTo(std::atomic<std::uint64_t>& slot, std::uint64_t value) noexcept
{
    auto seen = slot.load(std::memory_order_relaxed);
    while (seen > value && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed))
    {
    }
}

constexpr bool within(std::uint32_t offset, std::uint32_t first, std::uint32_t last) noexcept
{
    return offset >= first && offset <= last;
}

void writeHex(std::ostream& out, const char* key, std::uint32_t value, int digits)
{
    char text[24];
    std::snprintf(text, sizeof text, "0x%0*x", digits, value);
    out << key << " = " << text << '\n';
}

} // namespace

bool ReadDrawEffectGlobal(const DrawEffectReaders& readers, const std::uint8_t* base, std::uint32_t graphics, std::span<std::uint8_t> out) noexcept
{
    if (!graphics || out.empty() || out.size() % 4 || graphics > 0xFFFFFFFFU - (out.size() - 1))
        return false;
    if (readers.block)
        return readers.block(base, graphics, out);
    if (!readers.word)
        return false;
    for (std::size_t offset = 0; offset < out.size(); offset += 4)
    {
        std::uint32_t value = 0;
        if (!readers.word(base, static_cast<std::uint32_t>(graphics + offset), value))
            return false;
        for (unsigned i = 0; i < 4; ++i)
            out[offset + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
    }
    return true;
}

const char* DrawEffectClassName(DrawEffectClass kind) noexcept
{
    switch (kind)
    {
        case DrawEffectClass::ExpectedDirty:
            return "expected_dirty";
        case DrawEffectClass::ExpectedCursor:
            return "expected_cursor";
        case DrawEffectClass::ExpectedCache:
            return "expected_cache";
        case DrawEffectClass::ExpectedList:
            return "expected_list";
        case DrawEffectClass::ExpectedAllocator:
            return "expected_allocator";
        case DrawEffectClass::Unexpected:
            break;
    }
    return "unexpected";
}

DrawEffectClass ClassifyDrawEffectOffset(std::uint32_t offset) noexcept
{
    // Rows 39F8-R1 (five dirty qwords), R5 (cursor) and R3 (caches of 0x826ADD88).
    if (within(offset, 0x00, 0x27))
        return DrawEffectClass::ExpectedDirty;
    if (within(offset, 0x30, 0x33))
        return DrawEffectClass::ExpectedCursor;
    if (offset == 0x2ABE || within(offset, 0x2884, 0x2887) || within(offset, 0x2920, 0x292B) || within(offset, 0x2954, 0x2957) ||
        within(offset, 0x2E38, 0x2E47) || within(offset, 0x319C, 0x319F))
        return DrawEffectClass::ExpectedCache;
    return DrawEffectClass::Unexpected;
}

DrawEffectClass ClassifyTiledDrawEffectOffset(std::uint32_t offset) noexcept
{
    const auto untiled = ClassifyDrawEffectOffset(offset);
    if (untiled != DrawEffectClass::Unexpected)
        return untiled;
    // The tile-record branch (g148:13576-13606) stores the cursor G+0x33C8; the
    // list growth 0x8269C170 (g45:12730-12864) stores the head, chunk, cursor and
    // limit of the header at G+0x33C0, G+0x33C4, G+0x33C8 and G+0x33CC.
    if (within(offset, 0x33C0, 0x33CF))
        return DrawEffectClass::ExpectedList;
    // Allocator write map for growth of 4224 dwords:
    // 0x8269B818 (g93:12757-12880) | Adds to G+0x3A54 (g93:12835).
    // 0x8269B618 ring path        | G+0x3A44 at g185:12984; G+0x3A4C at
    //   g185:13032,13114; wrap count G+0x3A48 at g185:13108.
    // Wrap cache                 | Its G+0x2ABE bit belongs to the cache set.
    // 0x8269C270 segment switch   | Writes the same words, so it is
    //   indistinguishable; G+0x34..0x3B and G+0x3A50..0x3A53 stay unexpected.
    // Other 0x8269B818 writes     | G+0x34/G+0x38 require G+0x33B0 == 0;
    //   failure bit G+0x2ABD |= 0x20 is a tripwire. Both stay unexpected.
    if (within(offset, 0x3A44, 0x3A4F) || within(offset, 0x3A54, 0x3A57))
        return DrawEffectClass::ExpectedAllocator;
    return DrawEffectClass::Unexpected;
}

bool DrawEffectSelectRandom(std::uint64_t index, std::uint32_t interval) noexcept
{
    return interval <= 1 || mix(index) % interval == 0;
}

bool DrawEffectThreadBuffersAllocated() noexcept
{
    return threadSample != nullptr;
}

DrawEffectProbe::DrawEffectProbe(DrawEffectConfig config)
: config_(config)
, interval_(config.interval ? config.interval : 1)
{
    for (auto& kind : changes_)
    {
        kind.count.assign(kDrawEffectGlobalSize, 0);
        kind.before.assign(kDrawEffectGlobalSize, 0);
        kind.after.assign(kDrawEffectGlobalSize, 0);
    }
    auto&           live = liveProbes();
    std::lock_guard lock(live.mutex);
    live.probes.push_back(this);
}

DrawEffectProbe::~DrawEffectProbe()
{
    auto&           live = liveProbes();
    std::lock_guard lock(live.mutex);
    std::erase(live.probes, this);
}

void DrawEffectProbe::AbandonAtThreadExit(DrawEffectProbe* probe) noexcept
{
    try
    {
        auto&           live = liveProbes();
        std::lock_guard lock(live.mutex);
        if (std::find(live.probes.begin(), live.probes.end(), probe) != live.probes.end())
            probe->abandoned_.fetch_add(1, std::memory_order_relaxed);
    }
    catch (...)
    {
    }
}

bool DrawEffectProbe::reserve(std::uint64_t& sequence) noexcept
{
    try
    {
        std::lock_guard lock(budgetMutex_);
        if (reserved_.load(std::memory_order_relaxed) >= config_.limit)
        {
            limited_.fetch_add(1, std::memory_order_relaxed);
            auto none = kDrawEffectNoSwap;
            limitHitSwap_.compare_exchange_strong(none, swap_, std::memory_order_relaxed);
            return false;
        }
        if (config_.perSwap && swapUsed_ >= config_.perSwap)
        {
            budgetSkipped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // A copy that fails after this keeps its slot; it is counted as unreadable.
        reserved_.fetch_add(1, std::memory_order_relaxed);
        if (config_.perSwap && ++swapUsed_ == config_.perSwap)
            budgetSwaps_.fetch_add(1, std::memory_order_relaxed);
        sequence = swap_;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void DrawEffectProbe::NoteSwap(std::uint64_t sequence) noexcept
{
    swapsSeen_.fetch_add(1, std::memory_order_relaxed);
    try
    {
        std::lock_guard lock(budgetMutex_);
        const auto      now      = roots_.load(std::memory_order_relaxed);
        const auto      previous = now - swapStartRoots_;
        swapStartRoots_          = now;
        swap_                    = sequence;
        swapUsed_                = 0;
        if (!config_.perSwap)
            return;
        // Aim for three quarters of the budget from the previous swap's root
        // count, spread over the swap, so the budget rarely cuts the swap's tail.
        const std::uint64_t share    = 3ULL * config_.perSwap;
        const std::uint64_t fitted   = (previous * 4 + share - 1) / share;
        const std::uint64_t interval = std::max<std::uint64_t>(fitted, config_.interval ? config_.interval : 1);
        interval_.store(static_cast<std::uint32_t>(std::min<std::uint64_t>(interval, 0xFFFFFFFFULL)), std::memory_order_relaxed);
    }
    catch (...)
    {
    }
}

bool DrawEffectProbe::BeginRoot(const DrawEffectReaders& readers, const std::uint8_t* base, std::uint32_t graphics, const std::atomic<std::uint64_t>& inFlight) noexcept
{
    const auto index    = roots_.fetch_add(1, std::memory_order_relaxed);
    const auto interval = interval_.load(std::memory_order_relaxed);
    if (!(config_.selector ? config_.selector(index, interval) : DrawEffectSelectRandom(index, interval)))
        return false;
    selected_.fetch_add(1, std::memory_order_relaxed);
    bool tiled = false;
    switch (IndexedBufferConsumeGate(readers.word, base, graphics))
    {
        case IndexedBufferGate::Admit:
            break;
        case IndexedBufferGate::TiledRecord:
            tiledSelected_.fetch_add(1, std::memory_order_relaxed);
            tiled = true;
            break;
        case IndexedBufferGate::TilingSet:
            gateSkipped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        case IndexedBufferGate::Unreadable:
            gateUnreadable_.fetch_add(1, std::memory_order_relaxed);
            return false;
    }
    if (!threadSample)
        threadSample.reset(new (std::nothrow) ThreadSample);
    auto* sample = threadSample.get();
    if (!sample)
    {
        snapshotUnreadable_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // A sample left armed by a root that never reached its end cannot be paired.
    if (sample->owner)
        sample->owner->AbandonRoot();
    std::uint64_t sequence = 0;
    if (!reserve(sequence))
        return false;
    hook(DrawEffectHookPoint::Reserved, sequence);
    // The root-start count first, then the in-flight count, then the copy. A root
    // that starts after the marker moves the count; one that started before it is
    // either still in flight at the second read or finished before the copy.
    const auto marker = rootStarts_.load(std::memory_order_seq_cst);
    hook(DrawEffectHookPoint::BeginAfterMarker);
    const bool busy = inFlight.load(std::memory_order_seq_cst) > 1;
    if (!ReadDrawEffectGlobal(readers, base, graphics, sample->before))
    {
        snapshotUnreadable_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    sample->owner      = this;
    sample->graphics   = graphics;
    sample->swap       = sequence;
    sample->marker     = marker;
    sample->concurrent = busy;
    sample->tiled      = tiled;
    return true;
}

void DrawEffectProbe::EndRoot(const DrawEffectReaders& readers, const std::uint8_t* base, const std::atomic<std::uint64_t>& inFlight) noexcept
{
    auto* sample = threadSample.get();
    if (!sample || sample->owner != this)
        return;
    sample->owner = nullptr;
    if (!ReadDrawEffectGlobal(readers, base, sample->graphics, sample->after))
    {
        snapshotUnreadable_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // After the copy: the in-flight count, then the root-start count. A root that
    // overlapped the sample either is still in flight or has already counted its start.
    const bool busy = inFlight.load(std::memory_order_seq_cst) > 1;
    hook(DrawEffectHookPoint::EndAfterInFlight);
    const bool started    = rootStarts_.load(std::memory_order_seq_cst) != sample->marker;
    const bool concurrent = sample->concurrent || busy || started;
    const bool changed    = std::memcmp(sample->before.data(), sample->after.data(), kDrawEffectGlobalSize) != 0;
    hook(DrawEffectHookPoint::EndBeforeCommit);
    try
    {
        // One commit: counters and rows land together, or not at all after Seal.
        std::lock_guard lock(mutex_);
        if (closed_)
            return;
        // The untiled counters and the sample-swap range describe untiled samples only.
        if (sample->tiled)
            ++(concurrent ? tiledConcurrent_ : tiledIsolated_);
        else
        {
            ++(concurrent ? sampledConcurrent_ : sampledIsolated_);
            if (firstSampleSwap_ == kDrawEffectNoSwap || sample->swap < firstSampleSwap_)
                firstSampleSwap_ = sample->swap;
            if (lastSampleSwap_ == kDrawEffectNoSwap || sample->swap > lastSampleSwap_)
                lastSampleSwap_ = sample->swap;
        }
        if (!changed)
            return;
        ++(sample->tiled ? tiledWithChange_ : sampledWithChange_);
        auto& kind = changes_[(sample->tiled ? 2 : 0) + (concurrent ? 1 : 0)];
        for (std::size_t i = 0; i < kDrawEffectGlobalSize; ++i)
        {
            if (sample->before[i] == sample->after[i])
                continue;
            if (!kind.count[i]++)
            {
                kind.before[i] = sample->before[i];
                kind.after[i]  = sample->after[i];
            }
        }
    }
    catch (...)
    {
    }
}

void DrawEffectProbe::Seal() noexcept
{
    try
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    catch (...)
    {
    }
}

void DrawEffectProbe::AbandonRoot() noexcept
{
    auto* sample = threadSample.get();
    if (!sample || sample->owner != this)
        return;
    sample->owner = nullptr;
    abandoned_.fetch_add(1, std::memory_order_relaxed);
}

DrawEffectSnapshot DrawEffectProbe::Snapshot() const
{
    DrawEffectSnapshot view;
    auto&              counts = view.counts;
    counts.roots              = roots_.load(std::memory_order_relaxed);
    counts.selected           = selected_.load(std::memory_order_relaxed);
    counts.gateSkipped        = gateSkipped_.load(std::memory_order_relaxed);
    counts.gateUnreadable     = gateUnreadable_.load(std::memory_order_relaxed);
    counts.tiledSelected      = tiledSelected_.load(std::memory_order_relaxed);
    counts.snapshotUnreadable = snapshotUnreadable_.load(std::memory_order_relaxed);
    counts.abandoned          = abandoned_.load(std::memory_order_relaxed);
    counts.limited            = limited_.load(std::memory_order_relaxed);
    counts.budgetSkipped      = budgetSkipped_.load(std::memory_order_relaxed);
    counts.budgetSwaps        = budgetSwaps_.load(std::memory_order_relaxed);
    counts.swapsSeen          = swapsSeen_.load(std::memory_order_relaxed);
    counts.limitHitSwap       = limitHitSwap_.load(std::memory_order_relaxed);
    {
        // The sample counters and the rows are read under the lock that commits them.
        std::lock_guard lock(mutex_);
        counts.sampledIsolated        = sampledIsolated_;
        counts.sampledConcurrent      = sampledConcurrent_;
        counts.sampled                = counts.sampledIsolated + counts.sampledConcurrent;
        counts.sampledWithChange      = sampledWithChange_;
        counts.firstSampleSwap        = firstSampleSwap_;
        counts.lastSampleSwap         = lastSampleSwap_;
        counts.tiledSampledIsolated   = tiledIsolated_;
        counts.tiledSampledConcurrent = tiledConcurrent_;
        counts.tiledSampled           = tiledIsolated_ + tiledConcurrent_;
        counts.tiledSampledWithChange = tiledWithChange_;
        for (std::uint32_t i = 0; i < kDrawEffectGlobalSize; ++i)
        {
            const auto isolated   = changes_[0].count[i];
            const auto concurrent = changes_[1].count[i];
            if (isolated || concurrent)
                view.offsets.push_back({ i, isolated, concurrent, changes_[0].before[i], changes_[0].after[i], changes_[1].before[i], changes_[1].after[i], ClassifyDrawEffectOffset(i) });
            const auto tiledIsolated   = changes_[2].count[i];
            const auto tiledConcurrent = changes_[3].count[i];
            if (tiledIsolated || tiledConcurrent)
                view.tiledOffsets.push_back({ i, tiledIsolated, tiledConcurrent, changes_[2].before[i], changes_[2].after[i], changes_[3].before[i], changes_[3].after[i], ClassifyTiledDrawEffectOffset(i) });
        }
    }
    for (const auto& row : view.offsets)
    {
        const bool unexpected = row.kind == DrawEffectClass::Unexpected;
        ++counts.changedOffsets;
        counts.isolatedOffsets += row.isolated != 0;
        counts.unexpectedOffsets += unexpected && row.isolated;
        counts.unexpectedConcurrent += unexpected && row.concurrent;
    }
    for (const auto& row : view.tiledOffsets)
    {
        const bool unexpected = row.kind == DrawEffectClass::Unexpected;
        ++counts.tiledChangedOffsets;
        ++counts.tiledClassOffsets[static_cast<std::size_t>(row.kind)];
        counts.tiledIsolatedOffsets += row.isolated != 0;
        counts.tiledUnexpectedOffsets += unexpected && row.isolated;
        counts.tiledUnexpectedConcurrent += unexpected && row.concurrent;
    }
    return view;
}

DrawEffectCounts DrawEffectProbe::Counts() const
{
    return Snapshot().counts;
}

std::vector<DrawEffectOffset> DrawEffectProbe::Offsets() const
{
    return Snapshot().offsets;
}

void DrawEffectProbe::Write(std::ostream& out) const
{
    const auto  view      = Snapshot();
    const auto& counts    = view.counts;
    const auto  writeSwap = [&out](const char* key, std::uint64_t value)
    {
        out << key << " = ";
        if (value == kDrawEffectNoSwap)
            out << "'none'";
        else
            out << value;
        out << '\n';
    };
    out << "[draw_effect_probe]\n";
    out << "scope = 'G only: [G, G+0x5B10); ring words and writes outside G are not observed'\n";
    out << "interval = " << config_.interval << '\n';
    out << "sample_limit = " << config_.limit << '\n';
    out << "samples_per_swap = " << config_.perSwap << '\n';
    writeHex(out, "global_size", kDrawEffectGlobalSize, 4);
    out << "roots = " << counts.roots << '\n';
    out << "selected = " << counts.selected << '\n';
    out << "gate_skipped = " << counts.gateSkipped << '\n';
    out << "gate_unreadable = " << counts.gateUnreadable << '\n';
    out << "snapshot_unreadable = " << counts.snapshotUnreadable << '\n';
    out << "sampled = " << counts.sampled << '\n';
    out << "sampled_isolated = " << counts.sampledIsolated << '\n';
    out << "sampled_concurrent = " << counts.sampledConcurrent << '\n';
    out << "sampled_with_change = " << counts.sampledWithChange << '\n';
    out << "abandoned = " << counts.abandoned << '\n';
    out << "swaps_seen = " << counts.swapsSeen << '\n';
    writeSwap("first_sample_swap", counts.firstSampleSwap);
    writeSwap("last_sample_swap", counts.lastSampleSwap);
    out << "swap_budget_skipped = " << counts.budgetSkipped << '\n';
    out << "swap_budget_swaps = " << counts.budgetSwaps << '\n';
    out << "total_limit_refused = " << counts.limited << '\n';
    writeSwap("total_limit_first_swap", counts.limitHitSwap);
    out << "changed_offsets = " << counts.changedOffsets << '\n';
    out << "isolated_offsets = " << counts.isolatedOffsets << '\n';
    out << "unexpected_offsets = " << counts.unexpectedOffsets << '\n';
    out << "unexpected_offsets_concurrent = " << counts.unexpectedConcurrent << '\n';
    // Roots whose tiling byte is the tiled-record one, apart from the untiled keys above.
    out << "tiled_selected = " << counts.tiledSelected << '\n';
    out << "tiled_sampled = " << counts.tiledSampled << '\n';
    out << "tiled_sampled_isolated = " << counts.tiledSampledIsolated << '\n';
    out << "tiled_sampled_concurrent = " << counts.tiledSampledConcurrent << '\n';
    out << "tiled_sampled_with_change = " << counts.tiledSampledWithChange << '\n';
    out << "tiled_changed_offsets = " << counts.tiledChangedOffsets << '\n';
    out << "tiled_isolated_offsets = " << counts.tiledIsolatedOffsets << '\n';
    out << "tiled_unexpected_offsets = " << counts.tiledUnexpectedOffsets << '\n';
    out << "tiled_unexpected_offsets_concurrent = " << counts.tiledUnexpectedConcurrent << '\n';
    for (std::size_t i = 0; i < kDrawEffectClassCount; ++i)
        out << "tiled_offsets_" << DrawEffectClassName(static_cast<DrawEffectClass>(i)) << " = " << counts.tiledClassOffsets[i] << '\n';
    const auto writeRows = [&out](const char* table, const std::vector<DrawEffectOffset>& rows)
    {
        for (const auto& row : rows)
        {
            out << "\n[[" << table << "]]\n";
            writeHex(out, "offset", row.offset, 4);
            out << "isolated_count = " << row.isolated << '\n';
            if (row.isolated)
            {
                writeHex(out, "isolated_first_before", row.isolatedBefore, 2);
                writeHex(out, "isolated_first_after", row.isolatedAfter, 2);
            }
            out << "concurrent_count = " << row.concurrent << '\n';
            if (row.concurrent)
            {
                writeHex(out, "concurrent_first_before", row.concurrentBefore, 2);
                writeHex(out, "concurrent_first_after", row.concurrentAfter, 2);
            }
            out << "class = '" << DrawEffectClassName(row.kind) << "'\n";
        }
    };
    writeRows("changed_offset", view.offsets);
    writeRows("changed_offset_tiled", view.tiledOffsets);
}

namespace
{

struct ProcessProbe
{
    std::filesystem::path path;
    DrawEffectProbe       probe;
    bool                  written = false;
};

std::mutex processMutex;
// Never freed: a hook that loaded the pointer before Close may still be using it.
ProcessProbe* processProbe = nullptr;

} // namespace

bool OpenDrawEffectProbe(const std::filesystem::path& path, std::string& error)
{
    std::lock_guard lock(processMutex);
    if (processProbe)
    {
        error = "native_draw_effect_probe is already open";
        return false;
    }
    if (std::filesystem::exists(path))
    {
        error = "native_draw_effect_probe must name a new file";
        return false;
    }
    // Claim the name now so a bad path fails at startup instead of at close.
    std::ofstream claim(path);
    if (!claim)
    {
        error = "could not open native_draw_effect_probe";
        return false;
    }
    claim.close();
    processProbe       = new ProcessProbe;
    processProbe->path = path;
    drawEffectProbe.store(&processProbe->probe, std::memory_order_release);
    return true;
}

bool CloseDrawEffectProbe(std::string& error)
{
    std::lock_guard lock(processMutex);
    if (!processProbe || processProbe->written)
        return true;
    drawEffectProbe.store(nullptr, std::memory_order_release);
    processProbe->written = true;
    // A sample whose commit is under way finishes first; later ones are dropped.
    processProbe->probe.Seal();
    std::ofstream out(processProbe->path, std::ios::trunc);
    if (out)
        processProbe->probe.Write(out);
    out.flush();
    if (!out)
    {
        error = "could not write native_draw_effect_probe";
        return false;
    }
    return true;
}

} // namespace rerevved::gpu::guest_d3d
