#pragma once

#include <Arduino.h>
#include <vector>
#include "Config.h"
#include "../models/RFSignal.h"

struct RepeatUnit {
  bool valid = false;
  std::vector<int32_t> pulses;  // one repeat of the button frame
  String bitCode;               // smallest repeating bit pattern
  uint16_t midUs = 0;
  uint32_t repeats = 1;
  uint32_t gapUs = 10000;
};

class RFAnalyzer {
 public:
  static SignalAnalysis analyze(const std::vector<int32_t>& pulses,
                                uint32_t silenceGapUs = RfDefaults::SilenceGapUs);
  static uint32_t estimateRepeats(const std::vector<int32_t>& pulses,
                                  uint32_t& frameDurationUs,
                                  uint32_t silenceGapUs = RfDefaults::SilenceGapUs);

  // Split pulse stream into frames at long LOW gaps
  static std::vector<std::vector<int32_t>> splitFrames(
      const std::vector<int32_t>& pulses, uint32_t gapUs = 8000);

  // Drop sub-threshold glitch edges and merge consecutive same-sign pulses.
  // Fixes mid-frame noise like (444, -13, 597) → (1041) that corrupts EV1527 bits.
  static std::vector<int32_t> cleanGlitchPulses(const std::vector<int32_t>& pulses,
                                                uint32_t minEdgeUs = 80);

  // Decode OOK as short-HIGH=0 / long-HIGH=1.
  // If fixedMidUs > 0, use that threshold (stable across presses); else estimate.
  static String decodeOokBits(const std::vector<int32_t>& pulses,
                              uint32_t* midUsOut = nullptr,
                              uint32_t fixedMidUs = 0);

  // From a held-button capture (many repeats), find the smallest repeating unit.
  static RepeatUnit extractRepeatUnit(const std::vector<int32_t>& pulses,
                                      uint32_t frameGapUs = 6000);

  // Lowest Hamming distance of needle sliding over haystack, normalized 0..1
  static float bitDistance(const String& haystack, const String& needle);

  // Sliding Hamming of a short core signature inside haystack (0..1)
  static float coreDistance(const String& haystack, const String& core);

  // Find the smallest repeating period inside a long bit string (hold capture)
  static String smallestBitPeriod(const String& bits);

 private:
  static std::vector<TimingCluster> clusterTimings(const std::vector<uint32_t>& durations,
                                                   float tolerance = 0.25f);
  static String heuristicDecode(const std::vector<int32_t>& pulses,
                                uint32_t shortUs,
                                uint32_t longUs);
};
