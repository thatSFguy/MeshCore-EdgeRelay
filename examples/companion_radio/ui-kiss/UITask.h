#pragma once

// K.I.S.S. Messenger UI: a standalone chat interface for a companion with a
// 128x64 OLED and an M5Stack CardKB keyboard. Mirrors the phone app's simple
// mode: chat on a group channel, no auto-added contacts, and DMs set up by the
// app's request/accept exchange in the group chat, which carries both public
// keys ("DM request for @[Bob] mch:r:<key>" / "DM accepted @[Alice] mch:a:...").

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/SensorManager.h>
#include <helpers/MultiSerialInterface.h>
#include <Arduino.h>

#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
#endif

#include "../AbstractUITask.h"
#include "../NodePrefs.h"
#include "CardKB.h"

#define KISS_MAX_CHATS      24
#define KISS_MAX_MSGS       48
#define KISS_TEXT_MAX       MAX_TEXT_LEN
#define KISS_COMPOSE_MAX    150
#define KISS_HANDSHAKES     4
#define KISS_DM_ATTEMPTS    3

enum class KChatType : uint8_t { CHANNEL, DIRECT };

struct KChat {
  KChatType type;
  uint8_t ch_idx;                 // CHANNEL: channel slot
  uint8_t pub_key[PUB_KEY_SIZE];  // DIRECT: contact key
  char name[32];
  uint8_t unread;
  uint32_t last_activity;         // millis() of the last message
};

enum KMsgStatus : uint8_t { MSG_IN, MSG_OUT, MSG_OUT_PENDING, MSG_OUT_DELIVERED, MSG_OUT_FAILED };

struct KMsg {
  uint16_t id;                    // 0 = empty slot
  int8_t chat;                    // index into _chats
  uint8_t status;                 // KMsgStatus
  char text[KISS_TEXT_MAX + 1];   // incoming channel text keeps its "sender: " prefix
};

// A DM request we sent, waiting for "DM accepted" from that name.
struct KHandshake {
  char name[32];                  // empty = free
  unsigned long expires;
};

struct KOutgoingDM {
  bool active;
  uint16_t msg_id;
  uint8_t pub_key[PUB_KEY_SIZE];
  uint32_t timestamp;
  uint8_t attempt;
  uint32_t acks[KISS_DM_ATTEMPTS];
  unsigned long deadline;
};

// A time heard from one sender, as an offset from our clock when heard.
struct KClockSample {
  uint32_t source;                // hash of the sender's name; 0 = empty
  int32_t offset;                 // their time - our time, seconds
  unsigned long at;               // millis() when heard
};
#define KISS_CLOCK_SAMPLES  8

enum class KScreen : uint8_t { LIST, CHAT, PICK, MENU, NAME, ABOUT, PROMPT };
enum class KPrompt : uint8_t { NONE, CONFIRM_REQUEST, INCOMING_REQUEST, KEEP_OFF };

class UITask : public AbstractUITask {
  DisplayDriver* _display = NULL;
  SensorManager* _sensors = NULL;
  NodePrefs* _node_prefs = NULL;
#ifdef PIN_BUZZER
  genericBuzzer buzzer;
#endif
  // vibration: short pulses (the stock GenericVibration runs for 5 s)
  uint8_t _vib_pulses = 0;        // pulses still to start
  bool _vib_on = false;
  unsigned long _vib_next = 0;
  CardKB _kb;

  KChat _chats[KISS_MAX_CHATS];
  int _num_chats = 0;
  KMsg _msgs[KISS_MAX_MSGS];
  int _msg_head = 0;              // next slot to write
  uint16_t _next_msg_id = 1;
  KHandshake _handshakes[KISS_HANDSHAKES];
  KOutgoingDM _out;
  KClockSample _clock[KISS_CLOCK_SAMPLES];

  KScreen _screen = KScreen::LIST;
  KScreen _prompt_return = KScreen::LIST;
  KPrompt _prompt = KPrompt::NONE;
  char _prompt_name[32];
  uint8_t _prompt_key[PUB_KEY_SIZE];
  uint8_t _prompt_ch_idx = 0;

  int _list_sel = 0, _list_top = 0;
  int _chat = -1;                 // open chat
  int _scroll = 0;                // chat lines scrolled up from the bottom
  int _menu_sel = 0;
  char _pick_names[8][32];
  int _num_pick = 0, _pick_sel = 0;
  char _compose[KISS_COMPOSE_MAX + 1];
  int _compose_len = 0;

  char _alert[48];
  unsigned long _alert_until = 0;
  bool _dirty = true;
  unsigned long _next_refresh = 0, _auto_off = 0, _next_kb_poll = 0, _next_rebuild = 0;

  // saved messages ("Keep msgs" in the menu)
  bool _keep_msgs = true;
  bool _store_dirty = false;      // messages changed since the last save
  unsigned long _store_due = 0;
  void loadKeepPref();
  void saveKeepPref();
  void loadMessages();
  void saveMessages();
  void messagesChanged();
  void clearMessages();

  // chats + messages
  void rebuildChats();
  int findChannelChat(uint8_t ch_idx) const;
  int findDirectChat(const uint8_t* pub_key) const;
  KMsg* addMsg(int chat, uint8_t status, const char* text);
  KMsg* findMsg(uint16_t id);
  void markActivity(int chat, bool incoming);

  // sending
  int composeLimit() const;
  void sendCompose();
  bool sendChannelText(uint8_t ch_idx, const char* text);
  void startDM(const ContactInfo& c, const char* text);
  void sendDMAttempt();
  void checkOutgoing();

  // DM request handshake
  ContactInfo* findContactByName(const char* name);
  KHandshake* findHandshake(const char* name);
  void requestDM(const char* name, uint8_t ch_idx);
  void acceptDM();
  bool addContactFromKey(const char* name, const uint8_t* pub_key);
  void handleChannelControl(const char* sender, const char* body, uint8_t ch_idx);

  // input + display
  void handleKey(int key);
  void keyList(int key);
  void keyChat(int key);
  void keyPick(int key);
  void keyMenu(int key);
  void keyName(int key);
  void keyPrompt(int key);
  bool composeKey(int key);
  void openChat(int idx);
  void showPrompt(KPrompt p, const char* name, uint8_t ch_idx);
  void showAlert(const char* text, int millis);
  void wake();
  void render();
  void drawTitle(const char* left, const char* right);
  void drawList();
  void drawChat();
  void drawPick();
  void drawMenu();
  void drawName();
  void drawAbout();
  void drawPrompt();
  void drawCompose(int y);
  void syncClock(const char* source, uint32_t their_time);
  bool isDefaultName() const;
  void vibrate(uint8_t pulses);
  void vibrationLoop();

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
