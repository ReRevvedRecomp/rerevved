#pragma once

#include "ai_economy_settings.h"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include <rex/system/achievement_store.h>

namespace rerevved::custom_achievements
{

// XDBF achievement IDs are 16-bit, so these cannot collide with a native one.
constexpr uint32_t kConcentratedPowerOfWillId = 0x10001;
constexpr uint32_t kOneToRuleId               = 0x10002;
constexpr uint32_t kTheirRageId               = 0x10003;
constexpr uint32_t kReasonToRememberId        = 0x10004;
constexpr uint32_t kHalcyonTemplarId          = 0x10005;
constexpr uint32_t kBeyondTheFirmamentId      = 0x10006;
constexpr uint32_t kBlockbusterChampionId     = 0x10007;
constexpr uint32_t kShallowWorldviewId        = 0x10008;
constexpr uint32_t kCosmicEclipseId           = 0x10009;
constexpr uint32_t kArtOfTheWalkInId          = 0x1000A;
constexpr uint32_t kLastStandId               = 0x1000B;
constexpr uint32_t kWeekendWarriorId          = 0x1000C;
constexpr uint32_t kYeAreIdleId               = 0x1000D;
constexpr uint32_t kLeapThroughTimeId         = 0x1000E;
constexpr uint32_t kNoEnemiesId               = 0x1000F;
constexpr uint32_t kNoMoreWorldsId            = 0x10010;
constexpr uint32_t kTacticallyCenteredId      = 0x10011;

constexpr int32_t kDeityDifficulty       = 4;
constexpr int32_t kBarbarianDefeatTarget = 50;

// Thresholds. Years are signed calendar years (negative is BC).
constexpr int32_t  kTemplarLastYear         = -3500; // on or before
constexpr int32_t  kIcbmMinimumDistance     = 24;    // tiles, at least
constexpr int32_t  kBlockbusterLastYear     = 1994;  // on or before
constexpr int32_t  kCosmicEclipseLastYear   = -100;  // on or before
constexpr int32_t  kWalkInFirstYear         = -3500; // capture strictly before
constexpr int32_t  kWeekendWarriorFirstYear = -3000; // win strictly before
constexpr int32_t  kLateFoundingTurn        = 100;   // first city on or after
constexpr uint32_t kLeapWeek                = 36428;
constexpr int32_t  kNoMoreWorldsOpponents   = 6;

// Persisted per-game fields stay inside these ranges so a sidecar always validates.
constexpr int32_t kMaximumRecordedTurn = 0xFFFE;
constexpr int32_t kMaximumAiOpponents  = 6;

constexpr int32_t kMapSize   = 32; // Map side in tiles. x wraps.
constexpr int32_t kMapStride = 40; // Terrain plane index is kMapStride * x + y.

// Metadata for every registered achievement, in ID order.
std::vector<rex::system::AchievementInfo> AllAchievements();
rex::system::AchievementInfo              ConcentratedPowerOfWill();
rex::system::AchievementInfo              OneToRule();
rex::system::AchievementInfo              TheirRageIsWithinYourControl();
// Label for an ID in AllAchievements(), or "unknown" otherwise.
const char* AchievementName(uint32_t id) noexcept;

struct Tile
{
    int32_t x = -1;
    int32_t y = -1;
};

// Pure helpers.
// Chebyshev distance on the 32x32 cylinder: x wraps, y does not. -1 if either tile is outside 0..31.
int32_t TileDistance(Tile a, Tile b) noexcept;
// Read-only deep Ocean test (RVA-F-0358 currentTerrainClassifier). terrain is the
// 1600-byte plane at 0x8312B288 indexed 40*x+y. Current width/height are 1..40.
// True iff terrain[I]==0 and all 8 neighbours are 0, with x wrapped mod width
// and y admitted only when 0<=y+dy<height. False for (x,y) out of bounds or a short span.
bool IsDeepOcean(std::span<const uint8_t> terrain, int32_t width, int32_t height, Tile tile) noexcept;
// Last Stand strength rule: strictly greater.
bool AgainstTheOdds(int64_t attack, int64_t defense) noexcept;

// State of one local victory, read before the native handler runs.
struct VictoryContext
{
    int32_t            victoryType   = 0;
    int32_t            difficulty    = 0;
    bool               networkGame   = false;
    bool               nativeEconomy = false;
    ai_economy::Policy policy;
    // The frozen One City Challenge policy and whether it was enabled at game
    // setup rather than switched on by a load without a sidecar.
    bool occEnabled   = false;
    bool occFromSetup = false;
    // Signed year at victory. An unreadable year fails every predicate that checks it.
    bool    yearKnown = false;
    int32_t year      = 0;
    // Progress fields. historyKnown is false for games without tracked history.
    bool     historyKnown       = false;
    bool     unitBuilt          = false;
    bool     battleFought       = false;
    bool     deepOceanEntered   = false;
    int32_t  firstFoundingTurn  = -1;
    int32_t  initialAiOpponents = -1;
    bool     gotwGame           = false;
    uint32_t gotwWeek           = 0;
};

// 200% Concentrated Power of Will.
bool Qualifies(const VictoryContext& context) noexcept;

// One To Rule Them All is independent of Qualifies, so one win may satisfy both.
bool QualifiesOneToRule(const VictoryContext& context) noexcept;
bool QualifiesReasonToRemember(const VictoryContext& context) noexcept;
bool QualifiesBlockbusterChampion(const VictoryContext& context) noexcept;
bool QualifiesShallowWorldview(const VictoryContext& context) noexcept;
bool QualifiesCosmicEclipse(const VictoryContext& context) noexcept;
bool QualifiesWeekendWarrior(const VictoryContext& context) noexcept;
bool QualifiesYeAreIdle(const VictoryContext& context) noexcept;
bool QualifiesLeapThroughTime(const VictoryContext& context) noexcept;
bool QualifiesNoEnemies(const VictoryContext& context) noexcept;
bool QualifiesNoMoreWorlds(const VictoryContext& context) noexcept;
bool QualifiesTacticallyCentered(const VictoryContext& context) noexcept;

// Registers the achievements with the SDK catalog. Returns false unless every
// one registers, such as when no runtime is live.
bool Register();

enum class AwardResult
{
    NotQualified,
    AlreadyUnlocked,
    Unlocked,
    Failed,
};

using IsUnlockedFn = std::function<bool(uint32_t)>;
using UnlockFn     = std::function<bool(uint32_t)>;

// The unlock store is injected so tests need no kernel.
AwardResult Award(const VictoryContext& context, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock);

// Awards through the SDK unlock store and shows the toast.
AwardResult Award(const VictoryContext& context);

AwardResult AwardOneToRule(const VictoryContext& context, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock);

AwardResult AwardOneToRule(const VictoryContext& context);

// One victory-time decision for one achievement.
struct VictoryDecision
{
    uint32_t    id        = 0;
    bool        qualifies = false;
    AwardResult result    = AwardResult::NotQualified;
};

// Evaluates every victory predicate (all but the event and Barbarian ones), in ID order, and
// unlocks each qualifying achievement once. Does not log.
std::vector<VictoryDecision> AwardVictory(const VictoryContext& context, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock);

std::vector<VictoryDecision> AwardVictory(const VictoryContext& context);

// Per-game progress saved in the gameplay sidecar. gotwGame and gotwWeek are
// not saved: Game of the Week saving is disabled.
struct Progress
{
    // OCC was on when the game was set up, not switched on mid-game.
    bool    occFromSetup     = false;
    int32_t barbarianDefeats = 0;
    // Tracking began at this game's accepted setup. False for loads without a schema 2 sidecar.
    bool historyKnown     = false;
    bool unitBuilt        = false;
    bool battleFought     = false;
    bool deepOceanEntered = false;
    // Native turn of the first city, or -1 until one is founded.
    int32_t firstFoundingTurn = -1;
    // Effective AI opponents at setup, excluding humans and Barbarians. -1 means unknown.
    int32_t  initialAiOpponents                = -1;
    bool     gotwGame                          = false;
    uint32_t gotwWeek                          = 0; // 0 until the launch's weekly seed is latched.
    bool     operator==(const Progress&) const = default;
};

// Starts tracking a new game: counters and history cleared, historyKnown set.
void BeginNewGame(bool occEnabled, bool gotwGame);
// Setup facts known only after the native setup has run: the effective AI opponent count
// (-1 unknown) and the latched Game of the Week week (0 unknown). No-op when untracked.
void RecordSetupOutcome(int32_t aiOpponents, uint32_t gotwWeek);
// Everything unknown: history untracked, AI count -1, no Game of the Week.
void     ResetProgress();
Progress CurrentProgress();
// Clamps the defeat count, founding turn and AI count to their ranges and always clears the
// Game of the Week fields.
void RestoreProgress(Progress progress);

// Per-game disqualifiers and observations. Thread-safe. Each is a no-op when
// historyKnown is false.
void RecordUnitBuilt() noexcept;
void RecordBattle() noexcept;
void RecordDeepOceanEntry() noexcept;
void RecordCityFounded(int32_t turn) noexcept; // keeps only the first, turn >= 0

// Event achievements award immediately through the SDK and log "Custom achievement unlocked: <name>".
AwardResult RecordTemplarDiscovery(int32_t year, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock);
AwardResult RecordTemplarDiscovery(int32_t year); // year <= -3500
AwardResult RecordIcbmLaunch(Tile source, Tile target, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock);
AwardResult RecordIcbmLaunch(Tile source, Tile target); // TileDistance >= 24
AwardResult RecordCapitalCapture(int32_t year, int32_t difficulty, bool networkGame, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock);
AwardResult RecordCapitalCapture(int32_t year, int32_t difficulty, bool networkGame); // year < -3500, D>=4, !network
AwardResult RecordLastStand(int64_t attack, int64_t defense, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock);
AwardResult RecordLastStand(int64_t attack, int64_t defense); // AgainstTheOdds

// One native kill (sub_82D02390 with mode 0) as the hook observed it.
struct DefeatEvent
{
    bool    combatSite        = false;
    bool    participantsMatch = false;
    int32_t loser             = -1;
    int32_t winner            = -1;
    int32_t local             = -1;
    int32_t barbarian         = -1;
    bool    killedBefore      = false;
    bool    killedAfter       = false;
};

// True only for a combat defeat of a Barbarian unit by the local player that
// turned the unit from alive to killed.
bool CountsAsBarbarianDefeat(const DefeatEvent& event) noexcept;

// Counts one defeat, saturating at kBarbarianDefeatTarget. Below the target it
// returns NotQualified. At the target every defeat checks the unlock store.
AwardResult RecordBarbarianDefeat(const IsUnlockedFn& isUnlocked, const UnlockFn& unlock);

AwardResult RecordBarbarianDefeat();

} // namespace rerevved::custom_achievements
