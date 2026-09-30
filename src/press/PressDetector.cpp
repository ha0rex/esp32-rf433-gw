#include "PressDetector.h"
#include "../homekit/HomeKitManager.h"
#include "../radio/RFAnalyzer.h"
#include "../radio/WeatherDecoder.h"
#include <algorithm>

void PressDetector::begin() {
  if (begun_) return;
  begun_ = true;
  reloadTemplates();
  Serial.printf("[Press] ready — %u templates (core matcher)\n",
                (unsigned)templates_.size());
}

void PressDetector::buildCores() {
  // Prefer the shortest window that still separates siblings in the SAME
  // timing family. Cross-family templates (doorbell vs PWM remote) are ignored
  // so a new signal cannot reshape unrelated cores.
  auto midCompatible = [](uint16_t a, uint16_t b) -> bool {
    if (!a || !b) return true;
    const uint16_t lo = a < b ? a : b;
    const uint16_t hi = a < b ? b : a;
    if ((uint16_t)(hi - lo) <= 120) return true;
    return hi <= (uint32_t)lo * 14 / 10;
  };

  auto windowEntropyOk = [](const String& s) -> bool {
    if (s.length() < 16) return false;
    int ones = 0, flips = 0;
    for (int i = 0; i < (int)s.length(); i++) {
      if (s[i] == '1') ones++;
      if (i > 0 && s[i] != s[i - 1]) flips++;
    }
    float f = (float)ones / (float)s.length();
    // Reject all-zero / all-one cores — they match noise from other protocols
    // when that protocol is decoded with the wrong mid.
    return flips >= 2 && f >= 0.15f && f <= 0.85f;
  };

  for (size_t ti = 0; ti < templates_.size(); ti++) {
    auto& t = templates_[ti];
    if (t.bitCodes.empty()) continue;
    const String* bestCode = &t.bitCodes[0];
    // Prefer a moderate-length code (one frame), not a 300-bit hold blob
    for (const auto& c : t.bitCodes) {
      if (c.length() < 16) continue;
      if (c.length() <= 96 &&
          (bestCode->length() > 96 || c.length() > bestCode->length())) {
        bestCode = &c;
      } else if (bestCode->length() > 96 && c.length() > bestCode->length()) {
        bestCode = &c;
      }
    }
    // If still a long blob, keep it but cores will slide inside it
    if (bestCode->length() < 16) {
      t.core = *bestCode;
      continue;
    }

    int bestStart = 0;
    int bestW = std::min(24, (int)bestCode->length());
    float bestSep = -1.f;
    float bestEnt = -1.f;

    const int widths[] = {16, 20, 24, 32};
    for (int W : widths) {
      if ((int)bestCode->length() < W) continue;
      const int maxStart = (int)bestCode->length() - W;
      for (int s = 0; s <= maxStart; s++) {
        String window = bestCode->substring(s, s + W);
        if (!windowEntropyOk(window)) continue;

        float minSep = 1.f;
        bool anySibling = false;
        for (size_t oj = 0; oj < templates_.size(); oj++) {
          if (oj == ti) continue;
          if (!midCompatible(t.midUs, templates_[oj].midUs)) continue;
          anySibling = true;
          for (const auto& other : templates_[oj].bitCodes) {
            float d = 1.f;
            if ((int)other.length() >= W) {
              for (int os = 0; os + W <= (int)other.length(); os++) {
                int bad = 0;
                for (int i = 0; i < W; i++) {
                  if (other[os + i] != window[i]) bad++;
                }
                float hd = (float)bad / (float)W;
                if (hd < d) d = hd;
              }
            } else {
              d = RFAnalyzer::coreDistance(other, window);
            }
            if (d < minSep) minSep = d;
          }
        }
        if (!anySibling) minSep = 1.f;

        int ones = 0;
        for (int i = 0; i < W; i++) if (window[i] == '1') ones++;
        float ent = (float)ones / (float)W;
        // Prefer balanced entropy when sep ties (solo templates)
        float entScore = 1.f - fabsf(ent - 0.5f) * 2.f;

        // Prefer longer windows when separation ties — short address-only
        // cores collapse EV1527 sibling buttons (same 20-bit id, different data).
        if (minSep > bestSep + 0.001f ||
            (minSep >= bestSep - 0.001f && W > bestW && entScore >= bestEnt - 0.05f) ||
            (minSep >= bestSep - 0.001f && entScore > bestEnt + 0.05f) ||
            (minSep >= bestSep - 0.001f && entScore >= bestEnt - 0.02f &&
             W == bestW && s < bestStart)) {
          bestSep = minSep;
          bestStart = s;
          bestW = W;
          bestEnt = entScore;
        }
      }
      delay(0);
    }

    if (bestEnt < 0) {
      // Fallback: first entropy-ok window, else raw prefix
      bool found = false;
      for (int W : widths) {
        if ((int)bestCode->length() < W) continue;
        for (int s = 0; s + W <= (int)bestCode->length(); s++) {
          String w = bestCode->substring(s, s + W);
          if (windowEntropyOk(w)) {
            t.core = w;
            found = true;
            break;
          }
        }
        if (found) break;
      }
      if (!found) t.core = bestCode->substring(0, std::min(24, (int)bestCode->length()));
    } else {
      t.core = bestCode->substring(bestStart, bestStart + bestW);
    }
    Serial.printf("[Press] core '%s' start=%d sep=%.2f len=%u mid=%u\n",
                  t.buttonName.c_str(), bestStart, bestSep,
                  (unsigned)t.core.length(), (unsigned)t.midUs);
  }
}

