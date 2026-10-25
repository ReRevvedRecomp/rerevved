#pragma once

#include <array>
#include <cstdint>

namespace rerevved::unit_sale
{

constexpr uint32_t kUnitBase       = 0x830F2BF0;
constexpr uint32_t kDefinitionBase = 0x82F700D8;
constexpr uint32_t kGlobalBase     = 0x83130000;

enum class Phase
{
    Preview,
    Credit
};
enum class Decision
{
    Captured,
    Applied,
    Identity,
    Unreadable,
    Unsupported,
    Reentrant,
    Missing,
    Changed,
    NativeValue,
    Exception
};

class Memory
{
public:
    virtual ~Memory()                                             = default;
    virtual bool Read(uint32_t address, void* out, uint32_t size) = 0;
};

struct Seam
{
    uint32_t frame            = 0;
    uint32_t unitBase         = 0;
    uint32_t definitionBase   = 0;
    uint32_t deadMask         = 0;
    uint32_t globalBase       = 0;
    int64_t  originalPlayer   = -1;
    int64_t  slot             = -1;
    int64_t  type             = -1;
    uint64_t offset           = 0;
    uint64_t definitionOffset = 0;
    uint64_t definitionFlags  = 0;
    int64_t  nativeSum        = 0;
    int64_t  multiplier       = 0;
    int64_t  h                = 1;
};

struct Record
{
    uint8_t  owner                           = 0;
    int8_t   type                            = 0;
    uint8_t  marker                          = 0;
    uint32_t flags                           = 0;
    int16_t  link                            = 0;
    bool     operator==(const Record&) const = default;
};

struct Definition
{
    int8_t   factor                              = 0;
    uint32_t flags                               = 0;
    bool     operator==(const Definition&) const = default;
};

struct Snapshot
{
    uint32_t                   frame  = 0;
    uint32_t                   player = 0;
    uint32_t                   slot   = 0;
    std::array<Record, 256>    records{};
    std::array<Definition, 38> definitions{};
    int32_t                    sum                               = 0;
    int32_t                    weighted                          = 0;
    int32_t                    multiplier                        = 0;
    bool                       operator==(const Snapshot&) const = default;
};

// One pending collection per thread. A nested seam invalidates both users.
// A later unmatched apply can never consume an earlier collection.
class Collection
{
public:
    Decision Begin(Memory& memory, Phase phase, const Seam& seam) noexcept;
    Decision Apply(Memory& memory, Phase phase, const Seam& seam, uint64_t& amount) noexcept;

private:
    bool     busy     = false;
    bool     active   = false;
    bool     poisoned = false;
    Phase    phase    = Phase::Preview;
    Snapshot snapshot{};
};

#ifdef UNIT_SALE_TESTING
void SetMemoryForTesting(Memory* memory) noexcept;
#endif

} // namespace rerevved::unit_sale
