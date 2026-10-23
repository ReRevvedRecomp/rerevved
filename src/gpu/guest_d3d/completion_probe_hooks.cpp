#include <cstdint>

#include <rex/ppc/context.h>

#if REREVVED_DEVELOPER
#include "completion_probe.h"

namespace guest = rerevved::gpu::guest_d3d;

void CompletionConstructed(PPCRegister& r31, PPCRegister& r30)
{
    if (!guest::CompletionProbeRequested())
        return;
    if (r30.u32 == 0x5E00)
    {
        if (!r31.u32)
        {
            guest::PublishedCompletionProbe().Observe(0x8268D2AC, 0, guest::CompletionThread(), {}, { r30.u32, r31.u32 }, true, "construction");
            return;
        }
        guest::ObserveCompletionSample(0x8268D2AC, r31.u32, guest::CompletionThread(), { r30.u32, r31.u32 }, true, "construction");
    }
}

void CompletionDeferredBegin()
{
    guest::ObserveCompletionStoreBegin(0x8269BE1C);
}

void CompletionDeferredEnd(PPCRegister& r30, PPCRegister& r26, PPCRegister& r11)
{
    if (!guest::CompletionProbeRequested())
        return;
    guest::ObserveCompletionStoreEnd(0x8269BE1C, r30.u32, guest::CompletionThread(), guest::CompletionAdd::Deferred, r26.u32, r11.u32);
}

void CompletionDirectBegin()
{
    guest::ObserveCompletionStoreBegin(0x8269BE4C);
}

void CompletionDirectEnd(PPCRegister& r30, PPCRegister& r26, PPCRegister& r11)
{
    if (!guest::CompletionProbeRequested())
        return;
    guest::ObserveCompletionStoreEnd(0x8269BE4C, r30.u32, guest::CompletionThread(), guest::CompletionAdd::Direct, r26.u32, r11.u32);
}

void CompletionRetireBegin()
{
    guest::ObserveCompletionStoreBegin(0x826ABB34);
}

void CompletionWatchdogEnd()
{
    guest::ObserveCompletionResetEnd();
}

void CompletionRetireEnd(PPCRegister& r31, PPCRegister& r11)
{
    if (!guest::CompletionProbeRequested())
        return;
    guest::ObserveCompletionStoreEnd(0x826ABB34, r31.u32 >= 0x2AC0 ? r31.u32 - 0x2AC0 : 0, guest::CompletionThread(), guest::CompletionAdd::Direct, 1, r11.u32, true);
}

void CompletionInterrupt(PPCRegister& r3, PPCRegister& r4)
{
    if (!guest::CompletionProbeRequested())
        return;
    guest::ObserveCompletionSample(0x8269B450, r4.u32, guest::CompletionThread(), { r3.u32 }, false, "interrupt");
}

void CompletionInterruptCallback(PPCRegister& r31, PPCRegister& r30, PPCRegister& r3)
{
    if (!guest::CompletionProbeRequested())
        return;
    guest::ObserveCompletionCallback(0x8269B4A4, r31.u32, r30.u32, r3.u32, 0);
}

void CompletionOptional347C(PPCRegister& r9, PPCRegister& r4, PPCRegister& f1)
{
    if (!guest::CompletionProbeRequested())
        return;
    guest::ObserveCompletionSample(0x8268CB88, 0, guest::CompletionThread(), { r9.u32, r4.u32, static_cast<std::uint32_t>(f1.u64 >> 32), f1.u32 }, false, "callback");
}

void CompletionOptional40A0(PPCRegister& r31, PPCRegister& r11, PPCRegister& r3)
{
    if (!guest::CompletionProbeRequested())
        return;
    guest::ObserveCompletionCallback(0x826A3FA0, r31.u32, r11.u32, r3.u32, 3);
}

void CompletionOptional409C(PPCRegister& r31, PPCRegister& r9, PPCRegister& r3)
{
    if (!guest::CompletionProbeRequested())
        return;
    guest::ObserveCompletionCallback(0x826A40B4, r31.u32, r9.u32, r3.u32, 6);
}

void CompletionAllocationBegin(PPCRegister& r12, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5)
{
    if (!guest::CompletionProbeRequested())
        return;
    guest::ObserveCompletionAllocationBegin(guest::CompletionThread(), 0x8269B81C, r3.u32, r12.u32, r4.u32, r5.u32);
}

void CompletionAllocationEnd(PPCRegister& r3)
{
    if (!guest::CompletionProbeRequested())
        return;
    guest::ObserveCompletionAllocationEnd(guest::CompletionThread(), 0x8269B8EC, r3.u32);
}

void CompletionManagerBegin(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, std::uint64_t lr)
{
    if (!guest::CompletionProbeRequested())
        return;
    guest::ObserveCompletionAllocationBegin(guest::CompletionThread(), 0x826A6AB8, 0, static_cast<std::uint32_t>(lr), r5.u32, r6.u32, r3.u32, r4.u32);
}

void CompletionManagerTarget(PPCRegister& r31, PPCRegister& r11, PPCRegister& r3, PPCRegister& r5)
{
    if (!guest::CompletionProbeRequested())
        return;
    guest::ObserveCompletionAllocationTarget(guest::CompletionThread(), r11.u32, r3.u32, r5.u32, r31.u32);
}

void CompletionManagerEnd(PPCRegister& r3)
{
    if (!guest::CompletionProbeRequested())
        return;
    guest::ObserveCompletionAllocationEnd(guest::CompletionThread(), 0x826A6B1C, r3.u32);
}

#else

// The generated mid-body hooks call these in every build; the completion probe
// is developer-only, so the shipping bodies are empty.
void CompletionConstructed(PPCRegister&, PPCRegister&)
{
}

void CompletionDeferredBegin()
{
}

void CompletionDeferredEnd(PPCRegister&, PPCRegister&, PPCRegister&)
{
}

void CompletionDirectBegin()
{
}

void CompletionDirectEnd(PPCRegister&, PPCRegister&, PPCRegister&)
{
}

void CompletionRetireBegin()
{
}

void CompletionWatchdogEnd()
{
}

void CompletionRetireEnd(PPCRegister&, PPCRegister&)
{
}

void CompletionInterrupt(PPCRegister&, PPCRegister&)
{
}

void CompletionInterruptCallback(PPCRegister&, PPCRegister&, PPCRegister&)
{
}

void CompletionOptional347C(PPCRegister&, PPCRegister&, PPCRegister&)
{
}

void CompletionOptional40A0(PPCRegister&, PPCRegister&, PPCRegister&)
{
}

void CompletionOptional409C(PPCRegister&, PPCRegister&, PPCRegister&)
{
}

void CompletionAllocationBegin(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&)
{
}

void CompletionAllocationEnd(PPCRegister&)
{
}

void CompletionManagerBegin(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, std::uint64_t)
{
}

void CompletionManagerTarget(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&)
{
}

void CompletionManagerEnd(PPCRegister&)
{
}

#endif
