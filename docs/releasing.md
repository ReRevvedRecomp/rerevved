# Release process

This runbook builds and exercises a ReRevved release candidate locally. Pushing a
`v*` tag triggers the release workflow and publishes the candidate.

## Set the version

1. Update the version in the root `project(rerevved VERSION ...)` declaration
   in [`CMakeLists.txt`](../CMakeLists.txt).
2. After building and packaging, confirm that the archive name uses the project
   version. The window title and Windows executable tooltip intentionally omit
   it.

## Build the release candidate

1. Start from a fresh `out/build/win-amd64-release/` build directory and run the
   Release pipeline:

   ```powershell
   .\scripts\rexglue.ps1 -SelfTest
   .\scripts\rexglue.ps1 -Stage Codegen
   .\scripts\rexglue.ps1 -Stage Build
   ```

2. Create the release archive:

   ```powershell
   python .\scripts\package.py
   ```

3. Inspect `out/rerevved-v<version>-windows-x64.zip`. Confirm that it contains
   the executable, runtime libraries, licenses, player README, and an empty
   `game/` directory, with no retail content or user files.

Linux packaging produces a `.tar.gz` archive from a Linux Release binary. Linux
support is experimental, so record the build environment.

## Accept the standalone archive

Use a new extraction directory and test user data location. Complete every item
before publication:

- [ ] First run accepts a legally owned ISO and starts with the extracted
  content.
- [ ] First run accepts an extracted content folder and remembers it.
- [ ] Controller navigation reaches and operates the front end.
- [ ] Keyboard navigation reaches and operates the front end.
- [ ] A game can be saved, exited, launched again, and reloaded.
- [ ] One complete match reaches its natural end state.
- [ ] The archive and staged tree contain no retail content, saves, logs,
  caches, or paths specific to one machine.

Record the version, archive checksum, build environment, and acceptance result
in the release notes. Publish only a candidate that completes the standalone
archive acceptance list.
