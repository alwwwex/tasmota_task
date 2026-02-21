/*********************************************************************************************\
 * xsns_200_customsensor.ino - Custom Tasmota driver for BME280 (I2C) on ESP8266/ESP32
 *
 * - Fixed I2C address (default 0x76) with compile-time override
 * - Reads temperature/humidity/pressure every 10 seconds
 * - Publishes via Tasmota telemetry JSON (FUNC_JSON_APPEND)
 * - Error holding: retry logic, error counters, self-recovery (soft reset + reload calibration)
 * - Follows Tasmota conventions for Web UI (Convert*, Settings resolution)
 *
 * Code by Aleksandr Ruljov (alwwwex) 2026
 * Based on original Tasmota BME driver implementation and Bosch calibration principles
 *
\*********************************************************************************************/

#ifdef USE_CUSTOM_SENSOR
#ifdef USE_I2C

#define XSNS_200 200

// -------- User-configurable ----------
#ifndef CUSTOM_BME280_ADDR
#define CUSTOM_BME280_ADDR  0x76   // change to 0x77 if needed
#endif

#ifndef CUSTOM_BME280_BUS
#define CUSTOM_BME280_BUS   0      // i2c bus 0
#endif

#ifndef CUSTOM_BME280_NAME
#define CUSTOM_BME280_NAME  "CustomBME280"
#endif

// -------- BME280 regs/const ----------
#define BME280_REG_ID         0xD0
#define BME280_CHIP_ID_BME280 0x60	// id of bme280 chip for auto detection

#define BME280_REG_RESET      0xE0
#define BME280_RESET_CMD      0xB6

#define BME280_REG_STATUS     0xF3
#define BME280_REG_CTRL_HUM   0xF2
#define BME280_REG_CTRL_MEAS  0xF4

#define BME280_REG_PRESS_MSB  0xF7

// status bits
#define BME280_STATUS_IM_UPDATE 0x01  // bit0
#define BME280_STATUS_MEASURING 0x08  // bit3

// ctrl_meas bits
#define BME280_MODE_FORCED    0x01
#define BME280_OSRS_X1        0x01

typedef struct {
  uint16_t dig_T1;
  int16_t  dig_T2;
  int16_t  dig_T3;

  uint16_t dig_P1;
  int16_t  dig_P2;
  int16_t  dig_P3;
  int16_t  dig_P4;
  int16_t  dig_P5;
  int16_t  dig_P6;
  int16_t  dig_P7;
  int16_t  dig_P8;
  int16_t  dig_P9;

  uint8_t  dig_H1;
  int16_t  dig_H2;
  uint8_t  dig_H3;
  int16_t  dig_H4;
  int16_t  dig_H5;
  int8_t   dig_H6;
} Bme280Calib_t;

// -------- State ----------
static Bme280Calib_t BmeCal;

static bool     BmePresent   = false;
static bool     BmeReady     = false;

static float    BmeTempC     = NAN;
static float    BmeHumPct    = NAN;
static float    BmePresHpa   = NAN;

static uint32_t LastPollTime = 0;     // last attempt time (sec uptime)
static uint32_t LastGoodTime = 0;     // last success time (sec uptime)

static uint16_t FailStreak   = 0;     // consecutive fails
static uint32_t FailTotal    = 0;     // total fails since boot

static const uint32_t kPeriodSec     = 10;
static const uint8_t  kReadRetries   = 3;
static const uint16_t kReinitAfter   = 6;

// -------- I2C select helper (Tasmota style) ----------
static inline bool BmeSelect(void) {
  return I2cSetDevice(CUSTOM_BME280_ADDR, CUSTOM_BME280_BUS);
}

// -------- Short-read helpers (Variant A) ----------
static uint32_t BmeRead24(uint8_t reg) {
  // Reads 3 bytes MSB..LSB and returns 24-bit value.
  // 0 is used as "failed" sentinel here, but we also sanity-check downstream.
  if (!BmeSelect()) return 0;
  uint8_t msb  = I2cRead8(CUSTOM_BME280_ADDR, reg + 0, CUSTOM_BME280_BUS);
  uint8_t lsb  = I2cRead8(CUSTOM_BME280_ADDR, reg + 1, CUSTOM_BME280_BUS);
  uint8_t xlsb = I2cRead8(CUSTOM_BME280_ADDR, reg + 2, CUSTOM_BME280_BUS);
  return ((uint32_t)msb << 16) | ((uint32_t)lsb << 8) | (uint32_t)xlsb;
}

