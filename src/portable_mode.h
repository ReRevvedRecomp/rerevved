#pragma once

#include <filesystem>
#include <string>

// Player data lives in a userdata folder beside the executable, so deleting the
// game folder removes everything ReRevved wrote.
namespace rerevved::portable_mode
{

inline constexpr const char* kDataFolderName = "userdata";

// The default data root for an executable folder.
std::filesystem::path PortableRoot(const std::filesystem::path& exeFolder);

// Creates the folder if needed and proves it is writable by creating and
// removing a probe file. Returns an empty string on success, else a reason.
std::string PrepareRoot(const std::filesystem::path& root);

} // namespace rerevved::portable_mode
