/*
 * AQUA MONITOR
 * AIoT-Based Multi-Parameter Water Quality Monitoring and Assessment System for Aquaculture
 *
 * Target : ESP32-S3 (Arduino-ESP32 core 3.0.0 or newer)
 * Sensors: pH (GPIO4), Turbidity (GPIO5), TDS (GPIO6) - analog | DS18B20 (GPIO7) - OneWire
 * Display: 1.3" SH1106 128x64 OLED, I2C (SDA GPIO8, SCL GPIO9)
 *
 * What this firmware does
 *   1. Samples all sensors WITHOUT blocking (ADC trimmed mean + asynchronous DS18B20 conversion).
 *   2. Applies calibration (pH 2/3-point, turbidity reference, TDS factor, temperature offset).
 *   3. Keeps calibration + settings permanently in NVS flash (Preferences).
 *   4. Shows the same numbers on the OLED (original 2x2 layout).
 *   5. Creates its own Wi-Fi hotspot "AquaMonitor" at the fixed IP 192.168.4.1 and serves the
 *      dashboard + a small JSON HTTP API (own FreeRTOS task, so a slow browser never stalls sampling).
 *   6. Streams JSON records over USB serial (115200 baud) and accepts calibration commands,
 *      for the Web Serial mode of the dashboard.
 *
 * Arduino IDE settings (Tools menu) for the ESP32-S3 native USB port:
 *   Board                : "ESP32S3 Dev Module"
 *   USB CDC On Boot      : "Enabled"            <-- Serial = native USB CDC
 *   USB Mode             : "Hardware CDC and JTAG"
 *   Upload Mode          : "UART0 / Hardware CDC"
 *   Flash / PSRAM / Partition: defaults for your board are fine (the default 4MB/8MB schemes work)
 *   Serial Monitor baud  : 115200
 *
 * Serial protocol (newline-terminated, one record per line; everything else is ignored by the dashboard)
 *   ESP32 -> browser : DATA:{...}  sensor record          CFG:{...}  calibration + settings
 *                      STAT:{...}  system / Wi-Fi status   ACK:{...}  command result
 *                      LOG:text    human-readable debug
 *   browser -> ESP32 : CMD:{"action":"...", ...}
 *   Actions: ping, get_data, get_status, get_config (alias get_calibration), set_calibration,
 *            set_settings, calibrate_ph, reset_calibration
 *
 * Original code notes (what was kept / what was changed) are listed in README.md.
 */

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "web_ui.h"

#define FW_VERSION "1.0.0"

// ------------------------------------------------------------------------------------------------
// Hardware configuration (UNCHANGED from the original sketch)
// ------------------------------------------------------------------------------------------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define I2C_ADDRESS 0x3C

#define OLED_SDA 8
#define OLED_SCL 9
#define PH_SENSOR_PIN 4
#define TURB_SENSOR_PIN 5
#define TDS_SENSOR_PIN 6
#define TEMP_SENSOR_PIN 7

// Set to 0 to stop the once-per-cycle human readable "LOG:" line on the serial port.
#define SERIAL_HUMAN_LOG 1

// Native USB CDC detection (Arduino IDE: Tools > USB CDC On Boot > Enabled)
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
#define USB_CDC_MODE 1
#else
#define USB_CDC_MODE 0
#endif

// ------------------------------------------------------------------------------------------------
// Wi-Fi access point configuration
// ------------------------------------------------------------------------------------------------
static const char *AP_SSID = "AquaMonitor";
static const char *AP_PASS = "Aqua12345";  // WPA2 needs >= 8 characters
static const uint8_t AP_CHANNEL = 6;
static const uint8_t AP_MAX_CLIENTS = 6;
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_GATEWAY(192, 168, 4, 1);
static const IPAddress AP_MASK(255, 255, 255, 0);

// ------------------------------------------------------------------------------------------------
// Original calibration defaults (values taken from the original sketch)
// ------------------------------------------------------------------------------------------------
static const float DEF_PH_V_NEUTRAL = 2.574f;  // original PH_V_NEUTRAL
static const float DEF_PH_SLOPE_MAG = 5.70f;   // original PH_SLOPE   (pH = 7 - 5.70 * (V - 2.574))
static const float DEF_TURB_V_CLEAR = 1.957f;  // original TURB_V_CLEAR (0 NTU)
static const float DEF_TURB_V_DIRTY = 0.50f;   // original TURB_V_DIRTY (1000 NTU)
static const float DEF_TURB_NTU_REF = 1000.0f; // NTU that belongs to TURB_V_DIRTY in the original formula
static const float DEF_TDS_K_FACTOR = 1.0f;    // original TDS_K_FACTOR

// Sampling (same trimmed-mean scheme as the original: 20 samples, 8 ms apart, middle 10 averaged)
static const uint8_t SAMPLES = 20;
static const uint32_t ADC_SAMPLE_GAP_MS = 8;
static const uint32_t TEMP_CONVERSION_MS = 800;  // DS18B20 12-bit conversion needs up to 750 ms

// Electrical plausibility window for analog sensor outputs (volts)
static const float ADC_V_MIN_OK = 0.05f;
static const float ADC_V_MAX_OK = 3.20f;

// ------------------------------------------------------------------------------------------------
// Data structures
// ------------------------------------------------------------------------------------------------
struct Calibration {
  float phSlope;     // pH per volt (signed). Original code: pH = 7 - 5.70 * (V - 2.574)  => -5.70
  float phVNeutral;  // volts measured at pH 7.00
  float turbVClear;  // volts in clear water (0 NTU)
  float turbVRef;    // volts at the reference sample
  float turbNtuRef;  // NTU of the reference sample (original: 1000 NTU at 0.50 V)
  float tdsK;        // TDS correction factor
  float tempOffset;  // degrees C added to the DS18B20 reading
};

struct Settings {
  float intervalMs;  // measurement interval
  float phMin, phMax;
  float tempMin, tempMax;
  float tdsMin, tdsMax;
  float turbMin, turbMax;
  float warnPct;     // "Warning" band outside the target range, in % of the range width
};

struct Reading {
  bool valid;
  uint32_t seq;
  uint32_t uptimeMs;
  float phV, ph;
  bool phOk;
  float tempRaw, temp;
  bool tempOk;
  float turbV, ntu;
  bool turbOk;
  float tdsV, tdsRaw, tds;
  bool tdsOk;
  float tdsCompC;
  bool tdsTempFallback;
};

struct ConfigSnapshot {
  Calibration cal;
  Settings set;
  bool saved;     // last flash write succeeded AND was read back identically
  bool nvsOk;     // NVS namespace could be opened
};

