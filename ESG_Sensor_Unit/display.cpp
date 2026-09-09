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

namespace Display {

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

  // Header bar: device name (left) + status badge (right)
  oled.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  oled.setTextSize(1);

  // Measure the badge FIRST so the device name gets truncated to however
  // much room is actually left, instead of a fixed guess. The fixed guess
  // (13 chars) used to clip the default "Unnamed Device" (14 chars) down
  // to "Unnamed Devic" even though there was room to spare — this way the
  // name always uses all available space, whatever the badge text is.
  const char* badge = alarmActive ? "ALARM!" : (armed ? "ARMED" : "DISARM");
  int16_t x1, y1; uint16_t w, h;
  oled.getTextBounds(badge, 0, 0, &x1, &y1, &w, &h);

  // Default GFX font is a fixed 6px advance per character at text size 1.
  int availableW = SCREEN_WIDTH - (int)w - 6 /* gap between name and badge */ - 2 /* left margin */;
  int maxChars = availableW / 6;
  if (maxChars < 1) maxChars = 1;
  String hdr = (int)deviceName.length() > maxChars ? deviceName.substring(0, maxChars) : deviceName;
  oled.setCursor(2, 2);
  oled.print(hdr);

  oled.setCursor(SCREEN_WIDTH - w - 2, 2);
  oled.print(badge);

  // Big distance readout, centered
  oled.setTextColor(SSD1306_WHITE);
  bool inZone = distanceValid && triggerDistanceCm > 0 && distanceCm < triggerDistanceCm;
  if (distanceValid) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%.1f", distanceCm);
    drawCenteredText(buf, 22, 3, SSD1306_WHITE);
  } else {
    drawCenteredText("-- no reading --", 28, 1, SSD1306_WHITE);
  }

  // Bottom row: WiFi status (left) — Trigger distance / zone alert (center) — Buzzer status (right)
  drawWifiIcon(2, 54, wifiConnected);
  drawBuzzerIcon(SCREEN_WIDTH - 16, 54, buzzerReachable);

  bool blink = ((millis() / 300) % 2) == 0;
  if (inZone && blink) {
    drawCenteredText("* IN TRIGGER ZONE *", 56, 1, SSD1306_WHITE);
  } else if (triggerDistanceCm > 0) {
    char trigBuf[16];
    snprintf(trigBuf, sizeof(trigBuf), "Trig:%.0fcm", triggerDistanceCm);
    drawCenteredText(trigBuf, 56, 1, SSD1306_WHITE);
  } else {
    drawCenteredText("Not calibrated", 56, 1, SSD1306_WHITE);
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
