// resolve_effect: expected masks follow generated stores and shifts
// (g167:14299-14323; 14455-14523; 14527-14607; 14935-14976).

#include "developer_checks.hpp"
#include "gpu/guest_d3d/resolve_derive.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace rerevved::gpu::guest_d3d;

namespace
{

constexpr std::uint32_t kG = 0x40010000, kHeader = 0x40020000, kSurface = 0x40030000, kDepthSurface = 0x40031000;

using Memory = std::unordered_map<std::uint32_t, std::uint32_t>;

// The shadow word the slot bit tests: G+0x2884 + 4 * (slot == 0 ? 0 : slot + 1).
std::uint32_t shadowAddress(std::uint32_t slot)
{
    return kG + 0x2884 + 4 * (slot == 0 ? 0 : slot + 1);
}

constexpr std::uint32_t kShadowHit = 0x10000, kShadowMiss = 0x30000;

// The clear blocks' inputs: the caller's frame holds the stencil value at +92 and
// the parameter block pointer at +100 (g167:13519, 13539). The shadow words
// G+0x2A28..0x2A34 hold kClearEntry + i at the entry.
constexpr std::uint32_t kSp = 0x40060000, kParams = 0x40061000, kColorVector = 0x40062000, kClearEntry = 0xC1EA0000;

std::uint32_t bitsOf(float value)
{
    return std::bit_cast<std::uint32_t>(value);
}

void setByte(Memory& memory, std::uint32_t address, std::uint32_t value)
{
    auto&      word  = memory[address & ~3U];
    const auto shift = 8 * (3 - (address & 3));
    word             = (word & ~(0xFFU << shift)) | (value << shift);
}

void setHalf(Memory& memory, std::uint32_t address, std::uint32_t value)
{
    auto&      word  = memory[address & ~3U];
    const auto shift = (address & 2) ? 0 : 16;
    word             = (word & ~(0xFFFFU << shift)) | (value << shift);
}

void setColor(Memory& memory, std::uint32_t address, const std::array<float, 4>& color)
{
    for (std::uint32_t i = 0; i < 4; ++i)
        memory[address + 4 * i] = bitsOf(color[i]);
}

// The image constants and tables the blocks read: the depth scales 2047 and
// 16777215, 1.0, the default clear color, and the channel widths by format
// (rows 0 and 2: 8 and 10 bits of color, 8 and 2 of alpha) and the format map (all zero).
void clearInto(Memory& memory)
{
    memory[kSp + 92]             = 0;
    memory[kSp + 100]            = 0;
    memory[kG + 0x2940]          = 3;
    memory[kResolveClearScale11] = bitsOf(2047.0F);
    memory[kResolveClearScale24] = bitsOf(16777215.0F);
    memory[kResolveClearOne]     = bitsOf(1.0F);
    setColor(memory, kResolveClearDefault, { 0.0F, 1.0F, 0.5F, 0.25F });
    setColor(memory, kColorVector, { 1.0F, 0.5F, 0.25F, 0.75F });
    for (std::uint32_t word = 0; word < 4; ++word)
        memory[kResolveClearFormatMap + 4 * word] = 0;
    const std::uint32_t alpha[4] = { 8, 0, 2, 0 }, color[4] = { 8, 0, 10, 0 };
    for (std::uint32_t row = 0; row < 4; ++row)
    {
        setByte(memory, kResolveClearAlphaBits + row, alpha[row]);
        setByte(memory, kResolveClearColorBits + row, color[row]);
    }
}

struct Case
{
    const char*   name;
    std::uint32_t flags = 0;
    std::uint32_t d0 = 0x0A000000, d2 = 0x0059E4FF, d5 = 0;
    bool          shadowHit   = false; // the tested slot's shadow word hits; every other shadow word hits too
    bool          shadowOnly  = false; // only the tested slot's shadow word is set, to hit
    bool          large       = false;
    std::uint32_t pitchHeight = 0;
    std::uint64_t slotBit     = 0; // the one bit added to G+0x10 before the allocator
};

constexpr std::uint64_t kCommon0 = 0x0000138000000000ULL | 0xE0, kCommon1 = 0x0000000800000000ULL, kCommon2 = 0x80000;
constexpr std::uint64_t kAfter0 = 0x98908, kAfter1 = 0x80000000;

// A ring segment with room for the worked calls and a side buffer pool whose
// fixed buffer ends at 0x00204000: the 24 bytes come from (top - 24) & ~3 and
// lower both limits by 24. The render target count is 4.
constexpr std::uint32_t kCursor = 0x00200000, kLimit38 = 0x00203F00, kLimit34 = kLimit38 + 160;
constexpr std::uint32_t kPoolObject = 0x40040000, kPoolBase = 0x00203000, kPoolSize = 0x1000;
constexpr std::uint32_t kPoolUsed = 0x100, kPoolThreshold = 0x1000, kTargets = 4;
constexpr std::uint32_t kBuffer = 0x00203FE8, kSideBuffer = 24;

// Null-pool allocator uses G+0x3A3C, G+0x3A4C and G+0x3A50; zero G+0x3A44
// starts at G+0x34+60.
constexpr std::uint32_t kSideEnd = 0x00205000, kWrapPos = 0x00204200, kWrapLimit = 0x00204800, kSegment = 5, kProgressBlock = 0x40050000;
constexpr std::uint32_t kNullEnd = kLimit34 + 60 + kSideBuffer;

// The ring and pool words the exit state is derived from.
void ringInto(Memory& memory)
{
    memory[kG + 0x30]         = kCursor;
    memory[kG + 0x34]         = kLimit34;
    memory[kG + 0x38]         = kLimit38;
    memory[kG + 0x3A44]       = 0;
    memory[kG + 0x3A4C]       = 0;
    memory[kG + 0x3A54]       = kPoolUsed;
    memory[kG + 0x3A58]       = kPoolThreshold;
    memory[kG + 0x31CC]       = kTargets;
    memory[kG + 0x31AC]       = 0;
    memory[kG + 0x33B0]       = 0;
    memory[kG + 0x34CC]       = kPoolObject;
    memory[kPoolObject + 152] = kPoolBase;
    memory[kPoolObject + 156] = kPoolSize;
    for (std::uint32_t i = 0; i < kResolveClearWordCount; ++i)
        memory[kG + kResolveClearShadow + 4 * i] = kClearEntry + i;
}

// The same state as a watched entry.
void ringInto(ResolveWatch& watch)
{
    watch.cursor        = kCursor;
    watch.limit34       = kLimit34;
    watch.limit38       = kLimit38;
    watch.poolPos       = 0;
    watch.poolUsed      = kPoolUsed;
    watch.poolThreshold = kPoolThreshold;
    watch.targetCount   = kTargets;
    watch.poolObject    = kPoolObject;
    watch.poolBase      = kPoolBase;
    watch.poolSize      = kPoolSize;
    watch.tilingByte    = 0;
    watch.predicate     = 0;
    for (std::uint32_t i = 0; i < kResolveClearWordCount; ++i)
        watch.clear[i] = kClearEntry + i;
}

// The same ring with a null pool object and the allocator words around it.
void nullPoolInto(Memory& memory)
{
    memory[kG + 0x34CC] = 0;
    memory.erase(kPoolObject + 152);
    memory.erase(kPoolObject + 156);
    memory[kG + 0x3A3C]        = kSideEnd;
    memory[kG + 0x3A48]        = kSegment;
    memory[kG + 0x3A4C]        = kWrapPos;
    memory[kG + 0x3A50]        = kWrapLimit;
    memory[kG + 0x33B4]        = 0;
    memory[kG + 0x2A90]        = kProgressBlock;
    memory[kProgressBlock + 4] = kSegment;
}

void nullPoolInto(ResolveWatch& watch)
{
    watch.poolObject = watch.poolBase = watch.poolSize = 0;
    watch.poolPos                                      = 0;
    watch.wrapPos                                      = kWrapPos;
    watch.sideEnd                                      = kSideEnd;
    watch.segment                                      = kSegment;
    watch.wrapLimit                                    = kWrapLimit;
    watch.bracket = watch.bracketNext = 0;
    watch.progress                    = kSegment;
}

Memory build(const Case& c)
{
    Memory memory;
    ringInto(memory);
    for (std::uint32_t slot = 0; slot < 4; ++slot)
    {
        memory[kG + 0x3098 + 4 * slot]         = kSurface + 0x100 * slot;
        memory[kSurface + 0x100 * slot + 0x18] = 0x500;
    }
    memory[kG + 0x30A8]          = kDepthSurface;
    memory[kDepthSurface + 0x18] = 0x500;
    memory[kHeader + 28]         = c.d0;
    memory[kHeader + 32]         = 0x1E3B0006;
    memory[kHeader + 36]         = c.d2;
    memory[kHeader + 40]         = 0;
    memory[kHeader + 48]         = c.d5;
    memory[kResolveDefaultPoint] = memory[kResolveDefaultPoint + 4] = 0;
    for (std::uint32_t format = 0; format < 64; ++format)
    {
        const auto address = kResolveFormatTable + 2 * format;
        auto&      word    = memory[address & ~3U];
        const auto entry   = (1 + format % 8) << 3;
        word               = (address & 2) ? (word & 0xFFFF0000U) | entry : (word & 0x0000FFFFU) | (entry << 16);
    }
    const std::uint32_t slot = c.flags & 7;
    if (!c.shadowOnly)
        for (std::uint32_t k = 0; k <= 8; ++k)
            memory[kG + 0x2884 + 4 * k] = kShadowHit;
    memory[shadowAddress(slot)] = c.shadowHit || c.shadowOnly ? kShadowHit : kShadowMiss;
    clearInto(memory);
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

std::array<std::uint32_t, 8> registers(std::uint32_t flags)
{
    return { kG, flags, 0, kHeader, 0, 0, 0, 0 };
}

// d0 bits 2..3 equal 3: the only dword 0 the render target shadow block of
// the body runs for (g167:14417, 14527).
constexpr std::uint32_t kPair3 = 0x0A00000C;

// Slots 0..3 test G+0x2884, G+0x288C, G+0x2890, G+0x2894 and set bits 56, 54,
// 53, 52 respectively; depth slot 4 sets none. Expected masks model 0x8269E520
// after flush.
const Case kCases[] = {
    // Without the pair 3 the shadow words are never read, whatever they hold.
    { .name = "pair0_slot0_hit_sets_nothing", .flags = 0, .shadowHit = true },
    { .name = "pair1_slot0_hit_sets_nothing", .flags = 0, .d0 = 0x0A000004, .shadowHit = true },
    { .name = "pair2_slot2_hit_sets_nothing", .flags = 2, .d0 = 0x0A000008, .shadowHit = true },
    { .name = "pair0_slot3_only_hit_word_sets_nothing", .flags = 3, .shadowOnly = true },
    { .name = "slot0_shadow_miss_others_hit", .flags = 0, .d0 = kPair3 },
    { .name = "slot0_shadow_hit", .flags = 0, .d0 = kPair3, .shadowHit = true, .slotBit = 1ULL << 56 },
    { .name = "slot0_only_hit_word", .flags = 0, .d0 = kPair3, .shadowOnly = true, .slotBit = 1ULL << 56 },
    { .name = "slot1_shadow_miss_others_hit", .flags = 1, .d0 = kPair3 },
    { .name = "slot1_shadow_hit", .flags = 1, .d0 = kPair3, .shadowHit = true, .slotBit = 1ULL << 54 },
    { .name = "slot1_only_hit_word", .flags = 1, .d0 = kPair3, .shadowOnly = true, .slotBit = 1ULL << 54 },
    { .name = "slot2_shadow_hit", .flags = 2, .d0 = kPair3, .shadowHit = true, .slotBit = 1ULL << 53 },
    { .name = "slot2_only_hit_word", .flags = 2, .d0 = kPair3, .shadowOnly = true, .slotBit = 1ULL << 53 },
    { .name = "slot3_shadow_hit", .flags = 3, .d0 = kPair3, .shadowHit = true, .slotBit = 1ULL << 52 },
    { .name = "slot3_shadow_miss_others_hit", .flags = 3, .d0 = kPair3 },
    { .name = "slot4_depth_never_sets_a_slot_bit", .flags = 4, .d0 = kPair3, .shadowHit = true },
    { .name = "slot4_depth_only_hit_word", .flags = 4, .d0 = kPair3, .shadowOnly = true },
    // Flags 0x200 adds 0x20000 to G+0x10; 0x100 adds nothing that survives.
    { .name = "flags_0x200_pair0", .flags = 0x200, .shadowHit = true, .slotBit = 0x20000 },
    { .name = "flags_0x200_pair3_slot0_hit", .flags = 0x200, .d0 = kPair3, .shadowHit = true, .slotBit = (1ULL << 56) | 0x20000 },
    { .name = "flags_0x200_pair3_slot3_miss", .flags = 0x203, .d0 = kPair3, .slotBit = 0x20000 },
    { .name = "flags_0x100_adds_nothing", .flags = 0x100 },
    { .name = "flags_0x300_slot4", .flags = 0x304, .d0 = kPair3, .shadowHit = true, .slotBit = 0x20000 },
    // Fragment select and other flag bits do not change the masks.
    { .name = "fragment_select_flags", .flags = 0x00002070 | 0x54000000, .d0 = kPair3, .shadowHit = true, .slotBit = 1ULL << 56 },
    // The large form: G+0x2A44 = pitch * height with the 11-bit extent; the
    // masks are the same because the G+0x20 bit 4 it ORs is flushed.
    { .name = "large_form", .d2 = (719U << 11) | 1279U, .d5 = 0x400, .large = true, .pitchHeight = 0x500 * 720 },
    { .name = "large_form_max_extent", .d0 = 0x7FE00000, .d2 = (0x7FFU << 11) | 0x7FFU, .d5 = 0x400, .large = true, .pitchHeight = static_cast<std::uint32_t>(0x3FE0ULL * 0x800ULL) },
    { .name = "dword5_bits_0x200_is_not_large", .d5 = 0x200 },
    { .name = "dword5_bits_0x600_is_not_large", .d5 = 0x600 },
};

void testCases()
{
    for (const auto& c : kCases)
    {
        const auto             memory = build(c);
        const auto             read   = readerOf(memory);
        std::string            why;
        const auto             r = registers(c.flags);
        const ResolveClearArgs clear{ kSp, 0.5 };
        const auto             effects = DeriveResolveEffects(read, r, &why, &clear);
        const auto             words   = DeriveResolveWords(read, r);
        const std::string      label   = std::string(c.name) + ": ";
        UNSCOPED_INFO(label + "derived: " + why);
        REQUIRE((effects && words));
        UNSCOPED_INFO(label + "the copy words are DeriveResolveWords'");
        REQUIRE(effects->words == *words);
        UNSCOPED_INFO(label + "large form");
        REQUIRE(effects->large == c.large);
        if (c.large)
        {
            UNSCOPED_INFO(label + std::format("pitch times height {:08X} vs {:08X}", effects->pitchHeight, c.pitchHeight));
            REQUIRE(effects->pitchHeight == c.pitchHeight);
        }
        UNSCOPED_INFO(label + "depth clear flag");
        REQUIRE(effects->depthClear == ((c.flags & 0x200) != 0));
        UNSCOPED_INFO(label + "color clear flag");
        REQUIRE(effects->colorClear == ((c.flags & 0x100) != 0));
        UNSCOPED_INFO(label + std::format("G+0x10 before {:016X}", effects->setBeforeAlloc[0]));
        REQUIRE(effects->setBeforeAlloc[0] == (kCommon0 | c.slotBit));
        UNSCOPED_INFO(label + std::format("G+0x18 before {:016X}", effects->setBeforeAlloc[1]));
        REQUIRE(effects->setBeforeAlloc[1] == kCommon1);
        UNSCOPED_INFO(label + std::format("G+0x20 before {:016X}", effects->setBeforeAlloc[2]));
        REQUIRE(effects->setBeforeAlloc[2] == kCommon2);
        UNSCOPED_INFO(label + "after the allocator");
        REQUIRE((effects->setAfterAlloc[0] == kAfter0 && effects->setAfterAlloc[1] == kAfter1 && effects->setAfterAlloc[2] == 0));
    }
}

void testRefusals()
{
    std::string why;
    Case        base{ .name = "base", .shadowHit = true };
    auto        memory = build(base);
    auto        read   = readerOf(memory);
    auto        r      = registers(0);

    // The copy-word derivation's refusals carry through.
    auto mip = r;
    mip[5]   = 1;
    UNSCOPED_INFO("mip refused");
    REQUIRE((!DeriveResolveEffects(read, mip, &why) && why == "mip_or_slice"));
    auto slice = r;
    slice[6]   = 2;
    UNSCOPED_INFO("slice refused");
    REQUIRE((!DeriveResolveEffects(read, slice, &why) && why == "mip_or_slice"));
    auto wrapped = r;
    wrapped[0]   = 0xFFFFFFF0U;
    UNSCOPED_INFO("device overflow refused");
    REQUIRE((!DeriveResolveEffects(read, wrapped, &why) && why == "address_overflow"));
    wrapped    = r;
    wrapped[3] = 0xFFFFFFF0U;
    UNSCOPED_INFO("header overflow refused");
    REQUIRE((!DeriveResolveEffects(read, wrapped, &why) && why == "address_overflow"));

    // A word only the effects read: the tested shadow word, for the pair 3.
    base.d0 = kPair3;
    memory  = build(base);
    memory.erase(shadowAddress(0));
    UNSCOPED_INFO("unreadable shadow word: " + why);
    REQUIRE((!DeriveResolveEffects(read, r, &why) && why == std::format("unreadable {:08X}", shadowAddress(0))));
    // The depth slot and any other pair never read the shadow word.
    UNSCOPED_INFO("the depth slot reads no shadow word");
    REQUIRE(DeriveResolveEffects(read, registers(4)).has_value());
    memory               = build(base);
    memory[kHeader + 28] = 0x0A000004;
    memory.erase(shadowAddress(0));
    UNSCOPED_INFO("pair 1 reads no shadow word");
    REQUIRE(DeriveResolveEffects(read, r).has_value());
    memory[kHeader + 28] = 0x0A000000;
    UNSCOPED_INFO("pair 0 reads no shadow word");
    REQUIRE(DeriveResolveEffects(read, r).has_value());
    // A slot other than the tested one is not read.
    memory = build(base);
    memory.erase(shadowAddress(1));
    UNSCOPED_INFO("only the slot's own shadow word is read");
    REQUIRE(DeriveResolveEffects(read, r).has_value());
}

ResolveWatch watchOf(std::uint64_t d10, std::uint64_t d18, std::uint64_t d20)
{
    ResolveWatch watch;
    watch.copy      = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000300 };
    watch.size      = 0x77;
    watch.dirty     = { d10, d18, d20 };
    watch.allocByte = 0x80;
    watch.stateByte = 0x0F;
    watch.fence     = 0x1234;
    watch.dest8     = 0;
    watch.dest12    = 0;
    ringInto(watch);
    return watch;
}

ResolveEffects effectsOf(bool large, bool depthClear)
{
    ResolveEffects effects;
    effects.words          = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000300 };
    effects.large          = large;
    effects.pitchHeight    = 0x384000;
    effects.depthClear     = depthClear;
    effects.setBeforeAlloc = { kCommon0 | (depthClear ? 0x20000ULL : 0), kCommon1, kCommon2 };
    effects.setAfterAlloc  = { kAfter0, kAfter1, 0 };
    return effects;
}

// What the body leaves for `effects` from `entry`.
ResolveWatch applied(const ResolveEffects& effects, const ResolveWatch& entry)
{
    ResolveWatch exit = entry;
    exit.copy         = { 0, effects.words.destBase, effects.words.destPitch, 0 };
    exit.size         = effects.large ? effects.pitchHeight : entry.size;
    for (std::size_t i = 0; i < kResolveDirtyWords; ++i)
        exit.dirty[i] |= effects.setBeforeAlloc[i] | effects.setAfterAlloc[i];
    exit.stateByte &= static_cast<std::uint8_t>(~kResolveStateClearedBit);
    if (effects.depthClear)
    {
        exit.allocByte |= kResolveDepthClearBit;
        exit.clear[0] = effects.depthWords[0];
        exit.clear[1] = effects.depthWords[1];
    }
    if (effects.colorClear)
    {
        exit.clear[2] = effects.colorWords[0];
        exit.clear[3] = effects.colorWords[1];
    }
    // The tail stores the fence at the return to both words; a zero fence takes
    // the list path, where +8 holds the fence as the body first read it.
    exit.dest8  = exit.fence ? exit.fence : entry.fence;
    exit.dest12 = exit.fence ? exit.fence : entry.dest12;
    // The allocator's moves: a fixed pool lowers both limits and stores the buffer
    // top; the null pool's forward take from G+0x3A44 == 0 stores kNullEnd there.
    if (entry.poolObject)
    {
        exit.limit34 = entry.limit34 - kSideBuffer;
        exit.limit38 = entry.limit38 - kSideBuffer;
        exit.poolPos = kBuffer;
        exit.poolUsed += kSideBuffer;
    }
    else
    {
        exit.poolPos = kNullEnd;
        exit.poolUsed += kSideBuffer;
    }
    return exit;
}

#if REREVVED_DEVELOPER
constexpr unsigned bitOf(ResolveEffectField field)
{
    return 1U << static_cast<unsigned>(field);
}
#endif

