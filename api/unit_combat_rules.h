// Public C ABI for identity-targeted unit combat percentages on Forest.
//
// Mods resolve these entry points from the host process and check
// ReRevvedUnitCombatRulesAbiVersion before calling them. Registrations are
// copied by the host and target identities exposed by Unit Catalog ABI 1.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(REREVVED_UNIT_COMBAT_RULES_API_EXPORTS)
#if defined(_WIN32)
#define REREVVED_UNIT_COMBAT_RULES_API __declspec(dllexport)
#else
#define REREVVED_UNIT_COMBAT_RULES_API __attribute__((visibility("default")))
#endif
#else
#define REREVVED_UNIT_COMBAT_RULES_API
#endif

#define REREVVED_UNIT_COMBAT_RULES_ABI_VERSION 1u
#define REREVVED_UNIT_COMBAT_RULE_ID_CAPACITY  64u

enum
{
    REREVVED_UNIT_COMBAT_RULES_OK                    = 0,
    REREVVED_UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT  = -10,
    REREVVED_UNIT_COMBAT_RULES_ERR_BUFFER_TOO_SMALL  = -11,
    REREVVED_UNIT_COMBAT_RULES_ERR_DUPLICATE_RULE_ID = -12,
    REREVVED_UNIT_COMBAT_RULES_ERR_INTERNAL          = -13,
};

typedef int32_t ReRevvedUnitCombatProperty;

enum
{
    REREVVED_UNIT_COMBAT_ATTACK  = 0,
    REREVVED_UNIT_COMBAT_DEFENSE = 1,
};

enum
{
    REREVVED_UNIT_COMBAT_EVALUATION_OUT_OF_RANGE = 1u << 0,
};

typedef struct ReRevvedUnitCombatRule
{
    uint32_t                   struct_size;
    char                       provider_id[REREVVED_UNIT_COMBAT_RULE_ID_CAPACITY];
    char                       rule_id[REREVVED_UNIT_COMBAT_RULE_ID_CAPACITY];
    ReRevvedCivilizationId     civilization;
    ReRevvedUnitTypeId         base_unit_type;
    ReRevvedUnitIdentityId     identity;
    ReRevvedTerrainId          terrain;
    ReRevvedUnitCombatProperty property;
    // Additive percentage points relative to the native 100 percent scalar.
    int32_t percentage_delta;
    int32_t reserved[3];
} ReRevvedUnitCombatRule;

typedef struct ReRevvedUnitCombatRuleInfo
{
    // Current producer size. Callers may pass any buffer at least 160 bytes.
    uint32_t                   struct_size;
    char                       provider_id[REREVVED_UNIT_COMBAT_RULE_ID_CAPACITY];
    char                       rule_id[REREVVED_UNIT_COMBAT_RULE_ID_CAPACITY];
    ReRevvedCivilizationId     civilization;
    ReRevvedUnitTypeId         base_unit_type;
    ReRevvedUnitIdentityId     identity;
    ReRevvedTerrainId          terrain;
    ReRevvedUnitCombatProperty property;
    int32_t                    percentage_delta;
    uint32_t                   status_flags;
    int32_t                    reserved[8];
} ReRevvedUnitCombatRuleInfo;

typedef struct ReRevvedUnitCombatQuery
{
    uint32_t                   struct_size;
    ReRevvedCivilizationId     civilization;
    ReRevvedUnitTypeId         base_unit_type;
    ReRevvedUnitIdentityId     identity;
    ReRevvedTerrainId          terrain;
    ReRevvedUnitCombatProperty property;
    int32_t                    reserved[4];
} ReRevvedUnitCombatQuery;

typedef struct ReRevvedUnitCombatEvaluation
{
    // Current producer size. Callers may pass any buffer at least 20 bytes.
    uint32_t struct_size;
    int32_t  native_percent;
    int32_t  final_percent;
    uint32_t status_flags;
    uint32_t additive_count;
    int32_t  reserved[5];
} ReRevvedUnitCombatEvaluation;

typedef uint32_t (*ReRevvedUnitCombatRulesAbiVersionFn)(void);
typedef int32_t (*ReRevvedRegisterUnitCombatRuleFn)(
    const ReRevvedUnitCombatRule* rule);
typedef int32_t (*ReRevvedGetUnitCombatRuleCountFn)(uint32_t* out_count);
typedef int32_t (*ReRevvedGetUnitCombatRuleFn)(
    uint32_t                    index,
    ReRevvedUnitCombatRuleInfo* out,
    uint32_t                    out_size);
typedef int32_t (*ReRevvedEvaluateUnitCombatFn)(
    const ReRevvedUnitCombatQuery* query,
    ReRevvedUnitCombatEvaluation*  out,
    uint32_t                       out_size);

#ifdef __cplusplus
extern "C"
{
#endif

    REREVVED_UNIT_COMBAT_RULES_API uint32_t
                                           ReRevvedUnitCombatRulesAbiVersion(void);
    REREVVED_UNIT_COMBAT_RULES_API int32_t ReRevvedRegisterUnitCombatRule(
        const ReRevvedUnitCombatRule* rule);
    REREVVED_UNIT_COMBAT_RULES_API int32_t ReRevvedGetUnitCombatRuleCount(
        uint32_t* out_count);
    REREVVED_UNIT_COMBAT_RULES_API int32_t ReRevvedGetUnitCombatRule(
        uint32_t                    index,
        ReRevvedUnitCombatRuleInfo* out,
        uint32_t                    out_size);
    REREVVED_UNIT_COMBAT_RULES_API int32_t ReRevvedEvaluateUnitCombat(
        const ReRevvedUnitCombatQuery* query,
        ReRevvedUnitCombatEvaluation*  out,
        uint32_t                       out_size);

#ifdef __cplusplus
} // extern "C"
#endif

#undef REREVVED_UNIT_COMBAT_RULES_API
