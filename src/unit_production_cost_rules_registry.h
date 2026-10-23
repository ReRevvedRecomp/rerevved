#pragma once

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#include <cstdint>

#include <unit_production_cost_rules.h>

namespace rerevved::unit_production_cost_rules
{

bool TryEvaluate(CivilizationId                civilization,
                 UnitTypeId                    baseUnitType,
                 UnitIdentityId                identity,
                 UnitProductionCostEvaluation& evaluation);

int32_t                             NormalizeRequest(const UnitProductionCostRule* rule, UnitProductionCostRule& normalized) noexcept;
std::unique_lock<std::shared_mutex> AcquirePublicationLock();
// The caller holds the publication lock and passes normalized requests.
// failingOffset identifies the request if preparation throws or rejects it.
int32_t BuildCandidate(std::span<const UnitProductionCostRule> requests,
                       std::vector<UnitProductionCostRule>&    candidate,
                       size_t&                                 failingOffset,
                       void (*checkpoint)() = nullptr);
void    CommitCandidate(std::vector<UnitProductionCostRule>& candidate) noexcept;

void ResetForTests();

} // namespace rerevved::unit_production_cost_rules
