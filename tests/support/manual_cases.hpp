#pragma once

#include "helper_commands.hpp"

#include <string>
#include <string_view>
#include <vector>

// Manual cases are Catch2 cases hidden from discovery (tagged [.] and never
// [ctest]) that read command-line style inputs. A helper command runs one of
// them with `rerevved_tests --helper <family>.<command> [args...]`, so an
// operator-supplied capture or fixture root keeps the assertion macros of an
// ordinary case. The arguments mirror what the old standalone executable took
// after its program name.
namespace rerevved::tests
{

// The narrow, process-code-page arguments of the running manual case: argc()
// counts the arguments after the helper name, argv()[0] is the program name and
// argv()[1..argc()] are the arguments (argv()[argc() + 1] is null). The pointers
// stay valid while the case runs.
class ManualArgv
{
public:
    int argc() const noexcept
    {
        return pointers_.size() < 2 ? 0 : static_cast<int>(pointers_.size()) - 2;
    }

    char** argv() noexcept
    {
        return pointers_.data();
    }

    char* const* argv() const noexcept
    {
        return pointers_.data();
    }

    std::string_view operator[](std::size_t index) const
    {
        return storage_.at(index);
    }

private:
    friend const ManualArgv& CurrentManualArgv();
    friend int               RunManualCase(std::string_view caseName, const HelperArgs& args);

    std::vector<std::string> storage_;
    std::vector<char*>       pointers_;
};

// Arguments of the manual case being run; argc() is 0 when none were given.
const ManualArgv& CurrentManualArgv();

// Runs the Catch2 case named `caseName` with the helper arguments installed
// and returns its Catch2 exit code.
int RunManualCase(std::string_view caseName, const HelperArgs& args);

} // namespace rerevved::tests

// Registers helper `helperName` to run the hidden manual case `caseName`.
#define REREVVED_MANUAL_HELPER(identifier, helperName, caseName) \
    REREVVED_HELPER_COMMAND(identifier, helperName)              \
    {                                                            \
        return ::rerevved::tests::RunManualCase(caseName, args); \
    }
