// window_writer_consume checks admission, exit probe and fallback order against literal
// Original emulation.

#include "gpu/guest_d3d/window_writer_consume.h"

#include "developer_checks.hpp"

#include "window_writer_fixture.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

REXCVAR_DECLARE(bool, native_consume_window_writer);
DEV_CVAR_DECLARE(bool, native_window_writer_exit_check);

using namespace rerevved::gpu::guest_d3d;
using namespace window_writer_fixture;

namespace
{

// Guest words in a map; every store is logged.
class FakeMemory final : public IRingStubMemory
{
public:
    explicit FakeMemory(Memory initial)
    : words(std::move(initial))
    {
    }

    std::optional<std::uint32_t> Read32(std::uint32_t address) override
    {
        if (gate)
            std::exchange(gate, nullptr)();
        ++reads;
        if (refused.contains(address))
            return std::nullopt;
        const auto found = words.find(address);
        return found == words.end() ? std::nullopt : std::optional<std::uint32_t>(found->second);
    }

    std::optional<std::uint8_t> Read8(std::uint32_t address) override
    {
        const auto word = Read32(address & ~3U);
        return word ? std::optional<std::uint8_t>(static_cast<std::uint8_t>(*word >> (24 - 8 * (address & 3)))) : std::nullopt;
    }

    bool Write32(std::uint32_t address, std::uint32_t value) override
    {
        log.emplace_back(address, value);
        words[address] = value;
        return true;
    }

    bool Write8(std::uint32_t address, std::uint8_t) override
    {
        log.emplace_back(address, 0);
        return true;
    }

    std::optional<std::uint64_t> Exchange64(std::uint32_t, std::uint64_t) override
    {
        return std::nullopt;
    }

    std::optional<std::uint32_t> Physical(std::uint32_t) override
    {
        return std::nullopt;
    }

    Memory                                               words;
    std::set<std::uint32_t>                              refused;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> log;
    int                                                  reads = 0;
    // Runs once, on the next read, before it is counted.
    std::function<void()> gate;
};

constexpr std::uint32_t kStack = 0x70000000;
// The thread's stack: the KPCR at r13 holds the base at +0x70 and the limit at +0x74.
constexpr std::uint32_t kPcr = 0x70100000;

void placeStack(Memory& memory, std::uint32_t limit, std::uint32_t base)
{
    memory[kPcr + 0x70] = base;
    memory[kPcr + 0x74] = limit;
}

// The device words of `device` and a thread stack [kStack - 0x1000, kStack + 0x1000) clear of them.
Memory built(const Device& device)
{
    auto memory = build(device);
    placeStack(memory, kStack - 0x1000, kStack + 0x1000);
    return memory;
}

// The return addresses of the call sites.
constexpr std::uint32_t kLrF9c0 = 0x8268FAA0, kLr0040 = 0x826900F8, kLr22d0 = 0x82693110, kLr8720 = 0x82698AA0, kLrClear = 0x826A2948, kLrResolveA = 0x8269E964, kLrResolveB = 0x8269F1C4;

WindowWriterEntry entryOf(std::int32_t x0 = 0, std::int32_t y0 = 0, std::int32_t x1 = 1280, std::int32_t y1 = 720)
{
    return { kG, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0), static_cast<std::uint32_t>(x1), static_cast<std::uint32_t>(y1), kStack, kPcr };
}

PPCContext contextOf(const WindowWriterEntry& entry, std::uint32_t lr = kLrF9c0)
{
    PPCContext ctx{};
    ctx.r1.u64  = kStack;
    ctx.r13.u64 = kPcr;
    ctx.r3.u64  = entry.g;
    ctx.r4.u64  = entry.x0;
    ctx.r5.u64  = entry.y0;
    ctx.r6.u64  = entry.x1;
    ctx.r7.u64  = entry.y1;
    ctx.r11.u64 = 0x1111;
    ctx.r12.u64 = 0x2222;
    ctx.lr      = lr;
    return ctx;
}

Device tiledDevice(std::uint32_t count, bool extra = false)
{
    Device device;
    device.tiling    = 0x10;
    device.tileCount = count;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        Tile tile;
        tile.rect   = { static_cast<std::int32_t>(160 * i), 0, static_cast<std::int32_t>(160 * (i + 1)), 720 };
        tile.offset = { static_cast<std::int32_t>(160 * i), 0 };
        device.tiles.push_back(tile);
    }
    if (extra)
    {
        device.tiling |= 0x40;
        device.flags = 0x20;
    }
    return device;
}

// Published services; `ready` also names kG as the device they checked.
struct Stubs
{
    Stubs(bool ready, bool published = true)
    {
        ringStubsActive.store(published, std::memory_order_release);
        ringStubsValidatedDevice.store(ready ? kG : 0, std::memory_order_release);
    }

    ~Stubs()
    {
        ringStubsActive.store(false, std::memory_order_release);
        ringStubsValidatedDevice.store(0, std::memory_order_release);
    }
};

// Sets both cvars for its scope and restores the values it found.
struct Cvars
{
    Cvars(bool consume, [[maybe_unused]] bool probe)
    : savedConsume(REXCVAR_GET(native_consume_window_writer))
#if REREVVED_DEVELOPER
    , savedProbe(REXCVAR_GET(native_window_writer_exit_check))
#endif
    {
        REXCVAR_SET(native_consume_window_writer, consume);
        DEV_CVAR_SET(native_window_writer_exit_check, probe);
    }

    ~Cvars()
    {
        REXCVAR_SET(native_consume_window_writer, savedConsume);
#if REREVVED_DEVELOPER
        REXCVAR_SET(native_window_writer_exit_check, savedProbe);
#endif
    }

    bool savedConsume;
#if REREVVED_DEVELOPER
    bool savedProbe;
#endif
};

WindowWriterConsumeState readyState()
{
    WindowWriterConsumeState state;
    state.stubsReady = true;
    return state;
}

std::optional<WindowWriterFallback> verdictOf(const WindowWriterConsumeState& state, const Memory& memory, WindowWriterPlan* plan = nullptr)
{
    return WindowWriterConsumeVerdict(state, readerOf(memory), entryOf(), plan);
}

// The call with r1 = `sp` on the thread stack [limit, sp + 0x1000).
std::optional<WindowWriterFallback> verdictAt(const WindowWriterConsumeState& state, Memory memory, std::uint32_t limit, std::uint32_t sp)
{
    placeStack(memory, limit, sp + 0x1000);
    auto entry = entryOf();
    entry.sp   = sp;
    return WindowWriterConsumeVerdict(state, readerOf(memory), entry);
}

// The stack bytes Original's prologue stores (g153:12641-12645 and
// __savegprlr_21, g29:15204): the back chain word at sp-176, the eleven doublewords
// of r21..r31 at sp-96 and the word of r12 at sp-8.
std::vector<std::pair<std::uint32_t, std::uint32_t>> prologueStores(std::uint32_t sp)
{
    return { { sp - 176, 4 }, { sp - 96, 88 }, { sp - 8, 4 } };
}

// Original's prologue on the device words: every device word it stores is overwritten.
Memory afterPrologue(Memory memory, std::uint32_t sp)
{
    for (const auto& [address, bytes] : prologueStores(sp))
        for (std::uint32_t offset = 0; offset < bytes; offset += 4)
            if (memory.contains((address + offset) & ~3U))
                memory[(address + offset) & ~3U] = 0xDEADBEEF;
    return memory;
}

