# Tasmota Custom Sensor Automation & Test Tool

This repository contains a complete, working solution for a 3-part assignment:

1. **Custom Tasmota sensor driver** for a BME280 (I²C) on ESP8266/ESP32  
2. **Berry automation script** that monitors the sensor, generates alerts, and logs events with rotation  
3. **Flexible Python CLI tool** for discovery/provisioning/testing with different connectivity options and with a JSON report
4. In `Binaries` folder could be found `firmware.bin` with compiled Tasmota32 + Custom sensor firmware. Its wrong way of using git, but makes this-time solution easier to check.
The implementation was validated on **Tasmota 15.2.0.4 (tasmota32)** with a **BME280 at I²C address `0x76`**.

---

## Overview

- **Custom driver** (`xsns_200_customsensor.ino`)
  - Reads BME280 temperature / humidity / pressure
  - Polls every **10 seconds**
  - Publishes readings in **Tasmota telemetry JSON** (MQTT `tele/.../SENSOR`)
  - Includes **robust retry logic**, failure counters, and **automatic re-initialization**
  - Designed to follow Tasmota conventions (I²C helpers, JSON append, Web UI hook)

- **Berry script** (e.g. `customsensor_monitor.be`)
  - Subscribes/reads the sensor values from the telemetry stream
  - Uses a **moving average** (last N=5)
  - Sends **alerts** when thresholds are exceeded
  - Logs events to flash with **rotation** (max 100 entries)
  - Additionally: supports **auto-start** and double start protection

- **Python CLI tool** (`mqtt_tester.py`)
  - Fast and reliable **async HTTP scan** (aiohttp) for Tasmota devices[^1]
  - Provision WiFi/MQTT via **HTTP** or via **Serial** (and auto-detect for both of them)[^2]
  - Subscribes to MQTT and validates **telemetry JSON format**
  - Outputs a **JSON report** and prints status/progress to console
  
  [^1] I opted for Fast Async HTTP Discovery over mDNS/UDP scan because mDNS is typically deactivated in modern Tasmota builds and UDP is often filtered by routers. I can still implement the requested methods if necessary.
  [^2] LAN provisioning is only effective if the device is already connected. For new devices in AP mode, Serial connectivity is essential as they are not yet reachable via the local network.

---

# Part 1 — Custom Tasmota Driver (BME280 I²C)

## Goal
Write a Tasmota driver (`xsns_xx_customsensor.ino`) that:

- Auto detect using chip ID
- Based on original Tasmota BME driver implementation and Bosch calibration principles
- Interfaces with a **BME280** sensor (I²C) on ESP8266/ESP32  
- Reads temperature/humidity/pressure every **10 seconds**  
- Publishes via Tasmota **MQTT telemetry**  
- Handles errors (retry logic, error reporting)  
- Follows **Tasmota coding conventions**

## Implementation Summary
**File:** `xsns_200_customsensor.ino`

Key design points:

- **Fixed I²C address** by default (`0x76`), can be overridden at compile time.
- Polling uses `FUNC_EVERY_SECOND` with a 10-second interval.
- Telemetry output is produced using `FUNC_JSON_APPEND`, so values appear under:
  - `tele/<Topic>/SENSOR`  
  - JSON object: `"CustomBME280": {"Temperature":..., "Humidity":..., "Pressure":...}`

### Error Handling
- Multiple retries per read attempt
- Counters:
  - `FailStreak` (consecutive failures)
  - `FailTotal` (total failures since boot)
- Automatic recovery:
  - after N consecutive failures the driver performs re-init (soft reset + reload calibration)

### Notes on Compatibility / Stability
A hardware-specific issue was observed where burst reads from `0xF7` could fail.
The driver therefore uses **short reads (Variant A)** to read raw P/T/H registers reliably.

## Hardware

### Wiring diagram

For ESP32 (DevKit V1):
```text
+-----------------------+           +-----------------------+
|   BME280 (Sensor)     |           |   ESP32 DevKit V1     |
|                       |           |                       |
|          [ VIN ] <----|-----------|---- [ 3.3V ]          |
|          [ GND ] <----|-----------|---- [ GND  ]          |
|          [ SCL ] <----|-----------|---- [ GPIO 22 ]       |
|          [ SDA ] <----|-----------|---- [ GPIO 21 ]       |
+-----------------------+           +-----------------------+
```

