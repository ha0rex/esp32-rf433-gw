#include "HomeKitManager.h"
#include <HomeSpan.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <vector>
#include <cmath>

HomeKitManager* HomeKitManager::self_ = nullptr;

namespace {

uint32_t stableAid(const String& id) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < id.length(); i++) {
    h ^= (uint8_t)id[i];
    h *= 16777619u;
  }
  return (h % 9000) + 2;
}

struct RfProgButton : Service::StatelessProgrammableSwitch {
  SpanCharacteristic* switchEvent = nullptr;
  String buttonName;
  String signalId;
  uint8_t index = 1;
  uint32_t lastFiredMs = 0;

  RfProgButton(const String& name, const String& sigId, uint8_t idx)
      : Service::StatelessProgrammableSwitch() {
    buttonName = name;
    signalId = sigId;
    index = idx;
    switchEvent = new Characteristic::ProgrammableSwitchEvent();
    new Characteristic::ServiceLabelIndex(index);
    new Characteristic::Name(name.c_str());
  }

  bool fireSingle() {
    if (!switchEvent) return false;
    uint8_t v = Characteristic::ProgrammableSwitchEvent::SINGLE_PRESS;
    switchEvent->setVal(v);
    lastFiredMs = millis();
    Serial.printf("[HomeKit] fired SINGLE_PRESS '%s' (index %u) signal=%s\n",
                  buttonName.c_str(), (unsigned)index, signalId.c_str());
    return true;
  }
};

// Controllable On/Off switch — shows up under "An Accessory is Controlled"
struct RfStateSwitch : Service::Switch {
  SpanCharacteristic* power = nullptr;
  String buttonName;
  String signalId;
  String mode;  // stateful | toggle
  uint32_t lastFiredMs = 0;

  RfStateSwitch(const String& name, const String& sigId, const String& hkMode)
      : Service::Switch() {
    buttonName = name;
    signalId = sigId;
    mode = hkMode;
    power = new Characteristic::On(false);
    new Characteristic::Name(name.c_str());
  }

  bool fireFromRf() {
    if (!power) return false;
    if (millis() - lastFiredMs < 400) return false;
    const bool cur = power->getVal();
    bool next = true;
    if (mode == "toggle") {
      next = !cur;
    } else {
      // stateful: each RF press turns On (Home app can turn Off)
      next = true;
    }
    power->setVal(next);
    lastFiredMs = millis();
    Serial.printf("[HomeKit] switch '%s' → %s (%s) signal=%s\n",
                  buttonName.c_str(), next ? "ON" : "OFF", mode.c_str(),
                  signalId.c_str());
    return true;
  }
};

struct RfMotionSensor : Service::MotionSensor {
  SpanCharacteristic* motion = nullptr;
  String signalId;
  String deviceName;
  uint32_t lastFiredMs = 0;
  uint32_t clearAtMs = 0;

  RfMotionSensor(const String& name, const String& sigId)
      : Service::MotionSensor() {
    deviceName = name;
    signalId = sigId;
    motion = new Characteristic::MotionDetected(false);
    new Characteristic::Name(name.c_str());
  }

  bool trigger() {
    if (!motion) return false;
    if (millis() - lastFiredMs < 400) return false;
    motion->setVal(true);
    lastFiredMs = millis();
    clearAtMs = millis() + 1500;
    Serial.printf("[HomeKit] motion '%s' signal=%s\n",
                  deviceName.c_str(), signalId.c_str());
    return true;
  }

  void loop() override {
    if (clearAtMs && millis() >= clearAtMs && motion) {
      motion->setVal(false);
      clearAtMs = 0;
    }
  }
};

struct RfTempSensor : Service::TemperatureSensor {
  SpanCharacteristic* temp = nullptr;
  String deviceId;
  String protocol;
  int sensorId = -1;
  int channel = 0;

