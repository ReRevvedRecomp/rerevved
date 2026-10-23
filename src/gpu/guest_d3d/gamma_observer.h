#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Setter 0x8268FE08 converts three 256-entry big-endian halfword channels,
// left-aligned 10 bits(1536 bytes), at frame r1+80; hook 0x8268FE30 reads
// there. Emitter 0x826A7040 takes device r3/table r4 of same layout.
inline constexpr std::size_t kGuestGammaBytes = 1536;
// Setter only retains at device+15004/flags it; LUT changes when a later
// emitter returns. Before first emission it is linear power-on default.
inline constexpr std::uint32_t kGuestGammaRetainedOffset = 15004;
inline constexpr std::uint32_t kGuestGammaEmitterReturn  = 0x826A3EB0; // the call in 0x826A3E48; any other caller is a second site, such as 0x826A77F8 (returns to 0x826A7838)
inline constexpr std::size_t   kGuestGammaEntries        = 256;
inline constexpr std::size_t   kGuestGammaSetterLogs     = 8;
inline constexpr std::size_t   kMaxEmissionsInFlight     = 8;

// Entries packed as the renderer's gamma pass reads them: red in bits 20-29,
// green in 10-19 and blue in 0-9 (PackGammaEntry).
using GuestGammaTable = std::array<std::uint32_t, kGuestGammaEntries>;

enum class GammaDecode : std::uint8_t
{
    Ok,
    BadSize,   // not three complete 256-entry channels
    Unaligned, // a halfword has bits below the ten the table keeps
};

// Empty `table` on failure.
GammaDecode DecodeGuestGammaTable(std::span<const std::uint8_t> bytes, GuestGammaTable& table) noexcept;

// The display LUT before any emission: i * 0x3FF / 0xFF in every channel.
const GuestGammaTable& LinearGuestGammaTable() noexcept;

#if REREVVED_DEVELOPER
// The table as the little-endian bytes the guest frame capture saves, and the
// lowercase hex SHA-256 of those 1024 bytes.
std::string GuestGammaTableSha256(const GuestGammaTable& table);

// Entries of `a` that differ from `b`.
std::uint32_t CountGammaDifferences(const GuestGammaTable& a, const GuestGammaTable& b) noexcept;

// Window counters, taken and reset together; identityDiffEntries is a gauge.
struct GammaCounts
{
    std::uint64_t setter = 0, setterChanged = 0, setterDecodeFail = 0, pwlSetter = 0;
    std::uint64_t emit = 0, emitComplete = 0, emitDecodeFail = 0, pwlEmit = 0, emitEvicted = 0;
    std::uint64_t emitRetainedEqual = 0, emitRetainedMismatch = 0, emitOverlap = 0, emitSecondSite = 0;
    std::uint64_t deviceChange = 0;
    std::uint64_t swap = 0, swapBeforeEmit = 0, swapPendingSetter = 0, swapTableChanged = 0, noworkSwapChanged = 0;
    std::uint64_t errors              = 0;
    std::uint32_t identityDiffEntries = 0;
};
#endif

// What a swap saw of the display LUT.
struct GammaSwapView
{
    GuestGammaTable table{};                  // the table in effect: the last completed emission, else the linear default
    bool            assumed          = false; // the effect is not a table the observer could decode (a PWL or undecodable emission)
    std::uint64_t   emissionSequence = 0;
};

struct GammaSetterOutcome
{
    bool            decoded = false;
    bool            changed = false; // differs from the previous setter table of this device (the first one always)
    GuestGammaTable table{};
};

// One device: switching restarts setter/in-flight emissions, not completed GPU
// LUT. Last completion wins regardless of begin order. Oldest above
// kMaxEmissionsInFlight evicts; every method locks and reads no guest memory.
class GammaObserver
{
public:
    GammaSetterOutcome NoteSetter(std::uint32_t device, std::span<const std::uint8_t> converted);
    void               NotePwlSetter(std::uint32_t device);
    // `emitted` is the emitter's r4 table and `retained` the device copy at
    // device+15004, each empty when unreadable. `secondSite` marks a caller
    // other than 0x826A3E48. Returns the ticket EndEmission takes, or 0.
    std::uint64_t BeginEmission(std::uint32_t device, std::span<const std::uint8_t> emitted, std::span<const std::uint8_t> retained, bool secondSite);
    // A PWL emission, which leaves the LUT as something the observer cannot
    // name once it completes. Returns the ticket EndEmission takes, or 0.
    std::uint64_t BeginPwlEmission(std::uint32_t device);
    // Completes the emission of `ticket`. A ticket not in flight makes the LUT
    // unknown (see the class comment); zero or one not issued is also an error.
    void EndEmission(std::uint64_t ticket);

