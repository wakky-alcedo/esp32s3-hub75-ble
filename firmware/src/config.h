#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

struct PanelConfig {
  uint16_t panelW = 64;
  uint16_t panelH = 32;
  uint8_t cols = 1;
  uint8_t rows = 1;
  bool startTop = true;    // chain input panel is in the top row
  bool startLeft = true;   // chain input panel is in the left column
  bool serpentine = true;  // odd rows are mounted upside down (S-shaped chain)
  uint8_t rotation = 0;    // 0..3, quarter turns clockwise
  uint8_t driver = 0;      // HUB75_I2S_CFG::shift_driver
  bool clkphase = false;

  bool operator==(const PanelConfig &o) const {
    return panelW == o.panelW && panelH == o.panelH && cols == o.cols && rows == o.rows &&
           startTop == o.startTop && startLeft == o.startLeft && serpentine == o.serpentine &&
           rotation == o.rotation && driver == o.driver && clkphase == o.clkphase;
  }
  bool operator!=(const PanelConfig &o) const { return !(*this == o); }
};

enum PowerProfile : uint8_t { POWER_BATTERY = 0, POWER_AC = 1 };

struct Config {
  PanelConfig panel;
  uint8_t brightness = 60;
  uint8_t brightLevels[3] = {20, 60, 150};
  uint8_t powerProfile = POWER_BATTERY;
  uint16_t limitBatteryMa = 2000;
  uint16_t limitAcMa = 4000;
  // Estimated draw of one panel showing full white at brightness 255.
  // 0 = derive from the panel size.
  uint16_t panelFullMa = 0;
  String name = "LED-Display";
  uint8_t lastPreset = 0;
  uint16_t speed = 100;  // percent, applies to motion only
  int16_t tzOffsetMin = 540;

  uint16_t currentLimitMa() const {
    return powerProfile == POWER_AC ? limitAcMa : limitBatteryMa;
  }
  uint16_t panelFullWhiteMa() const;

  bool load();
  bool save() const;
  void toJson(JsonObject o) const;
  // Applies the given fields. Sets needsRestart when the panel layout changed.
  void fromJson(JsonObjectConst o, bool &needsRestart);
};

extern Config gConfig;
