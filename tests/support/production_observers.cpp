#include "production_observers.hpp"

#if REREVVED_DEVELOPER

#include "gpu/diagnostics/label_draw_capture.h"

#include <atomic>
#include <utility>

namespace rerevved::tests::production_observers
{
namespace
{

std::atomic<SceneEntryObserver*>   sceneObserver{ nullptr };
std::atomic<LabelAdapterObserver*> labelObserver{ nullptr };

} // namespace

SceneEntryObserver* CurrentSceneObserver() noexcept
{
    return sceneObserver.load(std::memory_order_acquire);
}

LabelAdapterObserver* CurrentLabelObserver() noexcept
{
    return labelObserver.load(std::memory_order_acquire);
}

ScopedSceneEntryObserver::ScopedSceneEntryObserver(SceneEntryObserver* observer) noexcept
: previous_(sceneObserver.exchange(observer, std::memory_order_acq_rel))
{
}

ScopedSceneEntryObserver::~ScopedSceneEntryObserver()
{
    sceneObserver.store(previous_, std::memory_order_release);
}

ScopedLabelAdapterObserver::ScopedLabelAdapterObserver(LabelAdapterObserver* observer) noexcept
: previous_(labelObserver.exchange(observer, std::memory_order_acq_rel))
{
}

ScopedLabelAdapterObserver::~ScopedLabelAdapterObserver()
{
    labelObserver.store(previous_, std::memory_order_release);
}

} // namespace rerevved::tests::production_observers

// The production definitions, compiled under Real<Name> by the test build.
namespace rerevved::gpu::diagnostics
{

SceneHookToken                       RealBeginSceneEntry(void* session, std::uint32_t entry, const PPCContext& ctx) noexcept;
void                                 RealEndSceneEntry(SceneHookToken token, const PPCContext& ctx) noexcept;
std::shared_ptr<LabelAdapterCapture> RealBeginLabelAdapterDraw(const guest_d3d::NativeDrawSubmission& submission);
std::shared_ptr<LabelAdapterCapture> RealBeginLabelAdapterDraw(const PPCContext& ctx);
void                                 RealFinishLabelAdapterDraw(const std::shared_ptr<LabelAdapterCapture>& capture, std::uint32_t result);
void                                 RealObserveLabelAdapterPatch(std::uint32_t shader, std::uint32_t code, std::uint32_t patchState, std::uint32_t variant, std::uint64_t caller);
void                                 RealObserveLabelAdapterSelection(std::uint32_t graphics, std::uint32_t shader, std::uint32_t conditional, std::uint32_t variant);

SceneHookToken BeginSceneEntry(void* session, std::uint32_t entry, const PPCContext& ctx) noexcept
{
    const auto* observer = tests::production_observers::CurrentSceneObserver();
    return observer && observer->begin ? observer->begin(session, entry, ctx) : RealBeginSceneEntry(session, entry, ctx);
}

void EndSceneEntry(SceneHookToken token, const PPCContext& ctx) noexcept
{
    const auto* observer = tests::production_observers::CurrentSceneObserver();
    if (observer && observer->end)
        observer->end(std::move(token), ctx);
    else
        RealEndSceneEntry(std::move(token), ctx);
}

std::shared_ptr<LabelAdapterCapture> BeginLabelAdapterDraw(const guest_d3d::NativeDrawSubmission& submission)
{
    return RealBeginLabelAdapterDraw(submission);
}

std::shared_ptr<LabelAdapterCapture> BeginLabelAdapterDraw(const PPCContext& ctx)
{
    const auto* observer = tests::production_observers::CurrentLabelObserver();
    return observer && observer->begin ? observer->begin(ctx) : RealBeginLabelAdapterDraw(ctx);
}

void FinishLabelAdapterDraw(const std::shared_ptr<LabelAdapterCapture>& capture, std::uint32_t result)
{
    const auto* observer = tests::production_observers::CurrentLabelObserver();
    if (observer && observer->finish)
        observer->finish(capture, result);
    else
        RealFinishLabelAdapterDraw(capture, result);
}

void ObserveLabelAdapterPatch(std::uint32_t shader, std::uint32_t code, std::uint32_t patchState, std::uint32_t variant, std::uint64_t caller)
{
    const auto* observer = tests::production_observers::CurrentLabelObserver();
    if (observer && observer->patch)
        observer->patch(shader, code, patchState, variant, caller);
    else
        RealObserveLabelAdapterPatch(shader, code, patchState, variant, caller);
}

void ObserveLabelAdapterSelection(std::uint32_t graphics, std::uint32_t shader, std::uint32_t conditional, std::uint32_t variant)
{
    const auto* observer = tests::production_observers::CurrentLabelObserver();
    if (observer && observer->selection)
        observer->selection(graphics, shader, conditional, variant);
    else
        RealObserveLabelAdapterSelection(graphics, shader, conditional, variant);
}

} // namespace rerevved::gpu::diagnostics
#endif
