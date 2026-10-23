// Gamma hooks (0x8268FE08, 0x826A7040) must count/return without a kernel.

#include "developer_checks.hpp"
#include "gpu/guest_d3d/gamma_observer.h"

#include <rex/cvar.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

DEV_CVAR_DECLARE(bool, native_gamma_observe_check);
REXCVAR_DECLARE(bool, native_gamma_apply);

using namespace rerevved::gpu::guest_d3d;

namespace
{

constexpr std::uint32_t kDeviceA = 0x40010000, kDeviceB = 0x40020000;

constexpr std::uint32_t pack(std::uint32_t red, std::uint32_t green, std::uint32_t blue)
{
    return (red << 20) | (green << 10) | blue;
}

// Three channels that differ everywhere: no swap of two channels, and no
// ramp that is the same in all three, reproduces it.
GuestGammaTable asymmetricTable(std::uint32_t seed = 0)
{
    GuestGammaTable table{};
    for (std::uint32_t i = 0; i < table.size(); ++i)
    {
        table[i] = pack(((i + seed) * 3) & 1023, ((i + seed) * 5 + 1) & 1023, ((i + seed) * 7 + 2) & 1023);
    }
    return table;
}

// The setter's layout: red, green, then blue, each 256 big-endian halfwords holding value << 6.
std::vector<std::uint8_t> convert(const GuestGammaTable& table)
{
    std::vector<std::uint8_t> bytes(kGuestGammaBytes);
    for (std::size_t i = 0; i < table.size(); ++i)
    {
        const std::uint32_t channels[3] = { (table[i] >> 20) & 1023, (table[i] >> 10) & 1023, table[i] & 1023 };
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            const std::uint32_t half         = channels[channel] << 6;
            bytes[channel * 512 + i * 2]     = static_cast<std::uint8_t>(half >> 8);
            bytes[channel * 512 + i * 2 + 1] = static_cast<std::uint8_t>(half);
        }
    }
    return bytes;
}

// One emission the way the hook sees it: the emitter's table, the device copy, and its return.
void emit(GammaObserver& observer, std::uint32_t device, const GuestGammaTable& table, bool secondSite = false)
{
    const auto bytes  = convert(table);
    const auto ticket = observer.BeginEmission(device, bytes, bytes, secondSite);
    UNSCOPED_INFO("an emission begins with a device");
    REQUIRE(ticket != 0);
    observer.EndEmission(ticket);
}

void setter(GammaObserver& observer, std::uint32_t device, const GuestGammaTable& table)
{
    const auto outcome = observer.NoteSetter(device, convert(table));
    UNSCOPED_INFO("the setter table decodes");
    REQUIRE(DEV_CHECK(outcome.decoded));
}

void testDecode()
{
    // Entries placed by hand: red of entry 0, green of entry 5 and blue of entry 255.
    std::vector<std::uint8_t> bytes(kGuestGammaBytes);
    bytes[0]                  = 0xFF;
    bytes[1]                  = 0xC0;
    bytes[512 + 5 * 2]        = 0x40;
    bytes[512 + 5 * 2 + 1]    = 0x00;
    bytes[1024 + 255 * 2]     = 0x00;
    bytes[1024 + 255 * 2 + 1] = 0x40;
    GuestGammaTable table{};
    UNSCOPED_INFO("a table of aligned halfwords decodes");
    REQUIRE(DecodeGuestGammaTable(bytes, table) == GammaDecode::Ok);
    UNSCOPED_INFO("red is the first channel, green the second, blue the third");
    REQUIRE((table[0] == pack(0x3FF, 0, 0) && table[5] == pack(0, 0x100, 0) && table[255] == pack(0, 0, 1)));
    UNSCOPED_INFO("unset entries are zero");
    REQUIRE((table[1] == 0 && table[254] == 0));

    const auto      asymmetric = asymmetricTable();
    GuestGammaTable round{};
    UNSCOPED_INFO("three different channels round-trip");
    REQUIRE((DecodeGuestGammaTable(convert(asymmetric), round) == GammaDecode::Ok && round == asymmetric));
    UNSCOPED_INFO("the fixture channels differ");
    REQUIRE((((asymmetric[1] >> 20) & 1023) != ((asymmetric[1] >> 10) & 1023) && ((asymmetric[1] >> 10) & 1023) != (asymmetric[1] & 1023)));

    // Any of the six bits below the ten kept, in the low byte of a halfword of any channel, makes it unaligned.
    for (const std::size_t offset : { std::size_t(1), std::size_t(512 + 101), std::size_t(1024 + 511) })
    {
        for (const std::uint8_t low : { std::uint8_t(0x01), std::uint8_t(0x20), std::uint8_t(0x3F) })
        {
            auto bad = convert(asymmetric);
            bad[offset] |= low;
            GuestGammaTable out = asymmetric;
            UNSCOPED_INFO("a low bit fails the decode");
            REQUIRE(DecodeGuestGammaTable(bad, out) == GammaDecode::Unaligned);
            UNSCOPED_INFO("a failed decode leaves an empty table");
            REQUIRE(out == GuestGammaTable{});
        }
    }
    auto high = convert(asymmetric);
    high[0]   = 0xFF;
    high[1]   = 0xC0;
    UNSCOPED_INFO("the top bit of a halfword is a table bit");
    REQUIRE(DecodeGuestGammaTable(high, round) == GammaDecode::Ok);

    GuestGammaTable out = asymmetric;
    UNSCOPED_INFO("short bytes fail");
    REQUIRE((DecodeGuestGammaTable(std::vector<std::uint8_t>(kGuestGammaBytes - 1), out) == GammaDecode::BadSize && out == GuestGammaTable{}));
    UNSCOPED_INFO("long bytes fail");
    REQUIRE(DecodeGuestGammaTable(std::vector<std::uint8_t>(kGuestGammaBytes + 1), out) == GammaDecode::BadSize);
    UNSCOPED_INFO("no bytes fail");
    REQUIRE(DecodeGuestGammaTable({}, out) == GammaDecode::BadSize);
}

