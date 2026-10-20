#include "sound.h"
#include <Wire.h>
#include <driver/i2s.h>
#include "config.h"
#include "fsutil.h"
#include "music.h"
#include "usb_console.h"

// Waveshare ESP32-S3-RGB-Matrix wiring (from its 09_Music_Player example).
static const int PIN_MCLK = 12, PIN_BCLK = 43, PIN_LRCK = 38, PIN_DOUT = 21, PIN_DIN = 39;
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

// --- Effect sounds ---------------------------------------------------------

static uint32_t sRng = 0x9E3779B9u;

static inline float noise() {  // -1..1
  sRng ^= sRng << 13;
  sRng ^= sRng >> 17;
  sRng ^= sRng << 5;
  return (int32_t)sRng * (1.0f / 2147483648.0f);
}

static inline float urand() { return noise() * 0.5f + 0.5f; }

// Streams `frames` samples from `sample(i)` to the codec.
template <typename F>
static void render(uint32_t frames, F sample) {
  static const int FRAMES = 256;
  int16_t buf[FRAMES * 2];
  for (uint32_t done = 0; done < frames;) {
    int count = min<uint32_t>(FRAMES, frames - done);
    for (int i = 0; i < count; i++, done++) {
      int32_t s = constrain((int32_t)sample(done), -32767, 32767);
      buf[i * 2] = buf[i * 2 + 1] = s;
    }
    size_t written;
    i2s_write(PORT, buf, count * 4, &written, portMAX_DELAY);
  }
}

// A struck hard object: two decaying partials (the upper one dies faster)
// and a little noise for the contact. Pitch varies a bit between hits.
static void playClick(float amp, float f1, float f2, float tauMs, float noiseMix) {
  const float detune = 1 + noise() * 0.08f;
  const float w1 = 2 * PI * f1 * detune / RATE, w2 = 2 * PI * f2 * detune / RATE;
  const float decay = expf(-1000.0f / (RATE * tauMs));
  const uint32_t total = RATE * tauMs * 6 / 1000, attack = RATE / 2000;  // 0.5ms
  float env = 1, p1 = 0, p2 = 0;
  render(total, [&](uint32_t i) {
    float v = sinf(p1) + 0.5f * env * sinf(p2) + noiseMix * env * env * noise();
    v *= env * (i < attack ? (float)i / attack : 1.0f);
    env *= decay;
    p1 += w1;
    p2 += w2;
    if (p1 > 2 * PI) p1 -= 2 * PI;
    if (p2 > 2 * PI) p2 -= 2 * PI;
    return v * amp * 0.7f;
  });
}

struct NoiseSpec {
  uint16_t ms, attackMs;
  float lp;       // one-pole low-pass coefficient: higher is brighter
  float hp;       // coefficient of the low-pass whose output is removed (0 = none)
  float grains;   // grain onsets per ms; 0 = continuous noise
  float grainMs;  // grain decay time
  uint8_t drops;  // rising water-drop blips sprinkled in
  float gain;     // brings the filtered noise back to about full scale
};

static const NoiseSpec RATTLE = {170, 2, 0.6f, 0.10f, 5.0f, 0.5f, 0, 2.9f};
static const NoiseSpec SPLASH = {300, 30, 0.07f, 0.010f, 0, 0, 3, 4.8f};
static const NoiseSpec WHOOSH = {340, 140, 0.10f, 0.020f, 0, 0, 0, 4.2f};
static const NoiseSpec FIZZ = {240, 4, 0.75f, 0.30f, 1.6f, 0.2f, 0, 4.8f};

