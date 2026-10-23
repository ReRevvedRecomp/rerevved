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

// SDK export call-shape stub, without scaler hardware emulation; guest packet
//   bytes remain intact.
// Site/output                  | Guest fact required by the stub and exit
//   checker
// 0x826B042C in sub_826B02B0    | native_video_scaler_stub skips only
//   VdInitializeScalerCommandBuffer at 0x826B0430.
// Stub result                  | 200 big-endian 0x80000000 dwords at SP+192;
//   full r3 = 200 (dword count).
// Retained guest work          | C7A8(G, 220) reservation; 800-byte copy to
//   [R+4, R+804); G+0x30 = R+800.
// LR after skipped BL          | Keeps its prior value instead of 0x826B0434
//   until retained memcpy sets 0x826B0448.
// LR safety                    | No intervening read; owner restores saved LR
//   in its epilogue.
// native_video_scaler_exit_check | Forces the import to run and pairs the same
//   three observations as the stub.
// Observation sites            | Call/baseline 0x826B042C; return before rlwinm
//   0x826B0434; copy/cursor end 0x826B044C.
// exit_ok                      | Only a fully paired, readable and matching
//   call counts.
inline constexpr std::uint32_t kVideoScalerHookAddress   = 0x826B042C;
inline constexpr std::uint32_t kVideoScalerImportAddress = 0x826B0430;
inline constexpr std::uint32_t kVideoScalerReturnAddress = 0x826B0434;
inline constexpr std::uint32_t kVideoScalerCopyAddress   = 0x826B044C;
inline constexpr std::uint32_t kVideoScalerFrameBytes    = 2464;
inline constexpr std::uint32_t kVideoScalerOutputOffset  = 192;
inline constexpr std::uint32_t kVideoScalerAboveSentinel = 992; // also the end of the output
inline constexpr std::uint32_t kVideoScalerBelowSentinel = 188;
inline constexpr std::uint32_t kVideoScalerWords         = 200;
inline constexpr std::uint32_t kVideoScalerBytes         = 800;
inline constexpr std::uint32_t kVideoScalerNop           = 0x80000000;
inline constexpr std::uint32_t kVideoScalerDeviceBytes   = 0x5E00; // the retained device allocation
inline constexpr std::uint32_t kVideoScalerCursorOffset  = 0x30;
inline constexpr std::uint32_t kVideoScalerMaxPending    = 64;
inline constexpr auto          kVideoScalerShutdownWait  = std::chrono::milliseconds(10);

// Why a call ran the SDK import with the cvar on, in the order the checks run.
enum class VideoScalerRefusal : std::uint8_t
{
    Services,    // no publication lease, or the validated device is not r31
    Diagnostics, // the exit check forces the import to run
    Stack,       // SP is misaligned, wraps, or an argument slot is not readable
    Shape,       // the arguments are not the owner's one call shape
    Unreadable,  // an observed sentinel word is not readable
    Unwritable,  // the 800-byte output is not entirely writable
    Reservation, // the reservation, its copy destination or G+0x30 is unusable
    Alias,       // two checked ranges share physical or virtual backing, or one is unknown
    Reentrant,   // an earlier call on this thread has not finished its observations
    Unpaired,    // no observation record can be made
};
inline constexpr std::size_t kVideoScalerRefusalCount = static_cast<std::size_t>(VideoScalerRefusal::Unpaired) + 1;

inline constexpr std::array<std::string_view, kVideoScalerRefusalCount> kVideoScalerRefusalNames{
    "services",
    "diagnostics",
    "stack",
    "shape",
    "unreadable",
    "unwritable",
    "reservation",
    "alias",
    "reentrant",
    "unpaired",
};

enum class VideoScalerMismatch : std::uint8_t
{
    Output,
    Return,
    Copy,
    Cursor,
    Sentinel,
};
inline constexpr std::size_t kVideoScalerMismatchCount = static_cast<std::size_t>(VideoScalerMismatch::Sentinel) + 1;

inline constexpr std::array<std::string_view, kVideoScalerMismatchCount> kVideoScalerMismatchNames{
    "output",
    "return",
    "copy",
    "cursor",
    "sentinel",
};

// Guest memory as the stub sees it. Every operation is checked and never throws
// into guest code.
struct VideoScalerMemory
{
    // A big-endian dword; false when the dword is not readable.
    std::function<bool(std::uint32_t address, std::uint32_t& value)> read32;
    // Contiguous backing identity: physical heap physical address, virtual heap
    // 2^32+address. Empty if any byte lacks access or backing is
    // unknown/noncontiguous.
    std::function<std::optional<std::uint64_t>(std::uint32_t address, std::uint32_t bytes, bool write)> identity;
    // A big-endian dword store; false when the dword is not writable.
    std::function<bool(std::uint32_t address, std::uint32_t value)> write32;
};

struct VideoScalerOperations
{
    VideoScalerMemory memory;
    // The fail-stop after a write fault that left partial output.
    std::function<void(std::string_view)> terminal;
    std::function<void(std::string_view)> log;
};

