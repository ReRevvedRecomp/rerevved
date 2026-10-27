#include "gameplay_session_settings.h"
#include "ai_economy_settings.h"
#include "custom_achievements.h"
#include "gameplay_fixes.h"
#include "gotw_settings.h"
#include "mod_script_rules.h"
#include "occ_settings.h"
#include "raging_barbarians_settings.h"

#include <chrono>
#include <mutex>
#include <utility>

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/session_data.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace rerevved::host_settings
{

HostSettings LocalSettings()
{
    const auto   boundary = gameplay_session::LockRuleBoundary();
    HostSettings settings;
    settings.aiCivilizations          = rex::cvar::Query<int32_t>("ai_civilizations");
    settings.ragingBarbarians         = rex::cvar::Query<bool>("raging_barbarians");
    settings.oneCityChallenge         = rex::cvar::Query<bool>("one_city_challenge");
    settings.resources                = rex::cvar::Query<int32_t>("resources");
    settings.aiResearchBonusPercent   = rex::cvar::Query<int32_t>("ai_research_bonus_percent");
    settings.aiProductionBonusPercent = rex::cvar::Query<int32_t>("ai_production_bonus_percent");
    settings.aiGoldBonusPercent       = rex::cvar::Query<int32_t>("ai_gold_bonus_percent");
    settings.aiFoodBonusPercent       = rex::cvar::Query<int32_t>("ai_food_bonus_percent");
    settings.aiCultureBonusPercent    = rex::cvar::Query<int32_t>("ai_culture_bonus_percent");
    gameplay_fixes::ParseDominationMinimumTurns(rex::cvar::Query<std::string>("domination_min_turns"), settings.dominationMinimumTurns);
    settings.combatSpeed       = rex::cvar::Query<std::string>("combat_speed") == "fast";
    settings.gotwCustomWeek    = rex::cvar::Query<bool>("gotw_custom_week");
    settings.gotwStandardRules = rex::cvar::Query<bool>("gotw_standard_rules");
    if (settings.gotwCustomWeek)
    {
        settings.gotwCustomWeek = gotw::ParseWeek(rex::cvar::Query<std::string>("gotw_week"), settings.gotwWeek);
        settings.gotwSeed       = settings.gotwCustomWeek ? gotw::SeedForWeek(settings.gotwWeek) : 0;
    }
    settings.gotwCustomCivilization = rex::cvar::Query<bool>("gotw_custom_civilization");
    if (settings.gotwCustomCivilization)
    {
        const auto civilization         = rex::cvar::Query<int32_t>("gotw_civilization");
        settings.gotwCustomCivilization = civilization >= 0 && gotw::IsValidCivilization(static_cast<uint32_t>(civilization));
        settings.gotwCivilization       = settings.gotwCustomCivilization ? static_cast<uint32_t>(civilization) : 0;
    }
    auto metadata               = LocalModSnapshotForSettings();
    settings.mods               = std::move(metadata.mods);
    settings.rulesDigestPresent = metadata.rulesDigestPresent;
    settings.rulesDigest        = metadata.rulesDigest;
    settings.modsTruncated      = !metadata.complete;
    return settings;
}

} // namespace rerevved::host_settings

