// Projects always-at-war onto the relation matrix and refuses peace; decisions: gameplay_fixes.h.

#include "always_at_war_hooks.h"

#include "ai_civilization_setup.h"
#include "ai_civilizations.h"
#include "gameplay_fixes.h"
#include "gameplay_session_settings.h"

#include <array>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace
{

namespace gf = rerevved::gameplay_fixes;
namespace ai = rerevved::ai_civilizations;

constexpr uint32_t kCities         = 0x8314FE28;
constexpr uint32_t kCityStride     = 188;
constexpr uint32_t kCityCount      = 128;
constexpr uint32_t kCityOwner      = 0;
constexpr uint32_t kCityPopulation = 3;

uint8_t* guest(uint32_t address)
{
    return REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address);
}

uint32_t word(uint32_t address)
{
    return rex::memory::load_and_swap<uint32_t>(guest(address));
}

struct Layout
{
    int32_t  count;
    int32_t  barbarian;
    uint32_t humans;
};

Layout layout()
{
    return { static_cast<int32_t>(word(ai::kPlayerCountGlobal)), static_cast<int32_t>(word(ai::kBarbarianPlayerGlobal)), word(ai::kHumanMaskGlobal) };
}

// Single-player has no host snapshot; use the recount wrapper's AI limit.
uint32_t intendedMask()
{
    const auto    l        = layout();
    const int32_t expanded = ai::ExpandedAiLimit();
    return gf::IntendedCivilizations(l.count, l.barbarian, l.humans, expanded != 0 ? expanded : rex::cvar::Query<int32_t>("ai_civilizations"));
}

uint32_t liveMask()
{
    const auto l      = layout();
    uint32_t   cities = 0;
    uint32_t   units  = 0;
    for (uint32_t city = 0; city < kCityCount; ++city)
    {
        const uint8_t* record = guest(kCities + kCityStride * city);
        const auto     owner  = static_cast<int8_t>(record[kCityOwner]);
        if (owner >= 0 && owner < gf::kRelationPlayers && static_cast<int8_t>(record[kCityPopulation]) > 0)
        {
            cities |= 1u << owner;
        }
    }
    for (uint32_t player = 0; player < gf::kRelationPlayers; ++player)
    {
        for (uint32_t unit = 0; unit < ai::kUnitsPerPlayer; ++unit)
        {
            const uint32_t record = ai::UnitRecord(player, unit);
            if (ai::IsLiveUnit(guest(record)[0], word(record + ai::kUnitFlagsOffset)))
            {
                units |= 1u << player;
                break;
            }
        }
    }
    return gf::LiveCivilizations(l.count, l.barbarian, cities, units);
}

uint32_t cellAddress(int32_t p, int32_t q)
{
    return gf::kRelationMatrix + 4 * static_cast<uint32_t>(gf::kRelationPlayers * p + q);
}

void project(const char* phase, uint32_t mask)
{
    std::array<uint32_t, 64> before{};
    for (uint32_t i = 0; i < before.size(); ++i)
    {
        before[i] = word(gf::kRelationMatrix + 4 * i);
    }
    auto          after   = before;
    const int32_t changed = gf::ProjectWar(after, mask);
    for (uint32_t i = 0; i < after.size(); ++i)
    {
        if (after[i] != before[i])
        {
            rex::memory::store_and_swap<uint32_t>(guest(gf::kRelationMatrix + 4 * i), after[i]);
        }
    }
    REXLOG_INFO("Always At War: projected phase={} mask=0x{:02X} cells={}", phase, mask, changed);
}

bool refuses(const char* site, int32_t p, int32_t q)
{
    if (!gf::AlwaysAtWarForGame() || !gf::ForbidsPeace(true, liveMask(), p, q))
    {
        return false;
    }
    REXLOG_INFO("Always At War: refused peace site={} p={} q={}", site, p, q);
    return true;
}