void PressDetector::reloadTemplates() {
  templates_.clear();

  auto addFromSignal = [&](RFSignal& sig, const String& remoteId,
                           const String& remoteName, const String& buttonName) {
    Template t;
    t.signalId = sig.id;
    t.signalName = sig.name;
    t.remoteId = remoteId;
    t.remoteName = remoteName;
    t.buttonName = buttonName;

    auto frames = RFAnalyzer::splitFrames(sig.pulses, kFrameGapUs);
    // Gapless remotes: also try a tighter gap, else treat whole capture as one frame
    if (frames.size() < 2) {
      auto alt = RFAnalyzer::splitFrames(sig.pulses, 3500);
      if (alt.size() > frames.size()) frames = std::move(alt);
    }
    if (frames.empty() && sig.pulses.size() >= 32) frames.push_back(sig.pulses);

    // Always re-estimate mid from pulses — saved mid may be wrong (too low)
    const std::vector<int32_t>* longest = nullptr;
    for (const auto& fr : frames) {
      if (!longest || fr.size() > longest->size()) longest = &fr;
    }
    uint32_t mid = 0;
    if (longest) {
      RFAnalyzer::decodeOokBits(*longest, &mid, 0);
    }
    if (!mid) mid = sig.bitMidUs;
    t.midUs = (uint16_t)mid;

    auto entropyOk = [](const String& s) -> bool {
      if (s.length() < 16) return false;
      int ones = 0, flips = 0;
      for (int i = 0; i < (int)s.length(); i++) {
        if (s[i] == '1') ones++;
        if (i > 0 && s[i] != s[i - 1]) flips++;
      }
      float f = (float)ones / (float)s.length();
      // PWM remotes (C3/C4) often have a single block of 1s → only 2 flips.
      // Require ones-fraction (rejects all-0 / all-1 from a bad mid); flips>=2.
      return flips >= 2 && f > 0.12f && f < 0.88f;
    };

    auto pushCode = [&](const String& bits) {
      if (bits.length() < 16 || !entropyOk(bits)) return;
      // Prefer a single-frame fingerprint over a multi-second hold blob
      String store = bits;
      if (bits.length() > 96) {
        String unit = RFAnalyzer::smallestBitPeriod(bits);
        if (unit.length() >= 16 && unit.length() <= 96 && entropyOk(unit)) {
          store = unit;
        } else {
          // Densest balanced 32-bit window inside the blob
          int bestS = 0;
          float bestScore = -1.f;
          const int W = 32;
          for (int s = 0; s + W <= (int)bits.length(); s++) {
            int ones = 0, flips = 0;
            for (int i = 0; i < W; i++) {
              if (bits[s + i] == '1') ones++;
              if (i > 0 && bits[s + i] != bits[s + i - 1]) flips++;
            }
            float f = (float)ones / (float)W;
            if (flips < 3 || f < 0.15f || f > 0.85f) continue;
            float score = (1.f - fabsf(f - 0.5f) * 2.f) + flips * 0.01f;
            if (score > bestScore) { bestScore = score; bestS = s; }
          }
          if (bestScore > 0) store = bits.substring(bestS, bestS + W);
          else return;  // refuse to keep a useless long zero-ish blob
        }
      }
      for (const auto& existing : t.bitCodes) {
        if (existing == store) return;
      }
      t.bitCodes.push_back(store);
    };

    // Keep saved codes only if they look real (not all-0s from a bad mid)
    for (const auto& c : sig.bitCodes) {
      pushCode(c);
    }

    for (const auto& fr : frames) {
      pushCode(RFAnalyzer::decodeOokBits(fr, nullptr, mid));
    }

    // Gapless: also store the smallest repeating unit as a fingerprint
    if (longest) {
      String full = RFAnalyzer::decodeOokBits(*longest, nullptr, mid);
      pushCode(full);
      if (full.length() >= 48) {
        RepeatUnit unit = RFAnalyzer::extractRepeatUnit(sig.pulses, kFrameGapUs);
        if (unit.valid) {
          pushCode(unit.bitCode);
          // Only adopt unit mid if it's in the same ballpark (avoid doorbell
          // contamination of EV1527 mids etc.)
          if (unit.midUs && t.midUs) {
            const uint16_t lo = unit.midUs < t.midUs ? unit.midUs : t.midUs;
            const uint16_t hi = unit.midUs < t.midUs ? t.midUs : unit.midUs;
            if ((uint16_t)(hi - lo) <= 120 || hi <= (uint32_t)lo * 14 / 10) {
              t.midUs = unit.midUs;
            }
          } else if (unit.midUs) {
            t.midUs = unit.midUs;
          }
        }
      }
    }

    if (t.bitCodes.empty()) {
      Serial.printf("[Press] skip '%s' — no decodable bits\n", buttonName.c_str());
      return;
    }
    Serial.printf("[Press] btn '%s'/'%s' codes=%u mid=%u\n",
                  remoteName.c_str(), buttonName.c_str(),
                  (unsigned)t.bitCodes.size(), (unsigned)t.midUs);
    templates_.push_back(std::move(t));
  };

  for (const auto& remote : remotes_.list()) {
    for (const auto& btn : remote.buttons) {
      auto addBtnSignal = [&](const String& sid) {
        if (sid.isEmpty()) return;
        RFSignal sig;
        if (!signals_.load(sid, sig)) {
          Serial.printf("[Press] MISSING signal %s for button '%s' — re-pair!\n",
                        sid.c_str(), btn.name.c_str());
          return;
        }
        addFromSignal(sig, remote.id, remote.name, btn.name);
      };
      addBtnSignal(btn.signalId);
      addBtnSignal(btn.secondarySignalId);
    }
  }

  // Always keep every saved signal as a match template. Creating a remote with
  // one button used to drop all other signals from matching (C4/Doorbell
  // vanished → C4 presses falsely hit the only remaining template).
  for (auto& sig : signals_.list(true)) {
    bool seen = false;
    for (const auto& t : templates_) {
      if (t.signalId == sig.id) { seen = true; break; }
    }
    if (seen) continue;
    // EV1527 frames are ~48 edges; allow slightly short captures if bits decode.
    if (sig.pulses.size() < 32) {
      RFSignal full;
      if (!signals_.load(sig.id, full) || full.pulses.size() < 32) continue;
      addFromSignal(full, "", "", full.name);
    } else {
      addFromSignal(sig, "", "", sig.name);
    }
  }

  buildCores();
  wantWeather_ = false;
  for (const auto& remote : remotes_.list()) {
    if (remote.isWeather()) { wantWeather_ = true; break; }
  }
  Serial.printf("[Press] templates reloaded: %u weather=%d\n",
                (unsigned)templates_.size(), wantWeather_ ? 1 : 0);
}

