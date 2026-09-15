/*
  =====================================================================
  config.h — WiFi Echo Smart Gaurd
  =====================================================================
  Central place for:
    - Firmware identity (name/version) — shown on OLED boot screen
    - Pin assignments — one source of truth, never hardcode pins elsewhere
    - Global constants used across multiple modules

  WHY THIS FILE EXISTS:
  Hardcoding pin numbers or magic constants inside .cpp files makes the
  firmware fragile — if you rewire the sensor to a different GPIO, you'd
  have to hunt through every file. Keeping it all here means one edit,
  and every module (sensor, alarm, display) picks it up automatically.
  =====================================================================
*/

#ifndef CONFIG_H
#define CONFIG_H

// ---------------------------------------------------------------------
// FIRMWARE IDENTITY
// ---------------------------------------------------------------------
#define FIRMWARE_NAME     "Echo Smart Gaurd"
#define FIRMWARE_VERSION  "1.1.0"

#define NTP_GMT_OFFSET_SEC 19800   // IST (+5:30)
  // NIGHT_START_HOUR/NIGHT_END_HOUR removed — now configurable at
  // runtime via settings.nightStartHour/nightEndHour (see storage.h).
#define EMERGENCY_STOP_DURATION_MIN 10   // Auto-resume after this many minutes
#define TEST_BUZZER_DURATION_MS     5000

// Consecutive in-zone readings required before firing an alarm — reduces
// false positives from a single glitchy reading, still fast (~300-400ms).
#define INTRUSION_CONFIRM_READINGS  2

// Sensor health: if a VALID reading is bit-for-bit identical (within a
// tiny epsilon) for this many seconds straight, it likely means the
// sensor is stuck/frozen — a real object always has micro-variance.
// Informational only — no automatic restart (an earlier version did
// that and caused false-positive restart loops on static objects).
#define SENSOR_FROZEN_THRESHOLD_SEC 30
#define TAMPER_DISTANCE_CM 4.0f      // closer than this suggests something is physically covering/blocking the sensor
#define TAMPER_SUSTAINED_SEC 12      // how long that close reading must hold before it's flagged (avoids false alarms from someone briefly walking right up to it)

// ---------------------------------------------------------------------
// PIN MAP
// ---------------------------------------------------------------------
// NOTE: On NodeMCU, "D" labels (D1, D2...) are silkscreen labels, NOT the
// same as the GPIO number the Arduino core expects in code. The core
// defines D1..D8 as macros that map to the correct GPIO automatically,
// so we use the D-macros here for clarity, matching your wiring notes.

// OLED (I2C) — uses hardware I2C, so these are informational; the
// Wire library uses D1/D2 as SCL/SDA by default on NodeMCU already.
#define PIN_OLED_SDA   D2   // GPIO4
#define PIN_OLED_SCL   D1   // GPIO5

// Ultrasonic sensor (JSN-SR04T)
#define PIN_SENSOR_TRIG D5  // GPIO14
#define PIN_SENSOR_ECHO D7  // GPIO13  (via 1K/2K resistor divider — see wiring doc)

// Buzzer now lives on a SEPARATE ESP8266 device, controlled over WiFi —
// see BUZZER_DEVICE_IP below. GPIO2 (D4) on THIS board is unused now.

// Onboard LED (NodeMCU) — used in Step 1 only, to prove the board is alive
#ifndef LED_BUILTIN
#define LED_BUILTIN 2        // GPIO2
#endif

// ---------------------------------------------------------------------
// REMOTE BUZZER DEVICE (second ESP8266)
// ---------------------------------------------------------------------
// IP address of the portable buzzer unit. Set this to whatever static/
// reserved IP your router gives that second device (check its Serial
// Monitor on boot, same as this one).
#define DEFAULT_BUZZER_DEVICE_IP  "192.168.1.XX" // Fallback only — real value is dashboard-editable, persisted

// Defaults for buzzer timing (dashboard-editable, persisted — these are
// only the factory-default starting values).
#define DEFAULT_SHORT_TERM_BUZZER_SEC   5
#define DEFAULT_LONG_TERM_BUZZER_SEC    10
#define DEFAULT_SUSTAINED_THRESHOLD_SEC 10

// Fallback device identity — dashboard-editable, persisted (Danger Zone).
#define DEFAULT_DEVICE_NAME "Unnamed Device"
#define DEFAULT_DEVICE_ID   "Unnamed ID"

// Dashboard login — CHANGE THESE from the dashboard's Danger Zone after
// first boot. These are only the factory-default fallback values.
// User-changeable login (editable from dashboard) — this is what
// normally logs you in day-to-day.
#define DEFAULT_DASHBOARD_USERNAME "admin"
#define DEFAULT_DASHBOARD_PASSWORD "admin"

