#include "display_aspect_hooks.h"

#include "display_aspect.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace da = rerevved::display_aspect;

REX_EXTERN(__imp__sub_82C71A00);
REX_EXTERN(__imp__sub_823528D8);
REX_EXTERN(__imp__sub_823AF9C8);
REX_EXTERN(__imp__sub_8250BA00);
REX_EXTERN(__imp__sub_82C71880);
REX_EXTERN(__imp__sub_82C5EC28);
REX_EXTERN(__imp__sub_823035D0);
REX_EXTERN(__imp__sub_8268F9C0);
REX_EXTERN(__imp__sub_82690138);
REX_EXTERN(__imp__sub_8234EB00);
REX_EXTERN(__imp__sub_8234CF18);
REX_EXTERN(__imp__sub_8234DF90);
REX_EXTERN(__imp__sub_8234D360);
REX_EXTERN(__imp__sub_82C5DF60);
REX_EXTERN(__imp__sub_8234D890);
REX_EXTERN(__imp__sub_82C49D08);

namespace
{

constexpr uint32_t kRendererGlobal = 0x82F8E270;
constexpr uint32_t kFrontendRoot   = 0x82FFD624;

constexpr uint32_t kCameraFrustum       = 0xF0;
constexpr uint32_t kCameraViewport      = 0x114;
constexpr uint32_t kRendererCacheCamera = 0xB0;
constexpr uint32_t kRendererG           = 0xB8;
constexpr uint32_t kGfxViewportX        = 0x3168;
constexpr uint32_t kGfxViewportWidth    = 0x3170;
constexpr uint32_t kGfxScissorState     = 12676;
constexpr uint32_t kGfxScissorEnable    = 11856;
// The viewport record sub_82302F60 builds on its stack for sub_826903A0: x, y,
// width, height as words, then the minimum and maximum depth floats.
constexpr uint32_t kGfxViewportRecord = 144;

constexpr uint32_t kHookFrame    = 0x82C71A00;
constexpr uint32_t kHookRenderer = 0x823528D8;
constexpr uint32_t kHookOwner    = 0x823AF9C8;
constexpr uint32_t kHookBuild    = 0x8250BA00;
constexpr uint32_t kHookGfxBegin = 0x82302F60;

//------------------------------------------------------------ guest memory

rex::memory::Memory* guestMemory()
{
    auto* runtime = rex::Runtime::instance();
    return runtime ? runtime->memory() : nullptr;
}

bool isGuestPointer(uint32_t address)
{
    return address >= 0x10000 && address < 0xFFFFF000;
}

bool accessible(uint32_t address, uint32_t extent, bool write)
{
    auto* memory = guestMemory();
    if (!memory || extent == 0 || address > UINT32_MAX - extent)
        return false;
    const uint32_t end = address + extent;
    if (!isGuestPointer(address) || !isGuestPointer(end - 1))
        return false;
    auto* heap = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(end - 1) != heap)
        return false;
    const auto access = static_cast<int>(heap->QueryRangeAccess(address, end - 1));
    const auto needed = static_cast<int>(write ? rex::memory::PageAccess::kReadWrite : rex::memory::PageAccess::kReadOnly);
    return (access & needed) == needed;
}

bool readBytes(uint32_t address, uint8_t* out, uint32_t length)
{
    if (!accessible(address, length, false))
        return false;
    std::memcpy(out, guestMemory()->TranslateVirtual<const uint8_t*>(address), length);
    return true;
}

bool writeBytes(uint32_t address, const uint8_t* in, uint32_t length)
{
    if (!accessible(address, length, true))
        return false;
    std::memcpy(guestMemory()->TranslateVirtual<uint8_t*>(address), in, length);
    return true;
}

bool readU32(uint32_t address, uint32_t& value)
{
    std::array<uint8_t, 4> bytes;
    if (!readBytes(address, bytes.data(), 4))
        return false;
    value = (uint32_t{ bytes[0] } << 24) | (uint32_t{ bytes[1] } << 16) | (uint32_t{ bytes[2] } << 8) | uint32_t{ bytes[3] };
    return true;
}

bool writeU32(uint32_t address, uint32_t value)
{
    const std::array<uint8_t, 4> bytes{ static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value) };
    return writeBytes(address, bytes.data(), 4);
}

bool readFloat(uint32_t address, float& value)
{
    uint32_t bits;
    if (!readU32(address, bits))
        return false;
    value = std::bit_cast<float>(bits);
    return true;
}

bool writeFloat(uint32_t address, float value)
{
    return writeU32(address, std::bit_cast<uint32_t>(value));
}

bool readKey(uint32_t& key)
{
    uint32_t root        = 0;
    uint32_t stateObject = 0;
    return readU32(kFrontendRoot, root) && readU32(root + 0x70, stateObject) && readU32(stateObject + 4, key);
}

//--------------------------------------------------------- per-thread scopes

constexpr size_t kScopeDepth = 8;

template <typename T>
using BoundedStack = da::BoundedStack<T, kScopeDepth>;

struct FrameScope
{
    uint32_t owner  = 0;
    uint32_t camera = 0;
    uint32_t caller = 0;
};

struct JoinScope
{
    uint32_t hook     = 0;
    uint32_t renderer = 0;
    uint32_t camera   = 0;
};

struct StageScope
{
    uint32_t first = 0;
    uint32_t last  = 0;
};

