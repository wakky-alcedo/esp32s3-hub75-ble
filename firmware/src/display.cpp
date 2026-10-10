#include "display.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "color.h"

Display gDisplay;

// ESP32 + BLE + panel driver ICs with all LEDs off.
static const uint32_t IDLE_MA = 200;

bool Display::begin(const PanelConfig &cfg) {
  panelCount_ = cfg.cols * cfg.rows;
  HUB75_I2S_CFG mx(cfg.panelW, cfg.panelH, panelCount_);
  mx.gpio.e = 9;
  mx.clkphase = cfg.clkphase;
  mx.driver = (HUB75_I2S_CFG::shift_driver)cfg.driver;
  mx.double_buff = true;

  dma_ = new MatrixPanel_I2S_DMA(mx);
  doubleBuffered_ = dma_->begin();
  if (!doubleBuffered_) {
    // Large layouts may not fit two DMA buffers; fall back to a single one.
    delete dma_;
    mx.double_buff = false;
    dma_ = new MatrixPanel_I2S_DMA(mx);
    if (!dma_->begin()) {
      Serial.println("[display] DMA allocation failed");
      return false;
    }
  }

  int16_t pw = cfg.panelW * cfg.cols, ph = cfg.panelH * cfg.rows;
  bool swap = cfg.rotation & 1;
  w_ = swap ? ph : pw;
  h_ = swap ? pw : ph;
  canvas_ = new GFXcanvas16(w_, h_);
  if (!canvas_->getBuffer()) {
    Serial.println("[display] canvas allocation failed");
    return false;
  }
  buildMap(cfg);
  dma_->setBrightness8(brightness_);
  dma_->clearScreen();
  dma_->flipDMABuffer();
  dma_->clearScreen();
  Serial.printf("[display] %dx%d (%d panel(s) %dx%d, dbuf=%d)\n", w_, h_, panelCount_,
                cfg.panelW, cfg.panelH, doubleBuffered_);
  return true;
}

void Display::buildMap(const PanelConfig &cfg) {
  const int pw = cfg.panelW * cfg.cols, ph = cfg.panelH * cfg.rows;
  if (!map_) map_ = (uint32_t *)ps_malloc(sizeof(uint32_t) * w_ * h_);
  for (int y = 0; y < h_; y++) {
    for (int x = 0; x < w_; x++) {
      int vx = cfg.flipH ? w_ - 1 - x : x;
      int vy = cfg.flipV ? h_ - 1 - y : y;
      // Virtual -> physical wall coordinates.
      int px, py;
      switch (cfg.rotation) {
        case 1: px = pw - 1 - vy; py = vx; break;
        case 2: px = pw - 1 - vx; py = ph - 1 - vy; break;
        case 3: px = vy; py = ph - 1 - vx; break;
        default: px = vx; py = vy; break;
      }
      // Physical -> position in the DMA chain.
      int col = px / cfg.panelW, row = py / cfg.panelH;
      int lx = px % cfg.panelW, ly = py % cfg.panelH;
      int k = cfg.startTop ? row : cfg.rows - 1 - row;  // chain row order
      bool flipped = cfg.serpentine && (k & 1);
      bool leftToRight = cfg.startLeft != flipped;
      int pos = leftToRight ? col : cfg.cols - 1 - col;
      int idx = k * cfg.cols + pos;
      if (flipped) {
        lx = cfg.panelW - 1 - lx;
        ly = cfg.panelH - 1 - ly;
      }
      int dx = idx * cfg.panelW + lx, dy = ly;
      map_[y * w_ + x] = ((uint32_t)dx << 16) | dy;
    }
  }
}

void Display::applyOrientation(const PanelConfig &cfg) {
  if (map_) buildMap(cfg);
}

void Display::setBrightness(uint8_t b) {
  if (b == brightness_) return;
  brightness_ = b;
  if (dma_ && !standby_) dma_->setBrightness8(b);
}

void Display::present() {
  if (!dma_) return;
  if (standby_) {
    if (!blanked_) {
      dma_->setBrightness8(0);
      dma_->clearScreen();
      dma_->flipDMABuffer();
      dma_->clearScreen();
      blanked_ = true;
    }
    estimatedMa_ = IDLE_MA;
    return;
  }
  if (blanked_) {
    dma_->setBrightness8(brightness_);
    blanked_ = false;
  }

  const uint16_t *buf = canvas_->getBuffer();
  const int n = w_ * h_;

  // Sum of channel levels, each normalised to 0..63.
  uint32_t sum = 0;
  for (int i = 0; i < n; i++) {
    uint16_t c = buf[i];
    sum += ((c >> 11) << 1) + ((c >> 5) & 0x3F) + ((c & 0x1F) << 1);
  }
  const uint32_t fullMa = (uint32_t)gConfig.panelFullWhiteMa() * panelCount_;
  const uint64_t ledMa = (uint64_t)fullMa * sum * brightness_ / ((uint64_t)n * 189 * 255);
  estimatedMa_ = IDLE_MA + (uint32_t)ledMa;

  const uint32_t limit = gConfig.currentLimitMa();
  uint16_t target = 256;
  if (estimatedMa_ > limit && ledMa > 0) {
    uint32_t budget = limit > IDLE_MA ? limit - IDLE_MA : 0;
    target = (uint16_t)(budget * 256 / ledMa);
  }
  // Clamp down immediately, recover gradually so the image does not pump.
  if (target < powerScale_) powerScale_ = target;
  else powerScale_ = min<uint16_t>(target, powerScale_ + 4);

  const uint16_t s = (uint32_t)powerScale_ * fade_ >> 8;
  for (int i = 0; i < n; i++) {
    uint32_t m = map_[i];
    uint16_t c = s >= 256 ? buf[i] : scale565(buf[i], s);
    dma_->drawPixel(m >> 16, m & 0xFFFF, c);
  }
  dma_->flipDMABuffer();
}
