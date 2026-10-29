#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// Ultrawide keeps the 1280x720 guest buffer, widens eligible world cameras and
// centers the interface in 16:9; unclassified frames stay 16:9.
namespace rerevved::display_aspect
{

inline constexpr uint32_t kCameraVtable = 0x8200AA28;
// Camera vtables of the 16:9 perspective cameras seen inside the world frame
// in the probe census of the 1.3 image.
inline constexpr uint32_t kWorldCameraVtable  = 0x82123738;
inline constexpr uint32_t kWorldCameraVtableB = 0x8216A3F8;
// Return address of the call into the world frame entry.
inline constexpr uint32_t kWorldFrameCaller = 0x82C7E108;
// Caller of the GFx display begin for the in-frame interface.
inline constexpr uint32_t kHudBeginCaller = 0x8222E3F8;
inline constexpr uint32_t kGuestWidth     = 1280;
inline constexpr uint32_t kGuestHeight    = 720;

struct Aspect
{
    uint32_t x = 16;
    uint32_t y = 9;

    bool operator==(const Aspect&) const = default;
};

inline constexpr Aspect kOriginalAspect{ 16, 9 };

enum class Mode : uint8_t
{
    Original,
    Auto,
    Custom,
};

enum class Reason : uint8_t
{
    Disabled,
    NativeRoute,
    InvalidAspect,
    NarrowAspect,
    SurfaceUnknown,
    StateUnknown,
    Frontend,
    Attract,
    Movie,
    MovieUnknown,
    Unclassified,
    EarlyUi,
    CameraUnknown,
    CameraOrtho,
    CameraUnreadable,
    ViewportInvalid,
    OffscreenUnconfirmed,
    FitConflict,
    TransportUnavailable,
    World,
    LateMovie,
    LateScene,
};

std::string_view ReasonToken(Reason reason) noexcept;

// Strict "numerator:denominator" of two unsigned decimal integers around one
// colon, ASCII-trimmed, zero and 32-bit overflow rejected, reduced by the GCD.
std::optional<Aspect> ParseAspect(std::string_view text) noexcept;
// "21:9" and "32:9" select Mode::Custom with that aspect; unknown text is
// Mode::Original.
Mode ParseMode(std::string_view text) noexcept;
// The fixed aspect a display_aspect preset value names, or empty.
std::string_view FixedPresetAspect(std::string_view text) noexcept;
std::string_view ModeToken(Mode mode) noexcept;

// True when a is wider than 16:9, by integer cross-product.
bool IsWiderThanOriginal(Aspect aspect) noexcept;

struct Surface
{
    uint32_t width  = 0;
    uint32_t height = 0;
};

// The aspect the settings ask for. k scales the horizontal projection row:
// 16:9 divided by the target aspect, 1 for original.
struct Request
{
    Aspect aspect = kOriginalAspect;
    float  k      = 1.0f;
    bool   wide   = false;
    // Why the request is original; World when it is wide.
    Reason reason = Reason::Disabled;
};

float   HorizontalScale(Aspect aspect) noexcept;
Request ResolveRequestedAspect(Mode mode, std::string_view custom, Surface surface) noexcept;

// Scale for a hypothetical record while no wide request is in force: the auto
// surface when wide, else a wide custom value in any mode, else 21:9.
float HypotheticalScale(Mode mode, std::string_view custom, Surface surface) noexcept;

struct Frustum
{
    float left      = 0;
    float right     = 0;
    float top       = 0;
    float bottom    = 0;
    float nearPlane = 0;
    float farPlane  = 0;
};

enum class FrustumStatus : uint8_t
{
    Ok,
    InvalidScale,
    NonFinite,
    BadBounds,
    BadDepth,
    Orthographic,
};

struct FrustumResult
{
    FrustumStatus status = FrustumStatus::Ok;
    Frustum       frustum;
};

FrustumStatus ValidateFrustum(const Frustum& frustum) noexcept;

// True for a valid, symmetric, about-16:9 frustum (|right / top| within 1% of
// 16/9; left = -right, top = -bottom within 1% of the span). Square render-to-
// texture and screen-space frustums fail.
bool IsSixteenByNineFrustum(const Frustum& frustum) noexcept;

// Keeps the horizontal center and widens the half-span by 1/k; every other
// field is unchanged. Computed from the given values only, never accumulated.
FrustumResult ScaleFrustumHorizontal(const Frustum& frustum, float k) noexcept;

// Guest camera frustum words at camera+0xF0: left, right, top, bottom, near,
// far and the orthographic word, each big endian.
inline constexpr size_t kFrustumBytes = 28;
using FrustumBytes                    = std::array<uint8_t, kFrustumBytes>;

bool DecodeFrustum(const FrustumBytes& bytes, Frustum& frustum, bool& orthographic) noexcept;
// Writes the horizontally scaled frustum into a copy of original; only the
// left and right words differ from it.
FrustumStatus CorrectFrustumBytes(const FrustumBytes& original, float k, FrustumBytes& corrected) noexcept;

// The first three floats of the composed GFx matrix: the horizontal row.
struct Affine
{
    float a  = 0;
    float b  = 0;
    float tx = 0;
};

enum class AffineStatus : uint8_t
{
    Ok,
    InvalidScale,
    NonFinite,
    BadViewport,
};

struct AffineResult
{
    AffineStatus status = AffineStatus::Ok;
    Affine       affine;
};

// Compresses the row by k about the guest center and re-centers a partial
// viewport at guest pixel (viewportX, viewportWidth).
AffineResult CorrectGfxAffine(const Affine& affine, float viewportX, float viewportWidth, float k) noexcept;

struct Rect
{
    int32_t left   = 0;
    int32_t top    = 0;
    int32_t right  = 0;
    int32_t bottom = 0;

