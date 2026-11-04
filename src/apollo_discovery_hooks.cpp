// Apollo (Wonder 17) makes technologies 1..46 query as owned, blocking first-discovery bonuses.
// Record (owner, city) after the native claim; the drain zeroes U32[0x830EDC48 + 4 * t]. Decisions: apollo_discovery.h.

#include "apollo_discovery_hooks.h"
#include "apollo_discovery.h"
#include "gotw_hooks.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

namespace
{

namespace ad = rerevved::apollo_discovery;

constexpr uint32_t kOwnershipWords   = 0x830EDC48;
constexpr uint32_t kClaimOwnerWord   = 0x830E51D0; // Wonder 17: 0x830E5148 + 8 * 17.
constexpr uint32_t kClaimCityWord    = 0x830E51D4;
constexpr uint32_t kCompletionMarker = 0x830E3348;
constexpr uint32_t kContext          = 0x830E3328;
constexpr uint32_t kRecursiveMarker  = 0x830E3430;
constexpr uint32_t kCities           = 0x8314FE28;
constexpr uint32_t kCityStride       = 188;
constexpr uint32_t kCityFlags        = 0x24;
constexpr uint32_t kAcquire          = 0x82D09208; // (owner, technology, city, source, phase).

struct Completion
{
    int32_t player;
    int32_t city;
};

std::mutex              queueMutex;
std::vector<Completion> queue;
std::atomic<uint32_t>   pending{ 0 };
std::atomic<uint32_t>   generation{ 0 };
std::atomic<bool>       draining{ false };

uint32_t word(uint32_t address)
{
    return rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address));
}

int32_t cityOwner(int32_t city)
{
    return static_cast<int8_t>(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(kCities + kCityStride * static_cast<uint32_t>(city))[0]);
}

uint32_t cityFlags(int32_t city)
{
    return word(kCities + kCityStride * static_cast<uint32_t>(city) + kCityFlags);
}

bool holdsApollo(const Completion& entry)
{
    return cityOwner(entry.city) == entry.player && static_cast<int32_t>(word(kClaimOwnerWord)) == entry.player &&
           static_cast<int32_t>(word(kClaimCityWord)) == entry.city;
}

// A grant could overwrite another player's recipient or a city's pending Wonder/spaceship part.
bool ready(const Completion&)
{
    for (uint32_t player = 0; player < ad::kPlayers; ++player)
    {
        if (!ad::DrainReady({ word(kCompletionMarker + 4 * player), word(kContext + 4 * player), word(kRecursiveMarker), 0 }))
        {
            return false;
        }
    }
    for (int32_t city = 0; city < ad::kCities; ++city)
    {
        if (cityOwner(city) >= 0 && !ad::DrainReady({ 0, 0, 0, cityFlags(city) }))
        {
            return false;
        }
    }
    return true;
}

ad::OwnershipWords ownershipWords()
{
    ad::OwnershipWords words{};
    for (uint32_t t = 0; t < words.size(); ++t)
    {
        words[t] = word(kOwnershipWords + 4 * t);
    }
    return words;
}

enum class Outcome
{
    Waiting,
    Finished,
    Dropped,
};

// Each technology's phase pair returns before the next; recheck premises first (nested calls may leave work pending).
Outcome grant(rex::runtime::ThreadState* thread, rex::runtime::FunctionDispatcher* dispatcher, const Completion& entry, uint32_t game, int32_t& granted)
{
    for (const int32_t technology : ad::EligibleTechnologies(ownershipWords()))
    {
        if (generation.load(std::memory_order_relaxed) != game || !holdsApollo(entry))
        {
            return Outcome::Dropped;
        }
        if (!ready(entry))
        {
            return Outcome::Waiting;
        }
        if (!ad::IsUnclaimed(word(kOwnershipWords + 4 * static_cast<uint32_t>(technology))))
        {
            continue;
        }
        const auto player = static_cast<uint64_t>(entry.player);
        const auto city   = static_cast<uint64_t>(entry.city);
        for (const uint64_t phase : { 1ull, 2ull })
        {
            uint64_t arguments[] = { player, static_cast<uint64_t>(technology), city, 0, phase };
            dispatcher->ExecuteTrap(thread, kAcquire, arguments, 5);
        }
        ++granted;
        REXLOG_INFO("Apollo Discovery Bonuses: granted player={} technology={} city={}", entry.player, technology, entry.city);
    }
    return generation.load(std::memory_order_relaxed) == game && holdsApollo(entry) ? Outcome::Finished : Outcome::Dropped;
}

} // namespace

uint32_t ApolloPendingCount() noexcept
{
    return pending.load(std::memory_order_relaxed);
}

void ResetApolloDiscovery() noexcept
try
{
    std::lock_guard lock(queueMutex);
    queue.clear();
    pending.store(0, std::memory_order_relaxed);
    generation.fetch_add(1, std::memory_order_relaxed);
}
catch (...)
{
    // No exception may escape a guest boundary.
}

void RecordApolloCompletion(PPCRegister& city, PPCRegister& wonder)
try
{
    if (wonder.s32 != ad::kApolloWonder || !rerevved::gotw::CurrentSetupIsNotGameOfTheWeek() || city.s32 < 0 ||
        city.s32 >= ad::kCities)
    {
        return;
    }
    const Completion entry{ cityOwner(city.s32), city.s32 };
    if (entry.player < 0 || entry.player >= ad::kPlayers || !holdsApollo(entry))
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        queue.push_back(entry);
        pending.store(static_cast<uint32_t>(queue.size()), std::memory_order_relaxed);
    }
    REXLOG_INFO("Apollo Discovery Bonuses: queued player={} city={}", entry.player, entry.city);
}
catch (...)
{
    // No exception may escape a guest boundary.
}

// The application frame driver 0x82C7DF58, past its first instruction, runs on
// the guest main thread; modal pumps (including pause) re-enter, draining makes those inert.
void DrainApolloDiscoveryBonuses(PPCRegister& sp)
try
{
    if (pending.load(std::memory_order_relaxed) == 0 || draining.exchange(true))
    {
        return;
    }

    struct Release
    {
        ~Release()
        {
            draining.store(false);
        }
    } release;

    // The interrupted register proves a bound guest thread; ExecuteTrap restores its state.
    auto* thread = rex::runtime::ThreadState::Get();
    if (!thread || &thread->context()->r1 != &sp)
    {
        return;
    }
    auto* kernel     = REX_KERNEL_STATE();
    auto* dispatcher = kernel ? kernel->function_dispatcher() : nullptr;
    if (!dispatcher)
    {
        return;
    }

    for (;;)
    {
        Completion entry{};
        uint32_t   game = 0;
        {
            std::lock_guard lock(queueMutex);
            if (queue.empty())
            {
                return;
            }
            entry = queue.front();
            game  = generation.load(std::memory_order_relaxed);
        }
        int32_t       granted = 0;
        const Outcome outcome = grant(thread, dispatcher, entry, game, granted);
        if (outcome == Outcome::Waiting)
        {
            return;
        }
        REXLOG_INFO("Apollo Discovery Bonuses: finished player={} city={} granted={} complete={}", entry.player, entry.city, granted, outcome == Outcome::Finished);
        std::lock_guard lock(queueMutex);
        if (generation.load(std::memory_order_relaxed) == game && !queue.empty())
        {
            queue.erase(queue.begin());
            pending.store(static_cast<uint32_t>(queue.size()), std::memory_order_relaxed);
        }
    }
}
catch (...)
{
    // A failed drain must not unwind through guest code.
}
