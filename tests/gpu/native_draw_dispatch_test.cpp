#include "callback_checks.hpp"
#include "developer_checks.hpp"
#include "gpu/completion/completion_ledger.h"
#include "gpu/d3d12/native_texture_upload.h"
#include "gpu/guest_d3d/indexed_buffer_consume.h"
#include "gpu/guest_d3d/lowerer_consume.h"
#include "gpu/guest_d3d/native_draw_dispatch.h"
#include "gpu/guest_d3d/resolve_surface_hooks.h"
#include "gpu/guest_d3d/ring_stubs.h"
#include "gpu/guest_d3d/user_pointer_consume.h"
#include "guest_import_doubles.hpp"

#if REREVVED_DEVELOPER
#include "directory_removal.hpp"
#include "gpu/diagnostics/label_draw_capture.h"
#include "gpu/diagnostics/route_trace.h"
#include "gpu/diagnostics/scene_resource_capture.h"
#include "gpu/diagnostics/shader_harvest.h"
#include "gpu/guest_d3d/draw_effect_probe.h"
#include "production_observers.hpp"
#endif

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DECLARE(bool, native_consume_indexed_buffer);
REXCVAR_DECLARE(bool, native_consume_indexed_buffer_tiled);
REXCVAR_DECLARE(bool, native_consume_indexed_buffer_dirty);
REXCVAR_DECLARE(bool, native_consume_user_pointer);
REXCVAR_DECLARE(bool, native_consume_user_pointer_tiled);
REXCVAR_DECLARE(bool, native_consume_user_pointer_dirty);
REXCVAR_DECLARE(bool, native_consume_dirty_vertex_fetch);
REXCVAR_DECLARE(bool, native_consume_lowerer);
REXCVAR_DECLARE(bool, native_consume_lowerer_tiled);
DEV_CVAR_DECLARE(bool, native_lowerer_exit_check);

#define ORIGINAL_New(address) REX_EXTERN(sub_##address);
#define ORIGINAL_Existing(address)
#define ROUTE_ENTRY(address, groups, mode) ORIGINAL_##mode(address)
#include "allocation_harness.hpp"
#include "gpu/guest_d3d/route_entries.inc"
#include "manual_cases.hpp"
#undef ROUTE_ENTRY
#undef ORIGINAL_Existing
#undef ORIGINAL_New

REX_EXTERN(sub_826A3568);
REX_EXTERN(sub_826A3000);
REX_EXTERN(sub_826A39F8);
REX_EXTERN(sub_826A2FF0);

using namespace rerevved::gpu::guest_d3d;
using namespace std::chrono_literals;

// The label adapter capture stays disabled unless a case claims it. A claimed
// adapter is a non-null token that owns nothing and is never dereferenced.
namespace
{

#if REREVVED_DEVELOPER
std::atomic<bool> claimLabelAdapter{ false };
char              adapterToken = 0;

template <class Capture>
std::shared_ptr<Capture> claimedAdapter()
{
    return std::shared_ptr<Capture>(std::shared_ptr<void>(), reinterpret_cast<Capture*>(&adapterToken));
}
#endif

constexpr std::size_t   kGuestBytes = 0x8000;
constexpr std::uint32_t kGraphics   = 0x1000;
constexpr std::uint32_t kStack      = 0x7000;
// The thread's stack: the KPCR at r13 holds the base at +0x70 and the limit at +0x74.
constexpr std::uint32_t kPcr       = 0x7A00;
constexpr std::uint32_t kLowerFail = 0x8007000E;

// Consume tests save and restore all six cvars; dirty vertex fetch starts off.
struct DrawConsumeCvars
{
    explicit DrawConsumeCvars(bool value = false)
    : saved{ REXCVAR_GET(native_consume_indexed_buffer),
             REXCVAR_GET(native_consume_indexed_buffer_tiled),
             REXCVAR_GET(native_consume_indexed_buffer_dirty),
             REXCVAR_GET(native_consume_user_pointer),
             REXCVAR_GET(native_consume_user_pointer_tiled),
             REXCVAR_GET(native_consume_user_pointer_dirty) }
    {
        set(value);
        REXCVAR_SET(native_consume_dirty_vertex_fetch, false);
    }

    ~DrawConsumeCvars()
    {
        REXCVAR_SET(native_consume_indexed_buffer, saved[0]);
        REXCVAR_SET(native_consume_indexed_buffer_tiled, saved[1]);
        REXCVAR_SET(native_consume_indexed_buffer_dirty, saved[2]);
        REXCVAR_SET(native_consume_user_pointer, saved[3]);
        REXCVAR_SET(native_consume_user_pointer_tiled, saved[4]);
        REXCVAR_SET(native_consume_user_pointer_dirty, saved[5]);
        REXCVAR_SET(native_consume_dirty_vertex_fetch, savedVertexFetch);
    }

    static void set(bool value)
    {
        REXCVAR_SET(native_consume_indexed_buffer, value);
        REXCVAR_SET(native_consume_indexed_buffer_tiled, value);
        REXCVAR_SET(native_consume_indexed_buffer_dirty, value);
        REXCVAR_SET(native_consume_user_pointer, value);
        REXCVAR_SET(native_consume_user_pointer_tiled, value);
        REXCVAR_SET(native_consume_user_pointer_dirty, value);
    }

    std::array<bool, 6> saved;
    bool                savedVertexFetch = REXCVAR_GET(native_consume_dirty_vertex_fetch);
};

// Fixture Original behavior. Outer stubs call the hooked child symbol on a
// deeper stack frame, as generated guest code does, so nesting runs through
// the real wrappers. The capture shows 0xB0 and 0x80 byte outer frames.
std::atomic<bool>                         lowererFails{ false }, sinkFails{ false }, originalThrows{ false };
std::atomic<std::uint64_t>                originalCalls{ 0 };
thread_local int                          outerChildren     = 1;
thread_local bool                         boundNests        = false;
thread_local std::uint32_t                replayChildCaller = 0; // nonzero: the captured child LR
thread_local std::barrier<>*              childBarrier      = nullptr;
std::atomic<std::uint64_t>                bufferOriginals{ 0 }; // Original runs of 0x826A39F8
std::array<std::atomic<std::uint64_t>, 2> wrapperOriginals{};   // Original runs of 0x826A3568 and 0x826A3000
thread_local bool                         nestBuffer   = false; // the IndexedUp fixture calls 0x826A39F8 instead of its lowerer
thread_local int                          nestUp       = 0;     // the 0x826A39F8 fixture calls 0x826A3568 (1) or 0x826A3000 (2)
std::latch*                               outerEntered = nullptr;
std::latch*                               outerRelease = nullptr;

std::uint32_t load(const std::uint8_t* base, std::uint32_t address)
{
    return std::uint32_t(base[address]) << 24 | std::uint32_t(base[address + 1]) << 16 |
           std::uint32_t(base[address + 2]) << 8 | base[address + 3];
}

void store(std::uint8_t* base, std::uint32_t address, std::uint32_t value)
{
    base[address]     = std::uint8_t(value >> 24);
    base[address + 1] = std::uint8_t(value >> 16);
    base[address + 2] = std::uint8_t(value >> 8);
    base[address + 3] = std::uint8_t(value);
}

// Every guest word the dispatcher and the consumes read or write through the fixture.
std::atomic<std::uint64_t> fixtureReads{ 0 };
std::atomic<std::uint64_t> fixtureWrites{ 0 };

// A test's choice of the thread's stack limit for the next root; by default it is 0x400 below r1.
thread_local std::optional<std::uint32_t> stackLimit;

// A test's choice of r13 for the next root (the KPCR the stack bounds are read from); by default the fixture's KPCR.
thread_local std::optional<std::uint32_t> pcrRegister;

// The KPCR words of a thread whose r1 is `r1`: the base is above r1, the limit the test's or r1 - 0x400.
void placeStack(std::uint8_t* memory, std::uint32_t r1)
{
    store(memory, kPcr + 0x70, r1 + 0x100);
    store(memory, kPcr + 0x74, stackLimit.value_or(r1 > 0x400 ? r1 - 0x400 : 1));
}

bool readFixture(const std::uint8_t* base, std::uint32_t address, std::uint32_t& value)
{
    fixtureReads.fetch_add(1, std::memory_order_relaxed);
    if (!base || address > kGuestBytes - 4)
        return false;
    value = load(base, address);
    return true;
}

bool writeFixture(const std::uint8_t* base, std::uint32_t address, std::uint32_t value)
{
    fixtureWrites.fetch_add(1, std::memory_order_relaxed);
    if (!base || address > kGuestBytes - 4)
        return false;
    store(const_cast<std::uint8_t*>(base), address, value);
    return true;
}

bool writableFixture(const std::uint8_t* base, std::uint32_t address, std::uint32_t bytes)
{
    return base && address >= 0x100 && std::uint64_t{ address } + bytes <= kGuestBytes;
}

// Dirty groups, pending cursor, allocation outputs and accounting beyond the 0x3500 shadow.
void lowerer(PPCContext& ctx, std::uint8_t* base)
{
    if (childBarrier)
        childBarrier->arrive_and_wait();
    if (originalThrows)
        throw std::runtime_error("fixture Original unwind");
    const auto g = ctx.r3.u32;
    store(base, g + 0x3A54, load(base, g + 0x3A54) + 1);
    if (lowererFails)
    {
        store(base, g + 0x30, 0x5000);
        ctx.r3.u64 = kLowerFail;
        return;
    }
    store(base, g + 0x18, load(base, g + 0x18) | 1);
    store(base, g + 0x3484, 0x5100);
    store(base, g + 0x3488, 0x6000);
    store(base, g + 0x348C, 0x6800);
    store(base, g + 0x3490, ctx.r6.u32 * ctx.r9.u32 / 4);
    store(base, g + 0x3494, (ctx.r7.u32 + 1) / 2);
    // Through the vertex out slot at [r1+84] and the index out slot r10.
    if (const auto slot = load(base, ctx.r1.u32 + 84))
        store(base, slot, 0x6000);
    store(base, ctx.r10.u32, 0x6800);
    ctx.r3.u64 = 0;
}

std::atomic<bool> exactLowerer{ false }, lowererDeviates{ false };

// The fixture Original cannot take a path it does not emulate; a test that reaches one is wrong.
[[noreturn]] void fixtureStops(const char* message)
{
    throw std::logic_error(std::string("fixture Original: ") + message);
}

// Fixed pool 0x826A6848: g100:13237-13311.
std::uint32_t exactFixedTake(std::uint8_t* base, std::uint32_t g, std::uint32_t size, std::uint32_t align)
{
    const auto pool = load(base, g + 0x34CC);
    auto       top  = load(base, g + 0x3A44);
    if (!top)
        top = (load(base, pool + 156) & 0xFFFFFFFC) + load(base, pool + 152);
    const auto buffer = (top - (size & 0xFFFFFFFC)) & ~(align - 1);
    if (buffer < load(base, g + 0x30) + 164)
        return 0;
    const auto delta = (top - buffer) & 0xFFFFFFFC;
    store(base, g + 0x3A44, buffer);
    store(base, g + 0x34, load(base, g + 0x34) - delta);
    store(base, g + 0x38, load(base, g + 0x38) - delta);
    return buffer;
}

// Null pool 0x8269B618: g185:12921-13040.
std::uint32_t exactNullTake(std::uint8_t* base, std::uint32_t g, std::uint32_t size, std::uint32_t align)
{
    auto start = load(base, g + 0x3A44);
    if (!start)
        start = load(base, g + 0x34) + 60;
    const auto segment = load(base, g + 0x3A48), sideEnd = load(base, g + 0x3A3C), bracket = load(base, g + 0x33B0), next = load(base, g + 0x33B4);
    auto       buffer = (start + align - 1) & ~(align - 1);
    auto       end    = buffer + size;
    auto       seg    = segment;
    if (end <= sideEnd)
    {
        if (bracket)
        {
            const auto difference = next - segment;
            if (difference == 0xFFFFFFFF || (difference == 0 && bracket < end))
                return 0;
        }
        store(base, g + 0x3A44, end);
    }
    else
    {
        buffer = (load(base, g + 0x3A4C) + align - 1) & ~(align - 1);
        end    = buffer + size;
        if (bracket)
        {
            const auto difference = next - segment - 1;
            if (difference == 0xFFFFFFFF || (difference == 0 && bracket < end))
                return 0;
        }
        if (end > load(base, g + 0x3A50))
            return 0;
        seg += 1;
        store(base, g + 0x3A4C, end);
    }
    // 0x8269B158 returns at once when the GPU's segment is this one, or one behind with the buffer inside its block.
    const auto progress = load(base, load(base, g + 0x2A90) + 4);
    const auto ahead    = (seg - progress) & 3;
    if (ahead != 0 && !(ahead == 1 && end <= (progress & 0xFFFFFFFC)))
        fixtureStops("the fixture allocator would wait");
    return buffer;
}

// Side-buffer allocator 0x8269B818: g93:12757-12888.
std::uint32_t exactAllocate(std::uint8_t* base, std::uint32_t g, std::uint32_t dwords)
{
    if (base[g + 0x2ABD] & 0x20)
        return 0;
    const auto    size   = dwords << 2;
    const auto    pool   = load(base, g + 0x34CC);
    std::uint32_t buffer = 0;
    if (!pool)
        buffer = exactNullTake(base, g, size, 16);
    else if (load(base, pool + 152))
        buffer = exactFixedTake(base, g, size, 16);
    else
        fixtureStops("the fixture has no indirect allocator");
    const auto used = load(base, g + 0x3A54) + size;
    store(base, g + 0x3A54, used);
    if (used > load(base, g + 0x3A58) && load(base, g + 0x33B0) == 0)
    {
        const auto cursor = load(base, g + 0x30);
        store(base, g + 0x34, cursor + 160);
        store(base, g + 0x38, cursor);
        if (load(base, g + 0x3A4C) != load(base, g + 0x3A38))
            store(base, g + 0x3A44, load(base, g + 0x3A3C));
    }
    if (!buffer)
        base[g + 0x2ABD] |= 0x20;
    return buffer;
}

// Direct lowerer 0x826A3048: g63:13039-13816.
void exactLowererOriginal(PPCContext& ctx, std::uint8_t* base)
{
    const auto g = ctx.r3.u32;
    // g63:13055-13081
    const auto product      = std::uint32_t(std::int64_t(ctx.r6.s32) * std::int64_t(ctx.r9.s32));
    const auto vertexDwords = product >> 2;
    const auto indexDwords  = (ctx.r8.u32 & 4) ? ctx.r7.u32 : (ctx.r7.u32 + 1) >> 1;
    // g63:13084-13106: the stride byte of the call against the recorded one.
    const auto strideByte = std::uint8_t(ctx.r9.u32 >> 2);
    if (strideByte != base[g + 0x2E38] && load(base, g + 0x3198))
        store(base, g + 0x14, load(base, g + 0x14) | 0x80000);
    // 0x826ADD88's words are not emulated; the call is only judged by what it leaves.
    if (load(base, g + 0x14) & 0x1E0000)
        store(base, g + 0x14, load(base, g + 0x14) & ~0x1E0000U);
    // Of the flush groups only G+0x18 bit 0 is emulated (0x826ACEA0, g92:13099-13297), the one a previous draw leaves.
    for (const auto offset : { 0x00U, 0x04U, 0x08U, 0x0CU, 0x10U, 0x14U, 0x18U, 0x20U, 0x24U })
        if (load(base, g + offset))
            fixtureStops("the fixture does not emulate these flush groups");
    auto cursor = load(base, g + 0x30);
    if (const auto low = load(base, g + 0x1C))
    {
        if (low != 1)
            fixtureStops("the fixture does not emulate these flush groups");
        const unsigned lead = (cursor & 7) ? 2 : 1;
        cursor += 4 * (lead + 6 + 4);
        store(base, g + 0x1C, 0);
    }
    if (cursor > load(base, g + 0x38))
        fixtureStops("the fixture does not emulate the soft limit flush");
    store(base, g + 0x30, cursor);
    const auto vertex = exactAllocate(base, g, vertexDwords);
    const auto index  = exactAllocate(base, g, indexDwords);
    const auto slot   = load(base, ctx.r1.u32 + 84);
    if (!vertex || !index)
    {
        // g63:13797-13809
        store(base, g + 0x30, cursor);
        store(base, ctx.r10.u32, 0);
        store(base, slot, 0);
        ctx.r3.u64 = kLowerFail;
        return;
    }
    const bool tiled = base[g + 0x2ABC] & 1;
    // The ring words: 13 common, 5 more untiled, 14 more with the tile record (g63:13460-13769).
    const unsigned words = tiled ? 27 : 18;
    if (tiled)
    {
        const auto list = load(base, g + 0x33C8);
        if (list + 16 > load(base, g + 0x33CC))
            fixtureStops("the fixture does not grow the tile list");
        store(base, list, cursor + 4 * 13);
        store(base, g + 0x33C8, list + 16);
    }
    for (unsigned i = 0; i < words; ++i)
        store(base, cursor + 4 * (i + 1), 0xC0DE0000 + i);
    cursor += 4 * words;
    // g63:13772-13795
    store(base, g + 0x1C, load(base, g + 0x1C) | 1);
    store(base, g + 0x3484, cursor);
    store(base, g + 0x3488, vertex);
    store(base, g + 0x348C, index);
    store(base, g + 0x3490, vertexDwords);
    store(base, g + 0x3494, indexDwords);
    store(base, slot, vertex);
    store(base, ctx.r10.u32, index);
    if (lowererDeviates)
        store(base, g + 0x3A54, load(base, g + 0x3A54) + 4);
    ctx.r3.u64 = 0;
}

void sink(PPCContext& ctx, std::uint8_t* base)
{
    if (childBarrier)
        childBarrier->arrive_and_wait();
    const auto g = ctx.r3.u32;
    if (sinkFails)
    {
        store(base, g + 0x30, 0x5200);
        ctx.r3.u64 = 0;
        return;
    }
    store(base, g + 0x18, load(base, g + 0x18) | 1);
    store(base, g + 0x3484, 0x5300);
    store(base, g + 0x3488, 0x7000);
    ctx.r3.u64 = 0x7000;
}

void packets(PPCContext& ctx, std::uint8_t* base)
{
    const auto g = ctx.r3.u32;
    // A device outside the fixture's memory (a wrapping one) has nothing for the double to write.
    if (g > kGuestBytes - 0x40)
    {
        ctx.r3.u64 = 0x12345678;
        return;
    }
    store(base, g + 0x08, load(base, g + 0x08) | 2);
    store(base, g + 0x30, load(base, g + 0x30) + 16);
    ctx.r3.u64 = 0x12345678;
}

void original(std::uint32_t address, PPCContext& ctx, std::uint8_t* base)
{
    ++originalCalls;
    switch (address)
    {
        case 0x826A3048:
            if (exactLowerer)
                exactLowererOriginal(ctx, base);
            else
                lowerer(ctx, base);
            break;
        case 0x826A2B58:
            sink(ctx, base);
            break;
        case 0x826A3608:
            packets(ctx, base);
            break;
        default:
            ++ctx.r3.u64;
            break;
    }
}

// 0x826A2FF0: publishes the pending draw, G+0x30 = G+0x3484. The commit hook
// under test is the production one.
void fixtureSub826A2FF0(std::uint32_t, PPCContext& ctx, std::uint8_t* base)
{
    const auto g = ctx.r3.u32;
    store(base, g + 0x30, load(base, g + 0x3484));
}

// 0x826A3568: lowerer at 0x826A35AC; on zero copies and publishes G+0x3484 into G+0x30.
void fixtureSub826A3568(std::uint32_t, PPCContext& ctx, std::uint8_t* base)
{
    ++wrapperOriginals[0];
    if (outerEntered)
    {
        outerEntered->count_down();
        outerRelease->wait();
    }
    if (nestBuffer)
    {
        const auto lr = static_cast<std::uint32_t>(ctx.lr);
        ctx.r1.u64 -= 0xB0;
        ctx.lr = 0x826A3D00;
        sub_826A39F8(ctx, base);
        ctx.r1.u64 += 0xB0;
        ctx.lr = lr;
        return;
    }
    const auto g = ctx.r3.u32, lr = static_cast<std::uint32_t>(ctx.lr);
    ctx.r1.u64 -= 0xB0;
    for (int i = 0; i < outerChildren; ++i)
    {
        ctx.r3.u64 = g;
        ctx.lr     = replayChildCaller ? replayChildCaller : kIndexedUpLowererReturn;
        sub_826A3048(ctx, base);
    }
    if (outerChildren && ctx.r3.u32 == 0)
    {
        store(base, load(base, g + 0x3488), 0xCAFEF00D);
        store(base, load(base, g + 0x348C), 0x00010002);
        store(base, g + 0x30, load(base, g + 0x3484));
    }
    ctx.r1.u64 += 0xB0;
    ctx.lr = lr;
}

// 0x826A3000: sink at 0x826A3020; a null allocation skips the copy.
void fixtureSub826A3000(std::uint32_t, PPCContext& ctx, std::uint8_t* base)
{
    ++wrapperOriginals[1];
    const auto g = ctx.r3.u32, lr = static_cast<std::uint32_t>(ctx.lr);
    ctx.r1.u64 -= 0x80;
    for (int i = 0; i < outerChildren; ++i)
    {
        ctx.r3.u64 = g;
        ctx.lr     = replayChildCaller ? replayChildCaller : kNonindexedSinkReturn;
        sub_826A2B58(ctx, base);
    }
    if (const auto destination = ctx.r3.u32; outerChildren && destination)
    {
        store(base, destination, 0xFEEDFACE);
        store(base, g + 0x30, load(base, g + 0x3484));
        ctx.r3.u64 = destination + 4;
    }
    ctx.r1.u64 += 0x80;
    ctx.lr = lr;
}

void fixtureSub826A39F8(std::uint32_t, PPCContext& ctx, std::uint8_t* base)
{
    ++bufferOriginals;
    if (boundNests)
    {
        const auto g = ctx.r3.u32, lr = static_cast<std::uint32_t>(ctx.lr);
        ctx.r1.u64 -= 0x40;
        ctx.lr = 0x826A3D00;
        sub_826A3048(ctx, base);
        ctx.r1.u64 += 0x40;
        ctx.r3.u64 = g;
        ctx.lr     = lr;
    }
    if (nestUp)
    {
        const auto g = ctx.r3.u32, lr = static_cast<std::uint32_t>(ctx.lr);
        ctx.r1.u64 -= 0x40;
        ctx.lr = 0x826A3D00;
        if (nestUp == 1)
            sub_826A3568(ctx, base);
        else
            sub_826A3000(ctx, base);
        ctx.r1.u64 += 0x40;
        ctx.r3.u64 = g;
        ctx.lr     = lr;
    }
    packets(ctx, base);
}

void fixtureSub826AD150(std::uint32_t, PPCContext& ctx, std::uint8_t* base)
{
    ++ctx.r3.u64;
}

void fixtureSub826A7040(std::uint32_t, PPCContext& ctx, std::uint8_t* base)
{
    ++ctx.r3.u64;
}

void fixtureSub826A7138(std::uint32_t, PPCContext& ctx, std::uint8_t* base)
{
    ++ctx.r3.u64;
}

using Hook = void (*)(PPCContext&, std::uint8_t*);

constexpr std::array<Hook, kNativeDrawHookCount> kWrappers{ sub_826A39F8, sub_826A3568, sub_826A3000, sub_826A3048, sub_826A2B58, sub_826A3608 };

Hook wrapper(NativeDrawHook hook)
{
    return kWrappers[static_cast<std::size_t>(hook)];
}

// The dispatcher this file installed through install() and has not withdrawn.
const NativeDrawDispatcher* installedByTest = nullptr;

void install(NativeDrawDispatcher& dispatcher)
{
    UNSCOPED_INFO("only one dispatcher is installed at a time");
    REQUIRE(InstallNativeDrawDispatcher(dispatcher));
    installedByTest = &dispatcher;
}

void remove()
{
    UNSCOPED_INFO("an idle dispatcher is withdrawn promptly");
    REQUIRE(RemoveNativeDrawDispatcher(1s));
    installedByTest = nullptr;
}

// A dispatcher that withdraws itself before it is destroyed, so a failing
// REQUIRE cannot leave the draw hooks pointing at a dead dispatcher.
class ScopedDispatcher : public NativeDrawDispatcher
{
public:
    using NativeDrawDispatcher::NativeDrawDispatcher;

