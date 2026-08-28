// Public C ABI for ReRevved Unique Era Ability replacement rules.
//
// Mods resolve these entry points from the host process and check
// ReRevvedUniqueEraAbilitiesAbiVersion before calling them. Registrations are
// copied by the host and affect only the cumulative era-ability lookup.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(REREVVED_UNIQUE_ERA_ABILITIES_API_EXPORTS)
#if defined(_WIN32)
#define REREVVED_UNIQUE_ERA_ABILITIES_API __declspec(dllexport)
#else
#define REREVVED_UNIQUE_ERA_ABILITIES_API __attribute__((visibility("default")))
#endif
#else
#define REREVVED_UNIQUE_ERA_ABILITIES_API
#endif

#define REREVVED_UNIQUE_ERA_ABILITIES_ABI_VERSION    1u
#define REREVVED_UNIQUE_ERA_ABILITY_RULE_ID_CAPACITY 64u

enum
{
    REREVVED_UNIQUE_ERA_ABILITIES_OK                    = 0,
    REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT  = -10,
    REREVVED_UNIQUE_ERA_ABILITIES_ERR_BUFFER_TOO_SMALL  = -11,
    REREVVED_UNIQUE_ERA_ABILITIES_ERR_DUPLICATE_RULE_ID = -12,
    REREVVED_UNIQUE_ERA_ABILITIES_ERR_INTERNAL          = -13,
};

typedef int32_t ReRevvedUniqueEraUnlockEra;

enum
{
    REREVVED_UNIQUE_ERA_ANCIENT    = 0,
    REREVVED_UNIQUE_ERA_MEDIEVAL   = 1,
    REREVVED_UNIQUE_ERA_INDUSTRIAL = 2,
    REREVVED_UNIQUE_ERA_MODERN     = 3,
};

typedef int32_t ReRevvedUniqueEraAbilityId;

