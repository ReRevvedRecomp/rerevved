#include "resource_identity_overrides.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

namespace rerevved::resource_identity_overrides
{
namespace
{

const auto emptySnapshot = std::make_shared<const Snapshot>();
// Apple libc++ has no std::atomic<std::shared_ptr>.
std::mutex                      currentMutex;
std::shared_ptr<const Snapshot> current = emptySnapshot;
std::atomic<Observer>           observer{ nullptr };

bool validRecord(Record record)
{
    return record.tileIndex / 40 < 32 && record.tileIndex % 40 < 32 &&
           record.ordinal < 22;
}

bool validSnapshot(const Snapshot& snapshot)
{
    for (size_t i = 0; i < snapshot.size(); ++i)
    {
        if (!validRecord(snapshot[i]) || (i != 0 && snapshot[i - 1].tileIndex >= snapshot[i].tileIndex))
        {
            return false;
        }
    }
    return true;
}

} // namespace

bool Publish(std::span<const Record> records, std::source_location caller)
{
    Snapshot sorted(records.begin(), records.end());
    std::sort(sorted.begin(), sorted.end(), [](Record left, Record right)
              {
                  return left.tileIndex < right.tileIndex;
              });
    if (!validSnapshot(sorted))
    {
        return false;
    }
    const auto                      next = sorted.empty() ? emptySnapshot : std::make_shared<const Snapshot>(std::move(sorted));
    std::shared_ptr<const Snapshot> previous;
    {
        std::lock_guard lock(currentMutex);
        previous = std::exchange(current, next);
    }
    if (const auto notify = observer.load())
    {
        notify(true, *previous, *next, caller);
    }
    return true;
}

std::shared_ptr<const Snapshot> Current()
{
    std::lock_guard lock(currentMutex);
    return current;
}

void Clear(std::source_location caller)
{
    std::shared_ptr<const Snapshot> previous;
    {
        std::lock_guard lock(currentMutex);
        previous = std::exchange(current, emptySnapshot);
    }
    if (const auto notify = observer.load())
    {
        notify(false, *previous, *emptySnapshot, caller);
    }
}

void SetObserver(Observer value)
{
    observer.store(value);
}

std::vector<uint8_t> Serialize(const Snapshot& snapshot)
{
    if (!validSnapshot(snapshot))
    {
        return {};
    }
    const auto           count = static_cast<uint16_t>(snapshot.size());
    std::vector<uint8_t> bytes{ kSchemaVersion, static_cast<uint8_t>(count >> 8), static_cast<uint8_t>(count) };
    bytes.reserve(3 + snapshot.size() * 3);
    for (Record record : snapshot)
    {
        bytes.push_back(static_cast<uint8_t>(record.tileIndex >> 8));
        bytes.push_back(static_cast<uint8_t>(record.tileIndex));
        bytes.push_back(record.ordinal);
    }
    return bytes;
}

std::optional<Snapshot> Deserialize(std::span<const uint8_t> bytes)
{
    if (bytes.size() < 3 || bytes[0] != kSchemaVersion)
    {
        return std::nullopt;
    }
    const size_t count = (size_t{ bytes[1] } << 8) | bytes[2];
    if (count > 32 * 32 || bytes.size() != 3 + count * 3)
    {
        return std::nullopt;
    }
    Snapshot snapshot;
    snapshot.reserve(count);
    for (size_t i = 3; i < bytes.size(); i += 3)
    {
        snapshot.push_back({ static_cast<uint16_t>((uint16_t{ bytes[i] } << 8) | bytes[i + 1]), bytes[i + 2] });
    }
    return validSnapshot(snapshot) ? std::optional<Snapshot>(std::move(snapshot)) : std::nullopt;
}

uint64_t CoordinateSum(const Snapshot& snapshot, uint32_t tileIndex, int8_t terrain, uint64_t nativeSum)
{
    const auto found = std::lower_bound(snapshot.begin(), snapshot.end(), tileIndex, [](Record record, uint32_t index)
                                        {
                                            return record.tileIndex < index;
                                        });
    if (found == snapshot.end() || found->tileIndex != tileIndex)
    {
        return nativeSum;
    }
    if (terrain < 0 || terrain > 6)
    {
        return nativeSum;
    }
    const int count = terrain == 1 ? 4 : 3;
    for (int position = 0; position < count; ++position)
    {
        if (kIdentities[terrain][position] == found->ordinal)
        {
            return static_cast<uint64_t>(position);
        }
    }
    return nativeSum;
}

} // namespace rerevved::resource_identity_overrides
