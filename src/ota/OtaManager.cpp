#include "OtaManager.h"
#include "Config.h"
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>

namespace {
constexpr const char* kRepoOwner = "ha0rex";
constexpr const char* kRepoName = "esp32-rf433-gw";
constexpr const char* kAssetName = "firmware.bin";
constexpr const char* kUserAgent = "esp32-rf433-gw-ota";

String releaseApiUrl(OtaChannel ch) {
  const char* tag = (ch == OtaChannel::Nightly) ? "nightly" : "stable";
  String url = "https://api.github.com/repos/";
  url += kRepoOwner;
  url += "/";
  url += kRepoName;
  url += "/releases/tags/";
  url += tag;
  return url;
}

// Strip build metadata (+...) for core compare; keep prerelease (-dev).
String versionCore(const String& v) {
  int plus = v.indexOf('+');
  return plus >= 0 ? v.substring(0, plus) : v;
}

bool parseSemver(const String& in, int& maj, int& min, int& pat, String& pre) {
  maj = min = pat = 0;
  pre = "";
  String v = versionCore(in);
  int dash = v.indexOf('-');
  String core = dash >= 0 ? v.substring(0, dash) : v;
  if (dash >= 0) pre = v.substring(dash + 1);

  int p1 = core.indexOf('.');
  if (p1 < 0) {
    maj = core.toInt();
    return true;
  }
  maj = core.substring(0, p1).toInt();
  int p2 = core.indexOf('.', p1 + 1);
  if (p2 < 0) {
    min = core.substring(p1 + 1).toInt();
    return true;
  }
  min = core.substring(p1 + 1, p2).toInt();
  pat = core.substring(p2 + 1).toInt();
  return true;
}
}  // namespace

bool OtaManager::versionNewer(const String& remote, const String& local,
                              bool nightlyChannel) {
  if (remote.isEmpty()) return false;
  if (remote == local) return false;

  int rMaj, rMin, rPat, lMaj, lMin, lPat;
  String rPre, lPre;
  parseSemver(remote, rMaj, rMin, rPat, rPre);
  parseSemver(local, lMaj, lMin, lPat, lPre);

  if (rMaj != lMaj) return rMaj > lMaj;
  if (rMin != lMin) return rMin > lMin;
  if (rPat != lPat) return rPat > lPat;

  // Same x.y.z — Stable ignores +build metadata (1.7.0 == 1.7.0+abc).
  // Nightly still offers when the full stamp differs (new Dev build).
  if (nightlyChannel) return remote != local;
  if (rPre.length() == 0 && lPre.length() > 0) return true;
  if (rPre.length() > 0 && lPre.length() == 0) return false;
  return false;
}

void OtaManager::begin() {
  prefs_.begin(PrefNamespace, false);
  uint8_t ch = prefs_.getUChar("ota_ch", (uint8_t)OtaChannel::Stable);
  channel_ = (ch == (uint8_t)OtaChannel::Nightly) ? OtaChannel::Nightly
                                                  : OtaChannel::Stable;
  message_ = "Idle";

  if (prefs_.getBool("ota_pend", false)) {
    pendingUrl_ = prefs_.getString("ota_url", "");
    pendingVersion_ = prefs_.getString("ota_ver", "");
  }

  String lastErr = prefs_.getString("ota_err", "");
  if (lastErr.length()) {
    prefs_.remove("ota_err");
    setPhase(OtaPhase::Failed, lastErr.c_str());
  }

  Serial.printf("[OTA] channel=%s version=%s pending=%d\n", channelName(),
                currentVersion(), pendingUrl_.length() ? 1 : 0);
}

void OtaManager::clearPending() {
  prefs_.remove("ota_pend");
  prefs_.remove("ota_url");
  prefs_.remove("ota_ver");
  pendingUrl_ = "";
  pendingVersion_ = "";
}

bool OtaManager::queuePending(const String& url, const String& version) {
  if (url.isEmpty()) return false;
  prefs_.putString("ota_url", url);
  prefs_.putString("ota_ver", version);
  prefs_.putBool("ota_pend", true);
  pendingUrl_ = url;
  pendingVersion_ = version;
  return true;
}

void OtaManager::loop() {
  if (!installTask_) return;
  if (!installDone_) return;

  installTask_ = nullptr;
  installDone_ = false;
  if (installOk_) {
    clearPending();
    setPhase(OtaPhase::Rebooting, "Update complete — rebooting");
    delay(600);
    ESP.restart();
  } else {
    clearPending();
    setPhase(OtaPhase::Failed,
             installError_.isEmpty() ? "Update failed" : installError_.c_str());
  }
}

const char* OtaManager::currentVersion() const {
#ifdef FW_VERSION
  return FW_VERSION;
#else
  return "0.0.0-dev";
#endif
}

const char* OtaManager::channelName() const { return channelName(channel_); }

const char* OtaManager::channelName(OtaChannel ch) {
  return ch == OtaChannel::Nightly ? "nightly" : "stable";
}