void PressDetector::resetCapture() {
  radio_.capture().stop();
  radio_.capture().reset();
  radio_.capture().start();
  lastPulseCount_ = 0;
  lastTryPulseCount_ = 0;
  burstPeakRssi_ = -128;
  inBurst_ = false;
  lastPulseMs_ = millis();
  lastTryMs_ = millis();
}

void PressDetector::recordEvent(PressEvent ev) {
  ev.seq = ++eventSeq_;
  ev.atMs = millis();
  recent_.push_back(ev);
  while (recent_.size() > kMaxRecent) recent_.erase(recent_.begin());

  if (ev.matched) {
    Serial.printf("[Press] MATCH btn='%s' rssi=%d pulses=%u hk=%d\n",
                  ev.buttonName.c_str(), ev.rssiDbm, ev.pulseCount,
                  ev.homekitFired ? 1 : 0);
  } else {
    Serial.printf("[Press] unmatched rssi=%d pulses=%u\n",
                  ev.rssiDbm, ev.pulseCount);
  }
}

const PressDetector::Template* PressDetector::matchDecoded(const String& bits,
                                                          bool gappedBurst,
                                                          float* distOut,
                                                          uint16_t decodeMidUs) const {
  if (bits.length() < 16 || templates_.empty()) {
    if (distOut) *distOut = 1.f;
    return nullptr;
  }

  auto midCompatible = [](uint16_t a, uint16_t b) -> bool {
    if (!a || !b) return true;
    const uint16_t lo = a < b ? a : b;
    const uint16_t hi = a < b ? b : a;
    // Same timing family: within 120µs or ~40%
    if ((uint16_t)(hi - lo) <= 120) return true;
    return hi <= (uint32_t)lo * 14 / 10;
  };

  const Template* best = nullptr;
  float bestDist = 1.f;
  float secondDist = 1.f;
  bool bestIsLong = false;
  size_t scored = 0;

  for (const auto& t : templates_) {
    // Timing-family gate: doorbell mid must not score against PWM templates.
    if (decodeMidUs > 0 && t.midUs > 0 &&
        !midCompatible(decodeMidUs, t.midUs)) {
      continue;
    }

    const String* bestCode = nullptr;
    for (const auto& code : t.bitCodes) {
      if (code.length() < 16 || code.length() > 96) continue;
      if (!bestCode || code.length() > bestCode->length()) bestCode = &code;
    }
    if (!bestCode) continue;

    // Doorbell = short code + low mid (~180–320). PWM remotes (~2ms mid) stay.
    const bool doorbellLike =
        (int)bestCode->length() <= 32 && t.midUs > 0 && t.midUs < 360;
    if (doorbellLike && !gappedBurst) {
      continue;
    }

    float d = RFAnalyzer::bitDistance(bits, *bestCode);
    // Soften with a short core only for LONG codes (rolling / multi-frame).
    // EV1527 siblings share a 20-bit address and differ by 1–2 button bits —
    // a 16-bit address core makes every button look identical (dist 0).
    const bool shortFixed = bestCode->length() <= 40;
    if (!doorbellLike && !shortFixed && t.core.length() >= 16) {
      d = std::min(d, RFAnalyzer::coreDistance(bits, t.core));
    }
    scored++;

    if (d < bestDist) {
      secondDist = bestDist;
      bestDist = d;
      best = &t;
      bestIsLong = bestCode->length() > 40;
    } else if (d < secondDist) {
      secondDist = d;
    }
  }

  if (distOut) *distOut = bestDist;
  if (!best) return nullptr;

  // Short doorbell codes need a tighter match than 65-bit remotes
  bool bestDoorbell = false;
  for (const auto& c : best->bitCodes) {
    if (c.length() >= 16 && c.length() <= 32 && best->midUs > 0 &&
        best->midUs < 360) {
      bestDoorbell = true;
      break;
    }
  }
  // Short PWM (EV1527): siblings differ by ~2/24 ≈ 0.083. Cap below that so
  // a missing sibling template cannot steal the press (C3 → C2).
  const float maxDist = bestDoorbell ? 0.06f
                       : bestIsLong  ? kMaxSiblingDist
                                     : 0.05f;
  if (bestDist > maxDist) return nullptr;

  // Exact / 0-bit: accept (also covers duplicate recordings of the same button).
  if (bestDist <= 0.02f) return best;

  if (scored > 1) {
    const float gap = secondDist - bestDist;
    const float minGap = bestIsLong ? kMinSiblingMargin : kMinCoreMargin;
    if (gap < minGap) return nullptr;
  }
  return best;
}

