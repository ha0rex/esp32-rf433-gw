#pragma once

#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>
#include "../radio/RadioManager.h"
#include "../radio/WeatherDecoder.h"
#include "../storage/SignalStorage.h"
#include "../storage/RemoteStorage.h"
#include "../action/ActionRunner.h"

struct PressEvent {
  uint32_t seq = 0;
  uint32_t atMs = 0;
  bool matched = false;
  bool homekitFired = false;
  String signalId;
  String signalName;
  String remoteId;
  String remoteName;
  String buttonName;
  int8_t rssiDbm = -128;
  uint16_t pulseCount = 0;
};

class HomeKitManager;
class ActionRunner;

class PressDetector {
 public:
  PressDetector(RadioManager& radio, SignalStorage& signals, RemoteStorage& remotes)
      : radio_(radio), signals_(signals), remotes_(remotes) {}

  void setHomeKit(HomeKitManager* hk) { homekit_ = hk; }
  void setActionRunner(ActionRunner* runner) { runner_ = runner; }
  void begin();
  void loop();
  void reloadTemplates();

  const std::vector<PressEvent>& recent() const { return recent_; }
  const std::vector<WeatherReading>& recentWeather() const { return recentWeather_; }
  uint32_t eventSeq() const { return eventSeq_; }
  bool listening() const { return radio_.isHkListening(); }

  bool takeNewerThan(uint32_t lastSeq, PressEvent& out) const;
  void toJson(JsonDocument& doc) const;
  void weatherToJson(JsonDocument& doc) const;

 private:
  struct Template {
    String signalId;
    String signalName;
    String remoteId;
    String remoteName;
    String buttonName;
    std::vector<String> bitCodes;
    String core;
    uint16_t midUs = 0;
  };

  void processBursts();
  void resetCapture();
  void recordEvent(PressEvent ev);
  void recordWeather(const WeatherReading& r);
  void buildCores();
  bool tryMatchPulses(const std::vector<int32_t>& pulses);
  bool tryWeatherPulses(const std::vector<int32_t>& pulses);
  const Template* matchDecoded(const String& bits, bool gappedBurst,
                               float* distOut = nullptr,
                               uint16_t decodeMidUs = 0) const;
  void fireMatch(const Template& hit, int8_t rssi, size_t pulseCount, float dist);
  bool needListen() const;

  RadioManager& radio_;
  SignalStorage& signals_;
  RemoteStorage& remotes_;
  HomeKitManager* homekit_ = nullptr;
  ActionRunner* runner_ = nullptr;

  std::vector<Template> templates_;
  std::vector<PressEvent> recent_;
  std::vector<WeatherReading> recentWeather_;
  uint32_t eventSeq_ = 0;
  uint32_t weatherSeq_ = 0;
  bool wantWeather_ = false;

  uint32_t lastPulseMs_ = 0;
  uint32_t lastFireMs_ = 0;
  uint32_t lastTryMs_ = 0;
  uint32_t lastWeatherMs_ = 0;
  size_t lastPulseCount_ = 0;
  size_t lastTryPulseCount_ = 0;
  int8_t burstPeakRssi_ = -128;
  bool inBurst_ = false;
  bool begun_ = false;

  static constexpr size_t kMaxRecent = 40;
  static constexpr size_t kMaxWeather = 20;
  static constexpr int8_t kMinRssiDbm = -82;
  static constexpr uint32_t kPulseSilenceMs = 18;
  static constexpr uint32_t kHoldCooldownMs = 900;
  static constexpr size_t kMaxBurstPulses = 2000;
  static constexpr uint32_t kFrameGapUs = 6000;
  static constexpr size_t kCoreLen = 32;
  static constexpr float kMaxCoreDist = 0.12f;
  static constexpr float kMinCoreMargin = 0.05f;
  static constexpr float kMaxFullDist = 0.12f;
  static constexpr float kMaxSiblingDist = 0.14f;
  static constexpr float kMinSiblingMargin = 0.12f;
};
