#include "system_link_trace.h"

#include <bit>

namespace rerevved::system_link
{

namespace
{

constexpr uint8_t kQr2Magic0 = 0xFE;
constexpr uint8_t kQr2Magic1 = 0xFD;
constexpr uint8_t kGt2Magic  = 0xFE;

bool isDiscoveryPort(uint16_t port)
{
    return port >= kDiscoveryFirstPort && port <= kDiscoveryLastPort;
}

bool isMasterPort(uint16_t port)
{
    return port == 53 || port == 27900 || port == 27901;
}

} // namespace

DatagramClass ClassifyDatagram(Direction                direction,
                               std::span<const uint8_t> head,
                               uint32_t                 size,
                               uint32_t                 remoteAddress,
                               uint16_t                 remotePort)
{
    if (isMasterPort(remotePort))
    {
        return { DatagramKind::Master, 0 };
    }
    if (head.size() >= 3 && head[0] == kQr2Magic0 && head[1] == kQr2Magic1)
    {
        switch (head[2])
        {
            case 0x02:
                return { DatagramKind::Echo, head[2] };
            case 0x09:
                return { DatagramKind::Prequery, head[2] };
            case 0x00:
                return { DatagramKind::Query, head[2] };
            default:
                return { DatagramKind::OtherRequest, head[2] };
        }
    }
    if (head.size() >= 5 && size >= 5 && head[2] == kGt2Magic && head[3] == kGt2Magic &&
        static_cast<uint16_t>(head[0] << 8 | head[1]) == size - 2)
    {
        return { DatagramKind::Gt2, head[4] };
    }
    if (head.size() >= 3 && head[0] == kGt2Magic && head[1] == kGt2Magic)
    {
        return { DatagramKind::Gt2, head[2] };
    }
    const bool hostReply   = direction == Direction::Send && remoteAddress == kLimitedBroadcast;
    const bool clientReply = direction == Direction::Receive && isDiscoveryPort(remotePort);
    if (size >= 5 && !head.empty() && head[0] <= 0x0A && (hostReply || clientReply))
    {
        return { DatagramKind::Reply, head[0] };
    }
    return {};
}

DatagramKey MakeDatagramKey(Direction                direction,
                            std::span<const uint8_t> head,
                            uint32_t                 size,
                            uint32_t                 remoteAddress,
                            uint16_t                 remotePort,
                            bool                     failed)
{
    const auto  classified = ClassifyDatagram(direction, head, size, remoteAddress, remotePort);
    DatagramKey key{};
    key.direction = direction;
    key.kind      = classified.kind;
    key.type      = classified.type;
    key.size      = size;
    key.port      = classified.kind == DatagramKind::Echo && direction == Direction::Send &&
                            isDiscoveryPort(remotePort)
                        ? uint16_t{ 0 }
                        : remotePort;
    key.failed    = failed;
    return key;
}

const char* KindName(DatagramKind kind)
{
    switch (kind)
    {
        case DatagramKind::Echo:
            return "echo";
        case DatagramKind::Prequery:
            return "prequery";
        case DatagramKind::Query:
            return "query";
        case DatagramKind::OtherRequest:
            return "qr2-request";
        case DatagramKind::Reply:
            return "reply";
        case DatagramKind::Gt2:
            return "gt2";
        case DatagramKind::Master:
            return "master";
        case DatagramKind::Other:
            break;
    }
    return "other";
}

uint64_t DatagramCounters::Record(const DatagramKey& key)
{
    std::lock_guard lock(mutex_);
    auto            existing = counts_.find(key);
    if (existing != counts_.end())
    {
        return ++existing->second;
    }
    if (counts_.size() >= kMaxKeys)
    {
        ++overflow_;
        return 0;
    }
    counts_.emplace(key, 1);
    return 1;
}

uint64_t DatagramCounters::overflow() const
{
    std::lock_guard lock(mutex_);
    return overflow_;
}

std::map<DatagramKey, uint64_t> DatagramCounters::snapshot() const
{
    std::lock_guard lock(mutex_);
    return counts_;
}

bool ShouldLogCount(uint64_t count)
{
    return count != 0 && std::has_single_bit(count);
}

} // namespace rerevved::system_link