struct GfxScope
{
    uint32_t renderer       = 0;
    uint32_t g              = 0;
    uint32_t depth          = 0;
    bool     movie          = false;
    bool     accepted       = false;
    bool     scissorTouched = false;
    // The viewport is compressed in this scope, so draws take no affine
    // correction. vpOriginal (uncompressed) is restored on the begin stack after
    // the call and onto the device at scope end.
    bool                    viewportCompressed = false;
    bool                    viewportPatched    = false;
    uint32_t                viewportStack      = 0;
    std::array<uint32_t, 6> viewportOriginal{};
    // Gear trace only.
    uint32_t serial         = 0;
    uint32_t affineSeen     = 0;
    uint32_t scissorSeen    = 0;
    uint32_t scissorApplied = 0;
};

thread_local uint32_t                 tFrameDepth = 0;
thread_local FrameScope               tFrame;
thread_local BoundedStack<JoinScope>  tJoins;
thread_local BoundedStack<StageScope> tStages;
thread_local BoundedStack<GfxScope>   tGfx;
thread_local uint32_t                 tMovieDepth = 0;
thread_local BoundedStack<uint32_t>   tCorrected;
thread_local bool                     tScissorBypass = false;
thread_local uint32_t                 tGfxOverflow   = 0;

bool isCorrected(uint32_t camera)
{
    for (size_t index = 0; index < tCorrected.Count(); ++index)
    {
        if (tCorrected.At(index) == camera)
            return true;
    }
    return false;
}

//------------------------------------------------------------- gear trace

// Probe-only trace of one sampled wide frame per thread: scope, affine,
// scissor, draw and mask events share an ordinal so the order is recoverable.
constexpr uint32_t kGearFrameLines    = 240;
constexpr uint32_t kGearOutsideLines  = 40;
constexpr uint32_t kHookDrawIndexed   = 0x82303C38;
constexpr uint32_t kHookDrawLine      = 0x82303EA8;
constexpr uint32_t kHookDrawBitmap    = 0x82303F70;
constexpr uint32_t kHookMaskEnter     = 0x82304288;
constexpr uint32_t kHookMaskTest      = 0x82304440;
constexpr uint32_t kHookMaskExit      = 0x823044C0;
constexpr uint32_t kComposerCallerOff = 0x78;

struct GearTrace
{
    uint64_t    interval      = UINT64_MAX;
    bool        on            = false;
    uint32_t    ord           = 0;
    uint32_t    lines         = 0;
    uint32_t    dropped       = 0;
    uint32_t    serial        = 0;
    uint32_t    draws         = 0;
    uint32_t    masks         = 0;
    uint32_t    scissorIn     = 0;
    uint32_t    scissorOut    = 0;
    uint32_t    outsideLogged = 0;
    uint32_t    drawOrd       = 0;
    const char* drawKind      = "none";
    bool        stencil       = false;
};

thread_local GearTrace tGear;

void gearSummary()
{
    if (!tGear.on)
        return;
    char text[256];
    std::snprintf(text, sizeof(text), "DisplayAspectGear: frame=%llu ord=%u ev=frame_end draws=%u masks=%u scissor_in=%u scissor_out=%u lines=%u dropped=%u", static_cast<unsigned long long>(tGear.interval), tGear.ord, tGear.draws, tGear.masks, tGear.scissorIn, tGear.scissorOut, tGear.lines, tGear.dropped);
    da::RecordGearLine(text);
}

// True while the calling thread is inside a traced wide frame. The sampling
// decision is made once per interval.
bool gearOpen()
{
    if (!da::ProbeEnabled())
        return false;
    const da::FrameView view = da::CurrentFrame();
    if (view.interval != tGear.interval)
    {
        gearSummary();
        tGear          = GearTrace{};
        tGear.interval = view.interval;
        tGear.on       = view.latched && view.wide && da::GearTraceFrame(view.interval);
    }
    return tGear.on;
}

void gearLine(const char* event, const char* format, ...)
{
    ++tGear.ord;
    if (tGear.lines >= kGearFrameLines)
    {
        ++tGear.dropped;
        return;
    }
    char buffer[560];
    int  length = std::snprintf(buffer, sizeof(buffer), "DisplayAspectGear: frame=%llu ord=%u ev=%s ", static_cast<unsigned long long>(tGear.interval), tGear.ord, event);
    if (length < 0 || length >= static_cast<int>(sizeof(buffer)))
        return;
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer + length, sizeof(buffer) - static_cast<size_t>(length), format, args);
    va_end(args);
    if (da::RecordGearLine(buffer))
        ++tGear.lines;
    else
        ++tGear.dropped;
}

struct MaskState
{
    uint32_t active  = 0;
    uint32_t flag188 = 0;
    uint32_t flag189 = 0;
    uint32_t ref192  = 0;
};

MaskState readMaskState(uint32_t renderer)
{
    MaskState state;
    uint8_t   byte = 0;
    if (readBytes(renderer + 16, &byte, 1))
        state.active = byte;
    if (readBytes(renderer + 188, &byte, 1))
        state.flag188 = byte;
    if (readBytes(renderer + 189, &byte, 1))
        state.flag189 = byte;
    readU32(renderer + 192, state.ref192);
    return state;
}

uint32_t gearScopeSerial()
{
    const GfxScope* top = tGfx.Top();
    return top ? top->serial : 0;
}

const char* admitReason(uint32_t renderer, uint32_t g, bool matchG)
{
    const GfxScope* scope = tGfx.Top();
    if (!scope)
        return "no_scope";
    if (tGfxOverflow != 0)
        return "overflow";
    if (!scope->accepted)
        return "not_accepted";
    if (scope->depth != 1)
        return "depth";
    if (scope->movie || tMovieDepth != 0)
        return "movie";
    if (matchG)
    {
        uint32_t current = 0;
        if (!readU32(scope->renderer + kRendererG, current))
            return "g_unreadable";
        return current == g ? "ok" : "g_mismatch";
    }
    return scope->renderer == renderer ? "ok" : "renderer_mismatch";
}

