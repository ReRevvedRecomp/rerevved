#include "start_resources_runtime.h"

#include "ai_civilization_setup.h"
#include "ai_civilizations.h"
#include "ai_economy_settings.h"
#include "custom_achievement_combat_hooks.h"
#include "custom_achievements.h"
#include "gameplay_fixes.h"
#include "gameplay_session_settings.h"
#include "gotw_settings.h"
#include "mod_script_rules.h"
#include "occ_settings.h"
#include "raging_barbarians_settings.h"
#include "resource_identity_overrides.h"
#include "start_resources.h"

#include <atomic>
#include <bit>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/notifications.h>

// Preserve serialized enum values; Abundant remains reserved.
REXCVAR_DEFINE_INT32(resources, 0, "ReRevved", "Resources for the next new game: 0 Normal, 1 Balanced, 2 Abundant (reserved), 3 Legendary")
    .range(0, 3);
REXCVAR_DEFINE_BOOL(resource_settings_diagnostics, false, "ReRevved", "Log new-game resource anchors, ring terrain mix, choices, overrides and final readback");
REXCVAR_DECLARE(int32_t, ai_civilizations);

REX_EXTERN(__imp__sub_82D7D950);
REX_EXTERN(__imp__sub_82D25170);
REX_EXTERN(__imp__sub_82D217C8);

namespace rerevved::start_resources
{
namespace
{

constexpr uint32_t kBoundsA       = 0x82F77364;
constexpr uint32_t kBoundsB       = 0x82F77448;
constexpr uint32_t kAnchorA       = 0x830E345C;
constexpr uint32_t kAnchorB       = 0x830E343C;
constexpr uint32_t kCivilizations = 0x830ECD28;
constexpr uint32_t kFlags         = 0x8312A608;
constexpr uint32_t kTerrain       = 0x8312B288;
constexpr uint32_t kSetupObject   = 0x8314EFC8;
constexpr uint32_t kReceiver      = 0x8314F76C;
constexpr uint32_t kVtable        = 0x82138768;
constexpr uint32_t kRefresh       = 0x82D7D950;

thread_local std::optional<resource_settings::FrozenSelection> pendingSetup;
std::atomic<int32_t>                                           frozenResources{ 0 };

class SetupScope
{
public:
    explicit SetupScope(bool standardRules, int32_t requested)
    : previous_(std::exchange(pendingSetup, std::nullopt))
    {
        resource_identity_overrides::Clear();
        const int32_t value = standardRules ? 0 : requested;
        frozenResources.store(static_cast<int32_t>(resource_settings::FromConfig(value)));
        pendingSetup.emplace(value);
    }

    ~SetupScope()
    {
        pendingSetup = std::move(previous_);
    }

private:
    std::optional<resource_settings::FrozenSelection> previous_;
};

uint16_t loadU16(const uint8_t* base, uint32_t address)
{
    return static_cast<uint16_t>(uint16_t{ base[address] } << 8 | base[address + 1]);
}

uint32_t loadU32(const uint8_t* base, uint32_t address)
{
    return uint32_t{ base[address] } << 24 | uint32_t{ base[address + 1] } << 16 |
           uint32_t{ base[address + 2] } << 8 | uint32_t{ base[address + 3] };
}

class GuestWorld final : public World
{
public:
    GuestWorld(PPCContext& context, uint8_t* base)
    : context_(context)
    , base_(base)
    {
    }

