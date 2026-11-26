#include "UITask.h"
#include <helpers/TxtDataHelpers.h>
#include <helpers/AdvertDataHelpers.h>
#include "../MyMesh.h"
#include "target.h"

#ifndef KISS_AUTO_OFF_MILLIS
  #define KISS_AUTO_OFF_MILLIS  60000   // screen off after a minute without input
#endif

#define HANDSHAKE_TTL_MILLIS   (60UL * 60 * 1000)   // app users accept by hand, give them time
#define KB_POLL_MILLIS         15

// 128x64 OLED, 6x8 font: 21 columns, 8 rows
#define COLS        21
#define ROW_H       8
#define C_DARK      0
#define C_LIGHT     1

static const char* DM_REQUEST_TAG  = "] DM request";
static const char* DM_ACCEPTED_TAG = "] DM accepted";

static const char* MENU_ITEMS[] = { "Send flood advert", "Send zero-hop advert", "Set name", "Sound", "About" };
#define MENU_COUNT  5

// ---------------------------------------------------------------- setup

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _display = display;
  _sensors = sensors;
  _node_prefs = node_prefs;
  memset(_msgs, 0, sizeof(_msgs));
  memset(_adverts, 0, sizeof(_adverts));
  memset(_handshakes, 0, sizeof(_handshakes));
  memset(&_out, 0, sizeof(_out));
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
  vibration.begin();
#endif

  rebuildChats();
  if (_display) _display->turnOn();
  _auto_off = millis() + KISS_AUTO_OFF_MILLIS;

  if (strcmp(_node_prefs->node_name, "NONAME") == 0 || _node_prefs->node_name[0] == 0) {
    _screen = KScreen::NAME;   // first boot: pick a name before chatting
    _compose[0] = 0;
    _compose_len = 0;
  }
  if (!_kb.isPresent()) showAlert("No keyboard found", 3000);
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
      notify(UIEventType::ack);
      return;
    }
  }
}

// ---------------------------------------------------------------- DM request handshake
//
// In the group chat, A asks B for a DM:
//   A: flood advert, then posts "@[B] DM request"
//   B: accepts -> adds A (from A's advert), flood advert, posts "@[A] DM accepted"
//   A: adds B as soon as B's advert is known (B's accept floods one)
// The requester doesn't wait for the "accepted" text: adding someone you asked
// to DM only lets you message them; they still choose by adding you. That way a
// phone-app user (who has no accept button) accepts by adding the requester
// from their discovered list and sending a flood advert.
// The texts stay readable for people using the phone app.

ContactInfo* UITask::findContactByName(const char* name) {
  ContactsIterator it = the_mesh.startContactsIterator();
  ContactInfo ci;
  while (it.hasNext(&the_mesh, ci)) {
    if (strcmp(ci.name, name) == 0) return the_mesh.lookupContactByPubKey(ci.id.pub_key, PUB_KEY_SIZE);
  }
  return NULL;
}

KAdvert* UITask::findAdvertByName(const char* name) {
  KAdvert* best = NULL;
  for (int i = 0; i < KISS_ADVERT_CACHE; i++) {
    KAdvert& a = _adverts[i];
    if (a.heard_at && strcmp(a.contact.name, name) == 0) {
      if (best == NULL || (long)(a.heard_at - best->heard_at) > 0) best = &a;   // newest wins
    }
  }
  return best;
}

KHandshake* UITask::findHandshake(const char* name, bool create) {
  KHandshake* free_slot = NULL;
  for (int i = 0; i < KISS_HANDSHAKES; i++) {
    KHandshake& h = _handshakes[i];
    if (h.state != HS_NONE && (long)(millis() - h.expires) > 0) h.state = HS_NONE;   // expired
    if (h.state != HS_NONE && strcmp(h.name, name) == 0) return &h;
    if (h.state == HS_NONE && free_slot == NULL) free_slot = &h;
  }
  if (!create) return NULL;
  if (free_slot == NULL) free_slot = &_handshakes[0];
  memset(free_slot, 0, sizeof(*free_slot));
  StrHelper::strncpy(free_slot->name, name, sizeof(free_slot->name));
  free_slot->expires = millis() + HANDSHAKE_TTL_MILLIS;
  return free_slot;
}

