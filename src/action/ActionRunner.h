#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "../storage/ActionStorage.h"
#include "../radio/RadioManager.h"
#include "../storage/SignalStorage.h"

class ActionRunner {
 public:
  ActionRunner(RadioManager& radio, SignalStorage& signals)
      : radio_(radio), signals_(signals) {}

  bool start(const RfAction& action);
  bool startById(const String& id, ActionStorage& store);
  // One-shot TX of a saved signal (same runner queue as Actions)
  bool startSignal(const String& signalId);
  void stop();
  void loop();

  bool busy() const { return active_; }
  String currentActionId() const { return action_.id; }
  String message() const { return message_; }
  int stepIndex() const { return stepIndex_; }
  int stepCount() const { return (int)action_.steps.size(); }

  void toJson(JsonObject o) const;

 private:
  bool beginStep(int index);
  void finish(bool ok, const char* msg);
  bool queueTx(const RFSignal& sig, const ActionStep& step);

  RadioManager& radio_;
  SignalStorage& signals_;
  RfAction action_;
  bool active_ = false;
  int stepIndex_ = -1;
  uint32_t delayUntilMs_ = 0;
  uint32_t settleUntilMs_ = 0;
  bool waitingTx_ = false;
  String message_;
};
