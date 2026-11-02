#include "map_packs.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace
{

using Entry = std::pair<std::string, std::string>;

void putU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
    {
        bytes[offset++] = static_cast<uint8_t>(value >> shift);
    }
}

std::vector<uint8_t> makeArchive(const std::vector<Entry>& entries)
{
    std::vector<uint8_t> bytes(14, 0);
    putU32(bytes, 0, 6);
    bytes[4] = 'F';
    bytes[5] = 'P';
    bytes[6] = 'K';
    bytes[7] = '_';
    putU32(bytes, 10, static_cast<uint32_t>(entries.size()));
    std::vector<size_t> tails;
    for (const auto& [name, data] : entries)
    {
        const size_t at = bytes.size();
        bytes.resize(at + 4 + ((name.size() + 3) & ~size_t{ 3 }) + 16, 0);
        putU32(bytes, at, static_cast<uint32_t>(name.size()));
        std::copy(name.begin(), name.end(), bytes.begin() + at + 4);
        tails.push_back(bytes.size() - 16);
    }
    for (size_t index = 0; index < entries.size(); ++index)
    {
        const auto& data = entries[index].second;
        putU32(bytes, tails[index] + 8, static_cast<uint32_t>(data.size()));
        putU32(bytes, tails[index] + 12, static_cast<uint32_t>(bytes.size()));
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    return bytes;
}

std::string scenarioXml(int id, const std::string& map)
{
    return "<S>\n  <EntryInfo>\n    <MAP>" + map + "</MAP>\n    <ID>" + std::to_string(id) +
           "</ID>\n  </EntryInfo>\n</S>\n";
}

rerevved::map_packs::Candidate parse(std::string id, const std::vector<Entry>& entries)
{
    rerevved::map_packs::Candidate candidate{ std::move(id), {} };
    REQUIRE(rerevved::map_packs::ParseArchive(makeArchive(entries), candidate.contents).empty());
    return candidate;
}

} // namespace

TEST_CASE("map pack archive lists its scenarios", "[ctest][map-packs]")
{
    rerevved::map_packs::Contents contents;
    const auto                    archive = makeArchive(
        { { "DLCScenarioData7.xml", scenarioXml(9000, "Island") }, { "Island.map", "map" } });
    REQUIRE(rerevved::map_packs::ParseArchive(archive, contents).empty());
    CHECK(contents.entryNames == std::vector<std::string>{ "DLCScenarioData7.xml", "Island.map" });
    CHECK(contents.slots == std::vector<uint32_t>{ 7 });
    REQUIRE(contents.scenarios.size() == 1);
    CHECK(contents.scenarios[0].id == 9000);
    CHECK(contents.scenarios[0].map == "Island");
}

TEST_CASE("map pack archive refuses retail slots, IDs and malformed tables", "[ctest][map-packs]")
{
    rerevved::map_packs::Contents contents;
    CHECK_FALSE(rerevved::map_packs::ParseArchive(
                    makeArchive({ { "DLCScenarioData0.xml", scenarioXml(9000, "Q") } }), contents)
                    .empty());
    CHECK_FALSE(rerevved::map_packs::ParseArchive(
                    makeArchive({ { "DLCScenarioData5.xml", scenarioXml(27, "Q") } }), contents)
                    .empty());
    CHECK_FALSE(rerevved::map_packs::ParseArchive(makeArchive({ { "Q.map", "map" } }), contents)
                    .empty());
    CHECK_FALSE(rerevved::map_packs::ParseArchive(
                    makeArchive({ { "DLCScenarioData5.xml", "<S><EntryInfo><MAP>Q</MAP></EntryInfo></S>" } }),
                    contents)
                    .empty());

    CHECK_FALSE(rerevved::map_packs::ParseArchive(
                    makeArchive({ { "DLCScenarioData05.xml", scenarioXml(9000, "Q") } }), contents)
                    .empty());

    auto truncated = makeArchive({ { "DLCScenarioData5.xml", scenarioXml(9000, "Q") } });
    truncated.resize(truncated.size() - 1);
    CHECK_FALSE(rerevved::map_packs::ParseArchive(truncated, contents).empty());
    auto shortTable = makeArchive({ { "DLCScenarioData5.xml", scenarioXml(9000, "Q") } });
    shortTable.resize(30);
    CHECK_FALSE(rerevved::map_packs::ParseArchive(shortTable, contents).empty());
}

