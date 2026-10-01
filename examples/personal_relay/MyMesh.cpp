#include "MyMesh.h"
#include <algorithm>

/* ------------------------------ Config -------------------------------- */

#ifndef LORA_FREQ
  #define LORA_FREQ 915.0
#endif
#ifndef LORA_BW
  #define LORA_BW 250
#endif
#ifndef LORA_SF
  #define LORA_SF 10
#endif
#ifndef LORA_CR
  #define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER 20
#endif

#ifndef ADVERT_NAME
  #define ADVERT_NAME "repeater"
#endif
#ifndef ADVERT_LAT
  #define ADVERT_LAT 0.0
#endif
#ifndef ADVERT_LON
  #define ADVERT_LON 0.0
#endif

#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY 300
#endif

#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY 200
#endif

#define FIRMWARE_VER_LEVEL       2

#define REQ_TYPE_GET_STATUS         0x01 // same as _GET_STATS
#define REQ_TYPE_KEEP_ALIVE         0x02
#define REQ_TYPE_GET_TELEMETRY_DATA 0x03
#define REQ_TYPE_GET_ACCESS_LIST    0x05
#define REQ_TYPE_GET_NEIGHBOURS     0x06
#define REQ_TYPE_GET_OWNER_INFO     0x07     // FIRMWARE_VER_LEVEL >= 2

#define RESP_SERVER_LOGIN_OK        0 // response to ANON_REQ

#define ANON_REQ_TYPE_REGIONS      0x01
#define ANON_REQ_TYPE_OWNER        0x02
#define ANON_REQ_TYPE_BASIC        0x03   // just remote clock

#define CLI_REPLY_DELAY_MILLIS      600

#define LAZY_CONTACTS_WRITE_DELAY    5000

void MyMesh::putNeighbour(const mesh::Identity &id, uint32_t timestamp, float snr) {
#if MAX_NEIGHBOURS // check if neighbours enabled
  // find existing neighbour, else use least recently updated
  uint32_t oldest_timestamp = 0xFFFFFFFF;
  NeighbourInfo *neighbour = &neighbours[0];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    // if neighbour already known, we should update it
    if (id.matches(neighbours[i].id)) {
      neighbour = &neighbours[i];
      break;
    }

    // otherwise we should update the least recently updated neighbour
    if (neighbours[i].heard_timestamp < oldest_timestamp) {
      neighbour = &neighbours[i];
      oldest_timestamp = neighbour->heard_timestamp;
    }
  }

  // update neighbour info
  neighbour->id = id;
  neighbour->advert_timestamp = timestamp;
  neighbour->heard_timestamp = getRTCClock()->getCurrentTime();
  neighbour->snr = (int8_t)(snr * 4);
#endif
}

uint8_t MyMesh::handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood) {
  ClientInfo* client = NULL;
  if (data[0] == 0) {   // blank password, just check if sender is in ACL
    client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    if (client == NULL) {
    #if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Login, sender not in ACL");
    #endif
    }
  }
  if (client == NULL) {
    uint8_t perms;
    if (strcmp((char *)data, _prefs.password) == 0) { // check for valid admin password
      perms = PERM_ACL_ADMIN;
    } else if (strcmp((char *)data, _prefs.guest_password) == 0) { // check guest password
      perms = PERM_ACL_GUEST;
    } else {
#if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Invalid password: %s", data);
#endif
      return 0;
    }

    client = acl.putClient(sender, 0);  // add to contacts (if not already known)
    if (sender_timestamp <= client->last_timestamp) {
      MESH_DEBUG_PRINTLN("Possible login replay attack!");
      return 0;  // FATAL: client table is full -OR- replay attack
    }

    MESH_DEBUG_PRINTLN("Login success!");
    client->last_timestamp = sender_timestamp;
    client->last_activity = getRTCClock()->getCurrentTime();
    client->permissions &= ~0x03;
    client->permissions |= perms;
    memcpy(client->shared_secret, secret, PUB_KEY_SIZE);

    if (perms != PERM_ACL_GUEST) {   // keep number of FS writes to a minimum
      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
    }
  }

  if (is_flood) {
    client->out_path_len = OUT_PATH_UNKNOWN;  // need to rediscover out_path
  }

  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  reply_data[4] = RESP_SERVER_LOGIN_OK;
  reply_data[5] = 0;  // Legacy: was recommended keep-alive interval (secs / 16)
  reply_data[6] = client->isAdmin() ? 1 : 0;
  reply_data[7] = client->permissions;
  getRNG()->random(&reply_data[8], 4);   // random blob to help packet-hash uniqueness
  reply_data[12] = FIRMWARE_VER_LEVEL;  // New field

  return 13;  // reply length
}

uint8_t MyMesh::handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++;
    if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding

    mesh::Packet::writePath(reply_path, data, reply_path_len);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)

    return 8 + region_map.exportNamesTo((char *) &reply_data[8], sizeof(reply_data) - 12, REGION_DENY_FLOOD);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++;
    if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding

    mesh::Packet::writePath(reply_path, data, reply_path_len);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    sprintf((char *) &reply_data[8], "%s\n%s", _prefs.node_name, _prefs.owner_info);

    return 8 + strlen((char *) &reply_data[8]);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++;
    if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding

    mesh::Packet::writePath(reply_path, data, reply_path_len);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    reply_data[8] = 0;  // features
#ifdef WITH_RS232_BRIDGE
    reply_data[8] |= 0x01;  // is bridge, type UART
#elif WITH_ESPNOW_BRIDGE
    reply_data[8] |= 0x03;  // is bridge, type ESP-NOW
#endif
    if (_prefs.disable_fwd) {   // is this repeater currently disabled
      reply_data[8] |= 0x80;  // is disabled
    }
    // TODO:  add some kind of moving-window utilisation metric, so can query 'how busy' is this repeater
    return 9;   // reply length
  }
  return 0;
}

