// Gives every Catch2 case the process state it started with. The converted
// guest-route tests set hook switches directly; this puts each changed console
// variable back when the case ends, however it ended, and ends the capture
// sessions production allows once per process, so cases cannot reach each other
// through process-wide state.
#include "production_observers.hpp"

#include <rex/cvar.h>

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <string>
#include <utility>
#include <vector>

namespace
{

class CvarIsolation final : public Catch::EventListenerBase
{
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testCaseStarting(const Catch::TestCaseInfo&) override
    {
        resetProcessState();
        saved_.clear();
        for (auto& name : rex::cvar::ListFlags())
        {
            const auto* info = rex::cvar::GetFlagInfo(name);
            if (!info || info->type == rex::cvar::FlagType::Command || !info->getter)
                continue;
            auto value = info->getter();
            saved_.emplace_back(std::move(name), std::move(value));
        }
    }

    void testCaseEnded(const Catch::TestCaseStats&) override
    {
        resetProcessState();
        for (const auto& [name, value] : saved_)
        {
            const auto* info = rex::cvar::GetFlagInfo(name);
            if (info && info->getter && info->getter() != value)
                rex::cvar::SetFlagByName(name, value);
        }
        saved_.clear();
    }

private:
    // Production keeps these sessions and counters for the life of the process;
    // the test builds of those sources can end them between cases.
    static void resetProcessState() noexcept
    {
#if REREVVED_DEVELOPER
        rerevved::gpu::diagnostics::StopAndResetSceneResourceCaptureForTests();
        rerevved::gpu::diagnostics::StopAndResetXenosFrameReferenceForTests();
        rerevved::gpu::diagnostics::StopAndResetRouteTraceForTests();
        rerevved::gpu::guest_d3d::ResetOriginalResidueProbeForTests();
#endif
    }

    std::vector<std::pair<std::string, std::string>> saved_;
};

} // namespace

CATCH_REGISTER_LISTENER(CvarIsolation)
