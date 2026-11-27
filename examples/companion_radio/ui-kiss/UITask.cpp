#include "UITask.h"
#include <helpers/TxtDataHelpers.h>
#include <helpers/AdvertDataHelpers.h>
#include <SPIFFS.h>
#include "../MyMesh.h"
#include "target.h"

#ifndef KISS_AUTO_OFF_MILLIS
  #define KISS_AUTO_OFF_MILLIS  60000   // screen off after a minute without input
#endif

#define KB_POLL_MILLIS         15

// 128x64 OLED, 6x8 font: 21 columns, 8 rows
#define COLS        21
#define ROW_H       8
#define C_DARK      0
#define C_LIGHT     1


static const char* MENU_ITEMS[] = { "Send flood advert", "Send zero-hop advert", "Set name", "Sound", "Vibrate", "Keep msgs", "About" };
#define MENU_COUNT  7

// ---------------------------------------------------------------- setup

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _display = display;
  _sensors = sensors;
  _node_prefs = node_prefs;
  memset(_msgs, 0, sizeof(_msgs));
  memset(_handshakes, 0, sizeof(_handshakes));
  memset(&_out, 0, sizeof(_out));
  memset(_clock, 0, sizeof(_clock));
  _compose[0] = 0;
  _alert[0] = 0;

  // Simple mode: contacts are only added through an accepted DM request.
  if (_node_prefs->manual_add_contacts != 1 || _node_prefs->autoadd_config != 0) {
    _node_prefs->manual_add_contacts = 1;
    _node_prefs->autoadd_config = 0;
    the_mesh.savePrefs();
  }

  user_btn.begin();
  _kb.begin(Wire);
#ifdef PIN_BUZZER
  buzzer.begin();
  buzzer.quiet(_node_prefs->buzzer_quiet);
  buzzer.startup();
#endif
#ifdef PIN_VIBRATION
  pinMode(PIN_VIBRATION, OUTPUT);
  digitalWrite(PIN_VIBRATION, LOW);
#endif

  rebuildChats();
  loadKeepPref();
  if (_keep_msgs) loadMessages();
  if (_display) _display->turnOn();
  _auto_off = millis() + KISS_AUTO_OFF_MILLIS;

  if (isDefaultName()) {
    _screen = KScreen::NAME;   // first boot: pick a name before chatting
    _compose[0] = 0;
    _compose_len = 0;
  }
  if (!_kb.isPresent()) showAlert("No keyboard found", 3000);
}

// The companion's default name is the first 4 bytes of the public key in hex
// (or "NONAME" on older firmware): ask for a real name before chatting.
bool UITask::isDefaultName() const {
  const char* n = _node_prefs->node_name;
  if (n[0] == 0 || strcmp(n, "NONAME") == 0) return true;
  char key_hex[9];
  mesh::Utils::toHex(key_hex, the_mesh.self_id.pub_key, 4);
  return strcasecmp(n, key_hex) == 0;
}

// ---------------------------------------------------------------- saved messages
//
// With "Keep msgs" on (the default), the message ring is written to
// /kiss_msgs at most every 15 s after a change (a power cut loses at most
// that much) and read back at boot. Each message is stored against its
// chat's identity (channel key or contact key), not a list position, so
// channels and contacts can change in between. Plain text in flash, like the
// companion's own files. Turning it off clears all messages.

#define KEEP_PREF_PATH  "/kiss_ui"
#define STORE_PATH      "/kiss_msgs"
#define STORE_MAGIC     0x314D534BUL   // "KSM1"
#define STORE_DELAY_MS  15000

struct KStoredMsgHead {
  uint8_t type;            // KChatType
  uint8_t status;
  uint8_t text_len;
  uint8_t reserved;
  uint8_t key[PUB_KEY_SIZE];   // channel: its 16-byte secret (zero padded); DM: the contact's key
};

void UITask::loadKeepPref() {
  _keep_msgs = true;   // default on; the file only exists once it's been changed
  File f = SPIFFS.open(KEEP_PREF_PATH, "r");
  if (!f) return;
  uint8_t v = 1;
  if (f.read(&v, 1) == 1) _keep_msgs = v != 0;
  f.close();
}

void UITask::saveKeepPref() {
  File f = SPIFFS.open(KEEP_PREF_PATH, "w");
  if (!f) return;
  uint8_t v = _keep_msgs ? 1 : 0;
  f.write(&v, 1);
  f.close();
}

void UITask::messagesChanged() {
  if (!_keep_msgs) return;
  if (!_store_dirty) _store_due = millis() + STORE_DELAY_MS;
  _store_dirty = true;
}

void UITask::saveMessages() {
  _store_dirty = false;
  if (!_keep_msgs) return;
  File f = SPIFFS.open(STORE_PATH, "w");
  if (!f) return;
  uint32_t magic = STORE_MAGIC;
  f.write((const uint8_t*)&magic, sizeof(magic));
  for (int n = KISS_MAX_MSGS; n >= 1; n--) {   // oldest first
    KMsg& m = _msgs[(_msg_head - n + KISS_MAX_MSGS) % KISS_MAX_MSGS];
    if (!m.id || m.chat < 0 || m.chat >= _num_chats) continue;
    KChat& c = _chats[m.chat];
    KStoredMsgHead h;
    memset(&h, 0, sizeof(h));
    h.type = (uint8_t)c.type;
    h.status = m.status;
    size_t len = strlen(m.text);
    h.text_len = len > 255 ? 255 : len;
    if (c.type == KChatType::CHANNEL) {
      ChannelDetails cd;
      if (!the_mesh.getChannel(c.ch_idx, cd)) continue;
      memcpy(h.key, cd.channel.secret, 16);
    } else {
      memcpy(h.key, c.pub_key, PUB_KEY_SIZE);
    }
    f.write((const uint8_t*)&h, sizeof(h));
    f.write((const uint8_t*)m.text, h.text_len);
  }
  f.close();
}

void UITask::loadMessages() {
  File f = SPIFFS.open(STORE_PATH, "r");
  if (!f) return;
  uint32_t magic = 0;
  if (f.read((uint8_t*)&magic, sizeof(magic)) != sizeof(magic) || magic != STORE_MAGIC) {
    f.close();
    return;
  }
  KStoredMsgHead h;
  uint32_t order = 1;   // restored chats keep their relative order, older than anything new
  while (f.read((uint8_t*)&h, sizeof(h)) == sizeof(h)) {
    char text[256];
    if (f.read((uint8_t*)text, h.text_len) != h.text_len) break;
    text[h.text_len] = 0;
    int chat = -1;
    if (h.type == (uint8_t)KChatType::CHANNEL) {
      mesh::GroupChannel probe;
      memset(&probe, 0, sizeof(probe));
      memcpy(probe.secret, h.key, 16);
      int ch = the_mesh.findChannelIdx(probe);
      if (ch >= 0) chat = findChannelChat(ch);
    } else {
      chat = findDirectChat(h.key);
    }
    if (chat < 0) continue;   // that channel or contact is gone
    KMsg* m = &_msgs[_msg_head];
    _msg_head = (_msg_head + 1) % KISS_MAX_MSGS;
    m->id = _next_msg_id++;
    if (_next_msg_id == 0) _next_msg_id = 1;
    m->chat = chat;
    // a DM that was still being sent when we stopped didn't get through
    m->status = h.status == MSG_OUT_PENDING ? MSG_OUT_FAILED : h.status;
    StrHelper::strncpy(m->text, text, sizeof(m->text));
    _chats[chat].last_activity = order++;
  }
  f.close();
  _dirty = true;
}

