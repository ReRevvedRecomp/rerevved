#include "gameplay_session_settings.h"
#include "resync_record.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REX_EXTERN(__imp__sub_82CD4CF0);
REX_EXTERN(__imp__sub_82D91BD0);
REX_EXTERN(__imp__sub_82D895B8);

namespace
{

namespace record = rerevved::resync_record;

constexpr uint32_t kTurn                  = 0x8312B8DC;
constexpr uint32_t kDispatcherEntryReturn = 0x82CDDF38;
constexpr uint32_t kTurnStartReturn       = 0x82CDE8BC;
constexpr uint32_t kExchangeReturn        = 0x82CDEC68;
constexpr uint32_t kStoredScalars         = 0x8312E5C0;
constexpr uint32_t kStoredCount           = 8;
constexpr uint32_t kTargetIndex           = 0x34;
constexpr uint32_t kExchangeFlags         = 0x82FFE7C4;
constexpr uint32_t kExchangeBit           = 0x10;
constexpr uint32_t kStreamCounterA        = 0x10;
constexpr uint32_t kStreamCounterB        = 0x14;
constexpr uint32_t kStreamBase            = 0x18;
constexpr uint32_t kStreamEnd             = 0x20;
constexpr uint32_t kStreamFrame           = 128; // Stream object offset in the sub_82D8FF78 frame.
constexpr uint32_t kUnknownTurn           = UINT32_MAX;

class MemoryReader final : public record::GuestReader
{
public:
    bool Read(uint32_t address, uint32_t size, uint8_t* out) const override
    {
        auto* memory = REX_KERNEL_MEMORY();
        if (!memory || !size || address < 0x10000 || address >= 0xFFFFF000u || size > 0xFFFFF000u - address)
        {
            return false;
        }
        const uint32_t last = address + size - 1;
        auto*          heap = memory->LookupHeap(address);
        if (!heap || memory->LookupHeap(last) != heap)
        {
            return false;
        }
        constexpr auto required = static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly);
        if ((static_cast<uint32_t>(heap->QueryRangeAccess(address, last)) & required) != required)
        {
            return false;
        }
        std::copy_n(memory->TranslateVirtual<const uint8_t*>(address), size, out);
        return true;
    }
};

bool readWord(uint32_t address, uint32_t& value)
{
    uint8_t bytes[4];
    if (!MemoryReader().Read(address, 4, bytes))
    {
        return false;
    }
    value = (uint32_t{ bytes[0] } << 24) | (uint32_t{ bytes[1] } << 16) | (uint32_t{ bytes[2] } << 8) | bytes[3];
    return true;
}

uint32_t wordOr(uint32_t address, uint32_t fallback)
{
    uint32_t value = 0;
    return readWord(address, value) ? value : fallback;
}

uint32_t currentTurn()
{
    return wordOr(kTurn, kUnknownTurn);
}

record::LedgerDedupe& dedupe()
{
    static record::LedgerDedupe instance;
    return instance;
}

void logLedger(uint32_t caller, uint32_t scalar) noexcept
try
{
    // The dispatcher samples on every record, at no comparable moment across peers.
    if (caller == kDispatcherEntryReturn || !rerevved::gameplay_session::NetworkGameActive())
    {
        return;
    }
    const uint32_t turn = currentTurn();
    if (!dedupe().Admit(turn, caller, scalar))
    {
        return;
    }
    const auto ledger = record::ComputeLedger(MemoryReader());
    if (!ledger)
    {
        REXLOG_INFO("Resync ledger: turn={} lr=0x{:08X} scalar=0x{:08X} unreadable", turn, caller, scalar);
        return;
    }
    std::string players;
    for (uint32_t player = 0; player < ledger->playerCount; ++player)
    {
        const auto& fields = ledger->players[player];
        fmt::format_to(std::back_inserter(players), " p{}=u:{:08X},r:{:08X},cs:{:08X},dc:{:08X},t:{:08X},s:{:08X},tp:{:08X},c:{:08X}", player, fields.units, fields.reserve, fields.cultureStrength, fields.derivedCulture, fields.tally, fields.strategy, fields.progress, fields.cities);
    }
    REXLOG_INFO("Resync ledger: turn={} lr=0x{:08X} scalar=0x{:08X} sum=0x{:08X} sum_ok={} n={} cities={:08X} wonders={:08X}{}",
                turn,
                caller,
                scalar,
                ledger->sum,
                ledger->sum == scalar ? 1 : 0,
                ledger->playerCount,
                ledger->cities,
                ledger->wonders,
                players);
    // Per-city fields name the city behind a differing c subtotal; two samples per turn bound the volume.
    if (caller != kTurnStartReturn && caller != kExchangeReturn)
    {
        return;
    }
    std::string cities;
    for (uint32_t entry = 0; entry < ledger->cityCount; ++entry)
    {
        const auto& city = ledger->cityFields[entry];
        fmt::format_to(std::back_inserter(cities), " {}:{}:{}:{:X}:{}:{}:{}:{}", city.index, city.owner, city.population, city.word10, city.half34, city.half36, city.half38, city.half3C);
    }
    REXLOG_INFO("Resync cities: turn={} lr=0x{:08X} n={} fields=index:owner:pop:w10:h34:h36:h38:h3C{}", turn, caller, ledger->cityCount, cities);
}
catch (...)
{
}

