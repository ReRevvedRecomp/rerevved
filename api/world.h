// Public C ABI for ReRevved's static world catalog.
//
// Mods resolve these entry points from the host process and check
// ReRevvedWorldAbiVersion before calling them. Results are copied into
// caller-owned storage and contain no guest pointers or borrowed strings.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(REREVVED_WORLD_API_EXPORTS)
#if defined(_WIN32)
#define REREVVED_WORLD_API __declspec(dllexport)
#else
#define REREVVED_WORLD_API __attribute__((visibility("default")))
#endif
#else
#define REREVVED_WORLD_API
#endif

#define REREVVED_WORLD_ABI_VERSION 1u

enum
{
    REREVVED_WORLD_OK                   = 0,
    REREVVED_WORLD_ERR_INVALID_ARGUMENT = -10,
    REREVVED_WORLD_ERR_BUFFER_TOO_SMALL = -11,
};

typedef struct ReRevvedWorldUnitDefinition
{
    // Current producer size. Callers may pass any buffer at least 16 bytes.
    uint32_t           struct_size;
    ReRevvedUnitTypeId unit_type;
    int32_t            base_attack;
    int32_t            base_defense;
    int32_t            reserved[4];
} ReRevvedWorldUnitDefinition;

typedef struct ReRevvedWorldUnitIdentity
{
    // Current producer size. Callers may pass any buffer at least 20 bytes.
    uint32_t                struct_size;
    ReRevvedCivilizationId  civilization;
    ReRevvedUnitTypeId      base_unit_type;
    ReRevvedUnitIdentityId  identity;
    ReRevvedUnitDisplayForm display_form;
    int32_t                 reserved[3];
} ReRevvedWorldUnitIdentity;

// A null output returns INVALID_ARGUMENT. Otherwise each query first clears
// min(out_size, current producer size). Storage below the documented minimum
// prefix returns BUFFER_TOO_SMALL before ID validation. Accepted prefixes
// report the current producer size and contain only complete 32-bit fields;
// bytes beyond the producer size are never changed.

typedef uint32_t (*ReRevvedWorldAbiVersionFn)(void);
typedef int32_t (*ReRevvedGetUnitDefinitionFn)(
    ReRevvedUnitTypeId           unit_type,
    ReRevvedWorldUnitDefinition* out,
    uint32_t                     out_size);
typedef int32_t (*ReRevvedResolveUnitIdentityFn)(
    ReRevvedCivilizationId     civilization,
    ReRevvedUnitTypeId         base_unit_type,
    ReRevvedUnitDisplayForm    display_form,
    ReRevvedWorldUnitIdentity* out,
    uint32_t                   out_size);

#ifdef __cplusplus
extern "C"
{
#endif

    REREVVED_WORLD_API uint32_t ReRevvedWorldAbiVersion(void);
    // Returns the immutable base definition for one unit type.
    REREVVED_WORLD_API int32_t ReRevvedGetUnitDefinition(
        ReRevvedUnitTypeId           unit_type,
        ReRevvedWorldUnitDefinition* out,
        uint32_t                     out_size);
    // Returns a civilization-specific identity or BASE when none exists.
    // Display form is echoed but does not change the resolved identity.
    REREVVED_WORLD_API int32_t ReRevvedResolveUnitIdentity(
        ReRevvedCivilizationId     civilization,
        ReRevvedUnitTypeId         base_unit_type,
        ReRevvedUnitDisplayForm    display_form,
        ReRevvedWorldUnitIdentity* out,
        uint32_t                   out_size);

#ifdef __cplusplus
} // extern "C"
#endif

#undef REREVVED_WORLD_API
