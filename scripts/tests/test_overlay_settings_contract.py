"""F1 gameplay and notification layout guards."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OVERLAY = (ROOT / "src/overlay_settings.cpp").read_text(encoding="utf-8")

NOTIFICATION_ORDER = [
    "spaceship_scenes",
    "show_research_prompt",
    "show_popup_city_status",
    "show_popup_discoveries",
    "show_popup_combat_reports",
    "show_popup_rival_great_people",
    "show_popup_rankings",
    "show_popup_rival_war_peace",
    "show_popup_save_reminder",
    "show_popup_villages",
    "show_village_barbarian_messages",
    "show_popup_city_captures",
    "show_popup_rival_government",
    "show_popup_artifacts",
    "show_popup_spy_reports",
    "show_popup_victory_warnings",
    "show_barbarian_messages",
    "show_popup_trophy_room",
]


def tab_groups(name: str) -> list[tuple[str, list[str]]]:
    tabs = re.split(r"layout\.tabs\s*=\s*\{", OVERLAY, maxsplit=1)[1]
    body = re.split(rf'\{{\s*"{name}",', tabs, maxsplit=1)[1]
    body = re.split(r'\n        \{ "', body, maxsplit=1)[0]
    return [
        (title, re.findall(r'"([^"]+)"', rows))
        for title, rows in re.findall(r'\{ "([^"]*)", \{ ([^}]*) \}', body)
    ]


def section(start: str, end: str) -> str:
    return OVERLAY.split(start, 1)[1].split(end, 1)[0]


class OverlaySettingsContractTests(unittest.TestCase):
    def test_gameplay_groups(self):
        self.assertEqual(
            tab_groups("Gameplay"),
            [
                (
                    "",
                    [
                        "combat_speed",
                        "unit_movement",
                        "resources",
                        "raging_barbarians",
                        "one_city_challenge",
                        "domination_min_turns",
                        "turn_limit",
                        "starting_era",
                        "ai_civilizations",
                        "always_at_war",
                    ],
                ),
                (
                    "AI Handicaps",
                    [
                        "ai_research_bonus_percent",
                        "ai_production_bonus_percent",
                        "ai_gold_bonus_percent",
                        "ai_food_bonus_percent",
                        "ai_culture_bonus_percent",
                    ],
                ),
                (
                    "Game of the Week",
                    [
                        "gotw_standard_rules",
                        "gotw_allow_saving",
                        "gotw_custom_week",
                        "gotw_week",
                        "gotw_custom_civilization",
                        "gotw_civilization",
                    ],
                ),
            ],
        )

    def test_general_ends_with_the_log_folder(self):
        self.assertEqual(
            tab_groups("General"),
            [
                (
                    "",
                    [
                        "game_language",
                        "show_fps_counter",
                        "skip_intro_movie",
                        "open_log_folder",
                    ],
                )
            ],
        )

    def test_online_starts_with_the_multiplayer_ai_difficulty(self):
        groups = tab_groups("Online")
        self.assertEqual(
            groups[0],
            (
                "",
                [
                    "multiplayer_ai_difficulty",
                    "system_link_status",
                    "system_link_profile_name",
                ],
            ),
        )

    def test_owner_tooltips(self):
        for key, text in (
            ("skip_intro_movie", "Enable to skip the opening scenes at startup."),
            ("turn_limit", "Set the year a game ends by turn limit."),
            ("starting_era", "Set the era a new game begins in."),
            ("ai_civilizations", "Set the number of AI opponents."),
            ("always_at_war", "Enable to keep every civilization at war."),
            (
                "multiplayer_ai_difficulty",
                "Set the difficulty of AI in multiplayer games you host.",
            ),
        ):
            self.assertIn(f'{{ "{key}", "{text}" }}', OVERLAY)

    def test_notifications_run_from_most_to_least_frequent(self):
        self.assertEqual(tab_groups("Notifications"), [("", NOTIFICATION_ORDER)])

    def test_earliest_domination_choices(self):
        choices = re.search(r'\{ "domination_min_turns", \{ (.+) \} \}', OVERLAY)
        self.assertIsNotNone(choices)
        assert choices is not None
        self.assertEqual(
            re.findall(r'\{ "(\d+)", "([^"]+)" \}', choices.group(1)),
            [("10", "10 Turns (3000 BC)"), ("5", "5 Turns (3500 BC)")],
        )
        source = (ROOT / "src/gameplay_fixes.cpp").read_text(encoding="utf-8")
        self.assertIn('.allowed({ "10", "5" })', source)
        self.assertIn(
            '{ "domination_min_turns", "Set the minimum turns before a Domination '
            'victory in new games." }',
            OVERLAY,
        )

    def test_tooltips_omit_host_and_load_sentences(self):
        tooltips = section("layout.tooltips = {", "layout.choice_labels")
        for sentence in (
            "The host controls new multiplayer games.",
            "Saved gameplay settings apply on load.",
            "Settler rewards become population.",
        ):
            self.assertNotIn(sentence, tooltips)


if __name__ == "__main__":
    unittest.main()
