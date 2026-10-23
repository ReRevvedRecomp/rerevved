// Public C ABI for ReRevved terrain yield composition rules.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(TERRAIN_YIELD_RULES_API_EXPORTS)
#if defined(_WIN32)
#define TERRAIN_YIELD_RULES_API __declspec(dllexport)
#else
#define TERRAIN_YIELD_RULES_API __attribute__((visibility("default")))
#endif
#else
#define TERRAIN_YIELD_RULES_API
#endif

#define TERRAIN_YIELD_RULES_ABI_VERSION 2u
#define TERRAIN_YIELD_RULE_ID_CAPACITY  64u

enum
{
    TERRAIN_YIELD_RULES_OK                    = 0,
    TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT  = -10,
    TERRAIN_YIELD_RULES_ERR_BUFFER_TOO_SMALL  = -11,
    TERRAIN_YIELD_RULES_ERR_DUPLICATE_RULE_ID = -12,
    TERRAIN_YIELD_RULES_ERR_INTERNAL          = -13,
};

typedef int32_t TerrainYieldComponent;

enum
{
    TERRAIN_YIELD_FOOD       = 0,
    TERRAIN_YIELD_PRODUCTION = 1,
    TERRAIN_YIELD_TRADE      = 2,
};

typedef int32_t TerrainYieldOperation;

enum
{
    TERRAIN_YIELD_REPLACE = 0,
    TERRAIN_YIELD_ADD     = 1,
};

enum
{
    TERRAIN_YIELD_RULE_REPLACEMENT_CONFLICT = 1u << 0,
};

enum
{
    TERRAIN_YIELD_EVALUATION_REPLACEMENT_CONFLICT = 1u << 0,
    TERRAIN_YIELD_EVALUATION_OVERFLOW             = 1u << 1,
};

typedef struct TerrainYieldRule
{
    uint32_t              structSize;
    char                  providerId[TERRAIN_YIELD_RULE_ID_CAPACITY];
    char                  ruleId[TERRAIN_YIELD_RULE_ID_CAPACITY];
    TerrainId             terrain;
    TerrainYieldComponent component;
    TerrainYieldOperation operation;
    int32_t               value;
    int32_t               reserved[5];
} TerrainYieldRule;

typedef struct TerrainYieldRuleInfo
{
    uint32_t              structSize; // Current producer size. Callers may pass any buffer at least 152 bytes.
    char                  providerId[TERRAIN_YIELD_RULE_ID_CAPACITY];
    char                  ruleId[TERRAIN_YIELD_RULE_ID_CAPACITY];
    TerrainId             terrain;
    TerrainYieldComponent component;
    TerrainYieldOperation operation;
    int32_t               value;
    uint32_t              statusFlags;
    int32_t               reserved[10];
} TerrainYieldRuleInfo;

typedef struct TerrainYieldQuery
{
    uint32_t              structSize;
    TerrainId             terrain;
    TerrainYieldComponent component;
    int32_t               nativeValue;
    int32_t               reserved[6];
} TerrainYieldQuery;

typedef struct TerrainYieldEvaluation
{
    uint32_t structSize; // Current producer size. Callers may pass any buffer at least 24 bytes.
    int32_t  nativeValue;
    int32_t  finalValue;
    uint32_t statusFlags;
    uint32_t replacementCount;
    uint32_t additiveCount;
    int32_t  reserved[4];
} TerrainYieldEvaluation;

typedef uint32_t (*TerrainYieldRulesAbiVersionFn)(void);
typedef int32_t (*RegisterTerrainYieldRuleFn)(
    const TerrainYieldRule* rule);
typedef int32_t (*GetTerrainYieldRuleCountFn)(uint32_t* outCount);
typedef int32_t (*GetTerrainYieldRuleFn)(
    uint32_t              index,
    TerrainYieldRuleInfo* out,
    uint32_t              outSize);
typedef int32_t (*EvaluateTerrainYieldFn)(
    const TerrainYieldQuery* query,
    TerrainYieldEvaluation*  out,
    uint32_t                 outSize);

#ifdef __cplusplus
extern "C"
{
#endif

    TERRAIN_YIELD_RULES_API uint32_t TerrainYieldRulesAbiVersion(void);
    TERRAIN_YIELD_RULES_API int32_t  RegisterTerrainYieldRule(
        const TerrainYieldRule* rule);
    TERRAIN_YIELD_RULES_API int32_t GetTerrainYieldRuleCount(
        uint32_t* outCount);
    TERRAIN_YIELD_RULES_API int32_t GetTerrainYieldRule(
        uint32_t              index,
        TerrainYieldRuleInfo* out,
        uint32_t              outSize);
    TERRAIN_YIELD_RULES_API int32_t EvaluateTerrainYield(
        const TerrainYieldQuery* query,
        TerrainYieldEvaluation*  out,
        uint32_t                 outSize);

#ifdef __cplusplus
} // extern "C"
#endif

#undef TERRAIN_YIELD_RULES_API
