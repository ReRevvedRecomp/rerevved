#include "capital_grace.h"
#include "capital_grace_hooks.h"
#include "capital_grace_special.h"
#include "cvar_scope.hpp"
#include "logging_scope.hpp"
#include "raging_barbarians_settings.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <spdlog/sinks/ostream_sink.h>

REXCVAR_DECLARE(bool, raging_barbarians_trace);

namespace
{

namespace grace                            = rerevved::capital_grace;
constexpr uint32_t                 kStack  = 0x40008000;
constexpr uint32_t                 kRecord = 0x40001000;
constexpr uint32_t                 kUnit   = 0x830F2BF0 + 84 * (256 * 5 + 9);
constexpr uint32_t                 kParent = kUnit - 84;
constexpr uint32_t                 kCity   = 0x8314FE28 + 188 * 3;
constexpr uint32_t                 kQueue  = 0x8314F1F8;
constexpr uint32_t                 kRing   = 0x83155C28;
constexpr uint32_t                 kSubmit = kStack - 256 - 144;
constexpr uint32_t                 kTarget = 40 * 11 + 10;
constexpr uint32_t                 kCache  = 0x830E33A8 + 4 * 5;
constexpr std::array<uint32_t, 12> kPages{
    0x40000000, 0x82CD0000, 0x82CE0000, 0x82D10000, 0x82D20000, 0x82F70000, 0x830E0000, 0x830F0000, 0x83100000, 0x83120000, 0x83140000, 0x83150000
};

struct Snapshot
{
    std::array<std::vector<uint8_t>, kPages.size()> pages;

    void Byte(uint32_t address, uint8_t value)
    {
        for (size_t i = 0; i < kPages.size(); ++i)
        {
            if (address >= kPages[i] && address - kPages[i] < 0x10000)
            {
                pages[i][address - kPages[i]] = value;
                return;
            }
        }
        FAIL("snapshot address is not mapped");
    }

    void MinusOne(uint32_t address)
    {
        for (uint32_t i = 0; i < 4; ++i)
        {
            Byte(address + i, 0xFF);
        }
    }
};

struct World
{
    rex::Runtime                               runtime{ std::filesystem::path{} };
    const bool                                 savedPolicy    = rerevved::raging_barbarians::Evaluate(0).enabled;
    rex::runtime::ThreadState*                 previousThread = rex::runtime::ThreadState::Get();
    std::unique_ptr<rex::runtime::ThreadState> thread;

