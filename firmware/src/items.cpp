#include "items.h"
#include <AnimatedGIF.h>
#include <new>
#include <sys/time.h>
#include "color.h"
#include "config.h"
#include "fsutil.h"
#include "mic.h"
#include "sensors.h"
#include "sound.h"
#include "usb_console.h"

// Asset names are files in /assets; "/sd/..." names a file on the TF card.
static String assetPath(const char *name) {
  if (!strncmp(name, "/sd/", 4)) return name;
  String p = "/assets/";
  p += name;
  return p;
}

static bool loadAssetBitmap(JsonVariantConst name, Bitmap &bmp) {
  if (!name.is<const char *>()) return false;
  return bmp.load(assetPath(name.as<const char *>()).c_str());
}

static void readTint(JsonObjectConst def, Tint &tint) {
  const char *color = def["color"] | "#ffffff";
  tint.rainbow = !strcmp(color, "rainbow");
  tint.color = parseColor(color, 0xFFFF);
}

// ---------------------------------------------------------------------------
bool Background::load(JsonObjectConst def, int w, int h) {
  if (def.isNull()) return true;
  const char *type = def["type"] | "color";
  if (!strcmp(type, "effect")) {
    effect_.reset(createEffect(def["effect"]));
    if (!effect_) return false;
    effect_->begin(w, h, def["params"]);
  } else if (!strcmp(type, "image")) {
    return loadAssetBitmap(def["asset"], image_);
  }
  color_ = parseColor(def["color"], 0);
  return true;
}

void Background::render(GFXcanvas16 &c, uint32_t dt) {
  if (effect_) {
    effect_->render(c, dt);
    return;
  }
  c.fillScreen(color_);
  if (image_.valid()) drawBitmap(c, image_, (c.width() - image_.w) / 2, (c.height() - image_.h) / 2);
}

// ---------------------------------------------------------------------------
bool Item::load(JsonObjectConst def, int w, int h) {
  w_ = w;
  h_ = h;
  durationMs_ = def["duration"] | 0;
  return bg_.load(def["bg"], w, h);
}

void Item::render(GFXcanvas16 &c, uint32_t dtMs, uint32_t motionMs) {
  elapsedMs_ += dtMs;
  bg_.render(c, motionMs);
  draw(c, motionMs);
}

bool Item::done() const {
  if (durationMs_) return elapsedMs_ >= durationMs_;
  if (endsNaturally()) return naturalEnd();
  return elapsedMs_ >= defaultDurationMs();
}

// ---------------------------------------------------------------------------
class TextItem : public Item {
 public:
  bool load(JsonObjectConst def, int w, int h) override {
    if (!Item::load(def, w, h)) return false;
    if (def["asset"].is<const char *>()) {
      if (!loadAssetBitmap(def["asset"], bmp_)) return false;
    } else {
      const char *text = def["text"] | "";
      uint8_t scale = def["scale"] | max(1, h / 8);
      if (!bmp_.fromText(text, scale)) return false;
    }
    readTint(def, tint_);
    const char *mode = def["mode"] | "scroll";
    scroll_ = !strcmp(mode, "scroll");
    blink_ = !strcmp(mode, "blink");
    const char *dir = def["dir"] | "left";
    dir_ = dir[0];
    speed_ = def["speed"] | 30;
    repeat_ = max(1, (int)(def["repeat"] | 1));
    blinkMs_ = max(50, (int)(def["blinkMs"] | 500));
    yOffset_ = def["y"] | INT16_MIN;
    return true;
  }

 protected:
  bool endsNaturally() const override { return scroll_; }
  bool naturalEnd() const override { return passes_ >= repeat_; }
  uint32_t defaultDurationMs() const override { return 5000; }

