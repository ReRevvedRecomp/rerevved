#include "options_modal.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace om = rerevved::options_modal;

namespace
{

class FakeEnv
{
public:
    bool Alive()
    {
        ++aliveChecks;
        return alive;
    }

    bool PageOpen()
    {
        ++pageChecks;
        if (pageClosedByUs)
            return false;
        if (!openAfter.empty())
        {
            const bool open = openAfter.front();
            openAfter.pop_front();
            return open;
        }
        return true;
    }

    std::optional<bool> CallApplication(const om::Call& call)
    {
        calls.push_back(call);
        if (missingAt && calls.size() == *missingAt)
            return std::nullopt;
        if (breakAt && calls.size() == *breakAt)
            return call.slot == om::kSlotFrame ? false : true;
        return call.slot == om::kSlotFrame;
    }

    bool CancelRequested()
    {
        return cancel;
    }

    void RequestClose()
    {
        ++closes;
        if (closeFailures > 0)
        {
            --closeFailures;
            return;
        }
        pageClosedByUs = true;
    }

    void EndIteration()
    {
        ++iterations;
        if (killAfterIteration && iterations == *killAfterIteration)
            alive = false;
    }

    bool                  alive         = true;
    bool                  cancel        = false;
    int                   closeFailures = 0;
    std::deque<bool>      openAfter;
    std::optional<size_t> breakAt;
    std::optional<size_t> missingAt;
    std::optional<size_t> killAfterIteration;

    std::vector<om::Call> calls;
    int                   aliveChecks    = 0;
    int                   pageChecks     = 0;
    int                   iterations     = 0;
    int                   closes         = 0;
    bool                  pageClosedByUs = false;
};

} // namespace

TEST_CASE("options_modal_routes_only_the_validated_pause_caller", "[ctest][options-modal]")
{
    CHECK(om::ChooseRoute(0x82D068E4, 0x8314F76C, true) == om::Route::kPage);
    CHECK(om::ChooseRoute(0x82D068E4, 0x8314F76C, false) == om::Route::kRetail);
    CHECK(om::ChooseRoute(0x82D068E8, 0x8314F76C, true) == om::Route::kRetail);
    CHECK(om::ChooseRoute(0x82D068E4, 0x8314F770, true) == om::Route::kRetail);
    CHECK(om::ChooseRoute(0, 0, true) == om::Route::kRetail);
}

TEST_CASE("options_modal_loop_calls_the_runner_slots_in_order", "[ctest][options-modal]")
{
    FakeEnv env;
    env.openAfter   = { true, true, false };
    const auto exit = om::RunLoop(env);
    CHECK(exit == om::Exit::kPageClosed);
    REQUIRE(env.calls.size() == 10);
    const uint32_t slots[] = { 0x20, 0x2C, 0x24, 0x44, 0x2C };
    for (size_t i = 0; i < env.calls.size(); ++i)
        CHECK(env.calls[i].slot == slots[i % 5]);
    // Only the frame service takes r4=0, r5=1.
    CHECK(env.calls[3].r4 == 0);
    CHECK(env.calls[3].r5 == 1);
    CHECK(env.calls[0].r5 == 0);
    CHECK(env.iterations == 2);
    CHECK(env.closes == 0);
}

TEST_CASE("options_modal_loop_skips_when_the_page_is_not_open", "[ctest][options-modal]")
{
    FakeEnv env;
    env.openAfter = { false };
    CHECK(om::RunLoop(env) == om::Exit::kPageClosed);
    CHECK(env.calls.empty());
    CHECK(env.closes == 0);
}

TEST_CASE("options_modal_application_breaks_close_the_page", "[ctest][options-modal]")
{
    // Nonzero +0x20/+0x2C/+0x24 or zero frame service must break and close.
    for (const size_t index : { size_t{ 1 }, size_t{ 2 }, size_t{ 3 }, size_t{ 4 }, size_t{ 5 } })
    {
        FakeEnv env;
        env.breakAt = index;
        CHECK(om::RunLoop(env) == om::Exit::kApplicationBreak);
        CHECK(env.calls.size() == index);
        CHECK(env.closes == 1);
        CHECK(env.iterations == 0);
    }
}

TEST_CASE("options_modal_application_break_on_a_later_iteration", "[ctest][options-modal]")
{
    FakeEnv env;
    env.breakAt = 7; // second iteration, +0x2C
    CHECK(om::RunLoop(env) == om::Exit::kApplicationBreak);
    CHECK(env.iterations == 1);
    CHECK(env.closes == 1);
}

TEST_CASE("options_modal_missing_application_closes_the_page", "[ctest][options-modal]")
{
    FakeEnv env;
    env.missingAt = 1;
    CHECK(om::RunLoop(env) == om::Exit::kNoApplication);
    CHECK(env.closes == 1);

    FakeEnv later;
    later.missingAt = 4;
    CHECK(om::RunLoop(later) == om::Exit::kNoApplication);
    CHECK(later.calls.size() == 4);
    CHECK(later.closes == 1);
}

TEST_CASE("options_modal_shutdown_ends_the_loop_without_a_close", "[ctest][options-modal]")
{
    FakeEnv before;
    before.alive = false;
    CHECK(om::RunLoop(before) == om::Exit::kShutdown);
    CHECK(before.calls.empty());
    CHECK(before.closes == 0);

    FakeEnv during;
    during.killAfterIteration = 3;
    CHECK(om::RunLoop(during) == om::Exit::kShutdown);
    CHECK(during.iterations == 3);
    CHECK(during.closes == 0);
}

