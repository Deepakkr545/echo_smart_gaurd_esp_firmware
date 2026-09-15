/*
  =====================================================================
  display.cpp — OLED Display Module (implementation)
  =====================================================================
*/

#include "display.h"
#include "config.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h> // sin/cos/fmod — used by the Radar Sweep, Sonar Pulse, and Analog Gauge skins

// ---------------------------------------------------------------------
// Hardware constants
// ---------------------------------------------------------------------
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_I2C_ADDR_PRIMARY  0x3C   // Most common address for 0.96" SSD1306.
#define OLED_I2C_ADDR_ALT      0x3D   // Second most common — some modules ship on this instead.
#define OLED_RESET     -1     // No dedicated reset pin on this module

// How many (address x delay) rounds to try before giving up. A very common
// real-world cause of "SSD1306 not found" on ESP8266 boot is the OLED's
// own power rail not having fully settled yet when Wire.begin()/oled.begin()
// run (it shares the 3.3V rail with the WiFi radio, which draws current
// spikes during association) — a short retry loop fixes that without
// needing any hardware change.
#define DISPLAY_INIT_ATTEMPTS  4
#define DISPLAY_INIT_RETRY_DELAY_MS 300

static Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
static bool initialized = false;
static uint8_t activeAddr = OLED_I2C_ADDR_PRIMARY;

// ---------------------------------------------------------------------
// Small drawing helpers (mirrors the style used on the buzzer unit's
// OLED for a consistent look across both devices)
// ---------------------------------------------------------------------
static void drawCenteredText(const char* s, int y, uint8_t size, uint16_t color) {
  oled.setTextSize(size);
  oled.setTextColor(color);
  int16_t x1, y1; uint16_t w, h;
  oled.getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  oled.setCursor((SCREEN_WIDTH - w) / 2, y);
  oled.print(s);
}

static void drawDashedHLine(int x, int y, int w, int on, int off, uint16_t color) {
  int i = 0;
  while (i < w) {
    int seg = min(on, w - i);
    oled.drawFastHLine(x + i, y, seg, color);
    i += on + off;
  }
}

static void drawWifiIcon(int x, int y, bool ok) {
  uint16_t c = SSD1306_WHITE;
  oled.fillCircle(x + 6, y + 8, 1, c);
  if (!ok) {
    oled.drawLine(x, y, x + 12, y + 8, c);
    oled.drawLine(x + 12, y, x, y + 8, c);
    return;
  }
  for (int i = 0; i < 3; i++) {
    int r = 3 + i * 3;
    oled.drawCircleHelper(x + 6, y + 8, r, 0x3, c);
  }
}

// Simple speaker silhouette (box + cone + sound-wave lines) when a buzzer
// is reachable, a cross (matching the WiFi icon's language) when not.
static void drawBuzzerIcon(int x, int y, bool ok) {
  uint16_t c = SSD1306_WHITE;
  if (!ok) {
    oled.drawLine(x, y, x + 12, y + 8, c);
    oled.drawLine(x + 12, y, x, y + 8, c);
    return;
  }
  oled.fillRect(x, y + 2, 3, 4, c);
  oled.fillTriangle(x + 3, y, x + 3, y + 8, x + 8, y + 4, c);
  oled.drawLine(x + 10, y + 1, x + 10, y + 7, c);
  oled.drawLine(x + 12, y - 1, x + 12, y + 9, c);
}

// ---------------------------------------------------------------------
// Reading-screen skins — see display.h for the public setSkin/getSkin
// API. Every skinDraw* function below has the EXACT same signature and
// receives the exact same data showReadingScreen() always did — a skin
// is purely a different arrangement of the same information, never a
// different data source. inZone/blink are precomputed once by
// showReadingScreen() and passed in, so every skin that wants the
// trigger-zone blink effect gets it for free and stays in sync with
// the others.
// ---------------------------------------------------------------------

static const char* SKIN_NAMES[DISPLAY_SKIN_COUNT] = {
  "Classic Numeric",
  "Bar Gauge",
  "Radar Sweep",
  "Minimal Shield",
  "Security HUD",
  "Sonar Pulse",
  "Retro Terminal",
  "Grid Dashboard",
  "Analog Gauge",
  "Big Digit",
  "Heartbeat Monitor",
  "Tachometer",
  "Thermometer",
  "Equalizer Bars",
  "VU Meter",
  "Digital Matrix",
  "CRT Scanlines",
  "Orbit Monitor",
  "Ripple Wave",
  "Compass Dial",
  "Pixel Guard",
  "Matrix Rain",
  "Fingerprint Scan",
  "Combination Lock",
  "Flame Alert",
  "Frost Idle",
  "Lightning Pulse",
  "Star Field",
  "Hourglass Timer",
  "Constellation",
  "Breathing Circle",
  "Ink Ripple",
  "Moon Phases",
  "Aurora Waves",
  "Cassette Reels",
  "Tuning Dial",
  "Holo Grid",
  "Orbital Rings Pro",
  "Celestial Drift",
  "Night Guardian",
  "Floating Particles",
  "Growing Tree",
  "Falling Leaves",
  "VHS Static",
  "Pixel Arcade",
  "Data Stream",
  "Sleepy Cat",
  "Robot Buddy",
  "Swiss Grid",
  "Orbiting Planets",
  "Soft Pulse Dot",
  "Raindrop Ripples",
  "Dot Matrix Print",
  "Cyberpunk Terminal",
  "Watchdog",
  "Minimalist Gauge",
  "Corporate Ticker",
  "Blueprint",
  "Zen Garden",
  "Polar Aurora",
};

static uint8_t currentSkin = 0;

// Skin 0 — Classic Numeric. The original always-on layout: header bar
// with device name + ARMED/DISARM/ALARM badge, a large centered number,
// WiFi/buzzer icons bottom corners, trigger-zone status bottom center.
static void skinDrawClassic(bool armed, bool alarmActive, float distanceCm,
                             bool distanceValid, bool wifiConnected, bool buzzerReachable,
                             const String &deviceName, float triggerDistanceCm,
                             bool inZone, bool blink) {
  oled.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  oled.setTextSize(1);

  const char* badge = alarmActive ? "ALARM!" : (armed ? "ARMED" : "DISARM");
  int16_t x1, y1; uint16_t w, h;
  oled.getTextBounds(badge, 0, 0, &x1, &y1, &w, &h);
  int availableW = SCREEN_WIDTH - (int)w - 6 - 2;
  int maxChars = availableW / 6;
  if (maxChars < 1) maxChars = 1;
  String hdr = (int)deviceName.length() > maxChars ? deviceName.substring(0, maxChars) : deviceName;
  oled.setCursor(2, 2);
  oled.print(hdr);
  oled.setCursor(SCREEN_WIDTH - w - 2, 2);
  oled.print(badge);

  oled.setTextColor(SSD1306_WHITE);
  if (distanceValid) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%.1f", distanceCm);
    drawCenteredText(buf, 22, 3, SSD1306_WHITE);
  } else {
    drawCenteredText("-- no reading --", 28, 1, SSD1306_WHITE);
  }

  drawWifiIcon(2, 54, wifiConnected);
  drawBuzzerIcon(SCREEN_WIDTH - 16, 54, buzzerReachable);

  if (inZone && blink) {
    drawCenteredText("* IN TRIGGER ZONE *", 56, 1, SSD1306_WHITE);
  } else if (triggerDistanceCm > 0) {
    char trigBuf[16];
    snprintf(trigBuf, sizeof(trigBuf), "Trig:%.0fcm", triggerDistanceCm);
    drawCenteredText(trigBuf, 56, 1, SSD1306_WHITE);
  } else {
    drawCenteredText("Not calibrated", 56, 1, SSD1306_WHITE);
  }
}

// Skin 1 — Bar Gauge. Distance shown as a horizontal fill bar (closer =
// fuller), with a tick mark on the bar at the trigger threshold. Reads
// almost like a fuel/signal gauge — quick to glance at from a distance
// without needing to read the exact number.
static void skinDrawBarGauge(bool armed, bool alarmActive, float distanceCm,
                              bool distanceValid, bool wifiConnected, bool buzzerReachable,
                              const String &deviceName, float triggerDistanceCm,
                              bool inZone, bool blink) {
  const char* badge = alarmActive ? "ALARM!" : (armed ? "ARMED" : "DISARM");
  drawCenteredText(badge, 2, 1, SSD1306_WHITE);
  drawDashedHLine(0, 12, SCREEN_WIDTH, 2, 2, SSD1306_WHITE);

  // Gauge range: 0..GAUGE_MAX_CM, clamped — this is a relative "how
  // close" indicator, not a precision readout (the exact number is
  // still printed above the bar).
  const float GAUGE_MAX_CM = 200.0f;
  float shown = distanceValid ? distanceCm : GAUGE_MAX_CM;
  if (shown < 0) shown = 0;
  if (shown > GAUGE_MAX_CM) shown = GAUGE_MAX_CM;
  // Fill grows as the object gets CLOSER (more "alert"), not farther.
  float fillRatio = 1.0f - (shown / GAUGE_MAX_CM);

  char distBuf[16];
  if (distanceValid) snprintf(distBuf, sizeof(distBuf), "%.1f cm", distanceCm);
  else snprintf(distBuf, sizeof(distBuf), "-- no reading --");
  drawCenteredText(distBuf, 18, 1, SSD1306_WHITE);

  int barX = 6, barY = 32, barW = SCREEN_WIDTH - 12, barH = 14;
  oled.drawRoundRect(barX, barY, barW, barH, 3, SSD1306_WHITE);
  int fillW = (int)((barW - 4) * fillRatio);
  if (fillW > 0) {
    oled.fillRoundRect(barX + 2, barY + 2, fillW, barH - 4, 2, SSD1306_WHITE);
  }
  if (triggerDistanceCm > 0 && triggerDistanceCm <= GAUGE_MAX_CM) {
    float trigRatio = 1.0f - (triggerDistanceCm / GAUGE_MAX_CM);
    int tickX = barX + (int)((barW - 4) * trigRatio) + 2;
    // Draw the tick INVERTED relative to whatever's under it so it stays
    // visible whether it lands inside the filled or unfilled part of the bar.
    oled.drawFastVLine(tickX, barY - 3, barH + 6, SSD1306_WHITE);
  }

  drawWifiIcon(2, 54, wifiConnected);
  drawBuzzerIcon(SCREEN_WIDTH - 16, 54, buzzerReachable);
  if (inZone && blink) {
    drawCenteredText("IN ZONE", 54, 1, SSD1306_WHITE);
  }
}

// Skin 2 — Radar Sweep. A classic rotating radar line inside concentric
// range rings, with a blip marking the object's approximate position
// once it's within the trigger zone. Continuously animated (uses
// millis()), so this skin visibly "lives" even when nothing's changing.
static void skinDrawRadarSweep(bool armed, bool alarmActive, float distanceCm,
                                bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                const String &deviceName, float triggerDistanceCm,
                                bool inZone, bool blink) {
  const int cx = 64, cy = 34, maxR = 26;
  oled.drawCircle(cx, cy, maxR, SSD1306_WHITE);
  oled.drawCircle(cx, cy, maxR * 2 / 3, SSD1306_WHITE);
  oled.drawCircle(cx, cy, maxR / 3, SSD1306_WHITE);
  oled.drawFastHLine(cx - maxR, cy, maxR * 2, SSD1306_WHITE);
  oled.drawFastVLine(cx, cy - maxR, maxR * 2, SSD1306_WHITE);

  // One full sweep every ~3 seconds — fast enough to feel alive on a
  // loop() that redraws several times a second, slow enough to actually
  // track with the eye.
  float angle = fmod(millis() / 3000.0f, 1.0f) * 2.0f * PI;
  int lx = cx + (int)(cos(angle) * maxR);
  int ly = cy + (int)(sin(angle) * maxR);
  oled.drawLine(cx, cy, lx, ly, SSD1306_WHITE);

  if (distanceValid && triggerDistanceCm > 0) {
    float ratio = distanceCm / (triggerDistanceCm * 2.0f); // 2x trigger distance = edge of radar
    if (ratio > 1.0f) ratio = 1.0f;
    if (ratio < 0.0f) ratio = 0.0f;
    int blipR = (int)(maxR * (1.0f - ratio)); // closer object = blip nearer center
    if (inZone) {
      int bx = cx + (int)(cos(angle) * blipR);
      int by = cy + (int)(sin(angle) * blipR);
      if (blink) oled.fillCircle(bx, by, 2, SSD1306_WHITE);
    }
  }

  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.0fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 55, 1, SSD1306_WHITE);

  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2);
  oled.print(badge);
  drawWifiIcon(SCREEN_WIDTH - 16, 2, wifiConnected);
}

