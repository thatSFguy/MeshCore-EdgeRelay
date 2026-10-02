#include "EdgePolicy.h"

EdgePolicy::EdgePolicy() {
  _num_owners = 0;
  _num_channels = 0;
  _mirror_adverts = false;
  _fwd_acks = false;
  _echo_suppress = true;
  _echo_wait = EDGE_ECHO_WAIT_DEFAULT;
  _home_prefix_len = 0;
  _home_enter = EDGE_HOME_ENTER_DEFAULT;
  _home_exit = EDGE_HOME_EXIT_DEFAULT;
  _home_timeout = EDGE_HOME_TIMEOUT_DEFAULT;
  memset(_home_prefix, 0, sizeof(_home_prefix));
  _valid = false;   // no policy loaded yet -> fail closed until load() succeeds
  _rl_start_ms = 0;
  _rl_count = 0;
  memset(_owner_keys, 0, sizeof(_owner_keys));
  memset(_channel_hashes, 0, sizeof(_channel_hashes));
}

bool EdgePolicy::isOwnerHash(uint8_t h) const {
  // The 1-byte wire hash is the first byte of the Ed25519 public key.
  for (int i = 0; i < _num_owners; i++) {
    if (_owner_keys[i][0] == h) return true;
  }
  return false;
}

bool EdgePolicy::isOwnerPubkey(const uint8_t pubkey[PUB_KEY_SIZE]) const {
  for (int i = 0; i < _num_owners; i++) {
    if (memcmp(_owner_keys[i], pubkey, PUB_KEY_SIZE) == 0) return true;
  }
  return false;
}

bool EdgePolicy::isChannel(uint8_t h) const {
  for (int i = 0; i < _num_channels; i++) {
    if (_channel_hashes[i] == h) return true;
  }
  return false;
}

int EdgePolicy::addOwner(const uint8_t pubkey[PUB_KEY_SIZE]) {
  if (isOwnerPubkey(pubkey)) return -1;  // duplicate
  if (_num_owners >= EDGE_MAX_OWNERS) return -1;
  memcpy(_owner_keys[_num_owners], pubkey, PUB_KEY_SIZE);
  return _num_owners++;
}

bool EdgePolicy::removeOwner(const uint8_t pubkey[PUB_KEY_SIZE]) {
  for (int i = 0; i < _num_owners; i++) {
    if (memcmp(_owner_keys[i], pubkey, PUB_KEY_SIZE) == 0) {
      // compact the list
      for (int j = i; j < _num_owners - 1; j++) {
        memcpy(_owner_keys[j], _owner_keys[j + 1], PUB_KEY_SIZE);
      }
      _num_owners--;
      return true;
    }
  }
  return false;
}

int EdgePolicy::addChannel(uint8_t hash1) {
  if (isChannel(hash1)) return -1;
  if (_num_channels >= EDGE_MAX_CHANNELS) return -1;
  _channel_hashes[_num_channels] = hash1;
  return _num_channels++;
}

bool EdgePolicy::removeChannel(uint8_t hash1) {
  for (int i = 0; i < _num_channels; i++) {
    if (_channel_hashes[i] == hash1) {
      for (int j = i; j < _num_channels - 1; j++) {
        _channel_hashes[j] = _channel_hashes[j + 1];
      }
      _num_channels--;
      return true;
    }
  }
  return false;
}

bool EdgePolicy::setEchoWait(int v) {
  if (v < 0 || v > EDGE_ECHO_WAIT_MAX) return false;
  _echo_wait = (uint8_t) v;
  return true;
}

bool EdgePolicy::setHome(const uint8_t* prefix, uint8_t prefix_len, int enter, int exit, int timeout_min) {
  if (prefix_len < 1 || prefix_len > EDGE_HOME_PREFIX_MAX) return false;
  if (enter > 0 || exit < -140 || enter <= exit) return false;
  if (timeout_min < 1 || timeout_min > EDGE_HOME_TIMEOUT_MAX) return false;
  memcpy(_home_prefix, prefix, prefix_len);
  _home_prefix_len = prefix_len;
  _home_enter = (int16_t) enter;
  _home_exit = (int16_t) exit;
  _home_timeout = (uint8_t) timeout_min;
  return true;
}

