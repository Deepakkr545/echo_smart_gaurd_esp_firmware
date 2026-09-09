/*
  storage.cpp — v5: added buzzerDeviceIp/deviceName/deviceId (dashboard-
  editable, persisted). Migrates from v1/v2/v3/v4 preserving everything
  — nothing is wiped on upgrade.
*/

#include "storage.h"
#include "config.h"
#include <EEPROM.h>

#define STORAGE_MAGIC        "SSS1"
#define STORAGE_VERSION      9
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
    Serial.println("[STORAGE] Settings loaded (v9).");
    return s;
  }

  if (magicOk) {
    SettingsV8 v8; EEPROM.get(EEPROM_START_ADDR, v8);
    if (v8.checksum == checksumOfV8(v8)) {
      Serial.println("[STORAGE] Migrating v8 -> v9 (all settings preserved).");
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
