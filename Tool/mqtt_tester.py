#!/usr/bin/env python3
"""
Code by Aleksandr Ruljov (alwwwex) 2026

-----------------------------------
Tasmota Testing & Provisioning Tool
-----------------------------------

- mDNS/UDP scan alternative: Fast Async HTTP Discovery.
- WiFi/MQTT Configuration: HTTP (Backlog) or Serial (UART).
- MQTT Validation: Real-time sensor data check with physical range validation.
- Report Generation: Detailed JSON output.

Usage:
  python mqtt_tester.py --provision-via http --cidr 192.168.1.0/24 --wifi-ssid *wifi-ssid* --wifi-password *wifi-pass* --mqtt-host 192.168.1.100 --mqtt-user *mqtt-user* --mqtt-password *mqtt-pass* --log 
  python mqtt_tester.py --provision-via serial --cidr 192.168.1.0/24--wifi-ssid *wifi-ssid* --wifi-password *wifi-pass* --mqtt-host 192.168.1.100 --mqtt-user *mqtt-user*  --mqtt-password *mqtt-pass* --log
  
"""

import argparse
import asyncio
import ipaddress
import json
import queue
import time
import socket
import os
import sys
from dataclasses import dataclass, asdict
from typing import Any, Dict, List, Optional

import aiohttp
import requests
import paho.mqtt.client as mqtt
from paho.mqtt.enums import CallbackAPIVersion
import serial
import serial.tools.list_ports

SHOW_LOGS = False

# -------------------- Data Structures --------------------

@dataclass
class DeviceInfo:
    ip: str
    port: int = 80
    topic: Optional[str] = None
    model: Optional[str] = None
    discovered_by: str = "scan"

@dataclass
class ProvisionResult:
    ok: bool
    method: str
    commands_sent: List[str]
    responses: Dict[str, Any]
    error: Optional[str] = None

@dataclass
class MqttValidationResult:
    ok: bool
    received_messages: int
    topic: str
    errors: List[str]
    first_payload: Optional[Dict[str, Any]] = None
    duration_sec: float = 0.0

# -------------------- Utility Functions --------------------

def utc_now_iso() -> str:
    """Returns current UTC time in ISO format."""
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

def console_log(msg: str):
    """Prints timestamped status to the console."""
    if SHOW_LOGS:
        print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)

def validate_sensor_data(payload: Dict[str, Any], sensor_name: str) -> List[str]:
    """Validates if sensor values are within realistic physical ranges."""
    errs = []
    if sensor_name not in payload:
        return [f"Sensor '{sensor_name}' not found in JSON"]
    
    s = payload.get(sensor_name, {})
    # Sanity boundaries for a typical indoor/outdoor environment
    limits = {
        "Temperature": (10.0, 85.0), 
        "Humidity": (0.0, 100.0), 
        "Pressure": (700.0, 1150.0)
    }
    
    for key, (min_v, max_v) in limits.items():
        val = s.get(key)
        if val is None:
            errs.append(f"Field {key} is missing")
        else:
            try:
                num_val = float(val)
                if not (min_v <= num_val <= max_v):
                    errs.append(f"Value {key}={num_val} out of range ({min_v}..{max_v})")
            except (ValueError, TypeError):
                errs.append(f"Field {key} is not a valid number")
                
    if s.get("Temperature") == 0.0 and s.get("Humidity") == 0.0:
        errs.append("Suspicious reading: Zero values might indicate sensor communication failure")
        
    return errs

# -------------------- Discovery Engine --------------------

async def _probe_ip(session: aiohttp.ClientSession, ip: str, port: int) -> Optional[DeviceInfo]:
    """Probes a single IP for a Tasmota identity."""
    url = f"http://{ip}:{port}/cm?cmnd=Status"
    try:
        async with session.get(url, timeout=1.5) as r:
            if r.status == 200:
                data = await r.json(content_type=None)
                if "Status" in data:
                    st = data["Status"]
                    return DeviceInfo(
                        ip=ip, 
                        port=port,
                        topic=st.get("Topic", "unknown"),
                        model=st.get("DeviceName") or st.get("FriendlyName", "Tasmota"),
                        discovered_by="http-probe"
                    )
    except:
        pass
    return None

async def scan_network(cidr: str, port: int, concurrency: int) -> List[DeviceInfo]:
    """Scans a network range concurrently."""
    network = ipaddress.ip_network(cidr, strict=False)
    ips = [str(ip) for ip in network.hosts()]
    
    connector = aiohttp.TCPConnector(limit=concurrency)
    async with aiohttp.ClientSession(connector=connector) as session:
        tasks = [_probe_ip(session, ip, port) for ip in ips]
        results = await asyncio.gather(*tasks)
    
    return [r for r in results if r]

