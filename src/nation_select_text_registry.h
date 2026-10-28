#pragma once

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#include <nation_select_text.h>

namespace rerevved::nation_select_text
{

bool TryEvaluate(const NationSelectTextQuery& query,
                 NationSelectTextEvaluation&  evaluation);

// Consumer presentation includes the live OCC choice before game initialization.
bool TryEvaluateEraText(CivilizationId              civilization,
                        UnlockEra                   era,
                        EraAbilityId                nativeAbility,
                        NationSelectTextEvaluation& presentation);

int32_t                             NormalizeRequest(const NationSelectTextRule* rule, NationSelectTextRule& normalized) noexcept;
std::unique_lock<std::shared_mutex> AcquirePublicationLock();
// The caller holds the publication lock and passes normalized requests.
// failingOffset identifies the request if preparation throws or rejects it.
int32_t BuildCandidate(std::span<const NationSelectTextRule> requests,
                       std::vector<NationSelectTextRule>&    candidate,
                       size_t&                               failingOffset,
                       void (*checkpoint)()                            = nullptr,
                       std::span<const NationSelectTextRule> removed   = {},
                       bool                                  exclusive = false);
void    CommitCandidate(std::vector<NationSelectTextRule>& candidate) noexcept;

void ResetForTests();

} // namespace rerevved::nation_select_text