  void draw(GFXcanvas16 &c, uint32_t motion) override {
    phaseMs_ += motion;
    if (tint_.rainbow) tint_.hueOffset = phaseMs_ / 10;
    int cx = (w_ - bmp_.w) / 2;
    int cy = yOffset_ != INT16_MIN ? yOffset_ : (h_ - bmp_.h) / 2;
    if (!scroll_) {
      if (blink_ && (phaseMs_ / blinkMs_) % 2) return;
      drawBitmap(c, bmp_, cx, cy, tint_);
      return;
    }
    bool vertical = dir_ == 'u' || dir_ == 'd';
    float span = vertical ? h_ + bmp_.h : w_ + bmp_.w;
    pos_ += speed_ * motion / 1000.0f;
    while (pos_ >= span) {
      pos_ -= span;
      passes_++;
    }
    // Hold the final frame off-screen once all passes are done.
    if (passes_ >= repeat_) return;
    int p = (int)pos_;
    switch (dir_) {
      case 'r': drawBitmap(c, bmp_, -bmp_.w + p, cy, tint_); break;
      case 'u': drawBitmap(c, bmp_, cx, h_ - p, tint_); break;
      case 'd': drawBitmap(c, bmp_, cx, -bmp_.h + p, tint_); break;
      default: drawBitmap(c, bmp_, w_ - p, cy, tint_); break;
    }
  }

 private:
  Bitmap bmp_;
  Tint tint_;
  bool scroll_ = true, blink_ = false;
  char dir_ = 'l';
  uint16_t speed_ = 30;
  int repeat_ = 1, passes_ = 0;
  uint32_t blinkMs_ = 500, phaseMs_ = 0;
  int16_t yOffset_ = INT16_MIN;
  float pos_ = 0;
};

// ---------------------------------------------------------------------------
class ImageItem : public Item {
 public:
  bool load(JsonObjectConst def, int w, int h) override {
    if (!Item::load(def, w, h)) return false;
    return loadAssetBitmap(def["asset"], bmp_);
  }

 protected:
  uint32_t defaultDurationMs() const override { return 5000; }
  void draw(GFXcanvas16 &c, uint32_t) override { drawBitmap(c, bmp_, (w_ - bmp_.w) / 2, (h_ - bmp_.h) / 2); }

 private:
  Bitmap bmp_;
};

// ---------------------------------------------------------------------------
class GifItem : public Item {
 public:
  ~GifItem() override {
    if (gif_) {
      gif_->close();
      gif_->~AnimatedGIF();
      free(gif_);
    }
    free(file_);
    free(frame_);
  }

  bool load(JsonObjectConst def, int w, int h) override {
    if (!Item::load(def, w, h)) return false;
    if (!def["asset"].is<const char *>()) return false;
    file_ = fsReadAlloc(assetPath(def["asset"]).c_str(), fileLen_);
    if (!file_) return false;
    // AnimatedGIF carries a large decode state; keep it in PSRAM.
    void *mem = heap_caps_malloc(sizeof(AnimatedGIF), MALLOC_CAP_SPIRAM);
    if (!mem) return false;
    gif_ = new (mem) AnimatedGIF();
    gif_->begin(GIF_PALETTE_RGB565_LE);
    if (!gif_->open(file_, fileLen_, onDraw)) return false;
    gw_ = gif_->getCanvasWidth();
    gh_ = gif_->getCanvasHeight();
    frame_ = (uint16_t *)ps_calloc((size_t)gw_ * gh_, 2);
    if (!frame_) return false;
    loops_ = max(1, (int)(def["loops"] | 1));
    return true;
  }

 protected:
  bool endsNaturally() const override { return true; }
  // A single-frame GIF "finishes" instantly; keep it up for a moment anyway.
  bool naturalEnd() const override { return played_ >= loops_ && elapsedMs_ >= 2000; }

  void draw(GFXcanvas16 &c, uint32_t motion) override {
    waitMs_ -= (int32_t)motion;
    if (waitMs_ <= 0) {
      int delayMs = 0;
      int rc = gif_->playFrame(false, &delayMs, this);
      if (rc <= 0) {
        played_++;
        gif_->reset();
      }
      waitMs_ = delayMs > 0 ? delayMs : 100;
    }
    int ox = (w_ - gw_) / 2, oy = (h_ - gh_) / 2;
    uint16_t *dst = c.getBuffer();
    for (int y = max(0, oy); y < min<int>(h_, oy + gh_); y++)
      for (int x = max(0, ox); x < min<int>(w_, ox + gw_); x++) dst[y * w_ + x] = frame_[(y - oy) * gw_ + (x - ox)];
  }

