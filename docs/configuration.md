# Configuration

Use F1 to open Settings. Select **Save** at the bottom to keep changes in
`rerevved.toml`. Settings that require a restart take effect on the next launch.
Close ReRevved before editing the file directly.

## Settings

- **Gameplay**: combat speed, AI civilizations, resources, Raging Barbarians,
  One City Challenge, Earliest Domination, Game of the Week, and the Support
  section.
- **Notifications**: research prompts, advisor pop-ups, and Barbarian messages.
  **Disable Save Reminder** suppresses a prompt; it does not save the game.
- **Online**: connection status, player name, relay invite, and Discord options.
  See [Online play](online-play.md).
- **Controls**: keyboard/controller bindings, mouse sensitivity, vibration,
  controller input in the background, and overlay hotkeys.
- **Display**: window and presentation options, described below.
- **Audio**: Volume, Mute, and Play Sound in Background.

Gameplay options that initialize a new game do not rewrite an existing save.
The [runtime contracts](rexglue-runtime.md) document individual option behavior.

**Earliest Domination** sets how many turns must pass before a Domination
victory can be declared: 10 turns (3000 BC, the default), 5 turns (3500 BC) or
1 turn (3900 BC). It applies to games started in the current launch. Loaded
saves and Game of the Week with Standard Rules keep the default.

## Display and audio

ReRevved renders with Xenos. **Render Resolution** defaults to **Auto (Match
Display)**, which chooses the render resolution at startup to cover the image
height in your window or fullscreen monitor. It uses multiples of 1280x720,
for example 2560x1440 on a 1440p or 3440x1440 display. You can also choose
1280x720, 2560x1440 or 3840x2160. **Window Size** sets the window dimensions
used by Auto at startup.
**Monitor** selects a display. **Fullscreen** fills that monitor
and applies immediately.

**Aspect Ratio** defaults to **16:9**. **Ultrawide** widens supported gameplay
views to match the window or monitor shape and keeps the interface centered
in a 16:9 region. The game's base resolution remains 1280x720.
**Render Resolution** controls the rendered detail.

With **Ultrawide** selected, you can set `display_aspect_custom` in
`rerevved.toml` to a fixed shape, such as `"21:9"` or `"3440:1440"`. This
option is config-only. An invalid value or a shape no wider than 16:9 keeps
16:9. With no custom value, an unknown window size also keeps 16:9.

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

Volume and Mute apply immediately. Turn off **Play Sound in Background** to
silence the game when its window is unfocused or minimized.

## File locations

The default player data folder is separate from the application folder:

| Platform | Default data folder |
| --- | --- |
| Windows | `Documents\My Games\ReRevved` |
| Linux, experimental | `$XDG_DATA_HOME/rerevved`, or `~/.local/share/rerevved` if unset |

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

## Back up or restore progress

1. Exit every running copy of ReRevved.
2. Copy the entire player data folder to a separate backup location. Include
   `profiles/` if you use named profiles.
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