void testWatch()
{
    Memory memory;
    for (std::uint32_t i = 0; i < 4; ++i)
        memory[kG + 0x2A18 + 4 * i] = 0x11110000 + i;
    memory[kG + 0x10]    = 0x00000001;
    memory[kG + 0x14]    = 0x00000002;
    memory[kG + 0x18]    = 0x00000003;
    memory[kG + 0x1C]    = 0x00000004;
    memory[kG + 0x20]    = 0x80000005;
    memory[kG + 0x24]    = 0x00000006;
    memory[kG + 0x2A44]  = 0xCAFE;
    memory[kG + 0x2ABC]  = 0x30A5C300;
    memory[kG + 0x2A9C]  = 0xFFFFFFFE;
    memory[kHeader + 8]  = 0x88;
    memory[kHeader + 12] = 0xCC;
    ringInto(memory);
    for (std::uint32_t i = 0; i < kResolveClearWordCount; ++i)
        memory[kG + 0x2A28 + 4 * i] = 0x22220000 + i;
    ResolveWatch watch;
    UNSCOPED_INFO("watch read");
    REQUIRE(ReadResolveWatch(readerOf(memory), kG, kHeader, watch));
    UNSCOPED_INFO("the clear shadow words G+0x2A28..0x2A34");
    REQUIRE((watch.clear == std::array<std::uint32_t, 4>{ 0x22220000, 0x22220001, 0x22220002, 0x22220003 }));
    UNSCOPED_INFO("copy words");
    REQUIRE((watch.copy == std::array<std::uint32_t, 4>{ 0x11110000, 0x11110001, 0x11110002, 0x11110003 }));
    UNSCOPED_INFO("dirty words are big-endian pairs");
    REQUIRE((watch.dirty[0] == 0x0000000100000002ULL && watch.dirty[1] == 0x0000000300000004ULL && watch.dirty[2] == 0x8000000500000006ULL));
    UNSCOPED_INFO("bytes 0x2ABD and 0x2ABE");
    REQUIRE((watch.size == 0xCAFE && watch.allocByte == 0xA5 && watch.stateByte == 0xC3));
    UNSCOPED_INFO("header words");
    REQUIRE((watch.dest8 == 0x88 && watch.dest12 == 0xCC));
    UNSCOPED_INFO("the fence G+0x2A9C");
    REQUIRE(watch.fence == 0xFFFFFFFE);
    UNSCOPED_INFO("the ring and pool words");
    REQUIRE((watch.cursor == kCursor && watch.limit34 == kLimit34 && watch.limit38 == kLimit38 && watch.poolPos == 0 && watch.poolUsed == kPoolUsed));
    UNSCOPED_INFO("the model words");
    REQUIRE((watch.poolThreshold == kPoolThreshold && watch.targetCount == kTargets && watch.poolObject == kPoolObject && watch.poolBase == kPoolBase && watch.poolSize == kPoolSize));
    UNSCOPED_INFO("the mode byte 0x30 is read and has bit 0x10");
    REQUIRE((watch.tilingByte == 0x30 && watch.predicate == 0 && watch.Tiling() == ResolveTiling::Byte));
    memory[kG + 0x2ABC] = 0x0FA5C300;
    UNSCOPED_INFO("mode byte bits other than 0x30 are not tiled");
    REQUIRE((ReadResolveWatch(readerOf(memory), kG, kHeader, watch) && watch.tilingByte == 0x0F && watch.Tiling() == ResolveTiling::None));

    struct TilingWord
    {
        std::uint32_t address, value;
        ResolveTiling want;
    };

    for (const auto& [address, value, want] : { TilingWord{ kG + 0x2ABC, 0x20A5C300U, ResolveTiling::State }, TilingWord{ kG + 0x31AC, 1U, ResolveTiling::Predicate }, TilingWord{ kG + 0x33B0, 0x80000000U, ResolveTiling::State } })
    {
        auto tiled     = memory;
        tiled[address] = value;
        UNSCOPED_INFO(std::format("{:08X} = {:08X} is classified", address, value));
        REQUIRE((ReadResolveWatch(readerOf(tiled), kG, kHeader, watch) && watch.Tiling() == want));
    }
    {
        auto open         = memory;
        open[kG + 0x2ABC] = 0x21A5C300;
        open[kG + 0x33B0] = 0x80000000U;
        UNSCOPED_INFO("the byte and the bracket word together are the bracket form");
        REQUIRE((ReadResolveWatch(readerOf(open), kG, kHeader, watch) && watch.tilingByte == 0x21 && watch.predicate == 0 && watch.Tiling() == ResolveTiling::Bracket));
        open[kG + 0x31AC] = 2;
        UNSCOPED_INFO("the predicate word is read whole");
        REQUIRE((ReadResolveWatch(readerOf(open), kG, kHeader, watch) && watch.predicate == 2 && watch.Tiling() == ResolveTiling::PredicatedBracket));
        open[kG + 0x33B0] = 0;
        UNSCOPED_INFO("the predicate without the bracket word");
        REQUIRE((ReadResolveWatch(readerOf(open), kG, kHeader, watch) && watch.predicate == 2 && watch.Tiling() == ResolveTiling::Predicate));
        open[kG + 0x33B0] = 0x80000000U;
        open[kG + 0x31AC] = 0;
        open[kG + 0x33B0] = 0x80000000U;
        UNSCOPED_INFO("the bracket word is read whole");
        REQUIRE((ReadResolveWatch(readerOf(open), kG, kHeader, watch) && watch.bracket == 0x80000000U));
    }
    UNSCOPED_INFO("the bracket word and the wrap position are always read");
    REQUIRE((watch.bracket == 0x80000000U && watch.wrapPos == 0));
    // A pool object: the null pool allocator's words are not read (the fixture has none).
    UNSCOPED_INFO("a pool object reads no null pool word");
    REQUIRE((ReadResolveWatch(readerOf(memory), kG, kHeader, watch) && watch.sideEnd == 0 && watch.segment == 0 && watch.wrapLimit == 0 && watch.bracketNext == 0 && watch.progress == 0));
    // No pool object: the fixed buffer words are not read, the allocator's are.
    Memory bare = memory;
    nullPoolInto(bare);
    {
        bare[kG + 0x33B0] = 0x40;
        bare[kG + 0x33B4] = 0x77;
        bare[kG + 0x3A44] = 0x1234;
        UNSCOPED_INFO("a null pool object reads no buffer words");
        REQUIRE((ReadResolveWatch(readerOf(bare), kG, kHeader, watch) && watch.poolObject == 0 && watch.poolBase == 0 && watch.poolSize == 0));
        UNSCOPED_INFO("the null pool allocator's words");
        REQUIRE((watch.sideEnd == kSideEnd && watch.segment == kSegment && watch.wrapPos == kWrapPos && watch.wrapLimit == kWrapLimit && watch.bracket == 0x40 && watch.bracketNext == 0x77 && watch.progress == kSegment && watch.poolPos == 0x1234));
        // The progress word is behind the block pointer.
        bare[kProgressBlock + 4] = 0xCAFE0001;
        UNSCOPED_INFO("the progress word is read through G+0x2A90");
        REQUIRE((ReadResolveWatch(readerOf(bare), kG, kHeader, watch) && watch.progress == 0xCAFE0001));
    }
    for (const std::uint32_t missing : { kG + 0x3A3C, kG + 0x3A48, kG + 0x3A4C, kG + 0x3A50, kG + 0x33B4, kG + 0x2A90, kProgressBlock + 4 })
    {
        auto copy = bare;
        copy.erase(missing);
        UNSCOPED_INFO(std::format("an unreadable {:08X} fails the null pool read", missing));
        REQUIRE(!ReadResolveWatch(readerOf(copy), kG, kHeader, watch));
    }
    // A null block pointer or one whose word would wrap is not a progress word.
    for (const std::uint32_t block : { 0U, 0xFFFFFFFDU })
    {
        auto copy         = bare;
        copy[kG + 0x2A90] = block;
        copy[block + 4]   = 5;
        UNSCOPED_INFO(std::format("a progress block at {:08X} fails the read", block));
        REQUIRE(!ReadResolveWatch(readerOf(copy), kG, kHeader, watch));
    }
    for (const std::uint32_t missing : { kG + 0x2A18, kG + 0x2A24, kG + 0x2A28, kG + 0x2A2C, kG + 0x2A30, kG + 0x2A34, kG + 0x10, kG + 0x24, kG + 0x2A44, kG + 0x2ABC, kG + 0x2A9C, kHeader + 8, kHeader + 12, kG + 0x30, kG + 0x34, kG + 0x38, kG + 0x3A44, kG + 0x3A4C, kG + 0x3A54, kG + 0x3A58, kG + 0x31CC, kG + 0x31AC, kG + 0x33B0, kG + 0x34CC, kPoolObject + 152, kPoolObject + 156 })
    {
        auto copy = memory;
        copy.erase(missing);
        UNSCOPED_INFO(std::format("an unreadable {:08X} fails the read", missing));
        REQUIRE(!ReadResolveWatch(readerOf(copy), kG, kHeader, watch));
    }
    UNSCOPED_INFO("wrapping addresses fail");
    REQUIRE((!ReadResolveWatch(readerOf(memory), 0xFFFFFF00U, kHeader, watch) && !ReadResolveWatch(readerOf(memory), kG, 0xFFFFFFF8U, watch)));
    // The device guard covers the highest word read, G+0x3A58: with every word readable
    // (a pool object at 0x1000), the last device the span allows reads and the next does not.
    const GuestWordReader everything = [](std::uint32_t, std::uint32_t& value)
    {
        value = 0x1000;
        return true;
    };
    UNSCOPED_INFO("the last device whose pool threshold word fits is read");
    REQUIRE(ReadResolveWatch(everything, UINT32_MAX - kResolveDeviceSpan, kHeader, watch));
    UNSCOPED_INFO("a device whose pool threshold word would wrap fails");
    REQUIRE(!ReadResolveWatch(everything, UINT32_MAX - kResolveDeviceSpan + 1, kHeader, watch));
    UNSCOPED_INFO("the span ends after G+0x3A58");
    REQUIRE(kResolveDeviceSpan == 0x3A5C);
}

#if REREVVED_DEVELOPER
void testCompare()
{
    const auto entry = watchOf(0x1234, 0x40, 0x4);

    // The body's own effects match, with and without the large form and 0x200.
    for (const bool large : { false, true })
        for (const bool depth : { false, true })
        {
            const auto effects = effectsOf(large, depth);
            const auto result  = CompareResolveEffects(effects, entry, applied(effects, entry));
            UNSCOPED_INFO(std::format("matching exit large={} depth={}", large, depth));
            REQUIRE((result.mismatch == 0 && !result.allocFailed && !result.listPath && !result.cleared1E0000));
        }

    const auto effects = effectsOf(false, true);
    const auto exit    = applied(effects, entry);

    // Cleared bits are counted, never a mismatch: the flush groups drop old
    // bits the body does not set again.
    {
        auto flushed   = entry;
        flushed.dirty  = { 0x7ULL | 0x1000 | (1ULL << 50), 1ULL << 40, 0x4ULL | 0x3C00 };
        const auto old = flushed.dirty;
        auto       out = applied(effects, flushed);
        for (std::size_t i = 0; i < kResolveDirtyWords; ++i)
            out.dirty[i] &= ~old[i];
        const auto result = CompareResolveEffects(effects, flushed, out);
        UNSCOPED_INFO(std::format("cleared bits {}", result.clearedBits));
        REQUIRE((result.mismatch == 0 && result.clearedBits == 5 + 1 + 5 && !result.cleared1E0000));
    }
    // Any of 0x1E0000 cleared in G+0x10 is counted apart, not a mismatch.
    for (const std::uint64_t protectedBit : { 0x20000ULL, 0x40000ULL, 0x80000ULL, 0x100000ULL })
    {
        auto held = entry;
        held.dirty[0] |= protectedBit;
        auto freed = applied(effects, held);
        freed.dirty[0] &= ~protectedBit;
        // 0x20000 and 0x80000 are also bits the body sets; clear them from the
        // prediction so the case is only the flush.
        auto test = effects;
        test.setBeforeAlloc[0] &= ~protectedBit;
        test.setAfterAlloc[0] &= ~protectedBit;
        const auto result = CompareResolveEffects(test, held, freed);
        UNSCOPED_INFO(std::format("cleared 0x1E0000 bit {:X}", protectedBit));
        REQUIRE((result.mismatch == 0 && result.cleared1E0000 && result.clearedBits == 1));
    }
    // A bit of 0x1E0000 cleared in G+0x18 is not the protected one.
    {
        auto held = entry;
        held.dirty[1] |= 0x40000;
        auto freed = applied(effects, held);
        freed.dirty[1] &= ~0x40000ULL;
        UNSCOPED_INFO("only G+0x10 is protected");
        REQUIRE(!CompareResolveEffects(effects, held, freed).cleared1E0000);
    }

    // One corrupted watched word gives exactly its field.
    {
        auto bad    = exit;
        bad.copy[0] = 0x00100000;
        UNSCOPED_INFO("G+0x2A18 left at the copy control");
        REQUIRE(CompareResolveEffects(effects, entry, bad).mismatch == bitOf(ResolveEffectField::CopyWords));
        bad = exit;
        bad.copy[2] ^= 1;
        UNSCOPED_INFO("dest pitch word");
        REQUIRE(CompareResolveEffects(effects, entry, bad).mismatch == bitOf(ResolveEffectField::CopyWords));
        bad         = exit;
        bad.copy[3] = 0x300;
        UNSCOPED_INFO("G+0x2A24 not zeroed");
        REQUIRE(CompareResolveEffects(effects, entry, bad).mismatch == bitOf(ResolveEffectField::CopyWords));
        bad = exit;
        bad.size ^= 1;
        UNSCOPED_INFO("unchanged size word of a not large call");
        REQUIRE(CompareResolveEffects(effects, entry, bad).mismatch == bitOf(ResolveEffectField::Size));
        const auto large = effectsOf(true, false);
        bad              = applied(large, entry);
        bad.size         = entry.size;
        UNSCOPED_INFO("a large call that stored no size");
        REQUIRE(CompareResolveEffects(large, entry, bad).mismatch == bitOf(ResolveEffectField::Size));
        bad = exit;
        bad.dirty[0] &= ~(1ULL << 41);
        auto result = CompareResolveEffects(effects, entry, bad);
        UNSCOPED_INFO("a missing G+0x10 bit");
        REQUIRE((result.mismatch == bitOf(ResolveEffectField::DirtySetMissing) && result.missing[0] == (1ULL << 41) && result.clearedBits == 0));
        bad = exit;
        bad.dirty[2] &= ~0x80000ULL;
        UNSCOPED_INFO("a missing G+0x20 bit");
        REQUIRE(CompareResolveEffects(effects, entry, bad).mismatch == bitOf(ResolveEffectField::DirtySetMissing));
        bad = exit;
        bad.dirty[1] &= ~0x80000000ULL;
        UNSCOPED_INFO("a missing G+0x18 bit");
        REQUIRE(CompareResolveEffects(effects, entry, bad).mismatch == bitOf(ResolveEffectField::DirtySetMissing));
        auto noFour     = entry;
        noFour.dirty[2] = 0;
        auto surviving  = applied(effects, noFour);
        surviving.dirty[2] |= 0x4;
        result = CompareResolveEffects(effects, noFour, surviving);
        UNSCOPED_INFO("a G+0x20 bit 4 that survived the flush");
        REQUIRE((result.mismatch == bitOf(ResolveEffectField::DirtyExtra) && result.extra[2] == 0x4));
        // The same bit already set at entry is not new.
        auto four     = entry;
        four.dirty[2] = 0x4;
        UNSCOPED_INFO("a bit set at entry is not extra");
        REQUIRE(CompareResolveEffects(effects, four, applied(effects, four)).mismatch == 0);
        bad = exit;
        bad.stateByte |= kResolveStateClearedBit;
        UNSCOPED_INFO("G+0x2ABE bit 8 left set");
        REQUIRE(CompareResolveEffects(effects, entry, bad).mismatch == bitOf(ResolveEffectField::Bytes));
        bad = exit;
        bad.allocByte &= static_cast<std::uint8_t>(~kResolveDepthClearBit);
        UNSCOPED_INFO("G+0x2ABD bit 4 missing after a depth clear");
        REQUIRE(CompareResolveEffects(effects, entry, bad).mismatch == bitOf(ResolveEffectField::Bytes));
        const auto color = effectsOf(false, false);
        bad              = applied(color, entry);
        UNSCOPED_INFO("bit 4 is not required without the depth clear flag");
        REQUIRE(CompareResolveEffects(color, entry, bad).mismatch == 0);
        bad       = exit;
        bad.dest8 = 0;
        UNSCOPED_INFO("dest+8 not stored");
        REQUIRE(CompareResolveEffects(effects, entry, bad).mismatch == bitOf(ResolveEffectField::Dest));
        bad = exit;
        bad.dest12 ^= 4;
        UNSCOPED_INFO("dest+12 differs from the fence");
        REQUIRE(CompareResolveEffects(effects, entry, bad).mismatch == bitOf(ResolveEffectField::Dest));
        bad = exit;
        bad.copy[1] ^= 4;
        bad.size ^= 1;
        UNSCOPED_INFO("two fields at once");
        REQUIRE(CompareResolveEffects(effects, entry, bad).mismatch == (bitOf(ResolveEffectField::CopyWords) | bitOf(ResolveEffectField::Size)));
    }

    // The list path: a zero fence leaves dest+12 alone and has dest+8 at the entry fence.
    {
        auto zero         = entry;
        zero.fence        = 0;
        const auto list   = effectsOf(false, false);
        auto       out    = applied(list, zero);
        const auto result = CompareResolveEffects(list, zero, out);
        UNSCOPED_INFO("list path");
        REQUIRE((result.mismatch == 0 && result.listPath && !result.allocFailed && !result.mainFailed));
        out.dest12 = 0xDEAD;
        UNSCOPED_INFO("the list path leaves dest+12 alone (g167:15117-15160 never stores it)");
        REQUIRE(CompareResolveEffects(list, zero, out).mismatch == bitOf(ResolveEffectField::Dest));
        out.dest12 = zero.dest12;
        out.dest8  = 4;
        UNSCOPED_INFO("list path dest+8");
        REQUIRE(CompareResolveEffects(list, zero, out).mismatch == bitOf(ResolveEffectField::Dest));
    }

    // The fence moves by two whenever the body waits for ring space
    // (g12:12903-12905), between the read stored to +8 at g167:14928 and the
    // tail's read at 15104 or before the first. The return value decides.
    {
        auto       moved  = entry;
        const auto moveTo = [&](std::uint32_t fence)
        {
            auto out  = applied(effects, entry);
            out.fence = fence;
            out.dest8 = out.dest12 = fence;
            return out;
        };
        moved.fence = 0x1234;
        UNSCOPED_INFO("both words hold the fence at the return");
        REQUIRE(CompareResolveEffects(effects, moved, moveTo(0x1238)).mismatch == 0);
        UNSCOPED_INFO("a nonzero fence is not the list path");
        REQUIRE(CompareResolveEffects(effects, moved, moveTo(0x1238)).listPath == false);
        // The header path goes from the reload straight to the return, so a pair
        // left at the entry fence, or at any other value, after it moved is no
        // header store.
        auto stale  = moveTo(0x1238);
        stale.dest8 = stale.dest12 = 0x1234;
        UNSCOPED_INFO("words left at the entry fence after it moved");
        REQUIRE(CompareResolveEffects(effects, moved, stale).mismatch == bitOf(ResolveEffectField::Dest));
        stale.dest8 = stale.dest12 = 0x1236;
        UNSCOPED_INFO("words at a middle value of the sequence");
        REQUIRE(CompareResolveEffects(effects, moved, stale).mismatch == bitOf(ResolveEffectField::Dest));
        stale.dest8 = stale.dest12 = 0x1230;
        UNSCOPED_INFO("words holding a value before the entry fence");
        REQUIRE(CompareResolveEffects(effects, moved, stale).mismatch == bitOf(ResolveEffectField::Dest));
        auto half   = moveTo(0x1238);
        half.dest12 = 0x1234;
        UNSCOPED_INFO("dest+12 holding the entry fence");
        REQUIRE(CompareResolveEffects(effects, moved, half).mismatch == bitOf(ResolveEffectField::Dest));
        // A fence that rolls over to zero in the body: the list path, +8 holding
        // the value before or after the step but nothing else.
        moved.fence = 0xFFFFFFFE;
        for (const std::uint32_t stored : { 0xFFFFFFFEU, 0U })
        {
            auto out          = applied(effects, moved);
            out.fence         = 0;
            out.dest8         = stored;
            out.dest12        = moved.dest12;
            const auto result = CompareResolveEffects(effects, moved, out);
            UNSCOPED_INFO(std::format("rollover with +8 {:08X}", stored));
            REQUIRE((result.mismatch == 0 && result.listPath));
        }
        for (const std::uint32_t stored : { 0xFFFFFFFCU, 2U, 0xFFFFFFFFU })
        {
            auto out   = applied(effects, moved);
            out.fence  = 0;
            out.dest8  = stored;
            out.dest12 = moved.dest12;
            UNSCOPED_INFO(std::format("rollover with +8 {:08X} is off the fence sequence", stored));
            REQUIRE(CompareResolveEffects(effects, moved, out).mismatch == bitOf(ResolveEffectField::Dest));
        }
        // Rolling over to a nonzero fence is the plain tail.
        moved.fence = 0xFFFFFFFC;
        auto out    = applied(effects, moved);
        out.fence   = 0xFFFFFFFE;
        out.dest8 = out.dest12 = 0xFFFFFFFE;
        UNSCOPED_INFO("no rollover");
        REQUIRE(CompareResolveEffects(effects, moved, out).mismatch == 0);
        // A list path whose later allocation moved the fence: +12 still holds its entry value.
        moved.fence = 0;
        out         = applied(effects, moved);
        out.fence   = 2;
        out.dest8   = 0;
        out.dest12  = moved.dest12;
        UNSCOPED_INFO("a fence moved after the list path was taken");
        REQUIRE(CompareResolveEffects(effects, moved, out).mismatch == 0);
    }

    // Header pairs no path of the body can leave (g167:14928, 15104-15114): the
    // list path is a zero reload and needs zero in the fence sequence; a nonzero
    // reload stores both words with one value of it.
    {
        const auto exitWith = [&](std::uint32_t entryFence, std::uint32_t exitFence, std::uint32_t dest8, std::uint32_t dest12, std::uint32_t entryDest12)
        {
            auto from   = entry;
            from.fence  = entryFence;
            from.dest12 = entryDest12;
            auto out    = applied(effects, from);
            out.fence   = exitFence;
            out.dest8   = dest8;
            out.dest12  = dest12;
            return CompareResolveEffects(effects, from, out);
        };
        const unsigned dest = bitOf(ResolveEffectField::Dest);
        // Entry 0x1234, return 0x1236: +8 at the entry value with +12 unchanged
        // would be the list path without a zero reload.
        UNSCOPED_INFO("no zero in the sequence: the list shape is impossible");
        REQUIRE(exitWith(0x1234, 0x1236, 0x1234, 0x77, 0x77).mismatch == dest);
        UNSCOPED_INFO("no zero in the sequence, +12 zero at the entry");
        REQUIRE(exitWith(0x1234, 0x1236, 0x1234, 0, 0).mismatch == dest);
        UNSCOPED_INFO("unequal header words");
        REQUIRE(exitWith(0x1234, 0x1236, 0x1234, 0x1236, 0x77).mismatch == dest);
        UNSCOPED_INFO("the header path at the return value");
        REQUIRE(exitWith(0x1234, 0x1236, 0x1236, 0x1236, 0x77).mismatch == 0);
        UNSCOPED_INFO("the pair at the entry fence after it moved");
        REQUIRE(exitWith(0x1234, 0x1236, 0x1234, 0x1234, 0x77).mismatch == dest);
        UNSCOPED_INFO("an odd step is not in the sequence");
        REQUIRE(exitWith(0x1234, 0x1236, 0x1235, 0x1235, 0x77).mismatch == dest);
        UNSCOPED_INFO("a value past the return fence");
        REQUIRE(exitWith(0x1234, 0x1236, 0x1238, 0x1238, 0x77).mismatch == dest);
        UNSCOPED_INFO("a value before the entry fence");
        REQUIRE(exitWith(0x1234, 0x1236, 0x1232, 0x1232, 0x77).mismatch == dest);
        // Entry 0, return 2: the zero is the entry itself, so +8 can only be 0
        // on the list path, and +12 is unchanged; +8 at 2 is a header pair.
        UNSCOPED_INFO("entry 0, return 2, +8 2 and +12 cleared");
        REQUIRE(exitWith(0, 2, 2, 0, 0x77).mismatch == dest);
        UNSCOPED_INFO("entry 0, return 2, +8 2 and +12 unchanged");
        REQUIRE(exitWith(0, 2, 2, 0x77, 0x77).mismatch == dest);
        UNSCOPED_INFO("entry 0, return 2, both words 2");
        REQUIRE(exitWith(0, 2, 2, 2, 0x77).mismatch == 0);
        UNSCOPED_INFO("entry 0, return 2, the zero reload with +8 0");
        REQUIRE(exitWith(0, 2, 0, 0x77, 0x77).mismatch == 0);
        UNSCOPED_INFO("the zero reload with +12 changed");
        REQUIRE(exitWith(0, 2, 0, 0, 0x77).mismatch == dest);
        // A header pair of zero is not a store of a nonzero reload.
        UNSCOPED_INFO("a zero pair against a nonzero +12 entry");
        REQUIRE(exitWith(0, 0, 0, 0, 0x77).mismatch == dest);
        UNSCOPED_INFO("zero pair as the list path when +12 was zero");
        REQUIRE((exitWith(0, 0, 0, 0, 0).mismatch == 0 && exitWith(0, 0, 0, 0, 0).listPath));
        // A zero in the sequence: +8 not past it.
        UNSCOPED_INFO("header pair before the zero is not the return fence");
        REQUIRE(exitWith(0xFFFFFFFC, 2, 0xFFFFFFFE, 0xFFFFFFFE, 0x77).mismatch == dest);
        UNSCOPED_INFO("header pair after the zero");
        REQUIRE(exitWith(0xFFFFFFFC, 2, 2, 2, 0x77).mismatch == 0);
        UNSCOPED_INFO("list path, +8 before the zero");
        REQUIRE(exitWith(0xFFFFFFFC, 2, 0xFFFFFFFE, 0x77, 0x77).mismatch == 0);
        UNSCOPED_INFO("list path, +8 at the zero");
        REQUIRE(exitWith(0xFFFFFFFC, 2, 0, 0x77, 0x77).mismatch == 0);
        UNSCOPED_INFO("list path, +8 after the zero");
        REQUIRE(exitWith(0xFFFFFFFC, 2, 2, 0x77, 0x77).mismatch == dest);
        UNSCOPED_INFO("list path, +12 changed");
        REQUIRE(exitWith(0xFFFFFFFC, 2, 0xFFFFFFFE, 0x78, 0x77).mismatch == dest);
        // The header pair is the nonzero return fence, which is never at or
        // before the zero of S, so no pair is both a header and a list exit.
        const auto header = exitWith(0xFFFFFFFC, 2, 2, 2, 0xFFFFFFFE);
        UNSCOPED_INFO("the header pair at the return fence");
        REQUIRE((header.mismatch == 0 && !header.listPath));
    }

    // An allocation failure: the post-allocator bits, the exit stores and the
    // header words are not predicted; G+0x2A18 and G+0x2A24 keep the copy words.
    {
        ResolveWatch failed = entry;
        failed.copy         = { effects.words.copyControl, effects.words.destBase, effects.words.destPitch, effects.words.destInfo };
        failed.allocByte |= kResolveAllocFailedBit | kResolveDepthClearBit;
        failed.stateByte &= static_cast<std::uint8_t>(~kResolveStateClearedBit);
        for (std::size_t i = 0; i < kResolveDirtyWords; ++i)
            failed.dirty[i] |= effects.setBeforeAlloc[i];
        auto result = CompareResolveEffects(effects, entry, failed);
        UNSCOPED_INFO("alloc failure matches");
        REQUIRE((result.mismatch == 0 && result.allocFailed && !result.listPath));
        // The same exit without the failure marker is the missing bits, copy words and header.
        failed.allocByte &= static_cast<std::uint8_t>(~kResolveAllocFailedBit);
        result = CompareResolveEffects(effects, entry, failed);
        UNSCOPED_INFO("the failure is told by the marker alone");
        REQUIRE(result.mismatch == (bitOf(ResolveEffectField::CopyWords) | bitOf(ResolveEffectField::DirtySetMissing) | bitOf(ResolveEffectField::Dest)));
        // A failure that lost a pre-allocator bit is still missing it.
        failed.allocByte |= kResolveAllocFailedBit;
        failed.dirty[1] &= ~(1ULL << 35);
        result = CompareResolveEffects(effects, entry, failed);
        UNSCOPED_INFO("pre-allocator bits are required after a failure");
        REQUIRE((result.mismatch == bitOf(ResolveEffectField::DirtySetMissing) && result.missing[1] == (1ULL << 35)));
        // The marker already set at entry is the allocator's early return: same prediction.
        auto sticky = entry;
        sticky.allocByte |= kResolveAllocFailedBit;
        failed      = sticky;
        failed.copy = { effects.words.copyControl, effects.words.destBase, effects.words.destPitch, effects.words.destInfo };
        for (std::size_t i = 0; i < kResolveDirtyWords; ++i)
            failed.dirty[i] |= effects.setBeforeAlloc[i];
        failed.allocByte |= kResolveDepthClearBit;
        failed.stateByte &= static_cast<std::uint8_t>(~kResolveStateClearedBit);
        UNSCOPED_INFO("a marker set at entry");
        REQUIRE(CompareResolveEffects(effects, sticky, failed).mismatch == 0);
        // The entry marker alone is the body's failure: the allocator returned at
        // once, whatever the marker reads at the return.
        failed.allocByte &= static_cast<std::uint8_t>(~kResolveAllocFailedBit);
        const auto cleared = CompareResolveEffects(effects, sticky, failed);
        UNSCOPED_INFO("a marker set at entry decides without the marker at the return");
        REQUIRE((cleared.mismatch == 0 && cleared.mainFailed && !cleared.allocFailed));
    }

    // The list path's own allocation (0x826A6958) can fail after the body's
    // succeeded, with the exit stores and the header words done: the marker is
    // set, nothing else changes shape.
    {
        auto zero       = entry;
        zero.fence      = 0;
        const auto list = effectsOf(false, true);
        auto       late = applied(list, zero);
        late.allocByte |= kResolveAllocFailedBit;
        auto result = CompareResolveEffects(list, zero, late);
        UNSCOPED_INFO("a late list path failure leaves the main success shape");
        REQUIRE((result.mismatch == 0 && result.allocFailed && !result.mainFailed && result.listPath));
        UNSCOPED_INFO("the late failure fixture zeroed the copy words");
        REQUIRE((late.copy[0] == 0 && late.copy[3] == 0));
        // The same marker with the copy words still in place is the body's own failure.
        ResolveWatch main = zero;
        main.copy         = { list.words.copyControl, list.words.destBase, list.words.destPitch, list.words.destInfo };
        main.allocByte |= kResolveAllocFailedBit | kResolveDepthClearBit;
        main.stateByte &= static_cast<std::uint8_t>(~kResolveStateClearedBit);
        for (std::size_t i = 0; i < kResolveDirtyWords; ++i)
            main.dirty[i] |= list.setBeforeAlloc[i];
        result = CompareResolveEffects(list, zero, main);
        UNSCOPED_INFO("the same marker after the body's own failure");
        REQUIRE((result.mismatch == 0 && result.allocFailed && result.mainFailed && !result.listPath));
        // A late failure judged as a main failure would flag the missing post-allocator bits.
        auto wrong = late;
        wrong.dirty[0] &= ~0x80000ULL;
        UNSCOPED_INFO("a late failure still requires the post-allocator bits");
        REQUIRE(CompareResolveEffects(list, zero, wrong).mismatch == bitOf(ResolveEffectField::DirtySetMissing));
    }
}
#endif

