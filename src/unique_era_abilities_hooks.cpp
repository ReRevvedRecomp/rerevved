#include "unique_era_abilities_registry.h"

#include <cstdint>

#include <rex/ppc.h>

namespace
{

constexpr int32_t kHorsebackRidingTechnologyId = 4;

// The retail UEA 57 branch is unreachable because 57 is absent from the
// retail table. ABI 2 reuses only its grant scaffold and keeps 57 invalid.
constexpr int32_t kMonarchyOwnershipOffset        = 72;
constexpr int32_t kHorsebackRidingOwnershipOffset = 16;

} // namespace

void ReRevvedApplyUniqueEraAbilityCell(PPCRegister& cellOffset,
                                       PPCRegister& unlockEra,
                                       PPCRegister& nativeAbility)
{
    if ((cellOffset.u32 & 3u) != 0 ||
        unlockEra.s32 < REREVVED_UNIQUE_ERA_ANCIENT ||
        unlockEra.s32 > REREVVED_UNIQUE_ERA_MODERN)
    {
        return;
    }

    const uint32_t wordIndex = cellOffset.u32 / sizeof(uint32_t);
    if (wordIndex < static_cast<uint32_t>(unlockEra.s32))
    {
        return;
    }
    const uint32_t civilizationWord =
        wordIndex - static_cast<uint32_t>(unlockEra.s32);
    if ((civilizationWord & 3u) != 0)
    {
        return;
    }

    const auto civilization = static_cast<ReRevvedCivilizationId>(
        civilizationWord / 4u);
    ReRevvedUniqueEraAbilityCellEvaluation evaluation{};
    if (rerevved::unique_era_abilities::TryEvaluate(civilization,
                                                    unlockEra.s32,
                                                    nativeAbility.s32,
                                                    evaluation))
    {
        nativeAbility.s64 = evaluation.effectiveAbility;
    }
}

void ReRevvedApplyBarbarianVillageCityReplacement(PPCRegister& civilization)
{
    if (civilization.s32 != REREVVED_CIVILIZATION_MONGOLIAN)
    {
        return;
    }

    ReRevvedUniqueEraAbilityCellEvaluation evaluation{};
    if (rerevved::unique_era_abilities::TryEvaluate(
            REREVVED_CIVILIZATION_MONGOLIAN,
            REREVVED_UNIQUE_ERA_ANCIENT,
            REREVVED_UNIQUE_ERA_ABILITY_BARBARIAN_VILLAGES_BECOME_CITIES,
            evaluation) &&
        evaluation.effectiveAbility !=
            REREVVED_UNIQUE_ERA_ABILITY_BARBARIAN_VILLAGES_BECOME_CITIES)
    {
        // This native effect hard-codes civilization 14 instead of querying
        // UEA 40. A non-civilization value selects its ordinary reward path.
        civilization.s64 = REREVVED_CIVILIZATION_UNKNOWN;
    }
}

void ReRevvedBeginHorsebackRidingOwnershipCheck(PPCRegister& ownershipBase)
{
    ownershipBase.s64 -=
        kMonarchyOwnershipOffset - kHorsebackRidingOwnershipOffset;
}

void ReRevvedEndHorsebackRidingOwnershipCheck(PPCRegister& ownershipBase)
{
    ownershipBase.s64 +=
        kMonarchyOwnershipOffset - kHorsebackRidingOwnershipOffset;
}

void ReRevvedSelectHorsebackRidingAbility(PPCRegister& ability)
{
    ability.s64 =
        REREVVED_UNIQUE_ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING;
}

void ReRevvedSelectHorsebackRidingTechnology(PPCRegister& technology)
{
    technology.s64 = kHorsebackRidingTechnologyId;
}
