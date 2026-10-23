#include "wonder_effect_rules_registry.h"

#include <cstdint>

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

namespace
{

namespace rules = rerevved::wonder_effect_rules;

// Evidence: https://github.com/ReRevvedRecomp/rerevved-research/blob/main/manifests/stonehenge-completion-religion-grant.json
// (RVA-F-0246).
constexpr uint32_t kClaimTable          = 0x830E5148; // Owner and city word pair per Wonder index.
constexpr uint32_t kClaimStride         = 8;
constexpr uint32_t kClaimOwnerWord      = kClaimTable + kClaimStride * rules::kStonehengeWonderIndex;
constexpr uint32_t kClaimCityWord       = kClaimOwnerWord + 4;
constexpr uint32_t kCityBase            = 0x8314FE28;
constexpr uint32_t kCityStride          = 0xBC;
constexpr uint32_t kPlayerCountWord     = 0x82F700AC;
constexpr uint32_t kExcludedPlayerWord  = 0x82F700B0;
constexpr uint32_t kTechnologyAvailable = 0x82CEF6A8; // (technology, owner) -> 0 when not available.
constexpr uint32_t kTechnologyAcquire   = 0x82D09208; // (owner, technology, city, reason, flags).
constexpr uint64_t kReligion            = 19;
constexpr uint64_t kWonderRewardReason  = 5;
constexpr uint64_t kStateFanOut         = 1;

static_assert(kClaimOwnerWord == 0x830E5160 && kClaimCityWord == 0x830E5164);

thread_local bool granting = false;

bool isGuestReadableRange(uint32_t address, uint32_t extent)
{
    if (address > UINT32_MAX - extent || address < 0x10000 ||
        address + extent - 1 >= 0xFFFFF000)
    {
        return false;
    }

    auto* memory = REX_KERNEL_MEMORY();
    if (!memory)
    {
        return false;
    }
    auto* heap = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(address + extent - 1) != heap)
    {
        return false;
    }
    const auto access   = static_cast<uint32_t>(heap->QueryRangeAccess(address, address + extent - 1));
    const auto required = static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly);
    return (access & required) == required;
}

bool tryReadS8(uint32_t address, int32_t& value)
{
    if (!isGuestReadableRange(address, sizeof(uint8_t)))
    {
        return false;
    }
    value = static_cast<int8_t>(REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address)[0]);
    return true;
}

bool tryReadS32(uint32_t address, int32_t& value)
{
    if (!isGuestReadableRange(address, sizeof(uint32_t)))
    {
        return false;
    }
    const auto* source =
        REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address);
    value = static_cast<int32_t>((uint32_t{ source[0] } << 24) | (uint32_t{ source[1] } << 16) |
                                 (uint32_t{ source[2] } << 8) | uint32_t{ source[3] });
    return true;
}

class GrantGuard
{
public:
    GrantGuard()
    {
        granting = true;
    }

    ~GrantGuard()
    {
        granting = false;
    }

    GrantGuard(const GrantGuard&)            = delete;
    GrantGuard& operator=(const GrantGuard&) = delete;
};

} // namespace

// After guarded CityUpdate claim, r27 is the normalized Wonder index and r23
// the city; new Stonehenge claim adds Religion when absent.
void GrantStonehengeReligion(PPCRegister& city, PPCRegister& wonder)
{
    if (granting || !rules::IsStonehengeReligionActive() || wonder.s32 != rules::kStonehengeWonderIndex)
    {
        return;
    }

    rules::GrantQuery query{};
    query.wonderIndex = wonder.s32;
    query.cityIndex   = city.s32;
    query.ruleActive  = true;
    if (query.cityIndex >= 0 && query.cityIndex < rules::kCitySlots)
    {
        const uint32_t record = kCityBase + static_cast<uint32_t>(query.cityIndex) * kCityStride;
        query.readsOk         = tryReadS32(kClaimOwnerWord, query.claimOwner) &&
                                tryReadS32(kClaimCityWord, query.claimCity) &&
                                tryReadS8(record, query.liveOwner) &&
                                tryReadS32(kPlayerCountWord, query.playerCount) &&
                                tryReadS32(kExcludedPlayerWord, query.excludedPlayer);
    }
    if (!rules::ShouldGrantReligion(query))
    {
        return;
    }

    // The generated call passes the interrupted context's own register, which
    // proves this is a bound guest thread whose state ExecuteTrap restores.
    auto* thread = rex::runtime::ThreadState::Get();
    if (!thread || &thread->context()->r23 != &city)
    {
        return;
    }
    auto* kernel     = REX_KERNEL_STATE();
    auto* dispatcher = kernel ? kernel->function_dispatcher() : nullptr;
    if (!dispatcher)
    {
        return;
    }

    // No host lock is held across these guest calls.
    GrantGuard     guard;
    const uint64_t owner          = static_cast<uint64_t>(query.claimOwner);
    uint64_t       availability[] = { kReligion, owner };
    if (dispatcher->ExecuteTrap(thread, kTechnologyAvailable, availability, 2) != 0)
    {
        return;
    }
    uint64_t acquisition[] = { owner, kReligion, static_cast<uint64_t>(query.cityIndex), kWonderRewardReason, kStateFanOut };
    dispatcher->ExecuteTrap(thread, kTechnologyAcquire, acquisition, 5);
}
