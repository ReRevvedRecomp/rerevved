#pragma once

#include "resource_identity_overrides.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace rerevved::resync_record
{

class GuestReader
{
public:
    virtual ~GuestReader()                                                 = default;
    virtual bool Read(uint32_t address, uint32_t size, uint8_t* out) const = 0;
};

inline constexpr uint32_t kMaxPlayers = 32;
inline constexpr uint32_t kMaxCities  = 128;

// Each field is the sum this player contributes to the compared scalar, modulo 2^32.
struct PlayerSubtotals
{
    uint32_t units           = 0;
    uint32_t reserve         = 0;
    uint32_t cultureStrength = 0;
    uint32_t derivedCulture  = 0;
    uint32_t tally           = 0;
    uint32_t strategy        = 0;
    uint32_t progress        = 0; // Selected technology progress, 0 when none is selected.
    uint32_t cities          = 0; // Full contribution of the counted cities this player owns.
};

struct CityFields
{
    uint32_t index      = 0;
    int32_t  owner      = 0;
    int32_t  population = 0;
    uint32_t word10     = 0;
    int32_t  half34     = 0;
    int32_t  half36     = 0;
    int32_t  half38     = 0;
    int32_t  half3C     = 0;
};

struct Ledger
{
    uint32_t                                 playerCount = 0;
    std::array<PlayerSubtotals, kMaxPlayers> players{};
    uint32_t                                 cities    = 0; // All counted cities, including owners outside 0..playerCount-1.
    uint32_t                                 wonders   = 0;
    uint32_t                                 sum       = 0; // Low 32 bits of the native return value.
    uint32_t                                 cityCount = 0; // Counted cities, in index order, in cityFields.
    std::array<CityFields, kMaxCities>       cityFields{};
};

// Reproduces the native compared scalar (sub_82CD4CF0) over the given memory.
std::optional<Ledger> ComputeLedger(const GuestReader& memory);

// FNV-1a over the serialized resource identity overrides.
uint32_t OverridesDigest(const resource_identity_overrides::Snapshot& snapshot);

// Admits each (turn, caller, scalar) triple once and refuses more than kCapacity triples per turn, so output stays bounded.
class LedgerDedupe
{
public:
    static constexpr size_t kCapacity = 32;
    bool                    Admit(uint32_t turn, uint32_t caller, uint32_t scalar);

private:
    struct Entry
    {
        uint32_t caller = 0;
        uint32_t scalar = 0;
    };

    std::mutex                   mutex_;
    bool                         started_ = false;
    uint32_t                     turn_    = 0;
    size_t                       count_   = 0;
    std::array<Entry, kCapacity> entries_{};
};

} // namespace rerevved::resync_record
