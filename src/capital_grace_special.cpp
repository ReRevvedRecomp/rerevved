#include "capital_grace_special.h"

#include "capital_grace_nuclear.h"
#include "raging_barbarians_settings.h"

#include <atomic>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(bool, raging_barbarians_trace);

namespace
{
namespace grace = rerevved::capital_grace;

bool read(void*, uint32_t address, void* destination, uint32_t size) noexcept
{
    try
    {
        auto* memory = REX_KERNEL_MEMORY();
        if (!memory || !size || address < 0x10000 || uint64_t{ address } + size > 0xFFFFF000ull)
        {
            return false;
        }
        auto*      heap = memory->LookupHeap(address);
        const auto last = address + size - 1;
        if (!heap || memory->LookupHeap(last) != heap)
        {
            return false;
        }
        const auto access   = static_cast<uint32_t>(heap->QueryRangeAccess(address, last));
        const auto required = static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly);
        if ((access & required) != required)
        {
            return false;
        }
        std::memcpy(destination, memory->TranslateVirtual<uint8_t*>(address), size);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

const grace::SpecialMemory memory{ nullptr, read };

std::array<std::atomic<uint64_t>, static_cast<size_t>(grace::SpecialReason::Count)> refusalCounts{};

void diagnostic(grace::SpecialReason reason) noexcept
{
    const auto            index          = static_cast<size_t>(reason);
    const auto            count          = ++refusalCounts[index];
    constexpr const char* descriptions[] = {
        "Type24 lifecycle bindings can change during message pumping",
        "nuclear profile or harm binding could not be confirmed",
        "nuclear invocation capacity",
        "nuclear helper frame binding",
        "nuclear late harm binding unreadable",
        "protected footprint"
    };
    try
    {
        if (reason != grace::SpecialReason::ProtectedFootprint && count == 1)
        {
            REXLOG_WARN("Capital grace: unsupported capability: {} (reason_refusals={})", descriptions[index], count);
        }
        else if (REXCVAR_GET(raging_barbarians_trace))
        {
            REXLOG_INFO("Capital grace: {}: {} (reason_refusals={})", reason == grace::SpecialReason::ProtectedFootprint ? "protected footprint" : "unsupported capability", descriptions[index], count);
        }
    }
    catch (...)
    {
    }
}

struct State
{
    rex::runtime::ThreadState*                            thread = nullptr;
    grace::NuclearInvocations                             invocations;
    std::array<grace::NuclearInvocations::Invocation, 16> admissions{};
};

thread_local State state;

State& current() noexcept
{
    auto* thread = rex::runtime::ThreadState::Get();
    if (state.thread != thread)
    {
        state        = {};
        state.thread = thread;
    }
    return state;
}

bool policy()
{
    return grace::Active(rerevved::raging_barbarians::Evaluate(0).enabled, memory.Signed(0x8312B8E0));
}

bool begin(PPCRegister& frame, PPCRegister& player, PPCRegister& slot, bool alternate) noexcept
{
    auto&                                 local = current();
    grace::NuclearInvocations::Invocation invocation{ .frame = frame.u32, .player = player.s32, .slot = slot.s32, .alternate = alternate };
    try
    {
        invocation.parent        = memory.Word(frame.u32);
        invocation.returnAddress = memory.Word(frame.u32 + 248);
        const auto expected      = alternate ? 0x82CDEEACu : 0x82CDF1ACu;
        if (invocation.parent != frame.u32 + 256 || invocation.returnAddress != expected)
        {
            return false;
        }
        for (auto& admission : local.admissions)
        {
            if (admission.parent == invocation.parent && admission.player == player.s32 && admission.slot == slot.s32)
            {
                invocation.causal = admission.causal;
                admission         = {};
                break;
            }
        }
        // The gate retained identity before any pump, so no late source read is needed.
        if (!invocation.causal)
        {
            return false;
        }
        if (!local.invocations.Begin(invocation))
        {
            diagnostic(grace::SpecialReason::NuclearCapacity);
            return true;
        }
        return false;
    }
    catch (...)
    {
        diagnostic(grace::SpecialReason::NuclearFrame);
        return invocation.causal;
    }
}

template <typename Check>
bool guard(PPCRegister& frame, bool alternate, Check check) noexcept
{
    auto* invocation = current().invocations.Find(frame.u32, alternate);
    if (!invocation || !invocation->causal)
    {
        return false;
    }
    bool refuse = true;
    auto reason = grace::SpecialReason::NuclearLateBinding;
    try
    {
        const auto barbarian = memory.Signed(0x82F700B0);
        if (!policy())
        {
            refuse = false;
        }
        else if (grace::Player(barbarian))
        {
            refuse = check(barbarian);
            reason = grace::SpecialReason::ProtectedFootprint;
        }
    }
    catch (...)
    {
    }
    if (refuse)
    {
        invocation->refused = true;
        diagnostic(reason);
    }
    return refuse;
}

} // namespace

namespace rerevved::capital_grace
{

uint64_t SpecialRefusalCount(SpecialReason reason) noexcept
{
    const auto index = static_cast<size_t>(reason);
    return index < refusalCounts.size() ? refusalCounts[index].load() : 0;
}

void BeginSpecialDispatch(uint32_t frame) noexcept
{
    auto& local = current();
    local.invocations.BeginDispatch(frame);
    for (auto& admission : local.admissions)
    {
        if (admission.parent == frame)
        {
            admission = {};
        }
    }
}

SpecialDecision AdmitSpecial(const SpecialInput& input) noexcept
{
    const auto result = AdmitSpecial(memory, input);
    if (result != SpecialDecision::Native)
    {
        diagnostic(result == SpecialDecision::RefuseProtected ? SpecialReason::ProtectedFootprint : input.route == SpecialRoute::Type24 ? SpecialReason::Type24Capability
                                                                                                                                        : SpecialReason::NuclearCapability);
        return result;
    }
    if (input.route == SpecialRoute::Nuclear)
    {
        auto&  local = current();
        size_t count = local.invocations.Occupied();
        for (const auto& admission : local.admissions)
        {
            count += admission.causal;
        }
        if (!local.thread || count >= 16)
        {
            diagnostic(SpecialReason::NuclearCapacity);
            return SpecialDecision::RefuseUnsupported;
        }
        for (auto& admission : local.admissions)
        {
            if (!admission.causal)
            {
                admission = { .parent = local.thread->context()->r1.u32, .player = input.actor.owner, .slot = static_cast<int32_t>(input.words[2]), .causal = true };
                return result;
            }
        }
        diagnostic(SpecialReason::NuclearCapacity);
        return SpecialDecision::RefuseUnsupported;
    }
    return result;
}

} // namespace rerevved::capital_grace

bool BeginCapitalGraceNuclearZero(PPCRegister& frame, PPCRegister& player, PPCRegister& slot)
{
    return begin(frame, player, slot, false);
}

bool BeginCapitalGraceNuclearAlternate(PPCRegister& frame, PPCRegister& player, PPCRegister& slot)
{
    return begin(frame, player, slot, true);
}

bool GuardCapitalGraceNuclearZeroPopulation(PPCRegister& frame, PPCRegister& offset, PPCRegister& base, PPCRegister& x, PPCRegister& y)
{
    return guard(frame, false, [&](int32_t barbarian)
                 {
                     return grace::NuclearPopulation(memory, offset.u32, base.u32, { x.s32, y.s32 }, barbarian);
                 });
}

bool GuardCapitalGraceNuclearAlternatePopulation(PPCRegister& frame, PPCRegister& offset, PPCRegister& base, PPCRegister& x, PPCRegister& y)
{
    return guard(frame, true, [&](int32_t barbarian)
                 {
                     return grace::NuclearPopulation(memory, offset.u32, base.u32, { x.s32, y.s32 }, barbarian);
                 });
}

bool GuardCapitalGraceNuclearZeroVictim(PPCRegister& frame, PPCRegister& bank, PPCRegister& slot, PPCRegister& flags, PPCRegister& x, PPCRegister& y)
{
    return guard(frame, false, [&](int32_t barbarian)
                 {
                     return grace::NuclearVictim(memory, bank.s32, slot.s32, flags.u32, { x.s32, y.s32 }, barbarian);
                 });
}

bool GuardCapitalGraceNuclearAlternateVictim(PPCRegister& frame, PPCRegister& bank, PPCRegister& slot, PPCRegister& flags, PPCRegister& x, PPCRegister& y)
{
    return guard(frame, true, [&](int32_t barbarian)
                 {
                     return grace::NuclearVictim(memory, bank.s32, slot.s32, flags.u32, { x.s32, y.s32 }, barbarian);
                 });
}

void EndCapitalGraceNuclearZero(PPCRegister& frame)
{
    current().invocations.End(frame.u32, true);
}

void EndCapitalGraceNuclearEarly(PPCRegister& frame)
{
    current().invocations.End(frame.u32, false);
}

bool FinishCapitalGraceNuclearZero(PPCRegister& frame, PPCRegister& player, PPCRegister& slot)
{
    return current().invocations.Consume(frame.u32, player.s32, slot.s32);
}

void EndCapitalGraceNuclearDispatch(PPCRegister& frame)
{
    grace::BeginSpecialDispatch(frame.u32);
}
