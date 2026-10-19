#include "rtc.h"
#include "usb_console.h"
#include <Wire.h>
#include <sys/time.h>

static const uint8_t ADDR = 0x51;
static const uint8_t REG_SECONDS = 0x04;
static bool sPresent = false;

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

bool rtcPresent() { return sPresent; }

bool rtcBegin() {
  Wire.begin(47, 48);
  Wire.setClock(400000);
  Wire.beginTransmission(ADDR);
  sPresent = Wire.endTransmission() == 0;
  if (!sPresent) {
    Console.println("[rtc] not found");
    return false;
  }
  Wire.beginTransmission(ADDR);
  Wire.write(REG_SECONDS);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)ADDR, 7) != 7) return false;
  uint8_t r[7];
  for (int i = 0; i < 7; i++) r[i] = Wire.read();
  if (r[0] & 0x80) {
    // Oscillator stopped: power was lost, the stored time is meaningless.
    Console.println("[rtc] time invalid (oscillator stop flag)");
    return false;
  }
  int sec = bcd2bin(r[0] & 0x7F), min = bcd2bin(r[1] & 0x7F), hour = bcd2bin(r[2] & 0x3F);
  int day = bcd2bin(r[3] & 0x3F), month = bcd2bin(r[5] & 0x1F), year = 2000 + bcd2bin(r[6]);
  time_t t = daysFromCivil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
  struct timeval tv = {t, 0};
  settimeofday(&tv, nullptr);
  Console.printf("[rtc] %04d-%02d-%02d %02d:%02d:%02d UTC\n", year, month, day, hour, min, sec);
  return true;
}

void rtcSetTime(time_t utc) {
  struct timeval tv = {utc, 0};
  settimeofday(&tv, nullptr);
  if (!sPresent) return;
  struct tm t;
  gmtime_r(&utc, &t);
  Wire.beginTransmission(ADDR);
  Wire.write(REG_SECONDS);
  Wire.write(bin2bcd(t.tm_sec));  // also clears the oscillator stop flag
  Wire.write(bin2bcd(t.tm_min));
  Wire.write(bin2bcd(t.tm_hour));
  Wire.write(bin2bcd(t.tm_mday));
  Wire.write(t.tm_wday);
  Wire.write(bin2bcd(t.tm_mon + 1));
  Wire.write(bin2bcd(t.tm_year - 100));
  Wire.endTransmission();
}
