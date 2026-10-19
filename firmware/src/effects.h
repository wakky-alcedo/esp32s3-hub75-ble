#pragma once
#include <Adafruit_GFX.h>
#include <ArduinoJson.h>

// Procedural animations. render() fills the whole canvas; dtMs is already
// scaled by the global speed setting.
class Effect {
 public:
  virtual ~Effect() = default;
  virtual void begin(int w, int h, JsonObjectConst params) {
    w_ = w;
    h_ = h;
  }
  virtual void render(GFXcanvas16 &c, uint32_t dtMs) = 0;

 protected:
  int w_ = 0, h_ = 0;
};

// Returns nullptr for an unknown name.
Effect *createEffect(const char *name);
// Microphone and motion sensor effects (effects_live.cpp).
Effect *createLiveEffect(const char *name);
// Comma separated list of effect names, for GET_INFO.
const char *effectNames();
