// Public C ABI for identity-targeted unit production cost percentages.
//
// Mods resolve these entry points from the host process and check
// ReRevvedUnitProductionCostRulesAbiVersion before calling them. Registrations are
// copied by the host and target identities exposed by Unit Catalog ABI 1.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(REREVVED_UNIT_PRODUCTION_COST_RULES_API_EXPORTS)
#if defined(_WIN32)
#define REREVVED_UNIT_PRODUCTION_COST_RULES_API __declspec(dllexport)
#else
#define REREVVED_UNIT_PRODUCTION_COST_RULES_API __attribute__((visibility("default")))
#endif
#else
#define REREVVED_UNIT_PRODUCTION_COST_RULES_API
#endif

#define REREVVED_UNIT_PRODUCTION_COST_RULES_ABI_VERSION 1u
#define REREVVED_UNIT_PRODUCTION_COST_RULE_ID_CAPACITY  64u

enum
{
    REREVVED_UNIT_PRODUCTION_COST_RULES_OK                    = 0,
    REREVVED_UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT  = -10,
    REREVVED_UNIT_PRODUCTION_COST_RULES_ERR_BUFFER_TOO_SMALL  = -11,
    REREVVED_UNIT_PRODUCTION_COST_RULES_ERR_DUPLICATE_RULE_ID = -12,
    REREVVED_UNIT_PRODUCTION_COST_RULES_ERR_INTERNAL          = -13,
};

enum
{
    REREVVED_UNIT_PRODUCTION_COST_EVALUATION_OUT_OF_RANGE = 1u << 0,
};

typedef struct ReRevvedUnitProductionCostRule
{
    uint32_t               struct_size;
    char                   provider_id[REREVVED_UNIT_PRODUCTION_COST_RULE_ID_CAPACITY];
    char                   rule_id[REREVVED_UNIT_PRODUCTION_COST_RULE_ID_CAPACITY];
    ReRevvedCivilizationId civilization;
    ReRevvedUnitTypeId     base_unit_type;
    ReRevvedUnitIdentityId identity;
    int32_t percentage_delta; // Additive percentage points relative to the native cost (100 percent).
    int32_t reserved[5];
} ReRevvedUnitProductionCostRule;

typedef struct ReRevvedUnitProductionCostRuleInfo
{
    uint32_t               struct_size; // Current producer size. Callers may pass any buffer at least 152 bytes.
    char                   provider_id[REREVVED_UNIT_PRODUCTION_COST_RULE_ID_CAPACITY];
    char                   rule_id[REREVVED_UNIT_PRODUCTION_COST_RULE_ID_CAPACITY];
    ReRevvedCivilizationId civilization;
    ReRevvedUnitTypeId     base_unit_type;
    ReRevvedUnitIdentityId identity;
    int32_t                percentage_delta;
    uint32_t               status_flags;
    int32_t                reserved[10];
} ReRevvedUnitProductionCostRuleInfo;

typedef struct ReRevvedUnitProductionCostQuery
{
    uint32_t               struct_size;
    ReRevvedCivilizationId civilization;
    ReRevvedUnitTypeId     base_unit_type;
    ReRevvedUnitIdentityId identity;
    int32_t                reserved[6];
} ReRevvedUnitProductionCostQuery;

typedef struct ReRevvedUnitProductionCostEvaluation
{
    uint32_t struct_size; // Current producer size. Callers may pass any buffer at least 20 bytes.
    int32_t  native_percent;
    int32_t  final_percent;
    uint32_t status_flags;
    uint32_t additive_count;
    int32_t  reserved[5];
} ReRevvedUnitProductionCostEvaluation;

typedef uint32_t (*ReRevvedUnitProductionCostRulesAbiVersionFn)(void);
typedef int32_t (*ReRevvedRegisterUnitProductionCostRuleFn)(
    const ReRevvedUnitProductionCostRule* rule);
typedef int32_t (*ReRevvedGetUnitProductionCostRuleCountFn)(uint32_t* out_count);
typedef int32_t (*ReRevvedGetUnitProductionCostRuleFn)(
    uint32_t                            index,
    ReRevvedUnitProductionCostRuleInfo* out,
    uint32_t                            out_size);
typedef int32_t (*ReRevvedEvaluateUnitProductionCostFn)(
    const ReRevvedUnitProductionCostQuery* query,
    ReRevvedUnitProductionCostEvaluation*  out,
    uint32_t                               out_size);

#ifdef __cplusplus
extern "C"
{
#endif

    REREVVED_UNIT_PRODUCTION_COST_RULES_API uint32_t
                                                    ReRevvedUnitProductionCostRulesAbiVersion(void);
    REREVVED_UNIT_PRODUCTION_COST_RULES_API int32_t ReRevvedRegisterUnitProductionCostRule(
        const ReRevvedUnitProductionCostRule* rule);
    REREVVED_UNIT_PRODUCTION_COST_RULES_API int32_t
                                                    ReRevvedGetUnitProductionCostRuleCount(uint32_t* out_count);
    REREVVED_UNIT_PRODUCTION_COST_RULES_API int32_t ReRevvedGetUnitProductionCostRule(
        uint32_t                            index,
        ReRevvedUnitProductionCostRuleInfo* out,
        uint32_t                            out_size);
    REREVVED_UNIT_PRODUCTION_COST_RULES_API int32_t ReRevvedEvaluateUnitProductionCost(
        const ReRevvedUnitProductionCostQuery* query,
        ReRevvedUnitProductionCostEvaluation*  out,
        uint32_t                               out_size);

#ifdef __cplusplus
} // extern "C"
#endif

#undef REREVVED_UNIT_PRODUCTION_COST_RULES_API
