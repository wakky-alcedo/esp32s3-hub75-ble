#pragma once
#include <ArduinoJson.h>

// BLE GATT transport for the JSON command protocol (see docs/protocol.md).
void bleBegin(const char *name);
bool bleConnected();
// Pushes an unsolicited event ({"evt": ...}) to the connected app.
void bleNotifyEvent(JsonDocument &evt);
