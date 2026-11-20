# Configuration

Use F1 to open Settings. Select **Save** at the bottom to keep changes in
`rerevved.toml`. Settings that require a restart take effect on the next launch.
Close ReRevved before editing the file directly.

## Settings

- **General**: Language (English, Deutsch, Francais, Espanol or Italiano, after a
  restart), Profile Name, FPS Counter, Skip Intro and Open Log Folder. **Profile
  Name** is your player name in single-player and online games, after a
  restart; an empty name reads "User" offline.
- **Gameplay**: combat speed, unit movement, resources, Raging Barbarians, One City
  Challenge, Earliest Domination, Turn Limit, Starting Era, Technology Cost, AI civilizations, Always At War, AI Handicaps,
  and Game of the Week.
- **Notifications**: Spaceship Scenes, research prompts, advisor pop-ups, and Barbarian messages. Each is on by default; turn a row off to hide it.
  New government prompts and Wonder completions always show, and online games always show the first Barbarian message.
  Turning off **Save Reminder** hides the prompt; it does not save the game.
- **Online**: AI Difficulty (Multiplayer), connection status, invite code, and Discord options.
  See [Online play](online-play.md).
- **Controls**: keyboard/controller bindings, Mouse Aim, mouse sensitivity,
  Drag to Pan, Drag Sensitivity, vibration, controller input in the
  background, Button Icons, and overlay hotkeys. **Drag to Pan** moves the map
  while the left mouse button is held. The drag drives the right stick, so pan
  speed follows mouse speed rather than the cursor position; **Drag
  Sensitivity** scales it. Mouse Aim takes precedence when both are on.
  Every connected controller plays as player one, so a second or virtual
  controller cannot be left without a profile.
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
Turn Limit, or whose settings file is missing or rejected, loads with 2100 AD.
Unlimited removes the turn limit and its warning; the other victories still end
the game.

**Starting Era** gives every civilization extra technologies when a new
game starts: Ancient (the default) adds none, Medieval adds 5, Industrial 14,
Modern 24 and Future all 46 ordinary technologies, through the game's own starting-technology grant. The game uses the
value it started with, so a later change applies only to the next new game. A new
multiplayer game uses the host's value for every player. A scenario and Game of
the Week keep their own era, and a loaded save never grants technologies again.

