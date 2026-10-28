#include "ai_civilizations.h"
#include "custom_achievements.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(bool, custom_achievement_trace);

REX_EXTERN(__imp__sub_82D8A3A0);

// Shallow Worldview: every committed movement segment of a local-player unit
// onto a deep Ocean tile. Evidence: rerevved-research manifests/
// achievement-event-boundaries.json RVA-F-0358 movementOriginCoverage and
// currentTerrainClassifier.

namespace
{

namespace achievements = rerevved::custom_achievements;
namespace ai_civs      = rerevved::ai_civilizations;

constexpr uint32_t kTerrainPlane = 0x8312B288;
constexpr uint32_t kTerrainBytes = 1600;
// Current map dimensions: sub_82CD69B8 wraps x modulo the first and bounds y
// by the second.
constexpr uint32_t kMapWidth    = 0x82F77364;
constexpr uint32_t kMapHeight   = 0x82F77448;
constexpr int32_t  kMaxMapSide  = 40;
constexpr uint32_t kUnitXOffset = 0x1C;
constexpr uint32_t kUnitYOffset = 0x1E;
// Executor frame slots holding the position read before the commit.
constexpr uint32_t kExecutorOldX = 0x64;
constexpr uint32_t kExecutorOldY = 0x68;

// Mapped, readable guest bytes, or null.
const uint8_t* mapped(uint32_t address, uint32_t size)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory || !size || address < 0x10000 || uint64_t{ address } + size > 0xFFFFF000ull)
    {
        return nullptr;
    }
    auto* heap = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(address + size - 1) != heap)
    {
        return nullptr;
    }
    const auto required = static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly);
    if ((static_cast<uint32_t>(heap->QueryRangeAccess(address, address + size - 1)) & required) != required)
    {
        return nullptr;
    }
    return memory->TranslateVirtual<const uint8_t*>(address);
}

uint16_t be16(const uint8_t* bytes) noexcept
{
    return static_cast<uint16_t>(uint32_t{ bytes[0] } << 8 | bytes[1]);
}

uint32_t be32(const uint8_t* bytes) noexcept
{
    return uint32_t{ bytes[0] } << 24 | uint32_t{ bytes[1] } << 16 | uint32_t{ bytes[2] } << 8 | bytes[3];
}

struct Point
{
    int32_t x = 0;
    int32_t y = 0;
};

// One unit record read from the unit table.
struct UnitView
{
    bool    live = false;
    uint8_t type = 0;
    Point   position;
};

// False when the player or slot is out of range or the record is unreadable.
bool readUnit(int32_t player, uint32_t unit, UnitView& out)
{
    if (player < 0 || player > ai_civs::kMaxCivilizationPlayers || unit >= ai_civs::kUnitsPerPlayer)
    {
        return false;
    }
    const auto* record = mapped(ai_civs::UnitRecord(static_cast<uint32_t>(player), unit), ai_civs::kUnitRecordSize);
    if (!record)
    {
        return false;
    }
    out.live       = ai_civs::IsLiveUnit(record[0], be32(record + ai_civs::kUnitFlagsOffset));
    out.type       = record[ai_civs::kUnitTypeOffset];
    out.position.x = static_cast<int16_t>(be16(record + kUnitXOffset));
    out.position.y = static_cast<int16_t>(be16(record + kUnitYOffset));
    return true;
}

// Creator scope: sub_82CF4C40 places a new unit through the ordinary executor
// (call at 0x82CF5560). Entries are keyed by the creator's frame and only
// match the new unit's own commits, so a nested move of another unit counts.
struct Creation
{
    uint32_t frame  = 0;
    int32_t  player = -1;
    uint32_t unit   = 0;
};

struct CreationScopes
{
    rex::runtime::ThreadState* thread = nullptr;
    std::array<Creation, 8>    entries{};
    size_t                     count = 0;
};

thread_local CreationScopes creationScopes;

CreationScopes& scopes() noexcept
{
    auto* current = rex::runtime::ThreadState::Get();
    if (creationScopes.thread != current)
    {
        creationScopes.count  = 0;
        creationScopes.thread = current;
    }
    return creationScopes;
}

bool inCreation(int32_t player, uint32_t unit) noexcept
{
    const auto& state = scopes();
    for (size_t i = 0; i < state.count; ++i)
    {
        if (state.entries[i].player == player && state.entries[i].unit == unit)
        {
            return true;
        }
    }
    return false;
}

