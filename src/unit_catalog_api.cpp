#include "unit_catalog_api.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <unit_catalog.h>

namespace rerevved::unit_catalog
{

namespace
{

constexpr uint32_t kDefinitionPrefix = 16;
constexpr uint32_t kIdentityPrefix   = 20;

struct UnitDefinitionEntry
{
    int32_t attack;
    int32_t defense;
};

constexpr std::array<UnitDefinitionEntry, REREVVED_UNIT_TYPE_COUNT>
    kUnitDefinitions = {
        {
            { 0, 0 },
            { 0, 0 },
            { 0, 1 },
            { 1, 1 },
            { 1, 1 },
            { 1, 1 },
            { 1, 1 },
            { 0, 1 },
            { 2, 1 },
            { 1, 2 },
            { 3, 5 },
            { 4, 8 },
            { 2, 1 },
            { 4, 2 },
            { 10, 6 },
            { 1, 3 },
            { 4, 1 },
            { 6, 2 },
            { 16, 2 },
            { 12, 2 },
            { 1, 1 },
            { 2, 2 },
            { 6, 6 },
            { 12, 18 },
            { 0, 3 },
            { 18, 3 },
            { 6, 4 },
            { 0, 0 },
            { 0, 0 },
        }
    };

struct UnitIdentityEntry
{
    ReRevvedCivilizationId civilization;
    ReRevvedUnitTypeId     unitType;
    ReRevvedUnitIdentityId identity;
};

constexpr std::array<UnitIdentityEntry, 27> kUnitIdentities = {
    {
        { REREVVED_CIVILIZATION_AZTEC,
          REREVVED_UNIT_TYPE_WARRIOR,
          REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR },
        { REREVVED_CIVILIZATION_ZULU,
          REREVVED_UNIT_TYPE_WARRIOR,
          REREVVED_UNIT_IDENTITY_IMPI_WARRIOR },
        { REREVVED_CIVILIZATION_JAPANESE,
          REREVVED_UNIT_TYPE_PHALANX,
          REREVVED_UNIT_IDENTITY_ASHIGARU_PIKEMEN },
        { REREVVED_CIVILIZATION_GREEK,
          REREVVED_UNIT_TYPE_PHALANX,
          REREVVED_UNIT_IDENTITY_HOPLITE },
        { REREVVED_CIVILIZATION_ENGLISH,
          REREVVED_UNIT_TYPE_ARCHER,
          REREVVED_UNIT_IDENTITY_LONGBOW_ARCHER },
        { REREVVED_CIVILIZATION_SPANISH,
          REREVVED_UNIT_TYPE_ARCHER,
          REREVVED_UNIT_IDENTITY_CROSSBOW_ARCHER },
        { REREVVED_CIVILIZATION_CHINESE,
          REREVVED_UNIT_TYPE_ARCHER,
          REREVVED_UNIT_IDENTITY_CROSSBOW_ARCHER },
        { REREVVED_CIVILIZATION_FRENCH,
          REREVVED_UNIT_TYPE_CATAPULT,
          REREVVED_UNIT_IDENTITY_TREBUCHET },
        { REREVVED_CIVILIZATION_RUSSIAN,
          REREVVED_UNIT_TYPE_HORSEMEN,
          REREVVED_UNIT_IDENTITY_COSSACK_HORSEMAN },
        { REREVVED_CIVILIZATION_JAPANESE,
          REREVVED_UNIT_TYPE_KNIGHTS,
          REREVVED_UNIT_IDENTITY_SAMURAI_KNIGHT },
        { REREVVED_CIVILIZATION_SPANISH,
          REREVVED_UNIT_TYPE_KNIGHTS,
          REREVVED_UNIT_IDENTITY_CONQUISTADOR },
        { REREVVED_CIVILIZATION_GERMAN,
          REREVVED_UNIT_TYPE_TANK,
          REREVVED_UNIT_IDENTITY_PANZER_TANK },
        { REREVVED_CIVILIZATION_RUSSIAN,
          REREVVED_UNIT_TYPE_TANK,
          REREVVED_UNIT_IDENTITY_T34_TANK },
        { REREVVED_CIVILIZATION_AMERICAN,
          REREVVED_UNIT_TYPE_TANK,
          REREVVED_UNIT_IDENTITY_SHERMAN_TANK },
        { REREVVED_CIVILIZATION_GERMAN,
          REREVVED_UNIT_TYPE_ARTILLERY,
          REREVVED_UNIT_IDENTITY_GERMAN_88MM_GUN },
        { REREVVED_CIVILIZATION_FRENCH,
          REREVVED_UNIT_TYPE_ARTILLERY,
          REREVVED_UNIT_IDENTITY_HOWITZER },
        { REREVVED_CIVILIZATION_JAPANESE,
          REREVVED_UNIT_TYPE_FIGHTER,
          REREVVED_UNIT_IDENTITY_ZERO_FIGHTER },
        { REREVVED_CIVILIZATION_AMERICAN,
          REREVVED_UNIT_TYPE_FIGHTER,
          REREVVED_UNIT_IDENTITY_MUSTANG_FIGHTER },
        { REREVVED_CIVILIZATION_ENGLISH,
          REREVVED_UNIT_TYPE_FIGHTER,
          REREVVED_UNIT_IDENTITY_SPITFIRE_FIGHTER },
        { REREVVED_CIVILIZATION_GERMAN,
          REREVVED_UNIT_TYPE_FIGHTER,
          REREVVED_UNIT_IDENTITY_ME109_FIGHTER },
        { REREVVED_CIVILIZATION_JAPANESE,
          REREVVED_UNIT_TYPE_BOMBER,
          REREVVED_UNIT_IDENTITY_VAL_BOMBER },
        { REREVVED_CIVILIZATION_AMERICAN,
          REREVVED_UNIT_TYPE_BOMBER,
          REREVVED_UNIT_IDENTITY_FLYING_FORTRESS },
        { REREVVED_CIVILIZATION_ENGLISH,
          REREVVED_UNIT_TYPE_BOMBER,
          REREVVED_UNIT_IDENTITY_LANCASTER_BOMBER },
        { REREVVED_CIVILIZATION_GERMAN,
          REREVVED_UNIT_TYPE_BOMBER,
          REREVVED_UNIT_IDENTITY_HEINKEL_BOMBER },
        { REREVVED_CIVILIZATION_GREEK,
          REREVVED_UNIT_TYPE_GALLEY,
          REREVVED_UNIT_IDENTITY_TRIREME },
        { REREVVED_CIVILIZATION_ROMAN,
          REREVVED_UNIT_TYPE_KNIGHTS,
          REREVVED_UNIT_IDENTITY_CATAPHRACT },
        { REREVVED_CIVILIZATION_MONGOLIAN,
          REREVVED_UNIT_TYPE_HORSEMEN,
          REREVVED_UNIT_IDENTITY_KESHIK },
    }
};

static_assert(kUnitIdentities.size() == 27);

bool isCivilizationIdValid(ReRevvedCivilizationId civilization)
{
    return civilization >= 0 && civilization < REREVVED_CIVILIZATION_COUNT;
}

bool isUnitTypeIdValid(ReRevvedUnitTypeId unitType)
{
    return unitType >= 0 && unitType < REREVVED_UNIT_TYPE_COUNT;
}

bool isDisplayFormValid(ReRevvedUnitDisplayForm displayForm)
{
    return displayForm == REREVVED_UNIT_DISPLAY_FORM_UNIT ||
           displayForm == REREVVED_UNIT_DISPLAY_FORM_ARMY;
}

void clearOutput(void* out, uint32_t outSize, uint32_t producerSize)
{
    if (out)
    {
        std::memset(out, 0, std::min(outSize, producerSize));
    }
}

} // namespace

int32_t CopySizedOutput(void*       out,
                        uint32_t    outSize,
                        const void* producer,
                        uint32_t    producerSize,
                        uint32_t    minimumPrefix)
{
    if (!out || !producer)
    {
        return REREVVED_UNIT_CATALOG_ERR_INVALID_ARGUMENT;
    }

    clearOutput(out, outSize, producerSize);
    if (outSize < minimumPrefix)
    {
        return REREVVED_UNIT_CATALOG_ERR_BUFFER_TOO_SMALL;
    }

    uint32_t copySize = std::min(outSize, producerSize);
    copySize -= copySize % sizeof(uint32_t);
    std::memcpy(out, producer, copySize);
    return REREVVED_UNIT_CATALOG_OK;
}

bool TryResolveUnitIdentity(ReRevvedCivilizationId  civilization,
                            ReRevvedUnitTypeId      unitType,
                            ReRevvedUnitIdentityId& identity)
{
    if (!isCivilizationIdValid(civilization) ||
        !isUnitTypeIdValid(unitType))
    {
        return false;
    }

    identity = REREVVED_UNIT_IDENTITY_BASE;
    for (const auto& entry : kUnitIdentities)
    {
        if (entry.civilization == civilization && entry.unitType == unitType)
        {
            identity = entry.identity;
            break;
        }
    }
    return true;
}

} // namespace rerevved::unit_catalog

