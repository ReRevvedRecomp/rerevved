#include <gameplay_state.h>
#include <unit_catalog.h>

#include <stddef.h>

#if GAMEPLAY_CIVILIZATION_UNKNOWN != -1
#error "gameplay civilization unknown must remain a numeric preprocessor value"
#endif

int main(void)
{
    UnitCatalogAbiVersionFn versionFn    = UnitCatalogAbiVersion;
    GetUnitDefinitionFn     definitionFn = GetUnitDefinition;
    ResolveUnitIdentityFn   identityFn   = ResolveUnitIdentity;
    UnitDefinition          definition   = { 0 };
    UnitIdentity            identity     = { 0 };

    if (sizeof(GameplayState) != 80 ||
        offsetof(GameplayState, civilization) != 44 ||
        sizeof(UnitDefinition) != 32 ||
        sizeof(UnitIdentity) != 32 ||
        GAMEPLAY_CIVILIZATION_UNKNOWN !=
            CIVILIZATION_UNKNOWN)
    {
        return 1;
    }
    if (versionFn() != UNIT_CATALOG_ABI_VERSION ||
        definitionFn(UNIT_TYPE_KNIGHTS,
                     &definition,
                     sizeof(definition)) != UNIT_CATALOG_OK ||
        identityFn(CIVILIZATION_ROMAN,
                   UNIT_TYPE_KNIGHTS,
                   UNIT_DISPLAY_FORM_UNIT,
                   &identity,
                   sizeof(identity)) != UNIT_CATALOG_OK)
    {
        return 1;
    }
    return definition.baseAttack == 4 &&
                   identity.identity == UNIT_IDENTITY_CATAPHRACT
               ? 0
               : 1;
}
