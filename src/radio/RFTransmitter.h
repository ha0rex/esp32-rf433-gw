#pragma once

#include <Arduino.h>
#include <vector>
#include "CC1101Radio.h"
#include "Config.h"

struct TxRequest {
  float frequencyMHz = RfDefaults::FrequencyMHz;
  uint8_t modulation = RfDefaults::ModulationOOK;
  std::vector<int32_t> pulses;
  uint32_t repeats = 1;
  uint32_t gapBetweenRepeatsUs = 10000;
  uint32_t maxDurationMs = RfDefaults::MaxTxDurationMs;
  uint32_t holdMs = 0;
};

struct TxStatus {
  bool active = false;
  uint32_t repeatsDone = 0;
  uint32_t repeatsTotal = 0;
  uint32_t elapsedMs = 0;
  bool aborted = false;
  String message;
};

class RFTransmitter {
 public:
  explicit RFTransmitter(CC1101Radio& radio) : radio_(radio) {}

  bool start(const TxRequest& req);
  void stop();
  bool isActive() const { return active_; }
  void tick();
  TxStatus status() const;

 private:
  void transmitOneBurst();
  void finish(bool aborted, const char* msg);

  CC1101Radio& radio_;
  TxRequest req_;
  bool active_ = false;
  bool aborted_ = false;
  uint32_t repeatsDone_ = 0;
  uint32_t startedMs_ = 0;
  uint32_t nextBurstMs_ = 0;
  bool waitingGap_ = false;
  uint32_t restxCount_ = 0;
  String message_;
};
