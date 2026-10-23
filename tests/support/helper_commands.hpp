#pragma once

#include <span>
#include <string_view>

// Helper commands are process entry points that must run before Catch2 starts:
// fatal-child probes, recorder modes and fixture emitters. They are invoked as
//   rerevved_tests --helper <name> [args...]
// and registered once per name with REREVVED_HELPER_COMMAND. Names are
// lowercase `<family>.<command>` words, for example `tiling-restore.fault-child`.
// Registration only links a node into a list; a registrar never touches
// devices, memory or files, so static initialization stays cheap and ordered.
namespace rerevved::tests
{

#if defined(_WIN32)
// Windows keeps the wide program path and arguments the GPU loader expects.
using NativeChar = wchar_t;
#else
using NativeChar = char;
#endif

struct HelperArgs
{
    // Arguments after the helper name, in the platform's native encoding.
    std::span<const NativeChar* const> values;
};

using HelperFunction = int (*)(const HelperArgs& args);

class HelperCommand
{
public:
    HelperCommand(const char* name, HelperFunction function) noexcept;
    HelperCommand(const HelperCommand&)            = delete;
    HelperCommand& operator=(const HelperCommand&) = delete;

    const char* Name() const noexcept
    {
        return name_;
    }

    HelperFunction Function() const noexcept
    {
        return function_;
    }

    const HelperCommand* Next() const noexcept
    {
        return next_;
    }

private:
    const char*    name_;
    HelperFunction function_;
    HelperCommand* next_;
};

// Looks up a registered helper. Returns null when the name is unknown.
const HelperCommand* FindHelperCommand(std::string_view name) noexcept;

// Runs `--helper <name> [args...]` when argv selects a helper. Returns false
// when argv is an ordinary Catch2 command line; otherwise sets exitCode. An
// unknown or duplicated name sets exitCode 2 and lists the registered names.
bool TryRunHelperCommand(int argc, const NativeChar* const* argv, int& exitCode);

// Runs `--helper <name>` as a child of the current executable and returns its
// exit status, or -1 when the child could not be started. For checks that need
// a process-wide state the Catch2 session has already changed, or a fatal exit.
int RunHelperChild(const char* name);

} // namespace rerevved::tests

// Defines and registers `int identifier(const HelperArgs& args)` as helper `name`.
#define REREVVED_HELPER_COMMAND(identifier, name)                                                        \
    static int                                    identifier(const ::rerevved::tests::HelperArgs& args); \
    static const ::rerevved::tests::HelperCommand identifier##Registration{ name, &identifier };         \
    static int                                    identifier(const ::rerevved::tests::HelperArgs& args)
