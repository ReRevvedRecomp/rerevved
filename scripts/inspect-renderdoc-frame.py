"""Inspect a RenderDoc 1.46 capture from qrenderdoc's embedded Python.

The script is intended for ``qrenderdoc.exe --python``.  It does not use
``__file__`` because RenderDoc's Python executor does not provide it.  Paths
are supplied through environment variables so the same script can be reused
for captures from different runs.

Required environment variable:
  RENDERDOC_CAPTURE_PATH  The .rdc file to replay.

Optional environment variables:
  RENDERDOC_REPORT_PATH   JSON report path (default: renderdoc-frame-report.json)
  RENDERDOC_MARKERS_PATH  JSON file containing expected marker definitions.
  RENDERDOC_EXPECTED_MARKERS_JSON  Inline JSON marker definitions.
  RENDERDOC_EVENT_IDS     Comma-separated or JSON list of event IDs to inspect.
  RENDERDOC_MAX_EVENTS    Maximum number of unfiltered action events to inspect.

Marker input accepts a list of strings or objects.  Objects may use ``name``,
``marker``, ``text``, or ``label`` and may optionally include ``event_id``.
Mapping is exact first and then a unique case-insensitive substring match.
An explicitly requested marker that cannot be mapped makes the report fail.
"""

import json
import os
import re
import traceback
from pathlib import Path

SCHEMA_VERSION = 1
DEFAULT_MAX_EVENTS = 256
ZERO_RESOURCE_ID = "0000000000000000"
SKIP_OBJECT_FIELDS = {
    "this",
    "thisown",
    "own",
    "next",
    "prev",
    "append",
    "acquire",
    "disown",
}


def _text(value):
    try:
        return str(value)
    except Exception:
        return "<unprintable>"


def _scalar(value):
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    return _text(value)


def _safe_value(value, depth=0):
    """Convert small RenderDoc binding values without walking large blobs."""
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if depth > 2:
        return _text(value)
    if isinstance(value, (bytes, bytearray)):
        return {"byte_length": len(value)}
    if isinstance(value, dict):
        result = {}
        for index, (key, item) in enumerate(value.items()):
            if index >= 64:
                result["truncated"] = True
                break
            result[_text(key)] = _safe_value(item, depth + 1)
        return result
    if isinstance(value, (list, tuple)):
        result = [_safe_value(item, depth + 1) for item in value[:64]]
        if len(value) > 64:
            result.append("<truncated>")
        return result
    return _text(value)


def _safe_call(function, *args):
    try:
        return function(*args), None
    except Exception as error:
        return None, _text(error)


def _object_fields(value, names):
    result = {}
    if value is None:
        return result
    for name in names:
        try:
            member = getattr(value, name)
        except Exception:
            continue
        if callable(member):
            continue
        result[name] = _safe_value(member)
    return result


def _public_fields(value, max_fields=48):
    """Capture useful scalar fields from generated binding records."""
    result = {}
    try:
        names = dir(value)
    except Exception:
        return result
    for name in names:
        if name.startswith("_") or name in SKIP_OBJECT_FIELDS:
            continue
        if len(result) >= max_fields:
            result["truncated"] = True
            break
        try:
            member = getattr(value, name)
        except Exception:
            continue
        if callable(member) or name in ("rawBytes", "initialisationChunks"):
            continue
        try:
            result[name] = _safe_value(member)
        except Exception:
            continue
    return result


def _resource_id(value):
    if value is None:
        return None
    result = _text(value)
    if result in ("", "0", ZERO_RESOURCE_ID):
        return None
    return result


def _resource_ref(value, resource_names):
    rid = _resource_id(value)
    if rid is None:
        return {"id": None, "name": None}
    return {"id": rid, "name": resource_names.get(rid)}


def _descriptor(value, resource_names):
    if value is None:
        return None
    result = _object_fields(
        value,
        (
            "type",
            "view",
            "textureType",
            "firstMip",
            "numMips",
            "firstSlice",
            "numSlices",
            "byteOffset",
            "byteSize",
            "elementByteSize",
            "bufferStructCount",
            "counterByteOffset",
            "flags",
        ),
    )
    result["format"] = value.format.Name()
    try:
        result["resource"] = _resource_ref(value.resource, resource_names)
    except Exception:
        pass
    return result


