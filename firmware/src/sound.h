#pragma once
#include <Arduino.h>

// On-board speaker: ES8311 codec (I2C, shared with the RTC) fed over I2S,
// followed by an amplifier with its own enable pin. Sounds are synthesized
// on a background task, so callers never block.

enum Sound : uint8_t {
  SOUND_NONE,
  SOUND_TICK,   // short blip, once per second near the end of a countdown
  SOUND_BEEP,   // one long beep
  SOUND_CHIME,  // two-tone "ping-pong"
  SOUND_ALARM,  // repeated beep-beep-beep, a few seconds long
  // Effect sounds (see soundEffect()).
  SOUND_CLACK,   // glassy click: marble against marble
  SOUND_KNOCK,   // duller knock: marble against a wall
  SOUND_RATTLE,  // burst of grains: sand shaken
  SOUND_SPLASH,  // slosh with a few drops: water shaken
  SOUND_WHOOSH,  // airy swell: flowing particles stirred
  SOUND_FIZZ,    // crackle: bubbles shaken
};

// Call after rtcBegin(), which starts the shared I2C bus.
bool soundBegin();
bool soundPresent();
// Queues a sound; dropped when the codec is missing or the volume is 0.
void soundPlay(Sound s);
// For sounds an effect fires often: `level` 0-255 scales the loudness, and
// the sound is dropped while another one is still waiting, so a burst of
// them never builds up a backlog.
void soundEffect(Sound s, uint8_t level);
// "none" / "tick" / "beep" / "chime" / "alarm" / "clack" / ...; unknown names map to SOUND_NONE.
Sound soundFromName(const char *name);

// Music from an MP3 or WAV file on LittleFS or the SD card ("/sd/..."),
// played to the end, or over and over with `loop`. Replaces whatever music
// was playing. Tones (ticks, alarms) pause it while they sound; effect sounds
// are muted during music. False when the file is missing or not playable.
bool musicPlay(const char *path, bool loop);
void musicStop();
bool musicPlaying();
String musicNowPlaying();  // "" when nothing plays