// Skin 3 — Minimal Shield. Almost entirely whitespace by design: one
// big shield silhouette (matching the app's own branding language) that
// switches between a closed padlock (armed) and an open one (disarmed),
// with the number tucked underneath, small. For anyone who wants "can I
// tell the state from across the room" over precision.
static void skinDrawMinimalShield(bool armed, bool alarmActive, float distanceCm,
                                   bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                   const String &deviceName, float triggerDistanceCm,
                                   bool inZone, bool blink) {
  const int cx = 64, topY = 4;
  bool solid = alarmActive ? blink : true; // flash the whole shield during an active alarm

  if (solid) {
    // Shield outline (a hexagon-ish badge shape via two triangles + a rect body).
    oled.drawLine(cx - 20, topY, cx, topY - 2, SSD1306_WHITE);
    oled.drawLine(cx, topY - 2, cx + 20, topY, SSD1306_WHITE);
    oled.drawLine(cx - 20, topY, cx - 20, topY + 24, SSD1306_WHITE);
    oled.drawLine(cx + 20, topY, cx + 20, topY + 24, SSD1306_WHITE);
    oled.drawLine(cx - 20, topY + 24, cx, topY + 40, SSD1306_WHITE);
    oled.drawLine(cx + 20, topY + 24, cx, topY + 40, SSD1306_WHITE);

    // Padlock glyph in the middle — shackle open/closed communicates
    // armed/disarmed at a glance even from across a room.
    int lockY = topY + 14;
    oled.drawRoundRect(cx - 7, lockY, 14, 10, 2, SSD1306_WHITE);
    if (armed) {
      oled.drawCircleHelper(cx, lockY - 3, 5, 0b0011, SSD1306_WHITE); // closed shackle
      oled.drawFastVLine(cx - 5, lockY - 3, 3, SSD1306_WHITE);
      oled.drawFastVLine(cx + 5, lockY - 3, 3, SSD1306_WHITE);
    } else {
      oled.drawCircleHelper(cx - 3, lockY - 3, 5, 0b0011, SSD1306_WHITE); // shackle swung open
      oled.drawFastVLine(cx - 8, lockY - 3, 3, SSD1306_WHITE);
    }
  }

  char buf[16];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1f cm", distanceCm);
  else snprintf(buf, sizeof(buf), "-- no reading --");
  drawCenteredText(buf, 50, 1, SSD1306_WHITE);
  drawWifiIcon(2, 2, wifiConnected);
  drawBuzzerIcon(SCREEN_WIDTH - 16, 2, buzzerReachable);
}

// Skin 4 — Security HUD. Camera-viewfinder corner brackets, a blinking
// "recording" dot while armed, and a bold center readout — deliberately
// evokes a CCTV/surveillance overlay rather than a plain instrument.
static void skinDrawSecurityHud(bool armed, bool alarmActive, float distanceCm,
                                 bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                 const String &deviceName, float triggerDistanceCm,
                                 bool inZone, bool blink) {
  const int m = 4, len = 10;
  // Four corner brackets.
  oled.drawFastHLine(m, m, len, SSD1306_WHITE); oled.drawFastVLine(m, m, len, SSD1306_WHITE);
  oled.drawFastHLine(SCREEN_WIDTH - m - len, m, len, SSD1306_WHITE); oled.drawFastVLine(SCREEN_WIDTH - m - 1, m, len, SSD1306_WHITE);
  oled.drawFastHLine(m, SCREEN_HEIGHT - m - 1, len, SSD1306_WHITE); oled.drawFastVLine(m, SCREEN_HEIGHT - m - len, len, SSD1306_WHITE);
  oled.drawFastHLine(SCREEN_WIDTH - m - len, SCREEN_HEIGHT - m - 1, len, SSD1306_WHITE); oled.drawFastVLine(SCREEN_WIDTH - m - 1, SCREEN_HEIGHT - m - len, len, SSD1306_WHITE);

  if (armed && (millis() / 500) % 2 == 0) {
    oled.fillCircle(SCREEN_WIDTH - 12, 12, 3, SSD1306_WHITE);
  }
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(16, 6);
  oled.print(alarmActive ? "ALARM" : (armed ? "ARMED" : "STANDBY"));

  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1f", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 24, 3, SSD1306_WHITE);

  if (inZone && blink) {
    drawCenteredText("TARGET IN RANGE", 54, 1, SSD1306_WHITE);
  } else {
    drawWifiIcon(SCREEN_WIDTH / 2 - 20, 52, wifiConnected);
    drawBuzzerIcon(SCREEN_WIDTH / 2 + 8, 52, buzzerReachable);
  }
}

// Skin 5 — Sonar Pulse. A center readout surrounded by rings that
// continuously expand outward and fade back to the center, like a
// sonar ping repeating — purely decorative motion when idle, and speeds
// up automatically once something's in the trigger zone.
static void skinDrawSonarPulse(bool armed, bool alarmActive, float distanceCm,
                                bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                const String &deviceName, float triggerDistanceCm,
                                bool inZone, bool blink) {
  const int cx = 64, cy = 30, maxR = 24;
  unsigned long cycleMs = inZone ? 700UL : 1800UL; // pulses faster when something's close
  float phase = fmod((float)(millis() % cycleMs) / (float)cycleMs, 1.0f);
  for (int ring = 0; ring < 3; ring++) {
    float r = fmod(phase + ring / 3.0f, 1.0f) * maxR;
    if (r > 2) oled.drawCircle(cx, cy, (int)r, SSD1306_WHITE);
  }
  oled.fillCircle(cx, cy, 2, SSD1306_WHITE);

  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 58, 1, SSD1306_WHITE);

  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2);
  oled.print(badge);
  drawWifiIcon(SCREEN_WIDTH - 24, 2, wifiConnected);
  drawBuzzerIcon(SCREEN_WIDTH - 12, 2, buzzerReachable);
}

// Skin 6 — Retro Terminal. Bracketed monospace-style status lines and a
// blinking block cursor, styled after an old text-mode security
// console rather than a modern instrument display.
static void skinDrawRetroTerminal(bool armed, bool alarmActive, float distanceCm,
                                   bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                   const String &deviceName, float triggerDistanceCm,
                                   bool inZone, bool blink) {
  oled.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  oled.setCursor(4, 4);
  oled.print("[");
  oled.print(alarmActive ? "ALARM!!" : (armed ? "ARMED  " : "DISARM "));
  oled.print("]");

  oled.setCursor(4, 16);
  char distLine[24];
  if (distanceValid) snprintf(distLine, sizeof(distLine), "[DIST %5.1fcm]", distanceCm);
  else snprintf(distLine, sizeof(distLine), "[DIST  -- N/A]");
  oled.print(distLine);

  oled.setCursor(4, 28);
  char trigLine[24];
  if (triggerDistanceCm > 0) snprintf(trigLine, sizeof(trigLine), "[TRIG  %5.1fcm]", triggerDistanceCm);
  else snprintf(trigLine, sizeof(trigLine), "[TRIG  NOT SET]");
  oled.print(trigLine);

  oled.setCursor(4, 40);
  oled.print("[NET ");
  oled.print(wifiConnected ? "OK" : "--");
  oled.print(" BUZ ");
  oled.print(buzzerReachable ? "OK" : "--");
  oled.print("]");

  oled.setCursor(4, 52);
  oled.print(inZone ? "> INTRUDER DETECTED" : "> monitoring");
  if (blink) {
    int curX = inZone ? 4 + 19 * 6 : 4 + 12 * 6;
    oled.fillRect(curX, 52, 6, 8, SSD1306_WHITE);
  }
}

// Skin 7 — Grid Dashboard. Four equal quadrant tiles, each dedicated to
// one piece of status — distance, armed state, WiFi, buzzer — for
// anyone who'd rather see everything at once than a single hero number.
static void skinDrawGridDashboard(bool armed, bool alarmActive, float distanceCm,
                                   bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                   const String &deviceName, float triggerDistanceCm,
                                   bool inZone, bool blink) {
  const int midX = SCREEN_WIDTH / 2, midY = SCREEN_HEIGHT / 2;
  oled.drawFastVLine(midX, 0, SCREEN_HEIGHT, SSD1306_WHITE);
  oled.drawFastHLine(0, midY, SCREEN_WIDTH, SSD1306_WHITE);
  oled.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);

  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // Top-left: distance
  oled.setCursor(4, 4);
  oled.print("DIST");
  char buf[10];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.0fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 18, 1, SSD1306_WHITE); // (visually centered under quadrant by eye — small screen, close enough)
  oled.setCursor(midX / 2 - (int)(strlen(buf) * 3), 18);
  oled.print(buf);

  // Top-right: armed state
  oled.setCursor(midX + 4, 4);
  oled.print("STATE");
  oled.setCursor(midX + 4, 18);
  oled.print(alarmActive ? "ALARM" : (armed ? "ARMED" : "OFF"));

  // Bottom-left: WiFi
  oled.setCursor(4, midY + 4);
  oled.print("WIFI");
  drawWifiIcon(6, midY + 16, wifiConnected);

  // Bottom-right: Buzzer
  oled.setCursor(midX + 4, midY + 4);
  oled.print("BUZZ");
  drawBuzzerIcon(midX + 8, midY + 16, buzzerReachable);

  if (inZone && blink) {
    oled.fillRect(1, 1, SCREEN_WIDTH - 2, 10, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    drawCenteredText("IN TRIGGER ZONE", 2, 1, SSD1306_BLACK);
  }
}

// Skin 8 — Analog Gauge. A semicircular dial with a needle sweeping
// from "far" to "at trigger threshold", styled after an analog
// speedometer/pressure gauge rather than a digital readout.
static void skinDrawAnalogGauge(bool armed, bool alarmActive, float distanceCm,
                                 bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                 const String &deviceName, float triggerDistanceCm,
                                 bool inZone, bool blink) {
  const int cx = 64, cy = 46, r = 34;
  // Draw the dial as a half-circle arc (180..360 degrees, i.e. the top half).
  for (int a = 180; a <= 360; a += 6) {
    float rad = a * PI / 180.0f;
    int x1 = cx + (int)(cos(rad) * r), y1 = cy + (int)(sin(rad) * r);
    int x2 = cx + (int)(cos(rad) * (r - 3)), y2 = cy + (int)(sin(rad) * (r - 3));
    oled.drawLine(x1, y1, x2, y2, SSD1306_WHITE);
  }

  // Needle angle: 180deg (far / all-clear) sweeping to 360deg (very
  // close / at-or-past trigger threshold).
  const float GAUGE_MAX_CM = 200.0f;
  float shown = distanceValid ? distanceCm : GAUGE_MAX_CM;
  if (shown > GAUGE_MAX_CM) shown = GAUGE_MAX_CM;
  if (shown < 0) shown = 0;
  float ratio = 1.0f - (shown / GAUGE_MAX_CM); // 0=far, 1=close
  float needleAngle = (180.0f + ratio * 180.0f) * PI / 180.0f;
  int nx = cx + (int)(cos(needleAngle) * (r - 6));
  int ny = cy + (int)(sin(needleAngle) * (r - 6));
  oled.drawLine(cx, cy, nx, ny, SSD1306_WHITE);
  oled.fillCircle(cx, cy, 2, SSD1306_WHITE);

  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 54, 1, SSD1306_WHITE);

  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2);
  oled.print(badge);
  drawWifiIcon(SCREEN_WIDTH - 14, 2, wifiConnected);
}

// Skin 9 — Big Digit. Maximum-size number, minimum everything else —
// the whole panel inverts (black-on-white) while an alarm is actively
// sounding, for the most visible-from-across-the-room possible state.
static void skinDrawBigDigit(bool armed, bool alarmActive, float distanceCm,
                              bool distanceValid, bool wifiConnected, bool buzzerReachable,
                              const String &deviceName, float triggerDistanceCm,
                              bool inZone, bool blink) {
  bool invert = alarmActive && blink;
  if (invert) oled.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  uint16_t fg = invert ? SSD1306_BLACK : SSD1306_WHITE;

  char buf[8];
  if (distanceValid) {
    // Whole-number only at this size — a decimal point would need to
    // compete with the huge digits for very little added precision.
    snprintf(buf, sizeof(buf), "%d", (int)distanceCm);
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  drawCenteredText(buf, 14, 4, fg);

  oled.setTextSize(1);
  oled.setTextColor(fg);
  oled.setCursor(2, 2);
  oled.print(alarmActive ? "ALARM" : (armed ? "ARMED" : "OFF"));
  oled.setCursor(SCREEN_WIDTH - 26, 2);
  oled.print(wifiConnected ? "NET" : "---");
}

// Skin 10 — Heartbeat Monitor. A scrolling ECG-style waveform along the
// bottom half, with a sharp spike drawn the instant something enters
// the trigger zone — medical-monitor visual language for "vitals".
static void skinDrawHeartbeat(bool armed, bool alarmActive, float distanceCm,
                               bool distanceValid, bool wifiConnected, bool buzzerReachable,
                               const String &deviceName, float triggerDistanceCm,
                               bool inZone, bool blink) {
  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2);
  oled.print(badge);
  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  int16_t x1, y1; uint16_t w, h;
  oled.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  oled.setCursor(SCREEN_WIDTH - w - 2, 2);
  oled.setTextColor(SSD1306_WHITE);
  oled.print(buf);

  const int baseY = 40;
  int prevX = 0, prevY = baseY;
  unsigned long phase = millis() / 12;
  for (int x = 0; x <= SCREEN_WIDTH; x += 2) {
    unsigned long t = (x + phase) % 40;
    int y = baseY;
    if (inZone) {
      // Sharp QRS-style spike, repeating.
      if (t == 18) y = baseY - 18;
      else if (t == 20) y = baseY + 8;
      else if (t == 22) y = baseY - 4;
    } else {
      // Gentle idle blip.
      if (t == 18) y = baseY - 6;
    }
    oled.drawLine(prevX, prevY, x, y, SSD1306_WHITE);
    prevX = x; prevY = y;
  }
  drawDashedHLine(0, 52, SCREEN_WIDTH, 2, 2, SSD1306_WHITE);
  drawWifiIcon(2, 54, wifiConnected);
  drawBuzzerIcon(SCREEN_WIDTH - 16, 54, buzzerReachable);
}