static uint16_t BmeRead16(uint8_t reg) {
  if (!BmeSelect()) return 0;
  uint8_t msb = I2cRead8(CUSTOM_BME280_ADDR, reg + 0, CUSTOM_BME280_BUS);
  uint8_t lsb = I2cRead8(CUSTOM_BME280_ADDR, reg + 1, CUSTOM_BME280_BUS);
  return ((uint16_t)msb << 8) | (uint16_t)lsb;
}

// -------- Wait helpers ----------
static bool BmeWaitImUpdateDone(uint16_t timeout_ms) {
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < timeout_ms) {
    if (!BmeSelect()) return false;
    uint8_t st = I2cRead8(CUSTOM_BME280_ADDR, BME280_REG_STATUS, CUSTOM_BME280_BUS);
    if (!(st & BME280_STATUS_IM_UPDATE)) return true;
    delay(2);
  }
  return false;
}

static bool BmeWaitMeasuringDone(uint16_t timeout_ms) {
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < timeout_ms) {
    if (!BmeSelect()) return false;
    uint8_t st = I2cRead8(CUSTOM_BME280_ADDR, BME280_REG_STATUS, CUSTOM_BME280_BUS);
    if (!(st & BME280_STATUS_MEASURING)) return true;
    delay(2);
  }
  return false;
}

// -------- Soft reset ----------
static bool BmeSoftReset(void) {
  if (!BmeSelect()) return false;
  if (!I2cWrite8(CUSTOM_BME280_ADDR, BME280_REG_RESET, BME280_RESET_CMD, CUSTOM_BME280_BUS)) return false;
  delay(5);
  return BmeWaitImUpdateDone(250);
}

// -------- Calibration read (short reads like xsns_09_bmp.ino) ----------
static bool BmeLoadCalibration(void) {
  if (!BmeSelect()) return false;

  BmeCal.dig_T1 = I2cRead16LE(CUSTOM_BME280_ADDR, 0x88, CUSTOM_BME280_BUS);
  BmeCal.dig_T2 = I2cReadS16_LE(CUSTOM_BME280_ADDR, 0x8A, CUSTOM_BME280_BUS);
  BmeCal.dig_T3 = I2cReadS16_LE(CUSTOM_BME280_ADDR, 0x8C, CUSTOM_BME280_BUS);

  BmeCal.dig_P1 = I2cRead16LE(CUSTOM_BME280_ADDR, 0x8E, CUSTOM_BME280_BUS);
  BmeCal.dig_P2 = I2cReadS16_LE(CUSTOM_BME280_ADDR, 0x90, CUSTOM_BME280_BUS);
  BmeCal.dig_P3 = I2cReadS16_LE(CUSTOM_BME280_ADDR, 0x92, CUSTOM_BME280_BUS);
  BmeCal.dig_P4 = I2cReadS16_LE(CUSTOM_BME280_ADDR, 0x94, CUSTOM_BME280_BUS);
  BmeCal.dig_P5 = I2cReadS16_LE(CUSTOM_BME280_ADDR, 0x96, CUSTOM_BME280_BUS);
  BmeCal.dig_P6 = I2cReadS16_LE(CUSTOM_BME280_ADDR, 0x98, CUSTOM_BME280_BUS);
  BmeCal.dig_P7 = I2cReadS16_LE(CUSTOM_BME280_ADDR, 0x9A, CUSTOM_BME280_BUS);
  BmeCal.dig_P8 = I2cReadS16_LE(CUSTOM_BME280_ADDR, 0x9C, CUSTOM_BME280_BUS);
  BmeCal.dig_P9 = I2cReadS16_LE(CUSTOM_BME280_ADDR, 0x9E, CUSTOM_BME280_BUS);

  BmeCal.dig_H1 = I2cRead8(CUSTOM_BME280_ADDR, 0xA1, CUSTOM_BME280_BUS);
  BmeCal.dig_H2 = I2cReadS16_LE(CUSTOM_BME280_ADDR, 0xE1, CUSTOM_BME280_BUS);
  BmeCal.dig_H3 = I2cRead8(CUSTOM_BME280_ADDR, 0xE3, CUSTOM_BME280_BUS);

  uint8_t e4 = I2cRead8(CUSTOM_BME280_ADDR, 0xE4, CUSTOM_BME280_BUS);
  uint8_t e5 = I2cRead8(CUSTOM_BME280_ADDR, 0xE5, CUSTOM_BME280_BUS);
  uint8_t e6 = I2cRead8(CUSTOM_BME280_ADDR, 0xE6, CUSTOM_BME280_BUS);

  BmeCal.dig_H4 = (int16_t)((e4 << 4) | (e5 & 0x0F));
  BmeCal.dig_H5 = (int16_t)((e6 << 4) | (e5 >> 4));
  BmeCal.dig_H6 = (int8_t)I2cRead8(CUSTOM_BME280_ADDR, 0xE7, CUSTOM_BME280_BUS);

  // sanity
  if (BmeCal.dig_T1 == 0x0000 || BmeCal.dig_T1 == 0xFFFF) return false;
  if (BmeCal.dig_P1 == 0x0000 || BmeCal.dig_P1 == 0xFFFF) return false;

  return true;
}

