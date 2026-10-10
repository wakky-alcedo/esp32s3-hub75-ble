#pragma once
#include <Arduino.h>
#include <FS.h>

// Receives one file in sequential chunks, verifies size and CRC32, then
// atomically moves it into place.
class FileReceiver {
 public:
  bool begin(const char *path, uint32_t size, uint32_t crc, String &err);
  // Returns false when offset is not the next expected byte; the sender
  // resumes from expected().
  bool write(uint32_t offset, const uint8_t *data, size_t len);
  bool end(String &err);
  void abort();
  bool active() const { return active_; }
  uint32_t expected() const { return received_; }
  const String &path() const { return path_; }

 private:
  File file_;
  String path_;
  uint32_t size_ = 0, crc_ = 0, received_ = 0, running_ = 0;
  bool active_ = false;
};

uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len);

extern FileReceiver gReceiver;