# -------------------- Provisioning Logic --------------------

def provision_via_http(dev: DeviceInfo, args: argparse.Namespace) -> ProvisionResult:
    """Configures Tasmota using a single HTTP Backlog command."""
    backlog = []
    if args.wifi_ssid:
        backlog.append(f"SSID1 {args.wifi_ssid}")
        if args.wifi_password: backlog.append(f"Password1 {args.wifi_password}")
    
    backlog.extend([
        f"MqttHost {args.mqtt_host}", 
        f"MqttPort {args.mqtt_port}", 
        f"TelePeriod {args.teleperiod}"
    ])
    if args.mqtt_user: backlog.append(f"MqttUser {args.mqtt_user}")
    if args.mqtt_password: backlog.append(f"MqttPassword {args.mqtt_password}")
    backlog.append("Restart 1")
    
    full_cmd = "Backlog " + "; ".join(backlog)
    try:
        r = requests.get(f"http://{dev.ip}:{dev.port}/cm", params={"cmnd": full_cmd}, timeout=5)
        return ProvisionResult(True, "http", [full_cmd], r.json())
    except Exception as e:
        return ProvisionResult(False, "http", [full_cmd], {}, str(e))

def provision_via_serial(port: str, args: argparse.Namespace) -> ProvisionResult:
    """Configures Tasmota via Serial (UART)."""
    cmds, resps = [], {}
    try:
        with serial.Serial(port, 115200, timeout=2.0) as ser:
            time.sleep(1.5)
            ser.reset_input_buffer()
            def send(c):
                cmds.append(c); ser.write(f"{c}\n".encode()); time.sleep(0.5)
                resps[c] = ser.read_all().decode(errors="ignore")
            
            if args.wifi_ssid:
                send(f"SSID1 {args.wifi_ssid}")
                if args.wifi_password: send(f"Password1 {args.wifi_password}")
            send(f"MqttHost {args.mqtt_host}")
            send(f"MqttPort {args.mqtt_port}")
            if args.mqtt_user: send(f"MqttUser {args.mqtt_user}")
            if args.mqtt_password: send(f"MqttPassword {args.mqtt_password}")
            send(f"TelePeriod {args.teleperiod}")
            send("Restart 1")
            return ProvisionResult(True, "serial", cmds, resps)
    except Exception as e:
        return ProvisionResult(False, "serial", cmds, resps, str(e))

# -------------------- MQTT Validation --------------------

def validate_mqtt_sensor(args, topic):
    """Subscribes to MQTT and validates telemetry using Paho-MQTT v2 API."""
    sub_topic = f"tele/{topic}/SENSOR"
    msg_queue = queue.Queue()
    errors = []

    def on_connect(client, userdata, flags, rc, properties):
        if rc == 0: client.subscribe(sub_topic)
        else: errors.append(f"MQTT Conn Failed: {rc}")

    def on_message(client, userdata, msg):
        msg_queue.put(msg.payload.decode())

    client = mqtt.Client(CallbackAPIVersion.VERSION2)
    if args.mqtt_user:
        client.username_pw_set(args.mqtt_user, args.mqtt_password)
    
    t0 = time.time()
    try:
        client.on_connect, client.on_message = on_connect, on_message
        client.connect(args.mqtt_host, args.mqtt_port, 60)
        client.loop_start()

        while time.time() - t0 < args.mqtt_wait_sec:
            try:
                raw_json = msg_queue.get(timeout=1)
                data = json.loads(raw_json)
                v_errs = validate_sensor_data(data, args.sensor_name)
                if not v_errs:
                    return MqttValidationResult(True, 1, sub_topic, [], data, time.time()-t0)
                else:
                    errors.extend(v_errs)
            except queue.Empty: continue
            except json.JSONDecodeError: errors.append("Invalid JSON received")
    except Exception as e:
        errors.append(f"MQTT Library Error: {e}")
    finally:
        client.loop_stop()
        
    return MqttValidationResult(False, 0, sub_topic, list(set(errors)) or ["No valid telemetry received"], None, time.time()-t0)

# -------------------- Main Orchestrator --------------------