    bool Capture(Snapshot& snapshot) override
    {
        namespace ai             = ai_civilizations;
        const uint32_t civilized = ai::CivilizationMask(static_cast<int32_t>(loadU32(base_, ai::kPlayerCountGlobal)),
                                                        static_cast<int32_t>(loadU32(base_, ai::kBarbarianPlayerGlobal)));
        snapshot.bounds          = { static_cast<int>(loadU32(base_, kBoundsA)), static_cast<int>(loadU32(base_, kBoundsB)) };
        if (!snapshot.bounds.Valid() || civilized == 0 || !refreshBound())
        {
            return false;
        }
        // 0x82D0F11C/0x82D0F13C reads this setup seed before the native RNG
        // store at 0x82D0F164. Use the seed directly, including named maps.
        const uint32_t setup = loadU32(base_, kSetupObject);
        if (setup == 0 || setup > UINT32_MAX - 0x13C)
        {
            return false;
        }
        snapshot.seed              = loadU32(base_, setup + 0x138);
        snapshot.civilizationCount = static_cast<size_t>(std::popcount(civilized));
        snapshot.players           = {};
        for (size_t player = 0; player < snapshot.civilizationCount; ++player)
        {
            auto& entry        = snapshot.players[player];
            entry.anchor       = { static_cast<int>(loadU32(base_, kAnchorA + 4 * static_cast<uint32_t>(player))),
                                   static_cast<int>(loadU32(base_, kAnchorB + 4 * static_cast<uint32_t>(player))) };
            entry.civilization = static_cast<int>(loadU32(base_, kCivilizations + 4 * static_cast<uint32_t>(player)));
            for (uint32_t unit = 0; unit < ai::kUnitsPerPlayer; ++unit)
            {
                const uint32_t record = ai::UnitRecord(static_cast<uint32_t>(player), unit);
                const Coord    coordinate{ std::bit_cast<int16_t>(loadU16(base_, record + 0x1C)),
                                           std::bit_cast<int16_t>(loadU16(base_, record + 0x1E)) };
                if (IsLiveSettler(static_cast<uint32_t>(player), base_[record], base_[record + 1], loadU32(base_, record + 0x0C), coordinate, entry.anchor))
                {
                    entry.liveSettler = true;
                    break;
                }
            }
        }
        for (int a = 0; a < snapshot.bounds.a; ++a)
        {
            for (int b = 0; b < snapshot.bounds.b; ++b)
            {
                snapshot.tiles[Index({ a, b })] = Read({ a, b });
            }
        }
        return true;
    }

    Tile Read(Coord coordinate) override
    {
        const auto index = static_cast<uint32_t>(Index(coordinate));
        return { loadU16(base_, kFlags + 2 * index), std::bit_cast<int8_t>(base_[kTerrain + index]) };
    }

    void WriteFlags(Coord coordinate, uint16_t flags) override
    {
        const auto index              = static_cast<uint32_t>(Index(coordinate));
        base_[kFlags + 2 * index]     = static_cast<uint8_t>(flags >> 8);
        base_[kFlags + 2 * index + 1] = static_cast<uint8_t>(flags);
    }

    bool Refresh(Coord coordinate) override
    {
        if (!refreshBound())
        {
            return false;
        }
        PPCContext call = context_;
        call.r3.u64     = kReceiver;
        call.r4.u64     = static_cast<uint32_t>(coordinate.a);
        call.r5.u64     = static_cast<uint32_t>(coordinate.b);
        __imp__sub_82D7D950(call, base_);
        context_.fpscr = call.fpscr;
        // The callback's return register is not a placement result.
        return true;
    }

private:
    bool refreshBound() const
    {
        return loadU32(base_, kReceiver) == kVtable && loadU32(base_, kVtable + 0xF4) == kRefresh;
    }

