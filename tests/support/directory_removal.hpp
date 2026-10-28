#pragma once

#include <filesystem>
#include <system_error>
#include <utility>

namespace rerevved::tests
{

// Removes a case's temporary directory tree when the scope ends, including
// when an assertion throws. An empty path removes nothing, so a case writing
// to a caller-supplied output directory can keep it.
class ScopedDirectoryRemoval
{
public:
    explicit ScopedDirectoryRemoval(std::filesystem::path path)
    : path_(std::move(path))
    {
    }

    ~ScopedDirectoryRemoval()
    {
        if (path_.empty())
            return;
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    ScopedDirectoryRemoval(const ScopedDirectoryRemoval&)            = delete;
    ScopedDirectoryRemoval& operator=(const ScopedDirectoryRemoval&) = delete;

private:
    std::filesystem::path path_;
};

} // namespace rerevved::tests
