#pragma once

// T-Deck 320x240 ST7789 for the Deck Messenger UI. Drawing goes to a
// full-screen RGB565 canvas (in PSRAM) and push() sends it in one go, so
// screens redraw without flicker. Implements DisplayDriver only so the
// companion's main.cpp can start it and show "Loading..."; the UI draws on
// gfx() directly.

// Force-included into every file the build compiles (target.cpp needs the
// class), so skip C files and libraries, which lack the project include path.
#if defined(__cplusplus) && __has_include(<helpers/ui/DisplayDriver.h>)

#include <helpers/ui/DisplayDriver.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#define DECK_W 320
#define DECK_H 240

class DeckDisplay : public DisplayDriver {
  SPIClass _spi;
  Adafruit_ST7789 _tft;
  GFXcanvas16* _canvas = NULL;
  bool _on = false;
  bool _started = false;
  uint16_t _color = 0xFFFF;

public:
  DeckDisplay();
  bool begin();

  GFXcanvas16* gfx() { return _canvas; }
  void push();

  bool isOn() override { return _on; }
  void turnOn() override;
  void turnOff() override;
  void clear() override;
  void startFrame(ColorVal bkg = UIColor::window_bkg) override;
  void setTextSize(int sz) override;
  void setColor(ColorVal c) override;
  void setCursor(int x, int y) override;
  void print(const char* str) override;
  void fillRect(int x, int y, int w, int h) override;
  void drawRect(int x, int y, int w, int h) override;
  void drawXbm(int x, int y, const uint8_t* bits, int w, int h) override { }
  uint16_t getTextWidth(const char* str) override;
  void endFrame() override { push(); }
};

#endif
