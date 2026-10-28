#pragma once

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#include <cstdint>

#include <unique_unit_rules.h>

namespace rerevved::unique_unit_rules
{

bool TryEvaluate(CivilizationId              civilization,
                 UnitTypeId                  baseUnitType,
                 UnitIdentityId              identity,
                 UniqueUnitScalarProperty    property,
                 int32_t                     nativeValue,
                 UniqueUnitScalarEvaluation& evaluation);

int32_t                             NormalizeRequest(const UniqueUnitScalarRule* rule, UniqueUnitScalarRule& normalized) noexcept;
std::unique_lock<std::shared_mutex> AcquirePublicationLock();
// The caller holds the publication lock and passes normalized requests.
// failingOffset identifies the request if preparation throws or rejects it.
int32_t BuildCandidate(std::span<const UniqueUnitScalarRule> requests,
                       std::vector<UniqueUnitScalarRule>&    candidate,
                       size_t&                               failingOffset,
                       void (*checkpoint)()                            = nullptr,
                       std::span<const UniqueUnitScalarRule> removed   = {},
                       bool                                  exclusive = false);
void    CommitCandidate(std::vector<UniqueUnitScalarRule>& candidate) noexcept;

void ResetForTests();

} // namespace rerevved::unique_unit_rules
