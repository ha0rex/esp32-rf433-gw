#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <vector>
#include <ArduinoJson.h>
#include "../models/RFSignal.h"

class SignalStorage {
 public:
  bool begin();
  std::vector<RFSignal> list(bool includePulses = false) const;
  bool load(const String& id, RFSignal& out) const;
  bool save(RFSignal& signal);
  bool remove(const String& id);
  bool rename(const String& id, const String& newName);
  bool exists(const String& id) const;
  String allocateId() const;

 private:
  String pathFor(const String& id) const;
  static constexpr const char* kDir = "/signals";
};
