#include "keyboard_icons.h"
#include "main_menu_logo_asset.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <rex/kernel/init.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

void ApplyAssetFileOverride(PPCRegister& stackPointer, PPCRegister& returnedDataPointer, PPCRegister& requestedName);

namespace
{

namespace ki = rerevved::keyboard_icons;
namespace fs = std::filesystem;

const fs::path kArtDir{ REREVVED_KEYBOARD_ICONS_DIR };

std::vector<uint8_t> readFile(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void putU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i)
    {
        bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

struct TempDir
{
    fs::path path;

    TempDir()
    {
        static std::atomic<int> counter{ 0 };
        const auto              stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path                          = fs::temp_directory_path() / ("rerevved_keyboard_icons_test_" + std::to_string(stamp) + "_" +
                                                                     std::to_string(counter++));
        fs::create_directories(path);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::vector<uint8_t>& logoFixture()
{
    static std::vector<uint8_t> bytes;
    return bytes;
}

rex::Result<rex::system::AssetOverlayResolution> resolveLogoFixture(std::span<const rex::system::AssetOverlayPackage>, std::string_view, size_t)
{
    rex::system::AssetOverlayResolution resolved;
    resolved.package_id = "test.logo";
    resolved.bytes      = logoFixture();
    return resolved;
}

struct ResetOnExit
{
    ~ResetOnExit()
    {
        ki::ResetForTests();
        rerevved::main_menu_logo::ResetForTests();
    }
};

} // namespace

TEST_CASE("keyboard_icons_primary_key", "[ctest][button-icons]")
{
    CHECK(ki::PrimaryKey("Semicolon,Space") == "Semicolon");
    CHECK(ki::PrimaryKey(" Space ,G") == "Space");
    CHECK(ki::PrimaryKey("G") == "G");
    CHECK(ki::PrimaryKey("LMB") == "LMB");
    CHECK(ki::PrimaryKey("PageDown,Space") == "PageDown");
    // A chord, an empty first alternative and an unshipped key give no icon,
    // and a later alternative is never used.
    CHECK(!ki::PrimaryKey("Shift+Space"));
    CHECK(ki::PrimaryKey("Space,Shift+G") == "Space");
    CHECK(!ki::PrimaryKey("Shift+Space,G"));
    CHECK(!ki::PrimaryKey("Comma,Space"));
    CHECK(!ki::PrimaryKey("Numpad1,Space"));
    CHECK(!ki::PrimaryKey("F13"));
    CHECK(!ki::PrimaryKey(""));
    CHECK(!ki::PrimaryKey("   "));
    CHECK(!ki::PrimaryKey(",Space"));
    // The SDK matches names case-sensitively, so a lowercase name has no art.
    CHECK(!ki::PrimaryKey("space"));
    CHECK(!ki::PrimaryKey("SPACE"));
    CHECK(!ki::PrimaryKey("\tSpace"));

    const auto keys = ki::ShippedKeys();
    CHECK(std::set<std::string_view>(keys.begin(), keys.end()).size() == keys.size());
    for (const std::string_view key : keys)
    {
        CHECK(ki::PrimaryKey(key) == key);
    }
}

TEST_CASE("keyboard_icons_dds_validator", "[ctest][button-icons]")
{
    const auto valid = readFile(kArtDir / "Semicolon.dds");
    REQUIRE(valid.size() == ki::kIconDdsSize);
    CHECK(ki::IsValidIconDds(valid));

    CHECK(!ki::IsValidIconDds({}));
    auto truncated = valid;
    truncated.pop_back();
    CHECK(!ki::IsValidIconDds(truncated));
    auto padded = valid;
    padded.push_back(0);
    CHECK(!ki::IsValidIconDds(padded));
    CHECK(!ki::IsValidIconDds(std::vector<uint8_t>(valid.begin(), valid.begin() + 128)));

    struct Mutation
    {
        const char* what;
        size_t      offset;
        uint32_t    value;
    };

    for (const Mutation& mutation : {
             Mutation{ "header size", 4, 123 },
             Mutation{ "flags", 8, 0x1007 },
             Mutation{ "height", 12, 32 },
             Mutation{ "width", 16, 32 },
             Mutation{ "pitch", 20, 128 },
             Mutation{ "depth", 24, 1 },
             Mutation{ "mip count", 28, 1 },
             Mutation{ "pixel format size", 76, 24 },
             Mutation{ "pixel format flags", 80, 0x4 },
             Mutation{ "fourcc DXT1", 84, 0x31545844 },
             Mutation{ "bits per pixel", 88, 24 },
             Mutation{ "red mask", 92, 0x000000ff },
             Mutation{ "green mask", 96, 0x00ff0000 },
             Mutation{ "blue mask", 100, 0x00ff0000 },
             Mutation{ "alpha mask", 104, 0 },
             Mutation{ "mipmap caps", 108, 0x401008 },
             Mutation{ "cubemap caps2", 112, 0x200 },
         })
    {
        auto bytes = valid;
        putU32(bytes, mutation.offset, mutation.value);
        INFO(mutation.what);
        CHECK(!ki::IsValidIconDds(bytes));
    }
    auto badMagic = valid;
    badMagic[0]   = 'X';
    CHECK(!ki::IsValidIconDds(badMagic));
}

TEST_CASE("keyboard_icons_shipped_files", "[ctest][button-icons]")
{
    std::set<std::string> onDisk;
    for (const auto& entry : fs::directory_iterator(kArtDir))
    {
        onDisk.insert(entry.path().filename().string());
    }
    std::set<std::string> listed;
    for (const std::string_view key : ki::ShippedKeys())
    {
        listed.insert(std::string(key) + ".dds");
    }
    CHECK(onDisk == listed);
    for (const std::string& name : listed)
    {
        INFO(name);
        CHECK(ki::IsValidIconDds(readFile(kArtDir / name)));
    }
}

TEST_CASE("keyboard_icons_prepare", "[ctest][button-icons]")
{
    const ResetOnExit reset;
    const auto        valid = readFile(kArtDir / "Space.dds");
    REQUIRE(ki::IsValidIconDds(valid));
    TempDir dir;
    writeFile(dir.path / "Space.dds", valid);
    writeFile(dir.path / "G.dds", std::vector<uint8_t>(valid.begin(), valid.end() - 4));
    writeFile(dir.path / "H.dds", std::vector<uint8_t>(ki::kIconDdsSize, 0));

    ki::Payload payload;
    REQUIRE(ki::Prepare(dir.path, " Space ,G"));
    REQUIRE(ki::TryGetPayload(payload));
    CHECK(*payload == valid);
    CHECK(ki::PreparedKey() == "Space");

    // A failed preparation drops the earlier payload.
    for (const std::string_view binding : { "G", "H", "A", "Shift+Space", "Comma,Space", "Numpad0", "" })
    {
        REQUIRE(ki::Prepare(dir.path, " Space "));
        INFO(binding);
        CHECK(!ki::Prepare(dir.path, binding));
        CHECK(!ki::TryGetPayload(payload));
        CHECK(ki::PreparedKey().empty());
    }
    // A missing directory is the same: the retail icon stays.
    CHECK(!ki::Prepare(dir.path / "missing", "Space"));
    CHECK(!ki::TryGetPayload(payload));

    REQUIRE(ki::Prepare(kArtDir, "Semicolon,Space"));
    REQUIRE(ki::TryGetPayload(payload));
    CHECK(*payload == readFile(kArtDir / "Semicolon.dds"));
    ki::ResetForTests();
    CHECK(!ki::TryGetPayload(payload));
}

TEST_CASE("keyboard_icons_asset_hook", "[ctest][button-icons]")
{
    const ResetOnExit reset;
    rex::Runtime      runtime(fs::path{});
    REQUIRE(runtime.Setup(rex::RuntimeConfig{
                .kernel_init = rex::kernel::InitializeKernel,
                .tool_mode   = true,
            }) == 0);
    auto*              memory = runtime.memory();
    constexpr uint32_t kPage  = 0x830F0000;
    auto*              heap   = memory->LookupHeap(kPage);
    REQUIRE((heap && heap->AllocFixed(kPage, 0x10000, 0x10000, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)));

    uint32_t next  = kPage;
    auto     guest = [&](std::string_view text)
    {
        const uint32_t address = next;
        std::memcpy(memory->TranslateVirtual<char*>(address), text.data(), text.size());
        memory->TranslateVirtual<char*>(address)[text.size()] = 0;
        next += 0x100;
        return address;
    };
    const uint32_t     stack     = kPage + 0x8000;
    constexpr uint32_t kSentinel = 0x11223344;
    auto               slot      = [&]() -> uint8_t*
    {
        return memory->TranslateVirtual<uint8_t*>(stack + 84);
    };
    PPCRegister dataPointer{};
    auto        run = [&](uint32_t nameAddress, uint32_t stackAddress = stack)
    {
        PPCRegister sp{}, name{};
        sp.u64          = stackAddress;
        name.u64        = nameAddress;
        dataPointer.u64 = kSentinel;
        rex::memory::store_and_swap<uint32_t>(slot(), kSentinel);
        ApplyAssetFileOverride(sp, dataPointer, name);
        return dataPointer.u32 != kSentinel;
    };
    auto served = [&](const std::vector<uint8_t>& expected)
    {
        const uint32_t pointer = dataPointer.u32;
        CHECK(rex::memory::load_and_swap<uint32_t>(slot()) == pointer);
        CHECK(rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(pointer - 4)) == expected.size());
        CHECK(std::memcmp(memory->TranslateVirtual<uint8_t*>(pointer), expected.data(), expected.size()) == 0);
    };

    const auto icon = readFile(kArtDir / "Semicolon.dds");
    REQUIRE(ki::IsValidIconDds(icon));
    const uint32_t kUntouched[] = { guest("B.dds"), guest("XA.dds"), guest("A.dds.bak"), guest("ui\\A.dds"), guest("A.dd"), guest("A.ddsx"), guest("") };

    // Without a prepared keyboard icon every name keeps the retail data.
    ki::ResetForTests();
    CHECK(!run(guest("A.dds")));
    CHECK(rex::memory::load_and_swap<uint32_t>(slot()) == kSentinel);

    REQUIRE(ki::Prepare(kArtDir, "Semicolon,Space"));
    REQUIRE(run(guest("A.dds")));
    served(icon);
    const uint32_t first = dataPointer.u32;
    REQUIRE(run(guest("a.DDS")));
    served(icon);
    CHECK(dataPointer.u32 == first);
    REQUIRE(run(guest("a.dds")));
    CHECK(dataPointer.u32 == first);
    for (const uint32_t name : kUntouched)
    {
        CHECK(!run(name));
        CHECK(rex::memory::load_and_swap<uint32_t>(slot()) == kSentinel);
    }
    // Bad inputs keep the retail data: no stack, no name, unmapped stack or name.
    CHECK(!run(guest("A.dds"), 0));
    CHECK(!run(0));
    CHECK(!run(guest("A.dds"), 0x1000));
    CHECK(!run(0x1000));

    // The logo keeps working beside the keyboard icon, in its own buffer.
    rerevved::main_menu_logo::ResetForTests();
    std::vector<uint8_t> logo(rerevved::main_menu_logo::kLogoDdsSize, 0);
    std::memcpy(logo.data(), "DDS ", 4);
    putU32(logo, 4, 124);
    putU32(logo, 12, 256);
    putU32(logo, 16, 1024);
    putU32(logo, 76, 32);
    putU32(logo, 80, 0x41);
    putU32(logo, 88, 32);
    putU32(logo, 92, 0x00ff0000);
    putU32(logo, 96, 0x0000ff00);
    putU32(logo, 100, 0x000000ff);
    putU32(logo, 104, 0xff000000);
    putU32(logo, 108, 0x1000);
    logo[128] = 0x5a;
    REQUIRE(rerevved::main_menu_logo::IsValidLogoDds(logo));
    logoFixture() = logo;
    REQUIRE(rerevved::main_menu_logo::ResolveSelectedLogo({}, &resolveLogoFixture));
    REQUIRE(run(guest("GFX_MainMenu_logo.dds")));
    served(logo);
    CHECK(dataPointer.u32 != first);
    CHECK(!run(guest("gfx_mainmenu_logo.dds")));
    // The keyboard icon still serves from its own buffer.
    REQUIRE(run(guest("A.dds")));
    CHECK(dataPointer.u32 == first);
    served(icon);
}
