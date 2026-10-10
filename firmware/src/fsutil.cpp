#include "fsutil.h"
#include <LittleFS.h>
#include <sys/stat.h>

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

bool fsExists(const char *path) {
  struct stat st;
  return stat((String("/littlefs") + path).c_str(), &st) == 0;
}

bool fsValidPath(const char *path) {
  if (!path || path[0] != '/') return false;
  if (strstr(path, "..")) return false;
  if (strlen(path) > 64) return false;
  return true;
}

void fsEnsureDir(const char *dir) {
  if (!fsExists(dir)) LittleFS.mkdir(dir);
}

bool fsCommitTmp(const char *path) {
  String tmp = String(path) + ".tmp";
  if (LittleFS.rename(tmp, path)) return true;
  LittleFS.remove(path);
  return LittleFS.rename(tmp, path);
}

bool fsWriteAtomic(const char *path, const uint8_t *data, size_t len) {
  String tmp = String(path) + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  size_t written = f.write(data, len);
  f.close();
  if (written != len) {
    LittleFS.remove(tmp);
    return false;
  }
  return fsCommitTmp(path);
}

bool fsWriteAtomic(const char *path, const String &text) {
  return fsWriteAtomic(path, (const uint8_t *)text.c_str(), text.length());
}

uint8_t *fsReadAlloc(const char *path, size_t &len) {
  if (!fsExists(path)) return nullptr;
  File f = LittleFS.open(path, "r");
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
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  out = f.readString();
  return true;
}