def _used_descriptor(value, resource_names):
    if value is None:
        return None
    result = {"fields": _public_fields(value)}
    try:
        result["access"] = _public_fields(value.access)
    except Exception:
        pass
    try:
        result["descriptor"] = _descriptor(value.descriptor, resource_names)
    except Exception:
        pass
    try:
        result["sampler"] = _public_fields(value.sampler)
        result["sampler"]["filter"] = _public_fields(value.sampler.filter)
    except Exception:
        pass
    return result


def _stage_values(rd):
    result = []
    for name in (
        "Vertex",
        "Hull",
        "Domain",
        "Geometry",
        "Pixel",
        "Compute",
        "Task",
        "Mesh",
    ):
        value = getattr(rd.ShaderStage, name, None)
        if value is not None:
            result.append((name, value))
        elif name not in ("Task", "Mesh"):
            raise RuntimeError("RenderDoc shader stage is unavailable: " + name)
    return result


def _iterable(value):
    if value is None:
        return []
    try:
        return list(value)
    except Exception:
        return []


def _action_name(action):
    name, error = _safe_call(action.GetName)
    if error is None and name:
        return _text(name)
    try:
        if action.customName:
            return _text(action.customName)
    except Exception:
        pass
    return ""


def _journal_marker(name):
    match = re.search(r"REXGLUE_FRAME_(DRAW|COPY|SWAP)_([0-9A-Fa-f]{16})", name or "")
    if not match:
        return None
    return {
        "kind": match.group(1).casefold(),
        "event_id": int(match.group(2), 16),
        "marker": match.group(0),
    }


def _action_record(action, rd, parent_event_id=None, path=None):
    path = list(path or [])
    name = _action_name(action)
    event_id = None
    action_id = None
    try:
        event_id = int(action.eventId)
    except Exception:
        pass
    try:
        action_id = int(action.actionId)
    except Exception:
        pass
    current_path = path + ([name] if name else [])
    record = {
        "event_id": event_id,
        "action_id": action_id,
        "parent_event_id": parent_event_id,
        "name": name,
        "path": current_path,
        "kind": "action",
    }
    for field in (
        "flags",
        "numIndices",
        "numInstances",
        "vertexOffset",
        "indexOffset",
        "baseVertex",
        "instanceOffset",
        "dispatchDimension",
        "dispatchThreadsDimension",
        "copySource",
        "copyDestination",
        "copySourceSubresource",
        "copyDestinationSubresource",
    ):
        try:
            record[field] = _safe_value(getattr(action, field))
        except Exception:
            continue
    for flag_name, kind in (
        ("SetMarker", "marker"),
        ("PushMarker", "marker"),
        ("PopMarker", "marker"),
        ("Drawcall", "draw"),
        ("Dispatch", "dispatch"),
        ("Copy", "copy"),
        ("Resolve", "resolve"),
        ("Clear", "clear"),
    ):
        if action.flags & getattr(rd.ActionFlags, flag_name):
            record["kind"] = kind
    marker = _journal_marker(name)
    if marker is not None:
        record["journal_marker"] = marker
    record["children"] = [
        _action_record(child, rd, event_id, current_path)
        for child in _iterable(getattr(action, "children", None))
    ]
    try:
        record["event_ids"] = [int(value.eventId) for value in _iterable(action.events)]
    except Exception:
        record["event_ids"] = []
    return record


def _flatten_actions(records):
    result = []
    for record in records:
        result.append(record)
        result.extend(_flatten_actions(record.get("children", [])))
    return result


def _marker_input():
    raw = os.environ.get("RENDERDOC_EXPECTED_MARKERS_JSON")
    marker_path = os.environ.get("RENDERDOC_MARKERS_PATH")
    if not raw and marker_path:
        raw = Path(marker_path).read_text()
    if not raw:
        return []
    value = json.loads(raw)
    if isinstance(value, dict):
        for key in (
            "expected_markers",
            "markers",
            "expected",
            "guest_markers",
            "events",
        ):
            if key in value:
                value = value[key]
                if key == "events":
                    value = [
                        {
                            "marker": item["marker"],
                            "journal_id": item["id"],
                            "journal_kind": item["kind"],
                            "requires_draw": item["kind"] == "draw",
                            "expected_shader_stages": (
                                ["Vertex"]
                                + (
                                    ["Pixel"]
                                    if int(item["host"]["pixel_shader_hash"], 16)
                                    else []
                                )
                                if item["kind"] == "draw"
                                else []
                            ),
                            "used_texture_mask": item.get("host", {}).get(
                                "used_texture_mask", 0
                            ),
                        }
                        for item in value
                        if item["kind"] in ("copy", "swap")
                        or (item["kind"] == "draw" and item["outcome"]["host_issued"])
                    ]
                break
        else:
            value = [{"name": key, "value": item} for key, item in value.items()]
    if not isinstance(value, list):
        raise ValueError(
            "marker configuration must be a list or an object containing one"
        )
    return value


