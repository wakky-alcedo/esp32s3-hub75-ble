#include "assets.h"
#include "color.h"
#include "fsutil.h"

void Bitmap::release() {
  free(data);
  data = nullptr;
  w = h = 0;
}

static size_t bytesPerPixel(uint8_t format) {
  switch (format) {
    case BMP_RGB565: return 2;
    case BMP_RGB565A8: return 3;
    case BMP_A8: return 1;
    default: return 0;
  }
}

bool Bitmap::alloc(uint16_t nw, uint16_t nh, uint8_t fmt) {
  release();
  size_t bpp = bytesPerPixel(fmt);
  if (!bpp || !nw || !nh) return false;
  data = (uint8_t *)ps_calloc((size_t)nw * nh, bpp);
  if (!data) return false;
  w = nw;
  h = nh;
  format = fmt;
  return true;
}

bool Bitmap::load(const char *path) {
  release();
  size_t len = 0;
  uint8_t *file = fsReadAlloc(path, len);
  if (!file) return false;
  bool ok = false;
  if (len >= 12 && memcmp(file, "LDB1", 4) == 0) {
    uint16_t fw = file[4] | (file[5] << 8);
    uint16_t fh = file[6] | (file[7] << 8);
    uint8_t fmt = file[8];
    size_t need = (size_t)fw * fh * bytesPerPixel(fmt);
    if (need && len >= 12 + need) {
      // Shift the pixels to the start of the buffer to avoid a second copy.
      memmove(file, file + 12, need);
      data = file;
      w = fw;
      h = fh;
      format = fmt;
      ok = true;
    }
  }
  if (!ok) {
    free(file);
    Serial.printf("[assets] bad bitmap %s\n", path);
  }
  return ok;
}

bool Bitmap::fromText(const char *text, uint8_t scale) {
  scale = max<uint8_t>(scale, 1);
  int16_t tw = builtinTextWidth(text, scale);
  int16_t th = 8 * scale;
  if (tw <= 0) return false;
  GFXcanvas1 mono(tw, th);
  if (!mono.getBuffer()) return false;
  mono.setTextWrap(false);
  mono.setTextSize(scale);
  mono.setTextColor(1);
  mono.setCursor(0, 0);
  mono.print(text);
  if (!alloc(tw, th, BMP_A8)) return false;
  for (int y = 0; y < th; y++)
    for (int x = 0; x < tw; x++) data[y * tw + x] = mono.getPixel(x, y) ? 255 : 0;
  return true;
}

void drawBitmap(GFXcanvas16 &c, const Bitmap &bmp, int x, int y, const Tint &tint) {
  if (!bmp.valid()) return;
  const int cw = c.width(), ch = c.height();
  int x0 = max(0, x), y0 = max(0, y);
  int x1 = min(cw, x + bmp.w), y1 = min(ch, y + bmp.h);
  if (x0 >= x1 || y0 >= y1) return;
  uint16_t *dst = c.getBuffer();
  for (int py = y0; py < y1; py++) {
    int sy = py - y;
    uint16_t *row = dst + py * cw;
    for (int px = x0; px < x1; px++) {
      int sx = px - x;
      size_t si = (size_t)sy * bmp.w + sx;
      switch (bmp.format) {
        case BMP_RGB565: {
          const uint8_t *p = bmp.data + si * 2;
          row[px] = p[0] | (p[1] << 8);
          break;
        }
        case BMP_RGB565A8: {
          const uint8_t *p = bmp.data + si * 3;
          row[px] = blend565(row[px], p[0] | (p[1] << 8), p[2]);
          break;
        }
        case BMP_A8: {
          uint8_t a = bmp.data[si];
          if (!a) break;
          uint16_t col = tint.rainbow ? hsv565(tint.hueOffset + sx * 4, 255, 255) : tint.color;
          row[px] = blend565(row[px], col, a);
          break;
        }
      }
    }
  }
}

int16_t builtinTextWidth(const char *text, uint8_t scale) {
  // Classic 5x7 font advances 6px per character; drop the trailing gap.
  int n = strlen(text);
  return n ? (n * 6 - 1) * scale : 0;
}

void drawBuiltinText(GFXcanvas16 &c, const char *text, int x, int y, uint8_t scale, uint16_t color) {
  c.setFont(nullptr);
  c.setTextWrap(false);
  c.setTextSize(scale);
  c.setTextColor(color);
  c.setCursor(x, y);
  c.print(text);
}
