#pragma once

#include <cstdint>
#include <thread>

// One global scalar and array operator new/delete set (allocation_harness.cpp)
// serves every test in rerevved_tests. A disarmed harness is a malloc/free
// pair behind one relaxed atomic load. The scopes below arm it and always
// restore the previous state, including when a REQUIRE unwinds.
//
// Do not run Catch2 assertions, INFO or reporters while a failure or counting
// scope is armed on the calling thread: Catch2 allocates. Disarm (or leave the
// scope) first and assert on recorded results. Aligned and nothrow forms are
// not replaced; the standard nothrow forms forward to the scalar one.
namespace rerevved::tests::allocation
{

// Fails allocations by countdown: `successfulAllocations` more succeed, then
// the next one throws std::bad_alloc. Sticky keeps failing every later
// allocation until the scope ends; OneShot fails exactly one.
class ScopedCountdown
{
public:
    enum class Scope
    {
        CallingThread, // other threads allocate normally
        AllThreads,    // one countdown shared by every thread
    };
    enum class Mode
    {
        Sticky,
        OneShot,
    };

    explicit ScopedCountdown(std::int64_t successfulAllocations, Scope scope = Scope::CallingThread, Mode mode = Mode::Sticky) noexcept;
    ~ScopedCountdown();
    ScopedCountdown(const ScopedCountdown&)            = delete;
    ScopedCountdown& operator=(const ScopedCountdown&) = delete;

    // Ends the countdown early; the destructor then restores nothing further.
    void Disarm() noexcept;

private:
    struct State
    {
        std::int64_t remaining;
        bool         oneShot;
    };

    Scope scope_;
    State previous_;
    bool  armed_ = false;
};

// Fails every allocation made by one thread while the scope lives.
class ScopedThreadFailure
{
public:
    explicit ScopedThreadFailure(std::thread::id thread) noexcept;
    ~ScopedThreadFailure();
    ScopedThreadFailure(const ScopedThreadFailure&)            = delete;
    ScopedThreadFailure& operator=(const ScopedThreadFailure&) = delete;

private:
    std::thread::id previous_;
    bool            previousActive_;
};

// Counts allocations made by any thread while the scope lives.
class ScopedAllocationCount
{
public:
    ScopedAllocationCount() noexcept;
    ~ScopedAllocationCount();
    ScopedAllocationCount(const ScopedAllocationCount&)            = delete;
    ScopedAllocationCount& operator=(const ScopedAllocationCount&) = delete;

    // Allocations since construction.
    std::uint64_t Count() const noexcept;

private:
    std::uint64_t baseline_;
};

} // namespace rerevved::tests::allocation
