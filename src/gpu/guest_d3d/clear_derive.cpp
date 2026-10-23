#include "clear_derive.h"

#include "resolve_derive.h"
#include "ring_words.h"

#include <rex/cvar.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <utility>

#if REREVVED_DEVELOPER
REXCVAR_DEFINE_BOOL(native_clear_derive_check,
                    false,
                    "ReRevved",
                    "Bring-up only for renderer=native: for each Clear call (0x826A2970), derive on the host the ordered list of per-target clears "
                    "the body makes to 0x826A1D90 (flags, color slot, clipped rectangle, color pointer, depth and stencil) from the entry registers "
                    "and the device, and compare it with the 0x826A1D90 calls the hooks record. Observes only; clear_derive_ok, "
                    "clear_derive_mismatch_*, clear_derive_unsupported and clear_derive_unpaired fields on the native_frame_timing line")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_clear_effect_check,
                    false,
                    "ReRevved",
                    "Bring-up only for renderer=native: for each Clear call (0x826A2970), predict on the host the exit state of the dirty words "
                    "G+0x10, G+0x18 and G+0x20 and of the byte G+0x2ABD from their entry values, the entry registers and the device, read them at "
                    "the entry and again at the exit of the body 0x826A2960, and compare exactly. Observes only; clear_effect_* fields on the "
                    "native_frame_timing line")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#endif
