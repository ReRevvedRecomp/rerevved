// Values below 4 remove excess civs before recount; barbarian_message_hooks.cpp
// hides farewell messages and peers use host setup.

#include "ai_civilizations.h"

#include "gameplay_fixes.h"
#include "gotw_settings.h"
#include "host_settings.h"
#include "occ_hooks.h"
#include "raging_barbarians_hooks.h"
#include "save_sidecar.h"
#include "start_resources_runtime.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/session_data.h>
#include <rex/ui/notifications.h>

REXCVAR_DECLARE(int32_t, ai_civilizations);

REX_EXTERN(__imp__sub_82D11AD8);
REX_EXTERN(__imp__sub_82D02390);
REX_EXTERN(__imp__sub_82D8A3A0);

namespace
{

namespace ai_civs = rerevved::ai_civilizations;
namespace host    = rerevved::host_settings;

// How long a joining peer waits for the host's settings at game start.
constexpr std::chrono::seconds kHostSettingsWait{ 3 };

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
    for (uint32_t player = 0; player < ai_civs::kBarbarianPlayer; ++player)
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

// The limit for this new game, and a warning to a peer whose game cannot match
// the host's. A peer keeps its own mods and settings; only this game changes.
int32_t resolveLimit(bool network, std::string& source)
{
    auto status = rex::session_data::Current();
    if (network && status.state == rex::session_data::State::kPending)
    {
        status = rex::session_data::WaitForHostData(kHostSettingsWait);
    }

    const int32_t limit = host::ResolveAiLimit(network, status, REXCVAR_GET(ai_civilizations), &source);
    if (!network)
    {
        return limit;
    }

    if (source == "fallback")
    {
        REXLOG_WARN("AI civilizations: host settings not received (state={}); using local limit {}",
                    static_cast<int>(status.state),
                    limit);
        rex::ui::PostNotification("Host settings not received",
                                  "Using your own AI Civilizations setting; this game may go out of sync.");
    }
    else if (source == "received")
    {
        const auto hostSettings = host::Decode(status.bytes);
        const auto differences  = hostSettings
                                      ? host::DescribeModMismatch(
                                            hostSettings->mods, host::LocalMods(), hostSettings->modsTruncated)
                                      : std::string{};
        if (!differences.empty())
        {
            REXLOG_WARN("AI civilizations: mods differ from the host: {}", differences);
            rex::ui::PostNotification("Mods differ from the host", differences + ". This game may go out of sync.");
        }
    }
    return limit;
}

} // namespace

bool rerevved::ai_civilizations::IsRemovedCivilization(uint32_t player)
{
    return player < 32 && (removedMask.load() >> player & 1) != 0;
}

// The statistics recount; acts only for its call right after new-game setup.
REX_HOOK_RAW(sub_82D11AD8)
{
    if (ctx.lr == ai_civs::kNewGameRecountReturn)
    {
        removedMask.store(0);
        const bool    network       = loadU8(base, ai_civs::kNetworkGameByte) != 0;
        const bool    standardRules = rerevved::gotw::StandardRulesForGame();
        std::string   source        = standardRules ? "standard GOTW" : "";
        const int32_t maxAi         = standardRules ? ai_civs::kAllAiCivilizations : resolveLimit(network, source);
        if (maxAi < ai_civs::kAllAiCivilizations)
        {
            removeAiCivilizations(ctx, base, maxAi, source);
        }
        else if (network)
        {
            REXLOG_INFO("AI civilizations: network game keeps every AI civilization, source={}", source);
        }
        rerevved::start_resources::OnNewGameSetup(ctx, base);
    }
    __imp__sub_82D11AD8(ctx, base);
}

// Before the load path's serializer call in sub_82D86400.
void ResetAiCivilizationsOnLoad()
{
    rerevved::start_resources::ResetOnLoad();
    removedMask.store(0);
    FreezeRagingBarbariansForGame();
    FreezeOccForGame();
    rerevved::gameplay_fixes::NoteGameLoad();
    rerevved::save_sidecar::RestoreAfterLoadReset();
}
