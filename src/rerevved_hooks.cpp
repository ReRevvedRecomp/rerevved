// Hooks preserve title behavior unless documented as a compatibility repair.

#include <atomic>
#include <bit>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/xenos_fence_trace.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "gameplay_state.h"
#include "gpu/diagnostics/native_renderer_guest_state.h"
#include "gpu/diagnostics/native_renderer_passive_trace.h"
#include "rush_cost.h"

REXCVAR_DECLARE(std::string, renderer);
REXCVAR_DECLARE(std::string, gpu_plugin);
REXCVAR_DECLARE(std::string, combat_speed);

namespace
{

rex::system::IGraphicsSystem* getGraphicsSystem()
{
    auto* kernelState = REX_KERNEL_STATE();
    if (!kernelState || !kernelState->emulator())
    {
        return nullptr;
    }

    return kernelState->emulator()->graphics_system();
}

bool isGuestPointer(uint32_t address)
{
    return address >= 0x10000 && address < 0xFFFFF000;
}

struct CheckedAddressRange
{
    uint32_t end;
    bool     valid;
};

constexpr CheckedAddressRange makeCheckedAddressRange(uint32_t base,
                                                      uint32_t extent,
                                                      uint32_t ceiling)
{
    return extent == 0 || extent > ceiling || base > UINT32_MAX - extent
               ? CheckedAddressRange{ 0, false }
               : CheckedAddressRange{ base + extent, true };
}

bool isGuestReadableRange(uint32_t address, uint32_t extent)
{
    const auto range = makeCheckedAddressRange(address, extent, UINT32_MAX);
    if (!range.valid || !isGuestPointer(address) ||
        !isGuestPointer(range.end - 1))
    {
        return false;
    }
    auto* memory = REX_KERNEL_MEMORY();
    auto* heap   = memory->LookupHeap(address);
    return heap && memory->LookupHeap(range.end - 1) == heap &&
           heap->QueryRangeAccess(address, range.end - 1) !=
               rex::memory::PageAccess::kNoAccess;
}

uint32_t readGuestU32(uint32_t address)
{
    if (!isGuestPointer(address))
    {
        return 0;
    }
    const auto* memory = REX_KERNEL_MEMORY();
    const auto* source = memory->TranslateVirtual<const uint8_t*>(address);
    return (uint32_t{ source[0] } << 24) | (uint32_t{ source[1] } << 16) |
           (uint32_t{ source[2] } << 8) | uint32_t{ source[3] };
}

bool readGuestFetchDescriptor(
    uint32_t                             address,
    rerevved::gpu::GuestFetchDescriptor& descriptor)
{
    constexpr uint32_t kDescriptorSize =
        sizeof(uint32_t) * rerevved::gpu::kGuestFetchDwordCount;
    if (!isGuestReadableRange(address, kDescriptorSize))
    {
        return false;
    }
    for (std::size_t i = 0; i < descriptor.size(); ++i)
    {
        descriptor[i] = readGuestU32(
            address + static_cast<uint32_t>(i * sizeof(uint32_t)));
    }
    return true;
}

bool writeGuestU32Safely(uint32_t address, uint32_t value)
{
    if (!isGuestReadableRange(address, sizeof(uint32_t)))
    {
        return false;
    }
    auto* memory      = REX_KERNEL_MEMORY();
    auto* destination = memory->TranslateVirtual<uint8_t*>(address);
    destination[0]    = static_cast<uint8_t>(value >> 24);
    destination[1]    = static_cast<uint8_t>(value >> 16);
    destination[2]    = static_cast<uint8_t>(value >> 8);
    destination[3]    = static_cast<uint8_t>(value);
    return true;
}

thread_local bool resumeAfterRingInitialize = false;

struct GfxRenderCapsState
{
    uint32_t renderer;
    uint32_t output;
    uint32_t caller;
    bool     observing;
};

thread_local GfxRenderCapsState       gfxRenderCaps{};
thread_local uint32_t                 gfxRenderConfigCandidate = 0;
thread_local uint32_t                 gfxRenderConfigRenderer  = 0;
thread_local rerevved::RushCostRepair rushCostRepair{};
std::atomic_uint32_t                  gfxStaleRenderConfig                  = 0;
std::atomic_uint32_t                  gfxStaleRenderConfigRenderer          = 0;
std::atomic_uint32_t                  nativeResolveProviderMatchLogCount    = 0;
std::atomic_uint32_t                  nativeResolveProviderMismatchLogCount = 0;
std::atomic_flag                      nativeResolveProviderErrorLog         = ATOMIC_FLAG_INIT;
std::atomic_uint32_t                  nativeExplicitFactoryLogCount         = 0;
std::atomic_flag                      nativeExplicitFactoryErrorLog         = ATOMIC_FLAG_INIT;

bool claimBoundedLog(std::atomic_uint32_t& count, uint32_t limit) noexcept
{
    uint32_t expected = count.load(std::memory_order_relaxed);
    while (expected < limit)
    {
        if (count.compare_exchange_weak(expected,
                                        expected + 1,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed))
        {
            return true;
        }
    }
    return false;
}

bool readGuestWordAt(uint32_t base, uint32_t offset, uint32_t& value)
{
    if (base > UINT32_MAX - offset)
    {
        return false;
    }
    const uint32_t address = base + offset;
    if (!isGuestReadableRange(address, sizeof(uint32_t)))
    {
        return false;
    }
    value = readGuestU32(address);
    return true;
}

using rerevved::gpu::diagnostics::PassiveTraceEvent;
using rerevved::gpu::diagnostics::PassiveTracePoint;
using rerevved::gpu::diagnostics::PassiveTraceRecordLease;

constexpr uint32_t kDevicePositionOffset       = 0x30;
constexpr uint32_t kDeviceEndOffset            = 0x34;
constexpr uint32_t kPrimarySystemBufferOffset  = 0x2A90;
constexpr uint32_t kIndependentSystemOffset    = 0x2A94;
constexpr uint32_t kReadPointerWritebackOffset = 0x3C;
constexpr uint32_t kCpRbWptrRegister           = 0x01C5;

bool passiveTraceEnabled() noexcept
{
    return rerevved::gpu::diagnostics::GetPassiveTraceBuffer().Enabled();
}

bool fenceTraceEnabled() noexcept
{
    return rex::graphics::diagnostic::GetXenosFenceTrace().enabled();
}

void readRingObservation(uint32_t device, PassiveTraceEvent& event)
{
    event.deviceAddress = device;
    if (!isGuestPointer(device))
    {
        return;
    }

    if (readGuestWordAt(device,
                        kDevicePositionOffset,
                        event.devicePosition))
    {
        event.validFields |=
            rerevved::gpu::diagnostics::TRACE_DEVICE_POSITION;
    }
    if (readGuestWordAt(device, kDeviceEndOffset, event.deviceEnd))
    {
        event.validFields |= rerevved::gpu::diagnostics::TRACE_DEVICE_END;
    }
    if (readGuestWordAt(device,
                        kIndependentSystemOffset,
                        event.systemState2a94))
    {
        event.validFields |= rerevved::gpu::diagnostics::TRACE_SYSTEM_STATE;
    }

    uint32_t primarySystemBuffer = 0;
    if (readGuestWordAt(device,
                        kPrimarySystemBufferOffset,
                        primarySystemBuffer) &&
        readGuestWordAt(primarySystemBuffer,
                        kReadPointerWritebackOffset,
                        event.readPointerWriteback))
    {
        event.readPointerWritebackAddress =
            primarySystemBuffer + kReadPointerWritebackOffset;
        event.validFields |=
            rerevved::gpu::diagnostics::TRACE_READ_POINTER_WRITEBACK;
    }

    rex::graphics::GraphicsSystem* graphicsSystem = nullptr;
    if (REXCVAR_GET(renderer) == "xenos" &&
        REXCVAR_GET(gpu_plugin) == "xenos")
    {
        graphicsSystem = static_cast<rex::graphics::GraphicsSystem*>(
            getGraphicsSystem());
    }
    if (graphicsSystem)
    {
        event.publishedWritePointer =
            (*graphicsSystem->register_file())[kCpRbWptrRegister];
        event.validFields |=
            rerevved::gpu::diagnostics::TRACE_PUBLISHED_WRITE;
    }
}

void recordTracePoint(PassiveTracePoint point) noexcept
{
    if (!passiveTraceEnabled())
    {
        return;
    }
    PassiveTraceEvent event{};
    event.point = point;
    (void)rerevved::gpu::diagnostics::GetPassiveTraceBuffer().Record(event);
}

enum class TracedCaller : uint8_t
{
    None,
    Ordinary,
    Alternate,
};

void recordCallerTracePoint(PassiveTracePoint point) noexcept;

thread_local bool         tracedExactResolveActive = false;
thread_local bool         tracedReservationActive  = false;
thread_local bool         tracedVdswapOwnerActive  = false;
thread_local bool         tracedVdswapActive       = false;
thread_local TracedCaller tracedCaller             = TracedCaller::None;

void recordCallerTracePoint(PassiveTracePoint point) noexcept
{
    if (tracedCaller != TracedCaller::None)
    {
        recordTracePoint(point);
    }
}

} // namespace

