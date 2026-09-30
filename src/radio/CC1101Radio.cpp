#include "CC1101Radio.h"
#include <math.h>

// CC1101 command strobes / registers (TI SWRS061I)
namespace CC {
constexpr uint8_t SRES    = 0x30;
constexpr uint8_t SFSTXON = 0x31;
constexpr uint8_t SXOFF   = 0x32;
constexpr uint8_t SCAL    = 0x33;
constexpr uint8_t SRX     = 0x34;
constexpr uint8_t STX     = 0x35;
constexpr uint8_t SIDLE   = 0x36;
constexpr uint8_t SFRX    = 0x3A;
constexpr uint8_t SFTX    = 0x3B;
constexpr uint8_t SNOP    = 0x3D;

constexpr uint8_t IOCFG2  = 0x00;
constexpr uint8_t IOCFG1  = 0x01;
constexpr uint8_t IOCFG0  = 0x02;
constexpr uint8_t FIFOTHR = 0x03;
constexpr uint8_t SYNC1   = 0x04;
constexpr uint8_t SYNC0   = 0x05;
constexpr uint8_t PKTLEN  = 0x06;
constexpr uint8_t PKTCTRL1= 0x07;
constexpr uint8_t PKTCTRL0= 0x08;
constexpr uint8_t ADDR    = 0x09;
constexpr uint8_t CHANNR  = 0x0A;
constexpr uint8_t FSCTRL1 = 0x0B;
constexpr uint8_t FSCTRL0 = 0x0C;
constexpr uint8_t FREQ2   = 0x0D;
constexpr uint8_t FREQ1   = 0x0E;
constexpr uint8_t FREQ0   = 0x0F;
constexpr uint8_t MDMCFG4 = 0x10;
constexpr uint8_t MDMCFG3 = 0x11;
constexpr uint8_t MDMCFG2 = 0x12;
constexpr uint8_t MDMCFG1 = 0x13;
constexpr uint8_t MDMCFG0 = 0x14;
constexpr uint8_t DEVIATN = 0x15;
constexpr uint8_t MCSM2   = 0x16;
constexpr uint8_t MCSM1   = 0x17;
constexpr uint8_t MCSM0   = 0x18;
constexpr uint8_t FOCCFG  = 0x19;
constexpr uint8_t BSCFG   = 0x1A;
constexpr uint8_t AGCCTRL2= 0x1B;
constexpr uint8_t AGCCTRL1= 0x1C;
constexpr uint8_t AGCCTRL0= 0x1D;
constexpr uint8_t FREND1  = 0x21;
constexpr uint8_t FREND0  = 0x22;
constexpr uint8_t FSCAL3  = 0x23;
constexpr uint8_t FSCAL2  = 0x24;
constexpr uint8_t FSCAL1  = 0x25;
constexpr uint8_t FSCAL0  = 0x26;
constexpr uint8_t TEST2   = 0x2C;
constexpr uint8_t TEST1   = 0x2D;
constexpr uint8_t TEST0   = 0x2E;

constexpr uint8_t PARTNUM = 0x30;
constexpr uint8_t VERSION = 0x31;
constexpr uint8_t FREQEST = 0x32;
constexpr uint8_t LQI     = 0x33;
constexpr uint8_t RSSI    = 0x34;
constexpr uint8_t MARCSTATE = 0x35;

constexpr uint8_t READ_SINGLE  = 0x80;
constexpr uint8_t READ_BURST   = 0xC0;
constexpr uint8_t WRITE_BURST  = 0x40;

constexpr float FXosc = 26.0f;  // MHz crystal on most modules
constexpr uint32_t SpiHz = 1000000;  // 1 MHz — reliable on breadboards
}  // namespace CC

namespace {
// ESP32-C3 HW SPI often breaks digitalRead(MISO) CHIP_RDYn waits.
// Prefer a short settle delay instead.
inline void csSettle() { delayMicroseconds(10); }
}  // namespace

