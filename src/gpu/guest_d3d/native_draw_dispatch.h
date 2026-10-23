#pragma once

#include "native_draw_hook.h"

#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

const char* NativeDrawHookName(NativeDrawHook hook) noexcept;

enum class NativeDrawMode : std::uint8_t
{
    Strict, // the first unsupported submission fails the session visibly
    Skip,   // bring-up only: unsupported submissions are counted and skipped
    // Bring-up only: like Skip, and completion logs each distinct unsupported
    // operation once, skips its guest effect and keeps releasing spans.
    Census,
};

enum class NativeDrawReason : std::uint8_t
{
    NoNativeRoute,   // no admitter accepts this submission
    NestingMismatch, // child calls differ from the static outer/child contract
    AutomaticIndex,  // 0x826A3608 has no live contract yet
    StateUnreadable, // the graphics state pointer did not name readable memory
    DispatchFailure, // the admitter or the dispatcher's own bookkeeping threw
    // A direct IndexedUpLowerer root that succeeded but whose commit
    // (0x826A2FF0) did not follow from the same frame and device.
    UncommittedSinkDraw,
};
inline constexpr std::size_t kNativeDrawReasonCount = 6;
static_assert(static_cast<std::size_t>(NativeDrawReason::UncommittedSinkDraw) + 1 == kNativeDrawReasonCount);

const char* NativeDrawReasonName(NativeDrawReason reason) noexcept;

// Commit 0x826A2FF0 publishes G+0x3484, G+0x3488..G+0x3494, caller r1+84/r10,
// and G+0x3198/G+0x3194/G+0x2E2C.
struct NativeDrawCommit
{
    bool          read       = false;
    std::uint32_t pendingEnd = 0, commitEnd = 0;
    std::uint32_t vertexDest = 0, indexDest = 0, vertexDwords = 0, indexDwords = 0;
    std::uint32_t vertexOut = 0, indexOut = 0;
    std::uint32_t vertexShader = 0, pixelShader = 0, declaration = 0;
};

// One logical draw. Guest inputs stay in guest memory and are named by the
// entry registers and the state pointers read before Original ran.
struct NativeDrawSubmission
{
    std::uint64_t                sequence = 0, frame = 0;
    std::uint64_t                operationMark = 0;
    NativeDrawHook               hook          = NativeDrawHook::IndexedBuffer;
    std::uint32_t                caller        = 0; // LR at root entry
    std::uint32_t                stack         = 0; // r1; indexed-UP passes its vertex stride at r1+84
    std::array<std::uint32_t, 8> arguments{};       // r3..r10 at entry; r3 is the graphics state pointer
    // G+0x3198, G+0x3194 and G+0x2E2C before Original, as read by the solid adapter.
    std::uint32_t vertexShader = 0, pixelShader = 0, declaration = 0;
    bool          stateRead = false;
    std::uint32_t result    = 0; // r3 after the root Original returned
    // Nested draw-hook calls folded into this submission.
    std::uint32_t                 nested = 0;
    std::optional<NativeDrawHook> child;
    std::uint32_t                 childCaller = 0, childResult = 0;
    bool                          nestingMismatch = false;
    // A direct IndexedUpLowerer root: the 0x3500-byte device shadow at entry
    // (empty when unreadable) and, once it is numbered, its commit.
    std::shared_ptr<const std::vector<std::uint8_t>> entryState;
    std::optional<NativeDrawCommit>                  commit;
    // What the dispatch config's `defer` returned for a root whose began,
    // admission, numbering and recorded run on the sequencer; null otherwise.
    std::shared_ptr<const void> deferral;
    // The guest thread that drew it; empty for a submission no hook made.
    std::thread::id thread;

    std::uint32_t Graphics() const noexcept
    {
        return arguments[0];
    }
};

// A direct IndexedUpLowerer call leaves its draw pending until the caller's
// commit, so its root is numbered there; the IndexedUp child commits inline.
constexpr bool NativeDrawDefersToCommit(NativeDrawHook hook, std::uint32_t caller) noexcept
{
    return hook == NativeDrawHook::IndexedUpLowerer && caller != kIndexedUpLowererReturn;
}

