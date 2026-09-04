#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rerevved
{

// Quick checks layout and sizes; full also hashes and diagnoses executables.
enum class ContentDepth
{
    kQuick,
    kFull,
};

struct ContentCheckResult
{
    bool                     ok = false;
    std::vector<std::string> errors;
};

// Checks required files before runtime construction and ignores unknown extras.
ContentCheckResult VerifyContentRoot(const std::filesystem::path& root, ContentDepth depth);

// Shared with the game data selector configuration in rerevved_app.cpp.
extern const std::uint32_t kTitleId;
extern const char* const   kBaseXexSha256;
extern const char* const   kUpdateXexpSha256;

} // namespace rerevved
