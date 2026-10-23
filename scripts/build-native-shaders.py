"""Translate captured shader words using explicitly pinned external tools."""

import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pinned(root, item):
    path = (root / item["file"]).resolve(strict=True)
    if digest(path) != item["sha256"]:
        raise ValueError(f"input digest mismatch: {path}")
    return path


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise ValueError(f"unexpected translated interface: {before}")
    return source.replace(before, after)


def fetch_instructions(words):
    """Follow the pinned raw adapter's control-flow clause/input ordering."""
    if not words or len(words) % 3:
        raise ValueError("invalid shader instruction extent")
    boundary = len(words) // 3
    pair = 0
    seen = set()
    result = []
    while pair < boundary:
        block = (
            words[pair * 3] | (words[pair * 3 + 1] << 32) | (words[pair * 3 + 2] << 64)
        )
        for half in range(2):
            cf = (block >> (half * 48)) & ((1 << 48) - 1)
            if (cf >> 44) not in (1, 2, 3, 4, 5, 6, 13, 14):
                continue
            address, count, sequence = cf & 4095, (cf >> 12) & 7, (cf >> 16) & 4095
            if not address or address * 3 + count * 3 > len(words):
                raise ValueError("shader clause exceeds instruction extent")
            boundary = min(boundary, address)
            for offset in range(count):
                index = address + offset
                if (sequence >> (offset * 2)) & 1 and index not in seen:
                    seen.add(index)
                    result.append((index, words[index * 3 : index * 3 + 3]))
        pair += 1
    return result


def vertex_texture_header(common_text, words):
    textures = [code for _, code in fetch_instructions(words) if (code[0] & 31) != 0]
    if not textures:
        return common_text
    for a, b, c in textures:
        # Vertex computed LOD has no screen derivatives. This path only admits
        # ordinary normalized 2D fetches without explicit LOD or gradients.
        if (
            (a & 31) != 1
            or a & (1 << 25)
            or b & (1 << 29)
            or c & 0x1FD
            or ((c >> 14) & 3) != 1
        ):
            raise ValueError("unsupported vertex texture sampling mode")
    return replace_once(
        common_text,
        "return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / getTexture2DDimensions(texture));",
        "return texture.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / getTexture2DDimensions(texture), 0.0);",
    )


def texture_layout(recipe):
    slots = recipe["texture_fetches"]
    if (
        not isinstance(slots, list)
        or any(type(slot) is not int or not 0 <= slot <= 31 for slot in slots)
        or len(set(slots)) != len(slots)
    ):
        raise ValueError("unsupported shader texture interface")
    independent_samplers = recipe.get("independent_samplers", False)
    if type(independent_samplers) is not bool:
        raise ValueError("invalid independent sampler setting")
    dimensions = recipe.get("texture_dimensions", ["2d"] * len(slots))
    if (
        not isinstance(dimensions, list)
        or len(dimensions) != len(slots)
        or any(dimension not in ("2d", "cube") for dimension in dimensions)
    ):
        raise ValueError("unsupported shader texture dimensions")
    return slots, independent_samplers, dimensions


def render_shared_header(common_text, slots, independent_samplers, dimensions=None):
    if dimensions is None:
        dimensions = ["2d"] * len(slots)
    texture_heap_length = max(1, len(slots))
    sampler_heap_length = texture_heap_length if independent_samplers else 1
    common_text = replace_once(
        common_text,
        "g_Texture2DDescriptorHeap[1]",
        f"g_Texture2DDescriptorHeap[{texture_heap_length}]",
    )
    common_text = replace_once(
        common_text,
        "g_SamplerDescriptorHeap[1]",
        f"g_SamplerDescriptorHeap[{sampler_heap_length}]",
    )
    if "cube" in dimensions:
        common_text = replace_once(
            common_text,
            "g_TextureCubeDescriptorHeap[1]",
            f"g_TextureCubeDescriptorHeap[{texture_heap_length}]",
        )
    bindings = "".join(
        f"#define s{fetch}_Texture{'Cube' if dimensions[slot] == 'cube' else '2D'}DescriptorIndex {slot}\n"
        f"#define s{fetch}_SamplerDescriptorIndex "
        f"{slot if independent_samplers else 0}\n"
        for slot, fetch in enumerate(slots)
    )
    return bindings + common_text


