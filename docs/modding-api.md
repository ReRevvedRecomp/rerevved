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
[V2 amount rules](#v2-amounts).
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

## Building and Wonder effects

[`building_effect_rules.h`](../api/building_effect_rules.h) and
[`wonder_effect_rules.h`](../api/wonder_effect_rules.h) each provide a version
function, a support query, registration, and a stateless preview. The two
domains version independently. The Lua kinds `building_yield` and
`wonder_completion_technology` publish through their V2 entry points.

| Effect | Behavior |
|---|---|
| `BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD` | A Library in a Chinese-owned city adds the rule amount (1 in V1) to the city's final Gold, after native modifiers and overrides. Native Science is unchanged. Existing Libraries qualify. |
| `WONDER_EFFECT_STONEHENGE_RELIGION` | When Stonehenge newly completes, the completing city's owner acquires ordinary Religion once if it does not already have it. Every native Stonehenge effect, including Culture, is unchanged. |
| `WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE` | Not implemented. A valid registration returns `ERR_UNAVAILABLE` (-14), retains nothing, and changes no guest state. The ID stays reserved; treat it as an inactive effect. |

Activation, loading, transfer, reset, owner synchronization, and repeated
yield calculation are not a new completion, so no Religion grant is retroactive. Acquired Religion is ordinary
saved technology state and remains after the package is removed. Ongoing
effects follow the current city owner. Loadout changes and package removal
take effect after a restart. The effects expose no guest item numbers, table
slots, or callbacks.

### Effect requests and registration

Resolve the domain's version function and every required entry point before
use. Each request sets `structSize` to at least the declared size, zeroes its
reserved fields, and supplies provider and rule IDs of 1 to 63 bytes followed
by NUL. IDs start with a lowercase ASCII letter or digit and contain only
lowercase ASCII letters, digits, `.`, `_`, or `-`. The host validates IDs before reporting an unavailable
effect and retains no request bytes.

Support and preview outputs need at least 32 bytes. A null output returns
`ERR_INVALID_ARGUMENT`. The host clears the smaller of `outSize` and its own
output size, checks the output size before the inputs, and leaves any later
bytes untouched. On success, `structSize` reports the producer's size. Inputs
and outputs must not overlap. Support and preview calls are stateless,
thread-safe, and independent of gameplay availability. These APIs accept no
live handles.

Both support queries report `runtimeAvailable = 1`. Support is not a
reservation or an activation guarantee, so always check the registration
result.

A rule's identity is its provider and rule ID within its domain; V1 and V2
share that identity. Registering an identical normalized request again
returns `OK` without retaining a duplicate. A different request under a
retained identity returns `ERR_CONFLICT` (-12) and leaves the registry
unchanged. Library rules with distinct identities add their amounts in any
registration order. A request that would move the sum outside the signed
32-bit range returns `ERR_INVALID_ARGUMENT` and retains nothing. Religion rules
do not add together: any number of them grants Religion at most once per
completion. Registration is thread-safe, and retained rules last for the
process.

### V2 amounts

The independently versioned V2 entry points are:

| Building | Wonder |
|---|---|
| `BuildingEffectRulesV2AbiVersion` | `WonderEffectRulesV2AbiVersion` |
| `GetBuildingEffectSupportV2` | `GetWonderEffectSupportV2` |
| `RegisterBuildingEffectRuleV2` | `RegisterWonderEffectRuleV2` |
| `PreviewBuildingEffectV2` | `PreviewWonderEffectV2` |

Both V2 version functions return 2; the V1 functions and layouts are
unchanged. V2 uses the same effect IDs. A V1 request equals the V2 request with
amount 1 for the Library and amount 0 for Stonehenge.

`BuildingEffectRuleV2` and `WonderEffectRuleV2` are 160 bytes: `structSize`,
two 64-byte ID arrays, `effect`, a signed 32-bit `amount`, and five zeroed
reserved fields. The 32-byte V2 support records hold `structSize`,
`runtimeAvailable`, signed `minAmount` and `maxAmount`, and four zeroed
reserved fields. Building support reports `INT32_MIN` through `INT32_MAX`.
Wonder support reports 0 through 0, because the Religion grant has no amount;
any other Religion amount returns `ERR_INVALID_ARGUMENT`.

The Library amount is a flat addition to final Gold and may be positive, zero,
or negative; zero adds nothing. Amounts never undo acquired Religion or yields
already credited.

### Effect previews

Previews evaluate hypothetical inputs supplied by the caller. They have no
side effects: they do not register rules, detect completion, credit yields,
change caches, or acquire technology. A successful preview does not show that
an effect is live. V1 and V2 previews share the same query and result records.

- The Library preview takes the owner's civilization, Library presence, and
  final native Gold and Science. It adds the amount to Gold only for a
  Chinese-owned Library and returns Science unchanged.
- The Religion preview takes Stonehenge presence, new-completion and
  Religion-owned flags (each exactly 0 or 1), and native Culture. It returns
  Culture unchanged, reports `PREVIEW_MATCHED`, and reports Religion grant
  intent only for a new completion when Religion is not owned.
- The unavailable Culture replacement still previews: it adds the amount (5 in
  V1) to Culture that the caller has already reduced by the native Stonehenge
  effects, and reports replacement and Religion intent. Any signed 32-bit
  amount validates; zero or negative amounts keep that intent.

Yields use the signed 16-bit range; out-of-range input returns
`ERR_INVALID_ARGUMENT`. Addition is checked in a wider type and never wraps or
clamps. An overflowing result returns success with `PREVIEW_MATCHED` and
`PREVIEW_OVERFLOW`, keeps the supplied yields, and reports no Wonder grant or
replacement intent. The Religion grant adds no Culture and cannot overflow.

### Library Gold hooks

[building_effect_rules_hooks.cpp](../src/building_effect_rules_hooks.cpp) adds
the summed amount N of the retained Library rules in two places in city update
`0x82D13978`. Both run on the guest main thread.

| Phase | Hook | Effect for a matching city |
|---|---|---|
| Cache | Before `0x82D1537C`, after native overrides and before the mode -1 exit | Modes -1 and 0 set the signed 16-bit Gold cache at city `+0x46` to its native value plus N. Mode 0 also adds N to the owner's Gold aggregate (`0x830EE388 + owner*0x28 + 0x0C`), which native code accumulated earlier in the same call. Other modes are unchanged. |
| Credit | Before the reserve store `0x82D15610` | Adds N to the stored value, the old reserve plus native credit Gold, for the owner's reserve `0x830ED544 + owner*4`. The native credit input at stack `+0x7C` is unchanged. |

A city matches when its owner index is a valid player, the live owner byte at
city `+0x00` equals the owner used for the aggregate and reserve rows, that
player's civilization at `0x830ECD28 + owner*4` is Chinese (6), and the Library
flag (`0x20` of the word at city `+0x10`) is set. Each phase re-reads these
inputs. A city whose owner bytes at `+0x00` and `+0x01` both equal the excluded
player word `0x82F700B0` never matches, because native code zeroes its credit
after the cache phase. Native Science and the Science aggregate are never
written.

Before writing anything, the cache phase checks the 16-bit cache, the 32-bit
aggregate in mode 0, and the reserve sum the credit will need. Any overflow or
unreadable input leaves that call native. In mode 0 it records N for the same
call's credit phase, keyed by the guest stack frame, so a later registry change
cannot split one call's display and credit. The credit phase rechecks the
32-bit sum; if the reserve changed so that the check fails, that call's credit
stays native while its cache and aggregate keep N. Neither phase wraps or
clamps.

N is added for each executed mode 0 credit, not once per city per turn. The
turn loop can make more than one mode 0 call for a city, and a cached turn path
can skip the credit; native Gold follows the same cadence. Ownership changes
take effect at the next call. The city Gold total is recomputed through mode -1
when read, so it includes N.

### Stonehenge Religion hooks

[wonder_effect_rules_hooks.cpp](../src/wonder_effect_rules_hooks.cpp) hooks
city update `0x82D13978` before `0x82D16344`, right after the guarded call at
`0x82D16340` to the Wonder claim helper `0x82D16BA0` returns. Native code
reaches that call only after sufficient production, an unclaimed slot, and
eligibility checks, so it marks a new production claim. A claim from any
origin that reaches this edge qualifies.
It runs on the guest thread executing city update. At the hook, `r27` holds the
normalized Wonder index and `r23` holds the city index.

The hook grants Religion only when all of these hold:

- A Religion grant rule is retained.
- The Wonder index is 3, Stonehenge, and the city index is in [0, 128).
- The Stonehenge claim city word `0x830E5164` equals that city, and the claim
  owner word `0x830E5160` equals the signed live owner byte at
  `0x8314FE28 + city*0xBC`.
- The owner is nonnegative, below the signed player count `0x82F700AC`, below
  32, and not the excluded player `0x82F700B0`.
- Every guarded guest read succeeds.

Otherwise the call stays native. When the gates hold, the hook checks that its
registers belong to the current guest thread, then runs guest code through the
SDK dispatcher's trap entry, which saves and restores the interrupted register
state. The availability query `0x82CEF6A8(19, owner)` runs first. Only a result
of exactly 0 continues to the ordinary acquisition
`0x82D09208(owner, 19, city, 5, 1)`: Religion 19, the completing city, the
generic Wonder reward source 5, and state fan-out phase 1. A nested hook hit
does nothing, and no host registry lock is held across these calls. The grant
adds no separate Religion message.

A human player's ordinary new completion without Religion acquired Religion
once, with native Culture and completion presentation unchanged. Completion
with Religion already owned, capture or transfer, save/load after the grant,
AI completion and strategy, hurry, scenario and network ingress, and
multiplayer replication are unverified. Whether the excluded player can reach
this edge is unproved; that owner stays native. If a restored claim slot reads
-1 while a city has enough production invested, Stonehenge can complete
natively again and the hook treats it as a new claim; the availability query
still prevents a second acquisition. There is no recovery policy for corrupted
claim state.

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
  applies it at the opener for type 11. An invalid or unavailable payload
  keeps the original asset. There is no public ABI for overriding asset
  files or general API for replacing assets.

These interfaces do not provide raw access to guest memory, save modification,
runtime event scripting, or a general API for live objects.