namespace rerevved::gpu::guest_d3d
{

namespace
{

#if REREVVED_DEVELOPER

struct DeriveCounters
{
    std::atomic<std::uint64_t>                               ok{ 0 }, unsupported{ 0 }, unpaired{ 0 };
    std::array<std::atomic<std::uint64_t>, kClearFieldCount> mismatch{};
};

DeriveCounters counters;

struct EffectCounters
{
    std::atomic<std::uint64_t>                                     exactOk{ 0 }, earlyOk{ 0 }, earlyChanged{ 0 }, allocFailed{ 0 }, unsupported{ 0 }, unpaired{ 0 };
    std::array<std::atomic<std::uint64_t>, kClearEffectFieldCount> mismatch{};
    std::atomic<std::uint64_t>                                     pathUntiled{ 0 }, pathTiledBlock{ 0 }, pathTiledLoop{ 0 }, pathTiledNone{ 0 };
    std::atomic<std::uint64_t>                                     color{ 0 }, depthOnly{ 0 }, stencil{ 0 }, fast{ 0 }, flushNoCalls{ 0 };
    std::atomic<std::uint64_t>                                     stateChanged{ 0 }, markerAtEntry{ 0 }, switchBit{ 0 };
    std::atomic<std::int64_t>                                      allocBytesSum{ 0 }, ringDeltaSum{ 0 }, cursorDeltaSum{ 0 };
};

EffectCounters effectCounters;

struct ExitCounters
{
    std::atomic<std::uint64_t>                                     ok{ 0 }, tiled{ 0 }, tiledOk{ 0 };
    std::array<std::atomic<std::uint64_t>, kClearShapeCount>       shapes{};
    std::array<std::atomic<std::uint64_t>, kClearExitWordCount>    mismatch{};
    std::array<std::atomic<std::uint64_t>, kClearExitRefusalCount> unmodeled{};
};

ExitCounters exitCounters;
#endif

// A 32-bit add that wraps, as the low word of the body's 64-bit add that the
// following cmpw reads (g158:13090, 13103).
std::int32_t add32(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

// One tile the loop of 0x826A1D90 selects for a call: its index and the call
// rectangle clipped to the tile rectangle.
struct SelectedTile
{
    std::uint32_t               index = 0;
    std::array<std::int32_t, 4> rect{};
};

struct SelectedTiles
{
    std::array<SelectedTile, kClearMaxLoopTiles> tile{};
    std::size_t                                  count = 0;
};

// g88:13631-13789 clips signed tile rects G+0x31D0+16i (g88:13653-13701) and
// tests (3 << (2*i)) & G+0x31A4 (g88:13702-13710). slw uses six shift bits, so
// tiles>=16 never select.
bool SelectClearTiles(const GuestWordReader& read, std::uint32_t g, const std::array<std::int32_t, 4>& callRect, std::uint32_t mask, std::uint32_t count, SelectedTiles& out, std::uint32_t& failed)
{
    out.count = 0;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        std::array<std::int32_t, 4> rect{};
        for (std::uint32_t j = 0; j < 4; ++j)
        {
            std::uint32_t value = 0;
            if (!read(g + 0x31D0 + 16 * i + 4 * j, value))
            {
                failed = g + 0x31D0 + 16 * i + 4 * j;
                return false;
            }
            rect[j] = static_cast<std::int32_t>(value);
        }
        const std::int32_t  left = std::max(callRect[0], rect[0]), top = std::max(callRect[1], rect[1]);
        const std::int32_t  right = std::min(callRect[2], rect[2]), bottom = std::min(callRect[3], rect[3]);
        const std::uint32_t shift = (2 * i) & 63;
        const std::uint32_t bits  = shift >= 32 ? 0U : (3U << shift);
        if (left < right && top < bottom && (bits & mask) != 0 && out.count < out.tile.size())
            out.tile[out.count++] = { i, { left, top, right, bottom } };
    }
    return true;
}

} // namespace

std::int32_t ClearFloatToInt32(float value) noexcept
{
    const double wide = value;
    if (std::isnan(wide))
        return std::numeric_limits<std::int32_t>::min();
    if (wide >= 2147483647.0)
        return std::numeric_limits<std::int32_t>::max();
    if (wide <= -2147483648.0)
        return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(wide);
}

std::optional<ClearCalls> DeriveClearCalls(const GuestWordReader& read, const ClearEntry& entry, std::string* why)
{
    const auto refuse = [&](std::string text) -> std::optional<ClearCalls>
    {
        if (why)
            *why = std::move(text);
        return std::nullopt;
    };
    const std::uint32_t g = entry.g;
    if (g > UINT32_MAX - 0x3384 || entry.rect > UINT32_MAX - 16 || entry.sp < kClearFrameSize)
        return refuse("address_overflow");
    std::uint32_t failed = 0;
    const auto    fetch  = [&](std::uint32_t address, std::uint32_t& value)
    {
        if (read(address, value))
            return true;
        failed = address;
        return false;
    };
    const auto unreadable = [&]()
    {
        return refuse(std::format("unreadable {:08X}", failed));
    };

    // The wrapper 0x826A2970 (g216:12892-13061). A rectangle from the caller
    // goes to the body as it is (bne at g216:12905-12908); a null one is built
    // in the wrapper's frame as {0, 0, width, height}.
    std::array<std::int32_t, 4> rect{};
    if (entry.rect)
    {
        for (std::uint32_t i = 0; i < 4; ++i)
        {
            std::uint32_t value = 0;
            if (!fetch(entry.rect + 4 * i, value))
                return unreadable();
            rect[i] = static_cast<std::int32_t>(value);
        }
    }
    else
    {
        // The extent word at +0x24 of the first bound surface: RT0 at G+0x3098,
        // or when that is null the depth surface at G+0x30A8; no other color
        // slot is consulted (g216:12909-12921).
        std::uint32_t rt0 = 0, first = 0, extent = 0, tileWord = 0;
        if (!fetch(g + 0x3098, rt0))
            return unreadable();
        first = rt0;
        if (!first && !fetch(g + 0x30A8, first))
            return unreadable();
        if (!first)
            return refuse("unbound_target_for_null_rect");
        if (first > UINT32_MAX - 0x28)
            return refuse("address_overflow");
        if (!fetch(first + 0x24, extent))
            return unreadable();
        // Bits 18..31 plus 1 are the width, stored as the right edge at
        // 88(r1), and bits 3..17 plus 1 the height, the bottom at 92(r1)
        // (g216:12926-12944).
        rect = { 0, 0, static_cast<std::int32_t>(((extent >> 18) & 0x3FFF) + 1), static_cast<std::int32_t>(((extent >> 3) & 0x7FFF) + 1) };
        // G+0x2ABC:0x10 always takes screen; 0x20 requires bound slots equal
        // BeginTiling G+0x31B8+4i, unbound slots/RT0 match anything(g216:12924,
        // 12938-13035; g216:12951-13018).
        if (!fetch((g + 0x2ABC) & ~3U, tileWord))
            return unreadable();
        const std::uint32_t tile  = (tileWord >> (24 - 8 * ((g + 0x2ABC) & 3))) & 0xFF;
        bool                tiled = (tile & 0x10) != 0;
        if (!tiled && (tile & 0x20))
        {
            tiled = true;
            for (std::uint32_t i = 0; i < 5 && tiled; ++i)
            {
                std::uint32_t saved = 0, current = rt0;
                if (!fetch(g + 0x31B8 + 4 * i, saved) || (i && !fetch(g + 0x3098 + 4 * i, current)))
                    return unreadable();
                tiled = saved == current || current == 0;
            }
        }
        if (tiled)
        {
            // The screen size, G+0x337C and G+0x3380 (g216:13036-13043).
            std::uint32_t width = 0, height = 0;
            if (!fetch(g + 0x337C, width) || !fetch(g + 0x3380, height))
                return unreadable();
            rect[2] = static_cast<std::int32_t>(width);
            rect[3] = static_cast<std::int32_t>(height);
        }
    }

    // The body 0x826A22F8 (g158:12972-13932). Without a bound depth surface at
    // G+0x30A8 the depth and stencil flags (0xF0) are dropped, and a flags
    // word left empty by that ends the body (g158:13017-13027).
    ClearCalls    out;
    std::uint32_t flags = entry.flags, depthSurface = 0;
    if (!fetch(g + 0x30A8, depthSurface))
        return unreadable();
    if (!depthSurface)
    {
        flags &= ~0xF0U;
        if (!flags)
            return out;
    }

    // The viewport floats G+0x3168, 0x316C, 0x3170, 0x3174 are x, y, width and
    // height after fctiwz (g158:13029-13067); the rectangle is clipped to
    // [x, x + width) and [y, y + height) as signed words (g158:13068-13112).
    std::array<std::int32_t, 4> viewport{};
    for (std::uint32_t i = 0; i < 4; ++i)
    {
        std::uint32_t bits = 0;
        if (!fetch(g + 0x3168 + 4 * i, bits))
            return unreadable();
        viewport[i] = ClearFloatToInt32(std::bit_cast<float>(bits));
    }
    std::int32_t left = std::max(rect[0], viewport[0]), top = std::max(rect[1], viewport[1]);
    std::int32_t right = std::min(rect[2], add32(viewport[2], viewport[0])), bottom = std::min(rect[3], add32(viewport[3], viewport[1]));
    // A nonzero G+0x2E50 enables the scissor words G+0x3184..0x3190, left, top,
    // right and bottom (g158:13114-13154).
    std::uint32_t scissorEnable = 0;
    if (!fetch(g + 0x2E50, scissorEnable))
        return unreadable();
    if (scissorEnable)
    {
        std::array<std::int32_t, 4> scissor{};
        for (std::uint32_t i = 0; i < 4; ++i)
        {
            std::uint32_t value = 0;
            if (!fetch(g + 0x3184 + 4 * i, value))
                return unreadable();
            scissor[i] = static_cast<std::int32_t>(value);
        }
        left   = std::max(left, scissor[0]);
        top    = std::max(top, scissor[1]);
        right  = std::min(right, scissor[2]);
        bottom = std::min(bottom, scissor[3]);
    }
    // An empty rectangle ends the body (g158:13156-13163).
    if (right <= left || bottom <= top)
        return out;
    out.reachedFlush = true;

    // A null color pointer becomes the body frame's r1+112, which holds a copy
    // of the four default words (g158:12998-13015, an aligned 16-byte store).
    const std::uint32_t colorPointer = entry.color ? entry.color : entry.sp - kClearFrameSize + kClearBodyColorOffset;
    const std::uint32_t colorSource  = entry.color ? entry.color : kClearDefaultColor;
    std::string         failure;
    const auto          emit = [&](std::uint32_t index)
    {
        ClearCall call;
        call.flags        = flags;
        call.index        = index;
        call.rect         = { left, top, right, bottom };
        call.colorPointer = colorPointer;
        call.depthBits    = std::bit_cast<std::uint64_t>(entry.depth);
        call.stencil      = entry.stencil;
        if (call.ColorUsed())
        {
            if (!entry.color && (colorPointer & 0xF))
            {
                failure = "misaligned_default_color";
                return false;
            }
            if (colorSource > UINT32_MAX - 16)
            {
                failure = "address_overflow";
                return false;
            }
            for (std::uint32_t i = 0; i < 4; ++i)
                if (!fetch(colorSource + 4 * i, call.color[i]))
                {
                    failure = std::format("unreadable {:08X}", failed);
                    return false;
                }
        }
        out.Add(call);
        return true;
    };
    if (!(flags & 0xF))
    {
        // No color bit: one call for the depth surface (g158:13610-13611,
        // 13727-13752).
        if (!emit(kGuestDepthOnlyClearIndex))
            return refuse(std::move(failure));
        return out;
    }
    // One call per set color bit k whose slot G+0x3098+4k is bound, in slot
    // order; the depth and stencil bits (0xF0) go with the first call only
    // (g158:13794-13880).
    for (std::uint32_t k = 0; k < 4; ++k)
    {
        std::uint32_t slot = 0;
        if (!((flags >> k) & 1))
            continue;
        if (!fetch(g + 0x3098 + 4 * k, slot))
            return unreadable();
        if (!slot)
            continue;
        if (!emit(k))
            return refuse(std::move(failure));
        flags &= ~0xF0U;
    }
    return out;
}

bool ClearDeviceTiled(const GuestWordReader& read, std::uint32_t g, bool& tiled, std::uint32_t* tileByte, std::uint32_t* failed)
{
    const auto fail = [&](std::uint32_t address)
    {
        if (failed)
            *failed = address;
        return false;
    };
    if (g > UINT32_MAX - 0x3400)
        return fail(g);
    std::uint32_t word = 0;
    if (!read((g + 0x2ABC) & ~3U, word))
        return fail((g + 0x2ABC) & ~3U);
    const std::uint32_t tile  = (word >> (24 - 8 * ((g + 0x2ABC) & 3))) & 0xFF;
    bool                value = (tile & 0x10) != 0;
    if (!value && (tile & 0x20))
    {
        value = true;
        for (std::uint32_t i = 0; i < 5 && value; ++i)
        {
            std::uint32_t saved = 0, current = 0;
            if (!read(g + 0x31B8 + 4 * i, saved))
                return fail(g + 0x31B8 + 4 * i);
            if (!read(g + 0x3098 + 4 * i, current))
                return fail(g + 0x3098 + 4 * i);
            value = saved == current || current == 0;
        }
    }
    tiled = value;
    if (tileByte)
        *tileByte = tile;
    return true;
}

std::optional<ClearEffects> DeriveClearEffects(const GuestWordReader& read, const ClearEntry& entry, const ClearCalls& calls, std::string* why)
{
    const auto refuse = [&](std::string text) -> std::optional<ClearEffects>
    {
        if (why)
            *why = std::move(text);
        return std::nullopt;
    };
    ClearEffects out;
    out.reachedFlush = calls.reachedFlush;
    out.keep         = { UINT64_MAX, UINT64_MAX, UINT64_MAX };
    if (!calls.reachedFlush)
        return out;
    const std::uint32_t g = entry.g;
    if (calls.overflow)
        return refuse("call_overflow");
    if (g > UINT32_MAX - 0x3400)
        return refuse("address_overflow");
    std::uint32_t failed = 0;
    const auto    fetch  = [&](std::uint32_t address, std::uint32_t& value)
    {
        if (read(address, value))
            return true;
        failed = address;
        return false;
    };
    const auto unreadable = [&]()
    {
        return refuse(std::format("unreadable {:08X}", failed));
    };
    const auto byteAt = [&](std::uint32_t offset, std::uint32_t& value)
    {
        std::uint32_t word = 0;
        if (!fetch((g + offset) & ~3U, word))
            return false;
        value = (word >> (24 - 8 * ((g + offset) & 3))) & 0xFF;
        return true;
    };

    // Flush keeps G+0x10 bits 17..20/58..63(g158:13190-13317), G+0x18 outside
    // 34..54(g158:13320-13359), G+0x20 bits 38..63(g158:13362-13389). Prelude
    // ORs follow(g158:13657-13725), then exit ORs(g158:13913-13924).
    out.keep           = { 0xFC000000001E0000ULL, 0xFF800003FFFFFFFFULL, 0xFFFFFFC000000000ULL };
    out.set            = { 0x00001380001A00E0ULL, 0x0000000800000000ULL, 0x0000000000080000ULL };
    out.setIfAllocated = { 0x0300000000000808ULL, 0x0000000080000000ULL, 0x0000001000000000ULL };
    out.byteSet        = kClearExitBit;
    // The color branch (g158:13794-13830) is taken for any of the flags 0xF, the
    // body having dropped only 0xF0 for want of a depth surface.
    out.color = (entry.flags & 0xF) != 0;
    if (out.color)
        out.set[0] |= 0x0000002000000600ULL;
    if (!calls.count)
        return out;

    // Whether the device is tiled (g88:13390-13487), which every call of the
    // body sees the same.
    std::uint32_t tile  = 0;
    bool          tiled = false;
    if (!ClearDeviceTiled(read, g, tiled, &tile, &failed))
        return unreadable();
    out.tiled               = tiled;
    std::uint32_t tileFlags = 0, tileMask = 0, tileCount = 0;
    if (tiled)
    {
        if (!byteAt(0x2ABF, tileFlags) || !fetch(g + 0x31A4, tileMask) || !fetch(g + 0x31CC, tileCount))
            return unreadable();
        if (tileCount > kClearMaxTiles)
            return refuse(std::format("tile_count {}", tileCount));
    }

    for (std::size_t n = 0; n < calls.count; ++n)
    {
        const ClearCall& call = calls.calls[n];
        ++out.calls;

        // The flag 0x20 ORs G+0x10 (g88:13109-13176).
        if (call.flags & 0x20)
        {
            ++out.stencilCalls;
            out.set[0] |= 0x0000000010000000ULL;
        }

        // Fast g88:13216-13389: no flags 0x30, format not 5/7/15,
        // size>=80x16/80x8/40x8 by(G+0x2880>>16)&3. Format=surface+0x1C, or0
        // depth-only; 0xA/0xC remaps(g88:13069-13090) hit none.
        if ((call.flags & 0x30) == 0)
        {
            std::uint32_t header = 0;
            if (call.index < 4)
            {
                std::uint32_t surface = 0;
                if (!fetch(g + 0x3098 + 4 * call.index, surface))
                    return unreadable();
                if (!surface)
                    return refuse("unbound_target");
                if (surface > UINT32_MAX - 0x20)
                    return refuse("address_overflow");
                if (!fetch(surface + 0x1C, header))
                    return unreadable();
            }
            const std::uint32_t format = (header >> 16) & 0xF;
            if (format != 5 && format != 7 && format != 15)
            {
                std::uint32_t control = 0;
                if (!fetch(g + 0x2880, control))
                    return unreadable();
                const std::uint32_t mode      = (control >> 16) & 3;
                const std::int32_t  minWidth  = mode <= 1 ? 80 : 40;
                const std::int32_t  minHeight = mode == 0 ? 16 : 8;
                const std::int32_t  width     = static_cast<std::int32_t>(static_cast<std::uint32_t>(call.rect[2]) - static_cast<std::uint32_t>(call.rect[0]));
                const std::int32_t  height    = static_cast<std::int32_t>(static_cast<std::uint32_t>(call.rect[3]) - static_cast<std::uint32_t>(call.rect[1]));
                if (width >= minWidth && height >= minHeight)
                {
                    ++out.fastCalls;
                    out.set[0] |= 0x0080000010000100ULL;
                }
            }
        }

        // The calls to 0x826A1AF0, each of which makes at least one call to
        // 0x826A1800 (g22:13105-13237). Untiled: one (g88:13488-13520). Tiled:
        // block A (g88:13522-13630) and the tile loop (g88:13631-13789).
        if (!tiled)
        {
            ++out.untiledCalls;
            out.allocates = true;
            continue;
        }
        // g88 reloads G+0x31A4 on every call (g88:13526), so block A clears
        // its bits only for the loop of the same call.
        std::uint32_t callMask = tileMask;
        bool          reaches  = false;
        if ((tileFlags & 0x20) && (tile & 0x40) && (callMask & 0x15555555U) != 0)
        {
            ++out.tiledBlockCalls;
            reaches = (call.flags & 0x30) != 0;
            callMask &= 0xEAAAAAAAU;
        }
        SelectedTiles selected;
        if (!SelectClearTiles(read, g, call.rect, callMask, tileCount, selected, failed))
            return unreadable();
        out.loopTiles += selected.count;
        if (selected.count)
            ++out.tiledLoopCalls;
        reaches = reaches || selected.count != 0;
        if (!reaches)
            ++out.tiledNoneCalls;
        out.allocates = out.allocates || reaches;
    }
    return out;
}

bool ReadClearWatch(const GuestWordReader& read, std::uint32_t g, ClearWatch& out)
{
    if (g > UINT32_MAX - kClearDeviceSpan)
        return false;
    std::array<std::uint32_t, 6> dirty{};
    std::uint32_t                state = 0;
    bool                         ok    = true;
    for (std::uint32_t i = 0; i < dirty.size(); ++i)
        ok = ok && read(g + 0x10 + 4 * i, dirty[i]);
    ok = ok && read(g + 0x30, out.ring) && read(g + 0x2ABC, state) && read(g + 0x3A44, out.cursor) && read(g + 0x3A54, out.allocated);
    ok = ok && read(g + 0x34, out.limit34) && read(g + 0x38, out.limit38) && read(g + 0x3A58, out.poolThreshold) && read(g + 0x31CC, out.targetCount);
    ok = ok && read(g + 0x33B0, out.bracket) && read(g + 0x34CC, out.poolObject) && read(g + 0x3A4C, out.wrapPos) && read(g + 0x31AC, out.predicate);
    // The pool fixed buffer words, read only through a pool object.
    out.poolBase = out.poolSize = 0;
    if (ok && out.poolObject)
        ok = out.poolObject <= UINT32_MAX - 160 && read(out.poolObject + 152, out.poolBase) && read(out.poolObject + 156, out.poolSize);
    // The null pool allocator words, read only without a pool object. 0x8269B158
    // loads the progress word through the block pointer G+0x2A90; a null block is
    // not a word the host can tell.
    out.sideEnd = out.wrapLimit = out.segment = out.bracketNext = out.progress = 0;
    if (ok && !out.poolObject)
    {
        std::uint32_t block = 0;
        ok                  = read(g + 0x3A3C, out.sideEnd) && read(g + 0x3A50, out.wrapLimit) && read(g + 0x3A48, out.segment) && read(g + 0x33B4, out.bracketNext) &&
                              read(g + 0x2A90, block) && block != 0 && block <= UINT32_MAX - 4 && read(block + 4, out.progress);
    }
    if (!ok)
        return false;
    for (std::size_t i = 0; i < kClearDirtyWords; ++i)
        out.dirty[i] = (std::uint64_t{ dirty[2 * i] } << 32) | dirty[2 * i + 1];
    out.allocByte  = static_cast<std::uint8_t>(state >> 16);
    out.stateByte  = static_cast<std::uint8_t>(state >> 8);
    out.tilingByte = state >> 24;
    return true;
}

bool ClearStackOverlaps(const GuestWordReader& read, const ClearEntry& entry)
{
    StackReach reach;
    if (StackReachRefuses(read, { entry.pcr, entry.sp }, kClearStackFootprint, entry.g, reach))
        return true;
    if (reach.Overlaps(entry.rect, entry.rect ? 16 : 0) || reach.Overlaps(entry.color, entry.color ? 16 : 0) || reach.OverlapsAny(kClearStackStatics))
        return true;
    const std::uint64_t g = entry.g;
    return StackReachesPointee(read, reach, g + 0x3098, 0x24, 4) || StackReachesPointee(read, reach, g + 0x30A8, 0x24, 4) || StackReachesAllocator(read, reach, entry.g);
}

unsigned ClearTilingShape(const ClearEffects& effects, const ClearWatch& entry) noexcept
{
    unsigned shape = 0;
    if (effects.tiled)
        shape |= kClearShapeLoop;
    if (entry.bracket)
        shape |= kClearShapeBracket;
    if (entry.tilingByte & 0x10)
        shape |= kClearShapeByte10;
    if (((entry.tilingByte & 0x20) != 0) != (entry.bracket != 0))
        shape |= kClearShapeState;
    if (ClearShapeTiled(shape) && entry.predicate)
        shape |= kClearShapePredicate;
    return shape;
}

#if REREVVED_DEVELOPER
ClearEffectResult CompareClearEffects(const ClearEffects& predicted, const ClearWatch& entry, const ClearWatch& exit) noexcept
{
    ClearEffectResult result;
    result.stateChanged = entry.stateByte != exit.stateByte;
    result.ringDelta    = static_cast<std::int32_t>(exit.ring - entry.ring);
    result.allocDelta   = static_cast<std::int32_t>(exit.allocated - entry.allocated);
    result.cursorDelta  = static_cast<std::int32_t>(exit.cursor - entry.cursor);
    if (!predicted.reachedFlush)
    {
        result.early        = true;
        result.earlyChanged = entry.dirty != exit.dirty || entry.allocByte != exit.allocByte;
        return result;
    }
    result.markerAtEntry = (entry.allocByte & kClearAllocFailedBit) != 0;
    if (!result.markerAtEntry && (exit.allocByte & kClearAllocFailedBit))
    {
        result.allocFailed = true;
        return result;
    }
    const bool allocated = predicted.allocates && !result.markerAtEntry;
    for (std::size_t i = 0; i < kClearDirtyWords; ++i)
    {
        result.expect[i] = (entry.dirty[i] & predicted.keep[i]) | predicted.set[i] | (allocated ? predicted.setIfAllocated[i] : 0);
        if (exit.dirty[i] != result.expect[i])
            result.mismatch |= 1U << i;
    }
    result.expectByte             = static_cast<std::uint8_t>(entry.allocByte | predicted.byteSet);
    result.compared               = true;
    const std::uint8_t difference = exit.allocByte ^ result.expectByte;
    if (difference == kClearSwitchBit && (exit.allocByte & kClearSwitchBit))
        result.switchBit = true;
    else if (difference)
        result.mismatch |= 1U << 3;
    return result;
}
#endif

namespace
{

// The most ring words of the body outside the calls: three blocks of 34, 15
// and 14 words, with one ring check before each (g158).
constexpr std::uint64_t kBodyBlockWords = 34 + 15 + 14;
// The most ring words 0x826A1D90 emits itself: the stencil block (2), the block
// after its 0x826AC8C8 (4) and the fast path block (6) (g88).
constexpr std::uint64_t kCallWords = 2 + 4 + 6;
// The ring words of one 0x826A1800 call: 14 + 8 after the vertex data (g96).
constexpr std::uint64_t kStripCallWords = 22;
// The ring words of one tile of the loop of 0x826A1D90 (and of its block A): the
// two words of the tile mask predicate (g88:13728-13759, 13576-13609).
constexpr std::uint64_t kTileWords = 2;
// The bytes of one strip in a side buffer: 21 dwords (g96:12915-12925).
constexpr std::uint32_t kStripBytes = 84;

// The tile grid of the multisample mode, width by height (g22:12905-12933): 80x16
// for mode 0, 80x8 for mode 1 and 40x8 for the others.
struct StripGrid
{
    std::int32_t width = 40, height = 8;
};

StripGrid stripGrid(std::uint32_t control) noexcept
{
    const std::uint32_t mode = (control >> 16) & 3;
    return { mode <= 1 ? 80 : 40, mode == 0 ? 16 : 8 };
}

// A coordinate aligned to the grid by divw and mullw (32-bit signed, truncating
// toward zero), rounded up first when `up` (g22:12934-12997).
std::int32_t stripAlign(std::int32_t value, std::int32_t unit, bool up) noexcept
{
    const std::int32_t adjusted = up ? add32(value, unit - 1) : value;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(adjusted / unit) * static_cast<std::uint32_t>(unit));
}

} // namespace

