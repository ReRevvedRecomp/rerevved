// Hooks preserve title behavior unless documented as a compatibility repair.

#include <atomic>
#include <cstdint>

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

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

thread_local GfxRenderCapsState gfx_render_caps{};
thread_local uint32_t           gfx_render_config_candidate      = 0;
thread_local uint32_t           gfx_render_config_renderer       = 0;
std::atomic_uint32_t            gfx_stale_render_config          = 0;
std::atomic_uint32_t            gfx_stale_render_config_renderer = 0;

} // namespace

void ReRevvedCompatNullOptionalDispatch(PPCRegister& r0, PPCRegister& r3)
{
    if (r0.u32 == 0)
    {
        r3.u64 = 0;
    }
}

void ReRevvedCompatRingInitializeBegin(PPCRegister&, PPCRegister&)
{
    auto* graphics_system = GetGraphicsSystem();
    resume_after_ring_initialize =
        graphics_system && graphics_system->PauseAndResetGpuWritePointer();
}

void ReRevvedCompatRingInitializeEnd()
{
    auto* graphics_system = GetGraphicsSystem();
    if (graphics_system && resume_after_ring_initialize)
    {
        graphics_system->ResumeGpu();
    }
    resume_after_ring_initialize = false;
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

void ReRevvedCompatExpandGfxVectorGlyphCache(PPCRegister& r31)
{
    constexpr uint32_t kManagerField          = 72;
    constexpr uint32_t kVectorCacheLimitField = 2512;
    constexpr uint32_t kDefaultLimit          = 512;
    constexpr uint32_t kCompatLimit           = 1024;

    if (!IsGuestReadableRange(r31.u32, kManagerField + sizeof(uint32_t)))
    {
        return;
    }
    const uint32_t manager = ReadGuestU32(r31.u32 + kManagerField);
    if (manager > UINT32_MAX - kVectorCacheLimitField)
    {
        return;
    }
    const uint32_t limit = manager + kVectorCacheLimitField;
    if (IsGuestReadableRange(limit, sizeof(uint32_t)) &&
        ReadGuestU32(limit) == kDefaultLimit)
    {
        WriteGuestU32Safely(limit, kCompatLimit);
    }
}
