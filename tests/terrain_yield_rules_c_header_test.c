#include <terrain_yield_rules.h>

#include <stddef.h>

int main(void)
{
    TerrainYieldRulesAbiVersionFn versionFn =
        TerrainYieldRulesAbiVersion;
    RegisterTerrainYieldRuleFn registerFn =
        RegisterTerrainYieldRule;
    GetTerrainYieldRuleCountFn countFn =
        GetTerrainYieldRuleCount;
    GetTerrainYieldRuleFn  getFn      = GetTerrainYieldRule;
    EvaluateTerrainYieldFn evaluateFn = EvaluateTerrainYield;

    TerrainYieldRule       rule       = { 0 };
    TerrainYieldRuleInfo   info       = { 0 };
    TerrainYieldQuery      query      = { 0 };
    TerrainYieldEvaluation evaluation = { 0 };

    if (!versionFn || !registerFn || !countFn || !getFn || !evaluateFn ||
        sizeof(rule) != 168 || sizeof(info) != 192 || sizeof(query) != 40 ||
        sizeof(evaluation) != 40 ||
        offsetof(TerrainYieldRule, terrain) != 132 ||
        offsetof(TerrainYieldRule, value) != 144 ||
        offsetof(TerrainYieldRuleInfo, statusFlags) != 148 ||
        offsetof(TerrainYieldQuery, nativeValue) != 12 ||
        offsetof(TerrainYieldEvaluation, finalValue) != 8 ||
        TERRAIN_UNKNOWN != -1 || TERRAIN_SEA != 0 ||
        TERRAIN_PLAINS != 1 || TERRAIN_FOREST != 2 ||
        TERRAIN_HILL != 3 || TERRAIN_DESERT != 4 ||
        TERRAIN_MOUNTAIN != 5 || TERRAIN_COUNT != 6)
    {
        return 1;
    }
    return versionFn() == TERRAIN_YIELD_RULES_ABI_VERSION ? 0 : 2;
}
