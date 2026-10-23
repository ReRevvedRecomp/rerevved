// resolve_consume: 0x8269E520 admission over a synthetic 1280x720 fixture.

#include "gpu/guest_d3d/resolve_consume.h"
#include "gpu/guest_d3d/ring_stubs.h"

#include <rex/cvar.h>

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

REXCVAR_DECLARE(bool, native_consume_resolve);
REXCVAR_DECLARE(bool, native_consume_resolve_tiled);

using namespace rerevved::gpu::guest_d3d;

namespace
{

constexpr std::uint32_t kG = 0x40010000, kHeader = 0x40020000, kSurface = 0x40030000;

// The caller's frame holds the stencil value at +92 and the parameter block
// pointer at +100 (g167:13519, 13539); the clear color vector is at kColorVector.
constexpr std::uint32_t kSp = 0x40060000, kColorVector = 0x40062000;

// The thread's stack: the KPCR at r13 holds the base at +0x70 and the limit at +0x74, and r1 is inside it.
constexpr std::uint32_t kPcr = 0x40070000, kStackLimit = kSp - 0x1000, kStackBase = kSp + 0x1000;

// A ring segment with room and a pool whose fixed buffer ends at 0x00204000.
constexpr std::uint32_t kCursor = 0x00200000, kLimit38 = 0x00203F00, kLimit34 = kLimit38 + 160;
constexpr std::uint32_t kPoolObject = 0x40040000, kPoolBase = 0x00203000, kPoolSize = 0x1000;
constexpr std::uint32_t kBuffer = 0x00203FE8, kSideBuffer = 24;

// Null pool uses G+0x3A3C, G+0x3A4C and G+0x3A50; zero G+0x3A44 starts at G+0x34+60.
constexpr std::uint32_t kSideEnd = 0x00205000, kWrapPos = 0x00204200, kWrapLimit = 0x00204800, kSegment = 5, kProgressBlock = 0x40050000;
constexpr std::uint32_t kNullEnd = kLimit34 + 60 + kSideBuffer;

using Memory = std::unordered_map<std::uint32_t, std::uint32_t>;

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

void setColor(Memory& memory, std::uint32_t address, const std::array<float, 4>& color)
{
    for (std::uint32_t i = 0; i < 4; ++i)
        memory[address + 4 * i] = bitsOf(color[i]);
}

void placeStack(Memory& memory, std::uint32_t limit, std::uint32_t base)
{
    memory[kPcr + 0x70] = base;
    memory[kPcr + 0x74] = limit;
}

// The words the clear blocks read: the stack words, G+0x2940, the depth scales,
// 1.0, the default color (0, 1, 0.5, 0.25) and the channel widths of formats 0 and 2.
void clearInputs(Memory& memory)
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

// Admitted calls read surfaces, fetch/format tables, watched/tiling/clear words and clear
// blocks; format-1 shadows refuse.
Memory admitted()
{
    Memory memory;
    for (std::uint32_t slot = 0; slot < 4; ++slot)
    {
        memory[kG + 0x3098 + 4 * slot]         = kSurface + 0x100 * slot;
        memory[kSurface + 0x100 * slot + 0x18] = 0x500;
    }
    memory[kG + 0x30A8]             = kSurface + 0x400;
    memory[kSurface + 0x400 + 0x18] = 0x500;
    memory[kHeader + 28]            = 0x0A000000;
    memory[kHeader + 32]            = 0x1E3B0006;
    memory[kHeader + 36]            = 0x0059E4FF;
    memory[kHeader + 40]            = 0;
    memory[kHeader + 48]            = 0;
    memory[kResolveDefaultPoint] = memory[kResolveDefaultPoint + 4] = 0;
    for (std::uint32_t format = 0; format < 64; ++format)
    {
        const auto address = kResolveFormatTable + 2 * format;
        auto&      word    = memory[address & ~3U];
        const auto entry   = (1 + format % 8) << 3;
        word               = (address & 2) ? (word & 0xFFFF0000U) | entry : (word & 0x0000FFFFU) | (entry << 16);
    }
    for (std::uint32_t k = 0; k <= 8; ++k)
        memory[kG + 0x2884 + 4 * k] = 0x10000;
    for (std::uint32_t i = 0; i < 4; ++i)
        memory[kG + 0x2A18 + 4 * i] = 0x11110000 + i;
    memory[kG + 0x10]         = 0xFC002000;
    memory[kG + 0x14]         = 0x4003FFFF;
    memory[kG + 0x18]         = 0x80700000;
    memory[kG + 0x1C]         = 0x00000007;
    memory[kG + 0x20]         = 0x40000020;
    memory[kG + 0x24]         = 0x00000004;
    memory[kG + 0x2A44]       = 0x77;
    memory[kG + 0x2ABC]       = 0x00000F00; // tiling byte 0, allocation byte 0, state byte 0x0F
    memory[kG + 0x2A9C]       = 0x1234;
    memory[kG + 0x31AC]       = 0;
    memory[kG + 0x33B0]       = 0;
    memory[kG + 0x2A28]       = 0xAAAA0000;
    memory[kG + 0x2A2C]       = 0xAAAA0001;
    memory[kG + 0x2A30]       = 0xBBBB0002;
    memory[kG + 0x2A34]       = 0xCCCC0003;
    memory[kHeader + 8]       = 0x1111;
    memory[kHeader + 12]      = 0x2222;
    memory[kG + 0x30]         = kCursor;
    memory[kG + 0x34]         = kLimit34;
    memory[kG + 0x38]         = kLimit38;
    memory[kG + 0x3A44]       = 0;
    memory[kG + 0x3A4C]       = 0;
    memory[kG + 0x3A54]       = 0x100;
    memory[kG + 0x3A58]       = 0x1000;
    memory[kG + 0x31CC]       = 4;
    memory[kG + 0x34CC]       = kPoolObject;
    memory[kPoolObject + 152] = kPoolBase;
    memory[kPoolObject + 156] = kPoolSize;
    clearInputs(memory);
    placeStack(memory, kStackLimit, kStackBase);
    return memory;
}

// The same call with a null pool object and the allocator words around it.
void makeNullPool(Memory& memory)
{
    // 0x8269B618 takes 24 bytes from G+0x34 + 60 with G+0x3A44 zero; segment G+0x3A48 is
    // 5 with matching GPU progress.
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

constexpr std::array<std::uint32_t, 8> kRegisters{ kG, 0, 0, kHeader, 0, 0, 0, 0 };

ResolveConsumeState readyState()
{
    ResolveConsumeState state;
    state.stubsReady  = true;
    state.nested      = false;
    state.ledgerReady = true;
    state.entryState  = true;
    state.diagnostics = false;
    state.sp          = kSp;
    state.pcr         = kPcr;
    return state;
}

struct Call
{
    ResolveConsumeState          state  = readyState();
    Memory                       memory = admitted();
    std::array<std::uint32_t, 8> r{ kRegisters };
    ResolveClearArgs             clear{ kSp, 1.0 };
    bool                         withClear = true;

    // A target's shadow word G+0x2884 holding color format 0 with no bias.
    void ModelColor()
    {
        memory[kG + 0x2884] = 0;
    }

    std::optional<ResolveFallback> Verdict(ResolvePlan* plan = nullptr) const
    {
        return ResolveConsumeVerdict(state, readerOf(memory), r, plan, withClear ? &clear : nullptr);
    }
};

struct Reason
{
    ResolveFallback            reason;
    std::function<void(Call&)> trigger;
};

// In verdict order; each trigger breaks only its own condition.
const std::vector<Reason>& reasons()
{
    static const std::vector<Reason> list = {
        { ResolveFallback::Stubs, [](Call& c)
          {
              c.state.stubsReady = false;
          } },
        { ResolveFallback::Nested, [](Call& c)
          {
              c.state.nested = true;
          } },
        { ResolveFallback::Ledger, [](Call& c)
          {
              c.state.ledgerReady = false;
          } },
        { ResolveFallback::EntryState, [](Call& c)
          {
              c.state.entryState = false;
          } },
        { ResolveFallback::Diagnostics, [](Call& c)
          {
              c.state.diagnostics = true;
          } },
        { ResolveFallback::StackOverlap, [](Call& c)
          {
              placeStack(c.memory, kG - 0x1000, kStackBase); // the thread's stack runs over the device block
              c.state.sp = kG + 0x40;
          } },
        { ResolveFallback::Tiling, [](Call& c)
          {
              c.memory[kG + 0x2ABC] |= 0x10000000;
          } },
        { ResolveFallback::TilingPredicate, [](Call& c)
          {
              c.memory[kG + 0x31AC] = 1;
          } },
        { ResolveFallback::TilingState, [](Call& c)
          {
              c.memory[kG + 0x2ABC] |= 0x20000000;
          } },
        { ResolveFallback::DeriveUnsupported, [](Call& c)
          {
              c.r[5] = 1;
          } },
        // The watched words are read right after the derivation, before the fence test.
        { ResolveFallback::Unreadable, [](Call& c)
          {
              c.memory.erase(kG + 0x2A2C);
          } },
        { ResolveFallback::Fence, [](Call& c)
          {
              c.memory[kG + 0x2A9C] = 0;
          } },
        { ResolveFallback::AllocMarker, [](Call& c)
          {
              c.memory[kG + 0x2ABC] |= 0x00200000;
          } },
        { ResolveFallback::ClearOverlap, [](Call& c)
          {
              c.r[1]              = 0x200;
              c.memory[kSp + 100] = kG + 0x2A18;
          } },
        { ResolveFallback::ClearColor, [](Call& c)
          {
              c.r[1] = 0x100;
          } },
        { ResolveFallback::AllocPath, [](Call& c)
          {
              c.memory[kPoolObject + 152] = 0;
          } },
        { ResolveFallback::RingSpace, [](Call& c)
          {
              c.memory[kG + 0x30] = kLimit38 - 16;
          } },
        { ResolveFallback::AllocCapacity, [](Call& c)
          {
              c.memory[kG + 0x3A44] = kCursor + 64;
          } },
    };
    return list;
}

void testAdmitted()
{
    Call        call;
    ResolvePlan plan;
    const auto  verdict = call.Verdict(&plan);
    UNSCOPED_INFO(std::string("the fixture is admitted, got ") + (verdict ? std::string(kResolveFallbackNames[static_cast<std::size_t>(*verdict)]) : ""));
    REQUIRE(!verdict);
    const auto read  = readerOf(call.memory);
    const auto words = DeriveResolveWords(read, kRegisters);
    UNSCOPED_INFO("the plan carries the derived copy words");
    REQUIRE((words && plan.effects.words == *words));
    ResolveWatch watch;
    UNSCOPED_INFO("the plan carries the watched words at the entry");
    REQUIRE((ReadResolveWatch(read, kG, kHeader, watch) && plan.watch.dirty == watch.dirty && plan.watch.fence == 0x1234));
    const auto exit = DeriveResolveExitState(read, kRegisters);
    UNSCOPED_INFO("the plan carries the exact exit");
    REQUIRE((exit && plan.exit.dirty == exit->dirty && plan.exit.copy == exit->copy && plan.exit.dest8 == 0x1234 && plan.exit.stateByte == 0x07));
    UNSCOPED_INFO("the clear words are read at the entry and kept without clear flags");
    REQUIRE((plan.watch.clear == std::array<std::uint32_t, 4>{ 0xAAAA0000, 0xAAAA0001, 0xBBBB0002, 0xCCCC0003 } && plan.exit.clear == plan.watch.clear));
    UNSCOPED_INFO("the plan carries the pool moves");
    REQUIRE((plan.exit.limit34 == kLimit34 - kSideBuffer && plan.exit.limit38 == kLimit38 - kSideBuffer && plan.exit.poolPos == kBuffer && plan.exit.poolUsed == 0x100 + kSideBuffer));
    UNSCOPED_INFO("the plan carries the ring cursor");
    REQUIRE(plan.watch.cursor == kCursor);
    const auto copy = ResolvePlanCopy(plan);
    UNSCOPED_INFO("the copy hook sees the derived copy words, not the stale ones in memory");
    REQUIRE((copy.copyControl == words->copyControl && copy.destBase == words->destBase && copy.destPitch == words->destPitch && copy.destInfo == words->destInfo));
    UNSCOPED_INFO("the copy hook sees the clear words of the device");
    REQUIRE((copy.depthClear == 0xAAAA0001 && copy.colorClear == 0xBBBB0002 && copy.colorClearLow == 0xCCCC0003));
    UNSCOPED_INFO("the fixture's stale copy word differs from the derived one");
    REQUIRE(words->copyControl != 0x11110000);

    // Without a plan the verdict is the same.
    UNSCOPED_INFO("the verdict needs no plan");
    REQUIRE(!call.Verdict());
}

void testEachReason()
{
    for (const auto& reason : reasons())
    {
        Call call;
        reason.trigger(call);
        const auto verdict = call.Verdict();
        const auto name    = std::string(kResolveFallbackNames[static_cast<std::size_t>(reason.reason)]);
        UNSCOPED_INFO("reason " + name + " is reported, got " + (verdict ? std::string(kResolveFallbackNames[static_cast<std::size_t>(*verdict)]) : "admit"));
        REQUIRE(verdict == reason.reason);
    }
    // alloc_wait needs a null pool object, which the fixed pool of these fixtures excludes (testNullPool).
    UNSCOPED_INFO("one fixture per reason");
    REQUIRE(reasons().size() + 1 == kResolveFallbackCount);
}

void testOrder()
{
    const auto& list = reasons();
    for (std::size_t first = 0; first < list.size(); ++first)
    {
        Call call;
        for (std::size_t i = first; i < list.size(); ++i)
            list[i].trigger(call);
        const auto verdict = call.Verdict();
        UNSCOPED_INFO(std::format("with {} and every later reason set the earliest is reported, got {}", kResolveFallbackNames[static_cast<std::size_t>(list[first].reason)], verdict ? kResolveFallbackNames[static_cast<std::size_t>(*verdict)] : "admit"));
        REQUIRE(verdict == list[first].reason);
    }
}

// One store of the skipped Original's prologue chain: `bytes` at `address`.
struct Store
{
    std::uint32_t address, bytes;
};

// What the body 0x8269E520 stores below the entry r1 `sp` before it reads any device word:
// __savegprlr_14 (r14..r31 at r1-152..-8, the link register at r1-8, g167:13044-13045), f31 at
// r1-160 and its back chain at r1-368 (g167:13048-13051).
std::vector<Store> prologueStores(std::uint32_t sp)
{
    return { { sp - 152, 144 }, { sp - 8, 4 }, { sp - 160, 8 }, { sp - 368, 4 } };
}

void applyStores(Memory& memory, const std::vector<Store>& stores)
{
    for (const auto& store : stores)
        for (std::uint32_t a = store.address & ~3U; a < store.address + store.bytes; a += 4)
            memory[a] = 0xDEADBEEF;
}

// Copies the words of the object at `from` to `to`, for the bytes the fixture holds.
void moveObject(Memory& memory, std::uint32_t from, std::uint32_t to, std::uint32_t bytes)
{
    for (std::uint32_t offset = 0; offset < bytes; offset += 4)
    {
        const auto found = memory.find(from + offset);
        if (found != memory.end())
            memory[to + offset] = found->second;
    }
}

// Resolve stack [limit,r1+104) overlaps device/object or unknown KPCR bounds -> refusal.
void testStackOverlap()
{
    // stack_guard.h refuses unknown bounds and device/object overlap.
    UNSCOPED_INFO("the Resolve footprint: 104 bytes above r1, the device up to G+0x3A5C");
    REQUIRE((kResolveStackFootprint.above == 104 && kResolveStackFootprint.deviceSpan == 0x3A5C && kResolveDeviceSpan == 0x3A5C));
    using Setup = std::function<void(Call&, std::uint32_t)>;
    // A call whose stack is [limit, sp + 0x1000) with r1 = `sp`; `setup` edits the call first.
    const auto verdictOf = [](std::uint32_t limit, std::uint32_t sp, const Setup& setup = {})
    {
        Call call;
        call.state.sp = sp;
        if (setup)
            setup(call, sp);
        placeStack(call.memory, limit, sp + 0x1000);
        return call.Verdict();
    };
    const auto refused = [&](std::uint32_t limit, std::uint32_t sp, const Setup& setup = {})
    {
        return verdictOf(limit, sp, setup) == ResolveFallback::StackOverlap;
    };
    constexpr std::uint32_t kSpan = 0x3A5C, kBlockEnd = kG + kSpan, kAbove = 104, kFrame = 368;

    // The device block [G, G + 0x3A5C) against the half-open reach [limit, sp + 104), at the pinned edges.
    UNSCOPED_INFO("a reach ending exactly at G is consumed");
    REQUIRE(!verdictOf(kG - 0x1000, kG - kAbove));
    UNSCOPED_INFO("a reach one byte into the block is refused");
    REQUIRE(refused(kG - 0x1000, kG - kAbove + 1));
    UNSCOPED_INFO("a reach starting exactly at G + 0x3A5C is consumed");
    REQUIRE(!verdictOf(kBlockEnd, kBlockEnd + 0x1000));
    UNSCOPED_INFO("a reach starting one byte inside the block is refused");
    REQUIRE(refused(kBlockEnd - 1, kBlockEnd + 0x1000));
    UNSCOPED_INFO("stacks over or inside the block are refused");
    REQUIRE((refused(kG - 0x1000, kBlockEnd + 0x1000) && refused(kG + 0x100, kG + 0x200)));
    UNSCOPED_INFO("stacks away from the block are consumed");
    REQUIRE((!verdictOf(kSp - 0x1000, kSp) && !verdictOf(kG - 0x3000, kG - 0x2000) && !verdictOf(kBlockEnd + 0x2000, kBlockEnd + 0x4000)));
    UNSCOPED_INFO("the pool words G+0x3500..G+0x3A5B are in the footprint");
    REQUIRE((refused(kG + 0x3500, kG + 0x3600) && refused(kG + 0x3A58, kG + 0x3A60)));
    // The 104 bytes above r1 are in the reach (the stencil and the parameter pointer are read there).
    UNSCOPED_INFO("a reach whose caller frame reaches the block is refused");
    REQUIRE(refused(kBlockEnd + 0x100, kG + 0x100));

    // The no-overflow domain: the bytes between the limit and r1 must hold the entry function's own frame (stwu
    // r1,-368(r1), g167:13049). Fewer is refused, wherever the stack lies; r1 at the limit is the case the body's
    // saves would store below the limit in.
    UNSCOPED_INFO("the Resolve frame is the entry function's 368 bytes");
    REQUIRE(kResolveStackFootprint.frame == 368);
    UNSCOPED_INFO("exactly the frame between the limit and r1 is consumed");
    REQUIRE(!verdictOf(kSp - 368, kSp));
    UNSCOPED_INFO("one byte short of the frame is refused");
    REQUIRE(refused(kSp - 367, kSp));
    UNSCOPED_INFO("r1 at the limit, or a word above it, is refused");
    REQUIRE((refused(kSp - 4, kSp) && refused(kSp, kSp)));

    // The pool object G+0x34CC names sits at r1 - 160, where the body's saves land (the link
    // register save at r1 - 8 overwrites the pool base word at +152): the stores change the word the model
    // reads, and the call is refused whether or not the object's words can be read.
    {
        Call                plain;
        const std::uint32_t pool = kSp - 160;
        moveObject(plain.memory, kPoolObject, pool, 160);
        plain.memory[kG + 0x34CC] = pool;
        Memory after              = plain.memory;
        applyStores(after, prologueStores(kSp));
        UNSCOPED_INFO("the body's link register save overwrites the pool base word");
        REQUIRE(after[pool + 152] != plain.memory[pool + 152]);
        UNSCOPED_INFO("a pool object in the frame is refused");
        REQUIRE(plain.Verdict() == ResolveFallback::StackOverlap);
        plain.memory.erase(pool + 152);
        plain.memory.erase(pool + 156);
        UNSCOPED_INFO("the pool object is tested before its words are read");
        REQUIRE(plain.Verdict() == ResolveFallback::StackOverlap);
    }

    // Every pointed object and register-named object of the model. The reach [sp - 368, sp + 104) ends at the
    // object's first byte (clear), or one byte later (refused); and starts at its last byte + 1 (clear), or one
    // byte earlier (refused). `place` names the object from the registers or the words, as the call does.
    struct Object
    {
        const char*   name;
        std::uint32_t bytes;
        std::uint32_t address;
        Setup         place;
    };

    static constexpr std::uint32_t kRectangle = 0x40080000, kPoint = 0x40080400, kColor = 0x40080800, kParameters = 0x40080C00;
    const std::vector<Object>      objects = {
        { "header r6", 52, kHeader, {} },
        { "rectangle r5", 8, kRectangle, [](Call& c, std::uint32_t)
          {
              c.memory[kRectangle] = c.memory[kRectangle + 4] = 0;
              c.r[2]                                          = kRectangle;
          } },
        { "point r7", 8, kPoint, [](Call& c, std::uint32_t)
          {
              c.memory[kPoint] = c.memory[kPoint + 4] = 0;
              c.r[4]                                  = kPoint;
          } },
        { "color r10", 16, kColor, [](Call& c, std::uint32_t)
          {
              c.r[7] = kColor;
          } },
        { "color r10, unaligned", 16, kColor, [](Call& c, std::uint32_t)
          {
              c.r[7] = kColor + 5;
          } },
        { "source surface word +0x18, slot 0", 4, kSurface + 0x18, {} },
        { "source surface word +0x18, slot 3", 4, kSurface + 0x300 + 0x18, [](Call& c, std::uint32_t)
          {
              c.r[1] = 3;
          } },
        { "source surface word +0x18, depth slot 4", 4, kSurface + 0x400 + 0x18, [](Call& c, std::uint32_t)
          {
              c.r[1] = 4;
          } },
        { "parameter block named by r1+100", 12, kParameters, [](Call& c, std::uint32_t sp)
          {
              c.memory[kParameters] = c.memory[kParameters + 4] = c.memory[kParameters + 8] = 0;
              c.memory[sp + 100]                                                            = kParameters;
          } },
        { "pool object G+0x34CC", 160, kPoolObject, {} },
        { "progress word of the block G+0x2A90 names", 4, kProgressBlock + 4, [](Call& c, std::uint32_t)
          {
              makeNullPool(c.memory);
          } },
    };
    for (const auto& object : objects)
    {
        const std::string name = object.name;
        // sp + 104 = `end`, the reach [end - 104 - kFrame, end).
        const auto endsAt = [&](std::uint32_t end)
        {
            return refused(end - kAbove - kFrame, end - kAbove, object.place);
        };
        // The reach [limit, limit + kFrame + 104).
        const auto startsAt = [&](std::uint32_t limit)
        {
            return refused(limit, limit + kFrame, object.place);
        };
        UNSCOPED_INFO(name + ": a reach ending exactly at the object is clear");
        REQUIRE(!endsAt(object.address));
        UNSCOPED_INFO(name + ": a reach one byte into the object is refused");
        REQUIRE(endsAt(object.address + 1));
        UNSCOPED_INFO(name + ": a reach starting exactly after the object is clear");
        REQUIRE(!startsAt(object.address + object.bytes));
        UNSCOPED_INFO(name + ": a reach starting on the object's last byte is refused");
        REQUIRE(startsAt(object.address + object.bytes - 1));
        // The refusal does not depend on a word of the object being readable.
        Call call;
        call.state.sp = object.address + kFrame;
        if (object.place)
            object.place(call, call.state.sp);
        for (std::uint32_t offset = 0; offset < object.bytes; offset += 4)
            call.memory.erase(object.address + offset);
        placeStack(call.memory, object.address, object.address + 0x1000);
        UNSCOPED_INFO(name + ": a reach over the object is refused");
        REQUIRE(call.Verdict() == ResolveFallback::StackOverlap);
    }

    // The fixed words of the title image the model reads: a reach that holds a byte is refused, one that
    // touches is not (unless it holds another of them).
    const auto holdsStatic = [](std::uint64_t low, std::uint64_t high)
    {
        for (const auto& object : kResolveStackStatics)
            if (low < object.address + object.bytes && object.address < high)
                return true;
        return false;
    };
    for (const auto& object : kResolveStackStatics)
    {
        const std::string name = std::format("static {:08X}", object.address);
        const auto        end = object.address, after = object.address + object.bytes;
        UNSCOPED_INFO(name + ": overlapping reaches are refused");
        REQUIRE((refused(end - kAbove - kFrame + 1, end - kAbove + 1) && refused(after - 1, after + kFrame)));
        if (!holdsStatic(end - kAbove - kFrame, end))
        {
            UNSCOPED_INFO(name + ": a reach ending at it is clear");
            REQUIRE(!refused(end - kAbove - kFrame, end - kAbove));
        }
        if (!holdsStatic(after, after + kFrame + kAbove))
        {
            UNSCOPED_INFO(name + ": a reach starting after it is clear");
            REQUIRE(!refused(after, after + kFrame));
        }
    }

    // Unknown bounds are refused like an overlap.
    {
        const auto unknown = [](const std::function<void(Call&)>& setup)
        {
            Call call;
            setup(call);
            return call.Verdict();
        };
        UNSCOPED_INFO("r13 zero");
        REQUIRE(unknown([](Call& c)
                        {
                            c.state.pcr = 0;
                        }) == ResolveFallback::StackOverlap);
        UNSCOPED_INFO("an unreadable limit");
        REQUIRE(unknown([](Call& c)
                        {
                            c.memory.erase(kPcr + 0x74);
                        }) == ResolveFallback::StackOverlap);
        UNSCOPED_INFO("an unreadable base");
        REQUIRE(unknown([](Call& c)
                        {
                            c.memory.erase(kPcr + 0x70);
                        }) == ResolveFallback::StackOverlap);
        UNSCOPED_INFO("a zero limit");
        REQUIRE(unknown([](Call& c)
                        {
                            placeStack(c.memory, 0, kStackBase);
                        }) == ResolveFallback::StackOverlap);
        UNSCOPED_INFO("an inverted range");
        REQUIRE(unknown([](Call& c)
                        {
                            placeStack(c.memory, kStackBase, kStackLimit);
                        }) == ResolveFallback::StackOverlap);
        UNSCOPED_INFO("r1 below the limit");
        REQUIRE(unknown([](Call& c)
                        {
                            c.state.sp = kStackLimit - 4;
                        }) == ResolveFallback::StackOverlap);
        UNSCOPED_INFO("r1 above the base");
        REQUIRE(unknown([](Call& c)
                        {
                            c.state.sp = kStackBase + 4;
                        }) == ResolveFallback::StackOverlap);
        UNSCOPED_INFO("a KPCR in the frames");
        REQUIRE(unknown([](Call& c)
                        {
                            c.state.pcr                 = kSp - 0x40;
                            c.memory[kSp - 0x40 + 0x70] = kStackBase;
                            c.memory[kSp - 0x40 + 0x74] = kStackLimit;
                        }) == ResolveFallback::StackOverlap);
        Call ok;
        UNSCOPED_INFO("the fixture's stack is known");
        REQUIRE(!ok.Verdict());
    }

    // The bounds decide, not a computed depth: a very deep stack that holds no model word is consumed.
    UNSCOPED_INFO("a deep stack is consumed when it holds no model word");
    REQUIRE(!verdictOf(kSp - 0x5000, kSp));

    // The stores of the body's prologue change words the model reads wherever they land in the device: a
    // stack with r1 over a model word is refused.
    const std::vector<std::uint32_t> read = { kG + 0x10, kG + 0x30, kG + 0x38, kG + 0x2A2C, kG + 0x2A9C, kG + 0x2ABC, kG + 0x3A44, kG + 0x34CC, kG + 0x3A58 };
    for (const auto word : read)
        for (const std::uint32_t sp : { word + 8, word + 152, word + 160, word + 368 })
        {
            Call call;
            call.state.sp = sp;
            placeStack(call.memory, kG - 0x1000, kBlockEnd + 0x2000);
            Memory after = call.memory;
            applyStores(after, prologueStores(sp));
            UNSCOPED_INFO(std::format("the saves change G+{:#x} (sp {:#x})", word - kG, sp));
            REQUIRE(after[word] != call.memory[word]);
            UNSCOPED_INFO(std::format("a stack over G+{:#x} is refused (sp {:#x})", word - kG, sp));
            REQUIRE(call.Verdict() == ResolveFallback::StackOverlap);
        }
    {
        // The stores alone turn the tiling word into a tiled device: another verdict. The refusal comes first.
        Call plain;
        UNSCOPED_INFO("the unchanged device is consumed");
        REQUIRE(!plain.Verdict());
        const std::uint32_t sp = kG + 0x2ABC + 368;
        Call                changed;
        applyStores(changed.memory, prologueStores(sp));
        UNSCOPED_INFO("the stores alone change the verdict");
        REQUIRE(changed.Verdict() != std::optional<ResolveFallback>{});
        placeStack(changed.memory, kG - 0x1000, kBlockEnd + 0x2000);
        changed.state.sp = sp;
        UNSCOPED_INFO("the refusal comes before the changed words are read");
        REQUIRE(changed.Verdict() == ResolveFallback::StackOverlap);
    }

    // Over every sp around the block, with a reach of exactly 0x1000 below r1: the reach decides.
    for (std::uint32_t sp = kG - 0x1400; sp < kBlockEnd + 0x1400; sp += 4)
    {
        const bool aliases = sp + kAbove > kG && sp - 0x1000 < kBlockEnd;
        UNSCOPED_INFO(std::format("the reach decides (sp {:#x})", sp));
        REQUIRE((aliases ? refused(sp - 0x1000, sp) : !verdictOf(sp - 0x1000, sp)));
    }

    // The order: the state reasons first, then the stack before any guest word of the model is read (the tiling
    // words, the derivation, the watch, the pointers), so an unreadable word cannot mask it.
    {
        Call call;
        placeStack(call.memory, kG - 0x1000, kStackBase);
        call.state.sp          = kG + 0x40;
        call.state.diagnostics = true;
        UNSCOPED_INFO("the state reasons come before the stack");
        REQUIRE(call.Verdict() == ResolveFallback::Diagnostics);
        call.state.diagnostics = false;
        call.memory[kG + 0x2ABC] |= 0x10000000;
        call.r[5] = 1;
        UNSCOPED_INFO("the stack comes before the tiling words and the derivation");
        REQUIRE(call.Verdict() == ResolveFallback::StackOverlap);
        for (const std::uint32_t word : { kG + 0x2ABC, kG + 0x3098, kG + 0x30A8, kG + 0x34CC, kG + 0x2A90, kG + 0x2A2C, kG + 0x10, kHeader + 28, kResolveFormatTable })
        {
            Call masked;
            placeStack(masked.memory, kG - 0x1000, kStackBase);
            masked.state.sp = kG + 0x40;
            masked.memory.erase(word);
            UNSCOPED_INFO(std::format("an unreadable {:08X} cannot mask the stack", word));
            REQUIRE(masked.Verdict() == ResolveFallback::StackOverlap);
        }
        Call high;
        high.r[0]     = 0xFFFFFFF0U;
        high.state.sp = 0xFFFFFFF8U;
        UNSCOPED_INFO("a device address whose span wraps is unreadable before the stack");
        REQUIRE(high.Verdict() == ResolveFallback::Unreadable);
    }

    // The pointer word is read after the stack and the device words are tested, then its object: a parameter
    // block named inside the frames is found whatever the later words hold.
    {
        Call call;
        call.memory[kSp + 100] = kSp - 100;
        for (std::uint32_t i = 0; i < 3; ++i)
            call.memory[kSp - 100 + 4 * i] = 0;
        UNSCOPED_INFO("the parameter block named in the frames is refused");
        REQUIRE(call.Verdict() == ResolveFallback::StackOverlap);
    }

#if REREVVED_DEVELOPER
    // The counter and the effect probe's reason.
    TakeResolveConsumeCounts();
    NoteResolveFallback(ResolveFallback::StackOverlap);
    const auto counts = TakeResolveConsumeCounts();
    UNSCOPED_INFO("counted");
    REQUIRE((counts.consumed == 0 && counts.fallback[static_cast<std::size_t>(ResolveFallback::StackOverlap)] == 1));
    UNSCOPED_INFO("the counter name: " + FormatResolveConsumeCounts(counts));
    REQUIRE(FormatResolveConsumeCounts(counts) == " resolve_consumed=0 resolve_fallback_stack_overlap=1");
    UNSCOPED_INFO("the effect probe reason");
    REQUIRE(std::string(kResolveExitRefusalNames[static_cast<std::size_t>(ResolveExitRefusal::StackOverlap)]) == "stack_overlap");
#endif
}

void testTilingWords()
{
    // Only the 0x30 bits of the tiling byte, the predicate and the bracket send a call to Original.
    for (const std::uint32_t top : { 0x01U, 0x02U, 0x04U, 0x08U, 0x40U, 0x80U, 0xCFU })
    {
        Call call;
        call.memory[kG + 0x2ABC] = (top << 24) | 0x00000F00;
        UNSCOPED_INFO(std::format("tiling byte {:02X} is not tiling", top));
        REQUIRE(!call.Verdict());
    }
    // Bit 0x10 is Tiling whatever else is set, with the cvar on or off.
    for (const std::uint32_t top : { 0x10U, 0x30U, 0xF0U, 0x3FU })
    {
        for (const bool tiled : { false, true })
        {
            Call call;
            call.state.tiled         = tiled;
            call.memory[kG + 0x2ABC] = (top << 24) | 0x00000F00;
            call.memory[kG + 0x33B0] = 0x80000000;
            UNSCOPED_INFO(std::format("tiling byte {:02X} with the bracket word, cvar {}", top, tiled));
            REQUIRE(call.Verdict() == ResolveFallback::Tiling);
        }
    }
    // The byte with bit 0x20 and the bracket word disagree: tiling_state, on or off.
    for (const bool tiled : { false, true })
    {
        Call call;
        call.state.tiled         = tiled;
        call.memory[kG + 0x2ABC] = 0x20000F00;
        UNSCOPED_INFO("byte 0x20 without the bracket word");
        REQUIRE(call.Verdict() == ResolveFallback::TilingState);
        call.memory[kG + 0x2ABC] = 0x00000F00;
        call.memory[kG + 0x33B0] = 0x40;
        UNSCOPED_INFO("the bracket word without the byte");
        REQUIRE(call.Verdict() == ResolveFallback::TilingState);
    }
    // The predicate is judged after bit 0x10 and before the disagreement; in the open bracket it is the
    // per-tile form, admitted with the cvar on and tiling_predicate with it off (testPredicatedBracket).
    for (const bool tiled : { false, true })
    {
        Call call;
        call.state.tiled         = tiled;
        call.memory[kG + 0x31AC] = 1;
        UNSCOPED_INFO("the predicate word alone");
        REQUIRE(call.Verdict() == ResolveFallback::TilingPredicate);
        call.memory[kG + 0x2ABC] = 0x20000F00;
        UNSCOPED_INFO("the predicate before the disagreement");
        REQUIRE(call.Verdict() == ResolveFallback::TilingPredicate);
        call.memory[kG + 0x33B0] = 0x80000000;
        ResolvePlan plan;
        if (tiled)
        {
            UNSCOPED_INFO("the predicate in the bracket form is admitted with the cvar on");
            REQUIRE((!call.Verdict(&plan) && plan.predicated));
        }
        else
        {
            UNSCOPED_INFO("the predicate in the bracket form with the cvar off");
            REQUIRE(call.Verdict() == ResolveFallback::TilingPredicate);
        }
        call.memory[kG + 0x2ABC] = 0x30000F00;
        UNSCOPED_INFO("bit 0x10 before the predicate");
        REQUIRE(call.Verdict() == ResolveFallback::Tiling);
    }
    // The bracket form: refused as tiling while the cvar is off, admitted when it is on.
    for (const std::uint32_t top : { 0x20U, 0x21U, 0x2FU, 0x60U, 0xEFU })
    {
        Call call;
        call.memory[kG + 0x2ABC] = (top << 24) | 0x00000F00;
        call.memory[kG + 0x33B0] = 0x80000000;
        ResolvePlan plan;
        UNSCOPED_INFO(std::format("bracket form {:02X} with the cvar off", top));
        REQUIRE(call.Verdict(&plan) == ResolveFallback::Tiling);
        call.state.tiled = true;
        UNSCOPED_INFO(std::format("bracket form {:02X} with the cvar on", top));
        REQUIRE((!call.Verdict(&plan) && plan.tiled));
    }

    // Tiling is judged before the clear color, whichever word set it: the exit state would refuse the shape too, so only the order tells the checks apart.
    const struct
    {
        std::uint32_t   address, value;
        ResolveFallback want;
    } words[] = { { kG + 0x31AC, 1, ResolveFallback::TilingPredicate }, { kG + 0x33B0, 1, ResolveFallback::TilingState }, { kG + 0x2ABC, 0x30000F00, ResolveFallback::Tiling } };

    for (const auto& word : words)
    {
        Call call;
        call.state.tiled          = true;
        call.memory[word.address] = word.value;
        call.r[1]                 = 0x300;
        UNSCOPED_INFO(std::format("{:08X} set with an unmodeled clear color is tiling first", word.address));
        REQUIRE(call.Verdict() == word.want);
    }
    {
        // The bracket form itself does not come before the clear color.
        Call call;
        call.state.tiled         = true;
        call.memory[kG + 0x2ABC] = 0x20000F00;
        call.memory[kG + 0x33B0] = 0x80000000;
        call.r[1]                = 0x300;
        UNSCOPED_INFO("the bracket form with an unmodeled clear color is clear_color");
        REQUIRE(call.Verdict() == ResolveFallback::ClearColor);
        call.ModelColor();
        ResolvePlan plan;
        UNSCOPED_INFO("the bracket form with modeled clear flags is admitted");
        REQUIRE((!call.Verdict(&plan) && plan.tiled && plan.effects.depthClear && plan.effects.colorClear));
    }
    // The verdict reads the three words before anything else of the device.
    for (const std::uint32_t address : { kG + 0x2ABC, kG + 0x31AC, kG + 0x33B0 })
    {
        Call call;
        call.memory.erase(address);
        UNSCOPED_INFO(std::format("{:08X} unreadable", address));
        REQUIRE(call.Verdict() == ResolveFallback::Unreadable);
    }
}

// Predicated bracket Resolve uses untiled plan; off cvar preserves tiling_predicate.
void testPredicatedBracket()
{
    // Per-tile 0x8250AC90 uses the bracket plan and tile predicate.
    const auto predicatedCall = [](bool nullPool, std::uint32_t mask, const std::function<void(Call&)>& change = {})
    {
        Call call;
        call.state.tiled         = true;
        call.memory[kG + 0x2ABC] = 0x20000F00;
        call.memory[kG + 0x33B0] = 0x80000000;
        call.memory[kG + 0x31AC] = 1;
        call.memory[kG + 0x31A4] = mask;
        if (nullPool)
        {
            makeNullPool(call.memory);
            call.memory[kG + 0x33B4] = kSegment + 1;
        }
        if (change)
            change(call);
        return call;
    };
    const auto bracketCall = [&](bool nullPool, const std::function<void(Call&)>& change = {})
    {
        auto call                = predicatedCall(nullPool, 0, change);
        call.memory[kG + 0x31AC] = 0;
        return call;
    };
    const auto sameExit = [](const ResolvePlan& a, const ResolvePlan& b)
    {
        return a.exit.copy == b.exit.copy && a.exit.size == b.exit.size && a.exit.dirty == b.exit.dirty && a.exit.allocByte == b.exit.allocByte && a.exit.stateByte == b.exit.stateByte && a.exit.dest8 == b.exit.dest8 &&
               a.exit.dest12 == b.exit.dest12 && a.exit.limit34 == b.exit.limit34 && a.exit.limit38 == b.exit.limit38 && a.exit.poolPos == b.exit.poolPos && a.exit.poolUsed == b.exit.poolUsed &&
               a.exit.wrapPos == b.exit.wrapPos && a.exit.clear == b.exit.clear;
    };

    // The cvar decides: off keeps the reason of the predicate, on admits every mask the tiles use.
    for (const std::uint32_t mask : { 3U, 0xCU, 0x30U, 0xC0U })
    {
        auto off        = predicatedCall(false, mask);
        off.state.tiled = false;
        UNSCOPED_INFO("the predicated bracket form keeps tiling_predicate with the cvar off");
        REQUIRE(off.Verdict() == ResolveFallback::TilingPredicate);
        ResolvePlan plan;
        auto        on = predicatedCall(false, mask);
        UNSCOPED_INFO(std::format("mask {:X} is admitted with the cvar on", mask));
        REQUIRE((!on.Verdict(&plan) && plan.tiled && plan.predicated));
        UNSCOPED_INFO("the plan watch carries the predicate");
        REQUIRE((plan.watch.Tiling() == ResolveTiling::PredicatedBracket && plan.watch.predicate == 1));
    }
    // The plan equals the bracket plan on every exit word, for both pool kinds and with clear flags.
    for (const bool nullPool : { false, true })
        for (const std::uint32_t flags : { 0U, 0x200U, 0x100U, 0x300U })
        {
            ResolvePlan predicated, plain;
            auto        a     = predicatedCall(nullPool, 0xC, [&](Call& c)
                                               {
                                        c.r[1] = flags;
                                        c.ModelColor();
                                               });
            auto        b     = bracketCall(nullPool, [&](Call& c)
                                            {
                                     c.r[1] = flags;
                                     c.ModelColor();
                                            });
            const auto  label = std::format("null pool {} flags {:X}", nullPool, flags);
            UNSCOPED_INFO("both admitted: " + label);
            REQUIRE((!a.Verdict(&predicated) && !b.Verdict(&plain)));
            UNSCOPED_INFO("only the first is predicated: " + label);
            REQUIRE((predicated.predicated && !plain.predicated && predicated.tiled && plain.tiled));
            UNSCOPED_INFO("the exit words are the bracket form's: " + label);
            REQUIRE(sameExit(predicated, plain));
            UNSCOPED_INFO("the same number of stores: " + label);
            REQUIRE(ResolveExitWrites(kG, kHeader, predicated.watch, predicated.exit).size() == ResolveExitWrites(kG, kHeader, plain.watch, plain.exit).size());
            UNSCOPED_INFO("the same stores: " + label);
            REQUIRE(ResolveExitWrites(kG, kHeader, predicated.watch, predicated.exit) == ResolveExitWrites(kG, kHeader, plain.watch, plain.exit));
        }
    // Every other reason keeps its place with the predicate set, as in the bracket form.
    for (const auto& reason : reasons())
    {
        if (reason.reason == ResolveFallback::Tiling || reason.reason == ResolveFallback::TilingPredicate || reason.reason == ResolveFallback::TilingState)
            continue;
        auto       plain     = bracketCall(false, reason.trigger);
        auto       predicate = predicatedCall(false, 0xC, reason.trigger);
        const auto want      = plain.Verdict();
        UNSCOPED_INFO(std::string("the reason is the bracket form's: ").append(kResolveFallbackNames[static_cast<std::size_t>(reason.reason)]));
        REQUIRE(predicate.Verdict() == want);
    }
    // The shapes outside an open bracket, and bit 0x10, stay refused with the cvar on.
    {
        auto loose = predicatedCall(false, 3, [](Call& c)
                                    {
                                        c.memory[kG + 0x33B0] = 0;
                                    });
        UNSCOPED_INFO("the byte without the bracket word is tiling_predicate");
        REQUIRE(loose.Verdict() == ResolveFallback::TilingPredicate);
        loose = predicatedCall(false, 3, [](Call& c)
                               {
                                   c.memory[kG + 0x2ABC] = 0x00000F00;
                               });
        UNSCOPED_INFO("the bracket word without the byte is tiling_predicate");
        REQUIRE(loose.Verdict() == ResolveFallback::TilingPredicate);
        loose = predicatedCall(false, 3, [](Call& c)
                               {
                                   c.memory[kG + 0x2ABC] = 0x30000F00;
                               });
        UNSCOPED_INFO("bit 0x10 stays tiling");
        REQUIRE(loose.Verdict() == ResolveFallback::Tiling);
        loose = predicatedCall(false, 3, [](Call& c)
                               {
                                   c.memory[kG + 0x31CC] = 17;
                               });
        UNSCOPED_INFO("more than 16 tiles is ring_space");
        REQUIRE(loose.Verdict() == ResolveFallback::RingSpace);
        loose = predicatedCall(false, 3, [](Call& c)
                               {
                                   c.memory[kG + 0x31CC] = 16;
                               });
        UNSCOPED_INFO("16 tiles are admitted");
        REQUIRE(!loose.Verdict());
    }
    // The shape is counted whatever else refuses the call, and untouched by an earlier refusal.
    {
        ResolveShape shape;
        auto         call = predicatedCall(false, 3);
        UNSCOPED_INFO("an admitted predicated call");
        REQUIRE((!ResolveConsumeVerdict(call.state, readerOf(call.memory), call.r, nullptr, &call.clear, &shape) && shape.predicated && !shape.clear && shape.tiles == 4));

        const struct
        {
            const char*                name;
            ResolveFallback            want;
            std::uint32_t              flags, tiles;
            std::function<void(Call&)> change;
        } later[] = {
            { "fence zero", ResolveFallback::Fence, 0, 2, [](Call& c)
              {
                  c.memory[kG + 0x2A9C] = 0;
              } },
            { "allocation marker", ResolveFallback::AllocMarker, 0x100, 3, [](Call& c)
              {
                  c.memory[kG + 0x2ABC] |= 0x00200000;
              } },
            { "unmodeled clear color", ResolveFallback::ClearColor, 0x300, 5, [](Call& c)
              {
                  c.memory[kG + 0x2884] = 0x30000;
              } },
        };

        for (const auto& l : later)
        {
            ResolveShape got;
            auto         c = predicatedCall(false, 3, [&](Call& x)
                                            {
                                        x.r[1]                = l.flags;
                                        x.memory[kG + 0x31CC] = l.tiles;
                                        l.change(x);
                                            });
            UNSCOPED_INFO(std::string("refused: ") + l.name);
            REQUIRE(ResolveConsumeVerdict(c.state, readerOf(c.memory), c.r, nullptr, &c.clear, &got) == l.want);
            UNSCOPED_INFO(std::string("and its shape is kept: ") + l.name);
            REQUIRE((got.predicated && got.clear == ((l.flags & 0x300) != 0) && got.tiles == l.tiles));
        }
        // Either clear bit alone is a clear shape, and none is not.
        for (const std::uint32_t flags : { 0U, 0x100U, 0x200U, 0x400U })
        {
            ResolveShape got;
            auto         c = predicatedCall(false, 4, [&](Call& x)
                                            {
                                        x.r[1] = flags;
                                            });
            ResolveConsumeVerdict(c.state, readerOf(c.memory), c.r, nullptr, &c.clear, &got);
            UNSCOPED_INFO("the clear shape of flags " + std::to_string(flags));
            REQUIRE((got.predicated && got.tiles == 4 && got.clear == ((flags & 0x300) != 0)));
        }
        ResolveShape early;
        auto         stubs = predicatedCall(false, 3, [](Call& c)
                                            {
                                        c.state.stubsReady = false;
                                            });
        UNSCOPED_INFO("an earlier refusal leaves the shape alone");
        REQUIRE((ResolveConsumeVerdict(stubs.state, readerOf(stubs.memory), stubs.r, nullptr, &stubs.clear, &early) == ResolveFallback::Stubs && !early.predicated));
        ResolveShape plainShape;
        auto         plain = bracketCall(false);
        UNSCOPED_INFO("the bracket form without the predicate has no shape");
        REQUIRE((!ResolveConsumeVerdict(plain.state, readerOf(plain.memory), plain.r, nullptr, &plain.clear, &plainShape) && !plainShape.predicated));
        ResolveShape off;
        auto         cvarOff = predicatedCall(false, 3);
        cvarOff.state.tiled  = false;
        UNSCOPED_INFO("and with the cvar off the shape is still seen");
        REQUIRE((ResolveConsumeVerdict(cvarOff.state, readerOf(cvarOff.memory), cvarOff.r, nullptr, &cvarOff.clear, &off) == ResolveFallback::TilingPredicate && off.predicated));
    }
#if REREVVED_DEVELOPER
    // The counters.
    {
        TakeResolveConsumeCounts();
        NoteResolveConsumed(true, true);
        NoteResolveConsumed(true, false);
        NoteResolveShape({ true, true, 3 });
        NoteResolveShape({ true, false, 2 });
        NoteResolveShape({ true, false, 17 });
        NoteResolveShape({ false, true, 3 });
        const auto counts = TakeResolveConsumeCounts();
        UNSCOPED_INFO("the consumed counts");
        REQUIRE((counts.consumed == 2 && counts.consumedTiled == 2 && counts.consumedPredicated == 1));
        UNSCOPED_INFO("the shape counts");
        REQUIRE((counts.shape.predicated == 3 && counts.shape.predicatedClear == 1 && counts.shape.tiles == decltype(counts.shape.tiles){ 1, 1, 0, 1 }));
        UNSCOPED_INFO("formatted: " + FormatResolveConsumeCounts(counts));
        REQUIRE(FormatResolveConsumeCounts(counts) == " resolve_consumed=2 resolve_consumed_tiled=2 resolve_consumed_predicated=1 resolve_predicated=3 resolve_predicated_clear=1 resolve_predicated_tiles_2=1 resolve_predicated_tiles_3=1 resolve_predicated_tiles_4=0 resolve_predicated_tiles_other=1");
    }
#endif
}

void testTiledBracket()
{
    const auto tiledCall = [](bool nullPool, const std::function<void(Call&)>& change = {})
    {
        Call call;
        call.state.tiled         = true;
        call.memory[kG + 0x2ABC] = 0x20000F00;
        call.memory[kG + 0x33B0] = 0x80000000;
        if (nullPool)
            makeNullPool(call.memory);
        if (change)
            change(call);
        return call;
    };
    // The plan equals the untiled plan on every word (fixed pool).
    {
        ResolvePlan plan, untiled;
        Call        plain;
        UNSCOPED_INFO("the untiled plan is not tiled");
        REQUIRE((!plain.Verdict(&untiled) && !untiled.tiled));
        auto call = tiledCall(false);
        UNSCOPED_INFO("a fixed pool in the bracket form is admitted");
        REQUIRE((!call.Verdict(&plan) && plan.tiled));
        UNSCOPED_INFO("the tiled plan carries the untiled exit");
        REQUIRE((plan.exit.dirty == untiled.exit.dirty && plan.exit.copy == untiled.exit.copy && plan.exit.size == untiled.exit.size && plan.exit.dest8 == untiled.exit.dest8 && plan.exit.stateByte == untiled.exit.stateByte &&
                 plan.exit.limit34 == untiled.exit.limit34 && plan.exit.limit38 == untiled.exit.limit38 && plan.exit.poolPos == untiled.exit.poolPos && plan.exit.poolUsed == untiled.exit.poolUsed && plan.exit.clear == untiled.exit.clear));
        UNSCOPED_INFO("the plan watch carries the tiling words");
        REQUIRE((plan.watch.tilingByte == 0x20 && plan.watch.bracket == 0x80000000 && plan.watch.Tiling() == ResolveTiling::Bracket));
    }
    // Null pool: the allocator bracket branch.
    {
        ResolvePlan plan;
        auto        forward = tiledCall(true, [](Call& c)
                                        {
                                     c.memory[kG + 0x33B4] = kSegment + 1;
                                        });
        UNSCOPED_INFO("a bracket forward take is admitted");
        REQUIRE((!forward.Verdict(&plan) && plan.tiled && plan.exit.poolPos == kNullEnd));
        auto behind = tiledCall(true, [](Call& c)
                                {
                                    c.memory[kG + 0x33B4] = kSegment - 1;
                                });
        UNSCOPED_INFO("the next segment one behind fails the allocation");
        REQUIRE(behind.Verdict() == ResolveFallback::AllocCapacity);
        behind.memory[kG + 0x33B4] = kSegment;
        behind.memory[kG + 0x33B0] = kNullEnd - 1;
        UNSCOPED_INFO("the same segment with the bracket word below the end fails");
        REQUIRE(behind.Verdict() == ResolveFallback::AllocCapacity);
        behind.memory[kG + 0x33B0] = kNullEnd;
        UNSCOPED_INFO("the same segment with the bracket word at the end is taken");
        REQUIRE(!behind.Verdict());
        auto untiledBehind = tiledCall(true, [](Call& c)
                                       {
                                           c.state.tiled         = false;
                                           c.memory[kG + 0x2ABC] = 0x00000F00;
                                           c.memory[kG + 0x33B0] = 0;
                                           c.memory[kG + 0x33B4] = kSegment - 1;
                                       });
        UNSCOPED_INFO("without the bracket G+0x33B4 is not looked at");
        REQUIRE(!untiledBehind.Verdict());
        auto waiting = tiledCall(true, [](Call& c)
                                 {
                                     c.memory[kG + 0x33B4]        = kSegment + 1;
                                     c.memory[kProgressBlock + 4] = kSegment - 2;
                                 });
        UNSCOPED_INFO("a tiled call waits like an untiled one");
        REQUIRE(waiting.Verdict() == ResolveFallback::AllocWait);
        // Wrap form.
        auto wrap = tiledCall(true, [](Call& c)
                              {
                                  c.memory[kG + 0x3A44]        = 0x00204000;
                                  c.memory[kG + 0x3A3C]        = 0x00204017;
                                  c.memory[kProgressBlock + 4] = kSegment + 1;
                                  c.memory[kG + 0x33B4]        = kSegment + 1;
                                  c.memory[kG + 0x33B0]        = kWrapPos + 23;
                              });
        UNSCOPED_INFO("wrap: next segment one ahead with the bracket word below the end fails");
        REQUIRE(wrap.Verdict() == ResolveFallback::AllocCapacity);
        wrap.memory[kG + 0x33B0] = kWrapPos + 24;
        UNSCOPED_INFO("wrap: at the end it is taken");
        REQUIRE((!wrap.Verdict(&plan) && plan.tiled && plan.exit.wrapPos == kWrapPos + kSideBuffer && plan.exit.poolPos == 0x00204000));
        // The threshold: no forced flush in the bracket form.
        auto flush = tiledCall(true, [](Call& c)
                               {
                                   c.memory[kG + 0x3A54] = 0x1000;
                                   c.memory[kG + 0x33B4] = kSegment + 1;
                               });
        UNSCOPED_INFO("past the threshold a bracket call has no forced flush");
        REQUIRE(!flush.Verdict());
        flush.state.tiled = false;
        UNSCOPED_INFO("with the cvar off it is refused as tiling");
        REQUIRE(flush.Verdict() == ResolveFallback::Tiling);
        // The ring bound is the same as untiled: 331 words for this entry.
        const std::uint32_t end  = kCursor + 4 * 331;
        auto                edge = tiledCall(true, [&](Call& c)
                                             {
                                  c.memory[kG + 0x38]   = end;
                                  c.memory[kG + 0x34]   = end + 160;
                                  c.memory[kG + 0x33B4] = kSegment + 1;
                                             });
        UNSCOPED_INFO("the tiled words end exactly at the soft limit");
        REQUIRE(!edge.Verdict());
        edge.memory[kG + 0x38] -= 1;
        UNSCOPED_INFO("a byte short of the soft limit");
        REQUIRE(edge.Verdict() == ResolveFallback::RingSpace);
        // A tile count past the bound of the loops.
        auto many = tiledCall(true, [](Call& c)
                              {
                                  c.memory[kG + 0x31CC] = 17;
                                  c.memory[kG + 0x33B4] = kSegment + 1;
                              });
        UNSCOPED_INFO("17 tiles are beyond the bound");
        REQUIRE(many.Verdict() == ResolveFallback::RingSpace);
        // Every word is read.
        auto missing = tiledCall(true, [](Call& c)
                                 {
                                     c.memory.erase(kG + 0x33B4);
                                 });
        UNSCOPED_INFO("G+0x33B4 is read in the bracket form");
        REQUIRE(missing.Verdict() == ResolveFallback::Unreadable);
    }
    // The gates after the tiling words hold in the bracket form, in the reasons' order.
    const std::vector<Reason> list = {
        { ResolveFallback::DeriveUnsupported, [](Call& c)
          {
              c.r[5] = 1;
          } },
        { ResolveFallback::Fence, [](Call& c)
          {
              c.memory[kG + 0x2A9C] = 0;
          } },
        { ResolveFallback::AllocMarker, [](Call& c)
          {
              c.memory[kG + 0x2ABC] |= 0x00200000;
          } },
        { ResolveFallback::ClearOverlap, [](Call& c)
          {
              c.r[1]              = 0x200;
              c.memory[kSp + 100] = kG + 0x2A18;
          } },
        { ResolveFallback::ClearColor, [](Call& c)
          {
              c.r[1] = 0x100;
          } },
        { ResolveFallback::AllocPath, [](Call& c)
          {
              c.memory[kPoolObject + 152] = 0;
          } },
        { ResolveFallback::RingSpace, [](Call& c)
          {
              c.memory[kG + 0x30] = kLimit38 - 16;
          } },
        { ResolveFallback::AllocCapacity, [](Call& c)
          {
              c.memory[kG + 0x3A44] = kCursor + 64;
          } },
    };
    for (std::size_t first = 0; first < list.size(); ++first)
    {
        auto call = tiledCall(false);
        for (std::size_t i = first; i < list.size(); ++i)
            list[i].trigger(call);
        UNSCOPED_INFO(std::format("bracket form: with {} and every later reason set the earliest is reported", kResolveFallbackNames[static_cast<std::size_t>(list[first].reason)]));
        REQUIRE(call.Verdict() == list[first].reason);
    }
    // The state gates before the tiling words still come first.
    for (const auto& reason : reasons())
    {
        if (reason.reason > ResolveFallback::Diagnostics)
            break;
        auto call = tiledCall(false);
        reason.trigger(call);
        UNSCOPED_INFO("state gate " + std::string(kResolveFallbackNames[static_cast<std::size_t>(reason.reason)]) + " in the bracket form");
        REQUIRE(call.Verdict() == reason.reason);
    }
}

// The ring and allocator gates at their edges, through the whole verdict.
void testRingAndAllocator()
{
    // 331 words for this entry: 295 for the fixed blocks of four render targets and 36 for its flush-group bits.
    const std::uint32_t end        = kCursor + 4 * 331;
    const auto          withCursor = [](std::uint32_t cursor)
    {
        Call call;
        call.memory[kG + 0x30] = cursor;
        return call;
    };
    // The soft limit after the pool lowers it by 24.
    {
        auto call              = withCursor(kCursor);
        call.memory[kG + 0x38] = end + kSideBuffer;
        call.memory[kG + 0x34] = end + kSideBuffer + 160;
        UNSCOPED_INFO("the words end exactly at the soft limit");
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x38] -= 1;
        UNSCOPED_INFO("a byte short of the soft limit");
        REQUIRE(call.Verdict() == ResolveFallback::RingSpace);
    }
    // A cursor whose words run to the limit.
    UNSCOPED_INFO("the cursor that ends at the limit");
    REQUIRE(!withCursor(kLimit38 - kSideBuffer - 4 * 331).Verdict());
    UNSCOPED_INFO("the cursor one word past it");
    REQUIRE(withCursor(kLimit38 - kSideBuffer - 4 * 331 + 4).Verdict() == ResolveFallback::RingSpace);
    // The side buffer count past its threshold: Original retargets and flushes.
    {
        Call call;
        call.memory[kG + 0x3A54] = 0x1000 - kSideBuffer;
        UNSCOPED_INFO("the count reaches the threshold");
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x3A54] += 1;
        UNSCOPED_INFO("the count passes the threshold");
        REQUIRE(call.Verdict() == ResolveFallback::RingSpace);
    }
    // The pool: no object, no fixed buffer, and a buffer below the cursor plus the words and 164 bytes.
    {
        Call call;
        call.memory[kPoolObject + 152] = 0;
        UNSCOPED_INFO("a pool object without a fixed buffer");
        REQUIRE(call.Verdict() == ResolveFallback::AllocPath);
    }
    {
        Call call;
        call.memory[kG + 0x3A44] = end + 164 + kSideBuffer;
        UNSCOPED_INFO("the buffer ends 164 bytes above the words");
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x3A44] -= 4;
        UNSCOPED_INFO("four bytes short");
        REQUIRE(call.Verdict() == ResolveFallback::AllocCapacity);
    }
}