For ESP8266:
```text
+-----------------------+           +-----------------------+
|   BME280 (Sensor)     |           |   ESP8266 (NodeMCU)   |
|                       |           |                       |
|          [ VIN ] <----|-----------|---- [ 3.3V ]          |
|          [ GND ] <----|-----------|---- [ GND  ]          |
|          [ SCL ] <----|-----------|---- [ GPIO 5 / D1 ]   |
|          [ SDA ] <----|-----------|---- [ GPIO 4 / D2 ]   |
+-----------------------+           +-----------------------+
```

### Connection Table
| BME280 | ESP32 (DevKit V1) | ESP8266 |
| :--- | :--- | :--- |
| VIN | 3.3V | 3.3V |
| GND | GND | GND |
| SCL | GPIO 22 | GPIO 5 / D1 |
| SDA | GPIO 21 | GPIO 4 / D2 |

## How to Build and Flash the Custom Tasmota with custom driver

### Prerequisites
- USB cable to connect ESP8266/ESP32 board
- Computer with at least 8 GB (PlatformIO downloads ~1–2 GB of toolchains the first time)
- Git installed

### Step-by-Step Instructions
1. **Install Visual Studio Code**  
   Download and install from official site:  
   https://code.visualstudio.com/

2. **Install PlatformIO IDE extension**  
   - Open VS Code  
   - Go to Extensions view (Ctrl+Shift+X or Cmd+Shift+X on macOS)  
   - Search for **PlatformIO IDE**  
   - Install the official one by PlatformIO → restart VS Code when prompted

