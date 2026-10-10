#include "button.h"

static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t LONG_MS = 800;
static const uint32_t VERY_LONG_MS = 3000;
static const uint32_t HOLD_MS = 10000;

void Button::begin(uint8_t pin) {
  pin_ = pin;
  pinMode(pin_, INPUT_PULLUP);
}

ButtonEvent Button::poll() {
  uint32_t now = millis();
  bool pressed = digitalRead(pin_) == LOW;
  if (pressed != down_ && now - lastChange_ >= DEBOUNCE_MS) {
    lastChange_ = now;
    down_ = pressed;
    if (pressed) {
      downAt_ = now;
      firedVeryLong_ = firedHold_ = false;
      return BTN_NONE;
    }
    // Released: short and long fire on release; the longer ones already fired.
    uint32_t held = now - downAt_;
    if (firedVeryLong_) return BTN_NONE;
    return held >= LONG_MS ? BTN_LONG : BTN_SHORT;
  }
  if (down_) {
    uint32_t held = now - downAt_;
    if (!firedVeryLong_ && held >= VERY_LONG_MS) {
      firedVeryLong_ = true;
      return BTN_VERY_LONG;
    }
    if (!firedHold_ && held >= HOLD_MS) {
      firedHold_ = true;
      return BTN_HOLD_10S;
    }
  }
  return BTN_NONE;
}
