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
  bool flipH = false;      // mirror left/right (after rotation)
  bool flipV = false;      // mirror top/bottom (after rotation)
  uint8_t driver = 0;      // HUB75_I2S_CFG::shift_driver
  bool clkphase = false;

  bool operator==(const PanelConfig &o) const {
    return panelW == o.panelW && panelH == o.panelH && cols == o.cols && rows == o.rows &&
           startTop == o.startTop && startLeft == o.startLeft && serpentine == o.serpentine &&
           rotation == o.rotation && flipH == o.flipH && flipV == o.flipV && driver == o.driver &&
           clkphase == o.clkphase;
  }
  bool operator!=(const PanelConfig &o) const { return !(*this == o); }
  // Changes limited to orientation that keep the canvas size can be applied live.
  bool sameHardware(const PanelConfig &o) const {
    return panelW == o.panelW && panelH == o.panelH && cols == o.cols && rows == o.rows &&
           startTop == o.startTop && startLeft == o.startLeft && serpentine == o.serpentine &&
           driver == o.driver && clkphase == o.clkphase && (rotation & 1) == (o.rotation & 1);
  }
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
  uint8_t volume = 60;  // speaker, 0-100 (0 = mute)
  uint8_t imuRotation = 0;   // board orientation on the panel, quarter turns
  int16_t tempOffset10 = 0;  // added to the SHTC3 temperature, 0.1 degC units
  uint8_t micGain = 24;      // ES7210 microphone gain, dB (0-37)

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