// Current local player through the native accessor, or -1 when unknown. Runs on a
// private frame below the hooked function's r1, as a native bl would.
int32_t localPlayer(PPCRegister& stack, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr)
{
    const auto* word = mapped(ai_civs::kSessionGlobal, 4);
    if (!word || stack.u32 < 0x20000)
    {
        return -1;
    }
    const uint32_t session = be32(word);
    if (session == 0)
    {
        return -1;
    }
    PPCContext call{};
    call.r1.u64 = stack.u32 - 0x70;
    call.r13    = threadPointer;
    call.fpscr  = fpscr;
    call.r3.u64 = session;

    struct HostFloatScope
    {
        uint32_t saved = PPCFPSCRRegister::Platform::getcsr();

        ~HostFloatScope()
        {
            PPCFPSCRRegister::Platform::setcsr(saved);
        }
    } floatScope;

    __imp__sub_82D8A3A0(call, REX_KERNEL_MEMORY()->virtual_membase());
    return call.r3.s32;
}

enum class Terrain
{
    Classified,
    Unreadable,
    InvalidDimensions,
};

struct Classification
{
    Terrain state  = Terrain::Unreadable;
    int32_t width  = 0;
    int32_t height = 0;
    int32_t cell   = -1; // terrain byte of the tile, -1 when out of bounds
    bool    deep   = false;
};

Classification classify(Point tile)
{
    Classification result;
    const auto*    width  = mapped(kMapWidth, 4);
    const auto*    height = mapped(kMapHeight, 4);
    const auto*    plane  = mapped(kTerrainPlane, kTerrainBytes);
    if (!width || !height || !plane)
    {
        return result;
    }
    result.width  = static_cast<int32_t>(be32(width));
    result.height = static_cast<int32_t>(be32(height));
    if (result.width < 1 || result.width > kMaxMapSide || result.height < 1 || result.height > kMaxMapSide)
    {
        result.state = Terrain::InvalidDimensions;
        return result;
    }
    result.state = Terrain::Classified;
    if (tile.x >= 0 && tile.x < result.width && tile.y >= 0 && tile.y < result.height)
    {
        result.cell = plane[40 * tile.x + tile.y];
    }
    result.deep = achievements::IsDeepOcean(std::span<const uint8_t>(plane, kTerrainBytes), result.width, result.height, achievements::Tile{ tile.x, tile.y });
    return result;
}

struct Segment
{
    const char* origin = "";
    int32_t     player = -1;
    uint32_t    unit   = 0;
    Point       from;
    Point       to;
};

std::atomic<bool> unclassifiableWarned{ false };

void trace(const Segment& s, const UnitView& unit, int32_t local, const Classification& c, const char* outcome)
{
    REXLOG_INFO("Custom achievement trace: movement origin={} player={} local={} unit={} type={} from={},{} to={},{} width={} height={} terrain={} deep={} outcome={}",
                s.origin,
                s.player,
                local,
                s.unit,
                static_cast<uint32_t>(unit.type),
                s.from.x,
                s.from.y,
                s.to.x,
                s.to.y,
                c.width,
                c.height,
                c.cell,
                c.deep,
                outcome);
}

// One committed segment of a live unit. Anything that cannot be classified is
// skipped rather than disqualifying.
void observe(const Segment& s, const UnitView& unit, PPCRegister& stack, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr)
{
    if (!unit.live || (s.from.x == s.to.x && s.from.y == s.to.y))
    {
        return;
    }
    const int32_t local = localPlayer(stack, threadPointer, fpscr);
    if (local < 0 || local != s.player)
    {
        return;
    }
    const bool           tracing = REXCVAR_GET(custom_achievement_trace);
    const Classification none{};
    if (inCreation(s.player, s.unit))
    {
        if (tracing)
        {
            trace(s, unit, local, none, "initial_placement");
        }
        return;
    }
    const auto c = classify(s.to);
    if (c.state != Terrain::Classified)
    {
        if (!unclassifiableWarned.exchange(true))
        {
            REXLOG_WARN("Custom achievement: movement could not be classified (map state {}); such segments are skipped",
                        c.state == Terrain::Unreadable ? "unreadable" : "invalid dimensions");
        }
        if (tracing)
        {
            trace(s, unit, local, c, c.state == Terrain::Unreadable ? "unreadable" : "invalid_dimensions");
        }
        return;
    }
    if (c.deep)
    {
        achievements::RecordDeepOceanEntry();
    }
    if (tracing)
    {
        trace(s, unit, local, c, c.deep ? "deep_ocean_entry" : "not_deep");
    }
}

// Old position of a relocated unit that the displacement paths already moved,
// plus the committed position read back from its record.
void observeDisplaced(const char* origin, int32_t player, uint32_t unit, int32_t oldX, int32_t oldY, PPCRegister& stack, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr)
{
    UnitView view;
    if (!readUnit(player, unit, view))
    {
        return;
    }
    Segment s;
    s.origin = origin;
    s.player = player;
    s.unit   = unit;
    s.from   = { oldX, oldY };
    s.to     = view.position;
    observe(s, view, stack, threadPointer, fpscr);
}

} // namespace

