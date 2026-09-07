// Public C ABI for civilization information screen text replacements.
//
// Mods resolve these entry points from the host process and check
// ReRevvedNationSelectTextAbiVersion before calling them. Registrations are
// copied by the host. ABI 1 accepts complete printable ASCII replacement
// fields and lines.

#pragma once

#include <stdint.h>

#include <game_ids.h>
#include <unique_era_abilities.h>

#if defined(REREVVED_NATION_SELECT_TEXT_API_EXPORTS)
#if defined(_WIN32)
#define REREVVED_NATION_SELECT_TEXT_API __declspec(dllexport)
#else
#define REREVVED_NATION_SELECT_TEXT_API __attribute__((visibility("default")))
#endif
#else
#define REREVVED_NATION_SELECT_TEXT_API
#endif

#define REREVVED_NATION_SELECT_TEXT_ABI_VERSION      1u
#define REREVVED_NATION_SELECT_TEXT_RULE_ID_CAPACITY 64u
#define REREVVED_NATION_SELECT_TEXT_CAPACITY         256u

enum
{
    REREVVED_NATION_SELECT_TEXT_OK                    = 0,
    REREVVED_NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT  = -10,
    REREVVED_NATION_SELECT_TEXT_ERR_BUFFER_TOO_SMALL  = -11,
    REREVVED_NATION_SELECT_TEXT_ERR_DUPLICATE_RULE_ID = -12,
    REREVVED_NATION_SELECT_TEXT_ERR_INTERNAL          = -13,
};

typedef int32_t ReRevvedNationSelectTextSurface;

enum
{
    // Civilization, unlock_era, and ability select one effective era value.
    REREVVED_NATION_SELECT_TEXT_SURFACE_ERA_ABILITY = 0,
    // Civilization, base_unit_type, identity, and display_form select one unit.
    REREVVED_NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT = 1,
    // These three fields use civilization; every other selector is UNUSED.
    REREVVED_NATION_SELECT_TEXT_SURFACE_LEADER_NAME        = 2,
    REREVVED_NATION_SELECT_TEXT_SURFACE_CIVILIZATION_NAME  = 3,
    REREVVED_NATION_SELECT_TEXT_SURFACE_CIVILIZATION_TRAIT = 4,
    // Surface 5 was reserved after runtime testing proved the internal era
    // block prefix is not rendered by the civilization information panel.
    REREVVED_NATION_SELECT_TEXT_SURFACE_RESERVED_5                  = 5,
    // Global headings use civilization UNUSED. ERA_HEADING additionally uses
    // unlock_era; every other selector is UNUSED.
    REREVVED_NATION_SELECT_TEXT_SURFACE_ERA_HEADING                 = 6,
    REREVVED_NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT_SECTION_HEADING = 7,
};

enum
{
    REREVVED_NATION_SELECT_TEXT_SELECTOR_UNUSED = -1,
};

enum
{
    REREVVED_NATION_SELECT_TEXT_RULE_REPLACEMENT_CONFLICT = 1u << 0,
};

enum
{
    REREVVED_NATION_SELECT_TEXT_EVALUATION_REPLACED             = 1u << 0,
    REREVVED_NATION_SELECT_TEXT_EVALUATION_REPLACEMENT_CONFLICT = 1u << 1,
};

typedef struct ReRevvedNationSelectTextRule
{
    uint32_t                    struct_size;
    char                        provider_id[REREVVED_NATION_SELECT_TEXT_RULE_ID_CAPACITY];
    char                        rule_id[REREVVED_NATION_SELECT_TEXT_RULE_ID_CAPACITY];
    ReRevvedNationSelectTextSurface surface;
    ReRevvedCivilizationId      civilization;
    ReRevvedUniqueEraUnlockEra  unlock_era;
    ReRevvedUniqueEraAbilityId  ability;
    ReRevvedUnitTypeId          base_unit_type;
    ReRevvedUnitIdentityId      identity;
    // ABI 1 accepts UNIT for the civilization information screen.
    ReRevvedUnitDisplayForm display_form;
    char                    text[REREVVED_NATION_SELECT_TEXT_CAPACITY];
    int32_t                 reserved[8];
} ReRevvedNationSelectTextRule;

