#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Committed guest memory as native draw and capture readers see it, as a copy
// or in place; throws for any unreadable byte. A view is valid while its range
// is committed.
std::vector<std::uint8_t>     ReadGuestMemory(std::uint64_t address, std::uint64_t size, bool physical);
std::span<const std::uint8_t> ViewGuestMemory(std::uint64_t address, std::uint64_t size, bool physical);

// memcpy of guest bytes that returns false instead of faulting when the pages are
// unmapped, decommitted or protected by the time of the copy.
bool CopyGuestGuarded(void* destination, const void* source, std::size_t size) noexcept;

// The same with the copy function chosen by the caller, for tests. Not noexcept: an
// exception the guard does not take unwinds through it, and a noexcept frame would
// terminate instead.
using GuestCopyFn = void* (*)(void*, const void*, std::size_t);
bool CopyGuestGuardedWith(GuestCopyFn copy, void* destination, const void* source, std::size_t size);

} // namespace rerevved::gpu::guest_d3d