std::optional<ClearSideBuffers> DeriveClearSideBuffers(const GuestWordReader& read, const ClearEntry& entry, const ClearCalls& calls, std::string* why)
{
    const auto refuse = [&](std::string text) -> std::optional<ClearSideBuffers>
    {
        if (why)
            *why = std::move(text);
        return std::nullopt;
    };
    ClearSideBuffers out;
    if (calls.overflow)
        return refuse("call_overflow");
    if (!calls.count)
        return out;
    if (entry.g > UINT32_MAX - 0x3400)
        return refuse("address_overflow");
    std::uint32_t control = 0, split = 0;
    if (!read(entry.g + 0x2880, control))
        return refuse(std::format("unreadable {:08X}", entry.g + 0x2880));
    if (!read(kClearStripWord, split))
        return refuse(std::format("unreadable {:08X}", kClearStripWord));
    const StripGrid grid = stripGrid(control);

    // The tile loop's inputs, as DeriveClearEffects reads them (g88:13389-13487).
    bool          tiled = false;
    std::uint32_t mask = 0, tileCount = 0, failed = 0;
    if (!ClearDeviceTiled(read, entry.g, tiled, nullptr, &failed))
        return refuse(std::format("unreadable {:08X}", failed));
    if (tiled)
    {
        if (!read(entry.g + 0x31A4, mask))
            return refuse(std::format("unreadable {:08X}", entry.g + 0x31A4));
        if (!read(entry.g + 0x31CC, tileCount))
            return refuse(std::format("unreadable {:08X}", entry.g + 0x31CC));
        if (tileCount > kClearMaxTiles)
            return refuse(std::format("tile_count {}", tileCount));
    }

    // The takes of one call to 0x826A1AF0 with this rectangle.
    const auto addStrips = [&](const std::array<std::int32_t, 4>& rect)
    {
        if (out.count + 2 > out.bytes.size())
            return false;
        const std::int32_t left   = stripAlign(rect[0], grid.width, true);
        const std::int32_t right  = stripAlign(rect[2], grid.width, false);
        const std::int32_t top    = stripAlign(rect[1], grid.height, true);
        const std::int32_t bottom = stripAlign(rect[3], grid.height, false);
        // No interior, or the split disabled: the whole rectangle in one strip.
        if (left >= right || top >= bottom || split == 0)
        {
            out.bytes[out.count++] = kStripBytes;
            return true;
        }
        const std::uint32_t edges = (left > rect[0] ? 1U : 0U) + (right < rect[2] ? 1U : 0U) + (top > rect[1] ? 1U : 0U) + (bottom < rect[3] ? 1U : 0U);
        if (edges)
            out.bytes[out.count++] = edges * kStripBytes;
        out.bytes[out.count++] = kStripBytes;
        return true;
    };
    for (std::size_t n = 0; n < calls.count; ++n)
    {
        const auto& rect = calls.calls[n].rect;
        if (!tiled)
        {
            if (!addStrips(rect))
                return refuse("side_overflow");
            continue;
        }
        SelectedTiles selected;
        if (!SelectClearTiles(read, entry.g, rect, mask, tileCount, selected, failed))
            return refuse(std::format("unreadable {:08X}", failed));
        for (std::size_t t = 0; t < selected.count; ++t)
        {
            // The clipped rectangle moved to the tile origin by the offsets
            // G+0x32C0+8i (x) and G+0x32C4+8i (y), as 32-bit subtractions
            // (g88:13760-13772).
            const SelectedTile& tile = selected.tile[t];
            std::uint32_t       x = 0, y = 0;
            if (!read(entry.g + 0x32C0 + 8 * tile.index, x))
                return refuse(std::format("unreadable {:08X}", entry.g + 0x32C0 + 8 * tile.index));
            if (!read(entry.g + 0x32C4 + 8 * tile.index, y))
                return refuse(std::format("unreadable {:08X}", entry.g + 0x32C4 + 8 * tile.index));
            const auto move = [](std::int32_t value, std::uint32_t offset)
            {
                return static_cast<std::int32_t>(static_cast<std::uint32_t>(value) - offset);
            };
            if (!addStrips({ move(tile.rect[0], x), move(tile.rect[1], y), move(tile.rect[2], x), move(tile.rect[3], y) }))
                return refuse("side_overflow");
        }
    }
    return out;
}

