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

static const uint32_t FRAME_MS = 16;

static Button sButton;

// esp_restart() only resets the CPUs. With the HUB75 GDMA and BLE both in use
// the next boot then hangs (in NimBLE init or display init), so every reboot
// goes through the RTC watchdog, which resets the whole chip like the RST
// button does. downloadMode instead uses a software system reset, which keeps
// the RTC domain so the forced-download flag survives and the ROM waits for
// esptool. (An RWDT system reset would leave the RWDT armed, and the ROM
// downloader never feeds it, so the chip reset-looped every ~13ms.)
[[noreturn]] static void fullReset(bool downloadMode) {
  gDisplay.end();
  Serial.flush();
  delay(100);
  if (downloadMode) {
    periph_module_reset(PERIPH_LCD_CAM_MODULE);
    periph_module_reset(PERIPH_GDMA_MODULE);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
    while (true) {
    }
  }
  wdt_hal_context_t rwdt;
  wdt_hal_init(&rwdt, WDT_RWDT, 0, false);
  wdt_hal_write_protect_disable(&rwdt);
  wdt_hal_config_stage(&rwdt, WDT_STAGE0, 2000,  // ~13ms on the 150kHz RTC clock
                       WDT_STAGE_ACTION_RESET_RTC);
  wdt_hal_enable(&rwdt);
  wdt_hal_write_protect_enable(&rwdt);
  while (true) {
  }
}
static String sLine;

// One JSON command per line, one JSON response per line.
static void pollSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
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
    serializeJson(resp, Serial);
    Serial.println();
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

void setup() {
  // A software reset (ESP.restart, esptool) leaves the HUB75 GDMA/LCD_CAM
  // transfer of the previous run going, which hangs BLE or display init.
  periph_module_reset(PERIPH_LCD_CAM_MODULE);
  periph_module_reset(PERIPH_GDMA_MODULE);
  // Left set by a "bootloader" command whose flash was aborted.
  REG_WRITE(RTC_CNTL_OPTION1_REG, 0);

  // file_data lines exceed the default 256 byte USB CDC buffer.
  Serial.setRxBufferSize(8192);
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[boot] LED Display Controller %s\n", FW_VERSION);
  Serial.printf("[boot] reset reason %d (cpu0 %d)\n", (int)esp_reset_reason(), (int)rtc_get_reset_reason(0));
  Serial.printf("[boot] PSRAM %u bytes free, heap %u\n", ESP.getFreePsram(), ESP.getFreeHeap());

  StateLock::init();
  if (!fsBegin()) Serial.println("[boot] LittleFS mount failed");
  else Serial.printf("[boot] LittleFS %u/%u bytes used\n", LittleFS.usedBytes(), LittleFS.totalBytes());
  if (!gConfig.load()) {
    Serial.println("[boot] no config, using defaults");
    gConfig.save();
  }
  rtcBegin();
  bleBegin(gConfig.name.c_str());

  if (!gDisplay.begin(gConfig.panel)) {
    Serial.println("[boot] display init failed");
  }
  gDisplay.setBrightness(gConfig.brightness);
  sButton.begin(0);
  gPlayer.begin();
  Serial.println("[boot] ready");
}

void loop() {
  static uint32_t last = millis();
  static uint32_t statAt = 0, frames = 0, busyUs = 0;
  uint32_t start = millis();
  uint32_t dt = start - last;
  last = start;

  pollSerial();
  handleButton();
  uint32_t t0 = micros();
  {
    StateLock lock;
    gPlayer.render(gDisplay.canvas(), dt);
    gDisplay.present();
  }
  busyUs += micros() - t0;
  frames++;

  if (gRebootRequested || gBootloaderRequested) {
    delay(200);  // let the response go out
    fullReset(gBootloaderRequested);
  }

  if (start - statAt >= 10000) {
    Serial.printf("[stat] %u fps, frame %u us, est %u mA, scale %u, heap %u\n", frames * 1000 / (start - statAt),
                  frames ? busyUs / frames : 0, gDisplay.estimatedMa(), gDisplay.powerScale(), ESP.getFreeHeap());
    statAt = start;
    frames = 0;
    busyUs = 0;
  }

  uint32_t spent = millis() - start;
  if (spent < FRAME_MS) delay(FRAME_MS - spent);
  else delay(1);  // let lower priority tasks run
}