bool EdgePolicy::isFromHome(const mesh::Packet* pkt) const {
  if (_home_prefix_len == 0) return false;
  uint8_t n = pkt->getPathHashCount();
  if (pkt->isRouteFlood() && n > 0) {
    // The last path entry was appended by whoever just transmitted it.
    uint8_t sz = pkt->getPathHashSize();
    uint8_t cmp = sz < _home_prefix_len ? sz : _home_prefix_len;
    return memcmp(&pkt->path[(n - 1) * sz], _home_prefix, cmp) == 0;
  }
  if (n == 0 && pkt->getPayloadType() == PAYLOAD_TYPE_ADVERT && pkt->payload_len >= PUB_KEY_SIZE) {
    // Zero-hop advert: the sender's full public key is in the payload.
    return memcmp(pkt->payload, _home_prefix, _home_prefix_len) == 0;
  }
  // Direct paths list hops still to go, not the transmitter; TRACE paths hold
  // SNRs. Neither identifies the sender.
  return false;
}

bool EdgePolicy::tryLocalCopy(uint32_t now_ms) {
  if (now_ms - _rl_start_ms >= EDGE_LOCAL_COPY_WINDOW_MS) {
    _rl_start_ms = now_ms;
    _rl_count = 0;
  }
  if (_rl_count >= EDGE_LOCAL_COPY_MAX) return false;
  _rl_count++;
  return true;
}

static bool isFloodRoute(uint8_t route) {
  return route == ROUTE_TYPE_FLOOD || route == ROUTE_TYPE_TRANSPORT_FLOOD;
}

static bool isDirectRoute(uint8_t route) {
  return route == ROUTE_TYPE_DIRECT || route == ROUTE_TYPE_TRANSPORT_DIRECT;
}

