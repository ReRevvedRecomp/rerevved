#include "custom_achievement_combat_hooks.h"

#include "ai_civilizations.h"
#include "capital_grace.h"
#include "custom_achievements.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(bool, custom_achievement_trace);

REX_EXTERN(__imp__sub_82CD9970);
REX_EXTERN(__imp__sub_82D8A3A0);

// Evidence: rerevved-research manifests/achievement-event-boundaries.json
// RVA-F-0359 (operative pair, defensive receipts, ordinary result bridge) and
// RVA-F-0360 (committed resolution observer). Every receipt is conditional
// static evidence; the trace lines are the runtime acceptance record.
namespace
{

namespace achievements = rerevved::custom_achievements;
namespace ai_civs      = rerevved::ai_civilizations;
namespace combat       = rerevved::custom_achievement_combat;
namespace grace        = rerevved::capital_grace;

using QueueWords = grace::Words;

constexpr uint32_t kTurn        = 0x8312B8DC;
constexpr uint32_t kQueue       = 0x8314F1F8; // read cursor R, with write cursor W in the preceding word
constexpr uint32_t kRngBlock    = 0x82F7AD00;
constexpr uint32_t kCities      = 0x8314FE28;
constexpr uint32_t kCityStride  = 188;
constexpr uint32_t kCityPlane   = 0x83128D08; // signed city ordinal per tile, 40 * x + y
constexpr int32_t  kMapStride   = 40;
constexpr int32_t  kPlayerLimit = 8;
constexpr int32_t  kMilitiaType = 7;
constexpr uint32_t kSameFrameLr = 0x82CDD124; // resolver: attacker killed at 0x82CDD120
constexpr uint32_t kResultLr    = 0x82CD83DC; // result handler: attacker killed at 0x82CD83D8

// Frame words of sub_82CD9970 (the arguments spilled by its prologue).
constexpr uint32_t kAttackerPlayer = 1572;
constexpr uint32_t kAttackerUnit   = 1580;
constexpr uint32_t kMode           = 1588;
constexpr uint32_t kDefenderPlayer = 1596;
constexpr uint32_t kDefenderUnit   = 1604;
constexpr uint32_t kAttackWeight   = 156;
constexpr uint32_t kDefenseWeight  = 160;
constexpr uint32_t kRngPointer     = 200;

// Frame sizes (stwu r1,-N(r1)) used to relate nested frames.
constexpr uint32_t kResolverFrame      = 1552; // sub_82CD9970
constexpr uint32_t kResultFrame        = 784;  // sub_82CD7A60
constexpr uint32_t kResultDefenderUnit = 124;  // normalized defender slot, stored at 0x82CD7C68
constexpr uint32_t kDispatchFrame      = 368;  // sub_82CDDF08
constexpr uint32_t kSubmitFrame        = 256;  // sub_82CE1950
constexpr uint32_t kQueueFrame         = 144;  // sub_82CE16C8

constexpr size_t kScopes  = 8;
constexpr size_t kTokens  = 8;
constexpr size_t kSubmits = 16;

struct NativeWord
{
    uint32_t address;
    uint32_t value;
};

// PowerPC encodings of the instructions each hook precedes plus the frame
// allocations the frame arithmetic relies on.
constexpr NativeWord kProfile[] = {
    { 0x82CD997C, 0x9421F9F0 }, // stwu r1,-1552(r1)
    { 0x82CD7A68, 0x9421FCF0 }, // stwu r1,-784(r1)
    { 0x82CDDF10, 0x9421FE90 }, // stwu r1,-368(r1)
    { 0x82CE1958, 0x9421FF00 }, // stwu r1,-256(r1)
    { 0x82CE16D0, 0x9421FF70 }, // stwu r1,-144(r1)
    { 0x82CDC424, 0x3D6082F8 }, // lis r11,-32008
    { 0x82CDC674, 0x8161009C }, // lwz r11,156(r1)
    { 0x82CDC898, 0x419A0018 }, // beq cr6,0x82CDC8B0
    { 0x82CDDEE8, 0x48003A69 }, // bl 0x82CE1950
    { 0x82CDDEEC, 0x3D40830E }, // lis r10,-31986
    { 0x82CDFAE8, 0x4BFF7F79 }, // bl 0x82CD7A60
    { 0x82CDFAEC, 0x2F030000 }, // cmpwi cr6,r3,0
};

// Kill call sites of routes outside the mode 0 observer, logged for the trace.
struct BypassSite
{
    uint32_t    lr;
    const char* name;
};

constexpr BypassSite kBypassSites[] = {
    { 0x82CF7818, "nuclear_zero_victim_kill" },
    { 0x82CF7D18, "nuclear_alternate_victim_kill" },
};

bool tracing() noexcept
{
    return REXCVAR_GET(custom_achievement_trace);
}

bool accessible(uint32_t address, uint32_t size, bool write) noexcept
{
    if (!size || address < 0x10000 || uint64_t{ address } + size > 0xFFFFF000ull)
    {
        return false;
    }
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory)
    {
        return false;
    }
    auto* heap = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(address + size - 1) != heap)
    {
        return false;
    }
    const auto access   = static_cast<uint32_t>(heap->QueryRangeAccess(address, address + size - 1));
    const auto required = static_cast<uint32_t>(write ? rex::memory::PageAccess::kReadWrite : rex::memory::PageAccess::kReadOnly);
    return (access & required) == required;
}

