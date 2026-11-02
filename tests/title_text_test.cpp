#include "title_text.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace tt = rerevved::title_text;

namespace
{

constexpr std::array<tt::Language, 4> kOtherLanguages{ tt::Language::kGerman, tt::Language::kFrench, tt::Language::kSpanish, tt::Language::kItalian };

std::vector<std::string> placeholdersOf(std::string_view text)
{
    std::vector<std::string> found;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '{')
            continue;
        const size_t close = text.find('}', i);
        if (close == std::string_view::npos)
            break;
        found.emplace_back(text.substr(i, close - i + 1));
        i = close;
    }
    std::sort(found.begin(), found.end());
    return found;
}

std::vector<uint32_t> codePointsOf(std::string_view utf8)
{
    std::vector<uint32_t> points;
    for (size_t i = 0; i < utf8.size();)
    {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        size_t     length;
        uint32_t   point;
        if (lead < 0x80)
        {
            length = 1;
            point  = lead;
        }
        else if ((lead & 0xE0) == 0xC0)
        {
            length = 2;
            point  = lead & 0x1Fu;
        }
        else if ((lead & 0xF0) == 0xE0)
        {
            length = 3;
            point  = lead & 0x0Fu;
        }
        else if ((lead & 0xF8) == 0xF0)
        {
            length = 4;
            point  = lead & 0x07u;
        }
        else
            return {};
        if (i + length > utf8.size())
            return {};
        for (size_t k = 1; k < length; ++k)
        {
            const auto next = static_cast<unsigned char>(utf8[i + k]);
            if ((next & 0xC0) != 0x80)
                return {};
            point = point << 6 | (next & 0x3Fu);
        }
        points.push_back(point);
        i += length;
    }
    return points;
}

} // namespace

TEST_CASE("title_text_picks_a_language_from_the_xbox_id", "[ctest][title-text]")
{
    CHECK(tt::LanguageForXboxId(1) == tt::Language::kEnglish);
    CHECK(tt::LanguageForXboxId(3) == tt::Language::kGerman);
    CHECK(tt::LanguageForXboxId(4) == tt::Language::kFrench);
    CHECK(tt::LanguageForXboxId(5) == tt::Language::kSpanish);
    CHECK(tt::LanguageForXboxId(6) == tt::Language::kItalian);
    CHECK(tt::LanguageForXboxId(0) == tt::Language::kEnglish);
    CHECK(tt::LanguageForXboxId(2) == tt::Language::kEnglish);
    CHECK(tt::LanguageForXboxId(7) == tt::Language::kEnglish);
    CHECK(tt::LanguageForXboxId(0xFFFFFFFFu) == tt::Language::kEnglish);
}

TEST_CASE("title_text_has_english_for_every_string", "[ctest][title-text]")
{
    for (size_t i = 0; i < tt::kIdCount; ++i)
    {
        const auto id = static_cast<tt::Id>(i);
        CAPTURE(i);
        CHECK_FALSE(tt::Entry(id, tt::Language::kEnglish).empty());
    }
    CHECK(tt::Entry(tt::Id::kExitGameRow, tt::Language::kEnglish) == "Exit Game");
    CHECK(tt::Entry(tt::Id::kPageOptions, tt::Language::kEnglish) == "Options");
    CHECK(tt::Entry(tt::Id::kPageAchievements, tt::Language::kEnglish) == "Achievements");
    CHECK(tt::Entry(tt::Id::kAchievementsSummary, tt::Language::kEnglish) == "{0} of {1} unlocked");
    CHECK(tt::Entry(tt::Id::kPageMods, tt::Language::kEnglish) == "Mods");
    CHECK(tt::Entry(tt::Id::kModsSummary, tt::Language::kEnglish) == "{0} installed, {1} enabled");
    CHECK(tt::Entry(tt::Id::kLegendCancel, tt::Language::kEnglish) == "Cancel");
    CHECK(tt::Entry(tt::Id::kAchievementsOriginal, tt::Language::kEnglish) == "Original");
    CHECK(tt::Entry(tt::Id::kAchievementsCustom, tt::Language::kEnglish) == "Custom");
    CHECK(tt::Entry(tt::Id::kModsTabPackages, tt::Language::kEnglish) == "Packages");
    CHECK(tt::Entry(tt::Id::kModsTabAssets, tt::Language::kEnglish) == "Assets");
    CHECK(tt::Entry(tt::Id::kCount, tt::Language::kEnglish).empty());
}

