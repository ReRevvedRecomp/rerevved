#include "nation_select_text_registry.h"

#include <array>
#include <cstdint>
#include <cstring>

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "unique_era_abilities_registry.h"
#include "unit_catalog_api.h"

namespace
{

constexpr uint32_t kUniqueEraAbilityTable = 0x82F6F950;
constexpr size_t   kEraBlockCapacity      = 512;

thread_local uint32_t eraTextBuffer               = 0;
thread_local uint32_t leaderTextBuffer            = 0;
thread_local uint32_t civilizationTextBuffer      = 0;
thread_local uint32_t traitTextBuffer             = 0;
thread_local uint32_t uniqueUnitHeadingTextBuffer = 0;
thread_local uint32_t unitTextBuffer              = 0;

uint32_t readBigEndianU32(const uint8_t* value)
{
    return (uint32_t{ value[0] } << 24) | (uint32_t{ value[1] } << 16) |
           (uint32_t{ value[2] } << 8) | uint32_t{ value[3] };
}

void writeBigEndianU32(uint8_t* destination, uint32_t value)
{
    destination[0] = static_cast<uint8_t>(value >> 24);
    destination[1] = static_cast<uint8_t>(value >> 16);
    destination[2] = static_cast<uint8_t>(value >> 8);
    destination[3] = static_cast<uint8_t>(value);
}

bool tryReadGuestU32(uint32_t address, uint32_t& value)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory)
    {
        return false;
    }
    value = readBigEndianU32(
        memory->TranslateVirtual<const uint8_t*>(address));
    return true;
}

bool tryPublishText(const char*  text,
                    size_t       capacity,
                    bool         includeLengthHeader,
                    uint32_t&    guestBuffer,
                    PPCRegister& out)
{
    const size_t length = std::strlen(text);
    if (length >= capacity)
    {
        return false;
    }

    auto* memory = REX_KERNEL_MEMORY();
    if (!memory)
    {
        return false;
    }
    if (guestBuffer == 0)
    {
        const size_t allocationSize =
            capacity + (includeLengthHeader ? sizeof(uint32_t) : 0u);
        guestBuffer =
            memory->SystemHeapAlloc(static_cast<uint32_t>(allocationSize));
        if (guestBuffer == 0)
        {
            return false;
        }
    }
    const uint32_t textAddress =
        guestBuffer + (includeLengthHeader ? sizeof(uint32_t) : 0u);
    if (includeLengthHeader)
    {
        writeBigEndianU32(memory->TranslateVirtual<uint8_t*>(guestBuffer),
                          static_cast<uint32_t>(length));
    }
    auto* destination = memory->TranslateVirtual<char*>(textAddress);
    std::memcpy(destination, text, length + 1);
    out.u64 = textAddress;
    return true;
}

bool tryEvaluateText(NationSelectTextSurface     surface,
                     CivilizationId              civilization,
                     UnlockEra                   unlockEra,
                     NationSelectTextEvaluation& presentation)
{
    const NationSelectTextQuery query = {
        sizeof(NationSelectTextQuery),
        surface,
        civilization,
        unlockEra,
        NATION_SELECT_TEXT_SELECTOR_UNUSED,
        NATION_SELECT_TEXT_SELECTOR_UNUSED,
        NATION_SELECT_TEXT_SELECTOR_UNUSED,
        NATION_SELECT_TEXT_SELECTOR_UNUSED,
        {},
    };
    return rerevved::nation_select_text::TryEvaluate(query, presentation) &&
           (presentation.statusFlags &
            NATION_SELECT_TEXT_EVALUATION_REPLACED) != 0;
}

bool tryReplaceText(PPCRegister&            localizedText,
                    CivilizationId          civilization,
                    NationSelectTextSurface surface,
                    bool                    includeLengthHeader,
                    uint32_t&               guestBuffer)
{
    NationSelectTextEvaluation presentation{};
    if (!tryEvaluateText(surface,
                         civilization,
                         NATION_SELECT_TEXT_SELECTOR_UNUSED,
                         presentation))
    {
        return false;
    }
    return tryPublishText(presentation.text,
                          NATION_SELECT_TEXT_CAPACITY,
                          includeLengthHeader,
                          guestBuffer,
                          localizedText);
}