// Skin 11 — Tachometer. A near-full-circle dial (like a car RPM gauge)
// with a "redline" arc drawn near the trigger threshold.
static void skinDrawTachometer(bool armed, bool alarmActive, float distanceCm,
                                bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                const String &deviceName, float triggerDistanceCm,
                                bool inZone, bool blink) {
  const int cx = 64, cy = 38, r = 30;
  const float startDeg = 135, sweepDeg = 270;
  for (int i = 0; i <= 20; i++) {
    float a = (startDeg + sweepDeg * i / 20.0f) * PI / 180.0f;
    int x1p = cx + (int)(cos(a) * r), y1p = cy + (int)(sin(a) * r);
    int x2p = cx + (int)(cos(a) * (r - 4)), y2p = cy + (int)(sin(a) * (r - 4));
    oled.drawLine(x1p, y1p, x2p, y2p, SSD1306_WHITE);
  }
  // Redline zone — last 15% of sweep.
  for (int i = 17; i <= 20; i++) {
    float a = (startDeg + sweepDeg * i / 20.0f) * PI / 180.0f;
    int x1p = cx + (int)(cos(a) * (r + 2)), y1p = cy + (int)(sin(a) * (r + 2));
    int x2p = cx + (int)(cos(a) * (r - 6)), y2p = cy + (int)(sin(a) * (r - 6));
    oled.drawLine(x1p, y1p, x2p, y2p, SSD1306_WHITE);
  }
  const float GAUGE_MAX_CM = 200.0f;
  float shown = distanceValid ? distanceCm : GAUGE_MAX_CM;
  if (shown > GAUGE_MAX_CM) shown = GAUGE_MAX_CM;
  float ratio = 1.0f - (shown / GAUGE_MAX_CM);
  float needleA = (startDeg + sweepDeg * ratio) * PI / 180.0f;
  oled.drawLine(cx, cy, cx + (int)(cos(needleA) * (r - 8)), cy + (int)(sin(needleA) * (r - 8)), SSD1306_WHITE);
  oled.fillCircle(cx, cy, 2, SSD1306_WHITE);
  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.0fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 56, 1, SSD1306_WHITE);
}

// Skin 12 — Thermometer. A vertical mercury-style bar (bulb at bottom)
// that rises as an object gets closer — proximity framed as "rising
// temperature" rather than distance.
static void skinDrawThermometer(bool armed, bool alarmActive, float distanceCm,
                                 bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                 const String &deviceName, float triggerDistanceCm,
                                 bool inZone, bool blink) {
  const int tubeX = 30, tubeTop = 4, tubeBottom = 50, tubeW = 8;
  const int bulbCy = tubeBottom + 6, bulbR = 7;
  oled.drawRoundRect(tubeX, tubeTop, tubeW, tubeBottom - tubeTop, 4, SSD1306_WHITE);
  oled.drawCircle(tubeX + tubeW / 2, bulbCy, bulbR, SSD1306_WHITE);

  const float GAUGE_MAX_CM = 200.0f;
  float shown = distanceValid ? distanceCm : GAUGE_MAX_CM;
  if (shown > GAUGE_MAX_CM) shown = GAUGE_MAX_CM;
  float fillRatio = 1.0f - (shown / GAUGE_MAX_CM);
  int fillH = (int)((tubeBottom - tubeTop - 4) * fillRatio);
  oled.fillCircle(tubeX + tubeW / 2, bulbCy, bulbR - 2, SSD1306_WHITE);
  if (fillH > 0) oled.fillRoundRect(tubeX + 2, tubeBottom - 2 - fillH, tubeW - 4, fillH, 2, SSD1306_WHITE);

  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(48, 6);
  oled.print(badge);
  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  oled.setCursor(48, 20);
  oled.print(buf);
  drawWifiIcon(48, 36, wifiConnected);
  drawBuzzerIcon(48, 50, buzzerReachable);
}

// Skin 13 — Equalizer Bars. A row of animated vertical bars, like an
// audio spectrum analyzer — idle bars gently bounce, and all bars snap
// to full height briefly when something enters the trigger zone.
static void skinDrawEqualizer(bool armed, bool alarmActive, float distanceCm,
                               bool distanceValid, bool wifiConnected, bool buzzerReachable,
                               const String &deviceName, float triggerDistanceCm,
                               bool inZone, bool blink) {
  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  drawCenteredText(badge, 2, 1, SSD1306_WHITE);
  const int barCount = 12, barW = 6, gap = 2, baseY = 50, maxH = 32;
  int totalW = barCount * (barW + gap) - gap;
  int startX = (SCREEN_WIDTH - totalW) / 2;
  for (int i = 0; i < barCount; i++) {
    unsigned long seed = (millis() / 90) + i * 37;
    int h = inZone ? (maxH - (int)((seed * 7) % 6)) : (6 + (int)((seed * 13) % (maxH - 10)));
    int x = startX + i * (barW + gap);
    oled.fillRect(x, baseY - h, barW, h, SSD1306_WHITE);
  }
  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 55, 1, SSD1306_WHITE);
}

// Skin 14 — VU Meter. A classic swinging analog needle that visibly
// bounces/oscillates rather than sitting still — evokes an old audio
// level meter more than a calm instrument gauge.
static void skinDrawVuMeter(bool armed, bool alarmActive, float distanceCm,
                             bool distanceValid, bool wifiConnected, bool buzzerReachable,
                             const String &deviceName, float triggerDistanceCm,
                             bool inZone, bool blink) {
  const int cx = 64, cy = 54, r = 40;
  for (int a = 200; a <= 340; a += 10) {
    float rad = a * PI / 180.0f;
    oled.drawLine(cx + (int)(cos(rad) * r), cy + (int)(sin(rad) * r),
                  cx + (int)(cos(rad) * (r - 4)), cy + (int)(sin(rad) * (r - 4)), SSD1306_WHITE);
  }
  const float GAUGE_MAX_CM = 200.0f;
  float shown = distanceValid ? distanceCm : GAUGE_MAX_CM;
  if (shown > GAUGE_MAX_CM) shown = GAUGE_MAX_CM;
  float baseRatio = 1.0f - (shown / GAUGE_MAX_CM);
  // Bounce jitter on top of the real reading — a genuine VU meter never
  // sits perfectly still even at a constant input level.
  float jitter = inZone ? (sin(millis() / 60.0f) * 0.06f) : (sin(millis() / 260.0f) * 0.03f);
  float ratio = baseRatio + jitter;
  if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
  float needleA = (200 + 140 * ratio) * PI / 180.0f;
  oled.drawLine(cx, cy, cx + (int)(cos(needleA) * (r - 8)), cy + (int)(sin(needleA) * (r - 8)), SSD1306_WHITE);
  oled.fillCircle(cx, cy, 2, SSD1306_WHITE);
  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2); oled.print(badge);
  drawWifiIcon(SCREEN_WIDTH - 16, 2, wifiConnected);
}

// Skin 15 — Digital Matrix. Distance rendered as a blocky hex readout,
// like a technical debug overlay — a deliberately more "raw data" look
// than any of the plain numeric skins.
static void skinDrawDigitalMatrix(bool armed, bool alarmActive, float distanceCm,
                                   bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                   const String &deviceName, float triggerDistanceCm,
                                   bool inZone, bool blink) {
  oled.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(4, 4);
  oled.print("STATUS: ");
  oled.print(alarmActive ? "ALARM" : (armed ? "ARM" : "OFF"));

  int hexVal = distanceValid ? (int)(distanceCm * 10) : 0;
  char hexBuf[10];
  snprintf(hexBuf, sizeof(hexBuf), "0x%04X", hexVal);
  drawCenteredText(hexBuf, 20, 2, SSD1306_WHITE);

  char decBuf[16];
  if (distanceValid) snprintf(decBuf, sizeof(decBuf), "DEC: %.1fcm", distanceCm);
  else snprintf(decBuf, sizeof(decBuf), "DEC: N/A");
  oled.setCursor(4, 42);
  oled.print(decBuf);

  oled.setCursor(4, 52);
  oled.print("NET:");
  oled.print(wifiConnected ? "1" : "0");
  oled.print(" BUZ:");
  oled.print(buzzerReachable ? "1" : "0");
  oled.print(" ZN:");
  oled.print(inZone ? "1" : "0");
}

// Skin 16 — CRT Scanlines. A big number with retro horizontal scanlines
// drawn across the whole panel — evokes an old analog security monitor.
static void skinDrawCrtScanlines(bool armed, bool alarmActive, float distanceCm,
                                  bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                  const String &deviceName, float triggerDistanceCm,
                                  bool inZone, bool blink) {
  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1f", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 16, 3, SSD1306_WHITE);

  for (int y = 0; y < SCREEN_HEIGHT; y += 2) {
    oled.drawFastHLine(0, y, SCREEN_WIDTH, SSD1306_WHITE);
  }
  // Redraw the number bright/solid so scanlines only affect the
  // background, not the readout itself, then re-add a thin border.
  drawCenteredText(buf, 16, 3, SSD1306_WHITE);
  oled.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);

  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(4, 4); oled.print(badge);
  if (inZone && blink) drawCenteredText("SIGNAL DETECTED", 54, 1, SSD1306_WHITE);
}

// Skin 17 — Orbit Monitor. A single dot continuously orbiting a center
// hub, speeding up once something's in the trigger zone — "actively
// monitoring" as a literal orbiting motion rather than a sweep/pulse.
static void skinDrawOrbitMonitor(bool armed, bool alarmActive, float distanceCm,
                                  bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                  const String &deviceName, float triggerDistanceCm,
                                  bool inZone, bool blink) {
  const int cx = 64, cy = 30, r = 22;
  oled.drawCircle(cx, cy, r, SSD1306_WHITE);
  oled.fillCircle(cx, cy, 3, SSD1306_WHITE);
  unsigned long periodMs = inZone ? 900UL : 2400UL;
  float angle = fmod(millis() / (float)periodMs, 1.0f) * 2.0f * PI;
  int dx = cx + (int)(cos(angle) * r), dy = cy + (int)(sin(angle) * r);
  oled.fillCircle(dx, dy, 3, SSD1306_WHITE);
  // Trailing dot for a sense of motion.
  float trailA = angle - 0.4f;
  oled.fillCircle(cx + (int)(cos(trailA) * r), cy + (int)(sin(trailA) * r), 1, SSD1306_WHITE);

  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 56, 1, SSD1306_WHITE);
  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2); oled.print(badge);
}

// Skin 18 — Ripple Wave. Water-ripple-style expanding rings from the
// center — paced and styled distinctly from Sonar Pulse (slower,
// softer rings that fade by shrinking their draw frequency near the edge).
static void skinDrawRippleWave(bool armed, bool alarmActive, float distanceCm,
                                bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                const String &deviceName, float triggerDistanceCm,
                                bool inZone, bool blink) {
  const int cx = 64, cy = 32, maxR = 28;
  unsigned long cycleMs = 2400;
  for (int i = 0; i < 4; i++) {
    float phase = fmod((millis() + i * (cycleMs / 4)) / (float)cycleMs, 1.0f);
    int r = (int)(phase * maxR);
    if (r > 1 && r < maxR) oled.drawCircle(cx, cy, r, SSD1306_WHITE);
  }
  if (inZone && blink) oled.fillCircle(cx, cy, 3, SSD1306_WHITE);
  else oled.drawCircle(cx, cy, 2, SSD1306_WHITE);
  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 56, 1, SSD1306_WHITE);
  drawWifiIcon(2, 2, wifiConnected);
  drawBuzzerIcon(SCREEN_WIDTH - 16, 2, buzzerReachable);
}

