#include "gotw_hooks.h"
#include "gotw_settings.h"

#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace
{

constexpr uint32_t kSetupGlobal = 0x8314EFC8;

bool readWord(uint64_t address, uint32_t& value) noexcept
try
{
    if (address < 0x10000 || address + 4 > 0xFFFFF000ull)
    {
        return false;
    }
    auto* kernel = REX_KERNEL_STATE();
    auto* memory = kernel ? kernel->memory() : nullptr;
    if (!memory)
    {
        return false;
    }
    const auto first = static_cast<uint32_t>(address);
    const auto last  = first + 3;
    auto*      heap  = memory->LookupHeap(first);
    if (!heap || memory->LookupHeap(last) != heap ||
        heap->QueryRangeAccess(first, last) == rex::memory::PageAccess::kNoAccess)
    {
        return false;
    }
    uint8_t bytes[4]{};
    std::memcpy(bytes, memory->TranslateVirtual<const uint8_t*>(first), sizeof(bytes));
    value = (uint32_t{ bytes[0] } << 24) | (uint32_t{ bytes[1] } << 16) |
            (uint32_t{ bytes[2] } << 8) | bytes[3];
    return true;
}
catch (...)
{
    return false;
}

// One-shot request for the civilization override at the writer's store.
struct CivilizationArm
{
    bool        armed        = false;
    uint32_t    setup        = 0;
    uint64_t    launch       = 0;
    uint32_t    civilization = 0;
    const char* site         = "";
};

// Thread-local: the launch hooks arm it just before `bl 0x82DADC88` and the
// writer reaches 0x82DADD18 without branching on the same guest thread, so a writer
// call on another thread can neither consume nor see it.
thread_local CivilizationArm civilizationArm;

void setCivilizationArm(const CivilizationArm& next) noexcept
{
    civilizationArm = next;
}

std::mutex traceMutex;
uint64_t   traceSequence = 0;

template <typename F>
void traceEvent(F&& log) noexcept
{
    if (!rerevved::gotw::TraceEnabled())
    {
        return;
    }
    try
    {
        std::lock_guard lock(traceMutex);
        if (traceSequence > rerevved::gotw::kTraceEventCap)
        {
            return;
        }
        const auto sequence = ++traceSequence;
        if (rerevved::gotw::ShouldTraceEvent(true, sequence))
        {
            log(sequence);
        }
        else
        {
            REXLOG_INFO("GOTW trace seq={} truncated cap={}; complete ordering is inconclusive", sequence, rerevved::gotw::kTraceEventCap);
        }
    }
    catch (...)
    {
        // Observers cannot unwind through guest code.
    }
}

void traceSetup(uint64_t sequence, const char* site, uint64_t lr, uint64_t r3, uint64_t r4, uint64_t r5, uint32_t setup)
{
    uint32_t   global = 0, mode = 0, week = 0, seed = 0, civilization = 0;
    const bool globalRead = readWord(kSetupGlobal, global);
    const bool modeRead   = readWord(uint64_t{ setup } + 4, mode);
    const bool weekRead   = readWord(uint64_t{ setup } + 0x134, week);
    const bool seedRead   = readWord(uint64_t{ setup } + 0x138, seed);
    const bool civRead    = readWord(uint64_t{ setup } + 0x13C, civilization);
    const auto policy     = rerevved::gotw::ForGame();
    REXLOG_INFO("GOTW trace seq={} site={} lr={:016X} r3={:016X} r4={:016X} r5={:016X} global={:08X} global_read={} setup={:08X} mode={} mode_read={} week={} week_read={} seed={} seed_read={} civ={} civ_read={} custom_week={} native_week={} latched_week={} latched_seed={} custom_civ={} latched_civ={}",
                sequence,
                site,
                lr,
                r3,
                r4,
                r5,
                global,
                globalRead,
                setup,
                mode,
                modeRead,
                week,
                weekRead,
                seed,
                seedRead,
                civilization,
                civRead,
                policy.customWeek,
                policy.nativeWeek,
                policy.week,
                policy.seed,
                policy.customCivilization,
                policy.civilization);
}

bool isolateCurrentWeeklyResult()
{
    if (!rerevved::gotw::ForGame().IsCustom())
    {
        return false;
    }
    uint32_t setup = 0;
    uint32_t mode  = 0;
    // Fail closed at these result-only boundaries if a custom setup is unreadable.
    return !readWord(kSetupGlobal, setup) || !readWord(uint64_t{ setup } + 4, mode) ||
           rerevved::gotw::IsolateWeeklyResult(static_cast<int32_t>(mode));
}

} // namespace

