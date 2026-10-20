#include "mic.h"
#include <Wire.h>
#include <driver/i2s.h>
#include <freertos/stream_buffer.h>
#include <new>
#include "config.h"
#include "sound.h"
#include "usb_console.h"

static const uint8_t ES7210_ADDR = 0x40;
static const i2s_port_t PORT = I2S_NUM_0;
static const float RATE = 48000;
static const int N = 2048;    // FFT size (23Hz bins)
static const int HOP = 1024;  // new samples per analysis: 21ms
static const float F_LO = 50, F_HI = 16000;

static bool sPresent = false;
static volatile int sUsers[MIC_SOURCES] = {};
static portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
static MicFrame sFrame[MIC_SOURCES];
static StreamBufferHandle_t sMusicIn = nullptr;  // sound task -> analysis
static uint32_t sSimUntil = 0;  // micSimulate()

// Made-up music: a few wandering peaks over a bass line beating at 120bpm.
static void simulate(MicFrame &f) {
  static uint32_t seq = 0, beats = 0, lastBeat = 0;
  uint32_t t = millis();
  float beatPhase = (t % 500) / 500.0f;
  float kick = expf(-beatPhase * 6);
  if (t - lastBeat >= 500) {
    lastBeat = t - t % 500;
    beats++;
  }
  float lvl = 0;
  for (int b = 0; b < MIC_BANDS; b++) {
    float x = (float)b / MIC_BANDS;
    float v = kick * expf(-x * 14) * 1.1f;
    for (int k = 0; k < 3; k++) {
      float c = 0.25f + 0.2f * k + 0.12f * sinf(t / (700.0f + k * 310) + k);
      v += 0.75f * expf(-sq((x - c) * 18)) * (0.6f + 0.4f * sinf(t / (230.0f + k * 90)));
    }
    v += 0.15f * (random(0, 100) / 100.0f) * (1 - x);
    f.bands[b] = constrain(v, 0.0f, 1.0f);
    f.bandDb[b] = -60 + (int)(f.bands[b] * 40);
    lvl += f.bands[b];
  }
  f.level = lvl / MIC_BANDS;
  f.bass = min(1.0f, kick * 1.1f);
  f.dbfs = -40 + f.level * 20;
  f.peakHz = 440;
  f.beats = beats;
  f.seq = ++seq;
}

// --- ES7210 ------------------------------------------------------------------
// Register sequence follows Espressif's esp_codec_dev es7210.c (Apache
// License 2.0): slave mode, MCLK = 256 * fs from the pin, 16-bit I2S, MIC1+2.

static bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES7210_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static uint8_t readReg(uint8_t reg) {
  Wire.beginTransmission(ES7210_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)ES7210_ADDR, 1) != 1) return 0;
  return Wire.read();
}

static bool updateReg(uint8_t reg, uint8_t mask, uint8_t val) {
  return writeReg(reg, (readReg(reg) & ~mask) | (val & mask));
}

static uint8_t gainCode(int db) {
  if (db < 33) return db / 3;  // 0..30dB in 3dB steps
  if (db < 36) return 12;      // 34.5dB
  if (db < 37) return 13;      // 36dB
  return 14;                   // 37.5dB
}

static bool selectMics(uint8_t gain) {
  bool ok = true;
  for (int i = 0; i < 4; i++) ok &= updateReg(0x43 + i, 0x10, 0x00);
  ok &= writeReg(0x4B, 0xFF);
  ok &= writeReg(0x4C, 0xFF);
  ok &= updateReg(0x01, 0x0B, 0x00);  // MIC1/2 clocks on
  ok &= writeReg(0x4B, 0x00);         // MIC1/2 power on
  for (uint8_t reg : {0x43, 0x44}) {
    ok &= updateReg(reg, 0x10, 0x10);
    ok &= updateReg(reg, 0x0F, gain);
  }
  ok &= writeReg(0x12, 0x00);  // not TDM: MIC1 left, MIC2 right
  return ok;
}

