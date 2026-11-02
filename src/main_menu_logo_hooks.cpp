#include "keyboard_icons.h"
#include "main_menu_logo_asset.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include <rex/logging.h>
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
// The keyboard A icon keeps its own buffer: both payloads stay live.
static_assert(rerevved::keyboard_icons::kGuestFileName == "A.dds");
thread_local uint32_t keyboardGuestBuffer    = 0;
thread_local bool     keyboardOverrideLogged = false;

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

// True when the NUL-terminated guest string at `address` is exactly `lowerName`
// apart from ASCII case. `lowerName` is lowercase.
bool guestNameEqualsIgnoreCase(rex::memory::Memory* memory,
                               uint32_t             address,
                               std::string_view     lowerName)
{
    if (!isGuestRangeAccessible(memory,
                                address,
                                static_cast<uint32_t>(lowerName.size() + 1),
                                false))
    {
        return false;
    }

    const auto* text = memory->TranslateVirtual<const unsigned char*>(address);
    for (size_t i = 0; i < lowerName.size(); ++i)
    {
        const unsigned char c = text[i];
        if ((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) != static_cast<unsigned char>(lowerName[i]))
        {
            return false;
        }
    }
    return text[lowerName.size()] == 0;
}

// Length-prefixed (big-endian) copy into process-lifetime guestBuffer; failure preserves registers and retail data.
bool serveGuestPayload(rex::memory::Memory*     memory,
                       PPCRegister&             stackPointer,
                       PPCRegister&             returnedDataPointer,
                       std::span<const uint8_t> data,
                       uint32_t&                guestBuffer)
{
    if (stackPointer.u32 > std::numeric_limits<uint32_t>::max() - 84 ||
        !isGuestRangeAccessible(
            memory, stackPointer.u32 + 84, sizeof(uint32_t), true))
    {
        return false;
    }

    const uint32_t allocationSize =
        kLengthPrefixSize + static_cast<uint32_t>(data.size());
    if (guestBuffer == 0)
    {
        guestBuffer = memory->SystemHeapAlloc(allocationSize);
        if (guestBuffer == 0)
        {
            return false;
        }
    }
    if (!isGuestRangeAccessible(memory,
                                guestBuffer,
                                allocationSize,
                                true))
    {
        guestBuffer = 0;
        return false;
    }

    auto* destination = memory->TranslateVirtual<uint8_t*>(guestBuffer);
    writeBigEndianU32(destination, static_cast<uint32_t>(data.size()));
    std::memcpy(destination + kLengthPrefixSize, data.data(), data.size());
    returnedDataPointer.u64 = guestBuffer + kLengthPrefixSize;
    rex::memory::store_and_swap<uint32_t>(
        memory->TranslateVirtual<uint8_t*>(stackPointer.u32 + 84),
        returnedDataPointer.u32);
    return true;
}

} // namespace

// r30 is the requested name; a match replaces the data pointer in r11 and SP+84.
void ApplyAssetFileOverride(PPCRegister& stackPointer,
                            PPCRegister& returnedDataPointer,
                            PPCRegister& requestedName)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory || stackPointer.u32 == 0 || requestedName.u32 == 0)
    {
        return;
    }

    const bool isLogo =
        isGuestRangeAccessible(memory,
                               requestedName.u32,
                               static_cast<uint32_t>(
                                   rerevved::main_menu_logo::kGuestFileName.size() + 1),
                               false) &&
        std::memcmp(memory->TranslateVirtual<const char*>(requestedName.u32),
                    rerevved::main_menu_logo::kGuestFileName.data(),
                    rerevved::main_menu_logo::kGuestFileName.size() + 1) == 0;
    if (!isLogo)
    {
        // The shared A prompt image is requested by this bare name, in any
        // case. Without a prepared keyboard icon no guest memory is read.
        rerevved::keyboard_icons::Payload icon;
        if (!rerevved::keyboard_icons::TryGetPayload(icon) ||
            icon->size() != rerevved::keyboard_icons::kIconDdsSize ||
            !guestNameEqualsIgnoreCase(
                memory, requestedName.u32, "a.dds"))
        {
            return;
        }
        if (serveGuestPayload(memory,
                              stackPointer,
                              returnedDataPointer,
                              *icon,
                              keyboardGuestBuffer) &&
            !keyboardOverrideLogged)
        {
            REXLOG_INFO("ButtonIcons: served A.dds as {}",
                        rerevved::keyboard_icons::PreparedKey());
            keyboardOverrideLogged = true;
        }
        return;
    }

    rerevved::main_menu_logo::Payload payload;
    if (!rerevved::main_menu_logo::TryGetPayload(payload) ||
        payload->size() != rerevved::main_menu_logo::kLogoDdsSize ||
        !serveGuestPayload(memory,
                           stackPointer,
                           returnedDataPointer,
                           *payload,
                           logoGuestBuffer))
    {
        return;
    }
    if (!logoOverrideLogged)
    {
        REXLOG_INFO("Applied main-menu logo asset override");
        logoOverrideLogged = true;
    }
}
