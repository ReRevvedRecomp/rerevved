// Exit Game uses the native quit question; Back reopens Special Functions.

#include "game_language.h"
#include "pause_menu.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REX_EXTERN(__imp__sub_82CE8338);
REX_EXTERN(__imp__sub_82E6A430);

namespace
{

namespace pm = rerevved::pause_menu;

std::mutex                                   gTextMutex;
std::map<std::string, uint32_t, std::less<>> gTexts;

uint32_t loadU32(const uint8_t* base, uint32_t address)
{
    const uint8_t* source = base + address;
    return uint32_t{ source[0] } << 24 | uint32_t{ source[1] } << 16 | uint32_t{ source[2] } << 8 |
           uint32_t{ source[3] };
}

// Process-lifetime heap block per distinct text; allocation failure returns 0.
uint32_t retainedText(std::string_view source)
{
    auto* kernel = REX_KERNEL_STATE();
    auto* memory = kernel ? kernel->memory() : nullptr;
    if (!memory)
        return 0;
    std::lock_guard lock(gTextMutex);
    if (const auto found = gTexts.find(source); found != gTexts.end())
        return found->second;
    const uint32_t block = memory->SystemHeapAlloc(static_cast<uint32_t>(source.size() + 1));
    if (block == 0)
        return 0;
    auto* bytes = memory->TranslateVirtual<char*>(block);
    std::memcpy(bytes, source.data(), source.size());
    bytes[source.size()] = '\0';
    gTexts.emplace(std::string(source), block);
    return block;
}

constexpr size_t kMaxTextLength = 2048;

std::optional<std::string> guestString(const uint8_t* base, uint32_t address)
{
    if (address == 0)
        return std::nullopt;
    const char* text = reinterpret_cast<const char*>(base + address);
    const void* end  = std::memchr(text, 0, kMaxTextLength);
    if (end == nullptr)
        return std::nullopt;
    return std::string(text, static_cast<const char*>(end));
}

// Runs on a copy so r1 stays the Special Functions frame.
uint32_t callGuest(PPCContext& caller, uint8_t* base, PPCFunc* function, const PPCContext& args)
{
    PPCContext call = args;
    function(call, base);
    caller.fpscr = call.fpscr;
    return call.r3.u32;
}

uint32_t formatText(PPCContext& ctx, uint8_t* base, const PPCContext& show, uint32_t manager, uint32_t text)
{
    PPCContext args = show;
    args.r3.u64     = manager;
    args.r4.u64     = text;
    return callGuest(ctx, base, __imp__sub_82E6A430, args);
}

// As 0x82D064E4..0x82D06510; retail sets it only before the first list.
void setHeaderText(PPCContext& ctx, uint8_t* base, const PPCContext& show, uint32_t manager, uint32_t text)
{
    const uint32_t vtable = loadU32(base, pm::kHeaderObject);
    const uint32_t setter = vtable != 0 ? loadU32(base, vtable + pm::kHeaderSetterSlot) : 0;
    if (setter == 0)
        return;
    PPCContext args           = show;
    args.r3.u64               = manager;
    args.r4.u64               = text;
    args.r3.u64               = callGuest(ctx, base, __imp__sub_82E6A430, args);
    args.r4.u64               = args.r3.u64;
    args.r3.u64               = pm::kHeaderObject;
    args.last_indirect_target = setter;
    (void)callGuest(ctx, base, rex::runtime::ResolveIndirectFunction(setter), args);
}

void setHeader(PPCContext& ctx, uint8_t* base, const PPCContext& show, uint32_t manager)
{
    setHeaderText(ctx, base, show, manager, pm::kHeaderText);
}

int32_t showList(PPCContext& ctx, uint8_t* base, const PPCContext& show, uint32_t manager, uint32_t text)
{
    setHeader(ctx, base, show, manager);
    formatText(ctx, base, show, manager, text);
    return static_cast<int32_t>(callGuest(ctx, base, __imp__sub_82CE8338, show));
}

// Retail's Exit to the Main Menu question (0x82D85394..0x82D853AC) without its callback.
int32_t showQuitQuestion(PPCContext& ctx, uint8_t* base, const PPCContext& show, uint32_t manager)
{
    const uint32_t session = loadU32(base, pm::kQuitSessionGlobal);
    const uint32_t mode    = session != 0 ? loadU32(base, session + 4) : 0;
    const bool     ranked  = base[pm::kQuitRankedByte] != 0;
    setHeader(ctx, base, show, manager);
    formatText(ctx, base, show, manager, pm::QuitText(mode, session != 0, ranked));
    PPCContext args = show;
    args.r3.s64     = -1;
    args.r4.u64     = 0;
    args.r5.u64     = 0;
    args.r6.u64     = 0;
    args.r7.u64     = 0;
    args.r8.u64     = 1;
    return static_cast<int32_t>(callGuest(ctx, base, __imp__sub_82CE8338, args));
}

} // namespace

REX_HOOK_RAW(sub_82CE8338)
{
    const auto variant = pm::VariantForReturn(static_cast<uint32_t>(ctx.lr));
    if (!variant)
    {
        __imp__sub_82CE8338(ctx, base);
        return;
    }
    // Missing pieces preserve retail rows; translate retail before inserting rows.
    const uint32_t manager = loadU32(base, pm::kTextManagerGlobal);
    if (manager == 0)
    {
        __imp__sub_82CE8338(ctx, base);
        return;
    }
    const PPCContext show     = ctx;
    const uint32_t   language = rerevved::game_language::ActiveId();
    const uint32_t   retailAt = formatText(ctx,
                                           base,
                                           show,
                                           manager,
                                           *variant == pm::Variant::kFull ? pm::kRetailFullTemplate
                                                                          : pm::kRetailAlternateTemplate);
    const auto       retail   = guestString(base, retailAt);
    const auto       built    = retail ? pm::BuildList(*variant, *retail, language) : std::nullopt;
    const uint32_t   text     = built ? retainedText(*built) : 0;
    if (text == 0)
    {
        if (!built)
            REXLOG_WARN("Pause menu: unexpected retail list text; keeping the retail rows");
        __imp__sub_82CE8338(ctx, base);
        return;
    }

    // Restore the retail header on later lists when No/Back returns here.
    formatText(ctx, base, show, manager, text);
    __imp__sub_82CE8338(ctx, base);
    for (;;)
    {
        const int32_t row    = ctx.r3.s32;
        const auto    choice = pm::ResolveRow(*variant, row);
        switch (choice.kind)
        {
            case pm::Choice::Kind::kRetail:
                ctx.r3.s64 = choice.retailRow;
                return;
            case pm::Choice::Kind::kExitGame:
                if (showQuitQuestion(ctx, base, show, manager) == pm::kQuitConfirmYes)
                {
                    REXLOG_INFO("Pause menu: Exit Game confirmed");
                    pm::RequestQuit();
                    ctx.r3.s64 = -1;
                    return;
                }
                break;
        }
        ctx.r3.s64 = showList(ctx, base, show, manager, text);
    }
}
