// Public C ABI for identity-targeted unit combat percentages on Forest.
//
// Mods resolve these entry points from the host process and check
// UnitCombatRulesAbiVersion before calling them. Registrations are
// copied by the host and target identities exposed by Unit Catalog ABI 2.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(UNIT_COMBAT_RULES_API_EXPORTS)
#if defined(_WIN32)
#define UNIT_COMBAT_RULES_API __declspec(dllexport)
#else
#define UNIT_COMBAT_RULES_API __attribute__((visibility("default")))
#endif
#else
#define UNIT_COMBAT_RULES_API
#endif

#define UNIT_COMBAT_RULES_ABI_VERSION 2u
#define UNIT_COMBAT_RULE_ID_CAPACITY  64u

enum
{
    UNIT_COMBAT_RULES_OK                    = 0,
    UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT  = -10,
    UNIT_COMBAT_RULES_ERR_BUFFER_TOO_SMALL  = -11,
    UNIT_COMBAT_RULES_ERR_DUPLICATE_RULE_ID = -12,
    UNIT_COMBAT_RULES_ERR_INTERNAL          = -13,
};

typedef int32_t UnitCombatProperty;

enum
{
    UNIT_COMBAT_ATTACK  = 0,
    UNIT_COMBAT_DEFENSE = 1,
};

enum
{
    UNIT_COMBAT_EVALUATION_OUT_OF_RANGE = 1u << 0,
};

typedef struct UnitCombatRule
{
    uint32_t           structSize;
    char               providerId[UNIT_COMBAT_RULE_ID_CAPACITY];
    char               ruleId[UNIT_COMBAT_RULE_ID_CAPACITY];
    CivilizationId     civilization;
    UnitTypeId         baseUnitType;
    UnitIdentityId     identity;
    TerrainId          terrain;
    UnitCombatProperty property;
    int32_t            percentageDelta; // Additive percentage points relative to the native 100 percent scalar.
    int32_t            reserved[3];
} UnitCombatRule;

typedef struct UnitCombatRuleInfo
{
    uint32_t           structSize; // Current producer size. Callers may pass any buffer at least 160 bytes.
    char               providerId[UNIT_COMBAT_RULE_ID_CAPACITY];
    char               ruleId[UNIT_COMBAT_RULE_ID_CAPACITY];
    CivilizationId     civilization;
    UnitTypeId         baseUnitType;
    UnitIdentityId     identity;
    TerrainId          terrain;
    UnitCombatProperty property;
    int32_t            percentageDelta;
    uint32_t           statusFlags;
    int32_t            reserved[8];
} UnitCombatRuleInfo;

typedef struct UnitCombatQuery
{
    uint32_t           structSize;
    CivilizationId     civilization;
    UnitTypeId         baseUnitType;
    UnitIdentityId     identity;
    TerrainId          terrain;
    UnitCombatProperty property;
    int32_t            reserved[4];
} UnitCombatQuery;

typedef struct UnitCombatEvaluation
{
    uint32_t structSize; // Current producer size. Callers may pass any buffer at least 20 bytes.
    int32_t  nativePercent;
    int32_t  finalPercent;
    uint32_t statusFlags;
    uint32_t additiveCount;
    int32_t  reserved[5];
} UnitCombatEvaluation;

typedef uint32_t (*UnitCombatRulesAbiVersionFn)(void);
typedef int32_t (*RegisterUnitCombatRuleFn)(
    const UnitCombatRule* rule);
typedef int32_t (*GetUnitCombatRuleCountFn)(uint32_t* outCount);
typedef int32_t (*GetUnitCombatRuleFn)(
    uint32_t            index,
    UnitCombatRuleInfo* out,
    uint32_t            outSize);
typedef int32_t (*EvaluateUnitCombatFn)(
    const UnitCombatQuery* query,
    UnitCombatEvaluation*  out,
    uint32_t               outSize);

#ifdef __cplusplus
extern "C"
{
#endif

    UNIT_COMBAT_RULES_API uint32_t UnitCombatRulesAbiVersion(void);
    UNIT_COMBAT_RULES_API int32_t  RegisterUnitCombatRule(
        const UnitCombatRule* rule);
    UNIT_COMBAT_RULES_API int32_t GetUnitCombatRuleCount(
        uint32_t* outCount);
    UNIT_COMBAT_RULES_API int32_t GetUnitCombatRule(
        uint32_t            index,
        UnitCombatRuleInfo* out,
        uint32_t            outSize);
    UNIT_COMBAT_RULES_API int32_t EvaluateUnitCombat(
        const UnitCombatQuery* query,
        UnitCombatEvaluation*  out,
        uint32_t               outSize);

#ifdef __cplusplus
} // extern "C"
#endif

#undef UNIT_COMBAT_RULES_API
