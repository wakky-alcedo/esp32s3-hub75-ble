#include "transfer.h"
#include <LittleFS.h>
#include <SD_MMC.h>
#include "fsutil.h"

FileReceiver gReceiver;

// Standard CRC-32 (IEEE 802.3, same as zlib). Pass 0 to start.
uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len) {
  crc = ~crc;
  while (len--) {
    crc ^= *data++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

static bool allowedTarget(const String &p) {
  return fsValidPath(p.c_str()) && (p.startsWith("/assets/") || p.startsWith("/presets/") || p.startsWith("/sd/")) &&
         !p.endsWith(".tmp") && !p.endsWith("/");
}

// Creates the missing parent directories of local.
static void ensureParents(fs::FS &fs, const String &local) {
  for (int i = local.indexOf('/', 1); i > 0; i = local.indexOf('/', i + 1)) {
    String dir = local.substring(0, i);
    if (!fs.exists(dir)) fs.mkdir(dir);
  }
}

bool FileReceiver::begin(const char *path, uint32_t size, uint32_t crc, String &err) {
  abort();
  path_ = path;
  if (!allowedTarget(path_)) {
    err = "bad path";
    return false;
  }
  fs_ = fsFor(path_.c_str(), local_);
  if (!fs_) {
    err = "no sd card";
    return false;
  }
  uint64_t free = fs_ == &LittleFS ? LittleFS.totalBytes() - LittleFS.usedBytes()
                                   : SD_MMC.totalBytes() - SD_MMC.usedBytes();
  if (size > free) {
    err = "no space";
    return false;
  }
  ensureParents(*fs_, local_);
  file_ = fs_->open(local_ + ".tmp", "w");
  if (!file_) {
    err = "open failed";
    return false;
  }
  size_ = size;
  crc_ = crc;
  received_ = 0;
  running_ = 0;
  active_ = true;
  return true;
}

bool FileReceiver::write(uint32_t offset, const uint8_t *data, size_t len) {
  if (!active_ || offset != received_ || received_ + len > size_) return false;
  if (file_.write(data, len) != len) return false;
  running_ = crc32Update(running_, data, len);
  received_ += len;
  return true;
}

bool FileReceiver::end(String &err) {
  if (!active_) {
    err = "no transfer";
    return false;
  }
  file_.close();
  active_ = false;
  if (received_ != size_) {
    err = "size mismatch";
    fs_->remove(local_ + ".tmp");
    return false;
  }
  if (running_ != crc_) {
    err = "crc mismatch";
    fs_->remove(local_ + ".tmp");
    return false;
  }
  if (!fsCommitTmp(path_.c_str())) {
    err = "commit failed";
    return false;
  }
  return true;
}

void FileReceiver::abort() {
  if (!active_) return;
  file_.close();
  fs_->remove(local_ + ".tmp");
  active_ = false;
}
