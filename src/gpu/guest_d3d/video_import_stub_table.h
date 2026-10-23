#pragma once

#include <rex/ppc/func.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// The retained SDK exports the table's rows skip.
extern "C" void __imp__VdIsHSIOTrainingSucceeded(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__VdRetrainEDRAMWorker(PPCContext& ctx, uint8_t* base);

namespace rerevved::gpu::guest_d3d
{

// One immutable table of before-instruction import stubs. Each row skips only
// the one import call at its PC, as the SDK export would: the whole r3 is
// assigned a constant, no guest byte and no other register is written, and the
// hook jumps to the instruction after the bl. Every row has its own options,
// counters and observation records.
//
// Skipping the bl leaves LR at its value before the call over the interval
// that ends at the next retained bl.
//   HSIO:   LR stays 0x826A6594 from 0x826A65A8 to 0x826A65C4; the bl at
//           0x826A65C8 sets 0x826A65CC.
//   worker: LR stays 0x826B0530 from 0x826B0540 to 0x826B0560; the bl at
//           0x826B0564 sets 0x826B0568.
// The observers inside those intervals take no LR and read none.
//
// The table has two rows, VdIsHSIOTrainingSucceeded and VdRetrainEDRAMWorker.
// The scaler and display mode stubs are separate implementations. A further row
// needs its own predicate, options and records.
enum class VideoImportId : std::uint8_t
{
    Hsio,
    EdramWorker,
};
inline constexpr std::size_t kVideoImportCount = 2;

enum class VideoImportReadiness : std::uint8_t
{
    RingStubsReady, // RingStubsReady(uint32_t(r31)) under the lease
};

inline constexpr std::uint32_t kVideoHsioCallPC          = 0x826A65A4;
inline constexpr std::uint32_t kVideoHsioReturnPC        = 0x826A65A8;
inline constexpr std::uint32_t kVideoHsioEffectPC        = 0x826A65C4;
inline constexpr std::uint32_t kVideoHsioPostPC          = 0x826A65CC;
inline constexpr std::uint32_t kVideoHsioImportAddress   = 0x82E92524;
inline constexpr std::uint32_t kVideoHsioCacheAddress    = 0x82F9B0A4; // the owner's cached training result
inline constexpr std::uint64_t kVideoHsioR30             = 0xFFFFFFFF82FA0000ULL;
inline constexpr std::uint32_t kVideoHsioFrameBytes      = 128;
inline constexpr std::uint32_t kVideoWorkerCallPC        = 0x826B053C;
inline constexpr std::uint32_t kVideoWorkerReturnPC      = 0x826B0540;
inline constexpr std::uint32_t kVideoWorkerPrePC         = 0x826B0564;
inline constexpr std::uint32_t kVideoWorkerPostPC        = 0x826B0568;
inline constexpr std::uint32_t kVideoWorkerImportAddress = 0x82E92634;
inline constexpr std::uint32_t kVideoWorkerFrameBytes    = 2192;
inline constexpr std::uint32_t kVideoWorkerStackOffsetA  = 80; // zeroed by the owner before the retrain
inline constexpr std::uint32_t kVideoWorkerStackOffsetB  = 84;
inline constexpr std::uint32_t kVideoWorkerArgOffset     = 96;
inline constexpr std::uint32_t kVideoImportMaxFrameBytes = 2192;
inline constexpr std::uint32_t kVideoImportMaxPending    = 64;
inline constexpr auto          kVideoImportShutdownWait  = std::chrono::milliseconds(10);

// Why a call ran the SDK import with an option on, in the order the checks run.
enum class VideoImportStubRefusal : std::uint8_t
{
    Pc,          // the table has no such row, or the row disagrees with its lookup
    Services,    // no lease, row readiness failed, no runtime, a graphics system or no delegate or ring service
    Provider,    // the dispatcher target is missing or is not the row's retained wrapper
    Diagnostics, // the exit check forces the import to run
    Stack,       // the owner's frame cannot be identified or read
    Shape,       // the row's predicate rejects the registers
    Unreadable,  // a required entry word cannot be read
    Reentrant,   // an earlier call on this thread has not finished its observations
    Unpaired,    // no observation record can be made
};
inline constexpr std::size_t kVideoImportRefusalCount = static_cast<std::size_t>(VideoImportStubRefusal::Unpaired) + 1;

inline constexpr std::array<std::string_view, kVideoImportRefusalCount> kVideoImportRefusalNames{
    "pc",
    "services",
    "provider",
    "diagnostics",
    "stack",
    "shape",
    "unreadable",
    "reentrant",
    "unpaired",
};

enum class VideoImportMismatch : std::uint8_t
{
    Return,       // the skipped import's full result
    Effect,       // HSIO cache, r11 and CR0; worker stack words
    Continuation, // retained registers and arguments
    Lr,           // the worker's LR after the retained retrain
};
inline constexpr std::size_t kVideoImportMismatchCount = static_cast<std::size_t>(VideoImportMismatch::Lr) + 1;

inline constexpr std::array<std::string_view, kVideoImportMismatchCount> kVideoImportMismatchNames{
    "return",
    "effect",
    "continuation",
    "lr",
};

// Guest memory permits checked reads only.
struct VideoImportStubMemory
{
    // A big-endian word; false when it is not readable.
    std::function<bool(std::uint32_t address, std::uint32_t& value)> read32;
};

// The integers bound at the entry and compared at every later boundary. A zero
// is an absent object.
struct VideoImportIdentity
{
    std::uint64_t runtime = 0, delegate = 0, ring = 0, target = 0;

    bool operator==(const VideoImportIdentity& other) const noexcept = default;
};

// What a probe saw. The shared pointers are boundary-local; a record keeps only
// weak references to them.
struct VideoImportEnvironment
{
    bool                        runtime    = false;   // kernel_state()->emulator() is valid
    bool                        graphics   = false;   // graphics_system() is not null
    bool                        delegate   = false;   // detached_video_delegate() is not null
    bool                        ring       = false;   // CurrentRingStubServices() is not null
    bool                        dispatcher = false;   // function_dispatcher() is not null
    PPCFunc*                    target     = nullptr; // GetFunction(row.importAddress)
    VideoImportIdentity         identity;
    std::shared_ptr<const void> delegateOwner, ringOwner;
};

struct VideoImportStubRow;

struct VideoImportStubOperations
{
    VideoImportStubMemory memory;
    // Called under the ring-service lease at the entry, and without it at every later boundary.
    std::function<VideoImportEnvironment(const VideoImportStubRow&)> environment;
    std::function<void(std::string_view)>                            log;
    // Test seam: called right before the full r3 is assigned, while the lease is held.
    std::function<void()> beforeCommit;
};

// The raw registers of the hook before the bl. Irrelevant fields are unused.
struct VideoImportStubCall
{
    std::uint64_t r1 = 0, r3 = 0, r11 = 0, r29 = 0, r30 = 0, r31 = 0, r13 = 0, lr = 0;
};

// The registers of the observers. The observers inside the LR divergence
// intervals have no LR; the post-child observer receives it separately.
struct VideoImportObservation
{
    std::uint64_t r1 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r11 = 0, r29 = 0, r30 = 0, r31 = 0, r13 = 0;
    std::uint32_t cr0 = 0; // CR0.raw(): lt, gt, eq, so
};

using VideoImportStubPredicate = bool (*)(const VideoImportStubCall&) noexcept;

// The HSIO row: the just-loaded cache word r11 is zero and r30 is the cache base.
bool VideoHsioPredicate(const VideoImportStubCall& call) noexcept;
// The worker row: the call's r3 is zero and the owner's saved r5 has a nonzero low word.
bool VideoEdramWorkerPredicate(const VideoImportStubCall& call) noexcept;

struct VideoImportStubRow
{
    std::uint32_t            callPC;
    VideoImportId            import;
    std::uint64_t            fullR3;
    std::uint32_t            truePC;
    VideoImportStubPredicate predicate;
    std::uint32_t            importAddress;
    PPCFunc*                 wrapper;
    std::uint32_t            frameBytes;
    VideoImportReadiness     readiness;
};

inline constexpr std::array<VideoImportStubRow, kVideoImportCount> kVideoImportStubRows{ {
    { kVideoHsioCallPC, VideoImportId::Hsio, 1, kVideoHsioReturnPC, &VideoHsioPredicate, kVideoHsioImportAddress, &__imp__VdIsHSIOTrainingSucceeded, kVideoHsioFrameBytes, VideoImportReadiness::RingStubsReady },
    { kVideoWorkerCallPC, VideoImportId::EdramWorker, 0, kVideoWorkerReturnPC, &VideoEdramWorkerPredicate, kVideoWorkerImportAddress, &__imp__VdRetrainEDRAMWorker, kVideoWorkerFrameBytes, VideoImportReadiness::RingStubsReady },
} };

// The row with exactly this PC and import, or null.
const VideoImportStubRow* FindVideoImportStubRow(std::uint32_t callPC, VideoImportId import) noexcept;

struct VideoImportStubState
{
    bool                   lease       = false; // the publication lease is held
    bool                   ready       = false; // the row's readiness under the lease
    bool                   diagnostics = false; // the exit check is on
    VideoImportEnvironment environment;
    // Set when the thread has no safe observation record for this call.
    std::optional<VideoImportStubRefusal> record;
};

// The entry words read before any commit: the whole frame and the HSIO cache.
struct VideoImportEntrySnapshot
{
    bool                                                     frameAttempted = false, frameValid = false, cacheAttempted = false, cacheValid = false;
    std::uint32_t                                            cache = 0;
    std::array<std::uint32_t, kVideoImportMaxFrameBytes / 4> frame{};
};

// The immutable result of a derivation: nothing was written.
struct VideoImportStubPlan
{
    const VideoImportStubRow* row = nullptr;
    VideoImportStubCall       call;
    std::uint64_t             fullR3 = 0;
    std::uint32_t             sp = 0, device = 0;
    VideoImportIdentity       identity;
    VideoImportEntrySnapshot  snapshot;
};

// Empty when the import may be replaced; `plan` is then filled. Reads the whole
// frame of the row and, for HSIO, the cache word.
std::optional<VideoImportStubRefusal> DeriveVideoImportStub(const VideoImportStubRow& row, const VideoImportStubCall& call, const VideoImportStubState& state, const VideoImportStubMemory& memory, VideoImportStubPlan& plan);

// The one effect of a replaced call: the whole r3 is the row's constant.
void ApplyVideoImportStub(const VideoImportStubPlan& plan, std::uint64_t& r3) noexcept;

// The cvars, per import. Requested is the stub option while ring stub services
// are published, Active is either option, and reporting intent is either option
// whether or not the services are published.
bool VideoHsioStubRequested() noexcept;
bool VideoHsioExitCheckRequested() noexcept;
bool VideoHsioActive() noexcept;
bool VideoHsioStubReportingRequested() noexcept;
bool VideoEdramWorkerStubRequested() noexcept;
bool VideoEdramWorkerExitCheckRequested() noexcept;
bool VideoEdramWorkerActive() noexcept;
bool VideoEdramWorkerStubReportingRequested() noexcept;
// Either import's reporting intent: the timing and shutdown drains.
bool VideoImportStubsReportingRequested() noexcept;

// True when the import is replaced: r3 is then the row's constant and the hook
// jumps to the row's truePC.
bool ReplaceVideoImportStubCall(std::uint32_t callPC, VideoImportId import, const VideoImportStubCall& call, std::uint64_t& r3) noexcept;
bool ReplaceVideoImportStubCallWith(const VideoImportStubOperations& operations, std::uint32_t callPC, VideoImportId import, const VideoImportStubCall& call, std::uint64_t& r3) noexcept;

// Import result captures return PC, HSIO/worker arguments, and post-child
// observation; worker r29 low word zero bypasses work.
void ObserveVideoImportReturn(VideoImportId import, const VideoImportObservation& observation) noexcept;
void ObserveVideoImportEffect(VideoImportId import, const VideoImportObservation& observation) noexcept;
void ObserveVideoImportPost(VideoImportId import, const VideoImportObservation& observation, std::uint64_t lr) noexcept;

struct VideoImportStubCounts
{
    std::uint64_t                                        enter = 0, readyAtEntry = 0, consumed = 0, original = 0;
    std::array<std::uint64_t, kVideoImportRefusalCount>  refusal{};
    std::uint64_t                                        exitOk = 0;
    std::array<std::uint64_t, kVideoImportMismatchCount> mismatch{};
    std::uint64_t                                        exitUnreadable = 0, exitUnpaired = 0, reentry = 0, abandoned = 0, correlationLost = 0, reportingFailed = 0;
    std::uint64_t                                        pending      = 0; // started calls whose observations have not finished
    std::uint64_t                                        finalSkipped = 0;
    bool                                                 unresolved   = false; // a quality failure survives every drain
    bool                                                 final        = false;
};

// Returns the counts since the previous take and resets them. `pending` is a
// gauge and is not reset. The retained skipped final count and the final flag
// belong to a final take only.
VideoImportStubCounts TakeVideoHsioStubCounts(bool final = false) noexcept;
VideoImportStubCounts TakeVideoEdramWorkerStubCounts(bool final = false) noexcept;
// " video_hsio_enter=N video_hsio_ready_at_entry=N ..." and the same with the
// video_edram_worker_ prefix; a final take adds _final_incomplete and _final_skipped.
std::string FormatVideoHsioStubCounts(const VideoImportStubCounts& counts);
std::string FormatVideoEdramWorkerStubCounts(const VideoImportStubCounts& counts);

struct VideoImportReportOperations
{
    std::string (*format)(VideoImportId import, const VideoImportStubCounts&) = nullptr;
    void (*log)(std::string_view)                                             = nullptr;
};

// Final import drain waits at most kVideoImportShutdownWait for a concurrent
// drain, then logs skipped-final with outstanding calls. Fields are empty with
// import options off.
std::string ReportVideoHsioStub(bool shutdown = false, const VideoImportReportOperations& operations = {}) noexcept;
std::string ReportVideoEdramWorkerStub(bool shutdown = false, const VideoImportReportOperations& operations = {}) noexcept;
// Both requested imports under one lock and one bounded wait.
std::string ReportVideoImportStubs(bool shutdown = false, const VideoImportReportOperations& operations = {}) noexcept;

namespace detail
{

std::timed_mutex& VideoImportReportMutex() noexcept;

} // namespace detail

// Test seams: operations replacing the production adapter (null restores it),
// the per-thread bookkeeping touch count, the per-import outstanding-record
// bound and the probe between the bound check and the slot reservation.
void          SetVideoImportOperationsForTesting(const VideoImportStubOperations* operations) noexcept;
std::uint64_t VideoImportThreadTouchesForTesting() noexcept;
void          SetVideoImportPendingLimitForTesting(VideoImportId import, std::uint32_t limit) noexcept;
void          SetVideoImportReserveProbeForTesting(void (*probe)()) noexcept;
// Restores the budget of sixteen logged ok records per import.
void ResetVideoImportLogBudgetForTesting() noexcept;
// Clears every counter, gauge, sticky flag and bound of both imports and the
// calling thread's records, for a fixture that starts from nothing.
void ResetVideoImportStateForTesting() noexcept;
// The production probe and memory read, callable without the hook.
VideoImportEnvironment ProductionVideoImportEnvironmentForTesting(const VideoImportStubRow& row);
bool                   ProductionVideoImportRead32ForTesting(std::uint32_t address, std::uint32_t& value) noexcept;

} // namespace rerevved::gpu::guest_d3d
