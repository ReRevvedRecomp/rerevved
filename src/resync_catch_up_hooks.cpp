// Shows whether peers move during a resync transfer and whether the target holds what
// it receives.

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(resync_catch_up_log, true, "ReRevved", "Log the commands and held messages while a resyncing player catches up");

namespace
{

constexpr uint32_t kRingProducer = 0x8314F1F4;
constexpr uint32_t kRingConsumer = 0x8314F1F8;
constexpr auto     kWindow       = std::chrono::minutes(2);
// Lines per resync start; the ring sees every player's commands while it is open.
constexpr int kLineCap = 256;

std::atomic<int64_t> windowEnd{ 0 };
std::atomic<int>     linesLeft{ kLineCap };

int64_t now()
{
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

// Takes one line from the budget; the last one says the rest are dropped.
bool take()
{
    int left = linesLeft.load(std::memory_order_relaxed);
    while (left > 0 && !linesLeft.compare_exchange_weak(left, left - 1, std::memory_order_relaxed))
    {
    }
    if (left == 1)
    {
        REXLOG_INFO("Resync catch-up: line limit {} reached; the rest of this resync is not logged", kLineCap);
    }
    return left > 1;
}

bool enabled()
{
    return REXCVAR_GET(resync_catch_up_log) && take();
}

bool armed()
{
    return REXCVAR_GET(resync_catch_up_log) && now() < windowEnd.load(std::memory_order_relaxed) && take();
}

uint32_t load(uint32_t address)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory || address == 0)
    {
        return UINT32_MAX;
    }
    return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<const uint8_t*>(address));
}

} // namespace

void TraceRestoreStart(PPCRegister& handler, PPCRegister& target)
{
    if (!REXCVAR_GET(resync_catch_up_log))
    {
        return;
    }
    windowEnd.store(now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(kWindow).count(),
                    std::memory_order_relaxed);
    linesLeft.store(kLineCap, std::memory_order_relaxed);
    REXLOG_INFO("Resync catch-up: start handler=0x{:08X} target={} ring={}/{}",
                handler.u32,
                target.s32,
                load(kRingProducer),
                load(kRingConsumer));
}

void TraceRestoreLocalGate(PPCRegister& flag)
{
    if (enabled())
    {
        REXLOG_INFO("Resync catch-up: local gate={}", flag.u32 & 0xFF);
    }
}

void TraceRestoreMemberHold(PPCRegister& member, PPCRegister& flag)
{
    if (enabled())
    {
        REXLOG_INFO("Resync catch-up: member={} hold={}", member.s32, flag.u32 & 0xFF);
    }
}

void TraceRestoreLocalHold(PPCRegister& flag)
{
    if (enabled())
    {
        REXLOG_INFO("Resync catch-up: local hold={}", flag.u32 & 0xFF);
    }
}

void TraceRestoreReceive(PPCRegister& held, PPCRegister& message)
{
    if (armed())
    {
        REXLOG_INFO("Resync catch-up: receive held={} seq={}", held.u32 & 0xFF, load(message.u32 + 0x24));
    }
}

void TraceRestoreCommand(PPCRegister& record, uint64_t lr)
{
    if (!armed())
    {
        return;
    }
    const uint32_t type = load(record.u32);
    if (type == UINT32_MAX)
    {
        return;
    }
    REXLOG_INFO("Resync catch-up: command type={} args={}/{}/{}/{} caller=0x{:08X} ring={}/{}",
                static_cast<int32_t>(type),
                load(record.u32 + 4),
                load(record.u32 + 8),
                load(record.u32 + 12),
                load(record.u32 + 16),
                static_cast<uint32_t>(lr),
                load(kRingProducer),
                load(kRingConsumer));
}
