#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace rerevved::save_slots
{

inline constexpr int32_t kNativeSlots  = 10;
inline constexpr int32_t kDefaultSlots = 99;
inline constexpr int32_t kMaxSlots     = 99;

// Config file, up to 50 numbered autosaves, war, quit and AI saves share the enumerator with the manual slots.
inline constexpr uint32_t kReservedEntries = 64;

inline constexpr uint32_t kObjectHeaderBytes = 1160;
inline constexpr uint32_t kEntryBytes        = 308;
inline constexpr uint32_t kRecordStride      = 356;
inline constexpr uint32_t kNativeObjectBytes = kObjectHeaderBytes + 11 * kEntryBytes;

int32_t ClampSlots(int64_t requested) noexcept;

uint32_t EnumeratorEntries(int32_t slots) noexcept;

uint32_t ObjectBytes(int32_t slots) noexcept;

uint32_t UsedSlots(std::span<const uint32_t> nameLengths) noexcept;

// Reports 10 once every slot is used, so the native "== 10 means full" tests mean full at `slots`.
uint32_t FullnessCount(uint32_t used, int32_t slots) noexcept;

uint32_t RowCount(uint32_t used, int32_t slots, bool saveMode) noexcept;

// The native parser gives the war autosave, any name starting "war", slot id 11.
bool KeepsManualSlot(uint32_t slot, std::string_view fileName, int32_t slots) noexcept;

} // namespace rerevved::save_slots
