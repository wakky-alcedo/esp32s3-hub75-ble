#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <driver/periph_ctrl.h>
#include <hal/wdt_hal.h>
#include <rom/rtc.h>
#include <soc/rtc_cntl_reg.h>
#include "ble.h"
#include "button.h"
#include "commands.h"
#include "config.h"
#include "display.h"
#include "fsutil.h"
#include "player.h"
#include "rtc.h"
#include "sound.h"
#include "trace.h"
#include "usb_console.h"

static const uint32_t FRAME_MS = 16;

static Button sButton;

RTC_NOINIT_ATTR volatile uint32_t gTraceMain;
RTC_NOINIT_ATTR volatile uint32_t gTraceBle;

// (Re)arms the RTC watchdog: after `ticks` of the ~150kHz RTC clock without
// another call it resets the whole chip, like the RST button.
static void rwdtArm(uint32_t ticks) {
  wdt_hal_context_t rwdt;
  wdt_hal_init(&rwdt, WDT_RWDT, 0, false);
  wdt_hal_write_protect_disable(&rwdt);
  wdt_hal_config_stage(&rwdt, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_RTC);
  wdt_hal_enable(&rwdt);
  wdt_hal_feed(&rwdt);
  wdt_hal_write_protect_enable(&rwdt);
}

// The main loop feeds this; if it stalls for ~8s the chip resets cleanly and
// the breadcrumbs above tell where it was stuck.
static void loopWatchdogFeed() { rwdtArm(150000 * 8); }

// esp_restart() only resets the CPUs. With the HUB75 GDMA and BLE both in use
// the next boot then hangs (in NimBLE init or display init), so every reboot
// goes through the RTC watchdog, which resets the whole chip like the RST
// button does. downloadMode instead uses a software system reset, which keeps
// the RTC domain so the forced-download flag survives and the ROM waits for
// esptool. (An RWDT system reset would leave the RWDT armed, and the ROM
// downloader never feeds it, so the chip reset-looped every ~13ms.)
[[noreturn]] static void fullReset(bool downloadMode) {
  gDisplay.end();
  Console.flush();
  delay(100);
  if (downloadMode) {
    periph_module_reset(PERIPH_LCD_CAM_MODULE);
    periph_module_reset(PERIPH_GDMA_MODULE);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
    while (true) {
    }
  }
  rwdtArm(2000);  // ~13ms
  while (true) {
  }
}
static String sLine;

// One JSON command per line, one JSON response per line.
static void pollSerial() {
  while (Console.available()) {
    char ch = Console.read();
    if (ch == '\r') continue;
    if (ch != '\n') {
      if (sLine.length() < 8192) sLine += ch;
      continue;
    }
    if (sLine.length() == 0) continue;
    JsonDocument req, resp;
    DeserializationError e = deserializeJson(req, sLine);
    sLine = "";
    if (e) {
      resp["ok"] = false;
      resp["error"] = "bad json";
    } else {
      handleCommand(req.as<JsonObjectConst>(), resp);
    }
    serializeJson(resp, Console);
    Console.println();
  }
}

static void handleButton() {
  ButtonEvent ev = sButton.poll();
  if (ev == BTN_NONE) return;
  JsonDocument evt;
  evt["evt"] = "status";
  StateLock lock;
  switch (ev) {
    case BTN_SHORT:
      if (gDisplay.standby()) {
        gDisplay.setStandby(false);
        break;
      }
      gPlayer.nextPreset();
      gPlayer.toast(String("P") + (gPlayer.currentPreset() + 1));
      break;
    case BTN_LONG: {
      // Cycle through the three brightness levels.
      int next = 0;
      for (int i = 0; i < 3; i++) {
        if (gConfig.brightLevels[i] > gConfig.brightness) {
          next = i;
          break;
        }
      }
      gConfig.brightness = gConfig.brightLevels[next];
      gDisplay.setBrightness(gConfig.brightness);
      gConfig.save();
      gPlayer.toast(String("BRIGHT ") + (next + 1));
      break;
    }
    case BTN_VERY_LONG:
      gDisplay.setStandby(!gDisplay.standby());
      break;
    case BTN_HOLD_10S:
      // Reaching 10s passed through the 3s standby toggle; undo it.
      gDisplay.setStandby(!gDisplay.standby());
      if (gClearBondsHook) gClearBondsHook();
      gPlayer.toast("UNPAIRED", 2000);
      break;
    default:
      break;
  }
  // Keep the app's controls in sync with changes made on the device.
  gPlayer.statusJson(evt["status"].to<JsonObject>());
  bleNotifyEvent(evt);
}

// Survives the RTC-WDT reset (not power-on), so the reason that triggered a
// recovery reset can still be logged on the clean boot that follows.
RTC_NOINIT_ATTR static uint32_t sRecoveredFrom;
static const uint32_t RECOVERED_MAGIC = 0x52450000u;

