#pragma once

// Force-included (-include) by the GPS companion env only.
// The external GPS is wired to GPS_PIN_A / GPS_PIN_B, but which one is the
// module's TX is not fixed: LilyGoTLoraBoard::begin() detects it at boot and
// stores the result here. PIN_GPS_TX / PIN_GPS_RX keep MeshCore's naming (the
// GPS module's side): PIN_GPS_TX is the line the module transmits on, i.e. the
// ESP32's RX.
#include <stdint.h>

extern int8_t tlora_gps_tx_pin;   // module TX -> ESP32 RX
extern int8_t tlora_gps_rx_pin;   // module RX <- ESP32 TX

#define PIN_GPS_TX tlora_gps_tx_pin
#define PIN_GPS_RX tlora_gps_rx_pin