void testVerdict()
{
    const auto       plain = built(Device{});
    WindowWriterPlan plan;
    UNSCOPED_INFO("an untiled call with room is consumed");
    REQUIRE(!verdictOf(readyState(), plain, &plan));
    UNSCOPED_INFO("the plan holds the exit");
    REQUIRE((plan.exit.form == WindowWriterForm::Plain && plan.exit.words == 4 && plan.watch.cursor == Device{}.cursor && plan.entry.g == kG));

    UNSCOPED_INFO("stubs not ready");
    REQUIRE(verdictOf({ false, false }, plain) == WindowWriterFallback::Stubs);
    UNSCOPED_INFO("diagnostics");
    REQUIRE(verdictOf({ true, true }, plain) == WindowWriterFallback::Diagnostics);
    // Stubs are tested before any guest word is read.
    int        asked    = 0;
    const auto counting = [&asked](std::uint32_t, std::uint32_t&)
    {
        ++asked;
        return false;
    };
    UNSCOPED_INFO("a device without stubs is not read");
    REQUIRE((WindowWriterConsumeVerdict({ false, false }, counting, entryOf()) == WindowWriterFallback::Stubs && asked == 0));

    auto unreadable = plain;
    unreadable.erase(kG + 0x38);
    UNSCOPED_INFO("unreadable limit");
    REQUIRE(verdictOf(readyState(), unreadable) == WindowWriterFallback::Unreadable);
    unreadable = plain;
    unreadable.erase(kG + 0x2ABC);
    UNSCOPED_INFO("unreadable mode");
    REQUIRE(verdictOf(readyState(), unreadable) == WindowWriterFallback::Unreadable);
    {
        Device tiled  = tiledDevice(2);
        auto   memory = built(tiled);
        memory.erase(kG + 0x31CC);
        UNSCOPED_INFO("unreadable tile count");
        REQUIRE(verdictOf(readyState(), memory) == WindowWriterFallback::Unreadable);
    }
    {
        auto reader = [](std::uint32_t, std::uint32_t&) -> bool
        {
            throw std::runtime_error("reader");
        };
        UNSCOPED_INFO("a reader that throws is unreadable");
        REQUIRE(WindowWriterConsumeVerdict(readyState(), reader, entryOf()) == WindowWriterFallback::Unreadable);
    }

    Device late;
    late.cursor = late.limit38 + 4;
    UNSCOPED_INFO("cursor past the limit");
    REQUIRE(verdictOf(readyState(), built(late)) == WindowWriterFallback::RingSpace);
    const auto tooMany = tiledDevice(17);
    UNSCOPED_INFO("too many tiles");
    REQUIRE(verdictOf(readyState(), built(tooMany)) == WindowWriterFallback::TileCount);
    auto tiledLate    = tiledDevice(3);
    tiledLate.limit38 = tiledLate.cursor + 4 * (2 + 21) - 4;
    UNSCOPED_INFO("a tile past the limit");
    REQUIRE(verdictOf(readyState(), built(tiledLate)) == WindowWriterFallback::RingSpace);
    tiledLate.limit38 += 4;
    UNSCOPED_INFO("the last tile at the limit is consumed");
    REQUIRE(!verdictOf(readyState(), built(tiledLate)));

    // The order: stubs, a device that wraps (unreadable), the stack, unreadable words, ring space (entry), tile count,
    // ring space (tiles), diagnostics.
    auto everything   = tooMany;
    everything.cursor = everything.limit38 + 4;
    auto memory       = built(everything);
    UNSCOPED_INFO("order: stubs first");
    REQUIRE(verdictOf({ false, true }, memory) == WindowWriterFallback::Stubs);
    memory.erase(kG + 0x31CC);
    UNSCOPED_INFO("order: unreadable before the model");
    REQUIRE(verdictOf({ true, true }, memory) == WindowWriterFallback::Unreadable);
    UNSCOPED_INFO("order: the stack before an unreadable word");
    REQUIRE(verdictAt({ true, true }, memory, kG - 0x1000, kG + 0x40) == WindowWriterFallback::StackOverlap);
    for (const std::uint32_t word : { 0x30U, 0x38U, 0x2ABCU, 0x3098U, 0x31A4U, 0x31CCU })
    {
        auto holes = built(everything);
        holes.erase(kG + word);
        UNSCOPED_INFO(std::format("order: an unreadable G+{:#x} cannot mask the stack", word));
        REQUIRE(verdictAt({ true, true }, holes, kG - 0x1000, kG + 0x40) == WindowWriterFallback::StackOverlap);
    }
    {
        auto high = entryOf();
        high.g    = 0xFFFFFFF0U;
        UNSCOPED_INFO("order: a device that wraps is unreadable before the stack");
        REQUIRE(WindowWriterConsumeVerdict({ true, true }, readerOf(memory), high) == WindowWriterFallback::Unreadable);
    }
    memory = built(everything);
    UNSCOPED_INFO("order: the stack before the ring check and the diagnostics");
    REQUIRE(verdictAt({ true, true }, memory, kG - 0x1000, kG + 0x40) == WindowWriterFallback::StackOverlap);
    UNSCOPED_INFO("order: stubs before the stack");
    REQUIRE(verdictAt({ false, true }, memory, kG - 0x1000, kG + 0x40) == WindowWriterFallback::Stubs);
    UNSCOPED_INFO("order: the entry check before the tile count");
    REQUIRE(verdictOf({ true, true }, memory) == WindowWriterFallback::RingSpace);
    everything.cursor = everything.limit38;
    UNSCOPED_INFO("order: tile count before diagnostics");
    REQUIRE(verdictOf({ true, true }, built(everything)) == WindowWriterFallback::TileCount);
    UNSCOPED_INFO("order: diagnostics last");
    REQUIRE(verdictOf({ true, true }, built(Device{})) == WindowWriterFallback::Diagnostics);
    UNSCOPED_INFO("fallback names");
    REQUIRE((kWindowWriterFallbackNames.size() == kWindowWriterFallbackCount && kWindowWriterFallbackCount == 6));
    UNSCOPED_INFO("the fallback names follow the verdict order");
    REQUIRE((std::string(kWindowWriterFallbackNames[0]) == "stubs" && std::string(kWindowWriterFallbackNames[1]) == "stack_overlap" && std::string(kWindowWriterFallbackNames[2]) == "unreadable"));
}

#if REREVVED_DEVELOPER
void testCallers()
{
    UNSCOPED_INFO("8268F9C0");
    REQUIRE(WindowWriterCallerOf(kLrF9c0) == WindowWriterCaller::Setter8268F9C0);
    UNSCOPED_INFO("82690040");
    REQUIRE(WindowWriterCallerOf(kLr0040) == WindowWriterCaller::Setter82690040);
    UNSCOPED_INFO("826922D0");
    REQUIRE(WindowWriterCallerOf(kLr22d0) == WindowWriterCaller::Setter826922D0);
    UNSCOPED_INFO("82698720");
    REQUIRE(WindowWriterCallerOf(kLr8720) == WindowWriterCaller::Setter82698720);
    UNSCOPED_INFO("clear");
    REQUIRE(WindowWriterCallerOf(kLrClear) == WindowWriterCaller::ClearBody);
    UNSCOPED_INFO("both resolve sites");
    REQUIRE((WindowWriterCallerOf(kLrResolveA) == WindowWriterCaller::Resolve && WindowWriterCallerOf(kLrResolveB) == WindowWriterCaller::Resolve));
    UNSCOPED_INFO("any other return address");
    REQUIRE((WindowWriterCallerOf(0x8268FAA4) == WindowWriterCaller::Other && WindowWriterCallerOf(0) == WindowWriterCaller::Other));
    UNSCOPED_INFO("caller names");
    REQUIRE(kWindowWriterCallerNames.size() == kWindowWriterCallerCount);
}

// What a consumed call may change relative to Original's run on the same words: only
// the ring words and G+0x30.
#endif
void checkOnlyRingDiffers(const Memory& entryWords, const Emulated& emu, const std::string& text)
{
    for (const auto& [address, value] : emu.memory)
    {
        const auto found = entryWords.find(address);
        if (found == entryWords.end())
            continue; // a ring word
        if (found->second != value)
        {
            UNSCOPED_INFO(std::format("Original stored a device word at {:#x}: ", address) + text);
            REQUIRE(address == kG + 0x30);
        }
    }
}