// Master/recovery login — permanent, NOT editable from the dashboard,
// works even if the user-changeable credentials above are changed or
// forgotten. Never displayed/removable via any UI control.
#define MASTER_DASHBOARD_USERNAME "user_esg"
#define MASTER_DASHBOARD_PASSWORD "pass_esg"

// How often the main ESP pings the remote buzzer unit to confirm it's
// still reachable (heartbeat check) — Telegram alerts on state change.
#define BUZZER_HEARTBEAT_INTERVAL_MS 60000UL
#define MAX_BUZZER_UNITS 5
// How often "last alive" timestamp is saved to EEPROM for diagnostics
// (used to approximate "last shutdown time" on next boot). Kept
// infrequent to limit flash wear.
#define ALIVE_SAVE_INTERVAL_MS 300000UL // 5 minutes
// NOTE: The remote buzzer ESP8266 implements these endpoints:
//   /trigger?duration=<ms>&pattern=<1-4>  — continuous or patterned buzz
//   /pulse?duration=<ms>&pattern=<1-4>    — sustained/pulsed alert pattern
//   /stop                   — immediate silence
// Only /trigger and /stop exist in earlier versions of that second
// device's firmware — /pulse needs to be added there too.

// ---------------------------------------------------------------------
// SERIAL DEBUG
// ---------------------------------------------------------------------
#define SERIAL_BAUD_RATE 115200

// ---------------------------------------------------------------------
// SENSOR CONSTANTS (JSN-SR04T)
// ---------------------------------------------------------------------
// Datasheet-typical blind zone: readings below this are noise, not real.
#define SENSOR_MIN_RANGE_CM   20
// Practical max range for reliable readings (datasheet claims up to 500,
// but 450 gives margin before the sensor's own accuracy degrades).
#define SENSOR_MAX_RANGE_CM   450

// Speed of sound ≈ 343 m/s at room temp → 29.15 µs per cm round-trip,
// commonly rounded to 58 µs/cm in hobbyist code. We use 58 for consistency
// with widely-documented reference designs.
#define SENSOR_US_PER_CM      58

// Timeout for a single pulseIn() call, derived from max range:
// SENSOR_MAX_RANGE_CM * SENSOR_US_PER_CM, plus margin.
#define SENSOR_TIMEOUT_US      ((unsigned long)(SENSOR_MAX_RANGE_CM * SENSOR_US_PER_CM) + 5000UL)

// Gap between the 3 quick pings used for median filtering (see sensor.cpp).
// JSN-SR04T needs some recovery time between pings to avoid echo overlap.
#define SENSOR_SAMPLE_GAP_MS   40

// ---------------------------------------------------------------------
// WIFI CONSTANTS
// ---------------------------------------------------------------------
// Fallback Access Point — created when no saved WiFi works. This is how
// you reach the device to configure it, per the "no physical buttons"
// requirement: everything is done over WiFi.
#define WIFI_AP_SSID      "Echo Smart Gaurd Setup"
#define WIFI_AP_PASSWORD  "12345678"

// How long to try connecting to saved WiFi before giving up and
// falling back to Access Point mode. Long enough for a normal router,
// short enough not to leave the device unreachable for ages if the
// saved network is out of range.
#define WIFI_CONNECT_TIMEOUT_MS  15000

// ---------------------------------------------------------------------
// CALIBRATION CONSTANTS
// ---------------------------------------------------------------------
// Number of readings taken during calibration. Each Sensor::readDistanceCM()
// call is itself already a median-of-3 (~150-200ms), so 20 samples takes
// roughly 3-4 seconds total — acceptable for a one-time setup action.
#define CALIBRATION_SAMPLE_COUNT     20

// Minimum fraction of samples that must be valid for calibration to be
// considered trustworthy, rather than accepting a near-empty dataset.
#define CALIBRATION_MIN_VALID_RATIO  0.5f

// How much closer than the calibrated wall distance counts as "intruder".
// This is the buffer that absorbs normal sensor jitter near the wall
// itself, so the wall never falsely triggers the alarm.
#define CALIBRATION_SAFETY_MARGIN_CM 15.0f

// ---------------------------------------------------------------------
// TELEGRAM ALERT CONFIGURATION
// ---------------------------------------------------------------------
// Fill these in with YOUR bot token and chat ID (see chat instructions
// for how to get them from BotFather). Kept as compile-time constants
// here (not in Settings/EEPROM) specifically so this feature can't ever
// trigger a struct-layout change that would wipe your saved calibration.
#define TELEGRAM_BOT_TOKEN  "PASTE_YOUR_BOT_TOKEN_HERE"
#define TELEGRAM_CHAT_ID    "PASTE_YOUR_CHAT_ID_HERE"

#endif // CONFIG_H
