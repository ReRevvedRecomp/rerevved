#include "main_menu_logo_asset.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace
{

constexpr uint32_t    kLengthPrefixSize    = sizeof(uint32_t);
thread_local uint32_t logo_guest_buffer    = 0;
thread_local bool     logo_override_logged = false;

bool IsGuestRangeAccessible(rex::memory::Memory* memory,
                            uint32_t             address,
                            uint32_t             size,
                            bool                 writable)
{
    if (!memory || size == 0 ||
        address > std::numeric_limits<uint32_t>::max() - (size - 1))
    {
        return false;
    }

    const uint32_t end_address = address + size - 1;
    auto*          heap        = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(end_address) != heap)
    {
        return false;
    }

    const uint32_t access = static_cast<uint32_t>(
        heap->QueryRangeAccess(address, end_address));
    constexpr uint32_t kRead = static_cast<uint32_t>(
        rex::memory::PageAccess::kReadOnly);
    constexpr uint32_t kWrite = static_cast<uint32_t>(
        rex::memory::PageAccess::kReadWrite);
    return writable ? (access & kWrite) == kWrite : (access & kRead) != 0;
}

void WriteBigEndianU32(uint8_t* destination, uint32_t value)
{
    destination[0] = static_cast<uint8_t>(value >> 24);
    destination[1] = static_cast<uint8_t>(value >> 16);
    destination[2] = static_cast<uint8_t>(value >> 8);
    destination[3] = static_cast<uint8_t>(value);
}

} // namespace

void ReRevvedApplyAssetFileOverride(PPCRegister& stack_pointer,
                                    PPCRegister& returned_data_pointer,
                                    PPCRegister& requested_name)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory || stack_pointer.u32 == 0 || requested_name.u32 == 0 ||
        !IsGuestRangeAccessible(memory,
                                requested_name.u32,
                                static_cast<uint32_t>(
                                    rerevved::main_menu_logo::kGuestFileName.size() + 1),
                                false) ||
        std::memcmp(memory->TranslateVirtual<const char*>(requested_name.u32),
                    rerevved::main_menu_logo::kGuestFileName.data(),
                    rerevved::main_menu_logo::kGuestFileName.size() + 1) != 0)
    {
        return;
    }

    rerevved::main_menu_logo::Payload payload;
    if (!rerevved::main_menu_logo::TryGetPayload(payload) ||
        !payload || payload->size() != rerevved::main_menu_logo::kLogoDdsSize ||
        payload->size() > std::numeric_limits<uint32_t>::max() -
                              kLengthPrefixSize ||
        stack_pointer.u32 > std::numeric_limits<uint32_t>::max() - 84 ||
        !IsGuestRangeAccessible(
            memory, stack_pointer.u32 + 84, sizeof(uint32_t), true))
    {
        return;
    }

    const uint32_t allocation_size =
        kLengthPrefixSize + static_cast<uint32_t>(payload->size());
    if (logo_guest_buffer == 0)
    {
        logo_guest_buffer = memory->SystemHeapAlloc(allocation_size);
        if (logo_guest_buffer == 0)
        {
            return;
        }
    }
    if (!IsGuestRangeAccessible(memory,
                                logo_guest_buffer,
                                allocation_size,
                                true))
    {
        logo_guest_buffer = 0;
        return;
    }

    auto* destination = memory->TranslateVirtual<uint8_t*>(logo_guest_buffer);
    WriteBigEndianU32(destination,
                      static_cast<uint32_t>(payload->size()));
    std::memcpy(destination + kLengthPrefixSize, payload->data(), payload->size());
    returned_data_pointer.u64 = logo_guest_buffer + kLengthPrefixSize;
    rex::memory::store_and_swap<uint32_t>(
        memory->TranslateVirtual<uint8_t*>(stack_pointer.u32 + 84),
        returned_data_pointer.u32);
    if (!logo_override_logged)
    {
        REXLOG_INFO("Applied main-menu logo asset override");
        logo_override_logged = true;
    }
}
