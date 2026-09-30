#include "WebServer.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <algorithm>

bool AppWebServer::begin(bool captivePortal) {
  captive_ = captivePortal;
  setupRoutes();
  server_.begin();
  ws_.begin();
  ws_.onEvent([this](uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    if (type == WStype_TEXT && payload && length) {
      JsonDocument doc;
      if (deserializeJson(doc, payload, length)) return;
      const char* cmd = doc["cmd"] | "";
      if (!strcmp(cmd, "rx_start")) radio_.startRx();
      else if (!strcmp(cmd, "rx_stop")) radio_.stopRx();
    }
  });
  Serial.printf("[Web] HTTP :80  WS :81  (captive=%d)\n", captive_ ? 1 : 0);
  return true;
}

void AppWebServer::pump() {
  server_.handleClient();
  ws_.loop();
}

void AppWebServer::loop() {
  pump();
  uint32_t now = millis();
  // Slow status push while TX/action/OTA runs so we don't fight the radio.
  const uint32_t pushMs =
      (radio_.transmitter().isActive() || runner_.busy() || ota_.busy()) ? 400 : 250;
  if (now - lastWsPushMs_ >= pushMs) {
    lastWsPushMs_ = now;
    if (ws_.connectedClients() > 0) {
      broadcastStatus();
      broadcastOta();
      PressEvent ev;
      while (press_.takeNewerThan(lastPressSeqSent_, ev)) {
        lastPressSeqSent_ = ev.seq;
        broadcastPress(ev);
      }
    }
  }
}

void AppWebServer::sendJson(int code, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server_.send(code, "application/json", out);
}

void AppWebServer::sendError(int code, const char* msg) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = msg;
  sendJson(code, doc);
}

bool AppWebServer::readJsonBody(JsonDocument& doc) {
  if (!server_.hasArg("plain")) return false;
  return deserializeJson(doc, server_.arg("plain")) == DeserializationError::Ok;
}

String AppWebServer::contentType(const String& path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".svg")) return "image/svg+xml";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".ico")) return "image/x-icon";
  return "text/plain";
}

bool AppWebServer::handleStatic(String path) {
  if (path == "/" || path.isEmpty()) path = "/index.html";
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  server_.streamFile(f, contentType(path));
  f.close();
  return true;
}

void AppWebServer::broadcastStatus() {
  // Avoid heavy SPI while transmitting — RSSI is cached in that state.
  JsonDocument doc;
  doc["type"] = "status";
  doc["radioState"] = radioStateName(radio_.state());
  doc["rssi"] = radio_.currentRssi();
  doc["freq"] = radio_.radio().frequency();
  doc["detected"] = radio_.radio().isDetected();
  doc["uptime"] = millis() / 1000;
  doc["heap"] = ESP.getFreeHeap();
  doc["tx"] = radio_.transmitter().isActive();

  auto cap = radio_.capture().stats();
  doc["edgeCount"] = cap.edgeCount;
  doc["pulseCount"] = cap.pulseCount;
  doc["captureLengthUs"] = cap.captureLengthUs;
  doc["bufferUsed"] = cap.bufferUsed;
  doc["bufferCapacity"] = cap.bufferCapacity;
  doc["overflow"] = cap.overflow;
  doc["rxActive"] = (radio_.state() == RadioState::Rx || radio_.state() == RadioState::Recording);
  doc["pressListening"] = press_.listening();
  doc["pressEventSeq"] = press_.eventSeq();

  auto rs = radio_.recordStatus();
  doc["recordPhase"] = (uint8_t)rs.phase;
  doc["recordPhaseName"] = recordPhaseName(rs.phase);
  doc["recordMessage"] = rs.message;
  doc["pressesGot"] = rs.pressesGot;
  doc["pressesNeeded"] = rs.pressesNeeded;
  doc["liveRssiDbm"] = rs.liveRssiDbm;
  doc["triggerThresholdDbm"] = rs.triggerThresholdDbm;
  doc["noiseFloorDbm"] = rs.noiseFloorDbm;

  if (radio_.state() == RadioState::Scanning) {
    auto ss = radio_.scanner().status();
    doc["scanRunning"] = ss.running;
    doc["scanCurrent"] = ss.currentMHz;
    doc["scanPeakMHz"] = ss.peakMHz;
    doc["scanPeakRssi"] = ss.peakRssi;
    doc["signalDetected"] = ss.signalDetected;
    doc["detectedMHz"] = ss.detectedMHz;
    doc["detectedRssi"] = ss.detectedRssi;
  }

  if (radio_.state() == RadioState::Rx || radio_.state() == RadioState::Recording) {
    int32_t tmp[RfDefaults::LivePulseWindow];
    size_t n = radio_.capture().copyRecent(tmp, RfDefaults::LivePulseWindow);
    JsonArray arr = doc["pulses"].to<JsonArray>();
    for (size_t i = 0; i < n; i++) arr.add(tmp[i]);
  }

  String out;
  serializeJson(doc, out);
  ws_.broadcastTXT(out);
}