bool refusesWrite(const char* site, int32_t p, int32_t q, uint32_t value)
{
    if (!gf::AlwaysAtWarForGame())
    {
        return false;
    }
    const uint32_t live = liveMask();
    // Command 39 writes both orientations, so either one at war counts.
    const uint32_t forward = word(cellAddress(p, q));
    const uint32_t current = word(cellAddress(q, p)) == gf::kRelationWar ? gf::kRelationWar : forward;
    if (!gf::IsPairOf(live, p, q) || !gf::ForbidsRelationWrite(true, live, p, q, current, value))
    {
        return false;
    }
    REXLOG_INFO("Always At War: refused peace site={} p={} q={}", site, p, q);
    return true;
}

} // namespace

void ProjectAlwaysAtWarFinal()
{
    if (gf::AlwaysAtWarForGame())
    {
        project("final", intendedMask());
    }
}

void ProjectAlwaysAtWarEarly()
{
    if (gf::AlwaysAtWarForGame() && rerevved::gameplay_session::SetupActive())
    {
        project("early", intendedMask());
    }
}

// Setup reaches it with the unlinked -1 relation request.
void ProjectAlwaysAtWarGroups()
{
    if (gf::AlwaysAtWarForGame() && rerevved::gameplay_session::SetupActive())
    {
        project("groups", intendedMask());
    }
}

void ProjectAlwaysAtWarOnLoad()
{
    if (gf::AlwaysAtWarForGame())
    {
        project("load", liveMask());
    }
}

void RefuseContactChoice(PPCRegister& choice, PPCRegister& p, PPCRegister& q)
{
    if (refuses("contact", p.s32, q.s32))
    {
        choice.s64 = -1;
    }
}

bool RefuseContactNotice(PPCRegister& p, PPCRegister& q)
{
    return refuses("notice", p.s32, q.s32);
}

bool RefuseFirstContact(PPCRegister& p, PPCRegister& q)
{
    return refuses("first_contact", p.s32, q.s32);
}

// Pair slots 1668(r1) and 1676(r1) also feed native completion 0x82CD4374.
bool RefuseRetainedContact(PPCRegister& sp)
{
    return refuses("retained_contact", static_cast<int32_t>(word(sp.u32 + 1668)), static_cast<int32_t>(word(sp.u32 + 1676)));
}

// Nonzero takes native war write 0x82CD206C before any transfer.
void RefuseGoldContact(PPCRegister& choice, PPCRegister& p, PPCRegister& q)
{
    if (refuses("gold", p.s32, q.s32))
    {
        choice.s64 = 1;
    }
}

// After Democracy's override of 0; response 1 is native refusal.
void RefuseDemocracyResponse(PPCRegister& response, PPCRegister& p, PPCRegister& q)
{
    if (refuses("response", p.s32, q.s32))
    {
        response.s64 = 1;
    }
}

// acknowledgement and completion.
void RefuseConsumedResponse(PPCRegister& consumed, PPCRegister& p, PPCRegister& q)
{
    if (refuses("consumed", p.s32, q.s32))
    {
        consumed.s64 = 1;
    }
}

bool RefuseTowerPair(PPCRegister& local, PPCRegister& other)
{
    return refuses("tower", local.s32, other.s32);
}

bool RefuseTeslaPair(PPCRegister& local, PPCRegister& other)
{
    return refuses("tesla", local.s32, other.s32);
}

bool RefuseRelationRequest(PPCRegister& p, PPCRegister& q, PPCRegister& value)
{
    return refusesWrite("request", p.s32, q.s32, value.u32);
}

// Completion balances dispatch depth and returns 1.
bool RefuseDeliveredRelation(PPCRegister& p, PPCRegister& q, PPCRegister& value)
{
    return refusesWrite("delivered", p.s32, q.s32, value.u32);
}

// Native checks need network 0, current player, nonhuman AI and known relation.
bool SilenceAiContact(PPCRegister& human, PPCRegister& other)
{
    const uint32_t humans = word(ai::kHumanMaskGlobal);
    const int32_t  x      = human.s32;
    const int32_t  y      = other.s32;
    if (!gf::AlwaysAtWarForGame() || !gf::IsPairOf(liveMask(), x, y) || (humans >> x & 1) == 0 || (humans >> y & 1) != 0)
    {
        return false;
    }
    REXLOG_INFO("Always At War: silenced contact x={} y={}", x, y);
    return true;
}
