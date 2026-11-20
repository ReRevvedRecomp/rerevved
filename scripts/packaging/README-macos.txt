ReRevved for macOS arm64

Build for Apple Silicon, a Vulkan-capable GPU and
macOS 13.3 or newer.
Supply your legally owned Civilization Revolution Xbox 360 game files and
version 1.3 title update. No original game content is included.

FIRST RUN

1. Extract all files into a folder you can write to. Keep them together.
2. Right-click rerevved and choose Open. This build is not notarized.
   If macOS blocks it, run this in Terminal for the extracted folder you
   trust, then open rerevved:

     xattr -dr com.apple.quarantine /path/to/rerevved-v<version>-macos-arm64

3. Choose your game ISO or a folder containing default.xex and Resource.
   ISOs are extracted into game/ beside rerevved. Selected folders are used
   directly. Supply the version 1.3 title update when prompted. If you select
   default.xexp, keep its Resource folder beside it.

F1 opens Settings, F2 manages mods, F3 opens Achievements, and F4 toggles FPS.
Bundled Lua mods start disabled. Enable them in F2 and restart.

User files are in the userdata folder beside rerevved.
Exit before backing up that folder. Use --profile=<id> for separate saves
and settings. F1 > General > Open Log Folder finds the active log.

Report problems with the release version, macOS version, Mac model, GPU,
reproduction steps, and a reviewed log. Do not share original game files or
secrets.
Documentation: https://github.com/ReRevvedRecomp/rerevved/blob/main/docs/README.md
Support: https://discord.gg/j6VttvdnMD

ReRevved is an unofficial project, built with the ReRevved SDK, a BSD-3-Clause
fork of ReXGlue. It is not affiliated with or endorsed by Firaxis Games,
2K or Take-Two Interactive. See LICENSE.txt and licenses/.
