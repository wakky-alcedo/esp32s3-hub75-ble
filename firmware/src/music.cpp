#include "music.h"
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourceFS.h>
#include <AudioFileSourceID3.h>
#include <AudioGeneratorMP3a.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutput.h>
#include "fsutil.h"

static const int OUT_RATE = 48000;
static const uint32_t READ_BUFFER = 16384;  // rides out slow SD card reads

// Takes the decoder's samples, resampled to 48kHz, into the caller's buffer.
// Refuses samples once the buffer is (nearly) full; the decoder keeps them
// and offers them again on the next read.
class Collector : public AudioOutput {
 public:
  bool begin() override { return true; }
  bool stop() override { return true; }
  bool SetRate(int hz) override {
    hertz = hz;
    step_ = hz > 0 ? (float)hz / OUT_RATE : 1.0f;
    return true;
  }
  void target(int16_t *out, size_t n) {
    out_ = out;
    cap_ = n;
    got_ = 0;
  }
  size_t got() const { return got_; }
  // One input sample makes at most this many output frames (8kHz and up).
  static const size_t ROOM = 8;

  bool ConsumeSample(int16_t sample[2]) override {
    if (got_ + ROOM > cap_) return false;
    MakeSampleStereo16(sample);
    const float l = sample[0], r = sample[1];
    while (pos_ < 1.0f) {
      out_[got_ * 2] = (int16_t)(prevL_ + (l - prevL_) * pos_);
      out_[got_ * 2 + 1] = (int16_t)(prevR_ + (r - prevR_) * pos_);
      got_++;
      pos_ += step_;
    }
    pos_ -= 1.0f;
    prevL_ = l;
    prevR_ = r;
    return true;
  }

 private:
  int16_t *out_ = nullptr;
  size_t cap_ = 0, got_ = 0;
  float step_ = 1, pos_ = 0, prevL_ = 0, prevR_ = 0;
};

bool musicFileSupported(const char *path) {
  String p = path;
  p.toLowerCase();
  return p.endsWith(".mp3") || p.endsWith(".wav");
}

bool MusicDecoder::open(const char *path) {
  close();
  if (!musicFileSupported(path)) return false;
  String local;
  fs::FS *fs = fsFor(path, local);
  if (!fs) return false;
  file_ = new AudioFileSourceFS(*fs, local.c_str());
  if (!file_->isOpen()) {
    close();
    return false;
  }
  buf_ = new AudioFileSourceBuffer(file_, READ_BUFFER);
  String p = path;
  p.toLowerCase();
  AudioFileSource *src = buf_;
  if (p.endsWith(".mp3")) {
    id3_ = new AudioFileSourceID3(buf_);  // skips the tag (and any cover art)
    src = id3_;
    gen_ = new AudioGeneratorMP3a();
  } else {
    gen_ = new AudioGeneratorWAV();
  }
  out_ = new Collector();
  if (!gen_->begin(src, out_)) {
    close();
    return false;
  }
  return true;
}

void MusicDecoder::close() {
  if (gen_) gen_->stop();
  delete gen_;
  delete out_;
  delete id3_;
  delete buf_;
  delete file_;
  gen_ = nullptr;
  out_ = nullptr;
  id3_ = buf_ = nullptr;
  file_ = nullptr;
}

size_t MusicDecoder::read(int16_t *out, size_t n) {
  if (!gen_ || n < Collector::ROOM) return 0;
  out_->target(out, n);
  while (out_->got() + Collector::ROOM <= n) {
    if (!gen_->isRunning() || !gen_->loop()) break;
  }
  return out_->got();
}
