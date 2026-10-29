#include "display_aspect.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>

#include "gpu/renderer_backend.h"

REXCVAR_DEFINE_STRING(display_aspect, "original", "ReRevved", "Ultrawide presentation: original keeps 16:9, auto matches the window or display_aspect_custom, 21:9 and 32:9 fix the shape")
    .allowed({ "original", "auto", "21:9", "32:9" });
REXCVAR_DEFINE_STRING(display_aspect_custom, "", "ReRevved", "Optional aspect for display_aspect=auto as numerator:denominator, for example 21:9 or 3440:1440; empty matches the window");
REXCVAR_DEFINE_BOOL(display_aspect_debug_scissor, true, "ReRevved", "Diagnostic: false skips ultrawide scissor compression while retaining viewport or affine correction")
    .audience(rex::cvar::Audience::kHidden);
REXCVAR_DEFINE_BOOL(display_aspect_probe, false, "ReRevved", "Bounded census of the ultrawide camera, interface and swap paths; observes only and applies no correction unless display_aspect enables it");

namespace rerevved::display_aspect
{
namespace
{

constexpr double kHalfWidth = kGuestWidth / 2.0;

bool isSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

std::optional<uint32_t> parseU32(std::string_view text) noexcept
{
    if (text.empty())
        return std::nullopt;
    uint64_t value = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
            return std::nullopt;
        value = value * 10 + static_cast<uint64_t>(c - '0');
        if (value > UINT32_MAX)
            return std::nullopt;
    }
    return static_cast<uint32_t>(value);
}

Aspect reduce(uint32_t x, uint32_t y) noexcept
{
    const uint32_t divisor = std::gcd(x, y);
    return divisor ? Aspect{ x / divisor, y / divisor } : Aspect{ x, y };
}

bool finite(float value) noexcept
{
    return std::isfinite(value);
}

bool validScale(float k) noexcept
{
    return finite(k) && k > 0.0f && k <= 1.0f;
}

uint32_t loadBe32(const uint8_t* bytes) noexcept
{
    return (uint32_t{ bytes[0] } << 24) | (uint32_t{ bytes[1] } << 16) | (uint32_t{ bytes[2] } << 8) | uint32_t{ bytes[3] };
}

void storeBe32(uint8_t* bytes, uint32_t value) noexcept
{
    bytes[0] = static_cast<uint8_t>(value >> 24);
    bytes[1] = static_cast<uint8_t>(value >> 16);
    bytes[2] = static_cast<uint8_t>(value >> 8);
    bytes[3] = static_cast<uint8_t>(value);
}

float loadBeFloat(const uint8_t* bytes) noexcept
{
    const uint32_t bits = loadBe32(bytes);
    float          value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void storeBeFloat(uint8_t* bytes, float value) noexcept
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    storeBe32(bytes, bits);
}

constexpr std::array<const char*, 22> kReasonTokens{
    "disabled", "native_route", "invalid_aspect", "narrow_aspect", "surface_unknown", "state_unknown", "frontend", "attract", "movie", "movie_unknown", "unclassified", "early_ui", "camera_unknown", "camera_ortho", "camera_unreadable", "viewport_invalid", "offscreen_unconfirmed", "fit_conflict", "transport_unavailable", "world", "late_movie", "late_scene"
};

} // namespace

std::string_view ReasonToken(Reason reason) noexcept
{
    const auto index = static_cast<size_t>(reason);
    return index < kReasonTokens.size() ? kReasonTokens[index] : "unknown";
}

std::optional<Aspect> ParseAspect(std::string_view text) noexcept
{
    text               = trim(text);
    const size_t colon = text.find(':');
    if (colon == std::string_view::npos || text.find(':', colon + 1) != std::string_view::npos)
        return std::nullopt;
    const auto numerator   = parseU32(text.substr(0, colon));
    const auto denominator = parseU32(text.substr(colon + 1));
    if (!numerator || !denominator || *numerator == 0 || *denominator == 0)
        return std::nullopt;
    return reduce(*numerator, *denominator);
}

Mode ParseMode(std::string_view text) noexcept
{
    if (text == "auto")
        return Mode::Auto;
    if (text == "custom" || !FixedPresetAspect(text).empty())
        return Mode::Custom;
    return Mode::Original;
}

std::string_view FixedPresetAspect(std::string_view text) noexcept
{
    if (text == "21:9" || text == "32:9")
        return text;
    return {};
}

std::string_view ModeToken(Mode mode) noexcept
{
    switch (mode)
    {
        case Mode::Auto:
            return "auto";
        case Mode::Custom:
            return "custom";
        default:
            return "original";
    }
}

bool IsWiderThanOriginal(Aspect aspect) noexcept
{
    return uint64_t{ aspect.x } * kOriginalAspect.y > uint64_t{ kOriginalAspect.x } * aspect.y;
}

float HorizontalScale(Aspect aspect) noexcept
{
    if (aspect.x == 0 || aspect.y == 0)
        return 1.0f;
    return static_cast<float>((double{ kOriginalAspect.x } * aspect.y) / (double{ kOriginalAspect.y } * aspect.x));
}

Request ResolveRequestedAspect(Mode mode, std::string_view custom, Surface surface) noexcept
{
    Request request;
    Aspect  aspect = kOriginalAspect;
    switch (mode)
    {
        case Mode::Original:
            request.reason = Reason::Disabled;
            return request;
        case Mode::Auto:
            if (surface.width == 0 || surface.height == 0)
            {
                request.reason = Reason::SurfaceUnknown;
                return request;
            }
            aspect = reduce(surface.width, surface.height);
            break;
        case Mode::Custom:
        {
            const auto parsed = ParseAspect(custom);
            if (!parsed)
            {
                request.reason = Reason::InvalidAspect;
                return request;
            }
            aspect = *parsed;
            break;
        }
    }
    if (!IsWiderThanOriginal(aspect))
    {
        request.reason = Reason::NarrowAspect;
        return request;
    }
    request.aspect = aspect;
    request.k      = HorizontalScale(aspect);
    request.wide   = true;
    request.reason = Reason::World;
    return request;
}

FrustumStatus ValidateFrustum(const Frustum& f) noexcept
{
    if (!finite(f.left) || !finite(f.right) || !finite(f.top) || !finite(f.bottom) || !finite(f.nearPlane) || !finite(f.farPlane))
        return FrustumStatus::NonFinite;
    if (!(f.right > f.left) || !(f.top > f.bottom))
        return FrustumStatus::BadBounds;
    if (!(f.nearPlane > 0.0f) || !(f.farPlane > f.nearPlane))
        return FrustumStatus::BadDepth;
    return FrustumStatus::Ok;
}

