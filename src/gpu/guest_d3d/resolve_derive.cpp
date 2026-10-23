#include "resolve_derive.h"

#include "ring_words.h"

#include <rex/cvar.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <utility>

#if REREVVED_DEVELOPER
REXCVAR_DEFINE_BOOL(native_resolve_derive_check,
                    false,
                    "ReRevved",
                    "Bring-up only for renderer=native: for each Resolve call the entry observer admits, derive the copy words G+0x2A18..0x2A24 on the "
                    "host from the entry registers, the device and the destination header, and compare them with the words the hook at 0x8269E8A0 "
                    "reads; also compare the clear words G+0x2A2C..0x2A34 at that hook with their values at the Resolve return. Observes only; "
                    "resolve_derive_ok, resolve_derive_mismatch_*, resolve_derive_unsupported and resolve_clear_words_* fields on the "
                    "native_frame_timing line")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_resolve_effect_check,
                    false,
                    "ReRevved",
                    "Bring-up only for renderer=native: for each Resolve call the entry observer admits, predict on the host the guest words the call "
                    "changes besides the copy words (G+0x2A18..0x2A24 at the return, G+0x2A44, the dirty words G+0x10, G+0x18 and G+0x20, the bytes "
                    "G+0x2ABD and G+0x2ABE, and the destination header words +8 and +12), read them at the entry and again at the return hook "
                    "0x8269F360, and compare. Observes only; resolve_effect_* fields on the native_frame_timing line")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif

namespace rerevved::gpu::guest_d3d
{

namespace
{

#if REREVVED_DEVELOPER
struct EffectCounters
{
    std::atomic<std::uint64_t>                                       ok{ 0 }, unsupported{ 0 }, unpaired{ 0 }, dirtyCleared{ 0 }, cleared1E0000{ 0 }, allocFailed{ 0 }, listPath{ 0 };
    std::array<std::atomic<std::uint64_t>, kResolveEffectFieldCount> mismatch{};
    std::atomic<std::uint64_t>                                       exactOk{ 0 }, exactSkipped{ 0 }, exactTiled{ 0 }, exactTiledOk{ 0 }, exactClear{ 0 }, exactClearOk{ 0 };
    std::atomic<std::uint64_t>                                       exactPredicated{ 0 }, exactPredicatedOk{ 0 }, predicated{ 0 }, predicatedClear{ 0 };
    std::array<std::atomic<std::uint64_t>, kResolveTileBuckets>      predicatedTiles{};
    std::array<std::atomic<std::uint64_t>, kResolveExitWordCount>    exactMismatch{};
    std::array<std::atomic<std::uint64_t>, kResolveExitRefusalCount> exactRefused{};
};

EffectCounters effectCounters;

struct DeriveCounters
{
    std::atomic<std::uint64_t>                                ok{ 0 }, unsupported{ 0 }, unpaired{ 0 };
    std::array<std::atomic<std::uint64_t>, kResolveWordCount> mismatch{};
    std::atomic<std::uint64_t>                                clearChecked{ 0 }, clearStale{ 0 };
};

DeriveCounters counters;
#endif

// The destination formats for which Resolve (0x8269DDA0, g210:12634-12668)
// forces the number format to 7.
bool forcesNumberFormat(std::uint32_t format)
{
    switch (format)
    {
        case 30:
        case 31:
        case 32:
        case 36:
        case 37:
        case 38:
        case 57:
        case 63:
            return true;
        default:
            return false;
    }
}

// The copy words and the intermediates the effect derivation reuses.
struct Derivation
{
    ResolveCopyWords words;
    bool             large  = false;
    std::uint32_t    d0     = 0;
    std::uint32_t    pitch  = 0;
    std::uint32_t    height = 0;
    std::uint32_t    slot   = 0;
};

std::optional<Derivation> derive(const GuestWordReader& read, const std::array<std::uint32_t, 8>& r, std::string* why)
{
    const auto refuse = [&](std::string text) -> std::optional<Derivation>
    {
        if (why)
            *why = std::move(text);
        return std::nullopt;
    };
    // r4 flags, r5 source rectangle or 0, r6 destination header, r7 destination
    // point or 0, r8 mip level, r9 slice (g167:13052-13073).
    const std::uint32_t g = r[0], rectangle = r[2], header = r[3];
    if (r[5] || r[6])
        return refuse("mip_or_slice");
    if (g > UINT32_MAX - 0x30B8 || header > UINT32_MAX - 0x34 || rectangle > UINT32_MAX - 8 || r[4] > UINT32_MAX - 8)
        return refuse("address_overflow");
    std::uint32_t failed = 0;
    const auto    word   = [&](std::uint32_t address)
    {
        std::uint32_t value = 0;
        if (!read(address, value) && !failed)
            failed = address;
        return value;
    };

    // Without fragment-select bits 0x70, source halfword+24&3 chooses
    // samples(g167:13075-13118). Slot 4 is depth G+0x30A8; color k<4 is
    // G+0x3098+4k.
    std::uint32_t       flags = r[1];
    const std::uint32_t slot  = flags & 7;
    if (!(flags & 0x70))
    {
        const auto surface = word(g + (slot == 4 ? 0x30A8 : 0x3098 + 4 * slot));
        if (failed || surface > UINT32_MAX - 0x1C)
            return refuse(std::format("unreadable {:08X}", failed ? failed : surface));
        const auto samples = (word(surface + 0x18) >> 16) & 3;
        flags |= samples == 0 ? 0x10U : samples == 1 ? 0x50U
                                                     : 0x70U;
    }

    // The destination fetch constant at header+0x1C: dwords 0, 1, 2, 3 and 5
    // (g167:13140-13204).
    const auto d0 = word(header + 28), d1 = word(header + 32), d2 = word(header + 36), d3 = word(header + 40), d5 = word(header + 48);
    // Dword 5 bits 9..10 equal to 0x400 select the 11-bit extent form
    // (g167:13148-13169, 13448-13484).
    const bool large = (d5 & 0x600) == 0x400;
    // The height is the extent field plus 1, or plus 3 with dword 3 bit 31
    // (g167:13144-13176).
    const std::uint32_t height = (large ? (d2 >> 11) & 0x7FF : (d2 >> 13) & 0x1FFF) + 1 + 2 * (d3 >> 31);
    const std::uint32_t base   = d1 & 0xFFFFF000U;
    // Dword 0 bits 22..30 are the pitch in units of 32 (g167:13196).
    const std::uint32_t pitch = (d0 >> 17) & 0x3FE0;
    // The size field is bits 3..7 of the table halfword at index dword 1 & 0x3F
    // (g167:13184, 13200-13204); the table is big-endian halfwords.
    const std::uint32_t tableAddress = kResolveFormatTable + ((d1 & 0x3F) << 1);
    const auto          tableWord    = word(tableAddress & ~3U);
    const std::uint32_t size         = (((tableAddress & 2) ? tableWord & 0xFFFF : tableWord >> 16) >> 3) & 0x1F;

    // A null rectangle is {0, 0, width, height}; a null point reads the zero
    // point (g167:13255-13283). Only left, top and the point are used.
    const std::uint32_t pointAddress = r[4] ? r[4] : kResolveDefaultPoint;
    const std::uint32_t left = rectangle ? word(rectangle) : 0, top = rectangle ? word(rectangle + 4) : 0;
    const std::uint32_t x = word(pointAddress), y = word(pointAddress + 4);
    if (failed)
        return refuse(std::format("unreadable {:08X}", failed));

    Derivation        result;
    ResolveCopyWords& out = result.words;
    result.large          = large;
    result.d0             = d0;
    result.pitch          = pitch;
    result.height         = height;
    result.slot           = slot;
    // Destination base: texels from the point's offset to the rectangle's
    // origin, 32 per row step of x, times the size, plus bit 12 of the
    // base's top 12 bits plus 512 and the base's low 29 bits (g167:13283-13331).
    const std::uint32_t offset = (y - top) * pitch + ((x - left) << 5);
    out.destBase               = offset * size + (((base >> 20) + 512) & 0x1000U) + (base & 0x1FFFFFFFU);
    // Destination pitch: the rows from the point to the destination's end
    // above the pitch (g167:13325-13333, 13441, 13452-13458).
    out.destPitch = ((((top - y) + height) & 0x3FFFU) << 16) | (pitch & 0x3FFFU);

    // Destination info (g167:13341-13460): the format map, the number format
    // and bits 16..21 from flags bits 26..31, the swap bit 24 when dword 3
    // has any of 0xE, and the endian bits from dword 1 bits 6..7.
    std::uint32_t format = d1 & 0x3F;
    switch (format)
    {
        case 54:
            format = 7;
            break;
        case 55:
            format = 16;
            break;
        case 56:
            format = 17;
            break;
        case 27:
            format = 30;
            break;
        case 28:
            format = 31;
            break;
        case 29:
            format = 32;
            break;
        case 22:
        case 23:
            format = 6;
            break;
        default:
            break;
    }
    // Number format: 7 for the formats of 0x8269DDA0, else 2 with dword 3 bit 0
    // plus 1 when dword 0 bits 2..3 equal 1 (g167:13407-13434).
    const std::uint32_t number = forcesNumberFormat(format) ? 7U : ((d3 & 1) ? 2U : 0U) + (((d0 >> 2) & 3) == 1 ? 1U : 0U);
    const std::uint32_t select = ((flags >> 26) & 0x3FU) | ((d3 & 0xE) ? 0x100U : 0U);
    const std::uint32_t info   = (((select << 3) | number) << 6) | format;
    out.destInfo               = (info << 7) | ((d1 >> 6) & 3);
    // The 11-bit form adds bit 3 and, from the slice (0 here), bits 4..6
    // (g167:13462-13484).
    if (large)
        out.destInfo |= 8;

    // Copy control: the flag bits 0x377 less 16, and the source select 3 with
    // flags bit 0x2000, else 0 for the depth slot and 1 (g167:13487-13526).
    const std::uint32_t command = (flags & 0x2000) ? 3U : slot == 4 ? 0U
                                                                    : 1U;
    out.copyControl             = ((flags & 0x377U) - 16U) | (command << 20);
    return result;
}

// Resolve fetch read order; reader: guest word, byte, or halfword; failed:
//   first address that did not read.
// overlapped | set: reading a word Original stores before making that read.
// overlap dirty | word: G+0x20.
// overlap copy | words: G+0x2A18..0x2A24.
// overlap size | word: G+0x2A44; source: g167:13471-13534.
// overlap frames | region: body and callees below entry r1 under 0x400 deep;
//   body=368; 0x8269E448=128; 0x8269E020=176.
// depthStored | set: once the depth block has run; stores: G+0x2A2C and
//   G+0x2A28; source: g50:12516,12532; order: before color vector load and
//   color block read; source: g167:13557-13564.
// block order | effect: within each block, loads of G and the parameter block
//   precede stores to G; source: g50:12452-12512,g209:13079-13154.
struct Fetch
{
    const GuestWordReader& read;
    std::uint32_t          failed = 0;
    std::uint32_t          g = 0, sp = 0;
    bool                   overlapped  = false;
    bool                   depthStored = false;

