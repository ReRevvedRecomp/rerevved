// Public C ABI for identity-targeted additive unit movement rules.
//
// Mods resolve these entry points from the host process and check
// UnitMovementRulesAbiVersion before calling them. Registrations are
// copied by the host and target identities exposed by Unit Catalog ABI 2.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(UNIT_MOVEMENT_RULES_API_EXPORTS)
#if defined(_WIN32)
#define UNIT_MOVEMENT_RULES_API __declspec(dllexport)
#else
#define UNIT_MOVEMENT_RULES_API __attribute__((visibility("default")))
#endif
#else
#define UNIT_MOVEMENT_RULES_API
#endif

#define UNIT_MOVEMENT_RULES_ABI_VERSION 2u
#define UNIT_MOVEMENT_RULE_ID_CAPACITY  64u

enum
{
    UNIT_MOVEMENT_RULES_OK                    = 0,
    UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT  = -10,
    UNIT_MOVEMENT_RULES_ERR_BUFFER_TOO_SMALL  = -11,
    UNIT_MOVEMENT_RULES_ERR_DUPLICATE_RULE_ID = -12,
    UNIT_MOVEMENT_RULES_ERR_INTERNAL          = -13,
};

enum
{
    UNIT_MOVEMENT_RULE_EVALUATION_OVERFLOW = 1u << 0,
};

typedef struct UnitMovementRule
{
    uint32_t       structSize;
    char           providerId[UNIT_MOVEMENT_RULE_ID_CAPACITY];
    char           ruleId[UNIT_MOVEMENT_RULE_ID_CAPACITY];
    CivilizationId civilization;
    UnitTypeId     baseUnitType;
    UnitIdentityId identity;
    int32_t        value;
    int32_t        reserved[5];
} UnitMovementRule;

typedef struct UnitMovementRuleInfo
{
    uint32_t       structSize; // Current producer size. Callers may pass any buffer at least 152 bytes.
    char           providerId[UNIT_MOVEMENT_RULE_ID_CAPACITY];
    char           ruleId[UNIT_MOVEMENT_RULE_ID_CAPACITY];
    CivilizationId civilization;
    UnitTypeId     baseUnitType;
    UnitIdentityId identity;
    int32_t        value;
    uint32_t       statusFlags;
    int32_t        reserved[10];
} UnitMovementRuleInfo;

typedef struct UnitMovementQuery
{
    uint32_t       structSize;
    CivilizationId civilization;
    UnitTypeId     baseUnitType;
    UnitIdentityId identity;
    int32_t        nativeValue;
    int32_t        reserved[5];
} UnitMovementQuery;

typedef struct UnitMovementEvaluation
{
    uint32_t structSize; // Current producer size. Callers may pass any buffer at least 20 bytes.
    int32_t  nativeValue;
    int32_t  finalValue;
    uint32_t statusFlags;
    uint32_t additiveCount;
    int32_t  reserved[5];
} UnitMovementEvaluation;

typedef uint32_t (*UnitMovementRulesAbiVersionFn)(void);
typedef int32_t (*RegisterUnitMovementRuleFn)(
    const UnitMovementRule* rule);
typedef int32_t (*GetUnitMovementRuleCountFn)(uint32_t* outCount);
typedef int32_t (*GetUnitMovementRuleFn)(
    uint32_t              index,
    UnitMovementRuleInfo* out,
    uint32_t              outSize);
typedef int32_t (*EvaluateUnitMovementFn)(
    const UnitMovementQuery* query,
    UnitMovementEvaluation*  out,
    uint32_t                 outSize);

#ifdef __cplusplus
extern "C"
{
#endif

    UNIT_MOVEMENT_RULES_API uint32_t UnitMovementRulesAbiVersion(void);
    UNIT_MOVEMENT_RULES_API int32_t  RegisterUnitMovementRule(
        const UnitMovementRule* rule);
    UNIT_MOVEMENT_RULES_API int32_t GetUnitMovementRuleCount(uint32_t* outCount);
    UNIT_MOVEMENT_RULES_API int32_t GetUnitMovementRule(
        uint32_t              index,
        UnitMovementRuleInfo* out,
        uint32_t              outSize);
    UNIT_MOVEMENT_RULES_API int32_t EvaluateUnitMovement(
        const UnitMovementQuery* query,
        UnitMovementEvaluation*  out,
        uint32_t                 outSize);

#ifdef __cplusplus
} // extern "C"
#endif

#undef UNIT_MOVEMENT_RULES_API