static const Calibration CAL_DEFAULT = {-DEF_PH_SLOPE_MAG, DEF_PH_V_NEUTRAL, DEF_TURB_V_CLEAR,
                                        DEF_TURB_V_DIRTY, DEF_TURB_NTU_REF, DEF_TDS_K_FACTOR, 0.0f};
// Generic starting values only - the user edits them for the species / system in use.
static const Settings SET_DEFAULT = {1000.0f, 6.5f, 8.5f, 24.0f, 30.0f, 0.0f, 500.0f, 0.0f, 50.0f, 10.0f};

struct KeyDef {
  const char *name;  // JSON key == NVS key (<= 15 chars)
  size_t off;        // offset of the float inside the struct
};

static const KeyDef CAL_KEYS[] = {
    {"ph_slope", offsetof(Calibration, phSlope)},       {"ph_v_neutral", offsetof(Calibration, phVNeutral)},
    {"turb_v_clear", offsetof(Calibration, turbVClear)}, {"turb_v_ref", offsetof(Calibration, turbVRef)},
    {"turb_ntu_ref", offsetof(Calibration, turbNtuRef)}, {"tds_k", offsetof(Calibration, tdsK)},
    {"temp_offset", offsetof(Calibration, tempOffset)}};
static const size_t N_CAL_KEYS = sizeof(CAL_KEYS) / sizeof(CAL_KEYS[0]);

static const KeyDef SET_KEYS[] = {
    {"interval_ms", offsetof(Settings, intervalMs)}, {"ph_min", offsetof(Settings, phMin)},
    {"ph_max", offsetof(Settings, phMax)},           {"temp_min", offsetof(Settings, tempMin)},
    {"temp_max", offsetof(Settings, tempMax)},       {"tds_min", offsetof(Settings, tdsMin)},
    {"tds_max", offsetof(Settings, tdsMax)},         {"turb_min", offsetof(Settings, turbMin)},
    {"turb_max", offsetof(Settings, turbMax)},       {"warn_pct", offsetof(Settings, warnPct)}};
static const size_t N_SET_KEYS = sizeof(SET_KEYS) / sizeof(SET_KEYS[0]);

// NOTE: JsonOut is defined here, BEFORE the first function, on purpose. The Arduino IDE auto-generates
// prototypes right before the first function; a type defined after that point is unknown to them.
// ------------------------------------------------------------------------------------------------
// JSON writer into a caller supplied buffer (never overflows, never prints NaN/inf)
// ------------------------------------------------------------------------------------------------
struct JsonOut {
  char *buf;
  size_t cap;
  size_t len;
  bool comma;
  bool overflow;
  JsonOut(char *b, size_t c) : buf(b), cap(c), len(0), comma(false), overflow(false) {
    if (cap) buf[0] = 0;
  }
  void put(const char *s) {
    size_t l = strlen(s);
    if (overflow || len + l + 1 > cap) {
      overflow = true;
      return;
    }
    memcpy(buf + len, s, l + 1);
    len += l;
  }
  void sep() {
    if (comma) put(",");
    comma = true;
  }
  void begin() {
    put("{");
    comma = false;
  }
  void end() {
    put("}");
    comma = true;
  }
  void key(const char *k) {
    sep();
    put("\"");
    put(k);
    put("\":");
  }
  void objBegin(const char *k) {
    key(k);
    put("{");
    comma = false;
  }
  void objEnd() {
    put("}");
    comma = true;
  }
  void numF(const char *k, float v, int dec) {
    key(k);
    if (isfinite(v)) {
      char t[28];
      snprintf(t, sizeof t, "%.*f", dec, (double)v);
      put(t);
    } else {
      put("null");
    }
  }
  void numG(const char *k, float v) {
    key(k);
    if (isfinite(v)) {
      char t[28];
      snprintf(t, sizeof t, "%.6g", (double)v);
      put(t);
    } else {
      put("null");
    }
  }
  void u32(const char *k, uint32_t v) {
    key(k);
    char t[16];
    snprintf(t, sizeof t, "%lu", (unsigned long)v);
    put(t);
  }
  void boolean(const char *k, bool v) {
    key(k);
    put(v ? "true" : "false");
  }
  void str(const char *k, const char *v) {
    key(k);
    put("\"");
    for (; *v; v++) {
      char c = *v;
      char t[3] = {0, 0, 0};
      if (c == '"' || c == '\\') {
        t[0] = '\\';
        t[1] = c;
      } else if ((uint8_t)c < 0x20) {
        t[0] = ' ';
      } else {
        t[0] = c;
      }
      put(t);
    }
    put("\"");
  }
};

static inline float *fld(void *base, size_t off) { return reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(base) + off); }
static inline const float *cfld(const void *base, size_t off) {
  return reinterpret_cast<const float *>(reinterpret_cast<const uint8_t *>(base) + off);
}

// ------------------------------------------------------------------------------------------------
// Globals
// ------------------------------------------------------------------------------------------------
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
OneWire oneWire(TEMP_SENSOR_PIN);
DallasTemperature tempSensor(&oneWire);
WebServer server(80);
Preferences prefs;

// Shared between the sensor/serial task (Arduino loop) and the web task. Guard with gMutex.
static SemaphoreHandle_t gMutex = NULL;
static Calibration gCal = CAL_DEFAULT;
static Settings gSet = SET_DEFAULT;
static Reading gReading = {};
static bool gSavedOk = false;
static bool gNvsOk = false;

static volatile bool gOledOk = false;
static volatile uint8_t gDsCount = 0;

