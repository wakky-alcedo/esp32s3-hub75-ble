#pragma once
#include <Arduino.h>

// RGB565 helpers shared by the renderer, effects and assets.

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

inline void unpack565(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint8_t r5 = c >> 11, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
  r = (r5 << 3) | (r5 >> 2);
  g = (g6 << 2) | (g6 >> 4);
  b = (b5 << 3) | (b5 >> 2);
}

// a: 0 = bg only, 255 = fg only
inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a) {
  if (a == 255) return fg;
  if (a == 0) return bg;
  uint8_t br, bgc, bb, fr, fgc, fb;
  unpack565(bg, br, bgc, bb);
  unpack565(fg, fr, fgc, fb);
  uint8_t r = br + (((int)fr - br) * a >> 8);
  uint8_t g = bgc + (((int)fgc - bgc) * a >> 8);
  uint8_t b = bb + (((int)fb - bb) * a >> 8);
  return rgb565(r, g, b);
}

// s: 0..256 (256 = unchanged)
inline uint16_t scale565(uint16_t c, uint16_t s) {
  if (s >= 256) return c;
  uint32_t r = ((c >> 11) * s) >> 8;
  uint32_t g = (((c >> 5) & 0x3F) * s) >> 8;
  uint32_t b = ((c & 0x1F) * s) >> 8;
  return (r << 11) | (g << 5) | b;
}

inline uint16_t hsv565(uint8_t h, uint8_t s, uint8_t v) {
  uint8_t region = h / 43;
  uint8_t rem = (h - region * 43) * 6;
  uint8_t p = (v * (255 - s)) >> 8;
  uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
  uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
  switch (region) {
    case 0: return rgb565(v, t, p);
    case 1: return rgb565(q, v, p);
    case 2: return rgb565(p, v, t);
    case 3: return rgb565(p, q, v);
    case 4: return rgb565(t, p, v);
    default: return rgb565(v, p, q);
  }
}

// "#rrggbb" -> RGB565. Returns fallback on malformed input.
inline uint16_t parseColor(const char *s, uint16_t fallback) {
  if (!s || s[0] != '#' || strlen(s) != 7) return fallback;
  char *end;
  uint32_t v = strtoul(s + 1, &end, 16);
  if (*end) return fallback;
  return rgb565(v >> 16, v >> 8, v);
}