void testLinearDefaultAndHash()
{
    const auto& linear = LinearGuestGammaTable();
    UNSCOPED_INFO("the power-on ramp is i * 0x3FF / 0xFF in every channel");
    REQUIRE((linear[0] == 0 && linear[1] == pack(4, 4, 4) && linear[2] == pack(8, 8, 8) && linear[128] == pack(513, 513, 513) && linear[255] == pack(1023, 1023, 1023)));
    // SHA-256 of the 1024 little-endian bytes of this table, from an independent computation.
    GuestGammaTable table{};
    for (std::uint32_t i = 0; i < table.size(); ++i)
    {
        table[i] = pack((i * 3) & 1023, (i * 5 + 1) & 1023, (i * 7 + 2) & 1023);
    }
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("the table hash is over its packed little-endian bytes: " + GuestGammaTableSha256(table));
    REQUIRE(GuestGammaTableSha256(table) == "24950bbab8e67f12fa6f97f877781ac8c0b39d6194d1f7d23a04ea93706cc864");
    auto other = table;
    other[255] ^= 1;
    UNSCOPED_INFO("one bit changes the hash");
    REQUIRE(GuestGammaTableSha256(other) != GuestGammaTableSha256(table));
    UNSCOPED_INFO("differences count entries, not bits");
    REQUIRE((CountGammaDifferences(table, other) == 1 && CountGammaDifferences(table, table) == 0));
#endif
}

#if REREVVED_DEVELOPER
void testSetter()
{
    GammaObserver observer;
    const auto    a     = asymmetricTable();
    auto          first = observer.NoteSetter(kDeviceA, convert(a));
    UNSCOPED_INFO("the first setter decodes and is a change");
    REQUIRE((first.decoded && first.changed && first.table == a));
    auto again = observer.NoteSetter(kDeviceA, convert(a));
    UNSCOPED_INFO("the same table is not a change");
    REQUIRE((again.decoded && !again.changed));
    auto next = observer.NoteSetter(kDeviceA, convert(asymmetricTable(1)));
    UNSCOPED_INFO("another table is a change");
    REQUIRE((next.decoded && next.changed && next.table == asymmetricTable(1)));

    // A table with a low bit set is refused and does not replace the value or count as a change.
    auto bad = convert(asymmetricTable(2));
    bad[11] |= 1;
    auto refused = observer.NoteSetter(kDeviceA, bad);
    UNSCOPED_INFO("an unaligned table is refused");
    REQUIRE((!refused.decoded && !refused.changed));
    UNSCOPED_INFO("the refused table did not replace the setter value");
    REQUIRE(observer.NoteSetter(kDeviceA, convert(asymmetricTable(1))).changed == false);
    UNSCOPED_INFO("unreadable bytes are refused");
    REQUIRE(!observer.NoteSetter(kDeviceA, {}).decoded);
    UNSCOPED_INFO("no device is refused");
    REQUIRE(!observer.NoteSetter(0, convert(a)).decoded);
    observer.NotePwlSetter(kDeviceA);

    const auto counts = observer.Take();
    UNSCOPED_INFO("setter counts: " + FormatGammaCounts(counts));
    REQUIRE((counts.setter == 7 && counts.setterChanged == 2 && counts.setterDecodeFail == 2 && counts.pwlSetter == 1 && counts.errors == 1));
}
#endif

void testDefaultBeforeFirstEmission()
{
    GammaObserver observer;
    const auto    identity = asymmetricTable(9);
    // The setter alone changes nothing the display shows.
    setter(observer, kDeviceA, asymmetricTable());
    const auto view = observer.BeginSwap(identity);
    UNSCOPED_INFO("before the first emission the table is the linear default, observed");
    REQUIRE((view.table == LinearGuestGammaTable() && !view.assumed && view.emissionSequence == 0));
#if REREVVED_DEVELOPER
    auto counts = observer.Take();
    UNSCOPED_INFO("the swap is before an emission and the setter waits: " + FormatGammaCounts(counts));
    REQUIRE((counts.swap == 1 && counts.swapBeforeEmit == 1 && counts.swapPendingSetter == 1));
    UNSCOPED_INFO("the gauge compares the table in effect");
    REQUIRE((counts.identityDiffEntries == CountGammaDifferences(LinearGuestGammaTable(), identity) && counts.identityDiffEntries > 0));
#endif

    // Without a setter either, the display still holds the default.
    GammaObserver fresh;
    const auto    quiet = fresh.BeginSwap(identity);
    UNSCOPED_INFO("no hook at all still reads the linear default");
    REQUIRE((quiet.table == LinearGuestGammaTable() && !quiet.assumed));
#if REREVVED_DEVELOPER
    const auto quietCounts = fresh.Take();
    UNSCOPED_INFO("no setter, no pending setter");
    REQUIRE((quietCounts.swapBeforeEmit == 1 && quietCounts.swapPendingSetter == 0));
#endif

    // ChooseSwapGamma: on takes the observed table, off keeps the assumed identity.
    const auto on = ChooseSwapGamma(true, view, identity);
    UNSCOPED_INFO("apply uses the default as observed, not assumed");
    REQUIRE((on.table == LinearGuestGammaTable() && !on.assumed));
    const auto off = ChooseSwapGamma(false, view, identity);
    UNSCOPED_INFO("without apply the identity ramp stays assumed");
    REQUIRE((off.table == identity && off.assumed));
    const auto none = ChooseSwapGamma(true, std::nullopt, identity);
    UNSCOPED_INFO("apply without a view has nothing to apply");
    REQUIRE((none.table == identity && none.assumed));
}

