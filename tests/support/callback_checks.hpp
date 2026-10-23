#pragma once

#include <catch2/catch_test_macros.hpp>

#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Checks made inside callbacks that production code may call from a thread
// other than the test thread. Catch2 assertions must stay on the test thread,
// so a failed check is recorded and thrown to the caller, as the old
// standalone fixtures did; the test thread then fails the case in Verify().
namespace rerevved::tests
{

class CallbackChecks
{
public:
    void Require(bool condition, std::string_view message)
    {
        if (condition)
        {
            return;
        }
        {
            std::lock_guard lock(mutex_);
            failures_.emplace_back(message);
        }
        throw std::runtime_error(std::string(message));
    }

    // Test thread only: reports every recorded failure and clears them.
    void Verify()
    {
        std::vector<std::string> failures;
        {
            std::lock_guard lock(mutex_);
            failures.swap(failures_);
        }
        for (const auto& failure : failures)
        {
            UNSCOPED_INFO("callback check failed: " << failure);
        }
        REQUIRE(failures.empty());
    }

private:
    std::mutex               mutex_;
    std::vector<std::string> failures_;
};

} // namespace rerevved::tests
