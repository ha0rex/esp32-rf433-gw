#include "WiFiManager.h"

bool WiFiManager::begin() {
  prefs_.begin(PrefNamespace, false);
  loadCredentials();
  apSsid_ = String(NetDefaults::ApPrefix) + macSuffix();

  WiFi.persistent(false);
  WiFi.setHostname(NetDefaults::Hostname);

  if (hasCredentials()) {
    provisioned_ = true;
    Serial.printf("[WiFi] Connecting to '%s'...\n", savedSsid_.c_str());
    if (connectSta(NetDefaults::WifiTimeoutMs)) {
      mode_ = WifiMode::Station;
      if (MDNS.begin(NetDefaults::Hostname)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[WiFi] mDNS http://%s.local\n", NetDefaults::Hostname);
      }
      Serial.printf("[WiFi] Connected IP=%s RSSI=%d\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      return true;
    }
    Serial.println("[WiFi] STA failed — falling back to SoftAP");
  } else {
    Serial.println("[WiFi] No credentials — SoftAP provisioning");
  }

  startAp();
  return true;
}

void WiFiManager::loop() {
  processDns();

  if (mode_ == WifiMode::Station && WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Lost connection — SoftAP fallback");
    startAp();
  }
}

void WiFiManager::processDns() {
  if (mode_ == WifiMode::AccessPoint) {
    dns_.processNextRequest();
  }
}

void WiFiManager::loadCredentials() {
  savedSsid_ = prefs_.getString("ssid", "");
  savedPass_ = prefs_.getString("pass", "");
}

bool WiFiManager::hasCredentials() const {
  return savedSsid_.length() > 0;
}

bool WiFiManager::saveCredentials(const String& ssid, const String& password) {
  if (ssid.length() == 0 || ssid.length() > 32) return false;
  prefs_.putString("ssid", ssid);
  prefs_.putString("pass", password);
  savedSsid_ = ssid;
  savedPass_ = password;
  provisioned_ = true;
  return true;
}

bool WiFiManager::clearCredentials() {
  prefs_.remove("ssid");
  prefs_.remove("pass");
  savedSsid_ = "";
  savedPass_ = "";
  provisioned_ = false;
  return true;
}

String WiFiManager::macSuffix() const {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[5];
  snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
  return String(buf);
}

void WiFiManager::startAp() {
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_AP);
  // SoftAP IP 192.168.4.1
  WiFi.softAP(apSsid_.c_str());
  delay(100);
  IPAddress apIP = WiFi.softAPIP();
  dns_.start(53, "*", apIP);
  mode_ = WifiMode::AccessPoint;
  Serial.printf("[WiFi] SoftAP SSID=%s IP=%s\n", apSsid_.c_str(), apIP.toString().c_str());
}

bool WiFiManager::connectSta(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSsid_.c_str(), savedPass_.c_str());
  mode_ = WifiMode::Connecting;
  connectStartedMs_ = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - connectStartedMs_ > timeoutMs) {
      return false;
    }
    delay(200);
    yield();
  }
  return true;
}

String WiFiManager::ipAddress() const {
  if (mode_ == WifiMode::AccessPoint) return WiFi.softAPIP().toString();
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return "0.0.0.0";
}

int8_t WiFiManager::rssi() const {
  if (WiFi.status() == WL_CONNECTED) return (int8_t)WiFi.RSSI();
  return 0;
}

void WiFiManager::stopMdns() {
  MDNS.end();
}

std::vector<WifiNetworkInfo> WiFiManager::scanNetworks() {
  std::vector<WifiNetworkInfo> result;
  // Preserve current mode as much as possible
  wifi_mode_t prev = WiFi.getMode();
  if (prev == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
  } else if (prev == WIFI_MODE_AP) {
    WiFi.mode(WIFI_AP_STA);
  }

  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
  if (n < 0) n = 0;
  for (int i = 0; i < n; i++) {
    WifiNetworkInfo info;
    info.ssid = WiFi.SSID(i);
    info.rssi = WiFi.RSSI(i);
    info.encryption = WiFi.encryptionType(i);
    if (info.ssid.length()) result.push_back(info);
  }
  WiFi.scanDelete();
  return result;
}