EdgeAction EdgePolicy::classify(const mesh::Packet* pkt, const uint8_t* self_hash, uint8_t self_hash_len) const {
  uint8_t route = pkt->getRouteType();
  uint8_t ptype = pkt->getPayloadType();
  uint8_t path_count = pkt->getPathHashCount();

  // Link-local anonymous requests (app login, info queries): heard directly,
  // i.e. an empty path, whether sent DIRECT or as a FLOOD. Apps send a login
  // as a flood when they have no stored path, which is the usual case since
  // this node never advertises. Checked before policy validity so a missing
  // or bad policy file never locks out admin over LoRa. Stock still checks
  // the password and the crypto; checkForward() never forwards these.
  if (ptype == PAYLOAD_TYPE_ANON_REQ) {
    if (path_count != 0) return EDGE_DROP;          // relayed: not link-local
    if (isDirectRoute(route)) return EDGE_STOCK;    // stock checks the dest hash
    // flood: only if addressed to this node, so others' logins are left alone
    return (pkt->payload_len >= 1 && self_hash_len >= 1 && pkt->payload[0] == self_hash[0])
               ? EDGE_STOCK : EDGE_DROP;
  }

  if (!_valid) return EDGE_DROP;  // fail closed: receive-only

  if (isFloodRoute(route)) {
    switch (ptype) {
      case PAYLOAD_TYPE_TXT_MSG:
      case PAYLOAD_TYPE_REQ:
      case PAYLOAD_TYPE_RESPONSE:
      case PAYLOAD_TYPE_PATH: {
        if (pkt->payload_len < 2) return EDGE_DROP;
        uint8_t dest_hash = pkt->payload[0];
        uint8_t src_hash = pkt->payload[1];
        if (path_count == 0) {
          // Directly heard (zero-hop). Only an owner's own transmission may enter
          // the stock flood path; anything else is foreign transit.
          // The empty-path requirement keeps a packet already circulating in the
          // mesh from being mistaken for a local owner transmission.
          return isOwnerHash(src_hash) ? EDGE_STOCK : EDGE_DROP;
        }
        // Already circulating: deliver a local copy if it is addressed to an owner.
        return isOwnerHash(dest_hash) ? EDGE_LOCAL_COPY : EDGE_DROP;
      }
      case PAYLOAD_TYPE_GRP_TXT:
      case PAYLOAD_TYPE_GRP_DATA: {
        if (pkt->payload_len < 1) return EDGE_DROP;
        uint8_t ch = pkt->payload[0];
        if (!isChannel(ch)) return EDGE_DROP;
        // Configured channel: owner opted in. Outbound (zero-hop) group traffic
        // goes through the normal flood path; inbound floods get a local copy.
        return path_count == 0 ? EDGE_STOCK : EDGE_LOCAL_COPY;
      }
      case PAYLOAD_TYPE_ADVERT: {
        if (pkt->payload_len < PUB_KEY_SIZE) return EDGE_DROP;
        // Never export an owner's advert outward: that would publish a route
        // through this moving relay.
        if (isOwnerPubkey(pkt->payload)) return EDGE_DROP;
        // Optional one-way discovery: mirror selected remote adverts locally.
        // Disabled by default.
        if (_mirror_adverts && path_count > 0) return EDGE_LOCAL_COPY;
        return EDGE_DROP;
      }
      case PAYLOAD_TYPE_ACK: {
        // ACKs carry no attributable owner identity; default is to drop.
        return _fwd_acks ? EDGE_STOCK : EDGE_DROP;
      }
      default:
        // TRACE, CONTROL, MULTIPART, RAW_CUSTOM, unknown: drop.
        // (ANON_REQ is handled at the top: link-local only.)
        return EDGE_DROP;
    }
  }

  if (isDirectRoute(route)) {
    if (path_count == 0) {
      // Zero-hop direct: link-local only. Allow control (e.g. discovery)
      // through stock handling (ANON_REQ and post-login admin sessions are
      // handled earlier); a direct exchange cannot propagate or teach the
      // mesh a path through us. Everything else: drop.
      return ptype == PAYLOAD_TYPE_CONTROL ? EDGE_STOCK : EDGE_DROP;
    }
    // This node must be the named next hop, or the packet is not ours to touch.
    uint8_t hash_len = pkt->getPathHashSize();
    if (hash_len > self_hash_len) return EDGE_DROP;
    if (memcmp(pkt->path, self_hash, hash_len) != 0) return EDGE_DROP;

    switch (ptype) {
      case PAYLOAD_TYPE_TXT_MSG:
      case PAYLOAD_TYPE_REQ:
      case PAYLOAD_TYPE_RESPONSE:
      case PAYLOAD_TYPE_PATH: {
        if (pkt->payload_len < 2) return EDGE_DROP;
        // Forward only owner-associated direct traffic; stock consumes our path
        // entry exactly as usual. This keeps a direct path the owner's companion
        // learned while parked working, without making this node general transit.
        uint8_t dest_hash = pkt->payload[0];
        uint8_t src_hash = pkt->payload[1];
        return (isOwnerHash(dest_hash) || isOwnerHash(src_hash)) ? EDGE_STOCK : EDGE_DROP;
      }
      case PAYLOAD_TYPE_GRP_TXT:
      case PAYLOAD_TYPE_GRP_DATA: {
        if (pkt->payload_len < 1) return EDGE_DROP;
        return isChannel(pkt->payload[0]) ? EDGE_STOCK : EDGE_DROP;
      }
      case PAYLOAD_TYPE_ACK: {
        return _fwd_acks ? EDGE_STOCK : EDGE_DROP;
      }
      default:
        // ANON_REQ is only allowed zero-hop (handled above); multi-hop and
        // everything else (TRACE, CONTROL, MULTIPART, RAW_CUSTOM): drop.
        return EDGE_DROP;
    }
  }

  return EDGE_DROP;
}