void testConsumed()
{
    const Cvars cvars(true, false);
    const Stubs stubs(true);
#if REREVVED_DEVELOPER
    (void)TakeWindowWriterConsumeCounts();
#endif

    std::vector<std::pair<std::string, Device>> scenes;
    scenes.emplace_back("plain", Device{});
    for (const auto count : { 0U, 1U, 2U, 4U, 16U })
    {
        scenes.emplace_back(std::format("tiled {}", count), tiledDevice(count));
        scenes.emplace_back(std::format("tiled {} with the extra block", count), tiledDevice(count, true));
    }
    {
        Device bound;
        bound.tiling    = 0x20;
        bound.tileCount = 2;
        bound.tiles     = { Tile{}, Tile{} };
        scenes.emplace_back("bound form", bound);
    }
    [[maybe_unused]] std::uint64_t consumedWords = 0;
    [[maybe_unused]] int           consumed      = 0;
    for (const auto& [name, device] : scenes)
        for (const auto& entry : { entryOf(), entryOf(40, 30, 700, 500), entryOf(-5, -5, 3, 3) })
        {
            const auto text = std::format("{} window {},{}", name, static_cast<std::int32_t>(entry.x0), static_cast<std::int32_t>(entry.y1));
            FakeMemory memory(built(device));
            const auto entryWords = memory.words;
            const auto emu        = emulateOriginal(entryWords, entry);
            UNSCOPED_INFO("the scene does not flush: " + text);
            REQUIRE(!emu.flushed);
            auto ctx = contextOf(entry);
            UNSCOPED_INFO("consumed: " + text);
            REQUIRE(ConsumeWindowWriter(ctx, &memory, false));
            UNSCOPED_INFO(std::format("r3 {:#x} vs Original {:#x}: ", ctx.r3.u64, emu.r3) + text);
            REQUIRE(ctx.r3.u64 == emu.r3);
            UNSCOPED_INFO("a consumed call stores nothing: " + text);
            REQUIRE((memory.log.empty() && memory.words == entryWords));
            UNSCOPED_INFO("the other registers stay: " + text);
            REQUIRE((ctx.r4.u64 == entry.x0 && ctx.r5.u64 == entry.y0 && ctx.r6.u64 == entry.x1 && ctx.r7.u64 == entry.y1 && ctx.r1.u64 == kStack && ctx.lr == kLrF9c0 && ctx.r11.u64 == 0x1111 && ctx.r12.u64 == 0x2222));
            checkOnlyRingDiffers(entryWords, emu, text);
            consumedWords += emu.ring.size();
            ++consumed;
        }
#if REREVVED_DEVELOPER
    const auto counts = TakeWindowWriterConsumeCounts();
    UNSCOPED_INFO("consumed count");
    REQUIRE(counts.consumed == static_cast<std::uint64_t>(consumed));
    UNSCOPED_INFO("all from the one caller");
    REQUIRE(counts.callers[static_cast<std::size_t>(WindowWriterCaller::Setter8268F9C0)] == counts.consumed);
    UNSCOPED_INFO("the skipped ring words are the words Original stored");
    REQUIRE(counts.ringWordsConsumed == consumedWords);
    for (const auto reason : counts.fallback)
    {
        UNSCOPED_INFO("no fallbacks");
        REQUIRE(reason == 0);
    }
#endif
}

void testFallbacks()
{
    const Cvars cvars(true, false);
#if REREVVED_DEVELOPER
    (void)TakeWindowWriterConsumeCounts();
#endif
    const auto entry = entryOf();

    // Each fallback leaves the registers and the memory alone and is counted once.
    const auto expectFallback = [&](bool stubsReady, const Memory& words, bool diagnostics, [[maybe_unused]] WindowWriterFallback reason, const std::string& text)
    {
        const Stubs stubs(stubsReady);
        FakeMemory  memory(words);
        auto        ctx = contextOf(entry);
        UNSCOPED_INFO("falls back: " + text);
        REQUIRE(!ConsumeWindowWriter(ctx, &memory, diagnostics));
        UNSCOPED_INFO("a fallback changes nothing: " + text);
        REQUIRE((ctx.r3.u64 == kG && memory.words == words && memory.log.empty()));
#if REREVVED_DEVELOPER
        const auto counts = TakeWindowWriterConsumeCounts();
        UNSCOPED_INFO("nothing consumed: " + text);
        REQUIRE((counts.consumed == 0 && counts.ringWordsConsumed == 0));
        for (std::size_t i = 0; i < kWindowWriterFallbackCount; ++i)
        {
            UNSCOPED_INFO(std::format("counted as {}: ", kWindowWriterFallbackNames[static_cast<std::size_t>(reason)]) + text);
            REQUIRE(counts.fallback[i] == (i == static_cast<std::size_t>(reason) ? 1U : 0U));
        }
#endif
    };
    expectFallback(false, built(Device{}), false, WindowWriterFallback::Stubs, "stubs");
    expectFallback(true, built(Device{}), true, WindowWriterFallback::Diagnostics, "diagnostics");
    auto unreadable = built(Device{});
    unreadable.erase(kG + 0x38);
    expectFallback(true, unreadable, false, WindowWriterFallback::Unreadable, "unreadable");
    Device late;
    late.cursor = late.limit38 + 4;
    expectFallback(true, built(late), false, WindowWriterFallback::RingSpace, "ring space");
    expectFallback(true, built(tiledDevice(17)), false, WindowWriterFallback::TileCount, "tile count");
    // Stubs ready for another device are not ready for this one.
    {
        const Stubs stubs(true);
        ringStubsValidatedDevice.store(kG + 0x1000, std::memory_order_release);
        FakeMemory memory(built(Device{}));
        auto       ctx = contextOf(entry);
        UNSCOPED_INFO("another device's stubs do not admit this one");
        REQUIRE(!ConsumeWindowWriter(ctx, &memory, false));
#if REREVVED_DEVELOPER
        UNSCOPED_INFO("counted as stubs");
        REQUIRE(TakeWindowWriterConsumeCounts().fallback[static_cast<std::size_t>(WindowWriterFallback::Stubs)] == 1);
#endif
    }
    // No memory view: not even the thread's stack bounds can be read, and unknown bounds are refused first.
    {
        const Stubs stubs(true);
        auto        ctx = contextOf(entry);
        UNSCOPED_INFO("no memory");
        REQUIRE(!ConsumeWindowWriter(ctx, nullptr, false));
#if REREVVED_DEVELOPER
        UNSCOPED_INFO("counted as stack_overlap");
        REQUIRE(TakeWindowWriterConsumeCounts().fallback[static_cast<std::size_t>(WindowWriterFallback::StackOverlap)] == 1);
#endif
    }
#if REREVVED_DEVELOPER
    // The probe's cvar forces the fallback even for a call that would be consumed.
    {
        const Cvars both(true, true);
        const Stubs stubs(true);
        FakeMemory  memory(built(Device{}));
        auto        ctx = contextOf(entry);
        UNSCOPED_INFO("the exit check forces the body");
        REQUIRE(!ConsumeWindowWriter(ctx, &memory, false));
        UNSCOPED_INFO("counted as diagnostics");
        REQUIRE(TakeWindowWriterConsumeCounts().fallback[static_cast<std::size_t>(WindowWriterFallback::Diagnostics)] == 1);
    }
#endif
}

void testCvarOff()
{
    const Cvars cvars(false, false);
    const Stubs stubs(true);
#if REREVVED_DEVELOPER
    (void)TakeWindowWriterConsumeCounts();
#endif
    FakeMemory memory(built(Device{}));
    auto       ctx = contextOf(entryOf());
    UNSCOPED_INFO("the cvar is off");
    REQUIRE(!ConsumeWindowWriter(ctx, &memory, false));
    UNSCOPED_INFO("an off consume reads nothing");
    REQUIRE((ctx.r3.u64 == kG && memory.reads == 0));
#if REREVVED_DEVELOPER
    const auto counts = TakeWindowWriterConsumeCounts();
    UNSCOPED_INFO("an off consume counts nothing");
    REQUIRE(counts.consumed == 0);
    for (const auto reason : counts.fallback)
    {
        UNSCOPED_INFO("an off consume counts no fallback");
        REQUIRE(reason == 0);
    }
#endif
    UNSCOPED_INFO("an off consume is not requested");
    REQUIRE(!WindowWriterConsumeRequested());
}

