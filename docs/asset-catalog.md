# Asset catalog

This catalog records the image inventory exported from the locally owned
`CivRev Icon Export` tree. The companion [asset-catalog.csv](asset-catalog.csv)
contains all 1,130 rows from `index.csv`, sorted by category, source archive,
source name, and exported file path. No image payloads are copied into this
repository.

The `file` column names an exported PNG reference. It is not a runtime overlay
key. Exported PNGs were made for inspection and cataloging; they do not prove
that the title can replace the corresponding game resource, that the PNG is the
native format, or that the resource has a stable public loader name.

## Runtime overlay status

The planned asset-only packages are separate from native code mods. Their files
will be keyed by the original runtime resource path and participate in the
selected asset load order. The first matching package at the top of that order
will win. The previously proven main-menu logo adapter supports one key:

| Catalog reference | Runtime overlay key | Status |
|---|---|---|
| `ui_external/gfx_mainmenu_logo.png` (`Pregame.FPK`, `gfx_mainmenu_logo.dds`) | `file-data/GFX_MainMenu_logo.dds` | Supported and runtime-tested |

The logo adapter accepts the supported legacy DDS payload and keeps the
original game asset as the fallback. Every other catalog row is
**reference-only/unmapped**. A row must not be treated as replaceable merely
because an exported PNG exists.

ReRevved releases include `rerevved-logo` as an ordinary asset override pack.
It appears in the Asset Overrides tab and is selected by the bundled default
order until the player saves a profile-specific asset order.

The catalog currently contains these category counts:

| Category | Entries |
|---|---:|
| buildings | 22 |
| categories | 17 |
| civilizations | 18 |
| concepts | 15 |
| controller_glyphs | 41 |
| cursor_actions | 44 |
| embedded_swf | 29 |
| entertainers | 10 |
| governments | 7 |
| great_people | 51 |
| great_people_portraits | 49 |
| leaders | 20 |
| leaders_large | 18 |
| media_icons | 5 |
| misc | 80 |
| nation_symbols | 17 |
| pedia_art | 324 |
| relics | 6 |
| resources | 23 |
| symbol_atlases | 1 |
| technologies | 48 |
| terrains | 8 |
| ui_external | 161 |
| unit_status | 11 |
| unit_upgrades | 10 |
| unit_upgrades_small | 9 |
| units | 46 |
| wonders | 21 |
| yield_icons | 19 |
| **Total** | **1,130** |

The source export describes `misc` as filename-identified icon/UI resources,
`ui_external` as images declared directly by GFx movies, and `embedded_swf` as
non-empty bitmap images stored inside SWF movies. Those distinctions help
triage future work but do not grant any category automatic runtime support.

## Graduating a reference to a supported adapter

When a resource is ready for runtime use, record the original archive/name
pair, exact runtime key, native format and dimension constraints, and the
fallback behavior. Then add a title-owned adapter that resolves that key
through the asset overlay loader, add malformed-input and priority tests, and
exercise the result in a Windows x64 runtime launch. Only after those checks
should the row receive a supported status and an entry in the runtime overlay
table above. If the exported PNG is only a preview, keep it as a reference and
document the required native payload instead of claiming that a PNG overlay is
accepted.

The asset map is intentionally an inventory rather than an FPK repacking plan.
Studio can use it for browsing, inspection, and future extraction/export
workflows; runtime loading and package priority remain title/SDK concerns.

## Source and maintenance

The source inventory was the local `CivRev Icon Export/index.csv` as of
2026-09-06. It identifies each exported PNG, dimensions, source archive, and
source name. Rebuild the tracked CSV from that source when the export changes,
preserving the deterministic sort order and updating the counts above.
