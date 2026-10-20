// Effects driven by the microphones (spectrum) and the IMU (tilt / shake).
#include <FastLED.h>
#include "color.h"
#include "effects.h"
#include "mic.h"
#include "sensors.h"
#include "sound.h"

static inline uint16_t crgb565(const CRGB &c) { return rgb565(c.r, c.g, c.b); }

// Cheap PRNG: Arduino's random() reads the hardware RNG, which waits ~20us
// per call and made the grain effects crawl at a few frames per second.
static uint32_t sRng = 0x9E3779B9;
static inline uint32_t rnd() {
  sRng ^= sRng << 13;
  sRng ^= sRng >> 17;
  sRng ^= sRng << 5;
  return sRng;
}
// Integer in [lo, hi).
static inline int rndInt(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo)); }
static inline float frand(float lo, float hi) { return lo + (hi - lo) * ((rnd() >> 8) * (1.0f / 16777216)); }

// Keeps the microphone analysis running while an effect is on screen.
struct MicUser {
  MicUser() { micAcquire(); }
  ~MicUser() { micRelease(); }
};

// Frame buffer kept between frames, for trails.
struct Trail {
  uint16_t *buf = nullptr;
  int n = 0;
  ~Trail() { free(buf); }
  void init(int w, int h) {
    n = w * h;
    buf = (uint16_t *)calloc(n, 2);
  }
  // Fades the stored frame by s/256 into the canvas.
  void restore(GFXcanvas16 &c, uint16_t s) {
    uint16_t *dst = c.getBuffer();
    for (int i = 0; i < n; i++) dst[i] = scale565(buf[i], s);
  }
  void store(GFXcanvas16 &c) { memcpy(buf, c.getBuffer(), n * 2); }
};

// Adds two RGB565 colors with saturation.
static inline uint16_t add565(uint16_t a, uint16_t b) {
  uint16_t r = min(31, (a >> 11) + (b >> 11));
  uint16_t g = min(63, ((a >> 5) & 0x3F) + ((b >> 5) & 0x3F));
  uint16_t bl = min(31, (a & 0x1F) + (b & 0x1F));
  return (r << 11) | (g << 5) | bl;
}

static inline void addPixel(GFXcanvas16 &c, int x, int y, uint16_t col) {
  if (x < 0 || y < 0 || x >= c.width() || y >= c.height()) return;
  uint16_t *p = c.getBuffer() + y * c.width() + x;
  *p = add565(*p, col);
}

// Effect sounds can be turned off per item with "sound": false.
static bool soundOn(JsonObjectConst p) { return p["sound"] | true; }

// How hard the panel is being shaken: 0 when still or barely moved, 1 for a
// vigorous shake by hand (measured: sensor shake ~0.05-0.2 g light, ~0.3 g hard).
static float shakeAmount(const SensorData &s) { return constrain((s.shake - 0.05f) / 0.22f, 0.0f, 1.5f); }

// Plays a motion effect's sound while the panel is being shaken, louder the
// harder it shakes, at most once per `everyMs`.
struct ShakeSound {
  Sound sound = SOUND_NONE;
  uint16_t everyMs = 200;
  uint32_t wait = 0;
  void set(JsonObjectConst p, Sound s, uint16_t every) {
    sound = soundOn(p) ? s : SOUND_NONE;
    everyMs = every;
  }
  void update(float amount, uint32_t dt) {
    wait = wait > dt ? wait - dt : 0;
    if (sound == SOUND_NONE || amount <= 0 || wait) return;
    soundEffect(sound, (uint8_t)constrain(70 + amount * 185, 70.0f, 255.0f));
    wait = everyMs;
  }
};

// ===========================================================================
// Audio
// ===========================================================================

// Smoothed bar levels with falling peak caps, shared by the spectrum styles.
struct Bars {
  int n = 0;
  float val[MIC_BANDS] = {}, peak[MIC_BANDS] = {};
  int32_t hold[MIC_BANDS] = {};