// Consume defaults on, exit check off; publication gates consume requests.
void testDefaults()
{
    UNSCOPED_INFO("the consume defaults on and the exit check off");
    REQUIRE((REXCVAR_GET(native_consume_window_writer) && DEV_CHECK(!REXCVAR_GET(native_window_writer_exit_check))));
    {
        const Stubs unpublished(true, false);
        UNSCOPED_INFO("unpublished ring stubs leave the consume unrequested");
        REQUIRE(!WindowWriterConsumeRequested());
    }
    const Stubs published(false);
    UNSCOPED_INFO("published ring stubs request the consume");
    REQUIRE(WindowWriterConsumeRequested());
}

#if REREVVED_DEVELOPER
void testCallerCounts()
{
    const Cvars cvars(true, false);
    const Stubs stubs(true);
    (void)TakeWindowWriterConsumeCounts();
    FakeMemory memory(built(Device{}));
    for (const auto lr : { kLrF9c0, kLrF9c0, kLr0040, kLr22d0, kLr8720, kLrClear, kLrResolveA, kLrResolveB, 0x82345678U })
    {
        auto ctx = contextOf(entryOf(), lr);
        UNSCOPED_INFO("consumed");
        REQUIRE(ConsumeWindowWriter(ctx, &memory, false));
    }
    const auto counts = TakeWindowWriterConsumeCounts();
    const auto at     = [&](WindowWriterCaller caller)
    {
        return counts.callers[static_cast<std::size_t>(caller)];
    };
    UNSCOPED_INFO("the consumed calls are split by caller");
    REQUIRE((counts.consumed == 9 && at(WindowWriterCaller::Setter8268F9C0) == 2 && at(WindowWriterCaller::Setter82690040) == 1 && at(WindowWriterCaller::Setter826922D0) == 1 && at(WindowWriterCaller::Setter82698720) == 1 &&
             at(WindowWriterCaller::ClearBody) == 1 && at(WindowWriterCaller::Resolve) == 2 && at(WindowWriterCaller::Other) == 1));
    UNSCOPED_INFO("four words each");
    REQUIRE(counts.ringWordsConsumed == 36);
    const auto line = FormatWindowWriterConsumeCounts(counts);
    UNSCOPED_INFO("consume line: " + line);
    REQUIRE(line == " window_writer_consumed=9 window_writer_consumed_8268f9c0=2 window_writer_consumed_82690040=1 window_writer_consumed_826922d0=1 window_writer_consumed_82698720=1 window_writer_consumed_clear=1 "
                    "window_writer_consumed_resolve=2 window_writer_consumed_other=1 window_writer_ring_words_consumed=36 window_writer_ring_words_original=0 window_writer_original_calls=0");
}

void testFormat()
{
    WindowWriterConsumeCounts counts;
    UNSCOPED_INFO("empty line");
    REQUIRE(FormatWindowWriterConsumeCounts(counts) == " window_writer_consumed=0 window_writer_ring_words_consumed=0 window_writer_ring_words_original=0 window_writer_original_calls=0");
    counts.fallback[static_cast<std::size_t>(WindowWriterFallback::RingSpace)]   = 7;
    counts.fallback[static_cast<std::size_t>(WindowWriterFallback::TileCount)]   = 2;
    counts.fallback[static_cast<std::size_t>(WindowWriterFallback::Diagnostics)] = 1;
    counts.originalUnbounded                                                     = 3;
    counts.ringWordsOriginal                                                     = 30;
    counts.original                                                              = 5;
    const auto line                                                              = FormatWindowWriterConsumeCounts(counts);
    UNSCOPED_INFO("fallback line: " + line);
    REQUIRE(line == " window_writer_consumed=0 window_writer_ring_words_consumed=0 window_writer_ring_words_original=30 window_writer_original_calls=5 window_writer_original_unbounded=3 "
                    "window_writer_fallback_tile_count=2 window_writer_fallback_ring_space=7 window_writer_fallback_diagnostics=1");
}

// The hook of window_writer_hooks.cpp: the consume first, then the probe around Original.
void runHook(PPCContext& ctx, FakeMemory* memory, bool diagnostics, const std::function<void(PPCContext&)>& original)
{
    if (WindowWriterConsumeRequested() && ConsumeWindowWriter(ctx, memory, diagnostics))
        return;
    const WindowWriterExitProbe probe(ctx, memory);
    original(ctx);
}

// Original's run on the fake memory, optionally deviating.
struct Deviation
{
    std::uint64_t resultAdd = 0;
    std::int32_t  cursorAdd = 0, limit34Add = 0, limit38Add = 0;
    bool          changeState = false, changeRing = false, dropRing = false;
};

void runOriginal(PPCContext& ctx, FakeMemory& memory, const WindowWriterEntry& entry, const Deviation& deviation = {})
{
    const auto emu = emulateOriginal(memory.words, entry);
    UNSCOPED_INFO("the probe scene does not flush");
    REQUIRE(!emu.flushed);
    memory.words = emu.memory;
    ctx.r3.u64   = emu.r3 + deviation.resultAdd;
    memory.words[kG + 0x30] += static_cast<std::uint32_t>(deviation.cursorAdd);
    memory.words[kG + 0x34] += static_cast<std::uint32_t>(deviation.limit34Add);
    memory.words[kG + 0x38] += static_cast<std::uint32_t>(deviation.limit38Add);
    if (deviation.changeState)
        memory.words[kG + 0x31A4] ^= 1;
    if (deviation.changeRing)
        memory.words[built(Device{}).at(kG + 0x30) + 8] ^= 0x100;
    if (deviation.dropRing)
        memory.words.erase(built(Device{}).at(kG + 0x30) + 16);
}

