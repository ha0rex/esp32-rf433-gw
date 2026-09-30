#include "RFAnalyzer.h"
#include <algorithm>
#include <cmath>

std::vector<TimingCluster> RFAnalyzer::clusterTimings(const std::vector<uint32_t>& durations,
                                                      float tolerance) {
  std::vector<TimingCluster> clusters;
  std::vector<uint32_t> sorted = durations;
  std::sort(sorted.begin(), sorted.end());

  for (uint32_t d : sorted) {
    if (d < 50 || d > 200000) continue;  // ignore noise / huge gaps
    bool placed = false;
    for (auto& c : clusters) {
      float tol = max(50.0f, (float)c.centerUs * tolerance);
      if (fabsf((float)d - (float)c.centerUs) <= tol) {
        // Running average
        uint64_t sum = (uint64_t)c.centerUs * c.count + d;
        c.count++;
        c.centerUs = (uint32_t)(sum / c.count);
        if (d < c.minUs) c.minUs = d;
        if (d > c.maxUs) c.maxUs = d;
        placed = true;
        break;
      }
    }
    if (!placed) {
      TimingCluster c;
      c.centerUs = d;
      c.count = 1;
      c.minUs = d;
      c.maxUs = d;
      clusters.push_back(c);
    }
  }

  std::sort(clusters.begin(), clusters.end(),
            [](const TimingCluster& a, const TimingCluster& b) { return a.count > b.count; });
  return clusters;
}

uint32_t RFAnalyzer::estimateRepeats(const std::vector<int32_t>& pulses,
                                     uint32_t& frameDurationUs,
                                     uint32_t silenceGapUs) {
  frameDurationUs = 0;
  if (pulses.size() < 8) return 1;

  // Split into frames by long LOW gaps
  std::vector<uint32_t> frameLens;
  uint32_t frameStart = 0;
  uint32_t acc = 0;
  for (size_t i = 0; i < pulses.size(); i++) {
    uint32_t dur = (uint32_t)abs(pulses[i]);
    acc += dur;
    bool gap = (pulses[i] < 0 && dur >= silenceGapUs);
    bool last = (i + 1 == pulses.size());
    if (gap || last) {
      uint32_t len = 0;
      size_t end = gap ? i : i + 1;
      for (size_t j = frameStart; j < end; j++) {
        len += (uint32_t)abs(pulses[j]);
      }
      if (len > 200) {
        frameLens.push_back(len);
      }
      frameStart = i + 1;
      acc = 0;
    }
  }

  if (frameLens.empty()) {
    for (int32_t p : pulses) frameDurationUs += (uint32_t)abs(p);
    return 1;
  }

  // Median frame length
  std::sort(frameLens.begin(), frameLens.end());
  frameDurationUs = frameLens[frameLens.size() / 2];
  return (uint32_t)frameLens.size();
}

String RFAnalyzer::heuristicDecode(const std::vector<int32_t>& pulses,
                                   uint32_t shortUs,
                                   uint32_t longUs) {
  if (shortUs == 0 || longUs == 0 || shortUs >= longUs) return "";

  String bits;
  bits.reserve(pulses.size() / 2);
  // Try classic: 0 = short HIGH + long LOW, 1 = long HIGH + short LOW
  for (size_t i = 0; i + 1 < pulses.size(); ) {
    int32_t a = pulses[i];
    int32_t b = pulses[i + 1];
    if (a <= 0 || b >= 0) {
      i++;
      continue;
    }
    uint32_t hi = (uint32_t)a;
    uint32_t lo = (uint32_t)(-b);
    float mid = (shortUs + longUs) / 2.0f;
    bool hiShort = hi < mid;
    bool loShort = lo < mid;
    if (hiShort && !loShort) {
      bits += '0';
      i += 2;
    } else if (!hiShort && loShort) {
      bits += '1';
      i += 2;
    } else {
      i++;
    }
    if (bits.length() > 256) break;
  }
  return bits;
}

