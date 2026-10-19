#include "sdcard.h"
#include <SD_MMC.h>
#include "usb_console.h"

static const int PIN_CLK = 1, PIN_CMD = 44, PIN_D0 = 17;
static const uint32_t RETRY_MS = 5000;

static bool sMounted = false;
static uint32_t sLastTry = 0;
static bool sTried = false;

bool sdBegin() {
  sTried = true;
  sLastTry = millis();
  SD_MMC.setPins(PIN_CLK, PIN_CMD, PIN_D0);
  sMounted = SD_MMC.begin("/sdcard", true);
  if (sMounted) {
    Console.printf("[sd] %s card, %llu MB\n", SD_MMC.cardType() == CARD_SDHC ? "SDHC" : "SD",
                   SD_MMC.cardSize() >> 20);
  } else {
    SD_MMC.end();
    Console.println("[sd] no card");
  }
  return sMounted;
}

bool sdMounted() {
  // A removed card shows up as a failing size query.
  if (sMounted && SD_MMC.cardType() == CARD_NONE) {
    SD_MMC.end();
    sMounted = false;
  }
  return sMounted;
}

bool sdEnsure() {
  if (sdMounted()) return true;
  if (sTried && millis() - sLastTry < RETRY_MS) return false;
  return sdBegin();
}

void sdJson(JsonObject o) {
  o["mounted"] = sdMounted();
  if (!sMounted) return;
  o["total"] = SD_MMC.totalBytes();
  o["used"] = SD_MMC.usedBytes();
}
