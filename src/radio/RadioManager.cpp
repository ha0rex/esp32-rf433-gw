#include "RadioManager.h"
#include <algorithm>

bool RadioManager::begin() {
  bool ok = radio_.begin();
  if (!capture_.begin(Pins::CC1101_GDO0, RfDefaults::CaptureBufSize)) {
    Serial.println("[Radio] capture buffer alloc failed");
  }
  state_ = ok ? RadioState::Idle : RadioState::Error;
  return ok;
}

void RadioManager::loop() {
  if (state_ == RadioState::Scanning) {
    scanner_.tick();
    if (!scanner_.isRunning()) {
      state_ = RadioState::Idle;
    }
  }

  if (state_ == RadioState::Rx || state_ == RadioState::Recording) {
    capture_.process();
  }

  if (state_ == RadioState::Recording) {
    updateRecording();
  }

  if (state_ == RadioState::Tx) {
    transmitter_.tick();
    if (!transmitter_.isActive()) {
      releaseToIdle();
    } else {
      // Watchdog: never leave the radio soft-bricked in TX
      uint32_t maxMs = RfDefaults::MaxTxDurationMs + 2000;
      auto st = transmitter_.status();
      if (st.elapsedMs > maxMs) {
        Serial.printf("[Radio] TX watchdog — forcing idle after %u ms\n",
                      (unsigned)st.elapsedMs);
        transmitter_.stop();
        releaseToIdle();
      }
    }
  } else if (transmitter_.isActive()) {
    // Desync: transmitter running but state isn't Tx
    Serial.println("[Radio] TX desync — forcing stop");
    transmitter_.stop();
    releaseToIdle();
  }
}

bool RadioManager::acquire(RadioState desired) {
  if (desired == RadioState::Idle) return true;
  if (state_ != RadioState::Idle && state_ != RadioState::Error) {
    if (state_ != desired) return false;
  }
  if (!radio_.isDetected() && desired != RadioState::Idle) return false;
  state_ = desired;
  return true;
}

void RadioManager::releaseToIdle() {
  capture_.stop();
  scanner_.stop();
  if (transmitter_.isActive()) transmitter_.stop();
  radio_.idle();
  pinMode(Pins::CC1101_GDO0, INPUT);
  hkListening_ = false;
  state_ = radio_.isDetected() ? RadioState::Idle : RadioState::Error;
  // Never clear Complete / Discarded — UI needs those until save or discard
  if (recordPhase_ == RecordPhase::Calibrating ||
      recordPhase_ == RecordPhase::Waiting ||
      recordPhase_ == RecordPhase::Capturing ||
      recordPhase_ == RecordPhase::PressAgain) {
    recordPhase_ = RecordPhase::Idle;
    recordMessage_ = "Idle";
  }
}

void RadioManager::yieldHkListen() {
  // Plain RX (live listen) also blocks TX — clear either form of listen.
  if (hkListening_ || state_ == RadioState::Rx) {
    releaseToIdle();
  }
}

bool RadioManager::startHkListen() {
  if (hkListening_) return true;
  if (state_ != RadioState::Idle) return false;
  if (!acquire(RadioState::Rx)) return false;
  if (!radio_.startAsyncRx()) {
    releaseToIdle();
    return false;
  }
  if (!capture_.start()) {
    releaseToIdle();
    return false;
  }
  hkListening_ = true;
  return true;
}

bool RadioManager::stopHkListen() {
  if (!hkListening_ && state_ != RadioState::Rx) return false;
  hkListening_ = false;
  releaseToIdle();
  return true;
}

bool RadioManager::startRx() {
  yieldHkListen();
  if (state_ != RadioState::Idle) return false;
  if (!acquire(RadioState::Rx)) return false;
  if (!radio_.startAsyncRx()) {
    releaseToIdle();
    return false;
  }
  if (!capture_.start()) {
    releaseToIdle();
    return false;
  }
  return true;
}

bool RadioManager::stopRx() {
  if (state_ != RadioState::Rx) return false;
  releaseToIdle();
  return true;
}

