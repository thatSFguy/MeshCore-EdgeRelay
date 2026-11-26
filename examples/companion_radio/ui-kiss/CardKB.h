#pragma once

#include <Arduino.h>
#include <Wire.h>

// M5Stack CardKB: I2C keyboard at 0x5F. Each read returns the next key code,
// or 0 when no key is waiting. Printable keys arrive as ASCII (Sym/shift
// handled on the keyboard itself); the special keys use the codes below.
#define CARDKB_ADDR       0x5F

#define KEY_KB_BACKSPACE  0x08
#define KEY_KB_TAB        0x09
#define KEY_KB_ENTER      0x0D
#define KEY_KB_ESC        0x1B
#define KEY_KB_LEFT       0xB4
#define KEY_KB_UP         0xB5
#define KEY_KB_DOWN       0xB6
#define KEY_KB_RIGHT      0xB7

class CardKB {
  TwoWire* _wire = NULL;
  bool _present = false;
  unsigned long _next_probe = 0;

  bool probe() {
    _wire->beginTransmission(CARDKB_ADDR);
    return _wire->endTransmission() == 0;
  }

public:
  // The CardKB shares the display's I2C bus, which the board already started.
  void begin(TwoWire& wire) {
    _wire = &wire;
    _present = probe();
    _next_probe = millis() + 5000;
  }

  bool isPresent() const { return _present; }

  // Returns a key code, or -1 if none. Re-probes every 5 s while absent, so a
  // keyboard that was unplugged or slow to power up is picked up later.
  int read() {
    if (_wire == NULL) return -1;
    if (!_present) {
      if ((long)(millis() - _next_probe) < 0) return -1;
      _next_probe = millis() + 5000;
      _present = probe();
      if (!_present) return -1;
    }
    if (_wire->requestFrom((uint8_t)CARDKB_ADDR, (uint8_t)1) != 1) {
      _present = false;   // stopped answering
      return -1;
    }
    int c = _wire->read();
    return c > 0 ? c : -1;
  }
};
