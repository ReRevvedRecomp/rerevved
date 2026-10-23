#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Tiling probe follows EndTiling 0x826AC218, submits 0x8269BD98, worker
// G+0x2AC0, queues G+0x2C3C+80*i, interpreter 0x826ABB90 and token handler
// 0x826AB740.
inline constexpr std::uint32_t kTilingWorker      = 0x2AC0; // W
inline constexpr std::uint32_t kTilingWindowBase  = 0x2AC0;
inline constexpr std::uint32_t kTilingWindowEnd   = 0x2D00;
inline constexpr std::size_t   kTilingWindowWords = (kTilingWindowEnd - kTilingWindowBase) / 4;
inline constexpr std::uint32_t kTilingQueueBase   = 0x2C3C; // Q_0
inline constexpr std::uint32_t kTilingQueueStride = 80;
inline constexpr std::size_t   kTilingQueueCount  = 6;
inline constexpr std::uint32_t kTilingDeviceBytes = 0x5E00; // G .. G+0x5E00: the 24064 bytes device creation 0x8268D2C8 allocates
inline constexpr std::size_t   kQuiesceLogWords   = 8;      // differing words kept for the log, per bracket
inline constexpr std::size_t   kTilingLogLimit    = 8;      // first-N limit for every log line family
inline constexpr std::size_t   kTilingDistinctCap = 64;     // distinct values tracked per family

// G[0x2AC0, 0x2D00) as host-order words.
using TilingWindow = std::array<std::uint32_t, kTilingWindowWords>;

// Replay window tracks G+0x2AC8, W+0x2C, G+0x2B00/G+0x2B04 and Q_i+0x38/+0x3C.
bool IsExpectedQuiesceOffset(std::uint32_t offset) noexcept;

struct QuiesceWord
{
    std::uint32_t offset = 0; // relative to G
    std::uint32_t before = 0;
    std::uint32_t after  = 0;
};

struct QuiesceDiff
{
    std::uint32_t                             words     = 0;     // words that differ and are not expected
    bool                                      waitCount = false; // W+0x2C differs
    std::uint32_t                             logged    = 0;
    std::array<QuiesceWord, kQuiesceLogWords> first{};
};

QuiesceDiff CompareQuiesceWindow(const TilingWindow& before, const TilingWindow& after) noexcept;

// The worker state that says no list is in flight: the lock W+0 free, no list in
// progress (W+0x3C), no pending list (W+0x48), and every queue's pushed count
// (Q_i+0x38) equal to its consumed count (Q_i+0x3C).
struct TilingWorkerState
{
    std::uint32_t                                lock    = 0;
    std::uint32_t                                lists   = 0;
    std::uint32_t                                pending = 0;
    std::array<std::uint32_t, kTilingQueueCount> pushed{};
    std::array<std::uint32_t, kTilingQueueCount> consumed{};
};

bool TilingWorkersIdle(const TilingWorkerState& state) noexcept;

// bit 31-clear=record; handler 0x80..0x8B/0x8E controls. Interpreter 0x8C000000
// sets W+0x10/+0x14, 0x8D000000 sets W+0x168; 0xC0000000=end, other top-two-set
// word=link; remaining controls separate.
enum class TilingToken : std::uint8_t
{
    Record,
    T80,
    T81,
    T82,
    T83,
    T84,
    T85,
    T86,
    T87,
    T88,
    T89,
    T8A,
    T8B,
    T8E,
    T8C,
    T8D,
    End,
    Link,
    Other,
};
inline constexpr std::size_t kTilingTokenKinds = static_cast<std::size_t>(TilingToken::Other) + 1;

TilingToken ClassifyTilingToken(std::uint32_t word) noexcept;
// The timing-line name of a kind: "record", "80", "8c", "end", "other".
const char* TilingTokenName(TilingToken token) noexcept;

