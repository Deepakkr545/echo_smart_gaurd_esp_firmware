/*
  =====================================================================
  ESG_Sensor_Unit.ino
  =====================================================================
  Web Dashboard (Core Controls + Live Status)

*/

#include "config.h"
#include "display.h"
#include "sensor.h"
#include "storage.h"
#include "wifi.h"
#include "calibration.h"
#include "alarm.h"
#include "webserver.h"
#include "notify.h"
#include "stats.h"
#include <time.h>
#include <EEPROM.h>

Settings settings; // Global — loaded once in setup(), used/updated across loop()
bool displayAvailable = false; // Set in setup(), used by the calibration progress callback
bool armed = true;             // Initialized from settings.armed in setup(); persisted on change
bool armedByNightMode = false; // true ONLY if Night Mode itself set armed=true — lets us tell
bool nightModeFailsafeGuess = false; // true when armedByNightMode was set by the boot-time failsafe below, not a genuine night-arm — see its self-correction further down
                                // apart "user manually armed" from "Night Mode auto-armed", so
                                // Night Mode only ever undoes its OWN action, never a manual one.
int consecutiveInZone = 0; // Requires INTRUSION_CONFIRM_READINGS in a row before firing (false-positive fix)
unsigned long entryGraceUntilMillis = 0; // 0 = no pending entry-delay countdown; disarming while this is set cancels the pending alarm
unsigned long exitGraceUntilMillis = 0; // 0 = not in an exit-delay window; armed=true already but intrusion checks are held off until this passes
bool exitGraceConfirmPending = false; // true = still owe the "System Armed" confirmation once the exit window actually closes
unsigned long zoneStartMillis = 0; // When continuous in-zone presence began (0 = not currently in zone)
bool sustainedFired = false;       // Ensures the sustained alert fires only once per continuous presence
float sustainedMin = 99999, sustainedMax = -1; // Track reading variance during the current window (informational only)
bool frozenAlertSent = false; // Edge-trigger flag for the sensor-frozen Telegram warning (runs 24/7, independent of arm state)
unsigned long tamperStartMillis = 0; // 0 = not currently in a close-reading streak
bool tamperAlertSent = false; // edge-trigger flag for the tamper Telegram warning
float lastDistanceCm = -1.0;   // Most recent sensor reading, shared with the dashboard via a pointer
bool emergencyStopActive = false;
unsigned long emergencyStopStartMillis = 0;
unsigned long muteStartMillis = 0;
unsigned long identifyUntilMillis = 0; // set by webserver.cpp's /identify handler; 0 = not identifying
bool sessionMarked = false;             // True once this session's start time is recorded (needs NTP)
unsigned long lastAliveSaveMillis = 0;  // Throttles periodic diagnostics EEPROM saves

// ---------------------------------------------------------------------
// Rapid power-cycle detection — lets you manually force Access Point
// (WiFi setup) mode with NO button and NO USB: unplug/replug power 3
// times quickly. This does NOT use RTC memory (RTC memory on ESP8266
// is powered by the same 3.3V rail as everything else, so it does NOT
// survive a genuine power loss — only resets/deep-sleep while VCC stays
// up). A dedicated flash address (well past the main Settings struct,
// so it can never collide with Storage::'s own region) is used instead,
// since flash is the only thing that survives a real unplug.
//
// Logic: every boot increments a counter here. If the device stays
// powered continuously for BOOTCYCLE_STABLE_MS (10s) without another
// reboot, the counter resets to 0 — a normal boot (or even an occasional
// power blip) will always clear it long before it could reach the
// trigger count. Only a deliberate rapid unplug-replug sequence racks
// the counter up to 3 before any single boot gets the chance to run
// long enough to reset it.
// ---------------------------------------------------------------------
#define BOOTCYCLE_EEPROM_ADDR 900  // Comfortably past any Settings struct size
#define BOOTCYCLE_EEPROM_SIZE 950  // Must cover BOOTCYCLE_EEPROM_ADDR + sizeof(BootCycleData)
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
    data.count = 0; // consume it immediately so it can't double-trigger
    Serial.println("[BOOTCYCLE] 3 rapid power-cycles detected — will force Access Point (WiFi setup) mode.");
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

