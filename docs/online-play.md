# Online play

ReRevved Online uses System Link through an online server that one player
runs. Everyone uses the same ReRevved release and an invite code from the
same host. This is not an Xbox Live connection. On Windows the host needs
nothing but the game.

## Host or join

1. Set up your [game content](getting-started.md) before hosting.
2. The host presses **F1 > Online > Host Game**. The game restarts,
   runs the online server itself and asks the router to open its UDP port.
   **Hosting Status** on the same page shows when friends can join, or why
   hosting failed. **Stop Hosting**, which replaces Host Game while hosting,
   or quitting ends it and removes the port.
3. Friends click Join on the host's Discord profile, or the host sends them
   an invite from a Discord chat. For friends without
   Discord, the host presses **Copy Friends' Code** and sends the code
   privately; they paste it into **F1 > Online > Invite Code**, leave
   **Direct LAN** off, then save and restart.
4. Open **Multiplayer > Player Match**. The host creates a game with an open
   public slot; the other players join it from the game list.

Use [ONLINE.txt](../scripts/packaging/ONLINE.txt) for the full
hosting and joining steps, Discord requirements, status messages, and
troubleshooting. That plain-text guide ships in the Windows download.

## If automatic hosting fails

The Windows download also includes `host-online.cmd`, which hosts from a
script window instead: close ReRevved and run it. It can ask for an address
friends can reach when the router does not help.
The packaged guide covers a different host, an overlay VPN (a private network
over the Internet) for a connection that stays unreachable even with a
forwarded port, such as one Internet address shared by many customers, and
`host-manual.cmd` for manual UDP port forwarding. The manual script runs the online server but
does not start the game or set up Discord's advertised address; use its invite
codes and start ReRevved yourself.

## Gameplay settings and mods

For each new multiplayer game, the host supplies AI civilization count, Resources,
Raging Barbarians, One City Challenge, the five AI bonus percentages,
Earliest Domination, Turn Limit, Starting Era, Technology Cost, AI difficulty and Game of the Week choices. AI civilization
count goes up to 6 online; it is reduced so the humans, AI civilizations and Barbarians fit in eight participants (AI = min(request, 7 - humans)). These values apply to that game without changing
the other players' saved settings. Set the host's choices before starting the game.
Always At War is a single-player setting; network games ignore it. Network
games always use normal combat speed and unit movement and show Spaceship Scenes.
The Tower of Babel and Tesla's Peace Ray artifacts affect only the player who
found them.
Joined players see these settings locked in F1. In the lobby they show the values
from when the host opened it, and the host can still change them until the game starts.
The Apollo Discovery Bonuses fix is always on and applies in network games
too (Game of the Week excepted). Each player's game grants the bonus on its own
timing, without waiting for the others.

While Raging Barbarians is on, Barbarians cannot capture a civilization's
capital before 3000 BC. Every player's game must support this rule, so older
builds cannot join such a game.

Every player must apply the same Lua packages, versions, order, and rule content
before joining. The game compares active package IDs and versions plus a
checksum of the Lua loadout's content. Native DLL loadouts cannot pass this
content check.
The host does not install or enable packages on other players' machines.
A player whose loadout differs sees a warning on joining and can't ready up, so
the host's countdown can't start until that player matches or leaves.

New-game setup waits up to 15 seconds for the host's verified settings for
that game. Missing, old, invalid, incomplete, or incompatible settings block
setup, as do different or unverified mod rules. The game shows **Can't start this game**
and logs the reason. Correct the settings or loadout before retrying.
If the settings cannot be confirmed as current, create the game again. See
[Mods](mods.md#enable-or-change-mods) to apply Lua changes after leaving a game.

This synchronization covers new games. Multiplayer save loading has not been
certified to give every player the host's saved gameplay settings. See the
[save settings file](rexglue-runtime.md#save-sidecar) for current load behavior.

## Keep the test private

Invite codes contain the password for your game. Share them only with the
players joining it. Game traffic through the online server is not encrypted
or protected against tampering. Keep the online server open only while testing
and check that any router mapping is removed afterwards. Review logs and
screenshots before sharing them.

For two game copies on one PC, use separate profiles and player IDs as shown
in the packaged guide. Do not copy one online profile's config into the other.
