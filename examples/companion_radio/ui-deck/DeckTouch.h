#pragma once

// T-Deck touch panel: a GT911 on the shared I2C bus (0x5D, or 0x14
// depending on how it was strapped at power-up), polled. Reports a tap
// (touch down and up without moving) or a vertical drag in screen
// coordinates (320x240, the display's rotation).

#include <Arduino.h>
#include <Wire.h>

#define GT911_ADDR_A    0x5D
#define GT911_ADDR_B    0x14
#define GT911_STATUS    0x814E
#define GT911_POINT1    0x8150
#define TOUCH_POLL_MS   20
#define TOUCH_SLOP      10        // px of movement before a touch becomes a drag

enum DeckTouchEvent { TOUCH_NONE, TOUCH_TAP, TOUCH_DRAG };

class DeckTouch {
  TwoWire* _wire = NULL;
  uint8_t _addr = 0;
  unsigned long _next_poll = 0, _next_probe = 0;
  bool _down = false, _dragging = false;
  int _start_x = 0, _start_y = 0, _last_y = 0;

  bool readReg(uint16_t reg, uint8_t* buf, uint8_t len) {
    _wire->beginTransmission(_addr);
    _wire->write(reg >> 8);
    _wire->write(reg & 0xFF);
    if (_wire->endTransmission() != 0) return false;
    if (_wire->requestFrom(_addr, len) != len) return false;
    for (int i = 0; i < len; i++) buf[i] = _wire->read();
    return true;
  }

  void writeReg(uint16_t reg, uint8_t v) {
    _wire->beginTransmission(_addr);
    _wire->write(reg >> 8);
    _wire->write(reg & 0xFF);
    _wire->write(v);
    _wire->endTransmission();
  }

  bool probe(uint8_t a) {
    _wire->beginTransmission(a);
    return _wire->endTransmission() == 0;
  }

  // panel coordinates -> screen (panel is 240x320 portrait; the display is
  // rotated to 320x240 landscape)
  void toScreen(int px, int py, int& x, int& y) {
#ifdef TOUCH_RAW
    x = px; y = py;
#else
    x = py;
    y = 239 - px;
    if (x < 0) x = 0;
    if (x > 319) x = 319;
    if (y < 0) y = 0;
    if (y > 239) y = 239;
#endif
  }

public:
  int x = 0, y = 0;     // TAP: where; DRAG: current point
  int dy = 0;           // DRAG: movement since the last event (+ = finger moved down)
  int raw_x = 0, raw_y = 0;

  void begin(TwoWire& wire) {
    _wire = &wire;
    _addr = probe(GT911_ADDR_A) ? GT911_ADDR_A : probe(GT911_ADDR_B) ? GT911_ADDR_B : 0;
    _next_probe = millis() + 3000;
  }

  bool present() const { return _addr != 0; }

  DeckTouchEvent read() {
    if (_wire == NULL || (long)(millis() - _next_poll) < 0) return TOUCH_NONE;
    _next_poll = millis() + TOUCH_POLL_MS;
    if (_addr == 0) {
      if ((long)(millis() - _next_probe) < 0) return TOUCH_NONE;
      _next_probe = millis() + 3000;
      begin(*_wire);
      if (_addr == 0) return TOUCH_NONE;
    }
    uint8_t status;
    if (!readReg(GT911_STATUS, &status, 1)) return TOUCH_NONE;
    if (!(status & 0x80)) return TOUCH_NONE;   // no new data yet
    int points = status & 0x0F;
    uint8_t p[4] = { 0, 0, 0, 0 };
    if (points > 0) readReg(GT911_POINT1, p, 4);
    writeReg(GT911_STATUS, 0);

    if (points > 0) {
      raw_x = p[0] | (p[1] << 8);
      raw_y = p[2] | (p[3] << 8);
      int sx, sy;
      toScreen(raw_x, raw_y, sx, sy);
      if (!_down) {
        _down = true;
        _dragging = false;
        _start_x = sx; _start_y = sy; _last_y = sy;
        return TOUCH_NONE;
      }
      if (!_dragging && (abs(sy - _start_y) > TOUCH_SLOP || abs(sx - _start_x) > TOUCH_SLOP)) _dragging = true;
      if (_dragging && sy != _last_y) {
        x = sx; y = sy;
        dy = sy - _last_y;
        _last_y = sy;
        return TOUCH_DRAG;
      }
      return TOUCH_NONE;
    }
    // released
    bool was_tap = _down && !_dragging;
    _down = false;
    if (was_tap) {
      x = _start_x; y = _start_y;
      return TOUCH_TAP;
    }
    return TOUCH_NONE;
  }
};
