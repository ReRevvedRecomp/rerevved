#include <rex/ppc/context.h>

#if REREVVED_DEVELOPER
#include "video_display_mode_stub.h"

namespace guest = rerevved::gpu::guest_d3d;

// Replace bl 0x826A6260 with jump 0x826A6264 and full r3=0; false runs import
// once. Only r3 changes; LR keeps its pre-call value until retained bl
// 0x826A6268. LR is recorded by value only.
bool ReplaceNativeVideoDisplayModeCall(PPCRegister& r1, PPCRegister& r3, PPCRegister& r31, uint64_t lr)
{
    if (!guest::VideoDisplayModeActive()) [[likely]]
        return false;
    guest::VideoDisplayModeCall call;
    call.r1  = r1.u64;
    call.r3  = r3.u64;
    call.r31 = r31.u64;
    call.lr  = lr;
    return guest::ReplaceVideoDisplayModeCall(call, r3.u64);
}

// Before the addi at 0x826A6264: r3 is the import's result, from the SDK or the
// replacement. Observation only, and it takes no LR: the skipped call leaves LR
// stale over this interval.
void ObserveNativeVideoDisplayModeReturn(PPCRegister& r1, PPCRegister& r3, PPCRegister& r5, PPCRegister& r31)
{
    if (!guest::VideoDisplayModeStubReportingRequested()) [[likely]]
        return;
    guest::ObserveVideoDisplayModeReturn(r1.u64, r3.u64, r5.u32, r31.u64);
}

// Before the first lhz at 0x826A626C, after the retained display-information
// import. Observation only.
void ObserveNativeVideoDisplayModeReconverged(PPCRegister& r1, PPCRegister& r3, PPCRegister& r5, PPCRegister& r31, uint64_t lr)
{
    if (!guest::VideoDisplayModeStubReportingRequested()) [[likely]]
        return;
    guest::ObserveVideoDisplayModeReconverged(r1.u64, r3.u64, r5.u32, r31.u64, lr);
}

#else

// The shipping build runs the import and observes nothing; the replacement
// and its observations are developer-only.
bool ReplaceNativeVideoDisplayModeCall(PPCRegister&, PPCRegister&, PPCRegister&, uint64_t)
{
    return false;
}

void ObserveNativeVideoDisplayModeReturn(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&)
{
}

void ObserveNativeVideoDisplayModeReconverged(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, uint64_t)
{
}

#endif
