/*
  =====================================================================
  calibration.h — Calibration Module (public interface)
  =====================================================================
  DESIGN PRINCIPLE:
  Calibration reads from Sensor:: and writes into Settings via Storage::,
  but owns none of that hardware/storage logic itself — it's a
  coordinator that sits on top of modules we already built and tested.

  SCOPE:
  Compute and save wallDistanceCm + triggerDistanceCm. No alarm/intruder
  detection logic yet (Step 7) — this module's only job is producing a
  trustworthy calibration result.
  =====================================================================
*/

#ifndef CALIBRATION_H
#define CALIBRATION_H

#include <Arduino.h>
#include "storage.h"

namespace Calibration {

  // Runs the full calibration process: takes multiple readings (assumes
  // the staircase is currently empty), rejects outliers, and if enough
  // valid data was collected, updates settings.wallDistanceCm and
  // settings.triggerDistanceCm, then saves via Storage::save().
  //
  // Prints progress to Serial and (if provided) shows progress on the
  // OLED via the displayCallback, so the caller doesn't need calibration
  // to know about Display:: directly (keeps this module display-agnostic
  // for now — the web dashboard in a later step will want progress too,
  // without needing OLED-specific code duplicated here).
  //
  // Returns true if calibration succeeded and settings were updated.
  bool run(Settings &settings, void (*progressCallback)(int percent) = nullptr);

}

#endif // CALIBRATION_H