void AppWebServer::broadcastPress(const PressEvent& ev) {
  JsonDocument doc;
  doc["type"] = "press";
  doc["seq"] = ev.seq;
  doc["matched"] = ev.matched;
  doc["homekitFired"] = ev.homekitFired;
  doc["signalId"] = ev.signalId;
  doc["signalName"] = ev.signalName;
  doc["remoteId"] = ev.remoteId;
  doc["remoteName"] = ev.remoteName;
  doc["buttonName"] = ev.buttonName;
  doc["rssi"] = ev.rssiDbm;
  doc["pulseCount"] = ev.pulseCount;
  doc["ageMs"] = 0;
  String out;
  serializeJson(doc, out);
  ws_.broadcastTXT(out);
}

void AppWebServer::broadcastOta() {
  const char* phase = ota_.phaseName();
  const int prog = ota_.progressPct();
  if (lastOtaPhaseSent_ == phase && prog == lastOtaProgressSent_ && !ota_.busy()) {
    return;
  }
  lastOtaPhaseSent_ = phase;
  lastOtaProgressSent_ = prog;
  JsonDocument doc;
  ota_.toJson(doc);
  doc["type"] = "ota";
  String out;
  serializeJson(doc, out);
  ws_.broadcastTXT(out);
}

void AppWebServer::setupRoutes() {
  server_.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
  server_.on("/api/signals", HTTP_GET, [this]() { handleSignals(); });
  server_.on("/api/signal", HTTP_GET, [this]() { handleSignalGet(); });
  server_.on("/api/signal", HTTP_DELETE, [this]() { handleSignalDelete(); });
  server_.on("/api/signal/replay", HTTP_POST, [this]() { handleSignalReplay(); });
  server_.on("/api/signal/rename", HTTP_POST, [this]() { handleSignalRename(); });
  server_.on("/api/signal/import", HTTP_POST, [this]() { handleSignalImport(); });
  server_.on("/api/presses", HTTP_GET, [this]() { handlePresses(); });

  server_.on("/api/scan/start", HTTP_POST, [this]() { handleScanStart(); });
  server_.on("/api/scan/stop", HTTP_POST, [this]() { handleScanStop(); });
  server_.on("/api/scan/data", HTTP_GET, [this]() { handleScanData(); });

  server_.on("/api/rx/start", HTTP_POST, [this]() { handleRxStart(); });
  server_.on("/api/rx/stop", HTTP_POST, [this]() { handleRxStop(); });
  server_.on("/api/rx/live", HTTP_GET, [this]() { handleRxLive(); });

  server_.on("/api/record/start", HTTP_POST, [this]() { handleRecordStart(); });
  server_.on("/api/record/stop", HTTP_POST, [this]() { handleRecordStop(); });
  server_.on("/api/record/status", HTTP_GET, [this]() { handleRecordStatus(); });
  server_.on("/api/record/save", HTTP_POST, [this]() { handleRecordSave(); });
  server_.on("/api/record/discard", HTTP_POST, [this]() { handleRecordDiscard(); });

  server_.on("/api/tx", HTTP_POST, [this]() { handleTx(); });
  server_.on("/api/settings", HTTP_GET, [this]() { handleSettingsGet(); });
  server_.on("/api/settings", HTTP_POST, [this]() { handleSettingsPost(); });
  server_.on("/api/settings/rf/reset", HTTP_POST, [this]() { handleRfReset(); });

  server_.on("/api/wifi/scan", HTTP_GET, [this]() { handleWifiScan(); });
  server_.on("/api/wifi/save", HTTP_POST, [this]() { handleWifiSave(); });
  server_.on("/api/wifi/clear", HTTP_POST, [this]() { handleWifiClear(); });

  server_.on("/api/remotes", HTTP_GET, [this]() { handleRemotes(); });
  server_.on("/api/remotes", HTTP_POST, [this]() { handleRemoteSave(); });
  server_.on("/api/remote", HTTP_GET, [this]() { handleRemoteGet(); });
  server_.on("/api/remote", HTTP_DELETE, [this]() { handleRemoteDelete(); });
  server_.on("/api/weather", HTTP_GET, [this]() { handleWeather(); });

  server_.on("/api/actions", HTTP_GET, [this]() { handleActions(); });
  server_.on("/api/actions", HTTP_POST, [this]() { handleActionSave(); });
  server_.on("/api/action", HTTP_GET, [this]() { handleActionGet(); });
  server_.on("/api/action", HTTP_DELETE, [this]() { handleActionDelete(); });
  server_.on("/api/action/run", HTTP_POST, [this]() { handleActionRun(); });
  server_.on("/api/action/stop", HTTP_POST, [this]() { handleActionStop(); });
  server_.on("/api/action/status", HTTP_GET, [this]() { handleActionStatus(); });

  server_.on("/api/homekit/status", HTTP_GET, [this]() { handleHomeKitStatus(); });
  server_.on("/api/homekit/rebuild", HTTP_POST, [this]() { handleHomeKitReboot(); });

  server_.on("/api/ota", HTTP_GET, [this]() { handleOtaStatus(); });
  server_.on("/api/ota/channel", HTTP_POST, [this]() { handleOtaChannel(); });
  server_.on("/api/ota/check", HTTP_POST, [this]() { handleOtaCheck(); });
  server_.on("/api/ota/install", HTTP_POST, [this]() { handleOtaInstall(); });

  // REST-ish signal paths
  server_.onNotFound([this]() {
    String uri = server_.uri();
    if (uri.startsWith("/api/signals/")) {
      String rest = uri.substring(String("/api/signals/").length());
      int slash = rest.indexOf('/');
      String id = (slash < 0) ? rest : rest.substring(0, slash);
      String action = (slash < 0) ? "" : rest.substring(slash + 1);
      if (server_.method() == HTTP_GET && action.isEmpty()) {
        RFSignal sig;
        if (!signals_.load(id, sig)) { sendError(404, "not found"); return; }
        JsonDocument doc;
        doc["ok"] = true;
        JsonObject o = doc["signal"].to<JsonObject>();
        sig.toJson(o, true);
        sendJson(200, doc);
        return;
      }
      if (server_.method() == HTTP_DELETE && action.isEmpty()) {
        if (!signals_.remove(id)) { sendError(404, "not found"); return; }
        press_.reloadTemplates();
        JsonDocument doc; doc["ok"] = true; sendJson(200, doc); return;
      }
      if (server_.method() == HTTP_POST && action == "replay") {
        RFSignal sig;
        if (!signals_.load(id, sig)) { sendError(404, "not found"); return; }
        uint32_t repeats = server_.hasArg("repeats") ? server_.arg("repeats").toInt()
                                                     : sig.estimatedRepeats;
        if (repeats < 1) repeats = 1;
        if (repeats > 50) repeats = 50;
        TxRequest tx;
        tx.frequencyMHz = sig.frequencyMHz;
        tx.modulation = sig.modulation;
        tx.pulses = sig.pulses;
        tx.repeats = repeats;
        tx.gapBetweenRepeatsUs = sig.gapBetweenRepeatsUs;
        if (!radio_.startTx(tx)) { sendError(409, "radio busy or TX failed"); return; }
        JsonDocument doc; doc["ok"] = true; doc["repeats"] = repeats; sendJson(200, doc); return;
      }
    }

    if (captive_) {
      const char* captivePaths[] = {
          "/generate_204", "/hotspot-detect.html", "/connecttest.txt", "/ncsi.txt", "/fwlink"};
      for (const char* p : captivePaths) {
        if (uri == p) {
          server_.sendHeader("Location", "/", true);
          server_.send(302, "text/plain", "");
          return;
        }
      }
      if (handleStatic(uri)) return;
      server_.sendHeader("Location", "/", true);
      server_.send(302, "text/plain", "");
      return;
    }

    if (handleStatic(uri)) return;
    sendError(404, "not found");
  });
}