def _marker_spec(value, index):
    if isinstance(value, str):
        return {"index": index, "label": value, "pattern": value}
    if not isinstance(value, dict):
        raise ValueError("marker entry %d must be a string or object" % index)
    pattern = None
    for key in (
        "marker",
        "name",
        "text",
        "label",
        "guest_marker",
        "guestMarker",
        "expected",
    ):
        if value.get(key) is not None:
            pattern = _text(value[key])
            break
    event_id = None
    for key in ("event_id", "eventId", "event"):
        if value.get(key) is not None:
            raw_event_id = value[key]
            event_id = (
                int(raw_event_id, 0)
                if isinstance(raw_event_id, str)
                else int(raw_event_id)
            )
            break
    return {
        "index": index,
        "label": pattern
        or ("event %d" % event_id if event_id is not None else "marker %d" % index),
        "pattern": pattern,
        "event_id": event_id,
        "requires_draw": value.get("requires_draw", False),
        "input": value,
    }


def _resolve_markers(actions, values):
    flat = _flatten_actions(actions)
    specs = [_marker_spec(value, index) for index, value in enumerate(values)]
    found = []
    unresolved = []
    for spec in specs:
        candidates = flat
        if spec.get("event_id") is not None:
            candidates = [
                item for item in candidates if item.get("event_id") == spec["event_id"]
            ]
        pattern = spec.get("pattern")
        if pattern:
            exact = [item for item in candidates if item.get("name", "") == pattern]
            if exact:
                candidates = exact
            else:
                folded = pattern.casefold()
                candidates = [
                    item
                    for item in candidates
                    if folded in item.get("name", "").casefold()
                ]
        if len(candidates) != 1:
            unresolved.append(
                {
                    "expected": spec.get("input", spec.get("label")),
                    "reason": "not found" if not candidates else "ambiguous",
                    "candidate_event_ids": [
                        item.get("event_id") for item in candidates
                    ],
                }
            )
            continue
        item = candidates[0]
        item_index = flat.index(item)
        associated_action = None
        for following in flat[item_index + 1 :]:
            if following.get("event_id") is None or following.get(
                "event_id"
            ) <= item.get("event_id", 0):
                continue
            if following.get("journal_marker") is not None:
                break
            if following.get("kind") != "marker":
                associated_action = following
                break
        if spec.get("requires_draw") and (
            associated_action is None or associated_action.get("kind") != "draw"
        ):
            unresolved.append(
                {
                    "expected": spec["input"],
                    "reason": "issued marker has no adjacent draw",
                }
            )
            continue
        found.append(
            {
                "expected": spec.get("input", spec.get("label")),
                "event_id": item.get("event_id"),
                "associated_event_id": associated_action.get("event_id")
                if associated_action
                else None,
                "action_id": item.get("action_id"),
                "name": item.get("name"),
                "path": item.get("path", []),
                "journal_marker": item.get("journal_marker"),
            }
        )
    return found, unresolved


def _requested_event_ids():
    raw = os.environ.get("RENDERDOC_EVENT_IDS", "")
    if not raw:
        return []
    try:
        value = json.loads(raw)
        if isinstance(value, list):
            return [
                int(item, 0) if isinstance(item, str) else int(item) for item in value
            ]
    except Exception:
        pass
    return [int(item, 0) for item in re.split(r"[,; ]+", raw) if item]


def _usage_kind(usage):
    name = _text(usage).split(".")[-1]
    writes = (
        "RWResource",
        "ColorTarget",
        "DepthStencilTarget",
        "CopyDst",
        "ResolveDst",
        "Clear",
        "CPUWrite",
        "StreamOut",
        "Discard",
        "GenMips",
    )
    reads = (
        "Constants",
        "Resource",
        "VertexBuffer",
        "IndexBuffer",
        "CopySrc",
        "ResolveSrc",
        "InputTarget",
        "Indirect",
    )
    is_write = any(token in name for token in writes)
    is_read = any(token in name for token in reads)
    if is_write and is_read:
        return "read_write"
    if is_write:
        return "write"
    if is_read:
        return "read"
    return "other"


