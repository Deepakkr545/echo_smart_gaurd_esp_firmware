/*
  =====================================================================
  storage.h — EEPROM Storage Module (public interface)
  =====================================================================
  DESIGN PRINCIPLE:
  ALL persistent settings live in ONE struct (Settings). Every future
  module (WiFi, calibration, alarm, web dashboard) reads/writes its data
  through Storage::load()/Storage::save(), never touching EEPROM.h
  directly. This means:
    - One place to see everything that persists across reboot
    - One place to bump a version number if the struct layout changes
    - No risk of two modules accidentally writing overlapping EEPROM
      addresses

  STEP 4 SCOPE:
  Define the FULL settings struct now (covering WiFi, calibration, alarm,
  etc. per the project spec), but only actually exercise `bootCount` in
  this step — a simple field we can increment and verify survives a
  reboot. Later steps will populate the other fields as those features
  are built, without needing to change this file's structure again.
  =====================================================================
*/

#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>

// -----------------------------------------------------------------------
// Settings struct — the single source of truth for everything persisted.
//
// IMPORTANT: If you add/remove/reorder fields after devices are already
// deployed with saved settings, old EEPROM data will fail the checksum
// check (safely falling back to defaults) rather than being misread as
// the wrong field — see STORAGE_VERSION below.
// -----------------------------------------------------------------------
struct Settings {
  char     magic[4];          // Fixed signature, e.g. "SSS1" — proves this
                               // EEPROM region was written by our firmware.
  uint8_t  version;           // Struct layout version (bump if fields change).

  // --- WiFi ---
  char     wifiSSID[32];
  char     wifiPassword[64];

  // --- Sensor / calibration ---
  float    triggerDistanceCm; // Distance threshold that counts as "intruder"
  float    wallDistanceCm;    // Calibrated far-wall distance (Step 6+)
  uint32_t lastCalibrationEpoch; // Unix time of last successful calibration

  // --- Alarm ---
  bool     alarmEnabled;
  uint16_t alarmDurationSec;

  // --- Modes / schedule ---
  bool     autoArm;
  bool     nightMode;
  int8_t   timezoneOffsetHours;

  // --- Diagnostics (Step 4 test field) ---
  uint32_t bootCount;

  // --- Runtime state that must survive power loss (added v2) ---
  bool     armed;
  bool     oledOn;

  // --- Telegram credentials, editable from dashboard (added v3) ---
  char     telegramBotToken[48];
  char     telegramChatId[16];

  // --- Buzzer timing / sustained-activity detection (added v4) ---
  bool     buzzerMasterEnabled;      // Hardware mute — silences buzzer only (Telegram alerts unaffected)
  uint16_t longTermBuzzerDurationSec; // Duration of the pulsed "sustained activity" buzzer pattern
  uint16_t sustainedThresholdSec;     // Continuous in-zone seconds before firing sustained alert

  // --- Multi-device identity + remote buzzer IP (added v5) ---
  char     buzzerDeviceIp[16];
  char     deviceName[32];
  char     deviceId[24];

  // --- Dashboard login (added v6) ---
  char     dashboardUsername[20];
  char     dashboardPassword[24];

  // --- Master dashboard: other devices to poll (added v7) ---
  // Format: "Name|IP|Username|Password;Name2|IP2|Username2|Password2"
  char     siblingDevices[220];

  // --- Buzzer pattern selection (added v8) ---
  // 1=Continuous, 2=Slow Pulse, 3=Fast Pulse, 4=Double-Beep Burst
  uint8_t  shortTermBuzzerPattern;
  uint8_t  longTermBuzzerPattern;

  // --- Multi-buzzer support (added v9) ---
  // Up to 5 buzzer units this sensor can alert. Empty string = unused slot.
  // buzzerDeviceIp (v5, above) stays as slot 0 for backward compatibility.
  char     buzzerIps[5][16];

