#include "ai_civilizations.h"
#include "button_icons.h"
#include "gfx_movie_rows.h"
#include "keyboard_icons.h"
#include "main_menu_logo_asset.h"
#include "place_badges.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

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

// Longest requested name read from the guest; covers a movie name with a path.
constexpr uint32_t kMaxRequestedNameLength = 127;

// Each patched movie, badge image and movie guest name is written once and kept
// for the process.
struct ServedAsset
{
    std::mutex lock;
    uint32_t   buffer = 0;
    uint32_t   name   = 0;
    bool       logged = false;
};

std::array<ServedAsset, 2>                                                                            rowsMovies;
std::array<ServedAsset, rerevved::gfx_movie_rows::kPromptMovies>                                      promptMovies;
std::array<ServedAsset, rerevved::place_badges::kLastPlace - rerevved::place_badges::kFirstPlace + 1> badges;

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
                       char (&out)[kMaxRequestedNameLength + 1],
                       size_t& length)
{
    constexpr uint32_t kPage = 0x1000;
    constexpr uint32_t kSpan = kMaxRequestedNameLength + 1;
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

// Points r11 and SP+84 at the data after a buffer's length prefix.
void publishServedPointer(rex::memory::Memory* memory,
                          PPCRegister&         stackPointer,
                          PPCRegister&         returnedDataPointer,
                          uint32_t             guestBuffer)
{
    returnedDataPointer.u64 = guestBuffer + kLengthPrefixSize;
    rex::memory::store_and_swap<uint32_t>(
        memory->TranslateVirtual<uint8_t*>(stackPointer.u32 + 84),
        returnedDataPointer.u32);
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
    publishServedPointer(memory, stackPointer, returnedDataPointer, guestBuffer);
    return true;
}

bool servePrepared(rex::memory::Memory*                      memory,
                   PPCRegister&                              stackPointer,
                   PPCRegister&                              returnedDataPointer,
                   const rerevved::gfx_movie_rows::Prepared& prepared,
                   ServedAsset&                              rows,
                   std::string_view                          name,
                   bool                                      announce = false)
{
    std::lock_guard guard(rows.lock);
    if (prepared.bytes.empty())
    {
        if (!rows.logged)
        {
            REXLOG_WARN("GfxMovieRows: {} unavailable ({})", name, prepared.reason);
            rows.logged = true;
        }
        return false;
    }
    if (rows.buffer != 0)
    {
        if (stackPointer.u32 > std::numeric_limits<uint32_t>::max() - 84 ||
            !isGuestRangeAccessible(memory, stackPointer.u32 + 84, sizeof(uint32_t), true))
        {
            return false;
        }
        publishServedPointer(memory, stackPointer, returnedDataPointer, rows.buffer);
        return true;
    }
    if (!serveGuestPayload(memory, stackPointer, returnedDataPointer, prepared.bytes, rows.buffer))
    {
        return false;
    }
    if (!rows.logged)
    {
        if (announce)
        {
            REXLOG_INFO("GfxMovieRows: served {} ({} bytes)", name, prepared.bytes.size());
        }
        else
        {
            REXLOG_DEBUG("GfxMovieRows: served {} ({} bytes)", name, prepared.bytes.size());
        }
        rows.logged = true;
    }
    return true;
}

// Expanded games run 7 or 8 players with the barbarian in the last slot. Retail
// layouts, including two-player setups (count 2, barbarian -1), keep the retail movies.
bool isExpandedLayout(rex::memory::Memory* memory)
{
    constexpr uint32_t kCount     = rerevved::ai_civilizations::kPlayerCountGlobal;
    constexpr uint32_t kBarbarian = rerevved::ai_civilizations::kBarbarianPlayerGlobal;
    if (!isGuestRangeAccessible(memory, kCount, sizeof(uint32_t), false) ||
        !isGuestRangeAccessible(memory, kBarbarian, sizeof(uint32_t), false))
    {
        return false;
    }
    const auto count = static_cast<int32_t>(
        rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<const uint8_t*>(kCount)));
    const auto barbarian = static_cast<int32_t>(
        rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<const uint8_t*>(kBarbarian)));
    return (count == 7 || count == 8) && barbarian == count - 1;
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
        // Bare prompt names ignore case.
        char   name[kMaxRequestedNameLength + 1];
        size_t length = 0;
        if (!readGuestFileName(memory, requestedName.u32, name, length))
        {
            return;
        }
        const std::string_view             fileName(name, length);
        rerevved::keyboard_icons::Prepared icon;
        if (const auto movie = rerevved::gfx_movie_rows::FindRowsMovie(fileName))
        {
            servePrepared(memory,
                          stackPointer,
                          returnedDataPointer,
                          rerevved::gfx_movie_rows::PreparedMovie(*movie),
                          rowsMovies[static_cast<size_t>(*movie)],
                          rerevved::gfx_movie_rows::RowsMovieName(*movie));
            return;
        }
        if (const auto prompt = rerevved::gfx_movie_rows::FindPromptMovie(fileName))
        {
            if (rerevved::button_icons::ActiveFamily() == rerevved::button_icons::Family::kKeyboard)
            {
                servePrepared(memory,
                              stackPointer,
                              returnedDataPointer,
                              rerevved::gfx_movie_rows::PreparedPromptMovie(*prompt),
                              promptMovies[static_cast<size_t>(*prompt)],
                              rerevved::gfx_movie_rows::PromptMovieName(*prompt),
                              true);
            }
            return;
        }
        if (const auto place = rerevved::place_badges::FindBadge(fileName))
        {
            servePrepared(memory,
                          stackPointer,
                          returnedDataPointer,
                          rerevved::gfx_movie_rows::PreparedBadge(*place),
                          badges[static_cast<size_t>(*place - rerevved::place_badges::kFirstPlace)],
                          fileName);
            return;
        }
        if (!rerevved::keyboard_icons::AnyPrepared() ||
            !rerevved::keyboard_icons::TryGetPrepared(fileName, icon))
        {
            return;
        }
        KeyboardServe& serve = keyboardServes[*rerevved::keyboard_icons::FindSlot(fileName)];
        if (serveGuestPayload(memory,
                              stackPointer,
                              returnedDataPointer,
                              *icon.payload,
                              serve.buffer) &&
            !serve.logged)
        {
            REXLOG_DEBUG("ButtonIcons: served {} as {}", fileName, icon.key);
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

namespace
{

// r4 is the screen's movie name; a widened game with a prepared copy asks for that copy instead.
void useRowsMovie(PPCRegister& movieName, rerevved::gfx_movie_rows::Movie movie)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory || !isExpandedLayout(memory))
    {
        return;
    }
    const auto&     prepared = rerevved::gfx_movie_rows::PreparedMovie(movie);
    ServedAsset&    rows     = rowsMovies[static_cast<size_t>(movie)];
    std::lock_guard guard(rows.lock);
    if (prepared.bytes.empty())
    {
        if (!rows.logged)
        {
            REXLOG_WARN("GfxMovieRows: {} kept retail ({})", rerevved::gfx_movie_rows::RowsMovieName(movie), prepared.reason);
            rows.logged = true;
        }
        return;
    }
    const std::string_view name = rerevved::gfx_movie_rows::RowsMovieName(movie);
    if (rows.name == 0)
    {
        const uint32_t guestName = memory->SystemHeapAlloc(static_cast<uint32_t>(name.size() + 1));
        if (guestName == 0 || !isGuestRangeAccessible(memory, guestName, static_cast<uint32_t>(name.size() + 1), true))
        {
            return;
        }
        auto* destination = memory->TranslateVirtual<char*>(guestName);
        std::memcpy(destination, name.data(), name.size());
        destination[name.size()] = 0;
        rows.name                = guestName;
    }
    movieName.u64 = rows.name;
}

} // namespace

void UseWinLoseRowsMovie(PPCRegister& movieName)
{
    useRowsMovie(movieName, rerevved::gfx_movie_rows::Movie::WinLoseScreen);
}

void UseHistographRowsMovie(PPCRegister& movieName)
{
    useRowsMovie(movieName, rerevved::gfx_movie_rows::Movie::Histograph);
}
