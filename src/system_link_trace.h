#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <tuple>

namespace rerevved::system_link
{

// Leading bytes the classifier reads. Payload bytes are never retained.
constexpr size_t kClassifiedHeadSize = 5;

constexpr uint16_t kDiscoveryFirstPort = 26900;
constexpr uint16_t kDiscoveryLastPort  = 27000;
constexpr uint32_t kLimitedBroadcast   = 0xFFFFFFFF;

enum class Direction : uint8_t
{
    Send,
    Receive,
};

enum class DatagramKind : uint8_t
{
    Echo,
    Prequery,
    Query,
    OtherRequest,
    Reply,
    Gt2,
    Master,
    Other,
};

struct DatagramClass
{
    DatagramKind kind = DatagramKind::Other;
    uint8_t      type = 0;
};

// System-link trace distinguishes QR2, GT2 with optional VDP prefix, and
// GameSpy master/DNS datagrams; VDP size is size - 2.
DatagramClass ClassifyDatagram(Direction                direction,
                               std::span<const uint8_t> head,
                               uint32_t                 size,
                               uint32_t                 remoteAddress,
                               uint16_t                 remotePort);

struct DatagramKey
{
    Direction    direction = Direction::Send;
    DatagramKind kind      = DatagramKind::Other;
    uint8_t      type      = 0;
    uint32_t     size      = 0;
    uint16_t     port      = 0; // Remote port; 0 for a discovery port sweep.
    bool         failed    = false;

    bool operator<(const DatagramKey& other) const
    {
        return std::tie(direction, kind, type, size, port, failed) <
               std::tie(other.direction, other.kind, other.type, other.size, other.port, other.failed);
    }
};

DatagramKey MakeDatagramKey(Direction                direction,
                            std::span<const uint8_t> head,
                            uint32_t                 size,
                            uint32_t                 remoteAddress,
                            uint16_t                 remotePort,
                            bool                     failed);

const char* KindName(DatagramKind kind);

// Bounded per-key counters. Record returns the new count, or 0 once the key
// table is full and the datagram only reaches the overflow count.
class DatagramCounters
{
public:
    static constexpr size_t kMaxKeys = 256;

    uint64_t                        Record(const DatagramKey& key);
    uint64_t                        overflow() const;
    std::map<DatagramKey, uint64_t> snapshot() const;

private:
    mutable std::mutex              mutex_;
    std::map<DatagramKey, uint64_t> counts_;
    uint64_t                        overflow_ = 0;
};

// Logs the first occurrence of a key and each power-of-two count after it.
bool ShouldLogCount(uint64_t count);

} // namespace rerevved::system_link
