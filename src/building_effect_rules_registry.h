#pragma once

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#include <cstdint>

#include <building_effect_rules.h>

namespace rerevved::building_effect_rules
{

// Normalized requests are V2 records with zero-filled ID tails. V1 requests
// normalize to amount 1.
int32_t                             NormalizeV1Request(const BuildingEffectRule* rule, BuildingEffectRuleV2& normalized) noexcept;
int32_t                             NormalizeRequest(const BuildingEffectRuleV2* rule, BuildingEffectRuleV2& normalized) noexcept;
std::unique_lock<std::shared_mutex> AcquirePublicationLock();
// The caller holds the publication lock and supplies normalized requests.
// An identical request is skipped; a changed amount returns ERR_CONFLICT.
// A sum outside int32 returns ERR_INVALID_ARGUMENT. failingOffset identifies
// the request that was rejected or threw.
int32_t BuildCandidate(std::span<const BuildingEffectRuleV2> requests,
                       std::vector<BuildingEffectRuleV2>&    candidate,
                       size_t&                               failingOffset,
                       void (*checkpoint)()                            = nullptr,
                       std::span<const BuildingEffectRuleV2> removed   = {},
                       bool                                  exclusive = false);
void    CommitCandidate(std::vector<BuildingEffectRuleV2>& candidate) noexcept;

// False when no rule is retained. Otherwise the checked sum of all retained amounts.
bool   TryGetEffectiveAmount(int32_t& amount);
size_t RuleCount();

void ResetForTests();

// Hook decision core for guest CityUpdate. These functions read no guest memory;
// the hook glue supplies guest values and applies the reported writes.

constexpr int32_t kPlayerCount   = 6;  // Owner indices are [0, kPlayerCount).
constexpr int32_t kModeCacheOnly = -1; // Compose the cached Gold halfword only.
constexpr int32_t kModeCredit    = 0;  // Cache, owner aggregate, and later reserve credit.

enum class HookStatus
{
    kUnmatched, // No action; native behavior.
    kMatched,   // The reported writes are valid.
    kOverflow,  // Matched, but a checked range or token capacity failed; native fallback.
};

struct CacheQuery
{
    uint32_t stackPointer;   // Guest r1 at the hook.
    uint32_t cityRecord;     // Guest city record address.
    int32_t  mode;           // kModeCacheOnly or kModeCredit.
    int32_t  ownerIndex;     // Aggregate and credit owner.
    int32_t  cityOwner;      // The city's current owner.
    int32_t  civilization;   // Civilization of cityOwner.
    bool     hasLibrary;     // The city has a Library.
    bool     excludedPath;   // Native excluded-player path, or guest state could not be read: do nothing.
    int16_t  nativeCache;    // City +0x46.
    int32_t  ownerAggregate; // Owner row +0x0C; read in kModeCredit only.
    int32_t  ownerReserve;   // Owner reserve word; read in kModeCredit only.
    int32_t  nativeCredit;   // Native credit Gold; read in kModeCredit only.
};

struct CacheDecision
{
    HookStatus status         = HookStatus::kUnmatched;
    bool       applyCache     = false;
    int16_t    newCache       = 0;
    bool       applyAggregate = false;
    int32_t    newAggregate   = 0;
};

// kModeCredit creates a pending token with all writes atomically; failure
// reports no writes. Discard keys <= stackPointer first: downward-stack frames
// at or above it have finished or are the same traversal.
CacheDecision EvaluateCachePhase(const CacheQuery& query);

struct CreditDecision
{
    HookStatus status         = HookStatus::kUnmatched;
    bool       apply          = false;
    int32_t    newReserveGold = 0; // Replacement for the guest value stored at the reserve write.
};

// Pure arithmetic of the credit phase: reserveWithCredit + amount must fit int32.
CreditDecision DecideCredit(int32_t reserveWithCredit, int32_t amount);

// Discards tokens deeper than stackPointer, then removes the token at
// stackPointer. Returns its amount only when city record and owner also match.
bool TakePendingCredit(uint32_t stackPointer, uint32_t cityRecord, int32_t ownerIndex, int32_t& amount);

struct CreditQuery
{
    uint32_t stackPointer;
    uint32_t cityRecord;
    int32_t  ownerIndex;
    int32_t  cityOwner; // Live values, gated exactly as in CacheQuery.
    int32_t  civilization;
    bool     hasLibrary;
    bool     excludedPath;      // The token is still consumed.
    int32_t  reserveWithCredit; // Guest r8: old reserve plus native credit Gold.
};

// TakePendingCredit then DecideCredit. Uses the amount recorded by the cache
// phase, never a fresh registry read.
CreditDecision EvaluateCreditPhase(const CreditQuery& query);

constexpr size_t kPendingTokenCapacity = 8;

} // namespace rerevved::building_effect_rules