// Null pool writes G+0x3A44 or G+0x3A4C, plus G+0x3A54; limits stay.
void testNullPool()
{
    const auto nullCall = [](const std::function<void(Call&)>& change = {})
    {
        Call call;
        makeNullPool(call.memory);
        if (change)
            change(call);
        return call;
    };
    ResolvePlan plan;
    {
        auto call = nullCall();
        UNSCOPED_INFO("a null pool object is admitted");
        REQUIRE(!call.Verdict(&plan));
        UNSCOPED_INFO("the plan carries the forward take and the untouched limits");
        REQUIRE((plan.exit.poolPos == kNullEnd && plan.exit.wrapPos == kWrapPos && plan.exit.poolUsed == 0x100 + kSideBuffer && plan.exit.limit34 == kLimit34 && plan.exit.limit38 == kLimit38));
        const auto                 writes = ResolveExitWrites(kG, kHeader, plan.watch, plan.exit);
        std::vector<std::uint32_t> pool;
        for (const auto& write : writes)
            if (write.address == kG + 0x34 || write.address == kG + 0x38 || write.address == kG + 0x3A44 || write.address == kG + 0x3A4C || write.address == kG + 0x3A54)
                pool.push_back(write.address);
        UNSCOPED_INFO("a forward take stores G+0x3A44 and G+0x3A54 and no limit");
        REQUIRE(pool == std::vector<std::uint32_t>{ kG + 0x3A44, kG + 0x3A54 });
    }
    {
        // A wrap: the side buffer area is used up and G+0x3A44 stays as it was.
        auto call = nullCall([](Call& c)
                             {
                                 c.memory[kG + 0x3A44]        = 0x00204000;
                                 c.memory[kG + 0x3A3C]        = 0x00204017;
                                 c.memory[kProgressBlock + 4] = kSegment + 1;
                             });
        UNSCOPED_INFO("a wrap is admitted");
        REQUIRE(!call.Verdict(&plan));
        UNSCOPED_INFO("the plan carries the wrap take");
        REQUIRE((plan.exit.poolPos == 0x00204000 && plan.exit.wrapPos == kWrapPos + kSideBuffer && plan.exit.poolUsed == 0x100 + kSideBuffer));
        std::vector<std::uint32_t> pool;
        for (const auto& write : ResolveExitWrites(kG, kHeader, plan.watch, plan.exit))
            if (write.address == kG + 0x34 || write.address == kG + 0x38 || write.address == kG + 0x3A44 || write.address == kG + 0x3A4C || write.address == kG + 0x3A54)
                pool.push_back(write.address);
        UNSCOPED_INFO("a wrap stores G+0x3A4C and G+0x3A54, not G+0x3A44");
        REQUIRE(pool == std::vector<std::uint32_t>{ kG + 0x3A54, kG + 0x3A4C });
    }

    // In verdict order, each breaking only its own condition.
    const std::vector<Reason> list = {
        { ResolveFallback::RingSpace, [](Call& c)
          {
              c.memory[kG + 0x30] = kLimit38 - 16;
          } },
        { ResolveFallback::AllocCapacity, [](Call& c)
          {
              c.memory[kG + 0x3A3C] = 0;
              c.memory[kG + 0x3A50] = kWrapPos;
          } },
        { ResolveFallback::AllocWait, [](Call& c)
          {
              c.memory[kProgressBlock + 4] = kSegment - 2;
          } },
    };
    for (const auto& reason : list)
    {
        auto       call    = nullCall(reason.trigger);
        const auto verdict = call.Verdict();
        const auto name    = std::string(kResolveFallbackNames[static_cast<std::size_t>(reason.reason)]);
        UNSCOPED_INFO("null pool reason " + name + " is reported, got " + (verdict ? std::string(kResolveFallbackNames[static_cast<std::size_t>(*verdict)]) : "admit"));
        REQUIRE(verdict == reason.reason);
    }
    for (std::size_t first = 0; first < list.size(); ++first)
    {
        auto call = nullCall();
        for (std::size_t i = first; i < list.size(); ++i)
            list[i].trigger(call);
        const auto verdict = call.Verdict();
        UNSCOPED_INFO(std::format("null pool: with {} and every later reason set the earliest is reported", kResolveFallbackNames[static_cast<std::size_t>(list[first].reason)]));
        REQUIRE(verdict == list[first].reason);
    }
    // The threshold with the bracket clear is ring space, as for the fixed pool.
    {
        auto call = nullCall([](Call& c)
                             {
                                 c.memory[kG + 0x3A54] = 0x1000 - kSideBuffer;
                             });
        UNSCOPED_INFO("the count reaches the threshold");
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x3A54] += 1;
        UNSCOPED_INFO("the count passes the threshold");
        REQUIRE(call.Verdict() == ResolveFallback::RingSpace);
    }
    // The ring words are judged against limits the allocator does not lower: the end may sit at G+0x38.
    {
        const std::uint32_t end  = kCursor + 4 * 331;
        auto                call = nullCall([&](Call& c)
                                            {
                                 c.memory[kG + 0x38] = end;
                                 c.memory[kG + 0x34] = end + 160;
                                            });
        UNSCOPED_INFO("the words end exactly at the soft limit");
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x38] -= 1;
        UNSCOPED_INFO("a byte short of the soft limit");
        REQUIRE(call.Verdict() == ResolveFallback::RingSpace);
    }
    // Tiling, the clear color and the earlier tests still come first.
    {
        auto call = nullCall([](Call& c)
                             {
                                 c.memory[kG + 0x33B0]        = 0x40;
                                 c.memory[kProgressBlock + 4] = kSegment - 2;
                             });
        UNSCOPED_INFO("a bracket word alone is tiling_state before the allocator");
        REQUIRE(call.Verdict() == ResolveFallback::TilingState);
        call = nullCall([](Call& c)
                        {
                            c.r[1]                       = 0x100;
                            c.memory[kProgressBlock + 4] = kSegment - 2;
                        });
        UNSCOPED_INFO("an unmodeled clear color comes before the allocator");
        REQUIRE(call.Verdict() == ResolveFallback::ClearColor);
        call = nullCall([](Call& c)
                        {
                            c.r[1]                       = 0x200;
                            c.memory[kProgressBlock + 4] = kSegment - 2;
                        });
        UNSCOPED_INFO("a depth clear reaches the allocator");
        REQUIRE(call.Verdict() == ResolveFallback::AllocWait);
    }
    // Every null pool word is read, and the progress word through a block that exists.
    for (const std::uint32_t address : { kG + 0x3A3C, kG + 0x3A48, kG + 0x3A4C, kG + 0x3A50, kG + 0x33B4, kG + 0x2A90, kProgressBlock + 4 })
    {
        auto call = nullCall();
        call.memory.erase(address);
        UNSCOPED_INFO(std::format("{:08X} is unreadable", address));
        REQUIRE(call.Verdict() == ResolveFallback::Unreadable);
    }
    {
        auto call = nullCall([](Call& c)
                             {
                                 c.memory[kG + 0x2A90] = 0;
                             });
        UNSCOPED_INFO("a null progress block is unreadable");
        REQUIRE(call.Verdict() == ResolveFallback::Unreadable);
    }
    // A pool object does not read them: the fixed pool fixture has none of these words.
    {
        Call call;
        for (const std::uint32_t address : { kG + 0x3A3C, kG + 0x3A48, kG + 0x3A50, kG + 0x33B4, kG + 0x2A90 })
        {
            UNSCOPED_INFO("the fixed pool fixture has no null pool words");
            REQUIRE(!call.memory.contains(address));
        }
        UNSCOPED_INFO("so a pool object reads none");
        REQUIRE(!call.Verdict());
    }
}

