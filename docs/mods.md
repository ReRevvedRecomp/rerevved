# Mods and asset overrides

F2 opens the mod manager. The Windows release includes first-party Lua mods
under `mods/` beside the executable. They start disabled when the active
profile has no saved loadout.

## Enable or change mods

1. Open F2 and review each package's compatibility and activation messages.
2. Enable the mods you want and set their load order.
3. Select **Save changes**. Lua mods take effect right away. Native DLL mods
   take effect after a restart.

Apply replaces the complete Lua loadout, including enabled packages, load order,
and edits to their Lua rules. Disabling every Lua mod removes its published
rules. A failed script, validation, preparation, or save leaves the running
rules and saved loadout unchanged. F2 shows the failure so you can repair it and
apply again. The active profile's `mod_order.txt` records the selection.

Game of the Week with Standard Rules runs without Lua mod rules. F2 still lists
the selection, and Apply succeeds during such a game, but the new selection
applies from your next game or a save load. Native DLL mods and asset packs stay
active.

F2 locks mod changes during a multiplayer game. Leave the network game session
to change mods, then save them before creating or joining the next session.
[Online play](online-play.md#gameplay-settings-and-mods) describes the required
matching loadouts.

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
- After editing a Lua package, select **Save changes** even if its selection
  has not changed. **Rescan** refreshes the list without executing Lua.
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
