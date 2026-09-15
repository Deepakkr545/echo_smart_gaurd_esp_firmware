/*
  storage.cpp — v5: added buzzerDeviceIp/deviceName/deviceId (dashboard-
  editable, persisted). Migrates from v1/v2/v3/v4 preserving everything
  — nothing is wiped on upgrade.
*/

#include "storage.h"
#include "config.h"
#include <EEPROM.h>

#define STORAGE_MAGIC        "SSS1"
#define STORAGE_VERSION      14
#define EEPROM_SIZE          880
#define EEPROM_START_ADDR    0

struct SettingsV1 {
  char magic[4]; uint8_t version;
  char wifiSSID[32]; char wifiPassword[64];
  float triggerDistanceCm; float wallDistanceCm; uint32_t lastCalibrationEpoch;
  bool alarmEnabled; uint16_t alarmDurationSec; bool autoArm; bool nightMode;
  int8_t timezoneOffsetHours; uint32_t bootCount; uint16_t checksum;
};

struct SettingsV2 {
  char magic[4]; uint8_t version;
  char wifiSSID[32]; char wifiPassword[64];
  float triggerDistanceCm; float wallDistanceCm; uint32_t lastCalibrationEpoch;
  bool alarmEnabled; uint16_t alarmDurationSec; bool autoArm; bool nightMode;
  int8_t timezoneOffsetHours; uint32_t bootCount;
  bool armed; bool oledOn;
  uint16_t checksum;
};

struct SettingsV3 {
  char magic[4]; uint8_t version;
  char wifiSSID[32]; char wifiPassword[64];
  float triggerDistanceCm; float wallDistanceCm; uint32_t lastCalibrationEpoch;
  bool alarmEnabled; uint16_t alarmDurationSec; bool autoArm; bool nightMode;
  int8_t timezoneOffsetHours; uint32_t bootCount;
  bool armed; bool oledOn;
  char telegramBotToken[48]; char telegramChatId[16];
  uint16_t checksum;
};

struct SettingsV4 {
  char magic[4]; uint8_t version;
  char wifiSSID[32]; char wifiPassword[64];
  float triggerDistanceCm; float wallDistanceCm; uint32_t lastCalibrationEpoch;
  bool alarmEnabled; uint16_t alarmDurationSec; bool autoArm; bool nightMode;
  int8_t timezoneOffsetHours; uint32_t bootCount;
  bool armed; bool oledOn;
  char telegramBotToken[48]; char telegramChatId[16];
  bool buzzerMasterEnabled; uint16_t longTermBuzzerDurationSec; uint16_t sustainedThresholdSec;
  uint16_t checksum;
};


struct SettingsV5 {
  char magic[4]; uint8_t version;
  char wifiSSID[32]; char wifiPassword[64];
  float triggerDistanceCm; float wallDistanceCm; uint32_t lastCalibrationEpoch;
  bool alarmEnabled; uint16_t alarmDurationSec; bool autoArm; bool nightMode;
  int8_t timezoneOffsetHours; uint32_t bootCount;
  bool armed; bool oledOn;
  char telegramBotToken[48]; char telegramChatId[16];
  bool buzzerMasterEnabled; uint16_t longTermBuzzerDurationSec; uint16_t sustainedThresholdSec;
  char buzzerDeviceIp[16]; char deviceName[32]; char deviceId[24];
  uint16_t checksum;
};


struct SettingsV6 {
  char magic[4]; uint8_t version;
  char wifiSSID[32]; char wifiPassword[64];
  float triggerDistanceCm; float wallDistanceCm; uint32_t lastCalibrationEpoch;
  bool alarmEnabled; uint16_t alarmDurationSec; bool autoArm; bool nightMode;
  int8_t timezoneOffsetHours; uint32_t bootCount;
  bool armed; bool oledOn;
  char telegramBotToken[48]; char telegramChatId[16];
  bool buzzerMasterEnabled; uint16_t longTermBuzzerDurationSec; uint16_t sustainedThresholdSec;
  char buzzerDeviceIp[16]; char deviceName[32]; char deviceId[24];
  char dashboardUsername[20]; char dashboardPassword[24];
  uint16_t checksum;
};


struct SettingsV7 {
  char magic[4]; uint8_t version;
  char wifiSSID[32]; char wifiPassword[64];
  float triggerDistanceCm; float wallDistanceCm; uint32_t lastCalibrationEpoch;
  bool alarmEnabled; uint16_t alarmDurationSec; bool autoArm; bool nightMode;
  int8_t timezoneOffsetHours; uint32_t bootCount;
  bool armed; bool oledOn;
  char telegramBotToken[48]; char telegramChatId[16];
  bool buzzerMasterEnabled; uint16_t longTermBuzzerDurationSec; uint16_t sustainedThresholdSec;
  char buzzerDeviceIp[16]; char deviceName[32]; char deviceId[24];
  char dashboardUsername[20]; char dashboardPassword[24];
  char siblingDevices[220];
  uint16_t checksum;
};


struct SettingsV8 {
  char magic[4]; uint8_t version;
  char wifiSSID[32]; char wifiPassword[64];
  float triggerDistanceCm; float wallDistanceCm; uint32_t lastCalibrationEpoch;
  bool alarmEnabled; uint16_t alarmDurationSec; bool autoArm; bool nightMode;
  int8_t timezoneOffsetHours; uint32_t bootCount;
  bool armed; bool oledOn;
  char telegramBotToken[48]; char telegramChatId[16];
  bool buzzerMasterEnabled; uint16_t longTermBuzzerDurationSec; uint16_t sustainedThresholdSec;
  char buzzerDeviceIp[16]; char deviceName[32]; char deviceId[24];
  char dashboardUsername[20]; char dashboardPassword[24];
  char siblingDevices[220];
  uint8_t shortTermBuzzerPattern; uint8_t longTermBuzzerPattern;
  uint16_t checksum;
};

