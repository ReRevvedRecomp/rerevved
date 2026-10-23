#include "host_settings.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace rerevved::host_settings;
using rex::session_data::State;
using rex::session_data::Status;

bool sameMods(const std::vector<ModRef>& left, const std::vector<ModRef>& right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index)
    {
        if (left[index].id != right[index].id || left[index].version != right[index].version ||
            left[index].displayName != right[index].displayName)
        {
            return false;
        }
    }
    return true;
}

HostSettings sample()
{
    HostSettings settings;
    settings.aiCivilizations = 2;
    settings.mods            = { { "hills_production", "1.2.0", "Hills Production" },
                                 { "keshik_movement", "0.4.1", "Keshik Movement" } };
    return settings;
}

void testRoundTrip()
{
    const auto bytes = Encode(sample());
    UNSCOPED_INFO("the encoding fits the channel");
    CHECK(bytes.size() <= rex::session_data::kMaxBytes);
    UNSCOPED_INFO("the first byte is the format version");
    CHECK(bytes[0] == 1);

    const auto decoded = Decode(bytes);
    UNSCOPED_INFO("an encoding decodes");
    CHECK(decoded.has_value());
    if (!decoded)
    {
        return;
    }
    UNSCOPED_INFO("ai_civilizations round trips");
    CHECK(decoded->aiCivilizations == 2);
    UNSCOPED_INFO("a short list is not truncated");
    CHECK(!decoded->modsTruncated);
    UNSCOPED_INFO("mods round trip");
    CHECK(sameMods(decoded->mods, sample().mods));

    const auto empty = Decode(Encode(HostSettings{ 0, {}, false }));
    UNSCOPED_INFO("no mods and no AI round trips");
    CHECK((empty && empty->aiCivilizations == 0 && empty->mods.empty()));
}

void testOrderIsDeterministic()
{
    HostSettings shuffled = sample();
    std::swap(shuffled.mods[0], shuffled.mods[1]);
    UNSCOPED_INFO("mod order does not change the bytes");
    CHECK(Encode(shuffled) == Encode(sample()));

    HostSettings doubled = sample();
    doubled.mods.push_back(doubled.mods[0]);
    UNSCOPED_INFO("a repeated id is encoded once");
    CHECK(Encode(doubled) == Encode(sample()));
}

void testClamping()
{
    HostSettings high    = sample();
    high.aiCivilizations = 9;
    HostSettings low     = sample();
    low.aiCivilizations  = -3;
    UNSCOPED_INFO("an encoded value above 4 becomes 4");
    CHECK(Decode(Encode(high))->aiCivilizations == 4);
    UNSCOPED_INFO("a negative encoded value becomes 0");
    CHECK(Decode(Encode(low))->aiCivilizations == 0);

    auto bytes = Encode(sample());
    bytes[1]   = 200;
    UNSCOPED_INFO("a received byte above 4 becomes 4");
    CHECK(Decode(bytes)->aiCivilizations == 4);
}

void testTruncation()
{
    HostSettings many;
    many.aiCivilizations = 3;
    for (int index = 0; index < 100; ++index)
    {
        std::string id = "mod_" + std::string(index < 10 ? "0" : "") + std::to_string(index);
        many.mods.push_back({ id, "1.0.0", "A Mod Display Name" });
    }
    const auto bytes = Encode(many);
    UNSCOPED_INFO("a long list is cut to fit the channel");
    CHECK(bytes.size() <= rex::session_data::kMaxBytes);

    const auto decoded = Decode(bytes);
    UNSCOPED_INFO("a cut list decodes");
    CHECK(decoded.has_value());
    if (!decoded)
    {
        return;
    }
    UNSCOPED_INFO("a cut list is marked truncated");
    CHECK(decoded->modsTruncated);
    UNSCOPED_INFO("a cut list keeps a prefix");
    REQUIRE((!decoded->mods.empty() && decoded->mods.size() < many.mods.size()));
    UNSCOPED_INFO("the cut keeps the lowest ids");
    CHECK(decoded->mods.front().id == "mod_00");
    UNSCOPED_INFO("a cut list encodes the same way twice");
    CHECK(Encode(many) == bytes);

    HostSettings wide = sample();
    wide.mods.push_back({ "zzz", std::string(300, 'v'), "Too Wide" });
    const auto wideDecoded = Decode(Encode(wide));
    UNSCOPED_INFO("a mod whose field cannot be encoded is dropped and marked truncated");
    CHECK((wideDecoded && wideDecoded->modsTruncated && wideDecoded->mods.size() == 2));
}

