#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rerevved
{

// Quick checks required paths and file sizes. Full also checks executable
// hashes and reports version mismatches.
enum class ContentDepth
{
    Quick,
    Full,
};

struct ContentCheckResult
{
    bool                     ok = false;
    std::vector<std::string> errors;
};

// Checks required files before runtime construction and ignores unknown extras.
ContentCheckResult VerifyContentRoot(const std::filesystem::path& root, ContentDepth depth);

// Shared with the game data selector configuration in app.cpp.
extern const std::uint32_t kTitleId;
extern const char* const   kBaseXexSha256;
extern const char* const   kUpdateXexpSha256;

} // namespace rerevved
