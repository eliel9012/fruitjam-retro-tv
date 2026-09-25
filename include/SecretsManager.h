#pragma once
// settings.json e secrets.json em /M5RETRO/config, o mesmo cartão do Core2.
//
// Gravação só por storage::replace (SafeStorage.h), que abre com "w". Nunca
// FILE_WRITE aqui: no arduino-pico ele é O_APPEND e anexaria o JSON novo ao
// velho (PORTING.md 3.7).
//
// Campos que ficam no JSON só por compatibilidade com o cartão do Core2:
// - allow_insecure_tls: o TLS roda no ESP32-C6 com o bundle de raízes do
//   firmware NINA, sem como desligar a validação (fj/Net.h). É lido, gravado de
//   volta e ignorado. O ca.pem do cartão também deixou de ser usado.
// - cores_16bits: o painel do DVI é sempre RGB565 (PORTING.md 3.5).
#include <Arduino.h>

struct SecretsConfig {
  String ssid, password, baseUrl, endpoint, authMode, authHeader, authPrefix, token;
  bool allowInsecureTls = false; // ignorado no Fruit Jam: ver acima
  bool valid() const { return ssid.length() > 0; }
};
struct RadarConfig {
  double latitude = 0.0, longitude = 0.0;
  int rangeKm = 250, refreshSeconds = 5, volume = 75;
  bool vhsOsd = true;
  bool color16 = true; // ignorado no Fruit Jam: ver acima
  int vhsWear = 2; // 0 desligado, 1 nova, 2 gasta, 3 ruim
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