// Clear flags carry modeled color/depth/stencil into selected packed layout.
void testClearFlags()
{
    // Fixture uses independent transliteration: depth 1.0/stencil 0, G+0x2940=3 under
    // G+0x2888, and format-0 color (0, 1, 0.5, 0.25).
    // Fixture layout and values follow g50:12389-12552.
    constexpr std::array<std::uint32_t, 4> entry{ 0xAAAA0000, 0xAAAA0001, 0xBBBB0002, 0xCCCC0003 };
    constexpr std::uint32_t                depthControl = 0x00000F80, depthPacked = 0xF0000000, color = 0x4080FF00;

    const struct
    {
        std::uint32_t                flags;
        std::array<std::uint32_t, 4> want;
        bool                         depth, paint;
    } cases[] = {
        { 0x200, { depthControl, depthPacked, entry[2], entry[3] }, true, false },
        { 0x100, { entry[0], entry[1], color, color }, false, true },
        { 0x300, { depthControl, depthPacked, color, color }, true, true },
        { 0x1100, { entry[0], entry[1], color, color }, false, true },
        { 0x7300, { depthControl, depthPacked, color, color }, true, true },
    };

    for (const auto& c : cases)
    {
        Call call;
        call.ModelColor();
        call.r[1] = c.flags;
        ResolvePlan plan;
        const auto  verdict = call.Verdict(&plan);
        UNSCOPED_INFO(std::format("flags {:X} are admitted, got {}", c.flags, verdict ? kResolveFallbackNames[static_cast<std::size_t>(*verdict)] : "admit"));
        REQUIRE(!verdict);
        UNSCOPED_INFO(std::format("flags {:X} are carried", c.flags));
        REQUIRE((plan.effects.depthClear == c.depth && plan.effects.colorClear == c.paint));
        UNSCOPED_INFO(std::format("flags {:X} clear words {:08X} {:08X} {:08X} {:08X}", c.flags, plan.exit.clear[0], plan.exit.clear[1], plan.exit.clear[2], plan.exit.clear[3]));
        REQUIRE(plan.exit.clear == c.want);
        UNSCOPED_INFO(std::format("flags {:X} watch the entry words", c.flags));
        REQUIRE(plan.watch.clear == entry);
        UNSCOPED_INFO(std::format("flags {:X} allocation byte", c.flags));
        REQUIRE(plan.exit.allocByte == (c.depth ? 0x04 : 0x00));
        // The stores: the changed clear words and the depth clear bit, nothing else of that region.
        std::vector<ResolveWrite> stores;
        for (const auto& write : ResolveExitWrites(kG, kHeader, plan.watch, plan.exit))
            if ((write.address >= kG + 0x2A28 && write.address <= kG + 0x2A34) || write.address == kG + 0x2ABD)
                stores.push_back(write);
        std::vector<ResolveWrite> want;
        if (c.depth)
        {
            want.push_back({ kG + 0x2A28, depthControl, false });
            want.push_back({ kG + 0x2A2C, depthPacked, false });
        }
        if (c.paint)
        {
            want.push_back({ kG + 0x2A30, color, false });
            want.push_back({ kG + 0x2A34, color, false });
        }
        if (c.depth)
            want.push_back({ kG + 0x2ABD, 0x04, true });
        const auto byAddress = [](const ResolveWrite& a, const ResolveWrite& b)
        {
            return a.address < b.address;
        };
        std::ranges::sort(stores, byAddress);
        std::ranges::sort(want, byAddress);
        UNSCOPED_INFO(std::format("flags {:X} stores", c.flags));
        REQUIRE(stores == want);
        // The copy hook runs before the blocks: it reads the words of the entry.
        const auto copy = ResolvePlanCopy(plan);
        UNSCOPED_INFO(std::format("flags {:X} copy hook words are the entry's", c.flags));
        REQUIRE((copy.depthClear == entry[1] && copy.colorClear == entry[2] && copy.colorClearLow == entry[3]));
    }
    // Other flag bits (slot, fragment select) are fine and leave the clear words alone.
    for (const std::uint32_t flags : { 0x1U, 0x2U, 0x3U, 0x4U, 0x70U, 0x2070U, 0x54000000U })
    {
        Call        call;
        ResolvePlan plan;
        call.r[1] = flags;
        UNSCOPED_INFO(std::format("flags {:X}", flags));
        REQUIRE(!call.Verdict(&plan));
        UNSCOPED_INFO(std::format("flags {:X} store no clear word", flags));
        REQUIRE((plan.exit.clear == entry && plan.exit.allocByte == 0));
    }
    // The depth clear does not look at the color; the color clear needs a format the host models.
    {
        Call call;
        call.r[1] = 0x200;
        UNSCOPED_INFO("a depth clear with an unmodeled color format");
        REQUIRE(!call.Verdict());
        call.r[1] = 0x100;
        UNSCOPED_INFO("a color clear with an unmodeled format");
        REQUIRE(call.Verdict() == ResolveFallback::ClearColor);
        call.r[1] = 0x300;
        UNSCOPED_INFO("both clears with an unmodeled format");
        REQUIRE(call.Verdict() == ResolveFallback::ClearColor);
    }
    // An input the body overwrites before the blocks read it: the color vector over the
    // destination pitch word G+0x2A20 (Original stores the derived pitch there first),
    // the parameter block over the copy words, and the same inputs clear of them.
    {
        Call call;
        call.ModelColor();
        call.r[1]                = 0x100;
        call.r[7]                = kG + 0x2A20;
        call.memory[kG + 0x2A20] = 0x3F800000;
        call.memory[kG + 0x2A28] = 0x3F800000;
        UNSCOPED_INFO("a color vector over the pitch word is clear_overlap");
        REQUIRE(call.Verdict() == ResolveFallback::ClearOverlap);
        call.r[7] = kG + 0x2A30;
        for (std::uint32_t offset = 0x2A30; offset < 0x2A40; offset += 4)
            call.memory[kG + offset] = 0x3F800000;
        UNSCOPED_INFO("a color vector over the clear words is not");
        REQUIRE(!call.Verdict());
        call.r[7]              = 0;
        call.memory[kSp + 100] = kG + 0x2A18;
        UNSCOPED_INFO("a parameter block over the copy words is clear_overlap");
        REQUIRE(call.Verdict() == ResolveFallback::ClearOverlap);
        call.r[1] = 0x200;
        UNSCOPED_INFO("and for a depth clear");
        REQUIRE(call.Verdict() == ResolveFallback::ClearOverlap);
        call.memory[kSp + 100] = 0;
        UNSCOPED_INFO("the same depth clear without the block");
        REQUIRE(!call.Verdict());
        // The depth block stores G+0x2A28 and G+0x2A2C before the color block reads its
        // parameter block there, so only the combined clear is an overlap.
        call.memory[kSp + 100]   = kG + 0x2A28;
        call.memory[kG + 0x2A2C] = 0x3F800000;
        UNSCOPED_INFO("a depth clear alone reads that block before the stores");
        REQUIRE(!call.Verdict());
        call.r[1] = 0x100;
        UNSCOPED_INFO("and so does a color clear alone");
        REQUIRE(!call.Verdict());
        call.r[1] = 0x300;
        UNSCOPED_INFO("both clears read it after the depth stores");
        REQUIRE(call.Verdict() == ResolveFallback::ClearOverlap);
        call.memory[kSp + 100] = kG + 0x2A30;
        UNSCOPED_INFO("both clears with the block clear of them");
        REQUIRE(!call.Verdict());
    }
    // The color format is the target's own shadow word: slot 0 reads G+0x2884, slots 1..3 G+0x288C, G+0x2890 and G+0x2894.
    for (std::uint32_t slot = 0; slot < 4; ++slot)
    {
        const std::uint32_t address = slot ? kG + 0x2888 + 4 * slot : kG + 0x2884;
        Call                call;
        call.r[1]            = 0x100 | slot;
        call.memory[address] = 0;
        ResolvePlan plan;
        UNSCOPED_INFO(std::format("the shadow word of slot {}", slot));
        REQUIRE((!call.Verdict(&plan) && plan.exit.clear[2] == color));
        call.memory[address] = 0x30000;
        UNSCOPED_INFO(std::format("slot {} with an unmodeled shadow word", slot));
        REQUIRE(call.Verdict() == ResolveFallback::ClearColor);
    }
    // Without the caller's stack words a clear call is not derived; others need none.
    {
        Call call;
        call.ModelColor();
        call.withClear = false;
        call.r[1]      = 0x200;
        UNSCOPED_INFO("clear flags without the stack arguments");
        REQUIRE(call.Verdict() == ResolveFallback::DeriveUnsupported);
        call.r[1] = 0x100;
        UNSCOPED_INFO("a color clear without the stack arguments");
        REQUIRE(call.Verdict() == ResolveFallback::DeriveUnsupported);
        call.r[1] = 0;
        UNSCOPED_INFO("no clear flags need none");
        REQUIRE(!call.Verdict());
    }
    // The words a flag needs are read, and only those.
    {
        const auto missing = [](std::uint32_t flags, std::uint32_t address)
        {
            Call call;
            call.ModelColor();
            call.r[1] = flags;
            call.memory.erase(address);
            return call.Verdict();
        };
        for (const std::uint32_t address : { kSp + 92, kSp + 100, kG + 0x2940, kResolveClearScale11, kResolveClearScale24 })
        {
            UNSCOPED_INFO(std::format("{:08X} is read for flags 0x200", address));
            REQUIRE(missing(0x200, address) == ResolveFallback::Unreadable);
        }
        for (const std::uint32_t address : { kResolveClearDefault + 4, kResolveClearOne, kResolveClearColorBits, kResolveClearAlphaBits })
        {
            UNSCOPED_INFO(std::format("{:08X} is read for flags 0x100", address));
            REQUIRE(missing(0x100, address) == ResolveFallback::Unreadable);
        }
        UNSCOPED_INFO("a color clear reads no depth word");
        REQUIRE((!missing(0x100, kSp + 92) && !missing(0x100, kG + 0x2940) && !missing(0x100, kResolveClearScale11)));
        UNSCOPED_INFO("a depth clear reads no color word");
        REQUIRE((!missing(0x200, kResolveClearOne) && !missing(0x200, kResolveClearDefault) && !missing(0x200, kResolveClearColorBits)));
    }
}