// Before 0x82CF5560 (bl sub_82CD69B8) in sub_82CF4C40: r3 = player and r4 =
// the new unit, which the executor is about to place.
void BeginAchievementUnitCreation(PPCRegister& stack, PPCRegister& player, PPCRegister& unit)
{
    try
    {
        auto& state = scopes();
        if (state.count < state.entries.size())
        {
            state.entries[state.count++] = { stack.u32, player.s32, unit.u32 };
        }
    }
    catch (...)
    {
    }
}

// 0x82CF5564, the return address of that call. The creator frame is unchanged.
void EndAchievementUnitCreation(PPCRegister& stack)
{
    try
    {
        auto& state = scopes();
        for (size_t i = state.count; i > 0; --i)
        {
            if (state.entries[i - 1].frame == stack.u32)
            {
                state.count = i - 1;
                return;
            }
        }
    }
    catch (...)
    {
    }
}

// 0x82CD6CD0, right after the executor's position stores: r19 = player,
// r15 = unit, r21/r20 = committed x/y. The old position is in the frame.
void ObserveAchievementOrdinaryMove(PPCRegister& stack, PPCRegister& player, PPCRegister& unit, PPCRegister& newX, PPCRegister& newY, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr)
{
    try
    {
        const auto* slots = mapped(stack.u32 + kExecutorOldX, kExecutorOldY - kExecutorOldX + 4);
        UnitView    view;
        if (!slots || !readUnit(player.s32, unit.u32, view))
        {
            return;
        }
        Segment s;
        s.origin = "ordinary";
        s.player = player.s32;
        s.unit   = unit.u32;
        s.from   = { static_cast<int32_t>(be32(slots)), static_cast<int32_t>(be32(slots + (kExecutorOldY - kExecutorOldX))) };
        s.to     = { newX.s32, newY.s32 };
        observe(s, view, stack, threadPointer, fpscr);
    }
    catch (...)
    {
    }
}

// 0x82CDFD94, command 19 entry: r31 = player, r15 = unit, r29 = packed
// coordinate. Nothing between here and the x/y stores at 0x82CDFDCC/0x82CDFDD8
// can branch, so the entry values are the committed ones.
void ObserveAchievementPackedMove(PPCRegister& stack, PPCRegister& player, PPCRegister& unit, PPCRegister& packed, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr)
{
    try
    {
        UnitView view;
        if (!readUnit(player.s32, unit.u32, view))
        {
            return;
        }
        Segment s;
        s.origin = "command19";
        s.player = player.s32;
        s.unit   = unit.u32;
        s.from   = view.position;
        s.to     = { static_cast<int16_t>(packed.s32 >> 8), static_cast<int32_t>(packed.u32 & 0xFF) };
        observe(s, view, stack, threadPointer, fpscr);
    }
    catch (...)
    {
    }
}

// 0x82CF3100, after the adjacent-tile stores at 0x82CF30F4/0x82CF30FC of
// sub_82CF2F60: r26 = player, r22 = unit, r20/r19 = old x/y.
void ObserveAchievementDisplacement(PPCRegister& stack, PPCRegister& player, PPCRegister& unit, PPCRegister& oldX, PPCRegister& oldY, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr)
{
    try
    {
        observeDisplaced("displacement", player.s32, unit.u32, oldX.s32, oldY.s32, stack, threadPointer, fpscr);
    }
    catch (...)
    {
    }
}

// 0x82CF320C, after the city-fallback stores at 0x82CF3204/0x82CF3208.
void ObserveAchievementCityDisplacement(PPCRegister& stack, PPCRegister& player, PPCRegister& unit, PPCRegister& oldX, PPCRegister& oldY, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr)
{
    try
    {
        observeDisplaced("city_displacement", player.s32, unit.u32, oldX.s32, oldY.s32, stack, threadPointer, fpscr);
    }
    catch (...)
    {
    }
}

// 0x82CF3150, before the carrier child's stores (0x82CF3150/0x82CF3158):
// r26 = player, r22 = carrier, r11 = child record + 0x1C. The carrier's own
// record already holds the new position. The child's still holds the old one.
void ObserveAchievementCarrierChild(PPCRegister& stack, PPCRegister& player, PPCRegister& carrier, PPCRegister& childPosition, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr)
{
    try
    {
        const uint32_t first = ai_civs::UnitRecord(static_cast<uint32_t>(player.s32), 0) + kUnitXOffset;
        const uint32_t child = childPosition.u32;
        if (player.s32 < 0 || child < first || (child - first) % ai_civs::kUnitRecordSize != 0)
        {
            return;
        }
        const uint32_t slot = (child - first) / ai_civs::kUnitRecordSize;
        UnitView       carrierView;
        UnitView       childView;
        if (!readUnit(player.s32, carrier.u32, carrierView) || !readUnit(player.s32, slot, childView))
        {
            return;
        }
        Segment s;
        s.origin = "carrier_child";
        s.player = player.s32;
        s.unit   = slot;
        s.from   = childView.position;
        s.to     = carrierView.position;
        observe(s, childView, stack, threadPointer, fpscr);
    }
    catch (...)
    {
    }
}
