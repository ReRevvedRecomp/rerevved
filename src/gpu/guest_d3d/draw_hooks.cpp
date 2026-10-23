#include <cstdint>
#include <utility>

#include <rex/hook.h>

#include "gpu/guest_d3d/developer_hooks.h"
#include "gpu/guest_d3d/gamma_observer.h"
#include "gpu/guest_d3d/indexed_buffer_consume.h"
#include "gpu/guest_d3d/native_draw_hook.h"
#include "gpu/guest_d3d/user_pointer_consume.h"

#if REREVVED_DEVELOPER
#include "gpu/diagnostics/label_draw_capture.h"
#include "gpu/diagnostics/route_trace.h"
#include "gpu/diagnostics/scene_resource_capture.h"
#include "gpu/diagnostics/shader_harvest.h"
#endif

REX_EXTERN(__imp__sub_826A3568);
REX_EXTERN(__imp__sub_826A3000);
REX_EXTERN(__imp__sub_826A39F8);
REX_EXTERN(__imp__sub_826A2FF0);
REX_EXTERN(__imp__sub_826AD150);
REX_EXTERN(__imp__sub_826A7040);
REX_EXTERN(__imp__sub_826A7138);

#if REREVVED_DEVELOPER
namespace
{

// The draw as its hook entered it, kept only while a shader harvest runs.
rerevved::gpu::diagnostics::ShaderHarvestCall harvestCall(std::uint32_t entry, const PPCContext& ctx) noexcept
{
    return { entry, static_cast<std::uint32_t>(ctx.lr), { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32 }, ctx.r1.u32 };
}

} // namespace
#endif

// The generated mid-body hook calls this in every build; only a developer build records.
void ObserveNativeGuestVertexSelection([[maybe_unused]] PPCRegister& r30, [[maybe_unused]] PPCRegister& r31, [[maybe_unused]] PPCRegister& r15, [[maybe_unused]] PPCRegister& r19)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::diagnostics::ObserveLabelAdapterSelection(r30.u32, r31.u32, r15.u32, r19.u32);
#endif
}

void ObserveNativeGuestGammaTable(PPCRegister& r31, PPCRegister& r1)
{
    rerevved::gpu::guest_d3d::ObserveGammaSetter(r31.u32, r1.u32);
}

void ObserveNativeGuestPwlGamma(PPCRegister& r31)
{
    rerevved::gpu::guest_d3d::ObserveGammaPwlSetter(r31.u32);
}

REX_HOOK_RAW(sub_826A39F8)
{
    [[maybe_unused]] const auto                     guestApiCall = rerevved::gpu::guest_d3d::CountGuestApiCall<0x826A39F8>();
    const rerevved::gpu::guest_d3d::NativeDrawScope nativeDraw(rerevved::gpu::guest_d3d::NativeDrawHook::IndexedBuffer, ctx, base);

#if REREVVED_DEVELOPER
    auto scene = rerevved::gpu::diagnostics::ObserveSceneEntry(0x826A39F8, ctx);

    struct SceneEndGuard
    {
        rerevved::gpu::diagnostics::SceneHookToken& scene;
        const PPCContext&                           context;

        ~SceneEndGuard()
        {
            if (scene.session)
                rerevved::gpu::diagnostics::EndSceneEntry(std::move(scene), context);
        }
    } guard{ scene, ctx };

    rerevved::gpu::diagnostics::TraceRouteEntry(rerevved::gpu::diagnostics::RouteFunction::sub_826A39F8, ctx.lr);
    const auto harvest   = rerevved::gpu::diagnostics::shaderHarvestObserver.load(std::memory_order_acquire);
    const auto harvested = harvest ? harvestCall(0x826A39F8, ctx) : rerevved::gpu::diagnostics::ShaderHarvestCall{};
    // The mid-body observers and the scene writers only run inside Original.
    const bool diagnostics = harvest || scene.session || rerevved::gpu::diagnostics::sceneCaptureSession.load(std::memory_order_relaxed) ||
                             rerevved::gpu::diagnostics::routeTraceEnabled.load(std::memory_order_relaxed);
    if (!rerevved::gpu::guest_d3d::ConsumeIndexedBuffer(nativeDraw, ctx, diagnostics))
        __imp__sub_826A39F8(ctx, base);
    if (harvest)
        harvest(harvested);
    if (scene.session)
    {
        rerevved::gpu::diagnostics::EndSceneEntry(std::move(scene), ctx);
        scene.session = nullptr;
    }
#else
    if (!rerevved::gpu::guest_d3d::ConsumeIndexedBuffer(nativeDraw, ctx, false))
        __imp__sub_826A39F8(ctx, base);
#endif
}

