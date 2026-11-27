#pragma once

// Short alert tones on the T-Deck speaker (I2S amplifier). Non-blocking:
// loop() tops up the DMA buffers with as much of the tune as fits. The I2S
// clock runs only while a tune plays: left running, it couples into the
// trackball's sensor lines (seen: GPIO 2 toggling ~30k times a second).

#include <Arduino.h>
#include <driver/i2s.h>

#define SND_PIN_BCK   7
#define SND_PIN_WS    5
#define SND_PIN_DOUT  6
#define SND_RATE      16000
#define SND_PORT      I2S_NUM_0
#define SND_MAX_NOTES 6

struct DeckNote { uint16_t freq; uint16_t ms; };   // freq 0 = rest

class DeckSound {
  bool _ok = false;
  bool _running = false;
  DeckNote _notes[SND_MAX_NOTES];
  int _count = 0, _idx = 0;
  uint32_t _left = 0;        // samples left in the current note
  uint32_t _phase = 0;       // 16.16 fixed point
  uint32_t _step = 0;
  uint32_t _tail = 0;        // silence samples to flush the DMA buffers
  int16_t _volume = 2500;

  void startNote() {
    _left = (uint32_t)_notes[_idx].ms * SND_RATE / 1000;
    _step = ((uint32_t)_notes[_idx].freq << 16) / SND_RATE;
    _phase = 0;
  }

public:
  void begin() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = SND_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.dma_buf_count = 4;
    cfg.dma_buf_len = 256;
    cfg.tx_desc_auto_clear = true;   // silence when we stop feeding it
    if (i2s_driver_install(SND_PORT, &cfg, 0, NULL) != ESP_OK) return;
    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = SND_PIN_BCK;
    pins.ws_io_num = SND_PIN_WS;
    pins.data_out_num = SND_PIN_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    _ok = i2s_set_pin(SND_PORT, &pins) == ESP_OK;
    i2s_zero_dma_buffer(SND_PORT);
    i2s_stop(SND_PORT);
  }

  void play(const DeckNote* notes, int count) {
    if (!_ok || count <= 0) return;
    if (count > SND_MAX_NOTES) count = SND_MAX_NOTES;
    memcpy(_notes, notes, count * sizeof(DeckNote));
    _count = count;
    _idx = 0;
    _tail = 0;
    startNote();
    if (!_running) {
      i2s_start(SND_PORT);
      _running = true;
    }
  }

  bool isPlaying() const { return _count > 0 || _tail > 0; }

  void loop() {
    if (!_ok) return;
    if (!isPlaying()) {
      if (_running) {
        i2s_stop(SND_PORT);
        _running = false;
      }
      return;
    }
    int16_t buf[128 * 2];
    for (int chunk = 0; chunk < 8; chunk++) {
      int n = 0;
      while (n < 128) {
        int16_t s = 0;
        if (_count > 0) {
          if (_left == 0) {
            if (++_idx >= _count) { _count = 0; _tail = 1024; continue; }
            startNote();
            continue;
          }
          if (_notes[_idx].freq) s = (_phase & 0x8000) ? _volume : -_volume;   // square wave
          _phase += _step;
          _left--;
        } else if (_tail > 0) {
          _tail--;
        } else {
          break;
        }
        buf[n * 2] = buf[n * 2 + 1] = s;
        n++;
      }
      if (n == 0) return;
      size_t written = 0;
      i2s_write(SND_PORT, buf, n * 4, &written, 0);
      if (written < (size_t)n * 4) {
        // DMA full: put back what didn't fit (only matters mid-note; a click at worst)
        uint32_t unsent = (n * 4 - written) / 4;
        if (_count > 0) _left += unsent;
        return;
      }
    }
  }
};