void PublishGameplayState()
{
    rerevved::gameplay::PublishFrameSnapshot();
}

void ApplyCombatPaceOverride()
{
    if (REXCVAR_GET(combat_speed) != "fast")
    {
        return;
    }

    constexpr uint32_t kCombatPaceDivisor = 0x82F79FBC;
    constexpr float    kNativeStandard    = 2.0f;
    constexpr float    kNativeAlternate   = 1.5f;
    constexpr float    kNativeFast        = 0.5f;
    if (!isGuestReadableRange(kCombatPaceDivisor, sizeof(uint32_t)))
    {
        return;
    }

    const float selected =
        std::bit_cast<float>(readGuestU32(kCombatPaceDivisor));
    if (selected != kNativeStandard && selected != kNativeAlternate)
    {
        return;
    }

    writeGuestU32Safely(kCombatPaceDivisor,
                        std::bit_cast<uint32_t>(kNativeFast));
}

void FixRushCostDisplay(PPCRegister& r27,
                        PPCRegister& r30,
                        PPCRegister& r31,
                        PPCRegister& r6,
                        PPCRegister& r7,
                        PPCRegister& r11)
{
    rushCostRepair    = {};
    int32_t corrected = 0;
    if (r6.s32 > 1 &&
        rerevved::TryCalculateRushCost(
            r31.s32, r6.s32, r7.s32, corrected) &&
        corrected != r11.s32)
    {
        r11.s32        = corrected;
        rushCostRepair = { true, r27.u32, r30.s32, r7.s32, corrected };
    }
}

