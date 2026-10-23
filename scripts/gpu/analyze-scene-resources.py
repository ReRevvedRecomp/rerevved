"""Join guest D3D resource provenance with a swap-delimited scene window.

Fetch fields: SDK include/rex/graphics/xenos.h, xe_gpu_texture_fetch_t.
Ranges: SDK texture_util::GetGuestTextureLayout, captured storage extents.
GPU-produced content is never inferred from a guest RAM hash.
"""

import argparse
import hashlib
import json
import pathlib
import struct
import tomllib
from bisect import bisect_left, bisect_right
from collections import Counter

FORM_FIELDS = (
    "format",
    "width",
    "height",
    "depth",
    "dimension",
    "min_mip",
    "max_mip",
    "packed_mips",
    "pitch",
    "tiled",
    "swizzle",
    "endian",
    "signs",
    "num_format",
    "exp_adjust",
    "stacked",
)
# SDK graphics/pipeline/texture/info.h GetBaseFormat; byte-compatible views.
BASE_FORMAT = {
    27: 30,
    28: 31,
    29: 32,
    50: 6,
    51: 18,
    52: 19,
    53: 20,
    54: 7,
    55: 16,
    56: 17,
    62: 6,
}


def compatible(a, b):
    return BASE_FORMAT.get(a, a) == BASE_FORMAT.get(b, b)


def signature(texture):
    """Keep sampler views distinct, including pre-swizzle component signs."""
    return {field: texture.get(field, 0) for field in FORM_FIELDS}


def ranges(texture):
    result = []
    for role, address, size in (
        ("base", "base", "base_bytes"),
        ("mips", "mip_base", "mip_bytes"),
    ):
        start, length = texture.get(address, 0), texture.get(size, 0)
        if start and length:
            if start < 0 or length < 0 or start + length > 0x20000000:
                raise ValueError("texture range exceeds physical memory")
            result.append({"role": role, "begin": start, "end": start + length})
    return result


def overlaps(a, b):
    return a["begin"] < b["end"] and b["begin"] < a["end"]


def admitted_writer(event):
    """Writer whose destination hashes completed despite an earlier read error."""
    spans = event.get("destination_ranges", [])
    return (
        event.get("kind") == "cpu_writer"
        and bool(spans)
        and all(span.get("destination_sha256") for span in spans)
    )


def producer_ranges(event):
    if event["kind"] == "cpu_writer":
        result = []
        for span in event.get("destination_ranges", []):
            # Recorder normalizes only the SDK's physical virtual aliases.
            # Ordinary CPU virtual storage has no physical begin/end join.
            if "begin" not in span or "end" not in span:
                continue
            begin, end = span["begin"], span["end"]
            if not 0 <= begin < end <= 0x20000000:
                raise ValueError("CPU writer range exceeds physical memory")
            if end - begin != span["bytes"]:
                raise ValueError("CPU writer range differs from hashed extent")
            result.append({"role": "writer", "begin": begin, "end": end})
        return result
    texture = event.get("texture", {})
    result = ranges(texture)
    if event["kind"] in ("resolve", "upload"):
        role = "mips" if event.get("mip", 0) else "base"
        result = [item for item in result if item["role"] == role]
    return result


def category(texture, producer):
    fmt = BASE_FORMAT.get(texture.get("format"), texture.get("format"))
    if producer == "unknown":
        return "unknown producer"
    if producer == "creation-only/import":
        return "import or creation-only"
    if producer == "unproved upload target":
        return "unproved CPU upload target kind"
    if fmt == 20:
        return "BC3 exact-form proof"
    if texture.get("max_mip", 0):
        return "mip-chain producer closure"
    if texture.get("signs", 0):
        return "signed texture producer"
    if fmt in (22, 23):
        return "depth producer"
    if producer in ("in-window resolve", "earlier resolve"):
        return "resolve attachment/offset/swizzle/endian/clear closure"
    if producer == "CPU writer":
        return "CPU writer exact-form/region admission"
    return "CPU upload exact-form admission"