# Xenos alpha functions to the pass condition against RB_ALPHA_REF. A NaN
# alpha fails both, as the guest comparison does.
ALPHA_PASS = {4: ">", 6: ">="}


def pixel_state(recipe, words):
    rasterized_depth = recipe.get("rasterized_depth", False)
    if type(rasterized_depth) is not bool:
        raise ValueError("invalid rasterized depth setting")
    alpha_test = bool(words[0x2202] & 8)
    # The exercised gameplay cutouts use Greater (4) and GreaterEqual (6).
    # Other guest comparisons need their own verified shader specialization.
    if alpha_test and (not rasterized_depth or (words[0x2202] & 7) not in ALPHA_PASS):
        raise ValueError("unsupported shader alpha comparison")
    generated_pixel_parameter(words)
    return rasterized_depth, alpha_test


def generated_pixel_parameter(words):
    if not words[0x2180] & (1 << 18):
        return None
    parameter = (words[0x2181] >> 8) & 0xFF
    if parameter >= 16:
        raise ValueError("unsupported generated pixel parameter register")
    return parameter


def _signed_six(value):
    value &= 0x3F
    return value - 0x40 if value & 0x20 else value


def target_state(recipe, words):
    """Validate and return the host render-target specialization inputs.

    RB_SURFACE_INFO carries the guest MSAA mode while RB_COLOR_INFO carries
    the Xenos color format and signed six-bit exponent bias. These values are
    part of shader identity: accepting a different state would bind a shader
    whose coverage or output encoding is wrong for the draw.
    """
    guest_msaa = (words[0x2000] >> 16) & 3
    expected_samples = {0: (1, 1), 1: (4, 9)}
    if guest_msaa not in expected_samples:
        raise ValueError("unsupported shader sample state")
    sample_count, sample_mask = expected_samples[guest_msaa]
    for name, expected in (
        ("sample_count", sample_count),
        ("sample_mask", sample_mask),
    ):
        if name in recipe:
            value = recipe[name]
            if type(value) is not int or value != expected:
                raise ValueError("shader recipe sample specialization mismatch")

    color_info = words[0x2001]
    color_format = (color_info >> 16) & 0xF
    color_exp_bias = _signed_six(color_info >> 20)
    if color_format not in (0, 2, 4):
        raise ValueError("unsupported shader target format")
    if color_format == 4 and "color_format" not in recipe:
        raise ValueError("missing shader target format specialization")
    if color_format == 4 and "color_exp_bias" not in recipe:
        raise ValueError("missing shader color exponent specialization")
    requested_format = recipe.get("color_format", 0)
    requested_exp_bias = recipe.get("color_exp_bias", 0)
    if type(requested_format) is not int or requested_format != color_format:
        raise ValueError("shader recipe target format specialization mismatch")
    if type(requested_exp_bias) is not int or not -32 <= requested_exp_bias <= 31:
        raise ValueError("invalid shader color exponent specialization")
    if requested_exp_bias != color_exp_bias:
        raise ValueError("shader recipe color exponent specialization mismatch")
    if color_format in (0, 2) and color_exp_bias != 0:
        raise ValueError("unsupported shader color exponent bias")
    return sample_count, sample_mask, color_format, color_exp_bias


def color_targets(recipe, words):
    """Return how many render targets the pixel shader writes.

    A second target is RB_COLOR1_INFO (register 0x2003). Its format joins the
    shader identity as RT0's does; only formats whose output needs no scale
    (k_8_8_8_8 and k_2_10_10_10, exponent bias 0) are admitted for now.
    """
    count = recipe.get("color_targets", 1)
    if type(count) is not int or count not in (1, 2):
        raise ValueError("unsupported shader color target count")
    if count == 2:
        info = words[0x2003]
        if (info >> 16) & 0xF not in (0, 2) or _signed_six(info >> 20) != 0:
            raise ValueError("unsupported second shader target format")
    return count