void AppWebServer::handleStatus() {
  JsonDocument doc;
  doc["ok"] = true;
  doc["wifiMode"] = (wifi_.mode() == WifiMode::AccessPoint) ? "AP"
                    : (wifi_.mode() == WifiMode::Station)    ? "STA"
                    : (wifi_.mode() == WifiMode::Connecting) ? "CONNECTING"
                                                             : "DISCONNECTED";
  doc["ip"] = wifi_.ipAddress();
  doc["wifiRssi"] = wifi_.rssi();
  doc["hostname"] = wifi_.hostname();
  doc["apSsid"] = wifi_.apSsid();
  doc["uptime"] = millis() / 1000;
  doc["heap"] = ESP.getFreeHeap();
  doc["radioState"] = radioStateName(radio_.state());
  auto& id = radio_.radio().identity();
  doc["cc1101"]["detected"] = id.detected;
  doc["cc1101"]["partnum"] = id.partnum;
  doc["cc1101"]["version"] = id.version;
  if (!id.detected) doc["cc1101"]["troubleshooting"] = radio_.troubleshooting();
  const auto& s = radio_.radio().settings();
  doc["rf"]["frequency"] = s.frequencyMHz;
  doc["rf"]["modulation"] = s.modulation;
  doc["rf"]["modulationName"] = CC1101Radio::modulationName(s.modulation);
  doc["rf"]["rxBandwidthKHz"] = s.rxBandwidthKHz;
  doc["rf"]["dataRateBaud"] = s.dataRateBaud;
  doc["rf"]["deviationKHz"] = s.deviationKHz;
  doc["rf"]["rssiThreshold"] = s.rssiThresholdDbm;
  doc["rf"]["rssi"] = radio_.currentRssi();
  doc["homekit"]["enabled"] = homekit_.isEnabled();
  doc["homekit"]["port"] = homekit_.hapPort();
  doc["fwVersion"] = ota_.currentVersion();
  doc["otaChannel"] = ota_.channelName();
  doc["otaPhase"] = ota_.phaseName();
  sendJson(200, doc);
}

void AppWebServer::handleSignals() {
  auto list = signals_.list(false);
  JsonDocument doc;
  doc["ok"] = true;
  JsonArray arr = doc["signals"].to<JsonArray>();
  for (const auto& s : list) {
    JsonObject o = arr.add<JsonObject>();
    s.toJson(o, false);
  }
  sendJson(200, doc);
}

