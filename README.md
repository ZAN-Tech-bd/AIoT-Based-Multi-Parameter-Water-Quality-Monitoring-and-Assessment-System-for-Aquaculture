# AQUA MONITOR
**AIoT-Based Multi-Parameter Water Quality Monitoring and Assessment System for Aquaculture**
ESP32-S3 firmware + offline web dashboard (Wi-Fi hotspot mode and USB Web Serial mode).

> Readings come from low-cost hobby-grade sensors. They are **indicative field measurements, not lab-certified results**, and nothing in this system says water is "safe". Status labels compare readings with *target ranges you configure*.

---

## 0. Files

| File | Purpose |
|---|---|
| `WaterQualityMonitor/WaterQualityMonitor.ino` | Firmware (sensors, calibration, OLED, USB protocol, HTTP API) |
| `WaterQualityMonitor/web_ui.h` | Dashboard embedded in firmware (served at `http://192.168.4.1/`). **Must sit next to the .ino.** |
| `index.html` | Same dashboard as one standalone file (for USB mode on your PC) |
| `make_web_ui.py` + `src/` | Rebuilds `index.html` and `web_ui.h` from source. Edit `src/*`, run `python3 make_web_ui.py` |
| `test/` | Host-side firmware tests (mocked Arduino) and headless-browser UI tests |

`index.html` and `web_ui.h` contain the identical HTML, so both modes behave the same. No CDN, no cloud, no external fonts.

---

## A. Arduino IDE setup

1. Install **ESP32 board package (Espressif) 3.0.0 or newer** (Boards Manager → "esp32").
2. Install libraries (Library Manager): **Adafruit GFX Library**, **Adafruit SH110X**, **OneWire**, **DallasTemperature**. `Wire`, `WiFi`, `WebServer`, `Preferences` come with the core.
3. Open `WaterQualityMonitor.ino` (the folder name must stay `WaterQualityMonitor`; `web_ui.h` stays inside it).
4. Tools menu:

| Setting | Value |
|---|---|
| Board | **ESP32S3 Dev Module** |
| **USB CDC On Boot** | **Enabled** (required for USB mode) |
| USB Mode | Hardware CDC and JTAG |
| Flash Size / Partition Scheme | match your module; "Default 4MB with spiffs" is fine (the dashboard adds ~120 KB) |
| Upload Speed | 921600 (use 115200 if uploads fail) |
| Serial Monitor | 115200 baud (it will show `DATA:`/`LOG:` lines, that is normal) |

5. Wiring is unchanged: OLED SDA **8**, SCL **9**; pH **4**; turbidity **5**; TDS **6**; DS18B20 **7**.
6. Upload. **Close the Serial Monitor before using USB mode in the browser**, only one program can hold the port.

---

## B. Wi-Fi mode (no PC software needed)

1. Power the ESP32. Join the Wi-Fi network **`AquaMonitor`**, password **`Aqua12345`**.
2. Open **`http://192.168.4.1`** in a browser. The dashboard is served by the ESP32 and starts streaming immediately.
3. The address is fixed (`192.168.4.1`, gateway `192.168.4.1`, mask `255.255.255.0`).

Notes
- This hotspot has no internet. Android may silently switch to mobile data: choose **"Keep connection"** when asked, or turn mobile data off.
- Use `http://`, not `https://` (browsers may auto-upgrade, retype it).
- Up to 6 clients; the ESP32 re-checks the hotspot every 10 s and restarts it if it dropped.

**HTTP API**

| Endpoint | Method | Use |
|---|---|---|
| `/api/data` | GET | Latest reading (JSON) |
| `/api/status` | GET | Firmware, uptime, heap, Wi-Fi clients, hardware |
| `/api/calibration` | GET / POST | Read / set calibration values (JSON body) |
| `/api/calibration/reset` | POST | `{"target":"ph\|turbidity\|tds\|temperature\|all\|settings"}` |
| `/api/calibration/ph` | POST | 2/3-point pH fit from buffer voltages |
| `/api/settings` | GET / POST | Target ranges, warning margin, interval |

---

## C. USB mode (PC ↔ ESP32 over the USB cable)