int MyMesh::handleRequest(ClientInfo *sender, uint32_t sender_timestamp, uint8_t *payload, size_t payload_len) {
  // uint32_t now = getRTCClock()->getCurrentTimeUnique();
  // memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  memcpy(reply_data, &sender_timestamp, 4); // reflect sender_timestamp back in response packet (kind of like a 'tag')

  if (payload[0] == REQ_TYPE_GET_STATUS) {  // guests can also access this now
    RepeaterStats stats;
    stats.batt_milli_volts = board.getBattMilliVolts();
    stats.curr_tx_queue_len = _mgr->getOutboundTotal();
    stats.noise_floor = (int16_t)_radio->getNoiseFloor();
    stats.last_rssi = (int16_t)radio_driver.getLastRSSI();
    stats.n_packets_recv = radio_driver.getPacketsRecv();
    stats.n_packets_sent = radio_driver.getPacketsSent();
    stats.total_air_time_secs = getTotalAirTime() / 1000;
    stats.total_up_time_secs = uptime_millis / 1000;
    stats.n_sent_flood = getNumSentFlood();
    stats.n_sent_direct = getNumSentDirect();
    stats.n_recv_flood = getNumRecvFlood();
    stats.n_recv_direct = getNumRecvDirect();
    stats.err_events = _err_flags;
    stats.last_snr = (int16_t)(radio_driver.getLastSNR() * 4);
    stats.n_direct_dups = ((SimpleMeshTables *)getTables())->getNumDirectDups();
    stats.n_flood_dups = ((SimpleMeshTables *)getTables())->getNumFloodDups();
    stats.total_rx_air_time_secs = getReceiveAirTime() / 1000;
    stats.n_recv_errors = radio_driver.getPacketsRecvErrors();
    memcpy(&reply_data[4], &stats, sizeof(stats));

    return 4 + sizeof(stats); //  reply_len
  }
  if (payload[0] == REQ_TYPE_GET_TELEMETRY_DATA) {
    uint8_t perm_mask = ~(payload[1]); // NEW: first reserved byte (of 4), is now inverse mask to apply to permissions

    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);

    // query other sensors -- target specific
    if ((sender->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
      perm_mask = 0x00;  // just base telemetry allowed
    }
    sensors.querySensors(perm_mask, telemetry);

	// This default temperature will be overridden by external sensors (if any)
    float temperature = board.getMCUTemperature();
    if(!isnan(temperature)) { // Supported boards with built-in temperature sensor. ESP32-C3 may return NAN
      telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature); // Built-in MCU Temperature
    }

    uint8_t tlen = telemetry.getSize();
    memcpy(&reply_data[4], telemetry.getBuffer(), tlen);
    return 4 + tlen; // reply_len
  }
  if (payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()) {
    uint8_t res1 = payload[1];   // reserved for future  (extra query params)
    uint8_t res2 = payload[2];
    if (res1 == 0 && res2 == 0) {
      uint8_t ofs = 4;
      for (int i = 0; i < acl.getNumClients() && ofs + 7 <= sizeof(reply_data) - 4; i++) {
        auto c = acl.getClientByIdx(i);
        if (c->permissions == 0) continue;  // skip deleted entries
        memcpy(&reply_data[ofs], c->id.pub_key, 6); ofs += 6;  // just 6-byte pub_key prefix
        reply_data[ofs++] = c->permissions;
      }
      return ofs;
    }
  }
  if (payload[0] == REQ_TYPE_GET_NEIGHBOURS) {
    uint8_t request_version = payload[1];
    if (request_version == 0) {

      // reply data offset (after response sender_timestamp/tag)
      int reply_offset = 4;

      // get request params
      uint8_t count = payload[2]; // how many neighbours to fetch (0-255)
      uint16_t offset;
      memcpy(&offset, &payload[3], 2); // offset from start of neighbours list (0-65535)
      uint8_t order_by = payload[5]; // how to order neighbours. 0=newest_to_oldest, 1=oldest_to_newest, 2=strongest_to_weakest, 3=weakest_to_strongest
      uint8_t pubkey_prefix_length = payload[6]; // how many bytes of neighbour pub key we want
      // we also send a 4 byte random blob in payload[7...10] to help packet uniqueness

      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS count=%d, offset=%d, order_by=%d, pubkey_prefix_length=%d", count, offset, order_by, pubkey_prefix_length);

      // clamp pub key prefix length to max pub key length
      if(pubkey_prefix_length > PUB_KEY_SIZE){
        pubkey_prefix_length = PUB_KEY_SIZE;
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS invalid pubkey_prefix_length=%d clamping to %d", pubkey_prefix_length, PUB_KEY_SIZE);
      }

      // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
      int16_t neighbours_count = 0;
#if MAX_NEIGHBOURS
      NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
      for (int i = 0; i < MAX_NEIGHBOURS; i++) {
        auto neighbour = &neighbours[i];
        if (neighbour->heard_timestamp > 0) {
          sorted_neighbours[neighbours_count] = neighbour;
          neighbours_count++;
        }
      }

      // sort neighbours based on order
      if (order_by == 0) {
        // sort by newest to oldest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting newest to oldest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp > b->heard_timestamp; // desc
        });
      } else if (order_by == 1) {
        // sort by oldest to newest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting oldest to newest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp < b->heard_timestamp; // asc
        });
      } else if (order_by == 2) {
        // sort by strongest to weakest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting strongest to weakest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr > b->snr; // desc
        });
      } else if (order_by == 3) {
        // sort by weakest to strongest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting weakest to strongest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr < b->snr; // asc
        });
      }
#endif

      // build results buffer
      int results_count = 0;
      int results_offset = 0;
      uint8_t results_buffer[130];
      for(int index = 0; index < count && index + offset < neighbours_count; index++){
        
        // stop if we can't fit another entry in results
        int entry_size = pubkey_prefix_length + 4 + 1;
        if(results_offset + entry_size > sizeof(results_buffer)){
          MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS no more entries can fit in results buffer");
          break;
        }

#if MAX_NEIGHBOURS
        // add next neighbour to results
        auto neighbour = sorted_neighbours[index + offset];
        uint32_t heard_seconds_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
        memcpy(&results_buffer[results_offset], neighbour->id.pub_key, pubkey_prefix_length); results_offset += pubkey_prefix_length;
        memcpy(&results_buffer[results_offset], &heard_seconds_ago, 4); results_offset += 4;
        memcpy(&results_buffer[results_offset], &neighbour->snr, 1); results_offset += 1;
        results_count++;
#endif

      }

      // build reply
      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS neighbours_count=%d results_count=%d", neighbours_count, results_count);
      memcpy(&reply_data[reply_offset], &neighbours_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_buffer, results_offset); reply_offset += results_offset;

      return reply_offset;
    }
  } else if (payload[0] == REQ_TYPE_GET_OWNER_INFO) {
    sprintf((char *) &reply_data[4], "%s\n%s\n%s", EDGE_RELAY_VERSION, _prefs.node_name, _prefs.owner_info);
    return 4 + strlen((char *) &reply_data[4]);
  }
  return 0; // unknown command
}

mesh::Packet *MyMesh::createSelfAdvert() {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len = _cli.buildAdvertData(ADV_TYPE_REPEATER, app_data);

  return createAdvert(self_id, app_data, app_data_len);
}

File MyMesh::openAppend(const char *fname) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return _fs->open(fname, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(fname, "a");
#else
  return _fs->open(fname, "a", true);
#endif
}

static uint8_t max_loop_minimal[] =  { 0, /* 1-byte */  4, /* 2-byte */  2, /* 3-byte */  1 };
static uint8_t max_loop_moderate[] = { 0, /* 1-byte */  2, /* 2-byte */  1, /* 3-byte */  1 };
static uint8_t max_loop_strict[] =   { 0, /* 1-byte */  1, /* 2-byte */  1, /* 3-byte */  1 };

bool MyMesh::isLooped(const mesh::Packet* packet, const uint8_t max_counters[]) {
  uint8_t hash_size = packet->getPathHashSize();
  uint8_t hash_count = packet->getPathHashCount();
  uint8_t n = 0;
  const uint8_t* path = packet->path;
  while (hash_count > 0) {      // count how many times this node is already in the path
    if (self_id.isHashMatch(path, hash_size)) n++;
    hash_count--;
    path += hash_size;
  }
  return n >= max_counters[hash_size];
}

void MyMesh::sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size) {
  TransportKey req_scope;
  bool is_wildcard = recv_pkt_region != NULL && recv_pkt_region->isWildcard();
  bool req_scope_known = recv_pkt_region != NULL && !is_wildcard
                      && region_map.getTransportKeysFor(*recv_pkt_region, &req_scope, 1) > 0;

  switch (mesh::chooseReplyScope(req_scope_known, is_wildcard, !default_scope.isNull())) {
    case mesh::REPLY_SCOPE_REQUEST:
      sendFloodScoped(req_scope, packet, delay_millis, path_hash_size);   // reply with same scope as request
      break;
    case mesh::REPLY_SCOPE_DEFAULT:
      // requester's scope is unknown: DIRECT request (no transport codes), or code matched no Region.
      // un-scoped would be dropped at hop 0 by repeaters running flood.max.unscoped=0
      sendFloodScoped(default_scope, packet, delay_millis, path_hash_size);
      break;
    case mesh::REPLY_SCOPE_NONE:
      sendFlood(packet, delay_millis, path_hash_size);  // send un-scoped
      break;
  }
}

