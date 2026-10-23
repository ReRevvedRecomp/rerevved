// Public C ABI for Stonehenge Wonder effects.
// ABI 1 supports stateless previews and runtime registration. Registrations
// are keyed by (providerId, ruleId); V1 and V2 share one identity space, and a
// V1 request is the V2 request with amount 0. Only
// WONDER_EFFECT_STONEHENGE_RELIGION registers live rules. A valid
// WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE request returns ERR_UNAVAILABLE
// and retains no rule.

#pragma once

#include <stdint.h>

#if defined(WONDER_EFFECT_RULES_API_EXPORTS)
#if defined(_WIN32)
#define WONDER_EFFECT_RULES_API __declspec(dllexport)
#else
#define WONDER_EFFECT_RULES_API __attribute__((visibility("default")))
#endif
#else
#define WONDER_EFFECT_RULES_API
#endif

#define WONDER_EFFECT_RULES_ABI_VERSION    1u
#define WONDER_EFFECT_RULES_V2_ABI_VERSION 2u
#define WONDER_EFFECT_RULE_ID_CAPACITY     64u

enum
{
    WONDER_EFFECT_RULES_OK                   = 0,
    WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT = -10,
    WONDER_EFFECT_RULES_ERR_BUFFER_TOO_SMALL = -11,
    WONDER_EFFECT_RULES_ERR_CONFLICT         = -12, // Same identity, different normalized request.
    WONDER_EFFECT_RULES_ERR_INTERNAL         = -13,
    WONDER_EFFECT_RULES_ERR_UNAVAILABLE      = -14,
};

typedef int32_t WonderEffectId;

enum
{
    WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE = 1, // Replace effects; new-completion Religion and 5 final Culture per city turn. Registration unavailable.
    WONDER_EFFECT_STONEHENGE_RELIGION         = 2, // Grant ordinary Religion once on new completion; native effects unchanged.
};

enum
{
    WONDER_EFFECT_PREVIEW_MATCHED  = 1u << 0,
    WONDER_EFFECT_PREVIEW_OVERFLOW = 1u << 1,
};

typedef struct WonderEffectRule
{
    uint32_t       structSize;
    char           providerId[WONDER_EFFECT_RULE_ID_CAPACITY];
    char           ruleId[WONDER_EFFECT_RULE_ID_CAPACITY];
    WonderEffectId effect;
    int32_t        reserved[6];
} WonderEffectRule;

typedef struct WonderEffectSupport
{
    uint32_t structSize;
    uint32_t runtimeAvailable; // Nonzero: valid registrations of the live effect are retained and applied.
    int32_t  reserved[6];
} WonderEffectSupport;

// Effect 2 requires amount 0. Effect 1 accepts any signed 32-bit amount.
typedef struct WonderEffectRuleV2
{
    uint32_t       structSize;
    char           providerId[WONDER_EFFECT_RULE_ID_CAPACITY];
    char           ruleId[WONDER_EFFECT_RULE_ID_CAPACITY];
    WonderEffectId effect;
    int32_t        amount;
    int32_t        reserved[5];
} WonderEffectRuleV2;

typedef struct WonderEffectSupportV2
{
    uint32_t structSize;
    uint32_t runtimeAvailable; // Nonzero: valid registrations of the live effect are retained and applied.
    int32_t  minAmount;        // Accepted amount range of the live effect.
    int32_t  maxAmount;
    int32_t  reserved[4];
} WonderEffectSupportV2;

// Hypothetical inputs, not live city state. Culture must fit signed 16 bits.
typedef struct WonderEffectPreviewQuery
{
    uint32_t structSize;
    int32_t  cultureBeforeFixedAddition; // Effect 1: after removing native Stonehenge effects and applying native modifiers. Effect 2: native Culture.
    uint32_t hasStonehenge;              // Exactly 0 or 1, for the city containing the Wonder.
    uint32_t isNewCompletion;            // Exactly 0 or 1; activation, load, transfer, and recalculation are not completion.
    uint32_t religionOwned;              // Exactly 0 or 1, for the completing city's owner.
    int32_t  reserved[3];
} WonderEffectPreviewQuery;

typedef struct WonderEffectPreview
{
    uint32_t structSize;
    int32_t  cultureBeforeFixedAddition;
    int32_t  finalCulture;
    uint32_t grantReligion;        // Intent only; preview acquires no technology.
    uint32_t replaceNativeEffects; // Intent only; no native consumer is suppressed. Effect 2 reports 0.
    uint32_t statusFlags;
    int32_t  reserved[2];
} WonderEffectPreview;

typedef uint32_t (*WonderEffectRulesAbiVersionFn)(void);
typedef uint32_t (*WonderEffectRulesV2AbiVersionFn)(void);
typedef int32_t (*GetWonderEffectSupportV2Fn)(WonderEffectSupportV2* out, uint32_t outSize);
typedef int32_t (*RegisterWonderEffectRuleV2Fn)(const WonderEffectRuleV2* rule);
typedef int32_t (*PreviewWonderEffectV2Fn)(const WonderEffectRuleV2* rule, const WonderEffectPreviewQuery* query, WonderEffectPreview* out, uint32_t outSize);
typedef int32_t (*GetWonderEffectSupportFn)(WonderEffectSupport* out, uint32_t outSize);
typedef int32_t (*RegisterWonderEffectRuleFn)(const WonderEffectRule* rule);
typedef int32_t (*PreviewWonderEffectFn)(const WonderEffectRule* rule, const WonderEffectPreviewQuery* query, WonderEffectPreview* out, uint32_t outSize);

// Requests require their full size, zero reserved fields, and NUL-terminated
// lowercase provider/rule IDs. Outputs require at least 32 bytes. Preview is
// stateless and does not detect completion or mutate gameplay. See the API guide.
#ifdef __cplusplus
extern "C"
{
#endif
    WONDER_EFFECT_RULES_API uint32_t WonderEffectRulesAbiVersion(void);
    WONDER_EFFECT_RULES_API uint32_t WonderEffectRulesV2AbiVersion(void);
    WONDER_EFFECT_RULES_API int32_t  GetWonderEffectSupportV2(WonderEffectSupportV2* out, uint32_t outSize);
    WONDER_EFFECT_RULES_API int32_t  RegisterWonderEffectRuleV2(const WonderEffectRuleV2* rule);
    WONDER_EFFECT_RULES_API int32_t  PreviewWonderEffectV2(const WonderEffectRuleV2* rule, const WonderEffectPreviewQuery* query, WonderEffectPreview* out, uint32_t outSize);
    WONDER_EFFECT_RULES_API int32_t  GetWonderEffectSupport(WonderEffectSupport* out, uint32_t outSize);
    WONDER_EFFECT_RULES_API int32_t  RegisterWonderEffectRule(const WonderEffectRule* rule);
    WONDER_EFFECT_RULES_API int32_t  PreviewWonderEffect(const WonderEffectRule* rule, const WonderEffectPreviewQuery* query, WonderEffectPreview* out, uint32_t outSize);
#ifdef __cplusplus
}
#endif

#undef WONDER_EFFECT_RULES_API
