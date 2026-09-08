#include "unit_catalog_api.h"

#include <gameplay_state.h>
#include <unit_catalog.h>

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

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

struct DefinitionFixture
{
    ReRevvedUnitTypeId unitType;
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
    int32_t unitType;
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

constexpr int32_t expectedIdentity(int32_t civilization, int32_t unitType)
{
    for (const auto& entry : kIdentities)
    {
        if (entry.civilization == civilization && entry.unitType == unitType)
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

    void* data()
    {
        return bytes.data() + kPrefix;
    }

    template <typename T>
    T read(uint32_t outSize) const
    {
        T result{};
        std::memcpy(&result,
                    bytes.data() + kPrefix,
                    std::min<size_t>(outSize, sizeof(T)));
        return result;
    }

    bool outsideCallerIsIntact(uint32_t outSize) const
    {
        const bool prefixOk = std::all_of(
            bytes.begin(), bytes.begin() + kPrefix, [](uint8_t value)
            {
                return value == kCanary;
            });
        const bool suffixOk = std::all_of(
            bytes.begin() + kPrefix + outSize,
            bytes.end(),
            [](uint8_t value)
            {
                return value == kCanary;
            });
        return prefixOk && suffixOk;
    }

    bool bytesAre(uint32_t begin, uint32_t end, uint8_t value) const
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
    static_assert(offsetof(ReRevvedGameplayState, structSize) == 0);
    static_assert(offsetof(ReRevvedGameplayState, validFields) == 4);
    static_assert(offsetof(ReRevvedGameplayState, frameSequence) == 8);
    static_assert(offsetof(ReRevvedGameplayState, gameplayActive) == 16);
    static_assert(offsetof(ReRevvedGameplayState, interfaceUpdate) == 20);
    static_assert(offsetof(ReRevvedGameplayState, activePlayer) == 24);
    static_assert(offsetof(ReRevvedGameplayState, humanPlayerMask) == 28);
    static_assert(offsetof(ReRevvedGameplayState, turnOwnerKnown) == 32);
    static_assert(offsetof(ReRevvedGameplayState, humanTurn) == 36);
    static_assert(offsetof(ReRevvedGameplayState, available) == 40);
    static_assert(offsetof(ReRevvedGameplayState, civilization) == 44);
    static_assert(offsetof(ReRevvedGameplayState, era) == 48);
    static_assert(offsetof(ReRevvedGameplayState, year) == 52);
    static_assert(offsetof(ReRevvedGameplayState, turn) == 56);
    static_assert(offsetof(ReRevvedGameplayState, reserved) == 60);

    static_assert(sizeof(ReRevvedUnitDefinition) == 32);
    static_assert(offsetof(ReRevvedUnitDefinition, unitType) == 4);
    static_assert(offsetof(ReRevvedUnitDefinition, baseAttack) == 8);
    static_assert(offsetof(ReRevvedUnitDefinition, baseDefense) == 12);
    static_assert(offsetof(ReRevvedUnitDefinition, reserved) == 16);
    static_assert(sizeof(ReRevvedUnitIdentity) == 32);
    static_assert(offsetof(ReRevvedUnitIdentity, civilization) == 4);
    static_assert(offsetof(ReRevvedUnitIdentity, baseUnitType) == 8);
    static_assert(offsetof(ReRevvedUnitIdentity, identity) == 12);
    static_assert(offsetof(ReRevvedUnitIdentity, displayForm) == 16);
    static_assert(offsetof(ReRevvedUnitIdentity, reserved) == 20);

    require(REREVVED_CIVILIZATION_UNKNOWN == -1,
            "civilization unknown value");
    require(REREVVED_UNIT_TYPE_UNKNOWN == -1, "unit type unknown value");
    for (size_t index = 0; index < kCivilizationIds.size(); ++index)
    {
        require(kCivilizationIds[index] == static_cast<int32_t>(index),
                "civilization ID ordering");
    }
    for (size_t index = 0; index < kUnitTypeIds.size(); ++index)
    {
        require(kUnitTypeIds[index] == static_cast<int32_t>(index),
                "unit type ID ordering");
    }
    for (size_t index = 0; index < kIdentityIds.size(); ++index)
    {
        require(kIdentityIds[index] == static_cast<int32_t>(index),
                "unit identity ID ordering");
    }
}

void TestDefinitions()
{
    for (int32_t unitType = 0; unitType < REREVVED_UNIT_TYPE_COUNT;
         ++unitType)
    {
        const auto&            fixture = kDefinitions[unitType];
        ReRevvedUnitDefinition result{};
        require(ReRevvedGetUnitDefinition(unitType, &result, sizeof(result)) ==
                    REREVVED_UNIT_CATALOG_OK,
                "definition query succeeds");
        require(result.structSize == sizeof(result),
                "definition producer size");
        require(fixture.unitType == unitType,
                "definition fixture covers each unit ID once");
        require(result.unitType == unitType, "definition unit type");
        require(result.baseAttack == fixture.attack,
                "definition base attack");
        require(result.baseDefense == fixture.defense,
                "definition base defense");
        require(std::ranges::all_of(result.reserved,
                                    [](int32_t value)
                                    {
                                        return value == 0;
                                    }),
                "definition reserved words");
    }

    ReRevvedUnitDefinition knights{};
    require(ReRevvedGetUnitDefinition(
                REREVVED_UNIT_TYPE_KNIGHTS, &knights, sizeof(knights)) ==
                    REREVVED_UNIT_CATALOG_OK &&
                knights.baseAttack == 4,
            "first-party Knights base attack readback");
}

void TestResolverMatrix()
{
    int32_t nonBaseCount = 0;
    for (int32_t civilization = 0;
         civilization < REREVVED_CIVILIZATION_COUNT;
         ++civilization)
    {
        for (int32_t unitType = 0; unitType < REREVVED_UNIT_TYPE_COUNT;
             ++unitType)
        {
            int32_t priorIdentity = -1;
            for (int32_t displayForm = REREVVED_UNIT_DISPLAY_FORM_UNIT;
                 displayForm <= REREVVED_UNIT_DISPLAY_FORM_ARMY;
                 ++displayForm)
            {
                ReRevvedUnitIdentity result{};
                require(ReRevvedResolveUnitIdentity(civilization,
                                                    unitType,
                                                    displayForm,
                                                    &result,
                                                    sizeof(result)) ==
                            REREVVED_UNIT_CATALOG_OK,
                        "identity resolver succeeds");
                require(result.structSize == sizeof(result),
                        "identity producer size");
                require(result.civilization == civilization,
                        "identity civilization");
                require(result.baseUnitType == unitType,
                        "identity base unit type");
                require(result.identity ==
                            expectedIdentity(civilization, unitType),
                        "resolved identity");
                require(result.displayForm == displayForm,
                        "identity display form");
                require(std::ranges::all_of(
                            result.reserved,
                            [](int32_t value)
                            {
                                return value == 0;
                            }),
                        "identity reserved words");
                if (result.identity != REREVVED_UNIT_IDENTITY_BASE)
                {
                    ++nonBaseCount;
                }
                if (displayForm == REREVVED_UNIT_DISPLAY_FORM_UNIT)
                {
                    priorIdentity = result.identity;
                }
                else
                {
                    require(result.identity == priorIdentity,
                            "display form does not change identity");
                }
            }
        }
    }

    require(nonBaseCount == 54, "resolver has 54 non-base outputs");
    require(expectedIdentity(REREVVED_CIVILIZATION_SPANISH,
                             REREVVED_UNIT_TYPE_ARCHER) ==
                    REREVVED_UNIT_IDENTITY_CROSSBOW_ARCHER &&
                expectedIdentity(REREVVED_CIVILIZATION_CHINESE,
                                 REREVVED_UNIT_TYPE_ARCHER) ==
                    REREVVED_UNIT_IDENTITY_CROSSBOW_ARCHER,
            "Spanish and Chinese share Crossbow Archer");
}

void TestDefinitionBufferContract()
{
    for (uint32_t outSize = 0; outSize <= 40; ++outSize)
    {
        GuardedOutput output;
        const int32_t status = ReRevvedGetUnitDefinition(
            REREVVED_UNIT_TYPE_KNIGHTS,
            static_cast<ReRevvedUnitDefinition*>(output.data()),
            outSize);
        require(status == (outSize < 16
                               ? REREVVED_UNIT_CATALOG_ERR_BUFFER_TOO_SMALL
                               : REREVVED_UNIT_CATALOG_OK),
                "definition prefix status");
        require(output.outsideCallerIsIntact(outSize),
                "definition caller canaries");
        if (outSize < 16)
        {
            require(output.bytesAre(0, outSize, 0),
                    "small definition buffer cleared");
            continue;
        }

        const auto result = output.read<ReRevvedUnitDefinition>(outSize);
        require(result.structSize == 32 &&
                    result.unitType == REREVVED_UNIT_TYPE_KNIGHTS &&
                    result.baseAttack == 4 && result.baseDefense == 2,
                "definition prefix fields");
        require(output.bytesAre(16, std::min(outSize, uint32_t{ 32 }), 0),
                "definition partial reserved bytes are zero");
        if (outSize > 32)
        {
            require(output.bytesAre(32, outSize, kCanary),
                    "definition leaves extended caller bytes untouched");
        }
    }
}

void TestIdentityBufferContract()
{
    for (uint32_t outSize = 0; outSize <= 40; ++outSize)
    {
        GuardedOutput output;
        const int32_t status = ReRevvedResolveUnitIdentity(
            REREVVED_CIVILIZATION_ROMAN,
            REREVVED_UNIT_TYPE_KNIGHTS,
            REREVVED_UNIT_DISPLAY_FORM_ARMY,
            static_cast<ReRevvedUnitIdentity*>(output.data()),
            outSize);
        require(status == (outSize < 20
                               ? REREVVED_UNIT_CATALOG_ERR_BUFFER_TOO_SMALL
                               : REREVVED_UNIT_CATALOG_OK),
                "identity prefix status");
        require(output.outsideCallerIsIntact(outSize),
                "identity caller canaries");
        if (outSize < 20)
        {
            require(output.bytesAre(0, outSize, 0),
                    "small identity buffer cleared");
            continue;
        }

        const auto result = output.read<ReRevvedUnitIdentity>(outSize);
        require(result.structSize == 32 &&
                    result.civilization == REREVVED_CIVILIZATION_ROMAN &&
                    result.baseUnitType == REREVVED_UNIT_TYPE_KNIGHTS &&
                    result.identity == REREVVED_UNIT_IDENTITY_CATAPHRACT &&
                    result.displayForm == REREVVED_UNIT_DISPLAY_FORM_ARMY,
                "identity prefix fields");
        require(output.bytesAre(20, std::min(outSize, uint32_t{ 32 }), 0),
                "identity partial reserved bytes are zero");
        if (outSize > 32)
        {
            require(output.bytesAre(32, outSize, kCanary),
                    "identity leaves extended caller bytes untouched");
        }
    }
}

template <typename Call>
void requireInvalidCallClears(Call call, std::string_view message)
{
    GuardedOutput output;
    const int32_t status = call(output);
    require(status == REREVVED_UNIT_CATALOG_ERR_INVALID_ARGUMENT, message);
    require(output.bytesAre(0, 32, 0), "invalid call clears producer bytes");
    require(output.bytesAre(32, 40, kCanary),
            "invalid call leaves extended caller bytes untouched");
    require(output.outsideCallerIsIntact(40), "invalid call canaries");
}

void TestInvalidArguments()
{
    require(ReRevvedGetUnitDefinition(REREVVED_UNIT_TYPE_KNIGHTS,
                                      nullptr,
                                      32) ==
                REREVVED_UNIT_CATALOG_ERR_INVALID_ARGUMENT,
            "definition null output");
    require(ReRevvedResolveUnitIdentity(REREVVED_CIVILIZATION_ROMAN,
                                        REREVVED_UNIT_TYPE_KNIGHTS,
                                        REREVVED_UNIT_DISPLAY_FORM_UNIT,
                                        nullptr,
                                        32) ==
                REREVVED_UNIT_CATALOG_ERR_INVALID_ARGUMENT,
            "identity null output");

    GuardedOutput invalidSmallDefinition;
    require(ReRevvedGetUnitDefinition(
                REREVVED_UNIT_TYPE_UNKNOWN,
                static_cast<ReRevvedUnitDefinition*>(
                    invalidSmallDefinition.data()),
                15) == REREVVED_UNIT_CATALOG_ERR_BUFFER_TOO_SMALL,
            "definition size validation precedes ID validation");
    require(invalidSmallDefinition.bytesAre(0, 15, 0) &&
                invalidSmallDefinition.outsideCallerIsIntact(15),
            "invalid small definition buffer is bounded and cleared");

    GuardedOutput invalidSmallIdentity;
    require(ReRevvedResolveUnitIdentity(
                REREVVED_CIVILIZATION_UNKNOWN,
                REREVVED_UNIT_TYPE_UNKNOWN,
                -1,
                static_cast<ReRevvedUnitIdentity*>(
                    invalidSmallIdentity.data()),
                19) == REREVVED_UNIT_CATALOG_ERR_BUFFER_TOO_SMALL,
            "identity size validation precedes ID validation");
    require(invalidSmallIdentity.bytesAre(0, 19, 0) &&
                invalidSmallIdentity.outsideCallerIsIntact(19),
            "invalid small identity buffer is bounded and cleared");

    constexpr std::array<int32_t, 4> kInvalidUnitTypes = {
        -1,
        REREVVED_UNIT_TYPE_COUNT,
        REREVVED_UNIT_TYPE_COUNT + 1,
        std::numeric_limits<int32_t>::max(),
    };
    for (int32_t unitType : kInvalidUnitTypes)
    {
        requireInvalidCallClears(
            [unitType](GuardedOutput& output)
            {
                return ReRevvedGetUnitDefinition(
                    unitType,
                    static_cast<ReRevvedUnitDefinition*>(output.data()),
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
        requireInvalidCallClears(
            [civilization](GuardedOutput& output)
            {
                return ReRevvedResolveUnitIdentity(
                    civilization,
                    REREVVED_UNIT_TYPE_KNIGHTS,
                    REREVVED_UNIT_DISPLAY_FORM_UNIT,
                    static_cast<ReRevvedUnitIdentity*>(output.data()),
                    40);
            },
            "invalid identity civilization");
    }
    for (int32_t unitType : kInvalidUnitTypes)
    {
        requireInvalidCallClears(
            [unitType](GuardedOutput& output)
            {
                return ReRevvedResolveUnitIdentity(
                    REREVVED_CIVILIZATION_ROMAN,
                    unitType,
                    REREVVED_UNIT_DISPLAY_FORM_UNIT,
                    static_cast<ReRevvedUnitIdentity*>(output.data()),
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
    for (int32_t displayForm : kInvalidForms)
    {
        requireInvalidCallClears(
            [displayForm](GuardedOutput& output)
            {
                return ReRevvedResolveUnitIdentity(
                    REREVVED_CIVILIZATION_ROMAN,
                    REREVVED_UNIT_TYPE_KNIGHTS,
                    displayForm,
                    static_cast<ReRevvedUnitIdentity*>(output.data()),
                    40);
            },
            "invalid identity display form");
    }
}

void TestEnlargedProducerContract()
{
    struct FutureOutput
    {
        uint32_t structSize;
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
    require(rerevved::unit_catalog::CopySizedOutput(
                prefix.data(), 16, &producer, sizeof(producer), 16) ==
                REREVVED_UNIT_CATALOG_OK,
            "enlarged producer accepts ABI 1 prefix");
    const auto prefixResult = prefix.read<FutureOutput>(16);
    require(prefixResult.structSize == 36 && prefixResult.first == 11 &&
                prefixResult.second == 22 && prefixResult.third == 33,
            "enlarged producer retains prefix meanings");
    require(prefix.outsideCallerIsIntact(16),
            "enlarged producer prefix canaries");

    GuardedOutput partialField;
    require(rerevved::unit_catalog::CopySizedOutput(partialField.data(),
                                                    17,
                                                    &producer,
                                                    sizeof(producer),
                                                    16) == REREVVED_UNIT_CATALOG_OK,
            "enlarged producer accepts partial trailing storage");
    require(partialField.bytesAre(16, 17, 0),
            "enlarged producer writes only complete fields");
    require(partialField.outsideCallerIsIntact(17),
            "partial field caller canaries");

    GuardedOutput extended;
    require(rerevved::unit_catalog::CopySizedOutput(
                extended.data(), 40, &producer, sizeof(producer), 16) ==
                REREVVED_UNIT_CATALOG_OK,
            "enlarged producer writes its full record");
    require(extended.bytesAre(36, 40, kCanary),
            "enlarged producer leaves later bytes untouched");
    require(extended.outsideCallerIsIntact(40),
            "enlarged producer full canaries");
}

} // namespace

int main()
{
    require(ReRevvedUnitCatalogAbiVersion() == REREVVED_UNIT_CATALOG_ABI_VERSION,
            "Unit Catalog ABI version");
    TestLayoutsAndIds();
    TestDefinitions();
    TestResolverMatrix();
    TestDefinitionBufferContract();
    TestIdentityBufferContract();
    TestInvalidArguments();
    TestEnlargedProducerContract();
    return failures == 0 ? 0 : 1;
}