bool PressDetector::tryMatchPulses(const std::vector<int32_t>& pulses) {
  if (pulses.size() < 48) return false;

  // Doorbell = many short frames with ~5ms gaps. Remotes = one gapless blob.
  auto gapFrames = RFAnalyzer::splitFrames(pulses, 4000);
  if (gapFrames.size() < 2) {
    auto alt = RFAnalyzer::splitFrames(pulses, 3500);
    if (alt.size() > gapFrames.size()) gapFrames = std::move(alt);
  }
  const bool gappedBurst = gapFrames.size() >= 2;

  std::vector<std::vector<int32_t>> windows;

  {
    size_t tailLen = std::min<size_t>(pulses.size(), 320);
    size_t start = pulses.size() - tailLen;
    while (start + 40 < pulses.size() && pulses[start] <= 0) start++;
    windows.emplace_back(pulses.begin() + (ptrdiff_t)start, pulses.end());
  }

  // For gapped bursts, also try individual strong frames (doorbell)
  if (gappedBurst) {
    const std::vector<int32_t>* longest = nullptr;
    for (const auto& fr : gapFrames) {
      if (fr.size() < 30) continue;
      if (!longest || fr.size() > longest->size()) longest = &fr;
    }
    if (longest) windows.push_back(*longest);
  } else {
    auto frames = RFAnalyzer::splitFrames(pulses, kFrameGapUs);
    const std::vector<int32_t>* longest = nullptr;
    for (const auto& fr : frames) {
      if (fr.size() < 40) continue;
      if (!longest || fr.size() > longest->size()) longest = &fr;
    }
    if (longest && longest->size() != windows[0].size()) {
      windows.push_back(*longest);
    }
  }

  // Collect mids: auto + one representative per timing family
  uint16_t mids[8];
  size_t nMids = 0;
  mids[nMids++] = 0;
  for (const auto& t : templates_) {
    if (!t.midUs) continue;
    bool near = false;
    for (size_t i = 0; i < nMids; i++) {
      if (!mids[i]) continue;
      const uint16_t lo = mids[i] < t.midUs ? mids[i] : t.midUs;
      const uint16_t hi = mids[i] < t.midUs ? t.midUs : mids[i];
      if ((uint16_t)(hi - lo) <= 120 || hi <= (uint32_t)lo * 14 / 10) {
        near = true;
        break;
      }
    }
    if (!near && nMids < 8) mids[nMids++] = t.midUs;
  }

  for (const auto& win : windows) {
    if (win.size() < 40) continue;

    // Estimate this burst's natural mid first — never force a foreign
    // protocol's mid onto it (B1@C3-mid → all zeros → false C3 match).
    uint32_t autoMid = 0;
    RFAnalyzer::decodeOokBits(win, &autoMid, 0);

    for (size_t mi = 0; mi < nMids; mi++) {
      if (!gappedBurst && mids[mi] > 0 && mids[mi] < 360) continue;

      if (mids[mi] > 0 && autoMid > 0) {
        const uint16_t lo = mids[mi] < autoMid ? mids[mi] : (uint16_t)autoMid;
        const uint16_t hi = mids[mi] < autoMid ? (uint16_t)autoMid : mids[mi];
        const bool near =
            (uint16_t)(hi - lo) <= 120 || hi <= (uint32_t)lo * 14 / 10;
        if (!near) continue;
      }

      uint32_t usedMid = 0;
      String bits = RFAnalyzer::decodeOokBits(win, &usedMid, mids[mi]);
      if (bits.length() < 16) continue;

      // Reject garbage bitstreams (wrong mid → near-all-zero / all-one)
      {
        int ones = 0, flips = 0;
        for (int i = 0; i < (int)bits.length(); i++) {
          if (bits[i] == '1') ones++;
          if (i > 0 && bits[i] != bits[i - 1]) flips++;
        }
        float f = (float)ones / (float)bits.length();
        if (flips < 3 || f < 0.10f || f > 0.90f) continue;
      }

      const uint16_t familyMid =
          mids[mi] > 0 ? mids[mi]
                       : (uint16_t)(usedMid ? usedMid : autoMid);
      float dist = 1.f;
      const Template* hit =
          matchDecoded(bits, gappedBurst, &dist, familyMid);
      if (hit) {
        // Doorbell false triggers: weak long noise blobs (~1500 pulses @ -70)
        // look a bit like a 24-bit code once. Real doorbell = strong, short,
        // many identical gapped frames.
        const bool doorbellLike =
            hit->midUs > 0 && hit->midUs < 360;
        if (doorbellLike) {
          if (burstPeakRssi_ < -65) continue;
          if (pulses.size() > 400) continue;
          if (gapFrames.size() < 3) continue;

          const String* code = nullptr;
          for (const auto& c : hit->bitCodes) {
            if (c.length() < 16 || c.length() > 36) continue;
            if (!code || c.length() > code->length()) code = &c;
          }
          if (!code) continue;

          int frameHits = 0;
          for (const auto& fr : gapFrames) {
            if (fr.size() < 24 || fr.size() > 90) continue;
            String fb = RFAnalyzer::decodeOokBits(fr, nullptr, hit->midUs);
            if (fb.length() < 16) continue;
            float fd = RFAnalyzer::bitDistance(fb, *code);
            if (fd <= 0.08f) frameHits++;
            if (frameHits >= 2) break;
          }
          if (frameHits < 2) continue;
        }

        fireMatch(*hit, burstPeakRssi_, win.size(), dist);
        return true;
      }
      delay(0);
    }
  }
  return false;
}

