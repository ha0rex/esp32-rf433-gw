#include <Arduino.h>
#include <LittleFS.h>
#include "Config.h"
#include "radio/RadioManager.h"
#include "network/WiFiManager.h"
#include "storage/SignalStorage.h"
#include "storage/RemoteStorage.h"
#include "storage/ActionStorage.h"
#include "action/ActionRunner.h"
#include "homekit/HomeKitManager.h"
#include "press/PressDetector.h"
#include "web/WebServer.h"

RadioManager gRadio;
WiFiManager gWifi;
SignalStorage gSignals;
RemoteStorage gRemotes;
ActionStorage gActions;
ActionRunner gRunner(gRadio, gSignals);
HomeKitManager gHomeKit(gRadio, gSignals, gRemotes, gActions, gRunner);
PressDetector gPress(gRadio, gSignals, gRemotes);
AppWebServer gWeb(gRadio, gWifi, gSignals, gRemotes, gActions, gRunner, gHomeKit,
                  gPress);

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("========================================");
  Serial.println(" ESP32-C3 CC1101 RF Gateway");
  Serial.println("========================================");
  Serial.printf("Chip: %s rev%d\n", ESP.getChipModel(), ESP.getChipRevision());
  Serial.printf("CPU: %u MHz  Free heap: %u\n", ESP.getCpuFreqMHz(), ESP.getFreeHeap());

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] FATAL: LittleFS mount failed");
  } else {
    Serial.println("[FS] mounted");
  }

  Serial.println("[Radio] Initializing CC1101...");
  bool rfOk = gRadio.begin();
  if (rfOk) {
    auto& id = gRadio.radio().identity();
    Serial.printf("[Radio] CC1101 detected PARTNUM=0x%02X VERSION=0x%02X\n",
                  id.partnum, id.version);
    gRadio.radio().startAsyncRx();
    delay(5);
    Serial.printf("[Radio] RSSI sample: %d dBm\n", gRadio.radio().readRssiDbm());
    gRadio.radio().idle();
  } else {
    Serial.println("[Radio] CC1101 NOT DETECTED — UI will still start");
    Serial.println(gRadio.troubleshooting());
  }

  gSignals.begin();
  gRemotes.begin();
  gActions.begin();
  gPress.setHomeKit(&gHomeKit);
  gPress.setActionRunner(&gRunner);
  gPress.begin();

  Serial.println("[WiFi] Starting...");
  gWifi.begin();
  bool captive = (gWifi.mode() == WifiMode::AccessPoint);
  Serial.printf("[WiFi] mode=%s IP=%s\n",
                captive ? "AP" : "STA",
                gWifi.ipAddress().c_str());

  gWeb.begin(captive);

  if (!captive && gWifi.isConnected()) {
    gWifi.stopMdns();
    gHomeKit.begin(gWifi.ssid(), gWifi.password());
  } else {
    Serial.println("[HomeKit] waiting for Wi-Fi STA (skip on SoftAP)");
  }

  Serial.println("[Boot] Ready.");
}

void loop() {
  const bool radioBusy =
      gRadio.transmitter().isActive() || gRunner.busy();

  gWifi.loop();
  gWeb.pump();
  gRadio.loop();
  gRunner.loop();
  gWeb.pump();

  if (radioBusy) {
    // HomeSpan/press are expensive and starve HTTP during TX holds.
    // Frames stay atomic; we only skip non-essential work between them.
    gWeb.pump();
    return;
  }

  gWeb.loop();  // includes periodic status broadcast
  gHomeKit.loop();
  gPress.loop();
  gWeb.pump();  // press/match can be heavy — always service HTTP after
  delay(1);
}