FrustumResult ScaleFrustumHorizontal(const Frustum& frustum, float k) noexcept
{
    FrustumResult result;
    result.frustum = frustum;
    if (!validScale(k))
    {
        result.status = FrustumStatus::InvalidScale;
        return result;
    }
    result.status = ValidateFrustum(frustum);
    if (result.status != FrustumStatus::Ok)
        return result;

    const double scale   = k;
    const double middle  = (double{ frustum.left } + double{ frustum.right }) / 2.0;
    const double half    = (double{ frustum.right } - double{ frustum.left }) / 2.0;
    result.frustum.left  = static_cast<float>(middle - half / scale);
    result.frustum.right = static_cast<float>(middle + half / scale);
    return result;
}

bool IsSixteenByNineFrustum(const Frustum& f) noexcept
{
    if (ValidateFrustum(f) != FrustumStatus::Ok || !(f.right > 0.0f) || !(f.top > 0.0f))
        return false;
    const double width  = double{ f.right } - double{ f.left };
    const double height = double{ f.top } - double{ f.bottom };
    if (std::abs(double{ f.left } + double{ f.right }) > 0.01 * width || std::abs(double{ f.top } + double{ f.bottom }) > 0.01 * height)
        return false;
    const double expected = double{ kOriginalAspect.x } / kOriginalAspect.y;
    return std::abs(double{ f.right } / double{ f.top } / expected - 1.0) <= 0.01;
}

bool DecodeFrustum(const FrustumBytes& bytes, Frustum& frustum, bool& orthographic) noexcept
{
    frustum.left      = loadBeFloat(bytes.data() + 0);
    frustum.right     = loadBeFloat(bytes.data() + 4);
    frustum.top       = loadBeFloat(bytes.data() + 8);
    frustum.bottom    = loadBeFloat(bytes.data() + 12);
    frustum.nearPlane = loadBeFloat(bytes.data() + 16);
    frustum.farPlane  = loadBeFloat(bytes.data() + 20);
    orthographic      = bytes[24] != 0;
    return true;
}

FrustumStatus CorrectFrustumBytes(const FrustumBytes& original, float k, FrustumBytes& corrected) noexcept
{
    corrected = original;
    Frustum frustum;
    bool    orthographic = false;
    DecodeFrustum(original, frustum, orthographic);
    if (orthographic)
        return FrustumStatus::Orthographic;
    const FrustumResult result = ScaleFrustumHorizontal(frustum, k);
    if (result.status != FrustumStatus::Ok)
        return result.status;
    storeBeFloat(corrected.data() + 0, result.frustum.left);
    storeBeFloat(corrected.data() + 4, result.frustum.right);
    return FrustumStatus::Ok;
}

AffineResult CorrectGfxAffine(const Affine& affine, float viewportX, float viewportWidth, float k) noexcept
{
    AffineResult result;
    result.affine = affine;
    if (!validScale(k))
    {
        result.status = AffineStatus::InvalidScale;
        return result;
    }
    if (!finite(affine.a) || !finite(affine.b) || !finite(affine.tx))
    {
        result.status = AffineStatus::NonFinite;
        return result;
    }
    if (!finite(viewportX) || !finite(viewportWidth) || !(viewportWidth > 0.0f) || viewportX < 0.0f ||
        double{ viewportX } + double{ viewportWidth } > kGuestWidth)
    {
        result.status = AffineStatus::BadViewport;
        return result;
    }

    const double scale  = k;
    const double width  = viewportWidth;
    const double offset = 2.0 * (1.0 - scale) * (kHalfWidth - double{ viewportX } - width / 2.0) / width;
    result.affine.a     = static_cast<float>(scale * affine.a);
    result.affine.b     = static_cast<float>(scale * affine.b);
    result.affine.tx    = static_cast<float>(scale * affine.tx + offset);
    return result;
}

Rect CorrectScissor(const Rect& rect, float k) noexcept
{
    if (!validScale(k) || rect.right <= rect.left)
        return rect;
    const double scale = k;
    const double left  = std::floor(kHalfWidth + scale * (static_cast<double>(rect.left) - kHalfWidth));
    const double right = std::ceil(kHalfWidth + scale * (static_cast<double>(rect.right) - kHalfWidth));
    Rect         out   = rect;
    out.left           = static_cast<int32_t>(std::clamp(left, 0.0, double{ kGuestWidth }));
    out.right          = static_cast<int32_t>(std::clamp(right, 0.0, double{ kGuestWidth }));
    if (out.right <= out.left)
        return rect;
    return out;
}

bool CompressViewportSpan(int32_t x, int32_t width, float k, ViewportSpan& out) noexcept
{
    if (!validScale(k) || width <= 0 || x < 0 || int64_t{ x } + width > kGuestWidth)
        return false;
    const double scale = k;
    const double left  = std::floor(kHalfWidth + scale * (static_cast<double>(x) - kHalfWidth));
    const double right = std::ceil(kHalfWidth + scale * (static_cast<double>(x) + width - kHalfWidth));
    const double lo    = std::clamp(left, 0.0, double{ kGuestWidth });
    const double hi    = std::clamp(right, 0.0, double{ kGuestWidth });
    if (!(hi > lo))
        return false;
    out.x     = static_cast<int32_t>(lo);
    out.width = static_cast<int32_t>(hi - lo);
    return true;
}

Rect CorrectScissorInViewport(const Rect& request, const ViewportSpan& compressed, float k) noexcept
{
    const Rect scaled = CorrectScissor(request, k);
    Rect       out    = request;
    out.left          = std::max(compressed.x, scaled.left);
    out.right         = std::min(compressed.x + compressed.width, scaled.right);
    return out;
}

PolicyDecision DecideFramePolicy(const PolicyInputs& in) noexcept
{
    PolicyDecision decision;
    const auto     original = [&](Reason reason)
    {
        decision.wide   = false;
        decision.aspect = kOriginalAspect;
        decision.k      = 1.0f;
        decision.reason = reason;
        return decision;
    };

    if (!in.request.wide)
        return original(in.request.reason == Reason::World ? Reason::Disabled : in.request.reason);
    if (!in.xenos)
        return original(Reason::NativeRoute);
    if (in.earlyMovie)
        return original(Reason::Movie);
    if (in.earlyUi)
        return original(Reason::EarlyUi);
    if (!in.transportAvailable)
        return original(Reason::TransportUnavailable);
    if (!in.fitOk)
        return original(Reason::FitConflict);
    if (!in.keyKnown)
        return original(Reason::StateUnknown);
    if (in.key == kAttractKey)
        return original(Reason::Attract);
    if (in.key != kGameplayKey)
        return original(Reason::Frontend);
    if (in.movie == MovieState::Active)
        return original(Reason::Movie);
    if (in.movie == MovieState::Unknown)
        return original(Reason::MovieUnknown);
    if (in.unclassified)
        return original(Reason::Unclassified);
    if (in.lateInhibit != Reason::World)
        return original(in.lateInhibit);
    if (!in.cameraAccepted)
        return original(in.cameraReason);

    decision.wide   = true;
    decision.aspect = in.request.aspect;
    decision.k      = in.request.k;
    decision.reason = Reason::World;
    return decision;
}