bool readBytes(uint32_t address, uint32_t size, uint8_t* out) noexcept
{
    if (!accessible(address, size, false))
    {
        return false;
    }
    std::memcpy(out, REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address), size);
    return true;
}

uint32_t be32(const uint8_t* bytes) noexcept
{
    return (uint32_t{ bytes[0] } << 24) | (uint32_t{ bytes[1] } << 16) | (uint32_t{ bytes[2] } << 8) | bytes[3];
}

int32_t bes16(const uint8_t* bytes) noexcept
{
    return std::bit_cast<int16_t>(static_cast<uint16_t>((uint16_t{ bytes[0] } << 8) | bytes[1]));
}

bool readWord(uint32_t address, uint32_t& value) noexcept
{
    uint8_t bytes[4];
    if (!readBytes(address, 4, bytes))
    {
        return false;
    }
    value = be32(bytes);
    return true;
}

// The profile is latched after the first complete comparison. An unreadable
// word is not a verdict and is retried.
bool supportedProfile() noexcept
{
    static std::atomic<int> verdict{ 0 };
    const int               known = verdict.load();
    if (known != 0)
    {
        return known == 1;
    }
    for (const auto& expected : kProfile)
    {
        uint32_t actual = 0;
        if (!readWord(expected.address, actual))
        {
            return false;
        }
        if (actual != expected.value)
        {
            if (verdict.exchange(2) == 0)
            {
                REXLOG_WARN("Custom achievement combat observers are unavailable: native word at 0x{:08X} is 0x{:08X}, expected 0x{:08X}", expected.address, actual, expected.value);
            }
            return false;
        }
    }
    verdict.store(1);
    return true;
}

// Keeps the host float control state unchanged around injected guest calls.
struct HostFloatScope
{
    uint32_t saved = PPCFPSCRRegister::Platform::getcsr();

    ~HostFloatScope()
    {
        PPCFPSCRRegister::Platform::setcsr(saved);
    }
};

// Local player through the native accessor on a private frame below the live
// one, as a native bl would. Returns -1 when unknown. Session word 0 means none.
int32_t localPlayer(PPCRegister& stack, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr) noexcept
{
    try
    {
        uint32_t session = 0;
        if (!readWord(ai_civs::kSessionGlobal, session) || session == 0 || stack.u32 < 0x10070 || !accessible(stack.u32 - 0x70, 0x70, true))
        {
            return -1;
        }
        PPCContext call{};
        call.r1.u64 = stack.u32 - 0x70;
        call.r13    = threadPointer;
        call.fpscr  = fpscr;
        call.r3.u64 = session;
        call.r4.u64 = 0;
        call.r5.u64 = 0;
        // Xenon stwu frames use a big-endian 32-bit back chain.
        const uint8_t backChain[] = { static_cast<uint8_t>(stack.u32 >> 24), static_cast<uint8_t>(stack.u32 >> 16), static_cast<uint8_t>(stack.u32 >> 8), static_cast<uint8_t>(stack.u32) };
        std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(call.r1.u32), backChain, sizeof(backChain));
        HostFloatScope scope;
        __imp__sub_82D8A3A0(call, REX_KERNEL_MEMORY()->virtual_membase());
        return call.r3.s32;
    }
    catch (...)
    {
        return -1;
    }
}

struct UnitView
{
    int32_t  owner = -1;
    int32_t  type  = -1;
    uint32_t flags = 0;
    int32_t  x     = -1;
    int32_t  y     = -1;
};

bool readUnit(int32_t player, uint32_t slot, UnitView& unit) noexcept
{
    uint8_t bytes[0x24];
    if (player < 0 || player >= kPlayerLimit || slot >= ai_civs::kUnitsPerPlayer || !readBytes(ai_civs::UnitRecord(static_cast<uint32_t>(player), slot), sizeof(bytes), bytes))
    {
        return false;
    }
    unit.owner = bytes[0];
    unit.type  = std::bit_cast<int8_t>(bytes[1]);
    unit.flags = be32(bytes + ai_civs::kUnitFlagsOffset);
    unit.x     = bes16(bytes + 0x1C);
    unit.y     = bes16(bytes + 0x1E);
    return true;
}

bool liveUnit(const UnitView& unit, int32_t player) noexcept
{
    return unit.owner == player && ai_civs::IsLiveUnit(static_cast<uint8_t>(unit.owner), unit.flags);
}

struct CityView
{
    int32_t ordinal    = -1;
    int32_t owner      = -1;
    int32_t population = 0;
    int32_t x          = -1;
    int32_t y          = -1;
};

// The city whose tile plane entry covers (x, y), as the capital grace
// admission reads it (0x83128D08 plane, city owner +0, population +3, tile +0x30/+0x32).
bool readCityAt(int32_t x, int32_t y, CityView& city) noexcept
{
    if (x < 0 || x >= kMapStride || y < 0 || y >= kMapStride)
    {
        return false;
    }
    uint8_t ordinal = 0;
    if (!readBytes(kCityPlane + static_cast<uint32_t>(kMapStride * x + y), 1, &ordinal))
    {
        return false;
    }
    const int32_t index = std::bit_cast<int8_t>(ordinal);
    if (index < 0 || index > 127)
    {
        return false;
    }
    uint8_t bytes[0x34];
    if (!readBytes(kCities + kCityStride * static_cast<uint32_t>(index), sizeof(bytes), bytes))
    {
        return false;
    }
    city.ordinal    = index;
    city.owner      = std::bit_cast<int8_t>(bytes[0]);
    city.population = std::bit_cast<int8_t>(bytes[3]);
    city.x          = bes16(bytes + 0x30);
    city.y          = bes16(bytes + 0x32);
    return true;
}

