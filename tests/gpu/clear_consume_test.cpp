

#include "gpu/guest_d3d/clear_consume.h"
#include "gpu/guest_d3d/resolve_derive.h"
#include "gpu/guest_d3d/ring_stubs.h"

#include <rex/cvar.h>

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <format>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

REXCVAR_DECLARE(bool, native_consume_clear);
REXCVAR_DECLARE(bool, native_consume_clear_tiled);

using namespace rerevved::gpu::guest_d3d;

namespace
{

constexpr std::uint64_t kAll = ~0ULL;

constexpr std::uint32_t kG = 0x40010000, kRect = 0x40021000, kColor = 0x40022000, kSurface = 0x40030000, kDepthSurface = 0x40031000, kSp = 0x70000000;

// The thread's stack: the KPCR at r13 holds the base at +0x70 and the limit at +0x74, and r1 is inside it.
constexpr std::uint32_t kPcr = 0x40060000, kStackLimit = kSp - 0x10000, kStackBase = kSp + 0x10000;

// A ring segment with room and a pool whose fixed buffer ends at 0x00204000.
constexpr std::uint32_t kCursor = 0x00200000, kLimit38 = 0x00203F00, kLimit34 = kLimit38 + 160;
constexpr std::uint32_t kPoolObject = 0x40040000, kPoolBase = 0x00203000, kPoolSize = 0x1000, kPoolTop = kPoolBase + kPoolSize;
constexpr std::uint32_t kUsed = 0x100, kThreshold = 0x1000, kTargets = 4;

// The null pool allocator's words: forward room up to kSideEnd above G+0x34 + 60, a wrap area, the segment
// and the GPU's progress word, which lets both a forward take (segment 5) and a wrap take (segment 6, ending
// inside the progress block) pass without a wait.
constexpr std::uint32_t kSideEnd = 0x00204100, kWrapPos = 0x00206000, kWrapLimit = 0x00207000, kSegment = 5, kProgress = 0x00207005;
constexpr std::uint32_t kBlock = 0x40050000;
// The start of the first forward take with G+0x3A44 zero: G+0x34 + 60.
constexpr std::uint32_t kForwardStart = kLimit34 + 60;

using Memory = std::unordered_map<std::uint32_t, std::uint32_t>;
using Rect   = std::array<std::int32_t, 4>;

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

struct Scene
{
    std::uint32_t       flags      = 1;
    Rect                rect       = { 0, 0, 64, 8 };
    std::array<bool, 4> rt         = { true, false, false, false };
    bool                depthBound = true;
    std::uint32_t       control    = 0; // G+0x2880
    std::uint64_t       dirty10 = 0, dirty18 = 0, dirty20 = 0;
    std::uint32_t       allocByte = 0;
};

void placeStack(Memory& memory, std::uint32_t limit, std::uint32_t base)
{
    memory[kPcr + 0x70] = base;
    memory[kPcr + 0x74] = limit;
}

Memory build(const Scene& scene)
{
    Memory memory;
    placeStack(memory, kStackLimit, kStackBase);
    for (std::uint32_t k = 0; k < 4; ++k)
    {
        memory[kG + 0x3098 + 4 * k]         = scene.rt[k] ? kSurface + 0x100 * k : 0;
        memory[kSurface + 0x100 * k + 0x1C] = 0; // format 0
    }
    memory[kG + 0x30A8] = scene.depthBound ? kDepthSurface : 0;
    for (std::uint32_t i = 0; i < 5; ++i)
        memory[kG + 0x31B8 + 4 * i] = 0;
    for (std::uint32_t i = 0; i < 4; ++i)
    {
        memory[kG + 0x3168 + 4 * i] = std::bit_cast<std::uint32_t>(i < 2 ? 0.0f : 4096.0f);
        memory[kG + 0x3184 + 4 * i] = 0;
        memory[kColor + 4 * i]      = 0x3F800000 + i;
        memory[kRect + 4 * i]       = static_cast<std::uint32_t>(scene.rect[i]);
    }
    memory[kG + 0x2E50]       = 0;
    memory[kG + 0x2880]       = scene.control;
    memory[kG + 0x2ABC]       = scene.allocByte << 16 | 0x00000F00; // tiling byte 0, state byte 0x0F
    memory[kG + 0x31A4]       = 0;
    memory[kG + 0x31AC]       = 0;
    memory[kG + 0x33B0]       = 0;
    memory[kClearStripWord]   = 1;
    memory[kG + 0x10]         = static_cast<std::uint32_t>(scene.dirty10 >> 32);
    memory[kG + 0x14]         = static_cast<std::uint32_t>(scene.dirty10);
    memory[kG + 0x18]         = static_cast<std::uint32_t>(scene.dirty18 >> 32);
    memory[kG + 0x1C]         = static_cast<std::uint32_t>(scene.dirty18);
    memory[kG + 0x20]         = static_cast<std::uint32_t>(scene.dirty20 >> 32);
    memory[kG + 0x24]         = static_cast<std::uint32_t>(scene.dirty20);
    memory[kG + 0x30]         = kCursor;
    memory[kG + 0x34]         = kLimit34;
    memory[kG + 0x38]         = kLimit38;
    memory[kG + 0x3A44]       = 0;
    memory[kG + 0x3A54]       = kUsed;
    memory[kG + 0x3A58]       = kThreshold;
    memory[kG + 0x31CC]       = kTargets;
    memory[kG + 0x34CC]       = kPoolObject;
    memory[kPoolObject + 152] = kPoolBase;
    memory[kPoolObject + 156] = kPoolSize;
    memory[kG + 0x3A4C]       = kWrapPos;
    memory[kG + 0x3A3C]       = kSideEnd;
    memory[kG + 0x3A50]       = kWrapLimit;
    memory[kG + 0x3A48]       = kSegment;
    memory[kG + 0x33B4]       = 0;
    memory[kG + 0x2A90]       = kBlock;
    memory[kBlock + 4]        = kProgress;
    return memory;
}

ClearEntry entryOf(const Scene& scene)
{
    ClearEntry entry;
    entry.sp      = kSp;
    entry.pcr     = kPcr;
    entry.g       = kG;
    entry.flags   = scene.flags;
    entry.rect    = kRect;
    entry.color   = kColor;
    entry.depth   = 1.0;
    entry.stencil = 0x5A;
    return entry;
}

ClearConsumeState readyState()
{
    ClearConsumeState state;
    state.stubsReady  = true;
    state.nested      = false;
    state.ledgerReady = true;
    state.entryState  = true;
    state.diagnostics = false;
    return state;
}

// A call that DeriveClearCalls refuses without a rectangle: a null rectangle needs a bound target.
void unbindTargets(Memory& memory)
{
    memory[kG + 0x3098] = 0;
    memory[kG + 0x30A8] = 0;
}

struct Call
{
    ClearConsumeState state = readyState();
    Scene             scene;
    Memory            memory = build(scene);
    ClearEntry        entry  = entryOf(scene);

    Call() = default;

    explicit Call(const Scene& s)
    : scene(s)
    , memory(build(s))
    , entry(entryOf(s))
    {
    }

