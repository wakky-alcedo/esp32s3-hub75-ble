#pragma once
#include <Arduino.h>

// Serial over the chip's built-in USB Serial/JTAG port, used instead of the
// core's HWCDC `Serial`. HWCDC guesses whether a host is listening and gets
// that wrong after the host closes and reopens the port: the device keeps
// receiving commands but never sends anything again until it is reset (which
// also made every other firmware upload fail). This driver just polls the
// endpoint FIFOs. Output written while no host is reading is dropped; the
// next host to open the port may get up to one stale 64-byte packet first.
class UsbConsole : public Stream {
 public:
  void begin();
  int available() override;
  int read() override;
  int peek() override;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *buf, size_t len) override;
  void flush() override;
  using Print::write;

 private:
  bool fill();
  int peeked_ = -1;
  bool stalled_ = false;
  SemaphoreHandle_t txLock_ = nullptr;
};

extern UsbConsole Console;