  RfTempSensor(const String& name, const String& id, const String& proto,
               int sid, int ch, float initialC)
      : Service::TemperatureSensor() {
    deviceId = id;
    protocol = proto;
    sensorId = sid;
    channel = ch;
    float v = isnan(initialC) ? 20.0f : initialC;
    temp = new Characteristic::CurrentTemperature(v);
    temp->setRange(-50, 100);
    new Characteristic::Name(name.c_str());
  }

  bool matches(const WeatherReading& r) const {
    if (!r.valid) return false;
    if (protocol != "auto" && protocol != r.protocol) return false;
    if (sensorId >= 0 && sensorId != r.sensorId) return false;
    if (channel > 0 && channel != r.channel) return false;
    return !isnan(r.tempC);
  }

  void update(float c) {
    if (!temp || isnan(c)) return;
    float cur = temp->getVal<float>();
    if (fabsf(cur - c) < 0.05f && temp->timeVal() < 30000) return;
    temp->setVal(c);
  }
};

struct RfHumiditySensor : Service::HumiditySensor {
  SpanCharacteristic* hum = nullptr;
  String deviceId;
  String protocol;
  int sensorId = -1;
  int channel = 0;

  RfHumiditySensor(const String& name, const String& id, const String& proto,
                   int sid, int ch, float initialH)
      : Service::HumiditySensor() {
    deviceId = id;
    protocol = proto;
    sensorId = sid;
    channel = ch;
    float v = isnan(initialH) ? 50.0f : initialH;
    hum = new Characteristic::CurrentRelativeHumidity(v);
    new Characteristic::Name(name.c_str());
  }

  bool matches(const WeatherReading& r) const {
    if (!r.valid) return false;
    if (protocol != "auto" && protocol != r.protocol) return false;
    if (sensorId >= 0 && sensorId != r.sensorId) return false;
    if (channel > 0 && channel != r.channel) return false;
    return !isnan(r.humidity);
  }

  void update(float h) {
    if (!hum || isnan(h)) return;
    float cur = hum->getVal<float>();
    if (fabsf(cur - h) < 0.5f && hum->timeVal() < 30000) return;
    hum->setVal(h);
  }
};

std::vector<RfProgButton*> gProgButtons;
std::vector<RfStateSwitch*> gStateSwitches;
std::vector<RfMotionSensor*> gMotionSensors;
std::vector<RfTempSensor*> gTempSensors;
std::vector<RfHumiditySensor*> gHumiditySensors;

// Switch that TX primary (On) / secondary (Off). Each slot = Action or signal.
struct RfActionSwitch : Service::Switch {
  SpanCharacteristic* power = nullptr;
  String primaryActionId;
  String primarySignalId;
  String secondaryActionId;
  String secondarySignalId;
  String listenPrimary;
  String listenSecondary;
  String actionName;
  String mode;  // stateless | stateful | toggle
  ActionRunner* runner = nullptr;
  ActionStorage* store = nullptr;
  bool wantAutoOff = false;
  uint32_t lastFiredMs = 0;

  RfActionSwitch(const String& name, const String& hkMode, ActionRunner* r,
                 ActionStorage* s)
      : Service::Switch() {
    actionName = name;
    mode = hkMode;
    runner = r;
    store = s;
    power = new Characteristic::On(false);
    new Characteristic::Name(name.c_str());
  }

  void setPrimary(const String& actionId, const String& signalId) {
    primaryActionId = actionId;
    primarySignalId = signalId;
    if (signalId.length()) listenPrimary = signalId;
  }
  void setSecondary(const String& actionId, const String& signalId) {
    secondaryActionId = actionId;
    secondarySignalId = signalId;
    if (signalId.length()) listenSecondary = signalId;
  }