TEST_CASE("title_text_falls_back_to_english_for_an_empty_entry", "[ctest][title-text]")
{
    for (size_t i = 0; i < tt::kIdCount; ++i)
    {
        const auto id = static_cast<tt::Id>(i);
        for (const auto language :
             { tt::Language::kGerman, tt::Language::kFrench, tt::Language::kSpanish, tt::Language::kItalian })
        {
            CAPTURE(i, static_cast<int>(language));
            const auto entry = tt::Entry(id, language);
            CHECK(tt::Localized(id, language) == (entry.empty() ? tt::Entry(id, tt::Language::kEnglish) : entry));
            CHECK_FALSE(tt::Localized(id, language).empty());
        }
    }
    CHECK(tt::Text(tt::Id::kPageMods, 99) == "Mods");
}

TEST_CASE("title_text_entries_convert_to_the_game_code_page", "[ctest][title-text]")
{
    for (size_t i = 0; i < tt::kIdCount; ++i)
    {
        const auto id = static_cast<tt::Id>(i);
        for (const auto language : { tt::Language::kEnglish,
                                     tt::Language::kGerman,
                                     tt::Language::kFrench,
                                     tt::Language::kSpanish,
                                     tt::Language::kItalian })
        {
            const auto entry = tt::Entry(id, language);
            CAPTURE(i, static_cast<int>(language));
            if (entry.find('?') == std::string_view::npos)
                CHECK(tt::ToWindows1252(entry).find('?') == std::string::npos);
        }
    }
}

TEST_CASE("title_text_converts_utf8_to_windows_1252", "[ctest][title-text]")
{
    CHECK(tt::ToWindows1252("") == "");
    CHECK(tt::ToWindows1252("Plain ASCII.") == "Plain ASCII.");
    // Latin-1 range: e acute, a umlaut, sharp s, n tilde, non-breaking space.
    CHECK(tt::ToWindows1252("\xC3\xA9\xC3\xA4\xC3\x9F\xC3\xB1\xC2\xA0") == "\xE9\xE4\xDF\xF1\xA0");
    CHECK(tt::ToWindows1252("\xC5\x93") == "\x9C");      // oe ligature
    CHECK(tt::ToWindows1252("\xE2\x80\x99") == "\x92");  // right single quote
    CHECK(tt::ToWindows1252("\xE2\x80\x93") == "\x96");  // en dash
    CHECK(tt::ToWindows1252("\xE2\x80\xA6") == "\x85");  // ellipsis
    CHECK(tt::ToWindows1252("\xE2\x82\xAC") == "\x80");  // euro
    CHECK(tt::ToWindows1252("\xE6\x97\xA5") == "?");     // a CJK character
    CHECK(tt::ToWindows1252("\xC2\x85") == "?");         // C1 control
    CHECK(tt::ToWindows1252("\xC0\x80") == "?");         // overlong NUL
    CHECK(tt::ToWindows1252("\xE0\x83\xA9") == "?");     // overlong e-acute
    CHECK(tt::ToWindows1252("\xF0\x82\x82\xAC") == "?"); // overlong euro
    CHECK(tt::ToWindows1252("a\xC3"
                            "b") == "a?b"); // truncated sequence
    CHECK(tt::ToWindows1252("a\xFF"
                            "b") == "a?b"); // invalid lead byte
    CHECK(tt::ToWindows1252("a\x80"
                            "b") == "a?b");      // stray continuation
    CHECK(tt::ToWindows1252("\xE2\x80") == "?"); // cut off at the end
}

