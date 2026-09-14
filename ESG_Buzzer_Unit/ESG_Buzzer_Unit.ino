/*
  =====================================================================
  BuzzerUnit.ino — Second ESP8266 (Portable Buzzer Receiver)
  =====================================================================
  NOTE ON THIS UPDATE: config struct changed again (magic bumped to
  "BZ04") to add pause/uptime-history/wifi-history fields. ONE-TIME
  reset of name/ID/WiFi/manual-sensors/login — re-enter via setup page
  / dashboard once after this update.

  Wiring: same as before (see earlier revisions) —
    OLED SDA->D2 SCL->D1, Button D6->GND w/ 10K pull-up to 3V3,
    Buzzer 12V+ -> buzzer+ ; buzzer- -> MOSFET Drain (D5 gate via 220R).

  First-time WiFi setup: AP "BuzzerSetup"/"12345678" if no saved WiFi.

  Sensor ESPs connect via AUTO (/announce + /trigger,/pulse carry
  sensorName/sensorId) or MANUAL (dashboard "Add Sensor" — pulls that
  sensor's /status). Combined total capped at MAX_SENSORS (5).

  Dashboard login optional (empty = no login). RESETLOGIN via Serial
  clears it if forgotten. Master fallback: admin / adminpass.
  =====================================================================
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h> // OTA firmware updates via /update — see the app's OTA_FIRMWARE_SETUP.md
#include <WiFiClientSecure.h> // Telegram integration (added BZ06)
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h> // sin/cos/fmod — used by the Radar Sweep, Sonar Pulse, and Analog Gauge skins
#include <EEPROM.h>
#include <time.h>

// ---------------------------------------------------------------------
// Persisted config
// ---------------------------------------------------------------------
#define BUZZ_EEPROM_SIZE 700
#define BUZZ_MAGIC "BZ07"
#define FIRMWARE_VERSION "1.0.0" // Buzzer Unit firmware version — exposed in /info for the app's OTA update-check
#define MAX_MANUAL_SENSORS 5
#define BUZZ_NTP_GMT_OFFSET_SEC 19800 // IST +5:30, same as main sensor ESP

// Forward declaration — defined later (near connectWifi()), but called
// from checkButton() and checkSerialCommands() which appear earlier in
// this file. Arduino's automatic prototype generation didn't pick this
// one up on its own, so it's declared explicitly here.
void forceApMode();

struct ManualSensorEntry {
  char ip[16];
  char username[20];
  char password[24];
};

struct BuzzerConfig {
  char magic[4];
  char name[24];
  char id[24];
  char wifiSSID[32];
  char wifiPassword[64];
  char dashUsername[20]; // empty = no login required (default)
  char dashPassword[24];
  ManualSensorEntry manualSensors[MAX_MANUAL_SENSORS];
  bool buzzerPaused;         // persisted mute — survives reboot
  uint32_t sessionStartEpoch;
  uint32_t lastAliveEpoch;
  uint32_t historyStart[5];  // uptime session history (ring buffer)
  uint32_t historyEnd[5];
  uint8_t historyCount;
  uint32_t wifiReconnectLog[5]; // WiFi connect/reconnect event timestamps
  uint8_t wifiReconnectCount;
  uint8_t displaySkin;       // Idle-screen skin (added BZ05) — see DISPLAY_SKIN_COUNT/skin drawing functions below
  char telegramBotToken[48]; // added BZ06 — Buzzer Unit's own Telegram integration
  char telegramChatId[16];
  bool telegramEnabled;       // master mute for THIS device's Telegram alerts (independent of buzzerPaused)
  char currentMode[16];       // added BZ07 — "off"|"home"|"red_alert"|"test", set by the app's /setmode call. Same single-source-of-truth purpose as the Sensor Unit's currentMode field.
  uint16_t checksum;
};
BuzzerConfig cfg;

// BuzzerConfig exactly as it existed at BZ06 (before currentMode) —
// kept so loadConfig() can migrate an existing device's settings
// forward instead of wiping them.
struct BuzzerConfigV6 {
  char magic[4];
  char name[24];
  char id[24];
  char wifiSSID[32];
  char wifiPassword[64];
  char dashUsername[20];
  char dashPassword[24];
  ManualSensorEntry manualSensors[MAX_MANUAL_SENSORS];
  bool buzzerPaused;
  uint32_t sessionStartEpoch;
  uint32_t lastAliveEpoch;
  uint32_t historyStart[5];
  uint32_t historyEnd[5];
  uint8_t historyCount;
  uint32_t wifiReconnectLog[5];
  uint8_t wifiReconnectCount;
  uint8_t displaySkin;
  char telegramBotToken[48];
  char telegramChatId[16];
  bool telegramEnabled;
  uint16_t checksum;
};

// BuzzerConfig exactly as it existed at BZ05 (before Telegram fields) —
// kept so loadConfig() can migrate an existing device's settings
// forward instead of wiping them.
struct BuzzerConfigV5 {
  char magic[4];
  char name[24];
  char id[24];
  char wifiSSID[32];
  char wifiPassword[64];
  char dashUsername[20];
  char dashPassword[24];
  ManualSensorEntry manualSensors[MAX_MANUAL_SENSORS];
  bool buzzerPaused;
  uint32_t sessionStartEpoch;
  uint32_t lastAliveEpoch;
  uint32_t historyStart[5];
  uint32_t historyEnd[5];
  uint8_t historyCount;
  uint32_t wifiReconnectLog[5];
  uint8_t wifiReconnectCount;
  uint8_t displaySkin;
  uint16_t checksum;
};

// BuzzerConfig exactly as it existed at BZ04 (before displaySkin) — kept
// so loadConfig() can migrate an existing device's WiFi/name/manual-
// sensor settings forward instead of wiping them just because one new
// cosmetic field was added. Same technique the Sensor Unit's
// storage.cpp uses for its own version history.
struct BuzzerConfigV4 {
  char magic[4];
  char name[24];
  char id[24];
  char wifiSSID[32];
  char wifiPassword[64];
  char dashUsername[20];
  char dashPassword[24];
  ManualSensorEntry manualSensors[MAX_MANUAL_SENSORS];
  bool buzzerPaused;
  uint32_t sessionStartEpoch;
  uint32_t lastAliveEpoch;
  uint32_t historyStart[5];
  uint32_t historyEnd[5];
  uint8_t historyCount;
  uint32_t wifiReconnectLog[5];
  uint8_t wifiReconnectCount;
  uint16_t checksum;
};

static uint16_t computeConfigChecksum(const BuzzerConfig &c) {
  const uint8_t *b = (const uint8_t*)&c;
  uint16_t sum = 0;
  for (size_t i = 0; i < offsetof(BuzzerConfig, checksum); i++) sum += b[i];
  return sum;
}
static uint16_t computeConfigChecksumV5(const BuzzerConfigV5 &c) {
  const uint8_t *b = (const uint8_t*)&c;
  uint16_t sum = 0;
  for (size_t i = 0; i < offsetof(BuzzerConfigV5, checksum); i++) sum += b[i];
  return sum;
}
static uint16_t computeConfigChecksumV6(const BuzzerConfigV6 &c) {
  const uint8_t *b = (const uint8_t*)&c;
  uint16_t sum = 0;
  for (size_t i = 0; i < offsetof(BuzzerConfigV6, checksum); i++) sum += b[i];
  return sum;
}
static uint16_t computeConfigChecksumV4(const BuzzerConfigV4 &c) {
  const uint8_t *b = (const uint8_t*)&c;
  uint16_t sum = 0;
  for (size_t i = 0; i < offsetof(BuzzerConfigV4, checksum); i++) sum += b[i];
  return sum;
}
void saveConfig() {
  memcpy(cfg.magic, BUZZ_MAGIC, 4);
  cfg.checksum = computeConfigChecksum(cfg);
  EEPROM.put(0, cfg);
  EEPROM.commit();
}
void loadConfig() {
  EEPROM.begin(BUZZ_EEPROM_SIZE);
  EEPROM.get(0, cfg);
  if (memcmp(cfg.magic, BUZZ_MAGIC, 4) == 0 && cfg.checksum == computeConfigChecksum(cfg)) {
    Serial.println("[CONFIG] Loaded config from EEPROM (BZ07).");
    return;
  }

  // Current-version load failed — check whether this is actually a
  // BZ06 device (the version right before currentMode was added)
  // before assuming the EEPROM is genuinely blank/corrupted.
  BuzzerConfigV6 v6;
  EEPROM.get(0, v6);
  if (memcmp(v6.magic, "BZ06", 4) == 0 && v6.checksum == computeConfigChecksumV6(v6)) {
    Serial.println("[CONFIG] Migrating BZ06 -> BZ07 (all settings preserved, currentMode defaults to home).");
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.name, v6.name, sizeof(cfg.name));
    memcpy(cfg.id, v6.id, sizeof(cfg.id));
    memcpy(cfg.wifiSSID, v6.wifiSSID, sizeof(cfg.wifiSSID));
    memcpy(cfg.wifiPassword, v6.wifiPassword, sizeof(cfg.wifiPassword));
    memcpy(cfg.dashUsername, v6.dashUsername, sizeof(cfg.dashUsername));
    memcpy(cfg.dashPassword, v6.dashPassword, sizeof(cfg.dashPassword));
    memcpy(cfg.manualSensors, v6.manualSensors, sizeof(cfg.manualSensors));
    cfg.buzzerPaused = v6.buzzerPaused;
    cfg.sessionStartEpoch = v6.sessionStartEpoch;
    cfg.lastAliveEpoch = v6.lastAliveEpoch;
    memcpy(cfg.historyStart, v6.historyStart, sizeof(cfg.historyStart));
    memcpy(cfg.historyEnd, v6.historyEnd, sizeof(cfg.historyEnd));
    cfg.historyCount = v6.historyCount;
    memcpy(cfg.wifiReconnectLog, v6.wifiReconnectLog, sizeof(cfg.wifiReconnectLog));
    cfg.wifiReconnectCount = v6.wifiReconnectCount;
    cfg.displaySkin = v6.displaySkin;
    memcpy(cfg.telegramBotToken, v6.telegramBotToken, sizeof(cfg.telegramBotToken));
    memcpy(cfg.telegramChatId, v6.telegramChatId, sizeof(cfg.telegramChatId));
    cfg.telegramEnabled = v6.telegramEnabled;
    strncpy(cfg.currentMode, "home", sizeof(cfg.currentMode) - 1);
    saveConfig();
    return;
  }

  // Current-version load failed — check whether this is actually a
  // BZ05 device (the version right before Telegram fields were added)
  // before assuming the EEPROM is genuinely blank/corrupted.
  BuzzerConfigV5 v5;
  EEPROM.get(0, v5);
  if (memcmp(v5.magic, "BZ05", 4) == 0 && v5.checksum == computeConfigChecksumV5(v5)) {
    Serial.println("[CONFIG] Migrating BZ05 -> BZ07 (all settings preserved, Telegram starts unconfigured, currentMode defaults to home).");
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.name, v5.name, sizeof(cfg.name));
    memcpy(cfg.id, v5.id, sizeof(cfg.id));
    memcpy(cfg.wifiSSID, v5.wifiSSID, sizeof(cfg.wifiSSID));
    memcpy(cfg.wifiPassword, v5.wifiPassword, sizeof(cfg.wifiPassword));
    memcpy(cfg.dashUsername, v5.dashUsername, sizeof(cfg.dashUsername));
    memcpy(cfg.dashPassword, v5.dashPassword, sizeof(cfg.dashPassword));
    memcpy(cfg.manualSensors, v5.manualSensors, sizeof(cfg.manualSensors));
    cfg.buzzerPaused = v5.buzzerPaused;
    cfg.sessionStartEpoch = v5.sessionStartEpoch;
    cfg.lastAliveEpoch = v5.lastAliveEpoch;
    memcpy(cfg.historyStart, v5.historyStart, sizeof(cfg.historyStart));
    memcpy(cfg.historyEnd, v5.historyEnd, sizeof(cfg.historyEnd));
    cfg.historyCount = v5.historyCount;
    memcpy(cfg.wifiReconnectLog, v5.wifiReconnectLog, sizeof(cfg.wifiReconnectLog));
    cfg.wifiReconnectCount = v5.wifiReconnectCount;
    cfg.displaySkin = v5.displaySkin;
    cfg.telegramEnabled = true; // default on, matching Sensor Unit's alarmEnabled default
    strncpy(cfg.currentMode, "home", sizeof(cfg.currentMode) - 1);
    saveConfig();
    return;
  }

  // Current-version load failed — check whether this is actually a
  // BZ04 device (the version right before displaySkin was added)
  // before assuming the EEPROM is genuinely blank/corrupted.
  BuzzerConfigV4 old;
  EEPROM.get(0, old);
  if (memcmp(old.magic, "BZ04", 4) == 0 && old.checksum == computeConfigChecksumV4(old)) {
    Serial.println("[CONFIG] Migrating BZ04 -> BZ06 (all settings preserved, displaySkin defaults to Classic, Telegram starts unconfigured).");
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.name, old.name, sizeof(cfg.name));
    memcpy(cfg.id, old.id, sizeof(cfg.id));
    memcpy(cfg.wifiSSID, old.wifiSSID, sizeof(cfg.wifiSSID));
    memcpy(cfg.wifiPassword, old.wifiPassword, sizeof(cfg.wifiPassword));
    memcpy(cfg.dashUsername, old.dashUsername, sizeof(cfg.dashUsername));
    memcpy(cfg.dashPassword, old.dashPassword, sizeof(cfg.dashPassword));
    memcpy(cfg.manualSensors, old.manualSensors, sizeof(cfg.manualSensors));
    cfg.buzzerPaused = old.buzzerPaused;
    cfg.sessionStartEpoch = old.sessionStartEpoch;
    cfg.lastAliveEpoch = old.lastAliveEpoch;
    memcpy(cfg.historyStart, old.historyStart, sizeof(cfg.historyStart));
    memcpy(cfg.historyEnd, old.historyEnd, sizeof(cfg.historyEnd));
    cfg.historyCount = old.historyCount;
    memcpy(cfg.wifiReconnectLog, old.wifiReconnectLog, sizeof(cfg.wifiReconnectLog));
    cfg.wifiReconnectCount = old.wifiReconnectCount;
    cfg.displaySkin = 0; // Classic — the only genuinely new field
    cfg.telegramEnabled = true;
    strncpy(cfg.currentMode, "home", sizeof(cfg.currentMode) - 1);
    saveConfig();
    return;
  }

  // Neither the current nor any previous version's checksum matched —
  // truly blank/foreign flash, or corrupted beyond recovery. Fall back
  // to factory defaults, same as before.
  memset(&cfg, 0, sizeof(cfg));
  strncpy(cfg.name, "Unnamed Buzzer", sizeof(cfg.name) - 1);
  strncpy(cfg.id, "Buzzer Unit", sizeof(cfg.id) - 1);
  cfg.displaySkin = 0;
  cfg.telegramEnabled = true;
  strncpy(cfg.currentMode, "home", sizeof(cfg.currentMode) - 1);
  saveConfig();
  Serial.println("[CONFIG] No valid config found — using defaults.");
}

// ---------------------------------------------------------------------
// Pins
// ---------------------------------------------------------------------
#define PIN_BUZZER      D5
#define PIN_BUTTON      D6
#define PIN_OLED_SDA    D2
#define PIN_OLED_SCL    D1

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDR     0x3C

#define AP_SSID     "BuzzerSetup"
#define AP_PASSWORD "12345678"
#define ESTOP_DURATION_MS (5UL * 60UL * 1000UL) // 5 minutes
#define ALIVE_SAVE_INTERVAL_MS (300000UL)        // 5 minutes

Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater; // Handles the entire /update OTA upload internally
bool apMode = false;

bool buzzerEStopActive = false;
unsigned long buzzerEStopStartMillis = 0;

bool sessionMarked = false;
unsigned long lastAliveSaveMillis = 0;
unsigned long lastNtpWarnMillis = 0;

// ---------------------------------------------------------------------
// Buzzer pattern engine (non-blocking)
// ---------------------------------------------------------------------
#define MAX_PATTERN_STEPS 4
struct PatternDef {
  int stepCount;
  unsigned long steps[MAX_PATTERN_STEPS];
};
PatternDef PATTERNS[] = {
  /* unused */ {2, {0, 0, 0, 0}},
  /* 1: Continuous       */ {2, {3600000UL, 0, 0, 0}},
  /* 2: Slow Pulse        */ {2, {600, 600, 0, 0}},
  /* 3: Fast Pulse         */ {2, {120, 120, 0, 0}},
  /* 4: Double-Beep Burst  */ {4, {150, 150, 150, 600}},
};
#define PATTERN_COUNT 4

bool buzzActive = false;
unsigned long buzzStartMillis = 0;
unsigned long buzzDurationMs = 0;
int currentPattern = 1;
int currentStep = 0;
unsigned long stepStartMillis = 0;
bool buzzerPinState = false;
unsigned long lastBuzzMillis = 0;
bool buzzWasRealTrigger = false; // true only for handleTrigger()/handlePulse() (real sensor relays) — never for handleTest(), so dashboard test beeps don't spam Telegram

// ---------------------------------------------------------------------
// Telegram integration (added BZ06) — deliberately minimal compared to
// the Sensor Unit's notify.cpp: no incoming-command polling (no /status
// command here), just outbound alerts for the handful of events that
// are genuinely useful to know about from the Buzzer Unit specifically:
// it started/stopped sounding a REAL alert, and it was paused/resumed
// from its own dashboard. Test beeps, WiFi reconnects, and e-stop
// toggles are deliberately NOT sent — kept quiet on purpose, matching
// the app-wide "declutter notifications" pass this was added alongside.
// ---------------------------------------------------------------------
bool isTelegramConfigured() {
  return strlen(cfg.telegramBotToken) > 0;
}

bool buzzerTestModeActive = false; // RAM-only — see Sensor Unit's notify.cpp for why this exists