    bool storedBefore(std::uint32_t address) const
    {
        const auto in = [address](std::uint64_t low, std::uint64_t size)
        {
            return std::uint64_t{ address } + 4 > low && address < low + size;
        };
        return in(std::uint64_t{ g } + 0x20, 8) || in(std::uint64_t{ g } + 0x2A18, 16) || in(std::uint64_t{ g } + 0x2A44, 4) || in(sp < 0x400 ? 0 : sp - 0x400, sp < 0x400 ? sp : 0x400) || (depthStored && in(std::uint64_t{ g } + 0x2A28, 8));
    }

    std::uint32_t word(std::uint32_t address)
    {
        std::uint32_t value = 0;
        if (!read(address, value) && !failed)
            failed = address;
        overlapped = overlapped || storedBefore(address);
        return value;
    }

    std::uint32_t byte(std::uint32_t address)
    {
        return (word(address & ~3U) >> (8 * (3 - (address & 3)))) & 0xFF;
    }

    std::uint32_t half(std::uint32_t address)
    {
        return (word(address & ~3U) >> ((address & 2) ? 0 : 16)) & 0xFFFF;
    }

    float real(std::uint32_t address)
    {
        return std::bit_cast<float>(word(address));
    }
};

// The shift of slw: zero when bit 5 of the count is set (g125:13417-13423,
// 13490-13494; the count is the low byte).
std::uint32_t shiftLeft(std::uint32_t value, std::uint32_t count)
{
    count &= 0xFF;
    return (count & 0x20) ? 0 : value << (count & 0x3F);
}

// fctidz as the generated code evaluates it (g50:12436): NaN and the
// indefinite result are the minimum, above 2^63 the maximum.
std::int64_t convertToZero(double value)
{
    constexpr double kLimit = 9223372036854775808.0;
    if (std::isnan(value))
        return std::numeric_limits<std::int64_t>::min();
    if (value > kLimit)
        return std::numeric_limits<std::int64_t>::max();
    if (value >= kLimit || value < -kLimit)
        return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(value);
}

// fmuls: the product of the operands as doubles, rounded to single.
double multiplySingle(double a, double b)
{
    return static_cast<double>(static_cast<float>(a * b));
}

// The depth in the float24 layout of 0x8269DE98 (g67:13029-13062): zero for a
// negative value, else the exponent and the top mantissa bits shifted into 24
// bits, with a denormal shifted down by the exponent's distance from 113.
std::uint32_t depthToFloat24(double depth)
{
    const auto bits = std::bit_cast<std::uint32_t>(static_cast<float>(depth));
    if (bits & 0x80000000U)
        return 0;
    const std::uint32_t exponent = (bits >> 23) & 0x1FF;
    if (exponent >= 113)
        return ((bits & 0x7800000U) | (bits + 4)) >> 3;
    const std::uint32_t mantissa = bits & 0x7FFFFF;
    const std::uint32_t distance = 113 - (exponent & 0xFF);
    const std::uint32_t scaled   = (shiftLeft(1, distance - 1) + mantissa) | 0x800000U;
    return (distance & 0x20) ? 0 : scaled >> (distance & 0x3F);
}

// 0x8269DF00 words use G+0x2A2C/G+0x2A28; format comes from G+0x2888 when no
// parameter block (g50:12389-12552).
std::array<std::uint32_t, 2> depthClearWords(Fetch& fetch, std::uint32_t g, std::uint32_t parameters, std::uint32_t stencil, double depth)
{
    const float   scale11 = fetch.real(kResolveClearScale11), scale24 = fetch.real(kResolveClearScale24);
    const bool    packedFormat = parameters ? (fetch.half(kResolveFormatTable + 2 * (fetch.word(parameters + 8) & 0x3F)) & 0xF00) != 0 : ((fetch.word(g + 0x2888) >> 16) & 1) != 0;
    std::uint32_t packed = 0, field = 0;
    if (!packedFormat)
    {
        packed = (static_cast<std::uint32_t>(convertToZero(multiplySingle(depth, scale24))) << 8) | stencil;
        field  = static_cast<std::uint32_t>(convertToZero(multiplySingle(depth, scale11)));
    }
    else
    {
        packed = ((depthToFloat24(depth) & 0xFFFFFF) << 8) | (stencil & 0xFF);
        field  = (packed >> 21) & 0x7FF;
    }
    // Bit 11 for a stencil value other than the device byte G+0x2942 and for
    // bit 5 of the word G+0x2940 clear, xored (g50:12510-12531).
    const std::uint32_t flags = fetch.word(g + 0x2940);
    const std::uint32_t byte  = (flags >> 8) & 0xFF;
    const std::uint32_t bitA  = stencil != byte ? 0x800U : 0U;
    const std::uint32_t bitB  = (~flags & 0x20) ? 0x800U : 0U;
    return { (bitA ^ bitB) | field, packed };
}

// Rounds a clamped non-negative channel to the nearest integer, ties to even.
std::uint32_t roundChannel(float value)
{
    const float below = std::floor(value), rest = value - below;
    const bool  up = rest > 0.5F || (rest == 0.5F && std::fmod(below, 2.0F) != 0.0F);
    return static_cast<std::uint32_t>(below + (up ? 1.0F : 0.0F));
}

// 0x8269E020(g125:13208-13890) applies exponent bias then packs unorm formats
// 0/2/10 or raw floats 14/15. Other formats, and NaN only on unorm path, return
// null.
std::optional<std::array<std::uint32_t, 2>> colorWords(Fetch& fetch, std::uint32_t format, std::uint32_t bias, std::array<std::uint32_t, 4> channel)
{
    if (bias)
    {
        // The signed 6-bit bias, ORed with the whole word (g125:13242-13306).
        const std::uint32_t adjust = ((bias & 0x20) ? 0xFFFFFFC0U : 0U) | bias;
        for (auto& bits : channel)
        {
            const std::uint32_t exponent = (bits >> 23) & 0xFF;
            if (exponent - 1 > 253)
                continue;
            const std::int32_t moved   = static_cast<std::int32_t>(exponent + adjust);
            const std::int32_t clamped = moved > 254 ? 254 : moved > 1 ? moved
                                                                       : 1;
            bits                       = (bits & 0x807FFFFFU) | (static_cast<std::uint32_t>(clamped) << 23);
        }
    }
    switch (format)
    {
        case 14:
            return std::array<std::uint32_t, 2>{ channel[0], channel[0] };
        case 15:
            return std::array<std::uint32_t, 2>{ channel[1], channel[0] };
        case 0:
        case 2:
        case 10:
            break;
        default:
            return std::nullopt;
    }
    // The channel widths by format (10 reads as 2), the largest value each
    // reaches, and the value rounded to the nearest integer.
    const std::uint32_t row       = format == 10 ? 2 : format;
    const std::uint32_t colorBits = fetch.byte(kResolveClearColorBits + row), alphaBits = fetch.byte(kResolveClearAlphaBits + row);
    const double        one     = fetch.real(kResolveClearOne);
    const auto          largest = [&](std::uint32_t bits)
    {
        const auto range = static_cast<std::int64_t>(static_cast<std::int32_t>(shiftLeft(1, bits)));
        return static_cast<float>(static_cast<double>(static_cast<float>(static_cast<double>(range))) - one);
    };
    const std::array<float, 4>   maximum{ largest(colorBits), largest(colorBits), largest(colorBits), largest(alphaBits) };
    std::array<std::uint32_t, 4> value{};
    for (std::size_t i = 0; i < 4; ++i)
    {
        const float product = std::bit_cast<float>(channel[i]) * maximum[i];
        if (std::isnan(product))
            return std::nullopt;
        const float scaled = 0.0F > product ? 0.0F : product;
        value[i]           = roundChannel(maximum[i] < scaled ? maximum[i] : scaled);
    }
    const std::uint32_t packed = shiftLeft(value[3], colorBits * 3) | shiftLeft(value[2], colorBits * 2) | shiftLeft(value[1], colorBits) | value[0];
    return std::array<std::uint32_t, 2>{ packed, packed };
}

} // namespace

std::optional<ResolveCopyWords> DeriveResolveWords(const GuestWordReader& read, const std::array<std::uint32_t, 8>& r, std::string* why)
{
    const auto derivation = derive(read, r, why);
    if (!derivation)
        return std::nullopt;
    return derivation->words;
}

std::optional<ResolveEffects> DeriveResolveEffects(const GuestWordReader& read, const std::array<std::uint32_t, 8>& r, std::string* why, const ResolveClearArgs* clear)
{
    const auto derivation = derive(read, r, why);
    if (!derivation)
        return std::nullopt;
    // derive() refused a device within 0x30B8 of the end of the address space,
    // so every device offset below is in range.
    const std::uint32_t g = r[0];
    ResolveEffects      out;
    out.words       = derivation->words;
    out.large       = derivation->large;
    out.pitchHeight = derivation->pitch * derivation->height;
    out.depthClear  = (r[1] & 0x200) != 0;
    out.colorClear  = (r[1] & 0x100) != 0;

    std::uint32_t failed = 0;
    const auto    word   = [&](std::uint32_t address)
    {
        std::uint32_t value = 0;
        if (!read(address, value) && !failed)
            failed = address;
        return value;
    };

    // Before 0x8269B818 (g167:14633), flush g167:13644-13867 clears G+0x10 bits
    // 0..16/21..57, G+0x18 bits 34..54, G+0x20 bits 0..37.
    constexpr auto bit = [](unsigned n)
    {
        return std::uint64_t{ 1 } << n;
    };
    // Earlier G+0x20 ORs 4/0x3C00/0x300/0xC0 (g167:13473-13534; clear blocks
    // 0x8269DF00/0x8269E448) are cleared before return.
    auto& before = out.setBeforeAlloc;
    // The flags 0x200 bit (g167:14299-14323), then the restore block
    // (g167:14455-14523).
    before[0] = (out.depthClear ? bit(17) : 0) | bit(41) | bit(40) | bit(39) | bit(44) | 0xE0;
    before[1] = bit(35);
    before[2] = 0x80000;
    // For non-depth, dword 0 bits 2..3=3 gates shadow G+0x2884+4*k bits
    // 16..19=1 selecting G+0x10 bit 56-k; k=0 for slot 0, else
    // slot+1(g167:14417, g167:14527-14607).
    const std::uint32_t slot = derivation->slot;
    if (((derivation->d0 >> 2) & 3) == 3 && slot != 4)
    {
        const std::uint32_t k      = slot ? slot + 1 : 0;
        const auto          shadow = word(g + 0x2884 + 4 * k);
        if ((shadow & 0xF0000) == 0x10000)
            before[0] |= bit(56 - k);
    }
    // After it, with the destination header stores (g167:14928-14976).
    out.setAfterAlloc[0] = 0x80000 | 0x10000 | 0x8000 | 0x800 | 0x100 | 8;
    out.setAfterAlloc[1] = 0x80000000;
    if (out.depthClear || out.colorClear)
    {
        // The caller's stack words the blocks read (g167:13519, 13539): the
        // stencil value at entry r1+92 and the parameter block at r1+100.
        if (!clear)
        {
            if (why)
                *why = "clear_inputs";
            return std::nullopt;
        }
        if (clear->sp > UINT32_MAX - 104 || (out.colorClear && r[7] > UINT32_MAX - 15))
        {
            if (why)
                *why = "address_overflow";
            return std::nullopt;
        }
        Fetch               fetch{ read, 0, g, clear->sp };
        const std::uint32_t parameters = fetch.word(clear->sp + 100);
        if (parameters > UINT32_MAX - 8)
        {
            if (why)
                *why = "address_overflow";
            return std::nullopt;
        }
        if (out.depthClear)
        {
            out.depthWords    = depthClearWords(fetch, g, parameters, fetch.word(clear->sp + 92), clear->depth);
            fetch.depthStored = true;
        }
        if (out.colorClear)
        {
            // Target shadow G+0x2884+4*(slot==0?0:slot+1) names
            // format(g209:13061-13158). Null r10 uses image default clear;
            // otherwise load aligned 16-byte block containing r10.
            const std::uint32_t          shadow = fetch.word(slot ? g + 0x2888 + 4 * slot : g + 0x2884);
            const std::uint32_t          source = r[7] ? r[7] & ~15U : kResolveClearDefault;
            std::array<std::uint32_t, 4> color{};
            for (std::uint32_t i = 0; i < 4; ++i)
                color[i] = fetch.word(source + 4 * i);
            const std::uint32_t                         kind = (shadow >> 16) & 0xF;
            std::optional<std::array<std::uint32_t, 2>> words;
            if (!parameters)
                words = colorWords(fetch, kind, (shadow >> 20) & 0x3F, color);
            else
            {
                // Beside the target's own format, the block reads the parameter
                // block's: dwords 0 and 1, whose low six bits name the format whose
                // table entry holds the channel layout and width (7 for 54).
                const std::uint32_t format = fetch.word(parameters) & 0x3F, bias = fetch.word(parameters + 4);
                const std::uint32_t entry = fetch.half(kResolveFormatTable + 2 * (format == 54 ? 7 : format));
                const std::uint32_t limit = fetch.byte(kResolveFormatTable + 1 + 2 * fetch.byte(kResolveClearFormatMap + kind));
                words                     = colorWords(fetch, (entry >> 8) & 0xF, bias, color);
                if (words && limit > (entry & 0xFF))
                    (*words)[1] = (*words)[0];
            }
            if (words)
                out.colorWords = *words;
            else
                out.clearRefused = "clear_color_format";
        }
        out.clearOverlap = fetch.overlapped;
        if (fetch.failed && !failed)
            failed = fetch.failed;
    }
    if (failed)
    {
        if (why)
            *why = std::format("unreadable {:08X}", failed);
        return std::nullopt;
    }
    return out;
}

bool ReadResolveWatch(const GuestWordReader& read, std::uint32_t g, std::uint32_t header, ResolveWatch& out)
{
    if (g > UINT32_MAX - kResolveDeviceSpan || header > UINT32_MAX - 16)
        return false;
    std::array<std::uint32_t, 6> dirty{};
    std::uint32_t                state = 0, predicate = 0;
    bool                         ok = true;
    for (std::uint32_t i = 0; i < 4; ++i)
        ok = ok && read(g + 0x2A18 + 4 * i, out.copy[i]);
    for (std::uint32_t i = 0; i < dirty.size(); ++i)
        ok = ok && read(g + 0x10 + 4 * i, dirty[i]);
    ok = ok && read(g + 0x2A44, out.size) && read(g + 0x2ABC, state) && read(g + 0x2A9C, out.fence) && read(header + 8, out.dest8) && read(header + 12, out.dest12);
    ok = ok && read(g + 0x30, out.cursor) && read(g + 0x34, out.limit34) && read(g + 0x38, out.limit38) && read(g + 0x3A44, out.poolPos) && read(g + 0x3A54, out.poolUsed);
    ok = ok && read(g + 0x3A58, out.poolThreshold) && read(g + 0x31CC, out.targetCount) && read(g + 0x34CC, out.poolObject);
    ok = ok && read(g + 0x31AC, predicate) && read(g + 0x33B0, out.bracket) && read(g + 0x3A4C, out.wrapPos);
    for (std::uint32_t i = 0; i < kResolveClearWordCount; ++i)
        ok = ok && read(g + kResolveClearShadow + 4 * i, out.clear[i]);
    // The pool's fixed buffer words, read only through a pool object.
    out.poolBase = out.poolSize = 0;
    if (ok && out.poolObject)
        ok = out.poolObject <= UINT32_MAX - 160 && read(out.poolObject + 152, out.poolBase) && read(out.poolObject + 156, out.poolSize);
    // The null pool allocator's words, read only without a pool object. 0x8269B158
    // loads the progress word through the block pointer G+0x2A90; a null block
    // is not a word the host can tell.
    out.sideEnd = out.wrapLimit = out.segment = out.bracketNext = out.progress = 0;
    if (ok && !out.poolObject)
    {
        std::uint32_t block = 0;
        ok                  = read(g + 0x3A3C, out.sideEnd) && read(g + 0x3A50, out.wrapLimit) && read(g + 0x3A48, out.segment) && read(g + 0x33B4, out.bracketNext) &&
                              read(g + 0x2A90, block) && block != 0 && block <= UINT32_MAX - 4 && read(block + 4, out.progress);
    }
    if (!ok)
        return false;
    for (std::size_t i = 0; i < kResolveDirtyWords; ++i)
        out.dirty[i] = (std::uint64_t{ dirty[2 * i] } << 32) | dirty[2 * i + 1];
    out.allocByte  = static_cast<std::uint8_t>(state >> 16);
    out.stateByte  = static_cast<std::uint8_t>(state >> 8);
    out.tilingByte = static_cast<std::uint8_t>(state >> 24);
    out.predicate  = predicate;
    return true;
}

namespace
{

// The largest render target count the loops of the body are bounded by here,
// and the side buffer 0x8269B818 takes: six dwords.
constexpr std::uint32_t kMaxTargetCount  = kRingMaxTargetCount;
constexpr std::uint32_t kSideBufferBytes = kRingResolveSideBufferBytes;

} // namespace

std::uint64_t ResolveMaxRingWords(const ResolveWatch& entry, std::uint64_t clearDirty) noexcept
{
    if (entry.targetCount > kMaxTargetCount)
        return UINT64_MAX;
    const std::uint64_t n     = entry.targetCount;
    std::uint64_t       words = 2 * RingWordsAc8c8(n)   // 0x826AC8C8 on the G+0x10 group and on 256
                                + 2 * RingWords8d3a0(n) // 0x8268D3A0 twice
                                + (6 + 5 * n)           // the render target loop
                                + 85;                   // the straight-line emits
    // Flush sees entry dirty bits(0x826AC8C8 only clears), plus G+0x20 OR
    // 0x3C00, clear-block bits, and large-form 4(g167:13476). Include4
    // unconditionally for a conservative upper bound.
    const std::uint64_t d1 = entry.dirty[0], d2 = entry.dirty[1], d3 = entry.dirty[2] | 0x3C04 | clearDirty;
    words += RingFlushGroupWords(d1, d2, d3);
    return words;
}

ResolveNullPoolAlloc ModelNullPoolAllocator(const ResolveWatch& entry, std::uint32_t bytes, std::uint32_t alignment) noexcept
{
    ResolveNullPoolAlloc out;
    // 0x8269B618 with r4 = 1 and r6 = the alignment: the buffer is the next
    // multiple of the alignment at or above the start (g185:12921-12950).
    const std::uint32_t mask    = ~(alignment - 1);
    const std::uint32_t start   = entry.poolPos ? entry.poolPos : entry.limit34 + 60;
    std::uint32_t       buffer  = (start + alignment - 1) & mask;
    std::uint32_t       end     = buffer + bytes;
    std::uint32_t       segment = entry.segment;
    // The bracket's check: the segment the tile group waits on against the
    // segment this take would leave behind (g185:12956-12976, 13005-13021).
    const auto bracketFails = [&](std::uint32_t difference, std::uint32_t take)
    {
        return entry.bracket != 0 && (difference == UINT32_MAX || (difference == 0 && entry.bracket < take));
    };
    if (end <= entry.sideEnd)
    {
        if (bracketFails(entry.bracketNext - entry.segment, end))
            return out;
    }
    else
    {
        buffer = (entry.wrapPos + alignment - 1) & mask;
        end    = buffer + bytes;
        if (bracketFails(entry.bracketNext - entry.segment - 1, end) || end > entry.wrapLimit)
            return out;
        out.wrap = true;
        segment += 1;
    }
    if (!buffer)
        return out;
    out.ok  = true;
    out.end = end;
    // 0x8269B158(G, end, segment) returns at once for a segment equal to the
    // GPU's, or one ahead with the buffer end inside its block (g206:12440-12459).
    const std::uint32_t ahead = (segment - entry.progress) & 3;
    out.waits                 = !(ahead == 0 || (ahead == 1 && end <= (entry.progress & ~3U)));
    return out;
}

std::optional<ResolveExitState> DeriveResolveExitState(const ResolveEffects& effects, const ResolveWatch& entry, ResolveExitRefusal* refusal, std::string* why)
{
    const auto refuse = [&](ResolveExitRefusal reason, const char* text) -> std::optional<ResolveExitState>
    {
        if (refusal)
            *refusal = reason;
        if (why)
            *why = text;
        return std::nullopt;
    };
    if (refusal)
        *refusal = ResolveExitRefusal::None;
    if (!entry.fence)
        return refuse(ResolveExitRefusal::Fence, "fence_zero");
    if (entry.allocByte & kResolveAllocFailedBit)
        return refuse(ResolveExitRefusal::AllocMarker, "alloc_marker");
    // The bracket form adds only ring words, which the bound below counts, and the
    // allocator's bracket terms, and the predicate of its per-tile calls only an
    // OR into a ring word; every other tiled shape is left to a census.
    switch (entry.Tiling())
    {
        case ResolveTiling::None:
        case ResolveTiling::Bracket:
        case ResolveTiling::PredicatedBracket:
            break;
        case ResolveTiling::Byte:
            return refuse(ResolveExitRefusal::Tiling, "tiling_byte");
        case ResolveTiling::Predicate:
            return refuse(ResolveExitRefusal::TilingPredicate, "tiling_predicate");
        case ResolveTiling::State:
            return refuse(ResolveExitRefusal::TilingState, "tiling_state");
    }

    // The clear blocks run in every tiling form. An input the body overwrites before
    // them, or a color whose words the host does not derive, leaves the shadow
    // words unknown.
    if (effects.clearOverlap)
        return refuse(ResolveExitRefusal::ClearOverlap, "clear_overlap");
    if (effects.colorClear && effects.clearRefused)
        return refuse(ResolveExitRefusal::ClearColor, effects.clearRefused);

    // A pool object without a fixed buffer is the indirect allocator 0x826A6AB8,
    // whose success the host cannot tell.
    if (entry.poolObject && !entry.poolBase)
        return refuse(ResolveExitRefusal::AllocPath, "alloc_path");
    const std::uint64_t words = ResolveMaxRingWords(entry, (effects.depthClear ? 0x300 : 0) | (effects.colorClear ? 0xC0 : 0));
    const std::uint64_t end   = words == UINT64_MAX ? UINT64_MAX : std::uint64_t{ entry.cursor } + 4 * words;
    if (end > UINT32_MAX - 164)
        return refuse(ResolveExitRefusal::RingSpace, "ring_space");
    // 0x8269B818 adds 24 to G+0x3A54 on every path (g93:12826-12839). Past the
    // threshold with the bracket clear the next ring check flushes (g93:12840-12865).
    const std::uint32_t used    = entry.poolUsed + kSideBufferBytes;
    std::uint32_t       limit34 = entry.limit34, limit38 = entry.limit38, poolPos = entry.poolPos, wrapPos = entry.wrapPos;
    if (entry.poolObject)
    {
        // 0x826A6848 takes 24 bytes below buffer top; ring limits are checked
        // first (g100:13237-13283).
        const std::uint32_t top    = entry.poolPos ? entry.poolPos : (entry.poolSize & ~3U) + entry.poolBase;
        const std::uint32_t buffer = (top - kSideBufferBytes) & ~3U;
        const std::uint32_t delta  = (top - buffer) & ~3U;
        if (entry.limit38 < delta || entry.limit34 < delta)
            return refuse(ResolveExitRefusal::RingSpace, "ring_limit");
        limit34 = entry.limit34 - delta;
        limit38 = entry.limit38 - delta;
        if (end > limit38 || end >= limit34)
            return refuse(ResolveExitRefusal::RingSpace, "ring_space");
        if (used > entry.poolThreshold && entry.bracket == 0)
            return refuse(ResolveExitRefusal::RingSpace, "pool_threshold");
        // The take fails below the cursor plus 164, and the cursor is no lower than
        // the end of the words.
        if (buffer < end + 164)
            return refuse(ResolveExitRefusal::AllocCapacity, "alloc_capacity");
        poolPos = buffer;
    }
    else
    {
        // 0x8269B618 takes the buffer from the side buffer area above G+0x34 and
        // moves neither limit, so the ring words are judged against the limits as
        // they stand.
        if (end > limit38 || end >= limit34)
            return refuse(ResolveExitRefusal::RingSpace, "ring_space");
        if (used > entry.poolThreshold && entry.bracket == 0)
            return refuse(ResolveExitRefusal::RingSpace, "pool_threshold");
        const auto take = ModelNullPoolAllocator(entry);
        if (!take.ok)
            return refuse(ResolveExitRefusal::AllocCapacity, "alloc_fail");
        if (take.waits)
            return refuse(ResolveExitRefusal::AllocWait, "alloc_wait");
        (take.wrap ? wrapPos : poolPos) = take.end;
    }

    ResolveExitState out;
    out.copy = { 0, effects.words.destBase, effects.words.destPitch, 0 };
    out.size = effects.large ? effects.pitchHeight : entry.size;
    for (std::size_t i = 0; i < kResolveDirtyWords; ++i)
        out.dirty[i] = (entry.dirty[i] & ~kResolveFlushClear[i]) | effects.setBeforeAlloc[i] | effects.setAfterAlloc[i];
    out.allocByte = static_cast<std::uint8_t>(entry.allocByte | (effects.depthClear ? kResolveDepthClearBit : 0));
    out.stateByte = static_cast<std::uint8_t>(entry.stateByte & ~kResolveStateClearedBit);
    out.clear     = entry.clear;
    if (effects.depthClear)
        std::copy(effects.depthWords.begin(), effects.depthWords.end(), out.clear.begin());
    if (effects.colorClear)
        std::copy(effects.colorWords.begin(), effects.colorWords.end(), out.clear.begin() + 2);
    out.dest8 = out.dest12 = entry.fence;
    out.limit34            = limit34;
    out.limit38            = limit38;
    out.poolPos            = poolPos;
    out.poolUsed           = used;
    out.wrapPos            = wrapPos;
    return out;
}

std::optional<ResolveExitState> DeriveResolveExitState(const GuestWordReader& read, const std::array<std::uint32_t, 8>& r, std::string* why, const ResolveClearArgs* clear)
{
    const auto effects = DeriveResolveEffects(read, r, why, clear);
    if (!effects)
        return std::nullopt;
    ResolveWatch watch;
    if (!ReadResolveWatch(read, r[0], r[3], watch))
    {
        if (why)
            *why = "unreadable watched words";
        return std::nullopt;
    }
    return DeriveResolveExitState(*effects, watch, nullptr, why);
}

#if REREVVED_DEVELOPER
unsigned ResolveExitMismatches(const ResolveExitState& predicted, const ResolveWatch& exit) noexcept
{
    unsigned mask = 0;
    for (std::size_t i = 0; i < predicted.copy.size(); ++i)
        mask |= predicted.copy[i] != exit.copy[i] ? 1U << i : 0U;
    mask |= predicted.size != exit.size ? 1U << 4 : 0U;
    for (std::size_t i = 0; i < kResolveDirtyWords; ++i)
        mask |= predicted.dirty[i] != exit.dirty[i] ? 1U << (5 + i) : 0U;
    mask |= predicted.allocByte != exit.allocByte ? 1U << 8 : 0U;
    mask |= predicted.stateByte != exit.stateByte ? 1U << 9 : 0U;
    mask |= predicted.dest8 != exit.dest8 ? 1U << 10 : 0U;
    mask |= predicted.dest12 != exit.dest12 ? 1U << 11 : 0U;
    mask |= predicted.limit34 != exit.limit34 ? 1U << 12 : 0U;
    mask |= predicted.limit38 != exit.limit38 ? 1U << 13 : 0U;
    mask |= predicted.poolPos != exit.poolPos ? 1U << 14 : 0U;
    mask |= predicted.poolUsed != exit.poolUsed ? 1U << 15 : 0U;
    mask |= predicted.wrapPos != exit.wrapPos ? 1U << 16 : 0U;
    for (std::size_t i = 0; i < kResolveClearWordCount; ++i)
        mask |= predicted.clear[i] != exit.clear[i] ? 1U << (17 + i) : 0U;
    return mask;
}
#endif

std::vector<ResolveWrite> ResolveExitWrites(std::uint32_t g, std::uint32_t header, const ResolveWatch& entry, const ResolveExitState& exit)
{
    std::vector<ResolveWrite> writes;
    const auto                word = [&](std::uint32_t address, std::uint32_t before, std::uint32_t after)
    {
        if (before != after)
            writes.push_back({ address, after, false });
    };
    const auto byte = [&](std::uint32_t address, std::uint8_t before, std::uint8_t after)
    {
        if (before != after)
            writes.push_back({ address, after, true });
    };
    for (std::uint32_t i = 0; i < exit.copy.size(); ++i)
        word(g + 0x2A18 + 4 * i, entry.copy[i], exit.copy[i]);
    word(g + 0x2A44, entry.size, exit.size);
    for (std::uint32_t i = 0; i < kResolveDirtyWords; ++i)
    {
        word(g + 0x10 + 8 * i, static_cast<std::uint32_t>(entry.dirty[i] >> 32), static_cast<std::uint32_t>(exit.dirty[i] >> 32));
        word(g + 0x14 + 8 * i, static_cast<std::uint32_t>(entry.dirty[i]), static_cast<std::uint32_t>(exit.dirty[i]));
    }
    byte(g + 0x2ABD, entry.allocByte, exit.allocByte);
    byte(g + 0x2ABE, entry.stateByte, exit.stateByte);
    word(header + 8, entry.dest8, exit.dest8);
    word(header + 12, entry.dest12, exit.dest12);
    word(g + 0x34, entry.limit34, exit.limit34);
    word(g + 0x38, entry.limit38, exit.limit38);
    word(g + 0x3A44, entry.poolPos, exit.poolPos);
    word(g + 0x3A54, entry.poolUsed, exit.poolUsed);
    word(g + 0x3A4C, entry.wrapPos, exit.wrapPos);
    for (std::uint32_t i = 0; i < kResolveClearWordCount; ++i)
        word(g + kResolveClearShadow + 4 * i, entry.clear[i], exit.clear[i]);
    return writes;
}

bool ResolveStackOverlaps(const GuestWordReader& read, const StackFrame& frame, const std::array<std::uint32_t, 8>& r)
{
    const std::uint32_t g = r[0];
    StackReach          reach;
    if (StackReachRefuses(read, frame, kResolveStackFootprint, g, reach))
        return true;
    const std::uint64_t device = g;
    const std::uint32_t slot   = r[1] & 7;
    if (reach.Overlaps(r[3], r[3] ? 52 : 0) || reach.Overlaps(r[2], r[2] ? 8 : 0) || reach.Overlaps(r[4] ? r[4] : kResolveDefaultPoint, 8) ||
        reach.Overlaps(r[7] & ~15U, r[7] ? 16 : 0) || reach.OverlapsAny(kResolveStackStatics))
        return true;
    return StackReachesPointee(read, reach, device + (slot == 4 ? 0x30A8 : 0x3098 + 4 * slot), 0x18, 4) ||
           StackReachesPointee(read, reach, std::uint64_t{ frame.sp } + 100, 0, 12) || StackReachesAllocator(read, reach, g);
}

#if REREVVED_DEVELOPER
ResolveEffectResult CompareResolveEffects(const ResolveEffects& predicted, const ResolveWatch& entry, const ResolveWatch& exit) noexcept
{
    ResolveEffectResult result;
    const auto          flag = [&](ResolveEffectField field)
    {
        result.mismatch |= 1U << static_cast<unsigned>(field);
    };
    // Marker:body 0x8269B818 or later list 0x826A6958(g52:13432). Entry marker
    // returns at once; new marker uses exit stores(g167:15084,
    // 15086):successful body zeroes G+0x2A18, copy control never zero.
    const bool entryMarker = (entry.allocByte & kResolveAllocFailedBit) != 0;
    result.allocFailed     = (exit.allocByte & kResolveAllocFailedBit) != 0;
    result.mainFailed      = entryMarker || (result.allocFailed && exit.copy[0] != 0);

    // The exit stores zero G+0x2A18 and G+0x2A24 (g167:15083-15086); a failed
    // allocation leaves the copy words in them.
    const ResolveCopyWords& words = predicted.words;
    result.expectCopy             = result.mainFailed ? std::array<std::uint32_t, 4>{ words.copyControl, words.destBase, words.destPitch, words.destInfo }
                                                      : std::array<std::uint32_t, 4>{ 0, words.destBase, words.destPitch, 0 };
    result.expectSize             = predicted.large ? predicted.pitchHeight : entry.size;
    if (exit.copy != result.expectCopy)
        flag(ResolveEffectField::CopyWords);
    if (exit.size != result.expectSize)
        flag(ResolveEffectField::Size);

    // Dirty bits: the body ORs, and the flush groups clear, so a clear bit is
    // never a mismatch and only a predicted bit that is clear, or a new bit
    // nobody predicted, is.
    for (std::size_t i = 0; i < kResolveDirtyWords; ++i)
    {
        const std::uint64_t want = predicted.setBeforeAlloc[i] | (result.mainFailed ? 0 : predicted.setAfterAlloc[i]);
        result.missing[i]        = want & ~exit.dirty[i];
        result.extra[i]          = exit.dirty[i] & ~entry.dirty[i] & ~want;
        result.clearedBits += static_cast<unsigned>(std::popcount(entry.dirty[i] & ~exit.dirty[i]));
        if (result.missing[i])
            flag(ResolveEffectField::DirtySetMissing);
        if (result.extra[i])
            flag(ResolveEffectField::DirtyExtra);
    }
    result.cleared1E0000 = (entry.dirty[0] & ~exit.dirty[0] & kResolveProtectedDirty) != 0;

    if ((exit.stateByte & kResolveStateClearedBit) || (predicted.depthClear && !(exit.allocByte & kResolveDepthClearBit)))
        flag(ResolveEffectField::Bytes);

    // Resolve header fact table:
    // Read path: the body reads the fence at g167:14916, stores it to +8
    //   (14928), waits for ring space at 14989, and g12:12903-12905 advances
    //   the fence by two; it reloads at 15104.
    // Selection: only reload 15104 selects the path; list-path allocation may
    //   move the return fence afterward.
    // Sequence: S contains the fence values from entry to return in steps of
    //   two, wrapping through zero.
    // Nonzero reload: 15104 stores the reload to +8 and +12 at 15112 and 15114,
    //   then goes straight to return hook 15115/0x8269F360 with no intervening
    //   call; both equal the nonzero return fence.
    // Zero reload: 15104 branches to loc_8269F310 at 15117; that path stores no
    //   +12, has zero in S, +8 holds the earlier read at an S value at or
    //   before zero, and +12 as at entry.
    if (!result.mainFailed)
    {
        const std::uint32_t span = exit.fence - entry.fence;
        const auto          inS  = [&](std::uint32_t value, std::uint32_t limit)
        {
            const std::uint32_t step = value - entry.fence;
            return step <= limit && !(step & 1);
        };
        const std::uint32_t zeroStep = 0U - entry.fence;
        const bool          header   = exit.fence != 0 && exit.dest8 == exit.fence && exit.dest12 == exit.fence;
        const bool          list     = inS(0, span) && inS(exit.dest8, zeroStep) && exit.dest12 == entry.dest12;
        result.listPath              = !header && list;
        if (!header && !list)
            flag(ResolveEffectField::Dest);
    }

    // The exact exit the consume writes, predicted from the entry state alone
    // (the fence included), for every call it models: a marker, a list path or a
    // moved fence at the return are then mismatches of the prediction.
    result.shape.predicated = entry.Tiling() == ResolveTiling::PredicatedBracket;
    result.shape.clear      = predicted.depthClear || predicted.colorClear;
    result.shape.tiles      = entry.targetCount;
    if (const auto exact = DeriveResolveExitState(predicted, entry, &result.exactRefusal))
    {
        result.exactModeled    = true;
        result.exactTiled      = ResolveBracketForm(entry.Tiling());
        result.exactPredicated = result.shape.predicated;
        result.exactClear      = predicted.depthClear || predicted.colorClear;
        result.exact           = *exact;
        result.exactMismatch   = ResolveExitMismatches(*exact, exit);
    }
    return result;
}

bool ResolveEffectCheckRequested() noexcept
{
    return REXCVAR_GET(native_resolve_effect_check);
}
#endif

unsigned ResolveProbeStack::Push(std::uint32_t entrySp, ResolveProbe probe, ResolveDropCounts* drops)
{
    unsigned          dropped = 0;
    ResolveDropCounts counts;
    // A live outer call's frame is at or above the entry r1 of any call it
    // makes (a tail-jumping callback enters with exactly its frame).
    while (count_ && slots_[count_ - 1].frame < entrySp)
    {
        --count_;
        ++dropped;
        counts.derive += slots_[count_].deriveCopy ? 1U : 0U;
        counts.effects += slots_[count_].effects ? 1U : 0U;
    }
    probe.frame = entrySp - kResolveFrameSize;
    if (count_ == kDepth)
    {
        ++dropped;
        counts.derive += probe.deriveCopy ? 1U : 0U;
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

ResolveProbe* ResolveProbeStack::Find(std::uint32_t frame, unsigned& dropped, ResolveDropCounts* drops)
{
    dropped = 0;
    for (std::size_t i = count_; i-- > 0;)
        if (slots_[i].frame == frame)
        {
            dropped = static_cast<unsigned>(count_ - 1 - i);
            if (drops)
                for (std::size_t above = i + 1; above < count_; ++above)
                {
                    drops->derive += slots_[above].deriveCopy ? 1U : 0U;
                    drops->effects += slots_[above].effects ? 1U : 0U;
                }
            count_ = i + 1;
            return &slots_[i];
        }
    return nullptr;
}

std::optional<ResolveProbe> ResolveProbeStack::Pop(std::uint32_t frame, unsigned& dropped, ResolveDropCounts* drops)
{
    auto* found = Find(frame, dropped, drops);
    if (!found)
        return std::nullopt;
    --count_;
    return std::move(*found);
}

#if REREVVED_DEVELOPER
unsigned ResolveWordMismatches(const ResolveCopyWords& derived, const ResolveCopyWords& observed) noexcept
{
    return (derived.copyControl != observed.copyControl ? 1U : 0U) | (derived.destBase != observed.destBase ? 2U : 0U) |
           (derived.destPitch != observed.destPitch ? 4U : 0U) | (derived.destInfo != observed.destInfo ? 8U : 0U);
}

bool ResolveDeriveCheckRequested() noexcept
{
    return REXCVAR_GET(native_resolve_derive_check);
}

unsigned NoteResolveDerive(const std::optional<ResolveCopyWords>& derived, const ResolveCopyWords& observed) noexcept
{
    if (!derived)
    {
        counters.unsupported.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const auto mask = ResolveWordMismatches(*derived, observed);
    if (!mask)
        counters.ok.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kResolveWordCount; ++i)
        if (mask & (1U << i))
            counters.mismatch[i].fetch_add(1, std::memory_order_relaxed);
    return mask;
}

void NoteResolveUnpaired(unsigned count) noexcept
{
    if (count)
        counters.unpaired.fetch_add(count, std::memory_order_relaxed);
}

void NoteResolveClearWords(bool stale) noexcept
{
    counters.clearChecked.fetch_add(1, std::memory_order_relaxed);
    if (stale)
        counters.clearStale.fetch_add(1, std::memory_order_relaxed);
}

ResolveDeriveCounts TakeResolveDeriveCounts() noexcept
{
    ResolveDeriveCounts taken;
    taken.ok           = counters.ok.exchange(0, std::memory_order_relaxed);
    taken.unsupported  = counters.unsupported.exchange(0, std::memory_order_relaxed);
    taken.unpaired     = counters.unpaired.exchange(0, std::memory_order_relaxed);
    taken.clearChecked = counters.clearChecked.exchange(0, std::memory_order_relaxed);
    taken.clearStale   = counters.clearStale.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kResolveWordCount; ++i)
        taken.mismatch[i] = counters.mismatch[i].exchange(0, std::memory_order_relaxed);
    return taken;
}

void NoteResolveEffect(const ResolveEffectResult& result) noexcept
{
    if (!result.mismatch)
        effectCounters.ok.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kResolveEffectFieldCount; ++i)
        if (result.mismatch & (1U << i))
            effectCounters.mismatch[i].fetch_add(1, std::memory_order_relaxed);
    effectCounters.dirtyCleared.fetch_add(result.clearedBits, std::memory_order_relaxed);
    if (result.cleared1E0000)
        effectCounters.cleared1E0000.fetch_add(1, std::memory_order_relaxed);
    if (result.allocFailed)
        effectCounters.allocFailed.fetch_add(1, std::memory_order_relaxed);
    if (result.listPath)
        effectCounters.listPath.fetch_add(1, std::memory_order_relaxed);
    if (result.shape.predicated)
    {
        effectCounters.predicated.fetch_add(1, std::memory_order_relaxed);
        if (result.shape.clear)
            effectCounters.predicatedClear.fetch_add(1, std::memory_order_relaxed);
        effectCounters.predicatedTiles[ResolveTileBucket(result.shape.tiles)].fetch_add(1, std::memory_order_relaxed);
    }
    if (!result.exactModeled)
    {
        effectCounters.exactSkipped.fetch_add(1, std::memory_order_relaxed);
        effectCounters.exactRefused[static_cast<std::size_t>(result.exactRefusal)].fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        if (!result.exactMismatch)
            effectCounters.exactOk.fetch_add(1, std::memory_order_relaxed);
        if (result.exactTiled)
        {
            effectCounters.exactTiled.fetch_add(1, std::memory_order_relaxed);
            if (!result.exactMismatch)
                effectCounters.exactTiledOk.fetch_add(1, std::memory_order_relaxed);
        }
        if (result.exactPredicated)
        {
            effectCounters.exactPredicated.fetch_add(1, std::memory_order_relaxed);
            if (!result.exactMismatch)
                effectCounters.exactPredicatedOk.fetch_add(1, std::memory_order_relaxed);
        }
        if (result.exactClear)
        {
            effectCounters.exactClear.fetch_add(1, std::memory_order_relaxed);
            if (!result.exactMismatch)
                effectCounters.exactClearOk.fetch_add(1, std::memory_order_relaxed);
        }
    }
    for (std::size_t i = 0; i < kResolveExitWordCount; ++i)
        if (result.exactMismatch & (1U << i))
            effectCounters.exactMismatch[i].fetch_add(1, std::memory_order_relaxed);
}

void NoteResolveEffectUnsupported() noexcept
{
    effectCounters.unsupported.fetch_add(1, std::memory_order_relaxed);
    effectCounters.exactSkipped.fetch_add(1, std::memory_order_relaxed);
}

void NoteResolveEffectStackOverlap() noexcept
{
    effectCounters.exactSkipped.fetch_add(1, std::memory_order_relaxed);
    effectCounters.exactRefused[static_cast<std::size_t>(ResolveExitRefusal::StackOverlap)].fetch_add(1, std::memory_order_relaxed);
}

void NoteResolveEffectUnpaired(unsigned count) noexcept
{
    if (count)
        effectCounters.unpaired.fetch_add(count, std::memory_order_relaxed);
}

ResolveEffectCounts TakeResolveEffectCounts() noexcept
{
    ResolveEffectCounts taken;
    taken.ok            = effectCounters.ok.exchange(0, std::memory_order_relaxed);
    taken.unsupported   = effectCounters.unsupported.exchange(0, std::memory_order_relaxed);
    taken.unpaired      = effectCounters.unpaired.exchange(0, std::memory_order_relaxed);
    taken.dirtyCleared  = effectCounters.dirtyCleared.exchange(0, std::memory_order_relaxed);
    taken.cleared1E0000 = effectCounters.cleared1E0000.exchange(0, std::memory_order_relaxed);
    taken.allocFailed   = effectCounters.allocFailed.exchange(0, std::memory_order_relaxed);
    taken.listPath      = effectCounters.listPath.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kResolveEffectFieldCount; ++i)
        taken.mismatch[i] = effectCounters.mismatch[i].exchange(0, std::memory_order_relaxed);
    taken.exactOk               = effectCounters.exactOk.exchange(0, std::memory_order_relaxed);
    taken.exactSkipped          = effectCounters.exactSkipped.exchange(0, std::memory_order_relaxed);
    taken.exactTiled            = effectCounters.exactTiled.exchange(0, std::memory_order_relaxed);
    taken.exactTiledOk          = effectCounters.exactTiledOk.exchange(0, std::memory_order_relaxed);
    taken.exactPredicated       = effectCounters.exactPredicated.exchange(0, std::memory_order_relaxed);
    taken.exactPredicatedOk     = effectCounters.exactPredicatedOk.exchange(0, std::memory_order_relaxed);
    taken.shape.predicated      = effectCounters.predicated.exchange(0, std::memory_order_relaxed);
    taken.shape.predicatedClear = effectCounters.predicatedClear.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kResolveTileBuckets; ++i)
        taken.shape.tiles[i] = effectCounters.predicatedTiles[i].exchange(0, std::memory_order_relaxed);
    taken.exactClear   = effectCounters.exactClear.exchange(0, std::memory_order_relaxed);
    taken.exactClearOk = effectCounters.exactClearOk.exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kResolveExitWordCount; ++i)
        taken.exactMismatch[i] = effectCounters.exactMismatch[i].exchange(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kResolveExitRefusalCount; ++i)
        taken.exactRefused[i] = effectCounters.exactRefused[i].exchange(0, std::memory_order_relaxed);
    return taken;
}

