#include <rex/ppc/context.h>

#if REREVVED_DEVELOPER
#include "video_import_stub_table.h"

namespace guest = rerevved::gpu::guest_d3d;

// Pre-bl imports:jump continuation/full r3 constant; false runs once. Only r3
// changes, LR stays until retained bl. Return/effect/pre-retrain take no LR;
// entry/post-child record LR by value.

namespace
{

guest::VideoImportObservation observation(const PPCRegister& r1, const PPCRegister& r31, const PPCRegister& r13)
{
    guest::VideoImportObservation o;
    o.r1  = r1.u64;
    o.r31 = r31.u64;
    o.r13 = r13.u64;
    return o;
}

} // namespace

// Before the bl at 0x826A65A4 in sub_826A63E8: the cache word was loaded zero.
bool ReplaceNativeVideoHsioCall(PPCRegister& r1, PPCRegister& r3, PPCRegister& r11, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13, uint64_t lr)
{
    if (!guest::VideoHsioActive()) [[likely]]
        return false;
    guest::VideoImportStubCall call;
    call.r1  = r1.u64;
    call.r3  = r3.u64;
    call.r11 = r11.u64;
    call.r30 = r30.u64;
    call.r31 = r31.u64;
    call.r13 = r13.u64;
    call.lr  = lr;
    return guest::ReplaceVideoImportStubCall(guest::kVideoHsioCallPC, guest::VideoImportId::Hsio, call, r3.u64);
}

// At 0x826A65A8, before cntlzw: r3 is the import's result. Observation only.
void ObserveNativeVideoHsioReturn(PPCRegister& r1, PPCRegister& r3, PPCRegister& r11, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13)
{
    if (!guest::VideoHsioStubReportingRequested()) [[likely]]
        return;
    auto o = observation(r1, r31, r13);
    o.r3   = r3.u64;
    o.r11  = r11.u64;
    o.r30  = r30.u64;
    guest::ObserveVideoImportReturn(guest::VideoImportId::Hsio, o);
}

// At 0x826A65C4, before mr r3,r31. Observation only.
void ObserveNativeVideoHsioEffect(PPCRegister& r1, PPCRegister& r11, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13, PPCCRRegister& cr0)
{
    if (!guest::VideoHsioStubReportingRequested()) [[likely]]
        return;
    auto o = observation(r1, r31, r13);
    o.r11  = r11.u64;
    o.r30  = r30.u64;
    o.cr0  = cr0.raw();
    guest::ObserveVideoImportEffect(guest::VideoImportId::Hsio, o);
}

// At 0x826A65CC, after the retained child call. Observation only.
void ObserveNativeVideoHsioPostChild(PPCRegister& r1, PPCRegister& r3, PPCRegister& r11, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13, PPCCRRegister& cr0, uint64_t lr)
{
    if (!guest::VideoHsioStubReportingRequested()) [[likely]]
        return;
    auto o = observation(r1, r31, r13);
    o.r3   = r3.u64;
    o.r11  = r11.u64;
    o.r30  = r30.u64;
    o.cr0  = cr0.raw();
    guest::ObserveVideoImportPost(guest::VideoImportId::Hsio, o, lr);
}

// Before the bl at 0x826B053C in sub_826B0508: r3 was set to zero by the owner.
bool ReplaceNativeVideoEdramWorkerCall(PPCRegister& r1, PPCRegister& r3, PPCRegister& r29, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13, uint64_t lr)
{
    if (!guest::VideoEdramWorkerActive()) [[likely]]
        return false;
    guest::VideoImportStubCall call;
    call.r1  = r1.u64;
    call.r3  = r3.u64;
    call.r29 = r29.u64;
    call.r30 = r30.u64;
    call.r31 = r31.u64;
    call.r13 = r13.u64;
    call.lr  = lr;
    return guest::ReplaceVideoImportStubCall(guest::kVideoWorkerCallPC, guest::VideoImportId::EdramWorker, call, r3.u64);
}

// At 0x826B0540, before li r11,0: r3 is the import's result. The owner's bypass
// reaches it with a zero low word of r29 and does no work. Observation only.
void ObserveNativeVideoEdramWorkerReturn(PPCRegister& r1, PPCRegister& r3, PPCRegister& r29, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13)
{
    if (!guest::VideoEdramWorkerStubReportingRequested()) [[likely]]
        return;
    auto o = observation(r1, r31, r13);
    o.r3   = r3.u64;
    o.r29  = r29.u64;
    o.r30  = r30.u64;
    guest::ObserveVideoImportReturn(guest::VideoImportId::EdramWorker, o);
}

// At 0x826B0564, before the retained retrain bl: the arguments are in place.
// Observation only.
void ObserveNativeVideoEdramWorkerPreRetrain(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r7, PPCRegister& r8, PPCRegister& r29, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13)
{
    if (!guest::VideoEdramWorkerStubReportingRequested()) [[likely]]
        return;
    auto o = observation(r1, r31, r13);
    o.r3   = r3.u64;
    o.r4   = r4.u64;
    o.r5   = r5.u64;
    o.r6   = r6.u64;
    o.r7   = r7.u64;
    o.r8   = r8.u64;
    o.r29  = r29.u64;
    o.r30  = r30.u64;
    guest::ObserveVideoImportEffect(guest::VideoImportId::EdramWorker, o);
}

// At 0x826B0568, after the retained retrain. Observation only.
void ObserveNativeVideoEdramWorkerPostRetrain(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r7, PPCRegister& r8, PPCRegister& r29, PPCRegister& r30, PPCRegister& r31, PPCRegister& r13, uint64_t lr)
{
    if (!guest::VideoEdramWorkerStubReportingRequested()) [[likely]]
        return;
    auto o = observation(r1, r31, r13);
    o.r3   = r3.u64;
    o.r4   = r4.u64;
    o.r5   = r5.u64;
    o.r6   = r6.u64;
    o.r7   = r7.u64;
    o.r8   = r8.u64;
    o.r29  = r29.u64;
    o.r30  = r30.u64;
    guest::ObserveVideoImportPost(guest::VideoImportId::EdramWorker, o, lr);
}

#else

// The shipping build runs the imports and observes nothing; the replacements
// and their observations are developer-only.
bool ReplaceNativeVideoHsioCall(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, uint64_t)
{
    return false;
}

void ObserveNativeVideoHsioReturn(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&)
{
}

void ObserveNativeVideoHsioEffect(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCCRRegister&)
{
}

void ObserveNativeVideoHsioPostChild(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCCRRegister&, uint64_t)
{
}

bool ReplaceNativeVideoEdramWorkerCall(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, uint64_t)
{
    return false;
}

void ObserveNativeVideoEdramWorkerReturn(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&)
{
}

void ObserveNativeVideoEdramWorkerPreRetrain(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&)
{
}

void ObserveNativeVideoEdramWorkerPostRetrain(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, uint64_t)
{
}

#endif
