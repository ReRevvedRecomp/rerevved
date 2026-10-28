#include "portable_mode.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <system_error>

#include <fmt/format.h>

namespace rerevved::portable_mode
{

std::optional<std::filesystem::path> PortableRoot(const std::filesystem::path& exeFolder)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(exeFolder / kMarkerFileName, ec) || ec)
    {
        return std::nullopt;
    }
    return exeFolder / kDataFolderName;
}

std::string PrepareRoot(const std::filesystem::path& root)
{
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec)
    {
        return fmt::format("cannot create the folder: {}", ec.message());
    }
    if (!std::filesystem::is_directory(root, ec))
    {
        return "the path is not a folder";
    }

    // Probe writability with an actual write because directory attributes can be misleading.
    const auto stamp = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto probe = root / fmt::format(".rerevved_write_probe_{:x}", stamp);
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        out << 'x';
        out.flush();
        if (!out)
        {
            return "cannot write files in the folder";
        }
    }
    std::filesystem::remove(probe, ec);
    if (ec)
    {
        return fmt::format("cannot remove a test file from the folder: {}", ec.message());
    }
    return {};
}

} // namespace rerevved::portable_mode
