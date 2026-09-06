// Public C ABI for synchronous type-11 asset file overrides.
//
// ABI 1 accepts the exact GFX_MainMenu_logo.dds path and its validated legacy
// DDS payload. Registration copies the caller's bytes before returning.

#pragma once

#include <stdint.h>

#if defined(REREVVED_ASSET_FILE_OVERRIDES_API_EXPORTS)
#if defined(_WIN32)
#define REREVVED_ASSET_FILE_OVERRIDES_API __declspec(dllexport)
#else
#define REREVVED_ASSET_FILE_OVERRIDES_API __attribute__((visibility("default")))
#endif
#else
#define REREVVED_ASSET_FILE_OVERRIDES_API
#endif

#define REREVVED_ASSET_FILE_OVERRIDES_ABI_VERSION         1u
#define REREVVED_ASSET_FILE_OVERRIDE_PROVIDER_ID_CAPACITY 64u
#define REREVVED_ASSET_FILE_OVERRIDE_RULE_ID_CAPACITY     64u
#define REREVVED_ASSET_FILE_OVERRIDE_PATH_CAPACITY        128u
#define REREVVED_ASSET_FILE_OVERRIDE_LOGO_DDS_SIZE        1048704u

enum
{
    REREVVED_ASSET_FILE_OVERRIDES_OK                   = 0,
    REREVVED_ASSET_FILE_OVERRIDES_ERR_INVALID_ARGUMENT = -10,
    REREVVED_ASSET_FILE_OVERRIDES_ERR_UNSUPPORTED_PATH = -11,
    REREVVED_ASSET_FILE_OVERRIDES_ERR_INVALID_PAYLOAD  = -12,
    REREVVED_ASSET_FILE_OVERRIDES_ERR_DUPLICATE_PATH   = -13,
    REREVVED_ASSET_FILE_OVERRIDES_ERR_INTERNAL         = -14,
};

typedef struct ReRevvedAssetFileOverride
{
    uint32_t    struct_size;
    char        provider_id[REREVVED_ASSET_FILE_OVERRIDE_PROVIDER_ID_CAPACITY];
    char        rule_id[REREVVED_ASSET_FILE_OVERRIDE_RULE_ID_CAPACITY];
    char        path[REREVVED_ASSET_FILE_OVERRIDE_PATH_CAPACITY];
    const void* data;
    uint32_t    data_size;
    uint32_t    reserved[8];
} ReRevvedAssetFileOverride;

typedef uint32_t (*ReRevvedAssetFileOverridesAbiVersionFn)(void);
typedef int32_t (*ReRevvedRegisterAssetFileOverrideFn)(
    const ReRevvedAssetFileOverride* rule);

#ifdef __cplusplus
extern "C"
{
#endif

    REREVVED_ASSET_FILE_OVERRIDES_API uint32_t
    ReRevvedAssetFileOverridesAbiVersion(void);
    REREVVED_ASSET_FILE_OVERRIDES_API int32_t
    ReRevvedRegisterAssetFileOverride(
        const ReRevvedAssetFileOverride* rule);

#ifdef __cplusplus
} // extern "C"
#endif

#undef REREVVED_ASSET_FILE_OVERRIDES_API