OtaChannel OtaManager::channelFromName(const char* name) {
  if (name && (!strcasecmp(name, "nightly") || !strcasecmp(name, "dev"))) {
    return OtaChannel::Nightly;
  }
  return OtaChannel::Stable;
}

const char* OtaManager::phaseName() const {
  switch (phase_) {
    case OtaPhase::Idle: return "idle";
    case OtaPhase::Checking: return "checking";
    case OtaPhase::Ready: return "ready";
    case OtaPhase::Downloading: return "downloading";
    case OtaPhase::Writing: return "writing";
    case OtaPhase::Rebooting: return "rebooting";
    case OtaPhase::Failed: return "failed";
    case OtaPhase::UpToDate: return "up_to_date";
  }
  return "idle";
}

bool OtaManager::setChannel(OtaChannel ch) {
  if (busy()) return false;
  channel_ = ch;
  prefs_.putUChar("ota_ch", (uint8_t)ch);
  available_ = OtaReleaseInfo{};
  if (phase_ == OtaPhase::Ready || phase_ == OtaPhase::UpToDate ||
      phase_ == OtaPhase::Failed) {
    setPhase(OtaPhase::Idle, "Channel changed — check again");
  }
  Serial.printf("[OTA] channel → %s\n", channelName());
  return true;
}

void OtaManager::setPhase(OtaPhase p, const char* msg) {
  phase_ = p;
  if (msg) message_ = msg;
}

void OtaManager::toJson(JsonDocument& doc) const {
  doc["ok"] = true;
  doc["version"] = currentVersion();
  doc["channel"] = channelName();
  doc["phase"] = phaseName();
  doc["progress"] = progressPct_;
  doc["message"] = message_;
  doc["busy"] = busy();
  doc["lastCheckAgeMs"] = lastCheckMs_ ? (millis() - lastCheckMs_) : 0;
  doc["repo"] = String("https://github.com/") + kRepoOwner + "/" + kRepoName;
  doc["stableBranch"] = "main";
  doc["nightlyBranch"] = "Dev";
  doc["pending"] = pendingUrl_.length() > 0;

  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running) doc["partition"] = running->label;

  JsonObject avail = doc["available"].to<JsonObject>();
  avail["valid"] = available_.valid;
  if (available_.valid) {
    avail["version"] = available_.version;
    avail["name"] = available_.name;
    avail["publishedAt"] = available_.publishedAt;
    avail["sizeBytes"] = (uint32_t)available_.sizeBytes;
    avail["prerelease"] = available_.prerelease;
    avail["notes"] = available_.notes;
    avail["newer"] = versionNewer(available_.version, currentVersion(),
                                  channel_ == OtaChannel::Nightly);
  }
}

