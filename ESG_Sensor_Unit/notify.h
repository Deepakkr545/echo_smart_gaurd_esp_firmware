#ifndef NOTIFY_H
#define NOTIFY_H
#include <Arduino.h>
#include "storage.h"

namespace Notify {
  // Pointers mirror DashboardServer::begin() — lets the /status Telegram
  // command build a full report without duplicating main.ino's state.
  void begin(Settings *settingsPtr, bool *armedPtr, float *lastDistanceCmPtr,
             bool *emergencyStopActivePtr, unsigned long *emergencyStopStartMillisPtr,
             unsigned long *muteStartMillisPtr);

  // Call every loop() — internally rate-limited (polls Telegram every
  // few seconds). Responds to the /status command with a full report.
  void checkIncomingCommands();

  String currentTimeString();
  String timeStringPlusSeconds(long secs);
  void sendTextMessage(const String &text, bool ignorePause = false);
  void sendIntruderAlert(float distanceCm, uint32_t triggerCount);

  // Test Mode — when active, every outgoing message is prefixed so a
  // walk-test intrusion looks nothing like a real one, without muting
  // Telegram entirely (muting would defeat the point of a walk-test:
  // confirming the full sensor -> buzzer -> Telegram chain works).
  void setTestMode(bool active);

  bool isConfigured();
  uint32_t getTotalSent();
  uint32_t getFailedCount();
  unsigned long getLastAlertMillis();     // 0 if none this session
  uint32_t getLastAlertEpoch();           // real NTP timestamp, 0 if unknown
  unsigned long getLastSuccessMillis();   // 0 if none this session
  unsigned long getLastResponseMs();      // duration of last HTTP call

  // Wipes this module's RAM-only send counters. Called by Factory Reset.
  void resetStats();
}
#endif
