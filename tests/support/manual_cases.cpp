#include "manual_cases.hpp"

#include <catch2/catch_session.hpp>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace rerevved::tests
{
namespace
{

ManualArgv current;

std::string ToNarrow(const NativeChar* text)
{
#if defined(_WIN32)
    // The old standalone mains took the narrow code-page argv; keep that
    // conversion so path arguments behave as before.
    const int length = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_ACP, 0, text, -1, result.data(), length, nullptr, nullptr);
    result.pop_back();
    return result;
#else
    return text;
#endif
}

} // namespace

const ManualArgv& CurrentManualArgv()
{
    return current;
}

int RunManualCase(std::string_view caseName, const HelperArgs& args)
{
    current.storage_.clear();
    current.pointers_.clear();
    current.storage_.emplace_back("rerevved_tests");
    for (const NativeChar* value : args.values)
    {
        current.storage_.push_back(ToNarrow(value));
    }
    for (auto& value : current.storage_)
    {
        current.pointers_.push_back(value.data());
    }
    current.pointers_.push_back(nullptr);

    const std::string name(caseName);
    const char*       arguments[] = { "rerevved_tests", name.c_str(), nullptr };
    Catch::Session    session;
    return session.run(2, arguments);
}

} // namespace rerevved::tests
