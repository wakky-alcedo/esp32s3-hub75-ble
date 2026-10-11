#pragma once
#include <stdint.h>

// Breadcrumbs kept in RTC memory: they survive the watchdog reset, so the
// next boot can report the last step each task reached before a hang.
extern volatile uint32_t gTraceMain;
extern volatile uint32_t gTraceBle;

#define TRACE_MAIN(n) (gTraceMain = (n))
#define TRACE_BLE(n) (gTraceBle = (n))