bool MyMesh::allowPacketForward(const mesh::Packet *packet) {
  if (_prefs.disable_fwd) return false;
  if (packet->isRouteFlood()
      && mesh::isFloodHopLimitExceeded(packet, _prefs.flood_max, _prefs.flood_max_unscoped, _prefs.flood_max_advert)) {
    return false;
  }
  if (packet->isRouteFlood() && recv_pkt_region == NULL) {
    MESH_DEBUG_PRINTLN("allowPacketForward: unknown transport code, or wildcard not allowed for FLOOD packet");
    return false;
  }
  if (packet->isRouteFlood() && _prefs.loop_detect != LOOP_DETECT_OFF) {
    const uint8_t* maximums;
    if (_prefs.loop_detect == LOOP_DETECT_MINIMAL) {
      maximums = max_loop_minimal;
    } else if (_prefs.loop_detect == LOOP_DETECT_MODERATE) {
      maximums = max_loop_moderate;
    } else {
      maximums = max_loop_strict;
    }
    if (isLooped(packet, maximums)) {
      MESH_DEBUG_PRINTLN("allowPacketForward: FLOOD packet loop detected!");
      return false;
    }
  }
  // Edge-relay final deny guard: never forward anything the directional policy
  // did not approve, even if it somehow reached stock handling.
  uint8_t self_hash[8];
  uint8_t hash_len = packet->getPathHashSize();
  if (hash_len > sizeof(self_hash)) hash_len = sizeof(self_hash);
  self_id.copyHashTo(self_hash, hash_len);
  if (!edge_policy.checkForward(packet, self_hash, hash_len)) return false;
  // Parked at home: the base station covers the owner, so forward nothing.
  if (isHomeHeld()) {
    edge_stats.n_home_held++;
    return false;
  }
  return true;
}

const char *MyMesh::getLogDateTime() {
  static char tmp[32];
  uint32_t now = getRTCClock()->getCurrentTime();
  DateTime dt = DateTime(now);
  sprintf(tmp, "%02d:%02d:%02d - %d/%d/%d U", dt.hour(), dt.minute(), dt.second(), dt.day(), dt.month(),
          dt.year());
  return tmp;
}

void MyMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
#if MESH_PACKET_LOGGING
  Serial.print(getLogDateTime());
  Serial.print(" RAW: ");
  mesh::Utils::printHex(Serial, raw, len);
  Serial.println();
#endif
}

void MyMesh::logRx(mesh::Packet *pkt, int len, float score) {
  // Called right after reception, so getLastRSSI() belongs to this packet.
  if (edge_policy.isFromHome(pkt)) {
    bool was_home = home_presence.isHome();
    if (home_presence.addSample((int16_t) _radio->getLastRSSI(), millis(),
                                edge_policy.getHomeEnter(), edge_policy.getHomeExit())) {
      MESH_DEBUG_PRINTLN("edge: %s (home avg RSSI %d)", was_home ? "left home, relaying" : "at home, relay paused",
                         (int) home_presence.average());
    }
  }

#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 1) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": RX, len=%d (type=%d, route=%s, payload_len=%d) SNR=%d RSSI=%d score=%d", len,
               pkt->getPayloadType(), pkt->isRouteDirect() ? "D" : "F", pkt->payload_len,
               (int)_radio->getLastSNR(), (int)_radio->getLastRSSI(), (int)(score * 1000));

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTx(mesh::Packet *pkt, int len) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 0) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX, len=%d (type=%d, route=%s, payload_len=%d)", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTxFail(mesh::Packet *pkt, int len) {
  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX FAIL!, len=%d (type=%d, route=%s, payload_len=%d)\n", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);
      f.close();
    }
  }
}

int MyMesh::calcRxDelay(float score, uint32_t air_time) const {
  if (_prefs.rx_delay_base <= 0.0f) return 0;
  return (int)((pow(_prefs.rx_delay_base, 0.85f - score) - 1.0) * air_time);
}

uint32_t MyMesh::getRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}
uint32_t MyMesh::getDirectRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.direct_tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}

mesh::DispatcherAction MyMesh::onRecvPacket(mesh::Packet* pkt) {
  bool is_owner_uplink = false;

  // --- Edge-relay directional policy: classify BEFORE any stock handling. ---
  {
    // Authenticated admin sessions: after an ANON login, the app talks from its
    // session identity, which lives in our ACL (it is NOT an owner pubkey — the
    // login uses an ephemeral key). Packets addressed to us, heard directly
    // (zero-hop), from a known client bypass the edge classifier here; stock
    // still performs the real cryptographic authentication (shared-secret
    // decrypt), so a spoofed 1-byte src prefix cannot get in. This deliberately
    // ignores edge_policy validity: a bad policy file must not lock out the
    // admin recovery path. Flood and multi-hop admin traffic stays dropped.
    uint8_t sess_ptype = pkt->getPayloadType();
    if ((sess_ptype == PAYLOAD_TYPE_REQ || sess_ptype == PAYLOAD_TYPE_TXT_MSG ||
         sess_ptype == PAYLOAD_TYPE_RESPONSE) &&
        pkt->getPathHashCount() == 0 && pkt->payload_len >= 2) {
      uint8_t dest_hash = pkt->payload[0];
      uint8_t src_hash = pkt->payload[1];
      if (self_id.isHashMatch(&dest_hash)) {
        int n = searchPeersByHash(&src_hash);
        for (int k = 0; k < n; k++) {
          if (acl.getClientByIdx(matching_peer_indexes[k])->permissions != 0) {
            return Mesh::onRecvPacket(pkt);  // stock authenticates + handles
          }
        }
      }
    }

    // Echo suppression: a flood that already carries a path is the mesh
    // repeating something. If it matches an uplink forward still waiting in our
    // send queue, another repeater heard the owner without us, so our copy
    // would only add airtime. Cancel it, then let the echo be classified as usual
    // (it is already in the seen-table, so it yields nothing further).
    if (edge_policy.getEchoSuppress() && pkt->isRouteFlood() && pkt->getPathHashCount() > 0
        && cancelEchoedForward(pkt)) {
      edge_stats.n_echo_cancel++;
      MESH_DEBUG_PRINTLN("edge: uplink forward cancelled, echo heard");
    }

    uint8_t self_hash[8];
    uint8_t hash_len = pkt->getPathHashSize();
    if (hash_len > sizeof(self_hash)) hash_len = sizeof(self_hash);
    self_id.copyHashTo(self_hash, hash_len);

    EdgeAction edge_action = edge_policy.classify(pkt, self_hash, hash_len);
    if (edge_action == EDGE_DROP) {
      if (!getTables()->wasSeen(pkt)) {
        getTables()->markSeen(pkt);
        edge_stats.n_dropped++;
      }
      return ACTION_RELEASE;
    }
    if (edge_action == EDGE_LOCAL_COPY) {
      if (!getTables()->wasSeen(pkt)) {
        getTables()->markSeen(pkt);  // the original goes no further, and never yields a second copy
        if (isHomeHeld()) {
          edge_stats.n_home_held++;   // parked at home: the owner hears the base station
        } else if (edge_policy.tryLocalCopy(millis())) {
          mesh::Packet* copy = obtainNewPacket();
          if (copy) {
            // Preserve payload bytes and type exactly; sendZeroHop() reframes the
            // route as DIRECT with an empty path, so neighboring repeaters neither
            // forward it nor learn paths from it. The companion still sees the
            // original sender (this node never re-signs or re-encrypts).
            copy->header = pkt->header;
            copy->transport_codes[0] = copy->transport_codes[1] = 0;
            copy->payload_len = pkt->payload_len;
            memcpy(copy->payload, pkt->payload, pkt->payload_len);
            sendZeroHop(copy, getRetransmitDelay(copy));
            edge_stats.n_local_copy++;
          } else {
            MESH_DEBUG_PRINTLN("edge: packet pool empty, local copy dropped");
            edge_stats.n_dropped++;
          }
        } else {
          MESH_DEBUG_PRINTLN("edge: local-copy rate limit reached");
          edge_stats.n_dropped++;
        }
      }
      return ACTION_RELEASE;
    }
    // EDGE_STOCK: fall through to normal handling below.
    if (pkt->isRouteFlood() && pkt->getPathHashCount() == 0) {
      edge_stats.n_owner_uplink++;
      is_owner_uplink = true;
    } else if (pkt->isRouteDirect() && pkt->getPathHashCount() > 0) {
      edge_stats.n_direct_fwd++;
    }
  }

  if (pkt->getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD) {
    recv_pkt_region = region_map.findMatch(pkt, REGION_DENY_FLOOD);
  } else if (pkt->getRouteType() == ROUTE_TYPE_FLOOD) {
    if (region_map.getWildcard().flags & REGION_DENY_FLOOD) {
      recv_pkt_region = NULL;
    } else {
      recv_pkt_region =  &region_map.getWildcard();
    }
  } else {
    recv_pkt_region = NULL;
  }
  mesh::DispatcherAction action = Mesh::onRecvPacket(pkt);

  // Hold an owner uplink forward a little longer than stock would, so a
  // repeater that heard the owner directly gets to echo it first (see
  // cancelEchoedForward()). No echo in that window -> we forward as normal.
  if (is_owner_uplink && edge_policy.getEchoSuppress() && (action >> 24) != 0) {
    uint32_t d = (action & 0xFFFFFF)
               + _radio->getEstAirtimeFor(pkt->getRawLength()) * edge_policy.getEchoWait();
    if (d > 0xFFFFFF) d = 0xFFFFFF;
    action = (action & 0xFF000000) | d;
  }
  return action;
}