void AppWebServer::handleSignalGet() {
  if (!server_.hasArg("id")) { sendError(400, "id required"); return; }
  RFSignal sig;
  if (!signals_.load(server_.arg("id"), sig)) { sendError(404, "not found"); return; }
  JsonDocument doc;
  doc["ok"] = true;
  JsonObject o = doc["signal"].to<JsonObject>();
  sig.toJson(o, true);
  sendJson(200, doc);
}

void AppWebServer::handleSignalDelete() {
  if (!server_.hasArg("id")) { sendError(400, "id required"); return; }
  if (!signals_.remove(server_.arg("id"))) { sendError(404, "not found"); return; }
  press_.reloadTemplates();
  JsonDocument doc; doc["ok"] = true; sendJson(200, doc);
}

void AppWebServer::handleSignalReplay() {
  if (!server_.hasArg("id")) { sendError(400, "id required"); return; }
  RFSignal sig;
  if (!signals_.load(server_.arg("id"), sig)) { sendError(404, "not found"); return; }
  uint32_t repeats = server_.hasArg("repeats") ? server_.arg("repeats").toInt()
                                               : sig.estimatedRepeats;
  if (repeats < 1) repeats = 1;
  if (repeats > 50) repeats = 50;
  TxRequest tx;
  tx.frequencyMHz = sig.frequencyMHz;
  tx.modulation = sig.modulation;
  tx.pulses = sig.pulses;
  tx.repeats = repeats;
  tx.gapBetweenRepeatsUs = sig.gapBetweenRepeatsUs;
  if (!radio_.startTx(tx)) { sendError(409, "radio busy or TX failed"); return; }
  JsonDocument doc; doc["ok"] = true; doc["repeats"] = repeats; sendJson(200, doc);
}

void AppWebServer::handleSignalRename() {
  JsonDocument body;
  if (!readJsonBody(body)) { sendError(400, "invalid json"); return; }
  String id = body["id"] | "";
  String name = body["name"] | "";
  if (id.isEmpty() || name.isEmpty()) { sendError(400, "id and name required"); return; }
  if (!signals_.rename(id, name)) { sendError(404, "not found"); return; }
  press_.reloadTemplates();
  JsonDocument doc; doc["ok"] = true; sendJson(200, doc);
}

void AppWebServer::handleScanStart() {
  JsonDocument body;
  if (server_.hasArg("plain")) readJsonBody(body);
  ScanConfig cfg;
  cfg.startMHz = body["start"] | RfDefaults::ScanStartMHz;
  cfg.endMHz = body["end"] | RfDefaults::ScanEndMHz;
  cfg.stepKHz = body["stepKHz"] | RfDefaults::ScanStepKHz;
  cfg.dwellMs = body["dwellMs"] | RfDefaults::ScanDwellMs;
  cfg.rssiThresholdDbm = body["rssiThreshold"] | RfDefaults::RssiThresholdDbm;
  cfg.findSignal = body["findSignal"] | false;
  if (!radio_.startScan(cfg)) { sendError(409, "cannot start scan"); return; }
  JsonDocument doc; doc["ok"] = true; sendJson(200, doc);
}

void AppWebServer::handleScanStop() {
  radio_.stopScan();
  JsonDocument doc; doc["ok"] = true; sendJson(200, doc);
}

void AppWebServer::handleScanData() {
  JsonDocument doc;
  doc["ok"] = true;
  auto st = radio_.scanner().status();
  doc["running"] = st.running;
  doc["findSignal"] = st.findSignal;
  doc["signalDetected"] = st.signalDetected;
  doc["currentMHz"] = st.currentMHz;
  doc["peakMHz"] = st.peakMHz;
  doc["peakRssi"] = st.peakRssi;
  doc["detectedMHz"] = st.detectedMHz;
  doc["detectedRssi"] = st.detectedRssi;
  doc["passCount"] = st.passCount;
  JsonArray arr = doc["points"].to<JsonArray>();
  for (const auto& p : radio_.scanner().points()) {
    JsonObject o = arr.add<JsonObject>();
    o["f"] = p.frequencyMHz;
    o["r"] = p.rssiDbm;
  }
  sendJson(200, doc);
}

void AppWebServer::handleRxStart() {
  if (!radio_.startRx()) { sendError(409, "cannot start RX"); return; }
  JsonDocument doc; doc["ok"] = true; sendJson(200, doc);
}
void AppWebServer::handleRxStop() {
  radio_.stopRx();
  JsonDocument doc; doc["ok"] = true; sendJson(200, doc);
}
void AppWebServer::handleRxLive() {
  JsonDocument doc;
  doc["ok"] = true;
  auto cap = radio_.capture().stats();
  doc["stats"]["edgeCount"] = cap.edgeCount;
  doc["stats"]["pulseCount"] = cap.pulseCount;
  doc["stats"]["captureLengthUs"] = cap.captureLengthUs;
  doc["stats"]["bufferUsed"] = cap.bufferUsed;
  doc["stats"]["bufferCapacity"] = cap.bufferCapacity;
  doc["stats"]["overflow"] = cap.overflow;
  doc["stats"]["active"] = cap.active;
  doc["frequency"] = radio_.radio().frequency();
  doc["rssi"] = radio_.currentRssi();
  int32_t tmp[1024];
  size_t n = radio_.capture().copyPulses(tmp, 1024);
  JsonArray arr = doc["pulses"].to<JsonArray>();
  for (size_t i = 0; i < n; i++) arr.add(tmp[i]);
  sendJson(200, doc);
}

