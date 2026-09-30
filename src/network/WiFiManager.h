#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <vector>
#include "Config.h"

struct WifiNetworkInfo {
  String ssid;
  int32_t rssi = 0;
  wifi_auth_mode_t encryption = WIFI_AUTH_OPEN;
};

enum class WifiMode : uint8_t {
  Disconnected = 0,
  Connecting,
  Station,
  AccessPoint
};

class WiFiManager {
 public:
  bool begin();
  void loop();

  WifiMode mode() const { return mode_; }
  bool isProvisioned() const { return provisioned_; }
  bool isConnected() const { return mode_ == WifiMode::Station && WiFi.status() == WL_CONNECTED; }
  String ipAddress() const;
  int8_t rssi() const;
  String apSsid() const { return apSsid_; }
  String hostname() const { return NetDefaults::Hostname; }

  bool saveCredentials(const String& ssid, const String& password);
  bool clearCredentials();
  bool hasCredentials() const;

  String ssid() const { return savedSsid_; }
  String password() const { return savedPass_; }
  void stopMdns();

  std::vector<WifiNetworkInfo> scanNetworks();

  // Captive portal DNS
  void processDns();

 private:
  void startAp();
  bool connectSta(uint32_t timeoutMs);
  void loadCredentials();
  String macSuffix() const;

  Preferences prefs_;
  DNSServer dns_;
  WifiMode mode_ = WifiMode::Disconnected;
  bool provisioned_ = false;
  String savedSsid_;
  String savedPass_;
  String apSsid_;
  uint32_t connectStartedMs_ = 0;
};