void UITask::clearMessages() {
  memset(_msgs, 0, sizeof(_msgs));
  _msg_head = 0;
  _scroll = 0;
  memset(&_out, 0, sizeof(_out));
  for (int i = 0; i < _num_chats; i++) {
    _chats[i].unread = 0;
    _chats[i].last_activity = 0;
  }
  SPIFFS.remove(STORE_PATH);
  _store_dirty = false;
  showAlert("Messages cleared", 1500);
}

// ---------------------------------------------------------------- chats + messages

void UITask::rebuildChats() {
  KChat old[KISS_MAX_CHATS];
  int old_n = _num_chats;
  memcpy(old, _chats, sizeof(old));
  _num_chats = 0;

  for (int i = 0; i < MAX_GROUP_CHANNELS && _num_chats < KISS_MAX_CHATS; i++) {
    ChannelDetails cd;
    if (!the_mesh.getChannel(i, cd) || cd.name[0] == 0) continue;
    KChat& c = _chats[_num_chats++];
    memset(&c, 0, sizeof(c));
    c.type = KChatType::CHANNEL;
    c.ch_idx = i;
    StrHelper::strncpy(c.name, cd.name, sizeof(c.name));
  }
  ContactsIterator it = the_mesh.startContactsIterator();
  ContactInfo ci;
  while (_num_chats < KISS_MAX_CHATS && it.hasNext(&the_mesh, ci)) {
    if (ci.type != ADV_TYPE_CHAT) continue;
    KChat& c = _chats[_num_chats++];
    memset(&c, 0, sizeof(c));
    c.type = KChatType::DIRECT;
    memcpy(c.pub_key, ci.id.pub_key, PUB_KEY_SIZE);
    StrHelper::strncpy(c.name, ci.name, sizeof(c.name));
  }

  // carry over unread counts and remap message ownership to the new indexes
  int remap[KISS_MAX_CHATS];
  for (int i = 0; i < old_n; i++) {
    remap[i] = old[i].type == KChatType::CHANNEL ? findChannelChat(old[i].ch_idx) : findDirectChat(old[i].pub_key);
    if (remap[i] >= 0) {
      _chats[remap[i]].unread = old[i].unread;
      _chats[remap[i]].last_activity = old[i].last_activity;
    }
  }
  for (int i = 0; i < KISS_MAX_MSGS; i++) {
    if (_msgs[i].id && _msgs[i].chat >= 0 && _msgs[i].chat < old_n) _msgs[i].chat = remap[_msgs[i].chat];
  }
  if (_chat >= 0 && _chat < old_n) _chat = remap[_chat];
  if (_list_sel >= _num_chats) _list_sel = _num_chats > 0 ? _num_chats - 1 : 0;
  _dirty = true;
}

int UITask::findChannelChat(uint8_t ch_idx) const {
  for (int i = 0; i < _num_chats; i++) {
    if (_chats[i].type == KChatType::CHANNEL && _chats[i].ch_idx == ch_idx) return i;
  }
  return -1;
}

int UITask::findDirectChat(const uint8_t* pub_key) const {
  for (int i = 0; i < _num_chats; i++) {
    if (_chats[i].type == KChatType::DIRECT && memcmp(_chats[i].pub_key, pub_key, PUB_KEY_SIZE) == 0) return i;
  }
  return -1;
}

KMsg* UITask::addMsg(int chat, uint8_t status, const char* text) {
  KMsg* m = &_msgs[_msg_head];
  _msg_head = (_msg_head + 1) % KISS_MAX_MSGS;
  m->id = _next_msg_id++;
  if (_next_msg_id == 0) _next_msg_id = 1;
  m->chat = chat;
  m->status = status;
  StrHelper::strncpy(m->text, text, sizeof(m->text));
  _dirty = true;
  messagesChanged();
  return m;
}

KMsg* UITask::findMsg(uint16_t id) {
  for (int i = 0; i < KISS_MAX_MSGS; i++) {
    if (_msgs[i].id == id) return &_msgs[i];
  }
  return NULL;
}

void UITask::markActivity(int chat, bool incoming) {
  if (chat < 0) return;
  _chats[chat].last_activity = millis();
  bool visible = _screen == KScreen::CHAT && _chat == chat && _display && _display->isOn();
  if (incoming && !visible && _chats[chat].unread < 99) _chats[chat].unread++;
  if (_screen == KScreen::CHAT && _chat == chat) _scroll = 0;
}

// ---------------------------------------------------------------- sending

int UITask::composeLimit() const {
  if (_screen == KScreen::NAME) return sizeof(_node_prefs->node_name) - 1;
  if (_chat >= 0 && _chats[_chat].type == KChatType::CHANNEL) {
    int lim = KISS_TEXT_MAX - (int)strlen(_node_prefs->node_name) - 2;   // "name: " prefix
    return lim < KISS_COMPOSE_MAX ? lim : KISS_COMPOSE_MAX;
  }
  return KISS_COMPOSE_MAX;
}

bool UITask::sendChannelText(uint8_t ch_idx, const char* text) {
  ChannelDetails cd;
  if (!the_mesh.getChannel(ch_idx, cd)) return false;
  uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
  return the_mesh.sendGroupMessage(ts, cd.channel, _node_prefs->node_name, text, strlen(text));
}

void UITask::sendCompose() {
  if (_compose_len == 0 || _chat < 0) return;
  KChat& c = _chats[_chat];
  if (c.type == KChatType::CHANNEL) {
    if (sendChannelText(c.ch_idx, _compose)) {
      addMsg(_chat, MSG_OUT, _compose);
    } else {
      showAlert("Send failed", 1500);
      return;
    }
  } else {
    if (_out.active) {
      showAlert("Still sending...", 1500);
      return;
    }
    ContactInfo* ci = the_mesh.lookupContactByPubKey(c.pub_key, PUB_KEY_SIZE);
    if (ci == NULL) {
      showAlert("Contact missing", 1500);
      return;
    }
    startDM(*ci, _compose);
  }
  markActivity(_chat, false);
  _compose[0] = 0;
  _compose_len = 0;
  _scroll = 0;
}

void UITask::startDM(const ContactInfo& c, const char* text) {
  KMsg* m = addMsg(findDirectChat(c.id.pub_key), MSG_OUT_PENDING, text);
  memset(&_out, 0, sizeof(_out));
  _out.active = true;
  _out.msg_id = m->id;
  memcpy(_out.pub_key, c.id.pub_key, PUB_KEY_SIZE);
  _out.timestamp = the_mesh.getRTCClock()->getCurrentTimeUnique();
  _out.attempt = 0;
  sendDMAttempt();
}