void UITask::requestDM(const char* name, uint8_t ch_idx) {
  ContactInfo* existing = findContactByName(name);
  if (existing) {
    int idx = findDirectChat(existing->id.pub_key);
    if (idx >= 0) openChat(idx);
    return;
  }
  char text[64];
  snprintf(text, sizeof(text), "@[%s%s", name, DM_REQUEST_TAG);
  the_mesh.floodAdvert();                  // so they can add us when they accept
  if (!sendChannelText(ch_idx, text)) {
    showAlert("Send failed", 1500);
    return;
  }
  int chat = findChannelChat(ch_idx);
  addMsg(chat, MSG_OUT, text);
  markActivity(chat, false);
  KHandshake* h = findHandshake(name, true);
  h->state = HS_WAIT_ACCEPT;
  h->expires = millis() + HANDSHAKE_TTL_MILLIS;
  if (!tryCompleteDM(name)) showAlert("DM request sent", 2000);   // their advert may be cached already
}

void UITask::acceptDM(const char* name, uint8_t ch_idx) {
  char text[64];
  snprintf(text, sizeof(text), "@[%s%s", name, DM_ACCEPTED_TAG);
  the_mesh.floodAdvert();                  // so they can add us
  if (sendChannelText(ch_idx, text)) {
    int chat = findChannelChat(ch_idx);
    addMsg(chat, MSG_OUT, text);
    markActivity(chat, false);
  }
  KHandshake* h = findHandshake(name, true);
  h->state = HS_ACCEPTED;
  h->expires = millis() + HANDSHAKE_TTL_MILLIS;
  if (!tryCompleteDM(name)) showAlert("Waiting for advert", 2000);
}

// Adds the contact once the request was accepted and their advert is known.
bool UITask::tryCompleteDM(const char* name) {
  KHandshake* h = findHandshake(name, false);
  if (h == NULL || h->state == HS_NONE) return false;   // requested by us, or accepted by us

  ContactInfo* existing = findContactByName(name);
  if (existing == NULL) {
    KAdvert* a = findAdvertByName(name);
    if (a == NULL) return false;           // completes when their advert arrives
    ContactInfo ci = a->contact;
    ci.out_path_len = OUT_PATH_UNKNOWN;    // first message floods; the path is learned from the reply
    if (!the_mesh.addContact(ci)) {
      showAlert("Contacts full", 2000);
      return false;
    }
    the_mesh.persistContacts();
    rebuildChats();
  }
  h->state = HS_NONE;
  char msg[48];
  snprintf(msg, sizeof(msg), "DM with %s ready", name);
  showAlert(msg, 2500);
  notify(UIEventType::newContactMessage);
  return true;
}

void UITask::handleChannelControl(const char* sender, const char* body, uint8_t ch_idx) {
  char prefix[40];
  snprintf(prefix, sizeof(prefix), "@[%s", _node_prefs->node_name);
  int plen = strlen(prefix);
  if (strncmp(body, prefix, plen) != 0) return;
  const char* rest = body + plen;

  if (strncmp(rest, DM_REQUEST_TAG, strlen(DM_REQUEST_TAG)) == 0) {
    if (findContactByName(sender)) return;   // already set up
    showPrompt(KPrompt::INCOMING_REQUEST, sender, ch_idx);
    notify(UIEventType::contactMessage);
  } else if (strncmp(rest, DM_ACCEPTED_TAG, strlen(DM_ACCEPTED_TAG)) == 0) {
    KHandshake* h = findHandshake(sender, false);
    if (h == NULL || h->state != HS_WAIT_ACCEPT) return;   // we didn't ask
    h->state = HS_ACCEPTED;
    if (!tryCompleteDM(sender)) showAlert("Accepted, waiting advert", 2000);
  }
}

// ---------------------------------------------------------------- mesh events

void UITask::syncClock(uint32_t their_time) {
  // No app to set the time: move our clock forward to the newest plausible time
  // heard on the mesh. Never backwards; ignore nonsense far in the future.
  uint32_t now = the_mesh.getRTCClock()->getCurrentTime();
  if (their_time > now + 120 && their_time < 4102444800UL) {
    the_mesh.getRTCClock()->setCurrentTime(their_time);
  }
}

