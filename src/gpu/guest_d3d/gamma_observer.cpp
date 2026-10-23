#include "gamma_observer.h"

#include <rex/crypto/sha256.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <string_view>

#if REREVVED_DEVELOPER
REXCVAR_DEFINE_BOOL(native_gamma_observe_check,
                    false,
                    "ReRevved",
                    "Bring-up only: observe the guest display gamma table from the setter 0x8268FE08 and the emitter 0x826A7040 without changing "
                    "the output; gamma_obs_* fields on the native_frame_timing line and a log line with the SHA-256 of each changed setter table")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif

REXCVAR_DEFINE_BOOL(native_gamma_apply,
                    true,
                    "ReRevved",
                    "On by default for renderer=native, observes the guest gamma setter and emitter itself: native frames use the table of the last completed "
                    "guest gamma emission (the linear default before the first) instead of the assumed identity ramp, and a swap without "
                    "presented work adopts a changed table instead of failing the session; other renderers ignore it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu::guest_d3d
{

namespace
{

// Set once at startup from the renderer selection; read by every hook.
std::atomic<bool> nativeRenderer{ false };

// Guest bytes at a virtual address, or false when the pages are not readable.
bool readGuest(std::uint32_t address, std::span<std::uint8_t> bytes)
{
    auto* kernel = rex::system::kernel_state();
    if (!kernel || !kernel->memory() || !address || address > UINT32_MAX - bytes.size())
    {
        return false;
    }
    auto*      memory = kernel->memory();
    auto*      heap   = memory->LookupHeap(address);
    const auto last   = address + static_cast<std::uint32_t>(bytes.size() - 1);
    if (!heap || memory->LookupHeap(last) != heap ||
        !(static_cast<unsigned>(heap->QueryRangeAccess(address, last)) & static_cast<unsigned>(rex::memory::PageAccess::kReadOnly)))
    {
        return false;
    }
    std::memcpy(bytes.data(), memory->TranslateVirtual<const std::uint8_t*>(address), bytes.size());
    return true;
}

GuestGammaTable makeLinearTable() noexcept
{
    GuestGammaTable table{};
    for (std::uint32_t i = 0; i < table.size(); ++i)
    {
        const std::uint32_t value = i * 0x3FF / 0xFF;
        table[i]                  = (value << 20) | (value << 10) | value;
    }
    return table;
}

} // namespace

GammaDecode DecodeGuestGammaTable(std::span<const std::uint8_t> bytes, GuestGammaTable& table) noexcept
{
    table = {};
    if (bytes.size() != kGuestGammaBytes)
    {
        return GammaDecode::BadSize;
    }
    GuestGammaTable decoded{};
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        for (std::size_t i = 0; i < decoded.size(); ++i)
        {
            const auto offset = channel * 512 + i * 2;
            const auto value  = (std::uint32_t(bytes[offset]) << 8) | bytes[offset + 1];
            if (value & 63U)
            {
                return GammaDecode::Unaligned;
            }
            decoded[i] |= (value >> 6) << ((2 - channel) * 10);
        }
    }
    table = decoded;
    return GammaDecode::Ok;
}

const GuestGammaTable& LinearGuestGammaTable() noexcept
{
    static const GuestGammaTable table = makeLinearTable();
    return table;
}

#if REREVVED_DEVELOPER
std::string GuestGammaTableSha256(const GuestGammaTable& table)
{
    std::array<std::uint8_t, kGuestGammaEntries * 4> packed{};
    for (std::size_t i = 0; i < table.size(); ++i)
    {
        for (std::size_t byte = 0; byte < 4; ++byte)
        {
            packed[i * 4 + byte] = static_cast<std::uint8_t>(table[i] >> (byte * 8));
        }
    }
    return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(packed.data()), packed.size()));
}

std::uint32_t CountGammaDifferences(const GuestGammaTable& a, const GuestGammaTable& b) noexcept
{
    std::uint32_t differing = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        differing += a[i] != b[i] ? 1U : 0U;
    }
    return differing;
}
#endif

void GammaObserver::adoptDevice(std::uint32_t device)
{
    if (device == device_)
    {
        return;
    }
#if REREVVED_DEVELOPER
    if (device_)
    {
        ++counts_.deviceChange;
    }
    setterSeen_  = false;
    setterTable_ = {};
#endif
    device_ = device;
    inFlight_.clear();
}