  bool startBind(const String& actionId, const String& signalId) {
    if (!runner) return false;
    if (actionId.isEmpty() && signalId.isEmpty()) return false;
    if (runner->busy()) {
      Serial.printf("[HomeKit] '%s' busy — ignored\n", actionName.c_str());
      return false;
    }
    bool ok = false;
    if (actionId.length()) {
      ok = store && runner->startById(actionId, *store);
    } else {
      ok = runner->startSignal(signalId);
    }
    Serial.printf("[HomeKit] '%s' start %s=%s → %s\n", actionName.c_str(),
                  actionId.length() ? "action" : "signal",
                  actionId.length() ? actionId.c_str() : signalId.c_str(),
                  ok ? "ok" : "fail");
    return ok;
  }

  boolean update() override {
    if (!power || !runner) return true;
    if (!power->updated()) return true;
    const bool on = power->getNewVal();

    if (mode == "stateful" || mode == "toggle") {
      if (on) startBind(primaryActionId, primarySignalId);
      else startBind(secondaryActionId, secondarySignalId);
      return true;
    }

    if (on) {
      if (startBind(primaryActionId, primarySignalId)) wantAutoOff = true;
    }
    return true;
  }

  bool fireFromRf(const String& matchedSignalId) {
    if (!power || matchedSignalId.isEmpty()) return false;
    if (millis() - lastFiredMs < 400) return false;
    bool next = true;
    if (listenSecondary.length() && matchedSignalId == listenSecondary) {
      next = false;
    } else if (listenPrimary.length() && matchedSignalId == listenPrimary) {
      next = (mode == "toggle") ? !power->getVal() : true;
    } else {
      return false;
    }
    power->setVal(next);
    lastFiredMs = millis();
    Serial.printf("[HomeKit] '%s' RF → %s\n", actionName.c_str(),
                  next ? "ON" : "OFF");
    return true;
  }

  void loop() override {
    if (!wantAutoOff || !power || !runner) return;
    if (runner->busy()) return;
    if (power->getVal()) {
      power->setVal(false);
      Serial.printf("[HomeKit] '%s' auto-off\n", actionName.c_str());
    }
    wantAutoOff = false;
  }
};

std::vector<RfActionSwitch*> gActionSwitches;

}  // namespace

HomeKitManager* HomeKitManager::instance() { return self_; }

int HomeKitManager::deviceCount() const {
  return (int)remotes_.list().size();
}

bool HomeKitManager::fireButtonForSignal(const String& signalId) {
  if (!enabled_ || signalId.isEmpty()) return false;
  bool any = false;
  for (auto* btn : gProgButtons) {
    if (!btn || btn->signalId != signalId) continue;
    if (millis() - btn->lastFiredMs < 400) continue;
    if (btn->fireSingle()) any = true;
  }
  for (auto* sw : gStateSwitches) {
    if (!sw || sw->signalId != signalId) continue;
    if (sw->fireFromRf()) any = true;
  }
  for (auto* sw : gActionSwitches) {
    if (!sw) continue;
    if (sw->fireFromRf(signalId)) any = true;
  }
  return any;
}

bool HomeKitManager::triggerSensorForSignal(const String& signalId) {
  if (!enabled_ || signalId.isEmpty()) return false;
  bool any = false;
  for (auto* s : gMotionSensors) {
    if (!s || s->signalId != signalId) continue;
    if (s->trigger()) any = true;
  }
  return any;
}

bool HomeKitManager::publishWeather(const WeatherReading& reading) {
  if (!reading.valid) return false;
  bool any = false;

  if (!enabled_) return false;

  for (auto* s : gTempSensors) {
    if (s && s->matches(reading)) {
      s->update(reading.tempC);
      any = true;
    }
  }
  for (auto* s : gHumiditySensors) {
    if (s && s->matches(reading)) {
      s->update(reading.humidity);
      any = true;
    }
  }
  if (any) {
    Serial.printf("[HomeKit] weather %s id=%d ch=%d T=%.1f H=%.0f\n",
                  reading.protocol.c_str(), reading.sensorId, reading.channel,
                  reading.tempC, reading.humidity);
  }
  return any;
}