// The call sites of 0x8269BD98 inside EndTiling, by the return address the
// callee finds in lr: the optional coherency submit, the middle submit that
// carries the worker-kick callback (r7 = 1) and the final fence submit (r7 = 0).
enum class TilingSubmitSite : std::uint8_t
{
    Optional, // 0x826AC364, returns to 0x826AC368
    Middle,   // 0x826AC3B0, returns to 0x826AC3B4
    Final,    // 0x826AC3E4, returns to 0x826AC3E8
    Other,
};
inline constexpr std::size_t kTilingSubmitSites = static_cast<std::size_t>(TilingSubmitSite::Other) + 1;

TilingSubmitSite ClassifyTilingSubmitSite(std::uint32_t lr) noexcept;
const char*      TilingSubmitSiteName(TilingSubmitSite site) noexcept;

// Which arm of 0x8269BD98 served a call: the deferred chain onto the worker's
// list, or a direct submit (the Original's or the ring stub's).
enum class TilingArm : std::uint8_t
{
    None,
    Deferred,
    Direct,
};

// The four stores whose destination the replay list selects.
enum class TilingWriteSite : std::uint8_t
{
    B120, // 0x826AB120, P+8
    B4C8, // 0x826AB4C8, P+0
    B60C, // 0x826AB60C, P+0
    B6FC, // 0x826AB6FC, P+0
};
inline constexpr std::size_t kTilingWriteSites = static_cast<std::size_t>(TilingWriteSite::B6FC) + 1;

enum class TilingWriteClass : std::uint8_t
{
    Device, // inside G .. G+0x5E00
    Ring,   // inside the registered command ring allocation
    Other,
};
inline constexpr std::size_t kTilingWriteClasses = static_cast<std::size_t>(TilingWriteClass::Other) + 1;

// Where a four-byte store at `address` lands. `physical` is its physical alias
// and `ringBytes` is zero while no ring is registered.
TilingWriteClass ClassifyTilingWrite(std::uint32_t address, std::uint32_t device, std::uint32_t physical, std::uint32_t ringPhysical, std::uint32_t ringBytes) noexcept;
const char*      TilingWriteSiteName(TilingWriteSite site) noexcept;
const char*      TilingWriteClassName(TilingWriteClass kind) noexcept;

struct TilingSubmitCounts
{
    std::uint64_t calls = 0, r7Zero = 0, r7One = 0, r7Other = 0;
    std::uint64_t deferred = 0, direct = 0, noArm = 0;
    std::uint64_t beforeNonzero = 0, afterNonzero = 0;
    std::uint64_t addsR7 = 0, addsOther = 0; // G+0x2B00 after minus before: r7, or anything else
};

struct TilingProbeCounts
{
    std::uint64_t                                                                 bracketEnd = 0, bracketDropped = 0, bracketUnreadable = 0;
    std::uint64_t                                                                 quiesceSnapshots = 0, quiesceOk = 0, quiesceDelta = 0, quiesceDeltaWords = 0, quiesceWaitCount = 0, quiesceUnobserved = 0;
    std::uint64_t                                                                 quiesceBusy = 0, quiesceEarly = 0, quiesceStale = 0, quiesceOverlap = 0, quiescePendingNonzero = 0;
    std::uint64_t                                                                 workerIdle = 0, workerLists = 0;
    std::uint64_t                                                                 guardNonzero = 0, guardZero = 0;
    std::array<TilingSubmitCounts, kTilingSubmitSites>                            submit{};
    std::array<std::uint64_t, kTilingTokenKinds>                                  token{};
    std::uint64_t                                                                 callbackCalls = 0, callbackDistinct = 0, installDistinct = 0;
    std::array<std::array<std::uint64_t, kTilingWriteClasses>, kTilingWriteSites> write{};
    std::uint64_t                                                                 ringRegistered = 0;
    std::uint64_t                                                                 publisher = 0, sink541C = 0, sink5B0C = 0;
    std::uint64_t                                                                 watchdogEnter = 0, watchdogGlobal = 0, watchdogReset = 0, watchdogZero = 0;
    std::uint64_t                                                                 errors = 0;
};

