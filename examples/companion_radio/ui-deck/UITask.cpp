#include "UITask.h"
#include <helpers/TxtDataHelpers.h>
#include <helpers/AdvertDataHelpers.h>
#include <SPIFFS.h>
#include <esp_system.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansOblique9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include "../MyMesh.h"
#include "target.h"
#include "DeckPresets.h"

#define G (_display->gfx())

volatile int16_t DeckInput::_edges[4] = { 0, 0, 0, 0 };
void IRAM_ATTR DeckInput::isrUp()    { _edges[0]++; }
void IRAM_ATTR DeckInput::isrDown()  { _edges[1]++; }
void IRAM_ATTR DeckInput::isrLeft()  { _edges[2]++; }
void IRAM_ATTR DeckInput::isrRight() { _edges[3]++; }

// ---------------------------------------------------------------- look
//
// The phone app's dark theme (Material 3, OLED black), as RGB565.

#define C_PRIMARY       0x5D9F   // #5EB0FF
#define C_ON_PRIMARY    0x0882   // #0F1115
#define C_OUT_BUBBLE    0x19CB   // primaryContainer #1A3A5C
#define C_BG            0x0000   // #000000
#define C_SURFACE       0x0841   // #0A0A0C
#define C_IN_BUBBLE     0x10A3   // surfaceVariant #15161B
#define C_SEC_CONT      0x2187   // secondaryContainer #26313F
#define C_TEXT          0xE75D   // #E6E8EE
#define C_MUTED         0xA577   // #A6ADBC
#define C_OUTLINE       0x6B90   // #6B7283
#define C_OUTLINE_VAR   0x29A7   // #2E343F
#define C_ERROR         0xFB8E   // #F87171
#define C_TERTIARY      0xE507   // #E0A33A
#define C_ONLINE        0x4508   // #43A047

#define BAR_H       30           // app bar
#define NAV_H       30           // bottom navigation (Chats | Settings)
#define COMPOSE_H   32
#define ROW_H       36           // chat list / settings rows
#define HDR_H       16           // settings group header
#define LINE_H      17           // FreeSans 9pt line pitch
#define PAD_X       8
#define PAD_Y       5
#define NAME_H      12           // sender name above the text (6x8 font)
#define META_H      11           // time + status line
#define MSG_GAP     4
#define SEP_H       20           // day separator pill
#define BUBBLE_MAX  224          // widest bubble text

#define FONT        (&FreeSans9pt7b)
#define FONT_BOLD   (&FreeSansBold9pt7b)
#define FONT_ITALIC (&FreeSansOblique9pt7b)
#define FONT_TITLE  (&FreeSansBold12pt7b)
#define FONT_SMALL  ((const GFXfont*)NULL)   // built-in 6x8

static const unsigned long TIMEOUTS[] = { 30000UL, 60000UL, 120000UL, 300000UL, 600000UL, 0 };
static const char* TIMEOUT_NAMES[] = { "30 seconds", "1 minute", "2 minutes", "5 minutes", "10 minutes", "Never" };
#define NUM_TIMEOUTS 6

static const uint8_t PUBLIC_PSK[16] = { 0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a, 0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72 };

static uint16_t rgb565(uint32_t rgb) {
  return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F);
}

// The app's avatar colour (shared/.../util/AvatarColors.kt): the first six hex
// digits found in the seed are the RGB value (zero padded); with none, Java's
// String.hashCode(). Text is black on light colours.
static uint32_t avatarRgb(const char* seed) {
  uint32_t v = 0;
  int n = 0;
  for (const char* p = seed; *p && n < 6; p++) {
    char c = tolower(*p);
    if (c >= '0' && c <= '9') { v = (v << 4) | (c - '0'); n++; }
    else if (c >= 'a' && c <= 'f') { v = (v << 4) | (c - 'a' + 10); n++; }
  }
  if (n == 0) {
    int32_t h = 0;
    for (const char* p = seed; *p; p++) h = 31 * h + (uint8_t)*p;
    return (uint32_t)h & 0xFFFFFF;
  }
  return v << (4 * (6 - n));
}

static bool isLight(uint32_t rgb) {
  float l = (0.299f * ((rgb >> 16) & 0xFF) + 0.587f * ((rgb >> 8) & 0xFF) + 0.114f * (rgb & 0xFF)) / 255.0f;
  return l > 0.5f;
}

// ---------------------------------------------------------------- text helpers

static int textWidth(const GFXfont* f, const char* s) {
  if (f == NULL) return 6 * strlen(s);
  int w = 0;
  for (; *s; s++) {
    uint8_t c = *s;
    if (c < f->first || c > f->last) continue;
    w += f->glyph[c - f->first].xAdvance;
  }
  return w;
}

static int charWidth(const GFXfont* f, char ch) {
  char s[2] = { ch, 0 };
  return textWidth(f, s);
}

// UTF-8 to the fonts' printable ASCII; a few common characters get a stand-in.
static void toAscii(char* dest, const char* src, size_t size) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 4 < size; i++) {
    uint8_t c = src[i];
    if (c >= 0x20 && c < 0x7F) { dest[j++] = c; continue; }
    if (c < 0x80) continue;
    uint32_t cp = 0;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    cp = c & (0x3F >> extra);
    for (int k = 0; k < extra && (src[i + 1] & 0xC0) == 0x80; k++) cp = (cp << 6) | (src[++i] & 0x3F);
    const char* sub = "?";
    if (cp == 0x2018 || cp == 0x2019) sub = "'";
    else if (cp == 0x201C || cp == 0x201D) sub = "\"";
    else if (cp == 0x2013 || cp == 0x2014) sub = "-";
    else if (cp == 0x2026) sub = "...";
    else if (cp == 0xA0) sub = " ";
    else if (cp >= 0xFE00 && cp <= 0xFE0F) sub = "";   // emoji variation selectors
    else if (cp == 0x200D) sub = "";
    for (const char* s = sub; *s && j + 1 < size; s++) dest[j++] = *s;
  }
  dest[j] = 0;
}

// Word wrap to max_w pixels; returns the number of lines.
static int wrapText(const GFXfont* f, const char* text, int max_w, char lines[][64], int max_lines) {
  int n = 0;
  int i = 0;
  while (text[i] && n < max_lines) {
    int start = i, w = 0, last_space = -1, j = i;
    while (text[j]) {
      int cw = charWidth(f, text[j]);
      if (w + cw > max_w || j - start >= 63) break;
      if (text[j] == ' ') last_space = j;
      w += cw;
      j++;
    }
    int end, next;
    if (text[j] == 0) { end = j; next = j; }
    else if (last_space > start) { end = last_space; next = last_space + 1; }
    else { end = j > start ? j : start + 1; next = end; }
    int len = end - start;
    if (len > 63) len = 63;
    memcpy(lines[n], text + start, len);
    lines[n][len] = 0;
    n++;
    i = next;
    while (text[i] == ' ') i++;
    if (text[i] && n == max_lines && len > 3) strcpy(lines[n - 1] + len - 3, "...");
  }
  return n;
}

// Copies s cut to max_w pixels with "..." if it doesn't fit.
static void ellipsize(const GFXfont* f, const char* s, int max_w, char* out, size_t size) {
  StrHelper::strncpy(out, s, size);
  if (textWidth(f, out) <= max_w) return;
  int len = strlen(out);
  while (len > 0) {
    out[--len] = 0;
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s...", out);
    if (textWidth(f, tmp) <= max_w) {
      StrHelper::strncpy(out, tmp, size);
      return;
    }
  }
}

static void drawText(GFXcanvas16* g, const GFXfont* f, int x, int y, uint16_t color, const char* s) {
  // y is the top of the line for every font (custom fonts draw at the baseline)
  g->setFont(f);
  g->setTextSize(1);
  g->setTextColor(color);
  g->setCursor(x, f ? y + 13 : y);
  g->print(s);
}

static void drawTextRight(GFXcanvas16* g, const GFXfont* f, int right_x, int y, uint16_t color, const char* s) {
  drawText(g, f, right_x - textWidth(f, s), y, color, s);
}

// The letter in an avatar circle, as the app's AvatarGlyph: the first letter
// or digit, upper-cased, skipping emoji and symbols in front of it; "?" when
// there is none (the app would show an emoji, which this font can't draw).
static void avatarGlyph(const char* name, char* out) {
  for (const char* p = name; *p; p++) {
    if (isalnum((uint8_t)*p)) { out[0] = toupper((uint8_t)*p); out[1] = 0; return; }
  }
  strcpy(out, "?");
}

static void drawTick(GFXcanvas16* g, int x, int y, uint16_t c) {   // 7x6 check mark
  g->drawLine(x, y + 3, x + 2, y + 5, c);
  g->drawLine(x + 2, y + 5, x + 6, y + 0, c);
}

// ---------------------------------------------------------------- setup

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _display = (DeckDisplay*)display;
  _sensors = sensors;
  _node_prefs = node_prefs;
  _msgs = (DMsg*)ps_calloc(DECK_MAX_MSGS, sizeof(DMsg));
  if (_msgs == NULL) _msgs = (DMsg*)calloc(DECK_MAX_MSGS, sizeof(DMsg));
  memset(_handshakes, 0, sizeof(_handshakes));
  memset(&_out, 0, sizeof(_out));
  memset(_clock, 0, sizeof(_clock));
  _compose[0] = 0;
  _alert[0] = 0;
  loadUiPrefs();

  // Simple mode: contacts are only added through an accepted DM request.
  if (_node_prefs->manual_add_contacts != 1 || _node_prefs->autoadd_config != 0) {
    _node_prefs->manual_add_contacts = 1;
    _node_prefs->autoadd_config = 0;
    the_mesh.savePrefs();
  }

  _input.begin(Wire);
  _touch.begin(Wire);
  _sound.begin();

  rebuildChats();
  if (_ui.keep_msgs) loadMessages();
  if (_display) _display->turnOn();
  wake();

  if (isDefaultName()) openRegion(true);   // first start: region, then a name
  if (!_node_prefs->buzzer_quiet) {
    static const DeckNote hello[] = { { 1047, 70 }, { 1568, 110 } };
    _sound.play(hello, 2);
  }
}

void UITask::loadUiPrefs() {
  File f = SPIFFS.open("/deck_ui", "r");
  if (!f) return;
  DeckPrefs p;   // defaults for fields an older, shorter file doesn't have
  int n = f.read((uint8_t*)&p, sizeof(p));
  if (n >= 2) {
    if (p.tz_halfhours >= -24 && p.tz_halfhours <= 28) _ui.tz_halfhours = p.tz_halfhours;
    if (p.timeout_idx < NUM_TIMEOUTS) _ui.timeout_idx = p.timeout_idx;
  }
  if (n >= 3) _ui.keep_msgs = p.keep_msgs ? 1 : 0;
  f.close();
}

void UITask::saveUiPrefs() {
  File f = SPIFFS.open("/deck_ui", "w");
  if (!f) return;
  f.write((const uint8_t*)&_ui, sizeof(_ui));
  f.close();
}

unsigned long UITask::timeoutMillis() const { return TIMEOUTS[_ui.timeout_idx]; }

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
// With "Keep messages" on, the message ring is written to /deck_msgs at most
// every 15 s after a change (a power cut loses at most that much) and read
// back at boot. Each message is stored against its chat's identity (channel
// key or contact key), not a list position, so channels and contacts can
// change in between. Stored as plain text in flash, like the companion's own
// offline queue.

#define STORE_PATH      "/deck_msgs"
#define STORE_MAGIC     0x314D4B44UL   // "DKM1"
#define STORE_DELAY_MS  15000

struct StoredMsgHead {
  uint8_t type;            // DChatType
  uint8_t status;
  uint8_t attempt;
  uint8_t text_len;
  uint32_t ts;
  uint8_t key[PUB_KEY_SIZE];   // channel: its 16-byte secret (zero padded); DM: the contact's key
};

void UITask::messagesChanged() {
  if (!_ui.keep_msgs) return;
  if (!_store_dirty) _store_due = millis() + STORE_DELAY_MS;
  _store_dirty = true;
}