def specialize_pixel_source(
    text,
    words,
    rasterized_depth,
    alpha_test,
    coverage_mask=9,
    color_format=0,
    color_exp_bias=0,
    targets=1,
):
    if coverage_mask not in (1, 9):
        raise ValueError("unsupported shader coverage mask")
    if (
        color_format not in (0, 2, 4)
        or not -32 <= color_exp_bias <= 31
        or (color_format in (0, 2) and color_exp_bias != 0)
    ):
        raise ValueError("unsupported shader target specialization")
    text = replace_once(text, "float4 c[224];", "float4 c[256];")
    if targets == 2:
        # The raw translation declares only SV_Target0 and keeps oC1 local.
        text = replace_once(
            text,
            "\tout float4 oC0 : SV_Target0,",
            "\tout float4 oC0 : SV_Target0,\n\tout float4 oC1 : SV_Target1,",
        )
        text = replace_once(text, "\tfloat4 oC1 = 0.0;", "\toC1 = 0.0;")
    spec = 2 if alpha_test else 0
    text = replace_once(
        text,
        "[[vk::constant_id(0)]] const uint g_SpecConstants = 0;",
        f"static const uint g_SpecConstants = {spec};",
    )
    text = replace_once(
        text, "uint g_SpecConstants();", f"uint g_SpecConstants() {{ return {spec}; }}"
    )
    if alpha_test:
        text = replace_once(
            text,
            "clip(oC0.w - g_AlphaThreshold);",
            f"if (!(oC0.w {ALPHA_PASS[words[0x2202] & 7]} g_AlphaThreshold)) discard;",
        )
    control, pattern = (words[0x2181] >> 2) & 3, words[0x2182] >> 16
    if control == 3:
        raise ValueError("unsupported interpolator sample control")
    for index in range(16):
        centroid = control == 0 or (control == 2 and not (pattern & (1 << index)))
        if centroid:
            text = replace_once(
                text,
                f"\tin float4 iTexCoord{index} : TEXCOORD{index},",
                f"\tcentroid in float4 iTexCoord{index} : TEXCOORD{index},",
            )
    parameter = generated_pixel_parameter(words)
    if parameter is not None:
        # SQ_PROGRAM_CNTL/SQ_CONTEXT_MISC replace this interpolator with the
        # integer pixel address. For the supported triangle draws, X carries
        # the face sign and ZW are zero (SDK DxbcShaderTranslator PS prologue).
        text = replace_once(
            text,
            f"\tfloat4 r{parameter} = iTexCoord{parameter};",
            f"\tfloat4 r{parameter} = float4(abs(floor(iPos.xy)), 0.0, 0.0);\n"
            f"\tr{parameter}.x = asfloat(asuint(r{parameter}.x) | "
            "(iFace ? 0u : 0x80000000u));",
        )
    # Two guest samples occupy native D3D sample positions zero and three.
    if rasterized_depth:
        # Raw translation always declares a depth export. Removing it is safe
        # only when the shader never reads or writes it beyond initialization.
        if len(re.findall(r"\boDepth\b", text)) != 2:
            raise ValueError("pixel shader has a guest depth export")
        text = replace_once(
            text,
            "\tout float oDepth : SV_Depth)",
            "\tout uint oCoverage : SV_Coverage)",
        )
        text = replace_once(text, "\toDepth = 0.0;", "\toCoverage = 0x9;")
    else:
        text = replace_once(
            text,
            "\tout float oDepth : SV_Depth)",
            "\tout float oDepth : SV_Depth,\n\tout uint oCoverage : SV_Coverage)",
        )
        text = replace_once(
            text,
            "\toDepth = 0.0;",
            f"\toDepth = 0.0;\n\toCoverage = 0x{coverage_mask:X};",
        )
    if rasterized_depth:
        text = replace_once(
            text, "\toCoverage = 0x9;", f"\toCoverage = 0x{coverage_mask:X};"
        )
    if color_format == 4:
        if text.count("return;") != 1:
            raise ValueError("pixel shader output scale requires one return")
        scale = f"\toC0.xy *= exp2({color_exp_bias}.0 - 5.0);\n\treturn;"
        text = text.replace("return;", scale, 1)
    return text


