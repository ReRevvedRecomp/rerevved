#pragma once

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#include <cstdint>

#include <wonder_effect_rules.h>

namespace rerevved::wonder_effect_rules
{

// Normalized requests are V2 records with zero-filled ID tails. V1 requests
// normalize to amount 0. A valid Culture replacement request returns
// ERR_UNAVAILABLE, so only Religion grant rules reach a candidate.
int32_t                             NormalizeV1Request(const WonderEffectRule* rule, WonderEffectRuleV2& normalized) noexcept;
int32_t                             NormalizeRequest(const WonderEffectRuleV2* rule, WonderEffectRuleV2& normalized) noexcept;
std::unique_lock<std::shared_mutex> AcquirePublicationLock();
// Caller holds publication lock and passes normalized requests. Identical
// retained identity skips; a different request returns ERR_CONFLICT.
// failingOffset names the rejected/throwing request.
int32_t BuildCandidate(std::span<const WonderEffectRuleV2> requests,
                       std::vector<WonderEffectRuleV2>&    candidate,
                       size_t&                             failingOffset,
                       void (*checkpoint)()                          = nullptr,
                       std::span<const WonderEffectRuleV2> removed   = {},
                       bool                                exclusive = false);
void    CommitCandidate(std::vector<WonderEffectRuleV2>& candidate) noexcept;

// True when any Religion grant rule is retained. Lock-free for the guest hook.
bool   IsStonehengeReligionActive() noexcept;
size_t RuleCount();

void ResetForTests();

// Hook decision core for the Stonehenge claim boundary after CityUpdate's
// guarded call to the claim helper. It reads no guest memory; the hook glue
// supplies guest values and performs the guest calls.

constexpr int32_t kStonehengeWonderIndex = 3;   // Normalized Wonder index of item 203.
constexpr int32_t kCitySlots             = 128; // City indices are [0, kCitySlots).
constexpr int32_t kOwnerLimit            = 32;  // Owner bits fit one 32-bit ownership word.

struct GrantQuery
{
    int32_t wonderIndex;    // Guest r27 at the hook.
    int32_t cityIndex;      // Guest r23 at the hook.
    int32_t claimOwner;     // Stonehenge claim owner word.
    int32_t claimCity;      // Stonehenge claim city word.
    int32_t liveOwner;      // Signed owner byte at the city record.
    int32_t playerCount;    // Ordinary player count word.
    int32_t excludedPlayer; // Excluded player word.
    bool    ruleActive;     // IsStonehengeReligionActive().
    bool    readsOk;        // Every guarded guest read succeeded.
};

// True only for a newly committed Stonehenge claim whose claim pair agrees
// with the live city owner, for an ordinary non-excluded player. The claim
// owner is then the grant owner.
bool ShouldGrantReligion(const GrantQuery& query) noexcept;

} // namespace rerevved::wonder_effect_rules