struct Lock {
  Lock() { xSemaphoreTake(gMutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(gMutex); }
};

// ------------------------------------------------------------------------------------------------
// Calculations (original formulas, extended with the calibration parameters)
// ------------------------------------------------------------------------------------------------
static inline bool inRange(float v, float lo, float hi) { return isfinite(v) && v >= lo && v <= hi; }

// Original: pH = 7.00 - PH_SLOPE * (V - PH_V_NEUTRAL)   (PH_SLOPE = 5.70)  ->  slope = -5.70
static float computePh(float v, const Calibration &c) { return 7.0f + c.phSlope * (v - c.phVNeutral); }

// Original: ntu = (V_CLEAR - V) * 1000 / (V_CLEAR - V_DIRTY), constrained to 0..1000
// Generalised: ntu = (V_CLEAR - V) * NTU_REF / (V_CLEAR - V_REF). With the defaults it is identical.
static float computeNtu(float v, const Calibration &c) {
  float d = c.turbVClear - c.turbVRef;
  if (!isfinite(d) || fabsf(d) < 0.001f) return NAN;
  float n = (c.turbVClear - v) * c.turbNtuRef / d;
  if (!isfinite(n)) return NAN;
  if (n < 0.0f) n = 0.0f;
  if (n > 1000.0f) n = 1000.0f;
  return n;
}

// Original TDS polynomial with temperature compensation, WITHOUT the K factor (uncorrected value).
static float computeTdsRaw(float v, float compTempC) {
  if (v <= 0.02f) return 0.0f;
  if (compTempC < 0.0f) compTempC = 0.0f;   // keeps the compensation coefficient positive
  if (compTempC > 60.0f) compTempC = 60.0f;
  float compCoeff = 1.0f + 0.02f * (compTempC - 25.0f);
  float cv = v / compCoeff;
  float t = (133.42f * cv * cv * cv - 255.86f * cv * cv + 857.39f * cv) * 0.5f;
  return (t < 0.0f) ? 0.0f : t;
}

// ------------------------------------------------------------------------------------------------
// Minimal flat-JSON helpers (no external JSON library needed; all commands are flat objects)
// ------------------------------------------------------------------------------------------------
static inline bool isWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Returns a pointer to the first character of the value belonging to "key", or NULL.
static const char *jsonFindValue(const char *json, const char *key) {
  if (!json || !key) return NULL;
  size_t klen = strlen(key);
  const char *p = json;
  while ((p = strchr(p, '"')) != NULL) {
    const char *s = p + 1;
    const char *e = s;
    while (*e && *e != '"') {
      if (*e == '\\' && e[1]) e++;
      e++;
    }
    if (!*e) return NULL;
    const char *q = e + 1;
    while (isWs(*q)) q++;
    if (*q == ':' && (size_t)(e - s) == klen && strncmp(s, key, klen) == 0) {
      q++;
      while (isWs(*q)) q++;
      return q;
    }
    p = e + 1;
  }
  return NULL;
}

// 0 = key absent, 1 = ok, -1 = present but not a finite number
static int jsonGetFloat(const char *json, const char *key, float *out) {
  const char *v = jsonFindValue(json, key);
  if (!v) return 0;
  if (*v == '"') v++;  // tolerate numbers sent as strings
  if (!(*v == '-' || *v == '+' || *v == '.' || (*v >= '0' && *v <= '9'))) return -1;
  char *end = NULL;
  double d = strtod(v, &end);
  if (end == v || !isfinite(d)) return -1;
  for (const char *c = v; c < end; c++)
    if (*c == 'x' || *c == 'X') return -1;
  if (!(*end == '\0' || *end == ',' || *end == '}' || *end == '"' || isWs(*end))) return -1;
  *out = (float)d;
  return 1;
}

static bool jsonGetString(const char *json, const char *key, char *out, size_t cap) {
  const char *v = jsonFindValue(json, key);
  if (!v || *v != '"' || cap == 0) return false;
  v++;
  size_t n = 0;
  while (*v && *v != '"' && n + 1 < cap) out[n++] = *v++;
  out[n] = 0;
  return *v == '"';
}

// ------------------------------------------------------------------------------------------------
// Validation
// ------------------------------------------------------------------------------------------------
static bool validateCal(const Calibration &c, const char **err) {
  if (!inRange(fabsf(c.phSlope), 1.0f, 30.0f)) { *err = "ph_slope magnitude must be 1..30 pH per volt"; return false; }
  if (!inRange(c.phVNeutral, 0.1f, 3.2f)) { *err = "ph_v_neutral must be 0.1..3.2 V"; return false; }
  if (!inRange(c.turbVClear, 0.1f, 3.3f)) { *err = "turb_v_clear must be 0.1..3.3 V"; return false; }
  if (!inRange(c.turbVRef, 0.0f, 3.3f)) { *err = "turb_v_ref must be 0..3.3 V"; return false; }
  if (fabsf(c.turbVClear - c.turbVRef) < 0.05f) { *err = "turbidity clear and reference voltages must differ by at least 0.05 V"; return false; }
  if (!inRange(c.turbNtuRef, 1.0f, 1000.0f)) { *err = "turb_ntu_ref must be 1..1000 NTU"; return false; }
  if (!inRange(c.tdsK, 0.1f, 10.0f)) { *err = "tds_k must be 0.1..10"; return false; }
  if (!inRange(c.tempOffset, -10.0f, 10.0f)) { *err = "temp_offset must be -10..10 C"; return false; }
  return true;
}

static bool validateSettings(const Settings &s, const char **err) {
  if (!inRange(s.intervalMs, 1000.0f, 60000.0f)) { *err = "interval_ms must be 1000..60000"; return false; }
  if (!inRange(s.phMin, 0.0f, 14.0f) || !inRange(s.phMax, 0.0f, 14.0f) || !(s.phMin < s.phMax)) { *err = "pH range must be within 0..14 with min < max"; return false; }
  if (!inRange(s.tempMin, -5.0f, 60.0f) || !inRange(s.tempMax, -5.0f, 60.0f) || !(s.tempMin < s.tempMax)) { *err = "temperature range must be within -5..60 with min < max"; return false; }
  if (!inRange(s.tdsMin, 0.0f, 5000.0f) || !inRange(s.tdsMax, 0.0f, 5000.0f) || !(s.tdsMin < s.tdsMax)) { *err = "TDS range must be within 0..5000 with min < max"; return false; }
  if (!inRange(s.turbMin, 0.0f, 1000.0f) || !inRange(s.turbMax, 0.0f, 1000.0f) || !(s.turbMin < s.turbMax)) { *err = "turbidity range must be within 0..1000 with min < max"; return false; }
  if (!inRange(s.warnPct, 0.0f, 50.0f)) { *err = "warn_pct must be 0..50"; return false; }
  return true;
}

// ------------------------------------------------------------------------------------------------
// Persistent storage (Preferences / NVS)
// ------------------------------------------------------------------------------------------------
static const char *NVS_NS = "aquamon";

// Caller must hold the lock (or be in setup before tasks start).
static bool saveConfigLocked() {
  bool ok = prefs.begin(NVS_NS, false);
  gNvsOk = ok;
  if (ok) {
    for (size_t i = 0; i < N_CAL_KEYS; i++)
      if (prefs.putFloat(CAL_KEYS[i].name, *cfld(&gCal, CAL_KEYS[i].off)) != sizeof(float)) ok = false;
    for (size_t i = 0; i < N_SET_KEYS; i++)
      if (prefs.putFloat(SET_KEYS[i].name, *cfld(&gSet, SET_KEYS[i].off)) != sizeof(float)) ok = false;
    prefs.end();
  }
  bool verified = false;
  if (ok && prefs.begin(NVS_NS, true)) {  // read everything back: proves it really is in flash
    verified = true;
    for (size_t i = 0; i < N_CAL_KEYS; i++)
      if (!(prefs.getFloat(CAL_KEYS[i].name, NAN) == *cfld(&gCal, CAL_KEYS[i].off))) verified = false;
    for (size_t i = 0; i < N_SET_KEYS; i++)
      if (!(prefs.getFloat(SET_KEYS[i].name, NAN) == *cfld(&gSet, SET_KEYS[i].off))) verified = false;
    prefs.end();
  }
  gSavedOk = ok && verified;
  return gSavedOk;
}

static void loadConfig() {
  Calibration c = CAL_DEFAULT;
  Settings s = SET_DEFAULT;
  bool opened = prefs.begin(NVS_NS, false);
  if (opened) {
    for (size_t i = 0; i < N_CAL_KEYS; i++) *fld(&c, CAL_KEYS[i].off) = prefs.getFloat(CAL_KEYS[i].name, *cfld(&CAL_DEFAULT, CAL_KEYS[i].off));
    for (size_t i = 0; i < N_SET_KEYS; i++) *fld(&s, SET_KEYS[i].off) = prefs.getFloat(SET_KEYS[i].name, *cfld(&SET_DEFAULT, SET_KEYS[i].off));
    prefs.end();
  }
  const char *err = NULL;
  if (!validateCal(c, &err)) c = CAL_DEFAULT;       // corrupt flash content -> defaults, never garbage
  if (!validateSettings(s, &err)) s = SET_DEFAULT;
  gCal = c;
  gSet = s;
  gNvsOk = opened;
  gSavedOk = opened;
}

// ------------------------------------------------------------------------------------------------
// Configuration actions (all run with the lock held by executeAction)
// ------------------------------------------------------------------------------------------------
static bool applyConfigJson(const char *json, char *msg, size_t cap) {
  Calibration nc = gCal;
  Settings ns = gSet;
  int found = 0;
  for (size_t i = 0; i < N_CAL_KEYS; i++) {
    float v;
    int r = jsonGetFloat(json, CAL_KEYS[i].name, &v);
    if (r < 0) { snprintf(msg, cap, "invalid number for %s", CAL_KEYS[i].name); return false; }
    if (r > 0) { *fld(&nc, CAL_KEYS[i].off) = v; found++; }
  }
  for (size_t i = 0; i < N_SET_KEYS; i++) {
    float v;
    int r = jsonGetFloat(json, SET_KEYS[i].name, &v);
    if (r < 0) { snprintf(msg, cap, "invalid number for %s", SET_KEYS[i].name); return false; }
    if (r > 0) { *fld(&ns, SET_KEYS[i].off) = v; found++; }
  }
  if (found == 0) { snprintf(msg, cap, "no known calibration or settings keys in command"); return false; }
  const char *err = NULL;
  if (!validateCal(nc, &err) || !validateSettings(ns, &err)) { snprintf(msg, cap, "%s", err); return false; }
  gCal = nc;
  gSet = ns;
  if (!saveConfigLocked()) { snprintf(msg, cap, "applied, but saving to flash FAILED"); return false; }
  snprintf(msg, cap, "%d value(s) applied and saved to flash", found);
  return true;
}

// Multi-point pH calibration. Accepts 2 or 3 points: ph_p1_ref/ph_p1_v ... ph_p3_ref/ph_p3_v.
// Least-squares line  pH = a + b * V  ->  slope = b,  V@pH7 = (7 - a) / b.
static bool calibratePh(const char *json, char *msg, size_t cap) {
  float ref[3], vol[3];
  int n = 0;
  for (int i = 1; i <= 3; i++) {
    char kr[16], kv[16];
    snprintf(kr, sizeof kr, "ph_p%d_ref", i);
    snprintf(kv, sizeof kv, "ph_p%d_v", i);
    float r = 0, v = 0;
    int a = jsonGetFloat(json, kr, &r);
    int b = jsonGetFloat(json, kv, &v);
    if (a < 0 || b < 0) { snprintf(msg, cap, "point %d: invalid number", i); return false; }
    if (a == 0 && b == 0) continue;
    if (a == 0 || b == 0) { snprintf(msg, cap, "point %d is incomplete (needs buffer pH and voltage)", i); return false; }
    if (!inRange(r, 0.0f, 14.0f)) { snprintf(msg, cap, "point %d: buffer pH must be 0..14", i); return false; }
    if (!inRange(v, ADC_V_MIN_OK, 3.30f)) { snprintf(msg, cap, "point %d: voltage %.3f V is outside the valid ADC range", i, (double)v); return false; }
    ref[n] = r;
    vol[n] = v;
    n++;
  }
  if (n < 2) { snprintf(msg, cap, "need at least 2 calibration points"); return false; }
  float vmin = vol[0], vmax = vol[0];
  for (int i = 0; i < n; i++) {
    if (vol[i] < vmin) vmin = vol[i];
    if (vol[i] > vmax) vmax = vol[i];
    for (int j = i + 1; j < n; j++)
      if (fabsf(ref[i] - ref[j]) < 0.5f) { snprintf(msg, cap, "buffer values must differ by at least 0.5 pH"); return false; }
  }
  if (vmax - vmin < 0.02f) { snprintf(msg, cap, "measured voltages are almost identical - probe not responding"); return false; }

  double sv = 0, sp = 0, svv = 0, svp = 0;
  for (int i = 0; i < n; i++) {
    sv += vol[i];
    sp += ref[i];
    svv += (double)vol[i] * vol[i];
    svp += (double)vol[i] * ref[i];
  }
  double den = n * svv - sv * sv;
  if (fabs(den) < 1e-9) { snprintf(msg, cap, "calibration points are degenerate"); return false; }
  double b = (n * svp - sv * sp) / den;
  double a = (sp - b * sv) / n;
  if (fabs(b) < 1e-6) { snprintf(msg, cap, "calculated slope is zero"); return false; }
  double v7 = (7.0 - a) / b;
  if (!isfinite(b) || !isfinite(v7)) { snprintf(msg, cap, "calibration produced an invalid result"); return false; }

  float maxErr = 0;
  for (int i = 0; i < n; i++) {
    float e = fabsf((float)(a + b * vol[i]) - ref[i]);
    if (e > maxErr) maxErr = e;
  }
  if (maxErr > 0.5f) { snprintf(msg, cap, "points are inconsistent (max fit error %.2f pH) - check buffers and rinse the probe", (double)maxErr); return false; }

  Calibration nc = gCal;
  nc.phSlope = (float)b;
  nc.phVNeutral = (float)v7;
  const char *err = NULL;
  if (!validateCal(nc, &err)) { snprintf(msg, cap, "result rejected: %s", err); return false; }
  gCal = nc;
  if (!saveConfigLocked()) { snprintf(msg, cap, "applied, but saving to flash FAILED"); return false; }
  snprintf(msg, cap, "pH calibrated (%d-point): slope %.3f pH/V, %.4f V at pH 7, max fit error %.2f pH", n, b, (double)nc.phVNeutral, (double)maxErr);
  return true;
}

static bool resetConfig(const char *target, char *msg, size_t cap) {
  bool factory = !strcmp(target, "factory");
  bool all = (target[0] == 0) || !strcmp(target, "all") || factory;
  bool matched = false;
  if (all || !strcmp(target, "ph")) {
    gCal.phSlope = CAL_DEFAULT.phSlope;
    gCal.phVNeutral = CAL_DEFAULT.phVNeutral;
    matched = true;
  }
  if (all || !strcmp(target, "turbidity") || !strcmp(target, "turb")) {
    gCal.turbVClear = CAL_DEFAULT.turbVClear;
    gCal.turbVRef = CAL_DEFAULT.turbVRef;
    gCal.turbNtuRef = CAL_DEFAULT.turbNtuRef;
    matched = true;
  }
  if (all || !strcmp(target, "tds")) {
    gCal.tdsK = CAL_DEFAULT.tdsK;
    matched = true;
  }
  if (all || !strcmp(target, "temperature") || !strcmp(target, "temp")) {
    gCal.tempOffset = CAL_DEFAULT.tempOffset;
    matched = true;
  }
  if (factory || !strcmp(target, "settings")) {
    gSet = SET_DEFAULT;
    matched = true;
  }
  if (!matched) { snprintf(msg, cap, "unknown reset target"); return false; }
  if (!saveConfigLocked()) { snprintf(msg, cap, "defaults applied, but saving to flash FAILED"); return false; }
  snprintf(msg, cap, "defaults restored (%s) and saved to flash", target[0] ? target : "all calibration");
  return true;
}

static bool executeAction(const char *action, const char *json, char *msg, size_t cap) {
  Lock lock;
  if (!strcmp(action, "set_calibration") || !strcmp(action, "set_settings")) return applyConfigJson(json, msg, cap);
  if (!strcmp(action, "calibrate_ph")) return calibratePh(json, msg, cap);
  if (!strcmp(action, "reset_calibration")) {
    char tgt[16] = "";
    jsonGetString(json, "target", tgt, sizeof tgt);
    return resetConfig(tgt, msg, cap);
  }
  snprintf(msg, cap, "unknown action");
  return false;
}

static void takeSnapshot(ConfigSnapshot &s) {
  Lock lock;
  s.cal = gCal;
  s.set = gSet;
  s.saved = gSavedOk;
  s.nvsOk = gNvsOk;
}

static bool takeReading(Reading &r) {
  Lock lock;
  r = gReading;
  return r.valid;
}

// ------------------------------------------------------------------------------------------------
// JSON record builders (shared by HTTP and USB serial)
// ------------------------------------------------------------------------------------------------
static bool nearlyEqual(float a, float b) { return fabsf(a - b) <= 1e-6f * (1.0f + fabsf(b)); }

static void writeCalObject(JsonOut &j, const char *name, const Calibration &c) {
  j.objBegin(name);
  for (size_t i = 0; i < N_CAL_KEYS; i++) j.numG(CAL_KEYS[i].name, *cfld(&c, CAL_KEYS[i].off));
  j.objEnd();
}

static void writeSetObject(JsonOut &j, const char *name, const Settings &s) {
  j.objBegin(name);
  for (size_t i = 0; i < N_SET_KEYS; i++) j.numG(SET_KEYS[i].name, *cfld(&s, SET_KEYS[i].off));
  j.objEnd();
}

static void writeConfigBody(JsonOut &j, const ConfigSnapshot &s) {
  writeCalObject(j, "cal", s.cal);
  writeCalObject(j, "def", CAL_DEFAULT);
  writeSetObject(j, "set", s.set);
  writeSetObject(j, "set_def", SET_DEFAULT);
  j.boolean("saved", s.saved);
  j.boolean("nvs_ok", s.nvsOk);
  j.objBegin("mod");  // true = differs from the original defaults
  j.boolean("ph", !(nearlyEqual(s.cal.phSlope, CAL_DEFAULT.phSlope) && nearlyEqual(s.cal.phVNeutral, CAL_DEFAULT.phVNeutral)));
  j.boolean("turb", !(nearlyEqual(s.cal.turbVClear, CAL_DEFAULT.turbVClear) && nearlyEqual(s.cal.turbVRef, CAL_DEFAULT.turbVRef) &&
                      nearlyEqual(s.cal.turbNtuRef, CAL_DEFAULT.turbNtuRef)));
  j.boolean("tds", !nearlyEqual(s.cal.tdsK, CAL_DEFAULT.tdsK));
  j.boolean("temp", !nearlyEqual(s.cal.tempOffset, CAL_DEFAULT.tempOffset));
  j.objEnd();
}

static void writeDataFields(JsonOut &j, const Reading &r, float intervalMs) {
  j.u32("t", r.uptimeMs);
  j.u32("seq", r.seq);
  j.numF("ph", r.ph, 2);
  j.numF("ph_v", r.phV, 4);
  j.boolean("ph_ok", r.phOk);
  j.numF("temperature", r.tempOk ? r.temp : NAN, 2);
  j.numF("temp_raw", r.tempOk ? r.tempRaw : NAN, 2);
  j.boolean("temp_ok", r.tempOk);
  j.numF("turbidity", r.ntu, 1);
  j.numF("turb_v", r.turbV, 4);
  j.boolean("turb_ok", r.turbOk);
  j.numF("tds", r.tds, 1);
  j.numF("tds_raw", r.tdsRaw, 1);
  j.numF("tds_v", r.tdsV, 4);
  j.boolean("tds_ok", r.tdsOk);
  j.numF("tds_comp_c", r.tdsCompC, 1);
  j.boolean("tds_temp_fallback", r.tdsTempFallback);
  j.numF("interval_ms", intervalMs, 0);
}

static void writeStatusBody(JsonOut &j, const Reading &r, float intervalMs) {
  j.str("fw", FW_VERSION);
  j.str("chip", "ESP32-S3");
  j.u32("uptime_ms", millis());
  j.u32("free_heap", ESP.getFreeHeap());
  j.numF("interval_ms", intervalMs, 0);
  j.boolean("ready", r.valid);
  j.u32("seq", r.seq);
  j.objBegin("wifi");
  j.str("status", "Access point active");
  j.str("ssid", AP_SSID);
  String ip = WiFi.softAPIP().toString();
  j.str("ip", ip.c_str());
  String gw = AP_GATEWAY.toString();
  j.str("gateway", gw.c_str());
  String mk = AP_MASK.toString();
  j.str("mask", mk.c_str());
  j.u32("clients", WiFi.softAPgetStationNum());
  j.u32("channel", AP_CHANNEL);
  j.u32("max_clients", AP_MAX_CLIENTS);
  j.objEnd();
  j.objBegin("usb");
  j.u32("baud", 115200);
  j.boolean("cdc", USB_CDC_MODE != 0);
  j.objEnd();
  j.objBegin("hw");
  j.boolean("oled", gOledOk);
  j.u32("ds18b20_count", gDsCount);
  j.objEnd();
}

// ------------------------------------------------------------------------------------------------
// USB serial (runs only in the Arduino loop task)
// ------------------------------------------------------------------------------------------------
static char gTx[2048];
static char gSerBody[1800];
static char gSerMsg[200];

// Sends "PREFIX:body\n" as ONE write. If the USB buffer cannot hold the whole record it is dropped
// instead of being cut in half, so the browser never sees a broken record.
static void serialSendRecord(const char *prefix, const char *body) {
  int n = snprintf(gTx, sizeof gTx, "%s:%s\n", prefix, body);
  if (n <= 0 || n >= (int)sizeof gTx) return;
#if USB_CDC_MODE
  if (Serial.availableForWrite() < n) return;
#endif
  Serial.write(reinterpret_cast<const uint8_t *>(gTx), (size_t)n);
}

static void logLine(const char *fmt, ...) {
  char body[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof body, fmt, ap);
  va_end(ap);
  serialSendRecord("LOG", body);
}

static void emitData(const Reading &r) {
  ConfigSnapshot s;
  takeSnapshot(s);
  JsonOut j(gSerBody, sizeof gSerBody);
  j.begin();
  writeDataFields(j, r, s.set.intervalMs);
  j.end();
  if (!j.overflow) serialSendRecord("DATA", gSerBody);
}

static void emitConfig() {
  ConfigSnapshot s;
  takeSnapshot(s);
  JsonOut j(gSerBody, sizeof gSerBody);
  j.begin();
  writeConfigBody(j, s);
  j.end();
  if (!j.overflow) serialSendRecord("CFG", gSerBody);
}

static void emitStatus() {
  Reading r;
  takeReading(r);
  ConfigSnapshot s;
  takeSnapshot(s);
  JsonOut j(gSerBody, sizeof gSerBody);
  j.begin();
  writeStatusBody(j, r, s.set.intervalMs);
  j.end();
  if (!j.overflow) serialSendRecord("STAT", gSerBody);
}

static void emitAck(bool ok, const char *action, const char *msg, bool hasId, long id) {
  char body[360];
  JsonOut j(body, sizeof body);
  j.begin();
  j.boolean("ok", ok);
  j.str("action", action);
  j.str("msg", msg);
  if (hasId) j.u32("id", (uint32_t)id);
  j.end();
  serialSendRecord("ACK", body);
}

static void handleSerialLine(char *line) {
  while (*line == ' ' || *line == '\t') line++;
  if (strncmp(line, "CMD:", 4) != 0) return;  // anything else (e.g. typing in a terminal) is ignored
  const char *json = line + 4;
  char action[28] = "";
  float idf = 0;
  bool hasId = (jsonGetFloat(json, "id", &idf) == 1);
  long id = hasId ? (long)idf : 0;
  if (!jsonGetString(json, "action", action, sizeof action)) {
    emitAck(false, "", "missing or invalid action", hasId, id);
    return;
  }

  if (!strcmp(action, "ping")) {
    emitAck(true, action, "pong", hasId, id);
  } else if (!strcmp(action, "get_data")) {
    Reading r;
    if (takeReading(r)) {
      emitAck(true, action, "ok", hasId, id);
      emitData(r);
    } else {
      emitAck(false, action, "no measurement available yet", hasId, id);
    }
  } else if (!strcmp(action, "get_status")) {
    emitAck(true, action, "ok", hasId, id);
    emitStatus();
  } else if (!strcmp(action, "get_config") || !strcmp(action, "get_calibration")) {
    emitAck(true, action, "ok", hasId, id);
    emitConfig();
  } else if (!strcmp(action, "set_calibration") || !strcmp(action, "set_settings") || !strcmp(action, "calibrate_ph") ||
             !strcmp(action, "reset_calibration")) {
    bool ok = executeAction(action, json, gSerMsg, sizeof gSerMsg);
    emitAck(ok, action, gSerMsg, hasId, id);
    emitConfig();
    logLine("%s: %s", action, gSerMsg);
  } else {
    emitAck(false, action, "unknown action", hasId, id);
  }
}

static char cmdBuf[900];
static size_t cmdLen = 0;
static bool cmdOverflow = false;

static void serviceSerial() {
  int budget = 512;
  while (budget-- > 0 && Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\n' || c == '\r') {
      if (cmdOverflow) {
        emitAck(false, "", "command too long", false, 0);
      } else if (cmdLen > 0) {
        cmdBuf[cmdLen] = 0;
        handleSerialLine(cmdBuf);
      }
      cmdLen = 0;
      cmdOverflow = false;
      continue;
    }
    if (cmdLen < sizeof(cmdBuf) - 1) cmdBuf[cmdLen++] = (char)c;
    else cmdOverflow = true;
  }
}

