#include "gpu/guest_d3d/tiling_restore_consume.h"
#include "helper_commands.hpp"
#include "manual_cases.hpp"

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include <array>
#include <bit>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

REXCVAR_DECLARE(bool, native_consume_tiling_restore);
REXCVAR_DECLARE(bool, native_tiling_restore_exit_check);
using namespace rerevved::gpu::guest_d3d;

namespace
{

constexpr std::uint32_t                G = 0x40102000;
constexpr std::array<std::uint32_t, 6> saved{ 0x19375281, 0x91A3B5C7, 0xD26E4953, 0x85B36D17, 0x6F291D83, 0xE172C54B };
constexpr TilingRestoreState           consume{ true, false, true, true, false };

struct Event
{
    char          kind;
    std::uint32_t address, value;
    bool          operator==(const Event&) const = default;
};

struct Memory
{
    int                                   identityCalls = 0;
    std::map<std::uint32_t, std::uint8_t> bytes;
    std::vector<Event>                    events;
    std::uint32_t                         denyRead = 0, denyWrite = 0, unknown = 0, alias = 0;
    std::uint64_t                         aliasIdentity = 0;
    bool                                  physical      = false;
    int                                   failWriter = -1, stores = 0;

    Memory()
    {
        for (std::uint32_t i = 0; i < 0x3AC0; ++i)
            bytes[G - 32 + i] = static_cast<std::uint8_t>((i * 29 + 73) ^ (i >> 3));
        for (std::uint32_t i = 0; i < 6; ++i)
            put(G + 0x3460 + i * 4, saved[i]);
    }

    void put(std::uint32_t address, std::uint32_t value)
    {
        bytes.at(address)     = static_cast<std::uint8_t>(value >> 24);
        bytes.at(address + 1) = static_cast<std::uint8_t>(value >> 16);
        bytes.at(address + 2) = static_cast<std::uint8_t>(value >> 8);
        bytes.at(address + 3) = static_cast<std::uint8_t>(value);
    }

    std::uint32_t get(std::uint32_t a) const
    {
        return (std::uint32_t(bytes.at(a)) << 24) | (std::uint32_t(bytes.at(a + 1)) << 16) |
               (std::uint32_t(bytes.at(a + 2)) << 8) | bytes.at(a + 3);
    }

    std::uint32_t load(std::uint32_t a)
    {
        const auto v = get(a);
        events.push_back({ 'R', a, v });
        return v;
    }

    void store(std::uint32_t a, std::uint32_t v)
    {
        events.push_back({ 'W', a, v });
        put(a, v);
        ++stores;
    }

