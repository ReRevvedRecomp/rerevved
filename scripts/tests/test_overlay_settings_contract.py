"""F1 gameplay and notification layout guards."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OVERLAY = (ROOT / "src/overlay_settings.cpp").read_text(encoding="utf-8")

NOTIFICATION_ORDER = [
    "hide_research_prompt",
    "hide_popup_city_status",
    "hide_popup_discoveries",
    "hide_popup_combat_reports",
    "hide_popup_rival_great_people",
    "hide_popup_rankings",
    "hide_popup_rival_war_peace",
    "hide_popup_save_reminder",
    "hide_popup_villages",
    "hide_village_barbarian_messages",
    "hide_popup_city_captures",
    "hide_popup_rival_government",
    "hide_popup_artifacts",
    "hide_popup_spy_reports",
    "hide_popup_victory_warnings",
    "hide_popup_government_available",
    "hide_barbarian_messages",
    "hide_popup_trophy_room",
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
                        "resources",
                        "raging_barbarians",
                        "one_city_challenge",
                        "domination_min_turns",
                        "ai_civilizations",
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
                        "gotw_custom_week",
                        "gotw_week",
                        "gotw_custom_civilization",
                        "gotw_civilization",
                    ],
                ),
                ("Support", ["open_log_folder"]),
            ],
        )

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
            'victory in new games. Loaded saves keep 10." }',
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
