// Public C ABI for ReRevved Unique Era Ability replacement rules.
//
// Mods resolve these entry points from the host process and check
// EraAbilitiesAbiVersion before calling them. Registrations are
// copied by the host. Retail replacements compose at the cumulative lookup;
// title-owned synthetic abilities may also have fixed effects documented here.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(ERA_ABILITIES_API_EXPORTS)
#if defined(_WIN32)
#define ERA_ABILITIES_API __declspec(dllexport)
#else
#define ERA_ABILITIES_API __attribute__((visibility("default")))
#endif
#else
#define ERA_ABILITIES_API
#endif

#define ERA_ABILITIES_ABI_VERSION    3u
#define ERA_ABILITY_RULE_ID_CAPACITY 64u

enum
{
    ERA_ABILITIES_OK                    = 0,
    ERA_ABILITIES_ERR_INVALID_ARGUMENT  = -10,
    ERA_ABILITIES_ERR_BUFFER_TOO_SMALL  = -11,
    ERA_ABILITIES_ERR_DUPLICATE_RULE_ID = -12,
    ERA_ABILITIES_ERR_INTERNAL          = -13,
};

typedef int32_t UnlockEra;

enum
{
    UNLOCK_ERA_ANCIENT    = 0,
    UNLOCK_ERA_MEDIEVAL   = 1,
    UNLOCK_ERA_INDUSTRIAL = 2,
    UNLOCK_ERA_MODERN     = 3,
};

typedef int32_t EraAbilityId;

// Accepted semantic IDs from the retail 16 by 4 Unique Era Ability table.
enum
{
    ERA_ABILITY_ROADS_HALF_COST                  = 1,
    ERA_ABILITY_FACTORIES_TRIPLE_PRODUCTION      = 2,
    ERA_ABILITY_CAVALRY_PLUS_ONE_MOVEMENT        = 3,
    ERA_ABILITY_TEMPLES_PLUS_THREE_SCIENCE       = 4,
    ERA_ABILITY_SETTLERS_HALF_COST               = 5,
    ERA_ABILITY_NAVAL_PLUS_ONE_COMBAT            = 6,
    ERA_ABILITY_EXPLORATION_DOUBLE_GOLD          = 7,
    ERA_ABILITY_FASTER_CITY_GROWTH               = 8,
    ERA_ABILITY_MATHEMATICS                      = 9,
    ERA_ABILITY_LITERACY                         = 10,
    ERA_ABILITY_RIFLEMEN_PLUS_ONE_MOVEMENT       = 12,
    ERA_ABILITY_CANNONS_PLUS_TWO_ATTACK          = 13,
    ERA_ABILITY_CAVALRY_KNIGHTS_PLUS_ONE_ATTACK  = 14,
    ERA_ABILITY_PLAINS_PLUS_ONE_FOOD             = 16,
    ERA_ABILITY_RIFLEMEN_HALF_COST               = 17,
    ERA_ABILITY_COURTHOUSES_HALF_COST            = 18,
    ERA_ABILITY_BARRACKS_HALF_COST               = 19,
    ERA_ABILITY_LIBRARIES_HALF_COST              = 20,
    ERA_ABILITY_INCREASED_GREAT_PEOPLE           = 23,
    ERA_ABILITY_WONDERS_HALF_COST                = 24,
    ERA_ABILITY_CITIES_PLUS_FIFTY_PERCENT_GOLD   = 25,
    ERA_ABILITY_DEFENSIVE_UNITS_LOYALTY          = 26,
    ERA_ABILITY_SAMURAI_PLUS_ONE_ATTACK          = 27,
    ERA_ABILITY_SEA_PLUS_ONE_FOOD                = 28,
    ERA_ABILITY_FOREST_PLUS_ONE_PRODUCTION       = 30,
    ERA_ABILITY_DESERT_PLUS_FOOD_AND_TRADE       = 32,
    ERA_ABILITY_SPIES_HALF_COST                  = 34,
    ERA_ABILITY_UNIT_RUSH_HALF_COST              = 35,
    ERA_ABILITY_NEW_CITIES_PLUS_ONE_POPULATION   = 36,
    ERA_ABILITY_CARAVANS_PLUS_FIFTY_PERCENT_GOLD = 38,
    ERA_ABILITY_BARBARIAN_VILLAGES_BECOME_CITIES = 40,
    ERA_ABILITY_HILLS_PLUS_ONE_PRODUCTION        = 41,
    ERA_ABILITY_NO_ANARCHY                       = 42,
    ERA_ABILITY_RELIGION                         = 43,
    ERA_ABILITY_HEAL_AFTER_COMBAT                = 46,
    ERA_ABILITY_GOLD_TWO_PERCENT_INTEREST        = 47,
    ERA_ABILITY_COMMUNISM                        = 48,
    ERA_ABILITY_NEW_WARRIORS_VETERAN             = 50,
    ERA_ABILITY_DOUBLE_NAVAL_SUPPORT             = 51,
    ERA_ABILITY_WARRIORS_PLUS_ONE_MOVEMENT       = 55,
    ERA_ABILITY_MOUNTAINS_PLUS_TWO_PRODUCTION    = 56,
    ERA_ABILITY_IRRIGATION                       = 58,
    ERA_ABILITY_POTTERY                          = 59,
    ERA_ABILITY_DEMOCRACY                        = 60,
    ERA_ABILITY_LONGBOW_PLUS_ONE_DEFENSE         = 61,
};