void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  delay(200); // Give the serial monitor time to attach after upload

  checkRapidBootCycle(); // Must run before anything else touches EEPROM

  Serial.println();
  Serial.println("=====================================");
  Serial.print(FIRMWARE_NAME);
  Serial.print(" — v");
  Serial.println(FIRMWARE_VERSION);
  Serial.println("Dashboard");
  Serial.println("=====================================");

  // Storage loads FIRST now — the boot screen needs settings.deviceName/
  // deviceId to show real device identity (not just generic firmware info).
  Storage::begin();
  settings = Storage::load();

  Serial.print("[MAIN] Boot count: ");
  Serial.println(settings.bootCount);
  settings.bootCount++;
  Storage::save(settings);

  armed = settings.armed; // Restore across power loss
  Serial.print("[MAIN] Restored ARMED state: ");
  Serial.println(armed ? "ARMED" : "DISARMED");

  // --- Night Mode fail-safe (frequent power-cut protection) ---
  // At this exact point we don't know the real time yet (NTP hasn't
  // synced — that needs WiFi, which isn't connected yet). If the last
  // saved (manual) state was disarmed and Night Mode is enabled, we
  // temporarily arm as a safe default — a security system should fail
  // TOWARD protection, not away from it. This does NOT touch
  // settings.armed in EEPROM, so the real manual preference (disarmed)
  // is preserved. Marking armedByNightMode=true means the existing
  // Night Mode logic in loop() will automatically self-correct this
  // guess once real time is known: stays armed if it's genuinely still
  // night, or auto-disarms within a minute or two if it turns out to
  // already be daytime.
  if (settings.nightMode && !armed) {
    armed = true;
    armedByNightMode = true;
    nightModeFailsafeGuess = true;
    Serial.println("[MAIN] Night Mode fail-safe: temporarily armed until time syncs (was disarmed before restart).");
  }

  // --- Diagnostics: record the PREVIOUS session's on/off history ---
  // (this boot's own start time gets recorded later, once NTP syncs)
  if (settings.sessionStartEpoch > 0 && settings.lastAliveEpoch >= settings.sessionStartEpoch) {
    int idx = settings.historyCount % 5;
    settings.historyStart[idx] = settings.sessionStartEpoch;
    settings.historyEnd[idx] = settings.lastAliveEpoch;
    settings.historyCount++;
    Storage::save(settings);
    Serial.println("[MAIN] Recorded previous session into diagnostics history.");
  }
  settings.sessionStartEpoch = 0; // Will be set once NTP syncs this session

  // Initialize display. If it fails, we keep running (Serial still
  // works) — a missing display should never brick the whole system.
  // This "degrade gracefully" pattern will repeat for every module:
  // a failure in one module should never silently freeze the others.
  bool displayOk = Display::begin();
  displayAvailable = displayOk;
  if (displayOk) {
    Display::setSkin(settings.displaySkin); // Apply the saved reading-screen skin before anything gets drawn
    Display::showBootScreen(settings.deviceName, settings.deviceId);
    Display::showMessage("System Ready", "Loading settings...");
  } else {
    Serial.println("[MAIN] Continuing without display.");
  }

  Sensor::begin();
  Alarm::begin();
  Notify::begin(&settings, &armed, &lastDistanceCm,
                &emergencyStopActive, &emergencyStopStartMillis, &muteStartMillis);
  Stats::begin();
  Serial.println("[MAIN] System is ARMED by default.");

  if (displayOk) {
    Display::showConnectingScreen(); // Same visual style as the boot screen that follows it
  }

  if (forceApModeRequested) {
    WiFiManager::forceAccessPoint();
  } else {
    WiFiManager::begin(settings);
  }
  if (WiFiManager::isConnected()) {
    Stats::recordWifiConnected();
    Alarm::announceNow(settings); // Push identity to buzzers immediately (was: wait up to 60s)
  }

  DashboardServer::begin(&settings, &armed, &armedByNightMode, &lastDistanceCm,
                          &emergencyStopActive, &emergencyStopStartMillis, &muteStartMillis);
  String dashboardUrl = "http://" + WiFiManager::getIPAddress();
  Serial.print("[MAIN] Dashboard available at: ");
  Serial.println(dashboardUrl);
  Notify::sendTextMessage("✅ A Sensor device is back online\n\nDashboard: " + dashboardUrl);

  configTime(NTP_GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.nist.gov");

  if (displayOk) {
    bool apMode = (WiFiManager::getMode() == WiFiManager::MODE_ACCESS_POINT);
    Display::showWifiBootScreen(apMode, WiFiManager::getIPAddress()); // ~7s total, animated
    Display::setPower(settings.oledOn); // Restore OLED on/off across power loss
  }

  Serial.println("[MAIN] Commands: CALIBRATE | SETWIFI:ssid,password | ARM | DISARM | TEST | STOP | RESETLOGIN | APMODE");

  // --- Calibration on first boot: intentionally NOT automatic ---
  // Earlier versions ran a blocking 10s countdown + auto-calibration here
  // the very first time no wall distance was on file. That's been removed
  // on purpose — calibration is now always a deliberate action taken from
  // the dashboard (Settings > Security > Calibrate now), so it only runs
  // when the person is actually ready and the area is actually clear,
  // instead of racing a fixed countdown right after a fresh flash/reset.
  if (settings.wallDistanceCm <= 0) {
    Serial.println("[MAIN] Not calibrated yet. Open the dashboard and use Settings > Security > Calibrate now when ready.");
  } else {
    Serial.print("[MAIN] Existing calibration found. Wall distance: ");
    Serial.print(settings.wallDistanceCm, 1);
    Serial.println(" cm.");
  }
}

