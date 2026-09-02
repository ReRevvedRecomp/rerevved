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

Packages store config, logs, saves, and caches under `Documents\rerevved` on
Windows. Linux uses the XDG documents directory, usually
`~/Documents/rerevved`. The development driver redirects user data, caches, and
logs to ignored `out/` paths instead of packaged player state.

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
path. That path reaches a usable game menu. Some GFx-composed content, including
the title logo and some menu graphics, is absent. This is a runtime correctness
boundary, not a build failure.

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