  void update(const MicFrame &f, uint32_t dt) {
    const float fdt = dt / 1000.0f;
    for (int i = 0; i < n; i++) {
      int b0 = i * MIC_BANDS / n, b1 = max(b0 + 1, (i + 1) * MIC_BANDS / n);
      float v = 0;
      for (int b = b0; b < b1; b++) v = max(v, f.bands[b]);
      val[i] = v > val[i] ? v : max(v, val[i] - 2.5f * fdt);
      if (val[i] >= peak[i]) {
        peak[i] = val[i];
        hold[i] = 350;
      } else if ((hold[i] -= (int32_t)dt) <= 0) {
        peak[i] = max(0.0f, peak[i] - 0.9f * fdt);
      }
    }
  }
};

// Classic bar graph, neon colors drifting across, peak caps. "style":
// "bars" grows from the bottom, "mirror" grows both ways from the middle.
class SpectrumEffect : public Effect {
 public:
  void begin(int w, int h, JsonObjectConst p) override {
    Effect::begin(w, h, p);
    mirror_ = !strcmp(p["style"] | "bars", "mirror");
    barW_ = w >= 96 ? 3 : 2;
    gap_ = w >= 40 ? 1 : 0;
    bars_.n = constrain((w + gap_) / (barW_ + gap_), 4, MIC_BANDS);
    x0_ = (w - (bars_.n * (barW_ + gap_) - gap_)) / 2;
  }
  void render(GFXcanvas16 &c, uint32_t dt) override {
    MicFrame f = micGet();
    bars_.update(f, dt);
    hue_ += dt * 0.02f;
    if (f.beats != beats_) {
      beats_ = f.beats;
      flash_ = 1;
    }
    flash_ = max(0.0f, flash_ - dt / 300.0f);
    c.fillScreen(hsv565((uint8_t)hue_ + 128, 255, (uint8_t)(flash_ * 36)));
    for (int i = 0; i < bars_.n; i++) {
      uint8_t hue = (uint8_t)hue_ + i * 160 / bars_.n;
      int x = x0_ + i * (barW_ + gap_);
      if (mirror_) drawMirror(c, x, hue, bars_.val[i], bars_.peak[i]);
      else drawUp(c, x, hue, bars_.val[i], bars_.peak[i]);
    }
  }

 private:
  void drawUp(GFXcanvas16 &c, int x, uint8_t hue, float v, float pk) {
    int len = (int)(v * (h_ - 1) + 0.5f);
    for (int j = 0; j < len; j++) {
      // Brighter and whiter towards the top of the bar.
      float t = (float)j / h_;
      uint16_t col = hsv565(hue + (int)(t * 50), 255 - (int)(t * 90), 120 + (int)(t * 135));
      c.drawFastHLine(x, h_ - 1 - j, barW_, col);
    }
    int py = h_ - 1 - (int)(pk * (h_ - 1) + 0.5f);
    if (pk > 0.02f) c.drawFastHLine(x, py, barW_, hsv565(hue + 40, 80, 255));
  }
  void drawMirror(GFXcanvas16 &c, int x, uint8_t hue, float v, float pk) {
    float half = h_ / 2.0f;
    int len = (int)(v * half + 0.5f);
    for (int j = 0; j < len; j++) {
      float t = (float)j / half;
      uint16_t col = hsv565(hue + (int)(t * 60), 255 - (int)((1 - t) * 120), 255 - (int)(t * 100));
      c.drawFastHLine(x, (int)half - 1 - j, barW_, col);
      c.drawFastHLine(x, (int)half + j, barW_, scale565(col, 200));
    }
    int d = (int)(pk * half + 0.5f);
    if (pk > 0.02f) {
      uint16_t cap = hsv565(hue + 60, 60, 255);
      c.drawFastHLine(x, (int)half - 1 - d, barW_, cap);
      c.drawFastHLine(x, (int)half + d, barW_, scale565(cap, 200));
    }
  }

  MicUser mic_;
  Bars bars_;
  int barW_ = 2, gap_ = 1, x0_ = 0;
  bool mirror_ = false;
  float hue_ = 0, flash_ = 0;
  uint32_t beats_ = 0;
};

