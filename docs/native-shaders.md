# Native shader artifact generation

This is a developer tool, not a player setup step. The Xenos player renderer
does not require these generated catalogs. See the [runtime
boundary](rexglue-runtime.md#runtime-boundary) before selecting a native route.

[`build-native-shaders.py`](../scripts/build-native-shaders.py) uses a recipe in
schema 1 with SHA-256 digests that pin the microcode, registers, shared HLSL
header, adapter, and compiler tools. Run
`python scripts/build-native-shaders.py <recipe.json> --check` for input preflight,
then omit `--check` to translate and compile into the recipe's fresh `output`
directory. Existing output directories are rejected.

`color_targets` selects how many render targets the pixel shader writes: 1
(the default) or 2. With 2, the captured RB_COLOR1_INFO (register `0x2003`)
must name `k_8_8_8_8` or `k_2_10_10_10` with exponent bias 0, and the pixel
source declares `oC1` as `SV_Target1` after `SV_Target0`; the raw translation
otherwise keeps `oC1` local. One target leaves the translation unchanged.

When the captured registers disable guest clipping and the viewport transform
(`PA_CL_CLIP_CNTL` `0x90000` and `PA_CL_VTE_CNTL` `0x400`), the vertex shader
maps its positions in pixel coordinates to the native full viewport, and its
`generation.json` record carries `"screen_space_position": true`. Copy that
flag into the pair's `shaders.toml` entry as `screen_space_position = true`.
Other records omit the key.

When the captured `PA_CL_CLIP_CNTL` is `0x80001` (user clip plane 0 with
clipping on), the vertex shader also writes `SV_ClipDistance0`, after its
interpolators, as the dot product of its position before the half-pixel
offset and `g_UserClipPlane0` at shared constant `c29`; its record carries
`"user_clip_plane": true`, which the entry copies as `user_clip_plane = true`.
Other enabled plane sets and cull-only planes are rejected. In SPIR-V output
DXC maps `SV_ClipDistance0` to the Vulkan `ClipDistance` built-in, so the
generation metadata then lists `shaderClipDistance` among the device features
the consuming Vulkan device must enable.

A scene catalog, which `--native_draw_scene_shaders` names, may hold several
entries for one pixel shader: one per vertex shader it pairs with and one per
specialization. The runtime selects an entry by the patched vertex word digest,
the alpha test state, the sample count and the program words. Its
`texture_fetches` list the pixel slots 0..n-1 and may end with slot 16 for the
vertex stage, which the shared header binds for both stages. The
runtime refuses draws of such a pair until it can bind a texture for the vertex stage.
Entries of one pixel shader must list the same pixel slots. Of the entries
with the alpha test enabled, only those comparing `Greater` are selected.

A scene catalog may also hold `depth-only.toml` (`schema_version` 1 or 2),
which `LoadNativeGameplayShaders` does not read. Each `[[depth_only]]` entry
names one vertex shader that draws without a pixel shader: `id`, `caller`,
`vertex_hash`, `vertex_word_sha256`, its vertex artifact (schema 1:
`vertex_dxil_sha256` and `vertex_file`; schema 2: see below), the unfolded
`program`, `context` and `interpolators` words, `mode_control = 5` and
`color_mask = 0`. Program words with generated pixel parameters (bit 18),
pixel artifacts and duplicate entries are refused. The vertex attributes come
from each draw's live microcode.

`output_format` accepts `dxil`, `spirv` or `dual`; omission selects DXIL. DXIL
retains the wrapper or standalone DXC compiler interface, `.dxil` artifacts,
and `generation.json` record. SPIR-V and dual output require standalone
`tools.dxc` and `tools.spirv_val`, each specified as a `file` and `sha256`,
alongside the existing pinned tools. Wrapper compilers cannot be mixed with
standalone DXC.

## Dual output

A `dual` recipe emits both formats in one run. The output directory holds
exactly the files, digests and `generation.json` keys a `dxil` run of the same
recipe produces. Each stage is translated once: the raw adapter inlines its
stage header ahead of the translation, so the SPIR-V variant replaces that
prefix with the SPIR-V header (below) instead of translating again, and a
translation that does not begin with its header is refused. The SPIR-V
variant then receives the same specializations as the DXIL source plus the
SPIR-V-only edits, and compiles in the `spirv/` subdirectory with its own
`shader_common.h`, `<stage>_<hash>.hlsl`, `.executable.hlsl`, `.spv` and
compile and validation logs. DXIL files and digests are unaffected.

`generation.json` of a dual run is a superset of the DXIL record: each shader
record adds `spirv_sha256`, and the top level adds a `spirv` object with
`target_env`, `required_features` and `spirv_val_sha256`. It is written only
after both formats of every stage compile and validate.

A dual recipe with `"depth_only": true` lists exactly one `vs` shader and
emits that vertex stage in both formats for a `depth-only.toml` entry; its
`generation.json` carries `"depth_only": true`. Pixel target and alpha state
do not apply to it, and its registers must select guest clipping and the
viewport transform: a screen-space position or user clip plane is refused
because a depth-only entry has no flag for it. Other recipes require one
vertex/pixel pair.

## Catalog schema 2

`shaders.toml` and `depth-only.toml` schema 2 keep every identity and
specialization key of schema 1 and replace the artifact keys (`vertex_file`,
`pixel_file`, `vertex_dxil_sha256`, `pixel_dxil_sha256`) with one sub-table
per stage and format, each holding a relative `file` and its `sha256`:

```toml
schema_version = 2
[[shaders]]
vertex_hash = "..."
# Identity and specialization keys as in schema 1.
[shaders.vertex_dxil]
file = "vs_<hash>.dxil"
sha256 = "<dxil_sha256 of the vs record>"
[shaders.pixel_dxil]
file = "ps_<hash>.dxil"
sha256 = "<dxil_sha256 of the ps record>"
[shaders.vertex_spirv]
file = "spirv/vs_<hash>.spv"
sha256 = "<spirv_sha256 of the vs record>"
[shaders.pixel_spirv]
file = "spirv/ps_<hash>.spv"
sha256 = "<spirv_sha256 of the ps record>"
```

Depth-only entries carry `vertex_dxil` and `vertex_spirv` only. The
`dxil_sha256` and `spirv_sha256` of each dual `generation.json` record become
the `sha256` of the matching sub-table; `screen_space_position` and
`user_clip_plane` copy into the entry as for DXIL.

`LoadNativeGameplayShaders` and `LoadNativeDepthOnlyShaders` take a
`NativeShaderFormat` (`Dxil` by default) and fill only that format's bytes and
digests. Every artifact keeps the bounds on relative paths, directories and the
4 MiB size limit
and its digest check. Schema 1 is DXIL-only. An entry without the requested
format's artifact, including an empty sub-table, refuses the whole catalog as
`shader_format_missing: <entry>`, where `<entry>` is the entry's `id` or else
`<vertex_hash>-<pixel_hash>`; a schema 1 catalog requested as SPIR-V refuses
on its first entry. A schema 2 entry with schema 1 artifact keys is refused.
Schema 1 catalogs load only when DXIL is requested.

SPIR-V compilation uses the established `vs_6_0` and `ps_6_0` profiles and constant
specialization with `-spirv -fspv-target-env=vulkan1.2 -fvk-use-dx-layout`.
Vertex compilation also uses `-fvk-invert-y`; pixel compilation uses
`-fvk-use-dx-position-w` to preserve the HLSL `SV_Position.w` convention.
The uniform layout requires the consuming Vulkan device to enable
`scalarBlockLayout`. See DXC's [memory layout and Vulkan options](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/SPIR-V.rst).

The raw adapter's descriptor-backed constants remain uniform buffers. The
generated shared header disables its buffer-device-address branch specifically
for this adapter. Texture register spaces map to sets 0, 1, and 2, each at
binding 0; samplers use set 3, binding 0. Vertex, pixel, and shared constant
buffers use set 4, bindings 0, 1, and 2 respectively. No buffer-device-address
or push-constant offsets are inferred.

A shader with loops reads its loop constants at draw time from
`g_LoopConstantsArr`, at `c21` of the shared constant buffer. Native gameplay
draws of such a pair bind the live `SHADER_CONSTANT_LOOP` registers
`0x4908..0x4927` at bytes 336..463, after the booleans; other pairs keep the
336-byte shared constants. Catalog entries do not depend on loop words.

Supported non-system stage parameters are `float4 TEXCOORD0` through
`TEXCOORD15`, with explicit Vulkan locations equal to the semantic index.
Vertex outputs and pixel inputs must contain all sixteen interpolators;
unsupported declarations, duplicate locations, and changed descriptor bindings
fail visibly. System semantics retain DXC's built-in mapping.

Each `.spv` is checked by the pinned validator with
`--target-env vulkan1.2 --scalar-block-layout`. Translation, compilation, and
validation logs remain in the output directory. A `spirv` run writes only
SPIR-V and, for a fully successful pair, `generation.spirv.json`, containing
`spirv_sha256` shader identities, the validator digest, target environment,
and required features. A failed run of any format retains diagnostic output,
writes no generation record and requires a new output directory for retry.

The SPIR-V vertex specialization `C3BAB94E67553E12` orders its world-Z product
as `(H * cos(a)) * cos(b)` to match DXIL. The generator retains `cos(b)` before
its translated register is overwritten and leaves world X/Y unchanged. It
requires one exact translated instruction block and rejects a changed or
duplicate block. Other vertex hashes and pixel shaders keep their existing
translation. DXIL generation does not apply this source adaptation.

The raw translation declares the pixel face input `iFace : SV_IsFrontFace` as
`uint`, which DXIL accepts. Vulkan requires the `FrontFacing` built-in to be a
boolean, so a pixel shader whose generated pixel address reads the face
declares it as `bool` in its SPIR-V variant; the edit requires the one exact
translated declaration. Other pixel shaders and all DXIL sources keep the
translated declaration.

This tool generates shader artifacts. The optional Windows
[offscreen draw comparison](../tools/README.md#offline-vulkan-draw-comparison)
consumes one exact validated specialization and compares native Vulkan and
D3D12 color samples. It rejects changed module identities, unsupported state,
and pixel mismatches. Runtime Vulkan rendering, FPS evidence, coverage of the full game,
and replacement of Xenos are not established by that comparison.
