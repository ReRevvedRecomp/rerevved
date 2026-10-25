REREVVED

ReRevved is a static recompilation of Civilization Revolution (Xbox 360).

REQUIREMENTS

Windows 10 or 11 x64 and a Direct3D 12 capable GPU. A Linux x64
download is also available. You need legally owned game files from the supported release
and its version 1.3 title update. No game data is included.

FIRST RUN

1. Unzip all of the download.
2. Run rerevved.exe on Windows or rerevved on Linux.
3. In the content selector, choose your game ISO or extracted content folder.

ISO: ReRevved extracts to the game folder beside the executable. Select the
original version 1.3 title update package as downloaded, or its extracted
default.xexp. A package is extracted and its game files merged automatically.
For default.xexp, keep its Resource folder beside it.

Folder: select the root containing the base executable, version 1.3 patch,
and Resource folder.

Your choice is saved as game_data_root in the active profile's rerevved.toml,
so the selector normally appears only once.

Missing GFX_ files: merge the version 1.3 update's Resource folder into the
content folder. To change content, exit ReRevved, back up rerevved.toml,
remove its game_data_root entry, and launch again.

The selector checks the base executable and patch hashes. Each launch checks
required files and their sizes, without hashing every file.

CONTROLS AND SETTINGS

In game:
F1  Settings: gameplay, notifications, online play, controls and keybinds,
    display, and audio. Select Save at the bottom to keep changes.
F2  Manage code mods and asset overrides.
F3  Achievements.
F4  Toggle the FPS counter.

Change overlay keys in F1 > Controls > Overlay Hotkeys. Rebind game controls
under Controls > Keybinds > Controller. Overlays need a mouse or keyboard;
a game controller cannot navigate them.

Display has resolution, window, and presentation settings. Fullscreen applies
immediately. Monitor, Window Size, Render Resolution, Upscaling, and
Anti-Aliasing need a restart.

The ReRevved logo is an enabled asset override by default. Saving the Asset
Overrides list creates a profile-specific selection and priority order.
Restart to apply it.

This release renders with Xenos. Renderer and native_* settings left in
rerevved.toml by earlier native test builds are ignored.

MODS

The mods folder holds the first-party gameplay mods, disabled by default.
Enable them in F2, select Save changes, and restart. Online players should
enable the same mods.

Each mod is an editable Lua script. Extracting a release can overwrite bundled
folders. To keep edits, copy a mod folder under a new name, set that name as
the id in its mod.toml, and edit the copy. Back up custom packages separately
from user data.

ONLINE

On Windows, see ONLINE.txt for System Link over the Internet.
Double-click host-online.cmd to host.

USER FILES AND BACKUPS

Windows config, logs, saves, and caches are in Documents\My Games\ReRevved.
Linux logs are in $XDG_DATA_HOME/rerevved/logs, or
~/.local/share/rerevved/logs when XDG_DATA_HOME is not set. Other writable
state uses the same rerevved data root.

Exit ReRevved before backing up or restoring. Copy the user data folder to
back up progress and settings; restore it to the same location. Writable user
files stay separate from the application folder.

STORAGE PROFILES

A normal launch uses the default profile in the user data folder. For a
separate named profile, run this from the package folder:

  rerevved.exe --profile=second

Named profiles use profiles\<id> in the user data folder. Config, default
logs, default cache, saves, and achievements are isolated. Marketplace content
stays shared in the base user data folder. Explicit log or cache overrides
still apply.

Use --profile=default, or omit --profile, to return to the default profile.
Use a prior --profile=<id> to return to that profile. Selection applies at
startup and is not saved.

To copy the default environment to a new profile, choose an ID whose folder
does not exist and add this option on its first launch only:

  --profile_copy_from_default=true

Full details: see Storage profiles at
https://github.com/ReRevvedRecomp/rerevved/blob/main/docs/rexglue-runtime.md

AUTOMATION

Select content for one session without changing the saved location:

  --game_data_root=<path>

HELP

F1 > Gameplay > Support > Open Log Folder opens the active log folder.
Report the release version, OS, CPU, GPU, steps to reproduce, and what
happened. Check logs for personal paths and secrets before sharing. Do not
upload game files, saves, configuration files, or the full data folder.

Documentation:
https://github.com/ReRevvedRecomp/rerevved/blob/main/docs/README.md
Discord: https://discord.gg/j6VttvdnMD

LEGAL

This is an unofficial project and contains no game assets. Use only game
content you legally own. See LICENSE.txt and licenses/REXGLUE-LICENSE.txt.

ReRevved is built with the ReRevved SDK, a BSD-3-Clause fork of ReXGlue.

ReRevved is not affiliated with or endorsed by Firaxis Games, 2K or
Take-Two Interactive. Civilization Revolution names and marks belong to
their owners.