void testRejection()
{
    const auto good = Encode(sample());

    UNSCOPED_INFO("empty input is rejected");
    CHECK(!Decode(std::span<const uint8_t>{}).has_value());
    UNSCOPED_INFO("a short header is rejected");
    CHECK(!Decode(std::vector<uint8_t>{ 1, 2, 0 }).has_value());

    auto version = good;
    version[0]   = 2;
    UNSCOPED_INFO("an unknown version is rejected");
    CHECK(!Decode(version).has_value());
    version[0] = 0;
    UNSCOPED_INFO("version 0 is rejected");
    CHECK(!Decode(version).has_value());

    auto flags = good;
    flags[2]   = 0x02;
    UNSCOPED_INFO("a reserved flag bit is rejected");
    CHECK(!Decode(flags).has_value());

    for (size_t length = 4; length < good.size(); ++length)
    {
        const std::vector<uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length));
        UNSCOPED_INFO("every cut-short encoding is rejected");
        CHECK(!Decode(cut).has_value());
    }

    auto trailing = good;
    trailing.push_back(0);
    UNSCOPED_INFO("trailing bytes are rejected");
    CHECK(!Decode(trailing).has_value());

    auto count = good;
    count[3]   = 3;
    UNSCOPED_INFO("a count above the entries is rejected");
    CHECK(!Decode(count).has_value());

    HostSettings unsorted;
    unsorted.mods = { { "b", "1", "" }, { "a", "1", "" } };
    UNSCOPED_INFO("the encoder always sorts");
    CHECK(Decode(Encode(unsorted)).has_value());
    const std::vector<uint8_t> swapped = { 1, 4, 0, 2, 1, 'b', 1, '1', 0, 1, 'a', 1, '1', 0 };
    UNSCOPED_INFO("mods out of id order are rejected");
    CHECK(!Decode(swapped).has_value());
}

void testMismatch()
{
    const std::vector<ModRef> host = { { "hills_production", "1.0", "Hills Production" },
                                       { "keshik_movement", "1.0", "Keshik Movement" } };

    UNSCOPED_INFO("equal lists have no mismatch");
    CHECK(DescribeModMismatch(host, host).empty());
    UNSCOPED_INFO("no mods on either side have no mismatch");
    CHECK(DescribeModMismatch({}, {}).empty());

    const std::vector<ModRef> reversed = { host[1], host[0] };
    UNSCOPED_INFO("list order is not a mismatch");
    CHECK(DescribeModMismatch(host, reversed).empty());

    std::vector<ModRef> renamed = host;
    renamed[0].displayName      = "Another Name";
    UNSCOPED_INFO("a display name is not a mismatch");
    CHECK(DescribeModMismatch(host, renamed).empty());

    UNSCOPED_INFO("a mod missing on the peer names it");
    CHECK(DescribeModMismatch(host, { host[1] }) == "Hills Production: host on, you off");
    UNSCOPED_INFO("a mod extra on the peer names it");
    CHECK(DescribeModMismatch({ host[1] }, host) == "Hills Production: host off, you on");

    std::vector<ModRef> older = host;
    older[1].version          = "0.9";
    UNSCOPED_INFO("a version difference names it");
    CHECK(DescribeModMismatch(host, older) == "Keshik Movement: different version");

    UNSCOPED_INFO("several differences are listed by id");
    CHECK(DescribeModMismatch(host, {}) == "Hills Production: host on, you off; Keshik Movement: host on, you off");

    const std::vector<ModRef> hostWithoutNames = { { "hills_production", "1.0", "" } };
    UNSCOPED_INFO("a mod without a display name shows its id");
    CHECK(DescribeModMismatch(hostWithoutNames, {}) == "hills_production: host on, you off");
    UNSCOPED_INFO("a missing display name on one side is not a mismatch");
    CHECK(DescribeModMismatch(hostWithoutNames, { { "hills_production", "1.0", "Hills Production" } }).empty());

    UNSCOPED_INFO("a truncated host list hides peer-only mods but says it was not fully checked");
    CHECK(DescribeModMismatch({ host[1] }, host, true) == "The host's full mod list could not be checked");
    UNSCOPED_INFO("matching retained mods on a truncated host list still say it was not fully checked");
    CHECK(DescribeModMismatch(host, host, true) == "The host's full mod list could not be checked");
    UNSCOPED_INFO("a truncated host list reports host-only mods and the unchecked note");
    CHECK(DescribeModMismatch(host, { host[1] }, true) ==
          "Hills Production: host on, you off; the host's full mod list could not be checked");
    UNSCOPED_INFO("a complete matching host list says nothing");
    CHECK(DescribeModMismatch(host, host, false).empty());

    std::vector<ModRef> many;
    for (const char* id : { "a", "b", "c", "d", "e", "f" })
    {
        many.push_back({ id, "1", "" });
    }
    UNSCOPED_INFO("a long list of differences is cut");
    CHECK(DescribeModMismatch(many, {}) == "a: host on, you off; b: host on, you off; c: host on, you off; "
                                           "d: host on, you off; and 2 more");
}