 private:
  static void onDraw(GIFDRAW *d) {
    GifItem *self = (GifItem *)d->pUser;
    int y = d->iY + d->y;
    if (y < 0 || y >= self->gh_) return;
    uint8_t *src = d->pPixels;
    uint16_t *pal = (uint16_t *)d->pPalette;
    uint16_t *row = self->frame_ + y * self->gw_;
    for (int x = 0; x < d->iWidth; x++) {
      int fx = d->iX + x;
      if (fx < 0 || fx >= self->gw_) continue;
      uint8_t idx = src[x];
      if (d->ucHasTransparency && idx == d->ucTransparent) {
        // Disposal 2 restores the background; otherwise keep the previous frame.
        if (d->ucDisposalMethod == 2) row[fx] = 0;
        continue;
      }
      row[fx] = pal[idx];
    }
  }

  AnimatedGIF *gif_ = nullptr;
  uint8_t *file_ = nullptr;
  size_t fileLen_ = 0;
  uint16_t *frame_ = nullptr;
  int gw_ = 0, gh_ = 0;
  int loops_ = 1, played_ = 0;
  int32_t waitMs_ = 0;
};

// ---------------------------------------------------------------------------
// Shared layout for clock and countdown: optional label on top, digits below.
class DigitsItem : public Item {
 public:
  bool load(JsonObjectConst def, int w, int h) override {
    if (!Item::load(def, w, h)) return false;
    if (def["label"].is<const char *>()) loadAssetBitmap(def["label"], label_);
    else if (def["labelText"].is<const char *>()) label_.fromText(def["labelText"], 1);
    readTint(def, tint_);
    labelTint_.color = parseColor(def["labelColor"], tint_.color);
    labelTint_.rainbow = tint_.rainbow;
    return true;
  }

 protected:
  virtual void text(char *out, size_t n, bool &dim) = 0;

  void draw(GFXcanvas16 &c, uint32_t motion) override {
    phaseMs_ += motion;
    int top = 0;
    if (label_.valid() && label_.h + 8 <= h_) {
      if (label_.w <= w_) {
        drawBitmap(c, label_, (w_ - label_.w) / 2, 0, labelTint_);
      } else {
        int span = w_ + label_.w;
        int p = (phaseMs_ * 30 / 1000) % span;
        drawBitmap(c, label_, w_ - p, 0, labelTint_);
      }
      top = label_.h;
    }
    char buf[16];
    bool dim = false;
    text(buf, sizeof(buf), dim);
    int areaH = h_ - top;
    int w1 = builtinTextWidth(buf, 1);
    int scale = max(1, min(w_ / max(1, w1), areaH / 8));
    int tw = builtinTextWidth(buf, scale), th = 7 * scale;
    uint16_t col = tint_.rainbow ? hsv565(phaseMs_ / 20, 255, 255) : tint_.color;
    if (dim) col = scale565(col, 64);
    drawBuiltinText(c, buf, (w_ - tw) / 2, top + (areaH - th) / 2, scale, col);
  }

  uint32_t phaseMs_ = 0;
  Bitmap label_;
  Tint tint_, labelTint_;
};

static bool timeValid() { return time(nullptr) > 1700000000; }

class ClockItem : public DigitsItem {
 public:
  bool load(JsonObjectConst def, int w, int h) override {
    seconds_ = !strcmp(def["format"] | "HM", "HMS");
    return DigitsItem::load(def, w, h);
  }

 protected:
  void text(char *out, size_t n, bool &dim) override {
    if (!timeValid()) {
      snprintf(out, n, seconds_ ? "--:--:--" : "--:--");
      dim = true;
      return;
    }
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    time_t local = tv.tv_sec + gConfig.tzOffsetMin * 60;
    struct tm t;
    gmtime_r(&local, &t);
    bool colon = seconds_ || tv.tv_usec < 500000;
    if (seconds_) snprintf(out, n, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    else snprintf(out, n, "%02d%c%02d", t.tm_hour, colon ? ':' : ' ', t.tm_min);
  }

 private:
  bool seconds_ = false;
};

class CountdownItem : public DigitsItem {
 public:
  bool load(JsonObjectConst def, int w, int h) override {
    stopwatch_ = !strcmp(def["mode"] | "countdown", "stopwatch");
    target_ = def["target"] | 0;
    timerMs_ = (uint32_t)(def["seconds"] | 0) * 1000;
    endSound_ = soundFromName(def["endSound"]);
    tickFrom_ = def["tickFrom"] | 0;
    return DigitsItem::load(def, w, h);
  }

