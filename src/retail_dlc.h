#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "map_packs.h"

namespace rerevved::retail_dlc
{

inline constexpr std::string_view kDlcDirectory    = "dlc";
inline constexpr std::string_view kTokenFileName   = "name.txt";
inline constexpr std::string_view kArchiveName     = "dlc.fpk";
inline constexpr size_t           kMaxPackageBytes = 256u * 1024u * 1024u;
inline constexpr size_t           kMaxTokenBytes   = 64;

inline constexpr map_packs::ArchiveRules kRetailRules{ 0, map_packs::kFirstPackSlot - 1, 0, map_packs::kFirstScenarioId - 1, false };

// The token in a package's name.txt, or empty when the text is not one printable word.
std::string ParseToken(std::string_view text);

// Empty when a file name can name XContent, otherwise why it cannot.
std::string CheckFileName(std::string_view name);

struct Candidate
{
    std::string         file;
    std::string         token;
    map_packs::Contents contents;
};

struct Decision
{
    std::string file;
    bool        accepted = false;
    std::string reason;
};

std::vector<Decision> SelectPackages(std::span<const Candidate> candidates);

// Must run before the guest main thread starts.
void RegisterFolder(const std::filesystem::path& folder);

} // namespace rerevved::retail_dlc
