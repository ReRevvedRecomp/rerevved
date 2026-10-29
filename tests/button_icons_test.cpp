#include "button_icons.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/kernel/init.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

void SwapButtonIconImage(PPCRegister& name);
void SwapButtonIconLabel(PPCRegister& text);
void SwapButtonIconTipImage(PPCRegister& name, PPCRegister& width, PPCRegister& height);

namespace bp = rerevved::button_icons;

TEST_CASE("button_icons_family_setting", "[ctest][button-icons]")
{
    CHECK(bp::ParseFamily("playstation") == bp::Family::kPlayStation);
    CHECK(bp::ParseFamily("xbox") == bp::Family::kXbox);
    CHECK(bp::ParseFamily("") == bp::Family::kXbox);
    CHECK(bp::ParseFamily("PlayStation") == bp::Family::kXbox);
}

TEST_CASE("button_icons_tables", "[ctest][button-icons]")
{
    CHECK(bp::ImageStem("A.dds") == "PS3X");
    CHECK(bp::ImageStem("a.DDS") == "PS3X");
    CHECK(bp::ImageStem("STARTBUTTON.dds") == "PS3START");
    CHECK(bp::ImageStem("XBOXVERTICAL.dds") == "PS3VERTICAL");
    // Unequal sizes, other families, other extensions and other images stay.
    for (const std::string_view kept : { "LB.dds", "RT.dds", "PS3X.dds", "A.png", "A", "bink.dds", "analogL.dds", ".dds" })
    {
        CHECK(!bp::ImageStem(kept));
    }

    CHECK(bp::Label("LB") == "PS3L1");
    CHECK(bp::Label("RT") == "PS3R2");
    CHECK(bp::Label("BACKBUTTON") == "PS3SELECT");
    CHECK(bp::Label("DHORIZONTAL") == "PS3DHORIZONTAL");
    // Face letters, markers, other cases and longer text stay.
    for (const std::string_view kept : { "A", "B", "X", "Y", "blank", "XBOX", "PS3", "lb", "LBX", "PS3L1", "" })
    {
        CHECK(!bp::Label(kept));
    }

    const auto shoulder = bp::Tip("LB");
    REQUIRE(shoulder);
    CHECK(shoulder->name == "PS3L1");
    CHECK(shoulder->square);
    const auto axis = bp::Tip("XBOXHORIZONTAL");
    REQUIRE(axis);
    CHECK(axis->name == "PS3HORIZONTAL");
    CHECK(!axis->square);
    CHECK(!bp::Tip("A"));
    CHECK(!bp::Tip("DPAD"));
    CHECK(bp::SquareSide(56, 28) == 28);
    CHECK(bp::SquareSide(32, 48) == 32);
}

TEST_CASE("button_icons_hooks_swap_only_matching_strings", "[ctest][button-icons]")
{
    REQUIRE(rex::cvar::SetFlagByName("button_icons", "playstation"));
    rex::Runtime runtime(std::filesystem::path{});
    REQUIRE(runtime.Setup(rex::RuntimeConfig{
                .kernel_init = rex::kernel::InitializeKernel,
                .tool_mode   = true,
            }) == 0);
    REQUIRE(bp::ActiveFamily() == bp::Family::kPlayStation);
    auto*              memory = runtime.memory();
    constexpr uint32_t kPage  = 0x830E0000;
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
    auto read = [&](const PPCRegister& r)
    {
        return std::string(memory->TranslateVirtual<const char*>(r.u32));
    };

    PPCRegister name{};
    name.u64 = guest("A.dds");
    SwapButtonIconImage(name);
    CHECK(read(name) == "PS3X.dds");
    const uint64_t first = name.u64;
    name.u64             = guest("A.dds");
    SwapButtonIconImage(name);
    CHECK(name.u64 == first);
    name.u64 = guest("ui\\backbutton.DDS");
    SwapButtonIconImage(name);
    CHECK(read(name) == "ui\\PS3SELECT.DDS");
    for (const std::string_view kept : { "LB.dds", "bink.dds" })
    {
        const uint32_t address = guest(kept);
        name.u64               = address;
        SwapButtonIconImage(name);
        CHECK(name.u64 == address);
    }

    PPCRegister text{};
    text.u64 = guest("RB");
    SwapButtonIconLabel(text);
    CHECK(read(text) == "PS3R1");
    for (const std::string_view kept : { "A", "Rome", "LBX", "RB " })
    {
        const uint32_t address = guest(kept);
        text.u64               = address;
        SwapButtonIconLabel(text);
        CHECK(text.u64 == address);
    }
    text.u64 = 0x1000;
    SwapButtonIconLabel(text);
    CHECK(text.u64 == 0x1000);
    // A label ending at the last mapped byte still reads; an unterminated one
    // running into the unmapped page does not.
    constexpr uint32_t kEdge = kPage + 0x10000 - 3;
    std::memcpy(memory->TranslateVirtual<char*>(kEdge), "LB", 3);
    text.u64 = kEdge;
    SwapButtonIconLabel(text);
    CHECK(read(text) == "PS3L1");
    std::memcpy(memory->TranslateVirtual<char*>(kEdge), "LBX", 3);
    text.u64 = kEdge;
    SwapButtonIconLabel(text);
    CHECK(text.u64 == kEdge);

    PPCRegister width{}, height{};
    name.u64   = guest("LT");
    width.s64  = 28;
    height.s64 = 56;
    SwapButtonIconTipImage(name, width, height);
    CHECK(read(name) == "PS3L2");
    CHECK(width.s64 == 28);
    CHECK(height.s64 == 28);
    name.u64   = guest("XBOXVERTICAL");
    width.s64  = 36;
    height.s64 = 36;
    SwapButtonIconTipImage(name, width, height);
    CHECK(read(name) == "PS3VERTICAL");
    CHECK(width.s64 == 36);
    const uint32_t other = guest("CityIcon");
    name.u64             = other;
    width.s64            = 56;
    height.s64           = 28;
    SwapButtonIconTipImage(name, width, height);
    CHECK(name.u64 == other);
    CHECK(width.s64 == 56);
    CHECK(height.s64 == 28);
}
