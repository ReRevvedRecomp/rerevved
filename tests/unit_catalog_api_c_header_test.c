#include <gameplay_state.h>
#include <unit_catalog.h>

#include <stddef.h>

#if REREVVED_GAMEPLAY_CIVILIZATION_UNKNOWN != -1
#error "gameplay civilization unknown must remain a numeric preprocessor value"
#endif

int main(void)
{
    ReRevvedUnitCatalogAbiVersionFn version_fn    = ReRevvedUnitCatalogAbiVersion;
    ReRevvedGetUnitDefinitionFn     definition_fn = ReRevvedGetUnitDefinition;
    ReRevvedResolveUnitIdentityFn   identity_fn   = ReRevvedResolveUnitIdentity;
    ReRevvedUnitDefinition          definition    = { 0 };
    ReRevvedUnitIdentity            identity      = { 0 };

    if (sizeof(ReRevvedGameplayState) != 80 ||
        offsetof(ReRevvedGameplayState, civilization) != 44 ||
        sizeof(ReRevvedUnitDefinition) != 32 ||
        sizeof(ReRevvedUnitIdentity) != 32 ||
        REREVVED_GAMEPLAY_CIVILIZATION_UNKNOWN !=
            REREVVED_CIVILIZATION_UNKNOWN)
    {
        return 1;
    }
    if (version_fn() != REREVVED_UNIT_CATALOG_ABI_VERSION ||
        definition_fn(REREVVED_UNIT_TYPE_KNIGHTS,
                      &definition,
                      sizeof(definition)) != REREVVED_UNIT_CATALOG_OK ||
        identity_fn(REREVVED_CIVILIZATION_ROMAN,
                    REREVVED_UNIT_TYPE_KNIGHTS,
                    REREVVED_UNIT_DISPLAY_FORM_UNIT,
                    &identity,
                    sizeof(identity)) != REREVVED_UNIT_CATALOG_OK)
    {
        return 1;
    }
    return definition.base_attack == 4 &&
                   identity.identity == REREVVED_UNIT_IDENTITY_CATAPHRACT
               ? 0
               : 1;
}