    std::optional<ClearFallback> Verdict(ClearPlan* plan = nullptr) const
    {
        return ClearConsumeVerdict(state, readerOf(memory), entry, plan);
    }
};

std::string nameOf(const std::optional<ClearFallback>& verdict)
{
    return verdict ? std::string(kClearFallbackNames[static_cast<std::size_t>(*verdict)]) : std::string("admit");
}

struct Reason
{
    ClearFallback              reason;
    std::function<void(Call&)> trigger;
};

// In verdict order; each trigger breaks only its own condition.
const std::vector<Reason>& reasons()
{
    static const std::vector<Reason> list = {
        { ClearFallback::Stubs, [](Call& c)
          {
              c.state.stubsReady = false;
          } },
        { ClearFallback::Nested, [](Call& c)
          {
              c.state.nested = true;
          } },
        { ClearFallback::Ledger, [](Call& c)
          {
              c.state.ledgerReady = false;
          } },
        { ClearFallback::EntryState, [](Call& c)
          {
              c.state.entryState = false;
          } },
        { ClearFallback::Diagnostics, [](Call& c)
          {
              c.state.diagnostics = true;
          } },
        { ClearFallback::StackOverlap, [](Call& c)
          {
              placeStack(c.memory, kG - 0x1000, kStackBase); // the thread's stack runs over the device block
              c.entry.sp = kG + 0x40;
          } },
        { ClearFallback::Tiled, [](Call& c)
          {
              c.memory[kG + 0x2ABC] |= 0x10000000;
          } },
        { ClearFallback::DeriveUnsupported, [](Call& c)
          {
              c.entry.rect = 0; // a null rectangle with no bound target
              unbindTargets(c.memory);
          } },
        { ClearFallback::EarlyExit, [](Call& c)
          {
              c.memory[kRect + 8] = 0; // right == left: empty after clipping
          } },
        { ClearFallback::AllocMarker, [](Call& c)
          {
              c.memory[kG + 0x2ABC] |= 0x00200000;
          } },
        { ClearFallback::Unreadable, [](Call& c)
          {
              c.memory.erase(kClearStripWord); // read after the marker, before the allocator tests
          } },
        { ClearFallback::AllocPath, [](Call& c)
          {
              c.memory[kPoolObject + 152] = 0; // a pool object without a fixed buffer
          } },
        { ClearFallback::RingSpace, [](Call& c)
          {
              c.memory[kG + 0x30] = kLimit38 - 16;
          } },
        { ClearFallback::AllocCapacity, [](Call& c)
          {
              c.memory[kG + 0x3A44] = kCursor + 64;
          } },
    };
    return list;
}

struct Expected
{
    const char*                  name;
    Scene                        scene;
    std::array<std::uint64_t, 3> exitDirty;
    std::uint8_t                 exitByte;
    std::vector<std::uint32_t>   strips; // bytes of each 0x826A1800 side buffer, in order
    std::size_t                  calls;
};

// Clear callees: 0x826A1D90, 0x826A1AF0, 0x826A1800. Dirty words follow
// clear_effect_test's store emulation: g158:13166-13924, g88:13109-13389,
// g96:13268-13306.
std::vector<Expected> expectations()
{
    std::vector<Expected> table;
    const auto            add = [&](const char* name, Scene scene, std::array<std::uint64_t, 3> dirty, std::uint8_t byte, std::vector<std::uint32_t> strips, std::size_t calls)
    {
        table.push_back({ name, std::move(scene), dirty, byte, std::move(strips), calls });
    };
    const auto withDirty = [](Scene scene, std::uint64_t a, std::uint64_t b, std::uint64_t c)
    {
        scene.dirty10 = a;
        scene.dirty18 = b;
        scene.dirty20 = c;
        return scene;
    };
    // A color clear with every dirty bit set at the entry: the flush groups keep 17..20 and 58..63 of G+0x10,
    // outside 34..54 of G+0x18 and 38..63 of G+0x20.
    add("color_ones", withDirty(Scene{}, kAll, kAll, kAll), { 0xFF0013A0001E0EE8ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFD000080000ULL }, 0x04, { 84 }, 1);
    add("color_zero", Scene{}, { 0x030013A0001A0EE8ULL, 0x0000000880000000ULL, 0x0000001000080000ULL }, 0x04, { 84 }, 1);
    {
        // The depth flag alone: no color branch, one call for the depth surface (index -1).
        Scene scene;
        scene.flags = 0x10;
        add("depth_only", scene, { 0x03001380001A08E8ULL, 0x0000000880000000ULL, 0x0000001000080000ULL }, 0x04, { 84 }, 1);
    }
    {
        // The stencil flag with color: the call ORs G+0x10 with 0x10000000.
        Scene scene;
        scene.flags = 0x21;
        add("stencil", withDirty(scene, kAll, kAll, kAll), { 0xFF0013A0101E0EE8ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFD000080000ULL }, 0x04, { 84 }, 1);
    }
    {
        // The fast path: no depth or stencil flag, a format other than 5, 7 and 15, at least 40x8 in mode 2.
        Scene scene;
        scene.flags   = 1;
        scene.rect    = { 0, 0, 40, 8 };
        scene.control = 2U << 16;
        add("fast", withDirty(scene, kAll, kAll, kAll), { 0xFF8013A0101E0FE8ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFD000080000ULL }, 0x04, { 84 }, 1);
        // The same rectangle in mode 0 (80x16 minimum) is below the fast threshold.
        scene.control = 0;
        add("fast_below_threshold", withDirty(scene, kAll, kAll, kAll), { 0xFF0013A0001E0EE8ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFD000080000ULL }, 0x04, { 84 }, 1);
    }
    {
        // Two color targets with the stencil flag: the first call keeps 0x23 and ORs 0x10000000, the second
        // has the depth bits dropped and takes the fast path. 640x480 has no edge strips in mode 0.
        Scene scene;
        scene.flags = 0x23;
        scene.rect  = { 0, 0, 640, 480 };
        scene.rt    = { true, true, false, false };
        add("stencil_then_fast", withDirty(scene, kAll, kAll, kAll), { 0xFF8013A0101E0FE8ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFD000080000ULL }, 0x04, { 84, 84 }, 2);
    }
    {
        // A color flag with no bound slot: no call, but the flush and the ORs still happen, and nothing is allocated.
        Scene scene;
        scene.rt = { false, false, false, false };
        add("flush_no_calls", withDirty(scene, kAll, kAll, kAll), { 0xFC0013A0001E06E0ULL, 0xFF80000BFFFFFFFFULL, 0xFFFFFFC000080000ULL }, 0x04, {}, 0);
    }
    {
        // Edge strips: left 10 -> 80, right 700 -> 640, top 5 -> 16 are cut, bottom 400 is on the grid: three
        // edge strips in one call, then the interior. The rectangle clears the fast threshold.
        Scene scene;
        scene.rect = { 10, 5, 700, 400 };
        add("edge_strips", scene, { 0x038013A0101A0FE8ULL, 0x0000000880000000ULL, 0x0000001000080000ULL }, 0x04, { 252, 84 }, 1);
    }
    {
        // The marker byte is kept; a byte with other bits keeps them.
        Scene scene;
        scene.allocByte = 0x18;
        add("alloc_byte_kept", scene, { 0x030013A0001A0EE8ULL, 0x0000000880000000ULL, 0x0000001000080000ULL }, 0x1C, { 84 }, 1);
    }
    return table;
}

void testAdmitted()
{
    Call       call;
    ClearPlan  plan;
    const auto verdict = call.Verdict(&plan);
    UNSCOPED_INFO("the fixture is admitted, got " + nameOf(verdict));
    REQUIRE(!verdict);
    UNSCOPED_INFO("the plan carries the derived calls and effects");
    REQUIRE((plan.calls.count == 1 && plan.calls.reachedFlush && plan.effects.untiledCalls == 1 && plan.effects.allocates));
    UNSCOPED_INFO("the plan carries the side buffers");
    REQUIRE((plan.sides.count == 1 && plan.sides.bytes[0] == 84));
    UNSCOPED_INFO("the plan carries the watched words at the entry");
    REQUIRE((plan.watch.ring == kCursor && plan.watch.cursor == 0 && plan.watch.allocated == kUsed && plan.watch.poolObject == kPoolObject));
    // 64x8 in mode 0: one 84-byte buffer from the top of the fixed buffer.
    constexpr std::uint32_t kBuffer = kPoolTop - 84;
    UNSCOPED_INFO("the plan carries the pool moves");
    REQUIRE((plan.exit.poolPos == kBuffer && plan.exit.poolUsed == kUsed + 84 && plan.exit.limit34 == kLimit34 - 84 && plan.exit.limit38 == kLimit38 - 84));
    UNSCOPED_INFO("G+0x2ABD gains 4");
    REQUIRE(plan.exit.allocByte == 0x04);
    UNSCOPED_INFO("the verdict needs no plan");
    REQUIRE(!call.Verdict());
}

void testExitState()
{
    for (const auto& row : expectations())
    {
        Call       call(row.scene);
        ClearPlan  plan;
        const auto verdict = call.Verdict(&plan);
        UNSCOPED_INFO(std::string(row.name) + " is admitted, got " + nameOf(verdict));
        REQUIRE(!verdict);
        for (std::size_t i = 0; i < 3; ++i)
        {
            UNSCOPED_INFO(std::format("{} dirty word {}: {:016X} expected {:016X}", row.name, i, plan.exit.dirty[i], row.exitDirty[i]));
            REQUIRE(plan.exit.dirty[i] == row.exitDirty[i]);
        }
        UNSCOPED_INFO(std::format("{} byte {:02X} expected {:02X}", row.name, plan.exit.allocByte, row.exitByte));
        REQUIRE(plan.exit.allocByte == row.exitByte);
        UNSCOPED_INFO(std::string(row.name) + " calls");
        REQUIRE(plan.calls.count == row.calls);
        UNSCOPED_INFO(std::string(row.name) + " strip calls");
        REQUIRE(plan.sides.count == row.strips.size());
        // The pool, take by take.
        std::uint32_t pos = 0, used = kUsed, limit34 = kLimit34, limit38 = kLimit38;
        for (std::size_t i = 0; i < row.strips.size(); ++i)
        {
            UNSCOPED_INFO(std::format("{} strip {}", row.name, i));
            REQUIRE(plan.sides.bytes[i] == row.strips[i]);
            const std::uint32_t top = pos ? pos : kPoolTop;
            pos                     = top - row.strips[i];
            limit34 -= row.strips[i];
            limit38 -= row.strips[i];
            used += row.strips[i];
        }
        // Pool words are hand-computed from 0x826A6848 (g100:13237-13331) and
        // 0x8269B818 (g93:12757-12888).
        UNSCOPED_INFO(std::string(row.name) + " pool words");
        REQUIRE((plan.exit.poolPos == pos && plan.exit.poolUsed == used && plan.exit.limit34 == limit34 && plan.exit.limit38 == limit38));
        if (row.strips.empty())
        {
            UNSCOPED_INFO(std::string(row.name) + " takes nothing");
            REQUIRE((plan.exit.poolPos == plan.watch.cursor && plan.exit.poolUsed == plan.watch.allocated));
        }
    }
}

void testWrites()
{
    Call      call;
    ClearPlan plan;
    UNSCOPED_INFO("admitted");
    REQUIRE(!call.Verdict(&plan));
    const auto writes = ClearExitWrites(kG, plan.watch, plan.exit);
    // dirty10 high half and low half, dirty18 and dirty20 (zero at the entry: each half that differs), the byte, then the pool.
    const std::vector<ClearWrite> expected{
        { kG + 0x10, 0x030013A0, false },
        { kG + 0x14, 0x001A0EE8, false },
        { kG + 0x18, 0x00000008, false },
        { kG + 0x1C, 0x80000000, false },
        { kG + 0x20, 0x00000010, false },
        { kG + 0x24, 0x00080000, false },
        { kG + 0x2ABD, 0x04, true },
        { kG + 0x34, kLimit34 - 84, false },
        { kG + 0x38, kLimit38 - 84, false },
        { kG + 0x3A44, kPoolTop - 84, false },
        { kG + 0x3A54, kUsed + 84, false },
    };
    UNSCOPED_INFO("the stores of a color clear against zero dirty words");
    REQUIRE(writes == expected);
    // Words that already hold the value are not stored.
    Scene again;
    again.dirty10   = 0x030013A0001A0EE8ULL;
    again.dirty18   = 0x0000000880000000ULL;
    again.dirty20   = 0x0000001000080000ULL;
    again.allocByte = 0x04;
    Call      same(again);
    ClearPlan samePlan;
    UNSCOPED_INFO("admitted again");
    REQUIRE(!same.Verdict(&samePlan));
    const auto sameWrites = ClearExitWrites(kG, samePlan.watch, samePlan.exit);
    UNSCOPED_INFO("only the pool words change");
    REQUIRE((sameWrites.size() == 4 && sameWrites[0].address == kG + 0x34 && sameWrites[3].address == kG + 0x3A54));
    // A no-call flush stores no pool word.
    Scene none;
    none.rt = { false, false, false, false };
    Call      flush(none);
    ClearPlan flushPlan;
    UNSCOPED_INFO("flush without calls is admitted");
    REQUIRE(!flush.Verdict(&flushPlan));
    for (const auto& write : ClearExitWrites(kG, flushPlan.watch, flushPlan.exit))
    {
        UNSCOPED_INFO(std::format("no ring or pool store at {:08X}", write.address));
        REQUIRE((write.address < kG + 0x30 || write.address == kG + 0x2ABD));
    }
}

void testEachReason()
{
    for (const auto& reason : reasons())
    {
        Call call;
        reason.trigger(call);
        const auto verdict = call.Verdict();
        const auto name    = std::string(kClearFallbackNames[static_cast<std::size_t>(reason.reason)]);
        UNSCOPED_INFO("reason " + name + " is reported, got " + nameOf(verdict));
        REQUIRE(verdict == reason.reason);
    }
    // alloc_wait is the null pool object's reason, tested with it (testNullPool), and tiled_block the tiled device's (testTiledClear).
    UNSCOPED_INFO("one fixture per reason");
    REQUIRE(reasons().size() + 2 == kClearFallbackCount);
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
        UNSCOPED_INFO(std::format("with {} and every later reason set the earliest is reported, got {}", kClearFallbackNames[first], nameOf(verdict)));
        REQUIRE(verdict == list[first].reason);
    }
}

// One store of the skipped Original's prologue chain: `bytes` at `address`.
struct Store
{
    std::uint32_t address, bytes;
};

// 0x826A2970: LR -8, r31 -16, chain -112 (g216:12893-12901). 0x826A22F8 enters
// r1'=sp-112; __savegprlr_19 saves r19..r31 at r1'-112..-16, LR r1'-8; f31
// r1'-120, chain r1'-256 (g158:12974-12986).
std::vector<Store> prologueStores(std::uint32_t sp)
{
    const std::uint32_t body = sp - 112;
    return { { sp - 8, 4 }, { sp - 16, 8 }, { sp - 112, 4 }, { body - 112, 104 }, { body - 8, 4 }, { body - 120, 8 }, { body - 256, 4 } };
}

// Overwrites every word the stores touch with a marker.
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