std::uint64_t ClearMaxRingWords(const ClearEffects& effects, const ClearSideBuffers& sides, const ClearWatch& entry) noexcept
{
    if (entry.targetCount > kRingMaxTargetCount)
        return UINT64_MAX;
    const std::uint64_t n = entry.targetCount;
    return RingWordsAc8c8(n)                                                     // the body flush call
           + RingFlushGroupWords(entry.dirty[0], entry.dirty[1], entry.dirty[2]) // the six flush groups
           + kBodyBlockWords                                                     // the three blocks after them
           + effects.calls * (kCallWords + RingWordsAc8c8(n))                    // each 0x826A1D90
           + sides.count * kStripCallWords                                       // each 0x826A1800
           + (effects.loopTiles + effects.tiledBlockCalls) * kTileWords          // each tile of a loop, and block A
           + RingWords8d3a0(n);                                                  // the body last flush call
}

std::optional<ClearExitState> DeriveClearExitState(const ClearEffects& effects, const ClearSideBuffers& sides, const ClearWatch& entry, ClearExitRefusal* refusal, std::string* why)
{
    const auto refuse = [&](ClearExitRefusal reason, const char* text) -> std::optional<ClearExitState>
    {
        if (refusal)
            *refusal = reason;
        if (why)
            *why = text;
        return std::nullopt;
    };
    if (refusal)
        *refusal = ClearExitRefusal::None;
    if (effects.tiledBlockCalls)
        return refuse(ClearExitRefusal::TiledBlock, "tiled_block");
    if (!effects.reachedFlush)
        return refuse(ClearExitRefusal::EarlyExit, "early_exit");
    if (entry.allocByte & kClearAllocFailedBit)
        return refuse(ClearExitRefusal::AllocMarker, "alloc_marker");
    // Every untiled call makes at least one 0x826A1800 call and a Clear without
    // calls makes none, so the side buffers must agree with the effects.
    if (effects.allocates != (sides.count != 0))
        return refuse(ClearExitRefusal::AllocPath, "alloc_count");
    // A pool object without a fixed buffer is the indirect allocator 0x826A6AB8
    // (g93:12821-12824), whose success the host cannot tell.
    if (sides.count && entry.poolObject && !entry.poolBase)
        return refuse(ClearExitRefusal::AllocPath, "alloc_path");
    const std::uint64_t words = ClearMaxRingWords(effects, sides, entry);
    const std::uint64_t end   = words == UINT64_MAX ? UINT64_MAX : std::uint64_t{ entry.ring } + 4 * words;
    if (end > UINT32_MAX - 164)
        return refuse(ClearExitRefusal::RingSpace, "ring_space");

    std::uint32_t pos = entry.cursor, wrapPos = entry.wrapPos, used = entry.allocated, limit34 = entry.limit34, limit38 = entry.limit38;
    if (entry.poolObject)
    {
        // 0x826A6848 takes at g100:13237-13331 lower ring limits by pool bytes
        // before ring-space checks.
        std::uint32_t lastBuffer = 0;
        for (std::size_t i = 0; i < sides.count; ++i)
        {
            const std::uint32_t bytes  = sides.bytes[i];
            const std::uint32_t top    = pos ? pos : (entry.poolSize & ~3U) + entry.poolBase;
            const std::uint32_t buffer = (top - bytes) & ~3U;
            const std::uint32_t delta  = (top - buffer) & ~3U;
            if (limit38 < delta || limit34 < delta)
                return refuse(ClearExitRefusal::RingSpace, "ring_limit");
            limit34 -= delta;
            limit38 -= delta;
            // 0x8269B818: past the threshold the next ring check flushes (g93:12840-12865).
            used += bytes;
            if (used > entry.poolThreshold && entry.bracket == 0)
                return refuse(ClearExitRefusal::RingSpace, "pool_threshold");
            pos        = buffer;
            lastBuffer = buffer;
        }
        if (end > limit38 || end >= limit34)
            return refuse(ClearExitRefusal::RingSpace, "ring_space");
        // A take fails below the cursor plus 164, the cursor is no lower than the
        // end of the words, and the takes only descend.
        if (sides.count && lastBuffer < end + 164)
            return refuse(ClearExitRefusal::AllocCapacity, "alloc_capacity");
    }
    else
    {
        // Null allocator 0x8269B618 takes above G+0x34 and moves neither ring
        // limit.0x8269B818 adds takes to G+0x3A54(g93:12826-12839); exceeding
        // threshold makes next ring check flush.
        if (end > limit38 || end >= limit34)
            return refuse(ClearExitRefusal::RingSpace, "ring_space");
        for (std::size_t i = 0; i < sides.count; ++i)
            used += sides.bytes[i];
        if (sides.count && used > entry.poolThreshold && entry.bracket == 0)
            return refuse(ClearExitRefusal::RingSpace, "pool_threshold");
        // The takes in order, each seeing the position the one before stored.
        ResolveWatch view;
        view.limit34     = entry.limit34;
        view.sideEnd     = entry.sideEnd;
        view.wrapLimit   = entry.wrapLimit;
        view.segment     = entry.segment;
        view.bracket     = entry.bracket;
        view.bracketNext = entry.bracketNext;
        view.progress    = entry.progress;
        view.poolPos     = pos;
        view.wrapPos     = wrapPos;
        for (std::size_t i = 0; i < sides.count; ++i)
        {
            const auto take = ModelNullPoolAllocator(view, sides.bytes[i], 4);
            if (!take.ok)
                return refuse(ClearExitRefusal::AllocCapacity, "alloc_fail");
            if (take.waits)
                return refuse(ClearExitRefusal::AllocWait, "alloc_wait");
            (take.wrap ? view.wrapPos : view.poolPos) = take.end;
        }
        pos     = view.poolPos;
        wrapPos = view.wrapPos;
    }

    ClearExitState out;
    for (std::size_t i = 0; i < kClearDirtyWords; ++i)
        out.dirty[i] = (entry.dirty[i] & effects.keep[i]) | effects.set[i] | (sides.count ? effects.setIfAllocated[i] : 0);
    out.allocByte = static_cast<std::uint8_t>(entry.allocByte | effects.byteSet);
    out.limit34   = limit34;
    out.limit38   = limit38;
    out.poolPos   = sides.count ? pos : entry.cursor;
    out.poolUsed  = used;
    out.wrapPos   = wrapPos;
    out.shape     = ClearTilingShape(effects, entry);
    return out;
}