GammaSetterOutcome GammaObserver::NoteSetter(std::uint32_t device, std::span<const std::uint8_t> converted)
{
    std::lock_guard    lock(mutex_);
    GammaSetterOutcome outcome;
#if REREVVED_DEVELOPER
    ++counts_.setter;
#endif
    if (!device)
    {
#if REREVVED_DEVELOPER
        ++counts_.errors;
#endif
        return outcome;
    }
    adoptDevice(device);
#if REREVVED_DEVELOPER
    GuestGammaTable table{};
    if (DecodeGuestGammaTable(converted, table) != GammaDecode::Ok)
    {
        ++counts_.setterDecodeFail;
        return outcome;
    }
    outcome.decoded = true;
    outcome.table   = table;
    outcome.changed = !setterSeen_ || table != setterTable_;
    if (outcome.changed)
    {
        ++counts_.setterChanged;
        setterTable_ = table;
        setterSeen_  = true;
    }
#else
    (void)converted;
#endif
    return outcome;
}

void GammaObserver::NotePwlSetter(std::uint32_t device)
{
    std::lock_guard lock(mutex_);
#if REREVVED_DEVELOPER
    ++counts_.pwlSetter;
#endif
    if (!device)
    {
#if REREVVED_DEVELOPER
        ++counts_.errors;
#endif
        return;
    }
    adoptDevice(device);
}

std::uint64_t GammaObserver::BeginEmission(std::uint32_t device, std::span<const std::uint8_t> emitted, std::span<const std::uint8_t> retained, bool secondSite)
{
    std::lock_guard lock(mutex_);
#if REREVVED_DEVELOPER
    ++counts_.emit;
    if (secondSite)
    {
        ++counts_.emitSecondSite;
    }
#else
    (void)retained;
    (void)secondSite;
#endif
    if (!device)
    {
#if REREVVED_DEVELOPER
        ++counts_.errors;
#endif
        return 0;
    }
    adoptDevice(device);
#if REREVVED_DEVELOPER
    if (!inFlight_.empty())
    {
        ++counts_.emitOverlap;
    }
    if (emitted.size() != kGuestGammaBytes || retained.size() != kGuestGammaBytes)
    {
        // An unreadable table or device copy says nothing about whether they agree.
        ++counts_.errors;
    }
    else if (std::equal(emitted.begin(), emitted.end(), retained.begin()))
    {
        ++counts_.emitRetainedEqual;
    }
    else
    {
        ++counts_.emitRetainedMismatch;
    }
#endif
    InFlight emission;
    emission.decoded = DecodeGuestGammaTable(emitted, emission.table) == GammaDecode::Ok;
#if REREVVED_DEVELOPER
    if (!emission.decoded)
    {
        ++counts_.emitDecodeFail;
    }
#endif
    return track(std::move(emission));
}

std::uint64_t GammaObserver::BeginPwlEmission(std::uint32_t device)
{
    std::lock_guard lock(mutex_);
#if REREVVED_DEVELOPER
    ++counts_.pwlEmit;
#endif
    if (!device)
    {
#if REREVVED_DEVELOPER
        ++counts_.errors;
#endif
        return 0;
    }
    adoptDevice(device);
#if REREVVED_DEVELOPER
    if (!inFlight_.empty())
    {
        ++counts_.emitOverlap;
    }
#endif
    InFlight emission;
    emission.pwl = true;
    return track(std::move(emission));
}

std::uint64_t GammaObserver::track(InFlight emission)
{
    // A guest cannot have more in flight than it has threads. A runaway count
    // evicts the oldest; EndEmission recognizes its return by the ticket order.
    if (inFlight_.size() >= kMaxEmissionsInFlight)
    {
        inFlight_.erase(inFlight_.begin());
    }
    emission.ticket = ++tickets_;
    inFlight_.push_back(std::move(emission));
    return inFlight_.back().ticket;
}

void GammaObserver::EndEmission(std::uint64_t ticket)
{
    std::lock_guard lock(mutex_);
    const auto      found = std::find_if(inFlight_.begin(), inFlight_.end(), [&](const InFlight& emission)
                                         {
                                        return emission.ticket == ticket;
                                         });
    if (found == inFlight_.end())
    {
        // A dropped or unknown ticket leaves LUT state unknown; an unissued
        // ticket is an error.
#if REREVVED_DEVELOPER
        if (ticket == 0 || ticket > tickets_)
        {
            ++counts_.errors;
        }
        else
        {
            ++counts_.emitEvicted;
        }
#endif
        staleThrough_  = tickets_;
        emissionKnown_ = false;
        emissionTable_ = {};
        ++emissionSequence_;
        return;
    }
    // The PWL layout is not a 256-entry table: the LUT then holds something the observer cannot name.
    const bool known = !found->pwl && found->decoded && found->ticket > staleThrough_;
#if REREVVED_DEVELOPER
    if (!found->pwl)
    {
        ++counts_.emitComplete;
    }
#endif
    ++emissionSequence_;
    emissionKnown_ = known;
    emissionTable_ = known ? found->table : GuestGammaTable{};
    inFlight_.erase(found);
}

