#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rerevved::gpu::guest_d3d
{

// A consume is exact only when Original's stack saves cannot overwrite any
//   modeled input/output byte.
// Guest field/domain           | Fact or required admission proof
// r13                          | Current thread's KPCR; title generated code
//   writes it only in setjmp restore, with the same thread value.
// KPCR+0x70 stack_base_ptr     | Highest stack address.
// KPCR+0x74 stack_end_ptr      | Lowest address/limit; hardware guard pages lie
//   below it.
// KPCR creation source         | rerevved-sdk include/rex/system/xthread.h
//   X_KPCR; src/system/xthread.cpp.
// Fiber-switch bounds source   | src/kernel/crt/threading.cpp
//   UpdateGuestStackPointers.
// Limit reader                 | rex::ppc::stack_limit_from_pcr in
//   include/rex/ppc/stack.h reads the same word.
// Skipped-write reach          | [limit, r1 + above), including Original
//   prologue saves and nested frames; no modeled device/object byte may
//   overlap.
// Entry frame check            | Refuse fewer than `frame` bytes below r1;
//   footprint uses the generated entry stwu, cited per consume.
// Overflow domain              | Original does not overflow; hardware would
//   fault. SDK fiber stacks (kernel crt threading.cpp) lack guards and rely on
//   this property.
// Nested frame overflow        | Outside the model; only the static entry frame
//   is checked.
// Caller parameter slots       | r1+84 stride, r1+92/r1+100 clear parameters:
//   interface within reach; pointed objects still checked.
// Check order                  | Stack range/device words before the first
//   modeled guest read; each pointed object immediately after its tested
//   pointer word.
// Unknown bounds refuse        | r13 zero, unreadable KPCR, zero/inverted
//   range, r1 outside range, insufficient `frame`, or KPCR inside reach.
// Aliasing consequence         | Back-chain/register/LR saves or nested frames
//   could change modeled words before Original reads them.

// KPCR offsets of the thread's stack bounds, and the bytes of the KPCR kept clear of the reach.
inline constexpr std::uint32_t kPcrStackBaseOffset  = 0x70;
inline constexpr std::uint32_t kPcrStackLimitOffset = 0x74;
inline constexpr std::uint32_t kPcrStackBytes       = 0x80;

// The registers a consume reads its stack from: r13 and the entry r1.
struct StackFrame
{
    std::uint32_t pcr = 0;
    std::uint32_t sp  = 0;
};

// Footprint:above=caller parameters at/above entry r1; deviceSpan=[G, G+size)
// model bytes; frame=own prologue stwu/saves below r1. Fewer frame bytes to
// stack limit leaves no-overflow model and refuses.
struct StackFootprint
{
    std::uint32_t above      = 0;
    std::uint32_t deviceSpan = 0;
    std::uint32_t frame      = 0;
};

// A fixed object of the title image the model reads.
struct StackObject
{
    std::uint32_t address = 0;
    std::uint32_t bytes   = 0;
};

// The half-open range [limit, r1 + above) a skipped Original can touch, or unknown.
class StackReach
{
public:
    constexpr StackReach() noexcept = default;

    // The reach for the range [limit, base) of the thread and its entry r1 `sp`. Unknown when the range is empty or
    // inverted, `sp` is outside [limit, base], or fewer than `frame` bytes lie between the limit and `sp` (the
    // entry function's own prologue would store below the limit).
    static constexpr StackReach Between(std::uint32_t limit, std::uint32_t base, std::uint32_t sp, std::uint32_t above, std::uint32_t frame = 0) noexcept
    {
        StackReach reach;
        if (limit == 0 || limit >= base || sp < limit || sp > base || sp - limit < frame)
            return reach;
        reach.low_   = limit;
        reach.high_  = std::uint64_t{ sp } + above;
        reach.known_ = true;
        return reach;
    }

    // The reach of the thread whose KPCR is at `pcr`: `read(address, value)` reads a big-endian guest word and returns
    // false when it cannot. Unknown when the KPCR cannot be read or lies in the reach.
    template <class Read>
    static StackReach Of(const Read& read, std::uint32_t pcr, std::uint32_t sp, std::uint32_t above, std::uint32_t frame = 0)
    {
        if (pcr == 0 || pcr > 0xFFFFFFFFU - kPcrStackBytes)
            return {};
        std::uint32_t base = 0, limit = 0;
        if (!read(pcr + kPcrStackBaseOffset, base) || !read(pcr + kPcrStackLimitOffset, limit))
            return {};
        const auto reach = Between(limit, base, sp, above, frame);
        if (!reach.Known() || reach.Overlaps(pcr, kPcrStackBytes))
            return {};
        return reach;
    }

    constexpr bool Known() const noexcept
    {
        return known_;
    }

    // True when [address, address + bytes) shares a byte with the reach, when the object cannot be placed (it runs
    // past 2^32), or when the reach is unknown. An empty object, and an empty reach (a footprint with no frame, only in tests), overlap nothing. Computed in
    // 64 bits; ranges that touch do not overlap.
    constexpr bool Overlaps(std::uint64_t address, std::uint64_t bytes) const noexcept
    {
        if (bytes == 0)
            return false;
        if (!known_ || address + bytes > (std::uint64_t{ 1 } << 32))
            return true;
        return low_ < high_ && address < high_ && low_ < address + bytes;
    }

    constexpr bool Overlaps(const StackObject& object) const noexcept
    {
        return Overlaps(object.address, object.bytes);
    }

    template <std::size_t N>
    constexpr bool OverlapsAny(const std::array<StackObject, N>& objects) const noexcept
    {
        for (const auto& object : objects)
            if (Overlaps(object))
                return true;
        return false;
    }

    constexpr std::uint64_t Low() const noexcept
    {
        return low_;
    }

    constexpr std::uint64_t High() const noexcept
    {
        return high_;
    }

private:
    std::uint64_t low_   = 0;
    std::uint64_t high_  = 0;
    bool          known_ = false;
};

// The reach of a consume's frame, tested against the device block of `g` and its footprint, before any guest word of
// the model is read. True means refuse.
template <class Read>
bool StackReachRefuses(const Read& read, const StackFrame& frame, const StackFootprint& footprint, std::uint32_t g, StackReach& reach)
{
    reach = StackReach::Of(read, frame.pcr, frame.sp, footprint.above, footprint.frame);
    return !reach.Known() || reach.Overlaps(g, footprint.deviceSpan);
}

// Guest reads never wrap at 2^32; known reach fails past the limit, unknown
// reach leaves it possible.
template <class Read>
bool StackReachesPointee(const Read& read, const StackReach& reach, std::uint64_t pointer, std::uint64_t offset, std::uint64_t bytes)
{
    std::uint32_t value = 0;
    if (pointer > 0xFFFFFFFFULL - 3 || !read(static_cast<std::uint32_t>(pointer), value) || value == 0)
        return false;
    const std::uint64_t start = std::uint64_t{ value } + offset;
    if (reach.Known() && start + bytes > (std::uint64_t{ 1 } << 32))
        return false;
    return reach.Overlaps(start, bytes);
}

// Allocator metadata: G+0x34CC pool object reads base/size at+152/+156(160-byte
// reach); G+0x2A90 progress reads+4. Models never read/write the fixed buffer
// itself.
inline constexpr std::uint32_t kStackPoolObjectWord  = 0x34CC;
inline constexpr std::uint32_t kStackPoolObjectBytes = 160;
inline constexpr std::uint32_t kStackProgressWord    = 0x2A90;
inline constexpr std::uint32_t kStackProgressOffset  = 4;

template <class Read>
bool StackReachesAllocator(const Read& read, const StackReach& reach, std::uint32_t g)
{
    return StackReachesPointee(read, reach, std::uint64_t{ g } + kStackPoolObjectWord, 0, kStackPoolObjectBytes) ||
           StackReachesPointee(read, reach, std::uint64_t{ g } + kStackProgressWord, kStackProgressOffset, 4);
}

} // namespace rerevved::gpu::guest_d3d
