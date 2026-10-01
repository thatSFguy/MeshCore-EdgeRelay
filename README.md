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

## Upstream base and versioning

Based on **MeshCore v1.17.1** (tag `repeater-v1.17.1`, commit `d929643`).
Core code in `src/` and the stock examples are unmodified from that tag;
upstream `main` has only documentation changes since.

The personal relay reports its version as `<upstream>-er-<commit>`, e.g.
`v1.17.1-er-1911a2f`: the MeshCore release it is built on, `er` for edge
relay, and the fork commit it was built from (just `v1.17.1-er` for local
builds outside `build.sh`). The `ver` CLI command and the app's owner info
show the full string; the OLED shows the upstream part. `build.sh` names the
firmware files the same way, e.g. `ProMicro_personal_relay-v1.17.1-er-1911a2f.uf2`,
so a file and the device running it always show the same version. When merging a newer
upstream release, bump `UPSTREAM_VERSION` in
`examples/personal_relay/MyMesh.h`.

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
| `onRecvPacket()` / `cancelEchoedForward()` | Echo suppression: owner uplinks are held briefly and cancelled if another repeater re-floods them first. |
| `HomePresence.h` (new), `logRx()` / `loop()` | Home detection: RSSI of the configured home node pauses all relaying while parked at home. |

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
edge chan add <2 hex>        - add channel, save
edge chan del <2 hex>        - remove channel, save
edge opt mirror_adverts 0|1  - remote advert mirroring, save (default 0)
edge opt fwd_acks 0|1        - ACK forwarding, save (default 0)
edge echo                    - show echo suppression settings + counter
edge echo on|off             - echo suppression, save (default on)
edge echo wait <0-20>        - extra uplink hold, in packet airtimes, save (default 4)
edge home                    - show home detection state + live RSSI
edge home set <hex> [enter exit timeout_min]
                             - home node pubkey or prefix, save (defaults -60 -80 10)
edge home off                - disable home detection, save (default off)
```

Configuration persists to `/edge_policy` on the device filesystem. Older
firmware does not know the `echo_*` / `home` directives and treats such a
file as invalid (receive-only), so after a downgrade, re-run your setup.

#### Echo suppression

When your companion is close enough to reach a real repeater on its own, the
relay's copy of your uplink is redundant. With echo suppression on (the
default), an owner uplink forward is held for a few extra packet airtimes
(`edge echo wait`, default 4) on top of the normal random retransmit delay.
If, during that hold, the relay hears another repeater re-flood the same
packet, the mesh already has it, so the queued forward is cancelled. If no
echo arrives, the relay forwards as normal.

This decides per packet, without thresholds: next to infrastructure the relay
stays quiet, deep inside a building it bridges. It costs a little extra
latency on uplink only (roughly 1–6 s depending on radio preset). Downlink
zero-hop copies are never cancelled, since the relay cannot tell whether
your companion heard the repeater. `edge status` reports cancellations as
`echo_cancel` (they are still counted in `uplink_fwd`, which counts uplink
packets accepted for forwarding).

#### Home detection (pause when parked at home)

When the car is parked at home, your base station already covers you, so the
relay can stop entirely: no uplink forwards and no downlink local copies.
Admin login over zero-hop still works.

The relay decides it is home from the RSSI of packets your **home node**
transmits: its zero-hop adverts (matched on the full public key) and any
flood it repeats (matched on the last path entry, i.e. the node that just
transmitted). The threshold is deliberately strict, so only the driveway
counts, not "somewhere in the home repeater's coverage":

- **Home** when the average of the last 4 home-node packets (at least 3)
  reaches `enter` (default -60 dBm).
- **Stays home** while that average stays at or above `exit` (default
  -80 dBm). The gap between the two prevents flapping.
- **Away** as soon as the average drops below `exit` (driving off, signal
  fading), or when no strong home-node packet has been heard for `timeout`
  minutes (default 10). Samples older than the timeout are discarded.

Setup:

1. Copy your home repeater's public key from the app (the full 64 hex chars is
   best: a short prefix can collide with another node's path hash).
2. `edge home set <key>` (or `edge home set <key> -55 -80 15` to choose
   thresholds and timeout).
3. Park where you normally do and run `edge home` a few times. It shows the
   live average (`avg`), latest reading (`last`), sample count, and the age of
   the last sample. Pick `enter` a few dB below what you see parked, and
   `exit` comfortably below that.

Example readings while parked (it needs 3 samples before it can switch to
home, so expect a few minutes after arriving):

```
edge home
  -> OK - away node:b389548d enter:-60 exit:-80 timeout:10m avg:-37 last:-42 n:2 age:34s held:0
edge home
  -> OK - HOME node:b389548d enter:-60 exit:-80 timeout:10m avg:-38 last:-40 n:4 age:58s held:0
```

While home, every forward or local copy the relay would otherwise have sent
is counted in `held` (shown as `home_held` in `edge status`) instead of being
transmitted. Send a message from your companion while parked to confirm it
goes up.

Notes: use RSSI rather than SNR, because SNR saturates at close range. The
car body and garage walls can shift readings by 10–15 dB, so tune it in place.
If the home node is quiet (few adverts, little traffic to repeat), make the
timeout longer than its advert interval.

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

Non-goals: general-purpose repeating, movement detection (beyond the
optional "parked at home" check), GPS,
telemetry, administration beyond direct radio range, or any change to the
MeshCore wire protocol.

## License

MIT — same as upstream. See `LICENSE`.
