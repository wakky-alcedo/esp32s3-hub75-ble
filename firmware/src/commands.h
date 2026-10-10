#pragma once
#include <ArduinoJson.h>

static const char *FW_VERSION = "0.1.0";
static const int PROTOCOL_VERSION = 1;

// Executes one JSON command ({"cmd": "...", "id": n, ...}) and fills resp.
// Shared by the serial console and the BLE control characteristic.
void handleCommand(JsonObjectConst req, JsonDocument &resp);

// Set by commands that need a reboot once the response has been sent.
extern volatile bool gRebootRequested;
// Hook for BLE bond clearing (set by the BLE module).
extern void (*gClearBondsHook)();