  // --- Diagnostics: on/off history (added v9) ---
  // Requires NTP time sync to be meaningful (epoch=0 means "unknown").
  uint32_t sessionStartEpoch;   // When THIS boot session began (0 until NTP syncs)
  uint32_t lastAliveEpoch;      // Updated periodically while running (~5 min)
  uint32_t historyStart[5];     // Ring buffer: past sessions' start times
  uint32_t historyEnd[5];       // Ring buffer: past sessions' end times
  uint8_t  historyCount;        // Total sessions ever recorded (index = count % 5)

  // --- OLED reading-screen skin (added v10) ---
  // Selects which visual style showReadingScreen() draws — see the
  // Display::Skin enum in display.h for the full list. Purely cosmetic;
  // never affects sensing/alarm logic.
  uint8_t  displaySkin;

  // --- Current mode (added v11) ---
  // "off" | "home" | "red_alert" | "test" — set whenever the app
  // applies a mode via /setmode. This is the SINGLE SOURCE OF TRUTH
  // for "which mode is this device in" — the app no longer relies on
  // its own local storage to know this, so a fresh phone install (or
  // a device that was powered off during a mode change) always
  // reports its own real current mode rather than the app guessing.
  char     currentMode[16];

  // --- Notification category toggle (added v12) ---
  // Security alerts (intrusion, sustained activity, sensor health) are
  // NEVER gated by this — only everything else (online/offline, mode
  // changes, system/config events) respects it. Lets a user turn down
  // routine notification noise without risking missing a real alert.
  bool     notifyOtherEnabled;

  // --- Configurable Night Mode schedule (added v13) ---
  // Replaces the old hardcoded NIGHT_START_HOUR/NIGHT_END_HOUR
  // #defines. 0-23, 24-hour format. Same wraparound rule as before:
  // if start > end, the window crosses midnight (e.g. 22 -> 7).
  uint8_t  nightStartHour;
  uint8_t  nightEndHour;

  // --- Extra Telegram recipients (added v14) ---
  // telegramChatId (above) is always recipient #1. These two are
  // optional additional phones/chats — empty string means "not set".
  // Lets a household with multiple phones all get the same alerts,
  // not just whoever's chat ID was configured first.
  char     telegramChatId2[16];
  char     telegramChatId3[16];

  // --- Entry/Exit delay (added v15) ---
  // Exit delay: seconds of grace after arming before the sensor
  // actually starts watching, so you can leave without tripping your
  // own alarm. Entry delay: seconds of grace after the FIRST movement
  // is detected while armed, before the alarm actually sounds, so you
  // can disarm on the way in. 0 = instant (old behavior) for either.
  uint16_t exitDelaySec;
  uint16_t entryDelaySec;

  // --- Sensitivity profile (added v15) ---
  // 0 = Pet-Friendly (requires more consecutive in-zone readings
  //     before firing, since a pet passing through won't hold in the
  //     beam as long as a person walking up to it), 1 = Normal
  //     (matches the original fixed behavior), 2 = High (fires on the
  //     very first reading, most responsive but most false-positive-
  //     prone). Ultrasonic sensors can't tell size/species the way a
  //     PIR sensor can, so this works by requiring more SUSTAINED
  //     presence rather than filtering by size.
  uint8_t  sensitivityProfile;

  uint16_t checksum;           // Computed over all fields above.
};

namespace Storage {

  // Initializes the EEPROM emulation layer. Call once in setup(),
  // BEFORE calling load().
  void begin();

  // Loads settings from EEPROM. If the magic number or checksum don't
  // match (first-ever boot, or corrupted flash), returns safe factory
  // defaults instead — and saves them, so the next boot has a valid
  // baseline to read.
  Settings load();

  // Saves the given settings to EEPROM, computing and storing a fresh
  // checksum, then commits to flash. Returns true on success.
  bool save(Settings &settings);

  // Resets to factory defaults and saves. Used by "Factory Reset" in
  // the web dashboard later, but also handy for testing now.
  Settings factoryReset();

}

#endif // STORAGE_H
