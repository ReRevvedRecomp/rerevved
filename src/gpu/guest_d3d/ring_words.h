#pragma once

#include <bit>
#include <cstdint>

namespace rerevved::gpu::guest_d3d
{

// Resolve/Clear exit models bound ring cursor G+0x30 before deciding whether
// Original can flush.

// The largest render target count G+0x31CC the loops of the helpers are
// bounded by; the array they walk (G+0x32C0...) holds 23 eight-byte entries.
inline constexpr std::uint32_t kRingMaxTargetCount = 16;

// The side buffer 0x8269B818 takes for Resolve: six dwords.
inline constexpr std::uint32_t kRingResolveSideBufferBytes = 24;

// 0x826AC8C8 emits at most 32 + 5n words for n render targets (g202:13191-13594).
constexpr std::uint64_t RingWordsAc8c8(std::uint64_t targets) noexcept
{
    return 32 + 5 * targets;
}

// 0x8268D3A0 emits at most 12 + 7n words (g153).
constexpr std::uint64_t RingWords8d3a0(std::uint64_t targets) noexcept
{
    return 12 + 7 * targets;
}

// The words one flush group emits through 0x826ACC60 for these dirty bits: each
// run of consecutive bits is a header word and one word per bit.
constexpr std::uint64_t RingGroupWords(std::uint64_t dirty, unsigned low, unsigned count) noexcept
{
    const std::uint64_t mask = count >= 64 ? ~std::uint64_t{ 0 } : (std::uint64_t{ 1 } << count) - 1;
    const std::uint64_t bits = (dirty >> low) & mask;
    return static_cast<std::uint64_t>(std::popcount(bits)) + static_cast<std::uint64_t>(std::popcount(bits & ~(bits << 1)));
}

// The flush groups of Resolve and Clear on dirty words G+0x10, G+0x18 and
// G+0x20: G+0x10 bits 0..11, 12..16, 21..41 and 42..57, G+0x18 bits 34..54 and
// G+0x20 bits 0..37 (g167:13644-13867, g158:13166-13390).
constexpr std::uint64_t RingFlushGroupWords(std::uint64_t dirty10, std::uint64_t dirty18, std::uint64_t dirty20) noexcept
{
    return RingGroupWords(dirty10, 0, 12) + RingGroupWords(dirty10, 12, 5) + RingGroupWords(dirty10, 21, 21) + RingGroupWords(dirty10, 42, 16) + RingGroupWords(dirty18, 34, 21) +
           RingGroupWords(dirty20, 0, 38);
}

} // namespace rerevved::gpu::guest_d3d
