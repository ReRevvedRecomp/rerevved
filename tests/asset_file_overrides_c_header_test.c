#include <asset_file_overrides.h>

#include <stddef.h>

int main(void)
{
    if (REREVVED_ASSET_FILE_OVERRIDES_ABI_VERSION != 1u ||
        REREVVED_ASSET_FILE_OVERRIDE_LOGO_DDS_SIZE != 1048704u)
    {
        return 1;
    }

    ReRevvedAssetFileOverridesAbiVersionFn version_fn =
        ReRevvedAssetFileOverridesAbiVersion;
    ReRevvedRegisterAssetFileOverrideFn register_fn =
        ReRevvedRegisterAssetFileOverride;
    ReRevvedAssetFileOverride rule = { 0 };

    if (version_fn() != REREVVED_ASSET_FILE_OVERRIDES_ABI_VERSION ||
        offsetof(ReRevvedAssetFileOverride, struct_size) != 0 ||
        offsetof(ReRevvedAssetFileOverride, provider_id) != 4 ||
        offsetof(ReRevvedAssetFileOverride, rule_id) != 68 ||
        offsetof(ReRevvedAssetFileOverride, path) != 132 ||
        rule.data != NULL || rule.data_size != 0)
    {
        return 1;
    }

    (void)register_fn;
    return 0;
}