// Hand effects use generated masks and 1280x720 copy words; entries include
// dropped and retained bits.
ResolveEffects handEffects(bool large)
{
    ResolveEffects effects;
    effects.words          = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000300 };
    effects.large          = large;
    effects.pitchHeight    = 0x00384000;
    effects.setBeforeAlloc = { 0x0000138000000000ULL | 0xE0, 0x0000000800000000ULL, 0x80000 };
    effects.setAfterAlloc  = { 0x98908, 0x80000000, 0 };
    return effects;
}

ResolveWatch handEntry(std::array<std::uint64_t, kResolveDirtyWords> dirty)
{
    ResolveWatch watch;
    watch.copy      = { 0x00100000, 0x1E3B0000, 0x02D00500, 0x00000300 };
    watch.size      = 0x77;
    watch.dirty     = dirty;
    watch.allocByte = 0x80;
    watch.stateByte = 0x0F;
    watch.fence     = 0x1234;
    watch.dest8     = 0x1111;
    watch.dest12    = 0x2222;
    ringInto(watch);
    return watch;
}

constexpr std::array<std::uint64_t, kResolveDirtyWords> kMixedEntry{ 0xFC0020004003FFFFULL, 0x8070000000000007ULL, 0x4000002000000004ULL };
constexpr std::array<std::uint64_t, kResolveDirtyWords> kMixedExit{ 0xFC001380000B89E8ULL, 0x8000000880000007ULL, 0x4000000000080000ULL };

struct ExitCase
{
    const char*                                   name;
    bool                                          large;
    std::array<std::uint64_t, kResolveDirtyWords> entry, exit;
    std::uint32_t                                 size;
};

// The exits are worked out by hand from the flush masks of g167:13644-13867:
// G+0x10 keeps bits 17..20 and 58..63, G+0x18 bits 0..33 and 55..63, G+0x20
// bits 38..63; the entry's other bits are dropped and the ORed masks added.
const ExitCase kExitCases[] = {
    { "mixed_entry", false, kMixedEntry, kMixedExit, 0x77 },
    { "mixed_entry_large", true, kMixedEntry, kMixedExit, 0x384000 },
    { "clean_entry", false, { 0, 0, 0 }, { 0x00001380000989E8ULL, 0x0000000880000000ULL, 0x80000ULL }, 0x77 },
    { "all_bits_set", false, { ~0ULL, ~0ULL, ~0ULL }, { 0xFC001380001F89E8ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFC000080000ULL }, 0x77 },
};

void testExitState()
{
    for (const auto& c : kExitCases)
    {
        const std::string label = std::string(c.name) + ": ";
        const auto        entry = handEntry(c.entry);
        std::string       why;
        const auto        exit = DeriveResolveExitState(handEffects(c.large), entry, nullptr, &why);
        UNSCOPED_INFO(label + "derived: " + why);
        REQUIRE(exit.has_value());
        UNSCOPED_INFO(label + "copy words zero the control and info words");
        REQUIRE((exit->copy == std::array<std::uint32_t, 4>{ 0, 0x1E3B0000, 0x02D00500, 0 }));
        UNSCOPED_INFO(label + std::format("size {:X}", exit->size));
        REQUIRE(exit->size == c.size);
        for (std::size_t i = 0; i < kResolveDirtyWords; ++i)
        {
            UNSCOPED_INFO(label + std::format("dirty word {} {:016X} vs {:016X}", i, exit->dirty[i], c.exit[i]));
            REQUIRE(exit->dirty[i] == c.exit[i]);
        }
        UNSCOPED_INFO(label + "bytes: 0x08 cleared from G+0x2ABE only");
        REQUIRE((exit->allocByte == 0x80 && exit->stateByte == 0x07));
        UNSCOPED_INFO(label + "both header words are the fence");
        REQUIRE((exit->dest8 == 0x1234 && exit->dest12 == 0x1234));
        UNSCOPED_INFO(label + "the side buffer pool moves");
        REQUIRE((exit->limit34 == kLimit34 - kSideBuffer && exit->limit38 == kLimit38 - kSideBuffer && exit->poolPos == kBuffer && exit->poolUsed == kPoolUsed + kSideBuffer));
    }

    // The header words are the fence of the ENTRY: a call that cannot flush leaves G+0x2A9C alone.
    {
        auto entry      = handEntry({ 0, 0, 0 });
        entry.fence     = 0x1236;
        const auto exit = DeriveResolveExitState(handEffects(false), entry);
        UNSCOPED_INFO("the header words follow the entry fence");
        REQUIRE((exit && exit->dest8 == 0x1236 && exit->dest12 == 0x1236));
    }
    // The state byte keeps every bit but 0x08, and the allocation byte keeps all of its own.
    for (const std::uint8_t state : { 0x00, 0x08, 0xF7, 0xFF })
        for (const std::uint8_t alloc : { 0x00, 0x04, 0x1F, 0xDF })
        {
            auto entry      = handEntry({ 0, 0, 0 });
            entry.stateByte = state;
            entry.allocByte = alloc;
            const auto exit = DeriveResolveExitState(handEffects(false), entry);
            UNSCOPED_INFO(std::format("bytes state {:02X} alloc {:02X}", state, alloc));
            REQUIRE((exit && exit->stateByte == (state & 0xF7) && exit->allocByte == alloc));
        }
}

void testExitRefusals()
{
    std::string why;
    const auto  entry          = handEntry({ 1, 2, 3 });
    auto        depth          = handEffects(false);
    depth.depthClear           = true;
    ResolveExitRefusal refusal = ResolveExitRefusal::None;
    UNSCOPED_INFO("a depth clear has an exact exit");
    REQUIRE((DeriveResolveExitState(depth, entry, &refusal, &why).has_value() && refusal == ResolveExitRefusal::None));
    auto color       = handEffects(false);
    color.colorClear = true;
    UNSCOPED_INFO("a color clear has an exact exit");
    REQUIRE((DeriveResolveExitState(color, entry, &refusal, &why).has_value() && refusal == ResolveExitRefusal::None));
    auto unmodeled         = color;
    unmodeled.clearRefused = "clear_color_format";
    UNSCOPED_INFO("an unmodeled color has no exact exit");
    REQUIRE((!DeriveResolveExitState(unmodeled, entry, &refusal, &why) && why == "clear_color_format" && refusal == ResolveExitRefusal::ClearColor));
    auto zero  = entry;
    zero.fence = 0;
    UNSCOPED_INFO("a zero fence is the list path");
    REQUIRE((!DeriveResolveExitState(handEffects(false), zero, &refusal, &why) && why == "fence_zero" && refusal == ResolveExitRefusal::Fence));
    auto marked      = entry;
    marked.allocByte = 0x20;
    UNSCOPED_INFO("the failure marker at entry has no exact exit");
    REQUIRE((!DeriveResolveExitState(handEffects(false), marked, &refusal, &why) && why == "alloc_marker" && refusal == ResolveExitRefusal::AllocMarker));
    marked.allocByte = 0xDF;
    UNSCOPED_INFO("every other allocation byte bit is fine");
    REQUIRE((DeriveResolveExitState(handEffects(false), marked, &refusal).has_value() && refusal == ResolveExitRefusal::None));
    // A tiled device other than the bracket form has no exact exit (the shapes are told apart in testTilingShapes).
    auto tiled       = entry;
    tiled.tilingByte = 0x10;
    UNSCOPED_INFO("a tiling byte with bit 0x10 has no exact exit");
    REQUIRE((!DeriveResolveExitState(handEffects(false), tiled, &refusal, &why) && why == "tiling_byte" && refusal == ResolveExitRefusal::Tiling));
    // The refusals are tested in the order of the enumerators; an unmodeled
    // color comes after tiling and before the allocator.
    auto every      = tiled;
    every.fence     = 0;
    every.allocByte = 0x20;
    every.poolBase  = 0;
    UNSCOPED_INFO("the fence first");
    REQUIRE((!DeriveResolveExitState(unmodeled, every, &refusal) && refusal == ResolveExitRefusal::Fence));
    every.fence = 1;
    UNSCOPED_INFO("then the marker");
    REQUIRE((!DeriveResolveExitState(unmodeled, every, &refusal) && refusal == ResolveExitRefusal::AllocMarker));
    every.allocByte = 0;
    UNSCOPED_INFO("then tiling");
    REQUIRE((!DeriveResolveExitState(handEffects(false), every, &refusal) && refusal == ResolveExitRefusal::Tiling));
    UNSCOPED_INFO("tiling before the clear color");
    REQUIRE((!DeriveResolveExitState(unmodeled, every, &refusal) && refusal == ResolveExitRefusal::Tiling));
    every.tilingByte = 0;
    UNSCOPED_INFO("then the clear color");
    REQUIRE((!DeriveResolveExitState(unmodeled, every, &refusal) && refusal == ResolveExitRefusal::ClearColor));
    UNSCOPED_INFO("then the pool object without a fixed buffer");
    REQUIRE((!DeriveResolveExitState(handEffects(false), every, &refusal) && refusal == ResolveExitRefusal::AllocPath));
    every.tilingByte = 0x21;
    every.bracket    = 0;
    UNSCOPED_INFO("tiling state before the clear color");
    REQUIRE((!DeriveResolveExitState(unmodeled, every, &refusal) && refusal == ResolveExitRefusal::TilingState));

    // The reader overload refuses what the effects refuse, and an unreadable watched word.
    const Memory memory = build({ .name = "r", .shadowHit = true });
    const auto   read   = readerOf(memory);
    UNSCOPED_INFO("an unwritten watched word: " + why);
    REQUIRE((!DeriveResolveExitState(read, registers(0), &why) && why == "unreadable watched words"));
    auto mip = registers(0);
    mip[5]   = 1;
    UNSCOPED_INFO("the derivation's refusal carries through");
    REQUIRE((!DeriveResolveExitState(read, mip, &why) && why == "mip_or_slice"));
}

// The watched words in guest memory form, for the reader overload.
Memory withWatch(Memory memory, const ResolveWatch& watch)
{
    for (std::uint32_t i = 0; i < 4; ++i)
        memory[kG + 0x2A18 + 4 * i] = watch.copy[i];
    for (std::uint32_t i = 0; i < kResolveDirtyWords; ++i)
    {
        memory[kG + 0x10 + 8 * i] = static_cast<std::uint32_t>(watch.dirty[i] >> 32);
        memory[kG + 0x14 + 8 * i] = static_cast<std::uint32_t>(watch.dirty[i]);
    }
    for (std::uint32_t i = 0; i < kResolveClearWordCount; ++i)
        memory[kG + kResolveClearShadow + 4 * i] = watch.clear[i];
    memory[kG + 0x2A44]  = watch.size;
    memory[kG + 0x2A9C]  = watch.fence;
    memory[kG + 0x2ABC]  = (std::uint32_t{ watch.tilingByte } << 24) | (std::uint32_t{ watch.allocByte } << 16) | (std::uint32_t{ watch.stateByte } << 8);
    memory[kHeader + 8]  = watch.dest8;
    memory[kHeader + 12] = watch.dest12;
    memory[kG + 0x30]    = watch.cursor;
    memory[kG + 0x34]    = watch.limit34;
    memory[kG + 0x38]    = watch.limit38;
    memory[kG + 0x3A44]  = watch.poolPos;
    memory[kG + 0x3A4C]  = watch.wrapPos;
    memory[kG + 0x3A54]  = watch.poolUsed;
    memory[kG + 0x3A58]  = watch.poolThreshold;
    memory[kG + 0x31CC]  = watch.targetCount;
    memory[kG + 0x34CC]  = watch.poolObject;
    if (watch.poolObject)
    {
        memory[watch.poolObject + 152] = watch.poolBase;
        memory[watch.poolObject + 156] = watch.poolSize;
    }
    else
    {
        memory[kG + 0x3A3C]        = watch.sideEnd;
        memory[kG + 0x3A48]        = watch.segment;
        memory[kG + 0x3A50]        = watch.wrapLimit;
        memory[kG + 0x33B4]        = watch.bracketNext;
        memory[kG + 0x2A90]        = kProgressBlock;
        memory[kProgressBlock + 4] = watch.progress;
    }
    memory[kG + 0x31AC] = watch.predicate;
    memory[kG + 0x33B0] = watch.bracket;
    return memory;
}

void testExitFromMemory()
{
    const auto entry  = handEntry(kMixedEntry);
    const auto memory = withWatch(build({ .name = "r", .shadowHit = true }), entry);
    const auto read   = readerOf(memory);
    const auto exit   = DeriveResolveExitState(read, registers(0));
    const auto words  = DeriveResolveWords(read, registers(0));
    UNSCOPED_INFO("the fixture memory is modeled");
    REQUIRE((exit && words));
    UNSCOPED_INFO("copy words from the derived words");
    REQUIRE((exit->copy == std::array<std::uint32_t, 4>{ 0, words->destBase, words->destPitch, 0 }));
    UNSCOPED_INFO("dirty words from memory");
    REQUIRE(exit->dirty == kMixedExit);
    UNSCOPED_INFO("bytes, size and header words from memory");
    REQUIRE((exit->size == 0x77 && exit->allocByte == 0x80 && exit->stateByte == 0x07 && exit->dest8 == 0x1234 && exit->dest12 == 0x1234));
    UNSCOPED_INFO("the pool words from memory");
    REQUIRE((exit->limit34 == kLimit34 - kSideBuffer && exit->limit38 == kLimit38 - kSideBuffer && exit->poolPos == kBuffer && exit->poolUsed == kPoolUsed + kSideBuffer));
    // The fence of memory is the fence of the exit.
    auto moved           = memory;
    moved[kG + 0x2A9C]   = 0x9998;
    const auto movedExit = DeriveResolveExitState(readerOf(moved), registers(0));
    UNSCOPED_INFO("the fence is read from memory");
    REQUIRE((movedExit && movedExit->dest8 == 0x9998 && movedExit->dest12 == 0x9998));
    // The zero fence is the list path.
    moved[kG + 0x2A9C] = 0;
    std::string why;
    UNSCOPED_INFO("a zero fence in memory");
    REQUIRE((!DeriveResolveExitState(readerOf(moved), registers(0), &why) && why == "fence_zero"));
    UNSCOPED_INFO("flags 0x200 need the stack words");
    REQUIRE((!DeriveResolveExitState(read, registers(0x200), &why) && why == "clear_inputs"));
    UNSCOPED_INFO("flags 0x100 need the stack words");
    REQUIRE((!DeriveResolveExitState(read, registers(0x100), &why) && why == "clear_inputs"));
}

ResolveWatch watchOfExit(const ResolveExitState& exit)
{
    ResolveWatch watch;
    watch.copy      = exit.copy;
    watch.size      = exit.size;
    watch.dirty     = exit.dirty;
    watch.allocByte = exit.allocByte;
    watch.stateByte = exit.stateByte;
    watch.dest8     = exit.dest8;
    watch.dest12    = exit.dest12;
    watch.limit34   = exit.limit34;
    watch.limit38   = exit.limit38;
    watch.poolPos   = exit.poolPos;
    watch.poolUsed  = exit.poolUsed;
    watch.wrapPos   = exit.wrapPos;
    watch.clear     = exit.clear;
    return watch;
}

#if REREVVED_DEVELOPER
void testExitMismatches()
{
    const auto entry    = handEntry({ 1, 2, 3 });
    const auto exitWant = *DeriveResolveExitState(handEffects(false), entry);
    const auto same     = watchOfExit(exitWant);
    UNSCOPED_INFO("an equal exit has no mismatch");
    REQUIRE(ResolveExitMismatches(exitWant, same) == 0);
    // Bit i is word i of kResolveExitWordNames.
    const std::function<void(ResolveWatch&)> changes[kResolveExitWordCount] = {
        [](ResolveWatch& w)
        {
            w.copy[0] ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.copy[1] ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.copy[2] ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.copy[3] ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.size ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.dirty[0] ^= 1ULL << 63;
        },
        [](ResolveWatch& w)
        {
            w.dirty[1] ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.dirty[2] ^= 1ULL << 32;
        },
        [](ResolveWatch& w)
        {
            w.allocByte ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.stateByte ^= 0x08;
        },
        [](ResolveWatch& w)
        {
            w.dest8 ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.dest12 ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.limit34 ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.limit38 ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.poolPos ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.poolUsed ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.wrapPos ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.clear[0] ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.clear[1] ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.clear[2] ^= 1;
        },
        [](ResolveWatch& w)
        {
            w.clear[3] ^= 1;
        },
    };
    for (std::size_t i = 0; i < kResolveExitWordCount; ++i)
    {
        auto changed = same;
        changes[i](changed);
        UNSCOPED_INFO(std::format("word {} ({}) differs alone", i, kResolveExitWordNames[i]));
        REQUIRE(ResolveExitMismatches(exitWant, changed) == 1U << i);
    }
}
#endif

void testExitWrites()
{
    const auto entry = handEntry(kMixedEntry);
    const auto exit  = *DeriveResolveExitState(handEffects(false), entry);
    // The words that change, in kResolveExitWordNames order: the control and info
    // copy words (the base and pitch equal the entry's), each half of the three
    // dirty words that differs, the state byte, both header words and the four pool words.
    const std::vector<ResolveWrite> expected = {
        { kG + 0x2A18, 0, false },
        { kG + 0x2A24, 0, false },
        { kG + 0x10, 0xFC001380, false },
        { kG + 0x14, 0x000B89E8, false },
        { kG + 0x18, 0x80000008, false },
        { kG + 0x1C, 0x80000007, false },
        { kG + 0x20, 0x40000000, false },
        { kG + 0x24, 0x00080000, false },
        { kG + 0x2ABE, 0x07, true },
        { kHeader + 8, 0x1234, false },
        { kHeader + 12, 0x1234, false },
        { kG + 0x34, kLimit34 - kSideBuffer, false },
        { kG + 0x38, kLimit38 - kSideBuffer, false },
        { kG + 0x3A44, kBuffer, false },
        { kG + 0x3A54, kPoolUsed + kSideBuffer, false },
    };
    UNSCOPED_INFO("the writes of the worked exit");
    REQUIRE(ResolveExitWrites(kG, kHeader, entry, exit) == expected);
    // An unchanged exit writes nothing; one low half alone is the one store.
    const auto same = watchOfExit(exit);
    UNSCOPED_INFO("no change, no write");
    REQUIRE(ResolveExitWrites(kG, kHeader, same, exit).empty());
    auto low = same;
    low.dirty[1] ^= 0x10;
    UNSCOPED_INFO("one half of a dirty word");
    REQUIRE(ResolveExitWrites(kG, kHeader, low, exit) == std::vector<ResolveWrite>{ { kG + 0x1C, 0x80000007, false } });
    auto sized      = same;
    sized.size      = 0;
    sized.allocByte = 0;
    UNSCOPED_INFO("size and the allocation byte");
    REQUIRE(ResolveExitWrites(kG, kHeader, sized, exit) == std::vector<ResolveWrite>{ { kG + 0x2A44, exit.size, false }, { kG + 0x2ABD, exit.allocByte, true } });
}

#if REREVVED_DEVELOPER
// Effect prediction uses entry; Original path changes mismatch, refusals skip.
void testCompareExact()
{
    const auto   effects = handEffects(false);
    const auto   entry   = handEntry(kMixedEntry);
    ResolveWatch exit    = watchOfExit(*DeriveResolveExitState(effects, entry));
    exit.fence           = 0x1234;
    auto result          = CompareResolveEffects(effects, entry, exit);
    UNSCOPED_INFO("the worked exit is exact");
    REQUIRE((result.mismatch == 0 && result.exactModeled && result.exactMismatch == 0 && result.exactRefusal == ResolveExitRefusal::None));
    UNSCOPED_INFO("the exact state is recorded");
    REQUIRE(result.exact.dirty == kMixedExit);

    // A fence that moved while the body waited for ring space: the old probe
    // accepts both header words at the return fence, the exact prediction takes
    // the entry fence, so the call that flushed is a dest mismatch of it.
    auto moved  = exit;
    moved.fence = 0x1238;
    moved.dest8 = moved.dest12 = 0x1238;
    result                     = CompareResolveEffects(effects, entry, moved);
    UNSCOPED_INFO("a moved fence is a dest_8 and dest_12 mismatch of the exact exit");
    REQUIRE((result.mismatch == 0 && result.exactModeled && result.exactMismatch == ((1U << 10) | (1U << 11))));

    // A bit the flush should have cleared is no mismatch of the old probe, which
    // only sees flushes as counts, but a dirty_10 mismatch of the exact one.
    auto unflushed = exit;
    unflushed.dirty[0] |= 1;
    result = CompareResolveEffects(effects, entry, unflushed);
    UNSCOPED_INFO("an unflushed bit is a dirty_10 mismatch");
    REQUIRE((result.mismatch == 0 && result.exactModeled && result.exactMismatch == (1U << 5)));
    auto lost     = exit;
    lost.dirty[2] = 0;
    result        = CompareResolveEffects(effects, entry, lost);
    UNSCOPED_INFO("a lost bit is a dirty_20 mismatch");
    REQUIRE((result.exactModeled && result.exactMismatch == (1U << 7)));
    // A flush in Original moves the ring limits and the pool words elsewhere.
    auto flushedRing    = exit;
    flushedRing.limit38 = entry.limit38 + 0x1000;
    flushedRing.poolPos = 0;
    result              = CompareResolveEffects(effects, entry, flushedRing);
    UNSCOPED_INFO("ring limit and pool position mismatches");
    REQUIRE((result.exactModeled && result.exactMismatch == ((1U << 13) | (1U << 14))));

    // A list path or an allocation failure at the return of a call the exit state
    // models is a mismatch of the prediction, not a skip.
    auto list   = exit;
    list.fence  = 0;
    list.dest8  = 0x1234;
    list.dest12 = entry.dest12;
    result      = CompareResolveEffects(effects, entry, list);
    UNSCOPED_INFO("the list path: +12 was never stored");
    REQUIRE((result.listPath && result.exactModeled && result.exactMismatch == (1U << 11)));
    auto failed = exit;
    failed.allocByte |= kResolveAllocFailedBit;
    result = CompareResolveEffects(effects, entry, failed);
    UNSCOPED_INFO("an allocation failure is an alloc_byte mismatch");
    REQUIRE((result.allocFailed && result.exactModeled && result.exactMismatch == (1U << 8)));

    // A call the exit state refuses is skipped, with its reason.
    auto zero  = entry;
    zero.fence = 0;
    result     = CompareResolveEffects(effects, zero, applied(effects, zero));
    UNSCOPED_INFO("the zero fence is skipped by reason");
    REQUIRE((!result.exactModeled && result.exactRefusal == ResolveExitRefusal::Fence));
    auto closeCursor   = entry;
    closeCursor.cursor = entry.limit38 - 16;
    result             = CompareResolveEffects(effects, closeCursor, exit);
    UNSCOPED_INFO("a cursor near the limit is skipped as ring_space");
    REQUIRE((!result.exactModeled && result.exactRefusal == ResolveExitRefusal::RingSpace));
    auto tight       = entry;
    tight.poolPos    = kCursor + 64;
    tight.poolObject = kPoolObject;
    result           = CompareResolveEffects(effects, tight, exit);
    UNSCOPED_INFO("a pool top near the cursor is skipped as alloc_capacity");
    REQUIRE((!result.exactModeled && result.exactRefusal == ResolveExitRefusal::AllocCapacity));

    // A call whose stack reach ResolveStackOverlaps refuses at the entry is counted as a skipped exact exit with
    // the stack_overlap reason; the comparison itself takes no stack flag, so the exit is exact otherwise.
    result = CompareResolveEffects(effects, entry, exit);
    UNSCOPED_INFO("no overlap leaves the exit exact");
    REQUIRE((result.exactModeled && result.exactRefusal == ResolveExitRefusal::None));
    TakeResolveEffectCounts();
    NoteResolveEffectStackOverlap();
    const auto stackCounts = TakeResolveEffectCounts();
    UNSCOPED_INFO("a stack overlap is counted as a skipped exact exit");
    REQUIRE((stackCounts.exactSkipped == 1 && stackCounts.exactOk == 0 && stackCounts.exactRefused[static_cast<std::size_t>(ResolveExitRefusal::StackOverlap)] == 1));
}
#endif

