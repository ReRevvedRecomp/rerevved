// Main-menu Options hooks, see main_menu_options.h: row callback 0x82E51560 (r3=M, r4=A) and frontend update 0x82C568C8.

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "main_menu_options.h"
#include "native_settings.h"

REX_EXTERN(__imp__sub_82E51560);
REX_EXTERN(__imp__sub_82C568C8);
REX_EXTERN(sub_82E4E680);

namespace
{

namespace mo = rerevved::main_menu_options;
namespace ns = rerevved::native_settings;

mo::Session gSession;

uint32_t loadU32(const uint8_t* base, uint32_t address)
{
    const uint8_t* source = base + address;
    return uint32_t{ source[0] } << 24 | uint32_t{ source[1] } << 16 | uint32_t{ source[2] } << 8 |
           uint32_t{ source[3] };
}

bool isBound(const std::optional<ns::HostBindings>& host)
{
    return host && host->post && host->alive && host->openOptionsPage && host->optionsPageOpen && host->closeOptionsPage;
}

// Retail stops at NUL; never read past a descriptor at a readable page's end.
std::optional<uint8_t> readTextByte(const uint8_t* base, uint32_t address)
{
    auto* memory = REX_KERNEL_MEMORY();
    auto* heap   = memory ? memory->LookupHeap(address) : nullptr;
    if (!heap)
        return std::nullopt;
    constexpr auto kRead = static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly);
    if ((static_cast<uint32_t>(heap->QueryRangeAccess(address, address)) & kRead) == 0)
        return std::nullopt;
    return base[address];
}

uint32_t managerOf(const uint8_t* base)
{
    return loadU32(base, mo::kManagerGlobal);
}

uint32_t managerPending(const uint8_t* base)
{
    const uint32_t manager = loadU32(base, mo::kManagerGlobal);
    return manager != 0 ? loadU32(base, manager + mo::kManagerPendingField) : 0;
}

mo::Entry readEntry(const PPCContext& ctx, const uint8_t* base, const std::optional<ns::HostBindings>& host)
{
    mo::Entry entry;
    entry.hostBound           = isBound(host);
    const uint32_t menu       = ctx.r3.u32;
    const uint32_t descriptor = ctx.r4.u32;
    if (menu == 0)
        return entry;
    entry.menuVtable    = loadU32(base, menu);
    entry.parent        = loadU32(base, menu + mo::kParentField);
    const uint32_t text = descriptor != 0 ? loadU32(base, descriptor) : 0;
    if (text != 0)
        entry.descriptorIsOptions = mo::IsOptionsDescriptor(
            [&](uint32_t offset)
            {
                return readTextByte(base, text + offset);
            });
    if (entry.parent == 0)
        return entry;
    entry.parentVtable = loadU32(base, entry.parent);
    entry.pending      = loadU32(base, entry.parent + mo::kPendingField);
    entry.exiting      = base[entry.parent + mo::kExitField];
    if (const uint32_t manager = managerOf(base))
    {
        entry.currentState = loadU32(base, manager + mo::kManagerStateField);
        entry.prompt       = loadU32(base, manager + mo::kManagerPromptField);
    }
    if (entry.parentVtable != 0)
        entry.parentHandler = loadU32(base, entry.parentVtable + mo::kParentSlot);
    if (host && host->optionsPageOpen)
    {
        const auto session = gSession.Current();
        entry.pageOpen     = session.page != 0 && host->optionsPageOpen(session.page);
    }
    return entry;
}

} // namespace

REX_HOOK_RAW(sub_82E51560)
{
    const auto host  = ns::Host();
    const auto entry = readEntry(ctx, base, host);
    switch (mo::ChooseRoute(entry))
    {
        case mo::Route::kRetail:
            __imp__sub_82E51560(ctx, base);
            return;
        case mo::Route::kIgnore:
            return;
        case mo::Route::kPage:
            break;
    }
    // Wait for UI-thread pad capture before the menu reads input again.
    const auto page = ns::RunOnUi<uint64_t>(host->post, host->alive, host->timeout, host->openOptionsPage);
    if (!page || *page == 0)
    {
        REXLOG_WARN("Main menu: the Options page did not open; running the retail Options screen");
        __imp__sub_82E51560(ctx, base);
        return;
    }
    gSession.Begin(*page, entry.parent, managerPending(base));
    REXLOG_INFO("Main menu: Options opened the Options page");
    // Keep the select sound without exiting; it ignores its first argument.
    PPCContext cue = ctx;
    cue.r3.u64     = entry.parent;
    cue.r4.u64     = mo::kCue;
    sub_82E4E680(cue, base);
    ctx.fpscr = cue.fpscr;
}

// Run Original first so the current state's update (R's virtual +8) sets transitions or prompts.
REX_HOOK_RAW(sub_82C568C8)
{
    // The body reuses r3 for later virtual calls, so keep the incoming manager.
    const uint32_t manager = ctx.r3.u32;
    __imp__sub_82C568C8(ctx, base);
    if (!gSession.Active())
        return;
    if (manager == 0 || manager != managerOf(base))
        return;
    const auto session = gSession.Current();
    if (session.page == 0)
        return;
    const auto host = ns::Host();
    if (!isBound(host))
        return;
    if (!host->optionsPageOpen(session.page))
    {
        gSession.End(session.page);
        return;
    }
    mo::Frame frame;
    frame.currentState   = loadU32(base, manager + mo::kManagerStateField);
    frame.managerPending = loadU32(base, manager + mo::kManagerPendingField);
    frame.prompt         = loadU32(base, manager + mo::kManagerPromptField);
    // A replaced parent may be gone; its fields are read only while current.
    if (frame.currentState == session.parent)
    {
        frame.parentTransition = loadU32(base, session.parent + mo::kPendingField);
        frame.parentExiting    = base[session.parent + mo::kExitField];
    }
    if (!mo::ShouldDismiss(session, frame))
        return;
    // The UI-thread job closes only the still-live page, and this thread does not wait.
    if (host->post([close = host->closeOptionsPage, page = session.page]
                   {
                       close(page);
                   }))
    {
        REXLOG_INFO("Main menu: the menu left or a prompt appeared; closing the Options page");
    }
}