void UITask::sendDMAttempt() {
  KMsg* m = findMsg(_out.msg_id);
  ContactInfo* ci = the_mesh.lookupContactByPubKey(_out.pub_key, PUB_KEY_SIZE);
  if (m == NULL || ci == NULL) {
    _out.active = false;
    return;
  }
  // last attempt: forget the stored route and flood, as the app does
  if (_out.attempt == KISS_DM_ATTEMPTS - 1) the_mesh.resetPathTo(*ci);

  uint32_t ack = 0, est_timeout = 0;
  int rc = the_mesh.sendMessage(*ci, _out.timestamp, _out.attempt, m->text, ack, est_timeout);
  if (rc == MSG_SEND_FAILED) {
    m->status = MSG_OUT_FAILED;
    _out.active = false;
    _dirty = true;
    messagesChanged();
    return;
  }
  _out.acks[_out.attempt] = ack;
  _out.deadline = millis() + est_timeout + 1000;
}

void UITask::checkOutgoing() {
  if (!_out.active || (long)(millis() - _out.deadline) < 0) return;
  if (_out.attempt + 1 < KISS_DM_ATTEMPTS) {
    _out.attempt++;
    sendDMAttempt();
  } else {
    KMsg* m = findMsg(_out.msg_id);
    if (m) m->status = MSG_OUT_FAILED;
    _out.active = false;
    _dirty = true;
    messagesChanged();
  }
}

void UITask::onACKRecv(uint32_t ack_crc) {
  if (!_out.active) return;
  for (int i = 0; i <= _out.attempt && i < KISS_DM_ATTEMPTS; i++) {
    if (_out.acks[i] == ack_crc && ack_crc != 0) {
      KMsg* m = findMsg(_out.msg_id);
      if (m) m->status = MSG_OUT_DELIVERED;
      _out.active = false;
      _dirty = true;
      messagesChanged();
      notify(UIEventType::ack);
      return;
    }
  }
}

// ---------------------------------------------------------------- DM request handshake
//
// Same exchange as the phone app (simple mode), over the group channel. A DM
// only decrypts if the sender is already a contact, and auto-add is off, so
// both sides add each other from the keys carried in these posts:
//   Alice: "DM request for @[Bob] mch:r:<Alice's 64-hex key>"
//   Bob accepts -> adds Alice, then (only if that worked) posts
//          "DM accepted @[Alice] mch:a:<first 8 hex of Alice's key>:<Bob's 64-hex key>"
//   Alice adds Bob only if the 8 hex match her key and she has an outstanding
//   request for that name (so nobody can add themselves by posting "accepted").
// Decline posts nothing. No adverts are sent or needed. Contacts are added with
// timestamp 0 (so their real adverts still update them) and a flood route.

static const char* REQ_PREFIX = "DM request for @[";
static const char* REQ_KEY    = "] mch:r:";
static const char* ACC_PREFIX = "DM accepted @[";
static const char* ACC_KEY    = "] mch:a:";

#define HANDSHAKE_TTL_MILLIS   (7UL * 24 * 3600 * 1000)   // 7 days, as the app

ContactInfo* UITask::findContactByName(const char* name) {
  ContactInfo* found = NULL;
  ContactsIterator it = the_mesh.startContactsIterator();
  ContactInfo ci;
  while (it.hasNext(&the_mesh, ci)) {
    if (ci.type == ADV_TYPE_CHAT && strcasecmp(ci.name, name) == 0) {
      if (found) return NULL;   // ambiguous: more than one person with this name
      found = the_mesh.lookupContactByPubKey(ci.id.pub_key, PUB_KEY_SIZE);
    }
  }
  return found;
}

KHandshake* UITask::findHandshake(const char* name) {
  for (int i = 0; i < KISS_HANDSHAKES; i++) {
    KHandshake& h = _handshakes[i];
    if (h.name[0] && (long)(millis() - h.expires) > 0) h.name[0] = 0;   // expired
    if (h.name[0] && strcasecmp(h.name, name) == 0) return &h;
  }
  return NULL;
}

// Parses "<prefix><name><mid><hex>"; copies name and returns the hex part.
static const char* parseControl(const char* body, const char* prefix, const char* mid, char* name, int name_size) {
  int plen = strlen(prefix);
  if (strncmp(body, prefix, plen) != 0) return NULL;
  const char* n = body + plen;
  const char* m = strstr(n, mid);
  if (m == NULL || m == n || m - n >= name_size) return NULL;
  memcpy(name, n, m - n);
  name[m - n] = 0;
  return m + strlen(mid);
}

static bool readHex(const char* src, uint8_t* dest, int bytes) {
  char buf[PUB_KEY_SIZE * 2 + 1];
  if (bytes * 2 >= (int)sizeof(buf)) return false;
  for (int i = 0; i < bytes * 2; i++) {
    if (!mesh::Utils::isHexChar(src[i])) return false;
    buf[i] = src[i];
  }
  buf[bytes * 2] = 0;
  return mesh::Utils::fromHex(dest, bytes, buf);
}

// Lowercase hex, as the app writes it (MeshCore's toHex() is uppercase), in case
// the app compares the key text exactly.
static void toLowerHex(char* dest, const uint8_t* src, size_t len) {
  mesh::Utils::toHex(dest, src, len);
  for (char* c = dest; *c; c++) *c = tolower(*c);
}

bool UITask::addContactFromKey(const char* name, const uint8_t* pub_key) {
  if (memcmp(pub_key, the_mesh.self_id.pub_key, PUB_KEY_SIZE) == 0) return false;
  if (the_mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE) == NULL) {
    ContactInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.id = mesh::Identity(pub_key);
    StrHelper::strncpy(ci.name, name, sizeof(ci.name));
    ci.type = ADV_TYPE_CHAT;
    ci.out_path_len = OUT_PATH_UNKNOWN;   // first DM floods; route learned from the reply
    ci.last_advert_timestamp = 0;         // their real adverts must still be accepted
    ci.lastmod = the_mesh.getRTCClock()->getCurrentTime();
    if (!the_mesh.addContact(ci)) return false;
    the_mesh.persistContacts();
  }
  rebuildChats();
  return true;
}

void UITask::requestDM(const char* name, uint8_t ch_idx) {
  ContactInfo* existing = findContactByName(name);
  if (existing) {
    int idx = findDirectChat(existing->id.pub_key);
    if (idx >= 0) openChat(idx);
    return;
  }
  char hex[PUB_KEY_SIZE * 2 + 1];
  toLowerHex(hex, the_mesh.self_id.pub_key, PUB_KEY_SIZE);
  char text[KISS_TEXT_MAX + 1];
  snprintf(text, sizeof(text), "%s%s%s%s", REQ_PREFIX, name, REQ_KEY, hex);
  if ((int)(strlen(text) + strlen(_node_prefs->node_name) + 2) > KISS_TEXT_MAX) {
    showAlert("Names too long", 2000);   // the firmware would cut the key off
    return;
  }
  if (!sendChannelText(ch_idx, text)) {
    showAlert("Send failed", 1500);
    return;
  }
  int chat = findChannelChat(ch_idx);
  addMsg(chat, MSG_OUT, text);
  markActivity(chat, false);

  KHandshake* h = findHandshake(name);
  if (h == NULL) {
    h = &_handshakes[0];
    for (int i = 0; i < KISS_HANDSHAKES; i++) {
      if (_handshakes[i].name[0] == 0) { h = &_handshakes[i]; break; }
      if ((long)(_handshakes[i].expires - h->expires) < 0) h = &_handshakes[i];   // replace soonest to expire
    }
  }
  StrHelper::strncpy(h->name, name, sizeof(h->name));
  h->expires = millis() + HANDSHAKE_TTL_MILLIS;
  showAlert("DM request sent", 2000);
}

