# Mod APIs

ReRevved exposes small C ABIs for native mods. Mods resolve these functions
from the running title, check the matching ABI version, and exchange only
fixed-width caller-owned data. Public values do not expose guest pointers,
guest addresses, or borrowed strings.

## Public headers

| Header | Contract |
|---|---|
| [`game_ids.h`](../api/game_ids.h) | Shared civilization, unit type, unit identity, and display-form IDs. |
| [`gameplay_state.h`](../api/gameplay_state.h) | Read-only gameplay availability and active-player snapshot. |
| [`unit_catalog.h`](../api/unit_catalog.h) | Static unit definitions and civilization-specific unit identity resolution. |
| [`unique_unit_rules.h`](../api/unique_unit_rules.h) | Registration and evaluation of Unique Unit base attack and defense rules. |
| [`unique_era_abilities.h`](../api/unique_era_abilities.h) | Registration and evaluation of supported Unique Era Ability replacements. |

The headers define the authoritative layouts, version constants, result codes,
function-pointer types, and size rules. An incompatible layout change requires
a new ABI version. Existing semantic IDs are never reassigned.

## Using an API

1. Include the exact public header mirrored by the mod repository.
2. Resolve the ABI-version function and required entry points from the host process.
3. Reject a missing function or unsupported ABI version before making another call.
4. Initialize each request or result structure exactly as its header requires,
   including `struct_size` where present.
5. Handle the declared result codes. Do not inspect guest memory or assume an undocumented field.

Related documentation:

- [Making mods](https://github.com/ReRevvedRecomp/rerevved-mods/blob/main/docs/making-mods.md) - source, manifest, build, and package layout
- [SDK mod system](https://github.com/ReRevvedRecomp/rerevved-sdk/blob/main/docs/mod-system.md) - reusable plugin loading and lifecycle

Game-specific IDs and behavior remain in this repository.

## Supported boundaries

- Gameplay state is a read-only snapshot. Validity bits identify meaningful fields.
- The unit catalog is static and does not enumerate live game objects.
- Unique Unit rules compose at the documented base-stat boundary before the title applies its native modifiers.
- Unique Era Ability rules cover only the IDs and effects documented by that ABI.
- Registration records are copied by the host. Provider and rule identifiers must obey their header's capacities and validation rules.

These interfaces do not provide raw guest-memory access, asset replacement,
save modification, scripting, or a general live-object API.
