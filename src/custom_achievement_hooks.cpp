#include "custom_achievements.h"

#include "ai_civilizations.h"
#include "ai_economy_settings.h"
#include "custom_achievement_combat_hooks.h"
#include "occ_settings.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(custom_achievement_trace, false, "ReRevved", "Log native victory records and custom achievement decisions");

REX_EXTERN(__imp__sub_82D052C8);
REX_EXTERN(__imp__sub_82D02AB8);
REX_EXTERN(__imp__sub_82D02390);
REX_EXTERN(__imp__sub_82D8A3A0);

namespace
{

namespace achievements = rerevved::custom_achievements;
namespace ai_civs      = rerevved::ai_civilizations;
namespace combat       = rerevved::custom_achievement_combat;

constexpr uint32_t kDifficulty = 0x82FFE7C8;
constexpr uint32_t kNetwork    = 0x8314F10B; // rerevved::ai_civilizations::kNetworkGameByte
constexpr uint32_t kYear       = 0x8312B8E0; // Current signed calendar year, negative for BC.

bool readBytes(uint32_t address, uint32_t size, uint8_t* out) noexcept
{
    auto* kernel = REX_KERNEL_STATE();
    auto* memory = kernel ? kernel->memory() : nullptr;
    if (!memory || !size || address < 0x10000 || uint64_t{ address } + size > 0xFFFFF000ull)
    {
        return false;
    }
    auto* heap = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(address + size - 1) != heap ||
        (static_cast<uint32_t>(heap->QueryRangeAccess(address, address + size - 1)) & static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly)) !=
            static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly))
    {
        return false;
    }
    std::copy_n(memory->TranslateVirtual<const uint8_t*>(address), size, out);
    return true;
}

bool readWord(uint32_t address, uint32_t& value) noexcept
{
    uint8_t bytes[4];
    if (!readBytes(address, 4, bytes))
    {
        return false;
    }
    value = uint32_t{ bytes[0] } << 24 | uint32_t{ bytes[1] } << 16 | uint32_t{ bytes[2] } << 8 | uint32_t{ bytes[3] };
    return true;
}

// Calls a guest function on a copy of the caller's context, as the hooked bl
// would. Mirrors callGuest in ai_civilizations_hooks.cpp.
uint32_t callGuest(PPCContext& caller, uint8_t* base, PPCFunc* function, uint32_t r3)
{
    PPCContext call = caller;
    call.r3.u64     = r3;
    call.r4.u64     = 0;
    call.r5.u64     = 0;
    function(call, base);
    caller.fpscr = call.fpscr;
    return call.r3.u32;
}

// The four combat-defeat calls of the kill function sub_82D02390, identified
// by return address. At hook entry r1 is the calling combat function's frame,
// whose stack words hold the loser and the winner.
struct CombatSite
{
    uint32_t    lr;
    uint32_t    loserPlayer;
    uint32_t    loserUnit;
    uint32_t    winner;
    const char* name;
};

constexpr CombatSite kCombatSites[] = {
    { 0x82CD83DC, 804, 812, 112, "sub_82CD7A60_attacker_loses" },
    { 0x82CD8938, 112, 124, 804, "sub_82CD7A60_defender_loses" },
    { 0x82CDCD80, 1596, 1604, 1572, "sub_82CD9970_defender_loses" },
    { 0x82CDD124, 1572, 1580, 1596, "sub_82CD9970_attacker_loses" },
};

const CombatSite* findCombatSite(uint64_t lr) noexcept
{
    for (const auto& site : kCombatSites)
    {
        if (site.lr == lr)
        {
            return &site;
        }
    }
    return nullptr;
}

// False when the unit record is unreadable or out of range.
bool readUnitKilled(int32_t player, uint32_t unit, bool& killed) noexcept
{
    uint32_t flags = 0;
    if (player < 0 || static_cast<uint32_t>(player) > static_cast<uint32_t>(ai_civs::kMaxCivilizationPlayers) || unit >= ai_civs::kUnitsPerPlayer ||
        !readWord(ai_civs::UnitRecord(static_cast<uint32_t>(player), unit) + ai_civs::kUnitFlagsOffset, flags))
    {
        return false;
    }
    killed = (flags & ai_civs::kUnitKilledFlag) != 0;
    return true;
}

const char* resultName(achievements::AwardResult result) noexcept
{
    switch (result)
    {
        case achievements::AwardResult::NotQualified:
            return "not_qualified";
        case achievements::AwardResult::AlreadyUnlocked:
            return "already_unlocked";
        case achievements::AwardResult::Unlocked:
            return "unlocked";
        case achievements::AwardResult::Failed:
            return "failed";
    }
    return "unknown";
}

} // namespace

