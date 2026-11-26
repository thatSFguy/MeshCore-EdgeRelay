# CLAUDE.md

Fork of upstream MeshCore (https://github.com/meshcore-dev/MeshCore) that adds
one firmware: `examples/personal_relay`, a **car-based directional edge relay**
for the owner's own companion devices. It is in daily use and working well, so
**preserving its behavior is the top priority** for any change, especially
upstream merges.

`README.md` is the authoritative spec (packet policy table, `edge` CLI, home
detection, echo suppression). Read it before changing the relay.

## Intent

A companion inside a building can't reach the mesh; a relay in the parked car
bridges that one hop. A moving node must not act like a normal repeater, so:

- **Uplink:** only floods heard *directly* (empty path) from a whitelisted
  owner are forwarded. Nothing else goes out.
- **Downlink:** traffic addressed to an owner (or on a configured channel) is
  re-emitted as a single **zero-hop direct** local copy. Neighbors ignore it and
  no path through the car is ever learned.
- **Everything else is dropped.** Fail closed: no valid `/edge_policy` file
  means receive-only.

## Invariants (do not break)

- The relay **never advertises on its own**: `sendSelfAdvertisement()` is a
  no-op and the advert timers are dead. Only manual `advert.zerohop` /
  `edge advert` (zero-hop) is allowed; flood `advert` is refused. So the
  `advert_interval` / `flood_advert_interval` prefs have no effect here and must
  not gate anything else (e.g. upstream v1.18's `isHiddenNode()` check on
  discovery is intentionally *not* ported).
- Owner adverts are never exported; the mesh must never learn a route via the car.
- Every packet passes `EdgePolicy::classify()` in `MyMesh::onRecvPacket()`
  before stock handling, and `allowPacketForward()` is the final deny guard.
- Admin login and sessions are allowed only link-local (zero-hop), with stock
  password + crypto. Zero-hop admin must keep working even with no or invalid
  policy, so the node can always be recovered over LoRa.
- Local copies preserve the payload bytes and type exactly; nothing is re-signed,
  re-encrypted or converted to a DM.
- Owner matching is by 1-byte hash prefix: a routing hint, not authentication.
- `/edge_policy` format is versioned (`ER1`); any parse error invalidates the
  whole file (fail closed). Keep new directives optional with defaults.

## Fork boundaries

- Changes live in `examples/personal_relay/`, `<board>_personal_relay` envs in
  `variants/*/platformio.ini`, `build.sh`, the personal relay workflow, and README.
- `src/` and the stock examples stay identical to upstream, except the portable
  `ltoa()` replacement in `src/helpers/TxtDataHelpers.cpp` (STM32 builds).
- Also fork-only: `LilyGo_TLora_V2_1_1_6_companion_radio_ble_gps` (stock
  companion + external GPS on GPIO 15/13, TX/RX auto-detected at boot by
  `LilyGoTLoraBoard::begin()`) in `variants/lilygo_tlora_v2_1`, with a
  `LocationProvider::sendSentence()` definition in its `target.cpp` working
  around an upstream link bug, and `GPS_ENABLED_BY_DEFAULT` (opt-in flag, only
  this env) in `examples/companion_radio/MyMesh.cpp`.
- Also fork-only: the K.I.S.S. Messenger (`LilyGo_TLora_V2_1_1_6_kiss_messenger`):
  a standalone on-device chat UI in `examples/companion_radio/ui-kiss/` (CardKB
  keyboard, OLED, buzzer GPIO 15, vibration GPIO 13, no BLE), plus two small
  public additions to the companion's `MyMesh`: `floodAdvert()` and
  `persistContacts()`. Simple mode: no auto-added contacts; DMs are set up by an
  "@[name] DM request" / "@[name] DM accepted" exchange in the group chat.
- Every board's `<board>_personal_relay` env mirrors its `_repeater` env with
  only the example and `ADVERT_NAME` changed.

## Upstream merge checklist

1. `git fetch upstream` (remote: `meshcore-dev/MeshCore`); merge `upstream/main`
   on a branch.
2. Diff `examples/simple_repeater` between the old and new merge-base and port
   each change into `personal_relay` (it was forked from that example), checking
   each against the invariants above. Note any deliberate skips in the code.
3. Review `src/Mesh.*`, `src/Dispatcher.*`, `MeshCore.h`, `CommonCLI.*`,
   `ClientACL.*`, `RegionMap.*` and the packet manager for changes affecting the
   relay's overrides, echo cancellation (outbound queue access) or the policy.
4. Add a `_personal_relay` env for any new board with a repeater env, and check
   that existing ones still mirror their repeater env.
5. Bump `UPSTREAM_VERSION`, `FIRMWARE_VERSION`, `FIRMWARE_BUILD_DATE` in
   `examples/personal_relay/MyMesh.h`, and the version text in README.
6. Build a spread of targets (nRF52, ESP32, STM32, RP2040):
   `pio run -e ProMicro_personal_relay` etc.
