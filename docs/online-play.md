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

## Keep the test private

Invite codes contain the room secret. Share them only with the players joining
your game. Relay traffic is not encrypted or protected by a payload integrity
check. Keep the relay open only while testing and check that any router
mapping is removed afterwards. Review logs and screenshots before sharing them.

For two game copies on one PC, use separate profiles and player identities as
shown in the packaged guide. Do not copy one online profile's config into the
other. Agree on the same enabled mods before playing together; see
[Mods](mods.md).
