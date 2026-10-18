#include "sound.h"
#include <Wire.h>
#include <driver/i2s.h>
#include "config.h"

// Waveshare ESP32-S3-RGB-Matrix wiring (from its 09_Music_Player example).
static const int PIN_MCLK = 12, PIN_BCLK = 43, PIN_LRCK = 38, PIN_DOUT = 21;
static const int PIN_PA_ENABLE = 11;
static const uint8_t ES8311_ADDR = 0x18;
static const uint32_t RATE = 48000;
static const i2s_port_t PORT = I2S_NUM_0;
// Keep the amplifier on this long after a sound, so the once-a-second ticks
// of a countdown do not switch it on and off (and pop) every time.
static const uint32_t PA_HOLD_MS = 1500;

static bool sPresent = false;
static QueueHandle_t sQueue = nullptr;

// --- ES8311 ----------------------------------------------------------------
// Register sequence follows Waveshare's es8311.cpp (Apache License 2.0) for
// slave mode, MCLK = 256 * 48kHz from the pin, 16-bit I2S, DAC only.

static bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static uint8_t readReg(uint8_t reg) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)ES8311_ADDR, 1) != 1) return 0;
  return Wire.read();
}

static bool codecInit() {
  bool ok = writeReg(0x00, 0x1F);  // reset
  delay(20);
  ok &= writeReg(0x00, 0x00);
  ok &= writeReg(0x00, 0x80);  // power on, slave mode
  ok &= writeReg(0x01, 0x3F);  // MCLK from pin, all clocks on
  ok &= writeReg(0x06, (readReg(0x06) & 0xC0) | 0x03);  // BCLK not inverted, BCLK = MCLK / 4
  ok &= writeReg(0x02, 0x00);  // pre_div 1, pre_multi 1x
  ok &= writeReg(0x03, 0x10);  // single speed, ADC OSR
  ok &= writeReg(0x04, 0x10);  // DAC OSR
  ok &= writeReg(0x05, 0x00);  // ADC/DAC clock dividers 1
  ok &= writeReg(0x07, readReg(0x07) & 0xC0);  // LRCK = MCLK / 256
  ok &= writeReg(0x08, 0xFF);
  ok &= writeReg(0x09, readReg(0x09) | 0x0C);  // 16-bit serial data in
  ok &= writeReg(0x0A, readReg(0x0A) | 0x0C);  // 16-bit serial data out
  ok &= writeReg(0x0D, 0x01);  // power up analog
  ok &= writeReg(0x0E, 0x02);
  ok &= writeReg(0x12, 0x00);  // power up DAC
  ok &= writeReg(0x13, 0x10);  // enable output drive
  ok &= writeReg(0x1C, 0x6A);
  ok &= writeReg(0x37, 0x08);  // bypass DAC equalizer
  ok &= writeReg(0x31, 0x00);  // unmute DAC (resets to 0x60 = muted)
  // DAC volume 0dB. Loudness is set in software (see amplitude()) so the
  // full-scale waveform never goes through the codec's extra gain.
  ok &= writeReg(0x32, 0xBF);
  return ok;
}

// --- Synthesis -------------------------------------------------------------

struct Tone {
  uint16_t hz;  // 0 = silence
  uint16_t ms;
};

static const Tone TICK[] = {{1000, 50}};
static const Tone BEEP[] = {{1500, 600}};
static const Tone CHIME[] = {{1319, 280}, {1047, 520}};
static const Tone ALARM[] = {
    {2000, 90}, {0, 70}, {2000, 90}, {0, 70}, {2000, 90}, {0, 70}, {2000, 90}, {0, 400},
    {2000, 90}, {0, 70}, {2000, 90}, {0, 70}, {2000, 90}, {0, 70}, {2000, 90}, {0, 400},
    {2000, 90}, {0, 70}, {2000, 90}, {0, 70}, {2000, 90}, {0, 70}, {2000, 90}, {0, 70},
};

static int16_t amplitude() {
  // Square law so the slider feels even; 100% peaks at about -6dBFS.
  uint32_t v = min<uint32_t>(gConfig.volume, 100);
  return v * v * 16000 / 10000;
}

