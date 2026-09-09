#include "stats.h"
#include "config.h"
#include <time.h>

namespace Stats {

static float minDist = -1, maxDist = -1, avgAccum = 0;
static uint32_t avgCount = 0;
static int lastDay = -1;
static uint32_t todayIntrusions = 0, todayAlerts = 0;

static int invalidStreak = 0, validStreak = 0;
static unsigned long lastReadingMillis = 0;
static float readingRateHz = 0;

static float lastDistinctValue = -1;
static unsigned long sameValueStreakStart = 0;
static bool frozenFlag = false;

static unsigned long lastTickMillis = 0;
static unsigned long protectionMs = 0, pauseMs = 0, estopMs = 0;
static unsigned long armedStartMillis = 0, longestArmedMs = 0;
static bool wasArmed = false;

static unsigned long wifiConnectedSince = 0, lastReconnect = 0;

static void checkDayRollover() {
  time_t t = time(nullptr);
  if (t < 100000) return; // not synced yet
  int day = localtime(&t)->tm_yday;
  if (lastDay == -1) { lastDay = day; return; }
  if (day != lastDay) {
    lastDay = day;
    minDist = -1; maxDist = -1; avgAccum = 0; avgCount = 0;
    todayIntrusions = 0; todayAlerts = 0;
  }
}

void begin() {
  lastTickMillis = millis();
}

void recordSensorReading(float distanceCm, bool valid) {
  checkDayRollover();

  unsigned long now = millis();
  if (lastReadingMillis > 0) {
    unsigned long dt = now - lastReadingMillis;
    if (dt > 0) readingRateHz = 1000.0f / dt;
  }
  lastReadingMillis = now;

  if (valid) {
    validStreak++; invalidStreak = 0;
    if (minDist < 0 || distanceCm < minDist) minDist = distanceCm;
    if (maxDist < 0 || distanceCm > maxDist) maxDist = distanceCm;
    avgAccum += distanceCm; avgCount++;

    // Frozen-sensor tracking — see config.h SENSOR_FROZEN_THRESHOLD_SEC.
    if (lastDistinctValue < 0 || fabs(distanceCm - lastDistinctValue) > 0.001f) {
      lastDistinctValue = distanceCm;
      sameValueStreakStart = now;
      frozenFlag = false;
    } else if (!frozenFlag && (now - sameValueStreakStart) >= (unsigned long)SENSOR_FROZEN_THRESHOLD_SEC * 1000UL) {
      frozenFlag = true;
    }
  } else {
    invalidStreak++; validStreak = 0;
  }
}

void recordState(bool armed, bool muted, bool estopActive) {
  unsigned long now = millis();
  unsigned long dt = now - lastTickMillis;
  lastTickMillis = now;

  if (armed) protectionMs += dt;
  if (muted) pauseMs += dt;
  if (estopActive) estopMs += dt;

  if (armed && !wasArmed) armedStartMillis = now;
  if (armed) {
    unsigned long dur = now - armedStartMillis;
    if (dur > longestArmedMs) longestArmedMs = dur;
  }
  wasArmed = armed;
}

void recordIntrusion() { checkDayRollover(); todayIntrusions++; }
void recordAlertSent() { checkDayRollover(); todayAlerts++; }

void recordWifiConnected() {
  unsigned long now = millis();
  if (wifiConnectedSince == 0) wifiConnectedSince = now;
  lastReconnect = now;
}

float getMinDistanceToday() { return minDist; }
float getMaxDistanceToday() { return maxDist; }
float getAvgDistanceToday() { return avgCount > 0 ? avgAccum / avgCount : -1; }

String getSensorHealth() {
  if (invalidStreak >= 8) return "Error";
  if (invalidStreak >= 3) return "Warning";
  return "Good";
}

float getSensorUpdateRateHz() { return readingRateHz; }
bool isSensorFrozen() { return frozenFlag; }
uint32_t getTodayIntrusions() { return todayIntrusions; }
uint32_t getTodayAlerts() { return todayAlerts; }
unsigned long getProtectionTimeMs() { return protectionMs; }
unsigned long getPauseTimeMs() { return pauseMs; }
unsigned long getEstopTimeMs() { return estopMs; }
unsigned long getLongestArmedMs() { return longestArmedMs; }
unsigned long getWifiConnectedSinceMillis() { return wifiConnectedSince; }
unsigned long getLastReconnectMillis() { return lastReconnect; }

void resetAll() {
  minDist = -1; maxDist = -1; avgAccum = 0; avgCount = 0;
  lastDay = -1;
  todayIntrusions = 0; todayAlerts = 0;
  invalidStreak = 0; validStreak = 0;
  lastReadingMillis = 0; readingRateHz = 0;
  lastDistinctValue = -1; sameValueStreakStart = 0; frozenFlag = false;
  lastTickMillis = millis();
  protectionMs = 0; pauseMs = 0; estopMs = 0;
  armedStartMillis = 0; longestArmedMs = 0; wasArmed = false;
  wifiConnectedSince = 0; lastReconnect = 0;
  Serial.println("[STATS] All counters cleared (factory reset).");
}

}