void testProbe()
{
    const Cvars cvars(false, true);
    const auto  entry = entryOf(10, 20, 900, 600);
    (void)TakeWindowWriterExitCounts();

    const auto probed = [&](const Device& device, const Deviation& deviation, FakeMemory** keep = nullptr)
    {
        FakeMemory memory(built(device));
        auto       ctx = contextOf(entry);
        runHook(ctx, &memory, false, [&](PPCContext& c)
                {
                    runOriginal(c, memory, entry, deviation);
                });
        (void)keep;
        return TakeWindowWriterExitCounts();
    };
    const auto mismatchesOnly = [](const WindowWriterExitCounts& counts, std::size_t word)
    {
        for (std::size_t i = 0; i < kWindowWriterExitWordCount; ++i)
            if (counts.mismatch[i] != (i == word ? 1U : 0U))
                return false;
        return counts.ok == 0 && counts.calls == 1;
    };

    // Agreeing Originals, plain and tiled.
    {
        const auto counts = probed(Device{}, {});
        UNSCOPED_INFO("a plain Original agrees");
        REQUIRE((counts.ok == 1 && counts.calls == 1 && counts.tiled == 0 && counts.ringWordsMax == 4 && counts.ringWordsSum == 4));
        for (const auto mismatch : counts.mismatch)
        {
            UNSCOPED_INFO("no mismatch");
            REQUIRE(mismatch == 0);
        }
    }
    {
        const auto counts = probed(tiledDevice(3, true), {});
        UNSCOPED_INFO("a tiled Original agrees");
        REQUIRE((counts.ok == 1 && counts.tiled == 1 && counts.tiledOk == 1 && counts.ringWordsMax == 2 + 21 + 6 + 4 && counts.ringWordsSum == 33));
    }
    // Each compared word.
    {
        Deviation deviation;
        deviation.resultAdd = 1;
        UNSCOPED_INFO("result");
        REQUIRE(mismatchesOnly(probed(tiledDevice(2), deviation), 0));
        deviation           = {};
        deviation.resultAdd = 1ULL << 32;
        UNSCOPED_INFO("a stray high half of r3 is a result mismatch");
        REQUIRE(mismatchesOnly(probed(Device{}, deviation), 0));
        deviation           = {};
        deviation.cursorAdd = 4;
        UNSCOPED_INFO("cursor");
        REQUIRE(mismatchesOnly(probed(Device{}, deviation), 1));
        deviation            = {};
        deviation.limit34Add = 4;
        UNSCOPED_INFO("limit 34");
        REQUIRE(mismatchesOnly(probed(Device{}, deviation), 2));
        deviation            = {};
        deviation.limit38Add = 4;
        UNSCOPED_INFO("limit 38");
        REQUIRE(mismatchesOnly(probed(Device{}, deviation), 3));
        deviation             = {};
        deviation.changeState = true;
        UNSCOPED_INFO("state");
        REQUIRE(mismatchesOnly(probed(tiledDevice(2), deviation), 4));
        deviation            = {};
        deviation.changeRing = true;
        UNSCOPED_INFO("ring content");
        REQUIRE(mismatchesOnly(probed(Device{}, deviation), 5));
        deviation          = {};
        deviation.dropRing = true;
        UNSCOPED_INFO("a ring word that cannot be read after the call is unreadable");
        REQUIRE(probed(Device{}, deviation).unreadable == 1);
    }
    // An observed cursor that moved by more than the model gives the observed words.
    {
        Deviation deviation;
        deviation.cursorAdd = 8;
        const auto counts   = probed(Device{}, deviation);
        UNSCOPED_INFO("ring words are the observed cursor delta");
        REQUIRE((counts.ringWordsSum == 6 && counts.ringWordsMax == 6));
    }
    // Entries the model refuses are counted by reason and not compared.
    {
        Device late;
        late.cursor = late.limit38 + 4;
        FakeMemory memory(built(late));
        auto       ctx = contextOf(entry);
        bool       ran = false;
        runHook(ctx, &memory, false, [&](PPCContext&)
                {
                    ran = true;
                });
        const auto counts = TakeWindowWriterExitCounts();
        UNSCOPED_INFO("a flushing entry is unmodeled");
        REQUIRE((ran && counts.calls == 0 && counts.unmodeled[static_cast<std::size_t>(WindowWriterRefusal::RingSpace)] == 1));
        FakeMemory many(built(tiledDevice(17)));
        runHook(ctx, &many, false, [&](PPCContext&)
                {
                });
        UNSCOPED_INFO("too many tiles are unmodeled");
        REQUIRE(TakeWindowWriterExitCounts().unmodeled[static_cast<std::size_t>(WindowWriterRefusal::TileCount)] == 1);
    }
    // Unreadable entries and exits.
    {
        FakeMemory memory(built(Device{}));
        memory.refused.insert(kG + 0x38);
        auto ctx = contextOf(entry);
        runHook(ctx, &memory, false, [&](PPCContext&)
                {
                });
        UNSCOPED_INFO("an unreadable entry");
        REQUIRE(TakeWindowWriterExitCounts().unreadable == 1);
        FakeMemory later(built(Device{}));
        auto       laterCtx = contextOf(entry);
        runHook(laterCtx, &later, false, [&](PPCContext& c)
                {
                    runOriginal(c, later, entry);
                    later.refused.insert(kG + 0x30);
                });
        const auto counts = TakeWindowWriterExitCounts();
        UNSCOPED_INFO("an unreadable exit");
        REQUIRE((counts.unreadable == 1 && counts.calls == 0));
        auto nullCtx = contextOf(entry);
        runHook(nullCtx, nullptr, false, [&](PPCContext&)
                {
                });
        const auto none = TakeWindowWriterExitCounts();
        UNSCOPED_INFO("no memory view: nothing to compare");
        REQUIRE((none.calls == 0 && none.unreadable == 0));
    }
    // A call that unwinds is not compared.
    {
        FakeMemory memory(built(Device{}));
        auto       ctx = contextOf(entry);
        try
        {
            runHook(ctx, &memory, false, [&](PPCContext&)
                    {
                        throw std::runtime_error("guest unwound");
                    });
        }
        catch (const std::runtime_error&)
        {
        }
        const auto unwound = TakeWindowWriterExitCounts();
        UNSCOPED_INFO("an unwound call has no exit to compare or count");
        REQUIRE((unwound.calls == 0 && unwound.unreadable == 0 && unwound.ok == 0));
    }
    // The probe never changes what runs: Original runs once and r3 is its own.
    {
        FakeMemory memory(built(Device{}));
        auto       ctx  = contextOf(entry);
        int        runs = 0;
        runHook(ctx, &memory, false, [&](PPCContext& c)
                {
                    ++runs;
                    runOriginal(c, memory, entry);
                });
        UNSCOPED_INFO("Original runs once under the probe");
        REQUIRE((runs == 1 && ctx.r3.u64 == Device{}.cursor + 12));
        (void)TakeWindowWriterExitCounts();
    }
    // A diagnostic does not need the probe's cvar to run Original with the consume on.
    {
        const Cvars both(true, true);
        const Stubs stubs(true);
        FakeMemory  memory(built(Device{}));
        auto        ctx  = contextOf(entry);
        int         runs = 0;
        runHook(ctx, &memory, false, [&](PPCContext& c)
                {
                    ++runs;
                    runOriginal(c, memory, entry);
                });
        UNSCOPED_INFO("with both cvars on Original runs and is compared");
        REQUIRE((runs == 1 && TakeWindowWriterExitCounts().ok == 1));
        (void)TakeWindowWriterConsumeCounts();
    }
}

