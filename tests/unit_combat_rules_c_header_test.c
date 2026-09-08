#include <stddef.h>

#include <unit_combat_rules.h>

int main(void)
{
    if (sizeof(UnitCombatRule) != 168 ||
        offsetof(UnitCombatRule, terrain) != 144 ||
        offsetof(UnitCombatRule, percentageDelta) != 152 ||
        sizeof(UnitCombatRuleInfo) != 192 ||
        sizeof(UnitCombatQuery) != 40 ||
        sizeof(UnitCombatEvaluation) != 40 ||
        UNIT_COMBAT_ATTACK != 0 ||
        UNIT_COMBAT_DEFENSE != 1 ||
        TERRAIN_FOREST != 2)
    {
        return 1;
    }
    return UnitCombatRulesAbiVersion() ==
                   UNIT_COMBAT_RULES_ABI_VERSION
               ? 0
               : 2;
}
