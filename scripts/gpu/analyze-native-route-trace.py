#!/usr/bin/env python3
"""Check the guest route table or join route snapshots to a supplied census."""

from __future__ import annotations

import argparse
import json
import re
import sys
import tomllib
from collections import Counter, defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TABLE = REPO / "src/gpu/guest_d3d/route_entries.inc"
CALLS = REPO / "src/gpu/diagnostics/route_trace_calls.inc"
COUNT_ONLY = REPO / "src/gpu/diagnostics/route_trace_count_only.inc"
EXISTING = {0x826A3568, 0x826A3000, 0x826A39F8, 0x826AD150, 0x826A7040, 0x826A7138}
ADDRESS = re.compile(r"0x[0-9A-Fa-f]{8}\b")


def addresses(text):
    return [int(value, 16) for value in ADDRESS.findall(text)]


def census_tables(text):
    """Select entry/owner columns, never instruction or displacement columns."""
    entries = defaultdict(set)
    calls = set()
    section = ""
    resource_groups = {
        "GFx texture object/backend creation": "resource_create",
        "GFx decoded texture CPU upload, base and mips": "resource_update",
        "Upload region/layout helper chain": "resource_update",
        "Resolve/present destination texture-like resource": "resource_create",
        "Render/depth target attachments": "clear_target",
        "Bound vertex/index geometry": "resource_update",
        "UP transient geometry update": "resource_update",
        "Resource memory access/lock candidate": "resource_lock",
        "Explicit retained-buffer release": "resource_release",
        "Lower resource destruction candidate": "resource_release",
        "Movie plane resources": "fixed_state_producer",
    }
    proposal_groups = [
        "draw_entry",
        "draw_entry",
        "draw_entry",
        "draw_entry",
        "template",
        "resolve_present",
        "resolve_present",
        "clear_target",
        "device_ring",
        "wait_reader",
        "event",
        "resource_create",
        "resource_update",
        "resource_lock",
        "resource_release",
        "fixed_state_producer",
        "shader_patch",
        "state_replay",
        "gamma",
    ]
    proposal_row = 0
    for line in text.splitlines():
        if line.startswith("## "):
            section = line[3:]
        if not line.startswith("| ") or line.startswith("|---"):
            continue
        cells = [cell.strip() for cell in line.split("|")[1:-1]]
        first = addresses(cells[0])
        if section.startswith("1.") and first:
            if len(cells) == 6:
                entries[first[0]].add("draw_entry")
                calls.add(
                    (first[0], addresses(cells[1])[0] + 4, addresses(cells[2])[0])
                )
            else:
                entries[first[0]].add(
                    "template" if first[0] == 0x8268CC88 else "internal_draw_sink"
                )
        elif section.startswith(("2.", "Appendix A.")) and first:
            for address in first:
                entries[address].add("fixed_state_producer")
        elif section.startswith("3.") and cells[0] in resource_groups:
            column = cells[1].split("callsites")[0].split(" at ")[0]
            for address in addresses(column):
                if cells[0] == "Bound vertex/index geometry" and address == 0x826A39F8:
                    continue
                if cells[0] == "UP transient geometry update" and address in (
                    0x826A3568,
                    0x826A3000,
                ):
                    continue
                entries[address].add(resource_groups[cells[0]])
        elif section.startswith("4.") and first:
            for address in first:
                entries[address].add(
                    "event"
                    if address in (0x826ABEA8, 0x826AC020, 0x826AC578)
                    else "wait_reader"
                )
                if address in (
                    0x8269B1F0,
                    0x8269B2A0,
                    0x8269B618,
                    0x8269B900,
                    0x826AC3F8,
                ):
                    entries[address].add("device_ring")
        elif section.startswith("Appendix B.") and first:
            entries[first[0]].add("result_reader")
        elif section.startswith("5.") and cells[0] != "Entry or entry group":
            if proposal_row >= len(proposal_groups):
                raise ValueError("unexpected census proposal row")
            for address in first:
                if (
                    address != 0x826A4884
                ):  # VdSwap instruction inside the present owner.
                    entries[address].add(proposal_groups[proposal_row])
            proposal_row += 1
    # Section 2 describes these bulk transfers in prose, rather than entry rows.
    section2 = text.split("## 2.", 1)[1].split("## 3.", 1)[0]
    for address in (0x826A4CD0, 0x826A50A0, 0x8281ABB8):
        if address not in addresses(section2):
            raise ValueError(f"missing bulk transfer entry 0x{address:08X}")
        entries[address].add("state_replay")
    section4 = text.split("## 4.", 1)[1].split("## 5.", 1)[0] if "## 4." in text else ""
    if 0x8268CAD8 in addresses(section4):
        entries[0x8268CAD8].add("wait_reader")
    if (
        len(calls) != 17
        or sum("result_reader" in groups for groups in entries.values()) != 33
    ):
        raise ValueError("census draw/reader table changed; review the parser")
    return dict(sorted(entries.items())), sorted(calls)