TEST_CASE("title_text_fills_every_language_and_converts_without_loss", "[ctest][title-text]")
{
    for (size_t i = 0; i < tt::kIdCount; ++i)
    {
        const auto id = static_cast<tt::Id>(i);
        for (const auto language :
             { tt::Language::kGerman, tt::Language::kFrench, tt::Language::kSpanish, tt::Language::kItalian })
        {
            CAPTURE(i, static_cast<int>(language));
            const auto entry = tt::Entry(id, language);
            REQUIRE_FALSE(entry.empty());
            const auto converted = tt::ToWindows1252(entry);
            // An entry may contain a literal question mark.
            if (entry.find('?') == std::string_view::npos)
                CHECK(converted.find('?') == std::string::npos);
            size_t characters = 0;
            for (const char c : entry)
                characters += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
            CHECK(converted.size() == characters);
        }
    }
    CHECK(tt::Text(tt::Id::kPageAchievements, 4) == "Succ\xE8"
                                                    "s");
    CHECK(tt::Text(tt::Id::kExitGameRow, 4) == "Quitter le jeu");
}

TEST_CASE("title_text_rows_have_unique_keys_and_english", "[ctest][title-text]")
{
    REQUIRE(tt::RowCount() > tt::kIdCount);
    std::set<std::string_view> keys;
    for (size_t i = 0; i < tt::RowCount(); ++i)
    {
        const auto& row = tt::RowAt(i);
        CAPTURE(i, row.key);
        CHECK_FALSE(row.key.empty());
        CHECK(keys.insert(row.key).second);
        CHECK_FALSE(row.text[static_cast<size_t>(tt::Language::kEnglish)].empty());
        CHECK(row.key.find('.') != std::string_view::npos);
        CHECK(std::all_of(row.key.begin(), row.key.end(), [](char c)
                          {
                              return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_';
                          }));
        CHECK(tt::FindRow(row.key) == &row);
    }
    for (size_t i = 0; i < tt::kIdCount; ++i)
        CHECK(tt::KeyOf(static_cast<tt::Id>(i)) == tt::RowAt(i).key);
    CHECK(tt::KeyOf(tt::Id::kCount).empty());
    CHECK(tt::FindRow("no.such.key") == nullptr);
}

TEST_CASE("title_text_translations_keep_the_english_placeholders", "[ctest][title-text]")
{
    for (size_t i = 0; i < tt::RowCount(); ++i)
    {
        const auto& row     = tt::RowAt(i);
        const auto  english = placeholdersOf(row.text[static_cast<size_t>(tt::Language::kEnglish)]);
        for (const auto language : kOtherLanguages)
        {
            const auto cell = row.text[static_cast<size_t>(language)];
            CAPTURE(row.key, static_cast<int>(language));
            if (!cell.empty())
                CHECK(placeholdersOf(cell) == english);
        }
    }
    CHECK(placeholdersOf("{0} of {1}, {0}") == std::vector<std::string>{ "{0}", "{0}", "{1}" });
    CHECK(placeholdersOf("none").empty());
}

TEST_CASE("title_text_screen_entries_stay_inside_the_fonts", "[ctest][title-text]")
{
    // ImGui fonts bake U+0020..U+00FF only; kExitGameRow, before kPageOptions, is tested separately.
    for (size_t i = static_cast<size_t>(tt::Id::kPageOptions); i < tt::RowCount(); ++i)
    {
        const auto& row = tt::RowAt(i);
        for (size_t column = 0; column < tt::kLanguageCount; ++column)
        {
            const auto cell = row.text[column];
            CAPTURE(row.key, column);
            if (cell.empty())
                continue;
            const auto points = codePointsOf(cell);
            CHECK_FALSE(points.empty());
            for (const uint32_t point : points)
                CHECK(((point >= 0x20 && point <= 0x7E) || (point >= 0xA0 && point <= 0xFF)));
            // An entry may hold a question mark, so compare the Windows-1252 bytes.
            const auto converted = tt::ToWindows1252(cell);
            REQUIRE(converted.size() == points.size());
            for (size_t k = 0; k < points.size(); ++k)
                CHECK(static_cast<unsigned char>(converted[k]) == points[k]);
        }
    }
}