    TilingRestoreMemory access()
    {
        return {
            [this](std::uint32_t a, std::uint32_t& v)
            {
                if (a == denyRead)
                    return false;
                v = load(a);
                return true;
            },
            [this](std::uint32_t a, std::uint32_t n)
            {
                return n == 4 && a != denyWrite;
            },
            [this](std::uint32_t a, std::uint32_t n) -> std::optional<std::uint64_t>
            {
                ++identityCalls;
                if (a == unknown || n != 4)
                    return std::nullopt;
                if (a == alias)
                    return aliasIdentity;
                return physical ? std::uint64_t{ a & 0x1FFFFFFF } : (std::uint64_t{ 1 } << 32) + a;
            },
            [this](const TilingRestoreWrite& w)
            {
                if (stores == failWriter)
                    return false;
                store(w.address, w.value);
                return true;
            }
        };
    }
};

PPCContext poisoned()
{
    PPCContext ctx{};
    ctx.r0.u64               = 0x9B3D517F12345678ULL + 0;
    ctx.r1.u64               = 0x9B3D517F12345678ULL + 1;
    ctx.r2.u64               = 0x9B3D517F12345678ULL + 2;
    ctx.r3.u64               = 0x9B3D517F12345678ULL + 3;
    ctx.r4.u64               = 0x9B3D517F12345678ULL + 4;
    ctx.r5.u64               = 0x9B3D517F12345678ULL + 5;
    ctx.r6.u64               = 0x9B3D517F12345678ULL + 6;
    ctx.r7.u64               = 0x9B3D517F12345678ULL + 7;
    ctx.r8.u64               = 0x9B3D517F12345678ULL + 8;
    ctx.r9.u64               = 0x9B3D517F12345678ULL + 9;
    ctx.r10.u64              = 0x9B3D517F12345678ULL + 10;
    ctx.r11.u64              = 0x9B3D517F12345678ULL + 11;
    ctx.r12.u64              = 0x9B3D517F12345678ULL + 12;
    ctx.r13.u64              = 0x9B3D517F12345678ULL + 13;
    ctx.r14.u64              = 0x9B3D517F12345678ULL + 14;
    ctx.r15.u64              = 0x9B3D517F12345678ULL + 15;
    ctx.r16.u64              = 0x9B3D517F12345678ULL + 16;
    ctx.r17.u64              = 0x9B3D517F12345678ULL + 17;
    ctx.r18.u64              = 0x9B3D517F12345678ULL + 18;
    ctx.r19.u64              = 0x9B3D517F12345678ULL + 19;
    ctx.r20.u64              = 0x9B3D517F12345678ULL + 20;
    ctx.r21.u64              = 0x9B3D517F12345678ULL + 21;
    ctx.r22.u64              = 0x9B3D517F12345678ULL + 22;
    ctx.r23.u64              = 0x9B3D517F12345678ULL + 23;
    ctx.r24.u64              = 0x9B3D517F12345678ULL + 24;
    ctx.r25.u64              = 0x9B3D517F12345678ULL + 25;
    ctx.r26.u64              = 0x9B3D517F12345678ULL + 26;
    ctx.r27.u64              = 0x9B3D517F12345678ULL + 27;
    ctx.r28.u64              = 0x9B3D517F12345678ULL + 28;
    ctx.r29.u64              = 0x9B3D517F12345678ULL + 29;
    ctx.r30.u64              = 0x9B3D517F12345678ULL + 30;
    ctx.r31.u64              = 0x9B3D517F12345678ULL + 31;
    ctx.f0.u64               = 0x9B3D517F12345678ULL + 0;
    ctx.f1.u64               = 0x9B3D517F12345678ULL + 1;
    ctx.f2.u64               = 0x9B3D517F12345678ULL + 2;
    ctx.f3.u64               = 0x9B3D517F12345678ULL + 3;
    ctx.f4.u64               = 0x9B3D517F12345678ULL + 4;
    ctx.f5.u64               = 0x9B3D517F12345678ULL + 5;
    ctx.f6.u64               = 0x9B3D517F12345678ULL + 6;
    ctx.f7.u64               = 0x9B3D517F12345678ULL + 7;
    ctx.f8.u64               = 0x9B3D517F12345678ULL + 8;
    ctx.f9.u64               = 0x9B3D517F12345678ULL + 9;
    ctx.f10.u64              = 0x9B3D517F12345678ULL + 10;
    ctx.f11.u64              = 0x9B3D517F12345678ULL + 11;
    ctx.f12.u64              = 0x9B3D517F12345678ULL + 12;
    ctx.f13.u64              = 0x9B3D517F12345678ULL + 13;
    ctx.f14.u64              = 0x9B3D517F12345678ULL + 14;
    ctx.f15.u64              = 0x9B3D517F12345678ULL + 15;
    ctx.f16.u64              = 0x9B3D517F12345678ULL + 16;
    ctx.f17.u64              = 0x9B3D517F12345678ULL + 17;
    ctx.f18.u64              = 0x9B3D517F12345678ULL + 18;
    ctx.f19.u64              = 0x9B3D517F12345678ULL + 19;
    ctx.f20.u64              = 0x9B3D517F12345678ULL + 20;
    ctx.f21.u64              = 0x9B3D517F12345678ULL + 21;
    ctx.f22.u64              = 0x9B3D517F12345678ULL + 22;
    ctx.f23.u64              = 0x9B3D517F12345678ULL + 23;
    ctx.f24.u64              = 0x9B3D517F12345678ULL + 24;
    ctx.f25.u64              = 0x9B3D517F12345678ULL + 25;
    ctx.f26.u64              = 0x9B3D517F12345678ULL + 26;
    ctx.f27.u64              = 0x9B3D517F12345678ULL + 27;
    ctx.f28.u64              = 0x9B3D517F12345678ULL + 28;
    ctx.f29.u64              = 0x9B3D517F12345678ULL + 29;
    ctx.f30.u64              = 0x9B3D517F12345678ULL + 30;
    ctx.f31.u64              = 0x9B3D517F12345678ULL + 31;
    ctx.v0.u64[0]            = 0xD193A57B2E416809ULL + 0;
    ctx.v0.u64[1]            = 0xD193A57B2E416809ULL + 1;
    ctx.v1.u64[0]            = 0xD193A57B2E416809ULL + 2;
    ctx.v1.u64[1]            = 0xD193A57B2E416809ULL + 3;
    ctx.v2.u64[0]            = 0xD193A57B2E416809ULL + 4;
    ctx.v2.u64[1]            = 0xD193A57B2E416809ULL + 5;
    ctx.v3.u64[0]            = 0xD193A57B2E416809ULL + 6;
    ctx.v3.u64[1]            = 0xD193A57B2E416809ULL + 7;
    ctx.v4.u64[0]            = 0xD193A57B2E416809ULL + 8;
    ctx.v4.u64[1]            = 0xD193A57B2E416809ULL + 9;
    ctx.v5.u64[0]            = 0xD193A57B2E416809ULL + 10;
    ctx.v5.u64[1]            = 0xD193A57B2E416809ULL + 11;
    ctx.v6.u64[0]            = 0xD193A57B2E416809ULL + 12;
    ctx.v6.u64[1]            = 0xD193A57B2E416809ULL + 13;
    ctx.v7.u64[0]            = 0xD193A57B2E416809ULL + 14;
    ctx.v7.u64[1]            = 0xD193A57B2E416809ULL + 15;
    ctx.v8.u64[0]            = 0xD193A57B2E416809ULL + 16;
    ctx.v8.u64[1]            = 0xD193A57B2E416809ULL + 17;
    ctx.v9.u64[0]            = 0xD193A57B2E416809ULL + 18;
    ctx.v9.u64[1]            = 0xD193A57B2E416809ULL + 19;
    ctx.v10.u64[0]           = 0xD193A57B2E416809ULL + 20;
    ctx.v10.u64[1]           = 0xD193A57B2E416809ULL + 21;
    ctx.v11.u64[0]           = 0xD193A57B2E416809ULL + 22;
    ctx.v11.u64[1]           = 0xD193A57B2E416809ULL + 23;
    ctx.v12.u64[0]           = 0xD193A57B2E416809ULL + 24;
    ctx.v12.u64[1]           = 0xD193A57B2E416809ULL + 25;
    ctx.v13.u64[0]           = 0xD193A57B2E416809ULL + 26;
    ctx.v13.u64[1]           = 0xD193A57B2E416809ULL + 27;
    ctx.v14.u64[0]           = 0xD193A57B2E416809ULL + 28;
    ctx.v14.u64[1]           = 0xD193A57B2E416809ULL + 29;
    ctx.v15.u64[0]           = 0xD193A57B2E416809ULL + 30;
    ctx.v15.u64[1]           = 0xD193A57B2E416809ULL + 31;
    ctx.v16.u64[0]           = 0xD193A57B2E416809ULL + 32;
    ctx.v16.u64[1]           = 0xD193A57B2E416809ULL + 33;
    ctx.v17.u64[0]           = 0xD193A57B2E416809ULL + 34;
    ctx.v17.u64[1]           = 0xD193A57B2E416809ULL + 35;
    ctx.v18.u64[0]           = 0xD193A57B2E416809ULL + 36;
    ctx.v18.u64[1]           = 0xD193A57B2E416809ULL + 37;
    ctx.v19.u64[0]           = 0xD193A57B2E416809ULL + 38;
    ctx.v19.u64[1]           = 0xD193A57B2E416809ULL + 39;
    ctx.v20.u64[0]           = 0xD193A57B2E416809ULL + 40;
    ctx.v20.u64[1]           = 0xD193A57B2E416809ULL + 41;
    ctx.v21.u64[0]           = 0xD193A57B2E416809ULL + 42;
    ctx.v21.u64[1]           = 0xD193A57B2E416809ULL + 43;
    ctx.v22.u64[0]           = 0xD193A57B2E416809ULL + 44;
    ctx.v22.u64[1]           = 0xD193A57B2E416809ULL + 45;
    ctx.v23.u64[0]           = 0xD193A57B2E416809ULL + 46;
    ctx.v23.u64[1]           = 0xD193A57B2E416809ULL + 47;
    ctx.v24.u64[0]           = 0xD193A57B2E416809ULL + 48;
    ctx.v24.u64[1]           = 0xD193A57B2E416809ULL + 49;
    ctx.v25.u64[0]           = 0xD193A57B2E416809ULL + 50;
    ctx.v25.u64[1]           = 0xD193A57B2E416809ULL + 51;
    ctx.v26.u64[0]           = 0xD193A57B2E416809ULL + 52;
    ctx.v26.u64[1]           = 0xD193A57B2E416809ULL + 53;
    ctx.v27.u64[0]           = 0xD193A57B2E416809ULL + 54;
    ctx.v27.u64[1]           = 0xD193A57B2E416809ULL + 55;
    ctx.v28.u64[0]           = 0xD193A57B2E416809ULL + 56;
    ctx.v28.u64[1]           = 0xD193A57B2E416809ULL + 57;
    ctx.v29.u64[0]           = 0xD193A57B2E416809ULL + 58;
    ctx.v29.u64[1]           = 0xD193A57B2E416809ULL + 59;
    ctx.v30.u64[0]           = 0xD193A57B2E416809ULL + 60;
    ctx.v30.u64[1]           = 0xD193A57B2E416809ULL + 61;
    ctx.v31.u64[0]           = 0xD193A57B2E416809ULL + 62;
    ctx.v31.u64[1]           = 0xD193A57B2E416809ULL + 63;
    ctx.v32.u64[0]           = 0xD193A57B2E416809ULL + 64;
    ctx.v32.u64[1]           = 0xD193A57B2E416809ULL + 65;
    ctx.v33.u64[0]           = 0xD193A57B2E416809ULL + 66;
    ctx.v33.u64[1]           = 0xD193A57B2E416809ULL + 67;
    ctx.v34.u64[0]           = 0xD193A57B2E416809ULL + 68;
    ctx.v34.u64[1]           = 0xD193A57B2E416809ULL + 69;
    ctx.v35.u64[0]           = 0xD193A57B2E416809ULL + 70;
    ctx.v35.u64[1]           = 0xD193A57B2E416809ULL + 71;
    ctx.v36.u64[0]           = 0xD193A57B2E416809ULL + 72;
    ctx.v36.u64[1]           = 0xD193A57B2E416809ULL + 73;
    ctx.v37.u64[0]           = 0xD193A57B2E416809ULL + 74;
    ctx.v37.u64[1]           = 0xD193A57B2E416809ULL + 75;
    ctx.v38.u64[0]           = 0xD193A57B2E416809ULL + 76;
    ctx.v38.u64[1]           = 0xD193A57B2E416809ULL + 77;
    ctx.v39.u64[0]           = 0xD193A57B2E416809ULL + 78;
    ctx.v39.u64[1]           = 0xD193A57B2E416809ULL + 79;
    ctx.v40.u64[0]           = 0xD193A57B2E416809ULL + 80;
    ctx.v40.u64[1]           = 0xD193A57B2E416809ULL + 81;
    ctx.v41.u64[0]           = 0xD193A57B2E416809ULL + 82;
    ctx.v41.u64[1]           = 0xD193A57B2E416809ULL + 83;
    ctx.v42.u64[0]           = 0xD193A57B2E416809ULL + 84;
    ctx.v42.u64[1]           = 0xD193A57B2E416809ULL + 85;
    ctx.v43.u64[0]           = 0xD193A57B2E416809ULL + 86;
    ctx.v43.u64[1]           = 0xD193A57B2E416809ULL + 87;
    ctx.v44.u64[0]           = 0xD193A57B2E416809ULL + 88;
    ctx.v44.u64[1]           = 0xD193A57B2E416809ULL + 89;
    ctx.v45.u64[0]           = 0xD193A57B2E416809ULL + 90;
    ctx.v45.u64[1]           = 0xD193A57B2E416809ULL + 91;
    ctx.v46.u64[0]           = 0xD193A57B2E416809ULL + 92;
    ctx.v46.u64[1]           = 0xD193A57B2E416809ULL + 93;
    ctx.v47.u64[0]           = 0xD193A57B2E416809ULL + 94;
    ctx.v47.u64[1]           = 0xD193A57B2E416809ULL + 95;
    ctx.v48.u64[0]           = 0xD193A57B2E416809ULL + 96;
    ctx.v48.u64[1]           = 0xD193A57B2E416809ULL + 97;
    ctx.v49.u64[0]           = 0xD193A57B2E416809ULL + 98;
    ctx.v49.u64[1]           = 0xD193A57B2E416809ULL + 99;
    ctx.v50.u64[0]           = 0xD193A57B2E416809ULL + 100;
    ctx.v50.u64[1]           = 0xD193A57B2E416809ULL + 101;
    ctx.v51.u64[0]           = 0xD193A57B2E416809ULL + 102;
    ctx.v51.u64[1]           = 0xD193A57B2E416809ULL + 103;
    ctx.v52.u64[0]           = 0xD193A57B2E416809ULL + 104;
    ctx.v52.u64[1]           = 0xD193A57B2E416809ULL + 105;
    ctx.v53.u64[0]           = 0xD193A57B2E416809ULL + 106;
    ctx.v53.u64[1]           = 0xD193A57B2E416809ULL + 107;
    ctx.v54.u64[0]           = 0xD193A57B2E416809ULL + 108;
    ctx.v54.u64[1]           = 0xD193A57B2E416809ULL + 109;
    ctx.v55.u64[0]           = 0xD193A57B2E416809ULL + 110;
    ctx.v55.u64[1]           = 0xD193A57B2E416809ULL + 111;
    ctx.v56.u64[0]           = 0xD193A57B2E416809ULL + 112;
    ctx.v56.u64[1]           = 0xD193A57B2E416809ULL + 113;
    ctx.v57.u64[0]           = 0xD193A57B2E416809ULL + 114;
    ctx.v57.u64[1]           = 0xD193A57B2E416809ULL + 115;
    ctx.v58.u64[0]           = 0xD193A57B2E416809ULL + 116;
    ctx.v58.u64[1]           = 0xD193A57B2E416809ULL + 117;
    ctx.v59.u64[0]           = 0xD193A57B2E416809ULL + 118;
    ctx.v59.u64[1]           = 0xD193A57B2E416809ULL + 119;
    ctx.v60.u64[0]           = 0xD193A57B2E416809ULL + 120;
    ctx.v60.u64[1]           = 0xD193A57B2E416809ULL + 121;
    ctx.v61.u64[0]           = 0xD193A57B2E416809ULL + 122;
    ctx.v61.u64[1]           = 0xD193A57B2E416809ULL + 123;
    ctx.v62.u64[0]           = 0xD193A57B2E416809ULL + 124;
    ctx.v62.u64[1]           = 0xD193A57B2E416809ULL + 125;
    ctx.v63.u64[0]           = 0xD193A57B2E416809ULL + 126;
    ctx.v63.u64[1]           = 0xD193A57B2E416809ULL + 127;
    ctx.v64.u64[0]           = 0xD193A57B2E416809ULL + 128;
    ctx.v64.u64[1]           = 0xD193A57B2E416809ULL + 129;
    ctx.v65.u64[0]           = 0xD193A57B2E416809ULL + 130;
    ctx.v65.u64[1]           = 0xD193A57B2E416809ULL + 131;
    ctx.v66.u64[0]           = 0xD193A57B2E416809ULL + 132;
    ctx.v66.u64[1]           = 0xD193A57B2E416809ULL + 133;
    ctx.v67.u64[0]           = 0xD193A57B2E416809ULL + 134;
    ctx.v67.u64[1]           = 0xD193A57B2E416809ULL + 135;
    ctx.v68.u64[0]           = 0xD193A57B2E416809ULL + 136;
    ctx.v68.u64[1]           = 0xD193A57B2E416809ULL + 137;
    ctx.v69.u64[0]           = 0xD193A57B2E416809ULL + 138;
    ctx.v69.u64[1]           = 0xD193A57B2E416809ULL + 139;
    ctx.v70.u64[0]           = 0xD193A57B2E416809ULL + 140;
    ctx.v70.u64[1]           = 0xD193A57B2E416809ULL + 141;
    ctx.v71.u64[0]           = 0xD193A57B2E416809ULL + 142;
    ctx.v71.u64[1]           = 0xD193A57B2E416809ULL + 143;
    ctx.v72.u64[0]           = 0xD193A57B2E416809ULL + 144;
    ctx.v72.u64[1]           = 0xD193A57B2E416809ULL + 145;
    ctx.v73.u64[0]           = 0xD193A57B2E416809ULL + 146;
    ctx.v73.u64[1]           = 0xD193A57B2E416809ULL + 147;
    ctx.v74.u64[0]           = 0xD193A57B2E416809ULL + 148;
    ctx.v74.u64[1]           = 0xD193A57B2E416809ULL + 149;
    ctx.v75.u64[0]           = 0xD193A57B2E416809ULL + 150;
    ctx.v75.u64[1]           = 0xD193A57B2E416809ULL + 151;
    ctx.v76.u64[0]           = 0xD193A57B2E416809ULL + 152;
    ctx.v76.u64[1]           = 0xD193A57B2E416809ULL + 153;
    ctx.v77.u64[0]           = 0xD193A57B2E416809ULL + 154;
    ctx.v77.u64[1]           = 0xD193A57B2E416809ULL + 155;
    ctx.v78.u64[0]           = 0xD193A57B2E416809ULL + 156;
    ctx.v78.u64[1]           = 0xD193A57B2E416809ULL + 157;
    ctx.v79.u64[0]           = 0xD193A57B2E416809ULL + 158;
    ctx.v79.u64[1]           = 0xD193A57B2E416809ULL + 159;
    ctx.v80.u64[0]           = 0xD193A57B2E416809ULL + 160;
    ctx.v80.u64[1]           = 0xD193A57B2E416809ULL + 161;
    ctx.v81.u64[0]           = 0xD193A57B2E416809ULL + 162;
    ctx.v81.u64[1]           = 0xD193A57B2E416809ULL + 163;
    ctx.v82.u64[0]           = 0xD193A57B2E416809ULL + 164;
    ctx.v82.u64[1]           = 0xD193A57B2E416809ULL + 165;
    ctx.v83.u64[0]           = 0xD193A57B2E416809ULL + 166;
    ctx.v83.u64[1]           = 0xD193A57B2E416809ULL + 167;
    ctx.v84.u64[0]           = 0xD193A57B2E416809ULL + 168;
    ctx.v84.u64[1]           = 0xD193A57B2E416809ULL + 169;
    ctx.v85.u64[0]           = 0xD193A57B2E416809ULL + 170;
    ctx.v85.u64[1]           = 0xD193A57B2E416809ULL + 171;
    ctx.v86.u64[0]           = 0xD193A57B2E416809ULL + 172;
    ctx.v86.u64[1]           = 0xD193A57B2E416809ULL + 173;
    ctx.v87.u64[0]           = 0xD193A57B2E416809ULL + 174;
    ctx.v87.u64[1]           = 0xD193A57B2E416809ULL + 175;
    ctx.v88.u64[0]           = 0xD193A57B2E416809ULL + 176;
    ctx.v88.u64[1]           = 0xD193A57B2E416809ULL + 177;
    ctx.v89.u64[0]           = 0xD193A57B2E416809ULL + 178;
    ctx.v89.u64[1]           = 0xD193A57B2E416809ULL + 179;
    ctx.v90.u64[0]           = 0xD193A57B2E416809ULL + 180;
    ctx.v90.u64[1]           = 0xD193A57B2E416809ULL + 181;
    ctx.v91.u64[0]           = 0xD193A57B2E416809ULL + 182;
    ctx.v91.u64[1]           = 0xD193A57B2E416809ULL + 183;
    ctx.v92.u64[0]           = 0xD193A57B2E416809ULL + 184;
    ctx.v92.u64[1]           = 0xD193A57B2E416809ULL + 185;
    ctx.v93.u64[0]           = 0xD193A57B2E416809ULL + 186;
    ctx.v93.u64[1]           = 0xD193A57B2E416809ULL + 187;
    ctx.v94.u64[0]           = 0xD193A57B2E416809ULL + 188;
    ctx.v94.u64[1]           = 0xD193A57B2E416809ULL + 189;
    ctx.v95.u64[0]           = 0xD193A57B2E416809ULL + 190;
    ctx.v95.u64[1]           = 0xD193A57B2E416809ULL + 191;
    ctx.v96.u64[0]           = 0xD193A57B2E416809ULL + 192;
    ctx.v96.u64[1]           = 0xD193A57B2E416809ULL + 193;
    ctx.v97.u64[0]           = 0xD193A57B2E416809ULL + 194;
    ctx.v97.u64[1]           = 0xD193A57B2E416809ULL + 195;
    ctx.v98.u64[0]           = 0xD193A57B2E416809ULL + 196;
    ctx.v98.u64[1]           = 0xD193A57B2E416809ULL + 197;
    ctx.v99.u64[0]           = 0xD193A57B2E416809ULL + 198;
    ctx.v99.u64[1]           = 0xD193A57B2E416809ULL + 199;
    ctx.v100.u64[0]          = 0xD193A57B2E416809ULL + 200;
    ctx.v100.u64[1]          = 0xD193A57B2E416809ULL + 201;
    ctx.v101.u64[0]          = 0xD193A57B2E416809ULL + 202;
    ctx.v101.u64[1]          = 0xD193A57B2E416809ULL + 203;
    ctx.v102.u64[0]          = 0xD193A57B2E416809ULL + 204;
    ctx.v102.u64[1]          = 0xD193A57B2E416809ULL + 205;
    ctx.v103.u64[0]          = 0xD193A57B2E416809ULL + 206;
    ctx.v103.u64[1]          = 0xD193A57B2E416809ULL + 207;
    ctx.v104.u64[0]          = 0xD193A57B2E416809ULL + 208;
    ctx.v104.u64[1]          = 0xD193A57B2E416809ULL + 209;
    ctx.v105.u64[0]          = 0xD193A57B2E416809ULL + 210;
    ctx.v105.u64[1]          = 0xD193A57B2E416809ULL + 211;
    ctx.v106.u64[0]          = 0xD193A57B2E416809ULL + 212;
    ctx.v106.u64[1]          = 0xD193A57B2E416809ULL + 213;
    ctx.v107.u64[0]          = 0xD193A57B2E416809ULL + 214;
    ctx.v107.u64[1]          = 0xD193A57B2E416809ULL + 215;
    ctx.v108.u64[0]          = 0xD193A57B2E416809ULL + 216;
    ctx.v108.u64[1]          = 0xD193A57B2E416809ULL + 217;
    ctx.v109.u64[0]          = 0xD193A57B2E416809ULL + 218;
    ctx.v109.u64[1]          = 0xD193A57B2E416809ULL + 219;
    ctx.v110.u64[0]          = 0xD193A57B2E416809ULL + 220;
    ctx.v110.u64[1]          = 0xD193A57B2E416809ULL + 221;
    ctx.v111.u64[0]          = 0xD193A57B2E416809ULL + 222;
    ctx.v111.u64[1]          = 0xD193A57B2E416809ULL + 223;
    ctx.v112.u64[0]          = 0xD193A57B2E416809ULL + 224;
    ctx.v112.u64[1]          = 0xD193A57B2E416809ULL + 225;
    ctx.v113.u64[0]          = 0xD193A57B2E416809ULL + 226;
    ctx.v113.u64[1]          = 0xD193A57B2E416809ULL + 227;
    ctx.v114.u64[0]          = 0xD193A57B2E416809ULL + 228;
    ctx.v114.u64[1]          = 0xD193A57B2E416809ULL + 229;
    ctx.v115.u64[0]          = 0xD193A57B2E416809ULL + 230;
    ctx.v115.u64[1]          = 0xD193A57B2E416809ULL + 231;
    ctx.v116.u64[0]          = 0xD193A57B2E416809ULL + 232;
    ctx.v116.u64[1]          = 0xD193A57B2E416809ULL + 233;
    ctx.v117.u64[0]          = 0xD193A57B2E416809ULL + 234;
    ctx.v117.u64[1]          = 0xD193A57B2E416809ULL + 235;
    ctx.v118.u64[0]          = 0xD193A57B2E416809ULL + 236;
    ctx.v118.u64[1]          = 0xD193A57B2E416809ULL + 237;
    ctx.v119.u64[0]          = 0xD193A57B2E416809ULL + 238;
    ctx.v119.u64[1]          = 0xD193A57B2E416809ULL + 239;
    ctx.v120.u64[0]          = 0xD193A57B2E416809ULL + 240;
    ctx.v120.u64[1]          = 0xD193A57B2E416809ULL + 241;
    ctx.v121.u64[0]          = 0xD193A57B2E416809ULL + 242;
    ctx.v121.u64[1]          = 0xD193A57B2E416809ULL + 243;
    ctx.v122.u64[0]          = 0xD193A57B2E416809ULL + 244;
    ctx.v122.u64[1]          = 0xD193A57B2E416809ULL + 245;
    ctx.v123.u64[0]          = 0xD193A57B2E416809ULL + 246;
    ctx.v123.u64[1]          = 0xD193A57B2E416809ULL + 247;
    ctx.v124.u64[0]          = 0xD193A57B2E416809ULL + 248;
    ctx.v124.u64[1]          = 0xD193A57B2E416809ULL + 249;
    ctx.v125.u64[0]          = 0xD193A57B2E416809ULL + 250;
    ctx.v125.u64[1]          = 0xD193A57B2E416809ULL + 251;
    ctx.v126.u64[0]          = 0xD193A57B2E416809ULL + 252;
    ctx.v126.u64[1]          = 0xD193A57B2E416809ULL + 253;
    ctx.v127.u64[0]          = 0xD193A57B2E416809ULL + 254;
    ctx.v127.u64[1]          = 0xD193A57B2E416809ULL + 255;
    ctx.cr0                  = { 1, 0, 1, 1 };
    ctx.cr1                  = { 1, 0, 1, 1 };
    ctx.cr2                  = { 1, 0, 1, 1 };
    ctx.cr3                  = { 1, 0, 1, 1 };
    ctx.cr4                  = { 1, 0, 1, 1 };
    ctx.cr5                  = { 1, 0, 1, 1 };
    ctx.cr6                  = { 1, 0, 1, 1 };
    ctx.cr7                  = { 1, 0, 1, 1 };
    ctx.r3.u64               = 0x7E93B5D100000000ULL | G;
    ctx.lr                   = 0x8250FA44;
    ctx.ctr.u64              = 0x91735426A8B1C3D5;
    ctx.reserved.u64         = 0xE1835B29D74F6A03;
    ctx.xer                  = { 1, 1, 1 };
    ctx.msr                  = 0x81B539D7;
    ctx.fpscr.csr            = 0x7135;
    ctx.vscr_sat             = 1;
    ctx.last_indirect_target = 0x8269B118;
    return ctx;
}

void sameContext(const PPCContext& a, const PPCContext& b)
{
    UNSCOPED_INFO("ABI r0");
    REQUIRE(a.r0.u64 == b.r0.u64);
    UNSCOPED_INFO("ABI r1");
    REQUIRE(a.r1.u64 == b.r1.u64);
    UNSCOPED_INFO("ABI r2");
    REQUIRE(a.r2.u64 == b.r2.u64);
    UNSCOPED_INFO("ABI r3");
    REQUIRE(a.r3.u64 == b.r3.u64);
    UNSCOPED_INFO("ABI r4");
    REQUIRE(a.r4.u64 == b.r4.u64);
    UNSCOPED_INFO("ABI r5");
    REQUIRE(a.r5.u64 == b.r5.u64);
    UNSCOPED_INFO("ABI r6");
    REQUIRE(a.r6.u64 == b.r6.u64);
    UNSCOPED_INFO("ABI r7");
    REQUIRE(a.r7.u64 == b.r7.u64);
    UNSCOPED_INFO("ABI r8");
    REQUIRE(a.r8.u64 == b.r8.u64);
    UNSCOPED_INFO("ABI r9");
    REQUIRE(a.r9.u64 == b.r9.u64);
    UNSCOPED_INFO("ABI r10");
    REQUIRE(a.r10.u64 == b.r10.u64);
    UNSCOPED_INFO("ABI r11");
    REQUIRE(a.r11.u64 == b.r11.u64);
    UNSCOPED_INFO("ABI r12");
    REQUIRE(a.r12.u64 == b.r12.u64);
    UNSCOPED_INFO("ABI r13");
    REQUIRE(a.r13.u64 == b.r13.u64);
    UNSCOPED_INFO("ABI r14");
    REQUIRE(a.r14.u64 == b.r14.u64);
    UNSCOPED_INFO("ABI r15");
    REQUIRE(a.r15.u64 == b.r15.u64);
    UNSCOPED_INFO("ABI r16");
    REQUIRE(a.r16.u64 == b.r16.u64);
    UNSCOPED_INFO("ABI r17");
    REQUIRE(a.r17.u64 == b.r17.u64);
    UNSCOPED_INFO("ABI r18");
    REQUIRE(a.r18.u64 == b.r18.u64);
    UNSCOPED_INFO("ABI r19");
    REQUIRE(a.r19.u64 == b.r19.u64);
    UNSCOPED_INFO("ABI r20");
    REQUIRE(a.r20.u64 == b.r20.u64);
    UNSCOPED_INFO("ABI r21");
    REQUIRE(a.r21.u64 == b.r21.u64);
    UNSCOPED_INFO("ABI r22");
    REQUIRE(a.r22.u64 == b.r22.u64);
    UNSCOPED_INFO("ABI r23");
    REQUIRE(a.r23.u64 == b.r23.u64);
    UNSCOPED_INFO("ABI r24");
    REQUIRE(a.r24.u64 == b.r24.u64);
    UNSCOPED_INFO("ABI r25");
    REQUIRE(a.r25.u64 == b.r25.u64);
    UNSCOPED_INFO("ABI r26");
    REQUIRE(a.r26.u64 == b.r26.u64);
    UNSCOPED_INFO("ABI r27");
    REQUIRE(a.r27.u64 == b.r27.u64);
    UNSCOPED_INFO("ABI r28");
    REQUIRE(a.r28.u64 == b.r28.u64);
    UNSCOPED_INFO("ABI r29");
    REQUIRE(a.r29.u64 == b.r29.u64);
    UNSCOPED_INFO("ABI r30");
    REQUIRE(a.r30.u64 == b.r30.u64);
    UNSCOPED_INFO("ABI r31");
    REQUIRE(a.r31.u64 == b.r31.u64);
    UNSCOPED_INFO("ABI f0");
    REQUIRE(a.f0.u64 == b.f0.u64);
    UNSCOPED_INFO("ABI f1");
    REQUIRE(a.f1.u64 == b.f1.u64);
    UNSCOPED_INFO("ABI f2");
    REQUIRE(a.f2.u64 == b.f2.u64);
    UNSCOPED_INFO("ABI f3");
    REQUIRE(a.f3.u64 == b.f3.u64);
    UNSCOPED_INFO("ABI f4");
    REQUIRE(a.f4.u64 == b.f4.u64);
    UNSCOPED_INFO("ABI f5");
    REQUIRE(a.f5.u64 == b.f5.u64);
    UNSCOPED_INFO("ABI f6");
    REQUIRE(a.f6.u64 == b.f6.u64);
    UNSCOPED_INFO("ABI f7");
    REQUIRE(a.f7.u64 == b.f7.u64);
    UNSCOPED_INFO("ABI f8");
    REQUIRE(a.f8.u64 == b.f8.u64);
    UNSCOPED_INFO("ABI f9");
    REQUIRE(a.f9.u64 == b.f9.u64);
    UNSCOPED_INFO("ABI f10");
    REQUIRE(a.f10.u64 == b.f10.u64);
    UNSCOPED_INFO("ABI f11");
    REQUIRE(a.f11.u64 == b.f11.u64);
    UNSCOPED_INFO("ABI f12");
    REQUIRE(a.f12.u64 == b.f12.u64);
    UNSCOPED_INFO("ABI f13");
    REQUIRE(a.f13.u64 == b.f13.u64);
    UNSCOPED_INFO("ABI f14");
    REQUIRE(a.f14.u64 == b.f14.u64);
    UNSCOPED_INFO("ABI f15");
    REQUIRE(a.f15.u64 == b.f15.u64);
    UNSCOPED_INFO("ABI f16");
    REQUIRE(a.f16.u64 == b.f16.u64);
    UNSCOPED_INFO("ABI f17");
    REQUIRE(a.f17.u64 == b.f17.u64);
    UNSCOPED_INFO("ABI f18");
    REQUIRE(a.f18.u64 == b.f18.u64);
    UNSCOPED_INFO("ABI f19");
    REQUIRE(a.f19.u64 == b.f19.u64);
    UNSCOPED_INFO("ABI f20");
    REQUIRE(a.f20.u64 == b.f20.u64);
    UNSCOPED_INFO("ABI f21");
    REQUIRE(a.f21.u64 == b.f21.u64);
    UNSCOPED_INFO("ABI f22");
    REQUIRE(a.f22.u64 == b.f22.u64);
    UNSCOPED_INFO("ABI f23");
    REQUIRE(a.f23.u64 == b.f23.u64);
    UNSCOPED_INFO("ABI f24");
    REQUIRE(a.f24.u64 == b.f24.u64);
    UNSCOPED_INFO("ABI f25");
    REQUIRE(a.f25.u64 == b.f25.u64);
    UNSCOPED_INFO("ABI f26");
    REQUIRE(a.f26.u64 == b.f26.u64);
    UNSCOPED_INFO("ABI f27");
    REQUIRE(a.f27.u64 == b.f27.u64);
    UNSCOPED_INFO("ABI f28");
    REQUIRE(a.f28.u64 == b.f28.u64);
    UNSCOPED_INFO("ABI f29");
    REQUIRE(a.f29.u64 == b.f29.u64);
    UNSCOPED_INFO("ABI f30");
    REQUIRE(a.f30.u64 == b.f30.u64);
    UNSCOPED_INFO("ABI f31");
    REQUIRE(a.f31.u64 == b.f31.u64);
    UNSCOPED_INFO("ABI v0[0]");
    REQUIRE(a.v0.u64[0] == b.v0.u64[0]);
    UNSCOPED_INFO("ABI v0[1]");
    REQUIRE(a.v0.u64[1] == b.v0.u64[1]);
    UNSCOPED_INFO("ABI v1[0]");
    REQUIRE(a.v1.u64[0] == b.v1.u64[0]);
    UNSCOPED_INFO("ABI v1[1]");
    REQUIRE(a.v1.u64[1] == b.v1.u64[1]);
    UNSCOPED_INFO("ABI v2[0]");
    REQUIRE(a.v2.u64[0] == b.v2.u64[0]);
    UNSCOPED_INFO("ABI v2[1]");
    REQUIRE(a.v2.u64[1] == b.v2.u64[1]);
    UNSCOPED_INFO("ABI v3[0]");
    REQUIRE(a.v3.u64[0] == b.v3.u64[0]);
    UNSCOPED_INFO("ABI v3[1]");
    REQUIRE(a.v3.u64[1] == b.v3.u64[1]);
    UNSCOPED_INFO("ABI v4[0]");
    REQUIRE(a.v4.u64[0] == b.v4.u64[0]);
    UNSCOPED_INFO("ABI v4[1]");
    REQUIRE(a.v4.u64[1] == b.v4.u64[1]);
    UNSCOPED_INFO("ABI v5[0]");
    REQUIRE(a.v5.u64[0] == b.v5.u64[0]);
    UNSCOPED_INFO("ABI v5[1]");
    REQUIRE(a.v5.u64[1] == b.v5.u64[1]);
    UNSCOPED_INFO("ABI v6[0]");
    REQUIRE(a.v6.u64[0] == b.v6.u64[0]);
    UNSCOPED_INFO("ABI v6[1]");
    REQUIRE(a.v6.u64[1] == b.v6.u64[1]);
    UNSCOPED_INFO("ABI v7[0]");
    REQUIRE(a.v7.u64[0] == b.v7.u64[0]);
    UNSCOPED_INFO("ABI v7[1]");
    REQUIRE(a.v7.u64[1] == b.v7.u64[1]);
    UNSCOPED_INFO("ABI v8[0]");
    REQUIRE(a.v8.u64[0] == b.v8.u64[0]);
    UNSCOPED_INFO("ABI v8[1]");
    REQUIRE(a.v8.u64[1] == b.v8.u64[1]);
    UNSCOPED_INFO("ABI v9[0]");
    REQUIRE(a.v9.u64[0] == b.v9.u64[0]);
    UNSCOPED_INFO("ABI v9[1]");
    REQUIRE(a.v9.u64[1] == b.v9.u64[1]);
    UNSCOPED_INFO("ABI v10[0]");
    REQUIRE(a.v10.u64[0] == b.v10.u64[0]);
    UNSCOPED_INFO("ABI v10[1]");
    REQUIRE(a.v10.u64[1] == b.v10.u64[1]);
    UNSCOPED_INFO("ABI v11[0]");
    REQUIRE(a.v11.u64[0] == b.v11.u64[0]);
    UNSCOPED_INFO("ABI v11[1]");
    REQUIRE(a.v11.u64[1] == b.v11.u64[1]);
    UNSCOPED_INFO("ABI v12[0]");
    REQUIRE(a.v12.u64[0] == b.v12.u64[0]);
    UNSCOPED_INFO("ABI v12[1]");
    REQUIRE(a.v12.u64[1] == b.v12.u64[1]);
    UNSCOPED_INFO("ABI v13[0]");
    REQUIRE(a.v13.u64[0] == b.v13.u64[0]);
    UNSCOPED_INFO("ABI v13[1]");
    REQUIRE(a.v13.u64[1] == b.v13.u64[1]);
    UNSCOPED_INFO("ABI v14[0]");
    REQUIRE(a.v14.u64[0] == b.v14.u64[0]);
    UNSCOPED_INFO("ABI v14[1]");
    REQUIRE(a.v14.u64[1] == b.v14.u64[1]);
    UNSCOPED_INFO("ABI v15[0]");
    REQUIRE(a.v15.u64[0] == b.v15.u64[0]);
    UNSCOPED_INFO("ABI v15[1]");
    REQUIRE(a.v15.u64[1] == b.v15.u64[1]);
    UNSCOPED_INFO("ABI v16[0]");
    REQUIRE(a.v16.u64[0] == b.v16.u64[0]);
    UNSCOPED_INFO("ABI v16[1]");
    REQUIRE(a.v16.u64[1] == b.v16.u64[1]);
    UNSCOPED_INFO("ABI v17[0]");
    REQUIRE(a.v17.u64[0] == b.v17.u64[0]);
    UNSCOPED_INFO("ABI v17[1]");
    REQUIRE(a.v17.u64[1] == b.v17.u64[1]);
    UNSCOPED_INFO("ABI v18[0]");
    REQUIRE(a.v18.u64[0] == b.v18.u64[0]);
    UNSCOPED_INFO("ABI v18[1]");
    REQUIRE(a.v18.u64[1] == b.v18.u64[1]);
    UNSCOPED_INFO("ABI v19[0]");
    REQUIRE(a.v19.u64[0] == b.v19.u64[0]);
    UNSCOPED_INFO("ABI v19[1]");
    REQUIRE(a.v19.u64[1] == b.v19.u64[1]);
    UNSCOPED_INFO("ABI v20[0]");
    REQUIRE(a.v20.u64[0] == b.v20.u64[0]);
    UNSCOPED_INFO("ABI v20[1]");
    REQUIRE(a.v20.u64[1] == b.v20.u64[1]);
    UNSCOPED_INFO("ABI v21[0]");
    REQUIRE(a.v21.u64[0] == b.v21.u64[0]);
    UNSCOPED_INFO("ABI v21[1]");
    REQUIRE(a.v21.u64[1] == b.v21.u64[1]);
    UNSCOPED_INFO("ABI v22[0]");
    REQUIRE(a.v22.u64[0] == b.v22.u64[0]);
    UNSCOPED_INFO("ABI v22[1]");
    REQUIRE(a.v22.u64[1] == b.v22.u64[1]);
    UNSCOPED_INFO("ABI v23[0]");
    REQUIRE(a.v23.u64[0] == b.v23.u64[0]);
    UNSCOPED_INFO("ABI v23[1]");
    REQUIRE(a.v23.u64[1] == b.v23.u64[1]);
    UNSCOPED_INFO("ABI v24[0]");
    REQUIRE(a.v24.u64[0] == b.v24.u64[0]);
    UNSCOPED_INFO("ABI v24[1]");
    REQUIRE(a.v24.u64[1] == b.v24.u64[1]);
    UNSCOPED_INFO("ABI v25[0]");
    REQUIRE(a.v25.u64[0] == b.v25.u64[0]);
    UNSCOPED_INFO("ABI v25[1]");
    REQUIRE(a.v25.u64[1] == b.v25.u64[1]);
    UNSCOPED_INFO("ABI v26[0]");
    REQUIRE(a.v26.u64[0] == b.v26.u64[0]);
    UNSCOPED_INFO("ABI v26[1]");
    REQUIRE(a.v26.u64[1] == b.v26.u64[1]);
    UNSCOPED_INFO("ABI v27[0]");
    REQUIRE(a.v27.u64[0] == b.v27.u64[0]);
    UNSCOPED_INFO("ABI v27[1]");
    REQUIRE(a.v27.u64[1] == b.v27.u64[1]);
    UNSCOPED_INFO("ABI v28[0]");
    REQUIRE(a.v28.u64[0] == b.v28.u64[0]);
    UNSCOPED_INFO("ABI v28[1]");
    REQUIRE(a.v28.u64[1] == b.v28.u64[1]);
    UNSCOPED_INFO("ABI v29[0]");
    REQUIRE(a.v29.u64[0] == b.v29.u64[0]);
    UNSCOPED_INFO("ABI v29[1]");
    REQUIRE(a.v29.u64[1] == b.v29.u64[1]);
    UNSCOPED_INFO("ABI v30[0]");
    REQUIRE(a.v30.u64[0] == b.v30.u64[0]);
    UNSCOPED_INFO("ABI v30[1]");
    REQUIRE(a.v30.u64[1] == b.v30.u64[1]);
    UNSCOPED_INFO("ABI v31[0]");
    REQUIRE(a.v31.u64[0] == b.v31.u64[0]);
    UNSCOPED_INFO("ABI v31[1]");
    REQUIRE(a.v31.u64[1] == b.v31.u64[1]);
    UNSCOPED_INFO("ABI v32[0]");
    REQUIRE(a.v32.u64[0] == b.v32.u64[0]);
    UNSCOPED_INFO("ABI v32[1]");
    REQUIRE(a.v32.u64[1] == b.v32.u64[1]);
    UNSCOPED_INFO("ABI v33[0]");
    REQUIRE(a.v33.u64[0] == b.v33.u64[0]);
    UNSCOPED_INFO("ABI v33[1]");
    REQUIRE(a.v33.u64[1] == b.v33.u64[1]);
    UNSCOPED_INFO("ABI v34[0]");
    REQUIRE(a.v34.u64[0] == b.v34.u64[0]);
    UNSCOPED_INFO("ABI v34[1]");
    REQUIRE(a.v34.u64[1] == b.v34.u64[1]);
    UNSCOPED_INFO("ABI v35[0]");
    REQUIRE(a.v35.u64[0] == b.v35.u64[0]);
    UNSCOPED_INFO("ABI v35[1]");
    REQUIRE(a.v35.u64[1] == b.v35.u64[1]);
    UNSCOPED_INFO("ABI v36[0]");
    REQUIRE(a.v36.u64[0] == b.v36.u64[0]);
    UNSCOPED_INFO("ABI v36[1]");
    REQUIRE(a.v36.u64[1] == b.v36.u64[1]);
    UNSCOPED_INFO("ABI v37[0]");
    REQUIRE(a.v37.u64[0] == b.v37.u64[0]);
    UNSCOPED_INFO("ABI v37[1]");
    REQUIRE(a.v37.u64[1] == b.v37.u64[1]);
    UNSCOPED_INFO("ABI v38[0]");
    REQUIRE(a.v38.u64[0] == b.v38.u64[0]);
    UNSCOPED_INFO("ABI v38[1]");
    REQUIRE(a.v38.u64[1] == b.v38.u64[1]);
    UNSCOPED_INFO("ABI v39[0]");
    REQUIRE(a.v39.u64[0] == b.v39.u64[0]);
    UNSCOPED_INFO("ABI v39[1]");
    REQUIRE(a.v39.u64[1] == b.v39.u64[1]);
    UNSCOPED_INFO("ABI v40[0]");
    REQUIRE(a.v40.u64[0] == b.v40.u64[0]);
    UNSCOPED_INFO("ABI v40[1]");
    REQUIRE(a.v40.u64[1] == b.v40.u64[1]);
    UNSCOPED_INFO("ABI v41[0]");
    REQUIRE(a.v41.u64[0] == b.v41.u64[0]);
    UNSCOPED_INFO("ABI v41[1]");
    REQUIRE(a.v41.u64[1] == b.v41.u64[1]);
    UNSCOPED_INFO("ABI v42[0]");
    REQUIRE(a.v42.u64[0] == b.v42.u64[0]);
    UNSCOPED_INFO("ABI v42[1]");
    REQUIRE(a.v42.u64[1] == b.v42.u64[1]);
    UNSCOPED_INFO("ABI v43[0]");
    REQUIRE(a.v43.u64[0] == b.v43.u64[0]);
    UNSCOPED_INFO("ABI v43[1]");
    REQUIRE(a.v43.u64[1] == b.v43.u64[1]);
    UNSCOPED_INFO("ABI v44[0]");
    REQUIRE(a.v44.u64[0] == b.v44.u64[0]);
    UNSCOPED_INFO("ABI v44[1]");
    REQUIRE(a.v44.u64[1] == b.v44.u64[1]);
    UNSCOPED_INFO("ABI v45[0]");
    REQUIRE(a.v45.u64[0] == b.v45.u64[0]);
    UNSCOPED_INFO("ABI v45[1]");
    REQUIRE(a.v45.u64[1] == b.v45.u64[1]);
    UNSCOPED_INFO("ABI v46[0]");
    REQUIRE(a.v46.u64[0] == b.v46.u64[0]);
    UNSCOPED_INFO("ABI v46[1]");
    REQUIRE(a.v46.u64[1] == b.v46.u64[1]);
    UNSCOPED_INFO("ABI v47[0]");
    REQUIRE(a.v47.u64[0] == b.v47.u64[0]);
    UNSCOPED_INFO("ABI v47[1]");
    REQUIRE(a.v47.u64[1] == b.v47.u64[1]);
    UNSCOPED_INFO("ABI v48[0]");
    REQUIRE(a.v48.u64[0] == b.v48.u64[0]);
    UNSCOPED_INFO("ABI v48[1]");
    REQUIRE(a.v48.u64[1] == b.v48.u64[1]);
    UNSCOPED_INFO("ABI v49[0]");
    REQUIRE(a.v49.u64[0] == b.v49.u64[0]);
    UNSCOPED_INFO("ABI v49[1]");
    REQUIRE(a.v49.u64[1] == b.v49.u64[1]);
    UNSCOPED_INFO("ABI v50[0]");
    REQUIRE(a.v50.u64[0] == b.v50.u64[0]);
    UNSCOPED_INFO("ABI v50[1]");
    REQUIRE(a.v50.u64[1] == b.v50.u64[1]);
    UNSCOPED_INFO("ABI v51[0]");
    REQUIRE(a.v51.u64[0] == b.v51.u64[0]);
    UNSCOPED_INFO("ABI v51[1]");
    REQUIRE(a.v51.u64[1] == b.v51.u64[1]);
    UNSCOPED_INFO("ABI v52[0]");
    REQUIRE(a.v52.u64[0] == b.v52.u64[0]);
    UNSCOPED_INFO("ABI v52[1]");
    REQUIRE(a.v52.u64[1] == b.v52.u64[1]);
    UNSCOPED_INFO("ABI v53[0]");
    REQUIRE(a.v53.u64[0] == b.v53.u64[0]);
    UNSCOPED_INFO("ABI v53[1]");
    REQUIRE(a.v53.u64[1] == b.v53.u64[1]);
    UNSCOPED_INFO("ABI v54[0]");
    REQUIRE(a.v54.u64[0] == b.v54.u64[0]);
    UNSCOPED_INFO("ABI v54[1]");
    REQUIRE(a.v54.u64[1] == b.v54.u64[1]);
    UNSCOPED_INFO("ABI v55[0]");
    REQUIRE(a.v55.u64[0] == b.v55.u64[0]);
    UNSCOPED_INFO("ABI v55[1]");
    REQUIRE(a.v55.u64[1] == b.v55.u64[1]);
    UNSCOPED_INFO("ABI v56[0]");
    REQUIRE(a.v56.u64[0] == b.v56.u64[0]);
    UNSCOPED_INFO("ABI v56[1]");
    REQUIRE(a.v56.u64[1] == b.v56.u64[1]);
    UNSCOPED_INFO("ABI v57[0]");
    REQUIRE(a.v57.u64[0] == b.v57.u64[0]);
    UNSCOPED_INFO("ABI v57[1]");
    REQUIRE(a.v57.u64[1] == b.v57.u64[1]);
    UNSCOPED_INFO("ABI v58[0]");
    REQUIRE(a.v58.u64[0] == b.v58.u64[0]);
    UNSCOPED_INFO("ABI v58[1]");
    REQUIRE(a.v58.u64[1] == b.v58.u64[1]);
    UNSCOPED_INFO("ABI v59[0]");
    REQUIRE(a.v59.u64[0] == b.v59.u64[0]);
    UNSCOPED_INFO("ABI v59[1]");
    REQUIRE(a.v59.u64[1] == b.v59.u64[1]);
    UNSCOPED_INFO("ABI v60[0]");
    REQUIRE(a.v60.u64[0] == b.v60.u64[0]);
    UNSCOPED_INFO("ABI v60[1]");
    REQUIRE(a.v60.u64[1] == b.v60.u64[1]);
    UNSCOPED_INFO("ABI v61[0]");
    REQUIRE(a.v61.u64[0] == b.v61.u64[0]);
    UNSCOPED_INFO("ABI v61[1]");
    REQUIRE(a.v61.u64[1] == b.v61.u64[1]);
    UNSCOPED_INFO("ABI v62[0]");
    REQUIRE(a.v62.u64[0] == b.v62.u64[0]);
    UNSCOPED_INFO("ABI v62[1]");
    REQUIRE(a.v62.u64[1] == b.v62.u64[1]);
    UNSCOPED_INFO("ABI v63[0]");
    REQUIRE(a.v63.u64[0] == b.v63.u64[0]);
    UNSCOPED_INFO("ABI v63[1]");
    REQUIRE(a.v63.u64[1] == b.v63.u64[1]);
    UNSCOPED_INFO("ABI v64[0]");
    REQUIRE(a.v64.u64[0] == b.v64.u64[0]);
    UNSCOPED_INFO("ABI v64[1]");
    REQUIRE(a.v64.u64[1] == b.v64.u64[1]);
    UNSCOPED_INFO("ABI v65[0]");
    REQUIRE(a.v65.u64[0] == b.v65.u64[0]);
    UNSCOPED_INFO("ABI v65[1]");
    REQUIRE(a.v65.u64[1] == b.v65.u64[1]);
    UNSCOPED_INFO("ABI v66[0]");
    REQUIRE(a.v66.u64[0] == b.v66.u64[0]);
    UNSCOPED_INFO("ABI v66[1]");
    REQUIRE(a.v66.u64[1] == b.v66.u64[1]);
    UNSCOPED_INFO("ABI v67[0]");
    REQUIRE(a.v67.u64[0] == b.v67.u64[0]);
    UNSCOPED_INFO("ABI v67[1]");
    REQUIRE(a.v67.u64[1] == b.v67.u64[1]);
    UNSCOPED_INFO("ABI v68[0]");
    REQUIRE(a.v68.u64[0] == b.v68.u64[0]);
    UNSCOPED_INFO("ABI v68[1]");
    REQUIRE(a.v68.u64[1] == b.v68.u64[1]);
    UNSCOPED_INFO("ABI v69[0]");
    REQUIRE(a.v69.u64[0] == b.v69.u64[0]);
    UNSCOPED_INFO("ABI v69[1]");
    REQUIRE(a.v69.u64[1] == b.v69.u64[1]);
    UNSCOPED_INFO("ABI v70[0]");
    REQUIRE(a.v70.u64[0] == b.v70.u64[0]);
    UNSCOPED_INFO("ABI v70[1]");
    REQUIRE(a.v70.u64[1] == b.v70.u64[1]);
    UNSCOPED_INFO("ABI v71[0]");
    REQUIRE(a.v71.u64[0] == b.v71.u64[0]);
    UNSCOPED_INFO("ABI v71[1]");
    REQUIRE(a.v71.u64[1] == b.v71.u64[1]);
    UNSCOPED_INFO("ABI v72[0]");
    REQUIRE(a.v72.u64[0] == b.v72.u64[0]);
    UNSCOPED_INFO("ABI v72[1]");
    REQUIRE(a.v72.u64[1] == b.v72.u64[1]);
    UNSCOPED_INFO("ABI v73[0]");
    REQUIRE(a.v73.u64[0] == b.v73.u64[0]);
    UNSCOPED_INFO("ABI v73[1]");
    REQUIRE(a.v73.u64[1] == b.v73.u64[1]);
    UNSCOPED_INFO("ABI v74[0]");
    REQUIRE(a.v74.u64[0] == b.v74.u64[0]);
    UNSCOPED_INFO("ABI v74[1]");
    REQUIRE(a.v74.u64[1] == b.v74.u64[1]);
    UNSCOPED_INFO("ABI v75[0]");
    REQUIRE(a.v75.u64[0] == b.v75.u64[0]);
    UNSCOPED_INFO("ABI v75[1]");
    REQUIRE(a.v75.u64[1] == b.v75.u64[1]);
    UNSCOPED_INFO("ABI v76[0]");
    REQUIRE(a.v76.u64[0] == b.v76.u64[0]);
    UNSCOPED_INFO("ABI v76[1]");
    REQUIRE(a.v76.u64[1] == b.v76.u64[1]);
    UNSCOPED_INFO("ABI v77[0]");
    REQUIRE(a.v77.u64[0] == b.v77.u64[0]);
    UNSCOPED_INFO("ABI v77[1]");
    REQUIRE(a.v77.u64[1] == b.v77.u64[1]);
    UNSCOPED_INFO("ABI v78[0]");
    REQUIRE(a.v78.u64[0] == b.v78.u64[0]);
    UNSCOPED_INFO("ABI v78[1]");
    REQUIRE(a.v78.u64[1] == b.v78.u64[1]);
    UNSCOPED_INFO("ABI v79[0]");
    REQUIRE(a.v79.u64[0] == b.v79.u64[0]);
    UNSCOPED_INFO("ABI v79[1]");
    REQUIRE(a.v79.u64[1] == b.v79.u64[1]);
    UNSCOPED_INFO("ABI v80[0]");
    REQUIRE(a.v80.u64[0] == b.v80.u64[0]);
    UNSCOPED_INFO("ABI v80[1]");
    REQUIRE(a.v80.u64[1] == b.v80.u64[1]);
    UNSCOPED_INFO("ABI v81[0]");
    REQUIRE(a.v81.u64[0] == b.v81.u64[0]);
    UNSCOPED_INFO("ABI v81[1]");
    REQUIRE(a.v81.u64[1] == b.v81.u64[1]);
    UNSCOPED_INFO("ABI v82[0]");
    REQUIRE(a.v82.u64[0] == b.v82.u64[0]);
    UNSCOPED_INFO("ABI v82[1]");
    REQUIRE(a.v82.u64[1] == b.v82.u64[1]);
    UNSCOPED_INFO("ABI v83[0]");
    REQUIRE(a.v83.u64[0] == b.v83.u64[0]);
    UNSCOPED_INFO("ABI v83[1]");
    REQUIRE(a.v83.u64[1] == b.v83.u64[1]);
    UNSCOPED_INFO("ABI v84[0]");
    REQUIRE(a.v84.u64[0] == b.v84.u64[0]);
    UNSCOPED_INFO("ABI v84[1]");
    REQUIRE(a.v84.u64[1] == b.v84.u64[1]);
    UNSCOPED_INFO("ABI v85[0]");
    REQUIRE(a.v85.u64[0] == b.v85.u64[0]);
    UNSCOPED_INFO("ABI v85[1]");
    REQUIRE(a.v85.u64[1] == b.v85.u64[1]);
    UNSCOPED_INFO("ABI v86[0]");
    REQUIRE(a.v86.u64[0] == b.v86.u64[0]);
    UNSCOPED_INFO("ABI v86[1]");
    REQUIRE(a.v86.u64[1] == b.v86.u64[1]);
    UNSCOPED_INFO("ABI v87[0]");
    REQUIRE(a.v87.u64[0] == b.v87.u64[0]);
    UNSCOPED_INFO("ABI v87[1]");
    REQUIRE(a.v87.u64[1] == b.v87.u64[1]);
    UNSCOPED_INFO("ABI v88[0]");
    REQUIRE(a.v88.u64[0] == b.v88.u64[0]);
    UNSCOPED_INFO("ABI v88[1]");
    REQUIRE(a.v88.u64[1] == b.v88.u64[1]);
    UNSCOPED_INFO("ABI v89[0]");
    REQUIRE(a.v89.u64[0] == b.v89.u64[0]);
    UNSCOPED_INFO("ABI v89[1]");
    REQUIRE(a.v89.u64[1] == b.v89.u64[1]);
    UNSCOPED_INFO("ABI v90[0]");
    REQUIRE(a.v90.u64[0] == b.v90.u64[0]);
    UNSCOPED_INFO("ABI v90[1]");
    REQUIRE(a.v90.u64[1] == b.v90.u64[1]);
    UNSCOPED_INFO("ABI v91[0]");
    REQUIRE(a.v91.u64[0] == b.v91.u64[0]);
    UNSCOPED_INFO("ABI v91[1]");
    REQUIRE(a.v91.u64[1] == b.v91.u64[1]);
    UNSCOPED_INFO("ABI v92[0]");
    REQUIRE(a.v92.u64[0] == b.v92.u64[0]);
    UNSCOPED_INFO("ABI v92[1]");
    REQUIRE(a.v92.u64[1] == b.v92.u64[1]);
    UNSCOPED_INFO("ABI v93[0]");
    REQUIRE(a.v93.u64[0] == b.v93.u64[0]);
    UNSCOPED_INFO("ABI v93[1]");
    REQUIRE(a.v93.u64[1] == b.v93.u64[1]);
    UNSCOPED_INFO("ABI v94[0]");
    REQUIRE(a.v94.u64[0] == b.v94.u64[0]);
    UNSCOPED_INFO("ABI v94[1]");
    REQUIRE(a.v94.u64[1] == b.v94.u64[1]);
    UNSCOPED_INFO("ABI v95[0]");
    REQUIRE(a.v95.u64[0] == b.v95.u64[0]);
    UNSCOPED_INFO("ABI v95[1]");
    REQUIRE(a.v95.u64[1] == b.v95.u64[1]);
    UNSCOPED_INFO("ABI v96[0]");
    REQUIRE(a.v96.u64[0] == b.v96.u64[0]);
    UNSCOPED_INFO("ABI v96[1]");
    REQUIRE(a.v96.u64[1] == b.v96.u64[1]);
    UNSCOPED_INFO("ABI v97[0]");
    REQUIRE(a.v97.u64[0] == b.v97.u64[0]);
    UNSCOPED_INFO("ABI v97[1]");
    REQUIRE(a.v97.u64[1] == b.v97.u64[1]);
    UNSCOPED_INFO("ABI v98[0]");
    REQUIRE(a.v98.u64[0] == b.v98.u64[0]);
    UNSCOPED_INFO("ABI v98[1]");
    REQUIRE(a.v98.u64[1] == b.v98.u64[1]);
    UNSCOPED_INFO("ABI v99[0]");
    REQUIRE(a.v99.u64[0] == b.v99.u64[0]);
    UNSCOPED_INFO("ABI v99[1]");
    REQUIRE(a.v99.u64[1] == b.v99.u64[1]);
    UNSCOPED_INFO("ABI v100[0]");
    REQUIRE(a.v100.u64[0] == b.v100.u64[0]);
    UNSCOPED_INFO("ABI v100[1]");
    REQUIRE(a.v100.u64[1] == b.v100.u64[1]);
    UNSCOPED_INFO("ABI v101[0]");
    REQUIRE(a.v101.u64[0] == b.v101.u64[0]);
    UNSCOPED_INFO("ABI v101[1]");
    REQUIRE(a.v101.u64[1] == b.v101.u64[1]);
    UNSCOPED_INFO("ABI v102[0]");
    REQUIRE(a.v102.u64[0] == b.v102.u64[0]);
    UNSCOPED_INFO("ABI v102[1]");
    REQUIRE(a.v102.u64[1] == b.v102.u64[1]);
    UNSCOPED_INFO("ABI v103[0]");
    REQUIRE(a.v103.u64[0] == b.v103.u64[0]);
    UNSCOPED_INFO("ABI v103[1]");
    REQUIRE(a.v103.u64[1] == b.v103.u64[1]);
    UNSCOPED_INFO("ABI v104[0]");
    REQUIRE(a.v104.u64[0] == b.v104.u64[0]);
    UNSCOPED_INFO("ABI v104[1]");
    REQUIRE(a.v104.u64[1] == b.v104.u64[1]);
    UNSCOPED_INFO("ABI v105[0]");
    REQUIRE(a.v105.u64[0] == b.v105.u64[0]);
    UNSCOPED_INFO("ABI v105[1]");
    REQUIRE(a.v105.u64[1] == b.v105.u64[1]);
    UNSCOPED_INFO("ABI v106[0]");
    REQUIRE(a.v106.u64[0] == b.v106.u64[0]);
    UNSCOPED_INFO("ABI v106[1]");
    REQUIRE(a.v106.u64[1] == b.v106.u64[1]);
    UNSCOPED_INFO("ABI v107[0]");
    REQUIRE(a.v107.u64[0] == b.v107.u64[0]);
    UNSCOPED_INFO("ABI v107[1]");
    REQUIRE(a.v107.u64[1] == b.v107.u64[1]);
    UNSCOPED_INFO("ABI v108[0]");
    REQUIRE(a.v108.u64[0] == b.v108.u64[0]);
    UNSCOPED_INFO("ABI v108[1]");
    REQUIRE(a.v108.u64[1] == b.v108.u64[1]);
    UNSCOPED_INFO("ABI v109[0]");
    REQUIRE(a.v109.u64[0] == b.v109.u64[0]);
    UNSCOPED_INFO("ABI v109[1]");
    REQUIRE(a.v109.u64[1] == b.v109.u64[1]);
    UNSCOPED_INFO("ABI v110[0]");
    REQUIRE(a.v110.u64[0] == b.v110.u64[0]);
    UNSCOPED_INFO("ABI v110[1]");
    REQUIRE(a.v110.u64[1] == b.v110.u64[1]);
    UNSCOPED_INFO("ABI v111[0]");
    REQUIRE(a.v111.u64[0] == b.v111.u64[0]);
    UNSCOPED_INFO("ABI v111[1]");
    REQUIRE(a.v111.u64[1] == b.v111.u64[1]);
    UNSCOPED_INFO("ABI v112[0]");
    REQUIRE(a.v112.u64[0] == b.v112.u64[0]);
    UNSCOPED_INFO("ABI v112[1]");
    REQUIRE(a.v112.u64[1] == b.v112.u64[1]);
    UNSCOPED_INFO("ABI v113[0]");
    REQUIRE(a.v113.u64[0] == b.v113.u64[0]);
    UNSCOPED_INFO("ABI v113[1]");
    REQUIRE(a.v113.u64[1] == b.v113.u64[1]);
    UNSCOPED_INFO("ABI v114[0]");
    REQUIRE(a.v114.u64[0] == b.v114.u64[0]);
    UNSCOPED_INFO("ABI v114[1]");
    REQUIRE(a.v114.u64[1] == b.v114.u64[1]);
    UNSCOPED_INFO("ABI v115[0]");
    REQUIRE(a.v115.u64[0] == b.v115.u64[0]);
    UNSCOPED_INFO("ABI v115[1]");
    REQUIRE(a.v115.u64[1] == b.v115.u64[1]);
    UNSCOPED_INFO("ABI v116[0]");
    REQUIRE(a.v116.u64[0] == b.v116.u64[0]);
    UNSCOPED_INFO("ABI v116[1]");
    REQUIRE(a.v116.u64[1] == b.v116.u64[1]);
    UNSCOPED_INFO("ABI v117[0]");
    REQUIRE(a.v117.u64[0] == b.v117.u64[0]);
    UNSCOPED_INFO("ABI v117[1]");
    REQUIRE(a.v117.u64[1] == b.v117.u64[1]);
    UNSCOPED_INFO("ABI v118[0]");
    REQUIRE(a.v118.u64[0] == b.v118.u64[0]);
    UNSCOPED_INFO("ABI v118[1]");
    REQUIRE(a.v118.u64[1] == b.v118.u64[1]);
    UNSCOPED_INFO("ABI v119[0]");
    REQUIRE(a.v119.u64[0] == b.v119.u64[0]);
    UNSCOPED_INFO("ABI v119[1]");
    REQUIRE(a.v119.u64[1] == b.v119.u64[1]);
    UNSCOPED_INFO("ABI v120[0]");
    REQUIRE(a.v120.u64[0] == b.v120.u64[0]);
    UNSCOPED_INFO("ABI v120[1]");
    REQUIRE(a.v120.u64[1] == b.v120.u64[1]);
    UNSCOPED_INFO("ABI v121[0]");
    REQUIRE(a.v121.u64[0] == b.v121.u64[0]);
    UNSCOPED_INFO("ABI v121[1]");
    REQUIRE(a.v121.u64[1] == b.v121.u64[1]);
    UNSCOPED_INFO("ABI v122[0]");
    REQUIRE(a.v122.u64[0] == b.v122.u64[0]);
    UNSCOPED_INFO("ABI v122[1]");
    REQUIRE(a.v122.u64[1] == b.v122.u64[1]);
    UNSCOPED_INFO("ABI v123[0]");
    REQUIRE(a.v123.u64[0] == b.v123.u64[0]);
    UNSCOPED_INFO("ABI v123[1]");
    REQUIRE(a.v123.u64[1] == b.v123.u64[1]);
    UNSCOPED_INFO("ABI v124[0]");
    REQUIRE(a.v124.u64[0] == b.v124.u64[0]);
    UNSCOPED_INFO("ABI v124[1]");
    REQUIRE(a.v124.u64[1] == b.v124.u64[1]);
    UNSCOPED_INFO("ABI v125[0]");
    REQUIRE(a.v125.u64[0] == b.v125.u64[0]);
    UNSCOPED_INFO("ABI v125[1]");
    REQUIRE(a.v125.u64[1] == b.v125.u64[1]);
    UNSCOPED_INFO("ABI v126[0]");
    REQUIRE(a.v126.u64[0] == b.v126.u64[0]);
    UNSCOPED_INFO("ABI v126[1]");
    REQUIRE(a.v126.u64[1] == b.v126.u64[1]);
    UNSCOPED_INFO("ABI v127[0]");
    REQUIRE(a.v127.u64[0] == b.v127.u64[0]);
    UNSCOPED_INFO("ABI v127[1]");
    REQUIRE(a.v127.u64[1] == b.v127.u64[1]);
    UNSCOPED_INFO("ABI cr0.lt");
    REQUIRE(a.cr0.lt == b.cr0.lt);
    UNSCOPED_INFO("ABI cr0.gt");
    REQUIRE(a.cr0.gt == b.cr0.gt);
    UNSCOPED_INFO("ABI cr0.eq");
    REQUIRE(a.cr0.eq == b.cr0.eq);
    UNSCOPED_INFO("ABI cr0.so");
    REQUIRE(a.cr0.so == b.cr0.so);
    UNSCOPED_INFO("ABI cr1.lt");
    REQUIRE(a.cr1.lt == b.cr1.lt);
    UNSCOPED_INFO("ABI cr1.gt");
    REQUIRE(a.cr1.gt == b.cr1.gt);
    UNSCOPED_INFO("ABI cr1.eq");
    REQUIRE(a.cr1.eq == b.cr1.eq);
    UNSCOPED_INFO("ABI cr1.so");
    REQUIRE(a.cr1.so == b.cr1.so);
    UNSCOPED_INFO("ABI cr2.lt");
    REQUIRE(a.cr2.lt == b.cr2.lt);
    UNSCOPED_INFO("ABI cr2.gt");
    REQUIRE(a.cr2.gt == b.cr2.gt);
    UNSCOPED_INFO("ABI cr2.eq");
    REQUIRE(a.cr2.eq == b.cr2.eq);
    UNSCOPED_INFO("ABI cr2.so");
    REQUIRE(a.cr2.so == b.cr2.so);
    UNSCOPED_INFO("ABI cr3.lt");
    REQUIRE(a.cr3.lt == b.cr3.lt);
    UNSCOPED_INFO("ABI cr3.gt");
    REQUIRE(a.cr3.gt == b.cr3.gt);
    UNSCOPED_INFO("ABI cr3.eq");
    REQUIRE(a.cr3.eq == b.cr3.eq);
    UNSCOPED_INFO("ABI cr3.so");
    REQUIRE(a.cr3.so == b.cr3.so);
    UNSCOPED_INFO("ABI cr4.lt");
    REQUIRE(a.cr4.lt == b.cr4.lt);
    UNSCOPED_INFO("ABI cr4.gt");
    REQUIRE(a.cr4.gt == b.cr4.gt);
    UNSCOPED_INFO("ABI cr4.eq");
    REQUIRE(a.cr4.eq == b.cr4.eq);
    UNSCOPED_INFO("ABI cr4.so");
    REQUIRE(a.cr4.so == b.cr4.so);
    UNSCOPED_INFO("ABI cr5.lt");
    REQUIRE(a.cr5.lt == b.cr5.lt);
    UNSCOPED_INFO("ABI cr5.gt");
    REQUIRE(a.cr5.gt == b.cr5.gt);
    UNSCOPED_INFO("ABI cr5.eq");
    REQUIRE(a.cr5.eq == b.cr5.eq);
    UNSCOPED_INFO("ABI cr5.so");
    REQUIRE(a.cr5.so == b.cr5.so);
    UNSCOPED_INFO("ABI cr6.lt");
    REQUIRE(a.cr6.lt == b.cr6.lt);
    UNSCOPED_INFO("ABI cr6.gt");
    REQUIRE(a.cr6.gt == b.cr6.gt);
    UNSCOPED_INFO("ABI cr6.eq");
    REQUIRE(a.cr6.eq == b.cr6.eq);
    UNSCOPED_INFO("ABI cr6.so");
    REQUIRE(a.cr6.so == b.cr6.so);
    UNSCOPED_INFO("ABI cr7.lt");
    REQUIRE(a.cr7.lt == b.cr7.lt);
    UNSCOPED_INFO("ABI cr7.gt");
    REQUIRE(a.cr7.gt == b.cr7.gt);
    UNSCOPED_INFO("ABI cr7.eq");
    REQUIRE(a.cr7.eq == b.cr7.eq);
    UNSCOPED_INFO("ABI cr7.so");
    REQUIRE(a.cr7.so == b.cr7.so);
    UNSCOPED_INFO("ABI LR/CTR");
    REQUIRE((a.lr == b.lr && a.ctr.u64 == b.ctr.u64));
    UNSCOPED_INFO("ABI XER");
    REQUIRE((a.xer.so == b.xer.so && a.xer.ov == b.xer.ov && a.xer.ca == b.xer.ca));
    UNSCOPED_INFO("ABI reservation/MSR");
    REQUIRE((a.reserved.u64 == b.reserved.u64 && a.msr == b.msr));
    UNSCOPED_INFO("ABI FP/vector/dispatch state");
    REQUIRE((a.fpscr.csr == b.fpscr.csr && a.vscr_sat == b.vscr_sat && a.last_indirect_target == b.last_indirect_target));
}

// Literal six lwz, addi and seven stw semantics of 8269B118..8269B150.
// This reference uses neither the production derivation nor its offset table.
void reference(Memory& memory, PPCContext& c)
{
    const auto g = c.r3.u32;
    c.r11.u64    = memory.load(g + 13412);
    c.r10.u64    = memory.load(g + 13408);
    c.r9.u64     = memory.load(g + 13416);
    c.r8.u64     = c.r11.u64 - 160ULL;
    c.r7.u64     = memory.load(g + 13420);
    c.r6.u64     = memory.load(g + 13424);
    c.r5.u64     = memory.load(g + 13428);
    memory.store(g + 48, c.r10.u32);
    memory.store(g + 52, c.r11.u32);
    memory.store(g + 14928, c.r9.u32);
    memory.store(g + 14924, c.r7.u32);
    memory.store(g + 14916, c.r6.u32);
    memory.store(g + 14920, c.r5.u32);
    memory.store(g + 56, c.r8.u32);
}

void testExactReplay()
{
    for (const auto s1 : { saved[1], 0U, 159U, 160U, 161U, UINT32_MAX })
    {
        Memory actual;
        actual.put(G + 0x3464, s1);
        Memory expected = actual;
        auto   a = poisoned(), b = a;
        reference(expected, b);
        UNSCOPED_INFO("exact replay admits");
        REQUIRE(ConsumeTilingRestoreWith(consume, actual.access(), a));
        UNSCOPED_INFO("full byte map including saves and canaries");
        REQUIRE(actual.bytes == expected.bytes);
        sameContext(a, b);
        const std::vector<Event> trace{
            { 'R', G + 0x3464, s1 }, { 'R', G + 0x3460, saved[0] }, { 'R', G + 0x3468, saved[2] }, { 'R', G + 0x346C, saved[3] }, { 'R', G + 0x3470, saved[4] }, { 'R', G + 0x3474, saved[5] }, { 'W', G + 0x30, saved[0] }, { 'W', G + 0x34, s1 }, { 'W', G + 0x3A50, saved[2] }, { 'W', G + 0x3A4C, saved[3] }, { 'W', G + 0x3A44, saved[4] }, { 'W', G + 0x3A48, saved[5] }, { 'W', G + 0x38, s1 - 160U }
        };
        UNSCOPED_INFO("all six reads precede exact ordered seven-store trace");
        REQUIRE((actual.events == trace && expected.events == trace));
        UNSCOPED_INFO("r8 is modulo64");
        REQUIRE(a.r8.u64 == std::uint64_t{ s1 } - 160ULL);
        UNSCOPED_INFO("limit is modulo32");
        REQUIRE(actual.get(G + 0x38) == s1 - 160U);
        if (!s1)
        {
            UNSCOPED_INFO("r8 underflow preserves high half");
            REQUIRE(a.r8.u64 == 0xFFFFFFFFFFFFFF60ULL);
        }
    }
}

void refused(Memory& m, TilingRestoreState state, TilingRestoreFallback reason, PPCContext ctx = poisoned())
{
    (void)TakeTilingRestoreCounts();
    const auto before   = m.bytes;
    const auto original = ctx;
    UNSCOPED_INFO("preflight refuses");
    REQUIRE(!ConsumeTilingRestoreWith(state, m.access(), ctx));
    sameContext(ctx, original);
    UNSCOPED_INFO("refusal has zero writes and unchanged bytes");
    REQUIRE((m.bytes == before && m.stores == 0));
    const auto counts = TakeTilingRestoreCounts();
    for (std::size_t i = 0; i < counts.fallback.size(); ++i)
    {
        UNSCOPED_INFO("exact refusal reason");
        REQUIRE(counts.fallback[i] == (i == static_cast<std::size_t>(reason) ? 1U : 0U));
    }
}

void testDisabledAndPreflight()
{
    (void)TakeTilingRestoreCounts();
    int                 calls = 0;
    TilingRestoreMemory poison{
        [&](auto, auto&) -> bool
        {
            ++calls;
            throw 1;
        },
        [&](auto, auto) -> bool
        {
            ++calls;
            throw 1;
        },
        [&](auto, auto) -> std::optional<std::uint64_t>
        {
            ++calls;
            throw 1;
        },
        [&](auto) -> bool
        {
            ++calls;
            throw 1;
        }
    };
    auto       ctx    = poisoned();
    const auto before = ctx;
    UNSCOPED_INFO("off returns");
    REQUIRE(!ConsumeTilingRestoreWith({}, poison, ctx));
    {
        const TilingRestoreExitScope scope(ctx, poison, false);
    }
    sameContext(ctx, before);
    UNSCOPED_INFO("off has zero reads writes counts allocations or locks");
    REQUIRE((calls == 0 && FormatTilingRestoreCounts(TakeTilingRestoreCounts()) == FormatTilingRestoreCounts({})));
    for (const auto reason : { TilingRestoreFallback::Stubs, TilingRestoreFallback::Device, TilingRestoreFallback::Diagnostics, TilingRestoreFallback::ExitCheck })
    {
        Memory m;
        auto   state = consume;
        if (reason == TilingRestoreFallback::Stubs)
            state.stubs = false;
        if (reason == TilingRestoreFallback::Device)
            state.device = false;
        if (reason == TilingRestoreFallback::Diagnostics)
            state.diagnostics = true;
        if (reason == TilingRestoreFallback::ExitCheck)
            state.checking = true;
        refused(m, state, reason);
        UNSCOPED_INFO("early refusal reads nothing");
        REQUIRE(m.events.empty());
    }
    for (std::uint32_t i = 0; i < 6; ++i)
    {
        Memory m;
        m.denyRead = G + 0x3460 + 4 * i;
        refused(m, consume, TilingRestoreFallback::Unreadable);
    }
    for (const auto offset : { 0x30U, 0x34U, 0x3A50U, 0x3A4CU, 0x3A44U, 0x3A48U, 0x38U })
    {
        Memory m;
        m.denyWrite = G + offset;
        refused(m, consume, TilingRestoreFallback::Unwritable);
    }
    for (const auto g : { 0U, 0xFFFFU, G + 1, 0xFFFFC5B0U })
    {
        Memory m;
        auto   bad = poisoned();
        bad.r3.u64 = g;
        refused(m, consume, TilingRestoreFallback::Mapping, bad);
        UNSCOPED_INFO("invalid device refuses before operand access");
        REQUIRE((m.identityCalls == 0 && m.events.empty()));
        auto unvalidated   = consume;
        unvalidated.device = false;
        refused(m, unvalidated, TilingRestoreFallback::Mapping, bad);
        UNSCOPED_INFO("invalid address is mapping even without validated device identity");
        REQUIRE((m.identityCalls == 0 && m.events.empty()));
    }
}

void testCanonicalIdentities()
{
    for (const auto offset : { 0x3460U, 0x3464U, 0x3468U, 0x346CU, 0x3470U, 0x3474U, 0x30U, 0x34U, 0x3A50U, 0x3A4CU, 0x3A44U, 0x3A48U, 0x38U })
    {
        Memory unknown;
        unknown.unknown = G + offset;
        refused(unknown, consume, TilingRestoreFallback::Mapping);
    }
    for (const bool physical : { false, true })
    {
        Memory valid;
        valid.physical = physical;
        auto ctx       = poisoned();
        UNSCOPED_INFO("normal virtual and physical identities admit");
        REQUIRE(ConsumeTilingRestoreWith(consume, valid.access(), ctx));
        for (const auto source : { G + 0x3464, G + 0x34 })
        {
            Memory aliased;
            aliased.physical      = physical;
            aliased.alias         = G + 0x38;
            aliased.aliasIdentity = (physical ? std::uint64_t{ source & 0x1FFFFFFF } : (std::uint64_t{ 1 } << 32) + source) + 2;
            refused(aliased, consume, TilingRestoreFallback::Alias);
        }
    }
}

void testChecker()
{
    REXCVAR_SET(native_consume_tiling_restore, true);
    REXCVAR_SET(native_tiling_restore_exit_check, true);
    Memory m, expected = m;
    auto   ctx = poisoned(), baseline = ctx;
    reference(expected, baseline);
    (void)TakeTilingRestoreCounts();
    int originals = 0;
    NoteTilingRestoreEnter();
    UNSCOPED_INFO("checker priority forces Original");
    REQUIRE(!ConsumeTilingRestoreWith({ true, true, true, true, false }, m.access(), ctx));
    {
        const auto                   entryBytes   = m.bytes;
        const auto                   entryContext = ctx;
        const TilingRestoreExitScope check(ctx, m.access(), true);
        sameContext(ctx, entryContext);
        UNSCOPED_INFO("checker construction leaves live guest bytes and registers unchanged");
        REQUIRE((m.bytes == entryBytes && m.stores == 0));
        NoteTilingRestoreOriginal();
        ++originals;
        reference(m, ctx);
    }
    UNSCOPED_INFO("checker has one Original zero replay stores and baseline bytes");
    REQUIRE((originals == 1 && m.stores == 7 && m.bytes == expected.bytes));
    sameContext(ctx, baseline);
    auto counts = TakeTilingRestoreCounts();
    UNSCOPED_INFO("checker dispatch reconciliation");
    REQUIRE((counts.enter == 1 && counts.original == 1 && counts.consumed == 0 && counts.exitChecked == 1 &&
             counts.fallback[static_cast<std::size_t>(TilingRestoreFallback::ExitCheck)] == 1));
    for (const auto offset : { 0x30U, 0x34U, 0x3A50U, 0x3A4CU, 0x3A44U, 0x3A48U, 0x38U })
    {
        Memory bad;
        auto   c = poisoned();
        {
            const TilingRestoreExitScope check(c, bad.access(), true);
            reference(bad, c);
            bad.put(G + offset, bad.get(G + offset) ^ 1);
        }
        counts = TakeTilingRestoreCounts();
        UNSCOPED_INFO("each output corruption is a mismatch never success");
        REQUIRE((counts.exitMismatch == 1 && counts.exitChecked == 0));
        (void)ReportTilingRestore();
    }
    const std::vector<std::function<void(PPCContext&)>> corrupt{
        [](PPCContext& c)
        {
            c.r0.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r1.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r2.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r3.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r4.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r5.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r6.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r7.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r8.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r9.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r10.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r11.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r12.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r13.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r14.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r15.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r16.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r17.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r18.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r19.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r20.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r21.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r22.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r23.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r24.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r25.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r26.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r27.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r28.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r29.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r30.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.r31.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f0.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f1.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f2.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f3.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f4.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f5.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f6.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f7.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f8.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f9.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f10.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f11.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f12.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f13.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f14.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f15.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f16.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f17.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f18.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f19.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f20.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f21.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f22.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f23.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f24.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f25.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f26.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f27.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f28.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f29.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f30.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.f31.u64 ^= 0x100000001ULL;
        },
        [](PPCContext& c)
        {
            c.v0.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v0.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v1.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v1.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v2.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v2.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v3.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v3.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v4.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v4.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v5.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v5.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v6.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v6.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v7.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v7.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v8.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v8.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v9.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v9.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v10.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v10.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v11.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v11.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v12.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v12.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v13.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v13.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v14.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v14.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v15.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v15.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v16.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v16.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v17.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v17.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v18.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v18.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v19.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v19.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v20.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v20.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v21.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v21.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v22.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v22.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v23.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v23.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v24.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v24.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v25.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v25.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v26.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v26.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v27.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v27.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v28.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v28.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v29.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v29.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v30.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v30.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v31.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v31.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v32.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v32.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v33.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v33.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v34.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v34.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v35.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v35.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v36.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v36.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v37.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v37.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v38.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v38.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v39.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v39.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v40.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v40.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v41.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v41.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v42.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v42.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v43.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v43.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v44.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v44.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v45.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v45.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v46.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v46.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v47.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v47.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v48.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v48.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v49.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v49.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v50.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v50.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v51.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v51.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v52.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v52.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v53.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v53.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v54.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v54.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v55.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v55.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v56.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v56.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v57.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v57.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v58.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v58.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v59.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v59.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v60.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v60.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v61.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v61.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v62.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v62.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v63.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v63.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v64.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v64.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v65.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v65.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v66.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v66.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v67.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v67.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v68.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v68.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v69.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v69.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v70.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v70.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v71.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v71.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v72.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v72.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v73.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v73.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v74.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v74.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v75.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v75.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v76.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v76.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v77.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v77.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v78.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v78.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v79.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v79.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v80.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v80.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v81.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v81.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v82.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v82.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v83.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v83.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v84.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v84.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v85.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v85.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v86.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v86.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v87.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v87.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v88.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v88.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v89.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v89.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v90.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v90.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v91.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v91.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v92.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v92.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v93.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v93.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v94.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v94.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v95.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v95.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v96.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v96.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v97.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v97.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v98.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v98.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v99.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v99.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v100.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v100.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v101.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v101.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v102.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v102.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v103.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v103.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v104.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v104.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v105.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v105.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v106.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v106.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v107.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v107.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v108.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v108.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v109.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v109.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v110.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v110.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v111.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v111.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v112.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v112.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v113.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v113.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v114.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v114.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v115.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v115.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v116.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v116.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v117.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v117.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v118.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v118.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v119.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v119.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v120.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v120.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v121.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v121.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v122.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v122.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v123.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v123.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v124.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v124.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v125.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v125.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v126.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v126.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v127.u64[0] ^= 1;
        },
        [](PPCContext& c)
        {
            c.v127.u64[1] ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr0.lt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr0.gt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr0.eq ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr0.so ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr1.lt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr1.gt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr1.eq ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr1.so ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr2.lt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr2.gt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr2.eq ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr2.so ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr3.lt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr3.gt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr3.eq ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr3.so ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr4.lt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr4.gt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr4.eq ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr4.so ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr5.lt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr5.gt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr5.eq ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr5.so ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr6.lt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr6.gt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr6.eq ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr6.so ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr7.lt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr7.gt ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr7.eq ^= 1;
        },
        [](PPCContext& c)
        {
            c.cr7.so ^= 1;
        },
        [](PPCContext& c)
        {
            c.lr ^= 1;
        },
        [](PPCContext& c)
        {
            c.ctr.u64 ^= 1;
        },
        [](PPCContext& c)
        {
            c.xer.so ^= 1;
        },
        [](PPCContext& c)
        {
            c.xer.ov ^= 1;
        },
        [](PPCContext& c)
        {
            c.xer.ca ^= 1;
        },
        [](PPCContext& c)
        {
            c.reserved.u64 ^= 1;
        },
        [](PPCContext& c)
        {
            c.msr ^= 1;
        },
        [](PPCContext& c)
        {
            c.fpscr.csr ^= 1;
        },
        [](PPCContext& c)
        {
            c.vscr_sat ^= 1;
        },
        [](PPCContext& c)
        {
            c.last_indirect_target ^= 1;
        }
    };
    for (const auto& change : corrupt)
    {
        Memory bad;
        auto   c = poisoned();
        {
            const TilingRestoreExitScope check(c, bad.access(), true);
            reference(bad, c);
            change(c);
        }
        counts = TakeTilingRestoreCounts();
        UNSCOPED_INFO("each semantic ABI corruption detected");
        REQUIRE((counts.exitAbiMismatch == 1 && counts.exitChecked == 0));
        (void)ReportTilingRestore();
    }
    REXCVAR_SET(native_consume_tiling_restore, false);
    REXCVAR_SET(native_tiling_restore_exit_check, false);
}

void testReportingAndQualification()
{
    (void)TakeTilingRestoreCounts();
    Memory     m;
    auto       ctx   = poisoned();
    const auto total = TilingRestoreLifetimeCounts();
    NoteTilingRestoreEnter();
    {
        const TilingRestoreExitScope scope(ctx, m.access(), true);
        NoteTilingRestoreOriginal();
        const auto first = TakeTilingRestoreCounts();
        UNSCOPED_INFO("drain retains active scope");
        REQUIRE((first.enter == 1 && first.original == 1 && first.exitChecked == 0));
        reference(m, ctx);
    }
    auto counts = TakeTilingRestoreCounts();
    UNSCOPED_INFO("scope closes after drain");
    REQUIRE(counts.exitChecked == 1);
    const auto after = TilingRestoreLifetimeCounts();
    UNSCOPED_INFO("lifetime survives drain");
    REQUIRE((after.enter == total.enter + 1 && after.original == total.original + 1 && after.exitChecked == total.exitChecked + 1));
    {
        Memory                       shared;
        auto                         a = poisoned(), b = a;
        const TilingRestoreExitScope outer(a, shared.access(), true);
        (void)TakeTilingRestoreCounts();
        {
            const TilingRestoreExitScope inner(b, shared.access(), true);
            reference(shared, b);
        }
        reference(shared, a);
    }
    UNSCOPED_INFO("drain retains overlap pairing for later entries");
    REQUIRE(TakeTilingRestoreCounts().exitIndeterminate == 2);

    for (int kind = 0; kind < 5; ++kind)
    {
        Memory bad;
        auto   c = poisoned();
        if (kind == 0)
            bad.denyRead = G + 0x3474;
        if (kind == 1)
            bad.unknown = G + 0x34;
        if (kind == 2)
        {
            bad.alias         = G + 0x38;
            bad.aliasIdentity = (std::uint64_t{ 1 } << 32) + G + 0x34;
        }
        {
            const TilingRestoreExitScope scope(c, bad.access(), true);
            reference(bad, c);
            if (kind == 3)
                bad.put(G + 0x3474, 0);
            if (kind == 4)
                bad.denyRead = G + 0x38;
        }
        counts = TakeTilingRestoreCounts();
        UNSCOPED_INFO("qualified comparisons never match");
        REQUIRE((counts.exitChecked == 0 && counts.exitMismatch == 0));
        UNSCOPED_INFO("unreadable alias mapping and changed saves qualified");
        REQUIRE(((kind == 0 || kind == 4) ? counts.exitUnreadable == 1 : counts.exitIndeterminate == 1));
    }
    {
        Memory                       shared;
        auto                         a = poisoned(), b = a;
        const TilingRestoreExitScope outer(a, shared.access(), true);
        {
            const TilingRestoreExitScope inner(b, shared.access(), true);
            reference(shared, b);
        }
        reference(shared, a);
    }
    UNSCOPED_INFO("overlapping observed calls are indeterminate");
    REQUIRE(TakeTilingRestoreCounts().exitIndeterminate == 2);
    {
        Memory                       shared;
        auto                         a = poisoned(), b = a;
        const TilingRestoreExitScope scope(a, shared.access(), true);
        UNSCOPED_INFO("competing replay admits");
        REQUIRE(ConsumeTilingRestoreWith(consume, shared.access(), b));
        reference(shared, a);
    }
    counts = TakeTilingRestoreCounts();
    UNSCOPED_INFO("observed replay writer qualifies endpoint");
    REQUIRE((counts.exitIndeterminate == 1 && counts.exitChecked == 0));
    {
        Memory     shared;
        auto       a = poisoned(), b = a;
        auto       access = shared.access();
        const auto writer = access.write;
        bool       nested = false;
        access.write      = [&](const TilingRestoreWrite& write)
        {
            const bool result = writer(write);
            if (!nested)
            {
                nested = true;
                const TilingRestoreExitScope scope(b, shared.access(), true);
                reference(shared, b);
            }
            return result;
        };
        UNSCOPED_INFO("replay with nested observer completes");
        REQUIRE(ConsumeTilingRestoreWith(consume, access, a));
    }
    counts = TakeTilingRestoreCounts();
    UNSCOPED_INFO("observer entering during replay sees competing writer");
    REQUIRE((counts.exitIndeterminate == 1 && counts.exitChecked == 0));
    {
        Memory                       pending;
        auto                         c = poisoned();
        const TilingRestoreExitScope scope(c, pending.access(), true);
        UNSCOPED_INFO("shutdown reports active scope");
        REQUIRE(TakeTilingRestoreCounts(true).exitUnmatched == 1);
        UNSCOPED_INFO("shutdown unmatched counted once");
        REQUIRE(TakeTilingRestoreCounts(true).exitUnmatched == 0);
        reference(pending, c);
    }
    (void)TakeTilingRestoreCounts();
    try
    {
        Memory                       throwing;
        auto                         c = poisoned();
        const TilingRestoreExitScope scope(c, throwing.access(), true);
        throw 7;
    }
    catch (int)
    {
    }
    counts = TakeTilingRestoreCounts();
    UNSCOPED_INFO("unwound Original is unmatched");
    REQUIRE((counts.exitUnmatched == 1 && counts.exitChecked == 0));

    Memory                                               bounded;
    std::array<PPCContext, 65>                           contexts{};
    std::vector<std::unique_ptr<TilingRestoreExitScope>> scopes;
    for (auto& c : contexts)
    {
        c = poisoned();
        scopes.push_back(std::make_unique<TilingRestoreExitScope>(c, bounded.access(), true));
    }
    counts = TakeTilingRestoreCounts();
    UNSCOPED_INFO("lost active record is explicit");
    REQUIRE(counts.eventDrop == 1);
    scopes.clear();
    counts = TakeTilingRestoreCounts();
    UNSCOPED_INFO("overflow never invents matches");
    REQUIRE((counts.exitIndeterminate == 65 && counts.exitChecked == 0));
    REXCVAR_SET(native_tiling_restore_exit_check, true);
    for (int i = 0; i < 33; ++i)
    {
        Memory                       bad;
        auto                         c = poisoned();
        const TilingRestoreExitScope scope(c, bad.access(), true);
        reference(bad, c);
        bad.put(G + 0x38, 0);
    }
    const auto lost = TakeTilingRestoreCounts();
    UNSCOPED_INFO("bounded mismatch records report drops without hiding mismatches");
    REQUIRE((lost.exitMismatch == 33 && lost.eventDrop == 1 && lost.exitChecked == 0));
    (void)ReportTilingRestore();
    REXCVAR_SET(native_tiling_restore_exit_check, false);
    const auto formatted = FormatTilingRestoreCounts(counts);
    UNSCOPED_INFO("all counter names formatted");
    REQUIRE((formatted.find("tr_exit_indeterminate=65") != std::string::npos && formatted.find("tr_replay_fault=0") != std::string::npos));
}

Memory* faultMemory    = nullptr;
int     faultOriginals = 0;

// Runs in a child of rerevved_tests (`tiling-restore.fault-child`): a replay
// fault past the first store must reach the named terminate handler.
int faultChild()
{
    Memory m;
    faultMemory  = &m;
    m.failWriter = 1;
    std::set_terminate([]
                       {
                           const bool namedFault = TilingRestoreLifetimeCounts().replayFault == 1;
                           std::_Exit(namedFault && faultMemory->stores == 1 && faultOriginals == 0 ? 73 : 74);
                       });
    auto ctx = poisoned();
    if (!ConsumeTilingRestoreWith(consume, m.access(), ctx))
        ++faultOriginals;
    std::_Exit(0);
}

void testCommitFault()
{
    const int status = rerevved::tests::RunHelperChild("tiling-restore.fault-child");
    UNSCOPED_INFO("named fatal replay fault after one store with zero Original");
    REQUIRE(status == 73);
}

using CaseTable = std::array<std::pair<const char*, void (*)()>, 6>;

const CaseTable kCases{
    CaseTable::value_type{ "testExactReplay", testExactReplay },
    CaseTable::value_type{ "testDisabledAndPreflight", testDisabledAndPreflight },
    CaseTable::value_type{ "testCanonicalIdentities", testCanonicalIdentities },
    CaseTable::value_type{ "testChecker", testChecker },
    CaseTable::value_type{ "testReportingAndQualification", testReportingAndQualification },
    CaseTable::value_type{ "testCommitFault", testCommitFault }
};

// Runs every table entry, or only the entry named `selected`.
unsigned runCases(std::string_view selected)
{
    unsigned ran = 0;
    for (const auto& [name, run] : kCases)
        if (selected.empty() || selected == name)
        {
            INFO("case " << name);
            run();
            ++ran;
        }
    return ran;
}

} // namespace

TEST_CASE("tiling_restore", "[ctest][tiling-restore][timeout-60]")
{
    runCases({});
}

// Runs one named case:
//   tiling-restore.case <testName>
TEST_CASE("tiling_restore_selected", "[.][manual][tiling-restore]")
{
    const auto& args = rerevved::tests::CurrentManualArgv();
    REQUIRE(args.argc() == 1);
    UNSCOPED_INFO("an unknown case name");
    REQUIRE(runCases(args[1]) == 1);
}

REREVVED_MANUAL_HELPER(tilingRestoreCase, "tiling-restore.case", "tiling_restore_selected")

// The fatal child of testCommitFault: exits 73 on the named replay fault.
REREVVED_HELPER_COMMAND(tilingRestoreFaultChild, "tiling-restore.fault-child")
{
    (void)args;
    return faultChild();
}