void gearDraw(const char* kind, uint32_t entry, uint32_t renderer, uint64_t lr)
{
    if (!da::Active() || !gearOpen())
        return;
    tGear.drawOrd        = ++tGear.draws;
    tGear.drawKind       = kind;
    const MaskState mask = readMaskState(renderer);
    gearLine("draw", "kind=%s fn=%08X draw=%u renderer=%08X caller=%08X scope=%u stencil=%d active=%u f188=%u f189=%u ref=%08X", kind, entry, tGear.drawOrd, renderer, static_cast<uint32_t>(lr), gearScopeSerial(), tGear.stencil ? 1 : 0, mask.active, mask.flag188, mask.flag189, mask.ref192);
}

void gearMask(const char* event, uint32_t entry, uint32_t renderer, uint64_t lr, int64_t kind)
{
    if (!da::Active() || !gearOpen())
        return;
    ++tGear.masks;
    const MaskState mask = readMaskState(renderer);
    if (entry == kHookMaskEnter)
        tGear.stencil = true;
    else if (entry == kHookMaskExit)
        tGear.stencil = false;
    gearLine(event, "fn=%08X kind=%lld renderer=%08X caller=%08X scope=%u draw=%u draw_kind=%s stencil=%d active=%u f188=%u f189=%u ref=%08X", entry, static_cast<long long>(kind), renderer, static_cast<uint32_t>(lr), gearScopeSerial(), tGear.drawOrd, tGear.drawKind, tGear.stencil ? 1 : 0, mask.active, mask.flag188, mask.flag189, mask.ref192);
}

bool readRow(uint32_t address, std::array<float, 3>& row)
{
    return readFloat(address, row[0]) && readFloat(address + 4, row[1]) && readFloat(address + 8, row[2]);
}

//------------------------------------------------------------- observations

struct CameraRead
{
    bool                 readable  = false;
    uint32_t             vtable    = 0;
    bool                 frustumOk = false;
    da::FrustumBytes     frustum{};
    bool                 orthographic = false;
    bool                 viewportOk   = false;
    std::array<float, 4> viewport{};
};

CameraRead readCamera(uint32_t camera)
{
    CameraRead read;
    if (!readU32(camera, read.vtable))
        return read;
    read.readable = true;
    if (readBytes(camera + kCameraFrustum, read.frustum.data(), static_cast<uint32_t>(read.frustum.size())))
    {
        read.frustumOk    = true;
        read.orthographic = read.frustum[24] != 0;
    }
    read.viewportOk = true;
    for (size_t index = 0; index < 4; ++index)
    {
        if (!readFloat(camera + kCameraViewport + static_cast<uint32_t>(index) * 4, read.viewport[index]) || !std::isfinite(read.viewport[index]))
        {
            read.viewportOk      = false;
            read.viewport[index] = 0.0f;
        }
    }
    return read;
}

std::array<float, 4> boundsOf(const da::FrustumBytes& bytes)
{
    da::Frustum f;
    bool        orthographic = false;
    da::DecodeFrustum(bytes, f, orthographic);
    return { f.left, f.right, f.top, f.bottom };
}

void observe(uint32_t hook, uint32_t camera, uint32_t lr, const CameraRead& read, bool applied, int restored, const da::FrustumBytes* corrected, da::Reason reason)
{
    if (!da::ProbeEnabled())
        return;
    da::CameraObservation o;
    o.hook          = hook;
    o.camera        = camera;
    o.cameraVtable  = read.vtable;
    o.owner         = tFrame.owner;
    o.returnAddress = lr;
    o.frameReturn   = tFrame.caller;
    if (tFrame.owner)
        readU32(tFrame.owner, o.ownerVtable);
    if (const StageScope* stage = tStages.Top())
    {
        o.stageFirst = stage->first;
        o.stageLast  = stage->last;
    }
    o.orthographic  = read.orthographic;
    o.viewportValid = read.viewportOk;
    o.viewport      = read.viewport;
    if (read.frustumOk)
    {
        o.original  = boundsOf(read.frustum);
        o.corrected = corrected ? boundsOf(*corrected) : o.original;
    }
    o.applied  = applied;
    o.restored = restored;
    o.reason   = reason;
    da::RecordCamera(o);
}

// Census entry for functions whose first argument may be a camera.
void observeConsumer(uint32_t hook, uint32_t candidate, uint32_t lr)
{
    if (!da::ProbeEnabled())
        return;
    const CameraRead read = readCamera(candidate);
    observe(hook, candidate, lr, read, false, -1, nullptr, da::Reason::CameraUnknown);
}

struct FrameGuard
{
    explicit FrameGuard(uint32_t owner, uint32_t camera, uint32_t caller)
    {
        outermost = tFrameDepth++ == 0;
        if (outermost)
            tFrame = FrameScope{ owner, camera, caller };
    }

    ~FrameGuard()
    {
        --tFrameDepth;
        if (outermost)
            tFrame = FrameScope{};
    }

    bool outermost = false;
};

template <typename T>
struct StackGuard
{
    StackGuard(BoundedStack<T>& stack, const T& item)
    : stack_(stack)
    , pushed_(stack.Push(item))
    {
    }

    ~StackGuard()
    {
        if (pushed_)
            stack_.Pop();
    }

    BoundedStack<T>& stack_;
    bool             pushed_;
};

struct MovieGuard
{
    MovieGuard()
    {
        ++tMovieDepth;
    }

