/*
  =====================================================================
  sensor.cpp — Ultrasonic Sensor Module (implementation)
  =====================================================================
  REVERTED: median-of-5 -> median-of-3. Median-of-5 gave slightly better
  single-spike rejection, but each reading took ~300-350ms (4 gaps of
  60ms + ping/echo time), and 3 CONSECUTIVE confirmed readings are
  required before an alarm fires (see INTRUSION_CONFIRM_READINGS in
  config.h) — meaning an intruder had to stay in the zone for ~1 full
  second before detection could ever fire. A person moving quickly
  through a narrow zone could cross it faster than that and never get
  detected at all — a real miss, not just a delay. Median-of-3 halves
  the per-reading time (~150-200ms), bringing total confirm time down
  to ~450-600ms, while still rejecting a single-frame noise spike (it
  just needs 2-of-3 samples to agree, same idea as before, less margin
  against a rare double-spike but a worthwhile trade for a security
  system where missing a fast-moving intruder is the bigger risk).
  =====================================================================
*/

#include "sensor.h"
#include "config.h"

namespace Sensor {

// -----------------------------------------------------------------------
// Takes ONE raw ping and converts it to centimeters.
// Returns -1.0 if the pulse timed out (no echo received in time) or the
// reading fell outside the sensor's valid range (blind zone / out of spec).
// -----------------------------------------------------------------------
static float pingOnce() {
  // Trigger pulse: LOW settle, then 10µs HIGH, per JSN-SR04T datasheet.
  digitalWrite(PIN_SENSOR_TRIG, LOW);
  delayMicroseconds(4);
  digitalWrite(PIN_SENSOR_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_SENSOR_TRIG, LOW);

  unsigned long durationUs = pulseIn(PIN_SENSOR_ECHO, HIGH, SENSOR_TIMEOUT_US);

  if (durationUs == 0) {
    // pulseIn returns 0 on timeout — no echo came back in time.
    return -1.0;
  }

  float distanceCm = (float)durationUs / (float)SENSOR_US_PER_CM;

  if (distanceCm < SENSOR_MIN_RANGE_CM || distanceCm > SENSOR_MAX_RANGE_CM) {
    // Outside the sensor's honest operating range — treat as invalid
    // rather than trusting a number we know the hardware can't reliably
    // produce.
    return -1.0;
  }

  return distanceCm;
}

// -----------------------------------------------------------------------
// Sorts exactly 3 floats ascending (tiny fixed-size sort — no need for a
// general sorting algorithm for 3 elements).
// -----------------------------------------------------------------------
static void sort3(float &a, float &b, float &c) {
  if (a > b) { float t = a; a = b; b = t; }
  if (b > c) { float t = b; b = c; c = t; }
  if (a > b) { float t = a; a = b; b = t; }
}

void begin() {
  pinMode(PIN_SENSOR_TRIG, OUTPUT);
  pinMode(PIN_SENSOR_ECHO, INPUT);
  digitalWrite(PIN_SENSOR_TRIG, LOW);
  Serial.println("[SENSOR] JSN-SR04T driver initialized.");
}

float readDistanceCM() {
  float s1 = pingOnce();
  delay(SENSOR_SAMPLE_GAP_MS);
  float s2 = pingOnce();
  delay(SENSOR_SAMPLE_GAP_MS);
  float s3 = pingOnce();

  // Count how many samples were valid. We need at least 2 valid samples
  // to trust a median — if only 0 or 1 samples are valid, the sensor is
  // either genuinely seeing nothing in range or something's wrong, and
  // we should say so honestly rather than guess.
  int validCount = 0;
  if (s1 > 0) validCount++;
  if (s2 > 0) validCount++;
  if (s3 > 0) validCount++;

  if (validCount < 2) {
    return -1.0;
  }

  // Replace any invalid (-1) sample with a duplicate of a valid one so
  // the median still makes sense with simple sorting, rather than writing
  // separate branching logic for every combination of valid/invalid.
  if (s1 < 0) s1 = (s2 > 0) ? s2 : s3;
  if (s2 < 0) s2 = (s1 > 0) ? s1 : s3;
  if (s3 < 0) s3 = (s1 > 0) ? s1 : s2;

  sort3(s1, s2, s3);
  return s2; // median
}

} // namespace Sensor