#endif
// Window writer refuses unknown/overlap [limit,r1) against [G,G+0x3340) or ring.
void testStackOverlap()
{
    // stack_guard.h refuses unknown bounds and device overlap; probe also checks ring.
    constexpr std::uint32_t kSpan = kWindowWriterDeviceSpan, kBlockEnd = kG + kSpan;
    UNSCOPED_INFO("the footprint: nothing above r1, the device up to G+0x3340");
    REQUIRE((kWindowWriterStackFootprint.above == 0 && kWindowWriterStackFootprint.deviceSpan == 0x3340 && kSpan == 0x3340));
    const auto plain   = built(Device{});
    const auto ready   = readyState();
    const auto refused = [&](std::uint32_t limit, std::uint32_t sp)
    {
        return verdictAt(ready, plain, limit, sp) == WindowWriterFallback::StackOverlap;
    };
    const auto consumed = [&](std::uint32_t limit, std::uint32_t sp)
    {
        return !verdictAt(ready, plain, limit, sp);
    };

    // The device words [G, G + 0x3340) against the half-open reach [limit, sp), at the pinned edges.
    UNSCOPED_INFO("a reach ending exactly at G is consumed");
    REQUIRE(consumed(kG - 0x1000, kG));
    UNSCOPED_INFO("a reach ending one byte into the device is refused");
    REQUIRE(refused(kG - 0x1000, kG + 1));
    UNSCOPED_INFO("a reach ending four bytes into the device is refused");
    REQUIRE(refused(kG - 0x1000, kG + 4));
    UNSCOPED_INFO("a reach starting exactly at G + 0x3340 is consumed");
    REQUIRE(consumed(kBlockEnd, kBlockEnd + 0x1000));
    UNSCOPED_INFO("a reach starting on the last device byte is refused");
    REQUIRE(refused(kBlockEnd - 1, kBlockEnd + 0x1000));
    UNSCOPED_INFO("the words between G+0x3340 and the old 0x3500 block are not in the footprint");
    REQUIRE(consumed(kG + 0x3400, kG + 0x3500));
    UNSCOPED_INFO("reaches over or inside the device are refused");
    REQUIRE((refused(kG - 0x1000, kBlockEnd + 0x1000) && refused(kG + 0x1000, kG + 0x3400)));
    UNSCOPED_INFO("reaches away from the device are consumed");
    REQUIRE((consumed(kG - 0x4000, kG - 0x1000) && consumed(kStack - 0x1000, kStack)));
    // The no-overflow domain: the bytes between the limit and r1 must hold the entry function's own frame (stwu
    // r1,-176(r1), g153:12642). Fewer is refused, wherever the stack lies; r1 at the limit is the case the callee
    // saves would store below the limit in.
    static_assert(kWindowWriterStackFootprint.frame == 176);
    UNSCOPED_INFO("exactly the frame between the limit and r1 is consumed");
    REQUIRE(consumed(kStack - 176, kStack));
    UNSCOPED_INFO("one byte short of the frame is refused");
    REQUIRE(refused(kStack - 175, kStack));
    UNSCOPED_INFO("r1 at the limit is refused");
    REQUIRE((refused(kStack, kStack) && refused(kG, kG)));

    // A device that cannot be placed.
    {
        StackReach reach;
        const auto read = readerOf(plain);
        UNSCOPED_INFO("a device ending exactly at 2^32 is placed");
        REQUIRE(!WindowWriterStackOverlaps(read, { kPcr, kStack }, 0xFFFFFFFFU - kSpan + 1, reach));
        UNSCOPED_INFO("a device running past 2^32 is refused");
        REQUIRE(WindowWriterStackOverlaps(read, { kPcr, kStack }, 0xFFFFFFFFU - kSpan + 2, reach));
    }

    // Unknown bounds are refused like an overlap.
    {
        const auto unknown = [&](const std::function<void(Memory&, WindowWriterEntry&)>& edit)
        {
            auto memory = plain;
            auto entry  = entryOf();
            edit(memory, entry);
            return WindowWriterConsumeVerdict(ready, readerOf(memory), entry);
        };
        const std::optional<WindowWriterFallback> stackOverlap{ WindowWriterFallback::StackOverlap };
        UNSCOPED_INFO("r13 zero");
        REQUIRE(unknown([](Memory&, WindowWriterEntry& e)
                        {
                            e.pcr = 0;
                        }) == stackOverlap);
        UNSCOPED_INFO("an unreadable limit");
        REQUIRE(unknown([](Memory& m, WindowWriterEntry&)
                        {
                            m.erase(kPcr + 0x74);
                        }) == stackOverlap);
        UNSCOPED_INFO("an unreadable base");
        REQUIRE(unknown([](Memory& m, WindowWriterEntry&)
                        {
                            m.erase(kPcr + 0x70);
                        }) == stackOverlap);
        UNSCOPED_INFO("a zero limit");
        REQUIRE(unknown([](Memory& m, WindowWriterEntry&)
                        {
                            placeStack(m, 0, kStack + 0x1000);
                        }) == stackOverlap);
        UNSCOPED_INFO("an inverted range");
        REQUIRE(unknown([](Memory& m, WindowWriterEntry&)
                        {
                            placeStack(m, kStack + 0x1000, kStack - 0x1000);
                        }) == stackOverlap);
        UNSCOPED_INFO("r1 below the limit");
        REQUIRE(unknown([](Memory&, WindowWriterEntry& e)
                        {
                            e.sp = kStack - 0x1004;
                        }) == stackOverlap);
        UNSCOPED_INFO("r1 above the base");
        REQUIRE(unknown([](Memory&, WindowWriterEntry& e)
                        {
                            e.sp = kStack + 0x1004;
                        }) == stackOverlap);
        UNSCOPED_INFO("a KPCR in the stack");
        REQUIRE(unknown([](Memory& m, WindowWriterEntry& e)
                        {
                            e.pcr                   = kStack - 0x40;
                            m[kStack - 0x40 + 0x70] = kStack + 0x1000;
                            m[kStack - 0x40 + 0x74] = kStack - 0x1000;
                        }) == stackOverlap);
        UNSCOPED_INFO("the fixture's stack is known");
        REQUIRE(!unknown([](Memory&, WindowWriterEntry&)
                         {
                         }));
    }

    // The regression: a stack that covers words the model reads. Original's prologue stores would change them, so the
    // model's inputs would not be what Original reads.
    for (const std::uint32_t word : { 0x30U, 0x38U, 0x2ABCU, 0x31CCU, 0x32C0U, 0x333CU })
        for (const std::uint32_t sp : { kG + word + 4, kG + word + 96, kG + word + 172 })
        {
            UNSCOPED_INFO(std::format("a stack over G+{:#x} is refused (sp {:#x})", word, sp));
            REQUIRE(refused(sp - 176, sp));
        }
    {
        const std::uint32_t sp    = kG + 0x30 + 12;
        const auto          after = afterPrologue(plain, sp);
        UNSCOPED_INFO("Original's saves change the cursor word the model reads");
        REQUIRE((after.at(kG + 0x30) == 0xDEADBEEF && after.at(kG + 0x30) != plain.at(kG + 0x30)));
        UNSCOPED_INFO("the stack over the cursor is refused");
        REQUIRE(refused(sp - 176, sp));
        const std::uint32_t spLimit = kG + 0x38 + 40;
        UNSCOPED_INFO("Original's saves change the limit word, and the call is refused");
        REQUIRE((afterPrologue(plain, spLimit).at(kG + 0x38) == 0xDEADBEEF && refused(spLimit - 176, spLimit)));
    }
    // Soundness over a sweep: a call whose prologue stores into the device is never consumed, and the reach
    // [sp - 176, sp) decides the rest.
    for (std::uint32_t sp = kG - 400; sp < kBlockEnd + 400; sp += 4)
    {
        bool stores = false;
        for (const auto& [address, bytes] : prologueStores(sp))
            stores |= address < kBlockEnd && address + bytes > kG;
        const bool overlaps = sp > kG && sp - 176 < kBlockEnd;
        if (stores)
        {
            UNSCOPED_INFO(std::format("a stored word in the device is refused (sp {:#x})", sp));
            REQUIRE(refused(sp - 176, sp));
        }
        UNSCOPED_INFO(std::format("the reach decides (sp {:#x})", sp));
        REQUIRE((overlaps ? refused(sp - 176, sp) : consumed(sp - 176, sp)));
    }

    // The consume counts it, falls back and changes nothing; a clear stack is consumed.
    {
        const Cvars cvars(true, false);
        const Stubs stubs(true);
#if REREVVED_DEVELOPER
        (void)TakeWindowWriterConsumeCounts();
#endif
        auto over = plain;
        placeStack(over, kG - 0x1000, kG + 0x4000);
        FakeMemory memory(over);
        auto       ctx = contextOf(entryOf());
        ctx.r1.u64     = kG + 0x34;
        UNSCOPED_INFO("a stack over the device falls back");
        REQUIRE(!ConsumeWindowWriter(ctx, &memory, false));
        UNSCOPED_INFO("a fallback changes nothing");
        REQUIRE((ctx.r3.u64 == kG && memory.words == over && memory.log.empty()));
#if REREVVED_DEVELOPER
        const auto counts = TakeWindowWriterConsumeCounts();
        UNSCOPED_INFO("counted as stack_overlap");
        REQUIRE((counts.consumed == 0 && counts.fallback[static_cast<std::size_t>(WindowWriterFallback::StackOverlap)] == 1));
        UNSCOPED_INFO("the counter name");
        REQUIRE(FormatWindowWriterConsumeCounts(counts).find(" window_writer_fallback_stack_overlap=1") != std::string::npos);
#endif
        ctx.r1.u64 = kG;
        UNSCOPED_INFO("a reach ending at G is consumed");
        REQUIRE(ConsumeWindowWriter(ctx, &memory, false));
#if REREVVED_DEVELOPER
        UNSCOPED_INFO("counted as consumed");
        REQUIRE(TakeWindowWriterConsumeCounts().consumed == 1);
#endif
    }
#if REREVVED_DEVELOPER
    // The probe: the unmodeled count; Original runs once and is not compared.
    {
        const Cvars cvars(false, true);
        (void)TakeWindowWriterExitCounts();
        auto over = plain;
        placeStack(over, kG - 0x1000, kG + 0x4000);
        FakeMemory memory(over);
        auto       ctx = contextOf(entryOf());
        ctx.r1.u64     = kG + 0x34;
        int runs       = 0;
        runHook(ctx, &memory, false, [&](PPCContext&)
                {
                    ++runs;
                });
        const auto counts = TakeWindowWriterExitCounts();
        UNSCOPED_INFO("the probe counts stack_overlap as unmodeled");
        REQUIRE((runs == 1 && counts.calls == 0 && counts.ok == 0 && counts.unmodeled[static_cast<std::size_t>(WindowWriterRefusal::StackOverlap)] == 1));
        UNSCOPED_INFO("the unmodeled counter name");
        REQUIRE(FormatWindowWriterExitCounts(counts).find(" window_writer_exit_unmodeled_stack_overlap=1") != std::string::npos);
        ctx.r1.u64 = kG;
        runHook(ctx, &memory, false, [&](PPCContext& c)
                {
                    runOriginal(c, memory, entryOf());
                });
        UNSCOPED_INFO("a clear reach is compared");
        REQUIRE(TakeWindowWriterExitCounts().ok == 1);
    }

    // The probe reads the ring words Original stores after the cursor, [cursor+4, cursor+20) for a plain call; a
    // reach over them is not modeled, while the consume never reads them. The cursor is kRing + 0x100.
    {
        const Cvars  cvars(false, true);
        const Device device;
        const auto   ringStart = device.cursor + 4, ringEnd = device.cursor + 4 + 4 * 4;
        const auto   probed = [&](std::uint32_t limit, std::uint32_t sp)
        {
            (void)TakeWindowWriterExitCounts();
            auto words = built(device);
            placeStack(words, limit, sp + 0x1000);
            FakeMemory memory(words);
            auto       ctx = contextOf(entryOf());
            ctx.r1.u64     = sp;
            runHook(ctx, &memory, false, [&](PPCContext& c)
                    {
                        runOriginal(c, memory, entryOf());
                    });
            const auto counts    = TakeWindowWriterExitCounts();
            const bool unmodeled = counts.unmodeled[static_cast<std::size_t>(WindowWriterRefusal::StackOverlap)] == 1;
            UNSCOPED_INFO("a probe is either compared or unmodeled");
            REQUIRE(unmodeled != (counts.ok == 1));
            return unmodeled;
        };
        UNSCOPED_INFO("a reach ending at the first ring word is compared");
        REQUIRE(!probed(ringStart - 0x1000, ringStart));
        UNSCOPED_INFO("a reach one byte into the ring words is not modeled");
        REQUIRE(probed(ringStart - 0x1000, ringStart + 1));
        UNSCOPED_INFO("a reach starting after the ring words is compared");
        REQUIRE(!probed(ringEnd, ringEnd + 0x200));
        UNSCOPED_INFO("a reach starting on the last ring byte is not modeled");
        REQUIRE(probed(ringEnd - 1, ringEnd + 0x200));
    }
#endif
    // The consume does not read the ring words.
    {
        const Cvars  cvarsConsume(true, false);
        const Stubs  stubs(true);
        const Device device;
        const auto   ringStart = device.cursor + 4;
        auto         words     = built(device);
        placeStack(words, ringStart, ringStart + 0x1000);
        FakeMemory memory(words);
        auto       ctx = contextOf(entryOf());
        ctx.r1.u64     = ringStart + 0x200;
        UNSCOPED_INFO("the consume does not test the ring words");
        REQUIRE(ConsumeWindowWriter(ctx, &memory, false));
    }
}

