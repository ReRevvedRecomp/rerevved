#include "xenos_frame_counter.h"

#include <algorithm>
#include <utility>

namespace rerevved::xenos_frames
{

Counter::Counter(bool enabled, Clock clock, void* clockContext)
: enabled(enabled)
, clock(clock)
, clockContext(clockContext)
{
    if (enabled)
    {
        current.startNs = clock(clockContext);
        summary.startNs = current.startNs;
        summary.endNs   = current.startNs;
    }
}

void Counter::Attempt()
{
    if (!enabled)
    {
        return;
    }
    advance(clock(clockContext));
    ++current.attempts;
}

void Counter::Return(uint32_t decodedInterval)
{
    if (!enabled)
    {
        return;
    }
    const int64_t now = clock(clockContext);
    advance(now);
    ++current.returns;
    ++returnSequence;
    ++current.intervals[decodedInterval < 3 ? decodedInterval : 3];
    if (hasPreviousReturn)
    {
        const int64_t dt = now - previousReturnNs;
        dtMinNs          = dtCount == 0 ? dt : std::min(dtMinNs, dt);
        dtMaxNs          = std::max(dtMaxNs, dt);
        dtSumNs += dt;
        ++dtCount;
    }
    previousReturnNs  = now;
    hasPreviousReturn = true;
}

void Counter::Poll()
{
    if (enabled)
    {
        advance(clock(clockContext));
    }
}

void Counter::Finish()
{
    if (!enabled)
    {
        return;
    }
    const int64_t now = clock(clockContext);
    advance(now);
    if (now > current.startNs || current.attempts || current.returns)
    {
        emit(now, true);
    }
    summary.endNs                = now;
    summary.totalReturns         = returnSequence;
    const int64_t duration       = now - summary.startNs;
    summary.meanReturnsPerSecond = duration > 0
                                       ? static_cast<double>(returnSequence) * kSecondNs / duration
                                       : 0.0;
    enabled                      = false;
}

std::vector<Bin> Counter::TakeBins()
{
    return std::exchange(completed, {});
}

const Summary& Counter::GetSummary() const
{
    return summary;
}

void Counter::advance(int64_t now)
{
    while (now - current.startNs >= kSecondNs)
    {
        emit(current.startNs + kSecondNs, false);
    }
}

void Counter::emit(int64_t end, bool partial)
{
    current.endNs            = end;
    current.totalReturns     = returnSequence;
    current.partial          = partial;
    const int64_t duration   = end - current.startNs;
    current.returnsPerSecond = duration > 0
                                   ? static_cast<double>(current.returns) * kSecondNs / duration
                                   : 0.0;
    if (dtCount)
    {
        current.dtMinMs  = static_cast<double>(dtMinNs) / 1'000'000;
        current.dtMeanMs = static_cast<double>(dtSumNs) / dtCount / 1'000'000;
        current.dtMaxMs  = static_cast<double>(dtMaxNs) / 1'000'000;
    }
    if (!partial)
    {
        summary.minReturnsPerSecond = summary.fullBins == 0
                                          ? current.returnsPerSecond
                                          : std::min(summary.minReturnsPerSecond, current.returnsPerSecond);
        summary.maxReturnsPerSecond = std::max(summary.maxReturnsPerSecond, current.returnsPerSecond);
        ++summary.fullBins;
    }
    completed.push_back(current);
    current         = {};
    current.startNs = end;
    dtCount         = 0;
    dtMinNs = dtSumNs = dtMaxNs = 0;
}

} // namespace rerevved::xenos_frames
