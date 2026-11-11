// 4:3 preset, see display_aspect.h: display setup 0x82E2A4B8 (r4=&width, r5=&height, r6=&widescreen byte).

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "display_aspect.h"

REX_EXTERN(__imp__sub_82E2A4B8);

namespace
{

namespace da = rerevved::display_aspect;

uint32_t loadU32(const uint8_t* base, uint32_t address)
{
    const uint8_t* source = base + address;
    return uint32_t{ source[0] } << 24 | uint32_t{ source[1] } << 16 | uint32_t{ source[2] } << 8 |
           uint32_t{ source[3] };
}

void storeU32(uint8_t* base, uint32_t address, uint32_t value)
{
    uint8_t* target = base + address;
    target[0]       = static_cast<uint8_t>(value >> 24);
    target[1]       = static_cast<uint8_t>(value >> 16);
    target[2]       = static_cast<uint8_t>(value >> 8);
    target[3]       = static_cast<uint8_t>(value);
}

} // namespace

// Run Original first; it derives the three outputs from the reported video mode.
REX_HOOK_RAW(sub_82E2A4B8)
{
    const uint32_t width      = ctx.r4.u32;
    const uint32_t height     = ctx.r5.u32;
    const uint32_t widescreen = ctx.r6.u32;
    __imp__sub_82E2A4B8(ctx, base);
    if (!da::NarrowLatched() || width == 0 || height == 0 || widescreen == 0)
        return;
    const da::GuestMode retail{ loadU32(base, width), loadU32(base, height), base[widescreen] != 0 };
    const da::GuestMode mode = da::CorrectNarrowSetup(retail);
    storeU32(base, width, mode.width);
    base[widescreen] = mode.widescreen ? 1 : 0;
    REXLOG_INFO("Display aspect: display setup {}x{} widescreen {} kept as {}x{} widescreen {}",
                retail.width,
                retail.height,
                retail.widescreen ? 1 : 0,
                mode.width,
                mode.height,
                mode.widescreen ? 1 : 0);
}
