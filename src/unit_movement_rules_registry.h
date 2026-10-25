#pragma once

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#include <cstdint>

#include <unit_movement_rules.h>

namespace rerevved::unit_movement_rules
{

bool TryEvaluate(CivilizationId          civilization,
                 UnitTypeId              baseUnitType,
                 UnitIdentityId          identity,
                 int32_t                 nativeValue,
                 UnitMovementEvaluation& evaluation);

int32_t                             NormalizeRequest(const UnitMovementRule* rule, UnitMovementRule& normalized) noexcept;
std::unique_lock<std::shared_mutex> AcquirePublicationLock();
// The caller holds the publication lock and passes normalized requests.
// failingOffset identifies the request if preparation throws or rejects it.
int32_t BuildCandidate(std::span<const UnitMovementRule> requests,
                       std::vector<UnitMovementRule>&    candidate,
                       size_t&                           failingOffset,
                       void (*checkpoint)()                        = nullptr,
                       std::span<const UnitMovementRule> removed   = {},
                       bool                              exclusive = false);
void    CommitCandidate(std::vector<UnitMovementRule>& candidate) noexcept;

void ResetForTests();

} // namespace rerevved::unit_movement_rules
