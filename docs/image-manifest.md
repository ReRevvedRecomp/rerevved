# Image manifest

[image-manifest.csv](image-manifest.csv) records 1,130 images extracted from
Civilization Revolution game content through a Studio-assisted export. It
preserves the original game archive and resource names alongside the exported
PNG paths and dimensions. Image payloads are not included in this repository.

## Fields

| Column | Meaning |
|---|---|
| `category` | Export grouping for browsing related images. |
| `file` | Relative path of the exported PNG preview. |
| `width`, `height` | Exported image dimensions in pixels. |
| `source_archive` | Original game archive containing the resource. |
| `source_name` | Original resource name, with a character locator for a bitmap embedded in an SWF. |

For example, `Pregame.FPK` and `gfx_mainmenu_logo.dds` identify the game source
for `ui_external/gfx_mainmenu_logo.png`. Embedded bitmap locators such as
`gfx_groundarrow.swf#character_1` identify an SWF resource and its character ID;
the suffix is a locator added by the export, not part of the archive filename.

The export groups filename-identified icon/UI resources under `misc`, images
declared externally by GFx movies under `ui_external`, and bitmap images stored
inside SWF movies under `embedded_swf`. Categories and preview filenames are
export metadata. The source columns retain the game's names and spelling.

## Runtime overrides

The manifest provides source references for image inspection and future
adapters. Each runtime override needs a proved loader key, native payload
format, dimension constraints, and fallback behavior. An exported PNG path
does not establish those properties.

The title currently supports one image override:

| Game source | Runtime override key | Accepted payload |
|---|---|---|
| `Pregame.FPK` / `gfx_mainmenu_logo.dds` | `file-data/GFX_MainMenu_logo.dds` | Legacy uncompressed 1024x256 A8R8G8B8 DDS |

The main-menu logo override was runtime-tested. Invalid or unavailable payloads
preserve the original game asset. Other manifest entries remain source
references without supported runtime keys.

Asset override packs are separate from native code mods. The first matching
pack in the selected top-to-bottom asset order wins. ReRevved releases include
`rerevved-logo` as an ordinary pack, selected by the bundled default order until
the player saves a profile-specific asset order.

## Provenance and maintenance

The manifest was derived from the image export's `index.csv`, produced from
game content with assistance from ReRevved Studio tooling. The export's
provenance record identifies the game-source archive/name pairs and the PNG
conversion; source DDS payloads and temporary conversion files were not retained
in the export.

When refreshing the manifest, compare every row with the export index and
preserve the original source names, dimensions, and SWF character locators.
Preserve the existing row order when updating entries. Export folders can be
renamed without changing the original game-source identities.

Studio owns inspection and export tooling. The title and SDK own runtime
loading and package priority. Add a supported runtime key only after native
format validation, malformed-input and priority tests, and Windows runtime
verification pass for that resource.
