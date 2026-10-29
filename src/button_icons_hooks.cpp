#include "button_icons.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace
{

namespace bp = rerevved::button_icons;

rex::memory::Memory* guestMemory() noexcept
{
    auto* kernel = REX_KERNEL_STATE();
    return kernel ? kernel->memory() : nullptr;
}

// Return a string shorter than `bound`, or empty on failure.
// Check each 4 KiB page separately so an unmapped page after the terminator
// does not reject a valid string.
std::string_view guestString(rex::memory::Memory& memory, uint32_t address, uint32_t bound) noexcept
{
    constexpr uint32_t kPage = 0x1000;
    if (address < 0x10000 || uint64_t{ address } + bound > 0xFFFFF000ull)
    {
        return {};
    }
    const char* text   = memory.TranslateVirtual<const char*>(address);
    uint32_t    length = 0;
    while (length < bound)
    {
        const uint32_t at   = address + length;
        const uint32_t end  = std::min<uint32_t>((at & ~(kPage - 1)) + kPage, address + bound);
        auto*          heap = memory.LookupHeap(at);
        if (!heap || heap->QueryRangeAccess(at, end - 1) == rex::memory::PageAccess::kNoAccess)
        {
            return {};
        }
        const size_t found = strnlen(text + length, end - at);
        if (found < end - at)
        {
            return std::string_view(text, length + found);
        }
        length = end - address;
    }
    return {};
}

std::mutex                      gMutex;
std::map<std::string, uint32_t> gStrings;

// Intern `text` in the guest system heap for process lifetime.
// Return zero on allocation failure.
uint32_t guestCopy(rex::memory::Memory& memory, const std::string& text)
{
    std::lock_guard lock(gMutex);
    if (const auto it = gStrings.find(text); it != gStrings.end())
    {
        return it->second;
    }
    const uint32_t guest = memory.SystemHeapAlloc(static_cast<uint32_t>(text.size() + 1));
    if (guest == 0)
    {
        return 0;
    }
    std::memcpy(memory.TranslateVirtual<char*>(guest), text.c_str(), text.size() + 1);
    gStrings.emplace(text, guest);
    return guest;
}

} // namespace

// Before the external image open at 0x821E3018. r4 is the requested name, a
// bare basename such as "A.dds"; any directory prefix is kept. Only r4 changes.
void SwapButtonIconImage(PPCRegister& name)
{
    if (bp::ActiveFamily() != bp::Family::kPlayStation)
    {
        return;
    }
    auto* memory = guestMemory();
    if (!memory)
    {
        return;
    }
    const std::string_view requested = guestString(*memory, name.u32, 260);
    const size_t           slash     = requested.find_last_of("/\\:");
    const size_t           start     = slash == std::string_view::npos ? 0 : slash + 1;
    const auto             stem      = bp::ImageStem(requested.substr(start));
    if (!stem)
    {
        return;
    }
    const size_t      dot      = requested.size() - 4;
    const std::string replaced = std::string(requested.substr(0, start)) + std::string(*stem) + std::string(requested.substr(dot));
    if (const uint32_t guest = guestCopy(*memory, replaced))
    {
        name.u64 = guest;
    }
}

// Constructor 0x82D3A6C8 copies the C string in r4.
// Replace whole control labels such as "LB"; write only r4.
void SwapButtonIconLabel(PPCRegister& text)
{
    if (bp::ActiveFamily() != bp::Family::kPlayStation)
    {
        return;
    }
    auto* memory = guestMemory();
    if (!memory)
    {
        return;
    }
    // Every label is 2 to 11 characters and starts with B, D, L, R or S.
    const std::string_view label = guestString(*memory, text.u32, 12);
    if (label.size() < 2 || std::string_view("BDLRS").find(label[0]) == std::string_view::npos)
    {
        return;
    }
    if (const auto swapped = bp::Label(label))
    {
        if (const uint32_t guest = guestCopy(*memory, std::string(*swapped)))
        {
            text.u64 = guest;
        }
    }
}

// At the entry of the loading-tip image builder 0x82E81F58, called while the
// tip table is built. r3 is the HTML image name, r4 and r5 its width and height.
// The vspace in r6 and the tip text stay native.
void SwapButtonIconTipImage(PPCRegister& name, PPCRegister& width, PPCRegister& height)
{
    if (bp::ActiveFamily() != bp::Family::kPlayStation)
    {
        return;
    }
    auto* memory = guestMemory();
    if (!memory)
    {
        return;
    }
    const auto tip = bp::Tip(guestString(*memory, name.u32, 16));
    if (!tip)
    {
        return;
    }
    const uint32_t guest = guestCopy(*memory, std::string(tip->name));
    if (guest == 0)
    {
        return;
    }
    name.u64 = guest;
    if (tip->square)
    {
        const int32_t side = bp::SquareSide(width.s32, height.s32);
        width.s64          = side;
        height.s64         = side;
    }
}
