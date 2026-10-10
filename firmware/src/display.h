#pragma once
#include <Adafruit_GFX.h>
#include "config.h"

class MatrixPanel_I2S_DMA;

// All content is drawn into an RGB565 canvas in virtual (user-facing)
// coordinates. present() maps it onto the chained panels, applies the power
// limit and fade, and flips the DMA buffer.
class Display {
 public:
  bool begin(const PanelConfig &cfg);
  // Rebuilds the pixel map for a new rotation/mirror with the same canvas size.
  void applyOrientation(const PanelConfig &cfg);
  // Stops the DMA output; call before a software reset.
  void end();
  GFXcanvas16 &canvas() { return *canvas_; }
  int16_t width() const { return w_; }
  int16_t height() const { return h_; }

  void setBrightness(uint8_t b);
  uint8_t brightness() const { return brightness_; }
  void setFade(uint16_t f) { fade_ = f; }  // 0..256
  void setStandby(bool on) { standby_ = on; }
  bool standby() const { return standby_; }

  void present();

  // Power estimate of the last presented frame, before limiting (mA).
  uint32_t estimatedMa() const { return estimatedMa_; }
  uint16_t powerScale() const { return powerScale_; }
  bool doubleBuffered() const { return doubleBuffered_; }

 private:
  void buildMap(const PanelConfig &cfg);

  MatrixPanel_I2S_DMA *dma_ = nullptr;
  GFXcanvas16 *canvas_ = nullptr;
  uint32_t *map_ = nullptr;  // virtual pixel index -> (dmaX << 16) | dmaY
  int16_t w_ = 0, h_ = 0;
  uint16_t panelCount_ = 1;
  uint8_t brightness_ = 0;
  uint16_t fade_ = 256;
  uint16_t powerScale_ = 256;
  uint32_t estimatedMa_ = 0;
  bool standby_ = false;
  bool blanked_ = false;
  bool doubleBuffered_ = false;
};

extern Display gDisplay;
