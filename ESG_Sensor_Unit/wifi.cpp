/*
  =====================================================================
  wifi.cpp — WiFi Module (implementation)
  =====================================================================
  See wifi.h for the behavior change rationale (no more auto-fallback
  to Access Point on a failed connection — keeps retrying forever).
  =====================================================================
*/

#include "wifi.h"
#include "config.h"
#include <ESP8266WiFi.h>

namespace WiFiManager {

static Mode currentMode = MODE_CONNECTING;
static unsigned long lastRetryMillis = 0;

// How often to retry the saved network while disconnected. Deliberately
// not too aggressive — this device may sit disconnected for hours during
// a real power cut (running on an inverter while the router itself is
// down), so there's no benefit to hammering WiFi.begin() every few
// seconds; it just wastes power and floods the log.
#define WIFI_RETRY_INTERVAL_MS 30000UL

static void attemptConnect(const Settings &settings) {
  Serial.print("[WIFI] Attempting to connect to saved network: ");
  Serial.println(settings.wifiSSID);

  WiFi.mode(WIFI_STA);
  // Disables the ESP8266's WiFi modem-sleep power-saving mode. With it
  // left at its default, the radio periodically powers its receiver down
  // between a router's DTIM beacons to save power — on some routers
  // (heavy traffic shaping, 2.4/5GHz band-steering, or just a busy
  // network), the ESP can miss enough beacons that it briefly believes
  // the router disconnected, even though nothing is actually wrong. This
  // is the most common real cause of a device flapping "unreachable" for
  // under a minute and then recovering on its own. WIFI_NONE_SLEEP trades
  // a small amount of extra power draw for a meaningfully more stable
  // connection — worth it for a security device that's mains-powered
  // anyway.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.begin(settings.wifiSSID, settings.wifiPassword);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - startAttempt) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    currentMode = MODE_STATION;
    Serial.print("[WIFI] Connected. IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    currentMode = MODE_CONNECTING;
    Serial.println("[WIFI] Not connected yet — will keep retrying in the background (no auto-AP-mode).");
  }
  lastRetryMillis = millis();
}

void begin(const Settings &settings) {
  bool hasSavedCreds = (strlen(settings.wifiSSID) > 0);

  if (!hasSavedCreds) {
    // Truly nothing to retry with — this is the legitimate "brand new
    // device" case, so Access Point mode here is correct and expected.
    Serial.println("[WIFI] No saved SSID — going straight to Access Point mode.");
    forceAccessPoint();
    return;
  }

  attemptConnect(settings);
}

void retry(const Settings &settings) {
  if (currentMode == MODE_ACCESS_POINT) return; // manual override — never auto-leave this

  if (currentMode == MODE_STATION && WiFi.status() != WL_CONNECTED) {
    // Was connected, just dropped — fall through to retry logic below
    // rather than waiting a full retry interval to notice.
    currentMode = MODE_CONNECTING;
    Serial.println("[WIFI] Connection dropped — will retry.");
  }

  if (currentMode == MODE_STATION) return; // still connected, nothing to do

  if (strlen(settings.wifiSSID) == 0) return; // nothing to retry with

  if (millis() - lastRetryMillis < WIFI_RETRY_INTERVAL_MS) return;

  Serial.println("[WIFI] Retrying connection...");
  attemptConnect(settings);
}

void forceAccessPoint() {
  Serial.println("[WIFI] Manual override — starting Access Point mode.");
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);

  currentMode = MODE_ACCESS_POINT;

  if (apOk) {
    Serial.print("[WIFI] Access Point started. SSID: ");
    Serial.print(WIFI_AP_SSID);
    Serial.print("  Password: ");
    Serial.println(WIFI_AP_PASSWORD);
    Serial.print("[WIFI] Connect to it, then browse to: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("[WIFI] ERROR: Failed to start Access Point. Device is unreachable over WiFi.");
  }
}

bool isConnected() {
  return (currentMode == MODE_STATION) && (WiFi.status() == WL_CONNECTED);
}

Mode getMode() {
  return currentMode;
}

String getIPAddress() {
  if (currentMode == MODE_STATION) {
    return WiFi.localIP().toString();
  } else if (currentMode == MODE_ACCESS_POINT) {
    return WiFi.softAPIP().toString();
  }
  return ""; // MODE_CONNECTING — nothing to show yet
}

int getRSSI() {
  if (currentMode == MODE_STATION) {
    return WiFi.RSSI();
  }
  return 0;
}

String getMacAddress() { return WiFi.macAddress(); }
int getChannel() { return WiFi.channel(); }

} // namespace WiFiManager
