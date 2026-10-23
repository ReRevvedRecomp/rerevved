// clear_derive: expected calls follow literal generated-code emulation
// (g216:12892-13061; g158:12972-13932).

#include "gpu/guest_d3d/clear_derive.h"

#include "developer_checks.hpp"

#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <climits>
#include <cmath>
#include <cstdint>
#include <format>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace rerevved::gpu::guest_d3d;

namespace
{

constexpr std::uint32_t kG = 0x40010000, kRect = 0x40021000, kColor = 0x40022000, kSurface = 0x40030000, kDepthSurface = 0x40031000, kSp = 0x70000000;

constexpr std::array<std::uint32_t, 4> kUserColor{ 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 };
constexpr std::array<std::uint32_t, 4> kDefaultColor{ 0x3DCCCCCD, 0x3E4CCCCD, 0x3E99999A, 0x3ECCCCCD };

// A surface extent word: width - 1 in bits 18..31 and height - 1 in bits 3..17,
// with bit 1 set so a wrong mask shows.
constexpr std::uint32_t E(int width, int height)
{
    return (static_cast<std::uint32_t>(width - 1) & 0x3FFF) << 18 | (static_cast<std::uint32_t>(height - 1) & 0x7FFF) << 3 | 0x2;
}

// One expected call of the 0x826A1D90 list; the depth and stencil come from the case.
struct Expected
{
    std::uint32_t                flags;
    std::int32_t                 index;
    std::array<std::int32_t, 4>  rect;
    std::uint32_t                colorPointer;
    std::array<std::uint32_t, 4> color; // the words at colorPointer when the call clears color
};

struct Case
{
    const char*                  name;
    std::uint32_t                flags          = 0;
    std::array<std::int32_t, 4>  rect           = {};
    bool                         hasRect        = false;
    bool                         hasColor       = true; // r6 points at kUserColor; otherwise 0
    std::uint64_t                depthBits      = 0x3FF0000000000000ULL;
    std::uint32_t                stencil        = 0x55;
    std::array<bool, 4>          rt             = { true, false, false, false };
    bool                         depthBound     = true;
    std::array<std::uint32_t, 5> extent         = { E(1280, 720), E(1280, 720), E(1280, 720), E(1280, 720), E(1280, 720) };
    std::uint32_t                tileByte       = 0;
    bool                         savedFromBound = false; // G+0x31B8.. hold the bound surfaces
    std::array<std::uint32_t, 5> saved          = {};
    std::array<std::uint32_t, 2> screen         = {};
    std::array<float, 4>         viewport       = { 0.0f, 0.0f, 1280.0f, 720.0f };
    std::uint32_t                scissorEnable  = 0;
    std::array<std::int32_t, 4>  scissor        = {};
    std::uint32_t                sp             = kSp;
    std::vector<Expected>        expect;
};

using Memory = std::unordered_map<std::uint32_t, std::uint32_t>;

Memory build(const Case& c)
{
    Memory memory;
    for (std::uint32_t k = 0; k < 4; ++k)
    {
        memory[kG + 0x3098 + 4 * k]         = c.rt[k] ? kSurface + 0x100 * k : 0;
        memory[kSurface + 0x100 * k + 0x24] = c.extent[k];
    }
    memory[kG + 0x30A8]          = c.depthBound ? kDepthSurface : 0;
    memory[kDepthSurface + 0x24] = c.extent[4];
    memory[kG + 0x2ABC]          = c.tileByte << 24;
    for (std::uint32_t i = 0; i < 5; ++i)
        memory[kG + 0x31B8 + 4 * i] = c.savedFromBound ? memory[kG + 0x3098 + 4 * i] : c.saved[i];
    memory[kG + 0x337C] = c.screen[0];
    memory[kG + 0x3380] = c.screen[1];
    for (std::uint32_t i = 0; i < 4; ++i)
    {
        memory[kG + 0x3168 + 4 * i]        = std::bit_cast<std::uint32_t>(c.viewport[i]);
        memory[kG + 0x3184 + 4 * i]        = static_cast<std::uint32_t>(c.scissor[i]);
        memory[kColor + 4 * i]             = kUserColor[i];
        memory[kClearDefaultColor + 4 * i] = kDefaultColor[i];
    }
    memory[kG + 0x2E50] = c.scissorEnable;
    if (c.hasRect)
        for (std::uint32_t i = 0; i < 4; ++i)
            memory[kRect + 4 * i] = static_cast<std::uint32_t>(c.rect[i]);
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

ClearEntry entryOf(const Case& c)
{
    ClearEntry entry;
    entry.sp      = c.sp;
    entry.g       = kG;
    entry.flags   = c.flags;
    entry.rect    = c.hasRect ? kRect : 0;
    entry.color   = c.hasColor ? kColor : 0;
    entry.depth   = std::bit_cast<double>(c.depthBits);
    entry.stencil = c.stencil;
    return entry;
}

constexpr std::uint32_t frameOf(std::uint32_t entrySp)
{
    return entrySp - kClearFrameSize;
}

ClearCall callOf(const Case& c, const Expected& e)
{
    ClearCall call;
    call.flags        = e.flags;
    call.index        = static_cast<std::uint32_t>(e.index);
    call.rect         = e.rect;
    call.colorPointer = e.colorPointer;
    call.depthBits    = c.depthBits;
    call.stencil      = c.stencil;
    if (call.ColorUsed())
        call.color = e.color;
    return call;
}

ClearCalls expectedOf(const Case& c)
{
    ClearCalls calls;
    for (const auto& e : c.expect)
        calls.Add(callOf(c, e));
    return calls;
}

bool same(const ClearCalls& a, const ClearCalls& b)
{
    if (a.count != b.count || a.overflow != b.overflow)
        return false;
    for (std::size_t i = 0; i < a.count; ++i)
        if (!(a.calls[i] == b.calls[i]))
            return false;
    return true;
}

// Cases vary base inputs; expected calls follow generated order.
// Calls emulate Clear wrapper 0x826A2970/body 0x826A22F8 and 0x826A1D90,
// independently of clear_derive.cpp.
const Case kCases[] = {
    { .name = "null_rect_rt0", .flags = 0x00000001, .expect = { { 0x00000001, 0, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "null_rect_rt0_not_depth_extent", .flags = 0x00000001, .extent = { E(640, 360), E(640, 360), E(640, 360), E(640, 360), E(1024, 512) }, .viewport = { 0.0f, 0.0f, 4000.0f, 4000.0f }, .expect = { { 0x00000001, 0, { 0, 0, 640, 360 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "null_rect_depth_header_rt0_unbound", .flags = 0x00000030, .rt = { false, false, false, false }, .extent = { E(1280, 720), E(1280, 720), E(1280, 720), E(1280, 720), E(1024, 512) }, .expect = { { 0x00000030, -1, { 0, 0, 1024, 512 }, 0x40022000, {} } } },
    { .name = "null_rect_rt1_bound_only_uses_depth", .flags = 0x00000002, .rt = { false, true, false, false }, .extent = { E(640, 360), E(640, 360), E(640, 360), E(640, 360), E(1024, 512) }, .viewport = { 0.0f, 0.0f, 4000.0f, 4000.0f }, .expect = { { 0x00000002, 1, { 0, 0, 1024, 512 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "rect_given", .flags = 0x00000001, .rect = { 16, 8, 200, 100 }, .hasRect = true, .expect = { { 0x00000001, 0, { 16, 8, 200, 100 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "rect_given_wide_not_clipped_to_surface", .flags = 0x00000001, .rect = { 0, 0, 1300, 900 }, .hasRect = true, .viewport = { 0.0f, 0.0f, 4000.0f, 4000.0f }, .expect = { { 0x00000001, 0, { 0, 0, 1300, 900 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "tile_override_byte_0x10", .flags = 0x00000001, .extent = { E(640, 360), E(640, 360), E(640, 360), E(640, 360), E(640, 360) }, .tileByte = 0x10, .screen = { 1280, 720 }, .viewport = { 0.0f, 0.0f, 4000.0f, 4000.0f }, .expect = { { 0x00000001, 0, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "tile_override_byte_0x20_saved_match", .flags = 0x00000003, .rt = { true, true, false, false }, .extent = { E(640, 360), E(640, 360), E(640, 360), E(640, 360), E(640, 360) }, .tileByte = 0x20, .savedFromBound = true, .screen = { 1280, 720 }, .viewport = { 0.0f, 0.0f, 4000.0f, 4000.0f }, .expect = { { 0x00000003, 0, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } }, { 0x00000003, 1, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "tile_byte_0x20_saved_mismatch_rt1", .flags = 0x00000003, .rt = { true, true, false, false }, .extent = { E(640, 360), E(640, 360), E(640, 360), E(640, 360), E(640, 360) }, .tileByte = 0x20, .saved = { 0x40030000, 0x40030500, 0x0, 0x0, 0x40031000 }, .screen = { 1280, 720 }, .viewport = { 0.0f, 0.0f, 4000.0f, 4000.0f }, .expect = { { 0x00000003, 0, { 0, 0, 640, 360 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } }, { 0x00000003, 1, { 0, 0, 640, 360 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "tile_byte_0x20_unbound_slots_pass", .flags = 0x00000002, .rt = { false, true, false, false }, .extent = { E(640, 360), E(640, 360), E(640, 360), E(640, 360), E(640, 360) }, .tileByte = 0x20, .saved = { 0x1234, 0x40030100, 0x7, 0x9, 0x40031000 }, .screen = { 1280, 720 }, .viewport = { 0.0f, 0.0f, 4000.0f, 4000.0f }, .expect = { { 0x00000002, 1, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "tile_byte_0x20_depth_mismatch", .flags = 0x00000001, .extent = { E(640, 360), E(640, 360), E(640, 360), E(640, 360), E(640, 360) }, .tileByte = 0x20, .saved = { 0x40030000, 0x0, 0x0, 0x0, 0x40031040 }, .screen = { 1280, 720 }, .viewport = { 0.0f, 0.0f, 4000.0f, 4000.0f }, .expect = { { 0x00000001, 0, { 0, 0, 640, 360 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "tile_byte_0x30_first_bit_wins", .flags = 0x00000001, .extent = { E(640, 360), E(640, 360), E(640, 360), E(640, 360), E(640, 360) }, .tileByte = 0x30, .saved = { 0x5555, 0x0, 0x0, 0x0, 0x0 }, .screen = { 1280, 720 }, .viewport = { 0.0f, 0.0f, 4000.0f, 4000.0f }, .expect = { { 0x00000001, 0, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "tile_byte_other_bits_no_override", .flags = 0x00000001, .extent = { E(640, 360), E(640, 360), E(640, 360), E(640, 360), E(640, 360) }, .tileByte = 0xCF, .screen = { 1280, 720 }, .viewport = { 0.0f, 0.0f, 4000.0f, 4000.0f }, .expect = { { 0x00000001, 0, { 0, 0, 640, 360 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "tile_ignored_with_rect", .flags = 0x00000001, .rect = { 0, 0, 300, 200 }, .hasRect = true, .tileByte = 0x10, .screen = { 1280, 720 }, .expect = { { 0x00000001, 0, { 0, 0, 300, 200 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "depth_unbound_masks_depth_flags", .flags = 0x00000031, .depthBound = false, .expect = { { 0x00000001, 0, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "depth_unbound_only_depth_flags_no_calls", .flags = 0x00000030, .depthBound = false, .expect = {} },
    { .name = "depth_unbound_zero_flags_no_calls", .flags = 0x00000000, .depthBound = false, .expect = {} },
    { .name = "depth_unbound_keeps_high_flags", .flags = 0x00000300, .depthBound = false, .expect = { { 0x00000300, -1, { 0, 0, 1280, 720 }, 0x40022000, {} } } },
    { .name = "zero_flags_depth_bound_single_call", .flags = 0x00000000, .expect = { { 0x00000000, -1, { 0, 0, 1280, 720 }, 0x40022000, {} } } },
    { .name = "depth_only_flags", .flags = 0x00000030, .rt = { false, false, false, false }, .expect = { { 0x00000030, -1, { 0, 0, 1280, 720 }, 0x40022000, {} } } },
    { .name = "depth_only_flags_with_rt0_bound", .flags = 0x00000020, .expect = { { 0x00000020, -1, { 0, 0, 1280, 720 }, 0x40022000, {} } } },
    { .name = "flags_high_bits_pass_through", .flags = 0x00000301, .expect = { { 0x00000301, 0, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "viewport_fractional", .flags = 0x00000001, .viewport = { 10.9f, 20.5f, 100.9f, 50.99f }, .expect = { { 0x00000001, 0, { 10, 20, 110, 70 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "viewport_negative_fractional", .flags = 0x00000001, .viewport = { -7.9f, -0.5f, 1300.5f, 800.2f }, .expect = { { 0x00000001, 0, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "viewport_just_below_integer", .flags = 0x00000001, .viewport = { 0.0f, 0.0f, 1279.9999f, 719.999f }, .expect = { { 0x00000001, 0, { 0, 0, 1279, 719 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "viewport_negative_origin_with_negative_rect", .flags = 0x00000001, .rect = { -50, -40, 200, 100 }, .hasRect = true, .viewport = { -20.5f, -10.5f, 500.0f, 500.0f }, .expect = { { 0x00000001, 0, { -20, -10, 200, 100 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "viewport_x_saturates_low", .flags = 0x00000001, .viewport = { -3000000000.0f, 0.0f, 3000000000.0f, 720.0f }, .expect = {} },
    { .name = "viewport_width_saturates_high", .flags = 0x00000001, .viewport = { 5.0f, 0.0f, 3000000000.0f, 720.0f }, .expect = {} },
    { .name = "viewport_nan_origin", .flags = 0x00000001, .viewport = { NAN, 0.0f, 1280.0f, 720.0f }, .expect = {} },
    { .name = "viewport_infinities", .flags = 0x00000001, .viewport = { -INFINITY, -INFINITY, INFINITY, INFINITY }, .expect = {} },
    { .name = "viewport_zero_size_empty", .flags = 0x00000001, .viewport = { 0.0f, 0.0f, 0.0f, 720.0f }, .expect = {} },
    { .name = "viewport_fractional_size_below_one_empty", .flags = 0x00000001, .viewport = { 0.0f, 0.0f, 0.99f, 720.0f }, .expect = {} },
    { .name = "scissor_clips", .flags = 0x00000001, .scissorEnable = 0x1, .scissor = { 100, 50, 900, 600 }, .expect = { { 0x00000001, 0, { 100, 50, 900, 600 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "scissor_flag_negative_counts_enabled", .flags = 0x00000001, .scissorEnable = 0xFFFFFFFF, .scissor = { 100, 50, 900, 600 }, .expect = { { 0x00000001, 0, { 100, 50, 900, 600 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "scissor_disabled_ignored", .flags = 0x00000001, .scissorEnable = 0x0, .scissor = { 100, 50, 900, 600 }, .expect = { { 0x00000001, 0, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "scissor_wider_than_viewport", .flags = 0x00000001, .viewport = { 10.0f, 10.0f, 100.0f, 100.0f }, .scissorEnable = 0x1, .scissor = { 0, 0, 4000, 4000 }, .expect = { { 0x00000001, 0, { 10, 10, 110, 110 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "scissor_negative_values", .flags = 0x00000001, .rect = { -100, -100, 300, 300 }, .hasRect = true, .viewport = { -500.0f, -500.0f, 2000.0f, 2000.0f }, .scissorEnable = 0x1, .scissor = { -60, -30, 250, 220 }, .expect = { { 0x00000001, 0, { -60, -30, 250, 220 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "scissor_empty_width", .flags = 0x00000001, .scissorEnable = 0x1, .scissor = { 300, 0, 300, 720 }, .expect = {} },
    { .name = "scissor_inverted_empty", .flags = 0x00000001, .scissorEnable = 0x2, .scissor = { 900, 0, 100, 720 }, .expect = {} },
    { .name = "scissor_empty_height", .flags = 0x00000001, .scissorEnable = 0x1, .scissor = { 0, 400, 1280, 400 }, .expect = {} },
    { .name = "rect_empty_width", .flags = 0x00000001, .rect = { 40, 0, 40, 100 }, .hasRect = true, .expect = {} },
    { .name = "rect_inverted_empty_height", .flags = 0x00000001, .rect = { 0, 50, 100, 20 }, .hasRect = true, .expect = {} },
    { .name = "mrt_all_bound", .flags = 0x0000003F, .rt = { true, true, true, true }, .expect = { { 0x0000003F, 0, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } }, { 0x0000000F, 1, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } }, { 0x0000000F, 2, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } }, { 0x0000000F, 3, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "mrt_missing_middle_target", .flags = 0x0000003F, .rt = { true, false, true, true }, .expect = { { 0x0000003F, 0, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } }, { 0x0000000F, 2, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } }, { 0x0000000F, 3, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "mrt_first_target_missing_keeps_depth_bits", .flags = 0x0000003F, .rt = { false, true, true, false }, .expect = { { 0x0000003F, 1, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } }, { 0x0000000F, 2, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "mrt_skipped_target_retains_depth_bits", .flags = 0x0000003A, .rt = { true, false, true, true }, .expect = { { 0x0000003A, 3, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "mrt_skipped_target_second_call_clean", .flags = 0x0000003F, .rt = { false, true, true, true }, .expect = { { 0x0000003F, 1, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } }, { 0x0000000F, 2, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } }, { 0x0000000F, 3, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "mrt_flag_bit_clear_target_bound_skipped", .flags = 0x00000005, .rt = { true, true, true, true }, .expect = { { 0x00000005, 0, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } }, { 0x00000005, 2, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "mrt_no_bound_targets_no_calls", .flags = 0x00000001, .rt = { false, false, false, false }, .expect = {} },
    { .name = "mrt_selected_unbound_only_no_calls", .flags = 0x00000004, .rt = { true, true, false, true }, .expect = {} },
    { .name = "mrt_selected_unbound_pair_no_calls", .flags = 0x0000000A, .rt = { true, false, true, false }, .expect = {} },
    { .name = "mrt_single_target_with_depth_bits", .flags = 0x00000031, .expect = { { 0x00000031, 0, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "default_color_pointer", .flags = 0x00000001, .hasColor = false, .expect = { { 0x00000001, 0, { 0, 0, 1280, 720 }, 0x6FFFFF00, { 0x3DCCCCCD, 0x3E4CCCCD, 0x3E99999A, 0x3ECCCCCD } } } },
    { .name = "default_color_depth_only", .flags = 0x00000030, .hasColor = false, .expect = { { 0x00000030, -1, { 0, 0, 1280, 720 }, 0x6FFFFF00, {} } } },
    { .name = "default_color_mrt", .flags = 0x00000007, .hasColor = false, .rt = { true, true, true, false }, .expect = { { 0x00000007, 0, { 0, 0, 1280, 720 }, 0x6FFFFF00, { 0x3DCCCCCD, 0x3E4CCCCD, 0x3E99999A, 0x3ECCCCCD } }, { 0x00000007, 1, { 0, 0, 1280, 720 }, 0x6FFFFF00, { 0x3DCCCCCD, 0x3E4CCCCD, 0x3E99999A, 0x3ECCCCCD } }, { 0x00000007, 2, { 0, 0, 1280, 720 }, 0x6FFFFF00, { 0x3DCCCCCD, 0x3E4CCCCD, 0x3E99999A, 0x3ECCCCCD } } } },
    { .name = "depth_nan_payload_and_high_stencil", .flags = 0x00000031, .depthBits = 0x7FF8000000000123ULL, .stencil = 0xFFFFFF80U, .expect = { { 0x00000031, 0, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "depth_negative_zero", .flags = 0x00000031, .depthBits = 0x8000000000000000ULL, .stencil = 0x00000000U, .expect = { { 0x00000031, 0, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
    { .name = "nested_entry_sp", .flags = 0x00000001, .sp = 0x6FFFE000, .expect = { { 0x00000001, 0, { 0, 0, 1280, 720 }, 0x40022000, { 0x3F800000, 0x3F000000, 0x3E800000, 0x3F400000 } } } },
};

#if REREVVED_DEVELOPER
Memory withHookMemory(const Case& c)
{
    auto memory                = build(c);
    memory[frameOf(c.sp) + 92] = c.stencil;
    for (std::uint32_t i = 0; i < 4; ++i)
        memory[(c.hasColor ? kColor : frameOf(c.sp) + kClearBodyColorOffset) + 4 * i] = c.hasColor ? kUserColor[i] : kDefaultColor[i];
    return memory;
}
#endif

void testCases()
{
    for (const auto& c : kCases)
    {
        const auto  memory = build(c);
        std::string why;
        const auto  derived = DeriveClearCalls(readerOf(memory), entryOf(c), &why);
        UNSCOPED_INFO(std::string(c.name) + ": refused: " + why);
        REQUIRE(derived.has_value());
        const auto expected = expectedOf(c);
#if REREVVED_DEVELOPER
        UNSCOPED_INFO(std::string(c.name) + ": derived " + FormatClearCalls(*derived) + " expected " + FormatClearCalls(expected));
#endif
        REQUIRE(same(*derived, expected));
    }
}

#if REREVVED_DEVELOPER
void testObservedRoundTrip()
{
    for (const auto& c : kCases)
    {
        const auto memory = withHookMemory(c);
        ClearCalls observed;
        for (const auto& e : c.expect)
        {
            ClearCall                          call;
            std::string                        why;
            const std::array<std::uint32_t, 8> r{ kG, e.flags, static_cast<std::uint32_t>(e.index), static_cast<std::uint32_t>(e.rect[0]), static_cast<std::uint32_t>(e.rect[1]), static_cast<std::uint32_t>(e.rect[2]), static_cast<std::uint32_t>(e.rect[3]), e.colorPointer };
            UNSCOPED_INFO(std::string(c.name) + ": observe refused: " + why);
            REQUIRE(ObserveClearCall(readerOf(memory), r, frameOf(c.sp), std::bit_cast<double>(c.depthBits), call, &why));
            observed.Add(call);
        }
        const auto derived = DeriveClearCalls(readerOf(build(c)), entryOf(c));
        UNSCOPED_INFO(std::string(c.name) + ": the hook's view differs from the derivation");
        REQUIRE((derived && same(observed, *derived) && ClearMismatches(*derived, observed) == 0));
    }
}
#endif

void testFloatConversion()
{
    struct Row
    {
        float        value;
        std::int32_t expected;
    };

    // fctiwz as written at g158:13038-13059: truncation toward zero, INT_MAX
    // from 2147483647.0, INT_MIN for NaN, -inf and values below -2^31.
    const Row rows[] = {
        { 0.0f, 0 },
        { -0.0f, 0 },
        { 0.9999f, 0 },
        { -0.9999f, 0 },
        { 1.5f, 1 },
        { -1.5f, -1 },
        { 1279.9999f, 1279 },
        { -7.9f, -7 },
        { 2147483520.0f, 2147483520 },
        { 2147483648.0f, INT_MAX },
        { 3.0e9f, INT_MAX },
        { INFINITY, INT_MAX },
        { -2147483648.0f, INT_MIN },
        { -3.0e9f, INT_MIN },
        { -INFINITY, INT_MIN },
        { NAN, INT_MIN },
        { -2147483520.0f, -2147483520 },
    };
    for (const auto& row : rows)
    {
        UNSCOPED_INFO(std::format("fctiwz({}) = {}, expected {}", row.value, ClearFloatToInt32(row.value), row.expected));
        REQUIRE(ClearFloatToInt32(row.value) == row.expected);
    }
}

// Every guest word the derivation reads must be readable; a missing one is
// refused with its address.
std::set<std::uint32_t> readsOf(const Case& c)
{
    const auto              memory = build(c);
    std::set<std::uint32_t> reads;
    const auto              reader = [&](std::uint32_t address, std::uint32_t& value)
    {
        reads.insert(address);
        return readerOf(memory)(address, value);
    };
    UNSCOPED_INFO(std::string(c.name) + ": derives");
    REQUIRE(DeriveClearCalls(reader, entryOf(c)).has_value());
    return reads;
}

void testRefusals()
{
    // Null rectangle, 0x20 tiling with every slot compared, a scissor, four
    // bound targets and an explicit color.
    const Case wide{ .name = "wide", .flags = 0x3F, .rt = { true, true, true, true }, .tileByte = 0x20, .savedFromBound = true, .screen = { 1280, 720 }, .scissorEnable = 1, .scissor = { 0, 0, 1280, 720 }, .expect = {} };
    // The same with the default color, and a given rectangle with a depth-only call.
    const Case defaulted{ .name = "defaulted", .flags = 0x3F, .hasColor = false, .rt = { true, true, true, true }, .scissorEnable = 1, .scissor = { 0, 0, 1280, 720 }, .expect = {} };
    const Case boxed{ .name = "boxed", .flags = 0x30, .rect = { 0, 0, 100, 100 }, .hasRect = true, .expect = {} };
    for (const Case* c : { &wide, &defaulted, &boxed })
    {
        const auto reads = readsOf(*c);
        UNSCOPED_INFO(std::string(c->name) + ": reads");
        REQUIRE(reads.size() >= 10);
        for (const auto address : reads)
        {
            auto memory = build(*c);
            memory.erase(address);
            std::string why;
            UNSCOPED_INFO(std::string(c->name) + ": " + std::format("{:08X}", address) + " unreadable is refused");
            REQUIRE(!DeriveClearCalls(readerOf(memory), entryOf(*c), &why));
            UNSCOPED_INFO(std::string(c->name) + ": reason " + why);
            REQUIRE(why == std::format("unreadable {:08X}", address));
        }
    }
    // The fixed set of reads of the widest case, by address.
    {
        const auto              reads = readsOf(wide);
        std::set<std::uint32_t> expected;
        for (std::uint32_t k = 0; k < 4; ++k)
            expected.insert(kG + 0x3098 + 4 * k);
        for (const std::uint32_t offset : { 0x30A8U, 0x2ABCU, 0x337CU, 0x3380U, 0x2E50U, 0x3168U, 0x316CU, 0x3170U, 0x3174U, 0x3184U, 0x3188U, 0x318CU, 0x3190U, 0x31B8U, 0x31BCU, 0x31C0U, 0x31C4U, 0x31C8U })
            expected.insert(kG + offset);
        expected.insert(kSurface + 0x24);
        for (std::uint32_t i = 0; i < 4; ++i)
            expected.insert(kColor + 4 * i);
        UNSCOPED_INFO("the widest case reads exactly the guest words of the generated body");
        REQUIRE(reads == expected);
    }

    // A null rectangle needs a bound target; the guest would read address 0x24.
    {
        const Case  none{ .name = "none", .flags = 0x1, .rt = { false, false, false, false }, .depthBound = false, .expect = {} };
        const auto  memory = build(none);
        std::string why;
        UNSCOPED_INFO("null rectangle with no bound target is refused: " + why);
        REQUIRE((!DeriveClearCalls(readerOf(memory), entryOf(none), &why) && why == "unbound_target_for_null_rect"));
        // With a given rectangle and no depth surface the same device derives.
        const Case boxedNone{ .name = "boxedNone", .flags = 0x1, .rect = { 0, 0, 10, 10 }, .hasRect = true, .rt = { true, false, false, false }, .depthBound = false, .expect = {} };
        UNSCOPED_INFO("a given rectangle needs no depth surface");
        REQUIRE(DeriveClearCalls(readerOf(build(boxedNone)), entryOf(boxedNone)).has_value());
    }

    // Addresses that would wrap are refused before any read.
    {
        const Case  base{ .name = "base", .flags = 0x1, .expect = {} };
        const auto  memory = build(base);
        const auto  read   = readerOf(memory);
        std::string why;
        auto        entry = entryOf(base);
        UNSCOPED_INFO("base derives");
        REQUIRE(DeriveClearCalls(read, entry).has_value());
        auto wrapped = entry;
        wrapped.g    = 0xFFFFFFF0U;
        UNSCOPED_INFO("device overflow refused");
        REQUIRE((!DeriveClearCalls(read, wrapped, &why) && why == "address_overflow"));
        wrapped      = entry;
        wrapped.rect = 0xFFFFFFF8U;
        UNSCOPED_INFO("rectangle overflow refused");
        REQUIRE((!DeriveClearCalls(read, wrapped, &why) && why == "address_overflow"));
        wrapped    = entry;
        wrapped.sp = 100;
        UNSCOPED_INFO("stack underflow refused");
        REQUIRE((!DeriveClearCalls(read, wrapped, &why) && why == "address_overflow"));
        // A surface pointer that would wrap is refused too.
        auto copy         = memory;
        copy[kG + 0x3098] = 0xFFFFFFF0U;
        UNSCOPED_INFO("surface overflow refused");
        REQUIRE((!DeriveClearCalls(readerOf(copy), entry, &why) && why == "address_overflow"));
        // A color pointer that would wrap is refused when the call clears color, and not otherwise.
        wrapped       = entry;
        wrapped.color = 0xFFFFFFF8U;
        UNSCOPED_INFO("color overflow refused");
        REQUIRE((!DeriveClearCalls(read, wrapped, &why) && why == "address_overflow"));
        wrapped       = entry;
        wrapped.flags = 0x30;
        wrapped.color = 0xFFFFFFF8U;
        UNSCOPED_INFO("an unread color pointer may be anything");
        REQUIRE(DeriveClearCalls(read, wrapped).has_value());
    }

    // The default color is copied to r1+112 of the body frame by an aligned
    // 16-byte store; a misaligned frame is not modeled, and only a call that
    // clears color needs it.
    {
        const Case  misaligned{ .name = "misaligned", .flags = 0x1, .hasColor = false, .sp = kSp + 8, .expect = {} };
        std::string why;
        UNSCOPED_INFO("misaligned default color slot refused: " + why);
        REQUIRE((!DeriveClearCalls(readerOf(build(misaligned)), entryOf(misaligned), &why) && why == "misaligned_default_color"));
        const Case depthOnly{ .name = "depthOnly", .flags = 0x30, .hasColor = false, .sp = kSp + 8, .expect = {} };
        const auto derived = DeriveClearCalls(readerOf(build(depthOnly)), entryOf(depthOnly));
        UNSCOPED_INFO("a depth-only call needs no color slot");
        REQUIRE((derived && derived->count == 1 && derived->calls[0].colorPointer == kSp + 8 - 256));
        // An unreadable default color is refused only when a call clears color.
        auto memory = build(Case{ .name = "d", .flags = 0x1, .hasColor = false, .expect = {} });
        memory.erase(0x8208EBE0 + 8);
        std::string reason;
        UNSCOPED_INFO("unreadable default color refused: " + reason);
        REQUIRE((!DeriveClearCalls(readerOf(memory), entryOf(Case{ .name = "d", .flags = 0x1, .hasColor = false, .expect = {} }), &reason) && reason == "unreadable 8208EBE8"));
        UNSCOPED_INFO("the default color is not read for a depth-only call");
        REQUIRE(DeriveClearCalls(readerOf(memory), entryOf(Case{ .name = "d", .flags = 0x30, .hasColor = false, .expect = {} })).has_value());
        // Nor the explicit color words.
        auto explicitMemory = build(Case{ .name = "e", .flags = 0x30, .expect = {} });
        for (std::uint32_t i = 0; i < 4; ++i)
            explicitMemory.erase(kColor + 4 * i);
        UNSCOPED_INFO("the color words are not read for a depth-only call");
        REQUIRE(DeriveClearCalls(readerOf(explicitMemory), entryOf(Case{ .name = "e", .flags = 0x30, .expect = {} })).has_value());
    }

#if REREVVED_DEVELOPER
    // The hook side: the stencil word and the color words of a call that clears color.
    {
        const Case                         base{ .name = "base", .flags = 0x3, .expect = {} };
        auto                               memory = build(base);
        const std::array<std::uint32_t, 8> color{ kG, 0x3, 1, 0, 0, 1280, 720, kColor };
        const std::array<std::uint32_t, 8> depthOnly{ kG, 0x30, 0xFFFFFFFF, 0, 0, 1280, 720, 0x1234 };
        ClearCall                          call;
        std::string                        why;
        memory[frameOf(kSp) + 92] = 0x77;
        UNSCOPED_INFO("observe reads");
        REQUIRE(ObserveClearCall(readerOf(memory), color, frameOf(kSp), 0.5, call, &why));
        UNSCOPED_INFO("observed call");
        REQUIRE((call.stencil == 0x77 && call.color == kUserColor && call.index == 1 && call.colorPointer == kColor && call.depthBits == std::bit_cast<std::uint64_t>(0.5)));
        UNSCOPED_INFO("depth-only call reads no color");
        REQUIRE((ObserveClearCall(readerOf(memory), depthOnly, frameOf(kSp), 0.5, call, &why) && call.index == kGuestDepthOnlyClearIndex && call.color == std::array<std::uint32_t, 4>{}));
        auto copy = memory;
        copy.erase(frameOf(kSp) + 92);
        UNSCOPED_INFO("stencil unreadable: " + why);
        REQUIRE((!ObserveClearCall(readerOf(copy), color, frameOf(kSp), 0.5, call, &why) && why == "unreadable stencil at r1+92"));
        UNSCOPED_INFO("stack overflow refused");
        REQUIRE(!ObserveClearCall(readerOf(memory), color, 0xFFFFFFF8U, 0.5, call, &why));
        copy = memory;
        copy.erase(kColor + 12);
        UNSCOPED_INFO("color unreadable: " + why);
        REQUIRE((!ObserveClearCall(readerOf(copy), color, frameOf(kSp), 0.5, call, &why) && why == std::format("unreadable color at {:08X}", kColor + 12)));
        UNSCOPED_INFO("an unused color pointer is not read");
        REQUIRE(ObserveClearCall(readerOf(copy), depthOnly, frameOf(kSp), 0.5, call, &why));
        auto wrapped = color;
        wrapped[7]   = 0xFFFFFFF8U;
        UNSCOPED_INFO("color pointer overflow refused");
        REQUIRE((!ObserveClearCall(readerOf(memory), wrapped, frameOf(kSp), 0.5, call, &why) && why == "address_overflow"));
    }
#endif
}

ClearProbe probeOf(std::uint32_t g, std::uint32_t flags = 0)
{
    ClearProbe probe;
#if REREVVED_DEVELOPER
    probe.entry.g     = g;
    probe.entry.flags = flags;
    ClearCall call;
    call.flags = flags;
    ClearCalls calls;
    calls.Add(call);
    probe.derived = calls;
#else
    (void)g;
    (void)flags;
#endif
    return probe;
}

void testProbeStack()
{
    constexpr std::uint32_t outer = 0x70000000, inner = 0x6FFFF000, innermost = 0x6FFFE000;
    unsigned                dropped = 99;

    // The body frame is the entry r1 less 368: the wrapper's 112 and the body's 256.
    UNSCOPED_INFO("frame size");
    REQUIRE((kClearFrameSize == 368 && kClearFrameSize == 112 + 256));

    // A nested call: each hook pair finds its own probe, the inner first.
    {
        ClearProbeStack stack;
        UNSCOPED_INFO("nested push");
        REQUIRE((stack.Push(outer, probeOf(1, 0x11)) == 0 && stack.Push(inner, probeOf(2, 0x22)) == 0 && stack.Size() == 2));
        auto* found = stack.Find(frameOf(inner), dropped);
        UNSCOPED_INFO("inner call hook");
        REQUIRE((found && DEV_CHECK(found->entry.g == 2) && found->frame == frameOf(inner) && dropped == 0 && stack.Size() == 2));
        found = stack.Find(frameOf(inner), dropped);
        UNSCOPED_INFO("inner call hook is repeatable");
        REQUIRE((found && DEV_CHECK(found->derived->calls[0].flags == 0x22)));
        auto popped = stack.Pop(frameOf(inner), dropped);
        UNSCOPED_INFO("inner exit hook");
        REQUIRE((popped && DEV_CHECK(popped->entry.g == 2) && dropped == 0 && stack.Size() == 1));
        found = stack.Find(frameOf(outer), dropped);
        UNSCOPED_INFO("outer call hook after the inner left");
        REQUIRE((found && DEV_CHECK(found->entry.g == 1) && DEV_CHECK(found->derived->calls[0].flags == 0x11) && dropped == 0));
        popped = stack.Pop(frameOf(outer), dropped);
        UNSCOPED_INFO("outer exit hook");
        REQUIRE((popped && DEV_CHECK(popped->entry.g == 1) && dropped == 0 && !stack.Size()));
    }
    // A callback that tail-jumps to Clear enters with exactly the outer body r1; the outer stays.
    {
        ClearProbeStack stack;
        stack.Push(outer, probeOf(1));
        UNSCOPED_INFO("tail-jumped entry keeps the outer");
        REQUIRE((stack.Push(frameOf(outer), probeOf(2)) == 0 && stack.Size() == 2));
        UNSCOPED_INFO("tail-jumped pairs");
        const auto second = stack.Pop(frameOf(frameOf(outer)), dropped);
        const auto first  = stack.Pop(frameOf(outer), dropped);
        REQUIRE((second && first && DEV_CHECK(second->entry.g == 2 && first->entry.g == 1)));
    }
    // A hook whose frame matches nothing disturbs nothing and counts nothing.
    {
        ClearProbeStack stack;
        stack.Push(outer, probeOf(1));
        UNSCOPED_INFO("unmatched hook leaves the stack");
        REQUIRE((!stack.Find(frameOf(inner), dropped) && dropped == 0 && stack.Size() == 1));
        UNSCOPED_INFO("unmatched exit leaves the stack");
        REQUIRE((!stack.Pop(frameOf(inner), dropped) && dropped == 0 && stack.Size() == 1));
    }
    // An inner call that never reached its exit hook: the outer exit drops it and counts it.
    {
        ClearProbeStack stack;
        stack.Push(outer, probeOf(1));
        stack.Push(inner, probeOf(2));
        stack.Push(innermost, probeOf(3));
        auto popped = stack.Pop(frameOf(outer), dropped);
        UNSCOPED_INFO("skipped exits above the outer are dropped");
        REQUIRE((popped && DEV_CHECK(popped->entry.g == 1) && dropped == 2 && !stack.Size()));
    }
    // The call hook drops a stale probe above too.
    {
        ClearProbeStack stack;
        stack.Push(outer, probeOf(1));
        stack.Push(inner, probeOf(2));
        auto* found = stack.Find(frameOf(outer), dropped);
        UNSCOPED_INFO("call hook drops the stale inner");
        REQUIRE((found && DEV_CHECK(found->entry.g == 1) && dropped == 1 && stack.Size() == 1));
    }
    // A later call that enters at or above a stale probe frame replaces it, even at the same r1.
    {
        ClearProbeStack stack;
        stack.Push(inner, probeOf(1));
        UNSCOPED_INFO("same entry r1 drops the stale probe");
        REQUIRE((stack.Push(inner, probeOf(2)) == 1 && stack.Size() == 1));
        auto* found = stack.Find(frameOf(inner), dropped);
        UNSCOPED_INFO("the new call owns the frame");
        REQUIRE((found && DEV_CHECK(found->entry.g == 2)));
        stack.Clear();
        stack.Push(inner, probeOf(1));
        UNSCOPED_INFO("a shallower entry drops the stale probe");
        REQUIRE((stack.Push(outer, probeOf(2)) == 1 && stack.Size() == 1));
        UNSCOPED_INFO("shallower call pairs");
        const auto* shallow = stack.Find(frameOf(outer), dropped);
        REQUIRE((shallow && DEV_CHECK(shallow->entry.g == 2)));
    }
    // Overflow: the fifth nested call is not kept and counts one; the others pair.
    {
        ClearProbeStack stack;
        std::uint32_t   sp = outer;
        for (std::uint32_t i = 0; i < ClearProbeStack::kDepth; ++i, sp -= 0x1000)
        {
            UNSCOPED_INFO("fill the stack");
            REQUIRE(stack.Push(sp, probeOf(i + 1)) == 0);
        }
        UNSCOPED_INFO("full");
        REQUIRE(stack.Size() == ClearProbeStack::kDepth);
        UNSCOPED_INFO("overflow counts one and keeps the stack");
        REQUIRE((stack.Push(sp, probeOf(9)) == 1 && stack.Size() == ClearProbeStack::kDepth));
        UNSCOPED_INFO("the overflowed call finds nothing");
        REQUIRE((!stack.Find(frameOf(sp), dropped) && dropped == 0));
        sp = outer - 0x1000 * (ClearProbeStack::kDepth - 1);
        for (std::uint32_t i = ClearProbeStack::kDepth; i > 0; --i, sp += 0x1000)
        {
            const auto popped = stack.Pop(frameOf(sp), dropped);
            UNSCOPED_INFO("overflowed stack still pairs");
            REQUIRE((popped && DEV_CHECK(popped->entry.g == i) && dropped == 0));
        }
        UNSCOPED_INFO("emptied");
        REQUIRE(!stack.Size());
    }
    // A shallow entry flushes a stack of stale probes and is kept.
    {
        ClearProbeStack stack;
        std::uint32_t   sp = outer - 0x10000;
        for (std::uint32_t i = 0; i < ClearProbeStack::kDepth; ++i, sp -= 0x1000)
            stack.Push(sp, probeOf(i + 1));
        UNSCOPED_INFO("a shallow entry flushes the stale stack");
        REQUIRE((stack.Push(outer, probeOf(7)) == ClearProbeStack::kDepth && stack.Size() == 1));
    }
}

#if REREVVED_DEVELOPER
// The hooks as the runtime runs them: Clear entry derives and pushes, each
// 0x826A1D90 hook finds its probe by r1 and records the call, the exit hook pops
// the probe and compares.
struct Harness
{
    ClearProbeStack stack;
    unsigned        unpaired = 0;

    void enter(const Case& c, const Memory& memory)
    {
        ClearProbe probe;
        probe.entry   = entryOf(c);
        probe.derived = DeriveClearCalls(readerOf(memory), probe.entry, &probe.why);
        unpaired += stack.Push(c.sp, std::move(probe));
    }

    void call(const Case& c, const Memory& memory, std::size_t index)
    {
        unsigned dropped = 0;
        auto*    found   = stack.Find(frameOf(c.sp), dropped);
        unpaired += dropped;
        if (!found)
            return;
        const auto&                        e = c.expect.at(index);
        const std::array<std::uint32_t, 8> r{ kG, e.flags, static_cast<std::uint32_t>(e.index), static_cast<std::uint32_t>(e.rect[0]), static_cast<std::uint32_t>(e.rect[1]), static_cast<std::uint32_t>(e.rect[2]), static_cast<std::uint32_t>(e.rect[3]), e.colorPointer };
        ClearCall                          observed;
        if (!ObserveClearCall(readerOf(memory), r, frameOf(c.sp), std::bit_cast<double>(c.depthBits), observed))
            found->observedUnreadable = true;
        found->observed.Add(observed);
    }

    // The mismatch mask at the exit hook; -1 when no probe is found.
    int leave(const Case& c)
    {
        unsigned dropped = 0;
        auto     popped  = stack.Pop(frameOf(c.sp), dropped);
        unpaired += dropped;
        if (!popped)
            return -1;
        return static_cast<int>(NoteClearDerive(popped->derived, popped->observed, !popped->observedUnreadable));
    }
};

const Case& named(const char* name)
{
    for (const auto& c : kCases)
        if (std::string(c.name) == name)
            return c;
    throw std::runtime_error(std::string("no case ") + name);
}

void testPairing()
{
    TakeClearDeriveCounts();
    const Case& four = named("mrt_missing_middle_target");
    UNSCOPED_INFO("case shape");
    REQUIRE(four.expect.size() == 3);

    // One clear: entry, its three calls, exit.
    {
        Harness    harness;
        const auto memory = withHookMemory(four);
        harness.enter(four, memory);
        for (std::size_t i = 0; i < 3; ++i)
            harness.call(four, memory, i);
        UNSCOPED_INFO("one clear pairs");
        REQUIRE((harness.leave(four) == 0 && harness.unpaired == 0 && !harness.stack.Size()));
    }
    // A clear that makes no calls is confirmed at the exit hook.
    {
        Harness     harness;
        const Case& none   = named("scissor_empty_width");
        const auto  memory = withHookMemory(none);
        harness.enter(none, memory);
        UNSCOPED_INFO("an empty clear pairs");
        REQUIRE(harness.leave(none) == 0);
    }
    // A clear nested in the second call of another (the allocator callback runs
    // guest code): each pairs its own calls.
    {
        Harness harness;
        Case    outerCase      = four;
        outerCase.sp           = 0x70000000;
        Case innerCase         = named("rect_given");
        innerCase.sp           = 0x6FFFF000;
        const auto outerMemory = withHookMemory(outerCase), innerMemory = withHookMemory(innerCase);
        harness.enter(outerCase, outerMemory);
        harness.call(outerCase, outerMemory, 0);
        harness.call(outerCase, outerMemory, 1);
        harness.enter(innerCase, innerMemory);
        harness.call(innerCase, innerMemory, 0);
        UNSCOPED_INFO("the inner clear pairs");
        REQUIRE(harness.leave(innerCase) == 0);
        harness.call(outerCase, outerMemory, 2);
        UNSCOPED_INFO("the outer clear pairs after the inner left");
        REQUIRE((harness.leave(outerCase) == 0 && harness.unpaired == 0 && !harness.stack.Size()));
    }
    // A call the body skipped shows as a count mismatch; an extra one too.
    {
        Harness    harness;
        const auto memory = withHookMemory(four);
        harness.enter(four, memory);
        harness.call(four, memory, 0);
        harness.call(four, memory, 1);
        UNSCOPED_INFO("a missing call is the count mismatch");
        REQUIRE(harness.leave(four) == 1);
        harness.enter(four, memory);
        for (std::size_t i = 0; i < 3; ++i)
            harness.call(four, memory, i);
        harness.call(four, memory, 2);
        UNSCOPED_INFO("an extra call is the count mismatch");
        REQUIRE(harness.leave(four) == 1);
    }
    // A clear that never reached its exit hook is dropped by the next shallower
    // entry and counted; the new clear pairs on its own frame.
    {
        Harness harness;
        Case    first = four, second = four;
        first.sp               = 0x6FFFE000;
        second.sp              = 0x70000000;
        const auto firstMemory = withHookMemory(first), secondMemory = withHookMemory(second);
        harness.enter(first, firstMemory);
        harness.call(first, firstMemory, 0);
        harness.enter(second, secondMemory);
        for (std::size_t i = 0; i < 3; ++i)
            harness.call(second, secondMemory, i);
        UNSCOPED_INFO("a skipped exit is dropped when the next call enters");
        REQUIRE((harness.unpaired == 1 && harness.leave(second) == 0 && !harness.stack.Size()));
    }
    // A hook of a frame without a probe is ignored.
    {
        Harness    harness;
        const auto memory = withHookMemory(four);
        harness.call(four, memory, 0);
        UNSCOPED_INFO("no probe, no pairing");
        REQUIRE((harness.leave(four) == -1 && harness.unpaired == 0));
    }
    // An observed call with an unreadable word is unsupported.
    {
        Harness harness;
        auto    memory = withHookMemory(four);
        harness.enter(four, memory);
        memory.erase(frameOf(four.sp) + 92);
        harness.call(four, memory, 0);
        UNSCOPED_INFO("an unreadable observed call is not a mismatch");
        REQUIRE(harness.leave(four) == 0);
    }
    auto counts = TakeClearDeriveCounts();
    UNSCOPED_INFO("pairing counts");
    REQUIRE((counts.ok == 5 && counts.unsupported == 1 && counts.mismatch[0] == 2));
}

void testComparison()
{
    ClearCall a;
    a.flags        = 0x33;
    a.index        = 1;
    a.rect         = { 1, 2, 3, 4 };
    a.colorPointer = 0x1000;
    a.color        = { 5, 6, 7, 8 };
    a.depthBits    = 0x3FF0000000000000ULL;
    a.stencil      = 0x55;
    ClearCalls one;
    one.Add(a);
    const auto with = [&](auto&& change)
    {
        ClearCall b = a;
        change(b);
        ClearCalls list;
        list.Add(b);
        return list;
    };
    UNSCOPED_INFO("equal lists match");
    REQUIRE(ClearMismatches(one, one) == 0);
    UNSCOPED_INFO("index bit");
    REQUIRE(ClearMismatches(one, with([](ClearCall& b)
                                      {
                                          b.index = 2;
                                      })) == 2);
    UNSCOPED_INFO("flags bit");
    REQUIRE(ClearMismatches(one, with([](ClearCall& b)
                                      {
                                          b.flags = 0x30;
                                      })) == 4);
    UNSCOPED_INFO("rect bit");
    REQUIRE(ClearMismatches(one, with([](ClearCall& b)
                                      {
                                          b.rect[3] = 5;
                                      })) == 8);
    UNSCOPED_INFO("color pointer bit");
    REQUIRE(ClearMismatches(one, with([](ClearCall& b)
                                      {
                                          b.colorPointer = 0x1010;
                                      })) == 16);
    UNSCOPED_INFO("color word bit");
    REQUIRE(ClearMismatches(one, with([](ClearCall& b)
                                      {
                                          b.color[2] = 9;
                                      })) == 16);
    UNSCOPED_INFO("depth bit");
    REQUIRE(ClearMismatches(one, with([](ClearCall& b)
                                      {
                                          b.depthBits = 0x3FF0000000000001ULL;
                                      })) == 32);
    UNSCOPED_INFO("stencil bit");
    REQUIRE(ClearMismatches(one, with([](ClearCall& b)
                                      {
                                          b.stencil = 0x56;
                                      })) == 64);
    // A NaN depth equals itself bitwise; -0.0 and 0.0 differ.
    {
        auto nan = with([](ClearCall& b)
                        {
                            b.depthBits = 0x7FF8000000000123ULL;
                        });
        UNSCOPED_INFO("NaN payload equals itself");
        REQUIRE(ClearMismatches(nan, nan) == 0);
        UNSCOPED_INFO("signed zeros differ");
        REQUIRE(ClearMismatches(with([](ClearCall& b)
                                     {
                                         b.depthBits = 0;
                                     }),
                                with([](ClearCall& b)
                                     {
                                         b.depthBits = 0x8000000000000000ULL;
                                     })) == 32);
    }
    // The color words matter only when both calls clear color.
    {
        auto b = with([](ClearCall& c)
                      {
                          c.flags = 0x30;
                          c.color = { 1, 1, 1, 1 };
                      });
        auto c = with([](ClearCall& d)
                      {
                          d.flags = 0x30;
                          d.color = { 2, 2, 2, 2 };
                      });
        UNSCOPED_INFO("unused color words are not compared");
        REQUIRE((ClearMismatches(b, c) & 16) == 0);
        UNSCOPED_INFO("one-sided use does not compare words");
        REQUIRE((ClearMismatches(one, with([](ClearCall& d)
                                           {
                                               d.color = { 5, 6, 7, 8 };
                                               d.flags = 0x30;
                                           })) &
                 16) == 0);
    }
    // Lengths: a missing call, an extra call and calls past the capacity.
    {
        ClearCalls two = one;
        two.Add(a);
        UNSCOPED_INFO("length bit");
        REQUIRE((ClearMismatches(one, two) == 1 && ClearMismatches(two, one) == 1));
        UNSCOPED_INFO("empty lists match");
        REQUIRE(ClearMismatches(ClearCalls{}, ClearCalls{}) == 0);
        UNSCOPED_INFO("an empty list against a call");
        REQUIRE(ClearMismatches(ClearCalls{}, one) == 1);
        // Calls that differ in a field as well as in length report both.
        auto changed             = two;
        changed.calls[1].stencil = 1;
        UNSCOPED_INFO("only paired calls are compared");
        REQUIRE(ClearMismatches(one, changed) == 1);
        changed                  = two;
        changed.calls[0].stencil = 1;
        UNSCOPED_INFO("paired calls compare field by field");
        REQUIRE(ClearMismatches(two, changed) == 64);
        ClearCalls full;
        for (std::size_t i = 0; i < kClearMaxCalls + 2; ++i)
            full.Add(a);
        UNSCOPED_INFO("overflow is counted");
        REQUIRE((full.count == kClearMaxCalls && full.overflow == 2 && full.Total() == kClearMaxCalls + 2));
        ClearCalls exact;
        for (std::size_t i = 0; i < kClearMaxCalls; ++i)
            exact.Add(a);
        UNSCOPED_INFO("overflowed lists differ in length");
        REQUIRE(ClearMismatches(full, exact) == 1);
    }

    TakeClearDeriveCounts();
    UNSCOPED_INFO("ok counts no mismatch");
    REQUIRE(NoteClearDerive(one, one, true) == 0);
    UNSCOPED_INFO("stencil mismatch mask");
    REQUIRE(NoteClearDerive(one, with([](ClearCall& b)
                                      {
                                          b.stencil = 1;
                                      }),
                            true) == 64);
    UNSCOPED_INFO("two-field mismatch mask");
    REQUIRE(NoteClearDerive(one, with([](ClearCall& b)
                                      {
                                          b.flags   = 0;
                                          b.rect[0] = 9;
                                      }),
                            true) == 12);
    UNSCOPED_INFO("count mismatch mask");
    REQUIRE(NoteClearDerive(one, ClearCalls{}, true) == 1);
    UNSCOPED_INFO("unmodeled counts no mismatch");
    REQUIRE(NoteClearDerive(std::nullopt, one, true) == 0);
    UNSCOPED_INFO("an unreadable observed call counts unsupported");
    REQUIRE(NoteClearDerive(one, one, false) == 0);
    NoteClearUnpaired(0);
    NoteClearUnpaired(2);
    NoteClearUnpaired(1);
    auto counts = TakeClearDeriveCounts();
    UNSCOPED_INFO("ok and unsupported");
    REQUIRE((counts.ok == 1 && counts.unsupported == 2));
    UNSCOPED_INFO("per-field mismatches");
    REQUIRE((counts.mismatch[0] == 1 && counts.mismatch[1] == 0 && counts.mismatch[2] == 1 && counts.mismatch[3] == 1 && counts.mismatch[4] == 0 && counts.mismatch[5] == 0 &&
             counts.mismatch[6] == 1));
    UNSCOPED_INFO("unpaired");
    REQUIRE(counts.unpaired == 3);
    const std::string expected =
        " clear_derive_ok=1 clear_derive_mismatch_count=1 clear_derive_mismatch_index=0 clear_derive_mismatch_flags=1 clear_derive_mismatch_rect=1"
        " clear_derive_mismatch_color=0 clear_derive_mismatch_depth=0 clear_derive_mismatch_stencil=1 clear_derive_unsupported=2 clear_derive_unpaired=3";
    UNSCOPED_INFO("formatted counts: " + FormatClearDeriveCounts(counts));
    REQUIRE(FormatClearDeriveCounts(counts) == expected);
    counts = TakeClearDeriveCounts();
    UNSCOPED_INFO("take resets");
    REQUIRE((counts.ok == 0 && counts.unsupported == 0 && counts.unpaired == 0 && counts.mismatch == std::array<std::uint64_t, kClearFieldCount>{}));
}

void testFormat()
{
    ClearCall call;
    call.flags        = 0x31;
    call.index        = 0;
    call.rect         = { -7, 0, 1280, 720 };
    call.colorPointer = 0x6FFFFF00;
    call.color        = { 0x3F800000, 0, 1, 2 };
    call.depthBits    = 0x3FF0000000000000ULL;
    call.stencil      = 0x55;
    UNSCOPED_INFO("color call: " + FormatClearCall(call));
    REQUIRE(FormatClearCall(call) == "flags=00000031 index=0 rect=-7,0,1280,720 color=6FFFFF00[3F800000,00000000,00000001,00000002] depth=3FF0000000000000 stencil=00000055");
    call.flags = 0x30;
    call.index = kGuestDepthOnlyClearIndex;
    UNSCOPED_INFO("depth-only call: " + FormatClearCall(call));
    REQUIRE(FormatClearCall(call) == "flags=00000030 index=-1 rect=-7,0,1280,720 color=6FFFFF00 depth=3FF0000000000000 stencil=00000055");
    ClearCalls calls;
    UNSCOPED_INFO("empty list");
    REQUIRE(FormatClearCalls(calls) == "[]");
    calls.Add(call);
    calls.Add(call);
    calls.overflow = 3;
    UNSCOPED_INFO("list: " + FormatClearCalls(calls));
    REQUIRE(FormatClearCalls(calls) == "[{" + FormatClearCall(call) + "} {" + FormatClearCall(call) + "} +3]");
}
#endif

} // namespace

TEST_CASE("clear_derive", "[ctest][clear-derive][timeout-60]")
{
    testCases();
    testFloatConversion();
    testRefusals();
    testProbeStack();
#if REREVVED_DEVELOPER
    testObservedRoundTrip();
    testPairing();
    testComparison();
    testFormat();
#endif
}
