#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include "../radio/RadioManager.h"
#include "../network/WiFiManager.h"
#include "../storage/SignalStorage.h"
#include "../storage/RemoteStorage.h"
#include "../storage/ActionStorage.h"
#include "../action/ActionRunner.h"
#include "../homekit/HomeKitManager.h"
#include "../press/PressDetector.h"

class AppWebServer {
 public:
  AppWebServer(RadioManager& radio, WiFiManager& wifi, SignalStorage& signals,
               RemoteStorage& remotes, ActionStorage& actions, ActionRunner& runner,
               HomeKitManager& homekit, PressDetector& press)
      : radio_(radio),
        wifi_(wifi),
        signals_(signals),
        remotes_(remotes),
        actions_(actions),
        runner_(runner),
        homekit_(homekit),
        press_(press),
        server_(80),
        ws_(81) {}

  bool begin(bool captivePortal);
  void loop();
  // Lightweight: accept HTTP/WS only (no status broadcast). Safe to call often during TX.
  void pump();

 private:
  void setupRoutes();
  void broadcastStatus();
  void broadcastPress(const PressEvent& ev);
  bool handleStatic(String path);
  String contentType(const String& path);
  bool readJsonBody(JsonDocument& doc);
  void sendJson(int code, JsonDocument& doc);
  void sendError(int code, const char* msg);

  void handleStatus();
  void handleSignals();
  void handleSignalGet();
  void handleSignalDelete();
  void handleSignalReplay();
  void handleSignalRename();
  void handleSignalImport();
  void handleScanStart();
  void handleScanStop();
  void handleScanData();
  void handleRxStart();
  void handleRxStop();
  void handleRxLive();
  void handleRecordStart();
  void handleRecordStop();
  void handleRecordStatus();
  void handleRecordSave();
  void handleRecordDiscard();
  void handleTx();
  void handleSettingsGet();
  void handleSettingsPost();
  void handleRfReset();
  void handleWifiScan();
  void handleWifiSave();
  void handleWifiClear();
  void handleRemotes();
  void handleRemoteGet();
  void handleRemoteSave();
  void handleRemoteDelete();
  void handleWeather();
  void handleActions();
  void handleActionGet();
  void handleActionSave();
  void handleActionDelete();
  void handleActionRun();
  void handleActionStop();
  void handleActionStatus();
  void handleHomeKitStatus();
  void handleHomeKitReboot();
  void handlePresses();

  RadioManager& radio_;
  WiFiManager& wifi_;
  SignalStorage& signals_;
  RemoteStorage& remotes_;
  ActionStorage& actions_;
  ActionRunner& runner_;
  HomeKitManager& homekit_;
  PressDetector& press_;
  WebServer server_;
  WebSocketsServer ws_;
  bool captive_ = false;
  uint32_t lastWsPushMs_ = 0;
  uint32_t lastPressSeqSent_ = 0;
};