class RangeIndex:
    """Centered interval tree; queries return only half-open storage overlaps."""

    def __init__(self, spans):
        self.spans = spans
        self.root = self._build(
            sorted(
                [
                    (span["begin"], span["end"], key)
                    for key in spans
                    for span in spans[key]
                ],
                key=lambda entry: (entry[0] + entry[1]) // 2,
            )
        )

    @classmethod
    def _build(cls, entries):
        if not entries:
            return None
        begin, end, _ = entries[len(entries) // 2]
        center = (begin + end) // 2
        left, middle, right = [], [], []
        for entry in entries:
            begin, end, _ = entry
            if end <= center:
                left.append(entry)
            elif begin > center:
                right.append(entry)
            else:
                middle.append(entry)
        return (
            center,
            sorted(middle),
            sorted(middle, key=lambda entry: entry[1], reverse=True),
            cls._build(left),
            cls._build(right),
        )

    def query(self, spans):
        found = set()
        for span in spans:
            self._query(self.root, span["begin"], span["end"], found)
        return found

    @classmethod
    def _query(cls, node, begin, end, found):
        if node is None:
            return
        center, by_begin, by_end, left, right = node
        if end <= center:
            for start, _, key in by_begin:
                if start >= end:
                    break
                found.add(key)
            cls._query(left, begin, end, found)
        elif begin > center:
            for _, stop, key in by_end:
                if stop <= begin:
                    break
                found.add(key)
            cls._query(right, begin, end, found)
        else:
            found.update(key for _, _, key in by_begin)
            cls._query(left, begin, end, found)
            cls._query(right, begin, end, found)


def join_binding(
    binding, demand, observed_overlaps, releases, spans, begin, end, latest=None
):
    """Retain sequence ordering, exact-base preference and object epoch rules."""

    def writer_targets(producer):
        return [
            target
            for target in producer.get("writer_targets", [])
            if (latest or {}).get(target["object"]) == target["creation_sequence"]
            and releases.get(target["object"], 0) < target["creation_sequence"]
            and compatible(target["texture"]["format"], binding["format"])
            and any(overlaps(a, b) for a in demand for b in ranges(target["texture"]))
        ]

    candidates = [
        p
        for p in observed_overlaps
        if (
            bool(writer_targets(p))
            if p["kind"] == "cpu_writer"
            else releases.get(p.get("object", 0), 0) <= p["sequence"]
            and compatible(p["texture"]["format"], binding["format"])
        )
    ]
    # Creation begins a new observed object epoch; older writes are not reused.
    creation = next((p for p in candidates if p["kind"] == "creation"), None)
    if creation:
        candidates = [
            p
            for p in candidates
            if (p["kind"] != "cpu_writer" and p.get("object") != creation.get("object"))
            or p["sequence"] >= creation["sequence"]
        ]
    # Old captures may contain child copy events from sub_82578E00. They are
    # implementation steps of the upload, not later producer overwrites.
    upload_children = {
        p["sequence"]
        for p in observed_overlaps
        if p["kind"] == "cpu_writer"
        and 0x82578E00 <= int(p.get("caller_lr", 0)) < 0x825794A8
        and any(u["kind"] == "upload" for u in candidates)
    }
    candidates = [p for p in candidates if p["sequence"] not in upload_children]
    writer_sequences = {p["sequence"] for p in candidates if p["kind"] == "cpu_writer"}
    # Keep existing upload/resolve/creation overwrite semantics; exclude only
    # writers rejected by the creation epoch or upload-child guard.
    observed_overlaps = [
        p
        for p in observed_overlaps
        if p["kind"] != "cpu_writer" or p["sequence"] in writer_sequences
    ]
    # Exact bases outrank later partial overlaps, which remain overwrites.
    exact = [
        p
        for p in candidates
        if (
            any(t["texture"].get("base") == binding["base"] for t in writer_targets(p))
            if p["kind"] == "cpu_writer"
            else p["texture"].get("base") == binding["base"]
        )
    ]
    chosen = exact[0] if exact else candidates[0] if candidates else None
    producer_class = "unknown"
    if chosen:
        producer_class = {
            "upload": (
                "unproved upload target"
                if chosen.get("raw_target", False)
                or "raw direct target" in chosen.get("target_source", "")
                else "CPU upload"
            ),
            "creation": "creation-only/import",
            "cpu_writer": "CPU writer",
            "resolve": (
                "in-window resolve"
                if begin <= chosen["frame"] < end
                else "earlier resolve"
            ),
        }[chosen["kind"]]
    covered = []
    ordered = ([chosen] if chosen else []) + candidates
    for span in demand:
        match = next(
            (
                p
                for p in ordered
                if any(overlaps(span, q) for q in spans[p["sequence"]])
            ),
            None,
        )
        covered.append(
            {
                "role": span["role"],
                "producer_sequence": match["sequence"] if match else None,
                "range_fully_contained": bool(match)
                and any(
                    q["begin"] <= span["begin"] and q["end"] >= span["end"]
                    for q in spans[match["sequence"]]
                ),
                "overwritten_by": [
                    p["sequence"]
                    for p in observed_overlaps
                    if match
                    and p["sequence"] > match["sequence"]
                    and any(overlaps(span, q) for q in spans[p["sequence"]])
                ],
            }
        )
    overwritten_by = sorted(
        {sequence for item in covered for sequence in item["overwritten_by"]}
    )
    return {
        "producer_class": producer_class,
        "producer_sequence": chosen["sequence"] if chosen else None,
        "range_producers": covered,
        "overlapping_producers": [
            p["sequence"] for p in observed_overlaps if p is not chosen
        ],
        "overwritten_by": overwritten_by,
        "ambiguous": bool(overwritten_by),
        "raw_target": producer_class == "unproved upload target",
        "proved_cpu_upload": producer_class == "CPU upload" and not overwritten_by,
        "cpu_writers": [
            {
                "sequence": p["sequence"],
                "path": p.get("writer_path", "raw"),
                **({"read_error": p["read_error"]} if p.get("read_error") else {}),
            }
            for p in candidates
            if p["kind"] == "cpu_writer"
        ],
        "open_category": (
            "ambiguous overwritten producer"
            if overwritten_by
            else category(binding, producer_class)
        ),
    }


def summarize_textures(rows):
    """Count uses, including missing span producers, without duplicating use rows."""
    summaries = []
    for row in rows:
        uses = row["uses"]
        span_counts = {}
        producer_classes = Counter(use["producer_class"] for use in uses)
        open_categories = Counter(use["open_category"] for use in uses)
        overwriters = Counter(
            sequence for use in uses for sequence in use["overwritten_by"]
        )
        for use in uses:
            for span in use["range_producers"]:
                span_counts.setdefault(span["role"], Counter())[
                    span["producer_sequence"]
                ] += 1
        summaries.append(
            {
                **{
                    field: row[field]
                    for field in (
                        "id",
                        "base",
                        "mip_base",
                        "form",
                        "ranges",
                        "first_use",
                        "last_use",
                        "aliases",
                    )
                },
                "use_count": len(uses),
                "producer_classes": dict(sorted(producer_classes.items())),
                "span_producers": {
                    role: [
                        {"sequence": sequence, "uses": count}
                        for sequence, count in sorted(
                            counts.items(),
                            key=lambda item: (item[0] is not None, item[0]),
                        )
                    ]
                    for role, counts in span_counts.items()
                },
                "ambiguous_uses": sum(use["ambiguous"] for use in uses),
                "overwriting_producers": [
                    {"sequence": sequence, "uses": count}
                    for sequence, count in sorted(overwriters.items())
                ],
                "open_categories": dict(sorted(open_categories.items())),
                "cpu_writers": row["cpu_writers"],
            }
        )
    return summaries


def build_graph(events, result):
    """Latest ordered overlapping producer, retaining partial/alias uncertainty."""
    if not result.get("complete") or result.get("overflow", 0):
        raise ValueError("capture is incomplete or overflowed; graph is not complete")
    begin, end = result["begin_frame"], result["end_frame"]
    if end - begin != result["requested_frames"] or begin >= end:
        raise ValueError("invalid complete-frame window")
    events = sorted(events, key=lambda event: event["sequence"])
    sequences = [event["sequence"] for event in events]
    if len(set(sequences)) != len(sequences):
        raise ValueError("duplicate global sequence")
    if "events" in result and result["events"] != len(events):
        raise ValueError("event count differs from drained log")
    if result.get("relevant_read_failures", 0) or any(
        event.get("read_error") and event.get("failure_relevant", True)
        for event in events
    ):
        raise ValueError("capture contains unreadable input")
    ignored_failures = [event for event in events if event.get("read_error")]
    creations = [
        e
        for e in events
        if e["kind"] == "creation" and e.get("texture") and not e.get("read_error")
    ]
    creation_index = RangeIndex(
        {e["sequence"]: ranges(e["texture"]) for e in creations}
        if any(e["kind"] == "cpu_writer" for e in events)
        else {}
    )
    creation_by_sequence = {e["sequence"]: e for e in creations}
    latest, released, writers = {}, {}, {}
    for e in events:
        if e.get("read_error") and not admitted_writer(e):
            continue
        if e["kind"] == "creation" and e.get("object"):
            latest[e["object"]] = e["sequence"]
        elif e["kind"] == "release":
            for item in [e, *e.get("released_slots", [])]:
                released[item.get("object", 0)] = e["sequence"]
        elif e["kind"] == "cpu_writer":
            # Failed conversion status must not promote the derived row ranges.
            if e.get("return_status", 0) < 0:
                continue
            targets = []
            for seq in sorted(creation_index.query(producer_ranges(e))):
                creation = creation_by_sequence[seq]
                obj = creation.get("object", 0)
                if (
                    latest.get(obj) == seq
                    and released.get(obj, 0) < seq < e["sequence"]
                ):
                    targets.append(
                        {
                            "object": obj,
                            "creation_sequence": seq,
                            "texture": creation["texture"],
                        }
                    )
            writers[e["sequence"]] = {**e, "writer_targets": targets}
    producers = [
        event
        for event in events
        if not event.get("read_error")
        and event["kind"] in ("creation", "upload", "resolve")
        and event.get("texture")
    ] + list(writers.values())
    producer_by_sequence = {p["sequence"]: p for p in producers}
    producer_spans = {}
    for producer in producers:
        # A lone producer without a later demand had no range validation before.
        inspected = len(producers) > 1 or any(
            not e.get("read_error")
            and e.get("kind") == "draw"
            and e["sequence"] > producer["sequence"]
            and begin <= e["frame"] < end
            and any(b.get("sampled") and b.get("type") == 2 for b in e["bindings"])
            for e in events
        )
        producer_spans[producer["sequence"]] = (
            producer_ranges(producer)
            if inspected or producer["kind"] == "cpu_writer"
            else []
        )
    producer_index = RangeIndex(producer_spans)
    releases, textures, consumer_index = {}, {}, {}
    use_cache = {}
    attachment_rows = []
    resolves = []
    draws = 0
    unbound_demands = []
    latest = {}
    for event in events:
        if (event.get("read_error") and not admitted_writer(event)) or event.get(
            "kind"
        ) == "rejected_by_guest":
            continue
        kind, seq = event["kind"], event["sequence"]
        if kind == "creation" and event.get("object"):
            latest[event["object"]] = seq
        if kind == "release":
            use_cache.clear()
            releases[event.get("object", 0)] = seq
            for item in event.get("released_slots", []):
                releases[item.get("object", 0)] = seq
        if kind == "cpu_writer" or (
            kind in ("creation", "upload", "resolve") and event.get("texture")
        ):
            use_cache.clear()
        if begin <= event["frame"] < end:
            if kind in ("target_bind", "depth_bind_candidate"):
                attachment_rows.append(event)
            if kind == "resolve":
                resolves.append(event)
        if kind != "draw" or not begin <= event["frame"] < end:
            continue
        draws += 1
        for attachment in event.get("resolve_source_objects", []):
            if attachment.get("object"):
                attachment_rows.append(
                    {
                        **attachment,
                        "kind": "attachment_snapshot",
                        "sequence": seq,
                        "frame": event["frame"],
                        "source": "draw G+3098..30A8; Resolve source selector roles",
                    }
                )
        for binding in event["bindings"]:
            if not binding.get("sampled"):
                continue
            if binding.get("type") != 2:
                unbound_demands.append(
                    {
                        "sequence": seq,
                        "frame": event["frame"],
                        "entry": event["entry"],
                        "slot": binding["slot"],
                        "type": binding.get("type"),
                        "words": binding.get("words", []),
                        "open_category": "unbound statically demanded texture slot",
                    }
                )
                continue
            form = signature(binding)
            key = (binding["base"], binding["mip_base"], *form.values())
            row = textures.setdefault(
                key,
                {
                    "id": len(textures),
                    "base": binding["base"],
                    "mip_base": binding["mip_base"],
                    "form": form,
                    "ranges": ranges(binding),
                    "first_use": seq,
                    "last_use": seq,
                    "uses": [],
                    "aliases": [],
                    "cpu_writers": [],
                },
            )
            row["last_use"] = seq
            demand = ranges(binding)
            cache_key = (
                key,
                tuple(binding.get("words", [])),
                tuple((span["role"], span["begin"], span["end"]) for span in demand),
            )
            if cache_key not in use_cache:
                observed_overlaps = [
                    producer_by_sequence[sequence]
                    for sequence in sorted(producer_index.query(demand), reverse=True)
                    if sequence < seq
                ]
                use_cache[cache_key] = join_binding(
                    binding,
                    demand,
                    observed_overlaps,
                    releases,
                    producer_index.spans,
                    begin,
                    end,
                    latest,
                )
            decision = use_cache[cache_key]
            for writer in decision["cpu_writers"]:
                if writer not in row["cpu_writers"]:
                    row["cpu_writers"].append(writer)
                consumer_index.setdefault(writer["sequence"], set()).add(row["id"])
            for sequence in (
                decision["producer_sequence"],
                *(span["producer_sequence"] for span in decision["range_producers"]),
            ):
                if sequence is not None:
                    consumer_index.setdefault(sequence, set()).add(row["id"])
            row["uses"].append(
                {
                    "sequence": seq,
                    "frame": event["frame"],
                    "entry": event["entry"],
                    "slot": binding["slot"],
                    **decision,
                }
            )
    if draws != result["draws"]:
        raise ValueError("draw count differs from completed window")
    rows = list(textures.values())
    texture_index = RangeIndex({row["id"]: row["ranges"] for row in rows})
    for row in rows:
        row["aliases"] = sorted(texture_index.query(row["ranges"]) - {row["id"]})
    producer_rows = []
    for producer in producers:
        copy = dict(producer)
        copy["consumers"] = sorted(consumer_index.get(producer["sequence"], []))
        copy["overlaps"] = sorted(
            producer_index.query(producer_index.spans[producer["sequence"]])
            - {producer["sequence"]}
        )
        producer_rows.append(copy)
    resolve_consumers, draw_consumers = {}, {}
    for resolve in resolves:
        obj = resolve.get("source_attachment", {}).get("object")
        resolve_consumers.setdefault(obj, []).append(resolve["sequence"])
    for event in events if attachment_rows else []:
        if event["kind"] == "draw" and begin <= event["frame"] < end:
            objects = {a.get("object") for a in event.get("resolve_source_objects", [])}
            for obj in objects:
                draw_consumers.setdefault(obj, []).append(event["sequence"])
    for attachment in attachment_rows:
        obj = attachment.get("object")
        consumers = resolve_consumers.get(obj, [])
        attachment["resolve_consumers"] = consumers[
            bisect_right(consumers, attachment["sequence"]) :
        ]
        consumers = draw_consumers.get(obj, [])
        attachment["draw_consumers"] = consumers[
            bisect_left(consumers, attachment["sequence"]) :
        ]
    resolve_sequences = {r["sequence"] for r in resolves}
    return {
        "schema": 1,
        "window": result,
        "sampling_evidence": "static VS/PS fetch demand; conditional use is conservative",
        "range_evidence": "padded descriptor storage overlap, not GPU completion",
        "textures": rows,
        "texture_summary": summarize_textures(rows),
        "unbound_demands": unbound_demands,
        "ignored_read_failures": ignored_failures,
        "producers": producer_rows,
        "attachments": attachment_rows,
        "window_resolves": [
            p for p in producer_rows if p["sequence"] in resolve_sequences
        ],
        "open_native_producers": [
            {
                "texture": row["id"],
                "form": row["form"],
                "categories": sorted({u["open_category"] for u in row["uses"]}),
                "cpu_writers": row["cpu_writers"],
                "cpu_writer_status": "observed" if row["cpu_writers"] else "open",
                "obligations": [
                    "Exact selected-form parity and native ownership",
                    "Complete per-mip/region writes; padded overlap is only a candidate",
                    "Allocation generation, release/reuse and GPU completion",
                ],
            }
            for row in rows
        ]
        + [
            {
                "texture": None,
                "sequence": demand["sequence"],
                "slot": demand["slot"],
                "categories": [demand["open_category"]],
                "obligations": [
                    "Determine whether the conditional fetch executes and bind a texture if required"
                ],
            }
            for demand in unbound_demands
        ],
    }


def rejected_stream(result, prefix, reason, **counters):
    """Describe an optional stream that could not be validated, never the metadata."""
    return {
        "accepted": False,
        "rejection_reason": reason,
        "reported_failure": result.get(f"{prefix}_failure_reason", ""),
        **counters,
    }


def payload_summary(capture, events, result):
    """Validate retained bytes separately from metadata and GPU completion.

    An unreadable or unparsable stream, or an invalid one the producer already
    reported as failed, is rejected without aborting the metadata analysis. A
    parsable stream that contradicts the events with no reported failure is
    still a hard error.
    """
    if "payload_manifest" not in result:
        return None
    if result["payload_manifest"] != "payload-manifest.toml":
        raise ValueError("unexpected payload manifest path")
    failures = any(
        result.get(key, 0)
        for key in (
            "payload_overflow",
            "payload_read_failures",
            "payload_failures",
        )
    )
    try:
        summary = validate_payloads(capture, events, result)
        if failures:
            summary.update(
                rejected_stream(result, "payload", "nonzero stream failure counters")
            )
            summary["retention_complete"] = False
        return summary
    except OSError:
        reason = "payload manifest or blob unreadable"
    except tomllib.TOMLDecodeError:
        reason = "payload manifest is truncated or not valid TOML"
    except (ValueError, KeyError, TypeError) as error:
        if not result.get("payload_failure_reason") and not failures:
            raise
        reason = f"payload stream invalid: {error}"
    return rejected_stream(
        result,
        "payload",
        reason,
        capture_id=result.get("capture_id", ""),
        retention_complete=False,
        executable_ready=False,
    )


def validate_payloads(capture, events, result):
    with (capture / result["payload_manifest"]).open("rb") as stream:
        manifest = tomllib.load(stream)
    identity = result.get("capture_id")
    if (
        not identity
        or manifest.get("schema") != 1
        or manifest.get("capture_id") != identity
    ):
        raise ValueError("payload capture identity mismatch")
    call_frames = {e["sequence"]: e["frame"] for e in events}
    submissions = {
        e["submission_id"]
        for e in events
        if e["kind"] == "draw" and e.get("submission_id") == e.get("call_id")
    }
    for e in events:
        if e.get("capture_id", identity) != identity:
            raise ValueError("event capture identity mismatch")
    rows = manifest.get("bindings", [])
    files, failed, unobserved_allocations = {}, 0, 0
    for row in rows:
        if row.get("capture_id") != identity or row.get("event") not in call_frames:
            raise ValueError("payload event identity mismatch")
        if row.get("frame") != call_frames[row["event"]]:
            raise ValueError("payload frame identity mismatch")
        if row.get("submission") and row["submission"] not in submissions:
            raise ValueError("payload submission identity mismatch")
        for field in (
            "frame",
            "event",
            "submission",
            "parent_call",
            "generation",
            "version",
            "address",
            "bytes",
        ):
            if type(row.get(field)) is not int or row[field] < 0:
                raise ValueError(f"invalid payload {field}")
        if not row.get("generation") or not row.get("resource") or not row.get("role"):
            raise ValueError("payload resource identity missing")
        if "allocation_generation" in row:
            generation = row["allocation_generation"]
            if type(generation) is not int or generation < 0:
                raise ValueError("invalid allocation generation")
            bound = (
                row.get("physical")
                and row["role"]
                in (
                    "vertices",
                    "indices",
                    "creation_initial_bytes",
                    "cpu_update",
                    "sampled_base",
                    "sampled_mip_base",
                )
            ) or row["role"].startswith("writer_C_region:")
            unobserved_allocations += bool(bound and not generation)
        if row.get("error"):
            if row.get("sha256") or row.get("file"):
                raise ValueError("failed payload has retained bytes")
            failed += 1
            continue
        digest = row.get("sha256", "")
        if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            raise ValueError("invalid payload SHA-256")
        name = f"payloads/{digest}.bin"
        if row.get("file") != name:
            raise ValueError("payload path differs from digest")
        if name not in files:
            path = capture / name
            if not path.resolve().is_relative_to(capture.resolve()):
                raise ValueError("payload escapes capture directory")
            with path.open("rb") as stream:
                actual = hashlib.file_digest(stream, "sha256").hexdigest()
            if actual != digest:
                raise ValueError("payload digest mismatch")
            files[name] = path.stat().st_size
        if files[name] != row["bytes"]:
            raise ValueError("payload size mismatch")
    if sum(files.values()) > result.get("payload_bytes", 0) or result.get(
        "payload_bytes", 0
    ) > manifest.get("byte_budget", 0):
        raise ValueError("payload budget accounting mismatch")
    complete = bool(result.get("payload_complete"))
    if complete and (
        failed
        or result.get("payload_overflow")
        or result.get("payload_read_failures")
        or result.get("payload_known_gaps")
        or result.get("payload_failures")
        or result.get("payload_failure_reason")
    ):
        raise ValueError("payload completeness contradicts failures")
    lifetimes = validate_allocation_generations(events, rows)
    coverage = writer_c_coverage(events, rows, capture)
    first = min(submissions, default=None)
    return {
        "accepted": complete,
        "capture_id": identity,
        "retention_complete": complete,
        "executable_ready": False,
        "external_requirements": [
            "GPU seeds/references and completion receipts",
            "guest effect/continuation records",
        ],
        "bindings": len(rows),
        "failed_bindings": failed,
        "unique_files": len(files),
        "published_bytes": sum(files.values()),
        "retained_bytes": result.get("payload_bytes", 0),
        "known_gaps": result.get("payload_known_gaps", 0),
        "failures": result.get("payload_failures", 0),
        "allocator_lifetime_gaps": result.get("payload_allocator_gaps", 0),
        "allocation_failures": result.get("allocation_failures", 0),
        "allocation_overflow": result.get("allocation_overflow", 0),
        "allocation_overlaps": result.get("allocation_overlaps", 0),
        "allocation_generations": lifetimes,
        "unobserved_allocation_bindings": unobserved_allocations,
        "writer_c_coverage": coverage,
        "writer_c_region_failures": result.get("region_failures", 0),
        "writer_c_pre_arm_region_records": result.get("pre_arm_region_records", 0),
        "writer_c_pre_arm_region_overflow": result.get("pre_arm_region_overflow", 0),
        "writer_c_pre_arm_region_failures": result.get("pre_arm_region_failures", 0),
        "writer_c_coverage_complete": all(
            use["closed"] for use in coverage if use["applicable"]
        ),
        "first_submission": first,
        "first_submission_bindings": [
            r for r in rows if first is not None and r["submission"] == first
        ],
    }


def opening_allocations(events):
    """A later window's opening snapshot stands for allocations made before it opened."""
    for event in events:
        if event["kind"] != "allocation_snapshot":
            yield event
            continue
        for allocation in event.get("allocations", []):
            yield {
                **allocation,
                "kind": "allocation",
                "sequence": event["sequence"],
            }


def validate_allocation_generations(events, rows):
    lifetimes = {}
    live = {}
    addresses = []
    for event in opening_allocations(sorted(events, key=lambda e: e["sequence"])):
        generation = event.get("allocation_generation", 0)
        if not generation:
            continue
        if event["kind"] == "allocation":
            if generation in lifetimes or event.get("bytes", 0) <= 0:
                raise ValueError("invalid allocation generation")
            start = bisect_left(addresses, event["address"])
            if start and (
                live[addresses[start - 1]]["address"]
                + live[addresses[start - 1]]["bytes"]
                > event["address"]
            ):
                start -= 1
            stop = bisect_left(addresses, event["address"] + event["bytes"])
            for address in addresses[start:stop]:
                live.pop(address)["free"] = event["sequence"]
            addresses[start:stop] = [event["address"]]
            lifetimes[generation] = {**event, "free": None}
            live[event["address"]] = lifetimes[generation]
        elif event["kind"] == "allocation_free":
            allocation = lifetimes.get(generation)
            if (
                not allocation
                or allocation["free"]
                or allocation["address"] != event["address"]
            ):
                raise ValueError("unpaired allocation free")
            allocation["free"] = event["sequence"]
            del live[event["address"]]
            addresses.pop(bisect_left(addresses, event["address"]))
    for row in rows:
        generation = row.get("allocation_generation", 0)
        if not generation:
            continue
        allocation = lifetimes.get(generation)
        address = row["address"]
        if not row.get("physical") and address >= 0xA0000000:
            address = (address & 0x1FFFFFFF) + (0x1000 if address >= 0xE0000000 else 0)
        if (
            not allocation
            or allocation["sequence"] > row["event"]
            or (allocation["free"] is not None and allocation["free"] <= row["event"])
            or address < allocation["address"]
            or address + row["bytes"] > allocation["address"] + allocation["bytes"]
        ):
            raise ValueError("payload outside observed allocation lifetime")
    return len(lifetimes)


def covered_texels(width, height, rectangles):
    """Half-open rectangle union, clipped to a conservative sampled footprint."""
    # Both region budgets plus the creation footprint may contribute rectangles.
    if not 0 < width <= 16384 or not 0 < height <= 16384 or len(rectangles) > 131073:
        raise ValueError("coverage extent exceeds budget")
    clipped = []
    for left, top, right, bottom in rectangles:
        if left > right or top > bottom:
            raise ValueError("reversed coverage rectangle")
        clipped.append(
            (max(0, left), max(0, top), min(width, right), min(height, bottom))
        )
    # Sweep only rectangle edges; no per-texel allocation or iteration.
    edges = sorted(
        {0, height, *(y for r in clipped for y in (r[1], r[3]) if 0 <= y <= height)}
    )
    area = 0
    for top, bottom in zip(edges, edges[1:]):
        intervals = sorted(
            (x0, x1) for x0, y0, x1, y1 in clipped if y0 <= top < y1 and x1 > x0
        )
        end, length = 0, 0
        for left, right in intervals:
            length += max(0, right - max(left, end))
            end = max(end, right)
        area += length * (bottom - top)
    return area


def writer_c_coverage(events, rows, capture=None):
    def physical_address(row):
        address = row["address"]
        return (
            (address & 0x1FFFFFFF) + (0x1000 if address >= 0xE0000000 else 0)
            if not row.get("physical") and address >= 0xA0000000
            else address
        )

    by_resource, versions, writers = {}, {}, {}
    for row in rows:
        key = (row["resource"], row["generation"])
        versions.setdefault(key, set()).add(row["version"])
        if not row.get("error") and row.get("sha256"):
            by_resource.setdefault(key, []).append(row)
    for event in events:
        for update in event.get("payload_updates", []):
            key = (update["resource"], update["generation"])
            versions.setdefault(key, set()).add(update["version"])
            writers.setdefault(key, []).append((event, update))
        for use in event.get("payload_texture_uses", []):
            if "version" in use:
                key = (use.get("resource"), use.get("generation"))
                versions.setdefault(key, set()).add(use["version"])
    output = []
    for event in events:
        for use in event.get("payload_texture_uses", []):
            tex = use.get("texture", {})
            key = (use.get("resource"), use.get("generation"))
            history = writers.get(key, [])
            if tex.get("format") != 2 or not (
                use.get("history_candidate")
                or any(w.get("writer_path") == "C" for w, _ in history)
            ):
                continue
            row = {
                "event": event["sequence"],
                "slot": use["slot"],
                "resource": key[0],
                "generation": key[1],
                "version": use["version"],
                "plane": use["plane"],
                "applicable": False,
                "closed": False,
            }
            output.append(row)
            if use["plane"] != "base" or use.get("gpu_produced") or tex.get("tiled"):
                row["status"] = "not_applicable"
                row["reason"] = "mip plane, GPU-produced storage or tiled layout"
                continue
            row["applicable"] = True
            width, height = tex["width"], tex["height"]
            allocation = use.get("allocation_generation", 0)
            payloads = [
                r
                for r in by_resource.get(key, [])
                if r["event"] <= event["sequence"]
                and r["version"] <= use["version"]
                and r.get("allocation_generation", 0) == allocation
            ]
            initial = next(
                (
                    r
                    for r in sorted(payloads, key=lambda r: r["version"])
                    if r["role"] == "creation_initial_bytes"
                    and r.get("physical")
                    and r["address"] == tex.get("base")
                    and r["bytes"] >= tex.get("base_bytes", 1 << 63)
                ),
                None,
            )
            rectangles = [[0, 0, width, height]] if initial else []
            accounted = {initial["version"]} if initial else set()
            replay = (
                bytearray((capture / initial["file"]).read_bytes()[: tex["base_bytes"]])
                if capture is not None and initial
                else None
            )
            complete_regions = not use.get("pre_arm_region_overflow", False)
            reasons = []
            if not complete_regions:
                reasons.append("pre-arm region budget overflow")
            for writer, update in sorted(history, key=lambda item: item[1]["version"]):
                if (
                    writer["sequence"] > event["sequence"]
                    or update["version"] > use["version"]
                ):
                    continue
                if writer.get("writer_path") != "C" or writer["kind"] != "cpu_writer":
                    reasons.append("non-C writer version")
                    continue
                regions = writer.get("region_updates", [])
                valid_update = len(regions) == writer.get("region_count")
                valid_update &= writer.get("mip", 0) == 0
                for index, region in enumerate(regions):
                    rect, source = region["destination_rect"], region["source_rect"]
                    nrows = rect[3] - rect[1]
                    row_bytes = rect[2] - rect[0]
                    written = nrows if row_bytes else 0
                    valid = (
                        region.get("index") == index
                        and 0 <= rect[0] <= rect[2] <= width
                        and 0 <= rect[1] <= rect[3] <= height
                        and region.get("rows_written") == written
                        and len(region.get("rows", [])) == written
                        and region.get("row_bytes") == row_bytes
                        and tex.get("pitch", 0) >= width
                        and source[2] - source[0] == row_bytes
                        and source[3] - source[1] == nrows
                    )
                    retained_rows = []
                    for row_index, retained in enumerate(region.get("rows", [])):
                        match = next(
                            (
                                r
                                for r in payloads
                                if r["event"] == writer["sequence"]
                                and r["version"] == update["version"]
                                and r["role"] == f"writer_C_region:{index}:{row_index}"
                                and r["sha256"] == retained.get("sha256")
                                and r["bytes"] == retained.get("bytes") == row_bytes
                                and physical_address(r)
                                == tex["base"]
                                + (rect[1] + row_index) * tex.get("pitch", 0)
                                + rect[0]
                            ),
                            None,
                        )
                        valid &= (
                            not retained.get("error")
                            and retained.get("row") == row_index
                            and match is not None
                        )
                        retained_rows.append(match)
                    valid_update &= valid
                    if valid:
                        rectangles.append(rect)
                        if replay is not None:
                            for retained in retained_rows:
                                offset = physical_address(retained) - tex["base"]
                                data = (capture / retained["file"]).read_bytes()
                                if offset + len(data) > len(replay):
                                    valid_update = False
                                else:
                                    replay[offset : offset + len(data)] = data
                complete_regions &= valid_update
                if valid_update:
                    accounted.add(update["version"])
            # Versions are capture-global IDs, not a contiguous per-resource counter.
            required = {v for v in versions.get(key, set()) if v <= use["version"]}
            unaccounted = sorted(required - accounted)
            if unaccounted:
                reasons.append("unaccounted resource versions")
            if not complete_regions:
                reasons.append("incomplete region history")
            covered = covered_texels(width, height, rectangles)
            if covered != width * height:
                reasons.append("uncovered sampled texels")
            if not allocation:
                reasons.append("unobserved allocation lifetime")
            replay_matches = None
            if capture is not None:
                sampled = next(
                    (
                        r
                        for r in payloads
                        if r["role"] == "sampled_base"
                        and r["event"] == event["sequence"]
                        and r["version"] == use["version"]
                        and r.get("physical")
                        and r["address"] == tex["base"]
                        and r["bytes"] == tex.get("base_bytes")
                    ),
                    None,
                )
                if replay is None or sampled is None or len(replay) != sampled["bytes"]:
                    reasons.append("initial or sampled replay payload unavailable")
                else:
                    replay_matches = (
                        hashlib.sha256(replay).hexdigest() == sampled["sha256"]
                    )
                    if not replay_matches:
                        reasons.append("sampled_base replay hash mismatch")
            row.update(
                covered_texels=covered,
                uncovered_texels=width * height - covered,
                region_history_complete=bool(complete_regions),
                unaccounted_versions=unaccounted,
                replay_matches_sampled=replay_matches,
                closed=not reasons,
                status="closed" if not reasons else "open",
                reason="; ".join(dict.fromkeys(reasons)),
            )
    return output


def target_summary(capture, result):
    if "target_manifest" not in result:
        return None
    try:
        if result["target_manifest"] != "target-manifest.toml":
            raise ValueError("unexpected target manifest path")
        with (capture / "target-manifest.toml").open("rb") as stream:
            manifest = tomllib.load(stream)
        if manifest.get("schema") != 1 or manifest.get("capture_id") != result.get(
            "capture_id"
        ):
            raise ValueError("target capture identity mismatch")
        rows = manifest.get("records", [])
        if (
            len(rows) != result.get("target_records")
            or len(rows) > manifest.get("record_budget", 0)
            or manifest.get("record_budget") != result.get("target_record_budget")
            or manifest.get("byte_budget") != result.get("target_byte_budget")
        ):
            raise ValueError("target record budget mismatch")
        total, references, seeds = 0, set(), 0
        failed = False
        for index, row in enumerate(rows):
            if row.get("error"):
                failed = True
                if row.get("file") or row.get("sha256") or row.get("bytes"):
                    raise ValueError("failed target has bytes")
                continue
            if (
                row.get("kind") not in ("seed", "reference")
                or not row.get("generation")
                or not row.get("completion")
                or any(
                    not row.get(k)
                    for k in (
                        "resource",
                        "aspect",
                        "format",
                        "sample_layout",
                        "resolve",
                    )
                )
                or any(
                    type(row.get(k)) is not int or row[k] <= 0
                    for k in ("generation", "completion", "samples", "bytes_per_sample")
                )
                or not 1 <= row.get("samples", 0) <= 16
                or not 1 <= row.get("bytes_per_sample", 0) <= 16
            ):
                raise ValueError("target interpretation missing")
            rect = row["rectangle"]
            if (
                len(rect) != 4
                or any(type(x) is not int or x < 0 for x in rect)
                or rect[2] <= rect[0]
                or rect[3] <= rect[1]
                or row.get("rows") != rect[3] - rect[1]
                or row.get("row_bytes", 0) <= 0
                or row["row_bytes"]
                != (rect[2] - rect[0]) * row["samples"] * row["bytes_per_sample"]
                or row.get("bytes") != row["rows"] * row["row_bytes"]
            ):
                raise ValueError("target footprint mismatch")
            name = f"targets/{index}.bin"
            path = capture / name
            if row.get("file") != name or not path.resolve().is_relative_to(
                capture.resolve()
            ):
                raise ValueError("target path mismatch")
            with path.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            if path.stat().st_size != row["bytes"] or digest != row.get("sha256"):
                raise ValueError("target payload mismatch")
            total += row["bytes"]
            if row["kind"] == "reference":
                if row["frame"] in references:
                    raise ValueError("duplicate reference frame")
                references.add(row["frame"])
            else:
                if row["frame"] != result["begin_frame"]:
                    raise ValueError("seed not at opening boundary")
                seeds += 1
        if total != result.get("target_bytes") or total > manifest["byte_budget"]:
            raise ValueError("target byte budget mismatch")
        if failed or any(
            result.get(k)
            for k in (
                "target_failures",
                "target_gaps",
                "target_overflow",
                "target_failure_reason",
            )
        ):
            raise ValueError("nonzero target failure counters or unavailable receipts")
        if references != set(range(result["begin_frame"], result["end_frame"])):
            raise ValueError("missing closing-present reference")
        if not seeds:
            raise ValueError(
                "opening footprints absent; no recorded clear establishes them"
            )
        # Receipt integrity alone cannot establish complete first-use footprint discovery.
        return {
            "accepted": bool(result.get("target_complete")),
            "executable_ready": False,
            "bytes": total,
            "records": len(rows),
            "reference_frames": sorted(references),
        }
    except (OSError, ValueError, KeyError, TypeError) as error:
        return rejected_stream(result, "target", str(error))


def effect_summary(capture, result):
    """Accept only a validated effect stream with zero failure counters.

    A missing or unreadable stream, or an invalid one the producer already
    reported as failed, is rejected without aborting the metadata analysis.
    """
    if "effect_file" not in result:
        return None
    if result["effect_file"] != "route-effects.bin":
        raise ValueError("unexpected effect file path")
    path = capture / result["effect_file"]
    if not path.resolve().is_relative_to(capture.resolve()):
        raise ValueError("effects escape capture directory")
    try:
        return validate_effects(path, result)
    except OSError:
        reason = "effect file unreadable"
    except ValueError as error:
        if not result.get("effect_failure_reason"):
            raise
        reason = f"effect stream invalid: {error}"
    return rejected_stream(result, "effect", reason)


def validate_effects(path, result):
    budget = result.get("effect_byte_budget", 0)
    if type(budget) is not int or not 0 < budget <= 10 * 1024 * 1024:
        raise ValueError("invalid effect byte budget")
    if not 64 <= path.stat().st_size <= budget + 64:
        raise ValueError("invalid effect file size")
    data = path.read_bytes()
    magic, schema, header, size, lost, reads, gaps, unfinished, readers = (
        struct.unpack_from("<8sII6Q", data)
    )
    if magic != b"RREVFX01" or schema != 1 or header != 64:
        raise ValueError("invalid effect header")
    if size != len(data) - header or readers >> 33:
        raise ValueError("invalid effect header extent or reader mask")
    counters = {
        "bytes": size,
        "lost_records": lost,
        "failed_reads": reads,
        "gaps": gaps,
        "unfinished_calls": unfinished,
    }
    for name, value in counters.items():
        if result.get(f"effect_{name}") != value:
            raise ValueError(f"effect {name} differs from header")
    offset, sequence, unchanged = header, 0, 0
    lengths = {1: 64, 2: 72, 3: 80, 4: 104, 5: 64, 6: 128, 7: 68}
    while offset < len(data):
        if len(data) - offset < 48:
            raise ValueError("truncated effect record")
        length, kind, reserved, ordinal = struct.unpack_from("<IHHQ", data, offset)
        if (
            length != lengths.get(kind)
            or length > len(data) - offset
            or reserved
            or ordinal <= sequence
        ):
            raise ValueError("invalid effect record")
        if kind == 2:
            skipped, fields = struct.unpack_from("<II", data, offset + 64)
            if fields not in (31, 33) or skipped > fields:
                raise ValueError("invalid effect unchanged field count")
            unchanged += skipped
        sequence = ordinal
        offset += length
    failed = bool(
        lost or reads or gaps or unfinished or result.get("effect_failure_reason")
    )
    if result.get("effect_complete") and failed:
        raise ValueError("effect completeness contradicts failures")
    if not lost and result.get("effect_unchanged_fields") != unchanged:
        raise ValueError("effect unchanged field count differs from records")
    return {
        **counters,
        "reader_mask": readers,
        "unchanged_fields": unchanged,
        "accepted": bool(result.get("effect_complete")) and not failed,
        "coverage": result.get("effect_coverage", "unknown"),
    }


def render(graph, uses=False):
    lines = [
        "# Scene resource producer graph",
        "",
        f"Window: frames {graph['window']['begin_frame']}..{graph['window']['end_frame']}.",
        graph["sampling_evidence"] + ".",
        graph["range_evidence"] + ".",
        "Full graph and per-use rows are available in JSON output.",
        "",
        "| Texture | Base / mips | Exact form | Uses | Producer classes (uses) | "
        "Span producers (uses) | Ambiguous uses / overwriters (uses) | "
        "Aliases | CPU writers | "
        "Open categories (uses) |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]
    for row in graph["texture_summary"]:
        lines.append(
            f"| {row['id']} | {row['base']:#x} / {row['mip_base']:#x} | "
            f"{json.dumps(row['form'], sort_keys=True)} | {row['use_count']} | "
            f"{json.dumps(row['producer_classes'], sort_keys=True)} | "
            f"{json.dumps(row['span_producers'])} | {row['ambiguous_uses']} / "
            f"{json.dumps(row['overwriting_producers'])} | {row['aliases']} | "
            f"{json.dumps(row['cpu_writers']) if row['cpu_writers'] else 'open'} | "
            f"{json.dumps(row['open_categories'], sort_keys=True)} |"
        )
    lines += [
        "",
        "## Attachments and window resolves",
        "",
        "Consumer lists show count and first/last sequence; exact lists are in JSON.",
        "",
        "| Kind | Event | Object / source | Destination | Draw consumers | "
        "Resolve consumers | Texture consumers | Producer overlaps |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for attachment in graph["attachments"]:
        lines.append(
            f"| {attachment['kind']} | {attachment['sequence']} | "
            f"{attachment.get('object', 0):#x} | - | "
            f"{sequence_extent(attachment['draw_consumers'])} | "
            f"{sequence_extent(attachment['resolve_consumers'])} | - | - |"
        )
    for resolve in graph["window_resolves"]:
        lines.append(
            f"| resolve | {resolve['sequence']} | "
            f"{resolve.get('source_attachment', {}).get('object', 0):#x} | "
            f"{resolve.get('destination_base', 0):#x} | - | - | "
            f"{resolve['consumers']} | {sequence_extent(resolve['overlaps'])} |"
        )
    if uses:
        lines += [
            "",
            "## Per-use producer choices",
            "",
            "| Texture | Draw | Frame | Entry | Slot | Producer | Spans | "
            "Overlapping producers | Overwriters | Ambiguous | Raw target | "
            "Proved CPU upload | Open category |",
            "|---|---|---|---|---|---|---|---|---|---|---|---|---|",
        ]
        for row in graph["textures"]:
            for use in row["uses"]:
                lines.append(
                    f"| {row['id']} | {use['sequence']} | {use['frame']} | "
                    f"{use['entry']} | {use['slot']} | {use['producer_class']} "
                    f"{use['producer_sequence']} | "
                    f"{json.dumps(use['range_producers'])} | "
                    f"{use['overlapping_producers']} | {use['overwritten_by']} | "
                    f"{use['ambiguous']} | {use['raw_target']} | "
                    f"{use['proved_cpu_upload']} | {use['open_category']} |"
                )
    lines += ["", "## Scene open native-producer list", ""]
    for row in graph["open_native_producers"]:
        identity = (
            f"Texture {row['texture']}"
            if row["texture"] is not None
            else f"Draw {row['sequence']} slot {row['slot']}"
        )
        lines.append(
            f"- {identity}: {', '.join(row['categories'])}. "
            + (
                f"CPU writers: {json.dumps(row['cpu_writers'])}; "
                if row.get("cpu_writers")
                else (
                    "CPU writer still open; "
                    if row.get("cpu_writer_status") == "open"
                    else ""
                )
            )
            + "; ".join(row["obligations"])
            + "."
        )
    if graph.get("payloads") is not None and "rejection_reason" in graph["payloads"]:
        lines += [
            "",
            "## Retained CPU inputs",
            "",
            f"Rejected: {graph['payloads']['rejection_reason']}.",
        ]
    elif graph.get("payloads") is not None:
        payloads = graph["payloads"]
        lines += [
            "",
            "## Retained CPU inputs",
            "",
            f"Capture: {payloads['capture_id']}.",
            f"Verified {payloads['unique_files']} unique files, {payloads['published_bytes']} bytes.",
            f"Retention complete: {payloads['retention_complete']}; known gaps: {payloads['known_gaps']}.",
            f"Allocator lifetime gaps: {payloads['allocator_lifetime_gaps']}; payload failures: {payloads['failures']}.",
            "GPU completion and executable readiness require the external records listed in JSON.",
        ]
    if graph.get("effects") is not None and "rejection_reason" in graph["effects"]:
        lines += [
            "",
            "## Guest effects",
            "",
            f"Rejected: {graph['effects']['rejection_reason']}.",
        ]
    elif graph.get("effects") is not None:
        effects = graph["effects"]
        lines += [
            "",
            "## Guest effects",
            "",
            f"Accepted: {effects['accepted']}; record bytes: {effects['bytes']}.",
            f"Lost records: {effects['lost_records']}; failed reads: {effects['failed_reads']}; gaps: {effects['gaps']}; unfinished calls: {effects['unfinished_calls']}.",
        ]
    if graph.get("payloads", {}).get("writer_c_coverage"):
        lines += [
            "",
            "## Writer C sampled coverage",
            "",
            f"Coverage complete: {graph['payloads']['writer_c_coverage_complete']}.",
            "",
            "| Draw | Slot | Resource | Covered texels | Uncovered texels | Status | Reason |",
            "|---|---|---|---|---|---|---|",
        ]
        for use in graph["payloads"]["writer_c_coverage"]:
            lines.append(
                f"| {use['event']} | {use['slot']} | {use['resource']} | "
                f"{use.get('covered_texels', 'n/a')} | {use.get('uncovered_texels', 'n/a')} | "
                f"{use['status']} | {use['reason']} |"
            )
    if graph.get("targets") is not None:
        targets = graph["targets"]
        lines += [
            "",
            "## Target seeds and references",
            "",
            f"Rejected: {targets['rejection_reason']}."
            if "rejection_reason" in targets
            else f"Accepted: {targets['accepted']}; lossless bytes: {targets['bytes']}.",
        ]
    return "\n".join(lines) + "\n"


def sequence_extent(sequences):
    if not sequences:
        return "0"
    return f"{len(sequences)} ({sequences[0]}..{sequences[-1]})"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument(
        "--uses", action="store_true", help="include per-use Markdown rows"
    )
    args = parser.parse_args(argv)
    if args.output.exists():
        parser.error("--output already exists; preserve it and use a fresh path")
    try:
        with (args.capture / "events.toml").open("rb") as stream:
            log = tomllib.load(stream)
        with (args.capture / "result.toml").open("rb") as stream:
            result = tomllib.load(stream)
        if log.get("schema") != 1:
            raise ValueError("unsupported capture schema")
        graph = build_graph(log.get("events", []), result)
        payloads = payload_summary(args.capture, log.get("events", []), result)
        if payloads is not None:
            graph["payloads"] = payloads
        effects = effect_summary(args.capture, result)
        if effects is not None:
            graph["effects"] = effects
        targets = target_summary(args.capture, result)
        if targets is not None:
            graph["targets"] = targets
        graph["title_revision"] = log.get("title_revision", "unknown")
        graph["sdk_revision"] = log.get("sdk_revision", "unknown")
        output = (
            json.dumps(graph, indent=2, sort_keys=True) + "\n"
            if args.output.suffix == ".json"
            else render(graph, uses=args.uses)
        )
        with args.output.open("x", encoding="ascii", newline="\n") as stream:
            stream.write(output)
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