// ------------------------------------------------------------------------------------------------
// OLED (original 2x2 layout; values come from the same Reading as the web dashboard)
// ------------------------------------------------------------------------------------------------
static bool oledProbe() {
  Wire.beginTransmission(I2C_ADDRESS);
  return Wire.endTransmission() == 0;
}

static bool oledInitOnce() {
  if (!oledProbe()) return false;
  if (!display.begin(I2C_ADDRESS, true)) return false;
  return true;
}

static void oledSplash() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(16, 18);
  display.println("Water Quality");
  display.setCursor(14, 34);
  display.println("pH | Temp | TDS");
  display.setCursor(28, 46);
  display.println("Turbidity");
  display.display();
}

static void oledNetworkScreen() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(10, 4);
  display.println("AQUA MONITOR");
  display.setCursor(0, 22);
  display.print("WiFi: ");
  display.println(AP_SSID);
  display.setCursor(0, 34);
  display.print("Pass: ");
  display.println(AP_PASS);
  display.setCursor(0, 46);
  display.print("http://192.168.4.1");
  display.display();
}

static void oledPrintValue(bool ok, float v, int decimals, int x, int y) {
  display.setTextSize(2);
  display.setCursor(x, y);
  if (ok && isfinite(v)) display.print(v, decimals);
  else display.print("ERR");
}