void testStackOverlap()
{
    // stack_guard.h refuses unknown bounds and device/object overlap.
    UNSCOPED_INFO("the Clear footprint: nothing above r1, the device up to G+0x3A5C");
    REQUIRE((kClearStackFootprint.above == 0 && kClearStackFootprint.deviceSpan == 0x3A5C && kClearDeviceSpan == 0x3A5C));
    // A call whose stack is [limit, base) and whose entry r1 is `sp`; `setup` edits the call first.
    const auto verdictOf = [](std::uint32_t limit, std::uint32_t sp, const std::function<void(Call&)>& setup = {})
    {
        Call call;
        if (setup)
            setup(call);
        placeStack(call.memory, limit, std::max(sp, kStackBase) + 0x1000);
        call.entry.sp = sp;
        return call.Verdict();
    };
    const auto refused = [&](std::uint32_t limit, std::uint32_t sp, const std::function<void(Call&)>& setup = {})
    {
        return verdictOf(limit, sp, setup) == ClearFallback::StackOverlap;
    };
    const auto consumed = [&](std::uint32_t limit, std::uint32_t sp, const std::function<void(Call&)>& setup = {})
    {
        return !verdictOf(limit, sp, setup);
    };
    constexpr std::uint32_t kSpan = 0x3A5C, kBlockEnd = kG + kSpan;

    // The device block [G, G + 0x3A5C) against the half-open reach [limit, sp), at the pinned edges.
    UNSCOPED_INFO("a stack ending exactly at G is consumed");
    REQUIRE(consumed(kG - 0x1000, kG));
    UNSCOPED_INFO("a stack ending one byte into the block is refused");
    REQUIRE(refused(kG - 0x1000, kG + 1));
    UNSCOPED_INFO("a stack starting exactly at G + 0x3A5C is consumed");
    REQUIRE(consumed(kBlockEnd, kBlockEnd + 0x1000));
    UNSCOPED_INFO("a stack starting one byte inside the block is refused");
    REQUIRE(refused(kBlockEnd - 1, kBlockEnd + 0x1000));
    UNSCOPED_INFO("a stack that covers the block is refused");
    REQUIRE(refused(kG - 0x1000, kBlockEnd + 0x1000));
    UNSCOPED_INFO("a stack inside the block is refused");
    REQUIRE(refused(kG + 0x100, kG + 0x200));
    UNSCOPED_INFO("stacks away from the block are consumed");
    REQUIRE((consumed(kStackLimit, kSp) && consumed(kG - 0x2000, kG - 0x1000) && consumed(kBlockEnd + 0x2000, kBlockEnd + 0x4000)));
    // The words just past the old 0x3500 block, up to G+0x3A58, are device words too.
    UNSCOPED_INFO("the pool words G+0x3500..G+0x3A5B are in the footprint");
    REQUIRE((refused(kG + 0x3500, kG + 0x3600) && refused(kG + 0x3A58, kG + 0x3A60)));

    // The no-overflow domain: the bytes between the limit and r1 must hold the entry function's own frame (the
    // wrapper's stwu r1,-112(r1), g216:12901). Fewer is refused, even away from every word the model reads; r1 at the
    // limit is the case Original's register saves would store below the limit in.
    constexpr std::uint32_t kFrame = 112;
    UNSCOPED_INFO("the Clear frame is the wrapper's 112 bytes");
    REQUIRE(kClearStackFootprint.frame == kFrame);
    UNSCOPED_INFO("exactly the frame between the limit and r1 is consumed");
    REQUIRE(consumed(kSp - kFrame, kSp));
    UNSCOPED_INFO("one byte short of the frame is refused");
    REQUIRE(refused(kSp - kFrame + 1, kSp));
    UNSCOPED_INFO("r1 at the limit, or a word above it, is refused");
    REQUIRE((refused(kSp, kSp) && refused(kSp - 4, kSp)));
    UNSCOPED_INFO("an empty reach is refused too (it leaves no room for a frame)");
    REQUIRE((refused(kG, kG) && refused(kBlockEnd, kBlockEnd)));

    // The regression of Original's saves. With the pool object G+0x34CC names at r1 - 160, its base word +152 is the
    // link register save at r1 - 8: the stores change the word the model reads, and the call is refused
    // wherever the object sits in the reach, whether or not the block of the device words is.
    {
        Call                plain;
        Memory              after = plain.memory;
        const std::uint32_t pool  = kSp - 160;
        moveObject(plain.memory, kPoolObject, pool, 160);
        plain.memory[kG + 0x34CC] = pool;
        after                     = plain.memory;
        applyStores(after, prologueStores(kSp));
        UNSCOPED_INFO("Original's link register save overwrites the pool base word");
        REQUIRE(after[pool + 152] != plain.memory[pool + 152]);
        UNSCOPED_INFO("a pool object whose base word is the saved link register is refused");
        REQUIRE(plain.Verdict() == ClearFallback::StackOverlap);
        // The refusal is the object's, not the poorer reading of its words: the words need not be readable.
        plain.memory.erase(pool + 152);
        plain.memory.erase(pool + 156);
        UNSCOPED_INFO("the pool object is tested before its words are read");
        REQUIRE(plain.Verdict() == ClearFallback::StackOverlap);
    }

    // Every pointed object and register-named object of the model, at the lower edge of the reach (ending exactly
    // at the limit is clear, one byte more is refused) and at r1 (starting exactly at r1 is clear, one byte before
    // is refused). `place` puts the object's first checked byte at `address`; `bytes` is its checked size.
    struct Object
    {
        const char*                                       name;
        std::uint32_t                                     bytes;
        std::function<void(Call&, std::uint32_t address)> place;
    };

    const std::vector<Object> objects = {
        { "rectangle r5", 16, [](Call& c, std::uint32_t a)
          {
              moveObject(c.memory, kRect, a, 16);
              c.entry.rect = a;
          } },
        { "color r6", 16, [](Call& c, std::uint32_t a)
          {
              moveObject(c.memory, kColor, a, 16);
              c.entry.color = a;
          } },
        { "RT0 surface extent word +0x24", 4, [](Call& c, std::uint32_t a)
          {
              moveObject(c.memory, kSurface, a - 0x24, 0x100);
              c.memory[kG + 0x3098] = a - 0x24;
          } },
        { "depth surface extent word +0x24", 4, [](Call& c, std::uint32_t a)
          {
              moveObject(c.memory, kDepthSurface, a - 0x24, 0x100);
              c.memory[kG + 0x30A8] = a - 0x24;
          } },
        { "pool object G+0x34CC", 160, [](Call& c, std::uint32_t a)
          {
              moveObject(c.memory, kPoolObject, a, 160);
              c.memory[kG + 0x34CC] = a;
          } },
        { "progress word of the block G+0x2A90 names", 4, [](Call& c, std::uint32_t a)
          {
              moveObject(c.memory, kBlock, a - 4, 8);
              c.memory[kG + 0x2A90] = a - 4;
          } },
    };
    // Clear of the stack by default, the object sits at the default place; the stack [kSp - 0x4000, kSp) is
    // a window of the fixture's own, with the base above it.
    constexpr std::uint32_t kWindow = kSp - 0x4000;
    for (const auto& object : objects)
    {
        const auto at = [&](std::uint32_t address)
        {
            return [&object, address](Call& c)
            {
                object.place(c, address);
            };
        };
        UNSCOPED_INFO(std::string(object.name) + " ending exactly at the limit is clear");
        REQUIRE(consumed(kWindow, kSp, at(kWindow - object.bytes)));
        UNSCOPED_INFO(std::string(object.name) + " one byte over the limit is refused");
        REQUIRE(refused(kWindow, kSp, at(kWindow - object.bytes + 1)));
        UNSCOPED_INFO(std::string(object.name) + " inside the reach is refused");
        REQUIRE((refused(kWindow, kSp, at(kWindow)) && refused(kWindow, kSp, at(kSp - object.bytes))));
        UNSCOPED_INFO(std::string(object.name) + " starting exactly at r1 is clear");
        REQUIRE(consumed(kWindow, kSp, at(kSp)));
        UNSCOPED_INFO(std::string(object.name) + " one byte below r1 is refused");
        REQUIRE(refused(kWindow, kSp, at(kSp - 1)));
        // The placement: the refusal does not need the object's words, so an unreadable word cannot mask it.
        Call call;
        object.place(call, kSp - object.bytes);
        UNSCOPED_INFO(std::string(object.name) + " in the frames is refused");
        REQUIRE(call.Verdict() == ClearFallback::StackOverlap);
    }

    // The fixed words of the title image the model reads: a stack that holds them is refused.
    const std::vector<std::pair<const char*, StackObject>> statics = { { "the default color", { kClearDefaultColor, 16 } }, { "the strip word", { kClearStripWord, 4 } } };
    for (const auto& [name, object] : statics)
    {
        UNSCOPED_INFO(std::string(name) + ": a stack starting exactly at its end is clear");
        REQUIRE(consumed(object.address + object.bytes, object.address + object.bytes + 0x1000));
        UNSCOPED_INFO(std::string(name) + ": a stack one byte over it is refused");
        REQUIRE(refused(object.address + object.bytes - 1, object.address + object.bytes + 0x1000));
        UNSCOPED_INFO(std::string(name) + ": a stack ending exactly at it is clear");
        REQUIRE(consumed(object.address - 0x1000, object.address));
        UNSCOPED_INFO(std::string(name) + ": a stack one byte over it is refused");
        REQUIRE(refused(object.address - 0x1000, object.address + 1));
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
                            c.entry.pcr = 0;
                        }) == ClearFallback::StackOverlap);
        UNSCOPED_INFO("an unreadable limit");
        REQUIRE(unknown([](Call& c)
                        {
                            c.memory.erase(kPcr + 0x74);
                        }) == ClearFallback::StackOverlap);
        UNSCOPED_INFO("an unreadable base");
        REQUIRE(unknown([](Call& c)
                        {
                            c.memory.erase(kPcr + 0x70);
                        }) == ClearFallback::StackOverlap);
        UNSCOPED_INFO("a zero limit");
        REQUIRE(unknown([](Call& c)
                        {
                            placeStack(c.memory, 0, kStackBase);
                        }) == ClearFallback::StackOverlap);
        UNSCOPED_INFO("an inverted range");
        REQUIRE(unknown([](Call& c)
                        {
                            placeStack(c.memory, kStackBase, kStackLimit);
                        }) == ClearFallback::StackOverlap);
        UNSCOPED_INFO("r1 below the limit");
        REQUIRE(unknown([](Call& c)
                        {
                            c.entry.sp = kStackLimit - 4;
                        }) == ClearFallback::StackOverlap);
        UNSCOPED_INFO("r1 above the base");
        REQUIRE(unknown([](Call& c)
                        {
                            c.entry.sp = kStackBase + 4;
                        }) == ClearFallback::StackOverlap);
        UNSCOPED_INFO("a KPCR in the frames");
        REQUIRE(unknown([](Call& c)
                        {
                            c.entry.pcr = kSp - 0x40;
                            placeStack(c.memory, kStackLimit, kStackBase);
                            c.memory[kSp - 0x40 + 0x70] = kStackBase;
                            c.memory[kSp - 0x40 + 0x74] = kStackLimit;
                        }) == ClearFallback::StackOverlap);
        Call ok;
        UNSCOPED_INFO("the fixture's stack is known");
        REQUIRE(!ok.Verdict());
    }

    // The bounds and the registers decide, not the old depth: the same r1 with a deeper or a shallower stack.
    UNSCOPED_INFO("a very deep stack that holds no model word is consumed");
    REQUIRE((consumed(kSp - 0x100, kSp) && consumed(kSp - 0x100000, kSp)));
    UNSCOPED_INFO("r1 at the limit leaves no room for the frame");
    REQUIRE(refused(kSp, kSp));

    // Over every sp around the block, with a reach of exactly 0x1000: the reach decides.
    for (std::uint32_t sp = kG - 0x1400; sp < kBlockEnd + 0x1400; sp += 4)
    {
        const bool aliases = sp > kG && sp - 0x1000 < kBlockEnd;
        UNSCOPED_INFO(std::format("the reach decides (sp {:#x})", sp));
        REQUIRE((aliases ? refused(sp - 0x1000, sp) : consumed(sp - 0x1000, sp)));
    }

    // The stores of Original's prologue change words the model reads wherever they land in the device: a stack with r1 over a
    // model word is refused.
    const std::vector<std::uint32_t> read = { kG + 0x10, kG + 0x14, kG + 0x30, kG + 0x38, kG + 0x2ABC, kG + 0x3098, kG + 0x31B8, kG + 0x3A44, kG + 0x3A58 };
    for (const auto word : read)
        for (const std::uint32_t sp : { word + 8, word + 16, word + 112, word + 112 + 120, word + 112 + 256 })
        {
            Call call;
            placeStack(call.memory, kG - 0x1000, kBlockEnd + 0x1000);
            call.entry.sp = sp;
            Memory after  = call.memory;
            applyStores(after, prologueStores(sp));
            UNSCOPED_INFO(std::format("Original's saves change G+{:#x} (sp {:#x})", word - kG, sp));
            REQUIRE(after[word] != call.memory[word]);
            UNSCOPED_INFO(std::format("a stack over G+{:#x} is refused (sp {:#x})", word - kG, sp));
            REQUIRE(call.Verdict() == ClearFallback::StackOverlap);
        }

    // The order: the state reasons first, then the stack before any guest word of the model is read: the tiled
    // device, the derivation and the watch, so an unreadable word cannot mask it; a device address too high to read
    // is unreadable before the stack.
    {
        Call call;
        placeStack(call.memory, kG - 0x1000, kStackBase);
        call.entry.sp          = kG + 0x40;
        call.state.diagnostics = true;
        UNSCOPED_INFO("the state reasons come before the stack");
        REQUIRE(call.Verdict() == ClearFallback::Diagnostics);
        call.state.diagnostics = false;
        call.memory[kG + 0x2ABC] |= 0x10000000;
        call.entry.rect = 0;
        unbindTargets(call.memory);
        UNSCOPED_INFO("the stack comes before the tiled device and the derivation");
        REQUIRE(call.Verdict() == ClearFallback::StackOverlap);
        for (const std::uint32_t word : { kG + 0x2ABC, kG + 0x33B0, kG + 0x3098, kG + 0x30A8, kG + 0x10, kG + 0x30, kG + 0x34CC, kG + 0x2A90, kClearStripWord })
        {
            Call masked;
            placeStack(masked.memory, kG - 0x1000, kStackBase);
            masked.entry.sp = kG + 0x40;
            masked.memory.erase(word);
            UNSCOPED_INFO(std::format("an unreadable {:08X} cannot mask the stack", word));
            REQUIRE(masked.Verdict() == ClearFallback::StackOverlap);
        }
        Call unreadable;
        unreadable.memory.erase(kG + 0x2ABC);
        UNSCOPED_INFO("the same word alone is unreadable");
        REQUIRE(unreadable.Verdict() == ClearFallback::Unreadable);
        Call high;
        high.entry.g  = 0xFFFFFFF0U;
        high.entry.sp = 0xFFFFFFF8U;
        UNSCOPED_INFO("a device address too high to read is unreadable before the stack");
        REQUIRE(high.Verdict() == ClearFallback::Unreadable);
    }

#if REREVVED_DEVELOPER
    // The counter and the exit probe's reason.
    TakeClearConsumeCounts();
    NoteClearFallback(ClearFallback::StackOverlap);
    const auto counts = TakeClearConsumeCounts();
    UNSCOPED_INFO("counted");
    REQUIRE((counts.consumed == 0 && counts.fallback[static_cast<std::size_t>(ClearFallback::StackOverlap)] == 1));
    UNSCOPED_INFO("the counter name: " + FormatClearConsumeCounts(counts));
    REQUIRE(FormatClearConsumeCounts(counts) == " clear_consume_ok=0 clear_consume_fallback_stack_overlap=1");
    TakeClearExitCounts();
    NoteClearExitUnmodeled(ClearExitRefusal::StackOverlap);
    const auto exits = TakeClearExitCounts();
    UNSCOPED_INFO("the exit probe counts stack_overlap as unmodeled");
    REQUIRE((exits.ok == 0 && exits.unmodeled[static_cast<std::size_t>(ClearExitRefusal::StackOverlap)] == 1));
    UNSCOPED_INFO("the unmodeled counter name: " + FormatClearExitCounts(exits));
    REQUIRE(FormatClearExitCounts(exits).ends_with(" clear_exit_unmodeled_stack_overlap=1"));
#endif
}

void testTiledConditions()
{
    // The tiling byte, with no surface to match: bit 0x10 tiles; bit 0x20 tiles when every bound slot is the saved one.
    for (const std::uint32_t top : { 0x10U, 0x30U, 0x50U, 0xF0U, 0x1FU })
    {
        Call call;
        call.memory[kG + 0x2ABC] = (top << 24) | 0x00000F00;
        UNSCOPED_INFO(std::format("tiling byte {:02X}", top));
        REQUIRE(call.Verdict() == ClearFallback::Tiled);
    }
    for (const std::uint32_t top : { 0x01U, 0x02U, 0x04U, 0x08U, 0x40U, 0x80U, 0xCFU })
    {
        Call call;
        call.memory[kG + 0x2ABC] = (top << 24) | 0x00000F00;
        UNSCOPED_INFO(std::format("tiling byte {:02X} is not tiled", top));
        REQUIRE(!call.Verdict());
    }
    {
        // Bit 0x20 and the bound RT0 differs from the saved one: the device is not tiled here.
        Call call;
        call.memory[kG + 0x2ABC] = 0x20000F00;
        call.memory[kG + 0x31B8] = kSurface + 0x900;
        UNSCOPED_INFO("bit 0x20 with a different saved surface is untiled");
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x31B8] = kSurface;      // the bound RT0; the other color slots are unbound
        call.memory[kG + 0x31C8] = kDepthSurface; // the depth slot
        UNSCOPED_INFO("bit 0x20 with every slot matching");
        REQUIRE(call.Verdict() == ClearFallback::Tiled);
    }
    {
        Call call;
        call.memory[kG + 0x33B0] = 0x40;
        UNSCOPED_INFO("the bracket word");
        REQUIRE(call.Verdict() == ClearFallback::Tiled);
        // The bracket is judged with the tiled device, before the derivation of the calls.
        call.entry.rect = 0;
        unbindTargets(call.memory);
        UNSCOPED_INFO("the bracket word before the derivation");
        REQUIRE(call.Verdict() == ClearFallback::Tiled);
    }
    // The words are read before the derivation, in Unreadable's name.
    for (const std::uint32_t address : { kG + 0x2ABC, kG + 0x33B0 })
    {
        Call call;
        call.memory.erase(address);
        UNSCOPED_INFO(std::format("{:08X} unreadable", address));
        REQUIRE(call.Verdict() == ClearFallback::Unreadable);
    }
}

