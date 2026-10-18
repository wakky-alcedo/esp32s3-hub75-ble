#include "config.h"
#include "fsutil.h"

Config gConfig;

static const char *CONFIG_PATH = "/config.json";

uint16_t Config::panelFullWhiteMa() const {
  if (panelFullMa) return panelFullMa;
  // Rough figure for cheap HUB75 modules: ~1mA per pixel at full white
  // (1/16 or 1/32 scan keeps the average far below the LED peak current).
  return (uint32_t)panel.panelW * panel.panelH * 1000 / 1024;
}

void Config::toJson(JsonObject o) const {
  JsonObject p = o["panel"].to<JsonObject>();
  p["w"] = panel.panelW;
  p["h"] = panel.panelH;
  p["cols"] = panel.cols;
  p["rows"] = panel.rows;
  p["startTop"] = panel.startTop;
  p["startLeft"] = panel.startLeft;
  p["serpentine"] = panel.serpentine;
  p["rotation"] = panel.rotation;
  p["flipH"] = panel.flipH;
  p["flipV"] = panel.flipV;
  p["driver"] = panel.driver;
  p["clkphase"] = panel.clkphase;
  o["brightness"] = brightness;
  JsonArray bl = o["brightLevels"].to<JsonArray>();
  for (uint8_t v : brightLevels) bl.add(v);
  o["powerProfile"] = powerProfile == POWER_AC ? "ac" : "battery";
  o["limitBatteryMa"] = limitBatteryMa;
  o["limitAcMa"] = limitAcMa;
  o["panelFullMa"] = panelFullMa;
  o["name"] = name;
  o["lastPreset"] = lastPreset;
  o["speed"] = speed;
  o["tzOffsetMin"] = tzOffsetMin;
  o["volume"] = volume;
}

template <typename T>
static bool setIf(JsonObjectConst o, const char *key, T &dst) {
  if (!o[key].is<T>()) return false;
  T v = o[key].as<T>();
  bool changed = v != dst;
  dst = v;
  return changed;
}

void Config::fromJson(JsonObjectConst o, bool &needsRestart) {
  JsonObjectConst p = o["panel"];
  if (!p.isNull()) {
    PanelConfig n = panel;
    setIf(p, "w", n.panelW);
    setIf(p, "h", n.panelH);
    setIf(p, "cols", n.cols);
    setIf(p, "rows", n.rows);
    setIf(p, "startTop", n.startTop);
    setIf(p, "startLeft", n.startLeft);
    setIf(p, "serpentine", n.serpentine);
    setIf(p, "rotation", n.rotation);
    setIf(p, "flipH", n.flipH);
    setIf(p, "flipV", n.flipV);
    setIf(p, "driver", n.driver);
    setIf(p, "clkphase", n.clkphase);
    n.panelW = constrain(n.panelW, 16, 128);
    n.panelH = constrain(n.panelH, 16, 64);
    n.cols = constrain(n.cols, 1, 6);
    n.rows = constrain(n.rows, 1, 6);
    if (n.cols * n.rows > 6) n.rows = 6 / n.cols;
    n.rotation &= 3;
    if (!n.sameHardware(panel)) needsRestart = true;
    panel = n;
  }
  setIf(o, "brightness", brightness);
  JsonArrayConst bl = o["brightLevels"];
  if (bl.size() == 3) {
    for (int i = 0; i < 3; i++) brightLevels[i] = bl[i];
  }
  if (o["powerProfile"].is<const char *>()) {
    powerProfile = strcmp(o["powerProfile"], "ac") == 0 ? POWER_AC : POWER_BATTERY;
  }
  setIf(o, "limitBatteryMa", limitBatteryMa);
  setIf(o, "limitAcMa", limitAcMa);
  setIf(o, "panelFullMa", panelFullMa);
  if (o["name"].is<const char *>()) {
    String n = o["name"].as<const char *>();
    n.trim();
    if (n.length() > 0 && n.length() <= 20 && n != name) {
      name = n;
      needsRestart = true;  // advertised name is fixed at BLE init
    }
  }
  setIf(o, "lastPreset", lastPreset);
  setIf(o, "speed", speed);
  speed = constrain(speed, 10, 400);
  setIf(o, "tzOffsetMin", tzOffsetMin);
  setIf(o, "volume", volume);
  volume = min<uint8_t>(volume, 100);
}

bool Config::load() {
  String text;
  if (!fsReadText(CONFIG_PATH, text)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, text)) return false;
  bool dummy = false;
  fromJson(doc.as<JsonObjectConst>(), dummy);
  return true;
}

bool Config::save() const {
  JsonDocument doc;
  toJson(doc.to<JsonObject>());
  String text;
  serializeJson(doc, text);
  return fsWriteAtomic(CONFIG_PATH, text);
}
