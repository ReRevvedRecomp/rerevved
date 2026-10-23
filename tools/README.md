# Native draw replay

This developer reference covers offline fixtures and bounded native rendering
diagnostics, not player setup. Use [Development](../docs/development.md) to
build the tools and [Native shader artifact
generation](../docs/native-shaders.md) to prepare shader inputs. A fixture
pass establishes only its declared forms and controls, not general game
coverage.

`native_draw_replay.exe` is built only by a developer build
(`REREVVED_DEVELOPER=ON`, preset `win-amd64-developer`, target
`native_draw_replay`; see [Development](../docs/development.md#developer-build)).
It consumes one validated TOML recipe and writes two
color sample planes in guest order, followed by two packed depth planes when
depth is enabled, after the D3D12 queue fence completes:

```
native_draw_replay.exe <recipe-file> <output-samples-file>
```

The title driver supplies `-Stage Replay`, `-ReplayRecipe`, and
`-ReplayOutput`; that stage runs the replay executable of the developer build
directory (`out/build/win-amd64-developer`) and never starts the guest
application. The output file must be absent and its parent directory
must exist. The live diagnostic sections below use separate title launch flags.

## Find a diagnostic

- [Prepared RGBA8/R8 forms](#rgba8r8-prepared-exact-form-matrix) and
  [pixel register LOD](#pixel-register-lod-fixture): fixture contracts and guards.
- [BC1/BC2 matrix](#bc1bc2-prepared-form-matrix) and
  [BC3 matrix](#bc3-prepared-form-matrix): compressed texture forms.
- [Offline Vulkan comparison](#offline-vulkan-draw-comparison): compare one
  admitted specialization on Vulkan and D3D12.
- [Recorded sequences](#recorded-sequences): replay saved inputs in guest order.
- [Native draw worker](#native-draw-worker) and
  [draw recipe schemas](#draw-recipe-schemas): worker caches and recipe files.
- [Textured recipes](#textured-triangle-recipes),
  [depth state](#depth-and-rasterizer-state) and
  [gameplay scenes](#owned-gameplay-scenes): supported recipe state.
- [Exact RGBA8/R8](#exact-rgba8r8-prepared-matrix),
  [float](#exact-float-prepared-matrix) and
  [CPU cube](#exact-cpu-cube-matrix) matrices: exact-form controls.

## RGBA8/R8 Prepared exact-form matrix

The fixture proofs below are cases of the optional `rerevved_prepared_tests`
executable. Build it with `-DREREVVED_PREPARED_TESTS=ON`:

```powershell
cmake --preset win-amd64-release -DREREVVED_PREPARED_TESTS=ON
cmake --build --preset win-amd64-release --target rerevved_prepared_tests --parallel 4
ctest --test-dir out/build/win-amd64-release -C Release -L prepared-research
```

CTest registers each fixture guard named below as `prepared.<case>` with the
`prepared-research` label; the guards need no GPU. The fixture emitters and
Prepared-native run commands are helper commands of the same executable:
`rerevved_prepared_tests --helper native-prepared-rgba8-matrix.emit` and `.run`
for this matrix, and the same `<proof>.emit` and `<proof>.run` pattern for the
others. `rerevved_prepared_tests --helper list` prints every helper name, and the
scripts named below pass the helper name themselves; give each script the
executable as `--test-exe`. `rerevved_tests` keeps one representative case per
format family (`native_prepared_bc1_fixture`,
`native_prepared_rgba8_matrix_fixture` for `rgba8-8-packed-base`,
`native_prepared_float_matrix_fixture`, `native_prepared_cube_matrix_fixture` for
`rgba8-cube-64-packed`, and `native_pixel_lod_fixture`) with CPU-only checks of
the fixture layout, payload, fetch words and geometry.

The `native_prepared_rgba8_matrix_fixture` case and
`scripts/gpu/test-native-prepared-rgba8-matrix.py` provide one offline,
table-driven proof for these unsigned, nonstacked 2D forms. Pitch uses
32-texel fetch units; minimum LOD is zero.

| Form | Pitch | Declared/effective maximum LOD | Texture IDs |
|---|---|---|---|
| RGBA8 linear packed 8x8 | 2 | 3/0 | 18 |
| R8 linear packed 512x512 | 16 | 9/9 | 125,126 |
| RGBA8 linear packed 256x4 | 8 | 8/0 | 130 |
| RGBA8 linear packed 256x1 | 8 | 8/0 | 131 |
| RGBA8 tiled 32x32 | 1 | 0/0 | 133 |
| RGBA8 tiled 128x64 | 4 | 0/0 | 134,136 |
| RGBA8 tiled 64x64 | 2 | 0/0 | 137,139,141,142,143,144,147 |
| RGBA8 tiled 128x128 | 4 | 0/0 | 138,145,146 |
| RGBA8 tiled 64x128 | 2 | 0/0 | 148 |

RGBA8 uses `k8in32` and raw swizzle `0x60A`; R8 uses `kNone` and
`0x124`. The fixture retains all six captured descriptor words apart from
relocated physical pages, including declared LODs and zero mip pages.
Textures 18, 130 and 131 have zero mip pages: the reference's
SDK `src/graphics/pipeline/texture/util.cpp:77` clamps them to effective
LOD0, and `cache.cpp:1429` stores that normalized maximum in the Original
key (line locators at SDK `ab2531a`). The title
[`native_texture_upload.cpp`](../src/gpu/d3d12/native_texture_upload.cpp)
lines 226-269 applies the same clamp. The oracle, CPU updates, mutations
and omissions cover that one effective level; declared nonzero levels are
clamped by the zero mip page.

The frozen layout table lives in
[`native_prepared_rgba8_matrix_forms.h`](../tests/gpu/native_prepared_rgba8_matrix_forms.h).
R8's separate mip range is 126752 bytes, including its 3872-byte packed
tail at 122880. Packed RGBA8 base extents are 1888, 20480 and 17408 bytes
for 8x8, 256x4 and 256x1. LOD0 starts at byte offsets 64, 16384 and
16384, respectively. The 8x8 Prepared range retains the already covered
legacy 2048 bytes. The fixture guard compares the real versioned upload
with independently decoded bytes and poisons the prefix at offset zero, so
reading a packed base from offset zero fails the guard.

The raw CPU oracle implements the
[revision-pinned Xenos 2D tiling formula](https://github.com/gildor2/UModel/blob/de8fbd3bc922427ea056b7340202dcdcc19ccff5/Unreal/UnTexture.cpp#L489)
without production conversion or SDK address helpers. It reverses endian
bytes, selects guest components and checks every texel of every effective
level. Packed mip and tail offsets come from the frozen table, which the guard
checks against SDK `GetPackedMipOffset`, so they are not independent of that
helper. Hashed per-texel X/Y/channel patterns, which no nonzero shift or
transpose of a level preserves, feed a weighted dot-product
shader with weights 1,2,4,8 and RGB replacement. This makes each component
output-sensitive, including R8's alpha-only swizzle. Full terminal RGBA8
planes compare at zero tolerance, with presenter alpha 255 and duplicated
native RT0 planes. This synthetic shader proves the forms' source handling;
it does not accept the saved shader graph.

Every form uses UV bias `0.25/baseExtent` on each axis. With integer-position
Original and translated-native sampling the coordinate is
`(pixel+0.25)*levelExtent/baseExtent`; the RGBA8/R8 runner's control shader
retains pixel-center sampling at `(pixel+0.75)*levelExtent/baseExtent`. The fixture
checks identical texels and no samples at texel edges for both conventions at
every pixel and level. `PA_SU_VTX_CNTL` selects D3D centers and the Original
run pins `half_pixel_offset=true`; result validation requires that field.

Run the `native_prepared_rgba8_matrix_fixture` CTest before using the runner's
`prepare`, `check`, `run` and `check-results` commands. Preparation reuses the
rectangular BC2 runner's tool flags and catalog workflow with the matrix's
emitted shader words and its weighted control shader. The GPU proof checks
partial and overlapping CPU updates within the same allocation, immutable old/new
owners, per-level mutations, pre-submission omissions, six callback negative
probes, native completion held separately from SDK copy retirement, and
isolated WARP device removal. Compilation and a completed GPU result are
required before treating this matrix as runtime acceptance.

## Pixel register-LOD fixture

The `native_pixel_lod_fixture` case and `scripts/gpu/test-native-prepared.py native-pixel-lod` prove explicit
register LOD for all eight levels of a linear 128x128 RGBA8 texture. The fixed
UV is `(0.5+0.25/128, 0.5+0.25/128)`. Its independent CPU oracle selects that
coordinate at each level. The fixture guard checks both axes at every level,
requiring the same interior texel under integer-position Original sampling
and plain pixel centers, with a fetch epsilon margin of `1.5/1024` base texels.
The runner rejects stale unbiased metadata and vertex bytes before submission
and validates the bias in the result receipt. Per-level mutations and omissions
remain output-sensitive. Solid mip colors do not prove spatial texel coverage
or general filtering behavior.

## 128x128 BC1 Prepared fixture

The `native_prepared_bc1_fixture` case and `scripts/gpu/test-native-prepared.py native-prepared-bc1` prove all
eight levels of a linear packed 128x128 BC1 texture through Original,
control-native and Prepared-native. The fixed UV is
`(0.5+0.25/128, 0.5+0.25/128)`. The independent CPU oracle selects that
coordinate, and the guard checks both axes at every level against both
pixel-center conventions with a fetch epsilon margin of `1.5/1024` base texels.
Fresh preparation is required: the runner rejects unbiased metadata and vertex
bytes before GPU submission and validates the bias in each result receipt.
The CPU update, mutation, omission, callback, completion and lifetime checks
remain separate controls. Solid mip colors do not prove spatial block coverage,
BC1 interpolation or transparency conformance.

## 256x256 BC1 Prepared fixture

The 256x256 BC1 proof registers the `native_prepared_bc1_256_fixture` case and uses an
unsigned linear, nonstacked `k_DXT1` Texture2D with pitch 8, `k8in16`, swizzle
`0x688`, and nine levels from 256x256 through 1x1. The exact guest footprint
is a `0x8000` byte base range and a `0x6340` byte canonical mip range; the
fixture page-aligns the stored mip bank to `0x7000` bytes. Levels
1, 2, and 3 use ordinary offsets `0`, `0x2000`, and `0x4000` with row pitch
`0x100`; levels 4 through 8 use the packed tail at `0x6000`, whose extent is
`0x340` bytes, with offsets `0x6020`, `0x6010`, `0x6008`, `0x6200`, and
`0x6100`. The selected raw fetch words are
`02000002 19FE9052 001FE0FF 00A80D10 00000203 19FE1A18`; physical addresses
are patched for each isolated allocation.

Run the fixture guard CTest, then prepare and run its focused proof with
`scripts/gpu/test-native-prepared.py native-prepared-bc1-256`. The fixture authors all nine raw
guest levels, decodes every full block independently including `k8in16`
byte pairs, and uses asymmetric X/Y block colors with biased UVs so the
last blocks and packed offsets are exercised. All nine LOD pairs compare the
actual Original capture, a raw independent control decode, and Prepared native
replay on a synthetic 1280x720 canvas with a 256x256 source viewport. The
fullscreen triangle uses UVs `(0,1)`, `(0,-1)`, and `(2,1)` with screen-Y
inversion, and the source and PM4 stream viewport registers use the affine
values `[128, 128, -128, 128]` to map NDC `[-1,1]` to the complete 256x256
region. Each of the nine pair records also emits
`candidate-baseline-<pair>.bin` and `control-baseline-<pair>.bin`, comparing
both native baseline replays with the Original baseline before the changed
`candidate-<pair>.bin` and `control-<pair>.bin` routes.
The result report names all 36 output files and records nine completed pairs,
nine sensitivity checks, and nine rejections for omitted levels.
The point/clamp sampler is defined by the fixture, LOD 0 through 8 is explicit through
pixel register LOD, and presenter alpha is normalized to 255 with duplicated
native RT0 diagnostic planes. Partial and overlapping CPU updates within the
same allocation, unchanged outside bytes, per-level mutation sensitivity, omission
rejection, callback failure probes, native completion, SDK copy retirement,
and isolated WARP failure remain separate checks in the `prepare` and `run`
workflow. The accepted profile uses opaque RGB565 endpoint 0 with selector 0;
interpolation and transparency conformance remain outside this fixture.

Authored UVs add `0.25/baseExtent` on each axis. The independent CPU oracle
models Original at integer pixel positions after the +0.5 pixel geometry shift.
The guard enumerates every pixel on both axes at every level, requiring the
same interior texel under plain pixel centers, with a fetch epsilon margin of
`1.5/1024` base texels. Coverage of distinct texels and the last block is retained.
The runner rejects stale unbiased metadata and vertex bytes before submission
and validates the recorded bias and guard in the result receipt.

## 512x512 BC1 Prepared fixture

The 512x512 BC1 proof registers the `native_prepared_bc1_512_fixture` case and uses an
unsigned linear, nonstacked `k_DXT1` Texture2D with pitch 16, `k8in16`, swizzle
`0x688`, and ten levels from 512x512 through 1x1. The exact guest footprint
is a `0x20000` byte base range and a `0xE340` byte canonical mip range. The
Legacy mip extent is `0xE000`, and the packed tail starts at `0xE000` with an
`0x340` byte extent. Levels 1 through 4 use ordinary offsets `0`, `0x8000`,
`0xA000`, and `0xC000`; levels 5 through 9 use packed offsets `0xE020`,
`0xE010`, `0xE008`, `0xE200`, and `0xE100`. The base and first mip row
pitches are `0x400` and `0x200`; later levels use `0x100`. The selected raw
fetch words are `04000002 19F61052 003FE1FF 00A80D10 00000243 19F51A18`,
with source base `0x19F61000` and mip `0x19F51000` addresses.

Run the fixture guard CTest, then prepare and run its focused proof with
`scripts/gpu/test-native-prepared.py native-prepared-bc1-512`. The fixture writes and independently
decodes all ten raw guest levels, including `k8in16` byte pairs, asymmetric
X/Y block colors, biased UVs, and the last blocks. Ten LOD pairs compare
the actual Original capture, a raw independent control decode, and Prepared
native replay on a synthetic 1280x720 canvas with a 512x512 source viewport.
The fullscreen triangle uses UVs `(0,1)`, `(0,-1)`, and `(2,1)` plus
`(0.25/baseWidth, 0.25/baseHeight)`, with screen-Y inversion. The point/clamp
sampler and register LOD range 0 through 9 are defined by the fixture. Presenter
alpha is normalized to 255 with duplicated native
RT0 diagnostic planes. The result contains 40 baseline and changed output
files, ten sensitivity checks, ten rejections for omitted levels, six reachable
callback failure probes, and separate native Completion, SDK copy retirement,
and isolated WARP failure checks. The profile uses opaque RGB565 endpoint 0
with selector 0; interpolation and transparency conformance remain outside
this fixture.

Authored UVs add `0.25/baseExtent` on each axis. The independent CPU oracle
models Original at integer pixel positions after the +0.5 pixel geometry shift.
The guard enumerates every pixel on both axes at every level, requiring the
same interior texel under plain pixel centers, with a fetch epsilon margin of
`1.5/1024` base texels. Coverage of distinct texels and the last block is retained.
The runner rejects stale unbiased metadata and vertex bytes before submission
and validates the recorded bias and guard in the result receipt.

## 256x128 BC1 Prepared fixture

The rectangular BC1 proof registers the `native_prepared_bc1_rect_fixture` case and uses
an unsigned linear, nonstacked `k_DXT1` Texture2D with pitch 8, `k8in16`,
swizzle `0x688`, and nine levels from 256x128 through 1x1. The exact guest
footprint is a `0x4000` byte base range and a `0x4740` byte canonical mip range.
The Legacy mip extent is `0x4000`, and the packed tail starts at `0x4000` with
an `0x740` byte extent. Levels 1 and 2 use ordinary offsets `0` and `0x2000`;
levels 3 through 8 use packed offsets `0x4400`, `0x4200`, `0x4100`, `0x4020`,
`0x4010`, and `0x4008`. The base row pitch is `0x200`; every mip row pitch is
`0x100`. The selected raw fetch words are
`02000002 169F8052 000FE0FF 00A80D10 00000203 169F2A18`, with source base
`0x169F8000` and mip `0x169F2000` addresses.

Run the fixture guard CTest, then prepare and run its focused proof with
`scripts/gpu/test-native-prepared.py native-prepared-bc1-rect`. The fixture writes and independently
decodes all nine raw guest levels, including `k8in16` byte pairs, asymmetric
X/Y block colors, biased UVs, the wide-axis packed tail, and the final
`2x1` and `1x1` levels. Nine LOD pairs compare the actual Original capture,
a raw independent control decode, and Prepared native replay on a synthetic
1280x720 canvas with a 256x128 source viewport. The fullscreen triangle uses
UVs `(0,1)`, `(0,-1)`, and `(2,1)` plus `(0.25/baseWidth, 0.25/baseHeight)`,
with screen-Y inversion. The point/clamp
sampler and register LOD range 0 through 8 are defined by the fixture. Presenter
alpha is normalized to 255 with duplicated native RT0 diagnostic planes. The
result contains 36 baseline and changed output files, nine sensitivity checks,
nine rejections for omitted levels, six reachable callback failure probes, and
separate native Completion, SDK copy retirement, and isolated WARP failure
checks. The profile uses opaque RGB565 endpoint 0 with selector 0;
interpolation and transparency conformance remain outside this fixture.

Authored UVs add `0.25/baseExtent` on each axis. The independent CPU oracle
models Original at integer pixel positions after the +0.5 pixel geometry shift.
The guard enumerates every pixel on both axes at every level, requiring the
same interior texel under plain pixel centers, with a fetch epsilon margin of
`1.5/1024` base texels. Coverage of distinct texels and the last block is retained.
The runner rejects stale unbiased metadata and vertex bytes before submission
and validates the recorded bias and guard in the result receipt.

## 64x64 BC1 Prepared fixture

The square 64x64 BC1 proof registers the `native_prepared_bc1_64_fixture` case and uses
an unsigned linear, nonstacked `k_DXT1` Texture2D with pitch 4, `k8in16`,
swizzle `0x688`, and seven levels from 64x64 through 1x1. The exact guest
footprint is a `0x1000` byte published base range with a `0xF80` byte
canonical base data extent, and a `0x2340` byte canonical mip range. The
Legacy mip extent is `0x2000`, and the packed tail starts at
`0x2000` with a `0x340` byte extent and 8x4 blocks. Level 1 uses ordinary
offset `0`; levels 2 through 6 use packed offsets `0x2020`, `0x2010`,
`0x2008`, `0x2200`, and `0x2100`. Every mip row pitch is `0x100`.
The ordinary mip1 slot has stride `0x2000` and a `0x740` byte data extent.

Run the fixture guard CTest, then prepare and run its focused proof with
`scripts/gpu/test-native-prepared.py native-prepared-bc1-64`. The fixture writes and independently
decodes all seven raw guest levels, including `k8in16` byte pairs, asymmetric
X/Y block colors, biased UVs, the final block, and the packed tail.
Seven LOD pairs compare the actual Original capture, a raw independent
control decode, and Prepared native replay on a synthetic 1280x720 canvas
with a 64x64 source viewport. The fullscreen triangle uses UVs `(0,1)`,
`(0,-1)`, and `(2,1)` plus `(0.25/baseWidth, 0.25/baseHeight)`, with screen-Y
inversion. The point/clamp sampler and
register LOD range 0 through 6 are defined by the fixture. Presenter alpha is
normalized to 255 with duplicated native RT0 diagnostic planes. The result
contains 28 baseline and changed output files, seven sensitivity checks,
seven rejections for omitted levels, six reachable callback failure probes, and
separate native Completion, SDK copy retirement, and isolated WARP failure
checks. The selected source descriptor words are provenance only:
`01000002 1BC73052 0007E03F 00A80D10 00000183 1BC6FA18`, with source base
`0x1BC73000` and mip `0x1BC6F000` addresses. The profile uses opaque RGB565
endpoint 0 with selector 0; interpolation and transparency conformance
remain outside this fixture.

Authored UVs add `0.25/baseExtent` on each axis. The independent CPU oracle
models Original at integer pixel positions after the +0.5 pixel geometry shift.
The guard enumerates every pixel on both axes at every level, requiring the
same interior texel under plain pixel centers, with a fetch epsilon margin of
`1.5/1024` base texels. Coverage of distinct texels and the last block is retained.
The runner rejects stale unbiased metadata and vertex bytes before submission
and validates the recorded bias and guard in the result receipt.

## Packed BC2 Prepared fixture

The bounded BC2 Prepared fixture uses the existing raw-adapter and replay
owners with a 64x64 unsigned linear `k_DXT2_3` texture, pitch 4, `k8in16`,
swizzle `0x688`, and seven levels. Its guest mip ranges are base `0x2000`
bytes and mips `0x4680` bytes, with packed tail offset `0x4000`. Run the
fixture guard CTest, then prepare fresh shader catalogs with
`scripts/gpu/test-native-prepared.py native-prepared-bc2 prepare` and execute the installed
callback proof with its `run` command. The proof compares Original, a raw
independent control decode, and Prepared native replay for every level,
including the full baseline and changed outputs, explicit BC2 alpha through
source-alpha color blending over a known clear color, and point-sampled UV
coverage of the patterned source texels. Both decoders promote the 5/6-bit
color endpoints to UNORM8 before BC2's four-color interpolation; the fixture
uses representable red/blue endpoints with constant green within each block
so derived colors are exact. Wider green interpolation and arbitrary BC2
conformance remain outside this fixture's acceptance. The expected
plane clamps summed RGB before source-alpha blending and applies D3D UNORM8
rounding; presenter alpha is normalized to 255 after capture. The generated
recipe uses the fixture's synthetic shader identities `D1A5A8B7C6E50123` (VS)
and `E2B6B9C8D7F61234` (PS); guest words and DXIL digests remain pinned in the
generated catalogs.

Authored UVs add `0.25/baseExtent` on each axis. The independent CPU oracle
models Original at integer pixel positions after the +0.5 pixel geometry shift.
The guard enumerates every pixel on both axes at every level, requiring the
same interior texel under plain pixel centers, with a fetch epsilon margin of
`1.5/1024` base texels. Coverage of distinct texels and the last block is retained.
The runner rejects stale unbiased metadata and vertex bytes before submission
and validates the recorded bias and guard in the result receipt.

## Rectangular BC2 Prepared fixture

The rectangular proof registers the `native_prepared_bc2_rect_fixture` case beside the
square BC2 fixture. It uses an unsigned linear, nonstacked 128x256 `k_DXT2_3`
texture with pitch 4, `k8in16`, swizzle `0x688`, and nine levels. The exact guest footprint
is a `0x8000` byte base allocation and `0x8E80` byte mip allocation. Levels 1
and 2 use ordinary offsets `0` and `0x4000`; levels 3 through 8 use the packed
tail at `0x8000` with row pitch `512` and offsets `0x8040`, `0x8020`, `0x8010`,
`0x8800`, `0x8400`, and `0x8200`. The final levels retain their `1x2` and
`1x1` shapes. The fixture records the selected source descriptor words
`01000002 168B6053 001FE07F 00A80D10 00000203 168AAA18`; physical addresses
are patched for each isolated allocation. The authored source viewport/scissor
is 128x256 inside the synthetic 1280x720 canvas; the proof records duplicated
Original RT0 into two output planes and requires presenter alpha 255.

Run the rectangular fixture guard CTest, then prepare and run its focused
proof with `scripts/gpu/test-native-prepared.py native-prepared-bc2-rect`. Original, control-native,
and Prepared-native routes decode all nine levels independently. Asymmetric
block patterns and point-sampled UVs cover both axes and the last blocks. The
native sampler keeps min/max LOD at 0/8 so the 1x2 and 1x1 tails are reachable.
The point/clamp state is defined by the fixture for this spatial oracle; it does not
claim a saved sampler descriptor from the selected source words. Color and
four-bit alpha changes remain visible through source-alpha blending.
The fixture retains representable red/blue endpoints and constant green within
each block; wider green interpolation and arbitrary BC2 conformance remain
outside this fixture's acceptance. Presenter alpha is normalized to 255.

Authored UVs add `0.25/baseExtent` on each axis. The independent CPU oracle
models Original at integer pixel positions after the +0.5 pixel geometry shift.
The guard enumerates every pixel on both axes at every level, requiring the
same interior texel under plain pixel centers, with a fetch epsilon margin of
`1.5/1024` base texels. Coverage of distinct texels and the last block is retained.
The runner rejects stale unbiased metadata and vertex bytes before submission
and validates the recorded bias and guard in the result receipt.

## 128x128 BC2 Prepared fixture

The exact 128x128 proof registers the `native_prepared_bc2_128_fixture` case beside the
accepted BC2 fixtures. It uses an unsigned linear, nonstacked 128x128
`k_DXT2_3` texture with pitch 4, `k8in16`, swizzle `0x688`, and eight packed
levels. The guest footprint is a `0x4000` byte base allocation and a
`0x8680` byte mip allocation. Ordinary levels 1 and 2 each occupy a
`0x4000` byte allocation at offsets `0` and `0x4000`; packed levels 3
through 7 use the `0x8000` tail
with row pitch `0x200` and tail extent `0x680`. Their offsets are
`0`, `0`, `0x4000`, `0x8040`, `0x8020`, `0x8010`, `0x8400`, and `0x8200`.
The selected source descriptor words are
`01000002 15AA7053 000FE07F 00A80D10 000001C3 15A9BA18`; physical addresses
are patched for each isolated allocation.

The maintained runner is `scripts/gpu/test-native-prepared.py native-prepared-bc2-128`. Its
schema check requires point sampling, clamp addressing, LOD 0 through 7,
the synthetic 1280x720 canvas, and presenter alpha normalized to 255.
The C++ fixture checks biased UVs and its authored 128x128 source
viewport. The fixture emits
raw bytes in the guest layout, checks every shape and endian pair through an
independent BC2 decode, and compares all eight LOD pairs across Original,
control-native, and Prepared-native routes. CPU combined, color-only, and
alpha-only updates, omission rejection, mutation sensitivity, callback failure
probes, native completion, SDK copy retirement, and isolated WARP failure
remain separate checks in the runner's `prepare`/`run` proof. The registered
no-argument CTest checks the fixture guard.

Authored UVs add `0.25/baseExtent` on each axis. The independent CPU oracle
models Original at integer pixel positions after the +0.5 pixel geometry shift.
The guard enumerates every pixel on both axes at every level, requiring the
same interior texel under plain pixel centers, with a fetch epsilon margin of
`1.5/1024` base texels. Coverage of distinct texels and the last block is retained.
The runner rejects stale unbiased metadata and vertex bytes before submission
and validates the recorded bias and guard in the result receipt.

## BC1/BC2 Prepared form matrix

The `native_prepared_dxt_matrix_fixture` case runs 18 exact unsigned,
nonstacked `k_DXT1` and `k_DXT2_3` forms in one table-driven fixture. The
14 linear rows use `k8in16`; four tiled rows use `kNone`. All use
swizzle `0x688` and minimum LOD 0. `tests/gpu/native_prepared_dxt_matrix_forms.h`
freezes each form's dimensions, fetch pitch, level count, guest offsets, row
pitches and base/mip/tail extents. The fixture checks each of these against
the SDK layout helpers before use.

| Format | Forms |
|---|---|
| BC1 | 8x8 and 512x512, 1024x1024, 2048x2048 and 4096x4096 single level; 32x32 LOD 0..5; 512x256 LOD 0..9; 1024x1024 LOD 0..10 |
| BC2 | 256x256 single level; 128x64 LOD 0..7; 256x128 and 256x256 LOD 0..8; 512x256 and 512x512 LOD 0..9 |

The table above lists linear forms. Tiled rows freeze stored allocation starts,
block pitches and packed block coordinates separately from logical dimensions.
The independent scalar oracle computes macro/micro addresses in compressed
blocks, using log2 bytes per block 3 for BC1 and 4 for BC2. SDK layout and
address helpers are checked separately. Base padding uses `0xD3`; mip padding
uses `0x6B`. Fixture guards reject overlapping levels, changed padding and a
control that collapses storage into tight linear rows.

| Tiled form | Raw pitch | LODs | Base / mip bytes | Baseline/update pairs | Native mutations | Omissions |
|---|---|---|---|---|---|---|
| BC1 128x64 | 4 | 0..7 | 8192 / 16384 | 8 | 8 | 8 |
| BC1 256x128 | 8 | 0..8 | 16384 / 24576 | 9 | 9 | 9 |
| BC2 128x64 | 4 | 0..7 | 16384 / 32768 | 8 | 16 | 8 |
| BC2 32x64, padded pitch 128 texels | 4 | 0 | 16384 / 0 | 1 | 2 | 1 |

Tiled BC2 uses endpoint selectors only. Each tiled BC2 level also runs isolated
color-only and alpha-only CPU updates. Each form retains six callback
negatives, one proof covering native completion and copy retirement, and one
proof for device removal. The complete matrix has 105 level pairs, 112 isolated
CPU update checks,
161 native mutations, 105 omissions and 108 callback negatives.
For 32x64 BC2, rounding logical width to a 32-block tile yields the same
block pitch as the padded descriptor. Its descriptor and pitch are checked
exactly; pixels distinguish tight/linear collapse, not that equivalent rounding.

Each form runs the same proof as the per-form fixtures above:

- every level is decoded from raw guest layout by an independent CPU oracle;
- Original, control-native and Prepared-native outputs are compared at zero
  tolerance for every level pair;
- partial and overlapping CPU updates within the same allocation are applied, with
  separate color-only and alpha-only updates for BC2;
- each level gets a native mutation, and an omission that must reject before
  submission;
- callback negative probes run;
- native completion is checked separately from SDK copy retirement, plus an
  isolated failure due to device removal.

The source viewport is the texture size, capped at 512x512 inside the
synthetic 1280x720 canvas. Every form adds `0.25/baseExtent` on each UV axis
so integer-position samples lie inside texels. The fixture sets `PA_SU_VTX_CNTL`
pixel centers to D3D and pins `half_pixel_offset`, so Original samples at
integer pixel positions, and the oracle models that convention. Both native
vertex shaders consume the recipe's shared half-pixel constants at bytes 292
and 296: `oPos.xy += float2(1/viewportWidth, -1/viewportHeight) * oPos.w`.
This moves geometry +0.5 pixels on both axes with D3D's inverted viewport Y.
The DXT runner supplies that adjustment to its control shader as well as the
translated candidate. Zero offset constants leave the position unchanged.
The guard enumerates every axis pixel and level with a fetch epsilon margin of
`1.5/1024` base texels. Forms up to 512 require identical interior texels under
integer positions and plain pixel centers, with coverage of distinct texels and
the last block. Forms wider than 512 retain their minification design and
require integer-position interior samples: at level 0 their plain pixel
centers select different texels. The 4096x4096 BC1 form (texture 26) detects an omitted offset
as a one-block shift on each axis. Each form receipt records the bias and
whether the two conventions match. Fresh preparation is required; the runner
rejects unbiased metadata and vertex bytes before GPU submission.
At 4096, eight texels map to one pixel; the final integer-position sample is
texel 4088 and skips the final block. Full raw decoding still checks that block.

Run `scripts/gpu/test-native-prepared-dxt-matrix.py prepare`, then `run` with the
printed metadata saved as JSON, then optionally `check-results`. The
registered no-argument CTest checks the fixture guards for all 18 forms.

## BC3 Prepared form matrix

The `native_prepared_bc3_matrix_fixture` case uses one table-driven fixture for ten
unsigned, nonstacked 2D `k_DXT4_5` forms. Eight linear rows use `k8in16`;
two tiled 128x128 rows use `kNone`. All use swizzle `0x688` and minimum
LOD 0. Fetch pitch is in 32-texel units.

| Dimensions | Fetch pitch | Levels | Scene texture IDs |
|---|---|---|---|
| 64x64 | 4 | Packed 0..6 | 100 |
| 128x128 | 4 | Packed 0..7 | 74, 82 |
| 256x128 | 8 | Single 0 | 85 |
| 256x128 | 8 | Packed 0..8 | 80 |
| 256x256 | 8 | Packed 0..8 | 55, 57, 59, 102, 104, 106 |
| 512x256 | 16 | Packed 0..9 | 95, 98 |
| 512x512 | 16 | Single 0 | 61, 62, 149 |
| 512x512 | 16 | Packed 0..9 | 19, 31, 49 |
| 128x128, tiled | 4 | Single 0 | 75 |
| 128x128, tiled | 4 | Packed 0..7 | CPU writer |

The frozen base, mip and tail extents, level offsets and row pitches live in
`tests/gpu/native_prepared_bc3_matrix_forms.h`. The no-argument CTest
`native_prepared_bc3_matrix_fixture` checks them against the SDK helpers.
The tiled scalar oracle addresses compressed blocks with log2 bytes per block 4,
using frozen starts, block pitches and tail coordinates. Tiled rows retain the
DXT matrix's distinct padding sentinels and tight-row corruption guard. The
single row owns 16384 base bytes; the packed row adds 49152 mip bytes with
stored starts 0, 16384 and 32768 for LOD1, LOD2 and the tail.
Every logical texel of every level is independently decoded from raw guest
bytes, using the DXT5 block encoding in
[EXT_texture_compression_s3tc revision 1.6, 15 July 2013](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_compression_s3tc.txt).
The oracle rejects alpha selectors above 1, color selectors above 1, and
color endpoints outside `c0 > c1`. Spatially asymmetric blocks use exact
0/full-scale RGB endpoints. Alpha endpoints remain below 128, so adding two
samples preserves their alpha effect in RGB through source-alpha blending.
Terminal alpha normalization to 255 cannot hide that readout.

The endpoint proof compares Original, control-native and Prepared-native at
zero tolerance for all levels. Each case samples the selected level twice;
adding another level could mask a color-only update through RGB saturation.
It includes combined, color-only and alpha-only
partial and overlapping CPU writes within the same allocation, per-level native mutations
for both block halves, pre-submission omissions and callback negative probes.
Native completion, SDK copy retirement and owner release have separate gates;
the removal control uses an isolated WARP device.

Two additional cases per form use all eight alpha selectors, separately in
`a0 > a1` and `a0 <= a1` modes, and all four color selectors. Their receipts
record `parity_kind = "device_interpolated"` and `oracle_claim = false`.
They require exact Original/control-native/Prepared-native equality and equal
LUIDs for the physical adapters. They generate no CPU expected plane. Hardware
interpolation remains device-dependent; this parity check makes no oracle
conformance claim.

The two tiled rows add nine baseline/update pairs, 18 isolated CPU update
checks, 18 native mutations, nine omissions, 12 callback negatives, two
completion/retirement proofs, two proofs for device removal and four interpolation
parity cases. The complete matrix has 64 pairs, 128 isolated updates, 128
mutations, 64 omissions, 60 callback negatives and 20 interpolation cases.

Both UV axes carry a bias of one quarter of a base texel. The fixture pins
`half_pixel_offset` and guards every pixel, axis and level under integer
positions and plain pixel centers. Both select the same interior texel, with
the fetch epsilon margin of `1.5/1024` base texels, coverage of all texels
and the last block. The control vertex shader applies the translated candidate's
W-scaled half-pixel constants.

Run `scripts/gpu/test-native-prepared-bc3-matrix.py prepare`, then `run --prepared`
with its `prepared.json`, followed by `check-results --output` if needed.
Preparation uses the same tool flags as the BC1/BC2 matrix. The runner checks
both parity kinds and the sampling metadata, and verifies SHA-256 pins for
raw inputs, source shaders, native bytecode and catalogs before GPU submission.
The [cube matrix](#exact-cpu-cube-matrix) covers cube texture 63.

## Offline Vulkan draw comparison

On Windows, a developer build with `REREVVED_VULKAN_INCLUDE_DIR` set to a
directory containing `vulkan/vulkan.h` enables the optional
`native_vulkan_draw_compare` build target.
The path may be supplied through the environment or CMake cache. It requires
Vulkan headers, a Vulkan 1.2 capable driver, D3D12, and DXC for the supplied
SPIR-V modules. Vulkan loads dynamically from the installed Windows driver;
the target does not link a Vulkan import library.

```
native_vulkan_draw_compare.exe [--shaders <VS-hash> <PS-hash>] --inspect <manifest> <shader-catalog> <matching-draw-index> <fresh-output-dir>
native_vulkan_draw_compare.exe [--shaders <VS-hash> <PS-hash>] <manifest> <shader-catalog> <matching-draw-index> <vertex.spv> <pixel.spv> <samples.spv> <fresh-output-dir>
native_vulkan_draw_compare.exe [--shaders <VS-hash> <PS-hash>] --pass <manifest> <shader-catalog> <selected-pass-index> <companion-pass-index> <vertex.spv> <pixel.spv> <samples.spv> <fresh-output-dir>
```

The index is zero-based among ordered admitted draws with the selected guest
shader pair. The default pair is `C3BAB94E67553E12` and `64D8E6A475FFCB2D`.
The leading `--shaders` option also accepts `1EE55F3AB5213177` and
`47F2D46F3B8F1668`, in vertex/pixel order. Hashes must match an approved pair
exactly, in uppercase; mixing pairs is rejected. All modes load one retained frame
through the native capture reader, shader catalog validation, and frame
converter. They verify the selected occurrence against its original event's
admitted recipe. Inspection writes `report.toml` without creating a GPU device.
Output must be fresh and outside the capture directory and shader catalog.
Inspection also records the selected pass's target, explicit clears, attachment
preservation and color-source dependency, resolve count, and complete draw
inventory with producer texture references. This metadata identifies the
surrounding admitted pass; inspection still validates the selected draw and
does not establish Vulkan support for the other draws or the complete pass.

The comparison executes that one recipe on native clear through D3D12 and
Vulkan, retaining its decoded geometry, constants, CPU textures, samplers,
viewport, scissor, blend, rasterizer, depth, and stencil state. It requires an
RGBA8 target with four samples and mask `0x9`; producer texture references and
preloaded sample planes are rejected. It does not execute a producer or the
complete frame. The Vulkan implementation rejects unsupported recipe state
and device capabilities explicitly.

`--pass` is a bounded two-draw comparison. Before starting either GPU oracle, it
validates both complete pass inventories, full clear values, each recipe's link
to its source event, the original shader pair, dependencies between producers
and resolves, and whether Vulkan accepts the recipes. D3D12 replays the selected
pass in the left half
and the required captured companion pass in the right half. Vulkan records the
selected two draws in order in one render pass with shared color/depth
attachments and one clear. The report retains `d3d12.samples.bin`,
`vulkan.samples.bin`, and `vulkan.reset.samples.bin`. The latter clears the
same attachments between draws as a control; its
`reset_between_draws.output_sensitive` result must be true before ordered state
inheritance is considered demonstrated. Depth inheritance is reported
separately and is not inferred from enabled depth testing. The accepted native
fixture uses selected pass index `0` (events `51` and `52`) and companion pass
index `3` (events `131` and `132`).

Supply the specialization's vertex and pixel SPIR-V modules with the binding
layout used by `vulkan_draw.cpp`: vertex constants at set 4/binding 0, pixel
constants at set 4/binding 1, shared constants at set 4/binding 2, one 2D image
at set 0/binding 0, and its sampler at set 3/binding 0. Vertex and pixel
constant payloads are each 4096 bytes; shared constants are 336 bytes.
Compile the sample extraction helper with DXC:

```
dxc -spirv -fspv-target-env=vulkan1.2 -T cs_6_0 -E main tools/vulkan_samples.hlsl -Fo samples.spv
```

The tool admits only the exact independently validated vertex, pixel and
helper module SHA256 identities recorded in `vulkan_compare.cpp`. These pins
establish the supported stages, descriptor layout and Vulkan 1.2 scalar block
layout. A malformed module, a swapped stage or any other digest mismatch fails
before GPU submission. Compiler or shader changes require independent
SPIRV-Tools validation and a reviewed identity update.

In the legacy single-draw comparison, `d3d12.samples.bin` contains two tightly
packed RGBA8 color planes, host samples 0 and 3, followed by two packed depth
planes when depth is enabled. In `--pass`, it contains four color planes in
order: selected (left) samples 0 and 3, then companion (right) samples 0 and 3;
no depth parity is tested for the pass. `vulkan.samples.bin` and
`vulkan.reset.samples.bin` contain the selected left color samples 0 and 3.
Each color plane is `width * height * 4` bytes. The report records source and
decoded payload hashes, exact
recipe state, module hashes, Vulkan adapter capabilities, and differing bytes,
pixels, maximum channel error, and nonclear pixel counts for each color plane.
The existing D3D12 renderer logs its selected adapter vendor and device IDs.
An exact mismatch or an all-clear plane returns nonzero and retains the report
and completed outputs. Passing establishes only this draw's color sample
agreement on native clear; it does not establish parity for the complete frame.

## Recorded sequences

A complete recording from the CPU observer can be replayed in ledger order on one
native renderer:

```toml
gameplay_sequence_schema_version = 1
recording = "recording/session.toml"
shaders = "shaders"
retain_pages = [100242]
```

Paths resolve beneath the recipe directory. `retain_pages` optionally lists
exact guest base pages whose latest native resolve must survive frame
boundaries. These pages may first be produced during recorded loading. The
list comes from the recording's resource dependencies; it does not supply
pixels or invent a missing producer. Legacy recordings with an ordinal of zero for the
producer also retain their declared `producer_page`.

Each successful frame produces a packed RGB10A2 image. The result TOML records
its hash, timings and retained producer event IDs. Resource imports require
matching descriptors and completed resolves on the same renderer. Recorded
CPU-owned texture uses supersede the corresponding native page. This exact
page mapping does not infer overlapping guest allocation aliases or lifetimes.
Unsupported inputs fail with the sequence ordinal and guest frame. Replay
timings include diagnostic readback and are not live gameplay frame rates.

## Native menu frame

The same tool accepts a frame recipe containing two ordered menu halves:

```toml
frame_schema_version = 1
gamma_table = "gamma.bin"

[[halves]]
draws = ["left-scene/frame.toml", "left-ui/frame.toml"]

[[halves]]
draws = ["right-scene/frame.toml", "right-ui/frame.toml"]
```

Each draw uses schema 2 at 640x720 and omits captured initial color and depth
attachments. Each half starts with transparent black and far depth; subsequent
draws retain those attachments on the GPU. After the last draw, a GPU pass
averages guest sample planes with byte half-up rounding and applies the 256
packed gamma entries. The output is one 1280x720 RGB10A2 image, with an
additional `<output>.samples` file containing left sample 0, left sample 1,
right sample 0 and right sample 1 in RGBA8. Frame replays submit at most two
draws per GPU fence and remain bounded diagnostics. Standalone draw replay
submits and waits for its single draw.

## Native draw worker

The native worker keeps bounded pipeline caches for its device. Guest pipeline
keys include the exact shader bytes and effective fixed state. Helper shaders,
root signatures, descriptor heaps and fixed helper constants are also reused;
each draw slot updates its descriptors after its preceding batch fence.
Recorded draws retain their pipeline objects across cache eviction. Resolve
and readback resources are allocated only for a requested
output. One retained output bundle is reused when extent, format, sample count,
sample plane layout, gamma presence and readback capacity are compatible; a
changed key recreates the bundle. Gamma table bytes update the retained gamma
constants for each requested output. Shutdown drains GPU use before releasing
the cache, and a restarted worker creates a fresh device cache. Two draw slots
each retain independent vertex, index and stage-constant uploads, texture/upload
resources and shader-visible descriptor heaps. Buffers grow only when required.
Every draw refreshes its slot after the prior batch fence, with constant
padding and retained tails cleared. Texture rows, constants and little-endian
indices are written directly into mapped upload memory, without temporary
padded input vectors. Texture footprint padding is cleared on every refresh.
Recipe admission bounds each geometry buffer
to 16 MiB and each constant buffer to 64 KiB per draw slot. Each packed texture slot
retains one texture and one upload buffer. Matching extent, mip count, dimension,
array size and format reuse the texture; changed keys recreate it. Upload
capacity grows only when needed
within the existing footprint limit. Every textured draw refreshes pixels,
component mappings and sampler state, transitioning reused textures through
copy-destination state after the prior batch fence. Four color targets and the
optional depth target retain compatible extent, format and sample-count
allocations across frames. Every frame still clears its color targets and
initializes depth at its first depth-enabled draw; supplied initial samples are
reapplied. Readback restores writable attachment states before completion.
Each frame ends with a submitted, completed batch before readback or publication;
an odd final draw submits alone. Only one frame is in flight. Replay timing
counters report CPU preparation/recording and duration of fence waits separately,
plus recorded draws and queue submissions. Time spent preparing texture and draw
uploads is part of recording time. It includes allocating, mapping, padding and
copying uploads, but excludes allocating texture objects, querying footprints
and updating descriptors. Shutdown logs those cumulative
counters; they are not GPU timestamps or physical presentation timing.

The Windows `native_frame_stream` CTest exercises attachment preservation,
next-frame clears, gamma channel mapping and updates, pipeline variant reuse,
identity of shader bytes, submission without readback, terminal renderer failure,
shutdown and restart. It requires a D3D12 device and is enabled with
`$env:REREVVED_TEST_NATIVE_D3D12 = '1'` before running CTest; otherwise it is
reported as skipped.

## Draw recipe schemas

The standalone replay tool configures diagnostics from the debug layer and DRED before
device creation. It owns its queue, command objects, resources, and completion
fence.

Schema 1 admits the captured untextured panel pair. Schema 2 supports translated
triangle draws with one to sixteen packed float4 vertex attributes and up to
32 packed texture bindings. Both use these base tables:

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
Omitting both color inputs in schema 2 initializes the target with
`clear_color_rgba8`. Schema 1 requires both complete captured color planes.
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
sample state, and inputs that exceed limits before creating GPU work. Depth is optional
in schema 2; stencil testing is disabled.

## Textured triangle recipes

For schema 2, set `vertex_attribute_count` to the translated VS input count,
`vertex_stride_bytes` to that count times 16, and provide consecutive float4
`TEXCOORD0` through `TEXCOORD<count-1>` values. Decode guest vertex formats and
endianness before writing the vertex file; leave destination swizzles to the
translated shader. Preserve non-finite guest float payloads; finite checks
apply to interpreted state such as viewports, depth clears and samplers.
Shader hashes must be nonzero provenance identifiers; the
harness does not derive or authenticate them from DXIL bytes.

With `texture_mask = 1`, add both tables below. The texture file contains
tightly packed subresources ordered first by array slice and then by mip,
without padding for upload rows. A legacy table omitting `mip_levels` and
`dimension` remains a single
2D mip. Supported `format` strings are `rgba8_unorm`, `r8_unorm`, `bc1_unorm`,
`bc2_unorm`, `bc3_unorm`, `rgba16_float`, `rg16_float`, and `r32_float`. BC
payloads retain their compressed blocks. `mip_levels` must fit the declared extent. Set
`dimension = "cube"` for a square six-face cube; the default is `"2d"`. Cube
payloads contain all six faces, each with the complete mip chain. `swizzle`
selects RGBA components with values 0 through 3, constant zero with 4, and
constant one with 5.

```toml
[texture]
file = "texture.bin"
format = "rgba8_unorm"
width = 64
height = 64
mip_levels = 1
dimension = "2d"
swizzle = [0, 1, 2, 3]

[sampler]
min_linear = true
mag_linear = true
mip_linear = true
anisotropy = 0
address_u = 3
address_v = 3
address_w = 3
min_lod = 0.0
max_lod = 0.0
mip_bias = 0.0
border = [0.0, 0.0, 0.0, 0.0]
```

Sampler address values use D3D12 numbering: wrap 1, mirror 2, clamp 3, border 4,
and mirror-once 5. Translate enums from foreign capture tools explicitly. Payload
lengths must match the declared dimensions and format exactly; the harness
adds D3D12 copy-footprint padding and keeps upload resources alive until fence
completion.

The optional `anisotropy` uses the Xenos enum: zero disables it, while one
through five select anisotropic filtering at 1x, 2x, 4x, 8x and 16x. Anisotropic
1x remains distinct from the ordinary linear filter. Omission preserves the
disabled default.

For multiple textures, supply one `[[textures]]` table per descriptor in order
and set `texture_mask` to the contiguous low bits for that count. For example,
thirteen textures use `8191`. The translator's ordered `texture_fetches` list
maps guest fetch constants to these packed descriptors. Use one shared
`[sampler]`, or matching ordered `[[samplers]]` tables and a shader translated
with `independent_samplers = true`. Mixing the two sampler forms is rejected.

The shader binding contract is:

| Input | Register | Space |
|---|---|---|
| Vertex constants, at least 256 float4 values | b0 | 4 |
| Pixel constants, at least 224 float4 values | b1 | 4 |
| Shared constants, at least 336 bytes | b2 | 4 |
| Packed Texture2D SRVs, up to 32 | t0 onward | 0 |
| Packed TextureCube SRVs, up to 32 | t0 onward | 2 |
| Shared sampler, or one per texture | s0 onward | 3 |

Both stages must agree on the supplied layout for shared constants. Texture and
sampler arrays must stay within the supplied descriptor count. The native
descriptor table contains one bounded range for each texture register space;
the ranges are disjoint in table order because D3D12 SRVs carry one view
dimension, and the same slot index is used in space 0 and space 2. An unused
dimension receives a null SRV. Volume textures remain unsupported. A successful replay
proves execution and readback;
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

## Owned gameplay scenes

The native renderer reconstructs the two main scene color halves from guest
geometry, translated shaders, copied textures, native black/far clears, depth,
blending, resolve and gamma. It requires a pinned shader catalog and complete
inputs for every main scene draw. It supports the observed indexed triangle
lists and strips, per-texture samplers, 2D and cube mip chains, and vertex
texture fetches at LOD zero. Unsupported state stops the replay. Later fog
composition and HUD passes remain outside the native scene boundary, and final
Xenos presentation pixels are never used as native draw inputs.

To replay a saved input corpus through the same converter, create a recipe
with paths relative to its containing directory:

```toml
gameplay_frame_schema_version = 1
input = "capture/input/manifest.toml"
shaders = "shaders"
```

Run it through the driver's `Replay` stage. The input manifest uses owned
capture schema 2 or batched schema 3, and the shader directory contains `shaders.toml` with pinned
guest-word and DXIL digests and explicit `independent_samplers = true` for
each specialization. The reader verifies payload hashes, path
containment and aggregate byte bounds. Native replay output remains separate
from the immutable capture inputs.

Schema 3 records `complete`, `event_count`, and ordered `event_batches` instead
of an inline `events` array. Each batch contains at most 4,096 events and starts
with a complete register bank. Its descriptor includes `file`, `bytes`,
`sha256`, `ordinal`, `event_count`, `first_event`, and `last_event`. Payload
offsets remain relative to the frame directory. Replay checks the entire frame
boundary, batch order, counts, digests and aggregate input budget before native
submission. The sequential cursor reconstructs one register bank at a time.

Shaders with `SQ_PROGRAM_CNTL.param_gen` enabled also declare
`generated_pixel_parameters = true` in their catalog entry. The native shader
builder replaces the register selected by `SQ_CONTEXT_MISC.param_gen_pos`
with integer pixel coordinates and the front/back face sign for triangle
draws. This lets screen-space effects sample depth at the current pixel.
Catalog loading and draw conversion reject a missing or mismatched declaration.

A vertex shader translated for pretransformed pixel positions declares
`screen_space_position = true` in its catalog entry. The guest state for it
has clipping off and the viewport transform disabled (`PA_CL_CLIP_CNTL`
`0x90000`, `PA_CL_VTE_CNTL` `0x400`), and the translated shader maps pixels to
the full viewport itself. Draw conversion admits that state only with the
flag (`screen_space_position_unpinned`) and the flag only with that state
(`viewport_transform_unexpected`). An absent key means false and the ordinary
`0x80000`/`0x43F` state.
Depth copies select the requested guest sample; they do not average depth
across geometry edges.


## Exact RGBA8/R8 Prepared matrix

The `native_prepared_rgba8_matrix_fixture` case covers ten exact forms. The frozen rows
in `tests/gpu/native_prepared_rgba8_matrix_forms.h` specify layout independently
of row order. The `r8-1024-regions` row uses unsigned linear `k_8`, 1024x1024,
raw pitch 32, `kNone`, swizzle `0x124`, and one 1048576-byte level.

The R8 region fixture initializes every texel, then copies three source LTRB
rectangles to destination XY in order. The first write has a nonzero source
origin and disjoint placement; the second crosses the boundaries of spatial windows;
the third overlaps the second. A scalar CPU model checks the entire backing
after each write. Four retained Prepared snapshots must preserve their bytes
and have distinct owners. Four 512x512 windows cover every texel at 1:1;
each window compares baseline and final Original, Prepared and control output
at zero tolerance.

Four controls with valid content omit each write or reverse the overlapping
pair. Each produces a different output in the first window and runs on both
native routes in every affected window, giving seven control/window cases.
These controls retain complete storage;
the separate checks for missing and truncated levels reject incomplete
payloads before submission. The matrix has 22 baseline/final pairs, 25 native
content controls, 19 level omissions and 44 counted sensitivity checks. The
nine other rows retain their fixtures, expected pixels and per-row counts.

## Exact float Prepared matrix

The `native_prepared_float_matrix_fixture` case covers `r32-float-512-tiled`: signed
`k_32_FLOAT`, 512x512, raw pitch 16, tiled `k8in32`, swizzle `0xB68`, raw signs
`0x55`, `num_format=1`, and LOD0 only. Its frozen layout has 2048-byte rows and
a 1048576-byte base allocation. The no-argument fixture check compares this
layout and every tiled address against the SDK helpers separately from the
oracle.

The oracle reconstructs tiled addresses and reverses each binary32 word without
SDK decoding helpers, comparing all 262144 decoded values bit for bit. Spatial
values are the odd numerators -7 through 7 divided by 8. Two identical samples
with weights `(1/16,1/128,7/128,1/16)` produce `(numerator+16)/64` from `(R,1,1,1)`;
the expected output bytes are 36, 44, 52, 60, 68, 76, 84 and 92. Three
additional controls replace each constant-one selection with zero and compare
the calculated pixels at zero tolerance on both native routes. The control
route consumes independently decoded R32_FLOAT data. Baseline and control
readouts stay strictly inside the byte range and at least 0.140625 byte from
a rounding boundary. Separate payload comparisons exercise nonzero low
mantissa bits without changing the exact rendered dyadic values.

The float matrix has one baseline/final pair, one localized value mutation,
three constant-selection controls and one level omission: five counted
sensitivity checks. A localized CPU update within the same allocation must
produce a fresh Prepared owner while preserving the baseline owner. Biased/gamma
interpretations and missing or truncated full levels are rejected.

Both rows include six callback rejection cases, checks of native completion and
SDK copy retirement, and a device removal failure with zero publications.
Every sample uses a bias of one quarter of a texel with a guard for both pixel-center
conventions. Both native vertex shaders apply `c18.yz` half-pixel adjustment;
each replay frame has one draw and one opaque initial clear. Original parameters
consume a copy of the requested fixture, preserving the fixture for reuse.

Run `scripts/gpu/test-native-prepared-rgba8-matrix.py` or
`scripts/gpu/test-native-prepared-float-matrix.py` with `prepare`, then `run`, using
the same tool flags as the BC1/BC2 matrix. `check-results --output <directory>`
validates exact tuples, pixels, controls and lifetime receipts. CTest registers
`native_prepared_rgba8_matrix_fixture` and `native_prepared_float_matrix_fixture`
for the offline fixture guards.

These synthetic proofs cover descriptor interpretation, storage and update
ownership. They do not establish guest initialization, completeness of captured
region writes, or the float producer's conversion formula.

## Exact CPU cube matrix

The `native_prepared_cube_matrix_fixture` case checks three six-face linear packed cube
forms: 64x64, seven-level RGBA8 with k8in32 and swizzle `0xA0A`
(B,G,R,constant one; texture 38), and signed RGBA16F with k8in16 and identity
mapping (textures 39..41), and 128x128, eight-level unsigned BC3 with k8in16
and identity mapping (texture 63). BC3 uses fetch pitch 4, a 98304-byte base
allocation and a 280192-byte mip allocation. The
Prepared constructor, synchronous live collector and command feed retain the
actual CPU owners before Original host issue. The translated route executes
the admitted command-feed recipes; the control route uses independently
authored TextureCube.SampleLevel HLSL and raw-decoded payloads.

Each LOD draws an atlas with six columns and four rows in a 1280x720 terminal. The
64x64 forms use 64x64 panels and a 384x256 atlas; BC3 uses 128x128 panels
and a 768x512 atlas. Columns select faces and rows project R,G,B,A separately
into RGB.
RGBA8's A row is constant 1.0 regardless of guest alpha bytes. Terminal alpha
remains 255. Expected pixels and all tight subresources (42 for each 64x64
form, 48 for BC3) read the copied raw allocations through frozen tables,
a local scalar IEEE-half decoder and an endpoint-only BC3 decoder, without
SDK layout, endian, conversion or half helpers. Production
helpers run separately as guards against those tables. Xenos face orientation
is corroborated by [revision-pinned Xenia emulator research](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/dxbc_shader_translator_alu.cc#L233);
the oracle shares no executable code with it. This is a synthetic form check,
not acceptance of the saved scene's shader and sampler graph.

A bias of one quarter of a base texel makes integer and half-integer pixel
centers select the same interior texel at every LOD. The guard includes
Original's 1.5/1024 base
texel adjustment, strict major axes, panel bounds and complete texel coverage.
Original pins D3D pixel centers and half_pixel_offset=true; native shared
constants retain the c18.yz half-pixel convention. The first panel draw clears
to opaque black and later panel draws preserve prior panels. VGT_MAX_VTX_INDX
is 71, covering all 72 panel vertices. Finite normal half
values use the exact readout (v+2)/4 and avoid UNORM rounding ties.

The matrix requires zero tolerance for baseline Original, control-native and
Prepared-native output and output updated within the same allocation, including
background and
two identical native RT0 planes. BC3 positives and channel/face mutations
compare to the independent oracle using alpha codes 0/1 with distinct
endpoints and color codes 0/1 with c0 > c1. Its color endpoint components are
0 or maximum, giving exact 0/255 readouts without interpolated rounding.
Both native mutation routes must equal the retained endpoint oracle plane.

The runner checks the following counts for each form:

| Control | RGBA8 | RGBA16F | BC3 |
|---|---:|---:|---:|
| LOD cases, each with baseline and updated output | 7 | 7 | 8 |
| Face/channel coverage groups and isolated mutations | 126 | 168 | 192 |
| Alpha-only guest updates with unchanged output | 7 | 0 | 0 |
| Face perturbations | 42 | 42 | 48 |
| Required owner omissions before recipe construction | 7 | 7 | 8 |
| Missing face/level packing spans | 42 | 42 | 48 |
| Semantic controls | 3 | 3 | 3 |
| Callback negatives per endpoint LOD | 15 | 17 | 15 |
| Interpolated device-parity cases | 0 | 0 | 16 |
| Proofs for device removal, with zero publications | 1 | 1 | 1 |

RGBA8 semantic controls are wrong endian, no swizzle and double swizzle;
RGBA16F controls are wrong endian, clamp-negative and clamp-unit. BC3
controls complement every color selector, complement every alpha selector
and transpose every block's selectors, each checked against the endpoint
oracle. The first
six callback negatives cover incomplete CPU inputs, memexport provenance,
GPU-dirty texture, truncated mip metadata, unreadable packed tail and owned
byte budget. Cube negatives cover normalized depth, raw stack depth,
raw/normalized dimension, non-square extent, face count, final-face tail,
signed binding, exponent adjustment and another format tuple. RGBA16F also
rejects biased and gamma signs. The runner requires 48 Prepared draw receipts
per baseline/update case, four distinct nonzero copy/swap identities, every
negative's rejection and absence of an admitted event, and all completion,
owner retention and retirement flags. Every native output has two equal RT0
planes; channel mutations must leave the other 23 panels unchanged.

BC3 additionally runs each LOD in both alpha modes with all eight alpha
selector codes and all four color codes. The six faces and two phases
collectively sample every code even at 1x1. These 16 cases require exact
Original, control-native and Prepared-native parity on the same physical
device, checked by adapter LUID. They produce no oracle plane and record
`interpolated_oracle_claim = false`. Immutable old/new versions, independent
native completion and SDK copy retirement are checked for every form. An
Original/oracle disagreement retains raw hashes and its first face, LOD,
texel and channel mismatch; a failed guard stops the form.

Build the [adapter](../scripts/native-shader-adapter/README.md) from its
pinned upstream and initialized submodules with a fresh build root. Build the
title test target against the matching SDK install. From the repository root:

```powershell
python scripts/native-shader-adapter/build_adapter.py --source-repo <pinned-upstream> --build-root out/build/cube-adapter-0001 --diagnostics-root out/diagnostics/cube-adapter-0001 --build
python scripts/gpu/test-native-prepared-cube-matrix.py prepare --test-exe <rerevved_prepared_tests.exe> --title-root . --rexglue scripts/rexglue.ps1 --sdk-install <matching-sdk-install> --adapter <fresh-raw_adapter.exe> --dxc <dxc.exe> --dxcompiler <dxcompiler.dll> --dxil <dxil.dll> --shared-header scripts/native-shader-adapter/shader_common.h --output out/diagnostics/cube-prepared-0001
python scripts/gpu/test-native-prepared-cube-matrix.py check --prepared out/diagnostics/cube-prepared-0001/prepared.json
python scripts/gpu/test-native-prepared-cube-matrix.py run --test-exe <rerevved_prepared_tests.exe> --prepared out/diagnostics/cube-prepared-0001/prepared.json --output out/diagnostics/cube-matrix-0001
python scripts/gpu/test-native-prepared-cube-matrix.py check-results --output out/diagnostics/cube-matrix-0001
```

Preparation and run destinations must be absent. `check-results` validates the
strict result schema, including half_pixel_offset, all control counts, receipt
provenance and every retained output plane. Mutation images require several
gigabytes of diagnostic storage. The default CTest invocation runs CPU fixture
guards; the explicit runner performs GPU comparisons and lifetime checks.