SignalAnalysis RFAnalyzer::analyze(const std::vector<int32_t>& pulses, uint32_t silenceGapUs) {
  SignalAnalysis a;
  a.pulseCount = pulses.size();
  for (int32_t p : pulses) a.durationUs += (uint32_t)abs(p);
  if (pulses.size() < 4) {
    a.heuristicNote = "Too few pulses for analysis";
    return a;
  }

  std::vector<uint32_t> durations;
  durations.reserve(pulses.size());
  for (int32_t p : pulses) {
    uint32_t d = (uint32_t)abs(p);
    if (d >= 80 && d < silenceGapUs) durations.push_back(d);
  }

  a.clusters = clusterTimings(durations, 0.25f);
  a.valid = a.clusters.size() >= 1;

  if (a.clusters.size() >= 1) {
    // Prefer two most common as short/long
    std::vector<TimingCluster> byCenter = a.clusters;
    std::sort(byCenter.begin(), byCenter.end(),
              [](const TimingCluster& x, const TimingCluster& y) {
                return x.centerUs < y.centerUs;
              });
    a.shortPulseUs = byCenter.front().centerUs;
    a.longPulseUs = byCenter.size() > 1 ? byCenter[1].centerUs : byCenter.front().centerUs * 2;
    if (byCenter.size() >= 3) {
      a.syncPulseUs = byCenter.back().centerUs;
    }
  }

  // Longest LOW as sync gap candidate
  uint32_t maxLow = 0;
  for (int32_t p : pulses) {
    if (p < 0) {
      uint32_t d = (uint32_t)(-p);
      if (d > maxLow && d < 200000) maxLow = d;
    }
  }
  a.syncGapUs = maxLow;

  a.estimatedRepeats = estimateRepeats(pulses, a.frameDurationUs, silenceGapUs);
  a.heuristicBits = heuristicDecode(pulses, a.shortPulseUs, a.longPulseUs);
  a.heuristicNote =
      "Heuristic only — do not assume this encoding matches your remote. "
      "Shown as short-HIGH+long-LOW=0 / long-HIGH+short-LOW=1 when pattern fits.";
  return a;
}

std::vector<std::vector<int32_t>> RFAnalyzer::splitFrames(
    const std::vector<int32_t>& pulses, uint32_t gapUs) {
  std::vector<std::vector<int32_t>> frames;
  std::vector<int32_t> cur;
  for (int32_t p : pulses) {
    const uint32_t d = (uint32_t)abs(p);
    if (p < 0 && d >= gapUs) {
      if (cur.size() >= 16) frames.push_back(cur);
      cur.clear();
      continue;
    }
    if (d < 40 && cur.empty()) continue;
    cur.push_back(p);
  }
  if (cur.size() >= 16) frames.push_back(std::move(cur));
  return frames;
}

std::vector<int32_t> RFAnalyzer::cleanGlitchPulses(const std::vector<int32_t>& pulses,
                                                   uint32_t minEdgeUs) {
  std::vector<int32_t> out;
  out.reserve(pulses.size());
  for (int32_t p : pulses) {
    uint32_t a = (uint32_t)abs(p);
    if (a < minEdgeUs) continue;  // drop glitch
    if (!out.empty() && ((out.back() > 0) == (p > 0))) {
      // Merge consecutive same polarity (glitch removal often joins a split long)
      uint32_t sum = (uint32_t)abs(out.back()) + a;
      out.back() = (out.back() > 0) ? (int32_t)sum : -(int32_t)sum;
    } else {
      out.push_back(p);
    }
  }
  return out;
}

