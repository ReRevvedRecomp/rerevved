#include <game_state.h>
#include <world.h>

#include <stddef.h>

#if REREVVED_GAMEPLAY_CIVILIZATION_UNKNOWN != -1
#error "gameplay civilization unknown must remain a numeric preprocessor value"
#endif

int main(void)
{
    ReRevvedWorldAbiVersionFn     version_fn    = ReRevvedWorldAbiVersion;
    ReRevvedGetUnitDefinitionFn   definition_fn = ReRevvedGetUnitDefinition;
    ReRevvedResolveUnitIdentityFn identity_fn   = ReRevvedResolveUnitIdentity;
    ReRevvedWorldUnitDefinition   definition    = { 0 };
    ReRevvedWorldUnitIdentity     identity      = { 0 };

    if (sizeof(ReRevvedGameplayState) != 80 ||
        offsetof(ReRevvedGameplayState, civilization) != 44 ||
        sizeof(ReRevvedWorldUnitDefinition) != 32 ||
        sizeof(ReRevvedWorldUnitIdentity) != 32 ||
        REREVVED_GAMEPLAY_CIVILIZATION_UNKNOWN !=
            REREVVED_CIVILIZATION_UNKNOWN)
    {
        return 1;
    }
    if (version_fn() != REREVVED_WORLD_ABI_VERSION ||
        definition_fn(REREVVED_UNIT_TYPE_KNIGHTS,
                      &definition,
                      sizeof(definition)) != REREVVED_WORLD_OK ||
        identity_fn(REREVVED_CIVILIZATION_ROMAN,
                    REREVVED_UNIT_TYPE_KNIGHTS,
                    REREVVED_UNIT_DISPLAY_FORM_UNIT,
                    &identity,
                    sizeof(identity)) != REREVVED_WORLD_OK)
    {
        return 1;
    }
    return definition.base_attack == 4 &&
                   identity.identity == REREVVED_UNIT_IDENTITY_CATAPHRACT
               ? 0
               : 1;
}
