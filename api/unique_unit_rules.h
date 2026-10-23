// Public C ABI for ReRevved Unique Unit base attack and defense rules.
//
// Mods resolve these entry points from the host process and check
// UniqueUnitRulesAbiVersion before calling them. Registrations are
// copied by the host and target identities exposed by Unit Catalog ABI 2.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(UNIQUE_UNIT_RULES_API_EXPORTS)
#if defined(_WIN32)
#define UNIQUE_UNIT_RULES_API __declspec(dllexport)
#else
#define UNIQUE_UNIT_RULES_API __attribute__((visibility("default")))
#endif
#else
#define UNIQUE_UNIT_RULES_API
#endif

#define UNIQUE_UNIT_RULES_ABI_VERSION 2u
#define UNIQUE_UNIT_RULE_ID_CAPACITY  64u

enum
{
    UNIQUE_UNIT_RULES_OK                    = 0,
    UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT  = -10,
    UNIQUE_UNIT_RULES_ERR_BUFFER_TOO_SMALL  = -11,
    UNIQUE_UNIT_RULES_ERR_DUPLICATE_RULE_ID = -12,
    UNIQUE_UNIT_RULES_ERR_INTERNAL          = -13,
};

typedef int32_t UniqueUnitScalarProperty;

// These values compose the signed base stat before the title applies its
// civilization, era, unit, army, and earned combat modifiers.
enum
{
    UNIQUE_UNIT_SCALAR_BASE_ATTACK  = 0,
    UNIQUE_UNIT_SCALAR_BASE_DEFENSE = 1,
};

typedef int32_t UniqueUnitScalarOperation;

enum
{
    UNIQUE_UNIT_SCALAR_REPLACE = 0,
    UNIQUE_UNIT_SCALAR_ADD     = 1,
};

enum
{
    UNIQUE_UNIT_RULE_REPLACEMENT_CONFLICT = 1u << 0,
};

enum
{
    UNIQUE_UNIT_EVALUATION_REPLACEMENT_CONFLICT = 1u << 0,
    UNIQUE_UNIT_EVALUATION_OVERFLOW             = 1u << 1,
};

typedef struct UniqueUnitScalarRule
{
    uint32_t                  structSize;
    char                      providerId[UNIQUE_UNIT_RULE_ID_CAPACITY];
    char                      ruleId[UNIQUE_UNIT_RULE_ID_CAPACITY];
    CivilizationId            civilization;
    UnitTypeId                baseUnitType;
    UnitIdentityId            identity;
    UniqueUnitScalarProperty  property;
    UniqueUnitScalarOperation operation;
    int32_t                   value;
    int32_t                   reserved[5];
} UniqueUnitScalarRule;

typedef struct UniqueUnitScalarRuleInfo
{
    uint32_t                  structSize; // Current producer size. Callers may pass any buffer at least 160 bytes.
    char                      providerId[UNIQUE_UNIT_RULE_ID_CAPACITY];
    char                      ruleId[UNIQUE_UNIT_RULE_ID_CAPACITY];
    CivilizationId            civilization;
    UnitTypeId                baseUnitType;
    UnitIdentityId            identity;
    UniqueUnitScalarProperty  property;
    UniqueUnitScalarOperation operation;
    int32_t                   value;
    uint32_t                  statusFlags;
    int32_t                   reserved[8];
} UniqueUnitScalarRuleInfo;

typedef struct UniqueUnitScalarQuery
{
    uint32_t                 structSize;
    CivilizationId           civilization;
    UnitTypeId               baseUnitType;
    UnitIdentityId           identity;
    UniqueUnitScalarProperty property;
    int32_t                  nativeValue;
    int32_t                  reserved[4];
} UniqueUnitScalarQuery;

typedef struct UniqueUnitScalarEvaluation
{
    uint32_t structSize; // Current producer size. Callers may pass any buffer at least 24 bytes.
    int32_t  nativeValue;
    int32_t  finalValue;
    uint32_t statusFlags;
    uint32_t replacementCount;
    uint32_t additiveCount;
    int32_t  reserved[4];
} UniqueUnitScalarEvaluation;

typedef uint32_t (*UniqueUnitRulesAbiVersionFn)(void);
typedef int32_t (*RegisterUniqueUnitScalarRuleFn)(
    const UniqueUnitScalarRule* rule);
typedef int32_t (*GetUniqueUnitScalarRuleCountFn)(uint32_t* outCount);
typedef int32_t (*GetUniqueUnitScalarRuleFn)(
    uint32_t                  index,
    UniqueUnitScalarRuleInfo* out,
    uint32_t                  outSize);
typedef int32_t (*EvaluateUniqueUnitScalarFn)(
    const UniqueUnitScalarQuery* query,
    UniqueUnitScalarEvaluation*  out,
    uint32_t                     outSize);

#ifdef __cplusplus
extern "C"
{
#endif

    UNIQUE_UNIT_RULES_API uint32_t UniqueUnitRulesAbiVersion(void);
    UNIQUE_UNIT_RULES_API int32_t  RegisterUniqueUnitScalarRule(
        const UniqueUnitScalarRule* rule);
    UNIQUE_UNIT_RULES_API int32_t GetUniqueUnitScalarRuleCount(uint32_t* outCount);
    UNIQUE_UNIT_RULES_API int32_t GetUniqueUnitScalarRule(
        uint32_t                  index,
        UniqueUnitScalarRuleInfo* out,
        uint32_t                  outSize);
    UNIQUE_UNIT_RULES_API int32_t EvaluateUniqueUnitScalar(
        const UniqueUnitScalarQuery* query,
        UniqueUnitScalarEvaluation*  out,
        uint32_t                     outSize);

#ifdef __cplusplus
} // extern "C"
#endif

#undef UNIQUE_UNIT_RULES_API