static bool codecInit() {
  uint8_t gain = gainCode(gConfig.micGain);
  bool ok = writeReg(0x00, 0xFF);  // reset
  ok &= writeReg(0x00, 0x41);
  ok &= writeReg(0x01, 0x3F);
  ok &= writeReg(0x09, 0x30);  // chip state cycle
  ok &= writeReg(0x0A, 0x30);  // power on state cycle
  ok &= writeReg(0x23, 0x2A);  // high-pass filters
  ok &= writeReg(0x22, 0x0A);
  ok &= writeReg(0x20, 0x0A);
  ok &= writeReg(0x21, 0x2A);
  ok &= updateReg(0x08, 0x01, 0x00);  // slave mode
  ok &= writeReg(0x40, 0x43);         // analog power, VMID 5k
  ok &= writeReg(0x41, 0x70);         // MIC1/2 bias 2.87V
  ok &= writeReg(0x42, 0x70);
  ok &= writeReg(0x07, 0x20);  // OSR
  ok &= writeReg(0x02, 0xC1);  // clock dividers for MCLK = 256 * fs
  ok &= selectMics(gain);
  uint8_t clockOff = readReg(0x01);
  ok &= updateReg(0x11, 0xE0, 0x60);  // 16-bit
  ok &= updateReg(0x11, 0x03, 0x00);  // standard I2S
  // start
  ok &= writeReg(0x01, clockOff);
  ok &= writeReg(0x06, 0x00);  // power up
  ok &= writeReg(0x40, 0x43);
  for (uint8_t reg = 0x47; reg <= 0x4A; reg++) ok &= writeReg(reg, 0x08);
  ok &= selectMics(gain);
  ok &= writeReg(0x40, 0x43);
  ok &= writeReg(0x00, 0x71);
  ok &= writeReg(0x00, 0x41);
  return ok;
}

// --- Analysis ----------------------------------------------------------------

// What the analysis remembers about one source between blocks.
struct SourceState {
  float history[N];          // last N mono samples
  float floor[MIC_BANDS];    // per-band noise floor, dB
  float smooth[MIC_BANDS];   // band power, smoothed
  bool primed = false;
  float ref = -60;           // auto-gain reference, dB
  float bassAvg = 0;
  uint32_t lastBeatMs = 0;
  MicFrame frame;
};

// FFT workspace and tables, shared by the sources.
struct Analyzer {
  float re[N], im[N];
  float window[N];
  float cosT[N / 2], sinT[N / 2];
  uint16_t lo[MIC_BANDS], hi[MIC_BANDS];  // FFT bin range per band

  void init() {
    for (int i = 0; i < N; i++) window[i] = 0.5f - 0.5f * cosf(2 * PI * i / (N - 1));
    for (int i = 0; i < N / 2; i++) {
      cosT[i] = cosf(2 * PI * i / N);
      sinT[i] = -sinf(2 * PI * i / N);
    }
    const float df = RATE / N;
    for (int b = 0; b < MIC_BANDS; b++) {
      float f0 = F_LO * powf(F_HI / F_LO, (float)b / MIC_BANDS);
      float f1 = F_LO * powf(F_HI / F_LO, (float)(b + 1) / MIC_BANDS);
      lo[b] = max(1, (int)lroundf(f0 / df));
      hi[b] = max<int>(lo[b], (int)lroundf(f1 / df) - 1);
    }
  }