void sendTelegramMessage(const String &text) {
  if (!isTelegramConfigured() || !cfg.telegramEnabled) return;

  String fullText = text;
  if (buzzerTestModeActive) fullText = "🧪 TEST MODE\n\n" + fullText;
  fullText += "\n\nDevice name: " + String(cfg.name) + ", ID: " + String(cfg.id);

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(512, 512); // keeps BearSSL's heap footprint small — see the matching comment in the Sensor Unit's notify.cpp for why this matters on an ESP8266
  client.setTimeout(4000);
  bool connected = client.connect("api.telegram.org", 443);
  if (!connected) {
    client.stop();
    delay(600);
    connected = client.connect("api.telegram.org", 443);
  }
  if (!connected) {
    Serial.println("[TELEGRAM] Connect failed (after retry).");
    return;
  }
  String msg = fullText;
  msg.replace("\n", "%0A");
  msg.replace(" ", "%20");
  String url = "/bot" + String(cfg.telegramBotToken) + "/sendMessage?chat_id=" +
               String(cfg.telegramChatId) + "&text=" + msg;
  client.print(String("GET ") + url + " HTTP/1.1\r\nHost: api.telegram.org\r\nConnection: close\r\n\r\n");
  unsigned long t0 = millis();
  while (client.connected() && millis() - t0 < 5000) {
    if (client.available()) client.read();
    yield();
  }
  client.stop();
  Serial.println("[TELEGRAM] Message sent.");
}

#define MAX_CALLERS 5
struct CallerEntry { String ip, name, id; long epoch; };
CallerEntry recentCallers[MAX_CALLERS];
int callerIdx = 0;

void logCaller(const String &ip, const String &name, const String &id) {
  time_t t = time(nullptr);
  long epoch = (t > 100000) ? (long)t : -1;
  recentCallers[callerIdx % MAX_CALLERS] = {ip, name, id, epoch};
  callerIdx++;
}

// ---------------------------------------------------------------------
// Auto-tracked sensors (pushed via /announce, /trigger, /pulse)
// ---------------------------------------------------------------------
#define MAX_SENSORS 5
struct SensorSlot {
  String ip, name, id;
  int shortPattern, shortSec, longPattern, longSec, thresholdSec;
  unsigned long lastAnnounceMillis;
  unsigned long lastTriggerMillis;
};
SensorSlot sensors[MAX_SENSORS];
int lastTriggerSensorIdx = -1;

int findOrCreateSensorSlot(const String &ip) {
  for (int i = 0; i < MAX_SENSORS; i++) {
    if (sensors[i].ip == ip) return i;
  }
  for (int i = 0; i < MAX_SENSORS; i++) {
    if (sensors[i].ip.length() == 0) { sensors[i].ip = ip; return i; }
  }
  int oldest = 0;
  for (int i = 1; i < MAX_SENSORS; i++) {
    if (sensors[i].lastAnnounceMillis < sensors[oldest].lastAnnounceMillis) oldest = i;
  }
  sensors[oldest] = SensorSlot();
  sensors[oldest].ip = ip;
  return oldest;
}

#define SENSOR_TIMEOUT_MS 90000UL
void pruneStaleSensors() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_SENSORS; i++) {
    if (sensors[i].ip.length() == 0) continue;
    unsigned long lastSeen = max(sensors[i].lastAnnounceMillis, sensors[i].lastTriggerMillis);
    if (lastSeen > 0 && now - lastSeen > SENSOR_TIMEOUT_MS) {
      sensors[i] = SensorSlot();
      if (lastTriggerSensorIdx == i) lastTriggerSensorIdx = -1;
    }
  }
}

int countConnectedSensors() {
  int n = 0;
  for (int i = 0; i < MAX_SENSORS; i++) if (sensors[i].ip.length() > 0) n++;
  return n;
}

// ---------------------------------------------------------------------
// OLED / display state
// ---------------------------------------------------------------------
bool oledOn = true;
bool lastButtonState = HIGH;
unsigned long lastButtonChangeMillis = 0;
const unsigned long DEBOUNCE_MS = 250;

enum DisplayState { DISP_BOOT_IP, DISP_IDLE, DISP_ALERT };
DisplayState dispState = DISP_BOOT_IP;
unsigned long bootIpShownAt = 0;
unsigned long bootAnimStart = 0;

// ---------------------------------------------------------------------
// Small drawing helpers
// ---------------------------------------------------------------------
static void drawCenteredText(const char* s, int y, uint8_t size, uint16_t color) {
  oled.setTextSize(size);
  oled.setTextColor(color);
  int16_t x1, y1; uint16_t w, h;
  oled.getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  oled.setCursor((SCREEN_WIDTH - w) / 2, y);
  oled.print(s);
}

static void drawDashedHLine(int x, int y, int w, int on, int off, uint16_t color) {
  int i = 0;
  while (i < w) {
    int seg = min(on, w - i);
    oled.drawFastHLine(x + i, y, seg, color);
    i += on + off;
  }
}

static void drawWifiIcon(int x, int y, bool ok) {
  uint16_t c = SSD1306_WHITE;
  oled.fillCircle(x + 6, y + 8, 1, c);
  if (!ok) {
    oled.drawLine(x, y, x + 12, y + 8, c);
    oled.drawLine(x + 12, y, x, y + 8, c);
    return;
  }
  for (int i = 0; i < 3; i++) {
    int r = 3 + i * 3;
    oled.drawCircleHelper(x + 6, y + 8, r, 0x3, c);
  }
}

static String cachedSensorListText = "";
static unsigned long lastListBuildMillis = 0;

String buildSensorListText() {
  unsigned long now = millis();
  if (now - lastListBuildMillis < 1000 && lastListBuildMillis != 0) return cachedSensorListText;
  lastListBuildMillis = now;

  String list = "";
  for (int i = 0; i < MAX_SENSORS; i++) {
    if (sensors[i].ip.length() == 0) continue;
    if (list.length() > 0) list += ", ";
    String nm = sensors[i].name.length() ? sensors[i].name : String("Sensor");
    list += nm;
    if (sensors[i].id.length()) list += " (" + sensors[i].id + ")";
  }
  cachedSensorListText = list;
  return list;
}

// ---------------------------------------------------------------------
// BOOT screen
// ---------------------------------------------------------------------
void drawBootIpScreen() {
  unsigned long now = millis();
  unsigned long t = now - bootAnimStart;
  oled.clearDisplay();

  oled.drawRoundRect(0, 0, SCREEN_WIDTH, 16, 3, SSD1306_WHITE);
  drawCenteredText("BUZZER  UNIT", 4, 1, SSD1306_WHITE);

  drawDashedHLine(0, 20, SCREEN_WIDTH, 2, 2, SSD1306_WHITE);

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  const char* full = apMode ? "SETUP MODE" : (wifiOk ? "SYSTEM ONLINE" : "LINKING WIFI");
  int fullLen = strlen(full);
  int shown = min((int)(t / 90), fullLen);
  char buf[24]; memset(buf, 0, sizeof(buf));
  strncpy(buf, full, shown);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(4, 26);
  oled.print(buf);
  if (((now / 350) % 2) == 0 && shown < 20) oled.print('_');

  oled.setCursor(4, 40);
  if (apMode) {
    oled.print("AP ");
    oled.print(WiFi.softAPIP().toString());
  } else {
    oled.print("IP ");
    oled.print(wifiOk ? WiFi.localIP().toString() : String("--.--.--.--"));
  }

  int barW = SCREEN_WIDTH - 8;
  int fill = min((int)(t * barW / 2500), barW);
  oled.drawRoundRect(4, 54, barW, 8, 2, SSD1306_WHITE);
  oled.fillRoundRect(4, 54, fill, 8, 2, SSD1306_WHITE);

  oled.display();
}

// ---------------------------------------------------------------------
// IDLE / "Listening" screen
// ---------------------------------------------------------------------
static void formatUptime(unsigned long ms, char *buf, size_t bufSize) {
  unsigned long s = ms / 1000;
  if (s < 1000) { snprintf(buf, bufSize, "%lus", s); return; }
  unsigned long m = s / 60;
  if (m < 1000) { snprintf(buf, bufSize, "%lum", m); return; }
  unsigned long h = m / 60;
  if (h < 1000) { snprintf(buf, bufSize, "%luh", h); return; }
  unsigned long d = h / 24;
  snprintf(buf, bufSize, "%lud", d);
}

// ---------------------------------------------------------------------
// Idle-screen skins — parallel to the Sensor Unit's own reading-screen
// skins (same 10-name set, same visual language), applied here to
// whatever the Buzzer Unit's idle screen actually has to show: uptime,
// WiFi, connected-sensor count/names, and listening/silenced state.
// Selected the same way — persisted in cfg.displaySkin, changed via
// /setskin, applied immediately with no restart.
// ---------------------------------------------------------------------
#define DISPLAY_SKIN_COUNT 30

static const char* SKIN_NAMES[DISPLAY_SKIN_COUNT] = {
  "Classic Numeric",
  "Bar Gauge",
  "Radar Sweep",
  "Minimal Shield",
  "Security HUD",
  "Sonar Pulse",
  "Retro Terminal",
  "Grid Dashboard",
  "Analog Gauge",
  "Big Digit",
  "Heartbeat Monitor",
  "Tachometer",
  "Thermometer",
  "Equalizer Bars",
  "VU Meter",
  "Digital Matrix",
  "CRT Scanlines",
  "Orbit Monitor",
  "Ripple Wave",
  "Compass Dial",
  "Pixel Guard",
  "Matrix Rain",
  "Fingerprint Scan",
  "Combination Lock",
  "Flame Alert",
  "Frost Idle",
  "Lightning Pulse",
  "Star Field",
  "Hourglass Timer",
  "Constellation",
};

const char* getSkinName(uint8_t skin) {
  if (skin >= DISPLAY_SKIN_COUNT) return SKIN_NAMES[0];
  return SKIN_NAMES[skin];
}

// Cached WiFi signal bar count (0-4), refreshed at most every 5s —
// shared by every skin that wants to show signal strength, so none of
// them need their own separate RSSI-polling timer.
static int getSignalBars(bool wifiOk) {
  static int cachedBars = 0;
  static unsigned long lastRssiCheckMillis = 0;
  unsigned long now = millis();
  if (now - lastRssiCheckMillis >= 5000 || lastRssiCheckMillis == 0) {
    lastRssiCheckMillis = now;
    int rssi = wifiOk ? WiFi.RSSI() : -100;
    if (rssi > -55) cachedBars = 4;
    else if (rssi > -65) cachedBars = 3;
    else if (rssi > -75) cachedBars = 2;
    else if (rssi > -85) cachedBars = 1;
    else cachedBars = 0;
  }
  return cachedBars;
}

// Skin 0 — Classic Numeric. The original always-on layout: header bar
// with uptime, WiFi + connected-sensor-count row, scrolling sensor name
// marquee, listening/silenced status with animated dots, signal bars.
static void skinDrawClassic(unsigned long now) {
  oled.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
  drawCenteredText("BUZZER UNIT", 2, 1, SSD1306_BLACK);
  char up[10];
  formatUptime(now, up, sizeof(up));
  int16_t x1, y1; uint16_t w, h;
  oled.setTextSize(1);
  oled.getTextBounds(up, 0, 0, &x1, &y1, &w, &h);
  oled.setTextColor(SSD1306_BLACK);
  oled.setCursor(SCREEN_WIDTH - w - 3, 2);
  oled.print(up);

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  drawWifiIcon(4, 16, wifiOk);

  int connectedCount = countConnectedSensors();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(22, 16);
  oled.print("Sensors: ");
  oled.print(connectedCount);
  oled.print("/5");

  if (buzzerEStopActive) {
    oled.setCursor(2, 16 + 12);
    oled.print("** PAUSED / E-STOP **");
  }

  int marqueeY = 30;
  oled.setTextWrap(false);
  if (connectedCount == 0) {
    drawCenteredText("No sensors connected", marqueeY, 1, SSD1306_WHITE);
  } else if (connectedCount == 1) {
    String list = buildSensorListText();
    oled.setTextSize(1);
    oled.getTextBounds(list.c_str(), 0, 0, &x1, &y1, &w, &h);
    int marqueeW = SCREEN_WIDTH - 4;
    while (list.length() > 3 && (int)w > marqueeW) {
      list = list.substring(0, list.length() - 4) + "...";
      oled.getTextBounds(list.c_str(), 0, 0, &x1, &y1, &w, &h);
    }
    drawCenteredText(list.c_str(), marqueeY, 1, SSD1306_WHITE);
  } else {
    String list = buildSensorListText();
    oled.setTextSize(1);
    oled.getTextBounds(list.c_str(), 0, 0, &x1, &y1, &w, &h);
    int marqueeW = SCREEN_WIDTH - 4;
    oled.setTextColor(SSD1306_WHITE);
    if ((int)w <= marqueeW) {
      drawCenteredText(list.c_str(), marqueeY, 1, SSD1306_WHITE);
    } else {
      int gap = 24;
      int total = w + gap;
      int offset = (int)((now / 40) % (unsigned long)total);
      int x1pos = 2 - offset;
      oled.setCursor(x1pos, marqueeY);
      oled.print(list);
      int x2pos = x1pos + total;
      if (x2pos < SCREEN_WIDTH) {
        oled.setCursor(x2pos, marqueeY);
        oled.print(list);
      }
    }
  }

  drawDashedHLine(0, 52, SCREEN_WIDTH, 2, 2, SSD1306_WHITE);
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(2, 54);
  oled.print(cfg.buzzerPaused || buzzerEStopActive ? "SILENCED" : "LISTENING");
  int dots = (now / 400) % 4;
  for (int i = 0; i < dots; i++) oled.print('.');

  int cachedBars = getSignalBars(wifiOk);
  for (int i = 0; i < 4; i++) {
    int bx = SCREEN_WIDTH - 2 - (4 - i) * 3;
    int bh = 2 + i * 2;
    if (i < cachedBars) oled.fillRect(bx, 62 - bh, 2, bh, SSD1306_WHITE);
    else oled.drawRect(bx, 62 - bh, 2, bh, SSD1306_WHITE);
  }
}

// Skin 1 — Bar Gauge. Connected-sensor count shown as a fill bar
// (X/5), signal strength as a second smaller bar — a quick-glance
// "how much is connected / how strong is WiFi" pair of gauges.
static void skinDrawBarGauge(unsigned long now) {
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  drawCenteredText(listening ? "LISTENING" : "SILENCED", 2, 1, SSD1306_WHITE);
  drawDashedHLine(0, 12, SCREEN_WIDTH, 2, 2, SSD1306_WHITE);

  int connectedCount = countConnectedSensors();
  char buf[16];
  snprintf(buf, sizeof(buf), "Sensors %d/5", connectedCount);
  drawCenteredText(buf, 18, 1, SSD1306_WHITE);

  int barX = 6, barY = 30, barW = SCREEN_WIDTH - 12, barH = 12;
  oled.drawRoundRect(barX, barY, barW, barH, 3, SSD1306_WHITE);
  int fillW = (int)((barW - 4) * (connectedCount / 5.0f));
  if (fillW > 0) oled.fillRoundRect(barX + 2, barY + 2, fillW, barH - 4, 2, SSD1306_WHITE);

  int bars = getSignalBars(wifiOk);
  drawCenteredText("Signal", 46, 1, SSD1306_WHITE);
  int sigBarX = SCREEN_WIDTH / 2 - 20, sigBarY = 56, sigBarW = 40, sigBarH = 6;
  oled.drawRoundRect(sigBarX, sigBarY, sigBarW, sigBarH, 2, SSD1306_WHITE);
  int sigFill = (int)((sigBarW - 4) * (bars / 4.0f));
  if (sigFill > 0) oled.fillRoundRect(sigBarX + 2, sigBarY + 2, sigFill, sigBarH - 4, 1, SSD1306_WHITE);
}

// Skin 2 — Radar Sweep. Same rotating-line-in-rings visual as the
// Sensor Unit, but the "blips" are the connected sensors — one dot per
// connected sensor, spaced evenly around the sweep.
static void skinDrawRadarSweep(unsigned long now) {
  const int cx = 64, cy = 32, maxR = 24;
  oled.drawCircle(cx, cy, maxR, SSD1306_WHITE);
  oled.drawCircle(cx, cy, maxR * 2 / 3, SSD1306_WHITE);
  oled.drawFastHLine(cx - maxR, cy, maxR * 2, SSD1306_WHITE);
  oled.drawFastVLine(cx, cy - maxR, maxR * 2, SSD1306_WHITE);

  float angle = fmod(millis() / 3000.0f, 1.0f) * 2.0f * PI;
  int lx = cx + (int)(cos(angle) * maxR);
  int ly = cy + (int)(sin(angle) * maxR);
  oled.drawLine(cx, cy, lx, ly, SSD1306_WHITE);

  int connectedCount = countConnectedSensors();
  for (int i = 0; i < connectedCount; i++) {
    float a = (2.0f * PI * i) / 5.0f; // fixed 5 evenly-spaced slots, matching MAX_SENSORS
    int bx = cx + (int)(cos(a) * (maxR * 2 / 3));
    int by = cy + (int)(sin(a) * (maxR * 2 / 3));
    oled.fillCircle(bx, by, 2, SSD1306_WHITE);
  }

  char buf[10];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 56, 1, SSD1306_WHITE);
}