void UITask::acceptDM() {
  if (!addContactFromKey(_prompt_name, _prompt_key)) {
    showAlert("Couldn't add contact", 2000);
    return;
  }
  char req_hex[9], my_hex[PUB_KEY_SIZE * 2 + 1];
  toLowerHex(req_hex, _prompt_key, 4);
  toLowerHex(my_hex, the_mesh.self_id.pub_key, PUB_KEY_SIZE);
  char text[KISS_TEXT_MAX + 1];
  snprintf(text, sizeof(text), "%s%s%s%s:%s", ACC_PREFIX, _prompt_name, ACC_KEY, req_hex, my_hex);
  if ((int)(strlen(text) + strlen(_node_prefs->node_name) + 2) > KISS_TEXT_MAX || !sendChannelText(_prompt_ch_idx, text)) {
    showAlert("Accept not sent", 2000);
    return;
  }
  int chat = findChannelChat(_prompt_ch_idx);
  addMsg(chat, MSG_OUT, text);
  markActivity(chat, false);
  char msg[48];
  snprintf(msg, sizeof(msg), "DM with %s ready", _prompt_name);
  showAlert(msg, 2500);
}

void UITask::handleChannelControl(const char* sender, const char* body, uint8_t ch_idx) {
  if (strcasecmp(sender, _node_prefs->node_name) == 0) return;   // our own post
  char name[32];
  const char* hex = parseControl(body, REQ_PREFIX, REQ_KEY, name, sizeof(name));
  if (hex) {
    uint8_t key[PUB_KEY_SIZE];
    if (strcasecmp(name, _node_prefs->node_name) != 0 || !readHex(hex, key, PUB_KEY_SIZE)) return;
    if (the_mesh.lookupContactByPubKey(key, PUB_KEY_SIZE)) return;   // already a contact
    memcpy(_prompt_key, key, PUB_KEY_SIZE);
    showPrompt(KPrompt::INCOMING_REQUEST, sender, ch_idx);
    notify(UIEventType::contactMessage);
    return;
  }
  hex = parseControl(body, ACC_PREFIX, ACC_KEY, name, sizeof(name));
  if (hex) {
    uint8_t prefix[4], key[PUB_KEY_SIZE];
    if (!readHex(hex, prefix, 4) || hex[8] != ':' || !readHex(hex + 9, key, PUB_KEY_SIZE)) return;
    if (memcmp(prefix, the_mesh.self_id.pub_key, 4) != 0) return;   // not accepting *our* request
    KHandshake* h = findHandshake(sender);
    if (h == NULL) return;                                          // we didn't ask them
    if (addContactFromKey(sender, key)) {
      h->name[0] = 0;
      char msg[48];
      snprintf(msg, sizeof(msg), "DM with %s ready", sender);
      showAlert(msg, 2500);
      notify(UIEventType::newContactMessage);
    }
  }
}

// ---------------------------------------------------------------- duplicates
//
// Copies of one message, dropped as the phone app drops them. A DM's retries
// are deliberately distinct packets (each carries its attempt number), so the
// mesh doesn't filter them and the firmware hands every copy up; they share
// the sender's key, the sender's timestamp and the text. A channel post can
// also come round again after the mesh's seen-table has forgotten it. The UI
// isn't given a channel post's timestamp, so those match on channel + exact
// "sender: text" within a few minutes.
#define DUP_SLOTS            32
#define DUP_DM_MILLIS        (60UL * 60 * 1000)
#define DUP_CHANNEL_MILLIS   (5UL * 60 * 1000)

struct SeenMsg { uint32_t key; unsigned long at; };
static SeenMsg s_seen[DUP_SLOTS];
static int s_seen_head = 0;

static uint32_t fnv1a(uint32_t h, const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 16777619UL; }
  return h;
}

// true if this key was seen within window_ms; otherwise remembers it
static bool seenRecently(uint32_t key, unsigned long window_ms) {
  unsigned long now = millis();
  for (int i = 0; i < DUP_SLOTS; i++) {
    if (s_seen[i].key == key && s_seen[i].at && now - s_seen[i].at < window_ms) return true;
  }
  s_seen[s_seen_head].key = key;
  s_seen[s_seen_head].at = now | 1;
  s_seen_head = (s_seen_head + 1) % DUP_SLOTS;
  return false;
}

// ---------------------------------------------------------------- mesh events

// No app or GPS to set the time, so take it from the mesh, but never from one
// node alone: a single node with a wrong clock (seen: a day fast) would drag
// ours along, and a wrong clock makes peers drop our adverts (replay guard).
// The clock moves only when two different senders agree within a few minutes;
// then by the smaller of their two corrections. Either direction.
#define CLOCK_TIME_MIN          1772323200UL   // 1 Mar 2026 (ESP32 boot seed)
#define CLOCK_TIME_MAX          4102444800UL   // 2100
#define CLOCK_CLOSE_ENOUGH      120            // seconds; ignore smaller differences
#define CLOCK_AGREE_SECS        300            // two sources must agree this closely
#define CLOCK_SAMPLE_TTL_MILLIS (30UL * 60 * 1000)

static uint32_t sourceHash(const char* name) {   // FNV-1a, case-insensitive
  uint32_t h = 2166136261UL;
  for (; *name; name++) {
    h ^= (uint8_t)tolower(*name);
    h *= 16777619UL;
  }
  return h ? h : 1;
}