    ~ScopedDispatcher()
    {
        if (installedByTest != static_cast<const NativeDrawDispatcher*>(this))
            return;
        installedByTest = nullptr;
        // Best effort: a hook still running on another thread is waited for.
        (void)RemoveNativeDrawDispatcher(10s);
    }
};

std::vector<std::uint8_t> guestMemory(std::uint32_t vs = 0x2000, std::uint32_t ps = 0x2400)
{
    std::vector<std::uint8_t> memory(kGuestBytes);
    store(memory.data(), kGraphics + 0x3198, vs);
    store(memory.data(), kGraphics + 0x3194, ps);
    store(memory.data(), kGraphics + 0x2E2C, 0x2800);
    placeStack(memory.data(), kStack);
    return memory;
}

PPCContext entry(std::uint32_t caller)
{
    PPCContext ctx{};
    ctx.r1.u64  = kStack;
    ctx.r13.u64 = pcrRegister.value_or(kPcr);
    ctx.r3.u64  = kGraphics;
    ctx.r4.u64  = 4;
    ctx.r5.u64  = 0;
    ctx.r6.u64  = 3;
    ctx.r7.u64  = 6;
    ctx.r8.u64  = 0x3000;
    ctx.r9.u64  = 0;
    ctx.r10.u64 = 0x3100;
    ctx.f1.u64  = 0x3FF0000000000000ULL;
    ctx.lr      = caller;
    return ctx;
}

struct Outcome
{
    PPCContext                ctx;
    std::vector<std::uint8_t> memory;
};

// sub_825107A0 commits its direct lowerer call at 0x82510CE0, from the
// frame that called the lowerer.
void commit(std::uint8_t* memory, std::uint32_t graphics = kGraphics, std::uint32_t stack = kStack)
{
    PPCContext ctx{};
    ctx.r1.u64 = stack;
    ctx.r3.u64 = graphics;
    ctx.lr     = 0x82510CE4;
    sub_826A2FF0(ctx, memory);
}

Outcome run(NativeDrawHook hook, std::uint32_t caller, NativeDrawDispatcher* dispatcher)
{
    Outcome outcome{ entry(caller), guestMemory() };
    if (dispatcher)
        install(*dispatcher);
    wrapper(hook)(outcome.ctx, outcome.memory.data());
    if (NativeDrawDefersToCommit(hook, caller))
        commit(outcome.memory.data());
    if (dispatcher)
        remove();
    return outcome;
}

// General, count and floating-point registers, compared by value rather than padding bytes.
const std::array<PPCRegister PPCContext::*, 65> kRegisters{ &PPCContext::r0, &PPCContext::r1, &PPCContext::r2, &PPCContext::r3, &PPCContext::r4, &PPCContext::r5, &PPCContext::r6, &PPCContext::r7, &PPCContext::r8, &PPCContext::r9, &PPCContext::r10, &PPCContext::r11, &PPCContext::r12, &PPCContext::r13, &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18, &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23, &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28, &PPCContext::r29, &PPCContext::r30, &PPCContext::r31, &PPCContext::ctr, &PPCContext::f0, &PPCContext::f1, &PPCContext::f2, &PPCContext::f3, &PPCContext::f4, &PPCContext::f5, &PPCContext::f6, &PPCContext::f7, &PPCContext::f8, &PPCContext::f9, &PPCContext::f10, &PPCContext::f11, &PPCContext::f12, &PPCContext::f13, &PPCContext::f14, &PPCContext::f15, &PPCContext::f16, &PPCContext::f17, &PPCContext::f18, &PPCContext::f19, &PPCContext::f20, &PPCContext::f21, &PPCContext::f22, &PPCContext::f23, &PPCContext::f24, &PPCContext::f25, &PPCContext::f26, &PPCContext::f27, &PPCContext::f28, &PPCContext::f29, &PPCContext::f30, &PPCContext::f31 };

bool sameRegisters(const PPCContext& a, const PPCContext& b)
{
    if (a.lr != b.lr || a.msr != b.msr)
        return false;
    for (const auto member : kRegisters)
        if ((a.*member).u64 != (b.*member).u64)
            return false;
    return true;
}

// The dispatcher must leave every guest-visible register and memory effect unchanged.
void requireSameAbi(NativeDrawHook hook, std::uint32_t caller, NativeDrawDispatcher& dispatcher, const char* message)
{
    const auto plain    = run(hook, caller, nullptr);
    const auto observed = run(hook, caller, &dispatcher);
    UNSCOPED_INFO(message);
    REQUIRE((sameRegisters(plain.ctx, observed.ctx) && plain.memory == observed.memory));
}

NativeDrawDispatchConfig skipConfig()
{
    NativeDrawDispatchConfig config;
    config.mode   = NativeDrawMode::Skip;
    config.read   = readFixture;
    config.failed = [](const NativeDrawFailure&)
    {
        UNSCOPED_INFO("skip mode must never fail");
        REQUIRE(false);
    };
    return config;
}

std::uint64_t reasonCount(const std::array<std::uint64_t, kNativeDrawReasonCount>& counts, NativeDrawReason reason)
{
    return counts[static_cast<std::size_t>(reason)];
}

// One root draw of `hook` with the tiling byte at G+0x2ABC set, and graphics
// pointer `graphics` (0 is unreadable to the census).
void drawWithTilingByte(NativeDrawHook hook, std::uint32_t caller, std::uint8_t tiling, NativeDrawDispatcher& dispatcher, std::uint32_t graphics = kGraphics)
{
    Outcome outcome{ entry(caller), guestMemory() };
    outcome.ctx.r3.u64 = graphics;
    store(outcome.memory.data(), kGraphics + kNativeDrawTilingFlagsOffset, std::uint32_t(tiling) << 24);
    install(dispatcher);
    wrapper(hook)(outcome.ctx, outcome.memory.data());
    if (NativeDrawDefersToCommit(hook, caller))
        commit(outcome.memory.data());
    remove();
}

void testRouteCensus()
{
    ScopedDispatcher dispatcher(skipConfig());
    const auto       drive = [&]
    {
        drawWithTilingByte(NativeDrawHook::IndexedBuffer, 0x8250C768, 0x00, dispatcher);
        drawWithTilingByte(NativeDrawHook::IndexedBuffer, 0x8250C768, 0x00, dispatcher);
        drawWithTilingByte(NativeDrawHook::IndexedBuffer, 0x8250C768, 0x20, dispatcher);
        drawWithTilingByte(NativeDrawHook::IndexedUp, 0x82303E90, 0x21, dispatcher);
        drawWithTilingByte(NativeDrawHook::NonindexedUp, 0x82304258, 0x80, dispatcher);
        drawWithTilingByte(NativeDrawHook::IndexedUpLowerer, 0x82510BFC, 0x00, dispatcher);
        drawWithTilingByte(NativeDrawHook::NonindexedSink, 0x8250E94C, 0x20, dispatcher);
        drawWithTilingByte(NativeDrawHook::AutomaticIndex, 0x82300000, 0x21, dispatcher);
        drawWithTilingByte(NativeDrawHook::IndexedBuffer, 0x8250C768, 0x00, dispatcher, 0);
    };

    // Off by default: nothing is counted.
    originalCalls = 0;
    drive();
    const auto offOriginals = originalCalls.load();
    const auto offFrame     = dispatcher.CloseFrame();
    const auto off          = TakeNativeDrawCensus();
    UNSCOPED_INFO("a disabled census counts nothing");
    REQUIRE((off.roots == std::array<std::uint64_t, kNativeDrawHookCount>{} && off.nested == off.roots && off.unread == off.roots));
    for (const auto& hook : off.tile)
        for (const auto count : hook)
        {
            UNSCOPED_INFO("a disabled census fills no histogram bucket");
            REQUIRE(count == 0);
        }
    UNSCOPED_INFO("a disabled census prints no histogram");
    REQUIRE(FormatNativeDrawCensus(off).find("draw_tile") == std::string::npos);

    EnableNativeDrawCensus(true);
    originalCalls = 0;
    drive();
    EnableNativeDrawCensus(false);
    const auto onOriginals = originalCalls.load();
    const auto onFrame     = dispatcher.CloseFrame();
    const auto on          = TakeNativeDrawCensus();
    // Counting must not change routing: Original runs the same and the same submissions result.
    UNSCOPED_INFO("a census leaves every Original call in place");
    REQUIRE((offOriginals != 0 && onOriginals == offOriginals));
    UNSCOPED_INFO("a census leaves submissions and outcomes unchanged");
    REQUIRE((onFrame.invocations == offFrame.invocations && onFrame.nested == offFrame.nested && onFrame.abandoned == offFrame.abandoned &&
             onFrame.attempted == offFrame.attempted && onFrame.admitted == offFrame.admitted && onFrame.skipped == offFrame.skipped &&
             onFrame.rejected == offFrame.rejected));
    // Roots per hook: the outer wrappers count once, not for their child.
    UNSCOPED_INFO("root invocations are counted once per hook");
    REQUIRE((on.roots == std::array<std::uint64_t, kNativeDrawHookCount>{ 4, 1, 1, 1, 1, 1 }));
    UNSCOPED_INFO("children of the outer wrappers count as nested");
    REQUIRE((on.nested == std::array<std::uint64_t, kNativeDrawHookCount>{ 0, 0, 0, 1, 1, 0 }));
    UNSCOPED_INFO("a null graphics pointer is unread");
    REQUIRE((on.unread == std::array<std::uint64_t, kNativeDrawHookCount>{ 1, 0, 0, 0, 0, 0 }));
    const auto hist = [&](NativeDrawHook hook, std::uint8_t byte)
    {
        return on.tile[static_cast<std::size_t>(hook)][byte];
    };
    std::uint64_t total = 0;
    for (const auto& hook : on.tile)
        for (const auto count : hook)
            total += count;
    UNSCOPED_INFO("the tiling byte histogram pins each root's value");
    REQUIRE((total == 8 && hist(NativeDrawHook::IndexedBuffer, 0x00) == 2 && hist(NativeDrawHook::IndexedBuffer, 0x20) == 1 &&
             hist(NativeDrawHook::IndexedUp, 0x21) == 1 && hist(NativeDrawHook::NonindexedUp, 0x80) == 1 &&
             hist(NativeDrawHook::IndexedUpLowerer, 0x00) == 1 && hist(NativeDrawHook::NonindexedSink, 0x20) == 1 &&
             hist(NativeDrawHook::AutomaticIndex, 0x21) == 1));
    const auto line = FormatNativeDrawCensus(on);
    UNSCOPED_INFO("the timing line fields are exact");
    REQUIRE(line == " draw_root_indexed_buffer=4 draw_root_indexed_up=1 draw_root_nonindexed_up=1 draw_root_indexed_up_lowerer=1 draw_root_nonindexed_sink=1 draw_root_automatic_index=1"
                    " draw_nested_indexed_up_lowerer=1 draw_nested_nonindexed_sink=1"
                    " draw_tile_indexed_buffer_00=2 draw_tile_indexed_buffer_20=1 draw_tile_indexed_buffer_unread=1 draw_tile_indexed_up_21=1 draw_tile_nonindexed_up_80=1"
                    " draw_tile_indexed_up_lowerer_00=1 draw_tile_nonindexed_sink_20=1 draw_tile_automatic_index_21=1");
    const auto reset = TakeNativeDrawCensus();
    UNSCOPED_INFO("a take resets the census");
    REQUIRE((reset.roots == std::array<std::uint64_t, kNativeDrawHookCount>{}));
}

void testNormalizationAndAbi()
{
    ScopedDispatcher dispatcher(skipConfig());
    requireSameAbi(NativeDrawHook::IndexedUp, 0x82303E90, dispatcher, "indexed-UP success keeps copies and cursor");
    requireSameAbi(NativeDrawHook::NonindexedUp, 0x82304258, dispatcher, "nonindexed-UP success keeps copy and return");
    requireSameAbi(NativeDrawHook::IndexedBuffer, 0x8250C768, dispatcher, "bound indexed keeps packets and return");
    requireSameAbi(NativeDrawHook::IndexedUpLowerer, 0x82510BFC, dispatcher, "direct lowerer keeps allocation outputs");
    requireSameAbi(NativeDrawHook::NonindexedSink, 0x8250E94C, dispatcher, "direct sink keeps allocation outputs");
    requireSameAbi(NativeDrawHook::AutomaticIndex, 0x82300000, dispatcher, "automatic index keeps packets");
    lowererFails = true;
    requireSameAbi(NativeDrawHook::IndexedUp, 0x82303E90, dispatcher, "lowerer failure skips copies and returns its code");
    lowererFails = false;
    sinkFails    = true;
    requireSameAbi(NativeDrawHook::NonindexedUp, 0x82304258, dispatcher, "sink allocation failure skips the copy");
    sinkFails = false;

    const auto counters = dispatcher.CloseFrame();
    UNSCOPED_INFO("closing a frame starts the next epoch");
    REQUIRE((counters.frame == 0 && dispatcher.CurrentFrame().frame == 1));
    // Eight roots plus one child under each of the four outer calls.
    UNSCOPED_INFO("outer wrappers fold their child into one submission");
    REQUIRE((counters.attempted == 8 && counters.nested == 4 && counters.abandoned == 0));
    UNSCOPED_INFO("invocation counts include nested children");
    REQUIRE((counters.invocations == std::array<std::uint64_t, kNativeDrawHookCount>{ 1, 2, 2, 3, 3, 1 }));
    UNSCOPED_INFO("without an admitter only the automatic index has a distinct reason");
    REQUIRE((reasonCount(counters.skipped, NativeDrawReason::NoNativeRoute) == 7 &&
             reasonCount(counters.skipped, NativeDrawReason::AutomaticIndex) == 1 && counters.admitted == 0));

    const auto seen = dispatcher.FirstSeen();
    UNSCOPED_INFO("repeated identities share one first-seen record");
    REQUIRE(seen.size() == 6);
    const auto& indexed = seen[0];
    UNSCOPED_INFO("a submission keeps entry references, child and shader identity");
    REQUIRE((indexed.submission.hook == NativeDrawHook::IndexedUp && indexed.submission.caller == 0x82303E90 &&
             indexed.occurrences == 2 && indexed.submission.child == NativeDrawHook::IndexedUpLowerer &&
             indexed.submission.childCaller == kIndexedUpLowererReturn && indexed.submission.childResult == 0 &&
             indexed.submission.vertexShader == 0x2000 && indexed.submission.pixelShader == 0x2400 &&
             indexed.submission.declaration == 0x2800 && indexed.submission.Graphics() == kGraphics &&
             indexed.submission.stack == kStack && indexed.submission.arguments[7] == 0x3100 &&
             indexed.reason == NativeDrawReason::NoNativeRoute));
    UNSCOPED_INFO("a direct lowerer caller is its own submission");
    REQUIRE((seen[3].submission.hook == NativeDrawHook::IndexedUpLowerer && seen[3].submission.caller == 0x82510BFC &&
             !seen[3].submission.child && !seen[3].submission.nestingMismatch));
}

void testFailurePropagation()
{
    ScopedDispatcher dispatcher(skipConfig());
    install(dispatcher);
    lowererFails = true;
    auto memory  = guestMemory();
    auto ctx     = entry(0x82303E90);
    sub_826A3568(ctx, memory.data());
    lowererFails = false;
    UNSCOPED_INFO("lowerer failure code reaches the caller unchanged");
    REQUIRE((ctx.r3.u32 == kLowerFail && load(memory.data(), kGraphics + 0x30) == 0x5000));
    auto seen = dispatcher.FirstSeen();
    UNSCOPED_INFO("the submission records the failing child and root result");
    REQUIRE((seen.size() == 1 && seen[0].submission.result == kLowerFail && seen[0].submission.childResult == kLowerFail));

    sinkFails = true;
    ctx       = entry(0x82304258);
    sub_826A3000(ctx, memory.data());
    sinkFails = false;
    seen      = dispatcher.FirstSeen();
    UNSCOPED_INFO("a null sink allocation returns null through the outer wrapper");
    REQUIRE((ctx.r3.u32 == 0 && seen.size() == 2 && seen[1].submission.childResult == 0 && seen[1].submission.result == 0));

    // Unwinding Original produces no submission and resets per-thread nesting.
    originalThrows = true;
    ctx            = entry(0x82303E90);
    bool thrown    = false;
    try
    {
        sub_826A3568(ctx, memory.data());
    }
    catch (const std::runtime_error&)
    {
        thrown = true;
    }
    originalThrows = false;
    UNSCOPED_INFO("an Original exception propagates through every wrapper");
    REQUIRE((thrown && !nativeDrawThreadActive));
    ctx = entry(0x8250C768);
    sub_826A39F8(ctx, memory.data());
    remove();
    const auto counters = dispatcher.CurrentFrame();
    UNSCOPED_INFO("an unwound draw is abandoned and later draws are roots");
    REQUIRE((counters.abandoned == 1 && counters.attempted == 3 && counters.nested == 3));
}

// Defers run in order; declined roots drain; unwound roots abandon.
void testSequencer()
{
    struct Seen
    {
        std::thread::id thread;
        std::uint32_t   caller   = 0;
        std::uint64_t   sequence = 0;
        bool            deferred = false;
    };

    std::mutex              mutex;
    std::vector<Seen>       began, recorded, abandoned;
    constexpr std::uint32_t kDeferred = 0x8250C768, kInline = 0x8250CB98;
    auto                    config = skipConfig();
    config.sequenced               = true;
    config.defer                   = [](const NativeDrawSubmission& submission) -> std::shared_ptr<const void>
    {
        if (submission.caller != kDeferred && submission.caller != 0x82303E90)
            return nullptr;
        return std::make_shared<int>(7);
    };
    const auto note = [&](std::vector<Seen>& into, const NativeDrawSubmission& submission)
    {
        std::lock_guard lock(mutex);
        into.push_back({ std::this_thread::get_id(), submission.caller, submission.sequence, submission.deferral != nullptr });
    };
    config.began = [&](const NativeDrawSubmission& submission)
    {
        note(began, submission);
    };
    config.recorded = [&](const NativeDrawSubmission& submission, std::optional<NativeDrawReason>)
    {
        note(recorded, submission);
    };
    config.abandoned = [&](const NativeDrawSubmission& submission)
    {
        note(abandoned, submission);
    };
    ScopedDispatcher dispatcher(std::move(config));
    requireSameAbi(NativeDrawHook::IndexedBuffer, kDeferred, dispatcher, "a deferred root keeps Original effects");
    const auto guest = std::this_thread::get_id();
    install(dispatcher);
    auto memory = guestMemory();
    for (const std::uint32_t caller : { kDeferred, kDeferred, kInline, kDeferred })
    {
        auto ctx = entry(caller);
        sub_826A39F8(ctx, memory.data());
    }
    DrainNativeDraws();
    {
        std::lock_guard lock(mutex);
        UNSCOPED_INFO("every root, deferred or not, began and was recorded once drained");
        REQUIRE((recorded.size() == 5 && began.size() == 5));
        for (std::size_t i = 0; i < recorded.size(); ++i)
        {
            const bool deferred = recorded[i].caller == kDeferred;
            UNSCOPED_INFO("roots are numbered in entry order, deferred ones with their deferral");
            REQUIRE((recorded[i].sequence == i + 1 && recorded[i].deferred == deferred && began[i].deferred == deferred && began[i].caller == recorded[i].caller));
            UNSCOPED_INFO("deferred roots begin and record on the sequencer, declined ones on their guest thread");
            REQUIRE(((recorded[i].thread != guest) == deferred && (began[i].thread != guest) == deferred && (!deferred || recorded[i].thread == began[i].thread)));
        }
    }
    // An unwound deferred root reports no verdict; the sequencer abandons it.
    originalThrows = true;
    auto ctx       = entry(0x82303E90);
    bool thrown    = false;
    try
    {
        sub_826A3568(ctx, memory.data());
    }
    catch (const std::runtime_error&)
    {
        thrown = true;
    }
    originalThrows = false;
    remove();
    const auto      stats = dispatcher.Sequencer();
    std::lock_guard lock(mutex);
    UNSCOPED_INFO("an unwound deferred root is abandoned on the sequencer without a verdict");
    REQUIRE((thrown && abandoned.size() == 1 && abandoned[0].thread != guest && abandoned[0].deferred && recorded.size() == 5));
    UNSCOPED_INFO("the sequencer ran each deferred root once");
    REQUIRE((stats.posted == 5 && stats.ran == 5 && dispatcher.CurrentFrame().abandoned == 1));
}

// The queued reader cannot run early and conceal an EndTiling ordering error.
void testResolveTilingDrain(bool drain)
{
    using namespace rerevved::gpu;
    constexpr std::array<std::uint32_t, 6> fetch{ 0x8A000002, 0x1DBB0086, 0x0059E4FF, 0x00A80C14, 0x00000003, 0x00000218 };
    std::string                            error;
    NativeTextureMemoryRanges              ranges;
    REQUIRE(GetNativeTextureMemoryRanges(fetch, ranges, error));
    const auto registry = std::make_shared<ResolveSurfaceRegistry>();
    const auto stored   = [](std::uint64_t mark, bool staged)
    {
        NativeRenderFrameTexture texture{ ResolveSurfaceTexture(mark), staged ? 640U : 1280U, 720, staged ? NativeDrawReplayTextureFormat::R32Uint : NativeDrawReplayTextureFormat::Rgba8, ResolveSurfaceKey(mark), true };
        texture.importedWords = staged ? NativeGuestWordPacking::Color8888 : NativeGuestWordPacking::None;
        return texture;
    };
    const auto record = [&](std::uint64_t mark, bool staged, std::uint32_t x, bool admit)
    {
        ResolveSurfaceRecord value;
        value.mark             = mark;
        value.destEnd          = std::uint64_t(ranges.base.address) + ranges.base.size;
        value.packing          = staged ? NativeGuestWordPacking::Color8888 : NativeGuestWordPacking::None;
        value.staged           = staged;
        value.copy.copyControl = 0x00100000;
        value.copy.destBase    = 0x1DBB0000;
        value.copy.destPitch   = 0x02D00500;
        value.copy.destInfo    = 0x01000302;
        value.copy.destFetch   = fetch;
        value.copy.rectX       = x;
        value.copy.destX       = x;
        value.copy.rectWidth   = staged ? 640 : 1280;
        value.copy.rectHeight  = 720;
        value.copy.packing     = staged ? GuestResolvePacking::Color8888Words : GuestResolvePacking::HostRgba8;
        registry->Record(value);
        if (admit)
            registry->Admit(mark, stored(mark, staged));
    };
    record(10, false, 0, true);
    record(20, true, 0, true);
    record(21, true, 640, false);

    std::mutex                                       mutex;
    std::condition_variable                          changed;
    bool                                             entered = false, released = false;
    std::vector<std::optional<ResolveSurfaceLookup>> observed;
    rerevved::tests::CallbackChecks                  callbackChecks;
    auto                                             config = skipConfig();
    config.sequenced                                        = true;
    config.defer                                            = [](const NativeDrawSubmission&) -> std::shared_ptr<const void>
    {
        return std::make_shared<int>(1);
    };
    config.began = [&](const NativeDrawSubmission& draw)
    {
        {
            std::unique_lock lock(mutex);
            entered = true;
            changed.notify_all();
            changed.wait(lock, [&]
                         {
                             return released;
                         });
        }
        callbackChecks.Require(draw.deferral != nullptr, "the surface reader runs as deferred work");
        const auto      lookup = registry->Find(fetch, true);
        std::lock_guard lock(mutex);
        observed.push_back(lookup);
    };
    config.failed = [&](const NativeDrawFailure&)
    {
        callbackChecks.Require(false, "the surface reader must not fail dispatch");
    };
    auto             memory = guestMemory();
    auto             ctx    = entry(0x8250C768);
    ScopedDispatcher dispatcher(std::move(config));

    struct Restore
    {
        std::shared_ptr<ResolveSurfaceRegistry> previous;
        std::mutex&                             mutex;
        std::condition_variable&                changed;
        bool&                                   released;

        ~Restore()
        {
            // Wake before ScopedDispatcher withdraws and joins its worker.
            {
                std::lock_guard lock(mutex);
                released = true;
            }
            changed.notify_all();
            PublishResolveSurfaces(previous);
        }
    } restore{ PublishedResolveSurfaces(), mutex, changed, released };

    const auto release = [&]
    {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        changed.notify_all();
    };
    unsigned barriers = 0;
    if (drain)
        registry->SetBarrier([&]
                             {
                                 ++barriers;
                                 release();
                                 DrainNativeDraws();
                             });
    install(dispatcher);
    PublishResolveSurfaces(registry);
    sub_826A39F8(ctx, memory.data());
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, 10s, [&]
                                 {
                                     return entered;
                                 }));
        REQUIRE(observed.empty());
    }
    ObserveResolveSurfaceTiling();
    const auto pending = registry->Find(fetch, true);
    REQUIRE((pending && std::holds_alternative<NativeResolvedTextureMiss>(*pending) &&
             std::get<NativeResolvedTextureMiss>(*pending) == NativeResolvedTextureMiss::Unmodeled));
    registry->Admit(21, stored(21, true));
    // Also wakes the control, or a queued reader if the observer missed its barrier.
    release();
    DrainNativeDraws();
    callbackChecks.Verify();
    REQUIRE(barriers == (drain ? 1U : 0U));
    {
        std::lock_guard lock(mutex);
        REQUIRE((observed.size() == 1 && observed[0] && std::holds_alternative<ResolveSurfaceBinding>(*observed[0])));
        const auto& value = std::get<ResolveSurfaceBinding>(*observed[0]);
        REQUIRE((value.mark == (drain ? 10U : 20U) && value.version == (drain ? 1U : 2U)));
    }
    ctx = entry(0x8250C768);
    sub_826A39F8(ctx, memory.data());
    DrainNativeDraws();
    callbackChecks.Verify();
    {
        std::lock_guard lock(mutex);
        REQUIRE((observed.size() == 2 && observed[1] && std::holds_alternative<ResolveSurfaceBinding>(*observed[1])));
        const auto& value = std::get<ResolveSurfaceBinding>(*observed[1]);
        REQUIRE((value.mark == 20 && value.version == 2 && value.texture.frameTextureId == ResolveSurfaceTexture(20)));
        const auto imported = registry->Import(value.texture.frameTextureId);
        REQUIRE((imported && imported->persistentKey == ResolveSurfaceKey(20) && imported->width == 1280 && imported->height == 720 &&
                 imported->format == NativeDrawReplayTextureFormat::R32Uint && imported->importedWords == NativeGuestWordPacking::Color8888));
    }
    REQUIRE((dispatcher.Sequencer().posted == 2 && dispatcher.Sequencer().ran == 2));
    remove();
}

// Callback self-removal never waits; timeouts keep the dispatcher withdrawn.
void testSequencerRemoval()
{
    constexpr std::uint32_t kDeferred = 0x8250C768;
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    entered = false, release = false;
    std::optional<bool>     fromWorker;
    auto                    config = skipConfig();
    config.sequenced               = true;
    config.defer                   = [](const NativeDrawSubmission&) -> std::shared_ptr<const void>
    {
        return std::make_shared<int>(1);
    };
    config.began = [&](const NativeDrawSubmission&)
    {
        std::unique_lock lock(mutex);
        if (!fromWorker)
        {
            lock.unlock();
            const bool removed = RemoveNativeDrawDispatcher(1s);
            lock.lock();
            fromWorker = removed;
            return;
        }
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&]
                     {
                         return release;
                     });
    };
    ScopedDispatcher dispatcher(std::move(config));
    install(dispatcher);
    auto memory = guestMemory();
    auto ctx    = entry(kDeferred);
    sub_826A39F8(ctx, memory.data());
    // Withdrawn by its own callback, so drained here rather than through the hooks.
    dispatcher.Drain();
    {
        std::lock_guard lock(mutex);
        UNSCOPED_INFO("a removal from a sequencer callback withdraws and returns false");
        REQUIRE(fromWorker == false);
    }
    remove();

    // A sequencer still running a root at the deadline keeps the removal open.
    install(dispatcher);
    ctx = entry(kDeferred);
    sub_826A39F8(ctx, memory.data());
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&]
                     {
                         return entered;
                     });
    }
    const auto started = std::chrono::steady_clock::now();
    UNSCOPED_INFO("a removal times out while the sequencer runs a root");
    REQUIRE((!RemoveNativeDrawDispatcher(50ms) && std::chrono::steady_clock::now() - started < 1s));
    UNSCOPED_INFO("the withdrawn dispatcher awaits a later removal");
    REQUIRE(!InstallNativeDrawDispatcher(dispatcher));
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    remove();
    UNSCOPED_INFO("both deferred roots concluded");
    REQUIRE(dispatcher.Sequencer().ran == 2);
}

void testNestingMismatch()
{
    ScopedDispatcher dispatcher(skipConfig());
    install(dispatcher);
    auto memory = guestMemory();
    for (const int children : { 0, 2 })
    {
        outerChildren = children;
        auto ctx      = entry(0x82303E90);
        sub_826A3568(ctx, memory.data());
    }
    outerChildren = 1;
    // A child entered from its outer call site without an observed outer.
    auto ctx = entry(kIndexedUpLowererReturn);
    sub_826A3048(ctx, memory.data());
    ctx = entry(kNonindexedSinkReturn);
    sub_826A2B58(ctx, memory.data());
    // A draw hook nested under a hook with no static child.
    boundNests = true;
    ctx        = entry(0x8250C768);
    sub_826A39F8(ctx, memory.data());
    boundNests = false;
    remove();
    const auto counters = dispatcher.CurrentFrame();
    UNSCOPED_INFO("missing, repeated, orphan and unexpected children are nesting mismatches");
    REQUIRE((counters.attempted == 5 && counters.nested == 3 &&
             reasonCount(counters.skipped, NativeDrawReason::NestingMismatch) == 5));
}

void testStrictMode()
{
    std::vector<NativeDrawFailure> failures;
    NativeDrawDispatchConfig       config;
    config.read   = readFixture;
    config.failed = [&](const NativeDrawFailure& failure)
    {
        failures.push_back(failure);
    };
    NativeDrawDispatcher* self = nullptr;
    // A later recorder supplies this decision; here only the indexed-UP family
    // is admitted. Reading the dispatcher proves admission runs outside its lock.
    config.admit = [&](const NativeDrawSubmission& submission) -> std::optional<NativeDrawReason>
    {
        (void)self->CurrentFrame();
        if (submission.hook == NativeDrawHook::IndexedUp)
            return std::nullopt;
        return NativeDrawReason::NoNativeRoute;
    };
    ScopedDispatcher dispatcher(std::move(config));
    self = &dispatcher;
    requireSameAbi(NativeDrawHook::IndexedUp, 0x82303E90, dispatcher, "admitted draw keeps Original effects");
    requireSameAbi(NativeDrawHook::IndexedBuffer, 0x8250C768, dispatcher, "strict rejection keeps Original effects");
    requireSameAbi(NativeDrawHook::NonindexedUp, 0x82304258, dispatcher, "strict mode after failure keeps Original effects");
    const auto counters = dispatcher.CurrentFrame();
    UNSCOPED_INFO("strict mode rejects rather than skips");
    REQUIRE((counters.attempted == 3 && counters.admitted == 1 &&
             reasonCount(counters.rejected, NativeDrawReason::NoNativeRoute) == 2 &&
             reasonCount(counters.skipped, NativeDrawReason::NoNativeRoute) == 0));
    UNSCOPED_INFO("strict mode reports the first unsupported submission once");
    REQUIRE((failures.size() == 1 && failures[0].submission.hook == NativeDrawHook::IndexedBuffer &&
             dispatcher.Failure() && dispatcher.Failure()->submission.caller == 0x8250C768));

    ScopedDispatcher unreadable(skipConfig());
    install(unreadable);
    auto memory = guestMemory();
    auto ctx    = entry(0x8250C768);
    ctx.r3.u64  = kGuestBytes - 0x100;
    sub_826A39F8(ctx, memory.data());
    remove();
    UNSCOPED_INFO("an unreadable graphics state is a distinct reason");
    REQUIRE(reasonCount(unreadable.CurrentFrame().skipped, NativeDrawReason::StateUnreadable) == 1);

    // A throwing admitter is a dispatch failure: counted in skip mode, never raised.
    auto throwing  = skipConfig();
    throwing.admit = [](const NativeDrawSubmission&) -> std::optional<NativeDrawReason>
    {
        throw std::runtime_error("fixture admitter");
    };
    ScopedDispatcher failing(std::move(throwing));
    requireSameAbi(NativeDrawHook::IndexedBuffer, 0x8250C768, failing, "a throwing admitter keeps Original effects");
    UNSCOPED_INFO("dispatch failures have their own reason and do not fail skip mode");
    REQUIRE((reasonCount(failing.CurrentFrame().skipped, NativeDrawReason::DispatchFailure) == 1 && !failing.Failure()));
}

void testFirstSeenBound()
{
    auto config              = skipConfig();
    config.firstSeenCapacity = 2;
    ScopedDispatcher dispatcher(std::move(config));
    install(dispatcher);
    auto memory = guestMemory();
    for (const std::uint32_t caller : { 0x82000010U, 0x82000020U, 0x82000030U, 0x82000010U })
    {
        auto ctx = entry(caller);
        sub_826A39F8(ctx, memory.data());
    }
    remove();
    const auto seen = dispatcher.FirstSeen();
    UNSCOPED_INFO("first-seen records stay bounded and count dropped submissions");
    REQUIRE((seen.size() == 2 && seen[0].occurrences == 2 && dispatcher.FirstSeenDropped() == 1));
}