bool CC1101Radio::begin() {
  if (begun_) return identity_.detected;

  pinMode(Pins::CC1101_CSN, OUTPUT);
  digitalWrite(Pins::CC1101_CSN, HIGH);
  pinMode(Pins::CC1101_GDO0, INPUT);

  // Dedicated FSPI instance (ESP32-C3 has one GP-SPI)
  static SPIClass spiBus(FSPI);
  spi_ = &spiBus;
  spi_->end();
  delay(1);
  spi_->begin(Pins::CC1101_SCK, Pins::CC1101_MISO, Pins::CC1101_MOSI, -1);

  // Keep CS under our control (not HW SS)
  pinMode(Pins::CC1101_CSN, OUTPUT);
  digitalWrite(Pins::CC1101_CSN, HIGH);

  // Allow crystal / regulator to settle after power-up
  delay(50);

  Serial.printf("[CC1101] SPI pins SCK=%d MISO=%d MOSI=%d CSN=%d GDO0=%d\n",
                Pins::CC1101_SCK, Pins::CC1101_MISO, Pins::CC1101_MOSI,
                Pins::CC1101_CSN, Pins::CC1101_GDO0);

  // Physical line test: GPIO2 has a strapping pulldown, so a floating MISO
  // reads 0. Enable pull-up briefly and see if the chip drives CHIP_RDYn.
  pinMode(Pins::CC1101_MISO, INPUT_PULLUP);
  digitalWrite(Pins::CC1101_CSN, HIGH);
  delayMicroseconds(50);
  int misoCsHigh = digitalRead(Pins::CC1101_MISO);
  digitalWrite(Pins::CC1101_CSN, LOW);
  delayMicroseconds(200);
  int misoCsLow = digitalRead(Pins::CC1101_MISO);
  digitalWrite(Pins::CC1101_CSN, HIGH);
  Serial.printf("[CC1101] MISO w/pullup: CS=HIGH -> %d, CS=LOW -> %d\n",
                misoCsHigh, misoCsLow);
  if (misoCsHigh == 0 && misoCsLow == 0) {
    Serial.println("[CC1101] MISO stuck LOW — likely wiring/power (not a software issue)");
    Serial.println("[CC1101] Check: 3.3V on VCC, common GND, CSN on GPIO1, MISO on GPIO2");
  } else if (misoCsHigh == 1 && misoCsLow == 1) {
    Serial.println("[CC1101] MISO stays HIGH when CS low — chip not asserting CHIP_RDYn");
    Serial.println("[CC1101] Check: CSN connection, module power, correct pad order");
  } else if (misoCsHigh == 1 && misoCsLow == 0) {
    Serial.println("[CC1101] CHIP_RDYn looks OK (MISO falls when CS low)");
  }

  hardReset();
  delay(10);

  // Probe with hardware SPI first
  useBitbang_ = false;
  identity_.partnum = readPartnum();
  identity_.version = readVersion();
  Serial.printf("[CC1101] HW SPI probe PARTNUM=0x%02X VERSION=0x%02X\n",
                identity_.partnum, identity_.version);

  identity_.detected = isValidId(identity_.partnum, identity_.version);

  if (!identity_.detected) {
    // Fall back to bit-bang SPI — isolates GPIO-matrix / HSPI issues
    Serial.println("[CC1101] HW SPI failed — trying bit-bang SPI");
    spi_->end();
    useBitbang_ = true;
    spiSck_ = Pins::CC1101_SCK;
    spiMosi_ = Pins::CC1101_MOSI;
    pinMode(spiSck_, OUTPUT);
    pinMode(spiMosi_, OUTPUT);
    pinMode(Pins::CC1101_MISO, INPUT_PULLUP);
    pinMode(Pins::CC1101_CSN, OUTPUT);
    digitalWrite(spiSck_, LOW);
    digitalWrite(spiMosi_, LOW);
    digitalWrite(Pins::CC1101_CSN, HIGH);
    delay(5);
    hardReset();
    delay(10);
    identity_.partnum = readPartnum();
    identity_.version = readVersion();
    Serial.printf("[CC1101] BB SPI probe PARTNUM=0x%02X VERSION=0x%02X\n",
                  identity_.partnum, identity_.version);
    identity_.detected = isValidId(identity_.partnum, identity_.version);
  }

  if (!identity_.detected) {
    // CHIP_RDYn worked earlier — likely SCK/MOSI wiring swap on the module header.
    Serial.println("[CC1101] Retrying with SCK/MOSI pins swapped");
    spiSck_ = Pins::CC1101_MOSI;
    spiMosi_ = Pins::CC1101_SCK;
    pinMode(spiSck_, OUTPUT);
    pinMode(spiMosi_, OUTPUT);
    pinMode(Pins::CC1101_MISO, INPUT_PULLUP);
    digitalWrite(spiSck_, LOW);
    digitalWrite(spiMosi_, LOW);
    digitalWrite(Pins::CC1101_CSN, HIGH);
    delay(5);
    hardReset();
    delay(10);
    identity_.partnum = readPartnum();
    identity_.version = readVersion();
    Serial.printf("[CC1101] swapped BB probe PARTNUM=0x%02X VERSION=0x%02X\n",
                  identity_.partnum, identity_.version);
    identity_.detected = isValidId(identity_.partnum, identity_.version);
    if (!identity_.detected) {
      // restore nominal mapping
      spiSck_ = Pins::CC1101_SCK;
      spiMosi_ = Pins::CC1101_MOSI;
    } else {
      Serial.println("[CC1101] NOTE: your module needs MOSI/SCK wires swapped vs the pad table");
    }
  }

  if (!identity_.detected) {
    clockProbe();
    // One more reset + dump raw status/data bytes
    hardReset();
    delay(20);
    uint8_t st = 0, ver = 0, part = 0;
    rawStatusRead(CC::VERSION, st, ver);
    rawStatusRead(CC::PARTNUM, st, part);
    identity_.partnum = part;
    identity_.version = ver;
    Serial.printf("[CC1101] raw VERSION status=0x%02X data=0x%02X\n", st, ver);
    Serial.printf("[CC1101] raw PARTNUM status=0x%02X data=0x%02X\n", st, part);
    identity_.detected = isValidId(part, ver);
  }

  begun_ = true;

  if (identity_.detected) {
    configureDefaults();
    applySettings(settings_);
    idle();
    // Sanity: write/read IOCFG2
    writeReg(CC::IOCFG2, 0x2E);
    uint8_t rd = readReg(CC::IOCFG2);
    Serial.printf("[CC1101] detected PARTNUM=0x%02X VERSION=0x%02X IOCFG2=0x%02X (%s)\n",
                  identity_.partnum, identity_.version, rd,
                  useBitbang_ ? "bitbang" : "hw-spi");
  } else {
    Serial.println("[CC1101] NOT DETECTED — check wiring / 3.3V / SPI pins");
    Serial.println("[CC1101] Tip: verify module pad order (some boards reverse VCC/GND silkscreen).");
  }

  return identity_.detected;
}

