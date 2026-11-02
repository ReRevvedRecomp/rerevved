#include "keyboard_icons.h"
#include "main_menu_logo_asset.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
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
    CHECK(ki::PrimaryKey("Shift+Space,G") == "G");
    CHECK(ki::PrimaryKey("Comma,Space") == "Space");
    CHECK(ki::PrimaryKey("Numpad1, ,Space,G") == "Space");
    CHECK(ki::PrimaryKey(",Space") == "Space");
    CHECK(ki::PrimaryKey("Space,Shift+G") == "Space");
    CHECK(ki::PrimaryKey("Shift+Up,Shift+Down,Up") == "Up");
    // Bare modifiers never activate a button in the SDK, so they are skipped.
    CHECK(ki::PrimaryKey("Shift+Up,Shift,Space") == "Space");
    CHECK(ki::PrimaryKey("Control,Alt, Shift ,G") == "G");
    CHECK(!ki::PrimaryKey("Shift"));
    CHECK(!ki::PrimaryKey("Shift+Up,Control,Alt"));
    CHECK(!ki::PrimaryKey("Shift+Space"));
    CHECK(!ki::PrimaryKey("Shift+Up,Ctrl+Down"));
    CHECK(!ki::PrimaryKey("Numpad1,Comma"));
    CHECK(!ki::PrimaryKey("F13"));
    CHECK(!ki::PrimaryKey(""));
    CHECK(!ki::PrimaryKey("   "));
    CHECK(!ki::PrimaryKey(","));
    // The SDK matches names case-sensitively, so a lowercase name has no art.
    CHECK(!ki::PrimaryKey("space"));
    CHECK(!ki::PrimaryKey("SPACE"));
    CHECK(!ki::PrimaryKey("\tSpace"));

    const auto keys = ki::ShippedKeys();
    CHECK(std::set<std::string_view>(keys.begin(), keys.end()).size() == keys.size());
    for (const std::string_view key : keys)
    {
        const bool modifier = key == "Shift" || key == "Control" || key == "Alt";
        CHECK(ki::PrimaryKey(key) == (modifier ? std::nullopt : std::optional<std::string_view>(key)));
    }
}

TEST_CASE("keyboard_icons_slots", "[ctest][button-icons]")
{
    std::set<std::string_view> names;
    std::set<std::string_view> keybinds;
    size_t                     longest = 0;
    for (const ki::Slot& slot : ki::kSlots)
    {
        INFO(slot.fileName);
        CHECK(names.insert(slot.fileName).second);
        CHECK(keybinds.insert(slot.keybind).second);
        CHECK(slot.keybind.starts_with("keybind_"));
        CHECK(slot.fileName.ends_with(".dds"));
        CHECK(std::none_of(slot.fileName.begin(), slot.fileName.end(), [](char c)
                           {
                               return c >= 'A' && c <= 'Z';
                           }));
        const bool known = (slot.width == 64 && slot.height == 64) || (slot.width == 128 && slot.height == 64) || (slot.width == 64 && slot.height == 128);
        CHECK(known);
        longest = std::max(longest, slot.fileName.size());
    }
    CHECK(longest == ki::kMaxFileNameLength);

    auto canvas = [](std::string_view name)
    {
        const ki::Slot& slot = ki::kSlots[*ki::FindSlot(name)];
        return std::pair<uint32_t, uint32_t>{ slot.width, slot.height };
    };
    CHECK(canvas("lb.dds") == std::pair<uint32_t, uint32_t>{ 128, 64 });
    CHECK(canvas("rb.dds") == std::pair<uint32_t, uint32_t>{ 128, 64 });
    CHECK(canvas("lt.dds") == std::pair<uint32_t, uint32_t>{ 64, 128 });
    CHECK(canvas("rt.dds") == std::pair<uint32_t, uint32_t>{ 64, 128 });
    CHECK(canvas("startbutton.dds") == std::pair<uint32_t, uint32_t>{ 64, 64 });

    CHECK(ki::FindSlot("LB.DDS") == ki::FindSlot("lb.dds"));
    CHECK(ki::FindSlot("A.dds") == 0);
    for (const std::string_view name : { "ls.dds", "rs.dds", "dpad.dds", "xboxvertical.dds", "xboxhorizontal.dds", "ps3x.dds", "lb.dd", "ui\\lb.dds", "lb.dds.bak", "" })
    {
        INFO(name);
        CHECK(!ki::FindSlot(name));
    }
}