void FixRushCostApply(PPCRegister& r25,
                      PPCRegister& r26,
                      PPCRegister& r28,
                      PPCRegister& r3,
                      PPCRegister& r6,
                      PPCRegister& r8)
{
    const rerevved::RushCostRepair repair = rushCostRepair;
    rushCostRepair                        = {};
    rerevved::TryCoordinateRushProduction(
        repair, r28.u32, r25.s32, r26.s32, r6.s32, r8.s32, r3.s32);
}

void CompatNullOptionalDispatch(PPCRegister& r0, PPCRegister& r3)
{
    if (r0.u32 == 0)
    {
        r3.u64 = 0;
    }
}

void CompatRingInitializeBegin(PPCRegister&, PPCRegister&)
{
    if (passiveTraceEnabled())
    {
        tracedExactResolveActive = false;
        tracedReservationActive  = false;
        tracedVdswapOwnerActive  = false;
        tracedVdswapActive       = false;
        tracedCaller             = TracedCaller::None;
        PassiveTraceEvent event{};
        event.point = PassiveTracePoint::RingResetBegin;
        (void)rerevved::gpu::diagnostics::GetPassiveTraceBuffer()
            .BeginObservationEpoch(event);
    }
    auto* graphicsSystem = getGraphicsSystem();
    resumeAfterRingInitialize =
        graphicsSystem && graphicsSystem->PauseAndResetGpuWritePointer();
    if (fenceTraceEnabled())
    {
        rex::graphics::diagnostic::GetXenosFenceTrace()
            .ResetObservationEpoch();
    }
}

void CompatRingInitializeEnd()
{
    auto* graphicsSystem = getGraphicsSystem();
    if (graphicsSystem && resumeAfterRingInitialize)
    {
        graphicsSystem->ResumeGpu();
    }
    resumeAfterRingInitialize = false;
    recordTracePoint(PassiveTracePoint::RingResetReturn);
}

void ObserveNativeDevicePublication(PPCRegister& r11,
                                    PPCRegister& r31)
{
    if (REXCVAR_GET(renderer) != "native")
    {
        return;
    }

    const uint32_t cellAddress   = r11.u32;
    const uint32_t deviceAddress = r31.u32;
    if (!isGuestReadableRange(cellAddress, sizeof(uint32_t)) ||
        !isGuestPointer(deviceAddress))
    {
        REXLOG_ERROR(
            "Native guest device publication invalid: cell={:08X} device={:08X}",
            cellAddress,
            deviceAddress);
        return;
    }

    const uint32_t storedAddress = readGuestU32(cellAddress);
    if (storedAddress != deviceAddress)
    {
        REXLOG_ERROR(
            "Native guest device publication mismatch: cell={:08X} stored={:08X} device={:08X}",
            cellAddress,
            storedAddress,
            deviceAddress);
        return;
    }

    if (rerevved::gpu::PublishGuestDevice(cellAddress, deviceAddress))
    {
        REXLOG_INFO(
            "Native guest device published: cell={:08X} device={:08X}",
            cellAddress,
            deviceAddress);
    }
}