void AppWebServer::handleRecordStart() {
  if (!radio_.startRecord()) { sendError(409, "cannot start recording"); return; }
  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Stay quiet… measuring background noise";
  sendJson(200, doc);
}
void AppWebServer::handleRecordStop() {
  radio_.stopRecord(false);
  JsonDocument doc; doc["ok"] = true; sendJson(200, doc);
}
void AppWebServer::handleRecordDiscard() {
  radio_.stopRecord(true);
  JsonDocument doc; doc["ok"] = true; sendJson(200, doc);
}
void AppWebServer::handleRecordStatus() {
  auto rs = radio_.recordStatus();
  JsonDocument doc;
  doc["ok"] = true;
  doc["phase"] = (uint8_t)rs.phase;
  doc["phaseName"] = recordPhaseName(rs.phase);
  doc["message"] = rs.message;
  doc["complete"] = rs.complete;
  doc["pressesGot"] = rs.pressesGot;
  doc["pressesNeeded"] = rs.pressesNeeded;
  doc["noiseFloorDbm"] = rs.noiseFloorDbm;
  doc["liveRssiDbm"] = rs.liveRssiDbm;
  doc["triggerThresholdDbm"] = rs.triggerThresholdDbm;
  doc["peakRssiDbm"] = rs.peakRssiDbm;
  if (rs.complete) {
    JsonObject o = doc["signal"].to<JsonObject>();
    // Metadata + short waveform preview only — full pulses stay in RAM until save
    rs.signal.toJson(o, false);
    o["bitCodeCount"] = (int)rs.signal.bitCodes.size();
    JsonArray preview = o["pulses"].to<JsonArray>();
    const size_t n = std::min<size_t>(rs.signal.pulses.size(), 256);
    for (size_t i = 0; i < n; i++) preview.add(rs.signal.pulses[i]);
    o["pulsesPreview"] = true;
  }
  sendJson(200, doc);
}
void AppWebServer::handleRecordSave() {
  JsonDocument body;
  if (!readJsonBody(body)) { sendError(400, "invalid json"); return; }
  RFSignal sig = radio_.lastRecorded();
  if (sig.pulses.empty()) { sendError(400, "no recorded signal"); return; }
  sig.name = body["name"] | "Unnamed";
  if (!signals_.save(sig)) { sendError(500, "save failed"); return; }
  press_.reloadTemplates();
  // Release complete-session lock so the next pairing starts clean
  radio_.stopRecord(true);  // discard session state; signal already saved
  JsonDocument doc; doc["ok"] = true; doc["id"] = sig.id; sendJson(200, doc);
}

void AppWebServer::handleSignalImport() {
  JsonDocument body;
  if (!readJsonBody(body)) { sendError(400, "invalid json"); return; }
  RFSignal sig;
  if (!sig.fromJson(body.as<JsonVariantConst>())) {
    // Also accept { signal: {...} }
    if (body["signal"].is<JsonObject>()) {
      if (!sig.fromJson(body["signal"].as<JsonVariantConst>())) {
        sendError(400, "invalid signal");
        return;
      }
    } else {
      sendError(400, "invalid signal");
      return;
    }
  }
  if (sig.pulses.empty()) { sendError(400, "pulses required"); return; }
  if (!signals_.save(sig)) { sendError(500, "save failed"); return; }
  press_.reloadTemplates();
  JsonDocument doc;
  doc["ok"] = true;
  doc["id"] = sig.id;
  sendJson(200, doc);
}

void AppWebServer::handlePresses() {
  JsonDocument doc;
  press_.toJson(doc);
  sendJson(200, doc);
}

void AppWebServer::handleTx() {
  JsonDocument body;
  if (!readJsonBody(body)) { sendError(400, "invalid json"); return; }
  TxRequest tx;
  tx.frequencyMHz = body["frequency"] | RfDefaults::FrequencyMHz;
  tx.modulation = body["modulation"] | RfDefaults::ModulationOOK;
  tx.repeats = body["repeats"] | 1;
  tx.gapBetweenRepeatsUs = body["gapUs"] | 10000;
  if (tx.repeats < 1) tx.repeats = 1;
  if (tx.repeats > 50) tx.repeats = 50;

  if (body["pulses"].is<JsonArray>()) {
    for (JsonVariant p : body["pulses"].as<JsonArray>()) tx.pulses.push_back(p.as<int32_t>());
  } else if (!body["pulsesCsv"].isNull()) {
    RFSignal tmp;
    if (!tmp.parsePulsesCsv(body["pulsesCsv"] | "")) { sendError(400, "invalid pulsesCsv"); return; }
    tx.pulses = tmp.pulses;
  } else if (!body["id"].isNull()) {
    RFSignal sig;
    if (!signals_.load(body["id"] | "", sig)) { sendError(404, "signal not found"); return; }
    tx.pulses = sig.pulses;
    tx.frequencyMHz = sig.frequencyMHz;
    tx.modulation = sig.modulation;
    if (body["repeats"].isNull()) tx.repeats = sig.estimatedRepeats;
    tx.gapBetweenRepeatsUs = sig.gapBetweenRepeatsUs;
  }
  if (tx.pulses.empty()) { sendError(400, "no pulses"); return; }
  if (!radio_.startTx(tx)) { sendError(409, "radio busy or TX failed"); return; }
  JsonDocument doc; doc["ok"] = true; sendJson(200, doc);
}

