# Getting started

This guide covers Windows and the [macOS arm64 build](#macos).
Check the [system requirements](../README.md#system-requirements) before installing.
ReRevved contains no retail content; supply your own legally obtained files.

## Install and select content

1. Download the Windows x64 zip from
   [Releases](https://github.com/ReRevvedRecomp/rerevved/releases).
2. Unzip all of it into a folder you can write to, then run `rerevved.exe`.
   Keep the other files beside it.
3. Choose **Select File...** for an Xbox 360 game ISO, or **Select Folder...**
   for a folder containing the base `default.xex` and `Resource` directory.
4. If prompted for the version 1.3 title update, select the original update
   package or its extracted `default.xexp`. When using an extracted patch,
   keep its `Resource` directory beside it so those files can be merged.

An ISO is extracted into `game/` beside the executable. A selected folder is
used directly. The selector checks the base executable and patch hashes;
startup checks that the required files exist with the expected sizes. An
incomplete or unsupported selection cannot launch.

The accepted content location is saved as `game_data_root` in the active
profile's `rerevved.toml`. Later launches normally reuse that location. See
[Configuration](configuration.md#file-locations) for the configuration folder,
or [Troubleshooting](troubleshooting.md#content-is-missing-or-rejected) if
content is rejected.

## macOS

The macOS arm64 build needs an Apple Silicon Mac, a
Vulkan-capable GPU and macOS 13.3 or newer.

1. Download `rerevved-v<version>-macos-arm64.tar.gz` from
   [Releases](https://github.com/ReRevvedRecomp/rerevved/releases). Extract all
   files into a folder you can write to and keep them together.
2. Right-click `rerevved` and choose **Open**. The build is not notarized.
   If macOS blocks it, run
   `xattr -dr com.apple.quarantine <extracted-folder>` in Terminal for a
   download you trust, then open `rerevved`.
3. Select your game ISO or extracted content folder, then the version 1.3
   title update, as described above. ISOs are extracted into `game/` beside
   `rerevved`; selected folders are used directly.

## Find the controls

- F1 opens Settings.
- F2 opens the mod manager.
- F3 opens Achievements.
- F4 toggles the FPS counter.

Use a mouse or keyboard in these overlays; a game controller does not navigate
them. Rebind game controls under **F1 > Controls > Keybinds > Controller** and
overlay keys under **F1 > Controls > Overlay Hotkeys**. Select **Save** in
Settings to keep changes.

Continue with [Configuration](configuration.md), [Mods](mods.md), or
[Online play](online-play.md). The download also includes `README.txt` for
use without a browser.