// -------- Trigger forced measurement ----------
static bool BmeTriggerForcedMeasurement(void) {
  if (!BmeSelect()) return false;

  if (!I2cWrite8(CUSTOM_BME280_ADDR, BME280_REG_CTRL_HUM, BME280_OSRS_X1, CUSTOM_BME280_BUS)) return false;

  uint8_t ctrl_meas = (BME280_OSRS_X1 << 5) | (BME280_OSRS_X1 << 2) | BME280_MODE_FORCED;
  if (!I2cWrite8(CUSTOM_BME280_ADDR, BME280_REG_CTRL_MEAS, ctrl_meas, CUSTOM_BME280_BUS)) return false;

  return true;
}

// -------- Bosch compensation ----------
static int32_t BmeCompensateTfine(int32_t adc_T) {
  int32_t var1 = ((((adc_T >> 3) - ((int32_t)BmeCal.dig_T1 << 1))) * ((int32_t)BmeCal.dig_T2)) >> 11;
  int32_t var2 = (((((adc_T >> 4) - ((int32_t)BmeCal.dig_T1)) * ((adc_T >> 4) - ((int32_t)BmeCal.dig_T1))) >> 12) *
                   ((int32_t)BmeCal.dig_T3)) >> 14;
  return var1 + var2;
}

static uint32_t BmeCompensateP(int32_t adc_P, int32_t t_fine) {
  int64_t var1 = ((int64_t)t_fine) - 128000;
  int64_t var2 = var1 * var1 * (int64_t)BmeCal.dig_P6;
  var2 += ((var1 * (int64_t)BmeCal.dig_P5) << 17);
  var2 += ((int64_t)BmeCal.dig_P4 << 35);
  var1 = ((var1 * var1 * (int64_t)BmeCal.dig_P3) >> 8) + ((var1 * (int64_t)BmeCal.dig_P2) << 12);
  var1 = ((((int64_t)1) << 47) + var1) * ((int64_t)BmeCal.dig_P1) >> 33;
  if (var1 == 0) return 0;

  int64_t p = 1048576 - adc_P;
  p = (((p << 31) - var2) * 3125) / var1;
  var1 = ((int64_t)BmeCal.dig_P9 * (p >> 13) * (p >> 13)) >> 25;
  var2 = ((int64_t)BmeCal.dig_P8 * p) >> 19;
  p = ((p + var1 + var2) >> 8) + (((int64_t)BmeCal.dig_P7) << 4);
  return (uint32_t)(p >> 8); // Pa
}

static uint32_t BmeCompensateH(int32_t adc_H, int32_t t_fine) {
  int32_t v_x1 = (t_fine - ((int32_t)76800));
  v_x1 = (((((adc_H << 14) - (((int32_t)BmeCal.dig_H4) << 20) - (((int32_t)BmeCal.dig_H5) * v_x1)) + ((int32_t)16384)) >> 15) *
          (((((((v_x1 * ((int32_t)BmeCal.dig_H6)) >> 10) * (((v_x1 * ((int32_t)BmeCal.dig_H3)) >> 11) + ((int32_t)32768))) >> 10) +
              ((int32_t)2097152)) * ((int32_t)BmeCal.dig_H2) + 8192) >> 14));
  v_x1 = (v_x1 - (((((v_x1 >> 15) * (v_x1 >> 15)) >> 7) * ((int32_t)BmeCal.dig_H1)) >> 4));
  if (v_x1 < 0) v_x1 = 0;
  if (v_x1 > 419430400) v_x1 = 419430400;
  return (uint32_t)(v_x1 >> 12);
}