// Title-owned synthetic abilities. Their fixed effects are part of this ABI;
// mods cannot define new ability IDs or parameterize these effects.
enum
{
    ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING =
        0x10000,
};

enum
{
    ERA_ABILITY_RULE_REPLACEMENT_CONFLICT = 1u << 0,
};

enum
{
    ERA_ABILITY_EVALUATION_REPLACED             = 1u << 0,
    ERA_ABILITY_EVALUATION_REPLACEMENT_CONFLICT = 1u << 1,
};

typedef struct EraAbilityReplacement
{
    uint32_t       structSize;
    char           providerId[ERA_ABILITY_RULE_ID_CAPACITY];
    char           ruleId[ERA_ABILITY_RULE_ID_CAPACITY];
    CivilizationId civilization;
    UnlockEra      unlockEra;
    EraAbilityId   replacementAbility;
    int32_t        reserved[8];
} EraAbilityReplacement;

typedef struct EraAbilityRuleInfo
{
    uint32_t       structSize; // Current producer size. Callers may pass any buffer at least 148 bytes.
    char           providerId[ERA_ABILITY_RULE_ID_CAPACITY];
    char           ruleId[ERA_ABILITY_RULE_ID_CAPACITY];
    CivilizationId civilization;
    UnlockEra      unlockEra;
    EraAbilityId   replacementAbility;
    uint32_t       statusFlags;
    int32_t        reserved[8];
} EraAbilityRuleInfo;

typedef struct EraAbilityCellQuery
{
    uint32_t       structSize;
    CivilizationId civilization;
    UnlockEra      unlockEra;
    EraAbilityId   nativeAbility;
    int32_t        reserved[6];
} EraAbilityCellQuery;

typedef struct EraAbilityCellEvaluation
{
    uint32_t     structSize; // Current producer size. Callers may pass any buffer at least 20 bytes.
    EraAbilityId nativeAbility;
    EraAbilityId effectiveAbility;
    uint32_t     replacementCount;
    uint32_t     statusFlags;
    int32_t      reserved[5];
} EraAbilityCellEvaluation;

typedef uint32_t (*EraAbilitiesAbiVersionFn)(void);
typedef int32_t (*RegisterEraAbilityReplacementFn)(
    const EraAbilityReplacement* rule);
typedef int32_t (*GetEraAbilityRuleCountFn)(uint32_t* outCount);
typedef int32_t (*GetEraAbilityRuleFn)(
    uint32_t            index,
    EraAbilityRuleInfo* out,
    uint32_t            outSize);
typedef int32_t (*EvaluateEraAbilityCellFn)(
    const EraAbilityCellQuery* query,
    EraAbilityCellEvaluation*  out,
    uint32_t                   outSize);

#ifdef __cplusplus
extern "C"
{
#endif

    ERA_ABILITIES_API uint32_t EraAbilitiesAbiVersion(void);
    ERA_ABILITIES_API int32_t  RegisterEraAbilityReplacement(
        const EraAbilityReplacement* rule);
    ERA_ABILITIES_API int32_t GetEraAbilityRuleCount(uint32_t* outCount);
    ERA_ABILITIES_API int32_t GetEraAbilityRule(
        uint32_t            index,
        EraAbilityRuleInfo* out,
        uint32_t            outSize);
    ERA_ABILITIES_API int32_t EvaluateEraAbilityCell(
        const EraAbilityCellQuery* query,
        EraAbilityCellEvaluation*  out,
        uint32_t                   outSize);

#ifdef __cplusplus
} // extern "C"
#endif

#undef ERA_ABILITIES_API