void UITask::syncClock(const char* source, uint32_t their_time) {
  if (source == NULL || source[0] == 0) return;
  if (their_time < CLOCK_TIME_MIN || their_time > CLOCK_TIME_MAX) return;
  mesh::RTCClock* rtc = the_mesh.getRTCClock();
  int64_t diff = (int64_t)their_time - (int64_t)rtc->getCurrentTime();
  if (diff > INT32_MAX || diff < INT32_MIN) return;
  int32_t offset = (int32_t)diff;
  uint32_t src = sourceHash(source);
  unsigned long now_ms = millis();

  // one sample per sender: replace theirs, else an empty/expired/oldest slot
  KClockSample* slot = NULL;
  for (int i = 0; i < KISS_CLOCK_SAMPLES; i++) {
    KClockSample& c = _clock[i];
    if (c.source && now_ms - c.at > CLOCK_SAMPLE_TTL_MILLIS) c.source = 0;   // expired
    if (c.source == src) { slot = &c; break; }
  }
  if (slot == NULL) {
    for (int i = 0; i < KISS_CLOCK_SAMPLES; i++) {
      if (_clock[i].source == 0) { slot = &_clock[i]; break; }
      if (slot == NULL || (long)(_clock[i].at - slot->at) < 0) slot = &_clock[i];
    }
  }
  slot->source = src;
  slot->offset = offset;
  slot->at = now_ms;

  if (abs(offset) <= CLOCK_CLOSE_ENOUGH) return;   // we already agree with them

  for (int i = 0; i < KISS_CLOCK_SAMPLES; i++) {
    KClockSample& c = _clock[i];
    if (c.source == 0 || c.source == src) continue;
    if (abs(c.offset - offset) > CLOCK_AGREE_SECS) continue;
    int32_t adjust = abs(c.offset) < abs(offset) ? c.offset : offset;   // the smaller correction
    rtc->setCurrentTime(rtc->getCurrentTime() + adjust);
    memset(_clock, 0, sizeof(_clock));   // offsets were relative to the old clock
    return;
  }
}

void UITask::onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp, const char* text) {
  if (!(txt_type == TXT_TYPE_PLAIN || txt_type == TXT_TYPE_SIGNED_PLAIN)) return;
  uint32_t key = fnv1a(2166136261UL, from.id.pub_key, PUB_KEY_SIZE);
  key = fnv1a(key, &sender_timestamp, sizeof(sender_timestamp));
  key = fnv1a(key, text, strlen(text));
  if (seenRecently(key, DUP_DM_MILLIS)) return;   // a retry of one we already have
  syncClock(from.name, sender_timestamp);
  int chat = findDirectChat(from.id.pub_key);
  if (chat < 0) {
    rebuildChats();
    chat = findDirectChat(from.id.pub_key);
  }
  addMsg(chat, MSG_IN, text);
  markActivity(chat, true);
  char msg[48];
  snprintf(msg, sizeof(msg), "DM from %s", from.name);
  if (!(_screen == KScreen::CHAT && _chat == chat)) showAlert(msg, 2500);
  notify(UIEventType::contactMessage);
}

void UITask::onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails& channel_details, const char* text) {
  int ch_idx = the_mesh.findChannelIdx(channel_details.channel);
  uint32_t key = fnv1a(2166136261UL ^ 0x5A, &ch_idx, sizeof(ch_idx));
  key = fnv1a(key, text, strlen(text));
  if (seenRecently(key, DUP_CHANNEL_MILLIS)) return;   // the same post again
  int chat = ch_idx >= 0 ? findChannelChat(ch_idx) : -1;
  if (chat < 0) {
    rebuildChats();
    chat = ch_idx >= 0 ? findChannelChat(ch_idx) : -1;
  }
  addMsg(chat, MSG_IN, text);
  markActivity(chat, true);

  // "sender: body"
  const char* sep = strstr(text, ": ");
  if (sep && ch_idx >= 0) {
    char sender[32];
    int n = sep - text;
    if (n > 0 && n < (int)sizeof(sender)) {
      memcpy(sender, text, n);
      sender[n] = 0;
      handleChannelControl(sender, sep + 2, ch_idx);
    }
  }
  if (_prompt == KPrompt::NONE && !(_screen == KScreen::CHAT && _chat == chat) && chat >= 0) {
    char msg[48];
    snprintf(msg, sizeof(msg), "New in %s", _chats[chat].name);
    showAlert(msg, 2000);
  }
  notify(UIEventType::channelMessage);
}

void UITask::onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) {
  syncClock(contact.name, contact.last_advert_timestamp);
}

// ---------------------------------------------------------------- notifications

void UITask::notify(UIEventType t) {
#ifdef PIN_BUZZER
  switch (t) {
    case UIEventType::contactMessage:
      buzzer.play("MsgRcv3:d=4,o=6,b=200:32e,32g,32b,16c7");
      break;
    case UIEventType::channelMessage:
      buzzer.play("kerplop:d=16,o=6,b=120:32g#,32c#");
      break;
    case UIEventType::ack:
      buzzer.play("ack:d=32,o=8,b=120:c");
      break;
    case UIEventType::newContactMessage:
      buzzer.play("contact:d=8,o=6,b=180:c,e,g");
      break;
    default:
      break;
  }
#endif
  if (t == UIEventType::channelMessage) vibrate(1);   // a light tick
  if (t == UIEventType::contactMessage || t == UIEventType::newContactMessage) vibrate(2);   // DMs, requests
  if (t != UIEventType::ack && t != UIEventType::none) wake();
}

#define VIB_ON_MILLIS   120
#define VIB_GAP_MILLIS  130

void UITask::vibrate(uint8_t pulses) {
#ifdef PIN_VIBRATION
  if (_node_prefs->vibe_quiet || _vib_pulses || _vib_on) return;   // off, or already buzzing
  _vib_pulses = pulses;
  _vib_next = millis();
#endif
}

void UITask::vibrationLoop() {
#ifdef PIN_VIBRATION
  if (!_vib_on && _vib_pulses == 0) return;
  if ((long)(millis() - _vib_next) < 0) return;
  if (_vib_on) {
    digitalWrite(PIN_VIBRATION, LOW);
    _vib_on = false;
    _vib_next = millis() + VIB_GAP_MILLIS;
  } else {
    digitalWrite(PIN_VIBRATION, HIGH);
    _vib_on = true;
    _vib_pulses--;
    _vib_next = millis() + VIB_ON_MILLIS;
  }
#endif
}

void UITask::wake() {
  if (_display && !_display->isOn()) _display->turnOn();
  _auto_off = millis() + KISS_AUTO_OFF_MILLIS;
  _dirty = true;
}

void UITask::showAlert(const char* text, int millis_dur) {
  StrHelper::strncpy(_alert, text, sizeof(_alert));
  _alert_until = millis() + millis_dur;
  _dirty = true;
}

void UITask::showPrompt(KPrompt p, const char* name, uint8_t ch_idx) {
  if (_screen != KScreen::PROMPT) _prompt_return = _screen;
  _prompt = p;
  StrHelper::strncpy(_prompt_name, name, sizeof(_prompt_name));
  _prompt_ch_idx = ch_idx;
  _screen = KScreen::PROMPT;
  wake();
}

// ---------------------------------------------------------------- input

void UITask::openChat(int idx) {
  if (idx < 0 || idx >= _num_chats) return;
  if (_chat != idx) {
    _compose[0] = 0;
    _compose_len = 0;
  }
  _chat = idx;
  _chats[idx].unread = 0;
  _scroll = 0;
  _screen = KScreen::CHAT;
  _dirty = true;
}

// printable characters, backspace; returns true if the key was used
bool UITask::composeKey(int key) {
  if (key == KEY_KB_BACKSPACE) {
    if (_compose_len > 0) _compose[--_compose_len] = 0;
    return true;
  }
  if (key >= 0x20 && key < 0x7F) {
    if (_compose_len < composeLimit()) {
      _compose[_compose_len++] = (char)key;
      _compose[_compose_len] = 0;
    }
    return true;
  }
  return false;
}