TEST_CASE("options_modal_shutdown_wins_over_a_closed_page", "[ctest][options-modal]")
{
    FakeEnv env;
    env.killAfterIteration = 1;
    env.openAfter          = { true, false };
    CHECK(om::RunLoop(env) == om::Exit::kShutdown);
    CHECK(env.closes == 0);
}

TEST_CASE("options_modal_session_end_breaks_through_the_application", "[ctest][options-modal]")
{
    // Session end, sign-out and external events break via application slots.
    FakeEnv env;
    env.breakAt = 2; // +0x2C
    CHECK(om::RunLoop(env) == om::Exit::kApplicationBreak);
    CHECK(env.closes == 1);
    CHECK(env.pageClosedByUs);
}

TEST_CASE("options_modal_gate_reports_open_until_closed", "[ctest][options-modal]")
{
    om::PageGate gate;
    CHECK_FALSE(gate.IsOpen(0));
    CHECK_FALSE(gate.IsOpen(1));
    const uint64_t first = gate.Opened();
    CHECK(first != 0);
    CHECK(gate.IsOpen(first));
    gate.Closed(first);
    CHECK_FALSE(gate.IsOpen(first));
    gate.Closed(first);
    CHECK_FALSE(gate.IsOpen(first));
}

TEST_CASE("options_modal_gate_ignores_a_stale_close", "[ctest][options-modal]")
{
    om::PageGate   gate;
    const uint64_t first = gate.Opened();
    gate.Closed(first);
    const uint64_t second = gate.Opened();
    CHECK(second > first);
    CHECK(gate.IsOpen(second));
    gate.Closed(first);
    CHECK(gate.IsOpen(second));
    gate.Closed(second);
    CHECK_FALSE(gate.IsOpen(second));
    CHECK_FALSE(gate.IsOpen(first));
}

TEST_CASE("options_modal_gate_shutdown_closes_every_page", "[ctest][options-modal]")
{
    om::PageGate   gate;
    const uint64_t page = gate.Opened();
    gate.Shutdown();
    CHECK_FALSE(gate.IsOpen(page));
    CHECK_FALSE(gate.IsOpen(gate.Opened()));
}

TEST_CASE("options_modal_gate_drives_the_loop_to_exit", "[ctest][options-modal]")
{
    struct GateEnv
    {
        om::PageGate& gate;
        uint64_t      page;
        int           iterations = 0;

        bool Alive()
        {
            return true;
        }

        bool PageOpen()
        {
            return gate.IsOpen(page);
        }

        std::optional<bool> CallApplication(const om::Call& call)
        {
            return call.slot == om::kSlotFrame;
        }

        bool CancelRequested()
        {
            return false;
        }

        void RequestClose()
        {
        }

        void EndIteration()
        {
            if (++iterations == 4)
                gate.Closed(page);
        }
    };

    om::PageGate gate;
    GateEnv      env{ gate, gate.Opened() };
    CHECK(om::RunLoop(env) == om::Exit::kPageClosed);
    CHECK(env.iterations == 4);

    struct DroppedEnv : GateEnv
    {
        bool alive = true;

        bool Alive()
        {
            return alive;
        }

        void EndIteration()
        {
            if (++iterations == 2)
                alive = false;
        }
    };

    om::PageGate gate2;
    DroppedEnv   dropped{ { gate2, gate2.Opened() } };
    CHECK(om::RunLoop(dropped) == om::Exit::kShutdown);
    CHECK(dropped.iterations == 2);
}

TEST_CASE("options_modal_cancel_predicate_matches_the_retail_update", "[ctest][options-modal]")
{
    // 0x82DE6B38: state object present, its word +4 is 2, status bit 0x4000.
    CHECK(om::CancelRequested(0x1000, 2, 0x4000));
    CHECK(om::CancelRequested(0x1000, 2, 0xFFFFFFFF));
    CHECK_FALSE(om::CancelRequested(0, 2, 0x4000));
    CHECK_FALSE(om::CancelRequested(0x1000, 1, 0x4000));
    CHECK_FALSE(om::CancelRequested(0x1000, 2, 0x3FFF));
    CHECK_FALSE(om::CancelRequested(0x1000, 2, 0));
}

TEST_CASE("options_modal_cancel_closes_the_page_while_slots_continue", "[ctest][options-modal]")
{
    FakeEnv env;
    env.cancel = true;
    CHECK(om::RunLoop(env) == om::Exit::kCancelled);
    CHECK(env.calls.empty());
    CHECK(env.closes == 1);
    CHECK(env.pageClosedByUs);

    struct LateCancel : FakeEnv
    {
        bool CancelRequested()
        {
            return iterations >= 2;
        }
    };

    LateCancel late;
    CHECK(om::RunLoop(late) == om::Exit::kCancelled);
    CHECK(late.iterations == 2);
    CHECK(late.calls.size() == 10);
    CHECK(late.closes == 1);
}

TEST_CASE("options_modal_close_retries_until_the_page_is_gone", "[ctest][options-modal]")
{
    FakeEnv env;
    env.breakAt       = 1;
    env.closeFailures = 3;
    CHECK(om::RunLoop(env) == om::Exit::kApplicationBreak);
    CHECK(env.closes == 4);
    CHECK(env.pageClosedByUs);
}

TEST_CASE("options_modal_close_retry_stops_at_shutdown", "[ctest][options-modal]")
{
    struct Stuck : FakeEnv
    {
        void RequestClose()
        {
            ++closes;
            if (closes == 5)
                alive = false;
        }
    };

    Stuck env;
    env.breakAt = 1;
    CHECK(om::RunLoop(env) == om::Exit::kShutdown);
    CHECK(env.closes == 5);
    CHECK_FALSE(env.pageClosedByUs);
}
