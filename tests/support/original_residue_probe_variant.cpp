// The original residue probe as its tests need it: the production source
// itself, plus the one function that lets each Catch2 case start open and
// empty. Production closes the probe for good at shutdown.
#include "gpu/guest_d3d/original_residue_probe.cpp"
#include "production_observers.hpp"

namespace rerevved::gpu::guest_d3d
{

void ResetOriginalResidueProbeForTests() noexcept
{
    originalResidueAdmissions.store(0, std::memory_order_release);
    originalResidueProbeState.store(-1, std::memory_order_relaxed);
    (void)TakeOriginalResidueCounts();
    explicitDispatch.store(false);
    ringConsume.store(false);
    internalError.store(0);
    lateDecisions.store(0);
    reportingFailures.store(0);
    skippedFinalReports.store(0);
    std::lock_guard lock(reportMutex);
    pendingReport = {};
}

} // namespace rerevved::gpu::guest_d3d
