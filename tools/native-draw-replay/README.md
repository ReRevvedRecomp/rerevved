# Native draw replay

`native_draw_replay.exe` consumes one validated TOML recipe and writes two
guest-order color sample planes, followed by two packed depth planes when
depth is enabled, after the D3D12 queue fence completes:

```
native_draw_replay.exe <recipe-file> <output-samples-file>
```

The title driver supplies `-Stage Replay`, `-ReplayRecipe`, and
`-ReplayOutput`; the harness never starts the guest application.

## Live panel comparison

The Windows title also supports a default-off, one-shot native comparison
while Xenos runs the guest. Supply `--native_menu_shadow_output=<fresh-dir>`
and `--native_menu_shadow_shaders=<shader-dir>` through the title driver's
interactive Launch stage. Both directories must be under the ignored `out/`
tree. The shader directory contains `vs.dxil` and `ps.dxil` for the validated
panel pair; the decoder checks their exact digests and the live guest shader
identity before submission. Private shader binaries are not distributed here.

After the natural menu is ready, create an `arm` file in the output directory's
`capture/` child. The Xenos command processor captures the next eligible panel
draw and completes its queue fence before publishing owned bytes to the title.
A title worker decodes that draw and submits it to native D3D12 in the same
process. The output includes the original capture, native and reference sample
planes, and `comparison.toml`. Completion requires a nonempty visible change
and at most two byte values of error per channel across both sample planes.

This comparison uses the live Xenos color contents before the panel as the
initial blend target. It exercises input acquisition, native submission and
retirement for one draw. Xenos retains guest execution and presentation;
continuous native frames and guest D3D replacement require separate work.

The standalone replay tool configures debug-layer and DRED diagnostics before
device creation. Live replay inherits the host's process settings and owns its
own queue, command objects, resources, and completion fence.

Schema 1 admits the captured untextured panel pair. Schema 2 supports translated
triangle draws with one to sixteen packed float4 vertex attributes and an
optional texture at fetch slot zero. Both use these base tables:

```
schema_version = 1

[paths]
vertex_dxil = "vertex.vs_6_0.dxil"
pixel_dxil = "pixel.ps_6_0.dxil"
vertices = "vertices.float4.bin"
indices_u32 = "indices.u32.bin"
vertex_constants = "vertex-constants.bin"
pixel_constants = "pixel-constants.bin"
shared_constants = "shared-constants.bin"
initial_samples = "initial-samples.rgba"

[target]
width = 640
height = 720
sample_count = 4
sample_mask = 9
clear_color_rgba8 = [0, 0, 0, 0]

[draw]
topology = "triangle_list"
indexed = true
vertex_stride_bytes = 32
vertex_attribute_count = 2
index_count = 30
texture_mask = 0

[shader]
vertex_hash = "0x11213E38D7154104"
pixel_hash = "0x3A92D78FE55C7B83"

[viewport]
x = 0.0
y = 0.0
width = 1280.0
height = 720.0
min_depth = 0.0
max_depth = 1.0

[scissor]
left = 0
top = 0
right = 640
bottom = 720

[blend]
enabled = true
alpha_to_coverage = false
logic_op_enabled = false
source_color = "source_alpha"
destination_color = "inverse_source_alpha"
color_op = "add"
source_alpha = "source_alpha"
destination_alpha = "inverse_source_alpha"
alpha_op = "add"
write_mask = 15
```

`vertices.float4.bin` is packed little-endian float32 TEXCOORD0 then TEXCOORD1
(32 bytes per vertex). `indices.u32.bin` is little-endian uint32. Constant
files are little-endian float4 register bytes and may be padded by the
harness to the D3D12 256-byte CBV alignment. `initial_samples` is two
concatenated 640 * 720 * 4 byte RGBA8 planes for the supported capture (the
parser also accepts separate `initial_sample0` and `initial_sample1` paths).
Host sample 0 maps to guest sample 0 and host sample 3 maps to guest sample 1.
Schema 1 uses host sample mask `0x9`; this restricts coverage before attribute
interpolation and can differ from Xenos ROV centroid interpolation.
Schema 2 also admits `sample_mask = 15`: the supplied pixel shader must export
`SV_Coverage = 0x9` on every non-discarded path. This retains four coverage
samples for interpolation and restricts color writes afterward. The harness
does not inspect shader computation to enforce that export contract. Neither
mode alone establishes parity; compare the resulting sample bytes.
The output path is the second CLI argument and is not taken from the recipe.

