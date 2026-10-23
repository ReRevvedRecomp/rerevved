// The route trace as its tests need it: the production source itself, plus the
// one function that lets each Catch2 case start a fresh session. Production
// keeps one session per process for its whole life.
#include "gpu/diagnostics/route_trace.cpp"
#include "production_observers.hpp"

namespace rerevved::gpu::diagnostics
{

void StopAndResetRouteTraceForTests() noexcept
{
    StopRouteTrace();
    std::lock_guard lock(lifecycle);
    routeTraceEnabled.store(0, std::memory_order_release);
    routeTraceRecorder.store(nullptr, std::memory_order_release);
    // A stopped session is never freed: a hook that loaded the recorder earlier
    // may still read it.
    session = nullptr;
}

} // namespace rerevved::gpu::diagnostics
