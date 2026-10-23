#include <rex/ppc/context.h>

#if REREVVED_DEVELOPER
#include "video_scaler_stub.h"

namespace guest = rerevved::gpu::guest_d3d;

// After 0x826B042C mr r3, r25, replace scaler import 0x826B0430 with jump 0x826B0434,
// r3=200 and 200 NOP dwords. False runs import; only r3 changes, LR stays pre-call until
// retained memcpy.
bool ReplaceNativeVideoScalerCall(PPCRegister& r1,
                                  PPCRegister& r3,
                                  PPCRegister& r4,
                                  PPCRegister& r5,
                                  PPCRegister& r6,
                                  PPCRegister& r7,
                                  PPCRegister& r8,
                                  PPCRegister& r9,
                                  PPCRegister& r10,
                                  PPCRegister& r30,
                                  PPCRegister& r31)
{
    if (!guest::VideoScalerActive()) [[likely]]
        return false;
    guest::VideoScalerCall call;
    call.r1           = r1.u64;
    call.arguments[0] = r3.u64;
    call.arguments[1] = r4.u64;
    call.arguments[2] = r5.u64;
    call.arguments[3] = r6.u64;
    call.arguments[4] = r7.u64;
    call.arguments[5] = r8.u64;
    call.arguments[6] = r9.u64;
    call.arguments[7] = r10.u64;
    call.r30          = r30.u64;
    call.r31          = r31.u64;
    return guest::ReplaceVideoScalerCall(call, r3.u64);
}

// Before the rlwinm at 0x826B0434: r3 is the import's count, from the SDK or the
// replacement. Observation only.
void ObserveNativeVideoScalerReturn(PPCRegister& r1, PPCRegister& r3, PPCRegister& r30, PPCRegister& r31)
{
    if (!guest::VideoScalerStubReportingRequested()) [[likely]]
        return;
    guest::ObserveVideoScalerReturn(r1.u32, r3.u64, r30.u32, r31.u32);
}

// After the stw of G+0x30 at 0x826B044C. Observation only.
void ObserveNativeVideoScalerCopy(PPCRegister& r1, PPCRegister& r30, PPCRegister& r31)
{
    if (!guest::VideoScalerStubReportingRequested()) [[likely]]
        return;
    guest::ObserveVideoScalerCopy(r1.u32, r30.u32, r31.u32);
}

#else

// The shipping build runs the import and observes nothing; the replacement
// and its observations are developer-only.
bool ReplaceNativeVideoScalerCall(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&)
{
    return false;
}

void ObserveNativeVideoScalerReturn(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&)
{
}

void ObserveNativeVideoScalerCopy(PPCRegister&, PPCRegister&, PPCRegister&)
{
}

#endif
