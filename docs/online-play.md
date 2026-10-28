# Online play

ReRevved uses System Link over a self-hosted relay. One player runs the relay;
everyone uses the same ReRevved release and relay room. This is not an Xbox
Live connection. The Windows download includes the hosting tools.

## Host or join

1. Set up your [game content](getting-started.md) before hosting.
2. The host closes ReRevved and runs `host-online.cmd` from the extracted
   package. It tries UPnP, creates invite codes, starts the relay, and launches
   the game connected to it.
3. Friends use Discord Join or the private invite code. With a code, paste it
   into **F1 > Online > Invite Code**, leave **Direct LAN** off, then save and
   restart.
4. Open **Multiplayer > Player Match**. The host creates a game with an open
   public slot; the other players join it from the game list.

Use [ONLINE.txt](../scripts/packaging/ONLINE.txt) for the full
hosting and joining steps, Discord requirements, status messages, and
troubleshooting. That plain-text guide ships beside the hosting scripts.

## If automatic hosting fails

The packaged guide covers a different host, an overlay VPN for CGNAT or double
NAT, and `host-manual.cmd` for manual UDP forwarding. The manual script runs
the relay but does not start the game or set up Discord's advertised address;
use its invite codes and start ReRevved yourself.

## Gameplay settings and mods

For each new multiplayer game, the host supplies AI civilization count, Resources,
Raging Barbarians, One City Challenge, the five AI bonus percentages,
Earliest Domination, Game of the Week choices, and combat speed. AI civilization
count is capped at 4 online. These values apply to that game without changing
the other players' saved settings. Set the host's choices before starting the game.

While Raging Barbarians is on, Barbarians cannot capture a civilization's
capital before 3000 BC. Every peer must support this rule. The host payload
requires the capital-grace capability bit whenever its Raging bit is set.
Older builds reject that unknown bit. A payload with only one of those bits
set is rejected as noncanonical.

Every player must apply the same Lua packages, versions, order, and rule content
before joining. The game compares active package IDs and versions plus a digest
of the decoded Lua loadout. Native DLL loadouts cannot pass this content check.
The host does not install or enable packages on other players' machines.

New-game setup waits up to 15 seconds for authenticated settings prepared for
that game. Missing, old, invalid, incomplete, or incompatible settings block
setup, as do different or unverified mod rules. The game shows **Cannot start
this game** and logs the reason. Correct the settings or loadout before retrying.
If session freshness cannot be established, recreate the session. See
[Mods](mods.md#enable-or-change-mods) to apply Lua changes after leaving a session.

This synchronization covers new games. Multiplayer save loading has not been
certified to restore one shared host gameplay snapshot. See the
[save sidecar contract](rexglue-runtime.md#save-sidecar) for current load behavior.

## Keep the test private

Invite codes contain the room secret. Share them only with the players joining
your game. Relay traffic is not encrypted or protected by a payload integrity
check. Keep the relay open only while testing and check that any router
mapping is removed afterwards. Review logs and screenshots before sharing them.

For two game copies on one PC, use separate profiles and player identities as
shown in the packaged guide. Do not copy one online profile's config into the
other.