    // A swap's view of the LUT, counted against the previous swap and the
    // setter. `identity` is the table the swap would otherwise assume.
    GammaSwapView BeginSwap(const GuestGammaTable& identity);
    // After the swap: `noWork` for an Empty swap that presents nothing new.
    void EndSwap(const GammaSwapView& view, bool noWork);

#if REREVVED_DEVELOPER
    void        NoteError() noexcept;
    GammaCounts Take();
#endif

private:
    struct InFlight;
    void          adoptDevice(std::uint32_t device);
    std::uint64_t track(InFlight emission);

    std::mutex mutex_;
#if REREVVED_DEVELOPER
    GammaCounts counts_;
#endif
    // The tracked device and, in a developer build, its setter value.
    std::uint32_t device_ = 0;
#if REREVVED_DEVELOPER
    bool            setterSeen_ = false;
    GuestGammaTable setterTable_{};
#endif

    // The emissions in flight, oldest first, and the last completed one.
    struct InFlight
    {
        std::uint64_t   ticket  = 0;
        bool            pwl     = false;
        bool            decoded = false;
        GuestGammaTable table{};
    };

    std::vector<InFlight> inFlight_;
    // Only an emission that began after the last return of one the observer no
    // longer followed, which made the LUT unknown, can name it again.
    std::uint64_t staleThrough_ = 0;
    // Issued tickets increase monotonically.
    std::uint64_t   tickets_          = 0;
    std::uint64_t   emissionSequence_ = 0;
    bool            emissionKnown_    = true;
    GuestGammaTable emissionTable_{};
#if REREVVED_DEVELOPER
    // The previous swap.
    bool                       swapSeen_ = false;
    GuestGammaTable            swapTable_{};
    std::uint64_t              presentedEmission_ = 0;
    std::atomic<std::uint64_t> errors_{ 0 };
#endif
};

// What a swap hands its assembler.
struct GammaSwapChoice
{
    GuestGammaTable table{};
    bool            assumed = true;
};

// With native_gamma_apply (`apply`) and a view: the table in effect, assumed
// only when the observer could not decode it. Otherwise the identity table,
// assumed.
GammaSwapChoice ChooseSwapGamma(bool apply, const std::optional<GammaSwapView>& view, const GuestGammaTable& identity);

// Startup latches renderer=native before native_gamma_apply takes effect. Do
// not gate on ring services: their mid-session stop must not reset an applied
// LUT to identity.
void SetGammaNativeRenderer(bool native) noexcept;
// The cvar native_gamma_apply under the native renderer.
bool GammaApplyRequested() noexcept;
// The apply above, which implies it; a developer build also honors the cvar native_gamma_observe_check.
bool GammaObserveRequested() noexcept;

// The process observer the hooks feed.
GammaObserver& PublishedGammaObserver() noexcept;

// A failed observing Begin still reaches ObserveGammaEmissionEnd, leaving the
// LUT unknown.
void          ObserveGammaSetter(std::uint32_t device, std::uint32_t stack) noexcept;
void          ObserveGammaPwlSetter(std::uint32_t device) noexcept;
std::uint64_t ObserveGammaEmissionBegin(std::uint32_t device, std::uint32_t table, std::uint32_t caller) noexcept;
void          ObserveGammaEmissionEnd(std::uint64_t ticket) noexcept;
std::uint64_t ObserveGammaPwlEmissionBegin(std::uint32_t device) noexcept;

#if REREVVED_DEVELOPER
// The " gamma_obs_<name>=N" fields, all of them, in a fixed order.
std::string FormatGammaCounts(const GammaCounts& counts);
#endif

} // namespace rerevved::gpu::guest_d3d
