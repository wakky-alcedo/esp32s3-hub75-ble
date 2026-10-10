#pragma once
#include <Arduino.h>

// BOOT button (GPIO0, active low).
// GPIO0 is a strapping pin: holding it while powering on enters the ROM
// download mode, so every action here is triggered after boot.
enum ButtonEvent { BTN_NONE, BTN_SHORT, BTN_LONG, BTN_VERY_LONG, BTN_HOLD_10S };

class Button {
 public:
  void begin(uint8_t pin = 0);
  ButtonEvent poll();

 private:
  uint8_t pin_ = 0;
  bool down_ = false;
  bool firedVeryLong_ = false, firedHold_ = false;
  uint32_t downAt_ = 0, lastChange_ = 0;
};