bool readQueue(uint32_t& write, uint32_t& read) noexcept
{
    return readWord(kQueue - 4, write) && readWord(kQueue, read);
}

// One mode 0 resolver invocation from the operative pair to its outcome.
struct Battle
{
    uint32_t id             = 0;
    uint32_t frame          = 0;
    uint32_t turn           = 0;
    int32_t  attackerPlayer = -1;
    uint32_t attackerUnit   = 0;
    int32_t  defenderPlayer = -1;
    uint32_t defenderUnit   = 0;
    int32_t  attack         = 0;
    int32_t  defense        = 0;
    int32_t  local          = -2; // -2 not asked, -1 unknown
    // The defender was a live Militia in a populated city of its owner at the pair snapshot.
    bool    militia       = false;
    int32_t city          = -1;
    int32_t x             = -1;
    int32_t y             = -1;
    bool    decision      = false;
    bool    stable        = true;
    bool    rng           = false;
    bool    battleCounted = false;
    bool    lastStandDone = false;
};

struct Scope
{
    uint32_t frame = 0;
    bool     armed = false;
    Battle   battle;
};

enum class Stage
{
    Open,      // emitted by the resolver, not yet appended
    Appending, // append observed, copy not complete
    Pending,   // queued at serial W, waiting for R == W
    Draining,  // claimed by the drain of one submit frame
    Invoked,   // the shared handler was called with this record
};

struct Token
{
    Battle     battle;
    Stage      stage = Stage::Open;
    QueueWords words{};
    uint32_t   emitFrame     = 0;
    bool       emitting      = false;
    uint32_t   wrapper       = 0;
    uint32_t   queue         = 0;
    uint32_t   serial        = 0;
    uint32_t   drainRecord   = 0;
    uint32_t   dispatchFrame = 0;
    int32_t    handlerResult = -1;
};

struct Submit
{
    uint32_t frame     = 0;
    uint32_t queue     = 0;
    uint32_t read      = 0;
    bool     validRead = false;
};

struct ThreadCombat
{
    rex::runtime::ThreadState*   thread     = nullptr;
    uint32_t                     generation = 0;
    std::array<Scope, kScopes>   scopes{};
    size_t                       scopeCount = 0;
    std::array<Token, kTokens>   tokens{};
    size_t                       tokenCount = 0;
    std::array<Submit, kSubmits> submits{};
    size_t                       submitCount = 0;
};

thread_local ThreadCombat threadCombat;
std::atomic<uint32_t>     generation{ 0 };
std::atomic<uint32_t>     nextId{ 1 };