static void playNoise(const NoiseSpec &n, float amp) {
  const uint32_t total = RATE * n.ms / 1000, attack = RATE * n.attackMs / 1000 + 1;
  const float pGrain = n.grains * 1000 / RATE;
  const float gDecay = n.grainMs > 0 ? expf(-1000.0f / (RATE * n.grainMs)) : 0;
  const uint32_t DROP = RATE * 45 / 1000;
  struct Drop {
    uint32_t at;
    float f, phase, env;
  } drops[4];
  const int nDrops = min<int>(n.drops, 4);
  for (int d = 0; d < nDrops; d++) drops[d] = {(uint32_t)(urand() * total * 0.6f), 700 + urand() * 900, 0, 1};
  const float dropDecay = expf(-1000.0f / (RATE * 10));
  float g = 0, lp = 0, lo = 0;
  render(total, [&](uint32_t i) {
    float x;
    if (n.grains > 0) {
      if (urand() < pGrain) g = 0.3f + 0.7f * urand();
      x = noise() * g;
      g *= gDecay;
    } else {
      x = noise();
    }
    lp += n.lp * (x - lp);
    lo += n.hp * (lp - lo);
    float v = (n.hp > 0 ? lp - lo : lp) * n.gain;
    for (int d = 0; d < nDrops; d++) {
      Drop &dr = drops[d];
      if (i < dr.at || i >= dr.at + DROP) continue;
      float t = (float)(i - dr.at) / DROP;
      dr.phase += 2 * PI * dr.f * (1 + t * 1.5f) / RATE;  // pitch glides up
      v += sinf(dr.phase) * dr.env * 0.6f;
      dr.env *= dropDecay;
    }
    float r = i < attack ? 1.0f : 1 - (float)(i - attack) / (total - attack);
    float env = i < attack ? (float)i / attack : r * r;
    return v * env * amp;
  });
}

// --- Music -----------------------------------------------------------------

// Requests the task handles besides sounds.
static const Sound CMD_MUSIC_PLAY = (Sound)200, CMD_MUSIC_STOP = (Sound)201;

static SemaphoreHandle_t sMusicLock = nullptr;  // guards the Strings below
static String sMusicReq;          // file asked for by musicPlay()
static bool sMusicReqLoop = false;
static String sMusicNow;          // file being played ("" = none), for status
static volatile bool sMusicOn = false;

static void setNowPlaying(const String &path) {
  xSemaphoreTake(sMusicLock, portMAX_DELAY);
  sMusicNow = path;
  xSemaphoreGive(sMusicLock);
  sMusicOn = path.length() > 0;
}

// Plays the next slice of music, about 5ms. Returns false at the end.
static bool playMusicSlice(MusicDecoder &dec) {
  static const int FRAMES = 256;
  int16_t buf[FRAMES * 2];
  size_t n = dec.read(buf, FRAMES);
  if (!n) return false;
  // Mixed to mono for the single speaker, at the same loudness scale as the tones.
  const int32_t amp = amplitude();
  for (size_t i = 0; i < n; i++) {
    int32_t m = ((int32_t)buf[i * 2] + buf[i * 2 + 1]) / 2;
    buf[i * 2] = buf[i * 2 + 1] = (int16_t)(m * amp / 32768);
  }
  size_t written;
  i2s_write(PORT, buf, n * 4, &written, portMAX_DELAY);
  return true;
}

struct Request {
  Sound s;
  uint8_t level;
};

