#pragma once
#include <Arduino.h>

// On-board PCF85063 RTC (I2C SDA=47, SCL=48). It keeps UTC; without a backup
// battery it loses time on power-off and reports itself invalid.

// Sets the system clock from the RTC if the RTC holds a valid time.
bool rtcBegin();
// Sets both the system clock and the RTC.
void rtcSetTime(time_t utc);
bool rtcPresent();