void testSetterNewerThanEmission()
{
    GammaObserver observer;
    const auto    identity = LinearGuestGammaTable();
    const auto    a = asymmetricTable(), b = asymmetricTable(1), c = asymmetricTable(2);

    setter(observer, kDeviceA, a);
    emit(observer, kDeviceA, a);
    auto view = observer.BeginSwap(identity);
    UNSCOPED_INFO("the emitted table is in effect");
    REQUIRE((view.table == a && !view.assumed && view.emissionSequence == 1));
    observer.EndSwap(view, false);
#if REREVVED_DEVELOPER
    auto counts = observer.Take();
    UNSCOPED_INFO("setter and emission agree: " + FormatGammaCounts(counts));
    REQUIRE((counts.swapPendingSetter == 0 && counts.swapBeforeEmit == 0 && counts.swapTableChanged == 0));
#endif

    // A newer setter value is not in effect until its emission returns.
    setter(observer, kDeviceA, b);
    view = observer.BeginSwap(identity);
    UNSCOPED_INFO("the setter value alone is not in effect");
    REQUIRE(view.table == a);
    observer.EndSwap(view, false);
#if REREVVED_DEVELOPER
    counts = observer.Take();
    UNSCOPED_INFO("the newer setter is pending: " + FormatGammaCounts(counts));
    REQUIRE((counts.swapPendingSetter == 1 && counts.swapTableChanged == 0));
#endif

    // An emission begun but not returned is not in effect either.
    const auto bytesB = convert(b);
    const auto ticket = observer.BeginEmission(kDeviceA, bytesB, bytesB, false);
    view              = observer.BeginSwap(identity);
    UNSCOPED_INFO("a pending emission is not in effect");
    REQUIRE((view.table == a && view.emissionSequence == 1));
    observer.EndSwap(view, false);
    observer.EndEmission(ticket);
    view = observer.BeginSwap(identity);
    UNSCOPED_INFO("the returned emission is in effect");
    REQUIRE((view.table == b && view.emissionSequence == 2));
    observer.EndSwap(view, false);
#if REREVVED_DEVELOPER
    counts = observer.Take();
    UNSCOPED_INFO("the second swap still sees the pending setter, the third a changed table: " + FormatGammaCounts(counts));
    REQUIRE((counts.emit == 1 && counts.emitComplete == 1 && counts.swapPendingSetter == 1 && counts.swapTableChanged == 1));
#endif

    // A third value after the emission is pending again, and the emitted one stays.
    setter(observer, kDeviceA, c);
    view = observer.BeginSwap(identity);
    UNSCOPED_INFO("the last completed emission stays in effect");
    REQUIRE(view.table == b);
#if REREVVED_DEVELOPER
    counts = observer.Take();
    UNSCOPED_INFO("pending again");
    REQUIRE(counts.swapPendingSetter == 1);
#endif
}

void testEmissionRetainedAndSites()
{
    GammaObserver observer;
    const auto    a     = asymmetricTable();
    auto          bytes = convert(a);
    // Equal to the device copy at the primary site.
    observer.EndEmission(observer.BeginEmission(kDeviceA, bytes, bytes, false));
    // A different device copy, at the second site.
    auto retained = bytes;
    retained[100] ^= 0x40;
    observer.EndEmission(observer.BeginEmission(kDeviceA, bytes, retained, true));
    // Unreadable bytes are an error, not a verdict on the copy; an undecodable table completes but is unknown.
    auto ticket = observer.BeginEmission(kDeviceA, {}, bytes, false);
    UNSCOPED_INFO("an unreadable table still begins");
    REQUIRE(ticket != 0);
    observer.EndEmission(ticket);
    auto view = observer.BeginSwap(a);
    UNSCOPED_INFO("an emission the observer cannot decode leaves the table unknown and the identity assumed");
    REQUIRE((view.assumed && view.table == a && view.emissionSequence == 3));
    UNSCOPED_INFO("apply passes the assumption on");
    REQUIRE(ChooseSwapGamma(true, view, a).assumed);
    observer.EndSwap(view, false);
    // The next decodable emission restores the table.
    emit(observer, kDeviceA, asymmetricTable(3));
    view = observer.BeginSwap(a);
    UNSCOPED_INFO("a decodable emission names the table again");
    REQUIRE((!view.assumed && view.table == asymmetricTable(3)));

    // A zero device is refused.
    UNSCOPED_INFO("no device begins nothing");
    REQUIRE(observer.BeginEmission(0, bytes, bytes, false) == 0);
    // Ticket zero was never issued: an error, and the LUT is no longer known.
    observer.EndEmission(0);
    UNSCOPED_INFO("a return with no ticket leaves the table unknown");
    REQUIRE(observer.BeginSwap(a).assumed);

#if REREVVED_DEVELOPER
    const auto counts = observer.Take();
    UNSCOPED_INFO("emission counts: " + FormatGammaCounts(counts));
    REQUIRE((counts.emit == 5 && counts.emitComplete == 4 && counts.emitRetainedEqual == 2 && counts.emitRetainedMismatch == 1 && counts.emitSecondSite == 1 &&
             counts.emitDecodeFail == 1 && counts.errors == 3 && counts.emitEvicted == 0));
#endif
}