    World()
    {
        REQUIRE(runtime.Setup(rex::RuntimeConfig{ .kernel_init = rex::kernel::InitializeKernel, .tool_mode = true }) == 0);
        constexpr uint32_t allocation = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
        constexpr uint32_t writable   = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
        for (const auto page : kPages)
        {
            auto* heap = runtime.memory()->LookupHeap(page);
            REQUIRE((heap && heap->AllocFixed(page, 0x10000, 0x10000, allocation, writable)));
        }
        // Literal native instruction encodings seed the capability check.
        for (const auto [address, value] : std::array<std::pair<uint32_t, uint32_t>, 31>{ {
                 { 0x82CDDF34, 0x4BFF6DBD },
                 { 0x82CE16AC, 0x3D408315 },
                 { 0x82CE16B0, 0x38600001 },
                 { 0x82CE16B4, 0x816A6344 },
                 { 0x82CE16B8, 0x396BFFFF },
                 { 0x82CE16BC, 0x916A6344 },
                 { 0x82CE16C0, 0x38210170 },
                 { 0x82CE16E8, 0x81090000 },
                 { 0x82CE16F4, 0x817FFFFC },
                 { 0x82CE1730, 0x48000008 },
                 { 0x82CE1748, 0x556A06FE },
                 { 0x82CE1784, 0x817DEFE0 },
                 { 0x82CE17EC, 0x38210090 },
                 { 0x82CE1828, 0x38210090 },
                 { 0x82D2C9BC, 0x4BFB4F95 },
                 { 0x82D2C9C0, 0x4BFA8EF9 },
                 { 0x82D2CE00, 0x4BFB4B51 },
                 { 0x82D2CE04, 0x3FA08313 },
                 { 0x82D2C170, 0x4BFB57E1 },
                 { 0x82D2C174, 0x4BFA9745 },
                 { 0x82D2BD84, 0x4BFB5BCD },
                 { 0x82D2BD88, 0x3D608315 },
                 { 0x82D2BBA0, 0x61280001 },
                 { 0x82D2BDE0, 0x8177B8E8 },
                 { 0x82D2BE00, 0x4BFFE90C },
                 { 0x82D2D2E0, 0x38600001 },
                 { 0x82D1B43C, 0x7F139000 },
                 { 0x82D1D1B4, 0x38210140 },
                 { 0x82D2BBA4, 0x7D1E292E },
                 { 0x82D2BDF8, 0x5507003C },
                 { 0x82D2BDFC, 0x7CEB512E },
             } })
        {
            Word(address, value);
        }
        Word(0x8312B8E0, static_cast<uint32_t>(-3001));
        Word(0x82F700B0, 5);
        Word(0x82F77364, 40);
        Word(0x82F77448, 40);
        Word(0x82F7A0B8 + 8, 1);
        Word(0x82F7A168 + 8, 0);
        Word(0x8312B8E8, 5);
        Word(0x8312B8EC, 9);
        Word(kCache, 9);
        std::memset(Base() + kUnit, 0xA5, 84);
        Base()[kUnit]     = 5;
        Base()[kUnit + 1] = 3;
        Base()[kUnit + 2] = 9;
        Word(kUnit + 12, 0x200);
        Half(kUnit + 0x1C, 10);
        Half(kUnit + 0x1E, 10);
        Half(kUnit + 0x20, 2);
        Half(kUnit + 0x22, 6);
        Half(kUnit + 0x24, 7);
        Base()[kCity]     = 1;
        Base()[kCity + 3] = 6;
        Word(kCity + 0x10, 1);
        Half(kCity + 0x30, 11);
        Half(kCity + 0x32, 10);
        Base()[0x83128D08 + kTarget] = 3;
        Base()[0x830E6850 + kTarget] = 1;
        Half(0x8312A608 + 2 * kTarget, 1);
        thread = std::make_unique<rex::runtime::ThreadState>(1, kStack, 0, runtime.memory());
        rex::runtime::ThreadState::Bind(thread.get());
        rerevved::raging_barbarians::RestoreForGame(true);
        Command({ 0, 5, 9, 0xABCD0002, 0x11223344 });
        // No test may inherit a pending producer/cleanup escape from another world.
        PPCRegister badQueue{};
        BeginCapitalGraceSubmit(thread->context()->r1, badQueue, badQueue);
    }

    ~World()
    {
        rerevved::raging_barbarians::RestoreForGame(savedPolicy);
        rex::runtime::ThreadState::Bind(previousThread);
    }

    uint8_t* Base()
    {
        return runtime.memory()->virtual_membase();
    }

    void Word(uint32_t address, uint32_t value)
    {
        rex::memory::store_and_swap<uint32_t>(Base() + address, value);
    }

    uint32_t Word(uint32_t address)
    {
        return rex::memory::load_and_swap<uint32_t>(Base() + address);
    }

    void Half(uint32_t address, uint16_t value)
    {
        rex::memory::store_and_swap<uint16_t>(Base() + address, value);
    }

    void Command(const grace::Words& words, uint32_t record = kRecord)
    {
        for (size_t i = 0; i < words.size(); ++i)
        {
            Word(record + static_cast<uint32_t>(4 * i), words[i]);
        }
        auto& context   = *thread->context();
        context.r3.u64  = record;
        context.r30.u64 = words[0];
        context.r31.u64 = words[1];
        context.r15.u64 = words[2];
        context.r29.u64 = words[3];
        context.r14.u64 = 0x123456789ABCDEF0;
        context.lr      = 0x82CDDF10;
    }

    bool Admit()
    {
        auto&      context = *thread->context();
        const auto before  = context;
        const bool result  = GuardCapitalGraceAdmission(context.r3, context.r30, context.r31, context.r15, context.r29);
        CHECK(std::memcmp(&before, &context, sizeof(context)) == 0);
        return result;
    }

    Snapshot Save()
    {
        Snapshot result;
        for (size_t i = 0; i < kPages.size(); ++i)
        {
            result.pages[i].assign(Base() + kPages[i], Base() + kPages[i] + 0x10000);
        }
        return result;
    }

