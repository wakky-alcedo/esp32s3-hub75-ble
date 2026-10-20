#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// On-board microphones: ES7210 ADC on the speaker's I2S port (full duplex).
// While at least one user holds a source, a background task runs an FFT over
// its input and publishes a log-spaced spectrum. Besides the microphones the
// source can be the music being played (fed in by the sound task).

static const int MIC_BANDS = 64;

struct MicFrame {
  // 0..1 per band, 40Hz..16kHz on a log scale, auto-gained so the loudest
  // recent sound reaches about 1.
  float bands[MIC_BANDS];
  int8_t bandDb[MIC_BANDS];  // raw band level, dB (for tuning)
  float level = 0;   // overall loudness, 0..1 on the same scale
  float bass = 0;    // 0..1, below ~150Hz
  float dbfs = -120; // input level, dB relative to full scale
  float peakHz = 0;  // strongest frequency
  uint32_t beats = 0;  // increments on every detected beat (bass onset)
  uint32_t seq = 0;    // increments with every analysed block
};

enum MicSource : uint8_t { MIC_SRC_MIC, MIC_SRC_MUSIC, MIC_SOURCES };

// Call after soundBegin(), which installs the shared I2S driver.
bool micBegin();
bool micPresent();
// Re-applies gConfig.micGain.
void micApplyGain();
// Reference counted; the analysis runs only while someone holds it.
void micAcquire(MicSource src = MIC_SRC_MIC);
void micRelease(MicSource src = MIC_SRC_MIC);
MicFrame micGet(MicSource src = MIC_SRC_MIC);
// Decoded music, 48kHz interleaved stereo, before the volume is applied.
// Dropped unless someone holds MIC_SRC_MUSIC.
void micFeedMusic(const int16_t *stereo, size_t frames);
void micJson(JsonObject o, MicSource src = MIC_SRC_MIC);
// Replaces the input with synthetic music for ms, for testing the effects.
void micSimulate(uint32_t ms);
