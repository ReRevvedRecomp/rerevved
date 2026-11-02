// Special Functions > Change Options reaches 0x82D84580 (virtual +0xCC of pause root 0x8314F76C, return 0x82D068E4).
// Retail builds GFX_OptionsScreen (0x82DE69D8) and runs it through 0x82E40738. The hook preserves the receiver token pair
// (+0x1F4, +0x1F0) and runs the title page and a copy of the runner loop instead.

#include "options_modal_guest.h"

#include <chrono>
#include <optional>
#include <thread>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "retail_audio.h"

REX_EXTERN(__imp__sub_82D84580);
REX_EXTERN(sub_82C79D38);

namespace rerevved::retail_audio
{

void DrainOnGuest(PPCContext& ctx, uint8_t* base) noexcept;

} // namespace rerevved::retail_audio

namespace
{

namespace om = rerevved::options_modal;
namespace ns = rerevved::native_settings;

// Guest frames start below the hooked call's r1 so a callee's prologue never touches the caller's frame.
constexpr uint32_t kFrameBytes = 0x100;

// Retail has no wait; pace callbacks that would otherwise pin a core.
constexpr std::chrono::milliseconds kMinIteration{ 1 };

constexpr std::chrono::milliseconds kCloseRetry{ 10 };

uint32_t loadU32(const uint8_t* base, uint32_t address)
{
    const uint8_t* source = base + address;
    return uint32_t{ source[0] } << 24 | uint32_t{ source[1] } << 16 | uint32_t{ source[2] } << 8 |
           uint32_t{ source[3] };
}

void storeU32(uint8_t* base, uint32_t address, uint32_t value)
{
    uint8_t* target = base + address;
    target[0]       = static_cast<uint8_t>(value >> 24);
    target[1]       = static_cast<uint8_t>(value >> 16);
    target[2]       = static_cast<uint8_t>(value >> 8);
    target[3]       = static_cast<uint8_t>(value);
}

PPCContext freshCall(const PPCContext& caller, uint8_t* base)
{
    PPCContext call = caller;
    call.r1.u64     = caller.r1.u32 - kFrameBytes;
    storeU32(base, call.r1.u32, caller.r1.u32);
    return call;
}

std::optional<uint32_t> callGuest(PPCContext& caller, uint8_t* base, uint32_t target, uint32_t r3, uint32_t r4, uint32_t r5)
{
    PPCFunc* function = target != 0 ? rex::runtime::ResolveIndirectFunction(target) : nullptr;
    if (!function)
        return std::nullopt;
    PPCContext call           = freshCall(caller, base);
    call.r3.u64               = r3;
    call.r4.u64               = r4;
    call.r5.u64               = r5;
    call.last_indirect_target = target;
    function(call, base);
    caller.fpscr = call.fpscr;
    return call.r3.u32;
}

std::optional<uint32_t> callVirtual(PPCContext& caller, uint8_t* base, uint32_t object, uint32_t slot, uint32_t r4)
{
    const uint32_t table = object != 0 ? loadU32(base, object) : 0;
    if (table == 0)
        return std::nullopt;
    return callGuest(caller, base, loadU32(base, table + slot), object, r4, 0);
}

// Preserve runner 0x82E40738's depth counter, flag byte and rumble stop, and set the application's gate byte to one.
class Modal
{
public:
    Modal(PPCContext& ctx, uint8_t* base, const ns::HostBindings& host, uint64_t page)
    : ctx_(ctx)
    , base_(base)
    , host_(host)
    , page_(page)
    {
        storeU32(base_, om::kModalDepthGlobal, loadU32(base_, om::kModalDepthGlobal) + 1);
        base_[om::kModalFlagGlobal] = 0; // the low byte of r4
        if (const uint32_t application = loadU32(base_, om::kApplicationGlobal))
        {
            saved_                                   = base_[application + om::kGateByteOffset];
            base_[application + om::kGateByteOffset] = 1;
        }
        if (const uint32_t input = loadU32(base_, om::kInputOwnerGlobal))
        {
            // 0x82C79D38(owner, 0.0, 0.0): the float at 0x8211B944 is zero.
            PPCContext call = freshCall(ctx_, base_);
            call.r3.u64     = input;
            call.f1.f64     = 0.0;
            call.f2.f64     = 0.0;
            sub_82C79D38(call, base_);
            ctx_.fpscr = call.fpscr;
        }
    }

    Modal(const Modal&)            = delete;
    Modal& operator=(const Modal&) = delete;