// The ring bound and the allocator model of the exit state, at the exact edges.
void testRingModel()
{
    const auto effects = handEffects(false);
    auto       clean   = handEntry({ 0, 0, 0 });
    // 2 * (32 + 5n) + 2 * (12 + 7n) + (6 + 5n) + 85 with n = 4 is 295; the only
    // group bits are the 0x3C00 and the 4 the body ORs into G+0x20 (runs of four: 5, and one: 2).
    UNSCOPED_INFO(std::format("clean entry words {}", ResolveMaxRingWords(clean)));
    REQUIRE(ResolveMaxRingWords(clean) == 302);
    // Every flush-group bit set: 13 + 6 + 22 + 17 for G+0x10, 22 for G+0x18, 39 for G+0x20.
    auto full = handEntry({ ~0ULL, ~0ULL, ~0ULL });
    UNSCOPED_INFO(std::format("full entry words {}", ResolveMaxRingWords(full)));
    REQUIRE(ResolveMaxRingWords(full) == 295 + 58 + 22 + 39);
    // Bits outside the groups (17..20, 58..63 of G+0x10; 0..33, 55..63 of G+0x18; 38..63 of G+0x20) add nothing.
    auto outside = handEntry({ 0xFC00000000000000ULL | 0x1E0000ULL, 0xFF800003FFFFFFFFULL, 0xFFFFFFC000000000ULL });
    UNSCOPED_INFO("bits the groups never flush add no words");
    REQUIRE(ResolveMaxRingWords(outside) == 302);
    // Runs: two apart bits are two headers and two words, adjacent ones one header.
    auto runs = handEntry({ 0x5, 0, 0 });
    UNSCOPED_INFO("two isolated bits in one group");
    REQUIRE(ResolveMaxRingWords(runs) == 302 + 4);
    runs = handEntry({ 0x3, 0, 0 });
    UNSCOPED_INFO("two adjacent bits in one group");
    REQUIRE(ResolveMaxRingWords(runs) == 302 + 3);
    // Groups are separate calls: bits 11 and 12 straddle the 0..11 and 12..16 groups.
    runs = handEntry({ 0x1800, 0, 0 });
    UNSCOPED_INFO("adjacent bits in two groups are two headers");
    REQUIRE(ResolveMaxRingWords(runs) == 302 + 4);
    // Each render target adds 29 words; more than 16 is unbounded.
    clean.targetCount = 5;
    UNSCOPED_INFO("a render target adds 29 words");
    REQUIRE(ResolveMaxRingWords(clean) == 302 + 29);
    clean.targetCount = 16;
    UNSCOPED_INFO("sixteen render targets");
    REQUIRE(ResolveMaxRingWords(clean) == 302 + 12 * 29);
    clean.targetCount = 17;
    UNSCOPED_INFO("an unbounded render target count");
    REQUIRE(ResolveMaxRingWords(clean) == UINT64_MAX);
    UNSCOPED_INFO("an unbounded count has no exact exit");
    REQUIRE(!DeriveResolveExitState(effects, clean).has_value());

    const auto refusalOf = [&](const ResolveWatch& watch)
    {
        ResolveExitRefusal refusal = ResolveExitRefusal::None;
        DeriveResolveExitState(effects, watch, &refusal);
        return refusal;
    };
    const auto          entry = handEntry({ 0, 0, 0 });
    const std::uint32_t end   = kCursor + 4 * 302;
    UNSCOPED_INFO("the base entry is modeled");
    REQUIRE(refusalOf(entry) == ResolveExitRefusal::None);

    // The soft limit G+0x38 after the pool lowers it by 24: the cursor plus every word may reach it.
    auto soft    = entry;
    soft.limit38 = end + kSideBuffer;
    soft.limit34 = end + kSideBuffer + 160;
    UNSCOPED_INFO("the words end exactly at the lowered soft limit");
    REQUIRE(refusalOf(soft) == ResolveExitRefusal::None);
    soft.limit38 -= 1;
    UNSCOPED_INFO("a byte short of the soft limit");
    REQUIRE(refusalOf(soft) == ResolveExitRefusal::RingSpace);
    // The hard limit G+0x34 is strict: 0x826ACC60 needs the run to end below it.
    auto hard    = entry;
    hard.limit34 = end + kSideBuffer + 1;
    UNSCOPED_INFO("the words end one byte below the lowered hard limit");
    REQUIRE(refusalOf(hard) == ResolveExitRefusal::None);
    hard.limit34 = end + kSideBuffer;
    UNSCOPED_INFO("the words reach the lowered hard limit");
    REQUIRE(refusalOf(hard) == ResolveExitRefusal::RingSpace);
    // A limit below the 24 bytes the pool takes would wrap in Original: refused.
    auto wrap    = entry;
    wrap.limit38 = 20;
    UNSCOPED_INFO("a soft limit below the pool take");
    REQUIRE(refusalOf(wrap) == ResolveExitRefusal::RingSpace);
    // One more dirty bit moves the end by its words.
    auto bit    = soft;
    bit.limit38 = end + kSideBuffer + 8;
    bit.limit34 = end + kSideBuffer + 8 + 160;
    UNSCOPED_INFO("the clean entry fits");
    REQUIRE(refusalOf(bit) == ResolveExitRefusal::None);
    bit.dirty[0] = 0x5;
    UNSCOPED_INFO("four more words pass the limit");
    REQUIRE(refusalOf(bit) == ResolveExitRefusal::RingSpace);
    // The cursor at the top of the address space cannot be bounded.
    auto top   = entry;
    top.cursor = 0xFFFFF000;
    UNSCOPED_INFO("a cursor whose words wrap");
    REQUIRE(refusalOf(top) == ResolveExitRefusal::RingSpace);

    // The side buffer: (top - 24) & ~3 must stay 164 bytes above the cursor plus every word.
    auto pool    = entry;
    pool.poolPos = end + 164 + kSideBuffer;
    UNSCOPED_INFO("the side buffer ends exactly 164 bytes above the words");
    REQUIRE(refusalOf(pool) == ResolveExitRefusal::None);
    pool.poolPos -= 4;
    UNSCOPED_INFO("four bytes short of the side buffer room");
    REQUIRE(refusalOf(pool) == ResolveExitRefusal::AllocCapacity);
    // The top of the fixed buffer is G+0x3A44 once set, else its end rounded down to 4.
    auto fixed     = entry;
    fixed.poolBase = end + 164 + kSideBuffer - 0x1000;
    fixed.poolSize = 0x1003;
    UNSCOPED_INFO("the buffer end is the rounded size plus the base");
    REQUIRE(refusalOf(fixed) == ResolveExitRefusal::None);
    fixed.poolSize = 0x0FFF;
    UNSCOPED_INFO("a size that rounds down short of the room");
    REQUIRE(refusalOf(fixed) == ResolveExitRefusal::AllocCapacity);
    // Alignment: a top that is not a multiple of 4 lowers the buffer to a multiple of 4 and the pool by 24.
    auto ragged           = entry;
    ragged.poolPos        = end + 164 + kSideBuffer + 3;
    const auto raggedExit = DeriveResolveExitState(effects, ragged);
    UNSCOPED_INFO("an unaligned top stores the aligned buffer");
    REQUIRE((raggedExit && raggedExit->poolPos == end + 164 && raggedExit->limit38 == ragged.limit38 - kSideBuffer));

    // The threshold: the side buffer count after this call may not pass G+0x3A58, or the retarget makes the next ring check flush.
    auto count     = entry;
    count.poolUsed = count.poolThreshold - kSideBuffer;
    UNSCOPED_INFO("the count reaches the threshold exactly");
    REQUIRE(refusalOf(count) == ResolveExitRefusal::None);
    count.poolUsed += 1;
    UNSCOPED_INFO("the count passes the threshold");
    REQUIRE(refusalOf(count) == ResolveExitRefusal::RingSpace);
    // A wrapped sum is below the threshold in Original too.
    count.poolUsed = 0xFFFFFFF0;
    UNSCOPED_INFO("the count wraps as the guest's 32-bit add does");
    REQUIRE(refusalOf(count) == ResolveExitRefusal::None);

    // The allocator path the host does not model.
    auto callback     = entry;
    callback.poolBase = 0;
    UNSCOPED_INFO("a pool object without a fixed buffer is 0x826A6AB8");
    REQUIRE(refusalOf(callback) == ResolveExitRefusal::AllocPath);
    // A null pool object is 0x8269B618, modeled (testNullPool).
    auto none = entry;
    nullPoolInto(none);
    UNSCOPED_INFO("a null pool object is modeled");
    REQUIRE(refusalOf(none) == ResolveExitRefusal::None);
}

ResolveNullPoolAlloc modelOf(const std::function<void(ResolveWatch&)>& change)
{
    auto watch = handEntry({ 0, 0, 0 });
    nullPoolInto(watch);
    change(watch);
    return ModelNullPoolAllocator(watch);
}

// Allocator model covers stores, alignment, brackets and 0x8269B618 waits.
void testNullPoolAllocator()
{
    // Generated evidence: g185:12899-13183 and g206:12440-12459; forward start
    // is G+0x34+60.
    {
        const auto take = modelOf([](ResolveWatch&)
                                  {
                                  });
        UNSCOPED_INFO(std::format("forward take from G+0x34 + 60: end {:08X}", take.end));
        REQUIRE((take.ok && !take.wrap && !take.waits && take.end == kNullEnd));
    }
    // Forward from G+0x3A44: aligned up to 4, then 24 bytes.
    for (const std::uint32_t start : { 0x00204000U, 0x00204001U, 0x00204002U, 0x00204003U, 0x00204004U })
    {
        const auto take = modelOf([&](ResolveWatch& w)
                                  {
                                      w.poolPos = start;
                                  });
        const auto want = ((start + 3) & ~3U) + 24;
        UNSCOPED_INFO(std::format("forward take from {:08X}: end {:08X} vs {:08X}", start, take.end, want));
        REQUIRE((take.ok && !take.wrap && take.end == want));
    }
    // The end at G+0x3A3C stays forward; a byte past it wraps.
    {
        const auto fits = modelOf([](ResolveWatch& w)
                                  {
                                      w.poolPos = 0x00204000;
                                      w.sideEnd = 0x00204018;
                                  });
        UNSCOPED_INFO("the end equal to G+0x3A3C is forward");
        REQUIRE((fits.ok && !fits.wrap && fits.end == 0x00204018));
        const auto past = modelOf([](ResolveWatch& w)
                                  {
                                      w.poolPos = 0x00204000;
                                      w.sideEnd = 0x00204017;
                                  });
        UNSCOPED_INFO(std::format("a byte past G+0x3A3C wraps: end {:08X}", past.end));
        REQUIRE((past.ok && past.wrap && past.end == kWrapPos + 24));
    }
    // Wrap: from G+0x3A4C aligned up, 24 bytes, no more than G+0x3A50.
    for (const std::uint32_t at : { 0x00204200U, 0x00204201U, 0x00204202U, 0x00204203U })
    {
        const auto take = modelOf([&](ResolveWatch& w)
                                  {
                                      w.sideEnd  = 0;
                                      w.wrapPos  = at;
                                      w.progress = kSegment + 1; // the wrap waits on the next segment
                                  });
        const auto want = ((at + 3) & ~3U) + 24;
        UNSCOPED_INFO(std::format("wrap take from {:08X}: end {:08X} vs {:08X}", at, take.end, want));
        REQUIRE((take.ok && take.wrap && take.end == want));
    }
    {
        const auto atLimit = modelOf([](ResolveWatch& w)
                                     {
                                         w.sideEnd   = 0;
                                         w.wrapLimit = kWrapPos + 24;
                                         w.progress  = kSegment + 1;
                                     });
        UNSCOPED_INFO("the wrap end equal to G+0x3A50 is taken");
        REQUIRE((atLimit.ok && atLimit.wrap && atLimit.end == kWrapPos + 24));
        const auto past = modelOf([](ResolveWatch& w)
                                  {
                                      w.sideEnd   = 0;
                                      w.wrapLimit = kWrapPos + 23;
                                      w.progress  = kSegment + 1;
                                  });
        UNSCOPED_INFO("the wrap end a byte past G+0x3A50 fails");
        REQUIRE(!past.ok);
        // An unaligned position rounds up before the limit test.
        const auto ragged = modelOf([](ResolveWatch& w)
                                    {
                                        w.sideEnd   = 0;
                                        w.wrapPos   = kWrapPos + 1;
                                        w.wrapLimit = kWrapPos + 27;
                                        w.progress  = kSegment + 1;
                                    });
        UNSCOPED_INFO("the aligned wrap end counts against G+0x3A50");
        REQUIRE(!ragged.ok);
    }
    // The 32-bit sum wraps as the guest's: an end that passes 2^32 is small and forward.
    {
        const auto take = modelOf([](ResolveWatch& w)
                                  {
                                      w.poolPos = 0xFFFFFFF0;
                                  });
        UNSCOPED_INFO(std::format("a 32-bit wrapped end {:08X}", take.end));
        REQUIRE((take.ok && !take.wrap && take.end == 8));
        // A buffer address that wraps to zero is a failure to the caller.
        for (const std::uint32_t start : { 0xFFFFFFFDU, 0xFFFFFFFEU, 0xFFFFFFFFU })
        {
            UNSCOPED_INFO(std::format("a buffer at address zero from {:08X} fails", start));
            REQUIRE(!modelOf([&](ResolveWatch& w)
                             {
                                 w.poolPos = start;
                             })
                         .ok);
        }
    }

    // The wait 0x8269B158(G, end, segment): a segment equal to the GPU's, or one ahead
    // with the end inside the GPU's block, returns at once.
    {
        const auto waits = [](std::uint32_t progress, std::uint32_t poolPos = 0)
        {
            return modelOf([&](ResolveWatch& w)
                           {
                               w.progress = progress;
                               w.poolPos  = poolPos;
                           })
                .waits;
        };
        UNSCOPED_INFO("the same segment does not wait");
        REQUIRE(!waits(kSegment));
        UNSCOPED_INFO("only the low two bits of the difference count");
        REQUIRE((!waits(kSegment + 4) && !waits(kSegment - 4) && !waits(0x1001)));
        // n - v == 1 (v & 3 == 0 for n == 5): the end must be at or below v & ~3.
        // The end is a multiple of 4, so v = end is the tightest block.
        UNSCOPED_INFO("one ahead: the end inside the block of v");
        REQUIRE((!waits(kNullEnd) && waits(kNullEnd - 4)));
        UNSCOPED_INFO("one ahead with the end below the block");
        REQUIRE(!waits(kNullEnd + 4));
        // n - v == 2 and 3 wait whatever v & ~3 is.
        UNSCOPED_INFO("two or three ahead wait");
        REQUIRE((waits(kSegment - 2) && waits(kSegment - 3) && waits(0xFFFFFFFE) && waits(0xFFFFFFFF)));
        // A GPU one segment ahead is a difference of 3 and waits even with a large block.
        UNSCOPED_INFO("a GPU one segment ahead waits");
        REQUIRE((waits(kSegment + 1) && waits(kSegment + 1 + 0x300000)));
    }
    // The wrap waits on the next segment: progress == n + 1 is level for it and behind for a forward take.
    {
        const auto wrapLevel = modelOf([](ResolveWatch& w)
                                       {
                                           w.sideEnd  = 0;
                                           w.progress = kSegment + 1;
                                       });
        UNSCOPED_INFO("a wrap is level with progress n + 1");
        REQUIRE((wrapLevel.ok && wrapLevel.wrap && !wrapLevel.waits));
        const auto wrapBehind = modelOf([](ResolveWatch& w)
                                        {
                                            w.sideEnd = 0;
                                        });
        UNSCOPED_INFO("a wrap with progress n waits");
        REQUIRE((wrapBehind.ok && wrapBehind.wrap && wrapBehind.waits));
        UNSCOPED_INFO("a forward take with progress n + 1 waits");
        REQUIRE(modelOf([](ResolveWatch& w)
                        {
                            w.progress = kSegment + 1;
                        })
                    .waits);
        // One ahead for a wrap: n + 1 - v == 1 needs the end inside v & ~3.
        const auto wrapAhead = [](std::uint32_t progress)
        {
            return modelOf([&](ResolveWatch& w)
                           {
                               w.sideEnd  = 0;
                               w.progress = progress;
                           })
                .waits;
        };
        UNSCOPED_INFO("a wrap one ahead: the end inside the block of v");
        REQUIRE((!wrapAhead(kWrapPos + 25) && wrapAhead(kWrapPos + 21)));
    }
}

// G+0x33B0 gates G+0x33B4 waits only for bracket ResolveTiling.
void testNullPoolBracket()
{
    // Generated bracket waits: g185:12955-12976 and g185:12990-13021.
    const auto bracketed = [](std::uint32_t bracket, std::uint32_t next, bool wrap, std::uint32_t wrapLimit = kWrapLimit)
    {
        return modelOf([&](ResolveWatch& w)
                       {
                           w.bracket     = bracket;
                           w.bracketNext = next;
                           w.wrapLimit   = wrapLimit;
                           if (wrap)
                               w.sideEnd = 0;
                           w.progress = kSegment + (wrap ? 1 : 0);
                       });
    };
    // Forward, difference d = G+0x33B4 - G+0x3A48.
    UNSCOPED_INFO("forward: d == -1 fails");
    REQUIRE(!bracketed(0x00300000, kSegment - 1, false).ok);
    UNSCOPED_INFO("forward: d == 0 with G+0x33B0 below the end fails");
    REQUIRE(!bracketed(kNullEnd - 1, kSegment, false).ok);
    UNSCOPED_INFO("forward: d == 0 with G+0x33B0 equal to the end is taken");
    REQUIRE(bracketed(kNullEnd, kSegment, false).ok);
    UNSCOPED_INFO("forward: d == 0 with G+0x33B0 above the end is taken");
    REQUIRE(bracketed(kNullEnd + 4, kSegment, false).ok);
    UNSCOPED_INFO("forward: d == 1 is taken whatever G+0x33B0 is");
    REQUIRE(bracketed(1, kSegment + 1, false).ok);
    UNSCOPED_INFO("forward: d == -2 is taken (only -1 and 0 are tested)");
    REQUIRE(bracketed(1, kSegment - 2, false).ok);
    UNSCOPED_INFO("forward: a negative d other than -1 is taken");
    REQUIRE(bracketed(1, kSegment + 0x80000000U, false).ok);
    // With the bracket clear the words are not looked at.
    UNSCOPED_INFO("forward: G+0x33B4 is not read without the bracket");
    REQUIRE(modelOf([](ResolveWatch& w)
                    {
                        w.bracketNext = kSegment - 1;
                    })
                .ok);

    // Wrap, difference d = G+0x33B4 - G+0x3A48 - 1.
    UNSCOPED_INFO("wrap: d == -1 (G+0x33B4 equal to the segment) fails");
    REQUIRE(!bracketed(0x00300000, kSegment, true).ok);
    UNSCOPED_INFO("wrap: d == 0 with G+0x33B0 below the end fails");
    REQUIRE(!bracketed(kWrapPos + 23, kSegment + 1, true).ok);
    UNSCOPED_INFO("wrap: d == 0 with G+0x33B0 equal to the end is taken");
    REQUIRE(bracketed(kWrapPos + 24, kSegment + 1, true).ok);
    UNSCOPED_INFO("wrap: d == 1 is taken");
    REQUIRE(bracketed(1, kSegment + 2, true).ok);
    UNSCOPED_INFO("wrap: G+0x33B4 less than the segment is d == -2 and taken");
    REQUIRE(bracketed(1, kSegment - 1, true).ok);
    // The wrap limit is judged after the bracket and fails either way.
    UNSCOPED_INFO("wrap: the limit still fails with the bracket satisfied");
    REQUIRE(!bracketed(1, kSegment + 2, true, kWrapPos + 23).ok);
}

ResolveWatch nullEntry(const std::function<void(ResolveWatch&)>& change = [](ResolveWatch&)
                       {
                       })
{
    auto watch = handEntry({ 0, 0, 0 });
    nullPoolInto(watch);
    change(watch);
    return watch;
}

// The stores every modeled call of handEntry({ 0, 0, 0 }) makes (the worked
// "clean_entry" exit), before its allocator words.
std::vector<ResolveWrite> cleanWrites()
{
    return {
        { kG + 0x2A18, 0, false },
        { kG + 0x2A24, 0, false },
        { kG + 0x10, 0x00001380, false },
        { kG + 0x14, 0x000989E8, false },
        { kG + 0x18, 0x00000008, false },
        { kG + 0x1C, 0x80000000, false },
        { kG + 0x24, 0x00080000, false },
        { kG + 0x2ABE, 0x07, true },
        { kHeader + 8, 0x1234, false },
        { kHeader + 12, 0x1234, false },
    };
}

