#include "ai_civilization_setup.h"

#include "ai_civilizations.h"

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace rerevved::ai_civilizations
{
namespace
{

constexpr uint32_t kSpecialGame    = 0x8314F180;
constexpr uint32_t kSelectionFlagA = 0x8314F1CC;
constexpr uint32_t kSelectionFlagB = 0x8314F1D0;

struct State
{
    uint8_t* base          = nullptr;
    int32_t  requested     = 0;
    int32_t  mode          = 0;
    bool     standardRules = false;
    bool     applied       = false;
    bool     recounted     = false;
    bool     committed     = false;
    bool     rejected      = false;
};

thread_local State    state;
thread_local uint32_t scopeDepth = 0;

bool accessible(uint32_t address, uint32_t size, bool write = false) noexcept
{
    auto* kernel = REX_KERNEL_STATE();
    auto* memory = kernel ? kernel->memory() : nullptr;
    if (!memory || state.base != memory->virtual_membase())
    {
        return false;
    }
    auto* heap = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(address + size - 1) != heap)
    {
        return false;
    }
    const auto access = heap->QueryRangeAccess(address, address + size - 1);
    return write ? (access == rex::memory::PageAccess::kReadWrite || access == rex::memory::PageAccess::kExecuteReadWrite)
                 : (static_cast<uint32_t>(access) & static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly)) != 0;
}

bool word(uint32_t address, uint32_t& value) noexcept
{
    if (!accessible(address, 4))
    {
        return false;
    }
    const auto* bytes = state.base + address;
    value             = uint32_t{ bytes[0] } << 24 | uint32_t{ bytes[1] } << 16 | uint32_t{ bytes[2] } << 8 | bytes[3];
    return true;
}

bool half(uint32_t address, uint16_t& value) noexcept
{
    if (!accessible(address, 2))
    {
        return false;
    }
    const auto* bytes = state.base + address;
    value             = static_cast<uint16_t>(uint16_t{ bytes[0] } << 8 | bytes[1]);
    return true;
}

void storeWord(uint32_t address, uint32_t value) noexcept
{
    auto* bytes = state.base + address;
    bytes[0]    = static_cast<uint8_t>(value >> 24);
    bytes[1]    = static_cast<uint8_t>(value >> 16);
    bytes[2]    = static_cast<uint8_t>(value >> 8);
    bytes[3]    = static_cast<uint8_t>(value);
}

bool storeLayout(uint32_t count, uint32_t barbarian) noexcept
{
    if (!accessible(kPlayerCountGlobal, 4, true) || !accessible(kBarbarianPlayerGlobal, 4, true))
    {
        return false;
    }
    storeWord(kPlayerCountGlobal, count);
    storeWord(kBarbarianPlayerGlobal, barbarian);
    return true;
}

bool layout(uint32_t count, uint32_t barbarian) noexcept
{
    uint32_t currentCount     = 0;
    uint32_t currentBarbarian = 0;
    return word(kPlayerCountGlobal, currentCount) && word(kBarbarianPlayerGlobal, currentBarbarian) &&
           currentCount == count && currentBarbarian == barbarian;
}

void reject(const char* reason)
{
    state.rejected = true;
    REXLOG_INFO("Expanded AI setup: requested={} effective=native reason={}", state.requested, reason);
}

void configure()
{
    if (scopeDepth == 0 || state.applied || state.rejected || (state.requested != 5 && state.requested != 6))
    {
        return;
    }
    if (state.standardRules)
    {
        reject("standard_rules");
        return;
    }
    // Mode 2 omits civilization 0 from the native duplicate comparison.
    if (state.mode == 2)
    {
        reject("gotw_setup");
        return;
    }
    uint32_t special        = 0;
    uint32_t humans         = 0;
    uint16_t selectionFlagA = 0;
    uint16_t selectionFlagB = 0;
    if (!accessible(kNetworkGameByte, 1) || !word(kSpecialGame, special) || !word(kHumanMaskGlobal, humans) ||
        !half(kSelectionFlagA, selectionFlagA) || !half(kSelectionFlagB, selectionFlagB))
    {
        reject("unreadable_state");
        return;
    }
    if (state.base[kNetworkGameByte] != 0)
    {
        reject("network_game");
        return;
    }
    if (special != 0)
    {
        reject("special_game");
        return;
    }
    // Native 0x82D22DF0..0x82D22E78 restricts the candidate pool under these flags.
    if (selectionFlagA != 0 || selectionFlagB != 0)
    {
        reject("restricted_civilizations");
        return;
    }
    // The native duplicate scan compares only earlier ordinals. Keep the
    // already selected human before every AI candidate.
    if (humans != 1)
    {
        reject("human_mask");
        return;
    }
    if (!layout(kPlayerCount, kBarbarianPlayer))
    {
        reject("unsupported_layout");
        return;
    }
    if (!storeLayout(static_cast<uint32_t>(state.requested + 2), static_cast<uint32_t>(state.requested + 1)))
    {
        reject("unwritable_layout");
        return;
    }
    // Retry 0x82D220CC returns to the native mask writer at 0x82D21FF4.
    // Keep the first accepted request throughout that native setup attempt.
    state.applied = true;
    REXLOG_INFO("Expanded AI setup: requested={} effective={} reason=applied", state.requested, state.requested);
}

} // namespace

SetupScope::SetupScope(int32_t requested, bool standardRules, uint8_t* base, int32_t mode) noexcept
{
    if (scopeDepth++ != 0)
    {
        return;
    }
    state = { .base = base, .requested = requested, .mode = mode, .standardRules = standardRules };
    // A new setup must not inherit an expanded layout from the preceding game.
    if (layout(7, 6) || layout(8, 7))
    {
        if (!storeLayout(kPlayerCount, kBarbarianPlayer))
        {
            reject("normalization_failed");
        }
    }
}

SetupScope::~SetupScope()
{
    if (--scopeDepth == 0)
    {
        if (state.applied && !state.committed)
        {
            const bool restored = storeLayout(kPlayerCount, kBarbarianPlayer);
            REXLOG_INFO("Expanded AI setup: requested={} effective={} reason={}", state.requested, restored ? "stock" : "unknown", restored ? "setup_incomplete" : "rollback_unavailable");
        }
        state = {};
    }
}

void CommitNewGame() noexcept
{
    if (scopeDepth != 0 && state.applied && state.recounted && !state.committed &&
        layout(static_cast<uint32_t>(state.requested + 2), static_cast<uint32_t>(state.requested + 1)))
    {
        state.committed = true;
    }
}

void NoteNewGameRecount() noexcept
{
    if (scopeDepth != 0 && state.applied)
    {
        state.recounted = true;
    }
}

int32_t ExpandedAiLimit() noexcept
{
    return scopeDepth != 0 && state.applied ? state.requested : 0;
}

void ResetOnLoad() noexcept
{
    state = {};
}

} // namespace rerevved::ai_civilizations

void ConfigureExpandedAiSetup()
{
    rerevved::ai_civilizations::configure();
}

void PreserveExpandedAiSetup(PPCRegister& count, PPCRegister& barbarian)
{
    const int32_t requested = rerevved::ai_civilizations::ExpandedAiLimit();
    if (requested != 0)
    {
        count.u64     = static_cast<uint32_t>(requested + 2);
        barbarian.u64 = static_cast<uint32_t>(requested + 1);
    }
}