static void soundTask(void *) {
  bool paOn = false;
  MusicDecoder music;
  String musicPath;
  bool musicLoop = false;
  for (;;) {
    Request req;
    const TickType_t wait = music.isOpen() ? 0 : paOn ? pdMS_TO_TICKS(PA_HOLD_MS) : portMAX_DELAY;
    const bool got = xQueueReceive(sQueue, &req, wait) == pdTRUE;
    if (got && req.s == CMD_MUSIC_PLAY) {
      xSemaphoreTake(sMusicLock, portMAX_DELAY);
      musicPath = sMusicReq;
      musicLoop = sMusicReqLoop;
      xSemaphoreGive(sMusicLock);
      bool ok = music.open(musicPath.c_str());
      Console.printf("[music] %s %s\n", ok ? "playing" : "cannot play", musicPath.c_str());
      setNowPlaying(ok ? musicPath : String());
      continue;
    }
    if (got && req.s == CMD_MUSIC_STOP) {
      music.close();
      setNowPlaying(String());
      continue;
    }
    if (!got && !music.isOpen()) {
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
    if (!got) {
      if (playMusicSlice(music)) continue;
      // End of the file: start over, or stop.
      if (!musicLoop || !music.open(musicPath.c_str())) {
        music.close();
        setNowPlaying(String());
      }
      continue;
    }
    // A sound pauses the music while it plays.
    const float amp = amplitude() * req.level / 255.0f;
    switch (req.s) {
      case SOUND_TICK: playTones(TICK, sizeof(TICK) / sizeof(Tone)); break;
      case SOUND_BEEP: playTones(BEEP, sizeof(BEEP) / sizeof(Tone)); break;
      case SOUND_CHIME: playTones(CHIME, sizeof(CHIME) / sizeof(Tone)); break;
      case SOUND_ALARM: playTones(ALARM, sizeof(ALARM) / sizeof(Tone)); break;
      case SOUND_CLACK: playClick(amp, 3300, 5200, 6, 0.3f); break;
      case SOUND_KNOCK: playClick(amp, 900, 2100, 8, 0.6f); break;
      case SOUND_RATTLE: playNoise(RATTLE, amp); break;
      case SOUND_SPLASH: playNoise(SPLASH, amp); break;
      case SOUND_WHOOSH: playNoise(WHOOSH, amp); break;
      case SOUND_FIZZ: playNoise(FIZZ, amp); break;
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
    Console.println("[sound] ES8311 not found");
    return false;
  }

  // The codec runs off MCLK, so start I2S before configuring it. RX is for
  // the ES7210 microphones on the same bus (see mic.cpp).
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  cfg.sample_rate = RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 256;
  cfg.tx_desc_auto_clear = true;  // outputs silence whenever nothing is queued
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  if (i2s_driver_install(PORT, &cfg, 0, nullptr) != ESP_OK) {
    Console.println("[sound] I2S install failed");
    return false;
  }
  i2s_pin_config_t pins = {};
  pins.mck_io_num = PIN_MCLK;
  pins.bck_io_num = PIN_BCLK;
  pins.ws_io_num = PIN_LRCK;
  pins.data_out_num = PIN_DOUT;
  pins.data_in_num = PIN_DIN;
  i2s_set_pin(PORT, &pins);

  if (!codecInit()) {
    Console.println("[sound] ES8311 init failed");
    i2s_driver_uninstall(PORT);
    return false;
  }
  sQueue = xQueueCreate(4, sizeof(Request));
  sMusicLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(soundTask, "sound", 8192, nullptr, 3, nullptr, 0);  // MP3 decoding needs the stack
  sPresent = true;
  Console.println("[sound] ES8311 ready");
  return true;
}

bool soundPresent() { return sPresent; }

void soundPlay(Sound s) {
  if (!sPresent || s == SOUND_NONE || gConfig.volume == 0) return;
  Request req = {s, 255};
  xQueueSend(sQueue, &req, 0);
}

void soundEffect(Sound s, uint8_t level) {
  if (!sPresent || s == SOUND_NONE || gConfig.volume == 0 || !level) return;
  if (sMusicOn || uxQueueMessagesWaiting(sQueue)) return;  // effect sounds would chop up the music
  Request req = {s, level};
  xQueueSend(sQueue, &req, 0);
}

bool musicPlay(const char *path, bool loop) {
  if (!sPresent || !fsValidPath(path) || !musicFileSupported(path) || !fsExists(path)) return false;
  xSemaphoreTake(sMusicLock, portMAX_DELAY);
  sMusicReq = path;
  sMusicReqLoop = loop;
  xSemaphoreGive(sMusicLock);
  Request req = {CMD_MUSIC_PLAY, 0};
  return xQueueSend(sQueue, &req, pdMS_TO_TICKS(200)) == pdTRUE;
}

void musicStop() {
  if (!sPresent) return;
  Request req = {CMD_MUSIC_STOP, 0};
  xQueueSend(sQueue, &req, pdMS_TO_TICKS(200));
}

bool musicPlaying() { return sMusicOn; }

String musicNowPlaying() {
  xSemaphoreTake(sMusicLock, portMAX_DELAY);
  String p = sMusicNow;
  xSemaphoreGive(sMusicLock);
  return p;
}

Sound soundFromName(const char *name) {
  if (!name) return SOUND_NONE;
  if (!strcmp(name, "tick")) return SOUND_TICK;
  if (!strcmp(name, "beep")) return SOUND_BEEP;
  if (!strcmp(name, "chime")) return SOUND_CHIME;
  if (!strcmp(name, "alarm")) return SOUND_ALARM;
  if (!strcmp(name, "clack")) return SOUND_CLACK;
  if (!strcmp(name, "knock")) return SOUND_KNOCK;
  if (!strcmp(name, "rattle")) return SOUND_RATTLE;
  if (!strcmp(name, "splash")) return SOUND_SPLASH;
  if (!strcmp(name, "whoosh")) return SOUND_WHOOSH;
  if (!strcmp(name, "fizz")) return SOUND_FIZZ;
  return SOUND_NONE;
}
