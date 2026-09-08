#include <terrain_yield_rules.h>

#include <stddef.h>

int main(void)
{
    ReRevvedTerrainYieldRulesAbiVersionFn versionFn =
        ReRevvedTerrainYieldRulesAbiVersion;
    ReRevvedRegisterTerrainYieldRuleFn registerFn =
        ReRevvedRegisterTerrainYieldRule;
    ReRevvedGetTerrainYieldRuleCountFn countFn =
        ReRevvedGetTerrainYieldRuleCount;
    ReRevvedGetTerrainYieldRuleFn  getFn      = ReRevvedGetTerrainYieldRule;
    ReRevvedEvaluateTerrainYieldFn evaluateFn = ReRevvedEvaluateTerrainYield;

    ReRevvedTerrainYieldRule       rule       = { 0 };
    ReRevvedTerrainYieldRuleInfo   info       = { 0 };
    ReRevvedTerrainYieldQuery      query      = { 0 };
    ReRevvedTerrainYieldEvaluation evaluation = { 0 };

    if (!versionFn || !registerFn || !countFn || !getFn || !evaluateFn ||
        sizeof(rule) != 168 || sizeof(info) != 192 || sizeof(query) != 40 ||
        sizeof(evaluation) != 40 ||
        offsetof(ReRevvedTerrainYieldRule, terrain) != 132 ||
        offsetof(ReRevvedTerrainYieldRule, value) != 144 ||
        offsetof(ReRevvedTerrainYieldRuleInfo, statusFlags) != 148 ||
        offsetof(ReRevvedTerrainYieldQuery, nativeValue) != 12 ||
        offsetof(ReRevvedTerrainYieldEvaluation, finalValue) != 8 ||
        REREVVED_TERRAIN_UNKNOWN != -1 || REREVVED_TERRAIN_SEA != 0 ||
        REREVVED_TERRAIN_PLAINS != 1 || REREVVED_TERRAIN_FOREST != 2 ||
        REREVVED_TERRAIN_HILL != 3 || REREVVED_TERRAIN_DESERT != 4 ||
        REREVVED_TERRAIN_MOUNTAIN != 5 || REREVVED_TERRAIN_COUNT != 6)
    {
        return 1;
    }
    return versionFn() == REREVVED_TERRAIN_YIELD_RULES_ABI_VERSION ? 0 : 2;
}