// --- persistence: simple line-based text format ---
//   ER1
//   owner <64 hex chars of pubkey>
//   chan <2 hex chars of channel hash>
//   mirror_adverts 0|1
//   fwd_acks 0|1
//   echo_suppress 0|1        (optional; default 1)
//   echo_wait <0..20>        (optional; default 4)
//   home <hex prefix> <enter dBm> <exit dBm> <timeout min>   (optional; absent = off)
// Any parse error invalidates the whole policy (fail closed).

static File openPolicyRead(FILESYSTEM* fs) {
#if defined(RP2040_PLATFORM)
  return fs->open(EDGE_POLICY_FILE, "r");
#else
  return fs->open(EDGE_POLICY_FILE);   // ESP32 default is read; NRF52 Adafruit default is FILE_O_READ
#endif
}

static File openPolicyWrite(FILESYSTEM* fs) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  // FILE_O_WRITE appends on Adafruit LittleFS: remove first, as ClientACL,
  // IdentityStore and CommonCLI do, or every save stacks another copy.
  fs->remove(EDGE_POLICY_FILE);
  return fs->open(EDGE_POLICY_FILE, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return fs->open(EDGE_POLICY_FILE, "w");
#else
  return fs->open(EDGE_POLICY_FILE, "w", true);
#endif
}

bool EdgePolicy::parseHome(const char* args) {
  // "<hex prefix> [enter exit timeout]"; numbers default when omitted.
  while (*args == ' ') args++;
  const char* hex = args;
  int hex_len = 0;
  while (hex[hex_len] != 0 && hex[hex_len] != ' ') {
    if (!mesh::Utils::isHexChar(hex[hex_len])) return false;  // fromHex() does not validate
    hex_len++;
  }
  if (hex_len < 2 || hex_len > EDGE_HOME_PREFIX_MAX * 2 || (hex_len & 1)) return false;
  char hex_buf[EDGE_HOME_PREFIX_MAX * 2 + 1];
  memcpy(hex_buf, hex, hex_len);
  hex_buf[hex_len] = 0;
  uint8_t prefix[EDGE_HOME_PREFIX_MAX];
  if (!mesh::Utils::fromHex(prefix, hex_len / 2, hex_buf)) return false;

  long vals[3] = { EDGE_HOME_ENTER_DEFAULT, EDGE_HOME_EXIT_DEFAULT, EDGE_HOME_TIMEOUT_DEFAULT };
  const char* p = hex + hex_len;
  for (int i = 0; i < 3; i++) {
    while (*p == ' ') p++;
    if (*p == 0) {
      if (i == 0) break;  // prefix only: all defaults
      return false;       // partial list: ambiguous, reject
    }
    char* end;
    vals[i] = strtol(p, &end, 10);
    if (end == p) return false;
    p = end;
  }
  while (*p == ' ') p++;
  if (*p != 0) return false;
  return setHome(prefix, hex_len / 2, (int) vals[0], (int) vals[1], (int) vals[2]);
}

void EdgePolicy::resetConfig() {
  _num_owners = 0;
  _num_channels = 0;
  _mirror_adverts = false;
  _fwd_acks = false;
  _echo_suppress = true;
  _echo_wait = EDGE_ECHO_WAIT_DEFAULT;
  _home_prefix_len = 0;
}

bool EdgePolicy::load(FILESYSTEM* fs) {
  _valid = false;
  resetConfig();
  bool ok = parseFile(fs);
  if (!ok) resetConfig();   // fail closed with nothing half-loaded
  _valid = ok;
  return ok;
}

