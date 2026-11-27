#pragma once

// K.I.S.S. Messenger for the LilyGo T-Deck: the K.I.S.S. Messenger's
// standalone chat (ui-kiss) on a 320x240 colour screen with keyboard,
// trackball and speaker, laid out like the MeshCore phone app in simple mode
// (Chats and Settings tabs, avatars, bubbles, the app's dark theme). Same
// behaviour as ui-kiss: no auto-added contacts, chat on group channels, and
// DMs set up by the app's request/accept exchange in the group chat, which
// carries both public keys ("DM request for @[Bob] mch:r:<key>" /
// "DM accepted @[Alice] mch:a:..."). Keep that protocol identical to ui-kiss
// and the app.

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/SensorManager.h>
#include <helpers/MultiSerialInterface.h>
#include <Arduino.h>

#include "../AbstractUITask.h"
#include "../NodePrefs.h"
#include "DeckDisplay.h"
#include "DeckInput.h"
#include "DeckSound.h"
#include "DeckTouch.h"

#define DECK_MAX_CHATS      32
#define DECK_MAX_MSGS       96
#define DECK_TEXT_MAX       MAX_TEXT_LEN
#define DECK_COMPOSE_MAX    150
#define DECK_HANDSHAKES     4
#define DECK_DM_ATTEMPTS    3
#define DECK_CLOCK_SAMPLES  8
#define DECK_MAX_ACTIONS    5

enum class DChatType : uint8_t { CHANNEL, DIRECT };
enum DChanKind : uint8_t { CHAN_PRIVATE, CHAN_PUBLIC, CHAN_HASHTAG };

struct DChat {
  DChatType type;
  uint8_t ch_idx;                 // CHANNEL: channel slot
  uint8_t kind;                   // CHANNEL: DChanKind
  uint8_t pub_key[PUB_KEY_SIZE];  // DIRECT: contact key
  char name[32];
  uint8_t unread;
  uint32_t last_activity;         // our clock at the last message, 0 = none (survives a restart with saved messages)
};

enum DMsgStatus : uint8_t { MSG_IN, MSG_OUT, MSG_OUT_PENDING, MSG_OUT_DELIVERED, MSG_OUT_FAILED };

struct DMsg {
  uint16_t id;                    // 0 = empty slot
  int8_t chat;                    // index into _chats
  uint8_t status;                 // DMsgStatus
  uint8_t attempt;                // DM retries so far
  uint32_t ts;                    // our clock when sent/received
  char text[DECK_TEXT_MAX + 1];   // incoming channel text keeps its "sender: " prefix
};

// A DM request we sent, waiting for "DM accepted" from that name.
struct DHandshake {
  char name[32];                  // empty = free
  unsigned long expires;
};

struct DOutgoingDM {
  bool active;
  uint16_t msg_id;
  uint8_t pub_key[PUB_KEY_SIZE];
  uint32_t timestamp;
  uint8_t attempt;
  uint32_t acks[DECK_DM_ATTEMPTS];
  unsigned long deadline;
};

// A time heard from one sender, as an offset from our clock when heard.
struct DClockSample {
  uint32_t source;                // hash of the sender's name; 0 = empty
  int32_t offset;                 // their time - our time, seconds
  unsigned long at;               // millis() when heard
};

// UI-only settings, kept in /deck_ui (the companion's prefs have no room)
struct DeckPrefs {
  int8_t tz_halfhours = -16;      // UTC offset for message times (UTC-8)
  uint8_t timeout_idx = 1;        // index into the screen timeout options
  uint8_t keep_msgs = 1;          // save messages to flash and reload them at boot
};

enum class DScreen : uint8_t { CHATS, SETTINGS, CHAT, ACTIONS, NAME, ABOUT, PROMPT, REGION,
                               CHANNELS, CHANNEL_INFO, ADD_CHANNEL, CHANNEL_FORM, SHARE_PICK };
enum class DPrompt : uint8_t { NONE, CONFIRM_REQUEST, INCOMING_REQUEST, REMOVE_CHANNEL, CLEAR_MESSAGES, KEEP_OFF, JOIN_CHANNEL, SHARE_CHANNEL };
// the app's add-channel choices
enum DAddChoice : uint8_t { ADD_PUBLIC, ADD_CREATE_PRIVATE, ADD_JOIN_PRIVATE, ADD_HASHTAG };
enum DAction : uint8_t { ACT_DM, ACT_ACCEPT, ACT_MENTION, ACT_RESEND, ACT_JOIN, ACT_CANCEL };