Schema 2 accepts optional `[target] format = "rgb10a2_unorm"`; the default is
`rgba8_unorm`. RGB10A2 initial and output samples are little-endian uint32
values with R in bits 0-9, G in 10-19, B in 20-29, and A in 30-31. Both formats
use four bytes per sample. Texture sampling, render targets, and readback retain
the declared target precision. The RGB10A2 subset requires depth disabled.
`clear_color_rgba8` remains four normalized byte components in either format.

The executable uses the native renderer's D3D12 device, direct queue, fence,
and renderer thread. It requires translated DXIL supplied by the recipe and
rejects non-triangle geometry, non-uint32 indices, invalid ranges, unsupported
sample state, and over-bound inputs before creating GPU work. Depth is optional
in schema 2; stencil testing is disabled.

## Textured triangle recipes

For schema 2, set `vertex_attribute_count` to the translated VS input count,
`vertex_stride_bytes` to that count times 16, and provide consecutive float4
`TEXCOORD0` through `TEXCOORD<count-1>` values. Decode guest vertex formats and
endianness before writing the vertex file; leave destination swizzles to the
translated shader. Shader hashes must be nonzero provenance identifiers; the
harness does not derive or authenticate them from DXIL bytes.

With `texture_mask = 1`, add both tables below. The texture file contains one
linear mip level, without upload-row padding. Supported `format` strings are
`rgba8_unorm`, `r8_unorm`, `bc1_unorm`, and `bc2_unorm`. BC payloads retain their
compressed blocks. `swizzle` selects RGBA components with values 0 through 3,
constant zero with 4, and constant one with 5.

```toml
[texture]
file = "texture.bin"
format = "rgba8_unorm"
width = 64
height = 64
swizzle = [0, 1, 2, 3]

[sampler]
min_linear = true
mag_linear = true
mip_linear = true
address_u = 3
address_v = 3
address_w = 3
min_lod = 0.0
max_lod = 0.0
mip_bias = 0.0
border = [0.0, 0.0, 0.0, 0.0]
```

Sampler address values use D3D12 numbering: wrap 1, mirror 2, clamp 3, border 4,
and mirror-once 5. Translate foreign capture-tool enums explicitly. Payload
lengths must match the declared dimensions and format exactly; the harness
adds D3D12 copy-footprint padding and keeps upload resources alive until fence
completion.

The shader binding contract is:

| Input | Register | Space |
|---|---|---|
| Vertex constants, at least 256 float4 values | b0 | 4 |
| Pixel constants, at least 224 float4 values | b1 | 4 |
| Shared constants, at least 336 bytes | b2 | 4 |
| One Texture2D SRV, descriptor index zero | t0 | 0 |
| One sampler, descriptor index zero | s0 | 3 |

Both stages must agree on the supplied shared-constant layout. Texture and
sampler arrays must be bounded to one descriptor, with accesses specialized to
index zero. Multi-texture, cube, volume, mip-chain, and stencil draws need
additional replay support. A successful replay proves execution and readback;
compare the sample bytes against the captured draw to assess rendering parity.

## Depth and rasterizer state

Schema 2 accepts these optional tables. Without them, depth is disabled and
culling is off.

```toml
[depth]
enabled = true
write_enabled = true
compare = "less_equal"
initial_clear = 1.0
initial_samples = "depth-before.bin"

[rasterizer]
cull = "back"
front_counter_clockwise = false
```

Depth uses a native four-sample `D24_UNORM_S8_UINT` resource. Comparisons are
`never`, `less`, `equal`, `less_equal`, `greater`, `not_equal`, `greater_equal`,
and `always`; culling is `none`, `front`, or `back`. The supplied vertex and
pixel shaders must preserve the captured depth and winding semantics. A pixel
shader that does not export guest depth must not introduce an `SV_Depth` output.

`initial_clear` initializes every depth sample. The optional `initial_samples`
file replaces host samples 0 and 3 with two packed depth planes, each exactly
`width * height * 4` bytes. Each little-endian uint32 is `depth24 << 8`; the
low stencil byte must be zero. Depth initialization uses sample-masked draws,
and depth tests and writes use the native DSV. Stencil remains zero.

With depth enabled, the output contains color sample 0, color sample 1, depth
sample 0, then depth sample 1. Depth readback uses the same packed format as
the input. These are sample planes, not a guest resolve. With depth disabled,
the output contains only the two color planes.
