#pragma once

#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>

struct TimingCluster {
  uint32_t centerUs = 0;
  uint32_t count = 0;
  uint32_t minUs = 0;
  uint32_t maxUs = 0;
};

struct SignalAnalysis {
  bool valid = false;
  uint32_t shortPulseUs = 0;
  uint32_t longPulseUs = 0;
  uint32_t syncPulseUs = 0;
  uint32_t syncGapUs = 0;
  uint32_t frameDurationUs = 0;
  uint32_t estimatedRepeats = 0;
  uint32_t pulseCount = 0;
  uint32_t durationUs = 0;
  std::vector<TimingCluster> clusters;
  String heuristicBits;          // guessed 0/1 string
  String heuristicNote;          // explanation / disclaimer
};

struct RFSignal {
  String id;
  String name;
  float frequencyMHz = 433.92f;
  uint8_t modulation = 2;  // ASK/OOK
  int8_t rssiDbm = -128;
  uint32_t timestamp = 0;
  uint32_t pulseCount = 0;
  uint32_t durationUs = 0;
  uint32_t estimatedRepeats = 1;
  uint32_t gapBetweenRepeatsUs = 10000;
  std::vector<int32_t> pulses;  // signed: +HIGH us, -LOW us
  std::vector<String> bitCodes; // one or more OOK bit fingerprints from pairing presses
  uint16_t bitMidUs = 0;       // short/long decision threshold used for bitCodes
  SignalAnalysis analysis;

  void clear() {
    id = "";
    name = "";
    frequencyMHz = 433.92f;
    modulation = 2;
    rssiDbm = -128;
    timestamp = 0;
    pulseCount = 0;
    durationUs = 0;
    estimatedRepeats = 1;
    gapBetweenRepeatsUs = 10000;
    pulses.clear();
    bitCodes.clear();
    bitMidUs = 0;
    analysis = SignalAnalysis{};
  }

  void toJson(JsonObject obj, bool includePulses = true) const {
    obj["id"] = id;
    obj["name"] = name;
    obj["frequency"] = frequencyMHz;
    obj["modulation"] = modulation;
    obj["rssi"] = rssiDbm;
    obj["timestamp"] = timestamp;
    obj["pulseCount"] = pulseCount;
    obj["durationUs"] = durationUs;
    obj["estimatedRepeats"] = estimatedRepeats;
    obj["gapBetweenRepeatsUs"] = gapBetweenRepeatsUs;

    if (includePulses) {
      JsonArray arr = obj["pulses"].to<JsonArray>();
      for (int32_t p : pulses) {
        arr.add(p);
      }
    }

    if (!bitCodes.empty()) {
      JsonArray bc = obj["bitCodes"].to<JsonArray>();
      for (const auto& b : bitCodes) bc.add(b);
    }
    if (bitMidUs) obj["bitMidUs"] = bitMidUs;

    JsonObject a = obj["analysis"].to<JsonObject>();
    a["valid"] = analysis.valid;
    a["shortPulseUs"] = analysis.shortPulseUs;
    a["longPulseUs"] = analysis.longPulseUs;
    a["syncPulseUs"] = analysis.syncPulseUs;
    a["syncGapUs"] = analysis.syncGapUs;
    a["frameDurationUs"] = analysis.frameDurationUs;
    a["estimatedRepeats"] = analysis.estimatedRepeats;
    a["pulseCount"] = analysis.pulseCount;
    a["durationUs"] = analysis.durationUs;
    a["heuristicBits"] = analysis.heuristicBits;
    a["heuristicNote"] = analysis.heuristicNote;
    JsonArray clusters = a["clusters"].to<JsonArray>();
    for (const auto& c : analysis.clusters) {
      JsonObject co = clusters.add<JsonObject>();
      co["centerUs"] = c.centerUs;
      co["count"] = c.count;
      co["minUs"] = c.minUs;
      co["maxUs"] = c.maxUs;
    }
  }

  bool fromJson(JsonVariantConst v) {
    clear();
    if (!v.is<JsonObjectConst>()) return false;
    JsonObjectConst obj = v.as<JsonObjectConst>();
    id = obj["id"] | "";
    name = obj["name"] | "Unnamed";
    frequencyMHz = obj["frequency"] | 433.92f;
    modulation = obj["modulation"] | 2;
    rssiDbm = obj["rssi"] | -128;
    timestamp = obj["timestamp"] | 0;
    pulseCount = obj["pulseCount"] | 0;
    durationUs = obj["durationUs"] | 0;
    estimatedRepeats = obj["estimatedRepeats"] | 1;
    gapBetweenRepeatsUs = obj["gapBetweenRepeatsUs"] | 10000;

    if (obj["pulses"].is<JsonArrayConst>()) {
      for (JsonVariantConst p : obj["pulses"].as<JsonArrayConst>()) {
        pulses.push_back(p.as<int32_t>());
      }
      pulseCount = pulses.size();
    }
    if (obj["bitCodes"].is<JsonArrayConst>()) {
      for (JsonVariantConst b : obj["bitCodes"].as<JsonArrayConst>()) {
        String s = b.as<const char*>();
        if (s.length() >= 24) bitCodes.push_back(s);
      }
    }
    bitMidUs = obj["bitMidUs"] | 0;
    return !pulses.empty() || !id.isEmpty();
  }

  String pulsesCsv() const {
    String s;
    s.reserve(pulses.size() * 6);
    for (size_t i = 0; i < pulses.size(); i++) {
      if (i) s += ',';
      s += String(pulses[i]);
    }
    return s;
  }

  bool parsePulsesCsv(const String& csv) {
    pulses.clear();
    int start = 0;
    while (start < (int)csv.length()) {
      int comma = csv.indexOf(',', start);
      if (comma < 0) comma = csv.length();
      String token = csv.substring(start, comma);
      token.trim();
      if (token.length()) {
        pulses.push_back(token.toInt());
      }
      start = comma + 1;
    }
    pulseCount = pulses.size();
    durationUs = 0;
    for (int32_t p : pulses) {
      durationUs += (uint32_t)abs(p);
    }
    return !pulses.empty();
  }
};