void AppWebServer::handleSettingsGet() {
  JsonDocument doc;
  doc["ok"] = true;
  const auto& s = radio_.radio().settings();
  doc["frequency"] = s.frequencyMHz;
  doc["modulation"] = s.modulation;
  doc["rxBandwidthKHz"] = s.rxBandwidthKHz;
  doc["dataRateBaud"] = s.dataRateBaud;
  doc["deviationKHz"] = s.deviationKHz;
  doc["rssiThreshold"] = s.rssiThresholdDbm;
  sendJson(200, doc);
}
void AppWebServer::handleSettingsPost() {
  JsonDocument body;
  if (!readJsonBody(body)) { sendError(400, "invalid json"); return; }
  RFSettings s = radio_.radio().settings();
  if (!body["frequency"].isNull()) s.frequencyMHz = body["frequency"] | s.frequencyMHz;
  if (!body["modulation"].isNull()) s.modulation = body["modulation"] | s.modulation;
  if (!body["rxBandwidthKHz"].isNull()) s.rxBandwidthKHz = body["rxBandwidthKHz"] | s.rxBandwidthKHz;
  if (!body["dataRateBaud"].isNull()) s.dataRateBaud = body["dataRateBaud"] | s.dataRateBaud;
  if (!body["deviationKHz"].isNull()) s.deviationKHz = body["deviationKHz"] | s.deviationKHz;
  if (!body["rssiThreshold"].isNull()) s.rssiThresholdDbm = body["rssiThreshold"] | s.rssiThresholdDbm;
  if (!radio_.applyRfSettings(s)) { sendError(409, "cannot apply"); return; }
  JsonDocument doc; doc["ok"] = true; sendJson(200, doc);
}
void AppWebServer::handleRfReset() {
  if (runner_.busy()) runner_.stop();
  radio_.stopTx();
  radio_.releaseToIdle();
  radio_.resetRfSettings();
  JsonDocument doc; doc["ok"] = true; doc["radioState"] = radioStateName(radio_.state());
  sendJson(200, doc);
}

void AppWebServer::handleWifiScan() {
  auto nets = wifi_.scanNetworks();
  JsonDocument doc;
  doc["ok"] = true;
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (const auto& n : nets) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = n.ssid;
    o["rssi"] = n.rssi;
    o["secure"] = n.encryption != WIFI_AUTH_OPEN;
  }
  sendJson(200, doc);
}
void AppWebServer::handleWifiSave() {
  JsonDocument body;
  if (!readJsonBody(body)) { sendError(400, "invalid json"); return; }
  String ssid = body["ssid"] | "";
  String pass = body["password"] | "";
  if (ssid.isEmpty()) { sendError(400, "ssid required"); return; }
  if (!wifi_.saveCredentials(ssid, pass)) { sendError(500, "save failed"); return; }
  JsonDocument doc; doc["ok"] = true; doc["reboot"] = true; sendJson(200, doc);
  delay(500);
  ESP.restart();
}
void AppWebServer::handleWifiClear() {
  wifi_.clearCredentials();
  JsonDocument doc; doc["ok"] = true; doc["reboot"] = true; sendJson(200, doc);
  delay(500);
  ESP.restart();
}

void AppWebServer::handleRemotes() {
  auto list = remotes_.list();
  JsonDocument doc;
  doc["ok"] = true;
  JsonArray arr = doc["remotes"].to<JsonArray>();
  int missing = 0;
  for (const auto& r : list) {
    JsonObject o = arr.add<JsonObject>();
    r.toJson(o);
    JsonArray warnings = o["warnings"].to<JsonArray>();
    for (const auto& b : r.buttons) {
      auto warnMissingAction = [&](const String& id, const char* slot) {
        if (id.isEmpty()) return;
        if (!actions_.exists(id)) {
          missing++;
          warnings.add(String("Button \"") + b.name + "\" " + slot +
                       " action missing");
        }
      };
      auto warnMissingSignal = [&](const String& id, const char* slot) {
        if (id.isEmpty()) return;
        if (!signals_.exists(id)) {
          missing++;
          warnings.add(String("Button \"") + b.name + "\" " + slot +
                       " signal missing — re-pair it");
        }
      };
      warnMissingAction(b.actionId, "primary");
      warnMissingAction(b.secondaryActionId, "secondary");
      warnMissingSignal(b.signalId, "primary");
      warnMissingSignal(b.secondarySignalId, "secondary");
    }
  }
  doc["missingSignals"] = missing;
  sendJson(200, doc);
}

