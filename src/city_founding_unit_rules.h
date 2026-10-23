#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

namespace rerevved::city_founding_unit
{

struct Rule
{
    uint32_t structSize;
    char     providerId[64];
    char     ruleId[64];
    uint32_t enable;
    int32_t  reserved[6];
};

constexpr int32_t kInvalidArgument = -10;
constexpr int32_t kConflict        = -12;
constexpr int32_t kInternalError   = -13;

bool                                IsEnabled();
size_t                              RuleCount();
int32_t                             NormalizeRequest(const Rule* rule, Rule& normalized) noexcept;
std::unique_lock<std::shared_mutex> AcquirePublicationLock();
int32_t                             BuildCandidate(std::span<const Rule> requests, std::vector<Rule>& candidate, size_t& failingOffset, void (*checkpoint)() = nullptr);
void                                CommitCandidate(std::vector<Rule>& candidate) noexcept;
void                                ResetForTests();

struct Input
{
    uint32_t frame;
    int32_t  citySlot;
    int32_t  owner;
};

enum class Decision
{
    Disabled,
    InvalidFrame,
    OriginMismatch,
    InvalidOwner,
    InvalidCity,
    Reentry,
    Admitted
};

struct Outcome
{
    Decision decision       = Decision::Disabled;
    uint32_t origin         = 0;
    int32_t  x              = 0;
    int32_t  y              = 0;
    bool     creatorCalled  = false;
    int32_t  unitSlot       = -1;
    bool     retainedUnit   = false;
    bool     loyaltyWritten = false;
    uint32_t upgradesBefore = 0;
    uint32_t upgradesAfter  = 0;
};

class World
{
public:
    virtual ~World()                                                          = default;
    virtual bool    Read(uint32_t address, void* out, uint32_t size)          = 0;
    virtual bool    Write(uint32_t address, const void* bytes, uint32_t size) = 0;
    virtual int32_t Create(int32_t owner, int32_t type, int32_t x, int32_t y) = 0;
};

// One allocation per admitted invocation, with a guard held across native callbacks.
Outcome Apply(World& world, Input input);

} // namespace rerevved::city_founding_unit