  void fft() {
    for (int i = 1, j = 0; i < N; i++) {  // bit reversal
      int bit = N >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) {
        std::swap(re[i], re[j]);
        std::swap(im[i], im[j]);
      }
    }
    for (int len = 2; len <= N; len <<= 1) {
      int step = N / len;
      for (int i = 0; i < N; i += len) {
        for (int k = 0; k < len / 2; k++) {
          float wr = cosT[k * step], wi = sinT[k * step];
          int a = i + k, b = a + len / 2;
          float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
          re[b] = re[a] - xr;
          im[b] = im[a] - xi;
          re[a] += xr;
          im[a] += xi;
        }
      }
    }
  }

  void run(const int16_t *stereo, SourceState &st) {
    float *history = st.history, *floor = st.floor, *smooth = st.smooth;
    float &ref = st.ref, &bassAvg = st.bassAvg;
    uint32_t &lastBeatMs = st.lastBeatMs;
    bool &primed = st.primed;
    MicFrame &f = st.frame;
    memmove(history, history + HOP, sizeof(float) * (N - HOP));
    double sumSq = 0;
    for (int i = 0; i < HOP; i++) {
      float s = (stereo[i * 2] + stereo[i * 2 + 1]) * (0.5f / 32768);
      sumSq += s * s;
      history[N - HOP + i] = s;
    }
    for (int i = 0; i < N; i++) {
      re[i] = history[i] * window[i];
      im[i] = 0;
    }
    fft();
    // Power per bin, scaled so a full-scale sine is about 0dB.
    const float norm = 4.0f / ((float)N * N * 0.25f);
    float best = 0;
    int bestBin = 0;
    for (int k = 1; k < N / 2; k++) {
      float p = (re[k] * re[k] + im[k] * im[k]) * norm;
      re[k] = p;
      if (p > best) best = p, bestBin = k;
    }
    float db[MIC_BANDS], top = -120, bassLin = 0;
    for (int b = 0; b < MIC_BANDS; b++) {
      float p = 0;
      for (int k = lo[b]; k <= hi[b]; k++) p += re[k];
      p /= hi[b] - lo[b] + 1;
      // Light smoothing over time evens out the flicker of noise.
      smooth[b] = p > smooth[b] ? p : smooth[b] * 0.5f + p * 0.5f;
      p = smooth[b];
      db[b] = 10 * log10f(p + 1e-12f);
      f.bandDb[b] = (int8_t)constrain((int)db[b], -128, 127);
      top = max(top, db[b]);
      if (b < 14) bassLin += p;
    }
    // Noise floor per band: follows quiet passages down at once, creeps up
    // slowly (~0.25dB/s) so steady room noise is subtracted out.
    float floorTop = -120;
    for (int b = 0; b < MIC_BANDS; b++) {
      if (!primed || db[b] < floor[b]) floor[b] = db[b];
      else floor[b] += 0.005f;
      floorTop = max(floorTop, floor[b]);
    }
    primed = true;
    // Auto gain: jump up to loud sounds, sink back slowly (~3dB/s), but
    // never below a fixed level (a quiet room measures -75..-110dB per band)
    // so that silence stays dark.
    const float RANGE = 36, MIN_REF = -52, GATE = -74;
    ref = top > ref ? top : max(ref - 0.06f, top);
    float r = max(max(ref, MIN_REF), floorTop + 18);
    float lvl = 0;
    for (int b = 0; b < MIC_BANDS; b++) {
      float low = max(max(r - RANGE, floor[b] + 8), GATE);
      float v = constrain((db[b] - low) / max(r - low, 6.0f), 0.0f, 1.0f);
      f.bands[b] = v;
      lvl += v;
    }
    f.level = lvl / MIC_BANDS;
    float bassSum = 0;
    for (int b = 0; b < 14; b++) bassSum += f.bands[b];
    f.bass = bassSum / 14;
    f.dbfs = 10 * log10f(sumSq / HOP + 1e-12f);
    f.peakHz = bestBin * RATE / N;
    // Beat: bass well above its running average.
    uint32_t now = millis();
    if (bassLin > bassAvg * 2.2f && f.bass > 0.35f && now - lastBeatMs > 180) {
      f.beats++;
      lastBeatMs = now;
    }
    bassAvg += (bassLin - bassAvg) * 0.08f;
    f.seq++;
  }
};

static void publish(MicSource src, const SourceState &st) {
  portENTER_CRITICAL(&sMux);
  sFrame[src] = st.frame;
  portEXIT_CRITICAL(&sMux);
}