// Clear ring bounds include the dirty-block flush words.
void testClearRing()
{
    // G+0x20: 331 words; 0x300 joins 0x3C00 (+2), 0xC0 is its own run (+3);
    // both add 4.
    const struct
    {
        std::uint32_t flags, words;
    } cases[] = { { 0, 331 }, { 0x200, 333 }, { 0x100, 334 }, { 0x300, 335 } };

    for (const auto& c : cases)
    {
        const std::uint32_t end = kCursor + 4 * c.words;
        Call                call;
        call.ModelColor();
        call.r[1]              = c.flags;
        call.memory[kG + 0x38] = end + kSideBuffer;
        call.memory[kG + 0x34] = end + kSideBuffer + 160;
        UNSCOPED_INFO(std::format("flags {:X}: {} words end exactly at the soft limit", c.flags, c.words));
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x38] -= 1;
        UNSCOPED_INFO(std::format("flags {:X}: a byte short of the soft limit", c.flags));
        REQUIRE(call.Verdict() == ResolveFallback::RingSpace);
    }
}

void testUnreadable()
{
    const auto expectUnreadable = [](const char* name, const std::function<void(Call&)>& change)
    {
        Call call;
        change(call);
        UNSCOPED_INFO(std::string(name) + " is unreadable");
        REQUIRE(call.Verdict() == ResolveFallback::Unreadable);
    };
    expectUnreadable("source surface", [](Call& c)
                     {
                         c.memory.erase(kG + 0x3098);
                     });
    expectUnreadable("destination fetch constant", [](Call& c)
                     {
                         c.memory.erase(kHeader + 32);
                     });
    expectUnreadable("format table", [](Call& c)
                     {
                         c.memory.erase((kResolveFormatTable + 12) & ~3U);
                     });
    expectUnreadable("header word +8", [](Call& c)
                     {
                         c.memory.erase(kHeader + 8);
                     });
    expectUnreadable("header word +12", [](Call& c)
                     {
                         c.memory.erase(kHeader + 12);
                     });
    expectUnreadable("dirty word", [](Call& c)
                     {
                         c.memory.erase(kG + 0x24);
                     });
    expectUnreadable("size word", [](Call& c)
                     {
                         c.memory.erase(kG + 0x2A44);
                     });
    expectUnreadable("fence word", [](Call& c)
                     {
                         c.memory.erase(kG + 0x2A9C);
                     });
    expectUnreadable("clear word -1", [](Call& c)
                     {
                         c.memory.erase(kG + 0x2A28);
                     });
    expectUnreadable("clear word 0", [](Call& c)
                     {
                         c.memory.erase(kG + 0x2A2C);
                     });
    expectUnreadable("clear word 1", [](Call& c)
                     {
                         c.memory.erase(kG + 0x2A30);
                     });
    expectUnreadable("clear word 2", [](Call& c)
                     {
                         c.memory.erase(kG + 0x2A34);
                     });
    for (const std::uint32_t address : { kG + 0x30, kG + 0x34, kG + 0x38, kG + 0x3A44, kG + 0x3A4C, kG + 0x3A54, kG + 0x3A58, kG + 0x31CC, kG + 0x33B0, kG + 0x34CC, kPoolObject + 152, kPoolObject + 156 })
    {
        Call call;
        call.memory.erase(address);
        UNSCOPED_INFO(std::format("{:08X} is unreadable", address));
        REQUIRE(call.Verdict() == ResolveFallback::Unreadable);
    }
    expectUnreadable("device at the end of the address space", [](Call& c)
                     {
                         c.r[0] = 0xFFFFFFF0U;
                     });
    // The guard covers the highest device word read, G+0x3A58. A reader that supplies
    // every word shows where it stops: the last device the span allows reaches the
    // predicate word (nonzero here, so tiling_predicate) and the next one is unreadable.
    const GuestWordReader everything = [](std::uint32_t address, std::uint32_t& value)
    {
        value = address == kPcr + 0x70 ? kStackBase : address == kPcr + 0x74 ? kStackLimit
                                                                             : 0x1000;
        return true;
    };
    std::array<std::uint32_t, 8> last{ 0xFFFFFFFFU - kResolveDeviceSpan, 0, 0, kHeader, 0, 0, 0, 0 };
    UNSCOPED_INFO("the last device the span allows is read");
    REQUIRE(ResolveConsumeVerdict(readyState(), everything, last) == ResolveFallback::TilingPredicate);
    last[0] += 1;
    UNSCOPED_INFO("a device whose pool threshold word would wrap is unreadable");
    REQUIRE(ResolveConsumeVerdict(readyState(), everything, last) == ResolveFallback::Unreadable);
}

