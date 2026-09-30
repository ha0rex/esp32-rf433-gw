#include "OtaManager.h"
#include "Config.h"
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>

namespace {
constexpr const char* kRepoOwner = "ha0rex";
constexpr const char* kRepoName = "esp32-rf433-gw";
constexpr const char* kAssetName = "firmware.bin";
constexpr const char* kUserAgent = "esp32-rf433-gw-ota";

String releaseApiUrl(OtaChannel ch) {
  // Rolling tags published by CI: stable ← main, nightly ← Dev
  const char* tag = (ch == OtaChannel::Nightly) ? "nightly" : "stable";
  String url = "https://api.github.com/repos/";
  url += kRepoOwner;
  url += "/";
  url += kRepoName;
  url += "/releases/tags/";
  url += tag;
  return url;
}

bool versionNewer(const String& remote, const String& local) {
  if (remote.isEmpty() || remote == local) return false;
  // Prefer exact mismatch as "available" so nightly rebuilds with same
  // semver but different +git suffix still offer an update.
  return remote != local;
}
}  // namespace

void OtaManager::begin() {
  prefs_.begin(PrefNamespace, false);
  uint8_t ch = prefs_.getUChar("ota_ch", (uint8_t)OtaChannel::Stable);
  channel_ = (ch == (uint8_t)OtaChannel::Nightly) ? OtaChannel::Nightly
                                                  : OtaChannel::Stable;
  message_ = "Idle";
  Serial.printf("[OTA] channel=%s version=%s\n", channelName(), currentVersion());
}

void OtaManager::loop() {
  if (!installTask_) return;
  if (!installDone_) return;

  installTask_ = nullptr;
  installDone_ = false;
  if (installOk_) {
    setPhase(OtaPhase::Rebooting, "Update complete — rebooting");
    delay(600);
    ESP.restart();
  } else {
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

  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running) {
    doc["partition"] = running->label;
  }

  JsonObject avail = doc["available"].to<JsonObject>();
  avail["valid"] = available_.valid;
  if (available_.valid) {
    avail["version"] = available_.version;
    avail["name"] = available_.name;
    avail["publishedAt"] = available_.publishedAt;
    avail["sizeBytes"] = (uint32_t)available_.sizeBytes;
    avail["prerelease"] = available_.prerelease;
    avail["notes"] = available_.notes;
    avail["newer"] = versionNewer(available_.version, currentVersion());
  }
}

bool OtaManager::fetchRelease(OtaChannel ch, OtaReleaseInfo& out, String& err) {
  out = OtaReleaseInfo{};
  if (WiFi.status() != WL_CONNECTED) {
    err = "Wi-Fi not connected";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();  // GitHub + CDN; cert bundle would add ~50KB flash
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

  // Releases JSON is large; filter to fields we need.
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

  // Prefer version from release name "Stable 1.7.0+abc" / "Nightly 1.7.0-dev+abc"
  // fall back to tag body first line "version: x" or tag name.
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

  if (!versionNewer(info.version, currentVersion())) {
    setPhase(OtaPhase::UpToDate, "You're on the latest build");
    return true;
  }

  String msg = String("Update available: ") + info.version;
  setPhase(OtaPhase::Ready, msg.c_str());
  return true;
}

bool OtaManager::startInstall() {
  if (installTask_) return false;
  if (phase_ != OtaPhase::Ready || !available_.valid ||
      available_.firmwareUrl.isEmpty()) {
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setPhase(OtaPhase::Failed, "Wi-Fi not connected");
    return false;
  }

  installDone_ = false;
  installOk_ = false;
  installError_ = "";
  progressPct_ = 0;
  setPhase(OtaPhase::Downloading, "Starting download…");

  BaseType_t ok = xTaskCreatePinnedToCore(
      installTaskThunk, "ota", 12288, this, 1, &installTask_, 0);
  if (ok != pdPASS) {
    installTask_ = nullptr;
    setPhase(OtaPhase::Failed, "Could not start update task");
    return false;
  }
  return true;
}

void OtaManager::installTaskThunk(void* arg) {
  static_cast<OtaManager*>(arg)->runInstall();
}

void OtaManager::runInstall() {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20);

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

  Serial.printf("[OTA] installing %s from %s\n", available_.version.c_str(),
                available_.firmwareUrl.c_str());

  t_httpUpdate_return ret =
      httpUpdate.update(client, available_.firmwareUrl);

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
        if (installError_.isEmpty()) installError_ = "Update failed";
      } else {
        String detail = httpUpdate.getLastErrorString();
        if (detail.length()) {
          installError_ += ": ";
          installError_ += detail;
        }
      }
      break;
  }

  installDone_ = true;
  vTaskDelete(nullptr);
}