static void oledDraw(const Reading &r) {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  display.drawFastHLine(0, 31, 128, SH110X_WHITE);
  display.drawFastVLine(63, 0, 64, SH110X_WHITE);

  display.setTextSize(1);
  display.setCursor(2, 1);
  display.print("pH");
  oledPrintValue(r.phOk, r.ph, 2, 2, 13);

  display.setTextSize(1);
  display.setCursor(67, 1);
  display.print("TEMP (C)");
  oledPrintValue(r.tempOk, r.temp, 1, 67, 13);

  display.setTextSize(1);
  display.setCursor(2, 35);
  display.print("TURB NTU");
  display.setTextSize(2);
  display.setCursor(2, 47);
  if (r.turbOk && isfinite(r.ntu)) display.print((int)r.ntu);
  else display.print("ERR");

  display.setTextSize(1);
  display.setCursor(67, 35);
  display.print("TDS ppm");
  display.setTextSize(2);
  display.setCursor(67, 47);
  if (r.tdsOk && isfinite(r.tds)) display.print((int)r.tds);
  else display.print("ERR");

  display.display();
}

// ------------------------------------------------------------------------------------------------
// Non-blocking sampler (cooperative state machine, runs in loop())
// ------------------------------------------------------------------------------------------------
static uint16_t mvBuf[3][SAMPLES];  // 0 = pH, 1 = turbidity, 2 = TDS
static uint8_t sampleIdx = 0;
static bool cycleActive = false;
static bool adcDone = false;
static bool tempDone = false;
static uint32_t lastCycleStartMs = 0;
static uint32_t lastAdcMs = 0;
static uint32_t tempRequestMs = 0;
static uint32_t lastDsInitMs = 0;
static float cycleTempRaw = DEVICE_DISCONNECTED_C;
static bool lastTempOk = false;
static uint32_t seqCounter = 0;
static uint32_t lastOledProbeMs = 0;