    bool operator==(const Rect&) const = default;
};

// Horizontal scissor correction about the guest center, rounded outward and
// clamped to the guest output; an empty rectangle stays unchanged.
Rect CorrectScissor(const Rect& rect, float k) noexcept;

// A hardware viewport span in guest pixels.
struct ViewportSpan
{
    int32_t x     = 0;
    int32_t width = 0;

    bool operator==(const ViewportSpan&) const = default;
};

// Compresses a viewport span like CorrectScissor. False (out untouched) for an
// invalid scale or a span that is empty or leaves the guest output.
bool CompressViewportSpan(int32_t x, int32_t width, float k, ViewportSpan& out) noexcept;

// The scissor the native viewport-and-scissor intersection gives after
// CorrectScissor on an already compressed viewport: the compressed request
// clipped to the compressed span, with top and bottom unchanged.
Rect CorrectScissorInViewport(const Rect& request, const ViewportSpan& compressed, float k) noexcept;

enum class MovieState : uint8_t
{
    Unknown,
    Inactive,
    Active,
};

// Gameplay frames with no movie hook in this or the previous interval are
// Inactive, otherwise Active; any other or unreadable state key is Unknown.
MovieState DeriveMovieState(bool keyKnown, uint32_t key, bool movieSeenNow, bool movieSeenPrevious) noexcept;

struct PolicyInputs
{
    Request request;
    bool    xenos              = true;
    bool    transportAvailable = true;
    bool    fitOk              = true;
    // A GFx or movie hook ran before the interval's frame entry.
    bool       earlyUi      = false;
    bool       earlyMovie   = false;
    bool       keyKnown     = false;
    uint32_t   key          = 0;
    MovieState movie        = MovieState::Unknown;
    bool       unclassified = false;
    // A late movie or scene blocks widening until the aspect setting changes.
    Reason lateInhibit    = Reason::World;
    bool   cameraAccepted = false;
    Reason cameraReason   = Reason::CameraUnknown;
};

struct PolicyDecision
{
    bool   wide   = false;
    Aspect aspect = kOriginalAspect;
    float  k      = 1.0f;
    Reason reason = Reason::Disabled;
};

// Wide only when every gate holds; otherwise original with the first failing
// gate's reason.
PolicyDecision DecideFramePolicy(const PolicyInputs& inputs) noexcept;

// State key used to identify gameplay.
inline constexpr uint32_t kGameplayKey = 2;
inline constexpr uint32_t kAttractKey  = 5;

// Fixed-window output limiter: at most `limit` lines per second of the supplied
// monotonic clock.
class LineBudget
{
public:
    bool Allow(int64_t nowNs, uint32_t limit) noexcept;
    // Whether Allow would pass, without consuming.
    bool Peek(int64_t nowNs, uint32_t limit) const noexcept;

private:
    int64_t  windowStartNs = 0;
    uint32_t count         = 0;
    bool     started       = false;
};

// Decides which swap intervals get a frame record: the first 120 and 120 after
// a policy change or anomaly, within a line budget, otherwise one in 60.
class FrameEmission
{
public:
    static constexpr uint64_t kDetailedIntervals = 120;
    static constexpr uint64_t kSampleEvery       = 60;
    static constexpr uint32_t kLineBudget        = 2048;

