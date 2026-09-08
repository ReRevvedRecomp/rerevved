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

constexpr std::array<UnitDefinitionEntry, UNIT_TYPE_COUNT>
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
    CivilizationId civilization;
    UnitTypeId     unitType;
    UnitIdentityId identity;
};

constexpr std::array<UnitIdentityEntry, 27> kUnitIdentities = {
    {
        { CIVILIZATION_AZTEC,
          UNIT_TYPE_WARRIOR,
          UNIT_IDENTITY_JAGUAR_WARRIOR },
        { CIVILIZATION_ZULU,
          UNIT_TYPE_WARRIOR,
          UNIT_IDENTITY_IMPI_WARRIOR },
        { CIVILIZATION_JAPANESE,
          UNIT_TYPE_PHALANX,
          UNIT_IDENTITY_ASHIGARU_PIKEMEN },
        { CIVILIZATION_GREEK,
          UNIT_TYPE_PHALANX,
          UNIT_IDENTITY_HOPLITE },
        { CIVILIZATION_ENGLISH,
          UNIT_TYPE_ARCHER,
          UNIT_IDENTITY_LONGBOW_ARCHER },
        { CIVILIZATION_SPANISH,
          UNIT_TYPE_ARCHER,
          UNIT_IDENTITY_CROSSBOW_ARCHER },
        { CIVILIZATION_CHINESE,
          UNIT_TYPE_ARCHER,
          UNIT_IDENTITY_CROSSBOW_ARCHER },
        { CIVILIZATION_FRENCH,
          UNIT_TYPE_CATAPULT,
          UNIT_IDENTITY_TREBUCHET },
        { CIVILIZATION_RUSSIAN,
          UNIT_TYPE_HORSEMEN,
          UNIT_IDENTITY_COSSACK_HORSEMAN },
        { CIVILIZATION_JAPANESE,
          UNIT_TYPE_KNIGHTS,
          UNIT_IDENTITY_SAMURAI_KNIGHT },
        { CIVILIZATION_SPANISH,
          UNIT_TYPE_KNIGHTS,
          UNIT_IDENTITY_CONQUISTADOR },
        { CIVILIZATION_GERMAN,
          UNIT_TYPE_TANK,
          UNIT_IDENTITY_PANZER_TANK },
        { CIVILIZATION_RUSSIAN,
          UNIT_TYPE_TANK,
          UNIT_IDENTITY_T34_TANK },
        { CIVILIZATION_AMERICAN,
          UNIT_TYPE_TANK,
          UNIT_IDENTITY_SHERMAN_TANK },
        { CIVILIZATION_GERMAN,
          UNIT_TYPE_ARTILLERY,
          UNIT_IDENTITY_GERMAN_88MM_GUN },
        { CIVILIZATION_FRENCH,
          UNIT_TYPE_ARTILLERY,
          UNIT_IDENTITY_HOWITZER },
        { CIVILIZATION_JAPANESE,
          UNIT_TYPE_FIGHTER,
          UNIT_IDENTITY_ZERO_FIGHTER },
        { CIVILIZATION_AMERICAN,
          UNIT_TYPE_FIGHTER,
          UNIT_IDENTITY_MUSTANG_FIGHTER },
        { CIVILIZATION_ENGLISH,
          UNIT_TYPE_FIGHTER,
          UNIT_IDENTITY_SPITFIRE_FIGHTER },
        { CIVILIZATION_GERMAN,
          UNIT_TYPE_FIGHTER,
          UNIT_IDENTITY_ME109_FIGHTER },
        { CIVILIZATION_JAPANESE,
          UNIT_TYPE_BOMBER,
          UNIT_IDENTITY_VAL_BOMBER },
        { CIVILIZATION_AMERICAN,
          UNIT_TYPE_BOMBER,
          UNIT_IDENTITY_FLYING_FORTRESS },
        { CIVILIZATION_ENGLISH,
          UNIT_TYPE_BOMBER,
          UNIT_IDENTITY_LANCASTER_BOMBER },
        { CIVILIZATION_GERMAN,
          UNIT_TYPE_BOMBER,
          UNIT_IDENTITY_HEINKEL_BOMBER },
        { CIVILIZATION_GREEK,
          UNIT_TYPE_GALLEY,
          UNIT_IDENTITY_TRIREME },
        { CIVILIZATION_ROMAN,
          UNIT_TYPE_KNIGHTS,
          UNIT_IDENTITY_CATAPHRACT },
        { CIVILIZATION_MONGOLIAN,
          UNIT_TYPE_HORSEMEN,
          UNIT_IDENTITY_KESHIK },
    }
};

static_assert(kUnitIdentities.size() == 27);

bool isCivilizationIdValid(CivilizationId civilization)
{
    return civilization >= 0 && civilization < CIVILIZATION_COUNT;
}

