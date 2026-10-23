// resolve_derive: expected copy words follow literal g167:13052-13526
// emulation, not formulas.

#include "gpu/guest_d3d/resolve_derive.h"

#include "developer_checks.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <unordered_map>

using namespace rerevved::gpu::guest_d3d;

namespace
{

constexpr std::uint32_t kG = 0x40010000, kHeader = 0x40020000, kRect = 0x40021000, kPoint = 0x40022000, kSurface = 0x40030000, kDepthSurface = 0x40031000;

struct Case
{
    const char*                  name;
    std::uint32_t                flags    = 0;
    bool                         hasRect  = false;
    std::int32_t                 rectLeft = 0, rectTop = 0;
    bool                         hasPoint = false;
    std::int32_t                 pointX = 0, pointY = 0;
    std::int32_t                 defaultX = 0, defaultY = 0;
    std::uint32_t                d0 = 0x0A000000, d1 = 0x1E3B0006, d2 = 0x0059E4FF, d3 = 0, d5 = 0;
    std::array<std::uint32_t, 4> msaa       = {};
    std::uint32_t                msaaDepth  = 0;
    bool                         noSurfaces = false;
    ResolveCopyWords             expect;
};

// The destination format table: the size field in halfword bits 3..7 and
// other bits set so a wrong mask shows.
std::uint32_t tableEntry(std::uint32_t format)
{
    return ((1 + format % 8) << 3) | (format % 5) | (((format * 37) & 0xFF) << 8);
}

using Memory = std::unordered_map<std::uint32_t, std::uint32_t>;

Memory build(const Case& c)
{
    Memory memory;
    for (std::uint32_t format = 0; format < 64; ++format)
    {
        const auto address = kResolveFormatTable + 2 * format;
        auto&      word    = memory[address & ~3U];
        word               = (address & 2) ? (word & 0xFFFF0000U) | tableEntry(format) : (word & 0x0000FFFFU) | (tableEntry(format) << 16);
    }
    memory[kResolveDefaultPoint]     = static_cast<std::uint32_t>(c.defaultX);
    memory[kResolveDefaultPoint + 4] = static_cast<std::uint32_t>(c.defaultY);
    memory[kHeader + 28]             = c.d0;
    memory[kHeader + 32]             = c.d1;
    memory[kHeader + 36]             = c.d2;
    memory[kHeader + 40]             = c.d3;
    memory[kHeader + 44]             = 0;
    memory[kHeader + 48]             = c.d5;
    if (!c.noSurfaces)
    {
        for (std::uint32_t slot = 0; slot < 4; ++slot)
        {
            memory[kG + 0x3098 + 4 * slot]         = kSurface + 0x100 * slot;
            memory[kSurface + 0x100 * slot + 0x18] = (c.msaa[slot] << 16) | 0x500;
        }
        memory[kG + 0x30A8]          = kDepthSurface;
        memory[kDepthSurface + 0x18] = (c.msaaDepth << 16) | 0x500;
    }
    if (c.hasRect)
    {
        memory[kRect]      = static_cast<std::uint32_t>(c.rectLeft);
        memory[kRect + 4]  = static_cast<std::uint32_t>(c.rectTop);
        memory[kRect + 8]  = static_cast<std::uint32_t>(c.rectLeft + 100);
        memory[kRect + 12] = static_cast<std::uint32_t>(c.rectTop + 100);
    }
    if (c.hasPoint)
    {
        memory[kPoint]     = static_cast<std::uint32_t>(c.pointX);
        memory[kPoint + 4] = static_cast<std::uint32_t>(c.pointY);
    }
    return memory;
}

GuestWordReader readerOf(const Memory& memory)
{
    return [&memory](std::uint32_t address, std::uint32_t& value)
    {
        const auto found = memory.find(address);
        if (found == memory.end())
            return false;
        value = found->second;
        return true;
    };
}

std::array<std::uint32_t, 8> registers(const Case& c)
{
    return { kG, c.flags, c.hasRect ? kRect : 0, kHeader, c.hasPoint ? kPoint : 0, 0, 0, 0 };
}

std::string hex(const ResolveCopyWords& w)
{
    return std::format("{:08X},{:08X},{:08X},{:08X}", w.copyControl, w.destBase, w.destPitch, w.destInfo);
}

// Base: slot 0, 1280x720, pitch 1280, format 6, no rectangle or point. Fields
// follow declaration order; 0x8269E520 copy words G+0x2A18..0x2A24 use literal
// emulation, not resolve_derive.cpp formulas.
const Case kCases[] = {
    { .name = "slot0_msaa0_fixup_0x10", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "slot0_msaa1_fixup_0x50", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .msaa = { 1, 0, 0, 0 }, .expect = { 0x00100040, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "slot0_msaa2_fixup_0x70", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .msaa = { 2, 0, 0, 0 }, .expect = { 0x00100060, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "slot0_msaa3_fixup_0x70", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .msaa = { 3, 0, 0, 0 }, .expect = { 0x00100060, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "slot2_msaa1_reads_slot2_surface", .flags = 0x00000002, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .msaa = { 0, 0, 1, 0 }, .expect = { 0x00100042, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "slot2_msaa0_ignores_other_slots", .flags = 0x00000002, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .msaa = { 3, 3, 0, 3 }, .expect = { 0x00100002, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "fragment_select_bits_skip_fixup", .flags = 0x00000030, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .noSurfaces = true, .expect = { 0x00100020, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "fragment_select_0x40_skip_fixup", .flags = 0x00000040, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .noSurfaces = true, .expect = { 0x00100030, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "depth_slot4_msaa0_sel0", .flags = 0x00000004, .d0 = 0x0A000000, .d1 = 0x1E3B0016, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00000004, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "depth_slot4_msaa1_sel0", .flags = 0x00000004, .d0 = 0x0A000000, .d1 = 0x1E3B0016, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .msaaDepth = 1, .expect = { 0x00000044, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "depth_slot4_flag_2000_sel3", .flags = 0x00002004, .d0 = 0x0A000000, .d1 = 0x1E3B0016, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00300004, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "color_flag_2000_sel3", .flags = 0x00002000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .msaa = { 2, 0, 0, 0 }, .expect = { 0x00300060, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "clear_and_exp_bias_bits", .flags = 0x54000300, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100300, 0x1E3B0000, 0x02D00500, 0x00150300 } },
    { .name = "exp_bias_all_bits", .flags = 0xFC000050, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100040, 0x1E3B0000, 0x02D00500, 0x003F0300 } },
    { .name = "flag_bits_outside_mask", .flags = 0x00008C50, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100040, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "rect_and_point_offsets", .flags = 0x00000010, .hasRect = true, .rectLeft = 64, .rectTop = 32, .hasPoint = true, .pointX = 96, .pointY = 48, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3D4C00, 0x02C00500, 0x00000300 } },
    { .name = "rect_with_null_point_negative", .flags = 0x00000010, .hasRect = true, .rectLeft = 32, .rectTop = 16, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E38B400, 0x02E00500, 0x00000300 } },
    { .name = "point_with_null_rect", .flags = 0x00000010, .hasPoint = true, .pointX = 64, .pointY = 24, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3E8000, 0x02B80500, 0x00000300 } },
    { .name = "point_above_rect_wraps_pitch_rows", .flags = 0x00000010, .hasRect = true, .rectLeft = 0, .rectTop = 100, .hasPoint = true, .pointX = 0, .pointY = 4, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x03FFFFFF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E2DE000, 0x20600500, 0x00000300 } },
    { .name = "default_point_nonzero", .flags = 0x00000010, .defaultX = 16, .defaultY = 8, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3C2600, 0x02C80500, 0x00000300 } },
    { .name = "page_bit_set", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0xFE3AF006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "page_bit_clear_boundary", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0xDFF00006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1FF00000, 0x02D00500, 0x00000300 } },
    { .name = "page_bit_edge_e00", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0xE0000006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x00001000, 0x02D00500, 0x00000300 } },
    { .name = "page_bit_with_offsets", .flags = 0x00000000, .hasRect = true, .rectLeft = 8, .rectTop = 8, .hasPoint = true, .pointX = 40, .pointY = 40, .d0 = 0x0A000000, .d1 = 0xFE3AF006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3F7C00, 0x02B00500, 0x00000300 } },
    { .name = "pitch_field_max", .flags = 0x00000000, .d0 = 0x7FC00000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D03FE0, 0x00000300 } },
    { .name = "pitch_low_bits_ignored", .flags = 0x00000000, .d0 = 0x0A3FFFE3, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "format_6", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0086, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000302 } },
    { .name = "format_0", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0000, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000000 } },
    { .name = "format_1", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0041, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000081 } },
    { .name = "format_2", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0082, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000102 } },
    { .name = "format_3", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B00C3, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000183 } },
    { .name = "format_7", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B00C7, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000383 } },
    { .name = "format_22", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0096, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000302 } },
    { .name = "format_23", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B00D7, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000303 } },
    { .name = "format_27", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B00DB, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x0000EF03 } },
    { .name = "format_28", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B001C, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x0000EF80 } },
    { .name = "format_29", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B005D, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x0000F001 } },
    { .name = "format_30", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B009E, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x0000EF02 } },
    { .name = "format_31", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B00DF, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x0000EF83 } },
    { .name = "format_32", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0020, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x0000F000 } },
    { .name = "format_36", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0024, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x0000F200 } },
    { .name = "format_37", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0065, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x0000F281 } },
    { .name = "format_38", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B00A6, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x0000F302 } },
    { .name = "format_54", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B00B6, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000382 } },
    { .name = "format_55", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B00F7, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000803 } },
    { .name = "format_56", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0038, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000880 } },
    { .name = "format_57", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0079, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x0000FC81 } },
    { .name = "format_63", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B00FF, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x0000FF83 } },
    { .name = "format_4", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0004, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000200 } },
    { .name = "format_10", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B008A, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000502 } },
    { .name = "format_62", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B00BE, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00001F02 } },
    { .name = "number_format_e0", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "number_format_e2", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000001, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00004300 } },
    { .name = "number_format_e1", .flags = 0x00000000, .d0 = 0x0A000004, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00002300 } },
    { .name = "number_format_e3", .flags = 0x00000000, .d0 = 0x0A000004, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000001, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00006300 } },
    { .name = "number_format_r17_2", .flags = 0x00000000, .d0 = 0x0A000008, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "number_format_r17_3_bit0", .flags = 0x00000000, .d0 = 0x0A00000C, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000001, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00004300 } },
    { .name = "number_format_forced_by_format_31", .flags = 0x00000000, .d0 = 0x0A000004, .d1 = 0x1E3B001F, .d2 = 0x0059E4FF, .d3 = 0x00000001, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x0000EF80 } },
    { .name = "swap_dword3_bit1", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000002, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x01000300 } },
    { .name = "swap_dword3_bit3", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000008, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x01000300 } },
    { .name = "swap_dword3_all_E", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x0000000E, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x01000300 } },
    { .name = "swap_dword3_bit0_only_no_swap", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000001, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00004300 } },
    { .name = "swap_with_exp_bias", .flags = 0xFC000010, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x0000000F, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x013F4300 } },
    { .name = "height_bias_3", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x80000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D20500, 0x00000300 } },
    { .name = "height_bias_3_with_swap", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x8000000E, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D20500, 0x01000300 } },
    { .name = "endian_bits_all", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B00C6, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000303 } },
    { .name = "kind_0x200_not_large", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000200, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "kind_0x600_not_large", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000600, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "kind_0_not_large_other_bits", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0xFFFFF9FF, .expect = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000300 } },
    { .name = "kind_0x400_large", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x0003F8FF, .d3 = 0x00000000, .d5 = 0x00000400, .expect = { 0x00100000, 0x1E3B0000, 0x00800500, 0x00000308 } },
    { .name = "kind_0x400_large_other_bits", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0xFFFFFFFF, .d3 = 0x00000000, .d5 = 0xFFFFFDFF, .expect = { 0x00100000, 0x1E3B0000, 0x08000500, 0x00000308 } },
    { .name = "kind_0x400_large_rect_point", .flags = 0x00000000, .hasRect = true, .rectLeft = 16, .rectTop = 8, .hasPoint = true, .pointX = 48, .pointY = 24, .d0 = 0x0A000000, .d1 = 0xFE3AF006, .d2 = 0x0007F9FF, .d3 = 0x00000000, .d5 = 0x00000400, .expect = { 0x00100000, 0x1E3D4C00, 0x00F00500, 0x00000308 } },
    { .name = "kind_0x400_large_format_54_bias3", .flags = 0x00000000, .d0 = 0x0A000000, .d1 = 0x1E3B0036, .d2 = 0x0001F83F, .d3 = 0x80000001, .d5 = 0x00000400, .expect = { 0x00100000, 0x1E3B0000, 0x00420500, 0x00004388 } },
    { .name = "large_slot4_sel0", .flags = 0x00000004, .d0 = 0x0A000000, .d1 = 0x1E3B0016, .d2 = 0x0000F81F, .d3 = 0x00000000, .d5 = 0x00000400, .msaaDepth = 2, .expect = { 0x00000064, 0x1E3B0000, 0x00200500, 0x00000308 } },
    { .name = "height_14bit_mask", .flags = 0x00000000, .hasRect = true, .rectLeft = 0, .rectTop = 0, .hasPoint = true, .pointX = 0, .pointY = 0, .d0 = 0x0A000000, .d1 = 0x1E3B0006, .d2 = 0x03FFFFFF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B0000, 0x20000500, 0x00000300 } },
    { .name = "format_with_table_bpp_varied_f9", .flags = 0x00000000, .hasRect = true, .rectLeft = 8, .rectTop = 8, .hasPoint = true, .pointX = 24, .pointY = 16, .d0 = 0x0A000000, .d1 = 0x1E3B0009, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3B5400, 0x02C80500, 0x00000480 } },
    { .name = "format_with_table_bpp_varied_f12", .flags = 0x00000000, .hasRect = true, .rectLeft = 8, .rectTop = 8, .hasPoint = true, .pointX = 24, .pointY = 16, .d0 = 0x0A000000, .d1 = 0x1E3B000C, .d2 = 0x0059E4FF, .d3 = 0x00000000, .d5 = 0x00000000, .expect = { 0x00100000, 0x1E3BD200, 0x02C80500, 0x00000600 } },
};

