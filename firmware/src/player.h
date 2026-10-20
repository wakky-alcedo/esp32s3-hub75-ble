#pragma once
#include <ArduinoJson.h>
#include <memory>
#include "items.h"

static const int MAX_PRESETS = 8;

// Plays the items of the selected preset in order, looping forever.
// Not thread safe: callers hold StateLock.
class Player {
 public:
  void begin();
  void render(GFXcanvas16 &c, uint32_t dtMs);

  bool selectPreset(int index, bool persist = true);
  void nextPreset();
  int currentPreset() const { return preset_; }
  void next();
  void prev();
  void setPaused(bool p) { paused_ = p; }
  bool paused() const { return paused_; }
  // Re-reads the preset file if it is the one playing.
  void presetChanged(int index);

  // Display standby; also stops music, and restarts the preset's BGM after.
  void setStandby(bool on);
  void toast(const String &text, uint32_t ms = 1200);
  void setPinOverlay(const String &pin) { pin_ = pin; }

  void statusJson(JsonObject o) const;

 private:
  void startItem(int index);
  void startBgm();
  void drawOverlay(GFXcanvas16 &c, const String &text);

  enum Phase { PLAYING, FADING_OUT, FADING_IN };

  JsonDocument doc_;
  JsonArrayConst items_;
  std::unique_ptr<Item> item_;
  int preset_ = -1;
  int index_ = 0;
  int pendingIndex_ = 0;
  bool fade_ = true;
  bool bgmOwned_ = false;  // the music playing is this preset's bgm
  bool paused_ = false;
  bool demo_ = false;
  Phase phase_ = PLAYING;
  uint32_t phaseMs_ = 0;
  String toast_;
  uint32_t toastMs_ = 0;
  String pin_;
};

// Recursive mutex around Player/Display state shared by the render loop,
// the serial console and BLE callbacks.
class StateLock {
 public:
  StateLock();
  ~StateLock();
  static void init();
};

extern Player gPlayer;
