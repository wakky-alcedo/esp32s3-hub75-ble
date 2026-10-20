#include "sensors.h"
#include <Wire.h>
#include "config.h"
#include "usb_console.h"

static const uint8_t SHTC3_ADDR = 0x70;
static const uint8_t QMI_ADDRS[] = {0x6B, 0x6A};
static uint8_t sQmiAddr = 0;

// QMI8658 registers
static const uint8_t QMI_WHO_AM_I = 0x00, QMI_CTRL1 = 0x02, QMI_CTRL2 = 0x03, QMI_CTRL3 = 0x04,
                     QMI_CTRL5 = 0x06, QMI_CTRL7 = 0x08, QMI_TEMP_L = 0x33, QMI_RESET = 0x60;
static const float ACC_LSB_PER_G = 8192;    // +-4g
static const float GYR_LSB_PER_DPS = 64;    // +-512dps

static const uint32_t IMU_PERIOD_MS = 10;
static const uint32_t ENV_PERIOD_MS = 2000;

static portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
static SensorData sData;
static float sRaw[3];  // last accelerometer sample in board axes, for setup
// Test override of gravity and shake (sensorsSimulate).
static float sSim[4];
static uint32_t sSimUntil = 0;
// Strongest shake since the last sensorsJson(), to tune the effect thresholds.
static float sShakePeak = 0;

// --- I2C helpers -------------------------------------------------------------

static bool writeReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool readRegs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool shtcCommand(uint16_t cmd) {
  Wire.beginTransmission(SHTC3_ADDR);
  Wire.write(cmd >> 8);
  Wire.write(cmd & 0xFF);
  return Wire.endTransmission() == 0;
}

static uint8_t crc8(const uint8_t *d, int n) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = crc & 0x80 ? (crc << 1) ^ 0x31 : crc << 1;
  }
  return crc;
}

// --- QMI8658 -----------------------------------------------------------------

static bool imuInit() {
  for (uint8_t a : QMI_ADDRS) {
    uint8_t who = 0;
    if (readRegs(a, QMI_WHO_AM_I, &who, 1) && who == 0x05) {
      sQmiAddr = a;
      break;
    }
  }
  if (!sQmiAddr) return false;
  writeReg(sQmiAddr, QMI_RESET, 0xB0);
  delay(20);
  bool ok = writeReg(sQmiAddr, QMI_CTRL1, 0x40);   // register auto-increment, little endian
  ok &= writeReg(sQmiAddr, QMI_CTRL2, 0x16);       // accel +-4g, 125Hz
  ok &= writeReg(sQmiAddr, QMI_CTRL3, 0x56);       // gyro +-512dps, 112Hz
  ok &= writeReg(sQmiAddr, QMI_CTRL5, 0x11);       // low-pass filters on
  ok &= writeReg(sQmiAddr, QMI_CTRL7, 0x03);       // accel + gyro enabled
  return ok;
}

// Board axes -> panel axes (as the viewer sees the physical panel). The
// board's +z points into the panel; how it is turned within the panel plane
// depends on the mounting, so that part is the imuRotation setting (set by
// sensorsCalibrateDown()).
static void boardToPanel(const float *b, float *p) {
  switch (gConfig.imuRotation & 3) {
    case 1: p[0] = -b[1]; p[1] = b[0]; break;
    case 2: p[0] = -b[0]; p[1] = -b[1]; break;
    case 3: p[0] = b[1]; p[1] = -b[0]; break;
    default: p[0] = b[0]; p[1] = b[1]; break;
  }
  p[2] = -b[2];
}

// Panel axes -> screen axes after the configured rotation and mirroring
// (inverse of the pixel mapping in Display::buildMap).
static void panelToScreen(const float *p, float *s) {
  const PanelConfig &c = gConfig.panel;
  float vx, vy;
  switch (c.rotation & 3) {
    case 1: vx = p[1]; vy = -p[0]; break;
    case 2: vx = -p[0]; vy = -p[1]; break;
    case 3: vx = -p[1]; vy = p[0]; break;
    default: vx = p[0]; vy = p[1]; break;
  }
  s[0] = c.flipH ? -vx : vx;
  s[1] = c.flipV ? -vy : vy;
  s[2] = p[2];
}

static bool imuRead(float *acc, float *gyr) {
  uint8_t b[14];
  if (!readRegs(sQmiAddr, QMI_TEMP_L, b, sizeof(b))) return false;
  float ra[3], rg[3], p[3];
  for (int i = 0; i < 3; i++) {
    ra[i] = (int16_t)(b[2 + i * 2] | b[3 + i * 2] << 8) / ACC_LSB_PER_G;
    rg[i] = (int16_t)(b[8 + i * 2] | b[9 + i * 2] << 8) / GYR_LSB_PER_DPS;
  }
  memcpy(sRaw, ra, sizeof(sRaw));
  // Report gravity rather than the upward reaction the sensor measures.
  for (float &v : ra) v = -v;
  boardToPanel(ra, p);
  panelToScreen(p, acc);
  boardToPanel(rg, p);
  panelToScreen(p, gyr);
  return true;
}

// --- Task --------------------------------------------------------------------

