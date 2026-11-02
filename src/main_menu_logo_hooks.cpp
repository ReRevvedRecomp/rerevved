#include "keyboard_icons.h"
#include "main_menu_logo_asset.h"

#include <algorithm>
#include <array>
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

// Each keyboard prompt image keeps its own buffer: all payloads stay live.
struct KeyboardServe
{
    uint32_t buffer = 0;
    bool     logged = false;
};

thread_local std::array<KeyboardServe, rerevved::keyboard_icons::kSlots.size()> keyboardServes;

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

// Check pages separately so short names before unmapped pages remain readable.
bool readGuestFileName(rex::memory::Memory* memory,
                       uint32_t             address,
                       char (&out)[rerevved::keyboard_icons::kMaxFileNameLength + 1],
                       size_t& length)
{
    constexpr uint32_t kPage = 0x1000;
    constexpr uint32_t kSpan = rerevved::keyboard_icons::kMaxFileNameLength + 1;
    for (uint32_t done = 0; done < kSpan;)
    {
        const uint32_t at    = address + done;
        const uint32_t chunk = std::min<uint32_t>(kPage - (at & (kPage - 1)), kSpan - done);
        if (!isGuestRangeAccessible(memory, at, chunk, false))
        {
            return false;
        }
        const auto* text = memory->TranslateVirtual<const char*>(at);
        for (uint32_t i = 0; i < chunk; ++i)
        {
            if (text[i] == 0)
            {
                length = done + i;
                return true;
            }
            out[done + i] = text[i];
        }
        done += chunk;
    }
    return false;
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
        // Bare prompt names ignore case; movie vector prompts bypass this hook and stay Xbox.
        char                               name[rerevved::keyboard_icons::kMaxFileNameLength + 1];
        size_t                             length = 0;
        rerevved::keyboard_icons::Prepared icon;
        if (!rerevved::keyboard_icons::AnyPrepared() ||
            !readGuestFileName(memory, requestedName.u32, name, length) ||
            !rerevved::keyboard_icons::TryGetPrepared(std::string_view(name, length), icon))
        {
            return;
        }
        KeyboardServe& serve = keyboardServes[*rerevved::keyboard_icons::FindSlot(std::string_view(name, length))];
        if (serveGuestPayload(memory,
                              stackPointer,
                              returnedDataPointer,
                              *icon.payload,
                              serve.buffer) &&
            !serve.logged)
        {
            REXLOG_INFO("ButtonIcons: served {} as {}",
                        std::string_view(name, length),
                        icon.key);
            serve.logged = true;
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
