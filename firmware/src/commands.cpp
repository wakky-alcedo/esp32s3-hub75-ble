#include "commands.h"
#include <LittleFS.h>
#include <mbedtls/base64.h>
#include "config.h"
#include "display.h"
#include "effects.h"
#include "fsutil.h"
#include "mic.h"
#include "player.h"
#include "rtc.h"
#include "sdcard.h"
#include "sensors.h"
#include "sound.h"
#include "transfer.h"

volatile bool gRebootRequested = false;
volatile bool gBootloaderRequested = false;
void (*gClearBondsHook)() = nullptr;
int gBootReason = 0;

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
  o["mic"] = micPresent();
  sdJson(o["sd"].to<JsonObject>());
  SensorData sd = sensorsGet();
  o["sensors"] = sd.imuOk || sd.envOk;
  o["time"] = (int64_t)time(nullptr);
  o["bootReason"] = gBootReason;
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
  } else if (!strcmp(cmd, "sensors")) {
    sensorsJson(resp["sensors"].to<JsonObject>());
  } else if (!strcmp(cmd, "mic")) {
    micJson(resp["mic"].to<JsonObject>());
  } else if (!strcmp(cmd, "mic_sim")) {
    micSimulate(req["ms"] | 10000u);
  } else if (!strcmp(cmd, "sensors_sim")) {
    JsonArrayConst g = req["grav"];
    sensorsSimulate(g[0] | 0.0f, g[1] | 1.0f, g[2] | 0.0f, req["shake"] | 0.0f, req["ms"] | 10000u);
  } else if (!strcmp(cmd, "imu_calibrate")) {
    // Hold the panel the way it is normally mounted, then send this.
    if (!sensorsCalibrateDown()) return fail(resp, "panel is lying flat");
    gConfig.save();
    resp["imuRotation"] = gConfig.imuRotation;
  } else if (!strcmp(cmd, "status")) {
    gPlayer.statusJson(resp["status"].to<JsonObject>());
  } else if (!strcmp(cmd, "get_config")) {
    gConfig.toJson(resp["config"].to<JsonObject>());
  } else if (!strcmp(cmd, "set_config")) {
    bool restart = false;
    gConfig.fromJson(req["config"], restart);
    gDisplay.setBrightness(gConfig.brightness);
    if (!restart) gDisplay.applyOrientation(gConfig.panel);
    micApplyGain();
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
    String local;
    fs::FS *fs = fsFor(req["dir"] | "/assets", local);
    if (!fs) return fail(resp, "no sd card");
    JsonArray files = resp["files"].to<JsonArray>();
    File d = fs->open(local);
    if (d && d.isDirectory()) {
      for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        JsonObject e = files.add<JsonObject>();
        e["name"] = f.name();
        e["size"] = f.size();
        if (f.isDirectory()) e["dir"] = true;
      }
    }
  } else if (!strcmp(cmd, "delete_file")) {
    const char *path = req["path"] | "";
    String local;
    fs::FS *fs = fsValidPath(path) ? fsFor(path, local) : nullptr;
    if (!fs || !(fs->remove(local) || fs->rmdir(local))) return fail(resp, "delete failed");
  } else if (!strcmp(cmd, "read_file")) {
    // Returns up to "length" bytes (max 3072) from "offset" as base64.
    const char *path = req["path"] | "";
    String local;
    fs::FS *fs = fsValidPath(path) ? fsFor(path, local) : nullptr;
    File f = fs ? fs->open(local, "r") : File();
    if (!f || f.isDirectory()) return fail(resp, "not found");
    uint32_t offset = req["offset"] | 0u;
    size_t len = min<size_t>(req["length"] | 3072u, 3072);
    std::unique_ptr<uint8_t[]> buf(new uint8_t[len]);
    f.seek(offset);
    size_t got = f.read(buf.get(), len);
    std::unique_ptr<uint8_t[]> b64(new uint8_t[got * 4 / 3 + 8]);
    size_t outLen = 0;
    mbedtls_base64_encode(b64.get(), got * 4 / 3 + 8, &outLen, buf.get(), got);
    resp["size"] = f.size();
    resp["offset"] = offset;
    resp["data"] = String((const char *)b64.get(), outLen);
  } else if (!strcmp(cmd, "snapshot")) {
    // The last rendered frame as base64 RGB565 (little endian), for debugging.
    GFXcanvas16 &c = gDisplay.canvas();
    size_t n = (size_t)c.width() * c.height() * 2, cap = n * 4 / 3 + 8, outLen = 0;
    uint8_t *b64 = (uint8_t *)ps_malloc(cap);
    if (!b64) return fail(resp, "no memory");
    mbedtls_base64_encode(b64, cap, &outLen, (const uint8_t *)c.getBuffer(), n);
    resp["w"] = c.width();
    resp["h"] = c.height();
    resp["data"] = String((const char *)b64, outLen);
    free(b64);
  } else if (!strcmp(cmd, "sd_mount")) {
    sdBegin();
    sdJson(resp["sd"].to<JsonObject>());
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