bool HomeKitManager::begin(const String& wifiSsid, const String& wifiPass) {
  if (begun_) return enabled_;
  self_ = this;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HomeKit] skipped — Wi-Fi STA not connected");
    enabled_ = false;
    begun_ = true;
    return false;
  }

  if (wifiSsid.isEmpty()) {
    Serial.println("[HomeKit] skipped — no Wi-Fi credentials to hand to HomeSpan");
    enabled_ = false;
    begun_ = true;
    return false;
  }

  MDNS.end();
  delay(50);

  homeSpan.setLogLevel(1);
  homeSpan.setPortNum(hapPort_);
  homeSpan.setHostNameSuffix("");
  homeSpan.setPairingCode(setupCode_.c_str());
  homeSpan.setQRID(setupId_.c_str());
  homeSpan.setWifiCredentials(wifiSsid.c_str(), wifiPass.c_str());

  homeSpan.setWifiCallback([]() {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[Web] mDNS http://cc1101.local (with HomeKit HAP)");
  });

  homeSpan.begin(Category::Bridges, "CC1101 RF Hub", "cc1101");

  buildAccessories();
  homeSpan.autoPoll();

  enabled_ = true;
  begun_ = true;
  Serial.printf("[HomeKit] ready  HAP port=%u  hostname=cc1101.local\n", hapPort_);
  Serial.printf("[HomeKit] Setup code: %c%c%c-%c%c-%c%c%c   Setup ID: %s\n",
                setupCode_[0], setupCode_[1], setupCode_[2],
                setupCode_[3], setupCode_[4],
                setupCode_[5], setupCode_[6], setupCode_[7],
                setupId_.c_str());
  return true;
}