struct NativeDrawFrameCounters
{
    std::uint64_t                                     frame = 0;
    std::array<std::uint64_t, kNativeDrawHookCount>   invocations{}; // every hook entry, nested included
    std::uint64_t                                     nested = 0, abandoned = 0;
    std::uint64_t                                     attempted = 0, admitted = 0;
    std::array<std::uint64_t, kNativeDrawReasonCount> skipped{}, rejected{};
};

struct NativeDrawFirstSeen
{
    NativeDrawSubmission            submission;
    std::optional<NativeDrawReason> reason; // empty when admitted
    std::uint64_t                   occurrences = 0;
};

struct NativeDrawFailure
{
    NativeDrawSubmission submission;
    NativeDrawReason     reason = NativeDrawReason::NoNativeRoute;
};

// Reads one big-endian guest word as a host value; false when unreadable.
using NativeDrawWordReader = bool (*)(const std::uint8_t* base, std::uint32_t address, std::uint32_t& value);
// Copies guest bytes as stored (big-endian); false when any is unreadable.
using NativeDrawBlockReader = bool (*)(const std::uint8_t* base, std::uint32_t address, std::span<std::uint8_t> bytes);
// Stores one big-endian guest word given as a host value, as a guest `stw`
// does; false, with nothing stored, when the word is not writable guest memory.
using NativeDrawWordWriter = bool (*)(const std::uint8_t* base, std::uint32_t address, std::uint32_t value);
// True when all `bytes` bytes at `address` are writable guest memory.
using NativeDrawWritable = bool (*)(const std::uint8_t* base, std::uint32_t address, std::uint32_t bytes);
// Returns empty to admit, or the reason the submission is unsupported. It runs
// outside the dispatcher lock, before the submission is numbered.
using NativeDrawAdmitter = std::function<std::optional<NativeDrawReason>(const NativeDrawSubmission&)>;

struct NativeDrawDispatchConfig
{
    NativeDrawMode       mode              = NativeDrawMode::Strict;
    std::size_t          firstSeenCapacity = 256;
    NativeDrawWordReader read              = nullptr; // null reads validated kernel memory
    // Null with a null `read` copies validated kernel memory; null with a
    // `read` copies word by word through it.
    NativeDrawBlockReader readBlock = nullptr;
    // The stores of a consumed draw (lowerer_consume.h). Both null with a null
    // `read` store to validated kernel memory; both null with a `read` of the
    // owner's, a consumed draw is refused as unreadable.
    NativeDrawWordWriter                          write    = nullptr;
    NativeDrawWritable                            writable = nullptr;
    NativeDrawAdmitter                            admit;  // empty admits nothing
    std::function<void(const NativeDrawFailure&)> failed; // empty logs an error
    // Optional observers for a frame owner; empty ones change nothing. Both run
    // on the guest thread outside the lock, and exceptions are dropped. began
    // sees a new root before Original; recorded sees each numbered verdict.
    std::function<void(const NativeDrawSubmission&)>                                  began;
    std::function<void(const NativeDrawSubmission&, std::optional<NativeDrawReason>)> recorded;
    // Sees a held direct-lowerer root at its commit, before admission.
    std::function<void(const NativeDrawSubmission&)> committed;
    std::function<std::uint64_t()>                   numberOperation;
    // Guest-thread defer runs after state, before Original. Accepted roots
    // began/admit/number/record on sequencer in entry order; no-verdict roots
    // abandon there. Declined/held-lowerer roots drain, then run guest-side.
    bool                                                                    sequenced         = false;
    std::size_t                                                             sequencerCapacity = 4096;
    std::function<std::shared_ptr<const void>(const NativeDrawSubmission&)> defer;
    std::function<void(const NativeDrawSubmission&)>                        abandoned;
};

// The sequencer's work: roots deferred to it and run, drains that waited
// for it and their time, and time posts waited for queue room.
struct NativeDrawSequencerStats
{
    std::uint64_t posted = 0, ran = 0, drains = 0, drainNanoseconds = 0, fullNanoseconds = 0;
};

