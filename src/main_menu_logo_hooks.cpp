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

constexpr uint32_t    kLengthPrefixSize  = sizeof(uint32_t);
thread_local uint32_t logoGuestBuffer    = 0;
thread_local bool     logoOverrideLogged = false;

bool isGuestRangeAccessible(rex::memory::Memory* memory,
                            uint32_t             address,
                            uint32_t             size,
                            bool                 writable)
{
    if (!memory || size == 0 ||
        address > std::numeric_limits<uint32_t>::max() - (size - 1))
    {
        return false;
    }

    const uint32_t endAddress = address + size - 1;
    auto*          heap       = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(endAddress) != heap)
    {
        return false;
    }

    const uint32_t access = static_cast<uint32_t>(
        heap->QueryRangeAccess(address, endAddress));
    constexpr uint32_t kRead = static_cast<uint32_t>(
        rex::memory::PageAccess::kReadOnly);
    constexpr uint32_t kWrite = static_cast<uint32_t>(
        rex::memory::PageAccess::kReadWrite);
    return writable ? (access & kWrite) == kWrite : (access & kRead) != 0;
}

void writeBigEndianU32(uint8_t* destination, uint32_t value)
{
    destination[0] = static_cast<uint8_t>(value >> 24);
    destination[1] = static_cast<uint8_t>(value >> 16);
    destination[2] = static_cast<uint8_t>(value >> 8);
    destination[3] = static_cast<uint8_t>(value);
}

} // namespace

void ReRevvedApplyAssetFileOverride(PPCRegister& stackPointer,
                                    PPCRegister& returnedDataPointer,
                                    PPCRegister& requestedName)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory || stackPointer.u32 == 0 || requestedName.u32 == 0 ||
        !isGuestRangeAccessible(memory,
                                requestedName.u32,
                                static_cast<uint32_t>(
                                    rerevved::main_menu_logo::kGuestFileName.size() + 1),
                                false) ||
        std::memcmp(memory->TranslateVirtual<const char*>(requestedName.u32),
                    rerevved::main_menu_logo::kGuestFileName.data(),
                    rerevved::main_menu_logo::kGuestFileName.size() + 1) != 0)
    {
        return;
    }

    rerevved::main_menu_logo::Payload payload;
    if (!rerevved::main_menu_logo::TryGetPayload(payload) ||
        payload->size() != rerevved::main_menu_logo::kLogoDdsSize ||
        stackPointer.u32 > std::numeric_limits<uint32_t>::max() - 84 ||
        !isGuestRangeAccessible(
            memory, stackPointer.u32 + 84, sizeof(uint32_t), true))
    {
        return;
    }

    const uint32_t allocationSize =
        kLengthPrefixSize + static_cast<uint32_t>(payload->size());
    if (logoGuestBuffer == 0)
    {
        logoGuestBuffer = memory->SystemHeapAlloc(allocationSize);
        if (logoGuestBuffer == 0)
        {
            return;
        }
    }
    if (!isGuestRangeAccessible(memory,
                                logoGuestBuffer,
                                allocationSize,
                                true))
    {
        logoGuestBuffer = 0;
        return;
    }

    auto* destination = memory->TranslateVirtual<uint8_t*>(logoGuestBuffer);
    writeBigEndianU32(destination,
                      static_cast<uint32_t>(payload->size()));
    std::memcpy(destination + kLengthPrefixSize, payload->data(), payload->size());
    returnedDataPointer.u64 = logoGuestBuffer + kLengthPrefixSize;
    rex::memory::store_and_swap<uint32_t>(
        memory->TranslateVirtual<uint8_t*>(stackPointer.u32 + 84),
        returnedDataPointer.u32);
    if (!logoOverrideLogged)
    {
        REXLOG_INFO("Applied main-menu logo asset override");
        logoOverrideLogged = true;
    }
}
