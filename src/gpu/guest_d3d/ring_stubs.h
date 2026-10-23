#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct PPCContext;

namespace rerevved::gpu::guest_d3d
{

// Ring stubs preserve stores; G+0x2A90 is progress block and G+0x2A94 scratch
// block.
namespace ring_layout
{

inline constexpr std::uint32_t kCursor          = 0x30;   // last written ring dword
inline constexpr std::uint32_t kHardLimit       = 0x34;   // the cursor stays below it between ring checks
inline constexpr std::uint32_t kCursorLimit     = 0x38;   // flush once the cursor passes it
inline constexpr std::uint32_t kProgressBlock   = 0x2A90; // B
inline constexpr std::uint32_t kScratchBlock    = 0x2A94; // S2
inline constexpr std::uint32_t kNextFence       = 0x2A9C; // F, the fence the next emitter writes
inline constexpr std::uint32_t kModeLow         = 0x2ABC; // byte; 0x80 while a command buffer records
inline constexpr std::uint32_t kModeHigh        = 0x2ABD; // byte; see kMode* below
inline constexpr std::uint32_t kModeExtra       = 0x2ABE; // byte; 0x04 asks the flush for a second coherency range
inline constexpr std::uint32_t kRingWriteIndex  = 0x2AC8; // dwords; 0x8269B900 stores it to CP_RB_WPTR
inline constexpr std::uint32_t kWorkerPending   = 0x2B00; // tile-replay worker owns the ring while nonzero
inline constexpr std::uint32_t kCoherencyRange  = 0x2E30; // doubleword; 0x00000000FFFFFFFF when empty
inline constexpr std::uint32_t kSwapFlags       = 0x2E4C; // bits 23..29 join the flip parameter
inline constexpr std::uint32_t kPredicateMask   = 0x31A4; // bin mask the predicate tests
inline constexpr std::uint32_t kPredicate       = 0x31AC; // nonzero predicates emitted packets
inline constexpr std::uint32_t kTiling          = 0x33B0; // set by BeginTiling 0x826AC0A8
inline constexpr std::uint32_t kTilingNext      = 0x33B4; // set with G+0x33B0, cleared with it by 0x826AC218
inline constexpr std::uint32_t kFenceCursorCopy = 0x33B8; // 0x8269BCA8 keeps the cursor here
inline constexpr std::uint32_t kFenceLowCopy    = 0x33BC; // and the low word here
inline constexpr std::uint32_t kTileList        = 0x3438; // the list BeginTiling started; the worker walks it
inline constexpr std::uint32_t kPreviousList    = 0x344C; // where BeginTiling moved the previous list
inline constexpr std::uint32_t kSwapInterval    = 0x351C;
inline constexpr std::uint32_t kRingMask        = 0x3A34; // ring size in dwords minus one
inline constexpr std::uint32_t kFenceLow        = 0x3A48; // its low two bits join the fence cursor
inline constexpr std::uint32_t kCpuFenceSink    = 0x541C; // optional interface the ring writers call
inline constexpr std::uint32_t kTimingMode      = 0x5490; // 2 makes 0x826A9C30 emit timestamps
inline constexpr std::uint32_t kRingCallback    = 0x5B0C; // optional function the ring writers call
inline constexpr std::uint32_t kDeviceEnd       = 0x5B10;

inline constexpr std::uint8_t kModeCpuFence      = 0x02; // set by 0x8269C588 when 0x82F9B0A4 is nonzero
inline constexpr std::uint8_t kModeAllocFailed   = 0x20; // set by 0x8269B818 when a side buffer allocation fails
inline constexpr std::uint8_t kModePreSwapDone   = 0x10; // set by 0x826A4150 on return
inline constexpr std::uint8_t kModeRingStarved   = 0x20; // set by 0x8269C270 when no segment was free
inline constexpr std::uint8_t kModeLowRecording  = 0x80;
inline constexpr std::uint8_t kModeExtraCoherent = 0x04;

inline constexpr std::uint64_t kEmptyCoherencyRange = 0x00000000FFFFFFFFULL;

inline constexpr std::uint32_t kFenceOffset       = 0x00;
inline constexpr std::uint32_t kFenceCursorOffset = 0x04;
inline constexpr std::uint32_t kTimestampEnd      = 0x60;

// The device the callback emitter reads S2 through: *(*0x820009F0).
inline constexpr std::uint32_t kDeviceGlobal = 0x820009F0;
// Nonzero makes 0x8269C588 switch the device to CPU fences.
inline constexpr std::uint32_t kCpuFenceGlobal = 0x82F9B0A4;
// The flip callback 0x826A4150 emits.
inline constexpr std::uint32_t kFlipCallback = 0x826A3FB8;
// 0x8269C8E0 initializes the ring through 0x8269B2A0; that call returns here.
inline constexpr std::uint32_t kRingInitWriterReturn = 0x8269CC68;
// The worker-kick callback 0x826AC218 emits: it queues the list for the
// replay worker and sets its event.
inline constexpr std::uint32_t kWorkerKick = 0x826AC020;
// The side buffers 0x826AC218 allocates through 0x8269B818, in dwords (the
// allocator reserves four bytes each): the callback, the fence and the
// 16-byte deferred-kick tokens.
inline constexpr std::uint32_t kEndTilingCallbackDwords = 148;
inline constexpr std::uint32_t kEndTilingFenceDwords    = 40;
inline constexpr std::uint32_t kEndTilingTokenDwords    = 48;
// The lists 0x826AC218 terminates through 0x8269B508, in its order.
inline constexpr std::array<std::uint32_t, 7> kEndTilingLists{ 0x3438, 0x33C0, 0x3424, 0x33D4, 0x33E8, 0x33FC, 0x3410 };

// native_ring_consume_publish
inline constexpr std::uint32_t kSubmitLock = 0x2B04; // spin lock 0x8269BD98 takes while G+0x2B00 is nonzero
inline constexpr std::uint32_t kTailStart  = 0x3A50; // S, where the unpublished dwords begin
inline constexpr std::uint32_t kListEnd    = 0x3440; // tiled segment list cursor
inline constexpr std::uint32_t kListLimit  = 0x3444; // and its capacity

} // namespace ring_layout

enum class RingStub : std::uint8_t
{
    Flush,        // 0x8269C588
    Publish,      // 0x8269C400
    Submit,       // 0x8269BD98
    Publisher,    // 0x8269B900
    DirectWriter, // 0x8269B2A0 outside ring initialization
    RingWait,     // 0x8269B1F0
    Fence,        // 0x8269BCA8
    Callback,     // 0x8269BF40
    Timestamp,    // 0x826A9BA0
    Drain,        // 0x8269C810
    PreSwap,      // 0x826A4150
    Present,      // the VdSwap call at 0x826A4884
    // The tiling bracket. EndTiling is stubbed; BeginTiling, the worker-kick
    // callback deliveries and the replay worker's token handler always run as
    // guest code and count as served while the stubs serve a checked device.
    TilingBegin,     // BeginTiling 0x826AC0A8
    TilingEnd,       // EndTiling 0x826AC218
    TilingCallbacks, // 0x826AC020 deliveries
    TilingTokens,    // the token handler 0x826AB740
};
inline constexpr std::size_t kRingStubCount      = static_cast<std::size_t>(RingStub::TilingTokens) + 1;
inline constexpr std::size_t kFirstTilingCounter = static_cast<std::size_t>(RingStub::TilingBegin);

inline constexpr std::array<std::string_view, kRingStubCount> kRingStubNames{
    "flush",
    "publish",
    "submit",
    "publisher",
    "direct_writer",
    "ring_wait",
    "fence",
    "callback",
    "timestamp",
    "drain",
    "pre_swap",
    "present",
    "tiling_begin",
    "tiling_end",
    "tiling_callbacks",
    "tiling_tokens",
};

// VdSwap outcomes partition enter into consumed, commit failure, and refusals;
// refused calls run Original once.
enum class PresentOutcome : std::uint8_t
{
    Enter,
    Consumed,         // the commit completed
    CommitFailure,    // the commit began and threw; the call is still consumed
    RefusedRuntime,   // no guest memory, kernel or owner
    RefusedDelegate,  // no detached video delegate
    RefusedArgs,      // the swap arguments are unreadable
    RefusedException, // the preflight threw
};
inline constexpr std::size_t kPresentOutcomeCount = static_cast<std::size_t>(PresentOutcome::RefusedException) + 1;

inline constexpr std::array<std::string_view, kPresentOutcomeCount> kPresentOutcomeNames{
    "present_enter",
    "present_consumed",
    "present_commit_failure",
    "present_refused_runtime",
    "present_refused_delegate",
    "present_refused_args",
    "present_refused_exception",
};

struct RingStubCounts
{
    std::array<std::uint64_t, kRingStubCount> stubbed{};
    std::array<std::uint64_t, kRingStubCount> original{};
    // Indexed by PresentOutcome.
    std::array<std::uint64_t, kPresentOutcomeCount> present{};
};

void CountRingStub(RingStub stub, bool stubbed) noexcept;
void CountPresentOutcome(PresentOutcome outcome) noexcept;
// Returns the calls counted since the previous take and resets them.
RingStubCounts TakeRingStubCounts() noexcept;
// " stub_<name>=N" and " stub_<name>_original=N" for each nonzero count,
// then " tiling_<name>=N" and " tiling_<name>_original=N", then
// " present_<outcome>=N" for each nonzero outcome.
std::string FormatRingStubCounts(const RingStubCounts& counts);

// Guest memory as the stubs see it: big-endian words and bytes at guest
// virtual addresses. A false or empty result means unmapped or not writable.
// The heap allocation that holds an address.
struct RingAllocation
{
    std::uint32_t base = 0;
    std::uint32_t size = 0;
};

class IRingStubMemory
{
public:
    virtual ~IRingStubMemory()                                                               = default;
    virtual std::optional<std::uint32_t> Read32(std::uint32_t address)                       = 0;
    virtual std::optional<std::uint8_t>  Read8(std::uint32_t address)                        = 0;
    virtual bool                         Write32(std::uint32_t address, std::uint32_t value) = 0;
    virtual bool                         Write8(std::uint32_t address, std::uint8_t value)   = 0;
    // Atomically swaps an aligned big-endian doubleword and returns the old value.
    virtual std::optional<std::uint64_t> Exchange64(std::uint32_t address, std::uint64_t value) = 0;
    virtual std::optional<std::uint32_t> Physical(std::uint32_t address)                        = 0;

