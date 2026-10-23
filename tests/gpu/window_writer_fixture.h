#pragma once

#include "gpu/guest_d3d/window_writer_derive.h"

#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace window_writer_fixture
{

using namespace rerevved::gpu::guest_d3d;

inline void require(bool value, const std::string& message)
{
    if (!value)
        throw std::runtime_error(message);
}

inline constexpr std::uint32_t kG = 0x40010000, kRing = 0x40080000;

using Memory = std::map<std::uint32_t, std::uint32_t>;

struct Tile
{
    std::array<std::int32_t, 4> rect{ 0, 0, 1280, 720 };
    std::array<std::int32_t, 2> offset{ 0, 0 };
};

// The device words the writer reads, as a test sets them.
struct Device
{
    std::uint32_t                cursor = kRing + 0x100, limit34 = kRing + 0x40000, limit38 = kRing + 0x3F000;
    std::uint8_t                 tiling = 0, flags = 0, stateByte = 0x0F;
    std::array<std::uint32_t, 5> bound{ 0x40100000, 0x40100100, 0, 0, 0 };
    std::array<std::uint32_t, 5> applied{ 0x40100000, 0x40100100, 0, 0, 0 };
    std::uint32_t                tileCount = 0, predicateMask = 0x0000FFFF, predicateWord = 0x00000055;
    std::vector<Tile>            tiles;
};

inline Memory build(const Device& device)
{
    Memory memory;
    memory[kG + 0x30]   = device.cursor;
    memory[kG + 0x34]   = device.limit34;
    memory[kG + 0x38]   = device.limit38;
    memory[kG + 0x2ABC] = std::uint32_t{ device.tiling } << 24 | std::uint32_t{ device.stateByte } << 16 | device.flags;
    for (std::uint32_t i = 0; i < 5; ++i)
    {
        memory[kG + 0x3098 + 4 * i] = device.bound[i];
        memory[kG + 0x31B8 + 4 * i] = device.applied[i];
    }
    memory[kG + 0x31CC] = device.tileCount;
    memory[kG + 0x31A4] = device.predicateMask;
    memory[kG + 0x31A8] = device.predicateWord;
    // The rects first: the last tiles' rects overlap the first offsets, and the later store wins as in the guest.
    for (std::uint32_t i = 0; i < device.tiles.size(); ++i)
        for (std::uint32_t k = 0; k < 4; ++k)
            memory[kG + 0x31D0 + 16 * i + 4 * k] = static_cast<std::uint32_t>(device.tiles[i].rect[k]);
    for (std::uint32_t i = 0; i < device.tiles.size(); ++i)
        for (std::uint32_t k = 0; k < 2; ++k)
            memory[kG + 0x32C0 + 8 * i + 4 * k] = static_cast<std::uint32_t>(device.tiles[i].offset[k]);
    return memory;
}

inline GuestWordReader readerOf(const Memory& memory)
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

struct Emulated
{
    std::uint32_t              r3      = 0;
    bool                       flushed = false;
    Memory                     memory;
    std::vector<std::uint32_t> ring; // the words stored through the cursor chain, in order
};

// Literal 0x8268D3A0 reference (g153:12634-13092), independent of
// window_writer_derive.cpp. Register names follow the listing.
inline Emulated emulateOriginal(Memory memory, const WindowWriterEntry& in)
{
    Emulated   out;
    const auto lwz = [&](std::uint32_t address)
    {
        return memory.at(address);
    };
    const auto lbz = [&](std::uint32_t address)
    {
        return (memory.at(address & ~3U) >> (24 - 8 * (address & 3))) & 0xFFU;
    };
    const auto stw = [&](std::uint32_t address, std::uint32_t value)
    {
        memory[address] = value;
    };
    const auto stwu = [&](std::uint32_t& reg, std::uint32_t value)
    {
        reg += 4;
        stw(reg, value);
        out.ring.push_back(value);
    };
    const auto s32 = [](std::uint32_t value)
    {
        return static_cast<std::int32_t>(value);
    };
    const auto finish = [&]()
    {
        out.memory = memory;
        return out;
    };

    const std::uint32_t r31 = in.g, r25 = in.x0, r24 = in.y0, r23 = in.x1, r22 = in.y1;
    std::uint32_t       r3 = lwz(r31 + 48), r11 = lwz(r31 + 56), r10 = 0, r9 = 0, r8 = 0, r7 = 0, r6 = 0, r5 = 0, r4 = 0;
    // The model must refuse any call that sets flushed=true.
    if (r3 > r11)
    {
        out.flushed = true;
        return finish();
    }
    // loc_8268D3D8: the tiled test
    r11 = lbz(r31 + 10940);
    bool tiled;
    if (r11 & 0x10)
        tiled = true;
    else if (!(r11 & 0x20))
        tiled = false;
    else
    {
        tiled = true;
        for (std::uint32_t i = 0; i < 5; ++i)
        {
            r11 = lwz(r31 + 12440 + 4 * i);
            r10 = lwz(r31 + 12728 + 4 * i);
            if (r10 == r11)
                continue;
            if (r11 != 0)
            {
                tiled = false;
                break;
            }
        }
    }
    r10 = 0;
    if (!tiled)
    {
        // loc_8268D47C
        r11 = (2U << 16) | 8320;
        r9  = std::rotl(r24, 16) & 0x3FFF0000U;
        r8  = std::rotl(r22, 16) & 0x3FFF0000U;
        stwu(r3, r11);
        r11 = r25 & 0x3FFFU;
        r11 = r9 | r11;
        r9  = r23 & 0x3FFFU;
        stwu(r3, r10);
        r9 = r8 | r9;
        stwu(r3, r11);
        r11 = r3;
        stwu(r11, r9);
    }
    else
    {
        // loc_8268D4C0
        const std::uint32_t r26 = (0xC000U << 16) | 24576;
        r11                     = (0xC000U << 16) | 24832;
        stwu(r3, r11);
        stwu(r3, r10);
        r10               = lwz(r31 + 12748);
        r11               = r3;
        std::uint32_t r27 = 0, r28 = 0, r29 = 0, r30 = 0;
        if (r10 != 0)
        {
            r29 = 0;
            r30 = r31 + 12756;
            r28 = r31 + 12996;
            do
            {
                r10 = lwz(r30 - 4);
                r7  = r25;
                r6  = lwz(r28 - 4);
                r5  = lwz(r28 + 0);
                if (!(s32(r25) > s32(r10)))
                    r7 = r10;
                r10 = lwz(r30 + 0);
                r8  = r24;
                if (!(s32(r24) > s32(r10)))
                    r8 = r10;
                r9 = lwz(r30 + 4);
                if (s32(r23) < s32(r9))
                    r9 = r23;
                r10 = lwz(r30 + 8);
                if (s32(r22) < s32(r10))
                    r10 = r22;
                if (!(s32(r7) < s32(r9)) || !(s32(r8) < s32(r10)))
                    r10 = r9 = r8 = r7 = 0;
                // loc_8268D568
                r4 = r26;
                r3 = 3;
                stwu(r11, r4);
                r4 = (0xC003U << 16) | 11521;
                // slw: the amount is the low six bits of r29, 32 and above give zero
                r3 = (r29 & 0x3F) >= 32 ? 0 : r3 << (r29 & 0x3F);
                stwu(r11, r3);
                const std::uint32_t r21 = (4U << 16) | 128;
                r6                      = 0U - r6;
                r5                      = 0U - r5;
                stwu(r11, r4);
                r5 = std::rotl(r5, 16) & 0x7FFF0000U;
                r6 = r6 & 0x7FFFU;
                r8 = std::rotl(r8, 16) & 0x3FFF0000U;
                r6 = r5 | r6;
                r7 = r7 & 0x3FFFU;
                stwu(r11, r21);
                r10 = std::rotl(r10, 16) & 0x3FFF0000U;
                r8  = r8 | r7;
                r9  = r9 & 0x3FFFU;
                r10 = r10 | r9;
                stwu(r11, r6);
                stwu(r11, r8);
                stwu(r11, r10);
                r10 = lwz(r31 + 56);
                stw(r31 + 48, r11);
                if (r11 > r10)
                {
                    out.flushed = true;
                    return finish();
                }
                r10 = lwz(r31 + 12748);
                r27 += 1;
                r28 += 8;
                r30 += 16;
                r29 += 2;
            } while (r27 < r10);
        }
        // loc_8268D604
        if ((lbz(r31 + 10943) & 0x20) && (lbz(r31 + 10940) & 0x40))
        {
            r10 = r26;
            r9  = 1;
            stwu(r11, r10);
            r8  = (0xC002U << 16);
            r7  = 4U << 16;
            r10 = r8 | 11521;
            r8  = r7 | 129;
            r7  = std::rotl(r24, 16) & 0x3FFF0000U;
            stwu(r11, r9);
            r6 = r25 & 0x3FFFU;
            r5 = std::rotl(r22, 16) & 0x3FFF0000U;
            r7 = r7 | r6;
            r9 = r23 & 0x3FFFU;
            stwu(r11, r10);
            r9 = r5 | r9;
            stwu(r11, r8);
            stwu(r11, r7);
            stwu(r11, r9);
        }
        // loc_8268D664
        stwu(r11, r26);
        r10 = (0xC000U << 16);
        r9  = lwz(r31 + 12708);
        r10 = r10 | 24832;
        stwu(r11, r9);
        stwu(r11, r10);
        r10 = lwz(r31 + 12712);
        stwu(r11, r10);
    }
    // loc_8268D684
    stw(r31 + 48, r11);
    out.r3 = r3;
    return finish();
}

} // namespace window_writer_fixture