// Null-pool exit does not lower ring limits; refusals keep order.
void testNullPoolExit()
{
    const auto effects = handEffects(false);
    const auto fixed   = *DeriveResolveExitState(effects, handEntry({ 0, 0, 0 }));

    // Forward: G+0x3A44 takes the end, G+0x3A54 grows by 24, nothing else of the pool moves.
    {
        const auto entry = nullEntry();
        const auto exit  = DeriveResolveExitState(effects, entry);
        UNSCOPED_INFO("the null pool entry is modeled");
        REQUIRE(exit.has_value());
        UNSCOPED_INFO("forward: G+0x3A44 and G+0x3A54");
        REQUIRE((exit->poolPos == kNullEnd && exit->wrapPos == entry.wrapPos && exit->poolUsed == kPoolUsed + kSideBuffer));
        UNSCOPED_INFO("the null pool allocator lowers neither limit");
        REQUIRE((exit->limit34 == kLimit34 && exit->limit38 == kLimit38));
        UNSCOPED_INFO("every other word is the fixed pool's");
        REQUIRE((exit->copy == fixed.copy && exit->dirty == fixed.dirty && exit->size == fixed.size && exit->stateByte == fixed.stateByte && exit->dest8 == 0x1234 && exit->dest12 == 0x1234));
        auto want = cleanWrites();
        want.push_back({ kG + 0x3A44, kNullEnd, false });
        want.push_back({ kG + 0x3A54, kPoolUsed + kSideBuffer, false });
        UNSCOPED_INFO("forward writes exactly G+0x3A44 and G+0x3A54 beside the common stores");
        REQUIRE(ResolveExitWrites(kG, kHeader, entry, *exit) == want);
    }
    // Wrap: G+0x3A4C takes the end; G+0x3A44 keeps its value and is not stored.
    {
        constexpr std::uint32_t kStart = 0x00204000;
        const auto              entry  = nullEntry([](ResolveWatch& w)
                                                   {
                                         w.poolPos  = kStart;
                                         w.sideEnd  = kStart + 23;
                                         w.progress = kSegment + 1;
                                                   });
        const auto              exit   = DeriveResolveExitState(effects, entry);
        UNSCOPED_INFO("the wrap entry is modeled");
        REQUIRE(exit.has_value());
        UNSCOPED_INFO("wrap: G+0x3A4C and G+0x3A54");
        REQUIRE((exit->poolPos == kStart && exit->wrapPos == kWrapPos + kSideBuffer && exit->poolUsed == kPoolUsed + kSideBuffer && exit->limit34 == kLimit34 && exit->limit38 == kLimit38));
        auto want = cleanWrites();
        want.push_back({ kG + 0x3A54, kPoolUsed + kSideBuffer, false });
        want.push_back({ kG + 0x3A4C, kWrapPos + kSideBuffer, false });
        UNSCOPED_INFO("wrap writes exactly G+0x3A4C and G+0x3A54, not G+0x3A44");
        REQUIRE(ResolveExitWrites(kG, kHeader, entry, *exit) == want);
    }

    const auto refusalOf = [&](const ResolveWatch& watch, std::string* why = nullptr)
    {
        ResolveExitRefusal refusal = ResolveExitRefusal::None;
        DeriveResolveExitState(effects, watch, &refusal, why);
        return refusal;
    };
    std::string why;
    // The ring bound: 302 words, judged against the limits as they stand (no 24-byte lowering).
    {
        const std::uint32_t end = kCursor + 4 * 302;
        UNSCOPED_INFO("the words end exactly at the soft limit, one byte below the hard limit");
        REQUIRE(refusalOf(nullEntry([&](ResolveWatch& w)
                                    {
                                        w.limit38 = end;
                                        w.limit34 = end + 1;
                                    })) == ResolveExitRefusal::None);
        UNSCOPED_INFO("a byte short of the soft limit");
        REQUIRE(refusalOf(nullEntry([&](ResolveWatch& w)
                                    {
                                        w.limit38 = end - 1;
                                        w.limit34 = end + 160;
                                    })) == ResolveExitRefusal::RingSpace);
        UNSCOPED_INFO("the words reach the hard limit");
        REQUIRE(refusalOf(nullEntry([&](ResolveWatch& w)
                                    {
                                        w.limit38 = end + 160;
                                        w.limit34 = end;
                                    })) == ResolveExitRefusal::RingSpace);
        UNSCOPED_INFO("a cursor whose words wrap");
        REQUIRE(refusalOf(nullEntry([](ResolveWatch& w)
                                    {
                                        w.cursor = 0xFFFFF000;
                                    })) == ResolveExitRefusal::RingSpace);
        UNSCOPED_INFO("an unbounded render target count");
        REQUIRE(refusalOf(nullEntry([](ResolveWatch& w)
                                    {
                                        w.targetCount = 17;
                                    })) == ResolveExitRefusal::RingSpace);
    }
    // The threshold: the count after this call may not pass G+0x3A58 unless the bracket is set.
    {
        UNSCOPED_INFO("the count reaches the threshold exactly");
        REQUIRE(refusalOf(nullEntry([](ResolveWatch& w)
                                    {
                                        w.poolUsed = w.poolThreshold - kSideBuffer;
                                    })) == ResolveExitRefusal::None);
        UNSCOPED_INFO("the count passes the threshold");
        REQUIRE((refusalOf(nullEntry([](ResolveWatch& w)
                                     {
                                         w.poolUsed = w.poolThreshold - kSideBuffer + 1;
                                     }),
                           &why) == ResolveExitRefusal::RingSpace &&
                 why == "pool_threshold"));
        UNSCOPED_INFO("the count wraps as the guest's 32-bit add does");
        REQUIRE(refusalOf(nullEntry([](ResolveWatch& w)
                                    {
                                        w.poolUsed = 0xFFFFFFF0;
                                    })) == ResolveExitRefusal::None);
        // The bracket skips the forced flush (g93:12840-12845); on the exit state this is
        // the bracket form of testTiledExit.
        UNSCOPED_INFO("past the threshold with the bracket set there is no forced flush");
        REQUIRE(refusalOf(nullEntry([](ResolveWatch& w)
                                    {
                                        w.poolUsed    = w.poolThreshold;
                                        w.tilingByte  = 0x20;
                                        w.bracket     = 0x00300000;
                                        w.bracketNext = kSegment + 1;
                                    })) == ResolveExitRefusal::None);
    }
    // A failed allocation, and a would-be wait.
    UNSCOPED_INFO("a wrap past its limit fails");
    REQUIRE((refusalOf(nullEntry([](ResolveWatch& w)
                                 {
                                     w.sideEnd   = 0;
                                     w.wrapLimit = kWrapPos;
                                     w.progress  = kSegment + 1;
                                 }),
                       &why) == ResolveExitRefusal::AllocCapacity &&
             why == "alloc_fail"));
    UNSCOPED_INFO("a bracket failure is a failed allocation");
    REQUIRE(refusalOf(nullEntry([](ResolveWatch& w)
                                {
                                    w.tilingByte  = 0x20;
                                    w.bracket     = 0x00300000;
                                    w.bracketNext = kSegment - 1;
                                })) == ResolveExitRefusal::AllocCapacity);
    UNSCOPED_INFO("a GPU two segments behind makes the allocator wait");
    REQUIRE((refusalOf(nullEntry([](ResolveWatch& w)
                                 {
                                     w.progress = kSegment - 2;
                                 }),
                       &why) == ResolveExitRefusal::AllocWait &&
             why == "alloc_wait"));
    // A pool object without a fixed buffer is still the indirect allocator.
    UNSCOPED_INFO("a pool object without a fixed buffer is alloc_path");
    REQUIRE(refusalOf(nullEntry([](ResolveWatch& w)
                                {
                                    w.poolObject = kPoolObject;
                                })) == ResolveExitRefusal::AllocPath);
    // A tiled shape the model leaves out is refused before the allocator is judged.
    UNSCOPED_INFO("a predicated device is refused first");
    REQUIRE(refusalOf(nullEntry([](ResolveWatch& w)
                                {
                                    w.predicate = 1;
                                    w.bracket   = 0x00300000;
                                    w.progress  = kSegment - 2;
                                })) == ResolveExitRefusal::TilingPredicate);

    // The order after tiling: the ring bound, the threshold, the allocation, the wait.
    {
        auto all = nullEntry([](ResolveWatch& w)
                             {
                                 w.cursor   = w.limit38 - 16;
                                 w.poolUsed = w.poolThreshold;
                                 w.progress = kSegment - 2;
                             });
        UNSCOPED_INFO("the ring bound first");
        REQUIRE((refusalOf(all, &why) == ResolveExitRefusal::RingSpace && why == "ring_space"));
        all.cursor = kCursor;
        UNSCOPED_INFO("then the threshold");
        REQUIRE((refusalOf(all, &why) == ResolveExitRefusal::RingSpace && why == "pool_threshold"));
        all.poolUsed  = kPoolUsed;
        all.sideEnd   = 0;
        all.wrapLimit = kWrapPos;
        UNSCOPED_INFO("then the allocation");
        REQUIRE(refusalOf(all, &why) == ResolveExitRefusal::AllocCapacity);
        all.wrapLimit = kWrapLimit;
        UNSCOPED_INFO("then the wait");
        REQUIRE(refusalOf(all, &why) == ResolveExitRefusal::AllocWait);
        all.progress = kSegment + 1;
        UNSCOPED_INFO("and nothing else");
        REQUIRE(refusalOf(all, &why) == ResolveExitRefusal::None);
    }
}

// Device tiling words classify order bit 0x10, predicate, and bracket bit 0x20.
void testTilingShapes()
{
    struct Shape
    {
        std::uint8_t  byte;
        std::uint32_t predicate, bracket;
        ResolveTiling want;
    };

    constexpr std::uint32_t kB       = 0x00300000;
    const Shape             shapes[] = {
        { 0x00, 0, 0, ResolveTiling::None },
        { 0xCF, 0, 0, ResolveTiling::None }, // bits other than 0x30 are not tiling
        { 0x01, 0, 0, ResolveTiling::None },
        { 0x20, 0, kB, ResolveTiling::Bracket },
        { 0x21, 0, kB, ResolveTiling::Bracket },
        { 0x61, 0, 0x80000000U, ResolveTiling::Bracket },
        { 0xEF & 0x2F, 0, 1, ResolveTiling::Bracket },
        { 0x20, 0, 0, ResolveTiling::State }, // byte without the bracket word
        { 0x21, 0, 0, ResolveTiling::State },
        { 0x00, 0, kB, ResolveTiling::State }, // bracket word without the byte
        { 0x01, 0, kB, ResolveTiling::State },
        { 0x10, 0, 0, ResolveTiling::Byte },
        { 0x10, 0, kB, ResolveTiling::Byte },
        { 0x30, 0, kB, ResolveTiling::Byte },
        { 0x30, 1, kB, ResolveTiling::Byte }, // bit 0x10 before the predicate
        { 0x20, 1, kB, ResolveTiling::PredicatedBracket },
        { 0x21, 1, kB, ResolveTiling::PredicatedBracket },
        { 0x61, 0x0C, 0x80000000U, ResolveTiling::PredicatedBracket },
        { 0xEF & 0x2F, 0x80000000U, 1, ResolveTiling::PredicatedBracket },
        { 0x00, 1, 0, ResolveTiling::Predicate },
        { 0x20, 1, 0, ResolveTiling::Predicate }, // the predicate before the disagreement
        { 0x01, 0x80000000U, kB, ResolveTiling::Predicate },
        { 0x00, 1, kB, ResolveTiling::Predicate }, // the bracket word without the byte is not an open bracket
    };
    for (const auto& shape : shapes)
    {
        UNSCOPED_INFO(std::format("byte {:02X} predicate {:X} bracket {:X}", shape.byte, shape.predicate, shape.bracket));
        REQUIRE(ClassifyResolveTiling(shape.byte, shape.predicate, shape.bracket) == shape.want);
    }

    // Each shape through the exit state: its own refusal, with its own reason.
    const auto effects = handEffects(false);

    const struct
    {
        ResolveTiling      shape;
        ResolveExitRefusal refusal;
        const char*        why;
    } refusals[] = {
        { ResolveTiling::Byte, ResolveExitRefusal::Tiling, "tiling_byte" },
        { ResolveTiling::Predicate, ResolveExitRefusal::TilingPredicate, "tiling_predicate" },
        { ResolveTiling::State, ResolveExitRefusal::TilingState, "tiling_state" },
    };

    for (const auto& want : refusals)
    {
        for (const auto& shape : shapes)
        {
            if (shape.want != want.shape)
                continue;
            auto entry                 = nullEntry();
            entry.tilingByte           = shape.byte;
            entry.predicate            = shape.predicate;
            entry.bracket              = shape.bracket;
            ResolveExitRefusal refusal = ResolveExitRefusal::None;
            std::string        why;
            UNSCOPED_INFO(std::format("byte {:02X} predicate {:X} bracket {:X}: {}", shape.byte, shape.predicate, shape.bracket, why));
            REQUIRE((!DeriveResolveExitState(effects, entry, &refusal, &why) && refusal == want.refusal && why == want.why));
        }
    }
    UNSCOPED_INFO("the refusal names");
    REQUIRE((std::string(kResolveExitRefusalNames[static_cast<std::size_t>(ResolveExitRefusal::Tiling)]) == "tiling" &&
             std::string(kResolveExitRefusalNames[static_cast<std::size_t>(ResolveExitRefusal::TilingPredicate)]) == "tiling_predicate" &&
             std::string(kResolveExitRefusalNames[static_cast<std::size_t>(ResolveExitRefusal::TilingState)]) == "tiling_state"));
}

// Tiled exit adds only the allocator's bracket branch to the untiled exit.
void testTiledExit()
{
    const auto effects = handEffects(false);
    // A forward take with G+0x33B4 one segment ahead of G+0x3A48 (d == 1).
    const auto bracketEntry = [](const std::function<void(ResolveWatch&)>& change = [](ResolveWatch&)
                                 {
                                 })
    {
        return nullEntry([&](ResolveWatch& w)
                         {
                             w.tilingByte  = 0x21;
                             w.bracket     = 0x00300000;
                             w.bracketNext = kSegment + 1;
                             w.targetCount = 2;
                             change(w);
                         });
    };
    const auto refusalOf = [&](const ResolveWatch& watch, std::string* why = nullptr)
    {
        ResolveExitRefusal refusal = ResolveExitRefusal::None;
        DeriveResolveExitState(effects, watch, &refusal, why);
        return refusal;
    };

    // The exit is the untiled null pool exit: same words, same stores, no store for the tiling.
    {
        const auto tiled   = bracketEntry();
        const auto exit    = DeriveResolveExitState(effects, tiled);
        const auto untiled = DeriveResolveExitState(effects, nullEntry([](ResolveWatch& w)
                                                                       {
                                                                           w.targetCount = 2;
                                                                       }));
        UNSCOPED_INFO("the bracket form is modeled");
        REQUIRE((exit && untiled));
        UNSCOPED_INFO("every word equals the untiled exit");
        REQUIRE((exit->copy == untiled->copy && exit->dirty == untiled->dirty && exit->size == untiled->size && exit->allocByte == untiled->allocByte && exit->stateByte == untiled->stateByte &&
                 exit->dest8 == untiled->dest8 && exit->dest12 == untiled->dest12 && exit->limit34 == untiled->limit34 && exit->limit38 == untiled->limit38 &&
                 exit->poolPos == untiled->poolPos && exit->wrapPos == untiled->wrapPos && exit->poolUsed == untiled->poolUsed));
        UNSCOPED_INFO("forward: G+0x3A44 and G+0x3A54");
        REQUIRE((exit->poolPos == kNullEnd && exit->wrapPos == tiled.wrapPos && exit->poolUsed == kPoolUsed + kSideBuffer));
        auto want = cleanWrites();
        want.push_back({ kG + 0x3A44, kNullEnd, false });
        want.push_back({ kG + 0x3A54, kPoolUsed + kSideBuffer, false });
        UNSCOPED_INFO("the writes are the untiled forward writes: none to the tiling byte, G+0x31xx, G+0x33xx or G+0x3A48");
        REQUIRE(ResolveExitWrites(kG, kHeader, tiled, *exit) == want);
    }
    // The wrap of a bracket call stores only G+0x3A4C; the segment is one on.
    {
        const auto tiled = bracketEntry([](ResolveWatch& w)
                                        {
                                            w.poolPos     = 0x00204000;
                                            w.sideEnd     = 0x00204000;
                                            w.bracketNext = kSegment + 2;
                                            w.progress    = kSegment + 1;
                                        });
        const auto exit  = DeriveResolveExitState(effects, tiled);
        UNSCOPED_INFO("wrap: G+0x3A4C only");
        REQUIRE((exit && exit->poolPos == 0x00204000 && exit->wrapPos == kWrapPos + kSideBuffer));
        auto want = cleanWrites();
        want.push_back({ kG + 0x3A54, kPoolUsed + kSideBuffer, false });
        want.push_back({ kG + 0x3A4C, kWrapPos + kSideBuffer, false });
        UNSCOPED_INFO("wrap writes exactly G+0x3A4C and G+0x3A54");
        REQUIRE(ResolveExitWrites(kG, kHeader, tiled, *exit) == want);
    }

    // ResolveMaxRingWords uses n=G+0x31CC: 2*(32+5n)+2*(12+7n)+(6+5n)+85 plus 7
    // G+0x20 words; it yields 244 for n=2 and 273 for n=3.
    // Loop sources: g202:13207-13297, g153:12839-12994, g167:13966-14126.
    UNSCOPED_INFO(std::format("n == 2 words {}", ResolveMaxRingWords(bracketEntry())));
    REQUIRE(ResolveMaxRingWords(bracketEntry()) == 244);
    UNSCOPED_INFO("n == 3 words");
    REQUIRE(ResolveMaxRingWords(bracketEntry([](ResolveWatch& w)
                                             {
                                                 w.targetCount = 3;
                                             })) == 273);
    {
        const std::uint32_t end = kCursor + 4 * 244;
        UNSCOPED_INFO("a bracket call whose 244 words end at the soft limit");
        REQUIRE(refusalOf(bracketEntry([&](ResolveWatch& w)
                                       {
                                           w.limit38 = end;
                                           w.limit34 = end + 1;
                                       })) == ResolveExitRefusal::None);
        UNSCOPED_INFO("a byte short of the soft limit");
        REQUIRE(refusalOf(bracketEntry([&](ResolveWatch& w)
                                       {
                                           w.limit38 = end - 1;
                                           w.limit34 = end + 160;
                                       })) == ResolveExitRefusal::RingSpace);
        UNSCOPED_INFO("the words reach the hard limit");
        REQUIRE(refusalOf(bracketEntry([&](ResolveWatch& w)
                                       {
                                           w.limit38 = end + 160;
                                           w.limit34 = end;
                                       })) == ResolveExitRefusal::RingSpace);
        UNSCOPED_INFO("a tile count beyond the loops' bound");
        REQUIRE(refusalOf(bracketEntry([](ResolveWatch& w)
                                       {
                                           w.targetCount = 17;
                                       })) == ResolveExitRefusal::RingSpace);
    }

    // The allocator's bracket branch, through the exit state.
    std::string why;
    UNSCOPED_INFO("forward: G+0x33B4 one segment behind fails");
    REQUIRE((refusalOf(bracketEntry([](ResolveWatch& w)
                                    {
                                        w.bracketNext = kSegment - 1;
                                    }),
                       &why) == ResolveExitRefusal::AllocCapacity &&
             why == "alloc_fail"));
    UNSCOPED_INFO("forward: the same segment with G+0x33B0 below the end fails");
    REQUIRE(refusalOf(bracketEntry([](ResolveWatch& w)
                                   {
                                       w.bracketNext = kSegment;
                                       w.bracket     = kNullEnd - 1;
                                   })) == ResolveExitRefusal::AllocCapacity);
    UNSCOPED_INFO("forward: the same segment with G+0x33B0 at the end is taken");
    REQUIRE(refusalOf(bracketEntry([](ResolveWatch& w)
                                   {
                                       w.bracketNext = kSegment;
                                       w.bracket     = kNullEnd;
                                   })) == ResolveExitRefusal::None);
    UNSCOPED_INFO("wrap: G+0x33B4 at the segment fails");
    REQUIRE(refusalOf(bracketEntry([](ResolveWatch& w)
                                   {
                                       w.sideEnd     = 0;
                                       w.bracketNext = kSegment;
                                       w.progress    = kSegment + 1;
                                   })) == ResolveExitRefusal::AllocCapacity);
    UNSCOPED_INFO("wrap: one segment ahead with G+0x33B0 below the end fails");
    REQUIRE(refusalOf(bracketEntry([](ResolveWatch& w)
                                   {
                                       w.sideEnd     = 0;
                                       w.bracketNext = kSegment + 1;
                                       w.bracket     = kWrapPos + 23;
                                       w.progress    = kSegment + 1;
                                   })) == ResolveExitRefusal::AllocCapacity);
    UNSCOPED_INFO("wrap: one segment ahead with G+0x33B0 at the end is taken");
    REQUIRE(refusalOf(bracketEntry([](ResolveWatch& w)
                                   {
                                       w.sideEnd     = 0;
                                       w.bracketNext = kSegment + 1;
                                       w.bracket     = kWrapPos + 24;
                                       w.progress    = kSegment + 1;
                                   })) == ResolveExitRefusal::None);
    // The wait is the same as without the bracket.
    UNSCOPED_INFO("a bracket call can wait too");
    REQUIRE(refusalOf(bracketEntry([](ResolveWatch& w)
                                   {
                                       w.progress = kSegment - 2;
                                   })) == ResolveExitRefusal::AllocWait);
    // Past the pool threshold the forced flush needs the bracket word clear: a bracket call is admitted
    // where the same count without the bracket is ring_space.
    UNSCOPED_INFO("past the threshold a bracket call has no forced flush");
    REQUIRE(refusalOf(bracketEntry([](ResolveWatch& w)
                                   {
                                       w.poolUsed = w.poolThreshold;
                                   })) == ResolveExitRefusal::None);
    UNSCOPED_INFO("the same count without the bracket flushes");
    REQUIRE((refusalOf(nullEntry([](ResolveWatch& w)
                                 {
                                     w.poolUsed = w.poolThreshold;
                                 }),
                       &why) == ResolveExitRefusal::RingSpace &&
             why == "pool_threshold"));

    // The tiling tests come before the allocator's and the clear flags and fence before them.
    UNSCOPED_INFO("a predicate outside an open bracket is refused before alloc_path");
    REQUIRE(refusalOf(bracketEntry([](ResolveWatch& w)
                                   {
                                       w.predicate  = 1;
                                       w.bracket    = 0;
                                       w.poolObject = kPoolObject;
                                   })) == ResolveExitRefusal::TilingPredicate);
    UNSCOPED_INFO("the allocation marker is refused before tiling");
    REQUIRE(refusalOf(bracketEntry([](ResolveWatch& w)
                                   {
                                       w.allocByte = 0x20;
                                       w.predicate = 1;
                                       w.bracket   = 0;
                                   })) == ResolveExitRefusal::AllocMarker);

    // A pool object with a fixed buffer: the fixed allocator has no bracket terms, so the exit is the untiled one; only the forced flush differs.
    {
        auto       fixed = handEntry({ 0, 0, 0 });
        const auto plain = DeriveResolveExitState(effects, fixed);
        fixed.tilingByte = 0x21;
        fixed.bracket    = 0x00300000;
        const auto exit  = DeriveResolveExitState(effects, fixed);
        UNSCOPED_INFO("a fixed pool under a bracket takes its buffer as untiled");
        REQUIRE((plain && exit && exit->poolPos == plain->poolPos && exit->limit34 == plain->limit34 && exit->limit38 == plain->limit38 && exit->poolUsed == plain->poolUsed));
        fixed.poolUsed  = fixed.poolThreshold;
        const auto past = DeriveResolveExitState(effects, fixed);
        UNSCOPED_INFO("and has no forced flush past the threshold");
        REQUIRE((past && past->poolUsed == fixed.poolThreshold + kSideBuffer && past->poolPos == plain->poolPos));
        fixed.tilingByte = 0;
        fixed.bracket    = 0;
        UNSCOPED_INFO("where the same count untiled still flushes");
        REQUIRE((refusalOf(fixed, &why) == ResolveExitRefusal::RingSpace && why == "pool_threshold"));
    }

    // From memory: the byte, the bracket word and the predicate are read from G.
    {
        const auto  entry  = bracketEntry();
        const auto  memory = withWatch(build({ .name = "r", .shadowHit = true }), entry);
        std::string why2;
        const auto  exit = DeriveResolveExitState(readerOf(memory), registers(0), &why2);
        UNSCOPED_INFO("the bracket form from memory: " + why2);
        REQUIRE((memory.at(kG + 0x2ABC) >> 24 == 0x21 && exit && exit->poolPos == kNullEnd));
        auto pred             = memory;
        pred[kG + 0x31AC]     = 1;
        const auto predicated = DeriveResolveExitState(readerOf(pred), registers(0), &why2);
        UNSCOPED_INFO("the predicate word inside the bracket: " + why2);
        REQUIRE((predicated && predicated->poolPos == kNullEnd && predicated->wrapPos == exit->wrapPos && predicated->poolUsed == exit->poolUsed));
        pred[kG + 0x33B0] = 0;
        pred[kG + 0x2ABC] &= 0x00FFFFFF;
        UNSCOPED_INFO("the predicate word without a bracket: " + why2);
        REQUIRE((!DeriveResolveExitState(readerOf(pred), registers(0), &why2) && why2 == "tiling_predicate"));
        auto flagged = memory;
        flagged[kG + 0x2ABC] |= 0x10000000;
        UNSCOPED_INFO("the byte with bit 0x10: " + why2);
        REQUIRE((!DeriveResolveExitState(readerOf(flagged), registers(0), &why2) && why2 == "tiling_byte"));
        auto noBracket         = memory;
        noBracket[kG + 0x33B0] = 0;
        UNSCOPED_INFO("the byte without the bracket word: " + why2);
        REQUIRE((!DeriveResolveExitState(readerOf(noBracket), registers(0), &why2) && why2 == "tiling_state"));
    }
}

