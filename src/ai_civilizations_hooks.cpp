// Remove excess civilizations before recount; peers use host setup.

#include "ai_civilizations.h"

#include "ai_civilization_setup.h"
#include "ai_economy_settings.h"
#include "custom_achievement_combat_hooks.h"
#include "custom_achievements.h"
#include "gameplay_fixes.h"
#include "gameplay_session_settings.h"
#include "gotw_settings.h"
#include "occ_hooks.h"
#include "raging_barbarians_hooks.h"
#include "save_sidecar.h"
#include "start_resources_runtime.h"

#include <atomic>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DECLARE(int32_t, ai_civilizations);

REX_EXTERN(__imp__sub_82D11AD8);
REX_EXTERN(__imp__sub_82D02390);
REX_EXTERN(__imp__sub_82D8A3A0);

namespace
{

namespace ai_civs = rerevved::ai_civilizations;

// Slots removed in the current game. Not saved: a load clears it, and the
// next new game replaces it.
std::atomic<uint32_t> removedMask{ 0 };

uint8_t loadU8(const uint8_t* base, uint32_t address)
{
    return base[address];
}

uint32_t loadU32(const uint8_t* base, uint32_t address)
{
    const uint8_t* source = base + address;
    return uint32_t{ source[0] } << 24 | uint32_t{ source[1] } << 16 | uint32_t{ source[2] } << 8 |
           uint32_t{ source[3] };
}

// Calls a guest function as the hooked bl would: on a copy of the caller's
// context, so r1 is the caller's stack at the call site and every register
// the recount and its caller rely on stays unchanged.
uint32_t callGuest(PPCContext& caller, uint8_t* base, PPCFunc* function, uint32_t r3, uint32_t r4, uint32_t r5)
{
    PPCContext call = caller;
    call.r3.u64     = r3;
    call.r4.u64     = r4;
    call.r5.u64     = r5;
    function(call, base);
    caller.fpscr = call.fpscr;
    return call.r3.u32;
}

void removeAiCivilizations(PPCContext& ctx, uint8_t* base, int32_t maxAi, const std::string& source)
{
    const auto     count     = static_cast<int32_t>(loadU32(base, ai_civs::kPlayerCountGlobal));
    const auto     barbarian = static_cast<int32_t>(loadU32(base, ai_civs::kBarbarianPlayerGlobal));
    const uint32_t humanMask = loadU32(base, ai_civs::kHumanMaskGlobal);
    const uint32_t network   = loadU8(base, ai_civs::kNetworkGameByte);
    const uint32_t session   = loadU32(base, ai_civs::kSessionGlobal);
    const auto     local =
        session != 0 ? static_cast<int32_t>(callGuest(ctx, base, __imp__sub_82D8A3A0, session, 0, 0)) : -1;

    const uint32_t removable = ai_civs::RemovableMask(count, barbarian, humanMask, maxAi);
    if (removable == 0)
    {
        REXLOG_INFO("AI civilizations: unchanged, limit={} source={} count={} barbarian={} human_mask=0x{:08X} "
                    "network={} local_player={}",
                    maxAi,
                    source,
                    count,
                    barbarian,
                    humanMask,
                    network,
                    local);
        return;
    }

    ai_civs::KillDigest digest;
    for (uint32_t player = 0; player < ai_civs::kMaxCivilizationPlayers; ++player)
    {
        if ((removable >> player & 1) == 0)
        {
            continue;
        }
        // Killing a unit also kills the units it carries, so each record is
        // tested when reached, as the native loop does.
        for (uint32_t unit = 0; unit < ai_civs::kUnitsPerPlayer; ++unit)
        {
            const uint32_t record = ai_civs::UnitRecord(player, unit);
            if (!ai_civs::IsLiveUnit(loadU8(base, record), loadU32(base, record + ai_civs::kUnitFlagsOffset)))
            {
                continue;
            }
            digest.Add(player, unit, loadU8(base, record + ai_civs::kUnitTypeOffset));
            callGuest(ctx, base, __imp__sub_82D02390, player, unit, 0);
        }
    }
    removedMask.store(removable);

    REXLOG_INFO("AI civilizations: limit={} source={} removed=0x{:02X} human_mask=0x{:08X} network={} "
                "local_player={} killed_units={} digest=0x{:08X}",
                maxAi,
                source,
                removable,
                humanMask,
                network,
                local,
                digest.Count(),
                digest.Value());
}

void logExpandedSetup(const uint8_t* base)
{
    const int32_t requested = ai_civs::ExpandedAiLimit();
    if (requested == 0)
    {
        return;
    }
    const uint32_t count     = loadU32(base, ai_civs::kPlayerCountGlobal);
    const uint32_t barbarian = loadU32(base, ai_civs::kBarbarianPlayerGlobal);
    uint32_t       seen      = 0;
    bool           distinct  = true;
    for (uint32_t player = 0; player < count && player <= ai_civs::kMaxCivilizationPlayers; ++player)
    {
        const uint32_t civilization = loadU32(base, 0x830ECD28 + 4 * player);
        if (player != barbarian)
        {
            distinct = distinct && civilization < 16 && (seen & (1u << civilization)) == 0;
            if (civilization < 16)
            {
                seen |= 1u << civilization;
            }
        }
        uint32_t live = 0;
        for (uint32_t unit = 0; unit < ai_civs::kUnitsPerPlayer; ++unit)
        {
            const uint32_t record = ai_civs::UnitRecord(player, unit);
            live += ai_civs::IsLiveUnit(loadU8(base, record), loadU32(base, record + ai_civs::kUnitFlagsOffset));
        }
        REXLOG_INFO("AI civilizations: player={} civilization={} barbarian={} start=({}, {}) live_records={}",
                    player,
                    civilization,
                    player == barbarian,
                    static_cast<int32_t>(loadU32(base, 0x830E345C + 4 * player)),
                    static_cast<int32_t>(loadU32(base, 0x830E343C + 4 * player)),
                    live);
    }
    REXLOG_INFO("AI civilizations: expanded setup requested={} count={} barbarian={} human_mask=0x{:08X} distinct_ids={}",
                requested,
                count,
                barbarian,
                loadU32(base, ai_civs::kHumanMaskGlobal),
                distinct);
}

} // namespace

