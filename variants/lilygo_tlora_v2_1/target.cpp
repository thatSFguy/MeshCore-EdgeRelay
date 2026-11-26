#include <Arduino.h>
#include "target.h"

LilyGoTLoraBoard board;

static SPIClass spi;
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_0, P_LORA_RESET, P_LORA_DIO_1, spi);

WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);
#if ENV_INCLUDE_GPS
  #include <helpers/sensors/MicroNMEALocationProvider.h>
  #if defined(GPS_PIN_A) && defined(GPS_PIN_B)
    // set by LilyGoTLoraBoard::begin() (see TLoraGpsPins.h)
    int8_t tlora_gps_tx_pin = GPS_PIN_A;
    int8_t tlora_gps_rx_pin = GPS_PIN_B;
  #endif
  MicroNMEALocationProvider nmea = MicroNMEALocationProvider(Serial1, &rtc_clock);
  EnvironmentSensorManager sensors = EnvironmentSensorManager(nmea);
  // Upstream (v1.18) declares LocationProvider::sendSentence() virtual but never
  // defines it, so its vtable is never emitted. Other boards get away with it;
  // with this board's -Os the base constructor keeps a vtable reference and the
  // link fails. Define it here (as a no-op, like the default it should have).
  // Remove if upstream adds a body (this then fails as a redefinition).
  void LocationProvider::sendSentence(const char* sentence) { }
#else
  EnvironmentSensorManager sensors;
#endif

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  #ifdef PIN_USER_BTN_PULLUP
    MomentaryButton user_btn(PIN_USER_BTN, 1000, true, true);   // active low, internal pull-up
  #else
    MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
  #endif
#endif

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);

#if defined(P_LORA_SCLK)
  return radio.std_init(&spi);
#else
  return radio.std_init();
#endif
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}