// Called by Calibration::run() after each sample, so calibration itself
// doesn't need to know anything about the OLED.
void onCalibrationProgress(int percent) {
  if (!displayAvailable) return;
  Display::showMessage("Calibrating...", String(percent) + "% (keep stairs empty)");
}

// -------------------------------------------------------------------
// TEMPORARY TEST TOOL (Step 5/6 only) — parses Serial commands. Both
// SETWIFI and CALIBRATE are replaced by real dashboard controls once
// the web server exists; they exist only so we can test each module
// in isolation before that.
// -------------------------------------------------------------------
void checkSerialCommands() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();

  if (line.length() == 0) {
    return;
  }

  if (line == "APMODE") {
    Serial.println("[MAIN] Manual command — switching to Access Point (WiFi setup) mode now.");
    WiFiManager::forceAccessPoint();
    return;
  }

  if (line == "CALIBRATE") {
    bool ok = Calibration::run(settings, onCalibrationProgress);
    if (displayAvailable) {
      if (ok) {
        Display::showMessage("Calibration OK", String(settings.wallDistanceCm, 1) + " cm wall");
      } else {
        Display::showMessage("Calibration", "FAILED - check sensor");
      }
      delay(2000);
    }
    return;
  }

  if (line == "ARM") {
    armed = true;
    armedByNightMode = false; // manual action — Night Mode should never auto-disarm this
    settings.armed = true;
    Storage::save(settings);
    Serial.println("[MAIN] System ARMED.");
    return;
  }

  if (line == "DISARM") {
    armed = false;
    armedByNightMode = false;
    settings.armed = false;
    Storage::save(settings);
    Serial.println("[MAIN] System DISARMED.");
    return;
  }

  if (line == "TEST") {
    Alarm::testBuzzer(settings);
    return;
  }

  if (line == "STOP") {
    Alarm::emergencyStop(settings);
    return;
  }

  if (line == "RESETLOGIN") {
    strncpy(settings.dashboardUsername, DEFAULT_DASHBOARD_USERNAME, sizeof(settings.dashboardUsername)-1);
    strncpy(settings.dashboardPassword, DEFAULT_DASHBOARD_PASSWORD, sizeof(settings.dashboardPassword)-1);
    Storage::save(settings);
    Serial.println("[MAIN] Dashboard login reset to factory default (see config.h DEFAULT_DASHBOARD_USERNAME/PASSWORD).");
    return;
  }

  if (line.startsWith("SETWIFI:")) {
    String payload = line.substring(String("SETWIFI:").length());
    int commaIndex = payload.indexOf(',');
    if (commaIndex < 0) {
      Serial.println("[MAIN] Invalid format. Use: SETWIFI:ssid,password");
      return;
    }

    String ssid = payload.substring(0, commaIndex);
    String password = payload.substring(commaIndex + 1);

    if (ssid.length() == 0 || ssid.length() >= sizeof(settings.wifiSSID) ||
        password.length() >= sizeof(settings.wifiPassword)) {
      Serial.println("[MAIN] SSID/password empty or too long.");
      return;
    }

    ssid.toCharArray(settings.wifiSSID, sizeof(settings.wifiSSID));
    password.toCharArray(settings.wifiPassword, sizeof(settings.wifiPassword));
    Storage::save(settings);

    // Keep diagnostics accurate even for this quick restart.
    time_t nowT = time(nullptr);
    if (nowT > 100000 && settings.sessionStartEpoch > 0) {
      settings.lastAliveEpoch = (uint32_t)nowT;
      Storage::save(settings);
    }

    Serial.print("[MAIN] Saved new WiFi credentials for SSID: ");
    Serial.println(ssid);
    Serial.println("[MAIN] Restarting to apply...");
    delay(500);
    ESP.restart();
    return;
  }

  Serial.println("[MAIN] Unknown command. Use: CALIBRATE | SETWIFI:ssid,password | ARM | DISARM | TEST | STOP");
}