ThreadCombat& state() noexcept
{
    auto*      current = rex::runtime::ThreadState::Get();
    const auto epoch   = generation.load(std::memory_order_relaxed);
    if (threadCombat.thread != current || threadCombat.generation != epoch)
    {
        threadCombat            = ThreadCombat{};
        threadCombat.thread     = current;
        threadCombat.generation = epoch;
    }
    return threadCombat;
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

const char* stageName(Stage stage) noexcept
{
    switch (stage)
    {
        case Stage::Open:
            return "open";
        case Stage::Appending:
            return "appending";
        case Stage::Pending:
            return "pending";
        case Stage::Draining:
            return "draining";
        case Stage::Invoked:
            return "invoked";
    }
    return "unknown";
}

std::string describe(const Battle& battle)
{
    return fmt::format("token={} frame=0x{:08X} mode=0 attacker={}/{} defender={}/{} local={} pair={}/{} decision={} stable={} rng={} militia={} city={} tile={},{} turn={} counted={} done={}",
                       battle.id,
                       battle.frame,
                       battle.attackerPlayer,
                       battle.attackerUnit,
                       battle.defenderPlayer,
                       battle.defenderUnit,
                       battle.local,
                       battle.attack,
                       battle.defense,
                       battle.decision,
                       battle.stable,
                       battle.rng,
                       battle.militia,
                       battle.city,
                       battle.x,
                       battle.y,
                       battle.turn,
                       battle.battleCounted,
                       battle.lastStandDone);
}

std::string queueWords(const QueueWords& words)
{
    return fmt::format("[{},{},{},{},{}]", words[0], words[1], words[2], words[3], words[4]);
}

// Raw tuple of one observer, only when custom_achievement_trace is on.
void trace(const char* site, const Battle& battle, const std::string& detail = {}) noexcept
{
    try
    {
        if (!tracing())
        {
            return;
        }
        uint32_t   write  = 0;
        uint32_t   read   = 0;
        const bool queued = readQueue(write, read);
        REXLOG_INFO("Custom achievement trace: combat site={} {} W={} R={} {}", site, describe(battle), queued ? static_cast<int64_t>(write) : -1, queued ? static_cast<int64_t>(read) : -1, detail);
    }
    catch (...)
    {
    }
}

void removeToken(ThreadCombat& thread, size_t index) noexcept
{
    for (size_t i = index + 1; i < thread.tokenCount; ++i)
    {
        thread.tokens[i - 1] = thread.tokens[i];
    }
    --thread.tokenCount;
}

void invalidate(ThreadCombat& thread, size_t index, const char* reason)
{
    trace("token_invalidated", thread.tokens[index].battle, fmt::format("reason={} stage={} serial={}", reason, stageName(thread.tokens[index].stage), thread.tokens[index].serial));
    removeToken(thread, index);
}

Scope* findScope(ThreadCombat& thread, uint32_t frame) noexcept
{
    for (size_t i = thread.scopeCount; i > 0; --i)
    {
        if (thread.scopes[i - 1].frame == frame)
        {
            return &thread.scopes[i - 1];
        }
    }
    return nullptr;
}

// Reads the resolver's frame words. False when any is unreadable.
struct Frame
{
    uint32_t mode           = 0;
    int32_t  attackerPlayer = -1;
    uint32_t attackerUnit   = 0;
    int32_t  defenderPlayer = -1;
    uint32_t defenderUnit   = 0;
    int32_t  attack         = 0;
    int32_t  defense        = 0;
    uint32_t rngPointer     = 0;
};

bool readFrame(uint32_t frame, Frame& out) noexcept
{
    uint32_t attackerPlayer = 0;
    uint32_t defenderPlayer = 0;
    uint32_t attack         = 0;
    uint32_t defense        = 0;
    if (!readWord(frame + kMode, out.mode) || !readWord(frame + kAttackerPlayer, attackerPlayer) || !readWord(frame + kAttackerUnit, out.attackerUnit) ||
        !readWord(frame + kDefenderPlayer, defenderPlayer) || !readWord(frame + kDefenderUnit, out.defenderUnit) || !readWord(frame + kAttackWeight, attack) ||
        !readWord(frame + kDefenseWeight, defense) || !readWord(frame + kRngPointer, out.rngPointer))
    {
        return false;
    }
    out.attackerPlayer = static_cast<int32_t>(attackerPlayer);
    out.defenderPlayer = static_cast<int32_t>(defenderPlayer);
    out.attack         = static_cast<int32_t>(attack);
    out.defense        = static_cast<int32_t>(defense);
    return true;
}

// The original defender is a live Militia in a populated city of its owner,
// standing on that city's tile. Fills the city facts.
bool militiaInCity(Battle& battle) noexcept
{
    UnitView defender;
    CityView city;
    if (!readUnit(battle.defenderPlayer, battle.defenderUnit, defender) || !liveUnit(defender, battle.defenderPlayer) || defender.type != kMilitiaType ||
        !readCityAt(defender.x, defender.y, city) || city.owner != battle.defenderPlayer || city.population <= 0 || city.x != defender.x || city.y != defender.y)
    {
        return false;
    }
    battle.city = city.ordinal;
    battle.x    = defender.x;
    battle.y    = defender.y;
    return true;
}

// Re-reads the Militia and its city after the battle: same record, tile and
// city, still live, and the city is still populated and owned by the local player.
bool militiaRetained(const Battle& battle, int32_t local) noexcept
{
    UnitView defender;
    CityView city;
    return local == battle.defenderPlayer && readUnit(battle.defenderPlayer, battle.defenderUnit, defender) && liveUnit(defender, battle.defenderPlayer) &&
           defender.type == kMilitiaType && defender.x == battle.x && defender.y == battle.y && readCityAt(defender.x, defender.y, city) &&
           city.ordinal == battle.city && city.owner == local && city.population > 0 && city.x == defender.x && city.y == defender.y;
}

// Last Stand for a decisive defense of a retained city by the local Militia.
// resultDefender is the result handler's normalized defender slot, or UINT32_MAX
// on the same-frame route where the resolver's own defender slot is the snapshot.
void awardLastStand(Battle& battle, const char* route, const combat::Defeat& defeat, uint32_t resultDefender)
{
    const char* verdict = "awarded";
    auto        result  = achievements::AwardResult::NotQualified;
    uint32_t    turn    = 0;
    if (battle.lastStandDone)
    {
        verdict = "already_done";
    }
    else if (!readWord(kTurn, turn) || turn != battle.turn)
    {
        // A queued record that outlived its turn has a broken lifetime.
        verdict = "turn_changed";
    }
    else if (!battle.militia)
    {
        verdict = "not_militia_defense";
    }
    else if (!battle.decision || !battle.rng)
    {
        verdict = "resolution_not_committed";
    }
    else if (!battle.stable)
    {
        verdict = "operative_pair_changed";
    }
    else if (defeat.local < 0 || defeat.local != battle.defenderPlayer)
    {
        verdict = "defender_not_local";
    }
    else if (defeat.loser != battle.attackerPlayer || defeat.unit != battle.attackerUnit || defeat.winner != battle.defenderPlayer)
    {
        verdict = "participant_mismatch";
    }
    else if (resultDefender != UINT32_MAX && resultDefender != battle.defenderUnit)
    {
        // The handler replaced the defender slot (0x10000 route): another unit won.
        verdict = "defender_slot_replaced";
    }
    else if (!defeat.liveBefore || !defeat.deadAfter)
    {
        verdict = "no_live_to_dead_transition";
    }
    else if (!militiaRetained(battle, defeat.local))
    {
        verdict = "militia_or_city_not_retained";
    }
    else
    {
        battle.lastStandDone = true;
        result               = achievements::RecordLastStand(battle.attack, battle.defense);
    }
    trace("last_stand", battle, fmt::format("route={} verdict={} result={} liveBefore={} deadAfter={} killLocal={} winner={}/{}", route, verdict, resultName(result), defeat.liveBefore, defeat.deadAfter, defeat.local, defeat.winner, static_cast<int64_t>(resultDefender == UINT32_MAX ? -1 : resultDefender)));
}

// Queue lineage ---------------------------------------------------------

Submit* findSubmit(ThreadCombat& thread, uint32_t frame);

// A drain finished: the claimed record is consumed unless the native retry
// rolled R back to its serial, which keeps the same pending token.
void finishDrain(ThreadCombat& thread, uint32_t frame, uint32_t cursor)
{
    for (size_t i = 0; i < thread.tokenCount; ++i)
    {
        auto& token = thread.tokens[i];
        if ((token.stage == Stage::Draining || token.stage == Stage::Invoked) && token.drainRecord == frame + 80)
        {
            if (token.serial >= cursor)
            {
                token.stage       = Stage::Pending;
                token.drainRecord = 0;
                trace("queue_retry", token.battle, fmt::format("serial={} cursor={}", token.serial, cursor));
            }
            else
            {
                removeToken(thread, i);
                --i;
            }
        }
    }
}

Submit* findSubmit(ThreadCombat& thread, uint32_t frame)
{
    for (size_t i = thread.submitCount; i > 0; --i)
    {
        if (thread.submits[i - 1].frame == frame)
        {
            if (thread.submitCount != i)
            {
                uint32_t   write = 0;
                uint32_t   read  = 0;
                const bool known = readQueue(write, read);
                for (size_t j = i; j < thread.submitCount; ++j)
                {
                    finishDrain(thread, thread.submits[j].frame, known ? read : UINT32_MAX);
                }
                thread.submitCount = i;
            }
            return &thread.submits[i - 1];
        }
    }
    return nullptr;
}

// A pending token must stay inside R <= serial < W of the live queue.
void validatePending(ThreadCombat& thread, uint32_t write, uint32_t read)
{
    for (size_t i = 0; i < thread.tokenCount; ++i)
    {
        if (thread.tokens[i].stage == Stage::Pending && (thread.tokens[i].serial < read || thread.tokens[i].serial >= write))
        {
            invalidate(thread, i, "queue_reset_or_unclaimed_drain");
            --i;
        }
    }
}

void queueBeginSubmit(uint32_t frame, uint32_t queue) noexcept
{
    try
    {
        if (queue != kQueue)
        {
            return;
        }
        auto& thread = state();
        // Every submit frame is tracked: a pending token may be claimed by a later, outer drain.
        findSubmit(thread, frame);
        for (size_t i = 0; i < thread.submitCount; ++i)
        {
            if (thread.submits[i].frame == frame)
            {
                thread.submitCount = i;
                break;
            }
        }
        if (thread.submitCount < kSubmits)
        {
            thread.submits[thread.submitCount++] = { frame, queue, 0, false };
        }
    }
    catch (...)
    {
    }
}

void queueAppend(uint32_t frame, uint32_t queue, uint32_t record, const QueueWords& words, uint32_t write, uint32_t read) noexcept
{
    try
    {
        if (queue != kQueue)
        {
            return;
        }
        auto& thread = state();
        if (thread.tokenCount == 0)
        {
            return;
        }
        // The append overwrites slot write & 31, the slot of serial write - 32.
        for (size_t i = 0; i < thread.tokenCount; ++i)
        {
            const auto& token = thread.tokens[i];
            if (token.stage != Stage::Open && write >= token.serial && write - token.serial >= 32)
            {
                invalidate(thread, i, "slot_overwritten");
                --i;
            }
        }
        const auto* submit = findSubmit(thread, frame);
        if (!submit || write < read || write - read >= 32 || write == UINT32_MAX)
        {
            return;
        }
        for (size_t i = thread.tokenCount; i > 0; --i)
        {
            auto& token = thread.tokens[i - 1];
            if (token.stage == Stage::Open && token.emitting && token.wrapper == record && token.words == words)
            {
                token.queue  = submit->queue;
                token.serial = write;
                token.stage  = Stage::Appending;
                trace("queue_append", token.battle, fmt::format("serial={} words={}", write, queueWords(words)));
                return;
            }
        }
    }
    catch (...)
    {
    }
}

void queueCompleteAppend(uint32_t frame) noexcept
{
    try
    {
        auto& thread = state();
        if (thread.tokenCount == 0)
        {
            return;
        }
        const auto* submit = findSubmit(thread, frame);
        if (!submit)
        {
            return;
        }
        // The append makes no calls. The record is the queue caller's frame + 0x50.
        for (size_t i = thread.tokenCount; i > 0; --i)
        {
            auto& token = thread.tokens[i - 1];
            if (token.stage == Stage::Appending && token.queue == submit->queue && token.wrapper == frame + kQueueFrame + 80)
            {
                token.stage = Stage::Pending;
                return;
            }
        }
    }
    catch (...)
    {
    }
}

void queueDrainSerial(uint32_t frame, uint32_t queue, uint32_t read, uint32_t write) noexcept
{
    try
    {
        if (queue != kQueue)
        {
            return;
        }
        auto& thread = state();
        if (auto* submit = findSubmit(thread, frame))
        {
            finishDrain(thread, frame, read);
            submit->read      = read;
            submit->validRead = write > read && write - read <= 32;
            validatePending(thread, write, read);
        }
    }
    catch (...)
    {
    }
}

void queueDrainRecord(uint32_t frame, uint32_t record, const QueueWords& words) noexcept
{
    try
    {
        auto& thread = state();
        if (thread.tokenCount == 0)
        {
            return;
        }
        auto* submit = findSubmit(thread, frame);
        if (!submit || !submit->validRead || record != frame + 80)
        {
            return;
        }
        submit->validRead = false;
        for (size_t i = thread.tokenCount; i > 0; --i)
        {
            auto& token = thread.tokens[i - 1];
            if (token.stage == Stage::Pending && token.queue == submit->queue && token.serial == submit->read && token.words == words)
            {
                token.stage       = Stage::Draining;
                token.drainRecord = record;
                trace("queue_drain", token.battle, fmt::format("serial={} record=0x{:08X} words={}", token.serial, record, queueWords(words)));
                return;
            }
        }
    }
    catch (...)
    {
    }
}

void queueEndSubmit(uint32_t frame) noexcept
{
    try
    {
        auto& thread = state();
        if (thread.submitCount == 0)
        {
            return;
        }
        if (findSubmit(thread, frame))
        {
            uint32_t write = 0;
            uint32_t read  = 0;
            if (readQueue(write, read))
            {
                finishDrain(thread, frame, read);
                validatePending(thread, write, read);
            }
            else
            {
                finishDrain(thread, frame, UINT32_MAX);
            }
            --thread.submitCount;
        }
    }
    catch (...)
    {
    }
}

const grace::QueueObserver kQueueObserver{ &queueBeginSubmit, &queueAppend, &queueCompleteAppend, &queueDrainSerial, &queueDrainRecord, &queueEndSubmit };

// Registers with the capital grace queue hooks, which own those addresses.
[[maybe_unused]] const bool kQueueObserverRegistered = (grace::QueueObserverSlot().store(&kQueueObserver, std::memory_order_release), true);

} // namespace