static void dsInit() {
  tempSensor.begin();
  tempSensor.setResolution(12);
  tempSensor.setWaitForConversion(false);  // never block: we poll the conversion time ourselves
  gDsCount = tempSensor.getDeviceCount();
  lastDsInitMs = millis();
}

// Sorted-middle mean exactly like the original: drop the lowest 5 and highest 5 of 20, average the middle 10.
static float trimmedMeanVolts(uint16_t *a) {
  for (int i = 1; i < SAMPLES; i++) {
    uint16_t k = a[i];
    int j = i - 1;
    while (j >= 0 && a[j] > k) {
      a[j + 1] = a[j];
      j--;
    }
    a[j + 1] = k;
  }
  const int lo = SAMPLES / 4;
  const int hi = SAMPLES - SAMPLES / 4;
  uint32_t sum = 0;
  for (int i = lo; i < hi; i++) sum += a[i];
  return ((float)sum / (float)(hi - lo)) / 1000.0f;
}

static void startCycle(uint32_t now) {
  lastCycleStartMs = now;
  cycleActive = true;
  adcDone = false;
  tempDone = false;
  sampleIdx = 0;
  lastAdcMs = now - ADC_SAMPLE_GAP_MS;  // first ADC sample immediately
  // Hot-plug support: if the DS18B20 was missing / failed, re-scan the bus every 5 s.
  if ((!lastTempOk || gDsCount == 0) && (now - lastDsInitMs) > 5000) dsInit();
  tempSensor.requestTemperatures();  // returns immediately (wait-for-conversion is off)
  tempRequestMs = millis();
}