bool CC1101Radio::isValidId(uint8_t partnum, uint8_t version) {
  // Known VERSION values: 0x03, 0x04, 0x14 (and similar). PARTNUM usually 0x00.
  if (version == 0x00 || version == 0xFF) return false;
  if (partnum == 0xFF) return false;
  return true;
}

void CC1101Radio::end() {
  if (!begun_) return;
  idle();
  if (!useBitbang_ && spi_) {
    spi_->end();
  }
  begun_ = false;
}

bool CC1101Radio::probe() {
  identity_.partnum = readPartnum();
  identity_.version = readVersion();
  identity_.detected = isValidId(identity_.partnum, identity_.version);
  return identity_.detected;
}

void CC1101Radio::select() {
  digitalWrite(Pins::CC1101_CSN, LOW);
  csSettle();
}

void CC1101Radio::deselect() {
  digitalWrite(Pins::CC1101_CSN, HIGH);
  csSettle();
}

uint8_t CC1101Radio::spiTransfer(uint8_t data) {
  if (useBitbang_) {
    return bitbangTransfer(data);
  }
  return spi_->transfer(data);
}

uint8_t CC1101Radio::bitbangTransfer(uint8_t data) {
  // SPI mode 0: CPOL=0 CPHA=0, MSB first
  pinMode(Pins::CC1101_MISO, INPUT_PULLUP);
  uint8_t out = 0;
  for (int i = 0; i < 8; i++) {
    digitalWrite(spiMosi_, (data & 0x80) ? HIGH : LOW);
    data <<= 1;
    delayMicroseconds(2);
    digitalWrite(spiSck_, HIGH);
    delayMicroseconds(2);
    out <<= 1;
    if (digitalRead(Pins::CC1101_MISO)) out |= 0x01;
    digitalWrite(spiSck_, LOW);
    delayMicroseconds(2);
  }
  return out;
}

