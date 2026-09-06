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
| [`unit_movement_rules.h`](../api/unit_movement_rules.h) | Registration and evaluation of identity-targeted additive base movement rules. |
| [`unit_production_cost_rules.h`](../api/unit_production_cost_rules.h) | Registration and evaluation of identity-targeted additive production cost percentages. |
| [`unit_combat_rules.h`](../api/unit_combat_rules.h) | Registration and evaluation of identity-targeted Forest attack and defense percentages. |
| [`unit_effect_rules.h`](../api/unit_effect_rules.h) | Registration and evaluation of named creation-time unit effects. |
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
- Unit movement rules add signed values to the completed native result on the
  ordinary `EffectiveUnitMovementLookup` return path. They match civilization,
  base type, and the accepted Unit Catalog identity. The special return,
  movement budget and command cost, AI, and presentation coverage are unproved.
- Unit production cost rules add signed percentage points to the native 100
  percent cost for an accepted civilization, base type, and Unit Catalog
  identity. The title applies the resulting positive percentage to its shared
  effective unit-cost scalar after native discounts. Static code establishes
  that the recovered completion, rush, network-state, and AI unit-cost
  consumers use this helper. A Windows x64 runtime check confirmed that an
  Aztec Jaguar rule of -50 percentage points changed its displayed production
  cost from 10 to 5 and made a +2 Production queue report three turns.
  Completion, rush application, AI, network, save, and multiplayer behavior
  remain unproved.
- Unit combat rules add signed percentage points to the native attack or defense
  accumulator for an accepted civilization, base type, Unit Catalog identity,
  semantic Forest terrain, and ATTACK or DEFENSE property. ABI 1 accepts only
  Forest. The attack callback joins the native modifier call at `0x82CDABBC`
  using the initialized `r16` accumulator, and defense joins at `0x82CDAC10`
  using `r17`; native call ordering and Guerilla behavior remain in the guest
  path. A nonzero rule also appends `Woodsman {signed}%` to the native
  attack text buffer at `r1+336` or defense buffer at `r1+592` when the full
  256-byte guest range, existing NUL, and formatted line fit checks succeed.
  Checked evaluation and accumulator arithmetic preserve native fallback. The
  exact runtime acceptance of ordinary attack and defense CombatResolve
  consumers, including combat promotions, AI, network, save, scenario, and
  multiplayer paths, remains required.
- Unit effect rules use ABI 2 and expose the creation-time Veteran grant plus
  the nine named native special upgrade effects: Blitz, Infiltration, Guerilla,
  Loyalty, Engineer, Leadership, March, Medic, and Scout. They match
  civilization, base type, and the accepted Unit Catalog identity. Veteran
  raises a lower creation rank to level two and preserves a higher native rank;
  the named special effects report a grant while preserving the queried native
  rank. The ABI 1 record layouts and Veteran effect ID remain unchanged. At
  creation, the title ORs granted special upgrades into the native upgrade word
  without spending rank; March also adds one movement point. The Veteran path
  preserves the native `+0x3C` gate, UEA 50 route, and maximum-two saturation.
  A Windows x64 runtime check confirmed that a newly created Jaguar Warrior
  received and displayed a registered Guerilla grant. Guerilla combat behavior
  and runtime coverage for the other named effects, combat promotion, AI, save,
  scenario, multiplayer, and persistence remain unproved.
- Unique Unit rules compose at the documented base-stat boundary before the title applies its native modifiers.
- Unique Era Ability rules cover only the IDs and effects documented by that ABI.
- Registration records are copied by the host. Provider and rule identifiers must obey their header's capacities and validation rules.

These interfaces do not provide raw guest-memory access, asset replacement,
save modification, scripting, or a general live-object API.