// While the device is in Access Point (first-time setup) mode, the OLED
// shows persistent "join this WiFi, then open this address" instructions
// instead of the normal live-reading screen — there's no point showing a
// sensor dashboard summary before the device is even reachable on the
// person's own network. Once it's in Station mode (connected to a real
// network), it shows the normal reading screen as before.
void updateDisplayScreen(bool armedState, bool alarmActive, float distanceCm, bool distanceValid) {
  if (identifyUntilMillis > 0) {
    if (millis() < identifyUntilMillis) {
      Display::setPower(true); // force the panel on even if the user had it set to off, every loop iteration is cheap/idempotent to call
      // Blink every 300ms — alternating a bold message with a blank
      // screen reads as an obvious "look at me" flash, not just a
      // static screen you might mistake for normal content.
      if ((millis() / 300) % 2 == 0) {
        Display::showMessage("IDENTIFY", settings.deviceName);
      } else {
        Display::showMessage("", "");
      }
      return;
    }
    identifyUntilMillis = 0; // flash window over, resume normal display
    Display::setPower(settings.oledOn); // restore whatever the saved on/off preference actually was
  }
  if (WiFiManager::getMode() == WiFiManager::MODE_ACCESS_POINT) {
    Display::showApInstructions(WIFI_AP_SSID, WiFiManager::getIPAddress());
    return;
  }
  Display::showReadingScreen(armedState, alarmActive, distanceCm, distanceValid,
                              WiFiManager::isConnected(), Alarm::isBuzzerReachable(),
                              settings.deviceName, settings.triggerDistanceCm);
}