static void playTones(const Tone *tones, size_t n) {
  static const int FRAMES = 256;
  static const uint32_t RAMP = RATE * 4 / 1000;  // 4ms fade in/out against clicks
  int16_t buf[FRAMES * 2];
  int16_t amp = amplitude();
  float phase = 0;
  for (size_t t = 0; t < n; t++) {
    uint32_t total = RATE * tones[t].ms / 1000;
    float step = 2 * PI * tones[t].hz / RATE;
    for (uint32_t done = 0; done < total;) {
      int count = min<uint32_t>(FRAMES, total - done);
      for (int i = 0; i < count; i++, done++) {
        int16_t s = 0;
        if (tones[t].hz) {
          uint32_t edge = min(done, total - 1 - done);
          float env = edge < RAMP ? (float)edge / RAMP : 1.0f;
          s = (int16_t)(sinf(phase) * amp * env);
          phase += step;
          if (phase > 2 * PI) phase -= 2 * PI;
        }
        buf[i * 2] = buf[i * 2 + 1] = s;
      }
      size_t written;
      i2s_write(PORT, buf, count * 4, &written, portMAX_DELAY);
    }
  }
}

static void soundTask(void *) {
  bool paOn = false;
  for (;;) {
    Sound s;
    if (xQueueReceive(sQueue, &s, paOn ? pdMS_TO_TICKS(PA_HOLD_MS) : portMAX_DELAY) != pdTRUE) {
      digitalWrite(PIN_PA_ENABLE, LOW);
      paOn = false;
      continue;
    }
    if (!paOn) {
      digitalWrite(PIN_PA_ENABLE, HIGH);
      paOn = true;
      static const Tone SETTLE[] = {{0, 30}};  // let the amplifier come up
      playTones(SETTLE, 1);
    }
    switch (s) {
      case SOUND_TICK: playTones(TICK, sizeof(TICK) / sizeof(Tone)); break;
      case SOUND_BEEP: playTones(BEEP, sizeof(BEEP) / sizeof(Tone)); break;
      case SOUND_CHIME: playTones(CHIME, sizeof(CHIME) / sizeof(Tone)); break;
      case SOUND_ALARM: playTones(ALARM, sizeof(ALARM) / sizeof(Tone)); break;
      default: break;
    }
  }
}

// --- API -------------------------------------------------------------------

bool soundBegin() {
  pinMode(PIN_PA_ENABLE, OUTPUT);
  digitalWrite(PIN_PA_ENABLE, LOW);
  Wire.beginTransmission(ES8311_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println("[sound] ES8311 not found");
    return false;
  }

  // The codec runs off MCLK, so start I2S before configuring it.
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 256;
  cfg.tx_desc_auto_clear = true;  // outputs silence whenever nothing is queued
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  if (i2s_driver_install(PORT, &cfg, 0, nullptr) != ESP_OK) {
    Serial.println("[sound] I2S install failed");
    return false;
  }
  i2s_pin_config_t pins = {};
  pins.mck_io_num = PIN_MCLK;
  pins.bck_io_num = PIN_BCLK;
  pins.ws_io_num = PIN_LRCK;
  pins.data_out_num = PIN_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(PORT, &pins);

  if (!codecInit()) {
    Serial.println("[sound] ES8311 init failed");
    i2s_driver_uninstall(PORT);
    return false;
  }
  sQueue = xQueueCreate(4, sizeof(Sound));
  xTaskCreatePinnedToCore(soundTask, "sound", 4096, nullptr, 3, nullptr, 0);
  sPresent = true;
  Serial.println("[sound] ES8311 ready");
  return true;
}

bool soundPresent() { return sPresent; }

void soundPlay(Sound s) {
  if (!sPresent || s == SOUND_NONE || gConfig.volume == 0) return;
  xQueueSend(sQueue, &s, 0);
}

Sound soundFromName(const char *name) {
  if (!name) return SOUND_NONE;
  if (!strcmp(name, "tick")) return SOUND_TICK;
  if (!strcmp(name, "beep")) return SOUND_BEEP;
  if (!strcmp(name, "chime")) return SOUND_CHIME;
  if (!strcmp(name, "alarm")) return SOUND_ALARM;
  return SOUND_NONE;
}