def main():
    global SHOW_LOGS
    p = argparse.ArgumentParser(description="Tasmota Provisioning & Testing CLI")
    p.add_argument("--cidr", help="Network CIDR (e.g. 192.168.1.0/24)")
    p.add_argument("--target-ip", help="Single device IP target")
    p.add_argument("--provision-via", choices=["http", "serial", "none"], default="http")
    p.add_argument("--mqtt-host", required=True)
    p.add_argument("--mqtt-port", type=int, default=1883)
    p.add_argument("--mqtt-user")
    p.add_argument("--mqtt-password")
    p.add_argument("--wifi-ssid")
    p.add_argument("--wifi-password")
    p.add_argument("--sensor-name", default="CustomBME280")
    p.add_argument("--teleperiod", type=int, default=10)
    p.add_argument("--mqtt-wait-sec", type=float, default=45.0)
    p.add_argument("--report-file", help="Path to save the JSON report")
    p.add_argument("--log", action="store_true", help="Enable verbose console logging")
    args = p.parse_args()

    SHOW_LOGS = args.log
    report_path = args.report_file or f"tasmota_report_{time.strftime('%Y%m%d_%H%M%S')}.json"
    devices: List[DeviceInfo] = []
    prov_results: Dict[str, ProvisionResult] = {}
    mqtt_results: Dict[str, MqttValidationResult] = {}

    console_log("=== Tasmota Automation Tool Started ===")

    # PHASE 1: Provisioning & Discovery Logic
    if args.provision_via == "serial":
        # Handle Serial first, then find the device on network
        ports = [p.device for p in serial.tools.list_ports.comports()]
        found_port = None
        for pt in ports:
            try:
                with serial.Serial(pt, 115200, timeout=1) as s:
                    s.write(b"\nStatus\n")
                    if b"Status" in s.read(128): found_port = pt; break
            except: continue
        
        if found_port:
            res = provision_via_serial(found_port, args)
            prov_results[found_port] = res
            console_log(f"Provision Serial {found_port}: {'OK' if res.ok else 'FAIL'}")
            
            if res.ok and args.cidr:
                console_log("Waiting 20s for device to reboot and join WiFi...")
                time.sleep(20)
                console_log(f"Scanning network {args.cidr} to locate device...")
                devices = asyncio.run(scan_network(args.cidr, 80, 200))
        else:
            console_log("Error: No Tasmota found on Serial ports.")

    else:
        # Standard Network Discovery (HTTP or None)
        if args.target_ip:
            devices.append(DeviceInfo(args.target_ip, discovered_by="manual"))
        elif args.cidr:
            console_log(f"Scanning network {args.cidr}...")
            devices = asyncio.run(scan_network(args.cidr, 80, 200))
            console_log(f"Discovered {len(devices)} device(s).")
        
        if args.provision_via == "http" and devices:
            for d in devices:
                res = provision_via_http(d, args)
                prov_results[d.ip] = res
                console_log(f"Provision HTTP {d.ip}: {'OK' if res.ok else 'FAIL'}")
            console_log("Waiting 15s for devices to stabilize...")
            time.sleep(15)

    # PHASE 2: Validation
    if devices:
        console_log("Starting MQTT Telemetry Validation...")
        for d in devices:
            # Refresh topic if unknown from initial probe
            if not d.topic or d.topic == "unknown":
                try:
                    r = requests.get(f"http://{d.ip}/cm?cmnd=Status", timeout=3).json()
                    d.topic = r['Status']['Topic']
                except: pass
            
            if d.topic:
                v_res = validate_mqtt_sensor(args, d.topic)
                mqtt_results[d.ip] = v_res
                console_log(f"Test Result {d.ip}: {'PASSED' if v_res.ok else 'FAILED'}")
            else:
                console_log(f"Skipping {d.ip}: Could not determine MQTT topic.")
    else:
        console_log("No devices found for validation.")

    # PHASE 3: Reporting
    final_report = {
        "report_timestamp": utc_now_iso(),
        "summary": {
            "devices_found": len(devices),
            "provisioning_count": len(prov_results),
            "successful_validations": sum(1 for v in mqtt_results.values() if v.ok)
        },
        "config_used": {k: v for k, v in vars(args).items() if "password" not in k},
        "discovered_devices": [asdict(d) for d in devices],
        "provisioning_logs": {k: asdict(v) for k, v in prov_results.items()},
        "mqtt_validation": {k: asdict(v) for k, v in mqtt_results.items()}
    }

    with open(report_path, "w", encoding="utf-8") as f:
        json.dump(final_report, f, indent=2, ensure_ascii=False)
    
    if not SHOW_LOGS:
        print(json.dumps(final_report, indent=2, ensure_ascii=False))
    else:
        console_log(f"Detailed JSON report saved: {report_path}")
        console_log("=== Process Finished ===")

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        if SHOW_LOGS:
            console_log("Interrupted by user.")
        sys.exit(1)