// What a worker-idle observation did to the pending bracket.
struct IdleVerdict
{
    enum class Kind : std::uint8_t
    {
        NoBracket, // nothing was waiting for the worker
        Busy,      // another queue or the interpreter still holds work
        Early,     // no list was interpreted since the bracket began
        Stale,     // a bracket or a list started after the observation was sampled
        Ok,
        Delta,
    };
    Kind        kind = Kind::NoBracket;
    QuiesceDiff diff{};
};

// Tiling counters log first differing words, callbacks, write targets, and
// sinks; each returns true only for its log event.
class TilingProbe
{
public:
    // The bracket and list counts an idle observation is sampled under.
    struct Generation
    {
        std::uint64_t bracket     = 0; // snapshots taken
        std::uint64_t interpreter = 0; // interpreter entries
        std::uint64_t inFlight    = 0; // interpreter runs that entered and have not returned
    };

    Generation Sample();

    // EndTiling was entered. Returns the interpreter entries so far, for NoteBracketEnd.
    std::uint64_t NoteBracketStart();
    // EndTiling receives prior started token; r3!=0 leaves pending snapshot
    // unchanged, unreadable device gives empty window. True means replacing a
    // snapshot never reported idle.
    bool NoteBracketEnd(bool dropped, const TilingWindow* window, std::uint64_t started);
    // The interpreter 0x826ABB90 started a list run, and returned from it.
    void NoteInterpreterEntry();
    void NoteInterpreterExit();
    // Park samples before guest reads; pendingNonzero=G+0x2B00!=0. Stale if
    // list ran in flight at sample or bracket/list began since; no run then/no
    // entry since proves none ran during guest reads.
    IdleVerdict NoteWorkerIdle(const Generation& sampled, const TilingWorkerState& state, const TilingWindow* current, bool pendingNonzero);

    void NoteGuard(bool nonzero);
    // The call of 0x8269BD98 at `site` returned: r7 and G+0x2B00 before and after.
    void NoteSubmit(TilingSubmitSite site, std::uint32_t r7, std::uint32_t before, std::uint32_t after, TilingArm arm);

    // Count interpreter records, 0x8C/0x8D, end/link only there; count other
    // controls only at handler. Seeing a word at the other site counts nothing,
    // once total. Log first kTilingLogLimit unknown controls.
    bool NoteToken(std::uint32_t word, bool handler);
    // The 0x8C token's payload, and the W+0x10/W+0x14 pair at a callback call.
    bool NoteInstalled(std::uint32_t function, std::uint32_t context);
    bool NoteCallback(std::uint32_t function, std::uint32_t context);

    // A list-selected store. Returns true for the first kTilingLogLimit distinct other targets.
    bool NoteWrite(TilingWriteSite site, TilingWriteClass kind, std::uint32_t address);

    void NoteRing(std::uint32_t physical, std::uint32_t log2);
    // The extent of the latest registered ring, bytes zero before the first.
    void RingExtent(std::uint32_t& physical, std::uint32_t& bytes);

    // The publisher 0x8269B900 found the sinks G+0x541C / G+0x5B0C: a distinct
    // nonnull value pair returns true, up to kTilingLogLimit pairs.
    bool NotePublisher(std::uint32_t sink, std::uint32_t callback);
    // 0 entered, 1 returned at the global gate, 2 reset path, 3 reread zero. Returns
    // true for the first kTilingLogLimit resets or zeros.
    bool NoteWatchdog(int kind);

    void              NoteError() noexcept;
    TilingProbeCounts Take();

private:
    std::mutex        mutex_;
    TilingProbeCounts counts_;
    std::uint64_t     interpreterTotal_ = 0;
    std::uint64_t     inFlight_         = 0;
    std::uint64_t     errors_           = 0;
    // The pending snapshot of the latest completed bracket.
    bool                       pending_ = false;
    TilingWindow               snapshot_{};
    std::uint64_t              pendingInterpreter_ = 0;
    std::uint64_t              bracketTotal_       = 0;
    bool                       pendingOverlap_     = false;
    std::size_t                deltaLogged_        = 0;
    std::size_t                otherLogged_        = 0;
    std::size_t                watchdogLogged_     = 0;
    std::uint32_t              ringPhysical_ = 0, ringBytes_ = 0;
    std::vector<std::uint64_t> callbacks_, installs_, writes_, sinks_;
    std::vector<std::uint32_t> otherWords_;
};

