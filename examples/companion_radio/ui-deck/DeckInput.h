#pragma once

// T-Deck input: the keyboard (an ESP32-C3 at I2C 0x55 that returns one ASCII
// byte per key, 0 when none) and the trackball (four hall sensors that toggle
// a pin as the ball rolls, plus a click on GPIO 0). Touch is in DeckTouch.h.

#include <Arduino.h>
#include <Wire.h>

#define DECK_KB_ADDR    0x55

#define TB_PIN_UP       3
#define TB_PIN_DOWN     15
#define TB_PIN_LEFT     1
#define TB_PIN_RIGHT    2
#define TB_PIN_CLICK    0

#ifndef TB_EDGES_PER_STEP
  #define TB_EDGES_PER_STEP  2      // pin toggles per reported move
#endif
#define TB_LONG_PRESS_MILLIS 700

// key codes; printable keys are ASCII
#define KEY_BACKSPACE   0x08
#define KEY_ENTER       0x0D
#define KEY_UP          0x101
#define KEY_DOWN        0x102
#define KEY_LEFT        0x103
#define KEY_RIGHT       0x104
#define KEY_CLICK       0x105
#define KEY_LONG_PRESS  0x106

class DeckInput {
  static volatile int16_t _edges[4];   // up, down, left, right
  static void isrUp();             // in UITask.cpp (IRAM code can't be inline)
  static void isrDown();
  static void isrLeft();
  static void isrRight();

  TwoWire* _wire = NULL;
  bool _kb_present = false;
  unsigned long _next_probe = 0, _next_poll = 0;
  bool _down = false, _long_sent = false;
  unsigned long _down_at = 0, _btn_change = 0;
  int16_t _acc[4] = { 0, 0, 0, 0 };

  bool probe() {
    _wire->beginTransmission(DECK_KB_ADDR);
    return _wire->endTransmission() == 0;
  }

  int readKeyboard() {
    if (_wire == NULL || (long)(millis() - _next_poll) < 0) return -1;
    _next_poll = millis() + 15;
    if (!_kb_present) {
      if ((long)(millis() - _next_probe) < 0) return -1;
      _next_probe = millis() + 3000;   // the keyboard boots after us; keep looking
      _kb_present = probe();
      if (!_kb_present) return -1;
    }
    if (_wire->requestFrom((uint8_t)DECK_KB_ADDR, (uint8_t)1) != 1) {
      _kb_present = false;
      return -1;
    }
    int c = _wire->read();
    return c > 0 ? c : -1;
  }

  int readButton() {
    bool pressed = digitalRead(TB_PIN_CLICK) == LOW;
    unsigned long now = millis();
    if (pressed != _down && now - _btn_change > 30) {   // debounce
      _btn_change = now;
      _down = pressed;
      if (pressed) {
        _down_at = now;
        _long_sent = false;
      } else if (!_long_sent) {
        return KEY_CLICK;
      }
    }
    if (_down && !_long_sent && now - _down_at >= TB_LONG_PRESS_MILLIS) {
      _long_sent = true;
      return KEY_LONG_PRESS;
    }
    return -1;
  }

  int readTrackball() {
    static const int keys[4] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };
    noInterrupts();
    for (int i = 0; i < 4; i++) { _acc[i] += _edges[i]; _edges[i] = 0; }
    interrupts();
    int best = -1;
    for (int i = 0; i < 4; i++) {
      if (_acc[i] >= TB_EDGES_PER_STEP && (best < 0 || _acc[i] > _acc[best])) best = i;
    }
    if (best < 0) return -1;
    for (int i = 0; i < 4; i++) _acc[i] = 0;   // one move at a time; drop sideways jitter
    return keys[best];
  }

public:
  void begin(TwoWire& wire) {
    _wire = &wire;
    _kb_present = probe();
    _next_probe = millis() + 3000;
    pinMode(TB_PIN_UP, INPUT_PULLUP);
    pinMode(TB_PIN_DOWN, INPUT_PULLUP);
    pinMode(TB_PIN_LEFT, INPUT_PULLUP);
    pinMode(TB_PIN_RIGHT, INPUT_PULLUP);
    pinMode(TB_PIN_CLICK, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TB_PIN_UP), isrUp, CHANGE);
    attachInterrupt(digitalPinToInterrupt(TB_PIN_DOWN), isrDown, CHANGE);
    attachInterrupt(digitalPinToInterrupt(TB_PIN_LEFT), isrLeft, CHANGE);
    attachInterrupt(digitalPinToInterrupt(TB_PIN_RIGHT), isrRight, CHANGE);
  }

  bool keyboardPresent() const { return _kb_present; }

  // next input event, or -1
  int read() {
    int k = readButton();
    if (k > 0) return k;
    k = readKeyboard();
    if (k > 0) return k;
    return readTrackball();
  }
};