void testThreads()
{
    ScopedDispatcher dispatcher(skipConfig());
    install(dispatcher);
    constexpr int            kThreads = 4, kRounds = 300;
    std::barrier<>           insideOuters(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([t, &insideOuters]
                             {
                                 auto memory = guestMemory(0x2000 + t * 0x10);
                                 for (int i = 0; i < kRounds; ++i)
                                 {
                                     // Every thread is inside an outer when its child passes the barrier.
                                     childBarrier = &insideOuters;
                                     auto ctx     = entry(0x82303E90);
                                     sub_826A3568(ctx, memory.data());
                                     ctx = entry(0x82304258);
                                     sub_826A3000(ctx, memory.data());
                                     childBarrier = nullptr;
                                     ctx          = entry(0x82510BFC);
                                     sub_826A3048(ctx, memory.data());
                                     commit(memory.data());
                                 }
                             });
    for (auto& thread : threads)
        thread.join();
    remove();
    const auto totals = dispatcher.Totals();
    UNSCOPED_INFO("concurrent guest threads never attach children across threads");
    REQUIRE((totals.attempted == kThreads * 3 * kRounds && totals.nested == kThreads * 2 * kRounds &&
             reasonCount(totals.skipped, NativeDrawReason::NestingMismatch) == 0));
    UNSCOPED_INFO("each thread's shader identity is retained");
    REQUIRE(dispatcher.FirstSeen().size() == kThreads * 3);
}

void testRemovalWaitsForInFlight()
{
    UNSCOPED_INFO("removal with nothing installed succeeds");
    REQUIRE(RemoveNativeDrawDispatcher(0ms));
    auto dispatcher = std::make_unique<ScopedDispatcher>(skipConfig());
    install(*dispatcher);
    std::latch entered(1), release(1);
    outerEntered = &entered;
    outerRelease = &release;
    std::thread drawer([]
                       {
                           auto memory = guestMemory();
                           auto ctx    = entry(0x82303E90);
                           sub_826A3568(ctx, memory.data());
                       });
    entered.wait();
    outerEntered = nullptr;
    UNSCOPED_INFO("removal times out while a root is in flight");
    REQUIRE(!RemoveNativeDrawDispatcher(20ms));
    std::atomic<bool> removed{ false }, succeeded{ false };
    std::thread       remover([&]
                              {
                            succeeded = RemoveNativeDrawDispatcher(10s);
                            removed   = true;
                              });
    std::this_thread::sleep_for(50ms);
    UNSCOPED_INFO("removal waits for the in-flight root");
    REQUIRE(!removed);
    // The outer now calls its child after the dispatcher was withdrawn.
    release.count_down();
    drawer.join();
    remover.join();
    outerRelease = nullptr;
    UNSCOPED_INFO("removal completes once the root returns");
    REQUIRE(succeeded);
    const auto counters = dispatcher->CurrentFrame();
    UNSCOPED_INFO("a child entered after removal still folds into its root");
    REQUIRE((counters.attempted == 1 && counters.nested == 1 &&
             reasonCount(counters.skipped, NativeDrawReason::NestingMismatch) == 0));
    dispatcher.reset();
}

void testStaleRoot()
{
    ScopedDispatcher dispatcher(skipConfig());
    install(dispatcher);
    auto memory = guestMemory();
    // A root whose scope a non-C++ unwind skipped: begun, never ended.
    auto ctx = entry(0x82303E90);
    (void)BeginNativeDraw(NativeDrawHook::IndexedUp, ctx, memory.data());
    UNSCOPED_INFO("the skipped root stays active on its thread");
    REQUIRE(nativeDrawThreadActive);
    // The next draw at the same guest stack level cannot be its callee.
    ctx = entry(0x8250C768);
    sub_826A39F8(ctx, memory.data());
    UNSCOPED_INFO("the stale root is reset");
    REQUIRE(!nativeDrawThreadActive);
    remove();
    const auto counters = dispatcher.CurrentFrame();
    UNSCOPED_INFO("a stale root is abandoned and releases its in-flight count");
    REQUIRE((counters.abandoned == 1 && counters.attempted == 1 && counters.nested == 0 &&
             reasonCount(counters.skipped, NativeDrawReason::NoNativeRoute) == 1));
}

void testRemoveFromCallback()
{
    // A failure callback runs inside the hook; removal must refuse rather than wait on itself.
    std::optional<bool>      removed;
    std::chrono::nanoseconds waited{};
    NativeDrawDispatchConfig config;
    config.read   = readFixture;
    config.failed = [&](const NativeDrawFailure&)
    {
        const auto start = std::chrono::steady_clock::now();
        removed          = RemoveNativeDrawDispatcher(5s);
        waited           = std::chrono::steady_clock::now() - start;
    };
    ScopedDispatcher dispatcher(std::move(config));
    install(dispatcher);
    auto memory = guestMemory();
    auto ctx    = entry(0x8250C768);
    sub_826A39F8(ctx, memory.data());
    UNSCOPED_INFO("removal from a dispatcher callback returns false immediately");
    REQUIRE((removed == false && waited < 1s));
    UNSCOPED_INFO("the root's count is released when the hook returns");
    REQUIRE((!nativeDrawThreadActive && RemoveNativeDrawDispatcher(1s)));
}

void testExitingThreadReleasesStaleRoot()
{
    ScopedDispatcher dispatcher(skipConfig());
    install(dispatcher);
    std::thread exiting([]
                        {
                            auto memory = guestMemory();
                            auto ctx    = entry(0x82303E90);
                            // The scope was skipped and the thread exits without another draw.
                            (void)BeginNativeDraw(NativeDrawHook::IndexedUp, ctx, memory.data());
                        });
    exiting.join();
    remove();
    UNSCOPED_INFO("thread exit abandons its stale root");
    REQUIRE(dispatcher.CurrentFrame().abandoned == 1);
}

// Held lowerer roots commit in guest order, with entry shadow and commit words.
void testHeldLowerer()
{
    std::vector<std::string>          events;
    std::uint64_t                     marks = 0;
    std::vector<NativeDrawSubmission> recorded;
    std::optional<NativeDrawReason>   verdict;
    std::vector<NativeDrawFailure>    failures;
    const auto                        config = [&](NativeDrawMode mode)
    {
        NativeDrawDispatchConfig value;
        value.mode            = mode;
        value.read            = readFixture;
        value.numberOperation = [&]
        {
            return ++marks;
        };
        value.began = [&](const NativeDrawSubmission&)
        {
            events.push_back("began");
        };
        value.committed = [&](const NativeDrawSubmission& submission)
        {
            events.push_back("committed");
            UNSCOPED_INFO("the committed observer sees the commit");
            REQUIRE(submission.commit.has_value());
        };
        value.recorded = [&](const NativeDrawSubmission& submission, std::optional<NativeDrawReason> reason)
        {
            events.push_back("recorded");
            recorded.push_back(submission);
            verdict = reason;
        };
        value.failed = [&](const NativeDrawFailure& failure)
        {
            failures.push_back(failure);
        };
        return value;
    };
    // The caller's out slot for the vertex destination is at [r1+84].
    const auto memoryWithSlots = []
    {
        auto memory = guestMemory();
        store(memory.data(), kStack + 84, 0x7100);
        store(memory.data(), kGraphics + 0x10, 0xABCD0000);
        return memory;
    };
    const auto lower = [](std::vector<std::uint8_t>& memory, std::uint32_t caller = 0x82510BFC)
    {
        auto ctx    = entry(caller);
        ctx.r6.u64  = 4;
        ctx.r9.u64  = 36;
        ctx.r10.u64 = 0x7104;
        sub_826A3048(ctx, memory.data());
        return ctx;
    };
    {
        ScopedDispatcher dispatcher(config(NativeDrawMode::Skip));
        install(dispatcher);
        auto       memory = memoryWithSlots();
        const auto before = ++marks; // a clear before the draw
        lower(memory);
        UNSCOPED_INFO("a direct lowerer root is held, unnumbered, until its commit");
        REQUIRE((events == std::vector<std::string>{ "began" } && dispatcher.CurrentFrame().attempted == 0 && nativeDrawThreadActive));
        // The caller fills the reserved vertices and indices, then commits.
        store(memory.data(), 0x6000, 0x3F800000);
        commit(memory.data());
        const auto after = ++marks; // a clear after the draw
        UNSCOPED_INFO("the commit builds, admits and records the held root once");
        REQUIRE((events == std::vector<std::string>{ "began", "committed", "recorded" } && !nativeDrawThreadActive));
        UNSCOPED_INFO("the held root is numbered at its commit, between the clears around it");
        REQUIRE((recorded.size() == 1 && recorded[0].operationMark > before && recorded[0].operationMark < after && recorded[0].sequence == 1));
        const auto& held = recorded[0];
        UNSCOPED_INFO("the commit reads the published destinations, counts, out slots and shaders");
        REQUIRE((held.commit && held.commit->read && held.commit->pendingEnd == 0x5100 && held.commit->commitEnd == 0x5100 &&
                 held.commit->vertexDest == 0x6000 && held.commit->indexDest == 0x6800 && held.commit->vertexDwords == 36 &&
                 held.commit->indexDwords == 3 && held.commit->vertexOut == 0x6000 && held.commit->indexOut == 0x6800 &&
                 held.commit->vertexShader == 0x2000 && held.commit->pixelShader == 0x2400 && held.commit->declaration == 0x2800));
        UNSCOPED_INFO("the root keeps the shadow from before the lowerer's flush and staging");
        REQUIRE((held.entryState && held.entryState->size() == 0x3500 && load(held.entryState->data(), 0x10) == 0xABCD0000 &&
                 load(held.entryState->data(), 0x18) == 0 && load(held.entryState->data(), 0x3484) == 0));
        UNSCOPED_INFO("the commit's Original still publishes the PM4 end");
        REQUIRE(load(memory.data(), kGraphics + 0x30) == 0x5100);
        remove();
    }
    {
        // A failed reservation stages nothing: no record, and the thread is released.
        events.clear();
        recorded.clear();
        ScopedDispatcher dispatcher(config(NativeDrawMode::Strict));
        install(dispatcher);
        auto memory    = memoryWithSlots();
        lowererFails   = true;
        const auto ctx = lower(memory);
        lowererFails   = false;
        UNSCOPED_INFO("a lowerer failure produces no record");
        REQUIRE((ctx.r3.u32 == kLowerFail && !nativeDrawThreadActive && events == std::vector<std::string>{ "began" }));
        commit(memory.data());
        remove();
        const auto counters = dispatcher.CurrentFrame();
        UNSCOPED_INFO("a failed lowering is counted as an invocation only");
        REQUIRE((counters.attempted == 0 && counters.abandoned == 0 && counters.invocations[3] == 1 && failures.empty() && recorded.empty()));
    }
    for (const auto mode : { NativeDrawMode::Skip, NativeDrawMode::Strict })
    {
        // The caller moves on without a commit: the next draw from its frame
        // refuses the held root, which is never built.
        events.clear();
        recorded.clear();
        failures.clear();
        ScopedDispatcher dispatcher(config(mode));
        install(dispatcher);
        auto memory = memoryWithSlots();
        lower(memory);
        lower(memory);
        UNSCOPED_INFO("a held root without its commit is refused when its caller draws again");
        REQUIRE((recorded.size() == 1 && verdict == NativeDrawReason::UncommittedSinkDraw && !recorded[0].commit &&
                 events == std::vector<std::string>{ "began", "recorded", "began" }));
        commit(memory.data());
        remove();
        const auto  counters = dispatcher.CurrentFrame();
        const auto& refused  = mode == NativeDrawMode::Skip ? counters.skipped : counters.rejected;
        UNSCOPED_INFO("the second root still commits");
        REQUIRE((counters.attempted == 2 && reasonCount(refused, NativeDrawReason::UncommittedSinkDraw) == 1 && recorded.size() == 2 &&
                 recorded[1].commit));
        UNSCOPED_INFO("strict mode fails on an uncommitted sink draw");
        REQUIRE((mode == NativeDrawMode::Skip ? failures.empty()
                                              : failures.size() == 1 && failures[0].reason == NativeDrawReason::UncommittedSinkDraw));
    }
    for (const auto [graphics, stack] : { std::pair{ kGraphics + 0x40, kStack }, std::pair{ kGraphics, kStack - 0x40 } })
    {
        // A commit from another device or frame is not this root's.
        recorded.clear();
        ScopedDispatcher dispatcher(config(NativeDrawMode::Skip));
        install(dispatcher);
        auto memory = memoryWithSlots();
        lower(memory);
        commit(memory.data(), graphics, stack);
        UNSCOPED_INFO("a commit with another device or stack refuses the held root");
        REQUIRE((recorded.size() == 1 && verdict == NativeDrawReason::UncommittedSinkDraw && !recorded[0].commit && !nativeDrawThreadActive));
        remove();
    }
    for (const auto mode : { NativeDrawMode::Skip, NativeDrawMode::Strict })
    {
        // Other PM4 staged after the lowering moves G+0x3484: the commit no
        // longer publishes the held draw, which is refused, not built.
        events.clear();
        recorded.clear();
        failures.clear();
        ScopedDispatcher dispatcher(config(mode));
        install(dispatcher);
        auto memory = memoryWithSlots();
        lower(memory);
        store(memory.data(), kGraphics + 0x3484, load(memory.data(), kGraphics + 0x3484) + 0x40);
        commit(memory.data());
        remove();
        const auto  counters = dispatcher.CurrentFrame();
        const auto& refused  = mode == NativeDrawMode::Skip ? counters.skipped : counters.rejected;
        UNSCOPED_INFO("a commit after G+0x3484 moved refuses the held root as uncommitted");
        REQUIRE((recorded.size() == 1 && verdict == NativeDrawReason::UncommittedSinkDraw && recorded[0].commit &&
                 recorded[0].commit->commitEnd == recorded[0].commit->pendingEnd + 0x40 && events == std::vector<std::string>{ "began", "recorded" } &&
                 reasonCount(refused, NativeDrawReason::UncommittedSinkDraw) == 1 && !nativeDrawThreadActive));
        UNSCOPED_INFO("strict mode fails on a moved pending end");
        REQUIRE((mode == NativeDrawMode::Skip ? failures.empty()
                                              : failures.size() == 1 && failures[0].reason == NativeDrawReason::UncommittedSinkDraw));
    }
    {
        // Commits without a held root: none at all, and one after a direct
        // NonindexedSink call, which stays out of scope.
        recorded.clear();
        ScopedDispatcher dispatcher(config(NativeDrawMode::Skip));
        install(dispatcher);
        auto memory = memoryWithSlots();
        commit(memory.data());
        auto ctx = entry(0x8250E94C);
        sub_826A2B58(ctx, memory.data());
        commit(memory.data());
        // The IndexedUp wrapper commits its child inline; its root is never held.
        ctx = entry(0x82303E90);
        sub_826A3568(ctx, memory.data());
        UNSCOPED_INFO("commits without a held root change nothing, and the IndexedUp child is not held");
        REQUIRE((!nativeDrawThreadActive && recorded.size() == 2 && !recorded[0].commit && !recorded[1].commit &&
                 !recorded[1].entryState && recorded[1].hook == NativeDrawHook::IndexedUp));
        commit(memory.data());
        remove();
        UNSCOPED_INFO("only the sink and the IndexedUp draw are numbered");
        REQUIRE(dispatcher.CurrentFrame().attempted == 2);
    }
    enum class Ledgered
    {
        FailedThenCommitted,
        OpenAtEntry,
        ThreadExitNextDraw,
        ThreadExitRemoval,
    };
    for (const auto scenario : { Ledgered::FailedThenCommitted, Ledgered::OpenAtEntry, Ledgered::ThreadExitNextDraw, Ledgered::ThreadExitRemoval })
    {
        // The ledger wired as src/app.cpp wires it: a held root opens its draw
        // at the commit, so a failed lowering leaves nothing open for the next
        // draw to abandon. OpenAtEntry is the entry wiring that would.
        const bool openAtEntry = scenario == Ledgered::OpenAtEntry;

        struct Sink final : rerevved::gpu::completion::IGuestProgressSink
        {
            bool WritePhysical(std::uint32_t, std::uint32_t) override
            {
                return true;
            }

            std::optional<std::uint32_t> ReadPhysical(std::uint32_t) override
            {
                return 0;
            }

            rerevved::gpu::completion::InterruptResult RaiseCallbackInterrupt(std::uint32_t) override
            {
                return rerevved::gpu::completion::InterruptResult::Raised;
            }
        } sink;

        std::vector<std::string>                    ledgerFailures;
        rerevved::gpu::completion::CompletionLedger ledger({ NativeDrawMode::Strict, &sink, nullptr, [&](const rerevved::gpu::completion::LedgerFailure& failure)
                                                             {
                                                                 ledgerFailures.push_back(failure.detail);
                                                             } });
        ledger.BeginRingEpoch();
        ledger.RegisterWriteback(0x1000);
        recorded.clear();
        failures.clear();
        auto wired  = config(NativeDrawMode::Strict);
        wired.admit = [](const NativeDrawSubmission&) -> std::optional<NativeDrawReason>
        {
            return std::nullopt;
        };
        wired.began = [&](const NativeDrawSubmission& submission)
        {
            if (openAtEntry || !NativeDrawDefersToCommit(submission.hook, submission.caller))
                ledger.BeginDraw(submission.hook, submission.caller);
        };
        wired.committed = [&](const NativeDrawSubmission& submission)
        {
            if (!openAtEntry)
                ledger.BeginDraw(submission.hook, submission.caller);
        };
        wired.recorded = [&](const NativeDrawSubmission& submission, std::optional<NativeDrawReason> reason)
        {
            recorded.push_back(submission);
            ledger.FinishDraw(submission.operationMark, submission.hook, submission.caller, reason);
        };
        ScopedDispatcher dispatcher(wired);
        install(dispatcher);
        if (scenario == Ledgered::ThreadExitNextDraw || scenario == Ledgered::ThreadExitRemoval)
        {
            // The thread ends while its lowering is held, never committed. Its
            // teardown only queues the root; a live entry refuses it.
            std::thread caller([&]
                               {
                                   auto memory = memoryWithSlots();
                                   lower(memory);
                               });
            caller.join();
            UNSCOPED_INFO("thread exit records nothing on the exiting thread");
            REQUIRE((recorded.empty() && failures.empty() && !ledger.Failure()));
            const bool nextDraw = scenario == Ledgered::ThreadExitNextDraw;
            if (nextDraw)
            {
                auto memory = memoryWithSlots();
                lower(memory);
                commit(memory.data());
            }
            remove();
            const auto failure = ledger.Failure();
            UNSCOPED_INFO((nextDraw ? "the next draw from another thread first refuses the exited thread's held root in the record and the strict ledger"
                                    : "removal refuses the exited thread's held root in the record and the strict ledger"));
            REQUIRE((recorded.size() == (nextDraw ? 2U : 1U) && !recorded[0].commit && (!nextDraw || recorded[1].commit) && failures.size() == 1 &&
                     failures[0].reason == NativeDrawReason::UncommittedSinkDraw && failure && failure->find("uncommitted_sink_draw") != std::string::npos));
            continue;
        }
        auto memory  = memoryWithSlots();
        lowererFails = true;
        lower(memory);
        lowererFails = false;
        lower(memory);
        commit(memory.data());
        remove();
        const auto failure = ledger.Failure();
        if (openAtEntry)
        {
            UNSCOPED_INFO("opening the ledger draw at entry abandons a failed lowering");
            REQUIRE((failure && failure->find("draw_abandoned") != std::string::npos));
        }
        else
        {
            UNSCOPED_INFO("a failed lowering then a committed one leave the strict ledger healthy");
            REQUIRE((!failure && ledgerFailures.empty() && failures.empty() && recorded.size() == 1 && recorded[0].commit));
        }
    }
    {
        // Two threads exit holding their roots. Removal drains both as one
        // batch; a removal from a refusal callback returns false at once and
        // the batch still refuses the second orphan against live state.
        recorded.clear();
        auto              reentrant = config(NativeDrawMode::Skip);
        std::vector<bool> inner;
        reentrant.recorded = [&](const NativeDrawSubmission& submission, std::optional<NativeDrawReason> reason)
        {
            recorded.push_back(submission);
            verdict = reason;
            inner.push_back(RemoveNativeDrawDispatcher(20ms));
        };
        ScopedDispatcher dispatcher(reentrant);
        install(dispatcher);
        // Both roots are held before either thread exits, so neither entry drains.
        std::latch               lowered(2);
        std::vector<std::thread> callers;
        for (int i = 0; i < 2; ++i)
            callers.emplace_back([&]
                                 {
                                     auto memory = memoryWithSlots();
                                     lower(memory);
                                     lowered.arrive_and_wait();
                                 });
        for (auto& caller : callers)
            caller.join();
        UNSCOPED_INFO("both exits only queue their roots");
        REQUIRE(recorded.empty());
        UNSCOPED_INFO("removal completes after draining");
        REQUIRE(RemoveNativeDrawDispatcher(1s));
        UNSCOPED_INFO("a removal from a drain callback returns false and the batch refuses both orphans");
        REQUIRE((recorded.size() == 2 && inner == std::vector<bool>{ false, false } && verdict == NativeDrawReason::UncommittedSinkDraw &&
                 reasonCount(dispatcher.CurrentFrame().skipped, NativeDrawReason::UncommittedSinkDraw) == 2));
    }
    {
        // The exit allocates nothing: the held root's node was reserved when
        // the lowerer returned.
        recorded.clear();
        ScopedDispatcher dispatcher(config(NativeDrawMode::Skip));
        install(dispatcher);
        std::thread::id callerId;
        std::latch      lowered(1), go(1);
        std::thread     caller([&]
                               {
                               auto memory = memoryWithSlots();
                               lower(memory);
                               callerId = std::this_thread::get_id();
                               lowered.count_down();
                               go.wait();
                               });
        lowered.wait();
        {
            // The exit runs with this thread's allocations failing; nothing
            // asserts until the scope ends.
            rerevved::tests::allocation::ScopedThreadFailure failing(callerId);
            go.count_down();
            caller.join();
        }
        remove();
        UNSCOPED_INFO("a thread exit that cannot allocate still queues its held root for refusal");
        REQUIRE((recorded.size() == 1 && verdict == NativeDrawReason::UncommittedSinkDraw));
    }
    {
        // A removal paused inside its lock, before withdrawal, keeps every
        // other removal from returning true or touching the installed pointer.
        recorded.clear();
        auto dispatcher = std::make_unique<ScopedDispatcher>(config(NativeDrawMode::Skip));
        install(*dispatcher);
        std::thread caller([&]
                           {
                               auto memory = memoryWithSlots();
                               lower(memory);
                           });
        caller.join();
        static std::latch* paused = nullptr;
        static std::latch* resume = nullptr;
        std::latch         pausedAt(1), resumeAt(1);
        paused                         = &pausedAt;
        resume                         = &resumeAt;
        detail::nativeDrawRemovalPause = []
        {
            detail::nativeDrawRemovalPause = nullptr;
            paused->count_down();
            resume->wait();
        };
        bool        first = false;
        std::thread firstRemover([&]
                                 {
                                     first = RemoveNativeDrawDispatcher(2s);
                                 });
        pausedAt.wait();
        const bool       timedOut = RemoveNativeDrawDispatcher(20ms);
        ScopedDispatcher probe(config(NativeDrawMode::Skip));
        const bool       stillInstalled = !InstallNativeDrawDispatcher(probe);
        UNSCOPED_INFO("a removal that times out on the lock leaves the dispatcher installed");
        REQUIRE((!timedOut && stillInstalled));
        std::atomic<bool> secondDone{ false };
        bool              second = false;
        std::thread       secondRemover([&]
                                        {
                                      second = RemoveNativeDrawDispatcher(2s);
                                      secondDone.store(true);
                                        });
        std::this_thread::sleep_for(100ms);
        const bool earlyReturn = secondDone.load();
        resumeAt.count_down();
        firstRemover.join();
        secondRemover.join();
        UNSCOPED_INFO("a second removal waits for a paused one, which drains its orphan once");
        REQUIRE((!earlyReturn && first && second && recorded.size() == 1));
        dispatcher.reset();
    }
    {
        // Two concurrent removals are serialized: each returns true only once
        // every orphan's refusal callback has finished, and each orphan is
        // refused once.
        recorded.clear();
        std::mutex       recordMutex;
        std::atomic<int> running{ 0 };
        auto             slow = config(NativeDrawMode::Skip);
        slow.recorded         = [&](const NativeDrawSubmission& submission, std::optional<NativeDrawReason>)
        {
            ++running;
            std::this_thread::sleep_for(30ms);
            {
                std::lock_guard lock(recordMutex);
                recorded.push_back(submission);
            }
            --running;
        };
        ScopedDispatcher dispatcher(slow);
        install(dispatcher);
        std::latch               lowered(2);
        std::vector<std::thread> callers;
        for (int i = 0; i < 2; ++i)
            callers.emplace_back([&]
                                 {
                                     auto memory = memoryWithSlots();
                                     lower(memory);
                                     lowered.arrive_and_wait();
                                 });
        for (auto& caller : callers)
            caller.join();
        std::atomic<int>         settled{ 0 };
        std::vector<std::thread> removers;
        for (int i = 0; i < 2; ++i)
            removers.emplace_back([&]
                                  {
                                      if (!RemoveNativeDrawDispatcher(2s))
                                          return;
                                      std::lock_guard lock(recordMutex);
                                      if (running == 0 && recorded.size() == 2)
                                          ++settled;
                                  });
        for (auto& remover : removers)
            remover.join();
        UNSCOPED_INFO("both concurrent removals return after every orphan is refused, once each");
        REQUIRE((settled == 2 && recorded.size() == 2));
    }
    for (const bool destroy : { false, true })
    {
        // Dispatcher A's removal times out with a root held. No dispatcher
        // installs until a removal that returns true drains A's orphan, or A
        // is destroyed and drops it unrefused; B's orphans keep FIFO order.
        constexpr std::uint32_t                kCallerA = 0x82510C40;
        constexpr std::array<std::uint32_t, 3> kCallersB{ 0x82510C10, 0x82510C20, 0x82510C30 };
        recorded.clear();
        auto first = std::make_unique<ScopedDispatcher>(config(NativeDrawMode::Skip));
        install(*first);
        std::latch  lowered(1), release(1);
        std::thread caller([&]
                           {
                               auto memory = memoryWithSlots();
                               lower(memory, kCallerA);
                               lowered.count_down();
                               release.wait();
                           });
        lowered.wait();
        UNSCOPED_INFO("A's removal times out while its root is held");
        REQUIRE(!RemoveNativeDrawDispatcher(20ms));
        // A is withdrawn and awaits a removal; destroying it is the case under test.
        installedByTest = nullptr;
        ScopedDispatcher second(config(NativeDrawMode::Skip));
        UNSCOPED_INFO("no dispatcher installs while A's root is held");
        REQUIRE(!InstallNativeDrawDispatcher(second));
        release.count_down();
        caller.join();
        UNSCOPED_INFO("no dispatcher installs while A's orphan awaits a removal");
        REQUIRE(!InstallNativeDrawDispatcher(second));
        if (destroy)
            first.reset();
        else
        {
            UNSCOPED_INFO("a later removal refuses A's orphan before returning true");
            REQUIRE((RemoveNativeDrawDispatcher(1s) && recorded.size() == 1 && recorded[0].caller == kCallerA));
        }
        install(second);
        // Every B root is held before any exits, so no draw entry drains a
        // predecessor and the queue alone orders the refusals.
        std::latch                loweredB(3);
        std::array<std::latch, 3> releaseB{ std::latch(1), std::latch(1), std::latch(1) };
        std::vector<std::thread>  callers;
        for (std::size_t i = 0; i < kCallersB.size(); ++i)
            callers.emplace_back([&, i]
                                 {
                                     auto memory = memoryWithSlots();
                                     lower(memory, kCallersB[i]);
                                     loweredB.count_down();
                                     releaseB[i].wait();
                                 });
        loweredB.wait();
        for (std::size_t i = 0; i < callers.size(); ++i)
        {
            releaseB[i].count_down();
            callers[i].join();
        }
        remove();
        const std::size_t base = destroy ? 0 : 1;
        UNSCOPED_INFO((destroy ? "a destroyed A's node is never refused; B's orphans are refused in exit order"
                               : "B's orphans are refused after A's, in exit order"));
        REQUIRE((recorded.size() == base + 3 && recorded[base].caller == kCallersB[0] && recorded[base + 1].caller == kCallersB[1] &&
                 recorded[base + 2].caller == kCallersB[2]));
        first.reset();
    }
    {
        // Removal waits through a held root; its own thread cannot wait on it.
        auto dispatcher = std::make_unique<ScopedDispatcher>(skipConfig());
        install(*dispatcher);
        std::latch          held(1), release(1);
        std::optional<bool> removedFromHolder;
        std::thread         holder([&]
                                   {
                               auto memory = guestMemory();
                               auto ctx    = entry(0x82510BFC);
                               sub_826A3048(ctx, memory.data());
                               removedFromHolder = RemoveNativeDrawDispatcher(5s);
                               held.count_down();
                               release.wait();
                               commit(memory.data());
                                   });
        held.wait();
        UNSCOPED_INFO("the holding thread's removal returns false at once");
        REQUIRE(removedFromHolder == false);
        UNSCOPED_INFO("removal times out while a root is held");
        REQUIRE(!RemoveNativeDrawDispatcher(20ms));
        release.count_down();
        holder.join();
        UNSCOPED_INFO("removal completes once the held root commits");
        REQUIRE(RemoveNativeDrawDispatcher(1s));
        UNSCOPED_INFO("a root held across removal is still numbered at its commit");
        REQUIRE(dispatcher->CurrentFrame().attempted == 1);
        dispatcher.reset();
    }
}

struct SceneDraw
{
    std::uint64_t sequence = 0, frame = 0;
    std::uint32_t entry = 0, caller = 0, graphics = 0;
};

// Reads top-level draw fields from a scene capture events.toml without loading
// the whole table. Rejects any shape it does not understand.
bool loadSceneDraws(std::istream& file, std::vector<SceneDraw>& draws, std::string& error)
{
    draws.clear();

    struct Event
    {
        std::optional<std::string>   kind;
        std::optional<std::uint64_t> sequence, frame, entry, caller, graphics;
    } event;

    bool        started = false, topLevel = false;
    int         arrayDepth = 0;
    std::size_t lineNumber = 0;
    const auto  fail       = [&](const std::string& reason)
    {
        error = "line " + std::to_string(lineNumber) + ": " + reason;
        return false;
    };
    const auto flush = [&]
    {
        if (!started)
            return true;
        if (!event.kind)
            return fail("events block without kind");
        if (*event.kind == "draw")
        {
            if (!event.sequence || !event.frame || !event.entry || !event.caller || !event.graphics)
                return fail("draw event without sequence, frame, entry, caller_lr or raw r3");
            draws.push_back({ *event.sequence, *event.frame, static_cast<std::uint32_t>(*event.entry), static_cast<std::uint32_t>(*event.caller), static_cast<std::uint32_t>(*event.graphics) });
        }
        event = {};
        return true;
    };
    // Unsigned values are bare digits or single-quoted digits.
    const auto number = [&](std::string value, std::optional<std::uint64_t>& out)
    {
        if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'')
            value = value.substr(1, value.size() - 2);
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
            return fail("unrecognized number '" + value + "'");
        out = std::stoull(value);
        return true;
    };
    std::string line;
    while (std::getline(file, line))
    {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto start = line.find_first_not_of(' ');
        if (start == std::string::npos)
            continue;
        if (!arrayDepth && line[start] == '[')
        {
            if (line == "[[events]]")
            {
                if (!flush())
                    return false;
                started  = true;
                topLevel = true;
            }
            else
                topLevel = false;
            continue;
        }
        const auto equals = arrayDepth ? std::string::npos : line.find(" = ");
        char       quote  = 0;
        for (std::size_t i = 0; i < line.size(); ++i)
        {
            const char c = line[i];
            if (quote == '"' && c == '\\')
                ++i;
            else if (quote)
                quote = c == quote ? 0 : quote;
            else if (c == '\'' || c == '"')
                quote = c;
            else if (c == '[')
                ++arrayDepth;
            else if (c == ']')
                --arrayDepth;
        }
        if (quote || arrayDepth < 0)
            return fail("unbalanced quoting or brackets");
        if (!topLevel || equals == std::string::npos || !started)
            continue;
        const auto key = line.substr(0, equals), value = line.substr(equals + 3);
        bool       ok = true;
        if (key == "kind")
        {
            if (value.size() < 2 || value.front() != '\'' || value.back() != '\'')
                return fail("kind is not a single-quoted string");
            event.kind = value.substr(1, value.size() - 2);
        }
        else if (key == "sequence")
            ok = number(value, event.sequence);
        else if (key == "frame")
            ok = number(value, event.frame);
        else if (key == "entry")
            ok = number(value, event.entry);
        else if (key == "caller_lr")
            ok = number(value, event.caller);
        else if (key == "raw_r3_r10_u64" && value != "[")
        {
            // Draw events print it on one line; a multi-line array leaves r3 unset.
            const auto first = value.find('\''), last = value.find('\'', first + 1);
            if (value.front() != '[' || first == std::string::npos || last == std::string::npos)
                return fail("raw_r3_r10_u64 is not a one-line quoted array");
            ok = number(value.substr(first, last - first + 1), event.graphics);
        }
        if (!ok)
            return false;
    }
    if (arrayDepth)
        return fail("unterminated array");
    return flush();
}

bool loadSceneDraws(const std::filesystem::path& path, std::vector<SceneDraw>& draws, std::string& error)
{
    std::ifstream file(path);
    if (!file)
    {
        error = "scene events file must open";
        return false;
    }
    return loadSceneDraws(file, draws, error);
}

struct ReplayResult
{
    std::uint64_t                        snapshots = 0, paired = 0;
    NativeDrawFrameCounters              totals;
    std::vector<NativeDrawFrameCounters> frames;
    std::vector<std::uint64_t>           captureFrames;
};

// Child replay requires immediate same graphics-pointer sequence; LR mismatch
// counts independently.
ReplayResult replaySceneDraws(const std::vector<SceneDraw>& draws)
{
    ScopedDispatcher dispatcher(skipConfig());
    install(dispatcher);
    auto         memory = guestMemory();
    ReplayResult result;
    result.snapshots = draws.size();
    for (std::size_t i = 0; i < draws.size(); ++i)
    {
        if (!i || draws[i].frame != draws[i - 1].frame)
        {
            if (i)
                result.frames.push_back(dispatcher.CloseFrame());
            result.captureFrames.push_back(draws[i].frame);
        }
        const auto hook = FindNativeDrawHook(draws[i].entry);
        UNSCOPED_INFO("scene draw entry must be one of the six draw hooks");
        REQUIRE(hook.has_value());
        outerChildren     = 0;
        replayChildCaller = 0;
        if (i + 1 < draws.size())
        {
            const auto& next  = draws[i + 1];
            const auto  child = FindNativeDrawHook(next.entry);
            if (next.sequence == draws[i].sequence + 1 && next.frame == draws[i].frame && next.graphics == draws[i].graphics &&
                ((*hook == NativeDrawHook::IndexedUp && child == NativeDrawHook::IndexedUpLowerer) ||
                 (*hook == NativeDrawHook::NonindexedUp && child == NativeDrawHook::NonindexedSink)))
            {
                outerChildren     = 1;
                replayChildCaller = next.caller;
            }
        }
        auto ctx = entry(draws[i].caller);
        wrapper (*hook)(ctx, memory.data());
        // A capture records no commit; a direct lowerer's caller always makes one.
        if (NativeDrawDefersToCommit(*hook, draws[i].caller))
            commit(memory.data());
        result.paired += outerChildren;
        i += outerChildren;
    }
    outerChildren     = 1;
    replayChildCaller = 0;
    remove();
    if (!draws.empty())
        result.frames.push_back(dispatcher.CloseFrame());
    result.totals = dispatcher.Totals();
    return result;
}

// Developer builds record shader harvests; the shipping build has none, so the
// guard-order assertions around a harvest still run and only the harvest arming
// and its Diagnostics outcome are conditional.
inline constexpr bool kDeveloperBuild = REREVVED_DEVELOPER != 0;

#if REREVVED_DEVELOPER
void harvestObserver(const rerevved::gpu::diagnostics::ShaderHarvestCall&) noexcept
{
}
#endif

void armShaderHarvest([[maybe_unused]] bool armed) noexcept
{
#if REREVVED_DEVELOPER
    rerevved::gpu::diagnostics::shaderHarvestObserver.store(armed ? harvestObserver : nullptr);
#endif
}

#if REREVVED_DEVELOPER
bool alwaysSelect(std::uint64_t, std::uint32_t)
{
    return true;
}
#endif

// The route trace and scene sessions are one per process, and the IndexedBuffer
// test holds them open, so it calls this in each window: with the wrapper cvar
// on, both wrappers run Original and count as a diagnostics fallback.
void requireUserPointerKeepsOriginal(const char* message)
{
    struct Wrapper
    {
        UserPointerWrapper id;
        NativeDrawHook     hook;
        std::uint32_t      caller;
    };

    ScopedDispatcher dispatcher(skipConfig());
    REXCVAR_SET(native_consume_user_pointer, true);
    DEV_DO(TakeUserPointerConsumeCounts());
    for (const auto& w : { Wrapper{ UserPointerWrapper::IndexedUp, NativeDrawHook::IndexedUp, 0x82303E90 },
                           Wrapper{ UserPointerWrapper::NonindexedUp, NativeDrawHook::NonindexedUp, 0x82304258 } })
    {
        const auto slot      = static_cast<std::size_t>(w.id);
        const auto originals = wrapperOriginals[slot].load();
        auto       memory    = guestMemory();
        auto       ctx       = entry(w.caller);
        // The stride byte matches G+0x2E38, so only the diagnostics apply.
        store(memory.data(), kGraphics + kIndexedBufferStrideByteOffset, ((w.id == UserPointerWrapper::IndexedUp ? 0U : ctx.r7.u32) >> 2 & 0xFF) << 24);
        install(dispatcher);
        wrapper(w.hook)(ctx, memory.data());
        remove();
#if REREVVED_DEVELOPER
        const auto counts                                                     = TakeUserPointerConsumeCounts();
        auto       one                                                        = decltype(counts.fallback){};
        one[slot][static_cast<std::size_t>(UserPointerFallback::Diagnostics)] = 1;
#endif
        UNSCOPED_INFO(message);
        REQUIRE((wrapperOriginals[slot] == originals + 1 && DEV_CHECK(counts.consumed == decltype(counts.consumed){}) && DEV_CHECK(counts.fallback == one)));
    }
    REXCVAR_SET(native_consume_user_pointer, false);
}

// Dirty bit 0x80000 triggers 0x826ADD88 VS fetch patch on cache miss; consume
// requires hit or no patch.
std::uint32_t failBegin = 0, failEnd = 0;

bool failingReader(const std::uint8_t* base, std::uint32_t address, std::uint32_t& value)
{
    return !(address >= failBegin && address < failEnd) && readFixture(base, address, value);
}

struct FetchHarness
{
    using Memory = std::vector<std::uint8_t>;
    using Adjust = std::function<void(Memory&)>;

    bool                                                                                                                userPointer = false;
    std::function<void(std::uint32_t, const Adjust&, std::optional<std::uint32_t>, const char*)>                        consumed;
    std::function<void(std::uint32_t, const Adjust&, std::optional<std::uint32_t>, IndexedBufferFallback, const char*)> fallback;
};

void testVertexFetchPatch(const FetchHarness& h)
{
    using Memory = FetchHarness::Memory;
    // The fixture names VS 0x2000 (header flags at +872, cache record v at +40+416v)
    // and the declaration 0x2800 (masks at +32 and +40, tag at +48). Zeroed memory
    // is a hit: equal tags and empty masks.
    constexpr std::uint32_t kVs = 0x2000, kVd = 0x2800, kDirty = 0x00080000;
    constexpr std::uint32_t kRecord0 = kVs + 40, kRecord1 = kVs + 40 + 416, kFlags = kVs + 872, kMask = kVd + 32, kTag = kVd + 48;
    constexpr std::uint32_t kStream = kGraphics + 0x30F0, kNoPixel = kGraphics + 0x3194, kMode = kGraphics + 0x2954;
    constexpr std::uint32_t kTagValue = 0x1234;
    const auto              set       = [](Memory& memory, std::uint32_t address, std::uint32_t value)
    {
        store(memory.data(), address, value);
    };
    const auto both = [](FetchHarness::Adjust a, FetchHarness::Adjust b) -> FetchHarness::Adjust
    {
        return [=](Memory& memory)
        {
            a(memory);
            b(memory);
        };
    };
    const FetchHarness::Adjust none = [](Memory&)
    {
    };
    // The declaration's tag differs from the record's.
    const FetchHarness::Adjust tagMiss = [&](Memory& memory)
    {
        set(memory, kTag, kTagValue);
    };
    // Equal tags, so only the stride bytes decide.
    const FetchHarness::Adjust tagged = [&](Memory& memory)
    {
        set(memory, kTag, kTagValue);
        set(memory, kRecord0, kTagValue);
        set(memory, kRecord1, kTagValue);
    };
    const auto word = [&](std::uint32_t address, std::uint32_t value) -> FetchHarness::Adjust
    {
        return [=](Memory& memory)
        {
            store(memory.data(), address, value);
        };
    };
    const auto consumed = [&](const FetchHarness::Adjust& adjust, const std::string& message)
    {
        h.consumed(kDirty, adjust, std::nullopt, message.c_str());
    };
    const auto fell = [&](const FetchHarness::Adjust& adjust, const std::string& message)
    {
        h.fallback(kDirty, adjust, std::nullopt, IndexedBufferFallback::DirtyVertexFetch, message.c_str());
    };

    consumed(none, "a dirty 0x80000 root whose fetch cache hits is consumed");
    consumed(tagged, "equal fetch cache tags with empty masks hit");
    fell(tagMiss, "a fetch cache tag that differs from the declaration's runs Original");
    fell(
        [&](Memory& memory)
        {
            set(memory, kRecord0, kTagValue);
        },
        "a record tag that differs from the declaration's runs Original");

    // The stride bytes under the masks: each of the four words of the two streams.
    struct Stride
    {
        const char*   name;
        std::uint32_t record, stream, mask;
    };

    for (const auto& s : { Stride{ "stream 0 high", kRecord0 + 8, kStream, kMask },
                           Stride{ "stream 0 low", kRecord0 + 12, kStream + 4, kMask + 4 },
                           Stride{ "stream 1 high", kRecord0 + 16, kStream + 8, kMask + 8 },
                           Stride{ "stream 1 low", kRecord0 + 20, kStream + 12, kMask + 12 } })
    {
        // The top byte of stream 0 is a wrapper's stride and is tested apart.
        const std::uint32_t top    = s.stream == kStream ? 0x00FFFFFF : 0xFFFFFFFF;
        const std::uint32_t value  = 0x00123456;
        const auto          shaped = [&](std::uint32_t streamValue, std::uint32_t mask)
        {
            return both(tagged,
                        [=](Memory& memory)
                        {
                            set(memory, s.record, value);
                            set(memory, s.stream, streamValue);
                            set(memory, s.mask, mask);
                        });
        };
        const std::string name  = s.name;
        const auto        cache = [&](const char* what)
        {
            return name + ' ' + what;
        };
        consumed(shaped(value, top), cache("stride equal under its mask is consumed"));
        fell(shaped(value ^ 0x1, top), cache("stride that differs under its mask runs Original"));
        consumed(shaped(value ^ 0x1, top & ~0x1U), cache("stride that differs only outside its mask is consumed"));
        consumed(shaped(value ^ 0x1, 0), cache("stride that differs under an empty mask is consumed"));
    }
    // A mask applies to its own stream only.
    consumed(both(tagged,
                  [&](Memory& memory)
                  {
                      set(memory, kMask, 0x00FFFFFF);
                      set(memory, kMask + 4, 0xFFFFFFFF);
                      set(memory, kStream + 8, 0x1);
                      set(memory, kStream + 12, 0x1);
                  }),
             "a stream 1 stride difference under only a stream 0 mask is consumed");
    consumed(both(tagged,
                  [&](Memory& memory)
                  {
                      set(memory, kMask + 8, 0xFFFFFFFF);
                      set(memory, kMask + 12, 0xFFFFFFFF);
                      set(memory, kStream + 4, 0x1);
                      set(memory, kStream, 0x00000001);
                  }),
             "a stream 0 stride difference under only a stream 1 mask is consumed");

    if (!h.userPointer)
    {
        // The IndexedBuffer entry compares G+0x30F0 as it is, top byte included.
        const auto topByte = [&](std::uint32_t streamValue, std::uint32_t mask)
        {
            return both(tagged,
                        [=](Memory& memory)
                        {
                            set(memory, kRecord0 + 8, 0xAB000000);
                            set(memory, kStream, streamValue);
                            set(memory, kMask, mask);
                        });
        };
        consumed(topByte(0xAB000000, 0xFF000000), "an equal stream 0 top byte is consumed");
        fell(topByte(0xAC000000, 0xFF000000), "a stream 0 top byte that differs under its mask runs Original");
        consumed(topByte(0xAC000000, 0x00FFFFFF), "a stream 0 top byte that differs outside its mask is consumed");
    }
    else
    {
        // A wrapper's nested call stores (stride >> 2) & 0xFF in the top byte first;
        // the stride 0x1234 gives 0x8D. The cache holds the stride and G+0x30F0 another: a hit.
        const auto strided = [&](std::uint32_t recordValue, std::uint32_t streamValue, std::uint32_t mask)
        {
            return both(tagged,
                        [=](Memory& memory)
                        {
                            set(memory, kRecord0 + 8, recordValue);
                            set(memory, kStream, streamValue);
                            set(memory, kMask, mask);
                        });
        };
        constexpr std::uint32_t kStride = 0x1234;
        h.consumed(kDirty, strided(0x8D123456, 0x00123456, 0xFFFFFFFF), kStride, "the wrapper stride byte replaces G+0x30F0 in the cache comparison");
        h.fallback(kDirty, strided(0x00123456, 0x8D123456, 0xFFFFFFFF), kStride, IndexedBufferFallback::DirtyVertexFetch, "the wrapper stride byte is compared, not the byte G+0x30F0 holds");
        h.fallback(kDirty, strided(0x8D123456, 0x8D123457, 0xFFFFFFFF), kStride, IndexedBufferFallback::DirtyVertexFetch, "only the top byte of stream 0 is replaced, the other bytes of G+0x30F0 are compared");
        h.consumed(kDirty, strided(0x8D123456, 0x00123456, 0x00FFFFFF), kStride, "a wrapper stride byte outside the mask is not compared");
        h.consumed(kDirty, strided(0x8D123456, 0x0F123456, 0xFFFFFFFF), kStride, "the wrapper stride byte replaces the top byte of G+0x30F0 rather than merging with it");
        h.fallback(kDirty, strided(0x8D123456, 0x8D123456, 0xFF000000), 0x1238, IndexedBufferFallback::DirtyVertexFetch, "a different wrapper stride byte differs from the cache");
        // The stride that dirties the root also consults the cache: G+0x2E38 holds another byte.
        const FetchHarness::Adjust differing = [&](Memory& memory)
        {
            set(memory, kGraphics + 0x2E38, 0x01000000);
        };
        h.fallback(0x00020000, both(tagMiss, differing), kStride, IndexedBufferFallback::DirtyVertexFetch, "a stride-dirtied root with a fetch cache miss runs Original");
        h.consumed(0x00020000, both(tagged, differing), kStride, "a stride-dirtied root with a fetch cache hit is consumed");
    }

    // The variant selection: without a PS and with VS flag 0x20 the second record is used.
    const auto records = [&](std::uint32_t tag0, std::uint32_t tag1, std::uint32_t flags, bool pixel)
    {
        return FetchHarness::Adjust{ [=](Memory& memory)
                                     {
                                         set(memory, kTag, kTagValue);
                                         set(memory, kRecord0, tag0);
                                         set(memory, kRecord1, tag1);
                                         set(memory, kFlags, flags);
                                         if (!pixel)
                                         {
                                             set(memory, kNoPixel, 0);
                                             set(memory, kMode, 5);
                                         }
                                     } };
    };
    consumed(records(kTagValue, 0, 0, true), "with a PS record 0 is tested: a hit");
    fell(records(0, kTagValue, 0, true), "with a PS record 0 is tested: a miss");
    consumed(records(kTagValue, 0, 0x20, true), "with a PS and VS flag 0x20 record 0 is still tested: a hit");
    fell(records(0, kTagValue, 0x20, true), "with a PS and VS flag 0x20 record 0 is still tested: a miss");
    consumed(records(kTagValue, 0, 0, false), "without a PS and without VS flag 0x20 record 0 is tested: a hit");
    fell(records(0, kTagValue, 0, false), "without a PS and without VS flag 0x20 record 0 is tested: a miss");
    consumed(records(kTagValue, 0, 0x10, false), "without a PS and with other VS flags than 0x20 record 0 is tested: a hit");
    fell(records(0, kTagValue, 0x10, false), "without a PS and with other VS flags than 0x20 record 0 is tested: a miss");
    consumed(records(0, kTagValue, 0x20, false), "without a PS and with VS flag 0x20 record 1 is tested: a hit");
    fell(records(kTagValue, 0, 0x20, false), "without a PS and with VS flag 0x20 record 1 is tested: a miss");
    {
        // Record 1's stride bytes are at +456+8.
        const auto second = [&](std::uint32_t recordValue)
        {
            return both(records(kTagValue, kTagValue, 0x20, false),
                        [=](Memory& memory)
                        {
                            set(memory, kRecord1 + 12, recordValue);
                            set(memory, kMask + 4, 0xFF);
                            set(memory, kStream + 4, 0x2);
                        });
        };
        consumed(second(0x2), "record 1's stride bytes are compared: a hit");
        fell(second(0x1), "record 1's stride bytes are compared: a miss");
    }

    // The conditions without a patch admit even on a miss.
    // With no declaration the read at address 48 would miss against record 0 if it were made.
    consumed(both(word(48, kTagValue), word(kGraphics + 0x2E2C, 0)), "a root without a declaration is consumed on a cache miss");
    consumed(both(tagMiss, word(kFlags, 0x40)), "a VS with header flag 0x40 is consumed on a cache miss");
    consumed(both(tagMiss, word(kGraphics + 0x2ABC, 0x00008000)), "a root with G+0x2ABE bit 0x80 is consumed on a cache miss");
    consumed(word(kFlags, 0xFFFFFFBF), "other VS header flags do not stop the cache test: a hit is consumed");
    fell(both(tagMiss, word(kFlags, 0xFFFFFFBF)), "other VS header flags do not stop the cache test: a miss runs Original");
    fell(both(tagMiss, word(kGraphics + 0x2ABC, 0x00DF3F00)), "other G+0x2ABD and G+0x2ABE bits do not stop the cache test");

    // Unreadable words.
    constexpr std::uint32_t kUnreadable = 0x7FF0;
    const auto              unreadable  = [&](const FetchHarness::Adjust& adjust, const char* message)
    {
        h.fallback(kDirty, adjust, std::nullopt, IndexedBufferFallback::Unreadable, message);
    };
    unreadable(word(kGraphics + 0x2E2C, kUnreadable), "an unreadable declaration runs Original");
    unreadable(word(kGraphics + 0x2E2C, 0x7FD0), "an unreadable declaration tag runs Original");
    unreadable(word(kGraphics + 0x2E2C, 0x7FDC), "a declaration whose tag lies past the readable range runs Original");
    unreadable(word(kGraphics + 0x3198, kUnreadable), "an unreadable VS header flag word runs Original");
    unreadable(word(kGraphics + 0x3198, 0x7E20), "an unreadable VS header flag word runs Original though its cache records are readable");
    // Word reads do not wrap.
    unreadable(word(kGraphics + 0x2E2C, 0xFFFFFFFE), "a declaration at the end of the address space runs Original");
    unreadable(word(kGraphics + 0x3198, 0xFFFFFFFE), "a VS at the end of the address space runs Original");

    // The existing guards are reported first.
    h.fallback(kDirty, both(tagMiss, word(kGraphics + 0x2ABC, 0x00004000)), std::nullopt, IndexedBufferFallback::DirtyRecorded, "G+0x2ABE bit 0x40 is reported before a fetch cache miss");
    h.fallback(kDirty, both(tagMiss, both(word(kNoPixel, 0), word(kMode, 4))), std::nullopt, IndexedBufferFallback::DirtyMode, "a mode toggle is reported before a fetch cache miss");
    h.fallback(kDirty, both(tagMiss, both(word(kGraphics + 0x2ABC, 0x40000000), word(kFlags, 0x20))), std::nullopt, IndexedBufferFallback::DirtyZPrepass, "the Z prepass guard is reported before a fetch cache miss");

    // Without dirty bit 0x80000 the cache is not consulted.
    h.consumed(0x00020000, tagMiss, std::nullopt, "without dirty bit 0x80000 a fetch cache miss is consumed");
    h.consumed(0x00020000, word(kGraphics + 0x2E2C, kUnreadable), std::nullopt, "without dirty bit 0x80000 the declaration is not read");
    h.consumed(0x00100000, both(tagMiss, word(kMode, 0xFFFFFFFC)), std::nullopt, "dirty bit 0x100000 alone does not consult the fetch cache");
    // Other dirty bits do not hide the cache test.
    h.fallback(0x001E0000, both(tagMiss, word(kMode, 4)), std::nullopt, IndexedBufferFallback::DirtyVertexFetch, "dirty bit 0x80000 among others consults the fetch cache");
}