void UITask::keyList(int key) {
  if (key == KEY_KB_UP) {
    if (_list_sel > 0) _list_sel--;
  } else if (key == KEY_KB_DOWN) {
    if (_list_sel + 1 < _num_chats) _list_sel++;
  } else if (key == KEY_KB_ENTER || key == KEY_KB_RIGHT) {
    openChat(_list_sel);
  } else if (key == KEY_KB_ESC || key == KEY_KB_LEFT) {
    _menu_sel = 0;
    _screen = KScreen::MENU;
  } else if (key >= 0x20 && key < 0x7F && _num_chats > 0) {
    openChat(_list_sel);      // start typing straight away
    composeKey(key);
  }
}

void UITask::keyChat(int key) {
  if (composeKey(key)) {
    _scroll = 0;
  } else if (key == KEY_KB_ENTER) {
    sendCompose();
  } else if (key == KEY_KB_UP) {
    _scroll++;
  } else if (key == KEY_KB_DOWN) {
    if (_scroll > 0) _scroll--;
  } else if (key == KEY_KB_ESC || (key == KEY_KB_LEFT && _compose_len == 0)) {
    _screen = KScreen::LIST;
    _list_sel = _chat >= 0 ? _chat : 0;
  } else if (key == KEY_KB_TAB && _chat >= 0 && _chats[_chat].type == KChatType::CHANNEL) {
    // who can we ask for a DM? recent senders in this channel, newest first
    _num_pick = 0;
    for (int n = 1; n <= KISS_MAX_MSGS && _num_pick < 8; n++) {
      KMsg& m = _msgs[(_msg_head - n + KISS_MAX_MSGS) % KISS_MAX_MSGS];
      if (!m.id || m.chat != _chat || m.status != MSG_IN) continue;
      const char* sep = strstr(m.text, ": ");
      if (!sep) continue;
      int len = sep - m.text;
      if (len <= 0 || len >= 32) continue;
      char name[32];
      memcpy(name, m.text, len);
      name[len] = 0;
      if (strcmp(name, _node_prefs->node_name) == 0) continue;
      bool dup = false;
      for (int i = 0; i < _num_pick; i++) dup |= strcmp(_pick_names[i], name) == 0;
      if (!dup) strcpy(_pick_names[_num_pick++], name);
    }
    if (_num_pick == 0) {
      showAlert("No one to DM yet", 1500);
    } else {
      _pick_sel = 0;
      _screen = KScreen::PICK;
    }
  }
}

void UITask::keyPick(int key) {
  if (key == KEY_KB_UP) {
    if (_pick_sel > 0) _pick_sel--;
  } else if (key == KEY_KB_DOWN) {
    if (_pick_sel + 1 < _num_pick) _pick_sel++;
  } else if (key == KEY_KB_ENTER || key == KEY_KB_RIGHT) {
    _screen = KScreen::CHAT;
    showPrompt(KPrompt::CONFIRM_REQUEST, _pick_names[_pick_sel], _chats[_chat].ch_idx);
  } else if (key == KEY_KB_ESC || key == KEY_KB_LEFT) {
    _screen = KScreen::CHAT;
  }
}

void UITask::keyMenu(int key) {
  if (key == KEY_KB_UP) {
    if (_menu_sel > 0) _menu_sel--;
  } else if (key == KEY_KB_DOWN) {
    if (_menu_sel + 1 < MENU_COUNT) _menu_sel++;
  } else if (key == KEY_KB_ESC || key == KEY_KB_LEFT) {
    _screen = KScreen::LIST;
  } else if (key == KEY_KB_ENTER || key == KEY_KB_RIGHT) {
    switch (_menu_sel) {
      case 0:
        showAlert(the_mesh.floodAdvert() ? "Flood advert sent" : "Advert failed", 1500);
        break;
      case 1:
        showAlert(the_mesh.advert() ? "Advert sent" : "Advert failed", 1500);
        break;
      case 2:
        StrHelper::strncpy(_compose, _node_prefs->node_name, sizeof(_compose));
        _compose_len = strlen(_compose);
        _screen = KScreen::NAME;
        break;
      case 3:
        _node_prefs->buzzer_quiet = !_node_prefs->buzzer_quiet;
#ifdef PIN_BUZZER
        buzzer.quiet(_node_prefs->buzzer_quiet);
#endif
        the_mesh.savePrefs();
        showAlert(_node_prefs->buzzer_quiet ? "Sound off" : "Sound on", 1200);
        break;
      case 4:
        _node_prefs->vibe_quiet = !_node_prefs->vibe_quiet;
        the_mesh.savePrefs();
        showAlert(_node_prefs->vibe_quiet ? "Vibrate off" : "Vibrate on", 1200);
        if (!_node_prefs->vibe_quiet) vibrate(1);   // a sample
        break;
      case 5:
        if (_keep_msgs) {
          showPrompt(KPrompt::KEEP_OFF, "", 0);   // turning it off clears everything: ask first
        } else {
          _keep_msgs = true;
          saveKeepPref();
          saveMessages();
          showAlert("Msgs kept on restart", 1500);
        }
        break;
      case 6:
        _screen = KScreen::ABOUT;
        break;
    }
  }
}

void UITask::keyName(int key) {
  if (key == ':' || key == '[' || key == ']' || key == '@') return;   // would break "name: text" and @[name]
  if (composeKey(key)) return;
  if (key == KEY_KB_ENTER && _compose_len > 0) {
    StrHelper::strncpy(_node_prefs->node_name, _compose, sizeof(_node_prefs->node_name));
    the_mesh.savePrefs();
    _compose[0] = 0;
    _compose_len = 0;
    _screen = KScreen::LIST;
    showAlert("Name saved", 1500);
  } else if (key == KEY_KB_ESC && !isDefaultName()) {
    _compose[0] = 0;
    _compose_len = 0;
    _screen = KScreen::MENU;
  }
}

void UITask::keyPrompt(int key) {
  bool yes = key == KEY_KB_ENTER || key == 'y' || key == 'Y';
  bool no = key == KEY_KB_ESC || key == 'n' || key == 'N';
  if (!yes && !no) return;
  KPrompt p = _prompt;
  _prompt = KPrompt::NONE;
  _screen = _prompt_return;
  if (yes && p == KPrompt::CONFIRM_REQUEST) requestDM(_prompt_name, _prompt_ch_idx);
  if (yes && p == KPrompt::INCOMING_REQUEST) acceptDM();   // decline posts nothing, as the app
  if (yes && p == KPrompt::KEEP_OFF) {
    _keep_msgs = false;
    saveKeepPref();
    clearMessages();
  }
}

void UITask::handleKey(int key) {
  switch (_screen) {
    case KScreen::LIST:   keyList(key); break;
    case KScreen::CHAT:   keyChat(key); break;
    case KScreen::PICK:   keyPick(key); break;
    case KScreen::MENU:   keyMenu(key); break;
    case KScreen::NAME:   keyName(key); break;
    case KScreen::ABOUT:  if (key == KEY_KB_ESC || key == KEY_KB_LEFT || key == KEY_KB_ENTER) _screen = KScreen::MENU; break;
    case KScreen::PROMPT: keyPrompt(key); break;
  }
  _dirty = true;
}