GammaSwapView GammaObserver::BeginSwap(const GuestGammaTable& identity)
{
    std::lock_guard lock(mutex_);
    GammaSwapView   view;
    view.emissionSequence = emissionSequence_;
    view.assumed          = emissionSequence_ != 0 && !emissionKnown_;
    view.table            = emissionSequence_ == 0 ? LinearGuestGammaTable() : emissionKnown_ ? emissionTable_
                                                                                              : identity;
#if REREVVED_DEVELOPER
    ++counts_.swap;
    if (emissionSequence_ == 0)
    {
        ++counts_.swapBeforeEmit;
    }
    // A setter whose table is not the one the LUT holds has not been emitted yet.
    if (setterSeen_ && !view.assumed && setterTable_ != view.table)
    {
        ++counts_.swapPendingSetter;
    }
    if (swapSeen_ && swapTable_ != view.table)
    {
        ++counts_.swapTableChanged;
    }
    swapSeen_                   = true;
    swapTable_                  = view.table;
    counts_.identityDiffEntries = CountGammaDifferences(view.table, identity);
#else
    (void)identity;
#endif
    return view;
}

void GammaObserver::EndSwap(const GammaSwapView& view, bool noWork)
{
#if REREVVED_DEVELOPER
    std::lock_guard lock(mutex_);
    if (!noWork)
    {
        presentedEmission_ = view.emissionSequence;
    }
    else if (view.emissionSequence != presentedEmission_)
    {
        ++counts_.noworkSwapChanged;
    }
#else
    (void)view;
    (void)noWork;
#endif
}

#if REREVVED_DEVELOPER
void GammaObserver::NoteError() noexcept
{
    errors_.fetch_add(1, std::memory_order_relaxed);
}

GammaCounts GammaObserver::Take()
{
    std::lock_guard lock(mutex_);
    GammaCounts     taken = counts_;
    taken.errors += errors_.exchange(0, std::memory_order_relaxed);
    counts_ = GammaCounts{};
    // The gauge outlives the window it was set in.
    counts_.identityDiffEntries = taken.identityDiffEntries;
    return taken;
}
#endif

GammaSwapChoice ChooseSwapGamma(bool apply, const std::optional<GammaSwapView>& view, const GuestGammaTable& identity)
{
    GammaSwapChoice choice;
    choice.table   = identity;
    choice.assumed = true;
    if (apply && view)
    {
        choice.table   = view->table;
        choice.assumed = view->assumed;
    }
    return choice;
}

void SetGammaNativeRenderer(bool native) noexcept
{
    nativeRenderer.store(native, std::memory_order_relaxed);
}

bool GammaApplyRequested() noexcept
{
    return nativeRenderer.load(std::memory_order_relaxed) && REXCVAR_GET(native_gamma_apply);
}

bool GammaObserveRequested() noexcept
{
#if REREVVED_DEVELOPER
    return REXCVAR_GET(native_gamma_observe_check) || GammaApplyRequested();
#else
    return GammaApplyRequested();
#endif
}

GammaObserver& PublishedGammaObserver() noexcept
{
    static GammaObserver observer;
    return observer;
}

namespace
{

// A hook failure leaves the LUT unknown; a developer build also counts it.
void NoteGammaError([[maybe_unused]] GammaObserver& observer) noexcept
{
#if REREVVED_DEVELOPER
    observer.NoteError();
#endif
}

} // namespace

void ObserveGammaSetter(std::uint32_t device, std::uint32_t stack) noexcept
{
    if (!GammaObserveRequested())
    {
        return;
    }
    auto& observer = PublishedGammaObserver();
    try
    {
#if REREVVED_DEVELOPER
        std::array<std::uint8_t, kGuestGammaBytes> converted{};
        // The converted table is 80 bytes into the setter's frame.
        const bool                      readable = stack <= UINT32_MAX - 80 && readGuest(stack + 80, converted);
        const auto                      outcome  = observer.NoteSetter(device, readable ? std::span<const std::uint8_t>(converted) : std::span<const std::uint8_t>());
        static std::atomic<std::size_t> logged{ 0 };
        if (outcome.changed && logged.fetch_add(1, std::memory_order_relaxed) < kGuestGammaSetterLogs)
        {
            REXLOG_INFO("Native gamma observer: setter device={:08X} table_sha256={}", device, GuestGammaTableSha256(outcome.table));
        }
#else
        // Only the device matters outside a developer build: the table is never compared or logged.
        (void)stack;
        observer.NoteSetter(device, {});
#endif
    }
    catch (...)
    {
        NoteGammaError(observer);
    }
}

