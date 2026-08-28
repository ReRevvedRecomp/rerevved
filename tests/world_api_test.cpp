#include "world_api.h"

#include <game_state.h>
#include <world.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string_view>
#include <type_traits>

namespace
{

int failures = 0;

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

struct DefinitionFixture
{
    ReRevvedUnitTypeId unit_type;
    int32_t            attack;
    int32_t            defense;
};

constexpr std::array<DefinitionFixture, REREVVED_UNIT_TYPE_COUNT>
    kDefinitions = {
        {
            { REREVVED_UNIT_TYPE_SETTLERS, 0, 0 },
            { REREVVED_UNIT_TYPE_FSETTLER, 0, 0 },
            { REREVVED_UNIT_TYPE_NAVAL_CREW, 0, 1 },
            { REREVVED_UNIT_TYPE_BARBARIAN_HOT, 1, 1 },
            { REREVVED_UNIT_TYPE_BARBARIAN_TEMPERATE, 1, 1 },
            { REREVVED_UNIT_TYPE_BARBARIAN_COLD, 1, 1 },
            { REREVVED_UNIT_TYPE_WARRIOR, 1, 1 },
            { REREVVED_UNIT_TYPE_MILITIA, 0, 1 },
            { REREVVED_UNIT_TYPE_LEGION, 2, 1 },
            { REREVVED_UNIT_TYPE_ARCHER, 1, 2 },
            { REREVVED_UNIT_TYPE_RIFLEMEN, 3, 5 },
            { REREVVED_UNIT_TYPE_MODERN_INFANTRY, 4, 8 },
            { REREVVED_UNIT_TYPE_HORSEMEN, 2, 1 },
            { REREVVED_UNIT_TYPE_KNIGHTS, 4, 2 },
            { REREVVED_UNIT_TYPE_TANK, 10, 6 },
            { REREVVED_UNIT_TYPE_PHALANX, 1, 3 },
            { REREVVED_UNIT_TYPE_CATAPULT, 4, 1 },
            { REREVVED_UNIT_TYPE_CANNON, 6, 2 },
            { REREVVED_UNIT_TYPE_ARTILLERY, 16, 2 },
            { REREVVED_UNIT_TYPE_SUBMARINE, 12, 2 },
            { REREVVED_UNIT_TYPE_GALLEY, 1, 1 },
            { REREVVED_UNIT_TYPE_GALLEON, 2, 2 },
            { REREVVED_UNIT_TYPE_CRUISER, 6, 6 },
            { REREVVED_UNIT_TYPE_BATTLESHIP, 12, 18 },
            { REREVVED_UNIT_TYPE_SPACE_STATION, 0, 3 },
            { REREVVED_UNIT_TYPE_BOMBER, 18, 3 },
            { REREVVED_UNIT_TYPE_FIGHTER, 6, 4 },
            { REREVVED_UNIT_TYPE_ICBM, 0, 0 },
            { REREVVED_UNIT_TYPE_SPY, 0, 0 },
        }
    };

struct IdentityFixture
{
    int32_t civilization;
    int32_t unit_type;
    int32_t identity;
};

constexpr std::array<IdentityFixture, 27> kIdentities = {
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

static_assert(kIdentities.size() == 27);

constexpr std::array<int32_t, REREVVED_CIVILIZATION_COUNT>
    kCivilizationIds = {
        REREVVED_CIVILIZATION_ROMAN,
        REREVVED_CIVILIZATION_EGYPTIAN,
        REREVVED_CIVILIZATION_GREEK,
        REREVVED_CIVILIZATION_SPANISH,
        REREVVED_CIVILIZATION_GERMAN,
        REREVVED_CIVILIZATION_RUSSIAN,
        REREVVED_CIVILIZATION_CHINESE,
        REREVVED_CIVILIZATION_AMERICAN,
        REREVVED_CIVILIZATION_JAPANESE,
        REREVVED_CIVILIZATION_FRENCH,
        REREVVED_CIVILIZATION_INDIAN,
        REREVVED_CIVILIZATION_ARABIAN,
        REREVVED_CIVILIZATION_AZTEC,
        REREVVED_CIVILIZATION_ZULU,
        REREVVED_CIVILIZATION_MONGOLIAN,
        REREVVED_CIVILIZATION_ENGLISH,
    };

constexpr std::array<int32_t, REREVVED_UNIT_TYPE_COUNT> kUnitTypeIds = {
    REREVVED_UNIT_TYPE_SETTLERS,
    REREVVED_UNIT_TYPE_FSETTLER,
    REREVVED_UNIT_TYPE_NAVAL_CREW,
    REREVVED_UNIT_TYPE_BARBARIAN_HOT,
    REREVVED_UNIT_TYPE_BARBARIAN_TEMPERATE,
    REREVVED_UNIT_TYPE_BARBARIAN_COLD,
    REREVVED_UNIT_TYPE_WARRIOR,
    REREVVED_UNIT_TYPE_MILITIA,
    REREVVED_UNIT_TYPE_LEGION,
    REREVVED_UNIT_TYPE_ARCHER,
    REREVVED_UNIT_TYPE_RIFLEMEN,
    REREVVED_UNIT_TYPE_MODERN_INFANTRY,
    REREVVED_UNIT_TYPE_HORSEMEN,
    REREVVED_UNIT_TYPE_KNIGHTS,
    REREVVED_UNIT_TYPE_TANK,
    REREVVED_UNIT_TYPE_PHALANX,
    REREVVED_UNIT_TYPE_CATAPULT,
    REREVVED_UNIT_TYPE_CANNON,
    REREVVED_UNIT_TYPE_ARTILLERY,
    REREVVED_UNIT_TYPE_SUBMARINE,
    REREVVED_UNIT_TYPE_GALLEY,
    REREVVED_UNIT_TYPE_GALLEON,
    REREVVED_UNIT_TYPE_CRUISER,
    REREVVED_UNIT_TYPE_BATTLESHIP,
    REREVVED_UNIT_TYPE_SPACE_STATION,
    REREVVED_UNIT_TYPE_BOMBER,
    REREVVED_UNIT_TYPE_FIGHTER,
    REREVVED_UNIT_TYPE_ICBM,
    REREVVED_UNIT_TYPE_SPY,
};

constexpr std::array<int32_t, REREVVED_UNIT_IDENTITY_COUNT> kIdentityIds = {
    REREVVED_UNIT_IDENTITY_BASE,
    REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
    REREVVED_UNIT_IDENTITY_IMPI_WARRIOR,
    REREVVED_UNIT_IDENTITY_ASHIGARU_PIKEMEN,
    REREVVED_UNIT_IDENTITY_HOPLITE,
    REREVVED_UNIT_IDENTITY_LONGBOW_ARCHER,
    REREVVED_UNIT_IDENTITY_CROSSBOW_ARCHER,
    REREVVED_UNIT_IDENTITY_TREBUCHET,
    REREVVED_UNIT_IDENTITY_COSSACK_HORSEMAN,
    REREVVED_UNIT_IDENTITY_SAMURAI_KNIGHT,
    REREVVED_UNIT_IDENTITY_CONQUISTADOR,
    REREVVED_UNIT_IDENTITY_PANZER_TANK,
    REREVVED_UNIT_IDENTITY_T34_TANK,
    REREVVED_UNIT_IDENTITY_SHERMAN_TANK,
    REREVVED_UNIT_IDENTITY_GERMAN_88MM_GUN,
    REREVVED_UNIT_IDENTITY_HOWITZER,
    REREVVED_UNIT_IDENTITY_ZERO_FIGHTER,
    REREVVED_UNIT_IDENTITY_MUSTANG_FIGHTER,
    REREVVED_UNIT_IDENTITY_SPITFIRE_FIGHTER,
    REREVVED_UNIT_IDENTITY_ME109_FIGHTER,
    REREVVED_UNIT_IDENTITY_VAL_BOMBER,
    REREVVED_UNIT_IDENTITY_FLYING_FORTRESS,
    REREVVED_UNIT_IDENTITY_LANCASTER_BOMBER,
    REREVVED_UNIT_IDENTITY_HEINKEL_BOMBER,
    REREVVED_UNIT_IDENTITY_TRIREME,
    REREVVED_UNIT_IDENTITY_CATAPHRACT,
    REREVVED_UNIT_IDENTITY_KESHIK,
};

constexpr int32_t ExpectedIdentity(int32_t civilization, int32_t unit_type)
{
    for (const auto& entry : kIdentities)
    {
        if (entry.civilization == civilization && entry.unit_type == unit_type)
        {
            return entry.identity;
        }
    }
    return REREVVED_UNIT_IDENTITY_BASE;
}

constexpr uint8_t kCanary = 0xA5;

struct GuardedOutput
{
    static constexpr size_t kPrefix = 4;
    alignas(4) std::array<uint8_t, 52> bytes{};

    GuardedOutput()
    {
        bytes.fill(kCanary);
    }

    void* Data()
    {
        return bytes.data() + kPrefix;
    }

    template <typename T>
    T Read(uint32_t out_size) const
    {
        T result{};
        std::memcpy(&result,
                    bytes.data() + kPrefix,
                    std::min<size_t>(out_size, sizeof(T)));
        return result;
    }

    bool OutsideCallerIsIntact(uint32_t out_size) const
    {
        const bool prefix_ok = std::all_of(
            bytes.begin(), bytes.begin() + kPrefix, [](uint8_t value)
            {
                return value == kCanary;
            });
        const bool suffix_ok = std::all_of(
            bytes.begin() + kPrefix + out_size,
            bytes.end(),
            [](uint8_t value)
            {
                return value == kCanary;
            });
        return prefix_ok && suffix_ok;
    }

    bool BytesAre(uint32_t begin, uint32_t end, uint8_t value) const
    {
        return std::all_of(bytes.begin() + kPrefix + begin,
                           bytes.begin() + kPrefix + end,
                           [value](uint8_t current)
                           {
                               return current == value;
                           });
    }
};

void TestLayoutsAndIds()
{
    static_assert(std::is_same_v<ReRevvedCivilizationId, int32_t>);
    static_assert(std::is_same_v<ReRevvedUnitTypeId, int32_t>);
    static_assert(std::is_same_v<ReRevvedUnitIdentityId, int32_t>);
    static_assert(std::is_same_v<ReRevvedUnitDisplayForm, int32_t>);

    static_assert(sizeof(ReRevvedGameplayState) == 80);
    static_assert(offsetof(ReRevvedGameplayState, struct_size) == 0);
    static_assert(offsetof(ReRevvedGameplayState, valid_fields) == 4);
    static_assert(offsetof(ReRevvedGameplayState, frame_sequence) == 8);
    static_assert(offsetof(ReRevvedGameplayState, gameplay_active) == 16);
    static_assert(offsetof(ReRevvedGameplayState, interface_update) == 20);
    static_assert(offsetof(ReRevvedGameplayState, active_player) == 24);
    static_assert(offsetof(ReRevvedGameplayState, human_player_mask) == 28);
    static_assert(offsetof(ReRevvedGameplayState, turn_owner_known) == 32);
    static_assert(offsetof(ReRevvedGameplayState, human_turn) == 36);
    static_assert(offsetof(ReRevvedGameplayState, available) == 40);
    static_assert(offsetof(ReRevvedGameplayState, civilization) == 44);
    static_assert(offsetof(ReRevvedGameplayState, era) == 48);
    static_assert(offsetof(ReRevvedGameplayState, year) == 52);
    static_assert(offsetof(ReRevvedGameplayState, turn) == 56);
    static_assert(offsetof(ReRevvedGameplayState, reserved) == 60);

    static_assert(sizeof(ReRevvedWorldUnitDefinition) == 32);
    static_assert(offsetof(ReRevvedWorldUnitDefinition, unit_type) == 4);
    static_assert(offsetof(ReRevvedWorldUnitDefinition, base_attack) == 8);
    static_assert(offsetof(ReRevvedWorldUnitDefinition, base_defense) == 12);
    static_assert(offsetof(ReRevvedWorldUnitDefinition, reserved) == 16);
    static_assert(sizeof(ReRevvedWorldUnitIdentity) == 32);
    static_assert(offsetof(ReRevvedWorldUnitIdentity, civilization) == 4);
    static_assert(offsetof(ReRevvedWorldUnitIdentity, base_unit_type) == 8);
    static_assert(offsetof(ReRevvedWorldUnitIdentity, identity) == 12);
    static_assert(offsetof(ReRevvedWorldUnitIdentity, display_form) == 16);
    static_assert(offsetof(ReRevvedWorldUnitIdentity, reserved) == 20);

    Require(REREVVED_CIVILIZATION_UNKNOWN == -1,
            "civilization unknown value");
    Require(REREVVED_UNIT_TYPE_UNKNOWN == -1, "unit type unknown value");
    for (size_t index = 0; index < kCivilizationIds.size(); ++index)
    {
        Require(kCivilizationIds[index] == static_cast<int32_t>(index),
                "civilization ID ordering");
    }
    for (size_t index = 0; index < kUnitTypeIds.size(); ++index)
    {
        Require(kUnitTypeIds[index] == static_cast<int32_t>(index),
                "unit type ID ordering");
    }
    for (size_t index = 0; index < kIdentityIds.size(); ++index)
    {
        Require(kIdentityIds[index] == static_cast<int32_t>(index),
                "unit identity ID ordering");
    }
}

void TestDefinitions()
{
    for (int32_t unit_type = 0; unit_type < REREVVED_UNIT_TYPE_COUNT;
         ++unit_type)
    {
        const auto&                 fixture = kDefinitions[unit_type];
        ReRevvedWorldUnitDefinition result{};
        Require(ReRevvedGetUnitDefinition(unit_type, &result, sizeof(result)) ==
                    REREVVED_WORLD_OK,
                "definition query succeeds");
        Require(result.struct_size == sizeof(result),
                "definition producer size");
        Require(fixture.unit_type == unit_type,
                "definition fixture covers each unit ID once");
        Require(result.unit_type == unit_type, "definition unit type");
        Require(result.base_attack == fixture.attack,
                "definition base attack");
        Require(result.base_defense == fixture.defense,
                "definition base defense");
        Require(std::ranges::all_of(result.reserved,
                                    [](int32_t value)
                                    {
                                        return value == 0;
                                    }),
                "definition reserved words");
    }

    ReRevvedWorldUnitDefinition knights{};
    Require(ReRevvedGetUnitDefinition(
                REREVVED_UNIT_TYPE_KNIGHTS, &knights, sizeof(knights)) ==
                    REREVVED_WORLD_OK &&
                knights.base_attack == 4,
            "first-party Knights base attack readback");
}

void TestResolverMatrix()
{
    int32_t non_base_count = 0;
    for (int32_t civilization = 0;
         civilization < REREVVED_CIVILIZATION_COUNT;
         ++civilization)
    {
        for (int32_t unit_type = 0; unit_type < REREVVED_UNIT_TYPE_COUNT;
             ++unit_type)
        {
            int32_t prior_identity = -1;
            for (int32_t display_form = REREVVED_UNIT_DISPLAY_FORM_UNIT;
                 display_form <= REREVVED_UNIT_DISPLAY_FORM_ARMY;
                 ++display_form)
            {
                ReRevvedWorldUnitIdentity result{};
                Require(ReRevvedResolveUnitIdentity(civilization,
                                                    unit_type,
                                                    display_form,
                                                    &result,
                                                    sizeof(result)) ==
                            REREVVED_WORLD_OK,
                        "identity resolver succeeds");
                Require(result.struct_size == sizeof(result),
                        "identity producer size");
                Require(result.civilization == civilization,
                        "identity civilization");
                Require(result.base_unit_type == unit_type,
                        "identity base unit type");
                Require(result.identity ==
                            ExpectedIdentity(civilization, unit_type),
                        "resolved identity");
                Require(result.display_form == display_form,
                        "identity display form");
                Require(std::ranges::all_of(
                            result.reserved,
                            [](int32_t value)
                            {
                                return value == 0;
                            }),
                        "identity reserved words");
                if (result.identity != REREVVED_UNIT_IDENTITY_BASE)
                {
                    ++non_base_count;
                }
                if (display_form == REREVVED_UNIT_DISPLAY_FORM_UNIT)
                {
                    prior_identity = result.identity;
                }
                else
                {
                    Require(result.identity == prior_identity,
                            "display form does not change identity");
                }
            }
        }
    }

    Require(non_base_count == 54, "resolver has 54 non-base outputs");
    Require(ExpectedIdentity(REREVVED_CIVILIZATION_SPANISH,
                             REREVVED_UNIT_TYPE_ARCHER) ==
                    REREVVED_UNIT_IDENTITY_CROSSBOW_ARCHER &&
                ExpectedIdentity(REREVVED_CIVILIZATION_CHINESE,
                                 REREVVED_UNIT_TYPE_ARCHER) ==
                    REREVVED_UNIT_IDENTITY_CROSSBOW_ARCHER,
            "Spanish and Chinese share Crossbow Archer");
}

void TestDefinitionBufferContract()
{
    for (uint32_t out_size = 0; out_size <= 40; ++out_size)
    {
        GuardedOutput output;
        const int32_t status = ReRevvedGetUnitDefinition(
            REREVVED_UNIT_TYPE_KNIGHTS,
            static_cast<ReRevvedWorldUnitDefinition*>(output.Data()),
            out_size);
        Require(status == (out_size < 16
                               ? REREVVED_WORLD_ERR_BUFFER_TOO_SMALL
                               : REREVVED_WORLD_OK),
                "definition prefix status");
        Require(output.OutsideCallerIsIntact(out_size),
                "definition caller canaries");
        if (out_size < 16)
        {
            Require(output.BytesAre(0, out_size, 0),
                    "small definition buffer cleared");
            continue;
        }

        const auto result = output.Read<ReRevvedWorldUnitDefinition>(out_size);
        Require(result.struct_size == 32 &&
                    result.unit_type == REREVVED_UNIT_TYPE_KNIGHTS &&
                    result.base_attack == 4 && result.base_defense == 2,
                "definition prefix fields");
        Require(output.BytesAre(16, std::min(out_size, uint32_t{ 32 }), 0),
                "definition partial reserved bytes are zero");
        if (out_size > 32)
        {
            Require(output.BytesAre(32, out_size, kCanary),
                    "definition leaves extended caller bytes untouched");
        }
    }
}

void TestIdentityBufferContract()
{
    for (uint32_t out_size = 0; out_size <= 40; ++out_size)
    {
        GuardedOutput output;
        const int32_t status = ReRevvedResolveUnitIdentity(
            REREVVED_CIVILIZATION_ROMAN,
            REREVVED_UNIT_TYPE_KNIGHTS,
            REREVVED_UNIT_DISPLAY_FORM_ARMY,
            static_cast<ReRevvedWorldUnitIdentity*>(output.Data()),
            out_size);
        Require(status == (out_size < 20
                               ? REREVVED_WORLD_ERR_BUFFER_TOO_SMALL
                               : REREVVED_WORLD_OK),
                "identity prefix status");
        Require(output.OutsideCallerIsIntact(out_size),
                "identity caller canaries");
        if (out_size < 20)
        {
            Require(output.BytesAre(0, out_size, 0),
                    "small identity buffer cleared");
            continue;
        }

        const auto result = output.Read<ReRevvedWorldUnitIdentity>(out_size);
        Require(result.struct_size == 32 &&
                    result.civilization == REREVVED_CIVILIZATION_ROMAN &&
                    result.base_unit_type == REREVVED_UNIT_TYPE_KNIGHTS &&
                    result.identity == REREVVED_UNIT_IDENTITY_CATAPHRACT &&
                    result.display_form == REREVVED_UNIT_DISPLAY_FORM_ARMY,
                "identity prefix fields");
        Require(output.BytesAre(20, std::min(out_size, uint32_t{ 32 }), 0),
                "identity partial reserved bytes are zero");
        if (out_size > 32)
        {
            Require(output.BytesAre(32, out_size, kCanary),
                    "identity leaves extended caller bytes untouched");
        }
    }
}

template <typename Call>
void RequireInvalidCallClears(Call call, std::string_view message)
{
    GuardedOutput output;
    const int32_t status = call(output);
    Require(status == REREVVED_WORLD_ERR_INVALID_ARGUMENT, message);
    Require(output.BytesAre(0, 32, 0), "invalid call clears producer bytes");
    Require(output.BytesAre(32, 40, kCanary),
            "invalid call leaves extended caller bytes untouched");
    Require(output.OutsideCallerIsIntact(40), "invalid call canaries");
}

void TestInvalidArguments()
{
    Require(ReRevvedGetUnitDefinition(REREVVED_UNIT_TYPE_KNIGHTS,
                                      nullptr,
                                      32) ==
                REREVVED_WORLD_ERR_INVALID_ARGUMENT,
            "definition null output");
    Require(ReRevvedResolveUnitIdentity(REREVVED_CIVILIZATION_ROMAN,
                                        REREVVED_UNIT_TYPE_KNIGHTS,
                                        REREVVED_UNIT_DISPLAY_FORM_UNIT,
                                        nullptr,
                                        32) ==
                REREVVED_WORLD_ERR_INVALID_ARGUMENT,
            "identity null output");

    GuardedOutput invalid_small_definition;
    Require(ReRevvedGetUnitDefinition(
                REREVVED_UNIT_TYPE_UNKNOWN,
                static_cast<ReRevvedWorldUnitDefinition*>(
                    invalid_small_definition.Data()),
                15) == REREVVED_WORLD_ERR_BUFFER_TOO_SMALL,
            "definition size validation precedes ID validation");
    Require(invalid_small_definition.BytesAre(0, 15, 0) &&
                invalid_small_definition.OutsideCallerIsIntact(15),
            "invalid small definition buffer is bounded and cleared");

    GuardedOutput invalid_small_identity;
    Require(ReRevvedResolveUnitIdentity(
                REREVVED_CIVILIZATION_UNKNOWN,
                REREVVED_UNIT_TYPE_UNKNOWN,
                -1,
                static_cast<ReRevvedWorldUnitIdentity*>(
                    invalid_small_identity.Data()),
                19) == REREVVED_WORLD_ERR_BUFFER_TOO_SMALL,
            "identity size validation precedes ID validation");
    Require(invalid_small_identity.BytesAre(0, 19, 0) &&
                invalid_small_identity.OutsideCallerIsIntact(19),
            "invalid small identity buffer is bounded and cleared");

    constexpr std::array<int32_t, 4> kInvalidUnitTypes = {
        -1,
        REREVVED_UNIT_TYPE_COUNT,
        REREVVED_UNIT_TYPE_COUNT + 1,
        std::numeric_limits<int32_t>::max(),
    };
    for (int32_t unit_type : kInvalidUnitTypes)
    {
        RequireInvalidCallClears(
            [unit_type](GuardedOutput& output)
            {
                return ReRevvedGetUnitDefinition(
                    unit_type,
                    static_cast<ReRevvedWorldUnitDefinition*>(output.Data()),
                    40);
            },
            "invalid definition unit type");
    }

    constexpr std::array<int32_t, 4> kInvalidCivilizations = {
        -1,
        REREVVED_CIVILIZATION_COUNT,
        REREVVED_CIVILIZATION_COUNT + 1,
        std::numeric_limits<int32_t>::max(),
    };
    for (int32_t civilization : kInvalidCivilizations)
    {
        RequireInvalidCallClears(
            [civilization](GuardedOutput& output)
            {
                return ReRevvedResolveUnitIdentity(
                    civilization,
                    REREVVED_UNIT_TYPE_KNIGHTS,
                    REREVVED_UNIT_DISPLAY_FORM_UNIT,
                    static_cast<ReRevvedWorldUnitIdentity*>(output.Data()),
                    40);
            },
            "invalid identity civilization");
    }
    for (int32_t unit_type : kInvalidUnitTypes)
    {
        RequireInvalidCallClears(
            [unit_type](GuardedOutput& output)
            {
                return ReRevvedResolveUnitIdentity(
                    REREVVED_CIVILIZATION_ROMAN,
                    unit_type,
                    REREVVED_UNIT_DISPLAY_FORM_UNIT,
                    static_cast<ReRevvedWorldUnitIdentity*>(output.Data()),
                    40);
            },
            "invalid identity unit type");
    }

    constexpr std::array<int32_t, 4> kInvalidForms = {
        -1,
        2,
        3,
        std::numeric_limits<int32_t>::max(),
    };
    for (int32_t display_form : kInvalidForms)
    {
        RequireInvalidCallClears(
            [display_form](GuardedOutput& output)
            {
                return ReRevvedResolveUnitIdentity(
                    REREVVED_CIVILIZATION_ROMAN,
                    REREVVED_UNIT_TYPE_KNIGHTS,
                    display_form,
                    static_cast<ReRevvedWorldUnitIdentity*>(output.Data()),
                    40);
            },
            "invalid identity display form");
    }
}

void TestEnlargedProducerContract()
{
    struct FutureOutput
    {
        uint32_t struct_size;
        int32_t  first;
        int32_t  second;
        int32_t  third;
        int32_t  added[5];
    };

    static_assert(sizeof(FutureOutput) == 36);

    const FutureOutput producer = {
        sizeof(FutureOutput),
        11,
        22,
        33,
        { 44, 55, 66, 77, 88 },
    };

    GuardedOutput prefix;
    Require(rerevved::world::CopySizedOutput(
                prefix.Data(), 16, &producer, sizeof(producer), 16) ==
                REREVVED_WORLD_OK,
            "enlarged producer accepts ABI 1 prefix");
    const auto prefix_result = prefix.Read<FutureOutput>(16);
    Require(prefix_result.struct_size == 36 && prefix_result.first == 11 &&
                prefix_result.second == 22 && prefix_result.third == 33,
            "enlarged producer retains prefix meanings");
    Require(prefix.OutsideCallerIsIntact(16),
            "enlarged producer prefix canaries");

    GuardedOutput partial_field;
    Require(rerevved::world::CopySizedOutput(partial_field.Data(),
                                             17,
                                             &producer,
                                             sizeof(producer),
                                             16) == REREVVED_WORLD_OK,
            "enlarged producer accepts partial trailing storage");
    Require(partial_field.BytesAre(16, 17, 0),
            "enlarged producer writes only complete fields");
    Require(partial_field.OutsideCallerIsIntact(17),
            "partial field caller canaries");

    GuardedOutput extended;
    Require(rerevved::world::CopySizedOutput(
                extended.Data(), 40, &producer, sizeof(producer), 16) ==
                REREVVED_WORLD_OK,
            "enlarged producer writes its full record");
    Require(extended.BytesAre(36, 40, kCanary),
            "enlarged producer leaves later bytes untouched");
    Require(extended.OutsideCallerIsIntact(40),
            "enlarged producer full canaries");
}

} // namespace

int main()
{
    Require(ReRevvedWorldAbiVersion() == REREVVED_WORLD_ABI_VERSION,
            "World ABI version");
    TestLayoutsAndIds();
    TestDefinitions();
    TestResolverMatrix();
    TestDefinitionBufferContract();
    TestIdentityBufferContract();
    TestInvalidArguments();
    TestEnlargedProducerContract();
    return failures == 0 ? 0 : 1;
}