// Stack/device reach of at least 0x400 bytes distinguishes requireFrameEdges
// from stack_overlap.
template <typename Consumed>
void requireStackEdges(const char* name, std::uint32_t g, std::uint32_t span, std::uint32_t above, Consumed&& consumed)
{
    const auto expect = [&](std::uint32_t limit, std::uint32_t r1, bool want, const char* what)
    {
        const auto message = std::string(name) + ": " + what + std::format(" (limit {:#x}, r1 {:#x})", limit, r1);
        UNSCOPED_INFO(message.c_str());
        REQUIRE(consumed(limit, r1) == want);
    };
    UNSCOPED_INFO("the fixture device leaves room below it for the edge cases");
    REQUIRE(g > above + 0x400);
    expect(g - above - 0x400, g - above, true, "a reach ending exactly at G is consumed");
    expect(g - above - 0x400, g - above + 1, false, "a reach one byte into the device words is refused");
    expect(g + span, g + span + 0x400, true, "a reach starting exactly at the end of the device words is consumed");
    expect(g + span - 1, g + span + 0x400, false, "a reach starting on the last device byte is refused");
    expect(g + 0x20, g + 0x600, false, "a stack inside the device words is refused");
    expect(g - 0x400, g + span + 0x400, false, "a stack over the device words is refused");
}

// The no-overflow domain (stack_guard.h): the bytes between the limit and r1 must hold the entry function's own
// frame, `frame`. Exactly the frame is consumed, one byte short is refused, and r1 at the limit (the prologue
// saves would store below the limit) is refused, with no other word in the reach.
template <typename Consumed>
void requireFrameEdges(const char* name, std::uint32_t frame, Consumed&& consumed)
{
    const auto expect = [&](std::uint32_t limit, bool want, const char* what)
    {
        const auto message = std::string(name) + ": " + what + std::format(" (limit {:#x}, r1 {:#x})", limit, kStack);
        UNSCOPED_INFO(message.c_str());
        REQUIRE(consumed(limit, kStack) == want);
    };
    expect(kStack - frame, true, "exactly the frame between the limit and r1 is consumed");
    expect(kStack - frame + 1, false, "one byte short of the frame is refused");
    expect(kStack - 4, false, "a word between the limit and r1 is refused");
    expect(kStack, false, "r1 at the limit is refused");
}

// The edges of the reach against one pointed object [address, address + bytes): the reach ending where it starts
// is clear, one byte further is refused, and the reach starting where it ends is clear, one byte earlier refused.
// The reach is the entry function's `frame` bytes below r1, the least the call may have.
template <typename Consumed>
void requireObjectEdges(const char* name, std::uint32_t address, std::uint32_t bytes, std::uint32_t above, std::uint32_t frame, Consumed&& consumed)
{
    const auto expect = [&](std::uint32_t limit, std::uint32_t r1, bool want, const char* what)
    {
        const auto message = std::string(name) + ": " + what + std::format(" (limit {:#x}, r1 {:#x})", limit, r1);
        UNSCOPED_INFO(message.c_str());
        REQUIRE(consumed(limit, r1) == want);
    };
    expect(address - above - frame, address - above, true, "a reach ending at the object is clear");
    expect(address - above - frame, address - above + 1, false, "a reach one byte into the object is refused");
    expect(address + bytes, address + bytes + frame, true, "a reach starting after the object is clear");
    expect(address + bytes - 1, address + bytes + frame, false, "a reach starting on the object's last byte is refused");
}

std::vector<std::uint8_t> dirtyExit(std::vector<std::uint8_t> memory, bool rearm)
{
    for (std::uint32_t offset = 0; offset < 40; offset += 4)
        store(memory.data(), kGraphics + offset, 0);
    if (rearm)
        store(memory.data(), kGraphics + 28, 1);
    return memory;
}

void placeReservation(std::uint8_t* memory, std::uint32_t g = kGraphics)
{
    store(memory, g + 0x34, 0x100000);
    store(memory, g + 0x38, 0x100000);
    store(memory, g + 0x34CC, 0x7C00);
    store(memory, 0x7C00 + 152, 0x20000);
    store(memory, 0x7C00 + 156, 0x20000);
    store(memory, g + 0x3A58, 0x100000);
    store(memory, g + 0x33CC, 0x100000);
}

struct DirtyReplayRun
{
    bool                                                 consumed = false;
    std::vector<std::uint8_t>                            before, after;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;
#if REREVVED_DEVELOPER
    IndexedBufferConsumeCounts buffer;
    UserPointerConsumeCounts   up;
#endif
};

thread_local std::optional<std::uint32_t>                         dirtyUnreadable, dirtyUnwritable;
thread_local std::vector<std::pair<std::uint32_t, std::uint32_t>> dirtyStores;

// Drive the real consume without calling the fixture Original on refusal.
DirtyReplayRun runDirtyReplay(NativeDrawHook hook, const std::function<void(std::vector<std::uint8_t>&)>& adjust = {}, bool noWriter = false)
{
    const DrawConsumeCvars cvars(true);
    ringStubsValidatedDevice.store(kGraphics);
    ringStubsActive.store(true);
    DEV_DO(TakeIndexedBufferConsumeCounts());
    DEV_DO(TakeUserPointerConsumeCounts());
    auto config = skipConfig();
    config.read = +[](const std::uint8_t* base, std::uint32_t address, std::uint32_t& value)
    {
        return (!dirtyUnreadable || address != *dirtyUnreadable) && readFixture(base, address, value);
    };
    config.writable = +[](const std::uint8_t* base, std::uint32_t address, std::uint32_t bytes)
    {
        return (!dirtyUnwritable || !(address <= *dirtyUnwritable && std::uint64_t{ address } + bytes > *dirtyUnwritable)) && writableFixture(base, address, bytes);
    };
    config.write = +[](const std::uint8_t* base, std::uint32_t address, std::uint32_t value)
    {
        dirtyStores.emplace_back(address, value);
        return writeFixture(base, address, value);
    };
    if (noWriter)
        config.write = nullptr;
    ScopedDispatcher dispatcher(std::move(config));
    DirtyReplayRun   run;
    run.after = guestMemory();
    placeReservation(run.after.data());
    store(run.after.data(), kGraphics + 0x30, 0x6000);
    store(run.after.data(), kGraphics + 0x2E38, 4U << 24);
    store(run.after.data(), kStack + 84, 16);
    if (adjust)
        adjust(run.after);
    run.before = run.after;
    auto ctx   = entry(0x8250C768);
    ctx.r5.u64 = 3;
    if (hook == NativeDrawHook::NonindexedUp)
        ctx.r7.u64 = 16;
    dirtyStores.clear();
    install(dispatcher);
    {
        const NativeDrawScope scope(hook, ctx, run.after.data());
        run.consumed = hook == NativeDrawHook::IndexedBuffer ? ConsumeIndexedBuffer(scope, ctx, false) : ConsumeUserPointer(hook == NativeDrawHook::IndexedUp ? UserPointerWrapper::IndexedUp : UserPointerWrapper::NonindexedUp, scope, ctx, false);
    }
    remove();
    run.writes = dirtyStores;
#if REREVVED_DEVELOPER
    run.buffer = TakeIndexedBufferConsumeCounts();
    run.up     = TakeUserPointerConsumeCounts();
#endif
    ringStubsActive.store(false);
    ringStubsValidatedDevice.store(0);
    return run;
}

void testDrawDirtyReplay()
{
#if REREVVED_DEVELOPER
    IndexedBufferConsumeCounts totalBuffer;
    UserPointerConsumeCounts   totalUp;
#endif

    struct Witness
    {
        std::uint32_t offset;
        std::uint64_t bits;
    };

    constexpr std::array<Witness, 6> witnesses{ { { 0, 0x8000000000000001ULL }, { 8, 0x8000000000000001ULL }, { 16, 0x800 }, { 24, 0x0040000000000000ULL }, { 32, 0x2000000000ULL }, { 16, 0x80000 } } };
    for (const auto hook : { NativeDrawHook::IndexedBuffer, NativeDrawHook::IndexedUp, NativeDrawHook::NonindexedUp })
    {
        for (unsigned gate = 0; gate < 4; ++gate)
        {
            for (const auto witness : witnesses)
            {
                const auto run = runDirtyReplay(hook, [&](std::vector<std::uint8_t>& memory)
                                                {
                                                    store(memory.data(), kGraphics + witness.offset, static_cast<std::uint32_t>(witness.bits >> 32));
                                                    store(memory.data(), kGraphics + witness.offset + 4, static_cast<std::uint32_t>(witness.bits));
                                                    if (witness.bits == 0x80000)
                                                        store(memory.data(), kGraphics + 0x3198, 0);
                                                    if (gate & 1)
                                                    {
                                                        store(memory.data(), kGraphics + 0x2ABC, 0x21000000);
                                                        store(memory.data(), kGraphics + 0x33B0, 1);
                                                    }
                                                    if (gate & 2)
                                                        store(memory.data(), kGraphics + 0x14, load(memory.data(), kGraphics + 0x14) | 0x20000);
                                                });
#if REREVVED_DEVELOPER
                totalBuffer.consumed += run.buffer.consumed;
                totalBuffer.consumedTiled += run.buffer.consumedTiled;
                totalBuffer.consumedDirty += run.buffer.consumedDirty;
                totalUp.consumedTiled += run.up.consumedTiled;
                for (std::size_t slot = 0; slot < 2; ++slot)
                {
                    totalUp.consumed[slot] += run.up.consumed[slot];
                    totalUp.fallback[slot][static_cast<std::size_t>(UserPointerFallback::Reservation)] += run.up.fallback[slot][static_cast<std::size_t>(UserPointerFallback::Reservation)];
                }
#endif
                const bool unknown = hook != NativeDrawHook::IndexedBuffer && (gate & 2);
                UNSCOPED_INFO("dirty replay: each family/gate/witness has the established outcome");
                REQUIRE(run.consumed != unknown);
                if (unknown)
                {
                    UNSCOPED_INFO("an unknown UP reservation refuses without partial stores");
                    REQUIRE((run.after == run.before && run.writes.empty()));
#if REREVVED_DEVELOPER
                    const auto slot = hook == NativeDrawHook::IndexedUp ? 0 : 1;
                    UNSCOPED_INFO("unknown reservation has its own census reason");
                    REQUIRE(run.up.fallback[slot][static_cast<std::size_t>(UserPointerFallback::Reservation)] == 1);
#endif
                    continue;
                }
                const bool up = hook != NativeDrawHook::IndexedBuffer;
                UNSCOPED_INFO("dirty replay leaves exactly Original's five qwords and preserves all other bytes");
                REQUIRE(run.after == dirtyExit(run.before, up));
                std::vector<std::pair<std::uint32_t, std::uint32_t>> expected;
                for (std::uint32_t offset = 0; offset < 40; offset += 4)
                    expected.emplace_back(kGraphics + offset, 0);
                if (up)
                {
                    expected.emplace_back(kGraphics + 24, 0);
                    expected.emplace_back(kGraphics + 28, 1);
                }
                UNSCOPED_INFO("dirty replay stores both halves in Original order before the UP rearm");
                REQUIRE(run.writes == expected);
#if REREVVED_DEVELOPER
                if (up)
                {
                    const auto slot = hook == NativeDrawHook::IndexedUp ? 0 : 1;
                    UNSCOPED_INFO("UP replay preserves family and tiled census counts");
                    REQUIRE((run.up.consumed[slot] == 1 && run.up.consumedTiled == (gate & 1)));
                }
                else
                {
                    UNSCOPED_INFO("IndexedBuffer replay preserves census subsets");
                    REQUIRE((run.buffer.consumed == 1 && run.buffer.consumedTiled == (gate & 1) && run.buffer.consumedDirty == ((gate & 2) || witness.bits == 0x80000)));
                }
#endif
            }
        }
    }
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("the dirty replay matrix census covers every family/gate/witness once");
    REQUIRE((totalBuffer.consumed == 24 && totalBuffer.consumedTiled == 12 && totalBuffer.consumedDirty == 14 &&
             totalUp.consumed[0] == 12 && totalUp.consumed[1] == 12 && totalUp.consumedTiled == 12 &&
             totalUp.fallback[0][static_cast<std::size_t>(UserPointerFallback::Reservation)] == 12 &&
             totalUp.fallback[1][static_cast<std::size_t>(UserPointerFallback::Reservation)] == 12));
    std::cout << "dirty replay census:" << FormatIndexedBufferConsumeCounts(totalBuffer) << FormatUserPointerConsumeCounts(totalUp) << '\n';
#endif
}

void testDrawDirtyPreflight()
{
    for (const auto hook : { NativeDrawHook::IndexedBuffer, NativeDrawHook::IndexedUp, NativeDrawHook::NonindexedUp })
    {
        for (std::uint32_t offset = 0; offset < 40; offset += 4)
        {
            for (const bool unreadable : { false, true })
            {
                (unreadable ? dirtyUnreadable : dirtyUnwritable) = kGraphics + offset;
                const auto run                                   = runDirtyReplay(hook);
                dirtyUnreadable.reset();
                dirtyUnwritable.reset();
                UNSCOPED_INFO("every failed dirty destination preflight refuses before the first store");
                REQUIRE((!run.consumed && run.after == run.before && run.writes.empty()));
            }
        }
        const auto noWriter = runDirtyReplay(hook, {}, true);
        UNSCOPED_INFO("a missing writer refuses without dirty stores");
        REQUIRE((!noWriter.consumed && noWriter.after == noWriter.before && noWriter.writes.empty()));
        for (const auto offset : { 0x34U, 0x38U })
        {
            if (hook == NativeDrawHook::IndexedBuffer)
                continue;
            const auto run = runDirtyReplay(hook, [=](std::vector<std::uint8_t>& memory)
                                            {
                                                store(memory.data(), kGraphics + offset, 0x6000);
                                                store(memory.data(), kGraphics + 0x14, 0x800);
                                            });
            UNSCOPED_INFO("a dirty flush without ring capacity refuses the UP reservation");
            REQUIRE((!run.consumed && run.after == run.before && run.writes.empty()));
        }
    }
    for (const auto hook : { NativeDrawHook::IndexedUp, NativeDrawHook::NonindexedUp })
    {
        // G18 bit 0 costs IndexedUp 12 words; NonindexedUp clears it at 0x826A2BA8.
        for (const auto offset : { 0x34U, 0x38U })
        {
            const auto boundary = runDirtyReplay(hook, [=](std::vector<std::uint8_t>& memory)
                                                 {
                                                     store(memory.data(), kGraphics + 0x1C, 1);
                                                     store(memory.data(), kGraphics + offset, 0x6000 + (offset == 0x34 ? 48 : 0));
                                                 });
            UNSCOPED_INFO("only NonindexedUp removes incoming G18 bit 0 from the flush bound at either ring limit");
            REQUIRE(boundary.consumed == (hook == NativeDrawHook::NonindexedUp));
            UNSCOPED_INFO("a ring boundary preserves the exact dirty replay outcome");
            REQUIRE(boundary.after == (boundary.consumed ? dirtyExit(boundary.before, true) : boundary.before));
        }
        // ADD88's null-VS return leaves G10 available to AC8C8's mask test.
        for (const auto targets : { 0U, 16U, 17U })
        {
            for (const bool fits : { false, true })
            {
                const auto masked = runDirtyReplay(hook, [=](std::vector<std::uint8_t>& memory)
                                                   {
                                                       store(memory.data(), kGraphics + 0x14, 0x80000);
                                                       store(memory.data(), kGraphics + 0x2C, 0x80000);
                                                       store(memory.data(), kGraphics + 0x3198, 0);
                                                       store(memory.data(), kGraphics + 0x31CC, targets);
                                                       store(memory.data(), kGraphics + 0x38, 0x6000 + 4 * (32 + 5 * targets) - (fits ? 0 : 1));
                                                   });
                UNSCOPED_INFO("a null-VS shader branch still proves AC8C8 capacity and target bounds");
                REQUIRE(masked.consumed == (fits && targets <= 16));
                UNSCOPED_INFO("the null-VS branch preserves exact replay or refuses before stores");
                REQUIRE(masked.after == (masked.consumed ? dirtyExit(masked.before, true) : masked.before));
            }
        }
        const auto failure = runDirtyReplay(hook, [](std::vector<std::uint8_t>& memory)
                                            {
                                                store(memory.data(), kGraphics + 0x2ABC, 0x00200000);
                                                store(memory.data(), kGraphics + 0x1C, 1);
                                            });
        UNSCOPED_INFO("UP allocation failure stays Original and does not guess the success rearm");
        REQUIRE((!failure.consumed && failure.after == failure.before && failure.writes.empty()));
        const auto secondTake = runDirtyReplay(hook, [](std::vector<std::uint8_t>& memory)
                                               {
                                                   store(memory.data(), kGraphics + 0x3A54, 0);
                                                   store(memory.data(), kGraphics + 0x3A58, 48);
                                               });
        UNSCOPED_INFO("IndexedUp proves both reservations; NonindexedUp takes only one");
        REQUIRE(secondTake.consumed == (hook == NativeDrawHook::NonindexedUp));
        if (!secondTake.consumed)
        {
            UNSCOPED_INFO("a failed second reservation proof leaves no first-take stores");
            REQUIRE((secondTake.writes.empty() && secondTake.after == secondTake.before));
        }
        const auto tileGrowth = runDirtyReplay(hook, [](std::vector<std::uint8_t>& memory)
                                               {
                                                   store(memory.data(), kGraphics + 0x2ABC, 0x21000000);
                                                   store(memory.data(), kGraphics + 0x33B0, 1);
                                                   store(memory.data(), kGraphics + 0x33C8, 0x6400);
                                                   store(memory.data(), kGraphics + 0x33CC, 0x640F);
                                               });
        UNSCOPED_INFO("unknown tile-list growth refuses before dirty replay");
        REQUIRE((!tileGrowth.consumed && tileGrowth.writes.empty() && tileGrowth.after == tileGrowth.before));
        const auto nullPool = [](std::vector<std::uint8_t>& memory)
        {
            store(memory.data(), kGraphics + 0x34CC, 0);
            store(memory.data(), kGraphics + 0x2A90, 0x5800);
            store(memory.data(), kGraphics + 0x3A44, 0x6200);
            store(memory.data(), kGraphics + 0x3A3C, 0x6800);
            store(memory.data(), kGraphics + 0x3A48, 0);
            store(memory.data(), 0x5804, 0);
        };
        const auto nullSuccess = runDirtyReplay(hook, nullPool);
        UNSCOPED_INFO("a proved null-pool reservation rearms G18 exactly");
        REQUIRE((nullSuccess.consumed && nullSuccess.after == dirtyExit(nullSuccess.before, true)));
        const auto nullWait = runDirtyReplay(hook, [&](std::vector<std::uint8_t>& memory)
                                             {
                                                 nullPool(memory);
                                                 store(memory.data(), 0x5804, 2);
                                             });
        UNSCOPED_INFO("a null-pool wait stays Original without partial dirty stores");
        REQUIRE((!nullWait.consumed && nullWait.writes.empty() && nullWait.after == nullWait.before));
    }
}

// Bounded instruction replay of the retained ACC60 fast/AC610 retry and C400
// descriptor arms (.117.cpp:12779..12805, .88.cpp:AC6B0, .85.cpp:13024..13039).
// Generated Originals are not linked into this fixture.
void retainedDirtyReaders(std::vector<std::uint8_t>& memory, bool tight, bool grow, unsigned& retries, unsigned& growth)
{
    auto* base   = memory.data();
    auto  cursor = load(base, kGraphics + 0x30);
    store(base, kGraphics + 0x34, cursor + (tight ? 4 : 12));
    const auto emit = [&](std::uint32_t offset, std::uint64_t mask, std::uint32_t header, std::uint32_t shadow)
    {
        const auto bits = (std::uint64_t{ load(base, kGraphics + offset) } << 32) | load(base, kGraphics + offset + 4);
        if (!(bits & mask))
            return;
        if (cursor + 4 >= load(base, kGraphics + 0x34))
        {
            ++retries;
            cursor = 0x6100;
        }
        store(base, cursor + 4, header);
        store(base, cursor + 8, load(base, kGraphics + shadow));
        cursor += 8;
    };
    emit(16, 0x800, 0x2200, 0x2934);
    emit(24, 0x0040000000000000ULL, 0x2280, 0x2964);
    emit(32, 0x2000000000ULL, 0x2300, 0x29B8);
    store(base, kGraphics + 0x30, cursor);
    const auto segment = load(base, kGraphics + 0x3A50);
    auto       list    = load(base, kGraphics + 0x3440);
    store(base, kGraphics + 0x3444, list + (grow ? 0 : 8));
    if (std::uint64_t{ list } + 8 > load(base, kGraphics + 0x3444))
    {
        ++growth;
        list = 0x6500;
    }
    const auto translated = (segment & 0x1FFFFFFF) + ((((segment << 12 | segment >> 20) & 0xFFF) + 0x200) & 0x1000);
    store(base, list, 0x81000000 | ((cursor + 4 - segment) >> 2));
    store(base, list + 4, translated);
    store(base, kGraphics + 0x3440, list + 8);
}

void testDrawDirtyRetainedReaders()
{
    for (const auto hook : { NativeDrawHook::IndexedBuffer, NativeDrawHook::IndexedUp, NativeDrawHook::NonindexedUp })
    {
        for (const auto witness : { 16U, 24U, 32U })
        {
            for (const bool tight : { false, true })
            {
                for (const bool grow : { false, true })
                {
                    const auto run = runDirtyReplay(hook, [=](std::vector<std::uint8_t>& memory)
                                                    {
                                                        store(memory.data(), kGraphics + witness, witness == 24 ? 0x00400000 : witness == 32 ? 0x20
                                                                                                                                             : 0);
                                                        store(memory.data(), kGraphics + witness + 4, witness == 16 ? 0x800 : 0);
                                                        store(memory.data(), kGraphics + 0x3A50, 0x5F00);
                                                        store(memory.data(), kGraphics + 0x3440, 0x6400);
                                                    });
                    UNSCOPED_INFO("retained reader witness is consumed");
                    REQUIRE(run.consumed);
                    auto     actual = run.after, expected = dirtyExit(run.before, hook != NativeDrawHook::IndexedBuffer), sticky = run.before;
                    unsigned retry = 0, growth = 0, expectedRetry = 0, expectedGrowth = 0, stickyRetry = 0, stickyGrowth = 0;
                    retainedDirtyReaders(actual, tight, grow, retry, growth);
                    retainedDirtyReaders(expected, tight, grow, expectedRetry, expectedGrowth);
                    retainedDirtyReaders(sticky, tight, grow, stickyRetry, stickyGrowth);
                    UNSCOPED_INFO("retained ACC60/C400 sees Original's cursor and record length after replay");
                    REQUIRE((actual == expected && retry == 0 && growth == static_cast<unsigned>(grow)));
                    UNSCOPED_INFO("the exact C400 record length stays 65 dwords");
                    REQUIRE((load(actual.data(), kGraphics + 0x30) == 0x6000 && load(actual.data(), (grow ? 0x6500 : 0x6400)) == 0x81000041));
                    UNSCOPED_INFO("the sticky counterfactual exercises the extra ACC60 write or insufficient-space retry");
                    REQUIRE((load(sticky.data(), kGraphics + 0x30) != 0x6000 && stickyRetry == static_cast<unsigned>(tight)));
                }
            }
        }
    }
}

void testDrawDirtyLaterOriginal()
{
    const auto run = runDirtyReplay(NativeDrawHook::IndexedBuffer, [](std::vector<std::uint8_t>& memory)
                                    {
                                        store(memory.data(), kGraphics + 0x14, 0x80000);
                                        store(memory.data(), kGraphics + 0x2C, 0x80000);
                                        store(memory.data(), kGraphics + 0x38, 0x5FFC);
                                    });
    UNSCOPED_INFO("IndexedBuffer consumes the bit80000 retained-reader witness");
    REQUIRE(run.consumed);
    // AE558..AE5B4 compatible-hit publication; AC8F0 tests overflow before
    // special dirty bits. A later independent setter must still activate both.
    const auto original = [](std::vector<std::uint8_t>& memory, unsigned& flushes)
    {
        auto*      base   = memory.data();
        const auto dirty  = load(base, kGraphics + 0x14);
        auto       cursor = load(base, kGraphics + 0x30);
        if ((dirty & load(base, kGraphics + 0x2C)) && cursor > load(base, kGraphics + 0x38))
            ++flushes;
        if (dirty & 0x80000)
        {
            store(base, cursor + 4, 0xC0012700);
            store(base, cursor + 8, 0x20001000);
            store(base, cursor + 12, 4);
            store(base, 0x2000 + 40 + 24, 7);
            store(base, kGraphics + 0x30, cursor + 12);
        }
    };
    auto     cleared = run.after, sticky = run.before;
    unsigned clearedFlush = 0, stickyFlush = 0;
    original(cleared, clearedFlush);
    original(sticky, stickyFlush);
    UNSCOPED_INFO("cleared dirty state skips later compatible-hit emission, stamp and G28 overflow");
    REQUIRE((cleared == run.after && clearedFlush == 0));
    UNSCOPED_INFO("a later Original with bit80000 emits 12 bytes, stamps the selected record and enters the guarded overflow");
    REQUIRE((load(sticky.data(), kGraphics + 0x30) == 0x600C && load(sticky.data(), 0x2040) == 7 && stickyFlush == 1));
    store(cleared.data(), kGraphics + 0x14, 0x80000);
    original(cleared, clearedFlush);
    UNSCOPED_INFO("an independent dirty setter still reaches the Original compatible-hit and overflow arms");
    REQUIRE((cleared == sticky && clearedFlush == 1));
}

