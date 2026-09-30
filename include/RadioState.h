#pragma once

#include <Arduino.h>

enum class RadioState : uint8_t {
  Idle = 0,
  Scanning,
  Rx,
  Recording,
  Tx,
  Error
};

inline const char* radioStateName(RadioState s) {
  switch (s) {
    case RadioState::Idle:      return "IDLE";
    case RadioState::Scanning:  return "SCANNING";
    case RadioState::Rx:        return "RX";
    case RadioState::Recording: return "RECORDING";
    case RadioState::Tx:        return "TX";
    case RadioState::Error:     return "ERROR";
    default:                    return "UNKNOWN";
  }
}