void testOverlap()
{
    const auto a = asymmetricTable(), b = asymmetricTable(1), c = asymmetricTable(2);
    const auto bytesA = convert(a), bytesB = convert(b), bytesC = convert(c);
    const auto linear = LinearGuestGammaTable();

    // A begins, B begins, A returns: A is in effect, not the default and not B.
    GammaObserver observer;
    const auto    first  = observer.BeginEmission(kDeviceA, bytesA, bytesA, false);
    const auto    second = observer.BeginEmission(kDeviceA, bytesB, bytesB, false);
    UNSCOPED_INFO("each emission has its own ticket");
    REQUIRE((first != 0 && second != 0 && first != second));
    auto view = observer.BeginSwap(linear);
    UNSCOPED_INFO("nothing has returned yet");
    REQUIRE((view.emissionSequence == 0 && view.table == linear));
    observer.EndEmission(first);
    view = observer.BeginSwap(linear);
    UNSCOPED_INFO("an emission that returned stays in effect while a later one is in flight");
    REQUIRE((view.table == a && !view.assumed && view.emissionSequence == 1));
    observer.EndEmission(second);
    view = observer.BeginSwap(linear);
    UNSCOPED_INFO("the next to return is in effect");
    REQUIRE((view.table == b && view.emissionSequence == 2));
    // A ticket that already returned is not in flight: its second return is a return the observer does not follow.
    observer.EndEmission(second);
    UNSCOPED_INFO("a second return of a ticket leaves the table unknown");
    REQUIRE(observer.BeginSwap(linear).assumed);
    observer.EndEmission(first);
#if REREVVED_DEVELOPER
    auto counts = observer.Take();
    UNSCOPED_INFO("overlap counts: " + FormatGammaCounts(counts));
    REQUIRE((counts.emit == 2 && counts.emitComplete == 2 && counts.emitOverlap == 1));
#endif

    // B returns before A: the last to return wins, so A.
    GammaObserver reversed;
    const auto    ra = reversed.BeginEmission(kDeviceA, bytesA, bytesA, false);
    const auto    rb = reversed.BeginEmission(kDeviceA, bytesB, bytesB, false);
    reversed.EndEmission(rb);
    UNSCOPED_INFO("the returned emission is in effect");
    REQUIRE(reversed.BeginSwap(linear).table == b);
    reversed.EndEmission(ra);
    view = reversed.BeginSwap(linear);
    UNSCOPED_INFO("the emission that returned last is in effect");
    REQUIRE((view.table == a && view.emissionSequence == 2));

    // Three in flight, returning out of order.
    GammaObserver three;
    const auto    ta = three.BeginEmission(kDeviceA, bytesA, bytesA, false);
    const auto    tb = three.BeginEmission(kDeviceA, bytesB, bytesB, false);
    const auto    tc = three.BeginEmission(kDeviceA, bytesC, bytesC, false);
    three.EndEmission(tb);
    three.EndEmission(tc);
    three.EndEmission(ta);
    UNSCOPED_INFO("completion order, not begin order, decides");
    REQUIRE(three.BeginSwap(linear).table == a);
#if REREVVED_DEVELOPER
    counts = three.Take();
    UNSCOPED_INFO("two begins overlapped: " + FormatGammaCounts(counts));
    REQUIRE((counts.emitOverlap == 2 && counts.emitComplete == 3));
#endif

    // A begin never discards a completed emission, and an undecodable one that returned last leaves the LUT unknown.
    GammaObserver mixed;
    emit(mixed, kDeviceA, a);
    const auto bad = mixed.BeginEmission(kDeviceA, {}, bytesA, false);
    view           = mixed.BeginSwap(linear);
    UNSCOPED_INFO("a begun emission leaves the completed one alone");
    REQUIRE((view.table == a && !view.assumed));
    mixed.EndEmission(bad);
    UNSCOPED_INFO("an undecodable emission that returned is unknown");
    REQUIRE(mixed.BeginSwap(linear).assumed);
}

