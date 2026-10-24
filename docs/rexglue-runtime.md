# ReXGlue runtime

This reference describes the title's host/guest contracts and developer
instrumentation. Start with [Getting started](getting-started.md) to play,
[Configuration](configuration.md) for settings and user files, or
[Development](development.md) to build and check the title.

ReRevved uses ReXGlue as its runtime and GPU foundation. The root CMake project
consumes the installed package from the sibling `rerevved-sdk` checkout. The
driver never modifies that checkout.

## Find a contract

- [Baseline](#maintained-baseline), [content](#game-content-and-writable-data)
  and [storage profiles](#storage-profiles): accepted inputs and state ownership.
- [Gameplay resources](#gameplay-resources), [save sidecar](#save-sidecar),
  [Raging Barbarians](#gameplay-raging-barbarians),
  [One City Challenge](#gameplay-one-city-challenge),
  [gameplay fixes](#gameplay-fixes) and
  [Game of the Week](#gameplay-game-of-the-week): gameplay integration limits.
- [System Link transports](#system-link-transports): online settings and
  transport behavior; [Online play](online-play.md) covers hosting and joining.
- [Runtime boundary](#runtime-boundary) and
  [presentation](#presentation-contract): renderer ownership and image delivery.
- [Ultrawide display aspect](#ultrawide-display-aspect): the default-off
  camera, interface and per-swap aspect hooks and their probe.
- [Scene capture](#native-scene-resource-capture),
  [shader harvest](#shader-harvest) and [frame reference](#xenos-frame-reference):
  recording formats and diagnostic controls of the developer build.
- [Native guest boot](#native-guest-boot),
  [completion ledger](#completion-ledger) and [native frames](#native-frames):
  guest progress, work admission and completion.
- [Native gameplay frames](#native-gameplay-frames): frame preparation and
  offline replay of recorded input sequences.

These contracts distinguish source checks, bounded runtime observations and
unverified paths. A diagnostic result does not establish coverage of the full game.
See the [evidence guidance](ai_agents/evidence-and-claims.md).

## Maintained baseline

The maintained fork provides this title's runtime compatibility support.
[`rexglue-sdk.lock.json`](../rexglue-sdk.lock.json) pins its repository, commit,
installed package version, and tracked dirty state. The root CMake project
checks the compatible API floor and installed target platform; the renderer
selection controls the `xenos` GPU plugin.
Update the lock instead of copying pin values into documentation.

The guest baseline is the retail `default.xex` at version 0.0.0.2 plus the
matching `game/default.xexp` title update. ReXGlue applies that patch during
code generation and launch, producing the maintained executable version
0.0.3.2 (game version 1.3, title ID `545407E5`, media ID `7DC1293B`). The
matching version 1.3 files under `game/Resource/Common/` are also required.
All of these retail inputs remain ignored and are not distributed by this
repository.

The installed SDK supports title configuration, guest code generation and
Release builds. The Windows deployment contains both `rexruntime.dll` and
`rexgpu-xenos.dll`.

## Game content and writable data

See [Getting started](getting-started.md) for content selection and
[Configuration](configuration.md) for locations of user files and backups. The
selected root is stored as `game_data_root` in the active profile's
`rerevved.toml`.

The selector checks the expected executable and title-update hashes.
`App::OnFinalizePaths` then calls `VerifyContentRoot` with
`ContentDepth::Quick`: it checks executable sizes and the 88-file Resource
manifest by size, not their hashes. Resource validation is size-based even in
`ContentDepth::Deep`; that mode adds executable hash checks. The manifest is
[`content_manifest.inc`](../src/content_manifest.inc).

The development driver bypasses selection with
`--game_data_root=<repo>/game`. Other launches can use
`--game_data_root=<path>` for the current session. The driver redirects user
data, caches and logs to ignored `out/` paths instead of packaged player state.
See [`game_content.cpp`](../src/game_content.cpp) and
[`App::OnFinalizePaths`](../src/app.cpp) for validation behavior.

## Storage profiles

ReXGlue calls the shared root for user data `B` and the active profile root `P`.
A bare launch, an omitted profile, or `--profile=default` selects the default
environment with `P=B`. On Windows,
`B` is normally `Documents\My Games\ReRevved`; on Linux, it is normally
`$XDG_DATA_HOME/rerevved` or `~/.local/share/rerevved` when
`XDG_DATA_HOME` is not set.

Use `--profile=<id>` to select a named profile:

```text
rerevved.exe --profile=alpha
```

The active root for this launch is `P=B/profiles/<id>`. Profile IDs are
lowercase names safe for use in paths: 1 to 32 characters, beginning with a lowercase
letter or digit, followed by lowercase letters, digits, `_`, or `-`.
`default` and Windows device names are reserved. The profile is selected at
startup, applies only to that process, and is not saved in `rerevved.toml`. Stop
and restart ReRevved to
switch profiles.

The active profile owns its writable state. In particular, config is
`P/rerevved.toml`, default logs are under `P/logs`, the default cache is
`P/cache`, save content is under `P`'s content roots for each user and title,
achievements are under `P/achievements`, and the mod and asset
override orders are `P/mod_order.txt` and `P/asset_order.txt`. This keeps
config, default logs, default cache, saves, achievements, and package
selection isolated between named profiles.
Explicit `log_file` and `cache_root` overrides retain their existing meanings
and may select locations outside `P`.

F2 discovers packages installed under `mods/` beside the executable and stages
enabling, disabling, and changing the load order for the active profile. Release archives
include the first-party mods there; with no profile `mod_order.txt` they start
disabled. Save changes writes the profile loadout atomically; mod changes take
effect after restarting the game. Missing or invalid enabled packages block the
complete desired mod set for that launch while leaving the game and F2 repair flow available.

Marketplace content is shared. Marketplace packages and their headers remain
in the base root's namespace for XUID zero, such as
`B/0000000000000000/<title-id>/00000002` and
`B/0000000000000000/<title-id>/Headers/00000002`; every profile sees that
content. Other title content and headers are stored separately for each profile under `P`.

To create a new named profile from the default environment, pass both options
on the first launch of an otherwise absent profile:

```text
rerevved.exe --profile=alpha --profile_copy_from_default=true
```

`--profile_copy_from_default=true` requests a single copy for this launch only. It
requires a named profile and an absent target. The SDK copies config,
ordinary title content and headers, title profile data, the title's
achievement record, and both package order files when present. An empty
saved order stays empty. It does not
copy marketplace content, logs, cache, unrelated files, or another profile.
After the copy succeeds, launch with `--profile=alpha` alone. An existing
profile is never overwritten by this operation.

To roll back or switch environments, omit `--profile` or select
`--profile=default` for the default `P=B` environment, or select a prior
named profile with `--profile=<id>`. Each profile's state remains in its own
root while it is not selected.

## Overlay controls

[Configuration](configuration.md) lists the player tabs, overlay hotkeys and
input settings. The title declares their groups, labels and audience in
[`overlay_settings.cpp`](../src/overlay_settings.cpp). Settings outside that
curated layout remain available through the config file, command line or
console, subject to their persistence policy.

The overlay bindings are `bind_settings`, `bind_mod_manager`,
`bind_achievements` and `bind_fps_counter`. A binding saved to config keeps
its saved key. Overlays accept mouse and keyboard input, not controller
navigation. The title's input layer keeps overlay input separate from guest
controller input.

## Display settings

[Configuration](configuration.md) owns the player display and audio controls,
including their restart requirements. Fullscreen applies while the game runs;
Monitor, Window Size, Render Resolution, Upscaling and Anti-Aliasing require
a restart.

Civilization Revolution renders a fixed 1280x720 guest image. The
`resolution_scale` setting scales that image. ReRevved defaults it to 0
(auto), which chooses the scale once at startup from the window or fullscreen
monitor size and logs a `Display:` line. With a fixed scale, `window_size` controls the host
window without changing render resolution. Explicit `window_width` and
`window_height` values in the config file or command line override Window Size.
ReRevved 0.5 renders with Xenos.

With `settings_show_developer` on, F1 adds a Developer tab with Renderer
and the relay address. The `renderer` cvar is for developers and defaults to
`xenos`; `native` selects the title's renderer for development. It
takes effect after restart.
Settings outside the curated layout can be set in the config file, on the
command line or in the console, subject to their persistence policy.

## Ultrawide display aspect

`display_aspect` accepts `original` and `auto`, shown in F1 as **Aspect Ratio**
choices **16:9** and **Ultrawide**. It defaults to `original`.
`display_aspect_custom` is config-only and empty by default. It holds a
`numerator:denominator` that `auto` uses instead of the window shape.
`display_aspect_probe` defaults to `false`. Starting with `original` and the
probe off leaves guest memory and the SDK presentation API untouched. After
widening, hooks remain active while cached projections need restoration.
The guest buffer stays 1280x720. With Xenos selected, the feature widens eligible
camera frustums, compresses the Scaleform (GFx) interface into a centered 16:9
region, and presents the frame at the target aspect. The native renderer is
unaffected.

Integer cross-products determine whether the requested aspect is wider than
16:9. With no custom value, `auto` uses the current presentation surface size;
a minimized or unknown surface selects original. `display_aspect_custom`
accepts two unsigned decimal integers separated by one colon, with surrounding
ASCII whitespace trimmed. Zero, overflow and invalid text select original.
An aspect of 16:9 or narrower also selects original.
With `k` as 16:9 divided by the target, 21:9 gives
16/21, 3440:1440 gives 32/43 and 32:9 gives 1/2.
`present_letterbox` on with both safe areas at 100 is the supported fit; the
title sets those defaults when the setting is not `original` and keeps 16:9
frames while explicit values conflict.

### Frame policy

The first of the outer `sub_82C71A00` entry, a GFx begin, a movie scope or the
guest swap freezes the swap interval's decision. A GFx or movie hook that runs
first freezes the interval as original (`early_ui`, `movie`); a later camera hook
does not upgrade it. The decision is wide only when Xenos is selected, the
request is valid, the presentation transport and fit are available, the state key
`[[[0x82FFD624]+0x70]+4]` is 2 (5 is attract), and the incoming camera has an
accepted vtable, frame caller and symmetric 16:9 perspective frustum. Movie
state is inferred as inactive when no movie hook ran in the current or previous
interval. An unreadable or non-gameplay state key leaves movie state unknown.
A movie observed after a wide latch records `late_movie`, keeps the frozen
decision and inhibits wide frames until `display_aspect` or
`display_aspect_custom` changes. `ObserveRendererSwapSource` forwards to
`display_aspect::OnGuestSwap`, which publishes the decision through the SDK
immediately before the `VdSwap` call and starts the next interval.

### Hooks

| Hook | Contract |
| --- | --- |
| `sub_82C71A00` | Frame owner `r3`, incoming camera `r4`. Latches the interval at the outermost entry. |
| `sub_823528D8`, `sub_823AF9C8` | Renderer-camera and owner-camera joins. The renderer-camera join clears the cached camera match (`+0xB0`) when the required scale differs from the last build. Untracked wide builds also require cache clearing during Original joins. |
| `sub_8250BA00` | Renderer `r3`, frustum `r8`, viewport `r9`. For an eligible camera, writes the corrected left and right words, calls Original once and restores the saved words. |
| `sub_82C71880` | Stage range `r4..r5`; labels the census only. |
| `sub_82C5EC28` | Movie scope; GFx correction is suspended inside it. |
| `sub_823035D0` | GFx display end. Pops the scope, sets the original viewport back when it was compressed, and reapplies the stored scissor with the correction off when this scope changed it. |
| `0x82302F60` | `BeginDisplayAspectGfxScope(r3, lr)` opens the GFx scope; the route hooks own its raw hook. Correction requires the accepted interface caller inside the accepted world frame, at the outermost scope and outside a movie. The probe observes every scope, subject to its output limits. |
| `0x823032B4`, `0x823032B8` | `CompressDisplayAspectGfxViewport(r1, r3, r31)` compresses the viewport record (`r1+144`: x, y, width, height) of the begin function's `sub_826903A0` call about guest `x = 640` for an admitted scope, rounded outward; `RestoreDisplayAspectGfxViewportRecord(r1)` puts the original words back, since the function reads width and height again. Scope end sets the original viewport back through `sub_82690138`. |
| `0x82303920` | `CorrectDisplayAspectGfxAffine(r1, r10)` rewrites the composed matrix copy on the stack; an admitted scope whose viewport was compressed takes no affine correction. |
| `0x8268FA7C` | `CorrectDisplayAspectGfxScissor(r1, r3..r7)` corrects the horizontal scissor after the viewport intersection; on a compressed viewport only the stored request is compressed and then clipped to that viewport. |
| `8234EB00`, `8234CF18`, `8234DF90`, `8234D360`, `82C5DF60`, `8234D890`, `82C49D08` | Probe-only observation; never mutated. |

For a wide frame, `r8 - 0xF0` identifies the camera to correct. Its vtable must
be accepted (`0x8200AA28`, `0x82123738` or `0x8216A3F8`), and the calling thread
must be inside the accepted world frame. The six frustum floats and viewport
must be readable and finite, with a symmetric 16:9 perspective frustum and
orthographic byte `+0x108` zero. Eligibility does not require the incoming
camera, renderer `[0x82F8E270]` or a join scope; the join scope controls baseline
observation. With `l`, `r` as the original words and `m`, `h` as their center
and half-span, the call sees `m - h/k` and `m + h/k`. Top, bottom, near, far and
the orthographic byte are unchanged. A nesting guard prevents repeated
correction. After the call, saved words are restored only if the current words
still match the correction; a mismatch is counted. CPU projection, picking and
culling consumers keep the original frustum.

For a viewport at guest pixel `v` of width `w`, the GFx row becomes
`a' = k*a`, `b' = k*b`, `tx' = k*tx + 2*(1-k)*(640-v-w/2)/w`. The scissor becomes
`floor(640+k*(left-640))` and `ceil(640+k*(right-640))`, clamped to the output;
an empty rectangle is unchanged.

### SDK presentation payload

`rex::system::IGraphicsSystem` carries the aspect with the swap rather than as
a latest value. `SetPresentationAspect(x, y)` (0, 0 clears) is taken by the next
`VdSwap`, which appends an `ASPX` trailer with the aspect and a sequence that
counts accepted calls from 1 to the swap command. The GPU thread applies the
aspect only to new source content, a swap that repeats the previous content keeps
its aspect, and a swap observer reports sequence, content identity, displayed
aspect and refresh result. The title compares its own sequence with the report
and counts a mismatch as `transport=mismatch`. The API uses GPU plugin
ABI 5 and requires matching runtime, Xenos plugin, installed headers and title
builds.

### Probe

`display_aspect_probe` observes without enabling correction; with the feature off
it logs the hypothetical corrected frustum with `applied=0`, using the custom
aspect when it is wide and 21:9 otherwise. Output is bounded: 64 camera families,
detail for the first 120 swap intervals and 120 after a policy change or
anomaly within 2048 frame lines (one in 60 otherwise), at most 4 camera lines and
64 lines per second, with dropped records counted and never waited on. Camera
counts are per interval; a missing line is not a zero-hit frame.

```text
DisplayAspectProbe: start version=1 title=<sha> sdk=<sha> mode=<mode> requested=<x>:<y> family_cap=64 frame_budget=2048
DisplayAspectCamera: frame=<u64> family=<u32> hook=<hex> cam=<hex> vtable=<hex> owner=<hex> owner_vtable=<hex> return=<hex> frame_return=<hex> stage=<first>:<last>|none ortho=<0|1> viewport=[<x>,<y>,<w>,<h>] original=[<l>,<r>,<t>,<b>] corrected=[<l>,<r>,<t>,<b>] applied=<0|1> restored=<0|1|na> frames_seen=<u64> hits=<u64> reason=<token>
DisplayAspectFrame: frame=<u64> policy=<wide|original> reason=<token> key=<u32|unknown> movie=<active|inactive|unknown> movie_owner=<hex> movie_handle=<hex> aspect=<x>:<y> k=<float> cam=<hex> camera_hits=<u32> camera_applied=<u32> gfx_hits=<u32> gfx_applied=<u32> scissor_hits=<u32> scissor_applied=<u32> movie_hits=<u32> early_gfx=<u32> late_movie=<u32> sequence=<u64>
DisplayAspectSwap: frame=<u64> sequence=<u64> source=<hex> content=<u64|unknown> aspect=<x>:<y> repeated=<0|1|unknown> transport=<ok|unsupported|mismatch> refresh=<ok|failed|skipped>
DisplayAspectProbe: summary frames=<u64> families=<u32> camera_hits=<u64> gfx_hits=<u64> refused=<u64> late_movie=<u64> restore_mismatch=<u64> transport_mismatch=<u64> dropped_records=<u64> family_overflow=<u64>
```

Addresses are uppercase eight-digit hex and floats carry nine significant
digits. The camera `viewport` shows the four words at `camera+0x114` as read.
The `reason` tokens are `disabled`, `native_route`, `invalid_aspect`,
`narrow_aspect`, `surface_unknown`, `state_unknown`, `frontend`, `attract`,
`movie`, `movie_unknown`, `unclassified`, `early_ui`, `camera_unknown`,
`camera_ortho`, `camera_unreadable`, `viewport_invalid`,
`offscreen_unconfirmed`, `fit_conflict`, `transport_unavailable`, `world`,
`late_movie` and `late_scene`. A baseline change without a guest setter counts
as `restore_mismatch`.

### Acceptance boundary

Source and unit tests cover the arithmetic and policy order. The frame policy
above enables widening using accepted camera identities and movie-hook history.
Runtime validation still needs to cover movie and loading transitions, culling
in the extra width, camera-family coverage, GFx scope selection and partial
viewport clipping, content-generation bumps for resolve-written and scaled
frontbuffers, resize and fullscreen surface-size updates, Direct3D 12 and
Vulkan parity, and label, cursor and picking alignment.

## Combat settings

The `combat_speed` setting accepts `normal` or `fast` and defaults to `normal`.
`fast` uses the guest's native faster combat presentation pace while preserving
resolution and completion.

## Gameplay Resources

F1 **Gameplay > Resources** offers **Normal**, **Balanced** and **Legendary**.
Normal is the default and performs no additional guest reads, writes or calls. The integer
config key `resources` stores `0` for Normal, `1` for Balanced and `3` for
Legendary. Value `2` reserves Abundant and performs no placement. Normal and
Abundant publish an empty identity snapshot, preserving the native resource reader.
GOTW Standard Rules freezes Resources as Normal regardless of this selection.

The Resources selection freezes on entry to initialization `0x82D25170`, before
its setup call at `0x82D25254`. The successful recount at return address
`0x82D25310` consumes that selection once, after AI civilization removal.
Failed setup discards the selection. Changing the setting during play affects
the next new game. Starting a new game clears the identity snapshot. The
existing load entry point at `0x82D86674` also clears it before
deserialization.

Balanced reads the final player coordinates at `0x830E345C+4*p` and
`0x830E343C+4*p`. Each anchor must match an owned live type-0 Settler.
The bridge accepts the ordinary layout of six slots, excluding barbarian slot 5,
and supported 32-by-32 bounds at `0x82F77364` and `0x82F77448`. Backing planes
use stride 40. The [start-assignment implementation](../src/start_resources_runtime.cpp)
records the arrays and unit evidence.

The placement area is the complete ring of eight tiles from the default city scan,
excluding its center. The first coordinate wraps; the second clips. An
incomplete ring skips the start. No fixed terrain counts are required.
The [coordinate-scan implementation](../src/start_resources.cpp)
records the offsets and topology. Balanced assumes disjoint player start rings.

Balanced never writes terrain. For each resource it chooses one eligible tile
of its existing terrain, preferring a native remainder match:

| Resource | Ordinal | Existing terrain | Native remainder | Override sum |
|---|---:|---|---|---:|
| Oxen | 0 | Grassland, byte 1 | `(a+b)%4 == 0` | 0 |
| Oak | 9 | Forest, byte 3 | `(a+b)%3 == 1` | 1 |
| Fish | 18 | Sea, byte 0 | `(a+b)%3 == 2` | 2 |

Presence `0x20` or forbidden flag mask `0x1001` excludes a tile, including an
existing resource of the requested type. A missing eligible terrain skips only
that resource and logs its ordinal. A host shuffle from setup seed
`[U32[0x8314EFC8]+0x138]` makes choices deterministic without advancing guest RNG.
The [resource definitions](../src/start_resources.cpp)
owns native terrain/remainder identities. Oxen `0` is valid; only `-1` is absent.

Legendary uses the same anchors and ring, without writing terrain. Each eligible
ring tile gets one resource from its own terrain's pool:

| Existing terrain | Permitted resources and ordinals |
|---|---|
| Sea, byte 0 | Fish 18 |
| Grassland, byte 1 | Random Oxen 0, Cattle 19 or Wheat 20 |
| Plains, byte 2 | Random Wine 8 or Marble 13 |
| Forest, byte 3 | Oak 9 |
| Hill, byte 4 | Iron 1 |
| Desert, byte 5 | Spices 12 |
| Mountain, byte 6 | Random Gems 15 or Gold 21 |

Duplicates are allowed, including Fish on every extra water tile. Existing
resources keep their native identity. Forbidden flags `0x1001` and unsupported
terrain skip only their tile. No terrain mix is enforced. Choices use the setup
seed mixed with the player index, without advancing guest RNG. Players decide
in index order; overlapping rings share the first deciding player's choice and
write each physical tile once. Native remainder matches need no override record.

For a selected tile on another remainder, an immutable per-game snapshot holds
sorted unique `(tileIndex U16, ordinal U8)` records. Publication precedes presence
writes and every refresh through receiver `0x8314F76C`, whose current vtable and
`+0xF4` slot must match `0x82D7D950`. A pre-instruction hook at `0x82CF15C4`
changes only full `r8` to the synthetic sum when the indexed tile still has the
required terrain. Matching uses the low word of `r11`, retaining compatibility
with callers that leave stale high words. The synthetic sum is the ordinal's
position in that terrain's native identity row. Other tiles and terrain
mismatches preserve every register.
Native presence, `0x1001`, technology, UEA39 and access gates for civilization 10
continue to run. The [override implementation](../src/resource_identity_overrides.cpp)
records the hook and the route for the new object's type/name. Allocation, model
fulfillment,
registration and retained visual objects still require runtime acceptance.

Balanced and Legendary suppress India's separate starting resource. Before `0x82D23324`,
its frozen policy branches to `0x82D23330`, skipping only the presence store and
refresh at `0x82D2332C`. The earlier RNG draw and coordinate calculation remain.
Normal and reserved values retain the native arm. The [India
implementation](../src/start_resources_runtime.cpp)
records that control flow. Raging Barbarians and OCC retain their existing freezes.

Final readback checks every tile's terrain and exact flags after all refreshes,
and verifies each selected identity against the published snapshot. A failure
clears overrides, restores original flags, refreshes changed flags and verifies
again. Failed rollback receives a final raw flag restoration and readback, then
an error report. Unexpected terrain changes are reported without a terrain
store. The refresh return register is not a success result. Resource objects
and caches remain uncertain after failed placement or rollback.

`resource_settings_diagnostics=true` logs rejected starts as well as the normal
choice records: anchor, ring terrain mix, chosen coordinates, override use and
final identities. It defaults to `false`; skipped resources always log.
Legendary records include player, tile, terrain, ordinal, override use, skip
reason and final identity.

`resource_identity_trace=true` enables a read-only identity and yield observer, default
off. At `0x82CF15C4`, it records calls with a nonempty override snapshot only
when the physical index exactly matches a record. Each decision includes the
reader's caller LR, input coordinates/player, full r11, r8 before/after, r7 terrain base,
terrain byte and flag indicating a successful read, snapshot address/count and
override decision.
A terrain mismatch is reported separately from an applied override.

A wrapper around reader `0x82CF1558` records returned ordinals for matching
indices, including early native absence returns. Food caller LR is `0x82CF1F10`;
production caller LR is `0x82CF1CB0`. A return's `decision_seq` links a newly
logged decision from the same call; zero means that decision was suppressed or
the call did not reach the override site. Returns also include the decision,
or `native return` for early exits. After a clear, the last nonempty tile
list remains a trace filter: `retained_watch=true` with `count=0` identifies
native reads while the snapshot is empty. Tiles chosen only by a future
publication cannot be identified before that first publication. For each
caller LR, tile index and player key, decisions and returns are independently
logged only when r8 after or the returned ordinal, respectively, or the decision
changes. A bounded map retains up to 4096 keys and resets on every publish or
clear, so the first decision and return for each key are logged again.

Wrappers around food `0x82CF1CE8`, production `0x82CF1AF0` and trade
`0x82CF17C8` log caller LR, raw coordinates, city input r5, player input r6
and returned yield. They include exact override indices, indices normalized
with the live map bounds, and all out-of-bounds inputs. Raw and wrapped indices
remain separate; observation never normalizes the guest registers. Food steps
record native/effective terrain food, city terrain bonuses, terrain ability
arguments/results, the tile bitmask/count and bonus gates, identity/class,
`0x82CF1F30 -> 0x82CF0630` ordinal and contribution, and the final player
modifier before/after division. Each step includes its total and resolved player.
City `0x82D13978` owner calls retain the same per-tile inputs; when any worked
tile matches an override, `0x82D13F2C` logs the city's summed work-tile food,
production and trade before later city modifiers. Settlers `0x82D931B8` logs
coordinates and food; `0x82D931D0` logs the icon, incoming food and cached food
at icon `+0x74`, with a flag indicating a successful read. Yield results
are deduplicated by
owner, caller, raw coordinates, both context inputs and result; intermediate,
city and overlay samples also include their observed values in bounded keys.
All sample histories reset on publish/clear and use the retained tile watch.

Publish and clear records include previous/current tile-index:ordinal lists,
counts, source function/line and reasons such as setup publish, load reset clear
and sidecar restore. Snapshot addresses identify the retained objects in flight;
sequence numbers order log emission, and a concurrent publication can become
visible before its notification is logged. All sites share one process-wide
sequence capped at 4096 events, followed by one truncation marker. Publish and
clear events are never deduplicated and share that cap. Restart for
each comparison if the cap is reached. A truncated trace cannot establish
complete ordering. The observer performs no guest stores or extra guest calls.

Snapshot schema 1 encodes a version byte, big-endian U16 record count and packed
U16 index/U8 ordinal triples. With [Save sidecar](#save-sidecar) enabled, a matching
sidecar restores the frozen Resources value and Balanced or Legendary snapshot on load.
Legendary needs at most eight records per player, or forty across five players;
the U8 ordinal and U16 count fields accommodate them without a schema change.
Without one, loaded presence flags use native identities. Save/load runtime
acceptance, appearance of the native Oak Forest child scene, minimap and
strategic AI/monopoly coverage remain acceptance gates. The [Forest and persistence
implementation](../src/resource_identity_overrides.cpp)
qualify unchanged terrain versus Oak's native child effects.

Runtime validation must cover Oxen/Oak/Fish at each start or logs of missing kinds,
unchanged terrain bytes and visible coastlines, tooltip/model agreement, India's
missing extra grant, city resource use after its normal access gates, parity
with a repeated seed, a following Normal game and the sidecar load/fallback
checks. Oak appearance and
retention of new objects need visible checks. Multiplayer synchronization remains
outside this acceptance.

Legendary runtime checks cover every permitted pool entry, extra water tiles,
existing resource preservation, unchanged terrain and coastlines, choices with a
repeated seed, overlapping rings and per-tile skip diagnostics. Check tooltip,
model
and city yields for overridden identities after their normal access gates.
Save a Legendary game, change the live Resources setting, then reload and
compare the same resources and frozen mode. Check missing/mismatched sidecar
fallback, a following Normal game and GOTW Standard Rules with Legendary selected.

## Save sidecar

`save_sidecar` (default `true`) controls whether the host stores gameplay
state separately for each save. The payload stores schema version 1, the frozen
Resources selection,
sorted unique Balanced or Legendary identity overrides, the frozen Raging Barbarians boolean
and the fixed One City Challenge policy for whether it is enabled and its
minimum population. Founding
Militia and Game of the Week seed state are excluded. New games still freeze
the effective gameplay selections, including standard GOTW values.

The sidecar is the native slot's host filename plus `.rerevved-gameplay`, in
the same directory. The title resolves the user/device namespace when opening
the selected content, before calling the stream opener at `0x82E83B38`; the
host hook resolves
the resulting guest filename through the VFS to its `HostPathEntry`. It does not
construct a separate user or device directory. The payload binds the internal
record `+0x108` slot filename and selected device to the native file's SHA-256
digest. Reusing a slot with different native bytes rejects the old sidecar.

At `0x82D829E8`, a nonzero final native result in `r25` permits an exclusive
temporary sidecar write using a process ID and counter, and atomic replacement
beside the native file. Leftover temporary files do not block later saves. Native
save bytes remain unchanged. Every successful save through `0x82D82510` uses
the same rule, including autosaves on that route. Distinct autosave routes have
not been certified. The [save storage implementation](../src/save_sidecar_hooks.cpp)
records the boundaries for the selected slot and finalization.

Load reads and validates the sidecar before the deserializer call returning at
`0x82D844CC`. It stages the result for that load only. After the existing
`0x82D86674` reset clears overrides and freezes live selections, restoration
publishes the saved snapshot and frozen settings before native readers resume.
A repeated reset in the same load restores the staged values again. A successful
restore logs once; a staged state never restored warns when the load ends.
A valid sidecar wins over live settings. Missing, unreadable or rejected
sidecars log one reason and retain live selections with no overrides. A save
made before sidecar support loads under live settings; saving it with sidecars
enabled records those effective values for subsequent loads. Disabling
`save_sidecar` retains that existing load behavior and performs no sidecar I/O.

Runtime acceptance requires a Balanced or Legendary, Raging Barbarians and OCC game: save,
quit, flip all gameplay selections, and load. Check resource identities and both
behaviors against the saved values. A save made before sidecar support must
load using live settings with no identity overrides. Repeat with an autosave;
capture of host paths, digest readability after finalization and autosave coverage
remain runtime gates.

## Gameplay Raging Barbarians

F1 **Gameplay > Raging Barbarians** is a checkbox, off by default. The boolean
config key `raging_barbarians` uses the title's ordinary settings storage.
Its effective value freezes at initialization for a new game `0x82D25170` and at
the hook for loading saves `0x82D86674` in `sub_82D86400`. Changing the checkbox during
play affects the next new game or save load. Loading uses the saved frozen value
when a matching [Save sidecar](#save-sidecar) is enabled, otherwise the current
checkbox.

When enabled, the consumer hook at `0x82D153E4` replaces the local signed
fecundity input with native case `2`, after `extsh r10,r11` and both native
guards for city owners. When disabled, every input stays native. Global `BFECUNDITY`,
city eligibility, weighted presence, mode suppression, production cost,
completion and allocation failure handling stay in the native path. Case `2`
conditionally credits 3 production when weighted presence is below 3; a
separate mode can suppress that credit. Weighted presence is not a fresh
count of hut guards.

The same frozen selection independently supplies Beta Centauri's aggression
level `BAGRESSIVE=2` at `0x82CB3F44` in the lookup of city ordinals
`sub_82CB3F08`. The hook follows the native check for a barbarian owner and replaces
local `r11` before both signed consumers at `0x82CB3F44` and `0x82CB3F68`.
The zero/99 branch, repeated owner check, `R=3-a` calculation and return stay
native. Global `BAGRESSIVE` and other scenario fields are unchanged. This
separate control does not read fecundity. It supplies `R=1`, versus `R=2` for
native aggression level `1`, to the departure comparison at `0x82CC1308`
and the earlier threshold consumer at `0x82CBF978`.

At the departure comparison, weighted defenders `C<=R` clear the current
unit's selected target. `C>R` retains the path for the selected neighbor only when
unit byte `+4==0` and the other native holds are bypassed. Thus `C=2` passes
this local hold at level `2`, while level `1` requires `C>=3`. Targeting,
fallback selection and command delivery remain native. These instruction
boundaries do not establish a fixed spawn multiplier, departure rate, forced
attacks or complete Beta Centauri parity.

The diagnostic `--raging_barbarians_trace=true` is off by default. It logs
cumulative override hits, verified barbarian unit creations and native
allocation failures. It also logs the frozen selection, native/effective input, weighted
presence, mode, production credit, city coordinates and creator result. It
logs aggression overrides with city ordinal, native/effective `a` and `R`.
The read-only departure observer at `0x82CC1308` logs weighted defenders,
`R`, the unit weight byte and whether the local hold clears or retains the
selected target. The production observers run at `0x82D1544C` and
`0x82D15AB4`. A creation is counted only when
a valid returned slot has the requested owner/type and matches the city's
coordinates; slot `0` is valid and `-1` is failure. `production_at_return`
precedes the native cost subtraction. Counters cover events while tracing is
enabled in the current process. Actual session child/result identity, a
completed presence rebuild and an ordered join with readers of the same cell remain
conditional. Runtime spawn cadence, save/load acceptance and delivered or
executed departures and attacks require gameplay validation.

## Gameplay One City Challenge

F1 **Gameplay > One City Challenge** is a checkbox, off by default. The
`one_city_challenge` config key uses ordinary title settings storage. Initialization
for a new game at `0x82D25170` and the load entry point at `0x82D86674` fix
whether the policy is enabled and its minimum population. Changes during play apply at the
next new game or save load. Loading uses the saved frozen policy when a matching
[Save sidecar](#save-sidecar) is enabled, otherwise the current checkbox.

When enabled, Settlers type `0` requires population `33`. The final candidate
comparison at `0x82CEC7A8` uses `max(native T, 33)` and accepts `P >= T`.
The ordinary completion comparison at `0x82D15A7C` uses
`max(native K, 32)` and accepts `P > K`. Other types retain their native
thresholds. Existing units and production queues are not migrated. At
`0x82CB5544`, an empty AI candidate suffix or an ordinal outside its populated
range returns `-1` through the native epilogue at `0x82CB6E30`. The caller's
existing no-choice branch skips production assignment. The native RNG record
is `0x82F7AD00`.

The applying 100g milestone branch at `0x82CF5760` grants one population to
each owned city with population `1..30`, then calls the native city growth
helper `0x82D00C78`. Cities at the cap of `31` stay at the cap. The dispatcher
preserves the interrupted PPC context. The replacement resumes `0x82CF57E8`
once, retaining the native store of the threshold marker at `0x82CF5BE0` and later
reward tiers. Disabled OCC leaves every hook's native path and operands
unchanged.

The frozen enabled policy also replaces Mongolian Ancient's UEA `40`,
barbarian villages become cities, with Knowledge of Horseback Riding. This uses
the existing consumer of era abilities and its check for technology ownership.
The camp capture path directly compares civilization `14` at `0x82D1B758`
instead of querying UEA `40`. Its existing replacement gate uses the same
frozen policy to select the ordinary reward path at `0x82D1B8BC`, bypassing
city creation at `0x82D1B784`. No entry in the rules bundle or second technology
grant is added. Enabling the Mongol Horseback mod as well has the same result.
With OCC off, published mod replacements retain their existing behavior.

Civilization selection precedes the freeze. Its Mongolian Ancient ability
text reads **Start with Horseback Riding** while the live OCC checkbox is
checked. Otherwise it uses native text or text published by mods.

The diagnostic `--occ_trace=true` is off by default. It logs blocked Settlers
comparisons, the shared policy identity and `R`, AI no-choice exits, reward
applications, owned city indices, population before/after, cap skips, helper
entry/return mask samples and marker retention. An application completes only
after all owned cities have been visited. Helper calls must return while
retaining city owner/population, saved frames, and marker memory. Actual mask
production/order and session-child dispatch/lifetime remain runtime acceptance
conditions; entry/return mask samples alone do not prove intermediate reads.
Human and AI rejection, continuation of turns with no candidates, one reward per
milestone, cap behavior, later tiers, and checkbox changes across new games
and save loads require gameplay validation. Mongolian starting Horseback
Riding, ordinary camp rewards without city conversion, and selection screen
text also require gameplay validation with OCC and the mod individually and
together. Multiplayer is unverified.

## Gameplay Game of the Week

F1 **Gameplay** has a **Game of the Week** section with **Week** above
**Standard Rules**. Week combines a checkbox (`gotw_custom_week`, default off)
and a decimal field (`gotw_week`). Checking Week unlocks the field and replays
the chosen week's game using `expanded_MapList[week % 277]` as its seed.
Unchecked Week preserves native selection for the shared week.

Week accepts `1..2147483647`. Zero is a native sentinel and is rejected.
Blank and other invalid inputs are rejected without changing the previous
valid field value. Revalidation at launch falls back to native for an
invalid checked Week and logs the rejected value once for that launch.
The Week tooltip is "Enable to play a different week's game instead of this week's."

The unchecked Week field receives this week's original native r4 value through
the UI dispatcher and its alive token. Checking Week before delivery protects
the field, and another launch invalidates older queued updates. The info log
records the native week, effective week and effective seed.

**Standard Rules** is a plain checkbox (`gotw_standard_rules`, default on).
Its tooltip is "Enable to play Game of the Week without gameplay changes."
For a new GOTW game, it freezes Resources as Normal, Raging Barbarians and
One City Challenge off, including OCC's Mongol ability swap, and keeps the
native AI civilization count. Unchecking it uses the player's gameplay settings.
Other game modes retain their settings. Combat speed, display, audio, controls,
notifications and online options retain their configured behavior.

The decision reads the live setup mode through `0x8314EFC8`, at setup `+4`,
once on entry to `0x82D25170`. Only mode 2 enables the gate. The GOTW menu
stores mode 2 at `0x82E77030`, before calling `0x82E76E90` at `0x82E77040`;
the accepted seed latch is inside that function at `0x82E76ED4`.
The new-game call at `0x82E610A0` enters the wrapper before its native setup
call at `0x82D25254`. Resources, Raging Barbarians and OCC freeze there,
before India's resource arm at `0x82D23324`. AI removal runs at the later
recount call `0x82D2530C` and uses the same frozen decision. A later checkbox
edit cannot change that decision. The save sidecar captures the effective frozen
Resources, Raging Barbarians and OCC values, so restoring it preserves standard
values even when the live checkboxes differ. AI elimination is native save state.

Gameplay Lua mods remain active with Standard Rules selected. The SDK
executes Lua and publishes each decoded bundle before guest execution.
The title retains those rules for the process; Standard Rules does not
suspend or restore them per game.

The accepted GOTW start snapshots the selection immediately before
`0x82E76ED4 -> 0x82DADC88`, after the native session result equals 1.
The launch retains the setup pointer and native week alongside the effective
week, seed and custom-week flag. A custom Week replaces r4 with its
zero-extended value and supplies its derived seed in r5.
With Week unchecked, both registers remain untouched, including native r5=0.
The native writer publishes week `+0x134`, seed `+0x138` and civilization
`+0x13C`. Native RNG initialization using the week as its seed still executes. Changes in
F1 affect the next GOTW start.

At the later new-game setup call `0x82D91A94 -> 0x82DADC88`, which runs
after the first writer and before map generation, a launch with latched
custom Week reapplies the same r4/r5 policy only when the live setup
read through global `0x8314EFC8` has mode `+4 == 2`, matches the accepted
setup pointer, and the incoming command carries the effective week. The
command week need not equal the original native week. Unreadable or unrelated
setups and ordinary new games in mode 1 retain their arguments. The load writer
at `0x82D8685C` remains native.

`gotw_trace` defaults off. When enabled it logs every entry to the setup writer at
`0x82DADC88` and the point before cleanup `0x82DADD1C`, including LR, r3/r4/r5,
the global setup pointer and receiver fields +4/+0x134/+0x138/+0x13C.
It also records the result of loading by name at `0x82D0F0F4`, procedural entry at
`0x82D0F100`, G and its seed before `0x82D0F13C`, and r27 plus PRNG words
+0/+4/+8 and normalization bits +0x10 at `0x82D0F168`. Flags indicating successful reads
distinguish unavailable memory from zero. The observer never changes guest
state. One increasing sequence spans all sites and launches in the process;
the cap is 512 events, followed by a truncation marker. A truncated trace
cannot establish complete ordering.

Results for a custom week in mode 2 take the native branch for nonlocal results
at `0x82D7DC90` before the outcome is classified. It writes outcome -1 and sets
the completion
byte required by native session cleanup. Native guards for null results and
failures remain before this diversion. Custom results also skip the score/week
cache stores at `0x82DBF138` and `0x82DBF14C` and the common base/victory-child
submission helper at `0x82DBE69C`. Cache request completion continues.
The submission guard also covers older replay weeks, which can pass the native
check for unequal weeks at `0x82DBE6CC`. These guards cover the researched
sites; complete local outcome/profile mutation coverage remains unproved.
No custom save path or GOTW sidecar is supplied.

The MapList order follows `Pregame.FPK` member `ccglobaldefines.xml:228`.
The native one-based ordinal is `(week % 277) + 1`: week 1084 selects seed
276 and week 1085 selects 277. The effective week also seeds gameplay RNG
that terrain generation saves and restores before choosing starts. Replay equivalence
requires matching title/content, difficulty, player configuration and gameplay
overrides. Complete generation and equivalence of start tiles still require runtime
corroboration.

The public implementation is in [gotw_hooks.cpp](../src/gotw_hooks.cpp),
with settings in [gotw_settings.cpp](../src/gotw_settings.cpp) and hook
locations in [gotw_hooks.toml](../config/gotw_hooks.toml).

## Gameplay fixes

Two always-on midasm hooks in `config/gameplay_hooks.toml` repair native
behavior; `src/gameplay_fixes.h` holds their arithmetic. Static evidence is in
rerevved-research manifests `science-completion.json`,
`technology-prompt-callers.json` and `spaceship-launch-arrival-year-boundary.json`.

- **AI research choice.** The computer-player branch of the technology prompt
  `sub_82CECCD8` lists the eligible technologies ascending at ordinals
  `0..N-1`, stores -1 at ordinal `N` and retains `N+1`. Its scoring loop ran
  `N` iterations from ordinal 1, so it skipped the first eligible technology
  and scored the sentinel. The hook at `0x82CED34C` sets the loop base to
  ordinal 0; the trip count and the no-candidate branch are unchanged.
- **Technology victory in BC.** The launch dialog result in `sub_82D259E8` is
  a signed arrival year, or -1 when cancelled. The native check at `0x82D2A084`
  admitted only results above zero, which rejected BC and year-zero arrivals.
  The hook at `0x82D2A084` admits every result except -1 and leaves the
  payload and every other gate unchanged.

Each hook logs at INFO with the prefix `GameplayFix:` so a play session shows
that it ran: `research scan base=<hex> count=<N> first=<tech> alphabet=<0|1>`
(on a change of count or first technology; the first 64 lines, then
`research scan suppressed=<n>` every 256th suppressed change),
and `spaceship launch year=<signed> admitted=<0|1> old=<0|1>` (every call).

## Research prompt

When no research is set, the game opens the technology prompt at the start of
each turn and after the local player visits a village, whatever the village
gives. A technology reward when a city changes hands can open it too.
Dismissing it leaves research unset, so the prompt returns.
`hide_research_prompt` defaults to `false`. When `true`, each of those
automatic prompts is skipped as if it had been dismissed: research stays unset
until the player chooses it from the technology window, and no technology is
chosen automatically. Special Powers > Research a New Technology, which the
player opens, is unchanged.

## Advisor pop-ups

F1 **Notifications** also lists 15 `hide_popup_*` settings, each default `false`.
Each one skips one group of advisor notices, for example rival war and peace
news, combat reports or village outcomes. A skipped notice is not shown and
the game continues as if it had been closed. Three of them answer a question:
`hide_popup_save_reminder` does not save, `hide_popup_trophy_room` does not
open the Trophy Room, and `hide_popup_government_available` keeps the current
government. `src/advisor_popups.h` lists the call sites in each group. Other
advisor screens, including the Special Functions menu, are unchanged.

Two more default-`false` settings skip the Barbarian leader message.
`hide_barbarian_messages` skips the message shown on first contact with a
Barbarian tribe; the game treats that tribe as not yet introduced, so the
message can appear at a later contact once the setting is off.
`hide_village_barbarian_messages` skips the message shown after capturing a
Barbarian village, which names the reward; the reward is still granted.
`src/barbarian_messages.h` lists the village call sites.

## AI civilizations

`ai_civilizations` sets the most AI civilizations in a new game, from 0 to 4.
It defaults to 4, which leaves the game unchanged, and appears in F1
**Gameplay** below Combat Speed, labeled **AI Civilizations**. Below 4, right after the game
sets up the players, the starting units of the AI civilizations beyond the
limit are removed through the game's own unit removal; the lowest AI slots are
kept. At the end of the first round the game eliminates the removed
civilizations as it would any civilization with no city and no unit.
Barbarians are unchanged.

- A game with one human keeps at least one AI civilization, so 0 (**None**)
  applies only to multiplayer games with two or more humans. Domination then
  counts the remaining capitals as usual.
- The farewell leader screens for the removed civilizations are hidden, along
  with the leader screen the game shows just before a farewell. Farewells of
  civilizations eliminated in play are unchanged.
- It applies only when a new game starts. Loading a saved game does not
  remove anything; a game saved after the removal keeps it.
- In a multiplayer game the host's value applies to every player for that
  game; the other players' saved settings do not change. A joining player
  requests the host's settings when the session is created and receives them
  in the background. If they have not arrived when the game starts, the game
  waits up to 3 seconds. If they still have not arrived, that player keeps
  their own value, logs a warning and sees an on-screen notice, and the games
  may diverge if the values differ.
- A joining player whose enabled mods differ from the host's still joins. The
  difference is logged as a warning and shown on screen when the game starts;
  the host's mods are not applied. The comparison uses the mods each side
  activated at launch, so enabling or rescanning mods later in F2 does not
  change it. If the host has too many mods to send in full, the warning says
  the full list could not be checked.
- With no AI civilizations, a player who has not founded a city by turn 10
  lets another player who has win by Domination.
- Setups that start players with a city instead of a Settler are not
  supported: an AI civilization there keeps its city.

Each new game with a limit below 4 logs one `AI civilizations:` line with the
limit, its source (`local`, `host`, `received` or `fallback`), the removed
slots, the human player mask, the network game flag, the local player, the
number of removed units and a digest of the removed units, so two peers can
compare their logs. When the limit removes no civilization,
the line says `unchanged` and gives the limit, slot layout, human player mask,
network game flag and local player instead.

## Xenos return counter

`xenos_frame_counter` defaults to `false` and is read at startup. With
`--renderer=xenos --xenos_frame_counter=true`, the title logs **VdSwap returns**,
not decoded or presented frames. The counter does not change guest timing settings or
presentation intervals and requires no other diagnostic setting.

Each `xenos_frames` CSV record has these fields:

```text
xenos_frames,start_ns,end_ns,attempts,returns,total_returns,returns_per_s,dt_min_ms,dt_mean_ms,dt_max_ms,interval0,interval1,interval2,interval_other,partial
```

Timestamps are host `steady_clock` nanoseconds. One-second bins begin when the
counter starts, before guest execution, and include empty seconds during stalls.
Attempts count calls to the import; returns count its completion, including early
validation returns. `total_returns` is a separate 64-bit cumulative sequence.
Inter-return gaps belong to the bin containing the later return, including gaps
across bin boundaries. Gap statistics are zero when a bin has no completed gap.
The final partial bin uses its actual duration and is flushed on a normal close,
shutdown or guest exit; a bin ending exactly at exit emits no partial record,
and a crash emits nothing further. Records carry the normal log prefix.
`xenos_frames_summary` covers the whole session from counter start, including
boot, loading and empty bins, so its minimum is usually 0. Measure a test window
by selecting the per-second records whose timestamps fall inside it.

The interval columns count returned swaps by the latest decoded policy observed
for their device when the attempt began. Immediate `0x80000000` decodes to 0,
D3D intervals 0 and 1 decode to 1, D3D interval 2 decodes to 2, and D3D interval
4 decodes to 3. Any other raw value makes the guest load its decoded value from
a stack word, which is counted as observed. Decoded values above 2 and devices
without a matching observation count as `interval_other`.
The observation occurs before packing at guest `0x826A4258` in `sub_826A4150`
and preserves all registers. A rate near 30 with interval 2 is consistent with
an interval ceiling at 60 Hz; interval 1 alone does not establish a CPU bottleneck.

## System Link trace

The `system_link_trace` setting is read only at startup and defaults to
`false`. When enabled,
the runtime log records System Link datagrams by direction, kind, framing type,
size and remote port. Kinds are the GameSpy QR2 echo, prequery, query and other
requests, QR2 replies, GT2 messages by type, and any traffic to UDP 53, 27900
or 27901. The log also records when the System Link browser starts its LAN port
sweep, the QR2 host initialization arguments and result, browser callback
reasons, and the host's GT2 connection admission decisions. It never records
payload bytes. Each distinct datagram record logs on its first occurrence and
at each power-of-two count. The datagram table holds at most 256 distinct
records; further new records are only counted as overflow. Browse, QR2 init and
result lines log on every call. Browser callback reasons are throttled like
datagrams.

The trace is independent of the transport. It works with direct LAN, with relay
mode, or with neither enabled.

## System Link transports

System Link transport is provided by the SDK and is off by default. Direct LAN
and relay mode are mutually exclusive; a process cannot enable both. Relay mode
forwards guest datagrams through a UDP relay for private tests.

Press F1 and open **Online** to configure either mode without launch flags.
Every editable setting except Rich Presence requires a restart: select
**Save**, then restart ReRevved. Those settings are frozen at
startup, so edits made in a running session apply at the next launch.

| Group | Row | Setting | Meaning |
|---|---|---|---|
| (top) | Status | `system_link_status` | Read-only connection status for this session |
| (top) | Player Name | `system_link_profile_name` | Optional player name, up to 15 letters, digits, spaces, `-`, `_` or `.` |
| Connection | Invite Code | `system_link_invite` | Invite code from the relay host; a nonempty code selects relay mode |
| Connection | Direct LAN | `system_link_lan_enabled` | Direct LAN mode; cannot be combined with an invite |
| Connection | Peer-to-Peer | `system_link_direct` | Direct UDP to other relay players, with the relay as fallback; off by default, relay mode only |
| Matchmaking | Player Match Through Relay | `live_sessions` | Player Match through the relay room; on by default in ReRevved, relay mode only |
| Discord | Rich Presence | `discord_activity` | Shows ReRevved as the Discord activity; on by default |
| Discord | Join Button | `discord_invites` | Discord join button while hosting Player Match through the relay; on by default |

A pasted invite applies when the field loses focus or Enter is pressed. An
empty name becomes `Player-XXXX`. On the first System Link launch, the SDK
creates a random identity and saves it as `system_link_xuid` in the active
profile's `rerevved.toml`; a configured value is kept. A second copy on the same
PC needs its own `--profile=<id>` so that it has a separate identity.

The status reads `Off`, `LAN`, `Relay: waiting for System Link` until the game
first starts networking, which ReRevved does during boot, `Relay: connecting`,
`Relay: connected as 198.18.x.y`,
`Relay: lost connection`, `Relay: disconnected`, or `Relay failed:` with a
reason such as a bad invite, an unreachable relay, a wrong secret, or a
different game build. With Peer-to-Peer on, a connected status adds the
number of players reached directly, such as `(direct: 1 of 1 peers)`.
Every player in a room must run the identical executable build.

With `system_link_direct` on, two players in the same relay room who both have
it on try a direct UDP connection the first time they exchange game traffic.
Traffic between them moves to that connection when it works and returns to the
relay within a few seconds when it stops; broadcast traffic always uses the
relay. Direct datagrams are authenticated with a key the relay hands each pair.
Each player's local network addresses are shared with the other players in the
room.

With `live_sessions` on and an invite set, Multiplayer > Player Match uses
the relay room as its matchmaking service. A hosted game with an open public
slot is listed for the other players in the same room, with its host name and
a round-trip time measured through the relay, and joining it uses the relay
connection. Ranked Match finds no games. With LAN mode or no
invite, the status adds `(Live sessions inactive)` and Player Match behaves
as it does with the setting off.

With `discord_invites` on, ReRevved publishes a Discord join button while this
player hosts a Player Match game with an open slot. The button's join secret
carries a relay join token that is valid for about 15 minutes and is replaced
while the game stays hosted. Friends need the Discord desktop app and the
same setting on. A friend whose game is closed is started through the
`discord-<application id>` launch protocol, which ReRevved registers for the
current user whenever it starts with the setting on; that launch waits up to
10 seconds for Discord's join event. For that launch only, the join takes
precedence over the friend's System Link invite, explicit relay settings and
LAN mode; their saved values are kept. A friend whose game is running joins in
place when it uses the same relay with
Live sessions active. Any other System Link state, such as LAN mode, Live
sessions off or a different relay, needs a restart: ReRevved shows the reason
and asks the player to close the game and click Join in Discord again.
`--live_invite=<code>` on the command line joins the same way without Discord.

`system_link_relay_address`, `system_link_relay_room` and
`system_link_relay_secret_file` replace an invite. They, and
`system_link_local_address`, which selects the direct LAN interface, are set
in the config file or on the command line like `system_link_xuid`,
`system_link_trace`, `live_invite`, `discord_instance_id` and the test-only
`system_link_direct_drop_after`.

Relay mode is for private tests only. Invite codes contain the room secret.
Payloads and session tokens are unencrypted, and relayed datagrams have no
payload integrity or replay protection. Do not run a relay as a public service; open
its port to the Internet only for the duration of a private test.

Create a secret and an invite code on the relay machine, then start the relay:

```powershell
rexglue relay-invite --address <public-host>:30100 --secret-file <path> --create-secret
rexglue relay --port 30100 --secret-file <path>
```

`relay-invite` prints one code for room `testers` unless `--room` is given. The
relay binds `0.0.0.0` by default, and Ctrl+C stops it. The Windows package adds
`rexglue.exe`, `host-online.cmd`, `host-online.ps1`, `host-manual.cmd` and
`ONLINE.txt`.
`host-online.cmd` tries to open the UDP port with UPnP, creates invite codes, starts
the game against its own relay and runs the relay; `ONLINE.txt` walks
players through hosting, joining and troubleshooting. The SDK's System
Link relay documentation lists the remaining options and the invite format.

## Runtime boundary

The default Windows launch uses the `xenos` plugin and the D3D12 ROV
render-target path. The experimental `renderer=native` route uses title-owned
D3D12 presentation and requires explicitly configured shader inputs. The
main-menu logo is supplied through the bundled `rerevved-logo` asset override.
The renderer selection and a successful build do not certify graphics coverage.

The settled guest behavior belongs in [`scaleform-gfx.md`](scaleform-gfx.md).

The scene capture, route trace, shader harvest, Xenos frame reference, label
adapter capture and guest API call counts, and the `native_draw_replay` and
`native_vulkan_draw_compare` tools, exist only in a developer build
(`REREVVED_DEVELOPER=ON`, see [Development](development.md#developer-build)).
The player build does not define their console variables, and its draw, clear
and resolve consumes have no recorder reason to run Original; the consume
fallback reason `diagnostics` below counts only recorders in developer builds and
the exit checks.

## Presentation contract

The guest and ReXGlue both report a 1280x720 presentation. The front buffer
texture is a tiled 1280x720 `k_8_8_8_8` texture. An intermediate D3D viewport
may differ from scanout dimensions. The measured ReXGlue fetch remains
1280x720.

The title passes `VdSwap` a kernel output command buffer. The kernel writes
fetch constant 0 and a `PM4_XE_SWAP` packet, and ReXGlue executes that packet.
The D3D12 path is:

1. Copy-mode `IssueDraw` calls `IssueCopy`.
2. The render-target cache derives the resolve destination, copies EDRAM into
   shared memory, and marks the destination range as GPU-written.
3. Shared-memory watches invalidate overlapping texture data.
4. `RequestSwapTexture` decodes fetch constant 0 and `LoadTextureData` refreshes
   the tiled texture from shared memory.
5. `IssueSwap` applies the selected gamma ramp and submits the guest output to
   the presenter.

A GPU resolve must expose its destination to overlapping texture aliases. CPU
writeback is needed only when guest CPU code must read the result. ReXGlue's
graphics and `VdSwap` implementations retain their Xenia lineage.

## Draw capture

Start Xenos with `--d3d12_capture_draw=<absolute-output-directory>` pointing to
a fresh directory. At the desired scene, create an empty file named `arm` in
that directory. The SDK checks this marker at swap and captures the next
eligible draw. Capture is disabled when the option is empty.

The current capture supports untextured, rasterized triangle lists on the
D3D12 ROV path at resolution scale 1. It excludes tessellation, memory export,
and converted index buffers. The vertex ranges used by the shader and the
original indices are copied from GPU shared memory after residency; EDRAM is
copied immediately
before and after the selected draw. Registers and shader instructions are
stored as little-endian dwords. Version 1 of the manifest is marked complete only
after GPU completion and successful file writes.

Inspect a completed snapshot with:

```powershell
cd <repo>; python scripts/gpu/summarize-draw-capture.py out/diagnostics/<capture>
```

The summary requires a completed manifest, checks each declared file size, and
requires the registers, shaders, and EDRAM snapshots. It also counts changed
EDRAM bytes between the snapshots before and after the draw. These checks
establish file consistency and a byte difference, not native replay or visible
parity. Captures contain retail graphics data and belong in
ignored output directories.

## Native scene resource capture

This recorder and its options are part of the developer build only. Run Xenos
with `--native_scene_capture=out/diagnostics/<fresh-directory>` and
`--native_scene_capture_frames=2`. At the desired scene, create an empty `arm`
file in that directory. The recorder opens at the next VdSwap boundary and
records two complete intervals. `status.toml` reports readiness and failures;
`result.toml` records the completed metadata window.

`--native_scene_capture_arm_frame=N` arms the recorder when the VdSwap boundary
count reaches N, with no file. The option is read only at startup; 0, the default, leaves
file arming alone. The two triggers are independent and the first to fire opens
the window: the file opens it at the next boundary after the recorder sees it,
and N opens it at boundary N, so the window begins at N unless the file came
first. A later trigger cannot move an open or closed window. `result.toml`
records `arm_frame` beside `begin_frame` and `end_frame`, and `window` and
`trigger` (`arm_frame`, `file` or `pixel_digest`).

`--native_scene_capture_arm_ps_digest=<hex>` arms the recorder on the first
guest draw whose pixel shader word digest starts with the 8 to 64 hex digits
(any case). The word digest is the SHA-256 of the microcode with each 32-bit
word byte-swapped to host order, the `pixel_word_sha256` of a shader catalog;
the title-screen scene pair is `2cac681f`. The window opens
`--native_scene_capture_arm_delay=D` plus one VdSwap boundaries after the
frame that holds the draw (D defaults to 0, the next boundary), so the frame
containing the draw is never captured. The first match fixes the window; the
file and `arm_frame` triggers still apply and the first trigger to fire wins.
While the digest is pending, each guest draw reads its pixel shader microcode
and hashes it. The frame boundary waits for a draw that is still hashing, so a
match always counts from the frame its draw began in.

`--native_scene_capture_windows=K` (1 to 16, default 1) records K sequential
windows. Window 1 opens as above and keeps the capture directory. Window k >= 2
opens at the next boundary after `<capture>/arm-<k>` exists and writes
`<capture>/window-<k>/` with its own `events.toml`, `status.toml`,
`result.toml` and payload manifest and capture ID. Only the next window's file
is read, so create `arm-<k>` after window k-1 has closed (its `result.toml`
appears) or earlier, and it opens as soon as that window closes. A window
that never opens gets a failed `result.toml` at shutdown. The recorder keeps
observing between windows, so texture identities, storage and allocation
lifetimes carry over; the bytes retained for an earlier window do not, and a
later window's `events.toml` does not repeat creation events from before it, so
earlier textures are read again when first sampled. A later window's first
event is an `allocation_snapshot` of the allocations live when its session
started (`allocation_opening_records` counts them); allocator calls after it,
including those an earlier window's hooks still deliver, are logged by the later
window, so every payload `allocation_generation` is defined in its own window.
The cvar triggers apply to window 1 only. Route
effects and target requests are refused with more than one window.

Add `--native_scene_capture_payloads=true` to retain CPU input bytes. The option
is read only at startup and defaults to false.
`--native_scene_capture_payload_bytes=268435456`
sets the hard budget for unique retained bytes, including unpublished history.
Overflow stops payload retention, records the failure, and continues metadata.
With payload mode disabled, the recorder keeps its metadata format and performs
no additional guest reads.

`payload-manifest.toml` binds each byte range to the capture ID, frame, event,
logical submission, parent call, resource generation and version. Files under
`payloads/` use their SHA-256 digest as the filename and store unmodified guest
bytes. Identical content shares a file. State deltas contain consecutive BE32
triples: byte offset, previous word, replacement word. Apply a delta to its
matching `state_pre` payload and verify the event's post-state digest.

Retention covers referenced geometry elements and indices, declarations, shader
metadata and literal values, fetch templates and linkage lists, sampled CPU
texture storage, and observed CPU updates. Pre-arm history is restricted to
live k_8 1024x1024 and k_32_FLOAT 512x512 candidates. It is published only when
sampled and remains subject to the same budget. Other textures begin with their
first sampled version; relevant in-window updates are retained in event order.
Creation payload rows refer to the End event sequence, after the creation returns.
Sampled CPU blobs are reused until an observed resource version changes. Writes
outside the observed writer routes cannot be detected by this cache.

Shader literal tables are read as the guest loader (`sub_826ADB98`) reads them.
Its `recordBytes` field, at table offset 16, may be any value up to 4096 and
need not be a multiple of 8; the movie pixel shader's table is 20 bytes. The
loader consumes whole 8-byte records while the cursor is below the end, so a
last record may run past `recordBytes`, and it stops at the first record with a
zero count. The table is retained as `ps_literal_table` or `vs_literal_table`,
extended to cover every record consumed, and each record retains its values as
`ps_literal_values:<first>` or `vs_literal_values:<first>`, `count * 4` bytes
at the shader's resource base plus the record offset. A `recordBytes` above 4096
or from 1 to 7 fails retention with `scene_literal_extent`, and a count above
16384 dwords fails with `scene_literal_values_extent`. Either failure marks the
event's `payload_error` and leaves metadata intact.

Resolve records carry destination and alias epochs, raw copy/clear arguments,
sample encoding and a completion key. They do not read GPU output through RAM.
A resolve key is `<capture_id>:resolve:<call_id>`; guest return does not
establish GPU completion.

Payload rows carry an `allocation_generation` separately from their descriptor
generation. Observed physical allocation and free routes establish these IDs;
zero means the range has no observed lifetime. Reuse receives a new ID and
invalidates cached sampled bytes. The allocator stream allows 131,072 records
and 65,536 live ranges, tracked from startup under the shared event bound.
An overlapping allocation implicitly ends every overlapped live generation and
receives a new generation. Tracking continues, and `allocation_overlaps` counts
the retired ranges separately from budget overflow.
Overflow invalidates the live map, so subsequent bindings use generation zero.
Unobserved allocator families remain informational gaps and do not reject retained
payloads. A pool or ring sub-allocation shares its pool-wide generation; reuse
inside that pool cannot be distinguished by these observations.

Writer C records each observed region's source and destination rectangles,
lock pitch, fallback choice and retained destination rows before unlock.
The capture window and pre-arm history each have a separate 65,536-region budget.
Pre-arm regions are retained only for `historyCandidate` backends, with their
payloads charged to the shared byte budget and published on first sampled use.
`pre_arm_region_record_budget`, `pre_arm_region_records`,
`pre_arm_region_overflow` and `pre_arm_region_failures` report that history's
limits and counters in `result.toml`. Overflow marks the affected resource's
coverage incomplete, even if its final bytes are retained. Non-candidate pre-arm
region calls consume neither region budget nor payload bytes.
Calls crossing a window boundary leave incomplete coverage without counting a
region failure. The analyzer reports `writer_c_coverage` per
sampled k_8 use with writer C history or a retained-history candidate. Mip planes,
GPU-produced storage and tiled layouts are not applicable to this proof.
Covered and uncovered texels use the whole bound footprint. Closure requires
every observed resource version through the use to be the initial snapshot or a
validated writer C update, a matching allocation generation, and full coverage.
The analyzer replays creation, pre-arm and in-window region rows in order and
compares the result to the retained `sampled_base` SHA-256. Missing replay bytes, another
writer's version, a non-C version bump or a hash mismatch leave coverage open.
A snapshot of the final image alone does not establish intermediate region coverage.
When creation storage includes mips, replay uses only the `base_bytes` prefix.

Inspect `payload_complete` separately from metadata `complete`.
`payload_known_gaps`, `payload_failures`, `payload_read_failures` and
`payload_overflow` prevent payload acceptance. Allocator gaps and failures are
reported separately. `writer_c_coverage_complete` and `region_failures` describe
coverage independently of payload acceptance.
The analyzer reports a rejected optional stream without discarding metadata.

`--native_scene_capture_targets=true` requests opening seeds and one lossless
reference per captured interval. It requires Xenos and payload capture and is
rejected on the native route. `--native_scene_capture_target_bytes=67108864`
sets the byte budget; the record budget is 256. Target receipts identify the
frame, resource generation, footprint, aspect, format, sample layout, resolve
interpretation and completion token. Their packed rows are stored unchanged
under `targets/` and verified by SHA-256 through `target-manifest.toml`.

The installed SDK does not expose the ordered opening-footprint readback and
guest-window join needed by this recorder. The title records an unavailable
opening request and unavailable closing references, with `target_complete=false`
and nonzero `target_gaps`. It does not substitute CPU memory or skip a seed based
on unproved clear parameters. Effective GPU copy/clear semantics, target bytes,
matching frame references and completion receipts remain external requirements;
this capture alone is not an executable scene package.

Add `--native_scene_capture_effects=true`, together with payload capture and
`--native_route_trace`, to write `route-effects.bin` in the scene directory for
the armed window. The 10 MiB recorder keeps call identities, full `r3` return
values, changed or unreadable field pairs, command-cursor and linked-record
range hashes, copy-destination hashes, first reader observations and aggregated progress
samples. Its header reports lost records, failed reads, coverage gaps and
unfinished calls. The same counters and encoded byte count appear in
`result.toml`; any failure counter rejects effect acceptance, and scene `complete`
alone is not an effects claim. Progress values are sampled at reader entry and
return, not at the exact guest load, and cursor relocation is a coverage gap.
The records associate observations with scene submissions; they do not prove
GPU retirement.

The binary header is 64 bytes, with magic `RREVFX01`, schema 1 and little-endian
integers. Begin/end records use 136 bytes per call together; each changed or
unreadable field uses 80 bytes, and each hashed range uses 104 bytes. The 72-byte
end record includes the unchanged field count and total observed field count
as its final two u32 values. A projection with two calls, 17 changed fields and
four ranges per logical draw is 2,048 bytes per draw: 3,075 draws use 6,297,600
record bytes, about 6.01 MiB, leaving about 40 percent for other records within
the 10 MiB budget. This is a sizing fixture, not a measured owner capture;
extra changes, copies, linked ranges and progress transitions consume the margin.

Validate hashes and identities with:

```powershell
cd <repo>; python scripts/gpu/analyze-scene-resources.py --capture out/diagnostics/<capture> --output out/diagnostics/<fresh-analysis>.json
```

The analyzer validates the effects header, record extents and result counters
before reporting effect acceptance. The JSON includes the first logical
submission's manifest bindings for independent extraction. `payload_bytes`
counts all retained unique bytes; `published_bytes`
in analysis counts files actually written. The existing `output_bytes` counter
covers the metadata recorder's files separately. Captures contain retail data
and remain in ignored output directories.

## Shader harvest

The harvest and its options are part of the developer build only. Run Xenos
with `--native_shader_harvest=out/diagnostics/<fresh-directory>` to
record the shaders of every distinct draw while the title runs, for offline
shader catalogs. Add `--native_shader_harvest_caller=0x8250CB98` (an exact
0x-prefixed 8-digit return address) to keep only one call site's draws. All
harvest options are read only at startup; the harvest is refused on the native
route, with
a caller that does not parse, with out-of-range frame form options, or in a
directory that exists or shares a tree with the scene capture, frame
reference, route trace or adapter capture output.

All six draw hooks observe each draw after Original returns, when the guest
has applied its vertex fetch patch: IndexedBuffer, indexed-UP, nonindexed-UP,
AutomaticIndex, and IndexedUpLowerer and NonindexedSink when guest code calls
them directly. The two UP wrappers' own child calls fold into the wrapper's
draw. A key is the caller, the hook entry, the XXH3-64 of each stage's
big-endian microcode (the shader catalogs' `vertex_hash` and `pixel_hash`;
the vertex shader is the patched one), the alpha test (RB_COLORCONTROL
function and enable, 0 when disabled), RT0's format and exponent bias, the
MSAA mode, the primitive (`r4`) and the target form: bound color slots, depth
binding, surface pitch, EDRAM bases, RT0's surface extent and the tile count
when RT0 is the target BeginTiling saved. A draw without a pixel shader is a
vertex-only key with pixel hash 0 and the vertex shader variant the guest
selects: variant 1 when the shader header word at +872 has bit 0x20, else 0.

A known key's draw reads the shadow and both shaders' microcode and counts it.
The first draw of a new key writes
`<caller>-<vertex_hash>-<pixel_hash>-<key>/` with `vs_code.be.bin`,
`ps_code.be.bin` (absent for a vertex-only key), `state.be.bin` (the
0x3500-byte device shadow at that point) and `record.toml`: the key and its
fields, the hook entry, `r3..r10`, `r1`, both shader objects and word digests,
stream 0's stride, the shader headers, metadata, fetch records and templates,
the vertex declaration, each stage's literal records with their values, and
each sampled texture slot's fetch words and decoded shape, and the bool and
loop constants. Key records keep no geometry or texture bytes. The harvest does
not use the SDK frame input
observer, so it runs beside a frame reference or scene capture.

The shadow holds the bool constants (registers 0x4900..0x4907) and loop
constants (0x4908..0x4927) contiguously from `G+0x2780`: the state block
routines 0x826A4CD0 and 0x826A50A0 copy the vertex bools at `+0x2780`, pixel
bools at `+0x2790`, vertex loops at `+0x27A0` and pixel loops at `+0x27E0`.
`record.toml` (schema 3) lists them as `bool_constants` (8 words) and
`loop_constants` (32 words, pixel loop constant 0 at index 16) in register
order; a register snapshot built from `state.be.bin` maps all 40 words.

`index.toml` (schema 3) lists the keys in discovery order with their
directory, fields, hashes, word digests, first frame and draw count, and
counts frames (VdSwap boundaries), harvested draws, draws without a vertex
shader, draws whose shaders could not be read, draws by hook entry and key
file write failures (`first_error` names the first read failure; a key row
carries its own `error`). `known_harvests` lists the known harvests, and a key
counted under a known harvest's record carries `known_harvest`. It is replaced
whole when a key is found, every 60
frames while counts change, and at shutdown.

### Frame forms

`--native_shader_harvest_frame_forms=N` (0..16, default 0) also records frame
forms in process; 0 keeps every frame form path off. Each frame from one
VdSwap to the next is a list of operations: clears at 0x826A1D90 entry,
resolves at 0x8269E8A0 with D3DDevice_Resolve's arguments from its entry,
BeginTiling when it returns, and draws as above. Its signature is the ordered
non-draw operations, with the distinct draw tokens between each two of them as
a sorted set, and a token where the tiling flag changes without a BeginTiling.
A clear token holds its flags, target index, rectangle and target form; a
resolve token its source flags, copy control, destination info and pitch,
rectangle, point and destination texture shape, without the destination base;
a draw token its key; and every token the tile count, predication and
predicate mask. A caller filter applies to frame form draws as well.

The first N frames of each new signature are written to
`frames/<signature>-<n>/frame.toml`: the canonical signature and each
operation in order with its hook entry, caller, `r3..r10`, `r1`, `r3` after
Original, the clear's `f1`, a draw's key directory and bool and loop constants,
the device shadow, the bound
surface objects and the memory it names. Indexed-UP and nonindexed-UP draws
keep their vertices and indices; a direct IndexedUpLowerer or NonindexedSink
call keeps the destinations its caller fills, read at the next operation;
IndexedBuffer and AutomaticIndex draws keep their declared streams (and
IndexedBuffer its indices), and every draw its sampled textures, read at the
swap. Each memory row names when it was read. Blobs over 64 bytes are stored
once by content under `payloads/`.

Frames whose first draw precedes every clear are written as
`<signature>-marked-<n>`, up to N per signature beside the unmarked ones,
from a budget of their own. `--native_shader_harvest_frame_mib=M` (1..65536,
default 1024) sets each budget; a frame is written only while its budget is
unspent. A frame whose buffered operations pass 128 MiB keeps only its
tokens and is counted as truncated, so a later frame of that signature is
written. `frames/index.toml` lists every signature with its first frame,
frame counts and written instances, and `frames/signatures.log` appends each
new signature's canonical form. At most 4096 signatures are listed; frames
of a further new signature are counted as `unlisted_frames` and neither
listed nor logged, unless known harvests are set and the frame is new content
(below). The written frames are not in the SDK frame
reference format: they hold guest CPU-side operations and state, the inputs
the native assembler and adapters consume.

### Known harvests and arming

`--native_shader_harvest_known=<dir>[;<dir>...]` names earlier harvests whose
frames are not captured again. Signatures are stable across runs: tokens hold
register fields, key hashes, rectangles and shapes, never heap addresses,
resolve destination bases, frame counts or times. They still vary within a
session with game state: draw runs change with visible content and stencil
clears under UI panels change their rectangle, so one 58-minute session had
35,727 signatures and an exact signature filter alone would spend the budget
again on early world frames. With known harvests set:

- A signature with a written instance in a known harvest (or a marked
  instance, for a marked frame) is counted as `known_frames`, never written.
- Before the first unmarked instance of any other signature is written, the
  frame's coverage is compared. Coverage keeps the frame's non-draw operations
  in order, each marked by whether draws precede it, and the draw tokens of
  each segment between two of them. A depth-only clear's rectangle becomes its
  class: the whole 1280x720 output, inside it and nonempty, or outside it,
  which are the cases the frame assembler tells apart. Every other clear keeps
  its rectangle. A written frame of the same shape covers the new one when
  each of its segments holds the new segment's draws. A frame that a known
  harvest's or this run's written frame covers is counted as
  `covered_frames`. Otherwise it is written, its signature listed past the
  4096 cap for at most 4096 further signatures, and later instances of its
  signature are written as usual. A marked frame that no known harvest wrote
  is listed past the cap the same way. A signature whose write fails is marked
  `write_failed`, counted as `failed_frames` and not written again.
- A known `index.toml` of schema 3 or later lends its key records: such a key
  is counted with `known_harvest` and its known directory, and no record is
  written. Keys of an older index are recorded again, since those records lack
  the bool and loop constants.
- Only files a known harvest holds count: an instance whose `frame.toml` is
  missing and a key whose `record.toml` is missing are not known. A row that a
  known harvest lent from a further harvest loads that harvest in turn; if it
  is unreadable, the row is not known.

Each known directory needs `index.toml`; `frames/index.toml` and the canonical
lists of written frames are read when present. An unreadable known harvest, or
one that would contain the new directory, refuses the harvest.

`rerevved_tests --helper shader-harvest.replay <known> <recorded> [frame] [MiB]
[signatures] [instances] [arm frame]` replays a recorded harvest's signatures, in
first-frame order, through the same admission (8192 MiB, 4096 signatures, two
instances and no arming unless given) and reports which would be written,
before and from the given frame, at the recorded harvest's mean instance
size.

`--native_shader_harvest_arm=<file>` writes frames only once the file exists,
relative to the harvest directory unless absolute (for example `arm` for
`<harvest>/arm`); it needs frame forms. Keys, records and signatures are
recorded from the start, and earlier frames are counted as `unarmed_frames`.
Signatures listed before arming do not count toward the cap for those listed
once armed.
`frames/index.toml` (schema 2) names `arm_file` and `armed_frame` (-1 while
unarmed) and lists `known_harvests`, `known_frames`, `covered_frames`,
`failed_frames` and, per signature, the `known_harvest` that wrote it.

## Xenos frame reference

The reference and its options are part of the developer build only. Run Xenos
with `--native_xenos_frame_reference=out/diagnostics/<fresh-directory>`
and `--native_xenos_frame_reference_arm_frame=N` to save one complete Xenos
frame with its presented output. Both options are read only at startup. The VdSwap
boundary that brings the count to N, counted as the scene capture counts it,
reserves the request before it returns. The SDK frame capture then owns the
next frame the command processor completes, so the saved frame is at or near N.
`input/manifest.toml` lists each
event's registers, microcode, vertex fetches and texture uses, the host
textures, the gamma table, the pre-gamma swap source (`source.bin`) and the
RGB10 output (`reference.rgb10`). `reference.toml` is written last and records
the status, the request boundary and the frame span. The SDK frame mailbox
holds one request at a time, so the option fails when another frame consumer
holds it.

The reference has the same triggers as the scene capture, and the first to fire
requests the frame; whichever fires reserves the mailbox before it returns, so a
slow worker cannot move the request.
`--native_xenos_frame_reference_arm_frame` is optional. An `arm` file in the
reference directory requests the frame at the next boundary after the worker sees it.
`--native_xenos_frame_reference_arm_ps_digest=<hex>` and
`--native_xenos_frame_reference_arm_delay=D` capture the frame `D + 1` swaps
after the frame holding the first draw with that pixel shader word digest (see
the scene capture). The digest is matched on the command processor through the
SDK frame input observer, not at the guest's VdSwap, so a command processor that
runs behind the guest still captures exactly that frame; the option fails when
another observer consumer holds the observer. Once window 1 is requested by any
trigger, the reference releases the observer. The SDK delivers draws only after
the first swap that follows registration, so a digest seen only before that swap
does not trigger. `--native_xenos_frame_reference_windows=K`
records K windows: window k >= 2 requests a frame at the next boundary after
`<reference>/arm-<k>` exists and writes `<reference>/window-<k>/`. `reference.toml`
adds `window`, `trigger` and, for the digest, `arm_pixel_digest`, `arm_delay`,
`digest_gpu_frame` (the SDK frame holding the draw) and `request_gpu_frame` (the
SDK frame whose end reserved the request; the captured `start_frame` follows it).
A window that never opens leaves no directory.

Title screen, armed by its pixel shader (both captures key on the same draw):

```text
--native_scene_capture=out/diagnostics/title-scene --native_scene_capture_payloads=true
--native_scene_capture_frames=4 --native_scene_capture_arm_ps_digest=2cac681f
--native_xenos_frame_reference=out/diagnostics/title-reference
--native_xenos_frame_reference_arm_ps_digest=2cac681f
```

Main menu, armed by the owner once it is on screen (create both files; the
captures open at the next boundary):

```text
--native_scene_capture=out/diagnostics/menu-scene --native_scene_capture_payloads=true
--native_scene_capture_frames=4 --native_xenos_frame_reference=out/diagnostics/menu-reference
New-Item out/diagnostics/menu-scene/arm; New-Item out/diagnostics/menu-reference/arm
```

For the new-game path add `--native_scene_capture_windows=5` and
`--native_xenos_frame_reference_windows=5`, then create `arm-2`, `arm-3` and so
on in both directories at each scene.

The `movie_draw_adapter_reference` case of `rerevved_tests` replays the saved
intro movie draw natively from the frame's own registers, vertices, planes and gamma table and
compares the RGB10 output with the Xenos output over the full frame, row 0 and
column 0. It skips unless `REREVVED_MOVIE_REFERENCE` names the directory and
`REREVVED_TEST_NATIVE_D3D12` is 1.

## Native offscreen draw replay

After a developer build of the `native_draw_replay` target (see
[Development](development.md#developer-build)), replay a decoded draw with the
headless native D3D12 harness:

```powershell
cd <repo>; .\scripts\rexglue.ps1 -Stage Replay -ReplayRecipe out/diagnostics/<capture>/replay-input/native-replay.toml -ReplayOutput out/diagnostics/<capture>/native-samples.rgba
```

The [recipe guide](../tools/README.md) defines the admitted
shader and resource subset. A recipe supplies executable shader stages, decoded
vertex attributes and indices, constants, render state, and the initial color
samples. Replay runs without booting guest code and writes its sample readback
after GPU completion. The output path must be fresh and its directory must
already exist. Compare the resulting sample planes with the captured draw's
output; the background supplied to replay remains captured data.

## Guest frame capture

The D3D12 frame capture uses [RenderDoc](https://github.com/baldurk/renderdoc)
for GPU resources and commands, with a companion SDK journal for original
Xenos registers, shader microcode, and guest draw/copy ordering. It is separate
from the single-draw replay and does not require native rendering.

Launch through the driver with RenderDoc injected before device creation:

```powershell
cd <repo>; .\scripts\rexglue.ps1 -Stage Launch -Interactive -RenderDocCmd <renderdoc-directory>/renderdoccmd.exe -LaunchArgument '--d3d12_capture_frame=<absolute-capture-directory>'
```

Use a fresh capture directory. At the desired scene, create its empty `arm`
file. The SDK starts capture after a completed guest swap and ends after the
next guest swap, so the boundary follows guest rendering rather than the
independent host presentation loop. Capture remains disabled without the option
and requires RenderDoc to be attached. A completed bundle contains the `.rdc`
and the guest-state journal; failures and bounds violations are explicit.
Publication requires the guest queue's ending fence to complete. Manifest
completion certifies the capture envelope and GPU completion; per-event results
and the validator's `failed_guest_events` retain failed guest operations.

Validate the journal and inventory before analyzing the GPU capture:

```powershell
cd <repo>; python .\scripts\gpu\validate-frame-capture.py <capture-directory>
```

This validation checks the capture envelope. RenderDoc replay and matching the
journal's issued-draw markers to GPU actions establish the GPU dependency set.

`-RenderDocCmd` requires an interactive Launch stage. The driver reports the
capture launcher's exit code because RenderDoc does not propagate the game's
exit code. Close the game normally after the capture completes. Captures contain
retail graphics data and stay in ignored output directories.

## Native fixture presentation

`renderer=native` uses a title-owned Plume D3D12 device, queues and swapchain.
ReXGlue creates the window with graphics detached. With
`--native_present_fixture=<catalog-directory>`, the validated shader catalog for
the admitted `rgba8-32-tiled` fixture, it renders the fixture draw and resolve
through the native draw stream without content selection, a Runtime or guest
launch. Missing or incompatible fixture inputs fail explicitly.

In a developer build, `--native_present_trace=<new-file.toml>` records startup,
publication frame IDs, gamma generations, presentation results, surface changes
and shutdown, and `--native_cadence_trace=<new-file.toml>` records timestamped
swap, publication and present events with their rates. The player build has
neither. The
producer applies its copied gamma table once; each published frame retains its
generation through host copies and repaints. Resize, DPI, fullscreen and focus
events update the title session; minimization suspends presentation. Closing
vetoes requests while admission closes and the producer and host drain within a
shared five-second deadline. An unproved drain is reported as failure and retains
GPU owners until process exit. Overlay teardown runs on the UI thread while the
device lives, before accepting ordinary window close.

SDK overlays use the title's immediate drawer on the same Plume device. F1 opens
settings, `bind_debug_overlay` (no default key) opens debug, Backtick opens the
console, F2 opens the mod manager,
and F3 opens achievements when a Runtime is available. Achievement notifications
also require a Runtime; the fixture route has none. SDK keyboard and mouse capture
remain active while a dialog requests them.

The UI thread copies each draw into an immutable snapshot of the latest frame containing
vertices, indices, commands and shared texture data. The presentation thread
uploads through Plume and retains snapshots, textures, descriptors and buffers
until their host submission slots retire. The mailbox mutex protects snapshot
exchange and readiness. Callers release it before entering session locks, ImGui
or GPU work. Close takes session, drain, then mailbox locks; there is no reverse
nesting.
Overlay work uses the session's existing drain and shutdown deadline.

Composition scales and letterboxes the post-gamma native image, draws overlays at
backbuffer resolution, then presents the same backbuffer. Repaint updates or
removes overlays without a new native frame or a second gamma pass. Minimized
and zero-size windows retain suspension behavior. Trace events include
`overlay_composed`, `overlay_cleared` and `overlay_failed`.

Native mode rejects hybrid collectors and Prepared advisory/live capture. The
Xenos route retains its existing controls. In a developer build,
`native_present_script` requires the fixture.

## Native guest boot

Without `--native_present_fixture`, `renderer=native` selects game content and
boots the guest with no SDK graphics system. Title guest video services receive
the interrupt callback, ring and read-pointer writeback registrations and
`VdSwap` notifications through the SDK detached-video delegate. They serve the
GPU register range, raise vblank interrupts at the configured refresh rate and
record ring, write-pointer and swap metadata. They do not execute GPU command
packets. The presentation session starts the native draw stream on its own
device. Until the first native frame is published it presents a black canvas
with SDK overlays, and in a developer build `native_present_trace` records
`idle_present` outcome changes. Published frames are traced as `published` and `present` events.

`--native_stream_queue` is read only at startup and is true by default. It lets
the guest queue
its next draw stream request while the stream records one. With
`native_direct_stream=false`, the stream holds two
requests, counting the one recording, and records them one at a time in order.
It admits a queued request when it takes it, against the surfaces, textures and
owned-byte charge the requests before it left, so a refused admission fails
that request's result, with no submitted fence, instead of the call that queued
it. A refused prefix packet, or a refused frame that would close an open
prefix, also stops the stream, since an open prefix cannot end without its
frame. With false, each request waits for the stream to drain and its caller
admits it. Synchronous submissions drain the queue first either way.

`--native_direct_stream` is read only at startup and is on by default.
The stream charges each request's own bytes to a running total, released when
the request retires, instead of walking every owner. It holds up to two
terminal frames instead of two requests, and destroys retired frames on a
deleter thread.

### Completion ledger

Guest progress comes only from a title completion ledger. Hooks on the guest's
own emitters record, in program order on the submitting thread:

- the fence emitter `0x8269BCA8`: `PM4_INVALIDATE_STATE`, then the cursor for
  `B+4` and the fence for `B+0`, where `B = *(G+0x2A90)`;
- the callback emitter `0x8269BF40`: callback, parameter, CPU mask and flags,
  with the scratch block `S2 = *(G+0x2A94)`;
- the present path `0x826A4150`: `SCRATCH_REG1 = 1` written back to `S2+4`,
  and its waits for `S2+4` to equal 1 and then 0;
- the GPU timestamp `EVENT_WRITE_SHD` at `0x826A9BDC`, which the swap path
  `0x826A4638` aims at one of the eight words at `B+0x40`. Its initiator
  `0x80000003` stores the GPU counter, so the ledger writes an increasing
  nonzero counter there. Any other address fails the session with
  `timestamp_address_outside_block`;
- every guest draw through the draw dispatcher, `D3DDevice_Resolve`
  `0x8269E520`, its computed copy words at `0x8269E8A0`, per-target clears at
  `0x826A1D90`, and every `VdSwap`.

Every original function still runs. A `CP_RB_WPTR` write closes the open span.
The GPU Completion thread releases closed spans in order. A span with admitted
draws waits for the native host fence that covers them. A span without GPU work
releases once its effects apply. Effects are aligned big-endian stores to guest
physical memory in record order. A callback writes `S2+0x10`, `S2+0x14` and
`S2+0`, raises interrupt source 1 on CPU 2 through the vblank thread, waits for
the handler to clear `S2+0`, then writes `0x0BADF00D` to `S2+0x10`. A wait holds
later records until the guest word matches. The read-pointer writeback
`B+0x3C` is written last with the span's write pointer.

A `COHER_STATUS_HOST` register wait, a `WAIT_REG_MEM` on register `0xA31`
until bit 31 clears, is recorded as an ordering barrier at its packet header in
`0x82697CA0`, `0x826980A0`, `0x8269BAE8` and `0x826ACD20`. It writes nothing.
Earlier work in its span completes before any later record applies; the span's
host fence already covers every admitted draw in the span.

After `VdSwap`, `0x826A4458` reprograms the display. Its `WAIT_REG_MEM` packets
at `0x826A44E0` and `0x826A45C8` wait until the pending bit of
`AVIVO_D1SCL_UPDATE` (register `0x1973`) clears. Only the GPU waits, on a
display register, and no guest word is written or polled, so these waits are
recorded as the same ordering barrier.

Content without a completion model leaves its span unowned, and that span never
releases. This covers the event writes listed in
`config/native_progress_hooks.toml`, callbacks for another CPU or with a predicate
other than the first-pass bin mask `0x80000000`, and work the active draw mode
may not skip. Nothing else writes fences, scratch words, the read pointer or
interrupt state. The ledger never sets the guest's CPU fence mode, bit `0x2` of
`G+0x2ABD`. A fence recorded with that bit set while `G+0x541C` is zero, when
the emitter stores the fence on the CPU, fails the session. So do a fence block
whose `B+0x3C` is not the registered writeback address and a second thread
recording into one span.

D3D's tile-replay worker (thread entry `0x826ABEA8`) re-submits tiled command
segments once per tile and owns the ring while `G+0x2B00` is nonzero:

- A segment closed while `G+0x33B0` is set joins the tiled list that
  `0x826AC0A8` started last, at `0x8269C4E8`, instead of being kicked. Its
  records release with the next main kick. A segment recorded before any list
  fails with `tiled_segment_without_list`.
- The worker replays only the active list: the newest list whose
  `0x826AC32C` callback buffer was submitted. EndTiling chains that buffer
  last on the previous list, so its submission retires every older list. The
  recording thread may begin several newer lists first, two per frame with the
  MRT prepass. Re-submissions of active-list segments are host-only. Only the
  read pointer passes them once nothing kicked earlier is unreleased.
- While tiling, a fence, scratch write, wait, unmodeled packet or unpredicated
  callback fails the session with `tiled_progress_<kind>`. The first-pass
  callback in the last tiled segment and the `0x826AC32C` callback after it
  each queue one entry that wakes the worker. Neither waits on the worker.
- While `G+0x2B00` is nonzero, `0x8269BD98` chains each kick onto the worker's
  list at `0x8269BDE8`. That deferred span releases only after a worker
  thread submits the same physical segment and size at `0x826AB890` or
  `0x826AB990`, in the order deferred. Otherwise it raises `G+0x2B00` by its
  `r7` argument and submits the segment directly at `0x8269BE5C`.
- EndTiling `0x826AC218` emits its worker-kick callback at `0x826AC32C` and its
  fence at `0x826AC33C` into side buffers before it submits them. Each record
  joins the span that submits its own buffer, directly or deferred. The
  first-pass callback joins the worker-kick callback, so no worker entry exists
  before the submission that raises `G+0x2B00`.
- A worker kick never closes the main thread's open span. In both draw modes
  the session fails with `replay_submit_inactive_list` for a segment of a
  retained list that is not active, and with `replay_submit_unmatched` for a
  submit that matches neither the oldest deferred segment nor a retained tiled
  one, or that names a younger deferred segment. It fails with
  `worker_kick_unattributed` for a worker kick without a submit, and with
  `progress_on_replay_thread` for any progress record on a thread that entered
  `0x826ABB90` or for the worker's own swap at `0x826ABF90`. A record before a
  held side buffer is submitted fails with `end_tiling_buffer_not_submitted`.
  A main kick without its `0x8269BE5C` submit while such a buffer is held fails
  with `main_kick_unattributed`. A `VdSwap` that still holds the first-pass
  callback fails with `first_pass_callback_not_submitted`.

Under `--renderer=native` the proved consume stack is on by default:
`native_ring_stubs`, `native_resource_mirrors`, `native_surface_registry`,
`native_direct_stream`, the IndexedBuffer and user-pointer draw consumes
(plain, `_tiled` and `_dirty`), `native_consume_clear` and
`native_consume_resolve` with their `_tiled` forms,
`native_consume_set_predication` with its pre-swap form,
`native_consume_window_writer`, and
`native_gamma_apply`, which applies the guest's display gamma table (see the
display gamma paragraphs below). The lowerer consumes stay off. The exit,
derive, effect and observe checks and the consume counters exist only in a
developer build (see [Development](development.md#developer-build)). Each
consume still refuses to Original when its model does not hold. The draw,
Clear, Resolve, SetPredication and window writer consumes act only while the
ring stub services are published, which only `--renderer=native` does. Without
them (any other renderer) a consume hook runs Original at the cost of one flag
load: it reads no guest memory and stacks no entry bookkeeping.
The lowerer consumes keep their own cvar gates. `native_gamma_apply` is gated
by the renderer selection instead of the published services, which
`--native_ring_stubs=false` turns off and which can be withdrawn mid-session:
it takes effect only when startup selected `--renderer=native`.

A consume that goes on holds a publication lease from its decision through its
guest stores. Closing the window withdraws the services, and a layout mismatch
disables the stubs; both clear the flag and then wait, up to five seconds with
an error logged if a lease never ends, for the leases other threads hold, and
the withdrawal runs before the ledger closes. A consume another thread
started with the stubs active therefore finishes before the withdrawal
returns, unless the five-second bound expires first, and one that starts
later runs Original. Leases the withdrawing thread itself holds are not
waited for. The window writer's Original observer stops counting once
the stubs are disabled. Ring helper stubs and the lowerer consumes take no
lease.

`--native_ring_stubs` is on by default. It runs the ring helpers of the native
renderer path without ring or PM4 writes; the completion ledger still
completes their fences, callbacks and timestamps. The `Native frame timing:`
line counts each served call as `stub_<helper>` and each call left to guest
code as `stub_<helper>_original`. Publish `0x8269C400` is a classification
exception: `stub_publish` counts Submit and Starved, while
`stub_publish_original` counts Recording, Tiling and Unreadable. Every
Publish classification still dispatches Original, including its common tail.
EndTiling `0x826AC218` is stubbed as a unit.
It drops the coherency packets of `0x8269BAE8` and their submit but keeps
their barrier records, and runs every other call at its original return
address. Its worker-kick callback and fence go through the stubbed emitters,
and both submits go through `0x8269BD98`. While the worker owns the ring, that
helper's deferred path stays guest code: it writes only list tokens, and the
deferred fence releases on the worker's submit. The tiled segment branch of
`0x8269C400` also stays guest code, because it writes only two list words.
After the stub fields the line adds `tiling_begin` (BeginTiling `0x826AC0A8`),
`tiling_end` (`0x826AC218`), `tiling_callbacks` (deliveries to `0x826AC020`)
and `tiling_tokens` (`0x826AB740` calls), each with an `_original` variant.
`tiling_end_original` counts EndTiling calls left to guest code; the other
variants count calls made before the stubs checked the device.

The VdSwap call at `0x826A4884` in the present owner `0x826A4638` is replaced
by a preflight followed by a commit. The preflight reads the guest memory, the
kernel and its owner, the detached video delegate and the swap arguments, and
changes nothing. A refusal or an exception in the preflight returns false, so
the original VdSwap runs exactly once. The commit calls the renderer swap
observer and then the delegate's `OnSwap`. Once the commit starts the hook
returns true on every path, so a call that threw after its first effect never
also runs the original VdSwap, which would swap a second time and write a PM4
packet into the reservation. The exception is counted as
`present_commit_failure`, logged for the first failure and for each later
power of two with the running total, and never rethrown. With the stubs
inactive the hook is one flag load and counts nothing.

While the stubs are active the `Native frame timing:` line adds one count per
outcome, omitted when zero, and, in a developer build, the `Native ring stubs: shutdown`
line of the original residue probe carries the same fields. They are `present_enter`,
`present_consumed`, `present_commit_failure`, `present_refused_runtime` for a
missing guest memory, kernel or owner, `present_refused_delegate`,
`present_refused_args` and `present_refused_exception`. Every call has exactly
one outcome, so `present_enter` equals `present_consumed` plus
`present_commit_failure` plus the four refusals. `stub_present_original` equals
the sum of the refusals and `stub_present` equals `present_consumed`. A commit
failure reports neither. The timing line takes the counts per window, so a call
in flight at a window boundary shows its entry in one window and its outcome in
the next. The equality holds exactly over the totals of all windows.

The tiling probe and its option are part of the developer build only (see
[Development](development.md#developer-build)).
`--native_tiling_quiesce_check=true` (read only at startup, off by default) observes the
tile-replay chain and changes neither guest memory nor output. Its hooks read
guest words and count. Without the cvar each wrapper scope costs an inline
flag test at entry and at exit, and the other hooks return at their first
line. A guest read copies under a structured-exception guard, so a page
decommitted or reprotected after the access check fails the read (counted
in `tiling_probe_error`) instead of faulting. Below, `G` is
the device, `W = G+0x2AC0` the replay worker and `Q_i = G+0x2C3C+80*i` its six
queues. The counters appear on the `Native frame timing:` line, so the probe
reports under `--renderer=native`. Each window adds the fields below, and the
first eight events of each kind log a `Native tiling probe:` line.

The EndTiling and `0x8269BD98` observations sit in the two wrappers, so they
hold with the ring stubs on or off. Everything inside EndTiling's own body,
the optional-submit guard and the `opt` site, counts only when that body runs,
which is `--native_ring_stubs=false` or the calls counted in
`tiling_end_original`. The worker, token handler and list-store hooks sit in
code that always runs as guest code. The publisher hook counts the calls that
reach the guest body, which with the stubs on are the calls the stub refused
(a sink is set or the device was not yet validated).

- `tiling_bracket_end` counts EndTiling returns, `tiling_bracket_dropped` the
  ones that return nonzero (an allocation failure drops the bracket) and
  `tiling_bracket_unreadable` the ones whose window could not be read. A
  completed bracket snapshots `G[0x2AC0,0x2D00)` (`tiling_quiesce_snapshots`).
- A worker thread that finds its queue empty at `0x826ABF20`, just before it
  parks, counts in `tiling_worker_idle`. The guest words of one park are
  sampled under the bracket count, the interpreter entry count and the number
  of interpreter runs in flight (entered at `0x826ABB90`, not yet returned at
  its epilogue branch `0x826ABC48`) of that moment. Several workers share `W`,
  so another worker can be mid-list while this one parks. When a snapshot is
  pending and a run was in flight at the sample, or a bracket or run began
  after it, the park counts in `tiling_quiesce_stale` and the snapshot stays
  pending: with none in flight at the sample and no entry since, no run
  executed while the words were read. Otherwise the replay
  counts as quiet only if every queue has `Q_i+0x38 == Q_i+0x3C`, and `W+0`,
  `W+0x3C` and `W+0x48` are zero. If not, the park counts in
  `tiling_quiesce_busy`. A park before the interpreter `0x826ABB90` started a
  list since EndTiling was entered counts in `tiling_quiesce_early`, and the
  snapshot stays pending. A list the worker starts while EndTiling still runs
  therefore counts, and `tiling_quiesce_overlap` counts the compares of such
  brackets: the words that list moved before the snapshot are in the snapshot,
  so only changes after it show as a delta. A quiet park compares the window
  with the snapshot.
  `tiling_quiesce_ok` counts windows equal outside the expected words, and
  `tiling_quiesce_delta` the ones that are not, with the sum of their differing
  words in `tiling_quiesce_delta_words`. The expected words are `G+0x2AC8` (the
  ring write index), `G+0x2B00` and `G+0x2B04`, and `Q_i+0x38` and `Q_i+0x3C`
  (the pushed and consumed counts). `W+0x2C`, the count of parked worker
  threads, is also ignored and counted apart in `tiling_quiesce_wait_count`.
  `tiling_quiesce_pending_nonzero` counts compares that found `G+0x2B00`
  nonzero. `tiling_quiesce_unobserved` counts snapshots that the next bracket
  replaced before any compare. The log line of a delta lists up to eight
  differing words as `+<offset from G>:<snapshot>-><now>`.
- `tiling_worker_lists` counts entries of the interpreter `0x826ABB90`.
- `tiling_submit_guard_nonzero` and `tiling_submit_guard_zero` count the guard
  word that EndTiling tests at `0x826AC348` before its optional submit at
  `0x826AC364`. Each call of `0x8269BD98` counts under the site of its return
  address: `opt` (`0x826AC368`), `mid` (`0x826AC3B4`, EndTiling's submit with
  the worker-kick callback), `final` (`0x826AC3E8`, its fence submit) or
  `other`. The fields are `tiling_submit_<site>_calls`, `_r7_0`, `_r7_1` and
  `_r7_other` (the incoming `r7`), `_deferred` and `_direct` (the arm that
  served the call, `_noarm` for neither), `_before_nonzero` and
  `_after_nonzero` (`G+0x2B00` at entry and return) and `_adds_r7` or
  `_adds_other` (whether `G+0x2B00` rose by exactly `r7`). The word is read
  without a lock, so a worker lowering it during the call shows in `_adds_other`.
- Each word the interpreter reads from a list counts once. `tiling_token_record`
  counts record words, and `tiling_token_8c`, `tiling_token_8d`,
  `tiling_token_end` (`0xC0000000`) and `tiling_token_link` the other words the
  interpreter handles itself, plus the links the token handler follows itself
  at `0x826AB960` (any word it does not dispatch, except exactly `0xC0000000`,
  continues at that word plus 4). The control words the token handler `0x826AB740`
  dispatches count by type as `tiling_token_80` to `tiling_token_8b` and
  `tiling_token_8e`, and any other control word, including an inexact `0x8C` or
  `0x8D`, as `tiling_token_other`. `tiling_callback_calls` counts the indirect
  calls at `0x826ABD6C` through `W+0x10`, `tiling_callback_distinct` the
  distinct `W+0x10`/`W+0x14` pairs among them (at most 64 are kept) and
  `tiling_token_8c_installs` the distinct function and context pairs the
  `0x8C` tokens carry.
- The four list-selected stores at `0x826AB120` (to `P+8`), `0x826AB4C8`,
  `0x826AB60C` and `0x826AB6FC` count as
  `tiling_write_<b120|b4c8|b60c|b6fc>_<device|ring|other>`. A store is `device`
  inside `G` to `G+0x5E00`, the 24064 bytes device creation `0x8268D2C8`
  allocates, and `ring` inside the allocation passed to the
  last ring initialization, which `tiling_ring_registered` counts. Anything
  else is `other`, and the first eight distinct addresses log with their
  heap allocation.
- `tiling_sink_publisher` counts calls of the publisher `0x8269B900` that reach
  the guest body, and `tiling_sink_541c` and `tiling_sink_5b0c` the ones that
  find `G+0x541C` or `G+0x5B0C` nonzero. The first eight distinct pairs log
  with the sink's vtable pointer.
- `tiling_watchdog_enter` counts entries of `0x826A5B48`,
  `tiling_watchdog_global` its return at the global gate (`0x82F9B0B0`
  nonzero), `tiling_watchdog_reset` the path that zeroes `G+0x2B00` and
  `tiling_watchdog_zero` the path whose reread of `G+0x3478` is zero and ends
  in a trap. The first eight of the last two log `G+0x2B00` and `G+0x3478`.
- `tiling_probe_error` counts guest words the probe could not read.

#### Original residue decision census

The residue probe and its option are part of the developer build only (see
[Development](development.md#developer-build)).
`--native_original_residue_probe=true` (read only at startup, off by default) counts the
existing Publish `0x8269C400` and Submit `0x8269BD98` wrapper decisions.
It works with ring services on or off. The disabled path checks the cached
request flag. Observation adds no guest reads or writes and changes no
dispatch, register, host record, callback or completion order. Optional
diagnostic results describe the existing readiness, classifier and Raise
operations, including failed reads and the actual failed write attempt.

Enabled reports contain `or_scope=wrapper_decision`, `or_publish_calls`,
`or_publish_original_dispatch`, `or_submit_calls`,
`or_submit_original_dispatch`, `or_submit_served_kick`,
`or_submit_served_silent`, `or_internal_error`, `or_closed`,
`or_late_decisions`, `or_reporting_failures`, `or_writers_admitted`,
`or_final_incomplete` and `or_final_skipped`, including valid zeros.
Each admitted decision increments one fixed atomic cell. Totals are derived from the
drained cells. Nonzero cells also produce
`or_<publish|submit>_reason_<reason>` totals and
`or_<publish|submit>_site_<site>_<reason>` counts.

| Function | Terminal reasons |
| --- | --- |
| Both | `inactive`, `not_ready`, `layout_mismatch` |
| Publish | `unreadable`, `starved`, `recording`, `tiling`, `submit` |
| Submit | `publisher_unreadable`, `publisher_sink`, `publisher_callback`, `pending_unreadable`, `pending_nonzero`, `raise_write_failed`, `served_kick`, `served_silent` |

Unreadability takes precedence over readable flags. Publish then prioritizes
Starved, Recording, Tiling and Submit. Submit checks all three publisher
reads before attributing a sink or callback refusal. The CPU flag `0x02`
with a null sink selects Silent even with a readable nonzero callback. Only supported
Kick/Silent paths read N. `pending_nonzero` describes that entry read; it
does not prove the later native locked deferred arm. A failed nonzero Raise
write is attempted once and falls back to Original. Existing host behavior
omits the N store for zero r7 and narrows a served r4 return to low32. This
observer preserves those behaviors without establishing native ABI parity.

Sites compare the full64 entry LR against zero-extended addresses. Publish
recognizes `8269c61c`. Submit recognizes `8269c53c`, `8269c614`, `826ac368`,
`826ac3b4` and `826ac3e8`. All other values, including call PCs and nonzero
high-half aliases, use `other`. Host calls may share a bucket, so these
names do not establish that a native parent body ran. `original_dispatch`
counts the wrapper immediately before its Original call, even if that call
never returns. Direct Original calls, interior entries and other bypasses
are outside this census. An impossible enum increments `or_internal_error`.

Without `--native_ring_consume_publish`, Publish calls equal Publish Original
dispatches and the census field format is unchanged. A developer build with
that control on also emits the consumed counts, plus nonzero
`or_<function>_site_<site>_<reason>_dispatch_<dispatch>` cells. Dispatch names
are `original`, `served_kick`, `served_silent`, `consumed_tiling`,
`consumed_submit`, `consumed_starved` and `consumed_deferred`. Each
reason/site/dispatch cell is drained once; reason totals and actual Original
totals derive from that same cell. Publish calls equal Original dispatches
plus the consumed Publish calls. Submit calls equal Original dispatches plus
served Kick, Silent and consumed deferred calls. Counts are raw calls since the
previous drain, not calls per swap. Before the final drain, shutdown closes
probe admission and waits up to 10 ms for admitted diagnostic writers. Sum the
`Native original residue probe: periodic` reports emitted on timing ticks
and the separate `Native original residue probe: shutdown` report for decisions
admitted through this boundary, marked by `or_closed=1`. Guest stragglers can
still run: later decisions increment `or_late_decisions` instead of reason
cells. That counter is sampled at each drain; it does not predict decisions
after the report. `Native ring stubs: shutdown` separately drains the legacy
counters, including calls after the last timing line and runs with timing
disabled. It is a snapshot, not proof that guest threads stopped. A second
empty final drain duplicates no counts. Failed formatting or logging retains
unreported batches for retry and increments `or_reporting_failures`; each
successfully logged family is cleared independently. Periodic batches are logged
by the probe before clearing, so a failed log retains its batch for retry.
`or_writers_admitted` samples writers before the drain. A closed drain with
remaining writers has `or_final_incomplete=1` and still reports its batch.
Final reporting waits up to 10 ms for the report lock. If it remains busy,
the call returns an incomplete marker, leaves pending batches intact, and
increments `or_final_skipped` and `or_reporting_failures` for the next drain.
A skipped final attempt is not logged through the busy reporter. The next
successful report includes its skipped count and `or_final_incomplete=1`.
Subtract inactive calls when reconciling against legacy counters, which
omit them. Cross-family timing windows are not one atomic ledger, and
legacy calls after close have no corresponding reason cells. For rates,
normalize summed raw calls by swaps from the same admitted interval. No scene
or swap change resets these counters.

The existing `cp_*` completion observations retain their separate ownership
of additions, retirement and balance. The `or_*` fields describe wrapper
reasons and callers; they do not prove coverage of generated bodies, root
ownership, allocation lifetime, callback delivery or completion.

#### Native ring consume (Publish and Submit)

`--native_ring_consume_publish=true` exists only in a developer build
(`REREVVED_DEVELOPER`), is read only at startup and off by default. A shipping
build has no
such control: Publish `0x8269C400` forwards to Original, Submit `0x8269BD98`
keeps its direct-stub or deferred-Original routing from the ring stubs, and
nothing below applies to it. The control applies
only with the native route active: published services for ring stubs, a validated
device, and a published completion ledger. With the control off, the Publish
`0x8269C400` and Submit `0x8269BD98` wrappers, counters and timing fields are
unchanged. With it on and any prerequisite missing, both run Original.

With the control on, a Publish call on the Submit, Starved or Tiling branch
runs on the host: no Original for Publish or Submit, no PM4 bytes, and every
CPU store the original makes. Calls on the recording branch stay on Original
(`rc_recording_original`, R1 reason `recording`): the recording decoder is not
modeled. No Original checker precedes the consume. Every refusal is decided
before the first store and runs Original exactly once. With the control on but
the completion ledger not published, Publish and Submit both run Original.

Publish reads `C=*(G+0x30)`, `S=*(G+0x3A50)` and `R=C+4` once. Branch
priority is unchanged: Starved, Recording, Tiling, Submit.

- Submit branch: a device in CPU fence mode (no sink and `G+0x2ABD` bit
  `0x02`) would complete the fence before the host draws recorded ahead of it
  retire, so the plan refuses it: Publish runs Original and counts
  `rc_publish_original_cpu_fence`. Otherwise the fence emitter `0x8269BCA8`
  runs behind its stub at
  `0x8269C50C` (`F` advances by two, `G+0x33B8` keeps `C`), then Submit runs at
  `0x8269C53C` with `P=return+4`, `A(S)` and `(P-S)>>2`. The tail aligns the
  actual Submit return, which is `P` for a direct arm and `P+0x10` for the
  deferred arm. `r3` is that return, full width.
- Starved: tail only, from `R`; `r3` is the incoming device.
- Tiling: count zero is tail only. A nonzero count records one tiled segment
  with the ledger, then stores `A(S)` at `E+4`, the unmasked `q|0x81000000`
  token at `E` and `E+8` at `G+0x3440`. When `E+8` is past `G+0x3444` the
  retained helper `0x8269C170` supplies the storage at `0x8269C4E4`. `r3` is the
  device plus `0x3438`, or the helper's return after growth.
- Tail: `G+0x3A50` receives the aligned value and `G+0x30` the value minus
  four. When the aligned value is past `G+0x38`, `G+0x30` receives the return
  minus four, the Publish lease is released, and the retained helper
  `0x8269C270` runs at `0x8269C568`; its return propagates.

Submit has three arms. With `G+0x2B00` (N) zero it runs the direct arm. With N
nonzero it takes the `G+0x2B04` spin lock at `0x8269BDD8` and reads N again:
zero runs the direct arm after the unlock, nonzero runs the deferred arm. The
direct arm stores `N=N+r7` (an add of zero still stores), records the direct
submit with the ledger, kicks the existing publisher index on the Kick path,
and returns the token `P` in full width. The Silent path records the direct
submit and then seals it on the host: the ledger takes the thread's pending
record, attaches the records held for its buffer to the open span and closes
that span as sealed. A sealed span retires through the normal host completion
path once the host draws before it complete, so its draws and fence words do
not wait for a later Kick, but it has no write pointer: there is no guest Kick,
no index advance and no RPTR store, and a later direct submit is no unkicked
one. The deferred arm records the
deferred kick, stores `U32(r6)|0x81000000` at `P+4` (not masked to 24 bits),
`A(S)` at `P+8` and `0xC0000000` at `P+0xC`, links the token with the retained
helper `0x8269B550` at `0x8269BE10`, stores `N=N+r7`, releases the lock at
`0x8269BE2C` and returns `P+0x10`. A token store that fails after the lock is
taken ends in the consumed fault `submit_token_store` before the link and the
add: the lock is released and no node becomes reachable. A Submit whose publisher has a CPU-fence
sink or a ring callback stays on Original with R1 reasons `publisher_sink` and
`publisher_callback`.

The consume never advances the publisher index (`G+0x2AC8`) and never writes
RPTR itself. The ledger stores B+4, B+0 and RPTR from the existing index after
host completion. A deferred span, its worker submit and a later main kick
therefore store the same RPTR value repeatedly; the completion-debt monitor
treats that as caught up. The deferral window is bounded by the same debt
threshold.

R1 gains the terminal outcomes `or_publish_consumed_starved` and
`or_submit_consumed_deferred` beside the existing consumed fields. Conservation
holds as `or_publish_calls = original + consumed_submit + consumed_tiling +
consumed_starved` and `or_submit_calls = original + served_kick + served_silent
+ consumed_deferred`, with `or_internal_error=0`. The deferred outcome keeps
the `pending_nonzero` reason; a locked reread of zero reports its direct arm
and no `pending_nonzero`. A Submit refused before the lock reports its own
reason: `raise_write_failed` (N not writable), `lock_storage`,
`token_storage`, `token_wrap` or `list_storage`. These fields and reasons
appear in the R1 line only with the control on; with it off the line is the
old one. The legacy `stub_publish`, `stub_submit` and `stub_fence` counters
keep their meanings: a consumed tiling Publish still counts under
`stub_publish_original` (its classification) without the first-call warning,
which only a refusal or a real Original logs. Zero-Original acceptance reads
`or_publish_original_dispatch` and `or_submit_original_dispatch`, never the
legacy count.

Supplemental fields in the timing line (absent when zero, printed only with the
control on): `rc_fault` (a consumed call failed after its first store; the
ledger also fails with `ring_consume_fault: <step>`),
`rc_publish_original_cpu_fence`,
`rc_recording_original`, `rc_invariant_<cursor_order|cursor_extent|start_extent|
fence_parity|pending_unreadable|list_capacity>`,
`rc_wait_<fence|ringspace|drain>_{calls,total_us,max_us,active}`,
`rc_watchdog_fence`, `rc_watchdog_drain` and `rc_final_incomplete`. The same
fields appear in the `Native ring consume: shutdown` log line.

`rc_publish_original_cpu_fence` counts Publish calls left on Original because
the fence emitter would run in CPU mode. That mode stores the fence before the
host draws recorded ahead of it complete; the consume does not redesign
retirement and refuses the mode before any store.

The entry invariants are counts only. `cursor_extent` and `start_extent` flag a
cursor or start that is unmapped, lies past the hard limit `G+0x34` (a zero
limit is none), or lies outside the heap allocation that holds the limit's
last dword, which is the command buffer's allocation, and `list_capacity` flags
`E+8` past `G+0x3444` when the growth
helper does not run. If the services are withdrawn or disabled with a wait or
consume call still open, the final report keeps `rc_final_incomplete`.

The fence wait (`0x8269BE70`), ring-space wait (`0x8269B1F0`) and the pending-N
drain of `0x8269C810` are measured by wait scopes in the route wrappers. The
vblank service reports a fence or drain scope open past the debt threshold
once, from its own loop, with the device, thread, caller, requested fence, N,
cursor, write pointer, oldest span and host fence state. The watchdog does not
depend on any consume call being active.

Retained as guest code or stubs: the fence emitter, the allocator, the list
helpers `0x8269B550`, `0x8269C170` and `0x8269C270`, and the replay worker.
Acceptance of the control needs positive consumed coverage, zero Original
dispatches for Publish and Submit on the supported route, zero `rc_fault`,
invariant and watchdog fields, both R1
conservation equations and no completion-debt report. Boot refusals and any
fallback after the route is ready are reported, not hidden.

#### Video scaler import stub

`--native_video_scaler_stub=true` and `--native_video_scaler_exit_check=true`
exist only in a developer build (`REREVVED_DEVELOPER`) and are read only at
startup and off by default. The generated call sites stay in every build; a
shipping build
binds passthrough bridges that replace and observe nothing, so the import
always runs. With both off a developer build's hooks return
after the cached flag test: no guest access, allocation, count, log or report,
and no `video_scaler_*` field on the `Native frame timing:` line.

The stub covers one call: the display owner `sub_826B02B0` calls the
`VdInitializeScalerCommandBuffer` import at `0x826B0430`. The hook sits after
the `mr r3,r25` at `0x826B042C`; when it replaces the call it jumps to
`0x826B0434` and nothing else is skipped. It does what the SDK export does for
the one call shape the owner makes: it stores 200 big-endian `0x80000000`
dwords at `SP+192` (800 bytes, ending exactly at the coefficient pointer
`SP+992`) and returns 200, a dword count, in the full 64-bit `r3`. It is not a
model of the hardware scaler and does not remove any guest packet bytes. The
owner's own `0x8269C7A8(G, 220)` reservation `R`, the 800-byte copy to
`[R+4, R+804)` and the cursor store `G+0x30 = R+800` keep running as guest
code, in that order; the ring header at `R` and the word at `R+804` are never
written. No completion, read-pointer or callback state is touched.

The skipped `bl` leaves LR at its value before the call instead of
`0x826B0434`. Nothing reads LR until the retained memcpy call sets it to
`0x826B0448`, and the owner restores its saved LR in its epilogue, so the
register state reconverges before the owner returns. The hooks cannot write LR
(generated hooks receive it by value), which is why the interval exists.

The stub needs published ring stub services (`native_ring_stubs`) and the
validated device `G` in `r31`; it holds a publication lease from its decision
through its last store, so a withdrawal waits for it. The call is the owner's
shape only when the stack pointer is 16-byte aligned and does not wrap, the
stack arguments (the low words of the 8-byte slots at `SP+84`, `+92`, `+100`
and `+108`) are `SP+172`, `SP+992`, `SP+192` and 200, and `r9` is `SP+156`.
Every refusal runs the SDK import exactly once with the original inputs and
before any stub store, in this order, each counted as
`video_scaler_refusal_<reason>`: `services`, `diagnostics` (the exit check is
on), `stack`, `shape`, `unreadable` (a sentinel word cannot be read),
`unwritable` (the 800 output bytes are not one contiguous writable range),
`reservation` (null, misaligned or wrapping `R`, an unusable copy destination
or `G+0x30`, or an unreadable header or tail word), `alias` (two checked
ranges share physical or virtual backing, or a backing is unknown),
`reentrant` (a call is nested below a live call on the thread) and `unpaired`
(no observation record can be made). A store that fails after the output
started cannot fall back to the import over partial output: it increments
`video_scaler_write_failed`, logs, and stops the process.

`native_video_scaler_exit_check` forces the import to run and observes it,
alone or together with the stub. Both options pair three observations per call:
the call (which takes a baseline of the words the owner leaves alone), the
return before the `rlwinm` at `0x826B0434`, and the end of the cursor store
at `0x826B044C`. A paired, readable call whose `r3` is 200, 200 output words,
800 copied bytes and cursor equal what the SDK contract defines, and whose
sentinel words (`SP+188`, `SP+992`, the ring header and the word after the
copy) are unchanged, counts `video_scaler_exit_ok`. A difference counts
`video_scaler_exit_mismatch_<output|return|copy|cursor|sentinel>`; an
unreadable word counts `video_scaler_exit_unreadable` and an incomplete,
nested or stale pairing counts `video_scaler_exit_unpaired`, never ok. Check
lines are logged as `Native video scaler check:` with the thread, SP, G, R and
verdicts (the first sixteen ok lines only).

The `Native frame timing:` line carries `video_scaler_enter`, `_consumed` and
`_original` since the previous drain, with the refusal, `write_failed` and exit
fields above and `video_scaler_pending`, the started calls whose observations
have not finished. Pairing records are per guest thread and survive each drain.
The shutdown line `Native video scaler: shutdown` drains the remaining counts,
waits at most 10 ms for a concurrent drain, and then reports
`video_scaler_final_incomplete=1 video_scaler_final_skipped=1` instead of
blocking.

For an owner census, run fresh processes with the same executable, SDK,
module and provider, the same other options, content and player data, and the
same supported sequence: boot, menu, Hall of Glory, world and transitions.
Record the executable, SDK, module and provider receipt of each process, and
the qualified `cp_*` results for the stable observed intervals.

1. Baseline: `native_video_scaler_stub=false` and
   `native_video_scaler_exit_check=true`, with timing and the completion probe
   enabled. Preserve the complete boot. Require positive natural coverage of
   the call with matched return and copy records, `r3` 200, correct output and
   copy bytes, `G+0x30 = R+800`, and no mismatch, unreadable, unpaired or
   count of write failures. If the route is not reached, the run supplies no
   scaler admission; do not manufacture a call.
2. Candidate: `native_video_scaler_stub=true` with the exit check off and the
   same sequence. Require `video_scaler_consumed` above zero,
   `video_scaler_original` and every refusal zero for the owner's shape,
   matched observations, the same output, copy and cursor bytes, and the same
   visual behavior. A refusal narrows or rejects the admission and is not a
   success. A scene that never makes the call needs no invented per-frame
   count.
3. Reject either run, and the admission, on any completion mismatch, newly
   dropped bracket, unexplained reset or completion debt, stale callback,
   crash, or close regression. The `cp_*` results do not substitute for
   scaler parity. Reuse the existing deterministic held and failed host work
   tests; a fault-injected owner run is not part of this census.

A passing census admits only this SDK-compatible call shape on the tested
native route. It does not admit removing the display owner, its NOP copy,
another video provider or xenos packaging, and the options stay off by default
until the result is accepted. Neither run is a timing benchmark.

#### Video display mode import stub

`--native_video_display_mode_stub=true` and
`--native_video_display_mode_exit_check=true` exist only in a developer build
(`REREVVED_DEVELOPER`) and are read only at startup and off by default. The generated call
sites stay in every build; a shipping build binds passthrough bridges that
replace and observe nothing, so the import always runs. With both off a
developer build's hooks return after the cached flag test: no
guest access, allocation, count, log or report, and no
`video_display_mode_*` field on the `Native frame timing:` line.

The stub covers one call: the display owner `sub_826A61F0` calls the
`VdSetDisplayMode` import at `0x826A6260` with the packed flags in `r3`. The
hook sits before that `bl`; when it replaces the call it jumps to `0x826A6264`
and nothing else is skipped. It does what the SDK export does: it writes no
guest byte and no register except `r3`, and returns 0 in the full 64-bit `r3`.
It is an SDK-compatible import cut, not a hardware mode switch, and it does not
remove any guest code besides that call. The owner's `addi r3,r1,80`, its
retained `VdGetCurrentDisplayInformation` import at `0x826A6268` and the three
device stores `G+0x5428`, `G+0x542C` and `G+0x5430` (the display width, height
and actual width read from the 88-byte information at `SP+80`) keep running as
guest code. The owner's bypass (`r5` zero) never reaches the call, is not
selected, and does no stub or observation work.

The skipped `bl` leaves LR at its value before the call instead of
`0x826A6264`, only over the interval `[0x826A6264, 0x826A6268)`. Its only guest
instruction is `addi r3,r1,80`, which reads `r1`, and the retained `bl` at
`0x826A6268` sets LR to `0x826A626C` before anything else runs. The hooks cannot
write LR (generated hooks receive it by value), which is why the interval
exists. The return observation at `0x826A6264` therefore takes no LR; LR is read
only at the call (`0x826A6260`) and at the reconvergence (`0x826A626C`), and a
contract test rejects any LR use between `loc_826A6264` and the next `bl`.

The stub needs published ring stub services (`native_ring_stubs`) and the
validated device `G` in `r31`. Every active entry, the exit check alone
included, takes the publication lease, reads the Runtime, detached delegate,
ring services and dispatcher target identities, and records `provider_valid`
from the same checks; a failed lease means `provider_valid=0`. The lease is held
through the `r3` assignment and released before the SDK import runs. The call
is the owner's shape only when the stack pointer is 16-byte aligned, does not
wrap and is at least 0x10000, `[SP]` is `SP+192`, `r3` has no high word, bit 30
is set and the other bits are within `0x38000002`. Every refusal runs the SDK
import exactly once with the original inputs and before any mutation, in this
order, each counted as `video_display_mode_refusal_<reason>`: `services` (no
lease, device not ready, no Runtime, a graphics system or no detached
delegate), `provider` (the function table entry for `0x82E924F4` is missing or
is not the retained `VdSetDisplayMode` wrapper), `diagnostics` (the exit check
is on), `stack`, `shape`, `reentrant` (a call is nested below a live call on the
thread) and `unpaired` (no observation record can be made). Only the first
reason is counted.

`native_video_display_mode_exit_check` forces the import to run and observes it,
alone or together with the stub. Both options pair three observations per call:
the call (`0x826A6260`), the return before the `addi` (`0x826A6264`) and the
reconvergence before the first `lhz` (`0x826A626C`, after the retained
information import). A paired, readable call with a valid provider and
unchanged identities, whose `r3` is 0 at `0x826A6264`, is `SP+80` at
`0x826A626C` and whose LR is `0x826A626C` there, counts
`video_display_mode_exit_ok`. A difference counts
`video_display_mode_exit_mismatch_<return|continuation|lr>`; an unreadable
information word counts `video_display_mode_exit_unreadable`; an incomplete,
nested or stale pairing, or an identity change before `0x826A626C`, counts
`video_display_mode_exit_unpaired`, never ok. A missing lease or provider
cannot be ok either. Check lines are logged as `Native video display mode
check:` with the thread, SP, G, flags, entry LR, results, identities, the 88
information bytes in hex, width, height and actual width, and verdicts (the
first sixteen ok lines only). The record is exact only for the supported frame
shape; one record exists per guest thread and at most 64 are outstanding.

The `Native frame timing:` line carries `video_display_mode_enter`, `_consumed`
and `_original` since the previous drain, with the refusal, exit and
`video_display_mode_reentry` fields above and `video_display_mode_pending`, the
started calls whose observations have not finished. Pairing records are per
guest thread and survive each drain. The shutdown and hard-exit line `Native
video display mode: shutdown` drains the remaining counts, waits at most 10 ms
for a concurrent drain, and then reports
`video_display_mode_final_incomplete=1 video_display_mode_final_skipped=1`
instead of blocking.

For an owner census, run fresh processes with the same executable, SDK,
module and provider, the same other options, content and player data, and the
same supported sequence: boot, menu, Hall of Glory, world and transitions.
Record the executable, SDK, module and provider receipt of each process, and
the qualified `cp_*` results for the stable observed intervals. Keep the video
scaler options off in both runs, and keep ring stubs, timing and the completion
probe on.

1. Baseline: `native_video_display_mode_stub=false` and
   `native_video_display_mode_exit_check=true`. Preserve the complete boot.
   Require positive natural coverage of the call with matched return and
   reconvergence records, `r3` 0 at `0x826A6264`, `r3 = SP+80` and LR
   `0x826A626C` at `0x826A626C`, and a valid provider. Over the whole run
   `video_display_mode_original` equals `video_display_mode_enter` and every
   refusal other than `diagnostics` is zero. If the route is not reached, the
   run supplies no display mode admission; do not manufacture a call.
2. Candidate: `native_video_display_mode_stub=true` with the exit check off and
   the same sequence. Require `video_display_mode_consumed` above zero,
   `video_display_mode_original` and every refusal zero for the owner's shape,
   `video_display_mode_enter` equal to `video_display_mode_consumed` and to
   `video_display_mode_exit_ok`, the same display information bytes, device
   stores and visual behavior as the baseline. A refusal narrows or rejects
   the admission and is not a success.
3. Acceptance gates for both runs. Each timing line reports only the counts
   since the previous drain, and the shutdown line only the residual since the
   last timing line, so a total is the sum of every timing line plus the
   shutdown line; the shutdown line alone is not the run's total, and
   `video_display_mode_pending` is a gauge, not a sum. End each run with a
   normal quit, so that the shutdown line is written; require zero
   `video_display_mode_exit_mismatch_*`, `video_display_mode_exit_unpaired`,
   `video_display_mode_reentry` and `video_display_mode_exit_unreadable`
   totals, a final `video_display_mode_pending` of zero, and
   `video_display_mode_final_incomplete=0` and
   `video_display_mode_final_skipped=0` on the final line. A run without its
   final line, or one ended by a crash, is rejected.
4. Reject either run, and the admission, on any completion mismatch, newly
   dropped bracket, unexplained reset or completion debt, stale callback,
   crash, or close regression. The `cp_*` results do not substitute for
   display mode parity. Reuse the existing deterministic held and failed host
   work tests; a fault-injected owner run is not part of this census.

A passing census admits only this SDK-compatible import cut on the tested
native route. It does not admit hardware mode switching, a frame rate result,
completion of any native renderer work, another video provider or xenos
packaging, and the options stay off by default until the result is accepted.
Neither run is a timing benchmark.

#### Video constant import stubs

One table with two rows covers two more constant SDK imports at the end of
device initialization. Its four options are read only at startup, exist only in
a developer build (`REREVVED_DEVELOPER`), and default to off. The generated call
sites stay in every build; a shipping build binds passthrough bridges that
replace and observe nothing, so both imports always run:

| Import | Owner | Call | Result | Options |
|---|---|---|---|---|
| `VdIsHSIOTrainingSucceeded` (`0x82E92524`) | `sub_826A63E8` | `0x826A65A4`, continuation `0x826A65A8` | full `r3` = 1 | `--native_video_hsio_stub`, `--native_video_hsio_exit_check` |
| `VdRetrainEDRAMWorker` (`0x82E92634`) | `sub_826B0508` | `0x826B053C`, continuation `0x826B0540` | full `r3` = 0 | `--native_video_edram_worker_stub`, `--native_video_edram_worker_exit_check` |

With an import's two options off, a developer build's hooks return after the
cached flag test:
no guest access, allocation, count, log or report, and no field of that import
on the `Native frame timing:` line. The two imports have separate options,
pairing records, outstanding limits and counters, so one import's option, budget
or drain never changes the other's. The table holds exactly these two rows; a
call site that is not one of them, or a row that disagrees with its own lookup,
is a `pc` refusal before any lease, probe or guest read.

A replaced call writes no guest byte and no register except `r3`, and assigns
the whole 64-bit `r3` the constant the SDK export returns (HSIO 1, worker 0),
whatever the incoming high half held. It is an SDK-compatible import cut. It
does not train hardware, replace the exports for any other caller, remove any
plugin or PM4 work, or change frame rate.

Everything the owner does around the call keeps running as guest code. In the
HSIO owner these are the cache load at `0x82F9B0A4` (the call is taken only when
the loaded word is zero, with `r30 = 0xFFFFFFFF82FA0000` and `r11 = 0`), the
`cntlzw`/`rlwinm.` compare, the owner's own store of the result into the cache
word, its CR0 branch and diagnostics, and the retained child call. The skipped
call leaves the cache word and CR0 exactly as the SDK run does, and the stub
itself writes no cache byte. In the worker owner they are the critical region
and section, the two stack-word stores at `SP+80` and `SP+84`, the retrain
arguments, the retained `VdRetrainEDRAM` call and any retry, and the unlock. The
owner's bypass (a zero low word of its saved `r5`, kept in `r29`, which is the
present path) never reaches the call; the hooks there count nothing and read
nothing.

Each skipped `bl` leaves LR at its value before the call over an interval:
`[0x826A65A8, 0x826A65CC)` for HSIO and `[0x826B0540, 0x826B0568)` for the
worker. The retained `bl` at `0x826A65C8` (HSIO) and `0x826B0564` (worker) sets
LR again. The hooks cannot write LR (generated hooks receive it by value), so
the return and effect observers of both imports and the worker's pre-retrain
observer take no LR; LR is read only at the call and after the retained bl (the
post observers). Contract tests list every statement of both generated
intervals and reject any LR use, hook or call that is not listed.

The stub needs published ring stub services and the validated device `G` in
`r31`. Every active entry, the exit check alone included, takes the publication
lease and reads the Runtime, detached delegate, ring services and dispatcher
target identities. The lease is held through the `r3` assignment and released
before the SDK import runs; a withdrawal waits for it. A call is the owner's
shape when the stack pointer is 16-byte aligned, does not wrap and is at least
0x10000, the whole owner frame (128 bytes for HSIO, 2192 for the worker) is
readable, the cache word is readable (HSIO) and the row's register predicate
holds. Every refusal runs the SDK import exactly once with the original inputs
and before any mutation, in this order, each counted as
`video_<import>_refusal_<reason>`: `pc`, `services` (no lease, device not
ready, no Runtime, a graphics system or no detached delegate or ring service),
`provider` (the function table entry for the import address is missing or is not
the row's retained wrapper), `diagnostics` (the exit check is on), `stack`,
`shape`, `unreadable`, `reentrant` (a call is nested below a live call on the
thread) and `unpaired` (no observation record can be made). Only the first
reason is counted.

`ready_at_entry` records whether the row's readiness held at entry (the ring
stubs have validated the device in `r31`). It is separate from the observation:
`video_<import>_enter` counts every active entry and
`video_<import>_ready_at_entry` the entries that were ready. A call that was
not ready can still be paired and counted `exit_ok`; it is then not admitted
for the stub, and the census reports that row as unadmitted by readiness. These
two entries can precede the first `native_ring_stubs: device ... layout checked`
log line, so readiness at entry is not predicted. How often either call is
reached is not predicted either: the HSIO call only follows earlier successful
initialization and a zero cache word, and the worker call only on activations
whose saved `r5` has a nonzero low word. A census that never reaches a row
supplies no admission for it; do not manufacture a call, edit the cache or force
`r5`.

The exit check forces the import to run and observes it, alone or together with
the stub. Four observations pair each call: the entry, the result at the
continuation, the preserved effect (HSIO: before the retained child call;
worker: before the retained retrain), and the observation after the retained
child or retrain. The record keeps only weak references to the delegate and ring
service and the integer identities, compares them with the current ones at every
later boundary, and never keeps either object alive. Each boundary also rechecks
that the ring stub services are still published and, for a call that was ready
at entry, that the validated device in `r31` is still the published one. A call
with a changed, expired or rebound identity, a withdrawn or disabled
publication, a lost readiness, a lost, duplicated or reordered boundary, a
nested call, or no valid provider at entry counts `video_<import>_exit_unpaired`,
and an unreadable word counts `_exit_unreadable`; neither is ever `_exit_ok`.
The worker's stack words are read before and after the retained retrain, and a
call whose post-retrain read fails is `_exit_unreadable`. A worker post
observer whose `r29` says a call was expected but finds no record (a duplicate
post after a completed call, or a lost record) counts `_correlation_lost` and
`_exit_unpaired`; the HSIO post hook is reached with no call on its cached path
and stays silent. A paired, readable call with a valid provider, unchanged
identities and unchanged services counts `_exit_ok`; a difference counts
`_exit_mismatch_<return|effect|continuation|lr>`
(the result; the HSIO cache word, `r11` and CR0, or the worker stack words; the
retained registers and retrain arguments; the worker's LR after the retained
retrain). Check lines are logged as `Native video HSIO check:` and `Native video
EDRAM worker check:` (the first sixteen ok lines per import, every other line
always). One record exists per guest thread and import, and at most 64 are
outstanding per import.

The `Native frame timing:` line carries, per requested import, `_enter`,
`_ready_at_entry`, `_consumed` and `_original` since the previous drain, the
refusal and exit fields above, `_reentry`, `_abandoned`, `_correlation_lost`,
`_reporting_failed` and `_pending` (the started calls whose observations have
not finished, a gauge). Pairing records are per guest thread and survive each
drain. The shutdown and hard-exit lines `Native video HSIO: shutdown` and
`Native video EDRAM worker: shutdown` drain the remaining counts, wait at most
10 ms for a concurrent drain, and then report `_final_incomplete=1
_final_skipped=1` instead of blocking. A final is incomplete while a call is
outstanding, a drain was skipped or a report failed, and an earlier incomplete
observation keeps every later final incomplete. A mismatch, like an unpaired or
unreadable call, is such an observation, and the flag survives clean calls and
timing drains.

Each timing line reports only the counts since the previous drain and the
shutdown line only the residual since the last timing line, so a total is the
sum of every timing line plus the shutdown line; the shutdown line alone is not
the run's total, and `_pending` is a gauge, not a sum.

For an owner census, run fresh processes with the same executable, SDK, module
and provider, the same other options, content and player data, and the same
supported sequence: boot, menu, Hall of Glory if reachable, load or start,
2-3 minutes of play, transitions and a normal quit. Back up player saves and
settings first and run no build meanwhile. Keep the video scaler and display
mode options off in both runs, and keep ring stubs, timing and the completion
probe on. Record the executable, SDK, module and provider receipt of each
process.

The completion probe exists only in a developer build, and `rexglue.ps1`
launches the player executable. Build the developer configuration (see
[Development](development.md#developer-build)) and start
`out/build/win-amd64-developer/rerevved.exe` from the repository root in both
arms, with `--gpu_plugin=xenos`, `--render_target_path_d3d12=rov`,
`--game_data_root`, `--user_data_root`, `--cache_root`, `--log_file` and
`--log_level=debug` set as the script sets them, followed by the arguments
shown below.

1. Baseline: both exit checks on and both stubs off.

   ```powershell
   cd <repo>; .\out\build\win-amd64-developer\rerevved.exe <common options> --renderer=native --native_frame_timing=true --native_ring_stubs=true --native_completion_probe=true --native_video_hsio_stub=false --native_video_hsio_exit_check=true --native_video_edram_worker_stub=false --native_video_edram_worker_exit_check=true
   ```

   Judge each import separately. Require positive natural coverage with
   `ready_at_entry` equal to `enter`, `original` equal to `enter`, every refusal
   other than `diagnostics` zero, matched records from four boundaries and a valid
   provider. A row whose baseline `ready_at_entry` is zero is unadmitted by
   readiness, not defective: do not enable its candidate and do not relax the
   readiness test. Positive HSIO coverage does not imply worker coverage or the
   other way round.
2. Candidate: the stub on and the exit check off, for each row that passed the
   baseline (both together when both qualify), with the same sequence and
   options.

   ```powershell
   cd <repo>; .\out\build\win-amd64-developer\rerevved.exe <common options> --renderer=native --native_frame_timing=true --native_ring_stubs=true --native_completion_probe=true --native_video_hsio_stub=true --native_video_hsio_exit_check=false --native_video_edram_worker_stub=true --native_video_edram_worker_exit_check=false
   ```

   Use `false` for an unadmitted row's stub. Require `consumed` above zero,
   `original` and every refusal zero for the owner's shape, `enter` equal to
   `consumed` and to `exit_ok`, and the same results, preserved effects
   (HSIO scalar, cache word, CR0 and retained child outcome; worker arguments,
   stack words, post-retrain LR and result), synchronization behavior and
   visuals as the baseline. Match records by row, activation shape and settings
   within each process, not by raw address or invocation number. A refusal
   narrows or rejects the admission and is not a success.
3. Acceptance gates for both runs. End each run with a normal quit so that the
   shutdown line is written; require zero `_exit_mismatch_*`, `_exit_unpaired`,
   `_reentry`, `_abandoned`, `_correlation_lost`, `_reporting_failed` and
   `_exit_unreadable` totals, a final `_pending` of zero, and
   `_final_incomplete=0` and `_final_skipped=0` on the final line, summing every
   timing line plus the shutdown line. A run without its final line, or one
   ended by a crash, is rejected.
4. Reject either run, and the admission, on any completion mismatch, newly
   dropped bracket, unexplained reset or completion debt, stale callback,
   crash, or close regression. The `cp_*` results corroborate and do not
   substitute for the per-import checks above. Zero worker counts during world
   presents are expected.

A passing census admits only these SDK-compatible import cuts on the tested
native route. It does not admit hardware training, replacement of the exports
for other callers, plugin or PM4 removal, a frame rate result or completion of
any native renderer work, and the options stay off by default until the result
is accepted. Neither run is a timing benchmark.

#### Completion accounting observation

The completion probe and its option are part of the developer build only (see
[Development](development.md#developer-build)).
`--native_completion_probe=true` (read only at startup, off by default) observes
completion
without consuming a function, changing a dispatch decision or writing guest
memory. It is independent of `native_ring_stubs` and the tiling probe. Its
disabled entry points return after the cached flag check; wrapper exits use the
entry's cached decision. Every guest word is a checked, big-endian guarded
copy. Null B/S2 chains are absent, while failed copies have invalid bits and
increment `cp_unreadable`; invalid words must not be interpreted as zero.
This observer grants no consume authority and proves no host/GPU retirement.
The [video scaler import stub](#video-scaler-import-stub) is a separate
contract; its counters are not completion counters.

The `Native frame timing:` line includes these counters since its previous
drain. Detailed `Native completion probe:` records are formatted at the drain,
outside guest spin locks. A shutdown line drains remaining counters and gives
the current `cp_unmatched`, including allocator pairs and store observations.
Totals, epochs, active calls and allocation identities survive each drain.
Initialization and callbacks are observed even before ring services publish.

- `cp_c810_enter`, `cp_c810_exit` and `cp_c810_exit_n_nonzero` count the drain
  wrapper on either selected path. `cp_init_success` and `cp_init_failure`
  classify the ring-configuration wrapper `0x8269C8E0` result (zero succeeds).
  `cp_dropped` counts nonzero EndTiling results. Entry/exit records preserve N,
  P, C, root/pending pointers and all six queue producer/consumer pairs.
- `cp_add_<deferred|direct|stub>` sums committed BD98 additions;
  `cp_add_<path>_calls` includes zero additions. Original store pairs bracket
  `0x8269BE1C` and `0x8269BE4C`; a branch that bypasses a store counts nothing.
  The host path counts only successful `RaiseForDirectSubmit` calls. `cp_d`
  counts only the final retirement store at `0x826ABB34`. Watchdog zeroing is
  separate (`cp_watchdog_reset`) and invalidates accounting intervals.
- `cp_balance_ok`, `cp_balance_bad` and `cp_balance_indeterminate` classify
  unsigned `N_end - N_start - additions + D` for AC218/C810 intervals and
  consecutive stable C810 exits (`kind=carry`). Balances retain carried work
  across reporting windows. Overlapping accounting intervals, unreadable or
  busy endpoints, an epoch change, a store crossing a snapshot or lost events
  make the result indeterminate. Ordinary nested BD98 calls remain accounting
  inputs. A balanced N does not prove root identity or exactly-once delivery.
- `cp_p_odd`, `cp_p_even`, `cp_p_zero`, `cp_mask_nonzero`, `cp_mask_change`
  and `cp_s2_change` describe sampled values. The record PC distinguishes
  construction (`0x8268D2AC`), publication (`0x826A645C`), configuration,
  brackets, drains and callback boundaries. Configuration entry advances the
  device epoch. `cp_slot_<347c|409c|40a0|s2_10|s2_14>_<samples|nonzero|change>`
  reports each sampled slot. First values and subsequent samples are retained
  with preceding/current call IDs, `writer_pc=unknown` and
  `cp_slot_coverage=sampled`. An unchanged sample cannot exclude transient or
  same-value writes. Callback consumer PCs do not identify installers.
- `cp_callbacks` counts observed actual callback boundaries. Callback records
  retain targets and arguments at `0x8269B4A4`, `0x826AC020`, `0x8268CB88`,
  `0x826A3FA0` and `0x826A40B4`; BF40 entry records retain packet requests.
  The AC020 record includes the CPU selector; its queue is
  `G+0x2C3C+80*selector`. Optional present callback payload reads have their own
  validity mask, and the 347C record retains the raw f1 bits and reason.
- `cp_queue_nonzero` counts nonzero sampled G+34D0/+34D4/+34D8 fields;
  `cp_queue_segment_change` counts changes at +34D0 and
  `cp_queue_cursor_change` changes at +34D4/+34D8. These are activity samples,
  not decoded records, Release or free events.
- `cp_extent_hit` counts allocator returns whose physical aliases lie in
  `[0x1EBB0000,0x1F1B0000)`. Both nested manager and outer B818 returns count.
  `cp_extent_owner_joined` counts returns equal to the queried region base
  with both region size and adjusted request equal to 0x600000. A small
  suballocation proves only a route hit. The first chain per device/epoch/region
  logs original dwords, modulo-32-bit byte size, adjusted bytes, alignment,
  caller, manager+A4/+A8/+AC, actual target/context, result, physical alias and
  region. An observed whole-region return also logs its chain. The identity
  does not establish free/reuse or outside readers.
- `cp_unreadable`, `cp_event_drop`, `cp_pair_error` and `cp_epoch_ambiguous`
  qualify observation quality. Storage holds 128 records per drain, 64 active
  wrapper calls, 64 allocator entries and eight device identities. Overflow
  increments `cp_event_drop`, invalidates affected accounting and preserves
  aggregate event counters. Allocator pairing stops after its storage
  overflows, retaining unmatched entries instead of pairing to the wrong call.

Record `words` indices 0..28 follow the offsets in
`kCompletionOffsets` (`completion_probe.h`); 29 is C=[B], and 30/31 are
S2+10/+14. `valid` marks readable words; `payload_valid` qualifies payload
fields, including unreadable configuration and allocator inputs. Balance records
use payload 0..5 for
start N, end N, additions, D, residual and preceding boundary ID; payload
10/11 retain selected-stub/result. `balance=0/1/2` means ok/bad/indeterminate
only on AC218/C810 exit and carry records. Add/retire payloads hold amount,
observed N, N validity and path (deferred/direct/stub = 0/1/2). Original N
store records name the exact writer PC, including same-value stores; host
additions name `host_RaiseForDirectSubmit` and their N reread is sampled.
Allocation payloads hold caller, original request, adjusted bytes, alignment,
result, physical alias, region base/size, manager, actual target/context and
owner-join flag. Their `words` 0..7 instead hold manager+A4/+A8/+AC, original
request bytes, flags, adjusted-request validity, request pointer and manager's
original byte request. For callbacks, payload 0/1 is target/argument, 2..7
contains guarded payload words and payload 8 is their validity mask; 347C uses
payload 1 for reason and 2/3 for the high/low f1 bits.

The controls `--native_consume_tiling_restore` and
`--native_tiling_restore_exit_check` are read only at startup and exist only in
a developer build
(`REREVVED_DEVELOPER`) and both default to false. They cover the raw wrapper
of `sub_8269B118`, the saved publication restore. A shipping build has no such
controls: the wrapper forwards to the Original exactly once. With both off in
a developer build, the wrapper forwards without guest reads, counters,
allocations or locks.
Consumption holds a ring service publication lease and rechecks the same
validated device. It reads the six saved words at `G+0x3460..0x3474`, in
the instruction order s1, s0, s2, s3, s4, s5, before any store. It writes
exactly seven big-endian words, in order:
`30=s0,34=s1,3A50=s2,3A4C=s3,3A44=s4,3A48=s5,38=U32(s1-160)`.
The offsets are hexadecimal; 160 is decimal. This is the entire memory
footprint of guest PCs `0x8269B118..0x8269B150`.

The restore reproduces r5=s5, r6=s4, r7=s3, r9=s2, r10=s0 and r11=s1
with zero extension, and r8=`U64(zeroExtend64(s1)-160)`. For s1=0,
r8 is `0xFFFFFFFFFFFFFF60`. Full incoming r3 and every other semantic
context field, including CR/XER, LR/CTR, floating/vector and reservation
state, remain intact. The surrounding EndTiling retains all three allocation
attempts, active clears, the restore call, the subsequent failure-bit clear
and its full r3=1 return. This leaf changes no completion accounting,
callbacks, queues or dirty state.

Requested consumes fall back before mutation for `stubs` (no held
publication lease), `device` (a different or unvalidated device),
`diagnostics` (harvest, scene capture or route trace needs the
body), `exit_check` (checker priority), `unreadable`, `unwritable`,
`mapping` (invalid, unaligned, wrapping or unresolved canonical memory)
or `alias` (intersecting canonical operands). Both physical and ordinary
virtual RAM identities are supported. Every saved word and all seven
destinations are checked before replay. An unexpected commit failure is a
fatal `tr_replay_fault`; Original cannot follow a partial replay.

The exit checker forces Original even when consumption is requested and
can observe without published ring stubs. It snapshots saved words and the
semantic context, then compares all seven output words and every semantic
register field after Original. It writes no guest state and executes no
shadow guest code. Unreadable input/output, unsupported aliases, changed
saved bytes or observed overlapping restore writers qualify the comparison.
An undetected intervening writer of the same value remains possible:
successful checks establish exercised endpoint equivalence under the
retained ownership contract, not concurrency exclusion.

When either control is on, timing drains and shutdown report `tr_enter`,
`tr_consumed`, `tr_original`, all eight `tr_fallback_<reason>` fields,
`tr_exit_checked`, `tr_exit_mismatch`, `tr_exit_abi_mismatch`,
`tr_exit_unreadable`, `tr_exit_indeterminate`, `tr_exit_unmatched`,
`tr_event_drop` and `tr_replay_fault`. Checked means a stable, readable
match; mismatches are never counted as checked. Original counts the actual
raw-wrapper dispatch, with `tr_enter=tr_consumed+tr_original` for closed
calls. Checker-only calls invent no consume refusals. Counts are intervals;
shutdown retains lifetime totals and reporting never resets active scope
pairing. Bounded mismatch records include device, caller, call/sequence
identity, saved values and validity, and the first differing word or register.
Lost records are explicit. This dispatch census does not cover arbitrary
generated-body or interior bypasses.

Runtime admission requires three fresh owner launches of the same candidate
with identical assets, scenes and inherited consume settings:

| Arm | Additional controls |
|---|---|
| Baseline | `--native_consume_tiling_restore=false --native_tiling_restore_exit_check=false` |
| Check | `--native_consume_tiling_restore=false --native_tiling_restore_exit_check=true` |
| Consume | `--native_consume_tiling_restore=true --native_tiling_restore_exit_check=false` |

Use `--renderer=native --native_frame_timing=true --native_census=false
--native_draw_skip=false --native_ring_stubs=true --native_completion_probe=true
--native_tiling_quiesce_check=true --native_consume_dirty_vertex_fetch=false
--native_consume_set_predication_preswap=false` in every arm. Record all other
dirty, tiled, lowerer and draw settings. Cover boot, menus, hall of glory,
world, transitions and normal close, including normal per-run verdicts.
An optional checker arm with ring stubs off still observes Original.

A clean census needs positive naturally reached `tr_exit_checked`, positive
`tr_consumed` for the same supported class, reconciled dispatch totals through
shutdown, and zero exit/ABI mismatches, unreadable, indeterminate, unmatched,
drop and replay-fault counts. Positive `cp_dropped` with matched AC218 records
establishes dropped-bracket coverage separately; it does not establish the
comparison of seven stores. Independent fixtures cover each allocation failure
and preexisting failure; do not force allocation failure in the game.
If B118 is absent, report runtime coverage absent. Offline validation can
support including the change with the option off by default. If no calls
exercise it, there is no basis for enabling consumption, either explicitly or by
default.

Use completion intervals with no losses and a known epoch with `cp_unreadable`,
`cp_event_drop`, `cp_pair_error`, `cp_epoch_ambiguous` and
`cp_watchdog_reset` zero. Require shutdown `cp_unmatched=0`,
`cp_balance_bad=0`, classified indeterminate intervals, positive completed
C810 coverage and `cp_c810_exit_n_nonzero=0` for the broader completion
census. Restore admission does not close unexercised N, callback or allocator
gates. Pictures and FPS alone cannot admit it.

`--native_consume_indexed_buffer` is on by default. With `native_ring_stubs`, it
skips the Original body of the IndexedBuffer draw entry `0x826A39F8` on a root
draw whose tiling byte at `G+0x2ABC` has neither bit of `0x21` and whose dirty
word at `G+0x14` has no bit of `0x1E0000`, so the body would reach none of the
render-register caches, shader records or variant records. A capture, shader harvest, scene
session, route trace or a sampled effect probe keeps the Original. Numbering,
the ledger record and native submission are unchanged. In a developer build the
`Native frame timing:` line adds `draw_consumed_indexed_buffer` and, for each nonzero
reason, `draw_fallback_indexed_buffer_<reason>` with the reasons `dispatcher`,
`nested`, `stubs`, `stack_overlap`, `tiling`, `unreadable`, `state_dirty`,
`dirty_recorded`, `dirty_mode`, `dirty_zprepass`, `dirty_vertex_fetch`,
`tiled_state` and `diagnostics`.

Every consume of an Original body (IndexedBuffer, the user-pointer wrappers, the
direct lowerer, Clear, Resolve, SetPredication and the window writer) treats the
stack of the skipped function as dead. That is exact only when no memory the model
reads or writes lies where the Original's own stores would land: its prologue saves
and the frames of its nested callees would change those words, and the model would
then predict from words the Original never saw. A real thread stack holds none of
them, and each consume refuses a call whose stack could, counting `stack_overlap`.

The bound is the guest thread's real stack, read from its KPCR. `r13` holds the
KPCR of the running guest thread; the SDK allocates it with the thread, stores the
stack's highest address at `KPCR+0x70` and its lowest at `KPCR+0x74`, the stack
limit, and updates both on a fiber switch (rerevved-sdk
`include/rex/system/xthread.h` `X_KPCR`, `src/system/xthread.cpp`,
`src/kernel/crt/threading.cpp` `UpdateGuestStackPointers`;
`rex::ppc::stack_limit_from_pcr` reads the same word). Below the limit lie guard
pages, so a skipped body writes only in `[limit, r1)` of its entry, and it reads
the caller's frame up to `r1 + above`. The reach `[limit, r1 + above)` is tested
against the memory the model touches, in 64-bit arithmetic with half-open ranges:
ranges that touch do not overlap, and an object with no bytes overlaps nothing.
The bounds are unknown, and the call is refused as an overlap, when `r13` is
zero, the KPCR words cannot be read, the limit is zero, the base is not above the
limit, `r1` is outside `[limit, base]`, or the KPCR bytes `[r13, r13 + 0x80)` lie
in the reach. An address range that runs past 2^32 cannot be placed and is
refused, except an object found through a guest pointer word, which no word read
can reach and which falls to `unreadable`.

The model's domain is "the skipped Original does not overflow its stack". On
hardware an overflow faults (the guard pages), so a correctly running title never
writes below its limit; the SDK's fiber stacks have no guard pages
(`src/kernel/crt/threading.cpp`) and rely on the same property. The model checks
the static part of it: the entry function's own prologue frame (its `stwu`, which
holds the register saves and the back chain, cited per consume below) must fit
between the limit and `r1`. A call with fewer bytes (`r1` at the limit, say) is
refused as `stack_overlap`, whatever else lies in the reach, because Original
would store below the limit, where the reach does not look. The frames of the
callees nested below the entry function are outside the model: a deeper overflow
would fault on hardware, and the model does not follow it.

The test comes first, before any guest word of the model is read other than the
KPCR (a device whose block `[G, G + span)` runs past 2^32 is `unreadable` before
that, as its words cannot be read): the reach and the device words
`[G, G + span)`, then each pointer word of the
device is read (it is already known not to be in the reach) and the object it
names is tested, all before the tiling words, the derivation or the watch reads, so
an unreadable word cannot mask a `stack_overlap`. The caller-frame words the call
takes as parameters (`r1+84`, `r1+92`, `r1+100`) are its interface, inside the
reach by definition; the objects they name are tested like any other. Per consume,
`above`, `span`, `frame` and the objects tested are:

- IndexedBuffer (`0x826A39F8`): above 0, span `0x33B4`, frame 208 (g148:13093);
  the vertex shader that
  `G+0x3198` names: the flag word `+872` (4 bytes) and the fetch cache records
  `+40` and `+456` (24 bytes each); the declaration that `G+0x2E2C` names:
  `+32..+51` (20 bytes).
- User-pointer wrappers: above 88 (the stride at `r1+84`), span `0x3A5C`, frame 176
  for IndexedUp (g188:13049) and 128 for NonindexedUp (g26:13025), the same objects.
- Direct lowerer: above 88 (the vertex out slot at `r1+84`), span `0x3A5C`, frame
  224 (g63:13048); the IndexedBuffer objects; the allocator (the pool object `G+0x34CC` names, 160
  bytes, and the progress block `G+0x2A90` names, `+4`, 4 bytes); the vertex out
  word that the slot at `r1+84` names (4 bytes) and the index out word `r10` (4
  bytes).
- Clear: above 0, span `0x3A5C`, frame 112 (the wrapper `0x826A2970`, g216:12901;
  the body it calls is nested); the rectangle `r5` (16 bytes), the color `r6` (16
  bytes), the `+0x24` word (4 bytes) of the surfaces `G+0x3098` and `G+0x30A8`
  name, the allocator, and the title image words it reads (the default color, 16
  bytes, and the strip word, 4 bytes).
- Resolve: above 104 (the stencil at `r1+92` and the parameter block pointer at
  `r1+100`, g167:13519 and g167:13539), span `0x3A5C`, frame 368 (g167:13049); the
  source surface's `+0x18` word (4 bytes; `G+0x30A8` for slot 4, else
  `G+0x3098+4*slot`), the header `r6`
  (52 bytes), the rectangle `r5` (8), the point `r7` or the default point (8), the
  color `r10` (16, aligned down), the parameter block that `r1+100` names (12), the
  allocator, and the title image words it reads (the format table, 128 bytes, the
  default point, the two depth scales, the default color, the format map, the
  alpha and color bit tables and the constant 1.0).
- SetPredication and the window writer: above 0, spans `0x31CC` and `0x3340`,
  frames 96 (g171:12569) and 176 (g153:12642); no pointed object. The exit
  probes also test the ring words Original stores after
  the cursor (`[cursor+4, cursor+12)` for SetPredication, `[cursor+4,
  cursor+4+4*words)` for the window writer), read once the cursor is known; the
  consumes read and write none of them.

The allocator objects are the words the Clear, Resolve and lowerer models share:
the pool object's fixed buffer base and size words at `+152` and `+156`, and the
progress word at `+4` of the progress block.

`--native_consume_indexed_buffer_tiled` is on by default. It extends that skip to a
root whose tiling byte is exactly `0x21`, inside a tiling bracket. The body of
such a root only adds a 16-byte record at the list cursor `G+0x33C8`, grows the
list `G+0x33C0` and emits a mask placeholder, all of which feed GPU tile masks.
A consumed tiled root replays the five dirty-qword clears. It needs the same dirty
word and diagnostic conditions as an untiled root, and three more conditions
read from `G` at entry: the word at `G+0x33B0` is not zero, the word at
`G+0x31AC` is zero, and bit `0x20` of the byte at `G+0x2ABD` is clear. A root
that fails one counts as `tiled_state`. Every other tiling byte with a bit of
`0x21`, such as `0x01`, `0x20` or `0x31`, still counts as `tiling`, and so does
`0x21` while the tiled cvar is off. The checks run in this order: dispatcher,
nested, stubs, stack, tiling byte, dirty word, tiled conditions, diagnostics. The
cvar has no effect without `native_consume_indexed_buffer`. In a developer build
the line adds `draw_consumed_indexed_buffer_tiled`, which is also counted in
`draw_consumed_indexed_buffer`. In a developer build with both cvars on,
EndTiling `0x826AC218` also reads the record list of its bracket before the stub
or Original terminates it, and the line adds `tiling_list_records`,
`tiling_list_empty` and `tiling_list_misaligned`; a shipping build does not read
the list. Records are counted in the current chunk from
`chunk + 4` to the cursor. A bracket with no record counts as empty. A cursor
before the first record, off a 16-byte boundary or with an unreadable header
counts as misaligned and adds no records.
The effect probe samples tiled roots apart from untiled ones, in `tiled_*` keys and
`[[changed_offset_tiled]]` rows, and keeps the Original for each sampled root.

`--native_consume_indexed_buffer_dirty` is on by default. It extends the skip to a
root whose dirty word at `G+0x14` has a bit of `0x1E0000`, whose body would call
the shader-state update `0x826ADD88`. The consume clears all five dirty qwords
at `G+0`, `G+8`, `G+0x10`, `G+0x18` and `G+0x20`, in the Original store order.
It validates every destination before storing and holds the ring-stub lease
through replay. An unreadable or unwritable destination counts as `unreadable`
without any replay stores. Three more conditions are read from `G` at entry. Bit
`0x40` of the byte at `G+0x2ABE` must be clear,
or the root counts as `dirty_recorded`. With a vertex shader at `G+0x3198`, the
low three bits of `RB_MODECONTROL` at `G+0x2954` must already hold the value the
update would store: 5 without a pixel shader at `G+0x3194`, and 4 with one when
the dirty word has `0x100000`. Otherwise the root counts as `dirty_mode`. With
the Z-prepass bit `0x40` of the byte at `G+0x2ABC` and a vertex shader, bit
`0x20` of the vertex shader's header word at `+872` must be clear, or the root
counts as `dirty_zprepass`. A dirty word with bit `0x80000` also makes the
update patch the vertex shader's vertex-fetch microcode in place, which the host
hashes and reads, when the vertex declaration at `G+0x2E2C` is not zero, bit
`0x40` of the vertex shader's header word at `+872` is clear, bit `0x80` of the
bytes at `G+0x2ABC` and `G+0x2ABE` is clear, and the vertex shader's fetch
cache record for its variant misses. The variant is the second record without a
pixel shader when header bit `0x20` is set, else the first; the record at
`VS+40+416*variant` hits when its tag equals the declaration's tag at `+48` and
its stride bytes at `+8` and `+16` agree with `G+0x30F0` and `G+0x30F8` under
the declaration's masks at `+32` and `+40`. A root that would patch counts as
`dirty_vertex_fetch`; the check ignores the record's in-flight timestamp and the
interpolator flag, so it may refuse a root whose update would not patch. For the
user-pointer wrappers the first stream byte is the wrapper's
stride, `(stride >> 2) & 0xFF`, which the nested call stores first. This check follows the other
dirty guards. An unreadable word counts as `unreadable`. With the
cvar off a dirty root counts as `state_dirty`. These checks follow the dirty
word and come before the tiled conditions. The cvar has no effect without
`native_consume_indexed_buffer`, and a dirty tiled root also needs the tiled
cvar. In a developer build the line adds `draw_consumed_indexed_buffer_dirty`, which is also counted
in `draw_consumed_indexed_buffer`, and in `draw_consumed_indexed_buffer_tiled`
for a tiled root.

`--native_consume_dirty_vertex_fetch` is read only at startup and is off by
default. It also consumes
a dirty root that would count as `dirty_vertex_fetch`; it needs the dirty cvar
of the entry that reads it, and every other dirty guard still applies. No host
decision reads the vertex shader's microcode words from guest memory: the
reader, the Scene depth-only listing, the pinned digest tables and the
ended-set check all use the words after the linkage fold and the fetch patch
for the draw's own state (stream 0's stride and the declaration at `G+0x2E2C`),
which the patch writes whole from the shader's fetch templates. Words in guest
memory that are unpatched, or patched for another stride, therefore route and
pin as the words patched for this draw; hashes of the words as read stay in
diagnostics only. A consumed root leaves the words as the previous patch left
them, and replays the dirty-qword clears. A later independent setter can
request another Original update. In a developer build the `Native frame timing:`
line adds `draw_consumed_indexed_buffer_dirty_vertex_fetch`, which is also counted in
`draw_consumed_indexed_buffer_dirty` and `draw_consumed_indexed_buffer`. With
the cvar off such a root counts as `dirty_vertex_fetch`.

A consumed root skips more than the patch. On its path 0x826ADD88 calls
0x826ADC48 for the bound variant: a hit stores nothing but the key cache, a
miss patches the words in guest memory in place (0x826AD150), stores the
declaration's tag and the key at the variant record (`VS+40+416*variant` tag,
`VS+48+416*variant` 16 bytes), and the allocation of a tag when the
declaration has none. When the old patch may still be loaded it makes an
inline copy instead (0x826ADA38), patched in the ring. Then 0x826ADD88 stores
the in-flight stamp `G+0x2A9C` at `VS+64+416*variant` for the in-memory path.
Every path that ends in a patch or a hit also stores the 16 bytes at
`G+0x30F0` to `G+0x2E38..0x2E47`; the guest readers of that key cache are the
stride setter 0x8268FB08, the sink 0x826A2B58 and the lowerer 0x826A3048, which
compare the new stride in words with the key cache byte for its stream (the
stride setter indexes `G+0x2E38` by stream) to set the dirty bit `0x80000`. The
key can be derived from entry state, but the rest cannot without
the guest's in-flight decision, tag counter and the patched words, so a consumed
root writes none of it: a store of the key alone would leave a cache, a record
and memory that no Original produces. The dirty qwords are cleared; the
shader cache and record stay as the last Original left them. An Original
that reaches 0x826ADD88 checks the selected variant's record itself: on a miss it patches
again, and a hit is correct because that record, its tag and memory still
agree.

The vertex shader code watch is part of the developer build only (see
[Development](development.md#developer-build)).
`--native_vs_code_watch` is read only at startup, is off by default and only
observes. It measures
what that skip leaves behind. At each consumed dirty vertex fetch draw it reads
the vertex shader at `G+0x3198` and compares the digest of its words in guest
memory with the expected fetch-patched backing for the draw's stride (the
entry's byte at `G+0x30F0` or the wrapper's own). Both sides select the same
VS variant. Inline VS/PS linkage changes execution code separately and does
not make a correct backing fetch patch stale. The watch uses guarded copies
and stores no guest state. It also compares the key the skipped patch
would have stored (the 16 bytes at `G+0x30F0`, with the entry's stride in words
as the first byte) with `G+0x2E38..0x2E47` and with the variant record's key at
`VS+48+416*variant`, where the variant is the second without a pixel shader when
header bit `0x20` is set. The `Native frame timing:` line adds
`vs_watch_consumed`, `vs_watch_stale`, `vs_watch_fresh`, `vs_watch_unreadable`
and `vs_watch_stale_shaders` (distinct stale shader objects since start), then
`vs_watch_key_cache_stale`, `vs_watch_key_record_stale` and
`vs_watch_key_unreadable` for the key, plus `vs_watch_bytes` for guest bytes
copied by all watch reads. The key is compared on every consume whose stride
was read, including those whose words were unreadable. Each consume is counted
once by its words: any failed word read makes it `unreadable`, while a failed
key read leaves that outcome and counts `vs_watch_key_unreadable`.
The first eight stale shaders are logged. It cannot see who reads those words
or the key cache: the SDK's physical memory watch reports guest stores only,
because its read-fault data providers are not implemented, so a guest load from
a watched page raises no event. The counts bound the exposure, not the readers.
A run with `vs_watch_stale=0` and `vs_watch_unreadable=0` left no word differing
from what the host ran; any other count is the set of draws and shaders whose
guest-visible words are not those of the host. A cache key that differs only
makes the setters and lowerers set `0x80000` again. The
in-flight stamp, the tag and the patched words are not compared, as the guest's
choice between the in-memory and the inline path is not derivable. The
[VS readback census](#vs-readback-census) observes the guest entries that do
read the code image.

`--native_consume_user_pointer` is on by default. With `native_ring_stubs`, it
applies the same skip to the two outer user-pointer draw wrappers, IndexedUp
`0x826A3568` and NonindexedUp `0x826A3000`. The native draw reads the caller's
vertex and index pointers and the stride at entry, before Original. The
wrapper's Original only copies those bytes into the ring through its nested
lowerer `0x826A3048` or sink `0x826A2B58` and publishes the cursor, so a
consumed wrapper skips that nested call and its ring and cursor stores; every
call site ignores the wrapper's return value. The dispatcher counts the skipped
child as a nested invocation, so a consumed wrapper is not a `nesting_mismatch`.
A root is consumed under the conditions of an IndexedBuffer root, with the
stack, tiling byte, dirty word and diagnostics tested in the same order; the
stack reach is the wrapper's own (`[limit, r1 + 88)`, its frame 176 or 128 bytes
below `r1` the least). The dirty
word is the one the nested call would leave: the lowerer and sink set bit
`0x80000` themselves when the stride, `(stride >> 2) & 0xFF`, differs from the
byte at `G+0x2E38` and the vertex shader word `G+0x3198` is nonzero, and then
run `0x826ADD88`. The verdict reads the stride at entry (IndexedUp: the word at
`r1+84`; NonindexedUp: `r7`) and applies that bit, so such a root is
`state_dirty`, or checked by the dirty guards with the dirty cvar on; an
unreadable stride slot, `G+0x2E38` or `G+0x3198` is `unreadable`. The
diagnostics also include a label adapter capture that this call began,
because its finish reads what Original leaves. The effect probe does not sample
the wrappers. The sink called directly by guest code, and `0x826A3608`, keep
their Original; the direct lowerer has its own bring-up skip below.
`--native_consume_user_pointer_tiled=true` and
`--native_consume_user_pointer_dirty=true` extend the skip to roots with tiling
byte `0x21` and to roots with a dirty bit of `0x1E0000`, under the same
tripwires and guards as the IndexedBuffer cvars of the same names; each needs
`native_consume_user_pointer`, and in a developer build the tiled cvar also turns
on the EndTiling list observation above. In a developer build the `Native frame timing:` line adds
`draw_consumed_indexed_up`, `draw_consumed_nonindexed_up`,
`draw_consumed_user_pointer_tiled`, `draw_consumed_user_pointer_dirty` and
`draw_consumed_user_pointer_dirty_vertex_fetch` (the roots
`--native_consume_dirty_vertex_fetch` admits); the tiled, dirty and dirty
vertex fetch counts sum both wrappers and are also counted in the wrapper
totals. For each nonzero reason it adds `draw_fallback_indexed_up_<reason>` and
`draw_fallback_nonindexed_up_<reason>`, with the reasons listed for
IndexedBuffer, plus `reservation` for an unestablished reservation outcome.

A consumed UP wrapper clears the same five dirty qwords, then writes
`G+0x18 = 1` on the successful reservation path (`0x826A3520` for IndexedUp,
`0x826A2FD8` for NonindexedUp). IndexedUp requires two successful 16-byte-aligned
side-buffer reservations; NonindexedUp requires one. The preflight uses the
existing bound on words emitted by a flush and the allocator model for a fixed
buffer or a null pool. It
refuses allocation failures, waits, tile-list growth, threshold flushes and
unbounded shader-update work before any replay store. In particular, dirty
UP roots that reach `0x826ADD88` count as `reservation` even when the dirty
guards admit them. The exception is a root whose only shader dirty bit is
`0x80000` with no vertex shader: `0x826ADD88` returns through `0x826AE5B8`
without allocating, so the preflight drops that bit. Allocation outputs and cursor stores remain
skipped. The protected device span includes the allocator words through
`G+0x3A58`, plus the selected pool or progress object.

The bring-up `--native_consume_lowerer=true`, with `native_ring_stubs`, skips
the Original of the direct draw lowerer `0x826A3048` on a held root: a call
from guest code outside the IndexedUp wrapper, which the dispatcher holds until
the commit `0x826A2FF0`. A call nested in another observed draw, including the
wrapper's own child call, is not a root and runs its Original. The native draw
keeps its dispatcher scope, so the numbering, the entry shadow and the host
draw are those of the Original path, and at the commit the host reads the same
buffers, dword counts and out words from guest memory. The skip stores what a
later reader takes: `G+0x1C` bit 0, the side-buffer allocator words
(`G+0x3A44`, `G+0x3A4C`, `G+0x3A54`, and for a fixed pool `G+0x34` and
`G+0x38`), the staging words `G+0x3484..0x3494`, the vertex and index buffers
through the caller's two out words (the vertex slot at `r1+84`, the index slot
in `r10`), and `r3 = 0`. The two buffers are the allocator's takes of
`(r6 * r9) / 4` dwords and of `r7` (32-bit indices, `r8` bit 2) or `(r7 + 1) / 2`
dwords, 16-byte aligned, from the fixed pool `0x826A6848` or the null pool
`0x8269B618` that `0x8269B818` selects. As for every consume the ring words of
the flush groups and the draw, the ring cursor `G+0x30`, the zeroing of the
dirty qwords and the tile record of a tiled root are not stored; the dirty words
stay set, so the next root that runs its Original flushes them, and `G+0x3484`
holds the entry cursor, so the commit leaves the cursor where it was. The
conditions are those of an IndexedBuffer root with the dirty cvar off, with the
stack reach of the lowerer (`[limit, r1 + 88)`, its frame 224 bytes below `r1`
the least) tested after the stubs
and before any guest word is read (an
unreadable word or an out word the dispatcher cannot write is `unreadable`; a
dirty bit of `0x1E0000` or a stride change with a vertex shader, which would run
`0x826ADD88`, is `state_dirty`), and the model must also show that Original
cannot flush, wait or fail: the failure marker bit `0x20` of the byte at
`G+0x2ABD` is clear (`alloc_marker`), a pool object has a fixed buffer
(`alloc_path`), the upper bound of the ring words of the dirty flush groups
stays below the soft limit `G+0x38` and the hard limit `G+0x34`, and no take
passes the pool threshold `G+0x3A58` with the bracket word `G+0x33B0` clear
(`ring_space`), both buffers fit in a fixed pool above the cursor plus 164
(`alloc_capacity`), and the null pool allocator does not wait for the GPU
(`alloc_wait`). In a developer build a capture, shader harvest, scene session,
route trace, effect sample or `native_lowerer_exit_check` that needs the body counts as
`diagnostics`. `--native_consume_lowerer_tiled=true` also skips roots with tiling
byte `0x21` under the IndexedBuffer tripwires when the tile list at `G+0x33C4`
has room for the 16-byte record (`tile_growth`); in a developer build it turns on
the EndTiling list observation above. The sink `0x826A2B58` is not skipped. In a developer build the
`Native frame timing:` line adds `draw_consumed_lowerer`, `draw_consumed_lowerer_tiled` (also
counted in the first) and `draw_fallback_lowerer_<reason>` for each nonzero
reason: `dispatcher`, `nested`, `stubs`, `stack_overlap`, `tiling`, `unreadable`,
`state_dirty`, `tiled_state`, `alloc_marker`, `tile_growth`, `alloc_path`,
`ring_space`, `alloc_capacity`, `alloc_wait` and `diagnostics`.
In a developer build, `--native_lowerer_exit_check=true` compares the model with every lowerer call
whose Original runs on a root under a dispatcher: it derives the exit state at
entry and reads the guest words when Original returns. It never changes what
runs. The line adds `lowerer_exit_ok`, `lowerer_exit_tiled`,
`lowerer_exit_tiled_ok`, `lowerer_exit_unreadable`,
`lowerer_exit_flush_words_max`, `lowerer_exit_flush_words_sum`,
`lowerer_exit_flush_calls`, one `lowerer_exit_mismatch_<word>` count for each of
22 compared words (`result`, the five dirty qwords, `limit_34`, `limit_38`,
`pool_pos`, `pool_used`, `wrap_pos`, `vertex_dest`, `index_dest`,
`vertex_dwords`, `index_dwords`, `vertex_out`, `index_out`, `alloc_byte`,
`pm4_words`, `ring_words`, `tile_record` and `fence`), and
`lowerer_exit_unmodeled_<reason>` for calls the model refuses, among them
`lowerer_exit_unmodeled_stack_overlap` for a call whose stack reach holds a device
word or an object the lowerer model touches (tested at entry, before any other
guest word is read). A call is `ok` when
no word differs; the flush groups must have left the dirty qwords zero, and
the cursor may not move by more than the model's bound, which a run of the
probe tests against the words Original emitted.

#### VS readback census

The readback census and its option are part of the developer build only (see
[Development](development.md#developer-build)).
`--native_vs_readback_probe=true` (read only at startup, off by default) observes the two
entries that hand a vertex shader's code image back to the guest: the refresh
`0x82662D28` (`r3` = owner O, `r4` = refresh record Q) and the readback
`0x82694480` (`r3` = shader VS, `r4` = destination, `r5` = size pointer). The
refresh sizes and copies the image through two nested readback calls and
clears Q's dirty bit `0x80000000` at `Q+0x14` before its size query; the
readback copies the metadata words and the whole code image. Both Originals
run exactly once on every path, so with the cvar off every hook, route,
refusal and counter line is unchanged. The cvar is independent of
`native_ring_stubs`, `native_vs_code_watch`,
`native_consume_dirty_vertex_fetch` and `native_frame_timing`, and enables
none of them. Two raw wrappers own those addresses; the disabled path adds
one cached flag read before the Original. Enabled, the observer writes no
guest memory and no register, and reads only the guarded big-endian scalars
named below, through the same checked copies as the completion probe, from the
first guest entry on. A diagnostic that fails to read leaves the call
untouched and is counted. An Original that throws is abandoned, never counted
as a return, and the exception propagates unchanged.

Each call keeps a token with its entry values: the raw `r3`, `r4` and `r5`, the
full 64-bit `r1` and link register, and the guest thread (`r13`). A readback is
a query when its entry destination is zero and a copy otherwise, whatever the
readability of its fields; the nested pair of one refresh is one query and one
copy, not two refreshes or two code copies.

- A refresh reads `Q+0x04` (VS), `Q+0x08` (buffer), `Q+0x14` (flags) and
  `O+0x0C` (owner device) at entry, before Original clears the dirty bit, and
  again at return from the saved Q and O, never from the returned registers.
  These are entry and exit samples, not a claim that nothing wrote between
  them. Its caller is `prep` for link register `0x825E1D30`, `descriptor` for
  `0x8265E270` and `other` for any other full 64-bit value.
- A query reads `VS+0x36C` (M) and `VS+0x370` (S) at entry and the word at the
  saved size pointer at return; it never reads code or output. A copy
  additionally reads `VS+0x20` (C) and never reads the size pointer: the copy
  body does not write it. The raw `r3` result of every call is kept in full
  width. The readback has no explicit status, so no result is classed as
  success.
- The intervals are the metadata `[VS+0x368, +M)`, the code `[C, +S)`, the
  output `[destination, +M+S)` and the four-byte size word. Each is validated
  alone with widened arithmetic, without wrapped endpoints. An empty extent is
  valid and takes part in no intersection; a nonempty extent at a null
  address, past the 32-bit address space, or whose M+S exceeds 32 bits is
  invalid. Intersections among them are flagged as virtual overlap candidates.
  Code and output bytes are never read and no physical alias is claimed.
- A readback pairs to a refresh only on the same guest thread, from the
  nearest active refresh frame above its own stack pointer, with the direct
  caller (`0x82662D64` for the query, `0x82662DFC` for the copy) and the same
  VS pointer. Otherwise it is `outside_refresh` when the thread has no active
  refresh, and `parent_unknown` for a mismatch, a frame that does not nest, an
  unknown thread, or while a refresh without correlation storage is active.
  Equal pointers alone never pair calls or establish one resource generation.

The `Native frame timing:` line adds these fields since its previous drain,
and a `Native VS readback probe: shutdown` line carries them for the rest of
the run even when timing is off or the last events follow the last timing
line. Detail records are logged at the drain, outside the guest hooks.
`rb_read_<refresh|query|copy>_enter`, `_return` and `_abandon` are interval
counts; `_pending` is the current number of calls inside their Original, and
`_enter_total`, `_return_total` and `_abandon_total` are cumulative. At every
drain `enter_total = return_total + abandon_total + pending` per kind, and
`rb_read_conserved` reports whether it holds. A shutdown report with active
refresh frames logs each as a pending record; no pending call is invented as a
return.

- `rb_read_refresh_<prep|descriptor|other>` partition refresh entries by
  caller and `rb_read_refresh_<dirty|clean|q_unknown>` by the entry Qdirty
  sample. `rb_read_refresh_negative_result` counts returns whose low 32 bits
  are negative.
- `rb_read_<query|copy>_<in_refresh|outside_refresh|parent_unknown>` partition
  each readback kind; only `in_refresh` is a proved nested pairing.
- `rb_read_query_size_valid` counts readable size words at query return and
  `rb_read_copy_header_valid` complete copy headers whose data intervals are
  all valid. `rb_read_requested_meta_bytes` and `rb_read_requested_code_bytes`
  sum the entry M and S of those copies once each. They are requested extents,
  not measured copy traffic: aliasing, concurrency and helper behavior can
  change what is read. `rb_read_probe_bytes` counts the bytes the reader
  copied, including every entry and exit scalar.
- `rb_read_unreadable` counts failed scalar reads, `rb_read_range_overflow`
  calls with an invalid interval, `rb_read_overlap` calls with an intersection,
  `rb_read_pair_error` inconsistent tokens, threads or parent unwinds,
  `rb_read_correlation_drop` refreshes denied one of the 64 correlation
  slots and `rb_read_event_drop` detail records lost to the 128-record ring.
  `rb_read_untracked_refresh` is the number of such refreshes still active.
  These are not disjoint terminal categories and none stands for zero reader
  traffic. A full ring or table never suppresses an entry or return count and
  never pairs a call to another live slot.
- `rb_read_reporting_failures`, `rb_read_final_skipped` and
  `rb_read_final_incomplete` follow the reporting rules below.

Storage is fixed. A drain resets interval counts and details together under
one lock; totals, call identities, active refresh frames and pending gauges
survive it, so a refresh that begins before a drain and returns after it keeps
its children. A final report waits at most 10 ms for each lock. If it cannot
take the reporting lock it logs and returns an incomplete marker, and the next
report carries the skipped count. Interval counts whose formatting or final log
fails are retained for the next drain; a detail line that fails to log is lost
and counted in `rb_read_reporting_failures`.

The census does not establish a shader generation, the draw that preceded a
read, the outer pass or query that caused a refresh, whether the copied bytes
equal what a host decision derived, or that no other reader exists; the logs
print `generation=unknown outer_activation=unknown` to say so. The consumed
dirty-fetch draws described above stay behind
`--native_consume_dirty_vertex_fetch`, default off, and a consumed UP wrapper
still refuses reservation before any replay store: this census admits nothing.
The VS watch above counts exposure, not loads, and the host shader identity
memo derives the current draw's fetch and linkage words and identity; neither
is generation tracking or a persistent record of the Original's patches. The
`cp_*` fields (see [completion accounting
observation](#completion-accounting-observation)) sample issue and completion
words; they contain no reader entry, and a balanced `cp_balance_ok` is not a
shader-history or retirement proof.

Owner census procedure. Use one build and one set of settings and run two
sessions through the same boot, menu and world actions, with no build running:
`--native_vs_readback_probe=false`, then `--native_vs_readback_probe=true`.
Record the executable revision and every effective startup flag. Keep
`native_consume_dirty_vertex_fetch=false` and `native_vs_code_watch=false` in
both; keep `native_consume_set_predication_preswap=true` and
`native_completion_probe=true` equal in both, with `native_frame_timing=true`,
and leave every draw, user-pointer, lowerer and ring-stub setting as it was.
End each run through the verdict script. This checks correctness and cadence,
not frame rate or bridge speed. Accept the observer when:

1. The enabled run shows no new visual or error class and every existing
   consume, fallback and reservation count keeps its definition; check the boot
   as well as the world, because effect loading reads shaders first.
2. At least one query, one copy and one paired refresh are seen. A natural run
   with none records that finite-window result; do not force one into it.
3. `rb_read_conserved=1` on every line including the shutdown line, with
   `rb_read_pair_error`, `rb_read_correlation_drop` and `rb_read_event_drop`
   zero over the accepted coverage. Unreadable, range and overlap cases remain
   visible; a window with drops supports no absence or rate claim.
4. Counts and requested extents are tabulated by query and copy, by caller
   (`prep`, `descriptor`, `other`) and by Qdirty sample, with nested calls
   correlated rather than added to refresh counts, together with the phases
   that had no coverage. Report the `cp_*` quality and sampled scope beside
   them.
5. `dirty_vertex_fetch` remains refused by default and reservation counts stay
   independent. A zero reader count, an unchanged picture or earlier dirty-fetch
   consumption never admits a consume.

`--native_consume_window_writer` is on by default. With `native_ring_stubs`, it
skips the Original of the window packet writer `0x8268D3A0` (g153:12634-13092)
on calls whose exit the host knows exactly. The writer takes `r3 = G` and the
window `r4 = x0`, `r5 = y0`, `r6 = x1`, `r7 = y1`. Its callers are the scissor
and window setters `0x8268F9C0` (g188:13025), `0x82690040` (g167:12996),
`0x826922D0` (a tail call, g160:13243) and `0x82698720` (g208:13450), the Clear
body (g158:13912) and the Resolve body (g167:13639 and g167:14913). The writer
stores only ring words through the cursor chain, the cursor `G+0x30`
(g153:13085) and stack saves below the caller's `r1`. The ring words are not
read by any host reader in native mode, and the consume sets `r3`, the one
result of the body that a reader can take, and stores nothing else. `r3` is
`cursor + 12` for the plain form (g153:12778-12794), `cursor + 8` for a tiled
call without tiles (g153:12815-12821), and `3 << 2(n - 1)` for `n` tiles (the
last `slw` of the loop, g153:12904-12912, where a shift amount with bit `0x20`
gives zero). As for every consume, `G+0x30` stays at its entry value: the ring
words and the cursor advance of the call are not stored, and the next flush or
ledger position is taken from the lower cursor. The form is tiled when the
tiling byte at `G+0x2ABC` has bit `0x10`, or has bit `0x20` and each of the
five bound surfaces at `G+0x3098` equals the applied surface at `G+0x31B8` or
is zero (g153:12670-12760); the tile count is `G+0x31CC`. The model emits the
exact ring words of each form (4 for the plain form; `2 + 7n` for a tiled form, six more
with the byte `G+0x2ABF` bit `0x20` and tiling bit `0x40`, and 4 for the
predicate tail), which the probe compares with the guest ring.

The consume runs Original instead, counted by reason, when Original could take
a path the model lacks: `stubs` (ring stubs are off, or have not validated this
device), `stack_overlap` (the thread's stack reach `[limit, r1)`, which holds the
prologue saves at `r1 - 96` and the back chain at `r1 - 176` (g153:12643,
g29:15204) that the epilogue reads back (g153:13090), holds a byte of the device
words `[G, G + 0x3340)` the body reads, or the stack bounds are unknown; the
saves would land in device words the body reads, so the skip would be inexact),
`unreadable` (a guest word could not be read, or the device span wraps 2^32),
`tile_count` (more than
16 tiles, the bound the other consumes use for render targets), `ring_space`
(the entry cursor is past the soft limit `G+0x38`, the cursor after the last
tile is past it, which calls the flush `0x8269C588` at g153:12656-12668 and
g153:12966-12979, or the words pass 2^32) and `diagnostics` (a shader harvest,
scene capture, route trace, the exit check of this or another
consume, which measure the ring cursor the skipped words would have moved). The
first test that applies is the reason; they run in the order `stubs`,
`unreadable` for a device that wraps 2^32, `stack_overlap` (before any other guest
word is read), `unreadable` for the words, `ring_space` for the entry cursor,
`tile_count`, `ring_space` for
the cursor after the last tile, `ring_space` for words that would pass 2^32 (any
form), then `diagnostics`. The `Native
frame timing:` line of a developer build adds `window_writer_consumed`, one
`window_writer_consumed_<caller>` for each nonzero caller (`8268f9c0`,
`82690040`, `826922d0`, `82698720`, `clear`, `resolve` or `other`, from the
return address), `window_writer_ring_words_consumed` (the words the consumed
calls would have stored), `window_writer_ring_words_original` and
`window_writer_original_calls` (the cursor advance of the Original runs, a cheap
read of `G+0x30` around the call), `window_writer_original_unbounded` when a
run moved the cursor back or by more than a call stores (a flush), and
`window_writer_fallback_<reason>` for each nonzero reason.
In a developer build, `--native_window_writer_exit_check=true` compares the model with every call
whose Original runs: it derives the exit state at entry and reads the guest
words when Original returns. It never changes what runs, and it turns the
consume's own skip off (`diagnostics`). The line adds `window_writer_exit_ok`,
`window_writer_exit_tiled`, `window_writer_exit_tiled_ok`,
`window_writer_exit_unreadable`, `window_writer_exit_ring_words_max`,
`window_writer_exit_ring_words_sum`, `window_writer_exit_calls`, one
`window_writer_exit_mismatch_<word>` count for each of six compared words
(`result`, `cursor`, `limit_34`, `limit_38`, `state`, the other guest words
the writer reads, and `ring_content`, the ring words stored after the entry
cursor), and
`window_writer_exit_unmodeled_<reason>` for calls the model refuses (the
reasons are `tile_count`, `ring_space` and `stack_overlap`; the last also
when the reach holds the ring words Original stores after the cursor, which the
probe reads). A call is
`ok` when no word differs; a call that unwinds is not compared.

The guest's display gamma has two cvars: `native_gamma_apply` is on by default
under `--renderer=native`; `native_gamma_observe_check`, a bring-up diagnostic
that exists only in a developer build, is off by default. The guest
sets its ramp once for each device create and reset: display init `0x826A5EB8`
(g204:13244) calls the setter `0x8268FE08` (g59:12850-12963), which converts the
ramp into 1536 bytes at `r1+80` of its frame, three channels of 256 big-endian
halfwords holding the ten-bit value shifted left by 6. The midasm hook
`ObserveNativeGuestGammaTable` at `0x8268FE30` (g59:12877) reads them before the
setter compares them with the device copy at `G+15004`, retains a changed table
there and flags the device (`G+23908` bit 31, `G+16708` bit 1). The setter does
not touch the display. The emitter `0x826A7040` (g206:12515) writes its `r4` table
to the display LUT. After `VdSwap` the chain `0x826A4638` (g84:13765), `0x826A4458`
and `0x826A3E48` (g11:13598) reaches it at the call returning to `0x826A3EB0`
(g11:13663), and `0x826A77F8` (g198:13208) is a second caller. A setter value
therefore takes effect only when the next emission returns. Before the first
emission the display holds the SDK's power-on ramp, linear at `i * 0x3FF / 0xFF`
in every channel. The display LUT belongs to the GPU, so a new guest device does
not restore it. The PWL setter `0x8268FED0` (g204:13263) and emitter `0x826A7138`
(g11:13655) use another layout and are only counted. A PWL emission, or one
whose table does not decode, leaves the LUT unknown once it returns, not
before. Emissions may overlap. Each is tracked from entry to return, and the
table in effect is that of the emission that returned last, whatever order they
began in. At most eight are followed at once; a ninth begin evicts the oldest.
Tickets are issued in order, so the return of a ticket below the next one that
is not in flight (evicted, dropped with its device, or already returned) means
the LUT holds a table the observer no longer has. It is then unknown until an
emission that began after that return completes, and apply uses the assumed
identity ramp. A return with no ticket, or one never issued, is an error with
the same effect; a begin that fails while observing returns no ticket, so its
return leaves the LUT unknown too.

`--native_gamma_observe_check=true` (developer build) observes this path and
changes no output; `native_gamma_apply` runs the same observer. The
setter hook and the emitter hook forward to one process observer before the
frame capture logic runs, and neither throws into the guest. A table decodes
when it has exactly 1536 bytes and no halfword has a bit below the ten it keeps;
the observer packs it as the renderer's gamma pass reads it, red in bits 20-29.
In a developer build each changed setter table logs `Native gamma observer:
setter device=<G> table_sha256=<hash>` (at most eight lines), the hash of its 1024 packed
little-endian bytes, the form the frame capture saves as `gamma.bin`. The
`Native frame timing:` line of a developer build adds every field below each
window. A player build keeps the device adoption, the emission tracking and the
table application, and keeps no counters, retained-table comparison or hash log.

- `gamma_obs_setter`, `gamma_obs_setter_changed` (a table unlike the previous
  setter table of the device), `gamma_obs_setter_decode_fail` and
  `gamma_obs_pwl_setter`.
- `gamma_obs_emit` (emitter calls), `gamma_obs_emit_complete` (calls that
  returned), `gamma_obs_emit_decode_fail`, `gamma_obs_pwl_emit`,
  `gamma_obs_emit_retained_equal` and `gamma_obs_emit_retained_mismatch` (the
  `r4` bytes against the device copy at `G+15004` at emitter entry),
  `gamma_obs_emit_overlap` (an emission began while another had not returned;
  both stay in flight), `gamma_obs_emit_evicted` (a return of an emission the
  observer no longer follows) and `gamma_obs_emit_second_site` (a caller
  other than the call returning to `0x826A3EB0`).
- `gamma_obs_device_change` (another device called in; its setter value and
  pending emission restart, the completed emission stays; the return of a
  pending one then counts as `gamma_obs_emit_evicted`).
- `gamma_obs_swap`, `gamma_obs_swap_before_emit` (no emission has returned, so
  the swap sees the linear default), `gamma_obs_swap_pending_setter` (the setter
  table differs from the table in effect), `gamma_obs_swap_table_changed` (the
  table in effect differs from the previous swap's) and
  `gamma_obs_nowork_swap_changed` (an Empty swap whose emission sequence moved
  since the last swap that presented work).
- `gamma_obs_error` (an unreadable guest word or a call with no device) and
  `gamma_obs_identity_diff_entries`, a gauge that keeps its last value and
  counts the entries of the table in effect that differ from the identity ramp
  the frames otherwise assume.

`native_gamma_apply` is on by default under `--renderer=native` and ignored under
any other renderer, where the hooks cost what a cvar that is off costs;
`--native_gamma_apply=false` turns it off. It implies the observer, so the
hooks and a per-swap view of the table in effect run by default under the
native renderer. The `gamma_obs_*` fields appear on the `Native frame timing:`
line only with `native_frame_timing=true`, which defaults to false.
Native frames then use the table
of the last completed emission, or the linear default before the first, for
`frame.gammaTable` and set `gammaAssumed` false. The default counts as
observed, because it is the hardware state. The table stays assumed, as the
identity ramp, only while the LUT is unknown after a PWL or undecodable
emission. Status labels do not change: the clear and resolve are still assumed,
so a frame stays `Provisional`. With the cvar off the frames keep the identity
ramp and every existing status and failure is unchanged. The assembler adopts a
changed table on a swap without presented work as described under the displayed
image above.

Each ring registration starts a ledger epoch. When `0x8269C8E0` has drained the
old ring and before it frees the blocks, unreleased spans of the earlier epoch
are dropped without writes. Later spans wait for the next writeback
registration before any write.

### Native frames

Each `VdSwap` closes a frame epoch. Draws, clears and resolves share increasing
operation marks. Per-target Clear capture runs after guest clipping, retaining
the target, rectangle, color, depth and stencil. The assembler admits full
unpredicated clears of RGBA8 target 0; unsupported clear forms remain unowned.
An off-screen color clear may instead cover a rectangle from the target's
origin, as the Hall of Glory panels clear 900x675 of their 928x688 surface:
the host clears that rectangle alone, and every resolve of the target until
its next clear must lie inside it (`resolve_form_unsupported: rect`). A
partial rectangle elsewhere, or of the presented target, fails
`clear_form_unsupported: color rect`.
A clear that does not decode fails with a named code: `clear_state_unreadable`
with the unread word's address, or the unmodeled guest value, such as
`clear_target_format_unsupported` (a color format other than `k_8_8_8_8`,
`k_2_10_10_10` or `k_16_16`, or a nonzero exponent bias), `clear_target_msaa_unsupported`
(4X), `clear_depth_format_unsupported` (`D24FS8`), `clear_depth_extent_mismatch`
or `clear_target_unbound`.

Clears, draws and resolves decode the bound color set `G+0x3098..0x30A4` as one
target. RT0..RTn must be bound contiguously (`target_set_sparse`), share RT0's
logical extent and samples (`target_set_shape_mismatch`), be the slots
BeginTiling saved while tiling maps (`target_set_not_tiling_saved`), and occupy
disjoint EDRAM tiles (`target_set_edram_overlap`). A `k_16_16` slot is a lone
target: in a set it fails `target_set_format_unsupported: RT<n> format 4`. A
depth resolve also needs a
bound depth surface (`depth_unbound`) outside every color slot. These details
reach the ledger as `clear_target_set_*` or `resolve_state_unreadable: target_set_*`.
`0x826A22F8` clears each bound render target separately; only index 0 may clear
depth and stencil (`clear_mrt_flags`), and its depth surface must lie outside
every color slot of the set (`clear_target_set_edram_overlap: RTn depth`). A set
with extra targets is never presented, even where its RT0 has the presented
shape or base. A draw joins a frame on an RGBA8 RT0, or on a lone
`k_2_10_10_10` or `k_16_16` RT0 without a depth test, depth write or stencil
test, which the host replays only depth-free; a family builds the latter only
from shaders specialized for that color format. Other draws fail
`frame_target_mismatch`. An off-screen clear of such a target that also clears
depth or stencil fails `clear_form_unsupported: depth_clear_packed_target`.

A tiled off-screen set is one pass. The index-0 clear must name every color
slot (`mrt_clear_partial`), and each later index must follow in order with the
same tiles and color before any draw (`mrt_clear_unpaired`); the clears cover
tile 0's rectangle, as the guest predicates them to tile 0. A draw before every
slot is cleared fails `mrt_clear_incomplete`, and a draw after the first resolve
fails `tiled_source_resolving`. Each resolve reads one slot of one tile: the
predicate mask `3 << 2i` names tile i, and the rectangle and point must be that
tile's (`tiled_resolve_form`). Every cleared slot of tile i resolves once before
tile i+1, in the loading pass's order: depth when it was cleared, then RT0, RT1 and so
on (`tiled_resolve_order`, naming the expected tile and slot). A resolve's clear
bits then reach only slots its tile already copied. Copy control must select the resolved slot
(4 for depth), sample k0, k1 or, for color, k01, and set no clear bits on a
depth source (`resolve_form_unsupported: control`); a depth resolve needs the
index-0 depth and stencil clear (`resolve_depth_uncleared`). The clear bits of
tile i's resolves define tile i+1's starting EDRAM: the color clear word in the
slot's 8:8:8:8 or 10:10:10:2 layout, and `RB_DEPTH_CLEAR` as depth in bits 8..31
and stencil in bits 0..7. A later tile without such a start for its slot fails
`tiled_background_undefined`. Each resolve is its own producer frame, which
replays the clears, those tile starts and the draws, then packs the tile's
rectangle into guest words with the slot's packing. Tiles are sampled k0 as host
sample 0, k1 as host sample 3 and k01 as both. After the last tile the set has
no modeled content, and any resolve of it fails `resolve_source_unmodeled`.
Until the next clear or swap, an untiled draw into that same set, with the same
EDRAM ranges, is admitted without host work, because nothing resolves the set
again. Only a dead color write qualifies: `RB_DEPTHCONTROL` (`G+0x2934`) may
enable no stencil, depth test or depth write, and `RB_COLOR_MASK` (`G+0x28DC`)
may write no slot beyond the set. Another such draw fails
`ended_set_write_unsupported` with both words. The shader mode bytes
`G+0x2ABC` and `G+0x2ABE` may not set bit 6, the conditional or recorded modes
that may run a VS variant other than the one named below
(`ended_set_conditional_or_recorded_shader: <word at G+0x2ABC>`).
`RB_SAMPLE_COUNT_CTL` and `RB_SAMPLE_COUNT_ADDR` (`G+0x2A48`, `G+0x2A4C`) must
keep their device-init values 2 and 0, so no occlusion query counts the dropped
samples (`ended_set_query_state: sample_count_ctl <word> addr <word>`); the
title emits no sample-count event. Any of these words unread fails
`ended_set_state_unreadable`.
Those registers bound only the attachments, so the shader pair must also be
one a captured loading frame draws into the set, whose microcode writes nothing
else: a GFx quad batch, an indexed-UP draw from `0x82303E90` with the pinned
GFx PS and the pinned VS; or the label batch, a nonindexed-UP draw from
`0x82304258` with the pinned label pair. Both are compared as catalog word
digests of the VS (variant zero) and PS objects at `G+0x3198` and `G+0x3194`.
The VS digest is of its words after the linkage fold and the fetch patch for
the draw's own stride and declaration, so it is the pinned one whether or not
the guest has patched the words in memory for that draw (the frame's first
batch holds the shader file's templates there).
Any other pair, such as a memory-exporting shader, fails
`ended_set_shader_unpinned: vs <digest> ps <digest>`, and unread shader words
fail `ended_set_shader_unreadable`. Skip mode drops either draw and leaves the
frame partial.
A depth-only clear (`0x8250E718`, flags `0x30`) of the set's own depth surface
also keeps the set and is admitted without host work; the loading frame
issues one after the pass, before its batches into the set. It must be untiled
and unpredicated, clear no color, and name the surface object and EDRAM range
that the pass's index-0 depth and stencil clear defined. A pass without that
clear, another surface or range, or a tiled clear fails
`depth_clear_target_unmodeled: <base>`, and a predicated clear, a color clear or
other flags fail `clear_form_unsupported`; either ends the set. Nothing reads
that depth: the set's draws neither test nor write it, and presented work over
the storage it may alias still needs the full presented clears below.
The set may alias presented EDRAM, so presented work that writes a color
channel after its first clear needs a full presented clear
(`tiled_target_uncleared`), and a presented draw needs each depth plane it
uses defined again (`tiled_depth_uncleared`): a depth test or write needs
depth, an enabled stencil needs stencil, and a full presented clear, with
color or depth-only, redefines each plane it clears. A tiled scanout leaves both
undefined the same way,
since its clear bits clear color and depth for the next tile. So does an
untiled off-screen clear whose color, or cleared depth, overlaps the presented
color or depth storage: by EDRAM tile range when the full presented clear and
the off-screen clear both decode one, otherwise by base. The off-screen range
spans the tiles its clear rectangle touches, from the decoded pitch; a tiled
or pitchless surface counts as its whole allocation. The scanout or the
presented draw then fails `presented_edram_aliased: off-screen clear
<target>` instead of presenting the overwritten work. An off-screen pass's
host depth and stencil start at the far clear, so a draw into it that tests
depth (other than always) or stencil needs that plane from the pass's clear;
otherwise it fails `offscreen_depth_undefined: depth <base> draw <target>`,
also when the clear names a shared depth surface but clears only stencil. A presented draw
that writes a color channel before any full clear after a tiled scanout fails
`presented_draw_before_clear` with the draw's tiling (`tiles <n> predicated
<p> mask <m>`, `untiled` or `tiling unread`): Gamebryo begins a tiled frame
(`0x8250F988`) with no BeginTiling clear color and clears tile 0 only for a
nonzero clear mode (`0x8250B168`), so such a frame draws over the EDRAM the
last scanout left, which the host does not model.
One form of such draws is dead and is discarded without host work: the game
start frame draws GFx and labels untiled into the 640x720 tile surface, then
begins tiling and clears tile 0. An untiled, unpredicated admitted draw is held
when its writes are bounded by guest state: one color target, PA_SC_WINDOW_OFFSET
zero, the window scissor's bottom-right corner inside the surface and its
pitch, RB_COLOR_MASK writing RT0 only, no conditional or recorded mode, the
device-init sample count state, and, when RB_DEPTHCONTROL enables stencil or
depth writes, an untiled depth surface. The next operation must then be the
tile-0 predicated clear of the whole screen (`0x8250F988`) on the same surface
object and EDRAM range, with tile 0's rectangle holding the held scissor and,
for held depth or stencil writes, depth and stencil cleared on the same depth
surface. Tiles replay in index order and the clear is tile 0's first operation,
so it overwrites every held write before any tile reads the EDRAM; later tiles
start from tile 0's resolve clear. Any other draw, clear, resolve or swap first
fails the frame as `presented_draw_before_clear: untiled; dead_write_unproven:
then <operation>`, and an unbounded draw names its bound, such as
`dead_write_unproven: window_scissor <w>x<h> outside surface <w>x<h> pitch <p>`.
A held draw has no host work, so its completion span releases as a no-op
draw's does while the hold is open, and a guest waiting for ring space reaches
the operation that decides the hold. After a failed hold, a flush through any
operation from the first held draw on fails with the frame's failure, so no
later span releases.
The `native_frame_assembler_harvest` check replays each marked frame form
(`draw_before_clear = true`, see Frame forms) of a shader harvest on its
recorded guest state, after a warm-up epoch of the frame's own tiled part, and
again without its clear. It then replays named world and front-end frames
whole, as the draw, clear and copy hooks feed them, with a stand-in family that
admits each single-target draw at its decoded target and frame state, gives
scene pairs their captured geometry, and proves coverage with the scene
coverage below. It compares every refused operation and the swap's verdict
with a fixed list, then replays each frame without its refused operations and
compares the extents and samples of the resolves from overwritten sources. It
skips unless `REREVVED_WORLD_HARVEST` or `--root <harvest>` names the harvest.
The named frames are the known fixtures of each harvest the root holds, all
of which must be present. `rerevved_tests --helper native-frame-assembler.harvest [--root
<harvest>] [--fixtures <list>] [<fixture>...]` instead replays only the named
frame instances, or those a list file names one per line, and reports the
refusals of an instance without a fixed list.
Until the open
epoch's tiled scanout received every tile, any work on the scanned-out storage,
and any clear or draw of a set with extra targets, fails
`tiled_scanout_in_progress`; afterwards only a set with extra targets may
proceed.

Under predicated tiling the presented target is the logical 1280x720 screen.
The tile-0 predicated clear is its first presented work; every tile replays the
presented work before it while only tile 0 runs the clear, so a later one fails
`clear_form_unsupported: tiled_clear_after_work`. A predicated depth-only clear
fails `clear_form_unsupported: depth_clear_predicated <mask>`, and any other
predicated clear outside tiling fails
`clear_form_unsupported: predicated_untiled <mask>`. An unpredicated
depth-only clear of the whole presented depth surface splits the frame into
preserving passes. A stencil-only one may cover any rectangle of the logical
screen, which the host clears once, as every tile runs it through its window
offset; a smaller clear with depth fails
`clear_form_unsupported: depth_clear_rect <left>,<top>,<right>,<bottom>`. The
per-tile scanouts (`0x8250AFEC`) are one host-only scanout: each tile copies
its own rectangle to the same frontbuffer point with clear bits on every tile
but the last (`tiled_scanout_form`), and the swap fails
`tiled_scanout_incomplete: <received> of <count> tiles` until all arrived.
Tiles share one EDRAM, so tile i+1 starts from tile i's resolve clear: the
`RB_COLOR_CLEAR` bytes from red up, and `RB_DEPTH_CLEAR` bits 8..31 as depth
(the SDK's `UNorm24To32`) with bits 0..7 as stencil. `RB_COLOR_CLEAR_LO` is the
upper half of a 64bpp value, which a 32bpp target ignores. Where that start
differs from the tile-0 clear, the clear narrows to tile 0 and each later tile
gets its own clear; a pass without a depth surface models the color alone. A
segment that already submitted the whole-screen clear is replayed from the
epoch's start. A tile larger than the one whose resolve cleared it fails
`tiled_background_extent: tile <n> is <w>x<h>, tile <n-1> cleared <w>x<h>`.

Off-screen resolves of one RGBA8, RGB10A2 or `k_16_16` target, copied from
the target's origin to point 0,0, inside its modeled content and without
clear bits, render from that source, read back raw texels before gamma, and
encode the tiled guest image. RGBA8 reads back host bytes; RGB10A2 packs
`k_2_10_10_10` words for copy format 7 (the world's format-54 fetches), and
`k_16_16` packs `k_16_16_FLOAT` words for copy format 31. The host holds
`k_16_16`, whose range is -32..32, as `Rg16Snorm` at a 32nd of each value, so
a clear of such a target stores its color divided by 32 and clamped to -1..1.
A tiled `k_16_16` resolve fails `resolve_form_unsupported: tiled k_16_16`. A
2X source selects sample k0, k1 or k01 as host samples 0, 3 or both; a 1X
source copies sample 0. Other forms fail `resolve_form_unsupported` with what
differs: `format <format> targets <count>` (a set),
`rect <x>,<y>,<w>x<h> point <x>,<y> of <left>,<top>,<right>,<bottom>` (the
modeled rectangle) or `control <control> samples <count>`. A family's
`coverage` callback may prove the rectangle an admitted off-screen draw covers
with every sample, writing without kill. When that draw writes all four
channels of color 0 alone with no blend or ONE/ZERO, tests no depth or
stencil, and covers the resolvable samples, it defines an uncleared target
within the covered rectangle, its scissor and the target. Its resolves then
replay the draws from it without clears and must lie inside that rectangle.
A tiled single target, such as the world's k_2_10_10_10 pass at EDRAM 0,
may be defined the same way by a draw under tiling whose tile rectangles cover
the target's logical screen; each tile's resolve then replays the draws when
that draw covered the whole tile, with no background from an earlier tile.
Like a tiled MRT pass, it leaves the presented EDRAM needing full clears.
Without such a draw an uncleared target fails `resolve_source_unmodeled`. The
title's coverage is the scene family's, for the pinned pairs of the world
frame's full-target passes, including its k_2_10_10_10 tiled and ping-pong
passes, whose vertex shader computes the position as
c0..c3 times (attribute 0, 1) and whose pixel shader kills only under the alpha
test. It requires `RB_COLORCONTROL` (`G+0x293C`) to run no alpha test or alpha
to mask, and the recipe's six indices to place, through the constants,
half-pixel offset and viewport, two unculled triangles that split one
axis-aligned rectangle along a diagonal, with positive w and, under depth
clip, depth inside it. Corners snap to 1/256 pixel as the rasterizer snaps
them, and a pixel counts as covered only when the snapped rectangle, within
the viewport, holds its whole square, so any sample pattern lies inside.
The world fog composite (key 62663883, caller `0x8230686C`) is the one
blended overwrite: its vertex shader passes attribute 0 through as the clip
position, and its pixel shader writes alpha `(c255.x - t.w) * k + t.w` with
`k` in [0, 1]. With the c252..c255 literal record setting c255.x to 1 and
fetch 2's swizzle selecting 1 for W (`G+0x4BC` bits 10..12 equal to 5), that
alpha is exactly 1, so its SRC_ALPHA/INV_SRC_ALPHA blend replaces the target.
The coverage admits it only on a single-sample 1280x720 RGBA8 target with
that blend, all channels written, no depth or stencil test, no depth
surface, mode 4, no window offset and the 1280x720 window scissor; a pixel
counts when its center lies strictly inside the snapped rectangle. The
proof matches the recipe's vertex hash, which for this pair is the scene
catalog's `96DCE6C1AB07E076` of the linked, fetch-patched shader rather than
guest digest `F65866A40B248418`. An
untiled off-screen resolve of the presented target fails
`resolve_presented_unmodeled`. Under predicated tiling the presented target
can instead be a tiled texture pass, as the world scene's main pass is: its
per-tile resolves copy depth (`k_24_8`, sample select k1, no clear bits) and
then color (k01, clear bits on every tile but the last) to textures. At the
first one, the presented work since the epoch's tile-0 predicated clear
becomes the pass's source in the per-tile order above, with that clear on tile
0's rectangle, and leaves presentation. Any other presented operation after
that clear fails `tiled_texture_pass_form: operation <n> after clear <n>`; a
segment that already rendered the clear fails `tiled_texture_pass_segmented`;
a pass without the clear fails `tiled_texture_pass_uncleared`, one after the
epoch's tiled scanout `tiled_texture_pass_after_scanout`, and other tiles than
the clear's `tiled_texture_pass_tiles`. The presented color and depth are then
undefined until full presented clears, and a later tile-0 predicated clear
starts a new pass. Each off-screen resolve is its own
producer, keyed by its destination address and sequence with its own extent
and destination format. Resolves that write one address in other formats or
extents within a frame therefore never share a texture, and a second resolve
with no draw between replays the same source. The ledger
writes that image after the host fence and before later progress records. It
checks write access over the entire physical range, copies once, and publishes
the bytes before B+0 or RPTR. Close and ring teardown serialize with bulk writes.
Missing bytes fail with `resolve_readback_failed`; a source with skipped draws
is held as `resolve_source_partial`, including in skip mode.

A native render frame pass may carry up to three extra color attachments,
RGBA8 or RGB10A2, for multiple-render-target draws. Each draw names the same
extras with its own blend and write mask per target. Such a pass is never
presented or initialized from an aliased pass, and its clears and resolves
address only the bound slots. A resolve can pack its source into guest EDRAM
words in an `R32Uint` texture: 8:8:8:8 from an RGBA8 slot, 10:10:10:2 from an
RGB10A2 slot, two halves (red low) from an `Rg16Snorm` slot, or
`depth24 << 8 | stencil` from one depth sample. Color packing repeats the
float sequence of the SDK's `resolve_full_32bpp` shader, including its
two-sample average. For `k_16_16` that shader multiplies each signed 16-bit
code by `0x3A800100` (32 / 32767), clamps the result below at -1.0 rather
than -32, and stores `f32tof16` halves; the host repeats the clamp. A
prepared resolve of an `Rg16Snorm` slot into an `Rg16Float` texture, which
later passes sample, computes the same halves rather than storing the
unclamped scaled value with the target's own rounding. Packed words are never
sampled; they leave a retained
producer frame through raw readback.

The resolve encoder (`EncodeGuestResolve`) takes the host texels as rows of
32-bit words in the packing the copy names: RGBA8 bytes or `k_8_8_8_8` words,
`k_2_10_10_10` words, `k_16_16_FLOAT` halves, or `k_24_8` depth words
(`depth24 << 8 | stencil`). The
destination format (`RB_COPY_DEST_INFO` bits 7..12) follows the guest's
derivation from the fetch format (`0x8269E750..0x8269E7C0`): `k_8_8_8_8` words
need copy and fetch format 6; `k_2_10_10_10` words need copy format 7 with fetch
format 7 or 54 (`k_2_10_10_10_AS_16_16_16_16`); halves need copy and fetch
format 31; depth words need copy format 6 with fetch format 22 (`k_24_8`). Any
other pair fails `resolve_dest_format_unsupported`, and `k_24_8_FLOAT` (23) is
refused until seen. The copy's number format must be 7 (float) for halves, as
the fog copies name it, and 0 otherwise
(`resolve_number_format_unsupported: <number> for dest <format>`); a nonzero
exponent bias fails `resolve_exp_bias_unsupported: <bias>`. Halves refuse the
red/blue swap (`resolve_swap_unsupported: dest 31`), which would move red into
the empty blue. A float fetch may name its components unsigned or all signed
and may set the integer number format, as the fog fetches do; any other sign,
and for the other formats any sign or integer format, fails
`resolve_fetch_sign_unsupported: <dword 0> <dword 3>`. A fetch with a nonzero
minimum mip, or a packed base level (the packed bit
with the smaller dimension at most 16), is refused. Only the base level is
written, so the mip address is never used. The fetch base (dword 1) is
physical below `0x20000000` or an alias in the `0xA0000000` or `0xE0000000`
view, translated as Resolve does (`0x8269E6DC..0x8269E714`) and as the SDK's
physical heaps do: minus the view base, plus `0x1000` for `0xE0000000`. It
must equal `RB_COPY_DEST_BASE`. Any other base, including the `0xC0000000`
view, fails as not physical or in a modeled view. The SDK widens a resolve
rectangle to 8-pixel multiples and writes every widened texel from EDRAM, so
a rectangle origin or destination point that is not a multiple of 8 fails
`resolve_rect_unaligned`, as does an unaligned width or height unless that
edge ends the destination's width or height. The widened texels then fall in
tiled padding, inside the 32-texel pitch and tile height, that no fetch of
the destination samples, and they are left unwritten: the Hall of Glory
panels resolve 900x675 of their 928x688 surface, which the SDK widens to
904x680, while the guest clear and draws define only 900x675, and world
frames copy 320x180 targets, widened to 320x184. Source
select 4 pairs only with depth, and color uses 0..3. The host has already
selected and averaged the samples, so the encoder only requires sample selects
k0..k3 (and k01 for color); k23 and k0123 are refused. The red/blue swap exchanges
bits 0..9 with 20..29 for `k_2_10_10_10`, bytes 0 and 2 for `k_8_8_8_8`, and does
nothing for depth. Host texel (x, y) lands at destination (destX + x, destY + y);
destination padding is allowed and never written. The image covers the base
through the last touched 32x32 tile, and `runs` lists the written texels as
maximal byte spans. A whole unpadded destination has no runs and is written
whole. With runs the ledger writes each run at its physical address, in order,
and a failing run fails the record as `resolve_readback_failed`; earlier runs
stay written.

The ledger writes an off-screen resolve's image only when it releases the
span, but later draws of the same frame may sample the destination, as the
world scene's pass 4 samples pass 3's depth. The families therefore read guest
physical memory through `NativeFrameAssembler::ReadResolved`. A read
overlapping an off-screen resolve destination whose image the ledger has not
written submits that resolve's producer frame or graph segment, peeks its
encoded image without taking it (`PeekGuestImage`), and lays the image's runs
over the guest bytes in op-mark order, so a later resolve to the same bytes
wins. The ledger keeps a copy of each image it takes laid over memory until it
reports the image written (`GuestImageWritten`); from then on guest memory, including any
later guest write, serves the destination. An image reported written while a
read is in progress is not laid over that read's bytes; the read rereads guest
memory without it. A read that finds the image already
taken from the source waits for that take. A discarded span's image is
dropped. While 256 off-screen resolves await the ledger, another one fails
`resolve_images_unwritten: <n> off-screen resolves await the ledger`. A failed
read logs `resolve_image_unsubmitted: op mark <n>` or
`resolve_image_unavailable: op mark <n>: <reason>` and returns no bytes, so the
draw's reader refuses it. The read waits at most 2 s for a retried submission
and for each readback; a submission blocked inside the completion source, or
behind a swap holding the submission lock, is not bounded. Texture caches are
keyed by content, so a new image decodes as a new texture.

The copy words `G+0x2A2C`, `G+0x2A30` and `G+0x2A34` are `RB_DEPTH_CLEAR`,
`RB_COLOR_CLEAR` and `RB_COLOR_CLEAR_LO`; the tiled scanout log prints the first
two as `resolve_clear=<depth>,<color>`.

In a developer build, the bring-up `--native_resolve_derive_check=true` observes only; the Original
bodies keep running. For each `Resolve` call (`0x8269E520`) whose entry the
observer admits (mip 0, slice 0, a decodable source set), it derives the four
copy words on the host from the entry registers, the device and the
destination header, before Original runs: `copyControl` from the flags (with
the fragment-select fixup from the source surface's multisample count and the
source select 3, 0 or 1), `destBase` from the header base, the point and
rectangle offsets and the size field of the per-format table at `0x8208EA10`,
`destPitch` from the point, rectangle and header extent, and `destInfo` from
the mapped destination format, number format, flag bits 26..31, swap and endian
bits. At the copy hook `0x8269E8A0` it compares them with `G+0x2A18..0x2A24`.
The clear words `depthClear`, `colorClear` and `colorClearLow` are not derived:
the body stores them after that hook, when the flags select a depth (`0x200`)
or color (`0x100`) clear, so the hook reads the previous resolve's. At the
return `0x8269F360` the probe reads `G+0x2A2C..0x2A34` again and counts a
difference as stale. The exit state of the effect probe below derives those
words. The `Native frame timing:` line of a developer build adds `resolve_derive_ok`,
`resolve_derive_mismatch_copy_control`, `resolve_derive_mismatch_dest_base`,
`resolve_derive_mismatch_dest_pitch`, `resolve_derive_mismatch_dest_info`,
`resolve_derive_unsupported` (a call or word the derivation does not model),
`resolve_derive_unpaired`, `resolve_clear_words_checked` and
`resolve_clear_words_stale`. Resolve can re-enter itself through the
command-buffer allocator callback, so
the probes of calls in flight stack per thread (four deep). The entry observer
pushes one for every call with its entry `r1`; both hooks read `r1` and find
their call by the body's frame, `r1` less 368 at `0x8269E8A0` and
`0x8269F360`, so a call the observer rejects never touches another call's
probe. Every path of the body reaches the return hook. A probe still stacked
above the matching one when a hook runs, or below the entry `r1` of a new
call, belongs to a call that left without reaching it; it is dropped and
counted in `resolve_derive_unpaired`, as is a call that finds the stack full
and a copy hook whose device differs from its probe's. A call counts ok
when all four words match, and under each differing word otherwise. The first
eight mismatches log the entry registers with the derived and guest values of
each differing word; the first eight unsupported calls log their reason, and the
first four stale clear words log both readings.

In a developer build, the bring-up `--native_clear_derive_check=true` observes only as well. For each
`Clear` call (`0x826A2970`: `r3` the device, `r4` the flags, `r5` a rectangle
or 0, `r6` a color pointer or 0, `f1` the depth, `r8` the stencil) it derives
on the host, before Original runs, the ordered list of per-target clears the
body `0x826A22F8` makes to `0x826A1D90`, and compares it with the calls the
hook on that function records (`r4` flags, `r5` the color slot or -1 for depth
only, `r6..r9` the left, top, right and bottom, `r10` the color pointer, `f1`
the depth and the stencil word at `r1+92`). A null rectangle is `{0, 0, width,
height}` from the extent word at `+0x24` of RT0 (`G+0x3098`), or when RT0 is
null of the depth surface (`G+0x30A8`), never of another color slot; the width
and height are the fields in bits 18..31 and 3..17, plus 1. It becomes the
screen size `G+0x337C` by `G+0x3380` when byte `G+0x2ABC` has `0x10`, or has
`0x20` and every bound slot RT0..RT3 and depth still equals the surface
`BeginTiling` saved at `G+0x31B8..0x31C8` (an unbound slot matches). A rectangle
the caller passes is used as it is. Without a depth surface the flags lose
`0xF0`, and a body left with no flags makes no call. The rectangle is then
clipped, as signed words, to the viewport floats `G+0x3168..0x3174` (x, y,
width, height after a truncating `fctiwz`: NaN and values below -2^31 give
INT_MIN, 2^31-1 and above INT_MAX, and x plus width wraps at 32 bits) and, when
`G+0x2E50` is nonzero, to the scissor words `G+0x3184..0x3190`; an empty result
makes no call. Flags with no color bit (`0xF`) make one call with slot -1;
otherwise each set bit whose slot `G+0x3098+4k` is bound makes a call, in slot
order, with `0xF0` cleared after the first call made. A null color pointer
becomes the body frame's `r1+112`, where the body copies the four words at
`0x8208EBE0`, so that call compares the words and not the address. The color
words are compared only for a call that clears color. Whether the guest word at
`0x82F1AC14`, which `0x826A1AF0` tests, is zero cannot change these calls: the
hook runs ahead of it. The derivation refuses, counted as
`clear_derive_unsupported`, any unreadable guest word, an address that would
wrap, a null rectangle with no bound target, and a null color pointer in a
frame the body's aligned 16-byte copy would not fill. The `Native frame
timing:` line of a developer build adds `clear_derive_ok` (every list matches),
`clear_derive_mismatch_count` (the lists differ in length),
`clear_derive_mismatch_index`, `_flags`, `_rect`, `_color`, `_depth` and
`_stencil` (any paired call differs in that field; each counts once per Clear
call), `clear_derive_unsupported` and `clear_derive_unpaired`. The body opens
frames of 112 (the wrapper) and 256 bytes, so its `r1` is the entry `r1` less
368; the hook on `0x826A1D90` reads it as its own `r1`, and a hook on the body's
single exit `0x826A2960` (reached by every path, early returns included) reads
it before the frame closes. The probes of calls in flight stack per thread (four
deep) and each hook finds its call by that frame, as for Resolve: a probe still
stacked above the matching one, or below the entry `r1` of a new call, belongs
to a call that left without reaching its exit; it is dropped and counted in
`clear_derive_unpaired`, as is a call that finds the stack full and a hook whose
device is not the probe's. The first eight mismatches log the entry registers
with both lists, and the first eight unsupported calls log their reason.

In a developer build, the bring-up `--native_resolve_effect_check=true` observes only too. For each
`Resolve` call the entry observer admits it predicts on the host, before
Original runs, the guest words the body changes besides the copy words, reads
the watched words at the entry, and reads them again at the return
`0x8269F360`, which the same per-thread probe stack pairs by the body frame
(`r1` less 368). The prediction: `G+0x2A18` and `G+0x2A24` are zero at the
return (the exit stores) and `G+0x2A1C`, `G+0x2A20` hold the derived base and
pitch; `G+0x2A44` is pitch times height, 32 bits, on the 11-bit extent form
(dword 5 bits 9..10 equal to `0x400`) and unchanged otherwise; the byte
`G+0x2ABE` has `0x08` clear and, with flags `0x200`, `G+0x2ABD` has `0x04`
set. The header words follow the fence `G+0x2A9C`, which the body reads and
stores to `dest+8` early, then reloads once at `0x8269F2F4` (the branch-time
read, not the return snapshot) to pick the path. Between the two the ring space
wait through `0x8269C588` moves the fence by two, and the list path's own
allocation can move it again after the branch, so the fence at the return is
only the end of a sequence. Let S be the fence values from the entry reading to
the return reading in steps of two, wrapping through zero. (a) A nonzero
reload takes the header path, which stores it to both words and goes straight
to the return hook with no call between, so `dest+8` and `dest+12` both equal
the nonzero return fence. (b) A zero reload takes the list path,
which never stores `dest+12`: zero is in S, `dest+12` is as at the entry, and
`dest+8` holds a value of S at or before that zero (its store precedes the
reload). Any other pair, such as both words left at the entry fence after it
moved, or `dest+8` at the entry value with `dest+12` unchanged and no zero in
S, is a `dest` mismatch.
The 64-bit dirty words `G+0x10`, `G+0x18` and `G+0x20` are written by ORing,
and the body's flush groups clear `G+0x10` bits 0..16 and 21..57, `G+0x18` bits
34..54 and `G+0x20` bits 0..37 ahead of its restore block, so the bits ORed
into `G+0x20` before the flush (4 on the large form, `0x3C00`, and `0x300` or
`0xC0` from the clear blocks) are gone at the return and are not predicted.
What survives is set before the allocator call `0x8269B818`: `G+0x10` bits 5..7,
39..41 and 44, bit 17 with flags `0x200`, and bit `56-k` when dword 0 bits 2..3
equal 3, the source is not the depth slot and the render target shadow word
`G+0x2884+4*k` has bits 16..19 equal to 1 (`k` is 0 for slot 0 and the slot
plus 1 otherwise); `G+0x18` bit 35; `G+0x20` bit 19. After the allocator call
the body ORs `0x98908` into `G+0x10` and bit 31 into `G+0x18` and stores the
header words. A failure of the body's allocator call (the marker `G+0x2ABD` bit
`0x20` already set at the entry, where the allocator returns 0 at once, or set
at the return with `G+0x2A18` still holding the copy control) leaves the body
there: the post-allocator bits, the exit stores and the header words are absent,
so `G+0x2A18` and `G+0x2A24` keep the copy words. The marker at the return alone
does not tell which allocation failed: the list path's own allocation
(`0x826A6958`) can fail after the body's succeeded and set it, leaving the
success shape, with `G+0x2A18` zero; that call counts in
`resolve_effect_alloc_failed` and is compared as a success. Each dirty
bit class is compared on its own: a predicted bit clear at the return is
`set_missing`, a bit set at the return that was clear at the entry and is not
predicted is `extra_set`, and a bit set at the entry and clear at the return is
a flush, counted and never a mismatch; a flush that clears any of
`G+0x10` mask `0x1E0000` is counted apart. This comparison does not look at the
ring cursor `G+0x30`, ring memory or the allocator pool words; the exact
comparison below reads the cursor, the ring limits and the pool words. The `Native frame timing:`
line of a developer build adds `resolve_effect_ok` (no field differs),
`resolve_effect_mismatch_<field>` for `copy_words`, `size`, `dirty_set_missing`,
`dirty_extra`, `bytes` and `dest` (each counts once per call),
`resolve_effect_dirty_cleared` (bits),
`resolve_effect_cleared_1e0000`, `resolve_effect_alloc_failed`,
`resolve_effect_list_path`, `resolve_effect_unsupported` (a call the
derivation does not model, or an unreadable word) and
`resolve_effect_unpaired` (probes dropped before their return hook, as for
`resolve_derive_unpaired`; a dropped probe counts under each check that was
enabled at its entry, so a probe stacked for one check never counts under the
other). The first eight mismatches log the entry
registers, the predicted and actual values of each differing word, the
missing and extra dirty masks, and the dirty words at both readings; the first
eight unsupported calls log their reason.

The same probe also compares the exact exit state of the call, the one
`--native_consume_resolve` writes. The prediction is made from the entry state
alone, so a call that flushed, waited for ring space or failed its allocation
mismatches it. Every watched word at the return is a function of the entry: each
dirty word is `(entry & ~flush_clear) | set`, where `flush_clear` is the bits
the flush groups drop (`G+0x10` all but bits 17..20 and 58..63, `G+0x18` bits
34..54, `G+0x20` bits 0..37) and `set` is the bits ORed before and after the
allocator call, because each flush group tests its own bits and clears them
whatever `G+0x28` holds. The first callee `0x826AC8C8` does not return its dirty
argument unchanged: it returns it in r24 with bit 8 cleared on its flush branch
and bits 37 and 57 cleared when it emits their blocks (g202:13407-13410,
13436-13439, 13471-13474, 13598-13599), and the body stores that to `G+0x10`
(g167:13664). Those three bits lie inside the flush mask, which the groups that
follow apply to the stored word, so the clear holds with or without the call.
The later callees write only ring words and `G+0x30`. The copy words are zero,
base, pitch, zero; `G+0x2A44` is pitch times height on the large form and
unchanged otherwise; `G+0x2ABD` is unchanged except for flags `0x200`, which set
its bit `0x04` (g167:14299-14311); `G+0x2A28..0x2A34` are unchanged except for the
words of the clear blocks, below; `G+0x2ABE` has `0x08` clear;
`dest+8` and `dest+12` are both the fence at the ENTRY (g167:14928, 15112,
15114), because only a call that flushes moves `G+0x2A9C`, by two through
`0x8269C588` (g202:12873). The allocator effects are part of the exit, below.

Clear blocks. The body calls `0x8269DF00` for flags `0x200` and `0x8269E448`
for flags `0x100` after the copy hook (g167:13519-13545), so the hook reads the
previous call's `G+0x2A2C..0x2A34`; the consume gives it the words at the entry,
as the entry hook would read them, and the host's clears and tile backgrounds
come from those words exactly as they do for Original. The blocks store
`G+0x2A28` and `G+0x2A2C` (`0x8269DF00`, g50:12389-12552) and `G+0x2A30` and
`G+0x2A34` (`0x8269E448`, g209:13049-13171), and OR `0x300` and `0xC0` into
`G+0x20` (g50:12537, g209:13162). Both masks lie inside the `G+0x20` bits the
flush groups clear, so they are gone at the return and only widen the ring bound
below. The blocks store nothing else outside their frames, and emit no ring
word. The exit state derives the four words exactly from the entry registers,
the caller's stack words (the stencil value at the entry `r1+92` and the
parameter block pointer at `r1+100`, g167:13519, 13539), `f1` as the depth, the
device words and the image tables, all read from guest memory:

- `G+0x2A2C` is the packed depth and stencil word and `G+0x2A28` the control
  word. The depth format is the halfword table entry (`0x8208EA10`) of the
  parameter block's dword 2 when a block is passed, else the device halfword
  `G+0x2888`. For the integer layout the packed word is the depth times
  `0x8208EB94` truncated, shifted left 8, with the stencil, and the control
  word is the depth times `0x8208EB90` truncated. For the float layout (any of
  `0xF00` in the table entry, or bit 0 of the device halfword) `0x8269DE98`
  (g67:13022-13084) converts the depth to a 24-bit float, and the control word
  is bits 21..31 of the packed word. Bit 11 of the control word is set for a
  stencil value other than the byte `G+0x2942`, xored with bit 5 of `G+0x2940`
  being clear (g50:12510-12531).
- `G+0x2A30` and `G+0x2A34` come from `0x8269E020` (g125:13208-13890) for the
  clear color, the 16-byte vector holding `r10` or the default at `0x8208EBE0`
  for a null `r10`. The signed exponent bias (bits 20..25 of the target's shadow
  word `G+0x2884`, or `G+0x288C`, `G+0x2890` and `G+0x2894` for targets 1..3; with
  a parameter block, its dword 1) is added to the exponent of each channel
  with an exponent of 1..254, clamped to that range (g125:13242-13306). The
  format is bits 16..19 of the shadow word, or the table entry of the parameter
  block's dword 0 (`54` reads entry 7). Formats 0, 2 and 10 (10 reads row 2)
  clamp each channel to the largest value its width holds (the byte tables
  `0x8208EBD0` for alpha and `0x8208EBD4` for color, less `0x820068A8`), round it
  to the nearest integer with ties to even and pack the channels; both words are
  the packed value. Formats 14 and 15 return the channels as floats: 14 returns
  channel 0 in both words, 15 channel 1 and then channel 0. With a parameter block,
  the low word becomes the high one when the map byte `0x8208EB50` of the shadow
  word's format names a table entry whose low byte exceeds the block entry's.
  Any other format, and a NaN channel in the integer formats, is not modeled:
  the call refuses as `clear_color`, since the shadow words would be left stale.
  The body stores `G+0x20`, the copy words `G+0x2A18..0x2A24`, the size `G+0x2A44`
  and its stack frames (the body's and its callees', under 0x400 bytes below the
  entry `r1`) before the blocks run, and the depth block stores `G+0x2A28` and
  `G+0x2A2C` before the color vector load and the color block read. Each block
  loads its inputs from `G` and the parameter block before its own stores to `G`.
  A clear input (the color vector, the parameter block or a table entry) that
  overlaps a word stored earlier than its read would be read after Original
  overwrote it: such a call refuses as `clear_overlap`.

Ring space. Every ring check of the body compares the cursor `G+0x30` with the
soft limit `G+0x38` and calls the flush `0x8269C588` once past it (g167:13972,
14073, 14133, 14353, 14560, 14614, 14846, 14982; g202:13043, 13278; g153:12660,
12968), and `0x826ACC60` compares the end of a run with the hard limit `G+0x34`
and calls `0x826AC610` when it does not fit below it (g71:12189-12203). The
consume keeps the entry fence and skips the kick, so it is exact only when
Original cannot flush. The cursor only grows through a call, so Original cannot
flush when `cursor + 4 * W <= G+0x38'` and `cursor + 4 * W < G+0x34'`, where
`W` is the most words the call can emit and the primed limits are those the side
buffer allocation leaves. `W` counts every optional block with `n` the render
target count `G+0x31CC` (at most 16, else unbounded): `2 * (32 + 5n)` for
`0x826AC8C8` called twice (g202:13191-13594), `2 * (12 + 7n)` for `0x8268D3A0`
called twice (g153), `6 + 5n` for the render target loop (g167:13967-14126),
85 for the straight-line emits (g167:14127-15100), and for the flush groups,
which are separate `0x826ACC60` calls, the popcount plus the number of runs of
each group's entry dirty bits (each run already counts its header word):
`G+0x10` bits 0..11, 12..16, 21..41
and 42..57, `G+0x18` bits 34..54 and `G+0x20` bits 0..37 including the `0x3C00`,
on the large form the `4` and, for
flags `0x200` and `0x100`, the `0x300` and `0xC0` the clear blocks OR in (g167:13476,
13530-13534; g50:12537; g209:13162; g71:12148-12330). For an entry
without them, `0x300` joins the run of the `0x3C00` (+2 words), `0xC0` forms a run
of its own (+3) and both add 4.

Allocator. `0x8269B818` dispatches on `G+0x34CC` and the marker `G+0x2ABD`
(g93:12757-12887). A pool object without a fixed buffer (`+152` zero) is
`0x826A6AB8` (g69:12999-13072), which calls an indirect allocator the host does
not model: it refuses as `alloc_path`. A null pool object is `0x8269B618`
(g185:12899-13183), called with `r4 = 1` and a 4-byte alignment. It starts at
`G+0x3A44`, or at `G+0x34 + 60` while that is zero, rounds up to 4 and takes 24
bytes. When the end is at most the end of the side buffer area `G+0x3A3C` it
stores the end to `G+0x3A44` (forward). Otherwise it takes the 24 bytes at
the wrap position `G+0x3A4C`, fails when that end passes `G+0x3A50`, and stores
it to `G+0x3A4C` (wrap); `G+0x3A48` is not stored. With the bracket `G+0x33B0`
set it also fails when `G+0x33B4 - G+0x3A48` (less 1 on a wrap) is -1, or is 0
with `G+0x33B0` below the end; the bracket terms apply only to the bracket form
of a tiled call, the one tiled shape the exit admits (below). The sums
are 32 bits, and a buffer address of zero is a failure to the caller. Neither
path moves `G+0x34` or `G+0x38`, so the ring words are judged against the limits
as they stand. The take then calls `0x8269B158(G, end, n)` with `n = G+0x3A48`
(one more on a wrap), which returns at once when `(n - v) & 3` is 0, or is 1 with
`end <= v & ~3`, for `v` the word at `[G+0x2A90] + 4` (g206:12421-12513).
Otherwise it spins on the GPU, and the call refuses as `alloc_wait`; a null
block pointer at `G+0x2A90` is not a word the host can read. On a failure the
call refuses as `alloc_capacity`: the body keeps the copy words in
`G+0x2A18..0x2A24` and skips the exit stores, so success must be provable before
a call is consumed, and a marker that is clear at the entry does not prove it.
With a fixed buffer `0x826A6848` takes 24 bytes
from the top of the pool, `G+0x3A44` once set, else `+152` plus the size `+156`
rounded down to 4, at `(top - 24) & ~3`, and returns zero when that is below the
cursor plus 164 (g100:13277-13287). On success it stores the address to
`G+0x3A44` and lowers both `G+0x34` and
`G+0x38` by `(top - address) & ~3` (g100:13288-13308; 24 for an unaligned top
as well); a limit below that wraps in
Original and refuses. `0x8269B818` then adds 24 to `G+0x3A54` on every path and,
when the sum passes the threshold `G+0x3A58` with the bracket `G+0x33B0` clear,
moves `G+0x34` to the cursor plus 160 and `G+0x38` to the cursor
(g93:12826-12865), so the next ring check flushes: that call refuses as
`ring_space`. The fixed pool's capacity test is
`(top - 24) & ~3 >= cursor + 4 * W + 164`.

Tiling. The device carries three tiling words: the byte `G+0x2ABC >> 24`, the
predicate `G+0x31AC` and the bracket word `G+0x33B0`. BeginTiling stores the
bracket word from `G+0x33B8` and EndTiling clears it (g123:12809-12817,
g12:12984-12988); the API `0x8269CE90` sets byte bit `0x20` before calling
BeginTiling and `0x8269D320` clears it (g76:13421-13447, g70:13136-13151).
SetPredication stores the predicate `G+0x31AC` (1 for a nonzero mask, else 0)
and the mask `G+0x31A4` (g171:12584, 12708, 12717, 12721) and `0x8269D320`
clears it (g70:13146), so a Resolve inside the tile loop of a frame carries it.
The loop is the game function `0x8250AC90` (g205:8780-9339): for each tile it
calls SetPredication with `3 << 2i` when the tile count is above 1 (g205:8896),
then issues the depth Resolve with flags 4 (g205:9005-9007) and one Resolve per
color target with the slot and, from `[r31+544]`, the clear bits `0x100` and
`0x200` (g205:9172-9264); after the last tile it calls SetPredication(0)
(g205:9309) and EndTiling (g205:9339). The body
tests tile mode as byte bit `0x10`, or bit `0x20` with every bound surface
`G+0x3098 + 4i` equal to the BeginTiling snapshot `G+0x31B8 + 4i` or zero (and,
for `0x826AC8C8`, bit `0x20` of the byte `G+0x2ABF`; g167:13868-13965,
g202:13082-13186, g153:12670-12769). When that holds it only selects extra ring
words (g167:13966-14126, g202:13207-13297, g153:12839-12994) and the final
header word ORs the predicate into a ring word (g167:14825-14833). No other word
of the body, `0x826ACC60` or the allocator reads or stores the tiling byte, the
predicate or `G+0x31A4`; the bracket word is read only by the allocator
(g93:12840-12841, g185:12955-13124). The only other loads of the mask `G+0x31A4`
and of `G+0x31A8` in the direct call closure of the body are ring words
(g167:14110-14120, g153:13067-13079, g202:13353-13363, 13580-13590), and
SetPredication changes the tiling byte only in bit 0, which no function of that
closure tests: the byte tests are bits `0x10`, `0x20` and `0x40`, and the flush
callees test the mask `0xFFFFFF80` (g58:12963, g202:12906, 12961). The indirect
calls of the closure are method calls on the pool, wait and diagnostic objects,
none of them the device. The ring bound above already counts every
tiled word, so a tiled call needs no other term, and the tiled body writes no
device word an untiled call does not. The tiled-only effects on the exit are
the bracket terms of the allocators above: the null pool allocator's failure
test, and the skipped forced flush past the threshold on every path. The words
are classified in this
order: byte bit `0x10` is `tiling` (a setter of that bit was not found among the
byte's store sites, so the shape is unmodeled); a nonzero predicate with byte
bit `0x20` and the bracket word both set is the predicated bracket form, and
any other nonzero predicate is `tiling_predicate`; byte bit `0x20` clear with
the bracket word set, or set with it clear, is `tiling_state` (BeginTiling sets
both and EndTiling clears both, so a disagreement is a state the host has not
seen); otherwise byte bit `0x20` with the bracket word set is the bracket form,
and neither is untiled. The bracket form and the predicated bracket form are
modeled alike: the predicate reaches no word of the exit, so the exit of a
per-tile Resolve is the exit of the same call with the predicate clear. The
tile count is not bounded by the classification: more than 16 is `ring_space`.
The probe counts a modeled bracket call under `resolve_effect_exact_tiled` and
an exact one under `resolve_effect_exact_tiled_ok`; an exact bracket call also
counts in `resolve_effect_exact_ok`, and a mismatching one in the mismatch counts.
The predicated ones also count in `resolve_effect_exact_predicated` and
`resolve_effect_exact_predicated_ok`. The shape of every predicated bracket call
the probe classifies, modeled or not, is counted apart so that a refusal does
not hide it: `resolve_effect_predicated`, `resolve_effect_predicated_clear` (flags
`0x100` or `0x200`) and `resolve_effect_predicated_tiles_<2|3|4|other>` by the
tile count `G+0x31CC`. A call whose entry or return state cannot be read or
derived is not classified and counts only as unsupported.

The exit is defined only for the header path with a successful allocation and no
possible flush or wait: a nonzero fence, a clear marker, no tiling or its bracket
form (predicated or not), clear inputs the body does not overwrite, a clear color the host models,
the allocator path above, and the ring and capacity bounds above. It carries 21
words: the 12 above plus `limit_34`,
`limit_38`, `pool_pos`, `pool_used`, `wrap_pos` (`G+0x3A4C`) and the four clear
words. The refusals are tested in that order and named `fence`, `alloc_marker`,
`tiling`, `tiling_predicate`, `tiling_state`, `clear_overlap`, `clear_color`, `alloc_path`,
`ring_space` (the ring bound, a limit below the 24 bytes, the cursor at the top
of the address space, or the side buffer count passing its threshold),
`alloc_capacity` (the allocation would fail) and `alloc_wait`. A call whose
stack reach `[limit, r1 + 104)` holds a device word or an object the Resolve model
touches is skipped first, as `stack_overlap`, at the entry, before any guest word
of the probe is read: neither the effects nor the exit state are derived. The line adds
`resolve_effect_exact_ok` (every word equals the exact exit),
`resolve_effect_exact_tiled` and `resolve_effect_exact_tiled_ok` (the bracket
form calls modeled and exact), `resolve_effect_exact_clear` and
`resolve_effect_exact_clear_ok` (the calls with flags `0x100` or `0x200`
modeled and exact),
`resolve_effect_exact_mismatch_<word>` for `copy_control`, `dest_base`,
`dest_pitch`, `dest_info`, `size`, `dirty_10`, `dirty_18`, `dirty_20`,
`alloc_byte`, `state_byte`, `dest_8`, `dest_12`, `limit_34`, `limit_38`,
`pool_pos`, `pool_used`, `wrap_pos`, `clear_control`, `clear_depth`, `clear_color` and
`clear_color_low`, `resolve_effect_exact_skipped` (every call the exit
refuses, and every unsupported call) and `resolve_effect_exact_skipped_<reason>`
for each refusal reason. The exact counters must read zero mismatches in a
census with the probe on before the consume is run.

`--native_consume_resolve` is on by default. With `native_ring_stubs`, it skips
Original of `Resolve` `0x8269E520` on the calls whose exit the host knows exactly
and replays what the Original-only hooks would have done. The call is consumed
when ring stubs have checked the device; no other `Resolve` of this thread is in
flight; a ledger and an assembler are published; the entry observer opened a
valid record that still awaits its copy words; no derive or effect probe, shader
harvest, scene capture or route trace of the call is on (each
needs the body or its mid-body hooks); the thread's stack reach `[limit, r1 + 104)`
holds no byte of the device words `[G, G+0x3A5C)` or an object the model reads
(`stack_overlap`, tested before any other guest word is read) and its bounds are
known; the tiling words are untiled, or, with
`--native_consume_resolve_tiled=true`, the bracket form (`G+0x2ABC & 0x30` is
exactly `0x20` and `G+0x33B0` is set), with `G+0x31AC` clear or set to the mask
of a per-tile Resolve; the derivation models the call,
which for flags `0x100` or `0x200` needs the caller's stack words; `G+0x2A9C` is
nonzero; `G+0x2ABD & 0x20` is clear; no clear input overlaps a word the body
stores before the blocks (`clear_overlap`); a color clear has a format the host
models (`clear_color`); the allocator path is modeled (`G+0x34CC` null, or nonzero with a fixed
buffer); Original cannot flush (`cursor + 4 * W <= G+0x38'` and `< G+0x34'`, and
the side buffer count stays within `G+0x3A58`); the allocation provably succeeds
(a fixed buffer holds the side buffer, `(top - 24) & ~3 >= cursor + 4 * W + 164`,
or the null pool allocator finds its 24 bytes); and the null pool allocator does
not wait for the GPU. The first condition that fails, in that order, is the
reason; a word that cannot be read, among them the progress word behind
`G+0x2A90` of a null pool object, is `unreadable`.
A bracket form call is replayed as an untiled one: the entry observer captured
its tiling words with the draft, so the assembler takes the same tiled target as
from Original, and the exit differs from an untiled call only by the allocator's
bracket terms already judged. The ring words and the cursor are not written, as
for the tiled draws. The entry observer reads the predicate and its mask with the
draft, and the assembler's per-tile Resolve identifies its tile from the mask
`3 << 2i`, so a consumed per-tile call reaches it exactly as from Original.
With the cvar off the predicated bracket form stays `tiling_predicate`. Any
other tiled shape runs Original under `tiling`, `tiling_predicate` or
`tiling_state`.
The replay makes the renderer observation of
the entry hook (`ObserveRendererResolve`), the ledger and assembler work of the
copy hook `0x8269E8A0` with the copy words the host derived and the clear words
`G+0x2A2C..0x2A34` it reads at the entry (the words the hook reads in Original),
the probe bookkeeping of the return
hook, and then the exact exit state as big-endian guest stores, only for the
words that change. Every store address is checked writable first, and a failure
runs Original. It changes no register, so r3 and the callee-saved registers keep their entry values.
The allocator `0x8269B818` is not called, but its successful effects are stored.
A fixed pool lowers `G+0x34` and `G+0x38` by the bytes it took and sets `G+0x3A44`
to the new buffer top. A null pool object stores the end of its take to
`G+0x3A44` (forward) or `G+0x3A4C` (wrap) and leaves both limits. Either grows
`G+0x3A54` by 24. The side buffer's
contents are not written, and the ring words and the cursor `G+0x30` are not
written, as for every consumed draw. The fence `G+0x2A9C` is left alone and
both header words take its entry value.
In a developer build the `Native frame timing:` line adds `resolve_consumed`, `resolve_consumed_tiled`
when nonzero (the bracket form calls, which `resolve_consumed` includes),
`resolve_consumed_predicated` when nonzero (the predicated ones, which both
include), the shape of the predicated calls seen when there were any
(`resolve_predicated`, `resolve_predicated_clear`,
`resolve_predicated_tiles_<2|3|4|other>`, counted at the classification whatever
refuses the call later) and, for each nonzero reason,
`resolve_fallback_<reason>` with `stubs`, `nested`, `ledger`,
`entry_state`, `diagnostics`, `stack_overlap`, `tiling`, `tiling_predicate`, `tiling_state`,
`derive_unsupported`,
`fence`, `alloc_marker`, `clear_overlap`, `clear_color`, `alloc_path`,
`ring_space`, `alloc_capacity`,
`alloc_wait` and `unreadable`. With the cvar off or the ring stubs unpublished
nothing is counted.

In a developer build, the bring-up `--native_clear_effect_check=true` observes only too. For each
`Clear` call it predicts on the host, before Original runs, the exit state of
the 64-bit dirty words `G+0x10`, `G+0x18`, `G+0x20` and of the byte `G+0x2ABD`
as `(entry & keep) | set`, reads those words at the entry, and reads them again
at the exit `0x826A2960`, which the per-thread probe stack of
`native_clear_derive_check` pairs by the body frame; the two checks share the
stack and each counts its own dropped probes. Past its early exits (no depth
surface with only depth flags, or an empty clipped rectangle: both leave every
watched word unchanged, `clear_effect_early_ok` or `_early_changed`) the body
runs its flush groups, even when the color flags have no bound slot and no call
follows (`clear_effect_flush_no_calls`). The flush keeps `G+0x10` bits 17..20
and 58..63 (it clears 0..11, 12..16, 21..41 and 42..57), `G+0x18` outside bits
34..54 and `G+0x20` bits 38..63; `0x826AC8C8` only clears bits 8, 37 and 57 of
the value it returns, inside those groups. The ORs that follow need no
allocation: the prelude (`G+0x10` bits 39, 40, 41 and 44, `0xE0`, `0x80000` and
`0x100000`; `G+0x18` bit 35; `G+0x20` `0x80000`), the color branch (`G+0x10`
bit 37, `0x200` and `0x400`, taken for any of the flags `0xF`), the exit
(`G+0x2ABD` `0x04`, `G+0x10` `0x20000`) and those of `0x826A1D90` ahead of its
allocation: `G+0x10` `0x10000000` for a call with the flag `0x20`, and `0x100`,
bit 55 and `0x10000000` on its fast path (flags without `0x30`, a color format
other than 5, 7 and 15 or the depth-only call, and a rectangle of at least
80x16, 80x8 or 40x8 by `(G+0x2880 >> 16) & 3`). After a successful allocation
of `0x826A1800` the call sets `G+0x10` bits 56 and 57 and `0x808`, `G+0x18` bit
31 and `G+0x20` bit 36. Every call of `0x826A1AF0` makes at least one call of
`0x826A1800`, so a call of `0x826A1D90` allocates unless the device is tiled:
`G+0x2ABC` has `0x10`, or has `0x20` and every bound slot is the saved one (the
test of the null rectangle). A tiled call makes block A (`G+0x2ABF` has `0x20`,
`G+0x2ABC` has `0x40` and `G+0x31A4 & 0x15555555` is nonzero: one call for the
flags `0x30`, and the mask loses those bits) and the tile loop (tiles `i` below
`G+0x31CC` whose rectangle at `G+0x31D0+16*i` intersects the clipped one, with
`(3 << 2*i) & mask` nonzero, one call each), and can make none. A failure of an
allocation sets `G+0x2ABD` `0x20` and skips the allocation ORs of that call;
which call fails is not predicted, so a `0x20` that appears during the call
counts `clear_effect_alloc_failed` and the exact comparison is skipped, while a
`0x20` set at the entry (`clear_effect_alloc_marker_entry`) fails every
allocation at once (`0x8269B818` returns 0 first) and is compared without the
allocation ORs. The ring-segment switch `0x8269C588` can set `G+0x2ABD` `0x02`
when the ring is full; that bit appearing at the exit is counted in
`clear_effect_2abd_switch_bit` and is not a mismatch. The `Native frame
timing:` line of a developer build adds `clear_effect_exact_ok` (the three words and the byte equal
the prediction), `clear_effect_exact_mismatch_dirty10`, `_dirty18`, `_dirty20`
and `_byte2abd` (each counts once per call), `clear_effect_early_ok`,
`_early_changed`, `_alloc_failed`, `_unsupported` (a call the derivation does
not model, or an unreadable word) and `_unpaired`. Path coverage counts per
call of `0x826A1D90` for the Clears that got past the early exits:
`clear_effect_path_untiled`, `_path_tiled_block`, `_path_tiled_loop` and
`_path_tiled_none` (a tiled call that allocates nothing), `clear_effect_stencil`
and `clear_effect_fast`; per Clear: `clear_effect_color`, `_depth_only` and
`_flush_no_calls`. Recorded and never compared: `clear_effect_2abe_changed`
(Clears after which `G+0x2ABE` differs), and the sums of the 32-bit deltas of
the ring cursor `G+0x30` (`clear_effect_ring_delta_sum`), the allocator bytes
`G+0x3A54` (`clear_effect_alloc_bytes_sum`) and the segment cursor `G+0x3A44`
(`clear_effect_cursor_delta_sum`), which go negative when a kick resets them.
The first eight mismatches log the entry registers, the predicted and actual
value and the xor of each differing word, and the dirty words at both readings.

With the effect check on, the probe also derives the exit state the consume
below would store, from the entry reading and the observed calls: the three
dirty words, `G+0x2ABD`, the ring limits `G+0x34` and `G+0x38`, the segment top
`G+0x3A44`, the wrap word `G+0x3A4C` and the byte count `G+0x3A54`. It compares
them with the real exit and adds `clear_exit_ok`, `clear_exit_mismatch_dirty10`,
`_dirty18`, `_dirty20`, `_byte2abd`, `_limit34`, `_limit38`, `_pool_pos`,
`_pool_used` and `_wrap_pos` (each counts once per call), and
`clear_exit_unmodeled_<reason>` for the calls the model refuses (`tiled_block`,
`early_exit`, `alloc_marker`, `alloc_path`, `ring_space`, `alloc_capacity`,
`alloc_wait` and `stack_overlap`, a stack reach `[limit, r1)` that holds a device
word or an object the model reads, or fewer than the wrapper's 112-byte frame
below `r1`, tested at the entry before any guest word is read; the probe reads no
further word after it, so an unreadable watched word cannot mask it). The exit
model also predicts the tiled Clears (a tile loop that
runs, or the bracket `G+0x33B0`): `clear_exit_tiled` counts them, `clear_exit_tiled_ok`
the exact ones (also in `clear_exit_ok`), and `clear_exit_tiled_<kind>` the
shapes seen, by the words that made the device tiled: `loop` (the device is tiled
by the tile test of `0x826A1D90`, so the tile loop runs, even with a zero tile
count `G+0x31CC`), `bracket` (`G+0x33B0` nonzero), `byte10`
(`G+0x2ABC` bit `0x10`), `state` (byte `0x20` against the bracket, which
disagree) and `predicate` (`G+0x31AC` nonzero, which the Clear call tree never
reads); a shape can have several kinds. The only tiled shape the model refuses is the tile block A
(`G+0x2ABF & 0x20`, `G+0x2ABC & 0x40` and a mask `G+0x31A4` bit in
`0x15555555`), as `tiled_block`. The mismatch counters must read zero in a census with the probe
on before the consume is run. A mismatch logs the predicted and actual words
and the entry allocator state, including the null pool area words, up to eight
times.

`--native_consume_clear` is on by default. With `native_ring_stubs`, it skips
Original of the `Clear` wrapper `0x826A2970` (the body `0x826A22F8` behind it)
on the untiled calls (and, with `--native_consume_clear_tiled=true`, the tiled
ones) whose exit the host knows exactly. The call is consumed
when ring stubs have checked the device; no other `Clear` of this thread is in
flight; a ledger and an assembler are published; the entry observer stacked the
call; no derive or effect probe, shader harvest, scene capture or route
trace is on (each needs the body or its midasm hooks); the thread's
stack reach `[limit, r1)` holds no byte of the device words `[G, G+0x3A5C)` or an
object the model reads, and its bounds are known (`stack_overlap`: the Original's
saves would alias those words; tested once the device address is placeable, before
any other guest word is read); the device is
not tiled for `0x826A1D90` (`G+0x2ABC` has neither `0x10` nor `0x20` with a
saved surface) and the bracket `G+0x33B0` is zero, unless the tiled option is
on (a tiled device is `tiled` without it, before any derivation), and no call
takes the tile block A (`tiled_block`); the derivation models the call; the body reaches its flush
groups; `G+0x2ABD & 0x20` is clear; the strip words of `0x826A1AF0` and the
allocator words are readable; the allocator path is modeled (`G+0x34CC` zero,
the null pool allocator `0x8269B618`, or nonzero with a fixed buffer, the fixed
pool `0x826A6848`; a pool object without a fixed buffer is the indirect
allocator `0x826A6AB8` and is `alloc_path`); Original cannot flush (the ring
words it may emit stay below `G+0x38'` and `G+0x34'` after every side buffer
take, and `G+0x3A54` stays below `G+0x3A58`, past which `0x8269B818` moves the
limits so that the next ring check flushes, which would change the guest's
kick cadence); and every side buffer take succeeds without waiting. The fixed
buffer provably holds every side buffer (`(top - 84 * n) & ~3 >= cursor + 4 * W
+ 164` for the last take). The null pool takes are modeled in sequence by the
Resolve model `ModelNullPoolAllocator` with the Clear's size (84 bytes per strip
group, `21n` words at alignment 4 from `0x826A1800`), each take seeing the
position the one before stored; a take that would fail is `alloc_capacity`, and
one that would wait in `0x8269B158` for the GPU to leave its segment is
`alloc_wait`. The first condition that fails, in that order, is the reason; a
body that leaves before its flush groups is `early_exit` (it changes nothing,
and runs Original at no cost).
The replay makes, for each per-target clear the derivation lists, the renderer
observation of the hook on `0x826A1D90` (drain of the deferred draws, the
assembler clear, the ledger clear, the logical clear), fed with the registers
and the stencil and color words the host derived in place of the body frame.
The shader harvest observer is not replayed: a harvest is a diagnostic and
makes the call run Original. It then stores the exact exit state as big-endian
guest words, only for the words that change: `G+0x10`, `G+0x18` and `G+0x20`
as `(entry & keep) | set`, the byte `G+0x2ABD` with `0x04`, and the accounting
of the allocator: `G+0x34` and `G+0x38` lowered by the bytes the pool took,
`G+0x3A44` set to the last buffer and `G+0x3A54` grown by 84 per strip; with a
null pool object the limits stay, `G+0x3A44` takes the end of the forward takes
and `G+0x3A4C` the end of the wrap takes. Every
store address is checked writable first, and a failure runs Original. It
changes no register; no caller of the wrapper reads its result. The words that
later guest code reads are modeled; the ring words, the cursor `G+0x30` and the
contents of the side buffers are not written, as for every consumed draw and
Resolve, and `G+0x3A48` is untouched on the admitted path (the allocator moves
it only on a forced flush, which is refused).

The tiled option adds the tile loop of `0x826A22F8` to the model. For each
per-target clear the loop visits the tiles `i < G+0x31CC`, clips the call
rectangle to the tile rectangle at `G+0x31D0+16i` and selects the tile when the
clip is not empty and `3 << 2i` meets the mask `G+0x31A4` (tiles of 16 and more
are never selected). Each selected tile emits 2 ring words and one call to the
strip splitter `0x826A1AF0` with the clipped rectangle moved by the tile origin
`G+0x32C0+8i`, `G+0x32C4+8i`; those are the takes, in order, which the allocator
models (above) judge one by one, and the ring bound counts the tile words. The
bracket `G+0x33B0` adds the null pool allocator's bracket terms and drops the
pool threshold refusal (the forced flush is skipped when it is set). The model
follows only words the guest reads, so every combination of the tiling words is
modeled; the predicate `G+0x31AC` is not read by the Clear call tree. The
host replay is unchanged: the hook on `0x826A1D90` fires once per call of the
body, ahead of its tile loop, so the observation is one per derived call, with
the tile layout and predicate read from the device. The tile block A is not
modeled and stays `tiled_block`. The option is on by default, after a census with
the effect probe on read `clear_exit_tiled_ok` equal to `clear_exit_tiled`
and no tiled mismatch.

In a developer build the `Native frame timing:` line adds `clear_consume_ok`, `clear_consume_ok_tiled`
and `clear_consume_tiled_<kind>` for the tiled consumes (the kinds of the probe
counters above) and, for each nonzero
reason, `clear_consume_fallback_<reason>` with `stubs`, `nested`, `ledger`,
`entry_state`, `diagnostics`, `stack_overlap`, `tiled`, `derive_unsupported`, `early_exit`,
`tiled_block`, `alloc_marker`, `unreadable`, `alloc_path`, `ring_space`, `alloc_capacity` and
`alloc_wait`. With the cvar off or the ring stubs unpublished nothing is
counted.

`--native_consume_set_predication` is on by default. With `native_ring_stubs`, it
skips Original of SetPredication `0x8269CD20` (g171:12559-12772) on the calls
whose exit the host knows exactly. The exit is a pure function of the argument
`r4`, the old tiling byte `G+0x2ABC`, the byte `G+0x2F9B` and the five bound
surfaces `G+0x3098 + 4i` with their BeginTiling snapshots `G+0x31B8 + 4i`.
A nonzero `r4` stores the mask `G+0x31A4 = r4`, the predicate `G+0x31AC = 1`
and the byte with bit `0x01` cleared (g171:12711-12721). `r4 == 0` stores the
predicate 0, the mask `0xFFFFFFFF` and the byte with bit `0x01` recomputed
(g171:12581-12708). The bit is clear when the old byte has bit `0x08` or `0x04`
or `G+0x2F9B` is nonzero. Otherwise it is set when the old byte has `0x10`.
Otherwise it is clear when the old byte lacks `0x20`, and with `0x20` it is set
only when every bound word is zero or equals its snapshot. Both paths then
store `G+0x31A8 = 0`, `G+0x2AB4 = r4`, `G+0x2AB8 = 0` and the byte with one
`stb` (g171:12722-12736). A word that keeps its value is not stored, and the
byte store changes that byte alone.

Bit `0x01` is the one exit word whose error is silent. The draw bodies take
their tile-record branch on it (g148:13542, g63:13607, g127:13621, g151:13281),
and the surface rebinding paths `0x82690830` (g138:12922) and `0x826904D0`
(g164:12773) recompute it from the predicate and the same byte tests. A wrong
bit changes which draws record tiles while every other word still matches, so
the exit check below compares the byte on every call.

On ordinary calls the skip omits Original's other stores. The ring words `cursor + 4 =
0xC0006000` and `cursor + 8 = G+0x31A4` and the ring cursor `G+0x30` follow the
convention of every consume: no host reader takes them, and the cursor stays at
its entry value, 8 bytes behind Original's for each consumed call. That delays
the next guest ring check's flush by the same distance. The registers stay as
they were at the entry. The prologue's stack stores are the one omitted write
that can reach a word the model reads: it saves the link register at `r1-8`,
`r31` at `r1-16` (eight bytes) and the old `r1` at `r1-96` (g171:12565-12572),
and nothing else in the body stores below `r1`. Those are the bytes `r1-96` to
`r1-93` and `r1-16` to `r1-5`. A frame that lands on `G+0x2F9B`, for one, hides
the byte from Original, which reads it after the stores. Such a call is not
modeled; any other frame is dead.
None of the 18 call sites (g205:8898, 9311; g180:9104, 9133, 9331, 9353, 9480,
9502; g70:12794, 12908, 12946, 12992; g14:13014, 13060; g156:12614, 12660,
12684; g119:12710) reads `r3`, the new cursor, `r8` to `r12`, `cr0` or `cr6`
after the call.

A call runs Original, with the first reason that applies counted, when:

1. ring stubs have not checked the device (`stubs`);
2. a route trace, shader harvest, scene capture or the exit
   check is on, because each needs the body or its mid-assembly hooks at
   `0x8269CD20` and `0x8269CE78` (`diagnostics`);
3. the call is at pre-swap return address `0x826A4178` without the separate
   pre-swap gate and trusted clear context described below (`preswap`);
4. the thread's stack reach `[limit, r1)` holds a byte of the device words
   `[G, G+0x31CC)`, which cover every offset the model reads or writes (the
   highest is `G+0x31CB`), or its bounds are unknown, or fewer than the entry
   function's 96-byte frame lie between the limit and `r1` (`stack_overlap`). The
   test reads only the KPCR and is made before any other guest word is read.
   A reach that ends at `G` or starts at `G+0x31CC` is clear. The consume reads
   none of the ring words
   Original stores after the cursor; the exit probe tests `[cursor+4, cursor+12)`
   for a call without a flush;
5. a watched word cannot be read, a store target is not writable, or the device
   base is too high for the offsets (`unreadable`, the span test before the stack
   test);
6. the cursor `G+0x30` is above the soft limit `G+0x38`, compared unsigned, so
   Original would call the flush `0x8269C588` (g202:12873) before its ring
   words (`ring_flush`). A cursor equal to the limit is consumed.

Without `native_consume_set_predication_preswap` (below) the pre-swap call
keeps Original. The pre-swap stub and the guest body
`0x826A4150` (g119:12710) both call SetPredication with `r4 = 0` at
`0x826A4178` and read the cursor right after it. The stub tests
`G+0x30 > G+0x38` after a flip request at `0x826A419C` and again at
`0x826A4280`, and then passes the cursor to the callback emitter at
`0x826A429C`. With the cursor 8 bytes behind, the stub would decide those
flushes on a different word than Original does, so the one call whose cursor is
read at once requires an exact cursor. The stub reaches the call through the census
hook.

`--native_consume_set_predication_preswap` is read only at startup and is on by
default. It consumes only the host pre-swap stub's `r4=0` call. It also
requires
`native_consume_set_predication`, published ring services, a validated device,
a held ring-stub lease and no diagnostic needing Original. A scoped, single-use
token binds the census call to the host invocation, context, thread and device.
The guest Original's call at the same LR remains refused. With the flag off,
the existing route, guest stores, registers and counters are unchanged.

This consume reuses the six-field derivation and preserves the packet words
`[entry cursor+4]=0xC0006000`, `[entry cursor+8]=0xFFFFFFFF`,
`G+0x30=entry cursor+8` and zero-extended `r3=entry cursor+8`
(g226:12961-12991). It preserves neighboring mode bytes. It checks the whole
stack reach and the 96-byte frame before device reads, then all output targets
before any store. Packets must be aligned, nonwrapping and disjoint from the
device, KPCR and stack, including physical aliases. Unknown backing mappings
are refused. An entry cursor above the unsigned limit keeps Original and its
flush before any replay store. Equality and a subsequent `+8` crossing keep the
pre-swap stub's existing flush decisions and callback cursor unchanged.

In a developer build the `Native frame timing:` line adds `setpred_consume_ok`,
`setpred_consume_ok_clear` (the `r4 == 0` calls, which are also in the first)
and `setpred_consume_fallback_<reason>` for each nonzero reason: `stubs`,
`diagnostics`, `preswap`, `stack_overlap`, `unreadable` and `ring_flush`. With the cvar off or
the ring stubs unpublished nothing is counted. A consume run compares
`stub_flush` with the cvar on and
off, because a lagging cursor moves the next flush.

In a developer build, `--native_set_predication_exit_check=true` compares the model with every
SetPredication call whose Original runs. It reads the entry state and derives
the exit when the call starts, reads the guest words when Original returns, and
never changes what runs. It also makes the consume fall back as `diagnostics`.
The line adds `setpred_exit_ok`, `setpred_exit_flushed` (calls where the cursor
was above the limit, whose cursor, ring words and `r3` are not compared),
`setpred_exit_unreadable`, `setpred_exit_unmodeled_stack_overlap` (calls whose
stack reach holds a device word or the ring words the probe reads, which the model
leaves out, shown when nonzero) and one `setpred_exit_mismatch_<word>` count for each
of the ten compared words: `predicate`, `mask`, `word31a8`, `word2ab4`,
`word2ab8`, `tiling` (the byte), `cursor` (`entry + 8`), `ring_header`
(`0xC0006000` at `entry + 4`), `ring_mask` (the mask at `entry + 8`) and
`result` (`r3`). The last four are compared only when no flush ran. A call is
`ok` when no compared word differs. Every mismatch count must read zero in a
census before the consume is run.

When either the pre-swap flag or the exit-check flag is enabled, the timing line
also reports `setpred_preswap_enter`, `setpred_preswap_original` (counted just
before actual Original), `setpred_consume_ok_preswap`, and
`setpred_consume_fallback_preswap_<reason>` for every reason listed above.
The scoped entries count only trusted host calls. Untrusted calls with the same LR
retain the aggregate `preswap` refusal. The new fields are absent when both
flags are off. Existing consume totals include admitted pre-swap calls.

The existing exit check forces Original even with the pre-swap flag enabled.
It partitions trusted samples into `setpred_exit_preswap_ok`, `_exact`,
`_flushed`, `_unreadable`, `_unmodeled_stack_overlap`, `_unpaired` and
`_mismatch_<word>`, using the same ten comparisons. `exact` requires all ten
fields to match without a flush. A flushed six-field match increases `ok`
and `flushed` but never `exact`. Unreadable, unwound or abandoned samples do
not count as matches. Collect positive scoped exact coverage and zero scoped
mismatches in a separate exit-check run before enabling the consume. In an
admitted consume interval, scoped consumed must equal scoped entries, with
zero scoped Original and refusals. This does not establish coverage of
untrusted or unsupported callers.

A resolve is a scanout candidate only when it copies color slot 0 of a lone
1280x720 RGBA8 target with the presented form (2X, four host samples) into a
1280x720 texture, and that target is not in a tiled texture pass that a depth
tile opened. Every harvested scanout is the tiled pair from that surface; the
calling routine does not distinguish them. 1280x720 copies from a 1X, k_16_16
or k_2_10_10_10 source, and a texture pass's color tiles, are off-screen
resolves. Scanout resolve candidates wait for the next `VdSwap` to confirm
their physical destination. Its native output owns those resolves in both modes, without
writing guest bytes. Presentation uses a 1280x720 target with four host samples
and resolves samples 0 and 3. An identity gamma table is provisional unless
`native_gamma_apply` (on by default here) supplies the guest's own; a target
without a captured clear and an implied scanout resolve also remain labeled
as assumptions. Target preservation includes the EDRAM base, format, extent
and sample count.

The presentation samples the presented target itself, not the frontbuffer bytes
the scanout resolve wrote, so it composes that resolve's color order with the
swap fetch's swizzle. The title's scanout resolves write `k_8_8_8_8` with the
red/blue swap and no endian swap (`RB_COPY_DEST_INFO` `0x01000300`), and its
`VdSwap` fetch selects ZYX1 (dword 3 `0x1414`), so the frame presents the
target unswapped. The swap therefore exchanges the fetch's X and Z selectors.
The host presents the epoch's own work, so a presenting swap needs its own
epoch's last complete modeled scanout (an untiled one, or the last tile of a
tiled one) to the swapped frontbuffer; an earlier epoch's scanout predates the
work. Until the swap, any other write into the scanout's destination range
drops its form: a resolve the assembler admitted or refused, one the copy hook
refused, including a resolve whose entry state did not decode or that the
ledger no longer settles, and a guest write. A refused copy whose words were
unread drops the form wherever it is. Writes between the tiles of a tiled
scanout reach it the same way, so its last tile records no form. The range is
whole 32x32 tiles of the copy's `RB_COPY_DEST_PITCH` pitch and height at the
format's size. The title alternates `0x1F6F8000` and `0x1F360000`, adjacent
ranges that leave each other's form. Each modeled scanout arms the SDK's
physical write notice on its range; a guest write it reports after arming
drops the form, and a report the assembler could not queue drops it wherever
it is. The notice lets the SDK unwatch only the written pages, so a write
elsewhere in the same 4 MB block leaves other ranges watched. A presenting
swap fails `swap_scanout_unmodeled: <frontbuffer>` without
such a scanout, including the first frame and a swap that repeats an earlier
epoch's frontbuffer, and
`swap_scanout_overwritten: <frontbuffer> by <writer>` after such a write,
where the writer is `a resolve to <destination>`, `an unread resolve`,
`a guest write at <address>` or `an unqueued guest write`. It fails
`swap_scanout_destination_mismatch: <scanout> is not swap <frontbuffer>` when
its epoch scanned out elsewhere. Separate scanouts may differ in form and the
last complete one decides.

A swap without presented-target work presents nothing, so the presenter keeps
the last presented image, such as the device selector's deferred worker swaps
and unchanged re-presents. New EDRAM work never stands in for that image: a
swap with any presents only its own scanout. Once an image was presented, a
swap without work must name its frontbuffer
(`swap_display_mismatch: <shown> is not swap <frontbuffer>`), no write may
have reached its range since its scanout
(`swap_display_overwritten: <frontbuffer> by <writer>`), and the swap's fetch
swizzle composed with the scanout form, and its gamma table, must match the
presented ones (`swap_display_form_changed: <frontbuffer>`). Before the first
presented image such a swap is not checked. With `native_gamma_apply` (the default) a swap
whose only change from the presented image is its gamma table does not fail.
It adopts the table and presents nothing, so the next presented frame carries
it, as a gamma constant applied at presentation would. The kept image shows the old table
until that frame, because the renderer retains only the post-gamma image. A
changed swizzle still fails.

A scanout resolve of another form fails
`resolve_frontbuffer_dest_unsupported: <dest info>`, one outside the 1280x720
view (`RB_COPY_DEST_PITCH` `0x02D00500`) fails
`resolve_frontbuffer_pitch_unsupported: <dest pitch>`, and tiles of one scanout
with a different form or destination fail
`resolve_frontbuffer_dest_mismatch: tile <n> ...`. An untiled scanout must copy
the whole 1280x720 frame to the frontbuffer origin with the title's copy
control (`0x00100040`, sample select k01, clear bits aside), or it fails
`resolve_frontbuffer_form: rect <x0>,<y0>,<x1>,<y1> point <x>,<y> control
<control>`. A swap fetch with an endian swap fails
`swap_frontbuffer_endian_unsupported: <endian>`.
Texture fetch swizzles compose with the host format the way the SDK D3D12
texture cache builds an SRV component mapping: a data selector reads the host
component that holds it (`k_8`, `k_24_8` and `k_32_FLOAT` hold one,
`k_16_16_FLOAT` two), the constant 0 and 1 selectors pass through, and
selectors 6 and 7 are refused. A `k_24_8` texture uploads as R32 float depth,
as the SDK's DepthUnorm load converts it: each endian-swapped word's upper 24
bits through `xenos::UNorm24To32`, with stencil dropped.

Eight families are admitted, tried in the order of
`NativeDrawFamilyRegistry` (`src/gpu/guest_d3d/native_draw_families.cpp`):

- The solid menu panel: indexed-UP draws at `0x826A3568` from `0x82303E40` or
  `0x82303E90` with an 8-byte stride. `--native_draw_panel_capture=<directory>`
  names a solid adapter capture whose saved panel shaders supply its
  interface; the saved vertex and pixel words must match the pinned panel
  shader identity.
- The label batches: nonindexed-UP draws at `0x826A3000` from `0x82304258`
  with a 28-byte stride and the pinned label pixel shader. On the presented
  2X surface the viewport and window scissor follow the GFx display rectangle
  rule below, with `label_` codes. The label pair also draws into a 256x32
  single-sample `k_8_8_8_8` off-screen target (`RB_SURFACE_INFO` `0x04000140`,
  depth `0x7F8`), whose viewport and window scissor must be the whole target
  (`label_offscreen_viewport`). Another surface or depth fails
  `label_fixed_state 0x2000` or `0x2002`. On the presented surface a label
  inside a Scaleform mask tests stencil as the GFx mask stack does
  (`RB_DEPTHCONTROL` `0x24F00271`: depth off, front-face stencil equal to
  the reference, all stencil ops keep), with full read and write masks and
  the mask depth as the reference in `RB_STENCILREFMASK`, or it fails
  `label_stencil_mask 0x210d: <value>`; the native draw takes that test,
  reference and masks as the SDK does. Other depth or color control words
  fail `label_fixed_state depth <value> color <value>`.
  `--native_draw_label_shaders=<directory>` names a shader catalog whose label
  entries must match the pinned word digests and input layout: one at 2X and
  optionally one at one sample, without which the off-screen form fails
  `label_offscreen_unpinned`.
- The GFx quads: indexed-UP draws from `0x82303E90` whose shader pair is one
  of the pinned forms: the loading-screen Gouraud texture pair (12-byte
  stride), the menu bitmap pair and the stencil mask pair (4-byte stride),
  the untextured Gouraud pair (12-byte stride), and a 12-byte pair whose pixel
  shader samples fetch constants 0 and 1. The route applies the
  guest's linkage (`0x826AD8E8`) and fetch patch, requires the patched vertex
  words to match the pinned digest, admits only the presented state with an
  enumerated blend and RT0 color mask and no depth, and one 2D texture level
  per sampled fetch, and refuses other forms with `gfx_` codes such as
  `gfx_fixed_state 0x2000: 0x70101e0` or `gfx_texture_mipmapped`. The one
  mipped form is the loading backgrounds' linear DXT1 with packed mips and
  equal magnification and minification filters: the reader also reads its
  mip storage, every level the fetch names is uploaded, and the sampler is
  the leader sampler with the fetch LOD bias as the host mip bias. The
  route's cache of the last texture keys the fetch with the exact base and mip
  bytes, so a repeated draw reuses the decoded texture. Its
  sampler refusals carry the leader names under `gfx_`. The
  viewport and window scissor are the movie's display rectangle, full screen
  or inset as the gameplay HUD movies draw: the viewport must be integral and
  inside the 1280x720 target, and the window scissor must lie inside the
  viewport, since Xenos clips only to the scissor and guard band while the
  host also clips to the viewport. Other rectangles fail as
  `gfx_viewport_form` (including a viewport the decoder refuses),
  `gfx_scissor_beyond_viewport`, `gfx_scissor_form` (reserved or window
  offset disable bits, or an inverted scissor) or `gfx_scissor_empty`. The fetch
  sampler must be one the host reproduces
  exactly: it refuses a texture border (`gfx_sampler_border_size`), the
  OpenGL nearest clamp policy (`gfx_sampler_nearest_clamp_policy`), the
  halfway and border clamp modes (`gfx_sampler_clamp_mode`), and, when the
  magnification and minification filters differ, a nonzero LOD bias
  (`gfx_sampler_lod_bias`) or gradient exponent adjust
  (`gfx_sampler_gradient_adjust`), which change the filter the guest picks.
  `--native_draw_gfx_shaders=<directory>` names a
  shader catalog whose GFx pairs must each match a pinned form's word digests
  and input layout. The loading pair also draws the Hall of Glory panels into
  the 928x688 1X surface (`RB_SURFACE_INFO` `0x0E8003C0`, depth `0x5FC`)
  with a 900x675 viewport at the origin and the matching window scissor;
  other viewports fail `gfx_offscreen_viewport`. That form builds only from
  a catalog entry for the same pair at one sample (sample mask 1), without
  which it fails `gfx_offscreen_unpinned`. The panels sample the Hall
  background's packed-mip DXT1 chain (1024x1024, levels 0..9) through the
  loading backgrounds' mipped form.
- The post-ESRB scene mesh: `IndexedBuffer` draws at `0x826A39F8` from
  `0x8250C768` with the pinned MRT prepass pixel shader. The reader takes the
  16- or 32-bit index buffer at `G+0x3094` and each declared stream `i` from
  vertex fetch `95 - i` with its stride at `G+0x30F0 + i`. The route applies
  the guest's fetch patch, requires the patched vertex words to match the
  pinned digest, and admits only one 16-bit triangle list from stream 0 at
  base vertex 0 into the tiled RT0 `k_8_8_8_8` and RT1 `k_2_10_10_10` set
  with the pinned blend, color mask, depth and cull state; it draws RT1 as a
  second host target. Other forms fail with `scene_mesh_` or
  `indexed_buffer_` codes such as `scene_mesh_fixed_state 0x2003: 0x21c` or
  `scene_mesh_unsupported_draw_tuple`.
  `--native_draw_mesh_shaders=<directory>` names a shader catalog whose
  prepass pair must match the pinned word digests and input layout and
  declare `color_targets = 2` with `color1_format = 2`. A catalog entry
  without `color_targets` writes RT0 only, and `color1_format` is valid only
  with two targets.
- The title and menu backdrop: the two `IndexedBuffer` scene draws at
  `0x826A39F8` from `0x8250C768` with the pinned menu pixel shader, one per
  texture: a BC1 1024x256 sky and a BC2 2048x512 island, each one 2D level
  sampled through fetch 0. The reader takes the stream and index buffers as
  for the prepass mesh and adds the one `c252..c255` vertex literal record
  that `0x826ADB98` loads. The route applies the guest's fetch patch,
  requires the patched vertex words to match the pinned digest, and admits
  only one 16-bit triangle list from stream 0 (32-byte vertices) at base
  vertex 0 into RT0 alone on the 1280x720 surface, with the pinned
  `LessEqual` depth test and write, alpha test `Greater` against
  `RB_ALPHA_REF`, source-alpha blend and back-face culling. Other forms fail
  with `scene_mesh_` or `indexed_buffer_` codes such as
  `scene_mesh_literal_shape` or `scene_mesh_texture_form`.
  `--native_draw_menu_shaders=<directory>` names a shader catalog whose menu
  pair must match the pinned word digests and input layout, with one 2D
  texture at fetch 0 and alpha test `Greater`. Every other entry is a civ
  select prop pair that `0x8250C768` draws with leader state into the
  leader viewport. These band pairs follow the leader catalog rules and build
  as leader pairs do, but only from `0x8250C768`; a band pair with the
  backdrop's pixel words fails with
  `scene_mesh_menu_catalog_duplicate_pixel`. The backdrop is tried first.
  Both routes take the vertex layout and texture bindings from the live
  microcode: the patched vertex shader's fetches and the pixel shader's
  texture fetches must agree with the entry, or the draw fails with
  `scene_mesh_vertex_fetch_layout` or `scene_mesh_texture_fetch_layout`.
- The leader models of civ select and the world: `IndexedBuffer` draws at
  `0x826A39F8` from `0x8250CB98` whose pixel shader matches a pair in the
  leader catalog. Each
  pair's entry supplies its word digests, stride of stream 0 and attribute
  offsets, texture fetches 0..n-1 with their 2D or cube dimensions, and its
  `SQ_PROGRAM_CNTL`, `SQ_CONTEXT_MISC` and `SQ_INTERPOLATOR_CNTL` words. The
  reader's leader form adds each stage's literal records and every texture
  fetch the pixel shader samples, with its mip storage. The route applies the
  guest's fetch patch, matching fetch records against every declaration
  element as `0x826AD150` does, and requires the patched vertex words to
  match the entry's digest. A record whose semantic has no element takes the
  guest's branch at `0x826AD228`: a k_8_8_8_8 fetch at offset 0 of fetch
  constant 95. The live microcode decides the layout: the
  patched vertex fetches, in clause order, must equal the entry's attributes
  (fetch constant, stride, offset, format, sign and normalization), and the
  pixel shader's texture instructions must bind exactly the entry's slots
  with their dimensions. It admits only one 16-bit triangle list from
  stream 0 at base vertex 0 into RT0 alone, with alpha test `Greater`, inside
  a viewport and window scissor at the origin of one of two target forms:
  the 1280x351 band civ select draws, or the whole 1280x720 window the world
  draws the same pairs in. The scissor and the viewport's Y transform must
  belong to one form, or the draw fails with
  `leader_target_form: <scissor> <y scale> <y offset>`. Each pair takes one
  of the harvested state forms: `LessEqual` depth test and write or test
  only; a source-alpha blend that keeps the source alpha, `SRC_ALPHA`/`ONE`,
  or source alpha for color and alpha; back-face culling or none. The literal
  records then overlay their stage's constants in guest order. The draw
  conversion accepts a window scissor that stops short of the 1280x720
  surface's bottom right and applies it. Samplers take point or linear
  filters with every mip level the fetch names; as the SDK texture cache does
  with its default `anisotropic_override`, a mipped fetch with linear
  minification and magnification samples at 4x anisotropy. Other forms fail
  with `leader_` or `indexed_buffer_` codes such as
  `leader_blend_control: 0x7010701`, `leader_fixed_state 0x2180: <value>`,
  `leader_literal_stage` or `leader_sampler_lod_bias`.
  `--native_draw_leader_shaders=<directory>` names a shader catalog. Every
  pair needs texture slots 0..n-1, alpha test `Greater`, one color target,
  a distinct pixel word digest, and attributes on vertex fetch 95 that share
  one stride: signed 32-bit float pairs or triples, or unsigned normalized
  `k_8_8_8_8`. Other catalogs fail with `leader_catalog_` codes.
- The intro movie quad: one nonindexed-UP strip of four 20-byte vertices at
  `0x826A3000` from `0x82E3987C` with the pinned movie pixel shader, once per
  movie frame. Its nested `0x826A2B58` call belongs to the same submission.
  Each movie frame presents through the title's two tile scanouts to the
  frontbuffer it presents.
  The pixel shader converts three linear `k_8` planes (fetch constants 0, 1
  and 2: 1280x720 luma, two 640x360 chroma planes at pitch 768) with the
  `c252..c255` literal record. The reader snapshots the planes at entry and
  admits exactly that one pixel literal record; the route requires the
  patched vertex words to match the pinned digest and pins the screen-space,
  unblended, depth-off state. With the alpha test off, `RB_COLORCONTROL` may
  differ only in its alpha function, which the boot movie sets to 7 and the
  game-start movie to 4. Every vertex Z must be +0.0, the captured value:
  guest clipping is off while the host pipeline clips depth, so other depths
  are refused as `movie_vertex_depth`. Other refusals carry `movie_` codes,
  such as `movie_plane_format`, `movie_literal_shape`, `movie_vertex_literals`,
  `movie_state 0x2201: 0x7060706` or the GFx sampler codes renamed, such as
  `movie_sampler_lod_bias`. The plane samplers use mip filter base map, which
  the one-level sampler admits like point or linear.
  `--native_draw_movie_shaders=<directory>` names a shader catalog whose movie
  pair must match the pinned word digests, input layout and
  `screen_space_position = true`.
- The scene catalog: `IndexedBuffer` draws at `0x826A39F8` from the engine
  mesh path (`0x8250C768`), the leader models (`0x8250CB98`), `0x8230686C`
  and the mesh batches of `0x82511518` (`0x825118D8`) that no earlier family
  claimed, and the committed particle batches that `0x825107A0` draws through
  IndexedUpLowerer (`0x82510BFC`). The reader reads them as the leader form
  does; a particle batch reads the shadow at the lowerer's entry, stream 0
  and its indices from the destinations, dword counts and index format the
  lowerer published (`G+0x3488..0x3494`, `r8`), and its stride from `r9`. A
  particle batch must be a triangle list; its codes are `particle_*`. Several
  catalog entries may share a pixel shader: the route tries
  each entry's stride and attribute offsets in the guest's fetch patch, and
  the patched vertex words select the pair. The draw's specialization then
  selects one of its entries: the alpha test state, the target's sample count
  and the folded `SQ_PROGRAM_CNTL`, `SQ_CONTEXT_MISC` and
  `SQ_INTERPOLATOR_CNTL` words. The live microcode decides the layout and
  texture bindings, as for leader pairs. The route admits a 16-bit or 32-bit
  triangle list or strip from stream 0 at base vertex 0 into RT0 alone, in
  one of eleven target forms (`RB_SURFACE_INFO`, `RB_COLOR_INFO`,
  `RB_DEPTH_INFO` and window scissor): the presented 1280x720 surface at 2X
  with the full scissor or the 1280x351 leader band, a single-sample
  `k_8_8_8_8` off-screen target of 512x512, 1024x1024 or 1280x720, a 2X
  `k_8_8_8_8` off-screen target of 640x368 (scissor 640x360) or 320x192
  (scissor 320x180), the fog passes' single-sample 1280x720 `k_16_16`
  target at EDRAM base 0 or `0x2D0`, or the post passes' 2X
  `k_2_10_10_10` target of 1280x720 (depth `0x530`) or 640x368 (scissor
  640x360, depth `0x690`). A single-sample surface's pitch is its
  width rounded up to 80 pixels; a 2X off-screen surface's pitch is its
  width. A `k_16_16` form draws as `Rg16Snorm` and a `k_2_10_10_10` form as
  `Rgb10a2`, each through an entry built for that format, with depth test, depth
  write and stencil off
  (`scene_packed_target_depth: <value>`), since the host keeps such a target
  without a depth attachment. Every
  draw shares the fixed index bounds, color mask `0xF` (or 0, which writes
  depth and stencil only while the pixel shader still kills; a `k_16_16`
  form may also write red or green alone), depth range,
  viewport transform and mode words. `PA_CL_CLIP_CNTL` is `0x80000`, or
  `0x80001` (user clip plane 0, as the hall reflections draw) through an
  entry marked `user_clip_plane = true`. That entry's vertex shader writes
  `SV_ClipDistance0` as the dot product of the guest clip-space position and
  `PA_CL_UCP_0`, as the SDK does, and the recipe binds the plane from the
  shadow at `G+0x2820` (written by `0x826ACD20`) at shared constant `c29`.
  Other plane sets, cull-only planes and an entry without the flag are
  refused. `RB_DEPTHCONTROL`, `RB_BLENDCONTROL0`
  and `PA_SU_SC_MODE_CNTL` each take one of the harvested values.
  `RB_COLORCONTROL` may vary only in its alpha function and test enable, and
  an enabled test must be `Greater` or `GreaterEqual`, each with its own
  entry. Textures may be `k_8`, `k_8_8_8_8`,
  DXT1, DXT2/3, DXT4/5, `k_24_8`, `k_16_16_FLOAT`, `k_16_16_16_16_FLOAT`,
  `k_32_FLOAT` or `k_2_10_10_10_AS_16_16_16_16`, sampled as
  leader textures are, except that a fetch LOD bias becomes the host mip
  bias when the magnification and minification filters agree. A pair whose
  vertex shader samples slot 16 binds that texture after the pixel slots. The
  reader snapshots fetch 16 for any pixel shader that such a pair uses; when
  that fetch names no readable storage, it keeps empty bytes, and only a pair
  that samples it is refused. Refusals carry `scene_` codes, such as
  `scene_no_pixel_shader`, `scene_catalog_pixel: <digest prefix>`,
  `scene_catalog_vertex: <digest prefix>`,
  `scene_target_form: <surface> <color> <depth> <scissor>`,
  `scene_vertex_fetch_unread`, `scene_alpha_func: <value>`,
  `scene_specialization: <detail>`, `scene_fixed_state 0x2104: <value>`,
  `scene_depth_control: <value>`, `scene_texture_format <n>` or the leader
  sampler codes renamed, such as `scene_sampler_lod_bias`.
  A draw inside RenderEntitiesList's Z prepass bracket (G+0x2ABC bit 6,
  set by `0x8269D7D8` and cleared by `0x8269DB20`) is replayed by the
  shader flush `0x826ADD88` depth-only in the Z bins and normally in the
  color bins. The route draws it once as its color pass when its depth
  test and depth write are on with LEQUAL or ALWAYS, its blend is
  ONE/ZERO, stencil is off, it writes color, and the Z bins keep the
  draw's own vertex shader (G+0x2ABE bit 6 and the bound VS's header word
  +872 bit 0x20 clear). Otherwise it is refused
  `scene_conditional_or_recorded_shader: zpass depth <value>`,
  `: zpass blend <value>`, `: zpass depth-only variant` or
  `: zpass depth-only draw`, or `scene_fixed_state 0x2104: 0x0` for a
  color mask of 0.
  `--native_draw_scene_shaders=<directory>` names a shader catalog. Every
  entry needs pixel texture slots 0..n-1, optionally followed by slot 16,
  the same pixel slots across entries of one pixel shader, and attributes
  as the leader catalog admits them, where `k_8_8_8_8` may also be an
  unsigned integer. Other catalogs fail with
  `scene_catalog_` codes. The family is tried after every other family.
  The catalog's `depth-only.toml` lists vertex shaders that draw without a
  pixel shader. Each entry pins the words the GPU runs: those the guest's
  fetch patch (`0x826AD150`) writes into the VS for the draw's declaration
  and stride before `0x826ADD88` emits it. The words read at the draw's
  entry are the previous patch's, or before the VS's first draw the shader
  file's fetch templates (the terrain depth VS `289B3AEDEC76A40F` loads
  `05F82000 000004C8 00000000` at words 9..11 and runs
  `05F82000 00393548 00000003` once patched). The route reads such a draw
  when its words as patched for the draw are listed, whatever the words in
  guest memory hold; otherwise it is refused `scene_no_pixel_shader`. The draw
  must run VS variant zero, its patched
  words must be listed, and the native draw runs them. The shadow is then
  committed as `0x826ADD88` commits a
  draw without a pixel shader. The draw callers run it only when the dirty
  word at `G+20` has a bit of `0x1E0000`. `RB_MODECONTROL` becomes 5 (depth
  only); when that changes the mode, `RB_COLOR_INFO` loses its format bits
  under `G+0x2ABF` bit 3. A mode change or `0x180000` dirty makes the SQ
  program and context words the VS metadata's; otherwise the shadow keeps
  the last commit's. A shadow still in the previous draw's color mode
  therefore draws depth only. The committed state must hold color mask 0, no
  alpha-to-mask, and the entry's SQ program and context words. In
  `RB_MODECONTROL` 5 the SDK binds no pixel shader and alpha tests only in a
  translated one, so an enabled alpha test (as key `A7E9CB62` draws) is a
  no-op and `SQ_INTERPOLATOR_CNTL` is unread. The target, depth, blend and
  raster rules are those above. Its native draw binds no pixel shader and
  writes depth and stencil only. Refusals add `scene_depth_only_variant`,
  `scene_depth_only_linkage` (a VS with exports to link),
  `scene_depth_only_fetch_patch`, `scene_vertex_fetch_layout` and
  `scene_color_control: <value>`.

Each option is bring-up only and is rejected outside `renderer=native` and
with `native_present_fixture`. A family without its option has no native
route, and without any of them no draw is admitted.

`ReadShaderResourceLiterals` reads the shader resource literals that
`0x826ADB98` loads when a shader is set (`0x826ADEF8` for a pixel shader), such
as the intro movie pixel shader's `c252..c255`. The guest emits them as
`LOAD_ALU_CONSTANT` packets from guest physical memory, so they never pass
through the constant shadow at `G+0x1780`. The reader walks the stage's
literal table, whose `recordBytes` word is at `table+16`, over whole 8-byte
records (a 16-bit first float4, a 16-bit dword count and a 32-bit offset from
the shader object's resource base) until a record with a zero count. It
returns each record and the (register, word) pairs at `0x4000 + first * 4`;
overlapping records keep guest order. An object with no table loads nothing.
The reader refuses, with its code as the error: an unreadable or short read
(`shader_literal_unreadable`), record bytes below 8 or above 4096
(`shader_literal_extent`), a nonzero record past the table end
(`shader_literal_record_extent`), a base plus offset above 32 bits
(`shader_literal_address`), a range ending past register `0x4800`
(`shader_literal_range`), and a range outside the requested stage's half of
the unified file (`shader_literal_stage`). Only routes for pinned shaders
admit literals: the movie route admits its `c252..c255` record and reports a
reader refusal as `movie_literals: <code>`; the GFx and label routes still
refuse literals.

When the ledger needs the host fence of admitted draws before their swap, the
open epoch is cut into a segment: those draws are submitted as prefix passes,
and the fence value covering them completes the segment. A later segment or
the epoch's final frame continues the target only when it is the target the
previous segment ended with. The final frame carries only its own passes and
the count of recorded segment passes it continues; the renderer joins them in
front of it, and only final frames are published. An epoch without draws or
scanout work publishes nothing and completes with
the work before it. Off-screen producer frames retain their resolved texture
and return raw readback without presentation. The renderer reads that readback
when the frame's fence completes, so it records the next frame meanwhile. A
segment may follow a final frame that is still in flight: the renderer records
requests in order, so the segment starts a new chain after that frame, and
completion still follows submission order.

`--native_resolve_graph` is read only at startup and is true by default. It
records off-screen
resolves inside the epoch's segments instead of as producer frames;
`--native_resolve_graph=false` returns each to a producer frame of its own. Each resolve becomes an
off-screen pass of the chain that copies its destination into a host readback
buffer. The image exists once the stream fence reaches the segment. A source's
next resolve continues its attachments with only the draws recorded since; a
new clear, a changed tile background or any pass in between replays the
source from its clear. Off-screen and presented passes keep separate
attachments, so a presented pass resumes the latest presented attachments
after off-screen passes and the epoch has one terminal frame. A swap submits
its open resolves as one segment before the final frame. An epoch with only
off-screen work ends its chain with a one-texel retained frame that presents
nothing. An off-screen pass on the presented EDRAM base joins the graph too,
since its attachments stay apart from the presented ones. A resolve falls back
to its own producer frame when a target slot is `Rg16Snorm`, or when a source
that gates segments cannot cut one yet.

`--native_eager_graph_segments` is read only at startup and is true by default.
It is effective
only with `--native_resolve_graph` and sends recorded graph passes as segments
during the epoch. With false, the swap submits the open resolves as one
segment.

`--native_resolve_alias` is read only at startup and is true by default. It is
effective only with `--native_resolve_graph` and keeps a table of off-screen
resolves by
destination. A resolve enters it when recorded, which arms the guest write
notice over the destination, and a graph segment admits it with its texture.
The swap that closes its epoch, a later resolve over its destination, an image
the ledger never writes, a guest write reported after it was recorded, or a
host write over the destination ends it. Before a family reads a texture
fetch's bytes it asks the table. A fetch matches when its base page lies in an
admitted resolve's destination, in the open epoch and chain, and it reads
k_8_8_8_8 (6) or k_2_10_10_10_AS_16_16_16_16 (54) with the destination's exact
layout, covering the whole copy; its texture samples with the fetch's swizzle,
red and blue exchanged for a swapped copy. Packed guest words match only on a
renderer with castable storage: the app session reports its producer's device
when the session starts (Direct3D 12 enhanced barriers and relaxed format
casting); on a device without it they are refused as `packing`.

Only a draw into the presented target binds a matched texture, in place of the
fetch's bytes; an off-screen draw is refused as `offscreen` and reads them.
When a fallback frame ends the chain, the producer's source pass and resolve
are recorded again, without a readback, in front of the frame that next
replays the bound draws: the fallback frame at a swap, a frame without a
chain, or the next chain's first segment. A bound match is never evicted from
the table. While no chain is open, a match from an ended chain still binds;
once a later chain opens, a match it did not adopt, being unbound when it
began, is refused as `reset`. A presented draw before the epoch's first
presented work, or after its tiled clear and before its tiled scanout, may
still join a tiled texture pass's off-screen content and is refused as
`offscreen` too; a tiled texture pass whose draws sample a matched texture
fails as `tiled_texture_pass_alias`. A draw binds matches of the epoch its
build began in: one of a later epoch is refused as `retired`, and an admitted
draw that bound a resolve and is recorded after a swap closed the resolve's
epoch fails the next epoch as `alias_epoch_crossed`.

`--native_surface_registry=true` is read only at startup. It binds texture fetches of
resolve destinations through a registry of the resolves' host textures in
place of the alias table; it is on by default, and off restores the alias table. Each
off-screen resolve enters it by its destination's physical range when
recorded, and takes effect at once. The tile resolves of a predicated-tiling
bracket write one pending version instead: a host texture of the destination
fetch's size, into which each tile's texture is copied at its destination
rectangle in resolve order, so where rectangles overlap the later one wins; a
version holds at most 64 tiles, and a further tile is refused and ends every
entry over its range. Any other resolve overlapping a pending version ends it
and counts as an overlap. A pending version that ends before its EndTiling
also ends the older entries over its range, since its tiles still wrote guest
memory. The version takes effect at the
bracket's EndTiling (0x8269D320), so a draw inside the bracket binds the
version before it, which no tile changes. When an entry takes effect, an older entry of the
same range whose rectangles it writes ends, and the new one is its next
version; any other older entry it overlaps ends too and counts as an overlap,
so a fetch of that destination reads its bytes. An entry binds once every
frame holding its textures was submitted. The renderer keeps the textures in a
store across frames and epochs, so draws of later epochs bind them too; a
replaced or ended entry leaves the store after the frame that releases it and
the frames still using it complete. The registry holds at most 64 entries; a
resolve beyond them is refused rather than evicting a live entry, ends the
entries its range overlaps and counts as refused. A fetch binds the entry
holding its base page when it reads the destination's exact layout, the
entry's rectangles cover the whole fetch, and its format is a view of the
copy's stored words. The copy swaps red and blue, then converts to its endian
mode, and the fetch reads back through its own; both endian modes permute a
word's bytes, so a view folds the permutation left over into its component
selectors. k_8_8_8_8 (6) reads any copy's bytes as RGBA8 in any pair of
endian modes, except swapped k_2_10_10_10 words, whose swap moves 10-bit
fields. k_2_10_10_10_AS_16_16_16_16 (54) reads packed words in the copy's
endian mode, swapped only as k_2_10_10_10 words. k_16_16_FLOAT (31) reads
unswapped packed words in the copy's endian mode or with their halves
exchanged. Packed words bind through an RGBA8, R10G10B10A2 or R16G16_FLOAT
view of their castable R32_UINT storage; RGBA8 storage has only its own view.
A fetch that reads texels no rectangle wrote is a `coverage` refusal, and
packed words need castable storage. A view that would split a byte, field or
half is a `format` or `endian` refusal. A fetch binds the one level the bytes
path would upload alone: no mip page, or minimum and maximum levels that clamp
to 0, else a `mip` refusal. k_24_8 (22) of depth words in the copy's endian
mode binds the entry's converted texture instead. Each depth resolve also
draws, at the tile's place in an R32_FLOAT store texture of its own, each
word's 24-bit depth `n` as `(n + (n >> 23)) / 2^24`, the bytes path's
conversion; it lives, ends and leaves the store with its version, and the
fetch's selectors read it as the bytes path's RRRR view. It holds the base
level only, so k_24_8 requires only a minimum level of 0: the world's one
k_24_8 fetch declares levels 1-10 in mip memory no resolve writes, and its one
consumer, pixel shader 1262103F2F06927F at 8250C768, samples it 1:1 over the
screen, at LOD 0 or below, where only the base level contributes. Each pixel
shader that first binds a converted texture is logged once as
`Native surface k_24_8 view bound by ps <hash> from <caller>`, so a consumer
that could sample past the base shows; it would need levels 1-10 decoded from
guest memory under the converted base. Any draw whose target decoded binds,
off-screen or presented; on a
hit the family neither reads nor decodes the fetch's bytes. Inside such a
build, an entry whose resolves the open epoch has not submitted yet, as when
the source had no graph segment ready, is first submitted by the lookup, as
a read of its bytes would submit it, and then binds without waiting for its
readback. The renderer
validates an epoch's graph segments and terminal as one frame, so a segment
draw's typed view over packed words carries to the terminal joined to it,
even when the terminal binds no entry itself. The lock core
0x82697CA0, observed at its entry, reads its flags word: flag 0x10 selects the
returned address view, and a lock without it is treated as writable, ending
the entries of the locked ranges at the last unlock through the unlock core
0x82697F38. A guest write the write watch reports outside a write-intent lock,
a host write, a resolve the registry does not model, and a physical free
through 0x8280DEC8 end an entry at once, as does a resolve image the ledger
skips, since guest memory kept older bytes. A live entry's resolve image
stays on the host: the readback completes the resolve's span as before, but
the ledger applies its record without encoding or writing the image. The
image lands in guest memory when a lock of either intent names its range
(registered first, so an image taken while the lock is open lands at once),
when a read of the range needs its bytes, as a fetch that misses does, or
when a resolve the registry does not model writes the range. An entry that
ends for any other reason than the ones below, as when a later resolve
overlaps it without covering it, leaves its unlanded images owed: they bind
no fetch, but stay on the host under the same rules, and past 64 such
entries the oldest land. It is dropped unwritten when the next
version covers its rectangles, when a later resolve of whole 32-row tile
rows across the full pitch from the origin writes every byte of its range,
when the memory is freed, or when a guest
store outside a lock or a ledger host write is about to change the range,
since the notice precedes the store; from that notice on no read, lock or
later image writes it. An image dropped before the ledger takes it ends at
the take unencoded. Images land in op-mark order: one
written now first writes every older held image it overlaps. At most 128
images and 128 MiB stay held; a further image lands when taken. A guest CPU
read of a destination without a lock reads older bytes; the SDK's watches
see writes only, so no such read is counted. With the registry on, the `Native frame timing:` line
adds `surface_hits`, fetches bound to an entry; `surface_misses`, fetches of
an entry's range that read their bytes instead, also counted as
`alias_miss_<reason>` and `alias_form_<condition>` on the
`Native resolve graph:` line; `surface_miss_form`, `surface_miss_coverage`
and `surface_miss_unmodeled`, those misses by reason; `surface_miss_pending`,
fetches of a range that only a pending version holds; `surface_overlaps`, entries a later resolve
overlapped without replacing them; `surface_dropped_covered`, entries
dropped because a later whole resolve writes every byte of their range;
`surface_owed_dropped`, owed images dropped unwritten; `surface_owed`, the
owed images left when the line was logged;
`surface_unlocked_writes`, entries ended by
a guest write outside a lock; `surface_refused`, resolves refused at either
capacity; `surface_entries`, the entries held when the line was logged;
`surface_depth_views`, hits that bound a converted depth texture;
`surface_submits`, hits whose lookup first submitted the entry's resolves;
`lock_readbacks`, resolve images written for an open lock;
`surface_flushes`, those written for any other reason, including resolves no
entry holds; `surface_readback_mb`, the host bytes of the images still held;
and `surface_conversions` and `surface_conversion_ms`, the depth conversions
the renderer recorded and their CPU recording time. With
`--native_frame_timing`, `encodes` equals `lock_readbacks` plus
`surface_flushes`, less any image encoded for a read whose span the ledger
then dropped, plus any encode that failed. Each
distinct pair of resolve and fetch shapes that misses by form is logged once,
as `Native surface form miss:` with the copy's format, endian, swap, packing,
size, pitch and tiling, every fetch field the view rule reads, and the
condition it failed; the registry keeps its first 256 pairs and logs no
further ones.

The log records the first eight rejected draws of each ring generation: hook,
caller, sequence, indexed-UP stride, vertex and pixel shader objects, the
pixel microcode word digest, texture fetch constant 0 at `G+0x480` and the
refusal detail of the family that recognized the draw (`refusal=none` when
no family did). In strict mode the session failure carries the same detail,
for example `no_native_route; gfx_viewport_form: 640,0 1280x720`. It also
records the first eight swaps: frame status, draw coverage, the first rejection
or failure detail and the host completion value. A frame that fails to
assemble or submit fails the ledger at its `VdSwap` in both draw modes. A
removed device or failed native frame stops completion and fails the session
once, for example with `native_device_lost`. The GPU Completion thread polls
the source after each ledger pump and forwards its failure outside the ledger
lock; UI dispatch is needed only to update the visible title.

The first eight clears and first four resolves log their target and operation
mark; resolves also log copy words and physical destination. The first 16
resolves from a tiled off-screen set log `Native tiled resolve` with the set's
surfaces, the slot, the tile the predicate selects, the rectangle, point, copy
control, destination, `RB_COPY_DEST_INFO`, fetch word 2, both clear words, the
operation mark and any error. A tiled source may carry slot, fragment select
and clear flags; other resolve flags fail `resolve_flags_unsupported` with
their value. The first eight
clears that do not decode log `Native clear refused` with the code and the raw
words of every bound render target and the depth surface (`+0x1C` info, `+0x18`
surface info, `+0x24` extent) and the tiling state. Successful writes to
guest images log their byte count and SHA-256.

Every assembler clear refusal ends with the clear's form in parentheses:
index, flags, predicate, rectangle, the color, depth and stencil bits, the
target (`<width>x<height>x<samples>`, format, EDRAM base, depth base, color
target count), the depth surface and the tile count and predicate mask, for
example `clear_form_unsupported: color rect (index 0 flags 3f ...)`. The
general clear refusals name the failed conditions after the refusal name:
`clear_form_unsupported: ended_set`, `depth_only`, `color` or `mrt` followed by
`+`-joined conditions such as `after_failure`, `index`, `flags`, `no_color`,
`no_depth_surface` or `rect`. Resolve refusals without a detail of their own,
and every per-tile resolve refusal, end with the copy's slot, control,
rectangle, point, destination, destination info and source target;
`resolve_source_unmodeled` from an untiled source names `after_failure`,
`other_source` or `uninitialized`. Draw target refusals
(`frame_target_mismatch`, `draw_target_unreadable`, `draw_tiling_unreadable`,
`draw_depth_unreadable`, `frame_depth_target_mismatch`,
`offscreen_depth_undefined`) name the decoded and recipe targets, and the
draw-time `tiled_target_uncleared`, `tiled_depth_uncleared`,
`presented_edram_aliased`, `tiled_scanout_in_progress` and
`tiled_source_resolving` failures name the target. The refusal name stays the
prefix.

The first four admitted panel draws log `Native visibility`: hook, caller,
viewport, indexed decoded position ranges, triangle count, and pixel shader
constants. Positions are raw because recipes provide no CPU shader transform.
The log includes c0 and every other nonzero pixel constant; omitted vectors
are zero. The first two published guest outputs also log whether the existing
packed gamma readback contains any non-black RGB or nonzero alpha. This tests
the published output before window scaling and overlays. These two outputs
use the renderer's synchronous readback path; later outputs request no
diagnostic readback. Presentation traces record frame, outcome and gamma
changes and drawn/empty overlay transitions; NoFrame ticks after a guest
frame do not produce idle presentation events.

The FPS counter and the debug overlay's Guest row count distinct guest frames
the native swap chain presented, excluding overlay repaints, repeats of a shown
frame and the idle canvas. They update over windows of at least one second.

`--native_frame_timing=true` is read only at startup and exists only in a
developer build; it
logs a `Native frame timing:` line for each window of at least one second of
guest swaps. Every `Native frame timing:` field and every console variable that
names the line in this document belongs to that build. It reports the swap
count and rate, the longest swap interval, swap assembly time, guest memory
reads through the resolve overlay with their total time including readback
waits, guest resolve encodes and their time, the producer's submissions,
recorded draws, times for recording, fence waits, uploads and pipeline creation,
pipeline, texture and texture-upload creations, and guest D3D calls by role.

The same line reports deterministic shader identity costs per window:
`vs_identity_hits` and `vs_identity_misses` count lookups of patched VS code
and its word digest. `vs_fetch_patches` counts actual `PatchVertexFetch`
invocations, including refused patches. `vs_identity_bytes` counts guest
payload bytes copied by shader-stage/interface reads and the VS identity
reader, excluding watch reads, draw-state snapshots and geometry. Reusing
validated guest content adds no copied bytes to this counter. These counts
can compare shader work while other builds make wall-clock timing noisy.

Qualification, routing and the optional watch share shader results by exact
content: VS object/header and code, active variant, VS/PS metadata and linkage
patch lists, declaration header and elements, fetch records and templates,
effective stride of stream zero in bytes, swizzle and format tables, and the
candidate's admitted offsets. Nonzero vertex streams remain refused. Guest
read-cache entries are reused only while all journaled bytes still match
their views. Replacement at the same address or in-place mutation invalidates them.
Full backing code identity conservatively invalidates after a guest fetch
patch too. The shader memo retains no effective device state or constants.

With a warm cache and unchanged validated content, a repeated depth-only
draw adds two identity hits (qualification and routing), zero misses, zero
fetch patches and zero identity bytes. An additional identity query or watch
comparison adds a lookup; watch payload copies are counted only in
`vs_watch_bytes`. With the watch disabled, its reads, diagnostic hashes, key
comparisons, stale tracking and logs perform no work and copy zero bytes.

Times are wall-clock sums on their own threads, so they overlap and can exceed
the window.
`texture_prepare_ms` is the producer's frame texture preparation before
recording, which assigns storage and matches texture content across payload
owners. `draw_builds` and `draw_build_ms` count the draw family builds on the
guest drawing threads, including their resolved reads and texture decodes.
The `rec_<phase>_ms` entries split `recording_ms` into the producer's per-draw
phases, which sum to it: `setup`; `textures`, the per-slot texture work not
named below; `texture_compare`, comparing a payload with the bytes its storage
retains; `texture_upload`, the span `texture_upload_ms` times, for recorded
draws; `texture_retain`, keeping a snapshot of uploaded bytes; `samplers`;
`draw_upload`, the constant, vertex and index copies; `views`; `pipeline`;
`draw`; and `tail`, the readback and state transitions after the draw.
`draw_upload_constants_mb`, `draw_upload_vertex_mb` and `draw_upload_index_mb`
give the megabytes those recorded draws wrote to upload memory: the three stage
constant buffers, including the zeroed tail through each buffer's capacity,
then the vertex and index bytes.
The `tex_<outcome>` entries count recorded texture slots by how the producer
bound them: `repeat`, storage already recorded in the frame; `match`, storage
holding the payload's version or bytes; and `frame`, a texture the frame
produced on the GPU. The remaining outcomes upload, with
`tex_<outcome>_mb` giving their payload megabytes: `new`, a layout and payload
owner the frame context's preceding texture preparation did not hold;
`changed`, one it held whose storage did not hold these bytes; `unretained`,
storage that kept no copy of its bytes to compare; and `uncached`, a draw
outside a frame, which keeps no storage by payload. A payload owner is the
decoded buffer, so a family that decodes into new buffers reports `new` for
content that changed.
The pool entries describe the producer's fenced pools. `upload_pool_hits`,
`upload_pool_new` and `upload_pool_evictions` count the window's upload buffer
acquisitions served by an idle buffer, the buffers created, and the idle
buffers released over the idle cap; `upload_pool_idle_mb` and
`upload_pool_live_mb` give the bytes held idle and in all when the line is
written. `texture_pool_hits` and `attachment_pool_hits` count the window's
frame CPU texture and frame attachment creations served by an idle resource,
and `texture_pool_evictions` and `attachment_pool_evictions` the idle resources
released over the cap; `texture_pool_idle_mb`, `texture_pool_pending_mb` and
`attachment_pool_idle_mb` give the bytes held when the line is written,
pending bytes waiting for their fence. `textures` counts only the frame CPU
textures the pool did not serve. `resolve_pool_hits` and
`resolve_pool_evictions` count the frame resolve destinations served by an
idle destination and the idle ones released over the cap or under storage
pressure, and `resolve_pool_idle_mb` and `resolve_pool_pending_mb` the bytes
held. A destination returns to that
pool when its last holder, a frame, the retained textures or the surface
store, releases it, and serves a later destination of its allocation once the
signal after its return completes. `version_texture_hits`,
`version_texture_uploads` and `version_texture_evictions` count the frame
texture bindings of an immutable texture version served by its stored
texture, the versions uploaded into one, and the stored textures evicted,
back to the texture pool, when their version expired, over the 256 MiB cap or
under storage pressure; `version_texture_mb` gives the bytes stored. While a
version stays stored, every later frame, on either frame context, samples its
texture without uploading it again. Megabytes are decimal.
`stream_queue_waits` and `stream_queue_wait_ms` count the window's draw stream
callers that blocked before the stream took their request, with their total
wait, and `stream_future_waits` and `stream_future_wait_ms` the synchronous
calls that then blocked on their own result. A guest read of a pending graph
resolve's image waits in the resolve graph line's `peek_wait_ms` instead.
`owned_mb` gives the bytes charged against the renderer's 512 MiB owned bound
when the line is written, and `owned_peak_mb` the most charged since the
renderer was created. `attachment_pool_misses` counts the window's frame
attachment creations the pool could not serve, and `attachment_pool_evicted_mb`
the megabytes of the idle attachments it released, over its cap or for storage
pressure. `attachment_creates` and `attachment_create_ms` count the window's
attachment creation calls after a pool miss, failed ones included, and the
time in them, at all four take sites listed below.
After a window with attachment pool misses, a `Native attachment misses:` line
gives them since the renderer was created. `frames` counts the render frames
the draw stream recorded, and `untracked` the misses of shapes past the
diagnostic's 256-shape table. Each `site_<site>` counts the misses of one take
site: `clear_color` and `clear_depth` in render frame clear passes, and
`replay_color` and `replay_depth` in draw replays. Each
`miss_<width>x<height>x<samples>_f<format>_g<flags>` entry, with `_c` for a
castable shape, is one of the eight most missed shapes. It gives the shape's
DXGI format number and its resource flags in hex. After `=` come its misses,
then, after `:`, those misses by the frames since the pool last took that
shape back: 0, 1, 2-3, 4-7, 8-15, 16-31, 32-63, 64 or more, and last a shape
it never took back. A small gap means the pool held the shape recently; the
miss alone does not say whether that storage was in flight, evicted or taken
again.
In a developer build the `api_<role>` entries count calls into the title's D3D runtime entries
listed in `src/gpu/guest_d3d/guest_api_entries.inc`, summed over the
window's swaps. A counting hook runs the guest function unchanged, and with
timing off it pays one relaxed load. Only roles with a nonzero count appear,
in this order: `device`, `resource_create`, `lock`, `unlock`, `release`,
`set_render_target`, `set_depth_stencil`, `set_texture`, `set_sampler_state`,
`set_stream_source`, `set_indices`, `set_vertex_shader`, `set_pixel_shader`,
`set_vertex_declaration`, `set_shader_constants`, `set_render_state`,
`viewport`, `scissor`, `clear`, `draw`, `resolve`, `present`, `ring_internal`
and `other`. A call made while the same thread is inside another call of
its role is part of that call and is not counted again, so a UP draw wrapper's
call to its draw sink, a `Clear` that loops over `ClearF`, and the per-tile
resolves of `EndTiling` each count once. Calls of different roles nest freely:
a device reset that reinitializes the ring counts in `api_device` and
`api_ring_internal`.
With the resolve graph on, a `Native resolve graph:` line follows in
each window. It counts graph resolve passes that continued a source's
attachments and those that replayed it whole, resolves that fell back to a
frame of their own by reason (segment not ready, a k_16_16 target slot, no
source content, or another resolve at the same swap falling back), guest peeks
that waited on a pending graph resolve with
their total wait, and with `--native_resolve_alias` whether packed words
can alias (`castable`, 1 for castable storage), the fetches bound to a
resolve's texture (`alias_hits`), producers recorded again after a chain ended
(`alias_reproduced`) and the table's refusals of texture fetches as
`alias_miss_<reason>`: `form`, `coverage`, `written`, `superseded`, `retired`,
`packing`, `reset` and `offscreen`. Each form miss also counts as
`alias_form_<condition>`, the first fetch condition it failed: `format`
(format or word packing), `type`, `base`, `endian`, `pitch` (pitch or
tiling), `size`, `stacked`, `dimension`, `signed` (sign or integer bits),
`exp_bias`, `mip` (mip level range), `border` (forced border alpha),
`packed_mips` and `swizzle`.
A `Native read mirror:` line follows in each window. Guest memory reads of at
least `native_read_mirror_min_kib` with no unwritten resolve image over them
are mirrored: `hits` and `hit_mb` count reads that returned the retained bytes
of the same range, `watched` and `watched_mb` the hits a watched range served
without comparing, and `misses` and `miss_mb` the reads that copied guest
memory again. Smaller reads are plain copies; those below 1 MiB are counted
with their megabytes by size as `small_4k` (below 4 KiB), `small_64k`,
`small_256k` and `small_1m` (each below that size and at least the previous
one).
A `Native frame reads:` line follows it. It splits the window's
`resolved_reads` by the draw input each read served, for each site with
reads: `code` (the IndexedBuffer families' shader code), `texture`, `vertex`
and `index` (a bound resource's bytes the mirror did not serve),
`mirror_texture`, `mirror_vertex` and `mirror_index` (a resource mirror's
refreshing reads), `other` (any other read inside a family build, such as
another family's shader code) and `outside` (a read outside any build). For
site `<s>`, `read_<s>` counts the reads, `read_<s>_ms` their time,
`read_<s>_lock_ms` their first wait for the assembler's lock and `read_<s>_view_ms`
their guest memory validation, `read_<s>_mb` their bytes, `read_<s>_images`
those an unwritten resolve image overlapped, `read_<s>_flushes` those that
waited for one's submission and readback, and `read_<s>_surfaces` those a
live resolve surface registry entry overlapped. The first read of each site
over a resolve range (an image or an entry) is logged once with its range,
the draw's caller, and the image's and entry's marks and ranges.
`tex_fresh_<source>` and `tex_fresh_<source>_mb` count the texture versions
a build returned that no earlier build did, with their payload megabytes, by
the build's texture reads: `resolve` (one overlapped a resolve range),
`read` (one read guest memory) and `mirror` (the mirrors served them all).

`--native_read_mirror_min_kib=<KiB>` is read only at startup. It sets the smallest guest
memory read the native renderer mirrors, 64 by default. A mirrored range
keeps its last bytes, returns them while guest memory still holds them, and is
watched for guest writes once it compares equal, so later reads skip the
compare until a write is reported. Smaller values trade a copy per read for a
retained entry and watched pages; mirrored bytes stay within the 256 MiB
mirror budget, and a read that does not fit is a plain copy.

`--native_resource_mirrors=true` is read only at startup. It binds the texture,
vertex stream
and index reads of the draw families through a registry of guest resources
instead of copying guest memory per draw; it is on by default, and off changes
nothing. The title's D3D creates and the XGraphics base patches register an
object by its header and a serial after the guest function returns. Resources
made elsewhere, such as the engine resource manager's, register at their first
`SetTexture`, `SetStreamSource` or `SetIndices` bind, which also replaces an
object whose storage words changed. The first bind of a dirty range watches it
for guest writes and copies it once; later binds share that copy. The lock core
(0x82697CA0, reached by every lock entry) and the unlock core (0x82697F38)
dirty the object, and a locked object is not copied; a lock opened before the
object's first registration, or still open when its predecessor with the same
header was released, stays open on it. A free of the predecessor's storage ends
those inherited locks only while they are still pending; a successor registered
first takes them over and keeps them until the guest unlocks them. An unlock of
a pending header closes an inherited lock first. A lock the registry cannot
record for lack of memory makes every bind fall back, counted in
`mirror_fallbacks`, until the device is released. The write watch dirties
ranges written outside a lock, host writes dirty theirs, and a range a resolve
writes stays on the per-draw copy. Scanout resolves bypass that path and do not
notify the registry, as scanout is host-only. Every bind rechecks the draw's
fetch words against the registered ones; a texture ignores its sampler and
clamp bits. A reference-count release, a device release and a physical free
(0x8280DEC8) release objects. The free passes only its address; before it runs,
the hook looks up the allocation's extent in the SDK heap, which is the span
the SDK invalidates on release, and releases every object with a range
intersecting it. When the lookup fails, the span is the freed page or, when
longer, the largest registered range starting there. A released object stops
binding at once and its entry is erased at the next registration or bind;
frames in flight keep the copies they share until the producer retires them by
fence. The full and subrange copies the registry keeps stay within one 1 GiB
budget, and an object keeps at most eight subrange copies, evicting the
oldest. Subrange copies served without being kept and copies that draws in
flight still hold can exceed the budget.
With the registry on, the `Native frame timing:` line adds its counters:
`mirror_hits`, binds served from a copy; `mirror_fallbacks`, binds that copied
guest memory (no registered object, changed words, a locked object, an
unrecorded lock, an unwatchable range, a write during the copy or the
budget);
`mirror_uploads` and `mirror_upload_mb`, ranges copied into the registry;
`mirror_resolved`, binds of resolve destinations; `mirror_watch_dirty`, write
notices over a held copy, which are guest writes outside a lock;
`mirror_bind_registrations`, objects first registered at a bind;
`mirror_view_evictions`, subrange copies evicted for the per-object limit or
the budget and subrange binds served without keeping their copy;
`mirror_free_unsized`, physical frees whose extent the heap lookup could not
give, shown when nonzero; and `mirror_locks_<kind>`, lock core calls by the
locked object's kind (`texture`, `vertex_buffer`, `index_buffer` or
`surface`), shown when nonzero.

### Draw modes

In the default strict mode the first unsupported draw, resolve or unmodeled
record fails the session. For a draw, the log and the window title name its
hook and caller. `native_present_trace` records `completion_failed`. After a
failure the ledger takes no new records. Spans that closed before the failing
one still release once fully owned. The failing span and every later span
never release.

`--native_draw_skip=true` selects a bring-up mode. Draws with no native route
are skipped, and their spans release labeled partial. Skip mode does not inspect
shaders, so it assumes a skipped draw writes no guest memory through memexport.
Frontbuffer resolves release after the matching swap output completes; skipped
source draws keep that frame partial. Unsupported resolves and draws skipped
for any other reason hold their spans. The log names the first partial span and
the first partial `VdSwap`.

`--native_census=true` selects a bring-up census mode and overrides
`--native_draw_skip`. Each draw refusal, unsupported clear or resolve, failed
resolve readback, failed host flush, frame or guest write, and failed native
epoch is logged once per distinct form with a `census:` prefix. The ledger then
applies none of that operation's remaining guest effect and keeps releasing
spans, fences and RPTR. Writes made before the failure, such as earlier runs of
a resolve image or callback scratch words, remain. Work submitted before a
failed flush is still awaited. A failed native epoch or segment drops only its
own work, and segments it left open end in a frame of their target. A frame
outside a segment chain that the renderer refuses at admission, before
submitting a batch, is skipped too; a recording failure before its first
batch reports the same and is skipped as well, although the renderer stops its
stream after it. Any other frame or segment the renderer fails ends the
session, since the renderer stops its draw stream after it. A
`census-summary:` block lists every form with its count every 600 swaps and at
shutdown. Output is not correct in this mode. Failures of the ledger's own
model, such as an unattributed kick, and device loss still end the session,
and a guest wait for a word that a skipped operation would have written can
still hang.

A direct IndexedUpLowerer `0x826A3048` call is held after it returns: the
dispatcher snapshots the 0x3500-byte shadow at its entry, and the draw is
numbered and built at the caller's commit `0x826A2FF0` (on the same device and
`r1`), once the caller has filled the vertices and indices the lowerer
reserved. A lowerer that fails records no draw. A held draw whose commit does
not come before the next draw or the thread's exit, that a commit from another
device or frame ends, or whose commit finds `G+0x3484` moved since the
lowerer returned, is refused as `uncommitted_sink_draw`; a thread's exit only
queues its held draw, which the next draw on any thread, or the dispatcher's
removal, refuses. A commit with no held draw, such
as one after a direct NonindexedSink call, is ignored.

### Registrations, debt and shutdown

The guest re-initializes its ring and writeback during boot. The newest safe
registration wins, and the log records each generation with its previous
values. An unsafe range freezes the retained metadata and fails the session.

When the write pointer published in the current writeback generation has
differed from an unchanged writeback word for two seconds, the log records one
completion-debt error for that generation. It names the ring, writeback, write
and read pointers and swap count, then the oldest unreleased span: its write
pointer, the record holding it and why it cannot release, the requested,
completed and last released fences, where the completed fence is `B+0` and reads
`unknown` until a fence record or fence wait names `B`, the host fence target,
skip counts per reason and any unmodeled emitter. A writeback word on a released
page is reported as unreadable and counts as no progress. `native_present_trace`
records a `completion_debt` event, and the window title names what holds the
span. A closed span held only because its epoch has no registered writeback
is reported the same way after two seconds, once per registration.

Closing the window first closes the native completion source. It refuses new
work, which stops frame assembly without a failure, and it stops reporting host
completion, so spans still waiting on native work never release. The
presentation drain then stops the producer. Closing also closes the ledger, so
no guest write starts afterwards, and stops the GPU Completion and vblank
threads within one second, inside the five-second presentation drain, before
the title is terminated. After the guest exits, the app withdraws the draw
dispatcher before it unbinds the register range, and frees the frame assembler
and completion source only after that withdrawal and the thread joins
succeeded. When a thread misses its one-second join, the app drains
presentation, flushes logs and exits the process without destroying the
Runtime.

The `native_app_hidden` CTest runs `rerevved_native_app_smoke`, which creates a
hidden window and the production presentation session over the admitted fixture,
not the full app. Set `REREVVED_NATIVE_PRESENT_CATALOG` to the admitted catalog
directory before CTest. It renders fixture frames, resizes the window, injects
minimize/restore through the production handlers, composes and clears an
overlay, and closes twice during draining. Its event record distinguishes
occlusion from successful presentation. Missing
fixture setup or unavailable GPU/window support is a CTest skip. A developer
build also registers `native_app_hidden_trace`, which launches the full app with
`window_hidden=true`, `fullscreen=false` and `native_present_script=lifecycle`
and checks the lifecycle in its trace file, including the SDK settings dialog
and SDK keyboard and mouse listener events. A hidden run does not establish
visible desktop presentation or actual OS minimize/restore behavior.

`native_overlay_snapshot` checks immutable handoff and texture ownership without
a GPU. `native_presentation_d3d12_fixture` compares solid, textured and scissored
overlay quads with a CPU blend model over the admitted fixture. Pixels outside
the overlays and opaque overlay pixels must match exactly; translucent RGB
allows one UNORM step for blend rounding. It also checks texture retirement and
restoration of the original backbuffer when the overlay closes.

## Native vertex residency

`--native_vertex_residency` is read only at startup and is off by default. It
keeps immutable
shared vertex owners in persistent GPU memory, so a frame draw whose vertex
span was copied earlier binds that copy instead of writing the bytes to upload
memory again. `--native_vertex_residency_exact` (off by default, no effect
without residency) additionally compares the CPU bytes of every resident span
with a snapshot before each hit and refuses an owner that changed in place.
Both are read once when the native device initializes. With residency off the
renderer allocates no table or chunks, uploads the same bytes, and reports every
new field as zero.

A span is keyed by the device generation, the owner's control block and
pointer, and the offset and length of the span. Upload arenas, frame contexts
and epochs are not part of the key. A new owner with equal bytes, or the same
pointer under another control block, is a different key. The table holds weak
owners only; an owner that expired leaves the table at the next lookup or sweep,
and its space stays in its chunk until the chunk is reclaimed. Only draws that
record inside a render frame use residency; a standalone draw uploads as
before and counts as `outside_frame`.

Storage is four 16 MiB default-heap chunks created when the device initializes,
filled in order with spans aligned to 256 bytes. A miss writes the vertex
bytes to the frame's upload arena as before and then copies them into the
chunk. When every chunk is in use, the oldest chunk is evicted whole, with all
its keys, unless it served a draw in the current or previous swap. A swap is a
terminal presentation draw, whether or not it succeeded; retain-only and prefix
packets do not advance it. A recently used oldest chunk is then replaced only
by the chunk with the fewest live bytes when it holds under 4 MiB and nothing
pending needs it, and its hot spans are admitted again on their next miss. If
no chunk qualifies, or an earlier eviction still waits for its fence, the draw
uploads normally and counts as `cap`. Residency never blocks.

A chunk is reused only after every command list that referenced it was
submitted and the queue fence stamped after the last such list completed. A
chunk whose list executed but whose signal failed is never reused. A device
that cannot prove completion at shutdown keeps its chunks, like its other
submitted resources. A failed recording packet removes every key.

Each copy records one transition of its chunk from copy destination to vertex
buffer state; a chunk already read or written in the same command list first
transitions back to copy destination. Chunks start in the common state, which
promotes implicitly.

The `Native frame timing:` line (`--native_frame_timing=true`) appends:

- `resident_hits`, `resident_bytes_avoided`: draws that bound a resident copy
  and the vertex bytes they did not upload. Index and constant uploads are
  unchanged.
- `resident_misses`, `resident_copy_bytes`: draws that admitted a copy and its
  bytes. The staging write of a miss still counts as vertex upload.
- `resident_fallbacks` and `resident_fallback_<reason>`: draws that used the
  normal upload, with one reason each: `outside_frame`, `owner`, `bounds`,
  `generation`, `mutated`, `cap`, `records`, `allocation`, `scheduling` and
  `disabled_after_init_failure`. The presentation draw has no owner and counts
  once per presented frame as `owner`.
- `resident_bytes`, `resident_entries`, `resident_payload_bytes`,
  `resident_retired_bytes`: gauges for the fixed chunk footprint (64 MiB), live
  keys, their payload bytes, and chunks awaiting reuse.
- `resident_evictions`, `resident_expiries`, `resident_clears`,
  `resident_upload_failures`, `resident_init_failures`: events outside the
  population of recorded draws.

The outcome fields count only draws that recorded successfully and only while
timing is on; the gauges and events always update, and the event totals stay
cumulative across a stream restart. The fixed footprint charges
the renderer's texture storage domain, and the table's metadata (plus the
64 MiB snapshot ceiling in exact mode) charges its owned bytes.

`native_vertex_residency` checks the table with fake fences, generations and
command lists. `native_frame_stream` draws with residency off, on and exact and
compares pixels, uploads and validation messages, and checks the fault, seam,
queue-hold, abandonment and accounting behavior on the device.

## Native gameplay frames

The native renderer assembles frames from recorded guest CPU input frames.
[Native shader artifact generation](native-shaders.md) documents pinned DXIL
and SPIR-V tool inputs. The renderer consumes the DXIL catalog.

Each recorded frame owns its registers, shaders, geometry and CPU texture
versions. GPU-owned texture inputs retain their dependency identities instead
of copied Xenos pixels. Each frame contains a manifest and packed payload
storage with SHA-256 checks for each payload. Registers use checked deltas between
draws and identical payloads share storage within the frame.

Replay converts each recorded frame into a complete native render frame and
submits it to the renderer, which returns after that frame completes.
Packets and the terminal graph share each operation's validated recipe.
Shared payload extents are rechecked, and recorded state and bytes are
independently hashed again at submission. Validated shader versions retain
their immutable bytecode owners. Each operation prepares its pipeline state
once; recording compares that state and exact shader identity, constructing
pipeline descriptors when needed. Raw and externally shared shader inputs
retain byte-sensitive preparation.
Validated CPU texture versions own immutable storage and packed mip/cube layout.
Recording retains device upload layouts and reuses an unchanged uploaded
version without another payload scan. External shared textures retain exact
byte comparison. Draw bindings and resource transitions remain ordered within
their fenced command owners.
Resolve and post-copy clear operations share immutable values prepared from the
recorded copy inputs. The converter compares the copy-state register words and
validates changes, while admitting the current rectangle bytes on every copy.
Only validated scalar state carries across frames; destinations and resource
versions retain their event ownership. Saved raw copy inputs retain strict
register-based preparation.

Owned input events use register checkpoints and deltas in batches of at most
4,096 events. Frames above that size use capture schema 3: their manifest names
ordered, hashed files for batches of events and records the complete terminal
swap.
Schema 2 remains readable. The reader enforces aggregate input byte bounds.

Native command passes contain at most 4,096 commands. Fenced draw groups reuse
descriptor storage after their actual fences. Shared uploads, attachments and
texture versions survive their last native use.
Only a completed frame produces output.

To replay a completed recording, place this recipe alongside its recording and
validated directories of shader catalogs:

```toml
gameplay_sequence_schema_version = 1
recording = "gameplay-recording/session.toml"
shaders = "gameplay-shaders"
```

```powershell
cd <repo>; .\scripts\rexglue.ps1 -Stage Replay -ReplayRecipe out/gameplay-sequence.toml -ReplayOutput out/gameplay-sequence-result.toml
```

Replay verifies the ledger and payloads, submits any prior native producer, then
renders successive frames on one native renderer. It writes an RGB10 output per
frame and a result manifest with hashes, conversion/rendering times and
submission counts. `capture_us` retains the original recording's capture cost;
`conversion_us` measures native frame assembly after loading the saved input.
`render_us` measures submission through completed output, including file writing.
Counters in nanoseconds break down the renderer's CPU work into draw recording,
fence waits and uploads; uploads are included in draw recording and must not be
added to it.
These are CPU measurements, not GPU timestamps or live presentation timings.
Unsupported shader, state or resource dependencies stop replay explicitly.
A completed recording establishes input continuity; image comparison requires
separate validation.

## Build and validation

Use [Development](development.md) for prerequisites, SDK setup, build stages
and checks. The [script reference](../scripts/README.md) describes diagnostic
and packaging tools.

A successful build proves compilation only. Runtime claims require a captured
milestone or another independently reviewable result. See the
[evidence-and-claims guidance](ai_agents/evidence-and-claims.md).

## Dependency and scope rules

- Keep the SDK as a sibling checkout, never a submodule.
- Update the SDK lock whenever the accepted fork commit changes.
- Keep `game/default.xex`, `game/default.xexp`, and extracted version 1.3
  assets ignored.
- Begin visual validation with D3D12 ROV until another path is proven.
- Treat import coverage and code generation as feasibility signals, not runtime
  correctness.
- Preserve ReXGlue's BSD-3-Clause notices in combined distributions.
