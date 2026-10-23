#include "xenos_frame_counter.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace
{

using rerevved::xenos_frames::Counter;
using rerevved::xenos_frames::kSecondNs;

bool near(double actual, double expected)
{
    return std::abs(actual - expected) < 0.00001;
}

struct Clock
{
    int64_t  now = 123 * kSecondNs;
    uint64_t reads{};

    static int64_t Read(void* context)
    {
        auto& clock = *static_cast<Clock*>(context);
        ++clock.reads;
        return clock.now;
    }
};

void steadyStream(int rate, uint32_t interval)
{
    Clock         clock;
    const int64_t start = clock.now;
    Counter       counter(true, Clock::Read, &clock);
    for (int index = 0; index < 2 * rate; ++index)
    {
        clock.now = start + (2LL * index + 1) * kSecondNs / (2 * rate);
        counter.Attempt();
        counter.Return(interval);
    }
    clock.now = start + 2 * kSecondNs;
    counter.Finish();
    const auto bins = counter.TakeBins();
    UNSCOPED_INFO("steady stream emits exactly two full bins");
    CHECK(bins.size() == 2);
    if (bins.size() != 2)
    {
        return;
    }
    for (std::size_t index = 0; index < bins.size(); ++index)
    {
        const auto& bin = bins[index];
        UNSCOPED_INFO("steady stream uses host-clock one-second boundaries");
        CHECK((bin.startNs == start + static_cast<int64_t>(index) * kSecondNs &&
               bin.endNs - bin.startNs == kSecondNs && !bin.partial));
        UNSCOPED_INFO("steady stream counts attempts, returns, and the dedicated sequence");
        CHECK((bin.attempts == static_cast<uint64_t>(rate) && bin.returns == bin.attempts &&
               bin.totalReturns == (index + 1) * rate));
        UNSCOPED_INFO("steady 60 or 30 Hz rates and gap statistics");
        CHECK((near(bin.returnsPerSecond, rate) &&
               near(bin.dtMinMs, 1000.0 / rate) &&
               near(bin.dtMeanMs, 1000.0 / rate) &&
               near(bin.dtMaxMs, 1000.0 / rate)));
        UNSCOPED_INFO("steady stream retains the decoded interval in every bin");
        CHECK(bin.intervals[interval] == bin.returns);
    }
    const auto& summary = counter.GetSummary();
    UNSCOPED_INFO("steady stream summary describes VdSwap returns per second");
    CHECK((summary.totalReturns == static_cast<uint64_t>(2 * rate) && summary.fullBins == 2 &&
           near(summary.meanReturnsPerSecond, rate) &&
           near(summary.minReturnsPerSecond, rate) && near(summary.maxReturnsPerSecond, rate)));
}

void stallAndPartial()
{
    Clock         clock;
    const int64_t start = clock.now;
    Counter       counter(true, Clock::Read, &clock);
    clock.now += kSecondNs / 4;
    counter.Attempt();
    counter.Return(1);
    clock.now = start + 3 * kSecondNs + kSecondNs / 10;
    counter.Poll();
    const auto bins = counter.TakeBins();
    UNSCOPED_INFO("a reporter poll emits elapsed bins during a guest stall");
    CHECK(bins.size() == 3);
    if (bins.size() == 3)
    {
        UNSCOPED_INFO("stall bins preserve sequence and contain zero rates and gap statistics");
        CHECK((bins[0].returns == 1 && bins[1].returns == 0 && bins[2].returns == 0 &&
               bins[1].attempts == 0 && bins[2].totalReturns == 1 &&
               bins[1].returnsPerSecond == 0 && bins[1].dtMeanMs == 0));
    }
    clock.now = start + 3 * kSecondNs + kSecondNs / 2;
    counter.Attempt();
    counter.Return(2);
    clock.now = start + 3 * kSecondNs + 3 * kSecondNs / 5;
    counter.Attempt();
    clock.now = start + 3 * kSecondNs + 3 * kSecondNs / 4;
    counter.Finish();
    const auto partial = counter.TakeBins();
    UNSCOPED_INFO("finalization emits one partial bin");
    CHECK(partial.size() == 1);
    if (partial.size() == 1)
    {
        const auto& bin = partial[0];
        UNSCOPED_INFO("partial bin uses its actual duration and retains an attempt still in flight");
        CHECK((bin.partial && bin.startNs == start + 3 * kSecondNs && bin.endNs == clock.now &&
               bin.returns == 1 && bin.attempts == 2 && bin.totalReturns == 2 &&
               near(bin.returnsPerSecond, 1.0 / 0.75)));
        UNSCOPED_INFO("the return after a stall carries its full gap and decoded interval");
        CHECK((near(bin.dtMinMs, 3250) && near(bin.dtMeanMs, 3250) && near(bin.dtMaxMs, 3250) &&
               bin.intervals[2] == 1));
    }
    const auto summary = counter.GetSummary();
    UNSCOPED_INFO("summary includes empty seconds and excludes the partial bin from one-second extrema");
    CHECK((summary.startNs == start && summary.endNs == clock.now && summary.totalReturns == 2 &&
           summary.fullBins == 3 && near(summary.meanReturnsPerSecond, 2.0 / 3.75) &&
           summary.minReturnsPerSecond == 0 && summary.maxReturnsPerSecond == 1));
    const uint64_t reads = clock.reads;
    counter.Finish();
    counter.Poll();
    counter.Attempt();
    counter.Return(0);
    UNSCOPED_INFO("repeated exits and late hooks cannot flush again or read the clock");
    CHECK((clock.reads == reads && counter.TakeBins().empty() && counter.GetSummary().totalReturns == 2));
}

void boundaryAndDistribution()
{
    Clock         clock;
    const int64_t start = clock.now;
    Counter       counter(true, Clock::Read, &clock);
    clock.now = start + kSecondNs / 2;
    counter.Attempt();
    counter.Return(0);
    clock.now = start + 9 * kSecondNs / 10;
    counter.Attempt();
    clock.now = start + kSecondNs;
    counter.Return(1);
    clock.now = start + 5 * kSecondNs / 4;
    counter.Attempt();
    counter.Return(2);
    clock.now = start + 3 * kSecondNs / 2;
    counter.Attempt();
    counter.Return(3);
    clock.now = start + 7 * kSecondNs / 4;
    counter.Attempt();
    counter.Return(rerevved::xenos_frames::kUnknownInterval);
    clock.now = start + 2 * kSecondNs;
    counter.Finish();
    const auto bins = counter.TakeBins();
    UNSCOPED_INFO("boundary stream emits two bins");
    CHECK(bins.size() == 2);
    if (bins.size() == 2)
    {
        UNSCOPED_INFO("attempts and returns straddling an exact boundary belong to their own bins");
        CHECK((bins[0].attempts == 2 && bins[0].returns == 1 && bins[0].intervals[0] == 1 &&
               bins[1].attempts == 3 && bins[1].returns == 4 && bins[1].totalReturns == 5));
        UNSCOPED_INFO("distribution separates immediate, one, two, other and unobserved policies");
        CHECK((bins[1].intervals[0] == 0 && bins[1].intervals[1] == 1 &&
               bins[1].intervals[2] == 1 && bins[1].intervals[3] == 2));
        UNSCOPED_INFO("inter-return statistics carry across the boundary without inventing a first gap");
        CHECK((bins[0].dtMinMs == 0 && bins[0].dtMeanMs == 0 && bins[0].dtMaxMs == 0 &&
               near(bins[1].dtMinMs, 250) && near(bins[1].dtMeanMs, 312.5) &&
               near(bins[1].dtMaxMs, 500)));
    }
}

void disabled()
{
    Clock   clock;
    Counter counter(false, Clock::Read, &clock);
    counter.Attempt();
    counter.Return(1);
    clock.now += 10 * kSecondNs;
    counter.Poll();
    counter.Finish();
    counter.Finish();
    UNSCOPED_INFO("disabled counter never calls its clock or produces records");
    CHECK((clock.reads == 0 && counter.TakeBins().empty() && counter.GetSummary().totalReturns == 0));
}

} // namespace

TEST_CASE("xenos_frame_counter", "[ctest][xenos-frame-counter]")
{
    steadyStream(60, 1);
    steadyStream(30, 2);
    stallAndPartial();
    boundaryAndDistribution();
    disabled();
}
