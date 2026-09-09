/*
  =====================================================================
  alarm.h — Alarm/Buzzer Module (public interface)
  =====================================================================
  DESIGN PRINCIPLE:
  Same pattern as every other module — main.ino decides WHEN an intruder
  event happened (by comparing sensor distance to the calibrated trigger
  distance), but Alarm:: owns HOW the buzzer actually behaves (timing,
  duration, silent mode, emergency stop). main.ino never calls
  digitalWrite() on the buzzer pin directly.

  SCOPE:
  - trigger(): starts the buzzer for settings.alarmDurationSec seconds
    (auto-stops even if the object is still in range — "Duration mode").
    Respects settings.alarmEnabled (silent/disabled).
  - update(): MUST be called every loop() iteration — non-blocking, just
    checks whether the current alarm's duration has elapsed and turns
    the buzzer off if so. This lets the rest of the firmware (sensor
    polling, WiFi, Serial commands) keep running while an alarm sounds.
  - testBuzzer(): short beep, for confirming wiring/volume.
  - emergencyStop(): immediately silences the buzzer regardless of timer.

  NOTE ON PERSISTENCE: triggerCount/lastTriggerMillis are RAM-only in
  this step (reset on reboot).
  =====================================================================
*/

#ifndef ALARM_H
#define ALARM_H

#include <Arduino.h>
#include "storage.h"

namespace Alarm {

  // Sets up the buzzer pin. Call once in setup().
  void begin();

  // MUST be called every loop() iteration. Non-blocking — turns the
  // buzzer off automatically once the current alarm's duration elapses.
  void update();

  // Call when an intruder event is detected (edge-triggered — call this
  // once per new entry into the trigger zone, not every loop iteration
  // while the object remains there). Starts the buzzer if
  // settings.alarmEnabled is true; always increments the trigger count
  // regardless, so a "silenced" intrusion is still logged.
  void trigger(const Settings &settings);

  // Call once when continuous in-zone activity crosses the sustained
  // threshold — sends a distinct PULSED buzzer pattern (not continuous)
  // so it's audibly different from a normal short trigger.
  void triggerSustained(const Settings &settings);

  // Short test beep, independent of the settings/duration logic —
  // for confirming the buzzer is wired correctly. Respects the buzzer
  // hardware mute (buzzerMasterEnabled).
  void testBuzzer(const Settings &settings);

  // Immediately silences the buzzer, canceling any in-progress alarm.
  void emergencyStop(const Settings &settings);

  // True if the buzzer is currently sounding.
  bool isActive();

  // Call periodically (e.g. every loop) — internally rate-limited to
  // BUZZER_HEARTBEAT_INTERVAL_MS. Pings the remote buzzer unit; sends a
  // Telegram alert on unreachable/restored transitions (edge-triggered,
  // not spammy).
  void checkHeartbeat(const Settings &settings);

  // Call once right after WiFi connects — pushes identity to buzzers
  // immediately instead of waiting for the first periodic heartbeat.
  void announceNow(const Settings &settings);

  // Last known reachability of the remote buzzer unit(s) — true if AT
  // LEAST ONE configured buzzer is reachable (updated by checkHeartbeat).
  bool isBuzzerReachable();

  // Reachability of one specific buzzer slot (0-4), for per-device display.
  bool isBuzzerSlotReachable(int index);

  // 0 if not currently connected (or time not synced when it connected).
  uint32_t getBuzzerConnectedSince(int index);

  // n=0 → most recent past connection session, n=1 → the one before.
  // Sets both to 0 if that many sessions haven't happened yet.
  void getBuzzerHistory(int index, int n, uint32_t &connectEpoch, uint32_t &disconnectEpoch);

  // RAM-only counters (see NOTE ON PERSISTENCE above).
  uint32_t getTriggerCount();
  unsigned long getLastTriggerMillis(); // 0 if no trigger yet this session

  // Wipes every RAM-only counter/history this module owns (trigger count,
  // last-trigger time, per-slot reachability + connect/disconnect history).
  // Called by Factory Reset so a reset takes effect immediately rather
  // than only appearing correct after the follow-up restart completes.
  void resetAll();

}

#endif // ALARM_H
