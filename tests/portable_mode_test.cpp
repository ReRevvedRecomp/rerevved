#include "portable_mode.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{

namespace pm = rerevved::portable_mode;
namespace fs = std::filesystem;

// A unique temp folder that is removed with the fixture.
struct TempDir
{
    fs::path path;

    TempDir()
    {
        static std::atomic<int> counter{ 0 };
        const auto              stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path                          = fs::temp_directory_path() / ("rerevved_portable_test_" + std::to_string(stamp) + "_" +
                                                                     std::to_string(counter++));
        fs::create_directories(path);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("portable_mode_uses_userdata_beside_the_executable", "[ctest][portable-mode]")
{
    TempDir dir;
    CHECK(pm::PortableRoot(dir.path) == dir.path / "userdata");
}

TEST_CASE("portable_mode_prepare_root", "[ctest][portable-mode]")
{
    TempDir dir;

    UNSCOPED_INFO("a writable target is created and the probe file is removed");
    const auto root = dir.path / "userdata";
    CHECK(pm::PrepareRoot(root).empty());
    CHECK(fs::is_directory(root));
    CHECK(fs::is_empty(root));

    UNSCOPED_INFO("a target below a regular file cannot be created");
    std::ofstream(dir.path / "blocker").close();
    CHECK(!pm::PrepareRoot(dir.path / "blocker" / "userdata").empty());
}