namespace rerevved::gameplay_session
{
namespace
{

// Apple libc++ has no std::atomic<std::shared_ptr>.
std::mutex                                         networkSettingsMutex;
std::shared_ptr<const host_settings::HostSettings> networkSettings;
std::recursive_mutex                               ruleBoundary;
thread_local bool                                  blocked = false;
thread_local bool                                  active  = false;

void StoreNetworkSettings(std::shared_ptr<const host_settings::HostSettings> next) noexcept
{
    std::lock_guard lock(networkSettingsMutex);
    networkSettings = std::move(next);
}

struct WeeklyAttempt
{
    bool                        active = false;
    uint32_t                    setup  = 0;
    std::optional<int32_t>      mode;
    host_settings::GameSnapshot snapshot;
    // gotw::ForGame().launch before the seed latch that follows this attempt.
    uint64_t launchBefore = 0;
};

WeeklyAttempt weeklyAttempt;
// Week latched by the weekly attempt the last accepted Game of the Week Prepare consumed; 0 otherwise.
uint32_t acceptedWeeklyWeek = 0;

host_settings::GameSnapshot captureSettings(bool networkGame)
{
    const auto local = host_settings::LocalSettings();
    if (networkGame)
    {
        rex::session_data::PrepareForGame();
        const auto status = rex::session_data::WaitForHostData(std::chrono::seconds{ 15 });
        return host_settings::ResolveForGame(true, status, local);
    }
    return host_settings::ResolveForGame(false, {}, local);
}

} // namespace

host_settings::GameSnapshot Prepare(bool networkGame, uint32_t setup, int32_t mode)
{
    return detail::Prepare(networkGame, setup, mode, captureSettings);
}

host_settings::GameSnapshot detail::Prepare(bool networkGame, uint32_t setup, int32_t mode, SnapshotProvider capture)
{
    const auto boundary = LockRuleBoundary();
    const auto kind     = ClassifySetupMode(mode);
    acceptedWeeklyWeek  = 0;
    if (kind == SetupKind::Load)
    {
        return { host_settings::SnapshotState::Missing, {}, "Loaded games use their saved gameplay settings" };
    }
    if (weeklyAttempt.active && (mode == 2 || (weeklyAttempt.setup == setup && weeklyAttempt.mode == std::optional{ mode })))
    {
        const bool matches  = weeklyAttempt.setup == setup && weeklyAttempt.mode == std::optional{ 2 } && mode == 2 && !networkGame;
        auto       snapshot = matches ? std::move(weeklyAttempt.snapshot) : host_settings::GameSnapshot{};
        // The seed latch runs between PrepareWeekly and this call and advances the launch counter by
        // one for its setup, so only that latch belongs to this attempt; an older one never does.
        const auto latched = gotw::ForGame();
        if (matches && snapshot.settings && latched.launch == weeklyAttempt.launchBefore + 1 && latched.setup == setup)
        {
            acceptedWeeklyWeek = latched.week;
        }
        weeklyAttempt = {};
        if (!matches)
        {
            StoreNetworkSettings(nullptr);
        }
        if (!snapshot.settings && snapshot.diagnostic.empty())
        {
            snapshot.diagnostic = "The weekly seed's gameplay settings could not be prepared for this setup";
        }
        return snapshot;
    }
    weeklyAttempt = {};
    StoreNetworkSettings(nullptr);
    if (kind == SetupKind::Unknown || networkGame != (kind == SetupKind::Network))
    {
        return { host_settings::SnapshotState::Missing, {}, "The current game setup mode is unavailable or unsupported" };
    }
    return capture(networkGame);
}

std::optional<host_settings::HostSettings> PrepareWeekly(uint32_t setup, std::optional<int32_t> mode) noexcept
{
    return detail::PrepareWeekly(setup, mode, captureSettings);
}

std::optional<host_settings::HostSettings> detail::PrepareWeekly(uint32_t setup, std::optional<int32_t> mode, SnapshotProvider capture) noexcept
try
{
    const auto boundary = LockRuleBoundary();
    StoreNetworkSettings(nullptr);
    weeklyAttempt              = {};
    weeklyAttempt.active       = true;
    weeklyAttempt.setup        = setup;
    weeklyAttempt.mode         = mode;
    weeklyAttempt.launchBefore = gotw::ForGame().launch;
    if (mode != std::optional{ 2 } || !capture)
    {
        return std::nullopt;
    }
    try
    {
        weeklyAttempt.snapshot = capture(false);
        Publish(weeklyAttempt.snapshot, false);
        return weeklyAttempt.snapshot.settings;
    }
    catch (...)
    {
        // Capture/publication allocation failures must stay rejected at outer setup.
        weeklyAttempt.snapshot = {};
        StoreNetworkSettings(nullptr);
        return std::nullopt;
    }
}
catch (...)
{
    return std::nullopt;
}

uint32_t AcceptedWeeklyWeek() noexcept
{
    const auto boundary = LockRuleBoundary();
    return acceptedWeeklyWeek;
}

std::unique_lock<std::recursive_mutex> LockRuleBoundary()
{
    return std::unique_lock(ruleBoundary);
}

bool RuleReplacementAllowed() noexcept
try
{
    auto*              kernel  = REX_KERNEL_STATE();
    auto*              memory  = kernel ? kernel->memory() : nullptr;
    constexpr uint32_t address = 0x8314F10B;
    auto*              heap    = memory ? memory->LookupHeap(address) : nullptr;
    const bool         network = heap &&
                                 (static_cast<uint32_t>(heap->QueryRangeAccess(address, address)) & static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly)) != 0 &&
                                 *memory->TranslateVirtual<const uint8_t*>(address) != 0;
    return rex::session_data::Current().state == rex::session_data::State::kNone ||
           (!network && !ForNetworkGame());
}
catch (...)
{
    return false;
}

