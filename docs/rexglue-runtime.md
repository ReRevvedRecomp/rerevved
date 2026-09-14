# ReXGlue runtime

ReRevved uses ReXGlue as its only runtime and GPU foundation. The
root CMake project consumes the installed package from the sibling
`rerevved-sdk` checkout. The driver never modifies that checkout.

## Maintained baseline

The maintained fork provides this title's runtime compatibility support.
[`rexglue-sdk.lock.json`](../rexglue-sdk.lock.json) pins its repository, commit,
installed package version, compatible SDK interface, and `xenos` GPU plugin.
Update the lock instead of copying pin values into documentation.

The guest baseline is the retail `default.xex` at version 0.0.0.2 plus the
matching `game/default.xexp` title update. ReXGlue applies that patch during
code generation and launch, producing the maintained executable version
0.0.3.2 (game version 1.3, title ID `545407E5`, media ID `7DC1293B`). The
matching version 1.3 files under `game/Resource/Common/` are also required.
All of these retail inputs remain ignored and are not distributed by this
repository.

The accepted package configures, generates guest code, and builds the Release
title. The deployment contains both `rexruntime.dll` and `rexgpu-xenos.dll`.

## Game content and writable data

A package contains no retail content. On first run, select a legally owned game
ISO or extracted content root. An ISO is extracted into `game/` beside the
executable. Select the version 1.3 patch if it is not beside the base executable.
Before launch, deep validation checks the base executable, patch, and 88-file
Resource manifest. The selected root is stored as `game_data_root` in the
`rerevved.toml` file in the user data folder and reused on later runs.

The development driver bypasses selection with
`--game_data_root=<repo>/game`. Other launches can use
`--game_data_root=<path>` for the current session.

Packages store config, logs, saves, and caches under `Documents\My Games\ReRevved` on
Windows. On Linux, logs are under `$XDG_DATA_HOME/rerevved/logs`, or
`~/.local/share/rerevved/logs` when `XDG_DATA_HOME` is not set. Other writable
state uses the same `rerevved` data root. The development driver redirects user
data, caches, and logs to ignored `out/` paths instead of packaged player state.

## Storage profiles

ReXGlue calls the shared user-data root `B` and the active profile root `P`.
The established default is unchanged: a bare launch, an omitted profile, or
`--profile=default` selects the default environment with `P=B`. On Windows,
`B` is normally `Documents\My Games\ReRevved`; on Linux, it is normally
`$XDG_DATA_HOME/rerevved` or `~/.local/share/rerevved` when
`XDG_DATA_HOME` is not set.

Use `--profile=<id>` to select a named profile:

```text
rerevved.exe --profile=alpha
```

The active root for this launch is `P=B/profiles/<id>`. Profile IDs are
lowercase path-safe names: 1 to 32 characters, beginning with a lowercase
letter or digit, followed by lowercase letters, digits, `_`, or `-`.
`default` and Windows device names are reserved. Profile selection is
restart-only and session-only: it is read during startup, applies to that
process, and is not written to `rerevved.toml`. Stop and restart ReRevved to
switch profiles.

The active profile owns its writable state. In particular, config is
`P/rerevved.toml`, default logs are under `P/logs`, the default cache is
`P/cache`, save content is under `P`'s per-user and per-title content roots,
achievements are under `P/achievements`, and the native mod and asset
override orders are `P/mod_order.txt` and `P/asset_order.txt`. This keeps
config, default logs, default cache, saves, achievements, and package
selection isolated between named profiles.
Explicit `log_file` and `cache_root` overrides retain their existing meanings
and may select locations outside `P`.

F1 discovers packages installed under `mods/` beside the executable and stages
enable, disable, and load-order changes for the active profile. Apply writes the
profile loadout atomically; native changes take effect after restarting the
game. Missing or invalid enabled packages block all native mods for that launch
while leaving the game and F1 repair flow available.

Marketplace content is shared. Marketplace packages and their headers remain
in the base root's XUID-zero namespace, such as
`B/0000000000000000/<title-id>/00000002` and
`B/0000000000000000/<title-id>/Headers/00000002`; every profile sees that
content. Other title content and headers are profile-local under `P`.

To create a new named profile from the default environment, pass both options
on the first launch of an otherwise absent profile:

```text
rerevved.exe --profile=alpha --profile_copy_from_default=true
```