def _pipeline_snapshot(controller, rd, resource_names):
    pipe = controller.GetPipelineState()
    snapshot = {
        "capture_api": {
            "d3d11": bool(pipe.IsCaptureD3D11()),
            "d3d12": bool(pipe.IsCaptureD3D12()),
            "vulkan": bool(pipe.IsCaptureVK()),
            "opengl": bool(pipe.IsCaptureGL()),
        },
        "graphics_pipeline": _resource_ref(
            pipe.GetGraphicsPipelineObject(), resource_names
        ),
        "compute_pipeline": _resource_ref(
            pipe.GetComputePipelineObject(), resource_names
        ),
        "shaders": {},
        "descriptors": [],
        "read_only_resources": {},
        "read_write_resources": {},
        "constant_blocks": {},
        "outputs": [],
        "depth_target": _descriptor(pipe.GetDepthTarget(), resource_names),
        "vertex_buffers": [
            _public_fields(item) for item in _iterable(pipe.GetVBuffers())
        ],
        "index_buffer": _public_fields(pipe.GetIBuffer()),
    }
    for key, stage in _stage_values(rd):
        stage_record = {
            "shader": _resource_ref(pipe.GetShader(stage), resource_names),
            "entry_point": _safe_value(pipe.GetShaderEntryPoint(stage)),
        }
        reflection = pipe.GetShaderReflection(stage)
        if reflection is not None:
            stage_record["reflection"] = _object_fields(
                reflection,
                (
                    "resourceId",
                    "stage",
                    "entryPoint",
                    "encoding",
                    "dispatchThreadsDimension",
                    "outputTopology",
                ),
            )
            for binding_name in (
                "readOnlyResources",
                "readWriteResources",
                "constantBlocks",
                "samplers",
            ):
                try:
                    stage_record[binding_name] = [
                        _public_fields(item)
                        for item in _iterable(getattr(reflection, binding_name))
                    ]
                except Exception:
                    continue
        snapshot["shaders"][key] = stage_record
        snapshot["read_only_resources"][key] = [
            _used_descriptor(item, resource_names)
            for item in _iterable(pipe.GetReadOnlyResources(stage, True))
        ]
        snapshot["read_write_resources"][key] = [
            _used_descriptor(item, resource_names)
            for item in _iterable(pipe.GetReadWriteResources(stage, True))
        ]
        snapshot["constant_blocks"][key] = [
            _used_descriptor(item, resource_names)
            for item in _iterable(pipe.GetConstantBlocks(stage, True))
        ]
    snapshot["descriptors"] = [
        _used_descriptor(item, resource_names)
        for item in _iterable(pipe.GetAllUsedDescriptors(True))
    ]
    snapshot["outputs"] = [
        _descriptor(item, resource_names) for item in _iterable(pipe.GetOutputTargets())
    ]
    return snapshot


def _report_path():
    return Path(os.environ.get("RENDERDOC_REPORT_PATH", "renderdoc-frame-report.json"))


def _validate_issued_pipeline(pipeline, expected, texture_resource_ids=()):
    if not pipeline["graphics_pipeline"]["id"]:
        raise ValueError("issued draw has no graphics pipeline")
    for stage in expected.get("expected_shader_stages", ["Vertex"]):
        if not pipeline["shaders"].get(stage, {}).get("shader", {}).get("id"):
            raise ValueError("issued draw is missing its " + stage + " shader")
    if expected.get("used_texture_mask"):
        resources = {
            item.get("descriptor", {}).get("resource", {}).get("id")
            for stage in pipeline["read_only_resources"].values()
            for item in stage
        }
        accessed = sorted(
            resource for resource in resources if resource in texture_resource_ids
        )
        declared = any(
            resource.get("isTexture")
            for stage in pipeline["shaders"].values()
            for resource in stage.get("readOnlyResources", [])
        )
        if not declared:
            raise ValueError("textured guest draw has no shader texture declaration")
        # onlyUsed omits bound descriptors that the replayed shader never accessed.
        # A static guest texture mask does not prove that any fragment sampled it.
        pipeline["guest_texture_access"] = {
            "shader_texture_mask": expected["used_texture_mask"],
            "accessed_texture_ids": accessed,
            "status": "accessed" if accessed else "no_dynamic_texture_access_reported",
        }