    // True when a word at `address` is mapped writable. The default is readable.
    virtual bool Writable32(std::uint32_t address)
    {
        return Read32(address).has_value();
    }

    // The allocation holding `address`; empty when unmapped or unknown, in which case
    // the allocation checks of the entry invariants are skipped.
    virtual std::optional<RingAllocation> Allocation(std::uint32_t address)
    {
        (void)address;
        return std::nullopt;
    }
};

// The guest video services behind the stubs, published while native_ring_stubs
// is on and the producers run.
class IRingStubServices
{
public:
    virtual ~IRingStubServices() = default;
    // The CP_RB_WPTR store 0x8269B900 would make: it closes the ledger's open
    // span exactly as the MMIO write does, without the store.
    virtual void Kick(std::uint32_t writePointer) = 0;
};

// Route trace entries whose raw wrapper calls RunRingStub after the progress
// observers. The worker kick 0x826AC020 is only counted and logged.
constexpr bool RingStubEntry(std::uint32_t address) noexcept
{
    return address == 0x8269B1F0 || address == 0x8269B2A0 || address == 0x8269B900 || address == 0x8269BCA8 || address == 0x8269BF40 ||
           address == 0x8269C588 || address == 0x8269C810 || address == 0x826A4150 || address == 0x826AC020;
}

// The hooks' view of guest memory; null before the kernel exists.
IRingStubMemory* GuestRingMemory() noexcept;
// Counts a call and logs the first stubbed and the first Original call of each stub.
void NoteRingStub(RingStub stub, bool stubbed, std::uint32_t site, std::string_view detail = {}) noexcept;

// True when the stub replaced Original. It forwards to the runner that
// ring_stub_hooks.cpp registers, so test targets that link the route
// wrappers without the guest hooks see no stubs.
bool RunRingStub(std::uint32_t address, PPCContext& ctx, std::uint8_t* base) noexcept;
using RingStubRunner = bool (*)(std::uint32_t address, PPCContext& ctx, std::uint8_t* base) noexcept;
void RegisterRingStubRunner(RingStubRunner runner) noexcept;

// The cvar native_ring_stubs; read once when the guest video services start.
bool RingStubsRequested() noexcept;

// True while services are published and no layout mismatch disabled the
// stubs; every hook runs Original otherwise.
extern std::atomic<bool> ringStubsActive;
// The device the stubs last checked; zero before the first check.
extern std::atomic<std::uint32_t> ringStubsValidatedDevice;

// Requested helpers test ringStubsActive before their cvar; a proceeding
// consume takes RingStubLease.
inline bool RingStubsPublished() noexcept
{
    return ringStubsActive.load(std::memory_order_relaxed);
}

// Acquire counts lease then reads ringStubsActive, both seq-cst: it fails or
// drain sees it. Withdraw/Disable clear flag then boundedly wait other-thread
// leases; timeout logs error. Own leases never block own drain.
bool AcquireRingStubLease() noexcept;
void ReleaseRingStubLease() noexcept;
// Test seam: a function that runs inside Acquire between the lease count and
// the flag test, which is where a withdrawal that races a consume lands. Null
// in production.
using RingStubLeaseProbe = void (*)() noexcept;
void SetRingStubLeaseProbe(RingStubLeaseProbe probe) noexcept;

class RingStubLease
{
public:
    RingStubLease() noexcept
    : held(AcquireRingStubLease())
    {
    }

