#pragma once
#include <Arduino.h>
#include <FS.h>

// LittleFS helpers. Writes go to "<path>.tmp" and are renamed into place so a
// power cut mid-write never leaves a half-written file behind.

bool fsBegin();
// Paths starting with "/sd/" live on the TF card (mounting it on demand),
// everything else on LittleFS. Sets local to the path within that FS;
// returns nullptr when the card is not available.
fs::FS *fsFor(const char *path, String &local);
bool fsWriteAtomic(const char *path, const uint8_t *data, size_t len);
bool fsWriteAtomic(const char *path, const String &text);
bool fsCommitTmp(const char *path);  // rename "<path>.tmp" -> path
// Reads a whole file into PSRAM. Caller frees with free(). Returns nullptr on failure.
uint8_t *fsReadAlloc(const char *path, size_t &len);
bool fsReadText(const char *path, String &out);
// LittleFS.exists() logs an error for every missing file; this stays quiet.
bool fsExists(const char *path);
bool fsValidPath(const char *path);
void fsEnsureDir(const char *dir);
