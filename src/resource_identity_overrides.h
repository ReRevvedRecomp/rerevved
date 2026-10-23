#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <vector>

namespace rerevved::resource_identity_overrides
{

// Native rows in coordinate-remainder order; Grassland has four entries.
// Evidence: https://github.com/ReRevvedRecomp/rerevved-research/blob/main/docs/reference/resource-definitions.md#terrain-and-coordinate-lookup
inline constexpr int kIdentities[7][4] = {
    { 10, 16, 18, -1 }, { 0, 20, 19, 11 }, { 8, 13, 14, -1 }, { 5, 9, 17, -1 }, { 1, 3, 6, -1 }, { 2, 4, 12, -1 }, { 7, 15, 21, -1 }
};

struct Record
{
    uint16_t tileIndex                       = 0;
    uint8_t  ordinal                         = 0;
    bool     operator==(const Record&) const = default;
};

using Snapshot = std::vector<Record>;

// Publication validates canonical stride-40 indices and rejects duplicate tiles.
// Readers retain an immutable snapshot across a concurrent publication or clear.
bool                            Publish(std::span<const Record> records, std::source_location caller = std::source_location::current());
std::shared_ptr<const Snapshot> Current();
void                            Clear(std::source_location caller = std::source_location::current());

// Optional host observer; notification follows publication and never owns state.
using Observer = void (*)(bool published, const Snapshot& previous, const Snapshot& next, std::source_location caller) noexcept;
void SetObserver(Observer observer);

// Schema 1: version U8, count U16, then (index U16, ordinal U8), big endian.
// Decode validates the whole payload without publishing or performing file I/O.
constexpr uint8_t       kSchemaVersion = 1;
std::vector<uint8_t>    Serialize(const Snapshot& snapshot);
std::optional<Snapshot> Deserialize(std::span<const uint8_t> bytes);

// Evidence: https://github.com/ReRevvedRecomp/rerevved-research/blob/main/docs/reference/resource-identity-overrides.md#override-and-fresh-visual-creation
uint64_t CoordinateSum(const Snapshot& snapshot, uint32_t tileIndex, int8_t terrain, uint64_t nativeSum);

} // namespace rerevved::resource_identity_overrides