void testConsumeIndexedBuffer()
{
    constexpr std::uint32_t    kCaller = 0x8250C768, kEdge = 0x7F00;
    std::atomic<std::uint64_t> began{ 0 }, recorded{ 0 };
    auto                       config = skipConfig();
    config.write                      = writeFixture;
    config.writable                   = writableFixture;
    config.began                      = [&](const NativeDrawSubmission&)
    {
        ++began;
    };
    config.recorded = [&](const NativeDrawSubmission&, std::optional<NativeDrawReason>)
    {
        ++recorded;
    };
    ScopedDispatcher dispatcher(std::move(config));

    struct Run
    {
        std::uint64_t             originals = 0, began = 0, recorded = 0, replayWrites = 0;
        PPCContext                ctx{};
        std::vector<std::uint8_t> before, after;
    };

    // One IndexedBuffer root with the dispatcher installed or not.
    const auto draw = [&](auto&& prepare, bool observed = true, std::uint32_t graphics = kGraphics, std::uint32_t r1 = kStack)
    {
        Run run;
        run.after = guestMemory();
        placeStack(run.after.data(), r1);
        prepare(run.after);
        run.before           = run.after;
        run.ctx              = entry(kCaller);
        run.ctx.r1.u64       = r1;
        run.ctx.r3.u64       = graphics;
        const auto originals = bufferOriginals.load(), seenBegan = began.load(), seenRecorded = recorded.load(), seenWrites = fixtureWrites.load();
        if (observed)
            install(dispatcher);
        sub_826A39F8(run.ctx, run.after.data());
        if (observed)
            remove();
        run.originals    = bufferOriginals - originals;
        run.began        = began - seenBegan;
        run.recorded     = recorded - seenRecorded;
        run.replayWrites = fixtureWrites - seenWrites;
        return run;
    };
    const auto none = [](std::vector<std::uint8_t>&)
    {
    };
    // Original ran once, nothing was consumed, and `reason` is the only fallback counted.
    const auto requireFallback = [](const Run& run, IndexedBufferFallback reason, const char* message)
    {
#if REREVVED_DEVELOPER
        const auto    counts = TakeIndexedBufferConsumeCounts();
        std::uint64_t others = 0;
        for (std::size_t i = 0; i < kIndexedBufferFallbackCount; ++i)
            if (i != static_cast<std::size_t>(reason))
                others += counts.fallback[i];
#endif
        UNSCOPED_INFO(message);
        REQUIRE((run.originals == 1 && run.replayWrites == 0 && DEV_CHECK(counts.consumed == 0) && DEV_CHECK(counts.fallback[static_cast<std::size_t>(reason)] == 1) && DEV_CHECK(others == 0)));
    };
    // Original was skipped and the root counted once, in the given subsets.
    const auto requireConsumed = [](const Run& run, bool tiled, bool dirty, const char* message)
    {
#if REREVVED_DEVELOPER
        const auto    counts = TakeIndexedBufferConsumeCounts();
        std::uint64_t fell   = 0;
        for (const auto count : counts.fallback)
            fell += count;
#endif
        UNSCOPED_INFO(message);
        REQUIRE((run.originals == 0 && DEV_CHECK(counts.consumed == 1) && DEV_CHECK(counts.consumedTiled == (tiled ? 1U : 0U)) && DEV_CHECK(counts.consumedDirty == (dirty ? 1U : 0U)) && DEV_CHECK(counts.consumedDirtyVertexFetch == 0) && DEV_CHECK(fell == 0)));
    };
    // Original was skipped for a dirty root the dirty vertex fetch cvar admitted: counted once as dirty and as dirty vertex fetch.
    const auto requireConsumedFetch = [](const Run& run, const char* message)
    {
#if REREVVED_DEVELOPER
        const auto    counts = TakeIndexedBufferConsumeCounts();
        std::uint64_t fell   = 0;
        for (const auto count : counts.fallback)
            fell += count;
#endif
        UNSCOPED_INFO(message);
        REQUIRE((run.originals == 0 && DEV_CHECK(counts.consumed == 1) && DEV_CHECK(counts.consumedTiled == 0) && DEV_CHECK(counts.consumedDirty == 1) && DEV_CHECK(counts.consumedDirtyVertexFetch == 1) && DEV_CHECK(fell == 0)));
        UNSCOPED_INFO("a root admitted with a pending vertex fetch patch replays only the dirty qwords");
        REQUIRE(run.after == dirtyExit(run.before, false));
    };

    // The tiled and dirty consumes and the wrappers start off, whatever their defaults.
    const DrawConsumeCvars cvars;
    REXCVAR_SET(native_consume_indexed_buffer, true);
    ringStubsValidatedDevice.store(kGraphics);
    ringStubsActive.store(true);
    DEV_DO(TakeIndexedBufferConsumeCounts());

    // Dirty qwords are cleared in full; the cursor stays.
    const auto numbered = dispatcher.NumberedThrough();
    const auto consumed = draw([](std::vector<std::uint8_t>& memory)
                               {
                                   store(memory.data(), kGraphics + 0x00, 0x00000001);
                                   store(memory.data(), kGraphics + 0x14, 0x00000401);
                                   store(memory.data(), kGraphics + 0x30, 0x5000);
                               });
#if REREVVED_DEVELOPER
    const auto    counts = TakeIndexedBufferConsumeCounts();
    std::uint64_t fell   = 0;
    for (const auto count : counts.fallback)
        fell += count;
#endif
    UNSCOPED_INFO("an admitted root skips Original and counts as consumed");
    REQUIRE((consumed.originals == 0 && DEV_CHECK(counts.consumed == 1) && DEV_CHECK(fell == 0)));
    UNSCOPED_INFO("a consumed root replays only the dirty qwords and no register");
    REQUIRE((consumed.after == dirtyExit(consumed.before, false) && sameRegisters(consumed.ctx, entry(kCaller))));
    UNSCOPED_INFO("a consumed root is still observed, numbered and recorded");
    REQUIRE((consumed.began == 1 && consumed.recorded == 1 && dispatcher.NumberedThrough() == numbered + 1));

    // The thread's stack reach [limit, r1) is tested against the device words and the objects the model reads
    // after the stubs and before any other guest word is read.
    {
        const auto root = [](std::vector<std::uint8_t>& memory)
        {
            store(memory.data(), kGraphics + 0x00, 0x00000001);
            store(memory.data(), kGraphics + 0x14, 0x00000401);
            store(memory.data(), kGraphics + 0x30, 0x5000);
        };
        const auto consumedWith = [&](const auto& rootFn, std::uint32_t limit, std::uint32_t r1)
        {
            stackLimit     = limit;
            const auto run = draw(rootFn, true, kGraphics, r1);
            stackLimit.reset();
#if REREVVED_DEVELOPER
            const auto    counts = TakeIndexedBufferConsumeCounts();
            std::uint64_t fell = 0, others = 0;
            for (std::size_t i = 0; i < kIndexedBufferFallbackCount; ++i)
            {
                fell += counts.fallback[i];
                if (i != static_cast<std::size_t>(IndexedBufferFallback::StackOverlap))
                    others += counts.fallback[i];
            }
#endif
            if (run.originals == 0 && DEV_CHECK(counts.consumed == 1) && DEV_CHECK(fell == 0))
                return true;
#if REREVVED_DEVELOPER
            UNSCOPED_INFO(std::format("a refused stack runs Original with no replay stores and counts stack_overlap alone (limit {:#x}, r1 {:#x}, originals {}, writes {}):{}", limit, r1, run.originals, run.replayWrites, FormatIndexedBufferConsumeCounts(counts)).c_str());
#else
            UNSCOPED_INFO(std::format("a refused stack runs Original with no replay stores (limit {:#x}, r1 {:#x}, originals {}, writes {})", limit, r1, run.originals, run.replayWrites).c_str());
#endif
            REQUIRE((run.originals == 1 && run.replayWrites == 0 && DEV_CHECK(counts.consumed == 0) && DEV_CHECK(counts.fallback[static_cast<std::size_t>(IndexedBufferFallback::StackOverlap)] == 1) && DEV_CHECK(others == 0)));
            return false;
        };
        const auto consumedAt = [&](std::uint32_t limit, std::uint32_t r1)
        {
            return consumedWith(root, limit, r1);
        };
        // The footprint is pinned here, so the edges below test the constants and not themselves.
        UNSCOPED_INFO("the IndexedBuffer footprint is nothing above r1 and the device up to G+0x33B4");
        REQUIRE((kIndexedBufferStackFootprint.above == 0 && kIndexedBufferStackFootprint.deviceSpan == 0x33B4 && kIndexedBufferDeviceSpan == 0x33B4));
        requireStackEdges("IndexedBuffer", kGraphics, kIndexedBufferDeviceSpan, kIndexedBufferStackFootprint.above, consumedAt);
        UNSCOPED_INFO("the IndexedBuffer frame is the entry function's 208 bytes (g148:13093)");
        REQUIRE(kIndexedBufferStackFootprint.frame == 208);
        requireFrameEdges("IndexedBuffer", kIndexedBufferStackFootprint.frame, consumedAt);

        // The objects the device words name, moved clear of the device block: the vertex shader's flag word and
        // its two fetch records, and the declaration's fields.
        {
            constexpr std::uint32_t kVs = 0x5000, kDecl = 0x6000;
            const auto              moved = [&](std::vector<std::uint8_t>& memory)
            {
                root(memory);
                store(memory.data(), kGraphics + 0x3198, kVs);
                store(memory.data(), kGraphics + 0x2E2C, kDecl);
            };
            const auto movedAt = [&](std::uint32_t limit, std::uint32_t r1)
            {
                return consumedWith(moved, limit, r1);
            };
            UNSCOPED_INFO("the moved objects are consumed on a clear stack");
            REQUIRE(movedAt(kStack - 0x400, kStack));
            requireObjectEdges("IndexedBuffer VS flags", kVs + 872, 4, 0, kIndexedBufferStackFootprint.frame, movedAt);
            requireObjectEdges("IndexedBuffer VS record 0", kVs + 40, 24, 0, kIndexedBufferStackFootprint.frame, movedAt);
            requireObjectEdges("IndexedBuffer VS record 1", kVs + 456, 24, 0, kIndexedBufferStackFootprint.frame, movedAt);
            requireObjectEdges("IndexedBuffer declaration", kDecl + 32, 20, 0, kIndexedBufferStackFootprint.frame, movedAt);
        }

        // Unknown bounds are refused as stack_overlap.
        {
            const auto unknown = [&](const std::function<void(std::vector<std::uint8_t>&)>& edit)
            {
                const auto run = draw([&](std::vector<std::uint8_t>& memory)
                                      {
                                          root(memory);
                                          edit(memory);
                                      },
                                      true,
                                      kGraphics,
                                      kStack);
#if REREVVED_DEVELOPER
                const auto counts = TakeIndexedBufferConsumeCounts();
#endif
                return run.originals == 1 && DEV_CHECK(counts.consumed == 0) && DEV_CHECK(counts.fallback[static_cast<std::size_t>(IndexedBufferFallback::StackOverlap)] == 1);
            };
            UNSCOPED_INFO("a zero limit is refused");
            REQUIRE(unknown([](std::vector<std::uint8_t>& m)
                            {
                                store(m.data(), kPcr + 0x74, 0);
                            }));
            UNSCOPED_INFO("a base below r1 is refused");
            REQUIRE(unknown([](std::vector<std::uint8_t>& m)
                            {
                                store(m.data(), kPcr + 0x70, kStack - 0x800);
                            }));
            UNSCOPED_INFO("a limit above r1 is refused");
            REQUIRE(unknown([](std::vector<std::uint8_t>& m)
                            {
                                store(m.data(), kPcr + 0x74, kStack + 4);
                            }));
            UNSCOPED_INFO("an inverted range is refused");
            REQUIRE(unknown([](std::vector<std::uint8_t>& m)
                            {
                                store(m.data(), kPcr + 0x74, 0x90000000U);
                            }));
        }

        // The regression: Original's entry frame stores its back chain at r1 - 208 (stwu g148:13093).
        // Over the dirty word G+0x14 it changes a word the model reads, so the root is refused.
        constexpr std::uint32_t r1     = kGraphics + 0x14 + 208;
        auto                    memory = guestMemory();
        root(memory);
        const auto word = load(memory.data(), kGraphics + 0x14);
        store(memory.data(), r1 - 208, r1);
        UNSCOPED_INFO("Original's back chain store changes the dirty word");
        REQUIRE(load(memory.data(), kGraphics + 0x14) != word);
        requireFallback(draw(root, true, kGraphics, r1), IndexedBufferFallback::StackOverlap, "a frame over G+0x14 runs Original");

        // The order: the dispatcher, the nesting and the stubs come first; the stack comes before
        // the tiling gate.
        requireFallback(draw(root, false, kGraphics, kGraphics + 0x40), IndexedBufferFallback::Dispatcher, "a root without a dispatcher is reported before the stack");
        ringStubsValidatedDevice.store(0);
        requireFallback(draw(root, true, kGraphics, kGraphics + 0x40), IndexedBufferFallback::Stubs, "ring stubs that have not checked the device are reported before the stack");
        ringStubsValidatedDevice.store(kGraphics);
        requireFallback(draw([&](std::vector<std::uint8_t>& memory)
                             {
                                 root(memory);
                                 store(memory.data(), kGraphics + 0x2ABC, 0x21000000);
                             },
                             true,
                             kGraphics,
                             kGraphics + 0x40),
                        IndexedBufferFallback::StackOverlap,
                        "the stack is reported before the tiling gate");

        // Device blocks ending at 2^32 wrap; one address before does not.
        constexpr std::uint32_t kFirstWrapping = 0xFFFFFFFFU - 0x33B4U + 1;
        static_assert(kFirstWrapping == 0xFFFFCC4C && kIndexedBufferDeviceSpan == 0x33B4);
        for (const std::optional<std::uint32_t> pcr : { std::optional<std::uint32_t>{}, std::optional<std::uint32_t>{ 0 } })
        {
            pcrRegister = pcr;
            for (const std::uint32_t wrapping : { 0xFFFFFFFCU, 0xFFFFF000U, kFirstWrapping, kFirstWrapping + 4 })
            {
                ringStubsValidatedDevice.store(wrapping);
                requireFallback(draw(none, true, wrapping, kGraphics + 0x40), IndexedBufferFallback::Unreadable, "a device whose block wraps is unreadable, not a stack overlap, whatever the stack bounds");
            }
        }
        pcrRegister.reset();
        // One device before it is a device: the unknown bounds are a stack overlap, ahead of its unreadable words.
        pcrRegister = 0;
        ringStubsValidatedDevice.store(kFirstWrapping - 4);
        requireFallback(draw(none, true, kFirstWrapping - 4, kGraphics + 0x40), IndexedBufferFallback::StackOverlap, "the last device that fits is tested against the stack bounds");
        pcrRegister.reset();
        ringStubsValidatedDevice.store(kGraphics);
    }

    // The fallbacks, each from the consumed case with one condition broken.
    auto run = draw(none, false);
    requireFallback(run, IndexedBufferFallback::Dispatcher, "a root without a dispatcher runs Original");
    UNSCOPED_INFO("no dispatcher observes it");
    REQUIRE(run.began == 0);

    nestBuffer                     = true;
    const auto originalsBeforeNest = bufferOriginals.load();
    {
        auto memory = guestMemory();
        auto ctx    = entry(0x82303E90);
        install(dispatcher);
        sub_826A3568(ctx, memory.data());
        remove();
    }
    nestBuffer = false;
    UNSCOPED_INFO("a draw nested in another observed draw runs Original once");
    REQUIRE(bufferOriginals.load() - originalsBeforeNest == 1);
    {
#if REREVVED_DEVELOPER
        const auto nested = TakeIndexedBufferConsumeCounts();
        UNSCOPED_INFO("a draw nested in another observed draw runs Original");
        REQUIRE((nested.consumed == 0 && nested.fallback[static_cast<std::size_t>(IndexedBufferFallback::Nested)] == 1));
#endif
    }

    // Unpublished services: Original runs and the root is not even counted as a fallback.
    ringStubsActive.store(false);
    run = draw(none);
    {
#if REREVVED_DEVELOPER
        const auto    inert = TakeIndexedBufferConsumeCounts();
        std::uint64_t fell  = 0;
        for (const auto count : inert.fallback)
            fell += count;
#endif
        UNSCOPED_INFO("unpublished ring stubs run Original and count nothing");
        REQUIRE((run.originals == 1 && DEV_CHECK(inert.consumed == 0) && DEV_CHECK(fell == 0)));
    }
    ringStubsActive.store(true);
    ringStubsValidatedDevice.store(0);
    run = draw(none);
    requireFallback(run, IndexedBufferFallback::Stubs, "ring stubs that have not checked the device run Original");
    ringStubsValidatedDevice.store(kGraphics);

    for (const std::uint32_t tiling : { 0x21000000U, 0x01000000U, 0x20000000U })
    {
        run = draw([&](std::vector<std::uint8_t>& memory)
                   {
                       store(memory.data(), kGraphics + 0x2ABC, tiling);
                   });
        requireFallback(run, IndexedBufferFallback::Tiling, "a set tiling byte runs Original");
    }

    ringStubsValidatedDevice.store(kEdge);
    run = draw(none, true, kEdge);
    requireFallback(run, IndexedBufferFallback::Unreadable, "an unreadable tiling word runs Original");
    ringStubsValidatedDevice.store(kGraphics);

    for (const std::uint32_t dirty : { 0x00020000U, 0x00100000U, 0x001E0000U, 0xFFFFFFFFU })
    {
        run = draw([&](std::vector<std::uint8_t>& memory)
                   {
                       store(memory.data(), kGraphics + 0x14, dirty);
                   });
        requireFallback(run, IndexedBufferFallback::StateDirty, "a dirty bit of 0x1E0000 runs Original");
    }

    // native_consume_indexed_buffer_dirty: the fixture names VS 0x2000 and PS 0x2400.
    REXCVAR_SET(native_consume_indexed_buffer_dirty, true);
    const auto dirtyRoot = [&](std::uint32_t dirty, auto&& adjust)
    {
        return draw([&](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x14, dirty);
                        adjust(memory);
                    });
    };
    const auto modeControl = [](std::uint32_t value)
    {
        return [=](std::vector<std::uint8_t>& memory)
        {
            store(memory.data(), kGraphics + 0x2954, value);
        };
    };
    const auto dirtyNumbered = dispatcher.NumberedThrough();
    const auto dirtyConsumed = dirtyRoot(0x00020000,
                                         [](std::vector<std::uint8_t>& memory)
                                         {
                                             store(memory.data(), kGraphics + 0x00, 0x00000001);
                                             store(memory.data(), kGraphics + 0x30, 0x5000);
                                         });
    requireConsumed(dirtyConsumed, false, true, "a dirty root whose guards hold skips Original and counts as consumed dirty");
    UNSCOPED_INFO("a consumed dirty root replays only the dirty qwords, so its dirty qwords are zero");
    REQUIRE((dirtyConsumed.after == dirtyExit(dirtyConsumed.before, false) && sameRegisters(dirtyConsumed.ctx, entry(kCaller))));
    UNSCOPED_INFO("a consumed dirty root is still observed, numbered and recorded");
    REQUIRE((dirtyConsumed.began == 1 && dirtyConsumed.recorded == 1 && dispatcher.NumberedThrough() == dirtyNumbered + 1));

    // RB_MODECONTROL: with a PS and dirty 0x100000 the update stores mode 4.
    run = dirtyRoot(0x00100000, modeControl(0));
    requireFallback(run, IndexedBufferFallback::DirtyMode, "a PS root whose Original would store mode 4 runs Original");
    run = dirtyRoot(0x00100000, modeControl(0xFFFFFFFC));
    requireConsumed(run, false, true, "a PS root already in mode 4 is consumed whatever the other mode bits");
    run = dirtyRoot(0x00080000, modeControl(5));
    requireConsumed(run, false, true, "without dirty 0x100000 a PS root does not store the mode");
    // Without a PS the update stores mode 5 whatever the dirty bit.
    const auto noPixel = [](std::uint32_t mode)
    {
        return [=](std::vector<std::uint8_t>& memory)
        {
            store(memory.data(), kGraphics + 0x3194, 0);
            store(memory.data(), kGraphics + 0x2954, mode);
        };
    };
    run = dirtyRoot(0x00020000, noPixel(4));
    requireFallback(run, IndexedBufferFallback::DirtyMode, "a root without a PS whose Original would store mode 5 runs Original");
    run = dirtyRoot(0x00020000, noPixel(5));
    requireConsumed(run, false, true, "a root without a PS already in mode 5 is consumed");
    // Without a VS the update returns after the HIZ derive.
    run = dirtyRoot(0x001E0000,
                    [](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x3198, 0);
                        store(memory.data(), kGraphics + 0x2954, 0);
                    });
    requireConsumed(run, false, true, "a dirty root without a VS is consumed");

    // G+0x2ABE bit 0x40, reported before a mode toggle.
    run = dirtyRoot(0x00100000,
                    [](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x2ABC, 0x00004000);
                    });
    requireFallback(run, IndexedBufferFallback::DirtyRecorded, "a dirty root with G+0x2ABE bit 0x40 runs Original");
    run = dirtyRoot(0x00020000,
                    [](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x2ABC, 0x00DFBF00);
                    });
    requireConsumed(run, false, true, "other G+0x2ABD and G+0x2ABE bits are no dirty guard");

    // The Z prepass: VS header word +872 bit 0x20 selects the alternate depth-only VS.
    const auto zprepass = [](std::uint32_t flags, std::uint32_t mode)
    {
        return [=](std::vector<std::uint8_t>& memory)
        {
            store(memory.data(), kGraphics + 0x2ABC, 0x40000000);
            store(memory.data(), kGraphics + 0x2954, mode);
            store(memory.data(), 0x2000 + 872, flags);
        };
    };
    run = dirtyRoot(0x00020000, zprepass(0x20, 0));
    requireFallback(run, IndexedBufferFallback::DirtyZPrepass, "a dirty Z-prepass root with the alternate depth-only VS runs Original");
    run = dirtyRoot(0x00020000, zprepass(0xFFFFFFDF, 0));
    requireConsumed(run, false, true, "a dirty Z-prepass root without the alternate VS is consumed");
    run = dirtyRoot(0x00100000, zprepass(0x20, 0));
    requireFallback(run, IndexedBufferFallback::DirtyMode, "a mode toggle is reported before the Z prepass");
    run = draw(zprepass(0x20, 0));
    requireConsumed(run, false, false, "a clean Z-prepass root needs no dirty guard");
    run = dirtyRoot(0x00020000,
                    [](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x3198, 0x7FF0);
                    });
    requireConsumed(run, false, true, "the VS header is read only for the Z prepass");
    run = dirtyRoot(0x00020000,
                    [](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x2ABC, 0x40000000);
                        store(memory.data(), kGraphics + 0x3198, 0x7FF0);
                    });
    requireFallback(run, IndexedBufferFallback::Unreadable, "an unreadable VS header word runs Original");
    // A device whose mode word is readable and whose shader words are not.
    constexpr std::uint32_t kShaderEdge = 0x5000;
    ringStubsValidatedDevice.store(kShaderEdge);
    const auto shaderEdge = [&](std::uint32_t modes)
    {
        return draw(
            [&](std::vector<std::uint8_t>& memory)
            {
                store(memory.data(), kShaderEdge + 0x14, 0x00020000);
                store(memory.data(), kShaderEdge + 0x2ABC, modes);
            },
            true,
            kShaderEdge,
            kShaderEdge - 0x1000); // the stack stays clear of this device block (the fixture stack at 0x7000 lies in it)
    };
    run = shaderEdge(0x00004000);
    requireFallback(run, IndexedBufferFallback::DirtyRecorded, "G+0x2ABE bit 0x40 is reported before an unreadable VS word");
    run = shaderEdge(0);
    requireFallback(run, IndexedBufferFallback::Unreadable, "an unreadable VS word runs Original");
    ringStubsValidatedDevice.store(kGraphics);

    // A root with dirty bit 0x80000 consults the VS's vertex fetch cache.
    {
        FetchHarness harness;
        harness.consumed = [&](std::uint32_t dirty, const FetchHarness::Adjust& adjust, std::optional<std::uint32_t>, const char* message)
        {
            requireConsumed(dirtyRoot(dirty, adjust), false, true, message);
        };
        harness.fallback = [&](std::uint32_t dirty, const FetchHarness::Adjust& adjust, std::optional<std::uint32_t>, IndexedBufferFallback reason, const char* message)
        {
            requireFallback(dirtyRoot(dirty, adjust), reason, message);
        };
        testVertexFetchPatch(harness);

        // With native_consume_dirty_vertex_fetch the roots that would patch are consumed, and every
        // other condition of the matrix holds as before. An observer sees exactly those roots.
        {
            static std::atomic<unsigned>      observed{ 0 };
            static std::atomic<std::uint32_t> observedDevice{ 0 };
            static std::atomic<bool>          observedStride{ false };
            observed = 0;
            SetVertexFetchConsumeObserver(+[](const VertexFetchConsume& consume) noexcept
                                          {
                                              ++observed;
                                              observedDevice = consume.graphics;
                                              observedStride = consume.stride != nullptr;
                                          });
            REXCVAR_SET(native_consume_dirty_vertex_fetch, true);
            unsigned     admittedRoots = 0;
            FetchHarness enabled;
            enabled.consumed = harness.consumed;
            enabled.fallback = [&](std::uint32_t dirty, const FetchHarness::Adjust& adjust, std::optional<std::uint32_t>, IndexedBufferFallback reason, const char* message)
            {
                const auto root = dirtyRoot(dirty, adjust);
                if (reason == IndexedBufferFallback::DirtyVertexFetch)
                {
                    requireConsumedFetch(root, message);
                    ++admittedRoots;
                }
                else
                    requireFallback(root, reason, message);
            };
            testVertexFetchPatch(enabled);
            REXCVAR_SET(native_consume_dirty_vertex_fetch, false);
            SetVertexFetchConsumeObserver(nullptr);
            UNSCOPED_INFO("the observer sees each root admitted by the dirty vertex fetch cvar, with the device and no wrapper stride");
            REQUIRE((admittedRoots >= 10 && observed == admittedRoots && observedDevice == kGraphics && !observedStride));

            // The cvar admits nothing without the dirty cvar or the base cvar, and while off keeps the refusal.
            REXCVAR_SET(native_consume_dirty_vertex_fetch, true);
            const auto missRoot = [&]
            {
                return dirtyRoot(0x00080000,
                                 [](std::vector<std::uint8_t>& memory)
                                 {
                                     store(memory.data(), 0x2800 + 48, 0x1234);
                                 });
            };
            requireConsumedFetch(missRoot(), "a root that would patch is consumed with the cvar on");
            REXCVAR_SET(native_consume_indexed_buffer_dirty, false);
            requireFallback(missRoot(), IndexedBufferFallback::StateDirty, "the dirty vertex fetch cvar does not replace the dirty cvar");
            REXCVAR_SET(native_consume_indexed_buffer_dirty, true);
            REXCVAR_SET(native_consume_indexed_buffer, false);
            const auto baseOff = missRoot();
#if REREVVED_DEVELOPER
            const auto counted = TakeIndexedBufferConsumeCounts();
#endif
            UNSCOPED_INFO("with the base cvar off the dirty vertex fetch cvar counts and skips nothing");
            REQUIRE((baseOff.originals == 1 && DEV_CHECK(counted.consumed == 0) && DEV_CHECK(counted.consumedDirtyVertexFetch == 0) && DEV_CHECK(counted.fallback == decltype(counted.fallback){})));
            REXCVAR_SET(native_consume_indexed_buffer, true);
            REXCVAR_SET(native_consume_dirty_vertex_fetch, false);
            requireFallback(missRoot(), IndexedBufferFallback::DirtyVertexFetch, "with the cvar off the root that would patch runs Original");
        }

        // G+0x2ABC bit 0x80 cannot reach a consumed root past the tiling gate, so the guard is checked on its own.
        auto memory = guestMemory();
        store(memory.data(), 0x2830, 0x1234);
        const auto state = [&](std::uint32_t modes, std::optional<std::uint8_t> stream0 = std::nullopt)
        {
            store(memory.data(), kGraphics + 0x2ABC, modes);
            return CheckIndexedBufferDirtyState(readFixture, memory.data(), kGraphics, 0x00080000, stream0);
        };
        UNSCOPED_INFO("a fetch cache miss is a dirty guard");
        REQUIRE(state(0) == IndexedBufferDirtyState::VertexFetch);
        UNSCOPED_INFO("byte G+0x2ABC bit 0x80 stops the in-place patch");
        REQUIRE(state(0x80000000) == IndexedBufferDirtyState::Holds);
        UNSCOPED_INFO("byte G+0x2ABE bit 0x80 stops the in-place patch");
        REQUIRE(state(0x00008000) == IndexedBufferDirtyState::Holds);
        store(memory.data(), 0x2830, 0);
        store(memory.data(), 0x2000 + 40 + 8, 0x8D000000);
        store(memory.data(), 0x2800 + 32, 0xFF000000);
        UNSCOPED_INFO("a stream 0 byte that differs from the record is a miss");
        REQUIRE(state(0) == IndexedBufferDirtyState::VertexFetch);
        UNSCOPED_INFO("a wrapper's stream 0 byte replaces the top byte of G+0x30F0");
        REQUIRE(state(0, 0x8D) == IndexedBufferDirtyState::Holds);
        UNSCOPED_INFO("another wrapper stream 0 byte is a miss");
        REQUIRE(state(0, 0x8E) == IndexedBufferDirtyState::VertexFetch);

        // A mask word that cannot be read, with the tag, record and streams readable.
        UNSCOPED_INFO("the failing reader reads everything outside its window");
        REQUIRE(CheckIndexedBufferDirtyState(failingReader, memory.data(), kGraphics, 0x00080000) == IndexedBufferDirtyState::VertexFetch);
        for (const std::uint32_t mask : { 0x2800U + 32, 0x2800U + 36, 0x2800U + 40, 0x2800U + 44 })
        {
            failBegin = mask;
            failEnd   = mask + 4;
            UNSCOPED_INFO("an unreadable mask word runs Original");
            REQUIRE(CheckIndexedBufferDirtyState(failingReader, memory.data(), kGraphics, 0x00080000) == IndexedBufferDirtyState::Unreadable);
        }
        failBegin = failEnd = 0;
    }

    // The diagnostics come after the dirty guards.
    armShaderHarvest(true);
    run = dirtyRoot(0x00100000, modeControl(0));
    requireFallback(run, IndexedBufferFallback::DirtyMode, "a failed dirty guard is reported before diagnostics");
    run = dirtyRoot(0x00020000, none);
    armShaderHarvest(false);
    if (kDeveloperBuild)
        requireFallback(run, IndexedBufferFallback::Diagnostics, "a shader harvest runs a dirty root's Original");
    else
        requireConsumed(run, false, true, "with no recorder a dirty root is consumed");

    // With the base cvar off the dirty cvar counts and skips nothing.
    REXCVAR_SET(native_consume_indexed_buffer, false);
    run = dirtyRoot(0x00020000, none);
#if REREVVED_DEVELOPER
    const auto dirtyOff = TakeIndexedBufferConsumeCounts();
#endif
    UNSCOPED_INFO("with the base cvar off a dirty root runs Original and nothing is counted");
    REQUIRE((run.originals == 1 && DEV_CHECK(dirtyOff.consumed == 0) && DEV_CHECK(dirtyOff.consumedDirty == 0) && DEV_CHECK(dirtyOff.fallback == decltype(dirtyOff.fallback){})));
    REXCVAR_SET(native_consume_indexed_buffer, true);
    REXCVAR_SET(native_consume_indexed_buffer_dirty, false);

    armShaderHarvest(true);
    run = draw(none);
    armShaderHarvest(false);
    if (kDeveloperBuild)
        requireFallback(run, IndexedBufferFallback::Diagnostics, "a shader harvest runs Original");
    else
        requireConsumed(run, false, false, "with no recorder a clean root is consumed");

#if REREVVED_DEVELOPER
    const auto scratch = [](const char* name)
    {
        return std::filesystem::temp_directory_path() / (std::string(name) + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    };
    const auto                                    routeDirectory = scratch("rerevved-consume-route-");
    const auto                                    sceneDirectory = scratch("rerevved-consume-scene-");
    const rerevved::tests::ScopedDirectoryRemoval routeRemoval(routeDirectory);
    const rerevved::tests::ScopedDirectoryRemoval sceneRemoval(sceneDirectory);

    // Declared after the removals, so both sessions close their files before
    // the directories go.
    struct SessionEnd
    {
        ~SessionEnd()
        {
            rerevved::gpu::diagnostics::StopAndResetSceneResourceCaptureForTests();
            rerevved::gpu::diagnostics::StopAndResetRouteTraceForTests();
        }
    } sessionEnd;

    std::string error;
    UNSCOPED_INFO("start the route trace fixture");
    REQUIRE(rerevved::gpu::diagnostics::StartRouteTrace(routeDirectory, "fixture", "fixture", error));
    run = draw(none);
    requireUserPointerKeepsOriginal("a route trace runs a wrapper's Original");
    rerevved::gpu::diagnostics::StopRouteTrace();
    requireFallback(run, IndexedBufferFallback::Diagnostics, "a route trace runs Original");

    // Before its window the session gives the draw an empty token; Original must still run.
    UNSCOPED_INFO("start the scene session fixture");
    REQUIRE(rerevved::gpu::diagnostics::StartSceneResourceCapture(sceneDirectory, 2, "fixture", "fixture", error, false, 256 * 1024 * 1024, false, false, 64 * 1024 * 1024, 1000));
    run = draw(none);
    requireUserPointerKeepsOriginal("a scene session runs a wrapper's Original");
    rerevved::gpu::diagnostics::StopAndResetSceneResourceCaptureForTests();
    requireFallback(run, IndexedBufferFallback::Diagnostics, "a scene session runs Original");
#endif

#if REREVVED_DEVELOPER
    DrawEffectConfig sampling;
    sampling.interval = 1;
    sampling.perSwap  = 0;
    sampling.selector = alwaysSelect;
    DrawEffectProbe probe(sampling);
    drawEffectProbe.store(&probe);
    run = draw(none);
    drawEffectProbe.store(nullptr);
    probe.Seal();
    requireFallback(run, IndexedBufferFallback::Diagnostics, "a root the effect probe samples runs Original");
    UNSCOPED_INFO("the probe sampled that root");
    REQUIRE(probe.Counts().sampled == 1);

    // An unsampled root next to an open probe is still consumed.
    DrawEffectConfig idle;
    idle.interval = 1;
    idle.limit    = 0;
    idle.selector = alwaysSelect;
    DrawEffectProbe capped(idle);
    drawEffectProbe.store(&capped);
    run = draw(none);
    drawEffectProbe.store(nullptr);
    capped.Seal();
    const auto spent = TakeIndexedBufferConsumeCounts();
    UNSCOPED_INFO("a root the probe does not sample is consumed");
    REQUIRE((run.originals == 0 && DEV_CHECK(spent.consumed == 1)));
#endif

    // Tiled roots: exactly 0x21 behind native_consume_indexed_buffer_tiled, and
    // then the tripwires read from G at entry. A root with the tripwires satisfied.
    constexpr std::uint32_t kTiledByte = 0x21000000;
    const auto              tiledRoot  = [&](auto&& adjust, std::uint32_t graphics = kGraphics)
    {
        return draw([&](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), graphics + 0x2ABC, kTiledByte);
                        // The unreadable-tripwire case puts this word outside the fixture.
                        if (graphics + 0x33B4 <= kGuestBytes)
                            store(memory.data(), graphics + 0x33B0, 0x1234);
                        adjust(memory);
                    },
                    true,
                    graphics,
                    graphics == kGraphics ? kStack : graphics - 0x1000); // a device elsewhere keeps the stack clear of its block
    };

    // The second cvar off: the record branch is any set tiling byte.
    run = tiledRoot(none);
    requireFallback(run, IndexedBufferFallback::Tiling, "a tiled root runs Original while the tiled cvar is off");

    REXCVAR_SET(native_consume_indexed_buffer_tiled, true);
    const auto tiledNumbered = dispatcher.NumberedThrough();
    // The bracket's list header, the cursor and the dirty qwords hold values a
    // consumed call must preserve apart from the five dirty qwords.
    const auto tiledConsumed = tiledRoot([](std::vector<std::uint8_t>& memory)
                                         {
                                             store(memory.data(), kGraphics + 0x00, 0x00000001);
                                             store(memory.data(), kGraphics + 0x14, 0x00000401);
                                             store(memory.data(), kGraphics + 0x30, 0x5000);
                                             store(memory.data(), kGraphics + 0x33C0, 0x00200000);
                                             store(memory.data(), kGraphics + 0x33C4, 0x00200000);
                                             store(memory.data(), kGraphics + 0x33C8, 0x00200024);
                                             store(memory.data(), kGraphics + 0x33CC, 0x0020107C);
                                         });
#if REREVVED_DEVELOPER
    const auto    tiledCounts = TakeIndexedBufferConsumeCounts();
    std::uint64_t tiledFell   = 0;
    for (const auto count : tiledCounts.fallback)
        tiledFell += count;
#endif
    UNSCOPED_INFO("a tiled root with its tripwires satisfied skips Original and counts as consumed and as consumed tiled");
    REQUIRE((tiledConsumed.originals == 0 && DEV_CHECK(tiledCounts.consumed == 1) && DEV_CHECK(tiledCounts.consumedTiled == 1) && DEV_CHECK(tiledFell == 0)));
    UNSCOPED_INFO("a consumed tiled root replays only the dirty qwords (cursor, list header, dirty words) and no register");
    REQUIRE((tiledConsumed.after == dirtyExit(tiledConsumed.before, false) && sameRegisters(tiledConsumed.ctx, entry(kCaller))));
    UNSCOPED_INFO("a consumed tiled root is still observed, numbered and recorded");
    REQUIRE((tiledConsumed.began == 1 && tiledConsumed.recorded == 1 && dispatcher.NumberedThrough() == tiledNumbered + 1));

    // An untiled consumed root is not a tiled one.
    run = draw(none);
#if REREVVED_DEVELOPER
    const auto untiledCounts = TakeIndexedBufferConsumeCounts();
#endif
    UNSCOPED_INFO("an untiled root does not count as consumed tiled");
    REQUIRE((run.originals == 0 && DEV_CHECK(untiledCounts.consumed == 1) && DEV_CHECK(untiledCounts.consumedTiled == 0)));

    // Each tripwire broken alone.
    run = tiledRoot([](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x33B0, 0);
                    });
    requireFallback(run, IndexedBufferFallback::TiledState, "a closed bracket word G+0x33B0 runs Original");
    run = tiledRoot([](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x31AC, 1);
                    });
    requireFallback(run, IndexedBufferFallback::TiledState, "a nonzero G+0x31AC runs Original");
    run = tiledRoot([](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x2ABC, 0x21200000);
                    });
    requireFallback(run, IndexedBufferFallback::TiledState, "an allocation failure bit G+0x2ABD & 0x20 runs Original");
    // Other bits of the mode byte are not a tripwire.
    run = tiledRoot([](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x2ABC, 0x21DF0000);
                    });
    UNSCOPED_INFO("mode bits other than 0x20 do not trip");
    REQUIRE(run.originals == 0);
    DEV_DO(TakeIndexedBufferConsumeCounts());

    // Only the exact byte 0x21 is a tiled root; every other set bit stays Tiling.
    for (const std::uint32_t tiling : { 0x01000000U, 0x20000000U, 0x31000000U, 0x11000000U, 0x61000000U, 0xA1000000U })
    {
        run = tiledRoot([&](std::vector<std::uint8_t>& memory)
                        {
                            store(memory.data(), kGraphics + 0x2ABC, tiling);
                        });
        requireFallback(run, IndexedBufferFallback::Tiling, "a tiling byte other than exactly 0x21 runs Original as tiling");
    }

    // The dirty word is tested before the tripwires.
    run = tiledRoot([](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x14, 0x00020000);
                    });
    requireFallback(run, IndexedBufferFallback::StateDirty, "a dirty bit of 0x1E0000 on a tiled root runs Original");
    run = tiledRoot([](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x14, 0x00020000);
                        store(memory.data(), kGraphics + 0x33B0, 0);
                    });
    requireFallback(run, IndexedBufferFallback::StateDirty, "state_dirty is reported before a tripped tiled tripwire");

    // A dirty tiled root needs both cvars; the dirty guards come before the tripwires.
    REXCVAR_SET(native_consume_indexed_buffer_dirty, true);
    run = tiledRoot([](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x14, 0x00020000);
                    });
    requireConsumed(run, true, true, "a dirty tiled root whose guards and tripwires hold counts in both subsets");
    run = tiledRoot([](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x14, 0x00020000);
                        store(memory.data(), kGraphics + 0x33B0, 0);
                    });
    requireFallback(run, IndexedBufferFallback::TiledState, "a dirty tiled root still checks the tripwires");
    run = tiledRoot([](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x14, 0x00100000);
                        store(memory.data(), kGraphics + 0x33B0, 0);
                    });
    requireFallback(run, IndexedBufferFallback::DirtyMode, "a failed dirty guard is reported before a tripped tripwire");
    REXCVAR_SET(native_consume_indexed_buffer_dirty, false);

    // The tripwires are tested before the diagnostics.
    armShaderHarvest(true);
    run = tiledRoot([](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x33B0, 0);
                    });
    requireFallback(run, IndexedBufferFallback::TiledState, "a tripped tripwire is reported before diagnostics");
    run = tiledRoot(none);
    armShaderHarvest(false);
    if (kDeveloperBuild)
        requireFallback(run, IndexedBufferFallback::Diagnostics, "a shader harvest runs a tiled root's Original");
    else
        requireConsumed(run, true, false, "with no recorder a tiled root is consumed");

    // A tripwire word outside readable memory is unreadable, not tripped.
    constexpr std::uint32_t kTripEdge = 0x4C50;
    ringStubsValidatedDevice.store(kTripEdge);
    run = tiledRoot(none, kTripEdge);
    requireFallback(run, IndexedBufferFallback::Unreadable, "an unreadable tripwire word runs Original");
    ringStubsValidatedDevice.store(kGraphics);

