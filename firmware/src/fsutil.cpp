#include "fsutil.h"
#include <LittleFS.h>
#include <SD_MMC.h>
#include <sys/stat.h>
#include "sdcard.h"

static void recoverTmp(const char *dir) {
  File d = LittleFS.open(dir);
  if (!d || !d.isDirectory()) return;
  String base = String(dir);
  if (!base.endsWith("/")) base += "/";
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String name = f.name();
    bool isDir = f.isDirectory();
    f.close();
    if (isDir) {
      recoverTmp((base + name).c_str());
      continue;
    }
    if (!name.endsWith(".tmp")) continue;
    String tmp = base + name;
    String target = tmp.substring(0, tmp.length() - 4);
    // The target existing means the rename never started; the tmp is a partial write.
    if (fsExists(target.c_str())) {
      LittleFS.remove(tmp);
    } else {
      LittleFS.rename(tmp, target);
    }
  }
}

bool fsBegin() {
  if (!LittleFS.begin(true)) return false;
  recoverTmp("/");
  return true;
}

static bool isSd(const char *path) { return !strncmp(path, "/sd/", 4) || !strcmp(path, "/sd"); }

fs::FS *fsFor(const char *path, String &local) {
  if (!isSd(path)) {
    local = path;
    return &LittleFS;
  }
  local = path[3] ? path + 3 : "/";
  return sdEnsure() ? &SD_MMC : nullptr;
}

bool fsExists(const char *path) {
  struct stat st;
  if (isSd(path)) {
    if (!sdEnsure()) return false;
    return stat((String("/sdcard") + (path[3] ? path + 3 : "/")).c_str(), &st) == 0;
  }
  return stat((String("/littlefs") + path).c_str(), &st) == 0;
}

bool fsValidPath(const char *path) {
  if (!path || path[0] != '/') return false;
  if (strstr(path, "..")) return false;
  if (strlen(path) > 96) return false;
  return true;
}

void fsEnsureDir(const char *dir) {
  String local;
  fs::FS *fs = fsFor(dir, local);
  if (fs && !fsExists(dir)) fs->mkdir(local);
}

bool fsCommitTmp(const char *path) {
  String local;
  fs::FS *fs = fsFor(path, local);
  if (!fs) return false;
  String tmp = local + ".tmp";
  if (fs->rename(tmp, local)) return true;
  fs->remove(local);
  return fs->rename(tmp, local);
}

bool fsWriteAtomic(const char *path, const uint8_t *data, size_t len) {
  String local;
  fs::FS *fs = fsFor(path, local);
  if (!fs) return false;
  String tmp = local + ".tmp";
  File f = fs->open(tmp, "w");
  if (!f) return false;
  size_t written = f.write(data, len);
  f.close();
  if (written != len) {
    fs->remove(tmp);
    return false;
  }
  return fsCommitTmp(path);
}

bool fsWriteAtomic(const char *path, const String &text) {
  return fsWriteAtomic(path, (const uint8_t *)text.c_str(), text.length());
}

uint8_t *fsReadAlloc(const char *path, size_t &len) {
  if (!fsExists(path)) return nullptr;
  String local;
  fs::FS *fs = fsFor(path, local);
  if (!fs) return nullptr;
  File f = fs->open(local, "r");
  if (!f) return nullptr;
  len = f.size();
  uint8_t *buf = (uint8_t *)ps_malloc(len ? len : 1);
  if (!buf) return nullptr;
  if (f.read(buf, len) != len) {
    free(buf);
    return nullptr;
  }
  return buf;
}

bool fsReadText(const char *path, String &out) {
  if (!fsExists(path)) return false;
  String local;
  fs::FS *fs = fsFor(path, local);
  if (!fs) return false;
  File f = fs->open(local, "r");
  if (!f) return false;
  out = f.readString();
  return true;
}
