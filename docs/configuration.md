# Configuration

Use F1 to open Settings. Select **Save** at the bottom to keep changes in
`rerevved.toml`. Settings that require a restart take effect on the next launch.
Close ReRevved before editing the file directly.

## Settings

- **General**: Language (English, Deutsch, Francais, Espanol or Italiano, after a
  restart), FPS Counter and Skip Intro.
- **Gameplay**: combat speed, resources, Raging Barbarians, One City
  Challenge, Earliest Domination, Turn Limit, Starting Era, AI civilizations, AI Handicaps,
  Game of the Week, and the Support section.
- **Notifications**: research prompts, advisor pop-ups, and Barbarian messages.
  **Disable Save Reminder** suppresses a prompt; it does not save the game.
- **Online**: connection status, player name, relay invite, and Discord options.
  See [Online play](online-play.md).
- **Controls**: keyboard/controller bindings, mouse sensitivity, vibration,
  controller input in the background, Button Icons, and overlay hotkeys.
  **Button Icons** chooses which button icons the game shows, after a
  restart. **Auto** shows PlayStation icons if the
  controller you last used, or else the first one connected, is a PlayStation
  controller, and Xbox icons for any other controller. With no controller
  connected at launch it shows keyboard icons. **Keyboard** always shows them;
  they replace each single-button image prompt with the first key bound to it.
- **Display**: window and presentation options, described below.
- **Audio**: Volume, Mute, and Play Sound in Background.

