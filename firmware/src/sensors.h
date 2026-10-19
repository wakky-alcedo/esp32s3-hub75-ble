#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// On-board environment and motion sensors, sampled on a background task:
// SHTC3 (temperature / humidity), QMI8658 (accelerometer / gyroscope) and
// the ESP32-S3's internal temperature sensor.

struct SensorData {
  bool envOk = false;  // SHTC3 answered
  bool imuOk = false;  // QMI8658 answered
  float tempC = 0, humidity = 0;
  float chipC = 0;
  // Acceleration in g and rotation in degrees/s, in screen coordinates:
  // +x right, +y down, +z out of the screen towards the viewer. With the
  // panel standing upright, gravity reads as about (0, +1, 0).
  float ax = 0, ay = 0, az = 0;
  float gx = 0, gy = 0, gz = 0;
  // Acceleration low-pass filtered (gravity direction) and how much the
  // board is being shaken (high-passed magnitude, g).
  float gravX = 0, gravY = 1, gravZ = 0;
  float shake = 0;
};

// Call after rtcBegin(), which starts the shared I2C bus.
void sensorsBegin();
SensorData sensorsGet();
// Takes the current direction of gravity as "down" on the physical panel and
// stores the matching imuRotation in gConfig (caller saves). False when the
// panel lies too flat to tell.
bool sensorsCalibrateDown();
void sensorsJson(JsonObject o);
// Overrides the gravity direction and shake level for ms, for testing the
// motion effects without moving the panel.
void sensorsSimulate(float gx, float gy, float gz, float shake, uint32_t ms);
