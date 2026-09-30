#pragma once

#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>

// One step in an Action sequence.
//  tx    — play signal once with N repeats (default = signal's estimatedRepeats)
//  hold  — press & hold: keep transmitting signal for holdSec seconds
//  delay — wait delaySec seconds
struct ActionStep {
  String type = "tx";  // tx | hold | delay
  String signalId;
  uint32_t repeats = 0;   // 0 = use signal default (tx only)
  float delaySec = 0;
  float holdSec = 0;

  void toJson(JsonObject o) const {
    o["type"] = type;
    if (type == "tx" || type == "hold") o["signalId"] = signalId;
    if (type == "tx") o["repeats"] = (int)repeats;
    if (type == "delay") o["delaySec"] = delaySec;
    if (type == "hold") o["holdSec"] = holdSec;
  }

  bool fromJson(JsonVariantConst v) {
    if (!v.is<JsonObjectConst>()) return false;
    JsonObjectConst o = v.as<JsonObjectConst>();
    type = o["type"] | "tx";
    signalId = o["signalId"] | "";
    repeats = o["repeats"] | 0;
    delaySec = o["delaySec"] | 0.f;
    holdSec = o["holdSec"] | 0.f;
    if (type != "tx" && type != "hold" && type != "delay") type = "tx";
    return true;
  }
};

struct RfAction {
  String id;
  String name;
  // HomeKit is attached via Devices (bind a button to this action), not here.
  std::vector<ActionStep> steps;

  void toJson(JsonObject obj) const {
    obj["id"] = id;
    obj["name"] = name;
    JsonArray arr = obj["steps"].to<JsonArray>();
    for (const auto& s : steps) {
      JsonObject o = arr.add<JsonObject>();
      s.toJson(o);
    }
  }

  bool fromJson(JsonVariantConst v) {
    if (!v.is<JsonObjectConst>()) return false;
    JsonObjectConst obj = v.as<JsonObjectConst>();
    id = obj["id"] | "";
    name = obj["name"] | "Action";
    steps.clear();
    if (obj["steps"].is<JsonArrayConst>()) {
      for (JsonVariantConst sv : obj["steps"].as<JsonArrayConst>()) {
        ActionStep s;
        if (s.fromJson(sv)) steps.push_back(s);
      }
    }
    return name.length() > 0;
  }
};

class ActionStorage {
 public:
  bool begin();
  std::vector<RfAction> list() const;
  bool load(const String& id, RfAction& out) const;
  bool save(RfAction& action);
  bool remove(const String& id);
  bool exists(const String& id) const;
  String allocateId() const;

 private:
  String pathFor(const String& id) const;
  static constexpr const char* kDir = "/actions";
};
