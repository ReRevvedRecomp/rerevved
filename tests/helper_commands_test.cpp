#include "helper_commands.hpp"

#include <catch2/catch_test_macros.hpp>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace rerevved::tests;

int                                        calls = 0;
std::vector<std::basic_string<NativeChar>> received;

int record(const HelperArgs& args)
{
    ++calls;
    received.assign(args.values.begin(), args.values.end());
    return 37;
}

std::pair<bool, int> dispatch(std::initializer_list<std::string_view> words)
{
    std::vector<std::basic_string<NativeChar>> storage;
    for (const auto word : words)
    {
        storage.emplace_back(word.begin(), word.end());
    }
    std::vector<const NativeChar*> argv;
    for (const auto& word : storage)
    {
        argv.push_back(word.c_str());
    }
    int        exitCode = 91;
    const bool handled  = TryRunHelperCommand(static_cast<int>(argv.size()), argv.data(), exitCode);
    return { handled, exitCode };
}

const HelperCommand command{ "helper-dispatch.record", record };

// Duplicate registrations stay in a child so they cannot pollute other cases.
REREVVED_HELPER_COMMAND(checkDuplicate, "helper-dispatch.duplicate")
{
    (void)args;
    static const HelperCommand first{ "helper-dispatch.ambiguous", record };
    static const HelperCommand second{ "helper-dispatch.ambiguous", record };
    const auto                 result = dispatch({ "tests", "--helper", "helper-dispatch.ambiguous" });
    return result == std::pair{ true, 2 } && calls == 0 &&
                   FindHelperCommand("helper-dispatch.ambiguous") == nullptr
               ? 0
               : 1;
}

} // namespace

TEST_CASE("helper_commands_dispatch", "[ctest][helper-commands]")
{
    calls = 0;
    received.clear();

    struct Case
    {
        std::initializer_list<std::string_view> words;
        bool                                    handled;
        int                                     exitCode;
    };

    const Case cases[] = {
        { { "tests" }, false, 91 },
        { { "tests", "--list-tests" }, false, 91 },
        { { "tests", "--helper" }, true, 2 },
        { { "tests", "--helper", "helper-dispatch.unknown" }, true, 2 },
        { { "tests", "--helper", "list" }, true, 0 },
    };
    for (const auto& test : cases)
    {
        REQUIRE((dispatch(test.words) == std::pair{ test.handled, test.exitCode }));
    }
    REQUIRE(calls == 0);
    REQUIRE(FindHelperCommand("helper-dispatch.unknown") == nullptr);
    REQUIRE(FindHelperCommand("helper-dispatch.record") == &command);
    REQUIRE((dispatch({ "tests", "--helper", "helper-dispatch.record", "payload" }) == std::pair{ true, 37 }));
    REQUIRE(calls == 1);
    const std::string_view payload = "payload";
    REQUIRE(received.size() == 1);
    REQUIRE(received.front() == std::basic_string<NativeChar>(payload.begin(), payload.end()));
}

TEST_CASE("helper_commands_duplicate", "[ctest][helper-commands]")
{
    REQUIRE(rerevved::tests::RunHelperChild("helper-dispatch.duplicate") == 0);
}