void testCases()
{
    for (const auto& c : kCases)
    {
        const auto  memory = build(c);
        std::string why;
        const auto  derived = DeriveResolveWords(readerOf(memory), registers(c), &why);
        UNSCOPED_INFO(std::string(c.name) + ": refused: " + why);
        REQUIRE(derived.has_value());
        UNSCOPED_INFO(std::string(c.name) + ": derived " + hex(*derived) + " expected " + hex(c.expect));
        REQUIRE(*derived == c.expect);
    }
}

void testRefusals()
{
    const Case base{ .name = "base", .flags = 0x10, .expect = {} };
    const auto memory = build(base);
    const auto read   = readerOf(memory);
    const auto r      = registers(base);
    UNSCOPED_INFO("base derives");
    REQUIRE(DeriveResolveWords(read, r).has_value());

    // Only mip 0 and slice 0 are modeled.
    std::string why;
    auto        mip = r;
    mip[5]          = 1;
    UNSCOPED_INFO("mip refused");
    REQUIRE((!DeriveResolveWords(read, mip, &why) && why == "mip_or_slice"));
    auto slice = r;
    slice[6]   = 2;
    UNSCOPED_INFO("slice refused");
    REQUIRE((!DeriveResolveWords(read, slice, &why) && why == "mip_or_slice"));

    // Each word the derivation reads must be readable.
    const auto without = [&](std::uint32_t address, const char* what)
    {
        auto copy = memory;
        copy.erase(address);
        std::string reason;
        UNSCOPED_INFO(std::string(what) + " unreadable is refused");
        REQUIRE(!DeriveResolveWords(readerOf(copy), r, &reason));
        UNSCOPED_INFO(std::string(what) + ": reason " + reason);
        REQUIRE(reason == std::format("unreadable {:08X}", address));
    };
    for (const std::uint32_t offset : { 28U, 32U, 36U, 40U, 48U })
        without(kHeader + offset, "header");
    without(kResolveDefaultPoint, "default point x");
    without(kResolveDefaultPoint + 4, "default point y");
    // Format 6 sits in the table's word at index 6 * 2 / 4.
    without((kResolveFormatTable + 12) & ~3U, "format table");

    const Case boxed{ .name = "boxed", .flags = 0x10, .hasRect = true, .rectLeft = 4, .rectTop = 4, .hasPoint = true, .pointX = 8, .pointY = 8, .expect = {} };
    const auto boxedMemory = build(boxed);
    for (const std::uint32_t address : { kRect, kRect + 4, kPoint, kPoint + 4 })
    {
        auto copy = boxedMemory;
        copy.erase(address);
        UNSCOPED_INFO("rect or point word unreadable is refused");
        REQUIRE(!DeriveResolveWords(readerOf(copy), registers(boxed)));
    }

    // The flags fixup needs the source surface.
    const Case fixup{ .name = "fixup", .flags = 0, .expect = {} };
    const auto fixupMemory = build(fixup);
    const auto fixupRegs   = registers(fixup);
    UNSCOPED_INFO("fixup derives");
    REQUIRE(DeriveResolveWords(readerOf(fixupMemory), fixupRegs).has_value());
    for (const std::uint32_t address : { kG + 0x3098, kSurface + 0x18 })
    {
        auto copy = fixupMemory;
        copy.erase(address);
        UNSCOPED_INFO("fixup surface unreadable is refused");
        REQUIRE(!DeriveResolveWords(readerOf(copy), fixupRegs));
    }
    auto depthRegs = fixupRegs;
    depthRegs[1]   = 4;
    {
        auto copy = fixupMemory;
        copy.erase(kG + 0x30A8);
        UNSCOPED_INFO("depth surface pointer unreadable is refused");
        REQUIRE(!DeriveResolveWords(readerOf(copy), depthRegs));
        copy = fixupMemory;
        copy.erase(kDepthSurface + 0x18);
        UNSCOPED_INFO("depth surface info unreadable is refused");
        REQUIRE(!DeriveResolveWords(readerOf(copy), depthRegs));
    }
    // The depth slot reads G+0x30A8, never the color slots.
    {
        auto copy = fixupMemory;
        for (std::uint32_t slot = 0; slot < 4; ++slot)
            copy.erase(kG + 0x3098 + 4 * slot);
        UNSCOPED_INFO("depth slot ignores the color surfaces");
        REQUIRE(DeriveResolveWords(readerOf(copy), depthRegs).has_value());
    }
    // A null surface pointer has no readable info word.
    {
        auto copy         = fixupMemory;
        copy[kG + 0x3098] = 0;
        UNSCOPED_INFO("null surface is refused");
        REQUIRE(!DeriveResolveWords(readerOf(copy), fixupRegs));
    }

    // Addresses that would wrap are refused before any read.
    auto wrapped = r;
    wrapped[3]   = 0xFFFFFFF0U;
    UNSCOPED_INFO("header overflow refused");
    REQUIRE((!DeriveResolveWords(read, wrapped, &why) && why == "address_overflow"));
    wrapped    = r;
    wrapped[0] = 0xFFFFFFF0U;
    UNSCOPED_INFO("device overflow refused");
    REQUIRE((!DeriveResolveWords(read, wrapped, &why) && why == "address_overflow"));
}