void testLocalMods()
{
    UNSCOPED_INFO("no mods are published at first");
    CHECK(LocalMods().empty());
    PublishLocalMods(sample().mods);
    UNSCOPED_INFO("published mods are returned");
    CHECK(sameMods(LocalMods(), sample().mods));
    PublishLocalMods({});
    UNSCOPED_INFO("a later publish replaces the snapshot");
    CHECK(LocalMods().empty());
}

Status statusWith(State state, const std::vector<uint8_t>& bytes)
{
    Status status;
    status.state = state;
    status.bytes = bytes;
    return status;
}

void testResolveAiLimit()
{
    const auto  host = Encode(sample());
    std::string source;

    UNSCOPED_INFO("a single-player game uses the local value");
    CHECK((ResolveAiLimit(false, statusWith(State::kNone, {}), 3, &source) == 3 && source == "local"));
    UNSCOPED_INFO("a stale received value does not reach a single-player game");
    CHECK((ResolveAiLimit(false, statusWith(State::kReceived, host), 3, &source) == 3 && source == "local"));
    UNSCOPED_INFO("a stale hosted value does not reach a single-player game");
    CHECK((ResolveAiLimit(false, statusWith(State::kHost, host), 3, &source) == 3 && source == "local"));

    UNSCOPED_INFO("a host uses its own frozen value");
    CHECK((ResolveAiLimit(true, statusWith(State::kHost, host), 4, &source) == 2 && source == "host"));
    UNSCOPED_INFO("a peer uses the host's value");
    CHECK((ResolveAiLimit(true, statusWith(State::kReceived, host), 4, &source) == 2 && source == "received"));
    UNSCOPED_INFO("a peer with a lower own value still uses the host's");
    CHECK((ResolveAiLimit(true, statusWith(State::kReceived, host), 0, &source) == 2 && source == "received"));

    UNSCOPED_INFO("an unavailable host value falls back to the local one");
    CHECK((ResolveAiLimit(true, statusWith(State::kUnavailable, {}), 1, &source) == 1 && source == "fallback"));
    UNSCOPED_INFO("a host value still pending falls back to the local one");
    CHECK((ResolveAiLimit(true, statusWith(State::kPending, {}), 1, &source) == 1 && source == "fallback"));
    UNSCOPED_INFO("a network game with no session data falls back to the local value");
    CHECK((ResolveAiLimit(true, statusWith(State::kNone, {}), 1, &source) == 1 && source == "fallback"));

    UNSCOPED_INFO("a malformed host value falls back to the local one");
    CHECK((ResolveAiLimit(true, statusWith(State::kReceived, { 9, 9, 9 }), 1, &source) == 1 && source == "fallback"));
    UNSCOPED_INFO("an empty host value falls back to the local one");
    CHECK((ResolveAiLimit(true, statusWith(State::kReceived, {}), 1, &source) == 1 && source == "fallback"));

    UNSCOPED_INFO("a null source is accepted");
    CHECK(ResolveAiLimit(true, statusWith(State::kReceived, host), 4, nullptr) == 2);
}

} // namespace

TEST_CASE("host_settings", "[ctest][host-settings][local-fast]")
{
    testRoundTrip();
    testOrderIsDeterministic();
    testClamping();
    testTruncation();
    testRejection();
    testMismatch();
    testLocalMods();
    testResolveAiLimit();
}
