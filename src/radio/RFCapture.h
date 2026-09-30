#pragma once

#include <Arduino.h>
#include <vector>
#include "Config.h"

struct PulseSample {
  int32_t durationUs;  // signed: +HIGH, -LOW
};

struct CaptureStats {
  uint32_t edgeCount = 0;
  uint32_t pulseCount = 0;
  uint32_t captureLengthUs = 0;
  uint32_t bufferUsed = 0;
  uint32_t bufferCapacity = 0;
  bool overflow = false;
  bool active = false;
};

class RFCapture {
 public:
  bool begin(int gdo0Pin, size_t capacity = RfDefaults::CaptureBufSize);
  void end();

  bool start();
  void stop();
  void reset();

  bool isActive() const { return active_; }
  bool hasOverflow() const { return overflow_; }

  CaptureStats stats() const;
  size_t copyPulses(int32_t* out, size_t maxCount) const;
  size_t pulseCount() const;
  void getSignedPulses(std::vector<int32_t>& out) const;

  // Called from main loop — drains ISR ring into pulse list if needed
  void process();

  // Live snapshot for websocket (most recent N pulses)
  size_t copyRecent(int32_t* out, size_t maxCount) const;

 private:
  static void IRAM_ATTR isrThunk();
  void IRAM_ATTR onEdge();

  static RFCapture* instance_;

  int pin_ = -1;
  volatile bool active_ = false;
  volatile bool overflow_ = false;
  volatile uint32_t edgeCount_ = 0;

  // ISR ring: raw edge timestamps + level after edge
  struct EdgeEvent {
    int64_t tUs;
    uint8_t level;  // level AFTER the transition
  };

  EdgeEvent* ring_ = nullptr;
  size_t capacity_ = 0;
  volatile size_t head_ = 0;  // write index (ISR)
  volatile size_t tail_ = 0;  // read index (task)

  // Processed signed pulses
  int32_t* pulses_ = nullptr;
  size_t pulseCapacity_ = 0;
  size_t pulseCount_ = 0;
  uint32_t captureLengthUs_ = 0;

  int64_t lastEdgeUs_ = 0;
  uint8_t lastLevel_ = 0;
  bool haveLast_ = false;
};