// Native local-player victory handler (r3 = victory type). Every victory test
// calls it only for the local winner. It unlocks the native per-type
// achievements and records the result through sub_82D02AB8.
REX_HOOK_RAW(sub_82D052C8)
{
    const int32_t type = ctx.r3.s32;
    // State is read before the native handler runs. Unreadable difficulty or
    // network state never qualifies, and an unreadable year fails every
    // year-gated predicate.
    uint32_t   difficultyWord = 0;
    uint8_t    networkByte    = 0;
    uint32_t   yearWord       = 0;
    const bool readable       = readWord(kDifficulty, difficultyWord) && readBytes(kNetwork, 1, &networkByte);
    const bool yearReadable   = readWord(kYear, yearWord);
    const auto policy         = rerevved::ai_economy::ForGame();
    const bool nativeEconomy  = rerevved::ai_economy::NativeCostsRequired();
    const auto occ            = rerevved::occ::ForGame();
    const bool occEnabled     = occ && occ->enabled;
    const auto game           = achievements::CurrentProgress();

    __imp__sub_82D052C8(ctx, base);

    try
    {
        achievements::VictoryContext context;
        context.victoryType        = type;
        context.difficulty         = readable ? static_cast<int32_t>(difficultyWord) : 0;
        context.networkGame        = !readable || networkByte != 0;
        context.nativeEconomy      = nativeEconomy;
        context.policy             = policy;
        context.occEnabled         = occEnabled;
        context.occFromSetup       = game.occFromSetup;
        context.yearKnown          = yearReadable;
        context.year               = yearReadable ? static_cast<int32_t>(yearWord) : 0;
        context.historyKnown       = game.historyKnown;
        context.unitBuilt          = game.unitBuilt;
        context.battleFought       = game.battleFought;
        context.deepOceanEntered   = game.deepOceanEntered;
        context.firstFoundingTurn  = game.firstFoundingTurn;
        context.initialAiOpponents = game.initialAiOpponents;
        context.gotwGame           = game.gotwGame;
        context.gotwWeek           = game.gotwWeek;

        const auto decisions = achievements::AwardVictory(context);
        if (REXCVAR_GET(custom_achievement_trace))
        {
            std::string decided;
            for (const auto& decision : decisions)
            {
                decided += fmt::format(" {:X}={}/{}", decision.id, decision.qualifies, resultName(decision.result));
            }
            REXLOG_INFO("Custom achievement trace: victory type={} difficulty={} network={} readable={} year={} year_read={} bonuses={}/{}/{}/{}/{} native_economy={} occ={} occ_from_setup={} history={} unit_built={} battle={} deep_ocean={} first_city_turn={} ai_opponents={} gotw={} week={} decisions:{}",
                        type,
                        context.difficulty,
                        networkByte,
                        readable,
                        context.year,
                        yearReadable,
                        policy.researchBonusPercent,
                        policy.productionBonusPercent,
                        policy.goldBonusPercent,
                        policy.foodBonusPercent,
                        policy.cultureBonusPercent,
                        nativeEconomy,
                        occEnabled,
                        game.occFromSetup,
                        game.historyKnown,
                        game.unitBuilt,
                        game.battleFought,
                        game.deepOceanEntered,
                        game.firstFoundingTurn,
                        game.initialAiOpponents,
                        game.gotwGame,
                        game.gotwWeek,
                        decided);
        }
        for (const auto& decision : decisions)
        {
            if (decision.result == achievements::AwardResult::Unlocked)
            {
                REXLOG_INFO("Custom achievement unlocked: {} (victory type {})", achievements::AchievementName(decision.id), type);
            }
        }
    }
    catch (...)
    {
    }
}

// Native victory record (r3 = winner, r4 = type). Observed only.
REX_HOOK_RAW(sub_82D02AB8)
{
    const int32_t winner = ctx.r3.s32;
    const int32_t type   = ctx.r4.s32;
    __imp__sub_82D02AB8(ctx, base);

    try
    {
        if (REXCVAR_GET(custom_achievement_trace))
        {
            uint32_t   difficulty = 0;
            const bool read       = readWord(kDifficulty, difficulty);
            REXLOG_INFO("Custom achievement trace: victory record winner={} type={} difficulty={} difficulty_read={}",
                        winner,
                        type,
                        static_cast<int32_t>(difficulty),
                        read);
        }
    }
    catch (...)
    {
    }
}