String RFAnalyzer::decodeOokBits(const std::vector<int32_t>& pulses,
                                 uint32_t* midUsOut,
                                 uint32_t fixedMidUs) {
  const std::vector<int32_t> cleaned = cleanGlitchPulses(pulses);
  const std::vector<int32_t>& src = cleaned.size() >= 16 ? cleaned : pulses;

  std::vector<uint32_t> highs;
  highs.reserve(src.size() / 2);
  for (int32_t p : src) {
    if (p > 100 && p < 5000) highs.push_back((uint32_t)p);
  }
  if (highs.size() < 8) {
    if (midUsOut) *midUsOut = 0;
    return "";
  }

  float mid = (float)fixedMidUs;
  if (mid < 80.f) {
    std::sort(highs.begin(), highs.end());
    uint32_t shortUs = highs[highs.size() / 5];
    uint32_t longUs = highs[(highs.size() * 4) / 5];
    if (longUs <= shortUs + 40) {
      shortUs = highs[highs.size() / 3];
      longUs = highs[(highs.size() * 2) / 3];
    }
    mid = (shortUs + longUs) / 2.0f;
    uint32_t sumS = 0, sumL = 0, nS = 0, nL = 0;
    for (uint32_t h : highs) {
      if (h < mid) { sumS += h; nS++; }
      else { sumL += h; nL++; }
    }
    if (nS && nL) mid = (sumS / (float)nS + sumL / (float)nL) / 2.0f;
  } else {
    // Validate a caller-supplied mid — if it sits below/above both clusters,
    // re-estimate. (Remote buttons were saved with mid≈180 while shorts are
    // ≈300µs, which decoded every bit as '1'.)
    uint32_t sumS = 0, sumL = 0, nS = 0, nL = 0;
    for (uint32_t h : highs) {
      if (h < mid) { sumS += h; nS++; }
      else { sumL += h; nL++; }
    }
    const bool useless = (nS < 3 || nL < 3);
    if (useless) {
      std::sort(highs.begin(), highs.end());
      uint32_t shortUs = highs[highs.size() / 5];
      uint32_t longUs = highs[(highs.size() * 4) / 5];
      mid = (shortUs + longUs) / 2.0f;
      sumS = sumL = nS = nL = 0;
      for (uint32_t h : highs) {
        if (h < mid) { sumS += h; nS++; }
        else { sumL += h; nL++; }
      }
      if (nS && nL) mid = (sumS / (float)nS + sumL / (float)nL) / 2.0f;
    }
  }
  if (midUsOut) *midUsOut = (uint32_t)mid;

  String bits;
  bits.reserve(highs.size() + 8);
  for (size_t i = 0; i + 1 < src.size(); ) {
    int32_t a = src[i];
    int32_t b = src[i + 1];
    if (a > 0 && b < 0) {
      uint32_t hi = (uint32_t)a;
      uint32_t lo = (uint32_t)(-b);
      if (lo >= 5000) { i += 2; continue; }  // frame gap
      if (hi >= 80 && hi < 5000) {
        bits += (hi < mid) ? '0' : '1';
      }
      i += 2;
    } else {
      i++;
    }
    if (bits.length() > 512) break;
  }
  return bits;
}

float RFAnalyzer::coreDistance(const String& haystack, const String& core) {
  const int cLen = core.length();
  const int hLen = haystack.length();
  // Allow 16-bit discriminative cores (sibling buttons that only differ by
  // 1–2 bits need a short window to clear the match margin).
  if (cLen < 16 || hLen < cLen) return 1.f;

  float best = 1.f;
  const int maxOff = hLen - cLen;
  for (int off = 0; off <= maxOff; off += (off < 48 ? 1 : 2)) {
    int bad = 0;
    for (int i = 0; i < cLen; i++) {
      if (haystack[off + i] != core[i]) bad++;
    }
    float d = (float)bad / (float)cLen;
    if (d < best) best = d;
    if (best <= 0.02f) return best;
  }
  return best;
}

String RFAnalyzer::smallestBitPeriod(const String& bits) {
  const int n = bits.length();
  if (n < 32) return bits;

  auto entropyOk = [](const String& s) -> bool {
    int ones = 0;
    int flips = 0;
    for (int i = 0; i < (int)s.length(); i++) {
      if (s[i] == '1') ones++;
      if (i > 0 && s[i] != s[i - 1]) flips++;
    }
    const float onesFrac = (float)ones / (float)s.length();
    return flips >= 3 && onesFrac > 0.08f && onesFrac < 0.92f;
  };

  // Allow 1 + partial repeat (hold often captures ~1.5 frames on gapless remotes)
  for (int p = 16; p <= n - 16; p++) {
    int bad = 0;
    int tot = 0;
    for (int i = 0; i + p < n; i++) {
      if (bits[i] != bits[i + p]) bad++;
      tot++;
    }
    if (tot < 16) continue;
    if ((float)bad / (float)tot <= 0.12f) {
      String unit = bits.substring(0, p);
      if (entropyOk(unit)) return unit;
    }
  }
  return bits;
}