#if REREVVED_DEVELOPER
    // The probe samples a tiled root and keeps its Original; the sample is tiled.
    DrawEffectProbe tiledProbe(sampling);
    drawEffectProbe.store(&tiledProbe);
    run = tiledRoot(none);
    drawEffectProbe.store(nullptr);
    tiledProbe.Seal();
    requireFallback(run, IndexedBufferFallback::Diagnostics, "a tiled root the effect probe samples runs Original");
    UNSCOPED_INFO("the probe sampled that root as tiled");
    REQUIRE((tiledProbe.Counts().tiledSampled == 1 && tiledProbe.Counts().sampled == 0));
#endif

    // With the base cvar off the tiled cvar counts and skips nothing.
    REXCVAR_SET(native_consume_indexed_buffer, false);
    run = tiledRoot(none);
#if REREVVED_DEVELOPER
    const auto tiledOff = TakeIndexedBufferConsumeCounts();
#endif
    UNSCOPED_INFO("with the base cvar off a tiled root runs Original and nothing is counted");
    REQUIRE((run.originals == 1 && DEV_CHECK(tiledOff.consumed == 0) && DEV_CHECK(tiledOff.consumedTiled == 0) && DEV_CHECK(tiledOff.fallback == decltype(tiledOff.fallback){})));
    REXCVAR_SET(native_consume_indexed_buffer, true);
    REXCVAR_SET(native_consume_indexed_buffer_tiled, false);

    // Off, nothing is skipped or counted.
    REXCVAR_SET(native_consume_indexed_buffer, false);
    run = draw(none);
#if REREVVED_DEVELOPER
    const auto    off     = TakeIndexedBufferConsumeCounts();
    std::uint64_t counted = off.consumed;
    for (const auto count : off.fallback)
        counted += count;
#endif
    UNSCOPED_INFO("with the cvar off Original runs and nothing is counted");
    REQUIRE((run.originals == 1 && DEV_CHECK(counted == 0)));

    ringStubsActive.store(false);
    ringStubsValidatedDevice.store(0);
}

// Admitted user-pointer consume preserves observation, numbering and count.
void testConsumeUserPointer()
{
    struct Wrapper
    {
        UserPointerWrapper id;
        NativeDrawHook     hook, child;
        std::uint32_t      caller, childCaller;
    };

    const std::array<Wrapper, kUserPointerWrapperCount> wrappers{ {
        { UserPointerWrapper::IndexedUp, NativeDrawHook::IndexedUp, NativeDrawHook::IndexedUpLowerer, 0x82303E90, kIndexedUpLowererReturn },
        { UserPointerWrapper::NonindexedUp, NativeDrawHook::NonindexedUp, NativeDrawHook::NonindexedSink, 0x82304258, kNonindexedSinkReturn },
    } };
    constexpr std::uint32_t                             kEdge = 0x7F00;

    struct Tally
    {
        std::array<std::uint64_t, kNativeDrawHookCount> invocations{};
        std::uint64_t                                   nested = 0, attempted = 0, mismatched = 0;

        bool operator==(const Tally&) const = default;
    };

    std::atomic<std::uint64_t> began{ 0 }, recorded{ 0 };
    NativeDrawSubmission       last;
    auto                       config = skipConfig();
    config.write                      = writeFixture;
    config.writable                   = writableFixture;
    config.began                      = [&](const NativeDrawSubmission&)
    {
        ++began;
    };
    config.recorded = [&](const NativeDrawSubmission& submission, std::optional<NativeDrawReason>)
    {
        ++recorded;
        last = submission;
    };
    ScopedDispatcher dispatcher(std::move(config));

    const auto tally = [&]
    {
        const auto totals = dispatcher.Totals();
        return Tally{ totals.invocations, totals.nested, totals.attempted, reasonCount(totals.skipped, NativeDrawReason::NestingMismatch) };
    };

    struct Run
    {
        std::uint64_t             originals = 0, childOriginals = 0, began = 0, recorded = 0, replayWrites = 0;
        Tally                     counted;
        PPCContext                ctx{};
        std::vector<std::uint8_t> before, after;
    };

    // Wrapper roots keep readable edge room; stride and r1 determine IndexedUp
    // and NonindexedUp patch inputs.
    const auto draw = [&](const Wrapper& w, auto&& prepare, bool observed = true, std::uint32_t graphics = kGraphics, std::optional<std::uint32_t> stride = std::nullopt, std::uint32_t r1 = kStack)
    {
        Run run;
        run.after = guestMemory();
        run.after.resize(kGuestBytes + 0x10000);
        placeReservation(run.after.data());
        if (graphics != kGraphics)
            placeReservation(run.after.data(), graphics);
        placeStack(run.after.data(), r1);
        run.ctx        = entry(w.caller);
        run.ctx.r1.u64 = r1;
        run.ctx.r3.u64 = graphics;
        if (stride && w.id == UserPointerWrapper::NonindexedUp)
            run.ctx.r7.u64 = *stride;
        if (stride && w.id == UserPointerWrapper::IndexedUp)
            store(run.after.data(), r1 + 84, *stride); // 260(r1) in the wrapper's 176-byte frame
        const auto passed = w.id == UserPointerWrapper::IndexedUp ? stride.value_or(0) : run.ctx.r7.u32;
        store(run.after.data(), graphics + kIndexedBufferStrideByteOffset, ((passed >> 2) & 0xFF) << 24);
        prepare(run.after);
        run.before           = run.after;
        const auto slot      = static_cast<std::size_t>(w.id);
        const auto originals = wrapperOriginals[slot].load(), childOriginals = originalCalls.load(), seenBegan = began.load(), seenRecorded = recorded.load(), seenWrites = fixtureWrites.load();
        const auto start = tally();
        if (observed)
            install(dispatcher);
        wrapper(w.hook)(run.ctx, run.after.data());
        if (observed)
            remove();
        const auto end     = tally();
        run.originals      = wrapperOriginals[slot] - originals;
        run.childOriginals = originalCalls - childOriginals;
        run.began          = began - seenBegan;
        run.recorded       = recorded - seenRecorded;
        run.replayWrites   = fixtureWrites - seenWrites;
        for (std::size_t i = 0; i < kNativeDrawHookCount; ++i)
            run.counted.invocations[i] = end.invocations[i] - start.invocations[i];
        run.counted.nested     = end.nested - start.nested;
        run.counted.attempted  = end.attempted - start.attempted;
        run.counted.mismatched = end.mismatched - start.mismatched;
        return run;
    };
    const auto none = [](std::vector<std::uint8_t>&)
    {
    };
#if REREVVED_DEVELOPER
    const auto fellTotal = [](const UserPointerConsumeCounts& counts)
    {
        std::uint64_t total = 0;
        for (const auto& row : counts.fallback)
            for (const auto count : row)
                total += count;
        return total;
    };
#endif
    // Original and its child ran once, nothing was consumed, and `reason` is the only fallback counted.
    const auto requireFallback = [&](const Wrapper& w, const Run& run, UserPointerFallback reason, const char* message)
    {
#if REREVVED_DEVELOPER
        const auto counts = TakeUserPointerConsumeCounts();
#endif
        UNSCOPED_INFO(message);
        REQUIRE((run.originals == 1 && run.childOriginals == 1 && run.replayWrites == 0 && DEV_CHECK(counts.consumed == decltype(counts.consumed){}) && DEV_CHECK(counts.consumedTiled == 0) && DEV_CHECK(counts.consumedDirty == 0) && DEV_CHECK(counts.fallback[static_cast<std::size_t>(w.id)][static_cast<std::size_t>(reason)] == 1) && DEV_CHECK(fellTotal(counts) == 1)));
    };
    // A clean admitted gate consumes; a dirty admitted gate still needs a bounded reservation.
    const auto requireGateOutcome = [&](const Wrapper& w, const Run& run, bool tiled, bool dirty, const char* message)
    {
        if (dirty)
        {
            requireFallback(w, run, UserPointerFallback::Reservation, "an admitted dirty gate with unbounded reservation work runs Original");
            return;
        }
#if REREVVED_DEVELOPER
        const auto counts                        = TakeUserPointerConsumeCounts();
        auto       expected                      = decltype(counts.consumed){};
        expected[static_cast<std::size_t>(w.id)] = 1;
#endif
        UNSCOPED_INFO(message);
        REQUIRE((run.originals == 0 && run.childOriginals == 0 && DEV_CHECK(counts.consumed == expected) && DEV_CHECK(counts.consumedTiled == (tiled ? 1U : 0U)) && DEV_CHECK(counts.consumedDirty == (dirty ? 1U : 0U)) && DEV_CHECK(counts.consumedDirtyVertexFetch == 0) && DEV_CHECK(fellTotal(counts) == 0)));
    };
    // The vertex-fetch gate can admit, but its reservation work is unbounded.
    const auto requireFetchReservationFallback = [&](const Wrapper& w, const Run& run, const char*)
    {
        requireFallback(w, run, UserPointerFallback::Reservation, "a vertex-fetch root with an unknown reservation runs Original");
    };

    const auto baseline = [](std::vector<std::uint8_t>& memory)
    {
        store(memory.data(), kGraphics + 0x00, 0x00000001);
        store(memory.data(), kGraphics + 0x14, 0x00000401);
        store(memory.data(), kGraphics + 0x30, 0x5000);
    };
    // The tiled and dirty consumes and the IndexedBuffer entry start off, whatever their defaults.
    const DrawConsumeCvars cvars;
    REXCVAR_SET(native_consume_user_pointer, true);
    ringStubsValidatedDevice.store(kGraphics);
    ringStubsActive.store(true);
    DEV_DO(TakeUserPointerConsumeCounts());

    for (const auto& w : wrappers)
    {
        // With the cvar off the wrapper and its child run Original and nothing is counted.
        REXCVAR_SET(native_consume_user_pointer, false);
        const auto reference = draw(w, baseline);
        REXCVAR_SET(native_consume_user_pointer, true);
#if REREVVED_DEVELOPER
        const auto cvarOff = TakeUserPointerConsumeCounts();
#endif
        UNSCOPED_INFO("with the cvar off a wrapper runs Original and nothing is counted");
        REQUIRE((reference.originals == 1 && reference.childOriginals == 1 && DEV_CHECK(cvarOff.consumed == decltype(cvarOff.consumed){}) && DEV_CHECK(fellTotal(cvarOff) == 0)));
        UNSCOPED_INFO("Original makes one nested call that the dispatcher counts");
        REQUIRE((reference.counted.invocations[static_cast<std::size_t>(w.child)] == 1 && reference.counted.nested == 1 && reference.counted.mismatched == 0 &&
                 reference.counted.attempted == 1));

        // Only the five dirty clears and the successful reservation rearm are replayed.
        const auto numbered = dispatcher.NumberedThrough();
        EnableNativeDrawCensus(true);
        TakeNativeDrawCensus();
        const auto consumed = draw(w, baseline);
        EnableNativeDrawCensus(false);
        const auto census = TakeNativeDrawCensus();
        requireGateOutcome(w, consumed, false, false, "an admitted wrapper skips Original and its child and counts as consumed");
        UNSCOPED_INFO("a consumed wrapper replays only the dirty qwords and no register");
        REQUIRE((consumed.after == dirtyExit(consumed.before, true) && sameRegisters(consumed.ctx, entry(w.caller))));
        UNSCOPED_INFO("a consumed wrapper is still observed, numbered and recorded");
        REQUIRE((consumed.began == 1 && consumed.recorded == 1 && dispatcher.NumberedThrough() == numbered + 1));
        UNSCOPED_INFO("a consumed wrapper counts its invocations, its nested child and no nesting mismatch as Original does");
        REQUIRE(consumed.counted == reference.counted);
        UNSCOPED_INFO("the recorded submission names the skipped child call");
        REQUIRE((last.hook == w.hook && last.nested == 1 && last.child == w.child && last.childCaller == w.childCaller && !last.nestingMismatch));
        UNSCOPED_INFO("the route census counts the skipped child as nested");
        REQUIRE((census.roots[static_cast<std::size_t>(w.hook)] == 1 && census.nested[static_cast<std::size_t>(w.child)] == 1));

        // The fallbacks, each from the consumed case with one condition broken.
        auto run = draw(w, none, false);
        requireFallback(w, run, UserPointerFallback::Dispatcher, "a wrapper without a dispatcher runs Original");
        UNSCOPED_INFO("no dispatcher observes it");
        REQUIRE(run.began == 0);

        // The IndexedBuffer fixture's Original calls the wrapper on a deeper frame.
        const auto slot      = static_cast<std::size_t>(w.id);
        const auto nestedRun = wrapperOriginals[slot].load();
        nestUp               = w.id == UserPointerWrapper::IndexedUp ? 1 : 2;
        {
            auto memory = guestMemory();
            auto ctx    = entry(0x8250C768);
            install(dispatcher);
            sub_826A39F8(ctx, memory.data());
            remove();
        }
        nestUp = 0;
        {
#if REREVVED_DEVELOPER
            const auto nested                                                = TakeUserPointerConsumeCounts();
            auto       one                                                   = decltype(nested.fallback){};
            one[slot][static_cast<std::size_t>(UserPointerFallback::Nested)] = 1;
#endif
            UNSCOPED_INFO("a wrapper nested in another observed draw runs Original");
            REQUIRE((DEV_CHECK(nested.consumed == decltype(nested.consumed){}) && DEV_CHECK(nested.consumedTiled == 0) && DEV_CHECK(nested.consumedDirty == 0) && DEV_CHECK(nested.fallback == one) && wrapperOriginals[slot] == nestedRun + 1));
        }

        // Unpublished services: Original and its child run and the root is not even counted as a fallback.
        ringStubsActive.store(false);
        run = draw(w, none);
        {
#if REREVVED_DEVELOPER
            const auto inert = TakeUserPointerConsumeCounts();
#endif
            UNSCOPED_INFO("unpublished ring stubs run Original and count nothing");
            REQUIRE((run.originals == 1 && run.childOriginals == 1 && DEV_CHECK(inert.consumed == decltype(inert.consumed){}) && DEV_CHECK(fellTotal(inert) == 0)));
        }
        ringStubsActive.store(true);
        ringStubsValidatedDevice.store(0);
        run = draw(w, none);
        requireFallback(w, run, UserPointerFallback::Stubs, "ring stubs that have not checked the device run Original");
        ringStubsValidatedDevice.store(kGraphics);

        // The thread's stack reach [limit, r1 + 88) against the device words and the objects the model reads,
        // tested after the stubs and before any other guest word is read. The device is copied 0x100 bytes up
        // so that its edges have room below it for the 88 bytes above r1.
        {
            constexpr std::uint32_t kMoved    = kGraphics + 0x100;
            const auto              movedRoot = [&](std::vector<std::uint8_t>& memory)
            {
                baseline(memory);
                const auto stride = load(memory.data(), kMoved + kIndexedBufferStrideByteOffset);
                std::memmove(memory.data() + kMoved, memory.data() + kGraphics, 0x3A5C);
                std::fill(memory.begin() + kGraphics, memory.begin() + kMoved, std::uint8_t{ 0 });
                store(memory.data(), kMoved + kIndexedBufferStrideByteOffset, stride);
                // The overlapping device setup wrote G+0x34CC through kMoved+0x33CC.
                // Seed the moved reservation after the copy so its pool pointer is valid.
                placeReservation(memory.data(), kMoved);
            };
            const auto slotOf = static_cast<std::size_t>(w.id);
            ringStubsValidatedDevice.store(kMoved);
            const auto consumedWith = [&](const auto& rootFn, std::uint32_t limit, std::uint32_t r1)
            {
                stackLimit    = limit;
                const auto at = draw(w, rootFn, true, kMoved, std::nullopt, r1);
                stackLimit.reset();
#if REREVVED_DEVELOPER
                const auto counts = TakeUserPointerConsumeCounts();
#endif
                if (at.originals == 0 && at.childOriginals == 0 && DEV_CHECK(counts.consumed[slotOf] == 1) && DEV_CHECK(fellTotal(counts) == 0))
                    return true;
#if REREVVED_DEVELOPER
                UNSCOPED_INFO(std::format("a refused stack runs Original with no replay stores and counts stack_overlap alone (wrapper {}, limit {:#x}, r1 {:#x}, originals {}, children {}, writes {}):{}", slotOf, limit, r1, at.originals, at.childOriginals, at.replayWrites, FormatUserPointerConsumeCounts(counts)).c_str());
#else
                UNSCOPED_INFO(std::format("a refused stack runs Original with no replay stores (wrapper {}, limit {:#x}, r1 {:#x}, originals {}, children {}, writes {})", slotOf, limit, r1, at.originals, at.childOriginals, at.replayWrites).c_str());
#endif
                REQUIRE((at.originals == 1 && at.childOriginals == 1 && at.replayWrites == 0 && DEV_CHECK(counts.consumed == decltype(counts.consumed){}) && DEV_CHECK(counts.fallback[slotOf][static_cast<std::size_t>(UserPointerFallback::StackOverlap)] == 1) && DEV_CHECK(fellTotal(counts) == 1)));
                return false;
            };
            const auto consumedAt = [&](std::uint32_t limit, std::uint32_t r1)
            {
                return consumedWith(movedRoot, limit, r1);
            };
            UNSCOPED_INFO("the wrapper footprint is 88 bytes above r1 and the device up to G+0x3A5C");
            REQUIRE((kUserPointerStackFootprints[slotOf].above == 88 && kUserPointerStackFootprints[slotOf].deviceSpan == 0x3A5C));
            const char* const wrapperName = w.id == UserPointerWrapper::IndexedUp ? "IndexedUp" : "NonindexedUp";
            requireStackEdges(wrapperName, kMoved, kUserPointerStackFootprints[slotOf].deviceSpan, kUserPointerStackFootprints[slotOf].above, consumedAt);
            UNSCOPED_INFO("the wrapper frame is the entry function's own (g188:13049, g26:13025)");
            REQUIRE(kUserPointerStackFootprints[slotOf].frame == (w.id == UserPointerWrapper::IndexedUp ? 176U : 128U));
            requireFrameEdges(wrapperName, kUserPointerStackFootprints[slotOf].frame, consumedAt);

            // The objects the device words name, moved clear of the device block (as for IndexedBuffer).
            {
                constexpr std::uint32_t kVs = 0x5000, kDecl = 0x6000;
                const auto              moved = [&](std::vector<std::uint8_t>& memory)
                {
                    movedRoot(memory);
                    store(memory.data(), kMoved + 0x3198, kVs);
                    store(memory.data(), kMoved + 0x2E2C, kDecl);
                };
                const auto movedAt = [&](std::uint32_t limit, std::uint32_t r1)
                {
                    return consumedWith(moved, limit, r1);
                };
                UNSCOPED_INFO("the moved objects of a user pointer wrapper are consumed on a clear stack");
                REQUIRE(movedAt(kStack - 0x400, kStack));
                requireObjectEdges("user pointer VS flags", kVs + 872, 4, 88, kUserPointerStackFootprints[slotOf].frame, movedAt);
                requireObjectEdges("user pointer VS record 0", kVs + 40, 24, 88, kUserPointerStackFootprints[slotOf].frame, movedAt);
                requireObjectEdges("user pointer VS record 1", kVs + 456, 24, 88, kUserPointerStackFootprints[slotOf].frame, movedAt);
                requireObjectEdges("user pointer declaration", kDecl + 32, 20, 88, kUserPointerStackFootprints[slotOf].frame, movedAt);
            }

            // The regression: the wrapper's frame stores its back chain at r1 - 176 (IndexedUp, stwu
            // g188:13049) or r1 - 128 (NonindexedUp, g26:13025). Over the dirty word G+0x14 it changes
            // a word the model reads, so the root is refused.
            const std::uint32_t frame  = w.id == UserPointerWrapper::IndexedUp ? 176 : 128;
            const std::uint32_t r1     = kMoved + 0x14 + frame;
            auto                memory = guestMemory();
            memory.resize(kGuestBytes + 0x10000);
            movedRoot(memory);
            const auto word = load(memory.data(), kMoved + 0x14);
            store(memory.data(), r1 - frame, r1);
            UNSCOPED_INFO("the wrapper's back chain store changes the dirty word");
            REQUIRE(load(memory.data(), kMoved + 0x14) != word);
            run = draw(w, movedRoot, true, kMoved, std::nullopt, r1);
            requireFallback(w, run, UserPointerFallback::StackOverlap, "a frame over G+0x14 runs Original");

            // The order: the stubs come first; the stack comes before the tiling gate.
            ringStubsValidatedDevice.store(0);
            run = draw(w, movedRoot, true, kMoved, std::nullopt, kMoved + 0x40);
            requireFallback(w, run, UserPointerFallback::Stubs, "ring stubs that have not checked the device are reported before the stack");
            ringStubsValidatedDevice.store(kMoved);
            run = draw(w, [&](std::vector<std::uint8_t>& m)
                       {
                           movedRoot(m);
                           store(m.data(), kMoved + 0x2ABC, 0x21000000);
                       },
                       true,
                       kMoved,
                       std::nullopt,
                       kMoved + 0x40);
            requireFallback(w, run, UserPointerFallback::StackOverlap, "the stack is reported before the tiling gate");
            ringStubsValidatedDevice.store(kGraphics);
        }

        for (const std::uint32_t tiling : { 0x21000000U, 0x01000000U, 0x20000000U })
        {
            run = draw(w, [&](std::vector<std::uint8_t>& memory)
                       {
                           store(memory.data(), kGraphics + 0x2ABC, tiling);
                       });
            requireFallback(w, run, UserPointerFallback::Tiling, "a set tiling byte runs Original");
        }

        ringStubsValidatedDevice.store(kEdge);
        run = draw(w, none, true, kEdge);
        requireFallback(w, run, UserPointerFallback::Unreadable, "an unreadable tiling word runs Original");
        ringStubsValidatedDevice.store(kGraphics);

        for (const std::uint32_t dirty : { 0x00020000U, 0x00100000U, 0x001E0000U, 0xFFFFFFFFU })
        {
            run = draw(w, [&](std::vector<std::uint8_t>& memory)
                       {
                           store(memory.data(), kGraphics + 0x14, dirty);
                       });
            requireFallback(w, run, UserPointerFallback::StateDirty, "a dirty bit of 0x1E0000 runs Original");
        }

        // native_consume_user_pointer_dirty: the fixture names VS 0x2000 and PS 0x2400.
        REXCVAR_SET(native_consume_user_pointer_dirty, true);
        const auto dirtyRoot = [&](std::uint32_t dirty, auto&& adjust)
        {
            return draw(w, [&](std::vector<std::uint8_t>& memory)
                        {
                            store(memory.data(), kGraphics + 0x14, dirty);
                            adjust(memory);
                        });
        };
        const auto modeControl = [](std::uint32_t value)
        {
            return [=](std::vector<std::uint8_t>& memory)
            {
                store(memory.data(), kGraphics + 0x2954, value);
            };
        };
        const auto dirtyNumbered = dispatcher.NumberedThrough();
        const auto dirtyConsumed = dirtyRoot(0x00020000,
                                             [](std::vector<std::uint8_t>& memory)
                                             {
                                                 store(memory.data(), kGraphics + 0x00, 0x00000001);
                                                 store(memory.data(), kGraphics + 0x30, 0x5000);
                                             });
        requireGateOutcome(w, dirtyConsumed, false, true, "a dirty wrapper whose guards hold still needs a bounded reservation");
        UNSCOPED_INFO("a dirty wrapper with unbounded reservation work runs Original");
        REQUIRE(dirtyConsumed.originals == 1);
        UNSCOPED_INFO("a refused dirty wrapper is still observed, numbered and recorded with its Original child");
        REQUIRE((dirtyConsumed.began == 1 && dirtyConsumed.recorded == 1 && dirtyConsumed.counted == reference.counted && dispatcher.NumberedThrough() == dirtyNumbered + 1));

        run = dirtyRoot(0x00100000, modeControl(0));
        requireFallback(w, run, UserPointerFallback::DirtyMode, "a PS root whose Original would store mode 4 runs Original");
        run = dirtyRoot(0x00100000, modeControl(0xFFFFFFFC));
        requireGateOutcome(w, run, false, true, "a PS root already in mode 4 passes the dirty guard");
        run = dirtyRoot(0x00020000,
                        [](std::vector<std::uint8_t>& memory)
                        {
                            store(memory.data(), kGraphics + 0x3194, 0);
                            store(memory.data(), kGraphics + 0x2954, 4);
                        });
        requireFallback(w, run, UserPointerFallback::DirtyMode, "a root without a PS whose Original would store mode 5 runs Original");
        run = dirtyRoot(0x001E0000,
                        [](std::vector<std::uint8_t>& memory)
                        {
                            store(memory.data(), kGraphics + 0x3198, 0);
                            store(memory.data(), kGraphics + 0x2954, 0);
                        });
        requireGateOutcome(w, run, false, true, "a dirty root without a VS passes the dirty guard");

        run = dirtyRoot(0x00100000,
                        [](std::vector<std::uint8_t>& memory)
                        {
                            store(memory.data(), kGraphics + 0x2ABC, 0x00004000);
                        });
        requireFallback(w, run, UserPointerFallback::DirtyRecorded, "a dirty root with G+0x2ABE bit 0x40 runs Original");

        const auto zprepass = [](std::uint32_t flags, std::uint32_t mode)
        {
            return [=](std::vector<std::uint8_t>& memory)
            {
                store(memory.data(), kGraphics + 0x2ABC, 0x40000000);
                store(memory.data(), kGraphics + 0x2954, mode);
                store(memory.data(), 0x2000 + 872, flags);
            };
        };
        run = dirtyRoot(0x00020000, zprepass(0x20, 0));
        requireFallback(w, run, UserPointerFallback::DirtyZPrepass, "a dirty Z-prepass root with the alternate depth-only VS runs Original");
        run = dirtyRoot(0x00020000, zprepass(0xFFFFFFDF, 0));
        requireGateOutcome(w, run, false, true, "a dirty Z-prepass root without the alternate VS passes the dirty guard");
        run = dirtyRoot(0x00020000,
                        [](std::vector<std::uint8_t>& memory)
                        {
                            store(memory.data(), kGraphics + 0x2ABC, 0x40000000);
                            store(memory.data(), kGraphics + 0x3198, 0x7FF0);
                        });
        requireFallback(w, run, UserPointerFallback::Unreadable, "an unreadable VS header word runs Original");
        // A device whose mode word is readable and whose shader words are not.
        constexpr std::uint32_t kShaderEdge = 0x5000;
        ringStubsValidatedDevice.store(kShaderEdge);
        run = draw(w, [&](std::vector<std::uint8_t>& memory)
                   {
                       store(memory.data(), kShaderEdge + 0x14, 0x00020000);
                   },
                   true,
                   kShaderEdge,
                   std::nullopt,
                   kShaderEdge - 0x1000); // the fixture stack at 0x7000 lies in this device's block
        requireFallback(w, run, UserPointerFallback::Unreadable, "an unreadable VS word runs Original");
        ringStubsValidatedDevice.store(kGraphics);

        // The diagnostics come after the dirty guards.
        armShaderHarvest(true);
        run = dirtyRoot(0x00100000, modeControl(0));
        requireFallback(w, run, UserPointerFallback::DirtyMode, "a failed dirty guard is reported before diagnostics");
        run = dirtyRoot(0x00020000, none);
        armShaderHarvest(false);
        if (kDeveloperBuild)
            requireFallback(w, run, UserPointerFallback::Diagnostics, "a shader harvest runs a dirty wrapper's Original");
        else
            requireGateOutcome(w, run, false, true, "with no recorder a dirty wrapper is judged by its reservation");

        // The nested lowerer or sink sets dirty bit 0x80000 itself when the stride
        // byte differs from G+0x2E38 and a VS is set, then runs 0x826ADD88. The
        // fixture names a VS; the stride below has the byte 0x8D.
        constexpr std::uint32_t kStride      = 0x1234;
        const auto              recordedByte = [](std::uint32_t byte)
        {
            return [=](std::vector<std::uint8_t>& memory)
            {
                store(memory.data(), kGraphics + 0x2E38, byte << 24);
            };
        };
        const auto strideRoot = [&](auto&& adjust, std::uint32_t graphics = kGraphics, std::uint32_t r1 = kStack)
        {
            return draw(w, adjust, true, graphics, kStride, r1);
        };
        // No PS with mode 4: a toggle to mode 5 if 0x826ADD88 runs.
        const auto noPixelShader = [](std::vector<std::uint8_t>& memory)
        {
            store(memory.data(), kGraphics + 0x3194, 0);
            store(memory.data(), kGraphics + 0x2954, 4);
        };
        const auto noVertexShader = [](std::vector<std::uint8_t>& memory)
        {
            store(memory.data(), kGraphics + 0x3198, 0);
        };

        run = strideRoot(none);
        requireGateOutcome(w, run, false, false, "a root whose stride byte equals G+0x2E38 stays clean and is consumed");
        UNSCOPED_INFO("a consumed stride-clean root replays only the dirty qwords");
        REQUIRE(run.after == dirtyExit(run.before, true));
        run = strideRoot(noPixelShader);
        requireGateOutcome(w, run, false, false, "an equal stride byte on a mode 4 root without a PS stays clean");
        REXCVAR_SET(native_consume_user_pointer_dirty, false);
        run = strideRoot(recordedByte(0x8E));
        requireFallback(w, run, UserPointerFallback::StateDirty, "a stride byte differing from G+0x2E38 with a VS dirties the root without the dirty cvar");
        run = strideRoot(recordedByte(0x00));
        requireFallback(w, run, UserPointerFallback::StateDirty, "a stride byte of 0x8D against a recorded 0 dirties the root without the dirty cvar");
        run = strideRoot([&](std::vector<std::uint8_t>& memory)
                         {
                             recordedByte(0x00)(memory);
                             noVertexShader(memory);
                         });
        requireGateOutcome(w, run, false, false, "a differing stride byte without a VS leaves the root clean");
        REXCVAR_SET(native_consume_user_pointer_dirty, true);
        run = strideRoot(
            [&](std::vector<std::uint8_t>& memory)
            {
                recordedByte(0x00)(memory);
                noPixelShader(memory);
            });
        requireFallback(w, run, UserPointerFallback::DirtyMode, "a differing stride byte on a mode 4 root without a PS runs Original for the toggle");
        run = strideRoot(
            [&](std::vector<std::uint8_t>& memory)
            {
                recordedByte(0x00)(memory);
                store(memory.data(), kGraphics + 0x3194, 0);
                store(memory.data(), kGraphics + 0x2954, 5);
            });
        requireGateOutcome(w, run, false, true, "a differing stride byte whose guards hold still needs a bounded reservation");
        UNSCOPED_INFO("a stride-dirty root with unbounded reservation work runs Original");
        REQUIRE(run.originals == 1);
        run = strideRoot(
            [&](std::vector<std::uint8_t>& memory)
            {
                recordedByte(0x00)(memory);
                noVertexShader(memory);
                store(memory.data(), kGraphics + 0x2954, 4);
            });
        requireGateOutcome(w, run, false, false, "a differing stride byte without a VS is consumed clean with the dirty cvar on");

        // The stride, G+0x2E38 and the VS word must all be readable.
        if (w.id == UserPointerWrapper::IndexedUp)
        {
            run = strideRoot(none, kGraphics, 0x7FD0);
            requireFallback(w, run, UserPointerFallback::Unreadable, "an unreadable stride slot runs Original");
        }
        ringStubsValidatedDevice.store(0x5300);
        run = strideRoot(none, 0x5300, 0x4000); // the fixture stack at 0x7000 lies in this device's block
        requireFallback(w, run, UserPointerFallback::Unreadable, "an unreadable G+0x2E38 runs Original");
        ringStubsValidatedDevice.store(0x5000);
        run = strideRoot(none, 0x5000, 0x4000);
        requireFallback(w, run, UserPointerFallback::Unreadable, "an unreadable VS word runs Original with a clean dirty word");
        ringStubsValidatedDevice.store(kGraphics);

        // A root with dirty bit 0x80000 consults the VS's vertex fetch cache.
        {
            FetchHarness harness;
            harness.userPointer = true;
            harness.consumed    = [&](std::uint32_t dirty, const FetchHarness::Adjust& adjust, std::optional<std::uint32_t> stride, const char* message)
            {
                const auto root = draw(
                    w,
                    [&](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x14, dirty);
                        adjust(memory);
                    },
                    true,
                    kGraphics,
                    stride);
                requireGateOutcome(w, root, false, true, message);
            };
            harness.fallback = [&](std::uint32_t dirty, const FetchHarness::Adjust& adjust, std::optional<std::uint32_t> stride, UserPointerFallback reason, const char* message)
            {
                const auto root = draw(
                    w,
                    [&](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x14, dirty);
                        adjust(memory);
                    },
                    true,
                    kGraphics,
                    stride);
                requireFallback(w, root, reason, message);
            };
            testVertexFetchPatch(harness);

            // The fetch cvar admits the state gate; the reservation still refuses before observer notification.
            static std::atomic<unsigned> observed{ 0 };
            static std::atomic<bool>     observedStride{ true };
            observed = 0;
            SetVertexFetchConsumeObserver(+[](const VertexFetchConsume& consume) noexcept
                                          {
                                              ++observed;
                                              observedStride = observedStride && consume.stride != nullptr;
                                          });
            REXCVAR_SET(native_consume_dirty_vertex_fetch, true);
            unsigned     admittedRoots = 0;
            FetchHarness enabled;
            enabled.userPointer = true;
            enabled.consumed    = harness.consumed;
            enabled.fallback    = [&](std::uint32_t dirty, const FetchHarness::Adjust& adjust, std::optional<std::uint32_t> stride, IndexedBufferFallback reason, const char* message)
            {
                const auto root = draw(
                    w,
                    [&](std::vector<std::uint8_t>& memory)
                    {
                        store(memory.data(), kGraphics + 0x14, dirty);
                        adjust(memory);
                    },
                    true,
                    kGraphics,
                    stride);
                if (reason == IndexedBufferFallback::DirtyVertexFetch)
                {
                    requireFetchReservationFallback(w, root, message);
                    ++admittedRoots;
                }
                else
                    requireFallback(w, root, reason, message);
            };
            testVertexFetchPatch(enabled);
            REXCVAR_SET(native_consume_dirty_vertex_fetch, false);
            SetVertexFetchConsumeObserver(nullptr);
            UNSCOPED_INFO("an unknown reservation is refused before notifying the vertex-fetch consume observer");
            REQUIRE((admittedRoots >= 10 && observed == 0 && observedStride));
        }

        // With the base cvar off the dirty cvar counts and skips nothing.
        REXCVAR_SET(native_consume_user_pointer, false);
        run = dirtyRoot(0x00020000, none);
