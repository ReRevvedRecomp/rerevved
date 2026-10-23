// Entry point of rerevved_tests. Helper commands (--helper <name> ...) run
// before Catch2 starts so fatal-child and recorder modes see the untouched
// process. Windows uses wmain: the installed GPU loader needs the wide CRT
// program path, and helpers keep their wide arguments.
#include "helper_commands.hpp"

#include <catch2/catch_session.hpp>

#include <string>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace
{

int RunCatch(std::vector<std::string>& arguments)
{
    std::vector<char*> pointers;
    pointers.reserve(arguments.size() + 1);
    for (auto& argument : arguments)
    {
        pointers.push_back(argument.data());
    }
    pointers.push_back(nullptr);
    Catch::Session session;
    return session.run(static_cast<int>(arguments.size()), pointers.data());
}

} // namespace

#if defined(_WIN32)

namespace
{

std::string ToUtf8(const wchar_t* text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    result.pop_back();
    return result;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    int exitCode = 0;
    if (rerevved::tests::TryRunHelperCommand(argc, argv, exitCode))
    {
        return exitCode;
    }
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i)
    {
        arguments.push_back(ToUtf8(argv[i]));
    }
    return RunCatch(arguments);
}

#else

int main(int argc, char** argv)
{
    int exitCode = 0;
    if (rerevved::tests::TryRunHelperCommand(argc, argv, exitCode))
    {
        return exitCode;
    }
    std::vector<std::string> arguments(argv, argv + argc);
    return RunCatch(arguments);
}

#endif