unsigned ClearExitMismatches(const ClearExitState& predicted, const ClearWatch& exit) noexcept
{
    unsigned mask = 0;
    for (std::size_t i = 0; i < kClearDirtyWords; ++i)
        mask |= predicted.dirty[i] != exit.dirty[i] ? 1U << i : 0U;
    mask |= predicted.allocByte != exit.allocByte ? 1U << 3 : 0U;
    mask |= predicted.limit34 != exit.limit34 ? 1U << 4 : 0U;
    mask |= predicted.limit38 != exit.limit38 ? 1U << 5 : 0U;
    mask |= predicted.poolPos != exit.cursor ? 1U << 6 : 0U;
    mask |= predicted.poolUsed != exit.allocated ? 1U << 7 : 0U;
    mask |= predicted.wrapPos != exit.wrapPos ? 1U << 8 : 0U;
    return mask;
}

std::vector<ClearWrite> ClearExitWrites(std::uint32_t g, const ClearWatch& entry, const ClearExitState& exit)
{
    std::vector<ClearWrite> writes;
    const auto              word = [&](std::uint32_t address, std::uint32_t before, std::uint32_t after)
    {
        if (before != after)
            writes.push_back({ address, after, false });
    };
    for (std::uint32_t i = 0; i < kClearDirtyWords; ++i)
    {
        word(g + 0x10 + 8 * i, static_cast<std::uint32_t>(entry.dirty[i] >> 32), static_cast<std::uint32_t>(exit.dirty[i] >> 32));
        word(g + 0x14 + 8 * i, static_cast<std::uint32_t>(entry.dirty[i]), static_cast<std::uint32_t>(exit.dirty[i]));
    }
    if (entry.allocByte != exit.allocByte)
        writes.push_back({ g + 0x2ABD, exit.allocByte, true });
    word(g + 0x34, entry.limit34, exit.limit34);
    word(g + 0x38, entry.limit38, exit.limit38);
    word(g + 0x3A44, entry.cursor, exit.poolPos);
    word(g + 0x3A54, entry.allocated, exit.poolUsed);
    word(g + 0x3A4C, entry.wrapPos, exit.wrapPos);
    return writes;
}

