#include "RFTransmitter.h"
#include "Config.h"
#include <esp_timer.h>

bool RFTransmitter::start(const TxRequest& req) {
  if (active_) return false;
  if (!radio_.isDetected()) return false;
  if (req.pulses.empty()) return false;
  if (!radio_.validateFrequency(req.frequencyMHz)) return false;

  req_ = req;
  if (req_.holdMs > 0) {
    uint32_t maxMs = req_.maxDurationMs ? req_.maxDurationMs : RfDefaults::MaxTxDurationMs;
    if (req_.holdMs > maxMs) req_.holdMs = maxMs;
    req_.repeats = 100000;
    req_.maxDurationMs = req_.holdMs + 200;
  } else {
    uint64_t oneBurst = 0;
    for (int32_t p : req.pulses) oneBurst += (uint32_t)abs(p);
    uint64_t totalUs = oneBurst * max<uint32_t>(1, req.repeats) +
                       (uint64_t)req.gapBetweenRepeatsUs *
                           max<uint32_t>(0, req.repeats - 1);
    uint32_t maxMs = req.maxDurationMs ? req.maxDurationMs : RfDefaults::MaxTxDurationMs;
    if (totalUs > (uint64_t)maxMs * 1000ULL) {
      uint32_t allowed = 1;
      uint64_t acc = oneBurst;
      while (allowed < req.repeats) {
        uint64_t next = acc + req.gapBetweenRepeatsUs + oneBurst;
        if (next > (uint64_t)maxMs * 1000ULL) break;
        acc = next;
        allowed++;
      }
      req_.repeats = max<uint32_t>(1, allowed);
    }
  }

  radio_.idle();
  radio_.setFrequency(req_.frequencyMHz);
  radio_.setModulation(req_.modulation);
  if (!radio_.startAsyncTx()) {
    Serial.println("[TX] startAsyncTx failed");
    return false;
  }

  active_ = true;
  aborted_ = false;
  repeatsDone_ = 0;
  startedMs_ = millis();
  waitingGap_ = false;
  restxCount_ = 0;
  message_ = req_.holdMs ? "Holding" : "Transmitting";
  Serial.printf("[TX] start reps=%u holdMs=%u pulses=%u gapUs=%u\n",
                (unsigned)req_.repeats, (unsigned)req_.holdMs,
                (unsigned)req_.pulses.size(), (unsigned)req_.gapBetweenRepeatsUs);
  return true;
}

void RFTransmitter::stop() {
  if (!active_) return;
  finish(true, "Stopped");
}

void RFTransmitter::finish(bool aborted, const char* msg) {
  active_ = false;
  aborted_ = aborted;
  waitingGap_ = false;
  message_ = msg;
  digitalWrite(Pins::CC1101_GDO0, LOW);
  radio_.idle();
  pinMode(Pins::CC1101_GDO0, INPUT);
  Serial.printf("[TX] %s repsDone=%u re-STX=%u\n", msg, (unsigned)repeatsDone_,
                (unsigned)restxCount_);
}

void RFTransmitter::transmitOneBurst() {
  // One complete frame, no gaps mid-code — splitting across loop ticks
  // inserted ms-scale holes and the door stopped decoding.
  if (!radio_.ensureAsyncTx()) {
    restxCount_++;
    if (!radio_.ensureAsyncTx()) {
      Serial.println("[TX] ensureAsyncTx failed — burst skipped");
      return;
    }
  }

  for (int32_t p : req_.pulses) {
    bool high = p > 0;
    uint32_t dur = (uint32_t)abs(p);
    if (dur == 0) continue;
    if (dur > 500000) dur = 500000;
    digitalWrite(Pins::CC1101_GDO0, high ? HIGH : LOW);
    int64_t deadline = esp_timer_get_time() + (int64_t)dur;
    while (esp_timer_get_time() < deadline) {
    }
  }
  digitalWrite(Pins::CC1101_GDO0, LOW);
}

void RFTransmitter::tick() {
  if (!active_) return;

  if (req_.holdMs > 0) {
    if ((millis() - startedMs_) >= req_.holdMs) {
      finish(false, "Hold complete");
      return;
    }
  } else {
    uint32_t maxMs = req_.maxDurationMs ? req_.maxDurationMs : RfDefaults::MaxTxDurationMs;
    if ((millis() - startedMs_) > maxMs) {
      finish(true, "Max TX duration reached");
      return;
    }
  }

  // Between frames: leave the main loop alone so HTTP can run.
  if (waitingGap_) {
    if ((int32_t)(millis() - nextBurstMs_) < 0) return;
    waitingGap_ = false;
  }

  if (req_.holdMs == 0 && repeatsDone_ >= req_.repeats) {
    finish(false, "Complete");
    return;
  }

  if (!radio_.isInTx()) restxCount_++;
  transmitOneBurst();
  repeatsDone_++;

  // Let HTTP accept connections before we spin on the next frame.
  yield();

  if (req_.holdMs == 0 && repeatsDone_ >= req_.repeats) {
    finish(false, "Complete");
    return;
  }

  // Prefer the signal's natural gap; keep at least ~8ms so the web UI can run.
  uint32_t gapMs = req_.gapBetweenRepeatsUs / 1000;
  if (gapMs < 8) gapMs = 8;
  waitingGap_ = true;
  nextBurstMs_ = millis() + gapMs;
}

TxStatus RFTransmitter::status() const {
  TxStatus s;
  s.active = active_;
  s.repeatsDone = repeatsDone_;
  s.repeatsTotal = req_.repeats;
  s.elapsedMs = active_ ? (millis() - startedMs_) : 0;
  s.aborted = aborted_;
  s.message = message_;
  return s;
}