def render_tables(entries, calls):
    table = "// Census entry/owner columns. Check with analyze-native-route-trace.py --check.\n"
    for address, groups in entries.items():
        mode = "Existing" if address in EXISTING else "New"
        table += f'ROUTE_ENTRY({address:08X}, "{"|".join(sorted(groups))}", {mode})\n'
    callers = "// Exact guest LR values: direct call instruction + 4.\n"
    for entry, caller, owner in calls:
        callers += f"ROUTE_CALL(0x{entry:08X}, 0x{caller:08X}, 0x{owner:08X})\n"
    return table, callers


def check_tables(entries, calls, table=TABLE, callers=CALLS):
    expected = render_tables(entries, calls)
    for path, content in zip((table, callers), expected, strict=True):
        if path.read_text(encoding="ascii") != content:
            raise ValueError(
                f"census table mismatch: {path}; review then use --regenerate"
            )


def scan_tail_entries(directory, entries):
    """Scan generated tail calls without an LR write in the branch instruction."""
    paths = sorted(directory.glob("*.cpp"))
    if not paths:
        raise ValueError(f"no generated C++ files found: {directory}")
    tails = Counter()
    indirect = 0
    for path in paths:
        text = path.read_text(encoding="utf-8")
        for match in re.finditer(r"sub_([0-9A-Fa-f]{8})\(ctx, base\);\s*return;", text):
            instruction = text[text.rfind("//", 0, match.start()) : match.start()]
            if re.match(
                r"//\s*(?:b\s+0x[0-9a-fA-F]+|bctr)\s", instruction
            ) and not re.search(r"ctx\.lr\s*=", instruction):
                target = int(match[1], 16)
                if target in entries:
                    tails[target] += 1
        indirect += len(
            re.findall(
                r"//\s*bctr\s+REX_CALL_INDIRECT_FUNC\(ctx\.ctr\.u32\);\s*return;",
                text,
            )
        )
    return dict(sorted(tails.items())), indirect


def count_only_functions(path=COUNT_ONLY):
    return {
        int(value, 16)
        for value in re.findall(
            r"ROUTE_COUNT_ONLY\(([0-9A-F]{8})\)", path.read_text(encoding="ascii")
        )
    }


def write_report(path, result):
    with path.open("x", encoding="ascii", newline="\n") as output:
        output.write(result)


