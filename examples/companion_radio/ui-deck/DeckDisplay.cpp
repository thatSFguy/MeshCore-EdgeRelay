#include "DeckDisplay.h"

// Same bus setup as upstream's ST7789LCDDisplay on the T-Deck: its own HSPI
// instance on the pins it shares with the radio (each has its own CS).
DeckDisplay::DeckDisplay() : DisplayDriver(DECK_W, DECK_H), _spi(HSPI), _tft(&_spi, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST) { }

bool DeckDisplay::begin() {
  if (!_started) {
    _canvas = new GFXcanvas16(DECK_W, DECK_H);   // 150 KB, lands in PSRAM
    if (_canvas == NULL || _canvas->getBuffer() == NULL) return false;
    _spi.begin(PIN_TFT_SCL, -1, PIN_TFT_SDA, PIN_TFT_CS);
    _tft.init(240, 320);
    _tft.setRotation(3);
    _tft.setSPISpeed(40000000);
    _tft.fillScreen(0);
    _started = true;
  }
  turnOn();
  return true;
}

void DeckDisplay::push() {
  if (!_on || _canvas == NULL) return;
  _tft.drawRGBBitmap(0, 0, _canvas->getBuffer(), DECK_W, DECK_H);
}

void DeckDisplay::turnOn() {
  if (_on) return;
  if (_started) _tft.enableSleep(false);
  pinMode(PIN_TFT_LEDA_CTL, OUTPUT);
  digitalWrite(PIN_TFT_LEDA_CTL, HIGH);
  _on = true;
}

void DeckDisplay::turnOff() {
  if (!_on) return;
  digitalWrite(PIN_TFT_LEDA_CTL, LOW);
  _tft.enableSleep(true);
  _on = false;
}

void DeckDisplay::clear() { if (_canvas) _canvas->fillScreen(0); }

void DeckDisplay::startFrame(ColorVal bkg) {
  if (!_canvas) return;
  _canvas->fillScreen(0x0000);
  _canvas->setFont(NULL);
  _canvas->setTextSize(1);
  _canvas->setTextColor(_color = 0xFFFF);
}

void DeckDisplay::setTextSize(int sz) { if (_canvas) _canvas->setTextSize(sz); }
void DeckDisplay::setColor(ColorVal c) { _color = c ? 0xFFFF : 0x0000; if (_canvas) _canvas->setTextColor(_color); }
void DeckDisplay::setCursor(int x, int y) { if (_canvas) _canvas->setCursor(x * DECK_W / 128, y * DECK_H / 64); }
void DeckDisplay::print(const char* str) { if (_canvas) _canvas->print(str); }
void DeckDisplay::fillRect(int x, int y, int w, int h) { if (_canvas) _canvas->fillRect(x, y, w, h, _color); }
void DeckDisplay::drawRect(int x, int y, int w, int h) { if (_canvas) _canvas->drawRect(x, y, w, h, _color); }

// main.cpp centres "Loading..." in 128x64 coordinates
uint16_t DeckDisplay::getTextWidth(const char* str) {
  if (!_canvas) return 0;
  int16_t x1, y1;
  uint16_t w, h;
  _canvas->getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
  return w * 128 / DECK_W;
}
