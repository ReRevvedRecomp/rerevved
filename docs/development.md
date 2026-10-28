# Development

This guide covers Windows x64 and macOS arm64 builds, checks, and
[pull requests](#pull-requests). No retail inputs or generated guest code
belong in the repository.

## Prerequisites

- LLVM Clang 22.1.4.
- Visual Studio 2022 17.14 C++ Build Tools (MSVC 14.44) and the Windows SDK,
  including `dxc.exe`. The native renderer proves its memory accounting only
  for this compiler and STL pair; other versions build, but its draw stream
  refuses to start.
- CMake 3.25 or newer and Ninja.
- Python 3.11 or newer and PowerShell.
- The title's tracked submodules, initialized recursively.
- An installed ReRevved SDK matching [`rexglue-sdk.lock.json`](../rexglue-sdk.lock.json).
- Your legally obtained base `game/default.xex`, version 1.3
  `game/default.xexp`, and the matching `game/Resource/` files.

The title compiles as C++23. The Windows shader build prefers the Windows SDK's
`dxc.exe`; another compiler path must have `dxil.dll` beside it.

## Prepare the SDK

Keep the SDK as a sibling checkout, not a title submodule:

```text
<workspace>/
|-- rerevved/
`-- rerevved-sdk/
```

Check out the exact SDK lock commit, initialize its submodules, and make the
upstream `v*` tags available for version derivation. Build and install it using
the [SDK documentation](https://github.com/ReRevvedRecomp/rerevved-sdk).
The title driver checks the checkout and installed package against the lock;
it does not build, update, or modify the SDK checkout.

## Build without launching

Run from the title repository root:

```powershell
.\scripts\rexglue.ps1 -SelfTest
.\scripts\rexglue.ps1 -Stage Codegen
.\scripts\rexglue.ps1 -Stage Build
```

`-SelfTest` validates paths and prints commands without running CMake or the
game. Both Codegen and Build run code generation with dependency tracking between
two configure passes; Build then compiles. The executable is
`out/build/win-amd64-release/rerevved.exe` and generated guest code is under
`generated/default/`.

`-SdkRepo` and `-SdkInstall` select another matching SDK checkout and install.
The default install is `rerevved-sdk/out/install/win-amd64`. `-LlvmBin` selects
the LLVM directory; its default is `C:\Program Files\LLVM\bin`.

Do not omit `-Stage` when you only want a build: the driver's default is
`All`, which also launches the game.

## macOS build

Use an Apple Silicon Mac with a Vulkan-capable
GPU and macOS 13.3 or newer. It uses Xenos over Vulkan/MoltenVK.
Native rendering and Plume require Windows.

Install and select Xcode 16.4, then install the build dependencies:

```sh
sudo xcode-select --switch /Applications/Xcode_16.4.app/Contents/Developer
brew install cmake ninja vulkan-loader molten-vk autoconf pkgconf
```

Build the SDK commit in `rexglue-sdk.lock.json` with its `mac-arm64` preset,
deployment target 13.3 and `CMAKE_OSX_ARCHITECTURES=arm64`, then install
Release. Use CMake directly on macOS. From the title root:

```sh
cmake --preset mac-arm64-release -Drexglue_DIR=../rerevved-sdk/out/install/mac-arm64/lib/cmake/rexglue
cmake --build --preset mac-arm64-release --target rerevved_codegen
cmake --preset mac-arm64-release -Drexglue_DIR=../rerevved-sdk/out/install/mac-arm64/lib/cmake/rexglue
cmake --build --preset mac-arm64-release --parallel 2
ctest --preset mac-arm64-release
```

The executable is `out/build/mac-arm64-release/rerevved`.
`mac-arm64-developer` adds recorders in a separate build directory.
See [Releasing](releasing.md) to package the archive.

## Developer build

The player build carries no recorders or offline tools. `REREVVED_DEVELOPER`
(CMake option, default `OFF`; the sources see it as `REREVVED_DEVELOPER=0|1`)
adds them: the scene resource, payload and target capture, the route and route
effect traces, the label adapter capture, the shader harvest, the Xenos frame
reference, the `Native frame timing:` line with its guest API call counts, the
frame cadence trace, the presentation trace and the synthetic lifecycle script
of the full app, the draw effect probe, the derive, effect, exit and observe checks of the draw,
Clear, Resolve, SetPredication, window writer and gamma paths with the consume
counters of that line, their console variables (`native_scene_capture*`,
`native_route_trace`, `native_shader_harvest*`, `native_xenos_frame_reference*`,
`native_guest_adapter_output`, `native_draw_effect_probe`,
`native_clear_derive_check`, `native_clear_effect_check`,
`native_resolve_derive_check`, `native_resolve_effect_check`,
`native_lowerer_exit_check`, `native_window_writer_exit_check`,
`native_set_predication_exit_check` and `native_gamma_observe_check`, plus
`native_frame_timing`, `native_cadence_trace`, `native_present_trace` and
`native_present_script`) and tests,
and the `native_draw_replay` and `native_vulkan_draw_compare` executables. The
runtime hooks, resource mirror, ring stubs and consumes are the same in both
builds; only the recorders and comparisons around them differ. In the player build a draw, clear or resolve consume never waits
for a recorder; in a developer build it still runs Original while a harvest,
scene capture or route trace is on.

Configure a developer build in its own directory so the default one stays a
player build. The Windows preset also enables the Prepared research executable:

```powershell
cmake --preset win-amd64-developer "-Drexglue_DIR=<sdk-install>/lib/cmake/rexglue"
cmake --build --preset win-amd64-developer --parallel 4
ctest --preset win-amd64-developer
```

`linux-amd64-developer` and `mac-arm64-developer` are the Linux and macOS
presets. All build directories share
`generated/default/`, so configure and build them one after the other, never
together. `scripts/package.py` rejects a build directory configured with
`REREVVED_DEVELOPER=ON`. The Python analyzers
(`scripts/gpu/analyze-scene-resources.py`, `scripts/gpu/analyze-native-route-trace.py`)
need no build but read what a developer build records.

## Checks and tests

From the repository root in `pwsh`, install the verification tools and run
the repository checks:

```powershell
python -m pip install -r scripts/requirements-dev.txt
$settings = Import-PowerShellDataFile ./scripts/powershell-checks.psd1
Install-Module -Name $settings.Module.ModuleName `
    -RequiredVersion $settings.Module.RequiredVersion `
    -Repository PSGallery -Scope CurrentUser -Force -ErrorAction Stop
.\scripts\verify.ps1
```

The script checks JSON/TOML parsing, clang-format 22, Ruff, Python unit tests,
lock file structure, PowerShell syntax, formatting and selected diagnostics,
Markdown file links, ASCII, and forbidden tracked paths. Its link check does
not validate heading anchors; inspect those separately.

PowerShell checks use the pinned module and settings in
[`scripts/powershell-checks.psd1`](../scripts/powershell-checks.psd1).
To format an authored PowerShell file locally:

```powershell
Import-Module -FullyQualifiedName $settings.Module
$path = './scripts/verify.ps1'
$source = [IO.File]::ReadAllText($path)
$formatted = Invoke-Formatter -ScriptDefinition $source -Settings $settings.Formatter
[IO.File]::WriteAllText($path, $formatted)
```

After a native build with `BUILD_TESTING` enabled, run its registered CTest
suite:

```powershell
ctest --test-dir out/build/win-amd64-release -C Release -j 2 --output-on-failure
```

Keep `-j 2`: graphics cases share one device lock, and wider runs only add
load on the build machine. The native tests (renderer, presentation, adapter, hook,
state, completion and recorder tests) are Catch2 cases in the single
`rerevved_tests` executable. Child processes that exit on fatal errors, recorder modes and
fixture emitters run as `rerevved_tests --helper <name>`; `--helper list` prints
the names. CTest discovers the cases after the executable links; each
registration runs one case by its exact name, so `ctest -R <name>` selects one.
To run a case directly, pass its name or a tag:

```powershell
out\build\win-amd64-release\rerevved_tests.exe "host_settings"
out\build\win-amd64-release\rerevved_tests.exe "[local-fast]"
```

Cases tagged `[ctest]` are the automatic registrations. Hidden
`[renderer-research]` cases (frame-stream, assembler and adapter harvests,
captures and proofs of draw cost, movie references and shader literal captures)
stay out of the default registration and run directly by exact name or
`"[renderer-research]"`. To schedule them through CTest, with the same device
lock, timeouts and a `renderer-research` label, configure with
`-DREREVVED_REGISTER_RESEARCH_TESTS=ON`. The `[release-smoke]` cases and
`native_app_hidden` carry the `release-smoke` label, so
`ctest -L release-smoke -j 2` runs the native release smoke set.
`native_app_hidden` runs the separate `rerevved_native_app_smoke` executable,
which drives the production presentation session in a hidden window through
resize, minimize and restore, an overlay compose and clear, and a double close,
and needs `REREVVED_NATIVE_PRESENT_CATALOG`. The developer build adds
`native_app_hidden_trace`, the lifecycle through the full app and its trace
file, including the SDK settings dialog and input listeners. The `native_gpu`,
`run_serial` and `timeout-<seconds>` tags become CTest scheduling properties,
and a misspelled scheduling tag stops CTest with an error. A case that skips
because an optional input is missing reports a CTest skip; invalid input
fails. The CPU-only subset carries the `[local-fast]` tag and the `local-fast`
CTest label:

```powershell
ctest --preset win-amd64-local-fast
```

Fixture emitters for the renderer proofs are helper commands of the same
executable, run as `rerevved_tests.exe --helper <family>.<command> <arguments>`.
Cases tagged `[manual]` are hidden from the automatic registrations and run only
through a helper.

The Prepared format matrices (BC1, BC2, BC3, DXT, RGBA8/R8, float, cube and the
register LOD fixture), their fixture guards and their GPU checks comparing
Original with native output live in the optional `rerevved_prepared_tests` executable. `rerevved_tests`
keeps one CPU-only representative per format family: `native_prepared_bc1_fixture`
(BC), `native_prepared_rgba8_matrix_fixture` (`rgba8-8-packed-base`),
`native_prepared_float_matrix_fixture` (`r32-float-512-tiled`),
`native_prepared_cube_matrix_fixture` (`rgba8-cube-64-packed`) and
`native_pixel_lod_fixture`. Build the research executable explicitly:

```powershell
cmake --preset win-amd64-release -DREREVVED_PREPARED_TESTS=ON
cmake --build --preset win-amd64-release --target rerevved_prepared_tests --parallel 4
ctest --test-dir out/build/win-amd64-release -C Release -L prepared-research
```

The `win-amd64-developer` preset enables it already.

CTest registers the executable's 14 cases that check fixture guards as
`prepared.<case>` with the `prepared-research` label; they run without a GPU.
Its fixture emitters and GPU proofs run as `rerevved_prepared_tests.exe --helper
<family>.<command> <arguments>` (for example `--helper native-prepared-bc1.emit
--emit-fixture <fresh-dir>`; `--helper list` prints every name). The
`scripts/gpu/test-native-prepared.py <family> <command>` runner and the separate
`scripts/gpu/test-native-prepared-*-matrix.py` validators call those helpers; pass
the path of `rerevved_prepared_tests.exe` as
`--test-exe`. Configure with `-DREREVVED_PREPARED_TESTS=OFF` to return to the
default build.

Some tests require graphics or game inputs. State which checks ran, skipped,
or failed; a successful build is not proof of gameplay or visual correctness.
For documentation changes, check relative links and anchors and run
`git diff --check` before review.

## Launch for a bounded check

Only launch when your task calls for runtime verification:

```powershell
.\scripts\rexglue.ps1 -Stage Launch -ProbeSeconds 30
.\scripts\rexglue.ps1 -Stage Launch -Interactive
```

Add `-Developer` to either command to use the developer preset.

The first command stops the launched process after the probe timeout. The
second waits for normal exit. The driver supplies the repository's `game/`
root and redirects development user data, cache, and logs under ignored
`out/` paths. See [Scripts](../scripts/README.md) for path overrides and other
stages, and [Evidence and claims](ai_agents/evidence-and-claims.md) for what a
bounded run establishes.

## CI and packaging

The Checks workflow runs on pull requests, pushes to `main` or `develop`, and
manual dispatch. CI build jobs run only for pull requests and manual dispatch,
and skip pull requests from forks. Each platform job builds, tests and
packages the player configuration only; build and test the developer
configuration locally. The separate release workflow builds and publishes from
`main`; it is not an acceptance check for pull requests.

See [Releasing](releasing.md) for mod assembly, package contents, publication
conditions, and the standalone archive acceptance checklist. CI does not
verify gameplay on Linux x64 or macOS arm64.

## Pull requests

Open pull requests against `develop`. Do not submit retail game files, extracted
assets, generated guest code, build output, credentials, or private working
material.

Use the SDK revision in [`rexglue-sdk.lock.json`](../rexglue-sdk.lock.json).
SDK changes belong in the SDK repository and need their own checks before the
title repins. Preserve guest control flow unless a feature needs a documented
compatibility hook. Keep diagnostics off by default. Change the inputs to generated code, not the generated files.

Add a focused regression check for changed behavior. For documentation, check
relative links and heading anchors and run `git diff --check`; a build does not
check prose. In the pull request, explain the new behavior, its evidence, the
checks run, and what they do not cover. Compiling and packaging do not prove
gameplay or graphics are correct.

Commit subjects use `type: imperative summary`, at most 50 characters, with no
body or parentheses, a lowercase type and a lowercase opening verb. Types
include `build`, `chore`, `ci`, `docs`, `feat`, `fix`, `resources`, `test`, and
`tools`.