// Touch targets, registered while drawing (so they always match the screen).
enum DHitType : uint8_t { HIT_TAB = 1, HIT_BACK, HIT_SEND, HIT_CHAT_ROW, HIT_MSG, HIT_ACTION,
                          HIT_YES, HIT_NO, HIT_SET_ROW, HIT_REGION_ROW, HIT_LIST_ROW, HIT_FIELD, HIT_BUTTON };
struct DHit { int16_t x, y, w, h; uint8_t type; uint8_t index; };
#define DECK_MAX_HITS       48
#define DECK_REGION_ROWS    64

class UITask : public AbstractUITask {
  DeckDisplay* _display = NULL;
  SensorManager* _sensors = NULL;
  NodePrefs* _node_prefs = NULL;
  DeckPrefs _ui;
  DeckInput _input;
  DeckSound _sound;
  DeckTouch _touch;
  DHit _hits[DECK_MAX_HITS];
  int _num_hits = 0;
  int _drag_acc = 0;             // touch drag not yet turned into moves
  bool _touch_scrolled = false;  // a list was dragged: show where the finger left it, not the highlight

  DChat _chats[DECK_MAX_CHATS];
  int _num_chats = 0;
  DMsg* _msgs = NULL;             // DECK_MAX_MSGS, in PSRAM
  int _msg_head = 0;              // next slot to write
  uint16_t _next_msg_id = 1;
  DHandshake _handshakes[DECK_HANDSHAKES];
  DOutgoingDM _out;
  DClockSample _clock[DECK_CLOCK_SAMPLES];

  DScreen _screen = DScreen::CHATS;
  DScreen _prompt_return = DScreen::CHATS;
  DPrompt _prompt = DPrompt::NONE;
  bool _prompt_yes = true;        // highlighted button
  char _prompt_name[32];
  uint8_t _prompt_key[PUB_KEY_SIZE];
  uint8_t _prompt_ch_idx = 0;

  int _list_sel = 0, _list_top = 0;
  int _set_sel = 1, _set_top = 0;
  bool _set_edit = false;         // adjusting a value with the trackball
  int _chat = -1;                 // open chat
  int _sel_msg = -1;              // highlighted message slot in the open chat, -1 = none
  int _scroll_px = 0;             // open chat scrolled up by touch, pixels
  int16_t _region_rows[DECK_REGION_ROWS];   // REGION: -1 keep current, -2-g header, else preset
  int _num_region_rows = 0, _region_sel = 0, _region_top = 0;
  bool _setup = false;            // first start: region, then name
  // channel management (CHANNELS / CHANNEL_INFO / ADD_CHANNEL / CHANNEL_FORM)
  int _cl_sel = 0, _cl_top = 0;   // CHANNELS list (row 0 = "Add a channel"), ADD_CHANNEL choices
  int _ch_slot = -1;              // CHANNEL_INFO: channel slot
  int _info_sel = 0;              // CHANNEL_INFO: highlighted button
  uint8_t _add_choice = ADD_PUBLIC;
  int _form_field = 0;            // CHANNEL_FORM: 0 name, 1 key
  char _form_name[32];
  char _form_key[40];
  uint8_t _actions[DECK_MAX_ACTIONS];
  int _num_actions = 0, _action_sel = 0;
  char _action_name[32];          // sender of the highlighted message
  // a channel shared as a meshcore://channel/add link (JOIN_CHANNEL / ACT_JOIN)
  char _join_name[32];
  uint8_t _join_secret[16];
  char _join_from[32];
  bool offerJoin(const char* text, const char* from, bool ask);
  // sharing a channel by DM (CHANNEL_INFO -> SHARE_PICK -> SHARE_CHANNEL)
  int _sp_sel = 0, _sp_top = 0;
  int _share_chat = -1;           // the DM chat picked
  int dmChats(int* out);
  void keySharePick(int key);
  void drawSharePick();
  void shareChannel();
  char _compose[DECK_COMPOSE_MAX + 1];
  int _compose_len = 0;

