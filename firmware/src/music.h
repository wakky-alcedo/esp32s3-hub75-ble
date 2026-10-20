#pragma once
#include <Arduino.h>

// Decodes an MP3 or WAV file (LittleFS, or the SD card for "/sd/...") into
// 48kHz stereo, the rate the speaker and microphones share. Other sample
// rates are resampled linearly. Used by the sound task (sound.cpp).
class MusicDecoder {
 public:
  ~MusicDecoder() { close(); }
  bool open(const char *path);
  void close();
  bool isOpen() const { return gen_ != nullptr; }
  // Writes up to n interleaved stereo frames to out; returns 0 at the end.
  size_t read(int16_t *out, size_t n);

 private:
  class AudioFileSourceFS *file_ = nullptr;
  class AudioFileSource *buf_ = nullptr, *id3_ = nullptr;
  class AudioGenerator *gen_ = nullptr;
  class Collector *out_ = nullptr;
};

// True for the file types MusicDecoder plays (.mp3, .wav).
bool musicFileSupported(const char *path);