// Only an emission begun after the late return restores known state.
void testEvictedEmission()
{
    static_assert(kMaxEmissionsInFlight == 8, "the sequence below is nine begins");
    const auto                   linear = LinearGuestGammaTable();
    std::vector<GuestGammaTable> tables;
    for (std::uint32_t i = 0; i < 9; ++i)
    {
        tables.push_back(asymmetricTable(10 + i));
    }
    const auto begin = [&](GammaObserver& observer, std::uint32_t index)
    {
        const auto bytes = convert(tables[index]);
        return observer.BeginEmission(kDeviceA, bytes, bytes, false);
    };

    GammaObserver              observer;
    std::vector<std::uint64_t> tickets;
    for (std::uint32_t i = 0; i < 9; ++i)
    {
        tickets.push_back(begin(observer, i));
    }
    for (std::uint32_t i = 1; i < 9; ++i)
    {
        observer.EndEmission(tickets[i]);
    }
    auto view = observer.BeginSwap(linear);
    UNSCOPED_INFO("the eight that stayed in flight returned: the last is in effect");
    REQUIRE((view.table == tables[8] && !view.assumed && view.emissionSequence == 8));
    // The evicted emission returns last: its table is gone, so the LUT is unknown.
    observer.EndEmission(tickets[0]);
    view = observer.BeginSwap(linear);
    UNSCOPED_INFO("an evicted emission that returned leaves the LUT unknown");
    REQUIRE((view.assumed && view.emissionSequence == 9));
    UNSCOPED_INFO("apply falls back to the assumed identity ramp");
    REQUIRE(ChooseSwapGamma(true, view, linear).assumed);
#if REREVVED_DEVELOPER
    auto counts = observer.Take();
    UNSCOPED_INFO("eviction counts: " + FormatGammaCounts(counts));
    REQUIRE((counts.emit == 9 && counts.emitComplete == 8 && counts.emitEvicted == 1 && counts.emitOverlap == 8));
#endif
    // A fresh emission begun and returned afterwards names the LUT again.
    emit(observer, kDeviceA, tables[0]);
    view = observer.BeginSwap(linear);
    UNSCOPED_INFO("a later emission restores a known table");
    REQUIRE((view.table == tables[0] && !view.assumed && view.emissionSequence == 10));

    // An emission that began before the evicted one returned cannot restore it.
    GammaObserver              second;
    std::vector<std::uint64_t> early;
    for (std::uint32_t i = 0; i < 9; ++i)
    {
        early.push_back(begin(second, i));
    }
    second.EndEmission(early[0]);
    second.EndEmission(early[8]);
    view = second.BeginSwap(linear);
    UNSCOPED_INFO("an emission that began before the evicted return does not restore the table");
    REQUIRE(view.assumed);
    emit(second, kDeviceA, tables[3]);
    UNSCOPED_INFO("one that began afterwards does");
    REQUIRE(second.BeginSwap(linear).table == tables[3]);

    // The evicted return can also come after a later emission completed.
    GammaObserver              third;
    std::vector<std::uint64_t> late;
    for (std::uint32_t i = 0; i < 9; ++i)
    {
        late.push_back(begin(third, i));
    }
    for (std::uint32_t i = 1; i < 9; ++i)
    {
        third.EndEmission(late[i]);
    }
    emit(third, kDeviceA, tables[5]);
    UNSCOPED_INFO("a fresh emission is in effect");
    REQUIRE(third.BeginSwap(linear).table == tables[5]);
    third.EndEmission(late[0]);
    UNSCOPED_INFO("the late return makes the table unknown again");
    REQUIRE(third.BeginSwap(linear).assumed);

    // Eight in flight is within the bound.
    GammaObserver              within;
    std::vector<std::uint64_t> eight;
    for (std::uint32_t i = 0; i < 8; ++i)
    {
        eight.push_back(begin(within, i));
    }
    for (std::uint32_t i = 8; i-- > 0;)
    {
        within.EndEmission(eight[i]);
    }
    view = within.BeginSwap(linear);
    UNSCOPED_INFO("eight in flight evict nothing");
    REQUIRE((view.table == tables[0] && !view.assumed && DEV_CHECK(within.Take().emitEvicted == 0)));
}

// An evicted ticket below the next issue still identifies a late return.
void testEvictedBeyondAnyList()
{
    constexpr std::uint32_t kBegins = 1033;
    const auto              linear  = LinearGuestGammaTable();
    const auto              tableA = asymmetricTable(1), tableB = asymmetricTable(2);
    const auto              bytesA = convert(tableA), bytesB = convert(tableB);

    GammaObserver              observer;
    std::vector<std::uint64_t> tickets;
    tickets.push_back(observer.BeginEmission(kDeviceA, bytesA, bytesA, false));
    for (std::uint32_t i = 1; i + 1 < kBegins; ++i)
    {
        tickets.push_back(observer.BeginEmission(kDeviceA, bytesA, bytesA, false));
    }
    tickets.push_back(observer.BeginEmission(kDeviceA, bytesB, bytesB, false));
    UNSCOPED_INFO("tickets are issued in order");
    REQUIRE((tickets.size() == kBegins && tickets.front() == 1 && tickets.back() == kBegins));
    observer.EndEmission(tickets.back());
    auto view = observer.BeginSwap(linear);
    UNSCOPED_INFO("the last begin returned first and is in effect");
    REQUIRE((view.table == tableB && !view.assumed));
    // The first returns last, with table A: B is stale.
    observer.EndEmission(tickets.front());
    view = observer.BeginSwap(linear);
    UNSCOPED_INFO("the return of the first, long evicted, leaves the table unknown");
    REQUIRE((view.assumed && ChooseSwapGamma(true, view, linear).assumed));
#if REREVVED_DEVELOPER
    auto counts = observer.Take();
    UNSCOPED_INFO("one evicted return, no error: " + FormatGammaCounts(counts));
    REQUIRE((counts.emitEvicted == 1 && counts.errors == 0));
#endif
    // Another early ticket returning later is the same.
    observer.EndEmission(tickets[100]);
    UNSCOPED_INFO("each return of an evicted ticket counts");
    REQUIRE(DEV_CHECK(observer.Take().emitEvicted == 1));
    // Only an emission that began after the last such return restores the table.
    const auto early = observer.BeginEmission(kDeviceA, bytesB, bytesB, false);
    observer.EndEmission(tickets[200]);
    observer.EndEmission(early);
    UNSCOPED_INFO("an emission begun before the stale return does not restore the table");
    REQUIRE(observer.BeginSwap(linear).assumed);
    emit(observer, kDeviceA, tableA);
    view = observer.BeginSwap(linear);
    UNSCOPED_INFO("a later emission restores a known table");
    REQUIRE((view.table == tableA && !view.assumed));

    // A ticket the observer never issued is an error that leaves the table unknown.
    DEV_DO(observer.Take());
    observer.EndEmission(observer.BeginEmission(kDeviceA, bytesB, bytesB, false) + 1);
    UNSCOPED_INFO("a ticket from the future leaves the table unknown");
    REQUIRE(observer.BeginSwap(linear).assumed);
#if REREVVED_DEVELOPER
    counts = observer.Take();
    UNSCOPED_INFO("a future ticket is an error: " + FormatGammaCounts(counts));
    REQUIRE((counts.errors == 1 && counts.emitEvicted == 0));
#endif
}