// Spectrum as rays around a pulsing core, rings bursting out on beats,
// everything leaving motion trails.
class RadialEffect : public Effect {
 public:
  void begin(int w, int h, JsonObjectConst p) override {
    Effect::begin(w, h, p);
    trail_.init(w, h);
    bars_.n = 24;
  }
  void render(GFXcanvas16 &c, uint32_t dt) override {
    MicFrame f = micGet();
    bars_.update(f, dt);
    const float fdt = dt / 1000.0f;
    hue_ += dt * 0.03f;
    rot_ += fdt * (0.4f + f.level * 2.5f);
    bass_ = f.bass > bass_ ? f.bass : max(f.bass, bass_ - 2 * fdt);
    if (f.beats != beats_) {
      beats_ = f.beats;
      rings_[ringNext_] = {1.0f, (uint8_t)hue_, true};
      ringNext_ = (ringNext_ + 1) % RINGS;
    }

    trail_.restore(c, 165);
    const float cx = (w_ - 1) / 2.0f, cy = (h_ - 1) / 2.0f;
    const float unit = min(w_, h_) / 2.0f;
    const float ex = (w_ / 2.0f) / unit, ey = (h_ / 2.0f) / unit;  // stretch to the panel
    const float core = unit * (0.1f + bass_ * 0.2f);

    for (Ring &r : rings_) {
      if (!r.on) continue;
      r.radius += fdt * unit * 1.6f;
      if (r.radius > unit * 2.2f) {
        r.on = false;
        continue;
      }
      uint8_t v = (uint8_t)(255 * (1 - r.radius / (unit * 2.2f)));
      ellipse(c, cx, cy, r.radius * ex, r.radius * ey, hsv565(r.hue + 100, 200, v));
    }

    // Rays: the spectrum mirrored around the circle.
    const int rays = bars_.n * 2;
    for (int k = 0; k < rays; k++) {
      int bi = k < bars_.n ? k : rays - 1 - k;
      float a = rot_ + k * 2 * PI / rays;
      float ca = cosf(a), sa = sinf(a);
      float len = bars_.val[bi] * unit * 1.1f;
      uint8_t hue = (uint8_t)hue_ + bi * 6;
      int steps = (int)len + 1;
      for (int s = 0; s <= steps; s++) {
        float rr = core + len * s / steps;
        uint8_t v = 150 - s * 90 / steps;
        addPixel(c, (int)lroundf(cx + ca * rr * ex), (int)lroundf(cy + sa * rr * ey), hsv565(hue, 255, v));
      }
    }
    // Glowing core.
    for (int y = (int)(cy - core - 1); y <= (int)(cy + core + 1); y++)
      for (int x = (int)(cx - core * ex - 1); x <= (int)(cx + core * ex + 1); x++) {
        float dx = (x - cx) / ex, dy = y - cy;
        float d = sqrtf(dx * dx + dy * dy);
        if (d <= core) c.drawPixel(x, y, hsv565((uint8_t)hue_ + 128, 200, (uint8_t)(70 + 150 * (1 - d / core))));
      }
    trail_.store(c);
  }

 private:
  static void ellipse(GFXcanvas16 &c, float cx, float cy, float rx, float ry, uint16_t col) {
    int steps = max(16, (int)((rx + ry) * 3));
    for (int i = 0; i < steps; i++) {
      float a = i * 2 * PI / steps;
      addPixel(c, (int)lroundf(cx + cosf(a) * rx), (int)lroundf(cy + sinf(a) * ry), col);
    }
  }
  struct Ring {
    float radius;
    uint8_t hue;
    bool on;
  };
  static const int RINGS = 6;
  MicUser mic_;
  Bars bars_;
  Trail trail_;
  Ring rings_[RINGS] = {};
  int ringNext_ = 0;
  float hue_ = 0, rot_ = 0, bass_ = 0;
  uint32_t beats_ = 0;
};