// Skin 19 — Compass Dial. A compass-styled ring (N/E/S/W) with a needle
// that slowly sweeps as a "scanning" motion — decorative rather than a
// literal heading, since there's no magnetometer, but reads as an
// actively-searching instrument.
static void skinDrawCompassDial(bool armed, bool alarmActive, float distanceCm,
                                 bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                 const String &deviceName, float triggerDistanceCm,
                                 bool inZone, bool blink) {
  const int cx = 64, cy = 34, r = 24;
  oled.drawCircle(cx, cy, r, SSD1306_WHITE);
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(cx - 3, cy - r - 9); oled.print("N");
  oled.setCursor(cx - 3, cy + r + 1); oled.print("S");
  oled.setCursor(cx - r - 9, cy - 4); oled.print("W");
  oled.setCursor(cx + r + 1, cy - 4); oled.print("E");

  unsigned long periodMs = inZone ? 1200UL : 5000UL;
  float angle = fmod(millis() / (float)periodMs, 1.0f) * 2.0f * PI;
  oled.drawLine(cx, cy, cx + (int)(cos(angle) * (r - 4)), cy + (int)(sin(angle) * (r - 4)), SSD1306_WHITE);
  oled.drawLine(cx, cy, cx - (int)(cos(angle) * (r - 10)), cy - (int)(sin(angle) * (r - 10)), SSD1306_WHITE);
  oled.fillCircle(cx, cy, 2, SSD1306_WHITE);

  char buf[10];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.0f", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 58, 1, SSD1306_WHITE);
}

// Skin 20 — Pixel Guard. A simple blocky pixel-art guard/shield
// character built from filled rectangles — playful, and changes color
// weight (armed = solid/bold outline, disarmed = lighter/thinner) more
// than shape.
static void skinDrawPixelGuard(bool armed, bool alarmActive, float distanceCm,
                                bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                const String &deviceName, float triggerDistanceCm,
                                bool inZone, bool blink) {
  const int bx = 48, by = 6, s = 4; // block size
  bool solid = !alarmActive || blink;
  // Head
  if (solid) oled.fillRect(bx + s * 2, by, s * 4, s * 3, SSD1306_WHITE);
  else oled.drawRect(bx + s * 2, by, s * 4, s * 3, SSD1306_WHITE);
  // Body/shield
  if (solid) oled.fillRect(bx, by + s * 3, s * 8, s * 5, SSD1306_WHITE);
  else oled.drawRect(bx, by + s * 3, s * 8, s * 5, SSD1306_WHITE);
  // "Eyes" cut out only when armed, to suggest an alert/watching guard.
  if (armed) {
    oled.fillRect(bx + s * 3, by + s, s, s, SSD1306_BLACK);
    oled.fillRect(bx + s * 4, by + s, s, s, SSD1306_BLACK);
  }
  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 50, 1, SSD1306_WHITE);
  drawWifiIcon(2, 2, wifiConnected);
  drawBuzzerIcon(SCREEN_WIDTH - 16, 2, buzzerReachable);
}

// Skin 21 — Matrix Rain. Falling character-stream columns (cyberpunk
// "digital rain") in the background with the reading overlaid — purely
// decorative motion using simple falling dashes rather than real glyphs
// to stay cheap to redraw every loop.
static void skinDrawMatrixRain(bool armed, bool alarmActive, float distanceCm,
                                bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                const String &deviceName, float triggerDistanceCm,
                                bool inZone, bool blink) {
  const int cols = 16, colW = SCREEN_WIDTH / cols;
  for (int c = 0; c < cols; c++) {
    unsigned long seed = c * 97 + 13;
    int y = (int)((millis() / (10 + (seed % 15)) + seed * 5) % (SCREEN_HEIGHT + 10)) - 10;
    oled.drawFastVLine(c * colW + colW / 2, y, 5, SSD1306_WHITE);
  }
  // Solid backing box behind the text so it stays legible over the rain.
  oled.fillRect(14, 22, 100, 20, SSD1306_BLACK);
  oled.drawRect(14, 22, 100, 20, SSD1306_WHITE);
  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 28, 1, SSD1306_WHITE);
  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2); oled.print(badge);
}

// Skin 22 — Fingerprint Scan. Concentric partial arcs mimicking a
// fingerprint-scanner UI — evokes "identity verification" rather than
// distance measurement.
static void skinDrawFingerprintScan(bool armed, bool alarmActive, float distanceCm,
                                     bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                     const String &deviceName, float triggerDistanceCm,
                                     bool inZone, bool blink) {
  const int cx = 64, cy = 30;
  for (int i = 0; i < 5; i++) {
    int r = 6 + i * 4;
    // Partial arc (not a full circle) using drawCircleHelper quadrant masks.
    oled.drawCircleHelper(cx, cy, r, 0b0110, SSD1306_WHITE);
    oled.drawCircleHelper(cx, cy, r, 0b1001, SSD1306_WHITE);
  }
  if (inZone && blink) oled.fillCircle(cx, cy, 3, SSD1306_WHITE);
  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 56, 1, SSD1306_WHITE);
  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2); oled.print(badge);
}

// Skin 23 — Combination Lock. A rotating dial with tick marks, like a
// padlock combination wheel, with a fixed pointer at the top.
static void skinDrawCombinationLock(bool armed, bool alarmActive, float distanceCm,
                                     bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                     const String &deviceName, float triggerDistanceCm,
                                     bool inZone, bool blink) {
  const int cx = 64, cy = 32, r = 26;
  oled.drawCircle(cx, cy, r, SSD1306_WHITE);
  float rotation = armed ? (millis() / 4000.0f) : 0.0f;
  for (int i = 0; i < 20; i++) {
    float a = (2 * PI * i / 20.0f) + rotation;
    int x1p = cx + (int)(cos(a) * r), y1p = cy + (int)(sin(a) * r);
    int x2p = cx + (int)(cos(a) * (r - (i % 5 == 0 ? 6 : 3))), y2p = cy + (int)(sin(a) * (r - (i % 5 == 0 ? 6 : 3)));
    oled.drawLine(x1p, y1p, x2p, y2p, SSD1306_WHITE);
  }
  // Fixed pointer at top.
  oled.fillTriangle(cx - 4, cy - r - 6, cx + 4, cy - r - 6, cx, cy - r + 2, SSD1306_WHITE);
  char buf[10];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.0f", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, cy - 4, 1, SSD1306_WHITE);
  drawCenteredText(armed ? "LOCKED" : "UNLOCKED", 56, 1, SSD1306_WHITE);
}

// Skin 24 — Flame Alert. A simple animated flickering flame icon —
// deliberately reserved as a "danger/heat" visual metaphor, most
// striking while an alarm is active.
static void skinDrawFlameAlert(bool armed, bool alarmActive, float distanceCm,
                                bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                const String &deviceName, float triggerDistanceCm,
                                bool inZone, bool blink) {
  const int cx = 64, baseY = 46;
  unsigned long t = millis() / 120;
  int flick = (t % 3) - 1; // -1, 0, 1 flicker offset
  // Outer flame
  oled.fillTriangle(cx - 14, baseY, cx + 14, baseY, cx + flick, baseY - 30, SSD1306_WHITE);
  // Inner cutout for a hollow flame look (only when calm; solid when alarming for max visibility)
  if (!alarmActive) {
    oled.fillTriangle(cx - 7, baseY - 4, cx + 7, baseY - 4, cx + flick, baseY - 18, SSD1306_BLACK);
  }
  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 52, 1, SSD1306_WHITE);
  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2); oled.print(badge);
  drawWifiIcon(SCREEN_WIDTH - 16, 2, wifiConnected);
}

// Skin 25 — Frost Idle. A calm crystalline snowflake pattern — the
// deliberate visual counterpart to Flame Alert, for a cool/all-clear
// aesthetic rather than an urgent one.
static void skinDrawFrostIdle(bool armed, bool alarmActive, float distanceCm,
                               bool distanceValid, bool wifiConnected, bool buzzerReachable,
                               const String &deviceName, float triggerDistanceCm,
                               bool inZone, bool blink) {
  const int cx = 64, cy = 28, r = 18;
  for (int i = 0; i < 6; i++) {
    float a = i * PI / 3.0f;
    int ex = cx + (int)(cos(a) * r), ey = cy + (int)(sin(a) * r);
    oled.drawLine(cx, cy, ex, ey, SSD1306_WHITE);
    // Small branch ticks partway along each arm.
    int mx = cx + (int)(cos(a) * r * 0.6f), my = cy + (int)(sin(a) * r * 0.6f);
    float branchA1 = a + 0.5f, branchA2 = a - 0.5f;
    oled.drawLine(mx, my, mx + (int)(cos(branchA1) * 5), my + (int)(sin(branchA1) * 5), SSD1306_WHITE);
    oled.drawLine(mx, my, mx + (int)(cos(branchA2) * 5), my + (int)(sin(branchA2) * 5), SSD1306_WHITE);
  }
  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 54, 1, SSD1306_WHITE);
  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2); oled.print(badge);
}

// Skin 26 — Lightning Pulse. A jagged animated bolt that flashes
// brighter/thicker while an alarm is active.
static void skinDrawLightningPulse(bool armed, bool alarmActive, float distanceCm,
                                    bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                    const String &deviceName, float triggerDistanceCm,
                                    bool inZone, bool blink) {
  const int cx = 64;
  bool flash = alarmActive && blink;
  int pts[5][2] = {{cx - 6, 4}, {cx + 4, 20}, {cx - 4, 20}, {cx + 8, 44}, {cx - 2, 26}};
  for (int i = 0; i < 4; i++) {
    oled.drawLine(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], SSD1306_WHITE);
    if (flash) {
      oled.drawLine(pts[i][0] + 1, pts[i][1], pts[i + 1][0] + 1, pts[i + 1][1], SSD1306_WHITE);
    }
  }
  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 50, 1, SSD1306_WHITE);
  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2); oled.print(badge);
  drawWifiIcon(SCREEN_WIDTH - 16, 2, wifiConnected);
}

// Skin 27 — Star Field. Scattered twinkling dots — density and
// twinkle-speed increase once something's in the trigger zone.
static void skinDrawStarField(bool armed, bool alarmActive, float distanceCm,
                               bool distanceValid, bool wifiConnected, bool buzzerReachable,
                               const String &deviceName, float triggerDistanceCm,
                               bool inZone, bool blink) {
  const int starCount = 18;
  for (int i = 0; i < starCount; i++) {
    unsigned long seed = i * 733 + 91;
    int x = (seed * 37) % SCREEN_WIDTH;
    int y = 2 + (int)((seed * 53) % 44);
    unsigned long twinklePeriod = inZone ? 300 : 900;
    bool on = ((millis() + seed) / twinklePeriod) % 2 == 0;
    if (on) oled.drawPixel(x, y, SSD1306_WHITE);
  }
  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 50, 2, SSD1306_WHITE);
  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2); oled.print(badge);
}

