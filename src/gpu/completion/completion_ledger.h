#pragma once

#include "gpu/guest_d3d/guest_resolve_encoder.h"
#include "gpu/guest_d3d/native_draw_dispatch.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rerevved::gpu::completion
{

// Progress block B = *(G+0x2A90) and scratch block S2 = *(G+0x2A94), as
// 0x8269C8E0 allocates them and 0x8269BCA8, 0x8269BF40 and 0x826A4150 encode them.
inline constexpr std::uint32_t kFenceOffset            = 0x00; // completed fence that 0x8269BE70 polls
inline constexpr std::uint32_t kCursorOffset           = 0x04; // command cursor at that fence
inline constexpr std::uint32_t kReadPointerOffset      = 0x3C; // RPTR writeback
inline constexpr std::uint32_t kScratchMaskOffset      = 0x00; // SCRATCH_REG0: CPUs still owing a callback
inline constexpr std::uint32_t kScratchFlipOffset      = 0x04; // SCRATCH_REG1: pending flip
inline constexpr std::uint32_t kScratchCallbackOffset  = 0x10; // SCRATCH_REG4
inline constexpr std::uint32_t kScratchParameterOffset = 0x14; // SCRATCH_REG5
// 0x8269B450 traps when it finds this callback, so it marks a retired slot.
inline constexpr std::uint32_t kRetiredCallback = 0x0BADF00D;
// The handler clears 1 << cpu in S2+0; mask 4 is the only modeled target.
inline constexpr std::uint32_t kCallbackMask = 4;
inline constexpr std::uint32_t kCallbackCpu  = 2;
// EndTiling predicates its worker-kick callback with this bin mask (G+0x31A4).
// Xenos sets bit 31 in BIN_SELECT only for an untiled pass or tile 0, so the
// callback fires once per tiled sequence.
inline constexpr std::uint32_t kFirstPassPredicate = 0x80000000;
// The title call site shared by off-screen and scanout resolves.
inline constexpr std::uint32_t kFrontbufferResolveCall = 0x8250AFEC;
// 0x826A9BA0 writes the GPU timestamp into one of eight words at B+0x40.
inline constexpr std::uint32_t kTimestampOffset = 0x40;
inline constexpr std::uint32_t kTimestampEnd    = 0x60;

enum class ProgressKind : std::uint8_t
{
    InvalidateState, // PM4_INVALIDATE_STATE 0x7FFF from 0x8269BCA8; no completion effect
    Fence,           // B+4 = cursor, then B+0 = fence
    Callback,        // source-1 interrupt through the scratch block
    ScratchWrite,    // scratch register writeback to guest memory
    GpuWait,         // later records wait until the guest word equals the reference
    Draw,
    Clear,
    Resolve,
    Unmodeled, // content with no model; its span never releases
    Swap,
    Barrier, // COHER_STATUS_HOST wait: earlier work completes first; no guest write
};

const char* ProgressKindName(ProgressKind kind) noexcept;

enum class InterruptResult : std::uint8_t
{
    Raised,    // the guest handler ran
    NoHandler, // no interrupt callback is registered yet; retried later
    Stopped,   // the interrupt thread is shutting down
};

// The only guest-visible effects the ledger produces. Addresses are guest
// physical; progress words are host order and stored big-endian. Images are
// already encoded guest bytes and are copied without another endian transform.
class IGuestProgressSink
{
public:
    virtual ~IGuestProgressSink()                                          = default;
    virtual bool WritePhysical(std::uint32_t address, std::uint32_t value) = 0;

    virtual bool WriteGuestImage(std::uint32_t, std::span<const std::uint8_t>)
    {
        return false;
    }

    virtual std::optional<std::uint32_t> ReadPhysical(std::uint32_t address) = 0;
    // Raises interrupt source 1 on `cpu` and returns after the guest handler ran.
    virtual InterruptResult RaiseCallbackInterrupt(std::uint32_t cpu) = 0;
};

// The host fence that completes a flush, or why there is none yet.
struct DrawFlush
{
    std::uint64_t fence = 0;     // nonzero on success
    bool          retry = false; // not yet possible; the ledger asks again on its next poll
    std::string   error;         // set when fence is zero
};

// Return the completed resolve image or why it is absent. Closing holds the
// span without failure because the source closes before the ledger.
struct GuestImageTake
{
    std::optional<guest_d3d::GuestResolveImage> image;
    std::string                                 error;
    bool                                        closing = false;
    // held keeps image unencoded for later WithImageWrites; its ledger record
    // applies without a write. Older before images write first by op mark, each
    // through GuestImageWritten.
    bool                                                                held = false;
    std::vector<std::pair<std::uint64_t, guest_d3d::GuestResolveImage>> before;
};

// Writes one encoded image to guest memory, false when it could not.
using GuestImageWriter = std::function<bool(const guest_d3d::GuestResolveImage&)>;
// CompletionLedger::WithImageWrites, as a holder of images reaches it.
using GuestImageWrites = std::function<bool(const std::function<void(const GuestImageWriter&)>&)>;

// Host completion of admitted GPU operations. The ledger cuts spans at WPTR kicks while
// frames close at VdSwap, so a fence emitted mid-epoch must not wait for the swap.
class IDrawCompletion
{
public:
    virtual ~IDrawCompletion() = default;
    // Submits every admitted operation through `sequence`, cutting the open
    // VdSwap epoch when its swap has not arrived, and returns the host fence
    // value that completes them. A zero fence without retry is a failure.
    virtual DrawFlush     FlushThrough(std::uint64_t sequence) = 0;
    virtual std::uint64_t Completed() const                    = 0;

    virtual GuestImageTake TakeGuestImage(std::uint64_t opMark)
    {
        static_cast<void>(opMark);
        return {};
    }

    // The ledger dropped every span holding a Resolve record through opMark, so
    // no image at or below it is taken; called outside the ledger's locks.
    virtual void DiscardGuestImagesThrough(std::uint64_t opMark)
    {
        static_cast<void>(opMark);
    }

    // The image taken at opMark is in guest memory: the ledger wrote all of
    // it. Called under the ledger's image-write gate, outside its lock.
    virtual void GuestImageWritten(std::uint64_t opMark)
    {
        static_cast<void>(opMark);
    }

    // Census mode: the ledger skipped the admitted resolve at opMark, so its
    // image is never written. Called outside the ledger's lock.
    virtual void GuestImageSkipped(std::uint64_t opMark)
    {
        static_cast<void>(opMark);
    }

    // HostWriting/HostWrote bracket unwatchable host-view physical writes, even
    // failed ones. Under ledger lock, callbacks may take only locks never held
    // across ledger calls.
    virtual void HostWriting(std::uint32_t physical, std::uint32_t length) noexcept
    {
        static_cast<void>(physical);
        static_cast<void>(length);
    }

    virtual void HostWrote(std::uint32_t physical, std::uint32_t length) noexcept
    {
        static_cast<void>(physical);
        static_cast<void>(length);
    }

    // Census mode: the newest host fence submitted so far, which covers every
    // admitted operation already submitted when a later flush fails.
    virtual std::uint64_t LastSubmitted() const
    {
        return 0;
    }
};

struct LedgerFailure
{
    std::string   detail;
    ProgressKind  kind    = ProgressKind::Unmodeled;
    std::uint32_t address = 0; // emitter, call site or draw caller
};

// One distinct census failure form and how often it occurred. A form is the
// kind, address and detail, with per-operation counters such as op marks
// collapsed; `failure` is its first occurrence.
struct CensusForm
{
    LedgerFailure failure;
    std::uint64_t count = 0;
};

// Distinct forms kept; later new forms count under one overflow form.
inline constexpr std::size_t kCensusFormLimit = 1024;

struct LedgerSnapshot
{
    std::uint32_t ringEpoch     = 0;
    std::uint64_t spansReleased = 0, partialSpans = 0, spansDropped = 0, callbacksReleased = 0;
    std::uint64_t swaps = 0, partialSwaps = 0;
    std::uint64_t deferredSpans = 0, replaySubmits = 0;
    std::uint64_t deviceSuspends = 0, deferredSwaps = 0;
    // Acquires that took a span with records from another thread.
    std::uint64_t deviceHandoffs = 0;
    // Tiled lists whose callback buffer was submitted, and the most lists
    // recorded and not yet retired at once.
    std::uint64_t tiledListsStarted = 0, maxTiledLists = 0;
    std::uint32_t lastCallback      = 0;
    std::uint32_t lastReleasedFence = 0;
    bool          fenceReleased     = false;
    std::uint32_t requestedFence    = 0; // last fence 0x8269BE70 was asked to wait for
    bool          fenceRequested    = false;
    std::uint32_t fenceBlock        = 0; // physical B from the newest fence record or fence wait; zero when unknown
    std::uint32_t writebackPointer  = 0;
    // The oldest unreleased span and what holds it.
    bool                                                         pending      = false;
    std::uint64_t                                                oldestSpan   = 0;
    bool                                                         oldestClosed = false;
    std::uint32_t                                                writePointer = 0;
    std::string                                                  oldestRecord;
    std::string                                                  unowned;
    std::uint64_t                                                hostTarget = 0, hostCompleted = 0;
    std::array<std::uint64_t, guest_d3d::kNativeDrawReasonCount> skipped{};
    std::uint32_t                                                unmodeledEmitter = 0;
    std::optional<std::string>                                   failure;
};

struct LedgerConfig
{
    guest_d3d::NativeDrawMode mode  = guest_d3d::NativeDrawMode::Strict;
    IGuestProgressSink*       sink  = nullptr; // not owned; must outlive Close
    IDrawCompletion*          draws = nullptr; // not owned; null leaves admitted draws unowned
    // Called once for the session failure, outside the ledger lock.
    std::function<void(const LedgerFailure&)> failed;
    // Census mode: called once per distinct form, outside the ledger lock.
    std::function<void(const LedgerFailure&)> census;
};

// Closed spans release in order after admitted draws host-complete (or with
//   none); effects apply in record order, RPTR last. Normally an unowned span
//   never releases. Failure stops records; fully owned earlier spans release,
//   the failing and later spans do not. Census counts unowned records/Fail by
//   form and skips their effects; model failures such as unattributed kicks
//   still end the session.
// Guest control             | Span/release dependency
// CP_RB_WPTR kick           | Closes the submitting thread's span; hooks record
//   in program order, completion thread releases.
// G+0x2B00 != 0             | 0x8269BD98 chains segments to the tile-replay
//   worker list rather than kicking.
// 0x8269B900 worker submit  | Releases deferred spans only after submission, in
//   deferred order.
// G+0x33B0 != 0             | Tiled segments release with the following main
//   kick; per-tile worker re-submissions are host-only.
// 0x826AC32C buffer submit  | Activates the newest list and retires all older
//   lists; the worker replays only that active list.
// Worker kicks             | Never close the main thread's open span.
// EndTiling side buffers   | Worker-kick callbacks/fence join the span
//   submitting their buffer, directly or deferred.
class CompletionLedger
{
public:
    explicit CompletionLedger(LedgerConfig config);
    ~CompletionLedger();

    CompletionLedger(const CompletionLedger&)            = delete;
    CompletionLedger& operator=(const CompletionLedger&) = delete;

    // Guest-thread records. None blocks; physical addresses are resolved by the caller.
    void InvalidateState(std::uint32_t emitter);
    // deferredBuffer names the side buffer EndTiling's fence is emitted into at
    // 0x826AC33C; the record joins the span that submits that buffer.
    void Fence(std::uint32_t block, std::uint32_t fence, std::uint32_t cursor, std::optional<std::uint32_t> deferredBuffer = std::nullopt);
    // predicateMask=G+0x31A4 when emitter ORs predicates(G+0x31AC); only
    // kFirstPassPredicate is modeled. sideBuffer names 0x826AC32C worker-kick
    // buffer; it and earlier first-pass callbacks join the submitting span.
    void Callback(std::uint32_t                scratch,
                  std::uint32_t                function,
                  std::uint32_t                parameter,
                  std::uint32_t                flags,
                  std::optional<std::uint32_t> predicateMask,
                  std::optional<std::uint32_t> sideBuffer = std::nullopt);
    void ScratchWrite(std::uint32_t address, std::uint32_t value, std::uint32_t emitter);
    // 0x826A9BA0's EVENT_WRITE_SHD with initiator bit 31 stores the GPU counter,
    // modeled as a nonzero, increasing ledger counter; `block` is phys(B).
    void GpuTimestamp(std::uint32_t address, std::uint32_t block);
    void GpuWait(std::uint32_t address, std::uint32_t reference, std::uint32_t emitter);
    void BeginDraw(guest_d3d::NativeDrawHook hook, std::uint32_t caller);
    // A rejected draw's failure names `refusal`, the family detail, when one is given.
    void FinishDraw(std::uint64_t sequence, guest_d3d::NativeDrawHook hook, std::uint32_t caller, std::optional<guest_d3d::NativeDrawReason> verdict, std::string_view refusal = {});

    // This thread's open draw record, handed off so another thread reports
    // its verdict: its span holds until then. Null without an open draw.
    struct DetachedDraw
    {
        std::uint64_t span  = 0;
        std::size_t   index = 0;
    };

    std::optional<DetachedDraw> DetachDraw();
    // FinishDraw for a detached record, from any thread. A record whose span
    // an epoch reset dropped is not recorded again.
    void FinishDraw(const DetachedDraw& draw, std::uint64_t sequence, guest_d3d::NativeDrawHook hook, std::uint32_t caller, std::optional<guest_d3d::NativeDrawReason> verdict, std::string_view refusal = {});
    // A detached record whose draw reports no verdict, as an unwound root.
    void AbandonDraw(const DetachedDraw& draw);
    // Resolve opens a record that FinishResolve settles on the same thread. Any
    // other record from that thread first unowns a still-open resolve as
    // resolve_copy_not_captured, so a missed copy hook never holds silently.
    void Resolve(std::uint32_t call, std::optional<std::uint32_t> destination);
    // True while this thread's resolve still awaits FinishResolve.
    bool AwaitingResolveCopy() const;
    void FinishResolve(std::uint64_t opMark, bool frontbuffer, bool partial, std::string error = {});
    // Skip mode treats an unsupported clear as a skipped raster operation.
    // Census mode counts it and skips it.
    void Clear(std::uint64_t opMark, std::string error = {});
    void Unmodeled(std::uint32_t emitter);
    void Barrier(std::uint32_t emitter);
    // Returns true when the VdSwap epoch it closes released anything partially.
    bool Swap(std::uint32_t frontbufferPhysical);
    // On a thread with a pending ReplaySubmit, submits that segment; otherwise
    // closes the open span with the records held for a DirectSubmit buffer.
    void Kick(std::uint32_t writePointer);
    // Progress that would run once per tile, after the worker starts.
    void TiledProgress(ProgressKind kind, std::uint32_t site);

    // Tile-replay worker. TiledSegment adds a segment to the newest tiled list,
    // which BeginTiledList (0x826AC0A8) starts; Defer closes the open span as
    // deferred until the worker submits `physical`.
    void TiledSegment(std::uint32_t physical, std::uint32_t dwords);
    void BeginTiledList();
    void Defer(std::uint32_t physical, std::uint32_t dwords);
    // Worker-thread records: no progress record is accepted from a marked
    // thread outside a device-owner swap.
    void MarkReplayThread();
    // Suspend 0x8268CF60 drains device to worker; resume 0x8268CFF8 restores
    // it. While suspended only device-owner-swap thread records. Fence waits
    // remain allowed.
    void Suspend();
    void Resume();
    // Acquire 0x8268CF10 stores owner in G+0x2A88; release 0x8268CF50 clears
    // it. Threads may share a span only by serial handoff; overlapping
    // acquire/unowned recording fails session. Only owner release frees device.
    void AcquireDevice();
    void ReleaseDevice();
    // Worker deferred swap starts on empty, kicked span with no frame work
    // since last swap; it records like main thread and must swap/kick before
    // EndDeviceOwnerSwap.
    void BeginDeviceOwnerSwap();
    void EndDeviceOwnerSwap();
    void ReplaySubmit(std::uint32_t physical, std::uint32_t dwords, std::uint32_t site);
    // 0x8269BD98 submits `physical` directly at 0x8269BE5C, before its kick.
    void DirectSubmit(std::uint32_t physical, std::uint32_t dwords);
    // Silent direct submit seals the open span without kick or guest RPTR
    // publication.
    void SilentSubmit();
    // `block` is phys(*(G+0x2A90)) as the waiter reads it, or zero when unknown.
    void RequestFence(std::uint32_t fence, std::uint32_t block);
    // A session failure that is not tied to one record; census mode counts it
    // unless it is `fatal`, as a stopped native draw stream or device loss is:
    // no later span could complete, so it fails the session in every mode.
    void Fail(std::string detail, ProgressKind kind, std::uint32_t address, bool fatal = false);

    // Ring lifecycle. CloseEpochWrites runs after the guest drained and before
    // it frees B and S2; no write for an earlier span starts after it returns.
    // Both drop every span and discard the dropped resolves' images.
    void CloseEpochWrites();
    void BeginRingEpoch();
    void RegisterWriteback(std::uint32_t pointer);

    // Completion thread: releases what it can without blocking on the guest.
    // Returns true when a span was released.
    bool Pump();
    // No guest write starts after this returns.
    void Close();
    // Run body under image-write gate with ledger writer; false after close
    // without running. Held-image records already applied, so writes belong to
    // no span. Never call from completion callbacks already holding gate.
    bool WithImageWrites(const std::function<void(const GuestImageWriter&)>& body);

    LedgerSnapshot Snapshot() const;
    // True while the oldest span is closed and owned but its epoch has no
    // registered writeback yet. Cheap enough for the vblank thread's debt check.
    bool                       AwaitingWritebackRegistration() const;
    std::optional<std::string> Failure() const;
    // Census forms in first-seen order; empty outside census mode.
    std::vector<CensusForm> Census() const;

private:
    struct State;
    std::unique_ptr<State> state;
};

} // namespace rerevved::gpu::completion