bool MyMesh::cancelEchoedForward(const mesh::Packet* echo) {
  // Only flood packets waiting in our own send queue are candidates: those are
  // forwards (or originations) not yet on air. Zero-hop local copies are DIRECT
  // and are never cancelled -- the owner may not hear the echoing repeater.
  for (int i = 0; i < _mgr->getOutboundTotal(); i++) {
    mesh::Packet* q = _mgr->getOutboundByIdx(i);
    if (q == NULL || !q->isRouteFlood()) continue;
    if (q->getPayloadType() != echo->getPayloadType()) continue;
    if (q->payload_len != echo->payload_len) continue;
    if (memcmp(q->payload, echo->payload, q->payload_len) != 0) continue;
    _mgr->free(_mgr->removeOutboundByIdx(i));
    return true;
  }
  return false;
}

void MyMesh::onAnonDataRecv(mesh::Packet *packet, const uint8_t *secret, const mesh::Identity &sender,
                            uint8_t *data, size_t len) {
  if (packet->getPayloadType() == PAYLOAD_TYPE_ANON_REQ) { // received an initial request by a possible admin
                                                           // client (unknown at this stage)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    data[len] = 0;  // ensure null terminator
    uint8_t reply_len;

    reply_path_len = 0xFF;
    if (data[4] == 0 || data[4] >= ' ') {   // is password, ie. a login request
      reply_len = handleLoginReq(sender, secret, timestamp, &data[4], packet->isRouteFlood());
    } else if (data[4] == ANON_REQ_TYPE_REGIONS && packet->isRouteDirect()) {
      reply_len = handleAnonRegionsReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_OWNER && packet->isRouteDirect()) {
      reply_len = handleAnonOwnerReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_BASIC && packet->isRouteDirect()) {
      reply_len = handleAnonClockReq(sender, timestamp, &data[5]);
    } else {
      reply_len = 0;  // unknown/invalid request type
    }

    if (reply_len == 0) return;   // invalid request

    // a DIRECT login can reply via the stored out_path, as onPeerDataRecv() does for REQ
    ClientInfo* client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    bool have_out_path = client != NULL && client->out_path_len != OUT_PATH_UNKNOWN;

    auto route = mesh::chooseReplyRoute(packet->isRouteFlood(), reply_path_len != 0xFF, have_out_path);

    if (route == mesh::REPLY_ROUTE_PATH_RETURN) {
      // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
      mesh::Packet* path = createPathReturn(sender, secret, packet->path, packet->path_len,
                                            PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
      if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
      return;
    }

    mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply_data, reply_len);
    if (reply == NULL) return;

    if (route == mesh::REPLY_ROUTE_DIRECT_SUPPLIED) {
      sendDirect(reply, reply_path, reply_path_len, SERVER_RESPONSE_DELAY);
    } else if (route == mesh::REPLY_ROUTE_DIRECT_OUT_PATH) {
      sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
    } else {
      sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
    }
  }
}

int MyMesh::searchPeersByHash(const uint8_t *hash) {
  int n = 0;
  for (int i = 0; i < acl.getNumClients(); i++) {
    if (acl.getClientByIdx(i)->id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i; // store the INDEXES of matching contacts (for subsequent 'peer' methods)
    }
  }
  return n;
}

void MyMesh::getPeerSharedSecret(uint8_t *dest_secret, int peer_idx) {
  int i = matching_peer_indexes[peer_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    // lookup pre-calculated shared_secret
    memcpy(dest_secret, acl.getClientByIdx(i)->shared_secret, PUB_KEY_SIZE);
  } else {
    MESH_DEBUG_PRINTLN("getPeerSharedSecret: Invalid peer idx: %d", i);
  }
}

static bool isShare(const mesh::Packet *packet) {
  if (packet->hasTransportCodes()) {
    return packet->transport_codes[0] == 0 && packet->transport_codes[1] == 0;  // codes { 0, 0 } means 'send to nowhere'
  }
  return false;
}

void MyMesh::onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id, uint32_t timestamp,
                          const uint8_t *app_data, size_t app_data_len) {
  mesh::Mesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len); // chain to super impl

  // if this a zero hop advert (and not via 'Share'), add it to neighbours
  if (packet->getPathHashCount() == 0 && !isShare(packet)) {
    AdvertDataParser parser(app_data, app_data_len);
    if (parser.isValid() && parser.getType() == ADV_TYPE_REPEATER) { // just keep neigbouring Repeaters
      putNeighbour(id, timestamp, packet->getSNR());
    }
  }
}

void MyMesh::onPeerDataRecv(mesh::Packet *packet, uint8_t type, int sender_idx, const uint8_t *secret,
                            uint8_t *data, size_t len) {
  int i = matching_peer_indexes[sender_idx];
  if (i < 0 || i >= acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("onPeerDataRecv: invalid peer idx: %d", i);
    return;
  }
  ClientInfo* client = acl.getClientByIdx(i);

  if (type == PAYLOAD_TYPE_REQ) { // request (from a Known admin client!)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    if (timestamp > client->last_timestamp) { // prevent replay attacks
      int reply_len = handleRequest(client, timestamp, &data[4], len - 4);
      if (reply_len == 0) return; // invalid command

      client->last_timestamp = timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      if (packet->isRouteFlood()) {
        // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
        mesh::Packet *path = createPathReturn(client->id, secret, packet->path, packet->path_len,
                                              PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
        if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
      } else {
        mesh::Packet *reply =
            createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret, reply_data, reply_len);
        if (reply) {
          if (client->out_path_len != OUT_PATH_UNKNOWN) { // we have an out_path, so send DIRECT
            sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
          } else {
            sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  } else if (type == PAYLOAD_TYPE_TXT_MSG && len > 5 && client->isAdmin()) { // a CLI command
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4); // timestamp (by sender's RTC clock - which could be wrong)
    uint8_t flags = (data[4] >> 2);        // message attempt number, and other flags

    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA)) {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: unsupported text type received: flags=%02x", (uint32_t)flags);
    } else if (sender_timestamp >= client->last_timestamp) { // prevent replay attacks
      bool is_retry = (sender_timestamp == client->last_timestamp);
      client->last_timestamp = sender_timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      // len can be > original length, but 'text' will be padded with zeroes
      data[len] = 0; // need to make a C string again, with null terminator

      if (flags == TXT_TYPE_PLAIN) { // for legacy CLI, send Acks
        uint32_t ack_hash; // calc truncated hash of the message timestamp + text + sender pub_key, to prove
                           // to sender that we got it
        mesh::Utils::sha256((uint8_t *)&ack_hash, 4, data, 5 + strlen((char *)&data[5]), client->id.pub_key,
                            PUB_KEY_SIZE);

        mesh::Packet *ack = createAck(ack_hash);
        if (ack) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFloodReply(ack, TXT_ACK_DELAY, packet->getPathHashSize());
          } else {
            sendDirect(ack, client->out_path, client->out_path_len, TXT_ACK_DELAY);
          }
        }
      }

      uint8_t temp[166];
      char *command = (char *)&data[5];
      char *reply = (char *)&temp[5];
      if (is_retry) {
        *reply = 0;
      } else {
        handleCommand(sender_timestamp, command, reply);
      }
      int text_len = strlen(reply);
      if (text_len > 0) {
        uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
        if (timestamp == sender_timestamp) {
          // WORKAROUND: the two timestamps need to be different, in the CLI view
          timestamp++;
        }
        memcpy(temp, &timestamp, 4);        // mostly an extra blob to help make packet_hash unique
        temp[4] = (TXT_TYPE_CLI_DATA << 2); // NOTE: legacy was: TXT_TYPE_PLAIN

        auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + text_len);
        if (reply) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFloodReply(reply, CLI_REPLY_DELAY_MILLIS, packet->getPathHashSize());
          } else {
            sendDirect(reply, client->out_path, client->out_path_len, CLI_REPLY_DELAY_MILLIS);
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  }
}