void PressDetector::fireMatch(const Template& hit, int8_t rssi, size_t pulseCount,
                              float dist) {
  PressEvent ev;
  ev.matched = true;
  ev.rssiDbm = rssi;
  ev.pulseCount = (uint16_t)std::min<size_t>(pulseCount, 65535);
  ev.signalId = hit.signalId;
  ev.signalName = hit.signalName;
  ev.remoteId = hit.remoteId;
  ev.remoteName = hit.remoteName;
  ev.buttonName = hit.buttonName;
  if (homekit_ && !hit.buttonName.isEmpty()) {
    ev.homekitFired = homekit_->fireButtonForSignal(hit.signalId);
    if (!ev.homekitFired) {
      ev.homekitFired = homekit_->triggerSensorForSignal(hit.signalId);
    }
  }
  lastFireMs_ = millis();
  Serial.printf("[Press] dist=%.3f → %s / %s\n", dist,
                hit.remoteName.c_str(), hit.buttonName.c_str());
  recordEvent(ev);
}

void PressDetector::processBursts() {
  if (!radio_.isHkListening()) return;

  radio_.capture().process();
  const int8_t rssi = radio_.currentRssi();
  const size_t pc = radio_.capture().pulseCount();
  const bool pulseActivity = pc > lastPulseCount_;
  if (pulseActivity) {
    lastPulseCount_ = pc;
    lastPulseMs_ = millis();
  }

  if (millis() - lastFireMs_ < kHoldCooldownMs) {
    if (inBurst_ || pc > 0) resetCapture();
    return;
  }

  if (!inBurst_) {
    if (!pulseActivity && pc > 0 && millis() - lastPulseMs_ > 180) {
      resetCapture();
      return;
    }
    // Never start a burst on noise-floor chatter — RSSI must clear the floor.
    if (rssi >= kMinRssiDbm && pulseActivity) {
      if (pc > 200) {
        radio_.capture().stop();
        radio_.capture().reset();
        radio_.capture().start();
        lastPulseCount_ = 0;
        lastTryPulseCount_ = 0;
      }
      inBurst_ = true;
      burstPeakRssi_ = rssi;
      lastPulseMs_ = millis();
      lastTryMs_ = 0;
      lastTryPulseCount_ = 0;
    } else if (pc > 80) {
      // Drain weak noise so the buffer doesn't fill forever
      resetCapture();
    }
    return;
  }

  if (rssi > burstPeakRssi_) burstPeakRssi_ = rssi;

  // Abort weak bursts early (peak never rose above the noise floor)
  if (burstPeakRssi_ < kMinRssiDbm && millis() - lastPulseMs_ > 40) {
    resetCapture();
    return;
  }

  // Match at most every 150ms, and only with a real RF peak
  const bool due =
      burstPeakRssi_ >= kMinRssiDbm &&
      pc >= 120 &&
      (millis() - lastTryMs_ >= 150) &&
      (pc - lastTryPulseCount_ >= 80);

  if (due) {
    lastTryMs_ = millis();
    lastTryPulseCount_ = pc;
    std::vector<int32_t> pulses;
    radio_.capture().getSignedPulses(pulses);
    if (tryMatchPulses(pulses)) {
      resetCapture();
      return;
    }
    delay(0);  // let WiFi/HTTP run between match attempts
  }

  const bool full = pc >= kMaxBurstPulses || radio_.capture().hasOverflow();
  const bool quiet = !pulseActivity && (millis() - lastPulseMs_ >= kPulseSilenceMs);
  if (!full && !quiet) return;

  std::vector<int32_t> pulses;
  radio_.capture().getSignedPulses(pulses);

  if (pulses.size() >= 80 && burstPeakRssi_ >= kMinRssiDbm) {
    if (tryMatchPulses(pulses)) {
      resetCapture();
      return;
    }
    if (tryWeatherPulses(pulses)) {
      resetCapture();
      return;
    }
    if (quiet) {
      PressEvent ev;
      ev.rssiDbm = burstPeakRssi_;
      ev.pulseCount = (uint16_t)std::min<size_t>(pulses.size(), 65535);
      recordEvent(ev);
    }
  }
  resetCapture();
}

