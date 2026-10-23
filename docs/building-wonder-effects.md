# Building and Wonder effects

Use this reference when implementing the Building or Wonder domains from the
[Mod APIs](modding-api.md).

Building Effect Rules ABI 1 and Wonder Effect Rules ABI 1 expose semantic
declarations, readiness queries, stateless previews, and runtime registration.
**Building Effect Rules register live rules**; the
[Library Gold integration](#library-gold-integration) describes the guest
hooks. **Wonder Effect Rules register live rules for
`WONDER_EFFECT_STONEHENGE_RELIGION`**; the
[Stonehenge Religion integration](#stonehenge-religion-integration) describes
the guest hook. **`WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE` is unavailable.**
A valid registration of it returns `ERR_UNAVAILABLE`, retains no rule, and
performs no guest mutation. Mods must treat that result as an inactive effect.
Matching the ABI version or getting a successful preview does not establish
gameplay support.

## Contracts

| Header and effect | Semantic declaration |
|---|---|
| [`building_effect_rules.h`](../api/building_effect_rules.h), `BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD` | An existing Library in a Chinese-owned city preserves native science and adds the rule amount (exactly 1 for V1) to the city's final Gold after native modifiers and overrides. Existing Libraries qualify. |
| [`wonder_effect_rules.h`](../api/wonder_effect_rules.h), `WONDER_EFFECT_STONEHENGE_RELIGION` | Stonehenge keeps every native effect, including Culture. On genuine new completion, the completing city's owner acquires ordinary Religion once when it does not already have it. Existing Stonehenge grants nothing. |
| [`wonder_effect_rules.h`](../api/wonder_effect_rules.h), `WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE` | Unavailable. Declares replacing Stonehenge effects, granting ordinary Religion once to the completing city's owner on new completion, and adding exactly 5 Culture per city turn to its containing city after native modifiers and overrides. Existing Stonehenge would qualify for ongoing Culture without an activation grant. |

The effects preserve ordinary identities, ownership, construction requirements,
costs, technology ownership, and saved state. Transfer selects the current city
owner for ongoing effects. Activation, loading, transfer, and repeated yield
calculation do not count as new completion. Acquired Religion remains ordinary
technology state after package removal. Loadout changes apply after restart.
These declarations do not expose guest item numbers, table slots, or callbacks.

## Calling convention

Resolve the domain version and every required function before use. The domains
version independently. Requests supply at least the declared `structSize`,
zero reserved fields, and provider/rule IDs of one to 63 bytes followed by NUL.
IDs start with a lowercase ASCII letter or digit and contain only lowercase
ASCII letters, digits, `.`, `_`, or `-`. The host validates IDs before reporting
unavailable integration. No request bytes are retained.

Support and preview outputs require at least 32 bytes. Null output returns
`ERR_INVALID_ARGUMENT`. The host clears the smaller of `outSize` and its current
output size, then checks output size before inputs. Bytes beyond the current
output size remain untouched. On success, `structSize` reports the producer
size. Inputs and outputs must not overlap. Support and preview calls are
stateless, thread-safe, and independent of gameplay availability. Registration
retains rules as described below. These APIs do not accept live handles.

`GetBuildingEffectSupport` and `GetWonderEffectSupport` both report
`runtimeAvailable = 1`. Wonder support describes
`WONDER_EFFECT_STONEHENGE_RELIGION`; Culture replacement registration
returns `ERR_UNAVAILABLE`. Callers must check registration's result
rather than treating support as a reservation or activation guarantee.

## Building registration

A Building rule's identity is its provider and rule ID. V1 and V2 share one
identity space: a V1 request is the V2 request with amount 1. Registering an
identical normalized request again returns `OK` without retaining a duplicate.
A changed amount under a retained identity returns `ERR_CONFLICT` (-12) and
leaves the registry unchanged. Rules with distinct identities compose by
summing their amounts, independent of registration order. A request whose
amount would move that sum outside the signed 32-bit range is rejected and
retains nothing. Registration is thread-safe. Retained rules last for the
process; removing a package takes effect after restart.

## Wonder registration

A Wonder rule's identity is its provider and rule ID. V1 and V2 share one
identity space: a V1 request is the V2 request with amount 0.
`WONDER_EFFECT_STONEHENGE_RELIGION` has no amount, so a V2 request must supply
amount 0. Any other amount returns `ERR_INVALID_ARGUMENT`. Registering an
identical normalized request again returns `OK` without retaining a duplicate.
A different normalized request under a retained identity returns
`ERR_CONFLICT` (-12) and leaves the registry unchanged. With one live effect
and a fixed amount, every valid request for one identity normalizes
identically. A valid `WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE` request
returns `ERR_UNAVAILABLE` (-14) and retains nothing, including under a retained
identity. Retained Religion rules do not add together: any number of them
grants Religion at most once per completion. Registration is thread-safe.
Retained rules last for the process; removing a package takes effect after
restart.

## Previews

`PreviewBuildingEffect` uses the caller's hypothetical inputs: the current
owner's civilization, Library presence, and final native Gold and Science. It preserves
Science and adds 1 Gold only for a Chinese-owned Library. `PreviewWonderEffect`
uses hypothetical Stonehenge presence, flags for new completion and Religion
ownership, and a Culture value. Flags must be exactly zero or one. For
`WONDER_EFFECT_STONEHENGE_RELIGION`, the Culture value is native Culture and is
returned unchanged. The preview reports `PREVIEW_MATCHED` for the containing
city, no replacement intent, and Religion grant intent only for new completion
when Religion is not owned. For `WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE`,
the Culture value is taken after native Stonehenge effects have already been
removed. The preview does not calculate that native removal. It reports
replacement intent and adds 5 Culture for the containing city, with the same
Religion grant intent.

Preview yields use the supported signed 16-bit range. Out-of-range input returns
`ERR_INVALID_ARGUMENT`. Addition uses wider checked arithmetic. An overflowing
result preserves the supplied baseline and reports `PREVIEW_OVERFLOW`; the
Culture replacement preview also clears action intent. The Religion grant adds
no Culture and cannot overflow. This fallback is a hypothetical
baseline, not a reconstructed native effect. Previews do not detect or
deduplicate completion, credit a treasury, alter a cache, or acquire technology.
Repeated previews have no side effects.

## Guest integration limits

These limits keep `WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE` unavailable. The
Religion grant follows its own
[Stonehenge Religion integration](#stonehenge-religion-integration) contract.
The implementation in [building_wonder_effect_rules.cpp](../src/building_wonder_effect_rules.cpp)
rejects Culture replacement registration. The hooks cover Library Gold
and the separate Religion grant. A stateless Culture preview does not remove
native effects, establish a credit cadence, or certify all claim consumers.

The static closure conditions are:

| Static boundary | Required closure |
|---|---|
| `stonehenge-predecessor-input-boundary.json`, `predecessorArguments` and `aiInputs` | Local argument paths are bounded at all 64 inventoried direct calls; two select index three. Selected AI input arithmetic is bounded, but its route-specific tables and complete valuation remain open. Resolve those limits and code that uses computed or aliased claim slots before claiming complete replacement coverage. Suppressing only the Temple contribution is incomplete. |
| `city-yield-scheduling-producer-boundary.json`, `selectedStores`, `calendarTableJoin`, and `conditionalMethodJoin` | Selected stores and a static base for a table of halfwords are bounded. One accepted publication path makes the indirect method a field accessor. The global meanings, full set of writers, table extent, valid indices, ownership of returned objects, and enclosing paths remain unresolved. Reuse the gates in `city-yield-turn-scheduling-inputs.json`. Three conditional calls in mode zero and cached paths do not prove that credit occurs once per city. |
| `city-yield-state-credit.json`, `laterOverrides`, `stateCredits`, and `turnPath` | The [Library Gold integration](#library-gold-integration) closes the Gold transaction. Stonehenge still needs a coherent final Culture transaction covering the city `+0x48` cache, retained locals, earlier aggregates, border inputs, and an established Culture credit destination. |
| `wonder-claim-slot-state.json`, `claimWrite` and `guards` | Distinguish the path that completes construction after sufficient production from reset, restored claims, other writers, ownership transfer, and repeated helper calls. No completion mutation or save/load acceptance has been observed. |
| `wonder-claim-table-consumer-boundary.json`, `selectedWriters` and `selectedConsumers` | Selected serializer, city-match, and Calendar owner stores can change claims outside construction. Resolve how inputs are retained, restored values are validated, invalid cities are handled, and lifecycle events are timed. Preserve ordinary claim membership and cost inputs; the 46 syntactic base pairs do not establish complete computed or alias coverage. |
| `starting-technology-grants.json`, `ordinaryOwnership`, and `oxford-technology-acquisition-branch.json`, `staticBranch.acquisition` | Use ordinary acquisition for Religion 19 with correct owner, context, reason, phase, and presentation. The Oxford parameter shape does not prove a Stonehenge acquisition contract. |

The static `city-yield-credit-call-boundary.json` inventory,
`selectedCalls`, bounds local mode writes at all 24 inventoried direct sites:
eight mode one, thirteen mode -1, and three conditional Calendar mode zero.
These are site counts. They do not exclude indirect calls or establish runtime
invocation counts or whether credit occurs once per city. An update entry or a claim
owner change cannot serve as an unconditional custom credit or completion event.

The title handles integration on the guest main thread, deterministic conflict
handling, checked writes to caches, aggregates, and reserves, completion
detection, technology acquisition, presentation, and runtime acceptance. The
Culture replacement
cannot become active from its preview contract. Runtime acceptance requires each package
alone and both together, fresh completion, Religion already owned, loaded
existing structures, Chinese and non-Chinese owners, native multipliers,
ownership changes, save/load, and restart removal. Displayed yields and every
claimed authoritative consumer must agree. AI, scenario, and multiplayer
coverage require their own observed cases.

## V2 adjustable amounts

The independently versioned V2 entry points are:

| Building | Wonder |
|---|---|
| `BuildingEffectRulesV2AbiVersion` | `WonderEffectRulesV2AbiVersion` |
| `GetBuildingEffectSupportV2` | `GetWonderEffectSupportV2` |
| `RegisterBuildingEffectRuleV2` | `RegisterWonderEffectRuleV2` |
| `PreviewBuildingEffectV2` | `PreviewWonderEffectV2` |

Both V2 version functions return 2. V1 layouts, functions, version answers,
validation, and fixed additions remain unchanged. V2 uses the same effect IDs.
`BuildingEffectRuleV2` and `WonderEffectRuleV2` are 160 bytes: `structSize`,
two 64-byte ID arrays, `effect`, signed 32-bit `amount`, and five zeroed
reserved fields. ID and size validation follow the calling convention above.

The 32-byte V2 support records contain `structSize`, `runtimeAvailable`,
signed `minAmount` and `maxAmount`, and four zeroed reserved fields. Both
report `runtimeAvailable = 1`. Building support reports the full signed 32-bit
range, `INT32_MIN` through `INT32_MAX`, and V2 Building registration follows
[Building registration](#building-registration). Wonder support reports
`minAmount = maxAmount = 0`, the only amount
`WONDER_EFFECT_STONEHENGE_RELIGION` accepts, and V2 Wonder registration follows
[Wonder registration](#wonder-registration). The Culture replacement still
validates and previews any signed 32-bit amount, but its valid V2
registrations return `ERR_UNAVAILABLE` and retain nothing. Protocol
availability alone does not enable effects.

V2 previews reuse the V1 query and result records, rules for output size, and
signed 16-bit yield range. The Library amount is a flat addition to final
native Gold for a Chinese-owned Library; native Science is preserved. The
Culture replacement adds the amount to Culture after the caller's hypothetical
removal of native Stonehenge effects and application of remaining native
modifiers. Amounts 1 and 5 respectively reproduce the complete V1 preview
outputs. The Religion grant requires amount 0 and reproduces its V1 preview.

Library and Culture replacement amounts may be positive, zero, or negative.
Zero Library Gold adds nothing. Zero or negative Culture replacement amounts
preserve replacement intent and the Religion grant predicate: genuine new
completion with Religion not already owned. Amounts do not undo acquired
Religion or previously credited yields.

Addition widens before checking the signed 16-bit result. Overflow returns
success with both `PREVIEW_MATCHED` and `PREVIEW_OVERFLOW`, preserves the
supplied yields, and reports neither Stonehenge replacement nor Religion
grant intent. It never wraps or clamps. Invalid queries still return
`ERR_INVALID_ARGUMENT`. All previews remain stateless and establish no
live credit cadence or coherent gameplay transaction.

## Library Gold integration

The public hook implementation is in
[building_effect_rules_hooks.cpp](../src/building_effect_rules_hooks.cpp).
Two guest hooks in city update `0x82D13978` apply the retained Building rules'
summed amount N. Both run on the guest main thread.

| Phase | Hook | Effect for a matching city |
|---|---|---|
| Cache | Before `0x82D1537C`, after native overrides and before the mode -1 exit | Mode -1 and mode 0 set the signed 16-bit Gold cache at city `+0x46` to its native value plus N. Mode 0 also adds N to the owner's Gold aggregate (`0x830EE388 + owner*0x28 + 0x0C`), which native code already accumulated earlier in the same call. Other modes are unchanged. |
| Credit | Before the reserve store `0x82D15610` | Adds N to the store operand, the old reserve plus native credit Gold, for the owner's reserve `0x830ED544 + owner*4`. The native credit input at stack `+0x7C` is unchanged, so N is credited once per executed credit. |

A city matches when its owner index is a valid player, the live owner byte at
city `+0x00` equals the owner used for the aggregate and reserve rows, that
player's civilization at `0x830ECD28 + owner*4` is Chinese (6), and the Library
flag (`0x20` of the word at city `+0x10`) is set. Each phase re-reads these
inputs. A city whose owner bytes at `+0x00` and `+0x01` both equal the
word for the excluded player `0x82F700B0` does not match: native code zeroes its credit
after the cache boundary, so neither phase adds N. Native Science, the
Library's own Science arithmetic, and the Science aggregate are never written.

The cache phase checks every write of its transaction before mutating: the
16-bit cache, the 32-bit aggregate in mode 0, and the reserve sum the credit
will need. Any overflow or unreadable input leaves that call native. In mode 0
it records the amount for the credit phase of the same call, keyed by the
guest stack frame. The credit phase applies only that recorded amount, and only
after rechecking the 32-bit sum, so a later registry change cannot split one
call's display and credit. If the reserve changes between the phases so that
the recheck fails, that call's credit stays native while its cache and
aggregate keep N. Neither phase wraps or clamps.

N is added for each credit executed in mode zero, not once per city per turn.
The turn loop can make more than one call in mode zero for the same city, and a
cached turn path can skip the credit. Those native cadence rules apply to the bonus as
they apply to native Gold. Ownership changes take effect at the next call. The
city Gold total is recomputed through mode -1 when read and therefore includes N.

## Stonehenge Religion integration

The public hook implementation is in
[wonder_effect_rules_hooks.cpp](../src/wonder_effect_rules_hooks.cpp).
One guest hook in city update `0x82D13978` applies any retained
`WONDER_EFFECT_STONEHENGE_RELIGION` rule. It runs on the guest thread that
executes city update.

The hook runs before `0x82D16344`, right after the guarded call at
`0x82D16340` to the Wonder claim helper `0x82D16BA0` returns. Native code
reaches that call only after sufficient production, an unclaimed slot, and
eligibility checks, so it witnesses a new production claim. A claim from any
origin that reaches this edge qualifies; complete human, AI, hurry, scenario,
and network ingress coverage is unverified. At the hook, `r27` holds the normalized
Wonder index and `r23` holds the city index.

The hook grants Religion only when all of these hold:

- A Religion grant rule is retained.
- The Wonder index is 3, Stonehenge.
- The city index is in [0, 128).
- The Stonehenge claim city word `0x830E5164` equals that city.
- The claim owner word `0x830E5160` equals the signed live owner byte at city
  `0x8314FE28 + city*0xBC`.
- The owner is nonnegative, below the signed player count `0x82F700AC`, below
  32, and not the excluded player `0x82F700B0`.
- Every guarded guest read succeeds.

Otherwise the call stays native. When the gates hold, the hook first proves
that its registers belong to the current guest thread's context. It then runs
guest code through the SDK dispatcher's trap entry, which saves and restores
the interrupted register state. The native availability query
`0x82CEF6A8(19, owner)` runs first. Only a result of exactly 0 continues to the
ordinary acquisition owner `0x82D09208(owner, 19, city, 5, 1)`: Religion 19,
the completing city, the generic Wonder reward source 5, and state fan-out
phase 1. A nested hook hit during these calls does nothing, and no host
registry lock is held across them.

Native Stonehenge effects, Culture, claim membership, construction costs, and
completion presentation are unchanged. The grant adds no separate Religion
message. Loading or restoring a save, transfer, reset, owner synchronization,
and registering a rule while Stonehenge already exists do not take this edge,
so no grant is retroactive. Acquired Religion is ordinary saved technology
state. Removing the rule later does not revoke it.

Open limits:

- Excluded owner. Whether the excluded player can reach this edge is unproved.
  That owner stays native.
- Malformed or externally rewritten claims. If a restored slot reads -1 while
  a city has enough production invested in Stonehenge, the Wonder can complete
  natively again, and the hook treats that as a new claim. The availability
  query still prevents a second acquisition. No recovery policy exists for
  corrupted claim state.
- Gameplay coverage. A human player's ordinary new completion, with Religion
  absent, acquired Religion once while native Culture and completion
  presentation stayed unchanged. Completion with Religion already owned,
  capture or transfer, save/load after the grant, AI completion, and AI
  strategy around the acquisition are unverified.
- Network. Multiplayer replication of the acquisition is unverified.