static_assert(sizeof(ReRevvedCivilizationId) == sizeof(int32_t));
static_assert(sizeof(ReRevvedUnitTypeId) == sizeof(int32_t));
static_assert(sizeof(ReRevvedUnitIdentityId) == sizeof(int32_t));
static_assert(sizeof(ReRevvedUnitDisplayForm) == sizeof(int32_t));
static_assert(sizeof(ReRevvedUnitDefinition) == 32);
static_assert(offsetof(ReRevvedUnitDefinition, structSize) == 0);
static_assert(offsetof(ReRevvedUnitDefinition, unitType) == 4);
static_assert(offsetof(ReRevvedUnitDefinition, baseAttack) == 8);
static_assert(offsetof(ReRevvedUnitDefinition, baseDefense) == 12);
static_assert(offsetof(ReRevvedUnitDefinition, reserved) == 16);
static_assert(sizeof(ReRevvedUnitIdentity) == 32);
static_assert(offsetof(ReRevvedUnitIdentity, structSize) == 0);
static_assert(offsetof(ReRevvedUnitIdentity, civilization) == 4);
static_assert(offsetof(ReRevvedUnitIdentity, baseUnitType) == 8);
static_assert(offsetof(ReRevvedUnitIdentity, identity) == 12);
static_assert(offsetof(ReRevvedUnitIdentity, displayForm) == 16);
static_assert(offsetof(ReRevvedUnitIdentity, reserved) == 20);