Browsers only allow Web Serial on a **secure context**: `https://` or **`http://localhost`**. The ESP32 page at `http://192.168.4.1` is *not* secure, so USB mode is disabled there (the dashboard tells you). Use the standalone file instead:

```bash
cd /path/to/aqua          # folder containing index.html
python3 -m http.server 5500
```
Open **http://localhost:5500** in **Chrome or Edge (desktop)** → choose **USB Serial Connection** → **Connect USB** → pick the ESP32-S3 port → allow.

- The dashboard can reconnect by itself after a cable pull or ESP32 reset (the S3 re-enumerates; this can take a few seconds).
- Protocol (115200 baud, one record per line): ESP32→PC `DATA:{…}`, `CFG:{…}`, `STAT:{…}`, `ACK:{…}`, `LOG:text`; PC→ESP32 `CMD:{"action":"…","id":N}`. Example: `CMD:{"action":"set_calibration","ph_slope":-5.7,"id":1}`. Actions: `ping`, `get_data`, `get_status`, `get_config` (alias `get_calibration`), `set_calibration`, `set_settings`, `calibrate_ph`, `reset_calibration`.

**Browser support**

| Browser | Wi-Fi mode | USB mode |
|---|---|---|
| Chrome / Edge / Opera, desktop | Yes | **Yes** |
| Chrome on Android | Yes | Web Serial not available; use Wi-Fi mode |
| Firefox (desktop) | Yes | No Web Serial |
| Safari (macOS) / **Safari & all iOS browsers** | Yes | **No Web Serial** |

---

## D. Calibration

Calibration is stored in the ESP32's flash (NVS) and survives power loss. Every save is read back and verified; the dashboard shows **"Saved in ESP32 flash (verified by read-back)"** or a warning if it could not be confirmed. Values are validated (rejects NaN/zero/out-of-range) and **Restore Defaults** is available per sensor and for all.

Original defaults: pH slope **−5.70** pH/V, **2.574 V** at pH 7; turbidity **1.957 V = 0 NTU**, **0.50 V = 1000 NTU**; TDS K **1.0**; temperature offset **0**.

Open **Calibration** in the dashboard, connect first, and wait for the **Stable** badge before every capture.

1. **pH (2 or 3 points).** Rinse the probe, put it in pH 7.00 buffer, wait until Stable, set the buffer value and press *Capture*. Repeat with pH 4.00 and/or 10.00 (rinse between). The preview shows slope, voltage at pH 7 and fit error. *Save Calibration*. Buffers must differ by ≥ 0.5 pH. Three points let you check linearity.
2. **Turbidity.** Clear water (distilled/filtered) → *Capture* for the 0 NTU voltage. Then a reference standard (e.g. a 100 NTU or 1000 NTU formazin/AMCO standard) → enter its NTU → *Capture*. The two voltages must differ by ≥ 0.05 V. Save. Without a standard, the original defaults remain valid as a rough estimate.
3. **TDS.** Put the probe in a known solution (e.g. 342 ppm NaCl or 1413 µS/cm ≈ 707 ppm @0.5), let it settle, *Capture*, enter the known ppm, *Calculate K*, Save. K = known ÷ uncorrected reading, allowed 0.1–10. Temperature compensation uses the DS18B20 value.
4. **Temperature.** Put the probe and a trusted reference thermometer in the same water, enter the reference reading, *Calculate offset*, Save (±10 °C max). The offset feeds the TDS compensation too.

**Advanced:** pH slope and neutral voltage can also be typed directly.

---

## E. Using the dashboard

- **Dashboard**: connection panel, four sensor cards (value, gauge, status, trend), assessment table. Before a connection it says *Waiting for sensor data*; nothing is simulated.
- **Analytics**: 4 live charts, 1 min–1 h window, pause/clear.
- **History**: table + **CSV export**, pause monitoring, clear. **History lives only in the browser's memory** (last 5000 readings). It is lost when the tab is closed or reloaded; export CSV first. The ESP32 stores no history.
- **Settings**: target ranges per parameter, warning margin (%), measurement interval (1–60 s). Stored on the ESP32 so every client sees the same values.
- Statuses: *Within Configured Range* / *Warning* (within the margin of a limit) / *Outside Configured Range* / *Sensor Error*. Default ranges are generic placeholders. **Set ranges for your species and system.**
- Dark/light toggle, responsive layout (bottom navigation on phones).