    bool Decide(uint64_t interval, bool changedOrAnomalous) noexcept;

    uint32_t Lines() const noexcept
    {
        return lines;
    }

private:
    uint64_t detailedUntil = kDetailedIntervals;
    uint32_t lines         = 0;
};

// Bounded set of camera families; a full table counts overflow and never grows.
struct FamilyKey
{
    uint32_t hook          = 0;
    uint32_t frameReturn   = 0;
    uint32_t ownerVtable   = 0;
    uint32_t cameraVtable  = 0;
    bool     orthographic  = false;
    uint32_t stageFirst    = UINT32_MAX;
    uint32_t stageLast     = UINT32_MAX;
    uint32_t viewportClass = 0;

    bool operator==(const FamilyKey&) const = default;
};

struct FamilyStats
{
    FamilyKey key;
    uint64_t  framesSeen   = 0;
    uint64_t  hits         = 0;
    uint64_t  lastInterval = UINT64_MAX;
    uint32_t  linesEmitted = 0;
};

class FamilyTable
{
public:
    static constexpr size_t kCapacity = 256;

    // Null when the key is new and the table is full.
    FamilyStats* Find(const FamilyKey& key, bool& created) noexcept;
    uint32_t     IndexOf(const FamilyStats* family) const noexcept;

    uint32_t Count() const noexcept
    {
        return count;
    }

    uint64_t Overflow() const noexcept
    {
        return overflow;
    }

    const FamilyStats& At(uint32_t index) const noexcept
    {
        return families[index];
    }

private:
    std::array<FamilyStats, kCapacity> families{};
    uint32_t                           count    = 0;
    uint64_t                           overflow = 0;
};

// The identities below come from the probe census of the 1.3 image.
bool CameraVtableAccepted(uint32_t vtable) noexcept;
// The frame caller whose world frame may be widened.
bool CameraCallerAccepted(uint32_t frameCaller) noexcept;
// Camera hooks that may be corrected, within an accepted frame caller and
// vtable. The NiCamera constructor reads uninitialized frustums; never accepted.
bool CameraFamilyAccepted(uint32_t frameCaller, uint32_t hook, uint32_t cameraVtable) noexcept;
// GFx display scopes accepted as final output: the in-frame interface only, by
// the caller of the begin hook and the enclosing frame caller.
bool GfxScopeAccepted(uint32_t beginCaller, uint32_t frameCaller) noexcept;

// Fixed-capacity stack; Push fails when full instead of overwriting.
template <typename T, size_t N>
class BoundedStack
{
public:
    bool Push(const T& item) noexcept
    {
        if (count >= N)
            return false;
        items[count++] = item;
        return true;
    }

    void Pop() noexcept
    {
        if (count)
            --count;
    }

    T* Top() noexcept
    {
        return count ? &items[count - 1] : nullptr;
    }

    size_t Count() const noexcept
    {
        return count;
    }

    const T& At(size_t index) const noexcept
    {
        return items[index];
    }

private:
    std::array<T, N> items{};
    size_t           count = 0;
};

// Correlates requests the title sent with those the GPU reports. The ledger
// outlasts the swap ring, so an evicted request is reported, not a mismatch.
class RequestLedger
{
public:
    static constexpr size_t kCapacity = 128;

