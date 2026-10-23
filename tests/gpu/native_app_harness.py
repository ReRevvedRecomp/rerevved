"""Run the bounded native lifecycle checks on a hidden window.

smoke <rerevved_native_app_smoke> <dir>: the test executable drives the real
presentation session and prints one PASS, FAIL or SKIP verdict.
app <rerevved> <dir>: a developer build runs the detached app with the injected
lifecycle script and its presentation event log.
"""

import os
import subprocess
import sys
import time
import tomllib
from pathlib import Path


def check(condition, message):
    if not condition:
        print(f"FAIL: {message}")
    return condition


def run_smoke(executable, base, catalog):
    root = Path(base) / str(time.time_ns())
    root.mkdir(parents=True)
    log = root / "smoke.log"
    with log.open("w") as handle:
        process = subprocess.Popen(
            [executable, catalog], stdout=handle, stderr=subprocess.STDOUT
        )
        try:
            code = process.wait(timeout=45)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            print(
                f"FAIL: smoke did not finish within 45 seconds; PID {process.pid}; {root}"
            )
            return 1
    verdicts = [
        line
        for line in log.read_text(errors="replace").splitlines()
        if line.startswith(("PASS:", "FAIL:", "SKIP:"))
    ]
    verdict = verdicts[-1] if verdicts else "no verdict"
    if code == 77:
        print(verdict if verdict.startswith("SKIP:") else "SKIP: smoke skipped")
        return 77
    if code == 0 and verdict.startswith("PASS:"):
        print(f"{verdict}; {log}")
        return 0
    print(f"FAIL: exit {code}; {verdict}; {log}")
    return 1


def run_app(executable, base, catalog):
    root = Path(base) / str(time.time_ns())
    root.mkdir(parents=True)
    trace = root / "events.toml"
    command = [
        executable,
        "--renderer=native",
        f"--native_present_fixture={catalog}",
        f"--native_present_trace={trace}",
        "--native_present_script=lifecycle",
        "--window_hidden=true",
        "--fullscreen=false",
        f"--user_data_root={root / 'user'}",
    ]
    with (root / "app.log").open("w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            code = process.wait(timeout=45)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            print(
                f"FAIL: app did not close within 45 seconds; PID {process.pid}; {root}"
            )
            return 1
    if not trace.exists():
        print(f"FAIL: missing trace, exit {code}; {root}")
        return 1
    try:
        events = tomllib.loads(trace.read_text())["event"]
    except (ValueError, KeyError) as error:
        print(f"FAIL: invalid trace: {error}; {root}")
        return 1
    names = [event["name"] for event in events]
    if names == ["unavailable"]:
        print("SKIP: GPU/window unavailable")
        return 77
    if not check(code == 0, f"exit {code}; {root}"):
        return 1
    if not check(
        not {"startup_failed", "producer_failed"}.intersection(names),
        f"startup or producer failed; {root}",
    ):
        return 1
    expected = [
        "startup",
        "detached",
        "published",
        "present",
        "script_resize",
        "script_minimize",
        "suspend",
        "script_restore",
        "restore",
        "script_overlay_open",
        "overlay_composed",
        "script_overlay_input",
        "overlay_input_capture",
        "script_overlay_close",
        "overlay_cleared",
        "script_close",
        "close_veto",
        "close_admission",
        "producer_stopped",
        "shutdown",
        "overlays_shutdown",
        "close_accept",
    ]
    cursor = 0
    for name in expected:
        if not check(name in names[cursor:], f"missing ordered event {name}; {root}"):
            return 1
        cursor = names.index(name, cursor) + 1
    published = [event["frame"] for event in events if event["name"] == "published"]
    if not check(
        [event["value"] for event in events if event["name"] == "script_overlay_input"]
        == [3],
        "SDK keyboard and mouse listeners did not receive detached input",
    ):
        return 1
    if not check(
        [event["value"] for event in events if event["name"] == "overlay_input_capture"]
        == [1],
        "SDK detached overlay capture predicate was not active",
    ):
        return 1
    opened = names.index("script_overlay_open")
    closed = names.index("script_overlay_close")
    overlay_frames = [
        event["frame"]
        for event in events[opened:closed]
        if event["name"] == "overlay_composed"
    ]
    overlay_publications = [
        event["frame"]
        for event in events[opened:closed]
        if event["name"] == "published"
    ]
    if not check(
        len(overlay_frames) >= 2
        and overlay_frames == sorted(set(overlay_frames))
        and set(overlay_frames).issubset(overlay_publications)
        and overlay_publications == sorted(set(overlay_publications)),
        "overlays must compose on at least two advancing published frames",
    ):
        return 1
    if not check(
        len(published) >= 9 and len(set(published)) == len(published), published
    ):
        return 1
    if not check(
        names[opened:].count("overlay_cleared") == 1,
        "overlay clear was not deduplicated",
    ):
        return 1
    if not check(
        names[names.index("overlay_cleared", closed) :].count("present") >= 2,
        "missing continued native presentation after overlay clear",
    ):
        return 1
    presents = [event for event in events if event["name"] == "present"]
    # DXGI occlusion is allowed for a hidden window, but is never counted as a visible present.
    if not check(all(event["value"] in (0, 1, 2, 3) for event in presents), presents):
        return 1
    if not check(
        any(event["value"] == 3 for event in presents), "missing suspended pump"
    ):
        return 1
    extents = {
        (event["frame"], event["value"])
        for event in events
        if event["name"] == "surface"
    }
    if not check(len(extents) >= 2, f"swapchain did not resize: {extents}"):
        return 1
    shown = {event["frame"] for event in presents if event["value"] in (0, 1)}
    if not check(shown == set(published), (shown, published)):
        return 1
    gamma = [
        event["value"]
        for event in events
        if event["name"] == "gamma" and event["frame"]
    ]
    if not check(
        gamma and all(value == 1 for value in gamma),
        f"incorrect gamma generation: {gamma}",
    ):
        return 1
    if not check(
        [event["value"] for event in events if event["name"] == "shutdown"] == [1],
        "shutdown did not drain successfully",
    ):
        return 1
    if not check(
        names.count("close_veto") >= 2 and names.count("close_admission") == 1,
        "repeated close did not share one drain",
    ):
        return 1
    print(
        f"PASS: {len(published)} native fixture frames occluded or presented, resize, "
        f"suspend/restore, SDK settings overlay/input and bounded close; {trace}"
    )
    return 0


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("smoke", "app"):
        print("usage: native_app_harness.py smoke|app <executable> <directory>")
        return 2
    catalog = os.environ.get("REREVVED_NATIVE_PRESENT_CATALOG")
    if not catalog:
        print("SKIP: REREVVED_NATIVE_PRESENT_CATALOG is not set")
        return 77
    run = run_smoke if sys.argv[1] == "smoke" else run_app
    return run(sys.argv[2], sys.argv[3], catalog)


if __name__ == "__main__":
    sys.exit(main())
