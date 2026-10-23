"""Synthetic ordering, range, form and refusal tests for scene provenance."""

import copy
import hashlib
import importlib.util
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import time
import tomllib
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "scene_analysis", ROOT / "scripts" / "gpu" / "analyze-scene-resources.py"
)
ANALYSIS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYSIS)
# rerevved_tests.exe; each recorder mode runs as `--helper scene-resource.<mode>`.
RECORDER = None
if "--recorder" in sys.argv:
    index = sys.argv.index("--recorder")
    RECORDER = pathlib.Path(sys.argv[index + 1])
    del sys.argv[index : index + 2]


def texture(base=0x1000, fmt=6, **fields):
    return {
        "type": 2,
        "base": base,
        "mip_base": 0,
        "base_bytes": 4096,
        "format": fmt,
        "width": 32,
        "height": 32,
        "depth": 1,
        "sampled": True,
        "slot": 0,
        **fields,
    }


def event(seq, frame, kind, **fields):
    return {"sequence": seq, "frame": frame, "kind": kind, "entry": 1, **fields}


def receipt(draws=1):
    return {
        "complete": True,
        "overflow": 0,
        "begin_frame": 10,
        "end_frame": 12,
        "requested_frames": 2,
        "draws": draws,
    }


class SceneAnalysisTests(unittest.TestCase):
    @unittest.skipUnless(RECORDER, "C++ recorder supplied by the integration CTest")
    def test_cpp_payload_windows(self):
        # Each window's payloads must name allocations its own events define.
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            capture = root / "windows"
            run = subprocess.run(
                [
                    str(RECORDER),
                    "--helper",
                    "scene-resource.payload-windows",
                    str(capture),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(run.returncode, 0, run.stderr + run.stdout)
            for window in (capture, capture / "window-2"):
                output = root / f"{window.name}.json"
                self.assertEqual(
                    ANALYSIS.main(["--capture", str(window), "--output", str(output)]),
                    0,
                )
                graph = json.loads(output.read_text(encoding="ascii"))
                self.assertTrue(graph["payloads"]["accepted"])
                self.assertEqual(graph["payloads"]["unobserved_allocation_bindings"], 0)

    @unittest.skipUnless(RECORDER, "C++ recorder supplied by the integration CTest")
    def test_cpp_payload_capture(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            legacy_writer = None
            for mode in (
                "legacy-schema",
                "payload",
                "payload-gaps",
                "payload-coalescing",
                "payload-allocator",
                "payload-regions",
                "payload-regions-copy",
                "payload-region-failure",
                "payload-region-window",
                "payload-pre-arm",
                "payload-pre-arm-overflow",
                "payload-reuse",
                "payload-overlap",
                "payload-targets",
                "payload-io-failure",
            ):
                capture = root / mode
                run = subprocess.run(
                    [str(RECORDER), "--helper", f"scene-resource.{mode}", str(capture)],
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(run.returncode, 0, run.stderr + run.stdout)
                with (capture / "result.toml").open("rb") as stream:
                    result = tomllib.load(stream)
                self.assertTrue(result["complete"])
                with (capture / "events.toml").open("rb") as stream:
                    events = tomllib.load(stream)["events"]
                output = root / f"{mode}.json"
                self.assertEqual(
                    ANALYSIS.main(["--capture", str(capture), "--output", str(output)]),
                    0,
                )
                graph = json.loads(output.read_text(encoding="ascii"))
                writer = next(
                    e
                    for e in events
                    if e["kind"] == "cpu_writer" and e.get("writer_path") == "A"
                )
                # Compare the full legacy writer schema and values, except the global
                # sequence/frame identities that paired payload rows necessarily shift.
                ignored = {"sequence", "frame", "entry_frame"}
                if mode == "legacy-schema":
                    legacy_writer = {
                        k: v for k, v in writer.items() if k not in ignored
                    }
                    self.assertNotIn("payloads", graph)
                    self.assertNotIn("effects", graph)
                    continue
                self.assertEqual({k: writer[k] for k in legacy_writer}, legacy_writer)
                self.assertTrue(graph["effects"]["accepted"])
                if mode == "payload-io-failure":
                    # A failed optional payload stream is reported, not accepted, and the
                    # metadata analysis still completes.
                    self.assertFalse(graph["payloads"]["retention_complete"])
                    self.assertFalse(graph["payloads"]["accepted"])
                    self.assertTrue(result["payload_failure_reason"])
                    continue
                self.assertEqual(
                    graph["payloads"]["retention_complete"],
                    mode != "payload-gaps",
                )
                self.assertTrue(graph["payloads"]["first_submission_bindings"])
                if mode in ("payload-pre-arm", "payload-pre-arm-overflow"):
                    overflow = mode == "payload-pre-arm-overflow"
                    coverage = graph["payloads"]["writer_c_coverage"]
                    self.assertTrue(coverage)
                    self.assertEqual(
                        result["pre_arm_region_records"], 1 if overflow else 2
                    )
                    self.assertEqual(result["pre_arm_region_overflow"], int(overflow))
                    self.assertEqual(result["region_records"], 0)
                    self.assertEqual(
                        graph["payloads"]["writer_c_coverage_complete"], not overflow
                    )
                    for use in coverage:
                        self.assertEqual(use["closed"], not overflow)
                        self.assertEqual(use["replay_matches_sampled"], not overflow)
                        if overflow:
                            self.assertIn(
                                "pre-arm region budget overflow", use["reason"]
                            )
                        else:
                            self.assertEqual(use["covered_texels"], 1024 * 1024)
                            self.assertEqual(use["uncovered_texels"], 0)
                    writer = next(e for e in events if e.get("pre_arm_regions"))
                    self.assertLess(writer["frame"], result["begin_frame"])
                if mode == "payload-overlap":
                    self.assertEqual(graph["payloads"]["allocation_overlaps"], 1)
                    self.assertEqual(graph["payloads"]["allocation_overflow"], 0)
                    self.assertTrue(graph["payloads"]["accepted"])
                if mode == "payload":
                    # Lose each optional stream in turn; the metadata analysis must survive.
                    (capture / "route-effects.bin").unlink()
                    lost = root / "lost-effects.json"
                    self.assertEqual(
                        ANALYSIS.main(
                            ["--capture", str(capture), "--output", str(lost)]
                        ),
                        0,
                    )
                    lost_graph = json.loads(lost.read_text(encoding="ascii"))
                    self.assertFalse(lost_graph["effects"]["accepted"])
                    self.assertTrue(lost_graph["payloads"]["accepted"])
                    self.assertEqual(
                        lost_graph["texture_summary"], graph["texture_summary"]
                    )
                    manifest = capture / "payload-manifest.toml"
                    data = manifest.read_bytes()
                    cut_at = data.rfind(b"[[bindings]]") + 8
                    manifest.write_bytes(data[:cut_at])
                    truncated = root / "truncated-manifest.json"
                    self.assertEqual(
                        ANALYSIS.main(
                            ["--capture", str(capture), "--output", str(truncated)]
                        ),
                        0,
                    )
                    cut = json.loads(truncated.read_text(encoding="ascii"))
                    self.assertFalse(cut["payloads"]["accepted"])
                    self.assertEqual(cut["texture_summary"], graph["texture_summary"])
                    continue
                sequences = {e["sequence"]: e for e in events}
                with (capture / "payload-manifest.toml").open("rb") as stream:
                    rows = tomllib.load(stream)["bindings"]
                for row in rows:
                    self.assertIn(row["event"], sequences)
                    self.assertEqual(row["frame"], sequences[row["event"]]["frame"])
                self.assertTrue(any(r["role"] == "cpu_update" for r in rows))
                if mode == "payload-gaps":
                    self.assertTrue(
                        any(
                            use.get("gap") == "unsupported_texture_layout"
                            for e in events
                            for use in e.get("payload_texture_uses", [])
                        )
                    )
                    self.assertTrue(
                        any(e.get("payload_fetch_mask_conservative") for e in events)
                    )
                    self.assertTrue(
                        any(
                            e.get("payload_error")
                            == "scene_direct_lowerer_geometry_unavailable"
                            for e in events
                        )
                    )
                    self.assertTrue(
                        any(
                            e.get("payload_gap")
                            == "scene_resolve_allocation_unobserved"
                            for e in events
                        )
                    )

    def test_effect_header_and_failure_acceptance(self):
        with tempfile.TemporaryDirectory() as temporary:
            capture = pathlib.Path(temporary)
            result = dict(
                effect_file="route-effects.bin",
                effect_byte_budget=10 * 1024 * 1024,
                effect_bytes=0,
                effect_lost_records=0,
                effect_failed_reads=0,
                effect_gaps=0,
                effect_unfinished_calls=0,
                effect_unchanged_fields=0,
                effect_complete=True,
            )
            path = capture / "route-effects.bin"

            def header(**changes):
                values = dict(size=0, lost=0, reads=0, gaps=0, unfinished=0, readers=0)
                values.update(changes)
                return struct.pack("<8sII6Q", b"RREVFX01", 1, 64, *values.values())

            path.write_bytes(header())
            self.assertTrue(ANALYSIS.effect_summary(capture, result)["accepted"])
            self.assertIsNone(ANALYSIS.effect_summary(capture, receipt()))
            for field, counter in (
                ("lost_records", "lost"),
                ("failed_reads", "reads"),
                ("gaps", "gaps"),
                ("unfinished_calls", "unfinished"),
            ):
                with self.subTest(field=field):
                    path.write_bytes(header(**{counter: 1}))
                    failed = {**result, f"effect_{field}": 1, "effect_complete": False}
                    self.assertFalse(
                        ANALYSIS.effect_summary(capture, failed)["accepted"]
                    )
                    failed["effect_complete"] = True
                    with self.assertRaisesRegex(ValueError, "contradicts failures"):
                        ANALYSIS.effect_summary(capture, failed)
                    with self.assertRaisesRegex(ValueError, "differs from header"):
                        ANALYSIS.effect_summary(capture, result)
            for invalid in (
                b"short",
                b"BADMAGIC" + header()[8:],
                header(size=1),
                header(readers=1 << 33),
                header()[:8] + struct.pack("<I", 2) + header()[12:],
            ):
                path.write_bytes(invalid)
                with self.assertRaisesRegex(ValueError, "effect"):
                    ANALYSIS.effect_summary(capture, result)
            record = struct.pack(
                "<IHH5QIIQII", 72, 2, 0, 1, 1, 1, 0, 1, 0x826A39F8, 0x1000, 0, 31, 31
            )
            path.write_bytes(header(size=len(record)) + record)
            counted = {
                **result,
                "effect_bytes": len(record),
                "effect_unchanged_fields": 31,
            }
            self.assertEqual(
                ANALYSIS.effect_summary(capture, counted)["unchanged_fields"], 31
            )
            path.write_bytes(header(size=48) + record[:48])
            with self.assertRaisesRegex(ValueError, "invalid effect record"):
                ANALYSIS.effect_summary(capture, {**counted, "effect_bytes": 48})

    def test_missing_effect_file_is_rejected_not_fatal(self):
        with tempfile.TemporaryDirectory() as temporary:
            capture = pathlib.Path(temporary)
            result = dict(
                effect_file="route-effects.bin",
                effect_byte_budget=10 * 1024 * 1024,
                effect_complete=False,
                effect_failure_reason="effect_file_write",
            )
            summary = ANALYSIS.effect_summary(capture, result)
            self.assertFalse(summary["accepted"])
            self.assertEqual(summary["reported_failure"], "effect_file_write")
            self.assertIn("unreadable", summary["rejection_reason"])
            # An invalid stream is rejected only when the producer reported the failure.
            (capture / "route-effects.bin").write_bytes(b"short")
            self.assertFalse(ANALYSIS.effect_summary(capture, result)["accepted"])
            with self.assertRaisesRegex(ValueError, "effect"):
                ANALYSIS.effect_summary(
                    capture, {**result, "effect_failure_reason": ""}
                )
            # The CLI still writes the metadata analysis around a rejected stream.
            events = capture / "events.toml"
            events.write_text(
                "schema = 1\n[[events]]\nsequence=1\nframe=10\nkind='draw'\nentry=1\n",
                encoding="ascii",
            )
            (capture / "route-effects.bin").unlink()
            receipt_fields = dict(receipt(), **result)
            (capture / "result.toml").write_text(
                "".join(f"{k} = {json.dumps(v)}\n" for k, v in receipt_fields.items()),
                encoding="ascii",
            )
            output = capture / "out.json"
            with mock.patch.object(ANALYSIS, "build_graph", return_value={}):
                self.assertEqual(
                    ANALYSIS.main(["--capture", str(capture), "--output", str(output)]),
                    0,
                )
            graph = json.loads(output.read_text(encoding="ascii"))
            self.assertFalse(graph["effects"]["accepted"])
            text = ANALYSIS.render(
                {
                    "window": {"begin_frame": 10, "end_frame": 12},
                    "sampling_evidence": "s",
                    "range_evidence": "r",
                    "texture_summary": [],
                    "attachments": [],
                    "window_resolves": [],
                    "open_native_producers": [],
                    "payloads": {"rejection_reason": "payload fixture"},
                    "effects": graph["effects"],
                }
            )
            self.assertIn("Rejected: payload fixture.", text)
            self.assertIn("Rejected: effect file unreadable.", text)

    def test_truncated_payload_manifest_is_rejected_not_fatal(self):
        with tempfile.TemporaryDirectory() as temporary:
            capture = pathlib.Path(temporary)
            result = dict(
                receipt(),
                capture_id="fixture",
                payload_manifest="payload-manifest.toml",
                payload_complete=False,
                payload_failure_reason="payload_blob_write",
            )
            events = [event(5, 10, "draw", call_id=5, submission_id=5)]
            for content in (None, "schema=1\ncapture_id='fix"):
                with self.subTest(content=content):
                    manifest = capture / "payload-manifest.toml"
                    if content is None:
                        manifest.unlink(missing_ok=True)
                    else:
                        manifest.write_text(content, encoding="ascii")
                    summary = ANALYSIS.payload_summary(capture, events, result)
                    self.assertFalse(summary["accepted"])
                    self.assertFalse(summary["retention_complete"])
                    self.assertEqual(summary["reported_failure"], "payload_blob_write")
                    # Unparsable or missing input is rejected even with no reported failure.
                    quiet = {**result, "payload_failure_reason": ""}
                    self.assertFalse(
                        ANALYSIS.payload_summary(capture, events, quiet)["accepted"]
                    )
            # A parsable manifest that contradicts the events is a hard error unless reported.
            manifest.write_text(
                "schema=1\ncapture_id='other'\nbyte_budget=0\n", encoding="ascii"
            )
            with self.assertRaisesRegex(ValueError, "capture identity"):
                ANALYSIS.payload_summary(
                    capture, events, {**result, "payload_failure_reason": ""}
                )
            self.assertFalse(
                ANALYSIS.payload_summary(capture, events, result)["accepted"]
            )

    def test_payload_binding_dedup_and_first_submission(self):
        with tempfile.TemporaryDirectory() as temporary:
            capture = pathlib.Path(temporary)
            (capture / "payloads").mkdir()
            digest = hashlib.sha256(b"abc").hexdigest()
            (capture / "payloads" / f"{digest}.bin").write_bytes(b"abc")
            row = (
                "\n[[bindings]]\ncapture_id='fixture'\nframe=10\nevent=5\n"
                "submission=5\nparent_call=0\ngeneration=7\nversion=5\n"
                "address=4096\nbytes=3\nresource='vertex:4096'\nrole='vertices'\n"
                "physical=true\nallocation_generation=0\n"
                f"sha256='{digest}'\nfile='payloads/{digest}.bin'\nerror=''\n"
            )
            manifest = (
                "schema=1\ncapture_id='fixture'\nbyte_budget=3\n"
                + row
                + row.replace("generation=7", "generation=8")
            )
            (capture / "payload-manifest.toml").write_text(manifest, encoding="ascii")
            events = [
                event(5, 10, "draw", capture_id="fixture", call_id=5, submission_id=5)
            ]
            result = dict(
                receipt(),
                capture_id="fixture",
                payload_manifest="payload-manifest.toml",
                payload_bytes=3,
                payload_complete=False,
                payload_known_gaps=1,
            )
            summary = ANALYSIS.payload_summary(capture, events, result)
            self.assertEqual(summary["unique_files"], 1)
            self.assertEqual(summary["published_bytes"], 3)
            self.assertEqual(len(summary["first_submission_bindings"]), 2)
            self.assertFalse(summary["executable_ready"])
            self.assertEqual(summary["unobserved_allocation_bindings"], 2)
            for counter in (
                "payload_allocator_gaps",
                "region_failures",
                "allocation_failures",
                "allocation_overflow",
            ):
                retained = ANALYSIS.payload_summary(
                    capture,
                    events,
                    {
                        **result,
                        "payload_complete": True,
                        "payload_known_gaps": 0,
                        counter: 1,
                    },
                )
                self.assertTrue(retained["accepted"])
                self.assertTrue(retained["retention_complete"])
                self.assertTrue(retained["writer_c_coverage_complete"])
            for applicable in (True, False):
                with mock.patch.object(
                    ANALYSIS,
                    "writer_c_coverage",
                    return_value=[dict(applicable=applicable, closed=False)],
                ):
                    retained = ANALYSIS.payload_summary(
                        capture,
                        events,
                        {**result, "payload_complete": True, "payload_known_gaps": 0},
                    )
                self.assertTrue(retained["accepted"])
                self.assertEqual(retained["writer_c_coverage_complete"], not applicable)
            # A call ID carried by a return is not an event sequence on its own.
            events.append(event(7, 10, "cpu_writer", call_id=6, entry_frame=10))
            for old, new, reason in (
                ("capture_id='fixture'", "capture_id='other'", "capture identity"),
                ("event=5", "event=6", "event identity"),
                ("frame=10", "frame=11", "frame identity"),
                ("submission=5", "submission=6", "submission identity"),
                ("bytes=3", "bytes=2", "size mismatch"),
                ("generation=7", "generation=0", "resource identity"),
                (
                    f"file='payloads/{digest}.bin'",
                    "file='../outside.bin'",
                    "path differs",
                ),
            ):
                with self.subTest(reason=reason):
                    (capture / "payload-manifest.toml").write_text(
                        manifest.replace(old, new), encoding="ascii"
                    )
                    with self.assertRaisesRegex(ValueError, reason):
                        ANALYSIS.payload_summary(capture, events, result)
            (capture / "payload-manifest.toml").write_text(manifest, encoding="ascii")
            (capture / "payloads" / f"{digest}.bin").write_bytes(b"abd")
            with self.assertRaisesRegex(ValueError, "digest mismatch"):
                ANALYSIS.payload_summary(capture, events, result)

    def test_payload_overflow_and_disabled_format(self):
        with tempfile.TemporaryDirectory() as temporary:
            capture = pathlib.Path(temporary)
            self.assertIsNone(ANALYSIS.payload_summary(capture, [], receipt()))
            (capture / "payload-manifest.toml").write_text(
                "schema=1\ncapture_id='fixture'\nbyte_budget=0\n"
                "[[bindings]]\ncapture_id='fixture'\nframe=10\nevent=5\n"
                "submission=5\nparent_call=0\ngeneration=1\nversion=5\n"
                "address=4096\nbytes=3\nresource='vertex:4096'\nrole='vertices'\n"
                "sha256=''\nerror='payload_byte_budget'\n",
                encoding="ascii",
            )
            events = [event(5, 10, "draw", call_id=5, submission_id=5)]
            result = dict(
                receipt(),
                capture_id="fixture",
                payload_manifest="payload-manifest.toml",
                payload_bytes=0,
                payload_complete=False,
                payload_overflow=1,
            )
            self.assertEqual(
                ANALYSIS.payload_summary(capture, events, result)["failed_bindings"], 1
            )
            result["payload_complete"] = True
            self.assertFalse(
                ANALYSIS.payload_summary(capture, events, result)["accepted"]
            )

    def test_coverage_union(self):
        self.assertEqual(ANALYSIS.covered_texels(4, 4, [[0, 0, 4, 4]]), 16)
        self.assertEqual(
            ANALYSIS.covered_texels(4, 4, [[0, 0, 3, 2], [1, 1, 4, 3]]), 10
        )
        self.assertEqual(
            ANALYSIS.covered_texels(4, 4, [[-2, -2, 2, 2], [4, 0, 5, 2]]), 4
        )
        self.assertEqual(ANALYSIS.covered_texels(4, 4, []), 0)
        with self.assertRaises(ValueError):
            ANALYSIS.covered_texels(4, 4, [[3, 0, 2, 4]])

    def test_region_coverage_requires_retained_rows(self):
        tex = texture(fmt=2, width=4, height=4, base_bytes=16, pitch=4)
        use = dict(
            slot=0,
            resource="texture:1",
            generation=1,
            version=3,
            plane="base",
            texture=tex,
            allocation_generation=1,
        )
        writer = event(
            3,
            10,
            "cpu_writer",
            writer_path="C",
            region_count=1,
            mip=0,
            payload_updates=[dict(resource="texture:1", generation=1, version=3)],
        )
        draw = event(4, 10, "draw", payload_texture_uses=[use])
        base = dict(
            event=3,
            resource="texture:1",
            generation=1,
            version=3,
            physical=True,
            address=4096,
            bytes=4,
            sha256="a" * 64,
            error="",
            allocation_generation=1,
        )
        # A fallback's final-image snapshot does not prove intermediate rows.
        final_only = {**base, "role": "cpu_update", "bytes": 16}
        summary = ANALYSIS.writer_c_coverage([writer, draw], [final_only])[0]
        self.assertEqual(
            (summary["covered_texels"], summary["uncovered_texels"]), (0, 16)
        )
        self.assertFalse(summary["closed"])
        writer["region_updates"] = [
            dict(
                index=0,
                destination_rect=[0, 0, 4, 2],
                source_rect=[0, 0, 4, 2],
                rows_written=2,
                row_bytes=4,
                fallback=True,
                rows=[
                    dict(row=i, bytes=4, sha256="a" * 64, error="") for i in range(2)
                ],
            )
        ]
        rows = [
            {**base, "role": f"writer_C_region:0:{i}", "address": 4096 + i * 4}
            for i in range(2)
        ]
        summary = ANALYSIS.writer_c_coverage([writer, draw], rows)[0]
        self.assertEqual(
            (summary["covered_texels"], summary["uncovered_texels"]), (8, 8)
        )
        initial = {
            **base,
            "event": 1,
            "version": 1,
            "role": "creation_initial_bytes",
            "bytes": 16,
        }
        summary = ANALYSIS.writer_c_coverage([writer, draw], [initial, *rows])[0]
        self.assertTrue(summary["closed"])
        rows[0]["sha256"] = "b" * 64
        self.assertFalse(
            ANALYSIS.writer_c_coverage([writer, draw], [initial, *rows])[0]["closed"]
        )
        use["generation"] = 2
        use["history_candidate"] = True
        self.assertEqual(
            ANALYSIS.writer_c_coverage([writer, draw], [initial, *rows])[0][
                "covered_texels"
            ],
            0,
        )

    def test_writer_c_version_and_byte_replay(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = pathlib.Path(directory)
            (capture / "payloads").mkdir()

            def payload(data, role, version, sequence, address=4096):
                digest = hashlib.sha256(data).hexdigest()
                name = f"payloads/{digest}.bin"
                (capture / name).write_bytes(data)
                return dict(
                    event=sequence,
                    resource="texture:1",
                    generation=1,
                    allocation_generation=1,
                    version=version,
                    physical=True,
                    address=address,
                    bytes=len(data),
                    sha256=digest,
                    file=name,
                    role=role,
                    error="",
                )

            initial = payload(b"abcd", "creation_initial_bytes", 1, 1)
            region_row = payload(b"XY", "writer_C_region:0:0", 3, 3)
            sampled = payload(b"XYcd", "sampled_base", 3, 4)
            region = dict(
                index=0,
                destination_rect=[0, 0, 2, 1],
                source_rect=[0, 0, 2, 1],
                rows_written=1,
                row_bytes=2,
                rows=[dict(row=0, bytes=2, sha256=region_row["sha256"], error="")],
            )
            update = dict(resource="texture:1", generation=1, version=3)
            writer = event(
                3,
                10,
                "cpu_writer",
                writer_path="C",
                mip=0,
                region_count=1,
                region_updates=[region],
                payload_updates=[update],
            )
            use = dict(
                slot=0,
                resource="texture:1",
                generation=1,
                version=3,
                allocation_generation=1,
                plane="base",
                texture=texture(fmt=2, width=2, height=2, pitch=2, base_bytes=4),
            )
            draw = event(4, 10, "draw", payload_texture_uses=[use])
            rows = [initial, region_row, sampled]

            def coverage(events=None, bindings=None):
                return ANALYSIS.writer_c_coverage(
                    events or [writer, draw], bindings or rows, capture
                )[0]

            self.assertTrue(coverage()["closed"])
            self.assertTrue(coverage()["replay_matches_sampled"])
            with_mips = payload(b"abcdMIPS", "creation_initial_bytes", 1, 1)
            self.assertTrue(
                coverage(bindings=[with_mips, region_row, sampled])["closed"]
            )
            use["pre_arm_region_overflow"] = True
            result = coverage()
            self.assertFalse(result["closed"])
            self.assertFalse(result["region_history_complete"])
            self.assertTrue(result["replay_matches_sampled"])
            self.assertIn("pre-arm region budget overflow", result["reason"])
            use["pre_arm_region_overflow"] = False
            pre_arm = {**writer, "frame": 0, "pre_arm_regions": True}
            later_row = payload(b"Z", "writer_C_region:0:0", 4, 4, address=4097)
            later_region = {
                **region,
                "destination_rect": [1, 0, 2, 1],
                "source_rect": [0, 0, 1, 1],
                "row_bytes": 1,
                "rows": [dict(row=0, bytes=1, sha256=later_row["sha256"], error="")],
            }
            later_writer = {
                **writer,
                "sequence": 4,
                "region_updates": [later_region],
                "payload_updates": [{**update, "version": 4}],
            }
            later_draw = {
                **draw,
                "sequence": 5,
                "payload_texture_uses": [{**use, "version": 4}],
            }
            later_sampled = payload(b"XZcd", "sampled_base", 4, 5)
            result = coverage(
                [later_writer, pre_arm, later_draw],
                [initial, region_row, later_row, later_sampled],
            )
            self.assertTrue(result["closed"])
            self.assertTrue(result["replay_matches_sampled"])
            for path in ("A", "B", "upload"):
                other = event(
                    2,
                    10,
                    "cpu_writer",
                    writer_path=path,
                    payload_updates=[{**update, "version": 2}],
                )
                result = coverage([other, writer, draw])
                self.assertFalse(result["closed"])
                self.assertEqual(result["unaccounted_versions"], [2])
            bump = event(2, 10, "draw_post", payload_updates=[{**update, "version": 2}])
            self.assertEqual(
                coverage([bump, writer, draw])["unaccounted_versions"], [2]
            )
            use["version"] = 5
            self.assertEqual(coverage()["unaccounted_versions"], [5])
            use["version"] = 3
            mismatch = payload(b"XYzz", "sampled_base", 3, 4)
            result = coverage(bindings=[initial, region_row, mismatch])
            self.assertFalse(result["closed"])
            self.assertIn("hash mismatch", result["reason"])
            # Empty-width rectangles write no rows, even when their height is positive.
            region.update(
                destination_rect=[0, 0, 0, 2],
                source_rect=[0, 0, 0, 2],
                rows_written=0,
                row_bytes=0,
                rows=[],
            )
            unchanged = payload(b"abcd", "sampled_base", 3, 4)
            self.assertTrue(coverage(bindings=[initial, unchanged])["closed"])
            use["plane"] = "mip_base"
            self.assertEqual(coverage()["status"], "not_applicable")
            use["plane"] = "base"
            use["gpu_produced"] = True
            self.assertFalse(coverage()["applicable"])
            use["gpu_produced"] = False
            use["texture"]["tiled"] = True
            self.assertEqual(coverage()["status"], "not_applicable")
            graph = ANALYSIS.build_graph(
                [event(4, 10, "draw", bindings=[texture()])], receipt()
            )
            graph["payloads"] = dict(
                capture_id="fixture",
                unique_files=3,
                published_bytes=10,
                retention_complete=True,
                known_gaps=0,
                allocator_lifetime_gaps=0,
                failures=0,
                writer_c_coverage_complete=True,
                writer_c_coverage=[coverage()],
            )
            self.assertIn("n/a | n/a | not_applicable", ANALYSIS.render(graph))
            self.assertEqual(ANALYSIS.writer_c_coverage([draw], rows, capture), [])
            use["history_candidate"] = True
            self.assertEqual(
                ANALYSIS.writer_c_coverage([draw], rows, capture)[0]["status"],
                "not_applicable",
            )

    def test_allocation_reuse_validation(self):
        events = [
            event(1, 0, "allocation", allocation_generation=1, address=100, bytes=20),
            event(3, 0, "allocation_free", allocation_generation=1, address=100),
            event(4, 0, "allocation", allocation_generation=2, address=100, bytes=20),
        ]
        row = dict(
            allocation_generation=2, event=5, address=100, bytes=20, physical=True
        )
        self.assertEqual(ANALYSIS.validate_allocation_generations(events, [row]), 2)
        with self.assertRaisesRegex(ValueError, "lifetime"):
            ANALYSIS.validate_allocation_generations(
                events, [{**row, "allocation_generation": 1}]
            )
        with self.assertRaisesRegex(ValueError, "lifetime"):
            ANALYSIS.validate_allocation_generations(events, [{**row, "bytes": 21}])

    def test_allocation_overlap_ends_old_lifetime(self):
        events = [
            event(1, 0, "allocation", allocation_generation=1, address=100, bytes=20),
            event(3, 0, "allocation", allocation_generation=2, address=110, bytes=20),
            event(4, 0, "allocation", allocation_generation=3, address=130, bytes=20),
            event(6, 0, "allocation", allocation_generation=4, address=115, bytes=30),
            event(8, 0, "allocation_free", allocation_generation=4, address=115),
        ]
        old = dict(
            allocation_generation=1, event=2, address=100, bytes=4, physical=True
        )
        current = dict(
            allocation_generation=2, event=5, address=110, bytes=20, physical=True
        )
        self.assertEqual(
            ANALYSIS.validate_allocation_generations(events, [old, current]), 4
        )
        with self.assertRaisesRegex(ValueError, "lifetime"):
            ANALYSIS.validate_allocation_generations(events, [{**old, "event": 3}])
        with self.assertRaisesRegex(ValueError, "lifetime"):
            ANALYSIS.validate_allocation_generations(events, [{**current, "event": 6}])
        with self.assertRaisesRegex(ValueError, "lifetime"):
            ANALYSIS.validate_allocation_generations(
                events,
                [
                    {
                        **current,
                        "allocation_generation": 3,
                        "address": 145,
                        "bytes": 4,
                        "event": 6,
                    }
                ],
            )

    def test_target_receipts_and_rejection(self):
        with tempfile.TemporaryDirectory() as temporary:
            capture = pathlib.Path(temporary)
            (capture / "targets").mkdir()
            data = b"abcd"
            digest = hashlib.sha256(data).hexdigest()
            manifest = (
                "schema=1\ncapture_id='fixture'\nbyte_budget=12\nrecord_budget=3\n"
            )
            for index, (kind, frame) in enumerate(
                (("seed", 10), ("reference", 10), ("reference", 11))
            ):
                (capture / f"targets/{index}.bin").write_bytes(data)
                manifest += (
                    f"[[records]]\nkind='{kind}'\nframe={frame}\nresource='target:1'\ngeneration=2\ncompletion=3\n"
                    "aspect='color'\nformat='RGBA8'\nsample_layout='pixel-major sample-0'\nresolve='average'\n"
                    "rectangle=[0,0,1,1]\nrow_bytes=4\nrows=1\nsamples=1\nbytes_per_sample=4\nbytes=4\nerror=''\n"
                    f"file='targets/{index}.bin'\nsha256='{digest}'\n"
                )
            (capture / "target-manifest.toml").write_text(manifest, encoding="ascii")
            result = dict(
                receipt(),
                capture_id="fixture",
                target_manifest="target-manifest.toml",
                target_records=3,
                target_record_budget=3,
                target_byte_budget=12,
                target_bytes=12,
                target_complete=True,
            )
            self.assertTrue(ANALYSIS.target_summary(capture, result)["accepted"])
            for counter in ("target_failures", "target_gaps", "target_overflow"):
                self.assertFalse(
                    ANALYSIS.target_summary(capture, {**result, counter: 1})["accepted"]
                )
            (capture / "targets/2.bin").write_bytes(b"abce")
            self.assertFalse(ANALYSIS.target_summary(capture, result)["accepted"])

    def writer(self, seq, path="A", begin=0x1000, size=32, **fields):
        return event(
            seq,
            1,
            "cpu_writer",
            writer_path=path,
            destination_ranges=[
                {
                    "begin": begin,
                    "end": begin + size,
                    "bytes": size,
                    "guest_address": 0xA0000000 + begin,
                    "destination_sha256": "fixture",
                }
            ],
            **fields,
        )

    def test_cpu_writer_paths_and_mip_overlap(self):
        for path in ("A", "B", "C", "D", "copy"):
            binding = texture(mip_base=0x4000, mip_bytes=4096, max_mip=2)
            graph = ANALYSIS.build_graph(
                [
                    event(1, 0, "creation", object=0x4026FAC4, texture=binding),
                    self.writer(2, path, begin=0x4FF0, size=32),
                    event(3, 10, "draw", bindings=[binding]),
                ],
                receipt(),
            )
            summary = graph["texture_summary"][0]
            self.assertEqual(summary["producer_classes"], {"CPU writer": 1})
            self.assertEqual(summary["cpu_writers"], [{"sequence": 2, "path": path}])
            self.assertEqual(graph["producers"][-1]["consumers"], [0])
            self.assertIn(path, ANALYSIS.render(graph))
            self.assertEqual(
                graph["open_native_producers"][0]["cpu_writer_status"], "observed"
            )

    def test_cpu_writer_earlier_creation_is_not_reused(self):
        for obj in (0x4026FAC4, 0x4026FAF8):
            graph = ANALYSIS.build_graph(
                [
                    event(1, 0, "creation", object=obj, texture=texture()),
                    self.writer(2),
                    event(3, 1, "creation", object=obj, texture=texture()),
                    event(4, 10, "draw", bindings=[texture()]),
                ],
                receipt(),
            )
            self.assertEqual(graph["textures"][0]["cpu_writers"], [])
            self.assertEqual(graph["textures"][0]["uses"][0]["producer_sequence"], 3)
            self.assertIn("CPU writer still open", ANALYSIS.render(graph))
            graph = ANALYSIS.build_graph(
                [
                    event(1, 0, "creation", object=obj, texture=texture()),
                    self.writer(2),
                    event(3, 1, "creation", object=obj, texture=texture()),
                    self.writer(4, "B"),
                    event(5, 10, "draw", bindings=[texture()]),
                ],
                receipt(),
            )
            self.assertEqual(
                graph["textures"][0]["cpu_writers"], [{"sequence": 4, "path": "B"}]
            )

    def test_cpu_writer_outside_release_failure_and_empty_stay_open(self):
        variants = [
            [self.writer(2, begin=0x2000)],
            [event(2, 0, "release", object=1), self.writer(3)],
            [self.writer(2), event(3, 1, "release", object=1)],
            [self.writer(2, return_status=-1)],
            [
                event(
                    2,
                    1,
                    "cpu_writer",
                    writer_path="C",
                    destination_ranges=[],
                    destination_open=True,
                )
            ],
        ]
        for middle in variants:
            graph = ANALYSIS.build_graph(
                [
                    event(1, 0, "creation", object=1, texture=texture()),
                    *middle,
                    event(4, 10, "draw", bindings=[texture()]),
                ],
                receipt(),
            )
            self.assertEqual(graph["texture_summary"][0]["cpu_writers"], [])
            self.assertEqual(
                graph["open_native_producers"][0]["cpu_writer_status"], "open"
            )

    def test_cpu_writer_invalid_hashed_extent_is_rejected(self):
        writer = self.writer(2)
        writer["destination_ranges"][0]["bytes"] = 31
        with self.assertRaisesRegex(ValueError, "hashed extent"):
            ANALYSIS.build_graph(
                [
                    event(1, 0, "creation", object=1, texture=texture()),
                    writer,
                    event(3, 10, "draw", bindings=[texture()]),
                ],
                receipt(),
            )

    def test_upload_nested_copy_remains_cpu_upload(self):
        graph = ANALYSIS.build_graph(
            [
                event(1, 0, "creation", object=1, texture=texture()),
                event(2, 1, "upload", object=1, texture=texture()),
                self.writer(3, "copy", caller_lr=str(0x82579360)),
                event(4, 10, "draw", bindings=[texture()]),
            ],
            receipt(),
        )
        use = graph["textures"][0]["uses"][0]
        self.assertEqual(use["producer_sequence"], 2)
        self.assertEqual(use["producer_class"], "CPU upload")
        self.assertTrue(use["proved_cpu_upload"])
        self.assertEqual(use["overwritten_by"], [])
        self.assertEqual(use["cpu_writers"], [])

    def test_cpu_writer_storage_reused_by_another_object(self):
        graph = ANALYSIS.build_graph(
            [
                event(1, 0, "creation", object=1, texture=texture()),
                self.writer(2),
                event(3, 1, "creation", object=2, texture=texture()),
                event(4, 10, "draw", bindings=[texture()]),
                self.writer(5, "B"),
                event(6, 11, "draw", bindings=[texture()]),
            ],
            receipt(2),
        )
        first, second = graph["textures"][0]["uses"]
        self.assertEqual(first["producer_sequence"], 3)
        self.assertEqual(first["cpu_writers"], [])
        self.assertEqual(first["overwritten_by"], [])
        self.assertEqual(second["cpu_writers"], [{"sequence": 5, "path": "B"}])

    def test_unreadable_writer_keeps_complete_graph_open(self):
        writer = self.writer(2, read_error="unreadable", failure_relevant=False)
        writer["destination_ranges"] = []
        graph = ANALYSIS.build_graph(
            [
                event(1, 0, "creation", object=1, texture=texture()),
                writer,
                event(3, 10, "draw", bindings=[texture()]),
            ],
            {**receipt(), "read_failures": 1, "relevant_read_failures": 0},
        )
        self.assertEqual(len(graph["ignored_read_failures"]), 1)
        self.assertEqual(graph["textures"][0]["uses"][0]["producer_sequence"], 1)
        self.assertEqual(graph["open_native_producers"][0]["cpu_writer_status"], "open")

    def test_writer_with_hashed_destination_survives_source_error(self):
        graph = ANALYSIS.build_graph(
            [
                event(1, 0, "creation", object=1, texture=texture()),
                self.writer(2, read_error="unreadable", failure_relevant=False),
                event(3, 10, "draw", bindings=[texture()]),
            ],
            {**receipt(), "read_failures": 1, "relevant_read_failures": 0},
        )
        self.assertEqual(len(graph["ignored_read_failures"]), 1)
        self.assertEqual(
            graph["texture_summary"][0]["cpu_writers"],
            [{"sequence": 2, "path": "A", "read_error": "unreadable"}],
        )
        self.assertEqual(
            graph["open_native_producers"][0]["cpu_writer_status"], "observed"
        )

    def test_latest_prior_compatible_resolve(self):
        events = [
            event(1, 0, "creation", object=1, texture=texture()),
            event(2, 9, "resolve", object=1, texture=texture()),
            event(3, 10, "resolve", object=1, texture=texture(fmt=18)),
            event(4, 10, "draw", bindings=[texture()]),
            event(5, 11, "resolve", object=1, texture=texture()),
            event(6, 11, "draw", bindings=[texture()]),
        ]
        graph = ANALYSIS.build_graph(list(reversed(events)), receipt(2))
        uses = graph["textures"][0]["uses"]
        self.assertEqual(uses[0]["producer_sequence"], 2)
        self.assertEqual(uses[0]["producer_class"], "earlier resolve")
        self.assertEqual(uses[0]["overwritten_by"], [3])
        self.assertEqual(uses[0]["overlapping_producers"], [3, 1])
        self.assertTrue(uses[0]["ambiguous"])
        self.assertEqual(uses[0]["open_category"], "ambiguous overwritten producer")
        self.assertEqual(uses[1]["producer_sequence"], 5)
        self.assertEqual(uses[1]["producer_class"], "in-window resolve")
        self.assertEqual(graph["window_resolves"][-1]["consumers"], [0])
        self.assertFalse(uses[1]["ambiguous"])

    def test_unproved_upload_target(self):
        for fields in (
            {"raw_target": True},
            {
                "target_source": "raw direct target descriptor interpretation; kind unproved"
            },
        ):
            graph = ANALYSIS.build_graph(
                [
                    event(1, 0, "upload", texture=texture(), **fields),
                    event(2, 10, "draw", bindings=[texture()]),
                ],
                receipt(),
            )
            use = graph["textures"][0]["uses"][0]
            self.assertEqual(use["producer_class"], "unproved upload target")
            self.assertTrue(use["raw_target"])
            self.assertFalse(use["proved_cpu_upload"])

    def test_unbound_demand_is_open(self):
        graph = ANALYSIS.build_graph(
            [event(1, 10, "draw", bindings=[texture(type=0, slot=7), texture()])],
            receipt(),
        )
        self.assertEqual(len(graph["textures"]), 1)
        self.assertEqual(graph["unbound_demands"][0]["slot"], 7)
        self.assertEqual(graph["open_native_producers"][-1]["texture"], None)
        self.assertIn("Draw 1 slot 7", ANALYSIS.render(graph))

    def test_only_relevant_failures_refused(self):
        events = [
            event(1, 0, "other", read_error="unreadable", failure_relevant=False),
            event(2, 0, "rejected_by_guest"),
            event(3, 10, "draw", bindings=[texture()]),
        ]
        graph = ANALYSIS.build_graph(events, receipt())
        self.assertEqual(len(graph["ignored_read_failures"]), 1)
        events[0]["failure_relevant"] = True
        with self.assertRaises(ValueError):
            ANALYSIS.build_graph(events, receipt())

    def test_gamma_edram_base_format(self):
        self.assertTrue(ANALYSIS.compatible(62, 6))
        graph = ANALYSIS.build_graph(
            [
                event(1, 0, "resolve", texture=texture(fmt=62)),
                event(2, 10, "draw", bindings=[texture()]),
            ],
            receipt(),
        )
        self.assertEqual(graph["textures"][0]["uses"][0]["producer_sequence"], 1)

    def test_upload_import_unknown_and_alias(self):
        bindings = [texture(), texture(0x2000), texture(0x3000), texture(endian=2)]
        graph = ANALYSIS.build_graph(
            [
                event(
                    1, 0, "upload", object=1, texture=texture(), source_sha256="a" * 64
                ),
                event(2, 0, "creation", object=2, texture=texture(0x2000)),
                event(3, 10, "draw", bindings=bindings),
            ],
            receipt(),
        )
        self.assertEqual(
            [r["uses"][0]["producer_class"] for r in graph["textures"]],
            ["CPU upload", "creation-only/import", "unknown", "CPU upload"],
        )
        self.assertEqual(graph["textures"][0]["aliases"], [3])
        self.assertEqual(graph["producers"][0]["source_sha256"], "a" * 64)

    def test_release_creation_and_mip_gap(self):
        graph = ANALYSIS.build_graph(
            [
                event(1, 0, "resolve", object=1, texture=texture()),
                event(2, 1, "release", object=1),
                event(
                    3,
                    10,
                    "draw",
                    bindings=[texture(mip_base=0x5000, mip_bytes=4096, max_mip=4)],
                ),
            ],
            receipt(),
        )
        use = graph["textures"][0]["uses"][0]
        self.assertEqual(use["producer_class"], "unknown")
        self.assertEqual(use["range_producers"][1]["producer_sequence"], None)

    def test_partial_overlap_is_explicit(self):
        graph = ANALYSIS.build_graph(
            [
                event(1, 9, "resolve", texture=texture(base_bytes=1024)),
                event(2, 10, "draw", bindings=[texture()]),
            ],
            receipt(),
        )
        self.assertFalse(
            graph["textures"][0]["uses"][0]["range_producers"][0][
                "range_fully_contained"
            ]
        )

    def test_exact_base_outranks_later_overlap(self):
        graph = ANALYSIS.build_graph(
            [
                event(1, 9, "resolve", object=1, texture=texture()),
                event(2, 9, "resolve", object=2, texture=texture(base=0x800)),
                event(3, 10, "draw", bindings=[texture()]),
            ],
            receipt(),
        )
        use = graph["textures"][0]["uses"][0]
        self.assertEqual(use["producer_sequence"], 1)
        self.assertIn(2, use["overlapping_producers"])
        self.assertEqual(use["overwritten_by"], [2])
        self.assertTrue(use["ambiguous"])

    def test_exact_base_upload_overwritten_is_not_proved(self):
        graph = ANALYSIS.build_graph(
            [
                event(1, 9, "upload", object=1, texture=texture()),
                event(2, 9, "resolve", object=2, texture=texture(base=0x800)),
                event(3, 10, "draw", bindings=[texture()]),
            ],
            receipt(),
        )
        use = graph["textures"][0]["uses"][0]
        self.assertEqual(use["producer_sequence"], 1)
        self.assertEqual(use["overwritten_by"], [2])
        self.assertFalse(use["proved_cpu_upload"])

    def test_invalid_capture_refused(self):
        for key, value in (("complete", False), ("overflow", 1), ("end_frame", 13)):
            result = receipt()
            result[key] = value
            with self.assertRaises(ValueError):
                ANALYSIS.build_graph([], result)
        with self.assertRaises(ValueError):
            ANALYSIS.build_graph([event(1, 10, "draw", bindings=[])] * 2, receipt(2))

    def test_output_exclusive(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "existing.md"
            output.write_text("preserve", encoding="ascii")
            with self.assertRaises(SystemExit):
                ANALYSIS.main(["--capture", directory, "--output", str(output)])
            self.assertEqual(output.read_text(encoding="ascii"), "preserve")

    def test_signed_mip_form_preserved(self):
        a = texture(signs=5, max_mip=10, tiled=1, packed_mips=1, swizzle=123)
        self.assertEqual(ANALYSIS.signature(a)["signs"], 5)
        self.assertEqual(ANALYSIS.signature(a)["max_mip"], 10)
        b = copy.deepcopy(a)
        b["signs"] = 0
        self.assertNotEqual(ANALYSIS.signature(a), ANALYSIS.signature(b))

    def test_summary_counts_span_choices_and_ambiguity(self):
        binding = texture(mip_base=0x5000, mip_bytes=4096, max_mip=4)
        graph = ANALYSIS.build_graph(
            [
                event(1, 0, "creation", object=1, texture=binding),
                event(2, 0, "upload", object=1, texture=binding),
                event(3, 0, "upload", object=2, mip=1, texture=binding),
                event(4, 9, "resolve", object=3, texture=texture(0x4800, fmt=18)),
                event(5, 10, "draw", bindings=[binding, {**binding, "slot": 1}]),
                event(6, 10, "draw", bindings=[binding]),
                event(7, 11, "resolve", object=2, mip=1, texture=binding),
                event(8, 11, "draw", bindings=[binding, {**binding, "endian": 2}]),
            ],
            receipt(3),
        )
        row = graph["texture_summary"][0]
        self.assertEqual(row["use_count"], 4)
        self.assertEqual(
            row["producer_classes"], {"CPU upload": 3, "in-window resolve": 1}
        )
        self.assertEqual(
            row["span_producers"],
            {
                "base": [{"sequence": 2, "uses": 4}],
                "mips": [{"sequence": 3, "uses": 3}, {"sequence": 7, "uses": 1}],
            },
        )
        self.assertEqual(row["ambiguous_uses"], 3)
        self.assertEqual(row["overwriting_producers"], [{"sequence": 4, "uses": 3}])
        self.assertEqual(
            row["open_categories"],
            {"ambiguous overwritten producer": 3, "mip-chain producer closure": 1},
        )
        self.assertEqual(row["aliases"], [1])
        for field in (
            "id",
            "base",
            "mip_base",
            "form",
            "ranges",
            "first_use",
            "last_use",
        ):
            self.assertEqual(row[field], graph["textures"][0][field])
        self.assertEqual(json.loads(json.dumps(graph))["texture_summary"][0], row)

    def test_summary_missing_span_and_multiple_overwriters(self):
        binding = texture(mip_base=0x5000, mip_bytes=4096)
        graph = ANALYSIS.build_graph(
            [
                event(1, 0, "upload", texture=texture()),
                event(2, 0, "resolve", texture=texture(0x800, fmt=18)),
                event(3, 0, "upload", texture=texture(0x1800, fmt=18)),
                event(4, 10, "draw", bindings=[binding]),
            ],
            receipt(),
        )
        summary = graph["texture_summary"][0]
        self.assertEqual(summary["ambiguous_uses"], 1)
        self.assertEqual(
            summary["overwriting_producers"],
            [{"sequence": 2, "uses": 1}, {"sequence": 3, "uses": 1}],
        )
        self.assertEqual(
            summary["span_producers"]["mips"], [{"sequence": None, "uses": 1}]
        )

    def test_cached_uses_keep_metadata_and_invalidate_on_epochs(self):
        binding = texture(words=[1])
        events = [
            event(1, 0, "upload", object=1, texture=texture()),
            event(2, 10, "draw", bindings=[binding, {**binding, "slot": 3}]),
            event(3, 10, "draw", entry=2, bindings=[binding]),
            event(4, 10, "draw", bindings=[{**binding, "words": [2]}]),
            event(5, 10, "release", object=9, released_slots=[{"object": 1}]),
            event(6, 10, "draw", bindings=[binding]),
            event(7, 10, "creation", object=1, texture=texture()),
            event(8, 10, "draw", bindings=[binding]),
            event(9, 11, "upload", object=1, raw_target=True, texture=texture()),
            event(10, 11, "draw", bindings=[binding]),
        ]
        with mock.patch.object(
            ANALYSIS, "join_binding", wraps=ANALYSIS.join_binding
        ) as join:
            graph = ANALYSIS.build_graph(events, receipt(6))
        self.assertEqual(join.call_count, 5)
        uses = graph["textures"][0]["uses"]
        self.assertEqual([use["sequence"] for use in uses], [2, 2, 3, 4, 6, 8, 10])
        self.assertEqual(uses[1]["slot"], 3)
        self.assertEqual(uses[2]["entry"], 2)
        self.assertEqual(
            [use["producer_sequence"] for use in uses], [1, 1, 1, 1, None, 7, 9]
        )
        self.assertEqual(uses[-1]["producer_class"], "unproved upload target")
        self.assertEqual(uses[-1]["frame"], 11)
        self.assertEqual(graph["producers"][0]["consumers"], [0])
        self.assertEqual(graph["producers"][2]["consumers"], [0])

    def test_cache_distinguishes_storage_extents(self):
        graph = ANALYSIS.build_graph(
            [
                event(1, 0, "upload", texture=texture(base_bytes=2048)),
                event(2, 10, "draw", bindings=[texture(base_bytes=2048), texture()]),
            ],
            receipt(),
        )
        uses = graph["textures"][0]["uses"]
        self.assertTrue(uses[0]["range_producers"][0]["range_fully_contained"])
        self.assertFalse(uses[1]["range_producers"][0]["range_fully_contained"])

    def test_creation_epoch_replaces_older_exact_write(self):
        graph = ANALYSIS.build_graph(
            [
                event(1, 0, "upload", object=1, texture=texture()),
                event(2, 10, "draw", bindings=[texture()]),
                event(3, 10, "creation", object=1, texture=texture(0x800)),
                event(4, 10, "draw", bindings=[texture()]),
            ],
            receipt(2),
        )
        uses = graph["textures"][0]["uses"]
        self.assertEqual(uses[0]["producer_sequence"], 1)
        self.assertEqual(uses[1]["producer_sequence"], 3)
        self.assertEqual(uses[1]["producer_class"], "creation-only/import")
        self.assertEqual(uses[1]["overlapping_producers"], [1])
        self.assertFalse(uses[1]["range_producers"][0]["range_fully_contained"])

    def test_uninspected_single_producer_keeps_refusal_boundary(self):
        producer = event(2, 11, "upload", texture=texture(base=0x1FFFFFFF))
        graph = ANALYSIS.build_graph(
            [event(1, 10, "draw", bindings=[texture()]), producer], receipt()
        )
        self.assertEqual(graph["producers"][0]["overlaps"], [])
        self.assertEqual(graph["textures"][0]["uses"][0]["producer_class"], "unknown")
        with self.assertRaisesRegex(
            ValueError, "texture range exceeds physical memory"
        ):
            ANALYSIS.build_graph(
                [producer, event(3, 11, "draw", bindings=[texture()])], receipt()
            )

    def test_interval_index_matches_half_open_overlap(self):
        spans = {
            1: [{"begin": 1, "end": 2}],
            2: [{"begin": 2, "end": 8}, {"begin": 20, "end": 22}],
            3: [{"begin": 1, "end": 25}],
            4: [],
            5: [{"begin": 8, "end": 20}],
        }
        index = ANALYSIS.RangeIndex(spans)
        for begin in range(26):
            for length in (1, 3, 30):
                demand = [{"begin": begin, "end": begin + length}]
                expected = {
                    key
                    for key, items in spans.items()
                    if any(ANALYSIS.overlaps(demand[0], span) for span in items)
                }
                self.assertEqual(index.query(demand), expected)
        self.assertEqual(index.query([]), set())
        self.assertEqual(index.query(spans[2]), {2, 3})

    def test_attachments_keep_ordered_consumers(self):
        source = {"object": 7}
        graph = ANALYSIS.build_graph(
            [
                event(1, 10, "target_bind", object=7),
                event(
                    2, 10, "draw", bindings=[], resolve_source_objects=[source, source]
                ),
                event(3, 10, "resolve", texture=texture(), source_attachment=source),
                event(4, 11, "draw", bindings=[], resolve_source_objects=[source]),
                event(5, 11, "resolve", texture=texture(), source_attachment=source),
            ],
            receipt(2),
        )
        self.assertEqual(
            [row["draw_consumers"] for row in graph["attachments"]],
            [[2, 4], [2, 4], [2, 4], [4]],
        )
        self.assertEqual(
            [row["resolve_consumers"] for row in graph["attachments"]],
            [[3, 5], [3, 5], [3, 5], [5]],
        )
        self.assertEqual(graph["producers"][0]["overlaps"], [5])
        self.assertEqual(graph["producers"][1]["overlaps"], [3])
        rendered = ANALYSIS.render(graph)
        self.assertIn("| target_bind | 1 | 0x7 |", rendered)
        self.assertIn("| resolve | 3 | 0x7 |", rendered)
        self.assertIn("2 (2..4)", rendered)

    def test_markdown_summary_and_optional_uses(self):
        graph = ANALYSIS.build_graph(
            [event(1, 10, "draw", bindings=[texture()])], receipt()
        )
        rendered = ANALYSIS.render(graph)
        self.assertIn("| 0 | 0x1000 / 0x0 |", rendered)
        self.assertIn('"unknown": 1', rendered)
        self.assertIn('"sequence": null, "uses": 1', rendered)
        self.assertIn("## Scene open native-producer list", rendered)
        self.assertNotIn("```json", rendered)
        self.assertNotIn("## Per-use producer choices", rendered)
        self.assertIn("## Per-use producer choices", ANALYSIS.render(graph, uses=True))
        graph["textures"][0]["uses"] *= 1000
        graph["texture_summary"] = ANALYSIS.summarize_textures(graph["textures"])
        self.assertLess(len(ANALYSIS.render(graph)), len(rendered) + 100)

    def test_cli_json_retains_graph_and_markdown_uses_flag(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = pathlib.Path(directory)
            (capture / "events.toml").write_text(
                "schema = 1\n[[events]]\nsequence = 1\nframe = 10\n"
                'kind = "draw"\nentry = 1\n[[events.bindings]]\n'
                "sampled = true\ntype = 2\nslot = 0\nbase = 4096\n"
                "mip_base = 0\nbase_bytes = 4096\nformat = 6\n",
                encoding="ascii",
            )
            (capture / "result.toml").write_text(
                "complete = true\noverflow = 0\nbegin_frame = 10\nend_frame = 12\n"
                "requested_frames = 2\ndraws = 1\n",
                encoding="ascii",
            )
            output = capture / "summary.json"
            self.assertEqual(
                ANALYSIS.main(["--capture", str(capture), "--output", str(output)]), 0
            )
            graph = json.loads(output.read_text(encoding="ascii"))
            self.assertEqual(graph["texture_summary"][0]["use_count"], 1)
            self.assertEqual(graph["textures"][0]["uses"][0]["sequence"], 1)
            output = capture / "summary.md"
            self.assertEqual(
                ANALYSIS.main(
                    ["--capture", str(capture), "--output", str(output), "--uses"]
                ),
                0,
            )
            self.assertIn(
                "## Per-use producer choices", output.read_text(encoding="ascii")
            )

    def test_sparse_join_scaling(self):
        producer_count, use_count = 20_000, 10_000
        events = [
            event(i + 1, 0, "upload", object=i + 1, texture=texture((i + 1) * 4096))
            for i in range(producer_count)
        ]
        events.extend(
            event(
                producer_count + i + 1,
                10,
                "draw",
                bindings=[texture((i + 1) * 4096)],
            )
            for i in range(use_count)
        )
        overlap = ANALYSIS.overlaps
        checks = 0

        def bounded_overlap(a, b):
            nonlocal checks
            checks += 1
            if checks > 100_000:
                self.fail("sparse join exceeded 100,000 exact overlap checks")
            return overlap(a, b)

        # CPU time excludes scheduling delays; the operation cap fails scans early.
        started = time.process_time()
        with mock.patch.object(ANALYSIS, "overlaps", bounded_overlap):
            graph = ANALYSIS.build_graph(events, receipt(use_count))
        elapsed = time.process_time() - started
        self.assertLess(elapsed, 5.0, f"sparse join used {elapsed:.2f} CPU seconds")
        self.assertEqual(len(graph["producers"]), producer_count)
        self.assertEqual(len(graph["textures"]), use_count)
        self.assertEqual(
            sum(row["use_count"] for row in graph["texture_summary"]), use_count
        )
        self.assertTrue(all(not row["overlaps"] for row in graph["producers"]))
        self.assertTrue(all(not row["aliases"] for row in graph["textures"]))
        self.assertEqual(
            graph["textures"][-1]["uses"][0]["producer_sequence"], use_count
        )


if __name__ == "__main__":
    unittest.main()