namespace rerevved::custom_achievement_combat
{

void EnterResolver(uint32_t frame, int32_t attackerPlayer, uint32_t attackerUnit) noexcept
{
    try
    {
        if (!supportedProfile())
        {
            return;
        }
        auto& thread = state();
        for (size_t i = 0; i < thread.scopeCount; ++i)
        {
            if (thread.scopes[i].frame == frame)
            {
                thread.scopeCount = i; // a stale scope at the same frame is never revisited
                break;
            }
        }
        if (thread.scopeCount == kScopes)
        {
            return;
        }
        Scope scope{};
        scope.frame                 = frame;
        scope.battle.frame          = frame;
        scope.battle.attackerPlayer = attackerPlayer;
        scope.battle.attackerUnit   = attackerUnit;

        thread.scopes[thread.scopeCount++] = scope;
    }
    catch (...)
    {
    }
}

void LeaveResolver(uint32_t frame, uint32_t result) noexcept
{
    try
    {
        auto& thread = state();
        for (size_t i = thread.scopeCount; i > 0; --i)
        {
            if (thread.scopes[i - 1].frame != frame)
            {
                continue;
            }
            const Scope scope = thread.scopes[i - 1];
            thread.scopeCount = i - 1;

            // Tokens still waiting inside this invocation's own submit never reach a queue.
            for (size_t t = 0; t < thread.tokenCount; ++t)
            {
                if (thread.tokens[t].emitting && thread.tokens[t].emitFrame == frame)
                {
                    thread.tokens[t].emitting = false;
                    if (thread.tokens[t].stage == Stage::Open || thread.tokens[t].stage == Stage::Appending)
                    {
                        invalidate(thread, t, "resolver_returned_before_queue");
                        --t;
                    }
                }
            }
            if (scope.armed)
            {
                trace("resolver_exit", scope.battle, fmt::format("result={}", result));
            }
            return;
        }
    }
    catch (...)
    {
    }
}

void ResetCombatObservers() noexcept
{
    generation.fetch_add(1, std::memory_order_relaxed);
}

bool IsBypassKill(uint32_t lr) noexcept
{
    for (const auto& site : kBypassSites)
    {
        if (site.lr == lr)
        {
            return true;
        }
    }
    return false;
}

void ObserveBypassKill(uint32_t lr, int32_t player, uint32_t unit, int32_t mode, int32_t local, int32_t launcher) noexcept
{
    try
    {
        // A nuclear strike is participation when the local player launched it or lost the unit.
        const bool involved = mode == 0 && local >= 0 && (player == local || launcher == local);
        if (involved)
        {
            achievements::RecordBattle();
        }
        if (!tracing())
        {
            return;
        }
        const char* name = "unknown";
        for (const auto& site : kBypassSites)
        {
            if (site.lr == lr)
            {
                name = site.name;
            }
        }
        UnitView   view;
        const bool read = readUnit(player, unit, view);
        REXLOG_INFO("Custom achievement trace: combat bypass route={} lr=0x{:08X} mode={} loser={}/{} loser_local={} launcher={} local={} unit_read={} owner={} type={} flags=0x{:08X} tile={},{} battle_recorded={}",
                    name,
                    lr,
                    mode,
                    player,
                    unit,
                    local >= 0 && player == local,
                    launcher,
                    local,
                    read,
                    view.owner,
                    view.type,
                    view.flags,
                    view.x,
                    view.y,
                    involved);
    }
    catch (...)
    {
    }
}

void ObserveAttackerDefeat(const Defeat& defeat) noexcept
{
    try
    {
        if (defeat.lr != kSameFrameLr && defeat.lr != kResultLr)
        {
            return;
        }
        auto& thread = state();
        if (defeat.lr == kSameFrameLr)
        {
            // Same-frame special: the resolver's own frame carries the receipt.
            auto* scope = findScope(thread, defeat.frame);
            if (!scope || !scope->armed)
            {
                return;
            }
            scope->battle.local = defeat.local;
            awardLastStand(scope->battle, "same_frame", defeat, UINT32_MAX);
            return;
        }
        // Ordinary: the result handler runs below the dispatcher frame that invoked it.
        for (size_t i = 0; i < thread.tokenCount; ++i)
        {
            auto& token = thread.tokens[i];
            if (token.stage != Stage::Invoked || token.dispatchFrame != defeat.frame + kResultFrame)
            {
                continue;
            }
            token.battle.local = defeat.local;
            // Frame word 124 of the handler holds the normalized defender slot,
            // stored after the replacement loop. An unreadable word never matches.
            uint32_t slot = 0;
            awardLastStand(token.battle, "queued_result", defeat, readWord(defeat.frame + kResultDefenderUnit, slot) ? slot : UINT32_MAX - 1);
            // Consuming the record prevents a native retry from awarding twice.
            token.battle.lastStandDone = true;
            return;
        }
    }
    catch (...)
    {
    }
}

} // namespace rerevved::custom_achievement_combat

