#include "RemoteStorage.h"
#include <LittleFS.h>
#include <algorithm>

bool RemoteStorage::begin() {
  if (!LittleFS.exists(kDir)) {
    LittleFS.mkdir(kDir);
  }
  return true;
}

String RemoteStorage::pathFor(const String& id) const {
  return String(kDir) + "/" + id + ".json";
}

String RemoteStorage::allocateId() const {
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

bool RemoteStorage::exists(const String& id) const {
  return LittleFS.exists(pathFor(id));
}

bool RemoteStorage::save(RfRemote& remote) {
  if (remote.id.isEmpty()) remote.id = allocateId();
  if (remote.name.isEmpty()) remote.name = "Device";
  if (remote.type.isEmpty()) remote.type = "remote";

  if (remote.isWeather()) {
    if (remote.weatherId < 0) return false;
  } else {
    if (remote.buttons.empty()) return false;
  }

  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  remote.toJson(obj);

  File f = LittleFS.open(pathFor(remote.id), "w");
  if (!f) return false;
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

bool RemoteStorage::load(const String& id, RfRemote& out) const {
  File f = LittleFS.open(pathFor(id), "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  return out.fromJson(doc.as<JsonVariantConst>());
}

bool RemoteStorage::remove(const String& id) {
  String path = pathFor(id);
  if (!LittleFS.exists(path)) return false;
  return LittleFS.remove(path);
}

std::vector<RfRemote> RemoteStorage::list() const {
  std::vector<RfRemote> out;
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
        if (deserializeJson(doc, rf) == DeserializationError::Ok) {
          RfRemote remote;
          if (remote.fromJson(doc.as<JsonVariantConst>())) {
            out.push_back(remote);
          }
        }
        rf.close();
      }
    }
    f = dir.openNextFile();
  }
  std::sort(out.begin(), out.end(), [](const RfRemote& a, const RfRemote& b) {
    return a.name < b.name;
  });
  return out;
}