// Skin 28 — Hourglass Timer. A sand-timer visual — the "sand" empties
// from the top chamber into the bottom one on a slow loop, purely as
// an ambient "time is passing / system is watching" motif.
static void skinDrawHourglassTimer(bool armed, bool alarmActive, float distanceCm,
                                    bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                    const String &deviceName, float triggerDistanceCm,
                                    bool inZone, bool blink) {
  const int cx = 64, topY = 4, midY = 28, botY = 52, halfW = 16;
  oled.drawLine(cx - halfW, topY, cx + halfW, topY, SSD1306_WHITE);
  oled.drawLine(cx - halfW, topY, cx, midY, SSD1306_WHITE);
  oled.drawLine(cx + halfW, topY, cx, midY, SSD1306_WHITE);
  oled.drawLine(cx, midY, cx - halfW, botY, SSD1306_WHITE);
  oled.drawLine(cx, midY, cx + halfW, botY, SSD1306_WHITE);
  oled.drawLine(cx - halfW, botY, cx + halfW, botY, SSD1306_WHITE);

  unsigned long cycleMs = 6000;
  float phase = fmod(millis() / (float)cycleMs, 1.0f);
  // Top chamber drains (shrinks) while bottom fills (grows).
  int topFill = (int)((midY - topY - 2) * (1.0f - phase));
  if (topFill > 0) {
    int w = (int)(halfW * ((float)topFill / (midY - topY)));
    oled.fillRect(cx - w, topY + 1, w * 2, topFill, SSD1306_WHITE);
  }
  int botFill = (int)((botY - midY - 2) * phase);
  if (botFill > 0) {
    int w = (int)(halfW * ((float)botFill / (botY - midY)));
    oled.fillRect(cx - w, botY - 1 - botFill, w * 2, botFill, SSD1306_WHITE);
  }
  char buf[10];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.0f", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 2);
  oled.print(alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM"));
  oled.setCursor(SCREEN_WIDTH - 30, 2);
  oled.print(buf);
}

// Skin 29 — Constellation. Connected dots forming a simple abstract
// network/constellation pattern — the Sensor Unit's counterpart to the
// Buzzer Unit's node-network theme, here styled purely decoratively
// since a single Sensor has no "other nodes" of its own to represent.
static void skinDrawConstellation(bool armed, bool alarmActive, float distanceCm,
                                   bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                   const String &deviceName, float triggerDistanceCm,
                                   bool inZone, bool blink) {
  const int pts[6][2] = {{20, 10}, {50, 6}, {80, 14}, {100, 30}, {60, 34}, {30, 32}};
  for (int i = 0; i < 6; i++) {
    oled.fillCircle(pts[i][0], pts[i][1], 2, SSD1306_WHITE);
    int next = (i + 1) % 6;
    oled.drawLine(pts[i][0], pts[i][1], pts[next][0], pts[next][1], SSD1306_WHITE);
  }
  char buf[12];
  if (distanceValid) snprintf(buf, sizeof(buf), "%.1fcm", distanceCm);
  else snprintf(buf, sizeof(buf), "--");
  drawCenteredText(buf, 46, 1, SSD1306_WHITE);
  const char* badge = alarmActive ? "ALARM" : (armed ? "ARMED" : "DISARM");
  drawCenteredText(badge, 56, 1, SSD1306_WHITE);
}

// =====================================================================
// PREMIUM PACK — pure ambient art. No text, no state badges, nothing
// tied to armed/alarm/wifi/etc — these are meant to just look good
// sitting on a shelf. State changes (armed/alarm) already have their
// own dedicated feedback (screen flash, buzzer, Telegram), so these
// skins deliberately don't duplicate that here.
// =====================================================================

// Premium 1 — Breathing Circle. A single ring that slowly grows and
// shrinks like a breath, ~4 second cycle. About as calm as this
// display can get.
static void skinDrawBreathingCircle(bool armed, bool alarmActive, float distanceCm,
                                     bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                     const String &deviceName, float triggerDistanceCm,
                                     bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int cx = 64, cy = 32;
  float phase = (millis() % 4000) / 4000.0f;
  float breath = (sin(phase * 2 * PI) + 1.0f) / 2.0f; // 0..1
  int r = 10 + (int)(breath * 16);
  oled.drawCircle(cx, cy, r, SSD1306_WHITE);
  oled.drawCircle(cx, cy, r > 2 ? r - 3 : 0, SSD1306_WHITE);
}

// Premium 2 — Ink Ripple. Concentric rings continuously born at the
// center and expand outward until they fade past the edge, staggered
// so 3 are always mid-flight, like drops landing in still water.
static void skinDrawInkRipple(bool armed, bool alarmActive, float distanceCm,
                               bool distanceValid, bool wifiConnected, bool buzzerReachable,
                               const String &deviceName, float triggerDistanceCm,
                               bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int cx = 64, cy = 32, maxR = 34;
  const unsigned long cycleMs = 3000;
  for (int i = 0; i < 3; i++) {
    unsigned long offset = (cycleMs / 3) * i;
    float phase = ((millis() + offset) % cycleMs) / (float)cycleMs;
    int r = (int)(phase * maxR);
    if (r > 1) oled.drawCircle(cx, cy, r, SSD1306_WHITE);
  }
}

// Premium 3 — Moon Phases. A moon disc where a shadow terminator
// slides slowly across it on a long, unhurried cycle, waxing and
// waning like a real lunar month, just sped up to something you'd
// actually see happen while glancing at it over time.
static void skinDrawMoonPhases(bool armed, bool alarmActive, float distanceCm,
                                bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                const String &deviceName, float triggerDistanceCm,
                                bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int cx = 64, cy = 32, r = 20;
  const unsigned long cycleMs = 24000; // a full "month" every 24s
  float phase = (millis() % cycleMs) / (float)cycleMs; // 0..1
  oled.fillCircle(cx, cy, r, SSD1306_WHITE);
  // Shadow is an ellipse sliding across the disc, width tracks phase.
  float shadowX = cos(phase * 2 * PI); // -1..1
  int shadowW = (int)(r * 2 * fabs(shadowX));
  if (phase < 0.5f) {
    // Waxing to waning through full — shadow eats from one side.
    oled.fillRect(shadowX < 0 ? cx - r : cx, cy - r, shadowW, r * 2, SSD1306_BLACK);
  } else {
    oled.fillRect(shadowX < 0 ? cx - r : cx, cy - r, shadowW, r * 2, SSD1306_BLACK);
  }
}

// Premium 4 — Aurora Waves. Three overlapping horizontal sine-waves
// drifting sideways at different speeds and amplitudes near the top
// third of the screen, like curtains of light.
static void skinDrawAuroraWaves(bool armed, bool alarmActive, float distanceCm,
                                 bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                 const String &deviceName, float triggerDistanceCm,
                                 bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const float layers[3][3] = {
    {18.0f, 6.0f, 40.0f},  // baseY, amplitude, speedDivisor
    {32.0f, 9.0f, 65.0f},
    {46.0f, 5.0f, 90.0f},
  };
  for (int layer = 0; layer < 3; layer++) {
    float baseY = layers[layer][0], amp = layers[layer][1], speed = layers[layer][2];
    int prevX = 0, prevY = (int)baseY;
    for (int x = 0; x <= 128; x += 3) {
      float t = (x / speed) + (millis() / (speed * 30.0f));
      int y = (int)(baseY + sin(t) * amp);
      if (x > 0) oled.drawLine(prevX, prevY, x, y, SSD1306_WHITE);
      prevX = x; prevY = y;
    }
  }
}

// Premium 5 — Cassette Reels. Two spoked wheels side by side, each
// slowly rotating, with a "tape" line connecting them, like watching
// a cassette play through the little viewing window.
static void skinDrawCassetteReels(bool armed, bool alarmActive, float distanceCm,
                                   bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                   const String &deviceName, float triggerDistanceCm,
                                   bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int cy = 32, r = 16, leftX = 34, rightX = 94;
  float angle = (millis() / 800.0f);
  oled.drawLine(leftX + r - 2, cy, rightX - r + 2, cy, SSD1306_WHITE);
  int centers[2] = {leftX, rightX};
  for (int w = 0; w < 2; w++) {
    int cx = centers[w];
    oled.drawCircle(cx, cy, r, SSD1306_WHITE);
    oled.drawCircle(cx, cy, 4, SSD1306_WHITE);
    for (int i = 0; i < 5; i++) {
      float a = angle + (i * 2 * PI / 5);
      oled.drawLine(cx, cy, cx + (int)(cos(a) * (r - 2)), cy + (int)(sin(a) * (r - 2)), SSD1306_WHITE);
    }
  }
}

// Premium 6 — Analog Tuning Dial. A radio-style arc scale with a
// needle that sweeps slowly back and forth, like someone endlessly,
// unhurriedly searching the dial.
static void skinDrawTuningDial(bool armed, bool alarmActive, float distanceCm,
                                bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                const String &deviceName, float triggerDistanceCm,
                                bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int cx = 64, cy = 50, r = 40;
  const float startDeg = 200, sweepDeg = 140;
  for (int i = 0; i <= 14; i++) {
    float a = (startDeg + sweepDeg * i / 14.0f) * PI / 180.0f;
    int len = (i % 2 == 0) ? 7 : 4;
    oled.drawLine(cx + (int)(cos(a) * r), cy + (int)(sin(a) * r),
                  cx + (int)(cos(a) * (r - len)), cy + (int)(sin(a) * (r - len)), SSD1306_WHITE);
  }
  float phase = (sin(millis() / 2200.0f) + 1.0f) / 2.0f; // 0..1, slow back-and-forth
  float needleA = (startDeg + sweepDeg * phase) * PI / 180.0f;
  oled.drawLine(cx, cy, cx + (int)(cos(needleA) * (r - 5)), cy + (int)(sin(needleA) * (r - 5)), SSD1306_WHITE);
  oled.fillCircle(cx, cy, 2, SSD1306_WHITE);
}

// Premium 7 — Holographic Grid. A perspective floor-grid that scrolls
// steadily "toward" the viewer, sci-fi HUD aesthetic.
static void skinDrawHoloGrid(bool armed, bool alarmActive, float distanceCm,
                              bool distanceValid, bool wifiConnected, bool buzzerReachable,
                              const String &deviceName, float triggerDistanceCm,
                              bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int horizonY = 14, vanishX = 64;
  // Converging side rails.
  oled.drawLine(vanishX, horizonY, 0, 64, SSD1306_WHITE);
  oled.drawLine(vanishX, horizonY, 128, 64, SSD1306_WHITE);
  // Horizontal rungs, spaced closer near the horizon, scrolling down over time.
  float scroll = (millis() % 1200) / 1200.0f;
  for (int i = 0; i < 6; i++) {
    float t = (i + scroll) / 6.0f; // 0 near horizon .. 1 near viewer
    int y = horizonY + (int)(t * t * (64 - horizonY));
    int halfW = (int)(t * (128 - vanishX));
    oled.drawLine(vanishX - halfW, y, vanishX + halfW, y, SSD1306_WHITE);
  }
}

// Premium 8 — Orbital Rings. Concentric rings, each with one small
// dot orbiting at its own speed, a lazy little solar system.
static void skinDrawOrbitalRingsPremium(bool armed, bool alarmActive, float distanceCm,
                                         bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                         const String &deviceName, float triggerDistanceCm,
                                         bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int cx = 64, cy = 32;
  const int radii[3] = {10, 20, 30};
  const float speeds[3] = {1800.0f, 3400.0f, 5200.0f};
  oled.fillCircle(cx, cy, 3, SSD1306_WHITE);
  for (int i = 0; i < 3; i++) {
    oled.drawCircle(cx, cy, radii[i], SSD1306_WHITE);
    float a = (millis() / speeds[i]) * 2 * PI;
    oled.fillCircle(cx + (int)(cos(a) * radii[i]), cy + (int)(sin(a) * radii[i]), 2, SSD1306_WHITE);
  }
}

// Premium 9 — Celestial Drift. A small field of stars that slowly
// rotates as one body around the screen center, twinkling gently.
// Distinct from the free-tier "Constellation" skin (static dot-to-dot
// shape) — this one has no fixed shape, just a drifting starfield.
static void skinDrawCelestialDrift(bool armed, bool alarmActive, float distanceCm,
                                    bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                    const String &deviceName, float triggerDistanceCm,
                                    bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int cx = 64, cy = 32;
  const int starCount = 14;
  float rotation = millis() / 9000.0f; // one full slow rotation ~56s
  for (int i = 0; i < starCount; i++) {
    unsigned long seed = i * 977 + 131;
    float baseAngle = (seed % 360) * PI / 180.0f;
    float dist = 8 + (seed % 26);
    float a = baseAngle + rotation;
    int x = cx + (int)(cos(a) * dist);
    int y = cy + (int)(sin(a) * dist * 0.6f); // slightly flattened, less circular-looking
    bool on = ((millis() + seed) / (400 + (seed % 300))) % 2 == 0;
    if (on) oled.drawPixel(x, y, SSD1306_WHITE);
  }
}

// Premium 10 — Night Guardian. An original bat silhouette (not any
// trademarked emblem — natural wing anatomy, not a stylized logo)
// that gently flaps in a soft glow, front-and-center, dark/heroic mood.
static void skinDrawNightGuardian(bool armed, bool alarmActive, float distanceCm,
                                   bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                   const String &deviceName, float triggerDistanceCm,
                                   bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int cx = 64, cy = 30;
  // Soft glow rings behind, slowly breathing.
  float breath = (sin(millis() / 1600.0f) + 1.0f) / 2.0f;
  int glowR = 26 + (int)(breath * 4);
  oled.drawCircle(cx, cy, glowR, SSD1306_WHITE);
  // Wings flap via a vertical scale factor on the wingtip Y offset.
  float flap = sin(millis() / 500.0f); // -1..1, faster than the glow
  int wingDrop = (int)(flap * 6);
  // Body
  oled.fillTriangle(cx - 3, cy - 4, cx + 3, cy - 4, cx, cy + 8, SSD1306_WHITE);
  // Left wing (3-point wing shape, tip moves with flap)
  oled.drawLine(cx - 3, cy - 2, cx - 24, cy - 2 + wingDrop, SSD1306_WHITE);
  oled.drawLine(cx - 24, cy - 2 + wingDrop, cx - 14, cy + 6, SSD1306_WHITE);
  oled.drawLine(cx - 14, cy + 6, cx - 3, cy, SSD1306_WHITE);
  // Right wing (mirrored)
  oled.drawLine(cx + 3, cy - 2, cx + 24, cy - 2 + wingDrop, SSD1306_WHITE);
  oled.drawLine(cx + 24, cy - 2 + wingDrop, cx + 14, cy + 6, SSD1306_WHITE);
  oled.drawLine(cx + 14, cy + 6, cx + 3, cy, SSD1306_WHITE);
}

// Premium 11 — Floating Particles. A handful of soft dust-motes drift
// slowly in straight lines, wrapping around the edges when they exit.
static void skinDrawFloatingParticles(bool armed, bool alarmActive, float distanceCm,
                                       bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                       const String &deviceName, float triggerDistanceCm,
                                       bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int count = 9;
  for (int i = 0; i < count; i++) {
    unsigned long seed = i * 733 + 91;
    float speedX = 6.0f + (seed % 5);           // px per "unit time"
    float speedY = 3.0f + ((seed / 5) % 4);
    float dirX = (seed % 2 == 0) ? 1.0f : -1.0f;
    float dirY = ((seed / 2) % 2 == 0) ? 1.0f : -1.0f;
    float t = millis() / 1000.0f;
    int x = ((int)(seed % 128) + (int)(t * speedX * dirX));
    int y = ((int)((seed * 7) % 64) + (int)(t * speedY * dirY));
    x = ((x % 128) + 128) % 128;
    y = ((y % 64) + 64) % 64;
    oled.drawPixel(x, y, SSD1306_WHITE);
    if ((seed % 3) == 0) oled.drawPixel(x + 1, y, SSD1306_WHITE); // a few slightly bigger motes
  }
}

// Premium 12 — Growing Tree. A simple line-art tree, trunk fixed,
// branches gently swaying like a light breeze.
static void skinDrawGrowingTree(bool armed, bool alarmActive, float distanceCm,
                                 bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                 const String &deviceName, float triggerDistanceCm,
                                 bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int baseX = 64, baseY = 58;
  float sway = sin(millis() / 1800.0f) * 3.0f;
  oled.drawLine(baseX, baseY, baseX, baseY - 24, SSD1306_WHITE); // trunk
  // Branch pairs, each higher pair sways a bit more than the one below.
  struct Branch { int fromY; int len; float swayMul; };
  Branch branches[4] = {{baseY - 8, 14, 0.6f}, {baseY - 14, 16, 0.9f}, {baseY - 19, 14, 1.2f}, {baseY - 23, 10, 1.5f}};
  for (int i = 0; i < 4; i++) {
    float s = sway * branches[i].swayMul;
    int fx = baseX, fy = branches[i].fromY;
    oled.drawLine(fx, fy, fx - branches[i].len + (int)s, fy - branches[i].len / 2, SSD1306_WHITE);
    oled.drawLine(fx, fy, fx + branches[i].len + (int)s, fy - branches[i].len / 2, SSD1306_WHITE);
  }
}

// Premium 13 — Falling Leaves. A few small leaf-shapes drift downward
// in a gentle zigzag, looping back to the top once off-screen.
static void skinDrawFallingLeaves(bool armed, bool alarmActive, float distanceCm,
                                   bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                   const String &deviceName, float triggerDistanceCm,
                                   bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int count = 5;
  for (int i = 0; i < count; i++) {
    unsigned long seed = i * 611 + 47;
    float fallSpeed = 10.0f + (seed % 6);
    unsigned long cycleMs = (unsigned long)((64.0f + 20) / fallSpeed * 1000);
    unsigned long offset = (cycleMs / count) * i;
    float phase = ((millis() + offset) % cycleMs) / (float)cycleMs;
    int y = (int)(phase * (64 + 20)) - 10;
    int baseX = (int)(seed % 118) + 5;
    int x = baseX + (int)(sin(phase * 6 * PI) * 10); // zigzag drift
    // Tiny diamond leaf shape.
    oled.drawLine(x, y - 2, x + 2, y, SSD1306_WHITE);
    oled.drawLine(x + 2, y, x, y + 2, SSD1306_WHITE);
    oled.drawLine(x, y + 2, x - 2, y, SSD1306_WHITE);
    oled.drawLine(x - 2, y, x, y - 2, SSD1306_WHITE);
  }
}

// Premium 14 — VHS Static. Horizontal scanlines with an occasional
// glitch-band that jitters sideways, classic old-tape aesthetic.
static void skinDrawVhsStatic(bool armed, bool alarmActive, float distanceCm,
                               bool distanceValid, bool wifiConnected, bool buzzerReachable,
                               const String &deviceName, float triggerDistanceCm,
                               bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  for (int y = 4; y < 60; y += 3) {
    oled.drawLine(0, y, 128, y, SSD1306_WHITE);
  }
  // A glitch band sweeps down slowly, jittering horizontally each pass.
  unsigned long cycle = millis() % 5000;
  int glitchY = (int)((cycle / 5000.0f) * 64);
  unsigned long jitterSeed = millis() / 80;
  int jitterX = (int)(jitterSeed % 7) - 3;
  oled.fillRect(jitterX, glitchY, 128, 4, SSD1306_BLACK);
  oled.drawLine(jitterX, glitchY, jitterX + 128, glitchY, SSD1306_WHITE);
}

// Premium 15 — Pixel Arcade. Blocky retro corner-brackets framing the
// screen, pulsing on/off like an 8-bit game's "insert coin" border.
static void skinDrawPixelArcade(bool armed, bool alarmActive, float distanceCm,
                                 bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                 const String &deviceName, float triggerDistanceCm,
                                 bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  bool on = ((millis() / 500) % 2) == 0;
  if (on) {
    const int cornerLen = 14, inset = 3;
    // 4 corner brackets, blocky (2px thick via double lines).
    int corners[4][2] = {{inset, inset}, {128 - inset, inset}, {inset, 64 - inset}, {128 - inset, 64 - inset}};
    for (int c = 0; c < 4; c++) {
      int cx = corners[c][0], cy = corners[c][1];
      int dx = (c % 2 == 0) ? 1 : -1;
      int dy = (c < 2) ? 1 : -1;
      oled.drawLine(cx, cy, cx + cornerLen * dx, cy, SSD1306_WHITE);
      oled.drawLine(cx, cy, cx, cy + cornerLen * dy, SSD1306_WHITE);
      oled.drawLine(cx, cy + dy, cx + cornerLen * dx, cy + dy, SSD1306_WHITE);
      oled.drawLine(cx + dx, cy, cx + dx, cy + cornerLen * dy, SSD1306_WHITE);
    }
  }
  // Center pixel-star, always visible, small blocky burst.
  const int cx = 64, cy = 32;
  oled.fillRect(cx - 1, cy - 1, 3, 3, SSD1306_WHITE);
  oled.drawPixel(cx - 3, cy, SSD1306_WHITE);
  oled.drawPixel(cx + 3, cy, SSD1306_WHITE);
  oled.drawPixel(cx, cy - 3, SSD1306_WHITE);
  oled.drawPixel(cx, cy + 3, SSD1306_WHITE);
}

// Premium 16 — Data Stream. Vertical columns of falling dashes at
// staggered speeds, an abstracted "digital rain" (no characters, just
// motion, since this display has no room for tiny readable glyphs).
static void skinDrawDataStream(bool armed, bool alarmActive, float distanceCm,
                                bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                const String &deviceName, float triggerDistanceCm,
                                bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int columns = 12;
  for (int c = 0; c < columns; c++) {
    int x = c * (128 / columns) + 4;
    unsigned long seed = c * 419 + 61;
    float speed = 24.0f + (seed % 20);
    unsigned long cycleMs = (unsigned long)((64.0f + 16) / speed * 1000);
    unsigned long offset = (seed % cycleMs);
    float phase = ((millis() + offset) % cycleMs) / (float)cycleMs;
    int headY = (int)(phase * (64 + 16)) - 8;
    // A short streak trailing above the head, fading via gaps.
    for (int t = 0; t < 4; t++) {
      int y = headY - t * 3;
      if (y >= 0 && y < 64 && (t == 0 || (seed + t) % 2 == 0)) {
        oled.drawPixel(x, y, SSD1306_WHITE);
      }
    }
  }
}

// Premium 17 — Sleepy Cat. A simple line-art cat, curled and resting,
// chest gently rising with breath and an occasional slow blink.
static void skinDrawSleepyCat(bool armed, bool alarmActive, float distanceCm,
                               bool distanceValid, bool wifiConnected, bool buzzerReachable,
                               const String &deviceName, float triggerDistanceCm,
                               bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int cx = 64, cy = 40;
  float breath = sin(millis() / 1400.0f);
  int bodyR = 20 + (int)(breath * 1.5f);
  oled.drawCircle(cx, cy, bodyR, SSD1306_WHITE); // curled body
  // Head, slightly above-left of body center.
  const int headX = cx - 6, headY = cy - 18;
  oled.drawCircle(headX, headY, 9, SSD1306_WHITE);
  // Ears (triangles).
  oled.drawLine(headX - 7, headY - 5, headX - 3, headY - 13, SSD1306_WHITE);
  oled.drawLine(headX - 3, headY - 13, headX - 1, headY - 6, SSD1306_WHITE);
  oled.drawLine(headX + 2, headY - 8, headX + 5, headY - 14, SSD1306_WHITE);
  oled.drawLine(headX + 5, headY - 14, headX + 7, headY - 7, SSD1306_WHITE);
  // Eyes — slow blink every ~5s.
  bool eyesClosed = (millis() % 5000) > 4700;
  if (eyesClosed) {
    oled.drawLine(headX - 4, headY, headX - 1, headY, SSD1306_WHITE);
    oled.drawLine(headX + 2, headY, headX + 5, headY, SSD1306_WHITE);
  } else {
    oled.drawPixel(headX - 3, headY, SSD1306_WHITE);
    oled.drawPixel(headX + 3, headY, SSD1306_WHITE);
  }
  // Tail curling around, fixed shape (sleeping, doesn't need to move much).
  oled.drawLine(cx + bodyR - 4, cy + 4, cx + bodyR + 4, cy - 6, SSD1306_WHITE);
}

// Premium 18 — Little Robot Buddy. A friendly blocky robot face, one
// blinking eye-pair and a gently swaying antenna.
static void skinDrawRobotBuddy(bool armed, bool alarmActive, float distanceCm,
                                bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                const String &deviceName, float triggerDistanceCm,
                                bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int cx = 64, cy = 34, headW = 34, headH = 26;
  // Antenna, swaying gently.
  float sway = sin(millis() / 900.0f) * 4.0f;
  int antX = cx, antY = cy - headH / 2;
  oled.drawLine(antX, antY, antX + (int)sway, antY - 12, SSD1306_WHITE);
  oled.drawCircle(antX + (int)sway, antY - 14, 2, SSD1306_WHITE);
  // Head (rounded rect via rect + corner adjustment isn't available, use plain rect).
  oled.drawRoundRect(cx - headW / 2, cy - headH / 2, headW, headH, 5, SSD1306_WHITE);
  // Eyes — blink every ~4s.
  bool eyesClosed = (millis() % 4000) > 3800;
  int eyeY = cy - 2;
  if (eyesClosed) {
    oled.drawLine(cx - 9, eyeY, cx - 4, eyeY, SSD1306_WHITE);
    oled.drawLine(cx + 4, eyeY, cx + 9, eyeY, SSD1306_WHITE);
  } else {
    oled.fillCircle(cx - 7, eyeY, 3, SSD1306_WHITE);
    oled.fillCircle(cx + 7, eyeY, 3, SSD1306_WHITE);
  }
  // Mouth, a simple fixed friendly line.
  oled.drawLine(cx - 6, cy + 8, cx + 6, cy + 8, SSD1306_WHITE);
  // Side bolts for character.
  oled.fillCircle(cx - headW / 2 - 2, cy, 1, SSD1306_WHITE);
  oled.fillCircle(cx + headW / 2 + 2, cy, 1, SSD1306_WHITE);
}

// Premium 19 — Swiss Grid. A clean Bauhaus-style grid of rectangles
// with one accent block that slowly drifts between grid positions.
static void skinDrawSwissGrid(bool armed, bool alarmActive, float distanceCm,
                               bool distanceValid, bool wifiConnected, bool buzzerReachable,
                               const String &deviceName, float triggerDistanceCm,
                               bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int cols = 6, rows = 3, cellW = 128 / cols, cellH = 64 / rows;
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      oled.drawRect(c * cellW, r * cellH, cellW - 2, cellH - 2, SSD1306_WHITE);
    }
  }
  // The accent block moves through cells slowly, one cell every 1.5s.
  int totalCells = cols * rows;
  int idx = (int)((millis() / 1500) % totalCells);
  int ar = idx / cols, ac = idx % cols;
  oled.fillRect(ac * cellW, ar * cellH, cellW - 2, cellH - 2, SSD1306_WHITE);
}