// Skin 3 — Minimal Shield. A big bell/speaker glyph that's solid when
// listening and outlined-only when silenced — the buzzer's equivalent
// of the Sensor Unit's open/closed padlock.
static void skinDrawMinimalShield(unsigned long now) {
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  const int cx = 64, cy = 24;
  if (listening) {
    oled.fillTriangle(cx - 14, cy + 10, cx + 14, cy + 10, cx, cy - 14, SSD1306_WHITE);
    oled.fillCircle(cx, cy + 14, 4, SSD1306_WHITE);
  } else {
    oled.drawTriangle(cx - 14, cy + 10, cx + 14, cy + 10, cx, cy - 14, SSD1306_WHITE);
    oled.drawCircle(cx, cy + 14, 4, SSD1306_WHITE);
    oled.drawLine(cx - 16, cy - 16, cx + 16, cy + 16, SSD1306_WHITE); // slash through it
  }

  int connectedCount = countConnectedSensors();
  char buf[16];
  snprintf(buf, sizeof(buf), "%d sensor%s linked", connectedCount, connectedCount == 1 ? "" : "s");
  drawCenteredText(buf, 50, 1, SSD1306_WHITE);
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  drawWifiIcon(4, 4, wifiOk);
}

// Skin 4 — Security HUD. Same viewfinder-corner treatment as the
// Sensor Unit, sensor count as the big central readout.
static void skinDrawSecurityHud(unsigned long now) {
  const int m = 4, len = 10;
  oled.drawFastHLine(m, m, len, SSD1306_WHITE); oled.drawFastVLine(m, m, len, SSD1306_WHITE);
  oled.drawFastHLine(SCREEN_WIDTH - m - len, m, len, SSD1306_WHITE); oled.drawFastVLine(SCREEN_WIDTH - m - 1, m, len, SSD1306_WHITE);
  oled.drawFastHLine(m, SCREEN_HEIGHT - m - 1, len, SSD1306_WHITE); oled.drawFastVLine(m, SCREEN_HEIGHT - m - len, len, SSD1306_WHITE);
  oled.drawFastHLine(SCREEN_WIDTH - m - len, SCREEN_HEIGHT - m - 1, len, SSD1306_WHITE); oled.drawFastVLine(SCREEN_WIDTH - m - 1, SCREEN_HEIGHT - m - len, len, SSD1306_WHITE);

  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  if (listening && (millis() / 500) % 2 == 0) oled.fillCircle(SCREEN_WIDTH - 12, 12, 3, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(16, 6);
  oled.print(listening ? "LISTENING" : "SILENCED");

  int connectedCount = countConnectedSensors();
  char buf[6];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 22, 3, SSD1306_WHITE);
  drawCenteredText("sensors linked", 50, 1, SSD1306_WHITE);
}

// Skin 5 — Sonar Pulse. Pulsing rings around the connected-sensor
// count, pulsing faster while actively sounding an alert (handled
// separately by drawAlertScreen — this is purely the idle look).
static void skinDrawSonarPulse(unsigned long now) {
  const int cx = 64, cy = 28, maxR = 22;
  float phase = fmod((float)(millis() % 2000UL) / 2000.0f, 1.0f);
  for (int ring = 0; ring < 3; ring++) {
    float r = fmod(phase + ring / 3.0f, 1.0f) * maxR;
    if (r > 2) oled.drawCircle(cx, cy, (int)r, SSD1306_WHITE);
  }
  oled.fillCircle(cx, cy, 2, SSD1306_WHITE);

  int connectedCount = countConnectedSensors();
  char buf[10];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 54, 1, SSD1306_WHITE);
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  drawWifiIcon(4, 4, wifiOk);
}

// Skin 6 — Retro Terminal. Bracketed status lines, matching the Sensor
// Unit's terminal styling exactly for a consistent "family" look.
static void skinDrawRetroTerminal(unsigned long now) {
  oled.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  oled.setCursor(4, 4);
  oled.print("[");
  oled.print(listening ? "LISTENING" : "SILENCED ");
  oled.print("]");

  oled.setCursor(4, 16);
  int connectedCount = countConnectedSensors();
  char buf[20];
  snprintf(buf, sizeof(buf), "[SENSORS %d/5]", connectedCount);
  oled.print(buf);

  oled.setCursor(4, 28);
  char up[10];
  formatUptime(now, up, sizeof(up));
  oled.print("[UPTIME ");
  oled.print(up);
  oled.print("]");

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  oled.setCursor(4, 40);
  oled.print("[NET ");
  oled.print(wifiOk ? "OK" : "--");
  oled.print("]");

  oled.setCursor(4, 52);
  oled.print("> ready");
  if ((millis() / 500) % 2 == 0) oled.fillRect(4 + 7 * 6, 52, 6, 8, SSD1306_WHITE);
}

// Skin 7 — Grid Dashboard. Four quadrants: Sensors | Status | WiFi | Uptime.
static void skinDrawGridDashboard(unsigned long now) {
  const int midX = SCREEN_WIDTH / 2, midY = SCREEN_HEIGHT / 2;
  oled.drawFastVLine(midX, 0, SCREEN_HEIGHT, SSD1306_WHITE);
  oled.drawFastHLine(0, midY, SCREEN_WIDTH, SSD1306_WHITE);
  oled.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);

  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  oled.setCursor(4, 4);
  oled.print("SENSORS");
  int connectedCount = countConnectedSensors();
  oled.setCursor(4, 18);
  oled.print(connectedCount);
  oled.print("/5");

  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  oled.setCursor(midX + 4, 4);
  oled.print("STATUS");
  oled.setCursor(midX + 4, 18);
  oled.print(listening ? "READY" : "OFF");

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  oled.setCursor(4, midY + 4);
  oled.print("WIFI");
  drawWifiIcon(6, midY + 16, wifiOk);

  oled.setCursor(midX + 4, midY + 4);
  oled.print("UPTIME");
  char up[10];
  formatUptime(now, up, sizeof(up));
  oled.setCursor(midX + 4, midY + 18);
  oled.print(up);
}

// Skin 8 — Analog Gauge. Needle position reflects how many of the 5
// sensor slots are filled — a half-circle dial instead of a number.
static void skinDrawAnalogGauge(unsigned long now) {
  const int cx = 64, cy = 46, r = 34;
  for (int a = 180; a <= 360; a += 6) {
    float rad = a * PI / 180.0f;
    int x1 = cx + (int)(cos(rad) * r), y1 = cy + (int)(sin(rad) * r);
    int x2 = cx + (int)(cos(rad) * (r - 3)), y2 = cy + (int)(sin(rad) * (r - 3));
    oled.drawLine(x1, y1, x2, y2, SSD1306_WHITE);
  }

  int connectedCount = countConnectedSensors();
  float ratio = connectedCount / 5.0f;
  float needleAngle = (180.0f + ratio * 180.0f) * PI / 180.0f;
  int nx = cx + (int)(cos(needleAngle) * (r - 6));
  int ny = cy + (int)(sin(needleAngle) * (r - 6));
  oled.drawLine(cx, cy, nx, ny, SSD1306_WHITE);
  oled.fillCircle(cx, cy, 2, SSD1306_WHITE);

  char buf[10];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 54, 1, SSD1306_WHITE);
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2);
  oled.print(listening ? "LISTEN" : "OFF");
}

// Skin 9 — Big Digit. Enormous connected-sensor count, minimal chrome.
static void skinDrawBigDigit(unsigned long now) {
  int connectedCount = countConnectedSensors();
  char buf[4];
  snprintf(buf, sizeof(buf), "%d", connectedCount);
  drawCenteredText(buf, 14, 4, SSD1306_WHITE);
  drawCenteredText("sensors linked", 50, 1, SSD1306_WHITE);

  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2);
  oled.print(listening ? "ON" : "OFF");
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  oled.setCursor(SCREEN_WIDTH - 26, 2);
  oled.print(wifiOk ? "NET" : "---");
}

// Skin 10 — Heartbeat Monitor. ECG-style waveform, spiking when the
// buzzer actively sounds rather than at a fixed trigger.
static void skinDrawHeartbeat(unsigned long now) {
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2);
  oled.print(listening ? "LISTEN" : "OFF");
  int connectedCount = countConnectedSensors();
  char buf[10];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  int16_t x1, y1; uint16_t w, h;
  oled.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  oled.setCursor(SCREEN_WIDTH - w - 2, 2);
  oled.print(buf);

  const int baseY = 40;
  int prevX = 0, prevY = baseY;
  unsigned long phase = millis() / 12;
  for (int x = 0; x <= SCREEN_WIDTH; x += 2) {
    unsigned long t = (x + phase) % 40;
    int y = baseY;
    if (buzzActive) {
      if (t == 18) y = baseY - 18;
      else if (t == 20) y = baseY + 8;
      else if (t == 22) y = baseY - 4;
    } else {
      if (t == 18) y = baseY - 6;
    }
    oled.drawLine(prevX, prevY, x, y, SSD1306_WHITE);
    prevX = x; prevY = y;
  }
  drawDashedHLine(0, 52, SCREEN_WIDTH, 2, 2, SSD1306_WHITE);
}

// Skin 11 — Tachometer. Needle position reflects connected-sensor ratio.
static void skinDrawTachometer(unsigned long now) {
  const int cx = 64, cy = 38, r = 30;
  const float startDeg = 135, sweepDeg = 270;
  for (int i = 0; i <= 20; i++) {
    float a = (startDeg + sweepDeg * i / 20.0f) * PI / 180.0f;
    oled.drawLine(cx + (int)(cos(a) * r), cy + (int)(sin(a) * r),
                  cx + (int)(cos(a) * (r - 4)), cy + (int)(sin(a) * (r - 4)), SSD1306_WHITE);
  }
  int connectedCount = countConnectedSensors();
  float ratio = connectedCount / 5.0f;
  float needleA = (startDeg + sweepDeg * ratio) * PI / 180.0f;
  oled.drawLine(cx, cy, cx + (int)(cos(needleA) * (r - 8)), cy + (int)(sin(needleA) * (r - 8)), SSD1306_WHITE);
  oled.fillCircle(cx, cy, 2, SSD1306_WHITE);
  char buf[10];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 56, 1, SSD1306_WHITE);
}

// Skin 12 — Thermometer. Fill level reflects connected-sensor ratio.
static void skinDrawThermometer(unsigned long now) {
  const int tubeX = 30, tubeTop = 4, tubeBottom = 50, tubeW = 8;
  const int bulbCy = tubeBottom + 6, bulbR = 7;
  oled.drawRoundRect(tubeX, tubeTop, tubeW, tubeBottom - tubeTop, 4, SSD1306_WHITE);
  oled.drawCircle(tubeX + tubeW / 2, bulbCy, bulbR, SSD1306_WHITE);
  int connectedCount = countConnectedSensors();
  float fillRatio = connectedCount / 5.0f;
  int fillH = (int)((tubeBottom - tubeTop - 4) * fillRatio);
  oled.fillCircle(tubeX + tubeW / 2, bulbCy, bulbR - 2, SSD1306_WHITE);
  if (fillH > 0) oled.fillRoundRect(tubeX + 2, tubeBottom - 2 - fillH, tubeW - 4, fillH, 2, SSD1306_WHITE);
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(48, 6); oled.print(listening ? "LISTEN" : "OFF");
  char buf[10];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  oled.setCursor(48, 20); oled.print(buf);
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  drawWifiIcon(48, 36, wifiOk);
}

// Skin 13 — Equalizer Bars. Bars snap to full height while sounding.
static void skinDrawEqualizer(unsigned long now) {
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  drawCenteredText(listening ? "LISTENING" : "OFF", 2, 1, SSD1306_WHITE);
  const int barCount = 12, barW = 6, gap = 2, baseY = 50, maxH = 32;
  int totalW = barCount * (barW + gap) - gap;
  int startX = (SCREEN_WIDTH - totalW) / 2;
  for (int i = 0; i < barCount; i++) {
    unsigned long seed = (millis() / 90) + i * 37;
    int h = buzzActive ? (maxH - (int)((seed * 7) % 6)) : (6 + (int)((seed * 13) % (maxH - 10)));
    oled.fillRect(startX + i * (barW + gap), baseY - h, barW, h, SSD1306_WHITE);
  }
  int connectedCount = countConnectedSensors();
  char buf[10];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 55, 1, SSD1306_WHITE);
}

// Skin 14 — VU Meter. Needle bounces, base position from sensor ratio.
static void skinDrawVuMeter(unsigned long now) {
  const int cx = 64, cy = 54, r = 40;
  for (int a = 200; a <= 340; a += 10) {
    float rad = a * PI / 180.0f;
    oled.drawLine(cx + (int)(cos(rad) * r), cy + (int)(sin(rad) * r),
                  cx + (int)(cos(rad) * (r - 4)), cy + (int)(sin(rad) * (r - 4)), SSD1306_WHITE);
  }
  int connectedCount = countConnectedSensors();
  float baseRatio = connectedCount / 5.0f;
  float jitter = buzzActive ? (sin(millis() / 60.0f) * 0.06f) : (sin(millis() / 260.0f) * 0.03f);
  float ratio = baseRatio + jitter;
  if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
  float needleA = (200 + 140 * ratio) * PI / 180.0f;
  oled.drawLine(cx, cy, cx + (int)(cos(needleA) * (r - 8)), cy + (int)(sin(needleA) * (r - 8)), SSD1306_WHITE);
  oled.fillCircle(cx, cy, 2, SSD1306_WHITE);
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2); oled.print(listening ? "LISTEN" : "OFF");
}

// Skin 15 — Digital Matrix. Sensor/WiFi/listening state as a raw readout.
static void skinDrawDigitalMatrix(unsigned long now) {
  oled.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(4, 4);
  oled.print("STATE: ");
  oled.print(listening ? "ON" : "OFF");
  int connectedCount = countConnectedSensors();
  char big[6];
  snprintf(big, sizeof(big), "%d/5", connectedCount);
  drawCenteredText(big, 20, 2, SSD1306_WHITE);
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  oled.setCursor(4, 42);
  oled.print("NET:");
  oled.print(wifiOk ? "1" : "0");
  oled.print(" BUZ:");
  oled.print(buzzActive ? "1" : "0");
  char up[10];
  formatUptime(now, up, sizeof(up));
  oled.setCursor(4, 52);
  oled.print("UP:");
  oled.print(up);
}

// Skin 16 — CRT Scanlines.
static void skinDrawCrtScanlines(unsigned long now) {
  int connectedCount = countConnectedSensors();
  char buf[6];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 16, 3, SSD1306_WHITE);
  for (int y = 0; y < SCREEN_HEIGHT; y += 2) oled.drawFastHLine(0, y, SCREEN_WIDTH, SSD1306_WHITE);
  drawCenteredText(buf, 16, 3, SSD1306_WHITE);
  oled.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(4, 4); oled.print(listening ? "LISTENING" : "SILENCED");
}

// Skin 17 — Orbit Monitor. One dot per connected sensor, orbiting.
static void skinDrawOrbitMonitor(unsigned long now) {
  const int cx = 64, cy = 30, r = 22;
  oled.drawCircle(cx, cy, r, SSD1306_WHITE);
  oled.fillCircle(cx, cy, 3, SSD1306_WHITE);
  int connectedCount = countConnectedSensors();
  for (int i = 0; i < connectedCount; i++) {
    float speed = 2400.0f - i * 200.0f;
    float angle = fmod(millis() / speed + (i * 0.3f), 1.0f) * 2.0f * PI;
    oled.fillCircle(cx + (int)(cos(angle) * r), cy + (int)(sin(angle) * r), 2, SSD1306_WHITE);
  }
  char buf[10];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 56, 1, SSD1306_WHITE);
}

// Skin 18 — Ripple Wave.
static void skinDrawRippleWave(unsigned long now) {
  const int cx = 64, cy = 32, maxR = 28;
  unsigned long cycleMs = 2400;
  for (int i = 0; i < 4; i++) {
    float phase = fmod((millis() + i * (cycleMs / 4)) / (float)cycleMs, 1.0f);
    int r = (int)(phase * maxR);
    if (r > 1 && r < maxR) oled.drawCircle(cx, cy, r, SSD1306_WHITE);
  }
  if (buzzActive) oled.fillCircle(cx, cy, 3, SSD1306_WHITE);
  else oled.drawCircle(cx, cy, 2, SSD1306_WHITE);
  int connectedCount = countConnectedSensors();
  char buf[10];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 56, 1, SSD1306_WHITE);
}

// Skin 19 — Compass Dial.
static void skinDrawCompassDial(unsigned long now) {
  const int cx = 64, cy = 34, r = 24;
  oled.drawCircle(cx, cy, r, SSD1306_WHITE);
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(cx - 3, cy - r - 9); oled.print("N");
  oled.setCursor(cx - 3, cy + r + 1); oled.print("S");
  oled.setCursor(cx - r - 9, cy - 4); oled.print("W");
  oled.setCursor(cx + r + 1, cy - 4); oled.print("E");
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  unsigned long periodMs = listening ? 3000UL : 8000UL;
  float angle = fmod(millis() / (float)periodMs, 1.0f) * 2.0f * PI;
  oled.drawLine(cx, cy, cx + (int)(cos(angle) * (r - 4)), cy + (int)(sin(angle) * (r - 4)), SSD1306_WHITE);
  oled.fillCircle(cx, cy, 2, SSD1306_WHITE);
  int connectedCount = countConnectedSensors();
  char buf[6];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 58, 1, SSD1306_WHITE);
}

// Skin 20 — Pixel Guard.
static void skinDrawPixelGuard(unsigned long now) {
  const int bx = 48, by = 6, s = 4;
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  bool solid = !buzzActive || (millis() / 300) % 2 == 0;
  if (solid) oled.fillRect(bx + s * 2, by, s * 4, s * 3, SSD1306_WHITE);
  else oled.drawRect(bx + s * 2, by, s * 4, s * 3, SSD1306_WHITE);
  if (solid) oled.fillRect(bx, by + s * 3, s * 8, s * 5, SSD1306_WHITE);
  else oled.drawRect(bx, by + s * 3, s * 8, s * 5, SSD1306_WHITE);
  if (listening) {
    oled.fillRect(bx + s * 3, by + s, s, s, SSD1306_BLACK);
    oled.fillRect(bx + s * 4, by + s, s, s, SSD1306_BLACK);
  }
  int connectedCount = countConnectedSensors();
  char buf[16];
  snprintf(buf, sizeof(buf), "%d linked", connectedCount);
  drawCenteredText(buf, 50, 1, SSD1306_WHITE);
}

