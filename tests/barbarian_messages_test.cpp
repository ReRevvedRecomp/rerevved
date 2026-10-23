#include "barbarian_messages.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <iterator>
#include <set>
#include <string_view>

namespace
{

using namespace rerevved::barbarian_messages;

constexpr uint32_t kMessage = 0x7000FE00;
constexpr uint32_t kShow    = kMessage - kShowFrameSize;

void testVillageSites()
{
    std::set<uint32_t> addresses(std::begin(kVillageSites), std::end(kVillageSites));
    UNSCOPED_INFO("nine distinct village sites");
    CHECK(addresses.size() == 9);
    UNSCOPED_INFO("capture site is a village call");
    CHECK(IsVillageReturn(0x82D1B8B4 + 4));
    UNSCOPED_INFO("capture hint site is a village call");
    CHECK(IsVillageReturn(0x82D09138 + 4));
    UNSCOPED_INFO("the call address itself is not a return address");
    CHECK(!IsVillageReturn(0x82D1B8B4));
    // The first-contact message has its own option and hook.
    UNSCOPED_INFO("contact message is not a village call");
    CHECK(!IsVillageReturn(0x82CD46C8 + 4));
    UNSCOPED_INFO("unknown caller is not a village call");
    CHECK(!IsVillageReturn(0));
}

void testSuppressesOnlyTheHiddenMessageShow()
{
    LeaderMessageGate gate;
    UNSCOPED_INFO("no message stays native");
    CHECK(!gate.SuppressShow(kShow));

    gate.Begin(false, kMessage);
    UNSCOPED_INFO("disabled or other caller stays native");
    CHECK(!gate.SuppressShow(kShow));

    gate.Begin(true, kMessage);
    UNSCOPED_INFO("nested advisor show stays native");
    CHECK(!gate.SuppressShow(kShow - 0x200));
    UNSCOPED_INFO("village message show is suppressed");
    CHECK(gate.SuppressShow(kShow));
    UNSCOPED_INFO("suppression applies once");
    CHECK(!gate.SuppressShow(kShow));

    gate.Begin(true, kMessage);
    gate.End(kMessage - 0x200);
    UNSCOPED_INFO("another frame's exit keeps the state");
    CHECK(gate.SuppressShow(kShow));

    gate.Begin(true, kMessage);
    gate.End(kMessage);
    UNSCOPED_INFO("show after the message stays native");
    CHECK(!gate.SuppressShow(kShow));

    gate.Begin(true, kMessage);
    gate.Begin(false, kMessage);
    UNSCOPED_INFO("a later message replaces the state");
    CHECK(!gate.SuppressShow(kShow));
}

void testNestedMessageInsideHiddenMessage()
{
    constexpr uint32_t kNested     = kMessage - 0x600;
    constexpr uint32_t kNestedShow = kNested - kShowFrameSize;

    LeaderMessageGate gate;
    gate.Begin(true, kMessage);
    gate.Begin(false, kNested, true);
    UNSCOPED_INFO("outer show waits for the nested message");
    CHECK(!gate.SuppressShow(kShow));
    UNSCOPED_INFO("nested message of a hidden one is suppressed");
    CHECK(gate.SuppressShow(kNestedShow));
    gate.End(kNested);
    UNSCOPED_INFO("outer show is still suppressed after the nested one");
    CHECK(gate.SuppressShow(kShow));
    gate.End(kMessage);

    gate.Begin(false, kMessage);
    gate.Begin(false, kNested, true);
    UNSCOPED_INFO("nested message of a shown one stays native");
    CHECK(!gate.SuppressShow(kNestedShow));
    gate.End(kNested);
    UNSCOPED_INFO("shown outer message stays native");
    CHECK(!gate.SuppressShow(kShow));

    gate.Begin(true, kMessage);
    gate.Begin(false, kNested);
    UNSCOPED_INFO("a message from another caller replaces the state");
    CHECK(!gate.SuppressShow(kNestedShow));
    UNSCOPED_INFO("replaced outer message stays native");
    CHECK(!gate.SuppressShow(kShow));

    gate.Begin(true, kMessage);
    gate.End(kMessage);
    gate.Begin(false, kNested, true);
    UNSCOPED_INFO("a finished hidden message is not inherited");
    CHECK(!gate.SuppressShow(kNestedShow));
}

} // namespace

TEST_CASE("barbarian_messages", "[ctest][barbarian-messages]")
{
    testVillageSites();
    testSuppressesOnlyTheHiddenMessageShow();
    testNestedMessageInsideHiddenMessage();
}