void AppWebServer::handleRemoteGet() {
  if (!server_.hasArg("id")) { sendError(400, "id required"); return; }
  RfRemote remote;
  if (!remotes_.load(server_.arg("id"), remote)) { sendError(404, "not found"); return; }
  JsonDocument doc;
  doc["ok"] = true;
  JsonObject o = doc["remote"].to<JsonObject>();
  remote.toJson(o);
  sendJson(200, doc);
}

void AppWebServer::handleRemoteSave() {
  JsonDocument body;
  if (!readJsonBody(body)) { sendError(400, "invalid json"); return; }
  RfRemote remote;
  remote.id = body["id"] | "";
  remote.name = body["name"] | "Device";
  remote.type = body["type"] | "remote";
  if (remote.type.isEmpty()) remote.type = "remote";
  remote.buttons.clear();
  if (body["buttons"].is<JsonArray>()) {
    for (JsonVariant bv : body["buttons"].as<JsonArray>()) {
      RemoteButton b;
      b.name = bv["name"] | "Button";
      b.signalId = bv["signalId"] | "";
      b.actionId = bv["actionId"] | "";
      b.secondarySignalId = bv["secondarySignalId"] | "";
      b.secondaryActionId = bv["secondaryActionId"] | "";
      // Legacy aliases
      if (!b.actionId.length()) b.actionId = bv["actionIdOn"] | "";
      if (!b.secondaryActionId.length()) b.secondaryActionId = bv["actionIdOff"] | "";
      b.homekitMode = bv["homekitMode"] | "stateless";
      if (b.homekitMode != "stateful" && b.homekitMode != "toggle") {
        b.homekitMode = "stateless";
      }

      auto requireAction = [&](const String& id) -> bool {
        if (id.isEmpty()) return true;
        if (!actions_.exists(id)) {
          sendError(400, "unknown action");
          return false;
        }
        return true;
      };
      auto requireSignal = [&](const String& id) -> bool {
        if (id.isEmpty()) return true;
        if (!signals_.exists(id)) {
          sendError(400, "unknown signal");
          return false;
        }
        return true;
      };

      // Prefer action over signal per slot
      if (b.actionId.length()) b.signalId = "";
      if (b.secondaryActionId.length()) b.secondarySignalId = "";
      if (b.homekitMode == "stateless") {
        b.secondarySignalId = "";
        b.secondaryActionId = "";
      }

      if (!requireAction(b.actionId) || !requireAction(b.secondaryActionId) ||
          !requireSignal(b.signalId) || !requireSignal(b.secondarySignalId)) {
        return;
      }
      if (b.hasBinding()) remote.buttons.push_back(b);
    }
  }
  remote.weatherProtocol = body["weatherProtocol"] | "auto";
  remote.weatherId = body["weatherId"] | -1;
  remote.weatherChannel = body["weatherChannel"] | 0;
  if (!body["lastTempC"].isNull()) remote.lastTempC = body["lastTempC"].as<float>();
  if (!body["lastHumidity"].isNull()) remote.lastHumidity = body["lastHumidity"].as<float>();

  if (remote.isWeather()) {
    if (remote.weatherId < 0) { sendError(400, "pick a weather station first"); return; }
  } else {
    if (remote.buttons.empty()) { sendError(400, "add at least one button"); return; }
  }

  if (!remotes_.save(remote)) { sendError(500, "save failed"); return; }
  press_.reloadTemplates();
  bool doReboot = body["reboot"] | true;
  JsonDocument doc;
  doc["ok"] = true;
  doc["id"] = remote.id;
  doc["reboot"] = doReboot;
  doc["message"] = doReboot ? "Device saved. Rebooting so HomeKit refreshes…"
                            : "Device saved.";
  sendJson(200, doc);
  if (doReboot) {
    delay(700);
    homekit_.requestRebuildAndReboot();
  }
}

void AppWebServer::handleWeather() {
  JsonDocument doc;
  press_.weatherToJson(doc);
  sendJson(200, doc);
}

void AppWebServer::handleRemoteDelete() {
  if (!server_.hasArg("id")) { sendError(400, "id required"); return; }
  if (!remotes_.remove(server_.arg("id"))) { sendError(404, "not found"); return; }
  JsonDocument doc;
  doc["ok"] = true;
  doc["reboot"] = true;
  sendJson(200, doc);
  delay(700);
  homekit_.requestRebuildAndReboot();
}

void AppWebServer::handleActions() {
  JsonDocument doc;
  doc["ok"] = true;
  JsonArray arr = doc["actions"].to<JsonArray>();
  for (const auto& a : actions_.list()) {
    JsonObject o = arr.add<JsonObject>();
    a.toJson(o);
  }
  JsonObject st = doc["runner"].to<JsonObject>();
  runner_.toJson(st);
  sendJson(200, doc);
}

