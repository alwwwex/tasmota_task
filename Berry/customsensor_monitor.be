# customsensor_monitor.be
# fixed topic, moving average, alerts, rotated flash log
# code by Aleksandr Ruljov (alwwwex) 2026

import json
import mqtt

# ---------------- USER CONFIG ----------------
var BASE_TOPIC     = "tasmota_EA7D58"   # <<< SET YOUR DEVICE TOPIC HERE

var SENSOR_NAME    = "CustomBME280"
var PERIOD_MS      = 10000
var MA_N           = 5
var TEMP_TH_C      = 30.0
var HYST_C         = 0.5

var LOG_FILE       = "bme_alert.log"
var LOG_MAX        = 100

# ---------------- SHOWING IN BOth CONSOLES ----------------

def debug_log(msg)
  print(msg)            # Berry Console
  tasmota.log(msg, 2)   # Tasmota common log
end

# ---- double-start guard ----
if global.contains("custom_monitor_running")
  debug_log("customsensor_monitor: already running, skip start")
  return true
end

# ----------------   ----------------

def stop()
  tasmota.remove_timer(200)
  
  if global.contains("custom_monitor_running")
    global.custom_monitor_running = nil
  end
  
  debug_log("Сustomsensor_monitor STOPPED and cleared from memory")
end

# ---------------- INTERNAL STATE ----------------
var _temps = []
var _alarm_active = false

# ---------------- TIMER ----------------
def set_timer_modulo(delay, f, id)
  var now = tasmota.millis()
  tasmota.set_timer((now + delay/4 + delay)/delay*delay - now,
                    def() set_timer_modulo(delay, f, id) f() end,
                    id)
end

# ---------------- MQTT TOPICS ----------------
def avg_topic()
  return "tele/" + BASE_TOPIC + "/CustomBME280_AVG"
end

def alert_topic()
  return "stat/" + BASE_TOPIC + "/CustomBME280_ALERT"
end

# ---------------- LOG ROTATION ----------------
def load_log()
  var lines = []
  try
    var f = open(LOG_FILE, "r")
    while true
      var l = f.readln()
      if l == nil break end
      if size(l) > 0 lines.push(l) end
    end
    f.close()
  except .. as e, m
  end
  return lines
end

def save_log(lines)
  var f = open(LOG_FILE, "w")
  for l : lines
    f.write(l)
    f.write("\n")
  end
  f.close()
end

def log_event(ev)
  var lines = load_log()
  lines.push(json.dump(ev))
  while size(lines) > LOG_MAX
    lines.remove(0)
  end
  save_log(lines)
end

# ---------------- SENSOR READ ----------------
def read_sensor()
  var s = tasmota.read_sensors()
  if s == nil return nil end

  var m = json.load(s)
  if m == nil return nil end
  if m.find(SENSOR_NAME) == nil return nil end

  var b = m[SENSOR_NAME]
  if b.find("Temperature") == nil return nil end

  var t = real(b["Temperature"])

  var unit = "C"
  if m.find("TempUnit") != nil
    unit = m["TempUnit"]
  end

  if unit == "F"
    t = (t - 32.0) * (5.0/9.0)
  end

  return {
    "ts": m.find("Time") != nil ? m["Time"] : nil,
    "t_c": t
  }
end

# ---------------- MOVING AVERAGE ----------------
def push_temp(t)
  _temps.push(t)
  while size(_temps) > MA_N
    _temps.remove(0)
  end
end

def avg_temp()
  var s = 0.0
  for v : _temps
    s += v
  end
  return s / size(_temps)
end

# ---------------- MAIN LOOP ----------------
def poll_once()
  var r = read_sensor()
  if r == nil return end

  push_temp(r["t_c"])
  var avg = avg_temp()

  # Publish actual + average
  var payload = {
    "ts": r["ts"],
    "temp_c": r["t_c"],
    "temp_avg_c": avg
  }
  mqtt.publish(avg_topic(), json.dump(payload))

  # Alert only when buffer full
  if size(_temps) < MA_N return end

  if !_alarm_active && avg > TEMP_TH_C
    _alarm_active = true
    var ev = {
      "ts": r["ts"],
      "event": "TEMP_HIGH",
      "temp_avg_c": avg,
      "temp_c": r["t_c"]
    }
    log_event(ev)
    mqtt.publish(alert_topic(), json.dump(ev))

  elif _alarm_active && avg < (TEMP_TH_C - HYST_C)
    _alarm_active = false
    var ev2 = {
      "ts": r["ts"],
      "event": "TEMP_NORMAL",
      "temp_avg_c": avg,
      "temp_c": r["t_c"]
    }
    log_event(ev2)
    mqtt.publish(alert_topic(), json.dump(ev2))
  end
end

# ---------------- START ----------------
def start()
  global.custom_monitor_running = true
  
  debug_log("customsensor_monitor started")

  poll_once()
  set_timer_modulo(PERIOD_MS, poll_once, 200)   # timer id fixed
end

start()