// Settings exactly as it existed before displaySkin was added (v9) — the
// multi-buzzer buzzerIps[5][16] array and on/off diagnostics history
// fields were already present at v9, so those are the only fields this
// version has that V8 doesn't.
struct SettingsV9 {
  char magic[4]; uint8_t version;
  char wifiSSID[32]; char wifiPassword[64];
  float triggerDistanceCm; float wallDistanceCm; uint32_t lastCalibrationEpoch;
  bool alarmEnabled; uint16_t alarmDurationSec; bool autoArm; bool nightMode;
  int8_t timezoneOffsetHours; uint32_t bootCount;
  bool armed; bool oledOn;
  char telegramBotToken[48]; char telegramChatId[16];
  bool buzzerMasterEnabled; uint16_t longTermBuzzerDurationSec; uint16_t sustainedThresholdSec;
  char buzzerDeviceIp[16]; char deviceName[32]; char deviceId[24];
  char dashboardUsername[20]; char dashboardPassword[24];
  char siblingDevices[220];
  uint8_t shortTermBuzzerPattern; uint8_t longTermBuzzerPattern;
  char buzzerIps[5][16];
  uint32_t sessionStartEpoch; uint32_t lastAliveEpoch;
  uint32_t historyStart[5]; uint32_t historyEnd[5]; uint8_t historyCount;
  uint16_t checksum;
};

// Settings exactly as it existed before currentMode was added (v10) —
// identical to V9 plus the single displaySkin byte.
struct SettingsV10 {
  char magic[4]; uint8_t version;
  char wifiSSID[32]; char wifiPassword[64];
  float triggerDistanceCm; float wallDistanceCm; uint32_t lastCalibrationEpoch;
  bool alarmEnabled; uint16_t alarmDurationSec; bool autoArm; bool nightMode;
  int8_t timezoneOffsetHours; uint32_t bootCount;
  bool armed; bool oledOn;
  char telegramBotToken[48]; char telegramChatId[16];
  bool buzzerMasterEnabled; uint16_t longTermBuzzerDurationSec; uint16_t sustainedThresholdSec;
  char buzzerDeviceIp[16]; char deviceName[32]; char deviceId[24];
  char dashboardUsername[20]; char dashboardPassword[24];
  char siblingDevices[220];
  uint8_t shortTermBuzzerPattern; uint8_t longTermBuzzerPattern;
  char buzzerIps[5][16];
  uint32_t sessionStartEpoch; uint32_t lastAliveEpoch;
  uint32_t historyStart[5]; uint32_t historyEnd[5]; uint8_t historyCount;
  uint8_t displaySkin;
  uint16_t checksum;
};

// Settings exactly as it existed before notifyOtherEnabled was added
// (v11) — identical to V10 plus the currentMode string.
struct SettingsV11 {
  char magic[4]; uint8_t version;
  char wifiSSID[32]; char wifiPassword[64];
  float triggerDistanceCm; float wallDistanceCm; uint32_t lastCalibrationEpoch;
  bool alarmEnabled; uint16_t alarmDurationSec; bool autoArm; bool nightMode;
  int8_t timezoneOffsetHours; uint32_t bootCount;
  bool armed; bool oledOn;
  char telegramBotToken[48]; char telegramChatId[16];
  bool buzzerMasterEnabled; uint16_t longTermBuzzerDurationSec; uint16_t sustainedThresholdSec;
  char buzzerDeviceIp[16]; char deviceName[32]; char deviceId[24];
  char dashboardUsername[20]; char dashboardPassword[24];
  char siblingDevices[220];
  uint8_t shortTermBuzzerPattern; uint8_t longTermBuzzerPattern;
  char buzzerIps[5][16];
  uint32_t sessionStartEpoch; uint32_t lastAliveEpoch;
  uint32_t historyStart[5]; uint32_t historyEnd[5]; uint8_t historyCount;
  uint8_t displaySkin;
  char currentMode[16];
  uint16_t checksum;
};

// Settings exactly as it existed before nightStartHour/nightEndHour
// were added (v12) — identical to V11 plus notifyOtherEnabled.
struct SettingsV12 {
  char magic[4]; uint8_t version;
  char wifiSSID[32]; char wifiPassword[64];
  float triggerDistanceCm; float wallDistanceCm; uint32_t lastCalibrationEpoch;
  bool alarmEnabled; uint16_t alarmDurationSec; bool autoArm; bool nightMode;
  int8_t timezoneOffsetHours; uint32_t bootCount;
  bool armed; bool oledOn;
  char telegramBotToken[48]; char telegramChatId[16];
  bool buzzerMasterEnabled; uint16_t longTermBuzzerDurationSec; uint16_t sustainedThresholdSec;
  char buzzerDeviceIp[16]; char deviceName[32]; char deviceId[24];
  char dashboardUsername[20]; char dashboardPassword[24];
  char siblingDevices[220];
  uint8_t shortTermBuzzerPattern; uint8_t longTermBuzzerPattern;
  char buzzerIps[5][16];
  uint32_t sessionStartEpoch; uint32_t lastAliveEpoch;
  uint32_t historyStart[5]; uint32_t historyEnd[5]; uint8_t historyCount;
  uint8_t displaySkin;
  char currentMode[16];
  bool notifyOtherEnabled;
  uint16_t checksum;
};

