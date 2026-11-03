#pragma once

// Disc's five languages: Id rows for guest screens/page chrome; keyed settings.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rerevved::title_text
{

enum class Language : uint8_t
{
    kEnglish,
    kGerman,
    kFrench,
    kSpanish,
    kItalian,
};

inline constexpr size_t kLanguageCount = 5;

enum class Id : uint8_t
{
    kExitGameRow,
    // The page fills {0}, {1}.
    kPageOptions,
    kPageAchievements,
    kAchievementsSummary, // {0} unlocked of {1}
    kAchievementsScore,   // {0} gamerscore earned of {1}, each followed by G
    kAchievementLocked,
    kAchievementUnlocked,
    kAchievementUnlockedOn, // {0}: unlock date
    kAchievementsNone,
    kPageMods,
    kModsSummary, // Mods strip: {0} installed, {1} enabled
    kModsNone,
    kModNative,
    kModUnavailable, // Missing enabled package; {0}: ID
    kModUnavailableTip,
    kModsMoving,
    kModsUnsaved,
    kModsRestart,
    kModsFixErrors,
    kModsReplaceFile,
    kModsSaveFailed,
    kModsDiscardAsk,
    kModsReplaceAsk,
    kLegendEnable,
    kLegendDisable,
    kLegendRemove,
    kLegendMove,
    kLegendDrop,
    kLegendApply,
    kLegendDiscard,
    kLegendReplace,
    kLegendKeep,
    kLegendCancel,
    kAchievementsOriginal,
    kAchievementsCustom,
    kModsTabPackages,
    kModsTabAssets,
    kAssetsNone,
    kAssetsMoving,
    kAssetsSaveFailed,
    kAssetsDiscardAsk,
    kAssetsReplaceAsk,
    kModsTabMaps,
    kMapsNone,
    kMapsMoving,
    kMapsSaveFailed,
    kMapsDiscardAsk,
    kMapsReplaceAsk,
    kLegendBack,
    kLegendSelect,
    kDiagnosticError,       // Package error; {0}: message
    kDiagnosticWarning,     // Package warning; {0}: message
    kDiagnosticLine,        // Loadout problem; {0}: line, {1}: message
    kAchievementGamerscore, // {0}: achievement gamerscore
    kModsLocked,
    kKeyShift,
    kKeySpace,
    kKeyPaste,
    kKeyClear,
    kKeyDelete,
    kKeyDone,
    kLegendType,
    kTextHint,
    kButtonCopy,
    kLegendReset,
    kBindPress,
    kCount,
};

inline constexpr size_t kIdCount = static_cast<size_t>(Id::kCount);

// Stable key, language texts and translator note; empty text is untranslated.
struct Row
{
    std::string_view                             key;
    std::array<std::string_view, kLanguageCount> text;
    std::string_view                             note;
};

// Xbox IDs 3, 4, 5, 6 are German, French, Spanish, Italian; anything else is English.
Language LanguageForXboxId(uint32_t xboxId) noexcept;

// Raw UTF-8 entry; empty if untranslated.
std::string_view Entry(Id id, Language language) noexcept;

std::string_view Localized(Id id, Language language) noexcept;

std::string Text(Id id, uint32_t xboxId);

size_t     RowCount() noexcept;
const Row& RowAt(size_t index) noexcept;
const Row* FindRow(std::string_view key) noexcept;

std::string_view KeyOf(Id id) noexcept;

// Keys of the options screen's settings text.
//   tab.<english name in lower case>         a tab title
//   section.<name in lower case, with _>     a section subheading
//   setting.<cvar>.label | .tip              a setting's label or tooltip
//   choice.<cvar>.<raw value>                a value caption; "default" for ""
std::string TabKey(std::string_view name);
std::string SectionKey(std::string_view name);
std::string SettingKey(std::string_view cvar, std::string_view field);
std::string ChoiceKey(std::string_view cvar, std::string_view raw);

// Translated text, else caller-supplied english; English never depends on the table.
std::string Screen(std::string_view key, std::string_view english, Language language);

// Unsupported characters and invalid sequences become '?'.
std::string ToWindows1252(std::string_view utf8);

} // namespace rerevved::title_text
