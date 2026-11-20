<p align="center">
<img width="640" src="res/rerevved_banner.png" alt="ReRevved banner">
</p>

<p align="center">
Static recompilation of Civilization Revolution for Xbox 360.
</p>

<p align="center">
<a href="LICENSE"><img src="https://img.shields.io/badge/License-GPLv3-blue.svg" alt="License: GPL v3"></a>
<a href="https://github.com/ReRevvedRecomp/rerevved/actions/workflows/checks.yml"><img src="https://github.com/ReRevvedRecomp/rerevved/actions/workflows/checks.yml/badge.svg" alt="Checks"></a>
<a href="https://discord.gg/j6VttvdnMD"><img src="https://img.shields.io/badge/Discord-Join-5865F2?logo=discord&amp;logoColor=white" alt="Join Discord"></a>
</p>

**You must own the game.** ReRevved contains no game assets. You supply your
own Xbox 360 copy of Civilization Revolution and its version 1.3 update.

## Install

[Download the latest release](https://github.com/ReRevvedRecomp/rerevved/releases),
unzip it, and run `rerevved.exe`. Select your game ISO or
extracted content folder, then supply the version 1.3 update when
prompted. [Getting started](docs/getting-started.md) covers content setup and
controls.

## System requirements

- **Windows 10 or 11 x64** with a Direct3D 12 capable GPU.
- **Game files:** your own legally obtained Xbox 360 copy of Civilization
  Revolution and its version 1.3 update.
- **Linux x64:** requires a Vulkan capable GPU.
- **macOS arm64:** requires Apple Silicon, a Vulkan capable GPU and macOS
  13.3 or newer. See [first launch](docs/getting-started.md#macos).

## Build from source

On Windows x64:

1. Clone the repository with its submodules:

   ```powershell
   git clone --recursive https://github.com/ReRevvedRecomp/rerevved
   ```

2. Build and install the [ReRevved SDK](https://github.com/ReRevvedRecomp/rerevved-sdk)
   at the commit in `rexglue-sdk.lock.json`, as a sibling checkout named
   `rerevved-sdk`.
3. Copy your base `default.xex`, the version 1.3 `default.xexp`, and the
   matching `Resource/` folder into `game/`.
4. Generate the guest code and build:

   ```powershell
   .\scripts\rexglue.ps1 -Stage Build
   ```

The executable is `out/build/win-amd64-release/rerevved.exe`.
[Development](docs/development.md) lists the exact toolchain and covers macOS,
developer builds, and tests.

## Documentation

- [Getting started](docs/getting-started.md): install and first run
- [Configuration](docs/configuration.md): settings, file locations, backups, and launch options
- [Troubleshooting](docs/troubleshooting.md): content, display, and startup help
- [Online play](docs/online-play.md): host or join through an online server
- [Mods](docs/mods.md): enable the bundled Lua mods or install a custom package
- [Support](docs/support.md): ask for help, report a problem, and share a log
- [Development](docs/development.md): prerequisites, builds, checks, and tests
- [Documentation index](docs/README.md): player guides and developer reference

## Contributing

Bug reports, ideas, and pull requests are welcome. AI-assisted work is reviewed
exactly like any other work: correct, tested changes that fit the project get
merged. Open pull requests against `develop`, keep each to one concern, and
leave AI tools out of commit credits. [Contributing](CONTRIBUTING.md) covers the
full process.

## Acknowledgements

- [ReXGlue](https://github.com/rexglue/rexglue-sdk)
- [Xelu's FREE Controller & Key Prompts](https://thoseawesomeguys.com/prompts/) (CC0), the keyboard button icons

## License

<a href="LICENSE"><img src="https://upload.wikimedia.org/wikipedia/commons/9/93/GPLv3_Logo.svg" width="88" alt="GPLv3 License"></a>

ReRevved is built with the
[ReRevved SDK](https://github.com/ReRevvedRecomp/rerevved-sdk), a BSD-3-Clause
fork of ReXGlue.

ReRevved is an unofficial project and is not affiliated with or endorsed by
Firaxis Games, 2K or Take-Two Interactive.