bool RadioManager::startScan(const ScanConfig& cfg) {
  yieldHkListen();
  if (state_ != RadioState::Idle) return false;
  if (!acquire(RadioState::Scanning)) return false;
  scanner_.setConfig(cfg);
  if (!scanner_.start()) {
    releaseToIdle();
    return false;
  }
  return true;
}

bool RadioManager::stopScan() {
  if (state_ != RadioState::Scanning) return false;
  scanner_.stop();
  releaseToIdle();
  return true;
}

bool RadioManager::startRecord() {
  yieldHkListen();
  if (state_ != RadioState::Idle) return false;
  if (!acquire(RadioState::Recording)) return false;

  lastRecorded_.clear();
  repeatsFound_ = 0;
  activitySeen_ = false;
  lastPulseCount_ = 0;
  strongHoldCount_ = 0;
  peakRssiDbm_ = -128;
  burstPeakRssi_ = -128;
  captureStartedMs_ = 0;
  noiseFloorDbm_ = -90;
  triggerThresholdDbm_ = kMinStrongDbm;
  recordStartedMs_ = millis();
  phaseStartedMs_ = millis();
  lastNoiseResetMs_ = millis();
  recordPhase_ = RecordPhase::Calibrating;
  recordMessage_ = "Stay quiet… measuring background noise";

  if (!radio_.startAsyncRx()) {
    releaseToIdle();
    recordPhase_ = RecordPhase::Idle;
    return false;
  }
  if (!capture_.start()) {
    releaseToIdle();
    recordPhase_ = RecordPhase::Idle;
    return false;
  }
  return true;
}

void RadioManager::resetCaptureFresh() {
  capture_.stop();
  capture_.reset();
  capture_.start();
  lastPulseCount_ = 0;
  activitySeen_ = false;
  strongHoldCount_ = 0;
  burstPeakRssi_ = -128;
  lastActivityMs_ = millis();
  lastNoiseResetMs_ = millis();
}

void RadioManager::enterWaitingForHold() {
  resetCaptureFresh();
  phaseStartedMs_ = millis();
  recordPhase_ = RecordPhase::Waiting;
  recordMessage_ = "Hold the button down (keep it pressed)";
}

bool RadioManager::isStrongSignal(int8_t rssi) const {
  return rssi >= triggerThresholdDbm_ && rssi >= kMinStrongDbm;
}

bool RadioManager::burstLooksValid(const std::vector<int32_t>& pulses, int8_t peakRssi) const {
  if (pulses.size() < kMinPulses) return false;
  if (!isStrongSignal(peakRssi) && peakRssi < triggerThresholdDbm_) return false;

  uint32_t duration = 0;
  uint32_t goodPulses = 0;
  for (int32_t p : pulses) {
    uint32_t d = (uint32_t)abs(p);
    duration += d;
    if (d >= 100 && d <= 12000) goodPulses++;
  }
  if (duration < kMinBurstUs || duration > kMaxBurstUs) return false;
  if (goodPulses < (kMinPulses * 3 / 4)) return false;
  return true;
}

