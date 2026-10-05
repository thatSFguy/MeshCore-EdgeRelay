# MeshCore EdgeRelay

A personal, car-based MeshCore edge relay. This is a **fork** of the official
[MeshCore](https://github.com/meshcore-dev/MeshCore) repository, modified so a
repeater installed in a parked car can serve its owner's companion devices
without burdening the wider mesh.

**Upstream:** https://github.com/meshcore-dev/MeshCore — all core protocol,
radio, and platform code is theirs, under the MIT license (see `LICENSE`).

## Why this fork exists

A companion node inside an office building can't always reach the fixed
repeater infrastructure directly. A relay on a car in the parking lot bridges
that gap — but a moving relay that behaves like a normal repeater is a bad
mesh citizen: it would learn and advertise paths that go stale, forward other
people's traffic, and add airtime load. So this fork adds a new
`examples/personal_relay` firmware — a **directional edge relay** built on the
stock `simple_repeater` example, which is left untouched:

- **Uplink:** forwards flood traffic that originates directly from a
  whitelisted companion the owner configured. Nothing else goes out.
- **Downlink:** re-emits selected inbound traffic as a single local
  zero-hop copy for nearby owners. Neighbors ignore it (direct route, empty
  path) and no return path is learned through the car.
- **Everything else:** dropped.

## What changed vs upstream

All changes are confined to `examples/personal_relay/` (plus this README,
`build.sh`, one new build environment, and one new workflow). Core protocol
code in `src/` and the stock `simple_repeater` example are untouched.

| Area | Change |
|---|---|
| `EdgePolicy.h` / `EdgePolicy.cpp` (new) | Packet classifier, persisted configuration, per-class rate limiting, counters. |
| `MyMesh::onRecvPacket()` | Every received packet is classified **before** stock handling: `EDGE_STOCK` (proceed), `EDGE_LOCAL_COPY` (re-emit once as zero-hop direct, drop original), or `EDGE_DROP`. |
| `MyMesh::allowPacketForward()` | Final deny guard — stock forwarding is permitted only for packets the policy approved. |
| `sendSelfAdvertisement()` | No-op. The car **never** advertises, so the mesh never learns routes through a moving node. |
| `updateAdvertTimer()` / `updateFloodAdvertTimer()` / `loop()` | Advert timers permanently stopped; timer blocks removed from `loop()`. Boot adverts are also suppressed via the no-op. |
| `handleCommand()` | New `edge` CLI for configuration over USB serial (see below). |

### Packet policy (default)

| Traffic | Flood | Direct (car is next hop) |
|---|---|---|
| `TXT_MSG`, `REQ`, `RESPONSE`, `PATH` from a whitelisted owner (heard directly) | Forward normally | — |
| Same, addressed to an owner (already circulating) | Re-emit locally as one zero-hop copy | — |
| Same, unrelated to owners | Drop | Drop |
| `GRP_TXT` / `GRP_DATA` on a configured channel | Owner's own transmission: forward; inbound: local copy | Forward if channel configured |
| `ADVERT` from an owner | Drop (never export owner adverts) | — |
| `ADVERT` from anyone else | Drop (mirroring opt-in only, off by default) | — |
| `ACK` | Drop (forwarding opt-in only, off by default) | Same |
| `ANON_REQ` | Drop | Link-local zero-hop only: info queries, password login, and the resulting admin session from authenticated clients (stock crypto auth still enforced on every packet; flood/multi-hop stays dropped) |
| `TRACE`, `MULTIPART`, `RAW_CUSTOM`, unknown | Drop | Drop |
| `CONTROL` | Drop | Drop (except link-local zero-hop, e.g. discovery replies, which can't propagate) |

Notes:

- **Source authentication is weak by design of the wire format.** Ordinary
  packets expose only a 1-byte public-key prefix, so owner matching is
  prefix-based and spoofable. This firmware does not claim cryptographic
  attribution of uplink traffic — it fails closed and treats the prefix
  check as a routing hint, not authentication. Group traffic is matched on
  the 1-byte channel hash you explicitly configure.
- Local copies preserve payload bytes and payload type exactly; only the
  route is reframed to zero-hop direct. Public/channel messages stay
  public/channel messages — no per-companion DMs are created. The relay
  never re-signs or re-encrypts anything.
- Dedup uses the same seen-table as stock MeshCore (hash over payload type
  + payload), so one inbound message yields at most one local copy.
- Local copies are rate-limited (30/minute, sliding window).
- App login and remote administration are allowed, but only from direct radio
  range (zero-hop): the admin password is still required, and stock's
  cryptographic authentication applies to every session packet (the 1-byte
  prefix the classifier sees is only a routing hint). Flood and multi-hop
  admin traffic is dropped, so the node cannot be administered through the
  wider mesh. USB serial remains available as the primary console.
- With no valid policy file on the filesystem, the node boots **receive-only**
  (fail closed) until you configure owners via the CLI.

### CLI (`edge`)

Over USB serial at 115200 baud (also works on the ethernet console where enabled):

```
edge status                  - show policy + counters
edge owner list              - list owner pubkeys
edge owner show <idx>        - show one owner pubkey in the reply
edge owner add <64 hex>      - add owner, save
edge owner del <64 hex>      - remove owner, save
edge chan list               - list mirrored channel hashes
edge chan add <ch>           - add channel, save
edge chan del <ch>           - remove channel, save
  <ch> = 2 hex channel hash, channel key (32/64 hex or base64), or 'public'
edge opt mirror_adverts 0|1  - remote advert mirroring, save (default 0)
edge opt fwd_acks 0|1        - ACK forwarding, save (default 0)
```

Configuration persists to `/edge_policy` on the device filesystem.

#### Adding your companions

1. In the MeshCore phone app, open the companion's node details and copy its
   public key (64 hex characters).
2. In the serial console: `edge owner add <paste the key>`.
3. Confirm with `edge owner list` and `edge status`.

Repeat for each companion you own (up to 8). Until at least one valid policy
is saved, the node stays receive-only.

## Build and flash

Same as upstream. From this directory:

```bash
pio run -e <your_target>        # build
pio run -e <your_target> -t upload   # flash
```

Every board with a stock repeater target also has a personal relay target:
`<board>_personal_relay` (e.g. `RAK_4631_personal_relay`, `Heltec_v3_personal_relay`).
Each one mirrors its board's normal repeater target with only the example
swapped to `examples/personal_relay` and the advert name changed to
`<board> Personal Relay`. Or run the **Build Personal Relay Firmwares**
workflow from the Actions tab and download the artifact (`.uf2` for the
RAK4631).

See the [upstream README](https://github.com/meshcore-dev/MeshCore#readme)
for hardware compatibility, the web flasher, clients, and unit tests
(`pio test -e native` covers `src/`, which this fork does not modify).

## Goals and non-goals

Goals: give the owner's companions one extra hop into the mesh; never
poison other repeaters' learned paths; add no measurable load to the wider
mesh; stay a single-purpose, reviewable change on top of stock code.

Non-goals: general-purpose repeating, movement/parked detection, GPS,
telemetry, administration beyond direct radio range, or any change to the
MeshCore wire protocol.

## License

MIT — same as upstream. See `LICENSE`.
