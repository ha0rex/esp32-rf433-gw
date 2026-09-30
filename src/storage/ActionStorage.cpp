#include "ActionStorage.h"
#include <LittleFS.h>

bool ActionStorage::begin() {
  if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);
  return true;
}

String ActionStorage::pathFor(const String& id) const {
  return String(kDir) + "/" + id + ".json";
}

String ActionStorage::allocateId() const {
  char buf[16];
  snprintf(buf, sizeof(buf), "%08X", (unsigned)esp_random());
  String id = buf;
  int guard = 0;
  while (exists(id) && guard++ < 8) {
    snprintf(buf, sizeof(buf), "%08X", (unsigned)esp_random());
    id = buf;
  }
  return id;
}

bool ActionStorage::exists(const String& id) const {
  return LittleFS.exists(pathFor(id));
}

bool ActionStorage::save(RfAction& action) {
  if (action.id.isEmpty()) action.id = allocateId();
  if (action.name.isEmpty()) action.name = "Action";
  if (action.steps.empty()) return false;

  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  action.toJson(obj);

  File f = LittleFS.open(pathFor(action.id), "w");
  if (!f) return false;
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

bool ActionStorage::load(const String& id, RfAction& out) const {
  File f = LittleFS.open(pathFor(id), "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  return out.fromJson(doc.as<JsonVariantConst>());
}

bool ActionStorage::remove(const String& id) {
  String path = pathFor(id);
  if (!LittleFS.exists(path)) return false;
  return LittleFS.remove(path);
}

std::vector<RfAction> ActionStorage::list() const {
  std::vector<RfAction> out;
  File dir = LittleFS.open(kDir);
  if (!dir || !dir.isDirectory()) return out;
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String name = f.name();
      String path = name;
      if (!path.startsWith("/")) path = String(kDir) + "/" + name;
      File rf = LittleFS.open(path, "r");
      if (rf) {
        JsonDocument doc;
        if (!deserializeJson(doc, rf)) {
          RfAction a;
          if (a.fromJson(doc.as<JsonVariantConst>())) out.push_back(a);
        }
        rf.close();
      }
    }
    f = dir.openNextFile();
  }
  return out;
}
