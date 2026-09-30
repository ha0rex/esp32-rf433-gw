#include "WeatherDecoder.h"
#include "RFAnalyzer.h"
#include <algorithm>
#include <cstring>
#include <cmath>

bool WeatherDecoder::packBits(const String& bits, int start, int nBits,
                              uint8_t* out, int outBytes) {
  if (start < 0 || nBits <= 0 || !out || outBytes <= 0) return false;
  if (start + nBits > (int)bits.length()) return false;
  memset(out, 0, outBytes);
  for (int i = 0; i < nBits; i++) {
    char c = bits[start + i];
    if (c != '0' && c != '1') return false;
    if (c == '1') {
      int byte = i / 8;
      int bit = 7 - (i % 8);
      if (byte >= outBytes) return false;
      out[byte] |= (1u << bit);
    }
  }
  return true;
}

uint8_t WeatherDecoder::crc8(const uint8_t* data, int len, uint8_t poly,
                             uint8_t init) {
  uint8_t crc = init;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      if (crc & 0x80) crc = (uint8_t)((crc << 1) ^ poly);
      else crc <<= 1;
    }
  }
  return crc;
}

// rtl_433 crc4: processes high nibble first within each byte.
uint8_t WeatherDecoder::crc4(const uint8_t* data, int len, uint8_t poly,
                             uint8_t init) {
  uint8_t crc = init;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      if (crc & 0x80) crc = (uint8_t)((crc << 1) ^ (poly << 4));
      else crc <<= 1;
    }
  }
  return (uint8_t)(crc >> 4);
}

static WeatherReading finish(WeatherReading r, const char* proto, int id,
                             int ch, bool battLow, float tempC, float hum,
                             int8_t rssi) {
  r.valid = true;
  r.protocol = proto;
  r.sensorId = id;
  r.channel = ch;
  r.batteryLow = battLow;
  r.tempC = tempC;
  r.humidity = hum;
  r.rssiDbm = rssi;
  r.atMs = millis();
  return r;
}

WeatherReading WeatherDecoder::tryNexus(const String& bits, int8_t rssi) {
  WeatherReading r;
  if (bits.length() < 36) return r;

  for (int start = 0; start + 36 <= (int)bits.length() && start < 12; start++) {
    uint8_t b[5] = {};
    if (!packBits(bits, start, 36, b, 5)) continue;
    if ((b[3] & 0xf0) != 0xf0) continue;
    if ((b[1] & 0x30) == 0x30) continue;
    if ((b[0] == 0 && b[2] == 0 && b[3] == 0) ||
        (b[0] == 0xff && b[2] == 0xff && b[3] == 0xff)) continue;

    // Rubicson shares the layout; reject if Rubicson CRC validates
    {
      uint8_t tmp[5];
      tmp[0] = b[0];
      tmp[1] = b[1];
      tmp[2] = b[2];
      tmp[3] = b[3] & 0xf0;
      tmp[4] = (uint8_t)(((b[3] & 0x0f) << 4) | (b[4] >> 4));
      if (crc8(tmp, 5, 0x31, 0x6c) == 0) continue;
    }

    int id = b[0];
    bool batteryOk = (b[1] & 0x80) != 0;
    int channel = ((b[1] & 0x30) >> 4) + 1;
    int16_t tempRaw = (int16_t)(((uint16_t)b[1] << 12) | ((uint16_t)b[2] << 4));
    float tempC = (tempRaw >> 4) * 0.1f;
    int humidity = ((b[3] & 0x0f) << 4) | (b[4] >> 4);

    if (tempC < -40.f || tempC > 70.f) continue;
    if (humidity > 100) continue;

    return finish(r, "nexus", id, channel, !batteryOk, tempC, (float)humidity,
                  rssi);
  }
  return r;
}

WeatherReading WeatherDecoder::tryGtWt02(const String& bits, int8_t rssi) {
  WeatherReading r;
  if (bits.length() < 37) return r;

  for (int start = 0; start + 37 <= (int)bits.length() && start < 12; start++) {
    uint8_t b[5] = {};
    if (!packBits(bits, start, 37, b, 5)) continue;
    if (!(b[0] || b[1] || b[2] || b[3] || b[4])) continue;

    int sum = (b[0] >> 4) + (b[0] & 0xF) + (b[1] >> 4) + (b[1] & 0xF) +
              (b[2] >> 4) + (b[2] & 0xF) + (b[3] >> 4) + (b[3] & 0xe);
    int checksum = ((b[3] & 1) << 5) + (b[4] >> 3);
    if ((sum & 0x3F) != checksum) continue;

    int humidity = (b[3] >> 1) & 0x7F;
    if (humidity <= 10) humidity = 0;
    else if (humidity > 90) humidity = 100;

    int sensorId = b[0];
    bool batteryLow = (b[1] >> 7) & 1;
    int channel = ((b[1] >> 4) & 3) + 1;
    int16_t tempRaw =
        (int16_t)((((uint16_t)(b[1] & 0x0f)) << 12) | ((uint16_t)b[2] << 4));
    float tempC = (tempRaw >> 4) * 0.1f;

    if (tempC < -40.f || tempC > 70.f) continue;
    if (channel < 1 || channel > 3) continue;

    return finish(r, "gt_wt_02", sensorId, channel, batteryLow, tempC,
                  (float)humidity, rssi);
  }
  return r;
}