bool LineBudget::Peek(int64_t nowNs, uint32_t limit) const noexcept
{
    if (!started || nowNs - windowStartNs >= 1000000000LL || nowNs < windowStartNs)
        return limit > 0;
    return count < limit;
}

bool LineBudget::Allow(int64_t nowNs, uint32_t limit) noexcept
{
    if (!started || nowNs - windowStartNs >= 1000000000LL || nowNs < windowStartNs)
    {
        started       = true;
        windowStartNs = nowNs;
        count         = 0;
    }
    if (count >= limit)
        return false;
    ++count;
    return true;
}

bool FrameEmission::Decide(uint64_t interval, bool changedOrAnomalous) noexcept
{
    if (lines >= kLineBudget)
        return false;
    if (changedOrAnomalous)
        detailedUntil = std::max(detailedUntil, interval + kDetailedIntervals);
    if (interval < detailedUntil || interval % kSampleEvery == 0)
    {
        ++lines;
        return true;
    }
    return false;
}

FamilyStats* FamilyTable::Find(const FamilyKey& key, bool& created) noexcept
{
    created = false;
    for (uint32_t index = 0; index < count; ++index)
    {
        if (families[index].key == key)
            return &families[index];
    }
    if (count >= kCapacity)
    {
        ++overflow;
        return nullptr;
    }
    families[count].key = key;
    created             = true;
    return &families[count++];
}

uint32_t FamilyTable::IndexOf(const FamilyStats* family) const noexcept
{
    return static_cast<uint32_t>(family - families.data());
}

bool CameraVtableAccepted(uint32_t vtable) noexcept
{
    return vtable == kCameraVtable || vtable == kWorldCameraVtable || vtable == kWorldCameraVtableB;
}

bool CameraCallerAccepted(uint32_t frameCaller) noexcept
{
    return frameCaller == kWorldFrameCaller;
}

bool CameraFamilyAccepted(uint32_t frameCaller, uint32_t hook, uint32_t cameraVtable) noexcept
{
    if (!CameraCallerAccepted(frameCaller) || !CameraVtableAccepted(cameraVtable))
        return false;
    switch (hook)
    {
        case 0x8234CF18:
        case 0x8234DF90:
        case 0x823528D8:
        case 0x8250BA00:
        case 0x82C71A00:
        case 0x82C49D08:
            return true;
        default:
            return false;
    }
}

MovieState DeriveMovieState(bool keyKnown, uint32_t key, bool movieSeenNow, bool movieSeenPrevious) noexcept
{
    if (!keyKnown || key != kGameplayKey)
        return MovieState::Unknown;
    return movieSeenNow || movieSeenPrevious ? MovieState::Active : MovieState::Inactive;
}

float HypotheticalScale(Mode mode, std::string_view custom, Surface surface) noexcept
{
    if (mode == Mode::Auto)
    {
        const Request request = ResolveRequestedAspect(Mode::Auto, custom, surface);
        if (request.wide)
            return request.k;
    }
    const auto parsed = ParseAspect(custom);
    if (parsed && IsWiderThanOriginal(*parsed))
        return HorizontalScale(*parsed);
    return HorizontalScale(Aspect{ 21, 9 });
}

bool GfxScopeAccepted(uint32_t beginCaller, uint32_t frameCaller) noexcept
{
    // The same begin caller also runs outside the world frame; only the
    // in-frame scope is final output.
    return beginCaller == kHudBeginCaller && CameraCallerAccepted(frameCaller);
}

void RequestLedger::Record(uint64_t sequence, uint64_t interval, uint32_t x, uint32_t y) noexcept
{
    Entry& slot = entries[next];
    if (slot.valid)
        evictedThrough = std::max(evictedThrough, slot.sequence);
    slot = Entry{ true, sequence, interval, x, y };
    next = (next + 1) % kCapacity;
}

RequestLedger::Match RequestLedger::Classify(uint64_t sequence, uint32_t x, uint32_t y, bool overrideValid, uint64_t& interval) const noexcept
{
    interval = 0;
    for (const Entry& entry : entries)
    {
        if (entry.valid && entry.sequence == sequence)
        {
            interval = entry.interval;
            return entry.x == x && entry.y == y && overrideValid == (entry.x != 0) ? Match::Ok : Match::Mismatch;
        }
    }
    return sequence != 0 && sequence <= evictedThrough ? Match::Evicted : Match::Mismatch;
}

//------------------------------------------------------------------ runtime

namespace detail
{

std::atomic<bool> active{ false };
ChangeGate        changeGate;

} // namespace detail

namespace
{

constexpr size_t   kSwapCapacity         = 120;
constexpr size_t   kBaselineCapacity     = 16;
constexpr uint32_t kCameraLinesPerSecond = 4;
constexpr uint32_t kTotalLinesPerSecond  = 64;

struct Counters
{
    uint32_t cameraHits     = 0;
    uint32_t cameraApplied  = 0;
    uint32_t gfxHits        = 0;
    uint32_t gfxApplied     = 0;
    uint32_t scissorHits    = 0;
    uint32_t scissorApplied = 0;
    uint32_t movieHits      = 0;
    uint32_t earlyGfx       = 0;
    uint32_t lateMovie      = 0;
};

struct Baseline
{
    bool         valid  = false;
    bool         setter = false;
    uint32_t     camera = 0;
    FrustumBytes bytes{};
};

struct Settings
{
    Mode        mode   = Mode::Original;
    std::string custom = "21:9";
    bool        probe  = false;
};

struct State
{
    std::mutex  mutex;
    bool        initialized = false;
    bool        shutdown    = false;
    std::string titleRevision;
    std::string sdkRevision;

    uint64_t   interval = 0;
    FrameView  view;
    Counters   counters;
    bool       keyKnown    = false;
    uint32_t   key         = 0;
    MovieState movie       = MovieState::Unknown;
    uint32_t   movieOwner  = 0;
    uint32_t   movieHandle = 0;
    bool       anomaly     = false;
    // A movie hook ran in the previous interval.
    bool   movieSeenPrevious = false;
    Reason lateInhibit       = Reason::World;

    // One settings snapshot is frozen per swap interval.
    Settings    snap;
    bool        snapValid = false;
    Mode        lastMode  = Mode::Original;
    std::string lastCustom;
    bool        publishedNonClear = false;

    bool          transportProbed    = false;
    bool          transportAvailable = true;
    uint64_t      acceptedCalls      = 0;
    RequestLedger ledger;

    rex::system::IGraphicsSystem* observed   = nullptr;
    bool                          fitApplied = false;