// The ring and allocator gates at their edges, through the whole verdict.
void testRingAndAllocator()
{
    // 241 words for the fixture (n = 4, one call, one strip, no dirty bits): the body's 0x826AC8C8 (52), the
    // blocks (63), the call's 0x826A1D90 (12 + 52), one 0x826A1800 (22) and 0x8268D3A0 (40).
    constexpr std::uint32_t kWords = 52 + 63 + (12 + 52) + 22 + 40;
    UNSCOPED_INFO("the hand count");
    REQUIRE(kWords == 241);
    {
        Call      call;
        ClearPlan plan;
        UNSCOPED_INFO("admitted");
        REQUIRE(!call.Verdict(&plan));
        UNSCOPED_INFO("the model counts the same words");
        REQUIRE(ClearMaxRingWords(plan.effects, plan.sides, plan.watch) == kWords);
    }
    // The limits after the pool lowers them by 84.
    const auto withCursor = [](std::uint32_t cursor)
    {
        Call call;
        call.memory[kG + 0x30] = cursor;
        return call;
    };
    UNSCOPED_INFO("the words end exactly at the soft limit");
    REQUIRE(!withCursor(kLimit38 - 84 - 4 * kWords).Verdict());
    UNSCOPED_INFO("one word past the soft limit");
    REQUIRE(withCursor(kLimit38 - 84 - 4 * kWords + 4).Verdict() == ClearFallback::RingSpace);
    {
        // The hard limit: the soft limit may be raised, the end must still be below G+0x34.
        auto call              = withCursor(kLimit38 - 84 - 4 * kWords);
        call.memory[kG + 0x34] = kLimit38 - 84 + 84;
        UNSCOPED_INFO("at the hard limit");
        REQUIRE(call.Verdict() == ClearFallback::RingSpace);
        call.memory[kG + 0x34] += 1;
        UNSCOPED_INFO("below the hard limit");
        REQUIRE(!call.Verdict());
    }
    // The pool threshold: used after the take, past it the allocator retargets and Original flushes.
    {
        Call call;
        call.memory[kG + 0x3A54] = kThreshold - 84;
        UNSCOPED_INFO("the count reaches the threshold");
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x3A54] += 1;
        UNSCOPED_INFO("the count passes the threshold");
        REQUIRE(call.Verdict() == ClearFallback::RingSpace);
    }
    {
        // Two edge calls: the threshold sees the sum.
        Scene scene;
        scene.rect = { 10, 5, 700, 400 };
        Call call(scene);
        call.memory[kG + 0x3A54] = kThreshold - 336;
        UNSCOPED_INFO("the sum reaches the threshold");
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x3A54] += 1;
        UNSCOPED_INFO("the sum passes the threshold");
        REQUIRE(call.Verdict() == ClearFallback::RingSpace);
    }
    // The pool object: none, no fixed buffer, and a buffer below the cursor plus the words and 164 bytes.
    {
        Call call;
        call.memory[kPoolObject + 152] = 0;
        UNSCOPED_INFO("a pool object without a fixed buffer");
        REQUIRE(call.Verdict() == ClearFallback::AllocPath);
    }
    {
        // The ring limits would be the first to fail with a pool this high, so move them out of the way.
        Call call;
        call.memory[kG + 0x38]   = kPoolTop + 0x100000;
        call.memory[kG + 0x34]   = kPoolTop + 0x100000 + 160;
        call.memory[kG + 0x30]   = kCursor;
        const std::uint32_t end  = kCursor + 4 * kWords;
        call.memory[kG + 0x3A44] = end + 164 + 84;
        UNSCOPED_INFO("the buffer ends 164 bytes above the words");
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x3A44] -= 4;
        UNSCOPED_INFO("four bytes short");
        REQUIRE(call.Verdict() == ClearFallback::AllocCapacity);
    }
    // The second take lands on the first buffer: capacity is judged on the last.
    {
        Scene scene;
        scene.rect = { 10, 5, 700, 400 };
        Call call(scene);
        call.memory[kG + 0x38] = kPoolTop + 0x100000;
        call.memory[kG + 0x34] = kPoolTop + 0x100000 + 160;
        ClearPlan plan;
        UNSCOPED_INFO("edge strips admitted");
        REQUIRE(!call.Verdict(&plan));
        const std::uint32_t end  = kCursor + 4 * ClearMaxRingWords(plan.effects, plan.sides, plan.watch);
        call.memory[kG + 0x3A44] = end + 164 + 336;
        UNSCOPED_INFO("the last buffer ends 164 bytes above the words");
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x3A44] -= 4;
        UNSCOPED_INFO("four bytes short for the last buffer");
        REQUIRE(call.Verdict() == ClearFallback::AllocCapacity);
    }
    // A flush without calls takes nothing from the pool, so a missing pool is not a reason.
    {
        Scene scene;
        scene.rt = { false, false, false, false };
        Call call(scene);
        call.memory[kG + 0x34CC] = 0;
        call.memory.erase(kPoolObject + 152);
        call.memory.erase(kPoolObject + 156);
        UNSCOPED_INFO("no pool is read without a take");
        REQUIRE(!call.Verdict());
        // It still flushes if its words pass the limit: 52 + 63 + 40 words.
        call.memory[kG + 0x30] = kLimit38 - 4 * (52 + 63 + 40);
        UNSCOPED_INFO("the no-call words end at the soft limit");
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x30] += 4;
        UNSCOPED_INFO("one word past it");
        REQUIRE(call.Verdict() == ClearFallback::RingSpace);
    }
    // The flush groups count the entry dirty bits: all ones adds 119 words.
    {
        Scene scene;
        scene.dirty10 = scene.dirty18 = scene.dirty20 = kAll;
        Call call(scene);
        call.memory[kG + 0x30] = kLimit38 - 84 - 4 * (kWords + 119);
        UNSCOPED_INFO("the words with every dirty bit end at the limit");
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x30] += 4;
        UNSCOPED_INFO("one word past it");
        REQUIRE(call.Verdict() == ClearFallback::RingSpace);
    }
    // A render target count past the loops' bound is unbounded words.
    {
        Call call;
        call.memory[kG + 0x31CC] = 17;
        UNSCOPED_INFO("17 render targets");
        REQUIRE(call.Verdict() == ClearFallback::RingSpace);
        call.memory[kG + 0x31CC] = 16;
        UNSCOPED_INFO("16 render targets");
        REQUIRE(!call.Verdict());
    }
}

void testEarlyExits()
{
    // Neither early exit reaches the flush groups: no depth surface with only depth flags, an empty rectangle.
    {
        Scene scene;
        scene.flags      = 0x10;
        scene.depthBound = false;
        Call call(scene);
        UNSCOPED_INFO("no depth surface for a depth-only clear");
        REQUIRE(call.Verdict() == ClearFallback::EarlyExit);
    }
    {
        Call call;
        call.memory[kRect + 12] = 0;
        UNSCOPED_INFO("an empty rectangle");
        REQUIRE(call.Verdict() == ClearFallback::EarlyExit);
    }
    {
        // The viewport clips the rectangle away.
        Call call;
        call.memory[kG + 0x3168] = std::bit_cast<std::uint32_t>(1000.0f);
        UNSCOPED_INFO("the viewport leaves nothing");
        REQUIRE(call.Verdict() == ClearFallback::EarlyExit);
    }
    {
        // Depth dropped but a color flag remains: the body still runs.
        Scene scene;
        scene.flags      = 0x11;
        scene.depthBound = false;
        Call      call(scene);
        ClearPlan plan;
        UNSCOPED_INFO("the depth bits are dropped and the clear goes on");
        REQUIRE((!call.Verdict(&plan) && plan.calls.count == 1 && plan.calls.calls[0].flags == 0x1));
    }
}

void testSideBuffers()
{
    const auto strips = [](std::int32_t left, std::int32_t top, std::int32_t right, std::int32_t bottom, std::uint32_t control, std::uint32_t split)
    {
        Scene scene;
        scene.rect              = { left, top, right, bottom };
        scene.control           = control;
        auto memory             = build(scene);
        memory[kClearStripWord] = split;
        ClearCalls calls;
        ClearCall  call;
        call.rect = { left, top, right, bottom };
        calls.Add(call);
        calls.reachedFlush = true;
        const auto sides   = DeriveClearSideBuffers(readerOf(memory), entryOf(scene), calls);
        UNSCOPED_INFO("side buffers derived");
        REQUIRE(sides.has_value());
        std::vector<std::uint32_t> bytes(sides->bytes.begin(), sides->bytes.begin() + static_cast<std::ptrdiff_t>(sides->count));
        return bytes;
    };
    using Bytes = std::vector<std::uint32_t>;
    // Mode 0 is the 80x16 grid, mode 1 80x8 and modes 2 and 3 40x8.
    UNSCOPED_INFO("a rectangle on the grid is all interior");
    REQUIRE(strips(0, 0, 640, 480, 0, 1) == Bytes{ 84 });
    UNSCOPED_INFO("no whole tile: one strip");
    REQUIRE(strips(0, 0, 64, 8, 0, 1) == Bytes{ 84 });
    UNSCOPED_INFO("three edges and the interior");
    REQUIRE(strips(10, 5, 700, 400, 0, 1) == Bytes{ 252, 84 });
    UNSCOPED_INFO("the split word zero keeps one strip");
    REQUIRE(strips(10, 5, 700, 400, 0, 0) == Bytes{ 84 });
    UNSCOPED_INFO("four edges");
    REQUIRE(strips(10, 5, 700, 405, 0, 1) == Bytes{ 336, 84 });
    UNSCOPED_INFO("tile-aligned edges");
    REQUIRE(strips(80, 16, 640, 480, 0, 1) == Bytes{ 84 });
    UNSCOPED_INFO("one edge (bottom)");
    REQUIRE(strips(0, 0, 640, 487, 0, 1) == Bytes{ 84, 84 });
    UNSCOPED_INFO("mode 1 has an 8 row grid: 487 -> 480");
    REQUIRE(strips(0, 0, 640, 487, 1U << 16, 1) == Bytes{ 84, 84 });
    UNSCOPED_INFO("mode 1, 483 -> 480");
    REQUIRE(strips(0, 0, 640, 483, 1U << 16, 1) == Bytes{ 84, 84 });
    UNSCOPED_INFO("mode 1, 480 is aligned");
    REQUIRE(strips(0, 0, 640, 480, 1U << 16, 1) == Bytes{ 84 });
    UNSCOPED_INFO("mode 0, 12 rows have no whole 16 row tile");
    REQUIRE(strips(0, 0, 640, 12, 0, 1) == Bytes{ 84 });
    UNSCOPED_INFO("mode 1, 12 rows: top 0, bottom 8: the interior and one edge");
    REQUIRE(strips(0, 0, 640, 12, 1U << 16, 1) == Bytes{ 84, 84 });
    UNSCOPED_INFO("mode 2 is 40 wide: 41 -> 40, one edge");
    REQUIRE(strips(0, 0, 41, 8, 2U << 16, 1) == Bytes{ 84, 84 });
    UNSCOPED_INFO("mode 3 is 40 wide too");
    REQUIRE(strips(0, 0, 41, 8, 3U << 16, 1) == Bytes{ 84, 84 });
    // divw truncates toward zero, so a negative edge aligns up to zero: -5 and -20 land on 0 and are edges; -100 too.
    UNSCOPED_INFO("a negative left and top: two edge strips");
    REQUIRE(strips(-5, -20, 640, 480, 0, 1) == Bytes{ 168, 84 });
    UNSCOPED_INFO("a negative left: one edge strip");
    REQUIRE(strips(-100, 0, 640, 480, 0, 1) == Bytes{ 84, 84 });
}

void testSideBufferErrors()
{
    Scene      scene;
    auto       memory = build(scene);
    ClearCalls calls;
    calls.reachedFlush = true;
    ClearCall call;
    call.rect = { 0, 0, 64, 8 };
    calls.Add(call);
    std::string why;
    memory.erase(kClearStripWord);
    UNSCOPED_INFO("the strip word is read");
    REQUIRE((!DeriveClearSideBuffers(readerOf(memory), entryOf(scene), calls, &why) && why.starts_with("unreadable")));
    memory = build(scene);
    memory.erase(kG + 0x2880);
    UNSCOPED_INFO("the multisample word is read");
    REQUIRE((!DeriveClearSideBuffers(readerOf(memory), entryOf(scene), calls, &why) && why.starts_with("unreadable")));
    ClearCalls overflow = calls;
    overflow.overflow   = 1;
    memory              = build(scene);
    UNSCOPED_INFO("an overflowed call list");
    REQUIRE((!DeriveClearSideBuffers(readerOf(memory), entryOf(scene), overflow, &why) && why == "call_overflow"));
    ClearCalls empty;
    empty.reachedFlush = true;
    memory.erase(kClearStripWord);
    const auto none = DeriveClearSideBuffers(readerOf(memory), entryOf(scene), empty);
    UNSCOPED_INFO("no calls read no word");
    REQUIRE((none && none->count == 0));
}

void testDeviceTiled()
{
    auto          memory = build(Scene{});
    bool          tiled  = true;
    std::uint32_t tile = 0, failed = 0;
    UNSCOPED_INFO("untiled");
    REQUIRE((ClearDeviceTiled(readerOf(memory), kG, tiled, &tile) && !tiled && tile == 0));
    memory[kG + 0x2ABC] = 0x30000F00;
    UNSCOPED_INFO("bit 0x10 tiles whatever else");
    REQUIRE((ClearDeviceTiled(readerOf(memory), kG, tiled, &tile) && tiled && tile == 0x30));
    memory[kG + 0x2ABC] = 0x20000F00;
    memory[kG + 0x31B8] = kSurface;
    memory[kG + 0x31C8] = kDepthSurface;
    UNSCOPED_INFO("bit 0x20 with the bound slot saved and the others unbound");
    REQUIRE((ClearDeviceTiled(readerOf(memory), kG, tiled, &tile) && tiled));
    memory[kG + 0x31BC] = 0x1234; // slot 1 saved, but unbound now
    UNSCOPED_INFO("an unbound current slot matches anything");
    REQUIRE((ClearDeviceTiled(readerOf(memory), kG, tiled, &tile) && tiled));
    memory[kG + 0x3098] = kSurface + 0x77;
    UNSCOPED_INFO("a different bound slot is not tiled");
    REQUIRE((ClearDeviceTiled(readerOf(memory), kG, tiled, &tile) && !tiled));
    memory.erase(kG + 0x31B8);
    UNSCOPED_INFO("an unreadable saved slot names its address");
    REQUIRE((!ClearDeviceTiled(readerOf(memory), kG, tiled, &tile, &failed) && failed == kG + 0x31B8));
    memory.erase(kG + 0x2ABC);
    UNSCOPED_INFO("an unreadable tiling byte");
    REQUIRE((!ClearDeviceTiled(readerOf(memory), kG, tiled, &tile, &failed) && failed == kG + 0x2ABC));
    UNSCOPED_INFO("an address that wraps");
    REQUIRE(!ClearDeviceTiled(readerOf(memory), 0xFFFFFFF0U, tiled, &tile, &failed));
}

