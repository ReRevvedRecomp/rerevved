// The Xenos frame reference as its tests need it: the production source itself,
// plus the one function that lets each Catch2 case start a fresh session.
// Production keeps one session per process for its whole life.
#include "gpu/diagnostics/xenos_frame_reference.cpp"
#include "production_observers.hpp"

namespace rerevved::gpu::diagnostics
{

void StopAndResetXenosFrameReferenceForTests() noexcept
{
    StopXenosFrameReference();
    std::lock_guard lock(lifecycle);
    active.store(nullptr, std::memory_order_release);
    // A stopped session is never freed: a boundary notification that loaded the
    // pointer earlier may still read it.
    (void)session.release();
}

} // namespace rerevved::gpu::diagnostics