bool MyMesh::onPeerPathRecv(mesh::Packet *packet, int sender_idx, const uint8_t *secret, uint8_t *path,
                            uint8_t path_len, uint8_t extra_type, uint8_t *extra, uint8_t extra_len) {
  // TODO: prevent replay attacks
  int i = matching_peer_indexes[sender_idx];

  if (i >= 0 && i < acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("PATH to client, path_len=%d", (uint32_t)path_len);
    auto client = acl.getClientByIdx(i);

    // store a copy of path, for sendDirect()
    client->out_path_len = mesh::Packet::copyPath(client->out_path, path, path_len);
    client->last_activity = getRTCClock()->getCurrentTime();
  } else {
    MESH_DEBUG_PRINTLN("onPeerPathRecv: invalid peer idx: %d", i);
  }

  // NOTE: no reciprocal path send!!
  return false;
}

#define CTL_TYPE_NODE_DISCOVER_REQ   0x80
#define CTL_TYPE_NODE_DISCOVER_RESP  0x90

void MyMesh::onControlDataRecv(mesh::Packet* packet) {
  uint8_t type = packet->payload[0] & 0xF0;    // just test upper 4 bits
  if (type == CTL_TYPE_NODE_DISCOVER_REQ && packet->payload_len >= 6
      && !_prefs.disable_fwd && discover_limiter.allow(rtc_clock.getCurrentTime())
  ) {
    int i = 1;
    uint8_t  filter = packet->payload[i++];
    uint32_t tag;
    memcpy(&tag, &packet->payload[i], 4); i += 4;
    uint32_t since;
    if (packet->payload_len >= i+4) {   // optional since field
      memcpy(&since, &packet->payload[i], 4); i += 4;
    } else {
      since = 0;
    }

    if ((filter & (1 << ADV_TYPE_REPEATER)) != 0 && _prefs.discovery_mod_timestamp >= since) {
      bool prefix_only = packet->payload[0] & 1;
      uint8_t data[6 + PUB_KEY_SIZE];
      data[0] = CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_REPEATER;   // low 4-bits for node type
      data[1] = packet->_snr;   // let sender know the inbound SNR ( x 4)
      memcpy(&data[2], &tag, 4);     // include tag from request, for client to match to
      memcpy(&data[6], self_id.pub_key, PUB_KEY_SIZE);
      auto resp = createControlData(data, prefix_only ? 6 + 8 : 6 + PUB_KEY_SIZE);
      if (resp) {
        sendZeroHop(resp, getRetransmitDelay(resp)*4);  // apply random delay (widened x4), as multiple nodes can respond to this
      }
    }
  } else if (type == CTL_TYPE_NODE_DISCOVER_RESP && packet->payload_len >= 6) {
    uint8_t node_type = packet->payload[0] & 0x0F;
    if (node_type != ADV_TYPE_REPEATER) {
      return;
    }
    if (packet->payload_len < 6 + PUB_KEY_SIZE) {
      MESH_DEBUG_PRINTLN("onControlDataRecv: DISCOVER_RESP pubkey too short: %d", (uint32_t)packet->payload_len);
      return;
    }

    if (pending_discover_tag == 0 || millisHasNowPassed(pending_discover_until)) {
      pending_discover_tag = 0;
      return;
    }
    uint32_t tag;
    memcpy(&tag, &packet->payload[2], 4);
    if (tag != pending_discover_tag) {
      return;
    }

    mesh::Identity id(&packet->payload[6]);
    if (id.matches(self_id)) {
      return;
    }
    putNeighbour(id, rtc_clock.getCurrentTime(), packet->getSNR());
  }
}

void MyMesh::sendNodeDiscoverReq() {
  uint8_t data[10];
  data[0] = CTL_TYPE_NODE_DISCOVER_REQ; // prefix_only=0
  data[1] = (1 << ADV_TYPE_REPEATER);
  getRNG()->random(&data[2], 4); // tag
  memcpy(&pending_discover_tag, &data[2], 4);
  pending_discover_until = futureMillis(60000);
  uint32_t since = 0;
  memcpy(&data[6], &since, 4);

  auto pkt = createControlData(data, sizeof(data));
  if (pkt) {
    sendZeroHop(pkt);
  }
}

MyMesh::MyMesh(mesh::MainBoard &board, mesh::Radio &radio, mesh::MillisecondClock &ms, mesh::RNG &rng,
               mesh::RTCClock &rtc, mesh::MeshTables &tables)
    : mesh::Mesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      region_map(key_store), temp_map(key_store),
      _cli(board, rtc, sensors, region_map, acl, &_prefs, this),
      telemetry(MAX_PACKET_PAYLOAD - 4),
      discover_limiter(4, 120),  // max 4 every 2 minutes
      anon_limiter(4, 180)   // max 4 every 3 minutes
#if defined(WITH_RS232_BRIDGE)
      , bridge(&_prefs, WITH_RS232_BRIDGE, _mgr, &rtc)
#endif
#if defined(WITH_ESPNOW_BRIDGE)
      , bridge(&_prefs, _mgr, &rtc)
#endif
{
  last_millis = 0;
  uptime_millis = 0;
  next_local_advert = next_flood_advert = 0;
  dirty_contacts_expiry = 0;
  set_radio_at = revert_radio_at = 0;
  _logging = false;
  region_load_active = false;
  recv_pkt_region = NULL;

#if MAX_NEIGHBOURS
  memset(neighbours, 0, sizeof(neighbours));
#endif

  // defaults
  _prefs.airtime_factor = 1.0;
  _prefs.rx_delay_base = 0.0f;   // turn off by default, was 10.0;
  _prefs.tx_delay_factor = 0.5f; // was 0.25f
  _prefs.direct_tx_delay_factor = 0.3f; // was 0.2
  StrHelper::strncpy(_prefs.node_name, ADVERT_NAME, sizeof(_prefs.node_name));
  _prefs.node_lat = ADVERT_LAT;
  _prefs.node_lon = ADVERT_LON;
  StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, sizeof(_prefs.password));
  _prefs.freq = LORA_FREQ;
  _prefs.sf = LORA_SF;
  _prefs.bw = LORA_BW;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.advert_interval = 1;        // default to 2 minutes for NEW installs
  _prefs.flood_advert_interval = 47; // 47 hours
  _prefs.flood_max = 64;
  _prefs.flood_max_unscoped = 64;
  _prefs.flood_max_advert = 8;
  _prefs.interference_threshold = 0; // disabled
  _prefs.cad_enabled = 0;            // hardware CAD before TX (off by default; 'set cad on')

  // bridge defaults
  _prefs.bridge_enabled = 1;    // enabled
  _prefs.bridge_delay   = 500;  // milliseconds
  _prefs.bridge_pkt_src = 0;    // logTx
  _prefs.bridge_baud = 115200;  // baud rate
  _prefs.bridge_channel = 1;    // channel 1

  StrHelper::strncpy(_prefs.bridge_secret, "LVSITANOS", sizeof(_prefs.bridge_secret));

  // GPS defaults
  _prefs.gps_enabled = 0;
  _prefs.gps_interval = 0;
  _prefs.advert_loc_policy = ADVERT_LOC_PREFS;

  _prefs.adc_multiplier = 0.0f; // 0.0f means use default board multiplier