void logTarget(uint32_t object) noexcept
try
{
    if (!rerevved::gameplay_session::NetworkGameActive())
    {
        return;
    }
    std::string stored;
    for (uint32_t index = 0; index < kStoredCount; ++index)
    {
        fmt::format_to(std::back_inserter(stored), "{}{:08X}", index ? "," : "", wordOr(kStoredScalars + 4 * index, 0));
    }
    const auto  ledger = record::ComputeLedger(MemoryReader());
    std::string local  = "unreadable";
    if (ledger)
    {
        local = fmt::format("0x{:08X}", ledger->sum);
    }
    REXLOG_INFO("Resync target: turn={} index={} stored={} local={}", currentTurn(), static_cast<int32_t>(wordOr(object + kTargetIndex, UINT32_MAX)), stored, local);
}
catch (...)
{
}

void logCallback(uint32_t transfer, uint32_t source, uint32_t outcome) noexcept
try
{
    if (rerevved::gameplay_session::NetworkGameActive())
    {
        REXLOG_INFO("Resync snapshot callback: turn={} transfer={} source={} outcome={}", currentTurn(), static_cast<int32_t>(transfer), static_cast<int32_t>(source), outcome);
    }
}
catch (...)
{
}

struct Exchange
{
    bool     scalarSeen     = false;
    bool     seedSeen       = false;
    uint32_t receivedScalar = 0;
    uint32_t localScalar    = 0;
    uint32_t receivedSeed   = 0;
    uint32_t localSeed      = 0;
};

thread_local Exchange exchange;

} // namespace

REX_HOOK_RAW(sub_82CD4CF0)
{
    const auto caller = static_cast<uint32_t>(ctx.lr);
    __imp__sub_82CD4CF0(ctx, base);
    logLedger(caller, ctx.r3.u32);
}

REX_HOOK_RAW(sub_82D91BD0)
{
    logTarget(ctx.r3.u32);
    __imp__sub_82D91BD0(ctx, base);
}

REX_HOOK_RAW(sub_82D895B8)
{
    logCallback(ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    __imp__sub_82D895B8(ctx, base);
}

void ObserveResyncExchangeScalar(PPCRegister& received, PPCRegister& local)
try
{
    exchange                = {};
    exchange.scalarSeen     = rerevved::gameplay_session::NetworkGameActive();
    exchange.receivedScalar = received.u32;
    exchange.localScalar    = local.u32;
}
catch (...)
{
}

void ObserveResyncExchangeSeed(PPCRegister& received, PPCRegister& local)
try
{
    exchange.seedSeen     = exchange.scalarSeen;
    exchange.receivedSeed = received.u32;
    exchange.localSeed    = local.u32;
}
catch (...)
{
}

void ReportResyncExchange()
try
{
    const Exchange seen = exchange;
    exchange            = {};
    if (!seen.scalarSeen || !seen.seedSeen)
    {
        return;
    }
    REXLOG_INFO("Resync exchange: turn={} recv_scalar=0x{:08X} local_scalar=0x{:08X} scalar_match={} recv_seed=0x{:08X} local_seed=0x{:08X} seed_match={} flag10={}",
                currentTurn(),
                seen.receivedScalar,
                seen.localScalar,
                seen.receivedScalar == seen.localScalar ? 1 : 0,
                seen.receivedSeed,
                seen.localSeed,
                seen.receivedSeed == seen.localSeed ? 1 : 0,
                (wordOr(kExchangeFlags, 0) & kExchangeBit) != 0 ? 1 : 0);
}
catch (...)
{
}

void ObserveResyncSnapshotQueued(PPCRegister& frame, PPCRegister& result, PPCRegister& target)
try
{
    if (!rerevved::gameplay_session::NetworkGameActive())
    {
        return;
    }
    const uint32_t stream = frame.u32 + kStreamFrame;
    const uint32_t begin  = wordOr(stream + kStreamBase, 0);
    const uint32_t end    = wordOr(stream + kStreamEnd, 0);
    REXLOG_INFO("Resync snapshot queued: turn={} target={} transfer={} counter_a={} counter_b={} span={}",
                currentTurn(),
                target.s32,
                result.s32,
                wordOr(stream + kStreamCounterA, 0),
                wordOr(stream + kStreamCounterB, 0),
                end - begin);
}
catch (...)
{
}
