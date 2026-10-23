#pragma once

#include "helper_commands.hpp"

#include <string>
#include <vector>

// The argument vector an old standalone main received, rebuilt from a helper
// command's arguments: the program name, then each argument in the platform's
// native encoding. Fixture emitters and prepared proofs keep their command-line
// parsing and their wide paths unchanged.
namespace rerevved::tests
{

class NativeArgv
{
public:
    explicit NativeArgv(const HelperArgs& args)
    {
        storage_.emplace_back(Program());
        for (const NativeChar* value : args.values)
        {
            storage_.emplace_back(value);
        }
        for (auto& value : storage_)
        {
            pointers_.push_back(value.data());
        }
        pointers_.push_back(nullptr);
    }

    int argc() const noexcept
    {
        return static_cast<int>(storage_.size());
    }

    NativeChar** argv() noexcept
    {
        return pointers_.data();
    }

private:
    static const NativeChar* Program() noexcept
    {
#if defined(_WIN32)
        return L"rerevved_tests";
#else
        return "rerevved_tests";
#endif
    }

    std::vector<std::basic_string<NativeChar>> storage_;
    std::vector<NativeChar*>                   pointers_;
};

} // namespace rerevved::tests
