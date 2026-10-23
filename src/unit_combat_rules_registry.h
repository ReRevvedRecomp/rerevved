#pragma once

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#include <unit_combat_rules.h>

namespace rerevved::unit_combat_rules
{

bool TryEvaluate(CivilizationId        civilization,
                 UnitTypeId            baseUnitType,
                 UnitIdentityId        identity,
                 TerrainId             terrain,
                 UnitCombatProperty    property,
                 UnitCombatEvaluation& evaluation);

int32_t                             NormalizeRequest(const UnitCombatRule* rule, UnitCombatRule& normalized) noexcept;
std::unique_lock<std::shared_mutex> AcquirePublicationLock();
// The caller holds the publication lock and passes normalized requests.
// failingOffset identifies the request if preparation throws or rejects it.
int32_t BuildCandidate(std::span<const UnitCombatRule> requests,
                       std::vector<UnitCombatRule>&    candidate,
                       size_t&                         failingOffset,
                       void (*checkpoint)() = nullptr);
void    CommitCandidate(std::vector<UnitCombatRule>& candidate) noexcept;

void ResetForTests();

} // namespace rerevved::unit_combat_rules