`--profile_copy_from_default=true` is a one-shot, session-only request. It
requires a named profile and an absent target. The SDK copies config,
ordinary title content and headers, title profile data, the title's
achievement record, and both package order files when present. An empty
saved order stays empty. It does not
copy marketplace content, logs, cache, unrelated files, or another profile.
After the copy succeeds, launch with `--profile=alpha` alone. An existing
profile is never overwritten by this operation.

To roll back or switch environments, omit `--profile` or select
`--profile=default` for the default `P=B` environment, or select a prior
named profile with `--profile=<id>`. Each profile's state remains in its own
root while it is not selected.

## Display settings

Press F4 in a packaged or development build to open the settings overlay. The
`resolution` preset sizes the window, while Civilization Revolution still
renders a fixed 1280x720 guest image. Use `resolution_scale` for higher quality
rendering:
it accepts 1 through 8, with 1 as native quality and 2 as a validated higher
quality setting. `resolution_scale` requires a restart. The `fullscreen` setting
also takes effect on restart. Select **Save to config** in the overlay to persist
these settings in `rerevved.toml`.

## Combat settings

The `combat_speed` setting accepts `normal` or `fast` and defaults to `normal`.
`fast` uses the guest's native faster combat presentation pace while preserving
resolution and completion.

## Runtime boundary

The supported launch uses the `xenos` plugin and the D3D12 ROV render-target
path. That path reaches a usable game menu. Some GFx-composed menu graphics
remain absent. The main-menu logo is supplied through the bundled
`rerevved-logo` asset override. This is a runtime correctness boundary, not a
build failure.

The settled guest behavior belongs in [`scaleform-gfx.md`](scaleform-gfx.md).

## Presentation contract

The guest and ReXGlue both report a 1280x720 presentation. The front buffer
texture is a tiled 1280x720 `k_8_8_8_8` texture. An intermediate D3D viewport
may differ from scanout dimensions. The measured ReXGlue fetch remains
1280x720.

The title passes `VdSwap` a kernel output command buffer. The kernel writes
fetch constant 0 and a `PM4_XE_SWAP` packet, and ReXGlue executes that packet.
The D3D12 path is:

1. Copy-mode `IssueDraw` calls `IssueCopy`.
2. The render-target cache derives the resolve destination, copies EDRAM into
   shared memory, and marks the destination range as GPU-written.
3. Shared-memory watches invalidate overlapping texture data.
4. `RequestSwapTexture` decodes fetch constant 0 and `LoadTextureData` refreshes
   the tiled texture from shared memory.
5. `IssueSwap` applies the selected gamma ramp and submits the guest output to
   the presenter.

A GPU resolve must expose its destination to overlapping texture aliases. CPU
writeback is needed only when guest CPU code must read the result. ReXGlue's
graphics and `VdSwap` implementations retain their Xenia lineage.

## Draw capture

Start Xenos with `--d3d12_capture_draw=<absolute-output-directory>` pointing to
a fresh directory. At the desired scene, create an empty file named `arm` in
that directory. The SDK checks this marker at swap and captures the next
eligible draw. Capture is disabled when the option is empty.

The current capture supports untextured, rasterized triangle lists on the
D3D12 ROV path at resolution scale 1. It excludes tessellation, memory export,
and converted index buffers. Shader-used vertex ranges and original indices
are copied from GPU shared memory after residency; EDRAM is copied immediately
before and after the selected draw. Registers and shader instructions are
stored as little-endian dwords. The version-1 manifest is marked complete only
after GPU completion and successful file writes.

Inspect a completed snapshot with:

```powershell
cd <repo>; python scripts/summarize-draw-capture.py out/diagnostics/<capture>
```

The summary requires a completed manifest, checks each declared file size, and
records SHA-256 digests for the registers, shaders, geometry, and GPU readbacks.
It also counts changed EDRAM bytes between the snapshots before and after the
draw. These checks establish file consistency and a byte difference, not native
replay or visible parity. Captures contain retail graphics data and belong in
ignored output directories.

## Native offscreen draw replay

After building, replay a decoded draw with the headless native D3D12 harness:

```powershell
cd <repo>; .\scripts\rexglue.ps1 -Stage Replay -ReplayRecipe out/diagnostics/<capture>/replay-input/native-replay.toml -ReplayOutput out/diagnostics/<capture>/native-samples.rgba
```

The [recipe guide](../tools/native-draw-replay/README.md) defines the admitted
shader and resource subset. A recipe supplies executable shader stages, decoded
vertex attributes and indices, constants, render state, and the initial color
samples. Replay runs without booting guest code and writes its sample readback
after GPU completion. The output path must be fresh and its directory must
already exist. Compare the resulting sample planes with the captured draw's
output; the background supplied to replay remains captured data.

