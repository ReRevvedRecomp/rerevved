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

The main menu's **Options** opens the Settings page. In a game, the Special
Functions menu has **Exit Game**, and **Change Options.** opens the Settings
page too. This full-screen page supports a controller or keyboard: Up
and Down move between settings, Left and Right change the value, A toggles a
setting or opens a list of choices, and B closes the page and returns to the
menu you came from. The settings are in tabs (General, Gameplay,
Notifications, Online, Controls, Display and Audio); LB and RB (or PageUp and
PageDown) switch tabs. A change applies and saves at once; the footer says
when a setting takes effect only after a restart or in the next game.
Gameplay also has **Turn Limit** (2100 AD by default, up to Unlimited), which
applies to the next new game and is saved with each game, and **Starting Era**
(Ancient by default, up to Future), which gives every civilization extra
technologies in the next new game. **AI Difficulty (Multiplayer)** on the Online
tab (King by default, up to Deity) sets the AI difficulty of the multiplayer games you host.
Game of the Week has **Allow Saving** under Gameplay, off by default; a game
started with it on is practice and can use Save and Load.
The Audio section also has Music, Effects, Advisor Speech and Leader Speech
volumes from 0 to 9. They apply at once and are reapplied every time the
game loads your profile.
The first tab, General, sets the game's language after a restart and holds
the Profile Name, FPS counter, Skip Intro and **Open Log Folder**. The Online
tab shows the connection status and has the Invite Code. Profile Name and
Invite Code open a text entry (a keyboard on screen for a gamepad); Invite Code
also has Paste and Copy buttons.
The Controls tab also lists the controller keybinds and the overlay hotkeys:
press A or click a row, then press the new key (hold Shift, Ctrl or Alt for a
combination on a controller key); Esc or B cancels, X or Delete clears and Y or
Backspace restores the default. Developer entries stay in the F1 window. **Mods.** and
**Achievements.** open the F2 and F3 windows.

A game controller also drives Settings, Mods and Achievements while one is
open: the D-pad or left stick moves, A selects, B backs out or closes the
window, and LB/RB switch tabs. The game ignores the controller until the last
of them closes. Typing text and capturing a key need the keyboard.

The A, B, LT, RT, Back and Start keys default to one key each (A is `;`, B is
`'`, LT is `Q`, RT is `E`, Back is `Z`, Start is `X`); Y or Backspace on a row restores its
default. Rebind game controls under **Controls > Keybinds** and overlay keys under
**Controls > Overlay Hotkeys**, on this page or in F1. Select **Save** in
Settings to keep changes.

Continue with [Configuration](configuration.md), [Mods](mods.md), or
[Online play](online-play.md). The download also includes `README.txt` for
use without a browser.
