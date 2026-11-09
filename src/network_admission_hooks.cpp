#include "network_admission_hooks.h"

#include "gameplay_session_settings.h"
#include "network_admission.h"

#include <atomic>
#include <cstring>
#include <optional>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/notifications.h>

REX_EXTERN(sub_82E54FD8);
REX_EXTERN(sub_82E6AFF8);

namespace
{

namespace admission = rerevved::network_admission;

struct NativeWord
{
    uint32_t address;
    uint32_t value;
};

constexpr NativeWord kOwnerProfile[] = {
    { 0x82E2B160, 0x7D4903A6 }, // mtctr r10
    { 0x82E2B164, 0x4E800421 }, // bctrl
    { 0x82E2B168, 0x7C7E1B78 }, // mr r30,r3
    { 0x82E2B16C, 0x57CB063E }, // clrlwi r11,r30,24
    { 0x82E2B170, 0x3BA00000 }, // li r29,0
    { 0x82E2B174, 0x2B0B0000 }, // cmplwi cr6,r11,0
    { 0x82E2B178, 0x419A00F4 }, // beq cr6,0x82e2b26c
    { 0x82E2B17C, 0x807F0070 }, // lwz r3,112(r31)
    { 0x82E2B180, 0x2B030000 }, // cmplwi cr6,r3,0
    { 0x82E2B184, 0x419A0040 }, // beq cr6,0x82e2b1c4
    { 0x82E2B188, 0x81630004 }, // lwz r11,4(r3)
    { 0x82E2B18C, 0x815F0078 }, // lwz r10,120(r31)
    { 0x82E2B190, 0x7F0B5000 }, // cmpw cr6,r11,r10
    { 0x82E2B194, 0x409A0010 }, // bne cr6,0x82e2b1a4
    { 0x82E2B198, 0x817F0074 }, // lwz r11,116(r31)
    { 0x82E2B19C, 0x2B0B0000 }, // cmplwi cr6,r11,0
    { 0x82E2B1A0, 0x419A0098 }, // beq cr6,0x82e2b238
    { 0x82E54FD8, 0x3D608300 }, // lis r11,-32000
    { 0x82E54FDC, 0x39400001 }, // li r10,1
    { 0x82E54FE0, 0x816BD624 }, // lwz r11,-10716(r11)
    { 0x82E54FE4, 0x914B0078 }, // stw r10,120(r11)
    { 0x82E54FE8, 0x4E800020 }, // blr
};

constexpr NativeWord kReturnProfile[] = {
    { 0x82E6AFF8, 0x7D8802A6 }, // mflr r12
    { 0x82E6B018, 0x4BF206B1 }, // bl 0x82D8B6C8
    { 0x82E6B020, 0x4BF23779 }, // bl 0x82D8E798
    { 0x82E6B028, 0x3880000A }, // li r4,10
    { 0x82E6B030, 0x814B001C }, // lwz r10,28(r11)
    { 0x82E6B034, 0x7D4903A6 }, // mtctr r10
    { 0x82E6B038, 0x4E800421 }, // bctrl
};

enum class Profile : int
{
    Unknown,
    Matched,
    Mismatched,
};

std::atomic<Profile> ownerProfile{ Profile::Unknown };
std::atomic<Profile> returnProfile{ Profile::Unknown };
// Stops a frontend that keeps requesting key 2 from re-running admission or repeating the toast.
std::atomic<bool> cancelling{ false };

const uint8_t* readable(uint32_t address, uint32_t size)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory || size == 0 || address < 0x10000 || uint64_t{ address } + size > 0xFFFFF000ull)
    {
        return nullptr;
    }
    auto* heap = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(address + size - 1) != heap)
    {
        return nullptr;
    }
    const auto access = heap->QueryRangeAccess(address, address + size - 1);
    if (access == rex::memory::PageAccess::kNoAccess)
    {
        return nullptr;
    }
    return memory->TranslateVirtual<const uint8_t*>(address);
}

class GuestMemory final : public admission::Memory
{
public:
    std::optional<uint32_t> Word(uint32_t address) override
    {
        const auto* bytes = readable(address, 4);
        if (!bytes)
        {
            return std::nullopt;
        }
        return (uint32_t{ bytes[0] } << 24) | (uint32_t{ bytes[1] } << 16) | (uint32_t{ bytes[2] } << 8) | bytes[3];
    }

    std::optional<uint8_t> Byte(uint32_t address) override
    {
        const auto* bytes = readable(address, 1);
        if (!bytes)
        {
            return std::nullopt;
        }
        return *bytes;
    }
};

bool confirmed(std::atomic<Profile>& cache, GuestMemory& memory, const NativeWord* begin, size_t count, const char* name)
{
    auto state = cache.load();
    if (state == Profile::Unknown)
    {
        bool matched = true;
        for (size_t index = 0; index < count; ++index)
        {
            matched = matched && memory.Word(begin[index].address) == begin[index].value;
        }
        state = matched ? Profile::Matched : Profile::Mismatched;
        cache.store(state);
        if (!matched)
        {
            REXLOG_ERROR("Network admission: native {} window does not match the supported image; it stays unused", name);
        }
    }
    return state == Profile::Matched;
}