// The resolver wrapper scopes each mode 0 invocation so a token never outlives
// its frame. Modes 1 and 2 (prediction, evaluation) are not tracked.
REX_HOOK_RAW(sub_82CD9970)
{
    const bool     tracked = ctx.r5.u32 == 0;
    const uint32_t frame   = ctx.r1.u32 - kResolverFrame;
    if (tracked)
    {
        combat::EnterResolver(frame, ctx.r3.s32, ctx.r4.u32);
    }
    __imp__sub_82CD9970(ctx, base);
    if (tracked)
    {
        combat::LeaveResolver(frame, ctx.r3.u32);
    }
}

// Final operative pair. Mode 0 with live, opposed participants.
void ArmCustomAchievementCombat(PPCRegister& stack)
{
    try
    {
        auto& thread = state();
        auto* scope  = findScope(thread, stack.u32);
        if (!scope)
        {
            return;
        }
        Frame frame;
        if (!readFrame(stack.u32, frame))
        {
            return;
        }
        uint32_t turn         = 0;
        Battle   battle       = scope->battle;
        battle.id             = nextId.fetch_add(1, std::memory_order_relaxed);
        battle.frame          = stack.u32;
        battle.attackerPlayer = frame.attackerPlayer;
        battle.attackerUnit   = frame.attackerUnit;
        battle.defenderPlayer = frame.defenderPlayer;
        battle.defenderUnit   = frame.defenderUnit;
        battle.attack         = frame.attack;
        battle.defense        = frame.defense;
        battle.turn           = readWord(kTurn, turn) ? turn : UINT32_MAX;

        UnitView    attacker;
        UnitView    defender;
        const char* verdict = "armed";
        if (frame.mode != 0)
        {
            verdict = "not_mode0";
        }
        else if (frame.attackerPlayer == frame.defenderPlayer || !readUnit(frame.attackerPlayer, frame.attackerUnit, attacker) || !readUnit(frame.defenderPlayer, frame.defenderUnit, defender) ||
                 !liveUnit(attacker, frame.attackerPlayer) || !liveUnit(defender, frame.defenderPlayer))
        {
            verdict = "participants_not_live_opposed";
        }
        else
        {
            battle.militia = militiaInCity(battle);
            scope->armed   = true;
        }
        scope->battle = battle;
        trace("operative_pair", battle, fmt::format("verdict={} rng_pointer=0x{:08X} attacker_type={} defender_type={}", verdict, frame.rngPointer, attacker.type, defender.type));
    }
    catch (...)
    {
    }
}

