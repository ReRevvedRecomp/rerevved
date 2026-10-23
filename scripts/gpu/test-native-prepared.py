"""Prepare and run bounded native Prepared and pixel register-LOD proofs."""

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import tomllib
from pathlib import Path

REGISTER_COUNT = 0x5003
VERTEX_HASH = "D1A5A8B7C6E50123"
PIXEL_HASH = "E2B6B9C8D7F61234"

ATTRIBUTES = (
    {
        "fetch": 95,
        "stride_bytes": 56,
        "offset_bytes": 0,
        "format": 57,
        "signed": True,
        "normalized": False,
    },
    {
        "fetch": 95,
        "stride_bytes": 56,
        "offset_bytes": 16,
        "format": 38,
        "signed": True,
        "normalized": False,
    },
)

CONTROL_VERTEX = """struct VSInput {
    float4 position : TEXCOORD0;
    float4 lod : TEXCOORD1;
};

struct VSOutput {
    float4 position : SV_Position;
    float4 lod : TEXCOORD0;
};

VSOutput main(VSInput input) {
    VSOutput output;
    output.position = float4(input.position.xyz, 1.0);
    output.lod = input.lod;
    return output;
}
"""

CONTROL_PIXEL = """Texture2D<float4> sourceTexture : register(t0);
SamplerState sourceSampler : register(s0, space3);

struct PSInput {
    float4 position : SV_Position;
    float4 lod : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0 {
    float4 first = sourceTexture.SampleLevel(sourceSampler, input.lod.xy, input.lod.z);
    float4 second = sourceTexture.SampleLevel(sourceSampler, input.lod.xy, input.lod.w);
    return first + second;
}
"""

