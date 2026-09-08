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
    UnitTypeId unitType;
    int32_t    attack;
    int32_t    defense;
};

constexpr std::array<DefinitionFixture, UNIT_TYPE_COUNT>
    kDefinitions = {
        {
            { UNIT_TYPE_SETTLERS, 0, 0 },
            { UNIT_TYPE_FSETTLER, 0, 0 },
            { UNIT_TYPE_NAVAL_CREW, 0, 1 },
            { UNIT_TYPE_BARBARIAN_HOT, 1, 1 },
            { UNIT_TYPE_BARBARIAN_TEMPERATE, 1, 1 },
            { UNIT_TYPE_BARBARIAN_COLD, 1, 1 },
            { UNIT_TYPE_WARRIOR, 1, 1 },
            { UNIT_TYPE_MILITIA, 0, 1 },
            { UNIT_TYPE_LEGION, 2, 1 },
            { UNIT_TYPE_ARCHER, 1, 2 },
            { UNIT_TYPE_RIFLEMEN, 3, 5 },
            { UNIT_TYPE_MODERN_INFANTRY, 4, 8 },
            { UNIT_TYPE_HORSEMEN, 2, 1 },
            { UNIT_TYPE_KNIGHTS, 4, 2 },
            { UNIT_TYPE_TANK, 10, 6 },
            { UNIT_TYPE_PHALANX, 1, 3 },
            { UNIT_TYPE_CATAPULT, 4, 1 },
            { UNIT_TYPE_CANNON, 6, 2 },
            { UNIT_TYPE_ARTILLERY, 16, 2 },
            { UNIT_TYPE_SUBMARINE, 12, 2 },
            { UNIT_TYPE_GALLEY, 1, 1 },
            { UNIT_TYPE_GALLEON, 2, 2 },
            { UNIT_TYPE_CRUISER, 6, 6 },
            { UNIT_TYPE_BATTLESHIP, 12, 18 },
            { UNIT_TYPE_SPACE_STATION, 0, 3 },
            { UNIT_TYPE_BOMBER, 18, 3 },
            { UNIT_TYPE_FIGHTER, 6, 4 },
            { UNIT_TYPE_ICBM, 0, 0 },
            { UNIT_TYPE_SPY, 0, 0 },
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

static_assert(kIdentities.size() == 27);

constexpr std::array<int32_t, CIVILIZATION_COUNT>
    kCivilizationIds = {
        CIVILIZATION_ROMAN,
        CIVILIZATION_EGYPTIAN,
        CIVILIZATION_GREEK,
        CIVILIZATION_SPANISH,
        CIVILIZATION_GERMAN,
        CIVILIZATION_RUSSIAN,
        CIVILIZATION_CHINESE,
        CIVILIZATION_AMERICAN,
        CIVILIZATION_JAPANESE,
        CIVILIZATION_FRENCH,
        CIVILIZATION_INDIAN,
        CIVILIZATION_ARABIAN,
        CIVILIZATION_AZTEC,
        CIVILIZATION_ZULU,
        CIVILIZATION_MONGOLIAN,
        CIVILIZATION_ENGLISH,
    };

constexpr std::array<int32_t, UNIT_TYPE_COUNT> kUnitTypeIds = {
    UNIT_TYPE_SETTLERS,
    UNIT_TYPE_FSETTLER,
    UNIT_TYPE_NAVAL_CREW,
    UNIT_TYPE_BARBARIAN_HOT,
    UNIT_TYPE_BARBARIAN_TEMPERATE,
    UNIT_TYPE_BARBARIAN_COLD,
    UNIT_TYPE_WARRIOR,
    UNIT_TYPE_MILITIA,
    UNIT_TYPE_LEGION,
    UNIT_TYPE_ARCHER,
    UNIT_TYPE_RIFLEMEN,
    UNIT_TYPE_MODERN_INFANTRY,
    UNIT_TYPE_HORSEMEN,
    UNIT_TYPE_KNIGHTS,
    UNIT_TYPE_TANK,
    UNIT_TYPE_PHALANX,
    UNIT_TYPE_CATAPULT,
    UNIT_TYPE_CANNON,
    UNIT_TYPE_ARTILLERY,
    UNIT_TYPE_SUBMARINE,
    UNIT_TYPE_GALLEY,
    UNIT_TYPE_GALLEON,
    UNIT_TYPE_CRUISER,
    UNIT_TYPE_BATTLESHIP,
    UNIT_TYPE_SPACE_STATION,
    UNIT_TYPE_BOMBER,
    UNIT_TYPE_FIGHTER,
    UNIT_TYPE_ICBM,
    UNIT_TYPE_SPY,
};

constexpr std::array<int32_t, UNIT_IDENTITY_COUNT> kIdentityIds = {
    UNIT_IDENTITY_BASE,
    UNIT_IDENTITY_JAGUAR_WARRIOR,
    UNIT_IDENTITY_IMPI_WARRIOR,
    UNIT_IDENTITY_ASHIGARU_PIKEMEN,
    UNIT_IDENTITY_HOPLITE,
    UNIT_IDENTITY_LONGBOW_ARCHER,
    UNIT_IDENTITY_CROSSBOW_ARCHER,
    UNIT_IDENTITY_TREBUCHET,
    UNIT_IDENTITY_COSSACK_HORSEMAN,
    UNIT_IDENTITY_SAMURAI_KNIGHT,
    UNIT_IDENTITY_CONQUISTADOR,
    UNIT_IDENTITY_PANZER_TANK,
    UNIT_IDENTITY_T34_TANK,
    UNIT_IDENTITY_SHERMAN_TANK,
    UNIT_IDENTITY_GERMAN_88MM_GUN,
    UNIT_IDENTITY_HOWITZER,
    UNIT_IDENTITY_ZERO_FIGHTER,
    UNIT_IDENTITY_MUSTANG_FIGHTER,
    UNIT_IDENTITY_SPITFIRE_FIGHTER,
    UNIT_IDENTITY_ME109_FIGHTER,
    UNIT_IDENTITY_VAL_BOMBER,
    UNIT_IDENTITY_FLYING_FORTRESS,
    UNIT_IDENTITY_LANCASTER_BOMBER,
    UNIT_IDENTITY_HEINKEL_BOMBER,
    UNIT_IDENTITY_TRIREME,
    UNIT_IDENTITY_CATAPHRACT,
    UNIT_IDENTITY_KESHIK,
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
    return UNIT_IDENTITY_BASE;
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
    static_assert(std::is_same_v<CivilizationId, int32_t>);
    static_assert(std::is_same_v<UnitTypeId, int32_t>);
    static_assert(std::is_same_v<UnitIdentityId, int32_t>);
    static_assert(std::is_same_v<UnitDisplayForm, int32_t>);

    static_assert(sizeof(GameplayState) == 80);
    static_assert(offsetof(GameplayState, structSize) == 0);
    static_assert(offsetof(GameplayState, validFields) == 4);
    static_assert(offsetof(GameplayState, frameSequence) == 8);
    static_assert(offsetof(GameplayState, gameplayActive) == 16);
    static_assert(offsetof(GameplayState, interfaceUpdate) == 20);
    static_assert(offsetof(GameplayState, activePlayer) == 24);
    static_assert(offsetof(GameplayState, humanPlayerMask) == 28);
    static_assert(offsetof(GameplayState, turnOwnerKnown) == 32);
    static_assert(offsetof(GameplayState, humanTurn) == 36);
    static_assert(offsetof(GameplayState, available) == 40);
    static_assert(offsetof(GameplayState, civilization) == 44);
    static_assert(offsetof(GameplayState, era) == 48);
    static_assert(offsetof(GameplayState, year) == 52);
    static_assert(offsetof(GameplayState, turn) == 56);
    static_assert(offsetof(GameplayState, reserved) == 60);

    static_assert(sizeof(UnitDefinition) == 32);
    static_assert(offsetof(UnitDefinition, unitType) == 4);
    static_assert(offsetof(UnitDefinition, baseAttack) == 8);
    static_assert(offsetof(UnitDefinition, baseDefense) == 12);
    static_assert(offsetof(UnitDefinition, reserved) == 16);
    static_assert(sizeof(UnitIdentity) == 32);
    static_assert(offsetof(UnitIdentity, civilization) == 4);
    static_assert(offsetof(UnitIdentity, baseUnitType) == 8);
    static_assert(offsetof(UnitIdentity, identity) == 12);
    static_assert(offsetof(UnitIdentity, displayForm) == 16);
    static_assert(offsetof(UnitIdentity, reserved) == 20);

    require(CIVILIZATION_UNKNOWN == -1,
            "civilization unknown value");
    require(UNIT_TYPE_UNKNOWN == -1, "unit type unknown value");
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
    for (int32_t unitType = 0; unitType < UNIT_TYPE_COUNT;
         ++unitType)
    {
        const auto&    fixture = kDefinitions[unitType];
        UnitDefinition result{};
        require(GetUnitDefinition(unitType, &result, sizeof(result)) ==
                    UNIT_CATALOG_OK,
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

    UnitDefinition knights{};
    require(GetUnitDefinition(
                UNIT_TYPE_KNIGHTS, &knights, sizeof(knights)) ==
                    UNIT_CATALOG_OK &&
                knights.baseAttack == 4,
            "first-party Knights base attack readback");
}

void TestResolverMatrix()
{
    int32_t nonBaseCount = 0;
    for (int32_t civilization = 0;
         civilization < CIVILIZATION_COUNT;
         ++civilization)
    {
        for (int32_t unitType = 0; unitType < UNIT_TYPE_COUNT;
             ++unitType)
        {
            int32_t priorIdentity = -1;
            for (int32_t displayForm = UNIT_DISPLAY_FORM_UNIT;
                 displayForm <= UNIT_DISPLAY_FORM_ARMY;
                 ++displayForm)
            {
                UnitIdentity result{};
                require(ResolveUnitIdentity(civilization,
                                            unitType,
                                            displayForm,
                                            &result,
                                            sizeof(result)) ==
                            UNIT_CATALOG_OK,
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
                if (result.identity != UNIT_IDENTITY_BASE)
                {
                    ++nonBaseCount;
                }
                if (displayForm == UNIT_DISPLAY_FORM_UNIT)
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
    require(expectedIdentity(CIVILIZATION_SPANISH,
                             UNIT_TYPE_ARCHER) ==
                    UNIT_IDENTITY_CROSSBOW_ARCHER &&
                expectedIdentity(CIVILIZATION_CHINESE,
                                 UNIT_TYPE_ARCHER) ==
                    UNIT_IDENTITY_CROSSBOW_ARCHER,
            "Spanish and Chinese share Crossbow Archer");
}

void TestDefinitionBufferContract()
{
    for (uint32_t outSize = 0; outSize <= 40; ++outSize)
    {
        GuardedOutput output;
        const int32_t status = GetUnitDefinition(
            UNIT_TYPE_KNIGHTS,
            static_cast<UnitDefinition*>(output.data()),
            outSize);
        require(status == (outSize < 16
                               ? UNIT_CATALOG_ERR_BUFFER_TOO_SMALL
                               : UNIT_CATALOG_OK),
                "definition prefix status");
        require(output.outsideCallerIsIntact(outSize),
                "definition caller canaries");
        if (outSize < 16)
        {
            require(output.bytesAre(0, outSize, 0),
                    "small definition buffer cleared");
            continue;
        }

        const auto result = output.read<UnitDefinition>(outSize);
        require(result.structSize == 32 &&
                    result.unitType == UNIT_TYPE_KNIGHTS &&
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
        const int32_t status = ResolveUnitIdentity(
            CIVILIZATION_ROMAN,
            UNIT_TYPE_KNIGHTS,
            UNIT_DISPLAY_FORM_ARMY,
            static_cast<UnitIdentity*>(output.data()),
            outSize);
        require(status == (outSize < 20
                               ? UNIT_CATALOG_ERR_BUFFER_TOO_SMALL
                               : UNIT_CATALOG_OK),
                "identity prefix status");
        require(output.outsideCallerIsIntact(outSize),
                "identity caller canaries");
        if (outSize < 20)
        {
            require(output.bytesAre(0, outSize, 0),
                    "small identity buffer cleared");
            continue;
        }

        const auto result = output.read<UnitIdentity>(outSize);
        require(result.structSize == 32 &&
                    result.civilization == CIVILIZATION_ROMAN &&
                    result.baseUnitType == UNIT_TYPE_KNIGHTS &&
                    result.identity == UNIT_IDENTITY_CATAPHRACT &&
                    result.displayForm == UNIT_DISPLAY_FORM_ARMY,
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
    require(status == UNIT_CATALOG_ERR_INVALID_ARGUMENT, message);
    require(output.bytesAre(0, 32, 0), "invalid call clears producer bytes");
    require(output.bytesAre(32, 40, kCanary),
            "invalid call leaves extended caller bytes untouched");
    require(output.outsideCallerIsIntact(40), "invalid call canaries");
}

void TestInvalidArguments()
{
    require(GetUnitDefinition(UNIT_TYPE_KNIGHTS,
                              nullptr,
                              32) ==
                UNIT_CATALOG_ERR_INVALID_ARGUMENT,
            "definition null output");
    require(ResolveUnitIdentity(CIVILIZATION_ROMAN,
                                UNIT_TYPE_KNIGHTS,
                                UNIT_DISPLAY_FORM_UNIT,
                                nullptr,
                                32) ==
                UNIT_CATALOG_ERR_INVALID_ARGUMENT,
            "identity null output");

    GuardedOutput invalidSmallDefinition;
    require(GetUnitDefinition(
                UNIT_TYPE_UNKNOWN,
                static_cast<UnitDefinition*>(
                    invalidSmallDefinition.data()),
                15) == UNIT_CATALOG_ERR_BUFFER_TOO_SMALL,
            "definition size validation precedes ID validation");
    require(invalidSmallDefinition.bytesAre(0, 15, 0) &&
                invalidSmallDefinition.outsideCallerIsIntact(15),
            "invalid small definition buffer is bounded and cleared");

    GuardedOutput invalidSmallIdentity;
    require(ResolveUnitIdentity(
                CIVILIZATION_UNKNOWN,
                UNIT_TYPE_UNKNOWN,
                -1,
                static_cast<UnitIdentity*>(
                    invalidSmallIdentity.data()),
                19) == UNIT_CATALOG_ERR_BUFFER_TOO_SMALL,
            "identity size validation precedes ID validation");
    require(invalidSmallIdentity.bytesAre(0, 19, 0) &&
                invalidSmallIdentity.outsideCallerIsIntact(19),
            "invalid small identity buffer is bounded and cleared");

    constexpr std::array<int32_t, 4> kInvalidUnitTypes = {
        -1,
        UNIT_TYPE_COUNT,
        UNIT_TYPE_COUNT + 1,
        std::numeric_limits<int32_t>::max(),
    };
    for (int32_t unitType : kInvalidUnitTypes)
    {
        requireInvalidCallClears(
            [unitType](GuardedOutput& output)
            {
                return GetUnitDefinition(
                    unitType,
                    static_cast<UnitDefinition*>(output.data()),
                    40);
            },
            "invalid definition unit type");
    }

    constexpr std::array<int32_t, 4> kInvalidCivilizations = {
        -1,
        CIVILIZATION_COUNT,
        CIVILIZATION_COUNT + 1,
        std::numeric_limits<int32_t>::max(),
    };
    for (int32_t civilization : kInvalidCivilizations)
    {
        requireInvalidCallClears(
            [civilization](GuardedOutput& output)
            {
                return ResolveUnitIdentity(
                    civilization,
                    UNIT_TYPE_KNIGHTS,
                    UNIT_DISPLAY_FORM_UNIT,
                    static_cast<UnitIdentity*>(output.data()),
                    40);
            },
            "invalid identity civilization");
    }
    for (int32_t unitType : kInvalidUnitTypes)
    {
        requireInvalidCallClears(
            [unitType](GuardedOutput& output)
            {
                return ResolveUnitIdentity(
                    CIVILIZATION_ROMAN,
                    unitType,
                    UNIT_DISPLAY_FORM_UNIT,
                    static_cast<UnitIdentity*>(output.data()),
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
                return ResolveUnitIdentity(
                    CIVILIZATION_ROMAN,
                    UNIT_TYPE_KNIGHTS,
                    displayForm,
                    static_cast<UnitIdentity*>(output.data()),
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
                UNIT_CATALOG_OK,
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
                                                    16) == UNIT_CATALOG_OK,
            "enlarged producer accepts partial trailing storage");
    require(partialField.bytesAre(16, 17, 0),
            "enlarged producer writes only complete fields");
    require(partialField.outsideCallerIsIntact(17),
            "partial field caller canaries");

    GuardedOutput extended;
    require(rerevved::unit_catalog::CopySizedOutput(
                extended.data(), 40, &producer, sizeof(producer), 16) ==
                UNIT_CATALOG_OK,
            "enlarged producer writes its full record");
    require(extended.bytesAre(36, 40, kCanary),
            "enlarged producer leaves later bytes untouched");
    require(extended.outsideCallerIsIntact(40),
            "enlarged producer full canaries");
}

} // namespace

int main()
{
    require(UnitCatalogAbiVersion() == UNIT_CATALOG_ABI_VERSION,
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
