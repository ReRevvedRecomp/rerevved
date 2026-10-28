#include "custom_achievement_event_hooks.h"

#include "ai_civilizations.h"
#include "custom_achievements.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(bool, custom_achievement_trace);

REX_EXTERN(__imp__sub_82D8A3A0);
REX_EXTERN(__imp__sub_82D1B400);

// Evidence: rerevved-research manifests/achievement-event-boundaries.json
// RVA-F-0354 (production), RVA-F-0355 (founding), RVA-F-0356 (ICBM),
// RVA-F-0357 (Templar), RVA-F-0361 (capture). Every receipt is conditional
// static evidence; the trace lines are the runtime acceptance record.
namespace
{

namespace achievements = rerevved::custom_achievements;
namespace ai_civs      = rerevved::ai_civilizations;
using achievements::Tile;

constexpr uint32_t kTurn          = 0x8312B8DC;
constexpr uint32_t kYear          = 0x8312B8E0;
constexpr uint32_t kDifficulty    = 0x82FFE7C8;
constexpr uint32_t kCities        = 0x8314FE28;
constexpr uint32_t kCityStride    = 188;
constexpr uint32_t kArtifacts     = 0x8312A608;
constexpr uint32_t kIcbmSelector  = 0x8314F1AC;
constexpr int32_t  kIcbmType      = 27;
constexpr uint32_t kArtifactBit   = 0x400;
constexpr uint32_t kTemplarCase   = 4;
constexpr int32_t  kPlayerLimit   = 8;
constexpr int32_t  kCityLimit     = 128;
constexpr int32_t  kMapStride     = 40;
constexpr uint32_t kProductionArg = 0x74; // CityUpdate frame word holding the city byte offset

struct NativeWord
{
    uint32_t address;
    uint32_t value;
};

// Instruction each hook precedes. The founding address belongs to
// ApplyCityFoundingUnit and the capture guard to capital grace.
constexpr NativeWord kProfile[] = {
    { 0x82D15AC0, 0x7D7588AE }, // lbzx r11,r21,r17
    { 0x82D17C50, 0x3D60830F }, // lis r11,-31985
    { 0x82D18BE4, 0x7D0B532E }, // sthx r8,r11,r10
    { 0x82CDEE84, 0x48028855 }, // bl 0x82D076D8
    { 0x82CDEE90, 0x2F0B0000 }, // cmpwi cr6,r11,0
    { 0x82CDEEAC, 0x48002800 }, // b 0x82CE16AC
    { 0x82CDF1C4, 0x480231CD }, // bl 0x82D02390
    { 0x82CF7B60, 0x1D6900BC }, // mulli r11,r9,188
    { 0x82CF7D58, 0x4800A639 }, // bl 0x82D02390
    { 0x82D1B460, 0x7D78502E }, // lwzx r11,r24,r10
};

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

uint16_t be16(const uint8_t* bytes) noexcept
{
    return static_cast<uint16_t>((uint16_t{ bytes[0] } << 8) | bytes[1]);
}

int32_t bes16(const uint8_t* bytes) noexcept
{
    return std::bit_cast<int16_t>(be16(bytes));
}

uint32_t be32(const uint8_t* bytes) noexcept
{
    return (uint32_t{ bytes[0] } << 24) | (uint32_t{ bytes[1] } << 16) | (uint32_t{ bytes[2] } << 8) | bytes[3];
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

bool readHalf(uint32_t address, uint32_t& value) noexcept
{
    uint8_t bytes[2];
    if (!readBytes(address, 2, bytes))
    {
        return false;
    }
    value = be16(bytes);
    return true;
}

bool tracing() noexcept
{
    return REXCVAR_GET(custom_achievement_trace);
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
                try
                {
                    REXLOG_WARN("Custom achievement events are unavailable: native word at 0x{:08X} is 0x{:08X}, expected 0x{:08X}", expected.address, actual, expected.value);
                }
                catch (...)
                {
                }
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

// Local player through the native accessor, or -1 when unknown. The call
// context is private to the caller. Session word 0 means no session.
int32_t invokeLocalPlayer(PPCContext& call)
{
    uint32_t session = 0;
    auto*    memory  = REX_KERNEL_MEMORY();
    if (!memory || !readWord(ai_civs::kSessionGlobal, session) || session == 0)
    {
        return -1;
    }
    call.r3.u64 = session;
    call.r4.u64 = 0;
    call.r5.u64 = 0;
    HostFloatScope scope;
    __imp__sub_82D8A3A0(call, memory->virtual_membase());
    return call.r3.s32;
}

// From a midasm hook: a private ABI linkage area below the live frame keeps
// the accessor's spills clear of the hooked function (as GuestWorld::Create in
// city_founding_unit_hooks.cpp).
int32_t localPlayer(PPCRegister& stack, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr)
{
    if (stack.u32 < 0x10070 || !accessible(stack.u32 - 0x70, 0x70, true))
    {
        return -1;
    }
    PPCContext call{};
    call.r1.u64               = stack.u32 - 0x70;
    call.r13                  = threadPointer;
    call.fpscr                = fpscr;
    const uint8_t backChain[] = { static_cast<uint8_t>(stack.u32 >> 24), static_cast<uint8_t>(stack.u32 >> 16), static_cast<uint8_t>(stack.u32 >> 8), static_cast<uint8_t>(stack.u32) };
    std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(call.r1.u32), backChain, sizeof(backChain));
    return invokeLocalPlayer(call);
}

struct UnitView
{
    int32_t  owner = -1;
    int32_t  type  = -1;
    uint32_t flags = 0;
    Tile     tile;
    uint32_t slot   = 0;
    uint32_t object = 0;
};

bool readUnit(int32_t player, uint32_t slot, UnitView& unit) noexcept
{
    uint8_t bytes[0x40];
    if (player < 0 || player >= kPlayerLimit || slot >= ai_civs::kUnitsPerPlayer || !readBytes(ai_civs::UnitRecord(static_cast<uint32_t>(player), slot), sizeof(bytes), bytes))
    {
        return false;
    }
    unit.owner  = bytes[0];
    unit.type   = bytes[1];
    unit.flags  = be32(bytes + ai_civs::kUnitFlagsOffset);
    unit.tile   = { bes16(bytes + 0x1C), bes16(bytes + 0x1E) };
    unit.slot   = be16(bytes + 0x30);
    unit.object = be32(bytes + 0x3C);
    return true;
}

// Invocation-scoped records. Each is keyed by the hooked function's frame (or
// the entry arguments) so nested or interleaved calls never share a cache.
struct TemplarBinding
{
    uint32_t frame  = 0;
    int32_t  player = -1;
    int32_t  x      = -1;
    int32_t  y      = -1;
};

struct IcbmLaunch
{
    uint32_t frame  = 0;
    int32_t  player = -1;
    uint32_t slot   = 0;
    Tile     source;
    Tile     target;
    uint32_t flagsBefore    = 0;
    uint32_t selectorBefore = 0;
    uint32_t selectorActual = 0;
    bool     selectorSeen   = false;
    bool     earlySdi       = false;
    bool     awarded        = false;
};

struct CityTransfer
{
    int32_t city     = -1;
    int32_t newOwner = -1;
    int32_t oldOwner = -1;
    bool    armed    = false;
};

struct ThreadEvents
{
    rex::runtime::ThreadState*    thread = nullptr;
    std::array<TemplarBinding, 8> templar{};
    size_t                        templarCount = 0;
    std::array<IcbmLaunch, 4>     icbm{};
    size_t                        icbmCount = 0;
    std::array<CityTransfer, 4>   transfers{};
    size_t                        transferDepth = 0;
};

thread_local ThreadEvents threadEvents;

ThreadEvents& events() noexcept
{
    auto* current = rex::runtime::ThreadState::Get();
    if (threadEvents.thread != current)
    {
        threadEvents        = ThreadEvents{};
        threadEvents.thread = current;
    }
    return threadEvents;
}

template <class T, size_t N>
void pushBounded(std::array<T, N>& items, size_t& count, const T& value) noexcept
{
    if (count == N)
    {
        for (size_t i = 1; i < N; ++i)
        {
            items[i - 1] = items[i];
        }
        --count;
    }
    items[count++] = value;
}

template <class T, size_t N>
void eraseAt(std::array<T, N>& items, size_t& count, size_t index) noexcept
{
    for (size_t i = index + 1; i < count; ++i)
    {
        items[i - 1] = items[i];
    }
    --count;
}

bool validPlayer(int32_t player) noexcept
{
    return player >= 0 && player < kPlayerLimit;
}

// ---- Unit production (RVA-F-0354) -----------------------------------------

// Null when the output is retained and live: owner/type/slot match, dead bit
// clear and the object back-pointer names the record. Ships may sit on a
// neighbor tile, so coordinates are not compared.
const char* productionOutput(int32_t owner, int32_t item, int32_t slot) noexcept
{
    if (slot < 0 || slot >= static_cast<int32_t>(ai_civs::kUnitsPerPlayer))
    {
        return "slot_range";
    }
    UnitView unit;
    if (!readUnit(owner, static_cast<uint32_t>(slot), unit))
    {
        return "unit_unreadable";
    }
    if (unit.owner != owner || unit.type != item || unit.slot != static_cast<uint32_t>(slot))
    {
        return "unit_mismatch";
    }
    if ((unit.flags & ai_civs::kUnitKilledFlag) != 0)
    {
        return "unit_dead";
    }
    uint32_t back = 0;
    if (unit.object < 0x10000 || unit.object > UINT32_MAX - 0x88 || !readWord(unit.object + 0x84, back) || back != ai_civs::UnitRecord(static_cast<uint32_t>(owner), static_cast<uint32_t>(slot)))
    {
        return "object_unbound";
    }
    return nullptr;
}

// ---- City transfer (RVA-F-0361) -------------------------------------------

class TransferScope
{
public:
    TransferScope(int32_t city, int32_t newOwner) noexcept
    {
        auto& e = events();
        index_  = e.transferDepth++;
        if (index_ < e.transfers.size())
        {
            e.transfers[index_] = { city, newOwner, -1, false };
        }
    }

    ~TransferScope()
    {
        auto& e = events();
        if (e.transferDepth > 0)
        {
            --e.transferDepth;
        }
    }

    bool Pending(CityTransfer& out) const noexcept
    {
        auto& e = events();
        if (index_ >= e.transfers.size() || e.transferDepth != index_ + 1)
        {
            return false;
        }
        out = e.transfers[index_];
        return true;
    }

    TransferScope(const TransferScope&)            = delete;
    TransferScope& operator=(const TransferScope&) = delete;

private:
    size_t index_ = 0;
};

void finishCityTransfer(const CityTransfer& pending, PPCContext& caller)
{
    try
    {
        uint8_t owner = 0;
        if (!readBytes(kCities + kCityStride * static_cast<uint32_t>(pending.city), 1, &owner))
        {
            if (tracing())
            {
                REXLOG_INFO("Custom achievement trace: capital capture city={} old={} new={} unreadable_after_return", pending.city, pending.oldOwner, pending.newOwner);
            }
            return;
        }
        const bool sameCity = static_cast<int8_t>(owner) == static_cast<int8_t>(pending.newOwner);
        int32_t    local    = -1;
        if (sameCity)
        {
            PPCContext call = caller;
            local           = invokeLocalPlayer(call);
        }
        uint32_t   year       = 0;
        uint32_t   difficulty = 0;
        uint8_t    network    = 0;
        const bool readable   = readWord(kYear, year) && readWord(kDifficulty, difficulty) && readBytes(ai_civs::kNetworkGameByte, 1, &network);
        auto       result     = achievements::AwardResult::NotQualified;
        if (sameCity && local == pending.newOwner && readable)
        {
            result = achievements::RecordCapitalCapture(std::bit_cast<int32_t>(year), std::bit_cast<int32_t>(difficulty), network != 0);
        }
        if (tracing())
        {
            REXLOG_INFO("Custom achievement trace: capital capture city={} old={} new={} owner_after={} same_city={} local={} year={} difficulty={} network={} readable={} result={}",
                        pending.city,
                        pending.oldOwner,
                        pending.newOwner,
                        static_cast<int>(static_cast<int8_t>(owner)),
                        sameCity,
                        local,
                        std::bit_cast<int32_t>(year),
                        std::bit_cast<int32_t>(difficulty),
                        static_cast<int>(network),
                        readable,
                        resultName(result));
        }
    }
    catch (...)
    {
    }
}

// ---- Intercontinental missile (RVA-F-0356) --------------------------------

IcbmLaunch* findIcbm(ThreadEvents& e, int32_t player, uint32_t slot, const uint32_t* frame) noexcept
{
    for (size_t i = e.icbmCount; i-- > 0;)
    {
        auto& launch = e.icbm[i];
        if (launch.player == player && launch.slot == slot && (!frame || launch.frame == *frame))
        {
            return &launch;
        }
    }
    return nullptr;
}

void eraseIcbm(ThreadEvents& e, const IcbmLaunch* launch) noexcept
{
    eraseAt(e.icbm, e.icbmCount, static_cast<size_t>(launch - e.icbm.data()));
}

bool sameTile(Tile a, int32_t x, int32_t y) noexcept
{
    return a.x == x && a.y == y;
}

// Awards the original pre-effect snapshot at a consumption receipt. The
// snapshot stays until the dispatcher returns.
void awardIcbm(IcbmLaunch& launch, const char* receipt, int32_t targetX, int32_t targetY)
{
    auto result = achievements::AwardResult::NotQualified;
    bool match  = sameTile(launch.target, targetX, targetY);
    if (match && !launch.earlySdi && !launch.awarded)
    {
        launch.awarded = true;
        result         = achievements::RecordIcbmLaunch(launch.source, launch.target);
    }
    if (tracing())
    {
        REXLOG_INFO("Custom achievement trace: icbm receipt={} player={} slot={} source=({},{}) target=({},{}) receipt_target=({},{}) selector_before={} selector_actual={} early_sdi={} distance={} result={}",
                    receipt,
                    launch.player,
                    launch.slot,
                    launch.source.x,
                    launch.source.y,
                    launch.target.x,
                    launch.target.y,
                    targetX,
                    targetY,
                    launch.selectorBefore,
                    launch.selectorSeen ? static_cast<int64_t>(launch.selectorActual) : -1,
                    launch.earlySdi,
                    achievements::TileDistance(launch.source, launch.target),
                    resultName(result));
    }
}

} // namespace

// ---- Founding (RVA-F-0355) ------------------------------------------------

// Accepted saved origin (Settle12 or accepted AI founding), live city owner
// == r20 == local player and matching coordinates. The turn is read before
// the read-only local accessor call and before any creator call.
void ObserveCustomAchievementCityFounded(PPCRegister& stack, PPCRegister& citySlot, PPCRegister& owner, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr)
{
    try
    {
        const char* reason    = "admitted";
        uint32_t    turn      = 0;
        const bool  turnRead  = readWord(kTurn, turn);
        uint32_t    origin    = 0;
        uint32_t    wordX     = 0;
        uint32_t    wordY     = 0;
        int32_t     cityOwner = -1;
        int32_t     cityX     = -1;
        int32_t     cityY     = -1;
        int32_t     local     = -1;

        const uint32_t frame = stack.u32;
        if (!supportedProfile())
        {
            reason = "profile";
        }
        else if (frame < 0x10000 || (frame & 0xF) != 0 || frame > UINT32_MAX - 0x1A8 || !readWord(frame + 0x178, origin) || !readWord(frame + 0x19C, wordX) || !readWord(frame + 0x1A4, wordY))
        {
            reason = "frame";
        }
        else if (origin != 0x82CE03AC && origin != 0x82CBC040)
        {
            reason = "origin";
        }
        else if (citySlot.s32 < 0 || citySlot.s32 >= kCityLimit || !validPlayer(owner.s32))
        {
            reason = "inputs";
        }
        else
        {
            uint8_t city[0x34];
            if (!readBytes(kCities + kCityStride * citySlot.u32, sizeof(city), city))
            {
                reason = "city_unreadable";
            }
            else
            {
                cityOwner = static_cast<int8_t>(city[0]);
                cityX     = bes16(city + 0x30);
                cityY     = bes16(city + 0x32);
                if (city[0] == 0xFF || cityOwner != owner.s32 || cityX != std::bit_cast<int32_t>(wordX) || cityY != std::bit_cast<int32_t>(wordY))
                {
                    reason = "city_mismatch";
                }
                else if (!turnRead || std::bit_cast<int32_t>(turn) < 0)
                {
                    reason = "turn";
                }
                else
                {
                    local = localPlayer(stack, threadPointer, fpscr);
                    if (local != owner.s32)
                    {
                        reason = local < 0 ? "local_unknown" : "not_local";
                    }
                    else
                    {
                        achievements::RecordCityFounded(std::bit_cast<int32_t>(turn));
                    }
                }
            }
        }
        if (tracing())
        {
            REXLOG_INFO("Custom achievement trace: city founded decision={} frame=0x{:08X} origin=0x{:08X} city={} owner={} x={} y={} city_owner={} city_x={} city_y={} local={} turn_read={} turn={}",
                        reason,
                        frame,
                        origin,
                        citySlot.s32,
                        owner.s32,
                        std::bit_cast<int32_t>(wordX),
                        std::bit_cast<int32_t>(wordY),
                        cityOwner,
                        cityX,
                        cityY,
                        local,
                        turnRead,
                        std::bit_cast<int32_t>(turn));
        }
    }
    catch (...)
    {
    }
}

// ---- Unit production (RVA-F-0354) -----------------------------------------

// CityUpdate after the creator returns a nonnegative slot for ordinary,
// alternate ship or gold rush completions. Any completion for the local owner
// counts unless provably absent, preventing an unearned no-build achievement.
void ObserveCustomAchievementUnitBuilt(PPCRegister& stack, PPCRegister& owner, PPCRegister& item, PPCRegister& slot, PPCRegister& cityBase, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr)
{
    bool otherPlayer = false;
    try
    {
        if (!supportedProfile())
        {
            return;
        }
        const int32_t local = localPlayer(stack, threadPointer, fpscr);
        if (local >= 0 && local != owner.s32)
        {
            otherPlayer = true;
            return;
        }
        const char* reason = nullptr;
        uint32_t    offset = 0;
        uint8_t     city[0x34];
        if (!readWord(stack.u32 + kProductionArg, offset) || !readBytes(cityBase.u32 + offset, sizeof(city), city))
        {
            reason = "city_unreadable";
        }
        else if (static_cast<int8_t>(city[0]) != owner.s32)
        {
            reason = "city_changed";
        }
        else
        {
            reason = productionOutput(owner.s32, item.s32, slot.s32);
        }
        if (!reason && local < 0)
        {
            reason = "local_unknown";
        }
        achievements::RecordUnitBuilt();
        if (tracing())
        {
            REXLOG_INFO("Custom achievement trace: unit built owner={} item={} slot={} local={} city_offset={} retained={} reason={} recorded=true",
                        owner.s32,
                        item.s32,
                        slot.s32,
                        local,
                        offset,
                        reason == nullptr,
                        reason ? reason : "retained");
        }
    }
    catch (...)
    {
        // A missed completion could award a no-build achievement.
        try
        {
            if (!otherPlayer)
            {
                achievements::RecordUnitBuilt();
            }
        }
        catch (...)
        {
        }
    }
}

// ---- Knights Templar (RVA-F-0357) -----------------------------------------

// After the subtype helper 0x821C6548 returns in sub_82D17AE0 (r3 = selector).
// Replaces any record left by an earlier invocation at the same frame.
void BindCustomAchievementTemplarSelector(PPCRegister& stack, PPCRegister& selector, PPCRegister& player, PPCRegister& x, PPCRegister& y)
{
    try
    {
        auto& e = events();
        for (size_t i = e.templarCount; i-- > 0;)
        {
            if (e.templar[i].frame == stack.u32)
            {
                eraseAt(e.templar, e.templarCount, i);
            }
        }
        if (selector.u32 != kTemplarCase)
        {
            return;
        }
        pushBounded(e.templar, e.templarCount, TemplarBinding{ stack.u32, player.s32, x.s32, y.s32 });
        if (tracing())
        {
            REXLOG_INFO("Custom achievement trace: templar selector frame=0x{:08X} player={} x={} y={}", stack.u32, player.s32, x.s32, y.s32);
        }
    }
    catch (...)
    {
    }
}

// Before the artifact-flags store at the common tail: old bit 0x400 set and
// cleared by the store, same invocation as a selector-4 binding.
void ObserveCustomAchievementTemplarConsumption(PPCRegister& stack, PPCRegister& player, PPCRegister& x, PPCRegister& y, PPCRegister& oldFlags, PPCRegister& newFlags, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr)
{
    try
    {
        auto&  e     = events();
        size_t found = e.templarCount;
        for (size_t i = e.templarCount; i-- > 0;)
        {
            if (e.templar[i].frame == stack.u32)
            {
                found = i;
                break;
            }
        }
        if (found == e.templarCount)
        {
            return;
        }
        const TemplarBinding binding = e.templar[found];
        eraseAt(e.templar, e.templarCount, found);

        const uint32_t before   = oldFlags.u32 & 0xFFFF;
        const uint32_t after    = newFlags.u32 & 0xFFFF;
        uint32_t       stored   = 0;
        const bool     inMap    = x.s32 >= 0 && x.s32 < kMapStride && y.s32 >= 0 && y.s32 < kMapStride;
        const bool     tileRead = inMap && readHalf(kArtifacts + 2 * static_cast<uint32_t>(kMapStride * x.s32 + y.s32), stored);
        const char*    reason   = "admitted";
        int32_t        local    = -1;
        int32_t        year     = 0;
        auto           result   = achievements::AwardResult::NotQualified;
        if (!supportedProfile())
        {
            reason = "profile";
        }
        else if (binding.player != player.s32 || binding.x != x.s32 || binding.y != y.s32)
        {
            reason = "binding_mismatch";
        }
        else if (!tileRead || stored != before)
        {
            reason = "tile_state";
        }
        else if ((before & kArtifactBit) == 0 || (after & kArtifactBit) != 0)
        {
            reason = "artifact_bit";
        }
        else
        {
            uint32_t yearWord = 0;
            local             = localPlayer(stack, threadPointer, fpscr);
            if (local != player.s32)
            {
                reason = local < 0 ? "local_unknown" : "not_local";
            }
            else if (!readWord(kYear, yearWord))
            {
                reason = "year";
            }
            else
            {
                year   = std::bit_cast<int32_t>(yearWord);
                result = achievements::RecordTemplarDiscovery(year);
            }
        }
        if (tracing())
        {
            REXLOG_INFO("Custom achievement trace: templar consumption decision={} frame=0x{:08X} player={} x={} y={} old=0x{:04X} new=0x{:04X} stored=0x{:04X} local={} year={} result={}",
                        reason,
                        stack.u32,
                        player.s32,
                        x.s32,
                        y.s32,
                        before,
                        after,
                        stored,
                        local,
                        year,
                        resultName(result));
        }
    }
    catch (...)
    {
    }
}

// ---- Intercontinental missile (RVA-F-0356) --------------------------------

// Before the opaque helper 0x82D076D8 in the nuclear Move case of sub_82CDDF08
// (r31 actor, r15 unit slot, target x/y = r27/r26). Retains the live type-27
// source, its original position and the local owner for this invocation.
void CaptureCustomAchievementIcbm(PPCRegister& stack, PPCRegister& player, PPCRegister& slot, PPCRegister& targetX, PPCRegister& targetY, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr)
{
    try
    {
        auto& e = events();
        for (size_t i = e.icbmCount; i-- > 0;)
        {
            if (e.icbm[i].frame == stack.u32 || (e.icbm[i].player == player.s32 && e.icbm[i].slot == slot.u32))
            {
                eraseAt(e.icbm, e.icbmCount, i);
            }
        }
        if (!supportedProfile())
        {
            return;
        }
        UnitView      unit;
        const bool    read     = readUnit(player.s32, slot.u32, unit);
        const bool    live     = read && unit.owner == player.s32 && unit.type == kIcbmType && (unit.flags & ai_civs::kUnitKilledFlag) == 0;
        uint32_t      selector = 0;
        const bool    selected = readWord(kIcbmSelector, selector);
        const int32_t local    = live ? localPlayer(stack, threadPointer, fpscr) : -1;
        const bool    eligible = live && local == player.s32;
        if (eligible)
        {
            IcbmLaunch launch;
            launch.frame          = stack.u32;
            launch.player         = player.s32;
            launch.slot           = slot.u32;
            launch.source         = unit.tile;
            launch.target         = { targetX.s32, targetY.s32 };
            launch.flagsBefore    = unit.flags;
            launch.selectorBefore = selected ? selector : 0;
            pushBounded(e.icbm, e.icbmCount, launch);
        }
        if (tracing())
        {
            REXLOG_INFO("Custom achievement trace: icbm capture frame=0x{:08X} player={} slot={} unit_read={} type={} live={} local={} eligible={} source=({},{}) target=({},{}) selector_read={} selector={}",
                        stack.u32,
                        player.s32,
                        slot.u32,
                        read,
                        unit.type,
                        live,
                        local,
                        eligible,
                        unit.tile.x,
                        unit.tile.y,
                        targetX.s32,
                        targetY.s32,
                        selected,
                        selector);
        }
    }
    catch (...)
    {
    }
}

// The selector word the native code just loaded at 0x82CDEE8C (r11).
void NoteCustomAchievementIcbmSelector(PPCRegister& stack, PPCRegister& selector)
{
    try
    {
        auto& e = events();
        for (size_t i = e.icbmCount; i-- > 0;)
        {
            if (e.icbm[i].frame == stack.u32)
            {
                e.icbm[i].selectorActual = selector.u32;
                e.icbm[i].selectorSeen   = true;
                return;
            }
        }
    }
    catch (...)
    {
    }
}

// Mode-zero consumption: UnitKill(r31, r15, 0) of the source.
void ConsumeCustomAchievementIcbmModeZero(PPCRegister& stack, PPCRegister& player, PPCRegister& slot, PPCRegister& targetX, PPCRegister& targetY)
{
    try
    {
        auto&          e      = events();
        const uint32_t frame  = stack.u32;
        auto*          launch = findIcbm(e, player.s32, slot.u32, &frame);
        if (!launch)
        {
            if (tracing())
            {
                REXLOG_INFO("Custom achievement trace: icbm receipt=mode_zero player={} slot={} target=({},{}) snapshot=none", player.s32, slot.u32, targetX.s32, targetY.s32);
            }
            return;
        }
        awardIcbm(*launch, "mode_zero", targetX.s32, targetY.s32);
        eraseIcbm(e, launch);
    }
    catch (...)
    {
    }
}

// Early nonzero SDI arm: observed without awarding. The tuple allows runtime
// classification. The unit slot is the frame word at +0x11C.
void ObserveCustomAchievementIcbmEarlySdi(PPCRegister& stack, PPCRegister& player, PPCRegister& targetX, PPCRegister& targetY)
{
    try
    {
        uint32_t slot = 0;
        if (!readWord(stack.u32 + 0x11C, slot))
        {
            return;
        }
        auto&      e      = events();
        auto*      launch = findIcbm(e, player.s32, slot, nullptr);
        UnitView   now;
        const bool nowRead = readUnit(player.s32, slot, now);
        if (launch)
        {
            launch->earlySdi = true;
        }
        if (tracing())
        {
            REXLOG_INFO("Custom achievement trace: icbm arm=early_sdi player={} slot={} snapshot={} local={} selector_before={} selector_actual={} source_before=({},{}) flags_before=0x{:08X} source_now=({},{}) flags_now=0x{:08X} now_read={} target=({},{}) snapshot_target=({},{})",
                        player.s32,
                        slot,
                        launch != nullptr,
                        launch ? player.s32 : -1,
                        launch ? launch->selectorBefore : 0,
                        launch && launch->selectorSeen ? static_cast<int64_t>(launch->selectorActual) : -1,
                        launch ? launch->source.x : -1,
                        launch ? launch->source.y : -1,
                        launch ? launch->flagsBefore : 0,
                        now.tile.x,
                        now.tile.y,
                        now.flags,
                        nowRead,
                        targetX.s32,
                        targetY.s32,
                        launch ? launch->target.x : -1,
                        launch ? launch->target.y : -1);
        }
    }
    catch (...)
    {
    }
}

// Nonzero-mode consumption: UnitKill(r16, frame+0x11C, 0) of the source,
// bound to the snapshot taken before the helper.
void ConsumeCustomAchievementIcbmNonzero(PPCRegister& player, PPCRegister& slot, PPCRegister& targetX, PPCRegister& targetY)
{
    try
    {
        auto& e      = events();
        auto* launch = findIcbm(e, player.s32, slot.u32, nullptr);
        if (!launch)
        {
            if (tracing())
            {
                REXLOG_INFO("Custom achievement trace: icbm receipt=nonzero player={} slot={} target=({},{}) snapshot=none", player.s32, slot.u32, targetX.s32, targetY.s32);
            }
            return;
        }
        awardIcbm(*launch, "nonzero", targetX.s32, targetY.s32);
    }
    catch (...)
    {
    }
}

// Dispatcher return after the nonzero helper (r31 actor, r15 unit slot):
// logs the same-source state after the call and drops the snapshot.
void FinishCustomAchievementIcbmDispatch(PPCRegister& stack, PPCRegister& player, PPCRegister& slot)
{
    try
    {
        auto&          e      = events();
        const uint32_t frame  = stack.u32;
        auto*          launch = findIcbm(e, player.s32, slot.u32, &frame);
        if (!launch)
        {
            return;
        }
        UnitView   after;
        const bool read = readUnit(player.s32, slot.u32, after);
        if (tracing())
        {
            REXLOG_INFO("Custom achievement trace: icbm dispatch_return player={} slot={} early_sdi={} awarded={} selector_before={} selector_actual={} source_before=({},{}) flags_before=0x{:08X} source_after=({},{}) flags_after=0x{:08X} after_read={} killed_after={}",
                        launch->player,
                        launch->slot,
                        launch->earlySdi,
                        launch->awarded,
                        launch->selectorBefore,
                        launch->selectorSeen ? static_cast<int64_t>(launch->selectorActual) : -1,
                        launch->source.x,
                        launch->source.y,
                        launch->flagsBefore,
                        after.tile.x,
                        after.tile.y,
                        after.flags,
                        read,
                        (after.flags & ai_civs::kUnitKilledFlag) != 0);
        }
        eraseIcbm(e, launch);
    }
    catch (...)
    {
    }
}

// ---- Capital capture (RVA-F-0361) -----------------------------------------

// After the owner byte store in sub_82D1B400 (r16 city, r19 old owner, r18
// new owner, r21 + r26 the city record). Native flags are still pre-transfer:
// no call or store touches C+0x10 before this point. Arms only a Palace city.
void CaptureCustomAchievementCityTransfer(PPCRegister& city, PPCRegister& oldOwner, PPCRegister& newOwner, PPCRegister& cityBase, PPCRegister& cityOffset)
{
    try
    {
        auto& e = events();
        if (e.transferDepth == 0 || e.transferDepth > e.transfers.size())
        {
            return;
        }
        auto& pending      = e.transfers[e.transferDepth - 1];
        pending.armed      = false;
        pending.oldOwner   = oldOwner.s32;
        uint32_t   flags   = 0;
        const bool matches = pending.city == city.s32 && pending.newOwner == newOwner.s32 && city.s32 >= 0 && city.s32 < kCityLimit &&
                             cityBase.u32 == kCities && cityOffset.u32 == kCityStride * city.u32;
        const bool read    = matches && readWord(kCities + kCityStride * city.u32 + 0x10, flags);
        const bool palace  = read && (flags & 1) != 0;
        pending.armed      = palace && supportedProfile() && oldOwner.s32 != newOwner.s32 && validPlayer(oldOwner.s32) && validPlayer(static_cast<int8_t>(newOwner.u32));
        if (tracing())
        {
            REXLOG_INFO("Custom achievement trace: city transfer city={} old={} new={} entry_matches={} flags_read={} flags=0x{:08X} palace={} armed={}",
                        city.s32,
                        oldOwner.s32,
                        newOwner.s32,
                        matches,
                        read,
                        flags,
                        palace,
                        pending.armed);
        }
    }
    catch (...)
    {
    }
}

// The midasm hook records a Palace city transfer only when its owner changes.
// After the native function returns normally, the same city must belong to
// the new owner, the local player. The record is bound to this invocation.
REX_HOOK_RAW(sub_82D1B400)
{
    TransferScope scope(ctx.r3.s32, ctx.r4.s32);
    __imp__sub_82D1B400(ctx, base);

    CityTransfer pending;
    if (scope.Pending(pending) && pending.armed)
    {
        finishCityTransfer(pending, ctx);
    }
}
