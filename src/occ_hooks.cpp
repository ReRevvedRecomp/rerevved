#include "occ_hooks.h"
#include "occ_settings.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(occ_trace, false, "ReRevved", "Log One City Challenge rewards, city populations, comparisons for blocked Settlers, village Settlers replacements and refused AI expansion")
    .debug_only();

namespace
{

namespace occ = rerevved::occ;

// Evidence: https://github.com/ReRevvedRecomp/rerevved-research/blob/main/manifests/occ-gold-population-reward.json
// (ownedCityRewardContract).
constexpr uint32_t kCities       = 0x8314FE28;
constexpr uint32_t kCityStride   = 0xBC;
constexpr uint32_t kGrowthHelper = 0x82D00C78;
constexpr uint32_t kMask         = 0x8312E608;
constexpr uint32_t kMaskSource   = 0x8314F1B0;

// Evidence: https://github.com/ReRevvedRecomp/rerevved-research/blob/main/manifests/friendly-village-rewards.json
// (ordinaryRewards category 1 / settlersDifficultyBoundary).
constexpr uint32_t kVillageSettlersBlocked = 0x8314F10B;
constexpr uint32_t kVillageDifficulty      = 0x82FFE7C8;

// Evidence: https://github.com/ReRevvedRecomp/rerevved-research/blob/main/manifests/city-militia-scout-feasibility.json
// (occAiFoundingBoundary).
constexpr uint32_t kAiExpansionPlayerOffset = 0x1C4;

std::atomic<uint64_t> rewardApplications{ 0 };

uint8_t* checkedBytes(uint64_t address, uint32_t size, bool write = false)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory || !size || address < 0x10000 || address + size > 0xFFFFF000ull)
    {
        throw std::runtime_error("OCC: unavailable guest memory");
    }
    const auto first = static_cast<uint32_t>(address);
    const auto last  = first + size - 1;
    auto*      heap  = memory->LookupHeap(first);
    if (!heap || memory->LookupHeap(last) != heap)
    {
        throw std::runtime_error("OCC: invalid guest range");
    }
    const auto access   = static_cast<uint32_t>(heap->QueryRangeAccess(first, last));
    const auto required = static_cast<uint32_t>(write ? rex::memory::PageAccess::kReadWrite : rex::memory::PageAccess::kReadOnly);
    if ((access & required) != required)
    {
        throw std::runtime_error("OCC: inaccessible guest range");
    }
    return memory->TranslateVirtual<uint8_t*>(first);
}

uint32_t readWord(uint64_t address)
{
    const auto* bytes = checkedBytes(address, 4);
    return (uint32_t{ bytes[0] } << 24) | (uint32_t{ bytes[1] } << 16) | (uint32_t{ bytes[2] } << 8) | bytes[3];
}

void traceComparison(const char* site, const occ::Policy& policy, int32_t native, int32_t effective, int32_t population, bool blocked) noexcept
{
    try
    {
        if (blocked && REXCVAR_GET(occ_trace))
        {
            REXLOG_INFO("OCC: blocked_settlers site={} policy={} R={} population={} native_threshold={} effective_threshold={}",
                        site,
                        fmt::ptr(&policy),
                        policy.minimumPopulation,
                        population,
                        native,
                        effective);
        }
    }
    catch (...)
    {
    }
}

class GuestRewardCities final : public occ::RewardCities
{
public:
    GuestRewardCities(rex::runtime::ThreadState* thread, rex::runtime::FunctionDispatcher* dispatcher, uint64_t application)
    : thread_(thread)
    , dispatcher_(dispatcher)
    , application_(application)
    {
        checkedBytes(kCities, occ::kCitySlots * kCityStride, true);
    }

    occ::City Read(int32_t index) override
    {
        const auto* city = checkedBytes(kCities + index * kCityStride, 4);
        return { std::bit_cast<int8_t>(city[0]), std::bit_cast<int8_t>(city[3]) };
    }

    void WritePopulation(int32_t index, int32_t population) override
    {
        *checkedBytes(kCities + index * kCityStride + 3, 1, true) = static_cast<uint8_t>(population);
    }

    void Refresh(int32_t index) override
    {
        const auto before = Read(index);
        // sub_82CF5588 owns a 400-byte frame, including saved r14..r31/LR.
        const uint32_t           stack = thread_->context()->r1.u32;
        std::array<uint8_t, 400> frame{};
        std::memcpy(frame.data(), checkedBytes(stack, frame.size()), frame.size());
        traceHelper(index, "begin");
        uint64_t args[] = { static_cast<uint64_t>(index) };
        // ExecuteTrap pads the guest stack and restores the complete PPC context.
        dispatcher_->ExecuteTrap(thread_, kGrowthHelper, args, 1);
        traceHelper(index, "return");
        if (std::memcmp(frame.data(), checkedBytes(stack, frame.size()), frame.size()) != 0)
        {
            throw std::runtime_error("OCC: growth helper changed the milestone frame");
        }
        const auto after = Read(index);
        if (before.owner != after.owner || before.population != after.population)
        {
            throw std::runtime_error("OCC: growth helper changed city owner/population");
        }
    }

    void Observe(int32_t index, occ::City before, occ::City after) override
    {
        try
        {
            if (REXCVAR_GET(occ_trace))
            {
                REXLOG_INFO("OCC: reward={} city={} owner={} population_before={} population_after={} at_cap={}",
                            application_,
                            index,
                            before.owner,
                            before.population,
                            after.population,
                            before.population >= occ::kPopulationCap);
            }
        }
        catch (...)
        {
        }
    }

private:
    void traceHelper(int32_t index, const char* phase) noexcept
    {
        try
        {
            if (REXCVAR_GET(occ_trace))
            {
                REXLOG_INFO("OCC: reward={} city={} growth_helper={} mask={:08X} source={:08X}",
                            application_,
                            index,
                            phase,
                            readWord(kMask),
                            readWord(kMaskSource));
            }
        }
        catch (...)
        {
        }
    }