TEST_CASE("keyboard_icons_compose_canvas", "[ctest][button-icons]")
{
    const auto icon = readFile(kArtDir / "Space.dds");
    REQUIRE(ki::IsValidIconDds(icon));

    CHECK(ki::ComposeCanvas(icon, 64, 64) == icon);

    for (const auto& [width, height] : { std::pair<uint32_t, uint32_t>{ 128, 64 }, std::pair<uint32_t, uint32_t>{ 64, 128 } })
    {
        INFO(width << "x" << height);
        const auto canvas = ki::ComposeCanvas(icon, width, height);
        REQUIRE(ki::IsValidCanvasDds(canvas, width, height));
        CHECK(canvas.size() == 128 + size_t{ width } * height * 4);
        CHECK(!ki::IsValidIconDds(canvas));

        const size_t left    = (width - 64) / 2;
        const size_t top     = (height - 64) / 2;
        size_t       visible = 0;
        for (size_t y = 0; y < height; ++y)
        {
            for (size_t x = 0; x < width; ++x)
            {
                const bool  inside = x >= left && x < left + 64 && y >= top && y < top + 64;
                const auto* pixel  = canvas.data() + 128 + (y * width + x) * 4;
                if (inside)
                {
                    const auto* source = icon.data() + 128 + ((y - top) * 64 + (x - left)) * 4;
                    CHECK(std::memcmp(pixel, source, 4) == 0);
                    visible += source[3] != 0 ? 1 : 0;
                }
                else
                {
                    CHECK(std::memcmp(pixel, "\0\0\0\0", 4) == 0);
                }
            }
        }
        CHECK(visible > 0);
    }

    // Canvas must center without shrinking: 64 plus an even amount.
    CHECK(ki::ComposeCanvas(icon, 63, 64).empty());
    CHECK(ki::ComposeCanvas(icon, 64, 32).empty());
    CHECK(ki::ComposeCanvas(icon, 129, 64).empty());
    CHECK(ki::ComposeCanvas(icon, 0, 0).empty());
    CHECK(ki::ComposeCanvas(icon, 100000, 64).empty());
    CHECK(ki::ComposeCanvas({}, 128, 64).empty());
    CHECK(ki::ComposeCanvas(std::span<const uint8_t>(icon.data(), 128), 128, 64).empty());
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

    const auto wide = ki::ComposeCanvas(valid, 128, 64);
    CHECK(ki::IsValidCanvasDds(wide, 128, 64));
    CHECK(!ki::IsValidCanvasDds(wide, 64, 128));
    CHECK(!ki::IsValidCanvasDds(wide, 64, 64));
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
    const auto        space = readFile(kArtDir / "Space.dds");
    REQUIRE(ki::IsValidIconDds(space));
    TempDir dir;
    writeFile(dir.path / "Space.dds", space);
    writeFile(dir.path / "G.dds", std::vector<uint8_t>(space.begin(), space.end() - 4));
    writeFile(dir.path / "H.dds", std::vector<uint8_t>(ki::kIconDdsSize, 0));

    auto bindAll = [](std::string binding, std::map<std::string, std::string> exceptions = {})
    {
        return [binding = std::move(binding), exceptions = std::move(exceptions)](std::string_view keybind)
        {
            const auto it = exceptions.find(std::string(keybind));
            return it != exceptions.end() ? it->second : binding;
        };
    };
    auto prepared = [](std::string_view name)
    {
        ki::Prepared result;
        return ki::TryGetPrepared(name, result) ? result : ki::Prepared{};
    };

    CHECK(!ki::AnyPrepared());
    CHECK(ki::Prepare(dir.path, bindAll(" Space ,G")) == ki::kSlots.size());
    CHECK(ki::AnyPrepared());
    {
        const auto a = prepared("A.dds");
        REQUIRE(a.payload);
        CHECK(*a.payload == space);
        CHECK(a.key == "Space");
        const auto lb = prepared("lb.dds");
        REQUIRE(lb.payload);
        CHECK(*lb.payload == ki::ComposeCanvas(space, 128, 64));
        CHECK(ki::IsValidCanvasDds(*lb.payload, 128, 64));
        const auto rt = prepared("RT.DDS");
        REQUIRE(rt.payload);
        CHECK(ki::IsValidCanvasDds(*rt.payload, 64, 128));
    }

    const auto count = ki::Prepare(dir.path,
                                   bindAll("Space",
                                           { { "keybind_b", "Shift+Space,Comma,Space" },
                                             { "keybind_x", "G" },
                                             { "keybind_y", "H,Space" },
                                             { "keybind_left_shoulder", "Shift+Space" },
                                             { "keybind_right_shoulder", "" },
                                             { "keybind_left_trigger", "Numpad0" },
                                             { "keybind_dpad_up", "Shift+Up" } }));
    CHECK(count == ki::kSlots.size() - 6);
    CHECK(prepared("b.dds").key == "Space");
    CHECK(prepared("a.dds").key == "Space");
    CHECK(prepared("startbutton.dds").key == "Space");
    for (const std::string_view name : { "x.dds", "y.dds", "lb.dds", "rb.dds", "lt.dds", "dup.dds" })
    {
        INFO(name);
        CHECK(!prepared(name).payload);
    }
    CHECK(prepared("rt.dds").payload);

    for (const std::string_view name : { "ls.dds", "dpad.dds", "ps3x.dds", "ui\\a.dds", "" })
    {
        CHECK(!prepared(name).payload);
    }

    CHECK(ki::Prepare(dir.path / "missing", bindAll("Space")) == 0);
    CHECK(!ki::AnyPrepared());
    REQUIRE(ki::Prepare(dir.path, bindAll("Space")) == ki::kSlots.size());
    CHECK(ki::Prepare(dir.path, nullptr) == 0);
    CHECK(!ki::AnyPrepared());
    CHECK(!prepared("a.dds").payload);

    const std::map<std::string, std::string> defaults{
        { "keybind_a", "Semicolon,Space" },
        { "keybind_b", "Quote,Backspace" },
        { "keybind_x", "L" },
        { "keybind_y", "P" },
        { "keybind_left_trigger", "Q,I" },
        { "keybind_right_trigger", "E,O" },
        { "keybind_left_shoulder", "1" },
        { "keybind_right_shoulder", "3" },
        { "keybind_lstick_press", "F" },
        { "keybind_rstick_press", "K" },
        { "keybind_dpad_up", "Shift+Up" },
        { "keybind_dpad_down", "Shift+Down" },
        { "keybind_dpad_left", "Shift+Left" },
        { "keybind_dpad_right", "Shift+Right" },
        { "keybind_start", "X,Return" },
        { "keybind_back", "Z,Tab" },
    };
    REQUIRE(defaults.size() == ki::kSlots.size());
    CHECK(ki::Prepare(kArtDir, [&](std::string_view keybind)
                      {
                          return defaults.at(std::string(keybind));
                      }) == ki::kSlots.size() - 4);
    CHECK(prepared("a.dds").key == "Semicolon");
    CHECK(prepared("b.dds").key == "Quote");
    CHECK(prepared("lt.dds").key == "Q");
    CHECK(prepared("rt.dds").key == "E");
    CHECK(prepared("lb.dds").key == "1");
    CHECK(prepared("startbutton.dds").key == "X");
    CHECK(prepared("backbutton.dds").key == "Z");
    CHECK(prepared("l3.dds").key == "F");
    CHECK(prepared("r3.dds").key == "K");
    CHECK(!prepared("dup.dds").payload);
    CHECK(*prepared("y.dds").payload == readFile(kArtDir / "P.dds"));
    ki::ResetForTests();
    CHECK(!ki::AnyPrepared());
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

    const auto semicolon = readFile(kArtDir / "Semicolon.dds");
    const auto space     = readFile(kArtDir / "Space.dds");
    REQUIRE(ki::IsValidIconDds(semicolon));
    REQUIRE(ki::IsValidIconDds(space));
    const uint32_t kUntouched[] = { guest("XA.dds"), guest("A.dds.bak"), guest("ui\\A.dds"), guest("A.dd"), guest("A.ddsx"), guest("ls.dds"), guest("dpad.dds"), guest("ps3x.dds"), guest("startbutton.ddsx"), guest("") };

    ki::ResetForTests();
    CHECK(!run(guest("A.dds")));
    CHECK(rex::memory::load_and_swap<uint32_t>(slot()) == kSentinel);

    const std::map<std::string, std::string> bindings{
        { "keybind_a", "Semicolon,Space" },
        { "keybind_left_shoulder", "Space" },
        { "keybind_right_trigger", "Space" },
        { "keybind_b", "Shift+Q" },
    };
    REQUIRE(ki::Prepare(kArtDir, [&](std::string_view keybind)
                        {
                            const auto it = bindings.find(std::string(keybind));
                            return it != bindings.end() ? it->second : std::string();
                        }) == 3);
    REQUIRE(run(guest("A.dds")));
    served(semicolon);
    const uint32_t first = dataPointer.u32;
    REQUIRE(run(guest("a.DDS")));
    served(semicolon);
    CHECK(dataPointer.u32 == first);
    REQUIRE(run(guest("a.dds")));
    CHECK(dataPointer.u32 == first);

    // Each slot needs its own live buffer.
    REQUIRE(run(guest("LB.dds")));
    const auto wide = ki::ComposeCanvas(space, 128, 64);
    served(wide);
    CHECK(wide.size() == 32896);
    const uint32_t second = dataPointer.u32;
    CHECK(second != first);
    REQUIRE(run(guest("rt.dds")));
    const auto tall = ki::ComposeCanvas(space, 64, 128);
    served(tall);
    CHECK(dataPointer.u32 != first);
    CHECK(dataPointer.u32 != second);
    REQUIRE(run(guest("lb.dds")));
    CHECK(dataPointer.u32 == second);
    served(wide);
    REQUIRE(run(guest("a.dds")));
    CHECK(dataPointer.u32 == first);
    served(semicolon);

    CHECK(!run(guest("b.dds")));
    CHECK(!run(guest("rb.dds")));
    CHECK(!run(guest("startbutton.dds")));
    for (const uint32_t name : kUntouched)
    {
        CHECK(!run(name));
        CHECK(rex::memory::load_and_swap<uint32_t>(slot()) == kSentinel);
    }
    CHECK(!run(guest("a-name-much-longer-than-any-prompt-image.dds")));
    CHECK(!run(guest("A.dds"), 0));
    CHECK(!run(0));
    CHECK(!run(guest("A.dds"), 0x1000));
    CHECK(!run(0x1000));

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
    REQUIRE(run(guest("A.dds")));
    CHECK(dataPointer.u32 == first);
    served(semicolon);
}