void testDeviceChange()
{
    GammaObserver observer;
    const auto    a = asymmetricTable(), b = asymmetricTable(1);
    setter(observer, kDeviceA, a);
    emit(observer, kDeviceA, a);
    // Another device restarts the setter value; the display LUT it left stays.
    UNSCOPED_INFO("the same table from another device is a first setter");
    const auto moved = observer.NoteSetter(kDeviceB, convert(a));
    REQUIRE(DEV_CHECK(moved.changed));
    auto view = observer.BeginSwap(b);
    UNSCOPED_INFO("the completed emission survives a device change");
    REQUIRE((view.table == a && view.emissionSequence == 1));
#if REREVVED_DEVELOPER
    auto counts = observer.Take();
    UNSCOPED_INFO("device change counts: " + FormatGammaCounts(counts));
    REQUIRE((counts.deviceChange == 1 && counts.setterChanged == 2 && counts.swapPendingSetter == 0));
#endif

    // An emission in flight on the old device is dropped.
    const auto bytes  = convert(b);
    const auto ticket = observer.BeginEmission(kDeviceB, bytes, bytes, false);
    observer.NotePwlSetter(kDeviceA);
    observer.EndEmission(ticket);
    view = observer.BeginSwap(a);
    UNSCOPED_INFO("an emission whose device left wrote the LUT with a table the observer dropped");
    REQUIRE((view.assumed && view.emissionSequence == 2));
#if REREVVED_DEVELOPER
    counts = observer.Take();
    UNSCOPED_INFO("second device change: " + FormatGammaCounts(counts));
    REQUIRE((counts.deviceChange == 1 && counts.emitComplete == 0 && counts.emitEvicted == 1));
#endif

    // The first device seen is not a change.
    GammaObserver fresh;
    setter(fresh, kDeviceB, a);
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("the first device is not a change");
    REQUIRE(fresh.Take().deviceChange == 0);
#endif
}

#if REREVVED_DEVELOPER
void testSwapCounts()
{
    GammaObserver observer;
    const auto    identity = asymmetricTable(9);
    const auto    a = asymmetricTable(), b = asymmetricTable(1);

    // A swap with work, then an empty swap with nothing new.
    auto view = observer.BeginSwap(identity);
    observer.EndSwap(view, false);
    view = observer.BeginSwap(identity);
    observer.EndSwap(view, true);
    auto counts = observer.Take();
    UNSCOPED_INFO("an empty swap with no emission is unchanged: " + FormatGammaCounts(counts));
    REQUIRE((counts.swap == 2 && counts.noworkSwapChanged == 0));

    // An emission, then an empty swap: the emission moved since the last presented swap.
    emit(observer, kDeviceA, a);
    view = observer.BeginSwap(identity);
    observer.EndSwap(view, true);
    view = observer.BeginSwap(identity);
    observer.EndSwap(view, true);
    counts = observer.Take();
    UNSCOPED_INFO("both empty swaps follow the moved emission: " + FormatGammaCounts(counts));
    REQUIRE((counts.noworkSwapChanged == 2 && counts.swapTableChanged == 1));

    // A swap with work takes the emission as presented.
    view = observer.BeginSwap(identity);
    observer.EndSwap(view, false);
    view = observer.BeginSwap(identity);
    observer.EndSwap(view, true);
    counts = observer.Take();
    UNSCOPED_INFO("an empty swap after a presented one is unchanged: " + FormatGammaCounts(counts));
    REQUIRE(counts.noworkSwapChanged == 0);

    // The gauge survives the take that reports it and follows the table in effect.
    UNSCOPED_INFO("the gauge compares with the identity table");
    REQUIRE((counts.identityDiffEntries == CountGammaDifferences(a, identity) && counts.identityDiffEntries > 0));
    UNSCOPED_INFO("an empty window keeps the gauge");
    REQUIRE(observer.Take().identityDiffEntries == counts.identityDiffEntries);
    emit(observer, kDeviceA, b);
    view = observer.BeginSwap(b);
    UNSCOPED_INFO("a table equal to the identity ramp differs nowhere");
    REQUIRE(observer.Take().identityDiffEntries == 0);
}
#endif

