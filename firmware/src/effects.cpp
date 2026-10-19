#include "effects.h"
#include <FastLED.h>
#include "color.h"

static CRGBPalette16 paletteByName(const char *name, const CRGBPalette16 &fallback) {
  if (!name) return fallback;
  if (!strcmp(name, "rainbow")) return RainbowColors_p;
  if (!strcmp(name, "heat")) return HeatColors_p;
  if (!strcmp(name, "lava")) return LavaColors_p;
  if (!strcmp(name, "cloud")) return CloudColors_p;
  if (!strcmp(name, "ocean")) return OceanColors_p;
  if (!strcmp(name, "forest")) return ForestColors_p;
  if (!strcmp(name, "party")) return PartyColors_p;
  return fallback;
}

static inline uint16_t crgb565(const CRGB &c) { return rgb565(c.r, c.g, c.b); }

// ---------------------------------------------------------------------------
// Plasma (ported from the 02_PatternPlasma sample)
class PlasmaEffect : public Effect {
 public:
  void begin(int w, int h, JsonObjectConst p) override {
    Effect::begin(w, h, p);
    palette_ = paletteByName(p["palette"], RainbowColors_p);
    cycle_ = !p["palette"].is<const char *>();
  }
  void render(GFXcanvas16 &c, uint32_t dt) override {
    acc_ += dt * 60 / 1000.0f;
    uint16_t t = (uint16_t)acc_;
    uint16_t *buf = c.getBuffer();
    uint8_t wibble = sin8(t);
    for (int y = 0; y < h_; y++) {
      for (int x = 0; x < w_; x++) {
        int32_t v = 128;
        v += sin16(x * wibble * 3 + t);
        v += cos16(y * (128 - wibble) + t);
        v += sin16(y * x * cos8(-t) / 8);
        buf[y * w_ + x] = crgb565(ColorFromPalette(palette_, (uint8_t)(v >> 8)));
      }
    }
    if (cycle_ && acc_ >= 1024) {
      static const TProgmemRGBPalette16 *const all[] = {&HeatColors_p, &LavaColors_p, &RainbowColors_p,
                                                        &RainbowStripeColors_p, &CloudColors_p};
      palette_ = *all[random(0, 5)];
    }
    if (acc_ >= 1024) acc_ -= 1024;
  }

 private:
  CRGBPalette16 palette_;
  bool cycle_ = true;
  float acc_ = 0;
};

// ---------------------------------------------------------------------------
// Fire: classic heat diffusion, sparks along the bottom edge.
class FireEffect : public Effect {
 public:
  ~FireEffect() override { free(heat_); }
  void begin(int w, int h, JsonObjectConst p) override {
    Effect::begin(w, h, p);
    cooling_ = p["cooling"] | 55;
    sparking_ = p["sparking"] | 120;
    heat_ = (uint8_t *)calloc(w * h, 1);
  }
  void render(GFXcanvas16 &c, uint32_t dt) override {
    acc_ += dt;
    while (acc_ >= 16) {
      acc_ -= 16;
      step();
    }
    uint16_t *buf = c.getBuffer();
    for (int y = 0; y < h_; y++)
      for (int x = 0; x < w_; x++) buf[y * w_ + x] = crgb565(HeatColor(heat_[y * w_ + x]));
  }

 private:
  void step() {
    const int coolMax = (cooling_ * 10) / h_ + 2;
    for (int i = 0; i < w_ * h_; i++) heat_[i] = qsub8(heat_[i], random8(0, coolMax));
    // Heat rises: each cell takes the average of the cells below it.
    for (int y = 0; y < h_ - 2; y++) {
      for (int x = 0; x < w_; x++) {
        int xl = (x + w_ - 1) % w_, xr = (x + 1) % w_;
        uint16_t sum = heat_[(y + 1) * w_ + x] * 2 + heat_[(y + 2) * w_ + xl] + heat_[(y + 2) * w_ + xr];
        heat_[y * w_ + x] = sum / 4;
      }
    }
    for (int x = 0; x < w_; x++) {
      if (random8() < sparking_ / 4) {
        int y = h_ - 1 - random8(2);
        heat_[y * w_ + x] = qadd8(heat_[y * w_ + x], random8(160, 255));
      }
    }
  }
  uint8_t *heat_ = nullptr;
  uint8_t cooling_ = 55, sparking_ = 120;
  uint32_t acc_ = 0;
};