    PPCContext& context_;
    uint8_t*    base_;
};

// Ordinary AI civilizations in the final layout, after the AI limit removed its excess; -1 when the
// layout or human mask is unsupported. An expansion the setup rejected leaves the native layout.
int32_t effectiveAiOpponents(const uint8_t* base)
{
    namespace civs           = ai_civilizations;
    const auto     count     = static_cast<int32_t>(loadU32(base, civs::kPlayerCountGlobal));
    const auto     barbarian = static_cast<int32_t>(loadU32(base, civs::kBarbarianPlayerGlobal));
    const uint32_t humans    = loadU32(base, civs::kHumanMaskGlobal);
    const uint32_t civilized = civs::CivilizationMask(count, barbarian);
    if (civilized == 0 || humans == 0 || (humans & ~civilized) != 0)
    {
        return -1;
    }
    int32_t opponents = 0;
    for (int32_t player = 0; player < barbarian; ++player)
    {
        opponents += civs::IsOrdinaryAi(count, barbarian, humans, player) && !civs::IsRemovedCivilization(static_cast<uint32_t>(player));
    }
    return opponents;
}

} // namespace

bool SuppressIndiaStartingResource()
{
    return pendingSetup && pendingSetup->SuppressIndiaStartingResource();
}

void ResetOnLoad()
{
    resource_identity_overrides::Clear();
    pendingSetup.reset();
    frozenResources.store(static_cast<int32_t>(resource_settings::FromConfig(REXCVAR_GET(resources))));
}

int32_t ForGame()
{
    return frozenResources.load();
}

void RestoreForGame(int32_t resources)
{
    frozenResources.store(resources);
}

void OnNewGameSetup(PPCContext& context, uint8_t* base)
{
    if (!pendingSetup)
    {
        return;
    }
    GuestWorld   world(context, base);
    const Result result = Apply(*pendingSetup, world);
    if (result.hardFailure)
    {
        REXLOG_ERROR("Resources: {} failed; original flags restored without terrain stores, resource objects and caches may be stale", resource_settings::Name(result.mode));
        return;
    }
    if (result.mode == resource_settings::Mode::Normal)
    {
        return;
    }
    if (result.mode != resource_settings::Mode::Balanced && result.mode != resource_settings::Mode::Legendary)
    {
        REXLOG_WARN("Resources: {} is reserved; native map retained", resource_settings::Name(result.mode));
        return;
    }
    if (!result.captured)
    {
        REXLOG_WARN("Resources: {} rejected before writes (map bounds, player layout, setup seed or refresh binding)", resource_settings::Name(result.mode));
        return;
    }
    for (size_t player = 0; player < kPlayers; ++player)
    {
        const auto& report = result.players[player];
        if (report.status == Status::NotRun)
        {
            continue;
        }
        if (result.mode == resource_settings::Mode::Legendary)
        {
            if (report.status != Status::Verified || REXCVAR_GET(resource_settings_diagnostics))
            {
                REXLOG_INFO("Resources: Legendary player={} anchor=({}, {}) ring={} status={} rollback={} rollback_verified={}",
                            player,
                            report.anchor.a,
                            report.anchor.b,
                            report.trio.mix.count,
                            Name(report.status),
                            result.rolledBack,
                            result.rollbackVerified);
            }
            for (const auto& placement : report.legendary)
            {
                if (Bounds{ 32, 32 }.Contains(placement.tile) &&
                    (placement.skip != SkipReason::None || REXCVAR_GET(resource_settings_diagnostics)))
                {
                    REXLOG_INFO("Resources: Legendary player={} tile=({}, {}) terrain={} ordinal={} override={} skip={} final={} status={}",
                                player,
                                placement.tile.a,
                                placement.tile.b,
                                placement.terrain,
                                placement.ordinal,
                                placement.needsOverride,
                                Name(placement.skip),
                                placement.finalIdentity,
                                Name(report.status));
                }
            }
            continue;
        }
        if (report.trio.valid)
        {
            for (size_t resource = 0; resource < kBalancedResources.size(); ++resource)
            {
                if (!Bounds{ 32, 32 }.Contains(report.trio.placements[resource].tile))
                {
                    REXLOG_WARN("Resources: Balanced player={} skipped ordinal={} (no eligible existing terrain tile)", player, kBalancedResources[resource]);
                }
            }
        }
        if (report.status != Status::Verified)
        {
            REXLOG_WARN("Resources: Balanced player={} anchor=({}, {}) ring={} terrain_mix[0..6]=[{},{},{},{},{},{},{}] other={} failure={} rollback={} rollback_verified={}",
                        player,
                        report.anchor.a,
                        report.anchor.b,
                        report.trio.mix.count,
                        report.trio.mix.terrain[0],
                        report.trio.mix.terrain[1],
                        report.trio.mix.terrain[2],
                        report.trio.mix.terrain[3],
                        report.trio.mix.terrain[4],
                        report.trio.mix.terrain[5],
                        report.trio.mix.terrain[6],
                        report.trio.mix.other,
                        Name(report.status),
                        result.rolledBack,
                        result.rollbackVerified);
        }
        if (report.trio.valid || REXCVAR_GET(resource_settings_diagnostics))
        {
            REXLOG_INFO("Resources: Balanced player={} anchor=({}, {}) ring={} terrain_mix[0..6]=[{},{},{},{},{},{},{}] other={} Oxen=({}, {}) override={} Oak=({}, {}) override={} Fish=({}, {}) override={} final=({},{},{}) status={}",
                        player,
                        report.anchor.a,
                        report.anchor.b,
                        report.trio.mix.count,
                        report.trio.mix.terrain[0],
                        report.trio.mix.terrain[1],
                        report.trio.mix.terrain[2],
                        report.trio.mix.terrain[3],
                        report.trio.mix.terrain[4],
                        report.trio.mix.terrain[5],
                        report.trio.mix.terrain[6],
                        report.trio.mix.other,
                        report.trio.placements[0].tile.a,
                        report.trio.placements[0].tile.b,
                        report.trio.placements[0].needsOverride,
                        report.trio.placements[1].tile.a,
                        report.trio.placements[1].tile.b,
                        report.trio.placements[1].needsOverride,
                        report.trio.placements[2].tile.a,
                        report.trio.placements[2].tile.b,
                        report.trio.placements[2].needsOverride,
                        report.finalIdentities[0],
                        report.finalIdentities[1],
                        report.finalIdentities[2],
                        Name(report.status));
        }
    }
}

void RunNativeNewGameSetup(PPCContext& context, uint8_t* base)
{
    // sub_82D25170 setup at 0x82D25254 bypasses recount at 0x82D25260 on -1;
    // mode 0x82E77030 precedes seed 0x82E76ED4 and init 0x82E610A0.
    const uint32_t setup = loadU32(base, kSetupObject);
    const int32_t  mode  = setup != 0 && setup <= UINT32_MAX - 4 ? static_cast<int32_t>(loadU32(base, setup + 4)) : 0;
    const auto     kind  = gameplay_session::ClassifySetupMode(mode);
    if (kind == gameplay_session::SetupKind::Load)
    {
        gameplay_session::SetupGate gate(true, false);
        __imp__sub_82D25170(context, base);
        return;
    }
    const auto boundary = gameplay_session::LockRuleBoundary();
    // Restore first so a failed restore rejects the snapshot in Prepare.
    SuspendModRulesForGame(false);
    // A rejected setup must not inherit the previous game's progress.
    custom_achievements::ResetProgress();
    custom_achievement_combat::ResetCombatObservers();
    const bool                  network  = kind == gameplay_session::SetupKind::Network;
    const auto                  snapshot = gameplay_session::Prepare(network, setup, mode);
    gameplay_session::SetupGate gate(snapshot.settings.has_value());
    if (!snapshot.settings)
    {
        REXLOG_ERROR("Gameplay settings: setup rejected: {}", snapshot.diagnostic);
        rex::ui::PostNotification("Cannot start this game", snapshot.diagnostic);
        __imp__sub_82D25170(context, base);
        return;
    }
    const auto& settings      = *snapshot.settings;
    const bool  standardRules = gameplay_session::ApplyNewGame(snapshot, network, mode);
    // The seed latch (0x82E76ED4) precedes this outer setup; only the latch of the attempt Prepare consumed counts.
    const uint32_t weeklyWeek = mode == 2 ? gameplay_session::AcceptedWeeklyWeek() : 0;
    REXLOG_INFO("GameplayFix: new game domination minimum={}",
                standardRules ? gameplay_fixes::kNativeDominationMinimumTurns : settings.dominationMinimumTurns);
    SetupScope scope(standardRules, settings.resources);

    ai_civilizations::SetupScope civilizations(settings.aiCivilizations, standardRules, base, mode);
    if (mode == 2)
    {
        REXLOG_INFO("Game of the Week: standard_rules={}", standardRules);
    }
    __imp__sub_82D25170(context, base);
    ai_civilizations::CommitNewGame();
    // The AI count is final only after the native setup; the week was bound to this accepted attempt.
    custom_achievements::RecordSetupOutcome(effectiveAiOpponents(base), weeklyWeek);
}

} // namespace rerevved::start_resources

REX_HOOK_RAW(sub_82D25170)
{
    rerevved::start_resources::RunNativeNewGameSetup(ctx, base);
}

REX_HOOK_RAW(sub_82D217C8)
{
    if (rerevved::gameplay_session::SetupBlocked())
    {
        ctx.r3.s64 = -1;
        return;
    }
    __imp__sub_82D217C8(ctx, base);
}