def analyze(snapshots, entries, calls, tail_entries=None, indirect_tails=0):
    tail_entries = tail_entries or {}
    count_only = count_only_functions()
    known = defaultdict(set)
    known_returns = {lr for _, lr, _ in calls}
    for entry, lr, _ in calls:
        known[entry].add(lr)
    lines = []
    open_items = set()
    overflow_totals = {}
    count_only_totals = {}
    tail_draws = sorted(
        entry for entry in tail_entries if "draw_entry" in entries[entry]
    )
    lines.append(
        "Generated direct tail-reachable traced entries: "
        + ", ".join(
            f"0x{entry:08X} ({count} sites)" for entry, count in tail_entries.items()
        )
    )
    lines.append(
        "Draw entries directly tail-reachable: "
        + (", ".join(f"0x{entry:08X}" for entry in tail_draws) or "none")
        + "."
    )
    for entry in tail_entries:
        open_items.add(
            f"Tail-reachable entry 0x{entry:08X}: LR is the nearest bl return, not an exact immediate caller; tail-chain attribution remains open."
        )
    if indirect_tails:
        open_items.add(
            f"Generated code has {indirect_tails} unresolved indirect bctr tail sites; their runtime targets and immediate callers remain open, including possible draw targets."
        )
    by_segment = {snapshot["segment"]: snapshot for snapshot in snapshots}
    if set(by_segment) != {"menu", "gameplay", "shutdown"} or len(snapshots) != 3:
        open_items.add(
            "Incomplete marker/shutdown coverage; require menu, gameplay and normal shutdown snapshots."
        )
    for snapshot in snapshots:
        segment = snapshot["segment"]
        if snapshot.get("schema") != 1:
            raise ValueError("unsupported route snapshot schema")
        if snapshot.get("entry_count") != len(entries):
            raise ValueError("snapshot traced set differs from census")
        if not snapshot.get("title_revision") or not snapshot.get("sdk_revision"):
            raise ValueError("snapshot revisions are missing")
        rows = snapshot.get("callers", [])
        overflow = snapshot.get("overflow", [])
        counts = snapshot.get("count_only", [])
        for row in overflow + counts:
            if row["function"] not in entries:
                raise ValueError("snapshot function differs from census")
            if row["segment_count"] < 0 or row["segment_count"] > row["count"]:
                raise ValueError("invalid segment count")
        for row in counts:
            if row["function"] not in count_only:
                raise ValueError("snapshot count-only function differs from policy")
        executed = {row["function"] for row in rows if row["segment_count"]}
        executed.update(
            row["function"] for row in overflow + counts if row["segment_count"]
        )
        lines.append(
            f"## {segment}: frames {snapshot['begin_frame']}..{snapshot['frame_count']}"
        )
        lines.append(
            f"Title {snapshot['title_revision']}; SDK {snapshot['sdk_revision']}."
        )
        lines.append("Executed entries and recorded caller LR:")
        for row in rows:
            entry = row["function"]
            caller = row["caller_lr"]
            lr = int(caller, 0) if isinstance(caller, str) else caller
            if entry not in entries or row["group"] != "|".join(sorted(entries[entry])):
                raise ValueError("snapshot function/group differs from census")
            if entry in count_only:
                raise ValueError("count-only function has caller rows")
            if row["segment_count"] < 0 or row["segment_count"] > row["count"]:
                raise ValueError("invalid segment count")
            if not row["segment_count"]:
                continue
            label = (
                "nearest bl return (tail-reachable)"
                if entry in tail_entries
                else "caller LR"
            )
            lines.append(
                f"- 0x{entry:08X} [{row['group']}] <- 0x{lr:08X} ({label}): segment={row['segment_count']}, cumulative={row['count']}, first_frame={row['first_frame']}, segment_first_frame={row['segment_first_frame']}"
            )
            if entry in tail_entries:
                continue
            if "draw_entry" in entries[entry] and lr not in known[entry]:
                lines.append(
                    f"  DRAW FRONTIER: 0x{lr:08X} is outside the 17 known draw return sites."
                )
                open_items.add(
                    f"Draw caller frontier 0x{entry:08X} <- 0x{lr:08X} needs owner/disassembly attribution."
                )
            elif lr not in known_returns:
                # Entry addresses alone do not establish an owning function's extent.
                lines.append(
                    f"  Caller 0x{lr:08X} is not an exact census return site; owning function is unclassified."
                )
                open_items.add(
                    "Non-draw caller LR ownership requires generated-function extent/disassembly attribution; census owner addresses alone do not prove membership."
                )
        missing = sorted(set(entries) - executed)
        lines.append(
            "Census candidates never executed in this segment: "
            + ", ".join(f"0x{x:08X}" for x in missing)
        )
        for address, label in (
            (0x826A3608, "automatic-index"),
            (0x8268CC88, "draw templates"),
        ):
            lines.append(
                f"{label} 0x{address:08X}: {'ran' if address in executed else 'not observed'}."
            )
        for label, groups in (
            ("Result-reader owners", {"result_reader"}),
            ("Event/wait owners", {"event", "wait_reader"}),
        ):
            selected = sorted(entry for entry in executed if entries[entry] & groups)
            lines.append(label + " ran: " + ", ".join(f"0x{x:08X}" for x in selected))
        for row in counts:
            function = row["function"]
            lines.append(
                f"Count-only 0x{function:08X}: segment={row['segment_count']}, cumulative={row['count']}; sampled relaxed counter, no caller attribution."
            )
            count_only_totals[function] = max(
                count_only_totals.get(function, 0), row["count"]
            )
        for row in overflow:
            if row["count"]:
                function = row["function"]
                lines.append(
                    f"Overflow 0x{function:08X}: segment={row['segment_count']}, cumulative={row['count']}."
                )
                overflow_totals[function] = max(
                    overflow_totals.get(function, 0), row["count"]
                )
    if "shutdown" in by_segment:
        final = by_segment["shutdown"]
        seen = {
            row["function"]
            for table in ("callers", "overflow", "count_only")
            for row in final.get(table, [])
            if row["count"]
        }
        lines.append(
            "Entries executed in the whole session: "
            + ", ".join(f"0x{x:08X}" for x in sorted(seen))
        )
        never = sorted(set(entries) - seen)
        lines.append(
            "Census candidates never executed in the whole session: "
            + ", ".join(f"0x{x:08X}" for x in never)
        )
    open_items.update(
        {
            "Candidate-entry tracing cannot exclude sinks, computed/vector/bulk state writes or indirect targets outside the census.",
            "Template creation/replay entry execution does not prove which stored draw words were consumed.",
            "Resource kinds, versions, typed lock/unlock pairing, readback and destruction/reuse need semantic evidence.",
            "Reader/event/wait entry execution does not prove field-load branch execution, returned values, signal pairing or writer-to-reader progress.",
            "Shader variants, literals, derived state and packed constant-bank interpretation need closure.",
            "Owner confirmation is required for cold boot, closed menu overlays, save 1 and the stable first gameplay marker.",
        }
    )
    for function, count in overflow_totals.items():
        open_items.add(
            f"Caller capacity overflow at 0x{function:08X}: {count} cumulative calls lack exact attribution."
        )
    for function, count in count_only_totals.items():
        if count:
            open_items.add(
                f"Count-only entry 0x{function:08X}: {count} cumulative calls have no caller attribution; per-segment counts use sampled boundaries, not the caller-bank cut."
            )
    lines += [
        "## Boundedness verdict",
        "Observed candidate route only; replacement surface remains OPEN.",
    ]
    lines += [f"- {item}" for item in sorted(open_items)]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--census", required=True, type=Path)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--check", action="store_true")
    action.add_argument("--regenerate", action="store_true")
    action.add_argument("--capture", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--generated",
        type=Path,
        default=REPO / "generated/default",
        help="generated-code directory scanned for tail branches (default: generated/default)",
    )
    args = parser.parse_args()
    if args.output and args.output.exists():
        raise FileExistsError(f"refusing to overwrite existing output: {args.output}")
    entries, calls = census_tables(args.census.read_text(encoding="utf-8"))
    if args.regenerate:
        for path, content in zip(
            (TABLE, CALLS), render_tables(entries, calls), strict=True
        ):
            path.write_text(content, encoding="ascii", newline="\n")
    else:
        check_tables(entries, calls)
    tails, indirect = scan_tail_entries(args.generated, entries)
    if args.capture:
        snapshots = []
        for segment in ("menu", "gameplay", "shutdown"):
            path = args.capture / f"route-{segment}.toml"
            if path.is_file():
                snapshots.append(tomllib.loads(path.read_text(encoding="utf-8")))
        if not snapshots:
            raise ValueError("no route snapshots found")
        result = analyze(snapshots, entries, calls, tails, indirect)
        if args.output:
            write_report(args.output, result)
        else:
            print(result, end="")
    else:
        counts = Counter(group for groups in entries.values() for group in groups)
        print(
            json.dumps(
                {
                    "entries": len(entries),
                    "draw_sites": len(calls),
                    "groups": counts,
                    "tail_entries": {f"0x{x:08X}": count for x, count in tails.items()},
                    "tail_draw_entries": [
                        f"0x{x:08X}" for x in tails if "draw_entry" in entries[x]
                    ],
                    "unresolved_indirect_tail_sites": indirect,
                },
                sort_keys=True,
            )
        )


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, tomllib.TOMLDecodeError) as error:
        print(f"route trace: {error}", file=sys.stderr)
        sys.exit(1)