#if REREVVED_DEVELOPER
void NoteClearExit(unsigned mismatch, unsigned shape) noexcept
{
    constexpr auto relaxed = std::memory_order_relaxed;
    if (!mismatch)
        exitCounters.ok.fetch_add(1, relaxed);
    if (ClearShapeTiled(shape))
    {
        exitCounters.tiled.fetch_add(1, relaxed);
        if (!mismatch)
            exitCounters.tiledOk.fetch_add(1, relaxed);
        for (std::size_t i = 0; i < kClearShapeCount; ++i)
            if (shape & (1U << i))
                exitCounters.shapes[i].fetch_add(1, relaxed);
    }
    for (std::size_t i = 0; i < kClearExitWordCount; ++i)
        if (mismatch & (1U << i))
            exitCounters.mismatch[i].fetch_add(1, std::memory_order_relaxed);
}

void NoteClearExitUnmodeled(ClearExitRefusal refusal) noexcept
{
    exitCounters.unmodeled[static_cast<std::size_t>(refusal)].fetch_add(1, std::memory_order_relaxed);
}

ClearExitCounts TakeClearExitCounts() noexcept
{
    ClearExitCounts taken;
    taken.ok      = exitCounters.ok.exchange(0, std::memory_order_relaxed);
    taken.tiled   = exitCounters.tiled.exchange(0, std::memory_order_relaxed);
    taken.tiledOk = exitCounters.tiledOk.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kClearShapeCount; ++i)
        taken.shapes[i] = exitCounters.shapes[i].exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kClearExitWordCount; ++i)
        taken.mismatch[i] = exitCounters.mismatch[i].exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kClearExitRefusalCount; ++i)
        taken.unmodeled[i] = exitCounters.unmodeled[i].exchange(0, std::memory_order_relaxed);
    return taken;
}