void Publish(const host_settings::GameSnapshot& snapshot, bool networkGame)
{
    StoreNetworkSettings(networkGame && snapshot.settings &&
                                 (snapshot.state == host_settings::SnapshotState::Host || snapshot.state == host_settings::SnapshotState::Received)
                             ? std::make_shared<const host_settings::HostSettings>(*snapshot.settings)
                             : nullptr);
}

bool ApplyNewGame(const host_settings::GameSnapshot& snapshot, bool networkGame, int32_t mode)
{
    if (!snapshot.settings)
    {
        return false;
    }
    Publish(snapshot, networkGame);
    const auto& settings      = *snapshot.settings;
    const bool  standardRules = gotw::FreezeStandardRules(mode, settings.gotwStandardRules);
    ai_economy::FreezeForGame(standardRules);
    ai_economy::RestoreForGame(standardRules ? ai_economy::Policy{} : ai_economy::Policy{ settings.aiResearchBonusPercent, settings.aiProductionBonusPercent, settings.aiGoldBonusPercent, settings.aiFoodBonusPercent, settings.aiCultureBonusPercent });
    raging_barbarians::RestoreForGame(!standardRules && settings.ragingBarbarians);
    occ::RestoreForGame({ !standardRules && settings.oneCityChallenge, occ::kMinimumPopulation });
    // The effective AI count and the latched weekly week follow the native setup (RunNativeNewGameSetup).
    custom_achievements::BeginNewGame(!standardRules && settings.oneCityChallenge, mode == 2);
    gameplay_fixes::NoteNewGame(standardRules);
    SuspendModRulesForGame(standardRules);
    return standardRules;
}

std::shared_ptr<const host_settings::HostSettings> ForNetworkGame() noexcept
{
    std::lock_guard lock(networkSettingsMutex);
    return networkSettings;
}

void ResetOnLoad() noexcept
try
{
    // Saved policies remain authoritative on load. A new network game must
    // authenticate its own snapshot rather than reuse the previous session.
    const auto boundary = LockRuleBoundary();
    StoreNetworkSettings(nullptr);
    weeklyAttempt      = {};
    acceptedWeeklyWeek = 0;
    SuspendModRulesForGame(false);
}
catch (...)
{
    // No exception may escape a guest load boundary.
}

SetupGate::SetupGate(bool admitted, bool newGame) noexcept
: previousBlocked(std::exchange(blocked, !admitted))
, previousActive(std::exchange(active, admitted && newGame))
{
}

SetupGate::~SetupGate()
{
    blocked = previousBlocked;
    active  = previousActive;
}

bool SetupBlocked() noexcept
{
    return blocked;
}

bool SetupActive() noexcept
{
    return active;
}

} // namespace rerevved::gameplay_session