void ObserveNativeTexturePublication(PPCRegister& r22,
                                     PPCRegister& r3)
{
    if (REXCVAR_GET(renderer) != "native" || r3.u32 == 0)
    {
        return;
    }

    const uint32_t     textureAddress = r22.u32;
    const uint32_t     backendAddress = r3.u32;
    constexpr uint32_t widthOffset    = 0x20;
    constexpr uint32_t heightOffset   = 0x24;
    constexpr uint32_t backendOffset  = 0x28;
    if (!isGuestPointer(textureAddress) ||
        !isGuestReadableRange(textureAddress + widthOffset,
                              sizeof(uint32_t) * 3) ||
        !isGuestPointer(backendAddress))
    {
        REXLOG_ERROR(
            "Native guest texture store observation rejected invalid pointers");
        return;
    }

    const uint32_t storedBackend =
        readGuestU32(textureAddress + backendOffset);
    if (storedBackend != backendAddress)
    {
        REXLOG_ERROR(
            "Native guest texture store observation rejected a store mismatch");
        return;
    }

    const uint32_t width    = readGuestU32(textureAddress + widthOffset);
    const uint32_t height   = readGuestU32(textureAddress + heightOffset);
    const uint64_t sequence = rerevved::gpu::ObserveGuestTexture(width, height);
    if (sequence == 1)
    {
        REXLOG_INFO(
            "Native guest texture store observed: size={}x{}",
            width,
            height);
    }
}

void ObserveNativeResolveProviderIdentity(PPCRegister& r3)
{
    if (REXCVAR_GET(renderer) != "native")
    {
        return;
    }

    const uint32_t object = r3.u32;
    if (!isGuestPointer(object))
    {
        if (!nativeResolveProviderErrorLog.test_and_set(
                std::memory_order_relaxed))
        {
            REXLOG_ERROR(
                "Native resolve-provider observation rejected invalid object: object={:08X}",
                object);
        }
        return;
    }

    uint32_t vptr = 0;
    if (!readGuestWordAt(object, 0x00, vptr))
    {
        if (!nativeResolveProviderErrorLog.test_and_set(
                std::memory_order_relaxed))
        {
            REXLOG_ERROR(
                "Native resolve-provider observation rejected unreadable vptr: object={:08X}",
                object);
        }
        return;
    }

    constexpr uint32_t kExplicitBuffersVtable = 0x8204767C;
    if (vptr != kExplicitBuffersVtable)
    {
        if (claimBoundedLog(nativeResolveProviderMismatchLogCount, 8))
        {
            REXLOG_INFO(
                "Native resolve provider observed: object={:08X} vptr={:08X} explicit_match=false",
                object,
                vptr);
        }
        return;
    }

    uint32_t   slot0          = 0;
    uint32_t   slot1          = 0;
    uint32_t   active         = 0;
    const bool slot0Readable  = readGuestWordAt(object, 0x90, slot0);
    const bool slot1Readable  = readGuestWordAt(object, 0x94, slot1);
    const bool activeReadable = readGuestWordAt(object, 0x98, active);
    if (!slot0Readable || !slot1Readable || !activeReadable)
    {
        if (!nativeResolveProviderErrorLog.test_and_set(
                std::memory_order_relaxed))
        {
            REXLOG_ERROR(
                "Native explicit resolve-provider observation rejected unreadable fields: object={:08X} slot90_readable={} slot94_readable={} active_readable={}",
                object,
                slot0Readable,
                slot1Readable,
                activeReadable);
        }
        return;
    }

    const bool     selectedIndexValid = active < 2;
    const uint32_t selected           = active == 0 ? slot0 : slot1;
    const bool     selectedValid =
        selectedIndexValid && selected != 0 &&
        isGuestReadableRange(selected, sizeof(uint32_t));
    if (!claimBoundedLog(nativeResolveProviderMatchLogCount, 8))
    {
        return;
    }

    if (selectedValid)
    {
        REXLOG_INFO(
            "Native resolve provider observed: object={:08X} vptr={:08X} explicit_match=true slot90={:08X} slot94={:08X} active={:08X} selected_index={} selected={:08X} selected_valid={}",
            object,
            vptr,
            slot0,
            slot1,
            active,
            selectedIndexValid ? active : 2,
            selected,
            selectedValid);
    }
    else
    {
        REXLOG_INFO(
            "Native resolve provider observed: object={:08X} vptr={:08X} explicit_match=true slot90={:08X} slot94={:08X} active={:08X} selected_index={} selected_valid={}",
            object,
            vptr,
            slot0,
            slot1,
            active,
            selectedIndexValid ? active : 2,
            selectedValid);
    }
}

