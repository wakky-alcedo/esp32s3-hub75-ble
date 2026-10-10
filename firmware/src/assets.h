#pragma once
#include <Adafruit_GFX.h>

// LDB1 bitmap asset (little endian):
//   0  'L','D','B','1'
//   4  u16 width
//   6  u16 height
//   8  u8  format (BitmapFormat)
//   9  u8[3] reserved
//   12 pixel data, row major
enum BitmapFormat : uint8_t {
  BMP_RGB565 = 0,    // 2 bytes / px
  BMP_RGB565A8 = 1,  // 3 bytes / px: RGB565 then alpha
  BMP_A8 = 2,        // 1 byte  / px: alpha mask, colored at draw time
};

struct Bitmap {
  uint16_t w = 0, h = 0;
  uint8_t format = BMP_RGB565;
  uint8_t *data = nullptr;  // PSRAM, owned

  Bitmap() = default;
  Bitmap(const Bitmap &) = delete;
  Bitmap &operator=(const Bitmap &) = delete;
  ~Bitmap() { release(); }
  void release();
  bool valid() const { return data != nullptr; }

  bool load(const char *path);
  // Renders ASCII text with the built-in 5x7 font into an A8 mask.
  bool fromText(const char *text, uint8_t scale);
  bool alloc(uint16_t w, uint16_t h, uint8_t format);
};

// Mask tint: a fixed color, or a hue that varies along x when rainbow is set.
struct Tint {
  uint16_t color = 0xFFFF;
  bool rainbow = false;
  uint8_t hueOffset = 0;
};

// Draws bmp with its top-left at (x, y); clips to the canvas.
void drawBitmap(GFXcanvas16 &c, const Bitmap &bmp, int x, int y, const Tint &tint = Tint());

// Built-in 5x7 font text, used for clock digits and overlays.
int16_t builtinTextWidth(const char *text, uint8_t scale);
void drawBuiltinText(GFXcanvas16 &c, const char *text, int x, int y, uint8_t scale, uint16_t color);