bool EdgePolicy::parseFile(FILESYSTEM* fs) {
  File f = openPolicyRead(fs);
  if (!f) return false;

  char line[112];   // longest directive: "home" + 64 hex + 3 numbers
  int n = 0;
  bool got_magic = false;
  // parse one line at a time, char by char (f.read() is the portable primitive here)
  while (true) {
    int c = f.read();
    if (c < 0 || c == '\n') {
      line[n] = 0;
      // trim trailing CR/whitespace
      while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
      if (n > 0 && line[0] != '#') {
        if (!got_magic) {
          if (strcmp(line, "ER1") != 0) { f.close(); return false; }
          got_magic = true;
        } else if (strcmp(line, "ER1") == 0) {
          // A later header: older firmware appended each save on nRF52
          // instead of replacing the file. Every save wrote a complete copy,
          // so the last block is the newest; start over from here.
          resetConfig();
        } else if (memcmp(line, "owner ", 6) == 0) {
          uint8_t key[PUB_KEY_SIZE];
          if (strlen(line + 6) != PUB_KEY_SIZE * 2 || !mesh::Utils::fromHex(key, PUB_KEY_SIZE, line + 6)) {
            f.close(); return false;
          }
          if (addOwner(key) < 0) { f.close(); return false; }  // duplicate or full
        } else if (memcmp(line, "chan ", 5) == 0) {
          uint8_t h;
          if (strlen(line + 5) != 2 || !mesh::Utils::fromHex(&h, 1, line + 5)) {
            f.close(); return false;
          }
          if (addChannel(h) < 0) { f.close(); return false; }
        } else if (memcmp(line, "mirror_adverts ", 15) == 0) {
          _mirror_adverts = (line[15] == '1');
        } else if (memcmp(line, "fwd_acks ", 9) == 0) {
          _fwd_acks = (line[9] == '1');
        } else if (memcmp(line, "echo_suppress ", 14) == 0) {
          _echo_suppress = (line[14] == '1');
        } else if (memcmp(line, "echo_wait ", 10) == 0) {
          char* end;
          long v = strtol(line + 10, &end, 10);
          if (end == line + 10 || *end != 0 || !setEchoWait((int) v)) { f.close(); return false; }
        } else if (memcmp(line, "home ", 5) == 0) {
          if (!parseHome(line + 5)) { f.close(); return false; }
        } else {
          f.close(); return false;  // unknown directive -> fail closed
        }
      }
      n = 0;
      if (c < 0) break;  // EOF
    } else if (n < (int) sizeof(line) - 1) {
      line[n++] = (char) c;
    }
    // lines longer than the buffer are truncated rather than aborting; the
    // truncated directive will simply fail to match and invalidate the file
  }
  f.close();
  return got_magic;
}

bool EdgePolicy::save(FILESYSTEM* fs) {
  File f = openPolicyWrite(fs);
  if (!f) return false;
  f.println("ER1");
  char hex[PUB_KEY_SIZE * 2 + 1];
  for (int i = 0; i < _num_owners; i++) {
    for (int k = 0; k < PUB_KEY_SIZE; k++) {
      sprintf(&hex[k * 2], "%02x", _owner_keys[i][k]);
    }
    f.print("owner ");
    f.println(hex);
  }
  for (int i = 0; i < _num_channels; i++) {
    sprintf(hex, "%02x", _channel_hashes[i]);
    f.print("chan ");
    f.println(hex);
  }
  f.print("mirror_adverts ");
  f.println(_mirror_adverts ? "1" : "0");
  f.print("fwd_acks ");
  f.println(_fwd_acks ? "1" : "0");
  f.print("echo_suppress ");
  f.println(_echo_suppress ? "1" : "0");
  f.print("echo_wait ");
  f.println((int) _echo_wait);
  if (_home_prefix_len > 0) {
    char prefix_hex[EDGE_HOME_PREFIX_MAX * 2 + 1];
    for (int k = 0; k < _home_prefix_len; k++) {
      sprintf(&prefix_hex[k * 2], "%02x", _home_prefix[k]);
    }
    f.printf("home %s %d %d %d\n", prefix_hex, (int) _home_enter, (int) _home_exit, (int) _home_timeout);
  }
  f.close();
  _valid = true;
  return true;
}