// Premium 20 — Orbiting Planets. A single planet on a slow elliptical
// orbit around a fixed sun, trailing a faint orbit-path ring.
static void skinDrawOrbitingPlanets(bool armed, bool alarmActive, float distanceCm,
                                     bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                     const String &deviceName, float triggerDistanceCm,
                                     bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int cx = 64, cy = 32, orbitRx = 40, orbitRy = 16;
  oled.fillCircle(cx, cy, 6, SSD1306_WHITE); // sun
  // Orbit path, drawn as a dashed ellipse approximation.
  for (int i = 0; i < 36; i += 2) {
    float a = i * 10 * PI / 180.0f;
    oled.drawPixel(cx + (int)(cos(a) * orbitRx), cy + (int)(sin(a) * orbitRy), SSD1306_WHITE);
  }
  float a = millis() / 4000.0f;
  int px = cx + (int)(cos(a) * orbitRx);
  int py = cy + (int)(sin(a) * orbitRy);
  oled.fillCircle(px, py, 3, SSD1306_WHITE);
}

// Premium 21 — Soft Pulse Dot. The simplest possible skin: one center
// dot with a halo that expands and fades, endlessly, about as
// minimal and calming as this display can get.
static void skinDrawSoftPulseDot(bool armed, bool alarmActive, float distanceCm,
                                  bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                  const String &deviceName, float triggerDistanceCm,
                                  bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int cx = 64, cy = 32;
  oled.fillCircle(cx, cy, 3, SSD1306_WHITE);
  const unsigned long cycleMs = 2600;
  float phase = (millis() % cycleMs) / (float)cycleMs;
  int haloR = 3 + (int)(phase * 24);
  if (phase < 0.85f) oled.drawCircle(cx, cy, haloR, SSD1306_WHITE);
}

