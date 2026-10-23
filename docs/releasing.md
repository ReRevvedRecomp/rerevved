# Release process

Windows x64 is the supported player platform; the workflow also produces an experimental Linux archive. This runbook describes building and accepting a standalone candidate.
Complete the manual acceptance checklist before landing a release version on
`main`; the workflow does not perform that player acceptance test.

A push to `main`, or a manual `workflow_dispatch` run on `main`, publishes
the CMake project version when no `v<version>` tag exists. The release
workflow builds the Windows and Linux archives, creates the tag at the
selected commit, and publishes the GitHub release. An already tagged version
publishes nothing. Releases publish as full releases, never as prereleases.

`main` carries releases, `develop` is the integration branch, and native
renderer work uses `native-rendering` with pull requests into `develop`.

## Set the version

1. Update the version in the root `project(rerevved VERSION ...)` declaration
   in [`CMakeLists.txt`](../CMakeLists.txt). Landing that change on `main`
   releases it as `v<version>`.
2. After building and packaging, confirm that the archive name uses the project
   version. The window title and Windows executable tooltip intentionally omit it.

## Build the release candidate

1. Follow [Development](development.md#build-without-launching) to build a
   Release candidate from a fresh `out/build/win-amd64-release/` directory and
   run its checks and tests.

2. Assemble the bundled mods from a `rerevved-mods` checkout at the commit in
   [`rerevved-mods.lock.json`](../rerevved-mods.lock.json), then create the
   release archive:

   ```powershell
   python .\scripts\assemble_mods.py --mods-checkout ..\rerevved-mods
   python .\scripts\package.py --mods-dir out\bundled-mods
   ```

   Assembly rejects a checkout at another commit or with local changes, and a
   mods title lock that is not an ancestor of the release commit. The assembly
   output directory must not already exist; choose a fresh `--out` path and
   pass that same path to `package.py --mods-dir` when preserving prior output.

3. Inspect `out/rerevved-v<version>-windows-x64.zip`. Confirm that it contains
   the executable, runtime libraries, the selected license files, player
   README, the bundled default logo asset pack, the bundled mods under
   `mods/`, and an empty `game/`
   directory, with no retail content, user files, or `mod_order.txt`. The
   Windows archive also contains the relay CLI `rexglue.exe`, `host-online.cmd`,
   `host-online.ps1`, `host-manual.cmd` and `ONLINE.txt` for online
   play.

Linux packaging produces a `.tar.gz` archive from a Linux Release binary. The
release workflow publishes it beside the Windows `.zip`. Linux support is
experimental, so record the build environment.

The Windows relay CLI comes from the build directory or the SDK install
recorded by `rexglue_DIR` in its `CMakeCache.txt`. Use
`--sdk-bin <matching-sdk>/bin` to override that lookup with the SDK used to
build the title. It needs the packaged `rexruntime.dll` beside it.

The package includes `LICENSE.txt` and `licenses/REXGLUE-LICENSE.txt`.

## Accept the standalone archive

Use a new extraction directory and isolated test user data. These are manual
acceptance checks, not claims made by archive assembly or CI. Complete every
item before publication:

- [ ] First run accepts a legally owned ISO and starts with the extracted
  content.
- [ ] First run accepts an extracted content folder and remembers it.
- [ ] Controller navigation reaches and operates the front end.
- [ ] Keyboard navigation reaches and operates the front end.
- [ ] A game can be saved, exited, launched again, and reloaded.
- [ ] One complete match reaches its natural end state.
- [ ] F2 lists every bundled mod, disabled, with no compatibility or
  activation diagnostic.
- [ ] The release renders with Xenos, and Display exposes the player
  presentation settings.
- [ ] The archive and staged tree contain no retail content, saves, logs,
  caches, or paths specific to one machine.

Record the version, archive checksum, build environment, and acceptance result
in the release notes. Publish only a candidate that completes the standalone
archive acceptance list.