    uint64_t totalCameraHits = 0;
    uint64_t totalGfxHits    = 0;
    // Gear trace: the interval of the last traced frame and what was written.
    bool     gearHaveLast      = false;
    uint64_t gearLast          = 0;
    uint64_t gearFrames        = 0;
    uint64_t gearLines         = 0;
    uint64_t gearDropped       = 0;
    uint64_t refused           = 0;
    uint64_t lateMovie         = 0;
    uint64_t restoreMismatch   = 0;
    uint64_t transportMismatch = 0;
    uint64_t droppedRecords    = 0;
    bool     haveLast          = false;
    bool     lastWide          = false;
    Reason   lastReason        = Reason::Disabled;
    uint64_t framesSeen        = 0;

    bool                                    startLogged = false;
    FamilyTable                             families;
    FrameEmission                           emission;
    LineBudget                              cameraBudget;
    LineBudget                              totalBudget;
    std::array<Baseline, kBaselineCapacity> baselines{};
    size_t                                  baselineNext = 0;
};

State& state()
{
    static State instance;
    return instance;
}

struct SwapRing
{
    std::mutex                                                     mutex;
    std::array<rex::system::SwapPresentationReport, kSwapCapacity> records{};
    size_t                                                         count   = 0;
    uint64_t                                                       dropped = 0;
};

SwapRing& swapRing()
{
    static SwapRing instance;
    return instance;
}

std::mutex        gScaleMutex;
CameraScaleTable  gScales;
std::atomic<bool> gAnyWide{ false };

int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

rex::system::IGraphicsSystem* graphics()
{
    auto* runtime = rex::Runtime::instance();
    return runtime ? runtime->graphics_system() : nullptr;
}

constexpr size_t kLogQueueCapacity = 512;
constexpr size_t kLogLineMax       = 1024;

// Probe lines reach a worker through a bounded queue so a slow log sink never
// stalls a guest hook; a full queue drops the line and the caller counts it.
class LogWorker
{
public:
    ~LogWorker()
    {
        Stop(false);
    }

    bool Push(const char* text, size_t length)
    {
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || count_ >= kLogQueueCapacity)
                return false;
            if (!thread_.joinable())
                thread_ = std::thread([this]
                                      {
                                          Run();
                                      });
            Line& line  = lines_[(head_ + count_) % kLogQueueCapacity];
            line.length = static_cast<uint16_t>(length);
            std::memcpy(line.text.data(), text, length);
            ++count_;
        }
        wake_.notify_one();
        return true;
    }

    void Restart()
    {
        std::lock_guard lock(mutex_);
        if (!thread_.joinable())
        {
            stopping_ = false;
            discard_  = false;
        }
    }

    void Stop(bool drain)
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            discard_  = !drain;
        }
        wake_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

private:
    struct Line
    {
        uint16_t                      length = 0;
        std::array<char, kLogLineMax> text{};
    };

    void Run()
    {
        std::string      text;
        std::unique_lock lock(mutex_);
        for (;;)
        {
            wake_.wait(lock, [this]
                       {
                           return count_ > 0 || stopping_;
                       });
            while (count_ > 0)
            {
                const Line& line = lines_[head_];
                text.assign(line.text.data(), line.length);
                head_ = (head_ + 1) % kLogQueueCapacity;
                --count_;
                const bool discard = discard_;
                lock.unlock();
                if (!discard)
                    REXLOG_INFO("{}", text);
                lock.lock();
            }
            if (stopping_)
                return;
        }
    }

    std::mutex                          mutex_;
    std::condition_variable             wake_;
    std::thread                         thread_;
    std::array<Line, kLogQueueCapacity> lines_{};
    size_t                              head_     = 0;
    size_t                              count_    = 0;
    bool                                stopping_ = false;
    bool                                discard_  = false;
};

LogWorker& logWorker()
{
    static LogWorker worker;
    return worker;
}

template <typename... Args>
void emit(State& s, fmt::format_string<Args...> format, Args&&... args)
{
    std::array<char, kLogLineMax> buffer;
    const auto                    result = fmt::format_to_n(buffer.data(), buffer.size() - 1, format, std::forward<Args>(args)...);
    const size_t                  length = std::min<size_t>(result.size, buffer.size() - 1);
    if (!logWorker().Push(buffer.data(), length))
        ++s.droppedRecords;
}

std::atomic<Mode> gMode{ Mode::Original };
std::atomic<bool> gProbe{ false };

Settings readSettings()
{
    Settings   settings;
    const auto text = rex::cvar::Query<std::string>("display_aspect");
    settings.mode   = ParseMode(text);
    settings.custom = rex::cvar::Query<std::string>("display_aspect_custom");
    settings.probe  = rex::cvar::Query<bool>("display_aspect_probe");
    // A fixed preset overrides the config-only custom value.
    if (const auto preset = FixedPresetAspect(text); !preset.empty())
        settings.custom = preset;
    // The config-only override turns Ultrawide into a fixed aspect.
    else if (settings.mode == Mode::Auto && !settings.custom.empty())
        settings.mode = Mode::Custom;
    return settings;
}

bool presentFitOk()
{
    return rex::cvar::Query<bool>("present_letterbox") && rex::cvar::Query<int32_t>("present_safe_area_x") == 100 &&
           rex::cvar::Query<int32_t>("present_safe_area_y") == 100;
}

void applyFit(bool wide)
{
    if (wide)
    {
        rex::cvar::SetFlagAsApplicationDefault("present_letterbox", "true");
        rex::cvar::SetFlagAsApplicationDefault("present_safe_area_x", "100");
        rex::cvar::SetFlagAsApplicationDefault("present_safe_area_y", "100");
        // Ultrawide only: 16:9 menus, movies and loading screens get blurred bars.
        rex::cvar::SetFlagAsApplicationDefault("present_bar_fill", "blur");
    }
    else
    {
        rex::cvar::SetFlagAsApplicationDefault("present_safe_area_x", "90");
        rex::cvar::SetFlagAsApplicationDefault("present_safe_area_y", "90");
        rex::cvar::SetFlagAsApplicationDefault("present_bar_fill", "black");
    }
}

void recomputeActive()
{
    detail::changeGate.Publish(detail::active, gMode.load() != Mode::Original || gProbe.load() || gAnyWide.load());
}

void onSettingChanged(std::string_view, std::string_view)
{
    // Called from any thread. The guest thread takes a new snapshot at the next
    // interval boundary; until then hooks simply run.
    detail::changeGate.Notify(detail::active);
}

void swapObserver(void*, const rex::system::SwapPresentationReport& report)
{
    SwapRing&       ring = swapRing();
    std::lock_guard lock(ring.mutex);
    if (ring.count >= kSwapCapacity)
    {
        ++ring.dropped;
        return;
    }
    ring.records[ring.count++] = report;
}

