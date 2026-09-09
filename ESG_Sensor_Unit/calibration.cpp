/*
  =====================================================================
  calibration.cpp — Calibration Module (implementation)
  =====================================================================
*/

#include "calibration.h"
#include "config.h"
#include "sensor.h"
#include <time.h>

namespace Calibration {

bool run(Settings &settings, void (*progressCallback)(int percent)) {
  Serial.println("[CALIBRATION] Starting. Ensure no-movement.");

  float samples[CALIBRATION_SAMPLE_COUNT];
  int validCount = 0;

  for (int i = 0; i < CALIBRATION_SAMPLE_COUNT; i++) {
    float reading = Sensor::readDistanceCM();
    if (reading > 0) {
      samples[validCount] = reading;
      validCount++;
    }

    int percent = ((i + 1) * 100) / CALIBRATION_SAMPLE_COUNT;
    Serial.print("[CALIBRATION] Progress: ");
    Serial.print(percent);
    Serial.println("%");
    if (progressCallback != nullptr) {
      progressCallback(percent);
    }
  }

  float minValidNeeded = CALIBRATION_SAMPLE_COUNT * CALIBRATION_MIN_VALID_RATIO;
  if (validCount < minValidNeeded) {
    Serial.print("[CALIBRATION] FAILED — only ");
    Serial.print(validCount);
    Serial.print("/");
    Serial.print(CALIBRATION_SAMPLE_COUNT);
    Serial.println(" readings were valid. Check sensor wiring/placement.");
    return false;
  }

  // --- Mean ---
  float sum = 0;
  for (int i = 0; i < validCount; i++) sum += samples[i];
  float mean = sum / validCount;

  // --- Standard deviation ---
  float varianceSum = 0;
  for (int i = 0; i < validCount; i++) {
    float diff = samples[i] - mean;
    varianceSum += diff * diff;
  }
  float stdDev = sqrt(varianceSum / validCount);

  // --- Discard outliers beyond 2 standard deviations, average the rest ---
  float filteredSum = 0;
  int filteredCount = 0;
  for (int i = 0; i < validCount; i++) {
    if (fabs(samples[i] - mean) <= (2.0f * stdDev) || stdDev < 0.001f) {
      filteredSum += samples[i];
      filteredCount++;
    }
  }

  if (filteredCount == 0) {
    // Shouldn't happen in practice (mean itself is always within 2*stdDev
    // of itself), but guard against it rather than divide by zero.
    Serial.println("[CALIBRATION] FAILED — no stable readings after outlier rejection.");
    return false;
  }

  float wallDistance = filteredSum / filteredCount;

  float triggerDistance = wallDistance - CALIBRATION_SAFETY_MARGIN_CM;
  if (triggerDistance < SENSOR_MIN_RANGE_CM) {
    triggerDistance = SENSOR_MIN_RANGE_CM;
    Serial.println("[CALIBRATION] WARNING: Wall is very close — trigger distance clamped to sensor minimum range.");
  }

  settings.wallDistanceCm = wallDistance;
  settings.triggerDistanceCm = triggerDistance;
  time_t nowEpoch = time(nullptr);
  settings.lastCalibrationEpoch = (nowEpoch > 100000) ? (uint32_t)nowEpoch : 0; // 0 = time not synced yet

  bool saved = Storage::save(settings);

  Serial.print("[CALIBRATION] SUCCESS. Wall distance: ");
  Serial.print(wallDistance, 1);
  Serial.print(" cm. Trigger distance set to: ");
  Serial.print(triggerDistance, 1);
  Serial.println(" cm.");

  return saved;
}

} // namespace Calibration
