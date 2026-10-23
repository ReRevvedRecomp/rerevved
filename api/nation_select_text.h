// Public C ABI for civilization information screen text replacements.
//
// Mods resolve these entry points from the host process and check
// NationSelectTextAbiVersion before calling them. Registrations are
// copied by the host. ABI 2 accepts complete printable ASCII replacement
// fields and lines.

#pragma once

#include <stdint.h>

#include <game_ids.h>
#include <unique_era_abilities.h>

#if defined(NATION_SELECT_TEXT_API_EXPORTS)
#if defined(_WIN32)
#define NATION_SELECT_TEXT_API __declspec(dllexport)
#else
#define NATION_SELECT_TEXT_API __attribute__((visibility("default")))
#endif
#else
#define NATION_SELECT_TEXT_API
#endif

#define NATION_SELECT_TEXT_ABI_VERSION      2u
#define NATION_SELECT_TEXT_RULE_ID_CAPACITY 64u
#define NATION_SELECT_TEXT_CAPACITY         256u

enum
{
    NATION_SELECT_TEXT_OK                    = 0,
    NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT  = -10,
    NATION_SELECT_TEXT_ERR_BUFFER_TOO_SMALL  = -11,
    NATION_SELECT_TEXT_ERR_DUPLICATE_RULE_ID = -12,
    NATION_SELECT_TEXT_ERR_INTERNAL          = -13,
};

typedef int32_t NationSelectTextSurface;

enum
{
    NATION_SELECT_TEXT_SURFACE_ERA_ABILITY                 = 0, // civilization, unlockEra, ability select an effective era value.
    NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT                 = 1, // civilization, baseUnitType, identity, displayForm select a unit.
    NATION_SELECT_TEXT_SURFACE_LEADER_NAME                 = 2, // civilization only; other selectors UNUSED.
    NATION_SELECT_TEXT_SURFACE_CIVILIZATION_NAME           = 3, // civilization only; other selectors UNUSED.
    NATION_SELECT_TEXT_SURFACE_CIVILIZATION_TRAIT          = 4, // civilization only; other selectors UNUSED.
    NATION_SELECT_TEXT_SURFACE_RESERVED_5                  = 5, // Reserved; the internal era block prefix is not displayed.
    NATION_SELECT_TEXT_SURFACE_ERA_HEADING                 = 6, // unlockEra only; other selectors UNUSED.
    NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT_SECTION_HEADING = 7, // Global heading; all selectors UNUSED.
};

enum
{
    NATION_SELECT_TEXT_SELECTOR_UNUSED = -1,
};

enum
{
    NATION_SELECT_TEXT_RULE_REPLACEMENT_CONFLICT = 1u << 0,
};

enum
{
    NATION_SELECT_TEXT_EVALUATION_REPLACED             = 1u << 0,
    NATION_SELECT_TEXT_EVALUATION_REPLACEMENT_CONFLICT = 1u << 1,
};

typedef struct NationSelectTextRule
{
    uint32_t                structSize;
    char                    providerId[NATION_SELECT_TEXT_RULE_ID_CAPACITY];
    char                    ruleId[NATION_SELECT_TEXT_RULE_ID_CAPACITY];
    NationSelectTextSurface surface;
    CivilizationId          civilization;
    UnlockEra               unlockEra;
    EraAbilityId            ability;
    UnitTypeId              baseUnitType;
    UnitIdentityId          identity;
    UnitDisplayForm         displayForm; // ABI 2 accepts UNIT for the civilization information screen.
    char                    text[NATION_SELECT_TEXT_CAPACITY];
    int32_t                 reserved[8];
} NationSelectTextRule;

typedef struct NationSelectTextRuleInfo
{
    uint32_t                structSize; // Current producer size. Callers may pass any buffer at least 420 bytes.
    char                    providerId[NATION_SELECT_TEXT_RULE_ID_CAPACITY];
    char                    ruleId[NATION_SELECT_TEXT_RULE_ID_CAPACITY];
    NationSelectTextSurface surface;
    CivilizationId          civilization;
    UnlockEra               unlockEra;
    EraAbilityId            ability;
    UnitTypeId              baseUnitType;
    UnitIdentityId          identity;
    UnitDisplayForm         displayForm;
    char                    text[NATION_SELECT_TEXT_CAPACITY];
    uint32_t                statusFlags;
    int32_t                 reserved[8];
} NationSelectTextRuleInfo;

typedef struct NationSelectTextQuery
{
    uint32_t                structSize;
    NationSelectTextSurface surface;
    CivilizationId          civilization;
    UnlockEra               unlockEra;
    EraAbilityId            ability;
    UnitTypeId              baseUnitType;
    UnitIdentityId          identity;
    UnitDisplayForm         displayForm;
    int32_t                 reserved[8];
} NationSelectTextQuery;

typedef struct NationSelectTextEvaluation
{
    uint32_t structSize; // Current producer size. Callers may pass any buffer at least 268 bytes.
    uint32_t replacementCount;
    uint32_t statusFlags;
    char     text[NATION_SELECT_TEXT_CAPACITY];
    int32_t  reserved[8];
} NationSelectTextEvaluation;

typedef uint32_t (*NationSelectTextAbiVersionFn)(void);
typedef int32_t (*RegisterNationSelectTextRuleFn)(
    const NationSelectTextRule* rule);
typedef int32_t (*GetNationSelectTextRuleCountFn)(uint32_t* outCount);
typedef int32_t (*GetNationSelectTextRuleFn)(
    uint32_t                  index,
    NationSelectTextRuleInfo* out,
    uint32_t                  outSize);
typedef int32_t (*EvaluateNationSelectTextFn)(
    const NationSelectTextQuery* query,
    NationSelectTextEvaluation*  out,
    uint32_t                     outSize);

#ifdef __cplusplus
extern "C"
{
#endif

    NATION_SELECT_TEXT_API uint32_t NationSelectTextAbiVersion(void);
    NATION_SELECT_TEXT_API int32_t  RegisterNationSelectTextRule(
        const NationSelectTextRule* rule);
    NATION_SELECT_TEXT_API int32_t GetNationSelectTextRuleCount(uint32_t* outCount);
    NATION_SELECT_TEXT_API int32_t GetNationSelectTextRule(
        uint32_t                  index,
        NationSelectTextRuleInfo* out,
        uint32_t                  outSize);
    NATION_SELECT_TEXT_API int32_t EvaluateNationSelectText(
        const NationSelectTextQuery* query,
        NationSelectTextEvaluation*  out,
        uint32_t                     outSize);

#ifdef __cplusplus
} // extern "C"
#endif

#undef NATION_SELECT_TEXT_API