// Keeps the swap observer registered exactly while the probe is on, so a
// disabled probe leaves no callback behind.
void syncObserver(State& s, rex::system::IGraphicsSystem* gs)
{
    if (s.observed && s.observed != gs)
    {
        if (s.observed == graphics())
            s.observed->SetSwapPresentationObserver(nullptr, nullptr);
        s.observed = nullptr;
    }
    if (s.snap.probe && gs && !s.observed)
    {
        gs->SetSwapPresentationObserver(&swapObserver, nullptr);
        s.observed = gs;
    }
    else if (!s.snap.probe && s.observed)
    {
        s.observed->SetSwapPresentationObserver(nullptr, nullptr);
        s.observed = nullptr;
    }
}

// Freezes one settings snapshot for the interval; every decision and probe
// record of the interval reads it, never the live values.
void ensureSnapshot(State& s)
{
    if (s.snapValid)
        return;
    // Consumed before the read: a change after this point stays pending.
    detail::changeGate.Consume();
    s.snap      = readSettings();
    s.snapValid = true;
    gMode.store(s.snap.mode);
    gProbe.store(s.snap.probe);
    if (s.snap.mode != s.lastMode || s.snap.custom != s.lastCustom)
    {
        // A settings change is the reclassification point that releases a
        // late-movie inhibit.
        s.lateInhibit = Reason::World;
        s.lastMode    = s.snap.mode;
        s.lastCustom  = s.snap.custom;
    }
    const bool wantFit = s.snap.mode != Mode::Original;
    if (wantFit != s.fitApplied)
    {
        applyFit(wantFit);
        s.fitApplied = wantFit;
    }
    syncObserver(s, graphics());
    recomputeActive();
}

bool allowLine(State& s, bool camera)
{
    const int64_t now = nowNs();
    if (camera && !s.cameraBudget.Peek(now, kCameraLinesPerSecond))
    {
        ++s.droppedRecords;
        return false;
    }
    if (!s.totalBudget.Peek(now, kTotalLinesPerSecond))
    {
        ++s.droppedRecords;
        return false;
    }
    if (camera)
        s.cameraBudget.Allow(now, kCameraLinesPerSecond);
    s.totalBudget.Allow(now, kTotalLinesPerSecond);
    return true;
}

Surface surfaceOf(rex::system::IGraphicsSystem* gs)
{
    uint32_t width  = 0;
    uint32_t height = 0;
    if (gs && !gs->GetDisplaySurfaceSize(width, height))
    {
        width  = 0;
        height = 0;
    }
    return Surface{ width, height };
}

Request currentRequest(const State& s, rex::system::IGraphicsSystem* gs)
{
    return ResolveRequestedAspect(s.snap.mode, s.snap.custom, surfaceOf(gs));
}

void logStart(State& s, const Request& request)
{
    if (s.startLogged)
        return;
    s.startLogged = true;
    emit(s,
         "DisplayAspectProbe: start version=1 title={} sdk={} mode={} requested={}:{} family_cap={} frame_budget={}",
         s.titleRevision,
         s.sdkRevision,
         ModeToken(s.snap.mode),
         request.aspect.x,
         request.aspect.y,
         static_cast<uint32_t>(FamilyTable::kCapacity),
         static_cast<uint32_t>(FrameEmission::kLineBudget));
}

void recordIssued(State& s, uint64_t interval, uint32_t x, uint32_t y)
{
    s.ledger.Record(s.acceptedCalls, interval, x, y);
}

// First call the SDK API once so an unsupported backend is known before the
// first wide decision. The explicit clear is also the normal original request.
void probeTransport(State& s, rex::system::IGraphicsSystem* gs)
{
    if (s.transportProbed || !gs)
        return;
    s.transportProbed = true;
    if (gs->SetPresentationAspect(0, 0))
    {
        ++s.acceptedCalls;
        recordIssued(s, s.interval, 0, 0);
        s.transportAvailable = true;
    }
    else
    {
        s.transportAvailable = false;
    }
}

const char* moviePrint(MovieState movie)
{
    switch (movie)
    {
        case MovieState::Active:
            return "active";
        case MovieState::Inactive:
            return "inactive";
        default:
            return "unknown";
    }
}

void logFrame(State& s, const FrameView& v, bool changed)
{
    if (!s.snap.probe)
        return;
    if (!s.emission.Decide(s.interval, changed))
        return;
    if (!allowLine(s, false))
        return;
    const Counters&   c   = s.counters;
    const std::string key = s.keyKnown ? std::to_string(s.key) : std::string("unknown");
    emit(s,
         "DisplayAspectFrame: frame={} policy={} reason={} key={} movie={} movie_owner={:08X} movie_handle={:08X} aspect={}:{} k={:.9g} cam={:08X} camera_hits={} camera_applied={} gfx_hits={} gfx_applied={} scissor_hits={} scissor_applied={} movie_hits={} early_gfx={} late_movie={} sequence={}",
         s.interval,
         v.wide ? "wide" : "original",
         ReasonToken(v.reason),
         key,
         moviePrint(s.movie),
         s.movieOwner,
         s.movieHandle,
         v.aspect.x,
         v.aspect.y,
         v.k,
         v.mainCamera,
         c.cameraHits,
         c.cameraApplied,
         c.gfxHits,
         c.gfxApplied,
         c.scissorHits,
         c.scissorApplied,
         c.movieHits,
         c.earlyGfx,
         c.lateMovie,
         s.acceptedCalls);
}

void flushSwaps(State& s, Mode mode)
{
    std::array<rex::system::SwapPresentationReport, kSwapCapacity> local;
    size_t                                                         count       = 0;
    uint64_t                                                       ringDropped = 0;
    {
        SwapRing&       ring = swapRing();
        std::lock_guard lock(ring.mutex);
        count = ring.count;
        std::copy_n(ring.records.begin(), count, local.begin());
        ring.count   = 0;
        ringDropped  = ring.dropped;
        ring.dropped = 0;
    }
    s.droppedRecords += ringDropped;
    for (size_t index = 0; index < count; ++index)
    {
        const auto& report    = local[index];
        const auto& request   = report.request;
        uint64_t    interval  = 0;
        const char* transport = "ok";
        if (!s.transportAvailable && mode != Mode::Original)
        {
            transport = "unsupported";
        }
        else if (request.policy_sequence == 0)
        {
            transport = mode == Mode::Original ? "ok" : "mismatch";
        }
        else
        {
            const auto match = s.ledger.Classify(request.policy_sequence, request.aspect_x, request.aspect_y, request.override_valid, interval);
            if (match == RequestLedger::Match::Evicted)
            {
                // The request left the ledger before the GPU reported it, so
                // nothing can be said about it; this is not a mismatch.
                ++s.droppedRecords;
                continue;
            }
            if (match == RequestLedger::Match::Mismatch)
                transport = "mismatch";
        }
        if (std::strcmp(transport, "mismatch") == 0)
            ++s.transportMismatch;

        if (!allowLine(s, false))
            continue;
        const char*       refresh  = report.refresh == rex::system::SwapRefreshResult::kOk
                                         ? "ok"
                                         : (report.refresh == rex::system::SwapRefreshResult::kFailed ? "failed" : "skipped");
        const std::string content  = report.content_known ? std::to_string(report.content_id) : std::string("unknown");
        const char*       repeated = report.content_known ? (report.repeated ? "1" : "0") : "unknown";
        emit(s,
             "DisplayAspectSwap: frame={} sequence={} source={:08X} content={} aspect={}:{} repeated={} transport={} refresh={}",
             interval,
             request.policy_sequence,
             report.frontbuffer_ptr,
             content,
             report.display_x,
             report.display_y,
             repeated,
             transport,
             refresh);
    }
}