    void Check(const Snapshot& expected)
    {
        for (size_t i = 0; i < kPages.size(); ++i)
        {
            INFO("guest page " << kPages[i]);
            CHECK(std::memcmp(expected.pages[i].data(), Base() + kPages[i], 0x10000) == 0);
        }
    }

    void QueueRefusal(bool linked = false)
    {
        auto& c  = *thread->context();
        c.r1.u64 = kStack;
        c.r3.u64 = 17;
        c.r4.u64 = 5;
        c.r5.u64 = 9;
        c.r6.u64 = 2;
        c.r7.u64 = 0x11223344;
        if (linked)
        {
            BeginCapitalGraceLinked(c.r1, c.r3, c.r4, c.r5, c.r6, c.r7);
        }
        else
        {
            BeginCapitalGraceBlitz(c.r1, c.r3, c.r4, c.r5, c.r6, c.r7);
        }
        const grace::Words words{ 17, 5, 9, 2, 0x11223344 };
        Command(words, kStack - 176);
        c.r1.u64  = kSubmit;
        c.r31.u64 = kQueue;
        c.r30.u64 = kRing;
        BeginCapitalGraceSubmit(c.r1, c.r31, c.r30);
        c.r9.u64 = kStack - 176;
        Word(kQueue - 4, 100);
        Word(kQueue, 100);
        AppendCapitalGraceToken(c.r1, c.r31, c.r9);
        Word(kQueue - 4, 101);
        CompleteCapitalGraceAppend(c.r1);
        c.r11.u64 = 100;
        ObserveCapitalGraceDrainSerial(c.r1, c.r31, c.r11);
        Command(words, kSubmit + 80);
        BindCapitalGraceDrain(c.r1);
        REQUIRE(Admit());
        EndCapitalGraceSubmit(c.r1);
        c.r1.u64 = kStack;
    }
};

} // namespace

TEST_CASE("capital_grace_admission_prevents_defended_combat", "[ctest][capital-grace]")
{
    World world;
    auto  expected = world.Save();
    expected.Byte(kUnit + 2, 0);
    expected.Byte(kUnit + 0x22, 0xFF);
    expected.Byte(kUnit + 0x23, 0xFF);
    expected.MinusOne(0x8312B8EC);
    expected.MinusOne(kCache);
    CHECK(world.Admit()); // True selects the native consumed/result-1 tail before combat.
    world.Check(expected);
}

TEST_CASE("capital_grace_type1_uses_remainder_and_current_direction", "[ctest][capital-grace]")
{
    World world;
    world.Command({ 1, 5, 0xAB09, 7, 255 });
    CHECK(world.Admit());
    CHECK(world.Base()[kUnit + 2] == 0);
    CHECK(world.Base()[kUnit + 0x22] == 0xFF);
}

TEST_CASE("capital_grace_native_paths_preserve_all_bytes", "[ctest][capital-grace]")
{
    World world;
    SECTION("Raging off")
    {
        rerevved::raging_barbarians::RestoreForGame(false);
    }
    SECTION("cutoff equality")
    {
        world.Word(0x8312B8E0, static_cast<uint32_t>(-3000));
    }
    SECTION("after cutoff")
    {
        world.Word(0x8312B8E0, static_cast<uint32_t>(-2999));
    }
    SECTION("other civilization")
    {
        world.Command({ 0, 1, 9, 2, 0 });
    }
    SECTION("noncapital")
    {
        world.Word(kCity + 0x10, 2);
    }
    SECTION("field")
    {
        world.Base()[0x83128D08 + kTarget] = 0xFF;
    }
    SECTION("empty city")
    {
        world.Base()[kCity + 3] = 0;
    }
    SECTION("different coordinates")
    {
        world.Half(kCity + 0x30, 12);
    }
    SECTION("Spy foreign-city arrival")
    {
        world.Base()[kUnit + 1] = 28;
        world.Command({ 17, 5, 9, 2, 0 });
    }
    SECTION("Caravan foreign-city arrival")
    {
        world.Base()[kUnit + 1] = 29;
        world.Command({ 17, 5, 9, 2, 0 });
    }
    const auto before = world.Save();
    CHECK_FALSE(world.Admit());
    world.Check(before);
}

