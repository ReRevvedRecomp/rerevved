# Scripts

Use [Development](../docs/development.md) for setup, build and verification
steps. This reference explains the scripts and the files they produce. Run the
commands from the repository root.

## Runtime driver

The supported runtime driver is [`rexglue.ps1`](rexglue.ps1):

| Command | Purpose and output |
|---|---|
| `.\scripts\rexglue.ps1 -SelfTest` | Validate required paths and print the configure, codegen, build, and launch commands. It does not run CMake or the game. |
| `.\scripts\rexglue.ps1 -Stage Configure` | Configure against the selected installed SDK. |
| `.\scripts\rexglue.ps1 -Stage Codegen` | Configure, regenerate ignored guest C++ under `generated/default/` when an input changed, and reload its source list. |
| `.\scripts\rexglue.ps1 -Stage Build` | Run staged codegen and build the Release title under `out/build/win-amd64-release/`. |
| `.\scripts\rexglue.ps1 -Stage Launch` | Launch the existing executable with `xenos` and D3D12 ROV, writing the log to `out/rexglue_boot.log`. |
| `.\scripts\rexglue.ps1 -Stage All` | Run the staged build and bounded launch check. |
| `.\scripts\rexglue.ps1 -Stage Replay -ReplayRecipe <recipe> -ReplayOutput <fresh-file>` | Run `native_draw_replay` from the developer build directory (`out/build/win-amd64-developer/`) without launching the game; the output parent directory must exist. |

`-ProbeSeconds <n>` bounds `Launch` and `All`. The default is 20 seconds. A
timeout stops the process started by the driver. `-Interactive` waits for normal
game exit and removes that timeout. `-LaunchArgument --name=value` appends one
validated game argument. Launch creates the ignored `out/rexglue-user/` and
`out/rexglue-cache/` directories.

The driver builds only the release (player) configuration. The recorders, their
console variables and the offline replay tools belong to the developer build
(`cmake --preset win-amd64-developer`; see
[Development](../docs/development.md#developer-build)).
`scripts/gpu/analyze-scene-resources.py` and `scripts/gpu/analyze-native-route-trace.py`
read output recorded by a developer build, and `scripts/package.py` rejects
a build directory configured with `REREVVED_DEVELOPER=ON`.

`-SdkRepo` and `-SdkInstall` select an isolated SDK checkout and installed
package. Omitting them preserves the sibling checkout and package defaults.

## Native shaders and fixture checks

Generate native shader stages with `rexglue.ps1 -Stage Codegen -ShaderRecipe
<recipe.json>`. Add `-SelfTest` to validate the recipe without running tools.
[Native shader artifact generation](../docs/native-shaders.md) defines the
recipe, pinned tools, DXIL/SPIR-V formats and requirement to use a new output directory.

[Native shader adapter inputs](native-shader-adapter/README.md) pin the upstream
source, raw interface, shared layout and adaptation for pixel register LOD. The
builder reconstructs a fresh tool without modifying the upstream checkout.

`python scripts/gpu/test-native-prepared.py <family> <command>` selects a bounded
Prepared or pixel LOD proof. Its commands are `prepare`, `check` and `run`;
`prepare --help` lists the explicit tool inputs.

| Family | Fixture |
|---|---|
| `native-pixel-lod` | Pixel register LOD with eight mips |
| `native-prepared-bc1` | Packed BC1, 128x128 |
| `native-prepared-bc1-256` | Packed BC1, 256x256 |
| `native-prepared-bc1-512` | Packed BC1, 512x512 |
| `native-prepared-bc1-64` | Packed BC1, 64x64 |
| `native-prepared-bc1-rect` | Packed BC1, 256x128 |
| `native-prepared-bc2` | Packed BC2, 64x64 |
| `native-prepared-bc2-128` | Packed BC2, 128x128 |
| `native-prepared-bc2-rect` | Packed BC2, 128x256 |

The separate `scripts/gpu/test-native-prepared-*-matrix.py` validators cover DXT, BC3,
RGBA8/R8, float and cube fixtures and share the preparation code. All runners
drive the optional `rerevved_prepared_tests`
executable: configure with `-DREREVVED_PREPARED_TESTS=ON`, build
`rerevved_prepared_tests` and pass its path as `--test-exe`; the runner calls its
`<family>.emit` and `<family>.run` helper commands
(`rerevved_prepared_tests --helper <command>`; `--helper list` prints them all).
Prepare fresh catalogs, then use the runner's `run` command for issued
original, independent control and candidate D3D12 comparisons. The
`native_pixel_lod_fixture` case checks fixture guards, as
`prepared.native_pixel_lod_fixture` under `ctest -L prepared-research`;
`rerevved_tests` keeps a representative CPU-only case per format family.
GPU comparisons require the prepared catalogs and fresh output path.

`python scripts/gpu/test-native-prepared.py native-prepared-bc1 prepare --help`
lists the tool inputs for the packed BC1 fixture. Pass
`rerevved_prepared_tests` as `--test-exe`;
the runner calls its `native-prepared-bc1.emit` and `native-prepared-bc1.run`
helper commands. Prepare fresh catalogs,
then use `run` to exercise installed Prepared callbacks, eight mip levels,
CPU updates and original/control/native D3D12 comparisons. The
`native_prepared_bc1_fixture` case checks fixture guards. GPU comparisons
require the prepared catalogs and a fresh output path. Presenter captures normalize alpha to 255; the native
comparisons use the same opaque background. This does not validate guest alpha.

## Release packaging

`package.py` stages release binaries, runtime libraries, the player README,
licenses, the bundled default logo asset pack, the bundled first-party mods,
and an empty `game/` directory. It rejects retail and writable runtime file
types. The root CMake `project()` declaration supplies the version.

`assemble_mods.py` builds the mods listed in `package.py`'s `BUNDLED_MODS` from a
`rerevved-mods` checkout at the commit in `rerevved-mods.lock.json`. The mods'
own title lock must be an ancestor of the commit being packaged. Bundled mods
ship without a `mod_order.txt`, so they start disabled.

See [Release process](../docs/releasing.md) for assembly commands, archive
names and the acceptance checklist. Use `package.py --platform`,
`--build-dir`, or `--out-dir` for custom layouts.

## Content manifest generation

`gen-content-manifest.ps1` generates `src/content_manifest.inc` for maintainers.
It records sorted relative paths and exact sizes from `Resource/Common` without
copying retail content. The default input is the ignored `game/` root.

```powershell
.\scripts\gen-content-manifest.ps1
.\scripts\gen-content-manifest.ps1 -ContentRoot <content-root> -OutputPath <path>
```

Regenerate the manifest only when the supported content set intentionally
changes, and review the resulting source diff.
