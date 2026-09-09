/*
  =====================================================================
  display.h — OLED Display Module (public interface)
  =====================================================================
  DESIGN PRINCIPLE:
  main.ino and other modules should NEVER call Adafruit_SSD1306 functions
  directly. They only call these Display:: functions. This means if we
  ever swap the OLED library or hardware, only display.cpp changes —
  nothing else in the project needs to know.

  SCOPE:
  Only boot screen + a generic "status line" function for testing.
  Main screen (distance, WiFi status, etc.) comes in a later step once
  we have real sensor/WiFi data to show.
  =====================================================================
*/

#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>

namespace Display {

  // Initializes I2C + SSD1306. Returns false if the display isn't found
  // at the expected I2C address (wiring/address problem).
  bool begin();

  // Shows the boot screen: firmware name, version, device name/ID, and
  // a short loading animation. Blocking call — takes ~2 seconds.
  // deviceName/deviceId default to "Unnamed Device"/"Unnamed ID" until
  // configured from the dashboard (Danger Zone) — lets you tell multiple
  // devices apart even before they're named.
  void showBootScreen(const String &deviceName = "Unnamed Device", const String &deviceId = "Unnamed ID");

  // Animated WiFi-status boot screen (typewriter text + IP + progress
  // bar), held for a total of ~7 seconds. Replaces the old two separate
  // "Dashboard Ready" / "WiFi Connected" screens with one combined one.
  void showWifiBootScreen(bool apMode, const String &ip);

  // Shown WHILE WiFiManager::begin() is attempting to connect (can take
  // up to WIFI_CONNECT_TIMEOUT_MS) — same visual "spot"/style as the
  // boot screen that follows it, just with connecting-in-progress text,
  // so it reads as one continuous screen rather than a generic message
  // that then gets replaced by a differently-styled one.
  void showConnectingScreen();

  // Persistent instructions shown on the OLED for as long as the device
  // stays in Access Point (first-time setup) mode — tells the person
  // standing at the device exactly what to do: join this WiFi network,
  // then open this address in a browser. Safe to call every loop; it's
  // static content so redrawing it is cheap.
  void showApInstructions(const String &ssid, const String &ip);

  // Main "live reading" screen shown every loop — distance, armed
  // state, alarm state, WiFi status, buzzer connectivity, and trigger
  // distance with an "IN TRIGGER ZONE" indicator. Fast/non-blocking.
  void showReadingScreen(bool armed, bool alarmActive, float distanceCm,
                         bool distanceValid, bool wifiConnected, bool buzzerReachable,
                         const String &deviceName, float triggerDistanceCm);

  // Simple one-line message screen, used for testing/debugging this
  // module before the real "main screen" layout exists.
  void showMessage(const String &line1, const String &line2 = "");

  // Turns the physical panel on/off (blank screen, e.g. to hide light
  // from view). Does not affect what gets drawn — just visibility.
  // If the panel was never successfully initialized, turning it "on"
  // first retries begin() — lets a manual OLED toggle from the dashboard
  // recover a panel that failed at boot (loose wire reseated, etc.)
  // without needing a full device restart.
  void setPower(bool on);

  // True once oled.begin() has succeeded (at boot or via a later retry).
  // false means every draw call is a safe no-op — nothing is wrong, the
  // panel just isn't there yet.
  bool isAvailable();

  // Call once per loop() iteration. Every PERIODIC_REINIT_INTERVAL_MS it
  // silently re-runs the full init sequence (contrast/segment-remap/etc
  // registers), even though isAvailable() already reports true.
  //
  // WHY: a marginal I2C connection (a loose/flexing SDA, SCL, or GND
  // wire) can corrupt the SSD1306's internal display registers WITHOUT
  // ever failing an I2C transaction outright — the classic symptom is
  // the whole panel turning into one uniform glow (often blue, on blue
  // OLEDs) instead of showing content, and physically pressing/wiggling
  // the wire "fixes" it because it restores a clean electrical contact.
  // The real fix is always the wiring (resolder the header, use a
  // shorter/thicker jumper, hot-glue the connector for strain relief,
  // confirm 4.7k pull-ups on SDA/SCL). This periodic re-init is just a
  // software safety net that recovers automatically from that corrupted
  // state within a few minutes even if the wiring is still marginal.
  void periodicMaintenance();

}

#endif // DISPLAY_H
