#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <LilyGoTLoraBoard.h>
#include <helpers/radiolib/CustomSX1276Wrapper.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/sensors/EnvironmentSensorManager.h>
#ifdef DISPLAY_CLASS
  #include <helpers/ui/SSD1306Display.h>
  #include <helpers/ui/MomentaryButton.h>
  #ifdef PIN_USER_BTN_DEBOUNCE_MS
    #include "DebouncedButton.h"
  #endif
#endif

extern LilyGoTLoraBoard board;
extern WRAPPER_CLASS radio_driver;
extern AutoDiscoverRTCClock rtc_clock;
extern EnvironmentSensorManager sensors;

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  #ifdef PIN_USER_BTN_DEBOUNCE_MS
    extern DebouncedButton user_btn;
  #else
    extern MomentaryButton user_btn;
  #endif
#endif

bool radio_init();
mesh::LocalIdentity radio_new_identity();