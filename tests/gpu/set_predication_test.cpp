

#include "developer_checks.hpp"
#include "gpu/guest_d3d/ring_stubs.h"
#include "gpu/guest_d3d/set_predication_consume.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

REXCVAR_DECLARE(bool, native_consume_set_predication);
REXCVAR_DECLARE(bool, native_consume_set_predication_preswap);
DEV_CVAR_DECLARE(bool, native_set_predication_exit_check);

using namespace rerevved::gpu::guest_d3d;

namespace
{

constexpr std::uint32_t kG = 0x40010000, kCursor = 0x00200000, kLimit = kCursor + 0x100;
// A caller stack far from the device, so its frame overlaps nothing the model reads. The thread's stack bounds
// are the base at KPCR+0x70 and the limit at KPCR+0x74, with the KPCR at r13.
constexpr std::uint32_t kQuietSp = 0x7000F000;
constexpr std::uint32_t kPcr     = 0x70200000;

// Big-endian guest bytes; an address that is absent is not mapped.
struct Memory
{
    std::unordered_map<std::uint32_t, std::uint8_t> bytes;

    bool has(std::uint32_t address, std::uint32_t size) const
    {
        for (std::uint32_t i = 0; i < size; ++i)
            if (!bytes.contains(address + i))
                return false;
        return true;
    }

    bool word(std::uint32_t address, std::uint32_t& value) const
    {
        if (!has(address, 4))
            return false;
        value = 0;
        for (std::uint32_t i = 0; i < 4; ++i)
            value = value << 8 | bytes.at(address + i);
        return true;
    }

    std::uint32_t word(std::uint32_t address) const
    {
        std::uint32_t value = 0;
        UNSCOPED_INFO("word not mapped");
        REQUIRE(word(address, value));
        return value;
    }