// Skin 21 — Matrix Rain.
static void skinDrawMatrixRain(unsigned long now) {
  const int cols = 16, colW = SCREEN_WIDTH / cols;
  for (int c = 0; c < cols; c++) {
    unsigned long seed = c * 97 + 13;
    int y = (int)((millis() / (10 + (seed % 15)) + seed * 5) % (SCREEN_HEIGHT + 10)) - 10;
    oled.drawFastVLine(c * colW + colW / 2, y, 5, SSD1306_WHITE);
  }
  oled.fillRect(14, 22, 100, 20, SSD1306_BLACK);
  oled.drawRect(14, 22, 100, 20, SSD1306_WHITE);
  int connectedCount = countConnectedSensors();
  char buf[10];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 28, 1, SSD1306_WHITE);
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2); oled.print(listening ? "ON" : "OFF");
}

// Skin 22 — Fingerprint Scan.
static void skinDrawFingerprintScan(unsigned long now) {
  const int cx = 64, cy = 30;
  for (int i = 0; i < 5; i++) {
    int r = 6 + i * 4;
    oled.drawCircleHelper(cx, cy, r, 0b0110, SSD1306_WHITE);
    oled.drawCircleHelper(cx, cy, r, 0b1001, SSD1306_WHITE);
  }
  if (buzzActive) oled.fillCircle(cx, cy, 3, SSD1306_WHITE);
  int connectedCount = countConnectedSensors();
  char buf[10];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 56, 1, SSD1306_WHITE);
}

// Skin 23 — Combination Lock.
static void skinDrawCombinationLock(unsigned long now) {
  const int cx = 64, cy = 32, r = 26;
  oled.drawCircle(cx, cy, r, SSD1306_WHITE);
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  float rotation = listening ? (millis() / 4000.0f) : 0.0f;
  for (int i = 0; i < 20; i++) {
    float a = (2 * PI * i / 20.0f) + rotation;
    oled.drawLine(cx + (int)(cos(a) * r), cy + (int)(sin(a) * r),
                  cx + (int)(cos(a) * (r - (i % 5 == 0 ? 6 : 3))), cy + (int)(sin(a) * (r - (i % 5 == 0 ? 6 : 3))), SSD1306_WHITE);
  }
  oled.fillTriangle(cx - 4, cy - r - 6, cx + 4, cy - r - 6, cx, cy - r + 2, SSD1306_WHITE);
  int connectedCount = countConnectedSensors();
  char buf[6];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, cy - 4, 1, SSD1306_WHITE);
  drawCenteredText(listening ? "ACTIVE" : "LOCKED", 56, 1, SSD1306_WHITE);
}

// Skin 24 — Flame Alert. Most striking while actively sounding.
static void skinDrawFlameAlert(unsigned long now) {
  const int cx = 64, baseY = 46;
  unsigned long t = millis() / 120;
  int flick = (t % 3) - 1;
  oled.fillTriangle(cx - 14, baseY, cx + 14, baseY, cx + flick, baseY - 30, SSD1306_WHITE);
  if (!buzzActive) {
    oled.fillTriangle(cx - 7, baseY - 4, cx + 7, baseY - 4, cx + flick, baseY - 18, SSD1306_BLACK);
  }
  int connectedCount = countConnectedSensors();
  char buf[10];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 52, 1, SSD1306_WHITE);
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2); oled.print(listening ? "ON" : "OFF");
}

// Skin 25 — Frost Idle.
static void skinDrawFrostIdle(unsigned long now) {
  const int cx = 64, cy = 28, r = 18;
  for (int i = 0; i < 6; i++) {
    float a = i * PI / 3.0f;
    int ex = cx + (int)(cos(a) * r), ey = cy + (int)(sin(a) * r);
    oled.drawLine(cx, cy, ex, ey, SSD1306_WHITE);
    int mx = cx + (int)(cos(a) * r * 0.6f), my = cy + (int)(sin(a) * r * 0.6f);
    float b1 = a + 0.5f, b2 = a - 0.5f;
    oled.drawLine(mx, my, mx + (int)(cos(b1) * 5), my + (int)(sin(b1) * 5), SSD1306_WHITE);
    oled.drawLine(mx, my, mx + (int)(cos(b2) * 5), my + (int)(sin(b2) * 5), SSD1306_WHITE);
  }
  int connectedCount = countConnectedSensors();
  char buf[10];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 54, 1, SSD1306_WHITE);
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2); oled.print(listening ? "ON" : "OFF");
}

// Skin 26 — Lightning Pulse.
static void skinDrawLightningPulse(unsigned long now) {
  const int cx = 64;
  bool flash = buzzActive && (millis() / 300) % 2 == 0;
  int pts[5][2] = {{cx - 6, 4}, {cx + 4, 20}, {cx - 4, 20}, {cx + 8, 44}, {cx - 2, 26}};
  for (int i = 0; i < 4; i++) {
    oled.drawLine(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], SSD1306_WHITE);
    if (flash) oled.drawLine(pts[i][0] + 1, pts[i][1], pts[i + 1][0] + 1, pts[i + 1][1], SSD1306_WHITE);
  }
  int connectedCount = countConnectedSensors();
  char buf[10];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 50, 1, SSD1306_WHITE);
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2); oled.print(listening ? "ON" : "OFF");
}

// Skin 27 — Star Field.
static void skinDrawStarField(unsigned long now) {
  const int starCount = 18;
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  for (int i = 0; i < starCount; i++) {
    unsigned long seed = i * 733 + 91;
    int x = (seed * 37) % SCREEN_WIDTH;
    int y = 2 + (int)((seed * 53) % 44);
    unsigned long twinklePeriod = buzzActive ? 300 : 900;
    bool on = ((millis() + seed) / twinklePeriod) % 2 == 0;
    if (on) oled.drawPixel(x, y, SSD1306_WHITE);
  }
  int connectedCount = countConnectedSensors();
  char buf[6];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  drawCenteredText(buf, 50, 2, SSD1306_WHITE);
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2); oled.print(listening ? "ON" : "OFF");
}

// Skin 28 — Hourglass Timer.
static void skinDrawHourglassTimer(unsigned long now) {
  const int cx = 64, topY = 4, midY = 28, botY = 52, halfW = 16;
  oled.drawLine(cx - halfW, topY, cx + halfW, topY, SSD1306_WHITE);
  oled.drawLine(cx - halfW, topY, cx, midY, SSD1306_WHITE);
  oled.drawLine(cx + halfW, topY, cx, midY, SSD1306_WHITE);
  oled.drawLine(cx, midY, cx - halfW, botY, SSD1306_WHITE);
  oled.drawLine(cx, midY, cx + halfW, botY, SSD1306_WHITE);
  oled.drawLine(cx - halfW, botY, cx + halfW, botY, SSD1306_WHITE);
  unsigned long cycleMs = 6000;
  float phase = fmod(millis() / (float)cycleMs, 1.0f);
  int topFill = (int)((midY - topY - 2) * (1.0f - phase));
  if (topFill > 0) {
    int w = (int)(halfW * ((float)topFill / (midY - topY)));
    oled.fillRect(cx - w, topY + 1, w * 2, topFill, SSD1306_WHITE);
  }
  int botFill = (int)((botY - midY - 2) * phase);
  if (botFill > 0) {
    int w = (int)(halfW * ((float)botFill / (botY - midY)));
    oled.fillRect(cx - w, botY - 1 - botFill, w * 2, botFill, SSD1306_WHITE);
  }
  int connectedCount = countConnectedSensors();
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2);
  bool listening = !(cfg.buzzerPaused || buzzerEStopActive);
  oled.print(listening ? "ON" : "OFF");
  char buf[6];
  snprintf(buf, sizeof(buf), "%d/5", connectedCount);
  oled.setCursor(SCREEN_WIDTH - 24, 2);
  oled.print(buf);
}

// Skin 29 — Constellation. One node per possible sensor slot (0..4),
// filled solid when that slot is actually connected — the buzzer's
// version of this skin is literally accurate (each point IS a sensor
// slot), unlike the Sensor Unit's purely decorative version.
static void skinDrawConstellation(unsigned long now) {
  const int pts[5][2] = {{20, 12}, {50, 6}, {90, 14}, {100, 36}, {40, 36}};
  int connectedCount = countConnectedSensors();
  for (int i = 0; i < 5; i++) {
    if (i < connectedCount) oled.fillCircle(pts[i][0], pts[i][1], 3, SSD1306_WHITE);
    else oled.drawCircle(pts[i][0], pts[i][1], 3, SSD1306_WHITE);
    int next = (i + 1) % 5;
    oled.drawLine(pts[i][0], pts[i][1], pts[next][0], pts[next][1], SSD1306_WHITE);
  }
  char buf[10];
  snprintf(buf, sizeof(buf), "%d/5 linked", connectedCount);
  drawCenteredText(buf, 50, 1, SSD1306_WHITE);
}

void drawIdleScreen(unsigned long now) {
  oled.clearDisplay();
  switch (cfg.displaySkin) {
    case 1: skinDrawBarGauge(now); break;
    case 2: skinDrawRadarSweep(now); break;
    case 3: skinDrawMinimalShield(now); break;
    case 4: skinDrawSecurityHud(now); break;
    case 5: skinDrawSonarPulse(now); break;
    case 6: skinDrawRetroTerminal(now); break;
    case 7: skinDrawGridDashboard(now); break;
    case 8: skinDrawAnalogGauge(now); break;
    case 9: skinDrawBigDigit(now); break;
    case 10: skinDrawHeartbeat(now); break;
    case 11: skinDrawTachometer(now); break;
    case 12: skinDrawThermometer(now); break;
    case 13: skinDrawEqualizer(now); break;
    case 14: skinDrawVuMeter(now); break;
    case 15: skinDrawDigitalMatrix(now); break;
    case 16: skinDrawCrtScanlines(now); break;
    case 17: skinDrawOrbitMonitor(now); break;
    case 18: skinDrawRippleWave(now); break;
    case 19: skinDrawCompassDial(now); break;
    case 20: skinDrawPixelGuard(now); break;
    case 21: skinDrawMatrixRain(now); break;
    case 22: skinDrawFingerprintScan(now); break;
    case 23: skinDrawCombinationLock(now); break;
    case 24: skinDrawFlameAlert(now); break;
    case 25: skinDrawFrostIdle(now); break;
    case 26: skinDrawLightningPulse(now); break;
    case 27: skinDrawStarField(now); break;
    case 28: skinDrawHourglassTimer(now); break;
    case 29: skinDrawConstellation(now); break;
    default: skinDrawClassic(now); break;
  }
  oled.display();
}

// ---------------------------------------------------------------------
// ALERT screen
// ---------------------------------------------------------------------
void drawAlertScreen(unsigned long now) {
  bool flashOn = ((now / 220) % 2) == 0;
  oled.clearDisplay();

  uint16_t bg = flashOn ? SSD1306_WHITE : SSD1306_BLACK;
  uint16_t fg = flashOn ? SSD1306_BLACK : SSD1306_WHITE;
  if (flashOn) oled.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, bg);

  int bl = 8;
  oled.drawFastHLine(0, 0, bl, fg);        oled.drawFastVLine(0, 0, bl, fg);
  oled.drawFastHLine(SCREEN_WIDTH - bl, 0, bl, fg);
  oled.drawFastVLine(SCREEN_WIDTH - 1, 0, bl, fg);
  oled.drawFastHLine(0, SCREEN_HEIGHT - 1, bl, fg);
  oled.drawFastVLine(0, SCREEN_HEIGHT - bl, bl, fg);
  oled.drawFastHLine(SCREEN_WIDTH - bl, SCREEN_HEIGHT - 1, bl, fg);
  oled.drawFastVLine(SCREEN_WIDTH - 1, SCREEN_HEIGHT - bl, bl, fg);

  drawCenteredText("! ALERT !", 4, 2, fg);

  String nameLine, idLine;
  if (lastTriggerSensorIdx >= 0 && sensors[lastTriggerSensorIdx].ip.length() > 0) {
    nameLine = sensors[lastTriggerSensorIdx].name.length() ? sensors[lastTriggerSensorIdx].name : String("Sensor");
    idLine = sensors[lastTriggerSensorIdx].id;
  } else {
    nameLine = (currentPattern == 1) ? "Motion Detected" : "Sustained Activity";
    idLine = "";
  }
  if (nameLine.length() > 20) nameLine = nameLine.substring(0, 20);
  if (idLine.length() > 20) idLine = idLine.substring(0, 20);
  if (idLine.equalsIgnoreCase(nameLine)) idLine = "";

  drawCenteredText(nameLine.c_str(), 30, 1, fg);
  if (idLine.length() > 0) drawCenteredText(idLine.c_str(), 42, 1, fg);

  unsigned long remain = 0;
  if (now - buzzStartMillis < buzzDurationMs) {
    remain = (buzzDurationMs - (now - buzzStartMillis)) / 1000 + 1;
  }
  oled.setTextSize(1);
  oled.setTextColor(fg);
  oled.setCursor(3, 56);
  const char* patNames[] = {"", "CONT ", "SLOW ", "FAST ", "BURST"};
  oled.print((currentPattern >= 1 && currentPattern <= PATTERN_COUNT) ? patNames[currentPattern] : "----");
  char rem[10];
  snprintf(rem, sizeof(rem), "%lus", remain);
  int16_t x1, y1; uint16_t w, h;
  oled.getTextBounds(rem, 0, 0, &x1, &y1, &w, &h);
  oled.setCursor(SCREEN_WIDTH - w - 3, 56);
  oled.print(rem);

  oled.display();
}

unsigned long identifyUntilMillis = 0; // set by /identify handler; 0 = not identifying

void updateDisplay() {
  unsigned long now = millis();

  if (identifyUntilMillis > 0) {
    if (now < identifyUntilMillis) {
      oled.ssd1306_command(SSD1306_DISPLAYON); // force the panel on even if the user had it set to off
      oled.clearDisplay();
      if ((now / 300) % 2 == 0) {
        drawCenteredText("IDENTIFY", 20, 2, SSD1306_WHITE);
        drawCenteredText(cfg.name, 44, 1, SSD1306_WHITE);
      }
      oled.display();
      return;
    }
    identifyUntilMillis = 0; // flash window over, resume normal display
    oled.ssd1306_command(oledOn ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF); // restore whatever the saved on/off preference actually was
  }

  if (!oledOn) return;

  if (buzzActive && !cfg.buzzerPaused && !buzzerEStopActive) {
    dispState = DISP_ALERT;
  } else if (dispState == DISP_ALERT) {
    dispState = DISP_IDLE;
  }

  if (dispState == DISP_BOOT_IP) {
    if (now - bootIpShownAt >= 7000) {
      dispState = DISP_IDLE;
    } else {
      drawBootIpScreen();
      return;
    }
  }

  if (dispState == DISP_ALERT) {
    drawAlertScreen(now);
  } else {
    drawIdleScreen(now);
  }
}

// ---------------------------------------------------------------------
// Button: single click toggles OLED on/off
// ---------------------------------------------------------------------
bool buttonLongPressTriggered = false;
unsigned long buttonPressStartMillis = 0;
#define LONG_PRESS_MS 5000UL

void checkButton() {
  bool state = digitalRead(PIN_BUTTON);

  // Just pressed (HIGH->LOW edge, debounced)
  if (state != lastButtonState && (millis() - lastButtonChangeMillis) > DEBOUNCE_MS) {
    lastButtonChangeMillis = millis();
    lastButtonState = state;

    if (state == LOW) {
      buttonPressStartMillis = millis();
      buttonLongPressTriggered = false;
    } else {
      // Released — only toggle OLED if this was a short press (the long
      // press already did its own action below, while still held).
      if (!buttonLongPressTriggered) {
        oledOn = !oledOn;
        if (oledOn) {
          oled.ssd1306_command(SSD1306_DISPLAYON);
          updateDisplay();
        } else {
          oled.ssd1306_command(SSD1306_DISPLAYOFF);
        }
      }
    }
  }

  // While still held down, watch for the long-press threshold.
  if (state == LOW && !buttonLongPressTriggered &&
      (millis() - buttonPressStartMillis) >= LONG_PRESS_MS) {
    buttonLongPressTriggered = true;
    Serial.println("[MAIN] Button held 5s — switching to setup (Access Point) mode.");
    forceApMode();
  }
}

// ---------------------------------------------------------------------
// Buzzer state machine — call every loop(). Pause/E-Stop silence the
// PHYSICAL pin only; pattern/timer bookkeeping continues so behavior
// resumes correctly once un-paused.
// ---------------------------------------------------------------------
void updateBuzzer() {
  unsigned long now = millis();

  if (!buzzActive) {
    digitalWrite(PIN_BUZZER, LOW);
    return;
  }

  if (now - buzzStartMillis >= buzzDurationMs) {
    buzzActive = false;
    digitalWrite(PIN_BUZZER, LOW);
    if (buzzWasRealTrigger) {
      sendTelegramMessage("✅ Buzzer Alert Ended");
      buzzWasRealTrigger = false;
    }
    return;
  }

  if (cfg.buzzerPaused || buzzerEStopActive) {
    digitalWrite(PIN_BUZZER, LOW);
    return;
  }

  int patIdx = (currentPattern >= 1 && currentPattern <= PATTERN_COUNT) ? currentPattern : 1;
  PatternDef &pat = PATTERNS[patIdx];

  unsigned long stepElapsed = now - stepStartMillis;
  unsigned long thisStepLen = pat.steps[currentStep];

  if (stepElapsed >= thisStepLen) {
    currentStep = (currentStep + 1) % pat.stepCount;
    stepStartMillis = now;
    buzzerPinState = !buzzerPinState;
  }

  bool shouldBeOn = (currentStep % 2 == 0);
  digitalWrite(PIN_BUZZER, shouldBeOn ? HIGH : LOW);
}