void UITask::onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp, const char* text) {
  if (!(txt_type == TXT_TYPE_PLAIN || txt_type == TXT_TYPE_SIGNED_PLAIN)) return;
  syncClock(sender_timestamp);
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
  syncClock(contact.last_advert_timestamp);
  if (contact.type != ADV_TYPE_CHAT) return;

  // cache it (replace same key, else oldest)
  KAdvert* slot = NULL;
  for (int i = 0; i < KISS_ADVERT_CACHE; i++) {
    if (_adverts[i].heard_at && memcmp(_adverts[i].contact.id.pub_key, contact.id.pub_key, PUB_KEY_SIZE) == 0) {
      slot = &_adverts[i];
      break;
    }
  }
  if (slot == NULL) {
    slot = &_adverts[0];
    for (int i = 0; i < KISS_ADVERT_CACHE; i++) {
      if (_adverts[i].heard_at == 0) { slot = &_adverts[i]; break; }
      if ((long)(_adverts[i].heard_at - slot->heard_at) < 0) slot = &_adverts[i];
    }
  }
  slot->contact = contact;
  slot->heard_at = millis() | 1;   // never 0

  tryCompleteDM(contact.name);   // an accepted request waiting on this advert
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
#ifdef PIN_VIBRATION
  if (t == UIEventType::contactMessage || t == UIEventType::channelMessage || t == UIEventType::newContactMessage) {
    vibration.trigger();
  }
#endif
  if (t != UIEventType::ack && t != UIEventType::none) wake();
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
  } else if (key == KEY_KB_ESC && strcmp(_node_prefs->node_name, "NONAME") != 0) {
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
  if (yes && p == KPrompt::INCOMING_REQUEST) acceptDM(_prompt_name, _prompt_ch_idx);
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

  // build wrapped lines, newest message last
  const int MAX_LINES = 40;
  static char lines[MAX_LINES][COLS + 1];
  int total = 0;
  int count = 0;
  int order[KISS_MAX_MSGS];
  for (int n = KISS_MAX_MSGS; n >= 1; n--) {   // oldest -> newest
    int i = (_msg_head - n + KISS_MAX_MSGS) % KISS_MAX_MSGS;
    if (_msgs[i].id && _msgs[i].chat == _chat) order[count++] = i;
  }
  // only the newest messages that fit
  char buf[KISS_TEXT_MAX + 16];
  char wrapped[12][COLS + 1];
  for (int k = count - 1; k >= 0 && total < MAX_LINES; k--) {
    KMsg& m = _msgs[order[k]];
    const char* mark = "";
    if (m.status == MSG_OUT_PENDING) mark = " ..";
    else if (m.status == MSG_OUT_DELIVERED) mark = " +";
    else if (m.status == MSG_OUT_FAILED) mark = " !";
    char text[KISS_TEXT_MAX + 1];
    _display->translateUTF8ToBlocks(text, m.text, sizeof(text));
    snprintf(buf, sizeof(buf), "%s%s%s", m.status == MSG_IN ? "" : ">", text, mark);
    int n = wrapText(buf, wrapped, 12);
    // prepend (we walk newest -> oldest)
    int shift = n;
    if (total + shift > MAX_LINES) shift = MAX_LINES - total;
    memmove(lines[shift], lines[0], total * (COLS + 1));
    for (int j = 0; j < shift; j++) strcpy(lines[j], wrapped[n - shift + j]);
    total += shift;
  }

  const int rows = 5;
  int max_scroll = total > rows ? total - rows : 0;
  if (_scroll > max_scroll) _scroll = max_scroll;
  int first = total - rows - _scroll;
  if (first < 0) first = 0;
  for (int r = 0; r < rows && first + r < total; r++) {
    _display->setCursor(0, ROW_H + 1 + r * ROW_H);
    _display->print(lines[first + r]);
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
  const char* title = _prompt == KPrompt::INCOMING_REQUEST ? "DM request" : "Request DM?";
  drawTitle(title, NULL);
  char line[48];
  if (_prompt == KPrompt::INCOMING_REQUEST) snprintf(line, sizeof(line), "%s wants to DM you.", _prompt_name);
  else snprintf(line, sizeof(line), "Ask %s for a DM?", _prompt_name);
  char wrapped[4][COLS + 1];
  int n = wrapText(line, wrapped, 4);
  for (int i = 0; i < n; i++) {
    _display->setCursor(0, ROW_H + 4 + i * ROW_H);
    _display->print(wrapped[i]);
  }
  _display->setCursor(0, _display->height() - ROW_H);
  _display->print(_prompt == KPrompt::INCOMING_REQUEST ? "Enter=accept Esc=no" : "Enter=yes Esc=no");
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

#ifdef PIN_BUZZER
  if (buzzer.isPlaying()) buzzer.loop();
#endif
#ifdef PIN_VIBRATION
  vibration.loop();
#endif

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
