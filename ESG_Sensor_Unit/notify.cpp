#include "notify.h"
#include "config.h"
#include "alarm.h"
#include "wifi.h"
#include "stats.h"
#include <WiFiClientSecure.h>
#include <time.h>

namespace Notify {

static Settings *gSettings = nullptr;
static bool gTestModeActive = false;

void setTestMode(bool active) {
  gTestModeActive = active;
}
static bool *gArmed = nullptr;
static float *gLastDistanceCm = nullptr;
static bool *gEStopActive = nullptr;
static unsigned long *gEStopStartMillis = nullptr;
static unsigned long *gMuteStartMillis = nullptr;

static uint32_t totalSent = 0;
static uint32_t failedCount = 0;
static unsigned long lastAlertMillis = 0;
static uint32_t lastAlertEpoch = 0;
static unsigned long lastSuccessMillis = 0;
static unsigned long lastResponseMs = 0;

static long lastUpdateId = 0;
static unsigned long lastPollMillis = 0;
#define TELEGRAM_POLL_INTERVAL_MS 17000 // Deliberately NOT a factor of 60000 (buzzer heartbeat interval) — was 20000, which meant both blocking network calls fired in the SAME loop pass every 60s, stacking their blocking time enough to trip the watchdog and force a reboot.
#define TELEGRAM_POLL_MAX_WAIT_MS 1200  // Was 4000 — getUpdates with timeout=0 replies almost instantly

static String formatTime(time_t t) {
  if (t < 100000) return "time not synced";
  char buf[32];
  strftime(buf, sizeof(buf), "%d-%b-%Y %H:%M:%S", localtime(&t));
  return String(buf);
}

static String fmtDur(unsigned long ms) {
  unsigned long s = ms / 1000;
  unsigned long h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
  if (h > 0) return String(h) + "h " + String(m) + "m";
  if (m > 0) return String(m) + "m " + String(sec) + "s";
  return String(sec) + "s";
}

void begin(Settings *settingsPtr, bool *armedPtr, float *lastDistanceCmPtr,
           bool *emergencyStopActivePtr, unsigned long *emergencyStopStartMillisPtr,
           unsigned long *muteStartMillisPtr) {
  gSettings = settingsPtr;
  gArmed = armedPtr;
  gLastDistanceCm = lastDistanceCmPtr;
  gEStopActive = emergencyStopActivePtr;
  gEStopStartMillis = emergencyStopStartMillisPtr;
  gMuteStartMillis = muteStartMillisPtr;
}

String currentTimeString() { return formatTime(time(nullptr)); }

String timeStringPlusSeconds(long secs) {
  time_t t = time(nullptr);
  if (t < 100000) return "time not synced";
  return formatTime(t + secs);
}

bool isConfigured() {
  if (gSettings == nullptr) return false;
  return String(gSettings->telegramBotToken) != "PASTE_YOUR_BOT_TOKEN_HERE" &&
         strlen(gSettings->telegramBotToken) > 0;
}

void sendTextMessage(const String &text, bool ignorePause, bool isSecurityAlert) {
  if (!isConfigured()) {
    Serial.println("[NOTIFY] Telegram not configured, skipping.");
    return;
  }
  if (gSettings != nullptr && !gSettings->alarmEnabled && !ignorePause) {
    Serial.println("[NOTIFY] Telegram alerts paused, message suppressed.");
    return;
  }
  // Security alerts (intrusion, sustained activity, sensor health) are
  // never gated by the "Other Notifications" master toggle below,
  // only the alarmEnabled pause above can silence them, that's the
  // one thing this toggle deliberately doesn't touch.
  if (!isSecurityAlert && gSettings != nullptr && !gSettings->notifyOtherEnabled) {
    Serial.println("[NOTIFY] Other Notifications disabled, non-alert message suppressed.");
    return;
  }

  // Every notification includes which device sent it — appended here
  // once so every call site gets this automatically, no need to edit
  // every message individually.
  String fullText = text;
  if (gTestModeActive) {
    fullText = "🧪 TEST MODE\n\n" + fullText;
  }
  if (gSettings != nullptr) {
    fullText += "\n\nDevice name: " + String(gSettings->deviceName) + ", ID: " + String(gSettings->deviceId);
  }

  unsigned long start = millis();
  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(512, 512); // Default BearSSL buffers (~16KB) can OOM-crash the ESP8266 right after boot when other modules already hold most of the heap. 512B is plenty for Telegram's small JSON responses.
  client.setTimeout(4000);
  bool connected = client.connect("api.telegram.org", 443);
  if (!connected) {
    // Transient failures are common right after WiFi just connected
    // (network stack/DNS not fully settled yet) — one quick retry
    // fixes most of these. MUST call stop() first — reconnecting the
    // same WiFiClientSecure object without it corrupts the BearSSL
    // session state and crashes the device (this was the actual bug
    // causing restarts on every button that sends a Telegram message).
    client.stop();
    delay(600);
    connected = client.connect("api.telegram.org", 443);
  }
  if (!connected) {
    Serial.println("[NOTIFY] Connect failed (after retry).");
    failedCount++;
    return;
  }
  String msg = fullText;
  msg.replace("\n", "%0A");
  msg.replace(" ", "%20");
  String url = "/bot" + String(gSettings->telegramBotToken) + "/sendMessage?chat_id=" +
               String(gSettings->telegramChatId) + "&text=" + msg;
  client.print(String("GET ") + url + " HTTP/1.1\r\nHost: api.telegram.org\r\nConnection: close\r\n\r\n");
  unsigned long t0 = millis();
  while (client.connected() && millis() - t0 < 5000) {
    if (client.available()) client.read();
    yield(); // Explicitly feed the watchdog during this wait
  }
  client.stop();
  lastResponseMs = millis() - start;
  lastSuccessMillis = millis();
  totalSent++;
  Serial.println("[NOTIFY] Telegram message sent.");
}

void sendIntruderAlert(float distanceCm, uint32_t triggerCount) {
  (void)distanceCm; // no longer shown in the message, kept in the signature so the call site doesn't need updating
  lastAlertMillis = millis();
  time_t t = time(nullptr);
  if (t > 100000) lastAlertEpoch = (uint32_t)t;
  String msg = "🚨 Possible Intruder Detected\n\n";
  msg += "Event ID: #" + String(triggerCount) + "\n";
  msg += "Time: " + currentTimeString() + "\n\n";
  msg += "Please check the protected area.";
  sendTextMessage(msg, false, true);
}

static void sendFullStatusReport() {
  if (gSettings == nullptr) return;

  float distanceCm = (gLastDistanceCm != nullptr) ? *gLastDistanceCm : -1.0f;
  bool calibrated = gSettings->triggerDistanceCm > 0;
  bool muted = !gSettings->alarmEnabled;

  long estopRemaining = 0;
  if (*gEStopActive) {
    long elapsed = (millis() - *gEStopStartMillis) / 1000;
    long total = (long)EMERGENCY_STOP_DURATION_MIN * 60;
    estopRemaining = total - elapsed; if (estopRemaining < 0) estopRemaining = 0;
  }
  unsigned long muteElapsed = *gMuteStartMillis > 0 ? (millis() - *gMuteStartMillis) / 1000 : 0;

  String r = "📋 Full System Status\n\n";

  r += "— Live Status —\n";
  r += "📏 Sensor Reading: " + String(distanceCm > 0 ? String(distanceCm,1)+" cm" : "No reading") + "\n";
  r += "🎯 Trigger Distance: " + String(calibrated ? String(gSettings->triggerDistanceCm,1)+" cm" : "Not calibrated") + "\n";
  r += "📐 Baseline Distance: " + String(calibrated ? String(gSettings->wallDistanceCm,1)+" cm" : "Not calibrated") + "\n";
  r += "🔢 Total Triggers: " + String(Alarm::getTriggerCount()) + "\n\n";

  r += "— Modes —\n";
  r += "🛡️ Armed: " + String(*gArmed ? "ON" : "OFF") + "\n";
  r += "🌙 Night Mode: " + String(gSettings->nightMode ? "ON" : "OFF") + "\n";
  r += "🔕 Pause Alerts: " + String(muted ? ("ON ("+fmtDur(muteElapsed*1000)+")") : "OFF") + "\n";
  r += "⛔ Emergency Stop: " + String(*gEStopActive ? ("ON — resumes in "+String(estopRemaining)+"s") : "OFF") + "\n\n";

  r += "— Security —\n";
  r += "📐 Calibration: " + String(calibrated ? "Calibrated" : "Not calibrated") + "\n";
  r += "🚨 Last Detection: " + String(Alarm::getLastTriggerMillis()>0 ? fmtDur(millis()-Alarm::getLastTriggerMillis())+" ago" : "Never") + "\n";
  r += "📅 Today's Intrusions: " + String(Stats::getTodayIntrusions()) + "\n\n";

  r += "— Sensor —\n";
  r += "✅ Sensor Health: " + Stats::getSensorHealth() + "\n";
  r += "⚠️ Frozen Warning: " + String(Stats::isSensorFrozen() ? "YES — check sensor" : "No") + "\n\n";

  r += "— Network —\n";
  r += "📶 SSID: " + String(gSettings->wifiSSID) + "\n";
  r += "📡 RSSI: " + String(WiFiManager::getRSSI()) + " dBm\n";
  r += "🌍 IP: " + WiFiManager::getIPAddress() + "\n\n";

  r += "— Telegram —\n";
  r += "🤖 Bot Status: Connected\n";
  r += "📬 Total Alerts Sent: " + String(totalSent) + "\n\n";

  r += "— Statistics (since reboot) —\n";
  r += "📅 Today's Alerts: " + String(Stats::getTodayAlerts()) + "\n";
  r += "🛡️ Total Protection Time: " + fmtDur(Stats::getProtectionTimeMs()) + "\n";
  r += "🔕 Total Pause Time: " + fmtDur(Stats::getPauseTimeMs()) + "\n";
  r += "⛔ Total Emergency Stop Time: " + fmtDur(Stats::getEstopTimeMs()) + "\n\n";

  r += "— Device —\n";
  r += "💡 OLED: " + String(gSettings->oledOn ? "ON" : "OFF") + "\n";
  r += "🔊 Wireless Siren: Not yet implemented\n\n";

  r += "— Buzzer Timing —\n";
  r += "🔊 Buzzer Hardware: " + String(gSettings->buzzerMasterEnabled ? "ON" : "OFF") + "\n";
  r += "⏱️ Short-term Duration: " + String(gSettings->alarmDurationSec) + "s\n";
  r += "⏱️ Long-term (Sustained) Duration: " + String(gSettings->longTermBuzzerDurationSec) + "s\n";
  r += "⏳ Sustained Activity Threshold: " + String(gSettings->sustainedThresholdSec) + "s";

  sendTextMessage(r);
}

void checkIncomingCommands() {
  if (!isConfigured()) return;
  unsigned long now = millis();
  if (now - lastPollMillis < TELEGRAM_POLL_INTERVAL_MS) return;
  lastPollMillis = now;

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(512, 512); // Default BearSSL buffers (~16KB) can OOM-crash the ESP8266 right after boot when other modules already hold most of the heap. 512B is plenty for Telegram's small JSON responses.
  client.setTimeout(3000);
  if (!client.connect("api.telegram.org", 443)) return;

  String url = "/bot" + String(gSettings->telegramBotToken) + "/getUpdates?offset=" +
               String(lastUpdateId + 1) + "&limit=5&timeout=0";
  client.print(String("GET ") + url + " HTTP/1.1\r\nHost: api.telegram.org\r\nConnection: close\r\n\r\n");

  String response;
  unsigned long t0 = millis();
  while ((client.connected() || client.available()) && millis() - t0 < TELEGRAM_POLL_MAX_WAIT_MS) {
    while (client.available()) response += (char)client.read();
    yield();
  }
  client.stop();

  int searchPos = 0;
  while (true) {
    int idIdx = response.indexOf("\"update_id\":", searchPos);
    if (idIdx < 0) break;
    int idStart = idIdx + 12;
    int idEnd = response.indexOf(",", idStart);
    if (idEnd < 0) idEnd = response.indexOf("}", idStart);
    long updateId = response.substring(idStart, idEnd).toInt();
    if (updateId > lastUpdateId) lastUpdateId = updateId;

    int nextIdIdx = response.indexOf("\"update_id\":", idStart);
    int textIdx = response.indexOf("\"text\":\"", idIdx);
    if (textIdx >= 0 && (nextIdIdx < 0 || textIdx < nextIdIdx)) {
      int textStart = textIdx + 8;
      int textEnd = response.indexOf("\"", textStart);
      String text = response.substring(textStart, textEnd);
      if (text.indexOf("/status") >= 0) {
        sendFullStatusReport();
      }
    }
    searchPos = idStart;
  }
}

uint32_t getTotalSent() { return totalSent; }
uint32_t getFailedCount() { return failedCount; }
unsigned long getLastAlertMillis() { return lastAlertMillis; }
uint32_t getLastAlertEpoch() { return lastAlertEpoch; }
unsigned long getLastSuccessMillis() { return lastSuccessMillis; }
unsigned long getLastResponseMs() { return lastResponseMs; }

void resetStats() {
  totalSent = 0;
  failedCount = 0;
  lastAlertMillis = 0;
  lastAlertEpoch = 0;
  lastSuccessMillis = 0;
  lastResponseMs = 0;
  lastUpdateId = 0;
  lastPollMillis = 0;
  Serial.println("[NOTIFY] Send counters cleared (factory reset).");
}

}
