#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace rerevved::capital_grace
{

using Words = std::array<uint32_t, 5>;

constexpr uint32_t kUnits  = 0x830F2BF0;
constexpr uint32_t kCities = 0x8314FE28;
// Move17 0x82CDEDFC..0x82CDEE2C: signed type, 148-byte definition, flags at +0x50.
constexpr uint32_t kDefinitions      = 0x82F700D8;
constexpr uint32_t kDefinitionStride = 148;

bool                    Active(bool enabled, int32_t year) noexcept;
bool                    Player(int32_t player) noexcept;
std::optional<uint32_t> UnitAddress(int32_t player, int32_t slot) noexcept;
std::optional<uint32_t> CityAddress(int32_t city) noexcept;
std::optional<int32_t>  ActorSlot(const Words& words) noexcept;
std::optional<int32_t>  Direction(const Words& words, int16_t currentDirection) noexcept;

struct Point
{
    int32_t x;
    int32_t y;
    bool    operator==(const Point&) const = default;
};

std::optional<Point> Adjacent(Point source, int32_t width, int32_t height, int32_t dx, int32_t dy) noexcept;

struct Actor
{
    int32_t  owner;
    int32_t  type;
    uint32_t flags;
    Point    position;
    int16_t  direction;
};

struct City
{
    int32_t  owner;
    int32_t  population;
    uint32_t buildings;
    Point    position;
};

bool LiveActor(const Actor& actor, int32_t player, int32_t barbarian) noexcept;
bool ProtectedCity(const City& city, int32_t barbarian, std::optional<Point> target = std::nullopt) noexcept;
bool SpecialArrival(uint32_t command, int32_t type, uint16_t tileFlags, int32_t tileOwner, int32_t player) noexcept;

struct Retirement
{
    bool actor;
    bool selection;
    bool cache;
};

Retirement PlanRetirement(uint32_t flags, int32_t player, int32_t slot, int32_t currentPlayer, int32_t selected, int32_t cached) noexcept;

enum class Callsite : uint32_t
{
    Blitz    = 0x82D2C9BC,
    Ordinary = 0x82D2CE00,
    Packed   = 0x82D2C170,
    Linked   = 0x82D2BD84,
};

struct Submission
{
    bool    refused      = false;
    int32_t parentPlayer = -1;
    int32_t parentSlot   = -1;
    int32_t childSlot    = -1;
};

// Full serials, exact wrapper/frame pointers and queue identity bind an escape.
// No slot lifetime or persistent actor fence is inferred from a token.
// Excess scopes stay untracked; their exits never discard existing scopes.
class Tokens
{
public:
    static constexpr size_t kCapacity = 16;
    void                    Reset() noexcept;
    bool                    PushProducer(uint32_t frame, Callsite site, const Words& words) noexcept;
    Submission              PopProducer(uint32_t frame, Callsite site) noexcept;
    void                    BeginSubmit(uint32_t frame, uint32_t queue) noexcept;
    void                    EndSubmit(uint32_t frame) noexcept;
    bool                    Append(uint32_t frame, uint32_t record, const Words& words, uint32_t write, uint32_t read) noexcept;
    void                    CompleteAppend(uint32_t frame) noexcept;
    void                    DrainSerial(uint32_t frame, uint32_t read, uint32_t write) noexcept;
    bool                    DrainRecord(uint32_t frame, uint32_t record, const Words& words) noexcept;
    bool                    Refuse(uint32_t record, const Words& words) noexcept;
    void                    PrepareParent(uint32_t frame, int32_t player, int32_t slot) noexcept;
    bool                    ArmCleanup(uint32_t frame, const Submission& submission) noexcept;
    bool                    FinishCleanup(uint32_t frame) noexcept;
    uint64_t                Imbalances() const noexcept;
    uint64_t                CapacityFailures() const noexcept;

private:
    enum class Stage
    {
        Open,
        Appending,
        Pending,
        Draining,
        Done,
        Refused
    };

    struct Producer
    {
        uint32_t frame = 0;
        Callsite site{};
        Words    words{};
        uint32_t wrapper      = 0;
        uint32_t queue        = 0;
        uint32_t serial       = 0;
        uint32_t drainRecord  = 0;
        Stage    stage        = Stage::Open;
        int32_t  parentPlayer = -1;
        int32_t  parentSlot   = -1;
    };

    struct Submit
    {
        uint32_t frame     = 0;
        uint32_t queue     = 0;
        uint32_t read      = 0;
        bool     validRead = false;
    };

    struct Parent
    {
        uint32_t frame  = 0;
        int32_t  player = -1;
        int32_t  slot   = -1;
        bool     armed  = false;
    };

    Submit*                         FindSubmit(uint32_t frame) noexcept;
    void                            FinishDrain(uint32_t frame) noexcept;
    std::array<Producer, kCapacity> producers_{};
    std::array<Submit, kCapacity>   submits_{};
    std::array<Parent, kCapacity>   parents_{};
    size_t                          producerCount_    = 0;
    size_t                          submitCount_      = 0;
    size_t                          parentCount_      = 0;
    uint64_t                        imbalances_       = 0;
    uint64_t                        capacityFailures_ = 0;
};

} // namespace rerevved::capital_grace