TEST_CASE("map pack archive ignores commented scenarios", "[ctest][map-packs]")
{
    rerevved::map_packs::Contents contents;
    const auto                    xml = "<S>\n<!-- <EntryInfo><MAP>Old</MAP><ID>12</ID></EntryInfo> -->\n" +
                                        scenarioXml(9000, "Q") + "</S>\n";
    REQUIRE(rerevved::map_packs::ParseArchive(makeArchive({ { "DLCScenarioData5.xml", xml } }),
                                              contents)
                .empty());
    REQUIRE(contents.scenarios.size() == 1);
    CHECK(contents.scenarios[0].map == "Q");
}

TEST_CASE("map pack content names fit XContent", "[ctest][map-packs]")
{
    CHECK(rerevved::map_packs::ContentName("island") == "rr_island");
    CHECK(rerevved::map_packs::ContentName(std::string(39, 'a')).size() == 42);
    CHECK(rerevved::map_packs::ContentName(std::string(40, 'a')).empty());
    CHECK(rerevved::map_packs::ContentName("a/b").empty());
    CHECK(rerevved::map_packs::ContentName("").empty());
}

TEST_CASE("map packs refuse a clash with an earlier accepted pack", "[ctest][map-packs]")
{
    const std::vector<rerevved::map_packs::Candidate> candidates = {
        parse("first", { { "DLCScenarioData5.xml", scenarioXml(9000, "Q") }, { "Q.map", "a" } }),
        parse("same_map", { { "DLCScenarioData6.xml", scenarioXml(9001, "q") }, { "R.map", "b" } }),
        parse("same_id", { { "DLCScenarioData6.xml", scenarioXml(9000, "R") }, { "R.map", "b" } }),
        parse("same_slot", { { "dlcscenariodata5.xml", scenarioXml(9002, "S") } }),
        parse("second", { { "DLCScenarioData6.xml", scenarioXml(9001, "R") }, { "R.map", "b" } }),
        parse("own_clash", { { "DLCScenarioData7.xml", scenarioXml(9003, "T") + scenarioXml(9004, "T") } }),
    };
    const auto decisions = rerevved::map_packs::SelectPacks(candidates);
    REQUIRE(decisions.size() == 6);
    CHECK_FALSE(decisions[5].accepted);
    CHECK(decisions[5].reason == "MAP T is already provided");
    CHECK(decisions[0].accepted);
    CHECK_FALSE(decisions[1].accepted);
    CHECK(decisions[1].reason == "MAP q is already provided");
    CHECK_FALSE(decisions[2].accepted);
    CHECK(decisions[2].reason == "scenario ID 9000 is already provided");
    CHECK_FALSE(decisions[3].accepted);
    CHECK(decisions[3].reason == "archive entry dlcscenariodata5.xml is already provided");
    CHECK(decisions[4].accepted);
    CHECK(rerevved::map_packs::ContentName("island") == "rr_island");
}

TEST_CASE("map packs hold only their DLC archive", "[ctest][map-packs]")
{
    namespace fs     = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root  = fs::temp_directory_path() / ("rerevved_map_pack_test_" + std::to_string(stamp));
    fs::create_directories(root / "map-pack");
    std::ofstream(root / "map-pack" / "dlc.fpk", std::ios::binary) << "fpk";
    CHECK(rerevved::map_packs::CheckPackFiles(root).empty());

    fs::create_directories(root / "ui");
    std::ofstream(root / "ui" / "logo.dds", std::ios::binary) << "dds";
    CHECK(rerevved::map_packs::CheckPackFiles(root) == "assets/ui/logo.dds is not part of a map pack");

    std::error_code ec;
    fs::remove_all(root, ec);
}