void RadioManager::finalizeFromHold(const std::vector<int32_t>& pulses, int8_t peakRssi) {
  recordPhase_ = RecordPhase::Complete;
  recordMessage_ = "Paired! Finding repeat pattern…";

  RepeatUnit unit = RFAnalyzer::extractRepeatUnit(pulses, 6000);
  delay(0);

  lastRecorded_.clear();
  lastRecorded_.frequencyMHz = radio_.frequency();
  lastRecorded_.modulation = radio_.settings().modulation;
  lastRecorded_.rssiDbm = peakRssi;
  lastRecorded_.timestamp = (uint32_t)(millis() / 1000);

  if (unit.valid) {
    lastRecorded_.pulses = std::move(unit.pulses);
    lastRecorded_.bitCodes.clear();
    lastRecorded_.bitCodes.push_back(unit.bitCode);
    lastRecorded_.bitMidUs = unit.midUs;
    lastRecorded_.estimatedRepeats = unit.repeats;
    lastRecorded_.gapBetweenRepeatsUs = unit.gapUs;
    repeatsFound_ = (uint8_t)std::min<uint32_t>(unit.repeats, 255);
  } else {
    // Fallback: keep raw hold and best-effort decode
    lastRecorded_.pulses = pulses;
    uint32_t mid = 0;
    String bits = RFAnalyzer::decodeOokBits(pulses, &mid);
    if (bits.length() >= 24) lastRecorded_.bitCodes.push_back(std::move(bits));
    lastRecorded_.bitMidUs = (uint16_t)mid;
    lastRecorded_.estimatedRepeats = 1;
    repeatsFound_ = 1;
  }

  lastRecorded_.pulseCount = lastRecorded_.pulses.size();
  lastRecorded_.durationUs = 0;
  for (int32_t p : lastRecorded_.pulses) {
    lastRecorded_.durationUs += (uint32_t)abs(p);
  }
  if (peakRssi > peakRssiDbm_) peakRssiDbm_ = peakRssi;

  lastRecorded_.analysis = RFAnalyzer::analyze(lastRecorded_.pulses, silenceGapUs_);
  if (lastRecorded_.estimatedRepeats < 1) {
    lastRecorded_.estimatedRepeats =
        max<uint32_t>(1, lastRecorded_.analysis.estimatedRepeats);
  }
  if (!lastRecorded_.gapBetweenRepeatsUs) {
    lastRecorded_.gapBetweenRepeatsUs =
        lastRecorded_.analysis.syncGapUs ? lastRecorded_.analysis.syncGapUs : 10000;
  }
  delay(0);

  recordMessage_ = String("Paired! Pattern ") +
                   String((unsigned)lastRecorded_.bitCodes.empty()
                              ? 0
                              : lastRecorded_.bitCodes[0].length()) +
                   " bits · ≈" + String((unsigned)lastRecorded_.estimatedRepeats) +
                   " repeats — name and save";
  Serial.printf("[Record] HOLD-COMPLETE peak=%d dBm unitPulses=%u bits=%u reps=%u\n",
                lastRecorded_.rssiDbm,
                (unsigned)lastRecorded_.pulses.size(),
                (unsigned)(lastRecorded_.bitCodes.empty()
                               ? 0
                               : lastRecorded_.bitCodes[0].length()),
                (unsigned)lastRecorded_.estimatedRepeats);

  releaseToIdle();
}

bool RadioManager::stopRecord(bool discard) {
  if (state_ != RadioState::Recording) {
    if (discard) {
      recordPhase_ = RecordPhase::Discarded;
      recordMessage_ = "Discarded";
      lastRecorded_.clear();
    }
    return true;
  }

  capture_.stop();
  capture_.process();

  if (discard) {
    recordPhase_ = RecordPhase::Discarded;
    recordMessage_ = "Discarded";
    lastRecorded_.clear();
    repeatsFound_ = 0;
    releaseToIdle();
    return true;
  }

  if (recordPhase_ == RecordPhase::Capturing) {
    std::vector<int32_t> pulses;
    capture_.getSignedPulses(pulses);
    if (burstLooksValid(pulses, burstPeakRssi_)) {
      finalizeFromHold(pulses, burstPeakRssi_);
      return true;
    }
  }
  recordPhase_ = RecordPhase::Discarded;
  recordMessage_ = "Cancelled — hold the button longer / closer";
  releaseToIdle();
  return true;
}

