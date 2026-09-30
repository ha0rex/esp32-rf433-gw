#include "RFCapture.h"
#include <esp_attr.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

RFCapture* RFCapture::instance_ = nullptr;

bool RFCapture::begin(int gdo0Pin, size_t capacity) {
  end();
  pin_ = gdo0Pin;
  capacity_ = capacity;
  pulseCapacity_ = capacity;
  ring_ = (EdgeEvent*)heap_caps_malloc(sizeof(EdgeEvent) * capacity_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  pulses_ = (int32_t*)heap_caps_malloc(sizeof(int32_t) * pulseCapacity_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!ring_ || !pulses_) {
    end();
    return false;
  }
  pinMode(pin_, INPUT);
  instance_ = this;
  reset();
  return true;
}

void RFCapture::end() {
  stop();
  if (ring_) {
    free(ring_);
    ring_ = nullptr;
  }
  if (pulses_) {
    free(pulses_);
    pulses_ = nullptr;
  }
  if (instance_ == this) instance_ = nullptr;
}

void RFCapture::reset() {
  stop();
  head_ = 0;
  tail_ = 0;
  pulseCount_ = 0;
  captureLengthUs_ = 0;
  edgeCount_ = 0;
  overflow_ = false;
  haveLast_ = false;
  lastEdgeUs_ = 0;
}

bool RFCapture::start() {
  if (!ring_ || pin_ < 0) return false;
  reset();
  lastLevel_ = (uint8_t)digitalRead(pin_);
  lastEdgeUs_ = esp_timer_get_time();
  haveLast_ = true;
  active_ = true;
  attachInterruptArg(digitalPinToInterrupt(pin_), [](void* arg) {
    static_cast<RFCapture*>(arg)->onEdge();
  }, this, CHANGE);
  return true;
}

void RFCapture::stop() {
  if (active_ && pin_ >= 0) {
    detachInterrupt(digitalPinToInterrupt(pin_));
  }
  active_ = false;
  process();  // drain remaining
}

void IRAM_ATTR RFCapture::isrThunk() {
  if (instance_) instance_->onEdge();
}

void IRAM_ATTR RFCapture::onEdge() {
  if (!active_ || !ring_) return;
  int64_t now = esp_timer_get_time();
  uint8_t level = (uint8_t)digitalRead(pin_);

  size_t next = (head_ + 1) % capacity_;
  if (next == tail_) {
    overflow_ = true;
    return;
  }
  ring_[head_].tUs = now;
  ring_[head_].level = level;
  head_ = next;
  edgeCount_++;
}

void RFCapture::process() {
  if (!ring_ || !pulses_) return;

  // Snapshot the ISR write index once. Chasing a live head under dense 433 MHz
  // noise livelocks the main loop (HTTP dies; HomeKit autoPoll keeps running).
  noInterrupts();
  const size_t snapshotHead = head_;
  interrupts();

  size_t processed = 0;
  // Bound work even on a huge backlog so loop() can pump WiFi/HTTP.
  constexpr size_t kMaxEdgesPerCall = 512;

  while (tail_ != snapshotHead && processed < kMaxEdgesPerCall) {
    EdgeEvent ev;
    noInterrupts();
    ev = ring_[tail_];
    tail_ = (tail_ + 1) % capacity_;
    interrupts();

    if (haveLast_) {
      int64_t dt = ev.tUs - lastEdgeUs_;
      if (dt < 0) dt = 0;
      if (dt > 5000000) dt = 5000000;  // clamp absurd gaps
      if (pulseCount_ < pulseCapacity_) {
        // Duration of the PREVIOUS level
        int32_t signedDur = (lastLevel_ ? (int32_t)dt : -(int32_t)dt);
        pulses_[pulseCount_++] = signedDur;
        captureLengthUs_ += (uint32_t)dt;
      } else {
        overflow_ = true;
      }
    }
    lastEdgeUs_ = ev.tUs;
    lastLevel_ = ev.level;
    haveLast_ = true;
    processed++;
  }
}

size_t RFCapture::pulseCount() const { return pulseCount_; }

CaptureStats RFCapture::stats() const {
  CaptureStats s;
  s.edgeCount = edgeCount_;
  s.pulseCount = pulseCount_;
  s.captureLengthUs = captureLengthUs_;
  size_t usedRing = (head_ >= tail_) ? (head_ - tail_) : (capacity_ - tail_ + head_);
  s.bufferUsed = pulseCount_ + usedRing;
  s.bufferCapacity = capacity_;
  s.overflow = overflow_;
  s.active = active_;
  return s;
}

size_t RFCapture::copyPulses(int32_t* out, size_t maxCount) const {
  size_t n = min(pulseCount_, maxCount);
  memcpy(out, pulses_, n * sizeof(int32_t));
  return n;
}

void RFCapture::getSignedPulses(std::vector<int32_t>& out) const {
  out.assign(pulses_, pulses_ + pulseCount_);
}

size_t RFCapture::copyRecent(int32_t* out, size_t maxCount) const {
  if (pulseCount_ == 0 || maxCount == 0) return 0;
  size_t n = min(pulseCount_, maxCount);
  size_t start = pulseCount_ - n;
  memcpy(out, pulses_ + start, n * sizeof(int32_t));
  return n;
}