Gameplay options that initialize a new game do not rewrite an existing save.
The [runtime contracts](rexglue-runtime.md) document individual option behavior.
For new multiplayer games, the host supplies the shared gameplay choices.
See [Online play](online-play.md#gameplay-settings-and-mods).

**Raging Barbarians** produces more Barbarian units and lets them leave camps
sooner. While it is on, Barbarians cannot capture a civilization's capital
before 3000 BC. The host controls new multiplayer games. Local saves restore
the frozen setting. Game of the Week Standard Rules disables it.

**Earliest Domination** sets how many turns must pass before a Domination
victory can be declared: 10 turns (3000 BC, the default) or 5 turns
(3500 BC). It applies to games started in the current launch, including Game of
the Week with Standard Rules. A new multiplayer game uses the host's value.
Loaded saves keep the default, except a Game of the Week practice save, which
restores the minimum it was saved with.

**Turn Limit** chooses the year a new game can end by turn limit: 2100 AD (the
default, as in the original game), 2200 AD, 2300 AD, 2500 AD or Unlimited. The
game keeps the value it started with and each save records it, so a later change
applies only to the next new game. A new multiplayer game uses the host's value
for every player. Game of the Week always uses 2100 AD. A save without a recorded
Turn Limit, or whose sidecar is missing or rejected, loads with 2100 AD.
Unlimited removes the turn limit and its warning; the other victories still end
the game.

**Starting Era** gives every civilization extra technologies when a new
game starts: Ancient (the default) adds none, Medieval adds 5, Industrial 14 and
Modern 24, through the game's own starting-technology grant. The game uses the
value it started with, so a later change applies only to the next new game. A new
multiplayer game uses the host's value for every player. A scenario and Game of
the Week keep their own era, and a loaded save never grants technologies again.

**Allow Saving** (Gameplay > Game of the Week, off by default) lets you save and
load a Game of the Week. When it is on as the game starts, the whole game is
practice: it never counts toward the weekly results and earns no Game of the
Week achievements. Use the Save and Load rows of the pause menu. A practice save
resumes as practice only with its gameplay sidecar, returning to the week, seed,
civilization and rules it was saved with. Load accepts any save, in a game or
from the main menu. Every save made during a practice game writes its gameplay
sidecar, even with the save sidecar setting off. Automatic saves are not
resumable: they load without practice rules and never count toward the weekly
results. A practice save whose sidecar is later lost loads as an ordinary game.

## Display and audio

ReRevved renders with Xenos. **Render Resolution** defaults to **Auto**, which chooses at startup the largest multiple of 1280x720 that fits
your window or fullscreen monitor, for example 1280x720 on a 1080p display or
2560x1440 on a 1440p display. You can also choose 1280x720, 2560x1440 or
3840x2160. **Window Size** sets the window dimensions used by Auto at startup.
**Monitor** selects a display. **Fullscreen** fills that monitor
and applies immediately.

**Aspect Ratio** defaults to **16:9**. **Ultrawide** widens supported gameplay
views to match the window or monitor shape; **21:9** and **32:9** widen them to
that fixed shape regardless of the window. Wide views keep the interface
centered in a 16:9 region. The game's base resolution remains 1280x720.
**Render Resolution** controls the rendered detail.

With **Ultrawide** selected, you can set `display_aspect_custom` in
`rerevved.toml` to another fixed shape, such as `"3440:1440"`. This option is
config-only and is ignored while **21:9** or **32:9** is selected. An invalid
value or a shape no wider than 16:9 keeps 16:9. With no custom value, an unknown window size also keeps 16:9.

Ultrawide defaults `present_letterbox` to on and both safe areas to 100 to
preserve the shape of 16:9 frames. Conflicting explicit values keep gameplay
at 16:9. It also defaults `present_bar_fill` to `blur`, filling the bars around
16:9 menus, movies and loading screens with a dimmed, blurred copy of the
image. Set it to `black` for plain bars. Aspect Ratio changes apply at the
next frame boundary.

Labels, the cursor and picking still use the original 16:9 coordinates, and
objects in the extra width may be hidden by the game's visibility checks.
Only recognized gameplay views widen. A movie detected after a wide frame
begins can keep subsequent frames at 16:9 until Aspect Ratio or the custom
value changes.

Monitor, Window Size, Render Resolution, Upscaling, and Anti-Aliasing require
a restart. Select **Save**, exit, and launch again after changing them.
Explicit `window_width` and `window_height` values in the config or command
line override Window Size.

**Skip Intro** skips the legal screens and the opening movie, starting with the
next launch. Other movies, including the attract movie, are unchanged.

Volume and Mute apply immediately. Turn off **Play Sound in Background** to
silence the game when its window is unfocused or minimized.

## File locations

The default player data folder is separate from the application folder:

| Platform | Default data folder |
| --- | --- |
| Windows | `Documents\My Games\ReRevved` |
| Linux x64 and macOS arm64 | `$XDG_DATA_HOME/rerevved`, or `~/.local/share/rerevved` if unset |

Windows uses your actual Documents location, including a redirected folder.
The data folder holds `rerevved.toml`, `logs/`, `cache/`, saves, achievements,
and saved mod/asset selections. **F1 > Gameplay > Support > Open Log Folder**
opens the active log folder, including when a profile or log override is used.

A named profile stores its state under `profiles/<id>/` inside the data folder.
For example, launch `rerevved.exe --profile=second` for a separate profile.
Use the same argument each time: the selected profile is not remembered.
Omit it or use `--profile=default` to return to the default profile.
See the [storage profile contract](rexglue-runtime.md#storage-profiles) for
valid names, copying a default profile, shared content, and path overrides.

### Portable Mode

To keep everything beside the game, create an empty file named `portable.txt`
in the folder that holds `rerevved.exe`. The data folder then becomes
`userdata\` in that folder instead of the table above. It needs a writable
location: if ReRevved cannot create or write `userdata\`, for example under
`Program Files`, it shows a message and refuses to start. Move the game to a
writable folder or delete `portable.txt`.

An explicit `--user_data_root` still overrides the marker. Named profiles live
under `userdata\profiles\<id>\`. Release archives do not include
`portable.txt`. To move existing progress, copy the contents of the Documents
data folder into `userdata\` while ReRevved is closed.

## Back up or restore progress

1. Exit every running copy of ReRevved.
2. Copy the entire player data folder (`userdata\` in Portable Mode) to a
   separate backup location. Include `profiles/` if you use named profiles.
3. To restore, close ReRevved and copy the backup to the same location.

If you set explicit paths for user data, logs, or caches, account for those
locations separately. Installed mods and asset overrides live beside the executable;
back up custom packages separately as described in [Mods](mods.md).
Do not share a backup publicly: it can contain saves, account identities,
paths, and relay credentials.

## Select different content

For one launch without changing the saved location, use:

```text
rerevved.exe --game_data_root="D:\Games\CivRev"
```

To reopen the selector, exit ReRevved, back up `rerevved.toml`, then remove its
`game_data_root` entry. The next launch asks for content again. Do not delete
your saves or the whole data folder to change the content path.