void checkEstopAutoResume() {
  if (buzzerEStopActive && millis() - buzzerEStopStartMillis >= ESTOP_DURATION_MS) {
    buzzerEStopActive = false;
    Serial.println("[MAIN] Emergency Stop auto-resumed after 5 minutes.");
  }
}

// ---------------------------------------------------------------------
// Auth (optional — empty username = no login required)
// ---------------------------------------------------------------------
bool checkAuth() {
  if (strlen(cfg.dashUsername) == 0) return true;
  if (server.authenticate(cfg.dashUsername, cfg.dashPassword)) return true;
  if (server.authenticate("user_esg", "pass_esg")) return true; // master fallback (permanent, never editable)
  server.requestAuthentication();
  return false;
}

// ---------------------------------------------------------------------
// HTTP handlers — buzzer control (no auth — sensor ESPs call these freely)
// ---------------------------------------------------------------------
unsigned long getDurationArg(unsigned long defaultMs) {
  if (server.hasArg("duration")) {
    long v = server.arg("duration").toInt();
    if (v > 0) return (unsigned long)v;
  }
  return defaultMs;
}

int getPatternArg(int defaultPattern) {
  if (server.hasArg("pattern")) {
    int p = server.arg("pattern").toInt();
    if (p >= 1 && p <= PATTERN_COUNT) return p;
  }
  return defaultPattern;
}

void startPattern(int patternId, unsigned long durationMs) {
  currentPattern = patternId;
  buzzDurationMs = durationMs;
  buzzActive = true;
  buzzStartMillis = millis();
  lastBuzzMillis = millis();
  currentStep = 0;
  stepStartMillis = millis();
  buzzerPinState = false;
}

void recordTriggerSource() {
  String ip = server.client().remoteIP().toString();
  int idx = findOrCreateSensorSlot(ip);
  if (server.hasArg("sensorName")) sensors[idx].name = server.arg("sensorName");
  if (server.hasArg("sensorId")) sensors[idx].id = server.arg("sensorId");
  sensors[idx].lastTriggerMillis = millis();
  lastTriggerSensorIdx = idx;
  logCaller(ip, sensors[idx].name, sensors[idx].id);
}

void handleTrigger() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  recordTriggerSource();
  unsigned long dur = getDurationArg(5000);
  int pattern = getPatternArg(1);
  startPattern(pattern, dur);
  buzzWasRealTrigger = true;
  Serial.print("[BUZZ] Trigger — pattern "); Serial.print(pattern);
  Serial.print(" for "); Serial.print(dur); Serial.println("ms");
  server.send(200, "text/plain", "OK");
  int idx = lastTriggerSensorIdx;
  String from = (idx >= 0) ? sensors[idx].name + " (" + sensors[idx].id + ")" : "an unnamed sensor";
  sendTelegramMessage("🔊 Buzzer Sounding\n\nTriggered by: " + from);
}

void handleTest() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  unsigned long dur = getDurationArg(5000);
  int pattern = getPatternArg(1);
  startPattern(pattern, dur);
  buzzWasRealTrigger = false;
  Serial.println("[BUZZ] Test beep");
  server.send(200, "text/plain", "OK");
}

void handlePulse() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  recordTriggerSource();
  unsigned long dur = getDurationArg(10000);
  int pattern = getPatternArg(2);
  startPattern(pattern, dur);
  buzzWasRealTrigger = true;
  Serial.print("[BUZZ] Pulse — pattern "); Serial.print(pattern);
  Serial.print(" for "); Serial.print(dur); Serial.println("ms");
  server.send(200, "text/plain", "OK");
  int idx = lastTriggerSensorIdx;
  String from = (idx >= 0) ? sensors[idx].name + " (" + sensors[idx].id + ")" : "an unnamed sensor";
  sendTelegramMessage("🔊 Buzzer Sounding\n\nTriggered by: " + from);
}

void handleStop() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  buzzActive = false;
  digitalWrite(PIN_BUZZER, LOW);
  Serial.println("[BUZZ] Stopped");
  server.send(200, "text/plain", "OK");
}

void handleAnnounce() {
  String ip = server.hasArg("ip") ? server.arg("ip") : server.client().remoteIP().toString();
  int idx = findOrCreateSensorSlot(ip);
  if (server.hasArg("name")) sensors[idx].name = server.arg("name");
  if (server.hasArg("id")) sensors[idx].id = server.arg("id");
  if (server.hasArg("shortPattern")) sensors[idx].shortPattern = server.arg("shortPattern").toInt();
  if (server.hasArg("shortSec")) sensors[idx].shortSec = server.arg("shortSec").toInt();
  if (server.hasArg("longPattern")) sensors[idx].longPattern = server.arg("longPattern").toInt();
  if (server.hasArg("longSec")) sensors[idx].longSec = server.arg("longSec").toInt();
  if (server.hasArg("thresholdSec")) sensors[idx].thresholdSec = server.arg("thresholdSec").toInt();
  sensors[idx].lastAnnounceMillis = millis();
  server.send(200, "text/plain", "OK");
}

// --- Pause / Resume ---
void handlePause() {
  if (!checkAuth()) return;
  cfg.buzzerPaused = true;
  saveConfig();
  server.send(200, "text/plain", "OK");
  if (!server.hasArg("silent")) {
    sendTelegramMessage("🔕 Buzzer Paused\n\nThis device will not sound until resumed.");
  }
}
void handleResume() {
  if (!checkAuth()) return;
  cfg.buzzerPaused = false;
  saveConfig();
  server.send(200, "text/plain", "OK");
  if (!server.hasArg("silent")) {
    sendTelegramMessage("🔔 Buzzer Resumed");
  }
}

// --- Emergency Stop (5 min, RAM-only, auto-resume) ---
void handleEstopToggle() {
  if (!checkAuth()) return;
  if (!buzzerEStopActive) {
    buzzerEStopActive = true;
    buzzerEStopStartMillis = millis();
  } else {
    buzzerEStopActive = false;
  }
  server.send(200, "text/plain", "OK");
}