bool OtaManager::fetchRelease(OtaChannel ch, OtaReleaseInfo& out, String& err) {
  out = OtaReleaseInfo{};
  if (WiFi.status() != WL_CONNECTED) {
    err = "Wi-Fi not connected";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);

  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(kUserAgent);
  http.addHeader("Accept", "application/vnd.github+json");

  const String url = releaseApiUrl(ch);
  if (!http.begin(client, url)) {
    err = "HTTP begin failed";
    return false;
  }

  const int code = http.GET();
  if (code == 404) {
    http.end();
    err = (ch == OtaChannel::Nightly)
              ? "No Nightly build published yet (Dev branch)"
              : "No Stable build published yet (main branch)";
    return false;
  }
  if (code != 200) {
    err = String("GitHub HTTP ") + code;
    http.end();
    return false;
  }

  JsonDocument filter;
  filter["tag_name"] = true;
  filter["name"] = true;
  filter["body"] = true;
  filter["published_at"] = true;
  filter["prerelease"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["size"] = true;

  JsonDocument doc;
  DeserializationError jerr =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (jerr) {
    err = String("JSON parse: ") + jerr.c_str();
    return false;
  }

  out.name = doc["name"] | "";
  out.publishedAt = doc["published_at"] | "";
  out.prerelease = doc["prerelease"] | false;
  out.notes = doc["body"] | "";
  if (out.notes.length() > 400) out.notes = out.notes.substring(0, 400) + "…";

  String ver;
  {
    String n = out.name;
    int sp = n.lastIndexOf(' ');
    if (sp >= 0 && sp + 1 < (int)n.length()) ver = n.substring(sp + 1);
  }
  if (ver.isEmpty()) ver = doc["tag_name"] | channelName(ch);
  out.version = ver;

  JsonArray assets = doc["assets"].as<JsonArray>();
  for (JsonObject a : assets) {
    const char* name = a["name"] | "";
    if (strcmp(name, kAssetName) != 0) continue;
    out.firmwareUrl = a["browser_download_url"] | "";
    out.sizeBytes = a["size"] | 0;
    break;
  }
  if (out.firmwareUrl.isEmpty()) {
    err = "Release has no firmware.bin asset";
    return false;
  }
  out.valid = true;
  return true;
}

bool OtaManager::checkForUpdate() {
  if (busy() && phase_ != OtaPhase::Failed && phase_ != OtaPhase::UpToDate &&
      phase_ != OtaPhase::Ready && phase_ != OtaPhase::Idle) {
    return false;
  }

  setPhase(OtaPhase::Checking, "Checking GitHub…");
  progressPct_ = 0;
  available_ = OtaReleaseInfo{};

  String err;
  OtaReleaseInfo info;
  if (!fetchRelease(channel_, info, err)) {
    setPhase(OtaPhase::Failed, err.c_str());
    lastCheckMs_ = millis();
    return false;
  }

  available_ = info;
  lastCheckMs_ = millis();

  const bool nightly = channel_ == OtaChannel::Nightly;
  if (!versionNewer(info.version, currentVersion(), nightly)) {
    setPhase(OtaPhase::UpToDate, "You're on the latest build");
    return true;
  }

  String msg = String("Update available: ") + info.version;
  setPhase(OtaPhase::Ready, msg.c_str());
  return true;
}

bool OtaManager::startInstall() {
  if (busy()) return false;
  if (phase_ != OtaPhase::Ready || !available_.valid ||
      available_.firmwareUrl.isEmpty()) {
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setPhase(OtaPhase::Failed, "Wi-Fi not connected");
    return false;
  }

  // Queue download for early-boot apply. Running beside HomeKit fragments
  // heap so Update.begin() cannot malloc its 4KB buffer.
  if (!queuePending(available_.firmwareUrl, available_.version)) {
    setPhase(OtaPhase::Failed, "Could not queue update");
    return false;
  }

  setPhase(OtaPhase::Rebooting,
           "Rebooting to install with a clean memory map…");
  progressPct_ = 0;
  Serial.printf("[OTA] queued %s — reboot to apply\n",
                available_.version.c_str());
  delay(900);
  ESP.restart();
  return true;
}

bool OtaManager::applyPendingIfNeeded() {
  if (pendingUrl_.isEmpty()) return false;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] pending update but Wi-Fi down — will retry next boot");
    return false;
  }

  applyingPending_ = true;
  available_.valid = true;
  available_.version = pendingVersion_;
  available_.firmwareUrl = pendingUrl_;
  progressPct_ = 0;
  setPhase(OtaPhase::Downloading, "Installing queued update…");
  Serial.printf("[OTA] applying pending %s (free=%u maxAlloc=%u)\n",
                pendingVersion_.c_str(), ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());

  installDone_ = false;
  installOk_ = false;
  installError_ = "";
  runInstall(pendingUrl_);

  applyingPending_ = false;
  if (installOk_) {
    clearPending();
    setPhase(OtaPhase::Rebooting, "Update complete — rebooting");
    delay(500);
    ESP.restart();
    return true;
  }

  prefs_.putString("ota_err", installError_.isEmpty() ? "Update failed"
                                                      : installError_);
  clearPending();
  setPhase(OtaPhase::Failed,
           installError_.isEmpty() ? "Update failed" : installError_.c_str());
  Serial.printf("[OTA] pending apply failed: %s\n", message_.c_str());
  return true;
}

void OtaManager::installTaskThunk(void* arg) {
  auto* self = static_cast<OtaManager*>(arg);
  self->runInstall(self->available_.firmwareUrl);
  self->installDone_ = true;
  vTaskDelete(nullptr);
}

void OtaManager::runInstall(const String& url) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(30);

  HTTPUpdate httpUpdate;
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.setLedPin(-1);

  httpUpdate.onStart([this]() {
    progressPct_ = 0;
    setPhase(OtaPhase::Downloading, "Downloading firmware…");
  });
  httpUpdate.onEnd([this]() {
    progressPct_ = 100;
    setPhase(OtaPhase::Writing, "Finalizing…");
  });
  httpUpdate.onProgress([this](int cur, int total) {
    if (total > 0) {
      progressPct_ = (int)((cur * 100LL) / total);
      if (progressPct_ > 99) progressPct_ = 99;
    }
    if (progressPct_ < 90) {
      setPhase(OtaPhase::Downloading, "Downloading firmware…");
    } else {
      setPhase(OtaPhase::Writing, "Writing flash…");
    }
  });
  httpUpdate.onError([this](int err) {
    installError_ = String("HTTPUpdate error ") + err;
  });

  Serial.printf("[OTA] downloading %s (heap=%u max=%u)\n", url.c_str(),
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  t_httpUpdate_return ret = httpUpdate.update(client, url);

  switch (ret) {
    case HTTP_UPDATE_OK:
      installOk_ = true;
      installError_ = "";
      break;
    case HTTP_UPDATE_NO_UPDATES:
      installOk_ = false;
      installError_ = "Server reported no update";
      break;
    case HTTP_UPDATE_FAILED:
    default:
      installOk_ = false;
      if (installError_.isEmpty()) {
        installError_ = httpUpdate.getLastErrorString();
        if (installError_.isEmpty()) {
          installError_ = String("Update failed (heap ") + ESP.getFreeHeap() +
                          "/" + ESP.getMaxAllocHeap() + ")";
        }
      } else {
        String detail = httpUpdate.getLastErrorString();
        if (detail.length()) {
          installError_ += ": ";
          installError_ += detail;
        }
      }
      break;
  }
}
