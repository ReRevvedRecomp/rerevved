#include "world_api.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <world.h>

namespace rerevved::world
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
    ReRevvedUnitTypeId     unit_type;
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

bool IsCivilizationIdValid(ReRevvedCivilizationId civilization)
{
    return civilization >= 0 && civilization < REREVVED_CIVILIZATION_COUNT;
}

bool IsUnitTypeIdValid(ReRevvedUnitTypeId unit_type)
{
    return unit_type >= 0 && unit_type < REREVVED_UNIT_TYPE_COUNT;
}

bool IsDisplayFormValid(ReRevvedUnitDisplayForm display_form)
{
    return display_form == REREVVED_UNIT_DISPLAY_FORM_UNIT ||
           display_form == REREVVED_UNIT_DISPLAY_FORM_ARMY;
}

void ClearOutput(void* out, uint32_t out_size, uint32_t producer_size)
{
    if (out)
    {
        std::memset(out, 0, std::min(out_size, producer_size));
    }
}

} // namespace

int32_t CopySizedOutput(void*       out,
                        uint32_t    out_size,
                        const void* producer,
                        uint32_t    producer_size,
                        uint32_t    minimum_prefix)
{
    if (!out || !producer)
    {
        return REREVVED_WORLD_ERR_INVALID_ARGUMENT;
    }

    ClearOutput(out, out_size, producer_size);
    if (out_size < minimum_prefix)
    {
        return REREVVED_WORLD_ERR_BUFFER_TOO_SMALL;
    }

    uint32_t copy_size = std::min(out_size, producer_size);
    copy_size -= copy_size % sizeof(uint32_t);
    std::memcpy(out, producer, copy_size);
    return REREVVED_WORLD_OK;
}

} // namespace rerevved::world

static_assert(sizeof(ReRevvedCivilizationId) == sizeof(int32_t));
static_assert(sizeof(ReRevvedUnitTypeId) == sizeof(int32_t));
static_assert(sizeof(ReRevvedUnitIdentityId) == sizeof(int32_t));
static_assert(sizeof(ReRevvedUnitDisplayForm) == sizeof(int32_t));
static_assert(sizeof(ReRevvedWorldUnitDefinition) == 32);
static_assert(offsetof(ReRevvedWorldUnitDefinition, struct_size) == 0);
static_assert(offsetof(ReRevvedWorldUnitDefinition, unit_type) == 4);
static_assert(offsetof(ReRevvedWorldUnitDefinition, base_attack) == 8);
static_assert(offsetof(ReRevvedWorldUnitDefinition, base_defense) == 12);
static_assert(offsetof(ReRevvedWorldUnitDefinition, reserved) == 16);
static_assert(sizeof(ReRevvedWorldUnitIdentity) == 32);
static_assert(offsetof(ReRevvedWorldUnitIdentity, struct_size) == 0);
static_assert(offsetof(ReRevvedWorldUnitIdentity, civilization) == 4);
static_assert(offsetof(ReRevvedWorldUnitIdentity, base_unit_type) == 8);
static_assert(offsetof(ReRevvedWorldUnitIdentity, identity) == 12);
static_assert(offsetof(ReRevvedWorldUnitIdentity, display_form) == 16);
static_assert(offsetof(ReRevvedWorldUnitIdentity, reserved) == 20);

extern "C" uint32_t ReRevvedWorldAbiVersion(void)
{
    return REREVVED_WORLD_ABI_VERSION;
}

extern "C" int32_t ReRevvedGetUnitDefinition(
    ReRevvedUnitTypeId           unit_type,
    ReRevvedWorldUnitDefinition* out,
    uint32_t                     out_size)
{
    constexpr uint32_t kProducerSize = sizeof(ReRevvedWorldUnitDefinition);
    if (!out)
    {
        return REREVVED_WORLD_ERR_INVALID_ARGUMENT;
    }

    rerevved::world::ClearOutput(out, out_size, kProducerSize);
    if (out_size < rerevved::world::kDefinitionPrefix)
    {
        return REREVVED_WORLD_ERR_BUFFER_TOO_SMALL;
    }
    if (!rerevved::world::IsUnitTypeIdValid(unit_type))
    {
        return REREVVED_WORLD_ERR_INVALID_ARGUMENT;
    }

    const auto&                       entry  = rerevved::world::kUnitDefinitions[unit_type];
    const ReRevvedWorldUnitDefinition result = {
        kProducerSize,
        unit_type,
        entry.attack,
        entry.defense,
        {},
    };
    return rerevved::world::CopySizedOutput(
        out,
        out_size,
        &result,
        kProducerSize,
        rerevved::world::kDefinitionPrefix);
}

extern "C" int32_t ReRevvedResolveUnitIdentity(
    ReRevvedCivilizationId     civilization,
    ReRevvedUnitTypeId         base_unit_type,
    ReRevvedUnitDisplayForm    display_form,
    ReRevvedWorldUnitIdentity* out,
    uint32_t                   out_size)
{
    constexpr uint32_t kProducerSize = sizeof(ReRevvedWorldUnitIdentity);
    if (!out)
    {
        return REREVVED_WORLD_ERR_INVALID_ARGUMENT;
    }

    rerevved::world::ClearOutput(out, out_size, kProducerSize);
    if (out_size < rerevved::world::kIdentityPrefix)
    {
        return REREVVED_WORLD_ERR_BUFFER_TOO_SMALL;
    }
    if (!rerevved::world::IsCivilizationIdValid(civilization) ||
        !rerevved::world::IsUnitTypeIdValid(base_unit_type) ||
        !rerevved::world::IsDisplayFormValid(display_form))
    {
        return REREVVED_WORLD_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUnitIdentityId identity = REREVVED_UNIT_IDENTITY_BASE;
    for (const auto& entry : rerevved::world::kUnitIdentities)
    {
        if (entry.civilization == civilization &&
            entry.unit_type == base_unit_type)
        {
            identity = entry.identity;
            break;
        }
    }

    const ReRevvedWorldUnitIdentity result = {
        kProducerSize,
        civilization,
        base_unit_type,
        identity,
        display_form,
        {},
    };
    return rerevved::world::CopySizedOutput(
        out,
        out_size,
        &result,
        kProducerSize,
        rerevved::world::kIdentityPrefix);
}
