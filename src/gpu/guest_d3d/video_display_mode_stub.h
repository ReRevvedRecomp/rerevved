#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace rerevved::gpu::guest_d3d
{

// Video display-mode guest-hook fact table:
// Hook: with native_video_display_mode_stub, 0x826A6260 in owner sub_826A61F0
//   skips only the VdSetDisplayMode import at 0x826A6260.
// Stub effect: it behaves as the SDK export, leaves every guest byte and
//   register alone, and returns 0 in full r3; no hardware mode switch and no
//   guest code is removed beyond that one call.
// Retained owner path: 0x826A6264 executes addi r3,r1,80;
//   VdGetCurrentDisplayInformation remains, and the three stores G+0x5428,
//   G+0x542C, and G+0x5430 still run as guest code.
// LR interval: the skipped BL leaves LR pre-call rather than 0x826A6264 over
//   [0x826A6264, 0x826A6268); the interval's only guest instruction, addi
//   r3,r1,80, reads r1 rather than LR.
// Reconvergence: the retained BL at 0x826A6268 sets LR = 0x826A626C before the
//   next provider; the 0x826A6264 observation never reads LR, which is captured
//   only at entry 0x826A6260 and reconvergence 0x826A626C.
// Exit check: native_video_display_mode_exit_check forces the import and pairs
//   call 0x826A6260, return before addi 0x826A6264, and reconvergence before
//   the first lhz 0x826A626C; exit_ok requires a fully paired, readable,
//   matching call with a valid stable provider.
inline constexpr std::uint32_t kVideoDisplayModeHookAddress        = 0x826A6260;
inline constexpr std::uint32_t kVideoDisplayModeReturnAddress      = 0x826A6264;
inline constexpr std::uint32_t kVideoDisplayModeReconvergedAddress = 0x826A626C;
inline constexpr std::uint32_t kVideoDisplayModeImportAddress      = 0x82E924F4;
inline constexpr std::uint32_t kVideoDisplayModeFrameBytes         = 192;
inline constexpr std::uint32_t kVideoDisplayModeInfoOffset         = 80; // the argument of the retained display-information import
inline constexpr std::uint32_t kVideoDisplayModeInfoBytes          = 88;
inline constexpr std::uint32_t kVideoDisplayModeWidthOffset        = 72; // BE16 in the information, SP+152
inline constexpr std::uint32_t kVideoDisplayModeHeightOffset       = 74; // SP+154
inline constexpr std::uint32_t kVideoDisplayModeActualWidthOffset  = 86; // SP+166
inline constexpr std::uint32_t kVideoDisplayModeRequiredFlag       = 0x40000000;
inline constexpr std::uint32_t kVideoDisplayModeOptionalFlags      = 0x38000002;
inline constexpr std::uint32_t kVideoDisplayModeMaxPending         = 64;
inline constexpr auto          kVideoDisplayModeShutdownWait       = std::chrono::milliseconds(10);

// Why a call ran the SDK import with the cvar on, in the order the checks run.
enum class VideoDisplayModeRefusal : std::uint8_t
{
    Services,    // no lease, device not ready, no runtime, a graphics system or no detached delegate
    Provider,    // the dispatcher target is missing or is not the retained wrapper
    Diagnostics, // the exit check forces the import to run
    Stack,       // the supported owner frame cannot be identified
    Shape,       // the flags are not one of the owner's sixteen values
    Reentrant,   // an earlier call on this thread has not finished its observations
    Unpaired,    // no observation record can be made
};
inline constexpr std::size_t kVideoDisplayModeRefusalCount = static_cast<std::size_t>(VideoDisplayModeRefusal::Unpaired) + 1;

inline constexpr std::array<std::string_view, kVideoDisplayModeRefusalCount> kVideoDisplayModeRefusalNames{
    "services",
    "provider",
    "diagnostics",
    "stack",
    "shape",
    "reentrant",
    "unpaired",
};

enum class VideoDisplayModeMismatch : std::uint8_t
{
    Return,       // the full r3 at 0x826A6264 is not 0
    Continuation, // the full r3 at 0x826A626C is not the entry r1 + 80
    Lr,           // the raw LR at 0x826A626C is not 0x826A626C
};
inline constexpr std::size_t kVideoDisplayModeMismatchCount = static_cast<std::size_t>(VideoDisplayModeMismatch::Lr) + 1;

inline constexpr std::array<std::string_view, kVideoDisplayModeMismatchCount> kVideoDisplayModeMismatchNames{
    "return",
    "continuation",
    "lr",
};

// Guest memory permits checked reads only.
struct VideoDisplayModeMemory
{
    // A big-endian dword; false when the dword is not readable.
    std::function<bool(std::uint32_t address, std::uint32_t& value)> read32;
};

// The identities bound at the entry and compared at the reconvergence. A zero is
// an absent object.
struct VideoDisplayModeIdentity
{
    std::uint64_t runtime = 0, delegate = 0, ring = 0, target = 0;

    bool operator==(const VideoDisplayModeIdentity& other) const noexcept = default;
};

// What the call-time probe saw. Production reads the Runtime, its detached
// delegate, its dispatcher and the published ring services.
struct VideoDisplayModeEnvironment
{
    bool                     runtime       = false; // kernel_state()->emulator() is valid
    bool                     graphics      = false; // graphics_system() is not null
    bool                     delegate      = false; // detached_video_delegate() is not null
    bool                     dispatcher    = false; // function_dispatcher() is not null
    bool                     targetPresent = false; // GetFunction(0x82E924F4) is not null
    bool                     targetMatches = false; // ... and is the retained __imp__VdSetDisplayMode
    VideoDisplayModeIdentity identity;
};

struct VideoDisplayModeOperations
{
    VideoDisplayModeMemory memory;
    // Called under the ring-service lease at the entry, and without it at the reconvergence.
    std::function<VideoDisplayModeEnvironment()> environment;
    std::function<void(std::string_view)>        log;
    // Test seam: called right before the full r3 is assigned, while the lease is held.
    std::function<void()> beforeCommit;
};

// The registers the hook at 0x826A6260 reads. Only r1, r3 and r31 take part in
// the decision; LR is recorded and never decides.
struct VideoDisplayModeCall
{
    std::uint64_t r1 = 0, r3 = 0, r31 = 0, lr = 0;
};

struct VideoDisplayModeState
{
    bool                        lease       = false; // the publication lease is held
    bool                        ready       = false; // RingStubsReady(G)
    bool                        diagnostics = false; // the exit check is on
    VideoDisplayModeEnvironment environment;
    // Set when the thread has no safe observation record for this call.
    std::optional<VideoDisplayModeRefusal> record;
};

// The immutable result of a derivation: nothing was written.
struct VideoDisplayModePlan
{
    std::uint64_t            r1 = 0, r3 = 0, r31 = 0, lr = 0;
    std::uint32_t            sp = 0, device = 0;
    VideoDisplayModeIdentity identity;
};

// Empty when the import may be replaced; `plan` is then filled. Reads only the
// backchain word at SP.
std::optional<VideoDisplayModeRefusal> DeriveVideoDisplayModeStub(const VideoDisplayModeMemory& memory, const VideoDisplayModeCall& call, const VideoDisplayModeState& state, VideoDisplayModePlan& plan);

// The one effect of a replaced call: the full r3 is 0.
void ApplyVideoDisplayModeStub(const VideoDisplayModePlan& plan, std::uint64_t& r3) noexcept;

// The cvar native_video_display_mode_stub while ring stub services are published.
bool VideoDisplayModeStubRequested() noexcept;
// The cvar native_video_display_mode_exit_check.
bool VideoDisplayModeExitCheckRequested() noexcept;
// Either of the above: the hooks do any work only then.
bool VideoDisplayModeActive() noexcept;
// Either cvar, whether or not the services are published: the timing and shutdown drains.
bool VideoDisplayModeStubReportingRequested() noexcept;

// True when the import is replaced: r3 is then 0 and the hook jumps past the call.
bool ReplaceVideoDisplayModeCall(const VideoDisplayModeCall& call, std::uint64_t& r3) noexcept;
bool ReplaceVideoDisplayModeCallWith(const VideoDisplayModeOperations& operations, const VideoDisplayModeCall& call, std::uint64_t& r3) noexcept;
// The return before the addi at 0x826A6264 (r3 is the import's result) and the
// reconvergence before the first lhz at 0x826A626C. r5 is the owner's own
// argument: zero is the owner's bypass of the import and does no work.
void ObserveVideoDisplayModeReturn(std::uint64_t r1, std::uint64_t r3, std::uint32_t r5, std::uint64_t r31) noexcept;
void ObserveVideoDisplayModeReconverged(std::uint64_t r1, std::uint64_t r3, std::uint32_t r5, std::uint64_t r31, std::uint64_t lr) noexcept;

struct VideoDisplayModeStubCounts
{
    std::uint64_t                                             enter = 0, consumed = 0, original = 0;
    std::array<std::uint64_t, kVideoDisplayModeRefusalCount>  refusal{};
    std::uint64_t                                             exitOk = 0;
    std::array<std::uint64_t, kVideoDisplayModeMismatchCount> mismatch{};
    std::uint64_t                                             exitUnreadable = 0, exitUnpaired = 0, reentry = 0;
    std::uint64_t                                             pending      = 0; // started calls whose observations have not finished
    std::uint64_t                                             finalSkipped = 0;
    bool                                                      final        = false;
};

// Returns the counts since the previous take and resets them. `pending` is the
// number of calls still outstanding and is not reset.
VideoDisplayModeStubCounts TakeVideoDisplayModeStubCounts() noexcept;

std::string FormatVideoDisplayModeStubCounts(const VideoDisplayModeStubCounts& counts);

struct VideoDisplayModeReportOperations
{
    std::string (*format)(const VideoDisplayModeStubCounts&) = FormatVideoDisplayModeStubCounts;
    void (*log)(std::string_view)                            = nullptr;
};

// The fields of the native_frame_timing line, or the final log at shutdown. The
// final drain waits at most kVideoDisplayModeShutdownWait for a concurrent drain
// and logs a skipped final with its outstanding calls. Empty with both cvars off.
std::string ReportVideoDisplayModeStub(bool shutdown = false, const VideoDisplayModeReportOperations& operations = {}) noexcept;

namespace detail
{

std::timed_mutex& VideoDisplayModeReportMutex() noexcept;

} // namespace detail

// Test seams: operations replacing the production adapter (null restores it),
// the per-thread bookkeeping touch count and the outstanding-record bound.
void          SetVideoDisplayModeOperationsForTesting(const VideoDisplayModeOperations* operations) noexcept;
std::uint64_t VideoDisplayModeThreadTouchesForTesting() noexcept;
void          SetVideoDisplayModePendingLimitForTesting(std::uint32_t limit) noexcept;
// Called between the outstanding-record bound check and the slot reservation.
void SetVideoDisplayModeReserveProbeForTesting(void (*probe)()) noexcept;
// Restores the budget of sixteen logged ok records.
void ResetVideoDisplayModeLogBudgetForTesting() noexcept;
// The production probe and memory read, callable without the hook.
VideoDisplayModeEnvironment ProductionVideoDisplayModeEnvironmentForTesting();
bool                        ProductionVideoDisplayModeRead32ForTesting(std::uint32_t address, std::uint32_t& value) noexcept;

} // namespace rerevved::gpu::guest_d3d