void testPlanArguments()
{
    Scene scene;
    scene.flags = 0x23;
    scene.rect  = { 0, 0, 640, 480 };
    scene.rt    = { true, true, false, false };
    Call      call(scene);
    ClearPlan plan;
    UNSCOPED_INFO("two calls");
    REQUIRE((!call.Verdict(&plan) && plan.calls.count == 2));
    const auto first = ClearPlanArguments(plan, 0);
    UNSCOPED_INFO("the registers of the first call");
    REQUIRE((first == std::array<std::uint32_t, 8>{ kG, 0x23, 0, 0, 0, 640, 480, kColor }));
    const auto second = ClearPlanArguments(plan, 1);
    UNSCOPED_INFO("the depth bits go with the first call only");
    REQUIRE((second[1] == 0x03 && second[2] == 1));
    const auto inputs = ClearPlanInputs(plan, 0);
    UNSCOPED_INFO("the stencil and the color words come from the entry");
    REQUIRE((inputs.stencil == 0x5A && inputs.color == std::array<std::uint32_t, 4>{ 0x3F800000, 0x3F800001, 0x3F800002, 0x3F800003 }));
    // The default color: a null pointer reads the guest's constant and passes the body frame copy.
    Call defaulted;
    defaulted.entry.color                     = 0;
    defaulted.memory[kClearDefaultColor]      = 0x11111111;
    defaulted.memory[kClearDefaultColor + 4]  = 0x22222222;
    defaulted.memory[kClearDefaultColor + 8]  = 0x33333333;
    defaulted.memory[kClearDefaultColor + 12] = 0x44444444;
    ClearPlan defaultPlan;
    UNSCOPED_INFO("the default color is admitted");
    REQUIRE(!defaulted.Verdict(&defaultPlan));
    const auto args = ClearPlanArguments(defaultPlan, 0);
    UNSCOPED_INFO("the color pointer is the body frame copy");
    REQUIRE(args[7] == kSp - kClearFrameSize + kClearBodyColorOffset);
    UNSCOPED_INFO("the default words");
    REQUIRE((ClearPlanInputs(defaultPlan, 0).color == std::array<std::uint32_t, 4>{ 0x11111111, 0x22222222, 0x33333333, 0x44444444 }));
    // A depth-only call names the depth slot.
    Scene depth;
    depth.flags = 0x10;
    Call      depthCall(depth);
    ClearPlan depthPlan;
    UNSCOPED_INFO("depth only admitted");
    REQUIRE(!depthCall.Verdict(&depthPlan));
    UNSCOPED_INFO("the depth-only index");
    REQUIRE(ClearPlanArguments(depthPlan, 0)[2] == kGuestDepthOnlyClearIndex);
}

#if REREVVED_DEVELOPER
void testExitMismatches()
{
    Call      call;
    ClearPlan plan;
    UNSCOPED_INFO("admitted");
    REQUIRE(!call.Verdict(&plan));
    ClearWatch exit = plan.watch;
    exit.dirty      = plan.exit.dirty;
    exit.allocByte  = plan.exit.allocByte;
    exit.limit34    = plan.exit.limit34;
    exit.limit38    = plan.exit.limit38;
    exit.cursor     = plan.exit.poolPos;
    exit.allocated  = plan.exit.poolUsed;
    exit.wrapPos    = plan.exit.wrapPos;
    UNSCOPED_INFO("an exact exit");
    REQUIRE(ClearExitMismatches(plan.exit, exit) == 0);
    for (std::size_t bit = 0; bit < kClearExitWordCount; ++bit)
    {
        ClearWatch changed = exit;
        switch (bit)
        {
            case 0:
            case 1:
            case 2:
                changed.dirty[bit] ^= 1;
                break;
            case 3:
                changed.allocByte ^= 1;
                break;
            case 4:
                changed.limit34 ^= 4;
                break;
            case 5:
                changed.limit38 ^= 4;
                break;
            case 6:
                changed.cursor ^= 4;
                break;
            case 7:
                changed.allocated ^= 4;
                break;
            default:
                changed.wrapPos ^= 4;
                break;
        }
        UNSCOPED_INFO(std::format("word {} is told apart", kClearExitWordNames[bit]));
        REQUIRE(ClearExitMismatches(plan.exit, changed) == (1U << bit));
    }
}

#endif

void testExitRefusals()
{
    // DeriveClearExitState on its own: each refusal in its order.
    Call      call;
    ClearPlan plan;
    UNSCOPED_INFO("admitted");
    REQUIRE(!call.Verdict(&plan));
    const auto refusal = [&](const ClearEffects& effects, const ClearSideBuffers& sides, const ClearWatch& watch)
    {
        ClearExitRefusal reason = ClearExitRefusal::None;
        std::string      why;
        UNSCOPED_INFO("refused with a reason");
        REQUIRE((!DeriveClearExitState(effects, sides, watch, &reason, &why) && !why.empty()));
        return reason;
    };
    {
        auto effects            = plan.effects;
        effects.tiledBlockCalls = 1;
        auto watch              = plan.watch;
        watch.allocByte         = 0x20; // later reasons set too
        UNSCOPED_INFO("a call that takes the tile block A");
        REQUIRE(refusal(effects, plan.sides, watch) == ClearExitRefusal::TiledBlock);
    }
    {
        // The bracket and the tile loop are modeled: neither is a reason, and the bracket alone leaves the fixed pool takes as they were.
        auto watch      = plan.watch;
        watch.bracket   = 1;
        const auto exit = DeriveClearExitState(plan.effects, plan.sides, watch);
        UNSCOPED_INFO("the bracket");
        REQUIRE((exit && exit->poolPos == plan.exit.poolPos && exit->limit34 == plan.exit.limit34 && ClearShapeTiled(exit->shape) && (exit->shape & kClearShapeBracket)));
        auto effects           = plan.effects;
        effects.tiledLoopCalls = 1;
        effects.tiledNoneCalls = 1;
        UNSCOPED_INFO("a tile loop and a tiled call that takes nothing");
        REQUIRE(DeriveClearExitState(effects, plan.sides, plan.watch).has_value());
    }
    {
        auto effects         = plan.effects;
        effects.reachedFlush = false;
        UNSCOPED_INFO("an early exit");
        REQUIRE(refusal(effects, plan.sides, plan.watch) == ClearExitRefusal::EarlyExit);
    }
    {
        auto watch      = plan.watch;
        watch.allocByte = 0x20;
        UNSCOPED_INFO("the marker");
        REQUIRE(refusal(plan.effects, plan.sides, watch) == ClearExitRefusal::AllocMarker);
    }
    {
        // Side buffers that disagree with the effects are an unmodeled allocator path.
        UNSCOPED_INFO("calls without side buffers");
        REQUIRE(refusal(plan.effects, ClearSideBuffers{}, plan.watch) == ClearExitRefusal::AllocPath);
        auto effects      = plan.effects;
        effects.allocates = false;
        UNSCOPED_INFO("side buffers without allocating calls");
        REQUIRE(refusal(effects, plan.sides, plan.watch) == ClearExitRefusal::AllocPath);
    }
    {
        auto watch       = plan.watch;
        watch.poolObject = 0;
        // A null pool object with the allocator's words unset has no area: the take fails.
        UNSCOPED_INFO("a null pool object without an area");
        REQUIRE(refusal(plan.effects, plan.sides, watch) == ClearExitRefusal::AllocCapacity);
        watch.sideEnd   = kSideEnd;
        watch.wrapPos   = kWrapPos;
        watch.wrapLimit = kWrapLimit;
        watch.segment   = kSegment;
        watch.progress  = kProgress;
        const auto exit = DeriveClearExitState(plan.effects, plan.sides, watch);
        UNSCOPED_INFO("a null pool object with an area");
        REQUIRE((exit && exit->poolPos == kLimit34 + 60 + 84 && exit->limit34 == kLimit34));
        watch.progress = 0x00207006;
        UNSCOPED_INFO("a take that waits");
        REQUIRE(refusal(plan.effects, plan.sides, watch) == ClearExitRefusal::AllocWait);
        watch          = plan.watch;
        watch.poolBase = 0;
        UNSCOPED_INFO("no fixed buffer");
        REQUIRE(refusal(plan.effects, plan.sides, watch) == ClearExitRefusal::AllocPath);
    }
    {
        auto watch = plan.watch;
        watch.ring = 0xFFFFFFF0U;
        UNSCOPED_INFO("a cursor that wraps");
        REQUIRE(refusal(plan.effects, plan.sides, watch) == ClearExitRefusal::RingSpace);
        watch         = plan.watch;
        watch.limit38 = 10; // below the 84 bytes a take lowers it by
        UNSCOPED_INFO("a limit below the take");
        REQUIRE(refusal(plan.effects, plan.sides, watch) == ClearExitRefusal::RingSpace);
    }
    {
        auto watch    = plan.watch;
        watch.cursor  = watch.ring + 64;
        watch.limit34 = watch.limit38 = kPoolTop;
        UNSCOPED_INFO("a pool top below the cursor");
        REQUIRE(refusal(plan.effects, plan.sides, watch) == ClearExitRefusal::AllocCapacity);
    }
}

void testProbeStackPlaceholder()
{
    // A probe with nothing to check is what the consume stacks: neither check flag, live, found by its frame.
    ClearProbeStack stack;
    ClearProbe      placeholder;
    placeholder.deriveCalls = false;
    placeholder.effects     = false;
    ClearDropCounts drops;
    UNSCOPED_INFO("stacked");
    REQUIRE((stack.Push(kSp, placeholder, &drops) == 0 && stack.Size() == 1));
    unsigned dropped = 0;
    UNSCOPED_INFO("found by frame");
    REQUIRE((stack.Find(kSp - kClearFrameSize, dropped) != nullptr && dropped == 0));
    // A second call above it, then the first leaving without its exit hook: dropped, and not counted as unpaired.
    ClearProbe inner = placeholder;
    UNSCOPED_INFO("nested placeholder");
    REQUIRE((stack.Push(kSp - 400, inner, &drops) == 0 && stack.Size() == 2));
    UNSCOPED_INFO("stale placeholders are dropped without being counted");
    REQUIRE((stack.Push(kSp, placeholder, &drops) == 2 && stack.Size() == 1 && drops.derive == 0 && drops.effects == 0));
    UNSCOPED_INFO("popped");
    REQUIRE((stack.Pop(kSp - kClearFrameSize, dropped).has_value() && stack.Size() == 0));
}

#if REREVVED_DEVELOPER
void testCounts()
{
    TakeClearConsumeCounts();
    NoteClearConsumed();
    NoteClearConsumed();
    NoteClearFallback(ClearFallback::Tiled);
    NoteClearFallback(ClearFallback::Tiled);
    NoteClearFallback(ClearFallback::Nested);
    auto counts = TakeClearConsumeCounts();
    UNSCOPED_INFO("counts");
    REQUIRE((counts.consumed == 2 && counts.fallback[static_cast<std::size_t>(ClearFallback::Tiled)] == 2 && counts.fallback[static_cast<std::size_t>(ClearFallback::Nested)] == 1));
    UNSCOPED_INFO("formatted: " + FormatClearConsumeCounts(counts));
    REQUIRE(FormatClearConsumeCounts(counts) == " clear_consume_ok=2 clear_consume_fallback_nested=1 clear_consume_fallback_tiled=2");
    counts = TakeClearConsumeCounts();
    UNSCOPED_INFO("take resets");
    REQUIRE((counts.consumed == 0 && counts.fallback == std::array<std::uint64_t, kClearFallbackCount>{}));
    UNSCOPED_INFO("an empty window");
    REQUIRE(FormatClearConsumeCounts(counts) == " clear_consume_ok=0");

    const std::array<const char*, kClearFallbackCount> expectedNames{ "stubs", "nested", "ledger", "entry_state", "diagnostics", "stack_overlap", "tiled", "derive_unsupported", "early_exit", "tiled_block", "alloc_marker", "unreadable", "alloc_path", "ring_space", "alloc_capacity", "alloc_wait" };
    for (std::size_t i = 0; i < kClearFallbackCount; ++i)
    {
        UNSCOPED_INFO(std::format("name {}", i));
        REQUIRE(kClearFallbackNames[i] == expectedNames[i]);
    }

    // The exit model counters.
    TakeClearExitCounts();
    NoteClearExit(0);
    NoteClearExit(0);
    NoteClearExit((1U << 1) | (1U << 7));
    NoteClearExitUnmodeled(ClearExitRefusal::TiledBlock);
    NoteClearExitUnmodeled(ClearExitRefusal::TiledBlock);
    NoteClearExitUnmodeled(ClearExitRefusal::RingSpace);
    const auto exits = TakeClearExitCounts();
    UNSCOPED_INFO("exit counts");
    REQUIRE((exits.ok == 2 && exits.mismatch[1] == 1 && exits.mismatch[7] == 1 && exits.mismatch[0] == 0 && exits.unmodeled[static_cast<std::size_t>(ClearExitRefusal::TiledBlock)] == 2));
    UNSCOPED_INFO("untiled shapes are not counted as tiled");
    REQUIRE((exits.tiled == 0 && exits.tiledOk == 0));
    UNSCOPED_INFO("exit formatted: " + FormatClearExitCounts(exits));
    REQUIRE(FormatClearExitCounts(exits) ==
            " clear_exit_ok=2 clear_exit_tiled=0 clear_exit_tiled_ok=0"
            " clear_exit_mismatch_dirty10=0 clear_exit_mismatch_dirty18=1 clear_exit_mismatch_dirty20=0 clear_exit_mismatch_byte2abd=0"
            " clear_exit_mismatch_limit34=0 clear_exit_mismatch_limit38=0 clear_exit_mismatch_pool_pos=0 clear_exit_mismatch_pool_used=1 clear_exit_mismatch_wrap_pos=0"
            " clear_exit_unmodeled_tiled_block=2 clear_exit_unmodeled_ring_space=1");
    // Tiled shapes: counted as tiled, ok when exact, and by kind (a shape can have several).
    constexpr unsigned loopShape = kClearShapeLoop | kClearShapeBracket | kClearShapePredicate;
    NoteClearExit(0, loopShape);
    NoteClearExit(1U << 2, kClearShapeLoop | kClearShapeByte10);
    NoteClearExit(0, kClearShapeBracket | kClearShapeState);
    NoteClearExit(0, 0);
    NoteClearExit(0, kClearShapePredicate | kClearShapeState);
    const auto tiledExits = TakeClearExitCounts();
    UNSCOPED_INFO("tiled exit counts: untiled, and shapes without the loop or bracket, are not tiled");
    REQUIRE((tiledExits.ok == 4 && tiledExits.tiled == 3 && tiledExits.tiledOk == 2));
    UNSCOPED_INFO("tiled kinds");
    REQUIRE((tiledExits.shapes[0] == 2 && tiledExits.shapes[1] == 2 && tiledExits.shapes[2] == 1 && tiledExits.shapes[3] == 1 && tiledExits.shapes[4] == 1));
    UNSCOPED_INFO("tiled exit formatted: " + FormatClearExitCounts(tiledExits));
    REQUIRE((FormatClearExitCounts(tiledExits).starts_with(" clear_exit_ok=4 clear_exit_tiled=3 clear_exit_tiled_ok=2 ") &&
             FormatClearExitCounts(tiledExits).ends_with(" clear_exit_tiled_loop=2 clear_exit_tiled_bracket=2 clear_exit_tiled_byte10=1 clear_exit_tiled_state=1 clear_exit_tiled_predicate=1")));

    // The consume counters: a tiled call is also an ok, with its kinds.
    NoteClearConsumed();
    NoteClearConsumed(kClearShapeLoop | kClearShapeBracket);
    NoteClearConsumed(kClearShapeLoop | kClearShapePredicate | kClearShapeState);
    NoteClearConsumed(kClearShapeState);
    const auto tiledConsumed = TakeClearConsumeCounts();
    UNSCOPED_INFO("tiled consume counts");
    REQUIRE((tiledConsumed.consumed == 4 && tiledConsumed.consumedTiled == 2 && tiledConsumed.shapes[0] == 2 && tiledConsumed.shapes[1] == 1 && tiledConsumed.shapes[3] == 1 && tiledConsumed.shapes[4] == 1));
    UNSCOPED_INFO("tiled consume formatted: " + FormatClearConsumeCounts(tiledConsumed));
    REQUIRE(FormatClearConsumeCounts(tiledConsumed) == " clear_consume_ok=4 clear_consume_ok_tiled=2 clear_consume_tiled_loop=2 clear_consume_tiled_bracket=1 clear_consume_tiled_state=1 clear_consume_tiled_predicate=1");
}

