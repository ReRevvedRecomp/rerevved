#include "gpu/diagnostics/route_effect_trace.h"
#include "gpu/diagnostics/route_trace.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace rerevved::gpu::diagnostics;

namespace
{

std::array<std::uint8_t, 0x10000> memory{};
unsigned                          reads    = 0;
bool                              failRead = false;
constexpr std::uint32_t           g        = 0x1000;

std::span<const std::uint8_t> read(std::uint64_t address, std::uint64_t size, bool physical)
{
    ++reads;
    if (failRead || physical || address > memory.size() || size > memory.size() - address)
        throw std::runtime_error("fixture_unreadable");
    return { memory.data() + address, static_cast<std::size_t>(size) };
}

void word(std::size_t address, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        memory.at(address + i) = static_cast<std::uint8_t>(value >> (24 - i * 8));
}

std::uint64_t le(const std::string& bytes, std::size_t offset, unsigned width)
{
    std::uint64_t result = 0;
    for (unsigned i = 0; i < width; ++i)
        result |= std::uint64_t(static_cast<unsigned char>(bytes.at(offset + i))) << (i * 8);
    return result;
}

std::string encoded(RouteEffectRecorder& recorder)
{
    std::ostringstream output(std::ios::binary);
    recorder.Write(output);
    return output.str();
}

std::vector<std::size_t> records(const std::string& bytes, RouteEffectKind kind)
{
    UNSCOPED_INFO("format magic");
    REQUIRE(bytes.substr(0, 8) == "RREVFX01");
    UNSCOPED_INFO("format version and header size");
    REQUIRE((le(bytes, 8, 4) == 1 && le(bytes, 12, 4) == 64));
    UNSCOPED_INFO("declared byte size");
    REQUIRE(le(bytes, 16, 8) + 64 == bytes.size());
    std::vector<std::size_t> result;
    for (std::size_t offset = 64; offset < bytes.size();)
    {
        const auto size = le(bytes, offset, 4);
        UNSCOPED_INFO("bounded record size");
        REQUIRE((size >= 48 && size <= bytes.size() - offset));
        if (le(bytes, offset + 4, 2) == static_cast<unsigned>(kind))
            result.push_back(offset);
        offset += size;
    }
    return result;
}

void encodingAndNesting()
{
    memory.fill(0);
    word(g + 0x30, 0x8000);
    word(0x7054, 0x7F00);
    RouteEffectRecorder recorder(read);
    recorder.Window(true, 17);
    const auto parent = recorder.Begin(0x826A3568, g, 0x7000, 0, { 17, 100, 0, 100 });
    const auto child  = recorder.Begin(0x826A3048, g, 0x7000, 0x7F04, { 17, 101, 100, 100 });
    word(g + 0x3484, 0x8004);
    word(g + 0x18, 0x12345678);
    word(g + 0x1C, 0x9ABCDEF0);
    word(g + 0x3A44, 0x11223344);
    word(g + 0x3A54, 0x40);
    word(0x8004, 0x61626364);
    word(0x7F00, 0x9000);
    word(0x7F04, 0x9100);
    recorder.End(child, 0);
    recorder.Window(false, 18);
    recorder.Copy(parent.identity, 0x8004, 4);
    word(g + 0x30, 0x8004);
    recorder.End(parent, 0xFEDCBA9876543210ULL);
    const auto bytes = encoded(recorder);
    const auto begin = records(bytes, RouteEffectKind::Begin);
    const auto end   = records(bytes, RouteEffectKind::End);
    UNSCOPED_INFO("paired nested records");
    REQUIRE((begin.size() == 2 && end.size() == 2));
    UNSCOPED_INFO("call parent submission encoding");
    REQUIRE((le(bytes, begin[1] + 24, 8) == 101 && le(bytes, begin[1] + 32, 8) == 100 &&
             le(bytes, begin[1] + 40, 8) == 100));
    UNSCOPED_INFO("full return register encoding");
    REQUIRE(le(bytes, end[1] + 56, 8) == 0xFEDCBA9876543210ULL);
    bool dirty = false, output = false, allocator = false;
    for (const auto at : records(bytes, RouteEffectKind::Field))
    {
        if (le(bytes, at + 48, 8) == g + 0x18)
            dirty = le(bytes, at + 72, 8) == 0x123456789ABCDEF0ULL;
        if (le(bytes, at + 48, 8) == 0x7F04)
            output = le(bytes, at + 72, 8) == 0x9100;
        if (le(bytes, at + 48, 8) == g + 0x3A44)
            allocator = le(bytes, at + 72, 8) == 0x11223344;
    }
    UNSCOPED_INFO("dirty output and external allocator fields");
    REQUIRE((dirty && output && allocator));
    const auto ranges = records(bytes, RouteEffectKind::Range);
    UNSCOPED_INFO("command intervals and copy destination");
    REQUIRE(ranges.size() == 3);
    UNSCOPED_INFO("copy destination retains producing call");
    REQUIRE((le(bytes, ranges[1] + 64, 4) == 3 && le(bytes, ranges[1] + 24, 8) == 100));
    UNSCOPED_INFO("cursor endpoints");
    REQUIRE((le(bytes, ranges[0] + 48, 8) == 0x8004 && le(bytes, ranges[0] + 56, 8) == 4));
    const std::array<unsigned char, 32> expected{
        0x88, 0xD4, 0x26, 0x6F, 0xD4, 0xE6, 0x33, 0x8D, 0x13, 0xB8, 0x45, 0xFC, 0xF2, 0x89, 0x57, 0x9D, 0x20, 0x9C, 0x89, 0x78, 0x23, 0xB9, 0x21, 0x7D, 0xA3, 0xE1, 0x61, 0x93, 0x6F, 0x03, 0x15, 0x89
    };
    for (unsigned i = 0; i < expected.size(); ++i)
    {
        UNSCOPED_INFO("SHA-256 of abcd");
        REQUIRE(le(bytes, ranges[0] + 72 + i, 1) == expected[i]);
    }
    UNSCOPED_INFO("clean encoding counters");
    REQUIRE((le(bytes, 24, 8) == 0 && le(bytes, 32, 8) == 0));
}

void pollsAndReaders()
{
    memory.fill(0);
    word(g + 0x2A90, 0x9000);
    word(g + 0x2A9C, 10);
    word(0x9000, 3);
    word(0x9100, g);
    RouteEffectRecorder recorder(read);
    recorder.Window(true, 20);
    recorder.Reader(0x8268CBB8, 0x8269BF08);
    recorder.Reader(0x8268CBB8, 0x8269BF08);
    recorder.Reader(0x826A5B48, 0x8268CC6C);
    for (unsigned i = 0; i < 100; ++i)
    {
        recorder.Progress(0x8268CBB8, 0, 0x9100);
        recorder.Progress(0x8268CBB8, 0, 0x9100, true);
    }
    word(0x9000, 4);
    recorder.Progress(0x8268CBB8, 0, 0x9100);
    const auto draw = recorder.Begin(0x826A39F8, g, 0, 0, { 20, 88, 0, 88 });
    recorder.End(draw, 7);
    recorder.Progress(0x8268CBB8, 0, 0x9100);
    const auto bytes = encoded(recorder);
    const auto polls = records(bytes, RouteEffectKind::Progress);
    UNSCOPED_INFO("value or submission transition flushes unchanged polls");
    REQUIRE(polls.size() == 3);
    UNSCOPED_INFO("unchanged poll aggregation");
    REQUIRE(le(bytes, polls[0] + 64, 8) == 100);
    UNSCOPED_INFO("return observations do not double the iteration count");
    REQUIRE(le(bytes, polls[0] + 120, 8) == 200);
    UNSCOPED_INFO("pointee transition");
    REQUIRE((le(bytes, polls[0] + 80, 8) == 3 && le(bytes, polls[1] + 80, 8) == 4));
    UNSCOPED_INFO("current submission at transition");
    REQUIRE(le(bytes, polls[2] + 40, 8) == 88);
    const auto owners = records(bytes, RouteEffectKind::Reader);
    UNSCOPED_INFO("reader owner is emitted only once");
    REQUIRE(owners.size() == 2);
    UNSCOPED_INFO("unobserved reader flag");
    REQUIRE(le(bytes, owners[1] + 52, 4) == 1);
}

void drawWindowBudget()
{
    memory.fill(0);
    word(0x7054, 0x7F00);
    RouteEffectRecorder recorder(read);
    recorder.Window(true, 1);
    for (unsigned i = 0; i < 3075; ++i)
    {
        word(g + 0x30, 0x8000);
        word(g + 0x3484, 0x8000);
        const auto parent = recorder.Begin(0x826A3568, g, 0x7000, 0, { 1, 2 * i + 1, 0, 2 * i + 1 });
        const auto child  = recorder.Begin(0x826A3048, g, 0x7000, 0x7F04, { 1, 2 * i + 2, 2 * i + 1, 2 * i + 1 });
        for (const auto offset : { 0x00, 0x08, 0x10, 0x18, 0x20, 0x3488, 0x3A54 })
            word(g + offset, i + 1);
        word(g + 0x3484, 0x8004);
        recorder.End(child, 0);
        recorder.Copy(parent.identity, 0x8004, 4);
        recorder.Copy(parent.identity, 0x8008, 4);
        word(g + 0x30, 0x8004);
        recorder.End(parent, 0);
    }
    recorder.Window(false, 2);
    const auto before = recorder.Stats();
    reads             = 0;
    recorder.Reader(0x8268CBB8, 0);
    recorder.Progress(0x8268CBB8, g);
    UNSCOPED_INFO("closing the window stops new draws, readers and progress");
    REQUIRE((!recorder.Begin(0x826A39F8, g, 0, 0, {}).recorder && reads == 0 && recorder.Stats().bytes == before.bytes));
    const auto bytes = encoded(recorder);
    UNSCOPED_INFO("3075 nested draws fit the 10 MiB budget with about 40 percent margin");
    REQUIRE((before.bytes == 3075ULL * 2048 && !before.lost && !before.failedReads && !before.gaps && !before.unfinished));
    UNSCOPED_INFO("unchanged field accounting");
    REQUIRE(before.unchangedFields == 3075ULL * (64 - 17));
    UNSCOPED_INFO("only changed fields are encoded");
    REQUIRE(records(bytes, RouteEffectKind::Field).size() == 3075ULL * 17);
    const auto ends = records(bytes, RouteEffectKind::End);
    UNSCOPED_INFO("end records count unchanged fields explicitly");
    REQUIRE((le(bytes, ends[0] + 64, 4) == 25 && le(bytes, ends[0] + 68, 4) == 33));
}

void disabledOverflowAndFailures()
{
    memory.fill(0);
    RouteEffectRecorder disabled(read);
    reads            = 0;
    const auto token = disabled.Begin(0x826A39F8, g, 0, 0, { 1, 1, 0, 1 });
    disabled.End(token, 1);
    disabled.Reader(0x8268CBB8, 0);
    disabled.Progress(0x8268CBB8, g);
    UNSCOPED_INFO("disabled path reads and records nothing");
    REQUIRE((!token.recorder && reads == 0 && encoded(disabled).size() == 64));
    RouteEffectRecorder small(read, 64);
    small.Window(true, 1);
    const auto call = small.Begin(0x826A39F8, g, 0, 0, { 1, 1, 0, 1 });
    small.End(call, 0);
    const auto bytes = encoded(small);
    UNSCOPED_INFO("overflow is bounded and serialized");
    REQUIRE((bytes.size() == 128 && le(bytes, 24, 8) > 0));
    UNSCOPED_INFO("overflow count matches header");
    REQUIRE(small.Overflow() == le(bytes, 24, 8));
    RouteEffectRecorder failed(read);
    failed.Window(true, 1);
    failRead       = true;
    const auto bad = failed.Begin(0x826A39F8, g, 0, 0, { 1, 1, 0, 1 });
    failed.End(bad, 0);
    failRead = false;
    UNSCOPED_INFO("read failure is visible");
    REQUIRE((failed.ReadFailures() > 0 && le(encoded(failed), 32, 8) > 0));
    UNSCOPED_INFO("unreadable zero values are never suppressed as unchanged");
    REQUIRE(records(encoded(failed), RouteEffectKind::Field).size() == std::size(kRouteEffectFields));
    RouteEffectRecorder moved(read);
    moved.Window(true, 1);
    word(g + 0x30, 0x8000);
    const auto relocation = moved.Begin(0x826A39F8, g, 0, 0, { 1, 1, 0, 1 });
    word(g + 0x30, 0x4000);
    moved.End(relocation, 0);
    UNSCOPED_INFO("ring relocation is never silently omitted");
    REQUIRE(!records(encoded(moved), RouteEffectKind::Gap).empty());

    RouteEffectRecorder slots(read);
    slots.Window(true, 1);
    word(g + 0x2A90, 0x9000);
    for (unsigned i = 0; i < 17; ++i)
    {
        word(0xA000 + i * 16, g);
        slots.Progress(0x8268CBB8, 0, 0xA000 + i * 16);
    }
    UNSCOPED_INFO("poll slot exhaustion is visible");
    REQUIRE(le(encoded(slots), 24, 8) == 1);

    RouteEffectRecorder unfinished(read);
    unfinished.Window(true, 1);
    const auto open       = unfinished.Begin(0x826A39F8, g, 0, 0, { 1, 9, 0, 9 });
    const auto incomplete = encoded(unfinished);
    UNSCOPED_INFO("unfinished return is visible");
    REQUIRE((le(incomplete, 48, 8) == 1 && !records(incomplete, RouteEffectKind::Gap).empty()));
    unfinished.End(open, 0);
}

void sharedRouteGate()
{
    routeTraceEnabled.store(0);
    routeTraceRecorder.store(nullptr);
    UNSCOPED_INFO("disabled hook does not dereference the recorder");
    REQUIRE(!TraceRouteEntry(RouteFunction::sub_8268CBB8, 0x8269BF08));
    auto counts = std::make_unique<RouteRecorder>();
    routeTraceRecorder.store(counts.get());
    routeTraceEnabled.store(1);
    UNSCOPED_INFO("count-only mode does not enter effect hooks");
    REQUIRE(!TraceRouteEntry(RouteFunction::sub_8268CBB8, 0x8269BF08));
    routeTraceEnabled.store(kRouteTraceEffects);
    UNSCOPED_INFO("effect mode shares the route gate");
    REQUIRE(TraceRouteEntry(RouteFunction::sub_8268CBB8, 0x8269BF08));
    routeTraceEnabled.store(0);
    const auto snapshot = counts->CloseSegment();
    UNSCOPED_INFO("route counts are identical in both enabled modes");
    REQUIRE((snapshot.callers.size() == 1 && snapshot.callers[0].count == 2));
    routeTraceRecorder.store(nullptr);
}

} // namespace

TEST_CASE("route_effect_trace", "[ctest][route-effect-trace]")
{
    encodingAndNesting();
    pollsAndReaders();
    drawWindowBudget();
    disabledOverflowAndFailures();
    sharedRouteGate();
}
