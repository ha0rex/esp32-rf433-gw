#include "ActionRunner.h"
#include "../radio/RFTransmitter.h"
#include <algorithm>

bool ActionRunner::start(const RfAction& action) {
  if (active_) return false;
  if (action.steps.empty()) return false;
  action_ = action;
  active_ = true;
  stepIndex_ = -1;
  waitingTx_ = false;
  delayUntilMs_ = 0;
  settleUntilMs_ = 0;
  message_ = "Starting";
  Serial.printf("[Action] start '%s' steps=%u\n", action_.name.c_str(),
                (unsigned)action_.steps.size());
  return beginStep(0);
}

bool ActionRunner::startById(const String& id, ActionStorage& store) {
  RfAction a;
  if (!store.load(id, a)) return false;
  return start(a);
}

bool ActionRunner::startSignal(const String& signalId) {
  if (signalId.isEmpty() || !signals_.exists(signalId)) return false;
  RfAction a;
  a.id = String("sig:") + signalId;
  a.name = signalId;
  ActionStep s;
  s.type = "tx";
  s.signalId = signalId;
  s.repeats = 0;  // use signal default (same as replay)
  a.steps.push_back(s);
  return start(a);
}

void ActionRunner::stop() {
  if (!active_) return;
  if (radio_.state() == RadioState::Tx) radio_.stopTx();
  finish(false, "Stopped");
}

void ActionRunner::finish(bool ok, const char* msg) {
  active_ = false;
  waitingTx_ = false;
  delayUntilMs_ = 0;
  settleUntilMs_ = 0;
  message_ = msg;
  Serial.printf("[Action] done ok=%d msg=%s\n", ok ? 1 : 0, msg);
}

bool ActionRunner::queueTx(const RFSignal& sig, const ActionStep& step) {
  TxRequest tx;
  tx.frequencyMHz = sig.frequencyMHz;
  tx.modulation = sig.modulation;
  tx.pulses = sig.pulses;
  // Match signal replay exactly (fallback only if gap unset)
  tx.gapBetweenRepeatsUs =
      sig.gapBetweenRepeatsUs ? sig.gapBetweenRepeatsUs : 10000;

  if (step.type == "hold") {
    float sec = step.holdSec;
    if (sec < 0.1f) sec = 0.1f;
    if (sec > 10.f) sec = 10.f;
    tx.holdMs = (uint32_t)(sec * 1000.f + 0.5f);
    tx.maxDurationMs = tx.holdMs + 500;
    message_ = "Hold " + sig.name + " " + String(sec, 1) + "s";
  } else {
    // repeats==0 → same default as /api/signal/replay
    uint32_t reps = step.repeats;
    if (reps == 0) reps = sig.estimatedRepeats ? sig.estimatedRepeats : 3;
    if (reps < 1) reps = 1;
    if (reps > 50) reps = 50;
    tx.repeats = reps;
    tx.holdMs = 0;
    message_ = "TX " + sig.name + " ×" + String(reps);
  }

  if (!radio_.startTx(tx)) {
    message_ = "TX busy/failed";
    Serial.printf("[Action] startTx failed state=%d\n", (int)radio_.state());
    return false;
  }
  waitingTx_ = true;
  delayUntilMs_ = 0;
  settleUntilMs_ = 0;
  return true;
}

bool ActionRunner::beginStep(int index) {
  stepIndex_ = index;
  if (index < 0 || index >= (int)action_.steps.size()) {
    finish(true, "Complete");
    return true;
  }

  const ActionStep& step = action_.steps[index];
  Serial.printf("[Action] step %d/%d type=%s sig=%s\n", index + 1,
                (int)action_.steps.size(), step.type.c_str(),
                step.signalId.c_str());

  if (step.type == "delay") {
    float sec = step.delaySec;
    if (sec < 0) sec = 0;
    if (sec > 120) sec = 120;
    uint32_t ms = (uint32_t)(sec * 1000.f + 0.5f);
    delayUntilMs_ = millis() + ms;
    waitingTx_ = false;
    settleUntilMs_ = 0;
    message_ = "Waiting " + String(sec, 1) + "s";
    return true;
  }

  if (step.signalId.isEmpty()) {
    message_ = "Missing signal";
    Serial.println("[Action] empty signalId — skip");
    return beginStep(index + 1);
  }

  RFSignal sig;
  if (!signals_.load(step.signalId, sig) || sig.pulses.empty()) {
    message_ = "Signal not found";
    Serial.printf("[Action] missing/empty signal %s\n", step.signalId.c_str());
    return beginStep(index + 1);
  }

  // Ensure listen mode is yielded before TX (startTx also does this)
  radio_.yieldHkListen();
  if (radio_.state() != RadioState::Idle && radio_.state() != RadioState::Error) {
    // Still busy — brief wait handled by retry via settle
    Serial.printf("[Action] radio busy state=%d — retry\n", (int)radio_.state());
    if (radio_.state() == RadioState::Tx) radio_.stopTx();
    radio_.yieldHkListen();
  }

  if (!queueTx(sig, step)) {
    finish(false, "TX failed");
    return false;
  }
  return true;
}

void ActionRunner::loop() {
  if (!active_) return;

  if (settleUntilMs_) {
    if ((int32_t)(millis() - settleUntilMs_) < 0) return;
    settleUntilMs_ = 0;
    beginStep(stepIndex_ + 1);
    return;
  }

  if (waitingTx_) {
    if (radio_.state() == RadioState::Tx) return;
    waitingTx_ = false;
    // Let the CC1101 and target device settle between hold → command
    settleUntilMs_ = millis() + 120;
    message_ = "Settling…";
    return;
  }

  if (delayUntilMs_) {
    if ((int32_t)(millis() - delayUntilMs_) < 0) return;
    delayUntilMs_ = 0;
    beginStep(stepIndex_ + 1);
  }
}

void ActionRunner::toJson(JsonObject o) const {
  o["active"] = active_;
  o["actionId"] = action_.id;
  o["actionName"] = action_.name;
  o["step"] = stepIndex_ + 1;
  o["steps"] = (int)action_.steps.size();
  o["message"] = message_;
}