TEST_CASE("capital_grace_invalid_input_and_capability_stay_native", "[ctest][capital-grace]")
{
    World world;
    SECTION("unsupported image")
    {
        world.Word(0x82CE16B0, 0x38600000); // A retry tail must never arm protection.
    }
    SECTION("invalid Barbarian")
    {
        world.Word(0x82F700B0, 8);
    }
    SECTION("invalid width")
    {
        world.Word(0x82F77364, 0);
    }
    SECTION("Type1 composite outside profile")
    {
        world.Command({ 1, 5, 65536, 2, 0 });
    }
    SECTION("Move17 requires full direction")
    {
        world.Command({ 17, 5, 9, 0x10002, 0 });
    }
    SECTION("dead actor")
    {
        world.Word(kUnit + 12, 0x80000000);
    }
    SECTION("invalid initialized type")
    {
        world.Base()[kUnit + 1] = 38;
    }
    SECTION("read-only actor cannot be partially retired")
    {
        auto* heap = world.runtime.memory()->LookupHeap(kUnit);
        REQUIRE(heap->Protect(kUnit & ~uint32_t{ 0xFFF }, 0x1000, rex::memory::kMemoryProtectRead));
    }
    SECTION("read-only cache cannot leave partial actor writes")
    {
        auto* heap = world.runtime.memory()->LookupHeap(kCache);
        REQUIRE(heap->Protect(kCache & ~uint32_t{ 0xFFF }, 0x1000, rex::memory::kMemoryProtectRead));
    }
    SECTION("unreadable record")
    {
        world.thread->context()->r3.u64 = 0x20000000;
    }
    const auto before = world.Save();
    CHECK_FALSE(world.Admit());
    world.Check(before);
}

TEST_CASE("capital_grace_busy_new_order_preserves_older_actor", "[ctest][capital-grace]")
{
    World world;
    world.Word(kUnit + 12, 0x201);
    const auto before = world.Save();
    CHECK(world.Admit());
    world.Check(before);
    world.QueueRefusal();
    CHECK(EndCapitalGraceBlitz(world.thread->context()->r1));
    CHECK(world.Base()[kUnit + 2] == 9);
    CHECK(world.Word(kUnit + 12) == 0x201);
    CHECK(world.Word(0x8312B8EC) == 9);
    CHECK(world.Word(kCache) == 9);
}

TEST_CASE("capital_grace_refusal_clears_producer_action_bit", "[ctest][capital-grace]")
{
    // Producers set 0x8000 before submitting. If it stays set, the next turn's
    // selection at 0x82D26CA4 returns "in progress" for the refilled unit forever.
    World world;
    SECTION("fresh actor")
    {
        world.Word(kUnit + 12, 0x8200);
        auto expected = world.Save();
        expected.Byte(kUnit + 12 + 2, 0x02);
        expected.Byte(kUnit + 2, 0);
        expected.Byte(kUnit + 0x22, 0xFF);
        expected.Byte(kUnit + 0x23, 0xFF);
        expected.MinusOne(0x8312B8EC);
        expected.MinusOne(kCache);
        CHECK(world.Admit());
        world.Check(expected);
    }
    SECTION("busy actor keeps everything else")
    {
        world.Word(kUnit + 12, 0x8201);
        auto expected = world.Save();
        expected.Byte(kUnit + 12 + 2, 0x02);
        CHECK(world.Admit());
        world.Check(expected);
    }
}

TEST_CASE("capital_grace_independent_cache_retirement", "[ctest][capital-grace]")
{
    World world;
    world.Word(0x8312B8E8, 4);
    auto expected = world.Save();
    expected.Byte(kUnit + 2, 0);
    expected.Byte(kUnit + 0x22, 0xFF);
    expected.Byte(kUnit + 0x23, 0xFF);
    expected.MinusOne(kCache);
    CHECK(world.Admit());
    world.Check(expected);
}