void ObserveNativeExplicitBufferFactoryStore(PPCRegister& r28,
                                             PPCRegister& r29,
                                             PPCRegister& r30,
                                             PPCRegister& r3)
{
    if (REXCVAR_GET(renderer) != "native")
    {
        return;
    }

    const uint32_t container      = r28.u32;
    const uint32_t slot           = r29.u32;
    const uint32_t result         = r3.u32;
    uint32_t       vptr           = 0;
    uint32_t       active         = 0;
    const bool     containerValid = isGuestPointer(container);
    const bool     vptrReadable =
        containerValid && readGuestWordAt(container, 0x00, vptr);
    constexpr uint32_t kExplicitBuffersVtable = 0x8204767C;
    const bool         vptrMatches            = vptrReadable && vptr == kExplicitBuffersVtable;
    const bool         activeReadable =
        vptrMatches && readGuestWordAt(container, 0x98, active);
    const bool slotReadable =
        isGuestReadableRange(slot, sizeof(uint32_t));
    const bool     storeMatches   = slotReadable && readGuestU32(slot) == result;
    const bool     slot0          = containerValid && container <= UINT32_MAX - 0x90 &&
                                    slot == container + 0x90;
    const bool     slot1          = containerValid && container <= UINT32_MAX - 0x94 &&
                                    slot == container + 0x94;
    const bool     slotIndexValid = slot0 || slot1;
    const uint32_t slotIndex      = slot1 ? 1 : 0;
    if (!containerValid || !vptrMatches || !activeReadable ||
        !slotReadable || !storeMatches || !slotIndexValid)
    {
        if (!nativeExplicitFactoryErrorLog.test_and_set(
                std::memory_order_relaxed))
        {
            REXLOG_ERROR(
                "Native explicit-buffer factory observation rejected: container={:08X} slot={:08X} result={:08X} remaining={} vptr={:08X} vptr_readable={} vptr_matches={} active_readable={} slot_readable={} store_matches={} slot_index_valid={}",
                container,
                slot,
                result,
                r30.u32,
                vptr,
                vptrReadable,
                vptrMatches,
                activeReadable,
                slotReadable,
                storeMatches,
                slotIndexValid);
        }
        return;
    }

    if (claimBoundedLog(nativeExplicitFactoryLogCount, 2))
    {
        REXLOG_INFO(
            "Native explicit-buffer factory store observed: container={:08X} vptr={:08X} slot_index={} result={:08X} active={:08X} remaining={}",
            container,
            vptr,
            slotIndex,
            result,
            active,
            r30.u32);
    }
}

void TraceReservationEnter(PPCRegister& r3, PPCRegister& r4)
{
    tracedReservationActive = false;
    if (!tracedVdswapOwnerActive || r4.u32 != 64)
    {
        return;
    }

    auto lease = rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
    if (!lease)
    {
        return;
    }
    tracedReservationActive = true;
    PassiveTraceEvent event{};
    event.point           = PassiveTracePoint::ReservationEnter;
    event.requestedDwords = r4.u32;
    readRingObservation(r3.u32, event);
    (void)lease.Commit(event);
}

void TraceReservationReturn(PPCRegister& r3,
                            PPCRegister& r29,
                            PPCRegister& r31)
{
    if (!tracedReservationActive)
    {
        return;
    }
    tracedReservationActive = false;
    auto lease              = rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
    if (!lease)
    {
        return;
    }
    PassiveTraceEvent event{};
    event.point              = PassiveTracePoint::ReservationReturn;
    event.requestedDwords    = r29.u32;
    event.reservationAddress = r3.u32;
    readRingObservation(r31.u32, event);
    (void)lease.Commit(event);
}