    enum class Match : uint8_t
    {
        Ok,
        Mismatch,
        Evicted,
    };

    void  Record(uint64_t sequence, uint64_t interval, uint32_t x, uint32_t y) noexcept;
    Match Classify(uint64_t sequence, uint32_t x, uint32_t y, bool overrideValid, uint64_t& interval) const noexcept;

private:
    struct Entry
    {
        bool     valid    = false;
        uint64_t sequence = 0;
        uint64_t interval = 0;
        uint32_t x        = 0;
        uint32_t y        = 0;
    };

    std::array<Entry, kCapacity> entries{};
    size_t                       next           = 0;
    uint64_t                     evictedThrough = 0;
};

//------------------------------------------------------------------ runtime

// Remembers settings changes no snapshot has consumed. A change arriving while
// a snapshot is read keeps the feature active until the next one.
class ChangeGate
{
public:
    // Called from the change callback on any thread.
    void Notify(std::atomic<bool>& active) noexcept
    {
        dirty_.store(true, std::memory_order_release);
        active.store(true, std::memory_order_relaxed);
    }

    // Called before the settings are read; true when a change was pending.
    bool Consume() noexcept
    {
        return dirty_.exchange(false, std::memory_order_acq_rel);
    }

    // Stores the computed state without hiding a change that raced the store.
    void Publish(std::atomic<bool>& active, bool want) noexcept
    {
        active.store(want || dirty_.load(std::memory_order_acquire), std::memory_order_relaxed);
        if (dirty_.load(std::memory_order_acquire))
            active.store(true, std::memory_order_relaxed);
    }

    void Reset() noexcept
    {
        dirty_.store(false, std::memory_order_relaxed);
    }

private:
    std::atomic<bool> dirty_{ false };
};

namespace detail
{

extern std::atomic<bool> active;
extern ChangeGate        changeGate;

} // namespace detail

// True for non-original settings, pending settings changes, the probe or
// corrections that still need to be undone.
inline bool Active() noexcept
{
    return detail::active.load(std::memory_order_relaxed);
}

// Reads settings, registers callbacks and applies presentation fitting defaults.
// Revisions are retained verbatim for the probe log, which starts on demand.
void Initialize(std::string_view titleRevision, std::string_view sdkRevision);
// Writes the probe summary and releases the SDK observer. Idempotent.
void Shutdown();

// Frozen at the interval's first classification point.
struct FrameView
{
    bool     latched     = false;
    bool     wide        = false;
    Aspect   aspect      = kOriginalAspect;
    float    k           = 1.0f;
    uint64_t interval    = 0;
    uint32_t mainCamera  = 0;
    uint32_t frameOwner  = 0;
    uint32_t frameCaller = 0;
    Reason   reason      = Reason::Disabled;
};

struct LatchInputs
{
    uint32_t frameOwner     = 0;
    uint32_t incomingCamera = 0;
    // Return address of the call into the frame entry.
    uint32_t frameCaller    = 0;
    bool     keyKnown       = false;
    uint32_t key            = 0;
    bool     cameraReadable = false;
    uint32_t cameraVtable   = 0;
    bool     orthographic   = false;
    Frustum  frustum;
};

enum class EarlyEvent : uint8_t
{
    Ui,
    Movie,
};

FrameView CurrentFrame();
// Frame entry: freezes the interval's policy unless an earlier hook already
// froze it as original.
FrameView LatchFrame(const LatchInputs& inputs);
// A GFx or movie hook ran; freezes an unlatched interval as original.
FrameView NoteEarlyEvent(EarlyEvent event);
void      NoteMovie(uint32_t owner, uint32_t handle, bool handleReadable);

// Unlisted cameras use scale 1 unless overflow or retirement leaves a wide
// build untracked; candidates then rebuild until Original sweeps clear it (see
// NoteOriginalJoin). A table with no wide builds stays inert.
class CameraScaleTable
{
public:
    static constexpr size_t kCapacity = 64;
    // Intervals without a join or build after which an entry is retired.
    static constexpr uint64_t kMaxAge = 600;
    // Distinct Original intervals of cache-clearing joins that clear the state.
    static constexpr uint32_t kSweepsNeeded = 2;

