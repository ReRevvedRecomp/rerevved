#include "unit_sale_value.h"

#include "gpu/guest_d3d/guest_memory.h"

#include <atomic>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(unit_sale_trace, false, "ReRevved", "Trace guarded unit sale valuation decisions; capped at 256 events per process");

namespace
{
namespace sale = rerevved::unit_sale;

thread_local sale::Collection collection;
#ifdef UNIT_SALE_TESTING
thread_local sale::Memory* testMemory = nullptr;
#endif

class GuestMemory final : public sale::Memory
{
public:
    bool Read(uint32_t address, void* out, uint32_t size) override
    {
#if defined(_WIN32)
        auto* kernel = REX_KERNEL_STATE();
        auto* memory = kernel ? kernel->memory() : nullptr;
        if (!memory || !size || address < 0x10000 || uint64_t{ address } + size > 0xFFFFF000ull)
        {
            return false;
        }
        const uint32_t end  = address + size - 1;
        auto*          heap = memory->LookupHeap(address);
        if (!heap || memory->LookupHeap(end) != heap ||
            (static_cast<uint32_t>(heap->QueryRangeAccess(address, end)) & static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly)) == 0)
        {
            return false;
        }
        return rerevved::gpu::guest_d3d::CopyGuestGuarded(out, memory->TranslateVirtual<const uint8_t*>(address), size);
#else
        // The portable copy helper does not contain access faults.
        (void)address;
        (void)out;
        (void)size;
        return false;
#endif
    }
};

sale::Memory& memory()
{
#ifdef UNIT_SALE_TESTING
    if (testMemory)
    {
        return *testMemory;
    }
#endif
    static GuestMemory guest;
    return guest;
}

const char* decisionName(sale::Decision decision)
{
    switch (decision)
    {
        case sale::Decision::Captured:
            return "captured";
        case sale::Decision::Applied:
            return "applied";
        case sale::Decision::Identity:
            return "identity mismatch";
        case sale::Decision::Unreadable:
            return "unreadable guest state";
        case sale::Decision::Unsupported:
            return "unsupported carrier or cargo";
        case sale::Decision::Reentrant:
            return "nested or stale collection";
        case sale::Decision::Missing:
            return "missing collection";
        case sale::Decision::Changed:
            return "roster or definition changed";
        case sale::Decision::NativeValue:
            return "native sum multiplier or amount mismatch";
        case sale::Decision::Exception:
            return "read interrupted";
    }
    return "unknown";
}

void trace(sale::Phase phase, bool apply, sale::Decision decision, uint32_t frame) noexcept
try
{
    if (!REXCVAR_GET(unit_sale_trace))
    {
        return;
    }
    static std::atomic<uint32_t> events{ 0 };
    uint32_t                     count = events.load(std::memory_order_relaxed);
    while (count < 256)
    {
        if (events.compare_exchange_weak(count, count + 1, std::memory_order_relaxed))
        {
            REXLOG_INFO("Unit sale: phase={} seam={} frame=0x{:08X} decision={}",
                        phase == sale::Phase::Preview ? "preview" : "credit",
                        apply ? "apply" : "begin",
                        frame,
                        decisionName(decision));
            return;
        }
    }
}
catch (...)
{
}

sale::Seam common(const PPCRegister& frame, const PPCRegister& units, const PPCRegister& definitions, const PPCRegister& mask, const PPCRegister& globals, const PPCRegister& original)
{
    sale::Seam seam;
    seam.frame          = frame.u32;
    seam.unitBase       = units.u32;
    seam.definitionBase = definitions.u32;
    seam.deadMask       = mask.u32;
    seam.globalBase     = globals.u32;
    seam.originalPlayer = original.s64;
    return seam;
}

} // namespace

#ifdef UNIT_SALE_TESTING
void rerevved::unit_sale::SetMemoryForTesting(Memory* reader) noexcept
{
    testMemory = reader;
    collection = {};
}
#endif

void BeginUnitSalePreview(PPCRegister& frame, PPCRegister& type, PPCRegister& offset, PPCRegister& flags, PPCRegister& definitionOffset, PPCRegister& units, PPCRegister& original, PPCRegister& mask, PPCRegister& definitions, PPCRegister& globals, PPCRegister& slot)
{
    auto seam             = common(frame, units, definitions, mask, globals, original);
    seam.slot             = slot.s64;
    seam.type             = type.s64;
    seam.offset           = offset.u64;
    seam.definitionOffset = definitionOffset.u64;
    seam.definitionFlags  = flags.u64;
    trace(sale::Phase::Preview, false, collection.Begin(memory(), sale::Phase::Preview, seam), seam.frame);
}

void ApplyUnitSalePreview(PPCRegister& frame, PPCRegister& type, PPCRegister& multiplier, PPCRegister& amount, PPCRegister& sum, PPCRegister& units, PPCRegister& original, PPCRegister& mask, PPCRegister& definitions, PPCRegister& globals, PPCRegister& slot)
{
    auto seam       = common(frame, units, definitions, mask, globals, original);
    seam.slot       = slot.s64;
    seam.type       = type.s64;
    seam.nativeSum  = sum.s64;
    seam.multiplier = multiplier.s64;
    trace(sale::Phase::Preview, true, collection.Apply(memory(), sale::Phase::Preview, seam, amount.u64), seam.frame);
}

void BeginUnitSaleCredit(PPCRegister& frame, PPCRegister& units, PPCRegister& original, PPCRegister& mask, PPCRegister& definitions, PPCRegister& globals, PPCRegister& slot)
{
    auto seam = common(frame, units, definitions, mask, globals, original);
    seam.slot = slot.s64;
    trace(sale::Phase::Credit, false, collection.Begin(memory(), sale::Phase::Credit, seam), seam.frame);
}

void ApplyUnitSaleCredit(PPCRegister& frame, PPCRegister& amount, PPCRegister& h, PPCRegister& units, PPCRegister& original, PPCRegister& mask, PPCRegister& definitions, PPCRegister& otherGlobals, PPCRegister& sum, PPCRegister& globals)
{
    auto seam = common(frame, units, definitions, mask, globals, original);
    if (otherGlobals.u32 != sale::kGlobalBase)
    {
        seam.globalBase = 0;
    }
    seam.nativeSum = sum.s64;
    seam.h         = h.s64;
    trace(sale::Phase::Credit, true, collection.Apply(memory(), sale::Phase::Credit, seam, amount.u64), seam.frame);
}
