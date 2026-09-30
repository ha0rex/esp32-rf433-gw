#pragma once

#include <Arduino.h>
#include <vector>
#include <cmath>

struct WeatherReading {
  bool valid = false;
  String protocol;   // nexus | gt_wt_02 | fineoffset_wh2 | rubicson | infactory | esperanza
  int sensorId = 0;
  int channel = 0;   // 1..8 when known, 0 if N/A
  bool batteryLow = false;
  float tempC = NAN;
  float humidity = NAN;  // 0..100, NAN if unknown / temp-only
  int8_t rssiDbm = -128;
  uint32_t atMs = 0;
};

class WeatherDecoder {
 public:
  // Try known OOK weather-station bit layouts (and inverted polarity).
  static WeatherReading decodeBits(const String& bits, int8_t rssiDbm = -128);

  // Decode from a pulse capture (splits frames, tries each).
  static WeatherReading decodePulses(const std::vector<int32_t>& pulses,
                                     int8_t rssiDbm = -128);

 private:
  static WeatherReading tryNexus(const String& bits, int8_t rssi);
  static WeatherReading tryGtWt02(const String& bits, int8_t rssi);
  static WeatherReading tryFineoffsetWh2(const String& bits, int8_t rssi);
  static WeatherReading tryRubicson(const String& bits, int8_t rssi);
  static WeatherReading tryInfactory(const String& bits, int8_t rssi);
  static WeatherReading tryEsperanza(const String& bits, int8_t rssi);
  static WeatherReading tryAll(const String& bits, int8_t rssi);

  static bool packBits(const String& bits, int start, int nBits, uint8_t* out,
                       int outBytes);
  static uint8_t crc8(const uint8_t* data, int len, uint8_t poly, uint8_t init);
  static uint8_t crc4(const uint8_t* data, int len, uint8_t poly, uint8_t init);
};
