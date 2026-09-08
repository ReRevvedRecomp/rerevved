// Public C ABI for ReRevved Unique Unit base attack and defense rules.
//
// Mods resolve these entry points from the host process and check
// ReRevvedUniqueUnitRulesAbiVersion before calling them. Registrations are
// copied by the host and target identities exposed by Unit Catalog ABI 1.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(REREVVED_UNIQUE_UNIT_RULES_API_EXPORTS)
#if defined(_WIN32)
#define REREVVED_UNIQUE_UNIT_RULES_API __declspec(dllexport)
#else
#define REREVVED_UNIQUE_UNIT_RULES_API __attribute__((visibility("default")))
#endif
#else
#define REREVVED_UNIQUE_UNIT_RULES_API
#endif

#define REREVVED_UNIQUE_UNIT_RULES_ABI_VERSION 1u
#define REREVVED_UNIQUE_UNIT_RULE_ID_CAPACITY  64u

enum
{
    REREVVED_UNIQUE_UNIT_RULES_OK                    = 0,
    REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT  = -10,
    REREVVED_UNIQUE_UNIT_RULES_ERR_BUFFER_TOO_SMALL  = -11,
    REREVVED_UNIQUE_UNIT_RULES_ERR_DUPLICATE_RULE_ID = -12,
    REREVVED_UNIQUE_UNIT_RULES_ERR_INTERNAL          = -13,
};

typedef int32_t ReRevvedUniqueUnitScalarProperty;

// These values compose the signed base stat before the title applies its
// civilization, era, unit, army, and earned combat modifiers.
enum
{
    REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK  = 0,
    REREVVED_UNIQUE_UNIT_SCALAR_BASE_DEFENSE = 1,
};

typedef int32_t ReRevvedUniqueUnitScalarOperation;

enum
{
    REREVVED_UNIQUE_UNIT_SCALAR_REPLACE = 0,
    REREVVED_UNIQUE_UNIT_SCALAR_ADD     = 1,
};

enum
{
    REREVVED_UNIQUE_UNIT_RULE_REPLACEMENT_CONFLICT = 1u << 0,
};

enum
{
    REREVVED_UNIQUE_UNIT_EVALUATION_REPLACEMENT_CONFLICT = 1u << 0,
    REREVVED_UNIQUE_UNIT_EVALUATION_OVERFLOW             = 1u << 1,
};

typedef struct ReRevvedUniqueUnitScalarRule
{
    uint32_t                          structSize;
    char                              providerId[REREVVED_UNIQUE_UNIT_RULE_ID_CAPACITY];
    char                              ruleId[REREVVED_UNIQUE_UNIT_RULE_ID_CAPACITY];
    ReRevvedCivilizationId            civilization;
    ReRevvedUnitTypeId                baseUnitType;
    ReRevvedUnitIdentityId            identity;
    ReRevvedUniqueUnitScalarProperty  property;
    ReRevvedUniqueUnitScalarOperation operation;
    int32_t                           value;
    int32_t                           reserved[5];
} ReRevvedUniqueUnitScalarRule;

typedef struct ReRevvedUniqueUnitScalarRuleInfo
{
    uint32_t                          structSize; // Current producer size. Callers may pass any buffer at least 160 bytes.
    char                              providerId[REREVVED_UNIQUE_UNIT_RULE_ID_CAPACITY];
    char                              ruleId[REREVVED_UNIQUE_UNIT_RULE_ID_CAPACITY];
    ReRevvedCivilizationId            civilization;
    ReRevvedUnitTypeId                baseUnitType;
    ReRevvedUnitIdentityId            identity;
    ReRevvedUniqueUnitScalarProperty  property;
    ReRevvedUniqueUnitScalarOperation operation;
    int32_t                           value;
    uint32_t                          statusFlags;
    int32_t                           reserved[8];
} ReRevvedUniqueUnitScalarRuleInfo;

typedef struct ReRevvedUniqueUnitScalarQuery
{
    uint32_t                         structSize;
    ReRevvedCivilizationId           civilization;
    ReRevvedUnitTypeId               baseUnitType;
    ReRevvedUnitIdentityId           identity;
    ReRevvedUniqueUnitScalarProperty property;
    int32_t                          nativeValue;
    int32_t                          reserved[4];
} ReRevvedUniqueUnitScalarQuery;

typedef struct ReRevvedUniqueUnitScalarEvaluation
{
    uint32_t structSize; // Current producer size. Callers may pass any buffer at least 24 bytes.
    int32_t  nativeValue;
    int32_t  finalValue;
    uint32_t statusFlags;
    uint32_t replacementCount;
    uint32_t additiveCount;
    int32_t  reserved[4];
} ReRevvedUniqueUnitScalarEvaluation;

typedef uint32_t (*ReRevvedUniqueUnitRulesAbiVersionFn)(void);
typedef int32_t (*ReRevvedRegisterUniqueUnitScalarRuleFn)(
    const ReRevvedUniqueUnitScalarRule* rule);
typedef int32_t (*ReRevvedGetUniqueUnitScalarRuleCountFn)(uint32_t* outCount);
typedef int32_t (*ReRevvedGetUniqueUnitScalarRuleFn)(
    uint32_t                          index,
    ReRevvedUniqueUnitScalarRuleInfo* out,
    uint32_t                          outSize);
typedef int32_t (*ReRevvedEvaluateUniqueUnitScalarFn)(
    const ReRevvedUniqueUnitScalarQuery* query,
    ReRevvedUniqueUnitScalarEvaluation*  out,
    uint32_t                             outSize);

#ifdef __cplusplus
extern "C"
{
#endif

    REREVVED_UNIQUE_UNIT_RULES_API uint32_t ReRevvedUniqueUnitRulesAbiVersion(void);
    REREVVED_UNIQUE_UNIT_RULES_API int32_t  ReRevvedRegisterUniqueUnitScalarRule(
        const ReRevvedUniqueUnitScalarRule* rule);
    REREVVED_UNIQUE_UNIT_RULES_API int32_t ReRevvedGetUniqueUnitScalarRuleCount(uint32_t* outCount);
    REREVVED_UNIQUE_UNIT_RULES_API int32_t ReRevvedGetUniqueUnitScalarRule(
        uint32_t                          index,
        ReRevvedUniqueUnitScalarRuleInfo* out,
        uint32_t                          outSize);
    REREVVED_UNIQUE_UNIT_RULES_API int32_t ReRevvedEvaluateUniqueUnitScalar(
        const ReRevvedUniqueUnitScalarQuery* query,
        ReRevvedUniqueUnitScalarEvaluation*  out,
        uint32_t                             outSize);

#ifdef __cplusplus
} // extern "C"
#endif

#undef REREVVED_UNIQUE_UNIT_RULES_API