#endif

void testEntry()
{
    UNSCOPED_INFO("the entry is the Clear wrapper only");
    REQUIRE((ClearConsumeEntry(0x826A2970) && !ClearConsumeEntry(0x826A22F8) && !ClearConsumeEntry(0x826A1D90) && !ClearConsumeEntry(0x826A2960)));
    // Both cvars are on by default; the consume acts only while ring stub services are published.
    UNSCOPED_INFO("the cvars are on by default");
    REQUIRE((REXCVAR_GET(native_consume_clear) && REXCVAR_GET(native_consume_clear_tiled) && ClearTiledConsumeRequested()));
    UNSCOPED_INFO("unpublished ring stubs leave the consume unrequested");
    REQUIRE((!RingStubsPublished() && !ClearConsumeRequested()));
    ringStubsActive.store(true);
    UNSCOPED_INFO("published ring stubs request the consume");
    REQUIRE(ClearConsumeRequested());
    REXCVAR_SET(native_consume_clear, false);
    UNSCOPED_INFO("the cvar off leaves it unrequested");
    REQUIRE(!ClearConsumeRequested());
    REXCVAR_SET(native_consume_clear, true);
    ringStubsActive.store(false);
}

// Null-pool strips start at G+0x34 + 60 when G+0x3A44 is zero
// (g185:12899-13183).
Call nullPoolCall(Scene scene = {})
{
    // Null-pool allocator 0x8269B618 takes each strip at alignment 4.
    Call call(scene);
    call.memory[kG + 0x34CC] = 0;
    return call;
}

Scene edgeStrips()
{
    Scene scene;
    scene.rect = { 10, 5, 700, 400 };
    return scene;
}

void testNullPool()
{
    static_assert(kForwardStart == 0x00203FDC);
    // One strip, forward: G+0x3A44 takes the end; neither limit moves; the wrap word stays.
    {
        auto      call = nullPoolCall();
        ClearPlan plan;
        UNSCOPED_INFO("a null pool object is admitted");
        REQUIRE(!call.Verdict(&plan));
        UNSCOPED_INFO("one 84 byte strip");
        REQUIRE((plan.sides.count == 1 && plan.sides.bytes[0] == 84));
        UNSCOPED_INFO("the forward take stores G+0x3A44");
        REQUIRE((plan.exit.poolPos == kForwardStart + 84 && plan.exit.wrapPos == kWrapPos));
        UNSCOPED_INFO("neither limit moves and G+0x3A54 grows");
        REQUIRE((plan.exit.limit34 == kLimit34 && plan.exit.limit38 == kLimit38 && plan.exit.poolUsed == kUsed + 84));
        std::vector<std::uint32_t> stored;
        for (const auto& write : ClearExitWrites(kG, plan.watch, plan.exit))
            stored.push_back(write.address);
        UNSCOPED_INFO("G+0x3A44 and G+0x3A54 are stored");
        REQUIRE((std::find(stored.begin(), stored.end(), kG + 0x3A44) != stored.end() && std::find(stored.begin(), stored.end(), kG + 0x3A54) != stored.end()));
        for (const auto address : { kG + 0x34, kG + 0x38, kG + 0x3A4C })
        {
            UNSCOPED_INFO(std::format("{:08X} is not stored", address));
            REQUIRE(std::find(stored.begin(), stored.end(), address) == stored.end());
        }
    }
    // A nonzero G+0x3A44 is the start, rounded up to 4.
    {
        auto call                = nullPoolCall();
        call.memory[kG + 0x3A44] = 0x00204002;
        ClearPlan plan;
        UNSCOPED_INFO("admitted");
        REQUIRE(!call.Verdict(&plan));
        UNSCOPED_INFO("the start is aligned up");
        REQUIRE(plan.exit.poolPos == 0x00204004 + 84);
    }
    // Two takes of 252 and 84 bytes (three edge strips, then the interior).
    {
        auto      call = nullPoolCall(edgeStrips());
        ClearPlan plan;
        UNSCOPED_INFO("admitted");
        REQUIRE(!call.Verdict(&plan));
        UNSCOPED_INFO("three edge strips and the interior");
        REQUIRE((plan.sides.count == 2 && plan.sides.bytes[0] == 252 && plan.sides.bytes[1] == 84));
        // The second take starts where the first ended (0x2040D8) and runs past the area (0x204100): it wraps.
        UNSCOPED_INFO("forward then wrap, the second take seeing the first");
        REQUIRE((plan.exit.poolPos == kForwardStart + 252 && plan.exit.wrapPos == kWrapPos + 84));
        UNSCOPED_INFO("both takes are counted");
        REQUIRE(plan.exit.poolUsed == kUsed + 336);
        // A wrap moves G+0x3A4C, and it is stored with the forward word.
        {
            bool wrapStored = false, forwardStored = false;
            for (const auto& write : ClearExitWrites(kG, plan.watch, plan.exit))
            {
                wrapStored    = wrapStored || (write.address == kG + 0x3A4C && write.value == kWrapPos + 84);
                forwardStored = forwardStored || (write.address == kG + 0x3A44 && write.value == kForwardStart + 252);
            }
            UNSCOPED_INFO("G+0x3A4C and G+0x3A44 are stored");
            REQUIRE((wrapStored && forwardStored));
        }
        // Room for both: forward and forward.
        call.memory[kG + 0x3A3C] = 0x00205000;
        UNSCOPED_INFO("admitted");
        REQUIRE(!call.Verdict(&plan));
        UNSCOPED_INFO("forward then forward");
        REQUIRE((plan.exit.poolPos == kForwardStart + 252 + 84 && plan.exit.wrapPos == kWrapPos));
        // No room for either: wrap and wrap, the second starting where the first ended.
        call.memory[kG + 0x3A3C] = 0x00203000;
        UNSCOPED_INFO("admitted");
        REQUIRE(!call.Verdict(&plan));
        UNSCOPED_INFO("wrap then wrap");
        REQUIRE((plan.exit.poolPos == 0 && plan.exit.wrapPos == kWrapPos + 252 + 84));
        // Room only for the second: the wrap does not advance the forward position.
        call.memory[kG + 0x3A3C] = 0x00204080;
        UNSCOPED_INFO("admitted");
        REQUIRE(!call.Verdict(&plan));
        UNSCOPED_INFO("wrap then forward");
        REQUIRE((plan.exit.poolPos == kForwardStart + 84 && plan.exit.wrapPos == kWrapPos + 252));
    }
    // The bracket is judged before the derivation.
    {
        auto call                = nullPoolCall();
        call.memory[kG + 0x33B0] = 1;
        UNSCOPED_INFO("the bracket");
        REQUIRE(call.Verdict() == ClearFallback::Tiled);
    }
    // A take that fails: past the wrap limit (to the byte), and a wrap at address zero.
    {
        auto call                = nullPoolCall();
        call.memory[kG + 0x3A3C] = 0x00203000;
        call.memory[kG + 0x3A50] = kWrapPos + 84;
        UNSCOPED_INFO("the wrap end at the limit fits");
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x3A50] = kWrapPos + 83;
        UNSCOPED_INFO("the wrap end past the limit fails");
        REQUIRE(call.Verdict() == ClearFallback::AllocCapacity);
        call.memory[kG + 0x3A50] = kWrapLimit;
        call.memory[kG + 0x3A4C] = 0;
        UNSCOPED_INFO("a buffer at address zero fails");
        REQUIRE(call.Verdict() == ClearFallback::AllocCapacity);
        // The second take failing fails the call even when the first fits.
        auto two                = nullPoolCall(edgeStrips());
        two.memory[kG + 0x3A50] = kWrapPos + 83;
        UNSCOPED_INFO("the second take fails");
        REQUIRE(two.Verdict() == ClearFallback::AllocCapacity);
    }
    // A take that would wait for the GPU: the segment this take leaves behind against the progress word.
    {
        auto call = nullPoolCall();
        // Segment 5 against progress 5: no wait. Against 6, three behind: waits.
        call.memory[kBlock + 4] = 0x00207006;
        UNSCOPED_INFO("a segment three behind waits");
        REQUIRE(call.Verdict() == ClearFallback::AllocWait);
        // One ahead (progress 0 mod 4 -> ahead 1) waits unless the buffer end is inside the progress block.
        call.memory[kBlock + 4] = 0x00204030;
        UNSCOPED_INFO("one ahead with the end inside the block does not wait");
        REQUIRE(!call.Verdict());
        call.memory[kBlock + 4] = 0x0020402C;
        UNSCOPED_INFO("one ahead with the end past the block waits");
        REQUIRE(call.Verdict() == ClearFallback::AllocWait);
        // The wait is judged per take: the second take wraps to segment 6, one ahead of progress 5.
        auto two               = nullPoolCall(edgeStrips());
        two.memory[kBlock + 4] = 0x00000005;
        UNSCOPED_INFO("the second take waits though the first does not");
        REQUIRE(two.Verdict() == ClearFallback::AllocWait);
    }
    // Forced flush: the count past the threshold, and ring words past the limits (the limits do not move).
    {
        auto call                = nullPoolCall();
        call.memory[kG + 0x3A54] = kThreshold - 84;
        UNSCOPED_INFO("the count reaches the threshold");
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x3A54] += 1;
        UNSCOPED_INFO("the count passes the threshold");
        REQUIRE(call.Verdict() == ClearFallback::RingSpace);
        auto two                = nullPoolCall(edgeStrips());
        two.memory[kG + 0x3A54] = kThreshold - 336;
        UNSCOPED_INFO("the sum reaches the threshold");
        REQUIRE(!two.Verdict());
        two.memory[kG + 0x3A54] += 1;
        UNSCOPED_INFO("the sum passes the threshold");
        REQUIRE(two.Verdict() == ClearFallback::RingSpace);
        constexpr std::uint32_t kWords = 241;
        auto                    at     = nullPoolCall();
        at.memory[kG + 0x30]           = kLimit38 - 4 * kWords;
        UNSCOPED_INFO("the words end at the soft limit, which the null pool does not lower");
        REQUIRE(!at.Verdict());
        at.memory[kG + 0x30] += 4;
        UNSCOPED_INFO("one word past the soft limit");
        REQUIRE(at.Verdict() == ClearFallback::RingSpace);
        auto hard              = nullPoolCall();
        hard.memory[kG + 0x30] = kLimit38 - 4 * kWords;
        hard.memory[kG + 0x34] = kLimit38;
        UNSCOPED_INFO("at the hard limit");
        REQUIRE(hard.Verdict() == ClearFallback::RingSpace);
    }
    // A pool object without a fixed buffer is still the indirect allocator.
    {
        Call call;
        call.memory[kPoolObject + 152] = 0;
        UNSCOPED_INFO("a pool object without a fixed buffer");
        REQUIRE(call.Verdict() == ClearFallback::AllocPath);
    }
    // The words only the null pool reads are read only for it, and must be readable: the progress block is
    // reached through G+0x2A90 and must not be null.
    for (const std::uint32_t address : { kG + 0x3A3C, kG + 0x3A50, kG + 0x3A48, kG + 0x33B4, kG + 0x2A90, kBlock + 4 })
    {
        auto call = nullPoolCall();
        call.memory.erase(address);
        UNSCOPED_INFO(std::format("{:08X} unreadable", address));
        REQUIRE(call.Verdict() == ClearFallback::Unreadable);
        Call fixed;
        fixed.memory.erase(address);
        UNSCOPED_INFO(std::format("{:08X} is not read with a pool object", address));
        REQUIRE(!fixed.Verdict());
    }
    {
        auto call                = nullPoolCall();
        call.memory[kG + 0x2A90] = 0;
        UNSCOPED_INFO("a null progress block");
        REQUIRE(call.Verdict() == ClearFallback::Unreadable);
    }
    {
        // G+0x3A4C is read for every pool object.
        Call call;
        call.memory.erase(kG + 0x3A4C);
        UNSCOPED_INFO("G+0x3A4C is always read");
        REQUIRE(call.Verdict() == ClearFallback::Unreadable);
    }
    // A fixed pool leaves the wrap word alone, and the null pool's order of reasons is the order of the enumerators.
    {
        Call      call;
        ClearPlan plan;
        UNSCOPED_INFO("admitted");
        REQUIRE(!call.Verdict(&plan));
        UNSCOPED_INFO("a fixed pool does not touch G+0x3A4C");
        REQUIRE((plan.exit.wrapPos == kWrapPos && plan.watch.wrapPos == kWrapPos));
    }
    {
        // ring_space < pool_threshold < alloc_fail < alloc_wait, with every later reason set too.
        const std::vector<std::pair<ClearFallback, std::function<void(Call&)>>> list = {
            { ClearFallback::RingSpace, [](Call& c)
              {
                  c.memory[kG + 0x30] = kLimit38 - 16;
              } },
            { ClearFallback::AllocCapacity, [](Call& c)
              {
                  c.memory[kG + 0x3A3C] = 0x00203000;
                  c.memory[kG + 0x3A50] = kWrapPos;
              } },
            { ClearFallback::AllocWait, [](Call& c)
              {
                  c.memory[kBlock + 4] = 0x00207006;
              } },
        };
        for (std::size_t first = 0; first < list.size(); ++first)
        {
            auto call = nullPoolCall();
            for (std::size_t i = first; i < list.size(); ++i)
                list[i].second(call);
            UNSCOPED_INFO(std::format("null pool: with {} and every later reason set the earliest is reported, got {}", kClearFallbackNames[static_cast<std::size_t>(list[first].first)], nameOf(call.Verdict())));
            REQUIRE(call.Verdict() == list[first].first);
        }
        auto alone = nullPoolCall();
        list[2].second(alone);
        UNSCOPED_INFO("alloc_wait alone");
        REQUIRE(alone.Verdict() == ClearFallback::AllocWait);
    }
}