// ---------------------------------------------------------------------------
// Matrix rain: falling heads leaving fading trails.
class MatrixEffect : public Effect {
 public:
  ~MatrixEffect() override {
    free(level_);
    free(drops_);
  }
  void begin(int w, int h, JsonObjectConst p) override {
    Effect::begin(w, h, p);
    color_ = parseColor(p["color"], rgb565(0, 255, 60));
    level_ = (uint8_t *)calloc(w * h, 1);
    drops_ = (Drop *)calloc(w, sizeof(Drop));
    for (int x = 0; x < w; x++) respawn(drops_[x], true);
  }
  void render(GFXcanvas16 &c, uint32_t dt) override {
    acc_ += dt;
    while (acc_ >= 30) {
      acc_ -= 30;
      step();
    }
    uint8_t r, g, b;
    unpack565(color_, r, g, b);
    uint16_t *buf = c.getBuffer();
    for (int i = 0; i < w_ * h_; i++) {
      uint8_t l = level_[i];
      buf[i] = l == 255 ? rgb565(200, 255, 200) : rgb565(scale8(r, l), scale8(g, l), scale8(b, l));
    }
  }

 private:
  struct Drop {
    float y, speed;
  };
  void respawn(Drop &d, bool anywhere) {
    d.y = anywhere ? -random(0, h_ * 2) : -random(0, h_);
    d.speed = random(30, 100) / 100.0f;
  }
  void step() {
    for (int i = 0; i < w_ * h_; i++) {
      if (level_[i]) level_[i] = level_[i] == 255 ? 200 : scale8(level_[i], 215);
    }
    for (int x = 0; x < w_; x++) {
      Drop &d = drops_[x];
      d.y += d.speed;
      int y = (int)d.y;
      if (y >= 0 && y < h_) level_[y * w_ + x] = 255;
      if (y >= h_ + 4) respawn(d, false);
    }
  }
  uint8_t *level_ = nullptr;
  Drop *drops_ = nullptr;
  uint16_t color_;
  uint32_t acc_ = 0;
};

// ---------------------------------------------------------------------------
// Conway's game of life on a torus, reseeded when it stagnates.
class LifeEffect : public Effect {
 public:
  ~LifeEffect() override {
    free(age_);
    free(next_);
  }
  void begin(int w, int h, JsonObjectConst p) override {
    Effect::begin(w, h, p);
    stepMs_ = p["stepMs"] | 100;
    age_ = (uint8_t *)calloc(w * h, 1);
    next_ = (uint8_t *)calloc(w * h, 1);
    seed();
  }
  void render(GFXcanvas16 &c, uint32_t dt) override {
    acc_ += dt;
    while (acc_ >= stepMs_) {
      acc_ -= stepMs_;
      step();
    }
    uint16_t *buf = c.getBuffer();
    for (int i = 0; i < w_ * h_; i++) {
      uint8_t a = age_[i];
      buf[i] = a ? hsv565(hueBase_ + min<int>(a, 64) * 2, 255, a == 1 ? 255 : 180) : 0;
    }
  }

 private:
  void seed() {
    for (int i = 0; i < w_ * h_; i++) age_[i] = random8() < 80 ? 1 : 0;
    gen_ = 0;
    stable_ = 0;
    hueBase_ = random8();
  }
  void step() {
    int alive = 0;
    for (int y = 0; y < h_; y++) {
      for (int x = 0; x < w_; x++) {
        int n = 0;
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            int xx = (x + dx + w_) % w_, yy = (y + dy + h_) % h_;
            n += age_[yy * w_ + xx] ? 1 : 0;
          }
        uint8_t a = age_[y * w_ + x];
        uint8_t na = (a && (n == 2 || n == 3)) ? min<int>(a + 1, 255) : (!a && n == 3) ? 1 : 0;
        next_[y * w_ + x] = na;
        alive += na ? 1 : 0;
      }
    }
    std::swap(age_, next_);
    // Still lifes and period-2 oscillators keep the population constant.
    if (alive == prevAlive_[1]) stable_++;
    else stable_ = 0;
    prevAlive_[1] = prevAlive_[0];
    prevAlive_[0] = alive;
    gen_++;
    if (stable_ > 20 || alive < w_ * h_ / 50 || gen_ > 1500) seed();
  }
  uint8_t *age_ = nullptr, *next_ = nullptr;
  uint32_t stepMs_ = 100, acc_ = 0;
  int prevAlive_[2] = {-1, -1};
  int stable_ = 0, gen_ = 0;
  uint8_t hueBase_ = 0;
};

