#pragma once

#include <filesystem>
#include <optional>
#include <string>

// Portable Mode keeps player data beside the executable. It is opt-in: an
// empty marker file next to rerevved.exe selects the userdata folder there.
namespace rerevved::portable_mode
{

inline constexpr const char* kMarkerFileName = "portable.txt";
inline constexpr const char* kDataFolderName = "userdata";

// The portable data root for an executable folder, or nullopt when the marker
// is absent. Only a regular file counts as the marker.
std::optional<std::filesystem::path> PortableRoot(const std::filesystem::path& exeFolder);

// Creates the folder if needed and proves it is writable by creating and
// removing a probe file. Returns an empty string on success, else a reason.
std::string PrepareRoot(const std::filesystem::path& root);

} // namespace rerevved::portable_mode