void CC1101Radio::clockProbe() {
  // Diagnose whether SCK reaches the chip: shift out 16 bits while MOSI idle low.
  pinMode(Pins::CC1101_MISO, INPUT_PULLUP);
  pinMode(spiSck_, OUTPUT);
  pinMode(spiMosi_, OUTPUT);
  digitalWrite(spiSck_, LOW);
  digitalWrite(spiMosi_, LOW);
  digitalWrite(Pins::CC1101_CSN, LOW);
  delayMicroseconds(200);
  uint16_t bits = 0;
  for (int i = 0; i < 16; i++) {
    digitalWrite(spiSck_, HIGH);
    delayMicroseconds(5);
    bits <<= 1;
    if (digitalRead(Pins::CC1101_MISO)) bits |= 1;
    digitalWrite(spiSck_, LOW);
    delayMicroseconds(5);
  }
  digitalWrite(Pins::CC1101_CSN, HIGH);
  Serial.printf("[CC1101] clock probe (MOSI=0) shifted=0x%04X  SCK=GPIO%d MOSI=GPIO%d\n",
                bits, spiSck_, spiMosi_);
  if (bits == 0x0000) {
    Serial.println("[CC1101] No data shifted on MISO — SCK likely not connected to module");
  }
}

void CC1101Radio::beginXfer() {
  if (!useBitbang_ && spi_) {
    spi_->beginTransaction(SPISettings(CC::SpiHz, MSBFIRST, SPI_MODE0));
  }
  select();
}

void CC1101Radio::endXfer() {
  deselect();
  if (!useBitbang_ && spi_) {
    spi_->endTransaction();
  }
}

void CC1101Radio::hardReset() {
  // Manual CS reset pulse per datasheet, then SRES strobe
  digitalWrite(Pins::CC1101_CSN, HIGH);
  delayMicroseconds(50);
  digitalWrite(Pins::CC1101_CSN, LOW);
  delayMicroseconds(50);
  digitalWrite(Pins::CC1101_CSN, HIGH);
  delayMicroseconds(50);

  beginXfer();
  spiTransfer(CC::SRES);
  // Datasheet: CHIP_RDYn goes low when ready. Avoid digitalRead(MISO) on HW SPI.
  delayMicroseconds(useBitbang_ ? 200 : 1000);
  if (useBitbang_) {
    // With GPIO MISO we can honour CHIP_RDYn
    uint32_t start = micros();
    while (digitalRead(Pins::CC1101_MISO) && (micros() - start) < 100000) {
    }
  }
  endXfer();
  delay(5);
}

void CC1101Radio::softReset() {
  strobe(CC::SRES);
  delay(5);
}

void CC1101Radio::strobe(uint8_t cmd) {
  beginXfer();
  spiTransfer(cmd);
  endXfer();
}

void CC1101Radio::rawStatusRead(uint8_t addr, uint8_t& status, uint8_t& value) {
  beginXfer();
  status = spiTransfer(addr | CC::READ_BURST);
  value = spiTransfer(0x00);
  endXfer();
}