void testPwlEmission()
{
    GammaObserver observer;
    const auto    a = asymmetricTable();
    emit(observer, kDeviceA, a);
    // The observed table stays in effect until the PWL emission returns.
    const auto pwl  = observer.BeginPwlEmission(kDeviceA);
    auto       view = observer.BeginSwap(LinearGuestGammaTable());
    UNSCOPED_INFO("a PWL emission in flight changes nothing");
    REQUIRE((pwl != 0 && !view.assumed && view.table == a && view.emissionSequence == 1));
    observer.EndEmission(pwl);
    view = observer.BeginSwap(LinearGuestGammaTable());
    UNSCOPED_INFO("a returned PWL emission leaves the LUT unknown");
    REQUIRE((view.assumed && view.emissionSequence == 2));
    setter(observer, kDeviceA, asymmetricTable(1));
    view = observer.BeginSwap(LinearGuestGammaTable());
#if REREVVED_DEVELOPER
    auto counts = observer.Take();
    UNSCOPED_INFO("an unknown LUT is not compared with the setter: " + FormatGammaCounts(counts));
    REQUIRE((counts.pwlEmit == 1 && counts.emitComplete == 1 && counts.swapPendingSetter == 0));
#endif

    // A PWL emission overlapping a table emission: each takes effect when it returns.
    GammaObserver overlap;
    const auto    bytes = convert(a);
    const auto    table = overlap.BeginEmission(kDeviceA, bytes, bytes, false);
    const auto    other = overlap.BeginPwlEmission(kDeviceA);
    overlap.EndEmission(table);
    view = overlap.BeginSwap(LinearGuestGammaTable());
    UNSCOPED_INFO("the table emission returned first and the PWL one is still in flight");
    REQUIRE((!view.assumed && view.table == a));
    overlap.EndEmission(other);
    UNSCOPED_INFO("the PWL emission returned last");
    REQUIRE(overlap.BeginSwap(LinearGuestGammaTable()).assumed);
#if REREVVED_DEVELOPER
    counts = overlap.Take();
    UNSCOPED_INFO("the PWL begin overlapped: " + FormatGammaCounts(counts));
    REQUIRE(counts.emitOverlap == 1);
#endif
    UNSCOPED_INFO("no device begins nothing");
    REQUIRE(overlap.BeginPwlEmission(0) == 0);
}

#if REREVVED_DEVELOPER
void testFormat()
{
    GammaCounts counts;
    counts.setter               = 1;
    counts.setterChanged        = 2;
    counts.emitRetainedMismatch = 3;
    counts.noworkSwapChanged    = 4;
    counts.identityDiffEntries  = 254;
    const auto text             = FormatGammaCounts(counts);
    UNSCOPED_INFO("fields start with the setter: " + text);
    REQUIRE(text.starts_with(" gamma_obs_setter=1 gamma_obs_setter_changed=2 gamma_obs_setter_decode_fail=0"));
    for (const char* field : { " gamma_obs_emit_retained_mismatch=3 ", " gamma_obs_nowork_swap_changed=4 ", " gamma_obs_identity_diff_entries=254", " gamma_obs_emit_retained_equal=0 ", " gamma_obs_emit_overlap=0 ", " gamma_obs_emit_evicted=0 ", " gamma_obs_emit_second_site=0 ", " gamma_obs_swap_before_emit=0 ", " gamma_obs_swap_pending_setter=0 ", " gamma_obs_swap_table_changed=0 ", " gamma_obs_emit=0 ", " gamma_obs_emit_complete=0 ", " gamma_obs_pwl_setter=0 " })
    {
        UNSCOPED_INFO(std::string("the line has ") + field + ": " + text);
        REQUIRE(text.find(field) != std::string::npos);
    }
    UNSCOPED_INFO("the gauge is last");
    REQUIRE(text.ends_with("gamma_obs_identity_diff_entries=254"));
}
#endif