void loop() {
  WiFiManager::retry(settings);       // Keeps trying the saved network forever if disconnected — never auto-AP
  clearBootCycleCounterIfStable();    // Clears the rapid-power-cycle counter once we've been up 10s

  // --- OLED self-heal ---
  // If the display failed to initialize at boot (most commonly: its
  // power rail hadn't settled yet, or a wire was momentarily loose), keep
  // retrying every 10s instead of requiring a full device restart to ever
  // recover it. Cheap: Display::begin() itself is fast when it fails.
  static unsigned long lastDisplayRetryMillis = 0;
  if (!displayAvailable && millis() - lastDisplayRetryMillis > 10000UL) {
    lastDisplayRetryMillis = millis();
    if (Display::begin()) {
      displayAvailable = true;
      Display::setPower(settings.oledOn); // Respect the saved on/off preference
      Serial.println("[MAIN] Display recovered — now initialized.");
    }
  }
  // Guards against a marginal I2C wire slowly corrupting the panel's
  // internal registers (symptom: whole screen turns into a uniform glow
  // instead of showing content) — see display.h for the full explanation.
  Display::periodicMaintenance();

  // Dashboard requests get top priority — processed BEFORE any periodic
  // network checks (Telegram poll, buzzer heartbeat), which can each
  // block for a couple seconds. Previously those ran first, so a button
  // click landing during one of those checks would sit queued for
  // seconds before being handled — this is what caused the inconsistent
  // "sometimes fast, sometimes slow" button response.
  DashboardServer::handleClient();

  Notify::checkIncomingCommands();

  // --- Diagnostics: mark this session's start once NTP time is known,
  // and periodically save "last alive" so next boot can show an
  // approximate shutdown time + on/off history. ---
  static unsigned long lastNtpWarnMillis = 0;
  if (!sessionMarked) {
    time_t t = time(nullptr);
    if (t > 100000) { // NTP has synced
      settings.sessionStartEpoch = t;
      settings.lastAliveEpoch = t;
      Storage::save(settings);
      sessionMarked = true;
      Serial.println("[MAIN] NTP time synced — diagnostics now active.");
    } else if (millis() - lastNtpWarnMillis > 30000) {
      lastNtpWarnMillis = millis();
      Serial.println("[MAIN] NTP not synced yet — Diagnostics (session times) will stay blank until it syncs. Check internet/DNS if this persists.");
    }
  } else if (millis() - lastAliveSaveMillis > ALIVE_SAVE_INTERVAL_MS) {
    lastAliveSaveMillis = millis();
    time_t t = time(nullptr);
    if (t > 100000) {
      settings.lastAliveEpoch = t;
      Storage::save(settings);
    }
  }

  // --- Emergency Stop auto-resume ---
  if (emergencyStopActive) {
    unsigned long elapsedSec = (millis() - emergencyStopStartMillis) / 1000UL;
    if (elapsedSec >= (unsigned long)EMERGENCY_STOP_DURATION_MIN * 60UL) {
      emergencyStopActive = false;
      Notify::sendTextMessage("✅ Emergency Stop Expired\n\nMonitoring auto-resumed at " + Notify::currentTimeString());
      Serial.println("[MAIN] Emergency Stop auto-expired.");
    }
  }

  // --- Night Mode ---
  // Auto-arms at night start ONLY if not already armed. Auto-disarms at
  // night end ONLY if Night Mode itself did the arming — a manual arm
  // (before or during the night window) is never touched by Night Mode,
  // so the user's own decision always takes priority.
  if (settings.nightMode) {
    time_t now = time(nullptr);
    if (now > 100000) { // time() only meaningful once NTP has synced
      int hr = localtime(&now)->tm_hour;
      bool isNight = (hr >= settings.nightStartHour || hr < settings.nightEndHour);
      if (isNight && !armed) {
        armed = true;
        armedByNightMode = true;
        Serial.println("[MAIN] Night Mode: auto-arming.");
        Notify::sendTextMessage("🌙 Night Mode Started\n\nThe system has armed itself for the night.");
      } else if (isNight && armedByNightMode && nightModeFailsafeGuess) {
        // The boot-time guess turned out to be correct — it really is
        // night. From here on this is a confirmed, genuine night-arm,
        // so the eventual dawn transition should notify normally.
        nightModeFailsafeGuess = false;
      } else if (!isNight && armedByNightMode) {
        armed = false;
        armedByNightMode = false;
        if (nightModeFailsafeGuess) {
          // This wasn't a genuine night-to-day transition — it's the
          // boot-time failsafe guess (see setup()) correcting itself
          // now that real time is known and it turns out to already be
          // daytime. Nothing to tell the user, the system was never
          // actually night-armed this session.
          nightModeFailsafeGuess = false;
          Serial.println("[MAIN] Night Mode: failsafe guess corrected (already daytime), no notification.");
        } else {
          Serial.println("[MAIN] Night Mode: auto-disarming (was auto-armed by Night Mode).");
          Notify::sendTextMessage("🌙 Night Mode Ended\n\nThe system has disarmed itself for the day.");
        }
      }
    }
  }

  // Temporary test tools — see checkSerialCommands() above.
  checkSerialCommands();

  // MUST run every loop iteration — non-blocking check for whether the
  // current alarm's duration has elapsed (see alarm.h/.cpp).
  Alarm::update();
  Alarm::checkHeartbeat(settings); // Rate-limited internally (every 60s)

  // NOTE: Sensor::readDistanceCM() is blocking (~120-200ms, see sensor.h),
  // so this loop naturally runs a few times per second. This also means
  // the dashboard can feel briefly unresponsive while a reading is in
  // progress — acceptable for now, revisit if it becomes noticeable.
  float distanceCm = Sensor::readDistanceCM();
  lastDistanceCm = distanceCm; // Shared with the dashboard's /status endpoint

  if (distanceCm > 0) {
    Serial.print("[SENSOR] Distance: ");
    Serial.print(distanceCm, 4); // 4 decimals — enough to see real vs frozen variance
    Serial.print(" cm");

    bool calibrated = (settings.triggerDistanceCm > 0);
    bool inZone = calibrated && (distanceCm < settings.triggerDistanceCm);

    if (calibrated) {
      Serial.print(inZone ? "  [WITHIN TRIGGER ZONE]" : "  [beyond trigger zone / at wall]");
    } else {
      Serial.print("  [not calibrated yet — type CALIBRATE]");
    }
    Serial.println();

    // --- Debounced intrusion detection (false-positive fix) ---
    // How many consecutive in-zone readings are required before this
    // counts as a real crossing depends on sensitivityProfile: Normal
    // matches the original fixed 2-reading behavior; Pet-Friendly
    // requires a longer hold (a pet passing through won't sustain in
    // the beam as long as a person walking up to it); High fires on
    // the very first reading.
    int requiredConfirms = (settings.sensitivityProfile == 0) ? 5 :  // Pet-Friendly
                            (settings.sensitivityProfile == 2) ? 1 :  // High
                            INTRUSION_CONFIRM_READINGS;               // Normal (2)
    if (inZone) consecutiveInZone++; else consecutiveInZone = 0;

    if (calibrated && armed && !emergencyStopActive && millis() >= exitGraceUntilMillis &&
        consecutiveInZone == requiredConfirms && entryGraceUntilMillis == 0) {
      if (settings.entryDelaySec > 0) {
        // Hold off actually sounding the alarm — gives a real occupant
        // time to disarm on the way in. If still armed once this
        // window closes, the check further below fires for real.
        entryGraceUntilMillis = millis() + (unsigned long)settings.entryDelaySec * 1000UL;
        Serial.println("[ALARM] Intrusion confirmed — entry delay grace period started.");
      } else {
        Alarm::trigger(settings);
        Stats::recordIntrusion();
        if (settings.alarmEnabled) {
          Notify::sendIntruderAlert(distanceCm, Alarm::getTriggerCount());
          Stats::recordAlertSent();
        }
      }
    }
    // Entry-delay grace period expiring — fire for real if still armed
    // (a disarm during the grace window already reset this to 0 in the
    // disarm handler, so reaching here means nobody disarmed in time).
    if (entryGraceUntilMillis > 0 && millis() >= entryGraceUntilMillis) {
      entryGraceUntilMillis = 0;
      if (armed && !emergencyStopActive) {
        Alarm::trigger(settings);
        Stats::recordIntrusion();
        if (settings.alarmEnabled) {
          Notify::sendIntruderAlert(distanceCm, Alarm::getTriggerCount());
          Stats::recordAlertSent();
        }
      }
    }
    // Exit-delay window closing — the sensor genuinely starts watching
    // as of this moment, so this is when "System Armed" actually means
    // it, not back when the button was first tapped.
    if (exitGraceUntilMillis > 0 && millis() >= exitGraceUntilMillis) {
      exitGraceUntilMillis = 0;
      if (exitGraceConfirmPending) {
        exitGraceConfirmPending = false;
        Notify::sendTextMessage("🛡️ System Armed\n\nThe sensor is now watching.");
      }
    }

    // --- Sustained / strong activity detection ---
    // Tracks continuous in-zone time (separate from the short-term
    // debounce above). If activity continues past sustainedThresholdSec,
    // fires ONE additional high-probability alert with a distinct
    // pulsed buzzer pattern — signals "this looks like a real, ongoing
    // presence" rather than a brief crossing.
    //
    // FAULT DETECTION: a real object/person always produces some tiny
    // reading variation (breathing, micro-movement, sensor noise). If
    // EVERY reading in the window is bit-for-bit identical, that could
    // mean a static object (a box, a wall within range) rather than a
    // moving person — NOT necessarily a sensor fault. A real stationary
    // object legitimately produces near-zero variance, so this is only
    // used as an informational note, never to auto-restart the device
    // (an earlier version did that and caused false-positive restart
    // loops whenever something static sat in the zone — removed).
    if (calibrated && inZone) {
      if (distanceCm < sustainedMin) sustainedMin = distanceCm;
      if (distanceCm > sustainedMax) sustainedMax = distanceCm;

      if (zoneStartMillis == 0) {
        zoneStartMillis = millis();
        sustainedFired = false;
        sustainedMin = distanceCm;
        sustainedMax = distanceCm;
      } else if (!sustainedFired &&
                 (millis() - zoneStartMillis) >= (unsigned long)settings.sustainedThresholdSec * 1000UL) {
        sustainedFired = true;
        bool lowVariance = (sustainedMax - sustainedMin) < 0.05f;

        if (armed && !emergencyStopActive) {
          Alarm::triggerSustained(settings);
          if (settings.alarmEnabled) {
            String note = lowVariance ?
              "\nThe reading stayed very stable, likely a static object rather than movement." : "";
            Notify::sendTextMessage("🆘 Strong / Sustained Activity Detected\n\n"
              "Continuous movement in the protected zone for " +
              String(settings.sustainedThresholdSec) + "+ seconds." + note + "\n"
              "Time: " + Notify::currentTimeString() + "\n\n"
              "This suggests a prolonged presence, please check as soon as you can.", false, true);
          }
        }
      }
    } else {
      zoneStartMillis = 0;
      sustainedFired = false;
      sustainedMin = 99999;
      sustainedMax = -1;
    }

    Stats::recordSensorReading(distanceCm, true);
    Stats::recordState(armed, !settings.alarmEnabled, emergencyStopActive);

    updateDisplayScreen(armed, Alarm::isActive(), distanceCm, true);

  } else {
    Serial.println("[SENSOR] INVALID / TIMEOUT — no reliable reading");
    // Don't reset consecutiveInZone here — a momentary sensor dropout
    // shouldn't be treated as "object left the zone" mid-confirmation.
    Stats::recordSensorReading(0, false);
    Stats::recordState(armed, !settings.alarmEnabled, emergencyStopActive);
    updateDisplayScreen(armed, Alarm::isActive(), 0, false);
  }

  // --- Tamper check (armed only — a close reading while disarmed is
  // just normal foot traffic near the sensor, not suspicious) ---
  // Distinct from the frozen-sensor check below: frozen means the SAME
  // reading forever (any distance); tamper means a very CLOSE reading
  // held for a while, which is what covering the sensor with a hand,
  // tape, or a box looks like.
  if (armed && distanceCm > 0 && distanceCm < TAMPER_DISTANCE_CM) {
    if (tamperStartMillis == 0) tamperStartMillis = millis();
    if (!tamperAlertSent && (millis() - tamperStartMillis) >= (unsigned long)TAMPER_SUSTAINED_SEC * 1000UL) {
      tamperAlertSent = true;
      Notify::sendTextMessage("🚫 Possible Tamper Detected\n\nThe sensor has read an object closer than " +
        String((int)TAMPER_DISTANCE_CM) + "cm for over " + String(TAMPER_SUSTAINED_SEC) +
        " seconds. This can mean something is covering or blocking the sensor.\n\nPlease check it in person.", false, true);
    }
  } else {
    tamperStartMillis = 0;
    tamperAlertSent = false;
  }

  // --- Sensor-frozen health check (runs always, independent of armed/zone) ---
  // Catches a genuinely stuck sensor even at night when nothing should
  // be near it — informational Telegram warning only, no auto-restart.
  if (Stats::isSensorFrozen()) {
    if (!frozenAlertSent) {
      frozenAlertSent = true;
      Notify::sendTextMessage("⚠️ Sensor Health Warning\n\nReading has not changed at all for over " +
        String(SENSOR_FROZEN_THRESHOLD_SEC) + " seconds.\nA real object or environment almost always shows "
        "tiny variation, this may indicate a stuck sensor. Please check the wiring and mounting if this persists.", false, true);
    }
  } else {
    frozenAlertSent = false;
  }
}