void resetInterval(State& s)
{
    s.movieSeenPrevious = s.counters.movieHits != 0;
    ++s.interval;
    s.view        = FrameView{};
    s.counters    = Counters{};
    s.keyKnown    = false;
    s.key         = 0;
    s.movie       = MovieState::Unknown;
    s.movieOwner  = 0;
    s.movieHandle = 0;
    s.anomaly     = false;
}

void freezeOriginal(State& s, Reason reason)
{
    FrameView& v = s.view;
    v.latched    = true;
    v.wide       = false;
    v.aspect     = kOriginalAspect;
    v.k          = 1.0f;
    v.reason     = reason;
    v.interval   = s.interval;
}

} // namespace

bool ProbeEnabled() noexcept
{
    return gProbe.load(std::memory_order_relaxed);
}

bool ScissorCorrectionEnabled() noexcept
{
    return REXCVAR_GET(display_aspect_debug_scissor);
}

bool GearTraceFrame(uint64_t interval)
{
    if (!gProbe.load(std::memory_order_relaxed))
        return false;
    State&          s = state();
    std::lock_guard lock(s.mutex);
    if (s.shutdown || s.gearLines >= kGearTraceLineBudget)
        return false;
    if (s.gearHaveLast && interval >= s.gearLast && interval - s.gearLast < kGearTraceEvery)
        return false;
    s.gearHaveLast = true;
    s.gearLast     = interval;
    ++s.gearFrames;
    return true;
}

bool RecordGearLine(std::string_view line)
{
    if (!gProbe.load(std::memory_order_relaxed))
        return false;
    State&          s = state();
    std::lock_guard lock(s.mutex);
    if (s.shutdown || s.gearLines >= kGearTraceLineBudget)
        return false;
    if (!logWorker().Push(line.data(), std::min<size_t>(line.size(), kLogLineMax - 1)))
    {
        ++s.gearDropped;
        return false;
    }
    ++s.gearLines;
    return true;
}

float ProbeScale()
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    ensureSnapshot(s);
    return HypotheticalScale(s.snap.mode, s.snap.custom, surfaceOf(graphics()));
}

bool CameraScaleTable::Tracked(uint32_t camera) const noexcept
{
    for (size_t index = 0; index < count; ++index)
    {
        if (entries[index].camera == camera)
            return true;
    }
    return false;
}

bool CameraScaleTable::Untracked() const noexcept
{
    return untracked;
}

bool CameraScaleTable::AnyWide() const noexcept
{
    return count > 0 || untracked;
}

bool CameraScaleTable::NeedsRebuild(uint32_t camera, float k, uint64_t now) noexcept
{
    for (size_t index = 0; index < count; ++index)
    {
        if (entries[index].camera == camera)
        {
            entries[index].lastSeen = now;
            return entries[index].scale != k;
        }
    }
    return untracked || k != 1.0f;
}

bool CameraScaleTable::RetireAged(uint64_t now) noexcept
{
    bool retired = false;
    for (size_t index = 0; index < count;)
    {
        if (now > entries[index].lastSeen && now - entries[index].lastSeen > kMaxAge)
        {
            entries[index] = entries[--count];
            retired        = true;
        }
        else
        {
            ++index;
        }
    }
    // A retired camera may still hold a wide projection in the renderer cache.
    if (retired)
        untracked = true;
    return retired;
}

bool CameraScaleTable::NoteBuilt(uint32_t camera, float k, uint64_t now) noexcept
{
    const bool before = AnyWide();
    size_t     found  = count;
    for (size_t index = 0; index < count; ++index)
    {
        if (entries[index].camera == camera)
            found = index;
    }
    if (k == 1.0f)
    {
        if (found < count)
            entries[found] = entries[--count];
        return before != AnyWide();
    }
    sweeps = 0;
    if (found < count)
    {
        entries[found].scale    = k;
        entries[found].lastSeen = now;
        return before != AnyWide();
    }
    if (count >= kCapacity)
        RetireAged(now);
    if (count < kCapacity)
        entries[count++] = Entry{ camera, k, now };
    else
        untracked = true;
    return before != AnyWide();
}

bool CameraScaleTable::NoteOriginalJoin(uint64_t now) noexcept
{
    if (!untracked)
        return false;
    if (now != sweepInterval)
    {
        sweepInterval = now;
        ++sweeps;
    }
    if (sweeps < kSweepsNeeded)
        return false;
    const bool before = AnyWide();
    untracked         = false;
    sweeps            = 0;
    sweepInterval     = UINT64_MAX;
    return before != AnyWide();
}

bool CameraTracked(uint32_t camera) noexcept
{
    if (!gAnyWide.load(std::memory_order_relaxed))
        return false;
    std::lock_guard lock(gScaleMutex);
    return gScales.Tracked(camera);
}

bool UntrackedWideBuilds() noexcept
{
    if (!gAnyWide.load(std::memory_order_relaxed))
        return false;
    std::lock_guard lock(gScaleMutex);
    return gScales.Untracked();
}

bool CacheNeedsRebuild(uint32_t camera, float k, uint64_t now) noexcept
{
    std::lock_guard lock(gScaleMutex);
    return gScales.NeedsRebuild(camera, k, now);
}

void NoteCacheBuilt(uint32_t camera, float k, uint64_t now) noexcept
{
    bool changed = false;
    {
        std::lock_guard lock(gScaleMutex);
        changed = gScales.NoteBuilt(camera, k, now);
        gAnyWide.store(gScales.AnyWide(), std::memory_order_relaxed);
    }
    if (changed)
        recomputeActive();
}

void NoteOriginalJoin(uint64_t now) noexcept
{
    bool changed = false;
    {
        std::lock_guard lock(gScaleMutex);
        changed = gScales.NoteOriginalJoin(now);
        gAnyWide.store(gScales.AnyWide(), std::memory_order_relaxed);
    }
    if (changed)
        recomputeActive();
}