#if REREVVED_DEVELOPER
        const auto dirtyOff = TakeUserPointerConsumeCounts();
#endif
        UNSCOPED_INFO("with the base cvar off a dirty wrapper runs Original and nothing is counted");
        REQUIRE((run.originals == 1 && run.childOriginals == 1 && DEV_CHECK(dirtyOff.consumed == decltype(dirtyOff.consumed){}) && DEV_CHECK(dirtyOff.consumedDirty == 0) && DEV_CHECK(fellTotal(dirtyOff) == 0)));
        REXCVAR_SET(native_consume_user_pointer, true);

        // The IndexedBuffer dirty cvar is no switch for the wrapper's dirty roots.
        REXCVAR_SET(native_consume_user_pointer_dirty, false);
        REXCVAR_SET(native_consume_indexed_buffer, true);
        REXCVAR_SET(native_consume_indexed_buffer_dirty, true);
        run = dirtyRoot(0x00020000, none);
        requireFallback(w, run, UserPointerFallback::StateDirty, "the IndexedBuffer dirty cvars do not consume a dirty wrapper");
        REXCVAR_SET(native_consume_indexed_buffer_dirty, false);
        REXCVAR_SET(native_consume_indexed_buffer, false);

        // The diagnostics.
        armShaderHarvest(true);
        run = draw(w, none);
        armShaderHarvest(false);
        if (kDeveloperBuild)
            requireFallback(w, run, UserPointerFallback::Diagnostics, "a shader harvest runs Original");
        else
            requireGateOutcome(w, run, false, false, "with no recorder a clean wrapper is consumed");

#if REREVVED_DEVELOPER
        // The label adapter capture the NonindexedUp hook began reads what Original leaves.
        claimLabelAdapter.store(true);
        run = draw(w, none);
        claimLabelAdapter.store(false);
        if (w.id == UserPointerWrapper::NonindexedUp)
            requireFallback(w, run, UserPointerFallback::Diagnostics, "a wrapper that began an adapter capture runs Original");
        else
            requireGateOutcome(w, run, false, false, "the label wrapper's adapter capture does not keep this Original");
#endif

        // Tiled roots: exactly 0x21 behind native_consume_user_pointer_tiled, and
        // then the tripwires read from G at entry.
        constexpr std::uint32_t kTiledByte = 0x21000000;
        const auto              tiledRoot  = [&](auto&& adjust, std::uint32_t graphics = kGraphics)
        {
            return draw(w, [&](std::vector<std::uint8_t>& memory)
                        {
                            store(memory.data(), graphics + 0x2ABC, kTiledByte);
                            // The unreadable-tripwire case puts this word outside the readable range.
                            store(memory.data(), graphics + 0x33B0, 0x1234);
                            adjust(memory);
                        },
                        true,
                        graphics,
                        std::nullopt,
                        graphics == kGraphics ? kStack : graphics - 0x1000); // a device elsewhere keeps the stack clear of its block
        };
        run = tiledRoot(none);
        requireFallback(w, run, UserPointerFallback::Tiling, "a tiled root runs Original while the tiled cvar is off");

        // The IndexedBuffer tiled cvars are no switch for the wrapper's tiled roots.
        REXCVAR_SET(native_consume_indexed_buffer, true);
        REXCVAR_SET(native_consume_indexed_buffer_tiled, true);
        run = tiledRoot(none);
        requireFallback(w, run, UserPointerFallback::Tiling, "the IndexedBuffer tiled cvars do not consume a tiled wrapper");
        REXCVAR_SET(native_consume_indexed_buffer_tiled, false);
        REXCVAR_SET(native_consume_indexed_buffer, false);

        REXCVAR_SET(native_consume_user_pointer_tiled, true);
        const auto tiledNumbered = dispatcher.NumberedThrough();
        const auto tiledConsumed = tiledRoot([](std::vector<std::uint8_t>& memory)
                                             {
                                                 store(memory.data(), kGraphics + 0x00, 0x00000001);
                                                 store(memory.data(), kGraphics + 0x14, 0x00000401);
                                                 store(memory.data(), kGraphics + 0x30, 0x5000);
                                                 store(memory.data(), kGraphics + 0x33C0, 0x00200000);
                                                 store(memory.data(), kGraphics + 0x33C4, 0x00200000);
                                                 store(memory.data(), kGraphics + 0x33C8, 0x00200024);
                                                 store(memory.data(), kGraphics + 0x33CC, 0x0020107C);
                                             });
        requireGateOutcome(w, tiledConsumed, true, false, "a tiled wrapper with its tripwires satisfied skips Original and counts as consumed tiled");
        UNSCOPED_INFO("a consumed tiled wrapper replays only the dirty qwords (cursor, list header, dirty words) and no register");
        REQUIRE((tiledConsumed.after == dirtyExit(tiledConsumed.before, true) && sameRegisters(tiledConsumed.ctx, entry(w.caller))));
        UNSCOPED_INFO("a consumed tiled wrapper is still observed, numbered and recorded with its child");
        REQUIRE((tiledConsumed.began == 1 && tiledConsumed.recorded == 1 && tiledConsumed.counted == reference.counted && dispatcher.NumberedThrough() == tiledNumbered + 1));

        // Each tripwire broken alone.
        run = tiledRoot([](std::vector<std::uint8_t>& memory)
                        {
                            store(memory.data(), kGraphics + 0x33B0, 0);
                        });
        requireFallback(w, run, UserPointerFallback::TiledState, "a closed bracket word G+0x33B0 runs Original");
        run = tiledRoot([](std::vector<std::uint8_t>& memory)
                        {
                            store(memory.data(), kGraphics + 0x31AC, 1);
                        });
        requireFallback(w, run, UserPointerFallback::TiledState, "a nonzero G+0x31AC runs Original");
        run = tiledRoot([](std::vector<std::uint8_t>& memory)
                        {
                            store(memory.data(), kGraphics + 0x2ABC, 0x21200000);
                        });
        requireFallback(w, run, UserPointerFallback::TiledState, "an allocation failure bit G+0x2ABD & 0x20 runs Original");
        constexpr std::uint32_t kTripEdge = 0x4C50;
        ringStubsValidatedDevice.store(kTripEdge);
        run = tiledRoot(none, kTripEdge);
        requireFallback(w, run, UserPointerFallback::Unreadable, "an unreadable tripwire word runs Original");
        ringStubsValidatedDevice.store(kGraphics);

        // Only the exact byte 0x21 is a tiled root.
        run = tiledRoot([](std::vector<std::uint8_t>& memory)
                        {
                            store(memory.data(), kGraphics + 0x2ABC, 0x31000000);
                        });
        requireFallback(w, run, UserPointerFallback::Tiling, "a tiling byte other than exactly 0x21 runs Original as tiling");

        // The dirty word is tested before the tripwires; a dirty tiled root needs both cvars.
        run = tiledRoot([](std::vector<std::uint8_t>& memory)
                        {
                            store(memory.data(), kGraphics + 0x14, 0x00020000);
                            store(memory.data(), kGraphics + 0x33B0, 0);
                        });
        requireFallback(w, run, UserPointerFallback::StateDirty, "state_dirty is reported before a tripped tiled tripwire");
        REXCVAR_SET(native_consume_user_pointer_dirty, true);
        run = tiledRoot([](std::vector<std::uint8_t>& memory)
                        {
                            store(memory.data(), kGraphics + 0x14, 0x00020000);
                        });
        requireGateOutcome(w, run, true, true, "a dirty tiled wrapper whose guards and tripwires hold still needs a bounded reservation");
        run = tiledRoot([](std::vector<std::uint8_t>& memory)
                        {
                            store(memory.data(), kGraphics + 0x14, 0x00020000);
                            store(memory.data(), kGraphics + 0x33B0, 0);
                        });
        requireFallback(w, run, UserPointerFallback::TiledState, "a dirty tiled wrapper still checks the tripwires");
        REXCVAR_SET(native_consume_user_pointer_dirty, false);

        // The tripwires are tested before the diagnostics.
        armShaderHarvest(true);
        run = tiledRoot([](std::vector<std::uint8_t>& memory)
                        {
                            store(memory.data(), kGraphics + 0x33B0, 0);
                        });
        requireFallback(w, run, UserPointerFallback::TiledState, "a tripped tripwire is reported before diagnostics");
        run = tiledRoot(none);
        armShaderHarvest(false);
        if (kDeveloperBuild)
            requireFallback(w, run, UserPointerFallback::Diagnostics, "a shader harvest runs a tiled wrapper's Original");
        else
            requireGateOutcome(w, run, true, false, "with no recorder a tiled wrapper is consumed");

        // With the base cvar off the tiled cvar counts and skips nothing.
        REXCVAR_SET(native_consume_user_pointer, false);
        run = tiledRoot(none);
#if REREVVED_DEVELOPER
        const auto tiledOff = TakeUserPointerConsumeCounts();
#endif
        UNSCOPED_INFO("with the base cvar off a tiled wrapper runs Original and nothing is counted");
        REQUIRE((run.originals == 1 && run.childOriginals == 1 && DEV_CHECK(tiledOff.consumed == decltype(tiledOff.consumed){}) && DEV_CHECK(tiledOff.consumedTiled == 0) && DEV_CHECK(fellTotal(tiledOff) == 0)));
        REXCVAR_SET(native_consume_user_pointer, true);
        REXCVAR_SET(native_consume_user_pointer_tiled, false);

        // The IndexedBuffer base cvar is no switch for the wrapper.
        REXCVAR_SET(native_consume_user_pointer, false);
        REXCVAR_SET(native_consume_indexed_buffer, true);
        run = draw(w, none);
        REXCVAR_SET(native_consume_indexed_buffer, false);
#if REREVVED_DEVELOPER
        const auto indexedOnly = TakeUserPointerConsumeCounts();
#endif
        UNSCOPED_INFO("the IndexedBuffer base cvar does not consume a wrapper");
        REQUIRE((run.originals == 1 && DEV_CHECK(indexedOnly.consumed == decltype(indexedOnly.consumed){}) && DEV_CHECK(fellTotal(indexedOnly) == 0)));
        REXCVAR_SET(native_consume_user_pointer, true);

        // Taking resets the counters.
        run = draw(w, none);
#if REREVVED_DEVELOPER
        const auto first  = TakeUserPointerConsumeCounts();
        const auto second = TakeUserPointerConsumeCounts();
        UNSCOPED_INFO("taking the wrapper counters resets them");
        REQUIRE((first.consumed[slot] == 1 && second.consumed == decltype(second.consumed){} && fellTotal(second) == 0));
#endif
    }

    ringStubsActive.store(false);
    ringStubsValidatedDevice.store(0);
    REXCVAR_SET(native_consume_user_pointer, false);
}

// Unpublished services force Original; published services enable consume.
void testConsumesWaitForPublishedStubs()
{
    struct Entry
    {
        NativeDrawHook hook;
        std::uint32_t  caller;
    };

    const std::array<Entry, 3> entries{ {
        { NativeDrawHook::IndexedBuffer, 0x8250C768 },
        { NativeDrawHook::IndexedUp, 0x82303E90 },
        { NativeDrawHook::NonindexedUp, 0x82304258 },
    } };
    const auto                 savedActive = ringStubsActive.load();
    const auto                 savedDevice = ringStubsValidatedDevice.load();
    ScopedDispatcher           dispatcher(skipConfig());
    ringStubsValidatedDevice.store(kGraphics);

    // The Original runs and the reads of one root, with the cvars and services as given.
    struct Measured
    {
        std::uint64_t originals = 0, reads = 0;
    };

    const auto measure = [&](const Entry& entry, bool cvars, bool published)
    {
        const DrawConsumeCvars scope(cvars);
        ringStubsActive.store(published);
#if REREVVED_DEVELOPER
        (void)TakeIndexedBufferConsumeCounts();
        (void)TakeUserPointerConsumeCounts();
#endif
        const auto originalRuns = []
        {
            return originalCalls.load() + bufferOriginals.load() + wrapperOriginals[0].load() + wrapperOriginals[1].load();
        };
        const auto originals = originalRuns(), reads = fixtureReads.load();
        (void)run(entry.hook, entry.caller, &dispatcher);
        return Measured{ originalRuns() - originals, fixtureReads - reads };
    };
    const auto requireNoCounts = []([[maybe_unused]] const char* message)
    {
#if REREVVED_DEVELOPER
        const auto    buffer  = TakeIndexedBufferConsumeCounts();
        const auto    user    = TakeUserPointerConsumeCounts();
        std::uint64_t counted = buffer.consumed + buffer.consumedTiled + buffer.consumedDirty + user.consumedTiled + user.consumedDirty;
        for (const auto count : buffer.fallback)
            counted += count;
        for (const auto count : user.consumed)
            counted += count;
        for (const auto& row : user.fallback)
            for (const auto count : row)
                counted += count;
        UNSCOPED_INFO(message);
        REQUIRE(counted == 0);
#endif
    };
    for (const auto& entry : entries)
    {
        const auto off = measure(entry, false, false);
        requireNoCounts("with the cvars off nothing is counted");
        const auto unpublished = measure(entry, true, false);
        requireNoCounts("unpublished ring stubs leave the consumes uncounted");
        UNSCOPED_INFO("unpublished ring stubs run the same Originals as the cvars off");
        REQUIRE((off.originals > 0 && unpublished.originals == off.originals));
        UNSCOPED_INFO("unpublished ring stubs read no word for a consume");
        REQUIRE(unpublished.reads == off.reads);
        const auto published = measure(entry, true, true);
#if REREVVED_DEVELOPER
        (void)TakeIndexedBufferConsumeCounts();
        (void)TakeUserPointerConsumeCounts();
#endif
        UNSCOPED_INFO("published ring stubs make the consume read the device");
        REQUIRE(published.reads > off.reads);
        // A withdrawal that lands between the cvar test and the lease: the call runs
        // Original as if the services had never been published.
        SetRingStubLeaseProbe([]() noexcept
                              {
                                  ringStubsActive.store(false, std::memory_order_seq_cst);
                              });
        const auto withdrawn = measure(entry, true, true);
        SetRingStubLeaseProbe(nullptr);
        requireNoCounts("a withdrawal before the lease leaves the consumes uncounted");
        UNSCOPED_INFO("a withdrawal before the lease reads no word for a consume");
        REQUIRE((withdrawn.originals == off.originals && withdrawn.reads == off.reads));
    }

    ringStubsActive.store(savedActive);
    ringStubsValidatedDevice.store(savedDevice);
}

// Skipping 0x826A3048 preserves allocator, staging and output for Original.
void testConsumeLowerer()
{
    constexpr std::uint32_t kRingCursor = 0x300, kSoft = 0x600, kHard = 0x680, kPool = 0x5000, kPoolBase = 0x5100, kPoolSize = 0x1D00, kProgress = 0x5800, kTileList = 0x7400;
    constexpr std::uint32_t kVertexOut = 0x7100, kIndexOut = 0x7104, kCaller = 0x82510BFC;

    struct Call
    {
        std::uint32_t vertices = 10, indices = 15, flags = 0, stride = 32, caller = kCaller, r1 = kStack;
    };

    struct Run
    {
        PPCContext                      ctx{};
        std::vector<std::uint8_t>       before, after;
        std::uint64_t                   originals = 0, recorded = 0, committed = 0;
        std::optional<NativeDrawCommit> commit;
        std::optional<NativeDrawReason> reason;
    };

    std::uint64_t                   recorded = 0, committed = 0;
    std::optional<NativeDrawCommit> lastCommit;
    std::optional<NativeDrawReason> lastReason;
    auto                            config = skipConfig();
    config.write                           = writeFixture;
    config.writable                        = writableFixture;
    config.committed                       = [&](const NativeDrawSubmission& submission)
    {
        ++committed;
        lastCommit = submission.commit;
    };
    config.recorded = [&](const NativeDrawSubmission&, std::optional<NativeDrawReason> reason)
    {
        ++recorded;
        lastReason = reason;
    };
    ScopedDispatcher dispatcher(std::move(config));
    // A dispatcher with a reader and no stores: the consume has nowhere to write.
    ScopedDispatcher readOnlyDispatcher(skipConfig());

    const auto fixedPool = [](std::vector<std::uint8_t>& memory)
    {
        store(memory.data(), kGraphics + 0x34CC, kPool);
        store(memory.data(), kPool + 152, kPoolBase);
        store(memory.data(), kPool + 156, kPoolSize);
    };
    const auto nullPool = [](std::vector<std::uint8_t>& memory)
    {
        store(memory.data(), kGraphics + 0x34CC, 0);
        store(memory.data(), kGraphics + 0x3A3C, 0x6F00); // the side buffer area end
        store(memory.data(), kGraphics + 0x3A50, 0x7F00); // the wrap limit
        store(memory.data(), kGraphics + 0x3A4C, 0x7C00); // the wrap position
        store(memory.data(), kGraphics + 0x3A48, 2);      // the segment
        store(memory.data(), kGraphics + 0x2A90, kProgress);
        store(memory.data(), kProgress + 4, 2); // the GPU is at the segment
    };
    const auto baseline = [&](std::vector<std::uint8_t>& memory)
    {
        store(memory.data(), kGraphics + 0x30, kRingCursor);
        store(memory.data(), kGraphics + 0x34, kHard);
        store(memory.data(), kGraphics + 0x38, kSoft);
        store(memory.data(), kGraphics + 0x2E38, 8U << 24); // the recorded stride byte of a 32-byte stride
        store(memory.data(), kGraphics + 0x3A58, 0x100000);
        store(memory.data(), kGraphics + 0x33C4, kTileList);
        store(memory.data(), kGraphics + 0x33C8, kTileList);
        store(memory.data(), kGraphics + 0x33CC, kTileList + 0x100);
        store(memory.data(), kStack + 84, kVertexOut);
        fixedPool(memory);
    };
    const auto drive = [&](const Call& call, auto&& prepare, NativeDrawDispatcher* installed)
    {
        Run run;
        run.after = guestMemory();
        placeStack(run.after.data(), call.r1);
        baseline(run.after);
        prepare(run.after);
        run.before           = run.after;
        run.ctx              = entry(call.caller);
        run.ctx.r1.u64       = call.r1;
        run.ctx.r6.u64       = call.vertices;
        run.ctx.r7.u64       = call.indices;
        run.ctx.r8.u64       = call.flags;
        run.ctx.r9.u64       = call.stride;
        run.ctx.r10.u64      = kIndexOut;
        const auto originals = originalCalls.load(), seenRecorded = recorded, seenCommitted = committed;
        lastCommit.reset();
        if (installed)
            install(*installed);
        sub_826A3048(run.ctx, run.after.data());
        if (NativeDrawDefersToCommit(NativeDrawHook::IndexedUpLowerer, call.caller))
            commit(run.after.data(), kGraphics, call.r1);
        if (installed)
            remove();
        run.originals = originalCalls - originals;
        run.recorded  = recorded - seenRecorded;
        run.committed = committed - seenCommitted;
        run.commit    = lastCommit;
        run.reason    = lastReason;
        return run;
    };
    const auto none = [](std::vector<std::uint8_t>&)
    {
    };

    // The words a consume leaves different from Original by design: the ring words,
    // the ring cursor the commit publishes, the staged end, and the tile list.
    const auto sameButRing = [&](const Run& a, const Run& b)
    {
        for (std::uint32_t address = 0; address < a.after.size(); address += 4)
        {
            if (address >= 0x100 && address < 0x700)
                continue;
            if (address == kGraphics + 0x30 || address == kGraphics + 0x3484 || address == kGraphics + 0x33C8 || (address >= kTileList && address < kTileList + 0x100))
                continue;
            if (load(a.after.data(), address) != load(b.after.data(), address))
            {
                std::cerr << "memory differs at 0x" << std::hex << address << ": " << load(a.after.data(), address) << " vs " << load(b.after.data(), address) << std::dec << '\n';
                return false;
            }
        }
        return a.ctx.r3.u64 == b.ctx.r3.u64;
    };
#if REREVVED_DEVELOPER
    const auto fellTotal = [](const LowererConsumeCounts& counts)
    {
        std::uint64_t total = 0;
        for (const auto count : counts.fallback)
            total += count;
        return total;
    };
#endif
    // Original ran once, nothing was consumed, and `reason` is the only fallback counted.
    const auto requireFallback = [&](const Run& run, LowererFallback reason, const char* message)
    {
#if REREVVED_DEVELOPER
        const auto counts = TakeLowererConsumeCounts();
#endif
        UNSCOPED_INFO(message);
        REQUIRE((run.originals == 1 && DEV_CHECK(counts.consumed == 0) && DEV_CHECK(counts.fallback[static_cast<std::size_t>(reason)] == 1) && DEV_CHECK(fellTotal(counts) == 1)));
    };
    const auto requireConsumed = [&](const Run& run, const char* message)
    {
#if REREVVED_DEVELOPER
        const auto counts = TakeLowererConsumeCounts();
#endif
#if REREVVED_DEVELOPER
        if (!(run.originals == 0 && counts.consumed == 1 && fellTotal(counts) == 0))
            std::cerr << "not consumed:" << FormatLowererConsumeCounts(counts) << " originals " << run.originals << '\n';
#endif
        UNSCOPED_INFO(message);
        REQUIRE((run.originals == 0 && DEV_CHECK(counts.consumed == 1) && DEV_CHECK(fellTotal(counts) == 0)));
    };

    const auto savedActive = ringStubsActive.load();
    const auto savedDevice = ringStubsValidatedDevice.load();
    exactLowerer.store(true);
    ringStubsValidatedDevice.store(kGraphics);
    ringStubsActive.store(true);
    REXCVAR_SET(native_consume_lowerer, false);
    REXCVAR_SET(native_consume_lowerer_tiled, false);
    DEV_CVAR_SET(native_lowerer_exit_check, false);
    DEV_DO(TakeLowererConsumeCounts());
    DEV_DO(TakeLowererExitCounts());

    // With the cvar off the root runs Original and nothing is counted.
    const auto reference = drive({}, none, &dispatcher);
    UNSCOPED_INFO("the reference root runs the faithful Original and commits");
    REQUIRE((reference.originals == 1 && reference.ctx.r3.u32 == 0 && reference.committed == 1 && reference.commit && reference.commit->read));
    {
#if REREVVED_DEVELOPER
        const auto counts = TakeLowererConsumeCounts();
        UNSCOPED_INFO("with the cvar off nothing is counted");
        REQUIRE((counts.consumed == 0 && fellTotal(counts) == 0));
#endif
    }
    UNSCOPED_INFO("the reference commit publishes the PM4 end");
    REQUIRE((load(reference.after.data(), kGraphics + 0x3484) == kRingCursor + 4 * 18 && load(reference.after.data(), kGraphics + 0x30) == kRingCursor + 4 * 18));

    REXCVAR_SET(native_consume_lowerer, true);
    const auto numbered = dispatcher.NumberedThrough();
    const auto consumed = drive({}, none, &dispatcher);
    requireConsumed(consumed, "an admitted root skips Original and counts as consumed");
    UNSCOPED_INFO("a consumed call returns zero");
    REQUIRE(consumed.ctx.r3.u64 == 0);
    UNSCOPED_INFO("a consumed call leaves every word but the ring's as Original does");
    REQUIRE(sameButRing(consumed, reference));
    UNSCOPED_INFO("the staged end is the entry cursor, so the commit leaves the cursor");
    REQUIRE((load(consumed.after.data(), kGraphics + 0x3484) == kRingCursor && load(consumed.after.data(), kGraphics + 0x30) == kRingCursor));
    UNSCOPED_INFO("a consumed root is still committed, numbered and recorded");
    REQUIRE((consumed.committed == 1 && consumed.recorded == 1 && dispatcher.NumberedThrough() == numbered + 1 && consumed.reason == reference.reason));
    UNSCOPED_INFO("the host reads the same buffers and counts at the commit");
    REQUIRE((consumed.commit && reference.commit && consumed.commit->read && consumed.commit->vertexDest == reference.commit->vertexDest && consumed.commit->indexDest == reference.commit->indexDest &&
             consumed.commit->vertexDwords == reference.commit->vertexDwords && consumed.commit->indexDwords == reference.commit->indexDwords &&
             consumed.commit->vertexOut == reference.commit->vertexOut && consumed.commit->indexOut == reference.commit->indexOut && consumed.commit->vertexDwords == 80 &&
             consumed.commit->indexDwords == 8 && consumed.commit->pendingEnd == kRingCursor && consumed.commit->commitEnd == kRingCursor && reference.commit->pendingEnd == kRingCursor + 72));
    UNSCOPED_INFO("the out words hold the buffers");
    REQUIRE((load(consumed.after.data(), kVertexOut) == consumed.commit->vertexDest && load(consumed.after.data(), kIndexOut) == consumed.commit->indexDest));

    // The pool shapes, sizes and flags, each against the faithful Original.
    const std::array<Call, 7>                                            calls{ { { 10, 15, 0, 32 }, { 10, 15, 4, 32 }, { 1, 1, 0, 4 }, { 0, 0, 0, 32 }, { 30, 41, 0, 32 }, { 3, 4, 4, 12 }, { 60, 2, 0, 16 } } };
    const std::array<std::function<void(std::vector<std::uint8_t>&)>, 4> pools{ {
        [](std::vector<std::uint8_t>&)
        {
        },
        [&](std::vector<std::uint8_t>& m)
        {
            store(m.data(), kGraphics + 0x3A44, kPoolBase + 0x1000 + 4);
            store(m.data(), kGraphics + 0x3A54, 1000);
        },
        [&](std::vector<std::uint8_t>& m)
        {
            nullPool(m);
        },
        [&](std::vector<std::uint8_t>& m)
        {
            // A wrap of the null pool: the position is at the end of the side buffer area, so a take with bytes wraps to segment 3 and
            // an empty one does not. The GPU word is segment 2 with the block end past the wrapped takes (g206:12440-12459).
            nullPool(m);
            store(m.data(), kGraphics + 0x3A44, 0x6F00 - 4);
            store(m.data(), kGraphics + 0x3A4C, 0x7000);
            store(m.data(), kGraphics + 0x3A50, 0x8100);
            store(m.data(), kProgress + 4, 0x8102);
        },
    } };
    for (const auto& call : calls)
    {
        for (std::size_t pool = 0; pool < pools.size(); ++pool)
        {
            const auto prepare = [&](std::vector<std::uint8_t>& memory)
            {
                store(memory.data(), kGraphics + 0x2E38, ((call.stride >> 2) & 0xFF) << 24);
                pools[pool](memory);
            };
            REXCVAR_SET(native_consume_lowerer, false);
            const auto original = drive(call, prepare, &dispatcher);
            REXCVAR_SET(native_consume_lowerer, true);
            DEV_DO(TakeLowererConsumeCounts());
            const auto skipped = drive(call, prepare, &dispatcher);
            requireConsumed(skipped, "every shape is consumed");
            UNSCOPED_INFO("the reference ran Original");
            REQUIRE(original.originals == 1);
            UNSCOPED_INFO(("a shape differs from Original: pool " + std::to_string(pool) + " vertices " + std::to_string(call.vertices)).c_str());
            REQUIRE(sameButRing(skipped, original));
        }
    }

    // A consumed draw leaves the pool where the next Original takes from.
    {
        const auto twice = [&](bool firstConsumed)
        {
            REXCVAR_SET(native_consume_lowerer, firstConsumed);
            auto first = drive({}, none, &dispatcher);
            REXCVAR_SET(native_consume_lowerer, false);
            // The second call is on the memory the first left.
            Run second;
            second.after       = first.after;
            second.ctx         = entry(kCaller);
            second.ctx.r6.u64  = 7;
            second.ctx.r7.u64  = 9;
            second.ctx.r9.u64  = 32;
            second.ctx.r10.u64 = kIndexOut;
            install(dispatcher);
            sub_826A3048(second.ctx, second.after.data());
            commit(second.after.data());
            remove();
            return second;
        };
        const auto afterOriginal = twice(false);
        const auto afterConsume  = twice(true);
        DEV_DO(TakeLowererConsumeCounts());
        UNSCOPED_INFO("a consumed draw leaves the pool, limits, staging and dirty words as Original does for the next draw");
        REQUIRE(sameButRing(afterConsume, afterOriginal));
    }

#if REREVVED_DEVELOPER
    // The exit probe against the faithful Original: no mismatch for either pool or the tiled shape.
    {
        REXCVAR_SET(native_consume_lowerer, false);
        REXCVAR_SET(native_lowerer_exit_check, true);
        TakeLowererExitCounts();
        for (std::size_t pool = 0; pool < pools.size(); ++pool)
        {
            for (const auto& call : calls)
            {
                drive(call, [&](std::vector<std::uint8_t>& memory)
                      {
                          store(memory.data(), kGraphics + 0x2E38, ((call.stride >> 2) & 0xFF) << 24);
                          pools[pool](memory);
                      },
                      &dispatcher);
            }
        }
        drive({}, [](std::vector<std::uint8_t>& memory)
              {
                  store(memory.data(), kGraphics + 0x2ABC, 0x21000000);
              },
              &dispatcher);
        auto          counts     = TakeLowererExitCounts();
        std::uint64_t mismatches = 0;
        for (const auto count : counts.mismatch)
            mismatches += count;
        UNSCOPED_INFO((std::string("the probe finds no mismatch: ") + FormatLowererExitCounts(counts)).c_str());
        REQUIRE((counts.ok == pools.size() * calls.size() + 1 && counts.tiled == 1 && counts.tiledOk == 1 && mismatches == 0 && counts.unreadable == 0));

        // A deviating Original is seen.
        lowererDeviates.store(true);
        drive({}, none, &dispatcher);
        lowererDeviates.store(false);
        counts = TakeLowererExitCounts();
        UNSCOPED_INFO("a deviating Original shows in the probe's pool_used word");
        REQUIRE((counts.ok == 0 && counts.mismatch[9] == 1));

        // The probe is only for a root under a dispatcher, and only when it is requested.
        drive({}, none, nullptr);
        drive({ .caller = kIndexedUpLowererReturn }, none, &dispatcher);
        counts = TakeLowererExitCounts();
        UNSCOPED_INFO("no dispatcher root, no probe");
        REQUIRE(counts.flushWordsCalls == 0);
        REXCVAR_SET(native_lowerer_exit_check, false);
        drive({}, none, &dispatcher);
        UNSCOPED_INFO("with the cvar off the probe does not run");
        REQUIRE(TakeLowererExitCounts().flushWordsCalls == 0);
    }
#endif

    // Fallbacks: Original runs, the one reason is counted, and nothing else changes.
    REXCVAR_SET(native_consume_lowerer, true);
    DEV_DO(TakeLowererConsumeCounts());
    {
        const auto run = drive({}, none, nullptr);
        requireFallback(run, LowererFallback::Dispatcher, "without a dispatcher the root runs Original");
    }
    {
        const auto run = drive({}, none, &readOnlyDispatcher);
        requireFallback(run, LowererFallback::Unreadable, "a dispatcher without stores cannot take the consume");
    }
    {
        ringStubsActive.store(false);
        const auto run = drive({}, none, &dispatcher);
        ringStubsActive.store(true);
        requireFallback(run, LowererFallback::Stubs, "without ring stubs the root runs Original");
    }
    {
        const auto run = drive({ .caller = kIndexedUpLowererReturn }, none, &dispatcher);
        requireFallback(run, LowererFallback::Nested, "the child call of an IndexedUp wrapper is not a held root");
    }

    // The thread's stack reach [limit, r1 + 88) against the device words and the objects the model reads or
    // writes, after the stubs and before any other guest word is read. The vertex out slot is at r1+84.
    {
        const auto slotAt = [&](std::uint32_t r1)
        {
            return [r1](std::vector<std::uint8_t>& memory)
            {
                if (r1 + 84 <= kGuestBytes - 4)
                    store(memory.data(), r1 + 84, kVertexOut);
            };
        };
        const auto consumedWith = [&](const auto& prepare, std::uint32_t limit, std::uint32_t r1)
        {
            stackLimit     = limit;
            const auto run = drive({ .r1 = r1 }, [&](std::vector<std::uint8_t>& memory)
                                   {
                                       slotAt(r1)(memory);
                                       prepare(memory);
                                   },
                                   &dispatcher);
            stackLimit.reset();
#if REREVVED_DEVELOPER
            const auto counts = TakeLowererConsumeCounts();
#endif
            if (run.originals == 0 && DEV_CHECK(counts.consumed == 1) && DEV_CHECK(fellTotal(counts) == 0))
                return true;
            UNSCOPED_INFO("a refused stack runs Original and counts stack_overlap alone");
            REQUIRE((run.originals == 1 && DEV_CHECK(counts.consumed == 0) && DEV_CHECK(counts.fallback[static_cast<std::size_t>(LowererFallback::StackOverlap)] == 1) && DEV_CHECK(fellTotal(counts) == 1)));
            return false;
        };
        const auto consumedAt = [&](std::uint32_t limit, std::uint32_t r1)
        {
            return consumedWith(none, limit, r1);
        };
        UNSCOPED_INFO("the lowerer footprint is 88 bytes above r1 and the device up to G+0x3A5C");
        REQUIRE((kLowererStackFootprint.above == 88 && kLowererStackFootprint.deviceSpan == 0x3A5C && kLowererDeviceSpan == 0x3A5C));
        requireStackEdges("lowerer", kGraphics, kLowererDeviceSpan, kLowererStackFootprint.above, consumedAt);
        UNSCOPED_INFO("the lowerer frame is the entry function's 224 bytes (g63:13048)");
        REQUIRE(kLowererStackFootprint.frame == 224);
        requireFrameEdges("lowerer", kLowererStackFootprint.frame, consumedAt);

        // The objects the model reads or writes besides the device words: the allocator's pool object and
        // progress block, and the vertex and index out words. The pool object's reach edges, the fixed pool at
        // kPool and the null pool's progress word at kProgress + 4:
        requireObjectEdges("lowerer pool object", kPool, 160, 88, kLowererStackFootprint.frame, consumedAt);
        {
            const auto progressAt = [&](std::uint32_t limit, std::uint32_t r1)
            {
                return consumedWith(nullPool, limit, r1);
            };
            requireObjectEdges("lowerer progress word", kProgress + 4, 4, 88, kLowererStackFootprint.frame, progressAt);
        }
        // The vertex out word the slot r1+84 names and the index out word r10: the reach ends where the vertex out
        // word starts, and starts where the index out word ends (the two words are adjacent).
        {
            const auto outAt = [&](std::uint32_t limit, std::uint32_t r1)
            {
                return consumedAt(limit, r1);
            };
            constexpr std::uint32_t kFrame = 224;
            UNSCOPED_INFO("a reach ending at the vertex out word is clear");
            REQUIRE(outAt(kVertexOut - 88 - kFrame, kVertexOut - 88));
            UNSCOPED_INFO("a reach one byte into the vertex out word is refused");
            REQUIRE(!outAt(kVertexOut - 88 - kFrame, kVertexOut - 88 + 1));
            UNSCOPED_INFO("a reach starting after the index out word is clear");
            REQUIRE(outAt(kIndexOut + 4, kIndexOut + 4 + kFrame));
            UNSCOPED_INFO("a reach starting on the last byte of the index out word is refused");
            REQUIRE(!outAt(kIndexOut + 3, kIndexOut + 4 + kFrame));
        }

        // The pool object G+0x34CC names at r1 - 160 puts its base word (+152) at r1 - 8, where
        // Original's link register save lands. The root is refused.
        {
            const auto poolInFrame = [&](std::vector<std::uint8_t>& memory)
            {
                store(memory.data(), kGraphics + 0x34CC, kStack - 160);
                store(memory.data(), kStack - 160 + 152, kPoolBase);
                store(memory.data(), kStack - 160 + 156, kPoolSize);
            };
            UNSCOPED_INFO("a pool object in the frame is refused");
            REQUIRE(!consumedWith(poolInFrame, kStack - 0x400, kStack));
            UNSCOPED_INFO("the same stack with the pool object elsewhere is consumed");
            REQUIRE(consumedWith(fixedPool, kStack - 0x400, kStack));

#if REREVVED_DEVELOPER
            // The exit probe tests the same objects: a pool object in the frame is counted as stack_overlap
            // although the device words are clear, and the pool elsewhere is not.
            REXCVAR_SET(native_consume_lowerer, false);
            REXCVAR_SET(native_lowerer_exit_check, true);
            TakeLowererExitCounts();
            const auto probeWith = [&](const auto& prepare)
            {
                stackLimit = kStack - 0x400;
                drive({ .r1 = kStack }, [&](std::vector<std::uint8_t>& memory)
                      {
                          slotAt(kStack)(memory);
                          prepare(memory);
                      },
                      &dispatcher);
                stackLimit.reset();
                return TakeLowererExitCounts();
            };
            auto probed = probeWith(poolInFrame);
            UNSCOPED_INFO("the probe refuses a pool object in the frame");
            REQUIRE(probed.unmodeled[static_cast<std::size_t>(LowererExitRefusal::StackOverlap)] == 1);
            probed = probeWith(fixedPool);
            UNSCOPED_INFO("the probe accepts the pool object elsewhere");
            REQUIRE(probed.unmodeled[static_cast<std::size_t>(LowererExitRefusal::StackOverlap)] == 0);
            REXCVAR_SET(native_lowerer_exit_check, false);
            REXCVAR_SET(native_consume_lowerer, true);
#endif
        }

        // The regression: Original's entry frame stores its back chain at r1 - 224 (stwu g63:13048).
        // Over the ring cursor G+0x30 it changes a word the model reads, so the root is refused.
        constexpr std::uint32_t r1     = kGraphics + 0x30 + 224;
        auto                    memory = guestMemory();
        baseline(memory);
        const auto word = load(memory.data(), kGraphics + 0x30);
        store(memory.data(), r1 - 224, r1);
        UNSCOPED_INFO("Original's back chain store changes the ring cursor");
        REQUIRE(load(memory.data(), kGraphics + 0x30) != word);
        requireFallback(drive({ .r1 = r1 }, slotAt(r1), &dispatcher), LowererFallback::StackOverlap, "a frame over G+0x30 runs Original");

        // The order: the nesting and the stubs come first; the stack comes before the tiling gate.
        requireFallback(drive({ .caller = kIndexedUpLowererReturn, .r1 = kGraphics + 0x40 }, none, &dispatcher), LowererFallback::Nested, "a nested call is reported before the stack");
        ringStubsActive.store(false);
        requireFallback(drive({ .r1 = kGraphics + 0x40 }, none, &dispatcher), LowererFallback::Stubs, "without ring stubs the stack is not reached");
        ringStubsActive.store(true);
        requireFallback(drive({ .r1 = kGraphics + 0x40 }, [](std::vector<std::uint8_t>& m)
                              {
                                  store(m.data(), kGraphics + 0x2ABC, 0x21000000);
                              },
                              &dispatcher),
                        LowererFallback::StackOverlap,
                        "the stack is reported before the tiling gate");

#if REREVVED_DEVELOPER
        // The exit probe applies the same test before it reads a guest word, and counts the call as
        // unmodeled by its own reason; a stack clear of the block is probed.
        REXCVAR_SET(native_consume_lowerer, false);
        REXCVAR_SET(native_lowerer_exit_check, true);
        TakeLowererExitCounts();
        drive({ .r1 = kGraphics + 0x40 }, slotAt(kGraphics + 0x40), &dispatcher);
        auto exits = TakeLowererExitCounts();
        UNSCOPED_INFO("the probe refuses a stack in the block");
        REQUIRE((exits.ok == 0 && exits.flushWordsCalls == 0 && exits.unmodeled[static_cast<std::size_t>(LowererExitRefusal::StackOverlap)] == 1));
        UNSCOPED_INFO("the probe counter is named stack_overlap");
        REQUIRE(FormatLowererExitCounts(exits).contains(" lowerer_exit_unmodeled_stack_overlap=1"));
        drive({ .r1 = kGraphics - 88 }, slotAt(kGraphics - 88), &dispatcher);
        exits = TakeLowererExitCounts();
        UNSCOPED_INFO("the probe runs on a stack ending exactly at G");
        REQUIRE((exits.ok == 1 && exits.unmodeled[static_cast<std::size_t>(LowererExitRefusal::StackOverlap)] == 0));
        REXCVAR_SET(native_lowerer_exit_check, false);
        REXCVAR_SET(native_consume_lowerer, true);
#endif
    }
    for (const std::uint32_t byte : { 0x01U, 0x20U, 0x21U })
    {
        const auto run = drive({}, [&](std::vector<std::uint8_t>& m)
                               {
                                   store(m.data(), kGraphics + 0x2ABC, byte << 24);
                               },
                               &dispatcher);
        requireFallback(run, LowererFallback::Tiling, "an untaken tiling byte runs Original, and 0x21 without the tiled cvar");
    }
    {
        auto run = drive({}, [](std::vector<std::uint8_t>& m)
                         {
                             store(m.data(), kGraphics + 0x14, 0x20000);
                         },
                         &dispatcher);
        requireFallback(run, LowererFallback::StateDirty, "a dirty bit that runs 0x826ADD88 runs Original");
        run = drive({ .stride = 36 }, none, &dispatcher);
        requireFallback(run, LowererFallback::StateDirty, "a stride change runs Original");
        // Without a vertex shader the stride change sets no bit.
        run = drive({ .stride = 36 }, [](std::vector<std::uint8_t>& m)
                    {
                        store(m.data(), kGraphics + 0x3198, 0);
                    },
                    &dispatcher);
        requireConsumed(run, "a stride change without a vertex shader is consumed");
    }
    {
        const auto run = drive({}, [](std::vector<std::uint8_t>& m)
                               {
                                   m[kGraphics + 0x2ABD] = 0x20;
                               },
                               &dispatcher);
        requireFallback(run, LowererFallback::AllocMarker, "the allocation failure marker runs Original");
    }
    {
        // The fixture Original has no indirect allocator, so the simple fixture stands in for it.
        exactLowerer.store(false);
        const auto run = drive({}, [](std::vector<std::uint8_t>& m)
                               {
                                   store(m.data(), kPool + 152, 0);
                               },
                               &dispatcher);
        exactLowerer.store(true);
        requireFallback(run, LowererFallback::AllocPath, "a pool without a fixed buffer runs Original");
    }
    {
        // The flush words reach the soft limit: Original may flush.
        auto run = drive({}, [](std::vector<std::uint8_t>& m)
                         {
                             store(m.data(), kGraphics + 0x1C, 1);
                             store(m.data(), kGraphics + 0x38, kRingCursor + 4 * 12 - 4);
                         },
                         &dispatcher);
        requireFallback(run, LowererFallback::RingSpace, "flush words past the soft limit run Original");
        run = drive({}, [](std::vector<std::uint8_t>& m)
                    {
                        store(m.data(), kGraphics + 0x1C, 1);
                    },
                    &dispatcher);
        requireConsumed(run, "a dirty G+0x18 below the limits is consumed");
        run = drive({}, [](std::vector<std::uint8_t>& m)
                    {
                        store(m.data(), kGraphics + 0x3A58, 100);
                    },
                    &dispatcher);
        requireFallback(run, LowererFallback::RingSpace, "a take past the pool threshold runs Original");
    }
    {
        const auto run = drive({}, [](std::vector<std::uint8_t>& m)
                               {
                                   // A pool whose top lies below the ring cursor plus 164.
                                   store(m.data(), kPool + 152, 0x200);
                                   store(m.data(), kPool + 156, 0x100);
                               },
                               &dispatcher);
        requireFallback(run, LowererFallback::AllocCapacity, "a pool too small for the takes runs Original");
    }
    {
        // The GPU is far behind the wrapped take: 0x8269B158 would wait. The fixture Original cannot wait, so the root is judged by its verdict alone.
        const auto prepare = [&](std::vector<std::uint8_t>& m)
        {
            nullPool(m);
            store(m.data(), kGraphics + 0x3A44, 0x6F00 - 100);
            store(m.data(), kProgress + 4, 0);
        };
        exactLowerer.store(false);
        const auto run = drive({}, prepare, &dispatcher);
        exactLowerer.store(true);
        requireFallback(run, LowererFallback::AllocWait, "a null pool that would wait runs Original");
    }
#if REREVVED_DEVELOPER
    {
        REXCVAR_SET(native_lowerer_exit_check, true);
        const auto run = drive({}, none, &dispatcher);
        REXCVAR_SET(native_lowerer_exit_check, false);
        TakeLowererExitCounts();
        requireFallback(run, LowererFallback::Diagnostics, "the probe needs the body");
    }
#endif

    // The tiled shape, with its tripwires.
    {
        REXCVAR_SET(native_consume_lowerer_tiled, true);
        const auto tile = [](std::vector<std::uint8_t>& m)
        {
            store(m.data(), kGraphics + 0x2ABC, 0x21000000);
            store(m.data(), kGraphics + 0x33B0, 1);
        };
        REXCVAR_SET(native_consume_lowerer, false);
        const auto tiledOriginal = drive({}, tile, &dispatcher);
        REXCVAR_SET(native_consume_lowerer, true);
        DEV_DO(TakeLowererConsumeCounts());
        const auto tiled = drive({}, tile, &dispatcher);
#if REREVVED_DEVELOPER
        const auto counts = TakeLowererConsumeCounts();
#endif
        UNSCOPED_INFO("a tiled root with its tripwires held is consumed");
        REQUIRE((tiled.originals == 0 && DEV_CHECK(counts.consumed == 1) && DEV_CHECK(counts.consumedTiled == 1) && DEV_CHECK(fellTotal(counts) == 0)));
        UNSCOPED_INFO("a consumed tiled call leaves the words Original does, but the ring and the tile record");
        REQUIRE(sameButRing(tiled, tiledOriginal));
        UNSCOPED_INFO("the tile record is skipped like the ring words");
        REQUIRE((load(tiledOriginal.after.data(), kGraphics + 0x33C8) == kTileList + 16 && load(tiled.after.data(), kGraphics + 0x33C8) == kTileList));
        UNSCOPED_INFO("the tiled call stages 27 ring words");
        REQUIRE(load(tiledOriginal.after.data(), kGraphics + 0x3484) == kRingCursor + 4 * 27);
        auto run = drive({}, [](std::vector<std::uint8_t>& m)
                         {
                             store(m.data(), kGraphics + 0x2ABC, 0x21000000);
                         },
                         &dispatcher);
        requireFallback(run, LowererFallback::TiledState, "a tiled root with the bracket clear runs Original");
        exactLowerer.store(false); // the fixture Original does not grow the tile list
        run = drive({}, [&](std::vector<std::uint8_t>& m)
                    {
                        tile(m);
                        store(m.data(), kGraphics + 0x33C8, kTileList + 0xF1);
                    },
                    &dispatcher);
        exactLowerer.store(true);
        requireFallback(run, LowererFallback::TileGrowth, "a full tile list runs Original");
        REXCVAR_SET(native_consume_lowerer_tiled, false);
    }

    // The counters format.
    {
#if REREVVED_DEVELOPER
        LowererConsumeCounts counts;
        counts.consumed                                                          = 5;
        counts.consumedTiled                                                     = 2;
        counts.fallback[static_cast<std::size_t>(LowererFallback::RingSpace)]    = 3;
        counts.fallback[static_cast<std::size_t>(LowererFallback::StackOverlap)] = 2;
        const auto line                                                          = FormatLowererConsumeCounts(counts);
        UNSCOPED_INFO(("the counters line: " + line).c_str());
        REQUIRE(line == " draw_consumed_lowerer=5 draw_consumed_lowerer_tiled=2 draw_fallback_lowerer_stack_overlap=2 draw_fallback_lowerer_ring_space=3");
#endif
    }
    REXCVAR_SET(native_consume_lowerer, false);
    exactLowerer.store(false);
    ringStubsActive.store(savedActive);
    ringStubsValidatedDevice.store(savedDevice);
}

