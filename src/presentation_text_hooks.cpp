#include "presentation_text_registry.h"

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

thread_local uint32_t era_text_buffer                 = 0;
thread_local uint32_t leader_text_buffer              = 0;
thread_local uint32_t civilization_text_buffer        = 0;
thread_local uint32_t trait_text_buffer               = 0;
thread_local uint32_t unique_unit_heading_text_buffer = 0;
thread_local uint32_t unit_text_buffer                = 0;

uint32_t ReadBigEndianU32(const uint8_t* value)
{
    return (uint32_t{ value[0] } << 24) | (uint32_t{ value[1] } << 16) |
           (uint32_t{ value[2] } << 8) | uint32_t{ value[3] };
}

void WriteBigEndianU32(uint8_t* destination, uint32_t value)
{
    destination[0] = static_cast<uint8_t>(value >> 24);
    destination[1] = static_cast<uint8_t>(value >> 16);
    destination[2] = static_cast<uint8_t>(value >> 8);
    destination[3] = static_cast<uint8_t>(value);
}

bool TryReadGuestU32(uint32_t address, uint32_t& value)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory)
    {
        return false;
    }
    value = ReadBigEndianU32(
        memory->TranslateVirtual<const uint8_t*>(address));
    return true;
}

bool TryPublishText(const char*  text,
                    size_t       capacity,
                    bool         include_length_header,
                    uint32_t&    guest_buffer,
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
    if (guest_buffer == 0)
    {
        const size_t allocation_size =
            capacity + (include_length_header ? sizeof(uint32_t) : 0u);
        guest_buffer =
            memory->SystemHeapAlloc(static_cast<uint32_t>(allocation_size));
        if (guest_buffer == 0)
        {
            return false;
        }
    }
    const uint32_t text_address =
        guest_buffer + (include_length_header ? sizeof(uint32_t) : 0u);
    if (include_length_header)
    {
        WriteBigEndianU32(memory->TranslateVirtual<uint8_t*>(guest_buffer),
                          static_cast<uint32_t>(length));
    }
    auto* destination = memory->TranslateVirtual<char*>(text_address);
    std::memcpy(destination, text, length + 1);
    out.u64 = text_address;
    return true;
}

bool TryEvaluateText(ReRevvedPresentationSurface         surface,
                     ReRevvedCivilizationId              civilization,
                     ReRevvedUniqueEraUnlockEra          unlock_era,
                     ReRevvedPresentationTextEvaluation& presentation)
{
    const ReRevvedPresentationTextQuery query = {
        sizeof(ReRevvedPresentationTextQuery),
        surface,
        civilization,
        unlock_era,
        REREVVED_PRESENTATION_SELECTOR_UNUSED,
        REREVVED_PRESENTATION_SELECTOR_UNUSED,
        REREVVED_PRESENTATION_SELECTOR_UNUSED,
        REREVVED_PRESENTATION_SELECTOR_UNUSED,
        {},
    };
    return rerevved::presentation_text::TryEvaluate(query, presentation) &&
           (presentation.status_flags &
            REREVVED_PRESENTATION_TEXT_EVALUATION_REPLACED) != 0;
}

bool TryReplaceText(PPCRegister&                localized_text,
                    ReRevvedCivilizationId      civilization,
                    ReRevvedPresentationSurface surface,
                    bool                        include_length_header,
                    uint32_t&                   guest_buffer)
{
    ReRevvedPresentationTextEvaluation presentation{};
    if (!TryEvaluateText(surface,
                         civilization,
                         REREVVED_PRESENTATION_SELECTOR_UNUSED,
                         presentation))
    {
        return false;
    }
    return TryPublishText(presentation.text,
                          REREVVED_PRESENTATION_TEXT_CAPACITY,
                          include_length_header,
                          guest_buffer,
                          localized_text);
}

bool TryEvaluateEraText(ReRevvedCivilizationId              civilization,
                        ReRevvedUniqueEraUnlockEra          era,
                        ReRevvedPresentationTextEvaluation& presentation)
{
    uint32_t       native_bits = 0;
    const uint32_t native_address =
        kUniqueEraAbilityTable + static_cast<uint32_t>(civilization) * 16u +
        static_cast<uint32_t>(era) * sizeof(uint32_t);
    if (!TryReadGuestU32(native_address, native_bits))
    {
        return false;
    }

    ReRevvedUniqueEraAbilityCellEvaluation ability{};
    if (!rerevved::unique_era_abilities::TryEvaluate(
            civilization,
            era,
            static_cast<ReRevvedUniqueEraAbilityId>(native_bits),
            ability) ||
        (ability.status_flags &
         REREVVED_UNIQUE_ERA_ABILITY_EVALUATION_REPLACEMENT_CONFLICT) != 0)
    {
        return false;
    }

    const ReRevvedPresentationTextQuery query = {
        sizeof(ReRevvedPresentationTextQuery),
        REREVVED_PRESENTATION_SURFACE_ERA_ABILITY,
        civilization,
        era,
        ability.effective_ability,
        REREVVED_PRESENTATION_SELECTOR_UNUSED,
        REREVVED_PRESENTATION_SELECTOR_UNUSED,
        REREVVED_PRESENTATION_SELECTOR_UNUSED,
        {},
    };
    return rerevved::presentation_text::TryEvaluate(query, presentation) &&
           (presentation.status_flags &
            REREVVED_PRESENTATION_TEXT_EVALUATION_REPLACED) != 0;
}