    rex::runtime::ThreadState*        thread_;
    rex::runtime::FunctionDispatcher* dispatcher_;
    uint64_t                          application_;
};

} // namespace

void FreezeOccForGame()
{
    occ::FreezeForGame();
}

void ApplyOccCandidateThreshold(PPCRegister& threshold, PPCRegister& type, PPCRegister& population)
{
    const auto    policy    = occ::ForGame();
    const int32_t native    = threshold.s32;
    const int32_t effective = occ::CandidateThreshold(*policy, type.s32, native);
    if (policy->enabled && type.s32 == occ::kSettlers)
    {
        threshold.s64 = effective;
        traceComparison("candidate", *policy, native, effective, population.s32, population.s32 < effective);
    }
}

void ApplyOccCompletionThreshold(PPCRegister& threshold, PPCRegister& type, PPCRegister& population)
{
    const auto    policy    = occ::ForGame();
    const int32_t native    = threshold.s32;
    const int32_t effective = occ::CompletionThreshold(*policy, type.s32, native);
    if (policy->enabled && type.s32 == occ::kSettlers)
    {
        threshold.s64 = effective;
        traceComparison("completion", *policy, native, effective, population.s32, population.s32 <= effective);
    }
}

bool GuardOccAiChoice(PPCRegister& stack, PPCRegister& ordinal)
{
    const auto policy = occ::ForGame();
    if (!policy->enabled)
    {
        return false;
    }
    // Evidence: https://github.com/ReRevvedRecomp/rerevved-research/blob/main/manifests/occ-settler-production-gates.json
    // (aiSelectionContract).
    // Saved prefix P and post-wonder total N' survive in the original argument slots.
    const int32_t prefix = std::bit_cast<int32_t>(readWord(uint64_t{ stack.u32 } + 580));
    const int32_t total  = std::bit_cast<int32_t>(readWord(uint64_t{ stack.u32 } + 588));
    if (!occ::RejectAiChoice(*policy, prefix, total, ordinal.s32))
    {
        return false;
    }
    ordinal.s64 = -1;
    try
    {
        if (REXCVAR_GET(occ_trace))
        {
            REXLOG_INFO("OCC: ai_no_choice prefix={} total={} result=-1 rng_base=82F7AD00", prefix, total);
        }
    }
    catch (...)
    {
    }
    return true;
}

bool ApplyOccGoldReward(PPCRegister& owner, PPCRegister& applying)
{
    const auto policy = occ::ForGame();
    if (!policy->enabled)
    {
        return false;
    }
    if (!applying.u32)
    {
        return true;
    }
    auto* thread     = rex::runtime::ThreadState::Get();
    auto* kernel     = REX_KERNEL_STATE();
    auto* dispatcher = kernel ? kernel->function_dispatcher() : nullptr;
    if (!thread || &thread->context()->r22 != &owner || &thread->context()->r15 != &applying || !dispatcher)
    {
        throw std::runtime_error("OCC: reward requires the interrupted guest context");
    }
    const uint32_t markerAddress = thread->context()->r14.u32;
    const uint32_t marker        = readWord(markerAddress);
    const uint64_t application   = ++rewardApplications;
    try
    {
        if (REXCVAR_GET(occ_trace))
        {
            REXLOG_INFO("OCC: reward={} begin owner={} policy={} R={} marker_address={:08X} marker_before={}",
                        application,
                        owner.s32,
                        fmt::ptr(policy.get()),
                        policy->minimumPopulation,
                        markerAddress,
                        marker);
        }
    }
    catch (...)
    {
    }
    GuestRewardCities cities(thread, dispatcher, application);
    occ::ApplyGoldReward(*policy, owner.s32, true, cities);
    if (readWord(markerAddress) != marker)
    {
        throw std::runtime_error("OCC: growth helper changed the threshold marker");
    }
    try
    {
        if (REXCVAR_GET(occ_trace))
        {
            REXLOG_INFO("OCC: reward={} complete owner={} marker_retained=1 continuation=82CF57E8", application, owner.s32);
        }
    }
    catch (...)
    {
    }
    return true;
}

bool GuardOccVillageSettlers(PPCRegister& player)
{
    const auto policy = occ::ForGame();
    if (!policy->enabled)
    {
        return false;
    }
    try
    {
        if (REXCVAR_GET(occ_trace))
        {
            const bool nativeEligible = *checkedBytes(kVillageSettlersBlocked, 1) == 0 && std::bit_cast<int32_t>(readWord(kVillageDifficulty)) < 2;
            REXLOG_INFO("OCC: village_settlers player={} native_eligible={} replacement=25_gold continuation=82D0EF30", player.s32, nativeEligible);
        }
    }
    catch (...)
    {
    }
    return true;
}

bool GuardOccAiExpansion(PPCRegister& stack, PPCRegister& x, PPCRegister& y)
{
    const auto policy = occ::ForGame();
    if (!policy->enabled)
    {
        return false;
    }
    try
    {
        if (REXCVAR_GET(occ_trace))
        {
            const int32_t player = std::bit_cast<int32_t>(readWord(uint64_t{ stack.u32 } + kAiExpansionPlayerOffset));
            REXLOG_INFO("OCC: ai_expansion_refused player={} x={} y={} continuation=82CBC3D4", player, x.s32, y.s32);
        }
    }
    catch (...)
    {
    }
    return true;
}