    ~MovieGuard()
    {
        --tMovieDepth;
    }
};

// The join scope that produced this build call, if any.
bool joinedCamera(uint32_t camera, uint32_t& hook)
{
    for (size_t index = tJoins.Count(); index > 0; --index)
    {
        if (tJoins.At(index - 1).camera == camera)
        {
            hook = tJoins.At(index - 1).hook;
            return true;
        }
    }
    return false;
}

void enterJoin(uint32_t hook, uint32_t camera, uint32_t lr)
{
    if (da::ProbeEnabled())
    {
        const CameraRead read = readCamera(camera);
        observe(hook, camera, lr, read, false, -1, nullptr, da::CurrentFrame().reason);
    }
}

} // namespace

//---------------------------------------------------------------- raw hooks

REX_HOOK_RAW(sub_82C71A00)
{
    if (!da::Active())
    {
        __imp__sub_82C71A00(ctx, base);
        return;
    }
    const uint32_t owner  = ctx.r3.u32;
    const uint32_t camera = ctx.r4.u32;
    const uint32_t caller = static_cast<uint32_t>(ctx.lr);
    FrameGuard     guard(owner, camera, caller);
    if (guard.outermost)
    {
        da::LatchInputs inputs;
        inputs.frameOwner     = owner;
        inputs.incomingCamera = camera;
        inputs.frameCaller    = caller;
        inputs.keyKnown       = readKey(inputs.key);
        const CameraRead read = readCamera(camera);
        inputs.cameraReadable = read.readable && read.frustumOk;
        inputs.cameraVtable   = read.vtable;
        inputs.orthographic   = read.orthographic;
        if (read.frustumOk)
        {
            bool ortho = false;
            da::DecodeFrustum(read.frustum, inputs.frustum, ortho);
        }
        const da::FrameView view = da::LatchFrame(inputs);
        observe(kHookFrame, camera, caller, read, false, -1, nullptr, view.reason);
    }
    __imp__sub_82C71A00(ctx, base);
}

REX_HOOK_RAW(sub_823528D8)
{
    if (!da::Active())
    {
        __imp__sub_823528D8(ctx, base);
        return;
    }
    const uint32_t renderer = ctx.r3.u32;
    const uint32_t camera   = ctx.r4.u32;
    const uint32_t lr       = static_cast<uint32_t>(ctx.lr);
    enterJoin(kHookRenderer, camera, lr);

    // A scale change must invalidate the renderer's cached camera match: tracked
    // cameras and candidates rebuild; an untracked wide build needs Original joins.
    const da::FrameView view = da::CurrentFrame();
    if (view.latched && camera != 0)
    {
        const bool  inWorld   = view.wide && da::CameraCallerAccepted(tFrame.caller);
        const bool  untracked = da::UntrackedWideBuilds();
        const bool  tracked   = da::CameraTracked(camera);
        const bool  vtableOk  = !tracked && (inWorld || untracked) && da::CameraVtableAccepted(readCamera(camera).vtable);
        const bool  candidate = tracked || vtableOk;
        const float wanted    = inWorld ? view.k : 1.0f;
        if (candidate && da::CacheNeedsRebuild(camera, wanted, view.interval))
        {
            uint32_t cached = 0;
            if (readU32(renderer + kRendererCacheCamera, cached) && (cached == camera || (untracked && wanted == 1.0f && cached != 0)))
                writeU32(renderer + kRendererCacheCamera, 0);
            if (untracked && !view.wide)
                da::NoteOriginalJoin(view.interval);
        }
    }

    StackGuard<JoinScope> scope(tJoins, JoinScope{ kHookRenderer, renderer, camera });
    __imp__sub_823528D8(ctx, base);
}

REX_HOOK_RAW(sub_823AF9C8)
{
    if (!da::Active())
    {
        __imp__sub_823AF9C8(ctx, base);
        return;
    }
    const uint32_t owner  = ctx.r3.u32;
    const uint32_t lr     = static_cast<uint32_t>(ctx.lr);
    uint32_t       camera = 0;
    readU32(owner + 0x20, camera);
    enterJoin(kHookOwner, camera, lr);
    StackGuard<JoinScope> scope(tJoins, JoinScope{ kHookOwner, owner, camera });
    __imp__sub_823AF9C8(ctx, base);
}