def screen_space_position(words):
    """Return whether the vertex shader outputs pretransformed pixel positions.

    With guest clipping off (PA_CL_CLIP_CNTL 0x90000) and only VTX_W0_FMT set
    in PA_CL_VTE_CNTL (0x400), the viewport transform is disabled and XY are
    pixel coordinates, so the translated vertex shader maps them to NDC.
    """
    return words[0x2206] == 0x400 and words[0x2204] == 0x90000


def user_clip_plane(words):
    """Return whether the vertex shader clips by user clip plane 0.

    PA_CL_CLIP_CNTL 0x80001 sets only UCP_ENA_0 with clipping on. The SDK
    then writes dot(guest position, PA_CL_UCP_0) to a clip distance before the
    host viewport scale (dxbc_translator.cpp:949-965); the runtime supplies the
    plane at shared constant c29. Other plane sets and cull-only planes are not
    modeled.
    """
    clip = words[0x2204]
    if clip & 0x3F and not clip & 0x10000 and clip != 0x80001:
        raise ValueError("unsupported user clip planes")
    return clip == 0x80001


def clip_user_plane(text):
    # The distance is appended after TEXCOORD15 so the pixel shader's input
    # signature keeps its registers.
    text = replace_once(
        text,
        "\tDEFINE_SHARED_CONSTANTS();\n",
        "\tDEFINE_SHARED_CONSTANTS();\n\tfloat4 g_UserClipPlane0 : packoffset(c29);\n",
    )
    text = replace_once(
        text,
        "\tout float4 oTexCoord15 : TEXCOORD15)",
        "\tout float4 oTexCoord15 : TEXCOORD15,\n\tout float oClipDistance0 : SV_ClipDistance0)",
    )
    return replace_once(
        text,
        "\toPos.xy += g_HalfPixelOffset * oPos.w;",
        "\toClipDistance0 = dot(oPos, g_UserClipPlane0);\n\toPos.xy += g_HalfPixelOffset * oPos.w;",
    )


def spirv_shared_header(text):
    # The raw adapter declares descriptor-backed constants, not BDA addresses.
    return replace_once(
        text, "#ifdef __spirv__", "#if 0 // Raw adapter uses uniform buffers."
    )


def specialize_spirv_vertex(text, shader_hash):
    if shader_hash != "C3BAB94E67553E12":
        return text
    # Match DXIL's world-Z product order without changing world X/Y.
    before = (
        "\t\tps = cos(r0.y);\n"
        "\t\tr0.z = ps;\n"
        "\t\tr3.x = r1.w * r0.z;\n"
        "\t\tps = sin(r0.y);\n"
        "\t\tr0.z = ps;\n"
        "\t\tr3.w = r0.z * -r1.w;\n"
        "\t\tps = sin(r0.x);\n"
        "\t\tr3.z = ps;\n"
        "\t\tr0.xyz = r3.wyx * r3.yzy;"
    )
    after = (
        "\t\tps = cos(r0.y);\n"
        "\t\tr0.z = ps;\n"
        "\t\tfloat worldZCosB = r0.z;\n"
        "\t\tr3.x = r3.y * r1.w;\n"
        "\t\tps = sin(r0.y);\n"
        "\t\tr0.z = ps;\n"
        "\t\tr3.w = r0.z * -r1.w;\n"
        "\t\tps = sin(r0.x);\n"
        "\t\tr3.z = ps;\n"
        "\t\tr0.xyz = r3.wyx * float3(r3.y, r3.z, worldZCosB);"
    )
    return replace_once(text, before, after)


