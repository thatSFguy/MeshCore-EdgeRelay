#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/IdentityStore.h>   // FILESYSTEM

// Maximum number of owner identities (companion devices) this relay serves.
#ifndef EDGE_MAX_OWNERS
#define EDGE_MAX_OWNERS  8
#endif
// Maximum number of group channel hashes mirrored to owners.
#ifndef EDGE_MAX_CHANNELS
#define EDGE_MAX_CHANNELS  8
#endif

// Filesystem path of the persisted policy.
#define EDGE_POLICY_FILE  "/edge_policy"

// Local-copy rate limit: at most this many zero-hop copies per window.
#define EDGE_LOCAL_COPY_MAX    30
#define EDGE_LOCAL_COPY_WINDOW_MS  60000UL

// Echo suppression: an owner uplink forward is held for this many airtimes
// (of the packet) on top of the normal retransmit delay, giving the mesh a
// chance to echo it first. Range accepted by the CLI / policy file.
#define EDGE_ECHO_WAIT_DEFAULT  4
#define EDGE_ECHO_WAIT_MAX      20

// Home detection: the owner's home node, identified by a public-key prefix.
#define EDGE_HOME_PREFIX_MAX     PUB_KEY_SIZE
#define EDGE_HOME_ENTER_DEFAULT  -60   // dBm
#define EDGE_HOME_EXIT_DEFAULT   -80   // dBm
#define EDGE_HOME_TIMEOUT_DEFAULT 10   // minutes
#define EDGE_HOME_TIMEOUT_MAX    240   // minutes

/**
 * Edge-relay packet policy.
 *
 * This is the "directional firewall" for the personal edge relay: it decides,
 * for every received packet, whether stock MeshCore handling may proceed
 * (EDGE_STOCK), whether the packet's payload should be re-emitted once as a
 * local zero-hop copy for nearby owners (EDGE_LOCAL_COPY), or whether the
 * packet must be dropped outright (EDGE_DROP).
 *
 * The classifier is intentionally pure (no side effects): rate-limit tokens
 * are consumed separately via tryLocalCopy() only when a copy is actually made.
 */
enum EdgeAction {
  EDGE_STOCK,       // hand to stock Mesh::onRecvPacket()
  EDGE_LOCAL_COPY,  // re-emit payload once as zero-hop direct; drop the original
  EDGE_DROP         // drop the packet entirely
};

struct EdgePolicyStats {
  uint32_t n_owner_uplink;  // owner floods forwarded via the stock path
  uint32_t n_local_copy;    // inbound floods re-emitted as local zero-hop copies
  uint32_t n_direct_fwd;    // owner-associated direct packets forwarded via stock
  uint32_t n_dropped;       // packets dropped by policy
  uint32_t n_echo_cancel;   // queued uplink forwards cancelled: another repeater echoed them first
  uint32_t n_home_held;     // forwards / local copies withheld because the relay is parked at home
};

class EdgePolicy {
  uint8_t _owner_keys[EDGE_MAX_OWNERS][PUB_KEY_SIZE];
  uint8_t _num_owners;
  uint8_t _channel_hashes[EDGE_MAX_CHANNELS];
  uint8_t _num_channels;
  bool _mirror_adverts;   // re-emit selected remote adverts locally (default: false)
  bool _fwd_acks;         // forward ACKs naming this node (default: false)
  bool _flood_login;      // accept a login sent as a flood if heard directly (default: false)
  bool _echo_suppress;    // cancel a queued uplink forward if the mesh echoes it first (default: true)
  uint8_t _echo_wait;     // extra uplink hold, in packet airtimes (default: EDGE_ECHO_WAIT_DEFAULT)
  uint8_t _home_prefix[EDGE_HOME_PREFIX_MAX];  // home node pubkey prefix
  uint8_t _home_prefix_len;                    // 0 -> home detection off (default)
  int16_t _home_enter;    // dBm: average at/above this -> home
  int16_t _home_exit;     // dBm: average below this -> away
  uint8_t _home_timeout;  // minutes without a strong home sample -> away
  bool _valid;            // false -> fail closed (receive-only)