struct HostFloatScope
{
    uint32_t saved = PPCFPSCRRegister::Platform::getcsr();

    ~HostFloatScope()
    {
        PPCFPSCRRegister::Platform::setcsr(saved);
    }
};

// Runs on a private frame below the live one, as a bl would, so the owner's registers stay untouched.
bool callGuest(PPCFunc* function, PPCRegister& stack, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr, uint32_t receiver, uint32_t returnAddress)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory || stack.u32 < 0x10070 || !readable(stack.u32 - 0x70, 0x70))
    {
        return false;
    }
    PPCContext call{};
    call.r1.u64 = stack.u32 - 0x70;
    call.r13    = threadPointer;
    call.fpscr  = fpscr;
    call.lr     = returnAddress;
    call.r3.u64 = receiver;
    // Xenon stwu frames use a big-endian 32-bit back chain.
    const uint8_t backChain[] = { static_cast<uint8_t>(stack.u32 >> 24),
                                  static_cast<uint8_t>(stack.u32 >> 16),
                                  static_cast<uint8_t>(stack.u32 >> 8),
                                  static_cast<uint8_t>(stack.u32) };
    std::memcpy(memory->TranslateVirtual<uint8_t*>(call.r1.u32), backChain, sizeof(backChain));
    HostFloatScope scope;
    function(call, memory->virtual_membase());
    return true;
}

void cancelAttempt(GuestMemory& memory, PPCRegister& stack, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr, bool completeRoute)
{
    if (!callGuest(sub_82E54FD8, stack, threadPointer, fpscr, 0, 0x82E2B16C))
    {
        REXLOG_ERROR("Network admission: frontend request could not be made");
        return;
    }
    if (!completeRoute)
    {
        return;
    }
    const auto route = admission::MatchFrontendReturn(memory);
    if (!route)
    {
        REXLOG_WARN("Network admission: no live controller matches the native frontend return; the frontend stays on its screen");
        return;
    }
    REXLOG_INFO("Network admission: frontend return route found slot={}", route->slot);
    if (!callGuest(sub_82E6AFF8, stack, threadPointer, fpscr, route->controller, 0x82E2B16C))
    {
        REXLOG_ERROR("Network admission: frontend return could not be made");
    }
}

void admitStart(PPCRegister& stack, PPCRegister& threadPointer, PPCRegister& updateResult, PPCRegister& app, PPCFPSCRRegister& fpscr)
{
    GuestMemory memory;
    if (!confirmed(ownerProfile, memory, std::begin(kOwnerProfile), std::size(kOwnerProfile), "state owner"))
    {
        return;
    }
    const auto before = admission::ReadTransition(memory, app.u32, updateResult.u32);
    if (!before || !admission::IsNetworkGameStart(*before))
    {
        cancelling.store(false);
        return;
    }
    const bool completeRoute = confirmed(returnProfile, memory, std::begin(kReturnProfile), std::size(kReturnProfile), "frontend return");
    if (cancelling.load())
    {
        cancelAttempt(memory, stack, threadPointer, fpscr, false);
        return;
    }

    // Admission can wait for the host, so act only on a transition that is still pending; a changed start is admitted once more.
    auto                                  current = *before;
    bool                                  retried = false;
    rerevved::gameplay_session::Admission result;
    for (;;)
    {
        try
        {
            result = rerevved::gameplay_session::AdmitNetworkGame(current.setup, current.mode);
        }
        catch (...)
        {
            result = { false, "The host's game settings could not be checked" };
        }
        const auto after  = admission::ReadTransition(memory, app.u32, updateResult.u32);
        const auto action = admission::DecideRecheck(current, after, retried);
        if (action == admission::Recheck::Apply)
        {
            break;
        }
        if (action == admission::Recheck::NoAction)
        {
            REXLOG_WARN("Network admission: the transition changed during admission; no action taken");
            return;
        }
        if (action == admission::Recheck::Refuse)
        {
            result = { false, "The game setup changed while its settings were being checked" };
            break;
        }
        current = *after;
        retried = true;
    }
    if (result.admitted)
    {
        REXLOG_INFO("Gameplay settings: network game admitted before game entry");
        return;
    }
    cancelling.store(true);
    REXLOG_ERROR("Gameplay settings: setup rejected before game entry: {}", result.diagnostic);
    rex::ui::PostNotification("Can't start this game", result.diagnostic);
    cancelAttempt(memory, stack, threadPointer, fpscr, completeRoute);
}

} // namespace

void AdmitNetworkGameStart(PPCRegister& stack, PPCRegister& threadPointer, PPCRegister& updateResult, PPCRegister& app, PPCFPSCRRegister& fpscr)
{
    try
    {
        admitStart(stack, threadPointer, updateResult, app, fpscr);
    }
    catch (...)
    {
        REXLOG_ERROR("Network admission: unexpected failure; the native transition continues");
    }
}