// At the round loop's selection decision, the pair must still match the snapshot.
void ObserveCustomAchievementCombatDecision(PPCRegister& stack)
{
    try
    {
        auto& thread = state();
        if (thread.scopeCount == 0)
        {
            return;
        }
        auto* scope = findScope(thread, stack.u32);
        if (!scope || !scope->armed)
        {
            return;
        }
        uint32_t attack  = 0;
        uint32_t defense = 0;
        if (!readWord(stack.u32 + kAttackWeight, attack) || !readWord(stack.u32 + kDefenseWeight, defense) || static_cast<int32_t>(attack) != scope->battle.attack ||
            static_cast<int32_t>(defense) != scope->battle.defense)
        {
            if (scope->battle.stable)
            {
                scope->battle.stable = false;
                trace("decision_pair_changed", scope->battle, fmt::format("now={}/{}", static_cast<int32_t>(attack), static_cast<int32_t>(defense)));
            }
            scope->battle.decision = true;
            return;
        }
        if (!scope->battle.decision)
        {
            scope->battle.decision = true;
            trace("decision", scope->battle);
        }
    }
    catch (...)
    {
    }
}

// Generator state published for the same invocation: the battle is committed.
void ObserveCustomAchievementCombatRng(PPCRegister& stack, PPCRegister& rngBlock, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr)
{
    try
    {
        auto& thread = state();
        if (thread.scopeCount == 0)
        {
            return;
        }
        auto* scope = findScope(thread, stack.u32);
        if (!scope || !scope->armed || !scope->battle.decision || scope->battle.rng)
        {
            return;
        }
        uint32_t pointer = 0;
        if (rngBlock.u32 != kRngBlock || !readWord(stack.u32 + kRngPointer, pointer) || pointer != kRngBlock)
        {
            trace("rng_unexpected_block", scope->battle, fmt::format("r28=0x{:08X} frame200=0x{:08X}", rngBlock.u32, pointer));
            return;
        }
        auto& battle         = scope->battle;
        battle.rng           = true;
        uint32_t   generator = 0;
        uint32_t   counter   = 0;
        const bool published = readWord(kRngBlock, generator) && readWord(kRngBlock + 4, counter);
        battle.local         = localPlayer(stack, threadPointer, fpscr);
        const bool involved  = battle.local >= 0 && (battle.local == battle.attackerPlayer || battle.local == battle.defenderPlayer);
        if (involved && !battle.battleCounted)
        {
            battle.battleCounted = true;
            achievements::RecordBattle();
        }
        trace("rng_published", battle, fmt::format("rng_read={} rng_state=0x{:08X} rng_counter={} local_involved={}", published, generator, counter, involved));
    }
    catch (...)
    {
    }
}