typedef struct ReRevvedNationSelectTextRuleInfo
{
    // Current producer size. Callers may pass any buffer at least 420 bytes.
    uint32_t                    struct_size;
    char                        provider_id[REREVVED_NATION_SELECT_TEXT_RULE_ID_CAPACITY];
    char                        rule_id[REREVVED_NATION_SELECT_TEXT_RULE_ID_CAPACITY];
    ReRevvedNationSelectTextSurface surface;
    ReRevvedCivilizationId      civilization;
    ReRevvedUniqueEraUnlockEra  unlock_era;
    ReRevvedUniqueEraAbilityId  ability;
    ReRevvedUnitTypeId          base_unit_type;
    ReRevvedUnitIdentityId      identity;
    ReRevvedUnitDisplayForm     display_form;
    char                        text[REREVVED_NATION_SELECT_TEXT_CAPACITY];
    uint32_t                    status_flags;
    int32_t                     reserved[8];
} ReRevvedNationSelectTextRuleInfo;

typedef struct ReRevvedNationSelectTextQuery
{
    uint32_t                    struct_size;
    ReRevvedNationSelectTextSurface surface;
    ReRevvedCivilizationId      civilization;
    ReRevvedUniqueEraUnlockEra  unlock_era;
    ReRevvedUniqueEraAbilityId  ability;
    ReRevvedUnitTypeId          base_unit_type;
    ReRevvedUnitIdentityId      identity;
    ReRevvedUnitDisplayForm     display_form;
    int32_t                     reserved[8];
} ReRevvedNationSelectTextQuery;

typedef struct ReRevvedNationSelectTextEvaluation
{
    // Current producer size. Callers may pass any buffer at least 268 bytes.
    uint32_t struct_size;
    uint32_t replacement_count;
    uint32_t status_flags;
    char     text[REREVVED_NATION_SELECT_TEXT_CAPACITY];
    int32_t  reserved[8];
} ReRevvedNationSelectTextEvaluation;

typedef uint32_t (*ReRevvedNationSelectTextAbiVersionFn)(void);
typedef int32_t (*ReRevvedRegisterNationSelectTextRuleFn)(
    const ReRevvedNationSelectTextRule* rule);
typedef int32_t (*ReRevvedGetNationSelectTextRuleCountFn)(uint32_t* out_count);
typedef int32_t (*ReRevvedGetNationSelectTextRuleFn)(
    uint32_t                          index,
    ReRevvedNationSelectTextRuleInfo* out,
    uint32_t                          out_size);
typedef int32_t (*ReRevvedEvaluateNationSelectTextFn)(
    const ReRevvedNationSelectTextQuery* query,
    ReRevvedNationSelectTextEvaluation*  out,
    uint32_t                             out_size);

#ifdef __cplusplus
extern "C"
{
#endif

    REREVVED_NATION_SELECT_TEXT_API uint32_t
    ReRevvedNationSelectTextAbiVersion(void);
    REREVVED_NATION_SELECT_TEXT_API int32_t
    ReRevvedRegisterNationSelectTextRule(
        const ReRevvedNationSelectTextRule* rule);
    REREVVED_NATION_SELECT_TEXT_API int32_t
                                           ReRevvedGetNationSelectTextRuleCount(uint32_t* out_count);
    REREVVED_NATION_SELECT_TEXT_API int32_t ReRevvedGetNationSelectTextRule(
        uint32_t                          index,
        ReRevvedNationSelectTextRuleInfo* out,
        uint32_t                          out_size);
    REREVVED_NATION_SELECT_TEXT_API int32_t ReRevvedEvaluateNationSelectText(
        const ReRevvedNationSelectTextQuery* query,
        ReRevvedNationSelectTextEvaluation*  out,
        uint32_t                             out_size);

#ifdef __cplusplus
} // extern "C"
#endif

#undef REREVVED_NATION_SELECT_TEXT_API