bool TryReplaceEraLines(const char*                          native_text,
                        ReRevvedCivilizationId               civilization,
                        std::array<char, kEraBlockCapacity>& output)
{
    std::array<const char*, 9> starts{};
    std::array<size_t, 9>      lengths{};
    const char*                current = native_text;
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

    size_t used       = 0;
    bool   any_change = false;
    for (size_t index = 0; index < starts.size(); ++index)
    {
        const char*                        replacement = starts[index];
        size_t                             length      = lengths[index];
        ReRevvedPresentationTextEvaluation presentation{};
        bool                               replaced = false;
        if (index != 0 && (index & 1u) != 0)
        {
            replaced = TryEvaluateText(
                REREVVED_PRESENTATION_SURFACE_ERA_HEADING,
                REREVVED_PRESENTATION_SELECTOR_UNUSED,
                static_cast<ReRevvedUniqueEraUnlockEra>((index - 1) / 2),
                presentation);
        }
        else if (index != 0 &&
                 TryEvaluateEraText(
                     civilization,
                     static_cast<ReRevvedUniqueEraUnlockEra>((index - 2) / 2),
                     presentation))
        {
            replaced = true;
        }
        if (replaced)
        {
            replacement = presentation.text;
            length      = std::strlen(replacement);
            any_change  = true;
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
    return any_change;
}

} // namespace

// The civilization-selection builder stores one heading followed by four
// label/value pairs. Global headings and civilization-specific values each
// replace only when exactly one matching rule is registered.
void ReRevvedApplyEraAbilityPresentationText(PPCRegister& era_block,
                                             PPCRegister& civilization)
{
    if (civilization.s32 < 0 ||
        civilization.s32 >= REREVVED_CIVILIZATION_COUNT ||
        era_block.u32 == 0)
    {
        return;
    }

    const auto* native_text =
        REX_KERNEL_MEMORY()->TranslateVirtual<const char*>(era_block.u32);
    std::array<char, kEraBlockCapacity> replacement{};
    if (TryReplaceEraLines(native_text, civilization.s32, replacement))
    {
        TryPublishText(replacement.data(),
                       replacement.size(),
                       true,
                       era_text_buffer,
                       era_block);
    }
}

void ReRevvedApplyLeaderNamePresentationText(PPCRegister& localized_text,
                                             PPCRegister& civilization)
{
    if (civilization.s32 < 0 ||
        civilization.s32 >= REREVVED_CIVILIZATION_COUNT)
    {
        return;
    }
    TryReplaceText(localized_text,
                   civilization.s32,
                   REREVVED_PRESENTATION_SURFACE_LEADER_NAME,
                   false,
                   leader_text_buffer);
}

void ReRevvedApplyCivilizationNamePresentationText(
    PPCRegister& localized_text,
    PPCRegister& civilization)
{
    if (civilization.s32 < 0 ||
        civilization.s32 >= REREVVED_CIVILIZATION_COUNT)
    {
        return;
    }
    TryReplaceText(localized_text,
                   civilization.s32,
                   REREVVED_PRESENTATION_SURFACE_CIVILIZATION_NAME,
                   false,
                   civilization_text_buffer);
}

void ReRevvedApplyCivilizationTraitPresentationText(
    PPCRegister& trait_text,
    PPCRegister& civilization)
{
    if (civilization.s32 < 0 ||
        civilization.s32 >= REREVVED_CIVILIZATION_COUNT)
    {
        return;
    }
    TryReplaceText(trait_text,
                   civilization.s32,
                   REREVVED_PRESENTATION_SURFACE_CIVILIZATION_TRAIT,
                   true,
                   trait_text_buffer);
}

void ReRevvedApplyUniqueUnitSectionHeadingPresentationText(
    PPCRegister& heading)
{
    TryReplaceText(heading,
                   REREVVED_PRESENTATION_SELECTOR_UNUSED,
                   REREVVED_PRESENTATION_SURFACE_UNIQUE_UNIT_SECTION_HEADING,
                   false,
                   unique_unit_heading_text_buffer);
}

// The live Special Units loop carries both selectors while each localized unit
// name is still separate, before the builder joins multiple names with commas.
void ReRevvedApplyUniqueUnitPresentationText(PPCRegister& localized_text,
                                             PPCRegister& base_unit_type,
                                             PPCRegister& civilization)
{
    if (civilization.s32 < 0 ||
        civilization.s32 >= REREVVED_CIVILIZATION_COUNT)
    {
        return;
    }
    ReRevvedUnitIdentityId identity = REREVVED_UNIT_IDENTITY_BASE;
    if (!rerevved::unit_catalog::TryResolveUnitIdentity(
            civilization.s32, base_unit_type.s32, identity) ||
        identity == REREVVED_UNIT_IDENTITY_BASE)
    {
        return;
    }

    const ReRevvedPresentationTextQuery query = {
        sizeof(ReRevvedPresentationTextQuery),
        REREVVED_PRESENTATION_SURFACE_UNIQUE_UNIT,
        civilization.s32,
        REREVVED_PRESENTATION_SELECTOR_UNUSED,
        0,
        base_unit_type.s32,
        identity,
        REREVVED_UNIT_DISPLAY_FORM_UNIT,
        {},
    };
    ReRevvedPresentationTextEvaluation presentation{};
    if (rerevved::presentation_text::TryEvaluate(query, presentation) &&
        (presentation.status_flags &
         REREVVED_PRESENTATION_TEXT_EVALUATION_REPLACED) != 0)
    {
        TryPublishText(presentation.text,
                       REREVVED_PRESENTATION_TEXT_CAPACITY,
                       false,
                       unit_text_buffer,
                       localized_text);
    }
}