bool tryEvaluateEraText(CivilizationId              civilization,
                        UnlockEra                   era,
                        NationSelectTextEvaluation& presentation)
{
    uint32_t       nativeBits = 0;
    const uint32_t nativeAddress =
        kUniqueEraAbilityTable + static_cast<uint32_t>(civilization) * 16u +
        static_cast<uint32_t>(era) * sizeof(uint32_t);
    if (!tryReadGuestU32(nativeAddress, nativeBits))
    {
        return false;
    }

    EraAbilityCellEvaluation ability{};
    if (!rerevved::unique_era_abilities::TryEvaluate(
            civilization,
            era,
            static_cast<EraAbilityId>(nativeBits),
            ability) ||
        (ability.statusFlags &
         ERA_ABILITY_EVALUATION_REPLACEMENT_CONFLICT) != 0)
    {
        return false;
    }

    const NationSelectTextQuery query = {
        sizeof(NationSelectTextQuery),
        NATION_SELECT_TEXT_SURFACE_ERA_ABILITY,
        civilization,
        era,
        ability.effectiveAbility,
        NATION_SELECT_TEXT_SELECTOR_UNUSED,
        NATION_SELECT_TEXT_SELECTOR_UNUSED,
        NATION_SELECT_TEXT_SELECTOR_UNUSED,
        {},
    };
    return rerevved::nation_select_text::TryEvaluate(query, presentation) &&
           (presentation.statusFlags &
            NATION_SELECT_TEXT_EVALUATION_REPLACED) != 0;
}

bool tryReplaceEraLines(const char*                          nativeText,
                        CivilizationId                       civilization,
                        std::array<char, kEraBlockCapacity>& output)
{
    std::array<const char*, 9> starts{};
    std::array<size_t, 9>      lengths{};
    const char*                current = nativeText;
    for (size_t index = 0; index < starts.size(); ++index)
    {
        starts[index]   = current;
        const char* end = std::strchr(current, '\n');
        if (!end)
        {
            if (index != starts.size() - 1)
            {
                return false;
            }
            lengths[index] = std::strlen(current);
            current += lengths[index];
        }
        else
        {
            lengths[index] = static_cast<size_t>(end - current);
            current        = end + 1;
        }
    }
    if (*current != '\0')
    {
        return false;
    }

    size_t used      = 0;
    bool   anyChange = false;
    for (size_t index = 0; index < starts.size(); ++index)
    {
        const char*                replacement = starts[index];
        size_t                     length      = lengths[index];
        NationSelectTextEvaluation presentation{};
        bool                       replaced = false;
        if (index != 0 && (index & 1u) != 0)
        {
            replaced = tryEvaluateText(
                NATION_SELECT_TEXT_SURFACE_ERA_HEADING,
                NATION_SELECT_TEXT_SELECTOR_UNUSED,
                static_cast<UnlockEra>((index - 1) / 2),
                presentation);
        }
        else if (index != 0 &&
                 tryEvaluateEraText(
                     civilization,
                     static_cast<UnlockEra>((index - 2) / 2),
                     presentation))
        {
            replaced = true;
        }
        if (replaced)
        {
            replacement = presentation.text;
            length      = std::strlen(replacement);
            anyChange   = true;
        }
        const size_t separator = index + 1 < starts.size() ? 1u : 0u;
        if (used + length + separator >= output.size())
        {
            return false;
        }
        std::memcpy(output.data() + used, replacement, length);
        used += length;
        if (separator != 0)
        {
            output[used++] = '\n';
        }
    }
    output[used] = '\0';
    return anyChange;
}

} // namespace