#if defined(USE_SX1262) || defined(USE_SX1268)
#ifdef SX126X_RX_BOOSTED_GAIN
  _prefs.rx_boosted_gain = SX126X_RX_BOOSTED_GAIN;
#else
  _prefs.rx_boosted_gain = 1; // enabled by default;
#endif
#endif
  _prefs.radio_fem_rxgain = 1;
  _prefs.radio_fem_txgain = 0;

  pending_discover_tag = 0;
  pending_discover_until = 0;

  memset(default_scope.key, 0, sizeof(default_scope.key));
}

void MyMesh::begin(FILESYSTEM *fs) {
  mesh::Mesh::begin();
  _fs = fs;
  // load edge-relay policy; an invalid/missing policy fails closed (receive-only)
  memset(&edge_stats, 0, sizeof(edge_stats));
  if (!edge_policy.load(_fs)) {
    MESH_DEBUG_PRINTLN("edge: no valid policy file, running receive-only until configured (see 'edge' CLI)");
  }
  // load persisted prefs
  _cli.loadPrefs(_fs);
  acl.load(_fs, self_id);
  // TODO: key_store.begin();
  region_map.load(_fs);

  // establish default-scope
  {
    RegionEntry* r = region_map.getDefaultRegion();
    if (r) {
      region_map.getTransportKeysFor(*r, &default_scope, 1);
    } else {
#ifdef DEFAULT_FLOOD_SCOPE_NAME
      r = region_map.findByName(DEFAULT_FLOOD_SCOPE_NAME);
      if (r == NULL) {
        r = region_map.putRegion(DEFAULT_FLOOD_SCOPE_NAME, 0);  // auto-create the default scope region
        if (r) { r->flags = 0; }   // Allow-flood
      }
      if (r) {
        region_map.setDefaultRegion(r);
        region_map.getTransportKeysFor(*r, &default_scope, 1);
      }
#endif
    }
  }

#if defined(WITH_BRIDGE)
  if (_prefs.bridge_enabled) {
    bridge.begin();
  }
#endif

  radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_driver.setTxPower(_prefs.tx_power_dbm);

  radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain);
  MESH_DEBUG_PRINTLN("RX Boosted Gain Mode: %s",
                     radio_driver.getRxBoostedGainMode() ? "Enabled" : "Disabled");
  board.setLoRaFemLnaEnabled(_prefs.radio_fem_rxgain);
  board.setLoRaFemPaGainEnabled(_prefs.radio_fem_txgain);

  updateAdvertTimer();
  updateFloodAdvertTimer();

  board.setAdcMultiplier(_prefs.adc_multiplier);

#if ENV_INCLUDE_GPS == 1
  applyGpsPrefs();
#endif
}

void MyMesh::sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size) {
  if (scope.isNull()) {
    sendFlood(pkt, delay_millis, path_hash_size);
  } else {
    uint16_t codes[2];
    codes[0] = scope.calcTransportCode(pkt);
    codes[1] = 0;  // REVISIT: set to 'home' Region, for sender/return region?
    sendFlood(pkt, codes, delay_millis, path_hash_size);
  }
}

void MyMesh::applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) {
  set_radio_at = futureMillis(2000); // give CLI reply some time to be sent back, before applying temp radio params
  pending_freq = freq;
  pending_bw = bw;
  pending_sf = sf;
  pending_cr = cr;

  revert_radio_at = futureMillis(2000 + timeout_mins * 60 * 1000); // schedule when to revert radio params
}

bool MyMesh::formatFileSystem() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return InternalFS.format();
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  return SPIFFS.format();
#else
#error "need to implement file system erase"
  return false;
#endif
}

void MyMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  // Edge relay: NEVER advertise. A moving node must not present itself as
  // infrastructure, or the mesh learns paths through it that go stale.
  (void) delay_millis;
  (void) flood;
  MESH_DEBUG_PRINTLN("edge: self-advert suppressed");
}

void MyMesh::updateAdvertTimer() {
  next_local_advert = 0;  // edge relay never sends adverts; timer stays stopped
}

void MyMesh::updateFloodAdvertTimer() {
  next_flood_advert = 0;  // edge relay never sends adverts; timer stays stopped
}

void MyMesh::dumpLogFile() {
#if defined(RP2040_PLATFORM)
  File f = _fs->open(PACKET_LOG_FILE, "r");
#else
  File f = _fs->open(PACKET_LOG_FILE);
#endif
  if (f) {
    while (f.available()) {
      int c = f.read();
      if (c < 0) break;
      Serial.print((char)c);
    }
    f.close();
  }
}

void MyMesh::setTxPower(int8_t power_dbm) {
  radio_driver.setTxPower(power_dbm);
}

bool MyMesh::setRxBoostedGain(bool enable) {
  return radio_driver.setRxBoostedGainMode(enable);
}

#if defined(USE_LR2021)
bool MyMesh::configSideDetectors(const uint8_t sideDetSFs[], uint8_t num, float bw) {
  return radio_driver.configSideDetectors(sideDetSFs, num, bw);
}
#endif

void MyMesh::formatNeighborsReply(char *reply) {
  char *dp = reply;

#if MAX_NEIGHBOURS
  // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
  int16_t neighbours_count = 0;
  NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    auto neighbour = &neighbours[i];
    if (neighbour->heard_timestamp > 0) {
      sorted_neighbours[neighbours_count] = neighbour;
      neighbours_count++;
    }
  }

  // sort neighbours newest to oldest
  std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
    return a->heard_timestamp > b->heard_timestamp; // desc
  });

  for (int i = 0; i < neighbours_count && dp - reply < 134; i++) {
    NeighbourInfo *neighbour = sorted_neighbours[i];

    // add new line if not first item
    if (i > 0) *dp++ = '\n';

    char hex[10];
    // get 4 bytes of neighbour id as hex
    mesh::Utils::toHex(hex, neighbour->id.pub_key, 4);

    // add next neighbour
    uint32_t secs_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
    sprintf(dp, "%s:%d:%d", hex, secs_ago, neighbour->snr);
    while (*dp)
      dp++; // find end of string
  }
#endif
  if (dp == reply) { // no neighbours, need empty response
    strcpy(dp, "-none-");
    dp += 6;
  }
  *dp = 0; // null terminator
}

void MyMesh::removeNeighbor(const uint8_t *pubkey, int key_len) {
#if MAX_NEIGHBOURS
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    NeighbourInfo *neighbour = &neighbours[i];
    if (memcmp(neighbour->id.pub_key, pubkey, key_len) == 0) {
      neighbours[i] = NeighbourInfo(); // clear neighbour entry
    }
  }
#endif
}

void MyMesh::startRegionsLoad() {
  temp_map.resetFrom(region_map);   // rebuild regions in a temp instance
  memset(load_stack, 0, sizeof(load_stack));
  load_stack[0] = &temp_map.getWildcard();
  region_load_active = true;
}

bool MyMesh::saveRegions() {
  return region_map.save(_fs);
}

void MyMesh::onDefaultRegionChanged(const RegionEntry* r) {
  if (r) {
    region_map.getTransportKeysFor(*r, &default_scope, 1);
  } else {
    memset(default_scope.key, 0, sizeof(default_scope.key));
  }
}

void MyMesh::formatStatsReply(char *reply) {
  StatsFormatHelper::formatCoreStats(reply, board, *_ms, _err_flags, _mgr);
}

void MyMesh::formatRadioStatsReply(char *reply) {
  StatsFormatHelper::formatRadioStats(reply, _radio, radio_driver, getTotalAirTime(), getReceiveAirTime());
}

void MyMesh::formatPacketStatsReply(char *reply) {
  StatsFormatHelper::formatPacketStats(reply, radio_driver, getNumSentFlood(), getNumSentDirect(), 
                                       getNumRecvFlood(), getNumRecvDirect());
}

void MyMesh::saveIdentity(const mesh::LocalIdentity &new_id) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#elif defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#else
#error "need to define saveIdentity()"
#endif
  store.save("_main", new_id);
}

void MyMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables *)getTables())->resetStats();
}

