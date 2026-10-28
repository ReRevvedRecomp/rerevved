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

TEST_CASE("portable_mode_marker_selects_userdata", "[ctest][portable-mode]")
{
    TempDir dir;

    UNSCOPED_INFO("no marker leaves the default data folder");
    CHECK(!pm::PortableRoot(dir.path).has_value());

    UNSCOPED_INFO("a directory named like the marker does not count");
    fs::create_directory(dir.path / pm::kMarkerFileName);
    CHECK(!pm::PortableRoot(dir.path).has_value());
    fs::remove(dir.path / pm::kMarkerFileName);

    UNSCOPED_INFO("a marker file selects userdata beside the executable");
    std::ofstream(dir.path / pm::kMarkerFileName).close();
    const auto root = pm::PortableRoot(dir.path);
    REQUIRE(root.has_value());
    CHECK(*root == dir.path / "userdata");
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
