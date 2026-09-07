// Public C ABI for identity-targeted additive unit movement rules.
//
// Mods resolve these entry points from the host process and check
// ReRevvedUnitMovementRulesAbiVersion before calling them. Registrations are
// copied by the host and target identities exposed by Unit Catalog ABI 1.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(REREVVED_UNIT_MOVEMENT_RULES_API_EXPORTS)
#if defined(_WIN32)
#define REREVVED_UNIT_MOVEMENT_RULES_API __declspec(dllexport)
#else
#define REREVVED_UNIT_MOVEMENT_RULES_API __attribute__((visibility("default")))
#endif
#else
#define REREVVED_UNIT_MOVEMENT_RULES_API
#endif

#define REREVVED_UNIT_MOVEMENT_RULES_ABI_VERSION 1u
#define REREVVED_UNIT_MOVEMENT_RULE_ID_CAPACITY  64u

enum
{
    REREVVED_UNIT_MOVEMENT_RULES_OK                    = 0,
    REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT  = -10,
    REREVVED_UNIT_MOVEMENT_RULES_ERR_BUFFER_TOO_SMALL  = -11,
    REREVVED_UNIT_MOVEMENT_RULES_ERR_DUPLICATE_RULE_ID = -12,
    REREVVED_UNIT_MOVEMENT_RULES_ERR_INTERNAL          = -13,
};

enum
{
    REREVVED_UNIT_MOVEMENT_RULE_EVALUATION_OVERFLOW = 1u << 0,
};

typedef struct ReRevvedUnitMovementRule
{
    uint32_t struct_size;
    char provider_id[REREVVED_UNIT_MOVEMENT_RULE_ID_CAPACITY];
    char rule_id[REREVVED_UNIT_MOVEMENT_RULE_ID_CAPACITY];
    ReRevvedCivilizationId civilization;
    ReRevvedUnitTypeId base_unit_type;
    ReRevvedUnitIdentityId identity;
    int32_t value;
    int32_t reserved[5];
} ReRevvedUnitMovementRule;

typedef struct ReRevvedUnitMovementRuleInfo
{
    uint32_t struct_size; // Current producer size. Callers may pass any buffer at least 152 bytes.
    char provider_id[REREVVED_UNIT_MOVEMENT_RULE_ID_CAPACITY];
    char rule_id[REREVVED_UNIT_MOVEMENT_RULE_ID_CAPACITY];
    ReRevvedCivilizationId civilization;
    ReRevvedUnitTypeId base_unit_type;
    ReRevvedUnitIdentityId identity;
    int32_t value;
    uint32_t status_flags;
    int32_t reserved[10];
} ReRevvedUnitMovementRuleInfo;

typedef struct ReRevvedUnitMovementQuery
{
    uint32_t struct_size;
    ReRevvedCivilizationId civilization;
    ReRevvedUnitTypeId base_unit_type;
    ReRevvedUnitIdentityId identity;
    int32_t native_value;
    int32_t reserved[5];
} ReRevvedUnitMovementQuery;

typedef struct ReRevvedUnitMovementEvaluation
{
    uint32_t struct_size; // Current producer size. Callers may pass any buffer at least 20 bytes.
    int32_t native_value;
    int32_t final_value;
    uint32_t status_flags;
    uint32_t additive_count;
    int32_t reserved[5];
} ReRevvedUnitMovementEvaluation;

typedef uint32_t (*ReRevvedUnitMovementRulesAbiVersionFn)(void);
typedef int32_t (*ReRevvedRegisterUnitMovementRuleFn)(
    const ReRevvedUnitMovementRule* rule);
typedef int32_t (*ReRevvedGetUnitMovementRuleCountFn)(uint32_t* out_count);
typedef int32_t (*ReRevvedGetUnitMovementRuleFn)(
    uint32_t index,
    ReRevvedUnitMovementRuleInfo* out,
    uint32_t out_size);
typedef int32_t (*ReRevvedEvaluateUnitMovementFn)(
    const ReRevvedUnitMovementQuery* query,
    ReRevvedUnitMovementEvaluation* out,
    uint32_t out_size);

#ifdef __cplusplus
extern "C"
{
#endif

    REREVVED_UNIT_MOVEMENT_RULES_API uint32_t
    ReRevvedUnitMovementRulesAbiVersion(void);
    REREVVED_UNIT_MOVEMENT_RULES_API int32_t ReRevvedRegisterUnitMovementRule(
        const ReRevvedUnitMovementRule* rule);
    REREVVED_UNIT_MOVEMENT_RULES_API int32_t
    ReRevvedGetUnitMovementRuleCount(uint32_t* out_count);
    REREVVED_UNIT_MOVEMENT_RULES_API int32_t ReRevvedGetUnitMovementRule(
        uint32_t index,
        ReRevvedUnitMovementRuleInfo* out,
        uint32_t out_size);
    REREVVED_UNIT_MOVEMENT_RULES_API int32_t ReRevvedEvaluateUnitMovement(
        const ReRevvedUnitMovementQuery* query,
        ReRevvedUnitMovementEvaluation* out,
        uint32_t out_size);

#ifdef __cplusplus
} // extern "C"
#endif

#undef REREVVED_UNIT_MOVEMENT_RULES_API