// ---------------------------------------------------------------------------
// Starfield warp.
class StarsEffect : public Effect {
 public:
  ~StarsEffect() override { free(stars_); }
  void begin(int w, int h, JsonObjectConst p) override {
    Effect::begin(w, h, p);
    count_ = constrain((int)(p["count"] | (w * h / 24)), 8, 400);
    stars_ = (Star *)calloc(count_, sizeof(Star));
    for (int i = 0; i < count_; i++) spawn(stars_[i], random(10, 100) / 100.0f);
  }
  void render(GFXcanvas16 &c, uint32_t dt) override {
    c.fillScreen(0);
    const float cx = w_ / 2.0f, cy = h_ / 2.0f, k = max(w_, h_) * 0.5f;
    const float dz = dt / 2500.0f;
    for (int i = 0; i < count_; i++) {
      Star &s = stars_[i];
      s.z -= dz;
      if (s.z <= 0.02f) {
        spawn(s, 1.0f);
        continue;
      }
      int sx = (int)(cx + s.x / s.z * k), sy = (int)(cy + s.y / s.z * k);
      if (sx < 0 || sx >= w_ || sy < 0 || sy >= h_) {
        spawn(s, 1.0f);
        continue;
      }
      uint8_t v = (uint8_t)constrain((1.0f - s.z) * 255, 30, 255);
      c.drawPixel(sx, sy, hsv565(s.hue, s.hue ? 120 : 0, v));
    }
  }

 private:
  struct Star {
    float x, y, z;
    uint8_t hue;
  };
  void spawn(Star &s, float z) {
    s.x = random(-1000, 1000) / 1000.0f;
    s.y = random(-1000, 1000) / 1000.0f;
    s.z = z;
    s.hue = random8() < 200 ? 0 : random8();
  }
  Star *stars_ = nullptr;
  int count_ = 0;
};

// ---------------------------------------------------------------------------
// Perlin noise through a palette.
class NoiseEffect : public Effect {
 public:
  void begin(int w, int h, JsonObjectConst p) override {
    Effect::begin(w, h, p);
    palette_ = paletteByName(p["palette"], OceanColors_p);
    scale_ = p["scale"] | 30;
  }
  void render(GFXcanvas16 &c, uint32_t dt) override {
    z_ += dt / 2;
    uint16_t *buf = c.getBuffer();
    for (int y = 0; y < h_; y++)
      for (int x = 0; x < w_; x++) {
        uint8_t n = inoise8(x * scale_, y * scale_ + z_ / 4, z_);
        buf[y * w_ + x] = crgb565(ColorFromPalette(palette_, n + (z_ >> 6)));
      }
  }

 private:
  CRGBPalette16 palette_;
  uint16_t scale_ = 30;
  uint32_t z_ = 0;
};

// ---------------------------------------------------------------------------
// Diagonal rainbow sweep.
class RainbowEffect : public Effect {
 public:
  void begin(int w, int h, JsonObjectConst p) override {
    Effect::begin(w, h, p);
    density_ = p["density"] | 4;
  }
  void render(GFXcanvas16 &c, uint32_t dt) override {
    hue_ += dt * 0.08f;
    uint8_t base = (uint8_t)hue_;
    uint16_t *buf = c.getBuffer();
    for (int y = 0; y < h_; y++)
      for (int x = 0; x < w_; x++) buf[y * w_ + x] = hsv565(base + (x + y) * density_, 255, 255);
  }

 private:
  float hue_ = 0;
  uint8_t density_ = 4;
};

Effect *createEffect(const char *name) {
  if (!name) return nullptr;
  if (!strcmp(name, "plasma")) return new PlasmaEffect();
  if (!strcmp(name, "fire")) return new FireEffect();
  if (!strcmp(name, "matrix")) return new MatrixEffect();
  if (!strcmp(name, "life")) return new LifeEffect();
  if (!strcmp(name, "stars")) return new StarsEffect();
  if (!strcmp(name, "noise")) return new NoiseEffect();
  if (!strcmp(name, "rainbow")) return new RainbowEffect();
  return createLiveEffect(name);
}

const char *effectNames() {
  return "plasma,fire,matrix,life,stars,noise,rainbow,spectrum,radial,waterfall,sand,liquid,flow,marbles,bubbles";
}