    void setWord(std::uint32_t address, std::uint32_t value)
    {
        for (std::uint32_t i = 0; i < 4; ++i)
            bytes[address + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
    }

    std::uint8_t byte(std::uint32_t address) const
    {
        return bytes.at(address);
    }

    void setByte(std::uint32_t address, std::uint8_t value)
    {
        bytes[address] = value;
    }

    friend bool operator==(const Memory&, const Memory&) = default;
};

GuestWordReader readerOf(const Memory& memory)
{
    return [&memory](std::uint32_t address, std::uint32_t& value)
    {
        return memory.word(address, value);
    };
}

SetPredicationWritable writableOf(const Memory& memory, const std::set<std::uint32_t>* blocked = nullptr)
{
    return [&memory, blocked](std::uint32_t address, std::uint32_t size)
    {
        if (blocked)
            for (std::uint32_t i = 0; i < size; ++i)
                if (blocked->contains(address + i))
                    return false;
        return memory.has(address, size);
    };
}

struct Writes
{
    std::vector<SetPredicationWrite> list;
};

SetPredicationWriter writerOf(Memory& memory, Writes* log = nullptr)
{
    return [&memory, log](const SetPredicationWrite& write)
    {
        if (log)
            log->list.push_back(write);
        if (write.byte)
            memory.setByte(write.address, static_cast<std::uint8_t>(write.value));
        else
            memory.setWord(write.address, write.value);
    };
}

void placeStack(Memory& memory, std::uint32_t limit, std::uint32_t base)
{
    memory.setWord(kPcr + 0x70, base);
    memory.setWord(kPcr + 0x74, limit);
}

struct Fixture
{
    std::uint8_t                 old  = 0;
    std::uint8_t                 flag = 0;
    std::array<std::uint32_t, 5> bound{ 0x40100000, 0x40100100, 0x40100200, 0x40100300, 0x40100400 };
    std::array<std::uint32_t, 5> snapshot{ 0x40100000, 0x40100100, 0x40100200, 0x40100300, 0x40100400 };
    std::uint32_t                cursor = kCursor, limit = kLimit;
    // The old values of the words the call stores; distinctive, so every store changes its word.
    std::uint32_t predicate = 0x7, mask = 0x12345678, word31a8 = 0x55, word2ab4 = 0x66, word2ab8 = 0x77;
};

// The other three bytes of the word G+0x2ABC and the other three of G+0x2F98 hold marks the byte store must keep.
constexpr std::uint32_t kTilingMarks = 0x00123456, kFlagMarks = 0xAABBCC00;
constexpr std::uint32_t kSentinels[] = { 0x2884, 0x2AB0, 0x2AC0, 0x2F94, 0x2F9C, 0x31A0, 0x31B0, 0x31B4, 0x31CC, 0x34, 0x3C };

Memory build(const Fixture& f)
{
    Memory m;
    placeStack(m, kQuietSp - 0x1000, kQuietSp + 0x1000);
    m.setWord(kG + 0x30, f.cursor);
    m.setWord(kG + 0x38, f.limit);
    m.setWord(kG + 0x2ABC, static_cast<std::uint32_t>(f.old) << 24 | kTilingMarks);
    m.setWord(kG + 0x2F98, kFlagMarks | f.flag);
    for (std::uint32_t i = 0; i < 5; ++i)
    {
        m.setWord(kG + 0x3098 + 4 * i, f.bound[i]);
        m.setWord(kG + 0x31B8 + 4 * i, f.snapshot[i]);
    }
    m.setWord(kG + 0x31AC, f.predicate);
    m.setWord(kG + 0x31A4, f.mask);
    m.setWord(kG + 0x31A8, f.word31a8);
    m.setWord(kG + 0x2AB4, f.word2ab4);
    m.setWord(kG + 0x2AB8, f.word2ab8);
    for (const auto offset : kSentinels)
        m.setWord(kG + offset, 0x99990000 + offset);
    // The ring: the two words Original writes, and a word past them it must not touch.
    m.setWord(f.cursor + 4, 0);
    m.setWord(f.cursor + 8, 0);
    m.setWord(f.cursor + 12, 0xFEEDFACE);
    return m;
}

// ---- The reference: the generated function, instruction by instruction. ----

struct Transcription
{
    std::uint64_t r3        = 0;
    bool          flushed   = false;
    std::uint64_t flushCall = 0; // r3 passed to the flush
};

// The caller's r31, which the prologue's std stores below the frame.
constexpr std::uint64_t kCallerR31 = 0x1122334455667788ULL;

// The prologue's stack stores (g171:12565-12572) with the caller's r1 = `sp` and link register `lr`.
void prologue(Memory& memory, std::uint32_t sp, std::uint32_t lr)
{
    memory.setWord(sp - 8, lr);                                            // stw r12,-8(r1)
    memory.setWord(sp - 16, static_cast<std::uint32_t>(kCallerR31 >> 32)); // std r31,-16(r1)
    memory.setWord(sp - 12, static_cast<std::uint32_t>(kCallerR31));
    memory.setWord(sp - 96, sp); // stwu r1,-96(r1)
}

// Literal 0x8269CD20 reference: g171:12559-12772. flush is 0x8269C588: memory/G
// in, cursor r3 out; nonzero sp runs prologue first with lr.
Transcription original(Memory&                                                     memory,
                       std::uint32_t                                               g,
                       std::uint32_t                                               r4,
                       const std::function<std::uint32_t(Memory&, std::uint32_t)>& flush = {},
                       std::uint32_t                                               sp    = 0,
                       std::uint32_t                                               lr    = 0)
{
    Transcription out;
    if (sp)
        prologue(memory, sp, lr);
    // Registers, declared first so the labels below may be jumped to.
    std::uint64_t r3 = g, r8 = 0, r9 = 0, r10 = 0, r11 = 0;
    const auto    r31 = static_cast<std::uint64_t>(g);
    const auto    lbz = [&](std::uint64_t offset)
    {
        return static_cast<std::uint64_t>(memory.byte(static_cast<std::uint32_t>(r31 + offset)));
    };
    const auto lwz = [&](std::uint64_t offset)
    {
        return static_cast<std::uint64_t>(memory.word(static_cast<std::uint32_t>(r31 + offset)));
    };
    const auto stw = [&](std::uint64_t offset, std::uint64_t value)
    {
        memory.setWord(static_cast<std::uint32_t>(r31 + offset), static_cast<std::uint32_t>(value));
    };
    const auto stb = [&](std::uint64_t offset, std::uint64_t value)
    {
        memory.setByte(static_cast<std::uint32_t>(r31 + offset), static_cast<std::uint8_t>(value));
    };

    // cmplwi cr6,r4,0 ; li r9,0 ; bne cr6,0x8269ce24
    r9 = 0;
    if (r4 != 0)
        goto loc_8269CE24;
    r10 = lbz(10940);
    stw(12716, r9);
    r11 = r10 & 0x8;
    if (r11 != 0)
        goto loc_8269CE10;
    r11 = r10 & 0x4;
    if (r11 != 0)
        goto loc_8269CE10;
    r11 = lbz(12187);
    if (static_cast<std::uint32_t>(r11) != 0)
        goto loc_8269CE10;
    r11 = r10 & 0x10;
    if (r11 == 0)
        goto loc_8269CD74;
    r11 = 1;
    goto loc_8269CE04;
loc_8269CD74:
    r11 = r10 & 0x20;
    if (r11 == 0)
        goto loc_8269CDFC;
    r11 = lwz(12440);
    r8  = lwz(12728);
    if (r8 == r11)
        goto loc_8269CD94;
    if (r11 != 0)
        goto loc_8269CDFC;
loc_8269CD94:
    r11 = lwz(12444);
    r8  = lwz(12732);
    if (r8 == r11)
        goto loc_8269CDAC;
    if (r11 != 0)
        goto loc_8269CDFC;
loc_8269CDAC:
    r11 = lwz(12448);
    r8  = lwz(12736);
    if (r8 == r11)
        goto loc_8269CDC4;
    if (r11 != 0)
        goto loc_8269CDFC;
loc_8269CDC4:
    r11 = lwz(12452);
    r8  = lwz(12740);
    if (r8 == r11)
        goto loc_8269CDDC;
    if (r11 != 0)
        goto loc_8269CDFC;
loc_8269CDDC:
    r11 = lwz(12456);
    r8  = lwz(12744);
    if (r8 == r11)
        goto loc_8269CDF4;
    if (r11 != 0)
        goto loc_8269CDFC;
loc_8269CDF4:
    r11 = 1;
    goto loc_8269CE00;
loc_8269CDFC:
    r11 = r9;
loc_8269CE00:
    r11 = r11 & 0xFF;
loc_8269CE04:
    r11 = r11 & 0xFF;
    {
        const bool nonzero = static_cast<std::uint32_t>(r11) != 0;
        r11                = 1;
        if (nonzero)
            goto loc_8269CE14;
    }
loc_8269CE10:
    r11 = r9;
loc_8269CE14:
    r11 = (r10 & 0xFE) | (r11 & 0xFFFFFFFFFFFFFF01ULL);
    r10 = 0xFFFFFFFFFFFFFFFFULL;
    stw(12708, r10);
    goto loc_8269CE38;
loc_8269CE24:
    r11 = lbz(10940);
    r10 = 1;
    stw(12708, r4);
    r11 = r11 & 0xFFFFFFFE;
    stw(12716, r10);
loc_8269CE38:
    stb(10940, r11);
    r11 = lwz(56);
    r3  = lwz(48);
    stw(12712, r9);
    stw(10932, r4);
    stw(10936, r9);
    if (!(static_cast<std::uint32_t>(r3) > static_cast<std::uint32_t>(r11)))
        goto loc_8269CE60;
    out.flushed   = true;
    out.flushCall = g;
    r3            = flush ? flush(memory, g) : lwz(48);
loc_8269CE60:
    r11 = 0xC0006000;
    r3  = static_cast<std::uint32_t>(r3 + 4);
    memory.setWord(static_cast<std::uint32_t>(r3), static_cast<std::uint32_t>(r11));
    r11 = lwz(12708);
    r3  = static_cast<std::uint32_t>(r3 + 4);
    memory.setWord(static_cast<std::uint32_t>(r3), static_cast<std::uint32_t>(r11));
    stw(48, r3);
    out.r3 = r3;
    return out;
}

// The words the model does not store: the cursor and the two ring words.
void maskRing(Memory& memory, std::uint32_t cursor)
{
    memory.setWord(kG + 0x30, 0);
    memory.setWord(cursor + 4, 0);
    memory.setWord(cursor + 8, 0);
}

SetPredicationConsumeState readyState()
{
    SetPredicationConsumeState state;
    state.stubsReady = true;
    state.caller     = 0x82500000;
    state.sp         = kQuietSp;
    state.pcr        = kPcr;
    return state;
}

std::vector<std::array<std::uint32_t, 5>> boundPatterns(std::vector<std::array<std::uint32_t, 5>>& snapshots)
{
    // The slot patterns that decide bit 0 for old & 0x20: all equal, all empty, one empty slot, one mismatch per slot.
    const std::array<std::uint32_t, 5>        equal{ 0x40100000, 0x40100100, 0x40100200, 0x40100300, 0x40100400 };
    std::vector<std::array<std::uint32_t, 5>> bound;
    bound.push_back(equal);
    snapshots.push_back(equal);
    bound.push_back({});
    snapshots.push_back(equal);
    for (std::size_t i = 0; i < 5; ++i)
    {
        auto empty = equal;
        empty[i]   = 0;
        bound.push_back(empty);
        snapshots.push_back(equal);
    }
    for (std::size_t i = 0; i < 5; ++i)
    {
        auto other = equal;
        other[i]   = 0x40100000 + 0x1000 + static_cast<std::uint32_t>(i);
        bound.push_back(other);
        snapshots.push_back(equal);
    }
    return bound;
}

void testPropertyAgainstTranscription()
{
#if REREVVED_DEVELOPER
    TakeSetPredicationConsumeCounts();
    TakeSetPredicationExitCounts();
#endif
    std::vector<std::array<std::uint32_t, 5>> snapshots;
    const auto                                bounds = boundPatterns(snapshots);
    std::vector<std::uint32_t>                arguments{ 0, 1, 2, 0x80000000 };
    for (std::uint32_t i = 0; i < 5; ++i)
        arguments.push_back(3U << (2 * i));
    std::uint64_t cases = 0, clears = 0, bitSet = 0, bitClear = 0;
    for (unsigned old = 0; old < 256; ++old)
    {
        for (const std::uint8_t flag : { std::uint8_t{ 0 }, std::uint8_t{ 1 }, std::uint8_t{ 0x80 } })
        {
            for (std::size_t p = 0; p < bounds.size(); ++p)
            {
                Fixture f;
                f.old              = static_cast<std::uint8_t>(old);
                f.flag             = flag;
                f.bound            = bounds[p];
                f.snapshot         = snapshots[p];
                const Memory entry = build(f);
                // Caller arguments: g205:8896 and 9309.
                for (const auto r4 : arguments)
                {
                    // The reference, with the exit probe armed on its entry state.
                    Memory reference = entry;
#if REREVVED_DEVELOPER
                    const auto armed = BeginSetPredicationProbe(readerOf(reference), kG, r4, kQuietSp, kPcr);
                    UNSCOPED_INFO("the probe reads the entry");
                    REQUIRE(armed.has_value());
#endif
                    const auto result = original(reference, kG, r4);
                    UNSCOPED_INFO("the fixture never flushes");
                    REQUIRE(!result.flushed);
#if REREVVED_DEVELOPER
                    EndSetPredicationProbe(readerOf(reference), *armed, static_cast<std::uint32_t>(result.r3));
#endif

                    // The consume.
                    Memory sut = entry;
                    UNSCOPED_INFO("the fixture is consumed");
                    REQUIRE(ConsumeSetPredicationWith(readyState(), readerOf(sut), writableOf(sut), writerOf(sut), kG, r4));

                    Memory expected = reference;
                    Memory actual   = sut;
                    maskRing(expected, kCursor);
                    maskRing(actual, kCursor);
                    if (expected != actual)
                    {
                        for (const auto& [address, value] : expected.bytes)
                            if (!actual.bytes.contains(address) || actual.bytes.at(address) != value)
                                throw std::runtime_error("memory differs at offset " + std::to_string(address - kG) + " old " + std::to_string(old) + " flag " + std::to_string(flag) +
                                                         " pattern " + std::to_string(p) + " r4 " + std::to_string(r4) + ": reference " + std::to_string(value) + " model " +
                                                         std::to_string(actual.bytes.contains(address) ? actual.bytes.at(address) : -1));
                        throw std::runtime_error("memory maps differ");
                    }
                    UNSCOPED_INFO("the consume leaves the cursor and the ring words");
                    REQUIRE((sut.word(kG + 0x30) == kCursor && sut.word(kCursor + 4) == 0 && sut.word(kCursor + 8) == 0 && sut.word(kCursor + 12) == 0xFEEDFACE));
                    ++cases;
                    clears += r4 == 0;
                    if (r4 == 0)
                    {
                        ((reference.byte(kG + 0x2ABC) & 1) ? bitSet : bitClear)++;
                    }
                }
            }
        }
    }
#if REREVVED_DEVELOPER
    const auto consume = TakeSetPredicationConsumeCounts();
    UNSCOPED_INFO("every case is counted consumed");
    REQUIRE((consume.consumed == cases && consume.consumedClear == clears));
    for (const auto n : consume.fallback)
    {
        UNSCOPED_INFO("no case falls back");
        REQUIRE(n == 0);
    }
    const auto exit = TakeSetPredicationExitCounts();
    UNSCOPED_INFO("the probe agrees with the transcription on every case");
    REQUIRE((exit.ok == cases && exit.flushed == 0 && exit.unreadable == 0));
    for (const auto n : exit.mismatch)
    {
        UNSCOPED_INFO("the probe finds no mismatch");
        REQUIRE(n == 0);
    }
#endif
    // The sweep reaches both values of bit 0 on the clear path.
    UNSCOPED_INFO("the sweep covers both results of the bit-0 chain");
    REQUIRE((bitSet == 496 && bitClear == 9216 - 496));
    UNSCOPED_INFO("the sweep size");
    REQUIRE(cases == 256ULL * 3 * 12 * 9);
}

SetPredicationConsumeState preSwapState()
{
    auto state           = readyState();
    state.caller         = kSetPredicationPreSwapReturn;
    state.preSwapEnabled = true;
    state.trustedPreSwap = true;
#if REREVVED_DEVELOPER
    state.reportPreSwap = true;
#endif
    return state;
}

void testPreSwapPropertyAgainstTranscription()
{
#if REREVVED_DEVELOPER
    TakeSetPredicationConsumeCounts();
    TakeSetPredicationPreSwapCounts();
    TakeSetPredicationExitCounts();
#endif
    std::vector<std::array<std::uint32_t, 5>> snapshots;
    const auto                                bounds = boundPatterns(snapshots);
    std::uint64_t                             cases  = 0;
    for (unsigned old = 0; old < 256; ++old)
        for (const std::uint8_t flag : { std::uint8_t{ 0 }, std::uint8_t{ 1 }, std::uint8_t{ 0x80 } })
            for (std::size_t p = 0; p < bounds.size(); ++p)
            {
                Fixture f;
                f.old          = static_cast<std::uint8_t>(old);
                f.flag         = flag;
                f.bound        = bounds[p];
                f.snapshot     = snapshots[p];
                auto reference = build(f);
                auto sut       = reference;
#if REREVVED_DEVELOPER
                const auto probe = BeginSetPredicationProbe(readerOf(reference), kG, 0, kQuietSp, kPcr, true);
                UNSCOPED_INFO("pre-swap probe armed");
                REQUIRE(probe.has_value());
#endif
                const auto    expected = original(reference, kG, 0);
                std::uint64_t result   = 0xDEADBEEF00000000ULL;
                UNSCOPED_INFO("pre-swap consumed");
                REQUIRE(ConsumeSetPredicationWith(preSwapState(), readerOf(sut), writableOf(sut), writerOf(sut), kG, 0, &result));
                UNSCOPED_INFO("all pre-swap bytes, packets, cursor and zero-extended r3 equal Original without masking");
                REQUIRE((sut == reference && result == expected.r3));
#if REREVVED_DEVELOPER
                EndSetPredicationProbe(readerOf(sut), *probe, static_cast<std::uint32_t>(result));
#endif
                ++cases;
            }
    UNSCOPED_INFO("pre-swap property census");
    REQUIRE(cases == 9216);
#if REREVVED_DEVELOPER
    const auto consume = TakeSetPredicationConsumeCounts();
    const auto pre     = TakeSetPredicationPreSwapCounts();
    const auto exit    = TakeSetPredicationExitCounts();
    UNSCOPED_INFO("pre-swap counts");
    REQUIRE((consume.consumed == cases && consume.consumedClear == cases && pre.consumed == cases));
    UNSCOPED_INFO("pre-swap exact coverage is partitioned");
    REQUIRE((exit.ok == cases && exit.preSwap.ok == cases && exit.preSwap.exact == cases && exit.preSwap.flushed == 0 && exit.preSwap.unpaired == 0));
    for (const auto n : exit.preSwap.mismatch)
    {
        UNSCOPED_INFO("pre-swap ten-field equality");
        REQUIRE(n == 0);
    }
#endif
}

void testPreSwapPreflight()
{
    const auto attempt = [](Fixture f, SetPredicationConsumeState state, std::uint32_t r4, const std::set<std::uint32_t>& blocked = {})
    {
        auto               m      = build(f);
        const auto         before = m;
        Writes             log;
        std::uint64_t      result = 0xABCDEF0100000001ULL;
        SetPredicationPlan plan;
        const auto         verdict  = SetPredicationConsumeVerdict(state, readerOf(m), writableOf(m, &blocked), kG, r4, &plan);
        const auto         consumed = ConsumeSetPredicationWith(state, readerOf(m), writableOf(m, &blocked), writerOf(m, &log), kG, r4, &result);
        UNSCOPED_INFO("verdict and replay agree");
        REQUIRE(consumed == !verdict);
        if (verdict)
        {
            UNSCOPED_INFO("every refusal is before any write or register update");
            REQUIRE((m == before && log.list.empty() && result == 0xABCDEF0100000001ULL));
        }
        else
        {
            auto       reference = before;
            const auto expected  = original(reference, kG, r4);
            UNSCOPED_INFO("boundary replay matches Original");
            REQUIRE((m == reference && result == expected.r3));
        }
        return verdict;
    };
    auto    state = preSwapState();
    Fixture f;
    for (const auto cursor : { kLimit - 8, kLimit - 4, kLimit, kLimit + 4 })
    {
        f.cursor           = cursor;
        const auto verdict = attempt(f, state, 0);
        UNSCOPED_INFO("unsigned strict pre-append flush boundary");
        REQUIRE((cursor > kLimit ? verdict == SetPredicationFallback::RingFlush : !verdict));
    }
    f.cursor = 0x80000000;
    UNSCOPED_INFO("high-bit cursor above low limit");
    REQUIRE(attempt(f, state, 0) == SetPredicationFallback::RingFlush);
    f.limit = 0xFFFFFFFF;
    UNSCOPED_INFO("high-bit cursor below high-bit limit");
    REQUIRE(!attempt(f, state, 0));
    f.cursor = 0xFFFFFFF8;
    UNSCOPED_INFO("packet address cannot wrap");
    REQUIRE(attempt(f, state, 0) == SetPredicationFallback::Unreadable);
    f = {};
    f.cursor += 1;
    UNSCOPED_INFO("unaligned packet refused");
    REQUIRE(attempt(f, state, 0) == SetPredicationFallback::Unreadable);
    f = {};
    for (const auto address : { kG + 0x30, kCursor + 4, kCursor + 8 })
    {
        UNSCOPED_INFO("each added output preflighted");
        REQUIRE(attempt(f, state, 0, { address + 3 }) == SetPredicationFallback::Unreadable);
    }
    state.preSwapEnabled = false;
    UNSCOPED_INFO("new gate off retains old refusal");
    REQUIRE(attempt(f, state, 0) == SetPredicationFallback::PreSwap);
    state                = preSwapState();
    state.trustedPreSwap = false;
    UNSCOPED_INFO("LR alone cannot authorize");
    REQUIRE(attempt(f, state, 0) == SetPredicationFallback::PreSwap);
    state = preSwapState();
    UNSCOPED_INFO("only clear admitted");
    REQUIRE(attempt(f, state, 4) == SetPredicationFallback::PreSwap);
    state.stubsReady = false;
    UNSCOPED_INFO("validated stubs required");
    REQUIRE(attempt(f, state, 0) == SetPredicationFallback::Stubs);
    state             = preSwapState();
    state.diagnostics = true;
    UNSCOPED_INFO("diagnostics force Original");
    REQUIRE(attempt(f, state, 0) == SetPredicationFallback::Diagnostics);
    state    = preSwapState();
    state.sp = kQuietSp - 0x1000 + 95;
    UNSCOPED_INFO("whole 96-byte frame required");
    REQUIRE(attempt(f, state, 0) == SetPredicationFallback::StackOverlap);
    state    = preSwapState();
    f.cursor = kQuietSp - 8;
    f.limit  = 0xFFFFFFFF;
    UNSCOPED_INFO("packet touches stack reach");
    REQUIRE(attempt(f, state, 0) == SetPredicationFallback::StackOverlap);
    f.cursor = kG + 0x2AB0;
    UNSCOPED_INFO("packet cannot alias device inputs or outputs");
    REQUIRE(attempt(f, state, 0) == SetPredicationFallback::Unreadable);
    f.cursor = kPcr + 0x20;
    UNSCOPED_INFO("packet cannot alias KPCR");
    REQUIRE(attempt(f, state, 0) == SetPredicationFallback::Unreadable);
    f                    = {};
    state.memoryIdentity = [](std::uint32_t, std::uint32_t) -> std::optional<std::uint64_t>
    {
        return std::nullopt;
    };
    UNSCOPED_INFO("unknown backing refused");
    REQUIRE(attempt(f, state, 0) == SetPredicationFallback::Unreadable);
    state.memoryIdentity = [](std::uint32_t address, std::uint32_t) -> std::optional<std::uint64_t>
    {
        return address == kCursor + 4 ? kG + 0x31A4 : std::uint64_t{ address };
    };
    UNSCOPED_INFO("physical packet/device alias refused");
    REQUIRE(attempt(f, state, 0) == SetPredicationFallback::Unreadable);
    state.memoryIdentity = [](std::uint32_t address, std::uint32_t) -> std::optional<std::uint64_t>
    {
        return address == kCursor + 4 ? kQuietSp - 16 : std::uint64_t{ address };
    };
    UNSCOPED_INFO("physical packet/stack alias refused");
    REQUIRE(attempt(f, state, 0) == SetPredicationFallback::StackOverlap);
#if REREVVED_DEVELOPER
    TakeSetPredicationConsumeCounts();
    TakeSetPredicationPreSwapCounts();
#endif
}

void testPreSwapToken()
{
    // Token bookkeeping with every flag off; the pre-swap consume defaults on.
    const bool savedPreSwap = REXCVAR_GET(native_consume_set_predication_preswap);
    REXCVAR_SET(native_consume_set_predication_preswap, false);
    PPCContext ctx{};
    ctx.r3.u64 = kG;
    ctx.lr     = kSetPredicationPreSwapReturn;
    UNSCOPED_INFO("no call scope");
    REQUIRE(!SetPredicationCallScope::TrustedPreSwap());
    {
        SetPredicationCallScope untrusted(ctx);
        UNSCOPED_INFO("Original A4150 same-LR call untrusted");
        REQUIRE(!SetPredicationCallScope::TrustedPreSwap());
    }
    {
        SetPredicationPreSwapScope token(ctx, kG);
        {
            SetPredicationCallScope call(ctx);
            UNSCOPED_INFO("host invocation trusted");
            REQUIRE(SetPredicationCallScope::TrustedPreSwap());
            {
                SetPredicationCallScope nested(ctx);
                UNSCOPED_INFO("nested CD20 cannot reuse token");
                REQUIRE(!SetPredicationCallScope::TrustedPreSwap());
            }
            UNSCOPED_INFO("outer scope restored");
            REQUIRE(SetPredicationCallScope::TrustedPreSwap());
            std::thread other([&]
                              {
                                  SetPredicationCallScope callOnOtherThread(ctx);
                                  UNSCOPED_INFO("token bound to thread");
                                  REQUIRE(!SetPredicationCallScope::TrustedPreSwap());
                              });
            other.join();
        }
        SetPredicationCallScope repeated(ctx);
        UNSCOPED_INFO("token single use");
        REQUIRE(!SetPredicationCallScope::TrustedPreSwap());
    }
    {
        SetPredicationPreSwapScope token(ctx, kG + 4);
        SetPredicationCallScope    wrongDevice(ctx);
        UNSCOPED_INFO("token bound to device");
        REQUIRE(!SetPredicationCallScope::TrustedPreSwap());
    }
    {
        SetPredicationPreSwapScope token(ctx, kG);
        PPCContext                 other = ctx;
        SetPredicationCallScope    wrongContext(other);
        UNSCOPED_INFO("token bound to context");
        REQUIRE(!SetPredicationCallScope::TrustedPreSwap());
    }
    UNSCOPED_INFO("token cannot leak past scope");
    REQUIRE(!SetPredicationCallScope::TrustedPreSwap());
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("all-off token bookkeeping counts nothing");
    REQUIRE(TakeSetPredicationPreSwapCounts().entered == 0);
#endif
    REXCVAR_SET(native_consume_set_predication_preswap, savedPreSwap);
}

#if REREVVED_DEVELOPER
void testPreSwapProbePartition()
{
    TakeSetPredicationExitCounts();
    for (std::size_t word = 0; word < kSetPredicationExitWordCount; ++word)
    {
        auto       m     = build({});
        const auto entry = BeginSetPredicationProbe(readerOf(m), kG, 0, kQuietSp, kPcr, true);
        UNSCOPED_INFO("partition probe armed");
        REQUIRE(entry.has_value());
        auto                               result = static_cast<std::uint32_t>(original(m, kG, 0).r3);
        const std::array<std::uint32_t, 9> addresses{ kG + 0x31AC, kG + 0x31A4, kG + 0x31A8, kG + 0x2AB4, kG + 0x2AB8, kG + 0x2ABC, kG + 0x30, kCursor + 4, kCursor + 8 };
        if (word < addresses.size())
        {
            const auto address = addresses[word];
            m.setWord(address, m.word(address) ^ (word == 5 ? 0x01000000U : 1U));
        }
        else
            ++result;
        EndSetPredicationProbe(readerOf(m), *entry, result);
        const auto counts = TakeSetPredicationExitCounts();
        UNSCOPED_INFO("scoped mismatch cannot be exact");
        REQUIRE((counts.ok == 0 && counts.preSwap.ok == 0 && counts.preSwap.exact == 0));
        for (std::size_t i = 0; i < counts.preSwap.mismatch.size(); ++i)
        {
            UNSCOPED_INFO("each scoped mismatch attributed");
            REQUIRE(counts.preSwap.mismatch[i] == (i == word ? 1U : 0U));
        }
    }
    Fixture f;
    f.cursor   = kLimit + 4;
    auto m     = build(f);
    auto entry = BeginSetPredicationProbe(readerOf(m), kG, 0, kQuietSp, kPcr, true);
    UNSCOPED_INFO("flush probe armed");
    REQUIRE(entry.has_value());
    const auto result = original(m, kG, 0, [](Memory&, std::uint32_t)
                                 {
                                     return kCursor;
                                 });
    EndSetPredicationProbe(readerOf(m), *entry, static_cast<std::uint32_t>(result.r3));
    auto counts = TakeSetPredicationExitCounts();
    UNSCOPED_INFO("six-field flush match never counted ten-field exact");
    REQUIRE((counts.preSwap.ok == 1 && counts.preSwap.flushed == 1 && counts.preSwap.exact == 0));
    m     = build({});
    entry = BeginSetPredicationProbe(readerOf(m), kG, 0, kQuietSp, kPcr, true);
    UNSCOPED_INFO("unreadable-exit probe armed");
    REQUIRE(entry.has_value());
    m.bytes.erase(kG + 0x31A4);
    EndSetPredicationProbe(readerOf(m), *entry, kCursor + 8);
    UNSCOPED_INFO("unreadable-entry probe refused");
    REQUIRE(!BeginSetPredicationProbe(readerOf(m), kG, 0, kQuietSp, kPcr, true));
    UNSCOPED_INFO("unknown-stack probe refused");
    REQUIRE(!BeginSetPredicationProbe(readerOf(m), kG, 0, 0, 0, true));
    counts = TakeSetPredicationExitCounts();
    UNSCOPED_INFO("unknown samples never count as success");
    REQUIRE((counts.preSwap.unreadable == 2 && counts.preSwap.unmodeledStackOverlap == 1 && counts.preSwap.exact == 0 && counts.preSwap.ok == 0));
    REXCVAR_SET(native_set_predication_exit_check, true);
    const auto fields = FormatSetPredicationExitCounts(counts);
    UNSCOPED_INFO("scoped timing fields");
    REQUIRE((fields.find(" setpred_exit_preswap_exact=0 ") != std::string::npos && fields.find(" setpred_exit_preswap_unreadable=2 ") != std::string::npos));
    REXCVAR_SET(native_set_predication_exit_check, false);
}

#endif

void testHandDerivedBit()
{
    // Bit 0 for r4 == 0, derived by hand from the branches (g171:12581-12704).
    struct Case
    {
        std::uint8_t old, flag;
        bool         slotMismatch;
        std::uint8_t expected;
        const char*  what;
    };

    const Case cases[] = {
        { 0x00, 0, false, 0x00, "neither 0x10 nor 0x20: clear" },
        { 0x10, 0, false, 0x11, "0x10: set" },
        { 0x11, 0, false, 0x11, "0x10 with bit 0: stays set" },
        { 0x01, 0, false, 0x00, "bit 0 alone: cleared" },
        { 0x20, 0, false, 0x21, "0x20 with matching slots: set" },
        { 0x20, 0, true, 0x20, "0x20 with one mismatching slot: clear" },
        { 0x21, 0, true, 0x20, "0x21 with one mismatching slot: cleared" },
        { 0x30, 0, true, 0x31, "0x10 wins over a mismatch" },
        { 0x18, 0, false, 0x18, "bit 8 blocks 0x10" },
        { 0x14, 0, false, 0x14, "bit 4 blocks 0x10" },
        { 0x28, 0, false, 0x28, "bit 8 blocks 0x20" },
        { 0x24, 0, false, 0x24, "bit 4 blocks 0x20" },
        { 0x10, 1, false, 0x10, "a nonzero G+0x2F9B blocks 0x10" },
        { 0x30, 0x80, false, 0x30, "a nonzero G+0x2F9B blocks 0x20" },
        { 0x31, 1, false, 0x30, "a nonzero G+0x2F9B clears bit 0" },
        { 0xFE, 0, false, 0xFE, "all but bit 0: blocked by 8 and 4" },
        { 0xFF, 0, false, 0xFE, "all bits: blocked, bit 0 cleared" },
    };
    for (const auto& c : cases)
    {
        Fixture f;
        f.old  = c.old;
        f.flag = c.flag;
        if (c.slotMismatch)
            f.bound[3] = 0x40100304;
        auto m = build(f);
        UNSCOPED_INFO(c.what);
        REQUIRE(ConsumeSetPredicationWith(readyState(), readerOf(m), writableOf(m), writerOf(m), kG, 0));
        UNSCOPED_INFO(std::string("tiling byte: ") + c.what);
        REQUIRE(m.byte(kG + 0x2ABC) == c.expected);
        UNSCOPED_INFO(std::string("the other bytes of the word stay: ") + c.what);
        REQUIRE((m.byte(kG + 0x2ABD) == 0x12 && m.byte(kG + 0x2ABE) == 0x34 && m.byte(kG + 0x2ABF) == 0x56));
        UNSCOPED_INFO(c.what);
        REQUIRE((m.word(kG + 0x31AC) == 0 && m.word(kG + 0x31A4) == 0xFFFFFFFF && m.word(kG + 0x31A8) == 0 && m.word(kG + 0x2AB4) == 0 && m.word(kG + 0x2AB8) == 0));
    }
    // r4 != 0: the predicate is 1, the mask the argument, and bit 0 is cleared whatever the old state.
    for (const std::uint8_t old : { std::uint8_t{ 0x00 }, std::uint8_t{ 0x21 }, std::uint8_t{ 0x11 }, std::uint8_t{ 0xFF } })
    {
        Fixture f;
        f.old  = old;
        auto m = build(f);
        UNSCOPED_INFO("r4 != 0 is consumed");
        REQUIRE(ConsumeSetPredicationWith(readyState(), readerOf(m), writableOf(m), writerOf(m), kG, 0x30));
        UNSCOPED_INFO("r4 != 0 clears bit 0 only");
        REQUIRE(m.byte(kG + 0x2ABC) == (old & 0xFE));
        UNSCOPED_INFO("r4 != 0 words");
        REQUIRE((m.word(kG + 0x31AC) == 1 && m.word(kG + 0x31A4) == 0x30 && m.word(kG + 0x31A8) == 0 && m.word(kG + 0x2AB4) == 0x30 && m.word(kG + 0x2AB8) == 0));
    }
}

void testCursorBoundary()
{
#if REREVVED_DEVELOPER
    TakeSetPredicationConsumeCounts();
#endif
    const auto attempt = [](std::uint32_t cursor, std::uint32_t limit)
    {
        Fixture f;
        f.cursor             = cursor;
        f.limit              = limit;
        f.old                = 0x20;
        auto               m = build(f);
        SetPredicationPlan plan;
        return SetPredicationConsumeVerdict(readyState(), readerOf(m), writableOf(m), kG, 0, &plan);
    };
    UNSCOPED_INFO("cursor == limit is consumed");
    REQUIRE(!attempt(kLimit, kLimit));
    UNSCOPED_INFO("cursor below the limit is consumed");
    REQUIRE(!attempt(kLimit - 1, kLimit));
    UNSCOPED_INFO("cursor == limit + 1 flushes in Original");
    REQUIRE(attempt(kLimit + 1, kLimit) == SetPredicationFallback::RingFlush);
    UNSCOPED_INFO("a high-bit cursor is above the limit, unsigned");
    REQUIRE(attempt(0x80000000, kLimit) == SetPredicationFallback::RingFlush);
    UNSCOPED_INFO("a negative cursor against a positive limit flushes");
    REQUIRE(attempt(0xFFFFFFF0, 0x7FFFFFFF) == SetPredicationFallback::RingFlush);
    UNSCOPED_INFO("a positive cursor below a high-bit limit is consumed");
    REQUIRE(!attempt(0x7FFFFFFF, 0x80000000));
    UNSCOPED_INFO("the largest limit consumes");
    REQUIRE(!attempt(0x00200000, 0xFFFFFFFF));
    UNSCOPED_INFO("cursor == limit == 0 is not a flush");
    REQUIRE(attempt(0, 0) != SetPredicationFallback::RingFlush);
}

void testOrder()
{
    // Every condition fails; each step repairs the earliest reason and the next one is reported.
    Fixture f;
    f.cursor = kLimit + 8; // ring_flush
    Memory m = build(f);
    placeStack(m, kG - 0x1000, kG + 0x4000); // the thread's stack runs over the device block
    std::set<std::uint32_t> blocked{ kG + 0x31AC };
    auto                    state = readyState();
    state.stubsReady              = false;
    state.diagnostics             = true;
    state.caller                  = kSetPredicationPreSwapReturn;
    state.sp                      = kG + 0x2FA0; // stack_overlap
    Memory holes                  = m;
    holes.bytes.erase(kG + 0x3098);
    const auto verdict = [&](const SetPredicationConsumeState& s, const GuestWordReader& r, const SetPredicationWritable& w)
    {
        return SetPredicationConsumeVerdict(s, r, w, kG, 0);
    };
    UNSCOPED_INFO("stubs first");
    REQUIRE(verdict(state, readerOf(holes), writableOf(m, &blocked)) == SetPredicationFallback::Stubs);
    state.stubsReady = true;
    UNSCOPED_INFO("diagnostics second");
    REQUIRE(verdict(state, readerOf(holes), writableOf(m, &blocked)) == SetPredicationFallback::Diagnostics);
    state.diagnostics = false;
    UNSCOPED_INFO("the pre-swap call third");
    REQUIRE(verdict(state, readerOf(holes), writableOf(m, &blocked)) == SetPredicationFallback::PreSwap);
    state.caller = 0x82500000;
    UNSCOPED_INFO("a stack over the device fourth, before the unreadable word");
    REQUIRE(verdict(state, readerOf(holes), writableOf(m, &blocked)) == SetPredicationFallback::StackOverlap);
    for (const std::uint32_t offset : { 0x30U, 0x38U, 0x2ABCU, 0x31ACU })
    {
        Memory masked = m;
        for (std::uint32_t i = 0; i < 4; ++i)
            masked.bytes.erase(kG + offset + i);
        UNSCOPED_INFO("an unreadable device word cannot mask the stack");
        REQUIRE(verdict(state, readerOf(masked), writableOf(m, &blocked)) == SetPredicationFallback::StackOverlap);
    }
    placeStack(m, kQuietSp - 0x1000, kQuietSp + 0x1000);
    placeStack(holes, kQuietSp - 0x1000, kQuietSp + 0x1000);
    state.sp = kQuietSp;
    UNSCOPED_INFO("an unreadable word fifth");
    REQUIRE(verdict(state, readerOf(holes), writableOf(m, &blocked)) == SetPredicationFallback::Unreadable);
    UNSCOPED_INFO("a store target that is not writable is unreadable, before the flush");
    REQUIRE(verdict(state, readerOf(m), writableOf(m, &blocked)) == SetPredicationFallback::Unreadable);
    UNSCOPED_INFO("the flush last");
    REQUIRE(verdict(state, readerOf(m), writableOf(m)) == SetPredicationFallback::RingFlush);
    state.sp = kG + 0x2FA0;
    placeStack(m, kG - 0x1000, kG + 0x4000);
    UNSCOPED_INFO("a stack over the device comes before the flush");
    REQUIRE(verdict(state, readerOf(m), writableOf(m)) == SetPredicationFallback::StackOverlap);
    state.sp = kQuietSp;
    f.cursor = kLimit;
    m        = build(f);
    UNSCOPED_INFO("all conditions repaired: consumed");
    REQUIRE(!verdict(state, readerOf(m), writableOf(m)));
    // The same call with another caller than the pre-swap stub's is consumed; the stub's own is not.
    state.caller = kSetPredicationPreSwapReturn;
    UNSCOPED_INFO("the pre-swap call keeps Original");
    REQUIRE(verdict(state, readerOf(m), writableOf(m)) == SetPredicationFallback::PreSwap);
    state.caller = kSetPredicationPreSwapReturn + 4;
    UNSCOPED_INFO("any other caller is consumed");
    REQUIRE(!verdict(state, readerOf(m), writableOf(m)));
}

// The addresses the prologue stores to, found by running the reference on an empty stack.
void testPrologueStores()
{
    Fixture      f;
    const Memory entry = build(f);
    Memory       m     = entry;
    original(m, kG, 5, {}, kQuietSp, 0x8250AD58);
    std::set<std::int64_t> touched;
    for (const auto& [address, value] : m.bytes)
        if (!entry.bytes.contains(address) || entry.bytes.at(address) != value)
            if (address >= kQuietSp - 256 && address < kQuietSp + 256)
                touched.insert(static_cast<std::int64_t>(address) - kQuietSp);
    std::set<std::int64_t> expected;
    for (std::int64_t i = -96; i < -92; ++i)
        expected.insert(i);
    for (std::int64_t i = -16; i < -4; ++i)
        expected.insert(i);
    UNSCOPED_INFO("the prologue stores exactly sp-96..sp-93 and sp-16..sp-5");
    REQUIRE(touched == expected);
    UNSCOPED_INFO("the lowest stored byte is sp-96 and the highest sp-5");
    REQUIRE((*touched.begin() == -96 && *touched.rbegin() == -5));
    UNSCOPED_INFO("the link register, the old r1 and r31 land where the generated function puts them");
    REQUIRE((m.word(kQuietSp - 8) == 0x8250AD58 && m.word(kQuietSp - 96) == kQuietSp && m.word(kQuietSp - 16) == 0x11223344 && m.word(kQuietSp - 12) == 0x55667788));
}

// The default fixture with the thread's stack [limit, sp + 0x1000) and r1 = `sp`.
std::optional<SetPredicationFallback> verdictAtStack(std::uint32_t limit, std::uint32_t sp, std::uint32_t r4 = 0)
{
    Fixture f;
    f.old  = 0x20;
    auto m = build(f);
    placeStack(m, limit, sp + 0x1000);
    auto state = readyState();
    state.sp   = sp;
    return SetPredicationConsumeVerdict(state, readerOf(m), writableOf(m), kG, r4);
}

void testStackOverlapEdges()
{
    UNSCOPED_INFO("the footprint: nothing above r1, the device up to G+0x31CC");
    REQUIRE((kSetPredicationStackFootprint.above == 0 && kSetPredicationStackFootprint.deviceSpan == 0x31CC && kSetPredicationDeviceSpan == 0x31CC));
    // The review's case: the link register save lands on the flag byte G+0x2F9B before Original reads it.
    {
        Fixture f;
        f.old                    = 0x20; // slots all bound and equal to their snapshots, flag 0
        Memory         entry     = build(f);
        constexpr auto sp        = kG + 0x2FA0;
        constexpr auto lr        = 0x8250AD58;
        Memory         reference = entry;
        original(reference, kG, 0, {}, sp, lr);
        UNSCOPED_INFO("Original: the saved link register hides bit 0, the tiling stays 0x20");
        REQUIRE((reference.byte(kG + 0x2F9B) == (lr & 0xFF) && reference.byte(kG + 0x2ABC) == 0x20));

        auto state   = readyState();
        state.sp     = sp;
        state.caller = lr;
        placeStack(entry, sp - 0x1000, sp + 0x1000);
        Memory sut = entry;
        UNSCOPED_INFO("the stack over G+0x2F9B is refused");
        REQUIRE(SetPredicationConsumeVerdict(state, readerOf(sut), writableOf(sut), kG, 0) == SetPredicationFallback::StackOverlap);
        UNSCOPED_INFO("Original runs and nothing is stored");
        REQUIRE((!ConsumeSetPredicationWith(state, readerOf(sut), writableOf(sut), writerOf(sut), kG, 0) && sut == entry));
        // On a quiet stack the same device state is consumed, with bit 0 set: the case the refusal protects.
        placeStack(sut, kQuietSp - 0x1000, kQuietSp + 0x1000);
        state.sp = kQuietSp;
        UNSCOPED_INFO("a quiet stack is consumed, with bit 0 set");
        REQUIRE((ConsumeSetPredicationWith(state, readerOf(sut), writableOf(sut), writerOf(sut), kG, 0) && sut.byte(kG + 0x2ABC) == 0x21));
    }

    // The device words [G, G + 0x31CC) against the half-open reach [limit, sp), at the pinned edges.
    constexpr std::uint32_t kSpan = kSetPredicationDeviceSpan, kEnd = kG + kSpan;
    UNSCOPED_INFO("a reach ending exactly at G is consumed");
    REQUIRE(!verdictAtStack(kG - 0x1000, kG));
    UNSCOPED_INFO("a reach ending one byte into the device is refused");
    REQUIRE(verdictAtStack(kG - 0x1000, kG + 1) == SetPredicationFallback::StackOverlap);
    UNSCOPED_INFO("a reach starting exactly at G + 0x31CC is consumed");
    REQUIRE(!verdictAtStack(kEnd, kEnd + 0x1000));
    UNSCOPED_INFO("a reach starting on G+0x31CB is refused");
    REQUIRE(verdictAtStack(kEnd - 1, kEnd + 0x1000) == SetPredicationFallback::StackOverlap);
    UNSCOPED_INFO("the words between G+0x31CC and the old 0x3500 block are not in the footprint");
    REQUIRE(!verdictAtStack(kG + 0x3200, kG + 0x3500));
    UNSCOPED_INFO("a reach inside or over the device is refused");
    REQUIRE((verdictAtStack(kG + 0x2F00, kG + 0x3000) == SetPredicationFallback::StackOverlap && verdictAtStack(kG - 0x1000, kEnd + 0x1000) == SetPredicationFallback::StackOverlap));
    UNSCOPED_INFO("far stacks are consumed");
    REQUIRE((!verdictAtStack(kQuietSp - 0x1000, kQuietSp) && !verdictAtStack(kG - 0x4000, kG - 0x3000)));
    // Original's own stores cover sp-96 .. sp-5, all inside the reach: the flag byte G+0x2F9B for sp from G+0x2FA0 to G+0x2FAB
    // and the old-r1 word for sp from G+0x2FF0 to G+0x2FFB are refused whatever the limit is below them.
    for (const std::uint32_t sp : { kG + 0x2FA0, kG + 0x2FAB, kG + 0x2FF0, kG + 0x2FFB })
    {
        UNSCOPED_INFO("the stores' reach over the flag byte is refused");
        REQUIRE(verdictAtStack(sp - 96, sp) == SetPredicationFallback::StackOverlap);
    }
    // The no-overflow domain: the bytes between the limit and r1 must hold the entry function's own frame (stwu
    // r1,-96(r1), g171:12569). Fewer is refused, wherever the stack lies; r1 at the limit is the case the saves at
    // r1-8 and r1-16 (g171:12565-12572) would store below the limit in, over the snapshot words.
    static_assert(kSetPredicationStackFootprint.frame == 96);
    UNSCOPED_INFO("exactly the frame between the limit and r1 is consumed");
    REQUIRE(!verdictAtStack(kQuietSp - 96, kQuietSp));
    UNSCOPED_INFO("one byte short of the frame is refused");
    REQUIRE(verdictAtStack(kQuietSp - 95, kQuietSp) == SetPredicationFallback::StackOverlap);
    UNSCOPED_INFO("r1 at the limit is refused");
    REQUIRE(verdictAtStack(kQuietSp, kQuietSp) == SetPredicationFallback::StackOverlap);
    UNSCOPED_INFO("an empty reach inside the device is refused");
    REQUIRE((verdictAtStack(kG, kG) == SetPredicationFallback::StackOverlap && verdictAtStack(kG + 0x100, kG + 0x100) == SetPredicationFallback::StackOverlap));

    // Unknown bounds are refused like an overlap.
    {
        Fixture f;
        f.old              = 0x20;
        const auto unknown = [&](const std::function<void(Memory&, SetPredicationConsumeState&)>& edit)
        {
            auto m     = build(f);
            auto state = readyState();
            edit(m, state);
            return SetPredicationConsumeVerdict(state, readerOf(m), writableOf(m), kG, 0);
        };
        const auto stackOverlap = std::optional<SetPredicationFallback>{ SetPredicationFallback::StackOverlap };
        UNSCOPED_INFO("r13 zero");
        REQUIRE(unknown([](Memory&, SetPredicationConsumeState& s)
                        {
                            s.pcr = 0;
                        }) == stackOverlap);
        UNSCOPED_INFO("an unreadable limit");
        REQUIRE(unknown([](Memory& m, SetPredicationConsumeState&)
                        {
                            for (std::uint32_t i = 0; i < 4; ++i)
                                m.bytes.erase(kPcr + 0x74 + i);
                        }) == stackOverlap);
        UNSCOPED_INFO("an unreadable base");
        REQUIRE(unknown([](Memory& m, SetPredicationConsumeState&)
                        {
                            for (std::uint32_t i = 0; i < 4; ++i)
                                m.bytes.erase(kPcr + 0x70 + i);
                        }) == stackOverlap);
        UNSCOPED_INFO("a zero limit");
        REQUIRE(unknown([](Memory& m, SetPredicationConsumeState&)
                        {
                            placeStack(m, 0, kQuietSp + 0x1000);
                        }) == stackOverlap);
        UNSCOPED_INFO("an inverted range");
        REQUIRE(unknown([](Memory& m, SetPredicationConsumeState&)
                        {
                            placeStack(m, kQuietSp + 0x1000, kQuietSp - 0x1000);
                        }) == stackOverlap);
        UNSCOPED_INFO("r1 below the limit");
        REQUIRE(unknown([](Memory&, SetPredicationConsumeState& s)
                        {
                            s.sp = kQuietSp - 0x1004;
                        }) == stackOverlap);
        UNSCOPED_INFO("r1 above the base");
        REQUIRE(unknown([](Memory&, SetPredicationConsumeState& s)
                        {
                            s.sp = kQuietSp + 0x1004;
                        }) == stackOverlap);
        UNSCOPED_INFO("a KPCR in the stack");
        REQUIRE(unknown([](Memory& m, SetPredicationConsumeState& s)
                        {
                            s.pcr = kQuietSp - 0x40;
                            m.setWord(kQuietSp - 0x40 + 0x70, kQuietSp + 0x1000);
                            m.setWord(kQuietSp - 0x40 + 0x74, kQuietSp - 0x1000);
                        }) == stackOverlap);
        UNSCOPED_INFO("the fixture's stack is known");
        REQUIRE(!unknown([](Memory&, SetPredicationConsumeState&)
                         {
                         }));
    }

    // The ring words the probe reads, [cursor+4, cursor+12): the consume does not read them and is not refused for a
    // stack over them, the probe is not armed for a call without a flush.
    {
        Fixture f;
        f.old = 0x20;
#if REREVVED_DEVELOPER
        TakeSetPredicationExitCounts();
        const auto probe = [&](std::uint32_t limit, std::uint32_t sp, const Fixture& fixture)
        {
            auto m = build(fixture);
            placeStack(m, limit, sp + 0x1000);
            return BeginSetPredicationProbe(readerOf(m), kG, 0, sp, kPcr).has_value();
        };
        UNSCOPED_INFO("a reach ending at the first ring word arms");
        REQUIRE(probe(kCursor - 0x1000, kCursor + 4, f));
        UNSCOPED_INFO("a reach one byte into the ring words is not armed");
        REQUIRE(!probe(kCursor - 0x1000, kCursor + 5, f));
        UNSCOPED_INFO("a reach starting after the ring words arms");
        REQUIRE(probe(kCursor + 12, kCursor + 12 + 0x100, f));
        UNSCOPED_INFO("a reach starting on the last ring byte is not armed");
        REQUIRE(!probe(kCursor + 11, kCursor + 11 + 0x100, f));
        Fixture flush = f;
        flush.cursor  = kLimit + 16;
        UNSCOPED_INFO("a flush call does not store the ring words, so they are not tested");
        REQUIRE(probe(flush.cursor, flush.cursor + 0x100, flush));
#endif
        auto m = build(f);
        placeStack(m, kCursor, kCursor + 0x1000);
        auto state = readyState();
        state.sp   = kCursor + 0x100;
        UNSCOPED_INFO("the consume reads no ring word, so a stack over them is consumed");
        REQUIRE(!SetPredicationConsumeVerdict(state, readerOf(m), writableOf(m), kG, 0));
#if REREVVED_DEVELOPER
        const auto counts = TakeSetPredicationExitCounts();
        UNSCOPED_INFO("the refused probes are counted as unmodeled");
        REQUIRE((counts.unmodeledStackOverlap == 2 && counts.ok == 0));
#endif
    }

    // A device at the top of the address space cannot be placed: refused.
    {
        const auto m = build(Fixture{});
        StackReach reach;
        UNSCOPED_INFO("a device ending exactly at 2^32 is placed");
        REQUIRE(!SetPredicationStackOverlaps(readerOf(m), { kPcr, kQuietSp }, 0xFFFFFFFFU - kSpan + 1, reach));
        UNSCOPED_INFO("a device running past 2^32 is refused");
        REQUIRE(SetPredicationStackOverlaps(readerOf(m), { kPcr, kQuietSp }, 0xFFFFFFFFU - kSpan + 2, reach));
    }
}

// Whatever frame the verdict admits, the consume leaves the device block as Original (with its prologue) does.
void testAdmittedFramesMatchOriginal()
{
#if REREVVED_DEVELOPER
    TakeSetPredicationConsumeCounts();
#endif
    std::uint64_t admitted = 0, refused = 0;
    for (const std::uint8_t flag : { std::uint8_t{ 0 }, std::uint8_t{ 1 } })
    {
        for (const std::uint8_t old : { std::uint8_t{ 0x20 }, std::uint8_t{ 0x30 }, std::uint8_t{ 0x00 } })
        {
            Fixture f;
            f.old              = old;
            f.flag             = flag;
            const Memory entry = build(f);
            for (std::int64_t delta = -130; delta < static_cast<std::int64_t>(kSetPredicationDeviceSpan) + 130; ++delta)
            {
                const auto sp = static_cast<std::uint32_t>(kG + delta);
                for (const std::uint32_t r4 : { 0U, 3U })
                {
                    auto state   = readyState();
                    state.sp     = sp;
                    state.caller = 0x8250AD58;
                    Memory sut   = entry;
                    // The thread's stack begins at the lowest byte Original stores, sp-96.
                    placeStack(sut, sp - 96, sp + 0x1000);
                    const bool aliases = static_cast<std::int64_t>(sp) > kG && static_cast<std::int64_t>(sp) - 96 < static_cast<std::int64_t>(kG) + kSetPredicationDeviceSpan;
                    if (!ConsumeSetPredicationWith(state, readerOf(sut), writableOf(sut), writerOf(sut), kG, r4))
                    {
                        ++refused;
                        UNSCOPED_INFO("only an overlapping reach is refused here");
                        REQUIRE(aliases);
                        continue;
                    }
                    UNSCOPED_INFO("an overlapping reach is never consumed");
                    REQUIRE(!aliases);
                    ++admitted;
                    Memory reference = entry;
                    original(reference, kG, r4, {}, sp, 0x8250AD58);
                    for (std::uint32_t offset = 0; offset < kSetPredicationDeviceSpan; ++offset)
                    {
                        if (offset >= 0x30 && offset < 0x34)
                            continue; // the cursor, which the consume leaves
                        const auto address = kG + offset;
                        if (!reference.bytes.contains(address))
                            continue;
                        UNSCOPED_INFO("device byte G+" + std::to_string(offset) + " differs at sp = G + " + std::to_string(delta) + " old " + std::to_string(old) + " flag " + std::to_string(flag));
                        REQUIRE((sut.bytes.contains(address) && sut.bytes.at(address) == reference.bytes.at(address)));
                    }
                }
            }
        }
    }
    UNSCOPED_INFO("the sweep admits and refuses frames");
    REQUIRE((admitted > 0 && refused > 0));
#if REREVVED_DEVELOPER
    TakeSetPredicationConsumeCounts();
#endif
}

void testUnreadable()
{
    const auto state = readyState();
    // Each watched word, removed one at a time, makes the call unreadable.
    const std::vector<std::uint32_t> watched{ 0x30, 0x38, 0x2ABC, 0x2F98, 0x31AC, 0x31A4, 0x31A8, 0x2AB4, 0x2AB8, 0x3098, 0x309C, 0x30A0, 0x30A4, 0x30A8, 0x31B8, 0x31BC, 0x31C0, 0x31C4, 0x31C8 };
    for (const auto offset : watched)
    {
        Fixture f;
        auto    m = build(f);
        m.bytes.erase(kG + offset + 2); // a partially mapped word
        UNSCOPED_INFO("a missing word is unreadable");
        REQUIRE(SetPredicationConsumeVerdict(state, readerOf(m), writableOf(m), kG, 0) == SetPredicationFallback::Unreadable);
    }
    // A device base too high for the offsets is unreadable without a read.
    Fixture    f;
    auto       m        = build(f);
    bool       reads    = false;
    const auto counting = [&](std::uint32_t, std::uint32_t&)
    {
        reads = true;
        return true;
    };
    UNSCOPED_INFO("a device base too high is unreadable");
    REQUIRE((SetPredicationConsumeVerdict(state, counting, writableOf(m), 0xFFFFFFFF - kSetPredicationDeviceSpan + 1, 0) == SetPredicationFallback::Unreadable && !reads));
    reads = false;
    SetPredicationConsumeVerdict(state, counting, [](std::uint32_t, std::uint32_t)
                                 {
                                     return true;
                                 },
                                 0xFFFFFFFF - kSetPredicationDeviceSpan,
                                 0);
    UNSCOPED_INFO("the highest device base that fits is read");
    REQUIRE(reads);
    // Each store target, blocked alone, is unreadable; an unchanged word has no store to block.
    for (const auto offset : { 0x31AC, 0x31A4, 0x31A8, 0x2AB4, 0x2AB8 })
    {
        const std::set<std::uint32_t> blocked{ kG + static_cast<std::uint32_t>(offset) + 3 };
        Fixture                       fixture;
        fixture.old        = 0x10;
        auto       mem     = build(fixture);
        const auto verdict = SetPredicationConsumeVerdict(state, readerOf(mem), writableOf(mem, &blocked), kG, 5);
        UNSCOPED_INFO("a word store target that is not writable is unreadable");
        REQUIRE(verdict == SetPredicationFallback::Unreadable);
    }
    {
        const std::set<std::uint32_t> blocked{ kG + 0x2ABC };
        Fixture                       fixture;
        fixture.old = 0x11;
        auto mem    = build(fixture);
        UNSCOPED_INFO("the byte target that is not writable is unreadable");
        REQUIRE(SetPredicationConsumeVerdict(state, readerOf(mem), writableOf(mem, &blocked), kG, 5) == SetPredicationFallback::Unreadable);
        // Bit 0 already clear: r4 != 0 leaves the byte, so a blocked byte is no obstacle.
        fixture.old = 0x10;
        mem         = build(fixture);
        UNSCOPED_INFO("an unchanged byte is not stored, so it need not be writable");
        REQUIRE(!SetPredicationConsumeVerdict(state, readerOf(mem), writableOf(mem, &blocked), kG, 5));
    }
}

void testUnchangedWordsAreNotWritten()
{
    // The device already holds the exit of SetPredication(0): nothing is stored.
    Fixture f;
    f.old       = 0x20;
    f.predicate = 0;
    f.mask      = 0xFFFFFFFF;
    f.word31a8  = 0;
    f.word2ab4  = 0;
    f.word2ab8  = 0;
    auto m      = build(f);
    m.setWord(kG + 0x2ABC, 0x21U << 24 | kTilingMarks);
    Writes log;
    UNSCOPED_INFO("consumed");
    REQUIRE(ConsumeSetPredicationWith(readyState(), readerOf(m), writableOf(m), writerOf(m, &log), kG, 0));
    UNSCOPED_INFO("a device that already holds the exit stores nothing");
    REQUIRE(log.list.empty());

    // One differing word is the only store.
    m.setWord(kG + 0x2AB4, 9);
    log = {};
    UNSCOPED_INFO("consumed");
    REQUIRE(ConsumeSetPredicationWith(readyState(), readerOf(m), writableOf(m), writerOf(m, &log), kG, 0));
    UNSCOPED_INFO("only the differing word is stored");
    REQUIRE((log.list.size() == 1 && log.list[0] == SetPredicationWrite{ kG + 0x2AB4, 0, false }));
    UNSCOPED_INFO("the word took the exit value");
    REQUIRE(m.word(kG + 0x2AB4) == 0);

    // The stores of a full change, in the order Original makes them, and the byte store is one byte.
    Fixture g;
    g.old  = 0x10;
    auto n = build(g);
    log    = {};
    UNSCOPED_INFO("consumed");
    REQUIRE(ConsumeSetPredicationWith(readyState(), readerOf(n), writableOf(n), writerOf(n, &log), kG, 0));
    const std::vector<SetPredicationWrite> expected{
        { kG + 0x31AC, 0, false },
        { kG + 0x31A4, 0xFFFFFFFF, false },
        { kG + 0x2ABC, 0x11, true },
        { kG + 0x31A8, 0, false },
        { kG + 0x2AB4, 0, false },
        { kG + 0x2AB8, 0, false },
    };
    UNSCOPED_INFO("the six stores in guest order");
    REQUIRE(log.list == expected);
    // Everything else in the map is as it was: the byte store touched one byte.
    Memory untouched = build(g);
    for (const auto& w : expected)
    {
        if (w.byte)
            untouched.setByte(w.address, static_cast<std::uint8_t>(w.value));
        else
            untouched.setWord(w.address, w.value);
    }
    UNSCOPED_INFO("no other byte changed");
    REQUIRE(untouched == n);
}

#if REREVVED_DEVELOPER
void testCounts()
{
    TakeSetPredicationConsumeCounts();
    Fixture f;
    f.old  = 0x10;
    auto m = build(f);
    UNSCOPED_INFO("clear consumed");
    REQUIRE(ConsumeSetPredicationWith(readyState(), readerOf(m), writableOf(m), writerOf(m), kG, 0));
    UNSCOPED_INFO("mask consumed");
    REQUIRE(ConsumeSetPredicationWith(readyState(), readerOf(m), writableOf(m), writerOf(m), kG, 3));
    auto stubs       = readyState();
    stubs.stubsReady = false;
    UNSCOPED_INFO("stubs");
    REQUIRE(!ConsumeSetPredicationWith(stubs, readerOf(m), writableOf(m), writerOf(m), kG, 0));
    auto trace        = readyState();
    trace.diagnostics = true;
    UNSCOPED_INFO("diagnostics");
    REQUIRE(!ConsumeSetPredicationWith(trace, readerOf(m), writableOf(m), writerOf(m), kG, 0));
    UNSCOPED_INFO("diagnostics again");
    REQUIRE(!ConsumeSetPredicationWith(trace, readerOf(m), writableOf(m), writerOf(m), kG, 0));
    auto pre   = readyState();
    pre.caller = kSetPredicationPreSwapReturn;
    UNSCOPED_INFO("pre-swap");
    REQUIRE(!ConsumeSetPredicationWith(pre, readerOf(m), writableOf(m), writerOf(m), kG, 0));
    auto overlap = readyState();
    overlap.sp   = kG + 0x2FA0;
    Memory over  = m;
    placeStack(over, kG - 0x1000, kG + 0x4000);
    Memory beforeOverlap = over;
    UNSCOPED_INFO("stack_overlap");
    REQUIRE(!ConsumeSetPredicationWith(overlap, readerOf(over), writableOf(over), writerOf(over), kG, 0));
    UNSCOPED_INFO("stack_overlap with a mask");
    REQUIRE(!ConsumeSetPredicationWith(overlap, readerOf(over), writableOf(over), writerOf(over), kG, 5));
    UNSCOPED_INFO("a stack_overlap call stores nothing");
    REQUIRE(beforeOverlap == over);
    Memory hole = m;
    hole.bytes.erase(kG + 0x38);
    UNSCOPED_INFO("unreadable");
    REQUIRE(!ConsumeSetPredicationWith(readyState(), readerOf(hole), writableOf(hole), writerOf(hole), kG, 0));
    Fixture overFixture;
    overFixture.cursor = kLimit + 1;
    auto high          = build(overFixture);
    UNSCOPED_INFO("ring_flush");
    REQUIRE(!ConsumeSetPredicationWith(readyState(), readerOf(high), writableOf(high), writerOf(high), kG, 0));
    Memory untouchedByRefusals = high;
    UNSCOPED_INFO("ring_flush with a mask");
    REQUIRE(!ConsumeSetPredicationWith(readyState(), readerOf(high), writableOf(high), writerOf(high), kG, 7));
    UNSCOPED_INFO("a refused call stores nothing");
    REQUIRE(untouchedByRefusals == high);

    const auto counts = TakeSetPredicationConsumeCounts();
    UNSCOPED_INFO("consumed counts");
    REQUIRE((counts.consumed == 2 && counts.consumedClear == 1));
    UNSCOPED_INFO("fallback counts");
    REQUIRE((counts.fallback[static_cast<std::size_t>(SetPredicationFallback::Stubs)] == 1 && counts.fallback[static_cast<std::size_t>(SetPredicationFallback::Diagnostics)] == 2 &&
             counts.fallback[static_cast<std::size_t>(SetPredicationFallback::PreSwap)] == 1 && counts.fallback[static_cast<std::size_t>(SetPredicationFallback::StackOverlap)] == 2 &&
             counts.fallback[static_cast<std::size_t>(SetPredicationFallback::Unreadable)] == 1 && counts.fallback[static_cast<std::size_t>(SetPredicationFallback::RingFlush)] == 2));
    UNSCOPED_INFO("consume line");
    REQUIRE(FormatSetPredicationConsumeCounts(counts) ==
            " setpred_consume_ok=2 setpred_consume_ok_clear=1 setpred_consume_fallback_stubs=1 setpred_consume_fallback_diagnostics=2 setpred_consume_fallback_preswap=1"
            " setpred_consume_fallback_stack_overlap=2 setpred_consume_fallback_unreadable=1 setpred_consume_fallback_ring_flush=2");
    UNSCOPED_INFO("an empty line lists the two ok counters");
    REQUIRE(FormatSetPredicationConsumeCounts({}) == " setpred_consume_ok=0 setpred_consume_ok_clear=0");
    const auto again = TakeSetPredicationConsumeCounts();
    UNSCOPED_INFO("a take resets the counts");
    REQUIRE((again.consumed == 0 && again.consumedClear == 0));
}

// A deviating Original: runs the transcription and then changes one word.
struct Deviation
{
    const char*                                  word;
    std::function<void(Memory&, std::uint32_t&)> apply; // memory after Original, r3
};

void testProbe()
{
    TakeSetPredicationExitCounts();
    Fixture f;
    f.old                              = 0x20;
    const Memory                 entry = build(f);
    const std::vector<Deviation> deviations{
        { "predicate", [](Memory& m, std::uint32_t&)
          {
              m.setWord(kG + 0x31AC, 1);
          } },
        { "mask", [](Memory& m, std::uint32_t&)
          {
              m.setWord(kG + 0x31A4, 0xFFFF0000);
          } },
        { "word31a8", [](Memory& m, std::uint32_t&)
          {
              m.setWord(kG + 0x31A8, 4);
          } },
        { "word2ab4", [](Memory& m, std::uint32_t&)
          {
              m.setWord(kG + 0x2AB4, 4);
          } },
        { "word2ab8", [](Memory& m, std::uint32_t&)
          {
              m.setWord(kG + 0x2AB8, 4);
          } },
        { "tiling", [](Memory& m, std::uint32_t&)
          {
              m.setByte(kG + 0x2ABC, 0x20);
          } },
        { "cursor", [](Memory& m, std::uint32_t&)
          {
              m.setWord(kG + 0x30, kCursor + 4);
          } },
        { "ring_header", [](Memory& m, std::uint32_t&)
          {
              m.setWord(kCursor + 4, 0xC0006001);
          } },
        { "ring_mask", [](Memory& m, std::uint32_t&)
          {
              m.setWord(kCursor + 8, 0);
          } },
        { "result", [](Memory&, std::uint32_t& r3)
          {
              r3 += 4;
          } },
    };
    UNSCOPED_INFO("one deviation per compared word");
    REQUIRE(deviations.size() == kSetPredicationExitWordCount);
    for (std::size_t i = 0; i < deviations.size(); ++i)
    {
        UNSCOPED_INFO("the deviation order follows the word names");
        REQUIRE(deviations[i].word == kSetPredicationExitWordNames[i]);
        Memory     reference = entry;
        const auto armed     = BeginSetPredicationProbe(readerOf(reference), kG, 0, kQuietSp, kPcr);
        UNSCOPED_INFO("armed");
        REQUIRE((armed.has_value() && !armed->predicted.flush));
        auto r3 = static_cast<std::uint32_t>(original(reference, kG, 0).r3);
        deviations[i].apply(reference, r3);
        EndSetPredicationProbe(readerOf(reference), *armed, r3);
        const auto counts = TakeSetPredicationExitCounts();
        UNSCOPED_INFO(std::string("a deviating ") + deviations[i].word + " is not ok");
        REQUIRE((counts.ok == 0 && counts.flushed == 0 && counts.unreadable == 0));
        for (std::size_t k = 0; k < counts.mismatch.size(); ++k)
        {
            UNSCOPED_INFO(std::string("only ") + deviations[i].word + " is flagged, not " + std::string(kSetPredicationExitWordNames[k]));
            REQUIRE(counts.mismatch[k] == (k == i ? 1U : 0U));
        }
    }

    // An exact Original is ok.
    {
        Memory     reference = entry;
        const auto armed     = BeginSetPredicationProbe(readerOf(reference), kG, 0, kQuietSp, kPcr);
        const auto r3        = static_cast<std::uint32_t>(original(reference, kG, 0).r3);
        EndSetPredicationProbe(readerOf(reference), *armed, r3);
        const auto counts = TakeSetPredicationExitCounts();
        UNSCOPED_INFO("an exact Original is ok");
        REQUIRE((counts.ok == 1 && counts.flushed == 0 && counts.unreadable == 0));
        for (const auto n : counts.mismatch)
        {
            UNSCOPED_INFO("no mismatch");
            REQUIRE(n == 0);
        }
    }

    // A flush: the cursor and the ring words are not compared, the exit words still are.
    Fixture over;
    over.old                  = 0x20;
    over.cursor               = kLimit + 16;
    const Memory flushedEntry = build(over);
    {
        Memory     reference = flushedEntry;
        const auto armed     = BeginSetPredicationProbe(readerOf(reference), kG, 0, kQuietSp, kPcr);
        UNSCOPED_INFO("the model predicts the flush");
        REQUIRE((armed && armed->predicted.flush));
        // The flush moves the cursor into another segment.
        const auto result = original(reference, kG, 0, [](Memory& m, std::uint32_t)
                                     {
                                         m.setWord(0x00300000 + 4, 0);
                                         m.setWord(0x00300000 + 8, 0);
                                         m.setWord(kG + 0x30, 0x00300000);
                                         return 0x00300000U;
                                     });
        UNSCOPED_INFO("the transcription flushed");
        REQUIRE(result.flushed);
        EndSetPredicationProbe(readerOf(reference), *armed, static_cast<std::uint32_t>(result.r3));
        const auto counts = TakeSetPredicationExitCounts();
        UNSCOPED_INFO("a flushed call is counted flushed and its exit words are ok");
        REQUIRE((counts.ok == 1 && counts.flushed == 1 && counts.unreadable == 0));
        for (const auto n : counts.mismatch)
        {
            UNSCOPED_INFO("no mismatch after a flush");
            REQUIRE(n == 0);
        }
    }
    {
        Memory     reference = flushedEntry;
        const auto armed     = BeginSetPredicationProbe(readerOf(reference), kG, 0, kQuietSp, kPcr);
        original(reference, kG, 0, [](Memory& m, std::uint32_t)
                 {
                     m.setWord(0x00300000 + 4, 0);
                     m.setWord(0x00300000 + 8, 0);
                     return 0x00300000U;
                 });
        reference.setWord(kG + 0x31AC, 3);
        EndSetPredicationProbe(readerOf(reference), *armed, 0x00300008);
        const auto counts = TakeSetPredicationExitCounts();
        UNSCOPED_INFO("a flushed call still compares the exit words");
        REQUIRE((counts.ok == 0 && counts.flushed == 1 && counts.mismatch[0] == 1));
    }

    // Unreadable: at the entry, and at the exit, including the ring words.
    {
        Memory broken = entry;
        broken.bytes.erase(kG + 0x31C8);
        UNSCOPED_INFO("an unreadable entry arms nothing");
        REQUIRE(!BeginSetPredicationProbe(readerOf(broken), kG, 0, kQuietSp, kPcr).has_value());
        auto counts = TakeSetPredicationExitCounts();
        UNSCOPED_INFO("an unreadable entry is counted");
        REQUIRE((counts.unreadable == 1 && counts.ok == 0));

        Memory     reference = entry;
        const auto armed     = BeginSetPredicationProbe(readerOf(reference), kG, 0, kQuietSp, kPcr);
        const auto r3        = static_cast<std::uint32_t>(original(reference, kG, 0).r3);
        reference.bytes.erase(kG + 0x31AC);
        EndSetPredicationProbe(readerOf(reference), *armed, r3);
        counts = TakeSetPredicationExitCounts();
        UNSCOPED_INFO("an unreadable exit word is counted");
        REQUIRE((counts.unreadable == 1 && counts.ok == 0));

        Memory     ringless  = entry;
        const auto armedRing = BeginSetPredicationProbe(readerOf(ringless), kG, 0, kQuietSp, kPcr);
        const auto r3Ring    = static_cast<std::uint32_t>(original(ringless, kG, 0).r3);
        ringless.bytes.erase(kCursor + 8);
        EndSetPredicationProbe(readerOf(ringless), *armedRing, r3Ring);
        counts = TakeSetPredicationExitCounts();
        UNSCOPED_INFO("an unreadable ring word is counted");
        REQUIRE((counts.unreadable == 1 && counts.ok == 0));
    }

    // A stack over the device is not modeled: nothing is armed and it is counted apart.
    {
        const auto armedAt = [&](std::uint32_t limit, std::uint32_t sp, std::uint32_t r4)
        {
            Memory reference = entry;
            placeStack(reference, limit, sp + 0x1000);
            return BeginSetPredicationProbe(readerOf(reference), kG, r4, sp, kPcr).has_value();
        };
        UNSCOPED_INFO("a stack overlap arms nothing");
        REQUIRE(!armedAt(kG + 0x2FA0 - 96, kG + 0x2FA0, 0));
        UNSCOPED_INFO("the first byte over the device arms nothing");
        REQUIRE(!armedAt(kG - 91, kG + 5, 5));
        UNSCOPED_INFO("a reach ending at G arms");
        REQUIRE(armedAt(kG - 96, kG, 0));
        const auto counts = TakeSetPredicationExitCounts();
        UNSCOPED_INFO("a stack overlap is counted as unmodeled");
        REQUIRE((counts.unmodeledStackOverlap == 2 && counts.unreadable == 0 && counts.ok == 0));
    }

    // The line. Pre-swap fields are absent with both pre-swap flags off.
    const bool savedPreSwap = REXCVAR_GET(native_consume_set_predication_preswap);
    REXCVAR_SET(native_consume_set_predication_preswap, false);
    SetPredicationExitCounts counts;
    counts.ok          = 5;
    counts.flushed     = 2;
    counts.unreadable  = 1;
    counts.mismatch[5] = 3;
    const auto line    = FormatSetPredicationExitCounts(counts);
    REXCVAR_SET(native_consume_set_predication_preswap, savedPreSwap);
    UNSCOPED_INFO("exit line head");
    REQUIRE(line.starts_with(" setpred_exit_ok=5 setpred_exit_flushed=2 setpred_exit_unreadable=1 setpred_exit_mismatch_predicate=0 setpred_exit_mismatch_mask=0"));
    UNSCOPED_INFO("exit line tiling");
    REQUIRE(line.find(" setpred_exit_mismatch_tiling=3 ") != std::string::npos);
    UNSCOPED_INFO("exit line tail");
    REQUIRE(line.ends_with(" setpred_exit_mismatch_ring_mask=0 setpred_exit_mismatch_result=0"));
    UNSCOPED_INFO("no unmodeled field while zero");
    REQUIRE(line.find("unmodeled") == std::string::npos);
    counts.unmodeledStackOverlap = 4;
    UNSCOPED_INFO("the unmodeled field follows the unreadable one");
    REQUIRE(FormatSetPredicationExitCounts(counts).starts_with(" setpred_exit_ok=5 setpred_exit_flushed=2 setpred_exit_unreadable=1 setpred_exit_unmodeled_stack_overlap=4 setpred_exit_mismatch_predicate=0"));
}

void testProbeUsesTheRegisterArgument()
{
    // The probe predicts for the argument of the call: a mask call and a clear call differ in the mask and the tiling bit.
    Fixture f;
    f.old                = 0x20;
    Memory     reference = build(f);
    const auto armed     = BeginSetPredicationProbe(readerOf(reference), kG, 0x30, kQuietSp, kPcr);
    UNSCOPED_INFO("r4 != 0 prediction");
    REQUIRE((armed && armed->predicted.mask == 0x30 && armed->predicted.predicate == 1 && armed->predicted.tiling == 0x20 && armed->predicted.word2ab4 == 0x30));
    const auto other = BeginSetPredicationProbe(readerOf(reference), kG, 0, kQuietSp, kPcr);
    UNSCOPED_INFO("r4 == 0 prediction");
    REQUIRE((other && other->predicted.mask == 0xFFFFFFFF && other->predicted.predicate == 0 && other->predicted.tiling == 0x21));
    TakeSetPredicationExitCounts();
}

#endif

void testHookEntry()
{
    // Both consumes are on by default and the exit check off; they act only
    // while ring stub services are published.
    UNSCOPED_INFO("the consumes default on and the exit check off");
    REQUIRE((REXCVAR_GET(native_consume_set_predication) && DEV_CHECK(!REXCVAR_GET(native_set_predication_exit_check)) && REXCVAR_GET(native_consume_set_predication_preswap)));
    UNSCOPED_INFO("unpublished ring stubs leave the consume unrequested");
    REQUIRE((!RingStubsPublished() && !SetPredicationConsumeRequested() && !SetPredicationExitCheckRequested()));
    PPCContext ctx{};
    ctx.r3.u64 = kG;
    ctx.r4.u64 = 0;
    ctx.lr     = 0x82500000;
#if REREVVED_DEVELOPER
    TakeSetPredicationConsumeCounts();
    const auto requireQuiet = [](const std::string& message)
    {
        const auto counts = TakeSetPredicationConsumeCounts();
        UNSCOPED_INFO(message + ": nothing consumed");
        REQUIRE((counts.consumed == 0 && counts.consumedClear == 0));
        for (const auto n : counts.fallback)
        {
            UNSCOPED_INFO(message + ": no verdict reached, no fallback counted");
            REQUIRE(n == 0);
        }
    };
#else
    const auto requireQuiet = [](const std::string&)
    {
    };
#endif

    // Without published services the hook does nothing, even for a caller that
    // reports the stubs ready, and even with the exit check on.
    for (const bool exitCheck : { false, true })
    {
        DEV_CVAR_SET(native_set_predication_exit_check, exitCheck);
        static_cast<void>(exitCheck);
        UNSCOPED_INFO("unpublished: Original runs");
        REQUIRE((!ConsumeSetPredication(ctx, true, false) && !ConsumeSetPredication(ctx, false, false) && !ConsumeSetPredication(ctx, true, true)));
        requireQuiet("unpublished");
    }
    DEV_CVAR_SET(native_set_predication_exit_check, false);

    // The services are published from here on.
    ringStubsActive.store(true);
    REXCVAR_SET(native_consume_set_predication, false);
    UNSCOPED_INFO("off: Original runs");
    REQUIRE(!ConsumeSetPredication(ctx, true, false));
    requireQuiet("off");

    // With the cvar on and no guest memory behind the reader, the call falls back as unreadable.
    REXCVAR_SET(native_consume_set_predication, true);
    UNSCOPED_INFO("published and on: requested");
    REQUIRE(SetPredicationConsumeRequested());
    UNSCOPED_INFO("on, no memory: Original runs");
    REQUIRE(!ConsumeSetPredication(ctx, true, false));
    UNSCOPED_INFO("on, stubs not ready: Original runs");
    REQUIRE(!ConsumeSetPredication(ctx, false, false));
    UNSCOPED_INFO("on, diagnostics: Original runs");
    REQUIRE(!ConsumeSetPredication(ctx, true, true));
    ctx.lr = kSetPredicationPreSwapReturn;
    UNSCOPED_INFO("on, the pre-swap call: Original runs");
    REQUIRE(!ConsumeSetPredication(ctx, true, false));
#if REREVVED_DEVELOPER
    auto counts = TakeSetPredicationConsumeCounts();
    UNSCOPED_INFO("nothing consumed");
    REQUIRE(counts.consumed == 0);
    UNSCOPED_INFO("the hook counts each reason once (no memory behind r13: unknown stack bounds)");
    REQUIRE((counts.fallback[static_cast<std::size_t>(SetPredicationFallback::StackOverlap)] == 1 && counts.fallback[static_cast<std::size_t>(SetPredicationFallback::Stubs)] == 1 &&
             counts.fallback[static_cast<std::size_t>(SetPredicationFallback::Diagnostics)] == 1 && counts.fallback[static_cast<std::size_t>(SetPredicationFallback::PreSwap)] == 1));
#endif
    // A withdrawal that lands between the cvar test and the lease: Original runs
    // and no verdict is reached, as if the services had never been published.
    ctx.lr = 0x82500000;
    SetRingStubLeaseProbe([]() noexcept
                          {
                              ringStubsActive.store(false, std::memory_order_seq_cst);
                          });
    UNSCOPED_INFO("a withdrawal before the lease: Original runs");
    REQUIRE(!ConsumeSetPredication(ctx, true, false));
    SetRingStubLeaseProbe(nullptr);
    ringStubsActive.store(true);
    requireQuiet("a withdrawal before the lease");
    // The stack is tested before any other guest word is read: with no guest memory behind r13 its bounds are unknown.
    ctx.lr      = 0x82500000;
    ctx.r1.u64  = kG + 0x2FA0;
    ctx.r13.u64 = kPcr;
    UNSCOPED_INFO("on, a frame over the device: Original runs");
    REQUIRE(!ConsumeSetPredication(ctx, true, false));
#if REREVVED_DEVELOPER
    counts = TakeSetPredicationConsumeCounts();
    UNSCOPED_INFO("the hook reads r1 and r13 and counts stack_overlap");
    REQUIRE((counts.fallback[static_cast<std::size_t>(SetPredicationFallback::StackOverlap)] == 1 && counts.fallback[static_cast<std::size_t>(SetPredicationFallback::Unreadable)] == 0));
#endif
    // The stack test is part of the decision made under the lease, and a refusal releases it: after another thread's
    // refused call the withdrawal's drain has no lease of that thread to wait for.
    {
        std::thread worker([ctx]() mutable
                           {
                               (void)ConsumeSetPredication(ctx, true, false);
                           });
        worker.join();
        const auto begun = std::chrono::steady_clock::now();
        WithdrawRingStubServices();
        UNSCOPED_INFO("a stack_overlap refusal released its lease");
        REQUIRE(std::chrono::steady_clock::now() - begun < std::chrono::seconds(1));
        ringStubsActive.store(true);
#if REREVVED_DEVELOPER
        (void)TakeSetPredicationConsumeCounts();
#endif
    }
    ctx.r1.u64  = 0;
    ctx.r13.u64 = 0;
#if REREVVED_DEVELOPER
    // The exit check forces the fallback.
    REXCVAR_SET(native_set_predication_exit_check, true);
    ctx.lr = 0x82500000;
    UNSCOPED_INFO("the exit check forces Original");
    REQUIRE(!ConsumeSetPredication(ctx, true, false));
    counts = TakeSetPredicationConsumeCounts();
    UNSCOPED_INFO("the exit check counts as diagnostics");
    REQUIRE(counts.fallback[static_cast<std::size_t>(SetPredicationFallback::Diagnostics)] == 1);
    REXCVAR_SET(native_set_predication_exit_check, false);
#endif
    ringStubsActive.store(false);
}

void testEntryAddress()
{
    UNSCOPED_INFO("the entry is 0x8269CD20 alone");
    REQUIRE((SetPredicationConsumeEntry(0x8269CD20) && !SetPredicationConsumeEntry(0x8269CD24) && !SetPredicationConsumeEntry(0x826A2970)));
    UNSCOPED_INFO("the pre-swap call returns to 0x826A4178");
    REQUIRE(kSetPredicationPreSwapReturn == 0x826A4178);
    UNSCOPED_INFO("fallback names");
    REQUIRE((kSetPredicationFallbackNames.size() == 6 && kSetPredicationFallbackNames[0] == "stubs" && kSetPredicationFallbackNames[2] == "preswap" &&
             kSetPredicationFallbackNames[3] == "stack_overlap" && kSetPredicationFallbackNames[4] == "unreadable" && kSetPredicationFallbackNames[5] == "ring_flush"));
}

} // namespace

TEST_CASE("set_predication", "[ctest][set-predication][timeout-120]")
{
    testEntryAddress();
    testPreSwapToken();
    testPreSwapPropertyAgainstTranscription();
    testPreSwapPreflight();
#if REREVVED_DEVELOPER
    testPreSwapProbePartition();
#endif
    testHandDerivedBit();
    testPropertyAgainstTranscription();
    testCursorBoundary();
    testPrologueStores();
    testStackOverlapEdges();
    testAdmittedFramesMatchOriginal();
    testOrder();
    testUnreadable();
    testUnchangedWordsAreNotWritten();
#if REREVVED_DEVELOPER
    testCounts();
    testProbe();
    testProbeUsesTheRegisterArgument();
#endif
    testHookEntry();
}