TEST_CASE("capital_grace_capture_gate_and_fresh_backstop", "[ctest][capital-grace]")
{
    World world;
    world.Command({ 27, 5, 3, 0, 9 });
    world.Half(kCity + 0x30, 30); // Capture uses the given ordinal, not target coordinates.
    const auto before = world.Save();
    CHECK(world.Admit());
    world.Check(before);
    auto& c              = *world.thread->context();
    c.r3.u64             = 3;
    c.r4.u64             = 5;
    const auto registers = c;
    CHECK(GuardCapitalGraceCapture(c.r3, c.r4));
    CHECK(std::memcmp(&registers, &c, sizeof(c)) == 0);
    world.Check(before);
    world.Word(kCity + 0x10, 0);
    CHECK_FALSE(GuardCapitalGraceCapture(c.r3, c.r4));
    world.Word(kCity + 0x10, 1);
    world.Word(0x8312B8E0, static_cast<uint32_t>(-3000));
    CHECK_FALSE(GuardCapitalGraceCapture(c.r3, c.r4));
    CHECK(c.r3.u64 == 3);
    CHECK(c.r4.u64 == 5);
}

TEST_CASE("capital_grace_unresolved_special_routes_consume_refuse", "[ctest][capital-grace]")
{
    World world;
    SECTION("nuclear packed Move17")
    {
        world.Word(0x82F700D8 + 148 * 3 + 0x50, 0x20000);
    }
    SECTION("Type24 without a coordinate target")
    {
        world.Base()[kUnit + 1] = 24;
    }
    world.Command({ 17, 5, 9, 1234, 0 });
    CHECK(world.Admit());
    CHECK(world.Base()[kUnit + 2] == 0);
}