void HomeKitManager::buildAccessories() {
  gProgButtons.clear();
  gStateSwitches.clear();
  gMotionSensors.clear();
  gTempSensors.clear();
  gHumiditySensors.clear();
  gActionSwitches.clear();

  new SpanAccessory(1);
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("CC1101 RF Hub");
      new Characteristic::Manufacturer("esp32-rf433gw");
      new Characteristic::Model("ESP32-C3 + CC1101");
      new Characteristic::SerialNumber("RFGW-001");
      new Characteristic::FirmwareRevision("1.6.0");

  auto list = remotes_.list();
  Serial.printf("[HomeKit] loading %u devices\n", (unsigned)list.size());

  for (const auto& remote : list) {
    uint32_t aid = stableAid(remote.id);

    if (remote.isRemote()) {
      if (remote.buttons.empty()) continue;

      int nStateless = 0, nSwitch = 0;
      for (const auto& btn : remote.buttons) {
        const bool isSwitch =
            btn.homekitMode == "stateful" || btn.homekitMode == "toggle" ||
            btn.hasPrimaryAction() || btn.hasSecondary();
        if (isSwitch) nSwitch++;
        else nStateless++;
      }

      new SpanAccessory(aid);
        new Service::AccessoryInformation();
          new Characteristic::Identify();
          new Characteristic::Name(remote.name.c_str());
          new Characteristic::Manufacturer("esp32-rf433gw");
          new Characteristic::Model(nSwitch && !nStateless ? "RF Switch Remote"
                                                           : "RF Programmable Remote");
          new Characteristic::SerialNumber(remote.id.c_str());
          new Characteristic::FirmwareRevision("1.6.0");

        Service::ServiceLabel* label = nullptr;
        if (nStateless > 0) {
          label = new Service::ServiceLabel();
          new Characteristic::ServiceLabelNamespace(
              Characteristic::ServiceLabelNamespace::NUMERALS);
        }

        uint8_t idx = 1;
        for (const auto& btn : remote.buttons) {
          const bool wantsSwitch =
              btn.homekitMode == "stateful" || btn.homekitMode == "toggle" ||
              btn.hasPrimaryAction() || btn.hasSecondary();

          if (wantsSwitch) {
            String mode = btn.homekitMode;
            if (mode != "stateful" && mode != "toggle") mode = "stateless";
            auto* sw = new RfActionSwitch(btn.name, mode, &runner_, &actions_);
            sw->setPrimary(btn.actionId, btn.signalId);
            if (mode != "stateless") {
              sw->setSecondary(btn.secondaryActionId, btn.secondarySignalId);
            }
            gActionSwitches.push_back(sw);
            Serial.printf("[HomeKit]   switch '%s' mode=%s prim=%s/%s sec=%s/%s\n",
                          btn.name.c_str(), mode.c_str(),
                          btn.actionId.c_str(), btn.signalId.c_str(),
                          btn.secondaryActionId.c_str(),
                          btn.secondarySignalId.c_str());
          } else if (btn.hasPrimarySignal()) {
            // RF-only stateful/toggle without TX still uses state switch
            if (btn.homekitMode == "stateful" || btn.homekitMode == "toggle") {
              auto* sw =
                  new RfStateSwitch(btn.name, btn.signalId, btn.homekitMode);
              gStateSwitches.push_back(sw);
            } else {
              auto* prog = new RfProgButton(btn.name, btn.signalId, idx++);
              if (label) label->addLink(prog);
              gProgButtons.push_back(prog);
            }
          }
        }
      continue;
    }

    if (remote.isSensor()) {
      if (remote.buttons.empty()) continue;
      new SpanAccessory(aid);
        new Service::AccessoryInformation();
          new Characteristic::Identify();
          new Characteristic::Name(remote.name.c_str());
          new Characteristic::Manufacturer("esp32-rf433gw");
          new Characteristic::Model("RF Motion Sensor");
          new Characteristic::SerialNumber(remote.id.c_str());
          new Characteristic::FirmwareRevision("1.6.0");
        for (const auto& btn : remote.buttons) {
          auto* mot = new RfMotionSensor(
              btn.name.length() ? btn.name : remote.name, btn.signalId);
          gMotionSensors.push_back(mot);
        }
      continue;
    }

    if (remote.isTemperature()) {
      new SpanAccessory(aid);
        new Service::AccessoryInformation();
          new Characteristic::Identify();
          new Characteristic::Name(remote.name.c_str());
          new Characteristic::Manufacturer("esp32-rf433gw");
          new Characteristic::Model("RF Temperature Sensor");
          new Characteristic::SerialNumber(remote.id.c_str());
          new Characteristic::FirmwareRevision("1.6.0");
        auto* ts = new RfTempSensor(remote.name, remote.id, remote.weatherProtocol,
                                    remote.weatherId, remote.weatherChannel,
                                    remote.lastTempC);
        gTempSensors.push_back(ts);
      continue;
    }

    if (remote.isHumidity()) {
      new SpanAccessory(aid);
        new Service::AccessoryInformation();
          new Characteristic::Identify();
          new Characteristic::Name(remote.name.c_str());
          new Characteristic::Manufacturer("esp32-rf433gw");
          new Characteristic::Model("RF Humidity Sensor");
          new Characteristic::SerialNumber(remote.id.c_str());
          new Characteristic::FirmwareRevision("1.6.0");
        auto* hs = new RfHumiditySensor(remote.name, remote.id, remote.weatherProtocol,
                                        remote.weatherId, remote.weatherChannel,
                                        remote.lastHumidity);
        gHumiditySensors.push_back(hs);
      continue;
    }
  }

  // Actions are exposed only when bound to a Device button — not as standalone
  // HomeKit accessories.
}

void HomeKitManager::loop() {
  if (enabled_ && homeSpan.getAutoPollTask() == NULL) {
    homeSpan.poll();
  }
}

bool HomeKitManager::isPaired() const {
  return enabled_;
}

void HomeKitManager::requestRebuildAndReboot() {
  Serial.println("[HomeKit] devices changed — rebooting to refresh HomeKit accessories");
  delay(400);
  ESP.restart();
}