REX_HOOK_RAW(sub_826A7040)
{
    [[maybe_unused]] const auto guestApiCall = rerevved::gpu::guest_d3d::CountGuestApiCall<0x826A7040>();
#if REREVVED_DEVELOPER
    rerevved::gpu::diagnostics::TraceRouteEntry(rerevved::gpu::diagnostics::RouteFunction::sub_826A7040, ctx.lr);
#endif
    const auto observed = rerevved::gpu::guest_d3d::ObserveGammaEmissionBegin(ctx.r3.u32, ctx.r4.u32, static_cast<std::uint32_t>(ctx.lr));
    __imp__sub_826A7040(ctx, base);
    rerevved::gpu::guest_d3d::ObserveGammaEmissionEnd(observed);
}

REX_HOOK_RAW(sub_826A7138)
{
    [[maybe_unused]] const auto guestApiCall = rerevved::gpu::guest_d3d::CountGuestApiCall<0x826A7138>();
#if REREVVED_DEVELOPER
    rerevved::gpu::diagnostics::TraceRouteEntry(rerevved::gpu::diagnostics::RouteFunction::sub_826A7138, ctx.lr);
#endif
    const auto observed = rerevved::gpu::guest_d3d::ObserveGammaPwlEmissionBegin(ctx.r3.u32);
    __imp__sub_826A7138(ctx, base);
    rerevved::gpu::guest_d3d::ObserveGammaEmissionEnd(observed);
}

REX_HOOK_RAW(sub_826A3568)
{
    [[maybe_unused]] const auto                     guestApiCall = rerevved::gpu::guest_d3d::CountGuestApiCall<0x826A3568>();
    const rerevved::gpu::guest_d3d::NativeDrawScope nativeDraw(rerevved::gpu::guest_d3d::NativeDrawHook::IndexedUp, ctx, base);

#if REREVVED_DEVELOPER
    auto scene = rerevved::gpu::diagnostics::ObserveSceneEntry(0x826A3568, ctx);

    struct SceneEndGuard
    {
        rerevved::gpu::diagnostics::SceneHookToken& scene;
        const PPCContext&                           context;

        ~SceneEndGuard()
        {
            if (scene.session)
                rerevved::gpu::diagnostics::EndSceneEntry(std::move(scene), context);
        }
    } guard{ scene, ctx };

    rerevved::gpu::diagnostics::TraceRouteEntry(rerevved::gpu::diagnostics::RouteFunction::sub_826A3568, ctx.lr);
    const auto harvest   = rerevved::gpu::diagnostics::shaderHarvestObserver.load(std::memory_order_acquire);
    const auto harvested = harvest ? harvestCall(0x826A3568, ctx) : rerevved::gpu::diagnostics::ShaderHarvestCall{};
    // As the IndexedBuffer entry.
    const bool diagnostics = harvest || scene.session || rerevved::gpu::diagnostics::sceneCaptureSession.load(std::memory_order_relaxed) ||
                             rerevved::gpu::diagnostics::routeTraceEnabled.load(std::memory_order_relaxed);
    if (!rerevved::gpu::guest_d3d::ConsumeUserPointer(rerevved::gpu::guest_d3d::UserPointerWrapper::IndexedUp, nativeDraw, ctx, diagnostics))
        __imp__sub_826A3568(ctx, base);
    if (harvest)
        harvest(harvested);
    if (scene.session)
    {
        rerevved::gpu::diagnostics::EndSceneEntry(std::move(scene), ctx);
        scene.session = nullptr;
    }
#else
    if (!rerevved::gpu::guest_d3d::ConsumeUserPointer(rerevved::gpu::guest_d3d::UserPointerWrapper::IndexedUp, nativeDraw, ctx, false))
        __imp__sub_826A3568(ctx, base);
#endif
}