TEST_CASE("title_text_screen_uses_a_translation_else_the_callers_english", "[ctest][title-text]")
{
    CHECK(tt::Screen("title.exit_game_row", "Exit", tt::Language::kGerman) == "Spiel beenden");
    CHECK(tt::Screen("title.exit_game_row", "Exit", tt::Language::kEnglish) == "Exit");
    CHECK(tt::Screen("setting.fullscreen.label", "Fullscreen", tt::Language::kItalian) == "Schermo intero");
    CHECK(tt::Screen("no.such.key", "Kept", tt::Language::kFrench) == "Kept");
}

TEST_CASE("title_text_builds_the_screen_keys", "[ctest][title-text]")
{
    CHECK(tt::TabKey("General") == "tab.general");
    CHECK(tt::SectionKey("Game of the Week") == "section.game_of_the_week");
    CHECK(tt::SettingKey("combat_speed", "label") == "setting.combat_speed.label");
    CHECK(tt::ChoiceKey("window_size", "") == "choice.window_size.default");
    CHECK(tt::ChoiceKey("resources", "3") == "choice.resources.3");
}

namespace
{

std::string listCell(std::string_view text)
{
    std::string out;
    for (const uint32_t point : codePointsOf(text))
    {
        if (point == '|')
            out += "\\|";
        else if (point < 0x80)
            out.push_back(static_cast<char>(point));
        else
        {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\u%04x", point);
            out += buffer;
        }
    }
    return out;
}

struct Area
{
    std::string_view              title;
    std::string_view              intro;
    std::vector<std::string_view> prefixes;
};

} // namespace

// Manual: set REREVVED_STRINGS_MD to the output path and run `rerevved_tests title_text_writes_the_translators_list`.
TEST_CASE("title_text_writes_the_translators_list", "[.][manual][title-text]")
{
    std::string path;
#if defined(_WIN32)
    char*       value  = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "REREVVED_STRINGS_MD") == 0 && value)
        path = value;
    std::free(value);
#else
    if (const char* value = std::getenv("REREVVED_STRINGS_MD"))
        path = value;