    ~Modal()
    {
        if (saved_)
            if (const uint32_t application = loadU32(base_, om::kApplicationGlobal))
                base_[application + om::kGateByteOffset] = *saved_;
        storeU32(base_, om::kModalDepthGlobal, loadU32(base_, om::kModalDepthGlobal) - 1);
    }

    bool Alive()
    {
        return host_.alive && host_.alive();
    }

    bool PageOpen()
    {
        return host_.optionsPageOpen && host_.optionsPageOpen(page_);
    }

    std::optional<bool> CallApplication(const om::Call& call)
    {
        const uint32_t application = loadU32(base_, om::kApplicationGlobal);
        const uint32_t table       = application != 0 ? loadU32(base_, application) : 0;
        if (table == 0)
            return std::nullopt;
        const auto result = callGuest(ctx_, base_, loadU32(base_, table + call.slot), application, call.r4, call.r5);
        if (!result)
            return std::nullopt;
        return (*result & 0xFF) != 0;
    }

    bool CancelRequested()
    {
        const uint32_t frontend = loadU32(base_, om::kFrontendGlobal);
        const uint32_t state    = frontend != 0 ? loadU32(base_, frontend + om::kFrontendStateField) : 0;
        const uint32_t type     = state != 0 ? loadU32(base_, state + om::kFrontendTypeField) : 0;
        return om::CancelRequested(state, type, loadU32(base_, om::kStatusWord));
    }

    // Refused or abandoned jobs leave the page up, so retry until the gate closes or the title shuts down.
    void RequestClose()
    {
        const auto done = ns::RunOnUi<bool>(host_.post,
                                            host_.alive,
                                            host_.timeout,
                                            [close = host_.closeOptionsPage, page = page_]
                                            {
                                                close(page);
                                                return true;
                                            });
        if (!done)
            std::this_thread::sleep_for(kCloseRetry);
    }

    // 0x82C7DF58's frame callback also drains audio, so drain here too (idempotent) to apply live volume edits.
    void EndIteration()
    {
        PPCContext call = freshCall(ctx_, base_);
        rerevved::retail_audio::DrainOnGuest(call, base_);
        const auto elapsed = std::chrono::steady_clock::now() - last_;
        if (elapsed < kMinIteration)
            std::this_thread::sleep_for(kMinIteration - elapsed);
        last_ = std::chrono::steady_clock::now();
    }

private:
    PPCContext&                           ctx_;
    uint8_t*                              base_;
    const ns::HostBindings&               host_;
    uint64_t                              page_;
    std::optional<uint8_t>                saved_;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

} // namespace

namespace rerevved::options_modal
{

Exit RunPageModal(PPCContext& ctx, uint8_t* base, const native_settings::HostBindings& host, uint64_t page)
{
    Modal modal(ctx, base, host, page);
    return RunLoop(modal);
}

} // namespace rerevved::options_modal

REX_HOOK_RAW(sub_82D84580)
{
    const auto host  = ns::Host();
    const bool bound = host && host->post && host->alive && host->openOptionsPage && host->optionsPageOpen &&
                       host->closeOptionsPage;
    if (om::ChooseRoute(static_cast<uint32_t>(ctx.lr), ctx.r3.u32, bound) != om::Route::kPage)
    {
        __imp__sub_82D84580(ctx, base);
        return;
    }
    // Open first so failure leaves retail untouched, and wait so pad capture exists before the next guest read.
    const auto page = ns::RunOnUi<uint64_t>(host->post, host->alive, host->timeout, host->openOptionsPage);
    if (!page || *page == 0)
    {
        REXLOG_WARN("Pause menu: the Options page did not open; running the retail Options screen");
        __imp__sub_82D84580(ctx, base);
        return;
    }
    REXLOG_INFO("Pause menu: Change Options opened the Options page");

    const uint32_t receiver = ctx.r3.u32;
    const uint32_t token    = callVirtual(ctx, base, receiver, om::kTokenSlot, 0).value_or(0);
    const auto     exit     = om::RunPageModal(ctx, base, *host, *page);
    if ((token & 0xFF) != 0)
        (void)callVirtual(ctx, base, receiver, om::kRestoreSlot, 0);
    REXLOG_INFO("Pause menu: the Options page ended ({})", static_cast<int>(exit));
    // 0x82D068E4 ignores the result and runs the Special Functions loop again.
    ctx.r3.u64 = 0;
}