REX_HOOK_RAW(sub_8250BA00)
{
    if (!da::Active())
    {
        __imp__sub_8250BA00(ctx, base);
        return;
    }

    const uint32_t      frustum = ctx.r8.u32;
    const uint32_t      camera  = frustum - kCameraFrustum;
    const uint32_t      lr      = static_cast<uint32_t>(ctx.lr);
    const da::FrameView view    = da::CurrentFrame();
    const CameraRead    read    = readCamera(camera);

    uint32_t   joinHook = 0;
    const bool joined   = joinedCamera(camera, joinHook);

    // Only cameras inside the accepted world frame are eligible. The 16:9
    // frustum check and nesting guard prevent repeated correction.
    da::Reason reason = view.reason;
    if (view.wide)
    {
        if (camera == 0 || !da::CameraFamilyAccepted(tFrame.caller, kHookBuild, read.vtable))
            reason = da::Reason::CameraUnknown;
        else if (!read.frustumOk)
            reason = da::Reason::CameraUnreadable;
        else if (read.orthographic)
            reason = da::Reason::CameraOrtho;
        else if (!read.viewportOk)
            reason = da::Reason::ViewportInvalid;
        else
        {
            da::Frustum bounds;
            bool        ortho = false;
            da::DecodeFrustum(read.frustum, bounds, ortho);
            if (!da::IsSixteenByNineFrustum(bounds))
                reason = da::Reason::CameraUnknown;
        }
    }
    const bool eligible = view.wide && reason == da::Reason::World;

    const float       scale     = view.wide ? view.k : (da::ProbeEnabled() ? da::ProbeScale() : 1.0f);
    da::FrustumBytes  corrected = read.frustum;
    da::FrustumStatus status    = da::FrustumStatus::Orthographic;
    if (read.frustumOk && (eligible || da::ProbeEnabled()))
        status = da::CorrectFrustumBytes(read.frustum, scale, corrected);

    // The guard slot is reserved before any write, so a full stack refuses the
    // correction instead of leaving a mutated camera unguarded.
    bool applied  = false;
    bool reserved = false;
    if (eligible && status == da::FrustumStatus::Ok && !isCorrected(camera))
    {
        reserved = tCorrected.Push(camera);
        if (reserved)
        {
            // Only the left and right words change; the originals are restored
            // from the saved copy after the call.
            applied = writeBytes(camera + kCameraFrustum, corrected.data(), 8);
        }
    }
    if (eligible && !applied)
        da::CountRefusal();

    if (read.frustumOk && joined)
        da::BaselineChangedWithoutSetter(camera, read.frustum);

    __imp__sub_8250BA00(ctx, base);
    if (reserved)
        tCorrected.Pop();

    int restored = -1;
    if (applied)
    {
        da::FrustumBytes current{};
        if (readBytes(camera + kCameraFrustum, current.data(), 8) &&
            std::memcmp(current.data(), corrected.data(), 8) == 0 && writeBytes(camera + kCameraFrustum, read.frustum.data(), 8))
        {
            restored = 1;
        }
        else
        {
            restored = 0;
            da::CountRestoreMismatch();
        }
    }
    if (camera != 0 && (applied || da::CameraTracked(camera)))
        da::NoteCacheBuilt(camera, applied ? view.k : 1.0f, view.interval);
    da::CountCameraBuild(applied);
    observe(kHookBuild, camera, lr, read, applied, restored, &corrected, applied ? da::Reason::World : reason);
}

REX_HOOK_RAW(sub_82C71880)
{
    if (!da::Active())
    {
        __imp__sub_82C71880(ctx, base);
        return;
    }
    StackGuard<StageScope> scope(tStages, StageScope{ ctx.r4.u32, ctx.r5.u32 });
    __imp__sub_82C71880(ctx, base);
}

REX_HOOK_RAW(sub_82C5EC28)
{
    if (!da::Active())
    {
        __imp__sub_82C5EC28(ctx, base);
        return;
    }
    const uint32_t owner    = ctx.r3.u32;
    uint32_t       handle   = 0;
    const bool     readable = readU32(owner + 0x34, handle);
    da::NoteEarlyEvent(da::EarlyEvent::Movie);
    da::NoteMovie(owner, handle, readable);
    MovieGuard guard;
    __imp__sub_82C5EC28(ctx, base);
}

REX_HOOK_RAW(sub_823035D0)
{
    if (!da::Active())
    {
        // A scope opened while active is closed even after the feature turns off.
        if (tGfx.Count() == 0 && tGfxOverflow == 0)
        {
            __imp__sub_823035D0(ctx, base);
            return;
        }
    }
    const uint32_t renderer  = ctx.r3.u32;
    const uint32_t endCaller = static_cast<uint32_t>(ctx.lr);
    GfxScope       scope;
    bool           open  = false;
    const uint32_t depth = static_cast<uint32_t>(tGfx.Count()) + tGfxOverflow;
    if (tGfxOverflow > 0)
    {
        --tGfxOverflow;
    }
    else if (GfxScope* top = tGfx.Top(); top && top->renderer == renderer)
    {
        scope = *top;
        open  = true;
        tGfx.Pop();
        readU32(renderer + kRendererG, scope.g);
    }
    __imp__sub_823035D0(ctx, base);

    // Restore the original viewport through the same body, so the compressed one
    // never leaks and the scissor is re-derived from the stored canonical rectangle.
    if (open && scope.viewportCompressed && scope.g != 0)
    {
        const auto word = [&](size_t index)
        {
            return static_cast<double>(static_cast<float>(static_cast<int32_t>(scope.viewportOriginal[index])));
        };
        PPCContext call = ctx;
        call.r3.u64     = scope.g;
        call.f1.f64     = word(0);
        call.f2.f64     = word(1);
        call.f3.f64     = word(2);
        call.f4.f64     = word(3);
        call.f5.f64     = static_cast<double>(std::bit_cast<float>(scope.viewportOriginal[4]));
        call.f6.f64     = static_cast<double>(std::bit_cast<float>(scope.viewportOriginal[5]));
        tScissorBypass  = true;
        __imp__sub_82690138(call, base);
        tScissorBypass = false;
        ctx.fpscr      = call.fpscr;
    }

    // Corrected scissor state must not leak into later non-GFx draws: reapply
    // the stored canonical rectangle through Original with the correction off.
    if (open && scope.scissorTouched && scope.g != 0)
    {
        PPCContext call = ctx;
        call.r3.u64     = scope.g;
        call.r4.u64     = scope.g + kGfxScissorState;
        tScissorBypass  = true;
        __imp__sub_8268F9C0(call, base);
        tScissorBypass = false;
        ctx.fpscr      = call.fpscr;
    }
    if (da::Active() && gearOpen())
    {
        gearLine("scope_end", "serial=%u depth=%u renderer=%08X caller=%08X frame_caller=%08X open=%d accepted=%d touched=%d affine=%u viewport=%d scissor=%u scissor_applied=%u", scope.serial, depth, renderer, endCaller, tFrame.caller, open ? 1 : 0, scope.accepted ? 1 : 0, scope.scissorTouched ? 1 : 0, scope.affineSeen, scope.viewportCompressed ? 1 : 0, scope.scissorSeen, scope.scissorApplied);
    }
}