static void micTask(void *) {
  Analyzer *an = (Analyzer *)heap_caps_malloc(sizeof(Analyzer), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  // The music's state lives in PSRAM; it is only touched once per block.
  SourceState *mic = (SourceState *)heap_caps_malloc(sizeof(SourceState), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  SourceState *music = (SourceState *)heap_caps_malloc(sizeof(SourceState), MALLOC_CAP_SPIRAM);
  int16_t *buf = (int16_t *)heap_caps_malloc(HOP * 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int16_t *musicBuf = (int16_t *)heap_caps_calloc(HOP, 4, MALLOC_CAP_SPIRAM);
  if (!an || !mic || !music || !buf || !musicBuf) {
    Console.println("[mic] out of memory");
    vTaskDelete(nullptr);
  }
  new (an) Analyzer();
  new (mic) SourceState();
  new (music) SourceState();
  an->init();
  size_t musicFill = 0;  // bytes of musicBuf filled
  for (;;) {
    const bool wantMic = sUsers[MIC_SRC_MIC] > 0, wantMusic = sUsers[MIC_SRC_MUSIC] > 0;
    if (!wantMic && !wantMusic) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (wantMic) {
      size_t got = 0;
      i2s_read(PORT, buf, HOP * 4, &got, pdMS_TO_TICKS(200));
      if (got == (size_t)HOP * 4) {
        an->run(buf, *mic);
        publish(MIC_SRC_MIC, *mic);
      }
    }
    if (wantMusic) {
      // Blocks only when the microphones are not pacing the loop already.
      const TickType_t wait = wantMic ? 0 : pdMS_TO_TICKS(30);
      musicFill += xStreamBufferReceive(sMusicIn, (uint8_t *)musicBuf + musicFill, HOP * 4 - musicFill, wait);
      if (musicFill == (size_t)HOP * 4) {
        an->run(musicBuf, *music);
        publish(MIC_SRC_MUSIC, *music);
        musicFill = 0;
      } else if (!musicPlaying()) {
        // Nothing playing: analyse silence so the display settles to dark.
        memset(musicBuf, 0, HOP * 4);
        musicFill = 0;
        an->run(musicBuf, *music);
        publish(MIC_SRC_MUSIC, *music);
      }
    }
  }
}

// --- API ---------------------------------------------------------------------

bool micBegin() {
  if (!soundPresent()) return false;  // no I2S driver
  Wire.beginTransmission(ES7210_ADDR);
  if (Wire.endTransmission() != 0) {
    Console.println("[mic] ES7210 not found");
    return false;
  }
  if (!codecInit()) {
    Console.println("[mic] ES7210 init failed");
    return false;
  }
  sMusicIn = xStreamBufferCreate(HOP * 4 * 2, 1);
  xTaskCreatePinnedToCore(micTask, "mic", 4096, nullptr, 2, nullptr, 0);
  sPresent = true;
  Console.println("[mic] ES7210 ready");
  return true;
}

bool micPresent() { return sPresent; }

void micApplyGain() {
  if (!sPresent) return;
  uint8_t g = gainCode(gConfig.micGain);
  updateReg(0x43, 0x0F, g);
  updateReg(0x44, 0x0F, g);
}

void micAcquire(MicSource src) {
  portENTER_CRITICAL(&sMux);
  sUsers[src]++;
  portEXIT_CRITICAL(&sMux);
}

void micRelease(MicSource src) {
  portENTER_CRITICAL(&sMux);
  if (sUsers[src] > 0) sUsers[src]--;
  portEXIT_CRITICAL(&sMux);
}

MicFrame micGet(MicSource src) {
  MicFrame f;
  if ((int32_t)(sSimUntil - millis()) > 0) {
    simulate(f);
    return f;
  }
  portENTER_CRITICAL(&sMux);
  f = sFrame[src];
  portEXIT_CRITICAL(&sMux);
  return f;
}

void micFeedMusic(const int16_t *stereo, size_t frames) {
  if (!sMusicIn || sUsers[MIC_SRC_MUSIC] <= 0) return;
  xStreamBufferSend(sMusicIn, stereo, frames * 4, 0);  // drops what does not fit
}

void micSimulate(uint32_t ms) { sSimUntil = millis() + ms; }

void micJson(JsonObject o, MicSource src) {
  // Briefly listens when nobody else is, so the command always has data.
  bool idle = sUsers[src] == 0;
  if (idle) {
    micAcquire(src);
    delay(150);
  }
  MicFrame f = micGet(src);
  if (idle) micRelease(src);
  o["present"] = sPresent;
  o["dbfs"] = roundf(f.dbfs * 10) / 10;
  o["level"] = roundf(f.level * 100) / 100;
  o["bass"] = roundf(f.bass * 100) / 100;
  o["peakHz"] = (int)f.peakHz;
  o["beats"] = f.beats;
  JsonArray b = o["bands"].to<JsonArray>();
  for (float v : f.bands) b.add((int)(v * 99));
  JsonArray db = o["bandDb"].to<JsonArray>();
  for (int8_t v : f.bandDb) db.add(v);
}
