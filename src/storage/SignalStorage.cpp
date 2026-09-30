#include "SignalStorage.h"
#include <algorithm>

bool SignalStorage::begin() {
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount failed");
    return false;
  }
  if (!LittleFS.exists(kDir)) {
    LittleFS.mkdir(kDir);
  }
  Serial.println("[FS] LittleFS ready");
  return true;
}

String SignalStorage::pathFor(const String& id) const {
  return String(kDir) + "/" + id + ".json";
}

String SignalStorage::allocateId() const {
  char buf[16];
  snprintf(buf, sizeof(buf), "%08X", (unsigned)(esp_random()));
  String id = buf;
  // Ensure unique
  int guard = 0;
  while (exists(id) && guard++ < 8) {
    snprintf(buf, sizeof(buf), "%08X", (unsigned)(esp_random()));
    id = buf;
  }
  return id;
}

bool SignalStorage::exists(const String& id) const {
  return LittleFS.exists(pathFor(id));
}

bool SignalStorage::save(RFSignal& signal) {
  if (signal.id.isEmpty()) {
    signal.id = allocateId();
  }
  if (signal.name.isEmpty()) signal.name = "Signal " + signal.id;
  if (signal.timestamp == 0) signal.timestamp = (uint32_t)(millis() / 1000);

  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  signal.toJson(obj, true);

  File f = LittleFS.open(pathFor(signal.id), "w");
  if (!f) return false;
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

bool SignalStorage::load(const String& id, RFSignal& out) const {
  File f = LittleFS.open(pathFor(id), "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  return out.fromJson(doc.as<JsonVariantConst>());
}

bool SignalStorage::remove(const String& id) {
  String path = pathFor(id);
  if (!LittleFS.exists(path)) return false;
  return LittleFS.remove(path);
}

bool SignalStorage::rename(const String& id, const String& newName) {
  RFSignal sig;
  if (!load(id, sig)) return false;
  sig.name = newName;
  return save(sig);
}

std::vector<RFSignal> SignalStorage::list(bool includePulses) const {
  std::vector<RFSignal> out;
  File dir = LittleFS.open(kDir);
  if (!dir || !dir.isDirectory()) return out;

  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String name = f.name();
      // name may be full path or basename depending on core
      String path = name;
      if (!path.startsWith("/")) path = String(kDir) + "/" + name;
      File rf = LittleFS.open(path, "r");
      if (rf) {
        JsonDocument doc;
        if (deserializeJson(doc, rf) == DeserializationError::Ok) {
          RFSignal sig;
          if (sig.fromJson(doc.as<JsonVariantConst>())) {
            if (!includePulses) sig.pulses.clear();
            out.push_back(sig);
          }
        }
        rf.close();
      }
    }
    f = dir.openNextFile();
  }
  // Sort by timestamp desc
  std::sort(out.begin(), out.end(), [](const RFSignal& a, const RFSignal& b) {
    return a.timestamp > b.timestamp;
  });
  return out;
}