void MyMesh::handleEdgeCommand(char* args, char* reply) {
  while (*args == ' ') args++;

  if (*args == 0 || strcmp(args, "help") == 0) {
    Serial.println("edge status                  - show policy + counters");
    Serial.println("edge owner list              - list owner pubkeys");
    Serial.println("edge owner show <idx>        - show one owner pubkey in reply");
    Serial.println("edge owner add <64 hex>      - add owner, save");
    Serial.println("edge owner del <64 hex>      - remove owner, save");
    Serial.println("edge chan list               - list mirrored channel hashes");
    Serial.println("edge chan add <2 hex>        - add channel, save");
    Serial.println("edge chan del <2 hex>        - remove channel, save");
    Serial.println("edge opt mirror_adverts 0|1  - remote advert mirroring, save");
    Serial.println("edge opt fwd_acks 0|1        - ACK forwarding, save");
    Serial.println("edge echo                    - show echo suppression settings");
    Serial.println("edge echo on|off             - cancel uplink if mesh echoes it first, save");
    Serial.println("edge echo wait <0-20>        - extra uplink hold in airtimes, save");
    Serial.println("edge home                    - show home detection state");
    Serial.println("edge home set <hex> [enter exit timeout_min] - home node pubkey (prefix), save");
    Serial.println("edge home off                - disable home detection, save");
    strcpy(reply, "OK");
    return;
  }

  if (strcmp(args, "status") == 0) {
    Serial.printf("edge: policy %s\n", edge_policy.isValid() ? "OK" : "INVALID (receive-only)");
    Serial.printf("  owners: %d  channels: %d  mirror_adverts: %d  fwd_acks: %d\n",
                  edge_policy.getNumOwners(), edge_policy.getNumChannels(),
                  edge_policy.getMirrorAdverts() ? 1 : 0, edge_policy.getFwdAcks() ? 1 : 0);
    Serial.printf("  echo_suppress: %d  echo_wait: %d\n",
                  edge_policy.getEchoSuppress() ? 1 : 0, (int) edge_policy.getEchoWait());
    Serial.printf("  home: %s\n", !edge_policy.isHomeEnabled() ? "off" : (isHomeHeld() ? "AT HOME (paused)" : "away"));
    Serial.printf("  uplink_fwd: %lu  echo_cancel: %lu  home_held: %lu  local_copy: %lu  direct_fwd: %lu  dropped: %lu\n",
                  (unsigned long) edge_stats.n_owner_uplink, (unsigned long) edge_stats.n_echo_cancel,
                  (unsigned long) edge_stats.n_home_held,
                  (unsigned long) edge_stats.n_local_copy,
                  (unsigned long) edge_stats.n_direct_fwd, (unsigned long) edge_stats.n_dropped);
    // Compact summary in the reply as well: the reply channel always reaches
    // the terminal, Serial block output may not on some setups.
    snprintf(reply, 160, "OK - valid:%d owners:%d ch:%d up:%lu echo:%lu copy:%lu dfwd:%lu drop:%lu",
             edge_policy.isValid() ? 1 : 0,
             edge_policy.getNumOwners(), edge_policy.getNumChannels(),
             (unsigned long) edge_stats.n_owner_uplink, (unsigned long) edge_stats.n_echo_cancel,
             (unsigned long) edge_stats.n_local_copy,
             (unsigned long) edge_stats.n_direct_fwd, (unsigned long) edge_stats.n_dropped);
    return;
  }

  if (strcmp(args, "owner list") == 0) {
    Serial.printf("owners (%d):\n", edge_policy.getNumOwners());
    for (int i = 0; i < edge_policy.getNumOwners(); i++) {
      Serial.print("  ");
      mesh::Utils::printHex(Serial, edge_policy.getOwnerKey(i), PUB_KEY_SIZE);
      Serial.println();
    }
    snprintf(reply, 160, "OK - %d owner(s)", edge_policy.getNumOwners());
    return;
  }

  if (memcmp(args, "owner show ", 11) == 0) {
    int idx = atoi(args + 11);
    if (idx < 0 || idx >= edge_policy.getNumOwners()) {
      strcpy(reply, "Err - bad index");
      return;
    }
    // One full 64-char pubkey fits comfortably in the 160-char reply.
    strcpy(reply, "OK ");
    for (int k = 0; k < PUB_KEY_SIZE; k++) {
      sprintf(reply + 3 + k * 2, "%02x", edge_policy.getOwnerKey(idx)[k]);
    }
    return;
  }

  if (memcmp(args, "owner add ", 10) == 0 || memcmp(args, "owner del ", 10) == 0) {
    bool is_add = (args[6] == 'a');
    char* hex = args + 10;
    while (*hex == ' ') hex++;
    char* end = hex + strlen(hex);
    while (end > hex && *(end - 1) == ' ') *(--end) = 0;
    uint8_t key[PUB_KEY_SIZE];
    if (strlen(hex) != PUB_KEY_SIZE * 2 || !mesh::Utils::fromHex(key, PUB_KEY_SIZE, hex)) {
      snprintf(reply, 160, "Err - bad pubkey (got %d chars, need 64)", (int) strlen(hex));
      return;
    }
    bool ok;
    if (is_add) {
      ok = edge_policy.addOwner(key) >= 0;
    } else {
      ok = edge_policy.removeOwner(key);
    }
    if (ok && edge_policy.save(_fs)) {
      strcpy(reply, is_add ? "OK - owner added" : "OK - owner removed");
    } else {
      strcpy(reply, is_add ? "Err - duplicate, full, or save failed" : "Err - not found or save failed");
    }
    return;
  }

  if (strcmp(args, "chan list") == 0) {
    Serial.printf("channels (%d):\n", edge_policy.getNumChannels());
    for (int i = 0; i < edge_policy.getNumChannels(); i++) {
      Serial.printf("  %02x\n", edge_policy.getChannelHash(i));
    }
    // Full list in the reply as well: the reply channel always reaches
    // the terminal, Serial block output may not on some setups.
    {
      char* p = reply;
      p += snprintf(p, 160, "OK - ch(%d):", edge_policy.getNumChannels());
      for (int i = 0; i < edge_policy.getNumChannels() && (p - reply) < 140; i++) {
        p += snprintf(p, 160 - (p - reply), " %02x", edge_policy.getChannelHash(i));
      }
    }
    return;
  }

  if (memcmp(args, "chan add ", 9) == 0 || memcmp(args, "chan del ", 9) == 0) {
    bool is_add = (args[5] == 'a');
    char* hex = args + 9;
    while (*hex == ' ') hex++;
    char* end = hex + strlen(hex);
    while (end > hex && *(end - 1) == ' ') *(--end) = 0;
    uint8_t h;
    if (strlen(hex) != 2 || !mesh::Utils::fromHex(&h, 1, hex)) {
      strcpy(reply, "Err - bad channel hash (need 2 hex chars)");
      return;
    }
    bool ok = is_add ? (edge_policy.addChannel(h) >= 0) : edge_policy.removeChannel(h);
    if (ok && edge_policy.save(_fs)) {
      strcpy(reply, is_add ? "OK - channel added" : "OK - channel removed");
    } else {
      strcpy(reply, is_add ? "Err - duplicate, full, or save failed" : "Err - not found or save failed");
    }
    return;
  }

  if (memcmp(args, "opt ", 4) == 0) {
    char* rest = args + 4;
    if (memcmp(rest, "mirror_adverts ", 15) == 0) {
      edge_policy.setMirrorAdverts(rest[15] == '1');
      strcpy(reply, edge_policy.save(_fs) ? "OK" : "Err - save failed");
    } else if (memcmp(rest, "fwd_acks ", 9) == 0) {
      edge_policy.setFwdAcks(rest[9] == '1');
      strcpy(reply, edge_policy.save(_fs) ? "OK" : "Err - save failed");
    } else {
      strcpy(reply, "Err - unknown opt (mirror_adverts, fwd_acks)");
    }
    return;
  }

  if (strcmp(args, "echo") == 0) {
    snprintf(reply, 160, "OK - echo:%s wait:%d cancelled:%lu",
             edge_policy.getEchoSuppress() ? "on" : "off", (int) edge_policy.getEchoWait(),
             (unsigned long) edge_stats.n_echo_cancel);
    return;
  }

  if (strcmp(args, "echo on") == 0 || strcmp(args, "echo off") == 0) {
    edge_policy.setEchoSuppress(args[6] == 'n');
    strcpy(reply, edge_policy.save(_fs) ? "OK" : "Err - save failed");
    return;
  }

  if (memcmp(args, "echo wait ", 10) == 0) {
    char* end;
    long v = strtol(args + 10, &end, 10);
    while (*end == ' ') end++;
    if (end == args + 10 || *end != 0 || !edge_policy.setEchoWait((int) v)) {
      strcpy(reply, "Err - echo wait must be 0-20");
      return;
    }
    strcpy(reply, edge_policy.save(_fs) ? "OK" : "Err - save failed");
    return;
  }

  if (strcmp(args, "home") == 0) {
    if (!edge_policy.isHomeEnabled()) {
      strcpy(reply, "OK - home:off");
      return;
    }
    char prefix_hex[9];   // first 4 bytes are plenty to recognise the node
    int show = edge_policy.getHomePrefixLen() < 4 ? edge_policy.getHomePrefixLen() : 4;
    for (int k = 0; k < show; k++) sprintf(&prefix_hex[k * 2], "%02x", edge_policy.getHomePrefix()[k]);
    prefix_hex[show * 2] = 0;
    long age = home_presence.numSamples() > 0 ? (long)((millis() - home_presence.lastSampleMs()) / 1000) : -1;
    snprintf(reply, 160, "OK - %s node:%s enter:%d exit:%d timeout:%dm avg:%d last:%d n:%d age:%lds held:%lu",
             home_presence.isHome() ? "HOME" : "away", prefix_hex,
             (int) edge_policy.getHomeEnter(), (int) edge_policy.getHomeExit(), (int) edge_policy.getHomeTimeout(),
             (int) home_presence.average(), (int) home_presence.lastRssi(), (int) home_presence.numSamples(),
             age, (unsigned long) edge_stats.n_home_held);
    return;
  }

  if (memcmp(args, "home set ", 9) == 0) {
    if (!edge_policy.parseHome(args + 9)) {
      strcpy(reply, "Err - need <hex prefix> [enter exit timeout_min], enter>exit, timeout 1-240");
      return;
    }
    home_presence.reset();
    strcpy(reply, edge_policy.save(_fs) ? "OK - home node set" : "Err - save failed");
    return;
  }

  if (strcmp(args, "home off") == 0) {
    edge_policy.clearHome();
    home_presence.reset();
    strcpy(reply, edge_policy.save(_fs) ? "OK - home detection off" : "Err - save failed");
    return;
  }

  strcpy(reply, "Err - try 'edge help'");
}