void RadioManager::updateRecording() {
  capture_.process();
  liveRssiDbm_ = radio_.readRssiDbm();
  size_t pc = capture_.pulseCount();

  if (millis() - recordStartedMs_ > (uint32_t)kSessionTimeoutMs) {
    if (recordPhase_ == RecordPhase::Capturing) {
      std::vector<int32_t> pulses;
      capture_.getSignedPulses(pulses);
      if (burstLooksValid(pulses, burstPeakRssi_)) {
        finalizeFromHold(pulses, burstPeakRssi_);
        return;
      }
    }
    recordMessage_ = "Timed out — hold the button closer for ~1 second";
    stopRecord(true);
    return;
  }

  if (recordPhase_ == RecordPhase::Calibrating) {
    static int32_t sum = 0;
    static int count = 0;
    static int8_t maxNoise = -128;
    static uint32_t calibToken = 0;

    if (calibToken != recordStartedMs_) {
      calibToken = recordStartedMs_;
      sum = 0;
      count = 0;
      maxNoise = -128;
    }

    sum += liveRssiDbm_;
    count++;
    if (liveRssiDbm_ > maxNoise) maxNoise = liveRssiDbm_;

    if (millis() - lastNoiseResetMs_ > 120) {
      capture_.stop();
      capture_.reset();
      capture_.start();
      lastPulseCount_ = 0;
      lastNoiseResetMs_ = millis();
    }

    if (millis() - phaseStartedMs_ >= (uint32_t)kCalibrateMs) {
      int8_t avg = (count > 0) ? (int8_t)(sum / count) : (int8_t)-90;
      noiseFloorDbm_ = (int8_t)((avg * 3 + maxNoise) / 4);
      int trig = (int)noiseFloorDbm_ + kNoiseMarginDb;
      if (trig < kMinStrongDbm) trig = kMinStrongDbm;
      if (trig > -35) trig = -35;
      triggerThresholdDbm_ = (int8_t)trig;
      Serial.printf("[Record] noise=%d dBm  trigger=%d dBm\n",
                    noiseFloorDbm_, triggerThresholdDbm_);
      enterWaitingForHold();
    } else {
      recordMessage_ = "Stay quiet… measuring background noise";
    }
    return;
  }

  bool pulseActivity = pc > lastPulseCount_;
  if (pulseActivity) {
    lastPulseCount_ = pc;
    lastActivityMs_ = millis();
  }

  if (recordPhase_ == RecordPhase::Waiting) {
    if (millis() - phaseStartedMs_ < (uint32_t)kArmDelayMs) {
      if (millis() - lastNoiseResetMs_ > 80) {
        capture_.stop();
        capture_.reset();
        capture_.start();
        lastPulseCount_ = 0;
        lastNoiseResetMs_ = millis();
      }
      return;
    }

    if (!isStrongSignal(liveRssiDbm_)) {
      strongHoldCount_ = 0;
      if (millis() - lastNoiseResetMs_ > 150) {
        capture_.stop();
        capture_.reset();
        capture_.start();
        lastPulseCount_ = 0;
        lastNoiseResetMs_ = millis();
      }
      recordMessage_ = String("Hold the button down — need ≥ ") +
                       String(triggerThresholdDbm_) + " dBm (now " +
                       String(liveRssiDbm_) + " dBm)";
      return;
    }

    strongHoldCount_++;
    if (strongHoldCount_ < kStrongHoldSamples) {
      recordMessage_ = "Signal locked — keep holding…";
      return;
    }

    capture_.stop();
    capture_.reset();
    capture_.start();
    lastPulseCount_ = 0;
    activitySeen_ = true;
    burstPeakRssi_ = liveRssiDbm_;
    lastActivityMs_ = millis();
    captureStartedMs_ = millis();
    recordPhase_ = RecordPhase::Capturing;
    recordMessage_ = "Recording… keep holding the button";
    return;
  }

  if (recordPhase_ == RecordPhase::Capturing) {
    if (liveRssiDbm_ > burstPeakRssi_) burstPeakRssi_ = liveRssiDbm_;
    if (isStrongSignal(liveRssiDbm_) || pulseActivity) {
      lastActivityMs_ = millis();
    }

    // Live repeat estimate for the UI (throttled — getSignedPulses is heavy)
    static uint32_t lastPeekMs = 0;
    if (pc >= 120 && (millis() - captureStartedMs_) > 200 &&
        millis() - lastPeekMs > 200) {
      lastPeekMs = millis();
      std::vector<int32_t> peek;
      capture_.getSignedPulses(peek);
      auto frames = RFAnalyzer::splitFrames(peek, 6000);
      uint8_t n = 0;
      for (const auto& f : frames) if (f.size() >= 40) n++;
      if (n > repeatsFound_) repeatsFound_ = n;
      recordMessage_ = String("Recording… ≈") + String(repeatsFound_) +
                       " repeats — release when done";
    }

    uint32_t silenceMs = silenceGapUs_ / 1000;
    if (silenceMs < 25) silenceMs = 25;

    const bool heldLongEnough =
        (millis() - captureStartedMs_) >= (uint32_t)kMinHoldMs && pc >= kMinPulses;

    if (heldLongEnough && millis() - lastActivityMs_ >= silenceMs) {
      std::vector<int32_t> pulses;
      capture_.getSignedPulses(pulses);

      if (burstLooksValid(pulses, burstPeakRssi_)) {
        Serial.printf("[Record] hold done pulses=%u peak=%d dBm\n",
                      (unsigned)pulses.size(), burstPeakRssi_);
        finalizeFromHold(pulses, burstPeakRssi_);
      } else {
        Serial.printf("[Record] rejected hold pulses=%u peak=%d thr=%d\n",
                      (unsigned)pulses.size(), burstPeakRssi_, triggerThresholdDbm_);
        recordMessage_ = "Too short / weak — hold closer for ~1 second";
        enterWaitingForHold();
      }
    }
  }
}

