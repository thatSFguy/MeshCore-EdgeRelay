#pragma once

#include <stdint.h>

// Number of recent home-node RSSI samples averaged, and how many are needed
// before the relay may decide it is home.
#define HOME_RSSI_SAMPLES      4
#define HOME_MIN_SAMPLES       3

/**
 * "Parked at home" detector, fed with the RSSI of packets transmitted by the
 * owner's home node.
 *
 * Two thresholds give hysteresis: the average must reach `enter` to become
 * home, and stays home while it remains at or above `exit`. Leaving is either
 * an average below `exit` (driving away, signal fading) or no strong sample
 * for `timeout` (out of range entirely). Samples older than the timeout are
 * discarded so stale readings never decide the state.
 *
 * Pure logic, no I/O: the caller supplies RSSI values and millis().
 */
class HomePresence {
  int16_t _samples[HOME_RSSI_SAMPLES];
  uint8_t _num, _next;
  bool _home;
  uint32_t _last_sample_ms;
  uint32_t _last_strong_ms;
  int16_t _last_rssi;

  void clearSamples() { _num = 0; _next = 0; }

public:
  HomePresence() { reset(); }

  void reset() {
    clearSamples();
    _home = false;
    _last_sample_ms = _last_strong_ms = 0;
    _last_rssi = 0;
  }

  bool isHome() const { return _home; }
  uint8_t numSamples() const { return _num; }
  int16_t lastRssi() const { return _last_rssi; }
  uint32_t lastSampleMs() const { return _last_sample_ms; }

  int16_t average() const {
    if (_num == 0) return 0;
    int32_t sum = 0;
    for (int i = 0; i < _num; i++) sum += _samples[i];
    return (int16_t)(sum / _num);
  }

  // Record one home-node packet. Returns true if the home state changed.
  bool addSample(int16_t rssi, uint32_t now_ms, int16_t enter, int16_t exit) {
    _samples[_next] = rssi;
    _next = (_next + 1) % HOME_RSSI_SAMPLES;
    if (_num < HOME_RSSI_SAMPLES) _num++;
    _last_sample_ms = now_ms;
    _last_rssi = rssi;

    int16_t avg = average();
    if (!_home) {
      if (_num >= HOME_MIN_SAMPLES && avg >= enter) {
        _home = true;
        _last_strong_ms = now_ms;
        return true;
      }
    } else if (avg >= exit) {
      _last_strong_ms = now_ms;
    } else {
      _home = false;
      return true;
    }
    return false;
  }

  // Call periodically. Returns true if the home state changed.
  bool tick(uint32_t now_ms, uint32_t timeout_ms) {
    bool changed = false;
    if (_home && now_ms - _last_strong_ms > timeout_ms) {
      _home = false;
      changed = true;
    }
    if (_num > 0 && now_ms - _last_sample_ms > timeout_ms) {
      clearSamples();
    }
    return changed;
  }
};