// The cvar native_tiling_quiesce_check, read once: -1 until the first query.
extern std::atomic<std::int8_t> tilingProbeState;
bool                            RefreshTilingProbeRequested() noexcept;

// One relaxed load once the cvar has been read.
inline bool TilingProbeRequested() noexcept
{
    const auto state = tilingProbeState.load(std::memory_order_relaxed);
    return state < 0 ? RefreshTilingProbeRequested() : state != 0;
}

// The process probe the hooks feed.
TilingProbe& PublishedTilingProbe() noexcept;

// Hook entries: read the guest, feed the process probe, and never throw or stop
// the guest. All return at once without the cvar.

// EndTiling 0x826AC218 on device `device`; the destructor reads `result`, the
// r3 the call leaves (nonzero when the bracket was dropped), and snapshots the
// window of a completed bracket.
class TilingBracketScope
{
public:
    // The probe being off costs this flag test and the one in the destructor.
    TilingBracketScope(std::uint32_t device, const std::uint64_t& result) noexcept
    : device_(device)
    , result_(result)
    , active_(TilingProbeRequested())
    {
        if (active_) [[unlikely]]
            Begin();
    }

    ~TilingBracketScope()
    {
        if (active_) [[unlikely]]
            End();
    }

    TilingBracketScope(const TilingBracketScope&)            = delete;
    TilingBracketScope& operator=(const TilingBracketScope&) = delete;

private:
    void Begin() noexcept;
    void End() noexcept;

    std::uint32_t        device_;
    const std::uint64_t& result_;
    bool                 active_;
    std::uint64_t        started_ = 0;
};

// 0x8269BD98 on device `device` with the return address `lr` and incoming r7.
class TilingSubmitScope
{
public:
    TilingSubmitScope(std::uint32_t device, std::uint64_t lr, std::uint32_t r7) noexcept
    : device_(device)
    , lr_(static_cast<std::uint32_t>(lr))
    , r7_(r7)
    , active_(TilingProbeRequested())
    {
        if (active_) [[unlikely]]
            Begin();
    }

    ~TilingSubmitScope()
    {
        if (active_) [[unlikely]]
            End();
    }

    TilingSubmitScope(const TilingSubmitScope&)            = delete;
    TilingSubmitScope& operator=(const TilingSubmitScope&) = delete;

private:
    void Begin() noexcept;
    void End() noexcept;

    std::uint32_t device_, lr_, r7_, before_ = 0;
    bool          active_;
    bool          readable_ = false;
};

// The arm the current thread's 0x8269BD98 call took, from the deferred-kick and
// direct-submit observers and the ring stub's direct path.
void NoteTilingSubmitArm(TilingArm arm) noexcept;

void ObserveTilingGuard(std::uint32_t guard) noexcept;
void ObserveTilingInterpreter() noexcept;
void ObserveTilingInterpreterExit() noexcept;
void ObserveTilingListWord(std::uint32_t address) noexcept;
void ObserveTilingHandlerToken(std::uint32_t word) noexcept;
void ObserveTilingHandlerLink(std::uint32_t word) noexcept;
void ObserveTilingCallback(std::uint32_t function, std::uint32_t worker) noexcept;
void ObserveTilingWrite(TilingWriteSite site, std::uint32_t destination) noexcept;
void ObserveTilingWorkerIdle(std::uint32_t worker) noexcept;
void ObserveTilingPublisher(std::uint32_t device) noexcept;
void ObserveTilingWatchdog(int kind, std::uint32_t device) noexcept;
void ObserveTilingRing(std::uint32_t physical, std::uint32_t log2) noexcept;

// The " tiling_<name>=N" fields, all of them, in a fixed order.
std::string FormatTilingProbeCounts(const TilingProbeCounts& counts);

} // namespace rerevved::gpu::guest_d3d