    ~RingStubLease()
    {
        if (held)
            ReleaseRingStubLease();
    }

    RingStubLease(const RingStubLease&)            = delete;
    RingStubLease& operator=(const RingStubLease&) = delete;

    explicit operator bool() const noexcept
    {
        return held;
    }

private:
    bool held;
};

// True while the stubs are active and have checked device `g`, so the stub
// flush and publish paths serve a draw that leaves the ring untouched.
inline bool RingStubsReady(std::uint32_t g) noexcept
{
    return g != 0 && ringStubsActive.load(std::memory_order_acquire) &&
           ringStubsValidatedDevice.load(std::memory_order_acquire) == g;
}

void                               PublishRingStubServices(std::shared_ptr<IRingStubServices> services);
void                               WithdrawRingStubServices();
std::shared_ptr<IRingStubServices> CurrentRingStubServices();
// Disables the stubs for the session. The ledger stays published and keeps
// every record already made; nothing else writes guest progress.
void DisableRingStubs(std::string_view reason);

enum class RingLayout : std::uint8_t
{
    Ok,
    NotReady, // the device has no blocks yet; this call runs Original
    Mismatch, // the fields disagree with the design; the session runs Original
};

struct RingLayoutCheck
{
    RingLayout  verdict = RingLayout::Ok;
    std::string detail;
};

enum class OriginalResidueReadiness : std::uint8_t;
// Test seam for the hooks' existing readiness decision over scripted memory.
bool CheckRingStubReadinessForTesting(IRingStubMemory& memory, std::uint32_t g, RingStub stub, std::uint32_t site, OriginalResidueReadiness* diagnostic = nullptr);

// Checks the fields the stubs rely on, as 0x8269C8E0 seeds them: the device
// global names G, B and S2 are mapped, the ring mask is 2^n - 1 and F is odd.
RingLayoutCheck CheckRingLayout(IRingStubMemory& memory, std::uint32_t g);

// The CPU-side effects of each helper. Every function writes only the listed
// guest fields, in the original's order; ring words are never written.

// 0x8269BCA8 without its 10 PM4 dwords: G+0x33B8 = cursor, G+0x33BC = low,
// in CPU fence mode (G+0x2ABD bit 0x2 without a sink) B+0 = F and
// B+4 = cursor | (low & 3), then F += 2. B is otherwise the ledger's to write.
bool ApplyFenceFields(IRingStubMemory& memory, std::uint32_t g);

enum class PublishBranch : std::uint8_t
{
    Submit,    // fence and kick, through the stubbed emitter and submit
    Starved,   // the tail alone
    Recording, // the command buffer list
    Tiling,    // the tiled segment list
    Unreadable,
};

// The branch 0x8269C400 takes. Its own body writes no ring word, so it runs
// as Original with its callees stubbed.
struct PublishDiagnostic
{
    std::uint8_t  valid = 0; // high=1, low=2, tiling=4; readable zero is valid
    std::uint8_t  high = 0, low = 0;
    std::uint32_t tiling = 0;
    PublishBranch branch = PublishBranch::Unreadable;
};

PublishBranch ClassifyPublish(IRingStubMemory& memory, std::uint32_t g, PublishDiagnostic* diagnostic = nullptr);

struct Coherency
{
    bool range = false; // G+0x2E30 held a range, now swapped empty; barrier 0x8269BB94
    bool extra = false; // G+0x2ABE bit 0x4; barrier 0x8269BC54
};

// The coherency packets 0x8269BAE8 writes into a side buffer: the range swap
// of 0x8269B3F8 and the two barriers they carry. Returns no value when a
// field is unreadable.
std::optional<Coherency> SwapCoherency(IRingStubMemory& memory, std::uint32_t g);

// 0x8269C588 runs 0x8269BAE8 only when it neither records nor tiles; no value
// when it skips it.
std::optional<Coherency> TakeCoherency(IRingStubMemory& memory, std::uint32_t g);

// The fence the tail of 0x8269C588 waits for (F - 2) before it sets the CPU
// fence mode bit: only with neither recording nor CPU fences and a nonzero
// 0x82F9B0A4.
std::optional<std::uint32_t> CpuFenceSwitch(IRingStubMemory& memory, std::uint32_t g);
bool                         SetCpuFenceMode(IRingStubMemory& memory, std::uint32_t g);

// True when G+0x30 is past G+0x38, where every emitter flushes first.
std::optional<bool> CursorPastLimit(IRingStubMemory& memory, std::uint32_t g);

// 0x8269B1F0: (r4 + r5) & *(G+0x3A34), returned at once.
std::optional<std::uint32_t> RingSpaceWait(IRingStubMemory& memory, std::uint32_t g, std::uint32_t start, std::uint32_t count);

enum class PublisherPath : std::uint8_t
{
    Kick,     // the CP_RB_WPTR store, as a ledger kick
    Silent,   // CPU fence mode without a sink: the original stores nothing
    Original, // a sink or ring callback is set
};

// 0x8269B900 and the publisher half of 0x8269BD98.
struct PublisherDiagnostic
{
    std::uint8_t  valid = 0; // sink=1, callback=2, high=4
    std::uint32_t sink = 0, callback = 0;
    std::uint8_t  high = 0;
    PublisherPath path = PublisherPath::Original;
};

PublisherPath ClassifyPublisher(IRingStubMemory& memory, std::uint32_t g, PublisherDiagnostic* diagnostic = nullptr);

// 0x8269BD98 kicks directly while G+0x2B00 is zero: G+0x2B00 += raise, then
// the ledger's direct submit and the publisher. Returns false when the worker
// owns the ring (the deferred chain runs as Original) or a field is unreadable.
struct RaiseDiagnostic
{
    bool          pendingValid   = false;
    std::uint32_t pending        = 0;
    bool          attemptedWrite = false, writeSucceeded = false;
};

bool RaiseForDirectSubmit(IRingStubMemory& memory, std::uint32_t g, std::uint32_t raise, RaiseDiagnostic* diagnostic = nullptr);

// 0x8269B2A0 publishes nothing outside ring initialization, without sinks.
bool DirectWriterIsStubbed(IRingStubMemory& memory, std::uint32_t g, std::uint32_t returnAddress);

// Ring dwords 0x8269BF40 writes for normalized flags: 27 for the interrupt
// (bit 0x2), 2 more for the idle wait when bit 0x1 is clear, and 8 for the
// retire wait (bit 0x4). The default flags 6 write 37, 148 bytes.
constexpr std::uint32_t CallbackEmitterDwords(std::uint32_t flags) noexcept
{
    std::uint32_t dwords = 0;
    if (flags & 2)
        dwords += (flags & 1) ? 27 : 29;
    if (flags & 4)
        dwords += 8;
    return dwords;
}

constexpr std::uint32_t NormalizeCallbackFlags(std::uint32_t flags) noexcept
{
    return (flags & 6) == 0 ? flags | 6 : flags;
}

// The cursor 0x8269BF40(G, cursor, flags, function, parameter) returns. The
// ledger already holds its callback: the observer recorded it at entry, and
// models or fails it whichever path runs.
std::optional<std::uint32_t> CallbackCursor(IRingStubMemory& memory, std::uint32_t g, std::uint32_t cursor, std::uint32_t flags);

struct PreSwapPlan
{
    std::uint32_t parameter = 0; // the flip callback's argument
    std::uint32_t interval  = 0; // the decoded interval code r11 holds at 0x826A4258
};

// The parts of 0x826A4150 decided before it runs: refuses the timestamp mode
// G+0x5490 == 2 and swap interval words outside {0, 1, 2, 4, 0x80000000},
// for which the original reads an uninitialized stack word.
std::optional<PreSwapPlan> PlanPreSwap(IRingStubMemory& memory, std::uint32_t g, std::uint32_t flipAddress, std::string* refusal = nullptr);

// The last store of 0x826A4150: the kModePreSwapDone bit.
bool FinishPreSwap(IRingStubMemory& memory, std::uint32_t g);

// The guest's physical form of a command buffer address, as 0x826AC218 and
// 0x8269BD98 compute it: the 0xE0000000 view is offset by 0x1000.
constexpr std::uint32_t CommandBufferPhysical(std::uint32_t address) noexcept
{
    return (address & 0x1FFFFFFFU) + ((((address >> 20) & 0xFFF) + 0x200) & 0x1000);
}

// The worker-kick callback's parameter, read after the lists are terminated:
// the list head *(G+0x3438) in the 0xC0000000 view, or zero for no list.
std::optional<std::uint32_t> EndTilingCallbackParameter(IRingStubMemory& memory, std::uint32_t g);

// 0x826AC218 submits the callback buffer unless the device is in CPU fence
// mode without a sink, the case the ledger fails.
std::optional<bool> EndTilingSubmitsCallback(IRingStubMemory& memory, std::uint32_t g);

// G+0x33B0 = 0 and G+0x33B4 = 0, as 0x826AC218 stores them after its side
// buffer allocations.
bool EndTilingState(IRingStubMemory& memory, std::uint32_t g);

#if REREVVED_DEVELOPER

// Host path handles Publish 0x8269C400 and Submit 0x8269BD98, preserves CPU
// store order, and drops PM4 bytes.
bool RingConsumeCvar() noexcept;

// The cvar with published ring stub services; every consume hook tests this first.
inline bool RingConsumePublishRequested() noexcept
{
    return RingStubsPublished() && RingConsumeCvar();
}

// Why an enabled call ran Original. Decided before the call's first guest mutation.
enum class RingConsumeRefusal : std::uint8_t
{
    None,
    Lease,             // the services were withdrawn
    Recording,         // the recording variant, whose decoder is unknown
    Unclassified,      // the device fields cannot tell the branch, so recording is not excluded
    Cursor,            // G+0x30 unreadable
    Start,             // G+0x3A50 unreadable
    Limit,             // G+0x38 unreadable
    TailStorage,       // G+0x30 or G+0x3A50 not writable
    ListHead,          // G+0x3440 unreadable
    ListLimit,         // G+0x3444 unreadable
    ListStorage,       // the tiled segment words are not writable
    PendingUnreadable, // G+0x2B00 unreadable
    PendingUnwritable, // G+0x2B00 not writable
    LockStorage,       // G+0x2B04 not writable
    TokenStorage,      // the three token words are not writable
    TokenWrap,         // the token buffer wraps the address space
    ListLink,          // the worker list words B550 links are not writable
    CpuFence,          // the Publish fence would complete on the CPU before the host draws retire
    Count,
};

inline constexpr std::array<std::string_view, static_cast<std::size_t>(RingConsumeRefusal::Count)> kRingConsumeRefusalNames{
    "none",
    "lease",
    "recording",
    "unclassified",
    "cursor_unreadable",
    "start_unreadable",
    "limit_unreadable",
    "tail_unwritable",
    "list_head_unreadable",
    "list_limit_unreadable",
    "list_unwritable",
    "pending_unreadable",
    "pending_unwritable",
    "lock_unwritable",
    "token_unwritable",
    "token_wrap",
    "worker_list_unwritable",
    "cpu_fence",
};

#if REREVVED_DEVELOPER
enum class OriginalResidueReason : std::uint8_t;
// The R1 reason a Submit refusal reports; each unsupported population keeps its own.
OriginalResidueReason SubmitRefusalResidueReason(RingConsumeRefusal refusal) noexcept;
#endif

// What Publish reads before it mutates anything. C = *(G+0x30), S = *(G+0x3A50),
// R = C + 4 (full width), A(x) = CommandBufferPhysical(x), q = S32(U32(R - S)) >> 2.
struct PublishConsumePlan
{
    PublishBranch branch = PublishBranch::Unreadable;
    std::uint32_t cursor = 0, start = 0, physical = 0;
    std::uint64_t next  = 0;
    std::int32_t  count = 0;
    // Tiling with q != 0: the list cursor E and its capacity.
    std::uint32_t listEnd = 0, listLimit = 0;
    bool          growth = false;
};

// Submit, Starved and Tiling only. Reads, never writes.
RingConsumeRefusal PlanPublishConsume(IRingStubMemory& memory, std::uint32_t g, PublishBranch branch, PublishConsumePlan& plan);

// A tiled segment: A(S) at E+4, U32(q) | 0x81000000 at E, then E+8 at G+0x3440.
bool StoreTilingSegment(IRingStubMemory& memory, std::uint32_t g, std::uint32_t listEnd, std::uint32_t physical, std::int32_t count);

enum class PublishTail : std::uint8_t
{
    Stored,   // G+0x3A50 and G+0x30 stored for the aligned return
    Rollover, // G+0x30 stored; the caller releases its lease and runs 0x8269C270
    Failed,   // a store or the limit read failed
};

// The common tail of 0x8269C400 for `branchReturn`, the full-width r28: a =
// align32(branchReturn); a <= *(G+0x38) stores G+0x3A50 = a, then G+0x30 = U32(a - 4);
// otherwise only G+0x30 = U32(branchReturn - 4).
PublishTail StorePublishTail(IRingStubMemory& memory, std::uint32_t g, std::uint64_t branchReturn);

// Submit's refusals before it takes G+0x2B04. `p` is the token buffer r4 and `list`
// the worker list r8. A nonzero entry N may end in either arm, so both are checked.
RingConsumeRefusal PlanSubmitConsume(IRingStubMemory& memory, std::uint32_t g, std::uint32_t p, std::uint32_t list, std::uint32_t entryPending);

// G+0x2B00 = U32(reread + add), stored even for an add of zero. Returns the stored
// value; nothing when the read or the store failed.
std::optional<std::uint32_t> StorePendingAdd(IRingStubMemory& memory, std::uint32_t g, std::uint32_t add);

// The deferred token: U32(dwords) | 0x81000000 at P+4, the physical address at
// P+8 and 0xC0000000 at P+0xC, unmasked.
bool StoreDeferredToken(IRingStubMemory& memory, std::uint32_t p, std::uint32_t physical, std::uint32_t dwords);

// True when the fence emitter would complete on the CPU (no sink, G+0x2ABD bit 0x2).
// Those stores would complete the fence before the host draws recorded ahead of it,
// so a Publish consume refuses the mode and runs Original.
bool FenceRunsInCpuMode(IRingStubMemory& memory, std::uint32_t g);

#endif

// 0x8269BE70 is satisfied once fence `requested` is no later than `completed`,
// measured back from F, the next fence, so the comparison survives wrap.
constexpr bool RingFenceSatisfied(std::uint32_t next, std::uint32_t requested, std::uint32_t completed) noexcept
{
    return static_cast<std::uint32_t>(next - requested) >= static_cast<std::uint32_t>(next - completed);
}

#if REREVVED_DEVELOPER

// Entry state a consume reads before it acts; each violation counts under
// rc_invariant_<field>. Detects invalid entry state, never an untraced writer.
enum class RingInvariant : std::uint8_t
{
    CursorOrder,       // S <= C + 4
    CursorExtent,      // C is not mapped
    StartExtent,       // S is not mapped
    FenceParity,       // F is unreadable or even
    PendingUnreadable, // G+0x2B00 is unreadable
    ListCapacity,      // the tiled list cursor is past its capacity
    Count,
};
inline constexpr std::size_t kRingInvariants = static_cast<std::size_t>(RingInvariant::Count);

inline constexpr std::array<std::string_view, kRingInvariants> kRingInvariantNames{
    "cursor_order",
    "cursor_extent",
    "start_extent",
    "fence_parity",
    "pending_unreadable",
    "list_capacity",
};

// Counts and logs the first few examples of each field.
void NoteRingInvariant(RingInvariant field, std::uint32_t g, std::uint64_t first, std::uint64_t second = 0) noexcept;
// The Publish plan's invariants plus F and N. Returns the number of violations.
unsigned CheckPublishEntry(IRingStubMemory& memory, std::uint32_t g, const PublishConsumePlan& plan) noexcept;
// F and N for a Submit that no Publish consume made.
unsigned CheckSubmitEntry(IRingStubMemory& memory, std::uint32_t g) noexcept;

// Waits the monitor measures. A scope covers one call; the vblank service polls the
// table, so a wait that never returns is still reported.
enum class RingWaitKind : std::uint8_t
{
    Fence,     // 0x8269BE70
    RingSpace, // 0x8269B1F0
    Drain,     // the pending-N loop of 0x8269C810
    Count,
};
inline constexpr std::size_t kRingWaitKinds = static_cast<std::size_t>(RingWaitKind::Count);

inline constexpr std::array<std::string_view, kRingWaitKinds> kRingWaitNames{ "fence", "ringspace", "drain" };

// Microseconds on a monotonic clock, never zero.
using RingConsumeClock = std::uint64_t (*)() noexcept;
// Test seam; null restores the steady clock.
void SetRingConsumeClock(RingConsumeClock clock) noexcept;

// Inert unless RingConsumePublishRequested() when it is constructed. `requested` is
// the fence a fence wait asks for; a drain waits for G+0x2B00 to reach zero.
class RingWaitScope
{
public:
    RingWaitScope(RingWaitKind kind, std::uint32_t device, std::uint64_t caller, std::uint32_t requested = 0) noexcept;
    ~RingWaitScope();