// Accepted semantic IDs from the retail 16 by 4 Unique Era Ability table.
enum
{
    REREVVED_UNIQUE_ERA_ABILITY_ROADS_HALF_COST                  = 1,
    REREVVED_UNIQUE_ERA_ABILITY_FACTORIES_TRIPLE_PRODUCTION      = 2,
    REREVVED_UNIQUE_ERA_ABILITY_CAVALRY_PLUS_ONE_MOVEMENT        = 3,
    REREVVED_UNIQUE_ERA_ABILITY_TEMPLES_PLUS_THREE_SCIENCE       = 4,
    REREVVED_UNIQUE_ERA_ABILITY_SETTLERS_HALF_COST               = 5,
    REREVVED_UNIQUE_ERA_ABILITY_NAVAL_PLUS_ONE_COMBAT            = 6,
    REREVVED_UNIQUE_ERA_ABILITY_EXPLORATION_DOUBLE_GOLD          = 7,
    REREVVED_UNIQUE_ERA_ABILITY_FASTER_CITY_GROWTH               = 8,
    REREVVED_UNIQUE_ERA_ABILITY_MATHEMATICS                      = 9,
    REREVVED_UNIQUE_ERA_ABILITY_LITERACY                         = 10,
    REREVVED_UNIQUE_ERA_ABILITY_RIFLEMEN_PLUS_ONE_MOVEMENT       = 12,
    REREVVED_UNIQUE_ERA_ABILITY_CANNONS_PLUS_TWO_ATTACK          = 13,
    REREVVED_UNIQUE_ERA_ABILITY_CAVALRY_KNIGHTS_PLUS_ONE_ATTACK  = 14,
    REREVVED_UNIQUE_ERA_ABILITY_PLAINS_PLUS_ONE_FOOD             = 16,
    REREVVED_UNIQUE_ERA_ABILITY_RIFLEMEN_HALF_COST               = 17,
    REREVVED_UNIQUE_ERA_ABILITY_COURTHOUSES_HALF_COST            = 18,
    REREVVED_UNIQUE_ERA_ABILITY_BARRACKS_HALF_COST               = 19,
    REREVVED_UNIQUE_ERA_ABILITY_LIBRARIES_HALF_COST              = 20,
    REREVVED_UNIQUE_ERA_ABILITY_INCREASED_GREAT_PEOPLE           = 23,
    REREVVED_UNIQUE_ERA_ABILITY_WONDERS_HALF_COST                = 24,
    REREVVED_UNIQUE_ERA_ABILITY_CITIES_PLUS_FIFTY_PERCENT_GOLD   = 25,
    REREVVED_UNIQUE_ERA_ABILITY_DEFENSIVE_UNITS_LOYALTY          = 26,
    REREVVED_UNIQUE_ERA_ABILITY_SAMURAI_PLUS_ONE_ATTACK          = 27,
    REREVVED_UNIQUE_ERA_ABILITY_SEA_PLUS_ONE_FOOD                = 28,
    REREVVED_UNIQUE_ERA_ABILITY_FOREST_PLUS_ONE_PRODUCTION       = 30,
    REREVVED_UNIQUE_ERA_ABILITY_DESERT_PLUS_FOOD_AND_TRADE       = 32,
    REREVVED_UNIQUE_ERA_ABILITY_SPIES_HALF_COST                  = 34,
    REREVVED_UNIQUE_ERA_ABILITY_UNIT_RUSH_HALF_COST              = 35,
    REREVVED_UNIQUE_ERA_ABILITY_NEW_CITIES_PLUS_ONE_POPULATION   = 36,
    REREVVED_UNIQUE_ERA_ABILITY_CARAVANS_PLUS_FIFTY_PERCENT_GOLD = 38,
    REREVVED_UNIQUE_ERA_ABILITY_BARBARIAN_VILLAGES_BECOME_CITIES = 40,
    REREVVED_UNIQUE_ERA_ABILITY_HILLS_PLUS_ONE_PRODUCTION        = 41,
    REREVVED_UNIQUE_ERA_ABILITY_NO_ANARCHY                       = 42,
    REREVVED_UNIQUE_ERA_ABILITY_RELIGION                         = 43,
    REREVVED_UNIQUE_ERA_ABILITY_HEAL_AFTER_COMBAT                = 46,
    REREVVED_UNIQUE_ERA_ABILITY_GOLD_TWO_PERCENT_INTEREST        = 47,
    REREVVED_UNIQUE_ERA_ABILITY_COMMUNISM                        = 48,
    REREVVED_UNIQUE_ERA_ABILITY_NEW_WARRIORS_VETERAN             = 50,
    REREVVED_UNIQUE_ERA_ABILITY_DOUBLE_NAVAL_SUPPORT             = 51,
    REREVVED_UNIQUE_ERA_ABILITY_WARRIORS_PLUS_ONE_MOVEMENT       = 55,
    REREVVED_UNIQUE_ERA_ABILITY_MOUNTAINS_PLUS_TWO_PRODUCTION    = 56,
    REREVVED_UNIQUE_ERA_ABILITY_IRRIGATION                       = 58,
    REREVVED_UNIQUE_ERA_ABILITY_POTTERY                          = 59,
    REREVVED_UNIQUE_ERA_ABILITY_DEMOCRACY                        = 60,
    REREVVED_UNIQUE_ERA_ABILITY_LONGBOW_PLUS_ONE_DEFENSE         = 61,
};

enum
{
    REREVVED_UNIQUE_ERA_ABILITY_RULE_REPLACEMENT_CONFLICT = 1u << 0,
};

enum
{
    REREVVED_UNIQUE_ERA_ABILITY_EVALUATION_REPLACED             = 1u << 0,
    REREVVED_UNIQUE_ERA_ABILITY_EVALUATION_REPLACEMENT_CONFLICT = 1u << 1,
};

typedef struct ReRevvedUniqueEraAbilityReplacement
{
    uint32_t                   struct_size;
    char                       provider_id[REREVVED_UNIQUE_ERA_ABILITY_RULE_ID_CAPACITY];
    char                       rule_id[REREVVED_UNIQUE_ERA_ABILITY_RULE_ID_CAPACITY];
    ReRevvedCivilizationId     civilization;
    ReRevvedUniqueEraUnlockEra unlock_era;
    ReRevvedUniqueEraAbilityId replacement_ability;
    int32_t                    reserved[8];
} ReRevvedUniqueEraAbilityReplacement;

