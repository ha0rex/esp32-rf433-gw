#pragma once

#include <Arduino.h>
#include <vector>
#include "../storage/RemoteStorage.h"
#include "../storage/SignalStorage.h"
#include "../storage/ActionStorage.h"
#include "../action/ActionRunner.h"
#include "../radio/RadioManager.h"
#include "../radio/WeatherDecoder.h"

class HomeKitManager {
 public:
  HomeKitManager(RadioManager& radio, SignalStorage& signals, RemoteStorage& remotes,
                 ActionStorage& actions, ActionRunner& runner)
      : radio_(radio),
        signals_(signals),
        remotes_(remotes),
        actions_(actions),
        runner_(runner) {}

  bool begin(const String& wifiSsid, const String& wifiPass);
  void loop();

  bool isEnabled() const { return enabled_; }
  bool isPaired() const;
  String setupCode() const { return setupCode_; }
  String setupId() const { return setupId_; }
  uint16_t hapPort() const { return hapPort_; }

  void requestRebuildAndReboot();

  static HomeKitManager* instance();

  bool fireButtonForSignal(const String& signalId);
  bool triggerSensorForSignal(const String& signalId);
  bool publishWeather(const WeatherReading& reading);

  int deviceCount() const;

 private:
  void buildAccessories();

  RadioManager& radio_;
  SignalStorage& signals_;
  RemoteStorage& remotes_;
  ActionStorage& actions_;
  ActionRunner& runner_;
  bool enabled_ = false;
  bool begun_ = false;
  String setupCode_ = "23114081";
  String setupId_ = "RFGW";
  uint16_t hapPort_ = 1201;

  static HomeKitManager* self_;
};
