#pragma once
#include <Arduino.h>

// On-board speaker: ES8311 codec (I2C, shared with the RTC) fed over I2S,
// followed by an amplifier with its own enable pin. Sounds are synthesized
// tones played on a background task, so callers never block.

enum Sound : uint8_t {
  SOUND_NONE,
  SOUND_TICK,   // short blip, once per second near the end of a countdown
  SOUND_BEEP,   // one long beep
  SOUND_CHIME,  // two-tone "ping-pong"
  SOUND_ALARM,  // repeated beep-beep-beep, a few seconds long
};

// Call after rtcBegin(), which starts the shared I2C bus.
bool soundBegin();
bool soundPresent();
// Queues a sound; dropped when the codec is missing or the volume is 0.
void soundPlay(Sound s);
// "none" / "tick" / "beep" / "chime" / "alarm"; unknown names map to SOUND_NONE.
Sound soundFromName(const char *name);