void Initialize(std::string_view titleRevision, std::string_view sdkRevision)
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    if (s.initialized)
        return;
    s.initialized   = true;
    s.shutdown      = false;
    s.titleRevision = std::string(titleRevision);
    s.sdkRevision   = std::string(sdkRevision);
    s.snap          = readSettings();
    s.snapValid     = false;
    s.lastMode      = s.snap.mode;
    s.lastCustom    = s.snap.custom;
    gMode.store(s.snap.mode);
    gProbe.store(s.snap.probe);
    logWorker().Restart();
    if (s.snap.mode != Mode::Original)
    {
        applyFit(true);
        s.fitApplied = true;
    }
    rex::cvar::RegisterChangeCallback("display_aspect", &onSettingChanged);
    rex::cvar::RegisterChangeCallback("display_aspect_custom", &onSettingChanged);
    rex::cvar::RegisterChangeCallback("display_aspect_probe", &onSettingChanged);
    recomputeActive();
}

void Shutdown()
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized || s.shutdown)
        return;
    s.shutdown = true;
    detail::active.store(false, std::memory_order_relaxed);
    rex::cvar::UnregisterChangeCallbacks("display_aspect");
    rex::cvar::UnregisterChangeCallbacks("display_aspect_custom");
    rex::cvar::UnregisterChangeCallbacks("display_aspect_probe");
    if (s.observed)
    {
        if (s.observed == graphics())
            s.observed->SetSwapPresentationObserver(nullptr, nullptr);
        s.observed = nullptr;
    }
    if (s.startLogged)
    {
        emit(s, "DisplayAspectProbe: summary frames={} families={} camera_hits={} gfx_hits={} refused={} late_movie={} restore_mismatch={} transport_mismatch={} dropped_records={} family_overflow={}", s.framesSeen, s.families.Count(), s.totalCameraHits, s.totalGfxHits, s.refused, s.lateMovie, s.restoreMismatch, s.transportMismatch, s.droppedRecords, s.families.Overflow());
    }
    if (s.gearFrames != 0)
        emit(s, "DisplayAspectGear: summary frames={} lines={} dropped={}", s.gearFrames, s.gearLines, s.gearDropped);
    logWorker().Stop(true);
    // Write families synchronously after draining the queue so none are dropped.
    if (s.startLogged)
    {
        for (uint32_t index = 0; index < s.families.Count(); ++index)
        {
            const FamilyStats& family = s.families.At(index);
            REXLOG_INFO("DisplayAspectFamily: family={} hook={:08X} frame_return={:08X} owner_vtable={:08X} vtable={:08X} ortho={} stage={}..{} viewport_class={} frames_seen={} hits={}", index, family.key.hook, family.key.frameReturn, family.key.ownerVtable, family.key.cameraVtable, family.key.orthographic ? 1 : 0, family.key.stageFirst, family.key.stageLast, family.key.viewportClass, family.framesSeen, family.hits);
        }
    }
}

FrameView CurrentFrame()
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    return s.view;
}

FrameView LatchFrame(const LatchInputs& inputs)
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    if (s.shutdown)
        return FrameView{};
    if (s.view.latched)
        return s.view;

    ensureSnapshot(s);
    auto*      gs   = graphics();
    const Mode mode = s.snap.mode;
    if (mode != Mode::Original)
        probeTransport(s, gs);

    PolicyInputs in;
    in.request            = currentRequest(s, gs);
    in.xenos              = gpu::SelectedRendererBackend() != gpu::RendererBackend::Native;
    in.transportAvailable = s.transportAvailable && gs != nullptr;
    in.fitOk              = presentFitOk();
    in.keyKnown           = inputs.keyKnown;
    in.key                = inputs.key;
    // No early inactive discriminator exists: a frame is movie-free when no movie
    // hook ran in this or the previous interval. The late-movie inhibit backstops.
    in.movie = DeriveMovieState(inputs.keyKnown, inputs.key, s.counters.movieHits != 0, s.movieSeenPrevious);
    if (s.movie == MovieState::Unknown)
        s.movie = in.movie;
    in.lateInhibit = s.lateInhibit;
    if (!inputs.cameraReadable)
    {
        in.cameraReason = Reason::CameraUnreadable;
    }
    else if (!CameraVtableAccepted(inputs.cameraVtable) || inputs.incomingCamera == 0)
    {
        in.cameraReason = Reason::CameraUnknown;
    }
    else if (inputs.orthographic)
    {
        in.cameraReason = Reason::CameraOrtho;
    }
    else
    {
        in.cameraAccepted = CameraCallerAccepted(inputs.frameCaller) && IsSixteenByNineFrustum(inputs.frustum);
        in.cameraReason   = Reason::CameraUnknown;
    }

    const PolicyDecision decision = DecideFramePolicy(in);
    FrameView&           v        = s.view;
    v.latched                     = true;
    v.wide                        = decision.wide;
    v.aspect                      = decision.aspect;
    v.k                           = decision.k;
    v.reason                      = decision.reason;
    v.interval                    = s.interval;
    v.mainCamera                  = inputs.incomingCamera;
    v.frameOwner                  = inputs.frameOwner;
    v.frameCaller                 = inputs.frameCaller;
    s.keyKnown                    = inputs.keyKnown;
    s.key                         = inputs.key;
    if (s.snap.probe)
        logStart(s, in.request);
    return v;
}

FrameView NoteEarlyEvent(EarlyEvent event)
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    if (s.shutdown)
        return FrameView{};
    ensureSnapshot(s);
    if (!s.view.latched)
    {
        freezeOriginal(s, event == EarlyEvent::Ui ? Reason::EarlyUi : Reason::Movie);
        if (event == EarlyEvent::Ui)
            ++s.counters.earlyGfx;
        s.anomaly = true;
    }
    else if (event == EarlyEvent::Movie && s.view.wide)
    {
        ++s.counters.lateMovie;
        ++s.lateMovie;
        s.lateInhibit = Reason::LateMovie;
        s.anomaly     = true;
    }
    return s.view;
}

void NoteMovie(uint32_t owner, uint32_t handle, bool handleReadable)
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    ++s.counters.movieHits;
    s.movie       = MovieState::Active;
    s.movieOwner  = owner;
    s.movieHandle = handleReadable ? handle : 0;
}

void CountCameraBuild(bool applied)
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    ++s.counters.cameraHits;
    ++s.totalCameraHits;
    if (applied)
        ++s.counters.cameraApplied;
}

void CountGfx(bool applied)
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    ++s.counters.gfxHits;
    ++s.totalGfxHits;
    if (applied)
        ++s.counters.gfxApplied;
}

void CountScissor(bool applied)
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    ++s.counters.scissorHits;
    if (applied)
        ++s.counters.scissorApplied;
}

void CountRefusal()
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    ++s.refused;
    s.anomaly = true;
}

void CountRestoreMismatch()
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    ++s.restoreMismatch;
    s.anomaly = true;
}