**Technology Cost** multiplies the research cost of every technology for every
civilization, human or AI. It runs from 1x to 100x in presets of 1x, 2x, 3x, 5x,
10x, 20x, 50x and 100x, and defaults to 1x, which keeps the original game's costs. A base cost
of 20 becomes 2000 at 100x. The multiplier applies before the AI Research Bonus.
The value applies to the next new game and is saved with it. A new multiplayer
game uses the host's value. Standard Rules use the original game's costs. The
gameplay settings file saved next to your save holds the value; a save without
it, and a Game of the Week practice save, loads 1x. The [runtime contract](rexglue-runtime.md#technology-cost)
describes rounding.

**Always At War** (off by default) keeps every civilization at war with every
other one for the whole game; peace can never be made. The game keeps the value it
started with and each save records it, so a later change applies only to the next
new game. It is for single player only: network games and Game of the Week ignore
it. A loaded save restores its own value. A save without a recorded Always At War
value, or whose settings file is missing or rejected, loads with it off.

**Apollo Discovery Bonuses** is an always-on fix, not a setting. When a
civilization completes Apollo, every technology first-discovery bonus nobody has
claimed is granted; otherwise technologies 1 to 46 count as owned and the bonuses
are unreachable. A bonus any civilization already claimed is never repeated. It
applies in single player and multiplayer and skips Game of the Week. A save made
after Apollo completes but before the bonuses arrive can lose them.

**AI Difficulty (Multiplayer)** sets the difficulty of every AI in a multiplayer
game you host: King (the default and the original game's multiplayer value),
Emperor or Deity. Every player in that game plays the host's value. A later
change applies only to the next new game. Single player keeps the difficulty
chosen at game setup, and a loaded save never applies the host's value.
At Emperor an AI city gets an extra city turn every 5 turns after the first 10, and
at Deity every 3 turns.
Online, no human gets it and it does not depend on rank, so every player's
game gives it to the same cities.

**Spaceship Scenes** (Notifications, on by default) shows the rocket
scene of every spaceship part, yours included; turn it off to skip them. Multiplayer games always
show them; otherwise your own setting applies, also to a Game of the
Week. Part scenes already running are not affected by a change.

**Allow Saving** (Gameplay > Game of the Week, off by default) lets you save and
load a Game of the Week. When it is on as the game starts, the whole game is
practice: it never counts toward the weekly results and earns no Game of the
Week achievements. Use the Save and Load rows of the pause menu. A practice save
resumes as practice only with its gameplay settings file (saved next to the
save), returning to the week, seed, civilization and rules it was saved with.
Load accepts any save, in a game or from the main menu. Every save made during
a practice game writes this file, even with `save_sidecar` off. Automatic saves are not resumable: they load without practice rules and
never count toward the weekly results. A practice save whose settings file is
later lost loads as an ordinary game.

The in-game Save list and both Load lists offer 99 manual slots. The config-only
key `save_slots` sets the number, from 10 to 99; 10 keeps the original list. It
is read when the first list opens, so restart after changing it. The Save list
shows the existing saves plus one empty slot, never a row of empty slots. Slots
beyond 10 are ordinary original-format saves named `save10` and up.

### AI handicaps

The Gameplay settings include **AI Research Bonus**, **AI Production Bonus**,
**AI Gold Bonus**, **AI Food Bonus**, and **AI Culture Bonus**. Each runs from 0
to 200 and defaults to 0, which keeps the original game's behavior. Humans and
Barbarians keep the original values. These options change the economy, not leader personality
or the number of AI civilizations.

Research and production costs are divided by `1 + bonus/100`. A bonus of 100
halves the cost and 200 divides it by three. Positive city Gold, Food, and
Culture contributions are multiplied by `1 + bonus/100`. The
[runtime contract](rexglue-runtime.md#ai-handicaps) describes rounding and limits.

Changes apply to the next new game. Multiplayer uses the host's values; see
[Online play](online-play.md#gameplay-settings-and-mods).

The gameplay settings file saved next to your save holds all five bonuses.
Loading a single-player save restores them. Saves without a valid settings
file, settings files from earlier releases, and loads with the settings file
turned off use 0 for all five. Keep the `.rerevved-gameplay` file beside its
save when copying saves.

Game of the Week with **Standard Rules** uses 0 for all five bonuses.
Multiplayer loads use the original game's behavior, whatever the save recorded.

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
centered in a 16:9 region. **16:10** fits a 16:10 screen: gameplay shows a
little more at the top and bottom, and the interface keeps its 16:9 shape in
the middle. Menus and movies stay 16:9. The game's base resolution remains
1280x720.
**Render Resolution** controls the rendered detail.

With **Ultrawide** selected, you can set `display_aspect_custom` in
`rerevved.toml` to another fixed shape, such as `"3440:1440"`. This option is
config-only and is ignored while **16:10**, **21:9** or **32:9** is selected. An
invalid value or exactly 16:9 keeps 16:9. With no custom value, an unknown window size also keeps 16:9.

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

**4:3** uses the game's own 4:3 layout for displays of that shape. The game
then renders from a 640x480 base, **Render Resolution** offers 640x480,
1280x960 and 1920x1440, and wider windows show bars at the sides. Changing to
or from **4:3** takes effect after a restart, and the Ultrawide choices do
not widen the view until then. Explicit `video_mode_width` and
`video_mode_height` values override the 640x480 base.

Monitor, Window Size, Render Resolution, Upscaling, and Anti-Aliasing require
a restart. Select **Save**, exit, and launch again after changing them.
Explicit `window_width` and `window_height` values in the config or command
line override Window Size.

**Skip Intro** skips the legal screens and the opening movie, starting with the
next launch. Other movies, including the attract movie, are unchanged.

Volume and Mute apply immediately. Turn off **Play Sound in Background** to
silence the game when its window is unfocused or minimized.

## File locations

ReRevved keeps its player data in a `userdata` folder beside `rerevved.exe`
(`rerevved` on Linux and macOS), so deleting the game folder removes every file
it wrote. On Windows, Discord Join also registers a `discord-<id>` link handler
in the current user's registry, which stays behind. The folder holds `rerevved.toml`, `logs/`, `cache/`, saves,
achievements, and saved mod/asset selections. **F1 > General > Open Log Folder**
opens the active log folder, including when a profile or log override is used.

The game folder must be writable. If ReRevved cannot create or write
`userdata`, for example under `Program Files`, it shows a message and refuses
to start. Move the game to a folder you can write to, or keep the data
elsewhere with `--user_data_root=<folder>`.

A named profile stores its state under `userdata/profiles/<id>/`.
For example, launch `rerevved.exe --profile=second` for a separate profile.
Use the same argument each time: the selected profile is not remembered.
Omit it or use `--profile=default` to return to the default profile.
See the [storage profile contract](rexglue-runtime.md#storage-profiles) for
valid names, copying a default profile, shared content, and path overrides.

## Back up or restore progress

1. Exit every running copy of ReRevved.
2. Copy the entire `userdata` folder to a separate backup location. Include
   `profiles/` if you use named profiles.
3. To restore, close ReRevved and copy the backup to the same location.

If you set explicit paths for user data, logs, or caches, account for those
locations separately. Installed mods and asset overrides live beside the executable;
back up custom packages separately as described in [Mods](mods.md).
Do not share a backup publicly: it can contain saves, player IDs,
paths, and online server credentials.

## Select different content

For one launch without changing the saved location, use:

```text
rerevved.exe --game_data_root="D:\Games\CivRev"
```

To reopen the selector, exit ReRevved, back up `rerevved.toml`, then remove its
`game_data_root` entry. The next launch asks for content again. Do not delete
your saves or the whole data folder to change the content path.

## Launch options

Every setting in `rerevved.toml` except the `[log.levels]` table can also be
given on the command line as `--<name>=<value>`. These are the ones launchers and players use most:

| Option | Effect |
| --- | --- |
| `--game_data_root=<folder>` | Use this game content folder for the launch instead of the saved one |
| `--user_data_root=<folder>` | Keep saves, settings, logs and caches in this folder instead of `userdata` |
| `--profile=<id>` | Use a separate named profile; see [File locations](#file-locations) |
| `--fullscreen=true` or `false` | Start fullscreen or windowed; fullscreen is the default |
| `--window_size=<size>` | Windowed size: `1280x720`, `1600x900`, `1920x1080`, `2560x1440`, `3840x2160` or `maximized` |
| `--skip_intro_movie=true` | Skip the legal screens and the opening movie |
| `--log_level=<level>` | `trace`, `debug`, `info` (the default), `warn`, `error`, `critical` or `off` |
| `--log_file=<path>` | Write the log to this file |
| `--live_invite=<code>` | Join a hosted Player Match game, as Discord Join does |

## Logs

Each launch writes `logs/rerevved_NNN.log` in the data folder. At the default
`info` level the log is meant to explain a crash or freeze on its own. It
starts with the ReRevved version and build, the operating system, CPU threads
and memory, the game, data and log folders, and the display, language and
online settings. It then records the graphics adapter, the content check, the
active mods and asset packages, online connection and session changes, and a
`Phase:` line when the game launches, a game begins or loads, and at shutdown.
Network games also log `Turn <n>` once per turn. Repeated warnings and
per-frame detail log at `debug`.

The log file writes your user profile folder as `%USERPROFILE%` (`~` on Linux
and macOS). Set `log_redact_user_folder = false` to write the real path. A
`[log.levels]` table in `rerevved.toml` sets the level for one category, for
example `krnl = "debug"` for the kernel.