def _write_report(path, report):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")


def _failed_report(error, capture_path=None, details=None):
    report = {
        "schema_version": SCHEMA_VERSION,
        "status": "failed",
        "capture_path": capture_path,
        "error": {
            "type": type(error).__name__,
            "message": _text(error),
            "traceback": traceback.format_exc(),
        },
    }
    if details:
        report["details"] = details
    return report


def main(rd):
    global ZERO_RESOURCE_ID
    ZERO_RESOURCE_ID = _text(rd.ResourceId.Null())
    capture_text = os.environ.get("RENDERDOC_CAPTURE_PATH")
    if not capture_text:
        raise ValueError("RENDERDOC_CAPTURE_PATH is required")
    capture_path = str(Path(capture_text).resolve())
    report_path = _report_path()
    capture = None
    controller = None
    try:
        capture = rd.OpenCaptureFile()
        open_result = capture.OpenFile(capture_path, "", None)
        if not open_result.OK():
            raise RuntimeError("OpenFile failed: " + _text(open_result))
        support = capture.LocalReplaySupport()
        if support == rd.ReplaySupport.Unsupported:
            raise RuntimeError("local replay is unsupported: " + _text(support))
        replay_result = capture.OpenCapture(rd.ReplayOptions(), None)
        if not isinstance(replay_result, tuple) or len(replay_result) < 2:
            raise RuntimeError("OpenCapture returned an unexpected result")
        replay_status, controller = replay_result[0], replay_result[1]
        if not replay_status.OK() or controller is None:
            raise RuntimeError("OpenCapture failed: " + _text(replay_status))

        resources = controller.GetResources()
        texture_details = {}
        for texture in controller.GetTextures():
            details = _object_fields(
                texture,
                (
                    "width",
                    "height",
                    "depth",
                    "mips",
                    "arraysize",
                    "cubemap",
                    "msSamp",
                    "msQual",
                    "byteSize",
                    "dimension",
                    "type",
                    "creationFlags",
                ),
            )
            details["format"] = texture.format.Name()
            texture_details[_resource_id(texture.resourceId)] = details
        buffer_details = {
            _resource_id(buffer.resourceId): _object_fields(
                buffer, ("length", "creationFlags", "gpuAddress")
            )
            for buffer in controller.GetBuffers()
        }
        resource_names = {}
        resource_records = []
        event_usages = {}
        usage_errors = []
        for resource in resources:
            rid = _resource_id(resource.resourceId)
            if rid is None:
                continue
            name = _text(resource.name or resource.autogeneratedName)
            resource_names[rid] = name
        for resource in resources:
            rid = _resource_id(resource.resourceId)
            if rid is None:
                continue
            try:
                usages = controller.GetUsage(resource.resourceId)
                usage_records = []
                for usage in usages:
                    event_id = int(usage.eventId)
                    usage_name = _text(usage.usage)
                    row = {
                        "event_id": event_id,
                        "usage": usage_name,
                        "kind": _usage_kind(usage.usage),
                    }
                    usage_records.append(row)
                    event_usages.setdefault(str(event_id), []).append(
                        {
                            "resource": _resource_ref(
                                resource.resourceId, resource_names
                            ),
                            "usage": usage_name,
                            "kind": row["kind"],
                        }
                    )
            except Exception as error:
                usage_records = []
                usage_errors.append({"resource": rid, "error": _text(error)})
            resource_records.append(
                {
                    "id": rid,
                    "name": resource_names.get(rid),
                    "type": _safe_value(resource.type),
                    "autogenerated_name": _safe_value(resource.autogeneratedName),
                    "custom_name": _safe_value(resource.name),
                    "texture": texture_details.get(rid),
                    "buffer": buffer_details.get(rid),
                    "parent_resources": [
                        _resource_ref(item, resource_names)
                        for item in _iterable(resource.parentResources)
                    ],
                    "derived_resources": [
                        _resource_ref(item, resource_names)
                        for item in _iterable(resource.derivedResources)
                    ],
                    "usages": usage_records,
                }
            )

        root_actions = [
            _action_record(item, rd) for item in _iterable(controller.GetRootActions())
        ]
        marker_values = _marker_input()
        found_markers, unresolved_markers = _resolve_markers(
            root_actions, marker_values
        )
        selected_ids = sorted(
            set(
                event_id
                for item in found_markers
                for event_id in (item.get("event_id"), item.get("associated_event_id"))
                if event_id is not None
            )
            | set(_requested_event_ids())
            | {
                item["event_id"]
                for item in _flatten_actions(root_actions)
                if marker_values
                and item["kind"] in ("copy", "resolve", "dispatch", "clear")
            }
        )
        if not selected_ids:
            max_events = int(os.environ.get("RENDERDOC_MAX_EVENTS", DEFAULT_MAX_EVENTS))
            selected_ids = sorted(
                set(
                    item["event_id"]
                    for item in _flatten_actions(root_actions)
                    if item.get("event_id") is not None and item.get("event_id") > 0
                )
            )[:max_events]

        selected_events = []
        replay_errors = []
        pipeline_errors = []
        expected_draws = {
            marker["associated_event_id"]: marker["expected"]
            for marker in found_markers
            if isinstance(marker["expected"], dict)
            and marker["expected"].get("requires_draw")
        }
        required_pipeline_fields = (
            "capture_api",
            "graphics_pipeline",
            "compute_pipeline",
            "shaders",
            "descriptors",
            "read_only_resources",
            "read_write_resources",
            "constant_blocks",
            "outputs",
            "depth_target",
            "vertex_buffers",
            "index_buffer",
        )
        for event_id in selected_ids:
            try:
                controller.SetFrameEvent(int(event_id), True)
                pipeline = _pipeline_snapshot(controller, rd, resource_names)
                if event_id in expected_draws:
                    _validate_issued_pipeline(
                        pipeline, expected_draws[event_id], texture_details
                    )
                missing_fields = [
                    field for field in required_pipeline_fields if field not in pipeline
                ]
                if missing_fields:
                    pipeline_errors.append(
                        {"event_id": int(event_id), "missing_fields": missing_fields}
                    )
                selected_events.append(
                    {
                        "event_id": int(event_id),
                        "action": next(
                            (
                                item
                                for item in _flatten_actions(root_actions)
                                if item.get("event_id") == event_id
                            ),
                            None,
                        ),
                        "usages": event_usages.get(str(event_id), []),
                        "pipeline": pipeline,
                    }
                )
            except Exception as error:
                replay_errors.append(
                    {
                        "event_id": int(event_id),
                        "phase": "set_frame_event_or_pipeline",
                        "error": _text(error),
                        "required_pipeline_fields": list(required_pipeline_fields),
                        "traceback": traceback.format_exc(),
                    }
                )

        frame_info = controller.GetFrameInfo()
        report = {
            "schema_version": SCHEMA_VERSION,
            "status": "ok"
            if not replay_errors and not unresolved_markers
            else "failed",
            "renderdoc_version": _text(rd.GetVersionString()),
            "capture_path": capture_path,
            "driver": _text(capture.DriverName()),
            "local_replay_support": _text(support),
            "frame_info": _public_fields(frame_info),
            "actions": root_actions,
            "resources": resource_records,
            "usage_errors": usage_errors,
            "pipeline_errors": pipeline_errors,
            "markers": {
                "requested": marker_values,
                "found": found_markers,
                "unresolved": unresolved_markers,
                "status": "ok" if not unresolved_markers else "unresolved",
            },
            "selected_event_ids": selected_ids,
            "events": selected_events,
            "replay_errors": replay_errors,
        }
        if usage_errors or pipeline_errors:
            report["status"] = "failed"
        _write_report(report_path, report)
        return report["status"] == "ok"
    finally:
        if controller is not None:
            try:
                controller.Shutdown()
            except Exception:
                pass
        if capture is not None:
            try:
                capture.Shutdown()
            except Exception:
                pass


def run():
    report_path = _report_path()
    capture_text = os.environ.get("RENDERDOC_CAPTURE_PATH")
    try:
        import renderdoc as rd

        success = main(rd)
        if not success:
            print("RenderDoc inspection failed; see " + str(report_path))
    except Exception as error:
        report = _failed_report(error, capture_text)
        try:
            _write_report(report_path, report)
        except Exception as write_error:
            print("RenderDoc inspection failed: " + _text(error))
            print("Could not write report: " + _text(write_error))
        else:
            print("RenderDoc inspection failed; see " + str(report_path))
    raise SystemExit(0)


if globals().get("__name__", "__main__") == "__main__":
    run()