// The resolver is about to submit its result record. A Militia defense keeps
// the record for the ordinary receipt. The same-frame arm never emits.
void BeginCustomAchievementCombatEmit(PPCRegister& stack, PPCRegister& type, PPCRegister& player, PPCRegister& composite, PPCRegister& third, PPCRegister& fourth)
{
    try
    {
        auto& thread = state();
        if (thread.scopeCount == 0)
        {
            return;
        }
        auto* scope = findScope(thread, stack.u32);
        if (!scope || !scope->armed)
        {
            return;
        }
        const QueueWords words{ type.u32, player.u32, composite.u32, third.u32, fourth.u32 };
        const auto&      battle         = scope->battle;
        uint32_t         defenderPlayer = 0;
        uint32_t         defenderUnit   = 0;
        const bool       frameRead      = readWord(stack.u32 + kDefenderPlayer, defenderPlayer) && readWord(stack.u32 + kDefenderUnit, defenderUnit);
        const char*      verdict        = "tracked";
        if (!battle.militia || battle.lastStandDone)
        {
            verdict = "no_pending_receipt";
        }
        else if (!battle.decision || !battle.rng)
        {
            verdict = "resolution_not_committed";
        }
        else if ((words[0] != 1 && words[0] != 42) || words[1] != static_cast<uint32_t>(battle.attackerPlayer) || (words[2] & 0xFF) != battle.attackerUnit || (words[2] >> 8) != battle.defenderUnit)
        {
            verdict = "record_not_attacker_result";
        }
        else if (!frameRead || static_cast<int32_t>(defenderPlayer) != battle.defenderPlayer || defenderUnit != battle.defenderUnit)
        {
            verdict = "defender_replaced";
        }
        else if (thread.tokenCount == kTokens)
        {
            verdict = "token_capacity";
        }
        else
        {
            Token token{};
            token.battle                       = battle;
            token.words                        = words;
            token.emitFrame                    = stack.u32;
            token.emitting                     = true;
            token.wrapper                      = stack.u32 - kSubmitFrame + 80;
            thread.tokens[thread.tokenCount++] = token;
        }
        trace("emit", battle, fmt::format("verdict={} words={}", verdict, queueWords(words)));
    }
    catch (...)
    {
    }
}

void EndCustomAchievementCombatEmit(PPCRegister& stack)
{
    try
    {
        auto& thread = state();
        for (size_t i = 0; i < thread.tokenCount; ++i)
        {
            auto& token = thread.tokens[i];
            if (!token.emitting || token.emitFrame != stack.u32)
            {
                continue;
            }
            token.emitting = false;
            // An unqueued record (such as a networked submit) never reaches a drain.
            if (token.stage == Stage::Open || token.stage == Stage::Appending)
            {
                invalidate(thread, i, "submit_returned_without_queue");
                --i;
            }
        }
    }
    catch (...)
    {
    }
}

// The shared command 1/42 handler is invoked for the record claimed by this
// frame's drain. The dispatcher's r1 + 368 is the queue frame holding it.
void BeginCustomAchievementCombatResult(PPCRegister& stack, PPCRegister& player, PPCRegister& composite, PPCRegister& third)
{
    try
    {
        auto& thread = state();
        if (thread.tokenCount == 0)
        {
            return;
        }
        const uint32_t record = stack.u32 + kDispatchFrame + 80;
        for (size_t i = 0; i < thread.tokenCount; ++i)
        {
            auto& token = thread.tokens[i];
            if (token.stage == Stage::Draining && token.drainRecord == record && token.words[1] == player.u32 && token.words[2] == composite.u32 && token.words[3] == third.u32)
            {
                token.stage         = Stage::Invoked;
                token.dispatchFrame = stack.u32;
                trace("result_invoked", token.battle, fmt::format("record=0x{:08X} dispatch=0x{:08X}", record, stack.u32));
                return;
            }
        }
    }
    catch (...)
    {
    }
}

void EndCustomAchievementCombatResult(PPCRegister& stack, PPCRegister& result)
{
    try
    {
        auto& thread = state();
        for (size_t i = 0; i < thread.tokenCount; ++i)
        {
            auto& token = thread.tokens[i];
            if (token.stage == Stage::Invoked && token.dispatchFrame == stack.u32)
            {
                // The claimed drain decides whether to roll back for a retry when it ends.
                token.stage         = Stage::Draining;
                token.dispatchFrame = 0;
                token.handlerResult = result.s32;
                trace("result_returned", token.battle, fmt::format("handler_result={}", result.s32));
                return;
            }
        }
    }
    catch (...)
    {
    }
}