// ---------------------------------------------------------------- display

void UITask::drawTitle(const char* left, const char* right) {
  _display->setColor(C_LIGHT);
  _display->fillRect(0, 0, _display->width(), ROW_H);
  _display->setColor(C_DARK);
  int right_w = right ? _display->getTextWidth(right) : 0;
  _display->drawTextEllipsized(1, 0, _display->width() - right_w - 4, left);
  if (right) _display->drawTextRightAlign(_display->width() - 1, 0, right);
  _display->setColor(C_LIGHT);
}

static void drawRow(DisplayDriver* d, int y, const char* text, bool selected) {
  if (selected) {
    d->setColor(C_LIGHT);
    d->fillRect(0, y, d->width(), ROW_H);
    d->setColor(C_DARK);
  } else {
    d->setColor(C_LIGHT);
  }
  d->drawTextEllipsized(1, y, d->width() - 2, text);
  d->setColor(C_LIGHT);
}

void UITask::drawList() {
  char batt[12];
  uint16_t mv = getBattMilliVolts();
  snprintf(batt, sizeof(batt), "%d.%dV", mv / 1000, (mv % 1000) / 100);
  drawTitle(_node_prefs->node_name, batt);

  const int rows = (_display->height() - ROW_H - 1) / ROW_H;   // 6
  if (_list_sel < _list_top) _list_top = _list_sel;
  if (_list_sel >= _list_top + rows) _list_top = _list_sel - rows + 1;
  for (int r = 0; r < rows && _list_top + r < _num_chats; r++) {
    KChat& c = _chats[_list_top + r];
    char line[40];
    if (c.unread) snprintf(line, sizeof(line), "%c%s (%d)", c.type == KChatType::CHANNEL ? '#' : '@', c.name, c.unread);
    else snprintf(line, sizeof(line), "%c%s", c.type == KChatType::CHANNEL ? '#' : '@', c.name);
    drawRow(_display, ROW_H + 1 + r * ROW_H, line, _list_top + r == _list_sel);
  }
  if (_num_chats == 0) _display->drawTextCentered(_display->width() / 2, 28, "No channels");
}

// Word-wraps 'src' to COLS-wide lines; returns the number of lines written.
static int wrapText(const char* src, char out[][COLS + 1], int max_lines) {
  int n = 0;
  int len = strlen(src);
  int pos = 0;
  while (pos < len && n < max_lines) {
    int take = len - pos;
    if (take > COLS) {
      take = COLS;
      for (int i = COLS; i > COLS / 2; i--) {   // break at a space if one is near the end
        if (src[pos + i] == ' ') { take = i; break; }
      }
    }
    memcpy(out[n], src + pos, take);
    out[n][take] = 0;
    n++;
    pos += take;
    while (src[pos] == ' ') pos++;
  }
  return n;
}

void UITask::drawCompose(int y) {
  _display->setColor(C_LIGHT);
  _display->fillRect(0, y - 2, _display->width(), 1);
  char line[COLS + 2];
  const int avail = COLS - 2;   // "> " ... "_"
  const char* tail = _compose_len > avail ? _compose + (_compose_len - avail) : _compose;
  snprintf(line, sizeof(line), ">%s_", tail);
  _display->setCursor(0, y);
  _display->print(line);
}

void UITask::drawChat() {
  if (_chat < 0 || _chat >= _num_chats) {
    _screen = KScreen::LIST;
    return;
  }
  KChat& c = _chats[_chat];
  char title[40];
  snprintf(title, sizeof(title), "%c%s", c.type == KChatType::CHANNEL ? '#' : '@', c.name);
  drawTitle(title, c.type == KChatType::CHANNEL ? "Tab:DM" : NULL);

  // build wrapped lines, newest message last. Each message starts with its
  // sender as an inverted label ("me" for ours) so messages don't run together.
  const int MAX_LINES = 40;
  const int LABEL_MAX = 10;
  static char lines[MAX_LINES][COLS + 1];
  static uint8_t label_len[MAX_LINES];   // leading chars of the line drawn inverted
  int total = 0;
  int count = 0;
  int order[KISS_MAX_MSGS];
  for (int n = KISS_MAX_MSGS; n >= 1; n--) {   // oldest -> newest
    int i = (_msg_head - n + KISS_MAX_MSGS) % KISS_MAX_MSGS;
    if (_msgs[i].id && _msgs[i].chat == _chat) order[count++] = i;
  }
  // only the newest messages that fit
  char buf[KISS_TEXT_MAX + 32];
  char wrapped[12][COLS + 1];
  for (int k = count - 1; k >= 0 && total < MAX_LINES; k--) {
    KMsg& m = _msgs[order[k]];
    const char* mark = "";
    if (m.status == MSG_OUT_PENDING) mark = " ..";
    else if (m.status == MSG_OUT_DELIVERED) mark = " +";
    else if (m.status == MSG_OUT_FAILED) mark = " !";
    char text[KISS_TEXT_MAX + 1];
    _display->translateUTF8ToBlocks(text, m.text, sizeof(text));
    char* mch = strstr(text, "] mch:");
    if (mch) mch[1] = 0;   // "DM request for @[Bob]" without the key

    // sender label + body
    char label[LABEL_MAX + 1];
    const char* body = text;
    if (m.status != MSG_IN) {
      strcpy(label, "me");
    } else if (c.type == KChatType::CHANNEL) {
      const char* sep = strstr(text, ": ");      // "sender: body"
      int len = sep ? sep - text : 0;
      if (len > 0) {
        if (len > LABEL_MAX) len = LABEL_MAX;
        memcpy(label, text, len);
        label[len] = 0;
        body = sep + 2;
      } else {
        label[0] = 0;
      }
    } else {
      StrHelper::strncpy(label, c.name, sizeof(label));   // DM: the other person
    }
    snprintf(buf, sizeof(buf), "%s%s%s%s", label, label[0] ? " " : "", body, mark);
    int n = wrapText(buf, wrapped, 12);
    // prepend (we walk newest -> oldest)
    int shift = n;
    if (total + shift > MAX_LINES) shift = MAX_LINES - total;
    memmove(lines[shift], lines[0], total * (COLS + 1));
    memmove(&label_len[shift], &label_len[0], total);
    for (int j = 0; j < shift; j++) {
      int src = n - shift + j;
      strcpy(lines[j], wrapped[src]);
      int ll = src == 0 ? strlen(label) : 0;     // only the message's first line
      if (ll > (int)strlen(wrapped[src])) ll = strlen(wrapped[src]);
      label_len[j] = ll;
    }
    total += shift;
  }

  const int rows = 5;
  int max_scroll = total > rows ? total - rows : 0;
  if (_scroll > max_scroll) _scroll = max_scroll;
  int first = total - rows - _scroll;
  if (first < 0) first = 0;
  for (int r = 0; r < rows && first + r < total; r++) {
    int y = ROW_H + 1 + r * ROW_H;
    const char* line = lines[first + r];
    int ll = label_len[first + r];
    if (ll > 0) {
      char lab[COLS + 1];
      memcpy(lab, line, ll);
      lab[ll] = 0;
      _display->setColor(C_LIGHT);
      _display->fillRect(0, y, ll * 6 + 1, ROW_H);
      _display->setColor(C_DARK);
      _display->setCursor(1, y);
      _display->print(lab);
      _display->setColor(C_LIGHT);
      _display->setCursor(ll * 6 + 1, y);
      _display->print(line + ll);
    } else {
      _display->setCursor(0, y);
      _display->print(line);
    }
  }
  drawCompose(_display->height() - ROW_H);
}