#define DISPLAY_ASPECT_PROBE_HOOK(address, noteSetter)                               \
    REX_HOOK_RAW(sub_##address)                                                      \
    {                                                                                \
        if (da::Active() && da::ProbeEnabled())                                      \
        {                                                                            \
            if (noteSetter)                                                          \
                da::NoteSetter(ctx.r3.u32);                                          \
            observeConsumer(0x##address, ctx.r3.u32, static_cast<uint32_t>(ctx.lr)); \
        }                                                                            \
        __imp__sub_##address(ctx, base);                                             \
    }

DISPLAY_ASPECT_PROBE_HOOK(8234EB00, true)
DISPLAY_ASPECT_PROBE_HOOK(8234CF18, true)
DISPLAY_ASPECT_PROBE_HOOK(8234DF90, false)
DISPLAY_ASPECT_PROBE_HOOK(8234D360, false)
DISPLAY_ASPECT_PROBE_HOOK(82C5DF60, false)
DISPLAY_ASPECT_PROBE_HOOK(8234D890, false)
DISPLAY_ASPECT_PROBE_HOOK(82C49D08, false)

//------------------------------------------------------------------ midasm

namespace
{

// sub_8268F9C0 clips the canonical request at G+12676 to the viewport. Compress
// it first; with the scissor test off the viewport is the scissor, unchanged.
bool compressedViewportScissor(uint32_t g, float k, const da::Rect& rect, da::Rect& out)
{
    out             = rect;
    uint32_t enable = 0;
    if (!readU32(g + kGfxScissorEnable, enable))
        return false;
    if (enable == 0)
        return true;
    float                   x = 0;
    float                   w = 0;
    std::array<uint32_t, 4> request{};
    bool                    ok = readFloat(g + kGfxViewportX, x) && readFloat(g + kGfxViewportWidth, w) && std::isfinite(x) && std::isfinite(w) && std::fabs(x) < 1.0e6f &&
                                 std::fabs(w) < 1.0e6f;
    for (size_t index = 0; ok && index < request.size(); ++index)
        ok = readU32(g + kGfxScissorState + static_cast<uint32_t>(index) * 4, request[index]);
    if (!ok)
        return false;
    const da::ViewportSpan span{ static_cast<int32_t>(x), static_cast<int32_t>(w) };
    const da::Rect         raw{ static_cast<int32_t>(request[0]), static_cast<int32_t>(request[1]), static_cast<int32_t>(request[2]), static_cast<int32_t>(request[3]) };
    const da::Rect         clipped = da::CorrectScissorInViewport(raw, span, k);
    out.left                       = clipped.left;
    out.right                      = clipped.right;
    return true;
}

// The outermost GFx scope of the final output, outside a movie.
GfxScope* admittedGfxScope(uint32_t renderer, uint32_t g, bool matchG)
{
    GfxScope* scope = tGfx.Top();
    if (!scope || !scope->accepted || scope->depth != 1 || scope->movie || tMovieDepth != 0 || tGfxOverflow != 0)
        return nullptr;
    if (matchG)
    {
        uint32_t current = 0;
        if (!readU32(scope->renderer + kRendererG, current) || current != g)
            return nullptr;
    }
    else if (scope->renderer != renderer)
    {
        return nullptr;
    }
    return scope;
}

} // namespace

// Midasm at the entry of sub_82302F60 (GFx display begin); the route hooks own a
// raw hook there. Device state is read at use time, as it may not exist yet.
void BeginDisplayAspectGfxScope(PPCRegister& r3, uint64_t lr) noexcept
{
    if (!da::Active())
        return;
    const uint32_t renderer    = r3.u32;
    const uint32_t beginCaller = static_cast<uint32_t>(lr);
    da::NoteEarlyEvent(da::EarlyEvent::Ui);

    GfxScope scope;
    scope.renderer = renderer;
    scope.depth    = static_cast<uint32_t>(tGfx.Count()) + tGfxOverflow + 1;
    scope.movie    = tMovieDepth != 0;
    uint32_t first = UINT32_MAX;
    uint32_t last  = UINT32_MAX;
    if (const StageScope* stage = tStages.Top())
    {
        first = stage->first;
        last  = stage->last;
    }
    scope.accepted = scope.depth == 1 && !scope.movie && da::GfxScopeAccepted(beginCaller, tFrame.caller);
    if (da::ProbeEnabled())
    {
        da::CameraObservation o;
        o.hook          = kHookGfxBegin;
        o.camera        = renderer;
        o.owner         = tFrame.owner;
        o.returnAddress = beginCaller;
        o.frameReturn   = tFrame.caller;
        o.stageFirst    = first;
        o.stageLast     = last;
        o.reason        = da::Reason::OffscreenUnconfirmed;
        da::RecordCamera(o);
    }
    // The scope outlives this call; the matching end hook pops it. A full stack
    // is counted so the end hook can skip the unmatched pop.
    if (gearOpen())
        scope.serial = ++tGear.serial;
    const bool pushed = tGfx.Push(scope);
    if (!pushed)
    {
        ++tGfxOverflow;
        da::CountRefusal();
    }
    if (gearOpen())
    {
        gearLine("scope_begin", "serial=%u depth=%u renderer=%08X caller=%08X frame_caller=%08X movie=%d accepted=%d movie_depth=%u pushed=%d", scope.serial, scope.depth, renderer, beginCaller, tFrame.caller, scope.movie ? 1 : 0, scope.accepted ? 1 : 0, tMovieDepth, pushed ? 1 : 0);
    }
}

// Compresses the viewport record (r4 at r1+144; device r3, renderer r31) of
// sub_82302F60's sub_826903A0 call, which maps and clips the whole scope.
// RestoreDisplayAspectGfxViewportRecord restores the words read again.
void CompressDisplayAspectGfxViewport(PPCRegister& r1, PPCRegister& r3, PPCRegister& r31) noexcept
{
    if (!da::Active())
        return;
    const da::FrameView view = da::CurrentFrame();
    if (!view.latched || !view.wide)
        return;
    GfxScope* scope = admittedGfxScope(r31.u32, 0, false);
    if (!scope)
        return;

    const char*             decision = "read_failed";
    const uint32_t          record   = r1.u32 + kGfxViewportRecord;
    uint32_t                g        = 0;
    std::array<uint32_t, 6> words{};
    da::ViewportSpan        span;
    bool                    ok = readU32(r31.u32 + kRendererG, g) && g == r3.u32;
    for (size_t index = 0; ok && index < words.size(); ++index)
        ok = readU32(record + static_cast<uint32_t>(index) * 4, words[index]);
    if (ok)
    {
        decision = "bad_viewport";
        if (da::CompressViewportSpan(static_cast<int32_t>(words[0]), static_cast<int32_t>(words[2]), view.k, span))
        {
            decision = "write_failed";
            if (writeU32(record, static_cast<uint32_t>(span.x)))
            {
                if (writeU32(record + 8, static_cast<uint32_t>(span.width)))
                {
                    scope->viewportCompressed = true;
                    scope->viewportPatched    = true;
                    scope->viewportStack      = record;
                    scope->viewportOriginal   = words;
                    decision                  = "applied";
                }
                else
                {
                    writeU32(record, words[0]);
                }
            }
        }
    }
    if (!scope->viewportCompressed)
        da::CountRefusal();
    if (gearOpen())
    {
        gearLine("viewport",
                 "scope=%u g=%08X orig=[%d,%d,%d,%d] out=[%d,%d] k=%.9g decision=%s",
                 scope->serial,
                 g,
                 static_cast<int32_t>(words[0]),
                 static_cast<int32_t>(words[1]),
                 static_cast<int32_t>(words[2]),
                 static_cast<int32_t>(words[3]),
                 span.x,
                 span.width,
                 view.k,
                 decision);
    }
}

void RestoreDisplayAspectGfxViewportRecord(PPCRegister& r1) noexcept
{
    GfxScope* scope = tGfx.Top();
    if (!scope || !scope->viewportPatched || scope->viewportStack != r1.u32 + kGfxViewportRecord)
        return;
    scope->viewportPatched = false;
    writeU32(scope->viewportStack, scope->viewportOriginal[0]);
    writeU32(scope->viewportStack + 8, scope->viewportOriginal[2]);
}

void CorrectDisplayAspectGfxAffine(PPCRegister& r1, PPCRegister& r10) noexcept
{
    if (!da::Active())
        return;
    const da::FrameView view     = da::CurrentFrame();
    const uint32_t      stack    = r1.u32;
    const uint32_t      renderer = r10.u32;

    const bool           trace = gearOpen();
    std::array<float, 3> inX{};
    std::array<float, 3> inY{};
    std::array<float, 3> srcX{};
    std::array<float, 3> srcY{};
    if (trace)
    {
        readRow(stack + 0x60, inX);
        readRow(stack + 0x6C, inY);
        readRow(renderer + 228, srcX);
        readRow(renderer + 240, srcY);
    }

    bool        applied       = false;
    bool        covered       = false;
    const char* decision      = "frame_not_wide";
    uint32_t    g             = 0;
    float       viewportX     = 0;
    float       viewportWidth = 0;
    if (view.latched && view.wide)
    {
        decision                      = admitReason(renderer, 0, false);
        const GfxScope* admittedScope = admittedGfxScope(renderer, 0, false);
        const bool      admitted      = admittedScope != nullptr;
        // The compressed hardware viewport already carries the correction.
        covered = admitted && admittedScope->viewportCompressed;
        if (covered)
            decision = "viewport";
        else if (admitted)
        {
            da::Affine affine;
            decision = "read_failed";
            if (readU32(renderer + kRendererG, g) && readFloat(stack + 0x60, affine.a) && readFloat(stack + 0x64, affine.b) &&
                readFloat(stack + 0x68, affine.tx) && readFloat(g + kGfxViewportX, viewportX) && readFloat(g + kGfxViewportWidth, viewportWidth))
            {
                const da::AffineResult result = da::CorrectGfxAffine(affine, viewportX, viewportWidth, view.k);
                switch (result.status)
                {
                    case da::AffineStatus::Ok:
                        applied  = writeFloat(stack + 0x60, result.affine.a) && writeFloat(stack + 0x64, result.affine.b) &&
                                   writeFloat(stack + 0x68, result.affine.tx);
                        decision = applied ? "applied" : "write_failed";
                        break;
                    case da::AffineStatus::InvalidScale:
                        decision = "invalid_scale";
                        break;
                    case da::AffineStatus::NonFinite:
                        decision = "non_finite";
                        break;
                    case da::AffineStatus::BadViewport:
                        decision = "bad_viewport";
                        break;
                }
            }
        }
        if (admitted && !applied && !covered)
            da::CountRefusal();
    }
    if (trace)
    {
        if (GfxScope* top = tGfx.Top())
            ++top->affineSeen;
        std::array<float, 3> outX{};
        readRow(stack + 0x60, outX);
        uint32_t composerCaller = 0;
        readU32(stack + kComposerCallerOff, composerCaller);
        gearLine("affine",
                 "scope=%u renderer=%08X g=%08X caller=%08X draw=%u draw_kind=%s stencil=%d src_x=[%.9g,%.9g,%.9g] src_y=[%.9g,%.9g,%.9g] in_x=[%.9g,%.9g,%.9g] in_y=[%.9g,%.9g,%.9g] out_x=[%.9g,%.9g,%.9g] vp=[%.9g,%.9g] k=%.9g decision=%s",
                 gearScopeSerial(),
                 renderer,
                 g,
                 composerCaller,
                 tGear.drawOrd,
                 tGear.drawKind,
                 tGear.stencil ? 1 : 0,
                 srcX[0],
                 srcX[1],
                 srcX[2],
                 srcY[0],
                 srcY[1],
                 srcY[2],
                 inX[0],
                 inX[1],
                 inX[2],
                 inY[0],
                 inY[1],
                 inY[2],
                 outX[0],
                 outX[1],
                 outX[2],
                 viewportX,
                 viewportWidth,
                 view.k,
                 decision);
    }
    da::CountGfx(applied || covered);
}

void CorrectDisplayAspectGfxScissor(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r7) noexcept
{
    if (!da::Active() || tScissorBypass)
        return;
    const da::FrameView view = da::CurrentFrame();
    const da::Rect      rect{ r4.s32, r5.s32, r6.s32, r7.s32 };
    const bool          trace = gearOpen();

    bool        applied  = false;
    const char* decision = "frame_not_wide";
    if (view.latched && view.wide)
    {
        if (trace)
            decision = admitReason(0, r3.u32, true);
        if (GfxScope* scope = admittedGfxScope(0, r3.u32, true))
        {
            if (da::ScissorCorrectionEnabled())
            {
                da::Rect out{};
                bool     ready = true;
                if (!scope->viewportCompressed)
                    out = da::CorrectScissor(rect, view.k);
                else
                    ready = compressedViewportScissor(r3.u32, view.k, rect, out);
                if (ready)
                {
                    r4.s64                = out.left;
                    r6.s64                = out.right;
                    scope->scissorTouched = true;
                    applied               = true;
                    decision              = "applied";
                }
                else
                {
                    decision = "viewport_read_failed";
                }
            }
            else
            {
                decision = "disabled";
            }
        }
    }
    if (trace)
    {
        GfxScope* top = tGfx.Top();
        if (top)
        {
            ++top->scissorSeen;
            if (applied)
                ++top->scissorApplied;
            ++tGear.scissorIn;
        }
        else
        {
            ++tGear.scissorOut;
        }
        if (top || tGear.outsideLogged++ < kGearOutsideLines)
        {
            uint32_t callerSaved = 0;
            readU32(r1.u32 + kComposerCallerOff, callerSaved);
            float gx = 0;
            float gw = 0;
            readFloat(r3.u32 + kGfxViewportX, gx);
            readFloat(r3.u32 + kGfxViewportWidth, gw);
            gearLine("scissor",
                     "scope=%u dev=%08X caller=%08X draw=%u draw_kind=%s stencil=%d in=[%d,%d,%d,%d] out=[%d,%d,%d,%d] gvp=[%.9g,%.9g] k=%.9g decision=%s",
                     top ? top->serial : 0,
                     r3.u32,
                     callerSaved,
                     tGear.drawOrd,
                     tGear.drawKind,
                     tGear.stencil ? 1 : 0,
                     rect.left,
                     rect.top,
                     rect.right,
                     rect.bottom,
                     r4.s32,
                     r5.s32,
                     r6.s32,
                     r7.s32,
                     gx,
                     gw,
                     view.k,
                     decision);
        }
    }
    da::CountScissor(applied);
}

// Midasm at the entries of the three Scaleform draw functions and the three
// mask functions: probe-only markers; r3 is the GFx renderer.
void TraceDisplayAspectGearIndexed(PPCRegister& r3, uint64_t lr) noexcept
{
    gearDraw("indexed", kHookDrawIndexed, r3.u32, lr);
}

void TraceDisplayAspectGearLine(PPCRegister& r3, uint64_t lr) noexcept
{
    gearDraw("line", kHookDrawLine, r3.u32, lr);
}

void TraceDisplayAspectGearBitmap(PPCRegister& r3, uint64_t lr) noexcept
{
    gearDraw("bitmap", kHookDrawBitmap, r3.u32, lr);
}

void TraceDisplayAspectGearMaskEnter(PPCRegister& r3, PPCRegister& r4, uint64_t lr) noexcept
{
    gearMask("mask_enter", kHookMaskEnter, r3.u32, lr, r4.s32);
}

void TraceDisplayAspectGearMaskTest(PPCRegister& r3, uint64_t lr) noexcept
{
    gearMask("mask_test", kHookMaskTest, r3.u32, lr, -1);
}

void TraceDisplayAspectGearMaskExit(PPCRegister& r3, uint64_t lr) noexcept
{
    gearMask("mask_exit", kHookMaskExit, r3.u32, lr, -1);
}

namespace rerevved::display_aspect
{

void OnGuestSwap()
{
    if (!Active())
        return;
    SwapInputs inputs;
    inputs.keyKnown = readKey(inputs.key);
    FinishInterval(inputs);
}

} // namespace rerevved::display_aspect