void testHookEntries()
{
    UNSCOPED_INFO("apply defaults on and the observe check off");
    REQUIRE((DEV_CHECK(!REXCVAR_GET(native_gamma_observe_check)) && REXCVAR_GET(native_gamma_apply)));
    // The default applies only under the native renderer; the process starts as no renderer.
    UNSCOPED_INFO("nothing is requested without the native renderer");
    REQUIRE((!GammaObserveRequested() && !GammaApplyRequested()));
    auto& observer = PublishedGammaObserver();
    DEV_DO(observer.Take());
    // Off: the entries return at once and nothing is counted.
    ObserveGammaSetter(kDeviceA, 0x7000F000);
    ObserveGammaPwlSetter(kDeviceA);
    UNSCOPED_INFO("no emission ticket with the cvars off");
    REQUIRE(ObserveGammaEmissionBegin(kDeviceA, 0x7000F000, kGuestGammaEmitterReturn) == 0);
    UNSCOPED_INFO("no PWL emission ticket with the cvars off");
    REQUIRE(ObserveGammaPwlEmissionBegin(kDeviceA) == 0);
    ObserveGammaEmissionEnd(0);
    ObserveGammaEmissionEnd(ObserveGammaEmissionBegin(0, 0x7000F000, kGuestGammaEmitterReturn));
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("the entries count nothing with the cvars off");
    REQUIRE(FormatGammaCounts(observer.Take()) == FormatGammaCounts(GammaCounts{}));
#endif
    // A table the observer knows stays known through begin/end calls that do nothing.
    const auto known = asymmetricTable(4);
    emit(observer, kDeviceA, known);
    ObserveGammaEmissionEnd(ObserveGammaEmissionBegin(kDeviceA, 0x7000F000, kGuestGammaEmitterReturn));
    ObserveGammaEmissionEnd(ObserveGammaPwlEmissionBegin(kDeviceA));
    auto view = observer.BeginSwap(LinearGuestGammaTable());
    UNSCOPED_INFO("begin and end with the cvars off leave the table alone");
    REQUIRE((view.table == known && !view.assumed));
    DEV_DO(observer.Take());

    // The native renderer turns the default on and implies the observer.
    SetGammaNativeRenderer(true);
    UNSCOPED_INFO("the native renderer applies and observes by default");
    REQUIRE((GammaObserveRequested() && GammaApplyRequested()));
    SetGammaNativeRenderer(false);
    UNSCOPED_INFO("another renderer keeps both off");
    REQUIRE((!GammaObserveRequested() && !GammaApplyRequested()));
    ObserveGammaSetter(kDeviceA, 0x7000F000);
    UNSCOPED_INFO("another renderer observes nothing");
    REQUIRE(ObserveGammaEmissionBegin(kDeviceA, 0x7000F000, kGuestGammaEmitterReturn) == 0);
#if REREVVED_DEVELOPER
    auto idle                = observer.Take();
    idle.identityDiffEntries = 0; // a gauge that outlives its window
    UNSCOPED_INFO("another renderer counts nothing");
    REQUIRE(FormatGammaCounts(idle) == FormatGammaCounts(GammaCounts{}));
#endif
    // An explicit false still turns it off under the native renderer, and an explicit true does nothing elsewhere.
    SetGammaNativeRenderer(true);
    REXCVAR_SET(native_gamma_apply, false);
    UNSCOPED_INFO("apply=false disables it under the native renderer");
    REQUIRE((!GammaObserveRequested() && !GammaApplyRequested()));
    SetGammaNativeRenderer(false);
    REXCVAR_SET(native_gamma_apply, true);
    UNSCOPED_INFO("apply=true does nothing under another renderer");
    REQUIRE((!GammaObserveRequested() && !GammaApplyRequested()));
    SetGammaNativeRenderer(true);
#if REREVVED_DEVELOPER
    REXCVAR_SET(native_gamma_apply, false);
    REXCVAR_SET(native_gamma_observe_check, true);
    UNSCOPED_INFO("observe alone does not apply");
    REQUIRE((GammaObserveRequested() && !GammaApplyRequested()));
#else
    REXCVAR_SET(native_gamma_apply, true);
#endif

    // On, with no guest memory behind the addresses: counted, never thrown.
    ObserveGammaSetter(kDeviceA, 0x7000F000);
    ObserveGammaSetter(kDeviceA, 0xFFFFFFF0);
    const auto ticket = ObserveGammaEmissionBegin(kDeviceA, 0x7000F000, kGuestGammaEmitterReturn);
    UNSCOPED_INFO("an unreadable emission still begins");
    REQUIRE(ticket != 0);
    ObserveGammaEmissionEnd(ticket);
    const auto second = ObserveGammaEmissionBegin(kDeviceB, 0xFFFFFFF0, 0x826A7838);
    ObserveGammaEmissionEnd(second);
    ObserveGammaPwlSetter(kDeviceA);
    const auto pwl = ObserveGammaPwlEmissionBegin(kDeviceA);
    UNSCOPED_INFO("a PWL emission begins");
    REQUIRE(pwl != 0);
    ObserveGammaEmissionEnd(pwl);
#if REREVVED_DEVELOPER
    const auto counts = observer.Take();
    UNSCOPED_INFO("hook entry counts: " + FormatGammaCounts(counts));
    REQUIRE((counts.setter == 2 && counts.setterDecodeFail == 2 && counts.emit == 2 && counts.emitSecondSite == 1 && counts.emitComplete == 2 && counts.errors == 2 && counts.pwlSetter == 1 &&
             counts.pwlEmit == 1 && counts.deviceChange >= 1));
#endif

    // A begin that fails while observing returns 0 and its end must not leave the previous table known.
    const auto before = asymmetricTable(6);
    emit(observer, kDeviceA, before);
    view = observer.BeginSwap(LinearGuestGammaTable());
    UNSCOPED_INFO("a table is known before the failed begin");
    REQUIRE((view.table == before && !view.assumed));
    const auto failed = ObserveGammaEmissionBegin(0, 0x7000F000, kGuestGammaEmitterReturn);
    UNSCOPED_INFO("a begin with no device fails");
    REQUIRE(failed == 0);
    ObserveGammaEmissionEnd(failed);
    view = observer.BeginSwap(LinearGuestGammaTable());
    UNSCOPED_INFO("the end of a failed begin leaves the table unknown");
    REQUIRE((view.assumed && ChooseSwapGamma(true, view, LinearGuestGammaTable()).assumed));
#if REREVVED_DEVELOPER
    const auto failedCounts = observer.Take();
    UNSCOPED_INFO("the failed begin and its end are errors: " + FormatGammaCounts(failedCounts));
    REQUIRE((failedCounts.errors == 2 && failedCounts.emitEvicted == 0));
#endif
    // The same for a PWL begin with no device.
    emit(observer, kDeviceA, before);
    ObserveGammaEmissionEnd(ObserveGammaPwlEmissionBegin(0));
    UNSCOPED_INFO("the end of a failed PWL begin leaves the table unknown");
    REQUIRE(observer.BeginSwap(LinearGuestGammaTable()).assumed);
    DEV_DO(observer.Take());
    DEV_CVAR_SET(native_gamma_observe_check, false);
    REXCVAR_SET(native_gamma_apply, true);
    SetGammaNativeRenderer(false);
}

} // namespace

TEST_CASE("gamma_observer", "[ctest][gamma-observer][timeout-60]")
{
    testDecode();
    testLinearDefaultAndHash();
    DEV_DO(testSetter());
    testDefaultBeforeFirstEmission();
    testSetterNewerThanEmission();
    testEmissionRetainedAndSites();
    testOverlap();
    testEvictedEmission();
    testEvictedBeyondAnyList();
    testDeviceChange();
    DEV_DO(testSwapCounts());
    testPwlEmission();
    DEV_DO(testFormat());
    testHookEntries();
}
