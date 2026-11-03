#include "custom_achievements.h"

#include "gameplay_fixes.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string_view>

#include <rex/logging.h>
#include <rex/system/achievements.h>

namespace rerevved::custom_achievements
{
namespace
{

std::mutex mutex;
Progress   progress;

// Total 1715G.
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
    { kConcentratedPowerOfWillId, "200% Concentrated Power of Will", "Win a game on Deity with all five AI Handicaps set to 200% for the entire game.", 250, 21, "fpk:Common0.FPK/gov_revolution.dds" },
    { kOneToRuleId, "One To Rule Them All", "Win a game on Deity with One City Challenge enabled.", 15, 5, "fpk:pedia.FPK/bldg_palace.dds" },
    { kTheirRageId, "Their Rage Is Within Your Control", "Defeat 50 Barbarian units in a single game.", 25, 21, "fpk:Level.FPK/unit_barbarian.dds" },
    { kReasonToRememberId, "100% Reason to Remember the Name", "Win a game on Deity with all five AI Handicaps set to 100% for the entire game.", 125, 21, "fpk:pedia.FPK/gov_fundamentalism.dds" },
    { kHalcyonTemplarId, "Halcyon Templar", "Discover the Knights Templar on or before 3500 BC.", 5, 19, "fpk:pedia.FPK/rel_templar.dds" },
    { kBeyondTheFirmamentId, "Beyond the Firmament", "Launch an ICBM at a target at least 24 tiles away.", 5, 4, "fpk:pedia.FPK/unit_icbm.dds" },
    { kBlockbusterChampionId, "Blockbuster World Champion", "Win a game on Deity on or before 1994 AD without building any units.", 50, 34, "fpk:pedia.FPK/wond_hollywood.dds" },
    { kShallowWorldviewId, "Shallow Worldview", "Win without moving units onto deep Ocean tiles.", 20, 9, "fpk:pedia.FPK/ter_ocean.dds" },
    { kCosmicEclipseId, "Cosmic Eclipse", "Win a technology victory on or before 100 BC.", 75, 32, "fpk:pedia.FPK/tech_spaceflight.dds" },
    { kArtOfTheWalkInId, "Art of the Walk In", "Capture an enemy capital city before 3500 BC on Deity.", 20, 12, "fpk:pedia.FPK/unit_great_artist_male.dds" },
    { kLastStandId, "Last Stand", "Win a battle against the odds while defending a city with a Militia.", 50, 1, "fpk:pedia.FPK/unit_militia.dds" },
    { kWeekendWarriorId, "Weekend Warrior", "Win a Game of the Week on Deity before 3000 BC.", 100, 13, "fpk:pedia.FPK/unit_warrior.dds" },
    { kYeAreIdleId, "Ye Are Idle, Ye Are Idle", "Win a game on Deity after settling your first city on or after turn 100.", 75, 20, "fpk:pedia.FPK/tech_alphabet.dds" },
    { kLeapThroughTimeId, "Leap Through Time", "Win a Game of the Week on Week 36428.", 10, 22, "fpk:pedia.FPK/tech_futuretechnology.dds" },
    { kNoEnemiesId, "I Have No Enemies", "Win a game on Deity without participating in a single battle.", 50, 51, "fpk:pedia.FPK/wond_unitednations.dds" },
    { kNoMoreWorldsId, "No More Worlds To Conquer", "Win a Domination victory on Deity with 6 AI opponents enabled.", 30, 37, "fpk:pedia.FPK/ldr_greece.dds" },
    { kTacticallyCenteredId, "Tactically Centered", "Win a game on Deity without building any units, with all five AI Handicaps set to 50% for the entire game.", 100, 21, "fpk:pedia.FPK/wond_thegreatwall.dds" },
    { kFlyMeToTheMoonId, "Fly Me to the Moon", "Win a Technology victory on Deity with the AI Research Handicap set to 200% for the entire game.", 75, 32, "fpk:pedia.FPK/wond_apolloprogram.dds" },
    { kMidasTouchId, "Midas Touch", "Win an Economic victory on Deity with the AI Gold Handicap set to 200% for the entire game.", 75, 16, "fpk:pedia.FPK/res_gold.dds" },
    { kCultureClubId, "Culture Club", "Win a Culture victory on Deity with the AI Culture Handicap set to 200% for the entire game.", 75, 9, "fpk:pedia.FPK/wond_shakespearestheatre.dds" },
    { kIronManId, "Iron Man", "Win a Domination victory on Deity with the AI Production Handicap set to 200% for the entire game.", 75, 13, "fpk:pedia.FPK/res_iron.dds" },
    { kOvertimeId, "Overtime", "Win after 2100 AD in a game with an extended Turn Limit.", 10, 51, "fpk:pedia.FPK/tech_navigation.dds" },
    { kWorldAtWarId, "World at War", "Win a game on Deity on the Earth map from the Real World map pack with Always At War and 6 AI opponents.", 100, 4, "fpk:pedia.FPK/wond_theinternet.dds" },
    { kContinentalDriftId, "Continental Drift", "Win a game on any map from the Landmasses map pack.", 25, 30, "fpk:pedia.FPK/unit_galley.dds" },
    { kIslandInTheSunId, "Island in the Sun", "Win a game on any map from the Islands map pack.", 25, 23, "fpk:pedia.FPK/unit_galleon.dds" },
    { kSmokeOnTheWaterId, "Smoke on the Water", "Win a game on any map from the Inland Water map pack.", 25, 26, "fpk:pedia.FPK/unit_submarine.dds" },
    { kClashOfTheTitansId, "Clash of the Titans", "Win a game on any map from the Matchups map pack.", 25, 18, "fpk:pedia.FPK/unit_great_leader.dds" },
    { kWeatherWithYouId, "Weather With You", "Win a game on any map from the Climates map pack.", 25, 25, "fpk:Common0.FPK/tech_astronomy.dds" },
    { kAroundTheWorldId, "Around the World", "Win a game on any map from the Real World map pack.", 25, 34, "fpk:pedia.FPK/tech_globalization.dds" },
    { kBayHarborButcherId, "The Bay Harbor Butcher", "Sink 50 ships in one game on Deity on any map from the Islands map pack.", 50, 24, "fpk:pedia.FPK/bldg_harbor.dds" },
    { kMorteEternaId, "Morte Eterna", "Win a Domination victory on the Europe map on Deity, with all five AI Handicaps set to 100% for the entire game and Always At War enabled.", 100, 35, "fpk:pedia.FPK/unit_great_general.dds" },
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

// The native game ends at 2100 AD, so a later win needs a Turn Limit that extends or removes it.
bool turnLimitExtended(const VictoryContext& context) noexcept
{
    namespace gf = gameplay_fixes;
    return gf::ExtendsTurnLimit(context.turnLimit) || context.turnLimit == gf::kTurnLimitUnlimited;
}

bool inPack(const VictoryContext& context, std::string_view pack) noexcept
{
    return !context.networkGame && context.packId == pack;
}

bool countNavalVictory(const VictoryContext& context) noexcept
{
    {
        std::lock_guard lock(mutex);
        progress.navalVictories = std::min(progress.navalVictories + 1, kMaximumNavalVictories);
        if (progress.navalVictories < kNavalVictoryTarget)
        {
            return false;
        }
    }
    return deity(context) && inPack(context, "rerevved-islands");
}

// Deity win of one victory type with a single AI Handicap at the maximum for the whole game.
bool handicapWin(const VictoryContext& context, int32_t victoryType, int32_t ai_economy::Policy::* handicap) noexcept
{
    return deity(context) && !context.nativeEconomy && context.victoryType == victoryType &&
           context.policy.*handicap == ai_economy::kMaxBonusPercent;
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
    { kFlyMeToTheMoonId, QualifiesFlyMeToTheMoon },
    { kMidasTouchId, QualifiesMidasTouch },
    { kCultureClubId, QualifiesCultureClub },
    { kIronManId, QualifiesIronMan },
    { kOvertimeId, QualifiesOvertime },
    { kWorldAtWarId, QualifiesWorldAtWar },
    { kContinentalDriftId, QualifiesContinentalDrift },
    { kIslandInTheSunId, QualifiesIslandInTheSun },
    { kSmokeOnTheWaterId, QualifiesSmokeOnTheWater },
    { kClashOfTheTitansId, QualifiesClashOfTheTitans },
    { kWeatherWithYouId, QualifiesWeatherWithYou },
    { kAroundTheWorldId, QualifiesAroundTheWorld },
    { kMorteEternaId, QualifiesMorteEterna },
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

bool QualifiesFlyMeToTheMoon(const VictoryContext& context) noexcept
{
    return handicapWin(context, 2, &ai_economy::Policy::researchBonusPercent);
}

bool QualifiesMidasTouch(const VictoryContext& context) noexcept
{
    return handicapWin(context, 3, &ai_economy::Policy::goldBonusPercent);
}

bool QualifiesCultureClub(const VictoryContext& context) noexcept
{
    return handicapWin(context, 4, &ai_economy::Policy::cultureBonusPercent);
}

bool QualifiesIronMan(const VictoryContext& context) noexcept
{
    return handicapWin(context, 1, &ai_economy::Policy::productionBonusPercent);
}

bool QualifiesOvertime(const VictoryContext& context) noexcept
{
    return !context.networkGame && context.yearKnown && context.year > kOvertimeFirstYear && turnLimitExtended(context);
}

bool QualifiesWorldAtWar(const VictoryContext& context) noexcept
{
    return deity(context) && inPack(context, "rerevved-real-world") && context.mapName == "rr_earth" && context.alwaysAtWar &&
           context.initialAiOpponents == kNoMoreWorldsOpponents;
}

bool QualifiesContinentalDrift(const VictoryContext& context) noexcept
{
    return inPack(context, "rerevved-landmasses");
}

bool QualifiesIslandInTheSun(const VictoryContext& context) noexcept
{
    return inPack(context, "rerevved-islands");
}

bool QualifiesSmokeOnTheWater(const VictoryContext& context) noexcept
{
    return inPack(context, "rerevved-inland-water");
}

bool QualifiesClashOfTheTitans(const VictoryContext& context) noexcept
{
    return inPack(context, "rerevved-matchups");
}

bool QualifiesWeatherWithYou(const VictoryContext& context) noexcept
{
    return inPack(context, "rerevved-climates");
}

bool QualifiesAroundTheWorld(const VictoryContext& context) noexcept
{
    return inPack(context, "rerevved-real-world");
}

bool QualifiesMorteEterna(const VictoryContext& context) noexcept
{
    return deity(context) && context.victoryType == 1 && handicapsAt(context, 100) &&
           context.alwaysAtWar && inPack(context, "rerevved-real-world") && context.mapName == "rr_europe";
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
    restored.navalVictories    = std::clamp(restored.navalVictories, 0, kMaximumNavalVictories);
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

void RecordStartingEraApplied() noexcept
{
    RecordCityFounded(0);
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

bool CountsAsNavalVictory(const DefeatEvent& event) noexcept
{
    return event.combatSite && event.participantsMatch && event.loserType >= kFirstShipType && event.loserType <= kLastShipType &&
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

AwardResult RecordNavalVictory(const VictoryContext& context, const IsUnlockedFn& isUnlocked, const UnlockFn& unlock)
{
    return awardId(kBayHarborButcherId, countNavalVictory(context), isUnlocked, unlock);
}

AwardResult RecordNavalVictory(const VictoryContext& context)
{
    return awardEvent(kBayHarborButcherId, countNavalVictory(context));
}

} // namespace rerevved::custom_achievements