// Premium 22 — Raindrop Ripples. Drops fall from the top edge at
// staggered points and moments; each lands and spawns an expanding
// ripple ring at that spot.
static void skinDrawRaindropRipples(bool armed, bool alarmActive, float distanceCm,
                                     bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                     const String &deviceName, float triggerDistanceCm,
                                     bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int drops = 3;
  const unsigned long cycleMs = 2400;
  for (int i = 0; i < drops; i++) {
    unsigned long seed = i * 853 + 29;
    unsigned long offset = (cycleMs / drops) * i;
    float phase = ((millis() + offset) % cycleMs) / (float)cycleMs;
    int landX = 24 + (int)(seed % 80);
    int landY = 16 + (int)((seed * 3) % 32);
    if (phase < 0.35f) {
      // Falling drop, a short vertical dash approaching landY.
      int dropY = (int)(phase / 0.35f * landY);
      oled.drawLine(landX, dropY > 3 ? dropY - 3 : 0, landX, dropY, SSD1306_WHITE);
    } else {
      // Expanding ripple after landing.
      float rp = (phase - 0.35f) / 0.65f;
      int r = (int)(rp * 18);
      if (r > 0) oled.drawCircle(landX, landY, r, SSD1306_WHITE);
    }
  }
}

// Premium 23 — Dot Matrix Print. A horizontal line sweeps down the
// screen, "printing" a fixed dot-pattern row by row as it passes,
// old dot-matrix-printer style, then resets and does it again.
static void skinDrawDotMatrixPrint(bool armed, bool alarmActive, float distanceCm,
                                    bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                    const String &deviceName, float triggerDistanceCm,
                                    bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const unsigned long cycleMs = 3500;
  float phase = (millis() % cycleMs) / (float)cycleMs;
  int headY = (int)(phase * 64);
  // Fixed dot-grid pattern, only drawn up to the print-head's current row.
  for (int y = 4; y <= headY; y += 6) {
    for (int x = 8; x < 120; x += 8) {
      unsigned long seed = (x * 7 + y * 13);
      if ((seed % 3) != 0) oled.drawPixel(x, y, SSD1306_WHITE);
    }
  }
  // Print head itself, a small solid bar.
  oled.drawLine(0, headY, 128, headY, SSD1306_WHITE);
}

// Premium 24 — Cyberpunk Terminal. Neon corner-brackets framing the
// screen with a scanline sweeping through, HUD-style, slightly edgy.
static void skinDrawCyberpunkTerminal(bool armed, bool alarmActive, float distanceCm,
                                       bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                       const String &deviceName, float triggerDistanceCm,
                                       bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int inset = 5, bracketLen = 16;
  int corners[4][2] = {{inset, inset}, {128 - inset, inset}, {inset, 64 - inset}, {128 - inset, 64 - inset}};
  for (int c = 0; c < 4; c++) {
    int cx = corners[c][0], cy = corners[c][1];
    int dx = (c % 2 == 0) ? 1 : -1;
    int dy = (c < 2) ? 1 : -1;
    oled.drawLine(cx, cy, cx + bracketLen * dx, cy, SSD1306_WHITE);
    oled.drawLine(cx, cy, cx, cy + bracketLen * dy, SSD1306_WHITE);
  }
  // Center diamond accent.
  const int mx = 64, my = 32;
  oled.drawLine(mx, my - 6, mx + 6, my, SSD1306_WHITE);
  oled.drawLine(mx + 6, my, mx, my + 6, SSD1306_WHITE);
  oled.drawLine(mx, my + 6, mx - 6, my, SSD1306_WHITE);
  oled.drawLine(mx - 6, my, mx, my - 6, SSD1306_WHITE);
  // Scanline sweeping top to bottom.
  int scanY = (int)((millis() % 2000) / 2000.0f * 64);
  oled.drawLine(inset, scanY, 128 - inset, scanY, SSD1306_WHITE);
}

// Premium 25 — Watchdog. A simple dog silhouette, ears perked, tail
// giving a slow, friendly wag.
static void skinDrawWatchdog(bool armed, bool alarmActive, float distanceCm,
                              bool distanceValid, bool wifiConnected, bool buzzerReachable,
                              const String &deviceName, float triggerDistanceCm,
                              bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int hx = 54, hy = 30; // head center
  oled.drawCircle(hx, hy, 10, SSD1306_WHITE);
  // Ears, perked triangles.
  oled.drawLine(hx - 8, hy - 6, hx - 12, hy - 16, SSD1306_WHITE);
  oled.drawLine(hx - 12, hy - 16, hx - 3, hy - 9, SSD1306_WHITE);
  oled.drawLine(hx + 6, hy - 8, hx + 11, hy - 17, SSD1306_WHITE);
  oled.drawLine(hx + 11, hy - 17, hx + 9, hy - 6, SSD1306_WHITE);
  // Snout.
  oled.drawLine(hx + 8, hy + 2, hx + 16, hy + 4, SSD1306_WHITE);
  oled.drawLine(hx + 16, hy + 4, hx + 16, hy + 8, SSD1306_WHITE);
  oled.drawLine(hx + 16, hy + 8, hx + 7, hy + 7, SSD1306_WHITE);
  oled.fillCircle(hx + 1, hy - 2, 1, SSD1306_WHITE); // eye
  // Body, simple oval-ish via ellipse-by-lines.
  oled.drawLine(hx - 8, hy + 8, hx - 22, hy + 16, SSD1306_WHITE);
  oled.drawLine(hx - 22, hy + 16, hx - 24, hy + 34, SSD1306_WHITE);
  oled.drawLine(hx - 24, hy + 34, hx + 10, hy + 34, SSD1306_WHITE);
  oled.drawLine(hx + 10, hy + 34, hx + 8, hy + 8, SSD1306_WHITE);
  // Tail, wagging left-right slowly at the fixed pivot.
  float wag = sin(millis() / 550.0f) * 14.0f;
  oled.drawLine(hx - 22, hy + 18, hx - 22 + (int)wag, hy + 4, SSD1306_WHITE);
}

// Premium 26 — Minimalist Gauge. One clean thin arc with a slow needle
// sweep, nothing else on screen, ultra restrained.
static void skinDrawMinimalistGauge(bool armed, bool alarmActive, float distanceCm,
                                     bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                     const String &deviceName, float triggerDistanceCm,
                                     bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int cx = 64, cy = 36, r = 26;
  const float startDeg = 160, sweepDeg = 220;
  for (int i = 0; i <= 60; i++) {
    float a = (startDeg + sweepDeg * i / 60.0f) * PI / 180.0f;
    int x = cx + (int)(cos(a) * r), y = cy + (int)(sin(a) * r);
    oled.drawPixel(x, y, SSD1306_WHITE);
  }
  float phase = (sin(millis() / 2600.0f) + 1.0f) / 2.0f;
  float needleA = (startDeg + sweepDeg * phase) * PI / 180.0f;
  oled.drawLine(cx, cy, cx + (int)(cos(needleA) * (r - 4)), cy + (int)(sin(needleA) * (r - 4)), SSD1306_WHITE);
  oled.fillCircle(cx, cy, 2, SSD1306_WHITE);
}

// Premium 27 — Corporate Ticker. An abstract horizontal bar-chart
// ticker that scrolls continuously right to left, stock-exchange feel
// without any actual numbers (this display can't render them small
// enough to be worth including).
static void skinDrawCorporateTicker(bool armed, bool alarmActive, float distanceCm,
                                     bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                     const String &deviceName, float triggerDistanceCm,
                                     bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  oled.drawLine(0, 34, 128, 34, SSD1306_WHITE); // baseline
  int scrollOffset = (int)((millis() / 30) % 160);
  const int barCount = 16, spacing = 10;
  for (int i = -1; i < barCount; i++) {
    int x = i * spacing - scrollOffset + 160;
    x = ((x % 160) + 160) % 160 - 16;
    unsigned long seed = (x / spacing) * 331 + 71;
    int h = 3 + (int)(seed % 20);
    bool up = (seed % 2) == 0;
    if (up) oled.drawLine(x, 34, x, 34 - h, SSD1306_WHITE);
    else oled.drawLine(x, 34, x, 34 + h, SSD1306_WHITE);
  }
}

// Premium 28 — Architectural Blueprint. A technical-drawing grid with
// a simple house/structure outline that "draws itself" repeatedly, a
// pen tracing the same shape on loop.
static void skinDrawBlueprint(bool armed, bool alarmActive, float distanceCm,
                               bool distanceValid, bool wifiConnected, bool buzzerReachable,
                               const String &deviceName, float triggerDistanceCm,
                               bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  // Faint background grid.
  for (int x = 0; x < 128; x += 16) oled.drawLine(x, 0, x, 64, SSD1306_WHITE);
  for (int y = 0; y < 64; y += 16) oled.drawLine(0, y, 128, y, SSD1306_WHITE);
  // A simple house outline, traced progressively over a 3s cycle.
  const int pts[6][2] = {{44, 44}, {44, 24}, {64, 10}, {84, 24}, {84, 44}, {44, 44}};
  const unsigned long cycleMs = 3000;
  float phase = (millis() % cycleMs) / (float)cycleMs;
  float totalSegs = 5.0f;
  float drawnSegs = phase * totalSegs;
  for (int i = 0; i < 5; i++) {
    if (drawnSegs >= i + 1) {
      oled.drawLine(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], SSD1306_WHITE);
    } else if (drawnSegs > i) {
      float segPhase = drawnSegs - i;
      int mx = pts[i][0] + (int)((pts[i + 1][0] - pts[i][0]) * segPhase);
      int my = pts[i][1] + (int)((pts[i + 1][1] - pts[i][1]) * segPhase);
      oled.drawLine(pts[i][0], pts[i][1], mx, my, SSD1306_WHITE);
    }
  }
}

// Premium 29 — Zen Sand Garden. Raked sand-lines that slowly shift
// their wave pattern, like a rock garden being re-raked over time.
static void skinDrawZenGarden(bool armed, bool alarmActive, float distanceCm,
                               bool distanceValid, bool wifiConnected, bool buzzerReachable,
                               const String &deviceName, float triggerDistanceCm,
                               bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  float drift = millis() / 3000.0f;
  for (int row = 0; row < 8; row++) {
    int y = row * 8 + 4;
    int prevX = 0, prevY = y;
    for (int x = 0; x <= 128; x += 4) {
      float t = (x / 20.0f) + drift + row * 0.3f;
      int wy = y + (int)(sin(t) * 3);
      if (x > 0) oled.drawLine(prevX, prevY, x, wy, SSD1306_WHITE);
      prevX = x; prevY = wy;
    }
  }
  // A "stone" circle sitting still among the raked lines.
  oled.fillCircle(96, 20, 5, SSD1306_BLACK);
  oled.drawCircle(96, 20, 5, SSD1306_WHITE);
}

