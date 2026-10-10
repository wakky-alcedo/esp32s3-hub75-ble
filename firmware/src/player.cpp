#include "player.h"
#include <LittleFS.h>
#include "config.h"
#include "display.h"
#include "fsutil.h"

Player gPlayer;

static SemaphoreHandle_t sLock = nullptr;
void StateLock::init() { sLock = xSemaphoreCreateRecursiveMutex(); }
StateLock::StateLock() { xSemaphoreTakeRecursive(sLock, portMAX_DELAY); }
StateLock::~StateLock() { xSemaphoreGiveRecursive(sLock); }

static const uint32_t FADE_MS = 250;

// Shown when no preset has been stored yet.
static const char DEMO_PRESET[] = R"JSON({
  "name": "demo",
  "transition": "fade",
  "items": [
    {"type": "text", "text": "LED DISPLAY", "color": "rainbow", "speed": 30, "repeat": 2,
     "bg": {"type": "effect", "effect": "stars"}},
    {"type": "effect", "effect": "plasma", "duration": 8000},
    {"type": "effect", "effect": "fire", "duration": 8000},
    {"type": "effect", "effect": "matrix", "duration": 8000},
    {"type": "effect", "effect": "life", "duration": 10000},
    {"type": "effect", "effect": "noise", "duration": 8000},
    {"type": "effect", "effect": "rainbow", "duration": 6000},
    {"type": "clock", "color": "#40c0ff", "duration": 5000}
  ]
})JSON";

static String presetPath(int i) { return String("/presets/") + i + ".json"; }

void Player::begin() { selectPreset(gConfig.lastPreset, false); }

bool Player::selectPreset(int index, bool persist) {
  if (index < 0 || index >= MAX_PRESETS) return false;
  String text;
  demo_ = !fsReadText(presetPath(index).c_str(), text);
  if (demo_) text = DEMO_PRESET;
  doc_.clear();
  if (deserializeJson(doc_, text) || !doc_["items"].is<JsonArrayConst>() || doc_["items"].size() == 0) {
    Serial.printf("[player] preset %d invalid, using demo\n", index);
    deserializeJson(doc_, DEMO_PRESET);
    demo_ = true;
  }
  items_ = doc_["items"].as<JsonArrayConst>();
  fade_ = strcmp(doc_["transition"] | "fade", "cut") != 0;
  preset_ = index;
  startItem(0);
  phase_ = PLAYING;
  gDisplay.setFade(256);
  if (persist && gConfig.lastPreset != index) {
    gConfig.lastPreset = index;
    gConfig.save();
  }
  Serial.printf("[player] preset %d (%s, %d items)\n", index, demo_ ? "demo" : "stored", items_.size());
  return true;
}

void Player::nextPreset() {
  // Skip empty slots; with nothing stored this stays on slot 0's demo.
  for (int i = 1; i <= MAX_PRESETS; i++) {
    int n = (preset_ + i) % MAX_PRESETS;
    if (fsExists(presetPath(n).c_str())) {
      selectPreset(n);
      return;
    }
  }
  selectPreset(0);
}

void Player::presetChanged(int index) {
  if (index == preset_) selectPreset(index, false);
}

void Player::startItem(int index) {
  item_.reset();
  int n = items_.size();
  if (n == 0) return;
  // Skip items whose assets fail to load, but give up after one full round.
  for (int tries = 0; tries < n; tries++) {
    index_ = ((index % n) + n) % n;
    Item *it = createItem(items_[index_], gDisplay.width(), gDisplay.height());
    if (it) {
      item_.reset(it);
      return;
    }
    index++;
  }
}

void Player::next() {
  pendingIndex_ = index_ + 1;
  phase_ = fade_ ? FADING_OUT : PLAYING;
  phaseMs_ = 0;
  if (!fade_) startItem(pendingIndex_);
}

void Player::prev() {
  pendingIndex_ = index_ - 1;
  phase_ = fade_ ? FADING_OUT : PLAYING;
  phaseMs_ = 0;
  if (!fade_) startItem(pendingIndex_);
}

void Player::toast(const String &text, uint32_t ms) {
  toast_ = text;
  toastMs_ = ms;
}

void Player::render(GFXcanvas16 &c, uint32_t dtMs) {
  uint32_t dt = paused_ ? 0 : dtMs;
  uint32_t motion = dt * gConfig.speed / 100;

  if (item_) item_->render(c, dt, motion);
  else c.fillScreen(0);

  switch (phase_) {
    case PLAYING:
      gDisplay.setFade(256);
      if (item_ && item_->done()) {
        if (fade_) {
          pendingIndex_ = index_ + 1;
          phase_ = FADING_OUT;
          phaseMs_ = 0;
        } else {
          startItem(index_ + 1);
        }
      }
      break;
    case FADING_OUT:
      phaseMs_ += dtMs;
      gDisplay.setFade(phaseMs_ >= FADE_MS ? 0 : 256 - phaseMs_ * 256 / FADE_MS);
      if (phaseMs_ >= FADE_MS) {
        startItem(pendingIndex_);
        phase_ = FADING_IN;
        phaseMs_ = 0;
      }
      break;
    case FADING_IN:
      phaseMs_ += dtMs;
      gDisplay.setFade(phaseMs_ >= FADE_MS ? 256 : phaseMs_ * 256 / FADE_MS);
      if (phaseMs_ >= FADE_MS) phase_ = PLAYING;
      break;
  }

  if (pin_.length()) {
    gDisplay.setFade(256);
    drawOverlay(c, "PIN " + pin_);
  } else if (toastMs_) {
    drawOverlay(c, toast_);
    toastMs_ = dtMs >= toastMs_ ? 0 : toastMs_ - dtMs;
  }
}

void Player::drawOverlay(GFXcanvas16 &c, const String &text) {
  int tw = builtinTextWidth(text.c_str(), 1);
  int y = (c.height() - 11) / 2;
  int x = max(0, (c.width() - tw) / 2);
  c.fillRect(0, y, c.width(), 11, 0);
  drawBuiltinText(c, text.c_str(), x, y + 2, 1, 0xFFFF);
}

void Player::statusJson(JsonObject o) const {
  o["preset"] = preset_;
  o["demo"] = demo_;
  o["item"] = index_;
  o["items"] = items_.size();
  o["paused"] = paused_;
  o["standby"] = gDisplay.standby();
  o["brightness"] = gConfig.brightness;
  o["speed"] = gConfig.speed;
  o["mA"] = gDisplay.estimatedMa();
  o["powerScale"] = gDisplay.powerScale();
}
