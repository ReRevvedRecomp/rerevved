// Hooks preserve title behavior unless documented as a compatibility repair.

#include <atomic>
#include <bit>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "display_aspect_hooks.h"
#include "gameplay_session_settings.h"
#include "gameplay_state.h"
#include "gpu/completion/guest_progress_hooks.h"
#include "gpu/diagnostics/native_renderer_guest_state.h"
#include "gpu/guest_d3d/developer_hooks.h"
#include "gpu/renderer_backend.h"
#include "rush_cost.h"
#include "xenos_frame_counter.h"
#if REREVVED_DEVELOPER
#include "gpu/diagnostics/scene_resource_capture.h"
#include "gpu/diagnostics/shader_harvest.h"
#include "gpu/diagnostics/xenos_frame_reference.h"
#endif

REXCVAR_DECLARE(std::string, combat_speed);
REXCVAR_DECLARE(bool, xenos_frame_counter);

// The generated mid-body hooks call these in every build; only a developer build records.
void ObserveSceneRegionLocked([[maybe_unused]] PPCRegister& r1, [[maybe_unused]] PPCRegister& r29, [[maybe_unused]] PPCRegister& r22, [[maybe_unused]] PPCRegister& r31, [[maybe_unused]] PPCRegister& r20)
{
#if REREVVED_DEVELOPER
    using namespace rerevved::gpu::diagnostics;
    if (auto* s = sceneCoverageSession.load(std::memory_order_acquire))
        ObserveSceneWriterRegion(s, false, r1.u32, r29.u32 - 16, r22.u32, r31.u32, r20.u32);
#endif
}

void ObserveSceneRegionWritten([[maybe_unused]] PPCRegister& r1, [[maybe_unused]] PPCRegister& r29, [[maybe_unused]] PPCRegister& r22, [[maybe_unused]] PPCRegister& r31, [[maybe_unused]] PPCRegister& r20)
{
#if REREVVED_DEVELOPER
    using namespace rerevved::gpu::diagnostics;
    if (auto* s = sceneCoverageSession.load(std::memory_order_acquire))
        ObserveSceneWriterRegion(s, true, r1.u32, r29.u32 - 16, r22.u32, r31.u32, r20.u32);
#endif
}

namespace
{

// Pair the last decoded policy with its device; unknown devices use interval_other.
std::atomic<uint64_t> decodedSwapPolicy{ rerevved::xenos_frames::kUnknownInterval };
thread_local uint32_t attemptedSwapInterval = rerevved::xenos_frames::kUnknownInterval;

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

} // namespace

void PublishGameplayState()
{
    rerevved::gameplay::PublishFrameSnapshot();
}

void ApplyCombatPaceOverride()
{
    const auto host = rerevved::gameplay_session::ForNetworkGame();
    if (host ? host->combatSpeed != 1 : REXCVAR_GET(combat_speed) != "fast")
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

void CompatRingInitializeBegin([[maybe_unused]] PPCRegister& r3, [[maybe_unused]] PPCRegister& r4)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingRing(r3.u32, r4.u32);
#endif
    auto* graphicsSystem = getGraphicsSystem();
    resumeAfterRingInitialize =
        graphicsSystem && graphicsSystem->PauseAndResetGpuWritePointer();
}

void CompatRingInitializeEnd()
{
    auto* graphicsSystem = getGraphicsSystem();
    if (graphicsSystem && resumeAfterRingInitialize)
    {
        graphicsSystem->ResumeGpu();
    }
    resumeAfterRingInitialize = false;
}