WeatherReading WeatherDecoder::tryFineoffsetWh2(const String& bits, int8_t rssi) {
  WeatherReading r;
  if (bits.length() < 40) return r;

  auto decodePayload = [&](uint8_t* b, bool wh5, bool battInTemp) -> WeatherReading {
    WeatherReading out;
    if (crc8(b, 4, 0x31, 0) != b[4]) return out;
    int type = b[0] >> 4;
    if (type != 0x4 && type != 0x5 && type != 0x6) return out;

    int id = ((b[0] & 0x0f) << 4) | (b[1] >> 4);
    int temp = ((b[1] & 0x0f) << 8) | b[2];
    bool batteryLow = false;
    float tempC;
    if (wh5 || battInTemp) {
      if (battInTemp) {
        batteryLow = (temp & 0x800) != 0;
        temp &= 0x7FF;
      }
      tempC = (temp - 400) * 0.1f;
    } else {
      if (temp & 0x800) {
        temp &= 0x7FF;
        temp = -temp;
      }
      tempC = temp * 0.1f;
    }
    int humidity = b[3];
    if (tempC < -40.f || tempC > 70.f) return out;
    if (humidity > 100 && humidity != 0xff) return out;

    float hum = (humidity == 0xff) ? NAN : (float)humidity;
    return finish(out, wh5 ? "fineoffset_wh5" : "fineoffset_wh2", id, 0,
                  batteryLow, tempC, hum, rssi);
  };

  for (int start = 0; start + 40 <= (int)bits.length() && start < 16; start++) {
    // WH2: 48 bits starting with 0xFF, payload after 8-bit preamble
    if (start + 48 <= (int)bits.length()) {
      uint8_t full[6] = {};
      if (packBits(bits, start, 48, full, 6) && full[0] == 0xFF) {
        uint8_t b[5] = {full[1], full[2], full[3], full[4], full[5]};
        WeatherReading got = decodePayload(b, false, false);
        if (got.valid) return got;
      }
    }
    // WH5 / WH2A: 47 bits starting with 0xFE, payload after 7-bit shift
    if (start + 47 <= (int)bits.length()) {
      uint8_t full[6] = {};
      if (packBits(bits, start, 47, full, 6) && full[0] == 0xFE) {
        // extract 40 bits starting at bit 7
        uint8_t b[5] = {};
        if (packBits(bits, start + 7, 40, b, 5)) {
          WeatherReading got = decodePayload(b, true, false);
          if (got.valid) return got;
        }
      }
    }
    // Bare 40-bit payload (preamble already stripped)
    uint8_t b[5] = {};
    if (!packBits(bits, start, 40, b, 5)) continue;
    WeatherReading got = decodePayload(b, false, false);
    if (got.valid) return got;
  }
  return r;
}

WeatherReading WeatherDecoder::tryRubicson(const String& bits, int8_t rssi) {
  WeatherReading r;
  if (bits.length() < 36) return r;

  for (int start = 0; start + 36 <= (int)bits.length() && start < 12; start++) {
    uint8_t b[5] = {};
    if (!packBits(bits, start, 36, b, 5)) continue;
    if ((b[3] & 0xf0) != 0xf0) continue;

    // rtl_433 rubicson CRC packing
    uint8_t tmp[5];
    tmp[0] = b[0];
    tmp[1] = b[1];
    tmp[2] = b[2];
    tmp[3] = b[3] & 0xf0;
    tmp[4] = (uint8_t)(((b[3] & 0x0f) << 4) | ((b[4] & 0xf0) >> 4));
    if (crc8(tmp, 5, 0x31, 0x6c) != 0) continue;

    int id = b[0];
    bool batteryOk = (b[1] & 0x80) != 0;
    int channel = ((b[1] & 0x30) >> 4) + 1;
    int16_t tempRaw = (int16_t)(((uint16_t)b[1] << 12) | ((uint16_t)b[2] << 4));
    float tempC = (tempRaw >> 4) * 0.1f;
    if (tempC < -40.f || tempC > 70.f) continue;
    if (channel < 1 || channel > 4) continue;

    return finish(r, "rubicson", id, channel, !batteryOk, tempC, NAN, rssi);
  }
  return r;
}