bool PressDetector::needListen() const {
  if (runner_ && runner_->busy()) return false;
  return true;
}

bool PressDetector::tryWeatherPulses(const std::vector<int32_t>& pulses) {
  if (millis() - lastWeatherMs_ < 800) return false;
  WeatherReading r = WeatherDecoder::decodePulses(pulses, burstPeakRssi_);
  if (!r.valid) return false;
  lastWeatherMs_ = millis();
  recordWeather(r);
  if (homekit_) homekit_->publishWeather(r);
  return true;
}

void PressDetector::recordWeather(const WeatherReading& r) {
  WeatherReading copy = r;
  copy.atMs = millis();
  recentWeather_.push_back(copy);
  while (recentWeather_.size() > kMaxWeather) recentWeather_.erase(recentWeather_.begin());
  weatherSeq_++;
  Serial.printf("[Weather] %s id=%d ch=%d T=%.1f°C H=%.0f%% rssi=%d\n",
                r.protocol.c_str(), r.sensorId, r.channel, r.tempC, r.humidity,
                r.rssiDbm);
}

void PressDetector::weatherToJson(JsonDocument& doc) const {
  doc["ok"] = true;
  doc["listening"] = listening();
  doc["weatherSeq"] = weatherSeq_;
  JsonArray arr = doc["readings"].to<JsonArray>();
  for (auto it = recentWeather_.rbegin(); it != recentWeather_.rend(); ++it) {
    JsonObject o = arr.add<JsonObject>();
    o["protocol"] = it->protocol;
    o["sensorId"] = it->sensorId;
    o["channel"] = it->channel;
    o["batteryLow"] = it->batteryLow;
    if (!isnan(it->tempC)) o["tempC"] = it->tempC;
    if (!isnan(it->humidity)) o["humidity"] = it->humidity;
    o["rssi"] = it->rssiDbm;
    o["ageMs"] = millis() - it->atMs;
  }
}

