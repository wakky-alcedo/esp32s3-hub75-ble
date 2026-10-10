#pragma once
#include <Adafruit_GFX.h>
#include <ArduinoJson.h>
#include <memory>
#include "assets.h"
#include "effects.h"

// What is drawn behind text, clock and countdown items.
class Background {
 public:
  bool load(JsonObjectConst def, int w, int h);
  void render(GFXcanvas16 &c, uint32_t dt);

 private:
  uint16_t color_ = 0;
  std::unique_ptr<Effect> effect_;
  Bitmap image_;
};

// One entry of a playlist.
class Item {
 public:
  virtual ~Item() = default;
  // Parses the item definition and loads its assets. Called once.
  virtual bool load(JsonObjectConst def, int w, int h);
  // dtMs: real elapsed time. motionMs: dtMs scaled by the speed setting.
  void render(GFXcanvas16 &c, uint32_t dtMs, uint32_t motionMs);
  bool done() const;

 protected:
  virtual void draw(GFXcanvas16 &c, uint32_t motionMs) = 0;
  // Items that end on their own (scroll passes, GIF loops) override these;
  // an explicit "duration" still takes precedence.
  virtual bool endsNaturally() const { return false; }
  virtual bool naturalEnd() const { return false; }
  virtual uint32_t defaultDurationMs() const { return 10000; }

  int w_ = 0, h_ = 0;
  uint32_t durationMs_ = 0;  // 0 = until naturalEnd()
  uint32_t elapsedMs_ = 0;
  Background bg_;
};

// Returns nullptr when the type is unknown or loading failed.
Item *createItem(JsonObjectConst def, int w, int h);
