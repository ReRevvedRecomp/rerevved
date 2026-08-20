ReRevved

ReRevved is a native PC recompilation of Civilization Revolution (Xbox 360) on
the ReXGlue runtime. This package contains no game data. You need legally owned
files from the supported release and its version 1.3 title update.

REQUIREMENTS

Windows 10/11 x64 requires a Direct3D 12 capable GPU. Linux builds are
experimental and require a recent x64 distribution.

FIRST RUN

1. Extract the entire archive.
2. Run rerevved.exe on Windows or rerevved on Linux.
3. In the content selector, choose your game ISO or select an extracted
   content folder.
4. For an ISO, ReRevved extracts into the game folder beside the executable.
   Select either the original version 1.3 title update package as downloaded or
   its extracted default.xexp. ReRevved extracts a package and merges its game
   files automatically. For default.xexp, keep its Resource folder beside it.
5. For a folder, select the root containing the base executable, version 1.3
   patch, and Resource folder.

ReRevved validates required files before launch and reports incomplete or
modified content, content from the wrong version, and content from the wrong
region. The accepted location is written to rerevved.toml, so the selector
normally appears only once.

USER FILES AND BACKUPS

Config, logs, saves, and caches are stored in the user's data folder:

  Documents\rerevved        Windows
  ~/Documents/rerevved      Linux, or the XDG documents directory

To back up progress and settings, exit ReRevved and copy the user data folder.
To restore it, exit ReRevved and copy the backup to the same location. The
application folder remains separate from writable user files.

CONTROLS AND SETTINGS

Press F4 in game for video settings, keyboard mode, and live keyboard or
controller rebinding. Conflict warnings identify overlapping bindings. Choose
"Save to config" to keep changes.

AUTOMATION

Use this launch argument to select content for one session without changing the
saved location:

  --game_data_root=<path>

LEGAL

Use only game content you legally own. See LICENSE.txt and the licenses folder
for software license details.