WeatherReading WeatherDecoder::tryInfactory(const String& bits, int8_t rssi) {
  WeatherReading r;
  if (bits.length() < 40) return r;

  for (int start = 0; start + 40 <= (int)bits.length() && start < 12; start++) {
    uint8_t b[5] = {};
    if (!packBits(bits, start, 40, b, 5)) continue;
    if (!(b[4] & 0x0F)) continue;

    // rtl_433 infactory_crc_check
    uint8_t msg[5];
    memcpy(msg, b, 5);
    uint8_t msg_crc = msg[1] >> 4;
    msg[1] = (uint8_t)((msg[1] & 0x0F) | ((msg[4] & 0x0F) << 4));
    uint8_t crc = crc4(msg, 4, 0x13, 0);
    crc ^= (msg[4] >> 4);
    if (crc != msg_crc) continue;

    int id = b[0];
    bool batteryLow = (b[1] >> 2) & 1;
    int tempRaw = (b[2] << 4) | (b[3] >> 4);
    int humidity = (b[3] & 0x0F) * 10 + (b[4] >> 4);  // BCD; A0 = 100%
    if ((b[3] & 0x0F) == 0x0A) humidity = 100;
    int channel = b[4] & 0x03;
    float tempF = (tempRaw - 900) * 0.1f;
    float tempC = (tempF - 32.f) * (5.f / 9.f);

    if (tempC < -40.f || tempC > 70.f) continue;
    if (humidity > 100) continue;
    if (channel < 1 || channel > 3) continue;

    return finish(r, "infactory", id, channel, batteryLow, tempC,
                  (float)humidity, rssi);
  }
  return r;
}

WeatherReading WeatherDecoder::tryEsperanza(const String& bits, int8_t rssi) {
  // Esperanza EWS / kedsum / s3318p — 40 bits, CRC-4 poly 0x3
  WeatherReading r;
  if (bits.length() < 40) return r;

  for (int start = 0; start + 40 <= (int)bits.length() && start < 16; start++) {
    uint8_t b[5] = {};
    // Some captures include 2 leading 0-bits before the payload
    int offset = start;
    if (start + 42 <= (int)bits.length() &&
        bits[start] == '0' && bits[start + 1] == '0') {
      offset = start + 2;
    }
    if (!packBits(bits, offset, 40, b, 5)) continue;

    int crc = crc4(b, 4, 0x3, 0x0) ^ (b[4] >> 4);
    if (crc != (b[4] & 0x0f)) continue;

    int id = b[0];
    int channel = ((b[1] & 0x30) >> 4) + 1;
    bool batteryLow = (b[4] & 0x40) != 0;
    int tempRaw = ((b[2] & 0x0f) << 8) | (b[2] & 0xf0) | (b[1] & 0x0f);
    float tempF = (tempRaw - 900) * 0.1f;
    float tempC = (tempF - 32.f) * (5.f / 9.f);
    int humidity = ((b[3] & 0x0f) << 4) | ((b[3] & 0xf0) >> 4);

    if (tempC < -40.f || tempC > 70.f) continue;
    if (humidity > 100) continue;
    if (channel < 1 || channel > 3) continue;

    return finish(r, "esperanza", id, channel, batteryLow, tempC,
                  (float)humidity, rssi);
  }
  return r;
}

WeatherReading WeatherDecoder::tryAll(const String& bits, int8_t rssi) {
  // Prefer CRC-validated protocols before heuristic Nexus
  WeatherReading r;
  r = tryGtWt02(bits, rssi);
  if (r.valid) return r;
  r = tryFineoffsetWh2(bits, rssi);
  if (r.valid) return r;
  r = tryRubicson(bits, rssi);
  if (r.valid) return r;
  r = tryInfactory(bits, rssi);
  if (r.valid) return r;
  r = tryEsperanza(bits, rssi);
  if (r.valid) return r;
  r = tryNexus(bits, rssi);
  if (r.valid) return r;
  return r;
}

WeatherReading WeatherDecoder::decodeBits(const String& bits, int8_t rssiDbm) {
  WeatherReading r = tryAll(bits, rssiDbm);
  if (r.valid) return r;

  String inv;
  inv.reserve(bits.length());
  for (size_t i = 0; i < bits.length(); i++) {
    inv += (bits[i] == '1') ? '0' : (bits[i] == '0') ? '1' : bits[i];
  }
  return tryAll(inv, rssiDbm);
}

WeatherReading WeatherDecoder::decodePulses(const std::vector<int32_t>& pulses,
                                           int8_t rssiDbm) {
  WeatherReading best;
  auto frames = RFAnalyzer::splitFrames(pulses, 4000);
  if (frames.size() < 2) {
    auto alt = RFAnalyzer::splitFrames(pulses, 3000);
    if (alt.size() > frames.size()) frames = std::move(alt);
  }
  if (frames.size() < 2) {
    auto alt = RFAnalyzer::splitFrames(pulses, 8000);
    if (alt.size() > frames.size()) frames = std::move(alt);
  }
  if (frames.empty()) frames.push_back(pulses);

  for (const auto& fr : frames) {
    if (fr.size() < 40) continue;
    uint32_t mid = 0;
    String bits = RFAnalyzer::decodeOokBits(fr, &mid, 0);
    if (bits.length() < 36) continue;
    WeatherReading r = decodeBits(bits, rssiDbm);
    if (r.valid) {
      if (!best.valid ||
          (!isnan(r.humidity) && isnan(best.humidity)) ||
          r.rssiDbm > best.rssiDbm) {
        best = r;
      }
    }
    delay(0);
  }
  return best;
}