bool isUnitTypeIdValid(UnitTypeId unitType)
{
    return unitType >= 0 && unitType < UNIT_TYPE_COUNT;
}

bool isDisplayFormValid(UnitDisplayForm displayForm)
{
    return displayForm == UNIT_DISPLAY_FORM_UNIT ||
           displayForm == UNIT_DISPLAY_FORM_ARMY;
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
        return UNIT_CATALOG_ERR_INVALID_ARGUMENT;
    }

    clearOutput(out, outSize, producerSize);
    if (outSize < minimumPrefix)
    {
        return UNIT_CATALOG_ERR_BUFFER_TOO_SMALL;
    }

    uint32_t copySize = std::min(outSize, producerSize);
    copySize -= copySize % sizeof(uint32_t);
    std::memcpy(out, producer, copySize);
    return UNIT_CATALOG_OK;
}

bool TryResolveUnitIdentity(CivilizationId  civilization,
                            UnitTypeId      unitType,
                            UnitIdentityId& identity)
{
    if (!isCivilizationIdValid(civilization) ||
        !isUnitTypeIdValid(unitType))
    {
        return false;
    }

    identity = UNIT_IDENTITY_BASE;
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

static_assert(sizeof(CivilizationId) == sizeof(int32_t));
static_assert(sizeof(UnitTypeId) == sizeof(int32_t));
static_assert(sizeof(UnitIdentityId) == sizeof(int32_t));
static_assert(sizeof(UnitDisplayForm) == sizeof(int32_t));
static_assert(sizeof(UnitDefinition) == 32);
static_assert(offsetof(UnitDefinition, structSize) == 0);
static_assert(offsetof(UnitDefinition, unitType) == 4);
static_assert(offsetof(UnitDefinition, baseAttack) == 8);
static_assert(offsetof(UnitDefinition, baseDefense) == 12);
static_assert(offsetof(UnitDefinition, reserved) == 16);
static_assert(sizeof(UnitIdentity) == 32);
static_assert(offsetof(UnitIdentity, structSize) == 0);
static_assert(offsetof(UnitIdentity, civilization) == 4);
static_assert(offsetof(UnitIdentity, baseUnitType) == 8);
static_assert(offsetof(UnitIdentity, identity) == 12);
static_assert(offsetof(UnitIdentity, displayForm) == 16);
static_assert(offsetof(UnitIdentity, reserved) == 20);

extern "C" uint32_t UnitCatalogAbiVersion(void)
{
    return UNIT_CATALOG_ABI_VERSION;
}

extern "C" int32_t GetUnitDefinition(
    UnitTypeId      unitType,
    UnitDefinition* out,
    uint32_t        outSize)
{
    constexpr uint32_t kProducerSize = sizeof(UnitDefinition);
    if (!out)
    {
        return UNIT_CATALOG_ERR_INVALID_ARGUMENT;
    }

    rerevved::unit_catalog::clearOutput(out, outSize, kProducerSize);
    if (outSize < rerevved::unit_catalog::kDefinitionPrefix)
    {
        return UNIT_CATALOG_ERR_BUFFER_TOO_SMALL;
    }
    if (!rerevved::unit_catalog::isUnitTypeIdValid(unitType))
    {
        return UNIT_CATALOG_ERR_INVALID_ARGUMENT;
    }

    const auto&          entry  = rerevved::unit_catalog::kUnitDefinitions[unitType];
    const UnitDefinition result = {
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

extern "C" int32_t ResolveUnitIdentity(
    CivilizationId  civilization,
    UnitTypeId      baseUnitType,
    UnitDisplayForm displayForm,
    UnitIdentity*   out,
    uint32_t        outSize)
{
    constexpr uint32_t kProducerSize = sizeof(UnitIdentity);
    if (!out)
    {
        return UNIT_CATALOG_ERR_INVALID_ARGUMENT;
    }

    rerevved::unit_catalog::clearOutput(out, outSize, kProducerSize);
    if (outSize < rerevved::unit_catalog::kIdentityPrefix)
    {
        return UNIT_CATALOG_ERR_BUFFER_TOO_SMALL;
    }
    if (!rerevved::unit_catalog::isCivilizationIdValid(civilization) ||
        !rerevved::unit_catalog::isUnitTypeIdValid(baseUnitType) ||
        !rerevved::unit_catalog::isDisplayFormValid(displayForm))
    {
        return UNIT_CATALOG_ERR_INVALID_ARGUMENT;
    }

    UnitIdentityId identity = UNIT_IDENTITY_BASE;
    if (!rerevved::unit_catalog::TryResolveUnitIdentity(
            civilization, baseUnitType, identity))
    {
        return UNIT_CATALOG_ERR_INVALID_ARGUMENT;
    }

    const UnitIdentity result = {
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