typedef struct ReRevvedUniqueEraAbilityRuleInfo
{
    // Current producer size. Callers may pass any buffer at least 148 bytes.
    uint32_t                   struct_size;
    char                       provider_id[REREVVED_UNIQUE_ERA_ABILITY_RULE_ID_CAPACITY];
    char                       rule_id[REREVVED_UNIQUE_ERA_ABILITY_RULE_ID_CAPACITY];
    ReRevvedCivilizationId     civilization;
    ReRevvedUniqueEraUnlockEra unlock_era;
    ReRevvedUniqueEraAbilityId replacement_ability;
    uint32_t                   status_flags;
    int32_t                    reserved[8];
} ReRevvedUniqueEraAbilityRuleInfo;

typedef struct ReRevvedUniqueEraAbilityCellQuery
{
    uint32_t                   struct_size;
    ReRevvedCivilizationId     civilization;
    ReRevvedUniqueEraUnlockEra unlock_era;
    ReRevvedUniqueEraAbilityId native_ability;
    int32_t                    reserved[6];
} ReRevvedUniqueEraAbilityCellQuery;

typedef struct ReRevvedUniqueEraAbilityCellEvaluation
{
    // Current producer size. Callers may pass any buffer at least 20 bytes.
    uint32_t                   struct_size;
    ReRevvedUniqueEraAbilityId native_ability;
    ReRevvedUniqueEraAbilityId effective_ability;
    uint32_t                   replacement_count;
    uint32_t                   status_flags;
    int32_t                    reserved[5];
} ReRevvedUniqueEraAbilityCellEvaluation;

typedef uint32_t (*ReRevvedUniqueEraAbilitiesAbiVersionFn)(void);
typedef int32_t (*ReRevvedRegisterUniqueEraAbilityReplacementFn)(
    const ReRevvedUniqueEraAbilityReplacement* rule);
typedef int32_t (*ReRevvedGetUniqueEraAbilityRuleCountFn)(uint32_t* out_count);
typedef int32_t (*ReRevvedGetUniqueEraAbilityRuleFn)(
    uint32_t                          index,
    ReRevvedUniqueEraAbilityRuleInfo* out,
    uint32_t                          out_size);
typedef int32_t (*ReRevvedEvaluateUniqueEraAbilityCellFn)(
    const ReRevvedUniqueEraAbilityCellQuery* query,
    ReRevvedUniqueEraAbilityCellEvaluation*  out,
    uint32_t                                 out_size);

#ifdef __cplusplus
extern "C"
{
#endif

    REREVVED_UNIQUE_ERA_ABILITIES_API uint32_t
    ReRevvedUniqueEraAbilitiesAbiVersion(void);
    REREVVED_UNIQUE_ERA_ABILITIES_API int32_t
    ReRevvedRegisterUniqueEraAbilityReplacement(
        const ReRevvedUniqueEraAbilityReplacement* rule);
    REREVVED_UNIQUE_ERA_ABILITIES_API int32_t
    ReRevvedGetUniqueEraAbilityRuleCount(uint32_t* out_count);
    REREVVED_UNIQUE_ERA_ABILITIES_API int32_t
    ReRevvedGetUniqueEraAbilityRule(
        uint32_t                          index,
        ReRevvedUniqueEraAbilityRuleInfo* out,
        uint32_t                          out_size);
    REREVVED_UNIQUE_ERA_ABILITIES_API int32_t
    ReRevvedEvaluateUniqueEraAbilityCell(
        const ReRevvedUniqueEraAbilityCellQuery* query,
        ReRevvedUniqueEraAbilityCellEvaluation*  out,
        uint32_t                                 out_size);

#ifdef __cplusplus
} // extern "C"
#endif

#undef REREVVED_UNIQUE_ERA_ABILITIES_API