std::string FormatClearExitCounts(const ClearExitCounts& counts)
{
    std::string fields = std::format(" clear_exit_ok={} clear_exit_tiled={} clear_exit_tiled_ok={}", counts.ok, counts.tiled, counts.tiledOk);
    for (std::size_t i = 0; i < kClearExitWordCount; ++i)
        fields += std::format(" clear_exit_mismatch_{}={}", kClearExitWordNames[i], counts.mismatch[i]);
    for (std::size_t i = 0; i < kClearShapeCount; ++i)
        if (counts.shapes[i])
            fields += std::format(" clear_exit_tiled_{}={}", kClearShapeNames[i], counts.shapes[i]);
    for (std::size_t i = 0; i < kClearExitRefusalCount; ++i)
        if (counts.unmodeled[i])
            fields += std::format(" clear_exit_unmodeled_{}={}", kClearExitRefusalNames[i], counts.unmodeled[i]);
    return fields;
}

bool ClearEffectCheckRequested() noexcept
{
    return REXCVAR_GET(native_clear_effect_check);
}

void NoteClearEffect(const ClearEffects& predicted, const ClearEffectResult& result) noexcept
{
    constexpr auto relaxed = std::memory_order_relaxed;
    if (result.early)
        (result.earlyChanged ? effectCounters.earlyChanged : effectCounters.earlyOk).fetch_add(1, relaxed);
    else
    {
        (predicted.color ? effectCounters.color : effectCounters.depthOnly).fetch_add(1, relaxed);
        if (!predicted.calls)
            effectCounters.flushNoCalls.fetch_add(1, relaxed);
        effectCounters.stencil.fetch_add(predicted.stencilCalls, relaxed);
        effectCounters.fast.fetch_add(predicted.fastCalls, relaxed);
        effectCounters.pathUntiled.fetch_add(predicted.untiledCalls, relaxed);
        effectCounters.pathTiledBlock.fetch_add(predicted.tiledBlockCalls, relaxed);
        effectCounters.pathTiledLoop.fetch_add(predicted.tiledLoopCalls, relaxed);
        effectCounters.pathTiledNone.fetch_add(predicted.tiledNoneCalls, relaxed);
        if (result.allocFailed)
            effectCounters.allocFailed.fetch_add(1, relaxed);
        if (result.markerAtEntry)
            effectCounters.markerAtEntry.fetch_add(1, relaxed);
        if (result.switchBit)
            effectCounters.switchBit.fetch_add(1, relaxed);
        if (result.compared)
        {
            if (!result.mismatch)
                effectCounters.exactOk.fetch_add(1, relaxed);
            for (std::size_t i = 0; i < kClearEffectFieldCount; ++i)
                if (result.mismatch & (1U << i))
                    effectCounters.mismatch[i].fetch_add(1, relaxed);
        }
    }
    if (result.stateChanged)
        effectCounters.stateChanged.fetch_add(1, relaxed);
    effectCounters.allocBytesSum.fetch_add(result.allocDelta, relaxed);
    effectCounters.ringDeltaSum.fetch_add(result.ringDelta, relaxed);
    effectCounters.cursorDeltaSum.fetch_add(result.cursorDelta, relaxed);
}

void NoteClearEffectUnsupported() noexcept
{
    effectCounters.unsupported.fetch_add(1, std::memory_order_relaxed);
}

void NoteClearEffectUnpaired(unsigned count) noexcept
{
    if (count)
        effectCounters.unpaired.fetch_add(count, std::memory_order_relaxed);
}

ClearEffectCounts TakeClearEffectCounts() noexcept
{
    constexpr auto    relaxed = std::memory_order_relaxed;
    ClearEffectCounts taken;
    taken.exactOk        = effectCounters.exactOk.exchange(0, relaxed);
    taken.earlyOk        = effectCounters.earlyOk.exchange(0, relaxed);
    taken.earlyChanged   = effectCounters.earlyChanged.exchange(0, relaxed);
    taken.allocFailed    = effectCounters.allocFailed.exchange(0, relaxed);
    taken.unsupported    = effectCounters.unsupported.exchange(0, relaxed);
    taken.unpaired       = effectCounters.unpaired.exchange(0, relaxed);
    taken.pathUntiled    = effectCounters.pathUntiled.exchange(0, relaxed);
    taken.pathTiledBlock = effectCounters.pathTiledBlock.exchange(0, relaxed);
    taken.pathTiledLoop  = effectCounters.pathTiledLoop.exchange(0, relaxed);
    taken.pathTiledNone  = effectCounters.pathTiledNone.exchange(0, relaxed);
    taken.color          = effectCounters.color.exchange(0, relaxed);
    taken.depthOnly      = effectCounters.depthOnly.exchange(0, relaxed);
    taken.stencil        = effectCounters.stencil.exchange(0, relaxed);
    taken.fast           = effectCounters.fast.exchange(0, relaxed);
    taken.flushNoCalls   = effectCounters.flushNoCalls.exchange(0, relaxed);
    taken.stateChanged   = effectCounters.stateChanged.exchange(0, relaxed);
    taken.markerAtEntry  = effectCounters.markerAtEntry.exchange(0, relaxed);
    taken.switchBit      = effectCounters.switchBit.exchange(0, relaxed);
    taken.allocBytesSum  = effectCounters.allocBytesSum.exchange(0, relaxed);
    taken.ringDeltaSum   = effectCounters.ringDeltaSum.exchange(0, relaxed);
    taken.cursorDeltaSum = effectCounters.cursorDeltaSum.exchange(0, relaxed);
    for (std::size_t i = 0; i < kClearEffectFieldCount; ++i)
        taken.mismatch[i] = effectCounters.mismatch[i].exchange(0, relaxed);
    return taken;
}

std::string FormatClearEffectCounts(const ClearEffectCounts& counts)
{
    std::string fields = std::format(" clear_effect_exact_ok={}", counts.exactOk);
    for (std::size_t i = 0; i < kClearEffectFieldCount; ++i)
        fields += std::format(" clear_effect_exact_mismatch_{}={}", kClearEffectFieldNames[i], counts.mismatch[i]);
    fields += std::format(
        " clear_effect_early_ok={} clear_effect_early_changed={} clear_effect_alloc_failed={} clear_effect_unsupported={} clear_effect_unpaired={}"
        " clear_effect_path_untiled={} clear_effect_path_tiled_block={} clear_effect_path_tiled_loop={} clear_effect_path_tiled_none={}"
        " clear_effect_color={} clear_effect_depth_only={} clear_effect_stencil={} clear_effect_fast={} clear_effect_flush_no_calls={}"
        " clear_effect_2abe_changed={} clear_effect_alloc_bytes_sum={} clear_effect_ring_delta_sum={} clear_effect_cursor_delta_sum={}"
        " clear_effect_alloc_marker_entry={} clear_effect_2abd_switch_bit={}",
        counts.earlyOk,
        counts.earlyChanged,
        counts.allocFailed,
        counts.unsupported,
        counts.unpaired,
        counts.pathUntiled,
        counts.pathTiledBlock,
        counts.pathTiledLoop,
        counts.pathTiledNone,
        counts.color,
        counts.depthOnly,
        counts.stencil,
        counts.fast,
        counts.flushNoCalls,
        counts.stateChanged,
        counts.allocBytesSum,
        counts.ringDeltaSum,
        counts.cursorDeltaSum,
        counts.markerAtEntry,
        counts.switchBit);
    return fields;
}