// -------- Read sample (Variant A: no burst read) ----------
static bool BmeReadOnce(uint8_t *fail_stage) {
  // stage: 1=trigger, 2=wait, 3=read raw (short reads)
  if (!BmeTriggerForcedMeasurement()) { *fail_stage = 1; return false; }

  // measurement complete
  if (!BmeWaitMeasuringDone(400)) { *fail_stage = 2; return false; }

  // Read raw values via short reads (no I2cReadBuffer burst)
  uint32_t p24 = BmeRead24(0xF7);  // P msb..xlsb
  uint32_t t24 = BmeRead24(0xFA);  // T msb..xlsb
  uint16_t h16 = BmeRead16(0xFD);  // H msb..lsb

  // sanity: raw temp/press shouldn't be zero typically
  if ((p24 == 0) || (t24 == 0)) { *fail_stage = 3; return false; }

  int32_t adc_P = (int32_t)(p24 >> 4);
  int32_t adc_T = (int32_t)(t24 >> 4);
  int32_t adc_H = (int32_t)h16;

  int32_t t_fine = BmeCompensateTfine(adc_T);

  BmeTempC   = (float)((t_fine * 5 + 128) >> 8) / 100.0f;
  BmePresHpa = (float)BmeCompensateP(adc_P, t_fine) / 100.0f;
  BmeHumPct  = (float)BmeCompensateH(adc_H, t_fine) / 1024.0f;

  return true;
}

static bool BmeReadWithRetry(uint8_t *fail_stage) {
  for (uint8_t i = 0; i < kReadRetries; i++) {
    uint8_t stage = 0;
    if (BmeReadOnce(&stage)) { *fail_stage = 0; return true; }
    *fail_stage = stage;
    delay(60);
  }
  return false;
}

// -------- Init / Reinit ----------
static void BmeInit(void) {
  BmePresent = false;
  BmeReady   = false;
  FailStreak = 0;

  if (!I2cEnabled(CUSTOM_BME280_BUS + 1)) return;
  if (!BmeSelect()) { AddLogMissed(PSTR(CUSTOM_BME280_NAME), 1); return; }

  uint8_t id = I2cRead8(CUSTOM_BME280_ADDR, BME280_REG_ID, CUSTOM_BME280_BUS);
  if (id != BME280_CHIP_ID_BME280) {
    AddLogMissed(PSTR(CUSTOM_BME280_NAME), 1);
    AddLog(LOG_LEVEL_ERROR, PSTR(CUSTOM_BME280_NAME ": chip id mismatch (0x%02X) at 0x%02X"),
           id, CUSTOM_BME280_ADDR);
    return;
  }

  if (!BmeSoftReset()) {
    AddLog(LOG_LEVEL_ERROR, PSTR(CUSTOM_BME280_NAME ": soft reset failed at 0x%02X"), CUSTOM_BME280_ADDR);
    return;
  }

  if (!BmeLoadCalibration()) {
    AddLog(LOG_LEVEL_ERROR, PSTR(CUSTOM_BME280_NAME ": calibration read failed at 0x%02X"), CUSTOM_BME280_ADDR);
    return;
  }

  BmePresent = true;
  BmeReady   = true;

  AddLog(LOG_LEVEL_INFO, PSTR(CUSTOM_BME280_NAME ": detected at 0x%02X (bus %u)"), CUSTOM_BME280_ADDR, CUSTOM_BME280_BUS);

  // immediate first read
  uint8_t stage = 0;
  LastPollTime = TasmotaGlobal.uptime;
  if (BmeReadWithRetry(&stage)) {
    LastGoodTime = TasmotaGlobal.uptime;
    FailStreak = 0;
  } else {
    FailStreak++;
    FailTotal++;
    AddLog(LOG_LEVEL_ERROR, PSTR(CUSTOM_BME280_NAME ": initial read failed (stage=%u)"), stage);
  }
}

static void BmeReinit(void) {
  if (!BmePresent) return;

  AddLog(LOG_LEVEL_ERROR, PSTR(CUSTOM_BME280_NAME ": reinitializing after failures"));

  if (!BmeSoftReset()) {
    AddLog(LOG_LEVEL_ERROR, PSTR(CUSTOM_BME280_NAME ": soft reset failed"));
    return;
  }

  if (!BmeLoadCalibration()) {
    AddLog(LOG_LEVEL_ERROR, PSTR(CUSTOM_BME280_NAME ": calibration reload failed"));
    return;
  }

  // Important: avoid immediate reinit loop
  FailStreak = 0;

  AddLog(LOG_LEVEL_INFO, PSTR(CUSTOM_BME280_NAME ": reinitialized"));
}

