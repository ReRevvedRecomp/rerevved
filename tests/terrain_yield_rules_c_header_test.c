#include <terrain_yield_rules.h>

#include <stddef.h>

int main(void)
{
    ReRevvedTerrainYieldRulesAbiVersionFn version_fn =
        ReRevvedTerrainYieldRulesAbiVersion;
    ReRevvedRegisterTerrainYieldRuleFn register_fn =
        ReRevvedRegisterTerrainYieldRule;
    ReRevvedGetTerrainYieldRuleCountFn count_fn =
        ReRevvedGetTerrainYieldRuleCount;
    ReRevvedGetTerrainYieldRuleFn  get_fn      = ReRevvedGetTerrainYieldRule;
    ReRevvedEvaluateTerrainYieldFn evaluate_fn = ReRevvedEvaluateTerrainYield;

    ReRevvedTerrainYieldRule       rule       = { 0 };
    ReRevvedTerrainYieldRuleInfo   info       = { 0 };
    ReRevvedTerrainYieldQuery      query      = { 0 };
    ReRevvedTerrainYieldEvaluation evaluation = { 0 };

    if (!version_fn || !register_fn || !count_fn || !get_fn || !evaluate_fn ||
        sizeof(rule) != 168 || sizeof(info) != 192 || sizeof(query) != 40 ||
        sizeof(evaluation) != 40 ||
        offsetof(ReRevvedTerrainYieldRule, terrain) != 132 ||
        offsetof(ReRevvedTerrainYieldRule, value) != 144 ||
        offsetof(ReRevvedTerrainYieldRuleInfo, status_flags) != 148 ||
        offsetof(ReRevvedTerrainYieldQuery, native_value) != 12 ||
        offsetof(ReRevvedTerrainYieldEvaluation, final_value) != 8 ||
        REREVVED_TERRAIN_UNKNOWN != -1 || REREVVED_TERRAIN_SEA != 0 ||
        REREVVED_TERRAIN_PLAINS != 1 || REREVVED_TERRAIN_FOREST != 2 ||
        REREVVED_TERRAIN_HILL != 3 || REREVVED_TERRAIN_DESERT != 4 ||
        REREVVED_TERRAIN_MOUNTAIN != 5 || REREVVED_TERRAIN_COUNT != 6)
    {
        return 1;
    }
    return version_fn() == REREVVED_TERRAIN_YIELD_RULES_ABI_VERSION ? 0 : 2;
}
