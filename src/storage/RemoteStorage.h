#pragma once

#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>
#include <cmath>

struct RemoteButton {
  String name;
  // Primary binding — On for stateful/toggle; sole binding for stateless
  String signalId;   // RF listen + optional TX
  String actionId;   // TX Action
  // Secondary binding — Off for stateful/toggle; unused for stateless
  String secondarySignalId;
  String secondaryActionId;
  // HomeKit: stateless | stateful | toggle
  String homekitMode = "stateless";

  bool hasPrimarySignal() const { return signalId.length() > 0; }
  bool hasPrimaryAction() const { return actionId.length() > 0; }
  bool hasPrimary() const { return hasPrimarySignal() || hasPrimaryAction(); }
  bool hasSecondarySignal() const { return secondarySignalId.length() > 0; }
  bool hasSecondaryAction() const { return secondaryActionId.length() > 0; }
  bool hasSecondary() const {
    return hasSecondarySignal() || hasSecondaryAction();
  }
  bool hasSignal() const { return hasPrimarySignal() || hasSecondarySignal(); }
  bool hasAction() const { return hasPrimaryAction() || hasSecondaryAction(); }
  bool hasBinding() const { return hasPrimary() || hasSecondary(); }
};

struct RfRemote {
  String id;
  String name;
  String type = "remote";  // remote | sensor | temperature | humidity

  std::vector<RemoteButton> buttons;

  String weatherProtocol = "auto";
  int weatherId = -1;
  int weatherChannel = 0;

  float lastTempC = NAN;
  float lastHumidity = NAN;
  uint32_t lastWeatherMs = 0;

  bool isRemote() const { return type == "remote" || type.isEmpty(); }
  bool isSensor() const { return type == "sensor"; }
  bool isTemperature() const { return type == "temperature"; }
  bool isHumidity() const { return type == "humidity"; }
  bool isWeather() const { return isTemperature() || isHumidity(); }

  void toJson(JsonObject obj) const {
    obj["id"] = id;
    obj["name"] = name;
    obj["type"] = type.length() ? type : "remote";
    JsonArray arr = obj["buttons"].to<JsonArray>();
    for (const auto& b : buttons) {
      JsonObject o = arr.add<JsonObject>();
      o["name"] = b.name;
      o["signalId"] = b.signalId;
      o["actionId"] = b.actionId;
      o["secondarySignalId"] = b.secondarySignalId;
      o["secondaryActionId"] = b.secondaryActionId;
      // Aliases for older UI / clarity
      o["actionIdOn"] = b.actionId;
      o["actionIdOff"] = b.secondaryActionId;
      o["homekitMode"] = b.homekitMode.length() ? b.homekitMode : "stateless";
    }
    if (isWeather()) {
      obj["weatherProtocol"] = weatherProtocol;
      obj["weatherId"] = weatherId;
      obj["weatherChannel"] = weatherChannel;
      if (!isnan(lastTempC)) obj["lastTempC"] = lastTempC;
      if (!isnan(lastHumidity)) obj["lastHumidity"] = lastHumidity;
      obj["lastWeatherAgeMs"] = lastWeatherMs ? (int)(millis() - lastWeatherMs) : -1;
    }
  }

  bool fromJson(JsonVariantConst v) {
    if (!v.is<JsonObjectConst>()) return false;
    JsonObjectConst obj = v.as<JsonObjectConst>();
    id = obj["id"] | "";
    name = obj["name"] | "Device";
    type = obj["type"] | "remote";
    if (type.isEmpty()) type = "remote";
    buttons.clear();
    if (obj["buttons"].is<JsonArrayConst>()) {
      for (JsonVariantConst bv : obj["buttons"].as<JsonArrayConst>()) {
        RemoteButton b;
        b.name = bv["name"] | "Button";
        b.signalId = bv["signalId"] | "";
        b.actionId = bv["actionId"] | "";
        b.secondarySignalId = bv["secondarySignalId"] | "";
        b.secondaryActionId = bv["secondaryActionId"] | "";
        b.homekitMode = bv["homekitMode"] | "stateless";
        if (b.homekitMode != "stateful" && b.homekitMode != "toggle") {
          b.homekitMode = "stateless";
        }

        // Migrate legacy actionIdOn / actionIdOff
        String legacyOn = bv["actionIdOn"] | "";
        String legacyOff = bv["actionIdOff"] | "";
        if (!b.actionId.length() && legacyOn.length()) b.actionId = legacyOn;
        if (!b.secondaryActionId.length() && legacyOff.length()) {
          b.secondaryActionId = legacyOff;
        }

        // Prefer action over signal on the same slot
        if (b.actionId.length()) b.signalId = "";
        if (b.secondaryActionId.length()) b.secondarySignalId = "";

        // Stateless never keeps a secondary binding
        if (b.homekitMode == "stateless") {
          b.secondarySignalId = "";
          b.secondaryActionId = "";
        }

        if (b.hasBinding()) buttons.push_back(b);
      }
    }
    weatherProtocol = obj["weatherProtocol"] | "auto";
    weatherId = obj["weatherId"] | -1;
    weatherChannel = obj["weatherChannel"] | 0;
    if (!obj["lastTempC"].isNull()) lastTempC = obj["lastTempC"].as<float>();
    if (!obj["lastHumidity"].isNull()) lastHumidity = obj["lastHumidity"].as<float>();
    return id.length() > 0 || name.length() > 0;
  }
};

class RemoteStorage {
 public:
  bool begin();
  std::vector<RfRemote> list() const;
  bool load(const String& id, RfRemote& out) const;
  bool save(RfRemote& remote);
  bool remove(const String& id);
  bool exists(const String& id) const;
  String allocateId() const;

 private:
  String pathFor(const String& id) const;
  static constexpr const char* kDir = "/remotes";
};