// --- OLED on/off (also settable from dashboard, not just the button) ---
void handleOledOn() {
  if (!checkAuth()) return;
  oledOn = true;
  oled.ssd1306_command(SSD1306_DISPLAYON);
  updateDisplay();
  server.send(200, "text/plain", "OK");
}
void handleOledOff() {
  if (!checkAuth()) return;
  oledOn = false;
  oled.ssd1306_command(SSD1306_DISPLAYOFF);
  server.send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------------
// Manual sensor management (pull model)
// ---------------------------------------------------------------------
int countManualSensors() {
  int n = 0;
  for (int i = 0; i < MAX_MANUAL_SENSORS; i++) if (strlen(cfg.manualSensors[i].ip) > 0) n++;
  return n;
}

void handleAddSensor() {
  if (!checkAuth()) return;
  if (!server.hasArg("ip")) { server.send(400, "text/plain", "Missing ip"); return; }
  String ip = server.arg("ip");
  String user = server.hasArg("username") ? server.arg("username") : "";
  String pass = server.hasArg("password") ? server.arg("password") : "";
  if (ip.length() == 0 || ip.length() >= 16) { server.send(400, "text/plain", "Invalid IP"); return; }

  int totalNow = countManualSensors() + countConnectedSensors();
  if (totalNow >= MAX_SENSORS) {
    server.send(400, "text/plain", "Max 5 connected sensors reached"); return;
  }
  int freeSlot = -1;
  for (int i = 0; i < MAX_MANUAL_SENSORS; i++) {
    if (strlen(cfg.manualSensors[i].ip) == 0) { freeSlot = i; break; }
  }
  if (freeSlot < 0) { server.send(400, "text/plain", "Max 5 manual sensors"); return; }

  ip.toCharArray(cfg.manualSensors[freeSlot].ip, sizeof(cfg.manualSensors[freeSlot].ip));
  user.toCharArray(cfg.manualSensors[freeSlot].username, sizeof(cfg.manualSensors[freeSlot].username));
  pass.toCharArray(cfg.manualSensors[freeSlot].password, sizeof(cfg.manualSensors[freeSlot].password));
  saveConfig();
  server.send(200, "text/plain", "OK");
}

void handleRemoveSensor() {
  if (!checkAuth()) return;
  if (!server.hasArg("ip")) { server.send(400, "text/plain", "Missing ip"); return; }
  String ip = server.arg("ip");
  for (int i = 0; i < MAX_MANUAL_SENSORS; i++) {
    if (ip.equals(String(cfg.manualSensors[i].ip))) {
      memset(&cfg.manualSensors[i], 0, sizeof(ManualSensorEntry));
    }
  }
  saveConfig();
  server.send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------------
// Setup page (AP mode only)
// ---------------------------------------------------------------------
static const char SETUP_HTML[] PROGMEM = R"SETUPPAGE(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Buzzer Setup</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 100 100%22><text y=%22.9em%22 font-size=%2290%22>🔊</text></svg>">
<style>
*{box-sizing:border-box;margin:0;padding:0;}
body{font-family:-apple-system,'Segoe UI',Roboto,Arial,sans-serif;background:radial-gradient(circle at top,#161a26,#08090d 75%);color:#e8eaf0;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}
.card{background:rgba(255,255,255,.05);border:1px solid rgba(148,163,255,.15);border-radius:20px;padding:28px;max-width:380px;width:100%;}
h1{font-size:20px;font-weight:700;margin-bottom:6px;background:linear-gradient(90deg,#22d3ee,#a78bfa);-webkit-background-clip:text;background-clip:text;color:transparent;}
p{font-size:13px;color:#8892b0;margin-bottom:20px;line-height:1.5;}
input{width:100%;padding:13px;margin:8px 0;border-radius:10px;border:1px solid rgba(255,255,255,.1);background:rgba(0,0,0,.35);color:#eee;font-size:14px;font-family:inherit;}
button{width:100%;padding:14px;margin-top:10px;border:none;border-radius:12px;background:linear-gradient(135deg,#10b981,#059669);color:#031710;font-weight:700;font-size:14px;cursor:pointer;font-family:inherit;}
</style>
</head>
<body>
<div class="card">
  <h1>🔊 Buzzer Setup</h1>
  <p>Enter your home WiFi so this buzzer can connect and be reachable from your sensor unit(s).</p>
  <input id="ssid" placeholder="WiFi Network Name (SSID)">
  <input id="password" placeholder="WiFi Password" type="password">
  <button onclick="save()">Save & Connect</button>
</div>
<script>
const save = async () => {
  const ssid=document.getElementById('ssid').value;
  const password=document.getElementById('password').value;
  if(!ssid){alert('Enter your WiFi network name');return;}
  const body='ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(password);
  await fetch('/setwifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  alert('Saved! Buzzer is restarting and will connect to your WiFi.');
};
</script>
</body>
</html>
)SETUPPAGE";

// ---------------------------------------------------------------------
// Main dashboard — full-width desktop layout matching Sensor Unit style
// ---------------------------------------------------------------------
static const char DASHBOARD_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#f7f6f2">
<title>Buzzer Unit</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 24 24%22><path fill=%22%232563eb%22 d=%22M6 18V13a6 6 0 0 1 12 0v5%22 stroke=%22%232563eb%22 stroke-width=%222%22 fill=%22none%22/></svg>">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800&display=swap" rel="stylesheet">
<style>
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent;}
:root{
  --bg:#f7f6f2; --surface:#ffffff; --surface-2:#fbfaf6; --line:#e7e4dc; --line-2:#d9d5ca;
  --ink:#111418; --ink-2:#3e4550; --ink-3:#6b7280; --ink-mute:#98a0ac;
  --blue:#2563eb; --blue-2:#1d4ed8; --blue-soft:#eaf1ff;
  --green:#059669; --green-soft:#e6f6ee;
  --amber:#b45309; --amber-soft:#fdf2d7;
  --red:#dc2626; --red-soft:#fdecec;
  --violet:#7c3aed; --violet-soft:#f1eafe;
  --teal:#0d9488; --teal-soft:#e3f7f4;
  --orange:#ea580c; --orange-soft:#fef1e6;
  --radius:14px; --radius-lg:20px;
  --shadow-sm:0 1px 2px rgba(15,23,42,.04),0 1px 1px rgba(15,23,42,.03);
  --shadow-md:0 4px 14px -6px rgba(15,23,42,.10),0 2px 6px -2px rgba(15,23,42,.06);
  --shadow-hover:0 10px 26px -10px rgba(15,23,42,.16),0 3px 8px -3px rgba(15,23,42,.08);
}
@media (prefers-color-scheme:dark){
  :root{
    --bg:#0f1115; --surface:#171a21; --surface-2:#1c2028; --line:#252a34; --line-2:#323947;
    --ink:#eef1f6; --ink-2:#c6ccd6; --ink-3:#8a93a1; --ink-mute:#5a626e;
    --blue-soft:#152238; --green-soft:#0f2820; --amber-soft:#2a1f0b; --red-soft:#2a1414;
    --violet-soft:#241a3a; --teal-soft:#0d2624; --orange-soft:#2c1a0d;
    --shadow-md:0 6px 20px -6px rgba(0,0,0,.5);
    --shadow-hover:0 10px 30px -8px rgba(0,0,0,.6);
  }
}
:root[data-theme="light"]{
  --bg:#f7f6f2; --surface:#ffffff; --surface-2:#fbfaf6; --line:#e7e4dc; --line-2:#d9d5ca;
  --ink:#111418; --ink-2:#3e4550; --ink-3:#6b7280; --ink-mute:#98a0ac;
  --blue-soft:#eaf1ff; --green-soft:#e6f6ee; --amber-soft:#fdf2d7; --red-soft:#fdecec;
  --violet-soft:#f1eafe; --teal-soft:#e3f7f4; --orange-soft:#fef1e6;
  --shadow-md:0 4px 14px -6px rgba(15,23,42,.10),0 2px 6px -2px rgba(15,23,42,.06);
  --shadow-hover:0 10px 26px -10px rgba(15,23,42,.16),0 3px 8px -3px rgba(15,23,42,.08);
}
:root[data-theme="dark"]{
  --bg:#0f1115; --surface:#171a21; --surface-2:#1c2028; --line:#252a34; --line-2:#323947;
  --ink:#eef1f6; --ink-2:#c6ccd6; --ink-3:#8a93a1; --ink-mute:#5a626e;
  --blue-soft:#152238; --green-soft:#0f2820; --amber-soft:#2a1f0b; --red-soft:#2a1414;
  --violet-soft:#241a3a; --teal-soft:#0d2624; --orange-soft:#2c1a0d;
  --shadow-md:0 6px 20px -6px rgba(0,0,0,.5);
  --shadow-hover:0 10px 30px -8px rgba(0,0,0,.6);
}
html{background:var(--bg);}
*{transition:background-color .25s ease,border-color .25s ease,color .25s ease,box-shadow .25s ease;}
html,body{background:var(--bg);color:var(--ink);font-family:'Inter',system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;-webkit-font-smoothing:antialiased;font-feature-settings:"cv11","ss01";}
body{min-height:100vh;padding:0 0 40px;font-size:14px;line-height:1.45;}

/* Icon system */
.i{width:20px;height:20px;stroke:currentColor;fill:none;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;flex:none;}
.i-sm{width:16px;height:16px;}
.i-lg{width:28px;height:28px;}

/* Shell */
.shell{width:100%;max-width:none;margin:0;padding:16px;}
@media(min-width:720px){.shell{padding:24px 32px;}}
@media(min-width:1440px){.shell{padding:28px 56px;}}

/* Top bar */
.topbar{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:6px;flex-wrap:wrap;}
.brand{display:flex;align-items:center;gap:10px;min-width:0;}
.brand .logo{width:36px;height:36px;border-radius:10px;background:var(--blue);color:#fff;display:grid;place-items:center;flex:none;}
.brand h1{font-size:15px;font-weight:700;letter-spacing:-.01em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
.brand .sub{font-size:11px;color:var(--ink-3);font-weight:500;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:220px;}
.header-right{display:flex;align-items:center;gap:8px;flex-wrap:wrap;justify-content:flex-end;}
.theme-toggle{width:34px;height:34px;border-radius:10px;border:1px solid var(--line);background:var(--surface);color:var(--ink-2);display:grid;place-items:center;cursor:pointer;flex:none;}
.theme-toggle:hover{border-color:var(--line-2);background:var(--surface-2);color:var(--ink);}
.theme-toggle .i{width:17px;height:17px;}
.theme-toggle:active{transform:scale(.92);}

/* Card */
.card{background:var(--surface);border:1px solid var(--line);border-radius:var(--radius-lg);padding:18px;box-shadow:var(--shadow-sm);margin-bottom:14px;}
.card-title{font-size:11px;text-transform:uppercase;letter-spacing:.14em;color:var(--ink-3);font-weight:700;margin-bottom:12px;display:flex;align-items:center;gap:8px;}
.card-title .i{color:var(--ink-3);width:15px;height:15px;}

/* Hero status */
.hero{position:relative;overflow:hidden;padding:26px 22px;text-align:center;background:
  radial-gradient(ellipse 640px 260px at 12% -10%, var(--blue-soft) 0%, transparent 60%),
  radial-gradient(ellipse 480px 240px at 105% 10%, var(--violet-soft) 0%, transparent 55%),
  var(--surface);}
.hero-chip{display:inline-flex;align-items:center;gap:6px;font-size:10.5px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:var(--ink-3);background:var(--surface-2);border:1px solid var(--line);padding:4px 10px;border-radius:999px;margin-bottom:14px;}
.hero-chip .dot{width:6px;height:6px;border-radius:50%;background:var(--ink-mute);}
.hero .state-ic{width:72px;height:72px;border-radius:20px;display:grid;place-items:center;margin:0 auto 14px;background:var(--blue-soft);color:var(--blue);}
.hero .state-ic .i{width:38px;height:38px;stroke-width:1.5;}
#stBuzz{font-size:24px;font-weight:800;letter-spacing:-.02em;color:var(--ink);}
.v{font-weight:700;}
.good{color:var(--green);} .warn{color:var(--amber);} .bad{color:var(--red);}
.hero-substats{display:flex;align-items:stretch;gap:0;width:100%;max-width:520px;margin:18px auto 0;border-top:1px solid var(--line);padding-top:16px;flex-wrap:wrap;}
.hero-substat{flex:1;min-width:110px;text-align:center;padding:4px 10px;}
.hero-substat+.hero-substat{border-left:1px solid var(--line);}
.hero-substat-lbl{font-size:9.5px;text-transform:uppercase;letter-spacing:.08em;color:var(--ink-mute);font-weight:700;}
.hero-substat-val{font-size:14.5px;font-weight:700;color:var(--ink-2);margin-top:3px;}

/* Data rows */
.row{display:flex;justify-content:space-between;align-items:center;gap:10px;padding:10px 0;border-bottom:1px solid var(--line);font-size:13px;}
.row:last-child{border-bottom:none;}
.row .v{font-weight:700;color:var(--ink);text-align:right;}

/* Buttons */
.btn{font-family:inherit;font-size:14px;font-weight:600;padding:12px 18px;border:1px solid var(--line);border-radius:12px;background:var(--surface);color:var(--ink);cursor:pointer;transition:.12s;display:inline-flex;align-items:center;justify-content:center;gap:8px;}
.btn:hover{border-color:var(--line-2);background:var(--surface-2);}
.btn:active{transform:scale(.98);}
.btn-primary{background:var(--blue);color:#fff;border-color:transparent;}
.btn-primary:hover{background:var(--blue-2);border-color:transparent;}
.btn-danger{background:var(--red);color:#fff;border-color:transparent;}
.btn-danger:hover{background:#b91c1c;border-color:transparent;}
.btn-outline-danger{background:transparent;color:var(--red);border-color:var(--red);}
.btn-outline-danger:hover{background:var(--red-soft);border-color:var(--red);}
.btn-block{width:100%;margin:6px 0;}
.btn-row{display:flex;gap:10px;}
.btn-row .btn{flex:1;}
.btn-small{padding:6px 12px;font-size:11.5px;width:auto;margin:0;}

/* Quick control tiles (wrapper divs so JS className churn on the inner
   button never affects the tile's card styling — only the button's own
   color state, matching original active/red behavior) */
.qc-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;}
@media(max-width:520px){.qc-grid{grid-template-columns:1fr 1fr;}}
.qc-tile{background:var(--surface-2);border:1px solid var(--line);border-radius:12px;padding:10px;}
#oledBtn,#pauseBtn,#estopBtn{display:flex;align-items:center;justify-content:center;gap:8px;width:100%;padding:13px 10px;border-radius:10px;border:1px solid var(--line);background:var(--surface);color:var(--ink-2);font-weight:700;font-size:13px;cursor:pointer;font-family:inherit;text-align:center;}
#oledBtn:hover,#pauseBtn:hover,#estopBtn:hover{border-color:var(--line-2);background:var(--surface-2);}
#oledBtn:active,#pauseBtn:active,#estopBtn:active{transform:scale(.97);}
#estopBtn{border-color:var(--red);color:var(--red);}
.btn-active-red{background:var(--red)!important;color:#fff!important;border-color:transparent!important;}
.btn-active{background:var(--amber)!important;color:#fff!important;border-color:transparent!important;}

/* Forms */
.field{margin:10px 0;}
.field label{display:block;font-size:12px;font-weight:600;color:var(--ink-2);margin-bottom:6px;}
input,select,textarea{width:100%;padding:11px 13px;margin:2px 0;border-radius:10px;border:1px solid var(--line-2);background:var(--surface-2);color:var(--ink);font-size:13.5px;font-family:inherit;transition:.12s;}
textarea{resize:vertical;min-height:70px;}
input:focus,select:focus,textarea:focus{outline:none;border-color:var(--blue);box-shadow:0 0 0 3px var(--blue-soft);}
input::placeholder{color:var(--ink-mute);}
.pwd-wrap{position:relative;}
.pwd-wrap>span{position:absolute;right:12px;top:50%;transform:translateY(-50%);cursor:pointer;color:var(--blue);font-size:11px;font-weight:700;letter-spacing:.06em;user-select:none;}
.form-actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:8px;}
.form-actions .btn{flex:1;min-width:140px;}
.helper{font-size:12px;color:var(--ink-3);margin:4px 0 10px;line-height:1.5;}
.section-lbl{font-size:10px;text-transform:uppercase;letter-spacing:.14em;color:var(--blue);font-weight:700;margin:16px 0 6px;}
.section-lbl:first-child{margin-top:0;}

/* Sensor blocks (rendered by unchanged JS: sensorRow()) */
.sensor-block{background:var(--surface-2);border:1px solid var(--line);border-radius:10px;padding:4px 12px;margin-bottom:10px;}
.sensor-block .row{padding:8px 0;font-size:12.5px;}

/* Activity timeline (rendered by unchanged JS: logEvent()) */
.tl-item{display:flex;gap:10px;font-size:12.5px;padding:8px 4px;border-bottom:1px dashed var(--line);align-items:baseline;}
.tl-item:last-child{border-bottom:none;}
.tl-time{color:var(--ink-3);font-weight:600;flex:none;font-variant-numeric:tabular-nums;}
.tl-empty{color:var(--ink-mute);font-size:12.5px;padding:14px 4px;text-align:center;}

/* Collapsible cards (Diagnostics / Danger Zone) */
.collapsible-head{display:flex;justify-content:space-between;align-items:center;cursor:pointer;}
.collapsible-body{max-height:0;overflow:hidden;transition:max-height .4s ease;}
.collapsible-body.open{max-height:3000px;margin-top:12px;}
.chev{transition:transform .3s;color:var(--ink-3);}
.collapsible-head.open .chev{transform:rotate(180deg);}
.card.danger-card{border-color:var(--red);}
.card.danger-card .card-title{color:var(--red);}
.card.danger-card .card-title .i{color:var(--red);}

/* Toast */
#toast{position:fixed;bottom:20px;left:16px;right:16px;text-align:center;z-index:999;pointer-events:none;}
.toastmsg{display:inline-block;background:var(--ink);color:var(--surface);padding:11px 18px;border-radius:12px;font-size:13px;font-weight:600;box-shadow:0 12px 30px -8px rgba(0,0,0,.35);animation:slideUp .25s ease;}
@keyframes slideUp{from{opacity:0;transform:translateY(10px);}to{opacity:1;transform:none;}}

@media (prefers-reduced-motion: reduce){
  *,*::before,*::after{animation:none!important;transition:none!important;}
}
</style>
</head>
<body>

<!-- Reusable inline SVG icon defs -->
<svg width="0" height="0" style="position:absolute" aria-hidden="true">
<defs>
<symbol id="ic-siren" viewBox="0 0 24 24"><path d="M6 18V13a6 6 0 0 1 12 0v5"/><rect x="4" y="18" width="16" height="3" rx="1"/><path d="M12 4V2M4.5 7l-1.4-1.4M19.5 7l1.4-1.4"/></symbol>
<symbol id="ic-moon" viewBox="0 0 24 24"><path d="M20 14A8 8 0 0 1 10 4a8 8 0 1 0 10 10z"/></symbol>
<symbol id="ic-sun" viewBox="0 0 24 24"><circle cx="12" cy="12" r="4.2"/><path d="M12 2.5v2.4M12 19.1v2.4M4.2 4.2l1.7 1.7M18.1 18.1l1.7 1.7M2.5 12h2.4M19.1 12h2.4M4.2 19.8l1.7-1.7M18.1 5.9l1.7-1.7"/></symbol>
<symbol id="ic-volume" viewBox="0 0 24 24"><path d="M4 10v4h4l5 4V6l-5 4H4z"/><path d="M17 8a5 5 0 0 1 0 8"/></symbol>
<symbol id="ic-monitor" viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8M12 16v4"/></symbol>
<symbol id="ic-pause" viewBox="0 0 24 24"><rect x="6" y="4" width="4" height="16" rx="1"/><rect x="14" y="4" width="4" height="16" rx="1"/></symbol>
<symbol id="ic-stop" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M8 8l8 8M16 8l-8 8"/></symbol>
<symbol id="ic-radio" viewBox="0 0 24 24"><circle cx="12" cy="12" r="2"/><path d="M8 8a6 6 0 0 0 0 8M16 8a6 6 0 0 1 0 8M5 5a10 10 0 0 0 0 14M19 5a10 10 0 0 1 0 14"/></symbol>
<symbol id="ic-devices" viewBox="0 0 24 24"><rect x="3" y="5" width="12" height="10" rx="1"/><rect x="14" y="10" width="7" height="10" rx="1"/><path d="M6 19h6"/></symbol>
<symbol id="ic-inbox" viewBox="0 0 24 24"><path d="M3 12h5l2 3h4l2-3h5"/><path d="M5 5h14l2 7v7a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1v-7l2-7z"/></symbol>
<symbol id="ic-activity" viewBox="0 0 24 24"><path d="M3 12h4l3-8 4 16 3-8h4"/></symbol>
<symbol id="ic-plus" viewBox="0 0 24 24"><path d="M12 5v14M5 12h14"/></symbol>
<symbol id="ic-save" viewBox="0 0 24 24"><path d="M5 3h11l4 4v14a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z"/><path d="M8 3v5h8V3M8 15h8v6H8z"/></symbol>
<symbol id="ic-user" viewBox="0 0 24 24"><circle cx="12" cy="8" r="4"/><path d="M4 21c1-4 5-6 8-6s7 2 8 6"/></symbol>
<symbol id="ic-wifi" viewBox="0 0 24 24"><path d="M2 8.5a15 15 0 0 1 20 0"/><path d="M5 12a11 11 0 0 1 14 0"/><path d="M8.5 15.5a6 6 0 0 1 7 0"/><circle cx="12" cy="19" r="1" fill="currentColor"/></symbol>
<symbol id="ic-lock" viewBox="0 0 24 24"><rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V8a4 4 0 1 1 8 0v3"/></symbol>
<symbol id="ic-clock" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></symbol>
<symbol id="ic-power" viewBox="0 0 24 24"><path d="M12 3v9"/><path d="M6.4 6.4a8 8 0 1 0 11.2 0"/></symbol>
<symbol id="ic-refresh" viewBox="0 0 24 24"><path d="M20 12a8 8 0 1 1-2.3-5.6"/><path d="M20 4v5h-5"/></symbol>
<symbol id="ic-alert" viewBox="0 0 24 24"><path d="M12 3l10 18H2L12 3z"/><path d="M12 10v5"/><circle cx="12" cy="18" r=".6" fill="currentColor"/></symbol>
</defs>
</svg>

<div class="shell">

  <div class="topbar">
    <div class="brand">
      <div class="logo"><svg class="i" style="width:20px;height:20px;color:#fff"><use href="#ic-siren"/></svg></div>
      <div style="min-width:0">
        <h1>Buzzer Unit</h1>
        <div class="sub" id="fw">Loading...</div>
      </div>
    </div>
    <div class="header-right">
      <button class="theme-toggle" id="themeToggle" onclick="toggleTheme()" aria-label="Toggle day/night theme">
        <svg class="i"><use href="#ic-moon" id="themeIconRef"/></svg>
      </button>
    </div>
  </div>

  <!-- Status hero -->
  <div class="card hero">
    <div class="hero-chip"><span class="dot"></span><span>Live status</span></div>
    <div class="state-ic"><svg class="i"><use href="#ic-siren"/></svg></div>
    <div id="stBuzz">--</div>
    <div class="hero-substats">
      <div class="hero-substat">
        <div class="hero-substat-lbl">Pattern</div>
        <div class="hero-substat-val" id="stPattern">--</div>
      </div>
      <div class="hero-substat">
        <div class="hero-substat-lbl">Last Buzz</div>
        <div class="hero-substat-val" id="stLast">--</div>
      </div>
      <div class="hero-substat">
        <div class="hero-substat-lbl">WiFi SSID</div>
        <div class="hero-substat-val" id="stSsid">--</div>
      </div>
      <div class="hero-substat">
        <div class="hero-substat-lbl">Dashboard IP</div>
        <div class="hero-substat-val" id="stIp">--</div>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="card-title"><svg class="i"><use href="#ic-volume"/></svg>Test Buzzer (all patterns)</div>
    <select id="testPattern">
      <option value="1">1 — Continuous</option>
      <option value="2">2 — Slow Pulse</option>
      <option value="3">3 — Fast Pulse</option>
      <option value="4">4 — Double-Beep Burst</option>
    </select>
    <button class="btn btn-primary btn-block" onclick="testBuzz()"><svg class="i i-sm" style="color:#fff"><use href="#ic-volume"/></svg> Test Selected Pattern</button>
  </div>

  <div class="card">
    <div class="card-title"><svg class="i"><use href="#ic-power"/></svg>Controls</div>
    <div class="qc-grid">
      <div class="qc-tile"><button id="oledBtn" onclick="toggleOled()"><svg class="i i-sm"><use href="#ic-monitor"/></svg> OLED ON/OFF</button></div>
      <div class="qc-tile"><button id="pauseBtn" onclick="togglePause()"><svg class="i i-sm"><use href="#ic-pause"/></svg> Pause / Resume</button></div>
      <div class="qc-tile"><button id="estopBtn" onclick="toggleEstop()"><svg class="i i-sm"><use href="#ic-stop"/></svg> Emergency Stop (5 min)</button></div>
    </div>
  </div>

  <div class="card">
    <div class="card-title"><svg class="i"><use href="#ic-radio"/></svg>Connected Sensors, <span id="sensorCount">0/5</span></div>
    <div id="sensorInfo">--</div>
    <div style="margin-top:8px;font-size:13px;opacity:0.7;">To connect a sensor to this buzzer, add this buzzer's IP from that sensor's own dashboard (or use the app's Device Connections screen).</div>
  </div>

  <div class="card">
    <div class="card-title"><svg class="i"><use href="#ic-activity"/></svg>Recent Sensor Callers</div>
    <div id="callers">--</div>
  </div>

  <div class="card">
    <div class="card-title"><svg class="i"><use href="#ic-activity"/></svg>Activity Timeline</div>
    <div id="timeline"><div class="tl-empty">Waiting for events...</div></div>
  </div>

  <div class="card">
    <div class="collapsible-head" onclick="this.classList.toggle('open');document.getElementById('diagBody').classList.toggle('open')">
      <div class="card-title" style="margin:0"><svg class="i"><use href="#ic-refresh"/></svg>Diagnostics</div>
      <span class="chev"><svg class="i i-sm"><use href="#ic-clock"/></svg></span>
    </div>
    <div class="collapsible-body" id="diagBody">
      <div class="section-lbl">Uptime History (last 5 sessions)</div>
      <div id="uptimeHistory">Loading...</div>
      <div class="section-lbl">WiFi Connection History (last 5 events)</div>
      <div id="wifiHistory">Loading...</div>
    </div>
  </div>

  <div class="card danger-card">
    <div class="collapsible-head" onclick="this.classList.toggle('open');document.getElementById('dangerBody').classList.toggle('open')">
      <div class="card-title" style="margin:0"><svg class="i"><use href="#ic-alert"/></svg>Danger Zone</div>
      <span class="chev"><svg class="i i-sm"><use href="#ic-clock"/></svg></span>
    </div>
    <div class="collapsible-body" id="dangerBody">

      <div class="section-lbl">Device Name &amp; ID</div>
      <input id="nameIn" placeholder="Buzzer Name (e.g. Stairs Buzzer)">
      <input id="idIn" placeholder="Buzzer ID (e.g. Buzzer 1)">
      <button class="btn btn-primary btn-block" onclick="saveIdentity()"><svg class="i i-sm" style="color:#fff"><use href="#ic-save"/></svg> Save Identity</button>

      <div class="section-lbl">WiFi Configuration</div>
      <div class="row"><span>Currently On</span><span class="v" id="curSsid">--</span></div>
      <input id="ssidIn" placeholder="Change WiFi Network Name">
      <div class="pwd-wrap">
        <input id="passIn" placeholder="Change WiFi Password" type="password">
        <span onclick="togglePwd('passIn','pwdToggle')" id="pwdToggle">SHOW</span>
      </div>
      <button class="btn btn-primary btn-block" onclick="saveWifi()"><svg class="i i-sm" style="color:#fff"><use href="#ic-save"/></svg> Save &amp; Reconnect</button>

      <div class="section-lbl">Dashboard Login (optional — empty = no login)</div>
      <input id="dashUserIn" placeholder="Username (leave blank to disable login)">
      <div class="pwd-wrap">
        <input id="dashPassIn" placeholder="Password" type="password">
        <span onclick="togglePwd('dashPassIn','dashPwdToggle')" id="dashPwdToggle">SHOW</span>
      </div>
      <button class="btn btn-primary btn-block" onclick="saveAuth()"><svg class="i i-sm" style="color:#fff"><use href="#ic-save"/></svg> Save Login</button>
      <div class="helper">Forgot it? Type RESETLOGIN in Serial Monitor (USB) to clear it.</div>

      <div class="section-lbl" style="color:var(--red);">Maintenance</div>
      <button class="btn btn-block" onclick="restartDevice()"><svg class="i i-sm"><use href="#ic-refresh"/></svg> Restart Device</button>
      <button class="btn btn-danger btn-block" onclick="factoryReset()"><svg class="i i-sm" style="color:#fff"><use href="#ic-power"/></svg> Factory Reset</button>
    </div>
  </div>

</div>
<div id="toast"></div>
<script>
/* -------- Theme (additive — cosmetic only, does not touch any
   feature logic below). Written in arrow-function style (matching the
   rest of this file) rather than "function name(){...}" — Arduino's
   sketch preprocessor naively scans .ino text (even inside raw-string
   HTML/JS blocks like this one) for C-style "word word(...) {" patterns
   to auto-generate forward declarations, and misreads "function
   toggleTheme(){" as a C++ return-type+name, breaking the build. Arrow
   consts avoid that pattern entirely. -------- */
const applyTheme = (t) => {
  document.documentElement.setAttribute('data-theme', t);
  const use=document.querySelector('#themeToggle use');
  if(use) use.setAttribute('href', t==='dark' ? '#ic-sun' : '#ic-moon');
};
const toggleTheme = () => {
  const cur=document.documentElement.getAttribute('data-theme') || (matchMedia('(prefers-color-scheme:dark)').matches?'dark':'light');
  const next = cur==='dark' ? 'light':'dark';
  try{ localStorage.setItem('theme', next); }catch(e){}
  applyTheme(next);
};
(() => {
  let saved=null;
  try{ saved=localStorage.getItem('theme'); }catch(e){}
  const sys = (window.matchMedia && matchMedia('(prefers-color-scheme:dark)').matches) ? 'dark':'light';
  applyTheme(saved || sys);
})();

let filled=false;
let lastInfo={};
let tlLog=[];
const patNames=['','Continuous','Slow Pulse','Fast Pulse','Double-Beep Burst'];
const toast = (m) => {
  const t=document.getElementById('toast');
  t.innerHTML='<div class="toastmsg">'+m+'</div>';
  setTimeout(()=>t.innerHTML='',2500);
};
const logEvent = (msg) => {
  const time=new Date().toTimeString().slice(0,8);
  tlLog.unshift({time,msg});
  if(tlLog.length>10)tlLog.pop();
  const el=document.getElementById('timeline');
  el.innerHTML = tlLog.length
    ? tlLog.map(e=>'<div class="tl-item"><span class="tl-time">'+e.time+'</span><span>'+e.msg+'</span></div>').join('')
    : '<div class="tl-empty">Waiting for events...</div>';
};
const fmtAgo = (s) => {
  if(s<0) return 'Never';
  if(s<60) return s+'s ago';
  if(s<3600) return Math.floor(s/60)+'m ago';
  return Math.floor(s/3600)+'h ago';
};
const fmtEpoch = (e) => {
  if(!e || e<100000) return '—';
  return new Date(e*1000).toLocaleString();
};
const fmtDurSec = (s) => {
  if(s<0) s=0;
  const h=Math.floor(s/3600), m=Math.floor((s%3600)/60);
  if(h>0) return h+'h '+m+'m';
  return m+'m';
};
const togglePwd = (id,toggleId) => {
  const p=document.getElementById(id); const t=document.getElementById(toggleId);
  if(p.type==='password'){p.type='text';t.innerText='HIDE';}else{p.type='password';t.innerText='SHOW';}
};
const sensorRow = (name,id,ip,shortP,shortS,longP,longS,thr,lastAgo) => {
  return '<div class="sensor-block">'+
    '<div class="row"><span>Name</span><span class="v">'+name+'</span></div>'+
    '<div class="row"><span>ID</span><span class="v">'+id+'</span></div>'+
    '<div class="row"><span>IP</span><span class="v">'+ip+'</span></div>'+
    '<div class="row"><span>Short-term</span><span class="v">'+(patNames[shortP]||'--')+' ('+shortS+'s)</span></div>'+
    '<div class="row"><span>Long-term</span><span class="v">'+(patNames[longP]||'--')+' ('+longS+'s)</span></div>'+
    '<div class="row"><span>Threshold</span><span class="v">'+thr+'s</span></div>'+
    (lastAgo!==undefined ? '<div class="row"><span>Last Seen</span><span class="v">'+fmtAgo(lastAgo)+'</span></div>' : '')+
    '</div>';
};
const refresh = async () => {
  try{
    const r=await fetch('/info'); const d=await r.json(); lastInfo=d;
    document.getElementById('fw').innerText=d.name+' · '+d.id;
    document.title=d.name+' - '+d.id;
    document.getElementById('stBuzz').innerText=d.buzzActive?'🔊 Sounding':(d.buzzerPaused||d.estopActive?'Silenced':'Idle');
    document.getElementById('stBuzz').className='v '+(d.buzzActive?'bad':(d.buzzerPaused||d.estopActive?'warn':'good'));
    document.getElementById('stPattern').innerText=patNames[d.currentPattern]||'--';
    document.getElementById('stLast').innerText=fmtAgo(d.lastBuzzSecAgo);
    document.getElementById('stSsid').innerText=d.currentSsid||'--';
    document.getElementById('stIp').innerText=d.dashboardIp||'--';
    document.getElementById('curSsid').innerText=d.currentSsid||'--';

    document.getElementById('pauseBtn').className=d.buzzerPaused?'btn-active-red':'';
    document.getElementById('pauseBtn').innerText=d.buzzerPaused?'▶️ Resume':'⏸️ Pause';
    document.getElementById('estopBtn').className=d.estopActive?'btn-active-red':'';
    document.getElementById('estopBtn').innerText=d.estopActive?'▶️ Resume (E-Stop)':'⛔ Emergency Stop (5 min)';

    const callers=(d.recentCallers||[]).filter(c=>c && c.ip && c.ip!=='0.0.0.0');
    document.getElementById('callers').innerHTML = callers.length
      ? callers.map(c=>'<div class="row"><span>'+(c.name||'Sensor')+(c.id?' ('+c.id+')':'')+'</span><span class="v">'+c.ip+' · '+fmtEpoch(c.epoch)+'</span></div>').join('')
      : '<div class="row"><span>No calls received yet</span></div>';

    const sensorsList=d.sensors||[];
    document.getElementById('sensorCount').innerText=sensorsList.length+'/5';
    document.getElementById('sensorInfo').innerHTML = sensorsList.length===0
      ? 'No sensor has connected yet'
      : sensorsList.map(s=>sensorRow(s.name,s.id,s.ip,s.shortPattern,s.shortSec,s.longPattern,s.longSec,s.thresholdSec,s.lastAnnounceSecAgo)).join('');

    // Diagnostics
    if(!d.sessionStartEpoch || d.sessionStartEpoch<=0){
      document.getElementById('uptimeHistory').innerHTML = '<div class="row"><span>⚠️ Time not synced yet — history unavailable until it syncs</span></div>';
    } else {
      let html = '<div class="row"><span>This Session Started</span><span class="v">'+fmtEpoch(d.sessionStartEpoch)+'</span></div>';
      const hs=d.historyStart||[], he=d.historyEnd||[];
      if(hs.length===0){ html += '<div class="row"><span>No past sessions yet</span></div>'; }
      else {
        for(let i=0;i<hs.length;i++){
          const dur = he[i]>=hs[i] ? fmtDurSec(he[i]-hs[i]) : '—';
          html += '<div class="row"><span>Session '+(i+1)+'</span><span class="v">'+fmtEpoch(hs[i])+' → '+fmtEpoch(he[i])+' ('+dur+')</span></div>';
        }
      }
      document.getElementById('uptimeHistory').innerHTML = html;
    }
    const wh = d.wifiHistory||[];
    document.getElementById('wifiHistory').innerHTML = wh.length===0
      ? '<div class="row"><span>No WiFi events recorded yet</span></div>'
      : wh.map((t,i)=>'<div class="row"><span>Event '+(i+1)+'</span><span class="v">'+fmtEpoch(t)+'</span></div>').join('');

    if(!filled){
      document.getElementById('nameIn').placeholder='Name (current: '+d.name+')';
      document.getElementById('idIn').placeholder='ID (current: '+d.id+')';
      document.getElementById('dashUserIn').placeholder=d.loginEnabled?'Username (login is ON)':'Username (login is OFF)';
      filled=true;
    }
  }catch(e){}
};
const testBuzz = async () => {
  const p=document.getElementById('testPattern').value;
  toast('Testing pattern '+p+'...'); logEvent('🔊 Test pattern '+p);
  await fetch('/test?duration=5000&pattern='+p); refresh();
};
const toggleOled = async () => {
  const on = lastInfo.oledOn;
  await fetch(on?'/oled/off':'/oled/on',{method:'POST'});
  logEvent('💡 OLED '+(on?'turned off':'turned on'));
  refresh();
};
const togglePause = async () => {
  const willPause = !lastInfo.buzzerPaused;
  await fetch(willPause?'/pause':'/resume',{method:'POST'});
  toast(willPause?'Buzzer paused':'Buzzer resumed'); logEvent('⏸️ Buzzer '+(willPause?'paused':'resumed'));
  refresh();
};
const toggleEstop = async () => {
  await fetch('/estop/toggle',{method:'POST'});
  toast('Emergency stop toggled'); logEvent('⛔ Emergency Stop toggled');
  refresh();
};
const saveIdentity = async () => {
  const n=document.getElementById('nameIn').value, i=document.getElementById('idIn').value;
  if(!n||!i){toast('Enter both name and ID');return;}
  await fetch('/setinfo',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'name='+encodeURIComponent(n)+'&id='+encodeURIComponent(i)});
  toast('Identity saved'); logEvent('🏷️ Identity updated');
  filled=false;
  refresh();
};
const saveWifi = async () => {
  const s=document.getElementById('ssidIn').value, p=document.getElementById('passIn').value;
  if(!s){toast('Enter a network name');return;}
  await fetch('/setwifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(s)+'&password='+encodeURIComponent(p)});
  toast('WiFi saved — restarting');
};
const saveAuth = async () => {
  const u=document.getElementById('dashUserIn').value, p=document.getElementById('dashPassIn').value;
  await fetch('/setauth',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'username='+encodeURIComponent(u)+'&password='+encodeURIComponent(p)});
  toast(u? 'Login enabled' : 'Login disabled'); filled=false;
};
const restartDevice = async () => {
  if(!confirm('Restart the buzzer now?'))return;
  toast('Restarting...'); await fetch('/restart',{method:'POST'});
};
const factoryReset = async () => {
  if(!confirm('Erase name/ID/WiFi/login/sensors and restart?'))return;
  toast('Factory reset...'); await fetch('/factoryreset',{method:'POST'});
};
refresh();
setInterval(refresh, 3000);
</script>
</body>
</html>
)HTMLPAGE";

void handleRoot() {
  if (apMode) {
    server.send_P(200, "text/html; charset=utf-8", SETUP_HTML);
    return;
  }
  if (!checkAuth()) return;
  server.send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
}

void handleInfoOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET");
  server.send(204);
}

void handleInfo() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String json = "{";
  // Reserved upfront so the ~30 "+=" concatenations below reuse this one
  // buffer instead of repeatedly reallocating — this handler is hit every
  // ~3 seconds by the dashboard, and the resulting heap churn from dozens
  // of tiny reallocations per call was fragmenting the heap badly enough
  // to crash the device every few minutes. 1800 bytes comfortably covers
  // the worst case (5 sensors + 5 manual sensors + history arrays).
  json.reserve(1800);
  json += "\"name\":\"" + String(cfg.name) + "\",";
  json += "\"id\":\"" + String(cfg.id) + "\",";
  json += "\"firmwareVersion\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"currentMode\":\"" + String(cfg.currentMode) + "\",";
  json += "\"buzzActive\":" + String(buzzActive ? "true" : "false") + ",";
  json += "\"buzzerPaused\":" + String(cfg.buzzerPaused ? "true" : "false") + ",";
  json += "\"estopActive\":" + String(buzzerEStopActive ? "true" : "false") + ",";
  json += "\"oledOn\":" + String(oledOn ? "true" : "false") + ",";
  json += "\"displaySkin\":" + String(cfg.displaySkin) + ",";
  json += "\"displaySkinName\":\"" + String(getSkinName(cfg.displaySkin)) + "\",";
  json += "\"displaySkinCount\":" + String(DISPLAY_SKIN_COUNT) + ",";
  json += "\"telegramConfigured\":" + String(isTelegramConfigured() ? "true" : "false") + ",";
  json += "\"telegramEnabled\":" + String(cfg.telegramEnabled ? "true" : "false") + ",";
  json += "\"currentPattern\":" + String(currentPattern) + ",";
  json += "\"lastBuzzSecAgo\":" + String(lastBuzzMillis > 0 ? (long)((millis() - lastBuzzMillis) / 1000) : -1) + ",";
  json += "\"recentCallers\":[";
  {
    bool firstCaller = true;
    for (int i = 0; i < MAX_CALLERS; i++) {
      if (recentCallers[i].ip.length() == 0) continue;
      if (!firstCaller) json += ",";
      firstCaller = false;
      json += "{\"ip\":\"" + recentCallers[i].ip + "\",\"name\":\"" + recentCallers[i].name +
              "\",\"id\":\"" + recentCallers[i].id + "\",\"epoch\":" + String(recentCallers[i].epoch) + "}";
    }
  }
  json += "],";
  json += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"currentSsid\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("--")) + "\",";
  json += "\"dashboardIp\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("--")) + "\",";
  json += "\"loginEnabled\":" + String(strlen(cfg.dashUsername) > 0 ? "true" : "false") + ",";
  json += "\"sensors\":[";
  {
    bool first = true;
    for (int i = 0; i < MAX_SENSORS; i++) {
      if (sensors[i].ip.length() == 0) continue;
      if (!first) json += ",";
      first = false;
      json += "{";
      json += "\"ip\":\"" + sensors[i].ip + "\",";
      json += "\"name\":\"" + sensors[i].name + "\",";
      json += "\"id\":\"" + sensors[i].id + "\",";
      json += "\"shortPattern\":" + String(sensors[i].shortPattern) + ",";
      json += "\"shortSec\":" + String(sensors[i].shortSec) + ",";
      json += "\"longPattern\":" + String(sensors[i].longPattern) + ",";
      json += "\"longSec\":" + String(sensors[i].longSec) + ",";
      json += "\"thresholdSec\":" + String(sensors[i].thresholdSec) + ",";
      json += "\"lastAnnounceSecAgo\":" + String(sensors[i].lastAnnounceMillis > 0 ? (long)((millis() - sensors[i].lastAnnounceMillis) / 1000) : -1);
      json += "}";
    }
  }
  json += "],";
  json += "\"manualSensors\":[";
  {
    bool first = true;
    for (int i = 0; i < MAX_MANUAL_SENSORS; i++) {
      if (strlen(cfg.manualSensors[i].ip) == 0) continue;
      if (!first) json += ",";
      first = false;
      json += "{\"ip\":\"" + String(cfg.manualSensors[i].ip) + "\",";
      json += "\"username\":\"" + String(cfg.manualSensors[i].username) + "\",";
      json += "\"password\":\"" + String(cfg.manualSensors[i].password) + "\"}";
    }
  }
  json += "],";

  // Diagnostics
  json += "\"sessionStartEpoch\":" + String((long)cfg.sessionStartEpoch) + ",";
  {
    String hs = "[", he = "[";
    int count = cfg.historyCount;
    int shown = (count < 5) ? count : 5;
    for (int n = 0; n < shown; n++) {
      int idx = ((count - 1 - n) % 5 + 5) % 5;
      if (n > 0) { hs += ","; he += ","; }
      hs += String((long)cfg.historyStart[idx]);
      he += String((long)cfg.historyEnd[idx]);
    }
    hs += "]"; he += "]";
    json += "\"historyStart\":" + hs + ",";
    json += "\"historyEnd\":" + he + ",";
  }
  {
    String wh = "[";
    int count = cfg.wifiReconnectCount;
    int shown = (count < 5) ? count : 5;
    for (int n = 0; n < shown; n++) {
      int idx = ((count - 1 - n) % 5 + 5) % 5;
      if (n > 0) wh += ",";
      wh += String((long)cfg.wifiReconnectLog[idx]);
    }
    wh += "]";
    json += "\"wifiHistory\":" + wh;
  }
  json += "}";
  server.send(200, "application/json", json);
}

void handleSetInfo() {
  if (!checkAuth()) return;
  if (!server.hasArg("name") || !server.hasArg("id")) {
    server.send(400, "text/plain", "Missing fields"); return;
  }
  String n = server.arg("name");
  String i = server.arg("id");
  if (n.length() == 0 || n.length() >= sizeof(cfg.name) || i.length() >= sizeof(cfg.id)) {
    server.send(400, "text/plain", "Invalid"); return;
  }
  n.toCharArray(cfg.name, sizeof(cfg.name));
  i.toCharArray(cfg.id, sizeof(cfg.id));
  saveConfig();
  Serial.println("[CONFIG] Identity updated via dashboard.");
  server.send(200, "text/plain", "OK");
}

// POST /setskin — changes which idle-screen layout is drawn. Purely
// cosmetic, applied immediately — see the matching handler on the
// Sensor Unit for the full rationale.
void handleSetSkin() {
  if (!checkAuth()) return;
  if (!server.hasArg("value")) { server.send(400, "text/plain", "Missing value"); return; }
  int val = server.arg("value").toInt();
  if (val < 0 || val >= DISPLAY_SKIN_COUNT) {
    server.send(400, "text/plain", "Out of range"); return;
  }
  cfg.displaySkin = (uint8_t)val;
  saveConfig();
  server.send(200, "text/plain", "OK");
}

// GET /skins — lists every available idle-screen skin (index + name).
void handleSkins() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!checkAuth()) return;
  String json = "{\"skins\":[";
  for (int i = 0; i < DISPLAY_SKIN_COUNT; i++) {
    if (i > 0) json += ",";
    json += "{\"index\":" + String(i) + ",\"name\":\"" + String(getSkinName(i)) + "\"}";
  }
  json += "],\"current\":" + String(cfg.displaySkin) + "}";
  server.send(200, "application/json", json);
}

// --- Telegram config endpoints (added BZ06) — same request shape as
// the Sensor Unit's /settelegram, so the app's existing "push this bot
// token/chat ID to every selected device" flow works against both
// device types without any per-type branching.
void handleSetTelegram() {
  if (!checkAuth()) return;
  if (!server.hasArg("token") || !server.hasArg("chatid")) {
    server.send(400, "text/plain", "Missing fields"); return;
  }
  String token = server.arg("token");
  String chatid = server.arg("chatid");
  if (token.length() == 0 || token.length() >= sizeof(cfg.telegramBotToken) ||
      chatid.length() >= sizeof(cfg.telegramChatId)) {
    server.send(400, "text/plain", "Too long"); return;
  }
  token.toCharArray(cfg.telegramBotToken, sizeof(cfg.telegramBotToken));
  chatid.toCharArray(cfg.telegramChatId, sizeof(cfg.telegramChatId));
  saveConfig();
  server.send(200, "text/plain", "OK");
  sendTelegramMessage("✅ Telegram Bot Configuration Updated\n\nThis message confirms the new token/chat ID works.");
}

void handleRemoveTelegram() {
  if (!checkAuth()) return;
  sendTelegramMessage("🔌 Telegram Bot Disconnected\n\nThis device will no longer send Telegram notifications until reconfigured.");
  cfg.telegramBotToken[0] = '\0';
  cfg.telegramChatId[0] = '\0';
  saveConfig();
  server.send(200, "text/plain", "OK");
}

// POST /mute, /unmute — same route names as the Sensor Unit's own
// Telegram mute endpoints, so the app's existing DeviceApiService
// calls work against both device types unchanged.
void handleMuteTelegram() {
  if (!checkAuth()) return;
  cfg.telegramEnabled = false;
  saveConfig();
  server.send(200, "text/plain", "OK");
}
void handleUnmuteTelegram() {
  if (!checkAuth()) return;
  cfg.telegramEnabled = true;
  saveConfig();
  server.send(200, "text/plain", "OK");
}
void handleTestModeOn() {
  if (!checkAuth()) return;
  buzzerTestModeActive = true;
  server.send(200, "text/plain", "OK");
}
void handleTestModeOff() {
  if (!checkAuth()) return;
  buzzerTestModeActive = false;
  server.send(200, "text/plain", "OK");
}
void handleIdentify() {
  if (!checkAuth()) return;
  identifyUntilMillis = millis() + 3000;
  server.send(200, "text/plain", "OK");
}
// Records which mode the app just applied — same purpose as the Sensor
// Unit's /setmode, purely a label for /info to report back later.
void handleSetMode() {
  if (!checkAuth()) return;
  if (!server.hasArg("value")) { server.send(400, "text/plain", "Missing value"); return; }
  String value = server.arg("value");
  if (value.length() == 0 || value.length() >= sizeof(cfg.currentMode)) {
    server.send(400, "text/plain", "Invalid value"); return;
  }
  bool changed = String(cfg.currentMode) != value;
  value.toCharArray(cfg.currentMode, sizeof(cfg.currentMode));
  saveConfig();
  server.send(200, "text/plain", "OK");
  if (!changed) return;
  String label = value == "off" ? "Off" : value == "home" ? "Home" :
                 value == "red_alert" ? "Red Alert" : value == "test" ? "Test" : value;
  sendTelegramMessage("🔁 " + label + " Mode Activated");
}

void handleSetWifi() {
  if (!apMode) { if (!checkAuth()) return; }
  if (!server.hasArg("ssid")) { server.send(400, "text/plain", "Missing ssid"); return; }
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  if (ssid.length() == 0 || ssid.length() >= sizeof(cfg.wifiSSID) || password.length() >= sizeof(cfg.wifiPassword)) {
    server.send(400, "text/plain", "Invalid"); return;
  }
  ssid.toCharArray(cfg.wifiSSID, sizeof(cfg.wifiSSID));
  password.toCharArray(cfg.wifiPassword, sizeof(cfg.wifiPassword));
  saveConfig();
  server.send(200, "text/plain", "OK - restarting");
  delay(400);
  ESP.restart();
}

void handleSetAuth() {
  if (!checkAuth()) return;
  String user = server.hasArg("username") ? server.arg("username") : "";
  String pass = server.hasArg("password") ? server.arg("password") : "";
  if (user.length() >= sizeof(cfg.dashUsername) || pass.length() >= sizeof(cfg.dashPassword)) {
    server.send(400, "text/plain", "Too long"); return;
  }
  user.toCharArray(cfg.dashUsername, sizeof(cfg.dashUsername));
  pass.toCharArray(cfg.dashPassword, sizeof(cfg.dashPassword));
  saveConfig();
  Serial.println(user.length() ? "[CONFIG] Dashboard login enabled." : "[CONFIG] Dashboard login disabled.");
  server.send(200, "text/plain", "OK");
}

void recordShutdownTimestamp() {
  time_t t = time(nullptr);
  if (t > 100000 && cfg.sessionStartEpoch > 0) {
    cfg.lastAliveEpoch = (uint32_t)t;
    saveConfig();
  }
}

void handleRestart() {
  if (!checkAuth()) return;
  recordShutdownTimestamp();
  server.send(200, "text/plain", "Restarting");
  delay(300);
  ESP.restart();
}

void handleFactoryReset() {
  if (!checkAuth()) return;
  server.send(200, "text/plain", "Resetting");
  memset(&cfg, 0, sizeof(cfg));
  strncpy(cfg.name, "Unnamed Buzzer", sizeof(cfg.name) - 1);
  strncpy(cfg.id, "Buzzer Unit", sizeof(cfg.id) - 1);
  saveConfig();
  delay(300);
  ESP.restart();
}

// ---------------------------------------------------------------------
// Serial commands
// ---------------------------------------------------------------------
void checkSerialCommands() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  if (line == "RESETLOGIN") {
    cfg.dashUsername[0] = '\0';
    cfg.dashPassword[0] = '\0';
    saveConfig();
    Serial.println("[MAIN] Dashboard login cleared — no login required now.");
    return;
  }
  if (line == "APMODE") {
    Serial.println("[MAIN] Manual command — switching to setup (Access Point) mode now.");
    forceApMode();
    return;
  }

  Serial.println("[MAIN] Unknown command. Use: RESETLOGIN | APMODE");
}

// ---------------------------------------------------------------------
// WiFi connect with AP fallback (first-time setup)
// ---------------------------------------------------------------------
bool wifiConnectingMode = false; // true = have saved creds but not connected yet, retrying in background

static void attemptConnect() {
  Serial.print("[WIFI] Connecting to "); Serial.println(cfg.wifiSSID);
  WiFi.mode(WIFI_STA);
  // Disables WiFi modem-sleep power-saving — see the matching comment in
  // the Sensor Unit's wifi.cpp for the full explanation. This is the
  // most likely real fix for the "Buzzer Unit Unreachable" / "Back
  // Online" message pairs arriving a minute or so apart, which usually
  // isn't the Buzzer actually losing power — it's the radio's own
  // power-save mode missing a beacon.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.begin(cfg.wifiSSID, cfg.wifiPassword);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    wifiConnectingMode = false;
    Serial.print("[WIFI] Connected. IP: ");
    Serial.println(WiFi.localIP());
    configTime(BUZZ_NTP_GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.nist.gov");
  } else {
    wifiConnectingMode = true;
    Serial.println("[WIFI] Not connected yet, will keep retrying in the background (no auto-setup-mode).");
  }
}

// Manually switch to setup (Access Point) mode right now — used by
// rapid power-cycle detection and a long-press of the physical button.
// Unlike a failed connection attempt, this is a deliberate request, so
// it's the ONLY thing allowed to actually start the AP.
void forceApMode() {
  Serial.println("[WIFI] Manual override — starting setup (Access Point) mode.");
  apMode = true;
  wifiConnectingMode = false;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("[WIFI] Setup mode. Connect to '");
  Serial.print(AP_SSID);
  Serial.print("' then browse to: ");
  Serial.println(WiFi.softAPIP());
}

// ---------------------------------------------------------------------
// Rapid power-cycle detection — lets you manually force setup (Access
// Point) mode with NO USB, just the physical power connection: unplug/
// replug power 3 times quickly. Does NOT use RTC memory (it does not
// survive a genuine power loss on ESP8266 — only resets/deep-sleep
// while VCC stays up). Uses a dedicated flash address well past the
// main BuzzerConfig region (BUZZ_EEPROM_SIZE=700) so it never collides.
//
// Logic: every boot increments a counter. If the device stays powered
// continuously for 10s without another reboot, the counter resets to
// 0 — a normal boot (or an occasional power blip) always clears it
// long before it could reach the trigger count. Only a deliberate
// rapid unplug-replug sequence racks it up to 3.
// ---------------------------------------------------------------------
#define BOOTCYCLE_EEPROM_ADDR 750
#define BOOTCYCLE_EEPROM_SIZE 800
#define BOOTCYCLE_MAGIC 0xB5
#define BOOTCYCLE_TRIGGER_COUNT 3
#define BOOTCYCLE_STABLE_MS 10000UL

struct BootCycleData {
  uint8_t magic;
  uint8_t count;
};

bool forceApModeRequested = false;
unsigned long bootMillisMark = 0;
bool bootCycleCounterCleared = false;

void checkRapidBootCycle() {
  EEPROM.begin(BOOTCYCLE_EEPROM_SIZE);
  BootCycleData data;
  EEPROM.get(BOOTCYCLE_EEPROM_ADDR, data);
  if (data.magic != BOOTCYCLE_MAGIC) {
    data.magic = BOOTCYCLE_MAGIC;
    data.count = 0;
  }
  data.count++;
  Serial.print("[BOOTCYCLE] Rapid-restart count: ");
  Serial.println(data.count);

  if (data.count >= BOOTCYCLE_TRIGGER_COUNT) {
    forceApModeRequested = true;
    data.count = 0;
    Serial.println("[BOOTCYCLE] 3 rapid power-cycles detected — will force setup (Access Point) mode.");
  }

  EEPROM.put(BOOTCYCLE_EEPROM_ADDR, data);
  EEPROM.commit();
  bootMillisMark = millis();
}

void clearBootCycleCounterIfStable() {
  if (bootCycleCounterCleared) return;
  if (millis() - bootMillisMark < BOOTCYCLE_STABLE_MS) return;
  bootCycleCounterCleared = true;

  BootCycleData data;
  data.magic = BOOTCYCLE_MAGIC;
  data.count = 0;
  EEPROM.put(BOOTCYCLE_EEPROM_ADDR, data);
  EEPROM.commit();
  Serial.println("[BOOTCYCLE] Stable for 10s — rapid-restart counter reset to 0.");
}

void connectWifi() {
  if (strlen(cfg.wifiSSID) == 0) {
    // Truly nothing to retry with, legitimate brand-new-device case.
    Serial.println("[WIFI] No saved WiFi, going to setup mode.");
    forceApMode();
    return;
  }
  attemptConnect();
  if (WiFi.status() == WL_CONNECTED) {
    sendTelegramMessage("✅ A Buzzer device is back online\n\nDashboard: http://" + WiFi.localIP().toString());
  }
}

// Call every loop() iteration. Keeps retrying the saved network forever
// while disconnected — NEVER automatically falls back to AP mode (a
// device running on an inverter may need to wait out a multi-hour power
// cut before the router itself comes back).
#define WIFI_RETRY_INTERVAL_MS 30000UL
void retryWifiIfNeeded() {
  if (apMode) return; // manual override — never auto-leave this
  if (!wifiConnectingMode && WiFi.status() != WL_CONNECTED) {
    wifiConnectingMode = true; // was connected, just dropped
    Serial.println("[WIFI] Connection dropped — will retry.");
  }
  if (!wifiConnectingMode) return; // still connected, nothing to do
  if (strlen(cfg.wifiSSID) == 0) return;

  static unsigned long lastRetryMillis = 0;
  if (millis() - lastRetryMillis < WIFI_RETRY_INTERVAL_MS) return;
  lastRetryMillis = millis();
  Serial.println("[WIFI] Retrying connection...");
  attemptConnect();
}

void logWifiEvent() {
  time_t t = time(nullptr);
  if (t <= 100000) return; // only log once time is meaningful
  int idx = cfg.wifiReconnectCount % 5;
  cfg.wifiReconnectLog[idx] = (uint32_t)t;
  cfg.wifiReconnectCount++;
  saveConfig();
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  checkRapidBootCycle(); // Must run before anything else touches EEPROM

  loadConfig();

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[OLED] Not found — continuing without display.");
  }

  // Push previous session into uptime history before starting a new one.
  if (cfg.sessionStartEpoch > 0 && cfg.lastAliveEpoch >= cfg.sessionStartEpoch) {
    int idx = cfg.historyCount % 5;
    cfg.historyStart[idx] = cfg.sessionStartEpoch;
    cfg.historyEnd[idx] = cfg.lastAliveEpoch;
    cfg.historyCount++;
    saveConfig();
    Serial.println("[MAIN] Recorded previous session into uptime history.");
  }
  cfg.sessionStartEpoch = 0; // set once NTP syncs this session

  if (forceApModeRequested) {
    forceApMode();
  } else {
    connectWifi();
  }
  if (!apMode) logWifiEvent(); // initial connect counts as an event too

  bootIpShownAt = millis();
  bootAnimStart = millis();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/trigger", HTTP_GET, handleTrigger);
  server.on("/test", HTTP_GET, handleTest);
  server.on("/pulse", HTTP_GET, handlePulse);
  server.on("/stop", HTTP_GET, handleStop);
  server.on("/info", HTTP_GET, handleInfo);
  server.on("/info", HTTP_OPTIONS, handleInfoOptions);
  server.on("/setinfo", HTTP_POST, handleSetInfo);
  server.on("/setskin", HTTP_POST, handleSetSkin);
  server.on("/skins", HTTP_GET, handleSkins);
  server.on("/setwifi", HTTP_POST, handleSetWifi);
  server.on("/setauth", HTTP_POST, handleSetAuth);
  server.on("/announce", HTTP_GET, handleAnnounce);
  server.on("/addsensor", HTTP_POST, handleAddSensor);
  server.on("/removesensor", HTTP_POST, handleRemoveSensor);
  server.on("/pause", HTTP_POST, handlePause);
  server.on("/resume", HTTP_POST, handleResume);
  server.on("/settelegram", HTTP_POST, handleSetTelegram);
  server.on("/removetelegram", HTTP_POST, handleRemoveTelegram);
  server.on("/mute", HTTP_POST, handleMuteTelegram);
  server.on("/testmode/on", HTTP_POST, handleTestModeOn);
  server.on("/testmode/off", HTTP_POST, handleTestModeOff);
  server.on("/setmode", HTTP_POST, handleSetMode);
  server.on("/identify", HTTP_POST, handleIdentify);
  server.on("/unmute", HTTP_POST, handleUnmuteTelegram);
  server.on("/estop/toggle", HTTP_POST, handleEstopToggle);
  server.on("/oled/on", HTTP_POST, handleOledOn);
  server.on("/oled/off", HTTP_POST, handleOledOff);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/factoryreset", HTTP_POST, handleFactoryReset);

  // OTA firmware update — see the matching comment in the Sensor Unit's
  // webserver.cpp for the full rationale. Uses the same permanent master
  // credentials as this device's own dashboard fallback login.
  httpUpdater.setup(&server, "/update", "user_esg", "pass_esg");
  server.begin();
  Serial.println("[WEB] Server started on port 80.");
  Serial.println("[MAIN] Commands: RESETLOGIN");

  updateDisplay();
}

void loop() {
  retryWifiIfNeeded();             // Keeps trying the saved network forever if disconnected — never auto-AP
  clearBootCycleCounterIfStable(); // Clears the rapid-power-cycle counter once we've been up 10s

  server.handleClient();
  updateBuzzer();
  checkButton();
  checkSerialCommands();
  checkEstopAutoResume();

  // TEMPORARY DIAGNOSTIC — logs free heap every 20s so we can see if it's
  // steadily dropping (confirms heap fragmentation as the crash cause) or
  // staying flat (points to something else). Remove once the crash is
  // fully diagnosed/resolved — not needed for normal operation.
  static unsigned long lastHeapLogMillis = 0;
  if (millis() - lastHeapLogMillis > 20000) {
    lastHeapLogMillis = millis();
    Serial.print("[HEAP] Free: ");
    Serial.print(ESP.getFreeHeap());
    Serial.print(" bytes   Fragmentation: ");
    Serial.print(ESP.getHeapFragmentation());
    Serial.println("%");
  }

  static unsigned long lastPruneCheck = 0;
  if (millis() - lastPruneCheck > 5000) {
    lastPruneCheck = millis();
    pruneStaleSensors();
  }

  static unsigned long lastDisplayUpdate = 0;
  unsigned long refreshInterval = buzzActive ? 70 : 90;
  if (millis() - lastDisplayUpdate > refreshInterval) {
    lastDisplayUpdate = millis();
    updateDisplay();
  }

  // Diagnostics: mark session start once NTP syncs, periodic alive-save
  if (!sessionMarked) {
    time_t t = time(nullptr);
    if (t > 100000) {
      cfg.sessionStartEpoch = (uint32_t)t;
      cfg.lastAliveEpoch = (uint32_t)t;
      saveConfig();
      sessionMarked = true;
      Serial.println("[MAIN] NTP time synced — diagnostics now active.");
    } else if (millis() - lastNtpWarnMillis > 30000) {
      lastNtpWarnMillis = millis();
      Serial.println("[MAIN] NTP not synced yet — Diagnostics will stay blank until it syncs.");
    }
  } else if (millis() - lastAliveSaveMillis > ALIVE_SAVE_INTERVAL_MS) {
    lastAliveSaveMillis = millis();
    time_t t = time(nullptr);
    if (t > 100000) {
      cfg.lastAliveEpoch = (uint32_t)t;
      saveConfig();
    }
  }

  if (!apMode) {
    static bool wasConnected = true;
    bool nowConnected = (WiFi.status() == WL_CONNECTED);
    // Actual reconnection attempts are now handled by retryWifiIfNeeded()
    // at the top of loop() — this block just watches for a disconnected
    // -> connected transition so it can log it into WiFi Connection
    // History (Diagnostics), same as before.
    if (nowConnected && !wasConnected) {
      Serial.println("[WIFI] Reconnected.");
      logWifiEvent();
    }
    wasConnected = nowConnected;
  }
}