// Native unit kill (r3 = player, r4 = unit, r5 = mode). It also serves many
// non-combat removals, so a defeat counts only from the four combat call sites
// and only when the unit goes from alive to killed.
REX_HOOK_RAW(sub_82D02390)
{
    const uint64_t lr          = ctx.lr;
    const uint32_t callerFrame = ctx.r1.u32;
    const int32_t  loser       = ctx.r3.s32;
    const uint32_t unit        = ctx.r4.u32;
    const int32_t  mode        = ctx.r5.s32;

    const CombatSite* site       = findCombatSite(lr);
    const bool        trace      = REXCVAR_GET(custom_achievement_trace);
    bool              match      = false;
    bool              beforeRead = false;
    bool              before     = false;
    int32_t           winner     = -1;
    int32_t           local      = -1;
    int32_t           barbarian  = -1;
    bool              relevant   = false;
    try
    {
        if (site || trace)
        {
            uint32_t word = 0;
            if (readWord(ai_civs::kBarbarianPlayerGlobal, word))
            {
                barbarian = static_cast<int32_t>(word);
            }
            relevant   = site || loser == barbarian;
            beforeRead = relevant && readUnitKilled(loser, unit, before);
        }
        if (site)
        {
            const uint32_t frame       = ctx.r1.u32;
            uint32_t       stackPlayer = 0;
            uint32_t       stackUnit   = 0;
            uint32_t       stackWinner = 0;
            match                      = readWord(frame + site->loserPlayer, stackPlayer) && readWord(frame + site->loserUnit, stackUnit) &&
                                         readWord(frame + site->winner, stackWinner) && static_cast<int32_t>(stackPlayer) == loser && stackUnit == unit;
            if (match)
            {
                winner           = static_cast<int32_t>(stackWinner);
                uint32_t session = 0;
                local            = readWord(ai_civs::kSessionGlobal, session) && session != 0
                                       ? static_cast<int32_t>(callGuest(ctx, base, __imp__sub_82D8A3A0, session))
                                       : -1;
            }
        }
        else if (combat::IsBypassKill(static_cast<uint32_t>(lr)))
        {
            // Nuclear victims bypass the mode 0 combat observers. The launcher is the helper's
            // player argument: frame+276 in sub_82CF7428, still in r16 in sub_82CF78D8.
            uint32_t   session  = 0;
            uint32_t   launched = 0;
            const bool known    = static_cast<uint32_t>(lr) == 0x82CF7818 ? readWord(callerFrame + 276, launched) : (launched = ctx.r16.u32, true);
            combat::ObserveBypassKill(static_cast<uint32_t>(lr),
                                      loser,
                                      unit,
                                      mode,
                                      readWord(ai_civs::kSessionGlobal, session) && session != 0 ? static_cast<int32_t>(callGuest(ctx, base, __imp__sub_82D8A3A0, session)) : -1,
                                      known && launched < 8 ? static_cast<int32_t>(launched) : -1);
        }
    }
    catch (...)
    {
        match = false;
    }

    __imp__sub_82D02390(ctx, base);

    if (!relevant)
    {
        return;
    }
    try
    {
        bool       after     = false;
        const bool afterRead = readUnitKilled(loser, unit, after);

        achievements::DefeatEvent event;
        event.combatSite        = site && mode == 0;
        event.participantsMatch = match;
        event.loser             = loser;
        event.winner            = winner;
        event.local             = local;
        event.barbarian         = barbarian;
        // An unreadable record never counts as a transition.
        event.killedBefore = !beforeRead || before;
        event.killedAfter  = afterRead && after;

        const bool counts = achievements::CountsAsBarbarianDefeat(event);
        auto       result = achievements::AwardResult::NotQualified;
        if (counts)
        {
            result = achievements::RecordBarbarianDefeat();
        }
        if (trace)
        {
            REXLOG_INFO("Custom achievement trace: kill site={} lr=0x{:08X} mode={} loser={}/{} winner={} local={} barbarian={} participants_match={} killed_before={} killed_after={} counted={} count={} result={}",
                        site ? site->name : "other",
                        static_cast<uint32_t>(lr),
                        mode,
                        loser,
                        unit,
                        winner,
                        local,
                        barbarian,
                        match,
                        event.killedBefore,
                        event.killedAfter,
                        counts,
                        achievements::CurrentProgress().barbarianDefeats,
                        resultName(result));
        }
        if (result == achievements::AwardResult::Unlocked)
        {
            REXLOG_INFO("Custom achievement unlocked: Their Rage Is Within Your Control");
        }
        // Last Stand: the two attacker-defeat sites, joined to the resolver token.
        if (site && match && mode == 0)
        {
            combat::ObserveAttackerDefeat({ static_cast<uint32_t>(lr), callerFrame, loser, unit, winner, local, beforeRead && !before, afterRead && after });
        }
    }
    catch (...)
    {
    }
}