// Settings exactly as it existed before telegramChatId2/3 were added
// (v13) — identical to V12 plus nightStartHour/nightEndHour.
struct SettingsV13 {
  char magic[4]; uint8_t version;
  char wifiSSID[32]; char wifiPassword[64];
  float triggerDistanceCm; float wallDistanceCm; uint32_t lastCalibrationEpoch;
  bool alarmEnabled; uint16_t alarmDurationSec; bool autoArm; bool nightMode;
  int8_t timezoneOffsetHours; uint32_t bootCount;
  bool armed; bool oledOn;
  char telegramBotToken[48]; char telegramChatId[16];
  bool buzzerMasterEnabled; uint16_t longTermBuzzerDurationSec; uint16_t sustainedThresholdSec;
  char buzzerDeviceIp[16]; char deviceName[32]; char deviceId[24];
  char dashboardUsername[20]; char dashboardPassword[24];
  char siblingDevices[220];
  uint8_t shortTermBuzzerPattern; uint8_t longTermBuzzerPattern;
  char buzzerIps[5][16];
  uint32_t sessionStartEpoch; uint32_t lastAliveEpoch;
  uint32_t historyStart[5]; uint32_t historyEnd[5]; uint8_t historyCount;
  uint8_t displaySkin;
  char currentMode[16];
  bool notifyOtherEnabled;
  uint8_t nightStartHour;
  uint8_t nightEndHour;
  uint16_t checksum;
};