RepeatUnit RFAnalyzer::extractRepeatUnit(const std::vector<int32_t>& pulsesIn,
                                         uint32_t frameGapUs) {
  RepeatUnit out;
  const std::vector<int32_t> cleaned = cleanGlitchPulses(pulsesIn);
  const std::vector<int32_t>& pulses =
      cleaned.size() >= 32 ? cleaned : pulsesIn;
  if (pulses.size() < 40) return out;

  auto frames = splitFrames(pulses, frameGapUs);
  // Doorbell-style: clear inter-frame gaps. Try a few gap sizes.
  if (frames.size() < 2) {
    auto alt = splitFrames(pulses, 4000);
    if (alt.size() > frames.size()) frames = std::move(alt);
  }
  if (frames.size() < 2) {
    auto alt = splitFrames(pulses, 3500);
    if (alt.size() > frames.size()) frames = std::move(alt);
  }
  if (frames.size() < 2) {
    auto alt = splitFrames(pulses, 9000);
    if (alt.size() > frames.size()) frames = std::move(alt);
  }
  if (frames.empty()) {
    frames.push_back(pulses);
  }

  std::vector<size_t> idxs;
  idxs.reserve(frames.size());
  for (size_t i = 0; i < frames.size(); i++) {
    if (frames[i].size() >= 40) idxs.push_back(i);
  }
  if (idxs.empty()) return out;

  size_t longest = idxs[0];
  for (size_t i : idxs) {
    if (frames[i].size() > frames[longest].size()) longest = i;
  }

  // Always auto-estimate mid from the longest frame (ignore bad saved mids)
  uint32_t mid = 0;
  String longBits = decodeOokBits(frames[longest], &mid, 0);
  if (!mid || longBits.length() < 24) return out;

  struct Cand {
    size_t idx;
    String bits;
  };
  std::vector<Cand> cands;
  cands.reserve(idxs.size());

  if (idxs.size() >= 2) {
    // Gapped repeats (doorbell): consensus across frames
    for (size_t i : idxs) {
      String b = decodeOokBits(frames[i], nullptr, mid);
      if (b.length() < 16) continue;
      cands.push_back(Cand{i, std::move(b)});
      delay(0);
    }
  } else {
    // Gapless remote: whole capture is one blob — find period inside the bits
    cands.push_back(Cand{longest, longBits});
  }
  if (cands.empty()) return out;

  int bestScore = -1;
  size_t bestCi = 0;
  for (size_t ci = 0; ci < cands.size(); ci++) {
    int score = 0;
    for (size_t cj = 0; cj < cands.size(); cj++) {
      if (ci == cj) {
        score++;
        continue;
      }
      float d = bitDistance(cands[cj].bits, cands[ci].bits);
      if (d <= 0.18f) score++;
    }
    if (score > bestScore ||
        (score == bestScore &&
         cands[ci].bits.length() < cands[bestCi].bits.length())) {
      bestScore = score;
      bestCi = ci;
    }
  }

  String frameBits = cands[bestCi].bits;
  String unitBits = smallestBitPeriod(frameBits);

  // If period shrink failed on a long gapless stream, try the raw full decode
  if (unitBits.length() == frameBits.length() && frameBits.length() > 80) {
    String alt = smallestBitPeriod(longBits);
    if (alt.length() < unitBits.length()) unitBits = alt;
  }

  // Build pulse unit: ~2 pulses per bit (HIGH+LOW), from a stable interior offset
  std::vector<int32_t> src = frames[cands[bestCi].idx];
  std::vector<int32_t> unitPulses;
  const int bitsWanted = (int)unitBits.length();
  if (bitsWanted >= 16 && bitsWanted * 2 + 4 < (int)src.size()) {
    // Skip a short noisy preamble if present; take one period of pairs
    size_t start = 0;
    // Align to first HIGH
    while (start < src.size() && src[start] <= 0) start++;
    const size_t need = (size_t)bitsWanted * 2;
    if (start + need <= src.size()) {
      unitPulses.assign(src.begin() + (ptrdiff_t)start,
                        src.begin() + (ptrdiff_t)(start + need));
    }
  }
  if (unitPulses.size() < 32) unitPulses = src;

  uint32_t gapSum = 0;
  uint32_t gapCount = 0;
  for (int32_t p : pulses) {
    if (p >= 0) continue;
    uint32_t d = (uint32_t)(-p);
    if (d >= 3000 && d <= 80000) {
      gapSum += d;
      gapCount++;
    }
  }

  // repeats ≈ how many times the unit fits in the captured bits
  uint32_t reps = 1;
  if (unitBits.length() >= 16) {
    reps = (uint32_t)std::max(1, (int)frameBits.length() / (int)unitBits.length());
    if ((int)bestScore > (int)reps) reps = (uint32_t)bestScore;
  }

  out.valid = unitBits.length() >= 16 && unitPulses.size() >= 32;
  out.pulses = std::move(unitPulses);
  out.bitCode = unitBits;
  out.midUs = (uint16_t)mid;
  out.repeats = reps;
  out.gapUs = gapCount ? (gapSum / gapCount) : 10000;

  Serial.printf("[RF] repeat-unit bits=%u (frame=%u) reps≈%u gap=%u mid=%u pulses=%u\n",
                (unsigned)out.bitCode.length(),
                (unsigned)frameBits.length(),
                (unsigned)out.repeats,
                (unsigned)out.gapUs,
                (unsigned)out.midUs,
                (unsigned)out.pulses.size());
  return out;
}