// Spectrogram: time runs right to left, low notes at the bottom.
class WaterfallEffect : public Effect {
 public:
  void begin(int w, int h, JsonObjectConst p) override {
    Effect::begin(w, h, p);
    palette_ = !strcmp(p["palette"] | "heat", "ocean") ? OceanColors_p : HeatColors_p;
    trail_.init(w, h);
  }
  void render(GFXcanvas16 &c, uint32_t dt) override {
    MicFrame f = micGet();
    if (f.seq != seq_) {
      seq_ = f.seq;
      // One column per two analysis blocks: ~23px/s.
      if (++tick_ % 2 == 0) {
        uint16_t *b = trail_.buf;
        for (int y = 0; y < h_; y++) {
          memmove(b + y * w_, b + y * w_ + 1, (w_ - 1) * 2);
          int b0 = (h_ - 1 - y) * MIC_BANDS / h_, b1 = max(b0 + 1, (h_ - y) * MIC_BANDS / h_);
          float v = 0;
          for (int k = b0; k < b1; k++) v = max(v, f.bands[k]);
          b[y * w_ + w_ - 1] = v < 0.04f ? 0 : crgb565(ColorFromPalette(palette_, (uint8_t)(v * 240), (uint8_t)(60 + v * 195)));
        }
      }
    }
    trail_.restore(c, 256);
  }

 private:
  MicUser mic_;
  Trail trail_;
  CRGBPalette16 palette_;
  uint32_t seq_ = 0, tick_ = 0;
};

// ===========================================================================
// Motion
// ===========================================================================

// In-plane gravity as one of 8 directions, picking between the two nearest
// at random so in-between angles average out; false when lying flat.
static bool gravityDir(const SensorData &s, int &dx, int &dy) {
  float m = sqrtf(s.gravX * s.gravX + s.gravY * s.gravY);
  if (m < 0.2f) return false;
  static const int8_t DX[8] = {1, 1, 0, -1, -1, -1, 0, 1}, DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
  float sector = atan2f(s.gravY, s.gravX) / (PI / 4);
  if (sector < 0) sector += 8;
  int base = (int)sector;
  int k = (frand(0, 1) < sector - base ? base + 1 : base) & 7;
  dx = DX[k];
  dy = DY[k];
  return true;
}

// Falling grains on a grid. "sand" piles up, "liquid" also flows sideways
// and levels out.
class GrainEffect : public Effect {
 public:
  explicit GrainEffect(bool liquid) : liquid_(liquid) {}
  ~GrainEffect() override {
    free(cell_);
    free(stamp_);
  }
  void begin(int w, int h, JsonObjectConst p) override {
    Effect::begin(w, h, p);
    cell_ = (uint8_t *)calloc(w * h, 1);
    stamp_ = (uint8_t *)calloc(w * h, 1);
    shakeSound_.set(p, liquid_ ? SOUND_SPLASH : SOUND_RATTLE, liquid_ ? 260 : 150);
    float fill = constrain((float)(p["fill"] | (liquid_ ? 50 : 40)), 5.0f, 90.0f) / 100;
    int rows = (int)(h * fill);
    for (int y = h - rows; y < h; y++)
      for (int x = 0; x < w; x++) {
        // Sand: colored layers. Liquid: a random shade per drop.
        cell_[y * w + x] = liquid_ ? 1 + rndInt(0, 255) : 1 + ((y - (h - rows)) * 6 / max(1, rows)) * 40 + rndInt(0, 12);
      }
  }
  void render(GFXcanvas16 &c, uint32_t dt) override {
    SensorData s = sensorsGet();
    shakeSound_.update(shakeAmount(s), dt);
    acc_ += dt;
    int steps = 0;
    while (acc_ >= 16 && steps < 4) {
      acc_ -= 16;
      steps++;
      step(s);
    }
    if (acc_ > 64) acc_ = 0;
    t_ += dt;
    uint16_t *buf = c.getBuffer();
    for (int i = 0; i < w_ * h_; i++) {
      uint8_t v = cell_[i];
      if (!v) {
        buf[i] = 0;
      } else if (liquid_) {
        // Blue water with a slow shimmer.
        uint8_t n = inoise8((i % w_) * 40, (i / w_) * 40, t_ / 3);
        buf[i] = hsv565(140 + (v >> 4) + (n >> 4), 230 - (n >> 3), 140 + (n >> 1));
      } else {
        buf[i] = hsv565(v, 220, 255);
      }
    }
  }

