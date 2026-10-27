#include "custom_achievements.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <mutex>

#include <rex/logging.h>
#include <rex/system/achievements.h>

namespace rerevved::custom_achievements
{
namespace
{

std::mutex mutex;
Progress   progress;

// Total 955G.
struct Entry
{
    uint32_t    id;
    const char* label;
    const char* description;
    uint32_t    gamerscore;
    uint32_t    imageId;
    const char* iconPath;
};

constexpr Entry kCatalog[] = {
    { kConcentratedPowerOfWillId, "200% Concentrated Power of Will", "Win a game on Deity with all five AI Handicaps set to 200% for the entire game.", 200, 0, "icons/200-percent-concentrated-power-of-will.png" },
    { kOneToRuleId, "One To Rule Them All", "Win a game on Deity with One City Challenge enabled.", 15, 5, "" },
    { kTheirRageId, "Their Rage Is Within Your Control", "Defeat 50 Barbarian units in a single game.", 25, 21, "" },
    { kReasonToRememberId, "100% Reason to Remember the Name", "Win a game on Deity with all five AI Handicaps set to 100% for the entire game.", 125, 0, "icons/100-percent-reason-to-remember-the-name.png" },
    { kHalcyonTemplarId, "Halcyon Templar", "Discover the Knights Templar on or before 3500 BC.", 5, 19, "" },
    { kBeyondTheFirmamentId, "Beyond the Firmament", "Launch an ICBM at a target at least 24 tiles away.", 5, 4, "" },
    { kBlockbusterChampionId, "Blockbuster World Champion", "Win a game on Deity on or before 1994 AD without building any units.", 50, 34, "" },
    { kShallowWorldviewId, "Shallow Worldview", "Win without moving units onto deep Ocean tiles.", 20, 9, "" },
    { kCosmicEclipseId, "Cosmic Eclipse", "Win a technology victory on or before 100 BC.", 75, 32, "" },
    { kArtOfTheWalkInId, "Art of the Walk In", "Capture an enemy capital city before 3500 BC on Deity.", 20, 12, "" },
    { kLastStandId, "Last Stand", "Win a battle against the odds while defending a city with a Militia.", 50, 1, "" },
    { kWeekendWarriorId, "Weekend Warrior", "Win a Game of the Week on Deity before 3000 BC.", 100, 13, "" },
    { kYeAreIdleId, "Ye Are Idle, Ye Are Idle", "Win a game on Deity after settling your first city on or after turn 100.", 75, 20, "" },
    { kLeapThroughTimeId, "Leap Through Time", "Win a Game of the Week on Week 36428.", 10, 22, "" },
    { kNoEnemiesId, "I Have No Enemies", "Win a game on Deity without participating in a single battle.", 50, 51, "" },
    { kNoMoreWorldsId, "No More Worlds To Conquer", "Win a Domination victory on Deity with 6 AI opponents enabled.", 30, 37, "" },
    { kTacticallyCenteredId, "Tactically Centered", "Win a game on Deity without building any units, with all five AI Handicaps set to 50% for the entire game.", 100, 21, "" },
};

rex::system::AchievementInfo makeInfo(const Entry& entry)
{
    rex::system::AchievementInfo info;
    info.id                     = entry.id;
    info.label                  = entry.label;
    info.description            = entry.description;
    info.unachieved_description = entry.description;
    info.icon_path              = entry.iconPath;
    info.image_id               = entry.imageId;
    info.gamerscore             = entry.gamerscore;
    info.flags                  = 0;
    info.custom                 = true;
    return info;
}

bool sdkIsUnlocked(uint32_t id)
{
    return rex::system::IsAchievementUnlocked(id);
}

bool sdkUnlock(uint32_t id)
{
    return rex::system::UnlockAchievement(id, true);
}

AwardResult awardId(uint32_t id, bool qualifies, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock)
{
    if (!qualifies)
    {
        return AwardResult::NotQualified;
    }
    if (isUnlocked(id))
    {
        return AwardResult::AlreadyUnlocked;
    }
    return unlock(id) ? AwardResult::Unlocked : AwardResult::Failed;
}

// Awards through the SDK store for an event achievement and logs the unlock.
AwardResult awardEvent(uint32_t id, bool qualifies)
{
    const auto result = awardId(id, qualifies, sdkIsUnlocked, sdkUnlock);
    if (result == AwardResult::Unlocked)
    {
        REXLOG_INFO("Custom achievement unlocked: {}", AchievementName(id));
    }
    return result;
}

// The AI Handicaps count only when every one is at percent for the whole game.
bool handicapsAt(const VictoryContext& context, int32_t percent) noexcept
{
    const auto& p = context.policy;
    return !context.nativeEconomy && p.researchBonusPercent == percent && p.productionBonusPercent == percent &&
           p.goldBonusPercent == percent && p.foodBonusPercent == percent && p.cultureBonusPercent == percent;
}

bool deity(const VictoryContext& context) noexcept
{
    return !context.networkGame && context.difficulty >= kDeityDifficulty;
}

bool yearAtMost(const VictoryContext& context, int32_t limit) noexcept
{
    return context.yearKnown && context.year <= limit;
}

bool yearBefore(const VictoryContext& context, int32_t limit) noexcept
{
    return context.yearKnown && context.year < limit;
}

bool noUnitsBuilt(const VictoryContext& context) noexcept
{
    return context.historyKnown && !context.unitBuilt;
}

struct VictoryRule
{
    uint32_t id;
    bool (*qualifies)(const VictoryContext&) noexcept;
};

constexpr VictoryRule kVictoryRules[] = {
    { kConcentratedPowerOfWillId, Qualifies },
    { kOneToRuleId, QualifiesOneToRule },
    { kReasonToRememberId, QualifiesReasonToRemember },
    { kBlockbusterChampionId, QualifiesBlockbusterChampion },
    { kShallowWorldviewId, QualifiesShallowWorldview },
    { kCosmicEclipseId, QualifiesCosmicEclipse },
    { kWeekendWarriorId, QualifiesWeekendWarrior },
    { kYeAreIdleId, QualifiesYeAreIdle },
    { kLeapThroughTimeId, QualifiesLeapThroughTime },
    { kNoEnemiesId, QualifiesNoEnemies },
    { kNoMoreWorldsId, QualifiesNoMoreWorlds },
    { kTacticallyCenteredId, QualifiesTacticallyCentered },
};

bool inMap(Tile tile) noexcept
{
    return tile.x >= 0 && tile.x < kMapSize && tile.y >= 0 && tile.y < kMapSize;
}

} // namespace

std::vector<rex::system::AchievementInfo> AllAchievements()
{
    std::vector<rex::system::AchievementInfo> all;
    all.reserve(std::size(kCatalog));
    for (const auto& entry : kCatalog)
    {
        all.push_back(makeInfo(entry));
    }
    return all;
}

rex::system::AchievementInfo ConcentratedPowerOfWill()
{
    return makeInfo(kCatalog[0]);
}

rex::system::AchievementInfo OneToRule()
{
    return makeInfo(kCatalog[1]);
}

rex::system::AchievementInfo TheirRageIsWithinYourControl()
{
    return makeInfo(kCatalog[2]);
}

const char* AchievementName(uint32_t id) noexcept
{
    for (const auto& entry : kCatalog)
    {
        if (entry.id == id)
        {
            return entry.label;
        }
    }
    return "unknown";
}

int32_t TileDistance(Tile a, Tile b) noexcept
{
    if (!inMap(a) || !inMap(b))
    {
        return -1;
    }
    const int32_t dx = std::abs(a.x - b.x);
    return std::max(std::min(dx, kMapSize - dx), std::abs(a.y - b.y));
}

bool IsDeepOcean(std::span<const uint8_t> terrain, int32_t width, int32_t height, Tile tile) noexcept
{
    if (width < 1 || width > kMapStride || height < 1 || height > kMapStride || tile.x < 0 || tile.x >= width || tile.y < 0 ||
        tile.y >= height || terrain.size() < static_cast<size_t>(kMapStride) * kMapStride)
    {
        return false;
    }
    const auto at = [&](int32_t x, int32_t y)
    {
        return terrain[static_cast<size_t>(kMapStride * x + y)];
    };
    if (at(tile.x, tile.y) != 0)
    {
        return false;
    }
    for (int32_t dx = -1; dx <= 1; ++dx)
    {
        for (int32_t dy = -1; dy <= 1; ++dy)
        {
            const int32_t y = tile.y + dy;
            if ((dx == 0 && dy == 0) || y < 0 || y >= height)
            {
                continue;
            }
            if (at((tile.x + dx + width) % width, y) != 0)
            {
                return false;
            }
        }
    }
    return true;
}

bool AgainstTheOdds(int64_t attack, int64_t defense) noexcept
{
    return attack > defense;
}

bool Qualifies(const VictoryContext& context) noexcept
{
    return deity(context) && handicapsAt(context, ai_economy::kMaxBonusPercent);
}

bool QualifiesOneToRule(const VictoryContext& context) noexcept
{
    return deity(context) && context.occEnabled && context.occFromSetup;
}

bool QualifiesReasonToRemember(const VictoryContext& context) noexcept
{
    return deity(context) && handicapsAt(context, 100);
}

bool QualifiesBlockbusterChampion(const VictoryContext& context) noexcept
{
    return deity(context) && noUnitsBuilt(context) && yearAtMost(context, kBlockbusterLastYear);
}

bool QualifiesShallowWorldview(const VictoryContext& context) noexcept
{
    return !context.networkGame && context.historyKnown && !context.deepOceanEntered;
}

bool QualifiesCosmicEclipse(const VictoryContext& context) noexcept
{
    return !context.networkGame && context.victoryType == 2 && yearAtMost(context, kCosmicEclipseLastYear);
}

bool QualifiesWeekendWarrior(const VictoryContext& context) noexcept
{
    return deity(context) && context.gotwGame && yearBefore(context, kWeekendWarriorFirstYear);
}

bool QualifiesYeAreIdle(const VictoryContext& context) noexcept
{
    return deity(context) && context.historyKnown && context.firstFoundingTurn >= kLateFoundingTurn;
}

bool QualifiesLeapThroughTime(const VictoryContext& context) noexcept
{
    return !context.networkGame && context.gotwGame && context.gotwWeek == kLeapWeek;
}

bool QualifiesNoEnemies(const VictoryContext& context) noexcept
{
    return deity(context) && context.historyKnown && !context.battleFought;
}

bool QualifiesNoMoreWorlds(const VictoryContext& context) noexcept
{
    return deity(context) && context.victoryType == 1 && context.initialAiOpponents == kNoMoreWorldsOpponents;
}

bool QualifiesTacticallyCentered(const VictoryContext& context) noexcept
{
    return deity(context) && noUnitsBuilt(context) && handicapsAt(context, 50);
}

bool Register()
{
    bool all = true;
    for (const auto& info : AllAchievements())
    {
        all = rex::system::RegisterAchievement(info) && all;
    }
    return all;
}

AwardResult Award(const VictoryContext& context, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock)
{
    return awardId(kConcentratedPowerOfWillId, Qualifies(context), isUnlocked, unlock);
}

AwardResult Award(const VictoryContext& context)
{
    return Award(context, sdkIsUnlocked, sdkUnlock);
}

AwardResult AwardOneToRule(const VictoryContext& context, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock)
{
    return awardId(kOneToRuleId, QualifiesOneToRule(context), isUnlocked, unlock);
}

AwardResult AwardOneToRule(const VictoryContext& context)
{
    return AwardOneToRule(context, sdkIsUnlocked, sdkUnlock);
}

std::vector<VictoryDecision> AwardVictory(const VictoryContext& context, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock)
{
    std::vector<VictoryDecision> decisions;
    decisions.reserve(std::size(kVictoryRules));
    for (const auto& rule : kVictoryRules)
    {
        const bool qualifies = rule.qualifies(context);
        decisions.push_back({ rule.id, qualifies, awardId(rule.id, qualifies, isUnlocked, unlock) });
    }
    return decisions;
}

std::vector<VictoryDecision> AwardVictory(const VictoryContext& context)
{
    return AwardVictory(context, sdkIsUnlocked, sdkUnlock);
}

void BeginNewGame(bool occEnabled, bool gotwGame)
{
    std::lock_guard lock(mutex);
    progress              = {};
    progress.occFromSetup = occEnabled;
    progress.historyKnown = true;
    progress.gotwGame     = gotwGame;
}

void RecordSetupOutcome(int32_t aiOpponents, uint32_t gotwWeek)
{
    std::lock_guard lock(mutex);
    if (!progress.historyKnown)
    {
        return;
    }
    progress.initialAiOpponents = aiOpponents >= 0 && aiOpponents <= kMaximumAiOpponents ? aiOpponents : -1;
    progress.gotwWeek           = progress.gotwGame ? gotwWeek : 0;
}

void ResetProgress()
{
    std::lock_guard lock(mutex);
    progress = {};
}

Progress CurrentProgress()
{
    std::lock_guard lock(mutex);
    return progress;
}

void RestoreProgress(Progress restored)
{
    restored.barbarianDefeats  = std::clamp(restored.barbarianDefeats, 0, kBarbarianDefeatTarget);
    restored.firstFoundingTurn = restored.firstFoundingTurn < 0 ? -1 : std::min(restored.firstFoundingTurn, kMaximumRecordedTurn);
    if (restored.initialAiOpponents < 0 || restored.initialAiOpponents > kMaximumAiOpponents)
    {
        restored.initialAiOpponents = -1;
    }
    restored.gotwGame = false;
    restored.gotwWeek = 0;
    std::lock_guard lock(mutex);
    progress = restored;
}

void RecordUnitBuilt() noexcept
{
    std::lock_guard lock(mutex);
    progress.unitBuilt = progress.unitBuilt || progress.historyKnown;
}

void RecordBattle() noexcept
{
    std::lock_guard lock(mutex);
    progress.battleFought = progress.battleFought || progress.historyKnown;
}

void RecordDeepOceanEntry() noexcept
{
    std::lock_guard lock(mutex);
    progress.deepOceanEntered = progress.deepOceanEntered || progress.historyKnown;
}

void RecordCityFounded(int32_t turn) noexcept
{
    std::lock_guard lock(mutex);
    if (progress.historyKnown && turn >= 0 && progress.firstFoundingTurn < 0)
    {
        progress.firstFoundingTurn = std::min(turn, kMaximumRecordedTurn);
    }
}

AwardResult RecordTemplarDiscovery(int32_t year, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock)
{
    return awardId(kHalcyonTemplarId, year <= kTemplarLastYear, isUnlocked, unlock);
}

AwardResult RecordTemplarDiscovery(int32_t year)
{
    return awardEvent(kHalcyonTemplarId, year <= kTemplarLastYear);
}

AwardResult RecordIcbmLaunch(Tile source, Tile target, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock)
{
    return awardId(kBeyondTheFirmamentId, TileDistance(source, target) >= kIcbmMinimumDistance, isUnlocked, unlock);
}

AwardResult RecordIcbmLaunch(Tile source, Tile target)
{
    return awardEvent(kBeyondTheFirmamentId, TileDistance(source, target) >= kIcbmMinimumDistance);
}

AwardResult RecordCapitalCapture(int32_t year, int32_t difficulty, bool networkGame, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock)
{
    return awardId(kArtOfTheWalkInId, year < kWalkInFirstYear && difficulty >= kDeityDifficulty && !networkGame, isUnlocked, unlock);
}

AwardResult RecordCapitalCapture(int32_t year, int32_t difficulty, bool networkGame)
{
    return awardEvent(kArtOfTheWalkInId, year < kWalkInFirstYear && difficulty >= kDeityDifficulty && !networkGame);
}

AwardResult RecordLastStand(int64_t attack, int64_t defense, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock)
{
    return awardId(kLastStandId, AgainstTheOdds(attack, defense), isUnlocked, unlock);
}

AwardResult RecordLastStand(int64_t attack, int64_t defense)
{
    return awardEvent(kLastStandId, AgainstTheOdds(attack, defense));
}

bool CountsAsBarbarianDefeat(const DefeatEvent& event) noexcept
{
    return event.combatSite && event.participantsMatch && event.barbarian >= 0 && event.loser == event.barbarian &&
           event.local >= 0 && event.winner == event.local && event.winner != event.loser && !event.killedBefore &&
           event.killedAfter;
}

AwardResult RecordBarbarianDefeat(const IsUnlockedFn& isUnlocked, const UnlockFn& unlock)
{
    {
        // Saturated progress still retries the unlock, e.g. a 50-count save
        // loaded under a profile that has not unlocked it.
        std::lock_guard lock(mutex);
        progress.barbarianDefeats = std::min(progress.barbarianDefeats + 1, kBarbarianDefeatTarget);
        if (progress.barbarianDefeats < kBarbarianDefeatTarget)
        {
            return AwardResult::NotQualified;
        }
    }
    return awardId(kTheirRageId, true, isUnlocked, unlock);
}

AwardResult RecordBarbarianDefeat()
{
    return RecordBarbarianDefeat(sdkIsUnlocked, sdkUnlock);
}

} // namespace rerevved::custom_achievements