#if REREVVED_DEVELOPER
// The probe comparison and counters for the bracket form.
void testTiledProbe()
{
    const auto effects = handEffects(false);
    const auto entry   = nullEntry([](ResolveWatch& w)
                                   {
                                     w.tilingByte  = 0x21;
                                     w.bracket     = 0x00300000;
                                     w.bracketNext = kSegment + 1;
                                     w.targetCount = 2;
                                   });
    const auto exit    = applied(effects, entry);
    auto       got     = CompareResolveEffects(effects, entry, exit);
    UNSCOPED_INFO("the bracket form exit is exact and marked tiled");
    REQUIRE((got.exactModeled && got.exactTiled && got.exactMismatch == 0 && got.exact.poolPos == kNullEnd));
    auto stale    = exit;
    stale.poolPos = 0;
    got           = CompareResolveEffects(effects, entry, stale);
    UNSCOPED_INFO("a bracket call whose G+0x3A44 was not stored");
    REQUIRE((got.exactModeled && got.exactTiled && got.exactMismatch == (1U << 14)));
    auto untiled       = entry;
    untiled.tilingByte = 0;
    untiled.bracket    = 0;
    UNSCOPED_INFO("an untiled call is not marked tiled");
    REQUIRE(!CompareResolveEffects(effects, untiled, exit).exactTiled);
    auto predicated      = entry;
    predicated.predicate = 1;
    got                  = CompareResolveEffects(effects, predicated, exit);
    UNSCOPED_INFO("a predicated call is modeled as the bracket form");
    REQUIRE((got.exactModeled && got.exactTiled && got.exactPredicated && got.exactMismatch == 0));
    auto loose    = predicated;
    loose.bracket = 0;
    got           = CompareResolveEffects(effects, loose, exit);
    UNSCOPED_INFO("a predicate without the bracket is skipped as tiling_predicate");
    REQUIRE((!got.exactModeled && !got.exactTiled && !got.exactPredicated && got.exactRefusal == ResolveExitRefusal::TilingPredicate));
    auto disagree    = entry;
    disagree.bracket = 0;
    got              = CompareResolveEffects(effects, disagree, exit);
    UNSCOPED_INFO("a disagreeing call is skipped as tiling_state");
    REQUIRE((!got.exactModeled && got.exactRefusal == ResolveExitRefusal::TilingState));
    auto flagged       = entry;
    flagged.tilingByte = 0x30;
    got                = CompareResolveEffects(effects, flagged, exit);
    UNSCOPED_INFO("bit 0x10 is skipped as tiling");
    REQUIRE((!got.exactModeled && got.exactRefusal == ResolveExitRefusal::Tiling));

    TakeResolveEffectCounts();
    NoteResolveEffect(CompareResolveEffects(effects, entry, exit));
    NoteResolveEffect(CompareResolveEffects(effects, entry, exit));
    NoteResolveEffect(CompareResolveEffects(effects, entry, stale));
    NoteResolveEffect(CompareResolveEffects(effects, untiled, exit));
    NoteResolveEffect(CompareResolveEffects(effects, predicated, exit));
    NoteResolveEffect(CompareResolveEffects(effects, loose, exit));
    NoteResolveEffect(CompareResolveEffects(effects, disagree, exit));
    const auto counts = TakeResolveEffectCounts();
    UNSCOPED_INFO("four modeled bracket calls, three exact");
    REQUIRE((counts.exactTiled == 4 && counts.exactTiledOk == 3));
    UNSCOPED_INFO("one of them predicated");
    REQUIRE((counts.exactPredicated == 1 && counts.exactPredicatedOk == 1));
    UNSCOPED_INFO("the untiled call counts apart from them");
    REQUIRE((counts.exactOk == 4 && counts.exactSkipped == 2));
    UNSCOPED_INFO("the skips by reason");
    REQUIRE((counts.exactRefused[static_cast<std::size_t>(ResolveExitRefusal::TilingPredicate)] == 1 && counts.exactRefused[static_cast<std::size_t>(ResolveExitRefusal::TilingState)] == 1));
    const auto line = FormatResolveEffectCounts(counts);
    UNSCOPED_INFO("the line: " + line);
    REQUIRE((line.contains(" resolve_effect_exact_ok=4 resolve_effect_exact_tiled=4 resolve_effect_exact_tiled_ok=3 resolve_effect_exact_predicated=1 resolve_effect_exact_predicated_ok=1 ") && line.contains(" resolve_effect_exact_skipped_tiling_predicate=1") &&
             line.contains(" resolve_effect_exact_skipped_tiling_state=1")));
}

// The reader overload, the probe comparison and the counters with a null pool object.
void testNullPoolProbe()
{
    const auto effects = handEffects(false);
    // From memory: the same exit as the watch, through the words the reader supplies.
    {
        const auto  entry  = nullEntry();
        const auto  memory = withWatch(build({ .name = "r", .shadowHit = true }), entry);
        const auto  read   = readerOf(memory);
        std::string why;
        const auto  exit = DeriveResolveExitState(read, registers(0), &why);
        UNSCOPED_INFO("the null pool exit from memory: " + why);
        REQUIRE((exit && exit->poolPos == kNullEnd && exit->wrapPos == kWrapPos && exit->poolUsed == kPoolUsed + kSideBuffer && exit->limit34 == kLimit34 && exit->limit38 == kLimit38));
        auto moved                = memory;
        moved[kProgressBlock + 4] = kSegment - 2;
        UNSCOPED_INFO("a progress word read through G+0x2A90 that makes it wait: " + why);
        REQUIRE((!DeriveResolveExitState(readerOf(moved), registers(0), &why) && why == "alloc_wait"));
        moved              = memory;
        moved[kG + 0x2A90] = 0;
        UNSCOPED_INFO("a null progress block is unreadable: " + why);
        REQUIRE((!DeriveResolveExitState(readerOf(moved), registers(0), &why) && why == "unreadable watched words"));
        moved = memory;
        moved.erase(kG + 0x3A50);
        UNSCOPED_INFO("an unreadable wrap limit: " + why);
        REQUIRE((!DeriveResolveExitState(readerOf(moved), registers(0), &why) && why == "unreadable watched words"));
        moved              = memory;
        moved[kG + 0x33B0] = 0x40;
        UNSCOPED_INFO("the bracket word without the tiling byte disagrees with it: " + why);
        REQUIRE((!DeriveResolveExitState(readerOf(moved), registers(0), &why) && why == "tiling_state"));
    }
    // The probe comparison: the exit the body leaves is exact, a store left undone is a mismatch of its word.
    {
        const auto entry = nullEntry();
        const auto exit  = applied(effects, entry);
        auto       got   = CompareResolveEffects(effects, entry, exit);
        UNSCOPED_INFO("the null pool forward exit is exact");
        REQUIRE((got.exactModeled && got.exactMismatch == 0 && got.exact.poolPos == kNullEnd));
        auto stale    = exit;
        stale.poolPos = 0;
        got           = CompareResolveEffects(effects, entry, stale);
        UNSCOPED_INFO("a forward take whose G+0x3A44 was not stored");
        REQUIRE((got.exactModeled && got.exactMismatch == (1U << 14)));
        auto lowered    = exit;
        lowered.limit38 = entry.limit38 - kSideBuffer;
        got             = CompareResolveEffects(effects, entry, lowered);
        UNSCOPED_INFO("a limit the null pool allocator would not lower");
        REQUIRE((got.exactModeled && got.exactMismatch == (1U << 13)));

        const auto wrapEntry = nullEntry([](ResolveWatch& w)
                                         {
                                             w.poolPos  = 0x00204000;
                                             w.sideEnd  = 0x00204000;
                                             w.progress = kSegment + 1;
                                         });
        auto       wrapExit  = applied(effects, wrapEntry);
        wrapExit.poolPos     = wrapEntry.poolPos;
        wrapExit.wrapPos     = kWrapPos + kSideBuffer;
        got                  = CompareResolveEffects(effects, wrapEntry, wrapExit);
        UNSCOPED_INFO("the wrap exit is exact");
        REQUIRE((got.exactModeled && got.exactMismatch == 0 && got.exact.wrapPos == kWrapPos + kSideBuffer && got.exact.poolPos == wrapEntry.poolPos));
        wrapExit.wrapPos = wrapEntry.wrapPos;
        got              = CompareResolveEffects(effects, wrapEntry, wrapExit);
        UNSCOPED_INFO("a wrap whose G+0x3A4C was not stored is a wrap_pos mismatch");
        REQUIRE((got.exactModeled && got.exactMismatch == (1U << 16)));

        // A would-be wait and a failed allocation are skipped by their own reasons and counted apart.
        auto waiting     = entry;
        waiting.progress = kSegment - 2;
        got              = CompareResolveEffects(effects, waiting, exit);
        UNSCOPED_INFO("a wait is skipped as alloc_wait");
        REQUIRE((!got.exactModeled && got.exactRefusal == ResolveExitRefusal::AllocWait));
        TakeResolveEffectCounts();
        NoteResolveEffect(got);
        auto failing      = entry;
        failing.sideEnd   = 0;
        failing.wrapLimit = 0;
        NoteResolveEffect(CompareResolveEffects(effects, failing, exit));
        const auto counts = TakeResolveEffectCounts();
        UNSCOPED_INFO("alloc_wait and alloc_capacity are counted apart");
        REQUIRE((counts.exactSkipped == 2 && counts.exactRefused[static_cast<std::size_t>(ResolveExitRefusal::AllocWait)] == 1 && counts.exactRefused[static_cast<std::size_t>(ResolveExitRefusal::AllocCapacity)] == 1));
        const auto line = FormatResolveEffectCounts(counts);
        UNSCOPED_INFO("the line names alloc_wait and wrap_pos");
        REQUIRE((line.contains(" resolve_effect_exact_skipped_alloc_wait=1") && line.contains(" resolve_effect_exact_skipped_alloc_capacity=1") && line.contains(" resolve_effect_exact_mismatch_wrap_pos=0")));
    }
}
#endif

// Clear-store transliteration is independent of resolve_derive.cpp: 0x8269DF00
// (g50:12389-12552), 0x8269E448 (g209:13049-13171), 0x8269E020
// (g125:13208-13890), 0x8269DE98 (g67:13022-13084).
std::optional<ResolveEffects> effectsFor(const Memory& memory, std::uint32_t flags, double depth, std::uint32_t r10 = 0, std::string* why = nullptr)
{
    auto r = registers(flags);
    r[7]   = r10;
    const ResolveClearArgs clear{ kSp, depth };
    return DeriveResolveEffects(readerOf(memory), r, why, &clear);
}

struct DepthCase
{
    const char*   name;
    double        depth;
    std::uint32_t stencil, flags2940;
    bool          packed;
    std::uint32_t control, packedWord;
};

const double kNan   = std::numeric_limits<double>::quiet_NaN();
const float  kNan32 = std::numeric_limits<float>::quiet_NaN();

const DepthCase kDepthCases[] = {
    { "one", 1.0, 0, 3, false, 0x00000FFF, 0xFFFFFF00 },
    { "one_packed", 1.0, 0, 3, true, 0x00000F80, 0xF0000000 },
    { "half", 0.5, 0x80, 0x20, false, 0x00000BFF, 0x7FFFFF80 },
    { "half_packed", 0.5, 0x80, 0x20, true, 0x00000F00, 0xE0000080 },
    { "zero", 0.0, 0x80, 0x20, false, 0x00000800, 0x00000080 },
    { "zero_packed", 0.0, 0x80, 0x20, true, 0x00000800, 0x00000080 },
    { "negative", -1.0, 0x80, 0x20, false, 0xFFFFF801, 0x00000180 },
    { "negative_packed", -1.0, 0x80, 0x20, true, 0x00000800, 0x00000080 },
    { "nan", kNan, 0x80, 0x20, false, 0x00000800, 0x00000080 },
    { "nan_packed", kNan, 0x80, 0x20, true, 0x00000FC0, 0xF8000080 },
    { "huge", 1e30, 0x80, 0x20, false, 0xFFFFFFFF, 0xFFFFFF80 },
    { "huge_packed", 1e30, 0x80, 0x20, true, 0x00000949, 0x293E5980 },
    { "quarter", 0.25, 0x80, 0x20, false, 0x000009FF, 0x3FFFFF80 },
    { "quarter_packed", 0.25, 0x80, 0x20, true, 0x00000E80, 0xD0000080 },
    // The smallest normal float24 exponent and the denormal shifts below it.
    { "pow_m14", 0x1p-14, 0x80, 0x20, false, 0x00000800, 0x0003FF80 },
    { "pow_m14_packed", 0x1p-14, 0x80, 0x20, true, 0x00000880, 0x10000080 },
    { "pow_m15_packed", 0x1p-15, 0x80, 0x20, true, 0x00000A00, 0x40000080 },
    { "pow_m19_packed", 0x1p-19, 0x80, 0x20, true, 0x00000820, 0x04000080 },
    { "pow_m20_packed", 0x1p-20, 0x80, 0x20, true, 0x00000810, 0x02000080 },
    { "mantissa_m20_packed", 0x1.8p-20, 0x80, 0x20, true, 0x00000818, 0x03000080 },
    { "pow_m125_packed", 0x1p-125, 0x80, 0x20, true, 0x00000800, 0x00000080 },
    // Bit 11 of the control word: the stencil against the byte G+0x2942, xored with bit 5 of G+0x2940.
    { "stencil_equal_bit5_set", 0.25, 0x12, 0x1220, false, 0x000001FF, 0x3FFFFF12 },
    { "stencil_equal_bit5_clear", 0.25, 0x12, 0x1200, false, 0x000009FF, 0x3FFFFF12 },
    { "stencil_differs_bit5_set", 0.25, 0x13, 0x1220, false, 0x000009FF, 0x3FFFFF13 },
    { "stencil_differs_bit5_clear", 0.25, 0x13, 0x1200, false, 0x000001FF, 0x3FFFFF13 },
    { "stencil_equal_bit5_set_packed", 0.25, 0x12, 0x1220, true, 0x00000680, 0xD0000012 },
    { "stencil_equal_bit5_clear_packed", 0.25, 0x12, 0x1200, true, 0x00000E80, 0xD0000012 },
    { "stencil_differs_bit5_set_packed", 0.25, 0x13, 0x1220, true, 0x00000E80, 0xD0000013 },
    { "stencil_differs_bit5_clear_packed", 0.25, 0x13, 0x1200, true, 0x00000680, 0xD0000013 },
};

constexpr std::uint32_t kPackedDevice = 0x00010000, kDirectDevice = 0xFFFEFFFF;

void testDepthClearWords()
{
    for (const auto& c : kDepthCases)
    {
        const std::string label  = std::string(c.name) + ": ";
        auto              memory = build({ .name = "depth", .flags = 0x200 });
        memory[kSp + 92]         = c.stencil;
        memory[kG + 0x2940]      = c.flags2940;
        memory[kG + 0x2888]      = c.packed ? kPackedDevice : kDirectDevice;
        std::string why;
        const auto  effects = effectsFor(memory, 0x200, c.depth, 0, &why);
        UNSCOPED_INFO(label + "derived: " + why);
        REQUIRE((effects && effects->depthClear && !effects->colorClear));
        UNSCOPED_INFO(label + std::format("words {:08X} {:08X} vs {:08X} {:08X}", effects->depthWords[0], effects->depthWords[1], c.control, c.packedWord));
        REQUIRE((effects->depthWords == std::array<std::uint32_t, 2>{ c.control, c.packedWord }));
        UNSCOPED_INFO(label + "no color words for flags 0x200");
        REQUIRE((effects->colorWords == std::array<std::uint32_t, 2>{} && !effects->clearRefused));
    }

    // With a parameter block the format is its dword 2: a halfword table entry
    // with any of 0xF00 set is the packed layout, whatever the device says.
    const auto viaBlock = [](std::uint32_t format, std::uint32_t entry, std::uint32_t device)
    {
        auto memory         = build({ .name = "block", .flags = 0x200 });
        memory[kSp + 92]    = 0x80;
        memory[kSp + 100]   = kParams;
        memory[kG + 0x2940] = 0x20;
        memory[kParams + 8] = 0xFFFFFFC0U | format;
        setHalf(memory, kResolveFormatTable + 2 * format, entry);
        memory[kG + 0x2888] = device;
        const auto effects  = effectsFor(memory, 0x200, 0.5);
        UNSCOPED_INFO("a parameter block derives");
        REQUIRE(effects.has_value());
        return effects->depthWords;
    };
    constexpr std::array<std::uint32_t, 2> direct{ 0x00000BFF, 0x7FFFFF80 }, packed{ 0x00000F00, 0xE0000080 };
    UNSCOPED_INFO("a packed table entry wins over a direct device");
    REQUIRE(viaBlock(9, 0x0F08, kDirectDevice) == packed);
    UNSCOPED_INFO("a table entry without 0xF00 wins over a packed device");
    REQUIRE(viaBlock(10, 0x00F8, kPackedDevice) == direct);
    UNSCOPED_INFO("bit 0x100 alone is the packed layout");
    REQUIRE(viaBlock(11, 0x0100, kDirectDevice) == packed);
    UNSCOPED_INFO("bit 0x200 alone is the packed layout");
    REQUIRE(viaBlock(12, 0x0200, kDirectDevice) == packed);
    UNSCOPED_INFO("bits above 0xF00 are not the layout");
    REQUIRE(viaBlock(13, 0x1008, kPackedDevice) == direct);

    // Only the words a flag needs are read.
    auto memory = build({ .name = "needs", .flags = 0x200 });
    memory.erase(kResolveClearOne);
    memory.erase(kResolveClearDefault);
    memory.erase(kColorVector);
    UNSCOPED_INFO("flags 0x200 read no color word");
    REQUIRE(effectsFor(memory, 0x200, 0.5).has_value());
    memory = build({ .name = "needs", .flags = 0x100 });
    memory.erase(kSp + 92);
    memory.erase(kResolveClearScale11);
    memory.erase(kResolveClearScale24);
    memory.erase(kG + 0x2940);
    UNSCOPED_INFO("flags 0x100 read no depth word");
    REQUIRE(effectsFor(memory, 0x100, 0.5, kColorVector).has_value());
}

struct ColorCase
{
    const char*          name;
    std::uint32_t        kind, bias;
    std::array<float, 4> color;
    std::uint32_t        high, low;
};

const ColorCase kColorCases[] = {
    { "black_opaque", 0, 0, { 0.0F, 0.0F, 0.0F, 1.0F }, 0xFF000000, 0xFF000000 },
    { "mixed", 0, 0, { 1.0F, 0.5F, 0.25F, 0.75F }, 0xBF4080FF, 0xBF4080FF },
    { "format2_ones", 2, 0, { 1.0F, 1.0F, 1.0F, 1.0F }, 0xFFFFFFFF, 0xFFFFFFFF },
    { "format2", 2, 0, { 0.5F, 0.0F, 1.0F, 0.5F }, 0xBFF00200, 0xBFF00200 },
    { "format10_reads_row2", 10, 0, { 0.5F, 0.0F, 1.0F, 0.5F }, 0xBFF00200, 0xBFF00200 },
    { "clamped", 0, 0, { 2.0F, -1.0F, 1e30F, -1e30F }, 0x00FF00FF, 0x00FF00FF },
    { "ties_to_even", 0, 0, { 1.5F / 255.0F, 2.5F / 255.0F, 0.5F / 255.0F, 3.5F / 255.0F }, 0x04000202, 0x04000202 },
    { "half_rounds_up_to_even", 0, 0, { 0.5F, 0.5F, 0.5F, 0.5F }, 0x80808080, 0x80808080 },
    { "format2_ties", 2, 0, { 0.5F / 1023.0F, 1.5F / 1023.0F, 2.5F / 1023.0F, 0.5F / 3.0F }, 0x00200800, 0x00200800 },
    { "bias_plus1", 0, 1, { 0.25F, 0.125F, 0.5F, 0.25F }, 0x80FF4080, 0x80FF4080 },
    { "bias_minus1", 0, 0x3F, { 1.0F, 0.5F, 1.0F, 1.0F }, 0x80804080, 0x80804080 },
    { "bias_minus32", 0, 0x20, { 1.0F, 1.0F, 1.0F, 1.0F }, 0x00000000, 0x00000000 },
    { "bias_plus31_tiny_and_zero", 0, 0x1F, { 0x1p-100F, 0.0F, 1.0F, 1.0F }, 0xFFFF0000, 0xFFFF0000 },
    { "float_format14", 14, 0, { 0.5F, 0.25F, 0.0F, 0.0F }, 0x3F000000, 0x3F000000 },
    { "float_format15", 15, 0, { 0.5F, 0.25F, 0.0F, 0.0F }, 0x3E800000, 0x3F000000 },
    { "float_format14_bias", 14, 1, { 0.5F, 0.25F, 0.0F, 0.0F }, 0x3F800000, 0x3F800000 },
    { "float_bias_clamps_high", 14, 1, { 0x1p127F, 0.0F, 0.0F, 0.0F }, 0x7F000000, 0x7F000000 },
    { "float_bias_clamps_low", 14, 0x3D, { 0x1p-125F, 0.0F, 0.0F, 0.0F }, 0x00800000, 0x00800000 },
    { "float_bias_skips_infinity", 14, 1, { std::numeric_limits<float>::infinity(), 0.0F, 0.0F, 0.0F }, 0x7F800000, 0x7F800000 },
    { "float_format15_bias_each_channel", 15, 1, { 0x1p127F, 0x1p-125F, 0.0F, 0.0F }, 0x01800000, 0x7F000000 },
};

void testColorClearWords()
{
    const auto shadowOf = [](std::uint32_t kind, std::uint32_t bias)
    {
        return (bias << 20) | (kind << 16) | 0x1234;
    };
    for (const auto& c : kColorCases)
    {
        const std::string label  = std::string(c.name) + ": ";
        auto              memory = build({ .name = "color", .flags = 0x100 });
        memory[shadowAddress(0)] = shadowOf(c.kind, c.bias);
        setColor(memory, kColorVector, c.color);
        std::string why;
        const auto  effects = effectsFor(memory, 0x100, 0.0, kColorVector, &why);
        UNSCOPED_INFO(label + "derived: " + why);
        REQUIRE((effects && effects->colorClear && !effects->depthClear && !effects->clearRefused));
        UNSCOPED_INFO(label + std::format("words {:08X} {:08X} vs {:08X} {:08X}", effects->colorWords[0], effects->colorWords[1], c.high, c.low));
        REQUIRE((effects->colorWords == std::array<std::uint32_t, 2>{ c.high, c.low }));
        UNSCOPED_INFO(label + "no depth words for flags 0x100");
        REQUIRE((effects->depthWords == std::array<std::uint32_t, 2>{}));
    }

    // The target's own shadow word: slots 1..3 read G+0x288C, G+0x2890 and G+0x2894,
    // and the others carry a format the host does not model.
    for (std::uint32_t slot = 0; slot < 4; ++slot)
    {
        auto memory = build({ .name = "slot", .flags = 0x100 | slot });
        for (std::uint32_t k = 0; k <= 8; ++k)
            memory[kG + 0x2884 + 4 * k] = shadowOf(1, 0);
        memory[shadowAddress(slot)] = shadowOf(0, 0);
        const auto effects          = effectsFor(memory, 0x100 | slot, 0.0, kColorVector);
        UNSCOPED_INFO(std::format("the shadow word of slot {}", slot));
        REQUIRE((effects && !effects->clearRefused && effects->colorWords == std::array<std::uint32_t, 2>{ 0xBF4080FF, 0xBF4080FF }));
    }

    // r10 selects the 16-byte block holding it; null selects the image default.
    for (const std::uint32_t offset : { 0U, 4U, 8U, 12U })
    {
        auto memory              = build({ .name = "block", .flags = 0x100 });
        memory[shadowAddress(0)] = shadowOf(0, 0);
        const auto effects       = effectsFor(memory, 0x100, 0.0, kColorVector + offset);
        UNSCOPED_INFO(std::format("r10 at +{} reads the block", offset));
        REQUIRE((effects && effects->colorWords[0] == 0xBF4080FF));
    }
    {
        auto memory              = build({ .name = "default", .flags = 0x100 });
        memory[shadowAddress(0)] = shadowOf(0, 0);
        const auto effects       = effectsFor(memory, 0x100, 0.0, 0);
        UNSCOPED_INFO("a null r10 reads the image default");
        REQUIRE((effects && effects->colorWords == std::array<std::uint32_t, 2>{ 0x4080FF00, 0x4080FF00 }));
        memory.erase(kResolveClearDefault + 8);
        UNSCOPED_INFO("an unreadable default word");
        REQUIRE(!effectsFor(memory, 0x100, 0.0, 0));
    }

    // A format the host does not model, and a NaN in the unorm path, are refused
    // by name with the words left empty; the raw float formats keep a NaN.
    for (const std::uint32_t kind : { 1U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 11U, 12U, 13U })
    {
        auto memory              = build({ .name = "refused", .flags = 0x100 });
        memory[shadowAddress(0)] = shadowOf(kind, 0);
        const auto effects       = effectsFor(memory, 0x100, 0.0, kColorVector);
        UNSCOPED_INFO(std::format("format {} is refused", kind));
        REQUIRE((effects && effects->colorClear && effects->clearRefused && std::string(effects->clearRefused) == "clear_color_format" && effects->colorWords == std::array<std::uint32_t, 2>{}));
    }
    {
        auto memory              = build({ .name = "nan", .flags = 0x100 });
        memory[shadowAddress(0)] = shadowOf(0, 0);
        setColor(memory, kColorVector, { kNan32, 0.0F, 0.0F, 0.0F });
        auto effects = effectsFor(memory, 0x100, 0.0, kColorVector);
        UNSCOPED_INFO("a NaN channel in the unorm path is refused");
        REQUIRE((effects && effects->clearRefused));
        memory[shadowAddress(0)] = shadowOf(14, 0);
        effects                  = effectsFor(memory, 0x100, 0.0, kColorVector);
        UNSCOPED_INFO("a NaN float color is raw");
        REQUIRE((effects && !effects->clearRefused && effects->colorWords == std::array<std::uint32_t, 2>{ 0x7FC00000, 0x7FC00000 }));
    }

    // Parameter block selects format, entry and bias; wider shadow format
    // determines low-word order.
    const auto viaBlock = [&](std::uint32_t format, std::uint32_t entry, std::uint32_t bias, std::uint32_t mapped, std::uint32_t limitEntry, std::uint32_t kind = 0)
    {
        auto memory              = build({ .name = "block", .flags = 0x100 });
        memory[shadowAddress(0)] = shadowOf(kind, 0x2A);
        memory[kSp + 100]        = kParams;
        memory[kParams]          = 0xFFFFFFC0U | format;
        memory[kParams + 4]      = bias;
        setHalf(memory, kResolveFormatTable + 2 * (format == 54 ? 7 : format), entry);
        setByte(memory, kResolveClearFormatMap + kind, mapped);
        setHalf(memory, kResolveFormatTable + 2 * mapped, limitEntry);
        const auto effects = effectsFor(memory, 0x100, 0.0, kColorVector);
        UNSCOPED_INFO("a parameter block derives");
        REQUIRE((effects && !effects->clearRefused));
        return effects->colorWords;
    };
    using Words = std::array<std::uint32_t, 2>;
    UNSCOPED_INFO("the block's unorm layout ignores the shadow word's bias");
    REQUIRE(viaBlock(5, 0x0010, 0, 4, 0x0008) == Words{ 0xBF4080FF, 0xBF4080FF });
    UNSCOPED_INFO("format 15 through the block, limit below the width");
    REQUIRE(viaBlock(6, 0x0F20, 0, 4, 0x0010) == Words{ 0x3F000000, 0x3F800000 });
    UNSCOPED_INFO("a limit equal to the width keeps the low word");
    REQUIRE(viaBlock(6, 0x0F20, 0, 4, 0x0020) == Words{ 0x3F000000, 0x3F800000 });
    UNSCOPED_INFO("a limit above the width copies the high word");
    REQUIRE(viaBlock(6, 0x0F20, 0, 4, 0x0021) == Words{ 0x3F000000, 0x3F000000 });
    UNSCOPED_INFO("the bias word of the block applies before the copy");
    REQUIRE(viaBlock(6, 0x0F20, 2, 4, 0x0030) == Words{ 0x40000000, 0x40000000 });
    UNSCOPED_INFO("format 54 reads entry 7");
    REQUIRE(viaBlock(54, 0x0E08, 0, 4, 0x0008) == Words{ 0x3F800000, 0x3F800000 });
    UNSCOPED_INFO("the whole bias word is ORed, a minus one lowers the exponent");
    REQUIRE(viaBlock(5, 0x0E08, 0xFFFFFFFF, 4, 0x0008) == Words{ 0x3F000000, 0x3F000000 });
    UNSCOPED_INFO("a bias of 3 saturates the unorm channels");
    REQUIRE(viaBlock(5, 0x0000, 3, 4, 0x0008) == Words{ 0xFFFFFFFF, 0xFFFFFFFF });
    UNSCOPED_INFO("the format map is indexed by the shadow word's kind");
    REQUIRE(viaBlock(5, 0x0F20, 0, 3, 0x0031, 7) == Words{ 0x3F000000, 0x3F000000 });
    UNSCOPED_INFO("the same map with a limit below the width");
    REQUIRE(viaBlock(5, 0x0F20, 0, 3, 0x0010, 7) == Words{ 0x3F000000, 0x3F800000 });
    // The refusal is the same through a block.
    {
        auto memory         = build({ .name = "block-refused", .flags = 0x100 });
        memory[kSp + 100]   = kParams;
        memory[kParams]     = 5;
        memory[kParams + 4] = 0;
        setHalf(memory, kResolveFormatTable + 10, 0x0300);
        const auto effects = effectsFor(memory, 0x100, 0.0, kColorVector);
        UNSCOPED_INFO("a block format the host does not model");
        REQUIRE((effects && effects->clearRefused));
    }
}