#if REREVVED_DEVELOPER
void testCounts()
{
    TakeResolveConsumeCounts();
    NoteResolveConsumed();
    NoteResolveConsumed(true);
    NoteResolveFallback(ResolveFallback::Tiling);
    NoteResolveFallback(ResolveFallback::Tiling);
    NoteResolveFallback(ResolveFallback::Nested);
    NoteResolveFallback(ResolveFallback::TilingPredicate);
    NoteResolveFallback(ResolveFallback::TilingState);
    auto counts = TakeResolveConsumeCounts();
    UNSCOPED_INFO("counts");
    REQUIRE((counts.consumed == 2 && counts.consumedTiled == 1 && counts.fallback[static_cast<std::size_t>(ResolveFallback::Tiling)] == 2 && counts.fallback[static_cast<std::size_t>(ResolveFallback::Nested)] == 1));
    UNSCOPED_INFO("formatted: " + FormatResolveConsumeCounts(counts));
    REQUIRE(FormatResolveConsumeCounts(counts) == " resolve_consumed=2 resolve_consumed_tiled=1 resolve_fallback_nested=1 resolve_fallback_tiling=2 resolve_fallback_tiling_predicate=1 resolve_fallback_tiling_state=1");
    counts = TakeResolveConsumeCounts();
    UNSCOPED_INFO("take resets");
    REQUIRE((counts.consumed == 0 && counts.consumedTiled == 0 && counts.fallback == std::array<std::uint64_t, kResolveFallbackCount>{}));
    UNSCOPED_INFO("an empty window");
    REQUIRE(FormatResolveConsumeCounts(counts) == " resolve_consumed=0");

    const std::array<const char*, kResolveFallbackCount> expectedNames{ "stubs", "nested", "ledger", "entry_state", "diagnostics", "stack_overlap", "tiling", "tiling_predicate", "tiling_state", "derive_unsupported", "fence", "alloc_marker", "clear_overlap", "clear_color", "alloc_path", "ring_space", "alloc_capacity", "alloc_wait", "unreadable" };
    for (std::size_t i = 0; i < kResolveFallbackCount; ++i)
    {
        UNSCOPED_INFO(std::format("name {}", i));
        REQUIRE(kResolveFallbackNames[i] == expectedNames[i]);
    }
}

