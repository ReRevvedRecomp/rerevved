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


def generate(recipe_path, check=False):
    recipe_path = recipe_path.resolve(strict=True)
    root = recipe_path.parent
    recipe = json.loads(recipe_path.read_text(encoding="ascii"))
    if recipe["schema_version"] != 1:
        raise ValueError("unsupported shader recipe schema")
    tools = {name: pinned(root, item) for name, item in recipe["tools"].items()}
    for name in ("adapter", "vertex_compiler", "pixel_compiler", "dxcompiler", "dxil"):
        if name not in tools:
            raise ValueError(f"missing pinned tool: {name}")
    # The compilers load this validator beside dxcompiler.dll.
    if tools["dxil"] != tools["dxcompiler"].with_name("dxil.dll"):
        raise ValueError("DXIL validator must be beside dxcompiler.dll")
    registers = pinned(root, recipe["registers"])
    common = pinned(root, recipe["shared_header"])
    output = (root / recipe["output"]).resolve()
    if output.exists():
        raise ValueError(f"shader output must be a fresh directory: {output}")
    slots = recipe["texture_fetches"]
    if slots not in ([0], [2, 0, 1]):
        raise ValueError("unsupported shader texture interface")
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
    if len(inputs) != 2 or {item[0]["stage"] for item in inputs} != {"vs", "ps"}:
        raise ValueError("shader recipe requires one vertex/pixel pair")
    register_bytes = registers.read_bytes()
    if len(register_bytes) != 0x5003 * 4:
        raise ValueError("invalid captured register extent")
    words = struct.unpack("<20483I", register_bytes)
    if ((words[0x2000] >> 16) & 3) != 1 or words[0x2202] & 8:
        raise ValueError(
            "shader recipe requires two-sample inputs without alpha testing"
        )
    if check:
        print("native shader inputs: PASS")
        return
    output.mkdir(parents=True)
    common_text = common.read_text(encoding="ascii")
    common_text = replace_once(
        common_text,
        "g_Texture2DDescriptorHeap[1]",
        f"g_Texture2DDescriptorHeap[{len(slots)}]",
    )
    bindings = "".join(
        f"#define s{fetch}_Texture2DDescriptorIndex {slot}\n#define s{fetch}_SamplerDescriptorIndex 0\n"
        for slot, fetch in enumerate(slots)
    )
    shared = output / "shader_common.h"
    shared.write_text(bindings + common_text, encoding="ascii")
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
        source = output / f"{prefix}.hlsl"
        result = subprocess.run(
            [
                str(tools["adapter"]),
                "--raw",
                stage,
                str(normalized),
                str(registers),
                str(source),
                str(shared),
                f"0x{shader_hash}",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        (output / f"{prefix}.translation.log").write_text(
            result.stdout + result.stderr, encoding="utf-8"
        )
        executable_source = source
        if stage == "vs" and words[0x2206] == 0x400 and words[0x2204] == 0x90000:
            # Disabled guest clipping/viewport treats XY as pixel coordinates.
            # Map those to the native full viewport before the half-pixel shift.
            text = replace_once(
                source.read_text(encoding="ascii"),
                "\toPos.xy += g_HalfPixelOffset * oPos.w;",
                "\toPos.xy = oPos.xy * float2(1.0 / 640.0, -1.0 / 360.0) + float2(-1.0, 1.0) * oPos.w;\n"
                "\toPos.xy += g_HalfPixelOffset * oPos.w;",
            )
            executable_source = output / f"{prefix}.executable.hlsl"
            executable_source.write_text(text, encoding="ascii")
        if stage == "ps":
            text = source.read_text(encoding="ascii")
            # Raw pixel shaders can reference resource literals above c223.
            text = replace_once(text, "float4 c[224];", "float4 c[256];")
            text = replace_once(
                text,
                "[[vk::constant_id(0)]] const uint g_SpecConstants = 0;",
                "static const uint g_SpecConstants = 0;",
            )
            text = replace_once(
                text, "uint g_SpecConstants();", "uint g_SpecConstants() { return 0; }"
            )
            control, pattern = (words[0x2181] >> 2) & 3, words[0x2182] >> 16
            if control == 3:
                raise ValueError("unsupported interpolator sample control")
            for index in range(16):
                centroid = control == 0 or (
                    control == 2 and not (pattern & (1 << index))
                )
                if centroid:
                    text = replace_once(
                        text,
                        f"\tin float4 iTexCoord{index} : TEXCOORD{index},",
                        f"\tcentroid in float4 iTexCoord{index} : TEXCOORD{index},",
                    )
            # Two guest samples occupy native D3D sample positions zero and three.
            text = replace_once(
                text,
                "\tout float oDepth : SV_Depth)",
                "\tout float oDepth : SV_Depth,\n\tout uint oCoverage : SV_Coverage)",
            )
            text = replace_once(
                text, "\toDepth = 0.0;", "\toDepth = 0.0;\n\toCoverage = 0x9;"
            )
            executable_source = output / f"{prefix}.executable.hlsl"
            executable_source.write_text(text, encoding="ascii")
        dxil = output / f"{prefix}.dxil"
        compiler = tools["vertex_compiler" if stage == "vs" else "pixel_compiler"]
        result = subprocess.run(
            [
                str(compiler),
                str(tools["dxcompiler"]),
                str(executable_source),
                str(dxil),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        (output / f"{prefix}.compile.log").write_text(
            result.stdout + result.stderr, encoding="utf-8"
        )
        records.append(
            {
                "stage": stage,
                "hash": shader_hash,
                "microcode_sha256": digest(normalized),
                "source_sha256": digest(source),
                "executable_source_sha256": digest(executable_source),
                "dxil_sha256": digest(dxil),
            }
        )
    (output / "generation.json").write_text(
        json.dumps(
            {
                "recipe": str(recipe_path),
                "recipe_sha256": digest(recipe_path),
                "shared_header_sha256": digest(shared),
                "shaders": records,
            },
            indent=2,
        )
        + "\n",
        encoding="ascii",
    )
    print(json.dumps(records, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recipe", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    generate(args.recipe, args.check)
