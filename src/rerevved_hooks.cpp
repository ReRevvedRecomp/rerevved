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
REXCVAR_DEFINE_STRING(combat_speed, "normal", "ReRevved/Combat", "Combat presentation speed")
    .allowed({ "normal", "fast" });

namespace
{

rex::system::IGraphicsSystem* GetGraphicsSystem()
{
    auto* kernel_state = REX_KERNEL_STATE();
    if (!kernel_state || !kernel_state->emulator())
    {
        return nullptr;
    }

    return kernel_state->emulator()->graphics_system();
}

bool IsGuestPointer(uint32_t address)
{
    return address >= 0x10000 && address < 0xFFFFF000;
}

struct CheckedAddressRange
{
    uint32_t end;
    bool     valid;
};

constexpr CheckedAddressRange MakeCheckedAddressRange(uint32_t base,
                                                      uint32_t extent,
                                                      uint32_t ceiling)
{
    return extent == 0 || extent > ceiling || base > UINT32_MAX - extent
               ? CheckedAddressRange{ 0, false }
               : CheckedAddressRange{ base + extent, true };
}

bool IsGuestReadableRange(uint32_t address, uint32_t extent)
{
    const auto range = MakeCheckedAddressRange(address, extent, UINT32_MAX);
    if (!range.valid || !IsGuestPointer(address) ||
        !IsGuestPointer(range.end - 1))
    {
        return false;
    }
    auto* memory = REX_KERNEL_MEMORY();
    auto* heap   = memory->LookupHeap(address);
    return heap && memory->LookupHeap(range.end - 1) == heap &&
           heap->QueryRangeAccess(address, range.end - 1) !=
               rex::memory::PageAccess::kNoAccess;
}

uint32_t ReadGuestU32(uint32_t address)
{
    if (!IsGuestPointer(address))
    {
        return 0;
    }
    const auto* memory = REX_KERNEL_MEMORY();
    const auto* source = memory->TranslateVirtual<const uint8_t*>(address);
    return (uint32_t{ source[0] } << 24) | (uint32_t{ source[1] } << 16) |
           (uint32_t{ source[2] } << 8) | uint32_t{ source[3] };
}

bool ReadGuestFetchDescriptor(
    uint32_t                             address,
    rerevved::gpu::GuestFetchDescriptor& descriptor)
{
    constexpr uint32_t kDescriptorSize =
        sizeof(uint32_t) * rerevved::gpu::kGuestFetchDwordCount;
    if (!IsGuestReadableRange(address, kDescriptorSize))
    {
        return false;
    }
    for (std::size_t i = 0; i < descriptor.size(); ++i)
    {
        descriptor[i] = ReadGuestU32(
            address + static_cast<uint32_t>(i * sizeof(uint32_t)));
    }
    return true;
}

bool WriteGuestU32Safely(uint32_t address, uint32_t value)
{
    if (!IsGuestReadableRange(address, sizeof(uint32_t)))
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

thread_local bool resume_after_ring_initialize = false;

struct GfxRenderCapsState
{
    uint32_t renderer;
    uint32_t output;
    uint32_t caller;
    bool     observing;
};

thread_local GfxRenderCapsState       gfx_render_caps{};
thread_local uint32_t                 gfx_render_config_candidate = 0;
thread_local uint32_t                 gfx_render_config_renderer  = 0;
thread_local rerevved::RushCostRepair rush_cost_repair{};
std::atomic_uint32_t                  gfx_stale_render_config                    = 0;
std::atomic_uint32_t                  gfx_stale_render_config_renderer           = 0;
std::atomic_uint32_t                  native_resolve_provider_match_log_count    = 0;
std::atomic_uint32_t                  native_resolve_provider_mismatch_log_count = 0;
std::atomic_flag                      native_resolve_provider_error_log          = ATOMIC_FLAG_INIT;
std::atomic_uint32_t                  native_explicit_factory_log_count          = 0;
std::atomic_flag                      native_explicit_factory_error_log          = ATOMIC_FLAG_INIT;

bool ClaimBoundedLog(std::atomic_uint32_t& count, uint32_t limit) noexcept
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

bool ReadGuestWordAt(uint32_t base, uint32_t offset, uint32_t& value)
{
    if (base > UINT32_MAX - offset)
    {
        return false;
    }
    const uint32_t address = base + offset;
    if (!IsGuestReadableRange(address, sizeof(uint32_t)))
    {
        return false;
    }
    value = ReadGuestU32(address);
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

bool PassiveTraceEnabled() noexcept
{
    return rerevved::gpu::diagnostics::GetPassiveTraceBuffer().enabled();
}

bool FenceTraceEnabled() noexcept
{
    return rex::graphics::diagnostic::GetXenosFenceTrace().enabled();
}

void ReadRingObservation(uint32_t device, PassiveTraceEvent& event)
{
    event.device_address = device;
    if (!IsGuestPointer(device))
    {
        return;
    }

    if (ReadGuestWordAt(device,
                        kDevicePositionOffset,
                        event.device_position))
    {
        event.valid_fields |=
            rerevved::gpu::diagnostics::kTraceDevicePosition;
    }
    if (ReadGuestWordAt(device, kDeviceEndOffset, event.device_end))
    {
        event.valid_fields |= rerevved::gpu::diagnostics::kTraceDeviceEnd;
    }
    if (ReadGuestWordAt(device,
                        kIndependentSystemOffset,
                        event.system_state_2a94))
    {
        event.valid_fields |= rerevved::gpu::diagnostics::kTraceSystemState;
    }

    uint32_t primary_system_buffer = 0;
    if (ReadGuestWordAt(device,
                        kPrimarySystemBufferOffset,
                        primary_system_buffer) &&
        ReadGuestWordAt(primary_system_buffer,
                        kReadPointerWritebackOffset,
                        event.read_pointer_writeback))
    {
        event.read_pointer_writeback_address =
            primary_system_buffer + kReadPointerWritebackOffset;
        event.valid_fields |=
            rerevved::gpu::diagnostics::kTraceReadPointerWriteback;
    }

    auto* graphics_system = dynamic_cast<rex::graphics::GraphicsSystem*>(
        GetGraphicsSystem());
    if (graphics_system)
    {
        event.published_write_pointer =
            (*graphics_system->register_file())[kCpRbWptrRegister];
        event.valid_fields |=
            rerevved::gpu::diagnostics::kTracePublishedWrite;
    }
}

void RecordTracePoint(PassiveTracePoint point) noexcept
{
    if (!PassiveTraceEnabled())
    {
        return;
    }
    PassiveTraceEvent event{};
    event.point = point;
    (void)rerevved::gpu::diagnostics::GetPassiveTraceBuffer().Record(event);
}

enum class TracedCaller : uint8_t
{
    kNone,
    kOrdinary,
    kAlternate,
};

void RecordCallerTracePoint(PassiveTracePoint point) noexcept;

thread_local bool         traced_exact_resolve_active = false;
thread_local bool         traced_reservation_active   = false;
thread_local bool         traced_vdswap_owner_active  = false;
thread_local bool         traced_vdswap_active        = false;
thread_local TracedCaller traced_caller               = TracedCaller::kNone;

void RecordCallerTracePoint(PassiveTracePoint point) noexcept
{
    if (traced_caller != TracedCaller::kNone)
    {
        RecordTracePoint(point);
    }
}

} // namespace

void ReRevvedPublishGameplayState()
{
    rerevved::gameplay::PublishFrameSnapshot();
}

void ReRevvedApplyCombatPaceOverride()
{
    if (REXCVAR_GET(combat_speed) != "fast")
    {
        return;
    }

    constexpr uint32_t kCombatPaceDivisor = 0x82F79FBC;
    constexpr float    kNativeStandard    = 2.0f;
    constexpr float    kNativeAlternate   = 1.5f;
    constexpr float    kNativeFast        = 0.5f;
    if (!IsGuestReadableRange(kCombatPaceDivisor, sizeof(uint32_t)))
    {
        return;
    }

    const float selected =
        std::bit_cast<float>(ReadGuestU32(kCombatPaceDivisor));
    if (selected != kNativeStandard && selected != kNativeAlternate)
    {
        return;
    }

    WriteGuestU32Safely(kCombatPaceDivisor,
                        std::bit_cast<uint32_t>(kNativeFast));
}

void ReRevvedFixRushCostDisplay(PPCRegister& r27,
                                PPCRegister& r30,
                                PPCRegister& r31,
                                PPCRegister& r6,
                                PPCRegister& r7,
                                PPCRegister& r11)
{
    rush_cost_repair  = {};
    int32_t corrected = 0;
    if (r6.s32 > 1 &&
        rerevved::TryCalculateRushCost(
            r31.s32, r6.s32, r7.s32, corrected) &&
        corrected != r11.s32)
    {
        r11.s32          = corrected;
        rush_cost_repair = { true, r27.u32, r30.s32, r7.s32, corrected };
    }
}

void ReRevvedFixRushCostApply(PPCRegister& r25,
                              PPCRegister& r26,
                              PPCRegister& r28,
                              PPCRegister& r3,
                              PPCRegister& r6,
                              PPCRegister& r8)
{
    const rerevved::RushCostRepair repair = rush_cost_repair;
    rush_cost_repair                      = {};
    rerevved::TryCoordinateRushProduction(
        repair, r28.u32, r25.s32, r26.s32, r6.s32, r8.s32, r3.s32);
}

void ReRevvedCompatNullOptionalDispatch(PPCRegister& r0, PPCRegister& r3)
{
    if (r0.u32 == 0)
    {
        r3.u64 = 0;
    }
}

void ReRevvedCompatRingInitializeBegin(PPCRegister&, PPCRegister&)
{
    if (PassiveTraceEnabled())
    {
        traced_exact_resolve_active = false;
        traced_reservation_active   = false;
        traced_vdswap_owner_active  = false;
        traced_vdswap_active        = false;
        traced_caller               = TracedCaller::kNone;
        PassiveTraceEvent event{};
        event.point = PassiveTracePoint::kRingResetBegin;
        (void)rerevved::gpu::diagnostics::GetPassiveTraceBuffer()
            .BeginObservationEpoch(event);
    }
    auto* graphics_system = GetGraphicsSystem();
    resume_after_ring_initialize =
        graphics_system && graphics_system->PauseAndResetGpuWritePointer();
    if (FenceTraceEnabled())
    {
        rex::graphics::diagnostic::GetXenosFenceTrace()
            .ResetObservationEpoch();
    }
}

void ReRevvedCompatRingInitializeEnd()
{
    auto* graphics_system = GetGraphicsSystem();
    if (graphics_system && resume_after_ring_initialize)
    {
        graphics_system->ResumeGpu();
    }
    resume_after_ring_initialize = false;
    RecordTracePoint(PassiveTracePoint::kRingResetReturn);
}

void ReRevvedObserveNativeDevicePublication(PPCRegister& r11,
                                            PPCRegister& r31)
{
    if (REXCVAR_GET(renderer) != "native")
    {
        return;
    }

    const uint32_t cell_address   = r11.u32;
    const uint32_t device_address = r31.u32;
    if (!IsGuestReadableRange(cell_address, sizeof(uint32_t)) ||
        !IsGuestPointer(device_address))
    {
        REXLOG_ERROR(
            "Native guest device publication invalid: cell={:08X} device={:08X}",
            cell_address,
            device_address);
        return;
    }

    const uint32_t stored_address = ReadGuestU32(cell_address);
    if (stored_address != device_address)
    {
        REXLOG_ERROR(
            "Native guest device publication mismatch: cell={:08X} stored={:08X} device={:08X}",
            cell_address,
            stored_address,
            device_address);
        return;
    }

    if (rerevved::gpu::PublishGuestDevice(cell_address, device_address))
    {
        REXLOG_INFO(
            "Native guest device published: cell={:08X} device={:08X}",
            cell_address,
            device_address);
    }
}

void ReRevvedObserveNativeTexturePublication(PPCRegister& r22,
                                             PPCRegister& r3)
{
    if (REXCVAR_GET(renderer) != "native" || r3.u32 == 0)
    {
        return;
    }

    const uint32_t     texture_address = r22.u32;
    const uint32_t     backend_address = r3.u32;
    constexpr uint32_t width_offset    = 0x20;
    constexpr uint32_t height_offset   = 0x24;
    constexpr uint32_t backend_offset  = 0x28;
    if (!IsGuestPointer(texture_address) ||
        !IsGuestReadableRange(texture_address + width_offset,
                              sizeof(uint32_t) * 3) ||
        !IsGuestPointer(backend_address))
    {
        REXLOG_ERROR(
            "Native guest texture store observation rejected invalid pointers");
        return;
    }

    const uint32_t stored_backend =
        ReadGuestU32(texture_address + backend_offset);
    if (stored_backend != backend_address)
    {
        REXLOG_ERROR(
            "Native guest texture store observation rejected a store mismatch");
        return;
    }

    const uint32_t width    = ReadGuestU32(texture_address + width_offset);
    const uint32_t height   = ReadGuestU32(texture_address + height_offset);
    const uint64_t sequence = rerevved::gpu::ObserveGuestTexture(width, height);
    if (sequence == 1)
    {
        REXLOG_INFO(
            "Native guest texture store observed: size={}x{}",
            width,
            height);
    }
}

void ReRevvedObserveNativeResolveProviderIdentity(PPCRegister& r3)
{
    if (REXCVAR_GET(renderer) != "native")
    {
        return;
    }

    const uint32_t object = r3.u32;
    if (!IsGuestPointer(object))
    {
        if (!native_resolve_provider_error_log.test_and_set(
                std::memory_order_relaxed))
        {
            REXLOG_ERROR(
                "Native resolve-provider observation rejected invalid object: object={:08X}",
                object);
        }
        return;
    }

    uint32_t vptr = 0;
    if (!ReadGuestWordAt(object, 0x00, vptr))
    {
        if (!native_resolve_provider_error_log.test_and_set(
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
        if (ClaimBoundedLog(native_resolve_provider_mismatch_log_count, 8))
        {
            REXLOG_INFO(
                "Native resolve provider observed: object={:08X} vptr={:08X} explicit_match=false",
                object,
                vptr);
        }
        return;
    }

    uint32_t   slot0           = 0;
    uint32_t   slot1           = 0;
    uint32_t   active          = 0;
    const bool slot0_readable  = ReadGuestWordAt(object, 0x90, slot0);
    const bool slot1_readable  = ReadGuestWordAt(object, 0x94, slot1);
    const bool active_readable = ReadGuestWordAt(object, 0x98, active);
    if (!slot0_readable || !slot1_readable || !active_readable)
    {
        if (!native_resolve_provider_error_log.test_and_set(
                std::memory_order_relaxed))
        {
            REXLOG_ERROR(
                "Native explicit resolve-provider observation rejected unreadable fields: object={:08X} slot90_readable={} slot94_readable={} active_readable={}",
                object,
                slot0_readable,
                slot1_readable,
                active_readable);
        }
        return;
    }

    const bool     selected_index_valid = active < 2;
    const uint32_t selected             = active == 0 ? slot0 : slot1;
    const bool     selected_valid =
        selected_index_valid && selected != 0 &&
        IsGuestReadableRange(selected, sizeof(uint32_t));
    if (!ClaimBoundedLog(native_resolve_provider_match_log_count, 8))
    {
        return;
    }

    if (selected_valid)
    {
        REXLOG_INFO(
            "Native resolve provider observed: object={:08X} vptr={:08X} explicit_match=true slot90={:08X} slot94={:08X} active={:08X} selected_index={} selected={:08X} selected_valid={}",
            object,
            vptr,
            slot0,
            slot1,
            active,
            selected_index_valid ? active : 2,
            selected,
            selected_valid);
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
            selected_index_valid ? active : 2,
            selected_valid);
    }
}

void ReRevvedObserveNativeExplicitBufferFactoryStore(PPCRegister& r28,
                                                     PPCRegister& r29,
                                                     PPCRegister& r30,
                                                     PPCRegister& r3)
{
    if (REXCVAR_GET(renderer) != "native")
    {
        return;
    }

    const uint32_t container       = r28.u32;
    const uint32_t slot            = r29.u32;
    const uint32_t result          = r3.u32;
    uint32_t       vptr            = 0;
    uint32_t       active          = 0;
    const bool     container_valid = IsGuestPointer(container);
    const bool     vptr_readable =
        container_valid && ReadGuestWordAt(container, 0x00, vptr);
    constexpr uint32_t kExplicitBuffersVtable = 0x8204767C;
    const bool         vptr_matches           = vptr_readable && vptr == kExplicitBuffersVtable;
    const bool         active_readable =
        vptr_matches && ReadGuestWordAt(container, 0x98, active);
    const bool slot_readable =
        IsGuestReadableRange(slot, sizeof(uint32_t));
    const bool     store_matches    = slot_readable && ReadGuestU32(slot) == result;
    const bool     slot0            = container_valid && container <= UINT32_MAX - 0x90 &&
                                      slot == container + 0x90;
    const bool     slot1            = container_valid && container <= UINT32_MAX - 0x94 &&
                                      slot == container + 0x94;
    const bool     slot_index_valid = slot0 || slot1;
    const uint32_t slot_index       = slot1 ? 1 : 0;
    if (!container_valid || !vptr_matches || !active_readable ||
        !slot_readable || !store_matches || !slot_index_valid)
    {
        if (!native_explicit_factory_error_log.test_and_set(
                std::memory_order_relaxed))
        {
            REXLOG_ERROR(
                "Native explicit-buffer factory observation rejected: container={:08X} slot={:08X} result={:08X} remaining={} vptr={:08X} vptr_readable={} vptr_matches={} active_readable={} slot_readable={} store_matches={} slot_index_valid={}",
                container,
                slot,
                result,
                r30.u32,
                vptr,
                vptr_readable,
                vptr_matches,
                active_readable,
                slot_readable,
                store_matches,
                slot_index_valid);
        }
        return;
    }

    if (ClaimBoundedLog(native_explicit_factory_log_count, 2))
    {
        REXLOG_INFO(
            "Native explicit-buffer factory store observed: container={:08X} vptr={:08X} slot_index={} result={:08X} active={:08X} remaining={}",
            container,
            vptr,
            slot_index,
            result,
            active,
            r30.u32);
    }
}

void ReRevvedTraceReservationEnter(PPCRegister& r3, PPCRegister& r4)
{
    traced_reservation_active = false;
    if (!traced_vdswap_owner_active || r4.u32 != 64)
    {
        return;
    }

    auto lease = rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
    if (!lease)
    {
        return;
    }
    traced_reservation_active = true;
    PassiveTraceEvent event{};
    event.point            = PassiveTracePoint::kReservationEnter;
    event.requested_dwords = r4.u32;
    ReadRingObservation(r3.u32, event);
    (void)lease.Commit(event);
}

void ReRevvedTraceReservationReturn(PPCRegister& r3,
                                    PPCRegister& r29,
                                    PPCRegister& r31)
{
    if (!traced_reservation_active)
    {
        return;
    }
    traced_reservation_active = false;
    auto lease                = rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
    if (!lease)
    {
        return;
    }
    PassiveTraceEvent event{};
    event.point               = PassiveTracePoint::kReservationReturn;
    event.requested_dwords    = r29.u32;
    event.reservation_address = r3.u32;
    ReadRingObservation(r31.u32, event);
    (void)lease.Commit(event);
}

void ReRevvedTraceVdSwapOwnerEnter(PPCRegister& r3, PPCRegister& r4)
{
    traced_vdswap_owner_active = false;
    if (traced_caller == TracedCaller::kNone)
    {
        return;
    }
    auto lease = rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
    if (!lease)
    {
        return;
    }
    traced_vdswap_owner_active = true;
    PassiveTraceEvent event{};
    event.point                    = PassiveTracePoint::kVdSwapOwnerEnter;
    event.resolve_resource_address = r4.u32;
    ReadRingObservation(r3.u32, event);
    (void)lease.Commit(event);
}

void ReRevvedTraceVdSwapOwnerReturn(PPCRegister& r31)
{
    if (traced_vdswap_owner_active)
    {
        auto lease =
            rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
        if (lease)
        {
            PassiveTraceEvent event{};
            event.point = PassiveTracePoint::kVdSwapOwnerReturn;
            ReadRingObservation(r31.u32, event);
            (void)lease.Commit(event);
        }
    }
    traced_reservation_active  = false;
    traced_vdswap_owner_active = false;
    traced_vdswap_active       = false;
}

void ReRevvedObserveRendererResolve(PPCRegister& r4,
                                    PPCRegister& r6,
                                    PPCRegister& r8,
                                    PPCRegister& r9,
                                    uint64_t     lr)
{
    constexpr uint32_t kFetchDescriptorOffset = 0x1C;
    constexpr uint32_t kExactResolveCallsite  = 0x8250AFEC;
    const uint32_t     return_address         = static_cast<uint32_t>(lr);
    const uint32_t     call_address =
        return_address >= 4 ? return_address - 4 : 0;
    const bool descriptor_address_valid =
        r6.u32 <= UINT32_MAX - kFetchDescriptorOffset;
    const uint32_t descriptor_address =
        descriptor_address_valid ? r6.u32 + kFetchDescriptorOffset : 0;
    PassiveTraceRecordLease trace_lease{};
    if (call_address == kExactResolveCallsite)
    {
        trace_lease =
            rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
    }
    traced_exact_resolve_active = static_cast<bool>(trace_lease);
    rerevved::gpu::GuestFetchDescriptor descriptor{};
    const bool                          descriptor_valid =
        descriptor_address_valid &&
        ReadGuestFetchDescriptor(descriptor_address, descriptor);

    if (traced_exact_resolve_active)
    {
        PassiveTraceEvent event{};
        event.point                    = PassiveTracePoint::kResolveEnter;
        event.resolve_resource_address = r6.u32;
        event.descriptor_address       = descriptor_address;
        event.resolve_call_address     = call_address;
        event.resolve_flags            = r4.u32;
        event.resolve_mip_level        = r8.u32;
        event.resolve_slice            = r9.u32;
        if (descriptor_valid)
        {
            event.descriptor = descriptor;
            event.valid_fields |=
                rerevved::gpu::diagnostics::kTraceDescriptor;
        }
        (void)trace_lease.Commit(event);
    }

    if (!descriptor_valid)
    {
        return;
    }

    const uint64_t sequence = rerevved::gpu::ObserveGuestResolve(
        descriptor, call_address, r4.u32, r8.u32, r9.u32);
    if (sequence == 1)
    {
        REXLOG_INFO(
            "Renderer resolve observed: call={:08X} flags={:08X} mip={} slice={} destination_base={:08X}",
            call_address,
            r4.u32,
            r8.u32,
            r9.u32,
            descriptor[1] & 0xFFFFF000u);
    }
}

void ReRevvedTraceResolveReturn()
{
    if (!traced_exact_resolve_active)
    {
        return;
    }
    RecordTracePoint(PassiveTracePoint::kResolveReturn);
    traced_exact_resolve_active = false;
}

void ReRevvedObserveRendererSwapSource(PPCRegister& r3,
                                       PPCRegister& r4,
                                       PPCRegister& r30,
                                       PPCRegister& r31)
{
    PassiveTraceRecordLease trace_lease{};
    if (traced_vdswap_owner_active)
    {
        trace_lease =
            rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
    }
    rerevved::gpu::GuestFetchDescriptor descriptor{};
    const bool                          descriptor_valid = ReadGuestFetchDescriptor(r4.u32, descriptor);
    traced_vdswap_active                                 = static_cast<bool>(trace_lease);
    if (traced_vdswap_active)
    {
        PassiveTraceEvent event{};
        event.point               = PassiveTracePoint::kVdSwapCall;
        event.requested_dwords    = 64;
        event.reservation_address = r30.u32;
        event.vdswap_argument     = r3.u32;
        event.descriptor_address  = r4.u32;
        ReadRingObservation(r31.u32, event);
        if (descriptor_valid)
        {
            event.descriptor = descriptor;
            event.valid_fields |=
                rerevved::gpu::diagnostics::kTraceDescriptor;
        }
        (void)trace_lease.Commit(event);
    }

    if (!descriptor_valid)
    {
        return;
    }

    const rerevved::gpu::GuestSwapCorrelation correlation =
        rerevved::gpu::ObserveGuestSwap(descriptor);
    if (correlation.match == rerevved::gpu::GuestFetchMatch::kExact &&
        correlation.matched_count == 1)
    {
        REXLOG_INFO(
            "Renderer swap source exactly matched resolve: resolve_call={:08X} resolve_flags={:08X} mip={} slice={} resolve_sequence={} swap_sequence={} source_base={:08X}",
            correlation.resolve_call_address,
            correlation.resolve_flags,
            correlation.resolve_mip_level,
            correlation.resolve_slice,
            correlation.resolve_sequence,
            correlation.swap_sequence,
            descriptor[1] & 0xFFFFF000u);
    }
    else if (correlation.match ==
                 rerevved::gpu::GuestFetchMatch::kBaseAddressCandidate &&
             correlation.candidate_count == 1)
    {
        REXLOG_INFO(
            "Renderer swap source has base-address resolve candidate: resolve_call={:08X} resolve_flags={:08X} mip={} slice={} resolve_sequence={} swap_sequence={} source_base={:08X}",
            correlation.resolve_call_address,
            correlation.resolve_flags,
            correlation.resolve_mip_level,
            correlation.resolve_slice,
            correlation.resolve_sequence,
            correlation.swap_sequence,
            descriptor[1] & 0xFFFFF000u);
    }
    else if (correlation.match == rerevved::gpu::GuestFetchMatch::kNone &&
             correlation.unmatched_count == 1)
    {
        REXLOG_INFO(
            "Renderer swap source has no retained resolve match: swap_sequence={} source_base={:08X}",
            correlation.swap_sequence,
            descriptor[1] & 0xFFFFF000u);
    }
}

void ReRevvedTraceVdSwapReturn(PPCRegister& r3,
                               PPCRegister& r30,
                               PPCRegister& r31)
{
    if (FenceTraceEnabled() &&
        (r30.u32 & (alignof(uint32_t) - 1)) == 0 &&
        IsGuestReadableRange(r30.u32, sizeof(uint32_t)))
    {
        const uint32_t physical_address =
            REX_KERNEL_MEMORY()->GetPhysicalAddress(r30.u32);
        if (physical_address != UINT32_MAX)
        {
            (void)rex::graphics::diagnostic::GetXenosFenceTrace()
                .WatchSwapReservation(r30.u32, physical_address);
        }
    }
    if (!traced_vdswap_active)
    {
        return;
    }
    auto lease = rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
    if (!lease)
    {
        return;
    }
    PassiveTraceEvent event{};
    event.point               = PassiveTracePoint::kVdSwapReturn;
    event.requested_dwords    = 64;
    event.reservation_address = r30.u32;
    event.return_value        = r3.u32;
    ReadRingObservation(r31.u32, event);
    (void)lease.Commit(event);
}

void ReRevvedTraceVdSwapPublished(PPCRegister& r30, PPCRegister& r31)
{
    if (!traced_vdswap_active)
    {
        traced_vdswap_active = false;
        return;
    }
    auto lease = rerevved::gpu::diagnostics::GetPassiveTraceBuffer().BeginRecord();
    if (!lease)
    {
        traced_vdswap_active = false;
        return;
    }
    PassiveTraceEvent event{};
    event.point               = PassiveTracePoint::kVdSwapPublished;
    event.requested_dwords    = 64;
    event.reservation_address = r30.u32;
    ReadRingObservation(r31.u32, event);
    constexpr uint32_t kReservationBytes =
        sizeof(uint32_t) *
        rerevved::gpu::diagnostics::kPassiveTraceReservationDwords;
    // RVA-F-0128 proves r30 is the 64-dword reservation returned by the helper.
    // VdSwap receives r30 + 4, but this capture intentionally covers the full
    // reservation from r30 through the published r30 + 256 boundary.
    if ((r30.u32 & (alignof(uint32_t) - 1)) == 0 &&
        IsGuestReadableRange(r30.u32, kReservationBytes))
    {
        for (std::size_t index = 0;
             index < event.reservation_words.size();
             ++index)
        {
            event.reservation_words[index] = ReadGuestU32(
                r30.u32 + static_cast<uint32_t>(index * sizeof(uint32_t)));
        }
        event.valid_fields |=
            rerevved::gpu::diagnostics::kTraceReservationWords;
    }
    (void)lease.Commit(event);
    traced_vdswap_active = false;
}

void ReRevvedTracePreSwapEnter()
{
    RecordCallerTracePoint(PassiveTracePoint::kPreSwapEnter);
}

void ReRevvedTracePreSwapReturn()
{
    RecordCallerTracePoint(PassiveTracePoint::kPreSwapReturn);
}

void ReRevvedTraceEmitterCd20Enter()
{
    RecordCallerTracePoint(PassiveTracePoint::kEmitterCd20Enter);
}

void ReRevvedTraceEmitterCd20Return()
{
    RecordCallerTracePoint(PassiveTracePoint::kEmitterCd20Return);
}

void ReRevvedTraceEmitterBf40Enter()
{
    RecordCallerTracePoint(PassiveTracePoint::kEmitterBf40Enter);
}

void ReRevvedTraceEmitterBf40Return()
{
    RecordCallerTracePoint(PassiveTracePoint::kEmitterBf40Return);
}

void ReRevvedTraceCallbackEnter()
{
    RecordCallerTracePoint(PassiveTracePoint::kCallbackEnter);
}

void ReRevvedTraceCallbackReturn()
{
    RecordCallerTracePoint(PassiveTracePoint::kCallbackReturn);
}

void ReRevvedTraceOrdinaryCallerEnter()
{
    if (!PassiveTraceEnabled())
    {
        return;
    }
    traced_caller = TracedCaller::kOrdinary;
    RecordTracePoint(PassiveTracePoint::kOrdinaryCallerEnter);
}

void ReRevvedTraceOrdinaryCallerReturn()
{
    if (traced_caller == TracedCaller::kOrdinary)
    {
        RecordTracePoint(PassiveTracePoint::kOrdinaryCallerReturn);
        traced_reservation_active  = false;
        traced_vdswap_owner_active = false;
        traced_vdswap_active       = false;
        traced_caller              = TracedCaller::kNone;
    }
}

void ReRevvedTraceAlternateCallerEnter()
{
    if (!PassiveTraceEnabled())
    {
        return;
    }
    traced_caller = TracedCaller::kAlternate;
    RecordTracePoint(PassiveTracePoint::kAlternateCallerEnter);
}

void ReRevvedTraceAlternateCallerReturn()
{
    if (traced_caller == TracedCaller::kAlternate)
    {
        RecordTracePoint(PassiveTracePoint::kAlternateCallerReturn);
        traced_reservation_active  = false;
        traced_vdswap_owner_active = false;
        traced_vdswap_active       = false;
        traced_caller              = TracedCaller::kNone;
    }
}

void ReRevvedRememberGfxRenderConfig(PPCRegister& r3, PPCRegister& r4)
{
    gfx_render_config_candidate = r3.u32;
    gfx_render_config_renderer  = r4.u32;
}

void ReRevvedHandleGfxRenderCapsBegin(PPCRegister& r3,
                                      PPCRegister& r4,
                                      uint64_t     lr)
{
    if (gfx_render_caps.observing)
    {
        return;
    }

    gfx_render_caps = {
        r3.u32,
        r4.u32,
        static_cast<uint32_t>(lr),
        true,
    };
}

void ReRevvedHandleGfxRenderCapsEnd(PPCRegister& r3, PPCRegister& r31)
{
    if (!gfx_render_caps.observing)
    {
        return;
    }

    const auto state          = gfx_render_caps;
    gfx_render_caps           = {};
    const bool output_matches = r31.u32 == state.output;
    const bool output_readable =
        output_matches && IsGuestReadableRange(r31.u32, 16);
    // GFx copies the failed query output, so retain that config and refresh it
    // only from a valid result produced by the same renderer.
    if (state.caller == 0x82245130 && r3.u32 == 0 &&
        gfx_render_config_renderer == state.renderer &&
        IsGuestReadableRange(gfx_render_config_candidate + 0x14, 0x18))
    {
        gfx_stale_render_config.store(gfx_render_config_candidate,
                                      std::memory_order_release);
        gfx_stale_render_config_renderer.store(state.renderer,
                                               std::memory_order_release);
    }
    else if (r3.u32 != 0 && output_readable &&
             gfx_stale_render_config_renderer.load(std::memory_order_acquire) ==
                 state.renderer)
    {
        const uint32_t config =
            gfx_stale_render_config.load(std::memory_order_acquire);
        if (config != 0 && IsGuestReadableRange(config + 0x14, 0x18) &&
            ReadGuestU32(config + 0x14) == state.renderer &&
            WriteGuestU32Safely(config + 0x24, ReadGuestU32(r31.u32)) &&
            WriteGuestU32Safely(config + 0x28, ReadGuestU32(r31.u32 + 4)))
        {
            gfx_stale_render_config.store(0, std::memory_order_release);
            gfx_stale_render_config_renderer.store(0, std::memory_order_release);
        }
    }
}
