#include "save_slots.h"

#include <algorithm>

namespace rerevved::save_slots
{

int32_t ClampSlots(int64_t requested) noexcept
{
    return static_cast<int32_t>(std::clamp<int64_t>(requested, kNativeSlots, kMaxSlots));
}

uint32_t EnumeratorEntries(int32_t slots) noexcept
{
    return static_cast<uint32_t>(ClampSlots(slots)) + kReservedEntries;
}

uint32_t ObjectBytes(int32_t slots) noexcept
{
    return kObjectHeaderBytes + kEntryBytes * EnumeratorEntries(slots);
}

uint32_t UsedSlots(std::span<const uint32_t> nameLengths) noexcept
{
    return static_cast<uint32_t>(std::ranges::count_if(nameLengths, [](uint32_t length)
                                                       {
                                                           return length != 0;
                                                       }));
}

uint32_t FullnessCount(uint32_t used, int32_t slots) noexcept
{
    return used >= static_cast<uint32_t>(ClampSlots(slots)) ? static_cast<uint32_t>(kNativeSlots) : std::min<uint32_t>(used, kNativeSlots - 1);
}

uint32_t RowCount(uint32_t used, int32_t slots, bool saveMode) noexcept
{
    return used >= static_cast<uint32_t>(ClampSlots(slots)) || !saveMode ? used : used + 1;
}

bool KeepsManualSlot(uint32_t slot, std::string_view fileName, int32_t slots) noexcept
{
    return slot < static_cast<uint32_t>(ClampSlots(slots)) && !(slot == 11 && fileName.starts_with("war"));
}

} // namespace rerevved::save_slots