REX_HOOK_RAW(sub_826AD150)
{
    [[maybe_unused]] const auto guestApiCall = rerevved::gpu::guest_d3d::CountGuestApiCall<0x826AD150>();
#if REREVVED_DEVELOPER
    rerevved::gpu::diagnostics::TraceRouteEntry(rerevved::gpu::diagnostics::RouteFunction::sub_826AD150, ctx.lr);
    const auto shader = ctx.r3.u32, code = ctx.r4.u32, patchState = ctx.r6.u32, variant = ctx.r7.u32;
    const auto caller = ctx.lr;
    __imp__sub_826AD150(ctx, base);
    rerevved::gpu::diagnostics::ObserveLabelAdapterPatch(shader, code, patchState, variant, caller);
#else
    __imp__sub_826AD150(ctx, base);
#endif
}

REX_HOOK_RAW(sub_826A3000)
{
    [[maybe_unused]] const auto                     guestApiCall = rerevved::gpu::guest_d3d::CountGuestApiCall<0x826A3000>();
    const rerevved::gpu::guest_d3d::NativeDrawScope nativeDraw(rerevved::gpu::guest_d3d::NativeDrawHook::NonindexedUp, ctx, base);

#if REREVVED_DEVELOPER
    auto scene = rerevved::gpu::diagnostics::ObserveSceneEntry(0x826A3000, ctx);

    struct SceneEndGuard
    {
        rerevved::gpu::diagnostics::SceneHookToken& scene;
        const PPCContext&                           context;

        ~SceneEndGuard()
        {
            if (scene.session)
                rerevved::gpu::diagnostics::EndSceneEntry(std::move(scene), context);
        }
    } guard{ scene, ctx };

    rerevved::gpu::diagnostics::TraceRouteEntry(rerevved::gpu::diagnostics::RouteFunction::sub_826A3000, ctx.lr);
    auto       adapter   = rerevved::gpu::diagnostics::BeginLabelAdapterDraw(ctx);
    const auto harvest   = rerevved::gpu::diagnostics::shaderHarvestObserver.load(std::memory_order_acquire);
    const auto harvested = harvest ? harvestCall(0x826A3000, ctx) : rerevved::gpu::diagnostics::ShaderHarvestCall{};
    // As the IndexedBuffer entry, plus an adapter capture, which Finish reads post-Original state for.
    const bool diagnostics = harvest || scene.session || rerevved::gpu::diagnostics::sceneCaptureSession.load(std::memory_order_relaxed) ||
                             rerevved::gpu::diagnostics::routeTraceEnabled.load(std::memory_order_relaxed) || adapter;
    if (!rerevved::gpu::guest_d3d::ConsumeUserPointer(rerevved::gpu::guest_d3d::UserPointerWrapper::NonindexedUp, nativeDraw, ctx, diagnostics))
        __imp__sub_826A3000(ctx, base);
    if (harvest)
        harvest(harvested);
    if (scene.session)
    {
        rerevved::gpu::diagnostics::EndSceneEntry(std::move(scene), ctx);
        scene.session = nullptr;
    }
    rerevved::gpu::diagnostics::FinishLabelAdapterDraw(adapter, ctx.r3.u32);
#else
    if (!rerevved::gpu::guest_d3d::ConsumeUserPointer(rerevved::gpu::guest_d3d::UserPointerWrapper::NonindexedUp, nativeDraw, ctx, false))
        __imp__sub_826A3000(ctx, base);
#endif
}

// The commit is not in the census, which generates route_entries.inc,
// so it is a raw hook like the three draw entries above, not a ROUTE_ENTRY.
REX_HOOK_RAW(sub_826A2FF0)
{
    [[maybe_unused]] const auto guestApiCall = rerevved::gpu::guest_d3d::CountGuestApiCall<0x826A2FF0>();
    rerevved::gpu::guest_d3d::NoteNativeDrawCommit(ctx, base);
    __imp__sub_826A2FF0(ctx, base);
}
