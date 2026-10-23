#pragma once

#include <cstdint>

#include <rex/system/xmemory.h>

namespace rerevved::gpu
{

// Guest physical watches return false if a store races before full arming; SDK
// and host writers must notify the watch.
inline bool WatchGuestPhysicalWrites(rex::memory::Memory& memory, std::uint32_t physical, std::uint32_t length)
{
    memory.EnablePhysicalMemoryAccessCallbacks(physical, length, true, false);
    return memory.IsPhysicalRangeWriteWatched(physical, length);
}

} // namespace rerevved::gpu