std::string FormatResolveShapeCounts(const ResolveShapeCounts& counts, const char* prefix)
{
    constexpr std::array<const char*, kResolveTileBuckets> names{ "2", "3", "4", "other" };
    std::string                                            fields = std::format(" {0}predicated={1} {0}predicated_clear={2}", prefix, counts.predicated, counts.predicatedClear);
    for (std::size_t i = 0; i < kResolveTileBuckets; ++i)
        fields += std::format(" {}predicated_tiles_{}={}", prefix, names[i], counts.tiles[i]);
    return fields;
}

std::string FormatResolveEffectCounts(const ResolveEffectCounts& counts)
{
    std::string fields = std::format(" resolve_effect_ok={}", counts.ok);
    for (std::size_t i = 0; i < kResolveEffectFieldCount; ++i)
        fields += std::format(" resolve_effect_mismatch_{}={}", kResolveEffectFieldNames[i], counts.mismatch[i]);
    fields += std::format(" resolve_effect_dirty_cleared={} resolve_effect_cleared_1e0000={} resolve_effect_alloc_failed={} resolve_effect_list_path={} resolve_effect_unsupported={} resolve_effect_unpaired={}",
                          counts.dirtyCleared,
                          counts.cleared1E0000,
                          counts.allocFailed,
                          counts.listPath,
                          counts.unsupported,
                          counts.unpaired);
    fields += std::format(" resolve_effect_exact_ok={} resolve_effect_exact_tiled={} resolve_effect_exact_tiled_ok={} resolve_effect_exact_predicated={} resolve_effect_exact_predicated_ok={} resolve_effect_exact_clear={} resolve_effect_exact_clear_ok={}",
                          counts.exactOk,
                          counts.exactTiled,
                          counts.exactTiledOk,
                          counts.exactPredicated,
                          counts.exactPredicatedOk,
                          counts.exactClear,
                          counts.exactClearOk);
    fields += FormatResolveShapeCounts(counts.shape, "resolve_effect_");
    for (std::size_t i = 0; i < kResolveExitWordCount; ++i)
        fields += std::format(" resolve_effect_exact_mismatch_{}={}", kResolveExitWordNames[i], counts.exactMismatch[i]);
    fields += std::format(" resolve_effect_exact_skipped={}", counts.exactSkipped);
    for (std::size_t i = 1; i < kResolveExitRefusalCount; ++i)
        fields += std::format(" resolve_effect_exact_skipped_{}={}", kResolveExitRefusalNames[i], counts.exactRefused[i]);
    return fields;
}

std::string FormatResolveDeriveCounts(const ResolveDeriveCounts& counts)
{
    std::string fields = std::format(" resolve_derive_ok={}", counts.ok);
    for (std::size_t i = 0; i < kResolveWordCount; ++i)
        fields += std::format(" resolve_derive_mismatch_{}={}", kResolveWordNames[i], counts.mismatch[i]);
    fields += std::format(" resolve_derive_unsupported={} resolve_derive_unpaired={} resolve_clear_words_checked={} resolve_clear_words_stale={}", counts.unsupported, counts.unpaired, counts.clearChecked, counts.clearStale);
    return fields;
}
#endif

} // namespace rerevved::gpu::guest_d3d
