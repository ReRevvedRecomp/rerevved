// The route effect recorder as the scene capture tests need it: the production
// source itself, plus the one function that lets each Catch2 case start a fresh
// recorder. Production keeps the first recorder for the life of the process.
#include "gpu/diagnostics/route_effect_trace.cpp"

namespace rerevved::gpu::diagnostics
{

void ResetRouteEffectsForTests() noexcept
{
    std::lock_guard lock(lifecycle);
    current.store(nullptr, std::memory_order_release);
    owner = nullptr;
    outputDirectory.clear();
    finishError.clear();
}

} // namespace rerevved::gpu::diagnostics
