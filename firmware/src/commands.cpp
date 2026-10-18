#include "commands.h"
#include <LittleFS.h>
#include <mbedtls/base64.h>
#include "config.h"
#include "display.h"
#include "effects.h"
#include "fsutil.h"
#include "player.h"
#include "rtc.h"
#include "sound.h"
#include "transfer.h"

volatile bool gRebootRequested = false;
volatile bool gBootloaderRequested = false;
void (*gClearBondsHook)() = nullptr;

static void fail(JsonDocument &resp, const char *err) {
  resp["ok"] = false;
  resp["error"] = err;
}

static void infoJson(JsonObject o) {
  o["fw"] = FW_VERSION;
  o["proto"] = PROTOCOL_VERSION;
  o["name"] = gConfig.name;
  o["width"] = gDisplay.width();
  o["height"] = gDisplay.height();
  o["fsTotal"] = LittleFS.totalBytes();
  o["fsUsed"] = LittleFS.usedBytes();
  o["rtc"] = rtcPresent();
  o["sound"] = soundPresent();
  o["time"] = (int64_t)time(nullptr);
  o["effects"] = effectNames();
  JsonArray presets = o["presets"].to<JsonArray>();
  for (int i = 0; i < MAX_PRESETS; i++) {
    if (fsExists((String("/presets/") + i + ".json").c_str())) presets.add(i);
  }
}

void handleCommand(JsonObjectConst req, JsonDocument &resp) {
  const char *cmd = req["cmd"] | "";
  if (req["id"].is<int>()) resp["id"] = req["id"];
  resp["ok"] = true;
  StateLock lock;

  if (!strcmp(cmd, "info")) {
    infoJson(resp["info"].to<JsonObject>());
  } else if (!strcmp(cmd, "status")) {
    gPlayer.statusJson(resp["status"].to<JsonObject>());
  } else if (!strcmp(cmd, "get_config")) {
    gConfig.toJson(resp["config"].to<JsonObject>());
  } else if (!strcmp(cmd, "set_config")) {
    bool restart = false;
    gConfig.fromJson(req["config"], restart);
    gDisplay.setBrightness(gConfig.brightness);
    if (!restart) gDisplay.applyOrientation(gConfig.panel);
    gConfig.save();
    resp["reboot"] = restart;
    if (restart) gRebootRequested = true;
  } else if (!strcmp(cmd, "brightness")) {
    // "save": false lets a slider stream values without wearing the flash.
    gConfig.brightness = constrain((int)(req["value"] | 60), 0, 255);
    gDisplay.setBrightness(gConfig.brightness);
    if (req["save"] | true) gConfig.save();
  } else if (!strcmp(cmd, "speed")) {
    gConfig.speed = constrain((int)(req["value"] | 100), 10, 400);
    if (req["save"] | true) gConfig.save();
  } else if (!strcmp(cmd, "play")) {
    gPlayer.setPaused(false);
  } else if (!strcmp(cmd, "pause")) {
    gPlayer.setPaused(true);
  } else if (!strcmp(cmd, "next")) {
    gPlayer.next();
  } else if (!strcmp(cmd, "prev")) {
    gPlayer.prev();
  } else if (!strcmp(cmd, "standby")) {
    gDisplay.setStandby(req["on"] | true);
  } else if (!strcmp(cmd, "preset")) {
    if (!gPlayer.selectPreset(req["index"] | -1)) fail(resp, "bad index");
  } else if (!strcmp(cmd, "get_preset")) {
    String text;
    int i = req["index"] | -1;
    if (!fsReadText((String("/presets/") + i + ".json").c_str(), text)) return fail(resp, "not found");
    deserializeJson(resp["preset"].to<JsonVariant>(), text);
  } else if (!strcmp(cmd, "set_preset")) {
    int i = req["index"] | -1;
    if (i < 0 || i >= MAX_PRESETS) return fail(resp, "bad index");
    if (!req["preset"]["items"].is<JsonArrayConst>()) return fail(resp, "missing items");
    fsEnsureDir("/presets");
    String text;
    serializeJson(req["preset"], text);
    if (!fsWriteAtomic((String("/presets/") + i + ".json").c_str(), text)) return fail(resp, "write failed");
    gPlayer.presetChanged(i);
  } else if (!strcmp(cmd, "delete_preset")) {
    int i = req["index"] | -1;
    LittleFS.remove(String("/presets/") + i + ".json");
    gPlayer.presetChanged(i);
  } else if (!strcmp(cmd, "reload")) {
    // After uploading /presets/N.json via the file transfer.
    gPlayer.presetChanged(req["index"] | -1);
  } else if (!strcmp(cmd, "list_files")) {
    const char *dir = req["dir"] | "/assets";
    JsonArray files = resp["files"].to<JsonArray>();
    File d = LittleFS.open(dir);
    if (d && d.isDirectory()) {
      for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        JsonObject e = files.add<JsonObject>();
        e["name"] = f.name();
        e["size"] = f.size();
      }
    }
  } else if (!strcmp(cmd, "delete_file")) {
    const char *path = req["path"] | "";
    if (!fsValidPath(path) || !LittleFS.remove(path)) return fail(resp, "delete failed");
  } else if (!strcmp(cmd, "file_begin")) {
    String err;
    if (!gReceiver.begin(req["path"] | "", req["size"] | 0u, req["crc"] | 0u, err)) return fail(resp, err.c_str());
  } else if (!strcmp(cmd, "file_data")) {
    // Serial fallback; BLE sends raw chunks on the bulk characteristic.
    const char *b64 = req["data"] | "";
    size_t inLen = strlen(b64), outLen = 0;
    std::unique_ptr<uint8_t[]> buf(new uint8_t[inLen * 3 / 4 + 4]);
    if (mbedtls_base64_decode(buf.get(), inLen * 3 / 4 + 4, &outLen, (const uint8_t *)b64, inLen) != 0)
      return fail(resp, "bad base64");
    if (!gReceiver.write(req["offset"] | 0u, buf.get(), outLen)) {
      resp["expected"] = gReceiver.expected();
      return fail(resp, "offset");
    }
  } else if (!strcmp(cmd, "file_end")) {
    String err;
    if (!gReceiver.end(err)) return fail(resp, err.c_str());
  } else if (!strcmp(cmd, "file_status")) {
    resp["active"] = gReceiver.active();
    resp["expected"] = gReceiver.expected();
  } else if (!strcmp(cmd, "file_abort")) {
    gReceiver.abort();
  } else if (!strcmp(cmd, "sync_time")) {
    if (!req["epoch"].is<int64_t>()) return fail(resp, "missing epoch");
    rtcSetTime((time_t)req["epoch"].as<int64_t>());
    if (req["tzOffsetMin"].is<int>()) {
      gConfig.tzOffsetMin = req["tzOffsetMin"];
      gConfig.save();
    }
  } else if (!strcmp(cmd, "sound")) {
    // Test playback, e.g. from the volume setting.
    if (!soundPresent()) return fail(resp, "no sound");
    soundPlay(soundFromName(req["name"] | "chime"));
  } else if (!strcmp(cmd, "clear_bonds")) {
    if (gClearBondsHook) gClearBondsHook();
  } else if (!strcmp(cmd, "reboot")) {
    gRebootRequested = true;
  } else if (!strcmp(cmd, "bootloader")) {
    gBootloaderRequested = true;
  } else {
    fail(resp, "unknown cmd");
  }
}