// The civilization-selection builder stores one heading followed by four
// label/value pairs. Global headings and civilization-specific values each
// replace only when exactly one matching rule is registered.
void ReRevvedApplyEraAbilityNationSelectText(PPCRegister& eraBlock,
                                             PPCRegister& civilization)
{
    if (civilization.s32 < 0 ||
        civilization.s32 >= CIVILIZATION_COUNT ||
        eraBlock.u32 == 0)
    {
        return;
    }

    const auto* nativeText =
        REX_KERNEL_MEMORY()->TranslateVirtual<const char*>(eraBlock.u32);
    std::array<char, kEraBlockCapacity> replacement{};
    if (tryReplaceEraLines(nativeText, civilization.s32, replacement))
    {
        tryPublishText(replacement.data(),
                       replacement.size(),
                       true,
                       eraTextBuffer,
                       eraBlock);
    }
}

void ReRevvedApplyLeaderNameNationSelectText(PPCRegister& localizedText,
                                             PPCRegister& civilization)
{
    if (civilization.s32 < 0 ||
        civilization.s32 >= CIVILIZATION_COUNT)
    {
        return;
    }
    tryReplaceText(localizedText,
                   civilization.s32,
                   NATION_SELECT_TEXT_SURFACE_LEADER_NAME,
                   false,
                   leaderTextBuffer);
}

void ReRevvedApplyCivilizationNameNationSelectText(
    PPCRegister& localizedText,
    PPCRegister& civilization)
{
    if (civilization.s32 < 0 ||
        civilization.s32 >= CIVILIZATION_COUNT)
    {
        return;
    }
    tryReplaceText(localizedText,
                   civilization.s32,
                   NATION_SELECT_TEXT_SURFACE_CIVILIZATION_NAME,
                   false,
                   civilizationTextBuffer);
}

void ReRevvedApplyCivilizationTraitNationSelectText(
    PPCRegister& traitText,
    PPCRegister& civilization)
{
    if (civilization.s32 < 0 ||
        civilization.s32 >= CIVILIZATION_COUNT)
    {
        return;
    }
    tryReplaceText(traitText,
                   civilization.s32,
                   NATION_SELECT_TEXT_SURFACE_CIVILIZATION_TRAIT,
                   true,
                   traitTextBuffer);
}

void ReRevvedApplyUniqueUnitSectionHeadingNationSelectText(
    PPCRegister& heading)
{
    tryReplaceText(heading,
                   NATION_SELECT_TEXT_SELECTOR_UNUSED,
                   NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT_SECTION_HEADING,
                   false,
                   uniqueUnitHeadingTextBuffer);
}

// The live Special Units loop carries both selectors while each localized unit
// name is still separate, before the builder joins multiple names with commas.
void ReRevvedApplyUniqueUnitNationSelectText(PPCRegister& localizedText,
                                             PPCRegister& baseUnitType,
                                             PPCRegister& civilization)
{
    if (civilization.s32 < 0 ||
        civilization.s32 >= CIVILIZATION_COUNT)
    {
        return;
    }
    UnitIdentityId identity = UNIT_IDENTITY_BASE;
    if (!rerevved::unit_catalog::TryResolveUnitIdentity(
            civilization.s32, baseUnitType.s32, identity) ||
        identity == UNIT_IDENTITY_BASE)
    {
        return;
    }

    const NationSelectTextQuery query = {
        sizeof(NationSelectTextQuery),
        NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT,
        civilization.s32,
        NATION_SELECT_TEXT_SELECTOR_UNUSED,
        0,
        baseUnitType.s32,
        identity,
        UNIT_DISPLAY_FORM_UNIT,
        {},
    };
    NationSelectTextEvaluation presentation{};
    if (rerevved::nation_select_text::TryEvaluate(query, presentation) &&
        (presentation.statusFlags &
         NATION_SELECT_TEXT_EVALUATION_REPLACED) != 0)
    {
        tryPublishText(presentation.text,
                       NATION_SELECT_TEXT_CAPACITY,
                       false,
                       unitTextBuffer,
                       localizedText);
    }
}
