#include "gpu/guest_d3d/shader_resource_literals.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <toml++/toml.hpp>
#include <vector>

using namespace rerevved::gpu::guest_d3d;

namespace
{

// The C10 movie pixel shader object (shader:1363475376, event 6394, ps_header
// 941c0041...): the resource base is word 6 and the literal table offset,
// 0xE0, is word 15, at stage header + 20 with the header at object + 40.
constexpr std::uint32_t                 kMovieObject = 0x5144FBB0;
constexpr std::array<std::uint32_t, 17> kMovieHeader{ 0x00100007, 1, 0, 0, 0, 0xFFFF0000, 0xFAE9F000, 0, 0, 0, 0x102A1100, 0x12C, 0xD0, 0, 0x24, 0xE0, 0x108 };
constexpr std::uint32_t                 kMovieTable = kMovieObject + 40 + 0xE0;
// The captured ps_code binding is physical 0x1AEA0040 and the loader's alias
// of the resource base is 0x1AEA0000: the 64-byte block precedes the code.
constexpr std::uint32_t kMovieBlock = 0x1AEA0000, kMovieCode = 0x1AEA0040;
// The movie literal table and its 64 value bytes as the S11 capture retained
// them (event 5490, ps_literal_table b97d0e67..., ps_literal_values:508
// 9ad5b36d...): one 01FC0010 record, then c252 = 0 and the YCbCr matrix rows.
constexpr std::array<std::uint32_t, 10> kMovieTableWords{ 0, 1, 0, 0, 20, 0x01FC0010, 0, 0, 0, 0 };
constexpr std::array<std::uint32_t, 16> kMovieValues{ 0, 0, 0, 0, 0xBEC86C00, 0x40012400, 0x3F950200, 0x3FCC4300, 0x3F079AC0, 0xBF8A7420, 0x3F950200, 0xBF504000, 0xBF5EE340, 0, 0, 0 };

constexpr std::uint32_t kObject = 0x20000, kValues = 0x1000;

void store(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    if (bytes.size() < offset + 4)
        bytes.resize(offset + 4);
    for (unsigned c = 0; c < 4; ++c)
        bytes[offset + c] = static_cast<std::uint8_t>(value >> (24 - c * 8));
}

std::uint32_t be(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) | (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

std::uint32_t pattern(std::uint32_t i)
{
    return 0x3F000000U + i * 0x00010001U;
}

std::vector<std::uint8_t> zeros(std::size_t count)
{
    return std::vector<std::uint8_t>(count, 0);
}

// Sparse guest memory. A read must fall inside regions of the matching kind,
// and a later region shadows an earlier one, so a patch word wins.
struct Guest
{
    struct Region
    {
        std::uint64_t             address;
        bool                      physical;
        std::vector<std::uint8_t> bytes;
    };

    std::vector<Region> regions;
    bool                shortReads = false, throwing = false;

    void put(std::uint64_t address, bool physical, std::vector<std::uint8_t> bytes)
    {
        regions.push_back({ address, physical, std::move(bytes) });
    }

    void word(std::uint64_t address, std::uint32_t value)
    {
        std::vector<std::uint8_t> bytes;
        store(bytes, 0, value);
        put(address, false, std::move(bytes));
    }

    ReadMemory reader()
    {
        return [this](std::uint64_t address, std::uint64_t size, bool physical) -> std::vector<std::uint8_t>
        {
            if (throwing)
                throw std::runtime_error("unreadable");
            std::vector<std::uint8_t> out(size, 0);
            for (std::uint64_t i = 0; i < size; ++i)
            {
                bool found = false;
                for (auto it = regions.rbegin(); it != regions.rend() && !found; ++it)
                    if (it->physical == physical && address + i >= it->address && address + i - it->address < it->bytes.size())
                    {
                        out[i] = it->bytes[address + i - it->address];
                        found  = true;
                    }
                if (!found)
                    throw std::runtime_error("unmapped");
            }
            if (shortReads && size)
                out.pop_back();
            return out;
        };
    }
};

struct Record
{
    std::uint16_t first, count;
    std::uint32_t offset;
};

// A shader object whose stage header names a literal table at +0xE0.
// `tableBytes` is the table's recordBytes word; `trailing` extends the table
// region with zero bytes past the declared records.
Guest shader(bool pixel, std::uint32_t base, const std::vector<Record>& records, std::uint32_t tableBytes, std::size_t trailing = 0)
{
    Guest      guest;
    const auto header = kObject + (pixel ? 40U : 872U);
    guest.word(kObject + (pixel ? 24U : 32U), base);
    guest.word(header + 20, 0xE0);
    std::vector<std::uint8_t> table(20, 0);
    store(table, 16, tableBytes);
    for (std::size_t i = 0; i < records.size(); ++i)
    {
        store(table, 20 + i * 8, (std::uint32_t(records[i].first) << 16) | records[i].count);
        store(table, 24 + i * 8, records[i].offset);
    }
    table.resize(std::max<std::size_t>(table.size(), 20 + std::min<std::size_t>(tableBytes, 8192)) + trailing, 0);
    guest.put(header + 0xE0, false, std::move(table));
    return guest;
}

// The movie pixel shader as captured: the C10 header, then the S11 table of
// one 01FC0010 record, a zero terminator and 4 pad bytes, and the S11 values.
Guest movie()
{
    Guest                     guest;
    std::vector<std::uint8_t> header;
    for (std::size_t i = 0; i < kMovieHeader.size(); ++i)
        store(header, i * 4, kMovieHeader[i]);
    guest.put(kMovieObject, false, header);
    std::vector<std::uint8_t> table;
    for (std::size_t i = 0; i < kMovieTableWords.size(); ++i)
        store(table, i * 4, kMovieTableWords[i]);
    guest.put(kMovieTable, false, table);
    std::vector<std::uint8_t> block;
    for (std::size_t i = 0; i < kMovieValues.size(); ++i)
        store(block, i * 4, kMovieValues[i]);
    guest.put(kMovieBlock, true, block);
    return guest;
}

struct Outcome
{
    bool                                                 ok;
    std::string                                          error;
    std::vector<ShaderLiteralRange>                      ranges;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> registers;
};

Outcome run(Guest& guest, std::uint32_t object, bool pixel)
{
    Outcome o;
    // Stale caller output must never survive a call.
    o.ranges    = { { 1, 2, 3 } };
    o.registers = { { 4, 5 } };
    o.error     = "stale";
    o.ok        = ReadShaderResourceLiterals(guest.reader(), object, pixel, o.ranges, o.registers, o.error);
    return o;
}

void refused(const Outcome& o, const char* code, const std::string& what)
{
    UNSCOPED_INFO(what + ": expected " + code + ", got [" + o.error + "]");
    REQUIRE((!o.ok && o.error == code && o.ranges.empty() && o.registers.empty()));
}

void accepted(const Outcome& o, const std::string& what)
{
    UNSCOPED_INFO(what + ": refused [" + o.error + "]");
    REQUIRE((o.ok && o.error.empty()));
}

void unit()
{
    {
        auto       guest = movie();
        const auto o     = run(guest, kMovieObject, true);
        accepted(o, "movie pixel shader");
        UNSCOPED_INFO("the movie record is {508, 16} at the aliased resource base");
        REQUIRE((o.ranges.size() == 1 && o.ranges[0].firstFloat4 == 508 && o.ranges[0].dwordCount == 16 && o.ranges[0].physical == kMovieBlock));
        UNSCOPED_INFO("the literal block precedes the captured movie code");
        REQUIRE(o.ranges[0].physical + 0x40 == kMovieCode);
        UNSCOPED_INFO("sixteen registers");
        REQUIRE(o.registers.size() == 16);
        for (std::uint32_t i = 0; i < 16; ++i)
        {
            UNSCOPED_INFO("register 0x47F0 + i holds host-order word i");
            REQUIRE((o.registers[i].first == 0x47F0 + i && o.registers[i].second == kMovieValues[i]));
        }
    }
    {
        // The scene vertex shader's 00FC0010 record under the vertex stage.
        auto                      guest = shader(false, kValues, { { 252, 16, 0x40 } }, 20);
        std::vector<std::uint8_t> block;
        for (std::uint32_t i = 0; i < 16; ++i)
            store(block, i * 4, pattern(i + 100));
        guest.put(kValues + 0x40, true, block);
        const auto o = run(guest, kObject, false);
        accepted(o, "vertex stage record");
        UNSCOPED_INFO("vertex record range");
        REQUIRE((o.ranges.size() == 1 && o.ranges[0].firstFloat4 == 252 && o.ranges[0].physical == kValues + 0x40));
        UNSCOPED_INFO("vertex literals land at 0x43F0..0x43FF");
        REQUIRE((o.registers.size() == 16 && o.registers[0] == std::make_pair(0x43F0U, pattern(100)) && o.registers[15].first == 0x43FF));
    }
    {
        auto guest = shader(true, kValues, {}, 8);
        guest.word(kObject + 40 + 20, 0);
        const auto o = run(guest, kObject, true);
        accepted(o, "no literal table");
        UNSCOPED_INFO("no table loads nothing");
        REQUIRE((o.ranges.empty() && o.registers.empty()));
    }
    {
        // A zero count stops the loop before the next record is read: the
        // third record's values are unmapped, so reading it would fail.
        auto guest = shader(true, kValues, { { 256, 4, 0 }, { 300, 0, 0xFFFF }, { 400, 4, 0x80 } }, 24);
        guest.put(kValues, true, zeros(16));
        const auto o = run(guest, kObject, true);
        accepted(o, "zero-count terminator");
        UNSCOPED_INFO("only the record before the terminator loads");
        REQUIRE((o.ranges.size() == 1 && o.registers.size() == 4));
    }
    {
        // Records load in guest order, an overlap included.
        auto                      guest = shader(true, kValues, { { 256, 4, 0 }, { 256, 2, 0x10 } }, 16);
        std::vector<std::uint8_t> block;
        for (std::uint32_t i = 0; i < 8; ++i)
            store(block, i * 4, 0x100 + i);
        guest.put(kValues, true, block);
        const auto o = run(guest, kObject, true);
        accepted(o, "two records");
        UNSCOPED_INFO("both records load");
        REQUIRE((o.ranges.size() == 2 && o.registers.size() == 6));
        const std::vector<std::pair<std::uint32_t, std::uint32_t>> expected{ { 0x4400, 0x100 }, { 0x4401, 0x101 }, { 0x4402, 0x102 }, { 0x4403, 0x103 }, { 0x4400, 0x104 }, { 0x4401, 0x105 } };
        UNSCOPED_INFO("the later overlapping record follows the earlier one");
        REQUIRE(o.registers == expected);
    }
    {
        // recordBytes 20 is one record plus a terminator whose head fits in
        // the table, so only that record loads.
        auto one = shader(true, kValues, { { 256, 4, 0 }, { 0, 0, 0 } }, 20);
        one.put(kValues, true, zeros(32));
        const auto o = run(one, kObject, true);
        accepted(o, "recordBytes 20 with a zero second record");
        UNSCOPED_INFO("recordBytes 20 reads one record");
        REQUIRE(o.ranges.size() == 1);
        // A second nonzero record leaves a third head at +16 whose body would
        // lie past the table end.
        auto partial = shader(true, kValues, { { 256, 4, 0 }, { 257, 4, 0x10 } }, 20, 4);
        partial.word(kObject + 40 + 0xE0 + 20 + 16, 0x01010004);
        partial.put(kValues, true, zeros(32));
        refused(run(partial, kObject, true), "shader_literal_record_extent", "a nonzero record that runs past the table");
        // The same third head with a zero count ends the loop in the table.
        auto ended = shader(true, kValues, { { 256, 4, 0 }, { 257, 4, 0x10 } }, 20, 4);
        ended.put(kValues, true, zeros(32));
        const auto e = run(ended, kObject, true);
        accepted(e, "a zero third head");
        UNSCOPED_INFO("two records before the zero head");
        REQUIRE(e.ranges.size() == 2);
        auto straddle = shader(true, kValues, { { 256, 4, 0 } }, 10);
        straddle.put(kValues, true, zeros(32));
        refused(run(straddle, kObject, true), "shader_literal_record_extent", "a record head that runs past the table");
    }
    {
        auto guest = shader(true, kValues, {}, 4096);
        accepted(run(guest, kObject, true), "4096 record bytes");
        auto exact = shader(true, kValues, { { 256, 4, 0 } }, 8);
        exact.put(kValues, true, zeros(16));
        const auto one = run(exact, kObject, true);
        accepted(one, "8 record bytes");
        UNSCOPED_INFO("8 record bytes hold one record");
        REQUIRE(one.ranges.size() == 1);
        for (const std::uint32_t size : { 0U, 4U, 7U, 4097U, 4100U, 0x10000U })
        {
            auto bad = shader(true, kValues, {}, size);
            refused(run(bad, kObject, true), "shader_literal_extent", "record bytes " + std::to_string(size));
        }
    }
    {
        auto guest = shader(true, kValues, { { 256, 1, 0xFFFFFFFF } }, 16);
        refused(run(guest, kObject, true), "shader_literal_address", "a resource sum past 32 bits");
        auto wild = shader(true, kValues, { { 256, 1, 0x70000 } }, 16);
        refused(run(wild, kObject, true), "shader_literal_unreadable", "a corrupt offset into unmapped memory");
    }
    {
        // A pixel read refuses a record in the vertex half, c252 included.
        auto guest = shader(true, kValues, { { 252, 16, 0 } }, 20);
        guest.put(kValues, true, zeros(64));
        refused(run(guest, kObject, true), "shader_literal_stage", "a vertex-stage record under pixel");
        auto edge = shader(true, kValues, { { 255, 4, 0 } }, 20);
        edge.put(kValues, true, zeros(16));
        refused(run(edge, kObject, true), "shader_literal_stage", "the last vertex float4 under pixel");
        auto first = shader(true, kValues, { { 256, 4, 0 } }, 20);
        first.put(kValues, true, zeros(16));
        accepted(run(first, kObject, true), "the first pixel float4");
        // A vertex read refuses a record that leaves the vertex half.
        auto cross = shader(false, kValues, { { 255, 8, 0 } }, 20);
        cross.put(kValues, true, zeros(32));
        refused(run(cross, kObject, false), "shader_literal_stage", "a vertex record crossing into the pixel half");
        auto inPixel = shader(false, kValues, { { 300, 4, 0 } }, 20);
        inPixel.put(kValues, true, zeros(16));
        refused(run(inPixel, kObject, false), "shader_literal_stage", "a pixel-half record under vertex");
        auto flush = shader(false, kValues, { { 255, 4, 0 } }, 20);
        flush.put(kValues, true, zeros(16));
        accepted(run(flush, kObject, false), "a vertex record ending at c255");
    }
    {
        // The file ends at register 0x4800.
        auto guest = shader(true, kValues, { { 511, 4, 0 } }, 20);
        guest.put(kValues, true, zeros(16));
        const auto last = run(guest, kObject, true);
        accepted(last, "the last float4");
        UNSCOPED_INFO("ends at register 0x47FF");
        REQUIRE(last.registers.back().first == 0x47FF);
        auto over = shader(true, kValues, { { 511, 5, 0 } }, 20);
        over.put(kValues, true, zeros(20));
        refused(run(over, kObject, true), "shader_literal_range", "one dword past 0x4800");
        auto beyond = shader(true, kValues, { { 0xFFFF, 1, 0 } }, 20);
        refused(run(beyond, kObject, true), "shader_literal_range", "a record far past the file");
    }
    {
        // Physical alias: the low 29 bits, plus 0x1000 from 0xE0000000.
        const auto aliased = [&](std::uint32_t base)
        {
            auto                guest    = shader(true, base, { { 256, 1, 0 } }, 16);
            const std::uint32_t expected = (base & 0x1FFFFFFFU) + (base >= 0xE0000000U ? 0x1000U : 0U);
            guest.put(expected, true, zeros(4));
            const auto o = run(guest, kObject, true);
            accepted(o, "alias of " + std::to_string(base));
            return o.ranges.at(0).physical == expected;
        };
        UNSCOPED_INFO("a low address is unchanged");
        REQUIRE(aliased(0x1000));
        UNSCOPED_INFO("the top three bits are masked");
        REQUIRE(aliased(0x5ABCDEF0));
        UNSCOPED_INFO("just below 0xE0000000 has no alias step");
        REQUIRE(aliased(0xDFFFFFF0));
        UNSCOPED_INFO("0xE0000000 adds 0x1000");
        REQUIRE(aliased(0xE0000010));
        UNSCOPED_INFO("the movie base adds 0x1000");
        REQUIRE(aliased(0xFAE9F000));
    }
    {
        // Values read from physical memory and the table from virtual: values
        // mapped only as virtual, or a table only as physical, fail.
        auto guest = shader(true, kValues, { { 256, 4, 0 } }, 16);
        guest.put(kValues, false, zeros(16));
        refused(run(guest, kObject, true), "shader_literal_unreadable", "values mapped only as virtual");
        auto swapped = shader(true, kValues, { { 256, 4, 0 } }, 16);
        swapped.put(kValues, true, zeros(16));
        for (auto& region : swapped.regions)
            if (region.address == kObject + 40 + 0xE0)
                region.physical = true;
        refused(run(swapped, kObject, true), "shader_literal_unreadable", "table mapped only as physical");
    }
    {
        // Each stage reads its own header and resource word.
        auto vertex = shader(false, kValues, { { 252, 16, 0 } }, 20);
        vertex.put(kValues, true, zeros(64));
        const auto asPixel = run(vertex, kObject, true);
        UNSCOPED_INFO("a vertex object under pixel is refused");
        REQUIRE((!asPixel.ok && asPixel.ranges.empty()));
        auto pixelObject = shader(true, kValues, { { 256, 4, 0 } }, 20);
        pixelObject.put(kValues, true, zeros(16));
        UNSCOPED_INFO("a pixel object under vertex is refused");
        REQUIRE(!run(pixelObject, kObject, false).ok);
    }
    {
        Guest unmapped;
        refused(run(unmapped, kObject, true), "shader_literal_unreadable", "an unmapped object");
        auto throwing     = movie();
        throwing.throwing = true;
        refused(run(throwing, kMovieObject, true), "shader_literal_unreadable", "a throwing read");
        auto shortRead       = movie();
        shortRead.shortReads = true;
        refused(run(shortRead, kMovieObject, true), "shader_literal_unreadable", "a short read");
        auto noBase = shader(true, kValues, { { 256, 4, 0 } }, 16);
        noBase.regions.erase(noBase.regions.begin());
        refused(run(noBase, kObject, true), "shader_literal_unreadable", "an unreadable resource base");
        auto high = movie();
        refused(run(high, 0xFFFFFFF0U, true), "shader_literal_address", "a header past 32 bits");
    }
    std::cout << "shader_resource_literals unit ok\n";
}

std::string environment(const char* name)
{
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    UNSCOPED_INFO("cannot open " + path.string());
    REQUIRE(bool(file));
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

// S11 event 5490: the first movie quad's captured literals match the fixture.
void movieCapture(const std::filesystem::path& directory)
{
    const auto  manifest = toml::parse_file((directory / "payload-manifest.toml").string());
    const auto* bindings = manifest["bindings"].as_array();
    UNSCOPED_INFO("the movie payload manifest has bindings");
    REQUIRE(bindings != nullptr);
    Guest                     guest;
    std::vector<std::uint8_t> values;
    std::int64_t              objectAddress = 0, codeAddress = 0;
    for (const auto& item : *bindings)
    {
        const auto* b = item.as_table();
        if (!b || (*b)["event"].value_or(std::int64_t{ 0 }) != 5490)
            continue;
        const auto role    = (*b)["role"].value_or(std::string{});
        const auto address = (*b)["address"].value_or(std::int64_t{ 0 });
        if (role == "ps_code")
        {
            codeAddress = address;
            continue;
        }
        if (role != "ps_header" && role != "ps_literal_table" && role != "ps_literal_values:508")
            continue;
        const auto bytes = readFile(directory / (*b)["file"].value_or(std::string{}));
        if (role == "ps_header")
        {
            UNSCOPED_INFO("event 5490 saved the 68-byte ps_header");
            REQUIRE(bytes.size() == 68);
            // Word 2 changes per frame; the literal fields are fixed.
            for (std::size_t i = 0; i < kMovieHeader.size(); ++i)
            {
                UNSCOPED_INFO("the S11 movie header equals C10's except word 2");
                REQUIRE((i == 2 || be(bytes, i * 4) == kMovieHeader[i]));
            }
            objectAddress = address;
            guest.put(std::uint64_t(address), false, bytes);
        }
        else if (role == "ps_literal_table")
        {
            UNSCOPED_INFO("the S11 literal table address and extent");
            REQUIRE((address == kMovieTable && bytes.size() == kMovieTableWords.size() * 4));
            for (std::size_t i = 0; i < kMovieTableWords.size(); ++i)
            {
                UNSCOPED_INFO("the fixture table equals the S11 table");
                REQUIRE(be(bytes, i * 4) == kMovieTableWords[i]);
            }
            guest.put(std::uint64_t(address), false, bytes);
        }
        else
        {
            UNSCOPED_INFO("the S11 literal values address and extent");
            REQUIRE((address == kMovieBlock && bytes.size() == 64));
            values = bytes;
            guest.put(std::uint64_t(address), true, bytes);
        }
    }
    UNSCOPED_INFO("event 5490 retained the movie header and values");
    REQUIRE((objectAddress == kMovieObject && values.size() == 64));
    for (std::size_t i = 0; i < kMovieValues.size(); ++i)
    {
        UNSCOPED_INFO("the fixture values equal the S11 values");
        REQUIRE(be(values, i * 4) == kMovieValues[i]);
    }
    const auto o = run(guest, kMovieObject, true);
    accepted(o, "S11 movie literals");
    UNSCOPED_INFO("the S11 record is {508, 16} and precedes the captured code by 0x40");
    REQUIRE((o.ranges.size() == 1 && o.ranges[0].firstFloat4 == 508 && o.ranges[0].dwordCount == 16 && codeAddress == std::int64_t(o.ranges[0].physical) + 0x40));
    for (std::uint32_t i = 0; i < 16; ++i)
    {
        UNSCOPED_INFO("S11 register 0x47F0 + i holds the retained word");
        REQUIRE((o.registers[i].first == 0x47F0 + i && o.registers[i].second == be(values, i * 4)));
    }
    std::cout << "shader_resource_literals movie capture ok\n";
}

// C10 event 6394 has no saved literals; capture requires its variable.
void capture()
{
    const auto movieRoot = environment("REREVVED_MOVIE_CAPTURE");
    const auto root      = environment("REREVVED_GFX_ADAPTER_CAPTURE");
    if (movieRoot.empty() && root.empty())
    {
        SKIP("REREVVED_GFX_ADAPTER_CAPTURE and REREVVED_MOVIE_CAPTURE are not set");
    }
    if (!movieRoot.empty())
        movieCapture(movieRoot);
    if (root.empty())
        return;
    const std::filesystem::path directory = root;
    const auto                  manifest  = toml::parse_file((directory / "payload-manifest.toml").string());
    const auto*                 bindings  = manifest["bindings"].as_array();
    UNSCOPED_INFO("the payload manifest has bindings");
    REQUIRE(bindings != nullptr);
    std::vector<std::uint8_t> header;
    std::int64_t              codeAddress = 0;
    for (const auto& item : *bindings)
    {
        const auto* b = item.as_table();
        if (!b || (*b)["event"].value_or(std::int64_t{ 0 }) != 6394)
            continue;
        const auto role = (*b)["role"].value_or(std::string{});
        if (role == "ps_header")
        {
            std::ifstream file(directory / (*b)["file"].value_or(std::string{}), std::ios::binary);
            header.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            UNSCOPED_INFO("the capture's movie PS object address");
            REQUIRE((*b)["address"].value_or(std::int64_t{ 0 }) == kMovieObject);
        }
        else if (role == "ps_code")
            codeAddress = (*b)["address"].value_or(std::int64_t{ 0 });
    }
    UNSCOPED_INFO("event 6394 saved the 68-byte ps_header");
    REQUIRE(header.size() == 68);
    for (std::size_t i = 0; i < kMovieHeader.size(); ++i)
    {
        UNSCOPED_INFO("the embedded movie header equals the captured ps_header");
        REQUIRE(be(header, i * 4) == kMovieHeader[i]);
    }
    auto       guest = movie();
    const auto o     = run(guest, kMovieObject, true);
    accepted(o, "captured movie header");
    UNSCOPED_INFO("the captured ps_code follows the literal block by 0x40");
    REQUIRE((o.ranges.size() == 1 && codeAddress == std::int64_t(o.ranges[0].physical) + 0x40));
    std::cout << "shader_resource_literals capture ok\n";
}

} // namespace

TEST_CASE("shader_resource_literals", "[ctest][shader-resource-literals][timeout-60]")
{
    unit();
}

TEST_CASE("shader_resource_literals_capture", "[.][renderer-research][shader-resource-literals][timeout-60]")
{
    capture();
}