// Premium 30 — Polar Aurora. Vertical drifting light-curtains (as
// opposed to Aurora Waves' horizontal layers), each ribbon swaying
// independently top to bottom.
static void skinDrawPolarAurora(bool armed, bool alarmActive, float distanceCm,
                                 bool distanceValid, bool wifiConnected, bool buzzerReachable,
                                 const String &deviceName, float triggerDistanceCm,
                                 bool inZone, bool blink) {
  (void)armed; (void)alarmActive; (void)distanceCm; (void)distanceValid;
  (void)wifiConnected; (void)buzzerReachable; (void)deviceName; (void)triggerDistanceCm;
  (void)inZone; (void)blink;
  const int ribbons = 5;
  for (int i = 0; i < ribbons; i++) {
    float baseX = 14 + i * 25;
    float speed = 1400.0f + i * 260.0f;
    int prevX = (int)baseX, prevY = 0;
    for (int y = 0; y <= 64; y += 4) {
      float t = (y / 12.0f) + (millis() / speed) + i;
      int x = (int)(baseX + sin(t) * 10);
      if (y > 0) oled.drawLine(prevX, prevY, x, y, SSD1306_WHITE);
      prevX = x; prevY = y;
    }
  }
}

namespace Display {

void setSkin(uint8_t skin) {
  currentSkin = (skin < DISPLAY_SKIN_COUNT) ? skin : 0;
}

uint8_t getSkin() {
  return currentSkin;
}

const char* getSkinName(uint8_t skin) {
  if (skin >= DISPLAY_SKIN_COUNT) return SKIN_NAMES[0];
  return SKIN_NAMES[skin];
}

bool begin() {
  // Wire.begin(SDA, SCL) — explicit pins, even though D2/D1 are the
  // ESP8266 I2C defaults. Being explicit here means this code keeps
  // working correctly even if someone changes the default pins later.
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);

  const uint8_t addrsToTry[2] = { OLED_I2C_ADDR_PRIMARY, OLED_I2C_ADDR_ALT };

  for (int attempt = 1; attempt <= DISPLAY_INIT_ATTEMPTS; attempt++) {
    for (int a = 0; a < 2; a++) {
      if (oled.begin(SSD1306_SWITCHCAPVCC, addrsToTry[a])) {
        activeAddr = addrsToTry[a];
        oled.clearDisplay();
        oled.display();
        initialized = true;
        Serial.print("[DISPLAY] SSD1306 initialized OK at 0x");
        Serial.print(activeAddr, HEX);
        if (attempt > 1) { Serial.print(" (attempt "); Serial.print(attempt); Serial.print(")"); }
        Serial.println();
        return true;
      }
    }
    if (attempt < DISPLAY_INIT_ATTEMPTS) {
      Serial.print("[DISPLAY] Not found yet (attempt ");
      Serial.print(attempt);
      Serial.print("/");
      Serial.print(DISPLAY_INIT_ATTEMPTS);
      Serial.println(") — retrying after power/bus settle delay...");
      delay(DISPLAY_INIT_RETRY_DELAY_MS);
    }
  }

  // Display not found at either address after retries — wrong address
  // (unlikely now, both were tried), bad wiring, or dead panel.
  Serial.println("[DISPLAY] ERROR: SSD1306 not found at 0x3C or 0x3D after retries.");
  Serial.println("[DISPLAY] Check wiring: SDA->D2, SCL->D1, plus VCC/GND, and that");
  Serial.println("[DISPLAY] the module is genuinely powered (some boards need a");
  Serial.println("[DISPLAY] jumper/solder bridge set for 3.3V vs 5V logic).");
  initialized = false;
  return false;
}

void showBootScreen(const String &deviceName, const String &deviceId) {
  if (!initialized) return; // no panel — nothing safe to draw to
  oled.clearDisplay();

  // --- Device identity only (firmware name/version kept in Serial) ---
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 10);
  oled.println("========================");
  oled.setCursor(0, 24);
  oled.print("Name: ");
  oled.println(deviceName);
  oled.setCursor(0, 34);
  oled.print("ID: ");
  oled.println(deviceId);
  oled.display();
  delay(1400);

  // --- Simple loading animation: a growing progress bar ---
  oled.clearDisplay();
  const int barX = 10;
  const int barY = 28;
  const int barW = 108;
  const int barH = 8;

  oled.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
  oled.display();

  const int steps = 10;
  for (int i = 1; i <= steps; i++) {
    int fillWidth = (barW - 2) * i / steps;
    oled.fillRect(barX + 1, barY + 1, fillWidth, barH - 2, SSD1306_WHITE);
    oled.display();
    delay(120); // Total animation time ≈ 1.2s
  }

  oled.setCursor(0, 44);
  oled.println("Loading complete");
  oled.display();
  delay(400);
}

void showWifiBootScreen(bool apMode, const String &ip) {
  if (!initialized) return; // no panel — nothing safe to draw to
  unsigned long screenStart = millis();
  const char* full = apMode ? "SETUP MODE" : "SYSTEM ONLINE";
  int fullLen = strlen(full);
  unsigned long animDuration = 2500;
  unsigned long t0 = millis();

  while (millis() - t0 < animDuration) {
    unsigned long t = millis() - t0;
    oled.clearDisplay();

    oled.drawRoundRect(0, 0, SCREEN_WIDTH, 16, 3, SSD1306_WHITE);
    drawCenteredText("ECHO SMART GUARD", 4, 1, SSD1306_WHITE);
    drawDashedHLine(0, 20, SCREEN_WIDTH, 2, 2, SSD1306_WHITE);

    int shown = min((int)(t / 90), fullLen);
    char buf[24]; memset(buf, 0, sizeof(buf));
    strncpy(buf, full, shown);
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(4, 26);
    oled.print(buf);
    if (((millis() / 350) % 2) == 0 && shown < 20) oled.print('_');

    oled.setCursor(4, 40);
    oled.print(apMode ? "AP " : "IP ");
    oled.print(ip);

    int barW = SCREEN_WIDTH - 8;
    int fill = min((int)(t * barW / (long)animDuration), barW);
    oled.drawRoundRect(4, 54, barW, 8, 2, SSD1306_WHITE);
    oled.fillRoundRect(4, 54, fill, 8, 2, SSD1306_WHITE);

    oled.display();
    delay(30);
  }

  // Hold the final frame (full bar, full text) for the remainder of ~7s total.
  unsigned long elapsed = millis() - screenStart;
  if (elapsed < 7000) delay(7000 - elapsed);
}

void showConnectingScreen() {
  if (!initialized) return; // no panel — nothing safe to draw to
  oled.clearDisplay();
  oled.drawRoundRect(0, 0, SCREEN_WIDTH, 16, 3, SSD1306_WHITE);
  drawCenteredText("ECHO SMART GUARD", 4, 1, SSD1306_WHITE);
  drawDashedHLine(0, 20, SCREEN_WIDTH, 2, 2, SSD1306_WHITE);
  drawCenteredText("Connecting to WiFi...", 34, 1, SSD1306_WHITE);
  oled.display();
}

void showApInstructions(const String &ssid, const String &ip) {
  if (!initialized) return; // no panel — nothing safe to draw to
  oled.clearDisplay();

  oled.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
  drawCenteredText("WI-FI SETUP NEEDED", 2, 1, SSD1306_BLACK);

  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 18);
  oled.print("Connect phone/PC to:");
  drawCenteredText(ssid.c_str(), 28, 1, SSD1306_WHITE);

  drawDashedHLine(0, 40, SCREEN_WIDTH, 2, 2, SSD1306_WHITE);

  oled.setCursor(0, 46);
  oled.print("Then open in browser:");
  String url = "http://" + ip;
  drawCenteredText(url.c_str(), 56, 1, SSD1306_WHITE);

  oled.display();
}

void showReadingScreen(bool armed, bool alarmActive, float distanceCm,
                       bool distanceValid, bool wifiConnected, bool buzzerReachable,
                       const String &deviceName, float triggerDistanceCm) {
  if (!initialized) return; // no panel — nothing safe to draw to
  oled.clearDisplay();

  // Computed ONCE here rather than inside each skin, so every skin's
  // trigger-zone blink stays in perfect sync with the others (switching
  // skins mid-blink never looks like it "jumped").
  bool inZone = distanceValid && triggerDistanceCm > 0 && distanceCm < triggerDistanceCm;
  bool blink = ((millis() / 300) % 2) == 0;

  switch (currentSkin) {
    case 1: skinDrawBarGauge(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 2: skinDrawRadarSweep(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 3: skinDrawMinimalShield(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 4: skinDrawSecurityHud(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 5: skinDrawSonarPulse(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 6: skinDrawRetroTerminal(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 7: skinDrawGridDashboard(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 8: skinDrawAnalogGauge(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 9: skinDrawBigDigit(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 10: skinDrawHeartbeat(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 11: skinDrawTachometer(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 12: skinDrawThermometer(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 13: skinDrawEqualizer(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 14: skinDrawVuMeter(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 15: skinDrawDigitalMatrix(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 16: skinDrawCrtScanlines(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 17: skinDrawOrbitMonitor(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 18: skinDrawRippleWave(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 19: skinDrawCompassDial(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 20: skinDrawPixelGuard(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 21: skinDrawMatrixRain(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 22: skinDrawFingerprintScan(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 23: skinDrawCombinationLock(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 24: skinDrawFlameAlert(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 25: skinDrawFrostIdle(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 26: skinDrawLightningPulse(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 27: skinDrawStarField(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 28: skinDrawHourglassTimer(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 29: skinDrawConstellation(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 30: skinDrawBreathingCircle(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 31: skinDrawInkRipple(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 32: skinDrawMoonPhases(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 33: skinDrawAuroraWaves(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 34: skinDrawCassetteReels(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 35: skinDrawTuningDial(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 36: skinDrawHoloGrid(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 37: skinDrawOrbitalRingsPremium(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 38: skinDrawCelestialDrift(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 39: skinDrawNightGuardian(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 40: skinDrawFloatingParticles(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 41: skinDrawGrowingTree(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 42: skinDrawFallingLeaves(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 43: skinDrawVhsStatic(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 44: skinDrawPixelArcade(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 45: skinDrawDataStream(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 46: skinDrawSleepyCat(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 47: skinDrawRobotBuddy(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 48: skinDrawSwissGrid(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 49: skinDrawOrbitingPlanets(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 50: skinDrawSoftPulseDot(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 51: skinDrawRaindropRipples(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 52: skinDrawDotMatrixPrint(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 53: skinDrawCyberpunkTerminal(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 54: skinDrawWatchdog(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 55: skinDrawMinimalistGauge(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 56: skinDrawCorporateTicker(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 57: skinDrawBlueprint(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 58: skinDrawZenGarden(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    case 59: skinDrawPolarAurora(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
    default: skinDrawClassic(armed, alarmActive, distanceCm, distanceValid, wifiConnected, buzzerReachable, deviceName, triggerDistanceCm, inZone, blink); break;
  }

  oled.display();
}

void showMessage(const String &line1, const String &line2) {
  if (!initialized) return; // no panel — nothing safe to draw to
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 20);
  oled.println(line1);
  if (line2.length() > 0) {
    oled.setCursor(0, 32);
    oled.println(line2);
  }
  oled.display();
}

// Tracks the user's last DELIBERATE on/off command (via setPower) so
// periodicMaintenance()'s re-init below can restore it afterward — begin()
// always leaves the physical panel powered ON, so without this, a user
// who turned the OLED off from the dashboard would see it silently turn
// itself back on every 5 minutes (this was a real reported bug: the
// periodic re-init existed to fix I2C register corruption, but never
// accounted for an intentional off command).
static bool lastCommandedPowerOn = true;

void setPower(bool on) {
  lastCommandedPowerOn = on;
  // Turning the panel ON is also treated as "please try to recover it" —
  // if it was never successfully initialized (or dropped out), retry
  // begin() right now instead of silently doing nothing until the next
  // reboot. This is what makes the dashboard's OLED quick-control able
  // to bring a display back after a loose wire gets reseated.
  if (on && !initialized) {
    Serial.println("[DISPLAY] setPower(true) requested but panel not initialized — retrying begin()...");
    begin();
  }
  if (!initialized) return; // still not there — nothing safe to command
  oled.ssd1306_command(on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
}

bool isAvailable() {
  return initialized;
}

// How often to force a fresh init even when the panel already reports
// "available" — masks slow register corruption from a marginal I2C
// connection (see display.h for the full explanation).
#define PERIODIC_REINIT_INTERVAL_MS (5UL * 60UL * 1000UL) // 5 minutes

void periodicMaintenance() {
  static unsigned long lastReinitMillis = 0;
  unsigned long now = millis();
  if (now - lastReinitMillis < PERIODIC_REINIT_INTERVAL_MS) return;
  lastReinitMillis = now;

  if (!initialized) return; // begin()'s own retry loop already handles this case
  Serial.println("[DISPLAY] Periodic re-init (guards against I2C-noise register corruption).");
  begin();
  // begin() always leaves the physical panel powered ON — immediately
  // re-apply whatever the user last actually commanded, so a
  // deliberately-off OLED doesn't silently light back up every 5 minutes.
  if (!lastCommandedPowerOn) {
    oled.ssd1306_command(SSD1306_DISPLAYOFF);
  }
}

} // namespace Display