uint8_t CC1101Radio::readReg(uint8_t addr) {
  beginXfer();
  // Status regs 0x30-0x3D need burst bit for single-byte read
  uint8_t header = (addr >= 0x30) ? (addr | CC::READ_BURST) : (addr | CC::READ_SINGLE);
  spiTransfer(header);
  uint8_t val = spiTransfer(0x00);
  endXfer();
  return val;
}

void CC1101Radio::writeReg(uint8_t addr, uint8_t value) {
  beginXfer();
  spiTransfer(addr);
  spiTransfer(value);
  endXfer();
}

void CC1101Radio::writeBurst(uint8_t addr, const uint8_t* data, size_t len) {
  beginXfer();
  spiTransfer(addr | CC::WRITE_BURST);
  for (size_t i = 0; i < len; i++) {
    spiTransfer(data[i]);
  }
  endXfer();
}

void CC1101Radio::readBurst(uint8_t addr, uint8_t* data, size_t len) {
  beginXfer();
  spiTransfer(addr | CC::READ_BURST);
  for (size_t i = 0; i < len; i++) {
    data[i] = spiTransfer(0x00);
  }
  endXfer();
}

uint8_t CC1101Radio::readPartnum() { return readReg(CC::PARTNUM); }
uint8_t CC1101Radio::readVersion() { return readReg(CC::VERSION); }
uint8_t CC1101Radio::readMarcState() { return readReg(CC::MARCSTATE) & 0x1F; }

int8_t CC1101Radio::readRssiDbm() {
  uint8_t raw = readReg(CC::RSSI);
  int16_t rssi;
  if (raw >= 128) {
    rssi = ((int16_t)raw - 256) / 2 - 74;
  } else {
    rssi = (raw / 2) - 74;
  }
  return (int8_t)constrain(rssi, -128, 127);
}

uint32_t CC1101Radio::calcFreqWord(float mhz) const {
  double word = ((double)mhz * 65536.0) / (double)CC::FXosc;
  if (word < 0) word = 0;
  if (word > 0xFFFFFF) word = 0xFFFFFF;
  return (uint32_t)(word + 0.5);
}

bool CC1101Radio::validateFrequency(float mhz) const {
  return (mhz >= 300.0f && mhz <= 348.0f) ||
         (mhz >= 387.0f && mhz <= 464.0f) ||
         (mhz >= 779.0f && mhz <= 928.0f);
}

bool CC1101Radio::setFrequency(float mhz) {
  if (!validateFrequency(mhz)) return false;
  uint32_t word = calcFreqWord(mhz);
  idle();
  writeReg(CC::FREQ2, (word >> 16) & 0xFF);
  writeReg(CC::FREQ1, (word >> 8) & 0xFF);
  writeReg(CC::FREQ0, word & 0xFF);
  settings_.frequencyMHz = mhz;
  return true;
}

uint8_t CC1101Radio::calcMdmcfg4Bw(float khz) const {
  struct BwEntry { float khz; uint8_t bits; };
  static const BwEntry table[] = {
    {812.0f, 0x00}, {650.0f, 0x10}, {541.0f, 0x20}, {464.0f, 0x30},
    {406.0f, 0x40}, {325.0f, 0x50}, {270.0f, 0x60}, {232.0f, 0x70},
    {203.0f, 0x80}, {162.0f, 0x90}, {135.0f, 0xA0}, {116.0f, 0xB0},
    {102.0f, 0xC0}, {81.0f,  0xD0}, {68.0f,  0xE0}, {58.0f,  0xF0},
  };
  uint8_t best = 0x90;
  float bestDiff = 1e9f;
  for (const auto& e : table) {
    float d = fabsf(e.khz - khz);
    if (d < bestDiff) {
      bestDiff = d;
      best = e.bits;
    }
  }
  return best;
}

void CC1101Radio::calcDataRateRegs(float baud, uint8_t& mdmcfg4_drate, uint8_t& mdmcfg3) const {
  double target = (double)baud;
  if (target < 20.0) target = 20.0;
  if (target > 500000.0) target = 500000.0;

  uint8_t e = 0;
  double m = 0;
  for (e = 0; e < 16; e++) {
    m = (target * pow(2.0, 28 - e) / ((double)CC::FXosc * 1e6)) - 256.0;
    if (m >= 0.0 && m <= 255.0) break;
  }
  if (e > 15) e = 15;
  if (m < 0) m = 0;
  if (m > 255) m = 255;
  mdmcfg4_drate = e & 0x0F;
  mdmcfg3 = (uint8_t)(m + 0.5);
}