void testNullPoolAllocator()
{
    ResolveWatch watch;
    watch.limit34         = kLimit34;
    watch.sideEnd         = kSideEnd;
    watch.wrapPos         = kWrapPos;
    watch.wrapLimit       = kWrapLimit;
    watch.segment         = kSegment;
    watch.progress        = kProgress;
    const auto defaults   = ModelNullPoolAllocator(watch);
    const auto explicit24 = ModelNullPoolAllocator(watch, 24, 4);
    UNSCOPED_INFO("the default take is the Resolve's 24 bytes");
    REQUIRE((defaults.ok && defaults.end == kForwardStart + 24 && !defaults.wrap && !defaults.waits));
    UNSCOPED_INFO("24 bytes at alignment 4 are the defaults");
    REQUIRE((explicit24.ok == defaults.ok && explicit24.end == defaults.end && explicit24.wrap == defaults.wrap && explicit24.waits == defaults.waits));
    // Clear takes 84 bytes (21 words, g96:12915-12925) and sees prior allocator
    // data.
    const auto strip = ModelNullPoolAllocator(watch, 84, 4);
    UNSCOPED_INFO("an 84 byte take");
    REQUIRE((strip.ok && strip.end == kForwardStart + 84));
    // Alignment 16: the start is rounded up to a multiple of 16.
    watch.poolPos      = 0x00204005;
    const auto aligned = ModelNullPoolAllocator(watch, 84, 16);
    UNSCOPED_INFO("alignment 16");
    REQUIRE((aligned.ok && aligned.end == 0x00204010 + 84));
    const auto four = ModelNullPoolAllocator(watch, 84, 4);
    UNSCOPED_INFO("alignment 4 of the same start");
    REQUIRE((four.ok && four.end == 0x00204008 + 84));
    // A take that does not fit forward wraps with its own size.
    watch.poolPos      = kSideEnd - 40;
    const auto wrapped = ModelNullPoolAllocator(watch, 84, 4);
    UNSCOPED_INFO("an 84 byte wrap");
    REQUIRE((wrapped.ok && wrapped.wrap && wrapped.end == kWrapPos + 84));
    const auto small = ModelNullPoolAllocator(watch, 24, 4);
    UNSCOPED_INFO("a 24 byte take of the same start fits forward");
    REQUIRE((small.ok && !small.wrap && small.end == kSideEnd - 40 + 24));
}

// The tiled device. The fixture clears 1280x720 on a 4 tile table: tiles 0 and 1 split the screen at row 384
// (tile 1's origin offset is y = 380, so its clipped rectangle moves to rows 4..340), tiles 2 and 3 lie off the
// screen. Mode 0 is the 80x16 grid, so every number below is worked by hand from g22:12934-12997.
struct Tiling
{
    std::uint32_t                               tileByte = 0x20, flags2abf = 0, mask = 0xF, bracket = 0x40, predicate = 0;
    std::array<Rect, 4>                         tiles{ Rect{ 0, 0, 1280, 384 }, Rect{ 0, 384, 1280, 720 }, Rect{ 1280, 0, 2560, 384 }, Rect{ 1280, 384, 2560, 720 } };
    std::array<std::array<std::uint32_t, 2>, 4> offsets{ std::array<std::uint32_t, 2>{ 0, 0 }, std::array<std::uint32_t, 2>{ 0, 380 }, std::array<std::uint32_t, 2>{ 1280, 0 }, std::array<std::uint32_t, 2>{ 1280, 380 } };
    std::uint32_t                               count = 4;
};

void applyTiling(Call& call, const Tiling& tiling)
{
    auto& memory = call.memory;
    // The saved slots are the bound surfaces (an unbound slot saves zero), so the device is tiled when bit 0x20 is set.
    for (std::uint32_t i = 0; i < 4; ++i)
        memory[kG + 0x31B8 + 4 * i] = memory[kG + 0x3098 + 4 * i];
    memory[kG + 0x31C8] = memory[kG + 0x30A8];
    memory[kG + 0x2ABC] = (tiling.tileByte << 24) | (call.scene.allocByte << 16) | 0x00000F00 | tiling.flags2abf;
    memory[kG + 0x31A4] = tiling.mask;
    memory[kG + 0x31AC] = tiling.predicate;
    memory[kG + 0x33B0] = tiling.bracket;
    memory[kG + 0x31CC] = tiling.count;
    for (std::uint32_t i = 0; i < 4; ++i)
    {
        for (std::uint32_t j = 0; j < 4; ++j)
            memory[kG + 0x31D0 + 16 * i + 4 * j] = static_cast<std::uint32_t>(tiling.tiles[i][j]);
        memory[kG + 0x32C0 + 8 * i] = tiling.offsets[i][0];
        memory[kG + 0x32C4 + 8 * i] = tiling.offsets[i][1];
    }
}

Scene wideScene()
{
    Scene scene;
    scene.rect = { 0, 0, 1280, 720 };
    return scene;
}

Call tiledCall(const Tiling& tiling = {}, const Scene& scene = wideScene())
{
    Call call(scene);
    applyTiling(call, tiling);
    call.state.tiled = true;
    return call;
}

std::vector<std::uint32_t> takesOf(const ClearPlan& plan)
{
    return std::vector<std::uint32_t>(plan.sides.bytes.begin(), plan.sides.bytes.begin() + static_cast<std::ptrdiff_t>(plan.sides.count));
}