ResolveProbe probeOf([[maybe_unused]] std::uint32_t g, bool admitted = true)
{
    ResolveProbe probe;
    probe.admitted = admitted;
#if REREVVED_DEVELOPER
    probe.g       = g;
    probe.derived = ResolveCopyWords{ g, g + 1, g + 2, g + 3 };
#endif
    return probe;
}

constexpr std::uint32_t frameOf(std::uint32_t entrySp)
{
    return entrySp - kResolveFrameSize;
}

void testProbeStack()
{
    constexpr std::uint32_t outer = 0x70000000, inner = 0x6FFFF000, innermost = 0x6FFFE000;
    unsigned                dropped = 99;

    // The body frame is the entry r1 less 368, the stwu of its prologue.
    UNSCOPED_INFO("frame size");
    REQUIRE(kResolveFrameSize == 368);

    // A nested call: each hook pair finds its own probe, the inner first.
    {
        ResolveProbeStack stack;
        UNSCOPED_INFO("nested push");
        REQUIRE((stack.Push(outer, probeOf(1)) == 0 && stack.Push(inner, probeOf(2)) == 0 && stack.Size() == 2));
        auto* found = stack.Find(frameOf(inner), dropped);
        UNSCOPED_INFO("inner copy hook");
        REQUIRE((found && DEV_CHECK(found->g == 2) && found->frame == frameOf(inner) && dropped == 0 && stack.Size() == 2));
        found = stack.Find(frameOf(inner), dropped);
        UNSCOPED_INFO("inner copy hook is repeatable");
        REQUIRE((found && DEV_CHECK(found->derived->copyControl == 2)));
        auto popped = stack.Pop(frameOf(inner), dropped);
        UNSCOPED_INFO("inner return hook");
        REQUIRE((popped && DEV_CHECK(popped->g == 2) && dropped == 0 && stack.Size() == 1));
        found = stack.Find(frameOf(outer), dropped);
        UNSCOPED_INFO("outer copy hook after the inner returned");
        REQUIRE((found && DEV_CHECK(found->g == 1) && DEV_CHECK(found->derived->destBase == 2) && dropped == 0));
        popped = stack.Pop(frameOf(outer), dropped);
        UNSCOPED_INFO("outer return hook");
        REQUIRE((popped && DEV_CHECK(popped->g == 1) && dropped == 0 && !stack.Size()));
    }
    // The inner call can run before the outer copy hook, and the outer hooks still pair.
    {
        ResolveProbeStack stack;
        stack.Push(outer, probeOf(1));
        stack.Push(inner, probeOf(2));
        UNSCOPED_INFO("inner returns first");
        const bool  innerPopped = static_cast<bool>(stack.Pop(frameOf(inner), dropped));
        const auto* outerFound  = stack.Find(frameOf(outer), dropped);
        REQUIRE((innerPopped && outerFound && DEV_CHECK(outerFound->g == 1)));
    }
    // A callback that tail-jumps to Resolve enters with exactly the outer body r1; the outer stays.
    {
        ResolveProbeStack stack;
        stack.Push(outer, probeOf(1));
        UNSCOPED_INFO("tail-jumped entry keeps the outer");
        REQUIRE((stack.Push(frameOf(outer), probeOf(2)) == 0 && stack.Size() == 2));
        UNSCOPED_INFO("tail-jumped pairs");
        const auto second = stack.Pop(frameOf(frameOf(outer)), dropped);
        const auto first  = stack.Pop(frameOf(outer), dropped);
        REQUIRE((second && first && DEV_CHECK(second->g == 2 && first->g == 1)));
    }
    // A call the observer rejected has a placeholder: its hooks find it, never the outer probe.
    {
        ResolveProbeStack stack;
        stack.Push(outer, probeOf(1));
        stack.Push(inner, probeOf(2, false));
        auto* found = stack.Find(frameOf(inner), dropped);
        UNSCOPED_INFO("rejected inner finds its placeholder");
        REQUIRE((found && !found->admitted && dropped == 0));
        UNSCOPED_INFO("rejected inner returns");
        REQUIRE((stack.Pop(frameOf(inner), dropped) && stack.Size() == 1));
        found = stack.Find(frameOf(outer), dropped);
        UNSCOPED_INFO("outer untouched by the rejected inner");
        REQUIRE((found && found->admitted && DEV_CHECK(found->g == 1) && DEV_CHECK(!found->hookRead)));
        // A frame that matches nothing disturbs nothing and counts nothing.
        UNSCOPED_INFO("unmatched hook leaves the stack");
        REQUIRE((!stack.Find(frameOf(inner), dropped) && dropped == 0 && stack.Size() == 1));
        UNSCOPED_INFO("unmatched return leaves the stack");
        REQUIRE((!stack.Pop(frameOf(inner), dropped) && dropped == 0 && stack.Size() == 1));
    }
    // An inner call that never reached its return hook: the outer return drops it and counts it.
    {
        ResolveProbeStack stack;
        stack.Push(outer, probeOf(1));
        stack.Push(inner, probeOf(2));
        stack.Push(innermost, probeOf(3));
        auto popped = stack.Pop(frameOf(outer), dropped);
        UNSCOPED_INFO("skipped returns above the outer are dropped");
        REQUIRE((popped && DEV_CHECK(popped->g == 1) && dropped == 2 && !stack.Size()));
    }
    // The copy hook drops a stale probe above too.
    {
        ResolveProbeStack stack;
        stack.Push(outer, probeOf(1));
        stack.Push(inner, probeOf(2));
        auto* found = stack.Find(frameOf(outer), dropped);
        UNSCOPED_INFO("copy hook drops the stale inner");
        REQUIRE((found && DEV_CHECK(found->g == 1) && dropped == 1 && stack.Size() == 1));
    }
    // A later call that enters at or above a stale probe frame replaces it, even at the same r1.
    {
        ResolveProbeStack stack;
        stack.Push(inner, probeOf(1));
        UNSCOPED_INFO("same entry r1 drops the stale probe");
        REQUIRE((stack.Push(inner, probeOf(2, false)) == 1 && stack.Size() == 1));
        auto* found = stack.Find(frameOf(inner), dropped);
        UNSCOPED_INFO("the new call owns the frame");
        REQUIRE((found && DEV_CHECK(found->g == 2) && !found->admitted));
        stack.Clear();
        stack.Push(inner, probeOf(1));
        UNSCOPED_INFO("a shallower entry drops the stale probe");
        REQUIRE((stack.Push(outer, probeOf(2)) == 1 && stack.Size() == 1));
        UNSCOPED_INFO("shallower call pairs");
        const auto* shallow = stack.Find(frameOf(outer), dropped);
        REQUIRE((shallow && DEV_CHECK(shallow->g == 2)));
    }
    // Overflow: the fifth nested call is not kept and counts one; the others pair.
    {
        ResolveProbeStack stack;
        std::uint32_t     sp = outer;
        for (std::uint32_t i = 0; i < ResolveProbeStack::kDepth; ++i, sp -= 0x1000)
        {
            UNSCOPED_INFO("fill the stack");
            REQUIRE(stack.Push(sp, probeOf(i + 1)) == 0);
        }
        UNSCOPED_INFO("full");
        REQUIRE(stack.Size() == ResolveProbeStack::kDepth);
        UNSCOPED_INFO("overflow counts one and keeps the stack");
        REQUIRE((stack.Push(sp, probeOf(9)) == 1 && stack.Size() == ResolveProbeStack::kDepth));
        UNSCOPED_INFO("the overflowed call finds nothing");
        REQUIRE((!stack.Find(frameOf(sp), dropped) && dropped == 0));
        sp = outer - 0x1000 * (ResolveProbeStack::kDepth - 1);
        for (std::uint32_t i = ResolveProbeStack::kDepth; i > 0; --i, sp += 0x1000)
        {
            const auto popped = stack.Pop(frameOf(sp), dropped);
            UNSCOPED_INFO("overflowed stack still pairs");
            REQUIRE((popped && DEV_CHECK(popped->g == i) && dropped == 0));
        }
        UNSCOPED_INFO("emptied");
        REQUIRE(!stack.Size());
    }
    // A shallow entry flushes a stack of stale probes and is kept.
    {
        ResolveProbeStack stack;
        std::uint32_t     sp = outer - 0x10000;
        for (std::uint32_t i = 0; i < ResolveProbeStack::kDepth; ++i, sp -= 0x1000)
            stack.Push(sp, probeOf(i + 1));
        UNSCOPED_INFO("a shallow entry flushes the stale stack");
        REQUIRE((stack.Push(outer, probeOf(7)) == ResolveProbeStack::kDepth && stack.Size() == 1));
    }
}