uint8_t CC1101Radio::calcDeviatn(float khz) const {
  double target = (double)khz * 1000.0;
  uint8_t best = 0x47;
  double bestDiff = 1e18;
  for (uint8_t e = 0; e < 8; e++) {
    for (uint8_t m = 0; m < 8; m++) {
      double dev = ((double)CC::FXosc * 1e6) * (8.0 + m) * pow(2.0, e) / pow(2.0, 17);
      double d = fabs(dev - target);
      if (d < bestDiff) {
        bestDiff = d;
        best = (uint8_t)((e << 4) | m);
      }
    }
  }
  return best;
}

void CC1101Radio::configureDefaults() {
  writeReg(CC::IOCFG2, 0x2E);
  writeReg(CC::IOCFG1, 0x2E);
  writeReg(CC::IOCFG0, 0x0D);
  writeReg(CC::FIFOTHR, 0x47);
  writeReg(CC::PKTCTRL1, 0x00);
  writeReg(CC::PKTCTRL0, 0x32);
  writeReg(CC::ADDR, 0x00);
  writeReg(CC::CHANNR, 0x00);
  writeReg(CC::FSCTRL1, 0x06);
  writeReg(CC::FSCTRL0, 0x00);
  writeReg(CC::MDMCFG2, 0x30);
  writeReg(CC::MDMCFG1, 0x02);
  writeReg(CC::MDMCFG0, 0xF8);
  writeReg(CC::MCSM2, 0x07);
  writeReg(CC::MCSM1, 0x30);
  writeReg(CC::MCSM0, 0x18);
  writeReg(CC::FOCCFG, 0x16);
  writeReg(CC::BSCFG, 0x6C);
  writeReg(CC::AGCCTRL2, 0x04);
  writeReg(CC::AGCCTRL1, 0x00);
  writeReg(CC::AGCCTRL0, 0x91);
  writeReg(CC::FREND1, 0x56);
  writeReg(CC::FREND0, 0x11);
  writeReg(CC::FSCAL3, 0xE9);
  writeReg(CC::FSCAL2, 0x2A);
  writeReg(CC::FSCAL1, 0x00);
  writeReg(CC::FSCAL0, 0x1F);
  writeReg(CC::TEST2, 0x81);
  writeReg(CC::TEST1, 0x35);
  writeReg(CC::TEST0, 0x09);

  uint8_t paTable[8] = {0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  writeBurst(0x3E, paTable, 8);
}

void CC1101Radio::configureAsyncSerial() {
  writeReg(CC::PKTCTRL0, 0x32);
  writeReg(CC::IOCFG0, 0x0D);
}

bool CC1101Radio::setModulation(uint8_t mod) {
  if (mod > 4) return false;
  settings_.modulation = mod;
  uint8_t mdmcfg2 = readReg(CC::MDMCFG2);
  mdmcfg2 = (mdmcfg2 & 0x8F) | ((mod & 0x07) << 4);
  if (mod == 2) {
    mdmcfg2 = 0x30;
  }
  writeReg(CC::MDMCFG2, mdmcfg2);
  if (mod == 2) {
    writeReg(CC::FREND0, 0x11);
  } else {
    writeReg(CC::FREND0, 0x10);
  }
  return true;
}

bool CC1101Radio::setRxBandwidth(float khz) {
  if (khz < 50.0f || khz > 850.0f) return false;
  settings_.rxBandwidthKHz = khz;
  uint8_t mdmcfg4 = readReg(CC::MDMCFG4);
  uint8_t bwBits = calcMdmcfg4Bw(khz);
  mdmcfg4 = (mdmcfg4 & 0x0F) | bwBits;
  writeReg(CC::MDMCFG4, mdmcfg4);
  return true;
}

bool CC1101Radio::setDataRate(float baud) {
  if (baud < 20.0f || baud > 500000.0f) return false;
  settings_.dataRateBaud = baud;
  uint8_t mdmcfg4 = readReg(CC::MDMCFG4);
  uint8_t e, m;
  calcDataRateRegs(baud, e, m);
  mdmcfg4 = (mdmcfg4 & 0xF0) | (e & 0x0F);
  writeReg(CC::MDMCFG4, mdmcfg4);
  writeReg(CC::MDMCFG3, m);
  return true;
}

bool CC1101Radio::setDeviation(float khz) {
  if (khz < 1.5f || khz > 380.0f) return false;
  settings_.deviationKHz = khz;
  writeReg(CC::DEVIATN, calcDeviatn(khz));
  return true;
}

bool CC1101Radio::applySettings(const RFSettings& s) {
  RFSettings tmp = s;
  if (!validateFrequency(tmp.frequencyMHz)) {
    tmp.frequencyMHz = RfDefaults::FrequencyMHz;
  }
  settings_ = tmp;
  idle();
  configureAsyncSerial();
  setFrequency(settings_.frequencyMHz);
  setModulation(settings_.modulation);
  setRxBandwidth(settings_.rxBandwidthKHz);
  setDataRate(settings_.dataRateBaud);
  setDeviation(settings_.deviationKHz);
  return true;
}

void CC1101Radio::resetSettings() {
  settings_ = RFSettings{};
  configureDefaults();
  applySettings(settings_);
}

bool CC1101Radio::idle() {
  strobe(CC::SIDLE);
  for (int i = 0; i < 100; i++) {
    if (readMarcState() == 0x01) return true;
    delayMicroseconds(100);
  }
  return readMarcState() == 0x01;
}

bool CC1101Radio::flushFifo() {
  strobe(CC::SFRX);
  strobe(CC::SFTX);
  return true;
}

bool CC1101Radio::startAsyncRx() {
  if (!identity_.detected) return false;
  configureAsyncSerial();
  idle();
  flushFifo();
  pinMode(Pins::CC1101_GDO0, INPUT);
  strobe(CC::SRX);
  delayMicroseconds(200);
  return true;
}

bool CC1101Radio::isInTx() const {
  return const_cast<CC1101Radio*>(this)->readMarcState() == 0x13;
}

bool CC1101Radio::startAsyncTx() {
  if (!identity_.detected) return false;
  configureAsyncSerial();
  // OOK PA: index0=off, index1=on (FREND0=0x11). 0xC0 ≈ max on most modules.
  uint8_t paTable[8] = {0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  writeBurst(0x3E, paTable, 8);
  writeReg(CC::FREND0, 0x11);
  idle();
  flushFifo();
  pinMode(Pins::CC1101_GDO0, OUTPUT);
  digitalWrite(Pins::CC1101_GDO0, LOW);
  strobe(CC::STX);
  for (int i = 0; i < 50; i++) {
    if (readMarcState() == 0x13) return true;  // TX
    delayMicroseconds(200);
  }
  Serial.printf("[CC1101] STX failed marc=0x%02X\n", readMarcState());
  return false;
}

bool CC1101Radio::ensureAsyncTx() {
  if (!identity_.detected) return false;
  if (readMarcState() == 0x13) return true;
  pinMode(Pins::CC1101_GDO0, OUTPUT);
  digitalWrite(Pins::CC1101_GDO0, LOW);
  strobe(CC::STX);
  for (int i = 0; i < 40; i++) {
    if (readMarcState() == 0x13) return true;
    delayMicroseconds(100);
  }
  return startAsyncTx();
}

const char* CC1101Radio::modulationName(uint8_t mod) {
  switch (mod) {
    case 0: return "2-FSK";
    case 1: return "GFSK";
    case 2: return "ASK/OOK";
    case 3: return "4-FSK";
    case 4: return "MSK";
    default: return "UNKNOWN";
  }
}