void PressDetector::loop() {
  if (!begun_) return;
  if (runner_ && runner_->busy()) {
    if (radio_.isHkListening()) radio_.yieldHkListen();
    return;
  }
  if (radio_.state() != RadioState::Idle && !radio_.isHkListening()) return;
  if (!needListen()) return;

  if (radio_.state() == RadioState::Idle) {
    if (radio_.startHkListen()) {
      lastPulseMs_ = millis();
      inBurst_ = false;
      lastPulseCount_ = 0;
      Serial.println("[Press] listening");
    }
  }

  if (radio_.isHkListening()) processBursts();
}

bool PressDetector::takeNewerThan(uint32_t lastSeq, PressEvent& out) const {
  if (recent_.empty() || eventSeq_ <= lastSeq) return false;
  for (const auto& e : recent_) {
    if (e.seq > lastSeq) {
      out = e;
      return true;
    }
  }
  return false;
}

void PressDetector::toJson(JsonDocument& doc) const {
  doc["ok"] = true;
  doc["listening"] = listening();
  doc["templateCount"] = (int)templates_.size();
  doc["eventSeq"] = eventSeq_;
  doc["matcher"] = "core";
  JsonArray tarr = doc["templates"].to<JsonArray>();
  for (const auto& t : templates_) {
    JsonObject o = tarr.add<JsonObject>();
    o["button"] = t.buttonName;
    o["remote"] = t.remoteName;
    o["signalId"] = t.signalId;
    o["codes"] = (int)t.bitCodes.size();
    o["coreLen"] = (int)t.core.length();
    o["midUs"] = t.midUs;
  }
  JsonArray arr = doc["presses"].to<JsonArray>();
  for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
    JsonObject o = arr.add<JsonObject>();
    o["seq"] = it->seq;
    o["ageMs"] = millis() - it->atMs;
    o["matched"] = it->matched;
    o["homekitFired"] = it->homekitFired;
    o["signalId"] = it->signalId;
    o["signalName"] = it->signalName;
    o["remoteId"] = it->remoteId;
    o["remoteName"] = it->remoteName;
    o["buttonName"] = it->buttonName;
    o["rssi"] = it->rssiDbm;
    o["pulseCount"] = it->pulseCount;
  }

  JsonObject last = doc["lastBySignal"].to<JsonObject>();
  for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
    if (!it->matched || it->signalId.isEmpty()) continue;
    if (last[it->signalId].isNull()) {
      JsonObject o = last[it->signalId].to<JsonObject>();
      o["seq"] = it->seq;
      o["ageMs"] = millis() - it->atMs;
      o["rssi"] = it->rssiDbm;
      o["buttonName"] = it->buttonName;
      o["homekitFired"] = it->homekitFired;
    }
  }
}