## Guest frame capture

The D3D12 frame capture uses [RenderDoc](https://github.com/baldurk/renderdoc)
for GPU resources and commands, with a companion SDK journal for original
Xenos registers, shader microcode, and guest draw/copy ordering. It is separate
from the single-draw replay and does not require native rendering.

Launch through the driver with RenderDoc injected before device creation:

```powershell
cd <repo>; .\scripts\rexglue.ps1 -Stage Launch -Interactive -RenderDocCmd <renderdoc-directory>/renderdoccmd.exe -LaunchArgument '--d3d12_capture_frame=<absolute-capture-directory>'
```

Use a fresh capture directory. At the desired scene, create its empty `arm`
file. The SDK starts capture after a completed guest swap and ends after the
next guest swap, so the boundary follows guest rendering rather than the
independent host presentation loop. Capture remains disabled without the option
and requires RenderDoc to be attached. A completed bundle contains the `.rdc`
and the guest-state journal; failures and bounds violations are explicit.
Publication requires the guest queue's ending fence to complete. Manifest
completion certifies the capture envelope and GPU completion; per-event results
and the validator's `failed_guest_events` retain failed guest operations.

Validate the journal, inventory, and input digests before analyzing the GPU
capture:

```powershell
cd <repo>; python .\scripts\validate-frame-capture.py <capture-directory>
```

This validation checks the capture envelope. RenderDoc replay and matching the
journal's issued-draw markers to GPU actions establish the GPU dependency set.
The offline inspector uses RenderDoc 1.46's embedded Python and an isolated
configuration directory:

```powershell
cd <repo>; .\scripts\run-renderdoc-script.ps1 -RenderDocRoot <renderdoc-directory> -Capture <capture.rdc> -Markers <capture-directory>/journal.json -Output <fresh-report.json>
```

The report includes GPU actions, resource and view formats, sampler state,
resource usages, shader bindings, and the issued guest draw joins. It inspects
copy and compute actions as well as guest markers. Missing required markers or
replay failures produce a failed report. Resource inventories can include unused
objects; dependency analysis combines recorded usages with per-event descriptor
accesses, which include dynamically selected textures. A shader's texture mask
does not prove that a fragment sampled a texture; the report records draws with
no reported dynamic texture access separately.

`-RenderDocCmd` requires an interactive Launch stage. The driver reports the
capture launcher's exit code because RenderDoc does not propagate the game's
exit code. Close the game normally after the capture completes. Captures contain
retail graphics data and stay in ignored output directories.

## Build and validation

The supported Windows build requires LLVM 18 or newer, Visual Studio 2022 Build
Tools with the C++ workload and Windows SDK, CMake 3.25 or newer, Ninja, and
Python 3.11 or newer. Keep the maintained SDK as a sibling checkout:

```text
<workspace>/
|-- rerevved/
`-- rerevved-sdk/
```

```powershell
cd <repo>; .\scripts\rexglue.ps1 -SelfTest
cd <repo>; .\scripts\rexglue.ps1 -Stage Codegen
cd <repo>; .\scripts\rexglue.ps1 -Stage Build
cd <repo>; .\scripts\verify.ps1
```

The driver runs from the repository root, locates Visual Studio 2022, checks the
selected SDK checkout and installed package against the lock, and consumes only
that package. `-SdkRepo` and `-SdkInstall` select an isolated SDK; their defaults
remain the sibling checkout and `out/install/win-amd64`. Generated guest code lives
under `generated/default/`. Build output, development user data, caches, and
logs are ignored.

A successful build proves compilation only. Runtime claims require a captured
milestone or another independently reviewable result. Use `-ProbeSeconds <n>`
to bound a smoke test; `-Interactive` waits for normal game exit. See the
[evidence-and-claims guidance](ai_agents/evidence-and-claims.md).

## Dependency and scope rules

- Keep the SDK as a sibling checkout, never a submodule.
- Update the SDK lock whenever the accepted fork commit changes.
- Keep `game/default.xex`, `game/default.xexp`, and extracted version 1.3
  assets ignored.
- Begin visual validation with D3D12 ROV until another path is proven.
- Treat import coverage and code generation as feasibility signals, not runtime
  correctness.
- Preserve ReXGlue's BSD-3-Clause notices in combined distributions.