static void finishCycle() {
  ConfigSnapshot cfg;
  takeSnapshot(cfg);

  Reading r = {};
  r.valid = true;
  r.uptimeMs = millis();

  // --- Temperature (valid range check like the original, plus the DS18B20 power-on value 85.0) ---
  r.tempRaw = cycleTempRaw;
  r.tempOk = (cycleTempRaw != DEVICE_DISCONNECTED_C && cycleTempRaw > -55.0f && cycleTempRaw < 125.0f && cycleTempRaw != 85.0f);
  r.temp = r.tempOk ? cycleTempRaw + cfg.cal.tempOffset : NAN;
  lastTempOk = r.tempOk;
  r.tdsTempFallback = !r.tempOk;
  r.tdsCompC = r.tempOk ? r.temp : 25.0f;  // original: fall back to 25 C when the probe is missing

  // --- Analog channels ---
  r.phV = trimmedMeanVolts(mvBuf[0]);
  r.turbV = trimmedMeanVolts(mvBuf[1]);
  r.tdsV = trimmedMeanVolts(mvBuf[2]);

  r.ph = computePh(r.phV, cfg.cal);
  r.phOk = inRange(r.phV, ADC_V_MIN_OK, ADC_V_MAX_OK) && inRange(r.ph, 0.0f, 14.0f);

  r.ntu = computeNtu(r.turbV, cfg.cal);
  r.turbOk = inRange(r.turbV, ADC_V_MIN_OK, ADC_V_MAX_OK) && isfinite(r.ntu);

  r.tdsRaw = computeTdsRaw(r.tdsV, r.tdsCompC);
  r.tds = r.tdsRaw * cfg.cal.tdsK;
  r.tdsOk = (r.tdsV <= ADC_V_MAX_OK) && isfinite(r.tds);  // 0 V is a legal "no conductivity" reading

  r.seq = ++seqCounter;
  {
    Lock lock;
    gReading = r;
  }

  // OLED + serial use exactly the same reading as the web API
  if (gOledOk) oledDraw(r);
  emitData(r);
#if SERIAL_HUMAN_LOG
  {
    char t[48], p[16], n[16], d[16];
    if (r.tempOk) snprintf(t, sizeof t, "%.1f C", (double)r.temp);
    else snprintf(t, sizeof t, "ERR (check 4.7k resistor / wiring)");
    if (r.phOk) snprintf(p, sizeof p, "%.2f", (double)r.ph);
    else snprintf(p, sizeof p, "ERR");
    if (r.turbOk) snprintf(n, sizeof n, "%d", (int)r.ntu);
    else snprintf(n, sizeof n, "ERR");
    if (r.tdsOk) snprintf(d, sizeof d, "%d", (int)r.tds);
    else snprintf(d, sizeof d, "ERR");
    logLine("pH: %s | Turb: %s NTU | TDS: %s ppm | Temp: %s", p, n, d, t);
  }
#endif
  cycleActive = false;
}

static void serviceSampler() {
  uint32_t now = millis();
  if (!cycleActive) {
    uint32_t interval = (uint32_t)gSet.intervalMs;  // aligned 32-bit read; written only under lock
    if (now - lastCycleStartMs >= interval) startCycle(now);
    return;
  }
  if (!adcDone && (now - lastAdcMs) >= ADC_SAMPLE_GAP_MS) {
    lastAdcMs = now;
    mvBuf[0][sampleIdx] = (uint16_t)analogReadMilliVolts(PH_SENSOR_PIN);
    mvBuf[1][sampleIdx] = (uint16_t)analogReadMilliVolts(TURB_SENSOR_PIN);
    mvBuf[2][sampleIdx] = (uint16_t)analogReadMilliVolts(TDS_SENSOR_PIN);
    if (++sampleIdx >= SAMPLES) adcDone = true;
  }
  if (!tempDone && (now - tempRequestMs) >= TEMP_CONVERSION_MS) {
    cycleTempRaw = tempSensor.getTempCByIndex(0);
    tempDone = true;
  }
  if (adcDone && tempDone) finishCycle();
}

