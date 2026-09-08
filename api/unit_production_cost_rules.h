// Public C ABI for identity-targeted unit production cost percentages.
//
// Mods resolve these entry points from the host process and check
// UnitProductionCostRulesAbiVersion before calling them. Registrations are
// copied by the host and target identities exposed by Unit Catalog ABI 2.
// Percentages scale the native integer cost scalar before multiplication by
// the unit's production factor and division by two. Both divisions truncate
// toward zero; percentages do not apply directly to the displayed cost.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(UNIT_PRODUCTION_COST_RULES_API_EXPORTS)
#if defined(_WIN32)
#define UNIT_PRODUCTION_COST_RULES_API __declspec(dllexport)
#else
#define UNIT_PRODUCTION_COST_RULES_API __attribute__((visibility("default")))
#endif
#else
#define UNIT_PRODUCTION_COST_RULES_API
#endif

#define UNIT_PRODUCTION_COST_RULES_ABI_VERSION 2u
#define UNIT_PRODUCTION_COST_RULE_ID_CAPACITY  64u

enum
{
    UNIT_PRODUCTION_COST_RULES_OK                    = 0,
    UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT  = -10,
    UNIT_PRODUCTION_COST_RULES_ERR_BUFFER_TOO_SMALL  = -11,
    UNIT_PRODUCTION_COST_RULES_ERR_DUPLICATE_RULE_ID = -12,
    UNIT_PRODUCTION_COST_RULES_ERR_INTERNAL          = -13,
};

enum
{
    UNIT_PRODUCTION_COST_EVALUATION_OUT_OF_RANGE = 1u << 0,
};

typedef struct UnitProductionCostRule
{
    uint32_t       structSize;
    char           providerId[UNIT_PRODUCTION_COST_RULE_ID_CAPACITY];
    char           ruleId[UNIT_PRODUCTION_COST_RULE_ID_CAPACITY];
    CivilizationId civilization;
    UnitTypeId     baseUnitType;
    UnitIdentityId identity;
    int32_t        percentageDelta; // Additive percentage points applied to the native integer cost scalar.
    int32_t        reserved[5];
} UnitProductionCostRule;

typedef struct UnitProductionCostRuleInfo
{
    uint32_t       structSize; // Current producer size. Callers may pass any buffer at least 152 bytes.
    char           providerId[UNIT_PRODUCTION_COST_RULE_ID_CAPACITY];
    char           ruleId[UNIT_PRODUCTION_COST_RULE_ID_CAPACITY];
    CivilizationId civilization;
    UnitTypeId     baseUnitType;
    UnitIdentityId identity;
    int32_t        percentageDelta;
    uint32_t       statusFlags;
    int32_t        reserved[10];
} UnitProductionCostRuleInfo;

typedef struct UnitProductionCostQuery
{
    uint32_t       structSize;
    CivilizationId civilization;
    UnitTypeId     baseUnitType;
    UnitIdentityId identity;
    int32_t        reserved[6];
} UnitProductionCostQuery;

typedef struct UnitProductionCostEvaluation
{
    uint32_t structSize; // Current producer size. Callers may pass any buffer at least 20 bytes.
    int32_t  nativePercent;
    int32_t  finalPercent;
    uint32_t statusFlags;
    uint32_t additiveCount;
    int32_t  reserved[5];
} UnitProductionCostEvaluation;

typedef uint32_t (*UnitProductionCostRulesAbiVersionFn)(void);
typedef int32_t (*RegisterUnitProductionCostRuleFn)(
    const UnitProductionCostRule* rule);
typedef int32_t (*GetUnitProductionCostRuleCountFn)(uint32_t* outCount);
typedef int32_t (*GetUnitProductionCostRuleFn)(
    uint32_t                    index,
    UnitProductionCostRuleInfo* out,
    uint32_t                    outSize);
typedef int32_t (*EvaluateUnitProductionCostFn)(
    const UnitProductionCostQuery* query,
    UnitProductionCostEvaluation*  out,
    uint32_t                       outSize);

#ifdef __cplusplus
extern "C"
{
#endif

    UNIT_PRODUCTION_COST_RULES_API uint32_t UnitProductionCostRulesAbiVersion(void);
    UNIT_PRODUCTION_COST_RULES_API int32_t  RegisterUnitProductionCostRule(
        const UnitProductionCostRule* rule);
    UNIT_PRODUCTION_COST_RULES_API int32_t GetUnitProductionCostRuleCount(uint32_t* outCount);
    UNIT_PRODUCTION_COST_RULES_API int32_t GetUnitProductionCostRule(
        uint32_t                    index,
        UnitProductionCostRuleInfo* out,
        uint32_t                    outSize);
    UNIT_PRODUCTION_COST_RULES_API int32_t EvaluateUnitProductionCost(
        const UnitProductionCostQuery* query,
        UnitProductionCostEvaluation*  out,
        uint32_t                       outSize);

#ifdef __cplusplus
} // extern "C"
#endif

#undef UNIT_PRODUCTION_COST_RULES_API
