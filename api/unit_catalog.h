// Public C ABI for ReRevved's static unit catalog.
//
// Mods resolve these entry points from the host process and check
// UnitCatalogAbiVersion before calling them. Results are copied into
// caller-owned storage and contain no guest pointers or borrowed strings.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(UNIT_CATALOG_API_EXPORTS)
#if defined(_WIN32)
#define UNIT_CATALOG_API __declspec(dllexport)
#else
#define UNIT_CATALOG_API __attribute__((visibility("default")))
#endif
#else
#define UNIT_CATALOG_API
#endif

#define UNIT_CATALOG_ABI_VERSION 2u

enum
{
    UNIT_CATALOG_OK                   = 0,
    UNIT_CATALOG_ERR_INVALID_ARGUMENT = -10,
    UNIT_CATALOG_ERR_BUFFER_TOO_SMALL = -11,
};

typedef struct UnitDefinition
{
    uint32_t   structSize; // Current producer size. Callers may pass any buffer at least 16 bytes.
    UnitTypeId unitType;
    int32_t    baseAttack;
    int32_t    baseDefense;
    int32_t    reserved[4];
} UnitDefinition;

typedef struct UnitIdentity
{
    uint32_t        structSize; // Current producer size. Callers may pass any buffer at least 20 bytes.
    CivilizationId  civilization;
    UnitTypeId      baseUnitType;
    UnitIdentityId  identity;
    UnitDisplayForm displayForm;
    int32_t         reserved[3];
} UnitIdentity;

// A null output returns INVALID_ARGUMENT. Otherwise each query first clears
// min(outSize, current producer size). Storage below the documented minimum
// prefix returns BUFFER_TOO_SMALL before ID validation. Accepted prefixes
// report the current producer size and contain only complete 32-bit fields;
// bytes beyond the producer size are never changed.

typedef uint32_t (*UnitCatalogAbiVersionFn)(void);
typedef int32_t (*GetUnitDefinitionFn)(
    UnitTypeId      unitType,
    UnitDefinition* out,
    uint32_t        outSize);
typedef int32_t (*ResolveUnitIdentityFn)(
    CivilizationId  civilization,
    UnitTypeId      baseUnitType,
    UnitDisplayForm displayForm,
    UnitIdentity*   out,
    uint32_t        outSize);

#ifdef __cplusplus
extern "C"
{
#endif

    UNIT_CATALOG_API uint32_t UnitCatalogAbiVersion(void);
    // Returns the immutable base definition for one unit type.
    UNIT_CATALOG_API int32_t GetUnitDefinition(
        UnitTypeId      unitType,
        UnitDefinition* out,
        uint32_t        outSize);
    // Returns a civilization-specific identity or BASE when none exists.
    // Display form is echoed but does not change the resolved identity.
    UNIT_CATALOG_API int32_t ResolveUnitIdentity(
        CivilizationId  civilization,
        UnitTypeId      baseUnitType,
        UnitDisplayForm displayForm,
        UnitIdentity*   out,
        uint32_t        outSize);

#ifdef __cplusplus
} // extern "C"
#endif

#undef UNIT_CATALOG_API