#if REREVVED_DEVELOPER
void testComparison()
{
    const ResolveCopyWords a{ 1, 2, 3, 4 };
    UNSCOPED_INFO("equal words match");
    REQUIRE(ResolveWordMismatches(a, a) == 0);
    UNSCOPED_INFO("copy control bit");
    REQUIRE(ResolveWordMismatches(a, { 9, 2, 3, 4 }) == 1);
    UNSCOPED_INFO("dest base bit");
    REQUIRE(ResolveWordMismatches(a, { 1, 9, 3, 4 }) == 2);
    UNSCOPED_INFO("dest pitch bit");
    REQUIRE(ResolveWordMismatches(a, { 1, 2, 9, 4 }) == 4);
    UNSCOPED_INFO("dest info bit");
    REQUIRE(ResolveWordMismatches(a, { 1, 2, 3, 9 }) == 8);
    UNSCOPED_INFO("all words");
    REQUIRE(ResolveWordMismatches(a, { 9, 9, 9, 9 }) == 15);

    TakeResolveDeriveCounts();
    UNSCOPED_INFO("ok counts no mismatch");
    REQUIRE(NoteResolveDerive(a, a) == 0);
    UNSCOPED_INFO("info mismatch mask");
    REQUIRE(NoteResolveDerive(a, { 1, 2, 3, 9 }) == 8);
    UNSCOPED_INFO("two-word mismatch mask");
    REQUIRE(NoteResolveDerive(a, { 9, 2, 9, 4 }) == 5);
    UNSCOPED_INFO("unmodeled counts no mismatch");
    REQUIRE(NoteResolveDerive(std::nullopt, a) == 0);
    NoteResolveUnpaired(0);
    NoteResolveUnpaired(2);
    NoteResolveUnpaired(1);
    NoteResolveClearWords(false);
    NoteResolveClearWords(true);
    NoteResolveClearWords(true);
    auto counts = TakeResolveDeriveCounts();
    UNSCOPED_INFO("ok and unsupported");
    REQUIRE((counts.ok == 1 && counts.unsupported == 1));
    UNSCOPED_INFO("per-word mismatches");
    REQUIRE((counts.mismatch[0] == 1 && counts.mismatch[1] == 0 && counts.mismatch[2] == 1 && counts.mismatch[3] == 1));
    UNSCOPED_INFO("unpaired");
    REQUIRE(counts.unpaired == 3);
    UNSCOPED_INFO("clear words");
    REQUIRE((counts.clearChecked == 3 && counts.clearStale == 2));
    const std::string expected =
        " resolve_derive_ok=1 resolve_derive_mismatch_copy_control=1 resolve_derive_mismatch_dest_base=0 resolve_derive_mismatch_dest_pitch=1"
        " resolve_derive_mismatch_dest_info=1 resolve_derive_unsupported=1 resolve_derive_unpaired=3 resolve_clear_words_checked=3 resolve_clear_words_stale=2";
    UNSCOPED_INFO("formatted counts: " + FormatResolveDeriveCounts(counts));
    REQUIRE(FormatResolveDeriveCounts(counts) == expected);
    counts = TakeResolveDeriveCounts();
    UNSCOPED_INFO("take resets");
    REQUIRE((counts.ok == 0 && counts.unsupported == 0 && counts.unpaired == 0 && counts.clearChecked == 0 && counts.clearStale == 0 && counts.mismatch == std::array<std::uint64_t, kResolveWordCount>{}));
}

#endif

} // namespace

TEST_CASE("resolve_derive", "[ctest][resolve-derive][timeout-60]")
{
    testCases();
    testRefusals();
    testProbeStack();
#if REREVVED_DEVELOPER
    testComparison();
#endif
}
