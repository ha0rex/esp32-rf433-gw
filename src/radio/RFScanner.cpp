#include "RFScanner.h"

bool RFScanner::start() {
  if (!radio_.isDetected()) return false;
  if (config_.endMHz <= config_.startMHz) return false;
  if (config_.stepKHz < 1.0f) config_.stepKHz = 1.0f;
  if (config_.dwellMs < 1) config_.dwellMs = 1;

  running_ = true;
  signalDetected_ = false;
  zoomMode_ = false;
  peakRssi_ = -128;
  peakMHz_ = config_.startMHz;
  passCount_ = 0;
  points_.clear();
  // Reserve approximate point count
  size_t approx = (size_t)(((config_.endMHz - config_.startMHz) * 1000.0f) / config_.stepKHz) + 4;
  points_.reserve(min(approx, (size_t)2048));
  preparePass();
  radio_.idle();
  return true;
}

void RFScanner::stop() {
  running_ = false;
  waitingDwell_ = false;
  radio_.idle();
}

void RFScanner::clearPoints() {
  points_.clear();
  peakRssi_ = -128;
}

void RFScanner::preparePass() {
  if (zoomMode_ && config_.findSignal) {
    float half = max(0.05f, (config_.stepKHz / 1000.0f) * 10.0f);
    currentMHz_ = zoomCenter_ - half;
  } else {
    currentMHz_ = config_.startMHz;
  }
  waitingDwell_ = false;
}

void RFScanner::measureCurrent() {
  if (!radio_.setFrequency(currentMHz_)) {
    currentMHz_ += config_.stepKHz / 1000.0f;
    return;
  }
  radio_.startAsyncRx();
  dwellUntilMs_ = millis() + (uint32_t)config_.dwellMs;
  waitingDwell_ = true;
}

void RFScanner::tick() {
  if (!running_) return;

  if (!waitingDwell_) {
    float end = config_.endMHz;
    float start = config_.startMHz;
    float step = config_.stepKHz / 1000.0f;
    if (zoomMode_ && config_.findSignal) {
      float half = max(0.05f, step * 10.0f);
      start = zoomCenter_ - half;
      end = zoomCenter_ + half;
      step = max(0.005f, step / 2.0f);
    }

    if (currentMHz_ > end + 0.0001f) {
      passCount_++;
      if (config_.findSignal && signalDetected_ && !zoomMode_) {
        zoomMode_ = true;
        zoomCenter_ = detectedMHz_;
      } else if (zoomMode_ && passCount_ > 0 && (passCount_ % 3 == 0)) {
        // After a few zoomed passes, resume wide scan
        zoomMode_ = false;
      }
      preparePass();
      return;
    }
    measureCurrent();
    return;
  }

  if ((int32_t)(millis() - dwellUntilMs_) < 0) return;

  int8_t rssi = radio_.readRssiDbm();
  radio_.idle();

  ScanPoint pt;
  pt.frequencyMHz = currentMHz_;
  pt.rssiDbm = rssi;

  // Update / insert into points (coarse map by rounded kHz bucket)
  bool updated = false;
  for (auto& existing : points_) {
    if (fabsf(existing.frequencyMHz - pt.frequencyMHz) < (config_.stepKHz / 2000.0f)) {
      existing.rssiDbm = pt.rssiDbm;
      updated = true;
      break;
    }
  }
  if (!updated && points_.size() < 2048) {
    points_.push_back(pt);
  }

  if (rssi > peakRssi_) {
    peakRssi_ = rssi;
    peakMHz_ = currentMHz_;
  }

  if (config_.findSignal && rssi >= config_.rssiThresholdDbm) {
    signalDetected_ = true;
    if (rssi >= detectedRssi_) {
      detectedRssi_ = rssi;
      detectedMHz_ = currentMHz_;
    }
    if (!zoomMode_) {
      zoomMode_ = true;
      zoomCenter_ = currentMHz_;
    }
  }

  float step = config_.stepKHz / 1000.0f;
  if (zoomMode_ && config_.findSignal) {
    step = max(0.005f, step / 2.0f);
  }
  currentMHz_ += step;
  waitingDwell_ = false;
}

ScanStatus RFScanner::status() const {
  ScanStatus s;
  s.running = running_;
  s.findSignal = config_.findSignal;
  s.signalDetected = signalDetected_;
  s.currentMHz = currentMHz_;
  s.peakMHz = peakMHz_;
  s.peakRssi = peakRssi_;
  s.detectedMHz = detectedMHz_;
  s.detectedRssi = detectedRssi_;
  s.pointCount = points_.size();
  s.passCount = passCount_;
  return s;
}