// The registers the hook at 0x826B042C reads. Only r1, r9, r30 and r31 take
// part in the decision; r3-r10 are kept to show the call is not altered.
struct VideoScalerCall
{
    std::uint64_t r1 = 0;
    std::uint64_t arguments[8]{}; // r3..r10
    std::uint64_t r30 = 0;
    std::uint64_t r31 = 0;
};

struct VideoScalerBaseline
{
    bool          valid = false;
    std::uint32_t below = 0, above = 0, header = 0, tail = 0; // [SP+188], [SP+992], [R], [R+804]
};

struct VideoScalerState
{
    bool services    = false; // publication lease held and the validated device is r31
    bool diagnostics = false; // the exit check is on
    // Set when the thread has no safe observation record for this call.
    std::optional<VideoScalerRefusal> record;
};

// The immutable result of a derivation: nothing was written.
struct VideoScalerPlan
{
    std::uint32_t       sp = 0, device = 0, reservation = 0;
    std::uint32_t       output      = 0; // SP + 192, 200 dwords
    std::uint32_t       copy        = 0; // R + 4, where the owner copies 800 bytes
    std::uint32_t       cursor      = 0; // G + 0x30
    std::uint32_t       cursorValue = 0; // R + 800
    VideoScalerBaseline baseline;
};

// Empty when the import may be replaced; `plan` is then filled. Reads only.
std::optional<VideoScalerRefusal> DeriveVideoScalerStub(const VideoScalerMemory& memory, const VideoScalerCall& call, const VideoScalerState& state, VideoScalerPlan& plan);

enum class VideoScalerApply : std::uint8_t
{
    Done,
    Unstarted, // the first store failed: nothing changed
    Failed,    // a later store failed: the output is partial
};

// Stores the 200 output dwords in order.
VideoScalerApply ApplyVideoScalerStub(const VideoScalerMemory& memory, const VideoScalerPlan& plan) noexcept;

// The cvar native_video_scaler_stub while ring stub services are published.
bool VideoScalerStubRequested() noexcept;
// The cvar native_video_scaler_exit_check.
bool VideoScalerExitCheckRequested() noexcept;
// Either of the above: the hooks do any work only then.
bool VideoScalerActive() noexcept;
// Either cvar, whether or not the services are published: the timing and shutdown drains.
bool VideoScalerStubReportingRequested() noexcept;

// True when the import is replaced: r3 is then 200 and the hook jumps past the call.
bool ReplaceVideoScalerCall(const VideoScalerCall& call, std::uint64_t& r3) noexcept;
bool ReplaceVideoScalerCallWith(const VideoScalerOperations& operations, const VideoScalerCall& call, std::uint64_t& r3) noexcept;
// The return before rlwinm (r3 is the count) and the end of the cursor store.
void ObserveVideoScalerReturn(std::uint32_t sp, std::uint64_t r3, std::uint32_t r30, std::uint32_t r31) noexcept;
void ObserveVideoScalerCopy(std::uint32_t sp, std::uint32_t r30, std::uint32_t r31) noexcept;

struct VideoScalerStubCounts
{
    std::uint64_t                                        enter = 0, consumed = 0, original = 0, writeFailed = 0;
    std::array<std::uint64_t, kVideoScalerRefusalCount>  refusal{};
    std::uint64_t                                        exitOk = 0, exitUnreadable = 0, exitUnpaired = 0;
    std::array<std::uint64_t, kVideoScalerMismatchCount> mismatch{};
    std::uint64_t                                        pending      = 0; // started calls whose observations have not finished
    std::uint64_t                                        finalSkipped = 0;
    bool                                                 final        = false;
};

// Returns the counts since the previous take and resets them. `pending` is the
// number of calls still outstanding and is not reset.
VideoScalerStubCounts TakeVideoScalerStubCounts() noexcept;

std::string FormatVideoScalerStubCounts(const VideoScalerStubCounts& counts);

struct VideoScalerReportOperations
{
    std::string (*format)(const VideoScalerStubCounts&) = FormatVideoScalerStubCounts;
    void (*log)(std::string_view)                       = nullptr;
};

// The fields of the native_frame_timing line, or the final log at shutdown. The
// final drain waits at most kVideoScalerShutdownWait for a concurrent drain and
// logs a skipped final with its outstanding calls. Empty with both cvars off.
std::string ReportVideoScalerStub(bool shutdown = false, const VideoScalerReportOperations& operations = {}) noexcept;

namespace detail
{

std::timed_mutex& VideoScalerReportMutex() noexcept;

} // namespace detail

// Test seams: operations replacing the production adapter (null restores it),
// the per-thread bookkeeping touch count and the outstanding-record bound.
void          SetVideoScalerOperationsForTesting(const VideoScalerOperations* operations) noexcept;
std::uint64_t VideoScalerThreadTouchesForTesting() noexcept;
void          SetVideoScalerPendingLimitForTesting(std::uint32_t limit) noexcept;

} // namespace rerevved::gpu::guest_d3d