void MyMesh::handleCommand(uint32_t sender_timestamp, char *command, char *reply) {
  if (region_load_active) {
    if (StrHelper::isBlank(command)) {  // empty/blank line, signal to terminate 'load' operation
      region_map = temp_map;  // copy over the temp instance as new current map
      region_load_active = false;

      sprintf(reply, "OK - loaded %d regions", region_map.getCount());
    } else {
      char *np = command;
      while (*np == ' ') np++;   // skip indent
      int indent = np - command;

      char *ep = np;
      while (RegionMap::is_name_char(*ep)) ep++;
      if (*ep) { *ep++ = 0; }  // set null terminator for end of name

      while (*ep && *ep != 'F') ep++;  // look for (optional) flags

      if (indent > 0 && indent < 8 && strlen(np) > 0) {
        auto parent = load_stack[indent - 1];
        if (parent) {
          auto old = region_map.findByName(np);
          auto nw = temp_map.putRegion(np, parent->id, old ? old->id : 0);  // carry-over the current ID (if name already exists)
          if (nw) {
            nw->flags = old ? old->flags : (*ep == 'F' ? 0 : REGION_DENY_FLOOD);   // carry-over flags from curr

            load_stack[indent] = nw;  // keep pointers to parent regions, to resolve parent_id's
          }
        }
      }
      reply[0] = 0;
    }
    return;
  }

  while (*command == ' ') command++; // skip leading spaces

  if (strlen(command) > 4 && command[2] == '|') { // optional prefix (for companion radio CLI)
    memcpy(reply, command, 3);                    // reflect the prefix back
    reply += 3;
    command += 3;
  }

  // handle ACL related commands
  if (memcmp(command, "setperm ", 8) == 0) {   // format:  setperm {pubkey-hex} {permissions-int8}
    char* hex = &command[8];
    char* sp = strchr(hex, ' ');   // look for separator char
    if (sp == NULL) {
      strcpy(reply, "Err - bad params");
    } else {
      *sp++ = 0;   // replace space with null terminator

      uint8_t pubkey[PUB_KEY_SIZE];
      int hex_len = min(sp - hex, PUB_KEY_SIZE*2);
      if (mesh::Utils::fromHex(pubkey, hex_len / 2, hex)) {
        uint8_t perms = atoi(sp);
        if (acl.applyPermissions(self_id, pubkey, hex_len / 2, perms)) {
          dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);   // trigger acl.save()
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Err - invalid params");
        }
      } else {
        strcpy(reply, "Err - bad pubkey");
      }
    }
  } else if (sender_timestamp == 0 && strcmp(command, "get acl") == 0) {
    Serial.println("ACL:");
    for (int i = 0; i < acl.getNumClients(); i++) {
      auto c = acl.getClientByIdx(i);
      if (c->permissions == 0) continue;  // skip deleted (or guest) entries

      Serial.printf("%02X ", c->permissions);
      mesh::Utils::printHex(Serial, c->id.pub_key, PUB_KEY_SIZE);
      Serial.printf("\n");
    }
    reply[0] = 0;
  } else if (memcmp(command, "discover.neighbors", 18) == 0) {
    const char* sub = command + 18;
    while (*sub == ' ') sub++;
    if (*sub != 0) {
      strcpy(reply, "Err - discover.neighbors has no options");
    } else {
      sendNodeDiscoverReq();
      strcpy(reply, "OK - Discover sent");
    }
  } else if (memcmp(command, "edge ", 5) == 0 || strcmp(command, "edge") == 0) {
    handleEdgeCommand(command + (command[4] == ' ' ? 5 : 4), reply);
  } else{
    _cli.handleCommand(sender_timestamp, command, reply);  // common CLI commands
  }
}

void MyMesh::loop() {
#ifdef WITH_BRIDGE
  bridge.loop();
#endif

  mesh::Mesh::loop();

  // NOTE: edge relay never sends self adverts (see sendSelfAdvertisement()).

  if (set_radio_at && millisHasNowPassed(set_radio_at)) { // apply pending (temporary) radio params
    set_radio_at = 0;                                     // clear timer
    radio_driver.setParams(pending_freq, pending_bw, pending_sf, pending_cr);
    MESH_DEBUG_PRINTLN("Temp radio params");
  }

  if (revert_radio_at && millisHasNowPassed(revert_radio_at)) { // revert radio params to orig
    revert_radio_at = 0;                                        // clear timer
    radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
    MESH_DEBUG_PRINTLN("Radio params restored");
  }

  // is pending dirty contacts write needed?
  if (dirty_contacts_expiry && millisHasNowPassed(dirty_contacts_expiry)) {
    acl.save(_fs);
    dirty_contacts_expiry = 0;
  }

  if (edge_policy.isHomeEnabled()
      && home_presence.tick(millis(), (uint32_t) edge_policy.getHomeTimeout() * 60000UL)) {
    MESH_DEBUG_PRINTLN("edge: home node not heard strongly, relaying");
  }

  // update uptime
  uint32_t now = millis();
  uptime_millis += now - last_millis;
  last_millis = now;
}

// To check if there is pending work
bool MyMesh::hasPendingWork() const {
#if defined(WITH_BRIDGE)
  if (bridge.isRunning()) return true;  // bridge needs WiFi radio, can't sleep
#endif
  return _mgr->getOutboundTotal() > 0;
}
