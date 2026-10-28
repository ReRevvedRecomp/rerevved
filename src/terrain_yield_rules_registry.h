#pragma once

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#include <cstdint>

#include <terrain_yield_rules.h>

namespace rerevved::terrain_yield_rules
{

bool TryEvaluate(TerrainId               terrain,
                 TerrainYieldComponent   component,
                 int32_t                 nativeValue,
                 TerrainYieldEvaluation& evaluation);

bool TryMapGuestTerrain(int32_t guestTerrain, TerrainId& terrain);

bool TryAddChecked(int64_t accumulator, int64_t value, int64_t& result);

int32_t                             NormalizeRequest(const TerrainYieldRule* rule, TerrainYieldRule& normalized) noexcept;
std::unique_lock<std::shared_mutex> AcquirePublicationLock();
// The caller holds the publication lock and passes normalized requests.
// failingOffset identifies the request if preparation throws or rejects it.
int32_t BuildCandidate(std::span<const TerrainYieldRule> requests,
                       std::vector<TerrainYieldRule>&    candidate,
                       size_t&                           failingOffset,
                       void (*checkpoint)()                        = nullptr,
                       std::span<const TerrainYieldRule> removed   = {},
                       bool                              exclusive = false);
void    CommitCandidate(std::vector<TerrainYieldRule>& candidate) noexcept;

void ResetForTests();

} // namespace rerevved::terrain_yield_rules