  void resetConfig();               // all settings back to defaults (not _valid)
  bool parseFile(FILESYSTEM* fs);   // read EDGE_POLICY_FILE into the settings

  // sliding-window rate limiter for local copies
  uint32_t _rl_start_ms;
  uint16_t _rl_count;

public:
  EdgePolicy();

  // --- persistence ---
  bool load(FILESYSTEM* fs);   // false -> policy invalid, fail closed
  bool save(FILESYSTEM* fs);

  bool isValid() const { return _valid; }

  // --- owner / channel management ---
  int addOwner(const uint8_t pubkey[PUB_KEY_SIZE]);  // returns idx, or -1 if full/duplicate
  bool removeOwner(const uint8_t pubkey[PUB_KEY_SIZE]);
  int addChannel(uint8_t hash1);                     // returns idx, or -1 if full/duplicate
  bool removeChannel(uint8_t hash1);
  void setMirrorAdverts(bool v) { _mirror_adverts = v; }
  void setFwdAcks(bool v) { _fwd_acks = v; }
  bool getMirrorAdverts() const { return _mirror_adverts; }
  bool getFwdAcks() const { return _fwd_acks; }
  void setFloodLogin(bool v) { _flood_login = v; }
  bool getFloodLogin() const { return _flood_login; }
  void setEchoSuppress(bool v) { _echo_suppress = v; }
  bool getEchoSuppress() const { return _echo_suppress; }
  bool setEchoWait(int v);   // false if out of range
  uint8_t getEchoWait() const { return _echo_wait; }

  // Home node: prefix_len 1..32 bytes; enter > exit, both -140..0 dBm;
  // timeout 1..240 minutes. Returns false (unchanged) if anything is invalid.
  bool setHome(const uint8_t* prefix, uint8_t prefix_len, int enter, int exit, int timeout_min);
  void clearHome() { _home_prefix_len = 0; }
  bool isHomeEnabled() const { return _home_prefix_len > 0; }
  const uint8_t* getHomePrefix() const { return _home_prefix; }
  uint8_t getHomePrefixLen() const { return _home_prefix_len; }
  int16_t getHomeEnter() const { return _home_enter; }
  int16_t getHomeExit() const { return _home_exit; }
  uint8_t getHomeTimeout() const { return _home_timeout; }
  // True if this received packet was put on air by the home node: a flood whose
  // last path entry is the home node's hash, or the home node's own advert.
  bool isFromHome(const mesh::Packet* pkt) const;
  // Parse "<hex prefix> [<enter> <exit> <timeout>]" (CLI and policy file).
  bool parseHome(const char* args);
  uint8_t getNumOwners() const { return _num_owners; }
  uint8_t getNumChannels() const { return _num_channels; }
  const uint8_t* getOwnerKey(int i) const { return _owner_keys[i]; }
  uint8_t getChannelHash(int i) const { return _channel_hashes[i]; }

  bool isOwnerHash(uint8_t h) const;
  bool isOwnerPubkey(const uint8_t pubkey[PUB_KEY_SIZE]) const;
  bool isChannel(uint8_t h) const;

  // --- classification ---
  // self_hash/self_hash_len: this node's path hash, for direct next-hop checks.
  EdgeAction classify(const mesh::Packet* pkt, const uint8_t* self_hash, uint8_t self_hash_len) const;

  // Final deny guard for allowPacketForward(): true only if stock handling is permitted.
  bool checkForward(const mesh::Packet* pkt, const uint8_t* self_hash, uint8_t self_hash_len) const {
    // Anonymous requests are only ever answered, never forwarded: stock would
    // otherwise re-flood a flood login that did not decrypt.
    if (pkt->getPayloadType() == PAYLOAD_TYPE_ANON_REQ) return false;
    return _valid && classify(pkt, self_hash, self_hash_len) == EDGE_STOCK;
  }

  // Rate limiter: call when actually making a local copy.
  bool tryLocalCopy(uint32_t now_ms);
};
