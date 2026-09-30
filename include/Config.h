#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Hardware pin map — ESP32-C3 Super Mini + CC1101
// ---------------------------------------------------------------------------
namespace Pins {
constexpr int CC1101_GDO0 = 5;
constexpr int CC1101_CSN  = 1;
constexpr int CC1101_SCK  = 4;
constexpr int CC1101_MOSI = 3;
constexpr int CC1101_MISO = 2;
}  // namespace Pins

// ---------------------------------------------------------------------------
// RF defaults
// ---------------------------------------------------------------------------
namespace RfDefaults {
constexpr float FrequencyMHz      = 433.92f;
constexpr float ScanStartMHz      = 433.00f;
constexpr float ScanEndMHz        = 434.79f;
constexpr float ScanStepKHz       = 25.0f;
constexpr int   ScanDwellMs       = 5;
constexpr int   RssiThresholdDbm  = -70;
constexpr int   SilenceGapUs      = 15000;   // end-of-burst gap
constexpr int   MaxTxDurationMs   = 10000;
constexpr int   CaptureBufSize    = 4096;
constexpr int   LivePulseWindow   = 512;
constexpr uint8_t ModulationOOK   = 2;
constexpr float RxBandwidthKHz    = 162.0f;
constexpr float DataRateBaud      = 10000.0f;
constexpr float DeviationKHz      = 47.6f;
}  // namespace RfDefaults

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------
namespace NetDefaults {
constexpr const char* Hostname       = "cc1101";
constexpr const char* ApPrefix       = "CC1101-Gateway-";
constexpr uint32_t    WifiTimeoutMs  = 30000;
constexpr uint16_t    HttpPort       = 80;
constexpr uint16_t    WsPort         = 81;
}  // namespace NetDefaults

// ---------------------------------------------------------------------------
// Firmware / OTA (FW_VERSION injected by platformio.ini build_flags)
// ---------------------------------------------------------------------------
#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif

namespace FwInfo {
constexpr const char* Version = FW_VERSION;
constexpr const char* GithubOwner = "ha0rex";
constexpr const char* GithubRepo = "esp32-rf433-gw";
}  // namespace FwInfo

constexpr const char* PrefNamespace = "rf433gw";
