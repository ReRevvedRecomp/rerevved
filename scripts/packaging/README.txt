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

On Windows, config, logs, saves, and caches are stored under
Documents\rerevved. On Linux, logs are under
$XDG_DATA_HOME/rerevved/logs, or ~/.local/share/rerevved/logs when
XDG_DATA_HOME is not set. Other writable state uses the same rerevved data
root.

To back up progress and settings, exit ReRevved and copy the user data folder.
To restore it, exit ReRevved and copy the backup to the same location. The
application folder remains separate from writable user files.

STORAGE PROFILES

Bare launch uses the default storage profile (P=B). To use an isolated named
profile for a session, restart ReRevved with:

  --profile=<id>

Named profiles use B\profiles\<id>; their config, default logs, default cache,
saves, and achievements are isolated. Explicit log or cache overrides keep
their existing meanings. Marketplace content remains shared from B. To
initialize a new named profile from the default environment once, add:

  --profile_copy_from_default=true

Use --profile=default, or omit --profile, to return to the default profile;
use a prior --profile=<id> to return to that profile. Selection is read at
startup and is not persisted. See docs\rexglue-runtime.md, "Storage profiles",
for the canonical contract.

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
