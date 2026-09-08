#include <stddef.h>

#include <unit_combat_rules.h>

int main(void)
{
    if (sizeof(ReRevvedUnitCombatRule) != 168 ||
        offsetof(ReRevvedUnitCombatRule, terrain) != 144 ||
        offsetof(ReRevvedUnitCombatRule, percentageDelta) != 152 ||
        sizeof(ReRevvedUnitCombatRuleInfo) != 192 ||
        sizeof(ReRevvedUnitCombatQuery) != 40 ||
        sizeof(ReRevvedUnitCombatEvaluation) != 40 ||
        REREVVED_UNIT_COMBAT_ATTACK != 0 ||
        REREVVED_UNIT_COMBAT_DEFENSE != 1 ||
        REREVVED_TERRAIN_FOREST != 2)
    {
        return 1;
    }
    return ReRevvedUnitCombatRulesAbiVersion() ==
                   REREVVED_UNIT_COMBAT_RULES_ABI_VERSION
               ? 0
               : 2;
}