FAMILIES = {
    "native-pixel-lod": {
        "metadata": {
            "mip_levels": 8,
            "width": 128,
            "height": 128,
        },
        "fixed_uv": True,
        "schema_error": (
            "prepared fixture is outside the bounded eight-level 128x128 schema"
        ),
        "success": "native pixel LOD prepared inputs: PASS",
    },
    "native-prepared-bc1": {
        "metadata": {
            "mip_levels": 8,
            "width": 128,
            "height": 128,
            "output_width": 1280,
            "output_height": 720,
            "format": "k_DXT1",
            "endianness": "k8in16",
            "swizzle": 0x688,
            "pitch": 4,
            "packed_mips": True,
            "packed_tail_offset": 0x4000,
        },
        "fixed_uv": True,
        "schema_error": "prepared fixture is outside the bounded packed BC1 schema",
        "success": "native prepared BC1 prepared inputs: PASS",
    },
    "native-prepared-bc1-256": {
        "metadata": {
            "mip_levels": 9,
            "width": 256,
            "height": 256,
            "output_width": 1280,
            "output_height": 720,
            "viewport_width": 256.0,
            "viewport_height": 256.0,
            "viewport_registers": (128.0, 128.0, -128.0, 128.0),
            "triangle_uvs": (
                (0.25 / 256, 1.0 + 0.25 / 256),
                (0.25 / 256, -1.0 + 0.25 / 256),
                (2.0 + 0.25 / 256, 1.0 + 0.25 / 256),
            ),
            "format": "k_DXT1",
            "endianness": "k8in16",
            "swizzle": 0x688,
            "raw_swizzle": 0x688,
            "selected_raw_fetch_words": (
                "02000002",
                "19FE9052",
                "001FE0FF",
                "00A80D10",
                "00000203",
                "19FE1A18",
            ),
            "sampler_filter": "point",
            "address_mode": "clamp_to_edge",
            "spatial_sampling": "nearest_clamp",
            "sampler_min_lod": 0.0,
            "sampler_max_lod": 8.0,
            "pitch": 8,
            "packed_mips": True,
            "stacked": False,
            "packed_level": 4,
            "packed_tail_offset": 0x6000,
            "packed_tail_extent": 0x340,
            "base_extent": 0x8000,
            "mip_extent": 0x6340,
            "legacy_mip_extent": 0x6000,
            "guest_offsets": (
                0x0000,
                0x0000,
                0x2000,
                0x4000,
                0x6020,
                0x6010,
                0x6008,
                0x6200,
                0x6100,
            ),
            "row_pitches": (
                0x0200,
                0x0100,
                0x0100,
                0x0100,
                0x0100,
                0x0100,
                0x0100,
                0x0100,
                0x0100,
            ),
            "presenter_alpha": 255,
            "duplicated_rt0": True,
            "synthetic_canvas": True,
        },
        "fixed_uv": False,
        "schema_error": "prepared fixture is outside the bounded packed BC1 schema",
        "success": "native prepared BC1 prepared inputs: PASS",
        "output_file_count": 36,
    },
    "native-prepared-bc1-512": {
        "metadata": {
            "mip_levels": 10,
            "width": 512,
            "height": 512,
            "output_width": 1280,
            "output_height": 720,
            "viewport_width": 512.0,
            "viewport_height": 512.0,
            "viewport_registers": (256.0, 256.0, -256.0, 256.0),
            "triangle_uvs": (
                (0.25 / 512, 1.0 + 0.25 / 512),
                (0.25 / 512, -1.0 + 0.25 / 512),
                (2.0 + 0.25 / 512, 1.0 + 0.25 / 512),
            ),
            "format": "k_DXT1",
            "endianness": "k8in16",
            "swizzle": 0x688,
            "raw_swizzle": 0x688,
            "selected_raw_fetch_words": (
                "04000002",
                "19F61052",
                "003FE1FF",
                "00A80D10",
                "00000243",
                "19F51A18",
            ),
            "sampler_filter": "point",
            "address_mode": "clamp_to_edge",
            "spatial_sampling": "nearest_clamp",
            "sampler_min_lod": 0.0,
            "sampler_max_lod": 9.0,
            "pitch": 16,
            "packed_mips": True,
            "stacked": False,
            "packed_level": 5,
            "packed_tail_offset": 0xE000,
            "packed_tail_extent": 0x340,
            "base_extent": 0x20000,
            "mip_extent": 0xE340,
            "legacy_mip_extent": 0xE000,
            "guest_offsets": (
                0x0000,
                0x0000,
                0x8000,
                0xA000,
                0xC000,
                0xE020,
                0xE010,
                0xE008,
                0xE200,
                0xE100,
            ),
            "row_pitches": (
                0x0400,
                0x0200,
                0x0100,
                0x0100,
                0x0100,
                0x0100,
                0x0100,
                0x0100,
                0x0100,
                0x0100,
            ),
            "presenter_alpha": 255,
            "duplicated_rt0": True,
            "synthetic_canvas": True,
            "caller_frame": 800,
            "caller_texture": 5,
            "first_draw": 988677,
            "required_uses": 18,
            "raw_base_address": 0x19F61000,
            "raw_mip_address": 0x19F51000,
        },
        "fixed_uv": False,
        "schema_error": "prepared fixture is outside the bounded packed BC1 schema",
        "success": "native prepared BC1 prepared inputs: PASS",
        "output_file_count": 40,
    },
    "native-prepared-bc1-64": {
        "metadata": {
            "mip_levels": 7,
            "width": 64,
            "height": 64,
            "output_width": 1280,
            "output_height": 720,
            "viewport_width": 64.0,
            "viewport_height": 64.0,
            "viewport_registers": (32.0, 32.0, -32.0, 32.0),
            "triangle_uvs": (
                (0.25 / 64, 1.0 + 0.25 / 64),
                (0.25 / 64, -1.0 + 0.25 / 64),
                (2.0 + 0.25 / 64, 1.0 + 0.25 / 64),
            ),
            "format": "k_DXT1",
            "endianness": "k8in16",
            "swizzle": 0x688,
            "raw_swizzle": 0x688,
            "selected_raw_fetch_words": (
                "01000002",
                "1BC73052",
                "0007E03F",
                "00A80D10",
                "00000183",
                "1BC6FA18",
            ),
            "sampler_filter": "point",
            "address_mode": "clamp_to_edge",
            "spatial_sampling": "nearest_clamp",
            "sampler_min_lod": 0.0,
            "sampler_max_lod": 6.0,
            "pitch": 4,
            "packed_mips": True,
            "stacked": False,
            "packed_level": 2,
            "packed_tail_offset": 0x2000,
            "packed_tail_extent": 0x340,
            "base_extent": 0x1000,
            "mip_extent": 0x2340,
            "legacy_mip_extent": 0x2000,
            "guest_offsets": (0x0000, 0x0000, 0x2020, 0x2010, 0x2008, 0x2200, 0x2100),
            "row_pitches": (0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100),
            "presenter_alpha": 255,
            "duplicated_rt0": True,
            "synthetic_canvas": True,
            "caller_frame": 800,
            "caller_texture": 21,
            "first_draw": 988701,
            "required_uses": 104,
            "raw_base_address": 0x1BC73000,
            "raw_mip_address": 0x1BC6F000,
            "source_guest_frame": 1541,
            "source_submission": 5538,
            "source_ps_binding": 20,
            "source_ps_fetch": 1,
            "source_vertex_shader_hash": "571D6127CBF96DD1",
            "source_pixel_shader_hash": "2DEEA7E9B28316E1",
        },
        "fixed_uv": False,
        "schema_error": "prepared fixture is outside the bounded packed BC1 schema",
        "success": "native prepared BC1 prepared inputs: PASS",
        "output_file_count": 28,
    },
    "native-prepared-bc1-rect": {
        "metadata": {
            "mip_levels": 9,
            "width": 256,
            "height": 128,
            "output_width": 1280,
            "output_height": 720,
            "viewport_width": 256.0,
            "viewport_height": 128.0,
            "viewport_registers": (128.0, 128.0, -64.0, 64.0),
            "triangle_uvs": (
                (0.25 / 256, 1.0 + 0.25 / 128),
                (0.25 / 256, -1.0 + 0.25 / 128),
                (2.0 + 0.25 / 256, 1.0 + 0.25 / 128),
            ),
            "format": "k_DXT1",
            "endianness": "k8in16",
            "swizzle": 0x688,
            "raw_swizzle": 0x688,
            "selected_raw_fetch_words": (
                "02000002",
                "169F8052",
                "000FE0FF",
                "00A80D10",
                "00000203",
                "169F2A18",
            ),
            "sampler_filter": "point",
            "address_mode": "clamp_to_edge",
            "spatial_sampling": "nearest_clamp",
            "sampler_min_lod": 0.0,
            "sampler_max_lod": 8.0,
            "pitch": 8,
            "packed_mips": True,
            "stacked": False,
            "packed_level": 3,
            "packed_tail_offset": 0x4000,
            "packed_tail_extent": 0x740,
            "base_extent": 0x4000,
            "mip_extent": 0x4740,
            "legacy_mip_extent": 0x4000,
            "guest_offsets": (
                0x0000,
                0x0000,
                0x2000,
                0x4400,
                0x4200,
                0x4100,
                0x4020,
                0x4010,
                0x4008,
            ),
            "row_pitches": (
                0x0200,
                0x0100,
                0x0100,
                0x0100,
                0x0100,
                0x0100,
                0x0100,
                0x0100,
                0x0100,
            ),
            "presenter_alpha": 255,
            "duplicated_rt0": True,
            "synthetic_canvas": True,
            "caller_frame": 800,
            "caller_texture": 6,
            "first_draw": 988683,
            "required_uses": 3,
            "raw_base_address": 0x169F8000,
            "raw_mip_address": 0x169F2000,
        },
        "fixed_uv": False,
        "schema_error": "prepared fixture is outside the bounded packed BC1 schema",
        "success": "native prepared BC1 prepared inputs: PASS",
        "output_file_count": 36,
    },
    "native-prepared-bc2": {
        "metadata": {
            "mip_levels": 7,
            "width": 64,
            "height": 64,
            "output_width": 1280,
            "output_height": 720,
            "format": "k_DXT2_3",
            "endianness": "k8in16",
            "swizzle": 0x688,
            "pitch": 4,
            "packed_mips": True,
            "packed_tail_offset": 0x4000,
            "base_extent": 0x2000,
            "mip_extent": 0x4680,
            "packed_tail_extent": 0x680,
            "guest_offsets": (0x0000, 0x0000, 0x4040, 0x4020, 0x4010, 0x4400, 0x4200),
            "row_pitch": 0x0200,
        },
        "fixed_uv": False,
        "schema_error": "prepared fixture is outside the bounded packed BC2 schema",
        "success": "native prepared BC2 prepared inputs: PASS",
    },
    "native-prepared-bc2-128": {
        "metadata": {
            "mip_levels": 8,
            "width": 128,
            "height": 128,
            "output_width": 1280,
            "output_height": 720,
            "presenter_alpha": 255,
            "duplicated_rt0": True,
            "synthetic_canvas": True,
            "format": "k_DXT2_3",
            "endianness": "k8in16",
            "swizzle": 0x688,
            "raw_swizzle": 1672,
            "selected_raw_fetch_words": (
                "01000002",
                "15AA7053",
                "000FE07F",
                "00A80D10",
                "000001C3",
                "15A9BA18",
            ),
            "sampler_filter": "point",
            "address_mode": "clamp_to_edge",
            "spatial_sampling": "nearest_clamp",
            "sampler_min_lod": 0.0,
            "sampler_max_lod": 7.0,
            "pitch": 4,
            "packed_mips": True,
            "stacked": False,
            "packed_tail_offset": 0x8000,
            "base_extent": 0x4000,
            "mip_extent": 0x8680,
            "packed_tail_extent": 0x680,
            "guest_offsets": (
                0x0000,
                0x0000,
                0x4000,
                0x8040,
                0x8020,
                0x8010,
                0x8400,
                0x8200,
            ),
            "row_pitch": 0x0200,
        },
        "fixed_uv": False,
        "schema_error": (
            "prepared fixture is outside the bounded packed BC2 128x128 schema"
        ),
        "success": "native prepared 128x128 BC2 prepared inputs: PASS",
    },
    "native-prepared-bc2-rect": {
        "metadata": {
            "mip_levels": 9,
            "width": 128,
            "height": 256,
            "output_width": 1280,
            "output_height": 720,
            "presenter_alpha": 255,
            "duplicated_rt0": True,
            "synthetic_canvas": True,
            "format": "k_DXT2_3",
            "endianness": "k8in16",
            "swizzle": 0x688,
            "raw_swizzle": 1672,
            "selected_raw_fetch_words": (
                "01000002",
                "168B6053",
                "001FE07F",
                "00A80D10",
                "00000203",
                "168AAA18",
            ),
            "sampler_filter": "point",
            "address_mode": "clamp_to_edge",
            "spatial_sampling": "nearest_clamp",
            "sampler_min_lod": 0.0,
            "sampler_max_lod": 8.0,
            "pitch": 4,
            "packed_mips": True,
            "stacked": False,
            "packed_tail_offset": 0x8000,
            "base_extent": 0x8000,
            "mip_extent": 0x8E80,
            "packed_tail_extent": 0xE80,
            "guest_offsets": (
                0x0000,
                0x0000,
                0x4000,
                0x8040,
                0x8020,
                0x8010,
                0x8800,
                0x8400,
                0x8200,
            ),
            "row_pitch": 0x0200,
        },
        "fixed_uv": False,
        "schema_error": "prepared fixture is outside the bounded packed BC2 schema",
        "success": "native prepared rectangular BC2 prepared inputs: PASS",
    },
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def quote(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def fresh_directory(path: Path) -> Path:
    path = path.resolve()
    if path.exists():
        raise ValueError(f"output directory must be fresh: {path}")
    path.mkdir(parents=True)
    return path


def require_file(path: Path, label: str) -> Path:
    path = path.resolve(strict=True)
    if not path.is_file():
        raise ValueError(f"{label} is not a file: {path}")
    return path


def require_directory(path: Path, label: str) -> Path:
    path = path.resolve(strict=True)
    if not path.is_dir():
        raise ValueError(f"{label} is not a directory: {path}")
    return path


def run_command(arguments: list[str], cwd: Path | None = None) -> None:
    subprocess.run(arguments, cwd=cwd, check=True)


def copy_input(source: Path, destination: Path, name: str) -> str:
    target = destination / name
    if target.exists():
        raise ValueError(f"duplicate prepared input: {target}")
    shutil.copyfile(source, target)
    return name


def write_catalog(
    path: Path, interface: dict, vertex_file: str, pixel_file: str
) -> None:
    lines = [
        "schema_version = 1",
        "",
        "[[shaders]]",
        f"vertex_hash = {quote(VERTEX_HASH)}",
        f"pixel_hash = {quote(PIXEL_HASH)}",
        f"vertex_word_sha256 = {quote(interface['vertex_word_sha256'])}",
        f"pixel_word_sha256 = {quote(interface['pixel_word_sha256'])}",
        f"vertex_dxil_sha256 = {quote(interface['vertex_dxil_sha256'])}",
        f"pixel_dxil_sha256 = {quote(interface['pixel_dxil_sha256'])}",
        f"vertex_file = {quote(vertex_file)}",
        f"pixel_file = {quote(pixel_file)}",
        "alpha_test = false",
        "alpha_compare = 0",
        "rasterized_depth = true",
        "independent_samplers = true",
        "program = 268500994",
        "context = 4",
        "interpolators = 1",
        "generated_pixel_parameters = false",
        "sample_count = 1",
        "sample_mask = 1",
        "color_format = 0",
        "color_exp_bias = 0",
        "texture_fetches = [0]",
        'texture_dimensions = ["2d"]',
    ]
    for attribute in ATTRIBUTES:
        lines.extend(
            [
                "",
                "[[shaders.attributes]]",
                f"fetch = {attribute['fetch']}",
                f"stride_bytes = {attribute['stride_bytes']}",
                f"offset_bytes = {attribute['offset_bytes']}",
                f"format = {attribute['format']}",
                f"signed = {str(attribute['signed']).lower()}",
                f"normalized = {str(attribute['normalized']).lower()}",
            ]
        )
    path.write_text("\n".join(lines) + "\n", encoding="ascii", newline="")


def pinned_input(root: Path, file_name: str) -> dict[str, str]:
    path = Path(file_name)
    resolved = (
        path.resolve(strict=True)
        if path.is_absolute()
        else (root / path).resolve(strict=True)
    )
    return {"file": file_name, "sha256": digest(resolved)}


def write_recipe(output: Path, paths: dict[str, str], tools: dict[str, Path]) -> Path:
    recipe = {
        "schema_version": 1,
        "tools": {
            key: {"file": str(value), "sha256": digest(value)}
            for key, value in tools.items()
        },
        "registers": pinned_input(output, paths["registers"]),
        "shared_header": pinned_input(output, paths["shared_header"]),
        "output": "candidate",
        "texture_fetches": [0],
        "independent_samplers": True,
        "texture_dimensions": ["2d"],
        "sample_count": 1,
        "sample_mask": 1,
        "color_format": 0,
        "color_exp_bias": 0,
        "rasterized_depth": True,
        "shaders": [
            {
                "stage": "vs",
                "hash": VERTEX_HASH,
                "microcode": pinned_input(output, paths["vertex_microcode"]),
                "byte_order": "little",
            },
            {
                "stage": "ps",
                "hash": PIXEL_HASH,
                "microcode": pinned_input(output, paths["pixel_microcode"]),
                "byte_order": "little",
            },
        ],
    }
    recipe_path = output / "shader-recipe.json"
    recipe_path.write_text(json.dumps(recipe, indent=2) + "\n", encoding="ascii")
    return recipe_path


def compile_control_pair(output: Path, dxc: Path) -> tuple[Path, Path, Path, Path]:
    source_root = output / "control-source"
    source_root.mkdir()
    vertex_source = source_root / "vertex.hlsl"
    pixel_source = source_root / "pixel.hlsl"
    vertex_source.write_text(CONTROL_VERTEX, encoding="ascii", newline="")
    pixel_source.write_text(CONTROL_PIXEL, encoding="ascii", newline="")
    common = [
        str(dxc),
        "-HV",
        "2021",
        "-all-resources-bound",
        "-Wno-ignored-attributes",
        "-Qstrip_reflect",
        "-Qstrip_debug",
        "-DREBLUE_RECOMP",
    ]
    control = output / "control"
    control.mkdir()
    vertex = control / "vertex.dxil"
    pixel = control / "pixel.dxil"
    run_command(
        common + ["-T", "vs_6_0", "-E", "main", "-Fo", str(vertex), str(vertex_source)]
    )
    run_command(
        common + ["-T", "ps_6_0", "-E", "main", "-Fo", str(pixel), str(pixel_source)]
    )
    return vertex, pixel, vertex_source, pixel_source


def emit_fixture(test_exe: Path, output: Path, family: str) -> Path:
    raw = (output / "inputs").resolve()
    if raw.exists():
        raise ValueError(f"fixture input directory must be fresh: {raw}")
    run_command(
        [
            str(require_file(test_exe, "test executable")),
            "--helper",
            f"{family}.emit",
            "--emit-fixture",
            str(raw),
        ]
    )
    require_file(raw / "fixture.json", "fixture metadata")
    return raw


def prepare(args: argparse.Namespace, family: str) -> None:
    output = fresh_directory(args.output)
    title_root = require_file(
        args.title_root / "scripts" / "rexglue.ps1", "ReXGlue script"
    ).parent.parent
    rexglue = require_file(args.rexglue, "ReXGlue script")
    adapter = require_file(args.adapter, "raw adapter")
    dxc = require_file(args.dxc, "pinned dxc")
    dxcompiler = require_file(args.dxcompiler, "dxcompiler.dll")
    dxil = require_file(args.dxil, "dxil.dll")
    shared_header = require_file(args.shared_header, "shader shared header")
    if dxcompiler.name.lower() != "dxcompiler.dll" or dxil.name.lower() != "dxil.dll":
        raise ValueError("--dxcompiler and --dxil must name the pinned DXC DLLs")
    if dxc.parent != dxcompiler.parent or dxcompiler.parent != dxil.parent:
        raise ValueError("DXC executable and both validator DLLs must be siblings")

    raw = emit_fixture(args.test_exe, output, family)
    emitted = json.loads((raw / "fixture.json").read_text(encoding="ascii"))
    if "form" not in emitted:
        fixture = FAMILIES.get(family, FAMILIES["native-prepared-bc2-rect"])
        validate_uv_bias(
            {"raw_inputs": str(raw), "fixture": emitted},
            fixture["metadata"]["width"],
            fixture["metadata"]["height"],
            fixed_uv=fixture["fixed_uv"],
        )
    copied = {}
    for source_name in (
        "registers.le.bin",
        "vertex-ucode.le.bin",
        "pixel-ucode.le.bin",
    ):
        copied[source_name] = copy_input(
            require_file(raw / source_name, source_name), output, source_name
        )
    if (output / copied["registers.le.bin"]).stat().st_size != REGISTER_COUNT * 4:
        raise ValueError("fixture register image has the wrong extent")
    interface = {
        "vertex_word_sha256": digest(output / copied["vertex-ucode.le.bin"]),
        "pixel_word_sha256": digest(output / copied["pixel-ucode.le.bin"]),
    }
    recipe_paths = {
        "registers": copied["registers.le.bin"],
        "vertex_microcode": copied["vertex-ucode.le.bin"],
        "pixel_microcode": copied["pixel-ucode.le.bin"],
        "shared_header": str(shared_header),
    }
    recipe = write_recipe(
        output,
        recipe_paths,
        {"dxc": dxc, "dxcompiler": dxcompiler, "dxil": dxil, "adapter": adapter},
    )
    codegen = [
        "powershell.exe",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(rexglue),
        "-Stage",
        "Codegen",
        "-ShaderRecipe",
        str(recipe),
    ]
    if args.sdk_install is not None:
        codegen.extend(["-SdkInstall", str(args.sdk_install.resolve(strict=True))])
    run_command(codegen, cwd=title_root)

    candidate = output / "candidate"
    candidate_vertex = require_file(
        candidate / f"vs_{VERTEX_HASH}.dxil", "candidate vertex DXIL"
    )
    candidate_pixel = require_file(
        candidate / f"ps_{PIXEL_HASH}.dxil", "candidate pixel DXIL"
    )
    candidate_source = require_file(
        candidate / f"ps_{PIXEL_HASH}.executable.hlsl", "candidate executable source"
    )
    candidate_catalog = output / "candidate_catalog"
    candidate_catalog.mkdir()
    copy_input(candidate_vertex, candidate_catalog, "candidate.vs.dxil")
    copy_input(candidate_pixel, candidate_catalog, "candidate.ps.dxil")
    interface["vertex_dxil_sha256"] = digest(candidate_vertex)
    interface["pixel_dxil_sha256"] = digest(candidate_pixel)
    write_catalog(
        candidate_catalog / "shaders.toml",
        interface,
        "candidate.vs.dxil",
        "candidate.ps.dxil",
    )

    control_vertex, control_pixel, vertex_source, pixel_source = compile_control_pair(
        output, dxc
    )
    control_catalog = output / "control_catalog"
    control_catalog.mkdir()
    copy_input(control_vertex, control_catalog, "control.vs.dxil")
    copy_input(control_pixel, control_catalog, "control.ps.dxil")
    interface["vertex_dxil_sha256"] = digest(control_vertex)
    interface["pixel_dxil_sha256"] = digest(control_pixel)
    write_catalog(
        control_catalog / "shaders.toml",
        interface,
        "control.vs.dxil",
        "control.ps.dxil",
    )

    prepared = {
        "schema_version": 1,
        "raw_inputs": str(raw),
        "candidate_catalog": str(candidate_catalog),
        "control_catalog": str(control_catalog),
        "candidate_source": str(candidate_source),
        "control_vertex_source": str(vertex_source),
        "control_pixel_source": str(pixel_source),
        "fixture": emitted,
    }
    prepared_path = output / "prepared.json"
    prepared_path.write_text(json.dumps(prepared, indent=2) + "\n", encoding="ascii")
    print(json.dumps(prepared, indent=2))


def run_test(args: argparse.Namespace, family: str) -> None:
    test_exe = require_file(args.test_exe, "test executable")
    prepared = json.loads(
        require_file(args.prepared, "prepared metadata").read_text(encoding="ascii")
    )
    output = args.output.resolve()
    if output.exists():
        raise ValueError(f"output directory must be fresh: {output}")
    run_command(
        [
            str(test_exe),
            "--helper",
            f"{family}.run",
            "--candidate-catalog",
            prepared["candidate_catalog"],
            "--control-catalog",
            prepared["control_catalog"],
            "--candidate-source",
            prepared["candidate_source"],
            "--output",
            str(output),
        ]
    )


def validate_uv_bias(
    prepared: dict, width: int, height: int, fixed_uv: bool = False
) -> None:
    raw = require_directory(Path(prepared["raw_inputs"]), "raw inputs")
    emitted = json.loads((raw / "fixture.json").read_text(encoding="ascii"))
    for metadata in (prepared.get("fixture", {}), emitted):
        if (
            metadata.get("uv_bias_base_texels") != 0.25
            or metadata.get("width") != width
            or metadata.get("height") != height
        ):
            raise ValueError(
                "prepared inputs need a fresh quarter-texel biased fixture"
            )
    vertices = require_file(raw / "vertices.bin", "biased vertices").read_bytes()
    if len(vertices) != 3 * 56:
        raise ValueError("biased fixture vertex extent changed")
    u_bias, v_bias = 0.25 / width, 0.25 / height
    expected = (
        ((0.5 + u_bias, 0.5 + v_bias),) * 3
        if fixed_uv
        else (
            (u_bias, 1.0 + v_bias),
            (u_bias, -1.0 + v_bias),
            (2.0 + u_bias, 1.0 + v_bias),
        )
    )
    for vertex, uv in enumerate(expected):
        if struct.unpack_from(">ff", vertices, vertex * 56 + 16) != uv:
            raise ValueError(
                "prepared vertex UVs do not contain the quarter-texel bias"
            )


def check(args: argparse.Namespace, family: str) -> None:
    fixture = FAMILIES[family]
    expected = fixture["metadata"]
    prepared = json.loads(
        require_file(args.prepared, "prepared metadata").read_text(encoding="ascii")
    )
    for field in ("raw_inputs", "candidate_catalog", "control_catalog"):
        require_directory(Path(prepared[field]), field)
    if "format" in expected:
        raw_root = Path(prepared["raw_inputs"])
        suffix = "bc1" if expected["format"] == "k_DXT1" else "bc2"
        for name in (
            f"texture-base.{suffix}.bin",
            f"texture-mips.{suffix}.bin",
            "fixture.json",
        ):
            require_file(raw_root / name, f"raw fixture {name}")
    require_file(Path(prepared["candidate_source"]), "candidate_source")
    validate_uv_bias(
        prepared,
        expected["width"],
        expected["height"],
        fixed_uv=fixture["fixed_uv"],
    )
    metadata = prepared.get("fixture", {})
    for field, value in expected.items():
        actual = metadata.get(field)
        if isinstance(value, tuple):
            actual = tuple(metadata.get(field, ()))
            if field == "triangle_uvs":
                actual = tuple(tuple(uv) for uv in actual)
        mismatch = actual is not value if isinstance(value, bool) else actual != value
        if mismatch:
            raise ValueError(fixture["schema_error"])
    print(fixture["success"])


def check_results(args: argparse.Namespace) -> None:
    receipt = tomllib.loads((args.output / "result.toml").read_text(encoding="ascii"))
    if (
        receipt.get("uv_bias_base_texels") != 0.25
        or receipt.get("texel_edge_guard") is not True
    ):
        raise ValueError(
            "result receipt lacks the quarter-texel bias and sampling guard"
        )
    print("quarter-texel bias result receipt: PASS")


def run_checked(args: argparse.Namespace, family: str) -> None:
    check(args, family)
    run_test(args, family)
    check_results(args)


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    root.add_argument("family", choices=FAMILIES)
    sub = root.add_subparsers(dest="command", required=True)
    prepare_parser = sub.add_parser("prepare")
    prepare_parser.add_argument("--test-exe", type=Path, required=True)
    prepare_parser.add_argument("--title-root", type=Path, required=True)
    prepare_parser.add_argument("--rexglue", type=Path, required=True)
    prepare_parser.add_argument("--sdk-install", type=Path)
    prepare_parser.add_argument("--adapter", type=Path, required=True)
    prepare_parser.add_argument("--dxc", type=Path, required=True)
    prepare_parser.add_argument("--dxcompiler", type=Path, required=True)
    prepare_parser.add_argument("--dxil", type=Path, required=True)
    prepare_parser.add_argument("--shared-header", type=Path, required=True)
    prepare_parser.add_argument("--output", type=Path, required=True)
    prepare_parser.set_defaults(function=prepare)

    run_parser = sub.add_parser("run")
    run_parser.add_argument("--test-exe", type=Path, required=True)
    run_parser.add_argument("--prepared", type=Path, required=True)
    run_parser.add_argument("--output", type=Path, required=True)
    run_parser.set_defaults(function=run_checked)

    check_parser = sub.add_parser("check")
    check_parser.add_argument("--prepared", type=Path, required=True)
    check_parser.set_defaults(function=check)
    return root


if __name__ == "__main__":
    arguments = parser().parse_args()
    try:
        arguments.function(arguments, arguments.family)
    except (
        OSError,
        ValueError,
        subprocess.CalledProcessError,
        json.JSONDecodeError,
    ) as error:
        raise SystemExit(f"test-native-prepared: {error}") from error