void ApplyGotwSeed(PPCRegister& setup, PPCRegister& week, PPCRegister& seed) noexcept
{
    rerevved::gotw::detail::ApplySeed(setup, week, seed, readWord);
}

void rerevved::gotw::detail::ApplySeed(PPCRegister& setup, PPCRegister& week, PPCRegister& seed, WordReader read, gameplay_session::detail::SnapshotProvider capture) noexcept
{
    // A stale arm must not reach a rejected or later launch.
    setCivilizationArm({});
    uint32_t   rawMode  = 0;
    const auto mode     = read && read(uint64_t{ setup.u32 } + 4, rawMode) ? std::optional{ static_cast<int32_t>(rawMode) } : std::nullopt;
    const auto prepared = capture ? gameplay_session::detail::PrepareWeekly(setup.u32, mode, capture) : gameplay_session::PrepareWeekly(setup.u32, mode);
    if (!prepared)
    {
        return;
    }
    // Accepted launch at 0x82E76ED4; the native writer publishes all three fields.
    const auto policy = rerevved::gotw::LatchForGame(week.u32, setup.u32, *prepared);
    rerevved::gotw::RecordAutomaticWeek(policy.nativeWeek);
    if (policy.customWeek)
    {
        week.u64 = policy.week;
        seed.u64 = policy.seed;
    }
    if (policy.customCivilization)
    {
        setCivilizationArm({ true, setup.u32, policy.launch, policy.civilization, "early" });
    }
}

void rerevved::gotw::detail::ReapplySeed(PPCRegister& receiver, PPCRegister& week, PPCRegister& seed, WordReader read) noexcept
{
    // The command transports the effective week, which can differ from nativeWeek.
    const auto policy = rerevved::gotw::ForGame();
    if (!policy.IsCustom())
    {
        return;
    }
    uint32_t setup = 0;
    uint32_t mode  = 0;
    if (read && read(kSetupGlobal, setup) && setup != 0 && setup == policy.setup &&
        receiver.u32 == setup && week.u32 == policy.week &&
        read(uint64_t{ setup } + 4, mode) && mode == 2)
    {
        if (policy.customWeek)
        {
            week.u64 = policy.week;
            seed.u64 = policy.seed;
        }
        if (policy.customCivilization)
        {
            setCivilizationArm({ true, setup, policy.launch, policy.civilization, "late" });
        }
    }
}

void rerevved::gotw::detail::OverrideCivilization(PPCRegister& civilization, PPCRegister& setup, PPCRegister& count) noexcept
try
{
    if (!civilizationArm.armed)
    {
        return;
    }
    const CivilizationArm armed = civilizationArm;

    civilizationArm = {};
    // Hooked before the store at 0x82DADD18, where r10 is the derived civilization,
    // r31 the setup and r5 the live civilization count.
    const uint32_t derived = civilization.u32;
    const auto     policy  = rerevved::gotw::ForGame();
    if (armed.setup != setup.u32 || armed.launch != policy.launch ||
        !rerevved::gotw::IsValidCivilization(armed.civilization) || armed.civilization >= count.u32)
    {
        REXLOG_WARN("Game of the Week: civilization site={} chosen={} rejected (setup={:08X} armed_setup={:08X} count={}); keeping derived={}",
                    armed.site,
                    armed.civilization,
                    setup.u32,
                    armed.setup,
                    count.u32,
                    derived);
        return;
    }
    civilization.u64 = armed.civilization;
    REXLOG_INFO("Game of the Week: civilization site={} derived={} chosen={}", armed.site, derived, armed.civilization);
}
catch (...)
{
    // Logging cannot unwind through the guest hook.
}

void ReapplyGotwSeed(PPCRegister& setup, PPCRegister& week, PPCRegister& seed) noexcept
{
    rerevved::gotw::detail::ReapplySeed(setup, week, seed, readWord);
}

