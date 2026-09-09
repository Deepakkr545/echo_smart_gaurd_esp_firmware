/*
  stats.h — Runtime statistics (Security/Sensor/Network/Time totals).
  All RAM-only — resets on reboot by design (avoids risky EEPROM writes
  every loop). "Today" fields reset at local midnight once NTP is synced.
*/
#ifndef STATS_H
#define STATS_H
#include <Arduino.h>

namespace Stats {
  void begin();

  // Call every loop with the latest reading (valid=false if no reading).
  void recordSensorReading(float distanceCm, bool valid);

  // Call every loop with current armed/muted/estop state (accumulates durations).
  void recordState(bool armed, bool muted, bool estopActive);

  // Call once when an intrusion is confirmed.
  void recordIntrusion();

  // Call once when a Telegram alert actually gets sent.
  void recordAlertSent();

  // Call once when WiFi (re)connects in Station mode.
  void recordWifiConnected();

  float getMinDistanceToday();
  float getMaxDistanceToday();
  float getAvgDistanceToday();
  String getSensorHealth();     // "Good" / "Warning" / "Error"
  float getSensorUpdateRateHz();

  // True if valid readings have been bit-for-bit identical for
  // SENSOR_FROZEN_THRESHOLD_SEC straight — runs independent of arm
  // state or trigger zone, so it also catches issues at night when
  // nothing should be near the sensor at all.
  bool isSensorFrozen();

  uint32_t getTodayIntrusions();
  uint32_t getTodayAlerts();

  unsigned long getProtectionTimeMs();
  unsigned long getPauseTimeMs();
  unsigned long getEstopTimeMs();
  unsigned long getLongestArmedMs();

  unsigned long getWifiConnectedSinceMillis(); // 0 if never
  unsigned long getLastReconnectMillis();      // 0 if never

  // Wipes every counter back to its just-booted state. Called by Factory
  // Reset so stats are visibly zeroed right away, not just implicitly
  // after the restart that follows.
  void resetAll();
}
#endif