---

## F. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| ESP32 doesn't appear as a port | Use a data-capable USB cable; try the other USB port on the board; hold BOOT while plugging in to enter download mode |
| Upload fails | Pick the right port; lower Upload Speed; hold BOOT during upload start |
| No port in "Connect USB" list | USB CDC On Boot not enabled → re-upload with it **Enabled** |
| "Port is busy" / can't open | Close Arduino Serial Monitor, PuTTY, other tabs/programs |
| USB button disabled | Not Chrome/Edge desktop, or page opened from `192.168.4.1` or a `file://` path. Use `http://localhost:5500` |
| Dashboard won't load at 192.168.4.1 | Not on the `AquaMonitor` network; phone using mobile data; typed `https://`; try disabling mobile data |
| Wi-Fi connects but drops | Weak signal/power: use a stable 5 V supply; the AP self-heals within ~10 s |
| Wi-Fi mode "Unreachable" | ESP32 reset/out of range; dashboard retries every 2 s and recovers by itself |
| OLED blank | Check SDA 8 / SCL 9, address 0x3C; the system keeps running without OLED and re-probes every 5 s |
| `ERR` / *Sensor Error* on temperature | DS18B20 missing or no 4.7 kΩ pull-up (readings of −127 °C or exactly 85.0 °C are treated as faults) |
| pH/turbidity/TDS *Sensor Error* | Analog voltage outside 0.05–3.20 V: disconnected/shorted or wrong supply. This check is electrical only |
| TDS 0 | Probe dry or unplugged |
| pH noisy / wrong | Probe not hydrated, dirty, or old buffers; recalibrate; the ADC clips near 3.1 V |
| Calibration "NOT saved" | NVS write failed; values are active until restart; try *Restore All Defaults*, re-upload |
| Values freeze, status "Stale" | No new data for 6 s: check cable/Wi-Fi |
| Charts empty after reload | History is in memory only; it restarts at zero on reload |
| Serial Monitor shows odd text | Normal: it carries the machine protocol (`DATA:{…}`) |

---

## G. Changes from your original sketch

| Change | Why |
|---|---|
| pH slope stored **signed** (−5.70) | Matches the `ph_slope:-5.7` command spec. Formula unchanged: `pH = 7 + slope·(V−V7)`, identical results at defaults |
| Sampling is **non-blocking** (20 samples, trimmed mean, async DS18B20) | Original `delay`s and 750 ms DS18B20 wait blocked Wi-Fi/USB |
| Missing OLED no longer halts the program | The monitor still works headless |
| 85.0 °C treated as sensor error | That is the DS18B20 power-on value |
| TDS compensation temperature clamped to 0–60 °C; falls back to 25 °C if the probe is bad | Prevents absurd corrections |
| Serial output is now a machine protocol with `LOG:` lines | Required for USB mode |
| Target ranges and interval stored on the ESP32 | Same values for all clients |
| Same GPIOs, libraries and OLED 2×2 layout | Unchanged |

## H. Verification status (please read)

- Firmware: compiled with `g++ -Wall -Wextra` against **host mocks** of Arduino/ESP32 APIs (zero warnings) and run through the main logic paths (formulas vs. the original sketch, calibration maths and rejection, NVS reload, sampler, serial protocol, JSON validity). Output with the real 120 KB `web_ui.h` also compiles and passes.
- Dashboard: tested in headless Chrome against a mock HTTP ESP32 and a mocked Web Serial port (connect, data, calibration, settings, CSV, theme, mobile layout, link loss and recovery, stale-record handling).
- **Not done:** compiling with the real ESP32 core 3.x, or running on actual hardware, a real ESP32-S3 USB port, a real phone or Safari. First upload may need a small API fix if your core version differs. Please send me the compiler output if so.

Run the tests yourself: `cd test && g++ -std=gnu++17 -I mocks host_test.cpp -o host_test && ./host_test`; UI tests: `test/ui/*.py` (need Playwright + Chrome).
