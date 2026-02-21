# Tasmota Custom Sensor Automation & Test Tool

This repository contains a complete, working solution for a 3-part assignment:

1. **Custom Tasmota sensor driver** for a BME280 (I²C) on ESP8266/ESP32  
2. **Berry automation script** that monitors the sensor, generates alerts, and logs events with rotation  
3. **Python CLI tool** for discovery/provisioning/testing with a JSON test report

The implementation was validated on **Tasmota 15.2.0.4 (tasmota32)** with a **BME280 at I²C address `0x76`**.

---

## Overview (What you get)

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
  - Supports **auto-start** and protects against double start

- **Python CLI tool** (`tasmota_tool.py`)
  - Fast **async HTTP scan** (aiohttp) for Tasmota devices
  - Provision WiFi/MQTT via **HTTP** or via **Serial** (auto-detect using `Status2`)
  - Subscribes to MQTT and validates **telemetry JSON format**
  - Outputs a **JSON report** and prints status/progress to console

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

## How to Verify
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
- Includes:
  - **auto-start**
  - protection against **double start**

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
**File:** `tasmota_tool.py`

Discovery was implemented as:
- **Fast async HTTP scan** across a CIDR using `aiohttp`
- Tasmota detection probe: `Status2` (no space)
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
```bash
python -m pip install aiohttp requests paho-mqtt pyserial pyserial-tools netifaces