 protected:
  // A timer without "duration" stays up until it has run out, plus a moment
  // at 00:00 for the end sound.
  bool endsNaturally() const override { return !stopwatch_ && !target_; }
  bool naturalEnd() const override { return elapsedMs_ >= timerMs_ + 5000; }

  void text(char *out, size_t n, bool &dim) override {
    int32_t secs;
    if (stopwatch_) {
      secs = elapsedMs_ / 1000;
    } else if (target_) {
      if (!timeValid()) {
        snprintf(out, n, "--:--");
        dim = true;
        return;
      }
      secs = (int32_t)(target_ - (int64_t)time(nullptr));
    } else {
      secs = ((int64_t)timerMs_ - elapsedMs_ + 999) / 1000;
    }
    if (!stopwatch_) sounds(secs);
    if (secs <= 0) {
      secs = 0;
      dim = (phaseMs_ / 500) % 2;  // blink at zero
    }
    int hh = secs / 3600, mm = (secs / 60) % 60, ss = secs % 60;
    if (hh > 0) snprintf(out, n, "%d:%02d:%02d", hh, mm, ss);
    else snprintf(out, n, "%02d:%02d", mm, ss);
  }

 private:
  // Plays only on transitions seen while this item is on screen, so showing
  // a countdown that already ended stays quiet.
  void sounds(int32_t secs) {
    if (lastSecs_ != INT32_MIN && secs != lastSecs_) {
      if (secs <= 0 && lastSecs_ > 0) soundPlay(endSound_);
      else if (secs > 0 && secs <= tickFrom_) soundPlay(SOUND_TICK);
    }
    lastSecs_ = secs;
  }

  bool stopwatch_ = false;
  int64_t target_ = 0;
  uint32_t timerMs_ = 0;
  Sound endSound_ = SOUND_NONE;
  int32_t tickFrom_ = 0;
  int32_t lastSecs_ = INT32_MIN;
};

// ---------------------------------------------------------------------------
class EffectItem : public Item {
 public:
  bool load(JsonObjectConst def, int w, int h) override {
    if (!Item::load(def, w, h)) return false;
    effect_.reset(createEffect(def["effect"]));
    if (!effect_) return false;
    effect_->begin(w, h, def["params"]);
    return true;
  }

 protected:
  void draw(GFXcanvas16 &c, uint32_t motion) override { effect_->render(c, motion); }

 private:
  std::unique_ptr<Effect> effect_;
};

// ---------------------------------------------------------------------------
// Live sensor readings as text, a few lines per page, pages cycling.
class SensorsItem : public Item {
 public:
  ~SensorsItem() override {
    if (usesMic_) micRelease();
  }
  bool load(JsonObjectConst def, int w, int h) override {
    if (!Item::load(def, w, h)) return false;
    JsonArrayConst pages = def["pages"];
    if (pages.isNull()) {
      for (Page p : {PAGE_ENV, PAGE_ACCEL, PAGE_GYRO, PAGE_SOUND}) pages_[count_++] = p;
    } else {
      for (JsonVariantConst v : pages) {
        Page p = pageFromName(v | "");
        if (p != PAGE_NONE && count_ < MAX_PAGES) pages_[count_++] = p;
      }
    }
    if (!count_) return false;
    for (int i = 0; i < count_; i++) usesMic_ |= pages_[i] == PAGE_SOUND;
    if (usesMic_) micAcquire();
    pageMs_ = max(1000, (int)(def["pageMs"] | 3000));
    color_ = parseColor(def["color"], 0xFFFF);
    headColor_ = parseColor(def["headColor"], rgb565(255, 200, 40));
    return true;
  }

 protected:
  uint32_t defaultDurationMs() const override { return pageMs_ * count_; }

  void draw(GFXcanvas16 &c, uint32_t) override {
    // Pages that fit at once (8px per line, header + 3 values each).
    int perScreen = max(1, h_ / 32);
    int first = (elapsedMs_ / pageMs_ * perScreen) % count_;
    for (int k = 0; k < perScreen && k < count_; k++) drawPage(c, pages_[(first + k) % count_], k * 32);
  }

