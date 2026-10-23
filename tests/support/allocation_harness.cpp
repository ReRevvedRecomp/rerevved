#include "allocation_harness.hpp"

#include "helper_commands.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace rerevved::tests::allocation
{
namespace
{

// Non-zero while any scope is armed, so a plain allocation pays one load.
std::atomic<unsigned> armedScopes{ 0 };

struct Countdown
{
    std::int64_t remaining = -1;
    bool         oneShot   = false;
};

thread_local Countdown threadCountdown;

std::atomic<std::int64_t> globalRemaining{ -1 };
std::atomic<bool>         globalOneShot{ false };

std::atomic<bool>            failThreadActive{ false };
std::atomic<std::thread::id> failThread{};

std::atomic<unsigned>      countingScopes{ 0 };
std::atomic<std::uint64_t> allocationCount{ 0 };

bool GlobalCountdownFires() noexcept
{
    for (auto remaining = globalRemaining.load(); remaining >= 0;)
    {
        if (remaining == 0)
        {
            if (globalOneShot.load() && !globalRemaining.compare_exchange_weak(remaining, -1))
            {
                continue;
            }
            return true;
        }
        if (globalRemaining.compare_exchange_weak(remaining, remaining - 1))
        {
            return false;
        }
    }
    return false;
}

void Admit()
{
    if (armedScopes.load(std::memory_order_relaxed) == 0)
    {
        return;
    }
    if (countingScopes.load(std::memory_order_relaxed) != 0)
    {
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    if (threadCountdown.remaining == 0)
    {
        if (threadCountdown.oneShot)
        {
            threadCountdown.remaining = -1;
        }
        throw std::bad_alloc();
    }
    if (threadCountdown.remaining > 0)
    {
        --threadCountdown.remaining;
    }
    if (GlobalCountdownFires())
    {
        throw std::bad_alloc();
    }
    if (failThreadActive.load(std::memory_order_relaxed) && failThread.load(std::memory_order_relaxed) == std::this_thread::get_id())
    {
        throw std::bad_alloc();
    }
}

void* Allocate(std::size_t size)
{
    Admit();
    if (void* memory = std::malloc(size ? size : 1))
    {
        return memory;
    }
    throw std::bad_alloc();
}

} // namespace

ScopedCountdown::ScopedCountdown(std::int64_t successfulAllocations, Scope scope, Mode mode) noexcept
: scope_(scope)
, armed_(true)
{
    if (scope == Scope::CallingThread)
    {
        previous_                 = { threadCountdown.remaining, threadCountdown.oneShot };
        threadCountdown.oneShot   = mode == Mode::OneShot;
        threadCountdown.remaining = successfulAllocations;
    }
    else
    {
        previous_ = { globalRemaining.load(), globalOneShot.load() };
        globalOneShot.store(mode == Mode::OneShot);
        globalRemaining.store(successfulAllocations);
    }
    armedScopes.fetch_add(1);
}

ScopedCountdown::~ScopedCountdown()
{
    Disarm();
}

void ScopedCountdown::Disarm() noexcept
{
    if (!armed_)
    {
        return;
    }
    armed_ = false;
    if (scope_ == Scope::CallingThread)
    {
        threadCountdown = { previous_.remaining, previous_.oneShot };
    }
    else
    {
        globalRemaining.store(previous_.remaining);
        globalOneShot.store(previous_.oneShot);
    }
    armedScopes.fetch_sub(1);
}

ScopedThreadFailure::ScopedThreadFailure(std::thread::id thread) noexcept
: previous_(failThread.load())
, previousActive_(failThreadActive.load())
{
    failThread.store(thread);
    failThreadActive.store(true);
    armedScopes.fetch_add(1);
}

ScopedThreadFailure::~ScopedThreadFailure()
{
    failThreadActive.store(previousActive_);
    failThread.store(previous_);
    armedScopes.fetch_sub(1);
}

ScopedAllocationCount::ScopedAllocationCount() noexcept
{
    countingScopes.fetch_add(1);
    armedScopes.fetch_add(1);
    baseline_ = allocationCount.load();
}

ScopedAllocationCount::~ScopedAllocationCount()
{
    armedScopes.fetch_sub(1);
    countingScopes.fetch_sub(1);
}

std::uint64_t ScopedAllocationCount::Count() const noexcept
{
    return allocationCount.load() - baseline_;
}

} // namespace rerevved::tests::allocation

void* operator new(std::size_t size)
{
    return rerevved::tests::allocation::Allocate(size);
}

void* operator new[](std::size_t size)
{
    return rerevved::tests::allocation::Allocate(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{

// Exercises each control without Catch2, which allocates. Run as
// `rerevved_tests --helper support.allocation-selfcheck`.
REREVVED_HELPER_COMMAND(allocationSelfCheck, "support.allocation-selfcheck")
{
    using namespace rerevved::tests::allocation;
    (void)args;
    int  failures = 0;
    auto expect   = [&](bool condition, const char* what)
    {
        if (!condition)
        {
            std::fprintf(stderr, "allocation self-check failed: %s\n", what);
            ++failures;
        }
    };
    // The pointer escapes through a volatile so the compiler cannot elide the pair.
    static int* volatile escaped;
    auto attempt = []() -> bool
    {
        try
        {
            int* memory = new int(0);
            escaped     = memory;
            delete memory;
            return true;
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
    };
    {
        ScopedCountdown countdown(2);
        const bool      first  = attempt();
        const bool      second = attempt();
        const bool      third  = attempt();
        const bool      fourth = attempt();
        countdown.Disarm();
        expect(first && second && !third && !fourth, "sticky thread countdown fails every allocation after the budget");
        expect(attempt(), "disarm restores allocation");
    }
    {
        ScopedCountdown countdown(1, ScopedCountdown::Scope::AllThreads, ScopedCountdown::Mode::OneShot);
        const bool      first  = attempt();
        const bool      second = attempt();
        const bool      third  = attempt();
        countdown.Disarm();
        expect(first && !second && third, "one-shot global countdown fails exactly one allocation");
    }
    {
        // The worker exists before the scope arms: creating a std::thread
        // allocates on the creating thread, which the scope would fail.
        std::atomic<bool> go{ false };
        bool              other = false;
        std::thread       worker([&]
                                 {
                               while (!go.load())
                               {
                                   std::this_thread::yield();
                               }
                               other = attempt();
                                 });
        bool              failed = false;
        {
            ScopedThreadFailure failure(std::this_thread::get_id());
            failed = !attempt();
            go.store(true);
        }
        worker.join();
        expect(failed, "selected thread fails");
        expect(other, "other threads allocate");
    }
    {
        ScopedAllocationCount count;
        int*                  scalar = new int(0);
        char*                 array  = new char[8];
        escaped                      = scalar;
        escaped                      = reinterpret_cast<int*>(array);
        delete scalar;
        delete[] array;
        expect(count.Count() >= 2, "counting sees scalar and array allocations");
    }
    expect(attempt(), "all scopes restore allocation");
    return failures == 0 ? 0 : 1;
}

} // namespace