#endif

void testHooks()
{
    UNSCOPED_INFO("the entry is the Resolve entry only");
    REQUIRE((ResolveConsumeEntry(0x8269E520) && !ResolveConsumeEntry(0x8269E8A0) && !ResolveConsumeEntry(0x8269F360)));
    // Both cvars are on by default; the consume acts only while ring stub services are published.
    UNSCOPED_INFO("the cvars are on by default");
    REQUIRE((REXCVAR_GET(native_consume_resolve) && REXCVAR_GET(native_consume_resolve_tiled) && ResolveTiledConsumeRequested()));
    UNSCOPED_INFO("unpublished ring stubs leave the consume unrequested");
    REQUIRE((!RingStubsPublished() && !ResolveConsumeRequested()));
    ringStubsActive.store(true);
    UNSCOPED_INFO("published ring stubs request the consume");
    REQUIRE(ResolveConsumeRequested());
    REXCVAR_SET(native_consume_resolve, false);
    UNSCOPED_INFO("the cvar off leaves it unrequested");
    REQUIRE(!ResolveConsumeRequested());
    REXCVAR_SET(native_consume_resolve, true);
    ringStubsActive.store(false);
}

} // namespace

TEST_CASE("resolve_consume", "[ctest][resolve-consume][timeout-60]")
{
    testAdmitted();
    testEachReason();
    testOrder();
    testStackOverlap();
    testTilingWords();
    testTiledBracket();
    testPredicatedBracket();
    testRingAndAllocator();
    testNullPool();
    testClearFlags();
    testClearRing();
    testUnreadable();
#if REREVVED_DEVELOPER
    testCounts();
#endif
    testHooks();
}