void UITask::saveMessages() {
  _store_dirty = false;
  if (!_ui.keep_msgs || _msgs == NULL) return;
  File f = SPIFFS.open(STORE_PATH, "w");
  if (!f) return;
  uint32_t magic = STORE_MAGIC;
  f.write((const uint8_t*)&magic, sizeof(magic));
  for (int n = DECK_MAX_MSGS; n >= 1; n--) {   // oldest first
    DMsg& m = _msgs[(_msg_head - n + DECK_MAX_MSGS) % DECK_MAX_MSGS];
    if (!m.id || m.chat < 0 || m.chat >= _num_chats) continue;
    DChat& c = _chats[m.chat];
    StoredMsgHead h;
    memset(&h, 0, sizeof(h));
    h.type = (uint8_t)c.type;
    h.status = m.status;
    h.attempt = m.attempt;
    h.ts = m.ts;
    size_t len = strlen(m.text);
    h.text_len = len > 255 ? 255 : len;
    if (c.type == DChatType::CHANNEL) {
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
  StoredMsgHead h;
  while (f.read((uint8_t*)&h, sizeof(h)) == sizeof(h)) {
    char text[DECK_TEXT_MAX + 1];
    int len = h.text_len > DECK_TEXT_MAX ? DECK_TEXT_MAX : h.text_len;
    if (f.read((uint8_t*)text, h.text_len) != h.text_len) break;
    text[len] = 0;
    int chat = -1;
    if (h.type == (uint8_t)DChatType::CHANNEL) {
      mesh::GroupChannel probe;
      memset(&probe, 0, sizeof(probe));
      memcpy(probe.secret, h.key, 16);
      int ch = the_mesh.findChannelIdx(probe);
      if (ch >= 0) chat = findChannelChat(ch);
    } else {
      chat = findDirectChat(h.key);
    }
    if (chat < 0) continue;   // that channel or contact is gone
    DMsg* m = &_msgs[_msg_head];
    _msg_head = (_msg_head + 1) % DECK_MAX_MSGS;
    m->id = _next_msg_id++;
    if (_next_msg_id == 0) _next_msg_id = 1;
    m->chat = chat;
    // a DM that was still being sent when we stopped didn't get through
    m->status = h.status == MSG_OUT_PENDING ? MSG_OUT_FAILED : h.status;
    m->attempt = h.attempt;
    m->ts = h.ts;
    StrHelper::strncpy(m->text, text, sizeof(m->text));
    if (h.ts > _chats[chat].last_activity) _chats[chat].last_activity = h.ts | 1;
  }
  f.close();
  _dirty = true;
}

void UITask::clearMessages() {
  memset(_msgs, 0, DECK_MAX_MSGS * sizeof(DMsg));
  _msg_head = 0;
  _sel_msg = -1;
  memset(&_out, 0, sizeof(_out));
  for (int i = 0; i < _num_chats; i++) {
    _chats[i].unread = 0;
    _chats[i].last_activity = 0;
  }
  SPIFFS.remove(STORE_PATH);
  _store_dirty = false;
  showAlert("All messages cleared", 2000);
}

// ---------------------------------------------------------------- chats + messages

void UITask::rebuildChats() {
  DChat old[DECK_MAX_CHATS];
  int old_n = _num_chats;
  memcpy(old, _chats, sizeof(old));
  _num_chats = 0;

  for (int i = 0; i < MAX_GROUP_CHANNELS && _num_chats < DECK_MAX_CHATS; i++) {
    ChannelDetails cd;
    if (!the_mesh.getChannel(i, cd) || cd.name[0] == 0) continue;
    DChat& c = _chats[_num_chats++];
    memset(&c, 0, sizeof(c));
    c.type = DChatType::CHANNEL;
    c.ch_idx = i;
    StrHelper::strncpy(c.name, cd.name, sizeof(c.name));
    if (memcmp(cd.channel.secret, PUBLIC_PSK, sizeof(PUBLIC_PSK)) == 0) c.kind = CHAN_PUBLIC;
    else if (cd.name[0] == '#') c.kind = CHAN_HASHTAG;
    else c.kind = CHAN_PRIVATE;
  }
  ContactsIterator it = the_mesh.startContactsIterator();
  ContactInfo ci;
  while (_num_chats < DECK_MAX_CHATS && it.hasNext(&the_mesh, ci)) {
    if (ci.type != ADV_TYPE_CHAT) continue;
    DChat& c = _chats[_num_chats++];
    memset(&c, 0, sizeof(c));
    c.type = DChatType::DIRECT;
    memcpy(c.pub_key, ci.id.pub_key, PUB_KEY_SIZE);
    StrHelper::strncpy(c.name, ci.name, sizeof(c.name));
  }

  // carry over unread counts and remap message ownership to the new indexes
  int remap[DECK_MAX_CHATS];
  for (int i = 0; i < old_n; i++) {
    remap[i] = old[i].type == DChatType::CHANNEL ? findChannelChat(old[i].ch_idx) : findDirectChat(old[i].pub_key);
    if (remap[i] >= 0) {
      _chats[remap[i]].unread = old[i].unread;
      _chats[remap[i]].last_activity = old[i].last_activity;
    }
  }
  for (int i = 0; i < DECK_MAX_MSGS; i++) {
    if (_msgs[i].id && _msgs[i].chat >= 0 && _msgs[i].chat < old_n) _msgs[i].chat = remap[_msgs[i].chat];
  }
  if (_chat >= 0 && _chat < old_n) _chat = remap[_chat];
  if (_list_sel >= _num_chats) _list_sel = _num_chats > 0 ? _num_chats - 1 : 0;
  _dirty = true;
}

int UITask::findChannelChat(uint8_t ch_idx) const {
  for (int i = 0; i < _num_chats; i++) {
    if (_chats[i].type == DChatType::CHANNEL && _chats[i].ch_idx == ch_idx) return i;
  }
  return -1;
}

int UITask::findDirectChat(const uint8_t* pub_key) const {
  for (int i = 0; i < _num_chats; i++) {
    if (_chats[i].type == DChatType::DIRECT && memcmp(_chats[i].pub_key, pub_key, PUB_KEY_SIZE) == 0) return i;
  }
  return -1;
}

DMsg* UITask::addMsg(int chat, uint8_t status, const char* text) {
  DMsg* m = &_msgs[_msg_head];
  _msg_head = (_msg_head + 1) % DECK_MAX_MSGS;
  if (_sel_msg == m - _msgs) _sel_msg = -1;   // overwriting the highlighted one
  m->id = _next_msg_id++;
  if (_next_msg_id == 0) _next_msg_id = 1;
  m->chat = chat;
  m->status = status;
  m->attempt = 0;
  m->ts = the_mesh.getRTCClock()->getCurrentTime();
  StrHelper::strncpy(m->text, text, sizeof(m->text));
  _dirty = true;
  messagesChanged();
  return m;
}

DMsg* UITask::findMsg(uint16_t id) {
  for (int i = 0; i < DECK_MAX_MSGS; i++) {
    if (_msgs[i].id == id) return &_msgs[i];
  }
  return NULL;
}

DMsg* UITask::lastMsg(int chat) {
  for (int n = 1; n <= DECK_MAX_MSGS; n++) {
    DMsg& m = _msgs[(_msg_head - n + DECK_MAX_MSGS) % DECK_MAX_MSGS];
    if (m.id && m.chat == chat) return &m;
  }
  return NULL;
}

// message slots of a chat, oldest first
int UITask::chatMessages(int chat, int* slots, int max) {
  int count = 0;
  for (int n = DECK_MAX_MSGS; n >= 1 && count < max; n--) {
    int i = (_msg_head - n + DECK_MAX_MSGS) % DECK_MAX_MSGS;
    if (_msgs[i].id && _msgs[i].chat == chat) slots[count++] = i;
  }
  return count;
}

// chats newest activity first (as the app); quiet ones keep their order
int UITask::sortedChats(int* order) {
  for (int i = 0; i < _num_chats; i++) order[i] = i;
  for (int i = 1; i < _num_chats; i++) {   // stable insertion sort
    int v = order[i], j = i - 1;
    while (j >= 0 && _chats[order[j]].last_activity < _chats[v].last_activity) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = v;
  }
  return _num_chats;
}

int UITask::totalUnread() const {
  int t = 0;
  for (int i = 0; i < _num_chats; i++) t += _chats[i].unread;
  return t;
}

void UITask::markActivity(int chat, bool incoming) {
  if (chat < 0) return;
  _chats[chat].last_activity = the_mesh.getRTCClock()->getCurrentTime() | 1;
  bool visible = _screen == DScreen::CHAT && _chat == chat && _display && _display->isOn();
  if (incoming && !visible && _chats[chat].unread < 99) _chats[chat].unread++;
}

// ---------------------------------------------------------------- sending

int UITask::composeLimit() const {
  if (_screen == DScreen::NAME) return sizeof(_node_prefs->node_name) - 1;
  if (_chat >= 0 && _chats[_chat].type == DChatType::CHANNEL) {
    int lim = DECK_TEXT_MAX - (int)strlen(_node_prefs->node_name) - 2;   // "name: " prefix
    return lim < DECK_COMPOSE_MAX ? lim : DECK_COMPOSE_MAX;
  }
  return DECK_COMPOSE_MAX;
}

bool UITask::sendChannelText(uint8_t ch_idx, const char* text) {
  ChannelDetails cd;
  if (!the_mesh.getChannel(ch_idx, cd)) return false;
  uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
  return the_mesh.sendGroupMessage(ts, cd.channel, _node_prefs->node_name, text, strlen(text));
}

void UITask::sendCompose() {
  if (_compose_len == 0 || _chat < 0) return;
  DChat& c = _chats[_chat];
  if (c.type == DChatType::CHANNEL) {
    if (sendChannelText(c.ch_idx, _compose)) {
      addMsg(_chat, MSG_OUT, _compose);
    } else {
      showAlert("Couldn't send", 2000);
      return;
    }
  } else {
    if (_out.active) {
      showAlert("Still sending the last message", 2000);
      return;
    }
    ContactInfo* ci = the_mesh.lookupContactByPubKey(c.pub_key, PUB_KEY_SIZE);
    if (ci == NULL) {
      showAlert("Contact missing", 2000);
      return;
    }
    startDM(*ci, _compose);
  }
  markActivity(_chat, false);
  _compose[0] = 0;
  _compose_len = 0;
  _sel_msg = -1;
  _scroll_px = 0;
}

void UITask::startDM(const ContactInfo& c, const char* text) {
  DMsg* m = addMsg(findDirectChat(c.id.pub_key), MSG_OUT_PENDING, text);
  memset(&_out, 0, sizeof(_out));
  _out.active = true;
  _out.msg_id = m->id;
  memcpy(_out.pub_key, c.id.pub_key, PUB_KEY_SIZE);
  _out.timestamp = the_mesh.getRTCClock()->getCurrentTimeUnique();
  _out.attempt = 0;
  sendDMAttempt();
}

void UITask::sendDMAttempt() {
  DMsg* m = findMsg(_out.msg_id);
  ContactInfo* ci = the_mesh.lookupContactByPubKey(_out.pub_key, PUB_KEY_SIZE);
  if (m == NULL || ci == NULL) {
    _out.active = false;
    return;
  }
  // last attempt: forget the stored route and flood, as the app does
  if (_out.attempt == DECK_DM_ATTEMPTS - 1) the_mesh.resetPathTo(*ci);

  uint32_t ack = 0, est_timeout = 0;
  int rc = the_mesh.sendMessage(*ci, _out.timestamp, _out.attempt, m->text, ack, est_timeout);
  m->attempt = _out.attempt;
  _dirty = true;
  if (rc == MSG_SEND_FAILED) {
    m->status = MSG_OUT_FAILED;
    _out.active = false;
    messagesChanged();
    return;
  }
  _out.acks[_out.attempt] = ack;
  _out.deadline = millis() + est_timeout + 1000;
}

void UITask::checkOutgoing() {
  if (!_out.active || (long)(millis() - _out.deadline) < 0) return;
  if (_out.attempt + 1 < DECK_DM_ATTEMPTS) {
    _out.attempt++;
    sendDMAttempt();
  } else {
    DMsg* m = findMsg(_out.msg_id);
    if (m) m->status = MSG_OUT_FAILED;
    _out.active = false;
    _dirty = true;
    messagesChanged();
  }
}

void UITask::onACKRecv(uint32_t ack_crc) {
  if (!_out.active) return;
  for (int i = 0; i <= _out.attempt && i < DECK_DM_ATTEMPTS; i++) {
    if (_out.acks[i] == ack_crc && ack_crc != 0) {
      DMsg* m = findMsg(_out.msg_id);
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

DHandshake* UITask::findHandshake(const char* name) {
  for (int i = 0; i < DECK_HANDSHAKES; i++) {
    DHandshake& h = _handshakes[i];
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

// ---------------------------------------------------------------- shared channel links
//
// The app shares a channel as meshcore://channel/add?name=<name>&secret=<32 hex>
// (its ShareUri, after the firmware's docs/qr_codes.md), and shows such links
// in chats as tappable. Sent in a DM, the key reaches only that person, so a
// received one is offered as "Join <name>?". Decoded as the app does: '+' is a
// space, %XX escapes, first copy of a parameter wins, "channel_secret" is the
// app's old name for "secret", a '#' ends the query, and an all-zero key is
// refused.

static bool linkEndChar(char c) {
  return c == 0 || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '<' || c == '>' || c == '"' || c == '\'' || c == ']';
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// percent-decode src[0..len) (with '+' as space) into out; false on a bad escape
static bool formDecode(const char* src, int len, char* out, int out_size) {
  int n = 0;
  for (int i = 0; i < len; i++) {
    char c = src[i];
    if (c == '%') {
      if (i + 2 >= len) return false;   // truncated escape
      int hi = hexVal(src[i + 1]), lo = hexVal(src[i + 2]);
      if (hi < 0 || lo < 0) return false;
      c = (char)((hi << 4) | lo);
      i += 2;
    } else if (c == '+') {
      c = ' ';
    }
    if (n + 1 < out_size) out[n++] = c;
  }
  out[n] = 0;
  return true;
}

// Finds a channel link in text. On success fills name (as the app cleans it:
// control characters dropped, trimmed, at most 31 bytes) and secret, and the
// link's span in text.
static bool parseChannelLink(const char* text, char* name, int name_size, uint8_t* secret,
                             const char** link_start, const char** link_end) {
  static const char SCHEME[] = "meshcore://";
  static const char PATH[] = "channel/add";
  for (const char* p = text; *p; p++) {
    if (strncasecmp(p, SCHEME, sizeof(SCHEME) - 1) != 0) continue;
    const char* end = p;
    while (!linkEndChar(*end)) end++;
    const char* q = p + sizeof(SCHEME) - 1;
    if (end - q < (int)sizeof(PATH) - 1 || strncasecmp(q, PATH, sizeof(PATH) - 1) != 0) continue;
    q += sizeof(PATH) - 1;
    if (q >= end || *q != '?') continue;
    q++;
    const char* qend = q;
    while (qend < end && *qend != '#') qend++;

    bool have_name = false, have_secret = false, have_legacy = false;
    char raw_name[96] = "", hex[40] = "", legacy[40] = "";
    bool ok = true;
    while (q < qend && ok) {
      const char* amp = q;
      while (amp < qend && *amp != '&') amp++;
      const char* eq = q;
      while (eq < amp && *eq != '=') eq++;
      int klen = eq - q;
      const char* v = eq < amp ? eq + 1 : amp;
      int vlen = amp - v;
      if (klen == 4 && strncasecmp(q, "name", 4) == 0 && !have_name) {
        ok = formDecode(v, vlen, raw_name, sizeof(raw_name)); have_name = true;
      } else if (klen == 6 && strncasecmp(q, "secret", 6) == 0 && !have_secret) {
        ok = formDecode(v, vlen, hex, sizeof(hex)); have_secret = true;
      } else if (klen == 14 && strncasecmp(q, "channel_secret", 14) == 0 && !have_legacy) {
        ok = formDecode(v, vlen, legacy, sizeof(legacy)); have_legacy = true;
      }
      q = amp < qend ? amp + 1 : qend;
    }
    if (!ok) continue;
    const char* key = have_secret ? hex : have_legacy ? legacy : NULL;
    if (key == NULL) continue;
    while (*key == ' ') key++;
    int klen = strlen(key);
    while (klen > 0 && key[klen - 1] == ' ') klen--;
    if (klen != 32 || !readHex(key, secret, 16)) continue;
    bool zero = true;
    for (int i = 0; i < 16; i++) if (secret[i]) zero = false;
    if (zero) continue;

    // clean the name: drop control characters, trim, cut to 31 bytes on a UTF-8 boundary
    char clean[96];
    int n = 0;
    for (const char* c = raw_name; *c; c++) {
      if ((uint8_t)*c >= 0x20 && *c != 0x7F && n + 1 < (int)sizeof(clean)) clean[n++] = *c;
    }
    clean[n] = 0;
    char* c0 = clean;
    while (*c0 == ' ') c0++;
    n = strlen(c0);
    while (n > 0 && c0[n - 1] == ' ') n--;
    int max = name_size - 1 < 31 ? name_size - 1 : 31;
    if (n > max) {
      n = max;
      while (n > 0 && ((uint8_t)c0[n] & 0xC0) == 0x80) n--;   // don't split a character
    }
    if (n == 0) continue;   // a channel needs a name
    memcpy(name, c0, n);
    name[n] = 0;
    if (link_start) *link_start = p;
    if (link_end) *link_end = end;
    return true;
  }
  return false;
}

// The channel slot already holding this key, or -1.
static int heldChannel(const uint8_t* secret) {
  mesh::GroupChannel probe;
  memset(&probe, 0, sizeof(probe));
  memcpy(probe.secret, secret, 16);
  int idx = the_mesh.findChannelIdx(probe);
  ChannelDetails cd;
  if (idx >= 0 && the_mesh.getChannel(idx, cd) && cd.name[0]) return idx;
  return -1;
}

// If text carries a channel link we don't hold yet, remember it for
// JOIN_CHANNEL / ACT_JOIN; with ask, also put up "Join <name>?" now.
bool UITask::offerJoin(const char* text, const char* from, bool ask) {
  char name[32];
  uint8_t secret[16];
  if (!parseChannelLink(text, name, sizeof(name), secret, NULL, NULL)) return false;
  if (heldChannel(secret) >= 0) return false;
  StrHelper::strncpy(_join_name, name, sizeof(_join_name));
  memcpy(_join_secret, secret, 16);
  StrHelper::strncpy(_join_from, from, sizeof(_join_from));
  if (ask) showPrompt(DPrompt::JOIN_CHANNEL, name, 0);
  return true;
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

// "Message privately": open the chat if we already have them, else confirm.
void UITask::askDM(const char* name, uint8_t ch_idx) {
  ContactInfo* existing = findContactByName(name);
  if (existing) {
    int idx = findDirectChat(existing->id.pub_key);
    if (idx >= 0) openChat(idx);
    return;
  }
  showPrompt(DPrompt::CONFIRM_REQUEST, name, ch_idx);
}

void UITask::requestDM(const char* name, uint8_t ch_idx) {
  char hex[PUB_KEY_SIZE * 2 + 1];
  toLowerHex(hex, the_mesh.self_id.pub_key, PUB_KEY_SIZE);
  char text[DECK_TEXT_MAX + 1];
  snprintf(text, sizeof(text), "%s%s%s%s", REQ_PREFIX, name, REQ_KEY, hex);
  if ((int)(strlen(text) + strlen(_node_prefs->node_name) + 2) > DECK_TEXT_MAX) {
    showAlert("Names too long to send the request", 2500);   // the key would be cut off
    return;
  }
  if (!sendChannelText(ch_idx, text)) {
    showAlert("Couldn't send", 2000);
    return;
  }
  int chat = findChannelChat(ch_idx);
  addMsg(chat, MSG_OUT, text);
  markActivity(chat, false);

  DHandshake* h = findHandshake(name);
  if (h == NULL) {
    h = &_handshakes[0];
    for (int i = 0; i < DECK_HANDSHAKES; i++) {
      if (_handshakes[i].name[0] == 0) { h = &_handshakes[i]; break; }
      if ((long)(_handshakes[i].expires - h->expires) < 0) h = &_handshakes[i];   // replace soonest to expire
    }
  }
  StrHelper::strncpy(h->name, name, sizeof(h->name));
  h->expires = millis() + HANDSHAKE_TTL_MILLIS;
  char msg[96];
  snprintf(msg, sizeof(msg), "Asked %s. You can message them once they accept.", name);
  showAlert(msg, 3500);
}

void UITask::acceptDM() {
  char msg[96];
  if (!addContactFromKey(_prompt_name, _prompt_key)) {
    snprintf(msg, sizeof(msg), "Couldn't add %s - is the radio's contact list full?", _prompt_name);
    showAlert(msg, 3500);
    return;
  }
  char req_hex[9], my_hex[PUB_KEY_SIZE * 2 + 1];
  toLowerHex(req_hex, _prompt_key, 4);
  toLowerHex(my_hex, the_mesh.self_id.pub_key, PUB_KEY_SIZE);
  char text[DECK_TEXT_MAX + 1];
  snprintf(text, sizeof(text), "%s%s%s%s:%s", ACC_PREFIX, _prompt_name, ACC_KEY, req_hex, my_hex);
  if ((int)(strlen(text) + strlen(_node_prefs->node_name) + 2) > DECK_TEXT_MAX || !sendChannelText(_prompt_ch_idx, text)) {
    showAlert("Couldn't send the answer", 2500);
    return;
  }
  int chat = findChannelChat(_prompt_ch_idx);
  addMsg(chat, MSG_OUT, text);
  markActivity(chat, false);
  snprintf(msg, sizeof(msg), "Added %s. You can message each other directly.", _prompt_name);
  showAlert(msg, 3500);
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
    showPrompt(DPrompt::INCOMING_REQUEST, sender, ch_idx);
    notify(UIEventType::contactMessage);
    return;
  }
  hex = parseControl(body, ACC_PREFIX, ACC_KEY, name, sizeof(name));
  if (hex) {
    uint8_t prefix[4], key[PUB_KEY_SIZE];
    if (!readHex(hex, prefix, 4) || hex[8] != ':' || !readHex(hex + 9, key, PUB_KEY_SIZE)) return;
    if (memcmp(prefix, the_mesh.self_id.pub_key, 4) != 0) return;   // not accepting *our* request
    DHandshake* h = findHandshake(sender);
    if (h == NULL) return;                                          // we didn't ask them
    if (addContactFromKey(sender, key)) {
      h->name[0] = 0;
      char msg[96];
      snprintf(msg, sizeof(msg), "%s accepted. You can message each other directly.", sender);
      showAlert(msg, 3500);
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
  DClockSample* slot = NULL;
  for (int i = 0; i < DECK_CLOCK_SAMPLES; i++) {
    DClockSample& c = _clock[i];
    if (c.source && now_ms - c.at > CLOCK_SAMPLE_TTL_MILLIS) c.source = 0;   // expired
    if (c.source == src) { slot = &c; break; }
  }
  if (slot == NULL) {
    for (int i = 0; i < DECK_CLOCK_SAMPLES; i++) {
      if (_clock[i].source == 0) { slot = &_clock[i]; break; }
      if (slot == NULL || (long)(_clock[i].at - slot->at) < 0) slot = &_clock[i];
    }
  }
  slot->source = src;
  slot->offset = offset;
  slot->at = now_ms;

  if (abs(offset) <= CLOCK_CLOSE_ENOUGH) return;   // we already agree with them

  for (int i = 0; i < DECK_CLOCK_SAMPLES; i++) {
    DClockSample& c = _clock[i];
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
  // a channel shared with us: ask now, unless another question is up or
  // they're typing (Enter would answer it)
  bool typing = _compose_len > 0 || _screen == DScreen::NAME || _screen == DScreen::CHANNEL_FORM;
  bool ask = _prompt == DPrompt::NONE && !typing;
  if (offerJoin(text, from.name, ask)) {
    if (!ask) {
      char msg[64];
      snprintf(msg, sizeof(msg), "%s shared a channel - tap the message to join", from.name);
      showAlert(msg, 3500);
    }
    notify(UIEventType::contactMessage);
    return;
  }
  char msg[48];
  snprintf(msg, sizeof(msg), "Message from %s", from.name);
  if (!(_screen == DScreen::CHAT && _chat == chat)) showAlert(msg, 2500);
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
  notify(UIEventType::channelMessage);
}

void UITask::onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) {
  syncClock(contact.name, contact.last_advert_timestamp);
}

// ---------------------------------------------------------------- notifications

void UITask::beep(const DeckNote* notes, int count) {
  if (!_node_prefs->buzzer_quiet) _sound.play(notes, count);
}

void UITask::notify(UIEventType t) {
  static const DeckNote dm[] = { { 1319, 60 }, { 0, 30 }, { 1568, 60 }, { 0, 30 }, { 1976, 90 } };
  static const DeckNote chan[] = { { 1568, 40 }, { 0, 25 }, { 1109, 60 } };
  static const DeckNote ack[] = { { 2093, 25 } };
  static const DeckNote contact[] = { { 1047, 80 }, { 1319, 80 }, { 1568, 120 } };
  switch (t) {
    case UIEventType::contactMessage:    beep(dm, 5); break;
    case UIEventType::channelMessage:    beep(chan, 3); break;
    case UIEventType::ack:               beep(ack, 1); break;
    case UIEventType::newContactMessage: beep(contact, 3); break;
    default: break;
  }
  if (t != UIEventType::ack && t != UIEventType::none) wake();
}

void UITask::wake() {
  if (_display && !_display->isOn()) _display->turnOn();
  _auto_off = millis() + timeoutMillis();
  _dirty = true;
}

void UITask::showAlert(const char* text, int millis_dur) {
  StrHelper::strncpy(_alert, text, sizeof(_alert));
  _alert_until = millis() + millis_dur;
  _dirty = true;
}

void UITask::showPrompt(DPrompt p, const char* name, uint8_t ch_idx) {
  if (_screen != DScreen::PROMPT) _prompt_return = _screen == DScreen::ACTIONS ? DScreen::CHAT : _screen;
  _prompt = p;
  _prompt_yes = true;
  StrHelper::strncpy(_prompt_name, name, sizeof(_prompt_name));
  _prompt_ch_idx = ch_idx;
  _screen = DScreen::PROMPT;
  wake();
}

// ---------------------------------------------------------------- time

long UITask::localDay(uint32_t ts) const {
  return ((int64_t)ts + _ui.tz_halfhours * 1800L) / 86400;
}

static const char* WEEKDAYS[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char* MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void civilFromDays(long z, int& y, int& m, int& d) {   // days since 1970 -> date
  z += 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  long doe = z - era * 146097;
  long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  long mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = yoe + era * 400 + (m <= 2);
}

// clock_only: "14:05"; else a day label for separators ("Today", "Yesterday",
// weekday within a week, "Nov 3")
void UITask::formatTime(uint32_t ts, char* out, bool clock_only) {
  int64_t local = (int64_t)ts + _ui.tz_halfhours * 1800L;
  if (clock_only) {
    sprintf(out, "%02d:%02d", (int)((local / 3600) % 24), (int)((local / 60) % 60));
    return;
  }
  long day = localDay(ts);
  long today = localDay(the_mesh.getRTCClock()->getCurrentTime());
  if (day == today) strcpy(out, "Today");
  else if (day == today - 1) strcpy(out, "Yesterday");
  else if (day > today - 7 && day < today) strcpy(out, WEEKDAYS[(day + 4) % 7]);
  else {
    int y, m, d;
    civilFromDays(day, y, m, d);
    sprintf(out, "%s %d", MONTHS[m - 1], d);
  }
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
  _sel_msg = -1;
  _scroll_px = 0;
  _screen = DScreen::CHAT;
  _dirty = true;
}

// printable characters, backspace; returns true if the key was used
bool UITask::composeKey(int key) {
  if (key == KEY_BACKSPACE) {
    if (_compose_len > 0) {
      _compose[--_compose_len] = 0;
      return true;
    }
    return false;
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

void UITask::keyChats(int key) {
  int order[DECK_MAX_CHATS];
  int n = sortedChats(order);
  if (key == KEY_UP) {
    if (_list_sel > 0) _list_sel--;
  } else if (key == KEY_DOWN) {
    if (_list_sel + 1 < n) _list_sel++;
  } else if (key == KEY_CLICK || key == KEY_ENTER) {
    if (n > 0) openChat(order[_list_sel]);
  } else if (key == KEY_RIGHT) {
    _screen = DScreen::SETTINGS;
  } else if (key >= 0x20 && key < 0x7F && n > 0) {
    openChat(order[_list_sel]);      // start typing straight away
    composeKey(key);
  }
}

// settings rows: 'H' group header, else an item id
struct SetRow { char kind; const char* title; };
static const SetRow SET_ROWS[] = {
  { 'H', "This node" },
  { 'n', "Identity" },
  { 'r', "Radio" },
  { 'z', "Send advert (0-hop)" },
  { 'f', "Send advert (flood)" },
  { 'H', "Messaging" },
  { 'c', "Channels" },
  { 'k', "Keep messages" },
  { 'x', "Clear all messages" },
  { 'H', "Appearance and alerts" },
  { 's', "Sound" },
  { 't', "Time zone" },
  { 'o', "Screen timeout" },
  { 'H', "App" },
  { 'a', "About" },
};
#define NUM_SET_ROWS (int)(sizeof(SET_ROWS) / sizeof(SET_ROWS[0]))

void UITask::activateSetting(int item) {
  switch (SET_ROWS[item].kind) {
    case 'n':
      StrHelper::strncpy(_compose, _node_prefs->node_name, sizeof(_compose));
      _compose_len = strlen(_compose);
      _screen = DScreen::NAME;
      break;
    case 'r':
      openRegion(false);
      break;
    case 'z':
      showAlert(the_mesh.advert() ? "Advert sent to nearby nodes" : "Advert failed", 2000);
      break;
    case 'f':
      showAlert(the_mesh.floodAdvert() ? "Advert sent across the mesh" : "Advert failed", 2000);
      break;
    case 'c':
      openChannels();
      break;
    case 'k':
      if (_ui.keep_msgs) {
        showPrompt(DPrompt::KEEP_OFF, "", 0);   // turning it off clears everything: ask first
      } else {
        _ui.keep_msgs = 1;
        saveUiPrefs();
        saveMessages();
        showAlert("Messages are saved on this device", 2500);
      }
      break;
    case 'x':
      showPrompt(DPrompt::CLEAR_MESSAGES, "", 0);
      break;
    case 's':
      _node_prefs->buzzer_quiet = !_node_prefs->buzzer_quiet;
      the_mesh.savePrefs();
      if (!_node_prefs->buzzer_quiet) notify(UIEventType::channelMessage);   // a sample
      break;
    case 't':
    case 'o':
      _set_edit = true;
      break;
    case 'a':
      _screen = DScreen::ABOUT;
      break;
  }
}

void UITask::adjustSetting(int item, int dir) {
  if (SET_ROWS[item].kind == 't') {
    int v = _ui.tz_halfhours + dir;
    if (v >= -24 && v <= 28) _ui.tz_halfhours = v;
  } else if (SET_ROWS[item].kind == 'o') {
    int v = _ui.timeout_idx + dir;
    if (v >= 0 && v < NUM_TIMEOUTS) _ui.timeout_idx = v;
  }
}

void UITask::keySettings(int key) {
  if (_set_edit) {
    if (key == KEY_UP) adjustSetting(_set_sel, 1);
    else if (key == KEY_DOWN) adjustSetting(_set_sel, -1);
    else if (key == KEY_CLICK || key == KEY_ENTER || key == KEY_LEFT || key == KEY_BACKSPACE) {
      _set_edit = false;
      saveUiPrefs();
      wake();   // new timeout
    }
    return;
  }
  if (key == KEY_UP) {
    for (int i = _set_sel - 1; i >= 0; i--) if (SET_ROWS[i].kind != 'H') { _set_sel = i; break; }
  } else if (key == KEY_DOWN) {
    for (int i = _set_sel + 1; i < NUM_SET_ROWS; i++) if (SET_ROWS[i].kind != 'H') { _set_sel = i; break; }
  } else if (key == KEY_CLICK || key == KEY_ENTER) {
    activateSetting(_set_sel);
  } else if (key == KEY_LEFT) {
    _screen = DScreen::CHATS;
  }
}

// ACTIONS: the app's long-press menu for the highlighted message
void UITask::openActions() {
  DMsg& m = _msgs[_sel_msg];
  DChat& c = _chats[_chat];
  _num_actions = 0;
  _action_name[0] = 0;
  if (c.type == DChatType::CHANNEL && m.status == MSG_IN) {
    const char* sep = strstr(m.text, ": ");
    int len = sep ? sep - m.text : 0;
    if (len > 0 && len < (int)sizeof(_action_name)) {
      memcpy(_action_name, m.text, len);
      _action_name[len] = 0;
      if (strcasecmp(_action_name, _node_prefs->node_name) != 0) {
        char name[32];
        const char* hex = parseControl(sep + 2, REQ_PREFIX, REQ_KEY, name, sizeof(name));
        uint8_t key[PUB_KEY_SIZE];
        if (hex && strcasecmp(name, _node_prefs->node_name) == 0 && readHex(hex, key, PUB_KEY_SIZE)
            && the_mesh.lookupContactByPubKey(key, PUB_KEY_SIZE) == NULL) {
          memcpy(_prompt_key, key, PUB_KEY_SIZE);
          _actions[_num_actions++] = ACT_ACCEPT;
        }
        _actions[_num_actions++] = ACT_DM;
        _actions[_num_actions++] = ACT_MENTION;
      }
    }
  }
  if (m.status == MSG_OUT_FAILED && c.type == DChatType::DIRECT) _actions[_num_actions++] = ACT_RESEND;
  if (m.status == MSG_IN) {
    const char* from = c.name;
    if (c.type == DChatType::CHANNEL && _action_name[0]) from = _action_name;
    if (offerJoin(m.text, from, false)) _actions[_num_actions++] = ACT_JOIN;
  }
  if (_num_actions == 0) return;   // nothing to do with this one
  _actions[_num_actions++] = ACT_CANCEL;
  _action_sel = 0;
  _screen = DScreen::ACTIONS;
}

void UITask::runAction(uint8_t act) {
  _screen = DScreen::CHAT;
  DChat& c = _chats[_chat];
  switch (act) {
    case ACT_DM:
      askDM(_action_name, c.ch_idx);
      break;
    case ACT_ACCEPT:
      showPrompt(DPrompt::INCOMING_REQUEST, _action_name, c.ch_idx);
      break;
    case ACT_JOIN:
      showPrompt(DPrompt::JOIN_CHANNEL, _join_name, 0);
      break;
    case ACT_MENTION: {
      char tag[40];
      snprintf(tag, sizeof(tag), "@[%s] ", _action_name);
      for (const char* p = tag; *p; p++) composeKey(*p);
      _sel_msg = -1;
      break;
    }
    case ACT_RESEND: {
      DMsg& m = _msgs[_sel_msg];
      ContactInfo* ci = the_mesh.lookupContactByPubKey(c.pub_key, PUB_KEY_SIZE);
      if (_out.active) showAlert("Still sending the last message", 2000);
      else if (ci) {
        char text[DECK_TEXT_MAX + 1];
        StrHelper::strncpy(text, m.text, sizeof(text));
        m.id = 0;   // replaced by the new copy
        messagesChanged();
        _sel_msg = -1;
        startDM(*ci, text);
        markActivity(_chat, false);
      }
      break;
    }
    default:
      break;
  }
}

void UITask::keyActions(int key) {
  if (key == KEY_UP) {
    if (_action_sel > 0) _action_sel--;
  } else if (key == KEY_DOWN) {
    if (_action_sel + 1 < _num_actions) _action_sel++;
  } else if (key == KEY_CLICK || key == KEY_ENTER) {
    runAction(_actions[_action_sel]);
  } else if (key == KEY_LEFT || key == KEY_BACKSPACE) {
    _screen = DScreen::CHAT;
  }
}

void UITask::keyChat(int key) {
  int slots[DECK_MAX_MSGS];
  int count = chatMessages(_chat, slots, DECK_MAX_MSGS);
  int pos = -1;
  for (int i = 0; i < count; i++) if (slots[i] == _sel_msg) pos = i;
  if (pos < 0) _sel_msg = -1;

  if (composeKey(key)) {
    return;
  } else if (key == KEY_ENTER || key == KEY_CLICK) {
    if (_sel_msg >= 0) openActions();
    else sendCompose();
  } else if (key == KEY_UP) {
    if (count == 0) return;
    if (pos < 0) _sel_msg = slots[count - 1];
    else if (pos > 0) _sel_msg = slots[pos - 1];
  } else if (key == KEY_DOWN) {
    if (pos >= 0) _sel_msg = pos + 1 < count ? slots[pos + 1] : -1;
  } else if (key == KEY_LEFT || key == KEY_BACKSPACE) {
    if (_sel_msg >= 0) _sel_msg = -1;
    else if (key == KEY_LEFT && _compose_len == 0) {
      _screen = DScreen::CHATS;
      int order[DECK_MAX_CHATS];
      int n = sortedChats(order);
      for (int i = 0; i < n; i++) if (order[i] == _chat) _list_sel = i;
    }
  }
}

void UITask::keyName(int key) {
  if (key == ':' || key == '[' || key == ']' || key == '@') return;   // would break "name: text" and @[name]
  if (composeKey(key)) return;
  if ((key == KEY_ENTER || key == KEY_CLICK) && _compose_len > 0) {
    StrHelper::strncpy(_node_prefs->node_name, _compose, sizeof(_node_prefs->node_name));
    the_mesh.savePrefs();
    _compose[0] = 0;
    _compose_len = 0;
    _screen = DScreen::CHATS;
    showAlert(_setup ? "All set. Say hello!" : "Name saved", 2000);
    _setup = false;
  } else if (key == KEY_LEFT && _setup) {
    openRegion(true);
  } else if (key == KEY_LEFT && !isDefaultName()) {
    _compose[0] = 0;
    _compose_len = 0;
    _screen = DScreen::SETTINGS;
  }
}

void UITask::keyPrompt(int key) {
  if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN) {
    _prompt_yes = !_prompt_yes;
    return;
  }
  bool yes, no;
  if (key == KEY_CLICK) { yes = _prompt_yes; no = !_prompt_yes; }
  else {
    yes = key == KEY_ENTER || key == 'y' || key == 'Y';
    no = key == KEY_BACKSPACE || key == 'n' || key == 'N';
  }
  if (!yes && !no) return;
  DPrompt p = _prompt;
  _prompt = DPrompt::NONE;
  _screen = _prompt_return;
  _sel_msg = -1;
  if (yes && p == DPrompt::CONFIRM_REQUEST) requestDM(_prompt_name, _prompt_ch_idx);
  if (yes && p == DPrompt::INCOMING_REQUEST) acceptDM();   // decline posts nothing, as the app
  if (yes && p == DPrompt::REMOVE_CHANNEL) removeChannel(_ch_slot);
  if (yes && p == DPrompt::CLEAR_MESSAGES) clearMessages();
  if (yes && p == DPrompt::JOIN_CHANNEL && joinChannel(_join_name, _join_secret) >= 0) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Joined %s", _join_name);
    showAlert(msg, 2500);
  }
  if (yes && p == DPrompt::SHARE_CHANNEL) shareChannel();
  if (yes && p == DPrompt::KEEP_OFF) {
    _ui.keep_msgs = 0;
    saveUiPrefs();
    clearMessages();
  }
}

void UITask::handleKey(int key) {
  _touch_scrolled = false;
  switch (_screen) {
    case DScreen::CHATS:    keyChats(key); break;
    case DScreen::SETTINGS: keySettings(key); break;
    case DScreen::CHAT:     keyChat(key); break;
    case DScreen::ACTIONS:  keyActions(key); break;
    case DScreen::NAME:     keyName(key); break;
    case DScreen::ABOUT:
      if (key == KEY_LEFT || key == KEY_CLICK || key == KEY_ENTER || key == KEY_BACKSPACE) _screen = DScreen::SETTINGS;
      break;
    case DScreen::PROMPT:   keyPrompt(key); break;
    case DScreen::REGION:   keyRegion(key); break;
    case DScreen::CHANNELS:     keyChannels(key); break;
    case DScreen::CHANNEL_INFO: keyChannelInfo(key); break;
    case DScreen::ADD_CHANNEL:  keyAddChannel(key); break;
    case DScreen::CHANNEL_FORM: keyChannelForm(key); break;
    case DScreen::SHARE_PICK:   keySharePick(key); break;
  }
  _dirty = true;
}


// ---------------------------------------------------------------- touch

void UITask::addHit(int x, int y, int w, int h, uint8_t type, uint8_t index) {
  if (_num_hits >= DECK_MAX_HITS || w <= 0 || h <= 0) return;
  _hits[_num_hits++] = { (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, type, index };
}

void UITask::handleTouch(int event) {
  bool was_off = _display && !_display->isOn();
  wake();
  if (was_off) return;   // the first touch only wakes the screen
  if (event == TOUCH_TAP) tapAt(_touch.x, _touch.y);
  else if (event == TOUCH_DRAG) dragBy(_touch.dy);
  _dirty = true;
}

void UITask::tapAt(int x, int y) {
  const DHit* hit = NULL;
  for (int i = _num_hits - 1; i >= 0 && hit == NULL; i--) {   // topmost first
    const DHit& h = _hits[i];
    if (x < h.x || x >= h.x + h.w || y < h.y || y >= h.y + h.h) continue;
    // a popup takes every tap: only its own targets count
    if (_screen == DScreen::ACTIONS && h.type != HIT_ACTION) continue;
    if (_screen == DScreen::PROMPT && h.type != HIT_YES && h.type != HIT_NO) continue;
    hit = &h;
  }
  if (hit == NULL) {
    if (_screen == DScreen::ACTIONS) _screen = DScreen::CHAT;   // tap outside: dismiss
    else if (_screen == DScreen::CHAT) _sel_msg = -1;
    return;
  }
  switch (hit->type) {
    case HIT_TAB:
      _set_edit = false;
      _screen = hit->index == 0 ? DScreen::CHATS : DScreen::SETTINGS;
      break;
    case HIT_BACK:
      if (_screen == DScreen::CHAT) {
        _sel_msg = -1;
        _compose[0] = 0;
        _compose_len = 0;
        handleKey(KEY_LEFT);
      } else {
        handleKey(KEY_LEFT);
      }
      break;
    case HIT_SEND:
      _sel_msg = -1;
      sendCompose();
      break;
    case HIT_CHAT_ROW: {
      int order[DECK_MAX_CHATS];
      int n = sortedChats(order);
      if (hit->index < n) {
        _list_sel = hit->index;
        openChat(order[hit->index]);
      }
      break;
    }
    case HIT_MSG:
      _sel_msg = hit->index;
      openActions();   // the app's long-press menu; stays highlighted if it has none
      break;
    case HIT_ACTION:
      runAction(_actions[hit->index]);
      break;
    case HIT_YES:
    case HIT_NO:
      if (_screen == DScreen::PROMPT) {
        _prompt_yes = hit->type == HIT_YES;
        keyPrompt(KEY_CLICK);
      } else if (_screen == DScreen::NAME) {
        keyName(KEY_ENTER);
      }
      break;
    case HIT_SET_ROW:
      if (_set_edit) {
        bool same = hit->index == _set_sel;
        _set_edit = false;
        saveUiPrefs();
        wake();
        if (same) break;
      }
      _set_sel = hit->index;
      activateSetting(_set_sel);
      break;
    case HIT_REGION_ROW:
      _region_sel = hit->index;
      chooseRegionRow(hit->index);
      break;
    case HIT_LIST_ROW:
      _cl_sel = hit->index;
      if (_screen == DScreen::CHANNELS) keyChannels(KEY_CLICK);
      else if (_screen == DScreen::ADD_CHANNEL) keyAddChannel(KEY_CLICK);
      else if (_screen == DScreen::SHARE_PICK) { _sp_sel = hit->index; keySharePick(KEY_CLICK); }
      break;
    case HIT_FIELD:
      _form_field = hit->index;
      break;
    case HIT_BUTTON:
      if (_screen == DScreen::CHANNEL_FORM) submitForm();
      else if (_screen == DScreen::CHANNEL_INFO) {
        _info_sel = hit->index;
        keyChannelInfo(KEY_CLICK);
      }
      break;
  }
}

// Finger moving up scrolls the content up (shows what's below), as on a phone.
void UITask::dragBy(int dy) {
  if (_screen == DScreen::CHAT) {
    _sel_msg = -1;
    _scroll_px += dy;   // finger down: older messages come into view
    return;
  }
  if (_screen == DScreen::SETTINGS && _set_edit) {   // adjusting a value: up = more
    _drag_acc += dy;
    while (_drag_acc <= -18 || _drag_acc >= 18) {
      bool down = _drag_acc > 0;
      _drag_acc += down ? -18 : 18;
      adjustSetting(_set_sel, down ? -1 : 1);
    }
    return;
  }
  // lists move with the finger, as on a phone (finger up: the list goes up)
  _touch_scrolled = true;
  if (_screen == DScreen::SETTINGS) _set_top -= dy;
  else if (_screen == DScreen::REGION) _region_top -= dy;
  else if (_screen == DScreen::CHATS || _screen == DScreen::CHANNELS || _screen == DScreen::SHARE_PICK) {
    int& top = _screen == DScreen::CHATS ? _list_top : _screen == DScreen::SHARE_PICK ? _sp_top : _cl_top;
    _drag_acc += dy;
    while (_drag_acc <= -ROW_H / 2 || _drag_acc >= ROW_H / 2) {
      bool down = _drag_acc > 0;
      _drag_acc += down ? -ROW_H / 2 : ROW_H / 2;
      top += down ? -1 : 1;   // whole rows; half a row of finger per row feels right
    }
  }
}

// ---------------------------------------------------------------- region picker

void UITask::openRegion(bool setup) {
  _setup = setup;
  _num_region_rows = 0;
  _region_rows[_num_region_rows++] = -1;   // keep the current settings
  int current = matchPreset(_node_prefs->freq, _node_prefs->bw, _node_prefs->sf, _node_prefs->cr);
  int sel = -1, usa = -1;
  for (int g = 0; g < NUM_PRESET_GROUPS; g++) {
    _region_rows[_num_region_rows++] = -2 - g;
    for (int i = 0; i < NUM_PRESETS && _num_region_rows < DECK_REGION_ROWS; i++) {
      if (PRESETS[i].group != g) continue;
      if (i == current) sel = _num_region_rows;
      if (i == 0) usa = _num_region_rows;
      _region_rows[_num_region_rows++] = i;
    }
  }
  // highlight where they are now; on a first start, the first region instead
  // of an unknown default
  _region_sel = sel >= 0 ? sel : setup ? usa : 0;
  _region_top = 0;
  _screen = DScreen::REGION;
  _dirty = true;
}

void UITask::chooseRegionRow(int row) {
  if (row < 0 || row >= _num_region_rows) return;
  int v = _region_rows[row];
  if (v <= -2) return;   // a group header
  if (v >= 0) {
    const DeckPreset& p = PRESETS[v];
    int tx = p.tx;
#ifdef LORA_TX_POWER
    if (tx > LORA_TX_POWER) tx = LORA_TX_POWER;
#endif
    _node_prefs->freq = p.freq;
    _node_prefs->bw = p.bw;
    _node_prefs->sf = p.sf;
    _node_prefs->cr = p.cr;
    _node_prefs->tx_power_dbm = tx;
    the_mesh.savePrefs();
    radio_driver.setParams(_node_prefs->freq, _node_prefs->bw, _node_prefs->sf, _node_prefs->cr);
    radio_driver.setTxPower(_node_prefs->tx_power_dbm);
    char msg[64];
    snprintf(msg, sizeof(msg), "Radio set to %s", p.name);
    showAlert(msg, 2500);
  }
  if (_setup) {
    _compose[0] = 0;
    _compose_len = 0;
    _screen = DScreen::NAME;
  } else {
    _screen = DScreen::SETTINGS;
  }
}

void UITask::keyRegion(int key) {
  if (key == KEY_UP) {
    for (int i = _region_sel - 1; i >= 0; i--) if (_region_rows[i] > -2) { _region_sel = i; break; }
  } else if (key == KEY_DOWN) {
    for (int i = _region_sel + 1; i < _num_region_rows; i++) if (_region_rows[i] > -2) { _region_sel = i; break; }
  } else if (key == KEY_CLICK || key == KEY_ENTER) {
    chooseRegionRow(_region_sel);
  } else if ((key == KEY_LEFT || key == KEY_BACKSPACE) && !_setup) {
    _screen = DScreen::SETTINGS;
  }
}

void UITask::drawRegion() {
  const int top = BAR_H, bottom = DECK_H;
  const int CAVEAT_H = 40;
  int current = matchPreset(_node_prefs->freq, _node_prefs->bw, _node_prefs->sf, _node_prefs->cr);

  // row positions below the caveat; keep the highlighted row in view
  static int ys[DECK_REGION_ROWS];
  int total = CAVEAT_H;
  for (int i = 0; i < _num_region_rows; i++) {
    ys[i] = total;
    total += _region_rows[i] <= -2 ? HDR_H + 4 : ROW_H;
  }
  int sel_y = ys[_region_sel];
  if (_touch_scrolled) {
    int max_top = total - (bottom - top);
    if (_region_top > max_top) _region_top = max_top;
    if (_region_top < 0) _region_top = 0;
    if (sel_y < _region_top || sel_y + ROW_H > _region_top + (bottom - top)) {
      for (int i = 0; i < _num_region_rows; i++) {
        if (_region_rows[i] <= -2 || ys[i] < _region_top) continue;
        if (ys[i] + ROW_H <= _region_top + (bottom - top)) { _region_sel = i; break; }
      }
    }
  } else {
    if (sel_y - HDR_H - 4 < _region_top) _region_top = sel_y - HDR_H - 4;
    if (_region_sel <= 1) _region_top = 0;
    if (sel_y + ROW_H > _region_top + (bottom - top)) _region_top = sel_y + ROW_H - (bottom - top);
    if (_region_top < 0) _region_top = 0;
  }

  if (-_region_top + CAVEAT_H > 0) {
    char lines[3][64];
    int n = wrapText(FONT_SMALL, PRESET_CAVEAT, DECK_W - 28, lines, 3);
    for (int i = 0; i < n; i++) drawText(G, FONT_SMALL, 14, top + 6 + i * 11 - _region_top, C_MUTED, lines[i]);
  }
  for (int i = 0; i < _num_region_rows; i++) {
    int y = top + ys[i] - _region_top;
    if (y + ROW_H < top || y >= bottom) continue;
    int v = _region_rows[i];
    if (v <= -2) {
      drawText(G, FONT_SMALL, 14, y + 8, C_PRIMARY, PRESET_GROUPS[-2 - v]);
      continue;
    }
    addHit(0, y < top ? top : y, DECK_W, ROW_H - (y < top ? top - y : 0), HIT_REGION_ROW, i);
    bool sel = i == _region_sel;
    if (sel) {
      G->fillRect(0, y, DECK_W, ROW_H, C_IN_BUBBLE);
      G->fillRect(0, y + 4, 3, ROW_H - 8, C_PRIMARY);
    }
    char title[48], sub[72];
    if (v == -1) {
      strcpy(title, _setup ? "Keep the current settings" : "Keep current settings");
      snprintf(sub, sizeof(sub), "%.3f MHz - %g kHz - SF%d - CR%d", _node_prefs->freq, _node_prefs->bw, _node_prefs->sf, _node_prefs->cr);
    } else {
      const DeckPreset& p = PRESETS[v];
      StrHelper::strncpy(title, p.name, sizeof(title));
      snprintf(sub, sizeof(sub), "%.3f MHz - %g kHz - SF%d - CR%d - %d dBm", p.freq, p.bw, p.sf, p.cr, p.tx);
    }
    drawText(G, FONT, 14, y + 1, C_TEXT, title);
    drawText(G, FONT_SMALL, 14, y + 22, C_MUTED, sub);
    if (v >= 0 && v == current) drawTextRight(G, FONT_SMALL, DECK_W - 10, y + 6, C_PRIMARY, "Current");
  }
  // drawn last so the list scrolls under it
  if (_setup) drawAppBar("Welcome", "Step 1 of 2 - choose your region", false, -1);
  else drawAppBar("Radio", "Choose your region", true, -1);
}


// ---------------------------------------------------------------- channels
//
// The app's channel management: list, add (its four choices, same wording and
// input rules as shared/.../presentation/ChannelSetup.kt), show the key, remove.

static const char* ADD_TITLES[] = { "Join the Public Channel", "Create a Private Channel", "Join a Private Channel", "Join a Hashtag Channel" };
static const char* ADD_SUBS[] = { "Anyone can join this channel.", "Only people you give the key to can join.",
                                  "Enter a secret key someone gave you.", "Anyone can join hashtag channels." };
static const char* ADD_EXPLAIN[] = {
  "The Public channel uses a key every MeshCore radio knows. Anyone in range can read and post in it.",
  "A new random key is made on this device. Only people you give it to can join. Its key is shown once it's created.",
  "The name can be anything you like - it stays on this device. Only the secret key must match.",
  "Hashtag channels are public. Anyone can join by entering the same name, because the key is made from the name. Only a-z, 0-9 and hyphens.",
};
static const char* CHANNEL_CAVEAT = "Channels are obfuscated, not secure (AES-ECB with a 2-byte MAC). Don't send anything you couldn't bear to be read.";

static uint8_t kindOf(const ChannelDetails& cd) {
  if (memcmp(cd.channel.secret, PUBLIC_PSK, sizeof(PUBLIC_PSK)) == 0) return CHAN_PUBLIC;
  return cd.name[0] == '#' ? CHAN_HASHTAG : CHAN_PRIVATE;
}

static const char* kindLabel(uint8_t kind) {
  return kind == CHAN_PUBLIC ? "Public - anyone can read it" : kind == CHAN_HASHTAG ? "Hashtag - anyone can read it" : "Private - not secure";
}

int UITask::channelSlots(int* slots) {
  int n = 0;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails cd;
    if (the_mesh.getChannel(i, cd) && cd.name[0]) slots[n++] = i;
  }
  return n;
}

void UITask::openChannels() {
  _cl_sel = 0;
  _cl_top = 0;
  _screen = DScreen::CHANNELS;
}

void UITask::keyChannels(int key) {
  int slots[MAX_GROUP_CHANNELS];
  int rows = channelSlots(slots) + 1;   // row 0: "Add a channel"
  if (key == KEY_UP) {
    if (_cl_sel > 0) _cl_sel--;
  } else if (key == KEY_DOWN) {
    if (_cl_sel + 1 < rows) _cl_sel++;
  } else if (key == KEY_CLICK || key == KEY_ENTER) {
    if (_cl_sel == 0) {
      _cl_sel = 0;
      _screen = DScreen::ADD_CHANNEL;
    } else if (_cl_sel < rows) {
      _ch_slot = slots[_cl_sel - 1];
      _info_sel = 0;
      _screen = DScreen::CHANNEL_INFO;
    }
  } else if (key == KEY_LEFT || key == KEY_BACKSPACE) {
    _screen = DScreen::SETTINGS;
  }
}

void UITask::keyAddChannel(int key) {
  if (key == KEY_UP) {
    if (_cl_sel > 0) _cl_sel--;
  } else if (key == KEY_DOWN) {
    if (_cl_sel < 3) _cl_sel++;
  } else if (key == KEY_CLICK || key == KEY_ENTER) {
    openForm(_cl_sel);
  } else if (key == KEY_LEFT || key == KEY_BACKSPACE) {
    openChannels();
  }
}

void UITask::openForm(uint8_t choice) {
  _add_choice = choice;
  _form_name[0] = 0;
  _form_key[0] = 0;
  _form_field = choice == ADD_PUBLIC ? -1 : 0;
  _screen = DScreen::CHANNEL_FORM;
}

void UITask::keyChannelForm(int key) {
  if (key == KEY_LEFT) {
    _cl_sel = _add_choice;
    _screen = DScreen::ADD_CHANNEL;
    return;
  }
  if (_form_field >= 0) {
    char* f = _form_field == 0 ? _form_name : _form_key;
    int max = _form_field == 0 ? 31 : 39;
    int len = strlen(f);
    if (key == KEY_BACKSPACE) {
      if (len > 0) f[len - 1] = 0;
      else if (_form_field == 1) _form_field = 0;
      return;
    }
    if (key >= 0x20 && key < 0x7F) {
      char c = _add_choice == ADD_HASHTAG ? tolower(key) : key;   // the key is the hash of the exact text
      if (len < max) { f[len] = c; f[len + 1] = 0; }
      return;
    }
  }
  if ((key == KEY_UP || key == KEY_DOWN) && _add_choice == ADD_JOIN_PRIVATE) {
    _form_field = _form_field == 0 ? 1 : 0;
  } else if (key == KEY_ENTER || key == KEY_CLICK) {
    if (_add_choice == ADD_JOIN_PRIVATE && _form_field == 0) _form_field = 1;
    else submitForm();
  }
}

// Adds a channel in the first free slot; returns the slot, or -1 (alert shown).
int UITask::joinChannel(const char* name, const uint8_t* secret) {
  mesh::GroupChannel probe;
  memset(&probe, 0, sizeof(probe));
  memcpy(probe.secret, secret, 16);
  int existing = the_mesh.findChannelIdx(probe);
  ChannelDetails cd;
  if (existing >= 0 && the_mesh.getChannel(existing, cd) && cd.name[0]) {
    char msg[64];
    snprintf(msg, sizeof(msg), "You're already in that channel (%s)", cd.name);
    showAlert(msg, 3000);
    return -1;
  }
  int slot = -1;
  for (int i = 0; i < MAX_GROUP_CHANNELS && slot < 0; i++) {
    if (!the_mesh.getChannel(i, cd) || cd.name[0] == 0) slot = i;
  }
  if (slot < 0) {
    showAlert("No free channel slots - remove one first", 3000);
    return -1;
  }
  memset(&cd, 0, sizeof(cd));
  StrHelper::strncpy(cd.name, name, sizeof(cd.name));
  memcpy(cd.channel.secret, secret, 16);   // 128-bit key, as the app
  if (!the_mesh.setChannel(slot, cd)) {
    showAlert("Couldn't add the channel", 2500);
    return -1;
  }
  the_mesh.persistChannels();
  rebuildChats();
  return slot;
}

void UITask::submitForm() {
  char name[32];
  uint8_t secret[16];
  char msg[80];
  if (_add_choice == ADD_PUBLIC) {
    strcpy(name, "Public");
    memcpy(secret, PUBLIC_PSK, 16);
  } else if (_add_choice == ADD_HASHTAG) {
    const char* t = _form_name;
    while (*t == ' ' || *t == '#') t++;
    char tag[32];
    StrHelper::strncpy(tag, t, sizeof(tag));
    for (int i = strlen(tag) - 1; i >= 0 && tag[i] == ' '; i--) tag[i] = 0;
    if (tag[0] == 0) { showAlert("Enter the channel's name.", 2500); _form_field = 0; return; }
    for (char* c = tag; *c; c++) {
      if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) {
        showAlert("Only a-z, 0-9 and hyphens.", 2500);
        return;
      }
    }
    if (strlen(tag) + 1 > 31) { showAlert("Too long - at most 30 characters.", 2500); return; }
    snprintf(name, sizeof(name), "#%s", tag);
    uint8_t hash[32];
    mesh::Utils::sha256(hash, sizeof(hash), (const uint8_t*)name, strlen(name));   // SHA256("#name")[0..15]
    memcpy(secret, hash, 16);
  } else {
    const char* t = _form_name;
    while (*t == ' ') t++;
    StrHelper::strncpy(name, t, sizeof(name));
    for (int i = strlen(name) - 1; i >= 0 && name[i] == ' '; i--) name[i] = 0;
    if (name[0] == 0) { showAlert("Enter a name.", 2500); _form_field = 0; return; }
    if (name[0] == '#') { showAlert("A name starting with # is for hashtag channels.", 3000); _form_field = 0; return; }
    if (_add_choice == ADD_CREATE_PRIVATE) {
      do { esp_fill_random(secret, sizeof(secret)); } while (memcmp(secret, PUBLIC_PSK, 16) == 0);
    } else {
      char hex[40];
      int n = 0;
      for (const char* c = _form_key; *c && n < 39; c++) if (*c != ' ') hex[n++] = *c;
      hex[n] = 0;
      if (n == 0) { showAlert("Enter the secret key.", 2500); _form_field = 1; return; }
      if (n != 32) {
        snprintf(msg, sizeof(msg), "The key is 32 hex characters (%d entered).", n);
        showAlert(msg, 3000);
        _form_field = 1;
        return;
      }
      if (!readHex(hex, secret, 16)) { showAlert("Only 0-9 and a-f.", 2500); _form_field = 1; return; }
      bool zero = true;
      for (int i = 0; i < 16; i++) zero &= secret[i] == 0;
      if (zero) { showAlert("A key can't be all zeros.", 2500); _form_field = 1; return; }
    }
  }
  int slot = joinChannel(name, secret);
  if (slot < 0) return;
  if (_add_choice == ADD_CREATE_PRIVATE) {   // show the key so it can be shared
    _ch_slot = slot;
    _info_sel = 0;
    _screen = DScreen::CHANNEL_INFO;
    showAlert("Created. Share its key with the people you want in it.", 3500);
  } else {
    snprintf(msg, sizeof(msg), "Joined %s", name);
    showAlert(msg, 2500);
    int chat = findChannelChat(slot);
    if (chat >= 0) openChat(chat);
    else openChannels();
  }
}

// ---------------------------------------------------------------- sharing a channel by DM
//
// Sends the channel as the app's share link (ShareUri.encodeChannel: name
// percent-encoded, key as upper-case hex) in a DM, end-to-end encrypted to
// that one contact. The app shows it as a tappable link; another T-Deck
// offers to join it.

int UITask::dmChats(int* out) {
  int n = 0;
  for (int i = 0; i < _num_chats; i++) if (_chats[i].type == DChatType::DIRECT) out[n++] = i;
  return n;
}

void UITask::keySharePick(int key) {
  int dms[DECK_MAX_CHATS];
  int n = dmChats(dms);
  if (key == KEY_UP) {
    if (_sp_sel > 0) _sp_sel--;
  } else if (key == KEY_DOWN) {
    if (_sp_sel + 1 < n) _sp_sel++;
  } else if ((key == KEY_CLICK || key == KEY_ENTER) && _sp_sel < n) {
    _share_chat = dms[_sp_sel];
    showPrompt(DPrompt::SHARE_CHANNEL, _chats[_share_chat].name, _ch_slot);
  } else if (key == KEY_LEFT || key == KEY_BACKSPACE) {
    _screen = DScreen::CHANNEL_INFO;
  }
}

void UITask::shareChannel() {
  ChannelDetails cd;
  if (_share_chat < 0 || _share_chat >= _num_chats || !the_mesh.getChannel(_ch_slot, cd) || cd.name[0] == 0) return;
  ContactInfo* ci = the_mesh.lookupContactByPubKey(_chats[_share_chat].pub_key, PUB_KEY_SIZE);
  if (ci == NULL) { showAlert("That contact is gone", 2000); return; }
  if (_out.active) { showAlert("Still sending the last message - try again in a moment", 2500); return; }
  static const char HEXU[] = "0123456789ABCDEF";
  char link[DECK_TEXT_MAX + 1];
  int n = snprintf(link, sizeof(link), "meshcore://channel/add?name=");
  for (const uint8_t* p = (const uint8_t*)cd.name; *p && n + 4 < (int)sizeof(link); p++) {
    if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') link[n++] = *p;
    else { link[n++] = '%'; link[n++] = HEXU[*p >> 4]; link[n++] = HEXU[*p & 15]; }
  }
  link[n] = 0;
  char hex[33];
  for (int i = 0; i < 16; i++) { hex[i * 2] = HEXU[cd.channel.secret[i] >> 4]; hex[i * 2 + 1] = HEXU[cd.channel.secret[i] & 15]; }
  hex[32] = 0;
  int total = n + 8 + 32;
  if (total > DECK_COMPOSE_MAX) { showAlert("That channel's name is too long to share in a message", 3000); return; }
  snprintf(link + n, sizeof(link) - n, "&secret=%s", hex);
  int chat = _share_chat;
  openChat(chat);
  startDM(*ci, link);
  markActivity(chat, false);
}

void UITask::drawSharePick() {
  int dms[DECK_MAX_CHATS];
  int n = dmChats(dms);
  const int top = BAR_H, rows = (DECK_H - BAR_H) / ROW_H;
  if (_sp_sel >= n) _sp_sel = n > 0 ? n - 1 : 0;
  if (_touch_scrolled) {
    if (_sp_top > n - rows) _sp_top = n - rows;
    if (_sp_top < 0) _sp_top = 0;
    if (_sp_sel < _sp_top) _sp_sel = _sp_top;
    if (_sp_sel >= _sp_top + rows) _sp_sel = _sp_top + rows - 1;
  } else {
    if (_sp_sel < _sp_top) _sp_top = _sp_sel;
    if (_sp_sel >= _sp_top + rows) _sp_top = _sp_sel - rows + 1;
  }
  if (n == 0) {
    char lines[4][64];
    int k = wrapText(FONT, "No one to send it to yet. Set up a direct chat first: tap someone's message in a channel and choose \"Message privately\".", DECK_W - 40, lines, 4);
    for (int i = 0; i < k; i++) drawText(G, FONT, 20, top + 30 + i * LINE_H, C_MUTED, lines[i]);
  }
  for (int r = 0; r < rows && _sp_top + r < n; r++) {
    int row = _sp_top + r, y = top + r * ROW_H;
    addHit(0, y, DECK_W, ROW_H, HIT_LIST_ROW, row);
    if (row == _sp_sel) {
      G->fillRect(0, y, DECK_W, ROW_H, C_IN_BUBBLE);
      G->fillRect(0, y + 4, 3, ROW_H - 8, C_PRIMARY);
    }
    drawChatAvatar(dms[row], 22, y + ROW_H / 2, 14);
    char nm[32], buf[48];
    toAscii(nm, _chats[dms[row]].name, sizeof(nm));
    ellipsize(FONT, nm, DECK_W - 54, buf, sizeof(buf));
    drawText(G, FONT, 44, y + 1, C_TEXT, buf);
    drawText(G, FONT_SMALL, 44, y + 22, C_MUTED, "Direct message");
  }
  drawAppBar("Share with", "Sent as a direct message", true, -1);
}

void UITask::removeChannel(int slot) {
  ChannelDetails cd;
  char name[32] = "";
  if (the_mesh.getChannel(slot, cd)) StrHelper::strncpy(name, cd.name, sizeof(name));
  memset(&cd, 0, sizeof(cd));   // an empty name and a zero key mark a free slot
  if (the_mesh.setChannel(slot, cd)) {
    the_mesh.persistChannels();
    if (_chat >= 0 && _chats[_chat].type == DChatType::CHANNEL && _chats[_chat].ch_idx == slot) _chat = -1;
    rebuildChats();
    char msg[64];
    snprintf(msg, sizeof(msg), "Removed %s", name);
    showAlert(msg, 2500);
  }
  openChannels();
}

void UITask::keyChannelInfo(int key) {
  if (key == KEY_UP || key == KEY_DOWN || key == KEY_RIGHT) {
    _info_sel = key == KEY_UP ? (_info_sel + 2) % 3 : (_info_sel + 1) % 3;
  } else if (key == KEY_CLICK || key == KEY_ENTER) {
    ChannelDetails cd;
    if (!the_mesh.getChannel(_ch_slot, cd) || cd.name[0] == 0) { openChannels(); return; }
    if (_info_sel == 0) {
      int chat = findChannelChat(_ch_slot);
      if (chat < 0) { rebuildChats(); chat = findChannelChat(_ch_slot); }
      if (chat >= 0) openChat(chat);
    } else if (_info_sel == 1) {
      _sp_sel = _sp_top = 0;
      _screen = DScreen::SHARE_PICK;
    } else {
      showPrompt(DPrompt::REMOVE_CHANNEL, cd.name, _ch_slot);
    }
  } else if (key == KEY_LEFT || key == KEY_BACKSPACE) {
    openChannels();
  }
}

void UITask::drawButton(int x, int y, const char* label, bool primary, bool focused, uint8_t index, bool danger) {
  int w = textWidth(FONT_BOLD, label) + 32;
  if (primary) G->fillRoundRect(x, y, w, 30, 15, C_PRIMARY);
  else G->drawRoundRect(x, y, w, 30, 15, C_OUTLINE);
  if (focused) G->drawRoundRect(x - 3, y - 3, w + 6, 36, 18, C_TEXT);
  drawText(G, FONT_BOLD, x + 16, y + 7, primary ? C_ON_PRIMARY : danger ? C_ERROR : C_PRIMARY, label);
  addHit(x - 4, y - 4, w + 8, 38, HIT_BUTTON, index);
}

void UITask::drawChannels() {
  int slots[MAX_GROUP_CHANNELS];
  int n = channelSlots(slots), rows_total = n + 1;
  const int top = BAR_H, rows = (DECK_H - BAR_H) / ROW_H;
  if (_cl_sel >= rows_total) _cl_sel = rows_total - 1;
  if (_touch_scrolled) {
    if (_cl_top > rows_total - rows) _cl_top = rows_total - rows;
    if (_cl_top < 0) _cl_top = 0;
    if (_cl_sel < _cl_top) _cl_sel = _cl_top;
    if (_cl_sel >= _cl_top + rows) _cl_sel = _cl_top + rows - 1;
  } else {
    if (_cl_sel < _cl_top) _cl_top = _cl_sel;
    if (_cl_sel >= _cl_top + rows) _cl_top = _cl_sel - rows + 1;
  }
  for (int r = 0; r < rows && _cl_top + r < rows_total; r++) {
    int row = _cl_top + r, y = top + r * ROW_H;
    addHit(0, y, DECK_W, ROW_H, HIT_LIST_ROW, row);
    if (row == _cl_sel) {
      G->fillRect(0, y, DECK_W, ROW_H, C_IN_BUBBLE);
      G->fillRect(0, y + 4, 3, ROW_H - 8, C_PRIMARY);
    }
    if (row == 0) {
      G->fillCircle(22, y + ROW_H / 2, 14, C_PRIMARY);
      G->fillRect(21, y + ROW_H / 2 - 7, 3, 15, C_ON_PRIMARY);
      G->fillRect(15, y + ROW_H / 2 - 1, 15, 3, C_ON_PRIMARY);
      drawText(G, FONT_BOLD, 44, y + 1, C_TEXT, "Add a channel");
      drawText(G, FONT_SMALL, 44, y + 22, C_MUTED, "Public, private or hashtag");
      continue;
    }
    ChannelDetails cd;
    if (!the_mesh.getChannel(slots[row - 1], cd)) continue;
    int chat = findChannelChat(slots[row - 1]);
    if (chat >= 0) drawChatAvatar(chat, 22, y + ROW_H / 2, 14);
    char buf[48];
    ellipsize(FONT, cd.name, DECK_W - 54, buf, sizeof(buf));
    drawText(G, FONT, 44, y + 1, C_TEXT, buf);
    drawText(G, FONT_SMALL, 44, y + 22, C_MUTED, kindLabel(kindOf(cd)));
  }
  drawAppBar("Channels", "Shared-key groups", true, -1);
}

void UITask::drawAddChannel() {
  for (int i = 0; i < 4; i++) {
    int y = BAR_H + i * ROW_H;
    addHit(0, y, DECK_W, ROW_H, HIT_LIST_ROW, i);
    if (i == _cl_sel) {
      G->fillRect(0, y, DECK_W, ROW_H, C_IN_BUBBLE);
      G->fillRect(0, y + 4, 3, ROW_H - 8, C_PRIMARY);
    }
    drawText(G, FONT, 14, y + 1, C_TEXT, ADD_TITLES[i]);
    drawText(G, FONT_SMALL, 14, y + 22, C_MUTED, ADD_SUBS[i]);
  }
  char lines[3][64];
  int n = wrapText(FONT_SMALL, CHANNEL_CAVEAT, DECK_W - 28, lines, 3);
  for (int i = 0; i < n; i++) drawText(G, FONT_SMALL, 14, BAR_H + 4 * ROW_H + 12 + i * 11, C_MUTED, lines[i]);
  drawAppBar("Add a channel", NULL, true, -1);
}

void UITask::drawChannelForm() {
  drawAppBar(ADD_TITLES[_add_choice], "Add a channel", true, -1);
  char lines[4][64];
  int n = wrapText(FONT_SMALL, ADD_EXPLAIN[_add_choice], DECK_W - 28, lines, 4);
  int y = BAR_H + 6;
  for (int i = 0; i < n; i++) { drawText(G, FONT_SMALL, 14, y, C_MUTED, lines[i]); y += 11; }
  y += 8;
  int fields = _add_choice == ADD_PUBLIC ? 0 : _add_choice == ADD_JOIN_PRIVATE ? 2 : 1;
  for (int f = 0; f < fields; f++) {
    const char* label = f == 1 ? "Secret key (32 hex characters)" : _add_choice == ADD_HASHTAG ? "Channel name" : "Name";
    drawText(G, FONT_SMALL, 14, y, f == _form_field ? C_PRIMARY : C_MUTED, label);
    y += 11;
    bool focus = f == _form_field;
    G->drawRoundRect(10, y, 300, 28, 8, focus ? C_PRIMARY : C_OUTLINE);
    addHit(10, y, 300, 28, HIT_FIELD, f);
    const char* text = f == 0 ? _form_name : _form_key;
    int tx = 20;
    if (f == 0 && _add_choice == ADD_HASHTAG) {   // the stored label is "#name"
      drawText(G, FONT, tx, y + 6, C_MUTED, "#");
      tx += textWidth(FONT, "#") + 1;
    }
    const GFXfont* ff = f == 1 ? FONT_SMALL : FONT;
    const char* t = text;
    while (textWidth(ff, t) > 300 - (tx - 10) - 14) t++;
    drawText(G, ff, tx, f == 1 ? y + 10 : y + 6, C_TEXT, t);
    if (focus && (millis() / 500) % 2 == 0) G->drawFastVLine(tx + textWidth(ff, t) + 1, y + 6, 16, C_PRIMARY);
    y += 34;
  }
  const char* label = _add_choice == ADD_CREATE_PRIVATE ? "Create" : "Join";
  drawButton(DECK_W - 14 - (textWidth(FONT_BOLD, label) + 32), DECK_H - 40, label, true, false, 0);
}

void UITask::drawChannelInfo() {
  ChannelDetails cd;
  if (!the_mesh.getChannel(_ch_slot, cd) || cd.name[0] == 0) {
    drawAppBar("Channel", NULL, true, -1);
    return;
  }
  uint8_t kind = kindOf(cd);
  int y = BAR_H + 8;
  drawText(G, FONT_SMALL, 14, y, C_MUTED, kind == CHAN_PRIVATE ? "Secret key - share it only with people you want in it" : "Key - anyone can work it out");
  y += 14;
  char hex[33];
  toLowerHex(hex, cd.channel.secret, 16);
  char l1[24], l2[24];
  snprintf(l1, sizeof(l1), "%.4s %.4s %.4s %.4s", hex, hex + 4, hex + 8, hex + 12);
  snprintf(l2, sizeof(l2), "%.4s %.4s %.4s %.4s", hex + 16, hex + 20, hex + 24, hex + 28);
  G->fillRoundRect(10, y, 300, 46, 8, C_IN_BUBBLE);
  drawText(G, FONT, 20, y + 4, C_TEXT, l1);
  drawText(G, FONT, 20, y + 24, C_TEXT, l2);
  y += 54;
  char line[48];
  snprintf(line, sizeof(line), "Slot %d on this radio", _ch_slot);
  drawText(G, FONT_SMALL, 14, y, C_MUTED, line);
  y += 12;
  char lines[3][64];
  int n = wrapText(FONT_SMALL, CHANNEL_CAVEAT, DECK_W - 28, lines, 3);
  for (int i = 0; i < n; i++) drawText(G, FONT_SMALL, 14, y + i * 11, C_MUTED, lines[i]);
  int by = DECK_H - 40;
  drawButton(14, by, "Open", true, _info_sel == 0, 0);
  drawButton(14 + textWidth(FONT_BOLD, "Open") + 32 + 10, by, "Share", false, _info_sel == 1, 1);
  int rw = textWidth(FONT_BOLD, "Remove") + 32;
  drawButton(DECK_W - 14 - rw, by, "Remove", false, _info_sel == 2, 2, true);
  drawAppBar(cd.name, kindLabel(kind), true, -1);
}

// ---------------------------------------------------------------- drawing


void UITask::drawBattery(int right_x, int y) {
  uint16_t mv = getBattMilliVolts();
  int pct = mv <= 3300 ? 0 : mv >= 4150 ? 100 : (mv - 3300) * 100 / 850;
  uint16_t c = pct < 20 ? C_ERROR : C_MUTED;
  int x = right_x - 20;
  G->drawRect(x, y, 18, 10, c);
  G->fillRect(x + 18, y + 3, 2, 4, c);
  int w = 14 * pct / 100;
  if (w > 0) G->fillRect(x + 2, y + 2, w, 6, c);
}

void UITask::drawChatAvatar(int chat, int cx, int cy, int r) {
  DChat& c = _chats[chat];
  char seed[64];
  if (c.type == DChatType::DIRECT) {
    toLowerHex(seed, c.pub_key, 8);
  } else {
    snprintf(seed, sizeof(seed), "channel|%d|%s", c.ch_idx, c.name);
  }
  uint32_t rgb = avatarRgb(seed);
  uint16_t bg = rgb565(rgb), fg = isLight(rgb) ? 0x0000 : 0xFFFF;
  G->fillCircle(cx, cy, r, bg);
  if (c.type == DChatType::CHANNEL && c.kind == CHAN_PUBLIC) {   // globe
    G->drawCircle(cx, cy, r / 2, fg);
    G->drawFastHLine(cx - r / 2, cy, r, fg);
    G->drawFastVLine(cx, cy - r / 2, r, fg);
    for (int dy = -r / 2; dy <= r / 2; dy++) {   // meridian ellipse
      int dx = (int)(r / 4.0f * sqrtf(1.0f - (4.0f * dy * dy) / (r * r)));
      G->drawPixel(cx - dx, cy + dy, fg);
      G->drawPixel(cx + dx, cy + dy, fg);
    }
  } else if (c.type == DChatType::CHANNEL && c.kind == CHAN_PRIVATE) {   // lock
    G->drawRoundRect(cx - 4, cy - 8, 8, 9, 3, fg);
    G->fillRoundRect(cx - 6, cy - 2, 12, 9, 2, fg);
  } else {
    char a[2];
    if (c.type == DChatType::CHANNEL) strcpy(a, "#");
    else avatarGlyph(c.name, a);
    drawText(G, FONT_BOLD, cx - textWidth(FONT_BOLD, a) / 2, cy - 7, fg, a);
  }
}

void UITask::drawAppBar(const char* title, const char* subtitle, bool back, int avatar_chat) {
  G->fillRect(0, 0, DECK_W, BAR_H, C_BG);
  int x = 8;
  if (back) {   // arrow
    G->drawLine(8, 15, 16, 7, C_TEXT);
    G->drawLine(8, 15, 16, 23, C_TEXT);
    G->drawFastHLine(8, 15, 13, C_TEXT);
    addHit(0, 0, 48, BAR_H, HIT_BACK, 0);
    x = 28;
  }
  if (avatar_chat >= 0) {
    drawChatAvatar(avatar_chat, x + 10, 15, 11);
    x += 26;
  }
  char buf[48];
  if (subtitle) {
    ellipsize(FONT_BOLD, title, 236 - x, buf, sizeof(buf));
    drawText(G, FONT_BOLD, x, 1, C_TEXT, buf);
    ellipsize(FONT_SMALL, subtitle, 236 - x, buf, sizeof(buf));
    drawText(G, FONT_SMALL, x, 19, C_MUTED, buf);
  } else {
    ellipsize(FONT_TITLE, title, 200 - x, buf, sizeof(buf));
    G->setFont(FONT_TITLE);
    G->setTextColor(C_TEXT);
    G->setCursor(x, 22);
    G->print(buf);
  }
  // the app's connection chip: our name with a green dot, then battery
  char name[24];
  ellipsize(FONT_SMALL, _node_prefs->node_name, 66, name, sizeof(name));
  int nx = DECK_W - 28 - textWidth(FONT_SMALL, name);
  if (subtitle == NULL) {
    G->fillCircle(nx - 7, 14, 3, C_ONLINE);
    drawText(G, FONT_SMALL, nx, 11, C_MUTED, name);
  }
  drawBattery(DECK_W - 4, 10);
}

void UITask::drawNav() {
  int y = DECK_H - NAV_H;
  G->fillRect(0, y, DECK_W, NAV_H, C_IN_BUBBLE);
  const char* labels[2] = { "Chats", "Settings" };
  bool chats = _screen == DScreen::CHATS;
  for (int i = 0; i < 2; i++) {
    bool sel = (i == 0) == chats;
    int cx = 80 + i * 160;
    addHit(i * 160, y, 160, NAV_H, HIT_TAB, i);
    int w = textWidth(FONT, labels[i]);
    if (sel) G->fillRoundRect(cx - w / 2 - 14, y + 4, w + 28, 22, 11, C_SEC_CONT);
    drawText(G, sel ? FONT_BOLD : FONT, cx - w / 2, y + 6, sel ? C_TEXT : C_MUTED, labels[i]);
    if (i == 0) {
      int unread = totalUnread();
      if (unread > 0) {
        char b[6];
        if (unread > 99) strcpy(b, "99+"); else sprintf(b, "%d", unread);
        int bw = textWidth(FONT_SMALL, b) + 8;
        int bx = cx + w / 2 + 4;
        G->fillRoundRect(bx, y + 3, bw, 12, 6, C_ERROR);
        drawText(G, FONT_SMALL, bx + 4, y + 5, C_ON_PRIMARY, b);
      }
    }
  }
}

// The app shows its control posts as plain sentences, worded for the reader.
void UITask::describeMessage(const DMsg& m, bool channel, char* sender, char* body, int body_size, bool* italic, char* extra) {
  char text[DECK_TEXT_MAX + 1];
  toAscii(text, m.text, sizeof(text));
  sender[0] = 0;
  extra[0] = 0;
  *italic = false;
  const char* b = text;
  bool own = m.status != MSG_IN;
  if (channel && !own) {
    const char* sep = strstr(text, ": ");
    int len = sep ? sep - text : 0;
    if (len > 0 && len < 32) {
      memcpy(sender, text, len);
      sender[len] = 0;
      b = sep + 2;
    }
  }
  if (channel) {
    char name[32];
    const char* who = own ? "You" : sender;
    const char* hex = parseControl(b, REQ_PREFIX, REQ_KEY, name, sizeof(name));
    if (hex) {
      *italic = true;
      if (own) snprintf(body, body_size, "You asked %s to message privately.", name);
      else if (strcasecmp(name, _node_prefs->node_name) == 0) {
        snprintf(body, body_size, "%s wants to message you privately.", who);
        snprintf(extra, 24, "Their key starts %.8s", hex);
      } else snprintf(body, body_size, "%s asked %s to message privately.", who, name);
      return;
    }
    hex = parseControl(b, ACC_PREFIX, ACC_KEY, name, sizeof(name));
    if (hex) {
      *italic = true;
      if (own) snprintf(body, body_size, "You accepted %s's request to message privately.", name);
      else if (strcasecmp(name, _node_prefs->node_name) == 0) snprintf(body, body_size, "%s accepted your request. You can message each other directly.", who);
      else snprintf(body, body_size, "%s accepted %s's request to message privately.", who, name);
      return;
    }
  }
  // a shared channel link: show what it is, not its key
  char cname[32];
  uint8_t secret[16];
  const char *ls, *le;
  if (parseChannelLink(b, cname, sizeof(cname), secret, &ls, &le)) {
    char nm[32];
    toAscii(nm, cname, sizeof(nm));
    snprintf(body, body_size, "%.*s[Channel: %s]%s", (int)(ls - b), b, nm, le);
    if (!own) strcpy(extra, heldChannel(secret) >= 0 ? "You're in this channel" : "Tap to join");
    return;
  }
  StrHelper::strncpy(body, b, body_size);
}

int UITask::messageHeight(const DMsg& m, bool channel) {
  char sender[32], body[DECK_TEXT_MAX + 64], extra[24];
  bool italic;
  describeMessage(m, channel, sender, body, sizeof(body), &italic, extra);
  char lines[10][64];
  int n = wrapText(italic ? FONT_ITALIC : FONT, body, BUBBLE_MAX, lines, 10);
  return PAD_Y + (sender[0] ? NAME_H : 0) + n * LINE_H + (extra[0] ? NAME_H : 0) + META_H + PAD_Y - 2;
}

void UITask::drawMessage(const DMsg& m, bool channel, int y_bottom, bool selected) {
  char sender[32], body[DECK_TEXT_MAX + 64], extra[24];
  bool italic;
  describeMessage(m, channel, sender, body, sizeof(body), &italic, extra);
  const GFXfont* f = italic ? FONT_ITALIC : FONT;
  char lines[10][64];
  int n = wrapText(f, body, BUBBLE_MAX, lines, 10);
  bool own = m.status != MSG_IN;

  char meta[16];
  formatTime(m.ts, meta, true);
  int ticks = 0;   // status after the time, as the app: pending ..., sent ✓, delivered ✓✓, failed ✗
  if (m.status == MSG_OUT || (m.status == MSG_OUT_PENDING && m.attempt > 0)) ticks = 1;
  if (m.status == MSG_OUT_DELIVERED) ticks = 2;
  if (m.status == MSG_OUT_PENDING && m.attempt == 0) strcat(meta, "  ...");
  if (m.status == MSG_OUT_FAILED) strcat(meta, "  x");
  if (m.status == MSG_OUT_PENDING && m.attempt > 0) sprintf(meta + strlen(meta), " (try %d)", m.attempt + 1);
  int meta_w = textWidth(FONT_SMALL, meta) + (ticks ? 4 + ticks * 5 + 2 : 0);

  int w = meta_w;
  for (int i = 0; i < n; i++) { int lw = textWidth(f, lines[i]); if (lw > w) w = lw; }
  if (sender[0]) { int sw = textWidth(FONT_SMALL, sender); if (sw > w) w = sw; }
  if (extra[0]) { int ew = textWidth(FONT_SMALL, extra); if (ew > w) w = ew; }
  w += 2 * PAD_X;
  int h = PAD_Y + (sender[0] ? NAME_H : 0) + n * LINE_H + (extra[0] ? NAME_H : 0) + META_H + PAD_Y - 2;
  int y = y_bottom - h;
  int x = own ? DECK_W - 6 - w : (channel ? 30 : 6);

  { int hy = y < BAR_H ? BAR_H : y, hb = y_bottom > DECK_H - COMPOSE_H ? DECK_H - COMPOSE_H : y_bottom;
    if (hb > hy) addHit(own ? x : 0, hy, own ? w : x + w, hb - hy, HIT_MSG, &m - _msgs); }
  uint16_t bg = own ? C_OUT_BUBBLE : C_IN_BUBBLE;
  bool mention = !own && strstr(m.text, "@[") && strcasestr(m.text, _node_prefs->node_name);
  G->fillRoundRect(x, y, w, h, 9, bg);
  G->fillRect(own ? x + w - 9 : x, y + h - 9, 9, 9, bg);   // the tail corner stays square
  if (mention) G->drawRoundRect(x, y, w, h, 9, C_TERTIARY);
  if (selected) {
    G->drawRoundRect(x - 2, y - 2, w + 4, h + 4, 10, C_PRIMARY);
    G->drawRoundRect(x - 3, y - 3, w + 6, h + 6, 11, C_PRIMARY);
  }

  if (!own && channel && sender[0]) {   // the sender's avatar beside the bubble
    char seed[48];
    snprintf(seed, sizeof(seed), "sender|%s", sender);
    uint32_t rgb = avatarRgb(seed);
    G->fillCircle(15, y_bottom - 10, 10, rgb565(rgb));
    char init[2];
    avatarGlyph(sender, init);
    drawText(G, FONT_SMALL, 15 - textWidth(FONT_SMALL, init) / 2, y_bottom - 13, isLight(rgb) ? 0x0000 : 0xFFFF, init);
  }

  int ty = y + PAD_Y;
  if (sender[0]) {
    drawText(G, FONT_SMALL, x + PAD_X, ty + 1, C_PRIMARY, sender);
    ty += NAME_H;
  }
  for (int i = 0; i < n; i++) {
    drawText(G, f, x + PAD_X, ty - 2, C_TEXT, lines[i]);
    ty += LINE_H;
  }
  if (extra[0]) {
    drawText(G, FONT_SMALL, x + PAD_X, ty + 1, C_MUTED, extra);
    ty += NAME_H;
  }
  int mx = x + w - PAD_X - meta_w;
  drawText(G, FONT_SMALL, mx, ty + 1, m.status == MSG_OUT_FAILED ? C_ERROR : C_MUTED, meta);
  int tx = mx + textWidth(FONT_SMALL, meta) + 4;
  for (int t = 0; t < ticks; t++) drawTick(G, tx + t * 5, ty + 2, ticks == 2 ? C_PRIMARY : C_MUTED);
}

void UITask::drawCompose(const char* placeholder) {
  int y = DECK_H - COMPOSE_H;
  G->fillRect(0, y, DECK_W, COMPOSE_H, C_BG);
  bool typing = _compose_len > 0;
  G->drawRoundRect(4, y + 3, 280, 26, 13, typing ? C_PRIMARY : C_OUTLINE);
  if (typing) {
    // the end of the text, so the cursor stays in view
    const char* t = _compose;
    while (textWidth(FONT, t) > 252) t++;
    drawText(G, FONT, 14, y + 6, C_TEXT, t);
    int cx = 14 + textWidth(FONT, t) + 1;
    if ((millis() / 500) % 2 == 0) G->drawFastVLine(cx, y + 9, 14, C_PRIMARY);
  } else {
    drawText(G, FONT, 14, y + 6, C_MUTED, placeholder);
  }
  // send button
  addHit(286, y, 34, COMPOSE_H, HIT_SEND, 0);
  G->fillCircle(302, y + 16, 13, typing ? C_PRIMARY : C_IN_BUBBLE);
  uint16_t pc = typing ? C_ON_PRIMARY : C_OUTLINE;
  G->fillTriangle(296, y + 9, 310, y + 16, 296, y + 23, pc);
  G->fillTriangle(296, y + 9, 300, y + 16, 296, y + 23, typing ? C_PRIMARY : C_IN_BUBBLE);
}

void UITask::drawChats() {
  drawAppBar("Chats", NULL, false, -1);
  int order[DECK_MAX_CHATS];
  int n = sortedChats(order);
  const int top = BAR_H, rows = (DECK_H - NAV_H - BAR_H) / ROW_H;
  if (_list_sel >= n) _list_sel = n > 0 ? n - 1 : 0;
  if (_touch_scrolled) {   // where the finger left it; keep the highlight on screen
    if (_list_top > n - rows) _list_top = n - rows;
    if (_list_top < 0) _list_top = 0;
    if (_list_sel < _list_top) _list_sel = _list_top;
    if (_list_sel >= _list_top + rows) _list_sel = _list_top + rows - 1;
  } else {
    if (_list_sel < _list_top) _list_top = _list_sel;
    if (_list_sel >= _list_top + rows) _list_top = _list_sel - rows + 1;
  }

  if (n == 0) {
    drawText(G, FONT, 160 - textWidth(FONT, "No conversations yet.") / 2, 90, C_MUTED, "No conversations yet.");
    const char* hint = "Add a channel on the config page.";
    drawText(G, FONT, 160 - textWidth(FONT, hint) / 2, 110, C_MUTED, hint);
  }
  for (int r = 0; r < rows && _list_top + r < n; r++) {
    int ci = order[_list_top + r];
    DChat& c = _chats[ci];
    int y = top + r * ROW_H;
    addHit(0, y, DECK_W, ROW_H, HIT_CHAT_ROW, _list_top + r);
    if (_list_top + r == _list_sel) {
      G->fillRect(0, y, DECK_W, ROW_H, C_IN_BUBBLE);
      G->fillRect(0, y + 4, 3, ROW_H - 8, C_PRIMARY);
    }
    drawChatAvatar(ci, 22, y + ROW_H / 2, 14);

    // right column: time, unread badge
    DMsg* last = lastMsg(ci);
    int right_w = 0;
    if (last) {
      char t[16];
      if (localDay(last->ts) == localDay(the_mesh.getRTCClock()->getCurrentTime())) formatTime(last->ts, t, true);
      else formatTime(last->ts, t, false);
      if (strlen(t) > 9) t[3] = 0;   // weekday -> "Mon"
      drawTextRight(G, FONT_SMALL, DECK_W - 8, y + 5, c.unread ? C_PRIMARY : C_MUTED, t);
      right_w = textWidth(FONT_SMALL, t);
    }
    if (c.unread) {
      char b[6];
      if (c.unread > 99) strcpy(b, "99+"); else sprintf(b, "%d", c.unread);
      int bw = textWidth(FONT_SMALL, b) + 10;
      if (bw < 16) bw = 16;
      G->fillRoundRect(DECK_W - 8 - bw, y + 18, bw, 14, 7, C_ERROR);
      drawText(G, FONT_SMALL, DECK_W - 8 - bw + (bw - textWidth(FONT_SMALL, b)) / 2 + 1, y + 21, C_ON_PRIMARY, b);
      if (bw > right_w) right_w = bw;
    }

    int tx = 44, tw = DECK_W - tx - right_w - 14;
    char buf[64];
    ellipsize(c.unread ? FONT_BOLD : FONT, c.name, tw, buf, sizeof(buf));
    drawText(G, c.unread ? FONT_BOLD : FONT, tx, y + 1, C_TEXT, buf);

    // preview: "Sender: text" for channels, the text for DMs
    char prev[DECK_TEXT_MAX + 80];
    if (last) {
      char sender[32], body[DECK_TEXT_MAX + 64], extra[24];
      bool italic;
      describeMessage(*last, c.type == DChatType::CHANNEL, sender, body, sizeof(body), &italic, extra);
      if (italic) StrHelper::strncpy(prev, body, sizeof(prev));
      else if (last->status != MSG_IN) snprintf(prev, sizeof(prev), "You: %s", body);
      else if (sender[0]) snprintf(prev, sizeof(prev), "%s: %s", sender, body);
      else StrHelper::strncpy(prev, body, sizeof(prev));
    } else {
      strcpy(prev, "No messages yet");
    }
    ellipsize(FONT, prev, DECK_W - tx - 34, buf, sizeof(buf));
    drawText(G, FONT, tx, y + 18, C_MUTED, buf);
  }
  drawNav();
}

void UITask::drawChat() {
  if (_chat < 0 || _chat >= _num_chats) {
    _screen = DScreen::CHATS;
    drawChats();
    return;
  }
  DChat& c = _chats[_chat];
  bool channel = c.type == DChatType::CHANNEL;
  const int area_top = BAR_H, area_bottom = DECK_H - COMPOSE_H;

  int slots[DECK_MAX_MSGS];
  int count = chatMessages(_chat, slots, DECK_MAX_MSGS);
  static int heights[DECK_MAX_MSGS];
  static bool seps[DECK_MAX_MSGS];
  int sel_pos = -1;
  for (int k = 0; k < count; k++) {
    heights[k] = messageHeight(_msgs[slots[k]], channel);
    seps[k] = k == 0 || localDay(_msgs[slots[k]].ts) != localDay(_msgs[slots[k - 1]].ts);
    if (slots[k] == _sel_msg) sel_pos = k;
  }
  // newest at the bottom; scroll up just enough to show the highlighted one
  int shift = 0;
  if (sel_pos < 0) {   // touch scroll, within what there is
    int total = 0;
    for (int k = 0; k < count; k++) total += heights[k] + MSG_GAP + (seps[k] ? SEP_H : 0);
    int max_shift = total - (area_bottom - area_top);
    if (max_shift < 0) max_shift = 0;
    if (_scroll_px > max_shift) _scroll_px = max_shift;
    if (_scroll_px < 0) _scroll_px = 0;
    shift = _scroll_px;
  } else {
    int below = 0;
    for (int k = count - 1; k > sel_pos; k--) below += heights[k] + MSG_GAP + (seps[k] ? SEP_H : 0);
    int need = below + heights[sel_pos] + MSG_GAP + (seps[sel_pos] ? SEP_H : 0);
    if (need > area_bottom - area_top) shift = need - (area_bottom - area_top);
  }
  int y = area_bottom - MSG_GAP + shift;
  for (int k = count - 1; k >= 0 && y > area_top; k--) {
    if (y - heights[k] < area_bottom) drawMessage(_msgs[slots[k]], channel, y, slots[k] == _sel_msg);
    y -= heights[k] + MSG_GAP;
    if (seps[k]) {
      char label[16];
      formatTime(_msgs[slots[k]].ts, label, false);
      int w = textWidth(FONT_SMALL, label) + 16;
      if (y - SEP_H < area_bottom) {
        G->fillRoundRect(160 - w / 2, y - SEP_H + 3, w, 14, 7, C_IN_BUBBLE);
        drawText(G, FONT_SMALL, 160 - w / 2 + 8, y - SEP_H + 6, C_MUTED, label);
      }
      y -= SEP_H;
    }
  }
  if (count == 0) {
    const char* t = "No messages yet";
    drawText(G, FONT, 160 - textWidth(FONT, t) / 2, 100, C_MUTED, t);
  }

  const char* sub = "Direct message";
  if (channel) {
    if (c.kind == CHAN_PUBLIC) sub = "Public - anyone can read it";
    else if (c.kind == CHAN_HASHTAG) sub = "Hashtag - anyone can read it";
    else sub = "Private - not secure";
  }
  char title[40];
  StrHelper::strncpy(title, c.name[0] ? c.name : "Channel", sizeof(title));
  drawAppBar(title, sub, true, _chat);
  if (_sel_msg >= 0) drawTextRight(G, FONT_SMALL, DECK_W - 30, 11, C_PRIMARY, "click: options");
  drawCompose(channel ? "Message channel..." : "Message...");
}

void UITask::drawActions() {
  drawChat();   // the app's popup, over the dimmed thread
  for (int y = 0; y < DECK_H; y += 2) G->drawFastHLine(0, y, DECK_W, C_BG);   // dim
  static const char* LABELS[] = { "Message privately", "Accept DM request", "Mention", "Resend", "Join channel", "Cancel" };
  int h = 12 + _num_actions * 30;
  int w = 220, x = (DECK_W - w) / 2, y = (DECK_H - h) / 2;
  G->fillRoundRect(x, y, w, h, 14, C_SURFACE);
  G->drawRoundRect(x, y, w, h, 14, C_OUTLINE_VAR);
  for (int i = 0; i < _num_actions; i++) {
    int ry = y + 6 + i * 30;
    bool sel = i == _action_sel;
    addHit(x, ry, w, 30, HIT_ACTION, i);
    if (sel) G->fillRoundRect(x + 6, ry, w - 12, 28, 8, C_SEC_CONT);
    char label[48];
    if (_actions[i] == ACT_DM) snprintf(label, sizeof(label), "Message %s privately", _action_name);
    else if (_actions[i] == ACT_JOIN) snprintf(label, sizeof(label), "Join %s", _join_name);
    else StrHelper::strncpy(label, LABELS[_actions[i]], sizeof(label));
    char buf[48];
    ellipsize(FONT, label, w - 32, buf, sizeof(buf));
    drawText(G, FONT, x + 16, ry + 6, _actions[i] == ACT_CANCEL ? C_MUTED : C_TEXT, buf);
  }
}

void UITask::drawSettings() {
  drawAppBar("Settings", NULL, false, -1);
  const int top = BAR_H, bottom = DECK_H - NAV_H;
  // keep the selected row in view
  int ys[NUM_SET_ROWS], total = 0;
  for (int i = 0; i < NUM_SET_ROWS; i++) {
    ys[i] = total;
    total += SET_ROWS[i].kind == 'H' ? HDR_H : ROW_H;
  }
  int sel_y = ys[_set_sel], sel_h = ROW_H;
  if (_touch_scrolled) {
    int max_top = total - (bottom - top);
    if (_set_top > max_top) _set_top = max_top;
    if (_set_top < 0) _set_top = 0;
    if (sel_y < _set_top || sel_y + sel_h > _set_top + (bottom - top)) {   // highlight the nearest row in view
      for (int i = 0; i < NUM_SET_ROWS; i++) {
        if (SET_ROWS[i].kind == 'H' || ys[i] < _set_top) continue;
        if (ys[i] + ROW_H <= _set_top + (bottom - top)) { _set_sel = i; break; }
      }
    }
  } else {
    if (sel_y - HDR_H < _set_top) _set_top = sel_y - HDR_H;
    if (_set_top < 0) _set_top = 0;
    if (sel_y + sel_h > _set_top + (bottom - top)) _set_top = sel_y + sel_h - (bottom - top);
  }

  for (int i = 0; i < NUM_SET_ROWS; i++) {
    int y = top + ys[i] - _set_top;
    if (y + ROW_H < top || y >= bottom) continue;
    const SetRow& r = SET_ROWS[i];
    if (r.kind == 'H') {
      drawText(G, FONT_SMALL, 14, y + 6, C_PRIMARY, r.title);
      continue;
    }
    bool sel = i == _set_sel;
    addHit(0, y, DECK_W, ROW_H, HIT_SET_ROW, i);
    if (sel) {
      G->fillRect(0, y, DECK_W, ROW_H, _set_edit ? C_SEC_CONT : C_IN_BUBBLE);
      G->fillRect(0, y + 4, 3, ROW_H - 8, C_PRIMARY);
    }
    char sub[64];
    sub[0] = 0;
    switch (r.kind) {
      case 'n': snprintf(sub, sizeof(sub), "Name: %s", _node_prefs->node_name); break;
      case 'r': {
        int pi = matchPreset(_node_prefs->freq, _node_prefs->bw, _node_prefs->sf, _node_prefs->cr);
        if (pi >= 0) snprintf(sub, sizeof(sub), "%s - change your region", PRESETS[pi].name);
        else snprintf(sub, sizeof(sub), "Custom: %.3f MHz, BW%g, SF%d", _node_prefs->freq, _node_prefs->bw, _node_prefs->sf);
        break;
      }
      case 'z': strcpy(sub, "Nearby nodes learn your name"); break;
      case 'f': strcpy(sub, "Announce yourself across the mesh"); break;
      case 'c': {
        int n = 0;
        for (int k = 0; k < _num_chats; k++) if (_chats[k].type == DChatType::CHANNEL) n++;
        snprintf(sub, sizeof(sub), "%d channel%s - add, remove, share keys", n, n == 1 ? "" : "s");
        break;
      }
      case 's': strcpy(sub, _node_prefs->buzzer_quiet ? "Off" : "On, for new messages"); break;
      case 'k': strcpy(sub, _ui.keep_msgs ? "On - saved here, kept after a restart" : "Off - in memory only, gone after a restart"); break;
      case 'x': {
        int n = 0;
        for (int k = 0; k < DECK_MAX_MSGS; k++) if (_msgs[k].id) n++;
        snprintf(sub, sizeof(sub), "%d message%s on this device", n, n == 1 ? "" : "s");
        break;
      }
      case 't': {
        int hh = abs(_ui.tz_halfhours) / 2;
        snprintf(sub, sizeof(sub), "UTC%c%d%s  -  for message times", _ui.tz_halfhours < 0 ? '-' : '+', hh, (abs(_ui.tz_halfhours) % 2) ? ":30" : "");
        break;
      }
      case 'o': snprintf(sub, sizeof(sub), "%s", TIMEOUT_NAMES[_ui.timeout_idx]); break;
      case 'a': strcpy(sub, "Version, radio, what this is"); break;
    }
    if (sel && _set_edit) strcat(sub, "   (drag or roll, tap when done)");
    drawText(G, FONT, 14, y + 1, C_TEXT, r.title);
    char buf[72];
    ellipsize(FONT_SMALL, sub, DECK_W - 24, buf, sizeof(buf));
    drawText(G, FONT_SMALL, 14, y + 22, sel && _set_edit ? C_PRIMARY : C_MUTED, buf);
  }
  drawNav();
}

void UITask::drawName() {
  drawAppBar("Your name", _setup ? "Step 2 of 2 - shown to others in the chat" : "Shown to others in the chat", _setup || !isDefaultName(), -1);
  drawText(G, FONT, 14, 52, C_TEXT, isDefaultName() ? "Welcome! Pick a name to start." : "Change your name");
  drawText(G, FONT, 14, 76, C_MUTED, "Type it and press Enter.");
  int y = 112;
  G->drawRoundRect(10, y, 300, 30, 8, C_PRIMARY);
  const char* t = _compose;
  while (textWidth(FONT, t) > 280) t++;
  drawText(G, FONT, 20, y + 7, C_TEXT, t);
  if ((millis() / 500) % 2 == 0) G->drawFastVLine(20 + textWidth(FONT, t) + 1, y + 8, 15, C_PRIMARY);
  char cnt[16];
  snprintf(cnt, sizeof(cnt), "%d/%d", _compose_len, composeLimit());
  drawTextRight(G, FONT_SMALL, 306, y + 36, C_MUTED, cnt);
  // Save button (Enter does the same)
  int bw = textWidth(FONT_BOLD, "Save") + 32, bx = 310 - bw, by = y + 52;
  G->fillRoundRect(bx, by, bw, 30, 15, _compose_len ? C_PRIMARY : C_IN_BUBBLE);
  drawText(G, FONT_BOLD, bx + 16, by + 7, _compose_len ? C_ON_PRIMARY : C_OUTLINE, "Save");
  addHit(bx, by, bw, 30, HIT_YES, 0);
}

void UITask::drawAbout() {
  drawAppBar("About", "K.I.S.S. Messenger for T-Deck", true, -1);
  char line[80], hex[17];
  int y = BAR_H + 6;
  auto row = [&](const char* k, const char* v) {
    drawText(G, FONT_SMALL, 14, y + 4, C_MUTED, k);
    char buf[64];
    ellipsize(FONT, v, DECK_W - 104, buf, sizeof(buf));
    drawText(G, FONT, 96, y, C_TEXT, buf);
    y += 21;
  };
  row("Name", _node_prefs->node_name);
  toLowerHex(hex, the_mesh.self_id.pub_key, 8);
  snprintf(line, sizeof(line), "%s...", hex);
  row("Key", line);
  row("Firmware", FIRMWARE_VERSION);
  snprintf(line, sizeof(line), "%.3f MHz  BW%g  SF%d  CR%d", _node_prefs->freq, _node_prefs->bw, _node_prefs->sf, _node_prefs->cr);
  row("Radio", line);
  uint16_t mv = getBattMilliVolts();
  snprintf(line, sizeof(line), "%u.%02u V", mv / 1000, (mv % 1000) / 10);
  row("Battery", line);
  snprintf(line, sizeof(line), "%d", the_mesh.getNumContacts());
  row("Contacts", line);
  row("Keyboard", _input.keyboardPresent() ? "Connected" : "Not found");
  uint32_t now = the_mesh.getRTCClock()->getCurrentTime();
  char t[8], d[16];
  formatTime(now, t, true);
  int yy, mm, dd;
  civilFromDays(localDay(now), yy, mm, dd);
  snprintf(d, sizeof(d), "%s %d", MONTHS[mm - 1], dd);
  snprintf(line, sizeof(line), "%s %s, %s", WEEKDAYS[(localDay(now) + 4) % 7], d, t);
  row("Clock", line);
}

void UITask::drawPrompt() {
  // the screen behind, dimmed
  DScreen behind = _prompt_return;
  _screen = behind;
  if (behind == DScreen::CHAT) drawChat();
  else if (behind == DScreen::SETTINGS) drawSettings();
  else if (behind == DScreen::CHANNEL_INFO) drawChannelInfo();
  else if (behind == DScreen::SHARE_PICK) drawSharePick();
  else drawChats();
  _screen = DScreen::PROMPT;
  for (int y = 0; y < DECK_H; y += 2) G->drawFastHLine(0, y, DECK_W, C_BG);

  char title[96], body[200];
  const char *no_label, *yes_label;
  if (_prompt == DPrompt::SHARE_CHANNEL) {
    ChannelDetails cd;
    char ch[32] = "this channel", who[32];
    if (the_mesh.getChannel(_ch_slot, cd)) toAscii(ch, cd.name, sizeof(ch));
    toAscii(who, _prompt_name, sizeof(who));
    snprintf(title, sizeof(title), "Share %s with %s?", ch, who);
    snprintf(body, sizeof(body), "This sends them the channel's key in a direct message. They'll be able to read everything on it, and can pass it on.");
    no_label = "Cancel";
    yes_label = "Share";
  } else if (_prompt == DPrompt::JOIN_CHANNEL) {
    char nm[32], from[32];
    toAscii(nm, _join_name, sizeof(nm));
    toAscii(from, _join_from, sizeof(from));
    snprintf(title, sizeof(title), "Join %s?", nm);
    snprintf(body, sizeof(body), "%s shared this channel with you. Anyone who has its key can read everything on it.", from);
    no_label = "Not now";
    yes_label = "Join";
  } else if (_prompt == DPrompt::KEEP_OFF) {
    strcpy(title, "Stop keeping messages?");
    strcpy(body, "All messages are cleared now, and new ones are gone after a restart. Channels and contacts stay.");
    no_label = "Cancel";
    yes_label = "Turn off";
  } else if (_prompt == DPrompt::CLEAR_MESSAGES) {
    strcpy(title, "Clear all messages?");
    strcpy(body, "Every chat's messages are deleted from this device. Channels and contacts stay.");
    no_label = "Cancel";
    yes_label = "Clear";
  } else if (_prompt == DPrompt::REMOVE_CHANNEL) {
    snprintf(title, sizeof(title), "Remove %s?", _prompt_name);
    strcpy(body, "You'll stop getting its messages here. You can join again later with the same key.");
    no_label = "Cancel";
    yes_label = "Remove";
  } else if (_prompt == DPrompt::INCOMING_REQUEST) {
    snprintf(title, sizeof(title), "%s wants to message you privately.", _prompt_name);
    char hex[9];
    toLowerHex(hex, _prompt_key, 4);
    snprintf(body, sizeof(body), "Their key starts %s", hex);
    no_label = "Decline";
    yes_label = "Accept";
  } else {
    snprintf(title, sizeof(title), "Message %s privately?", _prompt_name);
    snprintf(body, sizeof(body), "This posts a short request on this channel. Everyone here will see that you asked. If %s accepts, you'll both be able to send direct messages.", _prompt_name);
    no_label = "Cancel";
    yes_label = "Send request";
  }
  char tl[3][64], bl[6][64];
  int tn = wrapText(FONT_BOLD, title, 264, tl, 3);
  int bn = wrapText(FONT, body, 264, bl, 6);
  int h = 14 + tn * LINE_H + 6 + bn * LINE_H + 44;
  int x = 14, w = DECK_W - 28, y = (DECK_H - h) / 2;
  G->fillRoundRect(x, y, w, h, 16, C_SURFACE);
  G->drawRoundRect(x, y, w, h, 16, C_OUTLINE_VAR);
  int ty = y + 12;
  for (int i = 0; i < tn; i++) { drawText(G, FONT_BOLD, x + 14, ty, C_TEXT, tl[i]); ty += LINE_H; }
  ty += 6;
  for (int i = 0; i < bn; i++) { drawText(G, FONT, x + 14, ty, C_MUTED, bl[i]); ty += LINE_H; }

  // buttons, right-aligned as Material dialogs: text button, then filled
  int by = y + h - 36;
  int yw = textWidth(FONT_BOLD, yes_label) + 28, nw = textWidth(FONT_BOLD, no_label) + 28;
  int yx = x + w - 12 - yw, nx = yx - 8 - nw;
  if (_prompt_yes) {
    G->fillRoundRect(yx, by, yw, 28, 14, C_PRIMARY);
  } else {
    G->drawRoundRect(yx, by, yw, 28, 14, C_OUTLINE);
    G->fillRoundRect(nx, by, nw, 28, 14, C_SEC_CONT);
  }
  addHit(yx, by - 4, yw, 36, HIT_YES, 0);
  addHit(nx, by - 4, nw, 36, HIT_NO, 0);
  drawText(G, FONT_BOLD, yx + 14, by + 6, _prompt_yes ? C_ON_PRIMARY : C_PRIMARY, yes_label);
  drawText(G, FONT_BOLD, nx + 14, by + 6, C_PRIMARY, no_label);
}

void UITask::drawToast() {
  if (!_alert_until || (long)(millis() - _alert_until) >= 0) return;
  char lines[2][64];
  int n = wrapText(FONT_SMALL, _alert, 270, lines, 2);
  int w = 0;
  for (int i = 0; i < n; i++) { int lw = textWidth(FONT_SMALL, lines[i]); if (lw > w) w = lw; }
  w += 24;
  int h = 10 + n * 11;
  int x = (DECK_W - w) / 2, y = DECK_H - COMPOSE_H - h - 8;
  G->fillRoundRect(x, y, w, h, h / 2 < 12 ? h / 2 : 12, C_TEXT);
  for (int i = 0; i < n; i++) drawText(G, FONT_SMALL, x + 12, y + 6 + i * 11, C_BG, lines[i]);
}

void UITask::render() {
  if (_display == NULL || !_display->isOn() || G == NULL) return;
  G->fillScreen(C_BG);
  _num_hits = 0;
  switch (_screen) {
    case DScreen::CHATS:    drawChats(); break;
    case DScreen::SETTINGS: drawSettings(); break;
    case DScreen::CHAT:     drawChat(); break;
    case DScreen::ACTIONS:  drawActions(); break;
    case DScreen::NAME:     drawName(); break;
    case DScreen::ABOUT:    drawAbout(); break;
    case DScreen::PROMPT:   drawPrompt(); break;
    case DScreen::REGION:   drawRegion(); break;
    case DScreen::CHANNELS:     drawChannels(); break;
    case DScreen::CHANNEL_INFO: drawChannelInfo(); break;
    case DScreen::ADD_CHANNEL:  drawAddChannel(); break;
    case DScreen::CHANNEL_FORM: drawChannelForm(); break;
    case DScreen::SHARE_PICK:   drawSharePick(); break;
  }
  drawToast();
  _display->push();
}

// ---------------------------------------------------------------- loop

void UITask::loop() {
  unsigned long now = millis();

  int key = _input.read();
  if (key > 0) {
    if (key == KEY_LONG_PRESS) {   // hold the trackball: screen off / on
      if (_display && _display->isOn()) _display->turnOff();
      else wake();
    } else {
      bool was_off = _display && !_display->isOn();
      wake();
      if (!was_off) handleKey(key);   // the first input only wakes the screen
    }
  }

  DeckTouchEvent te = _touch.read();
  if (te != TOUCH_NONE) handleTouch(te);

  checkOutgoing();

  // channels/contacts can change over USB (the web config page)
  if ((_screen == DScreen::CHATS || _screen == DScreen::SETTINGS) && (long)(now - _next_rebuild) >= 0) {
    _next_rebuild = now + 3000;
    rebuildChats();
  }

  _sound.loop();

  if (_store_dirty && (long)(now - _store_due) >= 0) saveMessages();

  if (_alert_until && (long)(now - _alert_until) >= 0) {
    _alert_until = 0;
    _dirty = true;
  }

  if (_display && _display->isOn()) {
    if (timeoutMillis() && (long)(now - _auto_off) >= 0) {
      _display->turnOff();
    } else if (_dirty || (long)(now - _next_refresh) >= 0) {
      render();
      _dirty = false;
      // the compose cursor blinks while typing; otherwise just keep the clock fresh
      bool typing = (_screen == DScreen::CHAT && _compose_len > 0) || _screen == DScreen::NAME;
      _next_refresh = now + (typing ? 500 : 15000);
    }
  }
}