namespace Storage {

static uint16_t computeChecksum(const uint8_t *bytes, size_t len) {
  uint16_t sum = 0;
  for (size_t i = 0; i < len; i++) sum += bytes[i];
  return sum;
}
static uint16_t checksumOf(const Settings &s) { return computeChecksum((const uint8_t*)&s, offsetof(Settings, checksum)); }
static uint16_t checksumOfV1(const SettingsV1 &s) { return computeChecksum((const uint8_t*)&s, offsetof(SettingsV1, checksum)); }
static uint16_t checksumOfV2(const SettingsV2 &s) { return computeChecksum((const uint8_t*)&s, offsetof(SettingsV2, checksum)); }
static uint16_t checksumOfV3(const SettingsV3 &s) { return computeChecksum((const uint8_t*)&s, offsetof(SettingsV3, checksum)); }
static uint16_t checksumOfV4(const SettingsV4 &s) { return computeChecksum((const uint8_t*)&s, offsetof(SettingsV4, checksum)); }
static uint16_t checksumOfV5(const SettingsV5 &s) { return computeChecksum((const uint8_t*)&s, offsetof(SettingsV5, checksum)); }
static uint16_t checksumOfV6(const SettingsV6 &s) { return computeChecksum((const uint8_t*)&s, offsetof(SettingsV6, checksum)); }
static uint16_t checksumOfV7(const SettingsV7 &s) { return computeChecksum((const uint8_t*)&s, offsetof(SettingsV7, checksum)); }
static uint16_t checksumOfV8(const SettingsV8 &s) { return computeChecksum((const uint8_t*)&s, offsetof(SettingsV8, checksum)); }
static uint16_t checksumOfV9(const SettingsV9 &s) { return computeChecksum((const uint8_t*)&s, offsetof(SettingsV9, checksum)); }
static uint16_t checksumOfV10(const SettingsV10 &s) { return computeChecksum((const uint8_t*)&s, offsetof(SettingsV10, checksum)); }
static uint16_t checksumOfV11(const SettingsV11 &s) { return computeChecksum((const uint8_t*)&s, offsetof(SettingsV11, checksum)); }
static uint16_t checksumOfV12(const SettingsV12 &s) { return computeChecksum((const uint8_t*)&s, offsetof(SettingsV12, checksum)); }
static uint16_t checksumOfV13(const SettingsV13 &s) { return computeChecksum((const uint8_t*)&s, offsetof(SettingsV13, checksum)); }

static Settings defaults() {
  Settings s; memset(&s, 0, sizeof(Settings));
  memcpy(s.magic, STORAGE_MAGIC, 4);
  s.version = STORAGE_VERSION;
  s.alarmEnabled = true;
  s.alarmDurationSec = DEFAULT_SHORT_TERM_BUZZER_SEC;
  s.armed = true;
  s.oledOn = true;
  strncpy(s.telegramBotToken, TELEGRAM_BOT_TOKEN, sizeof(s.telegramBotToken)-1);
  strncpy(s.telegramChatId, TELEGRAM_CHAT_ID, sizeof(s.telegramChatId)-1);
  s.buzzerMasterEnabled = true;
  s.longTermBuzzerDurationSec = DEFAULT_LONG_TERM_BUZZER_SEC;
  s.sustainedThresholdSec = DEFAULT_SUSTAINED_THRESHOLD_SEC;
  strncpy(s.buzzerDeviceIp, DEFAULT_BUZZER_DEVICE_IP, sizeof(s.buzzerDeviceIp)-1);
  strncpy(s.deviceName, DEFAULT_DEVICE_NAME, sizeof(s.deviceName)-1);
  strncpy(s.deviceId, DEFAULT_DEVICE_ID, sizeof(s.deviceId)-1);
  strncpy(s.dashboardUsername, DEFAULT_DASHBOARD_USERNAME, sizeof(s.dashboardUsername)-1);
  strncpy(s.dashboardPassword, DEFAULT_DASHBOARD_PASSWORD, sizeof(s.dashboardPassword)-1);
  s.shortTermBuzzerPattern = 1; // Continuous
  s.longTermBuzzerPattern = 2;  // Slow Pulse
  strncpy(s.buzzerIps[0], s.buzzerDeviceIp, sizeof(s.buzzerIps[0])-1);
  s.sessionStartEpoch = 0;
  s.lastAliveEpoch = 0;
  s.historyCount = 0;
  s.displaySkin = 0; // Classic Numeric — matches the original always-on layout
  strncpy(s.currentMode, "home", sizeof(s.currentMode)-1); // matches the app's own first-run suggestion
  s.notifyOtherEnabled = true; // default on, everything notifies until the user turns it down
  s.nightStartHour = 22; // matches the old hardcoded NIGHT_START_HOUR (10 PM)
  s.nightEndHour = 7;    // matches the old hardcoded NIGHT_END_HOUR (7 AM)
  s.checksum = checksumOf(s);
  return s;
}

void begin() {
  EEPROM.begin(EEPROM_SIZE);
  Serial.print("[STORAGE] EEPROM initialized, size: ");
  Serial.println(EEPROM_SIZE);
}

Settings load() {
  Settings s;
  EEPROM.get(EEPROM_START_ADDR, s);
  bool magicOk = (memcmp(s.magic, STORAGE_MAGIC, 4) == 0);

  if (magicOk && s.checksum == checksumOf(s)) {
    Serial.println("[STORAGE] Settings loaded (v14).");
    return s;
  }

  if (magicOk) {
    SettingsV13 v13; EEPROM.get(EEPROM_START_ADDR, v13);
    if (v13.checksum == checksumOfV13(v13)) {
      Serial.println("[STORAGE] Migrating v13 -> v14 (all settings preserved, no extra Telegram recipients yet).");
      Settings m = defaults();
      memcpy(m.wifiSSID, v13.wifiSSID, sizeof(m.wifiSSID));
      memcpy(m.wifiPassword, v13.wifiPassword, sizeof(m.wifiPassword));
      m.triggerDistanceCm = v13.triggerDistanceCm; m.wallDistanceCm = v13.wallDistanceCm;
      m.lastCalibrationEpoch = v13.lastCalibrationEpoch;
      m.alarmEnabled = v13.alarmEnabled; m.alarmDurationSec = v13.alarmDurationSec;
      m.autoArm = v13.autoArm; m.nightMode = v13.nightMode;
      m.timezoneOffsetHours = v13.timezoneOffsetHours; m.bootCount = v13.bootCount;
      m.armed = v13.armed; m.oledOn = v13.oledOn;
      memcpy(m.telegramBotToken, v13.telegramBotToken, sizeof(m.telegramBotToken));
      memcpy(m.telegramChatId, v13.telegramChatId, sizeof(m.telegramChatId));
      m.buzzerMasterEnabled = v13.buzzerMasterEnabled;
      m.longTermBuzzerDurationSec = v13.longTermBuzzerDurationSec;
      m.sustainedThresholdSec = v13.sustainedThresholdSec;
      memcpy(m.buzzerDeviceIp, v13.buzzerDeviceIp, sizeof(m.buzzerDeviceIp));
      memcpy(m.deviceName, v13.deviceName, sizeof(m.deviceName));
      memcpy(m.deviceId, v13.deviceId, sizeof(m.deviceId));
      memcpy(m.dashboardUsername, v13.dashboardUsername, sizeof(m.dashboardUsername));
      memcpy(m.dashboardPassword, v13.dashboardPassword, sizeof(m.dashboardPassword));
      memcpy(m.siblingDevices, v13.siblingDevices, sizeof(m.siblingDevices));
      m.shortTermBuzzerPattern = v13.shortTermBuzzerPattern;
      m.longTermBuzzerPattern = v13.longTermBuzzerPattern;
      memcpy(m.buzzerIps, v13.buzzerIps, sizeof(m.buzzerIps));
      m.sessionStartEpoch = v13.sessionStartEpoch;
      m.lastAliveEpoch = v13.lastAliveEpoch;
      memcpy(m.historyStart, v13.historyStart, sizeof(m.historyStart));
      memcpy(m.historyEnd, v13.historyEnd, sizeof(m.historyEnd));
      m.historyCount = v13.historyCount;
      m.displaySkin = v13.displaySkin;
      memcpy(m.currentMode, v13.currentMode, sizeof(m.currentMode));
      m.notifyOtherEnabled = v13.notifyOtherEnabled;
      m.nightStartHour = v13.nightStartHour;
      m.nightEndHour = v13.nightEndHour;
      // telegramChatId2/3 intentionally left at defaults()'s empty
      // strings — this device never had extra recipients configured.
      save(m);
      return m;
    }
    SettingsV12 v12; EEPROM.get(EEPROM_START_ADDR, v12);
    if (v12.checksum == checksumOfV12(v12)) {
      Serial.println("[STORAGE] Migrating v12 -> v13 (all settings preserved, night schedule defaults to 22:00-07:00).");
      Settings m = defaults();
      memcpy(m.wifiSSID, v12.wifiSSID, sizeof(m.wifiSSID));
      memcpy(m.wifiPassword, v12.wifiPassword, sizeof(m.wifiPassword));
      m.triggerDistanceCm = v12.triggerDistanceCm; m.wallDistanceCm = v12.wallDistanceCm;
      m.lastCalibrationEpoch = v12.lastCalibrationEpoch;
      m.alarmEnabled = v12.alarmEnabled; m.alarmDurationSec = v12.alarmDurationSec;
      m.autoArm = v12.autoArm; m.nightMode = v12.nightMode;
      m.timezoneOffsetHours = v12.timezoneOffsetHours; m.bootCount = v12.bootCount;
      m.armed = v12.armed; m.oledOn = v12.oledOn;
      memcpy(m.telegramBotToken, v12.telegramBotToken, sizeof(m.telegramBotToken));
      memcpy(m.telegramChatId, v12.telegramChatId, sizeof(m.telegramChatId));
      m.buzzerMasterEnabled = v12.buzzerMasterEnabled;
      m.longTermBuzzerDurationSec = v12.longTermBuzzerDurationSec;
      m.sustainedThresholdSec = v12.sustainedThresholdSec;
      memcpy(m.buzzerDeviceIp, v12.buzzerDeviceIp, sizeof(m.buzzerDeviceIp));
      memcpy(m.deviceName, v12.deviceName, sizeof(m.deviceName));
      memcpy(m.deviceId, v12.deviceId, sizeof(m.deviceId));
      memcpy(m.dashboardUsername, v12.dashboardUsername, sizeof(m.dashboardUsername));
      memcpy(m.dashboardPassword, v12.dashboardPassword, sizeof(m.dashboardPassword));
      memcpy(m.siblingDevices, v12.siblingDevices, sizeof(m.siblingDevices));
      m.shortTermBuzzerPattern = v12.shortTermBuzzerPattern;
      m.longTermBuzzerPattern = v12.longTermBuzzerPattern;
      memcpy(m.buzzerIps, v12.buzzerIps, sizeof(m.buzzerIps));
      m.sessionStartEpoch = v12.sessionStartEpoch;
      m.lastAliveEpoch = v12.lastAliveEpoch;
      memcpy(m.historyStart, v12.historyStart, sizeof(m.historyStart));
      memcpy(m.historyEnd, v12.historyEnd, sizeof(m.historyEnd));
      m.historyCount = v12.historyCount;
      m.displaySkin = v12.displaySkin;
      memcpy(m.currentMode, v12.currentMode, sizeof(m.currentMode));
      m.notifyOtherEnabled = v12.notifyOtherEnabled;
      // nightStartHour/nightEndHour intentionally left at defaults()'s
      // 22/7 — matches the old hardcoded schedule this device was
      // already running under, so migrating changes nothing behaviorally.
      save(m);
      return m;
    }
    SettingsV11 v11; EEPROM.get(EEPROM_START_ADDR, v11);
    if (v11.checksum == checksumOfV11(v11)) {
      Serial.println("[STORAGE] Migrating v11 -> v12 (all settings preserved, Other Notifications defaults to on).");
      Settings m = defaults();
      memcpy(m.wifiSSID, v11.wifiSSID, sizeof(m.wifiSSID));
      memcpy(m.wifiPassword, v11.wifiPassword, sizeof(m.wifiPassword));
      m.triggerDistanceCm = v11.triggerDistanceCm; m.wallDistanceCm = v11.wallDistanceCm;
      m.lastCalibrationEpoch = v11.lastCalibrationEpoch;
      m.alarmEnabled = v11.alarmEnabled; m.alarmDurationSec = v11.alarmDurationSec;
      m.autoArm = v11.autoArm; m.nightMode = v11.nightMode;
      m.timezoneOffsetHours = v11.timezoneOffsetHours; m.bootCount = v11.bootCount;
      m.armed = v11.armed; m.oledOn = v11.oledOn;
      memcpy(m.telegramBotToken, v11.telegramBotToken, sizeof(m.telegramBotToken));
      memcpy(m.telegramChatId, v11.telegramChatId, sizeof(m.telegramChatId));
      m.buzzerMasterEnabled = v11.buzzerMasterEnabled;
      m.longTermBuzzerDurationSec = v11.longTermBuzzerDurationSec;
      m.sustainedThresholdSec = v11.sustainedThresholdSec;
      memcpy(m.buzzerDeviceIp, v11.buzzerDeviceIp, sizeof(m.buzzerDeviceIp));
      memcpy(m.deviceName, v11.deviceName, sizeof(m.deviceName));
      memcpy(m.deviceId, v11.deviceId, sizeof(m.deviceId));
      memcpy(m.dashboardUsername, v11.dashboardUsername, sizeof(m.dashboardUsername));
      memcpy(m.dashboardPassword, v11.dashboardPassword, sizeof(m.dashboardPassword));
      memcpy(m.siblingDevices, v11.siblingDevices, sizeof(m.siblingDevices));
      m.shortTermBuzzerPattern = v11.shortTermBuzzerPattern;
      m.longTermBuzzerPattern = v11.longTermBuzzerPattern;
      memcpy(m.buzzerIps, v11.buzzerIps, sizeof(m.buzzerIps));
      m.sessionStartEpoch = v11.sessionStartEpoch;
      m.lastAliveEpoch = v11.lastAliveEpoch;
      memcpy(m.historyStart, v11.historyStart, sizeof(m.historyStart));
      memcpy(m.historyEnd, v11.historyEnd, sizeof(m.historyEnd));
      m.historyCount = v11.historyCount;
      m.displaySkin = v11.displaySkin;
      memcpy(m.currentMode, v11.currentMode, sizeof(m.currentMode));
      // notifyOtherEnabled intentionally left at defaults()'s "true" —
      // v11 devices never tracked this, so "on" is the safe default
      // (matches the pre-existing behavior where everything notified).
      save(m);
      return m;
    }
    SettingsV10 v10; EEPROM.get(EEPROM_START_ADDR, v10);
    if (v10.checksum == checksumOfV10(v10)) {
      Serial.println("[STORAGE] Migrating v10 -> v11 (all settings preserved, currentMode defaults to home).");
      Settings m = defaults();
      memcpy(m.wifiSSID, v10.wifiSSID, sizeof(m.wifiSSID));
      memcpy(m.wifiPassword, v10.wifiPassword, sizeof(m.wifiPassword));
      m.triggerDistanceCm = v10.triggerDistanceCm; m.wallDistanceCm = v10.wallDistanceCm;
      m.lastCalibrationEpoch = v10.lastCalibrationEpoch;
      m.alarmEnabled = v10.alarmEnabled; m.alarmDurationSec = v10.alarmDurationSec;
      m.autoArm = v10.autoArm; m.nightMode = v10.nightMode;
      m.timezoneOffsetHours = v10.timezoneOffsetHours; m.bootCount = v10.bootCount;
      m.armed = v10.armed; m.oledOn = v10.oledOn;
      memcpy(m.telegramBotToken, v10.telegramBotToken, sizeof(m.telegramBotToken));
      memcpy(m.telegramChatId, v10.telegramChatId, sizeof(m.telegramChatId));
      m.buzzerMasterEnabled = v10.buzzerMasterEnabled;
      m.longTermBuzzerDurationSec = v10.longTermBuzzerDurationSec;
      m.sustainedThresholdSec = v10.sustainedThresholdSec;
      memcpy(m.buzzerDeviceIp, v10.buzzerDeviceIp, sizeof(m.buzzerDeviceIp));
      memcpy(m.deviceName, v10.deviceName, sizeof(m.deviceName));
      memcpy(m.deviceId, v10.deviceId, sizeof(m.deviceId));
      memcpy(m.dashboardUsername, v10.dashboardUsername, sizeof(m.dashboardUsername));
      memcpy(m.dashboardPassword, v10.dashboardPassword, sizeof(m.dashboardPassword));
      memcpy(m.siblingDevices, v10.siblingDevices, sizeof(m.siblingDevices));
      m.shortTermBuzzerPattern = v10.shortTermBuzzerPattern;
      m.longTermBuzzerPattern = v10.longTermBuzzerPattern;
      memcpy(m.buzzerIps, v10.buzzerIps, sizeof(m.buzzerIps));
      m.sessionStartEpoch = v10.sessionStartEpoch;
      m.lastAliveEpoch = v10.lastAliveEpoch;
      memcpy(m.historyStart, v10.historyStart, sizeof(m.historyStart));
      memcpy(m.historyEnd, v10.historyEnd, sizeof(m.historyEnd));
      m.historyCount = v10.historyCount;
      m.displaySkin = v10.displaySkin;
      // currentMode intentionally left at defaults()'s "home" — v10
      // devices never tracked a mode, so "home" is the safest guess
      // (matches the app's own suggested default for a device with no
      // mode history).
      save(m);
      return m;
    }
    SettingsV9 v9; EEPROM.get(EEPROM_START_ADDR, v9);
    if (v9.checksum == checksumOfV9(v9)) {
      Serial.println("[STORAGE] Migrating v9 -> v10 (all settings preserved, displaySkin defaults to Classic).");
      Settings m = defaults();
      memcpy(m.wifiSSID, v9.wifiSSID, sizeof(m.wifiSSID));
      memcpy(m.wifiPassword, v9.wifiPassword, sizeof(m.wifiPassword));
      m.triggerDistanceCm = v9.triggerDistanceCm; m.wallDistanceCm = v9.wallDistanceCm;
      m.lastCalibrationEpoch = v9.lastCalibrationEpoch;
      m.alarmEnabled = v9.alarmEnabled; m.alarmDurationSec = v9.alarmDurationSec;
      m.autoArm = v9.autoArm; m.nightMode = v9.nightMode;
      m.timezoneOffsetHours = v9.timezoneOffsetHours; m.bootCount = v9.bootCount;
      m.armed = v9.armed; m.oledOn = v9.oledOn;
      memcpy(m.telegramBotToken, v9.telegramBotToken, sizeof(m.telegramBotToken));
      memcpy(m.telegramChatId, v9.telegramChatId, sizeof(m.telegramChatId));
      m.buzzerMasterEnabled = v9.buzzerMasterEnabled;
      m.longTermBuzzerDurationSec = v9.longTermBuzzerDurationSec;
      m.sustainedThresholdSec = v9.sustainedThresholdSec;
      memcpy(m.buzzerDeviceIp, v9.buzzerDeviceIp, sizeof(m.buzzerDeviceIp));
      memcpy(m.deviceName, v9.deviceName, sizeof(m.deviceName));
      memcpy(m.deviceId, v9.deviceId, sizeof(m.deviceId));
      memcpy(m.dashboardUsername, v9.dashboardUsername, sizeof(m.dashboardUsername));
      memcpy(m.dashboardPassword, v9.dashboardPassword, sizeof(m.dashboardPassword));
      memcpy(m.siblingDevices, v9.siblingDevices, sizeof(m.siblingDevices));
      m.shortTermBuzzerPattern = v9.shortTermBuzzerPattern;
      m.longTermBuzzerPattern = v9.longTermBuzzerPattern;
      memcpy(m.buzzerIps, v9.buzzerIps, sizeof(m.buzzerIps));
      m.sessionStartEpoch = v9.sessionStartEpoch;
      m.lastAliveEpoch = v9.lastAliveEpoch;
      memcpy(m.historyStart, v9.historyStart, sizeof(m.historyStart));
      memcpy(m.historyEnd, v9.historyEnd, sizeof(m.historyEnd));
      m.historyCount = v9.historyCount;
      save(m);
      return m;
    }
    SettingsV8 v8; EEPROM.get(EEPROM_START_ADDR, v8);
    if (v8.checksum == checksumOfV8(v8)) {
      Serial.println("[STORAGE] Migrating v8 -> v10 (all settings preserved).");
      Settings m = defaults();
      memcpy(m.wifiSSID, v8.wifiSSID, sizeof(m.wifiSSID));
      memcpy(m.wifiPassword, v8.wifiPassword, sizeof(m.wifiPassword));
      m.triggerDistanceCm = v8.triggerDistanceCm; m.wallDistanceCm = v8.wallDistanceCm;
      m.lastCalibrationEpoch = v8.lastCalibrationEpoch;
      m.alarmEnabled = v8.alarmEnabled; m.alarmDurationSec = v8.alarmDurationSec;
      m.autoArm = v8.autoArm; m.nightMode = v8.nightMode;
      m.timezoneOffsetHours = v8.timezoneOffsetHours; m.bootCount = v8.bootCount;
      m.armed = v8.armed; m.oledOn = v8.oledOn;
      memcpy(m.telegramBotToken, v8.telegramBotToken, sizeof(m.telegramBotToken));
      memcpy(m.telegramChatId, v8.telegramChatId, sizeof(m.telegramChatId));
      m.buzzerMasterEnabled = v8.buzzerMasterEnabled;
      m.longTermBuzzerDurationSec = v8.longTermBuzzerDurationSec;
      m.sustainedThresholdSec = v8.sustainedThresholdSec;
      memcpy(m.buzzerDeviceIp, v8.buzzerDeviceIp, sizeof(m.buzzerDeviceIp));
      memcpy(m.buzzerIps[0], v8.buzzerDeviceIp, sizeof(m.buzzerIps[0]));
      memcpy(m.deviceName, v8.deviceName, sizeof(m.deviceName));
      memcpy(m.deviceId, v8.deviceId, sizeof(m.deviceId));
      memcpy(m.dashboardUsername, v8.dashboardUsername, sizeof(m.dashboardUsername));
      memcpy(m.dashboardPassword, v8.dashboardPassword, sizeof(m.dashboardPassword));
      memcpy(m.siblingDevices, v8.siblingDevices, sizeof(m.siblingDevices));
      m.shortTermBuzzerPattern = v8.shortTermBuzzerPattern;
      m.longTermBuzzerPattern = v8.longTermBuzzerPattern;
      save(m);
      return m;
    }
    SettingsV7 v7; EEPROM.get(EEPROM_START_ADDR, v7);
    if (v7.checksum == checksumOfV7(v7)) {
      Serial.println("[STORAGE] Migrating v7 -> v8 (all settings preserved).");
      Settings m = defaults();
      memcpy(m.wifiSSID, v7.wifiSSID, sizeof(m.wifiSSID));
      memcpy(m.wifiPassword, v7.wifiPassword, sizeof(m.wifiPassword));
      m.triggerDistanceCm = v7.triggerDistanceCm; m.wallDistanceCm = v7.wallDistanceCm;
      m.lastCalibrationEpoch = v7.lastCalibrationEpoch;
      m.alarmEnabled = v7.alarmEnabled; m.alarmDurationSec = v7.alarmDurationSec;
      m.autoArm = v7.autoArm; m.nightMode = v7.nightMode;
      m.timezoneOffsetHours = v7.timezoneOffsetHours; m.bootCount = v7.bootCount;
      m.armed = v7.armed; m.oledOn = v7.oledOn;
      memcpy(m.telegramBotToken, v7.telegramBotToken, sizeof(m.telegramBotToken));
      memcpy(m.telegramChatId, v7.telegramChatId, sizeof(m.telegramChatId));
      m.buzzerMasterEnabled = v7.buzzerMasterEnabled;
      m.longTermBuzzerDurationSec = v7.longTermBuzzerDurationSec;
      m.sustainedThresholdSec = v7.sustainedThresholdSec;
      memcpy(m.buzzerDeviceIp, v7.buzzerDeviceIp, sizeof(m.buzzerDeviceIp));
      memcpy(m.deviceName, v7.deviceName, sizeof(m.deviceName));
      memcpy(m.deviceId, v7.deviceId, sizeof(m.deviceId));
      memcpy(m.dashboardUsername, v7.dashboardUsername, sizeof(m.dashboardUsername));
      memcpy(m.dashboardPassword, v7.dashboardPassword, sizeof(m.dashboardPassword));
      memcpy(m.siblingDevices, v7.siblingDevices, sizeof(m.siblingDevices));
      save(m);
      return m;
    }
    SettingsV6 v6; EEPROM.get(EEPROM_START_ADDR, v6);
    if (v6.checksum == checksumOfV6(v6)) {
      Serial.println("[STORAGE] Migrating v6 -> v7 (all settings preserved).");
      Settings m = defaults();
      memcpy(m.wifiSSID, v6.wifiSSID, sizeof(m.wifiSSID));
      memcpy(m.wifiPassword, v6.wifiPassword, sizeof(m.wifiPassword));
      m.triggerDistanceCm = v6.triggerDistanceCm; m.wallDistanceCm = v6.wallDistanceCm;
      m.lastCalibrationEpoch = v6.lastCalibrationEpoch;
      m.alarmEnabled = v6.alarmEnabled; m.alarmDurationSec = v6.alarmDurationSec;
      m.autoArm = v6.autoArm; m.nightMode = v6.nightMode;
      m.timezoneOffsetHours = v6.timezoneOffsetHours; m.bootCount = v6.bootCount;
      m.armed = v6.armed; m.oledOn = v6.oledOn;
      memcpy(m.telegramBotToken, v6.telegramBotToken, sizeof(m.telegramBotToken));
      memcpy(m.telegramChatId, v6.telegramChatId, sizeof(m.telegramChatId));
      m.buzzerMasterEnabled = v6.buzzerMasterEnabled;
      m.longTermBuzzerDurationSec = v6.longTermBuzzerDurationSec;
      m.sustainedThresholdSec = v6.sustainedThresholdSec;
      memcpy(m.buzzerDeviceIp, v6.buzzerDeviceIp, sizeof(m.buzzerDeviceIp));
      memcpy(m.deviceName, v6.deviceName, sizeof(m.deviceName));
      memcpy(m.deviceId, v6.deviceId, sizeof(m.deviceId));
      memcpy(m.dashboardUsername, v6.dashboardUsername, sizeof(m.dashboardUsername));
      memcpy(m.dashboardPassword, v6.dashboardPassword, sizeof(m.dashboardPassword));
      save(m);
      return m;
    }
    SettingsV5 v5; EEPROM.get(EEPROM_START_ADDR, v5);
    if (v5.checksum == checksumOfV5(v5)) {
      Serial.println("[STORAGE] Migrating v5 -> v6 (all settings preserved).");
      Settings m = defaults();
      memcpy(m.wifiSSID, v5.wifiSSID, sizeof(m.wifiSSID));
      memcpy(m.wifiPassword, v5.wifiPassword, sizeof(m.wifiPassword));
      m.triggerDistanceCm = v5.triggerDistanceCm; m.wallDistanceCm = v5.wallDistanceCm;
      m.lastCalibrationEpoch = v5.lastCalibrationEpoch;
      m.alarmEnabled = v5.alarmEnabled; m.alarmDurationSec = v5.alarmDurationSec;
      m.autoArm = v5.autoArm; m.nightMode = v5.nightMode;
      m.timezoneOffsetHours = v5.timezoneOffsetHours; m.bootCount = v5.bootCount;
      m.armed = v5.armed; m.oledOn = v5.oledOn;
      memcpy(m.telegramBotToken, v5.telegramBotToken, sizeof(m.telegramBotToken));
      memcpy(m.telegramChatId, v5.telegramChatId, sizeof(m.telegramChatId));
      m.buzzerMasterEnabled = v5.buzzerMasterEnabled;
      m.longTermBuzzerDurationSec = v5.longTermBuzzerDurationSec;
      m.sustainedThresholdSec = v5.sustainedThresholdSec;
      memcpy(m.buzzerDeviceIp, v5.buzzerDeviceIp, sizeof(m.buzzerDeviceIp));
      memcpy(m.deviceName, v5.deviceName, sizeof(m.deviceName));
      memcpy(m.deviceId, v5.deviceId, sizeof(m.deviceId));
      save(m);
      return m;
    }
    SettingsV4 v4; EEPROM.get(EEPROM_START_ADDR, v4);
    if (v4.checksum == checksumOfV4(v4)) {
      Serial.println("[STORAGE] Migrating v4 -> v5 (all settings preserved).");
      Settings m = defaults();
      memcpy(m.wifiSSID, v4.wifiSSID, sizeof(m.wifiSSID));
      memcpy(m.wifiPassword, v4.wifiPassword, sizeof(m.wifiPassword));
      m.triggerDistanceCm = v4.triggerDistanceCm; m.wallDistanceCm = v4.wallDistanceCm;
      m.lastCalibrationEpoch = v4.lastCalibrationEpoch;
      m.alarmEnabled = v4.alarmEnabled; m.alarmDurationSec = v4.alarmDurationSec;
      m.autoArm = v4.autoArm; m.nightMode = v4.nightMode;
      m.timezoneOffsetHours = v4.timezoneOffsetHours; m.bootCount = v4.bootCount;
      m.armed = v4.armed; m.oledOn = v4.oledOn;
      memcpy(m.telegramBotToken, v4.telegramBotToken, sizeof(m.telegramBotToken));
      memcpy(m.telegramChatId, v4.telegramChatId, sizeof(m.telegramChatId));
      m.buzzerMasterEnabled = v4.buzzerMasterEnabled;
      m.longTermBuzzerDurationSec = v4.longTermBuzzerDurationSec;
      m.sustainedThresholdSec = v4.sustainedThresholdSec;
      save(m);
      return m;
    }
    SettingsV3 v3; EEPROM.get(EEPROM_START_ADDR, v3);
    if (v3.checksum == checksumOfV3(v3)) {
      Serial.println("[STORAGE] Migrating v3 -> v5.");
      Settings m = defaults();
      memcpy(m.wifiSSID, v3.wifiSSID, sizeof(m.wifiSSID));
      memcpy(m.wifiPassword, v3.wifiPassword, sizeof(m.wifiPassword));
      m.triggerDistanceCm = v3.triggerDistanceCm; m.wallDistanceCm = v3.wallDistanceCm;
      m.lastCalibrationEpoch = v3.lastCalibrationEpoch;
      m.alarmEnabled = v3.alarmEnabled; m.alarmDurationSec = v3.alarmDurationSec;
      m.autoArm = v3.autoArm; m.nightMode = v3.nightMode;
      m.timezoneOffsetHours = v3.timezoneOffsetHours; m.bootCount = v3.bootCount;
      m.armed = v3.armed; m.oledOn = v3.oledOn;
      memcpy(m.telegramBotToken, v3.telegramBotToken, sizeof(m.telegramBotToken));
      memcpy(m.telegramChatId, v3.telegramChatId, sizeof(m.telegramChatId));
      save(m);
      return m;
    }
    SettingsV2 v2; EEPROM.get(EEPROM_START_ADDR, v2);
    if (v2.checksum == checksumOfV2(v2)) {
      Serial.println("[STORAGE] Migrating v2 -> v5.");
      Settings m = defaults();
      memcpy(m.wifiSSID, v2.wifiSSID, sizeof(m.wifiSSID));
      memcpy(m.wifiPassword, v2.wifiPassword, sizeof(m.wifiPassword));
      m.triggerDistanceCm = v2.triggerDistanceCm; m.wallDistanceCm = v2.wallDistanceCm;
      m.lastCalibrationEpoch = v2.lastCalibrationEpoch;
      m.alarmEnabled = v2.alarmEnabled; m.alarmDurationSec = v2.alarmDurationSec;
      m.autoArm = v2.autoArm; m.nightMode = v2.nightMode;
      m.timezoneOffsetHours = v2.timezoneOffsetHours; m.bootCount = v2.bootCount;
      m.armed = v2.armed; m.oledOn = v2.oledOn;
      save(m);
      return m;
    }
    SettingsV1 v1; EEPROM.get(EEPROM_START_ADDR, v1);
    if (v1.checksum == checksumOfV1(v1)) {
      Serial.println("[STORAGE] Migrating v1 -> v5.");
      Settings m = defaults();
      memcpy(m.wifiSSID, v1.wifiSSID, sizeof(m.wifiSSID));
      memcpy(m.wifiPassword, v1.wifiPassword, sizeof(m.wifiPassword));
      m.triggerDistanceCm = v1.triggerDistanceCm; m.wallDistanceCm = v1.wallDistanceCm;
      m.lastCalibrationEpoch = v1.lastCalibrationEpoch;
      m.alarmEnabled = v1.alarmEnabled; m.alarmDurationSec = v1.alarmDurationSec;
      m.autoArm = v1.autoArm; m.nightMode = v1.nightMode;
      m.timezoneOffsetHours = v1.timezoneOffsetHours; m.bootCount = v1.bootCount;
      save(m);
      return m;
    }
  }

  Serial.println("[STORAGE] No valid settings found — using factory defaults.");
  Settings d = defaults();
  save(d);
  return d;
}

bool save(Settings &settings) {
  memcpy(settings.magic, STORAGE_MAGIC, 4);
  settings.version = STORAGE_VERSION;
  settings.checksum = checksumOf(settings);
  EEPROM.put(EEPROM_START_ADDR, settings);
  bool ok = EEPROM.commit();
  if (!ok) Serial.println("[STORAGE] ERROR: EEPROM.commit() failed.");
  return ok;
}

Settings factoryReset() {
  Settings d = defaults();
  save(d);
  return d;
}

}
