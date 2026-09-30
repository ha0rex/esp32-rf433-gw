#pragma once

#include <Arduino.h>
#include <vector>
#include "CC1101Radio.h"
#include "RFCapture.h"
#include "RFScanner.h"
#include "RFAnalyzer.h"
#include "RFTransmitter.h"
#include "RadioState.h"
#include "../models/RFSignal.h"

enum class RecordPhase : uint8_t {
  Idle = 0,
  Calibrating = 1,
  Waiting = 2,
  Capturing = 3,
  PressAgain = 4,  // unused (legacy); hold-to-pair is single capture
  Complete = 5,
  Discarded = 6
};

inline const char* recordPhaseName(RecordPhase p) {
  switch (p) {
    case RecordPhase::Idle:        return "idle";
    case RecordPhase::Calibrating: return "calibrating";
    case RecordPhase::Waiting:     return "waiting";
    case RecordPhase::Capturing:   return "capturing";
    case RecordPhase::PressAgain:  return "press_again";
    case RecordPhase::Complete:    return "complete";
    case RecordPhase::Discarded:   return "discarded";
    default:                       return "unknown";
  }
}

struct RecordStatus {
  RecordPhase phase = RecordPhase::Idle;
  String message;
  bool complete = false;
  uint8_t pressesGot = 0;      // estimated repeats found
  uint8_t pressesNeeded = 2;   // min repeats for a solid hold
  int8_t noiseFloorDbm = -128;
  int8_t liveRssiDbm = -128;
  int8_t triggerThresholdDbm = -55;
  int8_t peakRssiDbm = -128;
  RFSignal signal;
};

class RadioManager {
 public:
  bool begin();
  void loop();

  RadioState state() const { return state_; }
  CC1101Radio& radio() { return radio_; }
  RFScanner& scanner() { return scanner_; }
  RFCapture& capture() { return capture_; }
  RFTransmitter& transmitter() { return transmitter_; }

  bool acquire(RadioState desired);
  void releaseToIdle();

  bool startRx();
  bool stopRx();

  // Background RX used by HomeKit to detect physical remote presses.
  // Yields automatically when Record/Scan/Tx/Live-RX starts.
  bool startHkListen();
  bool stopHkListen();
  bool isHkListening() const { return hkListening_; }
  void yieldHkListen();

  bool startScan(const ScanConfig& cfg);
  bool stopScan();

  bool startRecord();
  bool stopRecord(bool discard = false);
  RecordStatus recordStatus() const;
  const RFSignal& lastRecorded() const { return lastRecorded_; }

  bool startTx(const TxRequest& req);
  bool stopTx();

  bool applyRfSettings(const RFSettings& s);
  void resetRfSettings();

  int8_t currentRssi();
  String troubleshooting() const;

 private:
  void updateRecording();
  void enterWaitingForHold();
  void resetCaptureFresh();
  bool isStrongSignal(int8_t rssi) const;
  bool burstLooksValid(const std::vector<int32_t>& pulses, int8_t peakRssi) const;
  void finalizeFromHold(const std::vector<int32_t>& pulses, int8_t peakRssi);

  CC1101Radio radio_;
  RFCapture capture_;
  RFScanner scanner_{radio_};
  RFTransmitter transmitter_{radio_};

  RadioState state_ = RadioState::Idle;
  bool hkListening_ = false;

  RecordPhase recordPhase_ = RecordPhase::Idle;
  String recordMessage_;
  RFSignal lastRecorded_;
  uint32_t recordStartedMs_ = 0;
  uint32_t phaseStartedMs_ = 0;
  uint32_t captureStartedMs_ = 0;
  uint32_t lastActivityMs_ = 0;
  uint32_t lastNoiseResetMs_ = 0;
  size_t lastPulseCount_ = 0;
  int silenceGapUs_ = 35000;  // end hold after release (inter-repeat gaps are shorter)
  bool activitySeen_ = false;

  static constexpr uint8_t kMinRepeats = 2;
  static constexpr int kCalibrateMs = 700;
  static constexpr int kArmDelayMs = 350;
  static constexpr int kSessionTimeoutMs = 90000;
  static constexpr int kMinHoldMs = 350;
  static constexpr int kMinStrongDbm = -58;
  static constexpr int kNoiseMarginDb = 16;
  static constexpr int kStrongHoldSamples = 3;
  static constexpr size_t kMinPulses = 80;
  static constexpr uint32_t kMinBurstUs = 5000;
  static constexpr uint32_t kMaxBurstUs = 2500000;

  int8_t noiseFloorDbm_ = -90;
  int8_t triggerThresholdDbm_ = -55;
  int8_t liveRssiDbm_ = -128;
  int8_t lastRssi_ = -128;
  int8_t peakRssiDbm_ = -128;
  int8_t burstPeakRssi_ = -128;
  int strongHoldCount_ = 0;
  uint8_t repeatsFound_ = 0;
};
