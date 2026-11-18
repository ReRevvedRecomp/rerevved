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

REXCVAR_DEFINE_BOOL(restore_trace, false, "ReRevved", "Log commands and the receive hold during a resync restore");

namespace
{

constexpr uint32_t kRingProducer = 0x8314F1F4;
constexpr uint32_t kRingConsumer = 0x8314F1F8;
constexpr auto     kWindow       = std::chrono::minutes(2);

std::atomic<int64_t> windowEnd{ 0 };

int64_t now()
{
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

bool armed()
{
    return REXCVAR_GET(restore_trace) && now() < windowEnd.load(std::memory_order_relaxed);
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
    if (!REXCVAR_GET(restore_trace))
    {
        return;
    }
    windowEnd.store(now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(kWindow).count(),
                    std::memory_order_relaxed);
    REXLOG_INFO("Restore trace: start handler=0x{:08X} target={} ring={}/{}",
                handler.u32,
                target.s32,
                load(kRingProducer),
                load(kRingConsumer));
}

void TraceRestoreLocalGate(PPCRegister& flag)
{
    if (REXCVAR_GET(restore_trace))
    {
        REXLOG_INFO("Restore trace: local gate={}", flag.u32 & 0xFF);
    }
}

void TraceRestoreMemberHold(PPCRegister& member, PPCRegister& flag)
{
    if (REXCVAR_GET(restore_trace))
    {
        REXLOG_INFO("Restore trace: member={} hold={}", member.s32, flag.u32 & 0xFF);
    }
}

void TraceRestoreLocalHold(PPCRegister& flag)
{
    if (REXCVAR_GET(restore_trace))
    {
        REXLOG_INFO("Restore trace: local hold={}", flag.u32 & 0xFF);
    }
}

void TraceRestoreReceive(PPCRegister& held, PPCRegister& message)
{
    if (armed())
    {
        REXLOG_INFO("Restore trace: receive held={} seq={}", held.u32 & 0xFF, load(message.u32 + 0x24));
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
    REXLOG_INFO("Restore trace: command type={} args={}/{}/{}/{} caller=0x{:08X} ring={}/{}",
                static_cast<int32_t>(type),
                load(record.u32 + 4),
                load(record.u32 + 8),
                load(record.u32 + 12),
                load(record.u32 + 16),
                static_cast<uint32_t>(lr),
                load(kRingProducer),
                load(kRingConsumer));
}
