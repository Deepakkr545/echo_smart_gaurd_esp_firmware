/*
  alarm.cpp — controls up to MAX_BUZZER_UNITS remote buzzer ESP8266
  devices over WiFi (HTTP). Every trigger/test/stop/pulse command is
  sent to ALL configured, non-empty buzzer IP slots (settings.buzzerIps).
*/

#include "alarm.h"
#include "config.h"
#include "notify.h"
#include "wifi.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <time.h>

namespace Alarm {

static bool active = false;
static unsigned long alarmStartMillis = 0;
static unsigned long alarmDurationMs = 0;
static uint32_t triggerCount = 0;
static unsigned long lastTriggerMillis = 0;

static bool slotReachable[MAX_BUZZER_UNITS] = {true, true, true, true, true};
static unsigned long lastHeartbeatMillis = 0;

// Connect/disconnect history per buzzer slot — last 2 sessions, plus
// when the CURRENT session started (0 = not currently connected, or
// time not synced yet). RAM-only (resets on reboot, like other stats).
struct ConnEvent { uint32_t connectEpoch; uint32_t disconnectEpoch; };
static ConnEvent connHistory[MAX_BUZZER_UNITS][2];
static int connHistCount[MAX_BUZZER_UNITS] = {0, 0, 0, 0, 0};
static uint32_t currentConnectEpoch[MAX_BUZZER_UNITS] = {0, 0, 0, 0, 0};

static void recordConnStateChange(int i, bool nowOk) {
  time_t t = time(nullptr);
  bool synced = t > 100000;

  if (nowOk && !slotReachable[i]) {
    if (synced) currentConnectEpoch[i] = (uint32_t)t;
  } else if (!nowOk && slotReachable[i]) {
    if (synced && currentConnectEpoch[i] > 0) {
      int idx = connHistCount[i] % 2;
      connHistory[i][idx].connectEpoch = currentConnectEpoch[i];
      connHistory[i][idx].disconnectEpoch = (uint32_t)t;
      connHistCount[i]++;
    }
    currentConnectEpoch[i] = 0;
  }
}

static bool sendRemote(const String &ip, const String &path, unsigned long timeoutMs = 1500) {
  if (ip.length() == 0) return false;
  WiFiClient client;
  HTTPClient http;
  client.setTimeout(timeoutMs);
  http.setTimeout(timeoutMs);
  String url = "http://" + ip + path;
  bool ok = false;
  if (http.begin(client, url)) {
    int code = http.GET(); yield();
    ok = (code > 0);
    if (!ok) {
      Serial.print("[ALARM] Buzzer unreachable: ");
      Serial.println(ip);
    }
    http.end();
  } else {
    Serial.println("[ALARM] Failed to start request to remote buzzer.");
  }
  return ok;
}

// Sends the given path+query to every configured, non-empty buzzer slot.
static void broadcastToBuzzers(const Settings &settings, const String &pathAndQuery) {
  for (int i = 0; i < MAX_BUZZER_UNITS; i++) {
    if (strlen(settings.buzzerIps[i]) > 0) {
      sendRemote(settings.buzzerIps[i], pathAndQuery);
    }
  }
}

// Extra "&sensorName=X&sensorId=Y" suffix so a buzzer that hasn't received
// an /announce yet (e.g. right after its own reboot) still knows WHICH
// sensor triggered it, immediately, without waiting for the next
// heartbeat/announce cycle.
static String identitySuffix(const Settings &settings) {
  String name = String(settings.deviceName); name.replace(" ", "%20");
  String id = String(settings.deviceId); id.replace(" ", "%20");
  return "&sensorName=" + name + "&sensorId=" + id;
}

void begin() {
  Serial.println("[ALARM] Multi-buzzer client ready (up to 5 units, set via dashboard Danger Zone).");
}

void update() {
  // Local timer only tracks dashboard "active" state; each remote device
  // runs its own independent timer for the actual buzzer duration.
  if (!active) return;
  if (millis() - alarmStartMillis >= alarmDurationMs) {
    active = false;
  }
}

void trigger(const Settings &settings) {
  triggerCount++;
  lastTriggerMillis = millis();
  Serial.print("[ALARM] INTRUDER EVENT #");
  Serial.println(triggerCount);

  // NOTE: settings.alarmEnabled ("Pause Telegram Alerts") is Telegram-only
  // now — the buzzer is NOT gated by it. Buzzer muting/pausing is the
  // buzzer unit's own responsibility (its own dashboard controls).
  if (!settings.buzzerMasterEnabled) {
    Serial.println("[ALARM] Buzzer hardware muted — event logged, buzzers not triggered.");
    return;
  }

  uint16_t durationSec = (settings.alarmDurationSec > 0) ? settings.alarmDurationSec : 5;
  alarmDurationMs = (unsigned long)durationSec * 1000UL;
  active = true;
  alarmStartMillis = millis();

  broadcastToBuzzers(settings, "/trigger?duration=" + String(alarmDurationMs) +
                      "&pattern=" + String(settings.shortTermBuzzerPattern) +
                      identitySuffix(settings));
  Serial.print("[ALARM] Buzzers triggered for ");
  Serial.print(durationSec);
  Serial.println("s.");
}

void triggerSustained(const Settings &settings) {
  Serial.println("[ALARM] SUSTAINED / STRONG ACTIVITY DETECTED.");

  if (!settings.buzzerMasterEnabled) {
    Serial.println("[ALARM] Buzzer hardware muted — sustained event logged, buzzers not triggered.");
    return;
  }

  uint16_t durationSec = (settings.longTermBuzzerDurationSec > 0) ? settings.longTermBuzzerDurationSec : 10;
  alarmDurationMs = (unsigned long)durationSec * 1000UL;
  active = true;
  alarmStartMillis = millis();

  broadcastToBuzzers(settings, "/pulse?duration=" + String(alarmDurationMs) +
                     "&pattern=" + String(settings.longTermBuzzerPattern) +
                     identitySuffix(settings));
  Serial.print("[ALARM] Buzzers pulsed for ");
  Serial.print(durationSec);
  Serial.println("s.");
}

void testBuzzer(const Settings &settings) {
  if (!settings.buzzerMasterEnabled) {
    Serial.println("[ALARM] Buzzer hardware muted — test skipped.");
    return;
  }
  Serial.println("[ALARM] Sending test beep to all buzzers.");
  broadcastToBuzzers(settings, "/test?duration=" + String(TEST_BUZZER_DURATION_MS) + identitySuffix(settings));
}

void emergencyStop(const Settings &settings) {
  active = false;
  broadcastToBuzzers(settings, "/stop");
  Serial.println("[ALARM] EMERGENCY STOP sent to all buzzers.");
}

bool isActive() {
  return active;
}

uint32_t getTriggerCount() {
  return triggerCount;
}

unsigned long getLastTriggerMillis() {
  return lastTriggerMillis;
}

static String buildAnnouncePath(const Settings &settings) {
  String name = String(settings.deviceName); name.replace(" ", "%20");
  String id = String(settings.deviceId); id.replace(" ", "%20");
  String ownIp = WiFiManager::getIPAddress();
  return "/announce?name=" + name + "&id=" + id + "&ip=" + ownIp +
    "&shortPattern=" + String(settings.shortTermBuzzerPattern) +
    "&shortSec=" + String(settings.alarmDurationSec) +
    "&longPattern=" + String(settings.longTermBuzzerPattern) +
    "&longSec=" + String(settings.longTermBuzzerDurationSec) +
    "&thresholdSec=" + String(settings.sustainedThresholdSec);
}

// Call once right after WiFi connects — pushes this sensor's identity to
// every configured buzzer immediately, instead of waiting up to
// BUZZER_HEARTBEAT_INTERVAL_MS (60s) for the first automatic announce.
void announceNow(const Settings &settings) {
  String path = buildAnnouncePath(settings);
  for (int i = 0; i < MAX_BUZZER_UNITS; i++) {
    if (strlen(settings.buzzerIps[i]) > 0) {
      sendRemote(settings.buzzerIps[i], path);
    }
  }
  Serial.println("[ALARM] Announced identity to configured buzzers.");
}

void checkHeartbeat(const Settings &settings) {
  unsigned long now = millis();
  if (now - lastHeartbeatMillis < BUZZER_HEARTBEAT_INTERVAL_MS) return;
  lastHeartbeatMillis = now;

  // Round-robin: check only ONE buzzer slot per cycle (not all 5 in a
  // row). Worst case blocking is now bounded to a single timeout
  // (~2s) no matter how many buzzers are configured — previously all
  // configured buzzers were checked back-to-back, so 5 unreachable
  // buzzers could block the dashboard for up to ~10s. Each buzzer is
  // now re-checked roughly once every (MAX_BUZZER_UNITS × 60s) instead
  // of every 60s — an acceptable tradeoff for a reachability check.
  static int nextSlot = 0;
  String announcePath = buildAnnouncePath(settings);

  for (int attempts = 0; attempts < MAX_BUZZER_UNITS; attempts++) {
    int i = nextSlot;
    nextSlot = (nextSlot + 1) % MAX_BUZZER_UNITS;
    if (strlen(settings.buzzerIps[i]) == 0) continue; // skip empty slots, try next

    bool ok = sendRemote(settings.buzzerIps[i], announcePath);

    // RAPID-RETRY BURST — a single failed attempt is very often just a
    // transient WiFi hiccup (one dropped packet, a brief radio-busy
    // moment) rather than the buzzer genuinely being down. This is
    // exactly what was causing "Unreachable" followed roughly a minute
    // later by "Back Online" for something that was never a real
    // outage — the old code trusted one failed attempt immediately and
    // then waited a full 60s before ever checking again.
    //
    // Before believing it, hammer the buzzer with quick, short-timeout
    // attempts for a few seconds. A genuine outage (power cut, dead
    // WiFi, crashed device) fails all of these too, so real problems
    // are still caught just as reliably — only now the "Unreachable"
    // alert is held back until we're actually confident it's real, and
    // a merely-blipped connection recovers silently, often within
    // well under a second, instead of round-tripping through a full
    // false-alarm Telegram message pair.
    if (!ok) {
      for (int retry = 0; retry < 10 && !ok; retry++) {
        delay(150);
        ok = sendRemote(settings.buzzerIps[i], announcePath, 250);
      }
    }

    if (!ok && slotReachable[i]) {
      Notify::sendTextMessage("⚠️ Buzzer Unit Unreachable\n\nCould not reach buzzer at " +
                               String(settings.buzzerIps[i]) + ". Check its power/WiFi connection.");
    } else if (ok && !slotReachable[i]) {
      Notify::sendTextMessage("✅ Buzzer Unit Back Online\n\nConnection to " +
                               String(settings.buzzerIps[i]) + " restored.");
    }
    recordConnStateChange(i, ok);
    slotReachable[i] = ok;
    break; // Checked one real slot this cycle — done until next call.
  }
}

// True if AT LEAST ONE configured buzzer is currently reachable — used
// for a simple overall "connected" indicator on the dashboard.
bool isBuzzerReachable() {
  for (int i = 0; i < MAX_BUZZER_UNITS; i++) {
    if (slotReachable[i]) return true;
  }
  return false;
}

bool isBuzzerSlotReachable(int index) {
  if (index < 0 || index >= MAX_BUZZER_UNITS) return false;
  return slotReachable[index];
}

// 0 if not currently connected (or time not synced when it connected).
uint32_t getBuzzerConnectedSince(int index) {
  if (index < 0 || index >= MAX_BUZZER_UNITS) return 0;
  return currentConnectEpoch[index];
}

// n=0 → most recent past session, n=1 → the one before that.
// Returns {0,0} if that many sessions haven't happened yet.
void getBuzzerHistory(int index, int n, uint32_t &connectEpoch, uint32_t &disconnectEpoch) {
  connectEpoch = 0; disconnectEpoch = 0;
  if (index < 0 || index >= MAX_BUZZER_UNITS) return;
  if (n < 0 || n >= 2) return;
  int count = connHistCount[index];
  if (n >= count) return; // not enough history yet
  int idx = ((count - 1 - n) % 2 + 2) % 2;
  connectEpoch = connHistory[index][idx].connectEpoch;
  disconnectEpoch = connHistory[index][idx].disconnectEpoch;
}

void resetAll() {
  active = false;
  alarmStartMillis = 0;
  alarmDurationMs = 0;
  triggerCount = 0;
  lastTriggerMillis = 0;
  lastHeartbeatMillis = 0;
  for (int i = 0; i < MAX_BUZZER_UNITS; i++) {
    slotReachable[i] = true; // unknown again until the next heartbeat check
    connHistCount[i] = 0;
    currentConnectEpoch[i] = 0;
    connHistory[i][0] = ConnEvent{0, 0};
    connHistory[i][1] = ConnEvent{0, 0};
  }
  Serial.println("[ALARM] All RAM-only counters/history cleared (factory reset).");
}

} // namespace Alarm