 private:
  bool free_(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_ && !cell_[y * w_ + x]; }

  void move(int from, int x, int y) {
    int to = y * w_ + x;
    cell_[to] = cell_[from];
    cell_[from] = 0;
    stamp_[to] = frame_;
  }

  void step(const SensorData &s) {
    frame_++;
    if (frame_ == 0) {
      memset(stamp_, 0, w_ * h_);
      frame_ = 1;
    }
    // Shaking throws grains against gravity.
    if (shakeAmount(s) > 0.15f) splash(s);
    int dx, dy;
    if (!gravityDir(s, dx, dy)) return;
    // Visit the cells nearest the "floor" first so columns fall as a whole.
    const bool flipX = rndInt(0, 2);
    for (int oy = 0; oy < h_; oy++) {
      int y = dy > 0 ? h_ - 1 - oy : dy < 0 ? oy : (frame_ & 1 ? oy : h_ - 1 - oy);
      for (int ox = 0; ox < w_; ox++) {
        int x = dx > 0 ? w_ - 1 - ox : dx < 0 ? ox : (flipX ? w_ - 1 - ox : ox);
        int i = y * w_ + x;
        if (!cell_[i] || stamp_[i] == frame_) continue;
        if (free_(x + dx, y + dy)) {
          move(i, x + dx, y + dy);
          continue;
        }
        // Diagonals: the gravity direction turned by +-45 degrees.
        int ax = dx - dy, ay = dy + dx, bx = dx + dy, by = dy - dx;
        ax = constrain(ax, -1, 1), ay = constrain(ay, -1, 1), bx = constrain(bx, -1, 1), by = constrain(by, -1, 1);
        if (rndInt(0, 2)) std::swap(ax, bx), std::swap(ay, by);
        if (free_(x + ax, y + ay)) {
          move(i, x + ax, y + ay);
          continue;
        }
        if (free_(x + bx, y + by)) {
          move(i, x + bx, y + by);
          continue;
        }
        // Slide sideways (perpendicular to gravity): liquid always and a few
        // cells at a time, sand now and then, which gives it a slope.
        if (!liquid_ && rndInt(0, 100) >= 30) continue;
        int px = -dy, py = dx;
        if (rndInt(0, 2)) px = -px, py = -py;
        const int reach = liquid_ ? 3 : 1;
        for (int dir = 0; dir < 2; dir++, px = -px, py = -py) {
          int nx = x, ny = y;
          for (int k = 0; k < reach && free_(nx + px, ny + py); k++) nx += px, ny += py;
          if (nx != x || ny != y) {
            move(i, nx, ny);
            break;
          }
        }
      }
    }
  }

  void splash(const SensorData &s) {
    float m = sqrtf(s.gravX * s.gravX + s.gravY * s.gravY);
    float ux = m > 0.2f ? -s.gravX / m : 0, uy = m > 0.2f ? -s.gravY / m : -1;
    const float amount = shakeAmount(s);
    int tries = (int)(w_ * h_ * min(1.0f, amount) / 30);
    for (int t = 0; t < tries; t++) {
      int i = rndInt(0, w_ * h_);
      if (!cell_[i]) continue;
      int x = i % w_, y = i / w_;
      float d = frand(2, 2 + amount * h_ / 3);
      int nx = x + (int)(ux * d + frand(-2, 2)), ny = y + (int)(uy * d + frand(-2, 2));
      if (free_(nx, ny)) move(i, nx, ny);
    }
  }

  bool liquid_;
  uint8_t *cell_ = nullptr;   // 0 = empty, else color
  uint8_t *stamp_ = nullptr;  // frame a cell last moved in
  uint8_t frame_ = 0;
  uint32_t acc_ = 0, t_ = 0;
  ShakeSound shakeSound_;
};

