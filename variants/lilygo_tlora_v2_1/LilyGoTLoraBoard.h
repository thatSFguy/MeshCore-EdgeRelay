#pragma once

#include <Arduino.h>
#include <helpers/ESP32Board.h>

// LILYGO T-LoRa V2.1-1.6 board with SX1276
class LilyGoTLoraBoard : public ESP32Board {
public:
  const char* getManufacturerName() const override {
    return "LILYGO T-LoRa V2.1-1.6";
  }

  uint16_t getBattMilliVolts() override {
    analogReadResolution(12);

    uint32_t raw = 0;
    for (int i = 0; i < 8; i++) {
      raw += analogReadMilliVolts(PIN_VBAT_READ);
    }
    raw = raw / 8;

    return (2 * raw);
  }

  uint32_t getIRQGpio() override {
    return P_LORA_DIO_0; // default for SX1276
  }
#if defined(GPS_PIN_A) && defined(GPS_PIN_B)
  void begin() {
    ESP32Board::begin();
    detectGpsPins();
  }

private:
  // The GPS module's TX line toggles with NMEA output (about once a second at
  // 9600 baud); its RX line just idles. Watch both pins and take the one that
  // toggles as the module's TX. With no data (no GPS, or it is silent), keep
  // the default: A = module TX. Bit time at 9600 baud is ~104 us, far longer
  // than one poll, so no edges are missed.
  void detectGpsPins() {
    pinMode(GPS_PIN_A, INPUT_PULLUP);
    pinMode(GPS_PIN_B, INPUT_PULLUP);
    int last_a = digitalRead(GPS_PIN_A), last_b = digitalRead(GPS_PIN_B);
    uint32_t edges_a = 0, edges_b = 0;
    const uint32_t EDGES_NEEDED = 40;   // a few NMEA characters
    unsigned long start = millis();
    while (millis() - start < 3000 && edges_a < EDGES_NEEDED && edges_b < EDGES_NEEDED) {
      int a = digitalRead(GPS_PIN_A), b = digitalRead(GPS_PIN_B);
      if (a != last_a) { edges_a++; last_a = a; }
      if (b != last_b) { edges_b++; last_b = b; }
    }
    if (edges_b >= EDGES_NEEDED && edges_b > edges_a) {
      tlora_gps_tx_pin = GPS_PIN_B;
      tlora_gps_rx_pin = GPS_PIN_A;
    } else {
      tlora_gps_tx_pin = GPS_PIN_A;
      tlora_gps_rx_pin = GPS_PIN_B;
    }
    Serial.printf("GPS pins: module TX on GPIO %d, module RX on GPIO %d (%s; edges %d:%lu %d:%lu)\n",
                  tlora_gps_tx_pin, tlora_gps_rx_pin,
                  (edges_a >= EDGES_NEEDED || edges_b >= EDGES_NEEDED) ? "detected" : "no data, default",
                  GPS_PIN_A, (unsigned long)edges_a, GPS_PIN_B, (unsigned long)edges_b);
  }
#endif
};