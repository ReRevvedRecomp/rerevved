#include "tiling_probe.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <span>
#include <string_view>

#include "gpu/guest_d3d/guest_memory.h"
#include "gpu/guest_d3d/resource_mirror_registry.h"
#include "gpu/guest_d3d/ring_stubs.h"

REXCVAR_DEFINE_BOOL(native_tiling_quiesce_check,
                    false,
                    "ReRevved",
                    "Bring-up only: observe the tile-replay chain without changing the output or the guest: whether the replay worker leaves "
                    "G[0x2AC0,0x2D00) as EndTiling left it, the shape of EndTiling's submits, the list tokens and the 0x8C callback, the "
                    "destinations of the list-selected stores, the ring sinks and the progress watchdog; tiling_* fields on the "
                    "native_frame_timing line and Native tiling log lines")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{

namespace
{

using namespace ring_layout;

constexpr std::uint32_t kEndTilingOptionalReturn = 0x826AC368;
constexpr std::uint32_t kEndTilingMiddleReturn   = 0x826AC3B4;
constexpr std::uint32_t kEndTilingFinalReturn    = 0x826AC3E8;
constexpr std::uint32_t kWaitCountOffset         = kTilingWorker + 0x2C;
constexpr std::uint32_t kQueuePushedOffset       = 0x38;
constexpr std::uint32_t kQueueConsumedOffset     = 0x3C;
constexpr std::uint32_t kWorkerListsOffset       = 0x3C;
constexpr std::uint32_t kWorkerPendingListOffset = 0x48;

// Guest bytes at a virtual address, or false when the pages are not readable.
bool readGuest(std::uint32_t address, std::span<std::uint8_t> bytes)
{
    auto* kernel = rex::system::kernel_state();
    if (!kernel || !kernel->memory() || !address || address > UINT32_MAX - bytes.size())
        return false;
    auto*      memory = kernel->memory();
    auto*      heap   = memory->LookupHeap(address);
    const auto last   = address + static_cast<std::uint32_t>(bytes.size() - 1);
    if (!heap || memory->LookupHeap(last) != heap ||
        !(static_cast<unsigned>(heap->QueryRangeAccess(address, last)) & static_cast<unsigned>(rex::memory::PageAccess::kReadOnly)))
        return false;
    return CopyGuestGuarded(bytes.data(), memory->TranslateVirtual<const std::uint8_t*>(address), bytes.size());
}

std::optional<std::uint32_t> readWord(std::uint32_t address)
{
    std::array<std::uint8_t, 4> bytes{};
    if (!readGuest(address, bytes))
        return std::nullopt;
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) | bytes[3];
}

std::optional<TilingWindow> readWindow(std::uint32_t device)
{
    if (device < 0x10000 || device > UINT32_MAX - kTilingWindowEnd)
        return std::nullopt;
    std::array<std::uint8_t, kTilingWindowWords * 4> bytes{};
    if (!readGuest(device + kTilingWindowBase, bytes))
        return std::nullopt;
    TilingWindow window{};
    for (std::size_t i = 0; i < window.size(); ++i)
        window[i] = (std::uint32_t(bytes[i * 4]) << 24) | (std::uint32_t(bytes[i * 4 + 1]) << 16) | (std::uint32_t(bytes[i * 4 + 2]) << 8) | bytes[i * 4 + 3];
    return window;
}

// *(*0x820009F0), the device the ring helpers name; zero before it exists.
std::uint32_t currentDevice()
{
    const auto global = readWord(kDeviceGlobal);
    if (!global || !*global)
        return 0;
    return readWord(*global).value_or(0);
}

// The allocation around `address`, for a log line.
std::string describeAllocation(std::uint32_t address)
{
    auto* kernel = rex::system::kernel_state();
    if (!kernel || !kernel->memory())
        return "unknown";
    auto* heap = kernel->memory()->LookupHeap(address);
    if (!heap)
        return "unmapped";
    std::uint32_t base = address, size = 0;
    if (!heap->QueryBaseAndSize(&base, &size))
        return "no region";
    return std::format("{:08X}+{:X}", base, size);
}

bool contains(const std::vector<std::uint64_t>& values, std::uint64_t value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

// Inserts `value` into a capped distinct set; true for a new one while the set holds at most kTilingLogLimit.
bool insertDistinct(std::vector<std::uint64_t>& values, std::uint64_t value, std::uint64_t& distinct)
{
    if (contains(values, value) || values.size() >= kTilingDistinctCap)
        return false;
    values.push_back(value);
    ++distinct;
    return values.size() <= kTilingLogLimit;
}

thread_local TilingArm submitArm = TilingArm::None;

void noteError() noexcept
{
    PublishedTilingProbe().NoteError();
}

std::string formatDiff(const QuiesceDiff& diff)
{
    std::string text;
    for (std::uint32_t i = 0; i < diff.logged; ++i)
        text += std::format(" +{:04X}:{:08X}->{:08X}", diff.first[i].offset, diff.first[i].before, diff.first[i].after);
    return text;
}

} // namespace

bool IsExpectedQuiesceOffset(std::uint32_t offset) noexcept
{
    if (offset == kRingWriteIndex || offset == kWaitCountOffset || offset == kWorkerPending || offset == kWorkerPending + 4)
        return true;
    for (std::size_t i = 0; i < kTilingQueueCount; ++i)
    {
        const std::uint32_t queue = kTilingQueueBase + kTilingQueueStride * static_cast<std::uint32_t>(i);
        if (offset == queue + kQueuePushedOffset || offset == queue + kQueueConsumedOffset)
            return true;
    }
    return false;
}

QuiesceDiff CompareQuiesceWindow(const TilingWindow& before, const TilingWindow& after) noexcept
{
    QuiesceDiff diff;
    for (std::size_t i = 0; i < before.size(); ++i)
    {
        if (before[i] == after[i])
            continue;
        const std::uint32_t offset = kTilingWindowBase + static_cast<std::uint32_t>(i) * 4;
        if (offset == kWaitCountOffset)
            diff.waitCount = true;
        if (IsExpectedQuiesceOffset(offset))
            continue;
        if (diff.logged < diff.first.size())
            diff.first[diff.logged++] = { offset, before[i], after[i] };
        ++diff.words;
    }
    return diff;
}

bool TilingWorkersIdle(const TilingWorkerState& state) noexcept
{
    if (state.lock != 0 || state.lists != 0 || state.pending != 0)
        return false;
    for (std::size_t i = 0; i < kTilingQueueCount; ++i)
        if (state.pushed[i] != state.consumed[i])
            return false;
    return true;
}

TilingToken ClassifyTilingToken(std::uint32_t word) noexcept
{
    if (!(word & 0x80000000U))
        return TilingToken::Record;
    if (word == 0x8C000000U)
        return TilingToken::T8C;
    if (word == 0x8D000000U)
        return TilingToken::T8D;
    if ((word & 0xC0000000U) == 0xC0000000U)
        return word == 0xC0000000U ? TilingToken::End : TilingToken::Link;
    switch (word >> 24)
    {
        case 0x80:
            return TilingToken::T80;
        case 0x81:
            return TilingToken::T81;
        case 0x82:
            return TilingToken::T82;
        case 0x83:
            return TilingToken::T83;
        case 0x84:
            return TilingToken::T84;
        case 0x85:
            return TilingToken::T85;
        case 0x86:
            return TilingToken::T86;
        case 0x87:
            return TilingToken::T87;
        case 0x88:
            return TilingToken::T88;
        case 0x89:
            return TilingToken::T89;
        case 0x8A:
            return TilingToken::T8A;
        case 0x8B:
            return TilingToken::T8B;
        case 0x8E:
            return TilingToken::T8E;
        default:
            return TilingToken::Other;
    }
}

const char* TilingTokenName(TilingToken token) noexcept
{
    static constexpr const char* names[kTilingTokenKinds] = { "record", "80", "81", "82", "83", "84", "85", "86", "87", "88", "89", "8a", "8b", "8e", "8c", "8d", "end", "link", "other" };
    return names[static_cast<std::size_t>(token)];
}

TilingSubmitSite ClassifyTilingSubmitSite(std::uint32_t lr) noexcept
{
    switch (lr)
    {
        case kEndTilingOptionalReturn:
            return TilingSubmitSite::Optional;
        case kEndTilingMiddleReturn:
            return TilingSubmitSite::Middle;
        case kEndTilingFinalReturn:
            return TilingSubmitSite::Final;
        default:
            return TilingSubmitSite::Other;
    }
}

const char* TilingSubmitSiteName(TilingSubmitSite site) noexcept
{
    static constexpr const char* names[kTilingSubmitSites] = { "opt", "mid", "final", "other" };
    return names[static_cast<std::size_t>(site)];
}

TilingWriteClass ClassifyTilingWrite(std::uint32_t address, std::uint32_t device, std::uint32_t physical, std::uint32_t ringPhysical, std::uint32_t ringBytes) noexcept
{
    if (device && address >= device && std::uint64_t(address) + 4 <= std::uint64_t(device) + kTilingDeviceBytes)
        return TilingWriteClass::Device;
    if (ringBytes && physical >= ringPhysical && std::uint64_t(physical) + 4 <= std::uint64_t(ringPhysical) + ringBytes)
        return TilingWriteClass::Ring;
    return TilingWriteClass::Other;
}

const char* TilingWriteSiteName(TilingWriteSite site) noexcept
{
    static constexpr const char* names[kTilingWriteSites] = { "b120", "b4c8", "b60c", "b6fc" };
    return names[static_cast<std::size_t>(site)];
}

const char* TilingWriteClassName(TilingWriteClass kind) noexcept
{
    static constexpr const char* names[kTilingWriteClasses] = { "device", "ring", "other" };
    return names[static_cast<std::size_t>(kind)];
}

TilingProbe::Generation TilingProbe::Sample()
{
    std::lock_guard lock(mutex_);
    return { bracketTotal_, interpreterTotal_, inFlight_ };
}

std::uint64_t TilingProbe::NoteBracketStart()
{
    std::lock_guard lock(mutex_);
    return interpreterTotal_;
}

bool TilingProbe::NoteBracketEnd(bool dropped, const TilingWindow* window, std::uint64_t started)
{
    std::lock_guard lock(mutex_);
    ++counts_.bracketEnd;
    if (dropped)
    {
        ++counts_.bracketDropped;
        return false;
    }
    if (!window)
    {
        ++counts_.bracketUnreadable;
        return false;
    }
    const bool replaced = pending_;
    if (replaced)
        ++counts_.quiesceUnobserved;
    pending_  = true;
    snapshot_ = *window;
    // Lists the worker started while EndTiling ran count: its next idle is then a real one.
    pendingInterpreter_ = started;
    pendingOverlap_     = interpreterTotal_ != started;
    ++bracketTotal_;
    ++counts_.quiesceSnapshots;
    return replaced;
}

void TilingProbe::NoteInterpreterEntry()
{
    std::lock_guard lock(mutex_);
    ++interpreterTotal_;
    ++inFlight_;
    ++counts_.workerLists;
}

void TilingProbe::NoteInterpreterExit()
{
    std::lock_guard lock(mutex_);
    if (inFlight_ == 0)
        ++errors_;
    else
        --inFlight_;
}

IdleVerdict TilingProbe::NoteWorkerIdle(const Generation& sampled, const TilingWorkerState& state, const TilingWindow* current, bool pendingNonzero)
{
    std::lock_guard lock(mutex_);
    ++counts_.workerIdle;
    IdleVerdict verdict;
    if (!pending_)
        return verdict;
    if (sampled.inFlight != 0 || sampled.bracket != bracketTotal_ || sampled.interpreter != interpreterTotal_)
    {
        // A list was running when the guest words were read, or a newer snapshot or list began
        // after: they say nothing about it. Another worker sharing W may consume queued work
        // after the fields read zero, so the counters alone cannot show it.
        ++counts_.quiesceStale;
        verdict.kind = IdleVerdict::Kind::Stale;
        return verdict;
    }
    if (!TilingWorkersIdle(state))
    {
        ++counts_.quiesceBusy;
        verdict.kind = IdleVerdict::Kind::Busy;
        return verdict;
    }
    if (interpreterTotal_ == pendingInterpreter_)
    {
        ++counts_.quiesceEarly;
        verdict.kind = IdleVerdict::Kind::Early;
        return verdict;
    }
    if (!current)
    {
        ++errors_;
        return verdict;
    }
    pending_ = false;
    if (pendingOverlap_)
        ++counts_.quiesceOverlap;
    if (pendingNonzero)
        ++counts_.quiescePendingNonzero;
    verdict.diff = CompareQuiesceWindow(snapshot_, *current);
    if (verdict.diff.waitCount)
        ++counts_.quiesceWaitCount;
    if (verdict.diff.words == 0)
    {
        ++counts_.quiesceOk;
        verdict.kind = IdleVerdict::Kind::Ok;
        return verdict;
    }
    ++counts_.quiesceDelta;
    counts_.quiesceDeltaWords += verdict.diff.words;
    verdict.kind = IdleVerdict::Kind::Delta;
    // Only the first kTilingLogLimit deltas are logged by the caller.
    if (deltaLogged_ >= kTilingLogLimit)
        verdict.diff.logged = 0;
    else
        ++deltaLogged_;
    return verdict;
}

void TilingProbe::NoteGuard(bool nonzero)
{
    std::lock_guard lock(mutex_);
    ++(nonzero ? counts_.guardNonzero : counts_.guardZero);
}

void TilingProbe::NoteSubmit(TilingSubmitSite site, std::uint32_t r7, std::uint32_t before, std::uint32_t after, TilingArm arm)
{
    std::lock_guard lock(mutex_);
    auto&           submit = counts_.submit[static_cast<std::size_t>(site)];
    ++submit.calls;
    ++(r7 == 0 ? submit.r7Zero : r7 == 1 ? submit.r7One
                                         : submit.r7Other);
    ++(arm == TilingArm::Deferred ? submit.deferred : arm == TilingArm::Direct ? submit.direct
                                                                               : submit.noArm);
    submit.beforeNonzero += before != 0 ? 1 : 0;
    submit.afterNonzero += after != 0 ? 1 : 0;
    ++(after - before == r7 ? submit.addsR7 : submit.addsOther);
}

bool TilingProbe::NoteToken(std::uint32_t word, bool handler)
{
    const auto      kind = ClassifyTilingToken(word);
    std::lock_guard lock(mutex_);
    switch (kind)
    {
        case TilingToken::Record:
        case TilingToken::T8C:
        case TilingToken::T8D:
        case TilingToken::End:
        case TilingToken::Link:
            // The interpreter reads these itself; the handler returns on 0x8C and 0x8D.
            if (handler)
                return false;
            break;
        default:
            // Control words are the handler's, wherever the interpreter first met them.
            if (!handler)
                return false;
            break;
    }
    ++counts_.token[static_cast<std::size_t>(kind)];
    if (kind != TilingToken::Other)
        return false;
    if (std::find(otherWords_.begin(), otherWords_.end(), word) != otherWords_.end() || otherWords_.size() >= kTilingDistinctCap)
        return false;
    otherWords_.push_back(word);
    return otherWords_.size() <= kTilingLogLimit;
}

bool TilingProbe::NoteInstalled(std::uint32_t function, std::uint32_t context)
{
    std::lock_guard lock(mutex_);
    return insertDistinct(installs_, (std::uint64_t(function) << 32) | context, counts_.installDistinct);
}

bool TilingProbe::NoteCallback(std::uint32_t function, std::uint32_t context)
{
    std::lock_guard lock(mutex_);
    ++counts_.callbackCalls;
    return insertDistinct(callbacks_, (std::uint64_t(function) << 32) | context, counts_.callbackDistinct);
}

bool TilingProbe::NoteWrite(TilingWriteSite site, TilingWriteClass kind, std::uint32_t address)
{
    std::lock_guard lock(mutex_);
    ++counts_.write[static_cast<std::size_t>(site)][static_cast<std::size_t>(kind)];
    if (kind != TilingWriteClass::Other)
        return false;
    std::uint64_t unused = 0;
    return insertDistinct(writes_, address, unused);
}

void TilingProbe::NoteRing(std::uint32_t physical, std::uint32_t log2)
{
    std::lock_guard lock(mutex_);
    ++counts_.ringRegistered;
    ringPhysical_ = physical;
    ringBytes_    = log2 < 29 ? std::uint32_t(1) << (log2 + 3) : 0;
}

void TilingProbe::RingExtent(std::uint32_t& physical, std::uint32_t& bytes)
{
    std::lock_guard lock(mutex_);
    physical = ringPhysical_;
    bytes    = ringBytes_;
}

bool TilingProbe::NotePublisher(std::uint32_t sink, std::uint32_t callback)
{
    std::lock_guard lock(mutex_);
    ++counts_.publisher;
    counts_.sink541C += sink ? 1 : 0;
    counts_.sink5B0C += callback ? 1 : 0;
    if (!sink && !callback)
        return false;
    std::uint64_t unused = 0;
    return insertDistinct(sinks_, (std::uint64_t(sink) << 32) | callback, unused);
}

bool TilingProbe::NoteWatchdog(int kind)
{
    std::lock_guard lock(mutex_);
    switch (kind)
    {
        case 0:
            ++counts_.watchdogEnter;
            return false;
        case 1:
            ++counts_.watchdogGlobal;
            return false;
        case 2:
            ++counts_.watchdogReset;
            break;
        default:
            ++counts_.watchdogZero;
            break;
    }
    return watchdogLogged_++ < kTilingLogLimit;
}

void TilingProbe::NoteError() noexcept
{
    try
    {
        std::lock_guard lock(mutex_);
        ++errors_;
    }
    catch (...)
    {
    }
}

TilingProbeCounts TilingProbe::Take()
{
    std::lock_guard   lock(mutex_);
    TilingProbeCounts taken = counts_;
    taken.errors += errors_;
    errors_ = 0;
    counts_ = TilingProbeCounts{};
    return taken;
}

std::atomic<std::int8_t> tilingProbeState{ -1 };

bool RefreshTilingProbeRequested() noexcept
{
    const bool requested = REXCVAR_GET(native_tiling_quiesce_check);
    tilingProbeState.store(requested ? 1 : 0, std::memory_order_relaxed);
    return requested;
}

TilingProbe& PublishedTilingProbe() noexcept
{
    static TilingProbe probe;
    return probe;
}

void TilingBracketScope::Begin() noexcept
{
    try
    {
        started_ = PublishedTilingProbe().NoteBracketStart();
    }
    catch (...)
    {
        noteError();
    }
}

void TilingBracketScope::End() noexcept
{
    try
    {
        auto&      probe   = PublishedTilingProbe();
        const auto window  = readWindow(device_);
        const bool dropped = static_cast<std::uint32_t>(result_) != 0;
        if (probe.NoteBracketEnd(dropped, window ? &*window : nullptr, started_))
            REXLOG_INFO("Native tiling probe: a bracket ended before the worker was idle for the previous one (device {:08X})", device_);
    }
    catch (...)
    {
        noteError();
    }
}

void TilingSubmitScope::Begin() noexcept
{
    submitArm = TilingArm::None;
    try
    {
        if (device_ <= UINT32_MAX - kWorkerPending - 4)
        {
            const auto word = readWord(device_ + kWorkerPending);
            readable_       = word.has_value();
            before_         = word.value_or(0);
        }
    }
    catch (...)
    {
        noteError();
    }
}

void TilingSubmitScope::End() noexcept
{
    try
    {
        const auto arm   = submitArm;
        submitArm        = TilingArm::None;
        const auto after = device_ <= UINT32_MAX - kWorkerPending - 4 ? readWord(device_ + kWorkerPending) : std::nullopt;
        if (!readable_ || !after)
        {
            PublishedTilingProbe().NoteError();
            return;
        }
        PublishedTilingProbe().NoteSubmit(ClassifyTilingSubmitSite(lr_), r7_, before_, *after, arm);
    }
    catch (...)
    {
        noteError();
    }
}

void NoteTilingSubmitArm(TilingArm arm) noexcept
{
    if (TilingProbeRequested())
        submitArm = arm;
}

void ObserveTilingGuard(std::uint32_t guard) noexcept
{
    if (!TilingProbeRequested())
        return;
    try
    {
        PublishedTilingProbe().NoteGuard(guard != 0);
    }
    catch (...)
    {
        noteError();
    }
}

void ObserveTilingInterpreter() noexcept
{
    if (!TilingProbeRequested())
        return;
    try
    {
        PublishedTilingProbe().NoteInterpreterEntry();
    }
    catch (...)
    {
        noteError();
    }
}

void ObserveTilingInterpreterExit() noexcept
{
    if (!TilingProbeRequested())
        return;
    try
    {
        PublishedTilingProbe().NoteInterpreterExit();
    }
    catch (...)
    {
        noteError();
    }
}

void ObserveTilingListWord(std::uint32_t address) noexcept
{
    if (!TilingProbeRequested())
        return;
    try
    {
        auto&      probe = PublishedTilingProbe();
        const auto word  = readWord(address);
        if (!word)
        {
            probe.NoteError();
            return;
        }
        if (probe.NoteToken(*word, false))
            REXLOG_INFO("Native tiling probe: list word {:08X} at {:08X}", *word, address);
        if (ClassifyTilingToken(*word) == TilingToken::T8C)
        {
            const auto function = readWord(address + 4);
            const auto context  = readWord(address + 8);
            if (!function || !context)
            {
                probe.NoteError();
                return;
            }
            if (probe.NoteInstalled(*function, *context))
                REXLOG_INFO("Native tiling probe: token 0x8C installs function={:08X} context={:08X}", *function, *context);
        }
    }
    catch (...)
    {
        noteError();
    }
}

void ObserveTilingHandlerToken(std::uint32_t word) noexcept
{
    if (!TilingProbeRequested())
        return;
    try
    {
        if (PublishedTilingProbe().NoteToken(word, true))
            REXLOG_INFO("Native tiling probe: handler token {:08X}", word);
    }
    catch (...)
    {
        noteError();
    }
}

void ObserveTilingHandlerLink(std::uint32_t word) noexcept
{
    if (!TilingProbeRequested())
        return;
    try
    {
        // The handler follows a link word itself; the interpreter never sees it.
        PublishedTilingProbe().NoteToken(word, false);
    }
    catch (...)
    {
        noteError();
    }
}

void ObserveTilingCallback(std::uint32_t function, std::uint32_t worker) noexcept
{
    if (!TilingProbeRequested())
        return;
    try
    {
        auto&      probe   = PublishedTilingProbe();
        const auto context = worker <= UINT32_MAX - 0x14 - 4 ? readWord(worker + 0x14) : std::nullopt;
        if (!context)
        {
            probe.NoteError();
            return;
        }
        if (probe.NoteCallback(function, *context))
            REXLOG_INFO("Native tiling probe: callback call W+0x10={:08X} W+0x14={:08X}", function, *context);
    }
    catch (...)
    {
        noteError();
    }
}

void ObserveTilingWrite(TilingWriteSite site, std::uint32_t destination) noexcept
{
    if (!TilingProbeRequested())
        return;
    try
    {
        auto&         probe        = PublishedTilingProbe();
        std::uint32_t ringPhysical = 0, ringBytes = 0;
        probe.RingExtent(ringPhysical, ringBytes);
        const auto device = currentDevice();
        const auto kind   = ClassifyTilingWrite(destination, device, GuestPhysicalAddress(destination), ringPhysical, ringBytes);
        if (probe.NoteWrite(site, kind, destination))
            REXLOG_INFO("Native tiling probe: list-selected store {} to {:08X} outside the device {:08X} and the ring ({:08X}+{:X}); allocation {}",
                        TilingWriteSiteName(site),
                        destination,
                        device,
                        ringPhysical,
                        ringBytes,
                        describeAllocation(destination));
    }
    catch (...)
    {
        noteError();
    }
}

void ObserveTilingWorkerIdle(std::uint32_t worker) noexcept
{
    if (!TilingProbeRequested())
        return;
    try
    {
        auto& probe = PublishedTilingProbe();
        if (worker < kTilingWorker + 0x10000)
        {
            probe.NoteError();
            return;
        }
        const std::uint32_t device  = worker - kTilingWorker;
        const auto          sampled = probe.Sample();
        TilingWorkerState   state;
        bool                readable = true;
        const auto          word     = [&](std::uint32_t offset)
        {
            const auto value = readWord(device + offset);
            readable         = readable && value.has_value();
            return value.value_or(0);
        };
        state.lock    = word(kTilingWorker);
        state.lists   = word(kTilingWorker + kWorkerListsOffset);
        state.pending = word(kTilingWorker + kWorkerPendingListOffset);
        for (std::size_t i = 0; i < kTilingQueueCount; ++i)
        {
            const std::uint32_t queue = kTilingQueueBase + kTilingQueueStride * static_cast<std::uint32_t>(i);
            state.pushed[i]           = word(queue + kQueuePushedOffset);
            state.consumed[i]         = word(queue + kQueueConsumedOffset);
        }
        const bool pendingNonzero = word(kWorkerPending) != 0;
        const auto current        = readWindow(device);
        if (!readable)
        {
            probe.NoteError();
            return;
        }
        const auto verdict = probe.NoteWorkerIdle(sampled, state, current ? &*current : nullptr, pendingNonzero);
        if (verdict.kind == IdleVerdict::Kind::Delta && verdict.diff.logged)
            REXLOG_INFO("Native tiling probe: quiesce delta device={:08X} words={} pending_nonzero={}:{}", device, verdict.diff.words, pendingNonzero ? 1 : 0, formatDiff(verdict.diff));
    }
    catch (...)
    {
        noteError();
    }
}

void ObserveTilingPublisher(std::uint32_t device) noexcept
{
    if (!TilingProbeRequested())
        return;
    try
    {
        auto&      probe    = PublishedTilingProbe();
        const auto sink     = device <= UINT32_MAX - kDeviceEnd ? readWord(device + kCpuFenceSink) : std::nullopt;
        const auto callback = device <= UINT32_MAX - kDeviceEnd ? readWord(device + kRingCallback) : std::nullopt;
        if (!sink || !callback)
        {
            probe.NoteError();
            return;
        }
        if (probe.NotePublisher(*sink, *callback))
        {
            const auto vtable = *sink ? readWord(*sink) : std::nullopt;
            REXLOG_INFO("Native tiling probe: publisher sinks G+0x541C={:08X} (vptr {:08X}) G+0x5B0C={:08X}", *sink, vtable.value_or(0), *callback);
        }
    }
    catch (...)
    {
        noteError();
    }
}

void ObserveTilingWatchdog(int kind, std::uint32_t device) noexcept
{
    if (!TilingProbeRequested())
        return;
    try
    {
        auto& probe = PublishedTilingProbe();
        if (!probe.NoteWatchdog(kind))
            return;
        const auto pending = device <= UINT32_MAX - kDeviceEnd ? readWord(device + kWorkerPending) : std::nullopt;
        const auto input   = device <= UINT32_MAX - kDeviceEnd ? readWord(device + 0x3478) : std::nullopt;
        REXLOG_INFO("Native tiling probe: watchdog {} G+0x2B00={:08X} G+0x3478={:08X}", kind == 2 ? "reset" : "reread zero", pending.value_or(0), input.value_or(0));
    }
    catch (...)
    {
        noteError();
    }
}

void ObserveTilingRing(std::uint32_t physical, std::uint32_t log2) noexcept
{
    if (!TilingProbeRequested())
        return;
    try
    {
        PublishedTilingProbe().NoteRing(physical, log2);
    }
    catch (...)
    {
        noteError();
    }
}

std::string FormatTilingProbeCounts(const TilingProbeCounts& counts)
{
    std::string fields = std::format(
        " tiling_bracket_end={} tiling_bracket_dropped={} tiling_bracket_unreadable={}"
        " tiling_quiesce_snapshots={} tiling_quiesce_ok={} tiling_quiesce_delta={} tiling_quiesce_delta_words={} tiling_quiesce_wait_count={}"
        " tiling_quiesce_unobserved={} tiling_quiesce_busy={} tiling_quiesce_early={} tiling_quiesce_stale={} tiling_quiesce_overlap={} tiling_quiesce_pending_nonzero={}"
        " tiling_worker_idle={} tiling_worker_lists={} tiling_submit_guard_nonzero={} tiling_submit_guard_zero={}",
        counts.bracketEnd,
        counts.bracketDropped,
        counts.bracketUnreadable,
        counts.quiesceSnapshots,
        counts.quiesceOk,
        counts.quiesceDelta,
        counts.quiesceDeltaWords,
        counts.quiesceWaitCount,
        counts.quiesceUnobserved,
        counts.quiesceBusy,
        counts.quiesceEarly,
        counts.quiesceStale,
        counts.quiesceOverlap,
        counts.quiescePendingNonzero,
        counts.workerIdle,
        counts.workerLists,
        counts.guardNonzero,
        counts.guardZero);
    for (std::size_t i = 0; i < kTilingSubmitSites; ++i)
    {
        const auto& s    = counts.submit[i];
        const auto  name = TilingSubmitSiteName(static_cast<TilingSubmitSite>(i));
        fields += std::format(
            " tiling_submit_{0}_calls={1} tiling_submit_{0}_r7_0={2} tiling_submit_{0}_r7_1={3} tiling_submit_{0}_r7_other={4}"
            " tiling_submit_{0}_deferred={5} tiling_submit_{0}_direct={6} tiling_submit_{0}_noarm={7}"
            " tiling_submit_{0}_before_nonzero={8} tiling_submit_{0}_after_nonzero={9} tiling_submit_{0}_adds_r7={10} tiling_submit_{0}_adds_other={11}",
            name,
            s.calls,
            s.r7Zero,
            s.r7One,
            s.r7Other,
            s.deferred,
            s.direct,
            s.noArm,
            s.beforeNonzero,
            s.afterNonzero,
            s.addsR7,
            s.addsOther);
    }
    for (std::size_t i = 0; i < kTilingTokenKinds; ++i)
        fields += std::format(" tiling_token_{}={}", TilingTokenName(static_cast<TilingToken>(i)), counts.token[i]);
    fields += std::format(" tiling_callback_calls={} tiling_callback_distinct={} tiling_token_8c_installs={}", counts.callbackCalls, counts.callbackDistinct, counts.installDistinct);
    for (std::size_t site = 0; site < kTilingWriteSites; ++site)
        for (std::size_t kind = 0; kind < kTilingWriteClasses; ++kind)
            fields += std::format(" tiling_write_{}_{}={}", TilingWriteSiteName(static_cast<TilingWriteSite>(site)), TilingWriteClassName(static_cast<TilingWriteClass>(kind)), counts.write[site][kind]);
    fields += std::format(" tiling_ring_registered={} tiling_sink_publisher={} tiling_sink_541c={} tiling_sink_5b0c={}"
                          " tiling_watchdog_enter={} tiling_watchdog_global={} tiling_watchdog_reset={} tiling_watchdog_zero={} tiling_probe_error={}",
                          counts.ringRegistered,
                          counts.publisher,
                          counts.sink541C,
                          counts.sink5B0C,
                          counts.watchdogEnter,
                          counts.watchdogGlobal,
                          counts.watchdogReset,
                          counts.watchdogZero,
                          counts.errors);
    return fields;
}

} // namespace rerevved::gpu::guest_d3d
