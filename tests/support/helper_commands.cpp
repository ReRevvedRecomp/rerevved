#include "helper_commands.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <cstdlib>
#include <filesystem>
#include <sys/wait.h>
#endif

namespace rerevved::tests
{
namespace
{

// Constant-initialized, so registrars in any translation unit may link in
// before dynamic initialization starts.
constinit HelperCommand* head = nullptr;

bool MatchesAscii(const NativeChar* text, std::string_view ascii) noexcept
{
    for (const char expected : ascii)
    {
        if (*text == NativeChar{} || *text != static_cast<NativeChar>(expected))
        {
            return false;
        }
        ++text;
    }
    return *text == NativeChar{};
}

std::string ToAscii(const NativeChar* text)
{
    std::string result;
    for (; *text != NativeChar{}; ++text)
    {
        result.push_back(*text >= 0 && *text < 0x80 ? static_cast<char>(*text) : '?');
    }
    return result;
}

std::vector<std::string> RegisteredNames()
{
    std::vector<std::string> names;
    for (const HelperCommand* command = head; command; command = command->Next())
    {
        names.emplace_back(command->Name());
    }
    std::sort(names.begin(), names.end());
    return names;
}

void PrintNames(std::FILE* stream)
{
    for (const auto& name : RegisteredNames())
    {
        std::fprintf(stream, "%s\n", name.c_str());
    }
}

} // namespace

HelperCommand::HelperCommand(const char* name, HelperFunction function) noexcept
: name_(name)
, function_(function)
, next_(head)
{
    head = this;
}

const HelperCommand* FindHelperCommand(std::string_view name) noexcept
{
    const HelperCommand* found = nullptr;
    for (const HelperCommand* command = head; command; command = command->Next())
    {
        if (name == command->Name())
        {
            if (found)
            {
                return nullptr;
            }
            found = command;
        }
    }
    return found;
}

bool TryRunHelperCommand(int argc, const NativeChar* const* argv, int& exitCode)
{
    if (argc < 2 || !MatchesAscii(argv[1], "--helper"))
    {
        return false;
    }
    if (argc < 3)
    {
        std::fprintf(stderr, "rerevved_tests --helper <name> [args...]; registered helpers:\n");
        PrintNames(stderr);
        exitCode = 2;
        return true;
    }
    const std::string name = ToAscii(argv[2]);
    if (name == "list")
    {
        PrintNames(stdout);
        exitCode = 0;
        return true;
    }
    const auto names = RegisteredNames();
    const auto count = std::count(names.begin(), names.end(), name);
    if (count != 1)
    {
        std::fprintf(stderr, "helper '%s' is %s; registered helpers:\n", name.c_str(), count == 0 ? "not registered" : "registered twice");
        PrintNames(stderr);
        exitCode = 2;
        return true;
    }
    const HelperCommand* command = FindHelperCommand(name);
    const HelperArgs     args{ std::span<const NativeChar* const>(argv + 3, static_cast<std::size_t>(argc - 3)) };
    exitCode = command->Function()(args);
    return true;
}

int RunHelperChild(const char* name)
{
#if defined(_WIN32)
    std::wstring executable(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (length == 0)
        {
            return -1;
        }
        if (length < executable.size())
        {
            executable.resize(length);
            break;
        }
        executable.resize(executable.size() * 2);
    }
    const std::wstring helper(name, name + std::char_traits<char>::length(name));
    return static_cast<int>(_wspawnl(_P_WAIT, executable.c_str(), executable.c_str(), L"--helper", helper.c_str(), nullptr));
#else
    const auto        executable = std::filesystem::read_symlink("/proc/self/exe").string();
    const std::string command    = std::string("\"") + executable + "\" --helper " + name;
    const int         status     = std::system(command.c_str());
    return status == -1 || !WIFEXITED(status) ? -1 : WEXITSTATUS(status);
#endif
}

} // namespace rerevved::tests