TEST_CASE("capital_grace_special_capability_warns_without_trace", "[ctest][capital-grace]")
{
    World                                world;
    const rerevved::tests::ScopedLogging logging(false);
    REREVVED_RESTORE_CVAR(raging_barbarians_trace);
    REXCVAR_SET(raging_barbarians_trace, false);
    std::ostringstream output;
    auto               sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    sink->set_pattern("%l %v");
    rex::AddSink(sink);

    struct RemoveSink
    {
        spdlog::sink_ptr sink;

        ~RemoveSink()
        {
            rex::RemoveSink(sink);
        }
    } removeSink{ sink };

    const auto          type24Before    = grace::SpecialRefusalCount(grace::SpecialReason::Type24Capability);
    const auto          nuclearBefore   = grace::SpecialRefusalCount(grace::SpecialReason::NuclearCapability);
    const auto          protectedBefore = grace::SpecialRefusalCount(grace::SpecialReason::ProtectedFootprint);
    grace::SpecialInput input{ grace::SpecialRoute::Type24, { 17, 5, 9, 410, 0 }, { 5, 24, 0, { 10, 10 }, 0 }, -3001, 5 };
    for (int i = 0; i < 2; ++i)
    {
        input.route = grace::SpecialRoute::Type24;
        CHECK(grace::AdmitSpecial(input) == grace::SpecialDecision::RefuseUnsupported);
        input.route = grace::SpecialRoute::Nuclear;
        CHECK(grace::AdmitSpecial(input) == grace::SpecialDecision::RefuseUnsupported);
    }
    CHECK(grace::SpecialRefusalCount(grace::SpecialReason::Type24Capability) == type24Before + 2);
    CHECK(grace::SpecialRefusalCount(grace::SpecialReason::NuclearCapability) == nuclearBefore + 2);
    CHECK(grace::SpecialRefusalCount(grace::SpecialReason::ProtectedFootprint) == protectedBefore);
    rex::FlushLogging();
    std::istringstream lines(output.str());
    std::string        line;
    size_t             warnings = 0;
    while (std::getline(lines, line))
    {
        if (line.find("warning Capital grace: unsupported capability:") != std::string::npos)
        {
            ++warnings;
        }
    }
    CHECK(warnings == static_cast<size_t>(type24Before == 0) + static_cast<size_t>(nuclearBefore == 0));
    CHECK(output.str().find("protected footprint") == std::string::npos);

    auto* heap = world.runtime.memory()->LookupHeap(0x82CF0000);
    REQUIRE(heap);
    REQUIRE(heap->AllocFixed(0x82CF0000, 0x10000, 0x10000, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
    for (const auto [address, value] : std::array<std::pair<uint32_t, uint32_t>, 15>{ { { 0x82CF7498, 0x3A06A608 }, { 0x82CF7938, 0x480034B9 }, { 0x82CF76A4, 0x7D3F51AE }, { 0x82CF77F0, 0x81410114 }, { 0x82CF7BF0, 0x7CFF59AE }, { 0x82CF7D08, 0x38A00000 }, { 0x82CF78CC, 0x38210100 }, { 0x82CF78D0, 0x4BB003D0 }, { 0x82CF7D34, 0x3D40830F }, { 0x82CF7D68, 0x38210100 }, { 0x82CF7B7C, 0x38210100 }, { 0x82CF75D0, 0x38210100 }, { 0x82CDF1C8, 0x2F14FFFF }, { 0x82CDF26C, 0x81790000 }, { 0x82CDF1C4, 0x480231CD } } })
    {
        world.Word(address, value);
    }
    // The center capital makes the known footprint refuse before victim scans.
    world.Base()[0x83128D08 + 410] = 3;
    world.Half(0x8312A608 + 2 * 410, 1);
    const auto beforeProtected = output.str();
    CHECK(grace::AdmitSpecial(input) == grace::SpecialDecision::RefuseProtected);
    CHECK(grace::SpecialRefusalCount(grace::SpecialReason::ProtectedFootprint) == protectedBefore + 1);
    rex::FlushLogging();
    const auto quiet = output.str();
    CHECK(quiet == beforeProtected);
    REXCVAR_SET(raging_barbarians_trace, true);
    CHECK(grace::AdmitSpecial(input) == grace::SpecialDecision::RefuseProtected);
    rex::FlushLogging();
    CHECK(output.str().substr(quiet.size()).find("info Capital grace: protected footprint:") != std::string::npos);
    CHECK(grace::SpecialRefusalCount(grace::SpecialReason::ProtectedFootprint) == protectedBefore + 2);
    CHECK(grace::SpecialRefusalCount(grace::SpecialReason::NuclearCapability) == nuclearBefore + 2);
}

TEST_CASE("capital_grace_Blitz_escape_is_exact_and_one_shot", "[ctest][capital-grace]")
{
    World world;
    world.QueueRefusal();
    auto&      c      = *world.thread->context();
    const auto before = world.Save();
    CHECK(EndCapitalGraceBlitz(c.r1));
    CHECK_FALSE(EndCapitalGraceBlitz(c.r1));
    world.Check(before);
    c.r3.u64 = 17;
    c.r4.u64 = 5;
    c.r5.u64 = 9;
    c.r6.u64 = 2;
    c.r7.u64 = 0x11223344;
    BeginCapitalGraceBlitz(c.r1, c.r3, c.r4, c.r5, c.r6, c.r7);
    CHECK_FALSE(EndCapitalGraceBlitz(c.r1));
}

TEST_CASE("capital_grace_linked_parent_cleanup_is_native", "[ctest][capital-grace]")
{
    World world;
    world.Base()[kParent] = 5;
    world.Word(kParent + 12, 0x200);
    world.Word(0x8312B8EC, 8);
    auto& c   = *world.thread->context();
    c.r1.u64  = kStack;
    c.r15.u64 = 0x830F2BF0;
    c.r23.u64 = 0x83130000;
    c.r25.u64 = c.r31.u64 = 8;
    c.r9.u64              = 0x200;
    PrepareCapitalGraceParent(c.r1, c.r15, c.r23, c.r25, c.r31, c.r9);
    world.Word(kParent + 12, 0x201); // Native BBA0/BBA4 has now prepared busy.
    world.QueueRefusal(true);
    c.r15.u64 = 0x830F2BF0;
    c.r23.u64 = 0x83130000;
    c.r25.u64 = c.r31.u64 = 8;
    SECTION("changed selection retains native continuation")
    {
        world.Word(0x8312B8EC, 7);
        const auto before = world.Save();
        CHECK_FALSE(EndCapitalGraceLinked(c.r1, c.r15, c.r23, c.r25, c.r31));
        CHECK_FALSE(FinishCapitalGraceParent(c.r1));
        world.Check(before);
        return;
    }
    SECTION("matched cleanup")
    {
        const auto before = world.Save();
        REQUIRE(EndCapitalGraceLinked(c.r1, c.r15, c.r23, c.r25, c.r31));
        world.Check(before); // The BD88 hook does not write the parent or child.
        CHECK(world.Word(kParent + 12) == 0x201);
        world.Word(kParent + 12, 0x200); // Native BDE0..BDFC clears only busy.
        CHECK(FinishCapitalGraceParent(c.r1));
        CHECK_FALSE(FinishCapitalGraceParent(c.r1));
        CHECK(world.Word(kParent + 12) == 0x200);
    }
}