// ------------------------------------------------------------------------------------------------
// Wi-Fi access point + HTTP server (the server runs in its own FreeRTOS task)
// ------------------------------------------------------------------------------------------------
static char gHttpBuf[1900];  // used ONLY by the web task

static void httpSendJson(int code, const char *body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

static void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", WEB_UI_HTML);
}

static void handleData() {
  Reading r;
  bool ready = takeReading(r);
  JsonOut j(gHttpBuf, sizeof gHttpBuf);
  j.begin();
  j.boolean("ready", ready);
  j.u32("uptime_ms", millis());
  if (ready) {
    ConfigSnapshot s;
    takeSnapshot(s);
    writeDataFields(j, r, s.set.intervalMs);
    j.u32("age_ms", millis() - r.uptimeMs);
  }
  j.end();
  httpSendJson(200, gHttpBuf);
}

static void handleStatus() {
  Reading r;
  takeReading(r);
  ConfigSnapshot s;
  takeSnapshot(s);
  JsonOut j(gHttpBuf, sizeof gHttpBuf);
  j.begin();
  writeStatusBody(j, r, s.set.intervalMs);
  j.end();
  httpSendJson(200, gHttpBuf);
}

static void handleConfigGet() {
  ConfigSnapshot s;
  takeSnapshot(s);
  JsonOut j(gHttpBuf, sizeof gHttpBuf);
  j.begin();
  writeConfigBody(j, s);
  j.end();
  httpSendJson(200, gHttpBuf);
}

static void respondAction(bool ok, const char *msg) {
  ConfigSnapshot s;
  takeSnapshot(s);
  JsonOut j(gHttpBuf, sizeof gHttpBuf);
  j.begin();
  j.boolean("ok", ok);
  j.str("msg", msg);
  j.objBegin("cfg");
  writeConfigBody(j, s);
  j.objEnd();
  j.end();
  httpSendJson(ok ? 200 : 400, gHttpBuf);
}

static void handleActionPost(const char *action, bool bodyOptional) {
  String body = server.arg("plain");
  if (body.length() == 0 && bodyOptional) body = "{}";
  if (body.length() == 0) { httpSendJson(400, "{\"ok\":false,\"msg\":\"empty request body\"}"); return; }
  if (body.length() > 900) { httpSendJson(413, "{\"ok\":false,\"msg\":\"request body too large\"}"); return; }
  char msg[200];
  bool ok = executeAction(action, body.c_str(), msg, sizeof msg);
  respondAction(ok, msg);
}

static void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) {  // CORS preflight (dashboard served from localhost)
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.sendHeader("Access-Control-Allow-Private-Network", "true");
    server.send(204);
  } else {
    httpSendJson(404, "{\"ok\":false,\"msg\":\"not found\"}");
  }
}

static void webTask(void *arg) {
  (void)arg;
  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

static bool startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);  // lowest latency for the dashboard
  bool cfgOk = WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_MASK);
  bool apOk = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  delay(100);
  if (WiFi.softAPIP() != AP_IP) cfgOk = WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_MASK);  // belt and braces
  return cfgOk && apOk && (WiFi.softAPIP() == AP_IP);
}

static void startWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/index.html", HTTP_GET, handleRoot);
  server.on("/api/data", HTTP_GET, handleData);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/calibration", HTTP_GET, handleConfigGet);
  server.on("/api/settings", HTTP_GET, handleConfigGet);
  server.on("/api/calibration", HTTP_POST, []() { handleActionPost("set_calibration", false); });
  server.on("/api/settings", HTTP_POST, []() { handleActionPost("set_settings", false); });
  server.on("/api/calibration/ph", HTTP_POST, []() { handleActionPost("calibrate_ph", false); });
  server.on("/api/calibration/reset", HTTP_POST, []() { handleActionPost("reset_calibration", true); });
  server.on("/favicon.ico", HTTP_GET, []() { server.send(204); });
  server.onNotFound(handleNotFound);
  server.begin();
  xTaskCreatePinnedToCore(webTask, "web", 10240, NULL, 1, NULL, 0);
}

// ------------------------------------------------------------------------------------------------
// Arduino entry points
// ------------------------------------------------------------------------------------------------
void setup() {
  gMutex = xSemaphoreCreateMutex();

#if USB_CDC_MODE
  Serial.setTxBufferSize(4096);   // must be called before begin(); holds several records
  Serial.setTxTimeoutMs(0);       // never block when no USB host is attached
#endif
  Serial.begin(115200);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  loadConfig();

  // DS18B20: one blocking conversion at boot to flush the 85.0 C power-on value (as in the original),
  // after that the sampler uses non-blocking conversions.
  tempSensor.begin();
  tempSensor.setResolution(12);
  tempSensor.setWaitForConversion(true);
  tempSensor.requestTemperatures();
  (void)tempSensor.getTempCByIndex(0);
  dsInit();

  // OLED (a missing OLED no longer halts the whole system - Wi-Fi and USB keep working)
  Wire.begin(OLED_SDA, OLED_SCL);
  delay(250);
  gOledOk = oledInitOnce();
  if (gOledOk) {
    oledSplash();
    delay(1500);
  }

  bool apOk = false;
  for (int i = 0; i < 3 && !apOk; i++) apOk = startAccessPoint();
  startWebServer();

  if (gOledOk) {
    oledNetworkScreen();
    delay(2000);
  }

  lastCycleStartMs = millis() - (uint32_t)gSet.intervalMs;  // first measurement right away
  logLine("boot fw=%s ap=%s ip=%s oled=%d ds18b20=%d nvs=%d", FW_VERSION, apOk ? "ok" : "FAILED",
       WiFi.softAPIP().toString().c_str(), gOledOk ? 1 : 0, (int)gDsCount, gNvsOk ? 1 : 0);
  emitConfig();
}

void loop() {
  serviceSerial();
  serviceSampler();

  uint32_t now = millis();

  // Wi-Fi watchdog: make sure the hotspot and the fixed IP are still in place
  static uint32_t lastApCheck = 0;
  if (now - lastApCheck > 10000) {
    lastApCheck = now;
    if ((WiFi.getMode() & WIFI_AP) == 0 || WiFi.softAPIP() != AP_IP) {
      logLine("wifi: access point lost, restarting");
      startAccessPoint();
    }
  }

  // OLED hot-plug: probe (cheap) every 5 s while it is missing
  if (!gOledOk && (now - lastOledProbeMs) > 5000) {
    lastOledProbeMs = now;
    gOledOk = oledInitOnce();
    if (gOledOk) logLine("oled detected");
  }

  delay(1);  // yield; the web server has its own task
}