// Resets that do not restart the whole chip (panic, esp_restart, task/int
// watchdogs). Booting on from one of these hangs, so turn it into a full reset.
static bool partialReset(int r) {
  switch (r) {
    case 3:   // RTC_SW_SYS_RESET
    case 7:   // TG0WDT_SYS_RESET
    case 8:   // TG1WDT_SYS_RESET
    case 9:   // RTCWDT_SYS_RESET
    case 11:  // TG0WDT_CPU_RESET
    case 12:  // RTC_SW_CPU_RESET
    case 13:  // RTCWDT_CPU_RESET
    case 17:  // TG1WDT_CPU_RESET
    case 18:  // SUPER_WDT_RESET
      return true;
    default:
      return false;
  }
}

void setup() {
  int resetReason = rtc_get_reset_reason(0);
  if (partialReset(resetReason)) {
    sRecoveredFrom = RECOVERED_MAGIC | resetReason;
    fullReset(false);
  }
  const uint32_t prevMain = gTraceMain, prevBle = gTraceBle;
  TRACE_MAIN(1);
  TRACE_BLE(0);
  rwdtArm(150000 * 15);  // setup itself must not hang either

  // A software reset (ESP.restart, esptool) leaves the HUB75 GDMA/LCD_CAM
  // transfer of the previous run going, which hangs BLE or display init.
  periph_module_reset(PERIPH_LCD_CAM_MODULE);
  periph_module_reset(PERIPH_GDMA_MODULE);
  // Left set by a "bootloader" command whose flash was aborted.
  REG_WRITE(RTC_CNTL_OPTION1_REG, 0);

  Console.begin();
  delay(200);
  Console.printf("\n[boot] LED Display Controller %s\n", FW_VERSION);
  Console.printf("[boot] reset reason %d\n", resetReason);
  if ((sRecoveredFrom & 0xFFFF0000u) == RECOVERED_MAGIC) {
    Console.printf("[boot] recovered from partial reset (reason %u)\n", sRecoveredFrom & 0xFFFF);
  }
  sRecoveredFrom = 0;
  if (resetReason != 1) Console.printf("[boot] last trace: main=%u ble=%u\n", prevMain, prevBle);
  Console.printf("[boot] PSRAM %u bytes free, heap %u\n", ESP.getFreePsram(), ESP.getFreeHeap());

  StateLock::init();
  TRACE_MAIN(2);
  if (!fsBegin()) Console.println("[boot] LittleFS mount failed");
  else Console.printf("[boot] LittleFS %u/%u bytes used\n", LittleFS.usedBytes(), LittleFS.totalBytes());
  if (!gConfig.load()) {
    Console.println("[boot] no config, using defaults");
    gConfig.save();
  }
  TRACE_MAIN(3);
  rtcBegin();
  soundBegin();  // shares the RTC's I2C bus
  TRACE_MAIN(4);
  bleBegin(gConfig.name.c_str());
  TRACE_MAIN(5);
  if (!gDisplay.begin(gConfig.panel)) {
    Console.println("[boot] display init failed");
  }
  gDisplay.setBrightness(gConfig.brightness);
  TRACE_MAIN(6);
  sButton.begin(0);
  gPlayer.begin();
  TRACE_MAIN(7);
  Console.println("[boot] ready");
}

void loop() {
  static uint32_t last = millis();
  static uint32_t statAt = 0, frames = 0, busyUs = 0;
  uint32_t start = millis();
  uint32_t dt = start - last;
  last = start;
  loopWatchdogFeed();

  TRACE_MAIN(10);
  pollSerial();
  TRACE_MAIN(11);
  handleButton();
  uint32_t t0 = micros();
  TRACE_MAIN(12);
  {
    StateLock lock;
    TRACE_MAIN(13);
    gPlayer.render(gDisplay.canvas(), dt);
    TRACE_MAIN(14);
    gDisplay.present();
  }
  busyUs += micros() - t0;
  frames++;

  if (gRebootRequested || gBootloaderRequested) {
    delay(200);  // let the response go out
    TRACE_MAIN(999);
    fullReset(gBootloaderRequested);
  }

  TRACE_MAIN(15);
  if (start - statAt >= 10000) {
    Console.printf("[stat] %u fps, frame %u us, est %u mA, scale %u, heap %u\n", frames * 1000 / (start - statAt),
                  frames ? busyUs / frames : 0, gDisplay.estimatedMa(), gDisplay.powerScale(), ESP.getFreeHeap());
    statAt = start;
    frames = 0;
    busyUs = 0;
  }

  TRACE_MAIN(16);
  uint32_t spent = millis() - start;
  if (spent < FRAME_MS) delay(FRAME_MS - spent);
  else delay(1);  // let lower priority tasks run
}