bool rerevved::ai_civilizations::IsRemovedCivilization(uint32_t player)
{
    return player < 32 && (removedMask.load() >> player & 1) != 0;
}

// The statistics recount; acts only for its call right after new-game setup.
REX_HOOK_RAW(sub_82D11AD8)
{
    if (ctx.lr == ai_civs::kNewGameRecountReturn && rerevved::gameplay_session::SetupActive())
    {
        removedMask.store(0);
        const bool    network       = loadU8(base, ai_civs::kNetworkGameByte) != 0;
        const bool    standardRules = rerevved::gotw::StandardRulesForGame();
        const auto    settings      = rerevved::gameplay_session::ForNetworkGame();
        std::string   source        = standardRules ? "standard GOTW" : settings ? "host snapshot"
                                                                                 : "local";
        const int32_t expanded      = ai_civs::ExpandedAiLimit();
        const int32_t maxAi         = standardRules ? ai_civs::kAllAiCivilizations : expanded != 0 ? expanded
                                                                                 : settings        ? settings->aiCivilizations
                                                                                                   : REXCVAR_GET(ai_civilizations);
        const int32_t count         = static_cast<int32_t>(loadU32(base, ai_civs::kPlayerCountGlobal));
        const int32_t barbarian     = static_cast<int32_t>(loadU32(base, ai_civs::kBarbarianPlayerGlobal));
        if (ai_civs::CivilizationMask(count, barbarian) != 0 && maxAi < barbarian - 1)
        {
            removeAiCivilizations(ctx, base, maxAi, source);
        }
        else if (network)
        {
            REXLOG_INFO("AI civilizations: network game keeps every AI civilization, source={}", source);
        }
        logExpandedSetup(base);
        ai_civs::NoteNewGameRecount();
        rerevved::start_resources::OnNewGameSetup(ctx, base);
    }
    __imp__sub_82D11AD8(ctx, base);
}

// Before the load path's serializer call in sub_82D86400.
void ResetAiCivilizationsOnLoad()
{
    rerevved::gameplay_session::ResetOnLoad();
    ai_civs::ResetOnLoad();
    rerevved::ai_economy::ResetForLoad();
    rerevved::start_resources::ResetOnLoad();
    removedMask.store(0);
    FreezeRagingBarbariansForGame();
    FreezeOccForGame();
    rerevved::gameplay_fixes::NoteGameLoad();
    rerevved::custom_achievements::ResetProgress();
    rerevved::custom_achievement_combat::ResetCombatObservers();
    rerevved::save_sidecar::RestoreAfterLoadReset();
}