  char _alert[96];
  unsigned long _alert_until = 0;
  bool _dirty = true;
  bool _store_dirty = false;      // messages changed since the last save
  unsigned long _store_due = 0;
  unsigned long _next_refresh = 0, _auto_off = 0, _next_rebuild = 0;

  void loadUiPrefs();
  void loadMessages();
  void saveMessages();
  void messagesChanged();
  void clearMessages();
  void saveUiPrefs();
  unsigned long timeoutMillis() const;

  // chats + messages
  void rebuildChats();
  int findChannelChat(uint8_t ch_idx) const;
  int findDirectChat(const uint8_t* pub_key) const;
  DMsg* addMsg(int chat, uint8_t status, const char* text);
  DMsg* findMsg(uint16_t id);
  DMsg* lastMsg(int chat);
  void markActivity(int chat, bool incoming);
  int chatMessages(int chat, int* slots, int max);
  int sortedChats(int* order);
  int totalUnread() const;

  // sending
  int composeLimit() const;
  void sendCompose();
  bool sendChannelText(uint8_t ch_idx, const char* text);
  void startDM(const ContactInfo& c, const char* text);
  void sendDMAttempt();
  void checkOutgoing();

  // DM request handshake
  ContactInfo* findContactByName(const char* name);
  DHandshake* findHandshake(const char* name);
  void askDM(const char* name, uint8_t ch_idx);
  void requestDM(const char* name, uint8_t ch_idx);
  void acceptDM();
  bool addContactFromKey(const char* name, const uint8_t* pub_key);
  void handleChannelControl(const char* sender, const char* body, uint8_t ch_idx);

  // input
  void handleKey(int key);
  void keyChats(int key);
  void keySettings(int key);
  void keyChat(int key);
  void keyActions(int key);
  void keyName(int key);
  void keyPrompt(int key);
  bool composeKey(int key);
  void openChat(int idx);
  void openActions();
  void runAction(uint8_t act);
  void activateSetting(int item);
  void adjustSetting(int item, int dir);
  void showPrompt(DPrompt p, const char* name, uint8_t ch_idx);
  void showAlert(const char* text, int millis);
  void handleTouch(int event);
  void tapAt(int x, int y);
  void dragBy(int dy);
  void addHit(int x, int y, int w, int h, uint8_t type, uint8_t index);
  void openRegion(bool setup);
  void keyRegion(int key);
  void chooseRegionRow(int row);
  void drawRegion();
  void openChannels();
  int channelSlots(int* slots);
  void keyChannels(int key);
  void keyChannelInfo(int key);
  void keyAddChannel(int key);
  void keyChannelForm(int key);
  void openForm(uint8_t choice);
  void submitForm();
  int joinChannel(const char* name, const uint8_t* secret);
  void removeChannel(int slot);
  void drawChannels();
  void drawChannelInfo();
  void drawAddChannel();
  void drawChannelForm();
  void drawButton(int x, int y, const char* label, bool primary, bool focused, uint8_t index, bool danger = false);
  void wake();

  // drawing
  void render();
  void drawAppBar(const char* title, const char* subtitle, bool back, int avatar_chat);
  void drawNav();
  void drawChats();
  void drawSettings();
  void drawChat();
  void drawActions();
  void drawName();
  void drawAbout();
  void drawPrompt();
  void drawCompose(const char* placeholder);
  void drawToast();
  void drawBattery(int right_x, int y);
  void drawChatAvatar(int chat, int cx, int cy, int r);
  int messageHeight(const DMsg& m, bool channel);
  void drawMessage(const DMsg& m, bool channel, int y_bottom, bool selected);
  void describeMessage(const DMsg& m, bool channel, char* sender, char* body, int body_size, bool* italic, char* extra);
  void formatTime(uint32_t ts, char* out, bool clock_only);
  long localDay(uint32_t ts) const;

  void syncClock(const char* source, uint32_t their_time);
  bool isDefaultName() const;
  void beep(const DeckNote* notes, int count);

public:
  UITask(mesh::MainBoard* board, MultiSerialInterface* interfaces) : AbstractUITask(board, interfaces) { }
  void begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs);

  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;

  // MyMesh::Listener
  void onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp, const char* text) override;
  void onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails& channel_details, const char* text) override;
  void onQueueSizeChanged(int msgcount) override { }
  void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) override;
  void onACKRecv(uint32_t ack_crc) override;
};
