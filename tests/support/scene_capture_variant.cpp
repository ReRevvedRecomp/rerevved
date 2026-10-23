// The scene capture session as the recorder tests need it: the production
// source itself, compiled under the renamed entry names with the test seam on.
// Production allows one session per process for the life of the title; this
// adds the one function that lets each Catch2 case start a fresh session.
#include "gpu/diagnostics/scene_resource_capture.cpp"
#include "production_observers.hpp"

namespace rerevved::gpu::diagnostics
{

void StopAndResetSceneResourceCaptureForTests() noexcept
{
    StopSceneResourceCapture();
    {
        std::lock_guard lock(lifecycle);
        session = nullptr;
        sessions.clear();
        stopped = false;
    }
    // Workers have ended; a reader a case installed must not outlive it.
    testMemoryReader = nullptr;
    ResetRouteEffectsForTests();
}

} // namespace rerevved::gpu::diagnostics
