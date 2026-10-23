#include "resource_identity_trace.h"

#include <bit>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(resource_identity_trace, false, "ReRevved", "Observe Balanced resource identities, yields and publication; capped at 4096 events per process");

namespace rerevved::resource_identity_trace
{
namespace
{

namespace overrides = resource_identity_overrides;

struct TraceState
{
    std::mutex          mutex;
    EventBudget         budget;
    EventHistory        history;
    overrides::Snapshot watched;
};

TraceState& state()
{
    static TraceState value;
    return value;
}

thread_local ReaderScope* reader     = nullptr;
thread_local YieldScope*  yield      = nullptr;
thread_local CityScope*   activeCity = nullptr;

template <typename Keep, typename F>
void event(Keep&& keep, F&& log) noexcept
{
    if (!REXCVAR_GET(resource_identity_trace))
    {
        return;
    }
    try
    {
        auto&           trace = state();
        std::lock_guard lock(trace.mutex);
        if (!keep(trace))
        {
            return;
        }
        const auto sequence = trace.budget.Next();
        if (sequence == 0)
        {
            return;
        }
        if (sequence <= kEventCap)
        {
            log(trace, sequence);
        }
        else
        {
            REXLOG_INFO("Resource identity trace seq={} truncated cap={}; complete ordering is inconclusive", sequence, kEventCap);
        }
    }
    catch (...)
    {
        // Observation cannot unwind through the guest or publication.
    }
}

std::string tiles(const overrides::Snapshot& snapshot)
{
    std::string result;
    for (const auto record : snapshot)
    {
        if (!result.empty())
        {
            result += ',';
        }
        result += std::to_string(record.tileIndex) + ':' + std::to_string(record.ordinal);
    }
    return result;
}

void publication(bool published, const overrides::Snapshot& previous, const overrides::Snapshot& next, std::source_location caller) noexcept
{
    event([&](TraceState& trace)
          {
              trace.history.Reset();
              if (!next.empty())
              {
                  trace.watched = next;
              }
              else if (!previous.empty())
              {
                  trace.watched = previous;
              }
              return true;
          },
          [&](TraceState&, uint64_t sequence)
          {
              const std::string_view function = caller.function_name();
              const char*            reason   = "other";
              if (function.find("RestoreAfterLoadReset") != function.npos)
              {
                  reason = "sidecar restore";
              }
              else if (function.find("ResetOnLoad") != function.npos)
              {
                  reason = "load reset clear";
              }
              else if (function.find("SetupScope") != function.npos)
              {
                  reason = "setup entry clear";
              }
              else if (function.find("Apply") != function.npos)
              {
                  reason = published ? "setup publish" : "setup apply clear";
              }
              REXLOG_INFO("Resource identity trace seq={} event={} reason={} source={}:{} previous_snapshot={:X} snapshot={:X} previous_count={} count={} previous_tiles=[{}] tiles=[{}]",
                          sequence,
                          published ? "publish" : "clear",
                          reason,
                          function,
                          caller.line(),
                          reinterpret_cast<uintptr_t>(&previous),
                          reinterpret_cast<uintptr_t>(&next),
                          previous.size(),
                          next.size(),
                          tiles(previous),
                          tiles(next));
          });
}

struct InstallObserver
{
    InstallObserver()
    {
        overrides::SetObserver(publication);
    }
} installObserver;

bool terrainByte(uint64_t terrainBase, uint64_t index, int& terrain) noexcept
try
{
    if (index > UINT32_MAX)
    {
        return false;
    }
    const uint64_t address = uint64_t{ static_cast<uint32_t>(terrainBase) } + index;
    if (address < 0x10000 || address >= 0xFFFFF000ull)
    {
        return false;
    }
    auto* kernel = REX_KERNEL_STATE();
    auto* memory = kernel ? kernel->memory() : nullptr;
    auto* heap   = memory ? memory->LookupHeap(static_cast<uint32_t>(address)) : nullptr;
    if (!heap || !(static_cast<unsigned>(heap->QueryRangeAccess(static_cast<uint32_t>(address), static_cast<uint32_t>(address))) & static_cast<unsigned>(rex::memory::PageAccess::kReadOnly)))
    {
        return false;
    }
    terrain = std::bit_cast<int8_t>(*memory->TranslateVirtual<const uint8_t*>(static_cast<uint32_t>(address)));
    return true;
}
catch (...)
{
    return false;
}

bool readU32(uint64_t address, uint32_t& value) noexcept
try
{
    if (address < 0x10000 || address > 0xFFFFEFFCull)
    {
        return false;
    }
    auto* kernel = REX_KERNEL_STATE();
    auto* memory = kernel ? kernel->memory() : nullptr;
    auto* heap   = memory ? memory->LookupHeap(static_cast<uint32_t>(address)) : nullptr;
    if (!heap || !(static_cast<unsigned>(heap->QueryRangeAccess(static_cast<uint32_t>(address), static_cast<uint32_t>(address + 3))) & static_cast<unsigned>(rex::memory::PageAccess::kReadOnly)))
    {
        return false;
    }
    const auto* bytes = memory->TranslateVirtual<const uint8_t*>(static_cast<uint32_t>(address));
    value             = uint32_t{ bytes[0] } << 24 | uint32_t{ bytes[1] } << 16 | uint32_t{ bytes[2] } << 8 | bytes[3];
    return true;
}
catch (...)
{
    return false;
}

bool cityOwnerCaller(uint64_t caller) noexcept
{
    return caller == 0x82D13E28 || caller == 0x82D13E48 || caller == 0x82D13E70;
}

} // namespace

YieldScope::YieldScope(YieldOwner ownerValue, uint64_t callerValue, uint64_t aValue, uint64_t bValue, uint64_t cityValue, uint64_t playerValue) noexcept
: owner(ownerValue)
, caller(callerValue)
, a(aValue)
, b(bValue)
, city(cityValue)
, player(playerValue)
{
    if (!REXCVAR_GET(resource_identity_trace))
    {
        return;
    }
    try
    {
        uint32_t   boundsA    = 0;
        uint32_t   boundsB    = 0;
        const bool boundsRead = readU32(0x82F77364, boundsA) && readU32(0x82F77448, boundsB);
        const auto snapshot   = overrides::Current();
        snapshotSize          = snapshot->size();
        snapshotId            = reinterpret_cast<uintptr_t>(snapshot.get());
        const auto match      = [&](const overrides::Snapshot& records)
        {
            return MatchCoordinates(records, a, b, boundsRead ? std::bit_cast<int32_t>(boundsA) : 0, boundsRead ? std::bit_cast<int32_t>(boundsB) : 0);
        };
        coordinate = match(*snapshot);
        if (snapshot->empty())
        {
            auto&           trace = state();
            std::lock_guard lock(trace.mutex);
            coordinate = match(trace.watched);
            retained   = coordinate.exact || coordinate.wrapped;
        }
        if (activeCity && cityOwnerCaller(caller) && (coordinate.exact || coordinate.wrapped))
        {
            activeCity->matched = true;
        }
        previous = yield;
        yield    = this;
        enabled  = true;
    }
    catch (...)
    {
    }
}

YieldScope::~YieldScope() noexcept
{
    if (enabled)
    {
        yield = previous;
    }
}

void YieldScope::Returned(int32_t value) noexcept
{
    if (!enabled || !coordinate.ShouldTrace())
    {
        return;
    }
    event([&](TraceState& trace)
          {
              return trace.history.ShouldEmitSample(static_cast<uint32_t>(owner), caller, a, b, city, player, { static_cast<uint64_t>(value), 0, 0, 0 });
          },
          [&](TraceState&, uint64_t sequence)
          {
              REXLOG_INFO("Resource identity trace seq={} event=yield_return owner={:08X} caller_lr={:016X} a={:016X} b={:016X} city_input={:016X} player_input={:016X} result={} index={} wrapped_index={} bounds_a={} bounds_b={} bounds_read={} out_of_bounds={} exact={} wrapped={} snapshot={:X} count={} retained_watch={} aggregation_city={} aggregation_mode={}",
                          sequence,
                          static_cast<uint32_t>(owner),
                          caller,
                          a,
                          b,
                          city,
                          player,
                          value,
                          coordinate.index,
                          coordinate.wrappedIndex,
                          coordinate.boundsA,
                          coordinate.boundsB,
                          coordinate.boundsRead,
                          coordinate.outOfBounds,
                          coordinate.exact,
                          coordinate.wrapped,
                          snapshotId,
                          snapshotSize,
                          retained,
                          activeCity && cityOwnerCaller(caller) ? activeCity->city : UINT64_MAX,
                          activeCity && cityOwnerCaller(caller) ? activeCity->mode : UINT64_MAX);
          });
}

void FoodStep(uint32_t site, const char* inputName, uint64_t total, uint64_t input, uint64_t auxiliary, uint64_t resolvedPlayer) noexcept
{
    if (!REXCVAR_GET(resource_identity_trace) || !yield || yield->owner != YieldOwner::Food || !yield->coordinate.ShouldTrace())
    {
        return;
    }
    auto& scope = *yield;
    if (site == 0x82CF1F30)
    {
        scope.resourceOrdinal = input;
    }
    event([&](TraceState& trace)
          {
              return trace.history.ShouldEmitSample(site, scope.caller, scope.a, scope.b, scope.city, scope.player, { total, input, auxiliary, resolvedPlayer });
          },
          [&](TraceState&, uint64_t sequence)
          {
              REXLOG_INFO("Resource identity trace seq={} event=food_step site={:08X} input_name={} caller_lr={:016X} a={:016X} b={:016X} city_input={:016X} player_input={:016X} resolved_player={:016X} total={} input={:016X} auxiliary={:016X} resource_ordinal={} index={} wrapped_index={} retained_watch={}",
                          sequence,
                          site,
                          inputName,
                          scope.caller,
                          scope.a,
                          scope.b,
                          scope.city,
                          scope.player,
                          resolvedPlayer,
                          std::bit_cast<int64_t>(total),
                          input,
                          auxiliary,
                          std::bit_cast<int64_t>(scope.resourceOrdinal),
                          scope.coordinate.index,
                          scope.coordinate.wrappedIndex,
                          scope.retained);
          });
}

CityScope::CityScope(uint64_t callerValue, uint64_t cityValue, uint64_t modeValue) noexcept
: caller(callerValue)
, city(cityValue)
, mode(modeValue)
{
    if (REXCVAR_GET(resource_identity_trace))
    {
        previous   = activeCity;
        activeCity = this;
        enabled    = true;
    }
}

CityScope::~CityScope() noexcept
{
    if (enabled)
    {
        activeCity = previous;
    }
}

void CityTotals(uint64_t stack) noexcept
{
    if (!REXCVAR_GET(resource_identity_trace) || !activeCity || !activeCity->matched)
    {
        return;
    }
    std::array<uint64_t, 4> totals{};
    event([&](TraceState& trace)
          {
              uint32_t                      food       = 0;
              uint32_t                      production = 0;
              uint32_t                      trade      = 0;
              const bool                    read       = readU32(uint64_t{ static_cast<uint32_t>(stack) } + 144, food) &&
                                                         readU32(uint64_t{ static_cast<uint32_t>(stack) } + 128, production) &&
                                                         readU32(uint64_t{ static_cast<uint32_t>(stack) } + 136, trade);
              const std::array<uint64_t, 4> values     = { food, production, trade, read };
              if (!trace.history.ShouldEmitSample(0x82D13F2C, activeCity->caller, activeCity->mode, 0, activeCity->city, 0, values))
              {
                  return false;
              }
              totals = values;
              return true;
          },
          [&](TraceState&, uint64_t sequence)
          {
              REXLOG_INFO("Resource identity trace seq={} event=city_work_totals site=82D13F2C caller_lr={:016X} city={} mode={} food={} production={} trade={} totals_read={}",
                          sequence,
                          activeCity->caller,
                          activeCity->city,
                          activeCity->mode,
                          static_cast<int32_t>(totals[0]),
                          static_cast<int32_t>(totals[1]),
                          static_cast<int32_t>(totals[2]),
                          totals[3] != 0);
          });
}

void OverlayFood(uint64_t a, uint64_t b, uint64_t food, uint64_t manager) noexcept
{
    if (!REXCVAR_GET(resource_identity_trace))
    {
        return;
    }
    uint32_t   player     = 0;
    const bool playerRead = readU32(uint64_t{ static_cast<uint32_t>(manager) } + 0x70, player);
    YieldScope scope(YieldOwner::Food, 0x82D931B8, a, b, UINT64_MAX, playerRead ? player : UINT64_MAX);
    if (!scope.enabled || !scope.coordinate.ShouldTrace())
    {
        return;
    }
    event([&](TraceState& trace)
          {
              return trace.history.ShouldEmitSample(0x82D931B8, 0, a, b, 0, scope.player, { food, manager, playerRead, 0 });
          },
          [&](TraceState&, uint64_t sequence)
          {
              REXLOG_INFO("Resource identity trace seq={} event=overlay_food site=82D931B8 a={:016X} b={:016X} food={} manager={:016X} player={} player_read={} index={} wrapped_index={} retained_watch={}",
                          sequence,
                          a,
                          b,
                          std::bit_cast<int64_t>(food),
                          manager,
                          player,
                          playerRead,
                          scope.coordinate.index,
                          scope.coordinate.wrappedIndex,
                          scope.retained);
          });
}

void OverlayCache(uint64_t a, uint64_t b, uint64_t icon, uint64_t food, uint64_t manager) noexcept
{
    if (!REXCVAR_GET(resource_identity_trace))
    {
        return;
    }
    uint32_t   player     = 0;
    const bool playerRead = readU32(uint64_t{ static_cast<uint32_t>(manager) } + 0x70, player);
    YieldScope scope(YieldOwner::Food, 0x82D931D0, a, b, UINT64_MAX, playerRead ? player : UINT64_MAX);
    if (!scope.enabled || !scope.coordinate.ShouldTrace())
    {
        return;
    }
    uint32_t   cached     = 0;
    const bool cachedRead = readU32(uint64_t{ static_cast<uint32_t>(icon) } + 0x74, cached);
    event([&](TraceState& trace)
          {
              return trace.history.ShouldEmitSample(0x82D931D0, 0, a, b, icon, scope.player, { food, cached, cachedRead, playerRead });
          },
          [&](TraceState&, uint64_t sequence)
          {
              REXLOG_INFO("Resource identity trace seq={} event=overlay_cache site=82D931D0 a={:016X} b={:016X} icon={:016X} incoming_food={} cached_food={} cached_read={} player={} player_read={} index={} wrapped_index={} retained_watch={}",
                          sequence,
                          a,
                          b,
                          icon,
                          std::bit_cast<int64_t>(food),
                          std::bit_cast<int32_t>(cached),
                          cachedRead,
                          player,
                          playerRead,
                          scope.coordinate.index,
                          scope.coordinate.wrappedIndex,
                          scope.retained);
          });
}

ReaderScope::ReaderScope(uint64_t lr, uint64_t a, uint64_t b, uint64_t playerValue) noexcept
: caller(lr)
, coordinateA(a)
, coordinateB(b)
, player(playerValue)
{
    if (!REXCVAR_GET(resource_identity_trace))
    {
        return;
    }
    try
    {
        const auto snapshot = overrides::Current();
        // Mirrors the reader's two masked shifts, then its full-width add of b.
        tileIndex    = uint64_t{ static_cast<uint32_t>(a) * uint32_t{ 40 } } + b;
        snapshotSize = snapshot->size();
        snapshotId   = reinterpret_cast<uintptr_t>(snapshot.get());
        matched      = Matches(*snapshot, tileIndex);
        if (snapshot->empty())
        {
            auto&           trace = state();
            std::lock_guard lock(trace.mutex);
            matched  = Matches(trace.watched, tileIndex);
            retained = matched;
        }
        previous = reader;
        reader   = this;
        enabled  = true;
    }
    catch (...)
    {
    }
}

ReaderScope::~ReaderScope()
{
    if (enabled)
    {
        reader = previous;
    }
}

void ReaderScope::Returned(int32_t ordinal) noexcept
{
    if (!enabled || !matched)
    {
        return;
    }
    event([&](TraceState& trace)
          {
              return trace.history.ShouldEmit(EventKind::Return, caller, tileIndex, player, static_cast<uint64_t>(ordinal), decision);
          },
          [&](TraceState&, uint64_t sequence)
          {
              REXLOG_INFO("Resource identity trace seq={} event=return reader=82CF1558 caller_lr={:016X} a={:016X} b={:016X} player={:016X} r11={:016X} ordinal={} snapshot={:X} count={} retained_watch={} decision_seq={} decision={}",
                          sequence,
                          caller,
                          coordinateA,
                          coordinateB,
                          player,
                          tileIndex,
                          ordinal,
                          snapshotId,
                          snapshotSize,
                          retained,
                          decisionSequence,
                          decision);
          });
}

void Decision(const overrides::Snapshot& snapshot, uint64_t index, uint64_t before, uint64_t after, uint64_t terrainBase, const char* decision) noexcept
{
    if (!REXCVAR_GET(resource_identity_trace))
    {
        return;
    }
    if (snapshot.empty())
    {
        if (reader)
        {
            reader->retained     = reader->matched;
            reader->tileIndex    = index;
            reader->snapshotSize = 0;
            reader->snapshotId   = reinterpret_cast<uintptr_t>(&snapshot);
            reader->decision     = decision;
        }
        return;
    }
    const bool exact = Matches(snapshot, index);
    if (reader)
    {
        reader->matched      = exact;
        reader->retained     = false;
        reader->tileIndex    = index;
        reader->snapshotSize = snapshot.size();
        reader->snapshotId   = reinterpret_cast<uintptr_t>(&snapshot);
        reader->decision     = decision;
    }
    if (!exact)
    {
        return;
    }
    event([&](TraceState& trace)
          {
              return trace.history.ShouldEmit(EventKind::Decision, reader ? reader->caller : 0, index, reader ? reader->player : 0, after, decision);
          },
          [&](TraceState&, uint64_t sequence)
          {
              int        terrain = 0;
              const bool read    = terrainByte(terrainBase, index, terrain);
              if (reader)
              {
                  reader->decisionSequence = sequence;
              }
              REXLOG_INFO("Resource identity trace seq={} event=decision site=82CF15C4 caller_lr={:016X} caller_known={} a={:016X} b={:016X} player={:016X} r11={:016X} r8_before={:016X} r8_after={:016X} r7={:016X} terrain_byte={:02X} terrain={} terrain_read={} snapshot={:X} count={} exact={} decision={}",
                          sequence,
                          reader ? reader->caller : 0,
                          reader != nullptr,
                          reader ? reader->coordinateA : 0,
                          reader ? reader->coordinateB : 0,
                          reader ? reader->player : 0,
                          index,
                          before,
                          after,
                          terrainBase,
                          static_cast<uint8_t>(terrain),
                          terrain,
                          read,
                          reinterpret_cast<uintptr_t>(&snapshot),
                          snapshot.size(),
                          exact,
                          decision);
          });
}

} // namespace rerevved::resource_identity_trace