static void sensorTask(void *) {
  SensorData d;
  d.imuOk = sQmiAddr != 0;
  uint32_t envAt = 0, measureAt = 0;
  bool measuring = false;
  bool first = true;
  float bias[3] = {};
  for (;;) {
    uint32_t now = millis();
    float a[3], g[3];
    if (d.imuOk && imuRead(a, g)) {
      d.ax = a[0], d.ay = a[1], d.az = a[2];
      // Learn the gyro's zero offset while the board is still.
      bool still = d.shake < 0.03f && fabsf(g[0] - bias[0]) < 8 && fabsf(g[1] - bias[1]) < 8 && fabsf(g[2] - bias[2]) < 8;
      for (int i = 0; i < 3; i++) {
        if (still || first) bias[i] += (g[i] - bias[i]) * (first ? 1.0f : 0.02f);
        g[i] -= bias[i];
      }
      d.gx = g[0], d.gy = g[1], d.gz = g[2];
      const float k = first ? 1.0f : 0.15f;
      first = false;
      d.gravX += (a[0] - d.gravX) * k;
      d.gravY += (a[1] - d.gravY) * k;
      d.gravZ += (a[2] - d.gravZ) * k;
      float hx = a[0] - d.gravX, hy = a[1] - d.gravY, hz = a[2] - d.gravZ;
      float jolt = sqrtf(hx * hx + hy * hy + hz * hz);
      d.shake = max(jolt, d.shake * 0.92f);
    }

    // SHTC3: wake + start a measurement, collect it on a later pass.
    if (!measuring && now - envAt >= ENV_PERIOD_MS) {
      envAt = now;
      if (shtcCommand(0x3517)) {  // wake up
        delayMicroseconds(300);
        measuring = shtcCommand(0x7866);  // T first, normal mode, no clock stretching
        measureAt = now;
      }
      if (!measuring) d.envOk = false;
      d.chipC = temperatureRead();
    } else if (measuring && now - measureAt >= 15) {
      measuring = false;
      uint8_t r[6];
      if (Wire.requestFrom((int)SHTC3_ADDR, 6) == 6) {
        for (int i = 0; i < 6; i++) r[i] = Wire.read();
        if (crc8(r, 2) == r[2] && crc8(r + 3, 2) == r[5]) {
          d.tempC = -45 + 175 * (float)(r[0] << 8 | r[1]) / 65536 + gConfig.tempOffset10 / 10.0f;
          d.humidity = 100 * (float)(r[3] << 8 | r[4]) / 65536;
          d.envOk = true;
        }
      }
      shtcCommand(0xB098);  // sleep
    }

    portENTER_CRITICAL(&sMux);
    sData = d;
    sShakePeak = max(sShakePeak, d.shake);
    if ((int32_t)(sSimUntil - now) > 0) {
      sData.gravX = sSim[0], sData.gravY = sSim[1], sData.gravZ = sSim[2], sData.shake = sSim[3];
    }
    portEXIT_CRITICAL(&sMux);
    vTaskDelay(pdMS_TO_TICKS(IMU_PERIOD_MS));
  }
}

void sensorsBegin() {
  bool imu = imuInit();
  bool env = shtcCommand(0x3517);
  if (env) shtcCommand(0xB098);
  Console.printf("[sensors] QMI8658 %s, SHTC3 %s\n", imu ? "ok" : "not found", env ? "ok" : "not found");
  xTaskCreatePinnedToCore(sensorTask, "sensors", 4096, nullptr, 2, nullptr, 0);
}

bool sensorsCalibrateDown() {
  if (!sQmiAddr) return false;
  // Find the rotation that makes the measured gravity point to the
  // bottom of the physical panel.
  // The accelerometer reads +1g upwards at rest, so gravity is -sRaw.
  float bx = -sRaw[0], by = -sRaw[1];
  if (sqrtf(bx * bx + by * by) < 0.5f) return false;  // lying flat
  // Panel y of the gravity vector for each rotation (see boardToPanel).
  const float py[4] = {by, bx, -by, -bx};
  int best = 0;
  for (int r = 1; r < 4; r++)
    if (py[r] > py[best]) best = r;
  gConfig.imuRotation = best;
  return true;
}

void sensorsSimulate(float gx, float gy, float gz, float shake, uint32_t ms) {
  portENTER_CRITICAL(&sMux);
  sSim[0] = gx, sSim[1] = gy, sSim[2] = gz, sSim[3] = shake;
  sSimUntil = millis() + ms;
  portEXIT_CRITICAL(&sMux);
}

SensorData sensorsGet() {
  portENTER_CRITICAL(&sMux);
  SensorData d = sData;
  portEXIT_CRITICAL(&sMux);
  return d;
}

void sensorsJson(JsonObject o) {
  SensorData d = sensorsGet();
  portENTER_CRITICAL(&sMux);
  float peak = sShakePeak;
  sShakePeak = 0;
  portEXIT_CRITICAL(&sMux);
  o["envOk"] = d.envOk;
  o["imuOk"] = d.imuOk;
  o["tempC"] = roundf(d.tempC * 10) / 10;
  o["humidity"] = roundf(d.humidity * 10) / 10;
  o["chipC"] = roundf(d.chipC * 10) / 10;
  JsonArray acc = o["acc"].to<JsonArray>();
  for (float v : {d.ax, d.ay, d.az}) acc.add(roundf(v * 100) / 100);
  JsonArray gyr = o["gyro"].to<JsonArray>();
  for (float v : {d.gx, d.gy, d.gz}) gyr.add(roundf(v * 10) / 10);
  JsonArray raw = o["accRaw"].to<JsonArray>();
  for (float v : sRaw) raw.add(roundf(v * 100) / 100);
  o["shake"] = roundf(d.shake * 100) / 100;
  o["shakePeak"] = roundf(peak * 100) / 100;
}