float RFAnalyzer::bitDistance(const String& haystack, const String& needle) {
  const int nLen = needle.length();
  const int hLen = haystack.length();
  if (nLen < 16 || hLen < 16) return 1.f;

  float best = 1.f;

  // Case A: haystack long enough — slide full needle
  if (hLen >= nLen) {
    for (int off = 0; off <= hLen - nLen; off++) {
      int bad = 0;
      for (int i = 0; i < nLen; i++) {
        if (haystack[off + i] != needle[i]) bad++;
      }
      float d = (float)bad / (float)nLen;
      if (d < best) best = d;
      if (best <= 0.02f) return best;
    }
  }

  // Short needles in a long stream: also score multi-repeat alignment.
  // (Doorbell frames repeat; PWM remotes with ~32-bit units do too.)
  // Take the better of slide vs repeat — do not require repeats exclusively,
  // or a mid-frame start on a 2-button remote fails entirely.
  if (nLen <= 36 && hLen >= nLen * 2) {
    for (int off = 0; off <= hLen - 2 * nLen; off++) {
      const int reps = std::min(4, (hLen - off) / nLen);
      if (reps < 2) continue;
      int bad = 0;
      int tot = 0;
      for (int r = 0; r < reps; r++) {
        for (int i = 0; i < nLen; i++) {
          if (haystack[off + r * nLen + i] != needle[i]) bad++;
          tot++;
        }
      }
      float d = (float)bad / (float)tot;
      if (d < best) best = d;
      if (best <= 0.02f) return best;
    }
  }

  if (hLen >= nLen) return best;

  // Case B: partial capture — rotate needle (gapless remotes, mid-frame start)
  const int minOverlap = std::max(24, (nLen * 2) / 3);
  if (hLen < minOverlap) return 1.f;

  for (int rot = 0; rot < nLen; rot++) {
    int bad = 0;
    for (int i = 0; i < hLen; i++) {
      if (haystack[i] != needle[(rot + i) % nLen]) bad++;
    }
    float d = (float)bad / (float)hLen;
    if (d < best) best = d;
    if (best <= 0.02f) return best;
  }
  return best;
}
