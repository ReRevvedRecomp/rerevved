#include "pause_menu.h"

#include "title_text.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pm = rerevved::pause_menu;

namespace
{

std::vector<std::string> lines(std::string_view text)
{
    std::vector<std::string> result;
    while (!text.empty())
    {
        const auto end = text.find('\n');
        REQUIRE(end != std::string_view::npos);
        result.emplace_back(text.substr(0, end));
        text.remove_prefix(end + 1);
    }
    return result;
}

// Retail texts 0x8214DC80 and 0x8214DCF8 and their translations; accented bytes are Windows-1252.
constexpr std::string_view kEnglishFull =
    "Special Functions:\n  Show Controller Map\n Save the Game.\n Load a Saved Game.\n Change Options.\n Exit to the "
    "Main Menu\n";
constexpr std::string_view kEnglishAlternate =
    "Special Functions:\n Show Controller Map.\n Change Options.\n Exit to the Main Menu.\n";
constexpr std::string_view kFrenchFull =
    "Fonctions sp\xE9"
    "ciales\xA0:\n Voir les commandes\n Sauvegarder la partie\n Charger une partie\n Modifier les options\n Revenir au "
    "menu principal\n";
constexpr std::string_view kGermanAlternate =
    "Spezielle Funktionen:\n Controller-Belegung anzeigen\n Optionen \xE4"
    "ndern\n Zur\xFC"
    "ck zum Hauptmen\xFC\n";
constexpr std::string_view kSpanishFull =
    "Funciones especiales:\n Mostrar configuraci\xF3"
    "n del mando.\n Guardar la partida.\n Cargar una partida guardada.\n Cambiar las opciones.\n "
    "Salir al men\xFA principal.\n";
constexpr std::string_view kSpanishAlternate =
    "Funciones especiales:\n Mostrar configuraci\xF3"
    "n del mando.\n Cambiar las opciones.\n Salir al men\xFA principal.\n";

} // namespace

TEST_CASE("pause_menu_builds_the_english_lists_from_the_retail_text", "[ctest][pause-menu]")
{
    CHECK(pm::BuildList(pm::Variant::kFull, kEnglishFull, 1) ==
          "Special Functions:\n  Show Controller Map\n Save the Game.\n Load a Saved Game.\n Change Options.\n "
          "Exit to the Main Menu\n Exit Game\n");
    CHECK(pm::BuildList(pm::Variant::kAlternate, kEnglishAlternate, 1) ==
          "Special Functions:\n Show Controller Map.\n Change Options.\n Exit to the Main Menu.\n Exit "
          "Game.\n");
}

TEST_CASE("pause_menu_inserts_rows_into_a_translated_retail_list", "[ctest][pause-menu]")
{
    const auto french = pm::BuildList(pm::Variant::kFull, kFrenchFull, 4);
    REQUIRE(french.has_value());
    CHECK(lines(*french) == std::vector<std::string>{ "Fonctions sp\xE9"
                                                      "ciales\xA0:",
                                                      " Voir les commandes",
                                                      " Sauvegarder la partie",
                                                      " Charger une partie",
                                                      " Modifier les options",
                                                      " Revenir au menu principal",
                                                      " Quitter le jeu" });

    const auto spanish = pm::BuildList(pm::Variant::kAlternate, kSpanishAlternate, 5);
    REQUIRE(spanish.has_value());
    CHECK(lines(*spanish) == std::vector<std::string>{ "Funciones especiales:",
                                                       " Mostrar configuraci\xF3n del mando.",
                                                       " Cambiar las opciones.",
                                                       " Salir al men\xFA principal.",
                                                       " Salir del juego." });

    const auto german = pm::BuildList(pm::Variant::kAlternate, kGermanAlternate, 3);
    REQUIRE(german.has_value());
    CHECK(lines(*german).back() == " Spiel beenden");
    CHECK(lines(*german).size() == 5);

    const auto spanishFull = pm::BuildList(pm::Variant::kFull, kSpanishFull, 5);
    REQUIRE(spanishFull.has_value());
    CHECK(lines(*spanishFull).back() == " Salir del juego.");
    CHECK(lines(*spanishFull)[4] == " Cambiar las opciones.");
    CHECK(lines(*spanishFull).size() == 7);
}