// -------- Tasmota callback ----------
bool Xsns200(uint32_t function) {
  if (!I2cEnabled(CUSTOM_BME280_BUS + 1)) return false;

  bool result = false;

  switch (function) {
    case FUNC_INIT:
      BmeInit();
      break;

    case FUNC_EVERY_SECOND:
      if (!BmeReady) break;

      if ((TasmotaGlobal.uptime - LastPollTime) >= kPeriodSec) {
        LastPollTime = TasmotaGlobal.uptime;

        uint8_t stage = 0;
        if (BmeReadWithRetry(&stage)) {
          LastGoodTime = TasmotaGlobal.uptime;
          FailStreak = 0;
        } else {
          FailStreak++;
          FailTotal++;

          AddLog(LOG_LEVEL_ERROR, PSTR(CUSTOM_BME280_NAME ": read failed (stage=%u,streak=%u,total=%u)"),
                 stage, FailStreak, FailTotal);

          if (FailStreak >= kReinitAfter) {
            BmeReinit();
          }
        }
      }
      break;

    case FUNC_JSON_APPEND:
      if (BmePresent) {
        const bool has_values = (!isnan(BmeTempC) && !isnan(BmeHumPct) && !isnan(BmePresHpa));
        const bool stale = (has_values && ((TasmotaGlobal.uptime - LastGoodTime) > (kPeriodSec * 2)));

        if (has_values) {
          char t_str[16], h_str[16], p_str[16];

          // В telemetry я бы оставил "сырые" единицы: C, %, hPa
          dtostrfd(BmeTempC, 1, t_str);
          dtostrfd(BmeHumPct, 1, h_str);
          dtostrfd(BmePresHpa, 1, p_str);

          ResponseAppend_P(PSTR(",\"" CUSTOM_BME280_NAME "\":{"
                                "\"Temperature\":%s,"
                                "\"Humidity\":%s,"
                                "\"Pressure\":%s,"
                                "\"Stale\":%u}"),
                          t_str, h_str, p_str,
                          stale ? 1 : 0);
        } else {
          ResponseAppend_P(PSTR(",\"" CUSTOM_BME280_NAME "\":{"
                                "\"FailStreak\":%u,"
                                "\"FailTotal\":%u,"
                                "\"Error\":\"no_data\"}"),
                          FailStreak, FailTotal);
        }
        result = true;
      }
      break;

#ifdef USE_WEBSERVER

// Im using both methods just to show possibilities, but they could be deleted from final code or combined as common fuction

    case FUNC_WEB_SENSOR:
      result = true;
      if (BmeReady && !isnan(BmeTempC)) {
        char t_str[16], h_str[16], p_str[16];

        dtostrfd(ConvertTemp(BmeTempC), Settings->flag2.temperature_resolution, t_str);
        dtostrfd(ConvertHumidity(BmeHumPct), Settings->flag2.humidity_resolution, h_str);
        dtostrfd(ConvertPressure(BmePresHpa), Settings->flag2.pressure_resolution, p_str);

        WSContentSend_PD(PSTR("{s}" CUSTOM_BME280_NAME " Temperature{m}%s %c{e}"), t_str, TempUnit());
        WSContentSend_PD(PSTR("{s}" CUSTOM_BME280_NAME " Humidity{m}%s %%{e}"), h_str);
        WSContentSend_PD(PSTR("{s}" CUSTOM_BME280_NAME " Pressure{m}%s hPa{e}"), p_str);

        if (FailStreak || FailTotal) {
          WSContentSend_PD(PSTR("{s}" CUSTOM_BME280_NAME " Errors{m}%u (total %u){e}"), FailStreak, FailTotal);
        }
      }
      break;

    case FUNC_WEB_COL_SENSOR:
      result = true;
      if (BmeReady && !isnan(BmeTempC)) {
        char t_str[16], h_str[16], p_str[16];

        dtostrfd(ConvertTemp(BmeTempC), Settings->flag2.temperature_resolution, t_str);
        dtostrfd(ConvertHumidity(BmeHumPct), Settings->flag2.humidity_resolution, h_str);
        dtostrfd(ConvertPressure(BmePresHpa), Settings->flag2.pressure_resolution, p_str);

        WSContentSend_PD(PSTR("{s}" CUSTOM_BME280_NAME "{m}%s %c / %s %% / %s hPa{e}"),
                 t_str, TempUnit(), h_str, p_str);
      }
      break;
#endif
  }

  return result;
}

#endif  // USE_I2C
#endif  // USE_CUSTOM_SENSOR