void ObserveGammaPwlSetter(std::uint32_t device) noexcept
{
    if (!GammaObserveRequested())
    {
        return;
    }
    auto& observer = PublishedGammaObserver();
    try
    {
        observer.NotePwlSetter(device);
    }
    catch (...)
    {
        NoteGammaError(observer);
    }
}

std::uint64_t ObserveGammaEmissionBegin(std::uint32_t device, std::uint32_t table, std::uint32_t caller) noexcept
{
    if (!GammaObserveRequested())
    {
        return 0;
    }
    auto& observer = PublishedGammaObserver();
    try
    {
        std::array<std::uint8_t, kGuestGammaBytes> emitted{}, retained{};
        const bool                                 emittedReadable  = readGuest(table, emitted);
        bool                                       retainedReadable = false;
#if REREVVED_DEVELOPER
        // The device copy only feeds the retained-table comparison.
        retainedReadable = device <= UINT32_MAX - kGuestGammaRetainedOffset && readGuest(device + kGuestGammaRetainedOffset, retained);
#endif
        return observer.BeginEmission(device,
                                      emittedReadable ? std::span<const std::uint8_t>(emitted) : std::span<const std::uint8_t>(),
                                      retainedReadable ? std::span<const std::uint8_t>(retained) : std::span<const std::uint8_t>(),
                                      caller != kGuestGammaEmitterReturn);
    }
    catch (...)
    {
        NoteGammaError(observer);
        return 0;
    }
}

void ObserveGammaEmissionEnd(std::uint64_t ticket) noexcept
{
    // Zero is a begin that failed while observing: it still reaches the observer, which
    // counts an error and no longer trusts the LUT. Off, a begin returns zero by design.
    if (!GammaObserveRequested())
    {
        return;
    }
    auto& observer = PublishedGammaObserver();
    try
    {
        observer.EndEmission(ticket);
    }
    catch (...)
    {
        NoteGammaError(observer);
    }
}

std::uint64_t ObserveGammaPwlEmissionBegin(std::uint32_t device) noexcept
{
    if (!GammaObserveRequested())
    {
        return 0;
    }
    auto& observer = PublishedGammaObserver();
    try
    {
        return observer.BeginPwlEmission(device);
    }
    catch (...)
    {
        NoteGammaError(observer);
        return 0;
    }
}

#if REREVVED_DEVELOPER
std::string FormatGammaCounts(const GammaCounts& counts)
{
    return std::format(" gamma_obs_setter={} gamma_obs_setter_changed={} gamma_obs_setter_decode_fail={} gamma_obs_pwl_setter={}"
                       " gamma_obs_emit={} gamma_obs_emit_complete={} gamma_obs_emit_decode_fail={} gamma_obs_pwl_emit={}"
                       " gamma_obs_emit_retained_equal={} gamma_obs_emit_retained_mismatch={} gamma_obs_emit_overlap={} gamma_obs_emit_evicted={} gamma_obs_emit_second_site={}"
                       " gamma_obs_device_change={} gamma_obs_swap={} gamma_obs_swap_before_emit={} gamma_obs_swap_pending_setter={}"
                       " gamma_obs_swap_table_changed={} gamma_obs_nowork_swap_changed={} gamma_obs_error={} gamma_obs_identity_diff_entries={}",
                       counts.setter,
                       counts.setterChanged,
                       counts.setterDecodeFail,
                       counts.pwlSetter,
                       counts.emit,
                       counts.emitComplete,
                       counts.emitDecodeFail,
                       counts.pwlEmit,
                       counts.emitRetainedEqual,
                       counts.emitRetainedMismatch,
                       counts.emitOverlap,
                       counts.emitEvicted,
                       counts.emitSecondSite,
                       counts.deviceChange,
                       counts.swap,
                       counts.swapBeforeEmit,
                       counts.swapPendingSetter,
                       counts.swapTableChanged,
                       counts.noworkSwapChanged,
                       counts.errors,
                       counts.identityDiffEntries);
}
#endif

} // namespace rerevved::gpu::guest_d3d