void TraceVdSwapOwnerEnter(PPCRegister& r3, PPCRegister& r4)
{
    tracedVdswapOwnerActive = false;
    if (tracedCaller == TracedCaller::None)
    {
        return;
    }
    auto lease = rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
    if (!lease)
    {
        return;
    }
    tracedVdswapOwnerActive = true;
    PassiveTraceEvent event{};
    event.point                  = PassiveTracePoint::VdSwapOwnerEnter;
    event.resolveResourceAddress = r4.u32;
    readRingObservation(r3.u32, event);
    (void)lease.Commit(event);
}

void TraceVdSwapOwnerReturn(PPCRegister& r31)
{
    if (tracedVdswapOwnerActive)
    {
        auto lease =
            rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
        if (lease)
        {
            PassiveTraceEvent event{};
            event.point = PassiveTracePoint::VdSwapOwnerReturn;
            readRingObservation(r31.u32, event);
            (void)lease.Commit(event);
        }
    }
    tracedReservationActive = false;
    tracedVdswapOwnerActive = false;
    tracedVdswapActive      = false;
}

void ObserveRendererResolve(PPCRegister& r4,
                            PPCRegister& r6,
                            PPCRegister& r8,
                            PPCRegister& r9,
                            uint64_t     lr)
{
    constexpr uint32_t kFetchDescriptorOffset = 0x1C;
    constexpr uint32_t kExactResolveCallsite  = 0x8250AFEC;
    const uint32_t     returnAddress          = static_cast<uint32_t>(lr);
    const uint32_t     callAddress =
        returnAddress >= 4 ? returnAddress - 4 : 0;
    const bool descriptorAddressValid =
        r6.u32 <= UINT32_MAX - kFetchDescriptorOffset;
    const uint32_t descriptorAddress =
        descriptorAddressValid ? r6.u32 + kFetchDescriptorOffset : 0;
    PassiveTraceRecordLease traceLease{};
    if (callAddress == kExactResolveCallsite)
    {
        traceLease =
            rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
    }
    tracedExactResolveActive = static_cast<bool>(traceLease);
    rerevved::gpu::GuestFetchDescriptor descriptor{};
    const bool                          descriptorValid =
        descriptorAddressValid &&
        readGuestFetchDescriptor(descriptorAddress, descriptor);

    if (tracedExactResolveActive)
    {
        PassiveTraceEvent event{};
        event.point                  = PassiveTracePoint::ResolveEnter;
        event.resolveResourceAddress = r6.u32;
        event.descriptorAddress      = descriptorAddress;
        event.resolveCallAddress     = callAddress;
        event.resolveFlags           = r4.u32;
        event.resolveMipLevel        = r8.u32;
        event.resolveSlice           = r9.u32;
        if (descriptorValid)
        {
            event.descriptor = descriptor;
            event.validFields |=
                rerevved::gpu::diagnostics::TRACE_DESCRIPTOR;
        }
        (void)traceLease.Commit(event);
    }

    if (!descriptorValid)
    {
        return;
    }

    const uint64_t sequence = rerevved::gpu::ObserveGuestResolve(
        descriptor, callAddress, r4.u32, r8.u32, r9.u32);
    if (sequence == 1)
    {
        REXLOG_INFO(
            "Renderer resolve observed: call={:08X} flags={:08X} mip={} slice={} destination_base={:08X}",
            callAddress,
            r4.u32,
            r8.u32,
            r9.u32,
            descriptor[1] & 0xFFFFF000u);
    }
}

void TraceResolveReturn()
{
    if (!tracedExactResolveActive)
    {
        return;
    }
    recordTracePoint(PassiveTracePoint::ResolveReturn);
    tracedExactResolveActive = false;
}

