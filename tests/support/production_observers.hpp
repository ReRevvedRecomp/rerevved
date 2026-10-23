#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <rex/ppc/context.h>

#ifndef REREVVED_DEVELOPER
#error "REREVVED_DEVELOPER must be defined to 0 or 1 by the build"
#endif

#if REREVVED_DEVELOPER
#include "gpu/diagnostics/scene_resource_capture.h"

namespace rerevved::gpu::diagnostics
{

struct LabelAdapterCapture;

// Ends any scene capture session and allows the next case to start one. The
// production session count is one per process; this is defined with the scene
// capture variant (tests/support/scene_capture_variant.cpp).
void StopAndResetSceneResourceCaptureForTests() noexcept;

// Clears the route effect recorder the scene capture session starts, which
// production keeps for the life of the process (tests/support/route_effect_variant.cpp).
void ResetRouteEffectsForTests() noexcept;

// Ends any Xenos frame reference session and allows the next case to start
// one (tests/support/xenos_frame_reference_variant.cpp).
void StopAndResetXenosFrameReferenceForTests() noexcept;

// Ends any route trace session and allows the next case to start one
// (tests/support/route_trace_variant.cpp).
void StopAndResetRouteTraceForTests() noexcept;

} // namespace rerevved::gpu::diagnostics

namespace rerevved::gpu::guest_d3d
{

// Reopens the original residue probe and clears its counts, which production
// closes for good at shutdown (tests/support/original_residue_probe_variant.cpp).
void ResetOriginalResidueProbeForTests() noexcept;

} // namespace rerevved::gpu::guest_d3d
#endif

#if REREVVED_DEVELOPER
// Observers for production seams that the GPU tests used to define for
// themselves. The production definitions (scene_resource_capture.cpp and
// label_draw_capture.cpp) compile into rerevved_tests under renamed symbols
// (Real<Name>); the definitions here take the production names and forward to
// the installed observer, or to the production code when none is installed.
//
// Scopes nest and must end in reverse order of construction. The observer must
// outlive every call that can reach it. The scene observer is called from
// noexcept hook paths on any thread: it must not throw and must not run Catch2
// assertions; record what it saw and assert after the hook returns.
namespace rerevved::tests::production_observers
{

struct SceneEntryObserver
{
    std::function<gpu::diagnostics::SceneHookToken(void* session, std::uint32_t entry, const PPCContext& ctx)> begin;
    std::function<void(gpu::diagnostics::SceneHookToken token, const PPCContext& ctx)>                         end;
};

// Replaces BeginSceneEntry / EndSceneEntry for the scope. An unset member
// forwards to the production function.
class ScopedSceneEntryObserver
{
public:
    explicit ScopedSceneEntryObserver(SceneEntryObserver* observer) noexcept;
    ~ScopedSceneEntryObserver();
    ScopedSceneEntryObserver(const ScopedSceneEntryObserver&)            = delete;
    ScopedSceneEntryObserver& operator=(const ScopedSceneEntryObserver&) = delete;

private:
    SceneEntryObserver* previous_;
};

struct LabelAdapterObserver
{
    std::function<std::shared_ptr<gpu::diagnostics::LabelAdapterCapture>(const PPCContext&)>          begin;
    std::function<void(const std::shared_ptr<gpu::diagnostics::LabelAdapterCapture>&, std::uint32_t)> finish;
    std::function<void(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint64_t)>    patch;
    std::function<void(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)>                   selection;
};

// Replaces the context-based Label adapter capture observers for the scope. An
// unset member forwards to the production function.
class ScopedLabelAdapterObserver
{
public:
    explicit ScopedLabelAdapterObserver(LabelAdapterObserver* observer) noexcept;
    ~ScopedLabelAdapterObserver();
    ScopedLabelAdapterObserver(const ScopedLabelAdapterObserver&)            = delete;
    ScopedLabelAdapterObserver& operator=(const ScopedLabelAdapterObserver&) = delete;

private:
    LabelAdapterObserver* previous_;
};

// The installed observers, or null (used by the forwarding definitions).
SceneEntryObserver*   CurrentSceneObserver() noexcept;
LabelAdapterObserver* CurrentLabelObserver() noexcept;

} // namespace rerevved::tests::production_observers
#endif
