# Native draw replay

`native_draw_replay.exe` consumes one validated TOML recipe and writes two
guest-order RGBA8 sample planes after the D3D12 queue fence completes:

```
native_draw_replay.exe <recipe-file> <output-samples-file>
```

The title driver supplies `-Stage Replay`, `-ReplayRecipe`, and
`-ReplayOutput`; the harness never starts the guest application.

The recipe has `schema_version = 1` and these tables:

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
Host sample 0 maps to guest sample 0 and host sample 3 maps to guest sample 1;
host sample mask `0x9` restricts native target writes to those initialized
planes. This is a replay subset: Xenos ROV uses all four coverage samples,
so this mapping alone does not establish matching centroid interpolation.
The output path is the second CLI argument and is not taken from the recipe.

The executable uses the native renderer's D3D12 device, direct queue, fence,
and renderer thread. It requires the actual translated DXIL supplied by the
recipe and rejects other shader hashes, texture use, non-triangle geometry,
non-uint32 indices, invalid ranges, unsupported sample state, and over-bound
inputs before creating GPU work.
