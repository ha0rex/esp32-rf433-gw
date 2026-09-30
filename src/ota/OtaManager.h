#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>

enum class OtaChannel : uint8_t { Stable = 0, Nightly = 1 };

enum class OtaPhase : uint8_t {
  Idle = 0,
  Checking,
  Ready,
  Downloading,
  Writing,
  Rebooting,
  Failed,
  UpToDate,
};

struct OtaReleaseInfo {
  bool valid = false;
  String version;
  String name;
  String publishedAt;
  String firmwareUrl;
  String notes;
  size_t sizeBytes = 0;
  bool prerelease = false;
};

class OtaManager {
 public:
  void begin();
  void loop();

  // Call after Wi-Fi is up, before HomeKit/press — applies a queued install.
  // Returns true if an update was attempted (device will reboot on success).
  bool applyPendingIfNeeded();

  OtaChannel channel() const { return channel_; }
  bool setChannel(OtaChannel ch);

  const char* channelName() const;
  static const char* channelName(OtaChannel ch);
  static OtaChannel channelFromName(const char* name);

  const char* currentVersion() const;
  OtaPhase phase() const { return phase_; }
  const char* phaseName() const;
  int progressPct() const { return progressPct_; }
  const String& message() const { return message_; }
  const OtaReleaseInfo& available() const { return available_; }
  uint32_t lastCheckMs() const { return lastCheckMs_; }

  bool checkForUpdate();
  bool startInstall();

  bool busy() const {
    return phase_ == OtaPhase::Checking || phase_ == OtaPhase::Downloading ||
           phase_ == OtaPhase::Writing || phase_ == OtaPhase::Rebooting ||
           installTask_ != nullptr || applyingPending_;
  }

  void toJson(JsonDocument& doc) const;

 private:
  bool fetchRelease(OtaChannel ch, OtaReleaseInfo& out, String& err);
  void setPhase(OtaPhase p, const char* msg);
  static void installTaskThunk(void* arg);
  void runInstall(const String& url);
  void clearPending();
  bool queuePending(const String& url, const String& version);
  static bool versionNewer(const String& remote, const String& local,
                           bool nightlyChannel);

  Preferences prefs_;
  OtaChannel channel_ = OtaChannel::Stable;
  OtaPhase phase_ = OtaPhase::Idle;
  int progressPct_ = 0;
  String message_;
  OtaReleaseInfo available_;
  uint32_t lastCheckMs_ = 0;
  TaskHandle_t installTask_ = nullptr;
  volatile bool installDone_ = false;
  bool installOk_ = false;
  String installError_;
  String pendingUrl_;
  String pendingVersion_;
  bool applyingPending_ = false;
};
