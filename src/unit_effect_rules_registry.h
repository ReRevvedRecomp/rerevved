#pragma once

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#include <cstdint>

#include <unit_effect_rules.h>

namespace rerevved::unit_effect_rules
{

bool TryEvaluate(CivilizationId        civilization,
                 UnitTypeId            baseUnitType,
                 UnitIdentityId        identity,
                 UnitEffectId          effect,
                 int32_t               nativeLevel,
                 UnitEffectEvaluation& evaluation);

// Returns the title-owned storage mask for a named native special upgrade.
// This remains an internal bridge so the public ABI exposes named effects
// instead of guest bit numbers.
bool TryGetNativeSpecialUpgradeMask(UnitEffectId effect,
                                    uint32_t&    mask);

int32_t                             NormalizeRequest(const UnitEffectRule* rule, UnitEffectRule& normalized) noexcept;
std::unique_lock<std::shared_mutex> AcquirePublicationLock();
// The caller holds the publication lock and passes normalized requests.
// failingOffset identifies the request if preparation throws or rejects it.
int32_t BuildCandidate(std::span<const UnitEffectRule> requests,
                       std::vector<UnitEffectRule>&    candidate,
                       size_t&                         failingOffset,
                       void (*checkpoint)() = nullptr);
void    CommitCandidate(std::vector<UnitEffectRule>& candidate) noexcept;

void ResetForTests();

} // namespace rerevved::unit_effect_rules