// Glowing particles streaming downhill and wrapping around the edges, stirred
// by a noise field so they swirl when the panel lies flat.
class FlowEffect : public Effect {
 public:
  ~FlowEffect() override { free(p_); }
  void begin(int w, int h, JsonObjectConst p) override {
    Effect::begin(w, h, p);
    n_ = constrain((int)(p["count"] | (w * h / 12)), 16, 600);
    p_ = (Particle *)calloc(n_, sizeof(Particle));
    for (int i = 0; i < n_; i++) p_[i] = {frand(0, w), frand(0, h), 0, 0, (uint8_t)rndInt(0, 64)};
    trail_.init(w, h);
    shakeSound_.set(p, SOUND_WHOOSH, 320);
  }
  void render(GFXcanvas16 &c, uint32_t dt) override {
    SensorData s = sensorsGet();
    shakeSound_.update(shakeAmount(s), dt);
    const float fdt = min(dt, (uint32_t)50) / 1000.0f;
    t_ += dt;
    hue_ += dt * 0.01f;
    const float G = 70, MAXV = 45, shake = shakeAmount(s);
    trail_.restore(c, 215);
    for (int i = 0; i < n_; i++) {
      Particle &q = p_[i];
      uint8_t ang = inoise8((uint16_t)(q.x * 18), (uint16_t)(q.y * 18), t_ / 4);
      float a = ang * (2 * PI / 128);
      q.vx += (s.gravX * G + cosf(a) * 14) * fdt;
      q.vy += (s.gravY * G + sinf(a) * 14) * fdt;
      if (shake > 0) {
        q.vx += frand(-1, 1) * shake * 60;
        q.vy += frand(-1, 1) * shake * 60;
      }
      float damp = 1 - 0.9f * fdt;
      q.vx *= damp;
      q.vy *= damp;
      float sp = sqrtf(q.vx * q.vx + q.vy * q.vy);
      if (sp > MAXV) q.vx *= MAXV / sp, q.vy *= MAXV / sp, sp = MAXV;
      q.x += q.vx * fdt;
      q.y += q.vy * fdt;
      if (q.x < 0) q.x += w_;
      if (q.x >= w_) q.x -= w_;
      if (q.y < 0) q.y += h_;
      if (q.y >= h_) q.y -= h_;
      // Slow ones are deep blue, fast ones turn cyan and white.
      uint8_t hue = (uint8_t)hue_ + q.hue - (uint8_t)(sp * 2);
      addPixel(c, (int)q.x, (int)q.y, hsv565(hue, 255 - (uint8_t)(sp * 3), 140 + (uint8_t)(sp * 2.5f)));
    }
    trail_.store(c);
  }

 private:
  struct Particle {
    float x, y, vx, vy;
    uint8_t hue;
  };
  Particle *p_ = nullptr;
  int n_ = 0;
  Trail trail_;
  uint32_t t_ = 0;
  float hue_ = 140;
  ShakeSound shakeSound_;
};

// Shiny balls rolling with the tilt, bouncing off the walls and each other,
// clicking when they hit something.
class MarblesEffect : public Effect {
 public:
  void begin(int w, int h, JsonObjectConst p) override {
    Effect::begin(w, h, p);
    n_ = constrain((int)(p["count"] | 8), 1, MAX);
    r_ = max(1.5f, h / 11.0f);
    for (int i = 0; i < n_; i++) {
      b_[i] = {frand(r_, w - r_), frand(r_, h - r_), frand(-20, 20), frand(-20, 20), (uint8_t)(i * 256 / n_)};
    }
    trail_.init(w, h);
    sound_ = soundOn(p);
  }
  void render(GFXcanvas16 &c, uint32_t dt) override {
    SensorData s = sensorsGet();
    const float fdt = min(dt, (uint32_t)50) / 1000.0f;
    hitBall_ = hitWall_ = 0;
    for (int sub = 0; sub < 3; sub++) physics(s, fdt / 3);
    clicks(dt);
    trail_.restore(c, 120);
    for (int i = 0; i < n_; i++) {
      Ball &b = b_[i];
      uint16_t col = hsv565(b.hue, 210, 255);
      c.fillCircle((int)lroundf(b.x), (int)lroundf(b.y), (int)r_, scale565(col, 170));
      c.fillCircle((int)lroundf(b.x), (int)lroundf(b.y), max(0, (int)r_ - 1), col);
      c.drawPixel((int)lroundf(b.x - r_ * 0.4f), (int)lroundf(b.y - r_ * 0.4f), hsv565(b.hue, 60, 255));
    }
    trail_.store(c);
  }

