/*
  =====================================================================
  wifi.h — WiFi Module (public interface)
  =====================================================================
  DESIGN PRINCIPLE:
  Named "WiFiManager" (not "WiFi") to avoid clashing with the ESP8266
  core's own global `WiFi` object from <ESP8266WiFi.h>. main.ino calls
  WiFiManager:: functions only — it never calls WiFi.begin()/WiFi.softAP()
  directly, so the connect/fallback/retry logic lives in exactly one
  place.

  UPDATED BEHAVIOR — no more auto-fallback to Access Point:
  Previously, if the saved WiFi network couldn't be reached within
  WIFI_CONNECT_TIMEOUT_MS, the device gave up and started its own
  Access Point automatically. That's wrong for a device that runs on
  an inverter during long power cuts (router can be down for hours) —
  the device would sit in AP mode, invisible to the dashboard, doing
  nothing useful, until someone manually reconfigured it.

  Now: a failed connection attempt just means "not connected yet" —
  the device keeps retrying quietly in the background (see retry())
  for as long as it takes, and Access Point mode is ONLY ever entered
  when explicitly requested via forceAccessPoint() (rapid power-cycle
  detection or a manual Serial/button trigger — see main.ino/sketch).
  =====================================================================
*/

#ifndef WIFI_H
#define WIFI_H

#include <Arduino.h>
#include "storage.h"

namespace WiFiManager {

  enum Mode {
    MODE_CONNECTING,     // No saved network reached yet — retrying in background
    MODE_STATION,        // Connected to a real router (normal operation)
    MODE_ACCESS_POINT    // Running our own hotspot (manually requested)
  };

  // First connection attempt at boot. If settings.wifiSSID is empty
  // (truly brand-new device, nothing to even try), goes straight to
  // Access Point mode — there's nothing to retry in that case. If a
  // saved SSID exists but this first attempt fails, leaves the device
  // in MODE_CONNECTING (not AP) — call retry() every loop() afterward
  // to keep trying indefinitely.
  void begin(const Settings &settings);

  // Call every loop() iteration. If not currently connected (and not
  // manually forced into Access Point mode), retries the saved network
  // every WIFI_RETRY_INTERVAL_MS. Safe/cheap to call constantly — it
  // internally rate-limits itself.
  void retry(const Settings &settings);

  // Manually switch to Access Point mode right now, regardless of
  // current state. Used by rapid power-cycle detection and any manual
  // trigger (Serial command / long button press). Once in this mode,
  // retry() will NOT automatically leave it — only a reboot (with the
  // saved credentials) returns to normal station-mode retrying.
  void forceAccessPoint();

  // True if currently connected to a router in Station Mode.
  bool isConnected();

  // Which mode we're in right now.
  Mode getMode();

  // IP address to reach the web dashboard at — either the router-assigned
  // IP (Station) or the fixed AP IP (Access Point, always 192.168.4.1).
  // Returns an empty string while MODE_CONNECTING (nothing to show yet).
  String getIPAddress();

  // Signal strength in dBm. Only meaningful in Station Mode; returns 0
  // otherwise.
  int getRSSI();

  String getMacAddress();
  int getChannel();

}

#endif // WIFI_H