void NoteSetter(uint32_t camera)
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    for (Baseline& entry : s.baselines)
    {
        if (entry.valid && entry.camera == camera)
            entry.setter = true;
    }
}

bool BaselineChangedWithoutSetter(uint32_t camera, const FrustumBytes& original)
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    for (Baseline& entry : s.baselines)
    {
        if (!entry.valid || entry.camera != camera)
            continue;
        const bool changed = entry.bytes != original && !entry.setter;
        entry.bytes        = original;
        entry.setter       = false;
        if (changed)
        {
            ++s.restoreMismatch;
            s.anomaly = true;
        }
        return changed;
    }
    Baseline& slot = s.baselines[s.baselineNext];
    s.baselineNext = (s.baselineNext + 1) % kBaselineCapacity;
    slot           = Baseline{ true, false, camera, original };
    return false;
}

void RecordCamera(const CameraObservation& o)
{
    if (!gProbe.load(std::memory_order_relaxed))
        return;
    State&          s = state();
    std::lock_guard lock(s.mutex);
    if (s.shutdown)
        return;

    uint32_t viewportClass = 0;
    if (o.viewportValid)
    {
        // The camera's four viewport words are logged as read. The class only
        // separates unit-range values from pixel-range values.
        bool unit = true;
        for (const float value : o.viewport)
            unit = unit && value >= 0.0f && value <= 1.0f;
        viewportClass = unit ? 1 : 2;
    }
    FamilyKey key;
    key.hook          = o.hook;
    key.frameReturn   = o.frameReturn;
    key.ownerVtable   = o.ownerVtable;
    key.cameraVtable  = o.cameraVtable;
    key.orthographic  = o.orthographic;
    key.stageFirst    = o.stageFirst;
    key.stageLast     = o.stageLast;
    key.viewportClass = viewportClass;

    bool         created = false;
    FamilyStats* family  = s.families.Find(key, created);
    if (!family)
    {
        ++s.droppedRecords;
        return;
    }
    if (family->lastInterval != s.interval)
    {
        family->lastInterval = s.interval;
        ++family->framesSeen;
    }
    ++family->hits;

    // The first hits and every power of two after them are detailed.
    const bool detailed = created || family->hits <= 3 || (family->hits & (family->hits - 1)) == 0 || o.applied || o.restored == 0;
    if (!detailed || !allowLine(s, true))
        return;
    ++family->linesEmitted;

    std::string stage    = o.stageFirst == UINT32_MAX ? std::string("none") : fmt::format("{}:{}", o.stageFirst, o.stageLast);
    const char* restored = o.restored < 0 ? "na" : (o.restored == 0 ? "0" : "1");
    emit(s,
         "DisplayAspectCamera: frame={} family={} hook={:08X} cam={:08X} vtable={:08X} owner={:08X} owner_vtable={:08X} return={:08X} frame_return={:08X} stage={} ortho={} viewport=[{:.9g},{:.9g},{:.9g},{:.9g}] original=[{:.9g},{:.9g},{:.9g},{:.9g}] corrected=[{:.9g},{:.9g},{:.9g},{:.9g}] applied={} restored={} frames_seen={} hits={} reason={}",
         s.interval,
         s.families.IndexOf(family),
         o.hook,
         o.camera,
         o.cameraVtable,
         o.owner,
         o.ownerVtable,
         o.returnAddress,
         o.frameReturn,
         stage,
         o.orthographic ? 1 : 0,
         o.viewport[0],
         o.viewport[1],
         o.viewport[2],
         o.viewport[3],
         o.original[0],
         o.original[1],
         o.original[2],
         o.original[3],
         o.corrected[0],
         o.corrected[1],
         o.corrected[2],
         o.corrected[3],
         o.applied ? 1 : 0,
         restored,
         family->framesSeen,
         family->hits,
         ReasonToken(o.reason));
}

void FinishInterval(const SwapInputs& inputs)
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    if (s.shutdown || !s.initialized)
        return;

    ensureSnapshot(s);
    auto*      gs   = graphics();
    const Mode mode = s.snap.mode;

    FrameView v = s.view;
    if (!v.latched)
    {
        freezeOriginal(s, Reason::Unclassified);
        v = s.view;
    }
    if (!s.keyKnown && inputs.keyKnown)
    {
        s.keyKnown = true;
        s.key      = inputs.key;
    }

    if (mode != Mode::Original || s.publishedNonClear)
    {
        const uint32_t x = v.wide ? v.aspect.x : 0;
        const uint32_t y = v.wide ? v.aspect.y : 0;
        if (gs && gs->SetPresentationAspect(x, y))
        {
            ++s.acceptedCalls;
            recordIssued(s, s.interval, x, y);
            s.transportAvailable = true;
            s.publishedNonClear  = mode != Mode::Original;
        }
        else
        {
            s.transportAvailable = false;
            s.publishedNonClear  = false;
            if (v.wide)
            {
                ++s.transportMismatch;
                s.anomaly = true;
            }
        }
    }

    if (s.snap.probe)
    {
        logStart(s, currentRequest(s, gs));
        const bool changed = !s.haveLast || s.lastWide != v.wide || s.lastReason != v.reason || s.anomaly;
        logFrame(s, v, changed);
        flushSwaps(s, mode);
    }
    s.haveLast   = true;
    s.lastWide   = v.wide;
    s.lastReason = v.reason;
    ++s.framesSeen;
    resetInterval(s);
    s.snapValid = false;
}

void ResetForTesting()
{
    State&          s = state();
    std::lock_guard lock(s.mutex);
    s.initialized = false;
    s.shutdown    = false;
    s.view        = FrameView{};
    s.counters    = Counters{};
    s.interval    = 0;
    s.lateInhibit = Reason::World;
    s.lastMode    = Mode::Original;
    s.lastCustom.clear();
    s.movie             = MovieState::Unknown;
    s.movieSeenPrevious = false;
    s.snap              = Settings{};
    s.snapValid         = false;
    s.ledger            = RequestLedger{};
    s.acceptedCalls     = 0;
    gMode.store(Mode::Original);
    gProbe.store(false);
    logWorker().Stop(false);
    s.gearHaveLast       = false;
    s.gearLast           = 0;
    s.gearFrames         = 0;
    s.gearLines          = 0;
    s.gearDropped        = 0;
    s.fitApplied         = false;
    s.transportProbed    = false;
    s.transportAvailable = true;
    s.startLogged        = false;
    s.baselines          = {};
    s.baselineNext       = 0;
    s.families           = FamilyTable{};
    s.emission           = FrameEmission{};
    {
        std::lock_guard scaleLock(gScaleMutex);
        gScales = CameraScaleTable{};
    }
    gAnyWide.store(false);
    detail::active.store(false);
    detail::changeGate.Reset();
}

} // namespace rerevved::display_aspect
