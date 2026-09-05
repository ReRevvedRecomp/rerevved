// Public C ABI for ReRevved terrain yield composition rules.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(REREVVED_TERRAIN_YIELD_RULES_API_EXPORTS)
#if defined(_WIN32)
#define REREVVED_TERRAIN_YIELD_RULES_API __declspec(dllexport)
#else
#define REREVVED_TERRAIN_YIELD_RULES_API __attribute__((visibility("default")))
#endif
#else
#define REREVVED_TERRAIN_YIELD_RULES_API
#endif

#define REREVVED_TERRAIN_YIELD_RULES_ABI_VERSION 1u
#define REREVVED_TERRAIN_YIELD_RULE_ID_CAPACITY  64u

enum
{
    REREVVED_TERRAIN_YIELD_RULES_OK                    = 0,
    REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT  = -10,
    REREVVED_TERRAIN_YIELD_RULES_ERR_BUFFER_TOO_SMALL  = -11,
    REREVVED_TERRAIN_YIELD_RULES_ERR_DUPLICATE_RULE_ID = -12,
    REREVVED_TERRAIN_YIELD_RULES_ERR_INTERNAL          = -13,
};

typedef int32_t ReRevvedTerrainYieldComponent;

enum
{
    REREVVED_TERRAIN_YIELD_FOOD       = 0,
    REREVVED_TERRAIN_YIELD_PRODUCTION = 1,
    REREVVED_TERRAIN_YIELD_TRADE      = 2,
};

typedef int32_t ReRevvedTerrainYieldOperation;

enum
{
    REREVVED_TERRAIN_YIELD_REPLACE = 0,
    REREVVED_TERRAIN_YIELD_ADD     = 1,
};

enum
{
    REREVVED_TERRAIN_YIELD_RULE_REPLACEMENT_CONFLICT = 1u << 0,
};

enum
{
    REREVVED_TERRAIN_YIELD_EVALUATION_REPLACEMENT_CONFLICT = 1u << 0,
    REREVVED_TERRAIN_YIELD_EVALUATION_OVERFLOW             = 1u << 1,
};

typedef struct ReRevvedTerrainYieldRule
{
    uint32_t                      struct_size;
    char                          provider_id[REREVVED_TERRAIN_YIELD_RULE_ID_CAPACITY];
    char                          rule_id[REREVVED_TERRAIN_YIELD_RULE_ID_CAPACITY];
    ReRevvedTerrainId             terrain;
    ReRevvedTerrainYieldComponent component;
    ReRevvedTerrainYieldOperation operation;
    int32_t                       value;
    int32_t                       reserved[5];
} ReRevvedTerrainYieldRule;

typedef struct ReRevvedTerrainYieldRuleInfo
{
    // Current producer size. Callers may pass any buffer at least 152 bytes.
    uint32_t                      struct_size;
    char                          provider_id[REREVVED_TERRAIN_YIELD_RULE_ID_CAPACITY];
    char                          rule_id[REREVVED_TERRAIN_YIELD_RULE_ID_CAPACITY];
    ReRevvedTerrainId             terrain;
    ReRevvedTerrainYieldComponent component;
    ReRevvedTerrainYieldOperation operation;
    int32_t                       value;
    uint32_t                      status_flags;
    int32_t                       reserved[10];
} ReRevvedTerrainYieldRuleInfo;

typedef struct ReRevvedTerrainYieldQuery
{
    uint32_t                      struct_size;
    ReRevvedTerrainId             terrain;
    ReRevvedTerrainYieldComponent component;
    int32_t                       native_value;
    int32_t                       reserved[6];
} ReRevvedTerrainYieldQuery;

typedef struct ReRevvedTerrainYieldEvaluation
{
    // Current producer size. Callers may pass any buffer at least 24 bytes.
    uint32_t struct_size;
    int32_t  native_value;
    int32_t  final_value;
    uint32_t status_flags;
    uint32_t replacement_count;
    uint32_t additive_count;
    int32_t  reserved[4];
} ReRevvedTerrainYieldEvaluation;

typedef uint32_t (*ReRevvedTerrainYieldRulesAbiVersionFn)(void);
typedef int32_t (*ReRevvedRegisterTerrainYieldRuleFn)(
    const ReRevvedTerrainYieldRule* rule);
typedef int32_t (*ReRevvedGetTerrainYieldRuleCountFn)(uint32_t* out_count);
typedef int32_t (*ReRevvedGetTerrainYieldRuleFn)(
    uint32_t                      index,
    ReRevvedTerrainYieldRuleInfo* out,
    uint32_t                      out_size);
typedef int32_t (*ReRevvedEvaluateTerrainYieldFn)(
    const ReRevvedTerrainYieldQuery* query,
    ReRevvedTerrainYieldEvaluation*  out,
    uint32_t                         out_size);

#ifdef __cplusplus
extern "C"
{
#endif

    REREVVED_TERRAIN_YIELD_RULES_API uint32_t
                                             ReRevvedTerrainYieldRulesAbiVersion(void);
    REREVVED_TERRAIN_YIELD_RULES_API int32_t ReRevvedRegisterTerrainYieldRule(
        const ReRevvedTerrainYieldRule* rule);
    REREVVED_TERRAIN_YIELD_RULES_API int32_t ReRevvedGetTerrainYieldRuleCount(
        uint32_t* out_count);
    REREVVED_TERRAIN_YIELD_RULES_API int32_t ReRevvedGetTerrainYieldRule(
        uint32_t                      index,
        ReRevvedTerrainYieldRuleInfo* out,
        uint32_t                      out_size);
    REREVVED_TERRAIN_YIELD_RULES_API int32_t ReRevvedEvaluateTerrainYield(
        const ReRevvedTerrainYieldQuery* query,
        ReRevvedTerrainYieldEvaluation*  out,
        uint32_t                         out_size);

#ifdef __cplusplus
} // extern "C"
#endif

#undef REREVVED_TERRAIN_YIELD_RULES_API