 private:
  void physics(const SensorData &s, float dt) {
    const float G = 160, E_WALL = 0.55f, E_BALL = 0.85f, shake = shakeAmount(s);
    for (int i = 0; i < n_; i++) {
      Ball &b = b_[i];
      b.vx += s.gravX * G * dt;
      b.vy += s.gravY * G * dt;
      if (shake > 0) {
        b.vx += frand(-1, 1) * shake * 40;
        b.vy += frand(-1, 1) * shake * 40;
      }
      // Rolling friction.
      b.vx *= 1 - 0.25f * dt;
      b.vy *= 1 - 0.25f * dt;
      b.x += b.vx * dt;
      b.y += b.vy * dt;
      if (b.x < r_) wall(-b.vx), b.x = r_, b.vx = fabsf(b.vx) * E_WALL;
      if (b.x > w_ - 1 - r_) wall(b.vx), b.x = w_ - 1 - r_, b.vx = -fabsf(b.vx) * E_WALL;
      if (b.y < r_) wall(-b.vy), b.y = r_, b.vy = fabsf(b.vy) * E_WALL;
      if (b.y > h_ - 1 - r_) wall(b.vy), b.y = h_ - 1 - r_, b.vy = -fabsf(b.vy) * E_WALL;
    }
    for (int i = 0; i < n_; i++)
      for (int j = i + 1; j < n_; j++) {
        Ball &a = b_[i], &b = b_[j];
        float dx = b.x - a.x, dy = b.y - a.y, d = sqrtf(dx * dx + dy * dy), minD = r_ * 2;
        if (d >= minD || d < 1e-3f) continue;
        float nx = dx / d, ny = dy / d, push = (minD - d) / 2;
        a.x -= nx * push, a.y -= ny * push;
        b.x += nx * push, b.y += ny * push;
        float rel = (b.vx - a.vx) * nx + (b.vy - a.vy) * ny;
        if (rel >= 0) continue;
        hitBall_ = max(hitBall_, -rel);
        float imp = -(1 + E_BALL) * rel / 2;
        a.vx -= imp * nx, a.vy -= imp * ny;
        b.vx += imp * nx, b.vy += imp * ny;
      }
  }

  void wall(float speed) { hitWall_ = max(hitWall_, speed); }

  // The hardest hit of the frame makes the sound. Below MIN_HIT are the
  // little bounces of balls resting on the floor or against each other.
  void clicks(uint32_t dt) {
    static const float MIN_HIT = 14;
    wait_ = wait_ > dt ? wait_ - dt : 0;
    if (!sound_ || wait_) return;
    float v = max(hitBall_, hitWall_);
    if (v < MIN_HIT) return;
    soundEffect(hitBall_ >= hitWall_ ? SOUND_CLACK : SOUND_KNOCK, (uint8_t)constrain(40 + v * 2.2f, 0.0f, 255.0f));
    wait_ = 35;  // about one click long
  }

  struct Ball {
    float x, y, vx, vy;
    uint8_t hue;
  };
  static const int MAX = 24;
  Ball b_[MAX];
  int n_ = 0;
  float r_ = 3;
  Trail trail_;
  bool sound_ = true;
  float hitBall_ = 0, hitWall_ = 0;  // strongest impact speeds this frame
  uint32_t wait_ = 0;
};