#endif
    REQUIRE_FALSE(path.empty());

    const std::vector<Area> areas{
        { "Game screens",
          "Text the title adds to the game's own menus. It goes through the game's text path (Windows-1252), so use letters of the Latin-1 set and the punctuation "
          "the retail tables use. Already translated; listed so the set is complete.",
          { "title." } },
        { "Page strip and tabs",
          "Titles of the screen's pages and of the tabs under them. Each is one short word where the language allows; the strip shrinks the text and then scrolls.",
          { "page.", "tab.", "section.", "achievements.tab_", "mods.tab_" } },
        { "Footer buttons", "Captions of the buttons along the bottom edge. A button is about 9 em wide, so keep each to a short word (about 10 characters).", { "legend." } },
        { "Settings: labels and tooltips",
          "One label and, for most settings, one tooltip per setting, in tab order. The label is the row's left text (one line); the tooltip fills the footer while "
          "the row is highlighted (at most 3 lines show). Match the game's own terms where the game has one (for example Resources, Barbarians, Domination).",
          { "setting." } },
        { "Settings: values",
          "Value captions shown centered in the value column, and the On and Off shared by every switch. Numbers, resolutions and product names are not listed. "
          "The Monitor row's other values come from the system.",
          { "choice.", "common." } },
        { "Settings: footer notes", "One-line notes under a highlighted setting.", { "note." } },
        { "Mods page",
          "The Mods page, its Assets tab and the messages about packages. Package names and descriptions come from the packages and stay as written.",
          { "mods.", "assets.", "diagnostic." } },
        { "Achievements page",
          "The Achievements page. Achievement names and descriptions come from the game or the data and stay as written. Dates are shown as YYYY-MM-DD.",
          { "achievements." } },
    };

    size_t longestLabel   = 0;
    size_t longestTip     = 0;
    size_t longestCaption = 0;
    for (size_t i = tt::kIdCount; i < tt::RowCount(); ++i)
    {
        const auto&  row = tt::RowAt(i);
        const size_t len = row.text[0].size();
        if (row.key.starts_with("setting.") && row.key.ends_with(".label"))
            longestLabel = std::max(longestLabel, len);
        else if (row.key.starts_with("setting."))
            longestTip = std::max(longestTip, len);
        else if (row.key.starts_with("choice."))
            longestCaption = std::max(longestCaption, len);
    }

    const auto context = [&](const tt::Row& row) -> std::string
    {
        if (!row.note.empty())
            return std::string(row.note);
        const std::string key(row.key);
        if (key.starts_with("setting.") && key.ends_with(".label"))
            return "Row label, left of the value column; one line. Longest English: " + std::to_string(longestLabel) + " characters; stay near that or shorter.";
        if (key.starts_with("setting."))
            return "Footer text while the row is highlighted; wraps, and at most 3 lines show. Longest English: " + std::to_string(longestTip) +
                   " characters; stay near that or shorter.";
        if (key.starts_with("choice.game_language."))
            return "Name of a language in the Language row. Planned: each language under its own name (Deutsch, Francais, Espanol, Italiano), the same in every column; English shows the English name today.";
        if (key.starts_with("choice.gotw_civilization."))
            return "Civilization adjective in the Game of the Week Civilization row. Use the game's own civilization name in this language. One line.";
        if (key.starts_with("choice."))
            return "Value shown centered in the value column; one line. Longest English: " + std::to_string(longestCaption) + " characters.";
        return {};
    };

    std::string out;
    out += "# Translatable strings\n\n";
    out += "Every string the combined options screen (Options, Mods, Achievements) and the title's menu rows draw, one row each. "
           "Fill the German, French, Spanish and Italian columns; an empty cell shows the English text. F1, F2 and F3 stay English.\n\n";
    out += "- Key: stable identifier; do not change it.\n";
    out += "- Placeholders such as {0} and {1} are filled in by the screen. Keep every one, in any order the language needs.\n";
    out += "- Write letters as the language spells them; the table is UTF-8. The screen's fonts hold U+0020..U+00FF (Latin-1), so avoid characters outside it "
           "(for example the oe ligature or curly quotes); a test rejects them.\n";
    out += "- Not listed because they are not translated: the keyboard key names on button prompts (Enter, Esc, PgUp, PgDn, Q, E), "
           "the Xbox, PlayStation, CAS, FSR and FXAA product names, numbers and resolutions, the version label of a mod (v1.2), "
           "and the date format.\n\n";

    size_t     listed = 0;
    const auto areaOf = [&](const tt::Row& row) -> size_t
    {
        for (size_t a = 0; a < areas.size(); ++a)
        {
            for (const auto prefix : areas[a].prefixes)
            {
                if (row.key.starts_with(prefix))
                    return a;
            }
        }
        return areas.size();
    };
    for (size_t a = 0; a < areas.size(); ++a)
    {
        const auto&                 area = areas[a];
        std::vector<const tt::Row*> rows;
        for (size_t i = 0; i < tt::RowCount(); ++i)
        {
            if (areaOf(tt::RowAt(i)) == a)
                rows.push_back(&tt::RowAt(i));
        }
        listed += rows.size();
        out += "## " + std::string(area.title) + " (" + std::to_string(rows.size()) + ")\n\n" + std::string(area.intro) + "\n\n";
        out += "| Key | English | Placeholders | Context | German | French | Spanish | Italian |\n";
        out += "|---|---|---|---|---|---|---|---|\n";
        for (const auto* row : rows)
        {
            std::string holders;
            for (const auto& holder : placeholdersOf(row->text[0]))
                holders += (holders.empty() ? "" : " ") + holder;
            out += "| `" + std::string(row->key) + "` | " + listCell(row->text[0]) + " | " + holders + " | " + listCell(context(*row)) + " |";
            for (size_t column = 1; column < tt::kLanguageCount; ++column)
                out += " " + listCell(row->text[column]) + " |";
            out += "\n";
        }
        out += "\n";
    }
    CHECK(listed == tt::RowCount());

    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file << out;
}

TEST_CASE("title_text_translates_every_row_into_every_language", "[ctest][title-text]")
{
    // English fallback is for incomplete builds; shipped rows require translations.
    for (size_t i = 0; i < tt::RowCount(); ++i)
    {
        const auto& row = tt::RowAt(i);
        for (const auto language : kOtherLanguages)
        {
            CAPTURE(row.key, static_cast<int>(language));
            CHECK_FALSE(row.text[static_cast<size_t>(language)].empty());
        }
    }
}
