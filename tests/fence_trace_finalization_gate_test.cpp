#include "fence_trace_finalization_gate.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

namespace
{

void require(bool condition, const char* message)
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
    std::promise<void>         winnerEntered;
    std::promise<void>         releaseWinner;
    auto                       release = releaseWinner.get_future().share();

    auto winner = std::async(std::launch::async,
                             [&]()
                             {
                                 return gate.Run(
                                     [&]()
                                     {
                                         ++calls;
                                         winnerEntered.set_value();
                                         release.wait();
                                         return true;
                                     });
                             });
    winnerEntered.get_future().wait();

    std::promise<void> loserEntered;
    auto               loser = std::async(std::launch::async,
                                          [&]()
                                          {
                                loserEntered.set_value();
                                return gate.Run(
                                    [&]()
                                    {
                                        ++calls;
                                        return true;
                                    });
                                          });
    loserEntered.get_future().wait();
    require(loser.wait_for(std::chrono::milliseconds(20)) ==
                std::future_status::timeout,
            "concurrent loser returned before finalization completed");

    releaseWinner.set_value();
    require(winner.get(), "winning finalizer failed");
    require(loser.get(), "waiting finalizer did not observe success");
    require(calls == 1, "successful finalization ran more than once");

    FenceTraceFinalizationGate retryGate;
    uint32_t                   retries = 0;
    require(!retryGate.Run(
                [&]()
                {
                    ++retries;
                    return false;
                }),
            "failed finalization reported success");
    require(retryGate.Run(
                [&]()
                {
                    ++retries;
                    return true;
                }),
            "failed finalization could not be retried");
    require(retryGate.Run(
                [&]()
                {
                    ++retries;
                    return true;
                }),
            "completed finalization was not retained");
    require(retries == 2, "completed finalization ran again");

    std::cout << "fence_trace_finalization_gate_test: PASS\n";
    return 0;
}