void ObserveNativeDevicePublication(PPCRegister& r11,
                                    PPCRegister& r31)
{
#if REREVVED_DEVELOPER
    if (rerevved::gpu::guest_d3d::CompletionProbeRequested())
        rerevved::gpu::guest_d3d::ObserveCompletionSample(0x826A645C, r31.u32, rerevved::gpu::guest_d3d::CompletionThread(), {}, false, "publication");
#endif
    if (rerevved::gpu::SelectedRendererBackend() != rerevved::gpu::RendererBackend::Native)
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
    if (rerevved::gpu::SelectedRendererBackend() != rerevved::gpu::RendererBackend::Native || r3.u32 == 0)
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
    if (rerevved::gpu::SelectedRendererBackend() != rerevved::gpu::RendererBackend::Native)
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
    if (rerevved::gpu::SelectedRendererBackend() != rerevved::gpu::RendererBackend::Native)
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

// Completion ledger records for renderer=native; inert without a published ledger.
void RecordGuestClearTarget(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r7, PPCRegister& r8, PPCRegister& r9, PPCRegister& r10, PPCRegister& r1, PPCRegister& f1)
{
    rerevved::gpu::completion::detail::RecordGuestClear({ r3.u32, r4.u32, r5.u32, r6.u32, r7.u32, r8.u32, r9.u32, r10.u32 }, r1.u32, f1.f64);
#if REREVVED_DEVELOPER
    if (const auto harvest = rerevved::gpu::diagnostics::shaderHarvestObserver.load(std::memory_order_acquire)) [[unlikely]]
        harvest({ rerevved::gpu::diagnostics::kHarvestClearEntry, 0, { r3.u32, r4.u32, r5.u32, r6.u32, r7.u32, r8.u32, r9.u32, r10.u32 }, r1.u32, 0, f1.f64 });
#endif
}

// The exit of the Clear body 0x826A22F8, which closes the native_clear_derive_check and native_clear_effect_check probes.
void RecordGuestClearReturn(PPCRegister& r1)
{
    rerevved::gpu::completion::detail::RecordGuestClearReturn(r1.u32);
}

void RecordGuestResolveWords(PPCRegister& r31, PPCRegister& r1)
{
    rerevved::gpu::completion::detail::RecordGuestResolveCopy(r31.u32, r1.u32);
#if REREVVED_DEVELOPER
    if (const auto harvest = rerevved::gpu::diagnostics::shaderHarvestObserver.load(std::memory_order_acquire)) [[unlikely]]
        harvest({ rerevved::gpu::diagnostics::kHarvestResolveCopyEntry, 0, { r31.u32 } });
#endif
}

void RecordPresentFlipRequest(PPCRegister& r31)
{
    rerevved::gpu::completion::detail::RecordPresentFlip(r31.u32, true);
}

void RecordPresentFlipWait(PPCRegister& r31)
{
    rerevved::gpu::completion::detail::RecordPresentFlip(r31.u32, false);
}

// 0x8269C8E0 after its drain, before it frees B and S2.
void CloseGuestRingEpochWrites()
{
    rerevved::gpu::completion::detail::CloseRingEpochWrites();
}

// COHER_STATUS_HOST 0x82697CA0 waits on 0xA31 mask 0x80000000 equal 0; it and
// display waits are GPU ordering barriers with no guest writes. Event writes
// have no completion model and hold their spans.
void RecordCoherBarrier82697E28()
{
    rerevved::gpu::completion::detail::RecordBarrier(0x82697E28);
}

// 0x826980A0 COHER_STATUS_HOST wait
void RecordCoherBarrier82698118()
{
    rerevved::gpu::completion::detail::RecordBarrier(0x82698118);
}

// 0x8269BAE8 COHER_STATUS_HOST wait
void RecordCoherBarrier8269BB94()
{
    rerevved::gpu::completion::detail::RecordBarrier(0x8269BB94);
}

// 0x8269BAE8 COHER_STATUS_HOST wait
void RecordCoherBarrier8269BC54()
{
    rerevved::gpu::completion::detail::RecordBarrier(0x8269BC54);
}

// 0x826A4458 WAIT_REG_MEM on AVIVO_D1SCL_UPDATE: a GPU-only display wait
void RecordDisplayWait826A44E0()
{
    rerevved::gpu::completion::detail::RecordDisplayWait(0x826A44E0);
}

// 0x826A4458 WAIT_REG_MEM on AVIVO_D1SCL_UPDATE: a GPU-only display wait
void RecordDisplayWait826A45C8()
{
    rerevved::gpu::completion::detail::RecordDisplayWait(0x826A45C8);
}

// 0x826ACD20 COHER_STATUS_HOST wait
void RecordCoherBarrier826ACD84()
{
    rerevved::gpu::completion::detail::RecordBarrier(0x826ACD84);
}

// 0x826A9BA0 EVENT_WRITE_SHD, initiator 0x80000003: r30 = va, r31 = G
void RecordGpuTimestamp(PPCRegister& r30, PPCRegister& r31)
{
    rerevved::gpu::completion::detail::RecordGpuTimestamp(r30.u32, r31.u32);
}

// 0x826A9C30 EVENT_WRITE_SHD, initiator 0x80000003
void RecordUnmodeledProgress826A9CA8()
{
    rerevved::gpu::completion::detail::RecordUnmodeled(0x826A9CA8);
}

// 0x826AB1C0 EVENT_WRITE_SHD
void RecordUnmodeledProgress826AB268()
{
    rerevved::gpu::completion::detail::RecordUnmodeled(0x826AB268);
}

// 0x8269C400 while G+0x33B0 != 0 appends {0x81000000 | r30 dwords, r29 phys}
// to the tiled list instead of kicking.
void RecordTiledSegment(PPCRegister& r29, PPCRegister& r30)
{
    rerevved::gpu::completion::detail::RecordTiledSegment(r29.u32, r30.u32);
}

// BeginTiling 0x826AC0A8 moves the previous list to G+0x344C and starts a new one.
void BeginTiledList()
{
    rerevved::gpu::completion::detail::BeginTiledList();
}

// 0x8269BD98 under its lock with G+0x2B00 != 0 chains {r28 phys, r27 dwords}
// onto the worker's list instead of kicking.
void RecordDeferredKick(PPCRegister& r28, PPCRegister& r27)
{
    rerevved::gpu::guest_d3d::NoteTilingSubmitArm(rerevved::gpu::guest_d3d::TilingArm::Deferred);
    rerevved::gpu::completion::detail::RecordDeferredKick(r28.u32, r27.u32);
}

// 0x8269BD98 with G+0x2B00 == 0 submits {r28 phys, r27 dwords} through
// 0x8269B900 after raising G+0x2B00 by its r7 argument.
void RecordDirectSubmit(PPCRegister& r28, PPCRegister& r27)
{
    rerevved::gpu::guest_d3d::NoteTilingSubmitArm(rerevved::gpu::guest_d3d::TilingArm::Direct);
    rerevved::gpu::completion::detail::RecordDirectSubmit(r28.u32, r27.u32);
}

// 0x826AB740 token 0x81 submits {r10 dwords, r11 phys} through 0x8269B900.
void RecordReplaySubmit826AB890(PPCRegister& r11, PPCRegister& r10)
{
    rerevved::gpu::completion::detail::RecordReplaySubmit(r11.u32, r10.u32, 0x826AB890);
}

// 0x826AB740 token 0x8E on tile 0 submits {r11 dwords, r10 phys} through 0x8269B900.
void RecordReplaySubmit826AB990(PPCRegister& r10, PPCRegister& r11)
{
    rerevved::gpu::completion::detail::RecordReplaySubmit(r10.u32, r11.u32, 0x826AB990);
}

// 0x826ABB90, called only by the worker thread entry 0x826ABEA8.
void MarkReplayWorker()
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingInterpreter();
#endif
    rerevved::gpu::completion::detail::MarkReplayWorker();
}

