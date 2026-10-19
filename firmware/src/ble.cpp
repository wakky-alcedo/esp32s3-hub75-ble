#include "ble.h"
#include <NimBLEDevice.h>
#include "commands.h"
#include "player.h"
#include "trace.h"
#include "transfer.h"
#include "usb_console.h"

#define SERVICE_UUID "6e4c0001-7b3a-4f2e-9a61-4c45442d4453"
#define CONTROL_UUID "6e4c0002-7b3a-4f2e-9a61-4c45442d4453"
#define NOTIFY_UUID "6e4c0003-7b3a-4f2e-9a61-4c45442d4453"
#define BULK_UUID "6e4c0004-7b3a-4f2e-9a61-4c45442d4453"

static NimBLEServer *sServer = nullptr;
static NimBLECharacteristic *sNotify = nullptr;
static volatile bool sConnected = false;
static volatile uint16_t sMtu = 23;
static volatile bool sNackPending = false;

// Notifications larger than one ATT payload are split into fragments.
// Byte 0 of each fragment: 0x01 = more fragments follow, 0x00 = last.
static void sendFragmented(const String &text) {
  if (!sConnected || !sNotify) return;
  const size_t maxPayload = max<int>(20, sMtu - 3) - 1;
  size_t off = 0;
  uint8_t buf[256];
  do {
    size_t n = min(maxPayload, text.length() - off);
    buf[0] = off + n < text.length() ? 0x01 : 0x00;
    memcpy(buf + 1, text.c_str() + off, n);
    sNotify->setValue(buf, n + 1);
    sNotify->notify();
    off += n;
  } while (off < text.length());
}

static void clearBonds() {
  NimBLEDevice::deleteAllBonds();
  Console.println("[ble] bonds cleared");
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, ble_gap_conn_desc *desc) override {
    TRACE_BLE(100);
    sConnected = true;
    Console.println("[ble] connected");
    TRACE_BLE(101);
    // Ask for a faster connection interval: 7.5-15ms, no latency, 4s timeout.
    s->updateConnParams(desc->conn_handle, 6, 12, 0, 400);
    TRACE_BLE(102);
  }
  void onDisconnect(NimBLEServer *s) override {
    TRACE_BLE(110);
    sConnected = false;
    sMtu = 23;
    Console.println("[ble] disconnected");
    {
      StateLock lock;
      gPlayer.setPinOverlay("");
    }
    gReceiver.abort();
    NimBLEDevice::startAdvertising();
    TRACE_BLE(111);
  }
  void onMTUChange(uint16_t mtu, ble_gap_conn_desc *) override {
    sMtu = mtu;
    Console.printf("[ble] mtu %u\n", mtu);
  }
  uint32_t onPassKeyRequest() override {
    // A fresh PIN per pairing attempt, shown on the panel.
    uint32_t pin = esp_random() % 1000000;
    char text[8];
    snprintf(text, sizeof(text), "%06u", pin);
    Console.printf("[ble] pairing PIN %s\n", text);
    StateLock lock;
    gPlayer.setPinOverlay(text);
    return pin;
  }
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    TRACE_BLE(130);
    Console.printf("[ble] auth complete: encrypted=%d bonded=%d\n", desc->sec_state.encrypted, desc->sec_state.bonded);
    StateLock lock;
    gPlayer.setPinOverlay("");
    if (!desc->sec_state.encrypted) gPlayer.toast("PAIR FAILED", 2000);
    TRACE_BLE(131);
  }
};

class ControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    TRACE_BLE(140);
    std::string v = c->getValue();
    JsonDocument req, resp;
    if (deserializeJson(req, v.data(), v.size())) {
      resp["ok"] = false;
      resp["error"] = "bad json";
    } else {
      handleCommand(req.as<JsonObjectConst>(), resp);
    }
    String out;
    serializeJson(resp, out);
    TRACE_BLE(141);
    sendFragmented(out);
    TRACE_BLE(142);
  }
};

class BulkCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    std::string v = c->getValue();
    if (v.size() < 4) return;
    const uint8_t *p = (const uint8_t *)v.data();
    uint32_t offset = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
    if (gReceiver.write(offset, p + 4, v.size() - 4)) {
      sNackPending = false;
    } else if (!sNackPending && gReceiver.active()) {
      // Report the gap once; the app resumes from "expected".
      sNackPending = true;
      JsonDocument evt;
      evt["evt"] = "file_nack";
      evt["expected"] = gReceiver.expected();
      String out;
      serializeJson(evt, out);
      sendFragmented(out);
    }
  }
};

void bleBegin(const char *name) {
  NimBLEDevice::init(name);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, true, true);  // bonding, MITM, secure connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  gClearBondsHook = clearBonds;

  sServer = NimBLEDevice::createServer();
  sServer->setCallbacks(new ServerCallbacks());
  NimBLEService *svc = sServer->createService(SERVICE_UUID);

  // Every characteristic requires an authenticated (PIN) bonded link.
  NimBLECharacteristic *control = svc->createCharacteristic(
      CONTROL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN, 512);
  control->setCallbacks(new ControlCallbacks());
  sNotify = svc->createCharacteristic(
      NOTIFY_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC |
                       NIMBLE_PROPERTY::READ_AUTHEN);
  NimBLECharacteristic *bulk = svc->createCharacteristic(
      BULK_UUID, NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN, 512);
  bulk->setCallbacks(new BulkCallbacks());
  svc->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->start();
  Console.printf("[ble] advertising as '%s' (%d bond(s))\n", name, NimBLEDevice::getNumBonds());
}

bool bleConnected() { return sConnected; }

void bleNotifyEvent(JsonDocument &evt) {
  String out;
  serializeJson(evt, out);
  sendFragmented(out);
}
