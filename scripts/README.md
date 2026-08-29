# Scripts

The supported runtime driver is `rexglue.ps1`:

| Command | Purpose and output |
|---|---|
| `.\scripts\rexglue.ps1 -SelfTest` | Validate required paths and print the configure, codegen, build, and launch commands. It does not run CMake or the game. |
| `.\scripts\rexglue.ps1 -Stage Configure` | Configure against the selected installed SDK. |
| `.\scripts\rexglue.ps1 -Stage Codegen` | Configure, regenerate ignored guest C++ under `generated/default/` when an input changed, and reload its source list. |
| `.\scripts\rexglue.ps1 -Stage Build` | Run staged codegen and build the Release title under `out/build/win-amd64-release/`. |
| `.\scripts\rexglue.ps1 -Stage Launch` | Launch the existing executable with `xenos` and D3D12 ROV, writing the log to `out/rexglue_boot.log`. |
| `.\scripts\rexglue.ps1 -Stage All` | Run the staged build and bounded launch check. |

`-ProbeSeconds <n>` bounds `Launch` and `All`. The default is 20 seconds. A
timeout stops the process started by the driver. `-Interactive` waits for normal
game exit and removes that timeout. `-LaunchArgument --name=value` appends one
validated game argument. Launch creates the ignored `out/rexglue-user/` and
`out/rexglue-cache/` directories.

`-SdkRepo` and `-SdkInstall` select an isolated SDK checkout and installed
package. Omitting them preserves the sibling checkout and package defaults.

## Release packaging

`package.py` stages release binaries, runtime libraries, the player README,
licenses, and an empty `game/` directory.
It rejects retail and writable runtime file types. The root CMake `project()`
declaration supplies the version.

After a Release build, package the native platform from the repository root:

```powershell
python .\scripts\package.py
```

Windows produces `out/rerevved-v<version>-windows-x64.zip`. A Linux build host
produces `out/rerevved-v<version>-linux-x64.tar.gz`. Linux remains experimental.
Use `--platform`, `--build-dir`, or `--out-dir` for custom layouts. See the
[release process](../docs/releasing.md) for the checklist.

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

## Trace analyzer

ReXGlue frame traces use `<title-id>_<counter>.xtr`, while stream traces use
`<title-id>_stream.xtr`. Use the analyzer on an existing .xtr file:

    python .\scripts\analyze-rexglue-trace.py .\545407E5_123.xtr --summary-only
    python .\scripts\analyze-rexglue-trace.py .\545407E5_123.xtr --minimum-read 0x2000 --frame 3

The analyzer prints trace metadata, draw inventories, texture fetches, and
memory reads at least `0x1000` bytes by default. `--summary-only` suppresses
individual draw records. `--minimum-read` and `--frame` narrow the report. It rejects
unsupported versions, truncated packets, and invalid compressed payloads.

Capture the current foreground window as an ignored BMP. The script temporarily
sets the window topmost and removes that temporary state even when capture fails:

    .\scripts\capture-window.ps1 -Out .\out\window_topmost_capture.bmp

Compare two BMPs without changing them. Crops are x,y,w,h and must have equal
dimensions:

    .\scripts\compare-frames.ps1 .\baseline.bmp .\out\window_topmost_capture.bmp -MeanAbsMax 0.01 -Ssim 0.99

The command reports `mean_abs` and `ssim`. Thresholds apply only to the supplied
images and crop regions.

## Cleanup

`clean-logs.ps1` deletes only repository root `*.log`, `out/*.log`, and the
known `out/*.txt` dump names. It does not recurse and does not remove `.xtr` or
BMP files. Review the targets first, then delete only stale files:

    .\scripts\clean-logs.ps1 -WhatIf
    .\scripts\clean-logs.ps1 -Days 7

Do not replace the dry run with an unrestricted recursive delete. The analyzer
does not prove that GFx pixels were displayed. Captures and comparisons can
document visible output, but they do not by themselves prove that missing
content was recovered or that a natural state transition completed.
See the [ReXGlue runtime contract](../docs/rexglue-runtime.md) and
[evidence-and-claims guidance](../docs/ai_agents/evidence-and-claims.md).
