#pragma once

// Regional radio presets, copied from the MeshCore phone app
// (shared/.../protocol/RadioPresets.kt, itself from the MeshCore Open
// reference client). What local communities run, not legal advice: the
// picker shows the app's caveat. Grouped as the app's picker groups them.

struct DeckPreset {
  const char* name;
  float freq;      // MHz
  float bw;        // kHz
  uint8_t sf, cr;
  int8_t tx;       // dBm
  uint8_t group;   // index into PRESET_GROUPS
};

static const char* PRESET_GROUPS[] = {
  "North America", "Europe", "Australia", "New Zealand", "Other", "Russia", "Off-grid (no repeaters)",
};
#define NUM_PRESET_GROUPS 7

static const char* PRESET_CAVEAT =
  "These are what local communities run, not legal advice. Frequency, duty cycle "
  "and transmit power are regulated where you are - check before you transmit.";

static const DeckPreset PRESETS[] = {
  { "USA/Canada",                  910.525f, 62.5f,  7, 5, 20, 0 },
  { "USA Arizona",                 908.205f, 62.5f, 10, 5, 20, 0 },
  { "EU/UK (Narrow)",              869.618f, 62.5f,  8, 5, 14, 1 },
  { "EU/UK (Medium Range)",        869.525f, 250.0f, 10, 5, 14, 1 },
  { "EU/UK (Long Range)",          869.525f, 250.0f, 11, 5, 14, 1 },
  { "EU 433MHz",                   433.65f,  250.0f, 11, 5, 20, 1 },
  { "Czech Republic",              869.432f, 62.5f,  7, 5, 14, 1 },
  { "Portugal 433",                433.375f, 62.5f,  9, 5, 20, 1 },
  { "Portugal 869",                869.618f, 62.5f,  7, 5, 14, 1 },
  { "Switzerland",                 869.618f, 62.5f,  8, 5, 14, 1 },
  { "Australia",                   915.8f,   250.0f, 10, 5, 20, 2 },
  { "Australia (Narrow)",          916.575f, 62.5f,  7, 5, 20, 2 },
  { "Australia (Mid)",             915.075f, 125.0f, 9, 5, 20, 2 },
  { "Australia SA, WA, QLD",       923.125f, 62.5f,  8, 5, 20, 2 },
  { "New Zealand",                 917.375f, 250.0f, 11, 5, 20, 3 },
  { "New Zealand (Narrow)",        917.375f, 62.5f,  7, 5, 20, 3 },
  { "Vietnam",                     920.25f,  250.0f, 11, 5, 20, 4 },
  { "Russia Artyom (VVO)",         864.281f, 62.5f,  8, 6, 20, 5 },
  { "Russia Biysk (BSK)",          869.0f,   62.5f,  8, 5, 20, 5 },
  { "Russia Chelyabinsk (CEK)",    868.731f, 62.5f,  8, 6, 20, 5 },
  { "Russia Cherepovets (CEE)",    868.57f,  62.5f,  7, 8, 20, 5 },
  { "Russia Irkutsk (IKT)",        868.731f, 62.5f,  7, 7, 20, 5 },
  { "Russia Ivanovo (IWA)",        868.731f, 62.5f,  8, 8, 20, 5 },
  { "Russia Izhevsk (IJK)",        868.732f, 62.5f,  8, 8, 20, 5 },
  { "Russia Kaluga (KLF)",         868.731f, 62.5f,  7, 7, 20, 5 },
  { "Russia Kazan (KZN)",          868.731f, 62.5f,  8, 6, 20, 5 },
  { "Russia Khabarovsk (KHV)",     864.281f, 62.5f,  8, 6, 20, 5 },
  { "Russia Kirov (KVX)",          868.731f, 62.5f,  8, 8, 20, 5 },
  { "Russia Lipetsk (LPK)",        868.95f,  62.5f,  9, 7, 20, 5 },
  { "Russia Moscow (MOW)",         868.731f, 62.5f,  7, 7, 20, 5 },
  { "Russia Nizhny Novgorod (GOJ)",868.731f, 62.5f,  8, 6, 20, 5 },
  { "Russia Novosibirsk (OVB)",    869.0f,   62.5f,  9, 8, 20, 5 },
  { "Russia Rostov-on-Don (ROV)",  868.731f, 62.5f,  9, 7, 20, 5 },
  { "Russia Ryazan (RZN)",         868.88f,  62.5f,  9, 5, 20, 5 },
  { "Russia Samara (KUF)",         864.281f, 62.5f,  8, 7, 20, 5 },
  { "Russia Saratov (GSV)",        864.281f, 62.5f,  8, 7, 20, 5 },
  { "Russia St. Petersburg (LED)", 868.856f, 62.5f,  7, 7, 20, 5 },
  { "Russia Tambov (TBW)",         868.95f,  62.5f, 10, 5, 20, 5 },
  { "Russia Tula (TYA)",           868.731f, 62.5f,  8, 7, 20, 5 },
  { "Russia Tver (KLD)",           869.169f, 62.5f,  8, 8, 20, 5 },
  { "Russia Ufa (UFA)",            868.732f, 62.5f,  8, 8, 20, 5 },
  { "Russia Volgograd (VOG)",      869.525f, 62.5f,  7, 7, 20, 5 },
  { "Russia Voronezh (VOZ)",       868.731f, 62.5f,  8, 6, 20, 5 },
  { "Russia Yekaterinburg (SVX)",  869.046f, 62.5f,  7, 7, 20, 5 },
  { "Off-Grid 433",                433.0f,   250.0f, 11, 8, 20, 6 },
  { "Off-Grid 869",                869.0f,   250.0f, 11, 8, 14, 6 },
  { "Off-Grid 918",                918.0f,   250.0f, 11, 8, 20, 6 },
};
#define NUM_PRESETS (int)(sizeof(PRESETS) / sizeof(PRESETS[0]))

// the preset these radio settings match, or -1 (several Russian city presets
// share settings; the first match wins, it only labels the current state)
static int matchPreset(float freq, float bw, uint8_t sf, uint8_t cr) {
  long f = lroundf(freq * 1000), b = lroundf(bw * 1000);
  for (int i = 0; i < NUM_PRESETS; i++) {
    if (lroundf(PRESETS[i].freq * 1000) == f && lroundf(PRESETS[i].bw * 1000) == b && PRESETS[i].sf == sf && PRESETS[i].cr == cr) return i;
  }
  return -1;
}