RecordStatus RadioManager::recordStatus() const {
  RecordStatus s;
  s.phase = recordPhase_;
  s.message = recordMessage_;
  s.complete = (recordPhase_ == RecordPhase::Complete);
  s.pressesGot = repeatsFound_;
  s.pressesNeeded = kMinRepeats;
  s.noiseFloorDbm = noiseFloorDbm_;
  s.liveRssiDbm = liveRssiDbm_;
  s.triggerThresholdDbm = triggerThresholdDbm_;
  s.peakRssiDbm = peakRssiDbm_ > burstPeakRssi_ ? peakRssiDbm_ : burstPeakRssi_;
  s.signal = lastRecorded_;
  return s;
}

bool RadioManager::startTx(const TxRequest& req) {
  // Always recover to a clean Idle first so a stale RX/TX can't soft-brick us.
  yieldHkListen();
  if (state_ == RadioState::Tx || transmitter_.isActive()) {
    Serial.println("[Radio] startTx: aborting in-progress TX");
    transmitter_.stop();
    releaseToIdle();
  }
  if (state_ == RadioState::Scanning) {
    scanner_.stop();
    releaseToIdle();
  }
  if (state_ == RadioState::Recording) {
    Serial.println("[Radio] startTx: aborting recording");
    releaseToIdle();
  }
  if (state_ != RadioState::Idle && state_ != RadioState::Error) {
    Serial.printf("[Radio] startTx: forcing idle from state=%d\n", (int)state_);
    releaseToIdle();
  }

  if (!acquire(RadioState::Tx)) return false;
  if (!transmitter_.start(req)) {
    releaseToIdle();
    return false;
  }
  return true;
}

bool RadioManager::stopTx() {
  if (state_ != RadioState::Tx && !transmitter_.isActive()) return false;
  transmitter_.stop();
  releaseToIdle();
  return true;
}

bool RadioManager::applyRfSettings(const RFSettings& s) {
  if (state_ != RadioState::Idle && state_ != RadioState::Error) return false;
  return radio_.applySettings(s);
}

void RadioManager::resetRfSettings() {
  if (state_ != RadioState::Idle && state_ != RadioState::Error) return;
  radio_.resetSettings();
}

int8_t RadioManager::currentRssi() {
  if (!radio_.isDetected()) return -128;
  // Never SPI the CC1101 while bit-banging TX — CSN traffic drops TX mode
  // and we keep toggling GDO0 into a dead chip (UI shows TX, door hears silence).
  if (state_ == RadioState::Tx || transmitter_.isActive()) return lastRssi_;
  lastRssi_ = radio_.readRssiDbm();
  return lastRssi_;
}

String RadioManager::troubleshooting() const {
  return String(
      "CC1101 NOT DETECTED\n"
      "Checks:\n"
      "1) Module powered from 3.3V only (not 5V)\n"
      "2) GND shared with ESP32\n"
      "3) Wiring: SCK=GPIO4 MOSI=GPIO3 MISO=GPIO2 CSN=GPIO1 GDO0=GPIO5\n"
      "4) Short wires / solid breadboard contacts\n"
      "5) Crystal is usually 26 MHz on common modules\n");
}
