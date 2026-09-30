#pragma once

#include <Arduino.h>
#include <SPI.h>
#include "Config.h"
#include "RadioState.h"

struct CC1101Identity {
  bool detected = false;
  uint8_t partnum = 0;
  uint8_t version = 0;
};

struct RFSettings {
  float frequencyMHz = RfDefaults::FrequencyMHz;
  uint8_t modulation = RfDefaults::ModulationOOK;  // 0=2FSK 1=GFSK 2=ASK/OOK 3=4FSK 4=MSK
  float rxBandwidthKHz = RfDefaults::RxBandwidthKHz;
  float dataRateBaud = RfDefaults::DataRateBaud;
  float deviationKHz = RfDefaults::DeviationKHz;
  int rssiThresholdDbm = RfDefaults::RssiThresholdDbm;
};

class CC1101Radio {
 public:
  bool begin();
  void end();

  bool isDetected() const { return identity_.detected; }
  const CC1101Identity& identity() const { return identity_; }

  bool applySettings(const RFSettings& settings);
  const RFSettings& settings() const { return settings_; }
  void resetSettings();

  bool setFrequency(float mhz);
  float frequency() const { return settings_.frequencyMHz; }

  bool setModulation(uint8_t mod);
  bool setRxBandwidth(float khz);
  bool setDataRate(float baud);
  bool setDeviation(float khz);

  bool idle();
  bool startAsyncRx();
  bool startAsyncTx();
  // Re-assert STX if the chip left TX (SPI traffic can knock it out).
  bool ensureAsyncTx();
  bool flushFifo();
  bool isInTx() const;

  int8_t readRssiDbm();
  uint8_t readMarcState();
  uint8_t readPartnum();
  uint8_t readVersion();

  // Low-level register access (for diagnostics / custom config)
  uint8_t readReg(uint8_t addr);
  void writeReg(uint8_t addr, uint8_t value);
  void strobe(uint8_t cmd);
  void writeBurst(uint8_t addr, const uint8_t* data, size_t len);
  void readBurst(uint8_t addr, uint8_t* data, size_t len);

  bool validateFrequency(float mhz) const;
  static const char* modulationName(uint8_t mod);

 private:
  bool probe();
  void hardReset();
  void softReset();
  void configureDefaults();
  void configureAsyncSerial();
  uint8_t spiTransfer(uint8_t data);
  uint8_t bitbangTransfer(uint8_t data);
  void select();
  void deselect();
  void beginXfer();
  void endXfer();
  void rawStatusRead(uint8_t addr, uint8_t& status, uint8_t& value);
  static bool isValidId(uint8_t partnum, uint8_t version);
  uint8_t calcMdmcfg4Bw(float khz) const;
  uint32_t calcFreqWord(float mhz) const;
  uint8_t calcDeviatn(float khz) const;
  void calcDataRateRegs(float baud, uint8_t& mdmcfg4, uint8_t& mdmcfg3) const;

  SPIClass* spi_ = nullptr;
  CC1101Identity identity_;
  RFSettings settings_;
  bool begun_ = false;
  bool useBitbang_ = false;
  int spiSck_ = Pins::CC1101_SCK;
  int spiMosi_ = Pins::CC1101_MOSI;
  void clockProbe();
};
