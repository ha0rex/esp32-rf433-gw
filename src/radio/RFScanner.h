#pragma once

#include <Arduino.h>
#include <vector>
#include "CC1101Radio.h"
#include "Config.h"

struct ScanPoint {
  float frequencyMHz = 0;
  int8_t rssiDbm = -128;
};

struct ScanConfig {
  float startMHz = RfDefaults::ScanStartMHz;
  float endMHz = RfDefaults::ScanEndMHz;
  float stepKHz = RfDefaults::ScanStepKHz;
  int dwellMs = RfDefaults::ScanDwellMs;
  int rssiThresholdDbm = RfDefaults::RssiThresholdDbm;
  bool findSignal = false;
};

struct ScanStatus {
  bool running = false;
  bool findSignal = false;
  bool signalDetected = false;
  float currentMHz = 0;
  float peakMHz = 0;
  int8_t peakRssi = -128;
  float detectedMHz = 0;
  int8_t detectedRssi = -128;
  size_t pointCount = 0;
  uint32_t passCount = 0;
};

class RFScanner {
 public:
  explicit RFScanner(CC1101Radio& radio) : radio_(radio) {}

  void setConfig(const ScanConfig& cfg) { config_ = cfg; }
  const ScanConfig& config() const { return config_; }

  bool start();
  void stop();
  bool isRunning() const { return running_; }

  // Non-blocking: call frequently from loop
  void tick();

  ScanStatus status() const;
  const std::vector<ScanPoint>& points() const { return points_; }
  void clearPoints();

 private:
  void preparePass();
  void measureCurrent();

  CC1101Radio& radio_;
  ScanConfig config_;
  bool running_ = false;
  float currentMHz_ = 0;
  float peakMHz_ = 0;
  int8_t peakRssi_ = -128;
  bool signalDetected_ = false;
  float detectedMHz_ = 0;
  int8_t detectedRssi_ = -128;
  uint32_t passCount_ = 0;
  uint32_t dwellUntilMs_ = 0;
  bool waitingDwell_ = false;
  bool zoomMode_ = false;
  float zoomCenter_ = 0;
  std::vector<ScanPoint> points_;
};
