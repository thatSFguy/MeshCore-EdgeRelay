#pragma once

#include <helpers/ui/MomentaryButton.h>

// MomentaryButton (upstream) has no debounce: fine for the onboard BOOT button
// (GPIO 0, RC-filtered on the board), but a bare switch bounces, and each
// bounce counts as a click, turning one press into a double or triple click.
//
// This only lets MomentaryButton::check() run once the pin has held the same
// level for debounce_ms, so the base class never sees the bounces. While the
// level is settling, check() reports no event; any pending multi-click
// decision is just delayed by up to debounce_ms.
class DebouncedButton : public MomentaryButton {
  uint16_t _debounce_ms;
  int8_t _raw;
  unsigned long _raw_changed_at;

public:
  DebouncedButton(int8_t pin, int long_press_millis, bool reverse, bool pulldownup, uint16_t debounce_ms)
    : MomentaryButton(pin, long_press_millis, reverse, pulldownup),
      _debounce_ms(debounce_ms), _raw(-1), _raw_changed_at(0) { }

  int check(bool repeat_click=false) override {
    int8_t pin = getPin();
    if (pin < 0) return BUTTON_EVENT_NONE;
    int8_t level = digitalRead(pin);
    unsigned long now = millis();
    if (level != _raw) {
      _raw = level;
      _raw_changed_at = now;
    }
    if (now - _raw_changed_at < _debounce_ms) return BUTTON_EVENT_NONE;  // still settling
    return MomentaryButton::check(repeat_click);
  }
};