TEST_CASE("pause_menu_keeps_the_retail_rows_when_the_retail_text_is_unexpected", "[ctest][pause-menu]")
{
    CHECK_FALSE(pm::BuildList(pm::Variant::kFull, "", 1).has_value());
    CHECK_FALSE(pm::BuildList(pm::Variant::kFull, kEnglishAlternate, 1).has_value());
    CHECK_FALSE(pm::BuildList(pm::Variant::kAlternate, kEnglishFull, 1).has_value());
    CHECK_FALSE(pm::BuildList(pm::Variant::kAlternate, kEnglishAlternate.substr(0, kEnglishAlternate.size() - 1), 1)
                    .has_value());
    CHECK_FALSE(pm::BuildList(pm::Variant::kAlternate, "\n Show Controller Map.\n Change Options.\n Exit.\n", 1)
                    .has_value());
    CHECK(pm::RetailRowCount(pm::Variant::kFull) == 5);
    CHECK(pm::RetailRowCount(pm::Variant::kAlternate) == 3);
}

TEST_CASE("pause_menu_falls_back_to_english_title_rows", "[ctest][pause-menu]")
{
    CHECK(pm::BuildList(pm::Variant::kFull, kEnglishFull, 77) == pm::BuildList(pm::Variant::kFull, kEnglishFull, 1));
}

TEST_CASE("pause_menu_maps_rows_to_retail_indices", "[ctest][pause-menu]")
{
    using Kind = pm::Choice::Kind;

    struct Case
    {
        pm::Variant variant;
        int32_t     row;
        Kind        kind;
        int32_t     retailRow;
    };

    const Case cases[] = {
        { pm::Variant::kFull, -1, Kind::kRetail, -1 },
        { pm::Variant::kFull, 0, Kind::kRetail, 0 },
        { pm::Variant::kFull, 3, Kind::kRetail, 3 },
        { pm::Variant::kFull, 4, Kind::kRetail, 4 },
        { pm::Variant::kFull, 5, Kind::kExitGame, -1 },
        { pm::Variant::kFull, 6, Kind::kRetail, -1 },
        { pm::Variant::kAlternate, -1, Kind::kRetail, -1 },
        { pm::Variant::kAlternate, 0, Kind::kRetail, 0 },
        { pm::Variant::kAlternate, 1, Kind::kRetail, 1 },
        { pm::Variant::kAlternate, 2, Kind::kRetail, 2 },
        { pm::Variant::kAlternate, 3, Kind::kExitGame, -1 },
        { pm::Variant::kAlternate, 4, Kind::kRetail, -1 },
        { pm::Variant::kAlternate, -7, Kind::kRetail, -1 },
    };
    for (const auto& c : cases)
    {
        CAPTURE(static_cast<int>(c.variant), c.row);
        const auto choice = pm::ResolveRow(c.variant, c.row);
        CHECK(choice.kind == c.kind);
        CHECK(choice.retailRow == c.retailRow);
    }
}

TEST_CASE("pause_menu_selects_call_sites_and_quit_text", "[ctest][pause-menu]")
{
    CHECK(pm::VariantForReturn(0x82D06538) == pm::Variant::kFull);
    CHECK(pm::VariantForReturn(0x82D06614) == pm::Variant::kAlternate);
    CHECK_FALSE(pm::VariantForReturn(0x82D853B0).has_value());

    CHECK(pm::QuitText(2, true, true) == 0x821377C0);
    CHECK(pm::QuitText(1, true, true) == 0x82137828);
    CHECK(pm::QuitText(1, true, false) == 0x821378E0);
    CHECK(pm::QuitText(2, false, false) == 0x821378E0);
}

TEST_CASE("pause_menu_host_actions_are_optional", "[ctest][pause-menu]")
{
    pm::RequestQuit();

    int quits = 0;
    pm::SetHostActions({ .quit = [&]
                         {
                             ++quits;
                         } });
    pm::RequestQuit();
    pm::SetHostActions({});
    pm::RequestQuit();

    CHECK(quits == 1);
}
