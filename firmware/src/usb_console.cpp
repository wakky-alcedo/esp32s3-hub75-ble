#include "usb_console.h"
#include <hal/usb_serial_jtag_ll.h>

UsbConsole Console;

// A host with the port open picks up the IN packet within a frame or two.
static const uint32_t TX_WAIT_MS = 50;

void UsbConsole::begin() {
  if (!txLock_) txLock_ = xSemaphoreCreateMutex();
}

// Moves one byte from the OUT FIFO into peeked_. Unread data stays in the
// FIFO and the host is NAKed meanwhile, so nothing is lost while we are busy.
bool UsbConsole::fill() {
  if (peeked_ >= 0) return true;
  uint8_t b;
  if (usb_serial_jtag_ll_read_rxfifo(&b, 1) != 1) return false;
  peeked_ = b;
  return true;
}

int UsbConsole::available() { return fill() ? 1 : 0; }

int UsbConsole::peek() { return fill() ? peeked_ : -1; }

int UsbConsole::read() {
  if (!fill()) return -1;
  int b = peeked_;
  peeked_ = -1;
  return b;
}

size_t UsbConsole::write(const uint8_t *buf, size_t len) {
  if (!txLock_ || xSemaphoreTake(txLock_, pdMS_TO_TICKS(TX_WAIT_MS)) != pdTRUE) return 0;
  size_t done = 0;
  uint32_t waitFrom = millis();
  while (done < len) {
    uint32_t n = usb_serial_jtag_ll_write_txfifo(buf + done, len - done);
    if (n) {
      done += n;
      stalled_ = false;
      waitFrom = millis();
      continue;
    }
    // FIFO full: send what is in it and wait for the host to take it.
    usb_serial_jtag_ll_txfifo_flush();
    if (stalled_ || millis() - waitFrom >= TX_WAIT_MS) {
      stalled_ = true;  // nobody is reading; drop the rest without waiting
      break;
    }
    delay(1);
  }
  usb_serial_jtag_ll_txfifo_flush();
  xSemaphoreGive(txLock_);
  return len;
}

void UsbConsole::flush() {
  uint32_t from = millis();
  while (!stalled_ && !usb_serial_jtag_ll_txfifo_writable() && millis() - from < TX_WAIT_MS) delay(1);
}