extern "C" uint32_t ReRevvedUnitCatalogAbiVersion(void)
{
    return REREVVED_UNIT_CATALOG_ABI_VERSION;
}

extern "C" int32_t ReRevvedGetUnitDefinition(
    ReRevvedUnitTypeId      unitType,
    ReRevvedUnitDefinition* out,
    uint32_t                outSize)
{
    constexpr uint32_t kProducerSize = sizeof(ReRevvedUnitDefinition);
    if (!out)
    {
        return REREVVED_UNIT_CATALOG_ERR_INVALID_ARGUMENT;
    }

    rerevved::unit_catalog::clearOutput(out, outSize, kProducerSize);
    if (outSize < rerevved::unit_catalog::kDefinitionPrefix)
    {
        return REREVVED_UNIT_CATALOG_ERR_BUFFER_TOO_SMALL;
    }
    if (!rerevved::unit_catalog::isUnitTypeIdValid(unitType))
    {
        return REREVVED_UNIT_CATALOG_ERR_INVALID_ARGUMENT;
    }

    const auto&                  entry  = rerevved::unit_catalog::kUnitDefinitions[unitType];
    const ReRevvedUnitDefinition result = {
        kProducerSize,
        unitType,
        entry.attack,
        entry.defense,
        {},
    };
    return rerevved::unit_catalog::CopySizedOutput(
        out,
        outSize,
        &result,
        kProducerSize,
        rerevved::unit_catalog::kDefinitionPrefix);
}

extern "C" int32_t ReRevvedResolveUnitIdentity(
    ReRevvedCivilizationId  civilization,
    ReRevvedUnitTypeId      baseUnitType,
    ReRevvedUnitDisplayForm displayForm,
    ReRevvedUnitIdentity*   out,
    uint32_t                outSize)
{
    constexpr uint32_t kProducerSize = sizeof(ReRevvedUnitIdentity);
    if (!out)
    {
        return REREVVED_UNIT_CATALOG_ERR_INVALID_ARGUMENT;
    }

    rerevved::unit_catalog::clearOutput(out, outSize, kProducerSize);
    if (outSize < rerevved::unit_catalog::kIdentityPrefix)
    {
        return REREVVED_UNIT_CATALOG_ERR_BUFFER_TOO_SMALL;
    }
    if (!rerevved::unit_catalog::isCivilizationIdValid(civilization) ||
        !rerevved::unit_catalog::isUnitTypeIdValid(baseUnitType) ||
        !rerevved::unit_catalog::isDisplayFormValid(displayForm))
    {
        return REREVVED_UNIT_CATALOG_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUnitIdentityId identity = REREVVED_UNIT_IDENTITY_BASE;
    if (!rerevved::unit_catalog::TryResolveUnitIdentity(
            civilization, baseUnitType, identity))
    {
        return REREVVED_UNIT_CATALOG_ERR_INVALID_ARGUMENT;
    }

    const ReRevvedUnitIdentity result = {
        kProducerSize,
        civilization,
        baseUnitType,
        identity,
        displayForm,
        {},
    };
    return rerevved::unit_catalog::CopySizedOutput(
        out,
        outSize,
        &result,
        kProducerSize,
        rerevved::unit_catalog::kIdentityPrefix);
}