void ObserveRendererSwapSource(PPCRegister& r3,
                               PPCRegister& r4,
                               PPCRegister& r30,
                               PPCRegister& r31)
{
    PassiveTraceRecordLease traceLease{};
    if (tracedVdswapOwnerActive)
    {
        traceLease =
            rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
    }
    rerevved::gpu::GuestFetchDescriptor descriptor{};
    const bool                          descriptorValid = readGuestFetchDescriptor(r4.u32, descriptor);
    tracedVdswapActive                                  = static_cast<bool>(traceLease);
    if (tracedVdswapActive)
    {
        PassiveTraceEvent event{};
        event.point              = PassiveTracePoint::VdSwapCall;
        event.requestedDwords    = 64;
        event.reservationAddress = r30.u32;
        event.vdswapArgument     = r3.u32;
        event.descriptorAddress  = r4.u32;
        readRingObservation(r31.u32, event);
        if (descriptorValid)
        {
            event.descriptor = descriptor;
            event.validFields |=
                rerevved::gpu::diagnostics::TRACE_DESCRIPTOR;
        }
        (void)traceLease.Commit(event);
    }

    if (!descriptorValid)
    {
        return;
    }

    const rerevved::gpu::GuestSwapCorrelation correlation =
        rerevved::gpu::ObserveGuestSwap(descriptor);
    if (correlation.match == rerevved::gpu::GuestFetchMatch::Exact &&
        correlation.matchedCount == 1)
    {
        REXLOG_INFO(
            "Renderer swap source exactly matched resolve: resolve_call={:08X} resolve_flags={:08X} mip={} slice={} resolve_sequence={} swap_sequence={} source_base={:08X}",
            correlation.resolveCallAddress,
            correlation.resolveFlags,
            correlation.resolveMipLevel,
            correlation.resolveSlice,
            correlation.resolveSequence,
            correlation.swapSequence,
            descriptor[1] & 0xFFFFF000u);
    }
    else if (correlation.match ==
                 rerevved::gpu::GuestFetchMatch::BaseAddressCandidate &&
             correlation.candidateCount == 1)
    {
        REXLOG_INFO(
            "Renderer swap source has base-address resolve candidate: resolve_call={:08X} resolve_flags={:08X} mip={} slice={} resolve_sequence={} swap_sequence={} source_base={:08X}",
            correlation.resolveCallAddress,
            correlation.resolveFlags,
            correlation.resolveMipLevel,
            correlation.resolveSlice,
            correlation.resolveSequence,
            correlation.swapSequence,
            descriptor[1] & 0xFFFFF000u);
    }
    else if (correlation.match == rerevved::gpu::GuestFetchMatch::None &&
             correlation.unmatchedCount == 1)
    {
        REXLOG_INFO(
            "Renderer swap source has no retained resolve match: swap_sequence={} source_base={:08X}",
            correlation.swapSequence,
            descriptor[1] & 0xFFFFF000u);
    }
}

void TraceVdSwapReturn(PPCRegister& r3,
                       PPCRegister& r30,
                       PPCRegister& r31)
{
    if (fenceTraceEnabled() &&
        (r30.u32 & (alignof(uint32_t) - 1)) == 0 &&
        isGuestReadableRange(r30.u32, sizeof(uint32_t)))
    {
        const uint32_t physicalAddress =
            REX_KERNEL_MEMORY()->GetPhysicalAddress(r30.u32);
        if (physicalAddress != UINT32_MAX)
        {
            (void)rex::graphics::diagnostic::GetXenosFenceTrace()
                .WatchSwapReservation(r30.u32, physicalAddress);
        }
    }
    if (!tracedVdswapActive)
    {
        return;
    }
    auto lease = rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
    if (!lease)
    {
        return;
    }
    PassiveTraceEvent event{};
    event.point              = PassiveTracePoint::VdSwapReturn;
    event.requestedDwords    = 64;
    event.reservationAddress = r30.u32;
    event.returnValue        = r3.u32;
    readRingObservation(r31.u32, event);
    (void)lease.Commit(event);
}

void TraceVdSwapPublished(PPCRegister& r30, PPCRegister& r31)
{
    if (!tracedVdswapActive)
    {
        tracedVdswapActive = false;
        return;
    }
    auto lease = rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
    if (!lease)
    {
        tracedVdswapActive = false;
        return;
    }
    PassiveTraceEvent event{};
    event.point              = PassiveTracePoint::VdSwapPublished;
    event.requestedDwords    = 64;
    event.reservationAddress = r30.u32;
    readRingObservation(r31.u32, event);
    constexpr uint32_t kReservationBytes =
        sizeof(uint32_t) *
        rerevved::gpu::diagnostics::kPassiveTraceReservationDwords;
    // RVA-F-0128 proves r30 is the 64-dword reservation returned by the helper.
    // VdSwap receives r30 + 4, but this capture intentionally covers the full
    // reservation from r30 through the published r30 + 256 boundary.
    if ((r30.u32 & (alignof(uint32_t) - 1)) == 0 &&
        isGuestReadableRange(r30.u32, kReservationBytes))
    {
        for (std::size_t index = 0;
             index < event.reservationWords.size();
             ++index)
        {
            event.reservationWords[index] = readGuestU32(
                r30.u32 + static_cast<uint32_t>(index * sizeof(uint32_t)));
        }
        event.validFields |=
            rerevved::gpu::diagnostics::TRACE_RESERVATION_WORDS;
    }
    (void)lease.Commit(event);
    tracedVdswapActive = false;
}

