# Mods and asset overrides

F2 opens the mod manager. The Windows release includes first-party Lua mods
under `mods/` beside the executable. They start disabled when the active
profile has no saved loadout.

## Enable or change mods

1. Open F2 and review each package's compatibility and activation messages.
2. Enable the mods you want and set their load order.
3. Select **Save changes**, then restart the game.

The active profile's `mod_order.txt` records the selection. A missing or invalid
enabled package can block the entire selected mod set for that launch; F2
remains available so you can repair and save the selection. To return to an
unmodded loadout, disable the code mods, save, and restart.

Everyone in an online game should use the same mod selection. Avoid changing
packages while the game is running.

## Install or customize a package

Place a compatible package folder under `mods/` beside the executable, then
review it in F2. Do not assume a package is compatible because its folder is
present. The [mod API guide](modding-api.md) defines the supported manifest
and Lua rules contracts.

For a custom version of a bundled mod, copy its folder under a new name and
use that name as the `id` in its `mod.toml`. Edit the copy, not the bundled
original, and enable only the version you intend to use. Keep backups of custom
packages outside the application folder before extracting another release.
Updating the application can overwrite bundled folders.

Package sources and contribution rules are in
[ReRevved Mods](https://github.com/ReRevvedRecomp/rerevved-mods).

## If a mod does not work

- Check the folder depth. The manifest must be at `mods/<id>/mod.toml`, not
  `mods/mods/<id>/mod.toml`.
- Restart after saving. **Rescan** refreshes the list in F2 but does not reload
  rules into the running game.
- Test with other mods disabled to rule out a conflict.
- Disabling a mod does not undo what it already changed in a save. Keep save
  backups when testing.
- Do not keep backup copies of a package inside `mods/`; F2 finds them as
  extra packages.

## Asset overrides

The **Asset Overrides** tab manages asset packages separately from code mods.
The bundled ReRevved logo is enabled by default. Saving the list writes the
active profile's `asset_order.txt`; restart to apply the selection and priority
order. An explicitly saved empty list disables the default selection.

Profiles share the packages installed beside the executable but keep their
selections separate. See [Configuration](configuration.md) for backups and
[storage profiles](rexglue-runtime.md#storage-profiles) for the full contract.
