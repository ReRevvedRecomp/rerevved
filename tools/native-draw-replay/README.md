# Native draw replay

`native_draw_replay.exe` consumes one validated TOML recipe and writes two
guest-order RGBA8 sample planes after the D3D12 queue fence completes:

```
native_draw_replay.exe <recipe-file> <output-samples-file>
```

The title driver supplies `-Stage Replay`, `-ReplayRecipe`, and
`-ReplayOutput`; the harness never starts the guest application.

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

The executable uses the native renderer's D3D12 device, direct queue, fence,
and renderer thread. It requires translated DXIL supplied by the recipe and
rejects non-triangle geometry, non-uint32 indices, invalid ranges, unsupported
sample state, and over-bound inputs before creating GPU work. Depth and stencil
are disabled in this replay subset.

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
index zero. Multi-texture, cube, volume, mip-chain, and depth/stencil draws need
additional replay support. A successful replay proves execution and readback;
compare the sample bytes against the captured draw to assess rendering parity.
