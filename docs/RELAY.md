# System Link over the Internet (the relay)

The original System Link feature plays over a local network. Reforged can carry it over the Internet through a small **relay server**: every console sends its network
frames to the relay, and the relay passes them on to the other consoles in the same **room**. To the game it looks like everyone is on one LAN: one player creates a
System Link game in the game's own menu, the others see it in the list and join.

This works between two copies of the runtime today (tested desktop to desktop through a relay, including finding and joining a game). It has **not** been tested on an
iPhone yet, and it has not been tested with more than two players or over a slow connection. Treat it as an early feature.

## What you need
1. **A relay server** that everyone can reach (below). One person runs it; it uses almost no resources.
2. **The same two values on every player's device:** the relay's address and a room name.
3. The game's own System Link menu to create and join games.

## In the iOS app
Open the library, tap **Settings**, and in **System Link over the Internet**:
- turn on **Use a relay server**,
- **Server**: the relay's address, such as `relay.example.com:3075` (the port defaults to 3075),
- **Room**: a name everyone who plays together agrees on. Use something nobody would guess.

The settings apply the next time a game starts. On a computer, set the environment variables `XBOX_RELAY=<host:port>` and `XBOX_RELAY_ROOM=<room>` before starting the
runtime. Each console gets its own network address automatically (derived from its own console address); `XBOX_GUEST_IP` can set one by hand.

## Running a relay
The relay is one Python file, `tools/relay.py`, with no dependencies.
```
python tools/relay.py --port 3075
```
or in Docker (from the repository folder):
```
docker compose -f tools/relay/docker-compose.yml up -d --build
```
Options: `--max-room` (consoles per room, default 16), `--max-rooms` (default 256), `--rate` (frames per second per console, default 600).

### Making it reachable
- **At home:** run it on a computer or home server that is always on, and forward **UDP port 3075** on your router to it. Players use your public address (a free
  dynamic-DNS name is better than an IP that changes). Allow UDP 3075 in the machine's firewall.
- **On a rented server (a small VPS):** run it there. Nothing to forward; open UDP 3075 in the provider's firewall.
- **Without opening any port:** put the relay and the players on a private network such as Tailscale, and give players the relay's private address.

Check it works: the relay prints `join` when a console connects and `drop` when one goes quiet for 30 seconds.

## Hosting from a device (no separate server)
The runtime contains the same relay. In the iOS app's Settings, turn on **Host a relay on this device**: the others use this device's address (shown in the
settings, with port 3075) and the same room name, and you play on this device as well. Everyone must be able to reach the host: the same Wi-Fi works, and so does a
private network such as Tailscale from anywhere. A phone on mobile data cannot accept connections from the Internet, and the relay stops when the app closes. On a
computer set `XBOX_RELAY_HOST=<port>` (and optionally `XBOX_RELAY_ROOM`).

## Using your own domain
A relay needs an address that does not change. If you have a domain:
1. Make a name such as `relay.yourdomain.com` with an **A record** pointing at the machine's public address (a rented server's address, or your home address).
2. At home, keep the record current with your DNS provider's dynamic-DNS feature or a small updater (for example `ddclient`). If the provider offers a "proxy", turn it
   **off** for this record (DNS only): proxies for web traffic do not carry UDP.
3. Players enter `relay.yourdomain.com:3075`. Pointing a name at your home address makes that address easy to find; a small rented server avoids that.

## A default relay in the app
The companion can build a relay address into a personal app so players only have to pick a room: `reforged-companion --relay relay.yourdomain.com:3075 --relay-room
<name>`. The app uses it until the player changes or turns it off in Settings. Only apps built with that option use it, so nobody is pointed at your server by accident.
The released app has no default relay.

## Safety, privacy and limits
- **Nothing is encrypted.** The relay (and everyone in your room) can see the game's network traffic, which can include player names. Do not use it for anything private.
- **There are no accounts.** Anyone who knows the room name can join it. Pick a private name and share it only with people you trust.
- **The relay does not look inside frames, store anything, or log contents.** It logs joins and leaves with a short hash of the address, never the address itself.
- **Abuse limits:** a room holds at most 16 consoles, at most 256 rooms exist at once, a console can send at most 600 frames a second, and frames longer than 1600
  bytes are dropped.
- **It only relays between consoles running Reforged.** It never connects to any official online service.
- Running a public relay means other people's traffic passes through your machine and your address is visible to them. Run it for friends, not the whole Internet.

## Troubleshooting
| Symptom | What to check |
|---|---|
| The game list stays empty | Both devices use exactly the same room name (names are case sensitive) and the same server. The relay should print a `join` for each. |
| The relay prints nothing | The address or port is wrong, or UDP 3075 is blocked or not forwarded. A VPN on the phone can also block it. |
| Players join but the game stutters | Latency to the relay. Pick a relay near the players. |
| "Unable to join" | Make sure both players run the same build of the game. |

## How it works (for developers)
`XRL2` datagrams: 4-byte magic, a 16-byte room key (FNV-1a 64 of the room name, twice, with two offsets: `tools/relay.py` and `src/net.cpp` implement the same
function), then one Ethernet frame. A datagram with no frame registers a console and keeps it alive (sent every 4 seconds). The relay learns which console owns which
address from the frames' source addresses and sends unicast frames only there; broadcasts go to everyone else in the room. The older `XRL1` form (no room) still works
and uses one shared room. The relay's rules are covered by `tests/relay_test.py`.