3. **Clone the official Tasmota repository**  
   Using git clone Tasmota repository to specific folder on PC or open terminal in VS Code (Ctrl+` or Terminal → New Terminal) and run:

   ```bash
   git clone https://github.com/arendst/Tasmota.git
   cd Tasmota```
   
4. **Switch to a specific stable commit**  
   Switch to a specific stable commit or the latest development branch (as of Feb 2026 ≈ v15.3.x Susan)

   ```git checkout development
	# or specific tag if needed:
	# git checkout v15.3.0```
	
5. **Open project in VS Code**  
   In VS Code: File → Open Folder... → select the Tasmota folder you just cloned

6. **Copy custom driver file**  
   `xsns_200_customsensor.ino` must be copied to `tasmota\tasmota_xsns_sensor\`
   
7. **Disable standard BME driver implementation**  
   In case of ESP32 open `tasmota\include\tasmota_configurations_ESP32.h` or 
   `tasmota\include\tasmota_configurations.h` in case of ESP8266 in editor and search for lines `#define USE_BMP` and `#define USE_BME68X`
   
   Put `//` comment lines before those lines to disable it. Example: `#define USE_BMP` to `//#define USE_BMP`
   
   Save changes.
   
   In case of troubles check for `tasmota_configurations_ESP32.h` in `driver` folder for example. I'm not recommeding to replace the file by
   this one due possible changes in the new Tasmota version or your own custom project changes.

8. **Add defenition for custom driver**  
   Open `tasmota\tasmota_xx2c_global\xsns_interface.ino` in editor and put next lines after first comment section:
   ```
   #ifdef USE_CUSTOM_SENSOR
   extern bool Xsns200(uint32_t function);
   #endif
   ```
   In `xsns_func_ptr` find the last sensor definition. For example:
   ```
   #ifdef XSNS_127
     &Xsns127
   #endif
   ```
   Add defenition for custom sensor:
   ```
   #ifdef XSNS_127
     &Xsns127,
   #endif

   #ifdef USE_CUSTOM_SENSOR
     &Xsns200
   #endif
   ```
   Be carefully with `,` symbol for last and penultimate sensor.

   In `kXsnsList[]` find the last sensor definition. For example:
   ```
   #ifdef XSNS_127
     XSNS_127
   #endif
   ```
   Add defenition for custom sensor:
   ```
   #ifdef XSNS_127
     XSNS_127,
   #endif

   #ifdef USE_CUSTOM_SENSOR
     200
   #endif
   ```
   Be carefully with `,` symbol for last and penultimate sensor.
   
   Save changes.
   
   In case of troubles check for `xsns_interface.ino` in `driver` folder for example. I'm not recommeding to replace the file by
   this one due possible changes in the new Tasmota version or your own custom project changes.

9. **Select build environment and build the firmware**

   Click PlatformIO icon on the left sidebar (alien head)
   
   Go to Project Tasks → `esp8266` or `esp32` section
   
   Select preferable option or use common popular one like `tasmota` or `tasmota32`
   
   Click Build (checkmark icon) next to chosen environment

10. **Optional. Find the compiled firmware**

   After successful build:
   ```
   .pio/build/tasmota/firmware.bin          ← main firmware
   .pio/build/tasmota/firmware.factory.bin  ← sometimes for initial flash
   ```

11. **Flash the firmware to your device**

   A. PlatformIO direct upload (serial)
     Connect board via USB
     Put board into flash mode (usually hold BOOT button while connecting or pressing RESET)
     In VS Code PlatformIO sidebar: click Upload (arrow icon) next to your environment

   B. Via browser, if device already runs Tasmota
     Connect to device web interface (usually 192.168.4.1 or its IP)
     Menu → Firmware Upgrade → choose firmware.bin → Start
	 
   C. using esptool (manual) or Tasmota web tool

## How to Verify

- **Auto detection**: After I2C pins activation and restarting Tasmota, driver must find the sensor automaticaly
- **Serial log**: should show sensor detection + periodic reads
- **MQTT**: `tele/<Topic>/SENSOR` should contain `CustomBME280` with numeric values
- **Tasmota Web UI**: sensor values can be rendered on the main page when the driver uses the proper web hook path

---

# Part 2 — Berry Script (Automation)

## Goal
Write a Berry script that:

- Monitors readings from Part 1
- Triggers alerts when thresholds are exceeded (example: `temp > 30°C`)
- Implements a moving average filter (last **5** readings)
- Logs events to flash with rotation (max **100** entries)

## Implementation Summary
**File:** `customsensor_monitor.be` (example name)

Features:

- Consumes sensor readings (`CustomBME280.Temperature`, etc.)
- Maintains a ring buffer (size 5) and publishes:
  - current temperature
  - moving average temperature
- Sends an alert message when thresholds are exceeded (configurable)
- Event logging:
  - stored in a file on FlashFS
  - rotation keeps max 100 entries
- Additionally:
  - protection against **double start**

## How to use
- Open file in editor `customsensor_monitor.be` and modify the User configs if needed
  - `SENSOR_NAME` must contain same name as custom sensor
  - `BASE_TOPIC` should be same as Tasmota name. 
  Berry can read this value automatically, but I made this value as constant for less resource consuming.
  Or user can define own topic for Berry script output.
- Connect to Tasmota device and open web UI
- In main menu to to `Tools` and `Manage File system`
- Select `customsensor_monitor.be` file and click `Upload` button
- Options to start the Berry script:
  - Auto start script using autoexec script
  Upload to Tasmota flash file `autoexec.be` from `Berry` folder or if file already existing, add new line:
```
load("customsensor_monitor.be")
```
  `customsensor_monitor.be` will be runned after each Tasmota restart automatically
  - Manual run
  From Tasmota main menu, open `Tools` and `Berry Scripting console`
  Write next command and click `Run code`
```
load("customsensor_monitor.be")
```
- Check for alert log:
  - Using same `Manage File system` open or download bme_alert.log
  - Log file exists and does not exceed 100 entries (oldest trimmed)
  - If file not existing, main script is not running or no alert was since first run

## How to Verify
- Check MQTT for:
  - periodic average output topic (repository-defined topic)
  - alert topic messages when thresholds are crossed
- Check FlashFS:
  - log file exists and does not exceed 100 entries (oldest trimmed)

---

# Part 3 — Python CLI Tool (Testing / Provisioning)

## Goal
Write a Python CLI tool that:

- Discovers Tasmota devices on local network (mDNS or UDP scan)
- Configures WiFi/MQTT via Tasmota HTTP API
- Subscribes to MQTT and validates sensor data format
- Generates a test report (JSON output)

## Implementation Summary
**File:** `mqtt_tester.py`

Discovery was implemented as:
- **Fast async HTTP scan** across a CIDR using `aiohttp`
- Tasmota detection probe: `Status2`
- Reads `Topic` via `Status 0` (required for `tele/<Topic>/SENSOR` validation)

Provisioning options:
- `--provision-via http`  
  Uses Tasmota HTTP API to set MQTT host/port/user/pass and TelePeriod.
- `--provision-via serial`  
  Auto-detects the correct serial port by sending `Status2` and parsing JSON, then applies settings.

Validation:
- Subscribes to `tele/<Topic>/SENSOR`
- Validates JSON payload includes:
  - top-level `Time`
  - `CustomBME280.Temperature`
  - `CustomBME280.Humidity`
  - `CustomBME280.Pressure`

Outputs:
- Human-readable progress in console
- JSON report (stdout + optional `--report-file`)

## Install

### Python
Install Pythin 3.12 or newer

### Environment Setup

The following modules are built into Python and **do not** require separate installation:
- `argparse`, `asyncio`, `ipaddress`, `json`, `queue`
- `time`, `socket`, `os`, `sys`, `dataclasses`, `typing`

External Dependencies (Manual Installation Required)
These libraries must be installed via `pip`. 

| Library | Purpose |
| :--- | :--- |
| **aiohttp** | Enables high-speed asynchronous network scanning to discover Tasmota devices. |
| **requests** | Handles synchronous HTTP POST/GET requests for device configuration (Provisioning). |
| **paho-mqtt** | Provides the MQTT client logic to validate sensor data from the broker. |
| **pyserial** | Facilitates communication with devices connected via USB/Serial ports. |

Open your terminal or command prompt and run the following command to install all necessary dependencies:

```bash
pip install aiohttp requests paho-mqtt>=2.0.0 pyserial
```

## Script usage

### 1. Discovery Arguments
These attributes control how the tool finds Tasmota devices on the network.

| Attribute | Description |
| :--- | :--- |
| `--cidr` | The network range to scan in CIDR notation (e.g., `192.168.1.0/24`). The tool will perform a fast asynchronous HTTP scan across all hosts in this range. |
| `--target-ip` | Directs the tool to a specific device IP address, bypassing the network scan. Useful for testing a single known device. |

### 2. Provisioning Arguments
These attributes define how the device is configured and what settings are applied.

| Attribute | Description |
| :--- | :--- |
| `--provision-via` | Specifies the configuration method. Options: <br> • `http`: Configures devices already on the network using the Tasmota HTTP API.<br> • `serial`: Configures a device connected via USB/UART (ideal for "fresh" devices).<br> • `none`: Skips the configuration phase. |
| `--wifi-ssid` | The SSID (name) of the WiFi network the device should connect to. (Optional if the device is already connected). |
| `--wifi-password` | The password for the specified WiFi network. |
| `--teleperiod` | Sets the interval (in seconds) at which the device sends MQTT telemetry. Default is `10` for fast validation. |

### 3. MQTT & Validation Arguments
These attributes are used to connect to the MQTT broker and validate sensor data.

| Attribute | Description |
| :--- | :--- |
| `--mqtt-host` | **(Required)** The IP address or hostname of the MQTT Broker. |
| `--mqtt-port` | The port for the MQTT Broker. Default is `1883`. |
| `--mqtt-user` | The username for MQTT authentication. |
| `--mqtt-password` | The password for MQTT authentication. |
| `--sensor-name` | The specific JSON object name to validate in the telemetry payload (e.g., `CustomBME280` or `BME280`). |
| `--mqtt-wait-sec` | Maximum time (in seconds) to wait for a valid MQTT message before marking the test as FAILED. Default is `45.0`. |

### 4. Reporting Arguments
| Attribute | Description |
| :--- | :--- |
| `--report-file` | Path to save the detailed JSON report. If omitted, the tool generates a filename automatically based on the current timestamp (e.g., `tasmota_report_20260221_120000.json`). 

### 5. Examples

Searching for Tasmota device in 192.168.1.x area, setting up WiFi credentials as *wifi-ssid* and *wifi-pass*, MQTT host as 192.168.1.100 with test_tasmota user and qwerty pass. Log in Python sonsole enabled.
```bash
python mqtt_tester.py --provision-via http --cidr 192.168.1.0/24 --wifi-ssid *wifi-ssid* --wifi-password *wifi-pass* --mqtt-host 192.168.1.100 --mqtt-user test_tasmota --mqtt-password qwerty --log
```

Searching for Tasmota device connected via USB, setting up WiFi credentials as *wifi-ssid* and *wifi-pass*, MQTT host as 192.168.1.100 with test_tasmota user and qwerty pass. Log in Python sonsole enabled.
```bash
python mqtt_tester.py --provision-via serial --cidr 192.168.1.0/24 --wifi-ssid *wifi-ssid* --wifi-password *wifi-pass* --mqtt-host 192.168.1.100 --mqtt-user test_tasmota --mqtt-password qwerty --log
```

## Technical Notes on Validation
The tool performs a **Sanity Check** on all received sensor data. A test is marked as **PASSED** only if:
1. The MQTT message is received within the timeout.
2. The payload is valid JSON.
3. Values for **Temperature**, **Humidity**, and **Pressure** are within realistic physical ranges:
    - **Temperature**: 10°C to +85°C
    - **Humidity**: 0% to 100%
    - **Pressure**: 700 hPa to 1150 hPa