// Unpublished consume reads/counts nothing; only the separate exit check reads.
void testUnpublished()
{
    const Cvars cvars(true, false);
    const Stubs stubs(true, false);
#if REREVVED_DEVELOPER
    (void)TakeWindowWriterConsumeCounts();
    (void)TakeWindowWriterExitCounts();
#endif
    FakeMemory memory(built(Device{}));
    auto       ctx = contextOf(entryOf());
#if REREVVED_DEVELOPER
    int runs = 0;
    runHook(ctx, &memory, false, [&](PPCContext&)
            {
                ++runs;
            });
    UNSCOPED_INFO("unpublished: Original runs once");
    REQUIRE((runs == 1 && ctx.r3.u64 == kG));
    UNSCOPED_INFO("unpublished: no guest word is read or stored");
    REQUIRE((memory.reads == 0 && memory.log.empty()));
#endif
    UNSCOPED_INFO("unpublished: the consume reads nothing");
    REQUIRE((!ConsumeWindowWriter(ctx, &memory, false) && memory.reads == 0));
#if REREVVED_DEVELOPER
    {
        const WindowWriterExitProbe probe(ctx, &memory);
        UNSCOPED_INFO("unpublished: the probe is not armed and reads nothing");
        REQUIRE((!probe.Armed() && memory.reads == 0));
    }
    const auto counts = TakeWindowWriterConsumeCounts();
    UNSCOPED_INFO("unpublished: nothing counted");
    REQUIRE((counts.consumed == 0 && counts.original == 0 && counts.originalUnbounded == 0 && counts.ringWordsOriginal == 0));
    for (const auto reason : counts.fallback)
    {
        UNSCOPED_INFO("unpublished: no fallback counted");
        REQUIRE(reason == 0);
    }

    // The exit check keeps its behavior without the consume's services.
    const Cvars check(true, true);
    runHook(ctx, &memory, false, [&](PPCContext& c)
            {
                runOriginal(c, memory, entryOf());
            });
    UNSCOPED_INFO("unpublished: the exit check still compares Original");
    REQUIRE((memory.reads > 0 && TakeWindowWriterExitCounts().ok == 1));
#endif
}

#if REREVVED_DEVELOPER
// Withdrawal/disable prevents reads/counts; published services expose Original.
void testObserverStopsWithTheStubs()
{
    const Cvars cvars(true, false);
    const Stubs stubs(false); // published, device not yet checked: the consume falls back
    (void)TakeWindowWriterConsumeCounts();
    const auto entry = entryOf();
    const auto run   = [&](bool disable)
    {
        FakeMemory memory(built(Device{}));
        auto       ctx             = contextOf(entry);
        int        readsBeforeExit = 0;
        {
            const WindowWriterExitProbe probe(ctx, &memory);
            UNSCOPED_INFO("the observer is armed while the stubs are published");
            REQUIRE(probe.Armed());
            runOriginal(ctx, memory, entry);
            if (disable)
                ringStubsActive.store(false, std::memory_order_release);
            readsBeforeExit = memory.reads;
        }
        ringStubsActive.store(true, std::memory_order_release);
        return memory.reads - readsBeforeExit;
    };
    UNSCOPED_INFO("published stubs: the exit reads the cursor");
    REQUIRE(run(false) > 0);
    auto counts = TakeWindowWriterConsumeCounts();
    UNSCOPED_INFO("published stubs: the Original is counted");
    REQUIRE((counts.original == 1 && counts.ringWordsOriginal > 0));
    UNSCOPED_INFO("disabled stubs: the exit reads nothing");
    REQUIRE(run(true) == 0);
    counts = TakeWindowWriterConsumeCounts();
    UNSCOPED_INFO("disabled stubs: nothing is counted");
    REQUIRE((counts.original == 0 && counts.originalUnbounded == 0 && counts.ringWordsOriginal == 0));
}

#endif
struct NoServices final : IRingStubServices
{
    void Kick(std::uint32_t) override
    {
    }
};

