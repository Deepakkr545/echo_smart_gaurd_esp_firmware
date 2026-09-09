#ifndef WEBSERVER_H
#define WEBSERVER_H
#include <Arduino.h>
#include "storage.h"

namespace DashboardServer {
  // emergencyStopActive/StartMillis: shared with main.ino so the dashboard
  // and the auto-resume timer in loop() see the same state.
  // armedByNightModePtr: shared so a manual Arm/Disarm from the dashboard
  // can clear the flag — Night Mode should only ever undo its OWN auto-arm.
  void begin(Settings *settingsPtr, bool *armedPtr, bool *armedByNightModePtr, float *lastDistanceCmPtr,
             bool *emergencyStopActivePtr, unsigned long *emergencyStopStartMillisPtr,
             unsigned long *muteStartMillisPtr);
  void handleClient();
}
#endif