    RingWaitScope(const RingWaitScope&)            = delete;
    RingWaitScope& operator=(const RingWaitScope&) = delete;

private:
    std::uint64_t start_      = 0;
    std::uint32_t generation_ = 0;
    int           slot_       = -1;
    RingWaitKind  kind_       = RingWaitKind::Fence;
    bool          counted_    = false;
};

struct NoRingWaitScope
{
};

// The route trace wrappers of 0x8269B1F0 and 0x8269BE70 construct this as their first
// statement, so the stubbed ring-space wait and every fallback Original are covered.
constexpr bool RingWaitEntry(std::uint32_t address) noexcept
{
    return address == 0x8269B1F0 || address == 0x8269BE70;
}

template <std::uint32_t Address>
auto ObserveRingWait(std::uint32_t device, std::uint64_t caller, std::uint32_t requested) noexcept
{
    if constexpr (Address == 0x8269BE70)
        return RingWaitScope(RingWaitKind::Fence, device, caller, requested);
    else if constexpr (Address == 0x8269B1F0)
        return RingWaitScope(RingWaitKind::RingSpace, device, caller, requested);
    else
        return NoRingWaitScope{};
}

// A wait scope still open past the bound, reported once.
struct RingWaitReport
{
    RingWaitKind  kind       = RingWaitKind::Fence;
    std::uint32_t device     = 0;
    std::uint32_t caller     = 0;
    std::uint32_t thread     = 0;
    std::uint32_t requested  = 0;
    std::uint32_t generation = 0;
    std::uint64_t elapsedUs  = 0;
};

// Marks and returns the fence and drain scopes open for at least `thresholdUs` that
// were not reported before, counting rc_watchdog_fence and rc_watchdog_drain. An idle
// table reports nothing. It reads only the monitor's table.
std::vector<RingWaitReport> PollRingWaits(std::uint64_t thresholdUs) noexcept;
// Clears the table and the active counts and starts a new generation.
void ResetRingConsumeMonitor() noexcept;
// Withdrawing or disabling the services: notes an open wait or consume call for the
// final report, then resets. The note is cleared by the final take.
void RetireRingConsumeMonitor() noexcept;
// Runs inside PollRingWaits between its checks of a slot and the claim to report it,
// with the slot index. A test seam that lets a test reuse the slot at that point.
void SetRingPollClaimHookForTesting(void (*hook)(std::size_t slot)) noexcept;

// Brackets one consumed call, so a final report can tell that one was still running.
class RingConsumeCall
{
public:
    RingConsumeCall() noexcept;
    ~RingConsumeCall();

