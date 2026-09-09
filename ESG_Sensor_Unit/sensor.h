/*
  =====================================================================
  sensor.h — Ultrasonic Sensor Module (public interface)
  =====================================================================
  DESIGN PRINCIPLE:
  Same as Display — main.ino never touches pulseIn() or raw GPIO timing
  directly. It just asks Sensor:: for a distance and gets back either a
  valid number or a clear "invalid" signal.

  SCOPE:
  Raw filtered distance reading only. No wall-ignore logic, no
  calibration, no alarm triggering yet — those come in later steps,
  built ON TOP of this reliable reading, not mixed into it.
  =====================================================================
*/

#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>

namespace Sensor {

  // Sets pin modes. Call once in setup().
  void begin();

  // Returns a filtered distance in centimeters.
  // Returns -1.0 if no valid reading could be obtained (timeout, or all
  // 3 samples were out of the valid sensor range).
  //
  // NOTE: This call takes ~120-200ms (3 pings + gaps). It is intentionally
  // blocking — for this project's polling rate (a few times per second)
  // that's acceptable, and it keeps the logic simple and easy to reason
  // about. If we ever need higher sample rates, this would need to
  // become non-blocking/interrupt-driven — noted for future scalability.
  float readDistanceCM();

}

#endif // SENSOR_H