// The tile-replay observers below belong to the developer tiling probe and only
// read the guest. The generated mid-body hooks call them in every build; only a
// developer build observes.

// The interpreter 0x826ABB90 returns through the branch at 0x826ABC48.
void ObserveTilingInterpreterExit()
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingInterpreterExit();
#endif
}

// The interpreter 0x826ABB90 reads the list word at r5 at 0x826ABD0C; at
// 0x826ABD6C it calls the function in r11 with W = r31.
void ObserveTilingListWord([[maybe_unused]] PPCRegister& r5)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingListWord(r5.u32);
#endif
}

void ObserveTilingCallback([[maybe_unused]] PPCRegister& r11, [[maybe_unused]] PPCRegister& r31)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingCallback(r11.u32, r31.u32);
#endif
}

// 0x826AB740 dispatches the token word r10 at 0x826AB770.
void ObserveTilingHandlerToken([[maybe_unused]] PPCRegister& r10)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingHandlerToken(r10.u32);
#endif
}

// The handler follows the link word r10 itself at 0x826AB960.
void ObserveTilingHandlerLink([[maybe_unused]] PPCRegister& r10)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingHandlerLink(r10.u32);
#endif
}

// The four list-selected stores: 0x826AB120 writes P+8 for P = r31, the others
// write 0(r9), 0(r3) and 0(r8).
void ObserveTilingWrite826AB120([[maybe_unused]] PPCRegister& r31)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingWrite(rerevved::gpu::guest_d3d::TilingWriteSite::B120, r31.u32 + 8);
#endif
}

void ObserveTilingWrite826AB4C8([[maybe_unused]] PPCRegister& r9)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingWrite(rerevved::gpu::guest_d3d::TilingWriteSite::B4C8, r9.u32);
#endif
}

void ObserveTilingWrite826AB60C([[maybe_unused]] PPCRegister& r3)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingWrite(rerevved::gpu::guest_d3d::TilingWriteSite::B60C, r3.u32);
#endif
}

void ObserveTilingWrite826AB6FC([[maybe_unused]] PPCRegister& r8)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingWrite(rerevved::gpu::guest_d3d::TilingWriteSite::B6FC, r8.u32);
#endif
}

