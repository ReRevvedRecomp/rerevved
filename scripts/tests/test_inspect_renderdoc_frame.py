from __future__ import annotations

import importlib.util
import json
import os
import unittest
from enum import IntFlag
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "inspect-renderdoc-frame.py"
SPEC = importlib.util.spec_from_file_location("renderdoc_frame_report", SCRIPT)
assert SPEC and SPEC.loader
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)


class Flags(IntFlag):
    SetMarker = 1
    PushMarker = 2
    PopMarker = 4
    Drawcall = 8
    Dispatch = 16
    Clear = 32
    Copy = 64
    Resolve = 128


class RenderDocJournalJoinTests(unittest.TestCase):
    marker = "REXGLUE_FRAME_DRAW_0000000000000001"

    def marker_action(self, marker: str, event: int) -> dict:
        return {
            "name": marker,
            "event_id": event,
            "kind": "marker",
            "journal_marker": report._journal_marker(marker),
        }

    def resolve(self, following: list[dict]) -> tuple:
        return report._resolve_markers(
            [self.marker_action(self.marker, 10), *following],
            [{"marker": self.marker, "requires_draw": True}],
        )

    def test_journal_selects_issued_draw_and_copy_swap_markers(self) -> None:
        events = [
            {
                "id": 1,
                "kind": "draw",
                "marker": "issued",
                "outcome": {"host_issued": True},
                "host": {
                    "pixel_shader_hash": "0000000000000000",
                    "used_texture_mask": 0,
                },
            },
            {
                "id": 2,
                "kind": "draw",
                "marker": "skipped",
                "outcome": {"host_issued": False},
            },
            {
                "id": 3,
                "kind": "draw",
                "marker": "copy_draw",
                "outcome": {"host_issued": False},
            },
            {"id": 4, "kind": "copy", "marker": "copy"},
            {"id": 5, "kind": "swap", "marker": "swap"},
        ]
        with patch.dict(
            os.environ,
            {"RENDERDOC_EXPECTED_MARKERS_JSON": json.dumps({"events": events})},
        ):
            selected = report._marker_input()
        self.assertEqual(
            [item["marker"] for item in selected], ["issued", "copy", "swap"]
        )
        self.assertEqual(
            [item["requires_draw"] for item in selected], [True, False, False]
        )

    def test_issued_marker_joins_adjacent_draw(self) -> None:
        found, missing = self.resolve([{"event_id": 11, "kind": "draw"}])
        self.assertFalse(missing)
        self.assertEqual(found[0]["associated_event_id"], 11)

    def test_issued_marker_cannot_join_another_guests_draw(self) -> None:
        found, missing = self.resolve(
            [
                self.marker_action("REXGLUE_FRAME_DRAW_0000000000000002", 11),
                {"event_id": 12, "kind": "draw"},
            ]
        )
        self.assertFalse(found)
        self.assertIn("no adjacent draw", missing[0]["reason"])

    def test_issued_marker_cannot_join_dispatch_or_end_of_capture(self) -> None:
        for following in ([], [{"event_id": 11, "kind": "dispatch"}]):
            with self.subTest(following=following):
                found, missing = self.resolve(following)
                self.assertFalse(found)
                self.assertTrue(missing)

    def test_action_flags_do_not_depend_on_enum_text(self) -> None:
        action = SimpleNamespace(
            GetName=lambda: "DrawIndexedInstanced",
            eventId=11,
            actionId=1,
            flags=Flags.Drawcall,
            events=[SimpleNamespace(eventId=11)],
            children=[],
        )
        self.assertEqual(str(action.flags), "8")
        result = report._action_record(action, SimpleNamespace(ActionFlags=Flags))
        self.assertEqual(result["kind"], "draw")
        self.assertEqual(result["event_ids"], [11])
        for flag, kind in ((Flags.Copy, "copy"), (Flags.Resolve, "resolve")):
            action.flags = flag
            self.assertEqual(
                report._action_record(action, SimpleNamespace(ActionFlags=Flags))[
                    "kind"
                ],
                kind,
            )

    def test_issued_pipeline_cannot_succeed_with_empty_dependencies(self) -> None:
        pipeline = {
            "graphics_pipeline": {"id": "pso"},
            "shaders": {},
            "read_only_resources": {},
        }
        with self.assertRaisesRegex(ValueError, "missing its Vertex"):
            report._validate_issued_pipeline(pipeline, {})
        pipeline["shaders"]["Vertex"] = {"shader": {"id": "vs"}}
        report._validate_issued_pipeline(pipeline, {})
        with self.assertRaisesRegex(ValueError, "missing its Pixel"):
            report._validate_issued_pipeline(
                pipeline, {"expected_shader_stages": ["Vertex", "Pixel"]}
            )
        with self.assertRaisesRegex(ValueError, "no shader texture declaration"):
            report._validate_issued_pipeline(pipeline, {"used_texture_mask": 1})
        pipeline["read_only_resources"] = {
            "Vertex": [{"descriptor": {"resource": {"id": "shared_memory"}}}]
        }
        with self.assertRaisesRegex(ValueError, "no shader texture declaration"):
            report._validate_issued_pipeline(
                pipeline, {"used_texture_mask": 1}, {"texture"}
            )
        pipeline["read_only_resources"]["Pixel"] = [
            {"descriptor": {"resource": {"id": "texture"}}}
        ]
        pipeline["shaders"]["Vertex"]["readOnlyResources"] = [{"isTexture": True}]
        report._validate_issued_pipeline(
            pipeline, {"used_texture_mask": 1}, {"texture"}
        )
        self.assertEqual(pipeline["guest_texture_access"]["status"], "accessed")
        pipeline["read_only_resources"]["Pixel"] = []
        report._validate_issued_pipeline(
            pipeline, {"used_texture_mask": 1}, {"texture"}
        )
        self.assertEqual(
            pipeline["guest_texture_access"]["status"],
            "no_dynamic_texture_access_reported",
        )
        pipeline["graphics_pipeline"]["id"] = None
        with self.assertRaisesRegex(ValueError, "no graphics pipeline"):
            report._validate_issued_pipeline(pipeline, {})

    def test_missing_standard_shader_stage_is_an_explicit_error(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "stage is unavailable"):
            report._stage_values(SimpleNamespace(ShaderStage=SimpleNamespace()))


if __name__ == "__main__":
    unittest.main()
