// Public C ABI for creation-time grant-only unit effects.
//
// ABI 3 exposes the Veteran creation grant and the nine named native special
// upgrade effects. Registrations are copied by the host and match a
// civilization, base unit type, and accepted Unit Catalog identity.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(UNIT_EFFECT_RULES_API_EXPORTS)
#if defined(_WIN32)
#define UNIT_EFFECT_RULES_API __declspec(dllexport)
#else
#define UNIT_EFFECT_RULES_API __attribute__((visibility("default")))
#endif
#else
#define UNIT_EFFECT_RULES_API
#endif

#define UNIT_EFFECT_RULES_ABI_VERSION 3u
#define UNIT_EFFECT_RULE_ID_CAPACITY  64u

enum
{
    UNIT_EFFECT_RULES_OK                    = 0,
    UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT  = -10,
    UNIT_EFFECT_RULES_ERR_BUFFER_TOO_SMALL  = -11,
    UNIT_EFFECT_RULES_ERR_DUPLICATE_RULE_ID = -12,
    UNIT_EFFECT_RULES_ERR_INTERNAL          = -13,
};

typedef int32_t UnitEffectId;

enum
{
    UNIT_EFFECT_CREATION_VETERAN      = 1,
    UNIT_EFFECT_CREATION_GUERILLA     = 2,
    UNIT_EFFECT_CREATION_BLITZ        = 3,
    UNIT_EFFECT_CREATION_INFILTRATION = 4,
    UNIT_EFFECT_CREATION_LOYALTY      = 5,
    UNIT_EFFECT_CREATION_ENGINEER     = 6,
    UNIT_EFFECT_CREATION_LEADERSHIP   = 7,
    UNIT_EFFECT_CREATION_MARCH        = 8,
    UNIT_EFFECT_CREATION_MEDIC        = 9,
    UNIT_EFFECT_CREATION_SCOUT        = 10,
};

enum
{
    UNIT_EFFECT_EVALUATION_GRANTED = 1u << 0,
};

typedef struct UnitEffectRule
{
    uint32_t       structSize;
    char           providerId[UNIT_EFFECT_RULE_ID_CAPACITY];
    char           ruleId[UNIT_EFFECT_RULE_ID_CAPACITY];
    CivilizationId civilization;
    UnitTypeId     baseUnitType;
    UnitIdentityId identity;
    UnitEffectId   effect;
    int32_t        reserved[5];
} UnitEffectRule;

typedef struct UnitEffectRuleInfo
{
    uint32_t       structSize; // Current producer size. Callers may pass any buffer at least 152 bytes.
    char           providerId[UNIT_EFFECT_RULE_ID_CAPACITY];
    char           ruleId[UNIT_EFFECT_RULE_ID_CAPACITY];
    CivilizationId civilization;
    UnitTypeId     baseUnitType;
    UnitIdentityId identity;
    UnitEffectId   effect;
    uint32_t       statusFlags;
    int32_t        reserved[10];
} UnitEffectRuleInfo;

typedef struct UnitEffectQuery
{
    uint32_t       structSize;
    CivilizationId civilization;
    UnitTypeId     baseUnitType;
    UnitIdentityId identity;
    UnitEffectId   effect;
    int32_t        nativeLevel;
    int32_t        reserved[5];
} UnitEffectQuery;

typedef struct UnitEffectEvaluation
{
    uint32_t structSize; // Current producer size. Callers may pass any buffer at least 20 bytes.
    int32_t  nativeLevel;
    int32_t  finalLevel;
    uint32_t statusFlags;
    uint32_t grantCount;
    int32_t  reserved[5];
} UnitEffectEvaluation;

typedef uint32_t (*UnitEffectRulesAbiVersionFn)(void);
typedef int32_t (*RegisterUnitEffectRuleFn)(
    const UnitEffectRule* rule);
typedef int32_t (*GetUnitEffectRuleCountFn)(uint32_t* outCount);
typedef int32_t (*GetUnitEffectRuleFn)(
    uint32_t            index,
    UnitEffectRuleInfo* out,
    uint32_t            outSize);
typedef int32_t (*EvaluateUnitEffectFn)(
    const UnitEffectQuery* query,
    UnitEffectEvaluation*  out,
    uint32_t               outSize);

#ifdef __cplusplus
extern "C"
{
#endif

    UNIT_EFFECT_RULES_API uint32_t UnitEffectRulesAbiVersion(void);
    UNIT_EFFECT_RULES_API int32_t  RegisterUnitEffectRule(
        const UnitEffectRule* rule);
    UNIT_EFFECT_RULES_API int32_t GetUnitEffectRuleCount(uint32_t* outCount);
    UNIT_EFFECT_RULES_API int32_t GetUnitEffectRule(
        uint32_t            index,
        UnitEffectRuleInfo* out,
        uint32_t            outSize);
    UNIT_EFFECT_RULES_API int32_t EvaluateUnitEffect(
        const UnitEffectQuery* query,
        UnitEffectEvaluation*  out,
        uint32_t               outSize);

#ifdef __cplusplus
} // extern "C"
#endif

#undef UNIT_EFFECT_RULES_API
