#include "fence_trace_finalization_gate.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

namespace
{

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "fence_trace_finalization_gate_test: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    using rerevved::diagnostics::FenceTraceFinalizationGate;

    FenceTraceFinalizationGate gate;
    std::atomic_uint32_t       calls = 0;
    std::promise<void>         winner_entered;
    std::promise<void>         release_winner;
    auto                       release = release_winner.get_future().share();

    auto winner = std::async(std::launch::async,
                             [&]()
                             {
                                 return gate.Run(
                                     [&]()
                                     {
                                         ++calls;
                                         winner_entered.set_value();
                                         release.wait();
                                         return true;
                                     });
                             });
    winner_entered.get_future().wait();

    std::promise<void> loser_entered;
    auto               loser = std::async(std::launch::async,
                                          [&]()
                                          {
                                loser_entered.set_value();
                                return gate.Run(
                                    [&]()
                                    {
                                        ++calls;
                                        return true;
                                    });
                                          });
    loser_entered.get_future().wait();
    Require(loser.wait_for(std::chrono::milliseconds(20)) ==
                std::future_status::timeout,
            "concurrent loser returned before finalization completed");

    release_winner.set_value();
    Require(winner.get(), "winning finalizer failed");
    Require(loser.get(), "waiting finalizer did not observe success");
    Require(calls == 1, "successful finalization ran more than once");

    FenceTraceFinalizationGate retry_gate;
    uint32_t                   retries = 0;
    Require(!retry_gate.Run(
                [&]()
                {
                    ++retries;
                    return false;
                }),
            "failed finalization reported success");
    Require(retry_gate.Run(
                [&]()
                {
                    ++retries;
                    return true;
                }),
            "failed finalization could not be retried");
    Require(retry_gate.Run(
                [&]()
                {
                    ++retries;
                    return true;
                }),
            "completed finalization was not retained");
    Require(retries == 2, "completed finalization ran again");

    std::cout << "fence_trace_finalization_gate_test: PASS\n";
    return 0;
}
