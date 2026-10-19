#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// TF card slot (SDMMC, 1-bit bus). Files on it are addressed with the
// "/sd/" path prefix everywhere a path is accepted (see fsFor()).

// Tries to mount the card; quiet when the slot is empty.
bool sdBegin();
bool sdMounted();
// Mounts on demand (at most every few seconds) for hot-inserted cards.
bool sdEnsure();
void sdJson(JsonObject o);