 private:
  enum Page : uint8_t { PAGE_NONE, PAGE_ENV, PAGE_ACCEL, PAGE_GYRO, PAGE_SOUND };
  static const int MAX_PAGES = 8;

  static Page pageFromName(const char *n) {
    if (!strcmp(n, "env")) return PAGE_ENV;
    if (!strcmp(n, "accel")) return PAGE_ACCEL;
    if (!strcmp(n, "gyro")) return PAGE_GYRO;
    if (!strcmp(n, "sound")) return PAGE_SOUND;
    return PAGE_NONE;
  }

  void line(GFXcanvas16 &c, int y, const char *text, uint16_t col) {
    drawBuiltinText(c, text, max(0, (w_ - builtinTextWidth(text, 1)) / 2), y, 1, col);
  }

  void drawPage(GFXcanvas16 &c, Page page, int top) {
    SensorData d = sensorsGet();
    char l[3][16];
    const char *head = "";
    switch (page) {
      case PAGE_ENV:
        head = "TEMP/HUMID";
        if (d.envOk) {
          snprintf(l[0], 16, "%.1fC", d.tempC);
          snprintf(l[1], 16, "%.1f%%", d.humidity);
        } else {
          strcpy(l[0], "--.-C");
          strcpy(l[1], "--.-%");
        }
        snprintf(l[2], 16, "CPU %.0fC", d.chipC);
        break;
      case PAGE_ACCEL:
        head = "ACCEL g";
        snprintf(l[0], 16, "X%+6.2f", d.ax);
        snprintf(l[1], 16, "Y%+6.2f", d.ay);
        snprintf(l[2], 16, "Z%+6.2f", d.az);
        break;
      case PAGE_GYRO:
        head = "GYRO dps";
        snprintf(l[0], 16, "X%+5d", (int)lroundf(d.gx));
        snprintf(l[1], 16, "Y%+5d", (int)lroundf(d.gy));
        snprintf(l[2], 16, "Z%+5d", (int)lroundf(d.gz));
        break;
      case PAGE_SOUND: {
        head = "SOUND";
        MicFrame m = micGet();
        snprintf(l[0], 16, "%.0fdBFS", m.dbfs);
        snprintf(l[1], 16, "%.0fHz", m.peakHz);
        l[2][0] = 0;
        // Level meter instead of a third line.
        int bw = (int)(m.level * 2.5f * (w_ - 8));
        bw = constrain(bw, 0, w_ - 8);
        for (int x = 0; x < bw; x++) c.drawFastVLine(4 + x, top + 25, 5, hsv565(96 - x * 96 / (w_ - 8), 255, 255));
        break;
      }
      default:
        return;
    }
    bool imuPage = page == PAGE_ACCEL || page == PAGE_GYRO;
    line(c, top, head, headColor_);
    for (int i = 0; i < 3; i++) {
      if (!l[i][0]) continue;
      line(c, top + 8 + i * 8, imuPage && !d.imuOk ? "--" : l[i], color_);
    }
  }

  Page pages_[MAX_PAGES] = {};
  int count_ = 0;
  bool usesMic_ = false;
  uint32_t pageMs_ = 3000;
  uint16_t color_ = 0xFFFF, headColor_ = 0;
};

// ---------------------------------------------------------------------------
Item *createItem(JsonObjectConst def, int w, int h) {
  const char *type = def["type"] | "";
  Item *item = nullptr;
  if (!strcmp(type, "text")) item = new TextItem();
  else if (!strcmp(type, "image")) item = new ImageItem();
  else if (!strcmp(type, "gif")) item = new GifItem();
  else if (!strcmp(type, "clock")) item = new ClockItem();
  else if (!strcmp(type, "countdown")) item = new CountdownItem();
  else if (!strcmp(type, "effect")) item = new EffectItem();
  else if (!strcmp(type, "sensors")) item = new SensorsItem();
  if (!item) {
    Console.printf("[items] unknown type '%s'\n", type);
    return nullptr;
  }
  if (!item->load(def, w, h)) {
    Console.printf("[items] failed to load '%s'\n", type);
    delete item;
    return nullptr;
  }
  return item;
}