    RingConsumeCall(const RingConsumeCall&)            = delete;
    RingConsumeCall& operator=(const RingConsumeCall&) = delete;
};

struct RingWaitStats
{
    std::uint64_t calls = 0, totalUs = 0, maxUs = 0, active = 0;
};

// Supplemental timing separates terminal R1 outcomes, CPU-fence Original calls,
// and recording Original calls.
struct RingConsumeCounts
{
    std::uint64_t                              fault = 0, publishOriginalCpuFence = 0, recordingOriginal = 0;
    std::array<std::uint64_t, kRingInvariants> invariant{};
    std::array<RingWaitStats, kRingWaitKinds>  wait{};
    std::uint64_t                              watchdogFence = 0, watchdogDrain = 0, finalIncomplete = 0;
};

// A consumed call that failed after its first mutation: counts rc_fault and logs
// the first occurrence of each step.
void CountRingConsumeFault(std::string_view step) noexcept;
// A Publish call left on Original because its fence would complete on the CPU.
void CountRingConsumeCpuFence() noexcept;
void CountRingConsumeRecording() noexcept;
// The counts since the previous take. Active counts are not reset. `final` marks
// rc_final_incomplete when a consumed call or a wait is still open.
RingConsumeCounts TakeRingConsumeCounts(bool final = false) noexcept;
void              MergeRingConsumeCounts(RingConsumeCounts& into, const RingConsumeCounts& from) noexcept;
// " rc_<field>=N" for each nonzero field.
std::string FormatRingConsumeCounts(const RingConsumeCounts& counts);

struct RingConsumeReportOperations
{
    // Null uses the runtime logger. A throwing callback keeps the counts for the retry.
    void (*log)(std::string_view fields) = nullptr;
};

// Logs the final counts, or retains them when logging fails. Returns the fields.
std::string ReportRingConsume(const RingConsumeReportOperations& operations = {}) noexcept;

// Replaces the kernel spin-lock calls Submit's deferred check makes; null restores
// them. A test seam defined with the hooks.
using RingConsumeLockFn = void (*)(PPCContext& ctx, std::uint8_t* base);
void SetRingConsumeLockCallsForTesting(RingConsumeLockFn acquire, RingConsumeLockFn release) noexcept;

#else

// The shipping build has no wait monitor: the route wrappers keep their source shape.
struct NoRingWaitScope
{
};

template <std::uint32_t>
constexpr NoRingWaitScope ObserveRingWait(std::uint32_t, std::uint64_t, std::uint32_t) noexcept
{
    return {};
}

#endif

// Whether the first "<stub>_original" warning was logged, and a reset of those one-shot
// flags. Test seams defined with the hooks.
bool RingStubOriginalLoggedForTesting(RingStub stub) noexcept;
void ResetRingStubLogsForTesting() noexcept;

// The consume leases held across all threads; a test seam for the lease release before
// the rollover helper.
std::uint32_t RingStubLeaseCountForTesting() noexcept;

} // namespace rerevved::gpu::guest_d3d
