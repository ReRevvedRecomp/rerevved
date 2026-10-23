# Mod APIs

ReRevved provides a [Lua rules contract](#lua-rules-contract-1) and C ABIs for
native mods. Use [Mods](mods.md) to install or enable a package; this page is
for authors integrating with the title.

Native mods resolve functions from the running title, check the matching ABI
version, and exchange fixed-width caller-owned data. Public values do not
expose guest pointers, guest addresses, or borrowed strings.

## Public headers

| Header | Contract |
|---|---|
| [`game_ids.h`](../api/game_ids.h) | Shared civilization, unit type, unit identity, and display-form IDs. |
| [`gameplay_state.h`](../api/gameplay_state.h) | Read-only snapshot of gameplay availability and the active player. |
| [`nation_select_text.h`](../api/nation_select_text.h) | Civilization information screen text replacements. |
| [`unit_catalog.h`](../api/unit_catalog.h) | Static unit definitions and unit identity resolution for each civilization. |
| [`terrain_yield_rules.h`](../api/terrain_yield_rules.h) | Registration and evaluation of additive terrain yield rules. |
| [`unit_movement_rules.h`](../api/unit_movement_rules.h) | Register and evaluate rules that add to base movement for a selected unit identity. |
| [`unit_production_cost_rules.h`](../api/unit_production_cost_rules.h) | Register and evaluate additive production cost percentages for a selected unit identity. |
| [`unit_combat_rules.h`](../api/unit_combat_rules.h) | Register and evaluate Forest attack and defense percentages for a selected unit identity. |
| [`unit_effect_rules.h`](../api/unit_effect_rules.h) | Register and evaluate named effects applied when a unit is created. |
| [`unique_unit_rules.h`](../api/unique_unit_rules.h) | Registration and evaluation of Unique Unit base attack and defense rules. |
| [`unique_era_abilities.h`](../api/unique_era_abilities.h) | Registration and evaluation of supported Unique Era Ability replacements. |
| [`building_effect_rules.h`](../api/building_effect_rules.h) | Registration, readiness query, and stateless preview of the Chinese Library Gold addition. |
| [`wonder_effect_rules.h`](../api/wonder_effect_rules.h) | Registration, readiness query, and stateless preview of Stonehenge effects. The Religion grant registers live rules; the Culture replacement is unavailable. |

The headers define the authoritative layouts, version constants, result codes,
function-pointer types, and size rules. An incompatible layout change requires
a new ABI version. Existing semantic IDs are never reassigned.

## Using an API

1. Include the exact public header from the title revision you target.
2. Resolve the function that reports the ABI version and the required entry
   points from the host process.
3. Reject a missing function or unsupported ABI version before making another call.
4. Initialize each request or result structure exactly as its header requires,
   including `structSize` where present.
5. Handle the declared result codes. Do not inspect guest memory or assume an undocumented field.

Related documentation:

- [Making mods](https://github.com/ReRevvedRecomp/rerevved-mods/blob/main/docs/making-mods.md) - source, manifest, build, and package layout
- [SDK mod system](https://github.com/ReRevvedRecomp/rerevved-sdk/blob/main/docs/mod-system.md) - reusable plugin loading and lifecycle

Game-specific IDs and behavior remain in this repository.

## Lua rules contract 1

Lua packages use `manifest_version = 2` and a payload such as:

```toml
[payload]
kind = "lua"
entry = "main.lua"
api = "rerevved.rules"
api_version = 1
requires = ["terrain_yield"]
```

`requires` declares every kind the script can return. Unused declarations are
allowed. The title advertises all eleven kinds below.

The SDK executes the source once at module launch, before guest execution.
Return exactly one plain table with exactly `provider_id` and `rules`:

```lua
return {
    provider_id = "example.hills",
    rules = {
        {
            id = "hills-production", kind = "terrain_yield",
            terrain = "hill", component = "production",
            operation = "add", value = 1,
        },
    },
}
```

`rules` is a dense array with integer keys 1 through n and 1 to 256 records.
Each record has string `id` and `kind`, plus exactly these fields:

| Kind | Additional required fields |
|---|---|
| `terrain_yield` | `terrain`, `component`, `operation`, `value` |
| `unit_combat` | `civilization`, `base_unit_type`, `identity`, `terrain`, `property`, `percentage_delta` |
| `unique_unit_scalar` | `civilization`, `base_unit_type`, `identity`, `property`, `operation`, `value` |
| `unit_movement` | `civilization`, `base_unit_type`, `identity`, `value` |
| `unit_effect` | `civilization`, `base_unit_type`, `identity`, `effect` |
| `unit_production_cost` | `civilization`, `base_unit_type`, `identity`, `percentage_delta` |
| `era_technology_ability` | `civilization`, `unlock_era`, `technology` |
| `nation_select_text` | `surface`, `text`, and the surface selectors below |
| `building_yield` | `civilization`, `building`, `yield`, `amount` |
| `wonder_completion_technology` | `wonder`, `technology` |
| `city_founding_unit` | `unit`, `upgrades` (array) |

Text surface `unique_unit` requires `civilization`, `base_unit_type`,
`identity`, and `display_form = "unit"`. Surface `era_ability` requires
`civilization`, `unlock_era`, and `ability`. Fields from the other surface
are rejected. The title supplies all unused native selectors and reserved
fields. Text is authored by the script; cost edits need a matching accurate
description and title readback.

Contract 1 accepts only the following tokens and valid native selector
combinations:

| Field | Tokens |
|---|---|
| `civilization` | `china` for `building_yield`; `aztec`, `roman`, `greek`, `mongolian` for other kinds |
| `base_unit_type` | `warrior`, `knights`, `horsemen`, `phalanx` |
| `identity` | `jaguar_warrior`, `cataphract`, `keshik`, `hoplite` |
| `terrain` | `hill`, `forest`; combat accepts only `forest` |
| `component` | `production` |
| `operation` | `add` |
| Combat `property` | `attack`, `defense` |
| Scalar `property` | `base_defense` |
| Unit `effect` | `creation_loyalty` |
| `building`, `yield` | `library`, `gold` respectively |
| `wonder` | `stonehenge` |
| `technology` | `religion` for `wonder_completion_technology`; `horseback_riding` for `era_technology_ability` |
| `unit`, `upgrades` entries | `militia`, `loyalty` respectively |
| `unlock_era` | `ancient` |
| Text `ability` | `knowledge_of_horseback_riding` |
| `surface` | `unique_unit`, `era_ability` |
| `display_form` | `unit` |

All fields are strings except the `upgrades` array and numeric `value`,
`percentage_delta`, and `amount`. Numbers must be finite, exactly integral
signed 32-bit values; integral floats are accepted.
Numeric strings and fractions are rejected. Percentage deltas are percentage
points, and native domain validation still applies.

`building_yield` adds its signed `amount` to final city Gold for Chinese-owned
Libraries, preserving native Science. Its amount follows the
[V2 adjustable amount contract](building-wonder-effects.md#v2-adjustable-amounts).
The conventional Library amount is 1, and zero is accepted.
`wonder_completion_technology` grants Religion on a new Stonehenge completion,
preserving native Culture. It has no `amount` field. Both kinds map to existing
native V2 effect requests with the bundle's provider ID and zeroed reserved
fields; the Wonder request uses native amount 0.

`era_technology_ability` replaces the selected civilization's era ability with
the native ability that grants Horseback Riding. Its Nation Select text uses
surface `era_ability` and `ability = "knowledge_of_horseback_riding"`.
`city_founding_unit` requires exactly `upgrades = {"loyalty"}` and grants one
Militia on an admitted founding. These parameter values are the complete
supported set; unsupported values report the field and its supported values.

Provider and rule IDs are 1 to 63 ASCII bytes, start with a lowercase letter
or digit, and then contain only lowercase letters, digits, `.`, `_`, or `-`.
Text is 1 to 255 printable ASCII bytes. Strings are never coerced, trimmed,
or truncated and cannot contain embedded NUL. Unknown fields, non-string
record keys, array holes, and undeclared returned kinds are rejected. The SDK
also rejects metatables, cycles, and non-data return values.

Publication is all-or-nothing per package across all affected native registries.
A rejected package leaves earlier bundles intact. Identical normalized replay
is idempotent; a changed request under the same domain, provider, and rule ID
fails. Native registry ordering and composition determine evaluation.

Diagnostics distinguish `schema`, `support`, and `publish` failures. Schema
errors identify the rule index, ID, and field when available, for example
`rules[1] hills-production: field value: expected integer, got string`.
Publication errors include the native result code and state that no rules from
the bundle were published. Unsupported kinds or an API/version mismatch report
`support`. The SDK adds the package and entry filename to the host diagnostic.

After schema validation and before any publication, the host queries V2
support for every effect domain in the bundle. A failed or unavailable support
query rejects the whole bundle at `support`. Building and Wonder effect records
then publish with the bundle's other rules. Earlier packages remain intact.
An advertised Lua binding or a successful
stateless preview does not imply that effect registration is available.

## City founding unit rule

The `city_founding_unit` kind in `rerevved.rules` version 1 enables one extra
ordinary Militia (native type 7) with Loyalty for each admitted successful
founding. It defaults off when no rule is published. Declare
`requires = ["city_founding_unit"]` in version 2 of the Lua payload manifest and
return:

```lua
return {
    provider_id = "example.city-founding-unit",
    rules = {
        {
            id = "city-founding-unit", kind = "city_founding_unit",
            unit = "militia", upgrades = {"loyalty"},
        },
    },
}
```

This kind accepts one provider/rule identity. Identical normalized replay is
idempotent; another identity rejects the entire bundle. Only `unit = "militia"`
and the dense one-element array `upgrades = {"loyalty"}` are supported.
An empty array, duplicate upgrades, or another unit or upgrade is rejected.
Omitting the rule preserves native founding behavior.

The hook at `0x82D1A314` admits saved CityCreate return PCs `0x82CE03AC`
(command founding) and `0x82CBC040` (AI founding), then validates the live
city, actual owner and coordinates. Barbarian owners are excluded. The
inventoried capture and setup callers fail this whitelist; deserialization
does not enter either admitted origin. Native and scenario defenders remain
in addition to the extra unit. A guard blocks recursive or overlapping grants
for the same active founding, without persisting state for city slots across saves
or later invocations.

Creation calls native `0x82D11750(owner, 7, x, y)` once. Result `-1` fails;
slot zero is valid. The returned record must retain its owner, type, slot,
coordinates and object back-pointer. Only that unit's big-endian upgrade word
at `+0x10` receives Loyalty bit `0x8`; rank and movement are unchanged.
Failures log and are never retried. Optional `--city_founding_unit_trace=true`
logs each hook's gate decision, creator result and Loyalty write; it defaults
off.

The public implementation is in [city_founding_unit_hooks.cpp](../src/city_founding_unit_hooks.cpp)
and [city_founding_unit_rules.cpp](../src/city_founding_unit_rules.cpp), with hook
locations in
[city_effect_hooks.toml](../config/city_effect_hooks.toml). Universal
human/AI event delivery and durable
replay exclusion are unproved. Tests in the running title must establish the
lifetimes of the creator, model, and manager. They must verify one extra unit on
each founding route and that capture, setup, and load paths are excluded. They
must also verify Loyalty defense and retention across save/load, and
presentation after the grant. Multiplayer authority and synchronization require
separate validation.

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
  scalar for effective unit cost after native discounts. Static code establishes
  that the recovered code that uses unit costs for completion, rush, network
  state, and AI uses this helper. A Windows x64 runtime check confirmed that an
  Aztec Jaguar rule of -50 percentage points changed its displayed production
  cost from 10 to 5 and made a +2 Production queue report three turns.
  Completion, rush application, AI, network, save, and multiplayer behavior
  remain unproved.
- Unit combat rules add signed percentage points to the native attack or defense
  accumulator for an accepted civilization, base type, Unit Catalog identity,
  semantic Forest terrain, and ATTACK or DEFENSE property. ABI 2 accepts only
  Forest. The attack callback joins the native modifier call at `0x82CDABBC`
  using the initialized `r16` accumulator, and defense joins at `0x82CDAC10`
  using `r17`; native call ordering and Guerilla behavior remain in the guest
  path. A nonzero rule also appends `Woodsman {signed}%` to the native
  attack text buffer at `r1+336` or defense buffer at `r1+592` when the full
  256-byte guest range, existing NUL, and formatted line fit checks succeed.
  Checked evaluation and accumulator arithmetic preserve native fallback. The
  Jaguar Woodsman +50% Forest attack and defense bonus passed bounded gameplay
  acceptance. Combat promotions, AI, network, save, scenario, and multiplayer
  paths remain outside that accepted boundary.
- Unit effect rules use ABI 3 and expose the Veteran grant at creation plus
  the nine named native special upgrade effects: Blitz, Infiltration, Guerilla,
  Loyalty, Engineer, Leadership, March, Medic, and Scout. They match
  civilization, base type, and the accepted Unit Catalog identity. Veteran
  raises a lower creation rank to level two and preserves a higher native rank;
  the named special effects report a grant while preserving the queried native
  rank. At creation, the title ORs granted special upgrades into the native upgrade word
  without spending rank; March also adds one movement point. The Veteran path
  preserves the native `+0x3C` gate, UEA 50 route, and saturation at a maximum of two.
  A Windows x64 runtime check confirmed that a newly created Jaguar Warrior
  received and displayed a registered Guerilla grant. Guerilla combat behavior
  and runtime coverage for the other named effects, combat promotion, AI, save,
  scenario, multiplayer, and persistence remain unproved.
- Unique Unit rules compose at the documented boundary for base stats before the
  title applies its native modifiers.
- Unique Era Ability rules cover only the IDs and effects documented by that ABI.
- Nation Select Text rules replace complete printable ASCII fields on the
  civilization information screen. ABI 2 supports leader, civilization, and
  trait fields for each civilization; the four global era labels; rows selected
  by civilization and effective ability; the global Unique Unit heading; and
  Unique Unit rows selected by civilization and identity. Multiple matching rules
  preserve native text.
- The title selects the main menu logo internally. At startup the title
  asks the SDK resolver for `file-data/GFX_MainMenu_logo.dds`, validates the
  selected package payload as the supported legacy 1024x256 A8R8G8B8 DDS, and
  applies it at the opener for type 11. There is no public ABI for overriding asset
  files or general API for replacing assets.

These interfaces do not provide raw access to guest memory, save modification,
runtime event scripting, or a general API for live objects.

The [Building and Wonder effect contracts](building-wonder-effects.md) define
the effects, validation and arithmetic, the live Library Gold and Stonehenge
Religion integrations, and their remaining limits. Preview results do not
establish live gameplay support.