void UITask::drawPick() {
  drawTitle("Request DM with", NULL);
  for (int r = 0; r < _num_pick && r < 6; r++) {
    drawRow(_display, ROW_H + 1 + r * ROW_H, _pick_names[r], r == _pick_sel);
  }
}

void UITask::drawMenu() {
  drawTitle("Menu", NULL);
  for (int r = 0; r < MENU_COUNT; r++) {
    char line[32];
    if (r == 3) snprintf(line, sizeof(line), "Sound: %s", _node_prefs->buzzer_quiet ? "off" : "on");
    else if (r == 4) snprintf(line, sizeof(line), "Vibrate: %s", _node_prefs->vibe_quiet ? "off" : "on");
    else if (r == 5) snprintf(line, sizeof(line), "Keep msgs: %s", _keep_msgs ? "on" : "off");
    else StrHelper::strncpy(line, MENU_ITEMS[r], sizeof(line));
    drawRow(_display, ROW_H + 1 + r * ROW_H, line, r == _menu_sel);
  }
}

void UITask::drawName() {
  drawTitle("Your name", NULL);
  _display->setCursor(0, ROW_H + 4);
  _display->print("Shown in the chat.");
  _display->setCursor(0, ROW_H * 2 + 4);
  _display->print("Enter to save.");
  drawCompose(_display->height() - ROW_H);
}

void UITask::drawAbout() {
  drawTitle("About", NULL);
  char line[40];
  int y = ROW_H + 1;
  snprintf(line, sizeof(line), "Name: %s", _node_prefs->node_name);
  _display->drawTextEllipsized(0, y, _display->width(), line); y += ROW_H;
  snprintf(line, sizeof(line), "FW: %s", FIRMWARE_VERSION);
  _display->drawTextEllipsized(0, y, _display->width(), line); y += ROW_H;
  snprintf(line, sizeof(line), "Batt: %u mV", getBattMilliVolts());
  _display->drawTextEllipsized(0, y, _display->width(), line); y += ROW_H;
  snprintf(line, sizeof(line), "Contacts: %d", the_mesh.getNumContacts());
  _display->drawTextEllipsized(0, y, _display->width(), line); y += ROW_H;
  snprintf(line, sizeof(line), "Keyboard: %s", _kb.isPresent() ? "ok" : "missing");
  _display->drawTextEllipsized(0, y, _display->width(), line); y += ROW_H;
  uint32_t t = the_mesh.getRTCClock()->getCurrentTime();
  snprintf(line, sizeof(line), "Clock: %02lu:%02lu UTC", (unsigned long)((t / 3600) % 24), (unsigned long)((t / 60) % 60));
  _display->drawTextEllipsized(0, y, _display->width(), line);
}

void UITask::drawPrompt() {
  const char* title = _prompt == KPrompt::INCOMING_REQUEST ? "DM request"
                    : _prompt == KPrompt::KEEP_OFF ? "Stop keeping msgs?" : "Message privately?";
  drawTitle(title, NULL);
  char line[64];
  if (_prompt == KPrompt::KEEP_OFF) {
    strcpy(line, "All messages are cleared. New ones are gone after a restart.");
  } else if (_prompt == KPrompt::INCOMING_REQUEST) {
    char hex[9];
    toLowerHex(hex, _prompt_key, 4);
    snprintf(line, sizeof(line), "%s wants to message you privately. Key %s", _prompt_name, hex);
  }
  else snprintf(line, sizeof(line), "Ask %s for a DM?", _prompt_name);
  char wrapped[5][COLS + 1];
  int n = wrapText(line, wrapped, 5);
  for (int i = 0; i < n; i++) {
    _display->setCursor(0, ROW_H + 2 + i * ROW_H);
    _display->print(wrapped[i]);
  }
  _display->setCursor(0, _display->height() - ROW_H);
  _display->print(_prompt == KPrompt::INCOMING_REQUEST ? "Enter=accept Esc=decl" : "Enter=yes Esc=no");
}

void UITask::render() {
  if (_display == NULL || !_display->isOn()) return;
  _display->startFrame();
  _display->setTextSize(1);
  _display->setColor(C_LIGHT);
  switch (_screen) {
    case KScreen::LIST:   drawList(); break;
    case KScreen::CHAT:   drawChat(); break;
    case KScreen::PICK:   drawPick(); break;
    case KScreen::MENU:   drawMenu(); break;
    case KScreen::NAME:   drawName(); break;
    case KScreen::ABOUT:  drawAbout(); break;
    case KScreen::PROMPT: drawPrompt(); break;
  }
  if (_alert_until && (long)(millis() - _alert_until) < 0) {
    int w = _display->getTextWidth(_alert) + 8;
    if (w > _display->width()) w = _display->width();
    int x = (_display->width() - w) / 2;
    _display->setColor(C_DARK);
    _display->fillRect(x, 24, w, 14);
    _display->setColor(C_LIGHT);
    _display->drawRect(x, 24, w, 14);
    _display->drawTextCentered(_display->width() / 2, 27, _alert);
  }
  _display->endFrame();
}

// ---------------------------------------------------------------- loop

void UITask::loop() {
  unsigned long now = millis();

  if ((long)(now - _next_kb_poll) >= 0) {
    _next_kb_poll = now + KB_POLL_MILLIS;
    int key = _kb.read();
    if (key > 0) {
      bool was_off = _display && !_display->isOn();
      wake();
      if (!was_off) handleKey(key);   // the first key only wakes the screen
    }
  }

  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    if (_display && _display->isOn()) _display->turnOff();
    else wake();
  }

  checkOutgoing();
  if (_store_dirty && (long)(now - _store_due) >= 0) saveMessages();

  // channels/contacts can change over USB (app or the web config page)
  if (_screen == KScreen::LIST && (long)(now - _next_rebuild) >= 0) {
    _next_rebuild = now + 3000;
    rebuildChats();
  }

#ifdef PIN_BUZZER
  if (buzzer.isPlaying()) buzzer.loop();
#endif
  vibrationLoop();

  if (_alert_until && (long)(now - _alert_until) >= 0) {
    _alert_until = 0;
    _dirty = true;
  }

  if (_display && _display->isOn()) {
    if ((long)(now - _auto_off) >= 0) {
      _display->turnOff();
    } else if (_dirty || (long)(now - _next_refresh) >= 0) {
      render();
      _dirty = false;
      _next_refresh = now + 1000;
    }
  }
}