void TracePreSwapEnter()
{
    recordCallerTracePoint(PassiveTracePoint::PreSwapEnter);
}

void TracePreSwapReturn()
{
    recordCallerTracePoint(PassiveTracePoint::PreSwapReturn);
}

void TraceEmitterCd20Enter()
{
    recordCallerTracePoint(PassiveTracePoint::EmitterCd20Enter);
}

void TraceEmitterCd20Return()
{
    recordCallerTracePoint(PassiveTracePoint::EmitterCd20Return);
}

void TraceEmitterBf40Enter()
{
    recordCallerTracePoint(PassiveTracePoint::EmitterBf40Enter);
}

void TraceEmitterBf40Return()
{
    recordCallerTracePoint(PassiveTracePoint::EmitterBf40Return);
}

void TraceCallbackEnter()
{
    recordCallerTracePoint(PassiveTracePoint::CallbackEnter);
}

void TraceCallbackReturn()
{
    recordCallerTracePoint(PassiveTracePoint::CallbackReturn);
}

void TraceOrdinaryCallerEnter()
{
    if (!passiveTraceEnabled())
    {
        return;
    }
    tracedCaller = TracedCaller::Ordinary;
    recordTracePoint(PassiveTracePoint::OrdinaryCallerEnter);
}

void TraceOrdinaryCallerReturn()
{
    if (tracedCaller == TracedCaller::Ordinary)
    {
        recordTracePoint(PassiveTracePoint::OrdinaryCallerReturn);
        tracedReservationActive = false;
        tracedVdswapOwnerActive = false;
        tracedVdswapActive      = false;
        tracedCaller            = TracedCaller::None;
    }
}

void TraceAlternateCallerEnter()
{
    if (!passiveTraceEnabled())
    {
        return;
    }
    tracedCaller = TracedCaller::Alternate;
    recordTracePoint(PassiveTracePoint::AlternateCallerEnter);
}

void TraceAlternateCallerReturn()
{
    if (tracedCaller == TracedCaller::Alternate)
    {
        recordTracePoint(PassiveTracePoint::AlternateCallerReturn);
        tracedReservationActive = false;
        tracedVdswapOwnerActive = false;
        tracedVdswapActive      = false;
        tracedCaller            = TracedCaller::None;
    }
}

void RememberGfxRenderConfig(PPCRegister& r3, PPCRegister& r4)
{
    gfxRenderConfigCandidate = r3.u32;
    gfxRenderConfigRenderer  = r4.u32;
}

void HandleGfxRenderCapsBegin(PPCRegister& r3,
                              PPCRegister& r4,
                              uint64_t     lr)
{
    if (gfxRenderCaps.observing)
    {
        return;
    }

    gfxRenderCaps = {
        r3.u32,
        r4.u32,
        static_cast<uint32_t>(lr),
        true,
    };
}

void HandleGfxRenderCapsEnd(PPCRegister& r3, PPCRegister& r31)
{
    if (!gfxRenderCaps.observing)
    {
        return;
    }

    const auto state         = gfxRenderCaps;
    gfxRenderCaps            = {};
    const bool outputMatches = r31.u32 == state.output;
    const bool outputReadable =
        outputMatches && isGuestReadableRange(r31.u32, 16);
    // GFx copies the failed query output, so retain that config and refresh it
    // only from a valid result produced by the same renderer.
    if (state.caller == 0x82245130 && r3.u32 == 0 &&
        gfxRenderConfigRenderer == state.renderer &&
        isGuestReadableRange(gfxRenderConfigCandidate + 0x14, 0x18))
    {
        gfxStaleRenderConfig.store(gfxRenderConfigCandidate,
                                   std::memory_order_release);
        gfxStaleRenderConfigRenderer.store(state.renderer,
                                           std::memory_order_release);
    }
    else if (r3.u32 != 0 && outputReadable &&
             gfxStaleRenderConfigRenderer.load(std::memory_order_acquire) ==
                 state.renderer)
    {
        const uint32_t config =
            gfxStaleRenderConfig.load(std::memory_order_acquire);
        if (config != 0 && isGuestReadableRange(config + 0x14, 0x18) &&
            readGuestU32(config + 0x14) == state.renderer &&
            writeGuestU32Safely(config + 0x24, readGuestU32(r31.u32)) &&
            writeGuestU32Safely(config + 0x28, readGuestU32(r31.u32 + 4)))
        {
            gfxStaleRenderConfig.store(0, std::memory_order_release);
            gfxStaleRenderConfigRenderer.store(0, std::memory_order_release);
        }
    }
}