// A worker thread finds its queue empty at 0x826ABF20 and parks: r25 = W.
void ObserveTilingWorkerIdle([[maybe_unused]] PPCRegister& r25)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingWorkerIdle(r25.u32);
#endif
}

// EndTiling 0x826AC218 tests the optional submit's guard r6 at 0x826AC348.
void ObserveTilingSubmitGuard([[maybe_unused]] PPCRegister& r6)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingGuard(r6.u32);
#endif
}

// The publisher 0x8269B900 at its entry: r3 = G.
void ObserveTilingPublisher([[maybe_unused]] PPCRegister& r3)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingPublisher(r3.u32);
#endif
}

// The watchdog 0x826A5B48: entry (r3 = G), the return at the global gate, the
// reset path at the G+0x2B00 store and the reread-zero path before the trap.
void ObserveTilingWatchdogEnter([[maybe_unused]] PPCRegister& r3)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingWatchdog(0, r3.u32);
#endif
}

void ObserveTilingWatchdogGlobal([[maybe_unused]] PPCRegister& r31)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingWatchdog(1, r31.u32);
#endif
}

void ObserveTilingWatchdogReset([[maybe_unused]] PPCRegister& r31)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveCompletionReset(r31.u32);
    rerevved::gpu::guest_d3d::ObserveTilingWatchdog(2, r31.u32);
#endif
}

void ObserveTilingWatchdogZero([[maybe_unused]] PPCRegister& r31)
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ObserveTilingWatchdog(3, r31.u32);
#endif
}

// 0x8268CF60 after storing G+0x2ABE |= 2 and the device owner, inside the
// G+0x3A60 section. Its stwu r1,-112 frame saved lr at r1+104.
void RecordDeviceSuspend(PPCRegister& r1)
{
    rerevved::gpu::completion::detail::RecordDeviceSuspend(r1.u32);
}

// 0x8268CFF8 after clearing G+0x2ABE bit 0x2, inside the G+0x3A60 section.
void RecordDeviceResume()
{
    rerevved::gpu::completion::detail::RecordDeviceResume();
}

// 0x8268CF10 (device acquire) after storing the caller's thread id in the
// owner G+0x2A88.
void RecordDeviceAcquire()
{
    rerevved::gpu::completion::detail::RecordDeviceAcquire();
}

// 0x8268CF50 (device release) at its store clearing G+0x2A88.
void RecordDeviceRelease()
{
    rerevved::gpu::completion::detail::RecordDeviceRelease();
}

// 0x826ABEA8 found G+0x2ABE bit 0x2 set under G+0x3A60 and calls the
// pre-swap 0x826A4630 and the swap 0x826A4638 itself: r30 = G.
void BeginDeferredSwap(PPCRegister& r30)
{
    rerevved::gpu::completion::detail::BeginDeferredSwap(r30.u32);
}

// 0x826ABF94, still under G+0x3A60, after the swap 0x826A4638 or the
// branch taken when G+0x2ABE bit 0x2 is clear: r30 = G.
void EndDeferredSwap(PPCRegister& r30)
{
    rerevved::gpu::completion::detail::EndDeferredSwap(r30.u32);
}

void TraceResolveReturn(PPCRegister& r1)
{
    rerevved::gpu::completion::detail::RecordGuestResolveReturn(r1.u32);
}

void ObserveDecodedSwapInterval(PPCRegister& r11, PPCRegister& r31)
{
    if (REXCVAR_GET(xenos_frame_counter))
    {
        decodedSwapPolicy.store((uint64_t{ r31.u32 } << 32) | r11.u32, std::memory_order_relaxed);
    }
}

void ObserveRendererSwapSource(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister& r31)
{
    if (REXCVAR_GET(xenos_frame_counter))
    {
        const uint64_t policy = decodedSwapPolicy.load(std::memory_order_relaxed);
        attemptedSwapInterval = (policy >> 32) == r31.u32
                                    ? static_cast<uint32_t>(policy)
                                    : rerevved::xenos_frames::kUnknownInterval;
        rerevved::xenos_frames::RecordAttempt();
    }
    // Publishes the frozen frame policy ahead of the VdSwap that follows.
    rerevved::display_aspect::OnGuestSwap();
#if REREVVED_DEVELOPER
    rerevved::gpu::diagnostics::NotifyXenosFrameReferenceBoundary();
#endif
}

void TraceVdSwapReturn()
{
    if (REXCVAR_GET(xenos_frame_counter))
    {
        rerevved::xenos_frames::RecordReturn(attemptedSwapInterval);
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