def spirv_front_face(text):
    # The raw translation declares the face as uint, which D3D accepts. Vulkan
    # requires the FrontFacing built-in to be bool, so a pixel shader that
    # reads it (the generated pixel address) declares it as bool for SPIR-V.
    return replace_once(
        text, "\tin uint iFace : SV_IsFrontFace,", "\tin bool iFace : SV_IsFrontFace,"
    )


def spirv_source(text, stage):
    expected = {
        ("t0", "0"),
        ("t0", "1"),
        ("t0", "2"),
        ("s0", "3"),
        ("b0" if stage == "vs" else "b1", "4"),
        ("b2", "4"),
    }
    bindings = re.findall(r"register\((\w+), space(\d+)\)", text)
    if len(bindings) != len(expected) or set(bindings) != expected:
        raise ValueError("unsupported SPIR-V descriptor bindings")
    matches = list(re.finditer(r"void main\((.*?)\)\s*\{", text, re.DOTALL))
    if len(matches) != 1:
        raise ValueError("unexpected SPIR-V stage interface")
    match = matches[0]
    seen = {"in": set(), "out": set()}
    parameters = []
    for parameter in match[1].split(","):
        declaration = re.fullmatch(
            r"(\s*)(centroid\s+)?(in|out) (\w+) (\w+) : (\w+)(\s*)", parameter
        )
        if declaration is None:
            raise ValueError("unsupported SPIR-V stage interface")
        whitespace, centroid, direction, kind, name, semantic, trailing = (
            declaration.groups()
        )
        if semantic.startswith("SV_"):
            parameters.append(parameter)
            continue
        location = re.fullmatch(r"TEXCOORD(\d+)", semantic)
        if location is None or kind != "float4" or not 0 <= int(location[1]) < 16:
            raise ValueError("unsupported SPIR-V stage semantic")
        index = int(location[1])
        if index in seen[direction]:
            raise ValueError("duplicate SPIR-V stage location")
        seen[direction].add(index)
        parameters.append(
            f"{whitespace}[[vk::location({index})]] {centroid or ''}"
            f"{direction} {kind} {name} : {semantic}{trailing}"
        )
    if seen["out" if stage == "vs" else "in"] != set(range(16)):
        raise ValueError("mismatched SPIR-V interpolator interface")
    return text[: match.start(1)] + ",".join(parameters) + text[match.end(1) :]


def compiler_arguments(tools, stage, source, output, output_format="dxil"):
    if "dxc" in tools:
        arguments = [
            str(tools["dxc"]),
            "-T",
            f"{stage}_6_0",
            "-E",
            "main",
            "-HV",
            "2021",
            "-all-resources-bound",
            "-Wno-ignored-attributes",
            "-Qstrip_reflect",
            "-Qstrip_debug",
            "-DREBLUE_RECOMP",
            "-Fo",
            str(output),
            str(source),
        ]
        if output_format == "spirv":
            arguments.remove("-Qstrip_reflect")
            arguments.remove("-Qstrip_debug")
            arguments[1:1] = [
                "-spirv",
                "-fspv-target-env=vulkan1.2",
                "-fvk-use-dx-layout",
                "-fvk-invert-y" if stage == "vs" else "-fvk-use-dx-position-w",
            ]
        return arguments
    compiler = tools["vertex_compiler" if stage == "vs" else "pixel_compiler"]
    return [str(compiler), str(tools["dxcompiler"]), str(source), str(output)]


def run_logged(arguments, log):
    result = subprocess.run(arguments, capture_output=True, text=True)
    log.write_text(result.stdout + result.stderr, encoding="utf-8")
    result.check_returncode()


def compile_artifact(tools, stage, source, artifact, output_format, prefix):
    """Compile one stage; a SPIR-V module must also pass the pinned validator."""
    run_logged(
        compiler_arguments(tools, stage, source, artifact, output_format),
        artifact.with_name(f"{prefix}.compile.log"),
    )
    if output_format == "spirv":
        run_logged(
            [
                str(tools["spirv_val"]),
                "--target-env",
                "vulkan1.2",
                "--scalar-block-layout",
                str(artifact),
            ],
            artifact.with_name(f"{prefix}.validation.log"),
        )