void OverrideGotwCivilization(PPCRegister& civilization, PPCRegister& setup, PPCRegister& count) noexcept
{
    rerevved::gotw::detail::OverrideCivilization(civilization, setup, count);
}

void LogGotwSeed(PPCRegister& setupGlobalBase, PPCRegister& week) noexcept
try
{
    const auto policy = rerevved::gotw::ForGame();
    uint32_t   setup  = 0;
    uint32_t   seed   = policy.seed;
    const bool read   = readWord(uint64_t{ setupGlobalBase.u32 } - 4152, setup) &&
                        readWord(uint64_t{ setup } + 0x138, seed);
    if (read)
    {
        rerevved::gotw::RecordAutomaticSeed(seed);
    }
    if (read || policy.IsCustom())
    {
        REXLOG_INFO("Game of the Week: origin={} native_week={} week={} seed={} seed_read={} custom_civilization={} civilization={}", policy.IsCustom() ? "custom" : "weekly", policy.nativeWeek, policy.week, seed, read, policy.customCivilization, policy.civilization);
    }
    else
    {
        REXLOG_INFO("Game of the Week: origin=weekly week={} seed=unavailable", week.u32);
    }
}
catch (...)
{
    // Logging cannot unwind through the guest hook.
}

void TraceGotwWriterEntry(uint64_t lr, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) noexcept
{
    traceEvent([&](uint64_t sequence)
               {
                   traceSetup(sequence, "82DADC88", lr, r3.u64, r4.u64, r5.u64, r3.u32);
               });
}

void TraceGotwWriterExit(uint64_t lr, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& setup) noexcept
{
    traceEvent([&](uint64_t sequence)
               {
                   traceSetup(sequence, "82DADD1C", lr, r3.u64, r4.u64, r5.u64, setup.u32);
               });
}

void TraceGotwGeneratorSeed(PPCRegister& setup) noexcept
{
    traceEvent([&](uint64_t sequence)
               {
                   uint32_t   seed = 0;
                   const bool read = readWord(uint64_t{ setup.u32 } + 0x138, seed);
                   REXLOG_INFO("GOTW trace seq={} site=82D0F13C G={:08X} seed={} seed_read={}", sequence, setup.u32, seed, read);
               });
}

void TraceGotwPrng(PPCRegister& seed) noexcept
{
    traceEvent([&](uint64_t sequence)
               {
                   uint32_t   first = 0, counter = 0, third = 0, normalization = 0;
                   const bool firstRead         = readWord(0x82F7AD00, first);
                   const bool counterRead       = readWord(0x82F7AD04, counter);
                   const bool thirdRead         = readWord(0x82F7AD08, third);
                   const bool normalizationRead = readWord(0x82F7AD10, normalization);
                   REXLOG_INFO("GOTW trace seq={} site=82D0F168 r27={} prng0={} read0={} prng4={} read4={} prng8={} read8={} normalization_bits={:08X} normalization_read={}",
                               sequence,
                               seed.u32,
                               first,
                               firstRead,
                               counter,
                               counterRead,
                               third,
                               thirdRead,
                               normalization,
                               normalizationRead);
               });
}

void TraceGotwProcedural() noexcept
{
    traceEvent([](uint64_t sequence)
               {
                   REXLOG_INFO("GOTW trace seq={} site=82D0F100 procedural", sequence);
               });
}

void TraceGotwNamedLoad(PPCRegister& result) noexcept
{
    traceEvent([&](uint64_t sequence)
               {
                   REXLOG_INFO("GOTW trace seq={} site=82D0F0F4 named_load_result={}", sequence, result.u8);
               });
}

bool GuardGotwOutcome(PPCRegister& mode) noexcept
{
    return rerevved::gotw::IsolateWeeklyResult(mode.s32);
}

bool GuardGotwScoreCache() noexcept
{
    return isolateCurrentWeeklyResult();
}

bool GuardGotwWeekCache() noexcept
{
    return isolateCurrentWeeklyResult();
}

bool GuardGotwSubmission() noexcept
{
    return isolateCurrentWeeklyResult();
}