void testClearInputs()
{
    std::string why;
    const auto  memory = build({ .name = "inputs", .flags = 0x200 });
    const auto  read   = readerOf(memory);
    auto        r      = registers(0x200);
    UNSCOPED_INFO("flags 0x200 without the stack words: " + why);
    REQUIRE((!DeriveResolveEffects(read, r, &why) && why == "clear_inputs"));
    r = registers(0x100);
    UNSCOPED_INFO("flags 0x100 without the stack words: " + why);
    REQUIRE((!DeriveResolveEffects(read, r, &why) && why == "clear_inputs"));
    r = registers(0);
    UNSCOPED_INFO("a call without clear flags needs none");
    REQUIRE(DeriveResolveEffects(read, r, &why).has_value());
    UNSCOPED_INFO("the exit state carries the refusal: " + why);
    REQUIRE((DeriveResolveExitState(read, registers(0x200), &why) == std::nullopt && why == "clear_inputs"));

    const ResolveClearArgs high{ 0xFFFFFFC0, 0.0 };
    r = registers(0x200);
    UNSCOPED_INFO("a stack pointer whose words would wrap: " + why);
    REQUIRE((!DeriveResolveEffects(read, r, &why, &high) && why == "address_overflow"));
    r    = registers(0x100);
    r[7] = 0xFFFFFFF1;
    const ResolveClearArgs here{ kSp, 0.0 };
    UNSCOPED_INFO("an r10 whose block would wrap: " + why);
    REQUIRE((!DeriveResolveEffects(read, r, &why, &here) && why == "address_overflow"));
    r    = registers(0x200);
    r[7] = 0xFFFFFFF1;
    UNSCOPED_INFO("r10 is not read for flags 0x200");
    REQUIRE(DeriveResolveEffects(read, r, &why, &here).has_value());
    auto wrapped       = build({ .name = "inputs", .flags = 0x200 });
    wrapped[kSp + 100] = 0xFFFFFFF9;
    UNSCOPED_INFO("a parameter block whose words would wrap: " + why);
    REQUIRE((!DeriveResolveEffects(readerOf(wrapped), registers(0x200), &why, &here) && why == "address_overflow"));

    for (const std::uint32_t missing : { kSp + 92, kSp + 100, kG + 0x2940, kG + 0x2888, kResolveClearScale11, kResolveClearScale24 })
    {
        auto copy = build({ .name = "inputs", .flags = 0x200 });
        copy.erase(missing);
        UNSCOPED_INFO(std::format("an unreadable {:08X} for flags 0x200: {}", missing, why));
        REQUIRE((!DeriveResolveEffects(readerOf(copy), registers(0x200), &why, &here) && why == std::format("unreadable {:08X}", missing)));
    }
    for (const std::uint32_t missing : { kResolveClearColorBits, kResolveClearAlphaBits, kResolveClearOne, kColorVector + 12 })
    {
        auto copy              = build({ .name = "inputs", .flags = 0x100 });
        copy[shadowAddress(0)] = 0;
        setColor(copy, kColorVector, { 0.0F, 0.0F, 0.0F, 1.0F });
        copy.erase(missing & ~3U);
        auto regs = registers(0x100);
        regs[7]   = kColorVector;
        UNSCOPED_INFO(std::format("an unreadable {:08X} for flags 0x100: {}", missing, why));
        REQUIRE((!DeriveResolveEffects(readerOf(copy), regs, &why, &here) && why == std::format("unreadable {:08X}", missing & ~3U)));
    }
}

// The exit of a call with clear flags: the clear words in G+0x2A28..0x2A34,
// the depth clear bit of G+0x2ABD, the ring bound and the writes.
ResolveEffects clearEffects(bool depth, bool color)
{
    auto effects       = handEffects(false);
    effects.depthClear = depth;
    effects.colorClear = color;
    effects.depthWords = { 0x000009FF, 0x3FFFFF80 };
    effects.colorWords = { 0xBF4080FF, 0xBF4080FF };
    if (depth)
        effects.setBeforeAlloc[0] |= 0x20000;
    return effects;
}

void testClearExit()
{
    const auto entry = handEntry(kMixedEntry);
    {
        const auto depth = DeriveResolveExitState(clearEffects(true, false), entry);
        UNSCOPED_INFO("flags 0x200 replace the first two words");
        REQUIRE((depth && depth->clear == std::array<std::uint32_t, 4>{ 0x000009FF, 0x3FFFFF80, kClearEntry + 2, kClearEntry + 3 }));
        UNSCOPED_INFO("flags 0x200 set the depth clear bit of G+0x2ABD");
        REQUIRE(depth->allocByte == (entry.allocByte | kResolveDepthClearBit));
        const auto color = DeriveResolveExitState(clearEffects(false, true), entry);
        UNSCOPED_INFO("flags 0x100 replace the last two words");
        REQUIRE((color && color->clear == std::array<std::uint32_t, 4>{ kClearEntry, kClearEntry + 1, 0xBF4080FF, 0xBF4080FF }));
        UNSCOPED_INFO("flags 0x100 leave G+0x2ABD alone");
        REQUIRE(color->allocByte == entry.allocByte);
        const auto both = DeriveResolveExitState(clearEffects(true, true), entry);
        UNSCOPED_INFO("flags 0x300 replace all four");
        REQUIRE((both && both->clear == std::array<std::uint32_t, 4>{ 0x000009FF, 0x3FFFFF80, 0xBF4080FF, 0xBF4080FF }));
        const auto plain = DeriveResolveExitState(handEffects(false), entry);
        UNSCOPED_INFO("no clear flag leaves the clear words");
        REQUIRE((plain && plain->clear == entry.clear && plain->allocByte == entry.allocByte));
        // Every other word is the plain exit's.
        auto same      = *both;
        same.clear     = plain->clear;
        same.allocByte = plain->allocByte;
        UNSCOPED_INFO("the other words are the plain exit's");
        REQUIRE((same.dirty[0] == (plain->dirty[0] | 0x20000) && same.size == plain->size && same.copy == plain->copy && same.poolPos == plain->poolPos));
    }

    // Clear masks add 2, 3 or 4 ring words as runs join the base mask.
    auto clean = handEntry({ 0, 0, 0 });
    UNSCOPED_INFO("no clear bits");
    REQUIRE(ResolveMaxRingWords(clean, 0) == 302);
    UNSCOPED_INFO("the 0x300 bits join the run of 0x3C00");
    REQUIRE(ResolveMaxRingWords(clean, 0x300) == 304);
    UNSCOPED_INFO("the 0xC0 bits are a run of their own");
    REQUIRE(ResolveMaxRingWords(clean, 0xC0) == 305);
    UNSCOPED_INFO("both clear blocks");
    REQUIRE(ResolveMaxRingWords(clean, 0x3C0) == 306);
    UNSCOPED_INFO("the masks are plain bits");
    REQUIRE(ResolveMaxRingWords(clean, 0x300 | 0xC0) == ResolveMaxRingWords(clean, 0x3C0));
    auto full = handEntry({ ~0ULL, ~0ULL, ~0ULL });
    UNSCOPED_INFO("bits already set add nothing");
    REQUIRE(ResolveMaxRingWords(full, 0x3C0) == ResolveMaxRingWords(full, 0));
    // The same bound decides the exit: the edge of the limit moves with the flags.
    {
        const auto edge = [&](const ResolveEffects& effects, std::uint32_t words)
        {
            auto watch                 = handEntry({ 0, 0, 0 });
            watch.limit38              = kCursor + 4 * words + kSideBuffer;
            watch.limit34              = watch.limit38 + 160;
            ResolveExitRefusal refusal = ResolveExitRefusal::None;
            DeriveResolveExitState(effects, watch, &refusal);
            return refusal;
        };
        UNSCOPED_INFO("the plain edge is 302 words");
        REQUIRE((edge(clearEffects(false, false), 302) == ResolveExitRefusal::None && edge(clearEffects(false, false), 301) == ResolveExitRefusal::RingSpace));
        UNSCOPED_INFO("the depth clear edge is 304 words");
        REQUIRE((edge(clearEffects(true, false), 304) == ResolveExitRefusal::None && edge(clearEffects(true, false), 303) == ResolveExitRefusal::RingSpace));
        UNSCOPED_INFO("the color clear edge is 305 words");
        REQUIRE((edge(clearEffects(false, true), 305) == ResolveExitRefusal::None && edge(clearEffects(false, true), 304) == ResolveExitRefusal::RingSpace));
        UNSCOPED_INFO("the combined edge is 306 words");
        REQUIRE((edge(clearEffects(true, true), 306) == ResolveExitRefusal::None && edge(clearEffects(true, true), 305) == ResolveExitRefusal::RingSpace));
    }

    // The color the host does not model is its own refusal, after tiling and before the allocator.
    {
        auto refused               = clearEffects(false, true);
        refused.colorWords         = {};
        refused.clearRefused       = "clear_color_format";
        ResolveExitRefusal refusal = ResolveExitRefusal::None;
        std::string        why;
        UNSCOPED_INFO("an unmodeled color has no exit");
        REQUIRE((!DeriveResolveExitState(refused, entry, &refusal, &why) && refusal == ResolveExitRefusal::ClearColor && why == "clear_color_format"));
        auto both       = refused;
        both.depthClear = true;
        UNSCOPED_INFO("flags 0x300 with an unmodeled color");
        REQUIRE((!DeriveResolveExitState(both, entry, &refusal, &why) && refusal == ResolveExitRefusal::ClearColor));
        auto depthOnly         = clearEffects(true, false);
        depthOnly.clearRefused = "clear_color_format";
        UNSCOPED_INFO("the refusal belongs to the color flag");
        REQUIRE((DeriveResolveExitState(depthOnly, entry, &refusal).has_value() && refusal == ResolveExitRefusal::None));
    }

    // The writes, in kResolveExitWordNames order after the pool words, and the mismatch bits.
    {
        const auto                      effects      = clearEffects(true, true);
        const auto                      exit         = *DeriveResolveExitState(effects, entry);
        auto                            writes       = ResolveExitWrites(kG, kHeader, entry, exit);
        const std::vector<ResolveWrite> expectedTail = {
            { kG + 0x2A28, 0x000009FF, false },
            { kG + 0x2A2C, 0x3FFFFF80, false },
            { kG + 0x2A30, 0xBF4080FF, false },
            { kG + 0x2A34, 0xBF4080FF, false },
        };
        UNSCOPED_INFO("the four clear words are stored last");
        REQUIRE((writes.size() > 4 && std::vector<ResolveWrite>(writes.end() - 4, writes.end()) == expectedTail));
        const auto it = std::find(writes.begin(), writes.end(), ResolveWrite{ kG + 0x2ABD, 0x84, true });
        UNSCOPED_INFO("the allocation byte stores the depth clear bit");
        REQUIRE(it != writes.end());
        // A word equal at the entry is not stored.
        auto equal     = entry;
        equal.clear[1] = 0x3FFFFF80;
        writes         = ResolveExitWrites(kG, kHeader, equal, exit);
        UNSCOPED_INFO("an unchanged clear word is not stored");
        REQUIRE(std::none_of(writes.begin(), writes.end(), [](const ResolveWrite& write)
                             {
                                 return write.address == kG + 0x2A2C;
                             }));
#if REREVVED_DEVELOPER
        // The mismatch bits 17..20 follow the order.
        const auto done = watchOfExit(exit);
        UNSCOPED_INFO("an equal clear exit has no mismatch");
        REQUIRE(ResolveExitMismatches(exit, done) == 0);
        for (std::size_t i = 0; i < kResolveClearWordCount; ++i)
        {
            auto stale = done;
            stale.clear[i] ^= 1;
            UNSCOPED_INFO(std::format("clear word {} differs alone", i));
            REQUIRE(ResolveExitMismatches(exit, stale) == 1U << (17 + i));
        }
        auto byteStale = done;
        byteStale.allocByte &= static_cast<std::uint8_t>(~kResolveDepthClearBit);
        UNSCOPED_INFO("a depth clear whose G+0x2ABD bit was not set");
        REQUIRE(ResolveExitMismatches(exit, byteStale) == 1U << 8);
#endif
    }

#if REREVVED_DEVELOPER
    // The probe comparison and the counters.
    {
        const auto effects = clearEffects(true, true);
        const auto exitOf  = [&](const ResolveEffects& wanted)
        {
            auto watch  = watchOfExit(*DeriveResolveExitState(wanted, entry));
            watch.fence = entry.fence;
            return watch;
        };
        const auto exit = exitOf(effects);
        auto       got  = CompareResolveEffects(effects, entry, exit);
        UNSCOPED_INFO("a clear call is exact and marked as one");
        REQUIRE((got.exactModeled && got.exactClear && got.exactMismatch == 0));
        auto stale     = exit;
        stale.clear[2] = entry.clear[2];
        got            = CompareResolveEffects(effects, entry, stale);
        UNSCOPED_INFO("a color word the body did not store");
        REQUIRE((got.exactModeled && got.exactClear && got.exactMismatch == (1U << 19)));
        UNSCOPED_INFO("a call without clear flags is not marked");
        REQUIRE(!CompareResolveEffects(handEffects(false), entry, exitOf(handEffects(false))).exactClear);
        auto refused         = clearEffects(false, true);
        refused.clearRefused = "clear_color_format";
        got                  = CompareResolveEffects(refused, entry, exit);
        UNSCOPED_INFO("an unmodeled color is skipped as clear_color");
        REQUIRE((!got.exactModeled && !got.exactClear && got.exactRefusal == ResolveExitRefusal::ClearColor));

        TakeResolveEffectCounts();
        NoteResolveEffect(CompareResolveEffects(effects, entry, exit));
        NoteResolveEffect(CompareResolveEffects(effects, entry, exit));
        NoteResolveEffect(CompareResolveEffects(effects, entry, stale));
        NoteResolveEffect(CompareResolveEffects(handEffects(false), entry, exitOf(handEffects(false))));
        NoteResolveEffect(CompareResolveEffects(refused, entry, exit));
        const auto counts = TakeResolveEffectCounts();
        UNSCOPED_INFO("three modeled clear calls, two exact");
        REQUIRE((counts.exactClear == 3 && counts.exactClearOk == 2 && counts.exactOk == 3));
        UNSCOPED_INFO("the skip and the word mismatch");
        REQUIRE((counts.exactRefused[static_cast<std::size_t>(ResolveExitRefusal::ClearColor)] == 1 && counts.exactMismatch[19] == 1));
        const auto line = FormatResolveEffectCounts(counts);
        UNSCOPED_INFO("the line: " + line);
        REQUIRE((line.contains(" resolve_effect_exact_clear=3 resolve_effect_exact_clear_ok=2 ") && line.contains(" resolve_effect_exact_mismatch_clear_color=1") && line.contains(" resolve_effect_exact_skipped_clear_color=1") &&
                 !line.contains("skipped_clear_flags")));
        const auto reset = TakeResolveEffectCounts();
        UNSCOPED_INFO("take resets the clear counters");
        REQUIRE((reset.exactClear == 0 && reset.exactClearOk == 0));
    }
#endif

    // Through the reader: the clear words and the byte come out of memory.
    {
        auto memory              = withWatch(build({ .name = "r", .flags = 0x300, .shadowHit = true }), entry);
        memory[shadowAddress(0)] = (0U << 20) | (0U << 16);
        setColor(memory, kColorVector, { 1.0F, 0.5F, 0.25F, 0.75F });
        memory[kG + 0x2888] = kDirectDevice;
        memory[kSp + 92]    = 0x80;
        memory[kG + 0x2940] = 0x20;
        auto regs           = registers(0x300);
        regs[7]             = kColorVector;
        const ResolveClearArgs args{ kSp, 0.25 };
        std::string            why;
        const auto             exit = DeriveResolveExitState(readerOf(memory), regs, &why, &args);
        UNSCOPED_INFO("flags 0x300 from memory: " + why);
        REQUIRE((exit && exit->clear == std::array<std::uint32_t, 4>{ 0x000009FF, 0x3FFFFF80, 0xBF4080FF, 0xBF4080FF } && exit->allocByte == (entry.allocByte | kResolveDepthClearBit)));
        memory[kG + 0x2A30] = 0xDEAD0000;
        UNSCOPED_INFO("the exit does not depend on the old color word");
        REQUIRE(DeriveResolveExitState(readerOf(memory), regs, &why, &args)->clear[2] == 0xBF4080FF);
        memory.erase(kG + 0x2A34);
        UNSCOPED_INFO("an unreadable shadow word: " + why);
        REQUIRE((!DeriveResolveExitState(readerOf(memory), regs, &why, &args) && why == "unreadable watched words"));
    }
}

// Overwritten Clear inputs use Original values for copy, dirty, size and frame.
void testClearOverlap()
{
    // Generated frame covers copy G+0x2A18..G+0x2A24, dirty G+0x20 and size
    // G+0x2A44 (g167:13471-13534).
    const auto derive = [](Memory memory, std::uint32_t flags, std::uint32_t r10)
    {
        return effectsFor(memory, flags, 0.5, r10);
    };
    const auto base = [](std::uint32_t flags)
    {
        auto memory         = build({ .name = "overlap", .flags = flags });
        memory[kG + 0x2940] = 3;
        memory[kG + 0x2884] = 0;
        // Words the vectors and blocks below read.
        for (std::uint32_t offset = 0x10; offset < 0x50; offset += 4)
            memory.try_emplace(kG + offset, 0x3F800000);
        for (std::uint32_t offset = 0x2A00; offset < 0x2A50; offset += 4)
            memory.try_emplace(kG + offset, 0x3F800000);
        for (std::uint32_t offset = 0; offset < 0x30; offset += 4)
        {
            memory[kSp - 0x30 + offset]  = 0x3F800000;
            memory[kSp + 0x100 + offset] = 0x3F800000;
        }
        return memory;
    };
    // The reviewer's case: format 0, no parameter block, r10 at G+0x2A20 and the entry
    // pitch word 1.0; Original stores the destination pitch there before the block loads the vector.
    {
        auto memory         = base(0x100);
        memory[kG + 0x2A20] = 0x3F800000;
        const auto effects  = derive(memory, 0x100, kG + 0x2A20);
        UNSCOPED_INFO("a color vector over the destination pitch word is an overlap");
        REQUIRE((effects && effects->clearOverlap));
        ResolveExitRefusal refusal = ResolveExitRefusal::None;
        std::string        why;
        UNSCOPED_INFO("the overlap has no exact exit");
        REQUIRE((!DeriveResolveExitState(*effects, handEntry({ 0, 0, 0 }), &refusal, &why) && refusal == ResolveExitRefusal::ClearOverlap && why == "clear_overlap"));
    }

    // Color vectors: the block holding r10 against each stored range.
    const struct
    {
        std::uint32_t offset;
        bool          overlap;
        const char*   what;
    } vectors[] = {
        { 0x2A10, true, "G+0x2A10..0x2A1C holds the control word" },
        { 0x2A18, true, "the copy words" },
        { 0x2A24, true, "the block holding G+0x2A24" },
        { 0x2A28, true, "G+0x2A20..0x2A2C holds the pitch and info words" },
        { 0x2A30, false, "G+0x2A30..0x2A3C is the clear words only" },
        { 0x2A00, false, "G+0x2A00..0x2A0C is below the copy words" },
        { 0x10, false, "G+0x10..0x1C is below G+0x20" },
        { 0x20, true, "G+0x20 is stored by the body" },
        { 0x30, false, "G+0x30..0x3C is above G+0x24" },
        { 0x2A40, true, "G+0x2A44 is stored on the large form" },
        { 0x2A50, false, "G+0x2A50..0x2A5C is above G+0x2A44" },
    };

    for (const auto& v : vectors)
    {
        auto memory = base(0x100);
        for (std::uint32_t offset = 0x2A50; offset < 0x2A60; offset += 4)
            memory[kG + offset] = 0x3F800000;
        const auto effects = derive(memory, 0x100, kG + v.offset);
        UNSCOPED_INFO(std::string("color vector at ") + v.what);
        REQUIRE((effects && effects->clearOverlap == v.overlap));
    }
    // The body's frame lies below the entry r1; the caller's side does not.
    {
        const auto memory = base(0x100);
        auto       below  = derive(memory, 0x100, kSp - 0x10);
        UNSCOPED_INFO("a color vector in the body's frame");
        REQUIRE((below && below->clearOverlap));
        below = derive(memory, 0x100, kSp + 0x100);
        UNSCOPED_INFO("a color vector in the caller's frame");
        REQUIRE((below && !below->clearOverlap));
        auto beyond = base(0x100);
        for (std::uint32_t offset = 0; offset < 0x10; offset += 4)
            beyond[kSp - 0x410 + offset] = 0x3F800000;
        below = derive(beyond, 0x100, kSp - 0x410);
        UNSCOPED_INFO("below the frame bound");
        REQUIRE((below && !below->clearOverlap));
    }

    // The parameter block: its words are inputs too, so a block over a stored word is an overlap.
    const struct
    {
        std::uint32_t address;
        std::uint32_t flags;
        bool          overlap;
        const char*   what;
    } blocks[] = {
        { kG + 0x2A18, 0x100, true, "a parameter block over the copy words" },
        { kG + 0x2A18, 0x200, true, "a parameter block over the copy words for a depth clear" },
        { kG + 0x18, 0x200, true, "a parameter block whose dword 2 is G+0x20" },
        { kG + 0x2A3C, 0x200, true, "a parameter block whose dword 2 is G+0x2A44" },
        { kG + 0x2A40, 0x100, true, "a parameter block whose dword 1 is G+0x2A44" },
        { kG + 0x2A30, 0x200, false, "a parameter block over the clear words" },
        { kG + 0x2A30, 0x100, false, "a parameter block over the clear words for a color clear" },
        { kSp + 0x100, 0x200, false, "a parameter block in the caller's frame" },
        { kSp - 0x30, 0x200, true, "a parameter block in the body's frame" },
        // The depth block stores G+0x2A2C and G+0x2A28 after its own loads and before the
        // color block's: dword 0 and dword 1 of a block there are read after the stores.
        { kG + 0x2A28, 0x300, true, "a parameter block over the depth block's words, read by the color block" },
        { kG + 0x2A2C, 0x300, true, "a parameter block whose dword 0 is G+0x2A2C, read by the color block" },
        { kG + 0x2A28, 0x100, false, "the same block for a color clear alone, nothing stored first" },
        { kG + 0x2A28, 0x200, false, "the same block for a depth clear alone, dword 2 at G+0x2A30" },
        { kG + 0x2A20, 0x200, false, "dword 2 at G+0x2A28 is loaded by the depth block before it stores there" },
        { kG + 0x2A30, 0x300, false, "a block over the color words, loaded before the color block stores" },
        // The deepest callee frame (0x8269E020 under 0x8269E448) and the depth block's own.
        { kSp - 0x2A0, 0x100, true, "a parameter block in the color block's callee frame" },
        { kSp - 0x1F0, 0x200, true, "a parameter block in the depth block's frame" },
    };

    for (const auto& b : blocks)
    {
        auto memory         = base(b.flags);
        memory[kSp + 100]   = b.address;
        memory[kG + 0x2A20] = 0x3F800000;
        for (std::uint32_t offset = 0; offset < 12; offset += 4)
            memory.try_emplace(b.address + offset, 0x3F800000);
        const auto effects = derive(memory, b.flags, kColorVector);
        UNSCOPED_INFO(std::string(b.what));
        REQUIRE((effects && effects->clearOverlap == b.overlap));
    }
    // Flags without a clear read nothing of this, and the stack words are not overwritten.
    {
        const auto memory  = base(0);
        const auto effects = derive(memory, 0, kG + 0x2A20);
        UNSCOPED_INFO("no clear flags, no overlap");
        REQUIRE((effects && !effects->clearOverlap));
        const auto depth = derive(base(0x200), 0x200, kG + 0x2A20);
        UNSCOPED_INFO("r10 is not an input of a depth clear");
        REQUIRE((depth && !depth->clearOverlap));
    }
    // Overlap is judged before the color: both reasons present, the overlap is reported.
    {
        auto both                  = handEffects(false);
        both.colorClear            = true;
        both.clearRefused          = "clear_color_format";
        both.clearOverlap          = true;
        ResolveExitRefusal refusal = ResolveExitRefusal::None;
        UNSCOPED_INFO("the overlap before the clear color");
        REQUIRE((!DeriveResolveExitState(both, handEntry({ 0, 0, 0 }), &refusal) && refusal == ResolveExitRefusal::ClearOverlap));
        auto depth         = handEffects(false);
        depth.depthClear   = true;
        depth.clearOverlap = true;
        UNSCOPED_INFO("a depth clear overlap");
        REQUIRE((!DeriveResolveExitState(depth, handEntry({ 0, 0, 0 }), &refusal) && refusal == ResolveExitRefusal::ClearOverlap));
        auto tiled       = handEntry({ 0, 0, 0 });
        tiled.tilingByte = 0x10;
        UNSCOPED_INFO("tiling before the overlap");
        REQUIRE((!DeriveResolveExitState(both, tiled, &refusal) && refusal == ResolveExitRefusal::Tiling));
        auto pool     = handEntry({ 0, 0, 0 });
        pool.poolBase = 0;
        UNSCOPED_INFO("the overlap before the allocator path");
        REQUIRE((!DeriveResolveExitState(both, pool, &refusal) && refusal == ResolveExitRefusal::ClearOverlap));
    }
#if REREVVED_DEVELOPER
    // The probe skips it by reason and counts it apart.
    {
        auto effects         = clearEffects(true, false);
        effects.clearOverlap = true;
        const auto entry     = handEntry(kMixedEntry);
        const auto exit      = watchOfExit(*DeriveResolveExitState(clearEffects(true, false), entry));
        const auto got       = CompareResolveEffects(effects, entry, exit);
        UNSCOPED_INFO("an overlapped clear is skipped as clear_overlap");
        REQUIRE((!got.exactModeled && !got.exactClear && got.exactRefusal == ResolveExitRefusal::ClearOverlap));
        TakeResolveEffectCounts();
        NoteResolveEffect(got);
        const auto counts = TakeResolveEffectCounts();
        UNSCOPED_INFO("clear_overlap is its own skip");
        REQUIRE((counts.exactSkipped == 1 && counts.exactRefused[static_cast<std::size_t>(ResolveExitRefusal::ClearOverlap)] == 1));
        UNSCOPED_INFO("the line names clear_overlap");
        REQUIRE(FormatResolveEffectCounts(counts).contains(" resolve_effect_exact_skipped_clear_overlap=1"));
    }
#endif
}

