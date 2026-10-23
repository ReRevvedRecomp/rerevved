# Troubleshooting

Start with the complete download and the
[system requirements](../README.md#system-requirements). Keep a backup of
player data before changing files; see [Configuration](configuration.md).

## Content is missing or rejected

Read the validation message for the missing file or wrong size. Use the
supported Xbox 360 base game and version 1.3 title update, not a modified or
partially extracted copy.

The final content folder needs `default.xex`, `default.xexp`, and `Resource/`.
The title update includes `GFX_` files not present on the disc. If those files
are missing, merge the update's `Resource` directory into the content folder.
The [first-run guide](getting-started.md#install-and-select-content) explains
how to supply a package or extracted patch.

If the saved path is wrong, follow
[Select different content](configuration.md#select-different-content) to
reopen the selector. A command-line content path bypasses the selector, not
startup validation.

## The game does not start

Unzip the whole download, not just `rerevved.exe`. Its DLLs must stay beside
it. On Windows, use the x64 download and a Direct3D 12 capable GPU.
If startup reports a missing file, restore it from the same release download.
Do not mix binaries from different releases.

If content extraction fails, use a writable application folder and check that
the selected ISO or update package is complete. ReRevved cannot supply missing
retail files.

## A setting or mod did not take effect

- Select **Save** in F1, or **Save changes** in F2, before exiting.
- Restart after a setting marked for restart or a mod/asset selection change.
- Check which [profile](configuration.md#file-locations) you launched. Profiles
  have separate settings and loadouts.
- Review F2 compatibility and activation messages. See [Mods](mods.md).

## Display or overlay input is wrong

Use **F1 > Display** to distinguish Window Size from Render Resolution.
After changing display settings that require a restart, save and restart.
Check for explicit `window_width` and `window_height` overrides if Window Size
appears to be ignored.

Use the mouse or keyboard in overlays. Controller bindings affect the game,
not overlay navigation. If F1-F4 no longer open their default overlays, check
**Controls > Overlay Hotkeys** or the saved bindings in `rerevved.toml`.

## Online play fails

Follow the troubleshooting advice for your status in
[ONLINE.txt](../scripts/packaging/ONLINE.txt). Everyone must use
the same release, relay, and room. Do not post invite codes or relay secrets
with a report.

## Report a crash, freeze, or unresolved problem

Record what you did, what happened, and the release version. Use
**F1 > Gameplay > Support > Open Log Folder** when available. Follow
[Support](support.md) to review the log before attaching it to an issue.