    bool Tracked(uint32_t camera) const noexcept;
    bool Untracked() const noexcept;
    // Counts as a sighting of the camera for age purposes.
    bool NeedsRebuild(uint32_t camera, float k, uint64_t now) noexcept;
    // Returns true when AnyWide changed.
    bool NoteBuilt(uint32_t camera, float k, uint64_t now) noexcept;
    // An Original join cleared the renderer's cache regardless of its holder.
    // Returns true when AnyWide changed.
    bool NoteOriginalJoin(uint64_t now) noexcept;
    bool AnyWide() const noexcept;

private:
    struct Entry
    {
        uint32_t camera   = 0;
        float    scale    = 1.0f;
        uint64_t lastSeen = 0;
    };

    bool RetireAged(uint64_t now) noexcept;

    std::array<Entry, kCapacity> entries{};
    size_t                       count         = 0;
    bool                         untracked     = false;
    uint64_t                     sweepInterval = UINT64_MAX;
    uint32_t                     sweeps        = 0;
};

// Whether the projection cached for this camera was built with a scale other
// than wanted (1 for original); only tracked or wide-candidate cameras are asked.
bool CameraTracked(uint32_t camera) noexcept;
bool UntrackedWideBuilds() noexcept;
bool CacheNeedsRebuild(uint32_t camera, float k, uint64_t now) noexcept;
void NoteCacheBuilt(uint32_t camera, float k, uint64_t now) noexcept;
void NoteOriginalJoin(uint64_t now) noexcept;

void CountCameraBuild(bool applied);
void CountGfx(bool applied);
void CountScissor(bool applied);
void CountRefusal();
void CountRestoreMismatch();

bool ProbeEnabled() noexcept;

// display_aspect_debug_scissor: false skips the scissor compression only.
bool ScissorCorrectionEnabled() noexcept;

// Gear trace (probe only): bounded per-event log of GFx scope, affine, scissor,
// draw and mask decisions for sampled wide frames: one frame per kGearTraceEvery
// intervals, at most kGearTraceLineBudget lines per run.
inline constexpr uint64_t kGearTraceEvery      = 240;
inline constexpr uint64_t kGearTraceLineBudget = 9000;
// Whether this wide interval is traced (decided once per interval by the caller).
bool GearTraceFrame(uint64_t interval);
// Queues one line; false when the budget is spent or the queue is full.
bool RecordGearLine(std::string_view line);
// Aspect factor used to compute the hypothetical correction while the feature
// is off; the settings' wide aspect when there is one, otherwise 21:9.
float ProbeScale();

struct CameraObservation
{
    uint32_t             hook          = 0;
    uint32_t             camera        = 0;
    uint32_t             cameraVtable  = 0;
    uint32_t             owner         = 0;
    uint32_t             ownerVtable   = 0;
    uint32_t             returnAddress = 0;
    uint32_t             frameReturn   = 0;
    uint32_t             stageFirst    = UINT32_MAX;
    uint32_t             stageLast     = UINT32_MAX;
    bool                 orthographic  = false;
    bool                 viewportValid = false;
    std::array<float, 4> viewport{};
    std::array<float, 4> original{};
    std::array<float, 4> corrected{};
    bool                 applied = false;
    // -1 not applicable, 0 mismatch or not restored, 1 restored.
    int    restored = -1;
    Reason reason   = Reason::CameraUnknown;
};

void RecordCamera(const CameraObservation& observation);

// Camera pointers whose frustum a guest setter just wrote.
void NoteSetter(uint32_t camera);
// Returns true when the camera's original words changed since the last build
// without a setter in between; remembers the words either way.
bool BaselineChangedWithoutSetter(uint32_t camera, const FrustumBytes& original);

struct SwapInputs
{
    bool     keyKnown = false;
    uint32_t key      = 0;
};

// Ends the interval at the guest swap: publishes the frozen policy through the
// SDK, writes the frame record and starts the next interval.
void FinishInterval(const SwapInputs& inputs);

// Test seam: restores the state a fresh process has.
void ResetForTesting();

} // namespace rerevved::display_aspect