struct NativeDrawState;

// Admission happens after normalization and before any recording. Hooks may
// run on several guest threads; nesting is tracked per thread.
class NativeDrawDispatcher
{
public:
    explicit NativeDrawDispatcher(NativeDrawDispatchConfig config = {});
    ~NativeDrawDispatcher();

    NativeDrawDispatcher(const NativeDrawDispatcher&)            = delete;
    NativeDrawDispatcher& operator=(const NativeDrawDispatcher&) = delete;

    // Closes the current frame epoch and returns its counters.
    NativeDrawFrameCounters          CloseFrame();
    NativeDrawFrameCounters          CurrentFrame() const;
    NativeDrawFrameCounters          Totals() const;
    std::vector<NativeDrawFirstSeen> FirstSeen() const;
    // Submissions whose identity arrived after the first-seen limit; counted, not retained.
    std::uint64_t FirstSeenDropped() const;
    // Highest sequence numbered so far; a swap owner hands it to the frame assembler.
    std::uint64_t                    NumberedThrough() const;
    std::optional<NativeDrawFailure> Failure() const;
    // Returns once every deferred root posted before it has its verdict; at
    // once without a sequencer or on the sequencer thread. False when the
    // deadline passed first.
    void                     Drain();
    bool                     Drain(std::chrono::steady_clock::time_point deadline);
    NativeDrawSequencerStats Sequencer() const;

private:
    friend struct NativeDrawAccess;
    std::unique_ptr<NativeDrawState> state;
};

// Publishes the dispatcher to all draw hooks; false if one is already installed,
// a removal is running, or a withdrawn one still awaits a removal that returns
// true (or its destruction).
bool InstallNativeDrawDispatcher(NativeDrawDispatcher& dispatcher) noexcept;
// Withdraws it and waits until no hook still uses it and its sequencer has
// concluded every posted root. False on timeout or when called from inside a
// draw hook or a sequencer callback: the owner must then keep the dispatcher alive.
bool RemoveNativeDrawDispatcher(std::chrono::milliseconds timeout);
// Drains the installed dispatcher's sequencer. A guest thread calls it before
// an operation that must follow every draw before it: a clear, a resolve or
// a swap.
void DrainNativeDraws() noexcept;

// Root read exposes dispatcher words, guest base, and draw-effect sample; null
// means no open root on the thread.
struct NativeDrawRootView
{
    NativeDrawWordReader read         = nullptr;
    const std::uint8_t*  base         = nullptr;
    bool                 effectSample = false;
    // The dispatcher's stores; null when it has none (see NativeDrawDispatchConfig).
    NativeDrawWordWriter write    = nullptr;
    NativeDrawWritable   writable = nullptr;
};

NativeDrawRootView CurrentNativeDrawRoot() noexcept;

// An outer UP skip records its one lowerer/sink child as made and zero,
// preserving dispatcher nesting and draw counts.
void NoteNativeDrawConsumedChild(const NativeDrawScope& scope) noexcept;

// native_frame_timing counts route entries and G+0x2ABC tiling byte per root;
// disabled roots pay one relaxed load.
inline constexpr std::uint32_t kNativeDrawTilingFlagsOffset = 0x2ABC;

struct NativeDrawCensus
{
    std::array<std::uint64_t, kNativeDrawHookCount>                  roots{}, nested{}, unread{};
    std::array<std::array<std::uint64_t, 256>, kNativeDrawHookCount> tile{};
};

void EnableNativeDrawCensus(bool enabled) noexcept;
// Returns the counts since the previous take and resets them.
NativeDrawCensus TakeNativeDrawCensus() noexcept;
// " draw_root_<hook>=N" for the six hooks, then " draw_nested_<hook>=N" and
// " draw_tile_<hook>_<hex>=N" / " draw_tile_<hook>_unread=N" for nonzero counts.
std::string FormatNativeDrawCensus(const NativeDrawCensus& census);

namespace detail
{

// Test hook: runs inside a removal that holds its lock, before withdrawal.
extern void (*nativeDrawRemovalPause)();

} // namespace detail

} // namespace rerevved::gpu::guest_d3d
