#include "system_link_trace.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <string_view>

namespace
{

using rerevved::system_link::ClassifyDatagram;
using rerevved::system_link::DatagramKind;
using rerevved::system_link::Direction;
using rerevved::system_link::kLimitedBroadcast;

bool classifiesAs(Direction                direction,
                  std::span<const uint8_t> head,
                  uint32_t                 size,
                  uint32_t                 address,
                  uint16_t                 port,
                  DatagramKind             kind,
                  uint8_t                  type)
{
    const auto result = ClassifyDatagram(direction, head, size, address, port);
    return result.kind == kind && result.type == type;
}

} // namespace

TEST_CASE("system_link_trace", "[ctest][system-link-trace]")
{
    constexpr uint32_t kPeer = 0x0A000088;

    const std::array<uint8_t, 5> echo{ 0xFE, 0xFD, 0x02, 0x11, 0x22 };
    const std::array<uint8_t, 5> prequery{ 0xFE, 0xFD, 0x09, 0x11, 0x22 };
    const std::array<uint8_t, 5> query{ 0xFE, 0xFD, 0x00, 0x11, 0x22 };
    const std::array<uint8_t, 5> otherRequest{ 0xFE, 0xFD, 0x07, 0x11, 0x22 };
    UNSCOPED_INFO("classify the QR2 echo request");
    CHECK(classifiesAs(Direction::Send, echo, 5, kLimitedBroadcast, 26950, DatagramKind::Echo, 0x02));
    UNSCOPED_INFO("classify the QR2 prequery request");
    CHECK(classifiesAs(Direction::Receive, prequery, 5, kPeer, 51000, DatagramKind::Prequery, 0x09));
    UNSCOPED_INFO("classify the QR2 query request");
    CHECK(classifiesAs(Direction::Send, query, 5, kLimitedBroadcast, 26900, DatagramKind::Query, 0x00));
    UNSCOPED_INFO("keep unknown QR2 request types distinct");
    CHECK(classifiesAs(Direction::Send, otherRequest, 5, kLimitedBroadcast, 26900, DatagramKind::OtherRequest, 0x07));

    const std::array<uint8_t, 5> echoReply{ 0x05, 0x11, 0x22, 0x33, 0x44 };
    UNSCOPED_INFO("classify a host reply sent to the limited broadcast address");
    CHECK(classifiesAs(Direction::Send, echoReply, 5, kLimitedBroadcast, 51000, DatagramKind::Reply, 0x05));
    UNSCOPED_INFO("classify a client reply received from a discovery port");
    CHECK(classifiesAs(Direction::Receive, echoReply, 5, kPeer, 26900, DatagramKind::Reply, 0x05));
    UNSCOPED_INFO("leave unicast datagrams without framing unclassified");
    CHECK(classifiesAs(Direction::Send, echoReply, 5, kPeer, 51000, DatagramKind::Other, 0x00));

    const std::array<uint8_t, 5> queryReply{ 0x00, 0x11, 0x22, 0x33, 0x44 };
    const std::array<uint8_t, 5> unframed{ 0x44, 0x11, 0x22, 0x33, 0x44 };
    const std::array<uint8_t, 5> invalidReply{ 0x0B, 0x11, 0x22, 0x33, 0x44 };
    UNSCOPED_INFO("leave unframed discovery-port traffic unclassified without a payload type");
    CHECK(classifiesAs(Direction::Receive, unframed, 5, kPeer, 26900, DatagramKind::Other, 0x00));
    UNSCOPED_INFO("classify a query reply with a four-byte instance key from a discovery port");
    CHECK(classifiesAs(Direction::Receive, queryReply, 5, kPeer, 26900, DatagramKind::Reply, 0x00));
    UNSCOPED_INFO("classify a query reply sent to the limited broadcast address");
    CHECK(classifiesAs(Direction::Send, queryReply, 5, kLimitedBroadcast, 51000, DatagramKind::Reply, 0x00));
    UNSCOPED_INFO("require a complete instance key before classifying a reply");
    CHECK(classifiesAs(Direction::Receive, std::span<const uint8_t>(queryReply.data(), 4), 4, kPeer, 26900, DatagramKind::Other, 0x00));
    UNSCOPED_INFO("reject reply types outside the QR2 packet range without logging a payload byte");
    CHECK(classifiesAs(Direction::Send, invalidReply, 5, kLimitedBroadcast, 51000, DatagramKind::Other, 0x00));

    const std::array<uint8_t, 5> vdpChallenge{ 0x00, 0x27, 0xFE, 0xFE, 0x01 };
    const std::array<uint8_t, 5> vdpSend{ 0x00, 0x03, 0xFE, 0xFE, 0x68 };
    const std::array<uint8_t, 5> udpAck{ 0xFE, 0xFE, 0x64, 0x00, 0x00 };
    UNSCOPED_INFO("read the GT2 type after the VDP length prefix");
    CHECK(classifiesAs(Direction::Send, vdpChallenge, 0x29, kPeer, 1000, DatagramKind::Gt2, 0x01));
    UNSCOPED_INFO("accept a VDP prefix containing the length of the following bytes");
    CHECK(classifiesAs(Direction::Send, vdpSend, 5, kPeer, 1000, DatagramKind::Gt2, 0x68));
    UNSCOPED_INFO("reject a VDP prefix that does not match the datagram size");
    CHECK(classifiesAs(Direction::Send, vdpChallenge, 5, kPeer, 1000, DatagramKind::Other, 0x00));
    UNSCOPED_INFO("read the GT2 type without a VDP length prefix");
    CHECK(classifiesAs(Direction::Receive, udpAck, 5, kPeer, 1000, DatagramKind::Gt2, 0x64));

    const std::array<uint8_t, 5> replyWithGt2Bytes{ 0x05, 0x11, 0xFE, 0xFE, 0x44 };
    UNSCOPED_INFO("keep GT2-like instance key bytes classified as a QR2 reply");
    CHECK(classifiesAs(Direction::Receive, replyWithGt2Bytes, 5, kPeer, 26900, DatagramKind::Reply, 0x05));
    UNSCOPED_INFO("accept prefixed GT2 only when the full big-endian length matches");
    CHECK(classifiesAs(Direction::Receive, replyWithGt2Bytes, 0x0513, kPeer, 26900, DatagramKind::Gt2, 0x44));

    UNSCOPED_INFO("report master-port traffic before framing");
    CHECK(classifiesAs(Direction::Send, query, 5, kPeer, 27900, DatagramKind::Master, 0x00));
    UNSCOPED_INFO("report unframed master-port traffic without logging a payload byte");
    CHECK(classifiesAs(Direction::Send, unframed, 5, kPeer, 27900, DatagramKind::Master, 0x00));
    UNSCOPED_INFO("report DNS traffic without payload bytes");
    CHECK(classifiesAs(Direction::Send, std::span<const uint8_t>{}, 0, kPeer, 53, DatagramKind::Master, 0x00));
    UNSCOPED_INFO("require the QR2 type byte before classifying a request");
    CHECK(classifiesAs(Direction::Send, std::span<const uint8_t>(echo.data(), 2), 2, kLimitedBroadcast, 26900, DatagramKind::Other, 0x00));

    const auto sweep = rerevved::system_link::MakeDatagramKey(
        Direction::Send, echo, 16, kLimitedBroadcast, 26950, false);
    const auto sweepEnd = rerevved::system_link::MakeDatagramKey(
        Direction::Send, echo, 16, kLimitedBroadcast, 27000, false);
    UNSCOPED_INFO("collapse the echo port sweep into one key");
    CHECK((sweep.port == 0 && !(sweep < sweepEnd) && !(sweepEnd < sweep)));
    const auto directed = rerevved::system_link::MakeDatagramKey(
        Direction::Send, query, 27, kLimitedBroadcast, 26900, false);
    UNSCOPED_INFO("keep directed query ports and sizes");
    CHECK((directed.port == 26900 && directed.size == 27));

    const auto emptyReceive = rerevved::system_link::MakeDatagramKey(
        Direction::Receive, std::span<const uint8_t>{}, 0, kPeer, 26900, false);
    UNSCOPED_INFO("keep a successful empty receive as an unclassified datagram key");
    CHECK((emptyReceive.direction == Direction::Receive && emptyReceive.kind == DatagramKind::Other &&
           emptyReceive.type == 0 && emptyReceive.size == 0 && emptyReceive.port == 26900 &&
           !emptyReceive.failed));

    rerevved::system_link::DatagramCounters counters;
    UNSCOPED_INFO("count repeated keys");
    CHECK((counters.Record(sweep) == 1 && counters.Record(sweepEnd) == 2));
    for (uint32_t size = 0; size < rerevved::system_link::DatagramCounters::kMaxKeys + 4; ++size)
    {
        auto key = directed;
        key.size = size + 1000;
        counters.Record(key);
    }
    UNSCOPED_INFO("bound the key table");
    CHECK(counters.snapshot().size() == rerevved::system_link::DatagramCounters::kMaxKeys);
    UNSCOPED_INFO("count datagrams beyond the key table");
    CHECK(counters.overflow() == 5);

    UNSCOPED_INFO("log first and power-of-two counts only");
    CHECK((rerevved::system_link::ShouldLogCount(1) && rerevved::system_link::ShouldLogCount(64) &&
           !rerevved::system_link::ShouldLogCount(3) && !rerevved::system_link::ShouldLogCount(0)));
}