void testConsumedChildNote()
{
    struct Seen
    {
        NativeDrawFrameCounters totals;
        NativeDrawCensus        census;
    };

    // `body` runs inside a fresh dispatcher.
    const auto observe = [](auto&& body)
    {
        ScopedDispatcher dispatcher(skipConfig());
        auto             memory = guestMemory();
        EnableNativeDrawCensus(true);
        TakeNativeDrawCensus();
        install(dispatcher);
        body(memory.data());
        remove();
        EnableNativeDrawCensus(false);
        return Seen{ dispatcher.Totals(), TakeNativeDrawCensus() };
    };
    const auto slotOf = [](NativeDrawHook hook)
    {
        return static_cast<std::size_t>(hook);
    };
    const auto mismatches = [](const Seen& seen)
    {
        return reasonCount(seen.totals.skipped, NativeDrawReason::NestingMismatch);
    };

    for (const auto [outer, child, caller] : { std::tuple{ NativeDrawHook::IndexedUp, NativeDrawHook::IndexedUpLowerer, 0x82303E90U },
                                               std::tuple{ NativeDrawHook::NonindexedUp, NativeDrawHook::NonindexedSink, 0x82304258U } })
    {
        // Noted twice, it counts the one child once.
        auto seen = observe([&, outer = outer, caller = caller](std::uint8_t* memory)
                            {
                                auto                  ctx = entry(caller);
                                const NativeDrawScope scope(outer, ctx, memory);
                                NoteNativeDrawConsumedChild(scope);
                                NoteNativeDrawConsumedChild(scope);
                            });
        UNSCOPED_INFO("a noted child counts once and is no nesting mismatch");
        REQUIRE((seen.totals.attempted == 1 && seen.totals.nested == 1 && seen.totals.invocations[slotOf(outer)] == 1 && seen.totals.invocations[slotOf(child)] == 1 &&
                 mismatches(seen) == 0 && reasonCount(seen.totals.skipped, NativeDrawReason::NoNativeRoute) == 1 && seen.census.nested[slotOf(child)] == 1));

        // A child that really ran is not counted again, noted from the root or from the child.
        seen = observe([&, outer = outer, child = child, caller = caller](std::uint8_t* memory)
                       {
                           auto                  ctx = entry(caller);
                           const NativeDrawScope scope(outer, ctx, memory);
                           {
                               auto childCtx   = entry(outer == NativeDrawHook::IndexedUp ? kIndexedUpLowererReturn : kNonindexedSinkReturn);
                               childCtx.r1.u64 = kStack - 0xB0;
                               const NativeDrawScope inner(child, childCtx, memory);
                               NoteNativeDrawConsumedChild(inner);
                           }
                           NoteNativeDrawConsumedChild(scope);
                       });
        UNSCOPED_INFO("a child that ran is not counted again");
        REQUIRE((seen.totals.attempted == 1 && seen.totals.nested == 1 && seen.totals.invocations[slotOf(child)] == 1 && mismatches(seen) == 0 &&
                 seen.census.nested[slotOf(child)] == 1));

        // A root draw overtaken by the next one at the same stack level is no longer open.
        seen = observe([&, outer = outer, caller = caller](std::uint8_t* memory)
                       {
                           auto                  ctx = entry(caller);
                           const NativeDrawScope stale(outer, ctx, memory);
                           const NativeDrawScope current(outer, ctx, memory);
                           NoteNativeDrawConsumedChild(stale);
                       });
        UNSCOPED_INFO("an abandoned root's scope notes nothing on the root that replaced it");
        REQUIRE((seen.totals.attempted == 1 && seen.totals.abandoned == 1 && seen.totals.nested == 0 && seen.totals.invocations[slotOf(child)] == 0 && mismatches(seen) == 1 &&
                 seen.census.nested[slotOf(child)] == 0));
    }

    // A root without a static child call has nothing to note.
    auto seen = observe([](std::uint8_t* memory)
                        {
                            auto                  ctx = entry(0x8250C768);
                            const NativeDrawScope scope(NativeDrawHook::IndexedBuffer, ctx, memory);
                            NoteNativeDrawConsumedChild(scope);
                        });
    UNSCOPED_INFO("an IndexedBuffer root has no child to note");
    REQUIRE((seen.totals.attempted == 1 && seen.totals.nested == 0 && seen.totals.invocations[slotOf(NativeDrawHook::IndexedBuffer)] == 1 &&
             seen.totals.invocations[slotOf(NativeDrawHook::IndexedUpLowerer)] == 0 && seen.totals.invocations[slotOf(NativeDrawHook::NonindexedSink)] == 0 &&
             mismatches(seen) == 0));

    // A root the next draw closed after the dispatcher was withdrawn is not open either.
    seen = observe([](std::uint8_t* memory)
                   {
                       auto                  ctx = entry(0x82303E90);
                       const NativeDrawScope closed(NativeDrawHook::IndexedUp, ctx, memory);
                       UNSCOPED_INFO("a withdrawal inside a draw returns false");
                       REQUIRE(!RemoveNativeDrawDispatcher(1s));
                       const NativeDrawScope next(NativeDrawHook::IndexedUp, ctx, memory);
                       UNSCOPED_INFO("no dispatcher observes the draw after the withdrawal");
                       REQUIRE(!next.Observed());
                       NoteNativeDrawConsumedChild(closed);
                       UNSCOPED_INFO("a closed root's scope is no observed wrapper");
                       REQUIRE(UserPointerConsumeVerdict(UserPointerWrapper::IndexedUp, closed, kGraphics, EntryStride{}, false) == UserPointerFallback::Dispatcher);
                   });
    UNSCOPED_INFO("a closed root's scope notes nothing");
    REQUIRE((seen.totals.attempted == 0 && seen.totals.abandoned == 1 && seen.totals.nested == 0 && seen.census.nested[slotOf(NativeDrawHook::IndexedUpLowerer)] == 0));
}

// Fixture guest memory for the tile record list header.
class ListMemory final : public IRingStubMemory
{
public:
    std::optional<std::uint32_t> Read32(std::uint32_t address) override
    {
        if (const auto found = words.find(address); found != words.end())
            return found->second;
        return std::nullopt;
    }

    std::optional<std::uint8_t> Read8(std::uint32_t) override
    {
        return std::nullopt;
    }

    bool Write32(std::uint32_t, std::uint32_t) override
    {
        return false;
    }

    bool Write8(std::uint32_t, std::uint8_t) override
    {
        return false;
    }

    std::optional<std::uint64_t> Exchange64(std::uint32_t, std::uint64_t) override
    {
        return std::nullopt;
    }

    std::optional<std::uint32_t> Physical(std::uint32_t) override
    {
        return std::nullopt;
    }

    std::map<std::uint32_t, std::uint32_t> words;
};

// Tile-list shape follows chunk/cursor words; EndTiling observes counters.
void testTileListShape()
{
    constexpr std::uint32_t kChunk = 0x00200000;
    const auto              shape  = [&](std::uint32_t cursor)
    {
        return ClassifyTileList(kChunk, cursor);
    };
    // Records start at chunk + 4, 16 bytes each.
    UNSCOPED_INFO("a cursor at the first record is an empty list");
    REQUIRE((shape(kChunk + 4).empty && shape(kChunk + 4).records == 0 && !shape(kChunk + 4).misaligned));
    UNSCOPED_INFO("one record");
    REQUIRE((shape(kChunk + 4 + 16).records == 1 && !shape(kChunk + 4 + 16).empty && !shape(kChunk + 4 + 16).misaligned));
    UNSCOPED_INFO("a full chunk of records");
    REQUIRE(shape(kChunk + 4 + 16 * 263).records == 263);
    for (const std::uint32_t cursor : { kChunk + 4 + 8U, kChunk + 4 + 15U, kChunk + 4 + 17U, kChunk + 4 + 16 + 4U })
    {
        UNSCOPED_INFO("a cursor off a record boundary is misaligned");
        REQUIRE((shape(cursor).misaligned && !shape(cursor).empty && shape(cursor).records == 0));
    }
    for (const std::uint32_t cursor : { 0U, kChunk, kChunk + 3, kChunk - 16 })
    {
        UNSCOPED_INFO("a cursor before the first record is misaligned");
        REQUIRE((shape(cursor).misaligned && !shape(cursor).empty && shape(cursor).records == 0));
    }
    // The first record's address does not wrap around the 32-bit space.
    UNSCOPED_INFO("a chunk at the top of the address space does not wrap");
    REQUIRE((ClassifyTileList(0xFFFFFFFC, 0).misaligned && ClassifyTileList(0xFFFFFFFC, 0xFFFFFFFF).misaligned));

    DEV_DO(TakeTileListCounts());
    constexpr std::uint32_t kDevice = 0x1000;
    const auto              observe = [&](std::optional<std::uint32_t> chunk, std::optional<std::uint32_t> cursor)
    {
        ListMemory memory;
        if (chunk)
            memory.words[kDevice + kTileListChunkOffset] = *chunk;
        if (cursor)
            memory.words[kDevice + kTileListCursorOffset] = *cursor;
        ObserveTileList(memory, kDevice);
    };
    observe(kChunk, kChunk + 4 + 16 * 3);
    observe(kChunk, kChunk + 4 + 16 * 2);
    observe(kChunk, kChunk + 4);
    observe(kChunk, kChunk + 4 + 8);
    observe(kChunk, kChunk);
    observe(kChunk, std::nullopt);
    observe(std::nullopt, kChunk + 4);
    ListMemory none;
    ObserveTileList(none, 0);
    ObserveTileList(none, 0xFFFFFFF0);
#if REREVVED_DEVELOPER
    const auto counts = TakeTileListCounts();
    UNSCOPED_INFO("the list counters sum records, empty and misaligned brackets");
    REQUIRE((counts.records == 5 && counts.empty == 1 && counts.misaligned == 6));
    UNSCOPED_INFO("the list counters format");
    REQUIRE(FormatTileListCounts(counts) == " tiling_list_records=5 tiling_list_empty=1 tiling_list_misaligned=6");
    const auto taken = TakeTileListCounts();
    UNSCOPED_INFO("taking the list counters resets them");
    REQUIRE((taken.records == 0 && taken.empty == 0 && taken.misaligned == 0));
#endif

#if REREVVED_DEVELOPER
    // The consume counters carry the tiled fields in the timing format.
    IndexedBufferConsumeCounts consume;
    consume.consumed                                                                = 3;
    consume.consumedTiled                                                           = 2;
    consume.consumedDirty                                                           = 1;
    consume.consumedDirtyVertexFetch                                                = 7;
    consume.fallback[static_cast<std::size_t>(IndexedBufferFallback::TiledState)]   = 4;
    consume.fallback[static_cast<std::size_t>(IndexedBufferFallback::DirtyMode)]    = 5;
    consume.fallback[static_cast<std::size_t>(IndexedBufferFallback::StackOverlap)] = 6;
    UNSCOPED_INFO("the consume counters format with the tiled, dirty and dirty vertex fetch fields");
    REQUIRE(FormatIndexedBufferConsumeCounts(consume) ==
            " draw_consumed_indexed_buffer=3 draw_consumed_indexed_buffer_tiled=2 draw_consumed_indexed_buffer_dirty=1 draw_consumed_indexed_buffer_dirty_vertex_fetch=7 "
            "draw_fallback_indexed_buffer_stack_overlap=6 "
            "draw_fallback_indexed_buffer_dirty_mode=5 draw_fallback_indexed_buffer_tiled_state=4");

    // The wrapper counters name each wrapper's fields and reasons.
    UNSCOPED_INFO("the wrapper counters format with no fallback fields when none counted");
    REQUIRE(FormatUserPointerConsumeCounts({}) ==
            " draw_consumed_indexed_up=0 draw_consumed_nonindexed_up=0 draw_consumed_user_pointer_tiled=0 draw_consumed_user_pointer_dirty=0 draw_consumed_user_pointer_dirty_vertex_fetch=0");
    UserPointerConsumeCounts wrappers;
    wrappers.consumed                                                                 = { 3, 5 };
    wrappers.consumedTiled                                                            = 2;
    wrappers.consumedDirty                                                            = 1;
    wrappers.consumedDirtyVertexFetch                                                 = 2;
    wrappers.fallback[0][static_cast<std::size_t>(UserPointerFallback::Tiling)]       = 4;
    wrappers.fallback[0][static_cast<std::size_t>(UserPointerFallback::DirtyMode)]    = 6;
    wrappers.fallback[1][static_cast<std::size_t>(UserPointerFallback::Diagnostics)]  = 7;
    wrappers.fallback[0][static_cast<std::size_t>(UserPointerFallback::StackOverlap)] = 8;
    wrappers.fallback[1][static_cast<std::size_t>(UserPointerFallback::StackOverlap)] = 9;
    UNSCOPED_INFO("the wrapper counters format with the tiled, dirty and dirty vertex fetch fields and per-wrapper fallbacks");
    REQUIRE(FormatUserPointerConsumeCounts(wrappers) ==
            " draw_consumed_indexed_up=3 draw_consumed_nonindexed_up=5 draw_consumed_user_pointer_tiled=2 draw_consumed_user_pointer_dirty=1 draw_consumed_user_pointer_dirty_vertex_fetch=2"
            " draw_fallback_indexed_up_stack_overlap=8 draw_fallback_indexed_up_tiling=4 draw_fallback_indexed_up_dirty_mode=6"
            " draw_fallback_nonindexed_up_stack_overlap=9 draw_fallback_nonindexed_up_diagnostics=7");
#endif
}

void testSceneParser()
{
    std::string text;
    const auto  event = [&](std::uint64_t sequence, std::uint64_t frame, const char* kind, std::uint32_t entry, std::uint32_t caller)
    {
        // A nested multi-line array with lines starting with '[' precedes the fields.
        text += "[[events]]\nraw_matrix = [\n    [ 1, 2 ],\n    [ 3, 4 ]\n]\ncaller_lr = '" + std::to_string(caller) +
                "'\nentry = " + std::to_string(entry) + "\nframe = " + std::to_string(frame) + "\nkind = '" + kind +
                "'\nraw_r3_r10_u64 = [ '" + std::to_string(kGraphics) + "', '4' ]\nsequence = " + std::to_string(sequence) +
                "\n\n    [events.vs]\n    entry = 1\n    kind = 'draw'\n\n    [[events.bindings]]\n    base = 0\n\n";
    };
    event(1, 10, "create_begin", 0x8269AB90, 0x82000000);
    event(2, 10, "draw", 0x826A39F8, 0x8250C768);
    event(3, 10, "draw", 0x826A3568, 0x82303E90);
    event(4, 10, "draw", 0x826A3048, kIndexedUpLowererReturn);
    event(5, 10, "draw", 0x826A3000, 0x82304258);
    event(6, 10, "draw", 0x826A2B58, kNonindexedSinkReturn);
    event(7, 11, "draw", 0x826A3048, 0x82510BFC);
    event(8, 11, "draw", 0x826A39F8, 0x8250C768);
    std::vector<SceneDraw> draws;
    std::string            error;
    std::istringstream     stream(text);
    UNSCOPED_INFO("parser keeps only top-level draw fields");
    REQUIRE((loadSceneDraws(stream, draws, error) && draws.size() == 7 && draws[2].caller == kIndexedUpLowererReturn &&
             draws[2].graphics == kGraphics));
    const auto result = replaySceneDraws(draws);
    UNSCOPED_INFO("scene replay normalizes nested children and keeps direct lowerers");
    REQUIRE((result.totals.attempted == 5 && result.totals.nested == 2 && result.paired == 2 && result.frames.size() == 2 &&
             result.frames[0].attempted == 3 && result.frames[1].attempted == 2 &&
             reasonCount(result.totals.skipped, NativeDrawReason::NestingMismatch) == 0));

    for (const char* broken : { "[[events]]\nentry = 1\n", "[[events]]\nkind = \"draw\"\n", "[[events]]\nkind = 'draw'\nframe = '12\n", "[[events]]\nkind = 'draw'\nentry = 1\n", "[[events]]\nraw = [\n    [ 1 ]\n" })
    {
        std::istringstream bad(broken);
        UNSCOPED_INFO("parser rejects missing kind, foreign quoting, missing fields and open arrays");
        REQUIRE(!loadSceneDraws(bad, draws, error));
    }
}

int replayScene(const std::filesystem::path& events, const char* expected)
{
    std::vector<SceneDraw> draws;
    std::string            error;
    if (!loadSceneDraws(events, draws, error) || draws.empty())
    {
        std::cerr << (error.empty() ? "no draw events" : error) << '\n';
        return 1;
    }
    const auto result     = replaySceneDraws(draws);
    const auto mismatches = reasonCount(result.totals.skipped, NativeDrawReason::NestingMismatch);
    std::cout << "snapshots " << result.snapshots << " submissions " << result.totals.attempted << " nested "
              << result.totals.nested << " adjacent_pairs " << result.paired << " nesting_mismatch " << mismatches << '\n';
    for (std::size_t i = 0; i < result.frames.size(); ++i)
        std::cout << "frame " << result.captureFrames[i] << " submissions " << result.frames[i].attempted << " nested "
                  << result.frames[i].nested << '\n';
    if (result.snapshots != result.totals.attempted + result.totals.nested || result.paired != result.totals.nested ||
        mismatches != 0)
        return 1;
    return expected && result.totals.attempted != std::stoull(expected) ? 1 : 0;
}

// The fixture Originals, label adapter capture and import doubles for one case.
class DispatchFixtures
{
public:
    DispatchFixtures() = default;

    ~DispatchFixtures()
    {
#if REREVVED_DEVELOPER
        claimLabelAdapter = false;
#endif
    }

    DispatchFixtures(const DispatchFixtures&)            = delete;
    DispatchFixtures& operator=(const DispatchFixtures&) = delete;

private:
    using Import = rerevved::tests::guest_imports::ScopedImport;
#if REREVVED_DEVELOPER
    using LabelAdapterCapture = rerevved::gpu::diagnostics::LabelAdapterCapture;

    static rerevved::tests::production_observers::LabelAdapterObserver makeLabel()
    {
        rerevved::tests::production_observers::LabelAdapterObserver observer;
        observer.begin = [](const PPCContext&)
        {
            return claimLabelAdapter ? claimedAdapter<LabelAdapterCapture>() : std::shared_ptr<LabelAdapterCapture>{};
        };
        observer.finish = [](const std::shared_ptr<LabelAdapterCapture>&, std::uint32_t)
        {
        };
        observer.patch = [](std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint64_t)
        {
        };
        observer.selection = [](std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)
        {
        };
        return observer;
    }

    rerevved::tests::production_observers::LabelAdapterObserver       label_{ makeLabel() };
    rerevved::tests::production_observers::ScopedLabelAdapterObserver labelScope_{ &label_ };
#endif
    rerevved::tests::guest_imports::ScopedFallbackImport originals_{ [](std::uint32_t address, PPCContext& ctx, std::uint8_t* base)
                                                                     {
                                                                         original(address, ctx, base);
                                                                     } };
    Import commit_{ 0x826A2FF0, fixtureSub826A2FF0 };
    Import lowerer_{ 0x826A3568, fixtureSub826A3568 };
    Import sink_{ 0x826A3000, fixtureSub826A3000 };
    Import buffer_{ 0x826A39F8, fixtureSub826A39F8 };
    Import raw1_{ 0x826AD150, fixtureSub826AD150 };
    Import raw2_{ 0x826A7040, fixtureSub826A7040 };
    Import raw3_{ 0x826A7138, fixtureSub826A7138 };
};

void runDispatch()
{
    DispatchFixtures fixtures;
    UNSCOPED_INFO("only the six draw entries are dispatched");
    REQUIRE((FindNativeDrawHook(0x826A3048) == NativeDrawHook::IndexedUpLowerer && !FindNativeDrawHook(0x826AD150)));
    testRouteCensus();
    testNormalizationAndAbi();
    testFailurePropagation();
    testNestingMismatch();
    testStrictMode();
    testSequencer();
    testSequencerRemoval();
    testFirstSeenBound();
    testThreads();
    testRemovalWaitsForInFlight();
    testStaleRoot();
    testRemoveFromCallback();
    testExitingThreadReleasesStaleRoot();
    testHeldLowerer();
    testDrawDirtyReplay();
    testDrawDirtyPreflight();
    testDrawDirtyRetainedReaders();
    testDrawDirtyLaterOriginal();
    testConsumeIndexedBuffer();
    testConsumeUserPointer();
    testConsumesWaitForPublishedStubs();
    testConsumeLowerer();
    testConsumedChildNote();
    testTileListShape();
    testSceneParser();
}

} // namespace

TEST_CASE("native_draw_dispatch", "[ctest][native-draw-dispatch][timeout-60]")
{
    runDispatch();
}

TEST_CASE("native_draw_dispatch_resolve_tiling", "[ctest][native-draw-dispatch][timeout-60]")
{
    bool drain = true;
    SECTION("EndTiling drains the queued reader before committing B")
    {
        drain = true;
    }
    SECTION("without a drain the delayed reader observes B")
    {
        drain = false;
    }
    DispatchFixtures fixtures;
    testResolveTilingDrain(drain);
}

// Runs one dirty-replay test with the dispatch fixtures:
//   native-draw-dispatch.dirty <testDrawDirtyReplay | testDrawDirtyPreflight | testDrawDirtyRetainedReaders | testDrawDirtyLaterOriginal>
TEST_CASE("native_draw_dispatch_dirty", "[.][manual][native-draw-dispatch]")
{
    const auto& args = rerevved::tests::CurrentManualArgv();
    REQUIRE(args.argc() == 1);
    DispatchFixtures fixtures;
    if (args[1] == "testDrawDirtyReplay")
        testDrawDirtyReplay();
    else if (args[1] == "testDrawDirtyPreflight")
        testDrawDirtyPreflight();
    else if (args[1] == "testDrawDirtyRetainedReaders")
        testDrawDirtyRetainedReaders();
    else if (args[1] == "testDrawDirtyLaterOriginal")
        testDrawDirtyLaterOriginal();
    else
    {
        UNSCOPED_INFO("an unknown dirty replay test");
        REQUIRE(false);
    }
}

// Replays a scene capture's draw sequence offline:
//   native-draw-dispatch.scene <events.toml> [submissions]
TEST_CASE("native_draw_dispatch_scene", "[.][manual][native-draw-dispatch]")
{
    const auto& args = rerevved::tests::CurrentManualArgv();
    REQUIRE((args.argc() == 1 || args.argc() == 2));
    DispatchFixtures fixtures;
    UNSCOPED_INFO("the scene replay reports a mismatch");
    REQUIRE(replayScene(std::string(args[1]), args.argc() == 2 ? args.argv()[2] : nullptr) == 0);
}

REREVVED_MANUAL_HELPER(nativeDrawDispatchDirty, "native-draw-dispatch.dirty", "native_draw_dispatch_dirty")
REREVVED_MANUAL_HELPER(nativeDrawDispatchScene, "native-draw-dispatch.scene", "native_draw_dispatch_scene")
