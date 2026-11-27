# AQUA MONITOR

**AIoT-based multi-parameter water-quality monitoring and assessment system for aquaculture, built on an ESP32-S3 with a live web dashboard.**

AQUA MONITOR measures **pH, temperature, total dissolved solids (TDS) and turbidity**. You can read the values on the onboard OLED display or in a browser, over the ESP32's own Wi-Fi hotspot or over a USB serial connection.

> **Important:** This project uses low-cost sensors and is meant for educational and indicative field monitoring. Readings are not laboratory-certified. Do not rely on them alone to decide whether water is safe or suitable for a species. Set target ranges for your own species and system, and check critical measurements with trusted instruments.

![AQUA MONITOR assembled system](docs/images/deployment/completed-system.jpg)

## Contents

1. [Features](#features)
2. [System overview](#system-overview)
3. [Repository structure](#repository-structure)
4. [Hardware and wiring](#hardware-and-wiring)
5. [Quick start: run the project](#quick-start-run-the-project)
6. [Connect to the dashboard](#connect-to-the-dashboard)
7. [Dashboard guide](#dashboard-guide)
8. [Calibration and settings](#calibration-and-settings)
9. [HTTP API](#http-api)
10. [Developing the dashboard](#developing-the-dashboard)
11. [Testing and field deployment](#testing-and-field-deployment)
12. [Troubleshooting](#troubleshooting)
13. [Limitations and safety](#limitations-and-safety)

## Features

- **Four sensors:** pH, DS18B20 temperature, TDS and turbidity.
- **Two connection modes:** Wi-Fi hotspot (HTTP API) or USB serial (Web Serial). Both use the same dashboard.
- **Onboard OLED:** shows readings with no phone or computer. The firmware keeps running if the OLED is missing.
- **Live dashboard:** gauges, sparkline trends and a status for each parameter against configurable target ranges.
- **Analytics:** live charts with selectable time windows.
- **History and export:** keeps up to 5,000 readings in the open tab and exports them to CSV.
- **Guided pH calibration:** two- or three-point buffer calibration (pH 4.00, 7.00, 10.00) fitted by least squares. Also supports turbidity, TDS and temperature calibration.
- **Persistent configuration:** calibration and settings are stored in ESP32 flash and verified by reading them back. Factory defaults are never overwritten, and you can restore them at any time.
- **Sensor plausibility checks:** analog voltages outside 0.05–3.20 V and temperature probe faults are flagged.
- **Offline and self-contained:** no internet, cloud account or external services needed.
- **Printable enclosure:** STL and Fusion 360 files are included.

## System overview

The ESP32-S3 samples the sensors, applies calibration and serves the dashboard locally. The dashboard can connect to the ESP32 access point or directly to the board's USB serial port.

```mermaid
flowchart LR
    PH[pH sensor] --> ESP[ESP32-S3]
    TURB[Turbidity sensor] --> ESP
    TDS[TDS sensor] --> ESP
    TEMP[DS18B20 temperature probe] --> ESP
    ESP --> OLED[128 x 64 OLED]
    ESP -->|Wi-Fi access point and HTTP| WIFI[Phone or computer browser]
    ESP -->|USB CDC serial| USB[Desktop browser with Web Serial]
```

The access point is named `AquaMonitor` and uses the address `192.168.4.1`. It does not provide internet access.

## Repository structure

```text
.
├── firmware/
│   └── WaterQualityMonitor/            # Arduino sketch folder (name must match the .ino)
│       ├── WaterQualityMonitor.ino     # ESP32-S3 firmware
│       └── web_ui.h                    # Dashboard embedded in the firmware (generated)
├── web/
│   └── index.html                      # Dashboard source, also used for USB mode
├── tools/
│   └── make_web_ui.py                  # Regenerates web_ui.h from web/index.html
├── docs/
│   ├── cad/                            # Enclosure STL and Fusion 360 files
│   ├── images/
│   │   ├── hardware/                   # Component photos
│   │   ├── testing/                    # Calibration and test-bench photos
│   │   └── deployment/                 # Assembled system and field photos
│   ├── screenshots/
│   │   ├── live/                       # Dashboard screenshots with live data
│   │   └── offline/                    # Dashboard screenshots while disconnected
│   └── video/                          # Project demonstration video
└── README.md
```

`web_ui.h` is generated from `web/index.html`. Do not edit it by hand.

## Hardware and wiring

| Component | ESP32-S3 connection |
|---|---:|
| OLED SDA | GPIO 8 |
| OLED SCL | GPIO 9 |
| pH sensor analog output | GPIO 4 |
| Turbidity sensor analog output | GPIO 5 |
| TDS sensor analog output | GPIO 6 |
| DS18B20 data | GPIO 7 |

Place a **4.7 kΩ pull-up resistor** between the DS18B20 data line and its supply. The OLED uses I²C address `0x3C`. Check the pinout and voltage requirements of your ESP32-S3 board and sensor modules before wiring. Analog inputs must stay within the board's supported range.

| ESP32-S3 board | pH sensor module | pH probe kit |
|:---:|:---:|:---:|
| <img src="docs/images/hardware/esp32-s3-board.jpeg" alt="ESP32-S3 board" width="260"> | <img src="docs/images/hardware/ph-sensor-module.jpeg" alt="pH sensor module" width="260"> | <img src="docs/images/hardware/ph-sensor-probe-kit.jpeg" alt="pH probe, module and manual" width="260"> |

| TDS sensor | OLED display |
|:---:|:---:|
| <img src="docs/images/hardware/tds-sensor.jpg" alt="TDS sensor" width="260"> | <img src="docs/images/hardware/oled-display.jpeg" alt="OLED display" width="260"> |

The enclosure design is in [`docs/cad/`](docs/cad/): two STL bodies for printing and the editable Fusion 360 file (`.f3z`).

## Quick start: run the project

There are three steps: upload the firmware, power the board, then open the dashboard.

### 1. Install the requirements

- [Arduino IDE](https://www.arduino.cc/en/software) 2.x
- Espressif **ESP32 board package 3.0.0 or newer** (Boards Manager)
- These libraries, installed through Library Manager:
  - Adafruit GFX Library
  - Adafruit SH110X
  - OneWire
  - DallasTemperature
- Python 3, only if you want USB mode or need to rebuild the dashboard

`Wire`, `WiFi`, `WebServer` and `Preferences` come with the ESP32 Arduino core.

### 2. Upload the firmware

1. In Arduino IDE, choose **File → Open** and select `firmware/WaterQualityMonitor/WaterQualityMonitor.ino`. Keep `web_ui.h` in the same folder, because the sketch includes it.
2. Select **Tools → Board → ESP32S3 Dev Module** and set:

   | Arduino IDE setting | Value |
   |---|---|
   | USB CDC On Boot | **Enabled** (required for USB dashboard mode) |
   | USB Mode | Hardware CDC and JTAG |
   | Flash size / partition | Match your module. 4 MB with the default partition works. |
   | Upload speed | 921600. Lower it to 115200 if uploads fail. |
   | Serial Monitor | 115200 baud |

3. Wire the sensors as in the [pin table](#hardware-and-wiring), connect the board by USB, select its port and click **Upload**.

The serial monitor shows machine-protocol lines such as `DATA:` and `LOG:`. This is expected. Close the serial monitor before using USB mode, because only one program can hold the port.

### 3. Open the dashboard

Pick one of the two modes below. Wi-Fi mode is the quickest way to start.

## Connect to the dashboard

### Wi-Fi hotspot

1. Power on the ESP32-S3. Connect your phone or computer to the **`AquaMonitor`** network.
2. Enter the default password **`Aqua12345`**.
3. Open **http://192.168.4.1** in a browser. Use `http://`, not `https://`.

The dashboard starts receiving readings automatically. The access point supports up to six clients. Because the network has no internet, a phone may offer to switch to mobile data. Choose to stay connected.

### USB serial

USB mode needs a browser with Web Serial and a secure origin, so serve the dashboard from `localhost`:

```bash
# run from the repository root
python3 -m http.server 5500 --directory web
```

On Windows, use `python` or `py` if `python3` is not available. Then:

1. Open **http://localhost:5500** in desktop Chrome or Edge.
2. Select **USB Serial Connection**, choose the ESP32-S3 port and grant access.
3. Make sure **USB CDC On Boot** is enabled in the firmware build and the Arduino Serial Monitor is closed.

| Browser or device | Wi-Fi mode | USB mode |
|---|:---:|:---:|
| Desktop Chrome or Edge | Yes | Yes |
| Firefox or Safari | Yes | No |
| Android browser | Yes | No |
| iPhone or iPad browser | Yes | No |

## Dashboard guide

| Page | What it does |
|---|---|
| **Dashboard** | Connection controls and status, live gauges with trend sparklines, and a water-quality assessment table. |
| **Analytics** | Live charts for each sensor with selectable time windows. |
| **History** | Pause monitoring and export readings as CSV. |
| **Calibration** | Calibrate each sensor, review active and default values, and restore defaults. |
| **Settings** | Set target ranges, warning margin and sample interval. |

History is kept only in the open browser tab (up to 5,000 readings). The ESP32 does not store it, and it is lost when the page is reloaded or closed. Export a CSV to keep it.

### Screenshots with live data

| Dashboard | Calibration |
|:---:|:---:|
| <img src="docs/screenshots/live/dashboard.png" alt="Dashboard with live readings" width="480"> | <img src="docs/screenshots/live/calibration.png" alt="pH calibration page" width="480"> |
| **Analytics** | **History** |
| <img src="docs/screenshots/live/analytics.png" alt="Analytics charts" width="480"> | <img src="docs/screenshots/live/history.png" alt="History table and CSV export" width="480"> |

<details>
<summary>Settings page and disconnected-state screenshots</summary>

| Settings (live) | Dashboard (disconnected) |
|:---:|:---:|
| <img src="docs/screenshots/live/settings.png" alt="Settings page" width="480"> | <img src="docs/screenshots/offline/dashboard.png" alt="Dashboard while disconnected" width="480"> |
| **Analytics (disconnected)** | **Calibration (disconnected)** |
| <img src="docs/screenshots/offline/analytics.png" alt="Analytics while disconnected" width="480"> | <img src="docs/screenshots/offline/calibration.png" alt="Calibration while disconnected" width="480"> |
| **History (disconnected)** | **Settings (disconnected)** |
| <img src="docs/screenshots/offline/history.png" alt="History while disconnected" width="480"> | <img src="docs/screenshots/offline/settings.png" alt="Settings while disconnected" width="480"> |

</details>

## Calibration and settings

Calibration and target settings are stored in ESP32 flash and persist across power cycles. The firmware reads saved values back to verify them. Use the dashboard's **Calibration** and **Settings** pages, and wait for the reading to stabilize before capturing a calibration point.

**pH calibration:** rinse the probe, place it in a buffer (4.00, 7.00 or 10.00), wait for the **Stable** badge, then press **Capture current voltage**. Capture at least two buffers (three is better) and press **Save Calibration**.

Factory calibration starting values:

| Measurement | Default |
|---|---|
| pH slope / voltage at pH 7 | −5.70 pH/V / 2.574 V |
| Turbidity clear-water point | 1.957 V = 0 NTU |
| Turbidity reference point | 0.50 V = 1000 NTU |
| TDS correction factor | 1.0 |
| Temperature offset | 0 °C |

These are starting estimates, not a substitute for calibration with known buffers, standards or reference instruments. The default assessment ranges are generic placeholders. Set them for your species and system before you read the status labels.

## HTTP API

The ESP32 serves these local endpoints at `http://192.168.4.1`:

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/data` | GET | Latest sensor reading |
| `/api/status` | GET | Firmware, uptime, memory, access point and hardware status |
| `/api/calibration` | GET | Read calibration and current settings |
| `/api/calibration` | POST | Set calibration values |
| `/api/calibration/ph` | POST | Calculate pH calibration from buffer points |
| `/api/calibration/reset` | POST | Reset calibration values |
| `/api/settings` | GET | Read calibration and current settings |
| `/api/settings` | POST | Update target ranges and sample interval |

The browser dashboard is the recommended client for calibration and settings. It sends the expected request fields and shows the firmware's responses.

## Developing the dashboard

`web/index.html` is the single source for the dashboard. `firmware/WaterQualityMonitor/web_ui.h` embeds the same page in the firmware. After editing `index.html`, regenerate the header from the repository root:

```bash
python3 tools/make_web_ui.py
```

Then re-upload the firmware, and commit both files together so Wi-Fi and USB modes keep showing the same dashboard.

## Testing and field deployment

Calibration and bench testing used pH buffer solutions, distilled water and a multimeter to check sensor voltages.

| pH sensor test | Mixing test solutions | Test setup |
|:---:|:---:|:---:|
| <img src="docs/images/testing/ph-sensor-test.jpg" alt="pH sensor test" width="260"> | <img src="docs/images/testing/solution-mixing.jpg" alt="Mixing test solutions" width="260"> | <img src="docs/images/testing/test-setup.jpg" alt="Project test setup" width="260"> |

| Distilled water | Multimeter check |
|:---:|:---:|
| <img src="docs/images/testing/distilled-water.jpg" alt="Distilled water bottle" width="260"> | <img src="docs/images/testing/multimeter.jpeg" alt="Multimeter used for voltage checks" width="260"> |

The enclosed monitor was mounted inside a floating ring for field testing, with the dashboard viewed on a laptop over the Wi-Fi hotspot.

| Live dashboard on laptop | Monitor close-up |
|:---:|:---:|
| <img src="docs/images/deployment/live-dashboard-on-laptop.jpeg" alt="Dashboard showing live readings next to the floating monitor" width="380"> | <img src="docs/images/deployment/monitor-closeup.jpeg" alt="Purple enclosure with pH probe and OLED inside the ring" width="380"> |

| Wide view | Front view | Angle view |
|:---:|:---:|:---:|
| <img src="docs/images/deployment/deployed-wide-view.jpeg" alt="Wide view of the deployment" width="260"> | <img src="docs/images/deployment/deployed-front-view.jpeg" alt="Front view of the deployment" width="260"> | <img src="docs/images/deployment/deployed-angle-view.jpeg" alt="Angled view of the deployment" width="260"> |

[Watch the project demonstration video](docs/video/project-demo.mp4).

## Troubleshooting

| Symptom | Checks |
|---|---|
| Sketch won't compile: `web_ui.h` not found | Open the `.ino` from `firmware/WaterQualityMonitor/` and keep `web_ui.h` in that folder. |
| Board does not appear as a port | Use a data-capable USB cable, try another port, or hold BOOT while connecting to enter download mode. |
| Upload fails | Confirm the board and port, lower the upload speed, or hold BOOT as the upload begins. |
| No port appears in the browser | Enable **USB CDC On Boot**, re-upload, and close the Arduino Serial Monitor and any other serial program. |
| USB mode is unavailable | Use desktop Chrome or Edge and open the page from `http://localhost`. USB mode does not work from `192.168.4.1` or `file://`. |
| Dashboard does not load | Join the `AquaMonitor` network, use `http://192.168.4.1`, and keep the phone connected to the hotspot. |
| OLED is blank | Check SDA on GPIO 8, SCL on GPIO 9 and I²C address `0x3C`. The monitor keeps working without the OLED. |
| Temperature shows an error | Check the DS18B20 wiring and the 4.7 kΩ pull-up. |
| Analog sensor reports an error | Check sensor wiring and supply. The firmware flags analog voltages outside its 0.05–3.20 V window. |
| Calibration is not saved | Check the dashboard's save status and restore defaults if needed. |
| History is empty after reopening the page | History is held in the browser tab only. Export a CSV before closing or reloading. |

## Limitations and safety

- Sensor quality, placement, ageing and calibration all affect accuracy.
- Status labels compare readings with your configured ranges. They are not a water-safety determination.
- The default Wi-Fi password is public in the firmware and this document. Change `AP_PASS` in `WaterQualityMonitor.ino` before deploying anywhere unauthorized access is a concern.
- The Wi-Fi hotspot is local-only and gives no cloud access.
- The device has no real-time clock. Dashboard timestamps use the browser device's local time.
- Test the firmware on your chosen ESP32 core and hardware before relying on it in an operational aquaculture setting.
