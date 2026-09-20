#pragma once
#include <Arduino.h>

struct SecretsConfig {
  String ssid, password, baseUrl, endpoint, authMode, authHeader, authPrefix, token;
  bool allowInsecureTls = false;
  bool valid() const { return ssid.length() > 0; }
};
struct RadarConfig {
  double latitude = 0.0, longitude = 0.0;
  int rangeKm = 250, refreshSeconds = 5, volume = 75;
  bool experimental320 = false;
  bool vhsOsd = true;
  String audioOutput = "rca";
};
class SecretsManager {
public:
  bool begin();
  bool hasSecrets() const;
  bool load(SecretsConfig &secrets, RadarConfig &settings);
  bool save(const SecretsConfig &secrets, const RadarConfig &settings);
  bool saveSettings(const RadarConfig &settings);
  bool eraseNetworkConfiguration();
  void createExampleIfMissing();

private:
  bool atomicWrite(const char *path, const String &data);
};