// Bubbles rising against gravity through dark water; shaking fizzes.
class BubblesEffect : public Effect {
 public:
  void begin(int w, int h, JsonObjectConst p) override {
    Effect::begin(w, h, p);
    shakeSound_.set(p, SOUND_FIZZ, 220);
  }
  void render(GFXcanvas16 &c, uint32_t dt) override {
    SensorData s = sensorsGet();
    shakeSound_.update(shakeAmount(s), dt);
    const float fdt = min(dt, (uint32_t)50) / 1000.0f;
    t_ += dt;
    float m = sqrtf(s.gravX * s.gravX + s.gravY * s.gravY);
    float ux = m > 0.2f ? -s.gravX / m : 0, uy = m > 0.2f ? -s.gravY / m : -1;  // "up"

    spawnAcc_ += fdt * (4 + min(1.0f, shakeAmount(s)) * 60);
    while (spawnAcc_ >= 1) {
      spawnAcc_ -= 1;
      spawn(ux, uy);
    }

    // Water gets darker with depth.
    uint16_t *buf = c.getBuffer();
    const float cx = w_ / 2.0f, cy = h_ / 2.0f, span = (fabsf(ux) * w_ + fabsf(uy) * h_) / 2;
    for (int y = 0; y < h_; y++)
      for (int x = 0; x < w_; x++) {
        float up = ((x - cx) * ux + (y - cy) * uy) / span;  // -1 bottom .. 1 top
        uint8_t n = inoise8(x * 30, y * 30, t_ / 5);
        buf[y * w_ + x] = hsv565(150 + (n >> 5), 255, (uint8_t)constrain(30 + up * 22 + (n >> 4), 4, 90));
      }

    for (int i = 0; i < MAX; i++) {
      Bubble &b = b_[i];
      if (!b.on) continue;
      float speed = 6 + b.r * 7;
      float wob = sinf(b.phase + t_ / 260.0f) * 4;
      b.x += (ux * speed - uy * wob) * fdt;
      b.y += (uy * speed + ux * wob) * fdt;
      if (b.x < -4 || b.y < -4 || b.x > w_ + 4 || b.y > h_ + 4) {
        b.on = false;
        continue;
      }
      int x = (int)lroundf(b.x), y = (int)lroundf(b.y);
      if (b.r < 1) {
        addPixel(c, x, y, rgb565(120, 200, 255));
      } else {
        c.drawCircle(x, y, (int)b.r, rgb565(110, 200, 255));
        if (b.r >= 2) addPixel(c, x - 1, y - 1, rgb565(200, 240, 255));  // glint
      }
    }
  }

 private:
  void spawn(float ux, float uy) {
    for (int i = 0; i < MAX; i++) {
      Bubble &b = b_[i];
      if (b.on) continue;
      // Along the bottom edge (relative to gravity).
      float along = frand(-0.5f, 0.5f);
      b.x = w_ / 2.0f - ux * (w_ / 2.0f + 2) + (-uy) * along * w_;
      b.y = h_ / 2.0f - uy * (h_ / 2.0f + 2) + ux * along * h_;
      b.r = rndInt(0, 4) == 0 ? 2 : rndInt(0, 2);
      b.phase = frand(0, 6.28f);
      b.on = true;
      return;
    }
  }
  struct Bubble {
    float x, y, r, phase;
    bool on;
  };
  static const int MAX = 48;
  Bubble b_[MAX] = {};
  float spawnAcc_ = 0;
  uint32_t t_ = 0;
  ShakeSound shakeSound_;
};

// ---------------------------------------------------------------------------
Effect *createLiveEffect(const char *name) {
  sRng ^= esp_random();
  if (!sRng) sRng = 1;
  if (!strcmp(name, "spectrum")) return new SpectrumEffect();
  if (!strcmp(name, "radial")) return new RadialEffect();
  if (!strcmp(name, "waterfall")) return new WaterfallEffect();
  if (!strcmp(name, "sand")) return new GrainEffect(false);
  if (!strcmp(name, "liquid")) return new GrainEffect(true);
  if (!strcmp(name, "flow")) return new FlowEffect();
  if (!strcmp(name, "marbles")) return new MarblesEffect();
  if (!strcmp(name, "bubbles")) return new BubblesEffect();
  return nullptr;
}