void testTiledClear()
{
    using Bytes = std::vector<std::uint32_t>;
    // The option gates the whole family: the same device is a fallback without it.
    {
        auto call        = tiledCall();
        call.state.tiled = false;
        UNSCOPED_INFO("a tiled device falls back without native_consume_clear_tiled");
        REQUIRE(call.Verdict() == ClearFallback::Tiled);
    }
    // Tile 0 clips to (0,0,1280,384): on the grid, one strip. Tile 1 clips to (0,384,1280,720) and moves by y 380 to
    // rows 4..340: top 4 -> 16 and bottom 340 -> 336 are cut, so two edge strips (168) and the interior (84).
    constexpr std::uint32_t kTakeBytes = 84 + 168 + 84;
    {
        auto       call = tiledCall();
        ClearPlan  plan;
        const auto verdict = call.Verdict(&plan);
        UNSCOPED_INFO("the tiled fixture is admitted, got " + nameOf(verdict));
        REQUIRE(!verdict);
        UNSCOPED_INFO("one call, two tiles in the loop");
        REQUIRE((plan.effects.tiled && plan.effects.tiledLoopCalls == 1 && plan.effects.loopTiles == 2 && plan.effects.untiledCalls == 0 && plan.effects.tiledNoneCalls == 0));
        UNSCOPED_INFO("the takes of the tiles in order, with the origin offset applied");
        REQUIRE(takesOf(plan) == Bytes{ 84, 168, 84 });
        // The fixed pool, take by take from the top; every take lowers both limits.
        UNSCOPED_INFO("the pool after three takes");
        REQUIRE((plan.exit.poolPos == kPoolTop - kTakeBytes && plan.exit.poolUsed == kUsed + kTakeBytes && plan.exit.limit34 == kLimit34 - kTakeBytes && plan.exit.limit38 == kLimit38 - kTakeBytes));
        // A reached 0x826A1800 with the fast path and the color branch: the edge_strips row of the untiled table.
        UNSCOPED_INFO("the dirty words of an allocating call");
        REQUIRE((plan.exit.dirty[0] == 0x038013A0101A0FE8ULL && plan.exit.dirty[1] == 0x0000000880000000ULL && plan.exit.dirty[2] == 0x0000001000080000ULL && plan.exit.allocByte == 0x04));
        // The shape: the tile loop runs and the bracket is set, byte bit 0x20 agrees with it, no predicate.
        UNSCOPED_INFO(std::format("shape {:02X}", plan.exit.shape));
        REQUIRE(plan.exit.shape == (kClearShapeLoop | kClearShapeBracket));
        // The host replay is the untiled one: one observation, with the registers of the call, not of a tile.
        UNSCOPED_INFO("the registers of the derived call");
        REQUIRE((plan.calls.count == 1 && ClearPlanArguments(plan, 0) == std::array<std::uint32_t, 8>{ kG, 0x1, 0, 0, 0, 1280, 720, kColor }));
    }
    // The tile mask selects tiles through the bits (3 << 2i).
    {
        const auto takes = [](std::uint32_t mask)
        {
            Tiling tiling;
            tiling.mask    = mask;
            auto      call = tiledCall(tiling);
            ClearPlan plan;
            UNSCOPED_INFO(std::format("mask {:X} is admitted", mask));
            REQUIRE(!call.Verdict(&plan));
            return takesOf(plan);
        };
        UNSCOPED_INFO("bit 0 selects tile 0");
        REQUIRE(takes(0x1) == Bytes{ 84 });
        UNSCOPED_INFO("bit 1 selects tile 0");
        REQUIRE(takes(0x2) == Bytes{ 84 });
        UNSCOPED_INFO("bit 2 selects tile 1");
        REQUIRE(takes(0x4) == Bytes{ 168, 84 });
        UNSCOPED_INFO("bit 3 selects tile 1");
        REQUIRE(takes(0x8) == Bytes{ 168, 84 });
        UNSCOPED_INFO("the tiles off the screen meet no rectangle");
        REQUIRE(takes(0xF0) == Bytes{});
        UNSCOPED_INFO("a full mask selects only the tiles the rectangle meets");
        REQUIRE(takes(0xFFFFFFFF) == Bytes{ 84, 168, 84 });
    }
    // A loop that selects nothing allocates nothing: the flush and the ORs happen, the pool is untouched.
    {
        Tiling tiling;
        tiling.mask    = 0;
        auto      call = tiledCall(tiling);
        ClearPlan plan;
        UNSCOPED_INFO("a loop with no tile is admitted");
        REQUIRE(!call.Verdict(&plan));
        UNSCOPED_INFO("no tile, no take");
        REQUIRE((plan.effects.tiledNoneCalls == 1 && plan.effects.loopTiles == 0 && plan.sides.count == 0 && !plan.effects.allocates));
        UNSCOPED_INFO("no pool word moves");
        REQUIRE((plan.exit.poolPos == plan.watch.cursor && plan.exit.poolUsed == plan.watch.allocated && plan.exit.limit34 == kLimit34 && plan.exit.limit38 == kLimit38));
        UNSCOPED_INFO("no post-allocation ORs");
        REQUIRE((plan.exit.dirty[0] == 0x008013A0101A07E0ULL && plan.exit.dirty[1] == 0x0000000800000000ULL && plan.exit.dirty[2] == 0x0000000000080000ULL));
    }
    // The origin offsets matter: with tile 1 at its own origin it is on the grid, and an x offset moves it against the grid.
    {
        Tiling tiling;
        tiling.offsets[1] = { 0, 384 };
        auto      call    = tiledCall(tiling);
        ClearPlan plan;
        UNSCOPED_INFO("tile 1 at its own origin is on the grid");
        REQUIRE((!call.Verdict(&plan) && takesOf(plan) == Bytes{ 84, 84 }));
        // x offset 10: left 0 -> -10 aligns up to 0 and right 1280 -> 1270 down to 1200, two edge strips.
        tiling.offsets[1] = { 10, 384 };
        auto      shifted = tiledCall(tiling);
        ClearPlan shiftedPlan;
        UNSCOPED_INFO("admitted");
        REQUIRE(!shifted.Verdict(&shiftedPlan));
        UNSCOPED_INFO("the x offset moves the rectangle against the grid: left and right edges");
        REQUIRE(takesOf(shiftedPlan) == Bytes{ 84, 168, 84 });
    }
    // A rectangle inside the tiles: tile 0 clips to (100,100,1000,384): left 100 -> 160, right 1000 -> 960, top 100 -> 112
    // are cut (three edges, 252), bottom 384 is on the grid. Tile 1 clips to (100,384,1000,500), moved to (100,4,1000,120):
    // four edges (336).
    {
        Scene scene    = wideScene();
        scene.rect     = { 100, 100, 1000, 500 };
        auto      call = tiledCall({}, scene);
        ClearPlan plan;
        UNSCOPED_INFO("admitted");
        REQUIRE(!call.Verdict(&plan));
        UNSCOPED_INFO("edges and interiors of two clipped tiles");
        REQUIRE(takesOf(plan) == Bytes{ 252, 84, 336, 84 });
    }
    // Two color targets: every call runs the whole loop, the takes in call order.
    {
        Scene scene    = wideScene();
        scene.flags    = 0x3;
        scene.rt       = { true, true, false, false };
        auto      call = tiledCall({}, scene);
        ClearPlan plan;
        UNSCOPED_INFO("two calls admitted");
        REQUIRE(!call.Verdict(&plan));
        UNSCOPED_INFO("two calls, four tile visits");
        REQUIRE((plan.calls.count == 2 && plan.effects.tiledLoopCalls == 2 && plan.effects.loopTiles == 4));
        UNSCOPED_INFO("the loop of the second call follows the first");
        REQUIRE(takesOf(plan) == Bytes{ 84, 168, 84, 84, 168, 84 });
    }
    // The tile loop's ring words: 241 for the untiled fixture; here n = 4, one call, three takes and two tiles.
    constexpr std::uint32_t kWords = 52 + 63 + (12 + 52) + 3 * 22 + 2 * 2 + 40;
    UNSCOPED_INFO("the hand count");
    REQUIRE(kWords == 289);
    {
        const auto withCursor = [&](std::uint32_t cursor)
        {
            auto call              = tiledCall();
            call.memory[kG + 0x30] = cursor;
            return call;
        };
        ClearPlan plan;
        UNSCOPED_INFO("the model counts the same words");
        REQUIRE((!tiledCall().Verdict(&plan) && ClearMaxRingWords(plan.effects, plan.sides, plan.watch) == kWords));
        // The limits after the pool lowered them by the three takes.
        const std::uint32_t limit = kLimit38 - kTakeBytes;
        UNSCOPED_INFO("the words end exactly at the soft limit");
        REQUIRE(!withCursor(limit - 4 * kWords).Verdict());
        UNSCOPED_INFO("one word past it");
        REQUIRE(withCursor(limit - 4 * kWords + 4).Verdict() == ClearFallback::RingSpace);
    }
    // The bracket drops the pool threshold flush (g93:12840-12841), so a count past the threshold is no reason; the
    // same device with the bracket clear (byte bit 0x20 still set) is.
    {
        auto call                = tiledCall();
        call.memory[kG + 0x3A54] = kThreshold - kTakeBytes;
        UNSCOPED_INFO("the count reaches the threshold");
        REQUIRE(!call.Verdict());
        call.memory[kG + 0x3A54] += 1;
        ClearPlan plan;
        UNSCOPED_INFO("the bracket lets the count pass the threshold");
        REQUIRE((!call.Verdict(&plan) && plan.exit.poolUsed == kThreshold + 1));
        Tiling open;
        open.bracket               = 0;
        auto closed                = tiledCall(open);
        closed.memory[kG + 0x3A54] = kThreshold - kTakeBytes + 1;
        UNSCOPED_INFO("without the bracket the count passing the threshold flushes");
        REQUIRE(closed.Verdict() == ClearFallback::RingSpace);
    }
    // The shapes of the tiling words: the model follows the words, so each is admitted with the shape it is counted under.
    {
        const auto shapeOf = [](const Tiling& tiling)
        {
            auto       call = tiledCall(tiling);
            ClearPlan  plan;
            const auto verdict = call.Verdict(&plan);
            UNSCOPED_INFO("admitted, got " + nameOf(verdict));
            REQUIRE(!verdict);
            return plan.exit.shape;
        };
        Tiling tiling;
        tiling.predicate = 1;
        UNSCOPED_INFO("the predicate is counted, and does not change the exit state");
        REQUIRE(shapeOf(tiling) == (kClearShapeLoop | kClearShapeBracket | kClearShapePredicate));
        {
            auto      withPredicate = tiledCall(tiling);
            auto      without       = tiledCall();
            ClearPlan a, b;
            UNSCOPED_INFO("admitted");
            REQUIRE((!withPredicate.Verdict(&a) && !without.Verdict(&b)));
            UNSCOPED_INFO("the predicate changes no word");
            REQUIRE((a.exit.dirty == b.exit.dirty && a.exit.poolPos == b.exit.poolPos && a.exit.limit34 == b.exit.limit34 && a.exit.poolUsed == b.exit.poolUsed && takesOf(a) == takesOf(b)));
        }
        tiling         = {};
        tiling.bracket = 0;
        UNSCOPED_INFO("byte bit 0x20 without the bracket: the loop runs, the state disagrees");
        REQUIRE(shapeOf(tiling) == (kClearShapeLoop | kClearShapeState));
        tiling          = {};
        tiling.tileByte = 0x10;
        tiling.bracket  = 0;
        UNSCOPED_INFO("byte bit 0x10");
        REQUIRE(shapeOf(tiling) == (kClearShapeLoop | kClearShapeByte10));
        tiling.bracket = 0x40;
        UNSCOPED_INFO("byte bit 0x10 with the bracket");
        REQUIRE(shapeOf(tiling) == (kClearShapeLoop | kClearShapeBracket | kClearShapeByte10 | kClearShapeState));
        // The bracket on an untiled device (the saved slot differs): one untiled strip call, with the bracket terms.
        auto call                = tiledCall();
        call.memory[kG + 0x31B8] = kSurface + 0x900;
        ClearPlan plan;
        UNSCOPED_INFO("the bracket alone is admitted");
        REQUIRE(!call.Verdict(&plan));
        UNSCOPED_INFO("an untiled call with the bracket set");
        REQUIRE((plan.effects.untiledCalls == 1 && !plan.effects.tiled && takesOf(plan) == Bytes{ 84 }));
        UNSCOPED_INFO(std::format("shape {:02X}", plan.exit.shape));
        REQUIRE(plan.exit.shape == kClearShapeBracket);
        // An untiled device, a byte 0x20 and no bracket, is not a tiled shape at all.
        tiling                   = {};
        tiling.bracket           = 0;
        auto open                = tiledCall(tiling);
        open.memory[kG + 0x31B8] = kSurface + 0x900;
        ClearPlan openPlan;
        UNSCOPED_INFO("no loop and no bracket");
        REQUIRE((!open.Verdict(&openPlan) && !ClearShapeTiled(openPlan.exit.shape)));
    }
    // The null pool allocator's bracket terms (g185:12955-13124): with G+0x33B4 - G+0x3A48 = 0 the take fails when the
    // bracket word is below its end, and with -1 it fails at once; two segments ahead it passes like an unbracketed take.
    {
        const auto nullTiled = [](std::uint32_t next)
        {
            auto call                = tiledCall();
            call.memory[kG + 0x34CC] = 0;
            call.memory[kG + 0x33B4] = next;
            return call;
        };
        UNSCOPED_INFO("a difference of 0 with the bracket word below the end fails");
        REQUIRE(nullTiled(kSegment).Verdict() == ClearFallback::AllocCapacity);
        UNSCOPED_INFO("a difference of -1 fails");
        REQUIRE(nullTiled(kSegment - 1).Verdict() == ClearFallback::AllocCapacity);
        ClearPlan plan;
        auto      passing = nullTiled(kSegment + 2);
        UNSCOPED_INFO("a difference of 2 passes");
        REQUIRE(!passing.Verdict(&plan));
        UNSCOPED_INFO("the takes");
        REQUIRE(takesOf(plan) == Bytes{ 84, 168, 84 });
        // The first two takes run forward from G+0x34 + 60 and end at `second`; the third would end past the area end
        // 0x204100, so it wraps and ends at G+0x3A4C + 84.
        const std::uint32_t second = kForwardStart + 84 + 168;
        UNSCOPED_INFO("forward, forward, then wrap");
        REQUIRE((second + 84 > kSideEnd && plan.exit.poolPos == second && plan.exit.wrapPos == kWrapPos + 84));
        UNSCOPED_INFO("the null pool moves neither limit");
        REQUIRE((plan.exit.limit34 == kLimit34 && plan.exit.limit38 == kLimit38 && plan.exit.poolUsed == kUsed + kTakeBytes));
        // The bracket also skips the forced flush past the threshold under a null pool: the count may pass it.
        passing.memory[kG + 0x3A54] = kThreshold - kTakeBytes + 1;
        ClearPlan past;
        UNSCOPED_INFO("the bracket lets the null pool count pass the threshold");
        REQUIRE((!passing.Verdict(&past) && past.exit.poolUsed == kThreshold + 1));
        // An untiled null pool call (one 84 byte take, no bracket) passing the threshold is the forced flush.
        auto open                = nullPoolCall(wideScene());
        open.memory[kG + 0x3A54] = kThreshold - 84 + 1;
        UNSCOPED_INFO("an untiled null pool call past the threshold flushes");
        REQUIRE(open.Verdict() == ClearFallback::RingSpace);
    }
    // The tile block A is not modeled: byte 0x40, G+0x2ABF bit 0x20 and a mask bit of 0x15555555 reach it.
    {
        Tiling tiling;
        tiling.tileByte  = 0x60;
        tiling.flags2abf = 0x20;
        auto call        = tiledCall(tiling);
        UNSCOPED_INFO("block A falls back as tiled_block");
        REQUIRE(call.Verdict() == ClearFallback::TiledBlock);
        // It is judged before the allocation marker, and after the early exits.
        Scene marker     = wideScene();
        marker.allocByte = 0x20;
        UNSCOPED_INFO("block A before the allocation marker");
        REQUIRE(tiledCall(tiling, marker).Verdict() == ClearFallback::TiledBlock);
        auto early               = tiledCall(tiling);
        early.memory[kRect + 12] = 0;
        UNSCOPED_INFO("an early exit before block A");
        REQUIRE(early.Verdict() == ClearFallback::EarlyExit);
        // Without the option the device is simply tiled.
        call.state.tiled = false;
        UNSCOPED_INFO("tiled without the option");
        REQUIRE(call.Verdict() == ClearFallback::Tiled);
        // Each of its three conditions.
        Tiling no = tiling;
        no.mask   = 0xA;
        UNSCOPED_INFO("mask bits outside 0x15555555 skip block A");
        REQUIRE(!tiledCall(no).Verdict());
        no           = tiling;
        no.flags2abf = 0;
        UNSCOPED_INFO("without G+0x2ABF bit 0x20");
        REQUIRE(!tiledCall(no).Verdict());
        no          = tiling;
        no.tileByte = 0x20;
        UNSCOPED_INFO("without tiling byte bit 0x40");
        REQUIRE(!tiledCall(no).Verdict());
        // The exit state refuses it with its own reason.
        ClearPlan filled;
        auto      other = tiledCall();
        UNSCOPED_INFO("admitted");
        REQUIRE(!other.Verdict(&filled));
        auto effects            = filled.effects;
        effects.tiledBlockCalls = 1;
        ClearExitRefusal reason = ClearExitRefusal::None;
        UNSCOPED_INFO("the exit state refuses block A");
        REQUIRE((!DeriveClearExitState(effects, filled.sides, filled.watch, &reason) && reason == ClearExitRefusal::TiledBlock));
    }
    // Unreadable and unsupported tile words.
    {
        for (const std::uint32_t address : { kG + 0x31D0 + 16 + 4, kG + 0x31A4, kG + 0x32C0 + 8, kG + 0x32C4 + 8 })
        {
            auto call = tiledCall();
            call.memory.erase(address);
            UNSCOPED_INFO(std::format("{:08X} unreadable", address));
            REQUIRE(call.Verdict() == ClearFallback::Unreadable);
        }
        // The offsets of a tile that is not selected are not read.
        auto unselected = tiledCall();
        unselected.memory.erase(kG + 0x32C0 + 16);
        UNSCOPED_INFO("the offsets of an unselected tile are not read");
        REQUIRE(!unselected.Verdict());
        Tiling tiling;
        tiling.count = 33;
        UNSCOPED_INFO("a tile table beyond 32");
        REQUIRE(tiledCall(tiling).Verdict() == ClearFallback::DeriveUnsupported);
        tiling.count = 17;
        auto many    = tiledCall(tiling);
        for (std::uint32_t i = 4; i < 17; ++i)
            for (std::uint32_t j = 0; j < 4; ++j)
                many.memory[kG + 0x31D0 + 16 * i + 4 * j] = 0;
        UNSCOPED_INFO("17 tiles pass the loops' bound");
        REQUIRE(many.Verdict() == ClearFallback::RingSpace);
    }
}

} // namespace

TEST_CASE("clear_consume", "[ctest][clear-consume][timeout-60]")
{
    testAdmitted();
    testExitState();
    testWrites();
    testEachReason();
    testOrder();
    testStackOverlap();
    testTiledConditions();
    testRingAndAllocator();
    testEarlyExits();
    testSideBuffers();
    testSideBufferErrors();
    testDeviceTiled();
    testPlanArguments();
#if REREVVED_DEVELOPER
    testExitMismatches();
#endif
    testExitRefusals();
    testNullPool();
    testNullPoolAllocator();
    testTiledClear();
    testProbeStackPlaceholder();
#if REREVVED_DEVELOPER
    testCounts();
#endif
    testEntry();
}