def generate(recipe_path, check=False):
    recipe_path = recipe_path.resolve(strict=True)
    root = recipe_path.parent
    recipe = json.loads(recipe_path.read_text(encoding="ascii"))
    if recipe["schema_version"] != 1:
        raise ValueError("unsupported shader recipe schema")
    output_format = recipe.get("output_format", "dxil")
    if output_format not in ("dxil", "spirv", "dual"):
        raise ValueError("unsupported shader output format")
    if output_format != "dxil":
        if "dxc" not in recipe["tools"]:
            raise ValueError("SPIR-V requires standalone pinned dxc")
        if "spirv_val" not in recipe["tools"]:
            raise ValueError("missing pinned tool: spirv_val")
    depth_only = recipe.get("depth_only", False)
    if type(depth_only) is not bool:
        raise ValueError("invalid depth-only setting")
    if depth_only and output_format != "dual":
        raise ValueError("depth-only recipes require dual output")
    # A dual run compiles today's DXIL output unchanged, then derives the
    # SPIR-V variant of each stage into spirv/ from the same translation.
    primary_format = "spirv" if output_format == "spirv" else "dxil"
    tools = {name: pinned(root, item) for name, item in recipe["tools"].items()}
    compiler_tools = (
        ("dxc",) if "dxc" in tools else ("vertex_compiler", "pixel_compiler")
    )
    for name in ("adapter", "dxcompiler", "dxil", *compiler_tools):
        if name not in tools:
            raise ValueError(f"missing pinned tool: {name}")
    # The compilers load this validator beside dxcompiler.dll.
    if tools["dxil"] != tools["dxcompiler"].with_name("dxil.dll"):
        raise ValueError("DXIL validator must be beside dxcompiler.dll")
    if "dxc" in tools:
        if "vertex_compiler" in tools or "pixel_compiler" in tools:
            raise ValueError("shader recipe must select one compiler interface")
        if tools["dxcompiler"] != tools["dxc"].with_name("dxcompiler.dll"):
            raise ValueError("DXC executable must be beside the pinned dxcompiler.dll")
    registers = pinned(root, recipe["registers"])
    common = pinned(root, recipe["shared_header"])
    output = (root / recipe["output"]).resolve()
    if output.exists():
        raise ValueError(f"shader output must be a fresh directory: {output}")
    slots, independent_samplers, dimensions = texture_layout(recipe)
    inputs = []
    for shader in recipe["shaders"]:
        if shader["stage"] not in ("vs", "ps") or not re.fullmatch(
            r"[0-9A-F]{16}", shader["hash"]
        ):
            raise ValueError("invalid shader stage or hash")
        path = pinned(root, shader["microcode"])
        if (
            path.stat().st_size == 0
            or path.stat().st_size > 65536
            or path.stat().st_size % 4
        ):
            raise ValueError("invalid shader microcode size")
        if shader["byte_order"] not in ("little", "big"):
            raise ValueError("invalid shader word order")
        inputs.append((shader, path))
    if depth_only:
        if len(inputs) != 1 or inputs[0][0]["stage"] != "vs":
            raise ValueError("depth-only recipe requires one vertex shader")
    elif len(inputs) != 2 or {item[0]["stage"] for item in inputs} != {"vs", "ps"}:
        raise ValueError("shader recipe requires one vertex/pixel pair")
    register_bytes = registers.read_bytes()
    if len(register_bytes) != 0x5003 * 4:
        raise ValueError("invalid captured register extent")
    words = struct.unpack("<20483I", register_bytes)
    if depth_only:
        # No pixel shader runs, so no target or alpha state specializes one.
        rasterized_depth = recipe.get("rasterized_depth", False)
        if type(rasterized_depth) is not bool:
            raise ValueError("invalid rasterized depth setting")
        # depth-only.toml entries carry no position specialization flags.
        if screen_space_position(words) or user_clip_plane(words):
            raise ValueError("depth-only recipes require guest clipping and viewport")
    else:
        sample_count, sample_mask, color_format, color_exp_bias = target_state(
            recipe, words
        )
        rasterized_depth, alpha_test = pixel_state(recipe, words)
        targets = color_targets(recipe, words)
    if output_format != "dxil":
        spirv_common_text = spirv_shared_header(common.read_text(encoding="ascii"))
    # Validate the raw vertex clauses before creating output, including in
    # check-only runs. A pinned digest alone cannot validate recipe byte order.
    if rasterized_depth:
        for shader, source_path in inputs:
            if shader["stage"] == "vs":
                raw = source_path.read_bytes()
                values = struct.unpack(
                    (">" if shader["byte_order"] == "big" else "<")
                    + f"{len(raw) // 4}I",
                    raw,
                )
                vertex_texture_header(common.read_text(encoding="ascii"), values)
    if check:
        print("native shader inputs: PASS")
        return
    output.mkdir(parents=True)
    common_text = (
        spirv_common_text
        if output_format == "spirv"
        else common.read_text(encoding="ascii")
    )
    shared = output / "shader_common.h"
    shared.write_text(
        render_shared_header(common_text, slots, independent_samplers, dimensions),
        encoding="ascii",
    )
    if output_format == "dual":
        spirv_output = output / "spirv"
        spirv_output.mkdir()
        spirv_shared = spirv_output / "shader_common.h"
        spirv_shared.write_text(
            render_shared_header(
                spirv_common_text,
                slots,
                independent_samplers,
                dimensions,
            ),
            encoding="ascii",
        )

    def executable_text(text, stage, screen_space, user_clip):
        """Apply the format-independent edits to one translated stage."""
        if stage == "vs" and screen_space:
            # Disabled guest clipping/viewport treats XY as pixel coordinates.
            # Map those to the native full viewport before the half-pixel shift.
            text = replace_once(
                text,
                "\toPos.xy += g_HalfPixelOffset * oPos.w;",
                "\toPos.xy = oPos.xy * float2(1.0 / 640.0, -1.0 / 360.0) + float2(-1.0, 1.0) * oPos.w;\n"
                "\toPos.xy += g_HalfPixelOffset * oPos.w;",
            )
        if stage == "vs" and user_clip:
            text = clip_user_plane(text)
        if stage == "ps":
            text = specialize_pixel_source(
                text,
                words,
                rasterized_depth,
                alpha_test,
                sample_mask,
                color_format,
                color_exp_bias,
                targets,
            )
        return text

    records = []
    for shader, source_path in inputs:
        stage, shader_hash = shader["stage"], shader["hash"]
        prefix = f"{stage}_{shader_hash}"
        raw = source_path.read_bytes()
        values = struct.unpack(
            (">" if shader["byte_order"] == "big" else "<") + f"{len(raw) // 4}I", raw
        )
        normalized = output / f"{prefix}.ucode.le.bin"
        normalized.write_bytes(struct.pack(f"<{len(values)}I", *values))
        stage_shared = shared
        if stage == "vs" and rasterized_depth:
            stage_shared = output / "shader_common_vs.h"
            stage_shared.write_text(
                vertex_texture_header(shared.read_text(encoding="ascii"), values),
                encoding="ascii",
            )
        source = output / f"{prefix}.hlsl"
        run_logged(
            [
                str(tools["adapter"]),
                "--raw",
                stage,
                str(normalized),
                str(registers),
                str(source),
                str(stage_shared),
                f"0x{shader_hash}",
            ],
            output / f"{prefix}.translation.log",
        )
        screen_space = stage == "vs" and screen_space_position(words)
        user_clip = stage == "vs" and user_clip_plane(words)
        executable_source = source
        if screen_space or user_clip or stage == "ps":
            text = executable_text(
                source.read_text(encoding="ascii"), stage, screen_space, user_clip
            )
            executable_source = output / f"{prefix}.executable.hlsl"
            executable_source.write_text(text, encoding="ascii")
        if primary_format == "spirv":
            text = executable_source.read_text(encoding="ascii")
            if stage == "vs":
                text = specialize_spirv_vertex(text, shader_hash)
            elif generated_pixel_parameter(words) is not None:
                text = spirv_front_face(text)
            text = spirv_source(text, stage)
            executable_source = output / f"{prefix}.executable.hlsl"
            executable_source.write_text(text, encoding="ascii")
        artifact = output / f"{prefix}.{'spv' if primary_format == 'spirv' else 'dxil'}"
        compile_artifact(
            tools, stage, executable_source, artifact, primary_format, prefix
        )
        record = {
            "stage": stage,
            "hash": shader_hash,
            "microcode_sha256": digest(normalized),
            f"{primary_format}_sha256": digest(artifact),
        }
        if output_format == "dual":
            spirv_stage_shared = spirv_shared
            if stage == "vs" and rasterized_depth:
                spirv_stage_shared = spirv_output / "shader_common_vs.h"
                spirv_stage_shared.write_text(
                    vertex_texture_header(
                        spirv_shared.read_text(encoding="ascii"), values
                    ),
                    encoding="ascii",
                )
            # The raw adapter inlines its stage header ahead of the translation.
            # Swapping in the SPIR-V header equals translating with it, so each
            # stage is translated once and the DXIL files stay untouched.
            translated = source.read_bytes()
            header = stage_shared.read_bytes()
            if not translated.startswith(header):
                raise ValueError("translated source does not begin with its header")
            spirv_raw = spirv_output / f"{prefix}.hlsl"
            spirv_raw.write_bytes(
                spirv_stage_shared.read_bytes() + translated[len(header) :]
            )
            text = executable_text(
                spirv_raw.read_text(encoding="ascii"), stage, screen_space, user_clip
            )
            if stage == "vs":
                text = specialize_spirv_vertex(text, shader_hash)
            elif generated_pixel_parameter(words) is not None:
                text = spirv_front_face(text)
            spirv_executable = spirv_output / f"{prefix}.executable.hlsl"
            spirv_executable.write_text(spirv_source(text, stage), encoding="ascii")
            spirv_artifact = spirv_output / f"{prefix}.spv"
            compile_artifact(
                tools, stage, spirv_executable, spirv_artifact, "spirv", prefix
            )
            record["spirv_sha256"] = digest(spirv_artifact)
        # The catalog entry for this pair carries screen_space_position = true.
        # The key is absent otherwise, so other records keep their bytes.
        if screen_space:
            record["screen_space_position"] = True
        # Set user_clip_plane = true only for a VS that clips against plane 0.
        if user_clip:
            record["user_clip_plane"] = True
        records.append(record)
    if output_format != "dxil":
        # DXC maps SV_ClipDistance0 to the Vulkan ClipDistance built-in, which
        # the consuming device must enable.
        required_features = ["scalarBlockLayout"]
        if any(record.get("user_clip_plane") for record in records):
            required_features.append("shaderClipDistance")
        spirv_metadata = {
            "target_env": "vulkan1.2",
            "required_features": required_features,
            "spirv_val_sha256": digest(tools["spirv_val"]),
        }
    metadata = (
        {"output_format": "spirv", **spirv_metadata} if output_format == "spirv" else {}
    )
    generation = {
        **metadata,
        "recipe": str(recipe_path),
        "generated_pixel_parameters": bool(words[0x2180] & (1 << 18)),
        "shaders": records,
    }
    if depth_only:
        generation["depth_only"] = True
    if output_format == "dual":
        generation["spirv"] = spirv_metadata
    (
        output
        / ("generation.spirv.json" if output_format == "spirv" else "generation.json")
    ).write_text(json.dumps(generation, indent=2) + "\n", encoding="ascii")
    print(json.dumps(records, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recipe", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    generate(args.recipe, args.check)