// Withdrawal waits for active consume; calls after flag clears fall back.
void testLeaseDrain(bool viaDisable)
{
    const Cvars cvars(true, false);
    // A fresh publication also clears an earlier disable.
    PublishRingStubServices(std::make_shared<NoServices>());
    const Stubs stubs(true);
#if REREVVED_DEVELOPER
    (void)TakeWindowWriterConsumeCounts();
#endif
    FakeMemory        memory(built(Device{}));
    auto              ctx = contextOf(entryOf());
    std::atomic<bool> inside{ false }, release{ false }, consumed{ false }, withdrawn{ false };
    memory.gate = [&]
    {
        inside.store(true);
        while (!release.load())
            std::this_thread::yield();
    };
    std::thread consumer([&]
                         {
                             consumed.store(ConsumeWindowWriter(ctx, &memory, false));
                         });
    while (!inside.load())
        std::this_thread::yield();
    std::thread withdrawal([&]
                           {
                               if (viaDisable)
                                   DisableRingStubs("test: layout mismatch");
                               else
                                   WithdrawRingStubServices();
                               withdrawn.store(true);
                           });
    // The flag clears first, so a call that starts now is not admitted.
    while (RingStubsPublished())
        std::this_thread::yield();
    FakeMemory later(built(Device{}));
    auto       laterCtx      = contextOf(entryOf());
    const bool laterConsumed = ConsumeWindowWriter(laterCtx, &later, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const bool waited = !withdrawn.load();
    release.store(true);
    consumer.join();
    withdrawal.join();
    UNSCOPED_INFO("a call that starts after the flag clears does not consume");
    REQUIRE((!laterConsumed && later.reads == 0));
    UNSCOPED_INFO("the withdrawal waits for the consume in flight");
    REQUIRE(waited);
    UNSCOPED_INFO("the call in flight finishes consumed");
    REQUIRE((consumed.load() && memory.log.empty()));
    UNSCOPED_INFO("the withdrawal returns after it");
    REQUIRE((withdrawn.load() && !RingStubsPublished()));
#if REREVVED_DEVELOPER
    const auto counts = TakeWindowWriterConsumeCounts();
    UNSCOPED_INFO("only the call in flight was consumed");
    REQUIRE(counts.consumed == 1);
#endif
}

// Withdrawal between cvar check and lease consumes/counts nothing.
void testWithdrawalBeforeTheLease()
{
    const Cvars cvars(true, false);
    const Stubs stubs(true);
    FakeMemory  memory(built(Device{}));
    auto        ctx = contextOf(entryOf());
    UNSCOPED_INFO("the cvar test passes");
    REQUIRE(WindowWriterConsumeRequested());
    SetRingStubLeaseProbe([]() noexcept
                          {
                              ringStubsActive.store(false, std::memory_order_seq_cst);
                          });
    const bool consumed = ConsumeWindowWriter(ctx, &memory, false);
    SetRingStubLeaseProbe(nullptr);
    UNSCOPED_INFO("a withdrawal before the lease: not consumed, nothing read");
    REQUIRE((!consumed && memory.reads == 0));
#if REREVVED_DEVELOPER
    const auto counts = TakeWindowWriterConsumeCounts();
    UNSCOPED_INFO("a withdrawal before the lease: nothing counted");
    REQUIRE(counts.consumed == 0);
    for (const auto reason : counts.fallback)
    {
        UNSCOPED_INFO("a withdrawal before the lease: no fallback counted");
        REQUIRE(reason == 0);
    }
#endif
}

// A thread's own lease does not block its own disable or withdrawal.
void testOwnLeaseDoesNotBlock()
{
    PublishRingStubServices(std::make_shared<NoServices>());
    const Stubs         stubs(true);
    const RingStubLease lease;
    UNSCOPED_INFO("the lease is granted while the stubs are published");
    REQUIRE(static_cast<bool>(lease));
    DisableRingStubs("test: own lease");
    UNSCOPED_INFO("the disable returns and clears the flag under its own lease");
    REQUIRE(!RingStubsPublished());
    const RingStubLease refused;
    UNSCOPED_INFO("no lease is granted once the stubs are disabled");
    REQUIRE(!refused);
}

#if REREVVED_DEVELOPER
void testObserver()
{
    // With only the consume on, the Original runs it falls back to report the ring words they stored.
    const Cvars cvars(true, false);
    const Stubs stubs(false); // stubs off: the consume falls back
    (void)TakeWindowWriterConsumeCounts();
    (void)TakeWindowWriterExitCounts();
    const auto entry = entryOf();
    const auto run   = [&](const Device& device, const Deviation& deviation)
    {
        FakeMemory memory(built(device));
        auto       ctx = contextOf(entry);
        runHook(ctx, &memory, false, [&](PPCContext& c)
                {
                    runOriginal(c, memory, entry, deviation);
                });
    };
    run(Device{}, {});
    run(tiledDevice(2), {});
    Deviation flush;
    flush.cursorAdd = -64;
    run(Device{}, flush); // the cursor ended below its entry value
    Deviation wide;
    wide.cursorAdd = 4 * 1000;
    run(Device{}, wide);
    Deviation partial;
    partial.cursorAdd = 2;
    run(Device{}, partial);
    const auto counts = TakeWindowWriterConsumeCounts();
    UNSCOPED_INFO("Original runs under the consume are counted");
    REQUIRE((counts.original == 2 && counts.ringWordsOriginal == 4 + 20 && counts.originalUnbounded == 3));
    UNSCOPED_INFO("every call fell back");
    REQUIRE((counts.fallback[static_cast<std::size_t>(WindowWriterFallback::Stubs)] == 5 && counts.consumed == 0));
    const auto exit = TakeWindowWriterExitCounts();
    UNSCOPED_INFO("the observer is not the exit probe");
    REQUIRE((exit.calls == 0 && exit.unreadable == 0));
    // The boundary: the most one call stores.
    Deviation most;
    most.cursorAdd = 4 * (kWindowWriterMaxWords - 4);
    run(Device{}, most);
    Deviation over;
    over.cursorAdd = 4 * (kWindowWriterMaxWords - 4 + 1);
    run(Device{}, over);
    const auto edge = TakeWindowWriterConsumeCounts();
    UNSCOPED_INFO("the bound is the most one call stores");
    REQUIRE((edge.original == 1 && edge.ringWordsOriginal == kWindowWriterMaxWords && edge.originalUnbounded == 1));

    // With neither cvar nothing is observed.
    const Cvars off(false, false);
    run(Device{}, {});
    const auto none = TakeWindowWriterConsumeCounts();
    UNSCOPED_INFO("an off observer counts nothing");
    REQUIRE((none.original == 0 && none.originalUnbounded == 0));
    {
        FakeMemory                  memory(built(Device{}));
        auto                        ctx = contextOf(entry);
        const WindowWriterExitProbe probe(ctx, &memory);
        UNSCOPED_INFO("an off probe is not armed and reads nothing");
        REQUIRE((!probe.Armed() && memory.reads == 0));
    }
    {
        const Cvars                 on(true, false);
        FakeMemory                  memory(built(Device{}));
        auto                        ctx = contextOf(entry);
        const WindowWriterExitProbe armed(ctx, &memory);
        UNSCOPED_INFO("the observer is armed with the consume on");
        REQUIRE(armed.Armed());
        const Cvars                 full(false, true);
        const WindowWriterExitProbe probe(ctx, &memory);
        UNSCOPED_INFO("the probe is armed with the exit check on");
        REQUIRE(probe.Armed());
        FakeMemory                  late(built([]
                                               {
                                  Device d;
                                  d.cursor = d.limit38 + 4;
                                  return d;
                                               }()));
        const WindowWriterExitProbe refused(ctx, &late);
        UNSCOPED_INFO("a refused entry is not armed");
        REQUIRE(!refused.Armed());
        (void)TakeWindowWriterExitCounts();
    }
}

#endif

} // namespace

TEST_CASE("window_writer_consume", "[ctest][window-writer-consume][timeout-60]")
{
    testDefaults();
    testVerdict();
#if REREVVED_DEVELOPER
    testCallers();
#endif
    testConsumed();
    testFallbacks();
    testCvarOff();
#if REREVVED_DEVELOPER
    testCallerCounts();
    testFormat();
    testProbe();
    testObserver();
#endif
    testUnpublished();
#if REREVVED_DEVELOPER
    testObserverStopsWithTheStubs();
#endif
    testLeaseDrain(false);
    testLeaseDrain(true);
    testWithdrawalBeforeTheLease();
    testOwnLeaseDoesNotBlock();
    testStackOverlap();
}