void AppWebServer::handleActionGet() {
  if (!server_.hasArg("id")) { sendError(400, "id required"); return; }
  RfAction a;
  if (!actions_.load(server_.arg("id"), a)) { sendError(404, "not found"); return; }
  JsonDocument doc;
  doc["ok"] = true;
  JsonObject o = doc["action"].to<JsonObject>();
  a.toJson(o);
  sendJson(200, doc);
}

void AppWebServer::handleActionSave() {
  JsonDocument body;
  if (!readJsonBody(body)) { sendError(400, "invalid json"); return; }
  RfAction a;
  a.id = body["id"] | "";
  a.name = body["name"] | "Action";
  a.steps.clear();
  if (body["steps"].is<JsonArray>()) {
    for (JsonVariant sv : body["steps"].as<JsonArray>()) {
      ActionStep s;
      if (s.fromJson(sv)) a.steps.push_back(s);
    }
  }
  if (a.steps.empty()) { sendError(400, "add at least one step"); return; }
  // Validate steps lightly
  for (const auto& s : a.steps) {
    if (s.type == "tx" || s.type == "hold") {
      if (s.signalId.isEmpty()) { sendError(400, "signal required for tx/hold"); return; }
      if (!signals_.exists(s.signalId)) { sendError(400, "unknown signal"); return; }
    }
  }
  if (!actions_.save(a)) { sendError(500, "save failed"); return; }
  JsonDocument doc;
  doc["ok"] = true;
  doc["id"] = a.id;
  doc["reboot"] = false;
  doc["message"] = "Action saved. Bind it to a Device button to expose in HomeKit.";
  sendJson(200, doc);
}

void AppWebServer::handleActionDelete() {
  if (!server_.hasArg("id")) { sendError(400, "id required"); return; }
  if (!actions_.remove(server_.arg("id"))) { sendError(404, "not found"); return; }
  JsonDocument doc;
  doc["ok"] = true;
  doc["reboot"] = false;
  sendJson(200, doc);
}

void AppWebServer::handleActionRun() {
  JsonDocument body;
  String id;
  if (readJsonBody(body)) id = body["id"] | "";
  if (id.isEmpty() && server_.hasArg("id")) id = server_.arg("id");
  if (id.isEmpty()) { sendError(400, "id required"); return; }
  if (runner_.busy()) { sendError(409, "action already running"); return; }
  if (!runner_.startById(id, actions_)) { sendError(404, "not found or empty"); return; }
  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "started";
  JsonObject st = doc["runner"].to<JsonObject>();
  runner_.toJson(st);
  sendJson(200, doc);
}

void AppWebServer::handleActionStop() {
  runner_.stop();
  JsonDocument doc;
  doc["ok"] = true;
  JsonObject st = doc["runner"].to<JsonObject>();
  runner_.toJson(st);
  sendJson(200, doc);
}

void AppWebServer::handleActionStatus() {
  JsonDocument doc;
  doc["ok"] = true;
  JsonObject st = doc["runner"].to<JsonObject>();
  runner_.toJson(st);
  sendJson(200, doc);
}

void AppWebServer::handleHomeKitStatus() {
  JsonDocument doc;
  doc["ok"] = true;
  doc["enabled"] = homekit_.isEnabled();
  doc["port"] = homekit_.hapPort();
  doc["setupCode"] = "231-14-081";
  doc["setupId"] = homekit_.setupId();
  doc["remoteCount"] = homekit_.deviceCount();
  doc["deviceCount"] = homekit_.deviceCount();
  sendJson(200, doc);
}

void AppWebServer::handleHomeKitReboot() {
  JsonDocument doc;
  doc["ok"] = true;
  doc["reboot"] = true;
  sendJson(200, doc);
  delay(500);
  homekit_.requestRebuildAndReboot();
}

void AppWebServer::handleOtaStatus() {
  JsonDocument doc;
  ota_.toJson(doc);
  sendJson(200, doc);
}

void AppWebServer::handleOtaChannel() {
  if (ota_.busy()) {
    sendError(409, "update in progress");
    return;
  }
  JsonDocument body;
  if (!readJsonBody(body)) {
    sendError(400, "invalid json");
    return;
  }
  const char* ch = body["channel"] | "stable";
  if (!ota_.setChannel(OtaManager::channelFromName(ch))) {
    sendError(409, "cannot change channel now");
    return;
  }
  JsonDocument doc;
  ota_.toJson(doc);
  sendJson(200, doc);
}

void AppWebServer::handleOtaCheck() {
  if (ota_.busy()) {
    sendError(409, "update in progress");
    return;
  }
  // Stop RF listen so TLS has heap/CPU
  radio_.yieldHkListen();
  ota_.checkForUpdate();
  JsonDocument doc;
  ota_.toJson(doc);
  sendJson(200, doc);
}

void AppWebServer::handleOtaInstall() {
  if (strcmp(ota_.phaseName(), "ready") != 0) {
    sendError(409, "no update ready — check first");
    return;
  }
  radio_.yieldHkListen();
  if (runner_.busy()) runner_.stop();
  if (!ota_.startInstall()) {
    sendError(500, ota_.message().c_str());
    return;
  }
  JsonDocument doc;
  ota_.toJson(doc);
  sendJson(200, doc);
}