// Predicated Resolve ORs tile mask into ring; matches bracket exit.
void testPredicatedExit()
{
    // Per-tile 0x8250AC90 ORs its tile predicate into the ring.
    // Generated predicate OR writes ring word g167:14825-14833.
    const auto bracketOf = [](std::uint32_t predicate, std::uint32_t tiles, const std::function<void(ResolveWatch&)>& change = [](ResolveWatch&)
                                                                            {
                                                                            })
    {
        return nullEntry([&](ResolveWatch& w)
                         {
                             w.tilingByte  = 0x21;
                             w.bracket     = 0x00300000;
                             w.bracketNext = kSegment + 1;
                             w.targetCount = tiles;
                             w.predicate   = predicate;
                             change(w);
                         });
    };
    const auto fieldsOf = [](const ResolveExitState& e)
    {
        return std::tie(e.copy, e.size, e.dirty, e.allocByte, e.stateByte, e.dest8, e.dest12, e.limit34, e.limit38, e.poolPos, e.poolUsed, e.wrapPos, e.clear);
    };

    const struct
    {
        const char*    name;
        ResolveEffects effects;
    } calls[] = {
        { "plain", handEffects(false) },
        { "large", handEffects(true) },
        { "depth clear", clearEffects(true, false) },
        { "color clear", clearEffects(false, true) },
        { "both clears", clearEffects(true, true) },
    };

    // The masks of the tiles are 3 << 2i; any nonzero predicate classifies the same.
    for (const auto& call : calls)
        for (const std::uint32_t tiles : { 2U, 3U, 4U, 9U })
            for (const std::uint32_t predicate : { 3U, 0xCU, 0x30U, 0xC0U, 1U, 0x80000000U })
            {
                const auto plain     = bracketOf(0, tiles);
                const auto predicted = bracketOf(predicate, tiles);
                UNSCOPED_INFO("the two classes");
                REQUIRE((plain.Tiling() == ResolveTiling::Bracket && predicted.Tiling() == ResolveTiling::PredicatedBracket));
                const auto a     = DeriveResolveExitState(call.effects, plain);
                const auto b     = DeriveResolveExitState(call.effects, predicted);
                const auto label = std::format("{} tiles {} predicate {:X}", call.name, tiles, predicate);
                UNSCOPED_INFO("the predicated exit equals the bracket exit: " + label);
                REQUIRE((a && b && fieldsOf(*a) == fieldsOf(*b)));
                UNSCOPED_INFO("the same writes: " + label);
                REQUIRE(ResolveExitWrites(kG, kHeader, predicted, *b) == ResolveExitWrites(kG, kHeader, plain, *a));
                UNSCOPED_INFO("the same ring bound: " + label);
                REQUIRE(ResolveMaxRingWords(predicted, 0x3C0) == ResolveMaxRingWords(plain, 0x3C0));
            }

    // Every refusal below the tiling tests keeps its reason with the predicate set.
    const auto reasonOf = [&](const ResolveEffects& effects, const ResolveWatch& watch)
    {
        ResolveExitRefusal refusal = ResolveExitRefusal::None;
        DeriveResolveExitState(effects, watch, &refusal);
        return refusal;
    };
    {
        auto overlap         = handEffects(false);
        overlap.clearOverlap = true;
        auto color           = clearEffects(false, true);
        color.clearRefused   = "clear_color_format";

        const struct
        {
            const char*                        name;
            ResolveExitRefusal                 want;
            ResolveEffects                     effects;
            std::function<void(ResolveWatch&)> change;
        } cases[] = {
            { "fence zero", ResolveExitRefusal::Fence, handEffects(false), [](ResolveWatch& w)
              {
                  w.fence = 0;
              } },
            { "allocation marker", ResolveExitRefusal::AllocMarker, handEffects(false), [](ResolveWatch& w)
              {
                  w.allocByte = 0x20;
              } },
            { "clear overlap", ResolveExitRefusal::ClearOverlap, overlap, [](ResolveWatch&)
              {
              } },
            { "clear color", ResolveExitRefusal::ClearColor, color, [](ResolveWatch&)
              {
              } },
            { "pool object without a buffer", ResolveExitRefusal::AllocPath, handEffects(false), [](ResolveWatch& w)
              {
                  w.poolObject = kPoolObject;
              } },
            { "tile count beyond the loops", ResolveExitRefusal::RingSpace, handEffects(false), [](ResolveWatch& w)
              {
                  w.targetCount = 17;
              } },
            { "tight ring", ResolveExitRefusal::RingSpace, handEffects(false), [](ResolveWatch& w)
              {
                  w.limit38 = w.cursor + 16;
                  w.limit34 = w.limit38 + 160;
              } },
            { "failing allocation", ResolveExitRefusal::AllocCapacity, handEffects(false), [](ResolveWatch& w)
              {
                  w.bracketNext = kSegment - 1;
              } },
            { "waiting allocation", ResolveExitRefusal::AllocWait, handEffects(false), [](ResolveWatch& w)
              {
                  w.progress = kSegment - 2;
              } },
        };

        for (const auto& c : cases)
        {
            const auto plain = bracketOf(0, 3, c.change);
            const auto tile  = bracketOf(0xC, 3, c.change);
            UNSCOPED_INFO(std::string("the bracket form refuses: ") + c.name);
            REQUIRE(reasonOf(c.effects, plain) == c.want);
            UNSCOPED_INFO(std::string("and so does the predicated one: ") + c.name);
            REQUIRE(reasonOf(c.effects, tile) == c.want);
        }
    }

    // The predicate outside an open bracket, and the byte with bit 0x10, stay refused.
    {
        const auto effects = handEffects(false);

        const struct
        {
            std::uint8_t       byte;
            std::uint32_t      bracket;
            ResolveExitRefusal want;
            const char*        name;
        } shapes[] = {
            { 0x21, 0, ResolveExitRefusal::TilingPredicate, "the byte without the bracket word" },
            { 0x00, 0x00300000, ResolveExitRefusal::TilingPredicate, "the bracket word without the byte" },
            { 0x00, 0, ResolveExitRefusal::TilingPredicate, "neither" },
            { 0x31, 0x00300000, ResolveExitRefusal::Tiling, "byte bit 0x10 with the bracket" },
            { 0x30, 0, ResolveExitRefusal::Tiling, "byte bit 0x10 without it" },
        };

        for (const auto& s : shapes)
        {
            const auto watch = bracketOf(3, 2, [&](ResolveWatch& w)
                                         {
                                             w.tilingByte = s.byte;
                                             w.bracket    = s.bracket;
                                         });
            UNSCOPED_INFO(std::string("still refused: ") + s.name);
            REQUIRE(reasonOf(effects, watch) == s.want);
        }
    }

#if REREVVED_DEVELOPER
    // The probe: an exact predicated call is counted as tiled and as predicated; a refused one
    // still counts its shape (predicated, clear flags, tile count) and not as modeled.
    {
        const auto effects = clearEffects(true, false);
        const auto entry   = bracketOf(3, 3);
        auto       exit    = watchOfExit(*DeriveResolveExitState(effects, entry));
        exit.fence         = entry.fence;
        auto got           = CompareResolveEffects(effects, entry, exit);
        UNSCOPED_INFO("an exact predicated clear call");
        REQUIRE((got.exactModeled && got.exactTiled && got.exactPredicated && got.exactClear && got.exactMismatch == 0));
        UNSCOPED_INFO("its shape");
        REQUIRE((got.shape.predicated && got.shape.clear && got.shape.tiles == 3));
        auto fenceless  = entry;
        fenceless.fence = 0;
        got             = CompareResolveEffects(effects, fenceless, exit);
        UNSCOPED_INFO("a refused predicated call is not modeled");
        REQUIRE((!got.exactModeled && !got.exactPredicated && got.exactRefusal == ResolveExitRefusal::Fence));
        UNSCOPED_INFO("and keeps its shape");
        REQUIRE((got.shape.predicated && got.shape.clear && got.shape.tiles == 3));
        auto plain = CompareResolveEffects(handEffects(false), bracketOf(0, 3), exit);
        UNSCOPED_INFO("the bracket form without the predicate is not predicated");
        REQUIRE((!plain.shape.predicated && !plain.exactPredicated));

        // A color clear alone sets the clear shape, and a fourth tile has its own bucket.
        const auto colorEffects = clearEffects(false, true);
        const auto colorEntry   = bracketOf(3, 4);
        auto       colorExit    = watchOfExit(*DeriveResolveExitState(colorEffects, colorEntry));
        colorExit.fence         = colorEntry.fence;
        const auto colorGot     = CompareResolveEffects(colorEffects, colorEntry, colorExit);
        UNSCOPED_INFO("an exact predicated color clear call");
        REQUIRE((colorGot.exactPredicated && colorGot.exactMismatch == 0 && colorGot.shape.clear && colorGot.shape.tiles == 4));

        // A predicated call whose exit differs is counted as predicated and not as exact.
        const auto wrong = CompareResolveEffects(handEffects(false), bracketOf(3, 3), exit);
        UNSCOPED_INFO("a predicated call with another exit mismatches");
        REQUIRE((wrong.exactModeled && wrong.exactPredicated && wrong.exactMismatch != 0));
        TakeResolveEffectCounts();
        NoteResolveEffect(CompareResolveEffects(effects, entry, exit));
        NoteResolveEffect(wrong);
        const auto exactCounts = TakeResolveEffectCounts();
        UNSCOPED_INFO("the exact predicated counts");
        REQUIRE((exactCounts.exactPredicated == 2 && exactCounts.exactPredicatedOk == 1));

        TakeResolveEffectCounts();
        NoteResolveEffect(CompareResolveEffects(effects, entry, exit));
        NoteResolveEffect(CompareResolveEffects(effects, fenceless, exit));
        NoteResolveEffect(CompareResolveEffects(handEffects(false), bracketOf(3, 2), exit));
        NoteResolveEffect(CompareResolveEffects(handEffects(false), bracketOf(3, 7), exit));
        NoteResolveEffect(CompareResolveEffects(handEffects(false), bracketOf(0, 3), exit));
        NoteResolveEffect(colorGot);
        const auto counts = TakeResolveEffectCounts();
        UNSCOPED_INFO("the predicated calls seen and those with clear flags");
        REQUIRE((counts.shape.predicated == 5 && counts.shape.predicatedClear == 3));
        UNSCOPED_INFO("the tile count histogram: 2, 3, 4, other");
        REQUIRE(counts.shape.tiles == decltype(counts.shape.tiles){ 1, 2, 1, 1 });
        const auto line = FormatResolveEffectCounts(counts);
        UNSCOPED_INFO("the line: " + line);
        REQUIRE(line.contains(" resolve_effect_predicated=5 resolve_effect_predicated_clear=3 resolve_effect_predicated_tiles_2=1 resolve_effect_predicated_tiles_3=2 resolve_effect_predicated_tiles_4=1 resolve_effect_predicated_tiles_other=1"));
    }
#endif
}

#if REREVVED_DEVELOPER
void testCounts()
{
    TakeResolveEffectCounts();
    const auto entry   = watchOf(0, 0, 0);
    const auto effects = effectsOf(false, false);
    const auto exit    = applied(effects, entry);
    NoteResolveEffect(CompareResolveEffects(effects, entry, exit));
    auto bad  = exit;
    bad.dest8 = 1;
    bad.size ^= 1;
    NoteResolveEffect(CompareResolveEffects(effects, entry, bad));
    auto zero  = entry;
    zero.fence = 0;
    NoteResolveEffect(CompareResolveEffects(effects, zero, applied(effects, zero)));
    auto held  = watchOf(0x20000, 0, 0);
    auto freed = applied(effects, held);
    freed.dirty[0] &= ~0x20000ULL;
    auto noBit = effects;
    noBit.setBeforeAlloc[0] &= ~0x20000ULL;
    NoteResolveEffect(CompareResolveEffects(noBit, held, freed));
    ResolveWatch failed = entry;
    failed.allocByte |= kResolveAllocFailedBit;
    failed.copy = { effects.words.copyControl, effects.words.destBase, effects.words.destPitch, effects.words.destInfo };
    for (std::size_t i = 0; i < kResolveDirtyWords; ++i)
        failed.dirty[i] |= effects.setBeforeAlloc[i];
    failed.stateByte &= static_cast<std::uint8_t>(~kResolveStateClearedBit);
    NoteResolveEffect(CompareResolveEffects(effects, entry, failed));
    NoteResolveEffectUnsupported();
    NoteResolveEffectUnpaired(0);
    NoteResolveEffectUnpaired(3);
    auto counts = TakeResolveEffectCounts();
    UNSCOPED_INFO("ok, unsupported and unpaired");
    REQUIRE((counts.ok == 4 && counts.unsupported == 1 && counts.unpaired == 3));
    UNSCOPED_INFO("per-field mismatches");
    REQUIRE((counts.mismatch[static_cast<std::size_t>(ResolveEffectField::Size)] == 1 && counts.mismatch[static_cast<std::size_t>(ResolveEffectField::Dest)] == 1));
    UNSCOPED_INFO("cleared, alloc failed and list path");
    REQUIRE((counts.dirtyCleared == 1 && counts.cleared1E0000 == 1 && counts.allocFailed == 1 && counts.listPath == 1));
    const std::string expected =
        " resolve_effect_ok=4 resolve_effect_mismatch_copy_words=0 resolve_effect_mismatch_size=1 resolve_effect_mismatch_dirty_set_missing=0"
        " resolve_effect_mismatch_dirty_extra=0 resolve_effect_mismatch_bytes=0 resolve_effect_mismatch_dest=1 resolve_effect_dirty_cleared=1"
        " resolve_effect_cleared_1e0000=1 resolve_effect_alloc_failed=1 resolve_effect_list_path=1 resolve_effect_unsupported=1 resolve_effect_unpaired=3"
        " resolve_effect_exact_ok=1 resolve_effect_exact_tiled=0 resolve_effect_exact_tiled_ok=0 resolve_effect_exact_predicated=0 resolve_effect_exact_predicated_ok=0 resolve_effect_exact_clear=0 resolve_effect_exact_clear_ok=0 resolve_effect_predicated=0 resolve_effect_predicated_clear=0 resolve_effect_predicated_tiles_2=0 resolve_effect_predicated_tiles_3=0 resolve_effect_predicated_tiles_4=0 resolve_effect_predicated_tiles_other=0 resolve_effect_exact_mismatch_copy_control=1 resolve_effect_exact_mismatch_dest_base=0 resolve_effect_exact_mismatch_dest_pitch=0"
        " resolve_effect_exact_mismatch_dest_info=1 resolve_effect_exact_mismatch_size=1 resolve_effect_exact_mismatch_dirty_10=2 resolve_effect_exact_mismatch_dirty_18=1"
        " resolve_effect_exact_mismatch_dirty_20=0 resolve_effect_exact_mismatch_alloc_byte=1 resolve_effect_exact_mismatch_state_byte=0 resolve_effect_exact_mismatch_dest_8=2"
        " resolve_effect_exact_mismatch_dest_12=1 resolve_effect_exact_mismatch_limit_34=1 resolve_effect_exact_mismatch_limit_38=1 resolve_effect_exact_mismatch_pool_pos=1"
        " resolve_effect_exact_mismatch_pool_used=1 resolve_effect_exact_mismatch_wrap_pos=0"
        " resolve_effect_exact_mismatch_clear_control=0 resolve_effect_exact_mismatch_clear_depth=0 resolve_effect_exact_mismatch_clear_color=0"
        " resolve_effect_exact_mismatch_clear_color_low=0 resolve_effect_exact_skipped=2 resolve_effect_exact_skipped_fence=1"
        " resolve_effect_exact_skipped_alloc_marker=0 resolve_effect_exact_skipped_tiling=0 resolve_effect_exact_skipped_tiling_predicate=0"
        " resolve_effect_exact_skipped_tiling_state=0 resolve_effect_exact_skipped_clear_overlap=0 resolve_effect_exact_skipped_clear_color=0 resolve_effect_exact_skipped_alloc_path=0"
        " resolve_effect_exact_skipped_ring_space=0 resolve_effect_exact_skipped_alloc_capacity=0 resolve_effect_exact_skipped_alloc_wait=0 resolve_effect_exact_skipped_stack_overlap=0";

    UNSCOPED_INFO("formatted counts: " + FormatResolveEffectCounts(counts));
    REQUIRE(FormatResolveEffectCounts(counts) == expected);
    counts = TakeResolveEffectCounts();
    UNSCOPED_INFO("take resets");
    REQUIRE((counts.ok == 0 && counts.unsupported == 0 && counts.unpaired == 0 && counts.dirtyCleared == 0 && counts.cleared1E0000 == 0 && counts.allocFailed == 0 &&
             counts.listPath == 0 && counts.mismatch == std::array<std::uint64_t, kResolveEffectFieldCount>{}));
    UNSCOPED_INFO("take resets the tiled and clear counters");
    REQUIRE((counts.exactTiled == 0 && counts.exactTiledOk == 0 && counts.exactClear == 0 && counts.exactClearOk == 0));
    UNSCOPED_INFO("take resets the exact counters");
    REQUIRE((counts.exactOk == 0 && counts.exactSkipped == 0 && counts.exactMismatch == std::array<std::uint64_t, kResolveExitWordCount>{} && counts.exactRefused == std::array<std::uint64_t, kResolveExitRefusalCount>{}));
}
#endif

ResolveProbe probeOf([[maybe_unused]] std::uint32_t g, bool effects, bool derive = true)
{
    ResolveProbe probe;
    probe.admitted   = true;
    probe.effects    = effects;
    probe.deriveCopy = derive;
#if REREVVED_DEVELOPER
    probe.g = g;
#endif
    return probe;
}

constexpr std::uint32_t frameOf(std::uint32_t entrySp)
{
    return entrySp - kResolveFrameSize;
}

// The probes dropped with a stale stack count each check apart.
void testStackCountsEffects()
{
    constexpr std::uint32_t outer = 0x70000000, inner = 0x6FFFF000, innermost = 0x6FFFE000;
    unsigned                dropped = 0;
    ResolveDropCounts       drops;
    {
        ResolveProbeStack stack;
        stack.Push(outer, probeOf(1, true), &drops);
        stack.Push(inner, probeOf(2, true, false), &drops);
        stack.Push(innermost, probeOf(3, false), &drops);
        UNSCOPED_INFO("pushes that drop nothing count nothing");
        REQUIRE((drops.derive == 0 && drops.effects == 0));
        auto popped = stack.Pop(frameOf(outer), dropped, &drops);
        UNSCOPED_INFO("the return drops the stale probes, one with effects, one for derive");
        REQUIRE((popped && DEV_CHECK(popped->g == 1) && dropped == 2 && drops.effects == 1 && drops.derive == 1 && !stack.Size()));
    }
    {
        ResolveProbeStack stack;
        drops = {};
        stack.Push(outer, probeOf(1, true));
        stack.Push(inner, probeOf(2, true, false));
        auto* found = stack.Find(frameOf(outer), dropped, &drops);
        UNSCOPED_INFO("the copy hook drops the stale effect-only probe without a derive count");
        REQUIRE((found && dropped == 1 && drops.effects == 1 && drops.derive == 0 && stack.Size() == 1));
        UNSCOPED_INFO("an unmatched frame counts nothing");
        REQUIRE((!stack.Find(frameOf(inner), dropped, &drops) && drops.effects == 1 && drops.derive == 0));
    }
    {
        // A shallower entry flushes stale probes; a full stack refuses the new one.
        ResolveProbeStack stack;
        drops            = {};
        std::uint32_t sp = outer - 0x10000;
        for (std::uint32_t i = 0; i < ResolveProbeStack::kDepth; ++i, sp -= 0x1000)
            stack.Push(sp, probeOf(i + 1, i % 2 == 0, i < 3));
        UNSCOPED_INFO("a shallow entry flushes the stale stack");
        REQUIRE((stack.Push(outer, probeOf(7, true), &drops) == ResolveProbeStack::kDepth && drops.effects == 2 && drops.derive == 3 && stack.Size() == 1));
        stack.Clear();
        sp = outer;
        for (std::uint32_t i = 0; i < ResolveProbeStack::kDepth; ++i, sp -= 0x1000)
            stack.Push(sp, probeOf(i + 1, false));
        drops = {};
        UNSCOPED_INFO("a full stack drops the new effect-only probe");
        REQUIRE((stack.Push(sp, probeOf(9, true, false), &drops) == 1 && drops.effects == 1 && drops.derive == 0 && stack.Size() == ResolveProbeStack::kDepth));
        drops = {};
        UNSCOPED_INFO("a full stack drops a derive-only probe");
        REQUIRE((stack.Push(sp - 0x1000, probeOf(9, false), &drops) == 1 && drops.effects == 0 && drops.derive == 1));
    }
}

} // namespace

TEST_CASE("resolve_effect", "[ctest][resolve-effect][timeout-60]")
{
    testCases();
    testRefusals();
    testWatch();
#if REREVVED_DEVELOPER
    testCompare();
#endif
    testExitState();
    testExitRefusals();
    testExitFromMemory();
#if REREVVED_DEVELOPER
    testExitMismatches();
#endif
    testExitWrites();
    testDepthClearWords();
    testColorClearWords();
    testClearInputs();
    testClearExit();
    testClearOverlap();
    testPredicatedExit();
#if REREVVED_DEVELOPER
    testCompareExact();
#endif
    testRingModel();
    testNullPoolAllocator();
    testNullPoolBracket();
    testNullPoolExit();
    testTilingShapes();
    testTiledExit();
#if REREVVED_DEVELOPER
    testTiledProbe();
    testNullPoolProbe();
    testCounts();
#endif
    testStackCountsEffects();
}
