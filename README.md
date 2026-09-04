<p align="center">
<img src="res/rerevved_banner.png" alt="ReRevved" width="640">
</p>

<p align="center">
Static recompilation of Civilization Revolution, built on the ReXGlue runtime and GPU stack.
</p>

<p align="center">
<a href="LICENSE"><img src="https://img.shields.io/badge/License-GPL--3.0-blue.svg" alt="License: GPL-3.0"></a>
<a href="https://github.com/ReRevvedRecomp/rerevved/actions/workflows/checks.yml"><img src="https://github.com/ReRevvedRecomp/rerevved/actions/workflows/checks.yml/badge.svg" alt="Checks"></a>
<a href="https://discord.gg/j6VttvdnMD"><img src="https://img.shields.io/badge/Discord-Join-5865F2?logo=discord&logoColor=white" alt="Discord"></a>
</p>

ReRevved includes no copyrighted game materials and is not affiliated with or
endorsed by the publisher. You must own the supported Xbox 360 release and its
version 1.3 title update. All trademarks belong to their respective owners.

## Play ReRevved

- Download a stable archive from the
  [ReRevved releases page](https://github.com/ReRevvedRecomp/rerevved/releases)
  and extract it completely.
- Run `rerevved.exe`.
- On first run, select a legally owned ISO or extracted version 1.3 content
  folder.
- Config and saves are stored under `Documents\My Games\ReRevved`.
- Press F1 to open the mod manager. The optional State Inspector mod uses F6.
- Press F4 in game for settings and keyboard or controller rebinding.
- See [display settings](docs/rexglue-runtime.md#display-settings) for window,
  fullscreen, and rendering quality options.

Windows 10 or 11 x64 with a Direct3D 12 capable GPU is the supported player
platform. Linux archives use the same first run flow when a Linux binary is
available, but Linux support is experimental.

See the [packaged player guide](scripts/packaging/README.txt) for detailed
setup, backup, restore, and automation instructions.

## Support

Read the [support and log-sharing guide](docs/support.md) before reporting a
bug or attaching a runtime log.

## Development

- [Mod APIs](docs/modding-api.md) - public title interfaces and ownership
- [ReXGlue runtime and build](docs/rexglue-runtime.md) - prerequisites,
  workspace setup, and the full build workflow
- [Scaleform/GFx behavior](docs/scaleform-gfx.md)
- [Tooling](scripts/README.md)
- [Release process](docs/releasing.md)

Pull requests are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) before you
open one.

## Acknowledgements

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)

## License

<a href="LICENSE"><img src="https://www.gnu.org/graphics/gplv3-127x51.png" alt="GNU General Public License version 3"></a>

Code and documentation use the [GNU General Public License version 3](LICENSE).
License notices for external components are in
`REXGLUE-LICENSE.txt`. Retail game content is not distributed by this project.
