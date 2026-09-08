// Public C ABI for creation-time grant-only unit effects.
//
// ABI 2 exposes the Veteran creation grant and the nine named native special
// upgrade effects. Registrations are copied by the host and match a
// civilization, base unit type, and accepted Unit Catalog identity.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(REREVVED_UNIT_EFFECT_RULES_API_EXPORTS)
#if defined(_WIN32)
#define REREVVED_UNIT_EFFECT_RULES_API __declspec(dllexport)
#else
#define REREVVED_UNIT_EFFECT_RULES_API __attribute__((visibility("default")))
#endif
#else
#define REREVVED_UNIT_EFFECT_RULES_API
#endif

#define REREVVED_UNIT_EFFECT_RULES_ABI_VERSION 2u
#define REREVVED_UNIT_EFFECT_RULE_ID_CAPACITY  64u

enum
{
    REREVVED_UNIT_EFFECT_RULES_OK                    = 0,
    REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT  = -10,
    REREVVED_UNIT_EFFECT_RULES_ERR_BUFFER_TOO_SMALL  = -11,
    REREVVED_UNIT_EFFECT_RULES_ERR_DUPLICATE_RULE_ID = -12,
    REREVVED_UNIT_EFFECT_RULES_ERR_INTERNAL          = -13,
};

typedef int32_t ReRevvedUnitEffectId;

enum
{
    REREVVED_UNIT_EFFECT_CREATION_VETERAN      = 1,
    REREVVED_UNIT_EFFECT_CREATION_GUERILLA     = 2,
    REREVVED_UNIT_EFFECT_CREATION_BLITZ        = 3,
    REREVVED_UNIT_EFFECT_CREATION_INFILTRATION = 4,
    REREVVED_UNIT_EFFECT_CREATION_LOYALTY      = 5,
    REREVVED_UNIT_EFFECT_CREATION_ENGINEER     = 6,
    REREVVED_UNIT_EFFECT_CREATION_LEADERSHIP   = 7,
    REREVVED_UNIT_EFFECT_CREATION_MARCH        = 8,
    REREVVED_UNIT_EFFECT_CREATION_MEDIC        = 9,
    REREVVED_UNIT_EFFECT_CREATION_SCOUT        = 10,
};

enum
{
    REREVVED_UNIT_EFFECT_EVALUATION_GRANTED = 1u << 0,
};

typedef struct ReRevvedUnitEffectRule
{
    uint32_t               structSize;
    char                   providerId[REREVVED_UNIT_EFFECT_RULE_ID_CAPACITY];
    char                   ruleId[REREVVED_UNIT_EFFECT_RULE_ID_CAPACITY];
    ReRevvedCivilizationId civilization;
    ReRevvedUnitTypeId     baseUnitType;
    ReRevvedUnitIdentityId identity;
    ReRevvedUnitEffectId   effect;
    int32_t                reserved[5];
} ReRevvedUnitEffectRule;

typedef struct ReRevvedUnitEffectRuleInfo
{
    uint32_t               structSize; // Current producer size. Callers may pass any buffer at least 152 bytes.
    char                   providerId[REREVVED_UNIT_EFFECT_RULE_ID_CAPACITY];
    char                   ruleId[REREVVED_UNIT_EFFECT_RULE_ID_CAPACITY];
    ReRevvedCivilizationId civilization;
    ReRevvedUnitTypeId     baseUnitType;
    ReRevvedUnitIdentityId identity;
    ReRevvedUnitEffectId   effect;
    uint32_t               statusFlags;
    int32_t                reserved[10];
} ReRevvedUnitEffectRuleInfo;

typedef struct ReRevvedUnitEffectQuery
{
    uint32_t               structSize;
    ReRevvedCivilizationId civilization;
    ReRevvedUnitTypeId     baseUnitType;
    ReRevvedUnitIdentityId identity;
    ReRevvedUnitEffectId   effect;
    int32_t                nativeLevel;
    int32_t                reserved[5];
} ReRevvedUnitEffectQuery;

typedef struct ReRevvedUnitEffectEvaluation
{
    uint32_t structSize; // Current producer size. Callers may pass any buffer at least 20 bytes.
    int32_t  nativeLevel;
    int32_t  finalLevel;
    uint32_t statusFlags;
    uint32_t grantCount;
    int32_t  reserved[5];
} ReRevvedUnitEffectEvaluation;

typedef uint32_t (*ReRevvedUnitEffectRulesAbiVersionFn)(void);
typedef int32_t (*ReRevvedRegisterUnitEffectRuleFn)(
    const ReRevvedUnitEffectRule* rule);
typedef int32_t (*ReRevvedGetUnitEffectRuleCountFn)(uint32_t* outCount);
typedef int32_t (*ReRevvedGetUnitEffectRuleFn)(
    uint32_t                    index,
    ReRevvedUnitEffectRuleInfo* out,
    uint32_t                    outSize);
typedef int32_t (*ReRevvedEvaluateUnitEffectFn)(
    const ReRevvedUnitEffectQuery* query,
    ReRevvedUnitEffectEvaluation*  out,
    uint32_t                       outSize);

#ifdef __cplusplus
extern "C"
{
#endif

    REREVVED_UNIT_EFFECT_RULES_API uint32_t ReRevvedUnitEffectRulesAbiVersion(void);
    REREVVED_UNIT_EFFECT_RULES_API int32_t  ReRevvedRegisterUnitEffectRule(
        const ReRevvedUnitEffectRule* rule);
    REREVVED_UNIT_EFFECT_RULES_API int32_t ReRevvedGetUnitEffectRuleCount(uint32_t* outCount);
    REREVVED_UNIT_EFFECT_RULES_API int32_t ReRevvedGetUnitEffectRule(
        uint32_t                    index,
        ReRevvedUnitEffectRuleInfo* out,
        uint32_t                    outSize);
    REREVVED_UNIT_EFFECT_RULES_API int32_t ReRevvedEvaluateUnitEffect(
        const ReRevvedUnitEffectQuery* query,
        ReRevvedUnitEffectEvaluation*  out,
        uint32_t                       outSize);

#ifdef __cplusplus
} // extern "C"
#endif

#undef REREVVED_UNIT_EFFECT_RULES_API