bool ObserveClearCall(const GuestWordReader& read, const std::array<std::uint32_t, 8>& r, std::uint32_t stack, double depth, ClearCall& out, std::string* why)
{
    out              = {};
    out.flags        = r[1];
    out.index        = r[2];
    out.rect         = { static_cast<std::int32_t>(r[3]), static_cast<std::int32_t>(r[4]), static_cast<std::int32_t>(r[5]), static_cast<std::int32_t>(r[6]) };
    out.colorPointer = r[7];
    out.depthBits    = std::bit_cast<std::uint64_t>(depth);
    const auto fail  = [&](std::string text)
    {
        if (why)
            *why = std::move(text);
        return false;
    };
    // The stencil is stored at 92(r1) of the body before each call (g158:13731, 13848).
    if (stack > UINT32_MAX - 92 || !read(stack + 92, out.stencil))
        return fail("unreadable stencil at r1+92");
    if (out.ColorUsed())
    {
        if (r[7] > UINT32_MAX - 16)
            return fail("address_overflow");
        for (std::uint32_t i = 0; i < 4; ++i)
            if (!read(r[7] + 4 * i, out.color[i]))
                return fail(std::format("unreadable color at {:08X}", r[7] + 4 * i));
    }
    return true;
}
#endif

unsigned ClearProbeStack::Push(std::uint32_t entrySp, ClearProbe probe, ClearDropCounts* drops)
{
    unsigned        dropped = 0;
    ClearDropCounts counts;
    // A live outer call's frame is at or above the entry r1 of any call it
    // makes (a tail-jumping callback enters with exactly its frame).
    while (count_ && slots_[count_ - 1].frame < entrySp)
    {
        --count_;
        ++dropped;
        // A probe a hook already counted as unpaired is not counted twice.
        counts.derive += slots_[count_].live && slots_[count_].deriveCalls ? 1U : 0U;
        counts.effects += slots_[count_].live && slots_[count_].effects ? 1U : 0U;
    }
    probe.frame = entrySp - kClearFrameSize;
    if (count_ == kDepth)
    {
        ++dropped;
        counts.derive += probe.deriveCalls ? 1U : 0U;
        counts.effects += probe.effects ? 1U : 0U;
    }
    else
        slots_[count_++] = std::move(probe);
    if (drops)
    {
        drops->derive += counts.derive;
        drops->effects += counts.effects;
    }
    return dropped;
}

ClearProbe* ClearProbeStack::Find(std::uint32_t frame, unsigned& dropped, ClearDropCounts* drops)
{
    dropped = 0;
    for (std::size_t i = count_; i-- > 0;)
        if (slots_[i].frame == frame)
        {
            dropped = static_cast<unsigned>(count_ - 1 - i);
            if (drops)
                for (std::size_t above = i + 1; above < count_; ++above)
                {
                    drops->derive += slots_[above].live && slots_[above].deriveCalls ? 1U : 0U;
                    drops->effects += slots_[above].live && slots_[above].effects ? 1U : 0U;
                }
            count_ = i + 1;
            return &slots_[i];
        }
    return nullptr;
}

std::optional<ClearProbe> ClearProbeStack::Pop(std::uint32_t frame, unsigned& dropped, ClearDropCounts* drops)
{
    auto* found = Find(frame, dropped, drops);
    if (!found)
        return std::nullopt;
    --count_;
    return std::move(*found);
}

#if REREVVED_DEVELOPER
unsigned ClearMismatches(const ClearCalls& derived, const ClearCalls& observed) noexcept
{
    unsigned mask = derived.Total() != observed.Total() ? 1U : 0U;
    for (std::size_t i = 0; i < std::min(derived.count, observed.count); ++i)
    {
        const auto& a = derived.calls[i];
        const auto& b = observed.calls[i];
        if (a.index != b.index)
            mask |= 2U;
        if (a.flags != b.flags)
            mask |= 4U;
        if (a.rect != b.rect)
            mask |= 8U;
        if (a.colorPointer != b.colorPointer || (a.ColorUsed() && b.ColorUsed() && a.color != b.color))
            mask |= 16U;
        if (a.depthBits != b.depthBits)
            mask |= 32U;
        if (a.stencil != b.stencil)
            mask |= 64U;
    }
    return mask;
}

bool ClearDeriveCheckRequested() noexcept
{
    return REXCVAR_GET(native_clear_derive_check);
}

unsigned NoteClearDerive(const std::optional<ClearCalls>& derived, const ClearCalls& observed, bool observedReadable) noexcept
{
    if (!derived || !observedReadable)
    {
        counters.unsupported.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const auto mask = ClearMismatches(*derived, observed);
    if (!mask)
        counters.ok.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kClearFieldCount; ++i)
        if (mask & (1U << i))
            counters.mismatch[i].fetch_add(1, std::memory_order_relaxed);
    return mask;
}

void NoteClearUnpaired(unsigned count) noexcept
{
    if (count)
        counters.unpaired.fetch_add(count, std::memory_order_relaxed);
}

ClearDeriveCounts TakeClearDeriveCounts() noexcept
{
    ClearDeriveCounts taken;
    taken.ok          = counters.ok.exchange(0, std::memory_order_relaxed);
    taken.unsupported = counters.unsupported.exchange(0, std::memory_order_relaxed);
    taken.unpaired    = counters.unpaired.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kClearFieldCount; ++i)
        taken.mismatch[i] = counters.mismatch[i].exchange(0, std::memory_order_relaxed);
    return taken;
}

std::string FormatClearDeriveCounts(const ClearDeriveCounts& counts)
{
    std::string fields = std::format(" clear_derive_ok={}", counts.ok);
    for (std::size_t i = 0; i < kClearFieldCount; ++i)
        fields += std::format(" clear_derive_mismatch_{}={}", kClearFieldNames[i], counts.mismatch[i]);
    fields += std::format(" clear_derive_unsupported={} clear_derive_unpaired={}", counts.unsupported, counts.unpaired);
    return fields;
}

std::string FormatClearCall(const ClearCall& call)
{
    std::string text = std::format("flags={:08X} index={} rect={},{},{},{} color={:08X}",
                                   call.flags,
                                   call.index == kGuestDepthOnlyClearIndex ? std::string("-1") : std::to_string(call.index),
                                   call.rect[0],
                                   call.rect[1],
                                   call.rect[2],
                                   call.rect[3],
                                   call.colorPointer);
    if (call.ColorUsed())
        text += std::format("[{:08X},{:08X},{:08X},{:08X}]", call.color[0], call.color[1], call.color[2], call.color[3]);
    text += std::format(" depth={:016X} stencil={:08X}", call.depthBits, call.stencil);
    return text;
}

std::string FormatClearCalls(const ClearCalls& calls)
{
    std::string text = "[";
    for (std::size_t i = 0; i < calls.count; ++i)
        text += std::format("{}{{{}}}", i ? " " : "", FormatClearCall(calls.calls[i]));
    if (calls.overflow)
        text += std::format(" +{}", calls.overflow);
    return text + "]";
}
#endif

} // namespace rerevved::gpu::guest_d3d
